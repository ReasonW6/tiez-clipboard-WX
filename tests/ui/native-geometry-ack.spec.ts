import { expect, test, type Page } from "@playwright/test";

async function ready(page: Page) {
  await page.emulateMedia({ reducedMotion: "reduce" });
  await page.goto("/tests/ui/harness.html?theme=liquid-glass&nativeGlass=true");
  await expect(page.locator(".history-item").first()).toBeVisible();
  await page.evaluate(() => (window as any).testApp.setGlassStatus("ready"));
  await expect(page.locator(".header-actions > .glass-lens")).toHaveAttribute("data-native-glass", "true");
  await expect.poll(() => page.evaluate(() => (window as any).testApp.getGlassGeometryInFlight())).toBe(0);
}

for (const mode of ["light", "dark"]) {
  test(`a completed IPC cannot hide ${mode} switches before their native frame submission`, async ({ page }) => {
    await page.emulateMedia({ reducedMotion: "reduce" });
    await page.goto(`/tests/ui/harness.html?theme=liquid-glass&mode=${mode}&nativeGlass=true`);
    await page.getByTitle("设置", { exact: true }).click();
    await expect(page.locator(".settings-view .group-header").first()).toBeVisible();
    await page.evaluate(() => (window as any).testApp.setGlassStatus("ready"));
    await expect(page.locator(".settings-reading-plane > .glass-lens")).toHaveAttribute("data-native-glass", "true");
    await expect.poll(() => page.evaluate(() => (window as any).testApp.getGlassGeometryInFlight())).toBe(0);
    const before = await page.evaluate(() => {
      const fixture = (window as any).testApp;
      fixture.delays.nativeGlassPresentation = 700;
      document.querySelector<HTMLElement>(".settings-group .group-header")!.click();
      return fixture.getCompletedGlassGeometryCount();
    });
    await expect.poll(() => page.evaluate(() => (window as any).testApp.getCompletedGlassGeometryCount())).toBeGreaterThan(before);
    await expect.poll(() => page.evaluate(() => (window as any).testApp.getGlassGeometryInFlight())).toBe(0);
    const evidence = await page.locator(".switch .glass-lens").first().evaluate(async lens => {
      await new Promise<void>(resolve => requestAnimationFrame(() => requestAnimationFrame(() => resolve())));
      const rect = lens.parentElement!.getBoundingClientRect();
      const contains = (geometry: any) => geometry?.controls.some((control: any) =>
        Math.abs(control.x - rect.x) < .01 && Math.abs(control.y - rect.y) < .01
        && Math.abs(control.width - rect.width) < .01 && Math.abs(control.height - rect.height) < .01);
      const fixture = (window as any).testApp;
      return { accepted: contains(fixture.getAppliedGlassGeometry()), submitted: contains(fixture.getSubmittedGlassGeometry()),
        reflection: getComputedStyle(lens.querySelector(".glass-lens-reflection")!).display };
    });
    expect(evidence).toEqual({ accepted: true, submitted: false, reflection: "block" });
    await expect(page.locator(".switch .glass-lens-reflection").first()).toHaveCSS("display", "none");
  });
}

test("an early frame event waits for its IPC receipt and then completes the handoff", async ({ page }) => {
  await ready(page);
  await page.evaluate(() => {
    (window as any).testApp.delays.nativeGlassGeometryReply = 600;
    document.querySelector<HTMLElement>(".header-actions")!.style.transform = "translateX(-8px)";
  });
  await expect.poll(() => page.evaluate(() => (window as any).testApp.getGlassGeometryInFlight())).toBe(1);
  await expect.poll(() => page.evaluate(() => {
    const fixture = (window as any).testApp;
    const rect = document.querySelector(".header-actions")!.getBoundingClientRect();
    return fixture.getSubmittedGlassGeometry()?.controls.some((control: any) => Math.abs(control.x - rect.x) < .01);
  })).toBe(true);
  const reflection = page.locator(".header-actions > .glass-lens > .glass-lens-reflection");
  await expect(reflection).toHaveCSS("display", "block");
  await expect(reflection).toHaveCSS("display", "none");
});

