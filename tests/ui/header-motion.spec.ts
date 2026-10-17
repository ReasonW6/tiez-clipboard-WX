import { test, expect } from "@playwright/test";

for (const width of [250, 352, 425]) for (const reducedMotion of ["no-preference", "reduce"] as const) {
  test(`header actions stay round and inside ${width}px during ${reducedMotion} transitions`, async ({ page }) => {
    await page.setViewportSize({ width, height: 589 });
    await page.emulateMedia({ reducedMotion });
    await page.goto("/tests/ui/harness.html?theme=mica&mode=light");
    await expect(page.locator(".header-actions > button:visible")).toHaveCount(5);

    const result = await page.evaluate(async () => {
      const group = document.querySelector<HTMLElement>(".header-actions")!;
      const rows: { phase: string; left: number; right: number; width: number; buttons: { left: number; right: number; width: number; height: number; iconWidth: number; iconHeight: number }[]; hiddenVisible: number }[] = [];
      const sample = (phase: string) => {
        const bounds = group.getBoundingClientRect();
        const buttons = [...group.querySelectorAll<HTMLButtonElement>(":scope > button")];
        rows.push({ phase, left: bounds.left, right: bounds.right, width: bounds.width,
          buttons: buttons.filter(button => getComputedStyle(button).visibility === "visible").map(button => {
            const rect = button.getBoundingClientRect(), icon = button.querySelector("svg")!.getBoundingClientRect();
            return { left: rect.left, right: rect.right, width: rect.width, height: rect.height, iconWidth: icon.width, iconHeight: icon.height };
          }),
          hiddenVisible: buttons.filter(button => button.getAttribute("aria-hidden") === "true"
            && (getComputedStyle(button).visibility !== "hidden" || !button.disabled || button.tabIndex !== -1)).length
        });
      };
      const record = (phase: string, duration: number) => new Promise<void>(resolve => {
        const start = performance.now();
        const frame = () => { sample(phase); if (performance.now() - start < duration) requestAnimationFrame(frame); else resolve(); };
        requestAnimationFrame(frame);
      });
      const toggle = () => {
        const back = document.querySelector<HTMLButtonElement>(".header-leading > button");
        if (back) back.click();
        else group.querySelector<HTMLButtonElement>('button[title="设置"]')!.click();
      };
      sample("before");
      toggle(); await record("collapse", 650);
      const collapsedCount = rows.at(-1)!.buttons.length;
      toggle(); await record("expand", 650);
      for (let i = 0; i < 6; i++) { toggle(); await record("interrupted", 85); }
      await record("settled", 650);
      const right = rows[0].right;
      return {
        samples: rows.length, collapsedCount, finalCount: rows.at(-1)!.buttons.length,
        maxRightDrift: Math.max(...rows.map(row => Math.abs(row.right - right))),
        maxOverflow: Math.max(0, ...rows.flatMap(row => row.buttons.map(button => button.right - innerWidth))),
        maxUnderflow: Math.max(0, ...rows.flatMap(row => row.buttons.map(button => -button.left))),
        maxOutsideCapsule: Math.max(0, ...rows.flatMap(row => row.buttons.map(button => Math.max(row.left - button.left, button.right - row.right)))),
        maxButtonSizeError: Math.max(...rows.flatMap(row => row.buttons.map(button => Math.max(Math.abs(button.width - rows[0].buttons[0].width), Math.abs(button.height - rows[0].buttons[0].width))))),
        maxIconSizeError: Math.max(...rows.flatMap(row => row.buttons.map(button => Math.max(Math.abs(button.iconWidth - 16), Math.abs(button.iconHeight - 16))))),
        hiddenViolations: rows.reduce((sum, row) => sum + row.hiddenVisible, 0),
        intermediateWidths: rows.filter(row => row.width > 66.1 && row.width < 155.9).length
      };
    });
    console.log(JSON.stringify({ width, reducedMotion, ...result }));
    expect(result.samples).toBeGreaterThan(15);
    expect(result.collapsedCount).toBe(2);
    expect(result.finalCount).toBe(5);
    expect(result.maxRightDrift).toBeLessThan(.1);
    expect(result.maxOverflow).toBeLessThan(.1);
    expect(result.maxUnderflow).toBeLessThan(.1);
    expect(result.maxOutsideCapsule).toBeLessThan(.1);
    expect(result.maxButtonSizeError).toBeLessThan(.1);
    expect(result.maxIconSizeError).toBeLessThan(.1);
    expect(result.hiddenViolations).toBe(0);
    if (reducedMotion === "no-preference") expect(result.intermediateWidths).toBeGreaterThan(0);
    await page.locator(".header-actions").getByTitle("固定", { exact: true }).click();
    await expect.poll(() => page.evaluate(() => (window as any).testApp.calls.filter((call: any) => call.command === "set_window_pinned").at(-1)?.args.pinned)).toBe(false);
    await page.locator(".header-actions").getByTitle("隐藏", { exact: true }).click();
    await expect.poll(() => page.evaluate(() => (window as any).testApp.calls.some((call: any) => call.command === "hide_window_cmd"))).toBe(true);
  });
}

for (const theme of ["paper", "sakura"]) {
  test(`${theme} retains its CSS hover transform after header layout animation`, async ({ page }) => {
    await page.emulateMedia({ reducedMotion: "no-preference" });
    await page.goto(`/tests/ui/harness.html?theme=${theme}&mode=light`);
    await page.locator(".header-actions").getByTitle("设置", { exact: true }).click();
    await expect(page.locator(".header-actions > button:visible")).toHaveCount(2);
    await expect(page.locator(".header-actions")).toHaveCSS("transform", "none");
    await page.locator(".header-leading > button").click();
    await expect(page.locator(".header-actions > button:visible")).toHaveCount(5);
    await expect(page.locator(".header-actions")).toHaveCSS("transform", "none");
    const pin = page.locator(".header-actions > button").first();
    await pin.hover();
    await expect.poll(() => pin.evaluate(element => new DOMMatrixReadOnly(getComputedStyle(element).transform).m42)).toBeLessThan(-.5);
    expect(await pin.evaluate(element => (element as HTMLElement).style.transform)).toBe("");
  });
}
