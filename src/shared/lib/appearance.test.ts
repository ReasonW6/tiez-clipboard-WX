import { expect, it } from "vitest";
import materialCss from "../../styles/themes/materials.css?raw";
import micaCss from "../../styles/themes/mica.css?raw";
import acrylicCss from "../../styles/themes/acrylic.css?raw";
import liquidGlassCss from "../../styles/themes/liquid-glass.css?raw";
import nativeMaterial from "../../../src-tauri/src/infrastructure/window_material.rs?raw";
import { getMaterialSurfaces } from "./appearance";

it.each(["mica", "acrylic", "liquid-glass"])("%s has a continuous readable range without opaque child surfaces", theme => {
  const stops = [0, 1, 25, 50, 75, 99, 100].map(value => getMaterialSurfaces(value, theme));
  expect(stops[0].window).toBeGreaterThan(0);
  expect(stops[stops.length - 1].window).toBeLessThan(1);
  expect(stops[1].window - stops[0].window).toBeLessThan(.01);
  expect(stops[stops.length - 1].window - stops[stops.length - 2].window).toBeLessThan(.01);
  for (let i = 1; i < stops.length; i++) {
    expect(stops[i].window).toBeGreaterThan(stops[i - 1].window);
    expect(stops[i].content).toBeLessThanOrEqual(.1);
    expect(stops[i].control).toBeLessThanOrEqual(.16);
    expect(stops[i].toolbar).toBe(0);
  }
});

it("preserves clear, frosted and diffuse texture across the slider", () => {
  for (const value of [0, 25, 50, 75, 100]) {
    const glass = getMaterialSurfaces(value, "liquid-glass");
    const acrylic = getMaterialSurfaces(value, "acrylic");
    const mica = getMaterialSurfaces(value, "mica");
    expect(glass.backgroundBlur).toBeLessThan(acrylic.backgroundBlur);
    expect(acrylic.backgroundBlur).toBeLessThan(mica.backgroundBlur);
  }
});

it("keeps a safe default and bounds invalid stored settings", () => {
  expect(getMaterialSurfaces(-1)).toEqual(getMaterialSurfaces(0));
  expect(getMaterialSurfaces(101)).toEqual(getMaterialSurfaces(100));
  expect(getMaterialSurfaces(Number.NaN)).toEqual(getMaterialSurfaces(50));
});

it("clear glass keeps optical depth at maximum transparency and is still glass at zero", () => {
  const clear = getMaterialSurfaces(0, "liquid-glass");
  const soft = getMaterialSurfaces(100, "liquid-glass");
  expect(clear.backgroundBlur).toBeGreaterThan(0);
  expect(clear.window).toBeGreaterThan(0);
  expect(soft.window).toBeLessThan(1);
  expect(soft.backgroundBlur - clear.backgroundBlur).toBeGreaterThan(10);
});

it("keeps neutral mica matte while acrylic diffuses fine background detail even at maximum clarity", () => {
  for (const value of [0, 25, 50, 75, 100]) {
    const mica = getMaterialSurfaces(value, "mica");
    const acrylic = getMaterialSurfaces(value, "acrylic");
    expect(mica.window - acrylic.window).toBeGreaterThanOrEqual(.075);
    expect(acrylic.backgroundBlur).toBeGreaterThanOrEqual(18);
    expect(mica.backgroundBlur - acrylic.backgroundBlur).toBeGreaterThanOrEqual(14);
  }
});

it("keeps CSS background-image blur aligned with the native desktop material", () => {
  for (const [theme, material] of [["mica", "Mica"], ["acrylic", "Frosted"], ["liquid-glass", "Clear"]]) {
    const profile = nativeMaterial.match(new RegExp(`Material::${material} => \\(([\\d.]+), ([\\d.]+)\\)`));
    expect(profile, theme).not.toBeNull();
    const [min, max] = profile!.slice(1).map(Number);
    for (const density of [0, 25, 50, 75, 100])
      expect(getMaterialSurfaces(density, theme).backgroundBlur).toBeCloseTo(min + (max - min) * density / 100, 5);
  }
});

// This proves the fixed-palette neutral-backdrop bound, not photographic or
// refractive rendering. Read the actual CSS colors so palette changes cannot
// silently invalidate the opacity floor.
it.each(["mica", "acrylic", "liquid-glass"])("%s protects every text tone across neutral backdrops in normal mode", theme => {
  const themeCss: Record<string, string> = { mica: micaCss, acrylic: acrylicCss, "liquid-glass": liquidGlassCss };
  const textColors = [...materialCss.matchAll(/--text-(?:primary|secondary|muted):\s*#([\da-f]{6});/g)]
    .map(match => [0, 2, 4].map(index => parseInt(match[1].slice(index, index + 2), 16)));
  const tints = [...themeCss[theme].matchAll(/--material-rgb:\s*([\d, ]+);/g)]
    .map(match => match[1].split(",").map(Number));
  const luminance = (rgb: number[]) => rgb.map(value => value / 255)
    .map(value => value <= .04045 ? value / 12.92 : ((value + .055) / 1.055) ** 2.4)
    .reduce((sum, value, index) => sum + value * [.2126, .7152, .0722][index], 0);
  expect(textColors).toHaveLength(6);
  expect(tints).toHaveLength(2);
  for (let opacity = 0; opacity <= 100; opacity++) {
    const fill = getMaterialSurfaces(opacity, theme).window;
    for (const mode of [0, 1]) for (const desktop of [0, 139, 255]) {
      const background = luminance(tints[mode].map(value => value * fill + desktop * (1 - fill)));
      for (const text of textColors.slice(mode * 3, mode * 3 + 3)) {
        const foreground = luminance(text);
        const contrast = (Math.max(foreground, background) + .05) / (Math.min(foreground, background) + .05);
        expect(contrast, `${theme}, mode ${mode}, stored opacity ${opacity}, desktop ${desktop}, text ${text}`).toBeGreaterThanOrEqual(4.5);
      }
    }
  }
});
