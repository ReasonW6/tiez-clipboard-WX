import { applyThemeClasses, DEFAULT_THEME } from "../config/themes";

export const applyColorMode = (mode: "light" | "dark") => {
  for (const target of [document.documentElement, document.body]) {
    target.classList.toggle("light-mode", mode === "light");
    target.classList.toggle("dark-mode", mode === "dark");
  }
  document.documentElement.style.colorScheme = mode;
};

export const MAX_GLASS_BLUR = 64;
export const clampSurfaceOpacity = (value: number) => Number.isFinite(value)
  ? Math.min(100, Math.max(0, value)) : 50;

export const nativeBackdropEnabled = (opacity: number) => clampSurfaceOpacity(opacity) > 0;

// One persisted value controls the entire material, including on reload.
export const getLiquidGlassMaterial = (opacity: number) => {
  const density = clampSurfaceOpacity(opacity) / 100;
  return {
    blur: Number((MAX_GLASS_BLUR * Math.pow(density, 1.85)).toFixed(2)),
    specular: .85 - density * .45,
    refraction: 24 - density * 18
  };
};

export const applySurfaceSettings = (opacity: number) => {
  const safeOpacity = clampSurfaceOpacity(opacity);
  const material = getLiquidGlassMaterial(safeOpacity);
  const root = document.documentElement;
  root.style.setProperty("--surface-opacity-scale", String(safeOpacity / 50));
  root.style.setProperty("--surface-opacity", String(safeOpacity / 100));
  root.style.setProperty("--surface-fill", String(Math.pow(safeOpacity / 100, 1.65)));
  root.style.setProperty("--glass-blur", `${material.blur}px`);
  root.style.setProperty("--glass-specular-strength", String(material.specular));
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
