import { invoke } from "@tauri-apps/api/core";
import { type UnlistenFn } from "@tauri-apps/api/event";
import { getCurrentWebviewWindow } from "@tauri-apps/api/webviewWindow";
import { glassPaintOrder } from "./glassPaintOrder";

export interface NativeGlassStatus {
  state: "inactive" | "pending" | "ready" | "resizing" | "suspended" | "failed";
  revision: number;
  geometryRevision: number;
}

export type GlassSurfaceMaterial = "clear" | "regular" | "reading";

export interface NativeGlassGeometry {
  viewportWidth: number;
  viewportHeight: number;
  controls: { x: number; y: number; width: number; height: number; radius: number; pressure: number; material?: GlassSurfaceMaterial; track?: NativeGlassTrack }[];
}

interface NativeGlassTrack {
  x: number;
  y: number;
  width: number;
  height: number;
  radius: number;
  progress: number;
  activeRgba: [number, number, number, number];
  inactiveRgba: [number, number, number, number];
}

interface Lens {
  host: HTMLElement;
  element: HTMLElement;
  radius: number;
  material: GlassSurfaceMaterial;
  pressed: boolean;
  pressure: number;
}

interface GeometrySnapshot {
  viewportWidth: number;
  viewportHeight: number;
  keys: string[];
}

const lenses = new Set<Lens>();
let generation = 0;
let pendingGeometry: { generation: number; geometry: NativeGlassGeometry; accepted?: (revision: number) => void } | undefined;
let sendingGeometry = false;

// A slow native call must not build up an animation's worth of obsolete rects.
function queueGeometry(geometry: NativeGlassGeometry, currentGeneration: number, accepted?: (revision: number) => void) {
  pendingGeometry = { generation: currentGeneration, geometry, accepted };
  if (sendingGeometry) return;
  sendingGeometry = true;
  void (async () => {
    try {
      while (pendingGeometry) {
        const next = pendingGeometry;
        pendingGeometry = undefined;
        if (next.generation !== generation) continue;
        try {
          const receipt = await invoke<number>("set_glass_geometry", { geometry: next.geometry });
          if (next.generation === generation && Number.isSafeInteger(receipt) && receipt > 0)
            next.accepted?.(receipt);
        }
        catch (error) { console.error("Failed to update native glass geometry", error); }
      }
    } finally { sendingGeometry = false; }
  })();
}

function setPresentation(ready: boolean, backgroundReady: boolean) {
  for (const element of [document.documentElement, document.body]) {
    element.classList.toggle("native-glass-ready", ready);
    element.classList.toggle("native-glass-background-ready", backgroundReady);
  }
}

function validStatus(value: NativeGlassStatus): boolean {
  return value != null && Number.isSafeInteger(value.revision) && value.revision >= 0
    && Number.isSafeInteger(value.geometryRevision) && value.geometryRevision >= 0
    && ["inactive", "pending", "ready", "resizing", "suspended", "failed"].includes(value.state);
}

const rounded = (value: number) => Math.round(value * 1000) / 1000;

function geometryKey(control: NativeGlassGeometry["controls"][number]) {
  // Press deformation keeps the same footprint and must not switch renderers
  // on every animation frame. Position, material and track changes do matter.
  const { pressure: _pressure, ...geometry } = control;
  return JSON.stringify(geometry);
}

interface MeasurementFrame {
  styles: Map<HTMLElement, CSSStyleDeclaration>;
  bounds: Map<HTMLElement, DOMRect>;
}

function measuredStyle(element: HTMLElement, frame: MeasurementFrame) {
  let style = frame.styles.get(element);
  if (!style) { style = getComputedStyle(element); frame.styles.set(element, style); }
  return style;
}

function measuredBounds(element: HTMLElement, frame: MeasurementFrame) {
  let bounds = frame.bounds.get(element);
  if (!bounds) { bounds = element.getBoundingClientRect(); frame.bounds.set(element, bounds); }
  return bounds;
}

