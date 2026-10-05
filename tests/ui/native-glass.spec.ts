import { test, expect, type Locator, type Page } from "@playwright/test";

async function start(page: Page, queryDelay = 0, mode = "light") {
  await page.emulateMedia({ reducedMotion: "reduce" });
  await page.goto(`/tests/ui/harness.html?theme=liquid-glass&mode=${mode}&nativeGlass=true&nativeGlassQueryDelay=${queryDelay}`);
  await expect(page.locator(".header-title")).toHaveText("TieZ");
  await expect.poll(() => page.evaluate(() => (window as any).testApp.calls.filter((call: any) => call.command === "get_native_glass_status").length)).toBeGreaterThan(0);
  await expect.poll(async () => (await latestGeometry(page))?.controls.length).toBeGreaterThanOrEqual(2);
  await expect(page.locator(".liquid-search .glass-lens")).toHaveAttribute("data-native-glass", "false");
}

async function setStatus(page: Page, state: string) {
  await page.evaluate(state => (window as any).testApp.setGlassStatus(state), state);
}

async function expectPresentation(page: Page, ready: boolean, backgroundReady = ready) {
  for (const target of [page.locator("html"), page.locator("body")]) {
    if (ready) await expect(target).toHaveClass(/native-glass-ready/);
    else await expect(target).not.toHaveClass(/native-glass-ready/);
    if (backgroundReady) await expect(target).toHaveClass(/native-glass-background-ready/);
    else await expect(target).not.toHaveClass(/native-glass-background-ready/);
  }
}

async function latestGeometry(page: Page) {
  return page.evaluate(() => (window as any).testApp.calls.filter((call: any) => call.command === "set_glass_geometry").at(-1)?.args.geometry);
}

async function outerShadows(host: Locator) {
  return host.evaluate(element => {
    const lens = element.querySelector(":scope > .glass-lens")!;
    const layers = [element, lens, ...lens.querySelectorAll("*")];
    return layers.flatMap(layer => {
      const style = getComputedStyle(layer);
      const bounds = layer.getBoundingClientRect();
      if (!bounds.width || !bounds.height) return [];
      for (let ancestor: Element | null = layer; ancestor; ancestor = ancestor.parentElement) {
        const ancestorStyle = getComputedStyle(ancestor);
        if (ancestorStyle.display === "none" || ancestorStyle.visibility !== "visible" || Number(ancestorStyle.opacity) === 0) return [];
      }
      // Split shadow lists, preserving commas inside rgb()/rgba(). Count paint
      // from every lens layer so moving a second shadow onto the rim is caught.
      return style.boxShadow.split(/,(?![^(]*\))/).map(shadow => shadow.trim())
        .filter(shadow => shadow !== "none" && !shadow.includes("inset") && !/rgba\([^)]*,\s*0\)/.test(shadow));
    });
  });
}

