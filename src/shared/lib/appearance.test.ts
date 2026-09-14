import { expect, it } from "vitest";
import { getMaterialSurfaces } from "./appearance";

it("keeps both endpoints on the same continuous material curve", () => {
  const stops = [0, 1, 25, 50, 75, 99, 100].map(getMaterialSurfaces);
  expect(stops[0].window).toBeGreaterThan(.2);
  expect(stops[stops.length - 1].window).toBeGreaterThan(.94);
  expect(stops[1].window - stops[0].window).toBeLessThan(.01);
  expect(stops[stops.length - 1].window - stops[stops.length - 2].window).toBeLessThan(.01);
  for (let i = 1; i < stops.length; i++) {
    expect(stops[i].window).toBeGreaterThan(stops[i - 1].window);
    expect(stops[i].content).toBeGreaterThanOrEqual(.9);
    expect(stops[i].control).toBeGreaterThanOrEqual(.76);
  }
});

it("keeps a safe default and bounds invalid stored settings", () => {
  expect(getMaterialSurfaces(-1)).toEqual(getMaterialSurfaces(0));
  expect(getMaterialSurfaces(101)).toEqual(getMaterialSurfaces(100));
  expect(getMaterialSurfaces(Number.NaN)).toEqual(getMaterialSurfaces(50));
});
