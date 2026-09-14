import { test, expect, type Page } from "@playwright/test";

const pageErrors = new WeakMap<Page, string[]>();
test.beforeEach(({ page }) => {
  const errors: string[] = [];
  pageErrors.set(page, errors);
  page.on("pageerror", error => errors.push(error.message));
});
test.afterEach(({ page }) => { expect(pageErrors.get(page)).toEqual([]); });

async function start(page: Page, query = "") {
  await page.goto(`/tests/ui/harness.html${query}`);
  await expect(page.locator(".header-title")).toHaveText("TieZ");
  await expect.poll(() => page.evaluate(() => (window as any).testApp.calls.some((c: any) => c.command === "set_theme"))).toBe(true);
}
async function openTags(page: Page) {
  await page.getByTitle("标签管理", { exact: true }).click();
  await expect(page.locator(".tm-card")).toHaveCount(4);
  await expect(page.locator(".themed-tag-manager").locator("..")).toHaveCSS("opacity", "1");
}
async function appearance(page: Page) {
  await page.getByTitle("设置", { exact: true }).click();
  await page.getByRole("heading", { name: "界面设置", exact: true }).click();
}
const commandCalls = (page: Page, command: string) => page.evaluate(command =>
  (window as any).testApp.calls.filter((c: any) => c.command === command), command);

for (const theme of ["mica", "acrylic", "liquid-glass", "retro", "sticky-note", "paper", "sakura"]) {
  test(`${theme}: explicit light survives a dark system theme`, async ({ page }) => {
    await start(page, `?theme=${theme}&mode=light`);
    await expect(page.locator("body")).toHaveClass(new RegExp(`light-mode`));
    await expect.poll(() => page.evaluate(() => (window as any).testApp.getTheme())).toBe("light");
    const before = await page.locator("#root").evaluate(root => ({
      background: getComputedStyle(root).backgroundColor, color: getComputedStyle(root).color
    }));
    await page.evaluate(() => (window as any).testApp.emit("tauri://theme-changed", "dark"));
    await page.emulateMedia({ colorScheme: "light" });
    await page.emulateMedia({ colorScheme: "dark" });
    await expect(page.locator("body")).toHaveClass(/light-mode/);
    expect(await page.locator("#root").evaluate(root => ({ background: getComputedStyle(root).backgroundColor, color: getComputedStyle(root).color }))).toEqual(before);
    const calls = await commandCalls(page, "set_theme");
    expect(calls.at(-1).args).toMatchObject({ theme, colorMode: "light", showAppBorder: true });
    expect(calls.at(-1).args).not.toHaveProperty("color_mode");
  });
}

test("explicit dark, rapid mode changes and returning to system stay consistent", async ({ page }) => {
  await page.emulateMedia({ colorScheme: "light" });
  await start(page, "?mode=dark");
  await expect.poll(() => page.evaluate(() => (window as any).testApp.getTheme())).toBe("dark");
  await expect(page.locator("body")).toHaveClass(/dark-mode/);
  await appearance(page);
  await page.getByRole("button", { name: "浅色", exact: true }).click();
  await page.getByRole("button", { name: "深色", exact: true }).click();
  await page.getByRole("button", { name: "浅色", exact: true }).click();
  await expect.poll(() => page.evaluate(() => (window as any).testApp.getTheme())).toBe("light");
  await page.getByRole("button", { name: "跟随系统", exact: true }).click();
  await expect(page.locator("body")).toHaveClass(/dark-mode/);
  await expect.poll(() => page.evaluate(() => (window as any).testApp.getTheme())).toBe("dark");
});

test("Windows 10 light fallback does not inherit the system's dark background", async ({ page }) => {
  await start(page, "?win10=true&mode=light");
  await expect(page.locator("body")).toHaveClass(/windows-10/);
  expect(await page.locator("#root").evaluate(el => getComputedStyle(el).backgroundColor)).toBe("rgb(243, 245, 248)");
});