for (const mode of ["light", "dark"]) {
  test(`control outer shadows survive native transitions, resize and search focus in ${mode}`, async ({ page }) => {
    await start(page, 0, mode);
    const hosts = [page.locator(".header-actions"), page.locator(".liquid-search"), page.locator(".history-item").first()];
    const search = page.locator(".search-input");
    const baseline = [];
    for (const host of hosts) {
      await expect(host).toBeVisible();
      await expect.poll(() => outerShadows(host)).toHaveLength(1);
      baseline.push(await outerShadows(host));
    }

    for (const width of [425, 280, 425]) {
      await page.setViewportSize({ width, height: 589 });
      for (const state of ["pending", "ready", "resizing", "ready", "suspended", "ready", "failed"]) {
        await setStatus(page, state);
        const backgroundReady = state === "ready" || state === "resizing";
        await expectPresentation(page, state === "ready", backgroundReady);
        if (backgroundReady) await expect(page.locator("#root")).toHaveCSS("background-color", "rgba(0, 0, 0, 0)");
        else await expect(page.locator("#root")).not.toHaveCSS("background-color", "rgba(0, 0, 0, 0)");
        for (const layer of ["refraction", "reflection"]) {
          const lens = hosts[1].locator(`.glass-lens-${layer}`);
          if (state === "ready") await expect(lens).toHaveCSS("display", "none");
          else await expect(lens).not.toHaveCSS("display", "none");
        }
        for (const [index, host] of hosts.entries()) {
          await expect(host).toBeVisible();
          await expect.poll(() => outerShadows(host), { message: `${mode} ${width}px ${state}: one unchanged outer shadow` }).toEqual(baseline[index]);
        }
        await search.focus();
        await expect(hosts[1]).toHaveCSS("outline-style", "solid");
        await expect(hosts[1]).toHaveCSS("outline-width", "2px");
        await expect(hosts[1]).not.toHaveCSS("outline-color", "rgba(0, 0, 0, 0)");
        expect(await outerShadows(hosts[1]), "focus must not replace the physical shadow with a focus ring").toEqual(baseline[1]);
        await search.blur();
        await expect(hosts[1]).toHaveCSS("outline-style", "none");
        expect(await outerShadows(hosts[1])).toEqual(baseline[1]);
      }
    }

    await setStatus(page, "ready");
    await expect(page.locator("body")).toHaveClass(/native-glass-ready/);
    await page.emulateMedia({ forcedColors: "active" });
    await expectPresentation(page, false);
    for (const host of hosts) await expect.poll(() => outerShadows(host)).toEqual([]);
    await search.focus();
    await expect(hosts[1]).toHaveCSS("outline-style", "solid");
    await page.emulateMedia({ forcedColors: "none" });
    await expect(page.locator("body")).toHaveClass(/native-glass-ready/);
    for (const [index, host] of hosts.entries()) await expect.poll(() => outerShadows(host)).toEqual(baseline[index]);
  });
}

test("newer native status survives a late initial query and older events", async ({ page }) => {
  await start(page, 1200);
  await setStatus(page, "ready");
  await expectPresentation(page, false);
  await setStatus(page, "resizing");
  await expectPresentation(page, false, true);
  for (const state of ["failed", "ready"]) {
    await page.evaluate(state => (window as any).testApp.emit("native-glass-status", { state, revision: 0, geometryRevision: 0 }), state);
    await expectPresentation(page, false, true);
  }
  const staleResize = await page.evaluate(() => (window as any).testApp.getGlassStatus());
  await setStatus(page, "ready");
  await expectPresentation(page, true);
  await page.evaluate(status => (window as any).testApp.emit("native-glass-status", status), staleResize);
  await expectPresentation(page, true);
  await setStatus(page, "suspended");
  await expectPresentation(page, false);
});

test("resize retains the background and CSS lenses while viewport geometry catches up", async ({ page }) => {
  await start(page);
  await setStatus(page, "ready");
  await expectPresentation(page, true);
  const search = page.locator(".liquid-search");
  const shadow = await outerShadows(search);
  expect(shadow).toHaveLength(1);
  await page.evaluate(() => { (window as any).testApp.delays.nativeGlassGeometry = 160; });

  for (const size of [{ width: 520, height: 650 }, { width: 280, height: 520 }, { width: 425, height: 589 }]) {
    await setStatus(page, "resizing");
    await page.setViewportSize(size);
    await expectPresentation(page, false, true);
    await expect(page.locator("#root")).toHaveCSS("background-color", "rgba(0, 0, 0, 0)");
    await expect(page.locator(".native-glass-resize-handles")).toHaveCSS("display", "block");
    for (const layer of ["refraction", "reflection"]) await expect(search.locator(`.glass-lens-${layer}`)).not.toHaveCSS("display", "none");
    expect(await outerShadows(search)).toEqual(shadow);

    // Viewport resize and IPC arrive independently. Wait for the new geometry
    // before simulating the native renderer's matching-frame acknowledgement.
    const bounds = await search.boundingBox();
    await expect.poll(async () => {
      const geometry = await latestGeometry(page);
      return geometry.viewportWidth === size.width && geometry.viewportHeight === size.height
        && geometry.controls.some((control: any) => Math.abs(control.x - bounds!.x) < .01
          && Math.abs(control.y - bounds!.y) < .01 && Math.abs(control.width - bounds!.width) < .01
          && Math.abs(control.height - bounds!.height) < .01);
    }).toBe(true);
    await setStatus(page, "ready");
    await expectPresentation(page, true);
    await expect(search.locator(".glass-lens-refraction")).toHaveCSS("display", "none");
    await expect(search.locator(".glass-lens-reflection")).toHaveCSS("display", "none");
    expect(await outerShadows(search)).toEqual(shadow);
  }
});

