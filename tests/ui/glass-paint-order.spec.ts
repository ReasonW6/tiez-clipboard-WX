import { expect, test } from "@playwright/test";

test("native surface order agrees with Chromium paint order across nested contexts", async ({ page }) => {
  await page.goto("/tests/ui/harness.html?theme=liquid-glass&nativeGlass=true");
  const results = await page.evaluate(async () => {
    const { glassPaintOrder } = await import("/src/shared/lib/glassPaintOrder.ts");
    const cases = [
      { name: "reversed registration", a: {}, b: {} },
      { name: "parent z index", a: { zIndex: "4" }, b: { zIndex: "1" } },
      { name: "negative context", a: { zIndex: "-2" }, b: { zIndex: "0" } },
      { name: "nested high index stays inside parent", a: { zIndex: "-2" }, b: { zIndex: "1" }, innerA: "100" },
      { name: "auto is not a context", a: { zIndex: "auto" }, b: { zIndex: "1" }, innerA: "100" },
      { name: "isolation creates context", a: { isolation: "isolate" }, b: { zIndex: "1" }, innerA: "100" },
      { name: "transform creates context", a: { transform: "translateX(0px)" }, b: { zIndex: "1" }, innerA: "100" },
      { name: "opacity .999 creates context", a: { opacity: ".999" }, b: { zIndex: "1" }, innerA: "100" },
      { name: "flex order", display: "flex", a: { order: "2" }, b: { order: "1" } },
      { name: "flex order does not override z", display: "flex", a: { order: "2", zIndex: "1" }, b: { order: "1", zIndex: "2" } },
      { name: "grid order", display: "grid", a: { order: "2" }, b: { order: "1" } },
    ];
    return cases.map(spec => {
      const frame = document.createElement("div");
      Object.assign(frame.style, { position: "fixed", left: "10px", top: "10px", width: "100px", height: "100px", zIndex: "999999", isolation: "isolate", display: spec.display || "block" });
      const surfaces: HTMLElement[] = [];
      for (const [name, custom] of [["a", spec.a], ["b", spec.b]] as const) {
        const host = document.createElement("div");
        Object.assign(host.style, { position: spec.display ? "relative" : "absolute", inset: "0", width: "100px", height: "100px", flexShrink: "0", marginRight: spec.display === "flex" ? "-100px" : "0", gridArea: "1 / 1", ...custom });
        const inner = document.createElement("div");
        Object.assign(inner.style, { position: "absolute", inset: "0", zIndex: name === "a" ? spec.innerA || "auto" : "auto" });
        const surface = document.createElement("div");
        surface.id = `paint-oracle-${name}`;
        Object.assign(surface.style, { position: "absolute", inset: "0", zIndex: "-1", pointerEvents: "auto", background: name === "a" ? "red" : "blue" });
        inner.append(surface); host.append(inner); frame.append(host); surfaces.push(surface);
      }
      document.body.append(frame);
      const compare = () => ({
        actual: glassPaintOrder([...surfaces].reverse()).map(element => element.id),
        expected: document.elementsFromPoint(50, 50).filter(element => surfaces.includes(element as HTMLElement)).reverse().map(element => element.id),
      });
      const before = compare();
      frame.prepend(frame.lastElementChild!);
      const after = compare();
      frame.remove();
      return { name: spec.name, before, after };
    });
  });
  for (const result of results) for (const phase of ["before", "after"] as const) {
    expect(result[phase].expected, `${result.name}: browser must hit both test surfaces`).toHaveLength(2);
    expect(result[phase].actual, `${result.name}: ${phase} DOM mutation`).toEqual(result[phase].expected);
  }
});
