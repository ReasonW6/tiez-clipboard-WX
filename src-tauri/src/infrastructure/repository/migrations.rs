use rusqlite::{params, Connection, Result};

pub fn run_migrations(conn: &Connection) -> Result<()> {
    conn.execute(
        "CREATE TABLE IF NOT EXISTS schema_migrations (
            version INTEGER PRIMARY KEY,
            applied_at TIMESTAMP DEFAULT CURRENT_TIMESTAMP
        )",
        [],
    )?;

    let current_version: i32 = conn
        .query_row(
            "SELECT COALESCE(MAX(version), 0) FROM schema_migrations",
            [],
            |row| row.get(0),
        )
        .unwrap_or(0);

    // Migration 1: Initial Baseline
    if current_version < 1 {
        conn.execute_batch(
            "
            CREATE TABLE IF NOT EXISTS clipboard_history (
                id INTEGER PRIMARY KEY,
                content_type TEXT NOT NULL,
                content TEXT NOT NULL,
                source_app TEXT NOT NULL,
                timestamp INTEGER NOT NULL,
                preview TEXT NOT NULL
            );
            CREATE TABLE IF NOT EXISTS settings (
                key TEXT PRIMARY KEY,
                value TEXT NOT NULL
            );
        ",
        )?;
        conn.execute("INSERT INTO schema_migrations (version) VALUES (1)", [])?;
    }

    // Migration 2: Add core feature columns
    if current_version < 2 {
        let columns = [
            ("is_pinned", "INTEGER NOT NULL DEFAULT 0"),
            ("tags", "TEXT NOT NULL DEFAULT '[]'"),
            ("use_count", "INTEGER NOT NULL DEFAULT 0"),
            ("pinned_order", "INTEGER NOT NULL DEFAULT 0"),
            ("content_hash", "INTEGER NOT NULL DEFAULT 0"),
            ("html_content", "TEXT"),
        ];

        for (name, def) in columns {
            if !has_column(conn, "clipboard_history", name)? {
                conn.execute(
                    &format!("ALTER TABLE clipboard_history ADD COLUMN {} {}", name, def),
                    [],
                )?;
            }
        }
        conn.execute("INSERT INTO schema_migrations (version) VALUES (2)", [])?;
    }

    // Migration 3: Add is_external
    if current_version < 3 {
        if !has_column(conn, "clipboard_history", "is_external")? {
            conn.execute(
                "ALTER TABLE clipboard_history ADD COLUMN is_external INTEGER NOT NULL DEFAULT 0",
                [],
            )?;
        }
        conn.execute("INSERT INTO schema_migrations (version) VALUES (3)", [])?;
    }

    // Migration 4: Tag management
    if current_version < 4 {
        conn.execute(
            "CREATE TABLE IF NOT EXISTS saved_tags (
                name TEXT PRIMARY KEY,
                color TEXT
            )",
            [],
        )?;

        // Insert default tags
        let _ = conn.execute(
            "INSERT OR IGNORE INTO saved_tags (name) VALUES ('sensitive')",
            [],
        );
        let _ = conn.execute(
            "INSERT OR IGNORE INTO saved_tags (name) VALUES ('密码')",
            [],
        );

        conn.execute("INSERT INTO schema_migrations (version) VALUES (4)", [])?;
    }

    // Migration 5: Performance indexes
    if current_version < 5 {
        conn.execute_batch(
            "
            CREATE INDEX IF NOT EXISTS idx_clipboard_history_pinned_order_time
                ON clipboard_history (is_pinned, pinned_order, timestamp);
            CREATE INDEX IF NOT EXISTS idx_clipboard_history_type_hash
                ON clipboard_history (content_type, content_hash);
            CREATE INDEX IF NOT EXISTS idx_clipboard_history_timestamp
                ON clipboard_history (timestamp);
        ",
        )?;
        conn.execute("INSERT INTO schema_migrations (version) VALUES (5)", [])?;
    }

    // Migration 6: Normalize tags into entry_tags
    if current_version < 6 {
        conn.execute_batch(
            "
            CREATE TABLE IF NOT EXISTS entry_tags (
                entry_id INTEGER NOT NULL,
                tag TEXT NOT NULL,
                PRIMARY KEY (entry_id, tag)
            );
            CREATE INDEX IF NOT EXISTS idx_entry_tags_tag ON entry_tags (tag);
            CREATE INDEX IF NOT EXISTS idx_entry_tags_entry ON entry_tags (entry_id);
        ",
        )?;

        // Backfill entry_tags from clipboard_history.tags JSON
        conn.execute("BEGIN", [])?;
        let backfill = (|| -> Result<()> {
            let mut stmt = conn.prepare("SELECT id, tags FROM clipboard_history")?;
            let rows = stmt.query_map([], |row| {
                let id: i64 = row.get(0)?;
                let tags: Option<String> = row.get(1)?;
                Ok((id, tags.unwrap_or_else(|| "[]".to_string())))
            })?;

            for row in rows {
                let (id, tags_json) = row?;
                let tags: Vec<String> = serde_json::from_str(&tags_json).unwrap_or_default();
                for tag in tags {
                    if tag.trim().is_empty() {
                        continue;
                    }
                    conn.execute(
                        "INSERT OR IGNORE INTO entry_tags (entry_id, tag) VALUES (?1, ?2)",
                        params![id, tag],
                    )?;
                }
            }
            Ok(())
        })();

        if let Err(err) = backfill {
            let _ = conn.execute("ROLLBACK", []);
            return Err(err);
        }
        conn.execute("COMMIT", [])?;
        conn.execute("INSERT INTO schema_migrations (version) VALUES (6)", [])?;
    }

    // Migration 7: Cloud sync tombstones for deletion propagation
    if current_version < 7 {
        conn.execute_batch(
            "
            CREATE TABLE IF NOT EXISTS cloud_sync_tombstones (
                content_type TEXT NOT NULL,
                content_hash INTEGER NOT NULL,
                deleted_at INTEGER NOT NULL,
                PRIMARY KEY (content_type, content_hash)
            );
            CREATE INDEX IF NOT EXISTS idx_cloud_sync_tombstones_deleted_at
                ON cloud_sync_tombstones (deleted_at);
            ",
        )?;
        conn.execute("INSERT INTO schema_migrations (version) VALUES (7)", [])?;
    }

    // Migration 8: Local incremental sync index (for delta diff against last uploaded state)
    if current_version < 8 {
        conn.execute_batch(
            "
            CREATE TABLE IF NOT EXISTS cloud_sync_local_index (
                sync_key TEXT PRIMARY KEY,
                digest TEXT NOT NULL
            );
            ",
        )?;
        conn.execute("INSERT INTO schema_migrations (version) VALUES (8)", [])?;
    }

    // Migration 9: Persist source app path for source icon rendering
    if current_version < 9 {
        if !has_column(conn, "clipboard_history", "source_app_path")? {
            conn.execute(
                "ALTER TABLE clipboard_history ADD COLUMN source_app_path TEXT",
                [],
            )?;
        }
        conn.execute("INSERT INTO schema_migrations (version) VALUES (9)", [])?;
    }

    // Migration 10: Cloud sync content type preferences (per-device; not synced in settings snapshot)
    if current_version < 10 {
        conn.execute(
            "INSERT OR IGNORE INTO settings (key, value) VALUES ('cloud_sync_content_prefs', '{\"text\":true,\"image\":true,\"file_path\":true,\"emoji\":true}')",
            [],
        )?;
        conn.execute("INSERT INTO schema_migrations (version) VALUES (10)", [])?;
    }


    // Migration 11: Remove retired network/AI settings and synchronization metadata.
    // Clipboard history, tags, and managed/user attachments are preserved.
    if current_version < 11 {
        let tx = conn.unchecked_transaction()?;
        tx.execute_batch(
            "
            DROP TABLE IF EXISTS cloud_sync_tombstones;
            DROP TABLE IF EXISTS cloud_sync_local_index;
            DELETE FROM settings
            WHERE key GLOB 'ai_*'
               OR key GLOB 'mqtt_*'
               OR key GLOB 'cloud_sync_*'
               OR key GLOB 'file_server_*'
               OR key GLOB 'file_transfer_*'
               OR key IN ('app.anon_id', 'app.last_ping_date');
            UPDATE settings SET value = 'mica' WHERE key = 'app.theme' AND value GLOB 'store-*';
            ",
        )?;
        tx.execute("INSERT INTO schema_migrations (version) VALUES (11)", [])?;
        tx.commit()?;
    }

    Ok(())
}

