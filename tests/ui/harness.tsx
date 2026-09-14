import React from "react";
import { createRoot } from "react-dom/client";
import { mockIPC, mockWindows, mockConvertFileSrc } from "@tauri-apps/api/mocks";
import { emit } from "@tauri-apps/api/event";
import "../../src/index.css";
import "../../src/styles/components/index.css";
import "../../src/styles/themes/load";
import { initializeAppearance } from "../../src/shared/lib/appearance";

// All native operations are intercepted. This fixture never reads the user's database
// or clipboard, opens other apps, or changes the system theme.
const params = new URLSearchParams(location.search);
const settings: Record<string, string> = {
  "app.theme": params.get("theme") || "acrylic",
  "app.color_mode": params.get("mode") || "light",
  "app.surface_opacity": params.get("opacity") || "50", "app.language": "zh",
  "app.tag_manager_enabled": "true", "app.show_app_border": "true", "app.window_pinned": "true",
  "app.show_search_box": "true", "app.capture_files": "true", "app.arrow_key_selection": "true"
};
Object.assign(settings, JSON.parse(localStorage.getItem("fixture-settings") || "{}"));
if (params.has("opacity")) settings["app.surface_opacity"] = params.get("opacity")!;
const samples = [
  ["工作", "设计评审记录\n统一组件间距，检查浅色与深色模式下的文字对比度。"],
  ["工作", "npm run build"],
  ["工作", "待办\n修复主题启动状态\n整理常用文本和标签"],
  ["工作", "https://example.com/design"],
  ["代码", "const greeting = 'Hello, world!';"],
  ["密码", "sample-password-only-for-test"],
];
let entries = samples.map(([tag, content], i) => ({ id: i + 1, content_type: "text", content, preview: content,
  source_app: "Test fixture", timestamp: Date.UTC(2026, 8, 14) - i * 60000, is_pinned: false, tags: [tag], use_count: i + 1 }));
let savedTags = ["工作", "代码", "密码", "个人收藏", "一个很长的标签名称用于验证数量不会被截断", "空标签"];
let theme = "dark";
let pinned = true;
const calls: {command: string; args: any}[] = [];
const delays: Record<string, number> = {};
const failures: Record<string, boolean> = {};
const pause = (ms: number) => new Promise(resolve => setTimeout(resolve, ms));
mockWindows("main");
mockConvertFileSrc("windows");
mockIPC(async (command, args: any = {}) => {
  calls.push({ command, args });
  switch (command) {
    case "get_settings": await pause(Number(params.get("settingsDelay") || "0")); return { ...settings };
    case "save_setting": settings[args.key] = args.value; localStorage.setItem("fixture-settings", JSON.stringify(settings)); return null;
    case "get_platform_info": return { platform: "windows", is_windows_10: params.get("win10") === "true", is_windows_11: params.get("win10") !== "true" };
    case "set_theme": await pause(args.colorMode === "dark" ? 80 : 5); theme = args.colorMode === "system" ? "dark" : args.colorMode; return null;
    case "plugin:window|theme": return theme;
    case "plugin:window|is_visible": return true;
    case "plugin:window|scale_factor": return 1;
    case "plugin:window|inner_size": case "plugin:window|outer_size": return { width: innerWidth, height: innerHeight };
    case "plugin:window|get_all_windows": case "plugin:webview|get_all_webviews": return [];
    case "plugin:window|is_focused": return true;
    case "plugin:app|version": return "0.3.4";
    case "get_clipboard_content": return entries.find(entry => entry.id === args.id)?.content;
    case "get_clipboard_history": return entries.slice(args.offset || 0, (args.offset || 0) + (args.limit || 80));
    case "search_clipboard_history": return entries.filter(entry => (args.tagOnly ? entry.tags.join(" ") : entry.content).includes(args.searchTerm));
    case "get_all_tags": return savedTags;
    case "get_all_tags_info": return Object.fromEntries(savedTags.map(tag => [tag, entries.filter(e => e.tags.includes(tag)).length]));
    case "get_tag_colors": return { "工作": "#507ed9", "代码": "#9681ce", "密码": "#c37797" };
    case "get_tag_items": {
      const result = structuredClone(entries.filter(entry => entry.tags.includes(args.tag)));
      await pause(delays[args.tag] || 0);
      if (failures[args.tag]) throw new Error("fixture read error");
      return result.map(entry => ({ ...entry, content: entry.content.slice(0, 50000) }));
    }
    case "create_new_tag": if (!savedTags.includes(args.tagName)) savedTags.push(args.tagName); return null;
    case "rename_tag_globally": savedTags = savedTags.map(tag => tag === args.oldName ? args.newName : tag);
      entries = entries.map(entry => ({ ...entry, tags: entry.tags.map(tag => tag === args.oldName ? args.newName : tag) })); return null;
    case "delete_tag_from_all": savedTags = savedTags.filter(tag => tag !== args.tagName);
      entries = entries.filter(entry => !entry.tags.includes(args.tagName)); return null;
    case "delete_clipboard_entry": entries = entries.filter(entry => entry.id !== args.id); return null;
    case "add_manual_item": entries.push({ ...entries[0], id: Math.max(...entries.map(e => e.id)) + 1, content: args.content, preview: args.content, tags: args.tags }); return null;
    case "update_item_content": entries = entries.map(entry => entry.id === args.id ? { ...entry, content: args.newContent, preview: args.newContent } : entry); return null;
    case "set_window_pinned": pinned = args.pinned; return null;
    case "copy_to_clipboard": {
      const entry = entries.find(entry => entry.id === args.id);
      if (entry) entry.use_count++;
      if (!pinned) await emit("main-window-hidden");
      return null;
    }
    case "hide_window_cmd": case "toggle_window_cmd": await emit("main-window-hidden"); return null;
    case "get_data_path": return "isolated-test-data";
    case "scan_installed_apps": return [];
    case "get_system_default_app": return "Test editor";
    case "is_autostart_enabled": return true;
    case "get_clipboard_count": return entries.length;
    case "get_paste_queue": return { items: [], current_index: 0 };
    default: return null;
  }
}, { shouldMockEvents: true });
Object.assign(window, { testApp: { settings, calls, delays, failures, emit,
  setContent: (id: number, content: string) => { const entry = entries.find(entry => entry.id === id)!; entry.content = content; },
  getEntries: () => structuredClone(entries), getTheme: () => theme } });
localStorage.setItem("tiez_theme", settings["app.theme"]);
localStorage.setItem("tiez_color_mode", settings["app.color_mode"]);
initializeAppearance();
const { default: App } = await import("../../src/App");
createRoot(document.getElementById("root")!).render(<React.StrictMode><App /></React.StrictMode>);
