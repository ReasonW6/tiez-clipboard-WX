import { test, expect } from "@playwright/test";

for (const [theme, mode] of [["mica", "light"], ["mica", "dark"], ["acrylic", "light"], ["acrylic", "dark"], ["sakura", "light"]]) {
  test(`${theme} ${mode}: compact rows keep the time and tags inside their own row`, async ({ page }) => {
    await page.addInitScript(() => localStorage.setItem("fixture-settings", JSON.stringify({ "app.compact_mode": "true" })));
    await page.goto(`/tests/ui/harness.html?theme=${theme}&mode=${mode}`);
    await expect(page.locator(".history-item.compact")).toHaveCount(6);
    for (const width of [250, 352, 425]) {
      await page.setViewportSize({ width, height: 589 });
      const problems = await page.evaluate(() => {
        const overlap = (a: DOMRect, b: DOMRect) =>
          a.left < b.right - .5 && b.left < a.right - .5 && a.top < b.bottom - .5 && b.top < a.bottom - .5;
        const visible = (element: Element) => {
          const rect = element.getBoundingClientRect();
          return rect.width > 0 && rect.height > 0 && getComputedStyle(element).visibility !== "hidden";
        };
        const rows = [...document.querySelectorAll(".history-item.compact")];
        const found: string[] = [];
        rows.forEach((row, index) => {
          const box = row.getBoundingClientRect();
          const chips = [...row.querySelectorAll(".item-tags-container .tag-chip")].filter(visible);
          const times = [...row.querySelectorAll(".meta-time")].filter(visible);
          for (const time of times) for (const chip of chips)
            if (overlap(time.getBoundingClientRect(), chip.getBoundingClientRect())) found.push(`row ${index}: time over ${chip.textContent}`);
          for (const element of [...times, ...chips]) {
            const rect = element.getBoundingClientRect();
            if (rect.top < box.top - .5 || rect.bottom > box.bottom + .5) found.push(`row ${index}: ${element.textContent} leaves the row`);
          }
          if (index && overlap(rows[index - 1].getBoundingClientRect(), box)) found.push(`rows ${index - 1}/${index} overlap`);
        });
        return found;
      });
      expect(problems, `${width}px`).toEqual([]);
    }
  });
}
