import { expect, test, type Locator, type Page } from "@playwright/test";
import { writeFile } from "node:fs/promises";

// Compare actual transparent browser rasters with and without just one shadow.
// Computed box-shadow alone cannot prove the layer was painted or not clipped.
async function shadowPixels(page: Page, host: Locator, evidencePath?: string) {
  const shadow = host.locator(":scope > .glass-lens > .glass-lens-shadow");
  await expect(shadow).toHaveCount(1);
  const bounds = await host.boundingBox();
  if (!bounds) throw new Error("Missing shadow host");
  const painted = await page.screenshot({ omitBackground: true, animations: "disabled" });
  await shadow.evaluate(element => (element as HTMLElement).style.setProperty("box-shadow", "none", "important"));
  const absent = await page.screenshot({ omitBackground: true, animations: "disabled" });
  await shadow.evaluate(element => (element as HTMLElement).style.removeProperty("box-shadow"));
  if (evidencePath) {
    await writeFile(`${evidencePath}-painted.png`, painted);
    await writeFile(`${evidencePath}-disabled.png`, absent);
  }
  expect(await host.boundingBox(), "Only the shadow paint may change").toEqual(bounds);
  return page.evaluate(async ({ a, b, bounds }) => {
    const decode = async (data: string) => {
      const image = new Image();
      image.src = `data:image/png;base64,${data}`;
      await image.decode();
      const canvas = document.createElement("canvas");
      canvas.width = image.width; canvas.height = image.height;
      const context = canvas.getContext("2d")!;
      context.drawImage(image, 0, 0);
      return { width: canvas.width, height: canvas.height, data: context.getImageData(0, 0, canvas.width, canvas.height).data };
    };
    const [withShadow, withoutShadow] = await Promise.all([decode(a), decode(b)]);
    let changed = 0, alphaMass = 0, peak = 0;
    for (let y = 0; y < withShadow.height; y++) for (let x = 0; x < withShadow.width; x++) {
      // Restrict to the exterior halo, away from the control's content and rim.
      if (x + .5 >= bounds.x && x + .5 <= bounds.x + bounds.width
        && y + .5 >= bounds.y && y + .5 <= bounds.y + bounds.height) continue;
      if (x < bounds.x - 24 || x > bounds.x + bounds.width + 24
        || y < bounds.y - 24 || y > bounds.y + bounds.height + 24) continue;
      const index = (y * withShadow.width + x) * 4 + 3;
      const delta = withShadow.data[index] - withoutShadow.data[index];
      if (delta > 0) { changed++; alphaMass += delta / 255; peak = Math.max(peak, delta); }
    }
    return { changed, alphaMass, peak, bounds };
  }, { a: painted.toString("base64"), b: absent.toString("base64"), bounds });
}

for (const mode of ["light", "dark"]) {
  test(`${mode} control shadows remain painted through native handoffs and viewport changes`, async ({ page }, testInfo) => {
    test.setTimeout(120000);
    await page.emulateMedia({ reducedMotion: "reduce" });
    await page.goto(`/tests/ui/harness.html?theme=liquid-glass&mode=${mode}&opacity=0&nativeGlass=true`);
    await expect(page.locator(".history-item").first()).toBeVisible();
    await expect.poll(() => page.evaluate(() => (window as any).testApp.getGlassStatus().state)).toBe("pending");
    // Keep the transparent WebView paint separate from a nonexistent native
    // image. No desktop pixels or invented native background enter this test.
    await page.addStyleTag({ content: "html,body,#root { background:transparent !important; }" });
    const records = [];
    for (const [width, state, focused] of [
      [425, "pending", false], [425, "ready", true], [280, "resizing", false],
      [280, "ready", false], [280, "suspended", true], [425, "ready", false],
    ] as const) {
      await page.setViewportSize({ width, height: 589 });
      await page.evaluate(state => (window as any).testApp.setGlassStatus(state), state);
      const search = page.locator(".search-input");
      if (focused) await search.focus(); else await search.blur();
      await page.evaluate(async () => { await Promise.allSettled(document.getAnimations().map(animation => animation.finished)); });
      for (const selector of [".header-actions", ".liquid-search", ".history-item"]) {
        const evidencePath = width === 425 && !focused && selector === ".liquid-search"
          ? testInfo.outputPath(`search-${state}`) : undefined;
        const result = await shadowPixels(page, page.locator(selector).first(), evidencePath);
        records.push({ width, state, focused, selector, ...result });
        expect(result.changed, JSON.stringify(records.at(-1))).toBeGreaterThan(40);
        expect(result.alphaMass, JSON.stringify(records.at(-1))).toBeGreaterThan(2);
        expect(result.peak, JSON.stringify(records.at(-1))).toBeGreaterThanOrEqual(2);
      }
    }
    const filename = testInfo.outputPath("shadow-pixels.json");
    await writeFile(filename, JSON.stringify({ scope: "Transparent headless CSS raster; mocked native state, no native/DWM pixel validation", records }, null, 2));
    await testInfo.attach("shadow-pixels", { path: filename, contentType: "application/json" });
  });
}
