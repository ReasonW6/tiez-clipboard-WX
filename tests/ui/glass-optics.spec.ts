import { test, expect, type Page } from "@playwright/test";
import { writeFile } from "node:fs/promises";

async function start(page: Page) {
  await page.emulateMedia({ reducedMotion: "reduce" });
  await page.goto("/tests/ui/harness.html?theme=liquid-glass&mode=light&opacity=0");
  await expect(page.locator('.liquid-search feImage[result="lens"]')).toHaveAttribute("href", /^data:image\/png/);
  await page.evaluate(() => new Promise<void>(resolve => requestAnimationFrame(() => requestAnimationFrame(() => resolve()))));
}

test("the optical color pass preserves a uniform source color and its single alpha", async ({ page }) => {
  await start(page);
  const samples = await page.locator(".liquid-search .glass-lens").evaluate(async element => {
    const width = element.clientWidth, height = element.clientHeight;
    const filter = element.querySelector("filter")!.cloneNode(true) as SVGFilterElement;
    // Rasterize the production color pass before its independent coverage pass.
    filter.querySelector('feComposite[in2="coverage"]')?.remove();
    filter.querySelector('[result="coverage"]')?.remove();
    filter.id = "probe";
    const pixels: number[][] = [];
    for (const alpha of [.25, .5, .8, 1]) {
      const svg = `<svg xmlns="http://www.w3.org/2000/svg" width="${width}" height="${height}"><defs>${filter.outerHTML}</defs><rect width="${width}" height="${height}" fill="rgb(90,150,210)" fill-opacity="${alpha}" filter="url(#probe)"/></svg>`;
      const image = new Image();
      image.src = `data:image/svg+xml;charset=utf-8,${encodeURIComponent(svg)}`;
      await image.decode();
      const canvas = document.createElement("canvas");
      canvas.width = width; canvas.height = height;
      const context = canvas.getContext("2d")!;
      context.drawImage(image, 0, 0);
      pixels.push([...context.getImageData(Math.floor(width / 2), Math.floor(height / 2), 1, 1).data]);
    }
    return pixels;
  });
  for (const [index, alpha] of [.25, .5, .8, 1].entries()) {
    for (const [channel, expected] of [90, 150, 210, Math.round(alpha * 255)].entries()) {
      expect(Math.abs(samples[index][channel] - expected), `alpha ${alpha}, channel ${channel}: ${samples[index]}`).toBeLessThanOrEqual(2);
    }
  }
});

test("the rim refracts background detail while its flat center stays unchanged", async ({ page }) => {
  await start(page);
  const difference = await page.locator(".liquid-search .glass-lens").evaluate(async element => {
    const width = element.clientWidth, height = element.clientHeight;
    const filter = element.querySelector("filter")!.cloneNode(true) as SVGFilterElement;
    filter.querySelector('feComposite[in2="coverage"]')?.remove();
    filter.querySelector('[result="coverage"]')?.remove();
    filter.id = "probe";
    const pixels: Uint8ClampedArray[] = [];
    for (const filtered of [false, true]) {
      const svg = `<svg xmlns="http://www.w3.org/2000/svg" width="${width}" height="${height}"><defs>${filter.outerHTML}<pattern id="stripes" width="4" height="4" patternUnits="userSpaceOnUse"><rect width="4" height="4" fill="white"/><rect width="2" height="4" fill="black"/></pattern></defs><rect width="${width}" height="${height}" fill="url(#stripes)" ${filtered ? 'filter="url(#probe)"' : ""}/></svg>`;
      const image = new Image(); image.src = `data:image/svg+xml;charset=utf-8,${encodeURIComponent(svg)}`; await image.decode();
      const canvas = document.createElement("canvas"); canvas.width = width; canvas.height = height;
      const context = canvas.getContext("2d")!; context.drawImage(image, 0, 0);
      pixels.push(context.getImageData(0, 0, width, height).data);
    }
    let center = 0, rim = 0;
    const y = Math.floor(height / 2);
    for (let x = 2; x < width - 2; x++) {
      const index = (y * width + x) * 4;
      const difference = Math.abs(pixels[0][index] - pixels[1][index]);
      if (x > width / 3 && x < width * 2 / 3) center = Math.max(center, difference);
      if (x > width - 9) rim = Math.max(rim, difference);
    }
    let chroma = 0;
    for (let i = 0; i < pixels[1].length; i += 4) chroma = Math.max(chroma,
      Math.max(...pixels[1].slice(i, i + 3)) - Math.min(...pixels[1].slice(i, i + 3)));
    return { center, rim, chroma };
  });
  expect(difference.center).toBeLessThanOrEqual(2);
  expect(difference.rim).toBeGreaterThan(20);
  expect(difference.chroma, "The production SVG color pass must separate a neutral detailed source").toBeGreaterThan(32);
});

