import { test, expect, type Locator, type Page } from "@playwright/test";

async function readableInk(control: Locator) {
  return control.evaluate(element => {
    const rgba = (text: string) => { const values = text.match(/[\d.]+/g)!.map(Number); return [values[0], values[1], values[2], values[3] ?? 1]; };
    const over = (front: number[], back: number[]) => front.slice(0, 3).map((value, i) => value * front[3] + back[i] * (1 - front[3]));
    const luminance = (rgb: number[]) => rgb.map(v => v / 255).map(v => v <= .04045 ? v / 12.92 : ((v + .055) / 1.055) ** 2.4)
      .reduce((sum, value, i) => sum + value * [.2126, .7152, .0722][i], 0);
    const ancestors: Element[] = [];
    for (let node: Element | null = element; node; node = node.parentElement) ancestors.unshift(node);
    return [0, 128, 255].map(backdrop => {
      let paper = [backdrop, backdrop, backdrop], opacity = 1;
      for (const node of ancestors) {
        const css = getComputedStyle(node);
        opacity *= Number(css.opacity);
        const fill = rgba(css.backgroundColor); fill[3] *= opacity;
        paper = over(fill, paper);
        const reflection = node.querySelector(":scope > .glass-lens > .glass-lens-reflection");
        if (reflection && getComputedStyle(reflection).display !== "none") {
          const glass = rgba(getComputedStyle(reflection).backgroundColor); glass[3] *= opacity;
          paper = over(glass, paper);
        }
      }
      const ink = rgba(getComputedStyle(element).color); ink[3] *= opacity;
      const light = [luminance(paper), luminance(over(ink, paper))].sort((a, b) => a - b);
      return (light[1] + .05) / (light[0] + .05);
    });
  });
}

async function start(page: Page, theme = "liquid-glass", mode = "light") {
  await page.emulateMedia({ reducedMotion: "reduce" });
  await page.goto(`/tests/ui/harness.html?theme=${theme}&mode=${mode}&nativeGlass=true&opacity=0`);
  await page.getByTitle("设置", { exact: true }).click();
  await expect(page.locator(".settings-view")).toBeVisible();
}
const geometry = (page: Page) => page.evaluate(() => (window as any).testApp.calls
  .filter((call: any) => call.command === "set_glass_geometry").at(-1)?.args.geometry);
async function ready(page: Page) {
  await page.evaluate(() => (window as any).testApp.setGlassStatus("ready"));
  await expect(page.locator("body")).toHaveClass(/native-glass-ready/);
}