function visibleRect(host: HTMLElement, viewportWidth: number, viewportHeight: number, frame: MeasurementFrame) {
  // WebView popup backplates cover the native composition plane. Keep their
  // controls on the CSS renderer so a ready native frame cannot erase them.
  if (host.closest("[data-glass-css-only]")) return;
  if (!host.isConnected || !host.getClientRects().length) return;
  const rect = measuredBounds(host, frame);
  if (![rect.x, rect.y, rect.width, rect.height].every(Number.isFinite)
    || rect.width < .5 || rect.height < .5 || rect.right <= 0 || rect.bottom <= 0
    || rect.left >= viewportWidth || rect.top >= viewportHeight) return;
  const style = measuredStyle(host, frame);
  if (style.visibility !== "visible" || style.display === "none") return;

  // Native glass sits below the WebView and cannot inherit its scroll/exit
  // clipping. Keep the CSS lens for those transition frames instead of drawing
  // an unbounded native shape outside its parent.
  for (let ancestor: HTMLElement | null = host; ancestor && ancestor !== document.body; ancestor = ancestor.parentElement) {
    const ancestorStyle = measuredStyle(ancestor, frame);
    if (Number(ancestorStyle.opacity) < .999) return;
    if (ancestor === host) continue;
    const clipsX = /^(hidden|clip|scroll|auto)$/.test(ancestorStyle.overflowX);
    const clipsY = /^(hidden|clip|scroll|auto)$/.test(ancestorStyle.overflowY);
    if (!clipsX && !clipsY) continue;
    const bounds = measuredBounds(ancestor, frame);
    const left = bounds.left + ancestor.clientLeft;
    const top = bounds.top + ancestor.clientTop;
    if ((clipsX && (rect.left < left - .5 || rect.right > left + ancestor.clientWidth + .5))
      || (clipsY && (rect.top < top - .5 || rect.bottom > top + ancestor.clientHeight + .5))) return;
  }
  return { rect, style };
}

function radiusInRect(host: HTMLElement, rect: DOMRect, style: CSSStyleDeclaration, fallback: number) {
  const cssRadius = parseFloat(style.borderTopLeftRadius);
  const scale = Math.min(rect.width / (host.offsetWidth || rect.width), rect.height / (host.offsetHeight || rect.height));
  return Math.min(Math.max(0, Number.isFinite(cssRadius) ? cssRadius * scale : fallback), rect.width / 2, rect.height / 2);
}

function rgba(color: string): [number, number, number, number] | undefined {
  // Computed accent-color and the shared track token resolve to RGB(A) in
  // WebView2. Unsupported CSS colors keep this control on the CSS renderer.
  const channels = color.trim().match(/^rgba?\(\s*([\d.]+)\s*,\s*([\d.]+)\s*,\s*([\d.]+)(?:\s*,\s*([\d.]+))?\s*\)$/);
  if (!channels) return;
  const values: [number, number, number, number] = [Number(channels[1]) / 255, Number(channels[2]) / 255, Number(channels[3]) / 255, channels[4] === undefined ? 1 : Number(channels[4])];
  return values.every(value => Number.isFinite(value) && value >= 0 && value <= 1) ? values : undefined;
}

function measureTrack(host: HTMLElement, viewportWidth: number, viewportHeight: number, frame: MeasurementFrame): NativeGlassTrack | undefined {
  const input = host.closest(".glass-slider-control")?.querySelector<HTMLInputElement>(".glass-material-slider");
  if (!input) return;
  // The track can cross a scroll clip even while its thumb is wholly visible.
  // Keep both on the same renderer unless their complete geometry is eligible.
  const visible = visibleRect(input, viewportWidth, viewportHeight, frame);
  if (!visible) return;
  const { rect, style } = visible;
  const min = Number(input.min), max = Number(input.max), value = input.valueAsNumber;
  const activeRgba = rgba(style.accentColor);
  const inactiveRgba = rgba(style.getPropertyValue("--glass-track-inactive"));
  if (![min, max, value].every(Number.isFinite) || max <= min || !activeRgba || !inactiveRgba) return;
  return {
    x: rounded(rect.x), y: rounded(rect.y), width: rounded(rect.width), height: rounded(rect.height),
    radius: rounded(radiusInRect(input, rect, style, rect.height / 2)),
    progress: rounded(Math.min(1, Math.max(0, (value - min) / (max - min)))),
    activeRgba, inactiveRgba
  };
}

function measureLens(lens: Lens, viewportWidth: number, viewportHeight: number, frame: MeasurementFrame) {
  const { host } = lens;
  const visible = visibleRect(host, viewportWidth, viewportHeight, frame);
  if (!visible) return;
  const { rect, style } = visible;
  const slider = host.matches(".glass-slider-thumb");
  const track = slider ? measureTrack(host, viewportWidth, viewportHeight, frame) : undefined;
  if (slider && !track) return;
  return {
    x: rounded(rect.x), y: rounded(rect.y), width: rounded(rect.width), height: rounded(rect.height),
    radius: rounded(radiusInRect(host, rect, style, lens.radius)), pressure: rounded(lens.pressure), material: lens.material,
    ...(track ? { track } : {})
  };
}

