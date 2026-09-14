import { applyThemeClasses, DEFAULT_THEME } from "../config/themes";

export const applyColorMode = (mode: "light" | "dark") => {
  for (const target of [document.documentElement, document.body]) {
    target.classList.toggle("light-mode", mode === "light");
    target.classList.toggle("dark-mode", mode === "dark");
  }
  document.documentElement.style.colorScheme = mode;
};

export const clampGlassBlur = (value: number) => Number.isFinite(value)
  ? Math.min(32, Math.max(0, value)) : 18;

export const applySurfaceSettings = (opacity: number, blur: number) => {
  const safeOpacity = Number.isFinite(opacity) ? Math.min(100, Math.max(0, opacity)) : 50;
  const root = document.documentElement;
  root.style.setProperty("--surface-opacity-scale", String(safeOpacity / 50));
  root.style.setProperty("--surface-opacity", String(safeOpacity / 100));
  root.style.setProperty("--glass-blur", `${clampGlassBlur(blur)}px`);
};

export const initializeAppearance = () => {
  let theme = DEFAULT_THEME;
  let mode = "system";
  try {
    theme = localStorage.getItem("tiez_theme") || DEFAULT_THEME;
    mode = localStorage.getItem("tiez_color_mode") || "system";
  } catch { /* SQLite settings will still load after startup. */ }
  applyThemeClasses(theme, document.documentElement, document.body);
  applyColorMode(mode === "dark" || (mode !== "light" && window.matchMedia("(prefers-color-scheme: dark)").matches)
    ? "dark" : "light");
};
