import { test, expect } from "@playwright/test";

test("glass shares ancestor measurements but still follows transforms each frame", async ({ page }) => {
  await page.addInitScript(() => {
    const original = window.getComputedStyle.bind(window);
    const trace = { enabled: false, current: 0, frames: [] as number[] };
    (window as any).glassMeasurementTrace = trace;
    window.getComputedStyle = (element, pseudo) => {
      if (trace.enabled && element.classList.contains("app-container")) ++trace.current;
      return original(element, pseudo);
    };
    const next = () => {
      if (trace.enabled) { trace.frames.push(trace.current); trace.current = 0; }
      requestAnimationFrame(next);
    };
    requestAnimationFrame(next);
  });
  await page.goto("/tests/ui/harness.html?theme=liquid-glass&nativeGlass=true");
  await expect(page.locator(".history-item").first()).toBeVisible();
  await expect.poll(() => page.evaluate(() => (window as any).testApp.calls.filter((c: any) => c.command === "set_glass_geometry").at(-1)?.args.geometry.controls.length)).toBeGreaterThan(5);
  await page.evaluate(() => { (window as any).glassMeasurementTrace.enabled = true; });
  await expect.poll(() => page.evaluate(() => (window as any).glassMeasurementTrace.frames.length)).toBeGreaterThan(30);
  const reads = await page.evaluate(() => (window as any).glassMeasurementTrace.frames.slice(2));
  expect(Math.max(...reads)).toBeLessThanOrEqual(4);
  const position = () => page.evaluate(() => (window as any).testApp.calls.filter((c: any) => c.command === "set_glass_geometry").at(-1)?.args.geometry.controls[0].x);
  const before = await position();
  await page.locator(".app-container").evaluate(element => { (element as HTMLElement).style.transform = "translateX(3px)"; });
  await expect.poll(position).not.toBe(before);
  await page.locator(".app-container").evaluate(element => { (element as HTMLElement).style.transform = ""; });
  await expect.poll(position).toBe(before);
});