test("pending submits geometry while retaining the readable CSS fallback", async ({ page }) => {
  await start(page);
  await expect.poll(async () => (await latestGeometry(page))?.controls.length).toBeGreaterThanOrEqual(2);
  await expect(page.locator("body")).not.toHaveClass(/native-glass-ready/);
  const rootAlpha = () => page.locator("#root").evaluate(element => getComputedStyle(element).backgroundColor);
  expect(await rootAlpha()).not.toBe("rgba(0, 0, 0, 0)");
  await setStatus(page, "ready");
  await expect(page.locator("body")).toHaveClass(/native-glass-ready/);
  await expect.poll(rootAlpha).toBe("rgba(0, 0, 0, 0)");
  await expect(page.locator(".liquid-search .glass-lens-refraction")).toHaveCSS("display", "none");
  await expect(page.locator(".liquid-search .glass-lens-reflection")).toHaveCSS("display", "none");
  await page.locator(".search-input").focus();
  await expect(page.locator(".liquid-search")).toHaveCSS("outline-style", "solid");
  await setStatus(page, "failed");
  await expectPresentation(page, false);
  expect(await rootAlpha()).not.toBe("rgba(0, 0, 0, 0)");
  await expect(page.locator(".liquid-search .glass-lens-reflection")).not.toHaveCSS("display", "none");
});

test("native readiness clears only the shell and preserves the window tint token for icons and popups", async ({ page }) => {
  await start(page);
  const readColors = () => page.evaluate(() => {
    const fixture = document.createElement("div");
    fixture.style.position = "fixed";
    fixture.style.left = "-10000px";
    const icon = document.createElement("div");
    icon.className = "video-icon-wrapper";
    const popup = document.createElement("div");
    popup.className = "modal-content";
    fixture.append(icon, popup);
    document.body.append(fixture);
    const colors = { icon: getComputedStyle(icon).color, popup: getComputedStyle(popup).backgroundColor };
    fixture.remove();
    return colors;
  });
  const baseline = await readColors();
  expect(baseline.icon).not.toBe("rgba(0, 0, 0, 0)");
  expect(baseline.popup).not.toBe("rgba(0, 0, 0, 0)");
  await setStatus(page, "ready");
  await expect(page.locator("body")).toHaveClass(/native-glass-ready/);
  await expect(page.locator("#root")).toHaveCSS("background-color", "rgba(0, 0, 0, 0)");
  expect(await readColors()).toEqual(baseline);
});

test("moving geometry coalesces slow IPC and eventually matches the latest DOM rect", async ({ page }) => {
  await start(page);
  const originalCount = await page.evaluate(() => {
    const fixture = (window as any).testApp;
    fixture.delays.nativeGlassGeometry = 240;
    return fixture.calls.filter((call: any) => call.command === "set_glass_geometry").length;
  });
  await page.locator(".header-actions").evaluate(async element => {
    for (let x = 1; x <= 12; x++) {
      (element as HTMLElement).style.transform = `translateX(${-x}px)`;
      await new Promise<void>(resolve => requestAnimationFrame(() => resolve()));
    }
  });
  const box = await page.locator(".header-actions").boundingBox();
  await expect.poll(async () => (await latestGeometry(page))?.controls.some((control: any) => Math.abs(control.x - box!.x) < .01 && Math.abs(control.width - box!.width) < .01)).toBe(true);
  const stats = await page.evaluate(() => {
    const fixture = (window as any).testApp;
    return { count: fixture.calls.filter((call: any) => call.command === "set_glass_geometry").length, concurrency: fixture.getGlassGeometryConcurrency() };
  });
  expect(stats.concurrency).toBe(1);
  expect(stats.count - originalCount).toBeLessThan(12);
  const stationaryCount = stats.count;
  const idleWrites = await page.evaluate(async () => {
    let writes = 0;
    const observer = new MutationObserver(records => { writes += records.length; });
    for (const lens of document.querySelectorAll(".glass-lens")) observer.observe(lens, { attributes: true, attributeFilter: ["data-native-glass"] });
    for (let i = 0; i < 5; i++) await new Promise<void>(resolve => requestAnimationFrame(() => resolve()));
    observer.disconnect();
    return writes;
  });
  expect(idleWrites).toBe(0);
  expect(await page.evaluate(() => (window as any).testApp.calls.filter((call: any) => call.command === "set_glass_geometry").length)).toBe(stationaryCount);
});

