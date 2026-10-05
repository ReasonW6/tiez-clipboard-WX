import { test, expect, type Page } from "@playwright/test";

const geometry = (page: Page) => page.evaluate(() => (window as any).testApp.calls
  .filter((call: any) => call.command === "set_glass_geometry").at(-1)?.args.geometry);

for (const mode of ["light", "dark"]) for (const density of [0, 100]) {
  test(`selected glass metadata stays readable: ${mode}, density ${density}`, async ({ page }) => {
    await page.emulateMedia({ reducedMotion: "reduce" });
    await page.goto(`/tests/ui/harness.html?theme=liquid-glass&mode=${mode}&opacity=${density}&nativeGlass=true`);
    const card = page.locator(".history-item").first();
    await expect(card).toBeVisible();
    await card.hover();
    await page.keyboard.press("ArrowDown");
    await expect(card).toHaveClass(/selected/);
    await page.mouse.move(424, 588);
    const source = mode === "dark" ? 255 : 0;
    const tinted = await page.evaluate(source => {
      const style = getComputedStyle(document.documentElement);
      const alpha = Number(style.getPropertyValue("--surface-fill"));
      return style.getPropertyValue("--material-rgb").split(",").map(Number)
        .map(channel => Math.round(source * (1 - alpha) + channel * alpha));
    }, source);
    for (const state of ["failed", "resizing"]) {
      await page.evaluate(async ({ state, input }) => {
        document.documentElement.style.setProperty("background", `rgb(${input.join(",")})`, "important");
        await (window as any).testApp.setGlassStatus(state);
      }, { state, input: state === "failed" ? [source, source, source] : tinted });
      for (const selector of [".app-info", ".item-meta-right-info"]) {
        const label = card.locator(selector);
        const ink = await label.evaluate(element => getComputedStyle(element).color.match(/[\d.]+/g)!.slice(0, 3).map(Number));
        const visible = await label.screenshot({ animations: "disabled" });
        const erased = await page.addStyleTag({ content: `.history-item.selected ${selector}, .history-item.selected ${selector} * {-webkit-text-fill-color:transparent!important;text-shadow:none!important}` });
        const paper = await label.screenshot({ animations: "disabled" });
        await erased.evaluate(element => element.remove());
        const result = await page.evaluate(async ({ visible, paper, ink }) => {
          const decode = async (encoded: string) => {
            const image = new Image(); image.src = `data:image/png;base64,${encoded}`; await image.decode();
            const canvas = document.createElement("canvas"); canvas.width = image.width; canvas.height = image.height;
            const context = canvas.getContext("2d")!; context.drawImage(image, 0, 0);
            return context.getImageData(0, 0, canvas.width, canvas.height).data;
          };
          const luminance = (rgb: number[]) => rgb.map(value => value / 255).map(value => value <= .04045 ? value / 12.92 : ((value + .055) / 1.055) ** 2.4)
            .reduce((sum, value, index) => sum + value * [.2126, .7152, .0722][index], 0);
          const [front, back] = await Promise.all([decode(visible), decode(paper)]);
          const foreground = luminance(ink);
          let minimum = Infinity, pixels = 0;
          for (let i = 0; i < front.length; i += 4) {
            if (Math.max(...[0, 1, 2].map(channel => Math.abs(front[i + channel] - back[i + channel]))) <= 2) continue;
            const background = luminance([...back.slice(i, i + 3)]);
            minimum = Math.min(minimum, (Math.max(foreground, background) + .05) / (Math.min(foreground, background) + .05));
            pixels++;
          }
          return { minimum, pixels };
        }, { visible: visible.toString("base64"), paper: paper.toString("base64"), ink });
        expect(result.pixels, `${state} ${selector}`).toBeGreaterThan(10);
        expect(result.minimum, `${state} ${selector}`).toBeGreaterThanOrEqual(4.5);
      }
    }
  });
}

async function ready(page: Page) {
  await page.evaluate(() => (window as any).testApp.setGlassStatus("ready"));
  await expect(page.locator("body")).toHaveClass(/native-glass-ready/);
}

