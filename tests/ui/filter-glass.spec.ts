import { test, expect, type Page } from "@playwright/test";

const geometry = (page: Page) => page.evaluate(() => (window as any).testApp.calls
  .filter((call: any) => call.command === "set_glass_geometry").at(-1)?.args.geometry);
const fill = (button: ReturnType<Page["locator"]>) => button.evaluate(element => getComputedStyle(element, "::after").backgroundColor);

for (const mode of ["light", "dark"]) for (const width of [250, 425]) {
  test(`category glass preserves visible surfaces, selection and keyboard focus: ${mode}, ${width}px`, async ({ page }, testInfo) => {
    await page.setViewportSize({ width, height: 589 });
    await page.emulateMedia({ reducedMotion: "reduce" });
    await page.goto(`/tests/ui/harness.html?theme=liquid-glass&mode=${mode}&opacity=0&nativeGlass=true`);
    const buttons = page.locator(".type-filter-btn");
    await expect(buttons).toHaveCount(7);
    await expect(page.locator(".type-filter-btn > .glass-lens")).toHaveCount(7);
    await expect(page.locator(".type-filter-selection, .glass-lens .glass-lens")).toHaveCount(0);
    const first = buttons.first();
    await expect(first.locator(".glass-lens-refraction")).toHaveCount(1);
    await expect(first).toHaveCSS("border-top-color", "rgba(0, 0, 0, 0)");
    await expect(first.locator(".glass-lens-reflection")).not.toHaveCSS("background-color", "rgba(0, 0, 0, 0)");
    const shadow = await first.locator(".glass-lens-shadow").evaluate(element => getComputedStyle(element).boxShadow);
    expect(shadow).not.toBe("none");
    const neutral = await fill(first);
    await first.hover();
    await expect.poll(() => fill(first)).not.toBe(neutral);
    const hover = await fill(first);
    await first.click();
    await expect(first).toHaveAttribute("aria-pressed", "true");
    await expect.poll(() => fill(first)).not.toBe(hover);
    await page.mouse.move(width - 1, 580);
    const selected = await fill(first);
    // Source-over contrast for the flat center over black/white desktop colors.
    // Native refraction and edge pixels are validated separately in the real app.
    const ratios = await first.evaluate(element => {
      const parse = (color: string) => {
        const values = color.match(/[\d.]+/g)!.map(Number);
        return [values[0], values[1], values[2], values[3] ?? 1];
      };
      const over = (front: number[], back: number[]) => front.slice(0, 3).map((c, i) => c * front[3] + back[i] * (1 - front[3])).concat(1);
      const luminance = (color: number[]) => color.slice(0, 3).map(c => c / 255).map(c => c <= .04045 ? c / 12.92 : ((c + .055) / 1.055) ** 2.4).reduce((sum, c, i) => sum + c * [.2126, .7152, .0722][i], 0);
      const ink = parse(getComputedStyle(element.querySelector(".type-filter-label")!).color);
      const surface = parse(getComputedStyle(document.querySelector("#root")!).backgroundColor);
      const tint = parse(getComputedStyle(element.querySelector(".glass-lens-reflection")!).backgroundColor);
      const selection = parse(getComputedStyle(element, "::after").backgroundColor);
      return [0, 255].map(value => {
        const background = over(selection, over(tint, over(surface, [value, value, value, 1])));
        const a = luminance(over(ink, background)), b = luminance(background);
        return (Math.max(a, b) + .05) / (Math.min(a, b) + .05);
      });
    });
    for (const ratio of ratios) expect(ratio).toBeGreaterThanOrEqual(4.5);
    await page.locator(".type-filter-scroll").screenshot({ path: testInfo.outputPath("filter-fallback.png") });
    for (const state of ["ready", "suspended", "ready", "failed"]) {
      await page.evaluate(state => (window as any).testApp.setGlassStatus(state), state);
      await expect(first.locator(".glass-lens-reflection")).toHaveCSS("display", state === "ready" ? "none" : "block");
      await expect(first.locator(".glass-lens-shadow")).toHaveCSS("box-shadow", shadow);
      expect(await fill(first)).toBe(selected);
    }
    await first.focus();
    await page.keyboard.press("Tab");
    const second = buttons.nth(1);
    await expect(second).toBeFocused();
    await expect(second).toHaveCSS("outline-style", "solid");
    await page.keyboard.press("Space");
    await expect(second).toHaveAttribute("aria-pressed", "true");
    await expect(first).toHaveAttribute("aria-pressed", "false");
    await page.keyboard.press("Space");
    await expect(second).toHaveAttribute("aria-pressed", "false");
    await buttons.last().focus();
    await page.keyboard.press("Enter");
    await expect(buttons.last()).toHaveAttribute("aria-pressed", "true");
    const lastBounds = await buttons.last().boundingBox();
    expect(lastBounds!.x).toBeGreaterThanOrEqual(0);
    expect(lastBounds!.x + lastBounds!.width).toBeLessThanOrEqual(width);
    expect(await page.evaluate(() => document.documentElement.scrollWidth)).toBe(width);
    expect(await buttons.last().locator(".type-filter-label").evaluate(element => {
      for (let current: Element | null = element; current; current = current.parentElement) {
        if (getComputedStyle(current).filter !== "none") return false;
      }
      return true;
    })).toBe(true);
    await page.emulateMedia({ forcedColors: "active" });
    await expect(buttons.last().locator(".glass-lens-shadow")).toHaveCSS("box-shadow", "none");
    await expect(buttons.last().locator(".type-filter-label")).toHaveCSS("text-decoration-line", "underline");
    await expect(buttons.last()).toHaveCSS("outline-style", "solid");
  });
}

