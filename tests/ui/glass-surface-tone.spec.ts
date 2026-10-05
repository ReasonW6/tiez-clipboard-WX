import { expect, test, type Page } from "@playwright/test";
import { readFile, writeFile } from "node:fs/promises";

// The flat portion of the native surface retains normal-incidence reflection.
// Keep the analytic oracle explicit; an optical-model change needs review.
const native = await readFile(new URL("../../src-tauri/native/glass_prefix.hlsli", import.meta.url), "utf8");
if (!native.includes("float f0 = (1.52 - 1.0) / (1.52 + 1.0); f0 *= f0;"))
  throw new Error("Review the native flat-surface reference before changing this oracle");
const f0 = ((1.52 - 1) / (1.52 + 1)) ** 2;

async function sampleCenter(page: Page, selector: string) {
  const bounds = await page.locator(selector).first().boundingBox();
  if (!bounds) throw new Error(`Missing surface: ${selector}`);
  const position = { x: Math.floor(bounds.x + bounds.width / 2), y: Math.floor(bounds.y + bounds.height / 2) };
  const pixel = await page.screenshot({ clip: { ...position, width: 1, height: 1 }, animations: "disabled" });
  const actual = await page.evaluate(async encoded => {
    const image = new Image(); image.src = `data:image/png;base64,${encoded}`; await image.decode();
    const canvas = document.createElement("canvas"); canvas.width = canvas.height = 1;
    const context = canvas.getContext("2d")!; context.drawImage(image, 0, 0);
    return [...context.getImageData(0, 0, 1, 1).data].slice(0, 3);
  }, pixel.toString("base64"));
  return { actual, position };
}

for (const mode of ["light", "dark"]) for (const density of [0, 100]) {
  test(`fallback footer text stays clear of its optical rim: ${mode}, density ${density}`, async ({ page }) => {
    await page.emulateMedia({ reducedMotion: "reduce" });
    await page.goto(`/tests/ui/harness.html?theme=liquid-glass&mode=${mode}&opacity=${density}`);
    await page.locator(".history-item").first().waitFor();
    await page.addStyleTag({ content: `html {background:${mode === "dark" ? "white" : "black"} !important}` });
    await page.getByTitle("设置", { exact: true }).click();
    await expect(page.locator(".settings-footer-meta")).toBeVisible();
    for (const button of await page.locator(".settings-footer-meta button").all()) {
      const label = await button.textContent();
      await expect(button).toHaveCSS("opacity", "1");
      const ink = await button.evaluate(el => getComputedStyle(el).color.match(/[\d.]+/g)!.slice(0, 3).map(Number));
      const front = await button.screenshot({ animations: "disabled" });
      const erased = await page.addStyleTag({ content: ".settings-footer-meta button,.settings-footer-meta button * {-webkit-text-fill-color:transparent !important}" });
      const paper = await button.screenshot({ animations: "disabled" });
      await erased.evaluate(el => el.remove());
      const contrast = await page.evaluate(async ({ front, paper, ink }) => {
        const decode = async (data: string) => {
          const image = new Image(); image.src = `data:image/png;base64,${data}`; await image.decode();
          const canvas = document.createElement("canvas"); canvas.width = image.width; canvas.height = image.height;
          const context = canvas.getContext("2d")!; context.drawImage(image, 0, 0);
          return context.getImageData(0, 0, canvas.width, canvas.height).data;
        };
        const luminance = (rgb: number[]) => rgb.map(v => v / 255).map(v => v <= .04045 ? v / 12.92 : ((v + .055) / 1.055) ** 2.4)
          .reduce((sum, v, i) => sum + v * [.2126, .7152, .0722][i], 0);
        const [a, b] = await Promise.all([decode(front), decode(paper)]);
        let minimum = Infinity, samples = 0;
        const fg = luminance(ink);
        for (let i = 0; i < a.length; i += 4) {
          if (Math.max(...[0, 1, 2].map(c => Math.abs(a[i+c] - b[i+c]))) <= 2) continue;
          const bg = luminance([...b.slice(i, i+3)]);
          minimum = Math.min(minimum, (Math.max(fg, bg)+.05)/(Math.min(fg, bg)+.05));
          samples++;
        }
        return { minimum, samples };
      }, { front: front.toString("base64"), paper: paper.toString("base64"), ink });
      expect(contrast.samples, label || "footer label").toBeGreaterThan(10);
      expect(contrast.minimum, label || "footer label").toBeGreaterThanOrEqual(4.5);
    }
  });
}

