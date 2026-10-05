import { test, expect, type Page } from "@playwright/test";

// Uniform desktop inputs are preserved by Gaussian blur. This checks the real
// DOM palette and source-over fills; it does not stand in for native optics QA.
async function contrast(page: Page, selector: string) {
  const [elementSelector, pseudo] = selector.split("::");
  return page.locator(elementSelector).first().evaluate((element, pseudo) => {
    const parse = (value: string) => {
      const color = value.match(/[\d.]+/g)!.map(Number);
      return [color[0], color[1], color[2], color[3] ?? 1];
    };
    const over = (fg: number[], bg: number[]) => fg.slice(0, 3)
      .map((channel, i) => channel * fg[3] + bg[i] * (1 - fg[3])).concat(1);
    const luminance = (color: number[]) => color.slice(0, 3).map(channel => channel / 255)
      .map(channel => channel <= .04045 ? channel / 12.92 : ((channel + .055) / 1.055) ** 2.4)
      .reduce((sum, channel, i) => sum + channel * [.2126, .7152, .0722][i], 0);
    const ancestors: Element[] = [];
    for (let current: Element | null = element; current; current = current.parentElement) ancestors.unshift(current);
    const style = getComputedStyle(element, pseudo ? `::${pseudo}` : null);
    const foreground = parse(style.color);
    foreground[3] *= ancestors.reduce((alpha, ancestor) => alpha * Number(getComputedStyle(ancestor).opacity), 1)
      * (pseudo ? Number(style.opacity) : 1);
    return [0, 139, 255].map(gray => {
      let background = [gray, gray, gray, 1];
      for (const ancestor of ancestors) background = over(parse(getComputedStyle(ancestor).backgroundColor), background);
      const ink = luminance(over(foreground, background)), paper = luminance(background);
      return { gray, ratio: (Math.max(ink, paper) + .05) / (Math.min(ink, paper) + .05) };
    });
  }, pseudo);
}

for (const theme of ["mica", "acrylic", "liquid-glass"]) {
  for (const mode of ["light", "dark"]) {
    test(`${theme} ${mode}: tag controls remain legible at both transparency endpoints`, async ({ page }) => {
      await page.emulateMedia({ reducedMotion: "reduce" });
      const cdp = await page.context().newCDPSession(page);
      await cdp.send("Emulation.setEmulatedMedia", { features: [
        { name: "prefers-reduced-motion", value: "reduce" },
        { name: "prefers-reduced-transparency", value: "no-preference" }
      ] });
      for (const opacity of [0, 100]) {
        await page.goto(`/tests/ui/harness.html?theme=${theme}&mode=${mode}&opacity=${opacity}`);
        await expect(page.locator(".header-title")).toHaveText("TieZ");
        await page.getByTitle("标签管理", { exact: true }).click();
        await page.locator(".tm-tag").first().click();
        await expect(page.locator(".tm-card").first()).toBeVisible();
        await page.locator(".tm-toolbar .tm-button").click();
        await page.locator(".tm-paste-target").first().click();
        for (const selector of [".tm-tag.active .tm-tag-name", ".tm-tag.active .tm-count",
          ".tm-view-toggle .active", ".tm-button.active", ".tm-primary",
          ".selected .tm-checkbox", ".tm-search input::placeholder"]) {
          for (const sample of await contrast(page, selector)) {
            expect.soft(sample.ratio, `${100 - opacity}% on ${sample.gray}: ${selector}`).toBeGreaterThanOrEqual(4.5);
          }
        }
        await page.locator(".tm-search input").fill("新建验收标签");
        await expect(page.locator(".tm-create-tag")).toBeVisible();
        for (const sample of await contrast(page, ".tm-create-tag")) {
          expect.soft(sample.ratio, `${100 - opacity}% on ${sample.gray}: new tag text`).toBeGreaterThanOrEqual(4.5);
        }
        // Test data stays in the fixture: entering a name does not create it.
        expect(await page.evaluate(() => (window as any).testApp.calls.some((call: any) => call.command === "create_new_tag"))).toBe(false);
      }
    });
  }
}
