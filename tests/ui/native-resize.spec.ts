import { test, expect, type Page } from "@playwright/test";

async function start(page: Page, resizable = true) {
  await page.emulateMedia({ reducedMotion: "reduce" });
  await page.goto(`/tests/ui/harness.html?theme=liquid-glass&mode=light&nativeGlass=true&resizable=${resizable}`);
  await expect(page.locator(".header-title")).toHaveText("TieZ");
  await expect.poll(() => page.evaluate(() => (window as any).testApp.calls.some((call: any) => call.command === "get_native_glass_status"))).toBe(true);
}

async function ready(page: Page) {
  await page.evaluate(() => (window as any).testApp.setGlassStatus("ready"));
  await expect(page.locator("body")).toHaveClass(/native-glass-ready/);
}

async function resizeCalls(page: Page) {
  return page.evaluate(() => (window as any).testApp.calls.filter((call: any) => call.command === "plugin:window|start_resize_dragging").map((call: any) => call.args));
}

test("native resize targets use all eight public directions only after readiness", async ({ page }) => {
  await start(page);
  await expect(page.locator(".native-glass-resize-handles")).toBeHidden();
  await ready(page);
  await expect(page.locator(".native-glass-resize-handles")).toBeVisible();
  await expect(page.locator(".native-glass-resize-handles")).toHaveAttribute("aria-hidden", "true");
  const { width, height } = page.viewportSize()!;
  const points = [
    ["North", width / 2, 2], ["NorthEast", width - 2, 12], ["East", width - 2, height / 2],
    ["SouthEast", width - 12, height - 2], ["South", width / 2, height - 2],
    ["SouthWest", 2, height - 12], ["West", 2, height / 2], ["NorthWest", 12, 2]
  ] as const;
  for (const [direction, x, y] of points) {
    expect(await page.evaluate(({ x, y }) => document.elementFromPoint(x, y)?.getAttribute("data-resize-direction"), { x, y })).toBe(direction);
    await page.mouse.click(x, y);
  }
  expect(await resizeCalls(page)).toEqual(points.map(([value]) => ({ label: "main", value })));
  await page.mouse.click(2, height / 2, { button: "right" });
  expect((await resizeCalls(page)).length).toBe(8);
  for (const state of ["suspended", "failed", "pending"]) {
    await page.evaluate(state => (window as any).testApp.setGlassStatus(state), state);
    await expect(page.locator(".native-glass-resize-handles")).toBeHidden();
    expect(await page.evaluate(() => document.elementFromPoint(2, innerHeight / 2)?.closest(".native-glass-resize-handles") !== null)).toBe(false);
  }
});

test("native resize bands leave corner interiors, content and keyboard focus available", async ({ page }) => {
  await start(page);
  await ready(page);
  const interiorPoints = await page.evaluate(() => [[9, 9], [innerWidth - 9, 9], [9, innerHeight - 9], [innerWidth - 9, innerHeight - 9], [innerWidth / 2, innerHeight / 2]]
    .map(([x, y]) => !!document.elementFromPoint(x, y)?.closest(".native-glass-resize-handles")));
  expect(interiorPoints).toEqual([false, false, false, false, false]);
  expect(await page.locator(".native-glass-resize-handles > span").evaluateAll(elements => elements.every(element => (element as HTMLElement).tabIndex === -1))).toBe(true);
  await page.locator(".search-input").click();
  await page.locator(".search-input").fill("fictional resize fixture");
  await expect(page.locator(".search-input")).toBeFocused();
  expect(await resizeCalls(page)).toEqual([]);
  await page.mouse.click(2, page.viewportSize()!.height / 2);
  await expect(page.locator(".search-input")).toBeFocused();
  await page.getByTitle("设置", { exact: true }).click();
  await expect(page.getByRole("heading", { name: "界面设置", exact: true })).toBeVisible();
  expect(await resizeCalls(page)).toEqual([{ label: "main", value: "West" }]);
});

test("a non-resizable window keeps its edges free even when native glass is ready", async ({ page }) => {
  await start(page, false);
  await ready(page);
  await expect(page.locator(".native-glass-resize-handles")).toHaveCount(0);
  expect(await resizeCalls(page)).toEqual([]);
});

test("maximized and fullscreen windows hide the bands with coalesced native state reads", async ({ page }) => {
  await start(page);
  await ready(page);
  await expect(page.locator(".native-glass-resize-handles")).toBeVisible();
  const queriesBefore = await page.evaluate(() => (window as any).testApp.calls.filter((call: any) => ["plugin:window|is_resizable", "plugin:window|is_maximized", "plugin:window|is_fullscreen"].includes(call.command)).length);
  await page.evaluate(async () => { for (let i = 0; i < 10; i++) await (window as any).testApp.setResizeState({ maximized: true }); });
  await expect(page.locator(".native-glass-resize-handles")).toHaveCount(0);
  const queriesAfter = await page.evaluate(() => (window as any).testApp.calls.filter((call: any) => ["plugin:window|is_resizable", "plugin:window|is_maximized", "plugin:window|is_fullscreen"].includes(call.command)).length);
  expect(queriesAfter - queriesBefore).toBe(3);
  await page.evaluate(() => (window as any).testApp.setResizeState({ maximized: false, fullscreen: true }));
  await expect.poll(() => page.evaluate(() => (window as any).testApp.calls.filter((call: any) => ["plugin:window|is_resizable", "plugin:window|is_maximized", "plugin:window|is_fullscreen"].includes(call.command)).length)).toBe(queriesAfter + 3);
  await expect(page.locator(".native-glass-resize-handles")).toHaveCount(0);
  await page.evaluate(() => (window as any).testApp.setResizeState({ fullscreen: false }));
  await expect(page.locator(".native-glass-resize-handles")).toBeVisible();
  expect(await resizeCalls(page)).toEqual([]);
});
