import { test, expect } from "@playwright/test";

test("fallback map evaluates the boundary only and keeps every displacement byte", async ({ page }) => {
  await page.goto("/tests/ui/harness.html?theme=acrylic");
  const result = await page.evaluate(async () => {
    // @ts-expect-error Vite serves the source module in this isolated fixture.
    const { makeLensMap } = await import("/src/shared/components/GlassLens.tsx");
    const original = Math.hypot;
    let calls = 0;
    Math.hypot = (...values: number[]) => { ++calls; return original(...values); };
    let error = 0, evaluatedLargeMap = 0;
    try {
      for (const [width, height, radius] of [[640, 896, 16], [176.3, 49.7, 24.85], [31, 22, 11], [17.2, 14.6, 0]]) {
        calls = 0;
        const image = new Image(); image.src = makeLensMap(width, height, radius); await image.decode();
        if (width === 640) evaluatedLargeMap = calls;
        const canvas = document.createElement("canvas"); canvas.width = Math.ceil(width); canvas.height = Math.ceil(height);
        const context = canvas.getContext("2d")!; context.drawImage(image, 0, 0);
        const pixels = context.getImageData(0, 0, canvas.width, canvas.height).data;
        const r = Math.min(radius, width / 2, height / 2), edge = Math.min(9, r), opticalWidth = Math.min(6, r);
        for (let y = 0; y < canvas.height; ++y) for (let x = 0; x < canvas.width; ++x) {
          const px = x + .5 - width / 2, py = y + .5 - height / 2;
          const qx = Math.abs(px) - (width / 2 - r), qy = Math.abs(py) - (height / 2 - r);
          const ox = Math.max(qx, 0), oy = Math.max(qy, 0), length = original(ox, oy);
          const distance = length + Math.min(Math.max(qx, qy), 0) - r;
          const nx = Math.sign(px) * (length ? ox / length : qx > qy ? 1 : 0);
          const ny = Math.sign(py) * (length ? oy / length : qy >= qx ? 1 : 0);
          const t = Math.max(0, Math.min(1, -distance / edge)), depth = Math.max(0, -distance);
          const bend = (distance <= 0 ? .9 * depth * (1 - t) ** 2 : 0) * 256 / 13;
          const expected = [Math.round(128 + nx * bend), Math.round(128 + ny * bend),
            distance <= 0 && depth < opticalWidth ? Math.round(255 * .35 * Math.sin(Math.PI * depth / opticalWidth) ** 2) : 0, 255];
          for (let c = 0; c < 4; ++c) error = Math.max(error, Math.abs(pixels[(y * canvas.width + x) * 4 + c] - expected[c]));
        }
      }
    } finally { Math.hypot = original; }
    return { error, evaluatedLargeMap };
  });
  await test.info().attach("fallback-map-cost", { body: JSON.stringify(result), contentType: "application/json" });
  expect(result.error).toBe(0);
  expect(result.evaluatedLargeMap).toBeLessThan(640 * 896 * .2);
});