test("horizontal scrolling keeps clipped category optics in CSS and submits only eligible native shapes", async ({ page }) => {
  await page.setViewportSize({ width: 250, height: 589 });
  await page.emulateMedia({ reducedMotion: "reduce" });
  await page.goto("/tests/ui/harness.html?theme=liquid-glass&mode=light&nativeGlass=true&empty=true");
  const first = page.locator(".type-filter-btn").first();
  const last = page.locator(".type-filter-btn").last();
  await expect.poll(() => page.evaluate(() => (window as any).testApp.getGlassStatus().state)).toBe("pending");
  await page.evaluate(() => (window as any).testApp.setGlassStatus("ready"));
  await expect(first.locator(".glass-lens")).toHaveAttribute("data-native-glass", "true");
  await expect(last.locator(".glass-lens")).toHaveAttribute("data-native-glass", "false");
  await expect(last.locator(".glass-lens-reflection")).not.toHaveCSS("display", "none");
  const scroll = page.locator(".type-filter-scroll");
  await scroll.evaluate(element => { element.scrollLeft = 14; });
  const partialBounds = await first.evaluate(element => {
    const button = element.getBoundingClientRect();
    const scroll = element.closest(".type-filter-scroll")!.getBoundingClientRect();
    return { left: button.left - scroll.left, right: button.right - scroll.left };
  });
  expect(partialBounds.left).toBeLessThan(0);
  expect(partialBounds.right).toBeGreaterThan(0);
  await expect(first.locator(".glass-lens")).toHaveAttribute("data-native-glass", "false");
  await expect(first.locator(".glass-lens-reflection")).not.toHaveCSS("display", "none");
  await scroll.hover();
  await page.mouse.wheel(0, 400);
  await expect.poll(() => scroll.evaluate(element => element.scrollLeft)).toBeGreaterThan(0);
  await expect(last.locator(".glass-lens")).toHaveAttribute("data-native-glass", "true");
  await expect(first.locator(".glass-lens")).toHaveAttribute("data-native-glass", "false");
  await expect(first.locator(".glass-lens-reflection")).not.toHaveCSS("display", "none");
  expect((await geometry(page)).controls.length).toBeLessThanOrEqual(9);
});

test("pressing one category deforms only that category", async ({ page }) => {
  await page.emulateMedia({ reducedMotion: "no-preference" });
  await page.goto("/tests/ui/harness.html?theme=liquid-glass&mode=light&nativeGlass=true&empty=true");
  const first = page.locator(".type-filter-btn").first();
  await expect.poll(() => page.evaluate(() => (window as any).testApp.getGlassStatus().state)).toBe("pending");
  await page.evaluate(() => (window as any).testApp.setGlassStatus("ready"));
  await expect(first.locator(".glass-lens")).toHaveAttribute("data-native-glass", "true");
  await first.dispatchEvent("pointerdown", { button: 0 });
  await expect.poll(async () => (await geometry(page)).controls.filter((control: any) => control.pressure > .75).length).toBe(1);
  const box = await first.boundingBox();
  const pressed = (await geometry(page)).controls.filter((control: any) => control.pressure > 0);
  expect(pressed).toHaveLength(1);
  expect(pressed[0].x).toBeCloseTo(box!.x, 2);
  expect(pressed[0].y).toBeCloseTo(box!.y, 2);
  await page.evaluate(() => window.dispatchEvent(new PointerEvent("pointerup")));
  await expect.poll(async () => (await geometry(page)).controls.every((control: any) => control.pressure === 0)).toBe(true);
});

test("category glass remains exclusive to the liquid theme", async ({ page }) => {
  for (const theme of ["mica", "acrylic"]) {
    await page.goto(`/tests/ui/harness.html?theme=${theme}&mode=light`);
    await expect(page.locator(".type-filter-btn")).toHaveCount(7);
    await expect(page.locator(".type-filter-btn > .glass-lens")).toHaveCount(0);
    await expect(page.locator(".type-filter-btn").first()).not.toHaveCSS("border-top-color", "rgba(0, 0, 0, 0)");
  }
});
