import { test, expect, type Page } from "@playwright/test";

// Renders clipboard HTML through the real HtmlContent pipeline inside Chromium.
async function render(page: Page, html: string, preview = false) {
  return page.evaluate(async ({ html, preview }) => {
    const { renderRichContent } = await import("/src/shared/components/HtmlContent.tsx");
    const host = document.createElement("div");
    document.body.appendChild(host);
    renderRichContent(host, html, { preview });
    (window as any).lastHost = host;
    return host.shadowRoot!.innerHTML;
  }, { html, preview });
}

test.beforeEach(async ({ page }) => {
  await page.goto("/tests/ui/harness.html");
  await expect(page.locator(".header-title")).toHaveText("TieZ");
});

test("strips script vectors that the old attribute filter missed", async ({ page }) => {
  const out = await render(page, [
    `<p>keep me</p>`,
    `<iframe srcdoc="<script>parent.pwned=1</script>"></iframe>`,
    `<a href="java&#9;script:parent.pwned=2">tab</a>`,
    `<a href=" javascript:parent.pwned=3">space</a>`,
    `<img src="x" onerror="parent.pwned=4">`,
    `<svg><a xlink:href="javascript:parent.pwned=5"><text>svg</text></a></svg>`,
    `<form action="https://example.com"><button>go</button></form>`,
    `<base href="https://example.com/">`
  ].join(""));
  expect(out).toContain("keep me");
  for (const banned of ["<iframe", "srcdoc", "javascript", "onerror", "<form", "<button", "<base"]) {
    expect(out.toLowerCase()).not.toContain(banned);
  }
  await page.waitForTimeout(100);
  expect(await page.evaluate(() => (window as any).pwned)).toBeUndefined();
});

test("copied <style> blocks stay inside the preview", async ({ page }) => {
  const header = page.locator(".header-title");
  const before = await header.evaluate(el => getComputedStyle(el).color);
  await render(page, `<style>* { color: rgb(1, 2, 3) !important; } body { display: none !important; }
    .xl65 { color: rgb(4, 5, 6); }</style><table><tr><td class="xl65">cell</td></tr></table>`);
  expect(await header.evaluate(el => getComputedStyle(el).color)).toBe(before);
  await expect(header).toBeVisible();
  const cellColor = await page.evaluate(() =>
    getComputedStyle((window as any).lastHost.shadowRoot.querySelector("td")).color);
  // The copied stylesheet still formats its own content.
  expect(cellColor).toBe("rgb(1, 2, 3)");
});

test("leading Excel styles survive sanitizing", async ({ page }) => {
  await render(page, `<style>.xl65 { color: rgb(4, 5, 6); }</style><table><tr><td class="xl65">cell</td></tr></table>`);
  const cellColor = await page.evaluate(() =>
    getComputedStyle((window as any).lastHost.shadowRoot.querySelector("td")).color);
  expect(cellColor).toBe("rgb(4, 5, 6)");
});

test("links in previews never navigate the app window", async ({ page }) => {
  await render(page, `<p><a href="https://example.com/">site</a></p>`);
  const prevented = await page.evaluate(() => {
    const link = (window as any).lastHost.shadowRoot.querySelector("a");
    const event = new MouseEvent("click", { bubbles: true, cancelable: true, composed: true });
    link.dispatchEvent(event);
    return event.defaultPrevented;
  });
  expect(prevented).toBe(true);
  expect(page.url()).toContain("/tests/ui/harness.html");
});

test("unrenderable markup falls back to plain text", async ({ page }) => {
  await page.evaluate(async () => {
    const { renderRichContent } = await import("/src/shared/components/HtmlContent.tsx");
    const host = document.createElement("div");
    document.body.appendChild(host);
    renderRichContent(host, "<style>p{}</style>", { fallbackText: "plain fallback" });
    (window as any).lastHost = host;
  });
  expect(await page.evaluate(() => (window as any).lastHost.shadowRoot.textContent)).toContain("plain fallback");
});
