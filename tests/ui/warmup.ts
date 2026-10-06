import { chromium, type FullConfig } from "@playwright/test";

// The first harness load makes Vite pre-bundle dependencies, which can exceed a
// single test's timeout. Load it once before the suite starts.
export default async function warmup(config: FullConfig) {
  const { baseURL, launchOptions } = config.projects[0].use;
  const browser = await chromium.launch(launchOptions);
  try {
    const page = await browser.newPage();
    await page.goto(`${baseURL}/tests/ui/harness.html`, { timeout: 120_000 });
    await page.locator(".header-title").waitFor({ timeout: 120_000 });
  } finally {
    await browser.close();
  }
}