for (const mode of ["light", "dark"]) for (const width of [250, 425]) {
  test(`settings reading plane stays behind controls while the content scrolls: ${mode} ${width}px`, async ({ page }) => {
    await page.setViewportSize({ width, height: 589 });
    await start(page, "liquid-glass", mode);
    const plane = page.locator(".settings-reading-plane"), lens = plane.locator(":scope > .glass-lens");
    const scroller = page.locator(".settings-view");
    await expect(plane).toHaveCount(1);
    await expect(plane).toHaveCSS("pointer-events", "none");
    await expect(lens).toHaveAttribute("data-native-glass", "false");
    await expect(lens).toHaveAttribute("data-glass-material", "reading");
    await ready(page);
    await expect(lens).toHaveAttribute("data-native-glass", "true");
    await expect(lens.locator(".glass-lens-reflection")).toHaveCSS("display", "none");
    await expect(lens.locator(".glass-lens-shadow")).not.toHaveCSS("box-shadow", "none");
    const bounds = await plane.boundingBox();
    expect(bounds).not.toBeNull();
    expect(bounds!.height).toBeLessThan(589);

    const generalHeader = page.getByRole("heading", { name: "常规设置", exact: true }).locator("..");
    await expect(generalHeader.locator(":scope > .glass-lens")).toHaveAttribute("data-glass-material", "clear");
    // A control keeps its material when its dimensions change. Its size must
    // not turn a clear surface into a second reading plane.
    const headerStyle = await generalHeader.getAttribute("style");
    await generalHeader.evaluate(element => { (element as HTMLElement).style.height = "120px"; });
    await expect.poll(async () => {
      const bounds = await generalHeader.boundingBox();
      return (await geometry(page)).controls.find((control: any) => bounds
        && Math.abs(control.x - bounds.x) < .01 && Math.abs(control.y - bounds.y) < .01
        && Math.abs(control.width - bounds.width) < .01 && Math.abs(control.height - bounds.height) < .01)?.material;
    }).toBe("clear");
    await generalHeader.evaluate((element, style) => {
      if (style === null) element.removeAttribute("style"); else element.setAttribute("style", style);
    }, headerStyle);
    await generalHeader.click();
    const group = generalHeader.locator("..");
    const toggle = group.locator(".switch").first();
    const initial = await toggle.locator("input").isChecked();
    await toggle.click();
    await expect(toggle.locator("input")).toBeChecked({ checked: !initial });
    // The native painter chooses the last overlapping control. Text between
    // controls must hit the reading plane; the toggle must be painted after it.
    await expect.poll(() => page.evaluate(() => {
      const controls = (window as any).testApp.calls.filter((call: any) => call.command === "set_glass_geometry").at(-1)?.args.geometry.controls ?? [];
      const plane = document.querySelector<HTMLElement>(".settings-reading-plane")!.getBoundingClientRect();
      const label = document.querySelector<HTMLElement>(".settings-group .item-label")!.getBoundingClientRect();
      const toggle = document.querySelector<HTMLElement>(".settings-group .toggle")!.getBoundingClientRect();
      const at = (x: number, y: number) => controls.findLastIndex((control: any) => x > control.x && x < control.x + control.width && y > control.y && y < control.y + control.height);
      const planeIndex = controls.findIndex((control: any) => Math.abs(control.x - plane.x) < .01 && Math.abs(control.y - plane.y) < .01 && Math.abs(control.width - plane.width) < .01 && Math.abs(control.height - plane.height) < .01);
      const toggleIndex = at(toggle.x + toggle.width / 2, toggle.y + toggle.height / 2);
      return planeIndex >= 0 && controls[planeIndex].material === "reading"
        && at(label.x + label.width / 2, label.y + label.height / 2) === planeIndex
        && toggleIndex > planeIndex && controls[toggleIndex].material === "clear";
    })).toBe(true);
    await generalHeader.click();
    await page.getByRole("heading", { name: "界面设置", exact: true }).click();
    await expect(page.locator(".glass-material-slider")).toHaveCount(3);
    for (const fraction of [0, .5, 1, 0]) {
      await scroller.evaluate(async (element, fraction) => {
        element.scrollTop = (element.scrollHeight - element.clientHeight) * fraction;
        await new Promise<void>(resolve => requestAnimationFrame(() => requestAnimationFrame(() => resolve())));
      }, fraction);
      await expect(lens).toHaveAttribute("data-native-glass", "true");
      await expect(lens).toHaveAttribute("data-glass-material", "reading");
      expect(await plane.boundingBox()).toEqual(bounds);
      await expect.poll(async () => (await geometry(page)).controls.filter((control: any) =>
        Math.abs(control.x - bounds!.x) < .01 && Math.abs(control.y - bounds!.y) < .01
        && Math.abs(control.width - bounds!.width) < .01 && Math.abs(control.height - bounds!.height) < .01
        && control.material === "reading").length).toBe(1);
      await expect.poll(async () => (await geometry(page)).controls.filter((control: any) => control.material === "reading").length).toBe(1);
    }
    expect(await scroller.evaluate(element => element.scrollHeight > element.clientHeight)).toBe(true);
    expect(await page.locator(".main-content").evaluate(element => element.scrollTop)).toBe(0);
    expect(await page.evaluate(() => (window as any).testApp.settings["app.surface_opacity"])).toBe("0");
  });
}

