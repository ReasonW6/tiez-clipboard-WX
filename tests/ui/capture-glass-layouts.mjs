// Captures only fictional project content in headless Chromium. No desktop read.
import { chromium } from "playwright";
import { mkdir, readFile, writeFile } from "node:fs/promises";
import path from "node:path";
import { createHash } from "node:crypto";

const out = path.resolve(process.argv[2] || "artifacts/material-redesign/qa/offscreen-layouts");
const baseURL = `http://127.0.0.1:${process.env.TIEZ_MATERIAL_QA_PORT || 5173}`;
const viewport = { width: 425, height: 589 };
const compose = process.argv[3] === "compose";
await mkdir(out, { recursive: true });
// Keep the exported native parameter conversion tied to the actual Rust values.
const rust = await readFile("src-tauri/src/infrastructure/window_material.rs", "utf8");
for (const pattern of [/Material::Clear\s*=>\s*\(1\.5,\s*16\.0\)/,
  /\(0\.74\s*\+\s*density\s*\*\s*0\.16\)/, /\[23\.0,\s*31\.0,\s*43\.0\]/,
  /\[245\.0,\s*248\.0,\s*252\.0\]/]) {
  if (!pattern.test(rust)) throw new Error("Review the exported material parameters after the Rust profile changes");
}
const hashes = {};
for (const file of ["src-tauri/native/glass_renderer.hlsl", "src-tauri/native/glass_prefix.hlsli",
  "src-tauri/native/glass_optics.hlsli", "src-tauri/src/infrastructure/window_material.rs",
  "src/shared/lib/nativeGlass.ts", "src/shared/lib/glassPaintOrder.ts",
  "src/styles/themes/materials.css", "src/styles/themes/liquid-glass.css", "src/styles/themes/liquid-glass-settings.css",
  "src/features/app/components/AppMainContent.tsx", "src/features/settings/components/SettingsPanel.tsx",
  "src/features/clipboard/components/ClipboardItem.tsx",
  "tests/ui/capture-glass-layouts.mjs", "tests/native/glass_layout_render.cpp", "tests/native/glass_prefix_gpu.h",
  "tests/native/glass_gpu_regression.cpp", "tests/native/audit_glass_layouts.py"]) {
  hashes[file] = createHash("sha256").update(await readFile(file)).digest("hex");
}
const browser = await chromium.launch({ headless: true,
  executablePath: process.env.PLAYWRIGHT_CHROMIUM_EXECUTABLE_PATH });
