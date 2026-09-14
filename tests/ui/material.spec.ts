import { test, expect, type Page } from "@playwright/test";

async function start(page: Page, theme = "liquid-glass", mode = "light") {
  await page.goto(`/tests/ui/harness.html?theme=${theme}&mode=${mode}`);
  await expect(page.locator(".header-title")).toHaveText("TieZ");
  await expect(page.locator("body")).toHaveClass(/native-window-frame/);
}
async function appearance(page: Page) {
  await page.getByTitle("设置", { exact: true }).click();
  await page.getByRole("heading", { name: "界面设置", exact: true }).click();
}
const alpha = (page: Page, selector: string) => page.locator(selector).first().evaluate(el => {
  const color = getComputedStyle(el).backgroundColor;
  return color.startsWith("rgba") ? Number(color.split(",").at(-1)!.replace(")", "")) : 1;
});

for (const theme of ["mica", "acrylic", "liquid-glass", "sakura"]) {
  for (const mode of ["light", "dark"]) {
    test(`${theme} ${mode}: full opacity range affects shell and controls`, async ({ page }) => {
      await start(page, theme, mode);
      await appearance(page);
      const slider = page.getByRole("slider", { name: theme === "liquid-glass" ? "液态玻璃透明度" : "界面不透明度" });
      const values: number[] = [];
      for (const opacity of [0, 1, 25, 50, 75, 99, 100]) {
        await slider.fill(String(theme === "liquid-glass" ? 100 - opacity : opacity));
        await expect.poll(() => page.evaluate(() => Number((window as any).testApp.settings["app.surface_opacity"]))).toBe(opacity);
        values.push(await alpha(page, "#root"));
        expect(await alpha(page, ".settings-group")).toBeGreaterThanOrEqual(.89);
      }
      expect(values[6] - values[0]).toBeGreaterThan(.7);
      expect(values[1] - values[0]).toBeGreaterThan(0);
      expect(values[1] - values[0]).toBeLessThan(.012);
      expect(values[6] - values[5]).toBeGreaterThan(0);
      expect(values[6] - values[5]).toBeLessThan(.012);
      for (let i = 1; i < values.length; i++) expect(values[i]).toBeGreaterThan(values[i - 1]);
      const last = values[6];
      await page.reload();
      await expect.poll(() => alpha(page, "#root")).toBe(last);
    });
  }
}

test("native corner clipping has one owner and fills all four corners", async ({ page }) => {
  for (const theme of ["mica", "acrylic", "liquid-glass", "sakura"]) {
    await start(page, theme);
    for (const selector of ["html", "body", "#root", ".app-container"]) {
      await expect(page.locator(selector)).toHaveCSS("border-top-left-radius", "0px");
      await expect(page.locator(selector)).toHaveCSS("clip-path", "none");
    }
    const bounds = await page.locator("#root").boundingBox();
    expect(bounds).toEqual({ x: 0, y: 0, width: 425, height: 589 });
  }
});

for (const width of [250, 352]) {
  test(`Windows shortcuts remain readable and record correctly at ${width}px`, async ({ page }) => {
    await page.setViewportSize({ width, height: 589 });
    await start(page);
    await page.getByTitle("设置", { exact: true }).click();
    await page.getByRole("heading", { name: "剪贴板设置", exact: true }).click();
    const rich = page.locator(".key-cap-chord").filter({ hasText: "Alt + Shift + V" });
    await expect(rich).toBeVisible();
    await expect(page.locator(".key-cap-chord").filter({ hasText: "Alt + F" })).toBeVisible();
    expect(await rich.evaluate(el => el.scrollWidth <= el.clientWidth)).toBe(true);
    await rich.click();
    await page.keyboard.press("Control+Shift+Y");
    await expect(page.locator(".key-cap-chord").filter({ hasText: "Ctrl + Shift + Y" })).toBeVisible();
    expect(await page.evaluate(() => document.documentElement.scrollWidth)).toBe(width);
  });
}

test("one transparency slider persists and ignores the removed blur option", async ({ page }) => {
  await page.addInitScript(() => {
    const saved = JSON.parse(localStorage.getItem("fixture-settings") || "{}");
    localStorage.setItem("fixture-settings", JSON.stringify({ ...saved, "app.liquid_glass_blur": "0" }));
  });
  await start(page);
  await appearance(page);
  const slider = page.getByRole("slider", { name: "液态玻璃透明度" });
  await expect(slider).toHaveCount(1);
  await expect(page.getByRole("slider", { name: "玻璃模糊度" })).toHaveCount(0);
  await slider.fill("100");
  const clear = await alpha(page, "#root");
  await expect(page.locator("#root")).toHaveCSS("backdrop-filter", "none");
  await page.reload();
  await expect.poll(() => alpha(page, "#root")).toBe(clear);
  await appearance(page);
  await expect(page.getByRole("slider", { name: "液态玻璃透明度" })).toHaveValue("100");
});

test("selection moves with interaction and reduced motion disables press animation", async ({ page }) => {
  const errors: string[] = [];
  page.on("pageerror", error => errors.push(error.message));
  await page.emulateMedia({ reducedMotion: "no-preference" });
  await start(page);
  const first = page.getByTitle("文本", { exact: true });
  const second = page.getByTitle("图片", { exact: true });
  await first.click();
  await expect(first.locator(".type-filter-selection")).toHaveCount(1);
  const initial = await first.boundingBox();
  await second.click();
  await expect(second.locator(".type-filter-selection")).toHaveCount(1);
  await expect(first.locator(".type-filter-selection")).toHaveCount(0);
  await expect.poll(async () => (await second.locator(".type-filter-selection").boundingBox())!.x).toBeGreaterThan(initial!.x);
  const settings = page.getByTitle("设置", { exact: true });
  await settings.hover(); await page.mouse.down();
  await expect(settings).toHaveCSS("scale", "0.97");
  await page.mouse.up();
  await page.evaluate(() => (window as any).testApp.emit("main-window-hidden"));
  await expect(page.locator(".type-filter-selection")).toHaveCount(0);
  await page.emulateMedia({ reducedMotion: "reduce" });
  await settings.hover(); await page.mouse.down();
  await expect(settings).toHaveCSS("transition-duration", "0s");
  await expect(settings).toHaveCSS("scale", "none");
  await page.mouse.up();
  expect(errors).toEqual([]);
});

test("only the custom background is blurred, never the labels or the content layer", async ({ page }) => {
  await start(page);
  await page.evaluate(() => {
    document.body.classList.add("has-custom-bg");
    document.documentElement.style.setProperty("--custom-bg-image", "repeating-conic-gradient(#f0c984 0% 25%, #4c78b5 0% 50%)");
    document.documentElement.style.setProperty("--custom-bg-opacity", "1");
  });
  expect(await page.locator("body").evaluate(el => getComputedStyle(el, "::before").filter)).toContain("blur(");
  for (const selector of ["#root", "header", ".header-actions", ".content-preview"]) {
    await expect(page.locator(selector).first()).toHaveCSS("filter", "none");
    await expect(page.locator(selector).first()).toHaveCSS("backdrop-filter", "none");
    await expect(page.locator(selector).first()).toHaveCSS("text-shadow", "none");
  }
});
