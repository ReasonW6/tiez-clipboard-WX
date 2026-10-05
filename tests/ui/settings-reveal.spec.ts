import { expect, test } from "@playwright/test";

for (const theme of ["liquid-glass", "mica", "acrylic"]) {
  test(`reopening ${theme} settings preserves visible switch states on the first frame`, async ({ page }) => {
    await page.emulateMedia({ reducedMotion: "no-preference" });
    await page.goto(`/tests/ui/harness.html?theme=${theme}&mode=dark&nativeGlass=true`);
    await page.getByTitle("设置", { exact: true }).click();
    await page.getByRole("heading", { name: "常规设置", exact: true }).click();
    await expect(page.locator(".group-content .switch input").first()).toBeAttached();
    const saved = await page.locator(".group-content .switch input").evaluateAll(inputs => inputs.map(input => (input as HTMLInputElement).checked));
    await page.locator(".header-back").click();
    await expect(page.locator(".settings-view")).toHaveCount(0);
    const frames = await page.evaluate(async () => {
      document.querySelector<HTMLElement>('[title="设置"]')!.click();
      const samples: { checked: boolean[]; opacity: number }[] = [];
      const start = performance.now();
      let opened = false;
      await new Promise<void>(resolve => {
        const observe = () => {
          const header = [...document.querySelectorAll<HTMLElement>(".group-header")]
            .find(element => element.querySelector("h3")?.textContent === "常规设置");
          if (header && !opened) { header.click(); opened = true; }
          const content = document.querySelector(".group-content");
          if (content) {
            let opacity = 1;
            for (let element: Element | null = content; element; element = element.parentElement)
              opacity *= Number(getComputedStyle(element).opacity);
            samples.push({ opacity, checked: [...content.querySelectorAll<HTMLInputElement>(".switch input")].map(input => input.checked) });
          }
          if (performance.now() - start < 400) requestAnimationFrame(observe); else resolve();
        };
        requestAnimationFrame(observe);
      });
      return samples;
    });
    expect(frames.length).toBeGreaterThan(1);
    for (const [index, frame] of frames.entries()) {
      expect(frame.checked).toEqual(saved);
      expect(frame.opacity, `frame ${index}; ${JSON.stringify(frames)}`).toBe(1);
    }
  });

  test(`expanding ${theme} settings shows the saved switch states on the first frame`, async ({ page }) => {
    await page.emulateMedia({ reducedMotion: "reduce" });
    await page.goto(`/tests/ui/harness.html?theme=${theme}&mode=dark&nativeGlass=true`);
    await page.getByTitle("设置", { exact: true }).click();
    await expect(page.locator(".settings-view")).toBeVisible();
    await expect(page.getByRole("heading", { name: "常规设置", exact: true })).toBeVisible();
    await page.evaluate(() => (window as any).testApp.setGlassStatus("ready"));
    await page.emulateMedia({ reducedMotion: "no-preference" });
    for (let round = 0; round < 2; ++round) {
      const evidence = await page.evaluate(async () => {
        const group = [...document.querySelectorAll<HTMLElement>(".settings-group")]
          .find(element => element.querySelector("h3")?.textContent === "常规设置")!;
        const calls = () => (window as any).testApp.calls.filter((call: any) => call.command === "get_settings").length;
        const beforeReads = calls();
        group.querySelector<HTMLElement>(".group-header")!.click();
        const frames: { elapsed: number; opacity: number; checked: boolean[]; thumbs: string[]; eligible: string[] }[] = [];
        const start = performance.now();
        await new Promise<void>(resolve => {
          const observe = () => {
            const content = group.querySelector<HTMLElement>(".group-content");
            if (content) frames.push({
              elapsed: performance.now() - start,
              opacity: Number(getComputedStyle(content).opacity),
              checked: [...content.querySelectorAll<HTMLInputElement>(".switch input")].map(input => input.checked),
              thumbs: [...content.querySelectorAll<HTMLElement>(".toggle")].map(toggle => getComputedStyle(toggle, "::before").transform),
              eligible: [...content.querySelectorAll<HTMLElement>(".toggle > .glass-lens")].map(lens => lens.dataset.nativeGlass || "unregistered"),
            });
            if (performance.now() - start < 300) requestAnimationFrame(observe);
            else resolve();
          };
          requestAnimationFrame(observe);
        });
        return { frames, beforeReads, afterReads: calls() };
      });
      expect(evidence.frames.length).toBeGreaterThan(1);
      expect(evidence.frames[0].checked.length).toBeGreaterThan(1);
      for (const frame of evidence.frames) {
        expect(frame.checked, `round ${round}, ${frame.elapsed} ms`).toEqual(evidence.frames.at(-1)!.checked);
        expect(frame.thumbs, `round ${round}, ${frame.elapsed} ms`).toEqual(evidence.frames.at(-1)!.thumbs);
        expect(frame.opacity, `round ${round}, ${frame.elapsed} ms; ${JSON.stringify(evidence.frames)}`).toBe(1);
      }
      expect(evidence.afterReads).toBe(evidence.beforeReads);
      await page.getByRole("heading", { name: "常规设置", exact: true }).click();
    }
  });
}
