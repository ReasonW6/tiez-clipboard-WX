import { defineConfig } from "@playwright/test";
export default defineConfig({
  testDir: "./tests/ui", testMatch: "**/*.spec.ts", fullyParallel: false, workers: 1,
  use: { baseURL: "http://127.0.0.1:1420", viewport: { width: 425, height: 589 }, colorScheme: "dark", screenshot: "only-on-failure",
    launchOptions: { executablePath: process.env.PLAYWRIGHT_CHROMIUM_EXECUTABLE_PATH } },
  outputDir: "artifacts/ui-tests",
  webServer: { command: "npm run dev -- --host 127.0.0.1", url: "http://127.0.0.1:1420/tests/ui/harness.html", reuseExistingServer: !process.env.CI }
});
