import { describe, expect, it } from "vitest";
import { clearRetiredBrowserData } from "./retiredStorage";
import { DEFAULT_THEME, normalizeThemeId } from "../config/themes";

const createStorage = (initial: Record<string, string>): Storage => {
  const values = new Map(Object.entries(initial));
  return {
    get length() { return values.size; },
    key: (index) => [...values.keys()][index] ?? null,
    getItem: (key) => values.get(key) ?? null,
    setItem: (key, value) => { values.set(key, value); },
    removeItem: (key) => { values.delete(key); },
    clear: () => values.clear()
  };
};

describe("retired browser data", () => {
  it("removes retired credentials, announcements and every cached store theme", () => {
    const storage = createStorage({
      tiez_theme_store_token: "test token",
      tiez_theme_store_username: "test user",
      device_id: "test device",
      dismissed_announcements: "[1]",
      "tiez_store_css_store-first": "body {}",
      "tiez_store_css_store-second": "body {}",
      tiez_theme: "paper",
      tiez_color_mode: "dark",
      tiez_compact_mode: "true"
    });
    clearRetiredBrowserData(storage);
    clearRetiredBrowserData(storage);
    expect(storage.length).toBe(3);
    expect(storage.getItem("tiez_theme")).toBe("paper");
    expect(storage.getItem("tiez_color_mode")).toBe("dark");
    expect(storage.getItem("tiez_compact_mode")).toBe("true");
  });

  it("falls back from a retired store theme and preserves built-in themes", () => {
    expect(normalizeThemeId("store-old-theme")).toBe(DEFAULT_THEME);
    expect(normalizeThemeId("paper")).toBe("paper");
    expect(normalizeThemeId("sakura")).toBe("sakura");
  });
});