export function registerGlassLens(host: HTMLElement, element: HTMLElement, radius: number, material: GlassSurfaceMaterial = "clear"): () => void {
  const lens: Lens = { host, element, radius, material, pressed: false, pressure: 0 };
  lenses.add(lens);
  const interaction = host.closest<HTMLElement>(".glass-slider-control, .type-filter-btn") ?? host;
  const press = (event: PointerEvent) => { if (event.button === 0) lens.pressed = true; };
  const release = () => { lens.pressed = false; };
  const keyDown = (event: KeyboardEvent) => {
    if (event.key === " " || event.key === "Enter" || (interaction.matches(".glass-slider-control") && event.key.startsWith("Arrow"))) lens.pressed = true;
  };
  interaction.addEventListener("pointerdown", press);
  interaction.addEventListener("keydown", keyDown);
  window.addEventListener("pointerup", release);
  window.addEventListener("pointercancel", release);
  window.addEventListener("keyup", release);
  window.addEventListener("blur", release);
  return () => {
    lenses.delete(lens);
    delete element.dataset.nativeGlass;
    interaction.removeEventListener("pointerdown", press);
    interaction.removeEventListener("keydown", keyDown);
    window.removeEventListener("pointerup", release);
    window.removeEventListener("pointercancel", release);
    window.removeEventListener("keyup", release);
    window.removeEventListener("blur", release);
  };
}

