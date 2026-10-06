import { defineConfig } from "@playwright/test";
const port = Number(process.env.TIEZ_MATERIAL_QA_PORT || 1420);
if (!Number.isInteger(port) || port < 1 || port > 65535) throw new Error("Invalid QA port");
const baseURL = `http://127.0.0.1:${port}`;
export default defineConfig({
  testDir: "./tests/ui", testMatch: "**/*.spec.ts", fullyParallel: false, workers: 1,
  globalSetup: "./tests/ui/warmup.ts",
  use: { baseURL, viewport: { width: 425, height: 589 }, colorScheme: "dark", screenshot: "only-on-failure",
    launchOptions: { executablePath: process.env.PLAYWRIGHT_CHROMIUM_EXECUTABLE_PATH } },
  outputDir: "artifacts/ui-tests",
  webServer: { command: `npm run dev -- --host 127.0.0.1 --port ${port}`, url: `${baseURL}/tests/ui/harness.html`, reuseExistingServer: !process.env.CI }
});