const snapshots = [];
try {
  for (const mode of ["light", "dark"]) for (const density of [0, 50, 100]) {
    const context = await browser.newContext({ viewport, deviceScaleFactor: 1, reducedMotion: "reduce" });
    // Fail rather than silently capturing live external content.
    await context.route("**/*", route => new URL(route.request().url()).origin === baseURL
      ? route.continue() : route.abort());
    const page = await context.newPage();
    await page.goto(`${baseURL}/tests/ui/harness.html?theme=liquid-glass&mode=${mode}&opacity=${density}&nativeGlass=true`);
    await page.locator(".history-item").first().waitFor();
    await page.waitForFunction(() => window.testApp.getGlassStatus().state === "pending");
    await page.evaluate(() => window.testApp.setGlassStatus("ready"));
    const capture = async name => {
      // Framer Motion's page entrance may keep geometry unchanged while all
      // descendants are ineligible. Wait for that entrance before counting
      // stable frames; reduced-motion media alone does not stop this animation.
      await page.waitForFunction(() => !document.querySelector(".settings-view")
        || getComputedStyle(document.querySelector(".settings-view")).opacity === "1");
      // Require stable exported geometry rather than using an arbitrary timeout.
      await page.evaluate(async () => {
        const geometry = () => JSON.stringify(window.testApp.calls.filter(c => c.command === "set_glass_geometry").at(-1)?.args.geometry);
        let previous, stable = 0;
        for (let frame = 0; frame < 60; ++frame) {
          await new Promise(resolve => requestAnimationFrame(resolve));
          const current = geometry();
          stable = current && current === previous ? stable + 1 : 0; previous = current;
          if (stable >= 4) return;
        }
        throw new Error("Geometry did not settle");
      });
      const details = await page.evaluate(() => {
        const geometry = window.testApp.calls.filter(c => c.command === "set_glass_geometry").at(-1).args.geometry;
        const rect = r => ({ x: r.x, y: r.y, width: r.width, height: r.height });
        const texts = [], walker = document.createTreeWalker(document.querySelector("#root"), NodeFilter.SHOW_TEXT);
        for (let node; (node = walker.nextNode());) {
          if (!node.textContent.trim()) continue;
          const el = node.parentElement, css = getComputedStyle(el), range = document.createRange();
          if (css.display === "none" || css.visibility !== "visible" || el.closest("script,style,svg")) continue;
          range.selectNodeContents(node);
          const rects = [...range.getClientRects()].filter(r => r.width && r.height && r.right > 0 && r.bottom > 0 && r.x < innerWidth && r.y < innerHeight).map(rect);
          if (!rects.length) continue;
          let opacity = 1;
          for (let ancestor = el; ancestor; ancestor = ancestor.parentElement) opacity *= Number(getComputedStyle(ancestor).opacity);
          texts.push({ text: node.textContent.trim(), color: css.color, opacity,
            fontSize: parseFloat(css.fontSize), fontWeight: css.fontWeight,
            disabled: !!el.closest(":disabled,[aria-disabled=true]"), rects });
        }
        const lenses = [...document.querySelectorAll(".glass-lens")].map(lens => ({
          eligible: lens.dataset.nativeGlass, material: lens.dataset.glassMaterial,
          host: lens.parentElement.className, rect: rect(lens.getBoundingClientRect()) }));
        return { geometry, texts, lenses };
      });
      if (details.geometry.viewportWidth !== viewport.width || details.geometry.viewportHeight !== viewport.height)
        throw new Error("Viewport does not match exported geometry");
      const stem = `${mode}-${density}-${name}`, destination = path.join(out, stem);
      if (compose) {
        const exported = JSON.parse(await readFile(`${destination}.json`, "utf8"));
        if (JSON.stringify(exported.geometry) !== JSON.stringify(details.geometry))
          throw new Error(`Layout changed between export and composition: ${stem}`);
        for (const backdrop of ["extreme", "pattern"]) {
          const bytes = await readFile(`${destination}-${backdrop}.glass.rgba`);
          if (bytes.length !== viewport.width * viewport.height * 4) throw new Error("Incorrect rendered image length");
          const dataURL = await page.evaluate(({ encoded, width, height }) => {
            const canvas = document.createElement("canvas"); canvas.width = width; canvas.height = height;
            const pixels = Uint8ClampedArray.from(atob(encoded), char => char.charCodeAt(0));
            canvas.getContext("2d").putImageData(new ImageData(pixels, width, height), 0, 0);
            return canvas.toDataURL();
          }, { encoded: bytes.toString("base64"), ...viewport });
          await page.evaluate(async url => { const image = new Image(); image.src = url; await image.decode(); }, dataURL);
          const background = await page.addStyleTag({ content: `html { background: url("${dataURL}") 0 0 / 100% 100% no-repeat !important; }` });
          await page.screenshot({ path: `${destination}-${backdrop}.composed.png`, animations: "disabled" });
          const erased = await page.addStyleTag({ content: "#root, #root * { -webkit-text-fill-color: transparent !important; text-shadow: none !important; }" });
          await page.screenshot({ path: `${destination}-${backdrop}.textless.png`, animations: "disabled" });
          await erased.evaluate(el => el.remove());
          await background.evaluate(el => el.remove());
        }
        console.log(`${stem}: composed production optics beneath real CSS foreground`);
        return;
      }
      await page.screenshot({ path: `${destination}.front.png`, omitBackground: true, animations: "disabled" });
      const hiddenText = await page.addStyleTag({ content: "#root, #root * { -webkit-text-fill-color: transparent !important; text-shadow: none !important; }" });
      await page.screenshot({ path: `${destination}.textless.png`, omitBackground: true, animations: "disabled" });
      await hiddenText.evaluate(el => el.remove());
      const floats = new Float32Array(1552);
      floats.set([viewport.width, viewport.height, 0, 0], 0);
      floats.set([...(mode === "dark" ? [23,31,43] : [245,248,252]).map(c => c/255), .74 + density/100*.16], 4);
      floats.set([1.5 + 14.5*density/100, details.geometry.controls.length, 0, 1], 8);
      let tracks = 0;
      details.geometry.controls.forEach((c, i) => {
        floats.set([c.x,c.y,c.width,c.height,c.radius,c.pressure,c.material === "reading" ? 2 : c.material === "regular" ? 1 : 0,0], 12+i*8);
        if (c.track?.width > 0) {
          const t = c.track;
          floats.set([t.x,t.y,t.width,t.height,t.radius,t.progress,i,0,...t.activeRgba,...t.inactiveRgba], 528+tracks*16);
          ++tracks;
        }
      });
      floats[524] = tracks;
      await writeFile(`${destination}.state`, Buffer.from(floats.buffer));
      await writeFile(`${destination}.json`, JSON.stringify({ mode, density, name, ...details }, null, 2));
      snapshots.push(stem);
      console.log(`${stem}: ${details.geometry.controls.length} native lenses, ${details.texts.length} text runs`);
    };
    await capture("clipboard");
    await page.getByTitle("设置", { exact: true }).click();
    const general = page.getByRole("heading", { name: "常规设置", exact: true });
    await general.waitFor();
    await capture("settings");
    await general.click();
    await capture("general");
    await general.click();
    const appearance = page.getByRole("heading", { name: "界面设置", exact: true });
    await appearance.click();
    await capture("appearance");
    await page.locator(".glass-material-slider").first().scrollIntoViewIfNeeded();
    await capture("sliders");
    await context.close();
  }
  if (!compose) await writeFile(path.join(out, "manifest.json"), JSON.stringify({
    scope: "Headless fictional React foreground + exported DOM geometry. Native shader must be rendered separately. No WGC, HWND, DComp or iOS equivalence proof.",
    browser: browser.version(), viewport, hashes, snapshots,
  }, null, 2));
} finally { await browser.close(); }
