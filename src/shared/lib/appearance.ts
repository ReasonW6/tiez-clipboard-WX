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

// Regular material: a continuous tint range above a stable native backdrop.
// Content retains its own contrast floor, including over a dark desktop.
export const getMaterialSurfaces = (opacity: number) => {
  const density = clampSurfaceOpacity(opacity) / 100;
  return {
    window: .22 + density * .74,
    content: .90 + density * .08,
    control: .76 + density * .20,
    toolbar: .78 + density * .18,
    backgroundBlur: 8 + density * 16
  };
};

export const applySurfaceSettings = (opacity: number) => {
  const value = clampSurfaceOpacity(opacity);
  const material = getMaterialSurfaces(value);
  const root = document.documentElement;
  root.style.setProperty("--surface-opacity", String(value / 100));
  root.style.setProperty("--surface-fill", String(material.window));
  root.style.setProperty("--material-content-opacity", String(material.content));
  root.style.setProperty("--material-control-opacity", String(material.control));
  root.style.setProperty("--material-toolbar-opacity", String(material.toolbar));
  root.style.setProperty("--material-background-blur", `${material.backgroundBlur}px`);
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
  applySurfaceSettings(opacity);
  applyThemeClasses(theme, document.documentElement, document.body);
  applyColorMode(mode === "dark" || (mode !== "light" && window.matchMedia("(prefers-color-scheme: dark)").matches)
    ? "dark" : "light");
};
