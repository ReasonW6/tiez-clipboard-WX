import { useEffect, useRef } from "react";
import { invoke } from "@tauri-apps/api/core";
import { getCurrentWindow } from "@tauri-apps/api/window";
import { applyThemeClasses, normalizeThemeId } from "../config/themes";
import { applyColorMode, applySurfaceSettings } from "../lib/appearance";
import { startNativeGlass } from "../lib/nativeGlass";

interface UseSettingsApplyOptions {
  theme: string;
  colorMode: string;
  compactMode: boolean;
  settingsLoaded: boolean;
  clipboardItemFontSize: number;
  clipboardTagFontSize: number;
  surfaceOpacity: number;
  showAppBorder: boolean;
}

// Keep rapid theme changes ordered so an older native request cannot win.
let nativeThemeUpdate: Promise<unknown> = Promise.resolve();

export const useSettingsApply = ({
  theme, colorMode, compactMode, settingsLoaded, clipboardItemFontSize,
  clipboardTagFontSize, surfaceOpacity, showAppBorder
}: UseSettingsApplyOptions) => {
  const latestOpacity = useRef(surfaceOpacity);
  latestOpacity.current = surfaceOpacity;
  useEffect(() => {
    let disposed = false;
    void invoke<{ is_windows_10: boolean; is_windows_11: boolean }>("get_platform_info").then(platform => {
      if (disposed) return;
      document.body.classList.toggle("windows-10", platform.is_windows_10);
      for (const target of [document.documentElement, document.body]) {
        target.classList.toggle("native-window-frame", platform.is_windows_11);
      }
    }).catch(console.error);
    return () => { disposed = true; };
  }, []);

  useEffect(() => {
    if (!settingsLoaded) return;
    let disposed = false;
    const normalizedTheme = normalizeThemeId(theme);
    applyThemeClasses(normalizedTheme, document.documentElement, document.body);
    if (colorMode === "light" || colorMode === "dark") applyColorMode(colorMode);
    const nativeGlass = normalizedTheme === "liquid-glass" ? startNativeGlass() : undefined;

    const updateNative = () => {
      nativeThemeUpdate = nativeThemeUpdate.catch(() => {}).then(async () => {
        if (disposed) return;
        await invoke("set_theme", {
          theme: normalizedTheme, colorMode, showAppBorder, surfaceOpacity: latestOpacity.current
        });
        if (!disposed) await nativeGlass?.afterThemeApplied();
        // Query only after clearing the window's explicit mode when following the OS.
        if (colorMode === "system" && !disposed) {
          const mode = await getCurrentWindow().theme();
          if (!disposed) applyColorMode(mode === "dark" ? "dark" : "light");
        }
      }).catch(error => {
        console.error("Failed to apply window appearance", error);
        if (!disposed && colorMode === "system") {
          applyColorMode(window.matchMedia("(prefers-color-scheme: dark)").matches ? "dark" : "light");
        }
      });
    };
    let unlisten: (() => void) | undefined;
    if (colorMode === "system") {
      void getCurrentWindow().onThemeChanged(event => {
        if (disposed) return;
        applyColorMode(event.payload === "dark" ? "dark" : "light");
        updateNative();
      }).then(off => { if (disposed) off(); else unlisten = off; }).catch(console.error);
    }
    updateNative();
    return () => { disposed = true; unlisten?.(); nativeGlass?.dispose(); };
  }, [theme, colorMode, settingsLoaded, showAppBorder]);

  useEffect(() => {
    if (!settingsLoaded || !["mica", "acrylic", "liquid-glass"].includes(theme)) return;
    let disposed = false;
    nativeThemeUpdate = nativeThemeUpdate.catch(() => {}).then(async () => {
      if (!disposed) await invoke("set_material_opacity", { opacity: surfaceOpacity });
    }).catch(error => console.error("Failed to apply material transparency", error));
    return () => { disposed = true; };
  }, [theme, surfaceOpacity, settingsLoaded]);

  useEffect(() => {
    if (!settingsLoaded) return;
    document.body.classList.toggle("compact-mode", compactMode);
    document.documentElement.style.setProperty("--clipboard-item-font-size", `${clipboardItemFontSize}px`);
    document.documentElement.style.setProperty("--clipboard-tag-font-size", `${clipboardTagFontSize}px`);
    applySurfaceSettings(surfaceOpacity, theme);
  }, [theme, compactMode, clipboardItemFontSize, clipboardTagFontSize, surfaceOpacity, settingsLoaded]);
};
