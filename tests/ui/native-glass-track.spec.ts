import { test, expect, type Page } from "@playwright/test";

const transparencyControl = (page: Page) => page.locator(".glass-slider-control:has(> .glass-transparency-slider)");

async function startSlider(page: Page, mode = "light") {
  await page.emulateMedia({ reducedMotion: "reduce" });
  await page.goto(`/tests/ui/harness.html?theme=liquid-glass&mode=${mode}&nativeGlass=true`);
  // These interaction checks isolate transparency geometry. The multi-track
  // cases below keep all settings ranges visible and check them together.
  await page.addStyleTag({ content: ".settings-glass-range { display: none !important; }" });
  await expect(page.locator(".header-title")).toHaveText("TieZ");
  await expect.poll(() => page.evaluate(() => (window as any).testApp.calls.filter((call: any) => call.command === "get_native_glass_status").length)).toBeGreaterThan(0);
  await page.getByTitle("设置", { exact: true }).click();
  await page.getByRole("heading", { name: "界面设置", exact: true }).click();
  const slider = page.getByRole("slider", { name: "液态玻璃透明度", exact: true });
  await slider.scrollIntoViewIfNeeded();
  await expect.poll(async () => Boolean((await latestTrack(page))?.track)).toBe(true);
  await expect(transparencyControl(page).locator(".glass-slider-thumb .glass-lens")).toHaveAttribute("data-native-glass", "false");
  return slider;
}

async function status(page: Page, state: string) {
  await page.evaluate(state => (window as any).testApp.setGlassStatus(state), state);
}

async function latestTrack(page: Page) {
  return page.evaluate(() => (window as any).testApp.calls.filter((call: any) => call.command === "set_glass_geometry").at(-1)?.args.geometry.controls.find((control: any) => control.track));
}

async function trackPaint(page: Page) {
  return transparencyControl(page).locator(":scope > .glass-transparency-slider").evaluate(element => {
    const style = getComputedStyle(element);
    return { image: style.backgroundImage, color: style.backgroundColor, border: style.borderTopColor, shadow: style.boxShadow };
  });
}

test("only native ready takes over the complete slider track, and suspension restores its paint", async ({ page }) => {
  await startSlider(page);
  const fallback = await trackPaint(page);
  expect(fallback.image).toContain("linear-gradient");
  expect(fallback.color).not.toBe("rgba(0, 0, 0, 0)");
  expect(fallback.shadow).toBe("none");
  await expect(transparencyControl(page).locator(".glass-lens-shadow")).not.toHaveCSS("box-shadow", "none");
  await status(page, "ready");
  await expect(page.locator("body")).toHaveClass(/native-glass-ready/);
  await expect.poll(() => trackPaint(page)).toEqual({ image: "none", color: "rgba(0, 0, 0, 0)", border: "rgba(0, 0, 0, 0)", shadow: "none" });
  for (const state of ["resizing", "suspended", "failed"]) {
    await status(page, state);
    await expect.poll(() => trackPaint(page)).toEqual(fallback);
    await status(page, "ready");
    await expect(transparencyControl(page).locator(":scope > .glass-transparency-slider")).toHaveCSS("background-image", "none");
  }
  await page.emulateMedia({ forcedColors: "active" });
  await expect(page.locator("body")).not.toHaveClass(/native-glass-ready/);
  await expect.poll(() => latestTrack(page)).toBeUndefined();
});