test("hide resets settings, tag dialogs, search and selection; blur alone preserves the page", async ({ page }) => {
  await start(page);
  await appearance(page);
  await page.getByTitle("隐藏", { exact: true }).click();
  await expect(page.locator(".header-title")).toHaveText("TieZ");
  await page.getByPlaceholder("搜索剪切板...").fill("query");
  await openTags(page);
  await page.evaluate(() => (window as any).testApp.emit("tauri://blur"));
  await expect(page.locator(".header-title")).toHaveText("标签管理");
  await page.getByRole("button", { name: "新建标签", exact: true }).click();
  await expect(page.getByRole("dialog")).toBeVisible();
  await page.evaluate(() => (window as any).testApp.emit("main-window-hidden"));
  await expect(page.locator(".header-title")).toHaveText("TieZ");
  await expect(page.getByRole("dialog")).toHaveCount(0);
  await expect(page.getByPlaceholder("搜索剪切板...")).toHaveValue("");
  await expect(page.locator("body")).toHaveClass(/theme-acrylic/);
  await openTags(page);
  await expect(page.getByRole("dialog")).toHaveCount(0);
});

for (const width of [250, 320, 352, 425, 780]) {
  test(`tag layout keeps controls and counts usable at ${width}px`, async ({ page }) => {
    await page.setViewportSize({ width, height: width === 250 ? 300 : width === 352 ? 380 : 589 });
    await start(page);
    await openTags(page);
    const geometry = await page.evaluate(() => {
      const content = document.querySelector(".tm-content")!.getBoundingClientRect();
      const sidebar = document.querySelector(".tm-sidebar")!.getBoundingClientRect();
      const buttons = [...document.querySelectorAll(".tm-toolbar button, .tm-content-heading button")].map(button => {
        const r = button.getBoundingClientRect(); return { left: r.left, right: r.right, width: r.width };
      });
      return { content: { top: content.top, left: content.left, width: content.width }, sidebar: { bottom: sidebar.bottom, right: sidebar.right }, buttons, scrollWidth: document.documentElement.scrollWidth };
    });
    expect(geometry.scrollWidth).toBeLessThanOrEqual(width);
    for (const rect of geometry.buttons) { expect(rect.left).toBeGreaterThanOrEqual(0); expect(rect.right).toBeLessThanOrEqual(width); expect(rect.width).toBeGreaterThanOrEqual(24); }
    if (width < 620) { expect(geometry.content.top).toBeGreaterThan(geometry.sidebar.bottom); expect(geometry.content.width).toBeGreaterThan(width - 36); }
    else expect(geometry.content.left).toBeGreaterThan(geometry.sidebar.right);
    await page.getByRole("button", { name: "卡片视图", exact: true }).click();
    expect(await page.locator(".tm-items-grid").evaluate(el => el.scrollWidth <= el.clientWidth)).toBe(true);
    const longTag = page.locator(".tm-tag").filter({ hasText: "一个很长的标签" });
    await longTag.scrollIntoViewIfNeeded();
    expect(await longTag.evaluate(el => {
      const chip = el.getBoundingClientRect(), count = el.querySelector(".tm-count")!.getBoundingClientRect();
      return count.left >= chip.left && count.right <= chip.right;
    })).toBe(true);
  });
}

test("out-of-order tag reads cannot replace the active tag's items", async ({ page }) => {
  await start(page);
  await openTags(page);
  await page.evaluate(() => { (window as any).testApp.delays["工作"] = 350; });
  await page.locator(".tm-tag").filter({ hasText: "工作" }).click();
  await page.locator(".tm-tag").filter({ hasText: "代码" }).click();
  await expect(page.locator(".tm-current-tag h2")).toHaveText("代码");
  await expect(page.locator(".tm-card")).toHaveCount(1);
  await expect(page.locator(".tm-text")).toContainText("const greeting");
  await page.waitForTimeout(400); // Let the deliberately delayed obsolete request finish.
  await expect(page.locator(".tm-text")).toContainText("const greeting");
  await page.evaluate(() => { (window as any).testApp.failures["代码"] = true; return (window as any).testApp.emit("clipboard-changed"); });
  await expect(page.locator(".tm-error")).toBeVisible();
  await expect(page.locator(".tm-card")).toHaveCount(1);
});