test("a newer status with an old geometry cannot hide a repositioned control", async ({ page }) => {
  await ready(page);
  const oldStatus = await page.evaluate(() => (window as any).testApp.getGlassStatus());
  const count = await page.evaluate(() => {
    const fixture = (window as any).testApp;
    fixture.delays.nativeGlassPresentation = 700;
    document.querySelector<HTMLElement>(".header-actions")!.style.transform = "translateX(-12px)";
    return fixture.getCompletedGlassGeometryCount();
  });
  await expect.poll(() => page.evaluate(() => (window as any).testApp.getCompletedGlassGeometryCount())).toBeGreaterThan(count);
  await page.evaluate(async old => {
    const fixture = (window as any).testApp;
    await fixture.emitTo("main", "native-glass-status", { ...old, revision: fixture.getGlassStatus().revision + 1 });
  }, oldStatus);
  await expect(page.locator(".header-actions .glass-lens-reflection")).toHaveCSS("display", "block");
  // Advance the fixture's native status sequence beyond the synthetic event.
  await page.evaluate(async () => { await (window as any).testApp.setGlassStatus("pending"); await (window as any).testApp.setGlassStatus("ready"); });
  await expect(page.locator(".header-actions .glass-lens-reflection")).toHaveCSS("display", "none");
});

test("a viewport change invalidates the old receipt even for an anchored control", async ({ page }) => {
  await ready(page);
  const reflection = page.locator(".liquid-search .glass-lens-reflection");
  await expect(reflection).toHaveCSS("display", "none");
  const before = await page.locator(".liquid-search").boundingBox();
  await page.evaluate(() => { (window as any).testApp.delays.nativeGlassPresentation = 700; });
  await page.setViewportSize({ width: 425, height: 650 });
  expect(await page.locator(".liquid-search").boundingBox()).toEqual(before);
  await expect(reflection).toHaveCSS("display", "block");
  await expect(reflection).toHaveCSS("display", "none");
});

test("unknown frame receipts retain the fallback and known receipts recover it", async ({ page }) => {
  await ready(page);
  await page.evaluate(async () => {
    const fixture = (window as any).testApp;
    await fixture.emitTo("main", "native-glass-status", { state: "ready", revision: fixture.getGlassStatus().revision + 1, geometryRevision: 900000 });
  });
  await expect(page.locator(".header-actions .glass-lens-reflection")).toHaveCSS("display", "block");
  await page.evaluate(async () => { await (window as any).testApp.setGlassStatus("suspended"); await (window as any).testApp.setGlassStatus("ready"); });
  await expect(page.locator(".header-actions .glass-lens-reflection")).toHaveCSS("display", "none");
});

for (const mode of ["light", "dark"]) {
  test(`an unknown ${mode} frame restores CSS glass before another animation frame`, async ({ page }, testInfo) => {
    await page.clock.install();
    await page.emulateMedia({ reducedMotion: "reduce" });
    await page.goto(`/tests/ui/harness.html?theme=liquid-glass&mode=${mode}&nativeGlass=true`);
    await expect(page.locator(".history-item").first()).toBeVisible();
    await expect.poll(() => page.evaluate(() => (window as any).testApp.getGlassStatus().state)).toBe("pending");
    await page.evaluate(() => (window as any).testApp.setGlassStatus("ready"));
    const reflection = page.locator(".header-actions .glass-lens-reflection");
    await expect(reflection).toHaveCSS("display", "none");
    await page.clock.pauseAt(new Date(Date.now() + 1000));
    const immediate = await page.evaluate(async () => {
      const fixture = (window as any).testApp;
      const known = fixture.getGlassStatus();
      const lens = document.querySelector<HTMLElement>(".header-actions > .glass-lens")!;
      const shadow = getComputedStyle(lens.querySelector(".glass-lens-shadow")!).boxShadow;
      await fixture.emitTo("main", "native-glass-status", {
        state: "ready", revision: known.revision + 1, geometryRevision: 900000,
      });
      return { native: lens.dataset.nativeGlass,
        reflection: getComputedStyle(lens.querySelector(".glass-lens-reflection")!).display,
        shadowBefore: shadow, shadowAfter: getComputedStyle(lens.querySelector(".glass-lens-shadow")!).boxShadow };
    });
    await testInfo.attach("status-before-next-frame", { body: JSON.stringify(immediate, null, 2), contentType: "application/json" });
    // Inspect the event result directly: a retrying locator assertion would let
    // the next rAF conceal an interval with neither confirmed nor CSS optics.
    expect(immediate.native).toBe("false");
    expect(immediate.reflection).toBe("block");
    expect(immediate.shadowAfter).toBe(immediate.shadowBefore);
    await page.clock.resume();
    await page.evaluate(async () => {
      await (window as any).testApp.setGlassStatus("suspended");
      await (window as any).testApp.setGlassStatus("ready");
    });
    await expect(reflection).toHaveCSS("display", "none");
  });
}