for (const mode of ["light", "dark"]) {
  test(`visible native tracks keep independent progress and colors in ${mode}`, async ({ page }) => {
    await page.setViewportSize({ width: 425, height: 1400 });
    await page.emulateMedia({ reducedMotion: "reduce" });
    await page.goto(`/tests/ui/harness.html?theme=liquid-glass&mode=${mode}&nativeGlass=true`);
    await page.getByTitle("设置", { exact: true }).click();
    await page.getByRole("heading", { name: "界面设置", exact: true }).click();
    const inputs = page.locator(".glass-material-slider");
    await expect(inputs).toHaveCount(3);
    const tracks = () => page.evaluate(() => (window as any).testApp.calls
      .filter((call: any) => call.command === "set_glass_geometry").at(-1)?.args.geometry.controls
      .filter((control: any) => control.track).sort((a: any, b: any) => a.track.y - b.track.y));
    await expect.poll(async () => (await tracks())?.length).toBe(3);
    const colors = [[180, 30, 60], [20, 140, 70], [30, 80, 210]];
    const alpha = [.2, .35, .6];
    const values = await inputs.evaluateAll(elements => [
      (elements[0] as HTMLInputElement).min, (elements[1] as HTMLInputElement).max, "25"
    ]);
    for (const [index, value] of values.entries()) {
      await inputs.nth(index).fill(value);
      await inputs.nth(index).evaluate((element, { color, opacity }) => {
        const input = element as HTMLElement;
        input.style.setProperty("--accent-color", `rgb(${color.join(",")})`);
        input.style.setProperty("--glass-track-inactive", `rgba(${color.join(",")},${opacity})`);
      }, { color: colors[index], opacity: alpha[index] });
    }
    const expected = colors.map((color, index) => ({ progress: [0, 1, .25][index],
      activeRgba: [...color.map(value => value / 255), 1], inactiveRgba: [...color.map(value => value / 255), alpha[index]] }));
    const valuesInIpc = async () => (await tracks()).map((control: any) => ({ progress: control.track.progress,
      activeRgba: control.track.activeRgba, inactiveRgba: control.track.inactiveRgba }));
    await expect.poll(valuesInIpc).toEqual(expected);
    for (const input of await inputs.all()) await expect(input).not.toHaveCSS("background-image", "none");
    await status(page, "ready");
    await expect(page.locator(".glass-slider-thumb > .glass-lens[data-native-glass='true']")).toHaveCount(3);
    for (const [index, input] of (await inputs.all()).entries()) {
      await expect(input).toHaveCSS("background-image", "none");
      const bounds = await input.boundingBox();
      const thumb = await input.locator("..").locator(".glass-slider-thumb").boundingBox();
      const control = (await tracks())[index];
      for (const key of ["x", "y", "width", "height"] as const) {
        expect(control.track[key]).toBeCloseTo(bounds![key], 2);
        expect(control[key]).toBeCloseTo(thumb![key], 2);
      }
    }

    // Change only the middle range; the other tracks must retain their own
    // progress and palette through the same controls[] IPC snapshot.
    await inputs.nth(1).fill(await inputs.nth(1).getAttribute("min") as string);
    await inputs.nth(1).evaluate(element => {
      (element as HTMLElement).style.setProperty("--accent-color", "rgb(105,20,210)");
      (element as HTMLElement).style.setProperty("--glass-track-inactive", "rgba(105,20,210,.45)");
    });
    expected[1] = { progress: 0, activeRgba: [105 / 255, 20 / 255, 210 / 255, 1],
      inactiveRgba: [105 / 255, 20 / 255, 210 / 255, .45] };
    await expect.poll(valuesInIpc).toEqual(expected);

    // Even with the global ready state, an opaque CSS-only ancestor removes
    // every native track and restores all of the DOM tracks and thumb lenses.
    await page.locator(".glass-slider-control").evaluateAll(elements => {
      for (const element of elements) (element as HTMLElement).dataset.glassCssOnly = "true";
    });
    await expect.poll(async () => (await tracks()).length).toBe(0);
    await expect(page.locator("body")).toHaveClass(/native-glass-ready/);
    await expect(page.locator(".glass-slider-thumb > .glass-lens[data-native-glass='false']")).toHaveCount(3);
    for (const input of await inputs.all()) {
      await expect(input).not.toHaveCSS("background-image", "none");
      await expect(input.locator("..").locator(".glass-lens-reflection")).not.toHaveCSS("display", "none");
      await expect(input.locator("..").locator(".glass-lens-refraction")).not.toHaveCSS("display", "none");
      await expect(input.locator("..").locator(".glass-lens-shadow")).not.toHaveCSS("box-shadow", "none");
    }
    await page.locator(".glass-slider-control").evaluateAll(elements => {
      for (const element of elements) delete (element as HTMLElement).dataset.glassCssOnly;
    });
    await expect.poll(valuesInIpc).toEqual(expected);
    for (const input of await inputs.all()) await expect(input).toHaveCSS("background-image", "none");
  });
}

