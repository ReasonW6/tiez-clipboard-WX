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
const lastBackdrop = (page: Page) => page.evaluate(() =>
  (window as any).testApp.calls.filter((c: any) => c.command === "set_theme").at(-1)?.args.backdropEnabled);
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
      await slider.fill(theme === "liquid-glass" ? "100" : "0");
      await expect.poll(() => lastBackdrop(page)).toBe(false);
      // Sakura adds a decorative image; its base color must still reach zero alpha.
      await expect.poll(() => alpha(page, "#root")).toBeLessThan(.01);
      await expect.poll(() => alpha(page, ".settings-group")).toBeLessThan(.18);
      if (theme !== "sakura") {
        // Opaque legacy toolbar/content layers must not cover a clear shell.
        for (const selector of ["header", "main"]) {
          expect(await alpha(page, selector)).toBe(0);
          await expect(page.locator(selector)).toHaveCSS("background-image", "none");
        }
      }
      await slider.fill(theme === "liquid-glass" ? "0" : "100");
      await expect.poll(() => lastBackdrop(page)).toBe(true);
      await expect.poll(() => alpha(page, "#root")).toBeGreaterThan(.98);
      await expect.poll(() => alpha(page, ".settings-group")).toBeGreaterThan(.98);
      await page.reload();
      await expect.poll(() => alpha(page, "#root")).toBeGreaterThan(.98);
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

test("one transparency slider controls tint, blur and light; old blur settings cannot override it", async ({ page }) => {
  await page.addInitScript(() => {
    const saved = JSON.parse(localStorage.getItem("fixture-settings") || "{}");
    localStorage.setItem("fixture-settings", JSON.stringify({ ...saved, "app.liquid_glass_blur": "0" }));
  });
  await start(page);
  await appearance(page);
  const slider = page.getByRole("slider", { name: "液态玻璃透明度" });
  await expect(slider).toHaveCount(1);
  await expect(page.getByRole("slider", { name: "玻璃模糊度" })).toHaveCount(0);
  await slider.fill("0");
  await expect.poll(() => page.evaluate(() => getComputedStyle(document.body).getPropertyValue("--glass-blur"))).toBe("64px");
  await expect.poll(() => lastBackdrop(page)).toBe(true);
  await slider.fill("100");
  await expect.poll(() => page.evaluate(() => getComputedStyle(document.body).getPropertyValue("--glass-blur"))).toBe("0px");
  await expect.poll(() => lastBackdrop(page)).toBe(false);
  await page.reload();
  await expect.poll(() => lastBackdrop(page)).toBe(false);
  await expect.poll(() => page.evaluate(() => getComputedStyle(document.body).getPropertyValue("--glass-blur"))).toBe("0px");
});


test("liquid highlights, press feedback and reduced motion respect lifecycle", async ({ page }) => {
  const errors: string[] = [];
  page.on("pageerror", error => errors.push(error.message));
  await page.emulateMedia({ reducedMotion: "no-preference" });
  await start(page);
  await page.mouse.move(30, 55);
  await expect.poll(() => page.evaluate(() => document.documentElement.style.getPropertyValue("--liquid-x"))).not.toBe("");
  const button = page.getByTitle("图片", { exact: true });
  await button.hover();
  await page.mouse.down();
  await expect(button.locator(".liquid-press-wave")).toHaveCount(1);
  await page.mouse.up();
  await expect(button.locator(".liquid-press-wave")).toHaveCount(0);
  await expect(button.locator(".type-filter-selection")).toHaveCount(1);
  await page.evaluate(() => (window as any).testApp.emit("main-window-hidden"));
  await expect.poll(() => page.evaluate(() => document.documentElement.style.getPropertyValue("--liquid-x"))).toBe("");
  await page.emulateMedia({ reducedMotion: "reduce" });
  await button.hover(); await page.mouse.down();
  await expect(page.locator(".liquid-press-wave")).toHaveCount(0);
  await page.mouse.up();
  await expect(button).toHaveCSS("transition-duration", "0s");
  expect(errors).toEqual([]);
});

test("the lens filter changes backdrop pixels without changing labels", async ({ page }) => {
  await page.emulateMedia({ reducedMotion: "reduce" });
  await start(page);
  await page.evaluate(() => {
    document.body.classList.add("has-custom-bg");
    document.documentElement.style.setProperty("--custom-bg-image", "repeating-conic-gradient(#f0c984 0% 25%, #4c78b5 0% 50%)");
    document.documentElement.style.setProperty("--custom-bg-opacity", "1");
    document.documentElement.style.setProperty("--glass-blur", "0px");
  });
  const glass = page.locator(".header-actions");
  const labels = await glass.innerText();
  await page.locator("feDisplacementMap").evaluate(el => el.setAttribute("scale", "0"));
  const flat = await glass.screenshot();
  await page.locator("feDisplacementMap").evaluate(el => el.setAttribute("scale", "32"));
  const refracted = await glass.screenshot();
  expect(flat.equals(refracted)).toBe(false);
  expect(await glass.innerText()).toBe(labels);
});
