import { expect, test, type Page } from "@playwright/test";

async function openAutostart(page: Page, query = "") {
  await page.goto(`/tests/ui/harness.html?${query}`);
  await page.getByTitle("设置", { exact: true }).click();
  await page.getByRole("heading", { name: "常规设置", exact: true }).click();
  return page.locator(".setting-item").filter({ has: page.getByText("开机自启动", { exact: true }) });
}

test("failed startup registration keeps the previous state and shows the error", async ({ page }) => {
  const row = await openAutostart(page);
  await row.locator(".toggle").click();
  await expect(row.getByRole("checkbox")).not.toBeChecked();
  await page.evaluate(() => { (window as any).testApp.failures.autostart = true; });
  await row.locator(".toggle").click();
  await expect(row.getByRole("checkbox")).not.toBeChecked();
  await expect(row.getByRole("alert")).toContainText("设置自启动失败");
  expect(await page.evaluate(() => (window as any).testApp.getAutostartEnabled())).toBe(false);
});

test("startup changes wait for native success and prevent overlapping requests", async ({ page }) => {
  const row = await openAutostart(page);
  await page.evaluate(() => { (window as any).testApp.delays.autostart = 500; });
  await row.locator(".toggle").click();
  await expect(row.getByRole("checkbox")).toBeDisabled();
  await expect(row.getByRole("checkbox")).toBeChecked();
  await expect(row.getByRole("checkbox")).toBeEnabled();
  await expect(row.getByRole("checkbox")).not.toBeChecked();
  expect(await page.evaluate(() => (window as any).testApp.getAutostartEnabled())).toBe(false);
});

test("an initial status query failure does not claim startup is enabled", async ({ page }) => {
  const row = await openAutostart(page, "autostartReadError=true");
  await expect(row.getByRole("checkbox")).not.toBeChecked();
});
