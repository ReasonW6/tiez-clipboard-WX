import { expect, test, type Page } from "@playwright/test";

async function ready(page: Page) {
  await page.emulateMedia({ reducedMotion: "reduce" });
  await page.goto("/tests/ui/harness.html?theme=liquid-glass&nativeGlass=true");
  await expect(page.locator(".history-item").first()).toBeVisible();
  await page.evaluate(() => (window as any).testApp.setGlassStatus("ready"));
  await expect(page.locator(".header-actions > .glass-lens")).toHaveAttribute("data-native-glass", "true");
  await expect.poll(() => page.evaluate(() => (window as any).testApp.getGlassGeometryInFlight())).toBe(0);
}

test("press animation keeps confirmed glass while an IPC reply is delayed", async ({ page }, testInfo) => {
  await ready(page);
  await page.emulateMedia({ reducedMotion: "no-preference" });
  const evidence = await page.evaluate(async () => {
    const fixture = (window as any).testApp;
    fixture.delays.nativeGlassGeometryReply = 600;
    const lens = document.querySelector<HTMLElement>(".header-actions > .glass-lens")!;
    const changes: string[] = [];
    const observer = new MutationObserver(() => changes.push(lens.dataset.nativeGlass!));
    observer.observe(lens, { attributes: true, attributeFilter: ["data-native-glass"] });
    lens.parentElement!.dispatchEvent(new PointerEvent("pointerdown", { button: 0, bubbles: true }));
    await new Promise(resolve => setTimeout(resolve, 400));
    const status = fixture.getGlassStatus();
    const submitted = fixture.getSubmittedGlassGeometry();
    window.dispatchEvent(new PointerEvent("pointerup"));
    observer.disconnect();
    return { changes, native: lens.dataset.nativeGlass,
      reflection: getComputedStyle(lens.querySelector(".glass-lens-reflection")!).display,
      pressure: Math.max(...submitted.controls.map((control: any) => control.pressure)),
      geometryRevision: status.geometryRevision };
  });
  await testInfo.attach("press-handoff", { body: JSON.stringify(evidence, null, 2), contentType: "application/json" });
  expect(evidence.pressure).toBeGreaterThan(0);
  expect(evidence.changes).not.toContain("false");
  expect(evidence.native).toBe("true");
  expect(evidence.reflection).toBe("none");
});

test("native slider pressure does not resize its DOM footprint", async ({ page }) => {
  await ready(page);
  await page.getByTitle("设置", { exact: true }).click();
  await page.getByRole("heading", { name: "界面设置", exact: true }).click();
  await page.emulateMedia({ reducedMotion: "no-preference" });
  await page.getByRole("slider", { name: "液态玻璃透明度", exact: true }).scrollIntoViewIfNeeded();
  await page.evaluate(() => (window as any).testApp.setGlassStatus("ready"));
  const thumb = page.locator(".glass-slider-control:has(> .glass-transparency-slider) > .glass-slider-thumb");
  await expect(thumb.locator(".glass-lens")).toHaveAttribute("data-native-glass", "true");
  const before = await thumb.boundingBox();
  const cdp = await page.context().newCDPSession(page);
  await cdp.send("DOM.enable");
  await cdp.send("CSS.enable");
  const { root } = await cdp.send("DOM.getDocument");
  const { nodeId } = await cdp.send("DOM.querySelector", { nodeId: root.nodeId, selector: ".glass-slider-control:has(> .glass-transparency-slider)" });
  await cdp.send("CSS.forcePseudoState", { nodeId, forcedPseudoClasses: ["active"] });
  await page.waitForTimeout(200);
  expect(await thumb.boundingBox()).toEqual(before);
  await expect(thumb.locator(".glass-lens")).toHaveAttribute("data-native-glass", "true");
  await cdp.send("CSS.forcePseudoState", { nodeId, forcedPseudoClasses: [] });
  await cdp.detach();
});

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