test("settings reading plane follows native fallback and accessibility preferences", async ({ page }) => {
  await start(page);
  const lens = page.locator(".settings-reading-plane > .glass-lens");
  const reflection = lens.locator(".glass-lens-reflection");
  await expect(lens).toHaveAttribute("data-glass-material", "reading");
  await expect(lens.locator("svg, .glass-lens-refraction")).toHaveCount(0);
  await expect(reflection).toHaveCSS("box-shadow", "none");
  await expect(reflection).toHaveCSS("backdrop-filter", /blur\([\d.]+px\)/);
  await ready(page);
  for (const state of ["resizing", "suspended", "failed"]) {
    await page.evaluate(state => (window as any).testApp.setGlassStatus(state), state);
    await expect(lens.locator(".glass-lens-reflection")).not.toHaveCSS("display", "none");
    await expect(reflection).toHaveCSS("backdrop-filter", /blur\([\d.]+px\)/);
    await expect(lens.locator(".glass-lens-shadow")).not.toHaveCSS("box-shadow", "none");
    await ready(page);
  }
  const cdp = await page.context().newCDPSession(page);
  await cdp.send("Emulation.setEmulatedMedia", { features: [{ name: "prefers-reduced-transparency", value: "reduce" }] });
  await expect(lens).toHaveAttribute("data-native-glass", "false");
  await expect(lens.locator(".glass-lens-refraction")).toHaveCount(0);
  await expect(lens.locator(".glass-lens-reflection")).not.toHaveCSS("display", "none");
  await expect(reflection).toHaveCSS("backdrop-filter", "none");
  await cdp.send("Emulation.setEmulatedMedia", { features: [{ name: "prefers-reduced-transparency", value: "no-preference" }] });
  await expect(lens).toHaveAttribute("data-native-glass", "true");
  await page.emulateMedia({ forcedColors: "active" });
  await expect(lens).toHaveAttribute("data-native-glass", "false");
  await expect(lens.locator(".glass-lens-shadow")).toHaveCSS("box-shadow", "none");
  await expect(reflection).toHaveCSS("backdrop-filter", "none");
});

for (const mode of ["light", "dark"]) for (const width of [250, 425]) {
  test(`settings glass controls preserve toggle, focus and readable selection: ${mode} ${width}px`, async ({ page }) => {
    await page.setViewportSize({ width, height: 589 });
    await start(page, "liquid-glass", mode);
    const generalHeader = page.getByRole("heading", { name: "常规设置", exact: true }).locator("..");
    await expect(generalHeader.locator(":scope > .glass-lens")).toHaveCount(1);
    await generalHeader.click();
    const group = generalHeader.locator("..");
    const toggle = group.locator(".switch").first();
    await expect(toggle.locator(".toggle > .glass-lens")).toHaveCount(1);
    await expect(toggle.locator(".toggle")).toHaveCSS("width", "44px");
    await expect(toggle.locator(".toggle")).toHaveCSS("height", "22px");
    const initial = await toggle.locator("input").isChecked();
    await toggle.click();
    await expect(toggle.locator("input")).toBeChecked({ checked: !initial });
    await page.keyboard.press("Tab");
    await toggle.locator("input").focus();
    await expect(toggle.locator(".toggle")).toHaveCSS("outline-style", "solid");
    await ready(page);
    await expect(toggle.locator(".glass-lens")).toHaveAttribute("data-native-glass", "true");
    await expect(toggle.locator(".glass-lens-reflection")).toHaveCSS("display", "none");
    await toggle.locator("input").evaluate(element => { (element as HTMLInputElement).disabled = true; });
    await expect(toggle.locator(".glass-lens")).toHaveAttribute("data-native-glass", "false");
    await expect(toggle.locator(".glass-lens-reflection")).not.toHaveCSS("display", "none");
    await generalHeader.click();
    await page.getByRole("heading", { name: "界面设置", exact: true }).click();
    const selected = page.getByRole("button", { name: "液态玻璃", exact: true });
    await selected.scrollIntoViewIfNeeded();
    await expect(selected).toHaveAttribute("aria-pressed", "true");
    await expect(selected.locator(":scope > .glass-lens")).toHaveCount(1);
    await expect(selected).toHaveCSS("border-top-color", "rgba(0, 0, 0, 0)");
    await selected.focus();
    await expect(selected).toHaveCSS("outline-style", "solid");
    expect(await selected.locator(".theme-choice-title").evaluate(element => getComputedStyle(element).filter)).toBe("none");
    expect(await page.evaluate(() => document.documentElement.scrollWidth)).toBeLessThanOrEqual(width);
    expect((await geometry(page)).controls.length).toBeLessThanOrEqual(64);
    await page.evaluate(() => (window as any).testApp.setGlassStatus("suspended"));
    await expect(selected.locator(".glass-lens-reflection")).not.toHaveCSS("display", "none");
  });
}