for (const mode of ["light", "dark"]) for (const density of [0, 50, 100]) {
  test(`fallback flat surfaces match the native reflection: ${mode}, density ${density}`, async ({ page }, testInfo) => {
    test.setTimeout(120000);
    await page.emulateMedia({ reducedMotion: "reduce" });
    await page.goto(`/tests/ui/harness.html?theme=liquid-glass&mode=${mode}&opacity=${density}&nativeGlass=true`);
    await expect(page.locator(".history-item").first()).toBeVisible();
    await expect.poll(() => page.evaluate(() => (window as any).testApp.getGlassStatus().state)).toBe("pending");
    // Isolate surface colour from ink and the independently tested outer shadows.
    // Neither native status nor the production reflection/filter is overridden.
    await page.addStyleTag({ content: `
      #root,#root * { -webkit-text-fill-color:transparent !important; text-shadow:none !important; }
      #root svg:not(.glass-lens-defs) { visibility:hidden !important; }
      .glass-lens-shadow { box-shadow:none !important; }
    ` });
    const tint = await page.evaluate(() => {
      const style = getComputedStyle(document.documentElement);
      return { rgb: style.getPropertyValue("--material-rgb").split(",").map(Number),
        alpha: Number(style.getPropertyValue("--surface-fill")) };
    });
    const records = [];
    for (const view of ["clipboard", "settings"]) {
      if (view === "settings") {
        await page.getByTitle("设置", { exact: true }).click();
        await expect(page.getByRole("heading", { name: "常规设置", exact: true })).toBeVisible();
      }
      // A settings header lies on one reading plane; ordinary controls have
      // one surface. Uniform input removes kernel/refraction differences.
      const hosts = view === "clipboard" ? [[".liquid-search", 1], [".history-item", 1]] as const
        : [[".group-header", 2]] as const;
      for (const source of [0, 128, 255]) for (const state of ["failed", "resizing"]) {
        const base = tint.rgb.map(value => source * (1 - tint.alpha) + value * tint.alpha);
        const input = state === "resizing" ? base.map(Math.round) : [source, source, source];
        await page.evaluate(async ({ state, input }) => {
          document.documentElement.style.setProperty("background", `rgb(${input.join(",")})`, "important");
          await (window as any).testApp.setGlassStatus(state);
          await new Promise<void>(resolve => requestAnimationFrame(() => requestAnimationFrame(() => resolve())));
        }, { state, input });
        for (const [selector, depth] of hosts) {
          const lens = page.locator(selector).first().locator(":scope > .glass-lens");
          await expect(lens.locator(".glass-lens-reflection")).not.toHaveCSS("display", "none");
          const { actual, position } = await sampleCenter(page, selector);
          const linearExpected = base.map(value => 255 - (255 - value) * (1 - f0) ** depth);
          // Compare two 8-bit outputs, including the native final quantization.
          const expected = linearExpected.map(Math.round);
          const error = Math.max(...actual.map((value, i) => Math.abs(value - expected[i])));
          records.push({ view, source, state, selector, depth, position, actual, linearExpected, expected, error });
        }
      }
    }
    const path = testInfo.outputPath("surface-tone.json");
    await writeFile(path, JSON.stringify({ scope: "Headless CSS raster against the native flat-surface invariant; uniform fictional input, no DWM timing or iOS comparison", records }, null, 2));
    await testInfo.attach("surface-tone", { path, contentType: "application/json" });
    const worst = records.reduce((a, b) => a.error > b.error ? a : b);
    expect(worst.error, JSON.stringify(worst)).toBeLessThanOrEqual(2);
  });
}
