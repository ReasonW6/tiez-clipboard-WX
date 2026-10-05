import { test, expect } from "@playwright/test";

test("advanced settings uses the same heading face and color as the other groups", async ({ page }) => {
  await page.goto("/tests/ui/harness.html?theme=acrylic&mode=light&opacity=0");
  await page.getByTitle("设置", { exact: true }).click();
  const normal = page.getByRole("heading", { name: "界面设置", exact: true });
  const advanced = page.getByRole("heading", { name: "高级设置", exact: true });
  const face = (element: Element) => {
    const css = getComputedStyle(element);
    return { color: css.color, family: css.fontFamily, size: css.fontSize, weight: css.fontWeight };
  };
  expect(await advanced.evaluate(face)).toEqual(await normal.evaluate(face));
  const note = page.locator(".settings-nav-card .settings-subpage-note");
  await expect(note).toHaveCSS("font-size", "12px");
  await expect(note).toHaveCSS("font-weight", "500");
  await expect(note).toHaveCSS("opacity", "1");
});

test("all surface controls show transparency and preserve the previous material after reload", async ({ page }) => {
  await page.goto("/tests/ui/harness.html?theme=acrylic&mode=light");
  await page.getByTitle("设置", { exact: true }).click();
  await page.getByRole("heading", { name: "界面设置", exact: true }).click();
  const slider = page.getByRole("slider", { name: "界面透明度", exact: true });
  await slider.fill("80");
  await expect.poll(() => page.evaluate(() => (window as any).testApp.settings["app.surface_opacity"])).toBe("20");
  const fill = await page.locator("#root").evaluate(el => getComputedStyle(el).backgroundColor);
  await page.reload();
  await expect(page.locator("#root")).toHaveCSS("background-color", fill);
  await page.getByTitle("设置", { exact: true }).click();
  await page.getByRole("heading", { name: "界面设置", exact: true }).click();
  await expect(slider).toHaveValue("80");
  await expect(page.getByText("界面不透明度", { exact: true })).toHaveCount(0);
});

test("background transparency also inverts the saved density and survives reload", async ({ page }) => {
  await page.route("http://asset.localhost/**", route => route.fulfill({ contentType: "image/svg+xml", body: '<svg xmlns="http://www.w3.org/2000/svg" width="32" height="32"><path fill="#9aaab9" d="M0 0h32v32H0z"/></svg>' }));
  await page.addInitScript(() => {
    if (!localStorage.getItem("fixture-settings")) localStorage.setItem("fixture-settings", JSON.stringify({
      "app.custom_background": "C:/material-test.svg", "app.custom_background_opacity": "40"
    }));
  });
  await page.goto("/tests/ui/harness.html?theme=acrylic&mode=light");
  await page.getByTitle("设置", { exact: true }).click();
  await page.getByRole("heading", { name: "界面设置", exact: true }).click();
  const slider = page.getByRole("slider", { name: "背景图片透明度", exact: true });
  await expect(slider).toHaveValue("60");
  await slider.fill("85");
  await expect.poll(() => page.evaluate(() => (window as any).testApp.settings["app.custom_background_opacity"])).toBe("15");
  await expect.poll(() => page.evaluate(() => document.documentElement.style.getPropertyValue("--custom-bg-opacity"))).toBe("0.15");
  await page.reload();
  await page.getByTitle("设置", { exact: true }).click();
  await page.getByRole("heading", { name: "界面设置", exact: true }).click();
  await expect(slider).toHaveValue("85");
});

for (const mode of ["light", "dark"]) {
  test(`clear acrylic ${mode} uses quiet outlines and retains keyboard focus and unfiltered text`, async ({ page }) => {
    await page.goto(`/tests/ui/harness.html?theme=acrylic&mode=${mode}&opacity=0`);
    await page.getByTitle("设置", { exact: true }).click();
    await page.getByRole("heading", { name: "界面设置", exact: true }).click();
    for (const selector of [".theme-choice-btn:not(.active)", ".settings-group", ".settings-group:has(.settings-nav-card)"]) {
      const border = await page.locator(selector).first().evaluate(el => {
        const css = getComputedStyle(el);
        return { width: parseFloat(css.borderTopWidth), alpha: Number(css.borderTopColor.match(/[\d.]+/g)?.[3] ?? 1) };
      });
      expect(border.width, selector).toBeGreaterThanOrEqual(1);
      expect(border.alpha, selector).toBeGreaterThan(0);
      expect(border.alpha, selector).toBeLessThanOrEqual(.16);
    }
    const choice = page.locator(".theme-choice-btn:not(.active)").first();
    await choice.focus();
    await page.keyboard.press("Tab");
    await page.keyboard.press("Shift+Tab");
    await expect(choice).toBeFocused();
    await expect(choice).toHaveCSS("outline-style", "solid");
    await expect(choice).toHaveCSS("outline-width", "2px");
    for (const selector of [".settings-nav-card h3", ".settings-subpage-note", ".glass-setting-hint"]) {
      await expect(page.locator(selector).first()).toHaveCSS("filter", "none");
      await expect(page.locator(selector).first()).toHaveCSS("opacity", "1");
      await expect(page.locator(selector).first()).toHaveCSS("text-shadow", "none");
    }
  });
}
