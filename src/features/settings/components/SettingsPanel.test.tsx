import { describe, expect, it } from "vitest";
import { renderToStaticMarkup } from "react-dom/server";
import { useAppState } from "../../app/hooks/useAppState";
import type { SettingsSubpage } from "../../app/types";
import { useSettingsPanelProps } from "../hooks/useSettingsPanelProps";
import SettingsPanel from "./SettingsPanel";

const noop = () => {};

const SettingsHarness = ({ subpage }: { subpage: SettingsSubpage }) => {
  const state = useAppState();
  const props = useSettingsPanelProps({
    t: (key) => key,
    theme: state.theme,
    language: "zh",
    colorMode: "light",
    hotkeyParts: ["Win", "V"],
    checkHotkeyConflict: () => false,
    updateHotkey: noop,
    updateSequentialHotkey: noop,
    updateRichPasteHotkey: noop,
    updateSearchHotkey: noop,
    saveAppSetting: noop,
    handleResetSettings: noop,
    toggleGroup: noop,
    state: { ...state, settingsSubpage: subpage, collapsedGroups: {} }
  });
  return <SettingsPanel {...props} />;
};

describe("local settings", () => {
  it("renders local settings, all built-in themes and update controls", () => {
    const html = renderToStaticMarkup(<SettingsHarness subpage="home" />);
    expect(html).toContain("clipboard_settings");
    expect(html).toContain("appearance_settings");
    expect(html).toContain("default_apps");
    expect(html).toContain("check_update");
    expect(html.match(/class="theme-choice-title"/g)).toHaveLength(7);
    for (const retired of ["ai_settings", "cloud_sync", "file_transfer", "mqtt", "theme_store"]) {
      expect(html).not.toContain(retired);
    }
  });

  it("keeps the advanced local settings page reachable", () => {
    const html = renderToStaticMarkup(<SettingsHarness subpage="advanced" />);
    expect(html).toContain("advanced_target_global");
    expect(html).toContain("advanced_add_rule");
  });

  it("shows readable Windows shortcut names and separators", () => {
    const html = renderToStaticMarkup(<SettingsHarness subpage="home" />);
    expect(html).toContain("Alt + Shift + V");
    expect(html).toContain("Alt + F");
    expect(html).not.toContain("⌥");
  });
});
