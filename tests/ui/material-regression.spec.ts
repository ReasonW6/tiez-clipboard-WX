import { test, expect } from "@playwright/test";

for (const theme of ["mica", "acrylic", "liquid-glass"]) {
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
      const slider = page.getByRole("slider", { name: theme === "liquid-glass" ? "液态玻璃透明度" : "界面透明度" });
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

test("every category has its own isolated lens without an overlapping selection lens", async ({ page }) => {
  await page.goto("/tests/ui/harness.html?theme=liquid-glass");
  await page.getByTitle("文本", { exact: true }).click();
  await expect(page.locator(".type-filter-selection")).toHaveCount(0);
  for (const button of await page.locator(".type-filter-btn").all()) {
    await expect(button.locator(":scope > .glass-lens")).toHaveCount(1);
    await expect(button).toHaveCSS("isolation", "isolate");
  }
});

for (const width of [250, 352, 425]) {
  test(`category surfaces reserve shadow space and scroll within the viewport at ${width}px`, async ({ page }) => {
    await page.setViewportSize({ width, height: 589 });
    await page.goto("/tests/ui/harness.html?theme=liquid-glass");
    await page.getByTitle("文本", { exact: true }).click();
    for (const title of ["文件", "富文本"]) {
      const button = page.getByTitle(title, { exact: true });
      await button.click();
      const bounds = await button.evaluate(element => {
        const pill = element.getBoundingClientRect();
        const scroll = element.closest(".type-filter-scroll")!.getBoundingClientRect();
        return { top: pill.top - scroll.top, bottom: scroll.bottom - pill.bottom, left: pill.left - scroll.left, right: scroll.right - pill.right };
      });
      expect(bounds.top).toBeGreaterThanOrEqual(8);
      expect(bounds.bottom).toBeGreaterThanOrEqual(10);
      expect(bounds.left).toBeGreaterThanOrEqual(0);
      expect(bounds.right).toBeGreaterThanOrEqual(0);
      await expect(button).toHaveAttribute("aria-pressed", "true");
    }
    expect(await page.evaluate(() => document.documentElement.scrollWidth)).toBe(width);
  });
}