for (const mode of ["light", "dark"]) for (const width of [250, 425]) {
  test(`clipboard glass keeps a single surface and readable interactions: ${mode}, ${width}px`, async ({ page }) => {
    await page.setViewportSize({ width, height: 589 });
    await page.emulateMedia({ reducedMotion: "reduce" });
    await page.goto(`/tests/ui/harness.html?theme=liquid-glass&mode=${mode}&opacity=0&nativeGlass=true`);
    const card = page.locator(".history-item").first();
    const lens = card.locator(":scope > .glass-lens");
    await expect(card).toBeVisible();
    await expect(lens).toHaveCount(1);
    await expect(lens).toHaveAttribute("data-glass-material", "regular");
    await expect(card).toHaveCSS("border-top-color", "rgba(0, 0, 0, 0)");
    await expect(card).toHaveCSS("outline-style", "none");
    await expect(card).toHaveCSS("box-shadow", "none");
    await expect(lens.locator(".glass-lens-reflection")).not.toHaveCSS("display", "none");
    const bodyInk = await card.locator(".content-preview").evaluate(element => getComputedStyle(element).color);
    await ready(page);
    await expect(lens).toHaveAttribute("data-native-glass", "true");
    await expect(lens.locator(".glass-lens-reflection")).toHaveCSS("display", "none");
    await expect(lens.locator(".glass-lens-refraction")).toHaveCSS("display", "none");
    await page.mouse.move(width - 1, 580);
    const idle = await card.evaluate(element => getComputedStyle(element).backgroundColor);
    await card.hover();
    await expect.poll(() => card.evaluate(element => getComputedStyle(element).backgroundColor)).not.toBe(idle);
    await page.keyboard.press("ArrowDown");
    await expect(card).toHaveClass(/selected/);
    await expect(card).toHaveCSS("outline-style", "none");
    await expect(card).toHaveCSS("border-top-color", "rgba(0, 0, 0, 0)");
    expect(await card.evaluate(element => getComputedStyle(element).backgroundColor)).not.toBe(idle);
    const button = card.locator(".item-actions button").first();
    await button.focus();
    await expect(button).toHaveCSS("outline-style", "solid");
    expect(await card.locator(".content-preview").evaluate(element => {
      for (let current: HTMLElement | null = element as HTMLElement; current; current = current.parentElement) {
        if (getComputedStyle(current).filter !== "none") return false;
      }
      return true;
    })).toBe(true);
    await expect(card.locator(".content-preview")).toHaveCSS("color", bodyInk);
    expect(await page.evaluate(() => document.documentElement.scrollWidth)).toBeLessThanOrEqual(width);
    const box = await card.boundingBox();
    expect(box!.x).toBeGreaterThanOrEqual(0);
    expect(box!.x + box!.width).toBeLessThanOrEqual(width);
    await page.evaluate(() => (window as any).testApp.setGlassStatus("failed"));
    await expect(lens.locator(".glass-lens-reflection")).not.toHaveCSS("display", "none");
    await page.emulateMedia({ forcedColors: "active" });
    await expect(lens.locator(".glass-lens-reflection")).toHaveCSS("border-top-style", "solid");
  });
}

test("virtualized clipboard cards submit only unclipped geometry and unregister on removal", async ({ page }) => {
  await page.emulateMedia({ reducedMotion: "reduce" });
  await page.goto("/tests/ui/harness.html?theme=liquid-glass&mode=light&nativeGlass=true");
  await expect(page.locator(".history-item").first()).toBeVisible();
  await page.evaluate(async () => {
    const path = "/node_modules/@tauri-apps/api/core.js";
    const { invoke } = await import(path);
    for (let i = 0; i < 100; i++) await invoke("add_manual_item", { content: `Virtual glass card ${i}`, tags: ["工作"] });
    await (window as any).testApp.emit("clipboard-changed");
  });
  await ready(page);
  await expect(page.locator(".history-item .glass-lens").first()).toHaveAttribute("data-native-glass", "true");
  const initialCards = await page.locator(".history-item").count();
  expect(initialCards).toBeLessThan(20);
  const scroller = page.locator("[data-virtuoso-scroller]");
  await scroller.evaluate(element => { element.scrollTop = 12; });
  const firstLens = page.locator("#clipboard-item-1 > .glass-lens");
  await expect(firstLens).toHaveAttribute("data-native-glass", "false");
  await expect(firstLens.locator(".glass-lens-reflection")).not.toHaveCSS("display", "none");
  await scroller.evaluate(element => { element.scrollTop = 0; });
  await expect(firstLens).toHaveAttribute("data-native-glass", "true");
  await scroller.evaluate(element => { element.scrollTop = element.scrollHeight; });
  await expect(page.locator(".history-item").last()).toContainText("Virtual glass card");
  await expect.poll(async () => (await geometry(page))?.controls.length).toBeLessThanOrEqual(64);
  const rendered = await page.locator(".history-item").count();
  expect(rendered).toBeLessThan(20);
  const ineligible = page.locator(".history-item .glass-lens[data-native-glass='false']");
  if (await ineligible.count()) await expect(ineligible.first().locator(".glass-lens-reflection")).not.toHaveCSS("display", "none");
  await page.locator(".search-input").fill("no-such-card-529807");
  await expect(page.locator(".history-item")).toHaveCount(0);
  await expect.poll(async () => {
    const submitted = (await geometry(page))?.controls.length;
    const visibleLenses = await page.locator('.glass-lens[data-native-glass="true"]').count();
    return submitted === visibleLenses && visibleLenses >= 2;
  }).toBe(true);
});

