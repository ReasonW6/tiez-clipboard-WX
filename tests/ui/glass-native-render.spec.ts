import { test, expect } from "@playwright/test";
import { mkdirSync, writeFileSync } from "node:fs";
import { execFileSync } from "node:child_process";
import { resolve } from "node:path";

const renderer = process.env.TIEZ_GLASS_OFFSCREEN_RENDERER;
test.skip(!renderer, "Requires the compiled production GPU frame fixture");

for (const mode of ["light", "dark"]) for (const settings of [false, true]) {
  test(`production GPU glass beneath real UI: ${mode}, settings=${settings}`, async ({ page }) => {
    await page.setViewportSize({ width: 640, height: 896 });
    await page.emulateMedia({ reducedMotion: "reduce" });
    await page.goto(`/tests/ui/harness.html?theme=liquid-glass&nativeGlass=true&mode=${mode}`);
    await expect(page.locator(".history-item").first()).toBeVisible();
    if (settings) {
      await page.getByTitle("设置", { exact: true }).click();
      await page.getByRole("heading", { name: "界面设置", exact: true }).click();
      await expect(page.getByRole("slider", { name: "液态玻璃透明度" })).toBeVisible();
    }
    await page.evaluate(() => (window as any).testApp.setGlassStatus("ready"));
    await expect.poll(() => page.locator('.glass-lens[data-native-glass="true"]').count()).toBeGreaterThan(0);
    await page.evaluate(() => new Promise<void>(done => requestAnimationFrame(() => requestAnimationFrame(() => done()))));
    const geometry = await page.evaluate(() => (window as any).testApp.calls.filter((c: any) => c.command === "set_glass_geometry").at(-1).args.geometry);
    const lines = [`640 896 ${geometry.controls.length}`];
    for (const c of geometry.controls) {
      const material = c.material === "reading" ? 2 : c.material === "regular" ? 1 : 0;
      const values = [c.x, c.y, c.width, c.height, c.radius, c.pressure, material, c.track ? 1 : 0];
      if (c.track) values.push(c.track.x, c.track.y, c.track.width, c.track.height, c.track.radius, c.track.progress, ...c.track.activeRgba, ...c.track.inactiveRgba);
      lines.push(values.join(" "));
    }
    const directory = resolve("artifacts/liquid-glass-performance-20261005");
    mkdirSync(directory, { recursive: true });
    const name = `native-${mode}-${settings ? "settings" : "clipboard"}`;
    const input = resolve(directory, `${name}.geometry`), bitmap = resolve(directory, `${name}.bmp`);
    writeFileSync(input, lines.join("\n"), "utf8");
    execFileSync(renderer!, [resolve("src-tauri/native/glass_renderer.hlsl"), input, bitmap, mode], { timeout: 30000 });
    await page.evaluate(async url => {
      const image = new Image(); image.src = url; await image.decode();
      document.body.style.backgroundImage = `url("${url}")`;
      document.body.style.backgroundSize = "100% 100%";
      document.documentElement.style.setProperty("--shell-radius", "16px", "important");
    }, `/artifacts/liquid-glass-performance-20261005/${name}.bmp`);
    await page.screenshot({ path: resolve(directory, `${name}.png`), omitBackground: true, animations: "disabled" });
    await expect(page.locator("#root")).toHaveCSS("background-color", "rgba(0, 0, 0, 0)");
    const unblurredInk = settings ? page.getByRole("heading", { name: "界面设置", exact: true }) : page.locator(".content-preview").first();
    await expect(unblurredInk).toHaveCSS("filter", "none");
  });
}
