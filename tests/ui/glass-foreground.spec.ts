import { expect, test } from "@playwright/test";
import { writeFile } from "node:fs/promises";

for (const theme of ["mica", "acrylic"]) for (const mode of ["light", "dark"] as const) {
  test(`${theme} footer ink follows a ${mode} palette change on its first frame`, async ({ page }) => {
    await page.emulateMedia({ reducedMotion: "no-preference" });
    await page.goto(`/tests/ui/harness.html?theme=${theme}&mode=${mode}`);
    await page.getByTitle("设置", { exact: true }).click();
    await expect(page.getByText("问题反馈", { exact: true })).toBeVisible();
    await expect(page.locator(".settings-view")).toHaveCSS("opacity", "1");
    const evidence = await page.evaluate(async () => {
      const modulePath = "/src/shared/lib/appearance.ts";
      const { applyColorMode } = await import(modulePath);
      const labels = ["问题反馈", "恢复初始设置", "检查更新"];
      const elements = labels.map(label => [...document.querySelectorAll<HTMLElement>(".settings-view span, .settings-view button")]
        .find(element => element.textContent?.trim() === label)!);
      if (elements.some(element => !element)) throw new Error("Footer fixture is incomplete");
      const colours = () => elements.map(element => getComputedStyle(element).color);
      const before = colours();
      const next = document.body.classList.contains("dark-mode") ? "light" : "dark";
      applyColorMode(next);
      const frames: { elapsed: number; colours: string[] }[] = [];
      const start = performance.now();
      await new Promise<void>(resolve => {
        const sample = () => {
          frames.push({ elapsed: performance.now() - start, colours: colours() });
          if (performance.now() - start < 350) requestAnimationFrame(sample); else resolve();
        };
        requestAnimationFrame(sample);
      });
      return { labels, before, next, frames };
    });
    const path = test.info().outputPath("foreground-frames.json");
    await writeFile(path, JSON.stringify(evidence, null, 2));
    await test.info().attach("foreground-frames", { path, contentType: "application/json" });
    expect(evidence.frames.length).toBeGreaterThan(2);
    const settled = evidence.frames.at(-1)!.colours;
    expect(settled.every((colour, index) => colour !== evidence.before[index])).toBe(true);
    for (const frame of evidence.frames) expect(frame.colours, `${frame.elapsed} ms`).toEqual(settled);
  });
}