test("mica and acrylic retain their original clipboard surfaces", async ({ page }) => {
  for (const theme of ["mica", "acrylic"]) {
    await page.goto(`/tests/ui/harness.html?theme=${theme}&mode=light`);
    await expect(page.locator("body")).toHaveClass(new RegExp(`theme-${theme}`));
    const card = page.locator(".history-item").first();
    await expect(card).toBeVisible();
    await expect(card.locator(".glass-lens")).toHaveCount(0);
    await expect(card).not.toHaveCSS("border-top-color", "rgba(0, 0, 0, 0)");
    await page.keyboard.press("ArrowDown");
    await expect(card).toHaveCSS("outline-style", "solid");
    await page.evaluate(() => localStorage.clear());
  }
});

test("the 64-shape budget keeps overflow glass visible and admits it when a shape unmounts", async ({ page }) => {
  await page.emulateMedia({ reducedMotion: "reduce" });
  await page.goto("/tests/ui/harness.html?theme=liquid-glass&mode=light&nativeGlass=true");
  await expect(page.locator(".history-item").first()).toBeVisible();
  await ready(page);
  // Card entrance animations can temporarily exclude their native surfaces.
  // Record the budget only after the fixture's six visible cards are admitted.
  await expect(page.locator(".history-item > .glass-lens[data-native-glass='true']")).toHaveCount(6);
  const originalCount = (await geometry(page)).controls.length;
  await page.evaluate(async () => {
    // Reuse the loaded module, including Vite's current HMR timestamp, so
    // these hosts join the same registry as the application's real lenses.
    const path = performance.getEntriesByType("resource").map(entry => entry.name)
      .find(url => new URL(url).pathname === "/src/shared/lib/nativeGlass.ts")!;
    const { registerGlassLens } = await import(path);
    const fixtures = Array.from({ length: 70 }, (_, index) => {
      const host = document.createElement("div");
      // The budget follows paint order. Put these fixtures above the app's
      // stacking context so the existing surfaces precede them deliberately.
      host.style.cssText = "position:fixed;z-index:100;left:10px;top:150px;width:80px;height:30px;border-radius:15px;isolation:isolate";
      host.dataset.budgetLens = String(index);
      host.innerHTML = '<span class="glass-lens"><span class="glass-lens-reflection"></span></span>';
      document.body.append(host);
      return { host, unregister: registerGlassLens(host, host.firstElementChild as HTMLElement, 15) };
    });
    (window as any).testGlassBudget = fixtures;
  });
  await expect.poll(async () => (await geometry(page)).controls.length).toBe(64);
  const overflow = page.locator("[data-budget-lens] .glass-lens[data-native-glass='false']");
  await expect(overflow).toHaveCount(70 + originalCount - 64);
  await expect(overflow.first().locator(".glass-lens-reflection")).not.toHaveCSS("display", "none");
  const firstOverflow = await overflow.first().evaluate(element => element.parentElement!.dataset.budgetLens);
  await page.evaluate(() => {
    const first = (window as any).testGlassBudget.shift();
    first.unregister();
    first.host.remove();
  });
  await expect(page.locator(`[data-budget-lens='${firstOverflow}'] .glass-lens`)).toHaveAttribute("data-native-glass", "true");
  expect((await geometry(page)).controls.length).toBe(64);
  await page.evaluate(() => {
    for (const fixture of (window as any).testGlassBudget) { fixture.unregister(); fixture.host.remove(); }
    delete (window as any).testGlassBudget;
  });
  await expect.poll(async () => (await geometry(page)).controls.length).toBe(originalCount);
});