test("all visible settings ranges use native tracks and retain complete fallback lenses", async ({ page }) => {
  await page.setViewportSize({ width: 425, height: 1400 });
  await start(page);
  await page.getByRole("heading", { name: "界面设置", exact: true }).click();
  await expect(page.locator(".glass-material-slider")).toHaveCount(3);
  await expect.poll(async () => (await geometry(page)).controls.filter((control: any) => control.track).length).toBe(3);
  await expect(page.locator(".glass-slider-thumb > .glass-lens[data-native-glass='true']")).toHaveCount(0);
  for (const track of await page.locator(".glass-material-slider").all()) {
    await expect(track).not.toHaveCSS("background-image", "none");
  }
  await ready(page);
  await expect(page.locator(".glass-slider-thumb > .glass-lens[data-native-glass='true']")).toHaveCount(3);
  for (const track of await page.locator(".glass-material-slider").all()) {
    await expect(track).toHaveCSS("background-image", "none");
    await expect(track.locator("..").locator(".glass-lens-reflection")).toHaveCSS("display", "none");
    await expect(track.locator("..").locator(".glass-lens-shadow")).not.toHaveCSS("box-shadow", "none");
  }
  const fontRange = page.locator(".settings-glass-range-input").first();
  await fontRange.focus();
  await page.keyboard.press("End");
  await expect(fontRange).toHaveValue("18");
  await expect.poll(() => page.evaluate(() => (window as any).testApp.settings["app.clipboard_item_font_size"])).toBe("18");
  await expect(fontRange.locator("..").locator(".glass-slider-thumb")).toHaveCSS("outline-style", "solid");
  await fontRange.locator("..").evaluate(element => { (element as HTMLElement).style.display = "none"; });
  await expect.poll(async () => (await geometry(page)).controls.filter((control: any) => control.track).length).toBe(2);
  await expect(page.locator(".glass-slider-thumb > .glass-lens[data-native-glass='true']")).toHaveCount(2);
  await expect(page.locator(".settings-glass-range-input").nth(1).locator("..").locator(".glass-lens")).toHaveAttribute("data-native-glass", "true");
  for (const state of ["resizing", "suspended", "failed"]) {
    await page.evaluate(state => (window as any).testApp.setGlassStatus(state), state);
    for (const track of await page.locator(".glass-material-slider:visible").all()) {
      await expect(track).not.toHaveCSS("background-image", "none");
      await expect(track.locator("..").locator(".glass-lens-reflection")).not.toHaveCSS("display", "none");
      await expect(track.locator("..").locator(".glass-lens-refraction")).not.toHaveCSS("display", "none");
      await expect(track.locator("..").locator(".glass-lens-shadow")).not.toHaveCSS("box-shadow", "none");
    }
    await ready(page);
    for (const track of await page.locator(".glass-material-slider:visible").all()) await expect(track).toHaveCSS("background-image", "none");
  }
  await page.emulateMedia({ forcedColors: "active" });
  await expect.poll(async () => (await geometry(page)).controls.length).toBe(0);
});

