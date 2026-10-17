import { test, expect } from "@playwright/test";

for (const theme of ["mica", "acrylic"]) for (const mode of ["light", "dark"]) {
  test(`${theme} ${mode}: the tag dropdown covers the type filters below it`, async ({ page }) => {
    await page.goto(`/tests/ui/harness.html?theme=${theme}&mode=${mode}`);
    await page.locator(".search-input").click();
    const dropdown = page.locator(".tags-dropdown");
    await expect(dropdown).toBeVisible();
    const result = await page.evaluate(() => {
      const panel = document.querySelector(".tags-dropdown")!.getBoundingClientRect();
      const covered = [...document.querySelectorAll(".type-filter-btn")].map(button => button.getBoundingClientRect())
        .filter(rect => rect.top >= panel.top && rect.bottom <= panel.bottom);
      const alpha = Number(getComputedStyle(document.querySelector(".tags-dropdown")!).backgroundColor.match(/[\d.]+/g)![3] ?? 1);
      return { covered: covered.length, alpha, topmost: covered.map(rect =>
        !!document.elementFromPoint(rect.left + rect.width / 2, rect.top + rect.height / 2)?.closest(".tags-dropdown")) };
    });
    expect(result.covered).toBeGreaterThan(0);
    expect(result.topmost.every(Boolean)).toBe(true);
    expect(result.alpha).toBeGreaterThanOrEqual(.9);
  });
}

test("a picked tag becomes a chip, narrows the list and turns back into text on backspace", async ({ page }) => {
  await page.goto("/tests/ui/harness.html?theme=mica&mode=dark");
  const input = page.locator(".search-input");
  const items = page.locator(".history-item");
  await expect(items).toHaveCount(6);
  await input.click();
  await page.locator('.tags-dropdown .tag-chip[data-tag="工作"]').click();
  const chip = page.locator(".search-tag-chip");
  await expect(chip).toHaveText("工作");
  await expect(input).toHaveValue("");
  await expect(items).toHaveCount(4);
  // The caret starts after the chip, not underneath it.
  const [chipBox, padding] = await Promise.all([chip.boundingBox(), input.evaluate(element => parseFloat(getComputedStyle(element).paddingLeft))]);
  const inputBox = (await input.boundingBox())!;
  expect(inputBox.x + padding).toBeGreaterThan(chipBox!.x + chipBox!.width);

  await input.pressSequentially("npm");
  await expect(items).toHaveCount(1);
  await expect(chip).toHaveText("工作");
  for (let i = 0; i < 3; i++) await input.press("Backspace");
  await expect(input).toHaveValue("");
  await expect(chip).toHaveText("工作");
  await expect(items).toHaveCount(4);

  await input.press("Backspace");
  await expect(chip).toHaveCount(0);
  await expect(input).toHaveValue("tag:工作");
  await input.press("Backspace");
  await expect(input).toHaveValue("tag:工");
});