for (const mode of ["light", "dark"]) {
  test(`track geometry, keyboard endpoints and persisted values agree with ${mode} CSS`, async ({ page }) => {
    const slider = await startSlider(page, mode);
    const expectedColors = await slider.evaluate(element => {
      const style = getComputedStyle(element);
      const read = (value: string) => {
        const channels = value.match(/[\d.]+/g)!.map(Number);
        return [channels[0] / 255, channels[1] / 255, channels[2] / 255, channels[3] ?? 1];
      };
      return { active: read(style.accentColor), inactive: read(style.backgroundColor) };
    });
    const bounds = await slider.boundingBox();
    await status(page, "ready");
    const thumbX: number[] = [];
    for (const value of [0, 50, 100]) {
      await slider.focus();
      if (value === 0) await slider.press("Home");
      else if (value === 100) await slider.press("End");
      else await slider.fill(String(value));
      await expect(slider).toHaveValue(String(value));
      await expect(slider).toHaveCSS("outline-style", "none");
      await expect(transparencyControl(page).locator(".glass-slider-thumb")).toHaveCSS("outline-style", "solid");
      await expect(transparencyControl(page).locator(".glass-slider-thumb")).toHaveCSS("outline-width", "2px");
      await expect.poll(async () => (await latestTrack(page))?.track.progress).toBe(value / 100);
      const control = await latestTrack(page);
      const thumb = await transparencyControl(page).locator(".glass-slider-thumb").boundingBox();
      expect(control.x).toBeCloseTo(thumb!.x, 2);
      expect(control.width).toBeCloseTo(thumb!.width, 2);
      for (const key of ["x", "y", "width", "height"] as const) expect(control.track[key]).toBeCloseTo(bounds![key], 2);
      expect(control.track.radius).toBeCloseTo(bounds!.height / 2, 2);
      expect(control.track.activeRgba).toEqual(expectedColors.active);
      expect(control.track.inactiveRgba).toEqual(expectedColors.inactive);
      expect(control.track.activeRgba[3]).toBe(1);
      expect(control.track.inactiveRgba[3]).toBe(.1);
      await expect.poll(() => page.evaluate(() => JSON.parse(localStorage.getItem("fixture-settings") || "{}")["app.surface_opacity"])).toBe(String(100 - value));
      thumbX.push(control.x);
    }
    expect(thumbX[0]).toBeLessThan(thumbX[1]);
    expect(thumbX[1]).toBeLessThan(thumbX[2]);
    await slider.click({ position: { x: bounds!.width / 2, y: bounds!.height / 2 } });
    await expect.poll(async () => Number(await slider.inputValue())).toBeGreaterThanOrEqual(49);
    expect(Number(await slider.inputValue())).toBeLessThanOrEqual(51);
    await expect.poll(async () => (await latestTrack(page))?.track.progress).toBe(Number(await slider.inputValue()) / 100);
    await page.emulateMedia({ forcedColors: "active" });
    await slider.focus();
    await slider.press("Home");
    await expect(slider).toHaveCSS("outline-style", "none");
    await expect(transparencyControl(page).locator(".glass-slider-thumb")).toHaveCSS("outline-style", "solid");
    await expect(transparencyControl(page).locator(".glass-slider-thumb")).toHaveCSS("outline-width", "2px");
    await expect(transparencyControl(page).locator(".glass-slider-thumb")).not.toHaveCSS("outline-color", "rgba(0, 0, 0, 0)");
    const ordinaryControl = page.locator(".settings-glass-range").first();
    await ordinaryControl.evaluate(element => (element as HTMLElement).style.setProperty("display", "flex", "important"));
    const ordinaryRange = ordinaryControl.locator('input[type="range"]');
    await ordinaryRange.scrollIntoViewIfNeeded();
    await ordinaryRange.focus();
    await ordinaryRange.press("Home");
    await expect(ordinaryRange).toHaveCSS("outline-style", "none");
    await expect(ordinaryControl.locator(".glass-slider-thumb")).toHaveCSS("outline-style", "solid");
    await expect(ordinaryControl.locator(".glass-slider-thumb")).toHaveCSS("outline-width", "2px");
    await expect(ordinaryControl.locator(".glass-slider-thumb")).not.toHaveCSS("outline-color", "rgba(0, 0, 0, 0)");
  });
}

