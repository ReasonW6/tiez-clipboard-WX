import { applyThemeClasses, DEFAULT_THEME } from "../config/themes";

export const applyColorMode = (mode: "light" | "dark") => {
  for (const target of [document.documentElement, document.body]) {
    target.classList.toggle("light-mode", mode === "light");
    target.classList.toggle("dark-mode", mode === "dark");
  }
  document.documentElement.style.colorScheme = mode;
};

export const clampSurfaceOpacity = (value: number) => Number.isFinite(value)
  ? Math.min(100, Math.max(0, value)) : 50;

// Neutral native blur cannot adjust desktop luminance. A single window tint
// therefore retains enough coverage for the fixed light/dark text palettes,
// even at maximum transparency. Child surfaces only add small same-color lifts.
export const getMaterialSurfaces = (opacity: number, theme = DEFAULT_THEME) => {
  const density = clampSurfaceOpacity(opacity) / 100;
  const profiles: Record<string, { minFill: number; maxFill: number; minBlur: number; maxBlur: number }> = {
    acrylic: { minFill: .74, maxFill: .90, minBlur: 18, maxBlur: 38 },
    mica: { minFill: .875, maxFill: .98, minBlur: 40, maxBlur: 64 }
  };
  const profile = profiles[theme];
  if (profile) return {
    window: profile.minFill + density * (profile.maxFill - profile.minFill),
    content: .025 + density * .075,
    control: .045 + density * .115,
    toolbar: 0,
    backgroundBlur: profile.minBlur + density * (profile.maxBlur - profile.minBlur)
  };
  // Preserve the existing range of decorative, non-material themes.
  return {
    window: .22 + density * .74,
    content: .90 + density * .08,
    control: .76 + density * .20,
    toolbar: .78 + density * .18,
    backgroundBlur: 8 + density * 16
  };
};

export const applySurfaceSettings = (opacity: number, theme = DEFAULT_THEME) => {
  const value = clampSurfaceOpacity(opacity);
  const material = getMaterialSurfaces(value, theme);
  const root = document.documentElement;
  const grain = theme === "acrylic" ? .02 + value / 100 * .025 : theme === "mica" ? .004 + value / 100 * .008 : 0;
  root.style.setProperty("--material-grain-opacity", String(grain));
  root.style.setProperty("--surface-opacity", String(value / 100));
  root.style.setProperty("--surface-fill", String(material.window));
  root.style.setProperty("--material-content-opacity", String(material.content));
  root.style.setProperty("--material-control-opacity", String(material.control));
  root.style.setProperty("--material-toolbar-opacity", String(material.toolbar));
  root.style.setProperty("--material-background-blur", `${material.backgroundBlur}px`);
  root.style.setProperty("--material-outline-opacity", String(.14 - value / 100 * .04));
};

export const initializeAppearance = () => {
  let theme = DEFAULT_THEME;
  let mode = "system";
  let opacity = 50;
  try {
    theme = localStorage.getItem("tiez_theme") || DEFAULT_THEME;
    mode = localStorage.getItem("tiez_color_mode") || "system";
    opacity = Number(localStorage.getItem("tiez_surface_opacity") ?? 50);
  } catch { /* SQLite settings will still load after startup. */ }
  applySurfaceSettings(opacity, theme);
  applyThemeClasses(theme, document.documentElement, document.body);
  applyColorMode(mode === "dark" || (mode !== "light" && window.matchMedia("(prefers-color-scheme: dark)").matches)
    ? "dark" : "light");
};