test("paste preserves IDs; edit/delete actions do not trigger a paste", async ({ page }) => {
  await start(page);
  await openTags(page);
  const before = await page.evaluate(() => (window as any).testApp.getEntries());
  await page.locator(".tm-paste-target").first().click();
  await expect.poll(async () => (await commandCalls(page, "copy_to_clipboard")).length).toBe(1);
  const paste = (await commandCalls(page, "copy_to_clipboard"))[0];
  expect(paste.args).toMatchObject({ id: 1, deleteAfterUse: false, paste: true });
  expect(await commandCalls(page, "delete_clipboard_entry")).toHaveLength(0);
  expect(await page.evaluate(() => (window as any).testApp.getEntries().map((e: any) => ({ id: e.id, tags: e.tags })))).toEqual(before.map((e: any) => ({ id: e.id, tags: e.tags })));
  await page.locator(".tm-card").first().getByRole("button", { name: "编辑条目内容", exact: true }).click();
  await expect(page.getByRole("dialog")).toBeVisible();
  expect(await commandCalls(page, "copy_to_clipboard")).toHaveLength(1);
  await page.getByRole("button", { name: "取消", exact: true }).click();
  await page.locator(".tm-card").first().getByRole("button", { name: "删除", exact: true }).click();
  await expect(page.getByRole("dialog")).toBeVisible();
  expect(await commandCalls(page, "delete_clipboard_entry")).toHaveLength(0);
  expect(await commandCalls(page, "copy_to_clipboard")).toHaveLength(1);
});

test("one glass transparency slider updates the material and persists", async ({ page }) => {
  await start(page, "?theme=liquid-glass");
  await appearance(page);
  const transparency = page.getByRole("slider", { name: "液态玻璃透明度" });
  await expect(page.getByRole("slider", { name: "玻璃模糊度" })).toHaveCount(0);
  await transparency.fill("80");
  await expect.poll(() => page.evaluate(() => getComputedStyle(document.body).getPropertyValue("--surface-opacity"))).toBe("0.2");
  const fill = await page.locator("#root").evaluate(el => getComputedStyle(el).backgroundColor);
  expect(await page.evaluate(() => (window as any).testApp.settings["app.surface_opacity"])).toBe("20");
  await page.reload();
  await expect(page.locator(".header-title")).toHaveText("TieZ");
  await expect(page.locator("#root")).toHaveCSS("background-color", fill);
  await openTags(page);
  await expect(page.locator(".tm-search")).toHaveCSS("backdrop-filter", "none");
  await expect(page.locator(".tm-text").first()).toHaveCSS("text-shadow", "none");
  await page.emulateMedia({ reducedMotion: "reduce" });
  expect(await page.locator(".tm-add").evaluate(el => getComputedStyle(el).transitionDuration)).toBe("0s");
});


test("tag CRUD preserves renamed items and confirms deleting a tag and its items", async ({ page }) => {
  await start(page);
  await openTags(page);
  await page.getByRole("button", { name: "新建标签", exact: true }).click();
  await page.getByRole("dialog").getByRole("textbox").fill("新分类");
  await page.getByRole("dialog").getByRole("button", { name: "保存", exact: true }).click();
  await expect(page.locator(".tm-current-tag h2")).toHaveText("新分类");
  await page.locator(".tm-add").click();
  await page.getByRole("dialog").getByRole("textbox").fill("我的常用文本");
  await page.getByRole("dialog").getByRole("button", { name: "保存", exact: true }).click();
  await expect(page.locator(".tm-text")).toHaveText("我的常用文本");
  await page.getByRole("button", { name: "重命名标签", exact: true }).click();
  await page.getByRole("dialog").getByRole("textbox").fill("重命名分类");
  await page.getByRole("dialog").getByRole("button", { name: "保存", exact: true }).click();
  await expect(page.locator(".tm-current-tag h2")).toHaveText("重命名分类");
  await expect(page.locator(".tm-text")).toHaveText("我的常用文本");
  await page.getByRole("button", { name: "删除标签", exact: true }).click();
  await expect(page.getByRole("dialog")).toContainText("所有条目都将被永久删除");
  expect((await page.evaluate(() => (window as any).testApp.getEntries())).some((e: any) => e.content === "我的常用文本")).toBe(true);
  await page.getByRole("dialog").getByRole("button", { name: "删除", exact: true }).click();
  expect(await commandCalls(page, "delete_clipboard_entry")).toHaveLength(0);
  expect((await page.evaluate(() => (window as any).testApp.getEntries())).some((e: any) => e.content === "我的常用文本")).toBe(false);
});