test("the flat center does not composite a transparent window background twice", async ({ page }) => {
  await start(page);
  await page.addStyleTag({ content: `
    html, body { background: transparent !important; }
    #root { background: rgba(90,150,210,.25) !important; }
    .liquid-search .search-input, .liquid-search > .search-icon { visibility: hidden; }
    .glass-lens-reflection { background: none !important; box-shadow: none !important; }
  ` });
  const samples: number[][] = [];
  for (const filtered of [true, false]) {
    if (!filtered) await page.addStyleTag({ content: ".glass-lens-refraction { backdrop-filter: none !important; }" });
    const image = await page.locator(".liquid-search").screenshot({ omitBackground: true, scale: "css" });
    samples.push(await page.evaluate(async value => {
      const image = new Image(); image.src = `data:image/png;base64,${value}`; await image.decode();
      const canvas = document.createElement("canvas"); canvas.width = image.width; canvas.height = image.height;
      const context = canvas.getContext("2d")!; context.drawImage(image, 0, 0);
      return [...context.getImageData(Math.floor(image.width / 2), Math.floor(image.height / 2), 1, 1).data];
    }, image.toString("base64")));
  }
  expect(samples[1][3]).toBe(64);
  expect(samples[0], "The transparent WebView's flat center must keep the same RGBA when refraction is enabled").toEqual(samples[1]);
});

test("the visible optical rim still refracts the page background", async ({ page }, testInfo) => {
  await start(page);
  await page.addStyleTag({ content: `
    #root { background: repeating-linear-gradient(90deg,#101010 0 2px,#f0f0f0 2px 4px) !important; }
    .liquid-search .search-input, .liquid-search > .search-icon { visibility: hidden; }
    .glass-lens-reflection { background: none !important; box-shadow: none !important; }
  ` });
  const bounds = await page.locator(".liquid-search").evaluate(element => {
    const host = element.getBoundingClientRect();
    const lens = element.querySelector(".glass-lens")!.getBoundingClientRect();
    return { x: lens.x - host.x, y: lens.y - host.y, width: lens.width, height: lens.height,
      radius: parseFloat(getComputedStyle(element.querySelector(".glass-lens")!).borderTopLeftRadius) };
  });
  // A single six-pixel row can hit equal-colored source stripes for every RGB
  // sample. Cover the whole optical shoulder and all four stripe phases; use
  // colored area as well as a peak, since subpixel sampling can make one old
  // subpixel edge bright without producing substantial dispersion.
  const frames: { variant: string; phase: number; data: string }[] = [];
  for (const variant of ["current", "old-dispersion", "unfiltered"]) {
    if (variant === "old-dispersion") await page.locator(".liquid-search .glass-lens").evaluate(element => {
      element.querySelector('feDisplacementMap[result="r"]')!.setAttribute("scale", "12");
      element.querySelector('feDisplacementMap[result="b"]')!.setAttribute("scale", "13");
    });
    if (variant === "unfiltered") await page.addStyleTag({ content: ".glass-lens-refraction { backdrop-filter: none !important; }" });
    for (const phase of [0, 1, 2, 3]) {
      await page.locator("#root").evaluate((element, value) => { element.style.setProperty("background-position", `${value}px 0px`, "important"); }, phase);
      await page.evaluate(() => new Promise<void>(resolve => requestAnimationFrame(() => requestAnimationFrame(() => resolve()))));
      const path = testInfo.outputPath(`${variant}-phase-${phase}.png`);
      const image = await page.locator(".liquid-search").screenshot({ scale: "css", path });
      await testInfo.attach(`${variant}-phase-${phase}`, { path, contentType: "image/png" });
      frames.push({ variant, phase, data: image.toString("base64") });
    }
  }
  const metrics = await page.evaluate(async ({ frames, bounds }) => {
    const decoded = await Promise.all(frames.map(async frame => {
      const image = new Image(); image.src = `data:image/png;base64,${frame.data}`; await image.decode();
      const canvas = document.createElement("canvas"); canvas.width = image.width; canvas.height = image.height;
      const context = canvas.getContext("2d")!; context.drawImage(image, 0, 0);
      return { ...frame, width: image.width, height: image.height,
        pixels: context.getImageData(0, 0, image.width, image.height).data };
    }));
    return decoded.map(frame => {
      const reference = decoded.find(value => value.variant === "unfiltered" && value.phase === frame.phase)!;
      const firstPhase = decoded.find(value => value.variant === "unfiltered" && value.phase === 0)!;
      let rimDifference = 0, centerDifference = 0, peakChroma = 0, visibleChromaPixels = 0, sampled = 0, backgroundPhaseDifference = 0;
      for (let y = 0; y < frame.height; y++) for (let x = 0; x < frame.width; x++) {
        const px = x + .5 - bounds.x, py = y + .5 - bounds.y;
        const qx = Math.abs(px - bounds.width / 2) - (bounds.width / 2 - bounds.radius);
        const qy = Math.abs(py - bounds.height / 2) - (bounds.height / 2 - bounds.radius);
        const depth = bounds.radius - Math.hypot(Math.max(qx, 0), Math.max(qy, 0)) - Math.min(Math.max(qx, qy), 0);
        const i = (y * frame.width + x) * 4;
        const difference = Math.max(...[0, 1, 2].map(c => Math.abs(frame.pixels[i + c] - reference.pixels[i + c])));
        if (depth >= 1 && depth <= 5.5) {
          const rgb = frame.pixels.slice(i, i + 3);
          const chroma = Math.max(...rgb) - Math.min(...rgb);
          peakChroma = Math.max(peakChroma, chroma);
          if (chroma > 16) visibleChromaPixels++;
          rimDifference = Math.max(rimDifference, difference);
          sampled++;
        }
        if (depth >= 10) {
          centerDifference = Math.max(centerDifference, difference);
          backgroundPhaseDifference = Math.max(backgroundPhaseDifference, Math.abs(reference.pixels[i] - firstPhase.pixels[i]));
        }
      }
      return { variant: frame.variant, phase: frame.phase, rimDifference, centerDifference, peakChroma, visibleChromaPixels, sampled, backgroundPhaseDifference };
    });
  }, { frames, bounds });
  const metricsPath = testInfo.outputPath("dispersion-metrics.json");
  await writeFile(metricsPath, JSON.stringify({ bounds, metrics }, null, 2));
  await testInfo.attach("dispersion-metrics", { path: metricsPath, contentType: "application/json" });
  for (const current of metrics.filter(value => value.variant === "current")) {
    const previous = metrics.find(value => value.variant === "old-dispersion" && value.phase === current.phase)!;
    expect(current.rimDifference, "The coverage mask must retain visible refraction of actual background detail at the rim").toBeGreaterThan(10);
    expect(current.peakChroma, "Monochrome background detail must visibly separate into RGB at the curved shoulder").toBeGreaterThan(16);
    expect(current.visibleChromaPixels, "Visible RGB separation must cover at least 64 shoulder pixels, not one isolated sample").toBeGreaterThanOrEqual(64);
    expect(previous.visibleChromaPixels, "The previous coefficient must fail the same 64-pixel dispersion visibility gate").toBeLessThan(64);
    expect(current.visibleChromaPixels, "The stronger coefficient must expand visible dispersion beyond the old sparse fringes").toBeGreaterThan(previous.visibleChromaPixels * 4);
    expect(current.centerDifference, "The flat center must preserve the page background").toBeLessThanOrEqual(2);
    expect(current.sampled).toBeGreaterThan(3000);
  }
  for (const reference of metrics.filter(value => value.variant === "unfiltered")) {
    expect(reference.peakChroma, "The unfiltered grayscale source must remain neutral").toBeLessThanOrEqual(2);
    if (reference.phase > 0)
      expect(reference.backgroundPhaseDifference, "Each phase must actually move the source stripes under the control").toBeGreaterThan(100);
  }
});

