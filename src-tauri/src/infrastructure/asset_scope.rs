use std::path::Path;
use tauri::scope::fs::Scope;

/// Restore media access that must survive a restart or a portable data directory.
/// The database and the rest of the data directory are not exposed by this grant.
pub fn allow_persisted_media(
    scope: &Scope,
    data_dir: &Path,
    custom_background: Option<&str>,
) -> tauri::Result<()> {
    for folder in ["attachments", "emoji_favorites"] {
        scope.allow_directory(data_dir.join(folder), true)?;
    }
    if let Some(background) = custom_background.filter(|path| !path.trim().is_empty()) {
        let path = Path::new(background);
        if path.is_absolute() {
            scope.allow_file(path)?;
        }
    }
    Ok(())
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::path::PathBuf;

    fn empty_scope() -> Scope {
        let app = tauri::test::mock_app();
        Scope::new(&app, &Default::default()).unwrap()
    }

    #[test]
    fn portable_media_is_accessible_without_exposing_the_database() {
        let scope = empty_scope();
        // No files are created; test the real scope matcher with future media paths.
        let data_dir = PathBuf::from(env!("CARGO_MANIFEST_DIR"))
            .join("target")
            .join("portable [scope test]")
            .join("data");
        let attachment = data_dir.join("attachments").join("rich").join("image.png");
        let favorite = data_dir.join("emoji_favorites").join("favorite.gif");
        assert!(!scope.is_allowed(&attachment));
        allow_persisted_media(&scope, &data_dir, None).unwrap();
        assert!(scope.is_allowed(attachment));
        assert!(scope.is_allowed(favorite));
        assert!(!scope.is_allowed(data_dir.join("clipboard.db")));
        assert!(!scope.is_allowed(data_dir.join("tiez.log")));
    }

    #[test]
    fn persisted_background_only_restores_access_to_the_selected_file() {
        let scope = empty_scope();
        let root = PathBuf::from(env!("CARGO_MANIFEST_DIR"))
            .join("target")
            .join("background scope test");
        let background = root.join("pictures").join("selected.png");
        assert!(!scope.is_allowed(&background));
        allow_persisted_media(&scope, &root.join("data"), background.to_str()).unwrap();
        assert!(scope.is_allowed(background));
        assert!(!scope.is_allowed(root.join("pictures").join("other.png")));
    }
}