fn has_column(conn: &Connection, table_name: &str, column_name: &str) -> Result<bool> {
    let mut stmt = conn.prepare(&format!("PRAGMA table_info({})", table_name))?;
    let mut rows = stmt.query([])?;
    while let Some(row) = rows.next()? {
        let name: String = row.get(1)?;
        if name == column_name {
            return Ok(true);
        }
    }
    Ok(false)
}

#[cfg(test)]
mod tests {
    use super::*;

    fn assert_retired_data_removed(conn: &Connection) {
        let tables: i64 = conn.query_row(
            "SELECT COUNT(*) FROM sqlite_master WHERE type = 'table' AND name IN ('cloud_sync_tombstones', 'cloud_sync_local_index')",
            [], |row| row.get(0),
        ).unwrap();
        assert_eq!(tables, 0);
        let settings: i64 = conn.query_row(
            "SELECT COUNT(*) FROM settings WHERE key GLOB 'ai_*' OR key GLOB 'mqtt_*' OR key GLOB 'cloud_sync_*' OR key GLOB 'file_server_*' OR key GLOB 'file_transfer_*'",
            [], |row| row.get(0),
        ).unwrap();
        assert_eq!(settings, 0);
    }

    #[test]
    fn fresh_database_contains_only_local_settings() {
        let conn = Connection::open_in_memory().unwrap();
        run_migrations(&conn).unwrap();
        crate::database::seed_defaults(&conn).unwrap();
        assert_retired_data_removed(&conn);
        assert!(has_column(&conn, "clipboard_history", "source_app_path").unwrap());
    }

