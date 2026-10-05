import { expect, test } from "@playwright/test";
import { writeFile } from "node:fs/promises";

for (const mode of ["light", "dark"]) {
  test(`new ${mode} setting switches retain glass while native geometry is pending`, async ({ page }) => {
    await page.goto(`/tests/ui/harness.html?theme=liquid-glass&mode=${mode}&nativeGlass=true`);
    await page.getByTitle("设置", { exact: true }).click();
    await expect(page.locator(".settings-view .group-header").first()).toBeVisible();
    await page.evaluate(() => (window as any).testApp.setGlassStatus("ready"));
    await expect.poll(() => page.evaluate(() => (window as any).testApp.getGlassGeometryInFlight())).toBe(0);
    const frames = await page.evaluate(async () => {
      const fixture = (window as any).testApp;
      fixture.delays.nativeGlassGeometry = 240;
      const group = document.querySelector<HTMLElement>(".settings-group")!;
      group.querySelector<HTMLElement>(".group-header")!.click();
      const samples: { pending: boolean; reflection: string; native: string | undefined }[] = [];
      const start = performance.now();
      await new Promise<void>(resolve => {
        const sample = () => {
          const lens = group.querySelector<HTMLElement>(".switch .glass-lens");
          if (lens) {
            const rect = lens.parentElement!.getBoundingClientRect();
            const applied = fixture.getAppliedGlassGeometry()?.controls.some((control: any) =>
              Math.abs(control.x - rect.x) < .01 && Math.abs(control.y - rect.y) < .01
              && Math.abs(control.width - rect.width) < .01 && Math.abs(control.height - rect.height) < .01);
            samples.push({ pending: !applied,
              reflection: getComputedStyle(lens.querySelector(".glass-lens-reflection")!).display,
              native: lens.dataset.nativeGlass });
          }
          if (performance.now() - start < 650) requestAnimationFrame(sample); else resolve();
        };
        requestAnimationFrame(sample);
      });
      return samples;
    });
    const file = test.info().outputPath("handoff-frames.json");
    await writeFile(file, JSON.stringify(frames, null, 2));
    await test.info().attach("handoff-frames", { path: file, contentType: "application/json" });
    const pending = frames.filter(frame => frame.pending);
    expect(pending.length).toBeGreaterThan(2);
    for (const frame of pending) expect(frame.reflection, JSON.stringify(frame)).toBe("block");
    expect(frames.at(-1)).toEqual({ pending: false, reflection: "none", native: "true" });
  });
}

test("a failed setting geometry update keeps the fallback until a successful update", async ({ page }) => {
  await page.goto("/tests/ui/harness.html?theme=liquid-glass&nativeGlass=true");
  await page.getByTitle("设置", { exact: true }).click();
  await expect(page.locator(".settings-view .group-header").first()).toBeVisible();
  await page.evaluate(() => (window as any).testApp.setGlassStatus("ready"));
  await expect.poll(() => page.evaluate(() => (window as any).testApp.getGlassGeometryInFlight())).toBe(0);
  const before = await page.evaluate(() => {
    const fixture = (window as any).testApp;
    fixture.failures.nativeGlassGeometry = true;
    return fixture.calls.filter((call: any) => call.command === "set_glass_geometry").length;
  });
  await page.getByRole("heading", { name: "常规设置", exact: true }).click();
  await expect.poll(() => page.evaluate(() => (window as any).testApp.calls.filter((call: any) => call.command === "set_glass_geometry").length)).toBeGreaterThan(before);
  await expect.poll(() => page.evaluate(() => (window as any).testApp.getGlassGeometryInFlight())).toBe(0);
  const reflection = page.locator(".switch .glass-lens-reflection").first();
  await expect(reflection).toHaveCSS("display", "block");
  await page.evaluate(() => { (window as any).testApp.failures.nativeGlassGeometry = false; });
  await page.setViewportSize({ width: 450, height: 589 });
  await expect(reflection).toHaveCSS("display", "none");
});

