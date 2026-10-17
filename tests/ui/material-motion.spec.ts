import { expect, test } from "@playwright/test";
import { writeFile } from "node:fs/promises";

for (const theme of ["mica", "acrylic"]) {
  test(`settings headings change ink together with the color mode: ${theme}`, async ({ page }) => {
    await page.goto(`/tests/ui/harness.html?theme=${theme}&mode=light&opacity=0`);
    await page.getByTitle("设置", { exact: true }).click();
    await page.getByRole("heading", { name: "界面设置", exact: true }).click();
    const samples = await page.evaluate(async () => {
      const parse = (value: string) => {
        const channels = value.match(/[\d.]+/g)!.map(Number);
        return [...channels.slice(0, 3), channels[3] ?? 1];
      };
      const over = (front: number[], back: number[]) => front.slice(0, 3)
        .map((value, i) => value * front[3] + back[i] * (1 - front[3])).concat(1);
      const luminance = (rgb: number[]) => rgb.slice(0, 3).map(value => value / 255)
        .map(value => value <= .04045 ? value / 12.92 : ((value + .055) / 1.055) ** 2.4)
        .reduce((sum, value, i) => sum + value * [.2126, .7152, .0722][i], 0);
      const frames: { mode: string; elapsed: number; heading: string; contrast: number }[] = [];
      for (const mode of ["深色", "浅色"]) {
        const choice = [...document.querySelectorAll<HTMLButtonElement>(".settings-inline-choice-btn")]
          .find(button => button.textContent === mode)!;
        choice.click();
        const start = performance.now();
        await new Promise<void>(resolve => {
          const sample = () => {
            for (const heading of document.querySelectorAll(".settings-group h3")) {
              const ancestors: Element[] = [];
              for (let current: Element | null = heading; current; current = current.parentElement) ancestors.unshift(current);
              const styles = ancestors.map(element => getComputedStyle(element));
              for (const backdrop of [0, 128, 255]) {
                let background = [backdrop, backdrop, backdrop, 1];
                for (const style of styles) background = over(parse(style.backgroundColor), background);
                const ink = parse(getComputedStyle(heading).color);
                ink[3] *= styles.reduce((alpha, style) => alpha * Number(style.opacity), 1);
                const light = [luminance(background), luminance(over(ink, background))].sort((a, b) => a - b);
                frames.push({ mode, elapsed: performance.now() - start, heading: heading.textContent!, contrast: (light[1] + .05) / (light[0] + .05) });
              }
            }
            if (performance.now() - start < 300) requestAnimationFrame(sample); else resolve();
          };
          requestAnimationFrame(sample);
        });
      }
      return frames;
    });
    await writeFile(test.info().outputPath("heading-contrast.json"), JSON.stringify(samples, null, 2));
    expect(samples.length).toBeGreaterThan(20);
    for (const sample of samples) expect(sample.contrast, JSON.stringify(sample)).toBeGreaterThanOrEqual(4.5);
  });
}

for (const theme of ["mica", "acrylic"]) for (const mode of ["light", "dark"]) {
  test(`pin control remains readable throughout state changes: ${theme} ${mode}`, async ({ page }) => {
    await page.emulateMedia({ reducedMotion: "no-preference" });
    await page.goto(`/tests/ui/harness.html?theme=${theme}&mode=${mode}&opacity=0`);
    const pin = page.locator(".header-actions .btn-icon").first();
    await expect(pin).toHaveClass(/active/);
    await pin.evaluate(async element => {
      await Promise.allSettled(element.getAnimations().map(animation => animation.finished));
    });
    const frames = await pin.evaluate(async element => {
      const button = element as HTMLButtonElement;
      const parse = (value: string) => {
        const c = value.match(/[\d.]+/g)!.map(Number);
        return [c[0], c[1], c[2], c[3] ?? 1];
      };
      const over = (front: number[], back: number[]) => front.slice(0, 3)
        .map((value, index) => value * front[3] + back[index] * (1 - front[3])).concat(1);
      const luminance = (color: number[]) => color.slice(0, 3).map(value => value / 255)
        .map(value => value <= .04045 ? value / 12.92 : ((value + .055) / 1.055) ** 2.4)
        .reduce((total, value, index) => total + value * [.2126, .7152, .0722][index], 0);
      const ancestors: Element[] = [];
      for (let current: Element | null = button; current; current = current.parentElement) ancestors.unshift(current);
      const samples: { round: number; active: boolean; elapsed: number; contrast: number[]; color: string; background: string }[] = [];
      for (let round = 0; round < 2; ++round) {
        button.click();
        const start = performance.now();
        await new Promise<void>(resolve => {
          const sample = () => {
            const styles = ancestors.map(ancestor => getComputedStyle(ancestor));
            const css = getComputedStyle(button), ink = parse(css.color);
            ink[3] *= styles.reduce((opacity, style) => opacity * Number(style.opacity), 1);
            samples.push({ round, active: button.classList.contains("active"), elapsed: performance.now() - start,
              color: css.color, background: css.backgroundColor,
              contrast: [0, 128, 255].map(channel => {
                let backdrop = [channel, channel, channel, 1];
                for (const style of styles) backdrop = over(parse(style.backgroundColor), backdrop);
                const light = [luminance(backdrop), luminance(over(ink, backdrop))].sort((a, b) => a - b);
                return (light[1] + .05) / (light[0] + .05);
              }) });
            if (performance.now() - start < 260) requestAnimationFrame(sample); else resolve();
          };
          requestAnimationFrame(sample);
        });
      }
      return samples;
    });
    const framePath = test.info().outputPath("contrast-frames.json");
    await writeFile(framePath, JSON.stringify(frames, null, 2));
    await test.info().attach("contrast-frames", { path: framePath, contentType: "application/json" });
    for (let round = 0; round < 2; ++round) {
      const samples = frames.filter(frame => frame.round === round);
      expect(samples.length).toBeGreaterThan(2);
      expect(samples.at(-1)!.active).toBe(round === 1);
      for (const frame of samples) for (const contrast of frame.contrast)
        expect(contrast, JSON.stringify(frame)).toBeGreaterThanOrEqual(4.5);
    }
  });
}
