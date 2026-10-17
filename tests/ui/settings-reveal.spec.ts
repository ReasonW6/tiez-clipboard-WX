import { expect, test } from "@playwright/test";

for (const theme of ["mica", "acrylic"]) {
  test(`reopening ${theme} settings preserves visible switch states on the first frame`, async ({ page }) => {
    await page.emulateMedia({ reducedMotion: "no-preference" });
    await page.goto(`/tests/ui/harness.html?theme=${theme}&mode=dark`);
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
            // The settings page fades in as a whole; its switches must not fade on top of that.
            const opacity = Number(getComputedStyle(content).opacity);
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
    await page.goto(`/tests/ui/harness.html?theme=${theme}&mode=dark`);
    await page.getByTitle("设置", { exact: true }).click();
    await expect(page.locator(".settings-view")).toBeVisible();
    await expect(page.getByRole("heading", { name: "常规设置", exact: true })).toBeVisible();
    await page.emulateMedia({ reducedMotion: "no-preference" });
    for (let round = 0; round < 2; ++round) {
      const evidence = await page.evaluate(async () => {
        const group = [...document.querySelectorAll<HTMLElement>(".settings-group")]
          .find(element => element.querySelector("h3")?.textContent === "常规设置")!;
        const calls = () => (window as any).testApp.calls.filter((call: any) => call.command === "get_settings").length;
        const beforeReads = calls();
        group.querySelector<HTMLElement>(".group-header")!.click();
        const frames: { elapsed: number; opacity: number; checked: boolean[]; thumbs: string[] }[] = [];
        const start = performance.now();
        await new Promise<void>(resolve => {
          const observe = () => {
            const content = group.querySelector<HTMLElement>(".group-content");
            if (content) frames.push({
              elapsed: performance.now() - start,
              opacity: Number(getComputedStyle(content).opacity),
              checked: [...content.querySelectorAll<HTMLInputElement>(".switch input")].map(input => input.checked),
              thumbs: [...content.querySelectorAll<HTMLElement>(".toggle")].map(toggle => getComputedStyle(toggle, "::before").transform),
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

// The history list remounts on return; it must not paint an empty frame first.
for (const title of ["设置", "标签管理"]) {
  test(`returning from ${title} shows the history cards on the first frame`, async ({ page }) => {
    await page.emulateMedia({ reducedMotion: "no-preference" });
    await page.goto("/tests/ui/harness.html?theme=mica");
    await expect(page.locator(".history-item")).toHaveCount(6);
    await page.getByTitle(title, { exact: true }).click();
    await expect(page.locator(".history-item")).toHaveCount(0);
    const frames = await page.evaluate(async () => {
      document.querySelector<HTMLElement>(".header-back")!.click();
      const counts: number[] = [];
      const start = performance.now();
      await new Promise<void>(resolve => {
        const sample = () => {
          counts.push(document.querySelectorAll(".history-item").length);
          if (performance.now() - start < 300) requestAnimationFrame(sample); else resolve();
        };
        requestAnimationFrame(sample);
      });
      return counts;
    });
    expect(frames.length).toBeGreaterThan(1);
    expect(frames, JSON.stringify(frames)).toEqual(frames.map(() => 6));
  });
}