test("a late update cannot hide glass for repositioned setting switches", async ({ page }) => {
  await page.goto("/tests/ui/harness.html?theme=liquid-glass&nativeGlass=true");
  await page.getByTitle("设置", { exact: true }).click();
  await expect(page.locator(".settings-view .group-header").first()).toBeVisible();
  await page.evaluate(() => (window as any).testApp.setGlassStatus("ready"));
  await expect.poll(() => page.evaluate(() => (window as any).testApp.getGlassGeometryInFlight())).toBe(0);
  const result = await page.evaluate(async () => {
    const fixture = (window as any).testApp;
    fixture.delays.nativeGlassGeometry = 300;
    const heading = document.querySelector<HTMLElement>(".settings-group .group-header")!;
    const frame = () => new Promise<void>(resolve => requestAnimationFrame(() => resolve()));
    const completed = fixture.getCompletedGlassGeometryCount();
    heading.click();
    while (!fixture.getGlassGeometryInFlight()) await frame();
    const original = document.querySelector(".switch .glass-lens");
    heading.click();
    await frame();
    heading.click();
    while (!document.querySelector(".switch .glass-lens")) await frame();
    const current = document.querySelector(".switch .glass-lens")!;
    (current.parentElement as HTMLElement).style.transform = "translateX(-8px)";
    while (fixture.getCompletedGlassGeometryCount() === completed) await frame();
    return { remounted: current !== original,
      waitingForCurrent: fixture.getGlassGeometryInFlight() > 0,
      reflection: getComputedStyle(current.querySelector(".glass-lens-reflection")!).display };
  });
  expect(result).toEqual({ remounted: true, waitingForCurrent: true, reflection: "block" });
  await expect(page.locator(".switch .glass-lens-reflection").first()).toHaveCSS("display", "none");
});

test("settings refreshes restore the confirmed glass after a label remount", async ({ page }) => {
  await page.emulateMedia({ reducedMotion: "reduce" });
  await page.goto("/tests/ui/harness.html?theme=liquid-glass&nativeGlass=true");
  await page.getByTitle("设置", { exact: true }).click();
  await page.getByRole("heading", { name: "常规设置", exact: true }).click();
  await page.evaluate(() => (window as any).testApp.setGlassStatus("ready"));
  const lens = page.locator(".group-content .hint-icon-btn .glass-lens").first();
  await expect(lens).toHaveAttribute("data-native-glass", "true");
  const id = await lens.locator("filter").getAttribute("id");
  const geometry = await page.evaluate(() => (window as any).testApp.getAppliedGlassGeometry());
  await page.evaluate(() => (window as any).testApp.emit("settings-changed"));
  await expect.poll(() => lens.locator("filter").getAttribute("id")).not.toBe(id);
  await expect.poll(() => page.evaluate(() => (window as any).testApp.getAppliedGlassGeometry())).toEqual(geometry);
  await expect(lens).toHaveAttribute("data-native-glass", "true");
});

test("replacing a glass node at an unchanged footprint needs no new native geometry", async ({ page }) => {
  await page.emulateMedia({ reducedMotion: "reduce" });
  await page.goto("/tests/ui/harness.html?theme=liquid-glass&nativeGlass=true");
  await expect(page.locator(".history-item").first()).toBeVisible();
  await page.evaluate(async () => {
    // Reuse Vite's active module URL, including its hot-update version.
    const modulePath = performance.getEntriesByType("resource").map(entry => entry.name)
      .find(url => new URL(url).pathname === "/src/shared/lib/nativeGlass.ts")!;
    const { registerGlassLens } = await import(modulePath);
    const host = document.createElement("div");
    host.id = "remount-glass-fixture";
    host.style.cssText = "position:fixed;left:150px;top:150px;width:44px;height:22px;border-radius:11px;isolation:isolate;z-index:1000";
    host.innerHTML = '<span class="glass-lens"><span class="glass-lens-reflection"></span></span>';
    document.body.append(host);
    const unregister = registerGlassLens(host, host.firstElementChild, 11);
    (window as any).testApp.replaceGlassNode = () => {
      const replacement = host.cloneNode(true) as HTMLElement;
      delete (replacement.firstElementChild as HTMLElement).dataset.nativeGlass;
      unregister();
      host.replaceWith(replacement);
      registerGlassLens(replacement, replacement.firstElementChild, 11);
    };
    await (window as any).testApp.setGlassStatus("ready");
  });
  const lens = page.locator("#remount-glass-fixture > .glass-lens");
  await expect(lens).toHaveAttribute("data-native-glass", "true");
  await expect.poll(() => page.evaluate(() => (window as any).testApp.getGlassGeometryInFlight())).toBe(0);
  const completed = await page.evaluate(() => (window as any).testApp.getCompletedGlassGeometryCount());
  await page.evaluate(() => (window as any).testApp.replaceGlassNode());
  await expect(lens).toHaveAttribute("data-native-glass", "true");
  expect(await page.evaluate(() => (window as any).testApp.getCompletedGlassGeometryCount())).toBe(completed);
});

for (const theme of ["liquid-glass", "mica", "acrylic"]) for (const mode of ["light", "dark"]) {
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
    await page.goto(`/tests/ui/harness.html?theme=${theme}&mode=${mode}&nativeGlass=true`);
    await page.getByTitle("设置", { exact: true }).click();
    await expect(page.locator(".settings-view .group-header").first()).toBeVisible();
    await page.evaluate(() => (window as any).testApp.setGlassStatus("ready"));
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
