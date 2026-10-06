import { expect, test } from "@playwright/test";

// Header transitions move controls every other frame. Each control may fall back
// to CSS once and return to native once; alternating renderers reads as flicker.
test("animated header transitions do not alternate glass renderers", async ({ page }) => {
  await page.goto("/tests/ui/harness.html?theme=liquid-glass&nativeGlass=true");
  await expect(page.locator(".history-item").first()).toBeVisible();
  await page.evaluate(() => (window as any).testApp.setGlassStatus("ready"));
  await expect(page.locator(".header-actions > .glass-lens")).toHaveAttribute("data-native-glass", "true");

  const changes = await page.evaluate(async () => {
    const counts = new Map<Element, number>();
    const observer = new MutationObserver(mutations => {
      for (const mutation of mutations) counts.set(mutation.target as Element, (counts.get(mutation.target as Element) ?? 0) + 1);
    });
    observer.observe(document.body, { attributes: true, subtree: true, attributeFilter: ["data-native-glass"] });
    document.querySelector<HTMLElement>('button[title="标签管理"]')!.click();
    await new Promise(resolve => setTimeout(resolve, 1200));
    observer.disconnect();
    return [...counts].map(([lens, count]) => ({
      control: (lens.parentElement as HTMLElement).className.toString().split(" ").slice(0, 2).join(" "), count }));
  });
  expect(changes.length).toBeGreaterThan(0);
  for (const change of changes) expect(change.count, change.control).toBeLessThanOrEqual(2);
  await expect(page.locator(".header-back > .glass-lens")).toHaveAttribute("data-native-glass", "true");
});