test("ancestor clipping and fading retain the CSS lens and remove native geometry", async ({ page }) => {
  await start(page);
  await setStatus(page, "ready");
  await expect(page.locator("body")).toHaveClass(/native-glass-ready/);
  await page.locator(".liquid-search").evaluate(element => {
    const parent = element.parentElement!;
    parent.style.height = "2px";
    parent.style.setProperty("overflow", "hidden", "important");
  });
  await expect(page.locator(".liquid-search .glass-lens")).toHaveAttribute("data-native-glass", "false");
  // This ancestor also contains category lenses. Clipping the whole row can
  // reveal more virtualized cards, so the total shape count is not invariant.
  await expect(page.locator(".liquid-search").locator("..").locator('.glass-lens[data-native-glass="true"]')).toHaveCount(0);
  const clippedSearch = await page.locator(".liquid-search").boundingBox();
  await expect.poll(async () => (await latestGeometry(page)).controls.some((control: any) =>
    Math.abs(control.x - clippedSearch!.x) < .01 && Math.abs(control.y - clippedSearch!.y) < .01
    && Math.abs(control.width - clippedSearch!.width) < .01 && Math.abs(control.height - clippedSearch!.height) < .01)).toBe(false);
  await expect(page.locator(".liquid-search .glass-lens-reflection")).not.toHaveCSS("display", "none");
  await page.locator(".liquid-search").evaluate(element => {
    const parent = element.parentElement!;
    parent.style.height = "";
    parent.style.overflow = "";
    parent.style.opacity = "0";
  });
  await expect(page.locator(".liquid-search .glass-lens")).toHaveAttribute("data-native-glass", "false");
  await page.locator(".liquid-search").evaluate(element => { element.parentElement!.style.opacity = "1"; });
  await expect(page.locator(".liquid-search .glass-lens")).toHaveAttribute("data-native-glass", "true");
  const restoredSearch = await page.locator(".liquid-search").boundingBox();
  await expect.poll(async () => (await latestGeometry(page)).controls.some((control: any) =>
    Math.abs(control.x - restoredSearch!.x) < .01 && Math.abs(control.y - restoredSearch!.y) < .01
    && Math.abs(control.width - restoredSearch!.width) < .01 && Math.abs(control.height - restoredSearch!.height) < .01)).toBe(true);
});

test("forced colors restores the CSS surface and clears optical controls during resize", async ({ page }) => {
  await start(page);
  await setStatus(page, "ready");
  await expectPresentation(page, true);
  await setStatus(page, "resizing");
  await expectPresentation(page, false, true);
  await page.emulateMedia({ forcedColors: "active" });
  expect(await page.evaluate(() => matchMedia("(forced-colors: active)").matches)).toBe(true);
  await expectPresentation(page, false);
  await expect.poll(async () => (await latestGeometry(page)).controls.length).toBe(0);
  await page.emulateMedia({ forcedColors: "none" });
  await expectPresentation(page, false, true);
  await expect.poll(async () => (await latestGeometry(page)).controls.length).toBeGreaterThanOrEqual(2);
});

test("reduced transparency clears both native presentation states", async ({ page }) => {
  await start(page);
  const cdp = await page.context().newCDPSession(page);
  for (const state of ["ready", "resizing"]) {
    await setStatus(page, state);
    await expectPresentation(page, state === "ready", true);
    await cdp.send("Emulation.setEmulatedMedia", { features: [
      { name: "prefers-reduced-transparency", value: "reduce" }, { name: "prefers-reduced-motion", value: "reduce" }
    ] });
    expect(await page.evaluate(() => matchMedia("(prefers-reduced-transparency: reduce)").matches)).toBe(true);
    await expectPresentation(page, false);
    await expect(page.locator("#root")).not.toHaveCSS("background-color", "rgba(0, 0, 0, 0)");
    await expect.poll(async () => (await latestGeometry(page)).controls.length).toBe(0);
    await cdp.send("Emulation.setEmulatedMedia", { features: [
      { name: "prefers-reduced-transparency", value: "no-preference" }, { name: "prefers-reduced-motion", value: "reduce" }
    ] });
    await expectPresentation(page, state === "ready", true);
  }
  await cdp.detach();
});