export function startNativeGlass() {
  const currentGeneration = ++generation;
  let disposed = false;
  let armed = false;
  let latestStatus: NativeGlassStatus | undefined;
  let unlisten: UnlistenFn | undefined;
  let lastGeometry = "";
  const receipts = new Map<number, GeometrySnapshot>();
  let submittedGeometry: GeometrySnapshot | undefined;
  let previousTime = 0;
  let frame = 0;
  const reducedMotion = window.matchMedia("(prefers-reduced-motion: reduce)");
  const reducedTransparency = window.matchMedia("(prefers-reduced-transparency: reduce)");
  const forcedColors = window.matchMedia("(forced-colors: active)");
  let transparencyAllowed = !reducedTransparency.matches && !forcedColors.matches;
  const current = () => !disposed && currentGeneration === generation;
  const updateSubmittedGeometry = () => {
    submittedGeometry = latestStatus?.state === "ready" ? receipts.get(latestStatus.geometryRevision) : undefined;
    // A status event can arrive without another rendering opportunity. Revoke
    // old handoffs immediately; only measurement may confirm a new footprint.
    if (!submittedGeometry) for (const lens of lenses) lens.element.dataset.nativeGlass = "false";
  };
  const updateReady = () => {
    transparencyAllowed = !reducedTransparency.matches && !forcedColors.matches;
    if (current()) {
      const allowed = armed && transparencyAllowed;
      const ready = latestStatus?.state === "ready";
      // Resize keeps the last captured background, while CSS lenses cover the
      // interval before a new capture and matching native geometry are ready.
      setPresentation(allowed && ready, allowed && (ready || latestStatus?.state === "resizing"));
    }
  };
  const acceptStatus = (status: NativeGlassStatus) => {
    if (!current() || !validStatus(status) || (latestStatus && status.revision <= latestStatus.revision)) return;
    latestStatus = status;
    updateSubmittedGeometry();
    updateReady();
  };
  setPresentation(false, false);
  const listenerReady = getCurrentWebviewWindow().listen<NativeGlassStatus>("native-glass-status", event => acceptStatus(event.payload))
    .then(off => { if (current()) unlisten = off; else off(); })
    .catch(error => console.error("Failed to observe native glass status", error));
  reducedTransparency.addEventListener("change", updateReady);
  forcedColors.addEventListener("change", updateReady);

  const measure = (time: number) => {
    if (!current()) return;
    // Also read the live value: browser media emulation can update .matches
    // without delivering a change event to existing MediaQueryLists.
    if (transparencyAllowed !== (!reducedTransparency.matches && !forcedColors.matches)) updateReady();
    const delta = previousTime ? Math.min(64, Math.max(0, time - previousTime)) : 16;
    previousTime = time;
    const geometry: NativeGlassGeometry = { viewportWidth: window.innerWidth, viewportHeight: window.innerHeight, controls: [] };
    const measured = new Map<HTMLElement, { control: NonNullable<ReturnType<typeof measureLens>>; key: string }>();
    // Share ancestor reads within this frame. Never cache across frames: scroll,
    // transforms, opacity and clips must still revoke stale native handoffs.
    const measurement: MeasurementFrame = { styles: new Map(), bounds: new Map() };
    for (const lens of lenses) {
      const target = !reducedMotion.matches && lens.pressed ? 1 : 0;
      lens.pressure = reducedMotion.matches ? 0 : lens.pressure + (target - lens.pressure) * (1 - Math.exp(-delta / 55));
      if (Math.abs(lens.pressure - target) < .001) lens.pressure = target;
      const control = transparencyAllowed ? measureLens(lens, geometry.viewportWidth, geometry.viewportHeight, measurement) : undefined;
      if (control) measured.set(lens.element, { control, key: geometryKey(control) });
    }
    const selected = new Set(glassPaintOrder([...measured.keys()]).slice(0, 64));
    const matchingViewport = submittedGeometry?.viewportWidth === geometry.viewportWidth
      && submittedGeometry.viewportHeight === geometry.viewportHeight;
    const confirmed = new Set([...selected].filter((element, index) =>
      matchingViewport && submittedGeometry?.keys[index] === measured.get(element)!.key));
    for (const lens of lenses) {
      // A selected rectangle is not yet a native surface. Keep its CSS glass
      // until that geometry and paint-order slot belong to a submitted frame.
      // Identical React remounts can reuse the already confirmed footprint.
      const eligible = confirmed.has(lens.element) ? "true" : "false";
      if (lens.element.dataset.nativeGlass !== eligible) lens.element.dataset.nativeGlass = eligible;
    }
    // The protocol array is the real back-to-front order, independent of React
    // effect registration. Overflow retains the complete CSS material.
    for (const element of selected) {
      geometry.controls.push(measured.get(element)!.control);
    }
    const fingerprint = JSON.stringify(geometry);
    if (fingerprint !== lastGeometry) {
      lastGeometry = fingerprint;
      queueGeometry(geometry, currentGeneration, receipt => {
        receipts.set(receipt, { viewportWidth: geometry.viewportWidth, viewportHeight: geometry.viewportHeight,
          keys: [...selected].map(element => measured.get(element)!.key) });
        // Deferral can outlive many IPC replies. Bound history while retaining
        // the currently acknowledged frame; unknown receipts keep CSS visible.
        for (const revision of receipts.keys()) {
          if (receipts.size <= 32) break;
          if (revision !== latestStatus?.geometryRevision) receipts.delete(revision);
        }
        // The frame event may arrive before its IPC reply.
        updateSubmittedGeometry();
      });
    }
    // ResizeObserver misses transforms and scroll-driven movement. These few
    // hosts are sampled once per frame; unchanged snapshots never cross IPC.
    frame = requestAnimationFrame(measure);
  };
  frame = requestAnimationFrame(measure);

  return {
    async afterThemeApplied() {
      if (!current()) return;
      lastGeometry = ""; // The new backend must receive even unchanged rects.
      receipts.clear();
      updateSubmittedGeometry();
      await listenerReady;
      if (!current()) return;
      try {
        const status = await invoke<NativeGlassStatus>("get_native_glass_status");
        if (!current()) return;
        acceptStatus(status);
        // An initial query is insufficient if later suspension cannot be seen.
        armed = !!unlisten && validStatus(status);
        updateReady();
      } catch (error) {
        if (current()) { armed = false; updateReady(); }
        console.error("Failed to query native glass status", error);
      }
    },
    dispose() {
      if (disposed) return;
      disposed = true;
      cancelAnimationFrame(frame);
      unlisten?.();
      reducedTransparency.removeEventListener("change", updateReady);
      forcedColors.removeEventListener("change", updateReady);
      if (currentGeneration === generation) {
        ++generation;
        setPresentation(false, false);
        queueGeometry({ viewportWidth: window.innerWidth, viewportHeight: window.innerHeight, controls: [] }, generation);
        for (const lens of lenses) { lens.pressure = 0; lens.pressed = false; delete lens.element.dataset.nativeGlass; }
      }
    }
  };
}
