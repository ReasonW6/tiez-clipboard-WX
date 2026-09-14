export const clearRetiredBrowserData = (storage: Storage): void => {
  const obsoleteKeys = new Set([
    "tiez_theme_store_token",
    "tiez_theme_store_username",
    "device_id",
    "dismissed_announcements"
  ]);
  const keys = Array.from({ length: storage.length }, (_, index) => storage.key(index));
  for (const key of keys) {
    if (key && (obsoleteKeys.has(key) || key.startsWith("tiez_store_css_"))) {
      storage.removeItem(key);
    }
  }
};
