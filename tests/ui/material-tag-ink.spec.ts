import { test, expect, type Locator } from "@playwright/test";

async function readChip(chip: Locator) {
  return chip.evaluate(element => {
    const css = getComputedStyle(element);
    const luminance = (color: string) => color.match(/[\d.]+/g)!.slice(0, 3).map(Number)
      .map(channel => channel / 255)
      .map(channel => channel <= .04045 ? channel / 12.92 : ((channel + .055) / 1.055) ** 2.4)
      .reduce((sum, channel, i) => sum + channel * [.2126, .7152, .0722][i], 0);
    const ink = luminance(css.color), paper = luminance(css.backgroundColor);
    return { color: css.color, background: css.backgroundColor, filter: css.filter,
      ratio: (Math.max(ink, paper) + .05) / (Math.min(ink, paper) + .05) };
  });
}

test("material tag ink covers generated HSL and opaque color-picker HEX", async ({ page }) => {
  await page.goto("/tests/ui/harness.html?theme=mica&mode=light&opacity=0");
  const result = await page.evaluate(async () => {
    const path = "/src/shared/lib/utils.ts";
    const { getTagColor, getTagTextColor } = await import(path);
    const chip = document.createElement("span");
    document.body.append(chip);
    const luminance = (color: string) => color.match(/[\d.]+/g)!.slice(0, 3).map(Number)
      .map(channel => channel / 255)
      .map(channel => channel <= .04045 ? channel / 12.92 : ((channel + .055) / 1.055) ** 2.4)
      .reduce((sum, channel, i) => sum + channel * [.2126, .7152, .0722][i], 0);
    const measure = (background: string, theme: string) => {
      chip.style.backgroundColor = background;
      chip.style.color = getTagTextColor(background, theme);
      const css = getComputedStyle(chip);
      const ink = luminance(css.color), paper = luminance(css.backgroundColor);
      return { background, color: css.color, ratio: (Math.max(ink, paper) + .05) / (Math.min(ink, paper) + .05) };
    };
    const samples = [getTagColor("测试", "mica"), "#507ed9", "#9681ce", "#c37797",
      "#000000", "#ffffff", "#747474", "#757575", "#777777", "#090", "#fff",
      ...Array.from({ length: 360 }, (_, hue) => `hsl(${hue}, 76%, 62%)`),
      ...Array.from({ length: 256 }, (_, value) => `#${value.toString(16).padStart(2, "0").repeat(3)}`)];
    const materials = ["mica", "acrylic"].map(theme => ({ theme,
      worst: samples.map(background => measure(background, theme)).sort((a, b) => a.ratio - b.ratio)[0] }));
    const originalFailure = measure(getTagColor("测试", "mica"), "mica");
    const unchanged = ["paper", "retro", "sakura", "sticky-note", "unknown"].map(theme => measure("#ffffff", theme).color);
    const unsupported = ["transparent", "var(--tag)", "rgba(0, 0, 0, .5)", "#0008", "hsl(0, 100%, 50%, .5)"]
      .map(background => getTagTextColor(background, "mica"));
    chip.remove();
    return { materials, originalFailure, unchanged, unsupported };
  });
  console.log(JSON.stringify(result));
  expect(result.originalFailure.ratio).toBeGreaterThanOrEqual(4.5);
  for (const { theme, worst } of result.materials) expect(worst.ratio, `${theme}: ${JSON.stringify(worst)}`).toBeGreaterThanOrEqual(4.5);
  expect(result.unchanged).toEqual(Array(5).fill("rgb(255, 255, 255)"));
  expect(result.unsupported).toEqual(Array(5).fill("#ffffff"));
});

for (const theme of ["mica", "acrylic"]) {
  test(`${theme}: all three tag call sites retain readable ink at both endpoints`, async ({ page }) => {
    await page.emulateMedia({ reducedMotion: "reduce" });
    for (const mode of ["light", "dark"]) for (const opacity of [0, 100]) {
      await page.goto(`/tests/ui/harness.html?theme=${theme}&mode=${mode}&opacity=${opacity}`);
      await expect(page.locator(".item-tags-container .tag-chip").first()).toBeVisible();
      const itemTag = page.locator(".item-tags-container .tag-chip").first();
      const beforeHover = await readChip(itemTag);
      expect(beforeHover.background).toBe("rgb(80, 126, 217)");
      expect(beforeHover.ratio).toBeGreaterThanOrEqual(4.5);
      await itemTag.hover();
      await expect(itemTag).toHaveCSS("filter", "none");
      expect((await readChip(itemTag)).background).toBe(beforeHover.background);
      await page.locator(".search-input").click();
      await expect(page.locator(".tags-dropdown .tag-chip").first()).toBeVisible();
      for (const tag of await page.locator(".tags-dropdown .tag-chip").all()) {
        expect((await readChip(tag)).ratio).toBeGreaterThanOrEqual(4.5);
      }
      await page.locator(".search-input").blur();
      await page.getByTitle("Tags", { exact: true }).first().click();
      await expect(page.locator(".tag-suggest-pill").first()).toBeVisible();
      for (const tag of await page.locator(".tag-suggest-pill").all()) {
        expect((await readChip(tag)).ratio).toBeGreaterThanOrEqual(4.5);
      }
    }
  });
}
