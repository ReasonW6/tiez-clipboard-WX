import { expect, test } from "@playwright/test";
import { readFile } from "node:fs/promises";

// CSS blur's parameter is a standard deviation. The native material uses a
// variance-matched mip footprint, so these are budgets, not identical kernels.
const shader = await readFile(new URL("../../src-tauri/native/glass_renderer.hlsl", import.meta.url), "utf8");
const regularFloor = Number(shader.match(/regularFootprint\s*=\s*sqrt\(12\.0\)\s*\*\s*max\(([\d.]+)\s*\*\s*dpi,/u)?.[1]);
const readingFloor = Number(shader.match(/readingFootprint\s*=\s*sqrt\(12\.0\)\s*\*\s*max\(([\d.]+)\s*\*\s*dpi,/u)?.[1]);
if (![regularFloor, readingFloor].every(Number.isFinite)) throw new Error("Review the native surface filtering contract before updating this test");

for (const mode of ["light", "dark"]) for (const density of [0, 50, 100]) {
  test(`Regular fallback preserves the filtering budget and shadow: ${mode}, density ${density}`, async ({ page }) => {
    await page.emulateMedia({ reducedMotion: "reduce" });
    await page.goto(`/tests/ui/harness.html?theme=liquid-glass&mode=${mode}&opacity=${density}&nativeGlass=true`);
    await expect(page.locator(".history-item").first()).toBeVisible();
    for (const settings of [false, true]) {
      if (settings) {
        await page.getByTitle("设置", { exact: true }).click();
        await expect(page.locator(".settings-view")).toBeVisible();
      }
      const lens = page.locator(settings ? ".settings-reading-plane > .glass-lens" : ".history-item > .glass-lens").first();
      await expect(lens).toHaveAttribute("data-glass-material", settings ? "reading" : "regular");
      const reflection = lens.locator(".glass-lens-reflection");
      const shadow = lens.locator(".glass-lens-shadow");
      const expectedBlur = await page.evaluate(floor => Math.max(floor,
        parseFloat(getComputedStyle(document.documentElement).getPropertyValue("--material-background-blur"))), settings ? readingFloor : regularFloor);
      const initialShadow = await shadow.evaluate(element => getComputedStyle(element).boxShadow);
      expect(initialShadow).not.toBe("none");
      for (const state of ["ready", "resizing", "ready", "suspended", "ready", "failed"]) {
        await page.evaluate(state => (window as any).testApp.setGlassStatus(state), state);
        if (state === "ready") await expect(reflection).toHaveCSS("display", "none");
        else {
          await expect(reflection).not.toHaveCSS("display", "none");
          await expect(reflection).toHaveCSS("backdrop-filter", `blur(${expectedBlur}px)`);
        }
        await expect(shadow).toHaveCSS("box-shadow", initialShadow);
      }
    }
  });
}
