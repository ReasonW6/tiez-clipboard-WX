import { useEffect } from "react";
import { invoke } from "@tauri-apps/api/core";
import type { MutableRefObject } from "react";
import type { AppCleanupPolicy } from "../../features/settings/types";
import type { QuickPasteModifier} from "../../features/app/types";

const QUICK_PASTE_MODIFIERS = new Set<QuickPasteModifier>([
  "disabled",
  "ctrl",
  "alt",
  "shift",
  "win"
]);

const normalizeQuickPasteModifier = (value?: string): QuickPasteModifier => {
  const normalized = value?.trim().toLowerCase();

  switch (normalized) {
    case "control":
      return "ctrl";
    case "option":
      return "alt";
    case "command":
    case "meta":
    case "super":
      return "win";
    default:
      return normalized && QUICK_PASTE_MODIFIERS.has(normalized as QuickPasteModifier)
        ? (normalized as QuickPasteModifier)
        : "disabled";
  }
};

interface UseSettingsPostInitOptions {
  settings: Record<string, string> | null;
  tagManagerSizeRef: MutableRefObject<{ width: number; height: number } | null>;
  setCustomBackground: (val: string) => void;
  setCustomBackgroundOpacity: (val: number) => void;
  setSurfaceOpacity: (val: number) => void;
  setPersistent: (val: boolean) => void;
  setPersistentLimitEnabled: (val: boolean) => void;
  setPersistentLimit: (val: number) => void;
  setDeduplicate: (val: boolean) => void;
  setCaptureFiles: (val: boolean) => void;
  setCaptureRichText: (val: boolean) => void;
  setRichTextSnapshotPreview: (val: boolean) => void;
  setPrivacyProtection: (val: boolean) => void;
  setPrivacyProtectionKinds: (val: string[]) => void;
  setPrivacyProtectionCustomRules: (val: string) => void;
  setSensitiveMaskPrefixVisible: (val: number) => void;
  setSensitiveMaskSuffixVisible: (val: number) => void;
  setSensitiveMaskEmailDomain: (val: boolean) => void;
  setCleanupRules: (val: string) => void;
  setAppCleanupPolicies: (val: AppCleanupPolicy[]) => void;
  setSilentStart: (val: boolean) => void;
  setFollowMouse: (val: boolean) => void;
  setShowAppBorder: (val: boolean) => void;
  setRegistryWinVEnabled: (val: boolean) => void;
  setPasteMethod: (val: string) => void;
  setShowSourceAppIcon: (val: boolean) => void;

  setDeleteAfterPaste: (val: boolean) => void;
  setMoveToTopAfterPaste: (val: boolean) => void;
  setHideTrayIcon: (val: boolean) => void;
  setHideDockIcon: (val: boolean) => void;
  setEdgeDocking: (val: boolean) => void;
  setShowSearchBox: (val: boolean) => void;
  setScrollTopButtonEnabled: (val: boolean) => void;
  setArrowKeySelection: (val: boolean) => void;

  setSequentialHotkey: (val: string) => void;
  setRichPasteHotkey: (val: string) => void;
  setSearchHotkey: (val: string) => void;
  setQuickPasteModifier: (val: QuickPasteModifier) => void;
  setSequentialModeState: (val: boolean) => void;
  setSoundEnabled: (val: boolean) => void;
  setPasteSoundEnabled: (val: boolean) => void;
  setSoundVolume: (val: number) => void;

  setIsWindowPinned: (val: boolean) => void;

  setSettingsLoaded: (val: boolean) => void;
  setClipboardItemFontSize: (val: number) => void;
  setClipboardTagFontSize: (val: number) => void;
  setEmojiPanelEnabled: (val: boolean) => void;
  setTagManagerEnabled: (val: boolean) => void;
  setEmojiPanelTab: (val: "emoji" | "favorites") => void;
  setEmojiFavorites: (val: string[]) => void;
}

