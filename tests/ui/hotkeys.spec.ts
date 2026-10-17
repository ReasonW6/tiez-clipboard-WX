import { test, expect, type Page } from "@playwright/test";

const recorder = (page: Page, label = "全局快捷键") => page.locator(".setting-item")
  .filter({ has: page.getByText(label, { exact: true }) }).locator(".key-group");

async function start(page: Page, query = "") {
  await page.goto(`/tests/ui/harness.html?${query}`);
  await expect(page.locator(".header-title")).toHaveText("TieZ");
  await page.getByTitle("设置", { exact: true }).click();
  await page.getByRole("heading", { name: "剪贴板设置", exact: true }).click();
}

async function nativeEvent(page: Page, name: string, payload?: string) {
  await page.evaluate(({ name, payload }) => (window as any).testApp.emit(name, payload), { name, payload });
}

test("Win+V takeover restores the previous shortcut and survives a reload", async ({ page }) => {
  await start(page);
  const row = page.locator(".setting-item").filter({ has: page.getByText("使用 Win+V 唤起", { exact: true }) });
  const toggle = row.getByRole("checkbox");
  await row.locator(".toggle").click();
  await expect(recorder(page)).toHaveText("Win + V");
  await expect(toggle).toBeChecked();
  expect(await page.evaluate(() => (window as any).testApp.calls.some((call: any) =>
    call.command === "test_hotkey_available" && call.args.hotkey === "Win+V"))).toBe(false);
  await page.reload();
  await page.getByTitle("设置", { exact: true }).click();
  await page.getByRole("heading", { name: "剪贴板设置", exact: true }).click();
  await expect(toggle).toBeChecked();
  await row.locator(".toggle").click();
  await expect(recorder(page)).toHaveText("Alt + V");
  await expect.poll(() => page.evaluate(() => (window as any).testApp.settings["app.use_win_v_shortcut"])).toBe("false");
});

for (const confirmed of [false, true]) {
  test(`Win+V asks before restarting Explorer (confirmed: ${confirmed})`, async ({ page }) => {
    await start(page, `winVChanged=true&confirmExplorerRestart=${confirmed}`);
    await page.locator(".setting-item").filter({ has: page.getByText("使用 Win+V 唤起", { exact: true }) }).locator(".toggle").click();
    await expect(recorder(page)).toHaveText("Win + V");
    const commands = await page.evaluate(() => (window as any).testApp.calls.map((call: any) => call.command));
    expect(commands).toContain("plugin:dialog|ask");
    expect(commands.includes("restart_explorer")).toBe(confirmed);
  });
}

for (const label of ["全局快捷键", "带格式粘贴快捷键", "搜索快捷键", "顺序粘贴快捷键"]) {
  test(`${label}: records and cancels when clicking elsewhere`, async ({ page }) => {
    await start(page);
    if (label === "顺序粘贴快捷键") {
      await page.locator(".setting-item").filter({ has: page.getByText("顺序粘贴模式", { exact: true }) }).locator(".toggle").click();
    }
    const field = recorder(page, label);
    await field.click();
    await expect(field).toHaveClass(/recording/);
    await page.mouse.click(8, 8);
    await expect(page.locator(".key-group.recording")).toHaveCount(0);
    await field.click();
    await page.keyboard.press("Control+Shift+Y");
    await expect(field).toHaveText("Ctrl + Shift + Y");
    await expect(page.locator(".key-group.recording")).toHaveCount(0);
  });
}

test("native Win chord releases the recorder before checking availability", async ({ page }) => {
  await start(page);
  const field = recorder(page);
  await field.click();
  await expect.poll(() => page.evaluate(() => (window as any).testApp.calls.at(-1)?.args?.enabled)).toBe(true);
  await nativeEvent(page, "hotkey-recorded", "Win+Y");
  await expect(field).toHaveText("Win + Y");
  const calls = await page.evaluate(() => (window as any).testApp.calls);
  const check = calls.findIndex((call: any) => call.command === "test_hotkey_available" && call.args.hotkey === "Win+Y");
  expect(calls.slice(0, check).findLast((call: any) => call.command === "set_recording_mode").args.enabled).toBe(false);
});

test("native recording is ready for an immediate key and only saves it once", async ({ page }) => {
  await start(page, "immediateHotkey=Ctrl%2BShift%2BY");
  await recorder(page).click();
  await expect(recorder(page)).toHaveText("Ctrl + Shift + Y");
  const saves = await page.evaluate(() => (window as any).testApp.calls.filter((call: any) => call.command === "register_hotkey" && call.args.hotkey === "Ctrl+Shift+Y").length);
  expect(saves).toBe(1);
});