test("theme teardown clears readiness and geometry despite a late ready query", async ({ page }) => {
  await start(page, 1200);
  await setStatus(page, "ready");
  await page.getByTitle("设置", { exact: true }).click();
  await page.getByRole("heading", { name: "界面设置", exact: true }).click();
  await page.getByRole("button", { name: "云母", exact: true }).click();
  await expect(page.locator("body")).toHaveClass(/theme-mica/);
  await expectPresentation(page, false);
  await expect.poll(async () => (await latestGeometry(page)).controls.length).toBe(0);
  for (const state of ["ready", "resizing"]) {
    await page.evaluate(state => (window as any).testApp.emit("native-glass-status", { state, revision: 100, geometryRevision: 0 }), state);
    await expectPresentation(page, false);
  }
  await expect.poll(() => page.evaluate(() => (window as any).testApp.getGlassStatus().state)).toBe("inactive");
  await expectPresentation(page, false);
});

test("theme teardown removes an already visible retained background", async ({ page }) => {
  await start(page);
  await setStatus(page, "ready");
  await expectPresentation(page, true);
  await setStatus(page, "resizing");
  await expectPresentation(page, false, true);
  await page.getByTitle("设置", { exact: true }).click();
  await page.getByRole("heading", { name: "界面设置", exact: true }).click();
  await page.getByRole("button", { name: "云母", exact: true }).click();
  await expect(page.locator("body")).toHaveClass(/theme-mica/);
  await expectPresentation(page, false);
  await expect.poll(async () => (await latestGeometry(page)).controls.length).toBe(0);
});

test("pressures ease in and out, while reduced motion keeps the optical shape still", async ({ page }) => {
  await start(page);
  await page.emulateMedia({ reducedMotion: "no-preference" });
  await page.locator(".header-actions").dispatchEvent("pointerdown", { button: 0 });
  await expect.poll(async () => Math.max(...(await latestGeometry(page)).controls.map((control: any) => control.pressure))).toBeGreaterThan(.75);
  const samples = await page.evaluate(() => (window as any).testApp.calls.filter((call: any) => call.command === "set_glass_geometry").flatMap((call: any) => call.args.geometry.controls.map((control: any) => control.pressure)));
  expect(samples.some((pressure: number) => pressure > 0 && pressure < .75)).toBe(true);
  await page.evaluate(() => window.dispatchEvent(new PointerEvent("pointerup")));
  await expect.poll(async () => Math.max(...(await latestGeometry(page)).controls.map((control: any) => control.pressure))).toBe(0);
  await page.emulateMedia({ reducedMotion: "reduce" });
  await page.locator(".header-actions").dispatchEvent("pointerdown", { button: 0 });
  await page.evaluate(async () => { for (let i = 0; i < 5; i++) await new Promise<void>(resolve => requestAnimationFrame(() => resolve())); });
  expect((await latestGeometry(page)).controls.every((control: any) => control.pressure === 0)).toBe(true);
  await page.evaluate(() => window.dispatchEvent(new PointerEvent("pointercancel")));
});

test("another window's newer native status cannot override this window", async ({ page }) => {
  await start(page, 1200);
  await page.evaluate(() => (window as any).testApp.emitTo("advanced-settings", "native-glass-status", { state: "ready", revision: 100, geometryRevision: 0 }));
  await expect(page.locator("body")).not.toHaveClass(/native-glass-ready/);
  await setStatus(page, "ready");
  await expect(page.locator("body")).toHaveClass(/native-glass-ready/);
  await page.evaluate(() => (window as any).testApp.emitTo("advanced-settings", "native-glass-status", { state: "suspended", revision: 101, geometryRevision: 0 }));
  await expect(page.locator("body")).toHaveClass(/native-glass-ready/);
  await setStatus(page, "suspended");
  await expect(page.locator("body")).not.toHaveClass(/native-glass-ready/);
});
