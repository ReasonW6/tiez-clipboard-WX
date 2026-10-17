import { test, expect } from "@playwright/test";

for (const theme of ["mica", "acrylic"]) {
  for (const mode of ["light", "dark"]) {
    test(`${theme} ${mode}: one continuous surface behind navigation and empty state`, async ({ page }) => {
      await page.goto(`/tests/ui/harness.html?theme=${theme}&mode=${mode}&empty=true&opacity=0`);
      await expect(page.locator(".empty-state-message")).toBeVisible();
      for (const selector of ["header", ".main-content", ".empty-state-message"]) {
        await expect(page.locator(selector)).toHaveCSS("background-color", "rgba(0, 0, 0, 0)");
        await expect(page.locator(selector)).toHaveCSS("background-image", "none");
      }
      await expect(page.locator(".empty-state-message")).toHaveCSS("border-top-width", "0px");
      await page.getByTitle("设置", { exact: true }).click();
      await page.getByRole("heading", { name: "界面设置", exact: true }).click();
      const samples: number[] = [];
      const slider = page.getByRole("slider", { name: "界面透明度" });
      for (const opacity of [0, 25, 50, 75, 100]) {
        await slider.fill(String(100 - opacity));
        await expect.poll(() => page.evaluate(() => Number((window as any).testApp.settings["app.surface_opacity"]))).toBe(opacity);
        samples.push(await page.locator("#root").evaluate(el => {
          const color = getComputedStyle(el).backgroundColor;
          return color.startsWith("rgba") ? Number(color.split(",").at(-1)!.replace(")", "")) : 1;
        }));
        await expect(page.locator(".settings-group").first()).toHaveCSS("background-color", "rgba(0, 0, 0, 0)");
      }
      // The clear endpoint keeps a single readable tint; child panels remain
      // transparent. Palette contrast is verified in material-quality.spec.ts.
      expect(samples[0]).toBeGreaterThan(0);
      expect(samples[4]).toBeLessThan(1);
      for (let i = 1; i < samples.length; i++) {
        expect(samples[i]).toBeGreaterThan(samples[i - 1]);
        // CSSOM may serialize a floating alpha such as .8999999999999999 as
        // .898 after 8-bit quantization; do not treat that as a material jump.
        expect(samples[i] - samples[i - 1]).toBeCloseTo((samples[4] - samples[0]) / 4, 2);
      }
    });
  }
}
