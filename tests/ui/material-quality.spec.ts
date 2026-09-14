import { test, expect, type Page } from "@playwright/test";

async function start(page: Page, theme = "liquid-glass", mode = "light") {
  await page.goto(`/tests/ui/harness.html?theme=${theme}&mode=${mode}`);
  await expect(page.locator(".history-item").first()).toBeVisible();
  await expect.poll(() => page.evaluate(() => (window as any).testApp.calls.filter((c: any) => c.command === "set_theme").length)).toBeGreaterThan(0);
}
const nativeCalls = (page: Page) => page.evaluate(() => (window as any).testApp.calls.filter((c: any) => c.command === "set_theme").length);

for (const theme of ["liquid-glass", "mica", "acrylic"]) {
  test(`${theme}: endpoints must not switch native rendering mode`, async ({ page }) => {
    await start(page, theme);
    await page.getByTitle("设置", { exact: true }).click();
    await page.getByRole("heading", { name: "界面设置", exact: true }).click();
    const before = await nativeCalls(page);
    const slider = page.getByRole("slider", { name: theme === "liquid-glass" ? "液态玻璃透明度" : "界面不透明度" });
    for (const opacity of [0, 1, 25, 50, 75, 99, 100]) {
      await slider.fill(String(theme === "liquid-glass" ? 100 - opacity : opacity));
      await expect.poll(() => page.evaluate(() => Number((window as any).testApp.settings["app.surface_opacity"]))).toBe(opacity);
      await expect.poll(() => nativeCalls(page)).toBe(before);
    }
  });
}

test("startup does not scan applications or create a hidden preview webview", async ({ page }) => {
  await page.addInitScript(() => localStorage.setItem("fixture-settings", JSON.stringify({"app.compact_mode":"true"})));
  await start(page);
  await page.waitForTimeout(2300); // Covers the former delayed WebView warmup.
  const commands = await page.evaluate(() => (window as any).testApp.calls.map((c: any) => c.command));
  expect(commands).not.toContain("scan_installed_apps");
  expect(commands).not.toContain("get_system_default_app");
  expect(commands).not.toContain("plugin:webview|create_webview_window");
});

test("content text has no blur, glow, displacement or global backdrop filter", async ({ page }) => {
  await start(page);
  await expect(page.locator("#root")).toHaveCSS("backdrop-filter", "none");
  await expect(page.locator(".content-preview").first()).toHaveCSS("text-shadow", "none");
  await expect(page.locator(".content-preview").first()).toHaveCSS("filter", "none");
  await expect(page.locator("feTurbulence, feDisplacementMap")).toHaveCount(0);
});

// Composite actual CSS surfaces over adversarial desktop colors, not a flattering wallpaper.
for (const theme of ["liquid-glass", "mica", "acrylic"]) {
  for (const mode of ["light", "dark"]) {
    test(`${theme} ${mode}: text contrast survives all slider stops and desktop colors`, async ({ page }) => {
      for (const opacity of [0, 1, 25, 50, 75, 99, 100]) {
        await page.goto(`/tests/ui/harness.html?theme=${theme}&mode=${mode}&opacity=${opacity}`);
        await expect(page.locator(".history-item").first()).toBeVisible();
        await expect.poll(() => page.evaluate(() => getComputedStyle(document.documentElement).getPropertyValue("--surface-opacity"))).toBe(String(opacity / 100));
        for (const background of ["#000000", "#8b8b8b", "#ffffff"]) {
          await page.evaluate(background => document.documentElement.style.setProperty("background-color", background, "important"), background);
          const ratios = await page.evaluate(() => {
            type Color = number[];
            const parse = (value: string): Color => {
              const c = value.match(/[\d.]+/g)!.map(Number);
              return [c[0], c[1], c[2], c[3] ?? 1];
            };
            const over = (foreground: Color, background: Color): Color => foreground.slice(0, 3).map((c, i) => c * foreground[3] + background[i] * (1 - foreground[3])).concat(1);
            const luminance = (color: Color) => color.slice(0, 3).map(c => c / 255).map(c => c <= .04045 ? c / 12.92 : ((c + .055) / 1.055) ** 2.4).reduce((n, c, i) => n + c * [.2126, .7152, .0722][i], 0);
            return [".header-title", ".content-preview", ".item-meta", ".type-filter-label"].map(selector => {
              const element = document.querySelector(selector)!;
              const ancestors: Element[] = [];
              for (let current: Element | null = element; current; current = current.parentElement) ancestors.unshift(current);
              let bg: Color = [255, 255, 255, 1];
              for (const ancestor of ancestors) bg = over(parse(getComputedStyle(ancestor).backgroundColor), bg);
              const text = parse(getComputedStyle(element).color);
              text[3] *= ancestors.reduce((opacity, ancestor) => opacity * Number(getComputedStyle(ancestor).opacity), 1);
              const a = luminance(over(text, bg)), b = luminance(bg);
              return { selector, contrast: (Math.max(a, b) + .05) / (Math.min(a, b) + .05) };
            });
          });
          for (const result of ratios) expect(result.contrast, `${opacity}% on ${background}: ${result.selector}`).toBeGreaterThanOrEqual(4.5);
        }
      }
    });
  }
}

test("application discovery is lazy and cached across closing settings", async ({ page }) => {
  await start(page);
  await page.getByTitle("设置", { exact: true }).click();
  await page.getByRole("heading", { name: "界面设置", exact: true }).click();
  const scanCount = () => page.evaluate(() => (window as any).testApp.calls.filter((c: any) => c.command === "scan_installed_apps").length);
  expect(await scanCount()).toBe(0);
  await page.getByRole("heading", { name: "默认打开程序", exact: true }).click();
  await expect.poll(scanCount).toBe(1);
  await expect.poll(() => page.evaluate(() => (window as any).testApp.calls.filter((c: any) => c.command === "get_system_default_app").length)).toBe(6);
  await page.evaluate(() => (window as any).testApp.emit("main-window-hidden"));
  await page.getByTitle("设置", { exact: true }).click();
  await page.getByRole("heading", { name: "默认打开程序", exact: true }).click();
  expect(await scanCount()).toBe(1);
});

test("saved material is already applied while native settings are still loading", async ({ page }) => {
  await start(page);
  await page.getByTitle("设置", { exact: true }).click();
  await page.getByRole("heading", { name: "界面设置", exact: true }).click();
  await page.getByRole("slider", { name: "液态玻璃透明度" }).fill("100");
  await expect.poll(() => page.evaluate(() => localStorage.getItem("tiez_surface_opacity"))).toBe("0");
  const saved = await page.locator("#root").evaluate(el => getComputedStyle(el).backgroundColor);
  await page.goto("/tests/ui/harness.html?theme=liquid-glass&mode=light&settingsDelay=3000");
  await expect(page.locator("body")).toHaveClass(/theme-liquid-glass/);
  await expect(page.locator("#root")).toHaveCSS("background-color", saved);
  expect(await page.evaluate(() => (window as any).testApp.calls.filter((c: any) => c.command === "set_theme").length)).toBe(0);
});
