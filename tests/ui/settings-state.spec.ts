import { expect, test } from "@playwright/test";
import { writeFile } from "node:fs/promises";

// Every settings change used to remount each labelled row.
test("settings refreshes keep labelled rows mounted", async ({ page }) => {
  await page.emulateMedia({ reducedMotion: "reduce" });
  await page.goto("/tests/ui/harness.html?theme=mica");
  await page.getByTitle("设置", { exact: true }).click();
  await page.getByRole("heading", { name: "常规设置", exact: true }).click();
  const hint = page.locator(".group-content .hint-icon-btn").first();
  await expect(hint).toBeVisible();
  const connected = await hint.evaluate(async element => {
    await (window as any).testApp.emit("settings-changed");
    await new Promise(resolve => setTimeout(resolve, 500));
    return element.isConnected;
  });
  expect(connected).toBe(true);
});

for (const theme of ["mica", "acrylic"]) for (const mode of ["light", "dark"]) {
  test(`first ${theme} ${mode} settings opening has no module-loading frame`, async ({ page }) => {
    // Amplify the existing cold module boundary without delaying settings data.
    await page.route(/\/SettingsPanel\.tsx(?:\?|$)/, async route => {
      await new Promise(resolve => setTimeout(resolve, 600));
      await route.continue();
    });
    await page.goto(`/tests/ui/harness.html?theme=${theme}&mode=${mode}`);
    await expect(page.getByTitle("设置", { exact: true })).toBeVisible();
    const frames = await page.evaluate(async () => {
      const result: { loading: boolean; groups: number }[] = [];
      document.querySelector<HTMLElement>('[title="设置"]')!.click();
      await new Promise<void>(resolve => {
        const start = performance.now();
        const sample = () => {
          result.push({ loading: !!document.querySelector(".settings-view .panel-loading"),
            groups: document.querySelectorAll(".settings-view .group-header").length });
          if (performance.now() - start < 900) requestAnimationFrame(sample); else resolve();
        };
        requestAnimationFrame(sample);
      });
      return result;
    });
    expect(frames.length).toBeGreaterThan(1);
    expect(frames.some(frame => frame.loading), JSON.stringify(frames)).toBe(false);
    expect(frames[0].groups).toBeGreaterThan(0);
  });

  test(`${theme} ${mode} expanded switches keep their saved paint from the first frame`, async ({ page }) => {
    await page.emulateMedia({ reducedMotion: "no-preference" });
    await page.goto(`/tests/ui/harness.html?theme=${theme}&mode=${mode}`);
    await page.getByTitle("设置", { exact: true }).click();
    await expect(page.locator(".settings-view .group-header").first()).toBeVisible();
    // The page itself fades in; sample the switches once it has settled.
    await expect(page.locator(".settings-view")).toHaveCSS("opacity", "1");
    const evidence = await page.evaluate(async () => {
      const reads = () => (window as any).testApp.calls.filter((call: any) => call.command === "get_settings").length;
      const beforeReads = reads();
      const result = [];
      for (const title of ["常规设置", "剪贴板设置", "界面设置"]) {
        const group = [...document.querySelectorAll<HTMLElement>(".settings-group")]
          .find(element => element.querySelector("h3")?.textContent === title)!;
        for (let round = 0; round < 2; ++round) {
          group.querySelector<HTMLElement>(".group-header")!.click();
          const frames: { elapsed: number; switches: { checked: boolean; opacity: number; track: string; thumb: string; position: string }[] }[] = [];
          const start = performance.now();
          await new Promise<void>(resolve => {
            const sample = () => {
              const content = group.querySelector(".group-content");
              if (content) frames.push({ elapsed: performance.now() - start,
                switches: [...content.querySelectorAll<HTMLInputElement>(".switch input")].map(input => {
                  const toggle = input.nextElementSibling!;
                  const track = getComputedStyle(toggle), thumb = getComputedStyle(toggle, "::before");
                  let opacity = Number(thumb.opacity);
                  for (let element: Element | null = toggle; element; element = element.parentElement)
                    opacity *= Number(getComputedStyle(element).opacity);
                  return { checked: input.checked, opacity, track: track.backgroundColor,
                    thumb: thumb.backgroundColor, position: thumb.transform };
                }) });
              if (performance.now() - start < 350) requestAnimationFrame(sample); else resolve();
            };
            requestAnimationFrame(sample);
          });
          result.push({ title, round, frames });
          group.querySelector<HTMLElement>(".group-header")!.click();
          await new Promise<void>(resolve => requestAnimationFrame(() => resolve()));
        }
      }
      return { beforeReads, afterReads: reads(), groups: result };
    });
    const file = test.info().outputPath("switch-frames.json");
    await writeFile(file, JSON.stringify(evidence, null, 2));
    await test.info().attach("switch-frames", { path: file, contentType: "application/json" });
    expect(evidence.afterReads).toBe(evidence.beforeReads);
    for (const group of evidence.groups) {
      expect(group.frames.length, group.title).toBeGreaterThan(1);
      const settled = group.frames.at(-1)!.switches;
      expect(settled.length, group.title).toBeGreaterThan(0);
      for (const frame of group.frames)
        expect(frame.switches, `${group.title}, round ${group.round}, ${frame.elapsed} ms`).toEqual(settled);
    }
  });
}