test("native Backspace clears instead of registering a Backspace shortcut", async ({ page }) => {
  await start(page);
  await recorder(page).click();
  await nativeEvent(page, "hotkey-recorded", "Backspace");
  await expect(recorder(page)).toHaveText("未设置");
});

test("switching fields leaves exactly one recorder and updates only that field", async ({ page }) => {
  await start(page);
  await recorder(page).click();
  await recorder(page, "搜索快捷键").click();
  await expect(page.locator(".key-group.recording")).toHaveCount(1);
  await nativeEvent(page, "hotkey-recorded", "Ctrl+Shift+Y");
  await expect(recorder(page, "搜索快捷键")).toHaveText("Ctrl + Shift + Y");
  await expect(recorder(page)).toHaveText("Alt + V");
});

test("window blur and collapsing settings both stop native recording", async ({ page }) => {
  await start(page);
  await recorder(page).click();
  await page.evaluate(() => window.dispatchEvent(new Event("blur")));
  await expect(page.locator(".key-group.recording")).toHaveCount(0);
  await recorder(page).click();
  await page.getByRole("heading", { name: "剪贴板设置", exact: true }).click();
  await expect.poll(() => page.evaluate(() => (window as any).testApp.calls.filter((call: any) => call.command === "set_recording_mode").at(-1)?.args.enabled)).toBe(false);
});

test("an unavailable shortcut keeps the saved shortcut", async ({ page }) => {
  await start(page);
  await recorder(page).click();
  await page.keyboard.press("Control+Shift+X");
  await expect(recorder(page)).toHaveText("Alt + V");
  await expect(page.locator(".key-group.recording")).toHaveCount(0);
});

test("modifier order cannot bypass the default rich paste shortcut conflict", async ({ page }) => {
  await start(page);
  await recorder(page).click();
  await page.keyboard.press("Alt+Shift+V");
  await expect(page.locator(".key-group.recording")).toHaveCount(0);
  await expect(recorder(page)).toHaveText("Alt + V");
  await expect(recorder(page, "带格式粘贴快捷键")).toHaveText("Alt + Shift + V");
  expect(await page.evaluate(() => (window as any).testApp.calls.some((call: any) =>
    call.command === "register_hotkey" && call.args.hotkey === "Shift+Alt+V"))).toBe(false);
});

test("blur cancels a recording start while its focus request is pending", async ({ page }) => {
  await start(page, "focusDelay=250");
  await recorder(page).click();
  await page.evaluate(() => window.dispatchEvent(new Event("blur")));
  await expect.poll(() => page.evaluate(() => (window as any).testApp.getFocusRequestsCompleted())).toBe(1);
  await expect(page.locator(".key-group.recording")).toHaveCount(0);
  expect(await page.evaluate(() => (window as any).testApp.calls.some((call: any) =>
    call.command === "set_recording_mode" && call.args.enabled))).toBe(false);
});

test("Win+V restores a custom previous shortcut", async ({ page }) => {
  await start(page);
  await recorder(page).click();
  await page.keyboard.press("Control+Alt+J");
  await expect(recorder(page)).toHaveText("Ctrl + Alt + J");
  const toggle = page.locator(".setting-item").filter({ has: page.getByText("使用 Win+V 唤起", { exact: true }) }).locator(".toggle");
  await toggle.click();
  await expect(recorder(page)).toHaveText("Win + V");
  await toggle.click();
  await expect(recorder(page)).toHaveText("Ctrl + Alt + J");
});

for (const label of ["全局快捷键", "带格式粘贴快捷键", "搜索快捷键", "顺序粘贴快捷键"]) {
  test(`${label}: clearing stays cleared after reload`, async ({ page }) => {
    await start(page);
    if (label === "顺序粘贴快捷键") {
      await page.locator(".setting-item").filter({ has: page.getByText("顺序粘贴模式", { exact: true }) }).locator(".toggle").click();
    }
    await recorder(page, label).click();
    await page.keyboard.press("Backspace");
    await expect(recorder(page, label)).toHaveText("未设置");
    await page.reload();
    await page.getByTitle("设置", { exact: true }).click();
    await page.getByRole("heading", { name: "剪贴板设置", exact: true }).click();
    await expect(recorder(page, label)).toHaveText("未设置");
  });
}