test("select fields, advanced rule inputs and an opaque app dialog keep their proper renderer", async ({ page }) => {
  await start(page);
  await page.getByRole("heading", { name: "剪贴板设置", exact: true }).click();
  const savedShortcut = page.locator(".key-cap-chord").first();
  await savedShortcut.scrollIntoViewIfNeeded();
  await expect(savedShortcut).toHaveCSS("border-top-color", "rgba(0, 0, 0, 0)");
  await ready(page);
  await expect(savedShortcut.locator(":scope > .glass-lens")).toHaveAttribute("data-native-glass", "true");
  await page.evaluate(() => (window as any).testApp.setGlassStatus("suspended"));
  await expect(savedShortcut.locator(".glass-lens-reflection")).not.toHaveCSS("display", "none");
  const select = page.locator(".settings-glass-field select");
  await select.scrollIntoViewIfNeeded();
  await select.selectOption("alt");
  await expect(select).toHaveValue("alt");
  await expect(select.locator("..").locator(".glass-lens")).toHaveCount(1);
  await page.getByRole("heading", { name: "高级设置", exact: true }).click();
  await page.locator(".advanced-add-rule-btn").click();
  const ruleInput = page.locator(".advanced-rule-input").first();
  await ruleInput.fill("Readable rule");
  await expect(ruleInput.locator("..").locator(".glass-lens")).toHaveCount(1);
  await expect(ruleInput.locator("..")).toHaveCSS("outline-style", "solid");
  await expect(page.locator(".advanced-workbench")).toHaveCSS("background-color", "rgba(0, 0, 0, 0)");
  await page.locator(".header-back").click();
  await page.getByRole("heading", { name: "默认打开程序", exact: true }).click();
  await page.evaluate(() => {
    const api = (window as any).__TAURI_INTERNALS__;
    const original = api.invoke;
    api.invoke = (command: string, ...args: any[]) => command === "get_associated_apps"
      ? Promise.resolve([{ name: "Fixture editor", path: "C:\\fictional\\editor.exe" }]) : original(command, ...args);
  });
  await page.getByRole("heading", { name: "默认打开程序", exact: true }).locator("../..").getByRole("button", { name: "更改", exact: true }).first().click();
  const modal = page.locator(".modal-content[data-glass-css-only]");
  await expect(modal).toBeVisible();
  await ready(page);
  await expect(modal.locator(".glass-lens[data-native-glass='true']")).toHaveCount(0);
  await expect(modal.locator(".settings-app-select__control .glass-lens-reflection")).not.toHaveCSS("display", "none");
  await modal.getByRole("combobox").click();
  const menu = page.locator(".settings-app-select__menu");
  await expect(menu.locator(":scope > .glass-lens")).toHaveAttribute("data-native-glass", "false");
  await expect(menu.locator(".glass-lens-reflection")).not.toHaveCSS("display", "none");
  await expect(menu).toHaveCSS("backdrop-filter", "none");
  await expect(menu.getByText("Fixture editor", { exact: true })).toBeVisible();
  await page.evaluate(() => (window as any).testApp.setGlassStatus("suspended"));
  await expect(menu.locator(".glass-lens-reflection")).not.toHaveCSS("display", "none");
});

test("mica settings keep their original DOM controls without glass decoration", async ({ page }) => {
  await start(page, "mica");
  await page.getByRole("heading", { name: "界面设置", exact: true }).click();
  await expect(page.locator(".settings-view .glass-lens")).toHaveCount(0);
  await expect(page.locator(".settings-reading-plane, .settings-glass-viewport")).toHaveCount(0);
  await expect(page.locator(".settings-glass-field, .settings-glass-range")).toHaveCount(0);
  await expect(page.locator(".settings-view input[type='range']")).toHaveCount(3);
});