for (const selector of [".liquid-search", ".header-actions"]) test(`${selector}: refraction cannot bend its own border into a second interior arc`, async ({ page }) => {
  await start(page);
  await page.addStyleTag({ content: `
    html, body, #root { background: rgb(116, 120, 125) !important; }
    .liquid-search .search-input, .liquid-search > .search-icon, .header-actions > button { visibility: hidden; }
  ` });
  const control = page.locator(selector);
  const filtered = await control.screenshot({ scale: "css" });
  await page.addStyleTag({ content: `${selector} .glass-lens-refraction { backdrop-filter: none !important; }` });
  const unfiltered = await control.screenshot({ scale: "css" });
  const difference = await page.evaluate(async ({ before, after }) => {
    const decode = async (value: string) => {
      const image = new Image(); image.src = `data:image/png;base64,${value}`; await image.decode();
      const canvas = document.createElement("canvas"); canvas.width = image.width; canvas.height = image.height;
      const context = canvas.getContext("2d")!; context.drawImage(image, 0, 0);
      return { width: canvas.width, height: canvas.height, data: context.getImageData(0, 0, canvas.width, canvas.height).data };
    };
    const a = await decode(before), b = await decode(after);
    const radius = Math.min(20, a.height / 2);
    let maximum = 0, compared = 0;
    for (let y = 0; y < a.height; y++) for (let x = a.width - 2 * radius; x < a.width; x++) {
      const qx = Math.abs(x + .5 - a.width / 2) - (a.width / 2 - radius);
      const qy = Math.abs(y + .5 - a.height / 2) - (a.height / 2 - radius);
      const distance = Math.hypot(Math.max(qx, 0), Math.max(qy, 0)) + Math.min(Math.max(qx, qy), 0) - radius;
      if (distance > -3.5) continue;
      const index = (y * a.width + x) * 4;
      for (let channel = 0; channel < 3; channel++) maximum = Math.max(maximum, Math.abs(a.data[index + channel] - b.data[index + channel]));
      compared++;
    }
    return { maximum, compared };
  }, { before: filtered.toString("base64"), after: unfiltered.toString("base64") });
  expect(difference.compared).toBeGreaterThan(300);
  expect(difference.maximum, "A uniform backdrop must not acquire a second arc from the control's own decoration").toBeLessThanOrEqual(2);
});
