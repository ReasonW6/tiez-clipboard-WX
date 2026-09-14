import { expect, it } from "vitest";
import { getLiquidGlassMaterial, nativeBackdropEnabled } from "./appearance";

it("one value moves the complete material from clear to frosted", () => {
  const clear = getLiquidGlassMaterial(0), frosted = getLiquidGlassMaterial(100);
  expect(clear.blur).toBe(0);
  expect(frosted.blur).toBe(64);
  expect(clear.specular).toBeGreaterThan(frosted.specular);
  expect(clear.refraction).toBeGreaterThan(frosted.refraction);
  expect(getLiquidGlassMaterial(50).blur).toBeCloseTo(18, 0);
  let previous = 0;
  for (let opacity = 0; opacity <= 100; opacity++) {
    const material = getLiquidGlassMaterial(opacity);
    expect(material.blur).toBeGreaterThanOrEqual(previous);
    previous = material.blur;
  }
  expect(getLiquidGlassMaterial(-5)).toEqual(clear);
  expect(getLiquidGlassMaterial(999)).toEqual(frosted);
  expect(getLiquidGlassMaterial(Number.NaN)).toEqual(getLiquidGlassMaterial(50));
});

it("the clear endpoint removes the native backdrop rather than just its tint", () => {
  expect(nativeBackdropEnabled(0)).toBe(false);
  expect(nativeBackdropEnabled(100)).toBe(true);
  expect(nativeBackdropEnabled(Number.NaN)).toBe(true);
});