test("expanded privacy choices, masking switch, textareas and data captions keep their glass surfaces", async ({ page }) => {
  await start(page);
  await page.getByRole("heading", { name: "剪贴板设置", exact: true }).click();
  await page.getByText("保护种类", { exact: true }).locator("../../..").locator("button").first().click();
  const choices = page.locator(".settings-glass-checkbox");
  await expect(choices).toHaveCount(6);
  for (const choice of await choices.all()) await expect(choice.locator(":scope > .glass-lens")).toHaveCount(1);
  const first = choices.first();
  await first.scrollIntoViewIfNeeded();
  const before = await first.locator("input").isChecked();
  await first.click();
  await expect(first.locator("input")).toBeChecked({ checked: !before });
  await page.getByText("自定义规则", { exact: true }).locator("../../..").locator("button").first().click();
  const rules = page.locator(".settings-glass-field textarea");
  await rules.fill("^fictional-only$");
  await expect(rules.locator("..").locator(".glass-lens")).toHaveCount(1);
  expect(await rules.evaluate(element => { const r = element.getBoundingClientRect(), p = element.parentElement!.getBoundingClientRect(); return r.left >= p.left && r.right <= p.right; })).toBe(true);
  await page.getByText("脱敏预览设置", { exact: true }).locator("..").locator("button").click();
  const maskRow = page.getByText("脱敏邮箱域名", { exact: true }).locator("../../../..");
  const mask = maskRow.locator(".switch");
  await expect(mask.locator(".toggle > .glass-lens")).toHaveCount(1);
  await expect(mask.locator(".toggle")).toHaveCSS("width", "44px");
  await mask.scrollIntoViewIfNeeded();
  await ready(page);
  await expect(mask.locator(".glass-lens")).toHaveAttribute("data-native-glass", "true");
  await page.getByRole("heading", { name: "高级设置", exact: true }).click();
  await page.locator(".advanced-add-rule-btn").click();
  const areas = page.locator(".advanced-rule-textarea");
  await expect(areas).toHaveCount(2);
  for (const area of await areas.all()) await expect(area.locator("..").locator(":scope > .glass-lens")).toHaveCount(1);
  await page.locator(".header-back").click();
  await page.getByRole("heading", { name: "默认打开程序", exact: true }).click();
  const panel = page.locator(".data-panel").first();
  await panel.scrollIntoViewIfNeeded();
  await expect(panel).toHaveCSS("overflow", "visible");
  await expect(panel.locator(".settings-data-text")).toHaveCSS("overflow", "hidden");
  await expect(panel.locator(".glass-lens-shadow")).not.toHaveCSS("box-shadow", "none");
});

for (const mode of ["light", "dark"]) test(`modal and footer glass preserve foreground contrast: ${mode}`, async ({ page }) => {
  await start(page, "liquid-glass", mode);
  for (const button of await page.locator(".settings-footer-meta button, .settings-danger-action").all()) {
    expect(Math.min(...await readableInk(button))).toBeGreaterThanOrEqual(4.5);
  }
  await page.evaluate(() => {
    const api = (window as any).__TAURI_INTERNALS__, original = api.invoke;
    api.invoke = (command: string, ...args: any[]) => command === "plugin:updater|check"
      ? Promise.resolve({ rid: 901, currentVersion: "0.3.4", version: "0.3.5", body: "Fictional update", rawJson: {} })
      : command === "get_associated_apps" ? Promise.resolve([{ name: "Fixture editor", path: "C:\\fictional\\editor.exe" }]) : original(command, ...args);
  });
  await page.getByRole("button", { name: "检查更新", exact: true }).click();
  await expect(page.locator(".update-install-btn")).toBeVisible();
  for (const button of await page.locator(".update-install-btn, .update-modal-cancel-btn").all()) {
    await expect.poll(async () => Math.min(...await readableInk(button))).toBeGreaterThanOrEqual(4.5);
  }
  await page.locator(".update-modal-cancel-btn").click();
  await page.getByRole("heading", { name: "默认打开程序", exact: true }).click();
  await page.getByRole("heading", { name: "默认打开程序", exact: true }).locator("../..").getByRole("button", { name: "更改", exact: true }).first().click();
  const modal = page.locator(".modal-content");
  // Buttons can meet their contrast target while the modal is still fading in.
  await expect(modal).toHaveCSS("opacity", "1");
  for (const name of ["取消", "浏览文件..."]) {
    await expect.poll(async () => Math.min(...await readableInk(modal.getByRole("button", { name, exact: true })))).toBeGreaterThanOrEqual(4.5);
  }
  expect(Math.min(...await readableInk(modal.locator(".settings-app-select__placeholder")))) .toBeGreaterThanOrEqual(4.5);
  await modal.getByRole("combobox").click();
  const option = page.locator(".settings-app-select__option").first();
  await option.hover();
  expect(Math.min(...await readableInk(option))).toBeGreaterThanOrEqual(4.5);
});
