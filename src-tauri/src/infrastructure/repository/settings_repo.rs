use rusqlite::{params, Connection, OptionalExtension, Result};
use std::collections::HashMap;
use std::sync::{Arc, Mutex};

pub trait SettingsRepository {
    fn set(&self, key: &str, value: &str) -> Result<()>;
    fn get(&self, key: &str) -> Result<Option<String>>;
    fn get_all(&self) -> Result<HashMap<String, String>>;
    fn clear(&self) -> Result<()>;
}

pub struct SqliteSettingsRepository {
    conn: Arc<Mutex<Connection>>,
}

impl SqliteSettingsRepository {
    pub fn new(conn: Arc<Mutex<Connection>>) -> Self {
        Self { conn }
    }

    pub fn get_raw(conn: &Connection, key: &str) -> Result<Option<String>> {
        conn.query_row("SELECT value FROM settings WHERE key = ?", [key], |row| row.get(0))
            .optional()
    }
}

impl SettingsRepository for SqliteSettingsRepository {
    fn set(&self, key: &str, value: &str) -> Result<()> {
        let conn = self.conn.lock().unwrap();
        conn.execute(
            "INSERT OR REPLACE INTO settings (key, value) VALUES (?, ?)",
            params![key, value],
        )?;
        Ok(())
    }

    fn get(&self, key: &str) -> Result<Option<String>> {
        Self::get_raw(&self.conn.lock().unwrap(), key)
    }

    fn get_all(&self) -> Result<HashMap<String, String>> {
        let conn = self.conn.lock().unwrap();
        let mut stmt = conn.prepare("SELECT key, value FROM settings")?;
        let rows = stmt.query_map([], |row| Ok((row.get::<_, String>(0)?, row.get::<_, String>(1)?)))?;
        rows.collect()
    }

    fn clear(&self) -> Result<()> {
        self.conn.lock().unwrap().execute("DELETE FROM settings", [])?;
        Ok(())
    }
}