    #[test]
    fn upgrade_removes_retired_settings_and_preserves_history_and_tags() {
        let conn = Connection::open_in_memory().unwrap();
        run_migrations(&conn).unwrap();
        conn.execute_batch(
            "DELETE FROM schema_migrations WHERE version = 11;
             CREATE TABLE cloud_sync_tombstones (content_type TEXT, content_hash INTEGER, deleted_at INTEGER);
             CREATE TABLE cloud_sync_local_index (sync_key TEXT, digest TEXT);
             INSERT INTO settings (key, value) VALUES
                 ('ai_profiles', 'obsolete test profile'),
                 ('mqtt_password', 'obsolete test credential'),
                 ('cloud_sync_webdav_password', 'obsolete test credential'),
                 ('file_server_enabled', 'true'),
                 ('file_transfer_path', 'D:/Downloads'),
                 ('app.theme', 'paper'),
                 ('app.sound_paste_enabled', 'false');
             INSERT INTO clipboard_history (id, content_type, content, source_app, timestamp, preview, is_pinned, tags)
                 VALUES (42, 'text', 'Keep this local note', 'Editor', 123, 'Keep this local note', 1, '[\"work\"]');
             INSERT INTO saved_tags (name) VALUES ('work');
             INSERT INTO entry_tags (entry_id, tag) VALUES (42, 'work');",
        ).unwrap();

        run_migrations(&conn).unwrap();
        run_migrations(&conn).unwrap();
        crate::database::seed_defaults(&conn).unwrap();
        assert_retired_data_removed(&conn);
        let note: (String, bool) = conn.query_row(
            "SELECT content, is_pinned FROM clipboard_history WHERE id = 42",
            [], |row| Ok((row.get(0)?, row.get(1)?)),
        ).unwrap();
        assert_eq!(note, ("Keep this local note".to_string(), true));
        let tag: String = conn.query_row("SELECT tag FROM entry_tags WHERE entry_id = 42", [], |row| row.get(0)).unwrap();
        assert_eq!(tag, "work");
        let theme: String = conn.query_row("SELECT value FROM settings WHERE key = 'app.theme'", [], |row| row.get(0)).unwrap();
        assert_eq!(theme, "paper");
        let sound: String = conn.query_row("SELECT value FROM settings WHERE key = 'app.sound_paste_enabled'", [], |row| row.get(0)).unwrap();
        assert_eq!(sound, "false");
    }
}