export const useSettingsPostInit = ({
  settings,
  tagManagerSizeRef,
  setCustomBackground,
  setCustomBackgroundOpacity,
  setSurfaceOpacity,
  setPersistent,
  setPersistentLimitEnabled,
  setPersistentLimit,
  setDeduplicate,
  setCaptureFiles,
  setCaptureRichText,
  setRichTextSnapshotPreview,
  setPrivacyProtection,
  setPrivacyProtectionKinds,
  setPrivacyProtectionCustomRules,
  setSensitiveMaskPrefixVisible,
  setSensitiveMaskSuffixVisible,
  setSensitiveMaskEmailDomain,
  setCleanupRules,
  setAppCleanupPolicies,
  setSilentStart,
  setFollowMouse,
  setShowAppBorder,
  setRegistryWinVEnabled,
  setPasteMethod,
  setShowSourceAppIcon,

  setDeleteAfterPaste,
  setMoveToTopAfterPaste,
  setHideTrayIcon,
  setHideDockIcon,
  setEdgeDocking,
  setShowSearchBox,
  setScrollTopButtonEnabled,
  setArrowKeySelection,
  setSequentialHotkey,
  setRichPasteHotkey,
  setSearchHotkey,
  setQuickPasteModifier,
  setSequentialModeState,
  setSoundEnabled,
  setPasteSoundEnabled,
  setSoundVolume,
  setIsWindowPinned,
  setSettingsLoaded,
  setClipboardItemFontSize,
  setClipboardTagFontSize,
  setEmojiPanelEnabled,
  setTagManagerEnabled,
  setEmojiPanelTab,
  setEmojiFavorites
}: UseSettingsPostInitOptions) => {
  useEffect(() => {
    if (!settings) return;

    if (settings["app.tag_manager_size"]) {
      try {
        const parsed = JSON.parse(settings["app.tag_manager_size"]);
        if (parsed && typeof parsed.width === "number" && typeof parsed.height === "number") {
          tagManagerSizeRef.current = { width: parsed.width, height: parsed.height };
        }
      } catch (e) {
        console.warn("Invalid tag manager size:", e);
      }
    }

    // Theme application is centralized in the theme effect below
    if (settings["app.custom_background"]) setCustomBackground(settings["app.custom_background"]);
    if (settings["app.custom_background_opacity"]) {
      setCustomBackgroundOpacity(parseInt(settings["app.custom_background_opacity"]));
    }
    if (settings["app.surface_opacity"]) {
      const next = parseInt(settings["app.surface_opacity"]);
      if (Number.isFinite(next)) {
        setSurfaceOpacity(Math.min(100, Math.max(0, next)));
      }
    }
    if (settings["app.clipboard_item_font_size"]) {
      const next = parseInt(settings["app.clipboard_item_font_size"]);
      if (Number.isFinite(next)) setClipboardItemFontSize(next);
    }
    if (settings["app.clipboard_tag_font_size"]) {
      const next = parseInt(settings["app.clipboard_tag_font_size"]);
      if (Number.isFinite(next)) setClipboardTagFontSize(next);
    }
    if (settings["app.emoji_panel_enabled"] !== undefined) {
      setEmojiPanelEnabled(settings["app.emoji_panel_enabled"] === "true");
    }
    if (settings["app.tag_manager_enabled"] !== undefined) {
      setTagManagerEnabled(settings["app.tag_manager_enabled"] !== "false");
    }
    if (settings["app.emoji_panel_tab"] === "favorites" || settings["app.emoji_panel_tab"] === "emoji") {
      setEmojiPanelTab(settings["app.emoji_panel_tab"] as "emoji" | "favorites");
    }
    if (settings["app.emoji_favorites"]) {
      try {
        const parsed = JSON.parse(settings["app.emoji_favorites"]);
        if (Array.isArray(parsed)) {
          setEmojiFavorites(parsed.filter((p) => typeof p === "string"));
        }
      } catch (e) {
        console.warn("Invalid emoji favorites:", e);
      }
    }

    // Fix: explicitly handle both true and false cases for all boolean settings
    setPersistent(settings["app.persistent"] !== "false");
    setPersistentLimitEnabled(settings["app.persistent_limit_enabled"] !== "false");
    if (settings["app.persistent_limit"]) {
      setPersistentLimit(parseInt(settings["app.persistent_limit"]) || 1000);
    }
    setDeduplicate(settings["app.deduplicate"] !== "false");
    setCaptureFiles(settings["app.capture_files"] !== "false");
    setCaptureRichText(settings["app.capture_rich_text"] === "true");
    setRichTextSnapshotPreview(settings["app.rich_text_snapshot_preview"] === "true");
    setPrivacyProtection(settings["app.privacy_protection"] !== "false");
    if (settings["app.privacy_protection_kinds"]) {
      const list = settings["app.privacy_protection_kinds"]
        .split(",")
        .map((s) => s.trim())
        .filter(Boolean);
      if (list.length > 0) setPrivacyProtectionKinds(list);
    }
    if (settings["app.privacy_protection_custom_rules"] !== undefined) {
      setPrivacyProtectionCustomRules(settings["app.privacy_protection_custom_rules"] || "");
    }
    if (settings["app.sensitive_mask_prefix_visible"]) {
      const next = parseInt(settings["app.sensitive_mask_prefix_visible"]);
      if (Number.isFinite(next)) setSensitiveMaskPrefixVisible(Math.min(20, Math.max(0, next)));
    }
    if (settings["app.sensitive_mask_suffix_visible"]) {
      const next = parseInt(settings["app.sensitive_mask_suffix_visible"]);
      if (Number.isFinite(next)) setSensitiveMaskSuffixVisible(Math.min(20, Math.max(0, next)));
    }
    if (settings["app.sensitive_mask_email_domain"] !== undefined) {
      setSensitiveMaskEmailDomain(settings["app.sensitive_mask_email_domain"] === "true");
    }
    if (settings["app.cleanup_rules"] !== undefined) {
      setCleanupRules(settings["app.cleanup_rules"] || "");
    }
    if (settings["app.app_cleanup_policies"]) {
      try {
        const parsed = JSON.parse(settings["app.app_cleanup_policies"]);
        if (Array.isArray(parsed)) {
          setAppCleanupPolicies(
            parsed.filter(
              (item): item is AppCleanupPolicy =>
                !!item &&
                typeof item === "object" &&
                typeof item.id === "string" &&
                typeof item.enabled === "boolean" &&
                typeof item.appName === "string" &&
                typeof item.appPath === "string" &&
                (item.action === "ignore" || item.action === "clean") &&
                Array.isArray(item.contentTypes) &&
                typeof item.cleanupRules === "string"
            )
          );
        }
      } catch (e) {
        console.warn("Invalid app cleanup policies:", e);
      }
    }
    setSilentStart(settings["app.silent_start"] !== "false");
    setFollowMouse(settings["app.follow_mouse"] === "true");
    setShowAppBorder(settings["app.show_app_border"] === "true");
    setRegistryWinVEnabled(settings["app.registry_win_v_enabled"] === "true");
    setPasteMethod(settings["app.paste_method"] || "simulate");
    setShowSourceAppIcon(settings["app.show_source_app_icon"] !== "false");

    // These have false as default, so check for 'true'
    setDeleteAfterPaste(settings["app.delete_after_paste"] === "true");
    setMoveToTopAfterPaste(settings["app.move_to_top_after_paste"] !== "false");
    setHideTrayIcon(settings["app.hide_tray_icon"] === "true");
    setHideDockIcon(settings["app.hide_dock_icon"] === "true");
    const edgeDockingEnabled = settings["app.edge_docking"] === "true";
    setEdgeDocking(edgeDockingEnabled);

    if (settings["app.show_search_box"] === "false") setShowSearchBox(false);
    setScrollTopButtonEnabled(settings["app.show_scroll_top_button"] !== "false");
    if (settings["app.arrow_key_selection"] === "false") setArrowKeySelection(false);

    if (settings["app.sequential_hotkey"]) setSequentialHotkey(settings["app.sequential_hotkey"]);
    if (settings["app.rich_paste_hotkey"]) setRichPasteHotkey(settings["app.rich_paste_hotkey"]);
    if (settings["app.search_hotkey"] !== undefined) setSearchHotkey(settings["app.search_hotkey"]);
    setQuickPasteModifier(normalizeQuickPasteModifier(settings["app.quick_paste_modifier"]));
    if (settings["app.sequential_mode"] === "true") setSequentialModeState(true);
    if (settings["app.sound_enabled"] === "true") setSoundEnabled(true);
    setPasteSoundEnabled(settings["app.sound_paste_enabled"] !== "false");
    if (settings["app.sound_volume"]) {
      setSoundVolume(parseFloat(settings["app.sound_volume"]) || 1.0);
    }

    if (settings["app.window_pinned"] === "true") {
      setIsWindowPinned(true);
      invoke("set_window_pinned", { pinned: true }).catch(console.error);
    }

    setSettingsLoaded(true);
  }, [
    settings,
    tagManagerSizeRef,
    setCustomBackground,
    setCustomBackgroundOpacity,
    setSurfaceOpacity,
    setPersistent,
    setPersistentLimitEnabled,
    setPersistentLimit,
    setDeduplicate,
    setCaptureFiles,
    setCaptureRichText,
    setRichTextSnapshotPreview,
    setPrivacyProtection,
    setPrivacyProtectionKinds,
    setPrivacyProtectionCustomRules,
    setCleanupRules,
    setAppCleanupPolicies,
    setSilentStart,
    setFollowMouse,
    setShowAppBorder,
    setRegistryWinVEnabled,
    setPasteMethod,
    setShowSourceAppIcon,

    setDeleteAfterPaste,
    setMoveToTopAfterPaste,
    setHideTrayIcon,
    setHideDockIcon,
    setEdgeDocking,
    setShowSearchBox,
    setScrollTopButtonEnabled,
    setArrowKeySelection,
    setSequentialHotkey,
    setRichPasteHotkey,
    setSearchHotkey,
    setQuickPasteModifier,
    setSequentialModeState,
    setSoundEnabled,
    setPasteSoundEnabled,
    setSoundVolume,
    setIsWindowPinned,
    setSettingsLoaded,
    setClipboardItemFontSize,
    setClipboardTagFontSize,
    setEmojiPanelEnabled,
    setTagManagerEnabled,
    setEmojiPanelTab,
    setEmojiFavorites
  ]);
};