test("glass uses solid surfaces when transparency is reduced, including dark mode", async ({ page }) => {
  await start(page, "?theme=liquid-glass&mode=dark");
  const cdp = await page.context().newCDPSession(page);
  await cdp.send("Emulation.setEmulatedMedia", { features: [{ name: "prefers-reduced-transparency", value: "reduce" }] });
  expect(await page.locator("#root").evaluate(el => getComputedStyle(el).backgroundColor)).toBe("rgb(35, 39, 49)");
  await openTags(page);
  expect(await page.locator(".tm-search").evaluate(el => getComputedStyle(el, "::before").backdropFilter)).toBe("none");
});


for (const theme of ["mica", "acrylic", "liquid-glass", "retro", "sticky-note", "paper", "sakura"]) {
  test(`${theme}: explicit dark stays dark in a light system`, async ({ page }) => {
    await page.emulateMedia({ colorScheme: "light" });
    await start(page, `?theme=${theme}&mode=dark`);
    await expect(page.locator("body")).toHaveClass(/dark-mode/);
    await expect.poll(() => page.evaluate(() => (window as any).testApp.getTheme())).toBe("dark");
    const color = await page.locator("#root").evaluate(el => getComputedStyle(el).color);
    await page.evaluate(() => (window as any).testApp.emit("tauri://theme-changed", "light"));
    await expect(page.locator("body")).toHaveClass(/dark-mode/);
    expect(await page.locator("#root").evaluate(el => getComputedStyle(el).color)).toBe(color);
  });
}


test("editing and bulk pasting use full content rather than a truncated tag preview", async ({ page }) => {
  await start(page);
  await page.evaluate(() => (window as any).testApp.setContent(1, "x".repeat(51000) + "END-OF-FULL-CONTENT"));
  await openTags(page);
  await page.locator(".tm-card").first().getByRole("button", { name: "编辑条目内容", exact: true }).click();
  const content = await page.getByRole("dialog").getByRole("textbox").inputValue();
  expect(content).toHaveLength(51019);
  expect(content).toMatch(/END-OF-FULL-CONTENT$/);
  await page.getByRole("dialog").getByRole("button", { name: "取消", exact: true }).click();
  await page.getByRole("button", { name: "管理", exact: true }).click();
  await page.locator(".tm-paste-target").first().click();
  await page.locator(".tm-batch-bar").getByRole("button", { name: "粘贴", exact: true }).click();
  const calls = await commandCalls(page, "copy_to_clipboard");
  expect(calls.at(-1).args).toMatchObject({ id: 0, deleteAfterUse: false, content });
});


test("tag inputs reactivate the native window even when DOM focus is unchanged", async ({ page }) => {
  await start(page);
  await openTags(page);
  const input = page.getByRole("textbox", { name: "查找或创建...", exact: true });
  await input.focus();
  const before = (await commandCalls(page, "activate_window_focus")).length;
  await input.dispatchEvent("mousedown");
  await expect.poll(async () => (await commandCalls(page, "activate_window_focus")).length).toBe(before + 1);
  expect(await commandCalls(page, "copy_to_clipboard")).toHaveLength(0);
});
