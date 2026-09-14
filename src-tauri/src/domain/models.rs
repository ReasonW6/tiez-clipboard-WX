use serde::{Deserialize, Serialize};

#[derive(Debug, Serialize, Deserialize, Clone)]
pub struct ClipboardEntry {
    pub id: i64,
    pub content_type: String, // 'text', 'image', 'code', 'file', 'video'
    pub content: String,
    #[serde(default)]
    pub html_content: Option<String>,
    pub source_app: String,
    #[serde(default)]
    pub source_app_path: Option<String>,
    pub timestamp: i64,
    pub preview: String,
    pub is_pinned: bool,
    #[serde(default)]
    pub tags: Vec<String>,
    #[serde(default)]
    pub use_count: i32,
    #[serde(default)]
    pub is_external: bool, // New field to track if content is a file path
    #[serde(default)]
    pub pinned_order: i64, // For manual sorting of pinned items
    #[serde(default = "default_true")]
    pub file_preview_exists: bool, // Transient field: does the file exist on disk?
}

impl ClipboardEntry {
    pub fn preserve_capture_metadata(&mut self, existing: &Self) {
        self.is_pinned = existing.is_pinned;
        self.pinned_order = existing.pinned_order;
        self.use_count = existing.use_count;
        let detected_tags = std::mem::take(&mut self.tags);
        self.tags = existing.tags.clone();
        for tag in detected_tags {
            if !self.tags.contains(&tag) {
                self.tags.push(tag);
            }
        }
    }
}

fn default_true() -> bool {
    true
}