test("a clipped track returns both track and fully visible thumb to the CSS renderer", async ({ page }) => {
  const slider = await startSlider(page);
  await slider.fill("50");
  await status(page, "ready");
  await expect(slider).toHaveCSS("background-image", "none");
  const clipped = await transparencyControl(page).evaluate(element => {
    const control = element as HTMLElement;
    const parent = control.parentElement!;
    control.style.width = `${control.getBoundingClientRect().width}px`;
    parent.style.width = `${parent.getBoundingClientRect().width * .75}px`;
    parent.style.overflowX = "hidden";
    const bounds = parent.getBoundingClientRect();
    const thumb = control.querySelector(".glass-slider-thumb")!.getBoundingClientRect();
    const track = control.querySelector("input")!.getBoundingClientRect();
    return { thumbInside: thumb.left >= bounds.left && thumb.right <= bounds.right, trackOutside: track.right > bounds.right };
  });
  expect(clipped).toEqual({ thumbInside: true, trackOutside: true });
  await expect(transparencyControl(page).locator(".glass-slider-thumb .glass-lens")).toHaveAttribute("data-native-glass", "false");
  await expect.poll(() => latestTrack(page)).toBeUndefined();
  await expect.poll(async () => (await trackPaint(page)).image).toContain("linear-gradient");
  await expect(transparencyControl(page).locator(".glass-slider-thumb .glass-lens-reflection")).not.toHaveCSS("display", "none");
  await transparencyControl(page).evaluate(element => {
    const control = element as HTMLElement;
    control.style.width = "";
    control.parentElement!.style.width = "";
    control.parentElement!.style.overflowX = "";
  });
  await expect(transparencyControl(page).locator(".glass-slider-thumb .glass-lens")).toHaveAttribute("data-native-glass", "true");
  await expect.poll(async () => Boolean((await latestTrack(page))?.track)).toBe(true);
  await expect(slider).toHaveCSS("background-image", "none");
});

test("track-only color updates cross IPC while moving track and thumb coalesce without idle writes", async ({ page }) => {
  const slider = await startSlider(page);
  await status(page, "ready");
  const initial = await latestTrack(page);
  await slider.evaluate(element => {
    const input = element as HTMLElement;
    input.style.setProperty("--accent-color", "rgb(10, 80, 180)");
    input.style.setProperty("--glass-track-inactive", "rgba(10, 80, 180, .2)");
  });
  await expect.poll(async () => (await latestTrack(page))?.track.activeRgba).toEqual([10 / 255, 80 / 255, 180 / 255, 1]);
  const updated = await latestTrack(page);
  expect(updated.track.inactiveRgba).toEqual([10 / 255, 80 / 255, 180 / 255, .2]);
  expect({ x: updated.x, y: updated.y, width: updated.width, height: updated.height }).toEqual({ x: initial.x, y: initial.y, width: initial.width, height: initial.height });
  const initialCount = await page.evaluate(() => {
    (window as any).testApp.delays.nativeGlassGeometry = 240;
    return (window as any).testApp.calls.filter((call: any) => call.command === "set_glass_geometry").length;
  });
  await transparencyControl(page).evaluate(async element => {
    for (let y = 1; y <= 12; y++) {
      (element as HTMLElement).style.transform = `translateY(${-y}px)`;
      await new Promise<void>(resolve => requestAnimationFrame(() => resolve()));
    }
  });
  const finalTrack = await slider.boundingBox();
  const finalThumb = await transparencyControl(page).locator(".glass-slider-thumb").boundingBox();
  await expect.poll(async () => (await latestTrack(page))?.track.y).toBeCloseTo(finalTrack!.y, 2);
  expect((await latestTrack(page)).y).toBeCloseTo(finalThumb!.y, 2);
  // A queued final rect is still in flight; its confirmed handoff is not an idle write.
  await expect(transparencyControl(page).locator(".glass-slider-thumb .glass-lens")).toHaveAttribute("data-native-glass", "true");
  const stats = await page.evaluate(async () => {
    let writes = 0;
    const observer = new MutationObserver(records => { writes += records.length; });
    observer.observe(document.querySelector(".glass-slider-control:has(> .glass-transparency-slider) > .glass-slider-thumb .glass-lens")!, { attributes: true, attributeFilter: ["data-native-glass"] });
    const calls = () => (window as any).testApp.calls.filter((call: any) => call.command === "set_glass_geometry").length;
    const before = calls();
    for (let frame = 0; frame < 5; frame++) await new Promise<void>(resolve => requestAnimationFrame(() => resolve()));
    observer.disconnect();
    return { before, after: calls(), writes, concurrency: (window as any).testApp.getGlassGeometryConcurrency() };
  });
  expect(stats.concurrency).toBe(1);
  expect(stats.before - initialCount).toBeLessThan(12);
  expect(stats.after).toBe(stats.before);
  expect(stats.writes).toBe(0);
});
