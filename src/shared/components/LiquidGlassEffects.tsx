import { useEffect, useRef } from "react";
import { listen } from "@tauri-apps/api/event";
import { getLiquidGlassMaterial } from "../lib/appearance";

const SURFACES = ".header-actions, .liquid-search, .tm-search, .tm-view-toggle, .tm-batch-bar, .queue-bar";
const CONTROLS = "button:not(:disabled), .tm-tag, .segment-btn";

/** Optical feedback is confined to the glass layer; text and icons remain sharp. */
export default function LiquidGlassEffects({ theme, opacity }: { theme: string; opacity: number }) {
  const displacement = useRef<SVGFEDisplacementMapElement>(null);
  const { refraction } = getLiquidGlassMaterial(opacity);
  const baseRefraction = useRef(refraction);
  useEffect(() => { baseRefraction.current = refraction; }, [refraction]);
  useEffect(() => {
    if (theme !== "liquid-glass") return;
    const root = document.documentElement;
    const reduced = window.matchMedia("(prefers-reduced-motion: reduce)");
    const transparent = window.matchMedia("(prefers-reduced-transparency: reduce)");
    const contrast = window.matchMedia("(forced-colors: active)");
    const enabled = () => !reduced.matches && !transparent.matches && !contrast.matches && !document.hidden;
    const styled = new Set<HTMLElement>();
    const presses = new Map<HTMLElement, () => void>();
    let frame = 0, disposed = false;
    let x = innerWidth / 2, y = innerHeight / 3, targetX = x, targetY = y;
    let surface: HTMLElement | null = null;

    const clearSurface = (element: HTMLElement) => {
      for (const name of ["--liquid-x", "--liquid-y", "--liquid-angle"]) element.style.removeProperty(name);
    };
    const reset = () => {
      cancelAnimationFrame(frame); frame = 0;
      for (const cancel of presses.values()) cancel();
      presses.clear();
      for (const element of styled) clearSurface(element);
      styled.clear(); clearSurface(root); surface = null;
      displacement.current?.setAttribute("scale", String(baseRefraction.current));
    };
    const tick = () => {
      frame = 0;
      if (!enabled()) { reset(); return; }
      const dx = targetX - x, dy = targetY - y;
      x += dx * .22; y += dy * .22;
      root.style.setProperty("--liquid-x", `${x / innerWidth * 100}%`);
      root.style.setProperty("--liquid-y", `${y / innerHeight * 100}%`);
      root.style.setProperty("--liquid-angle", `${115 + x / innerWidth * 70}deg`);
      if (surface?.isConnected) {
        const box = surface.getBoundingClientRect();
        surface.style.setProperty("--liquid-x", `${x - box.left}px`);
        surface.style.setProperty("--liquid-y", `${y - box.top}px`);
        styled.add(surface);
      }
      displacement.current?.setAttribute("scale", String(baseRefraction.current + Math.min(12, Math.hypot(dx, dy) * .08)));
      if (Math.abs(dx) + Math.abs(dy) > .3) frame = requestAnimationFrame(tick);
    };
    const move = (event: PointerEvent) => {
      if (!enabled()) return;
      targetX = event.clientX; targetY = event.clientY;
      const next = event.target instanceof Element ? event.target.closest<HTMLElement>(SURFACES) : null;
      if (surface && surface !== next) { clearSurface(surface); styled.delete(surface); }
      surface = next;
      if (!frame) frame = requestAnimationFrame(tick);
    };
    const press = (event: PointerEvent) => {
      if (!enabled() || event.button !== 0 || !(event.target instanceof Element)) return;
      const control = event.target.closest<HTMLElement>(CONTROLS);
      if (!control || control.closest(".window-drag-region") && control.hasAttribute("data-tauri-drag-region")) return;
      presses.get(control)?.();
      const box = control.getBoundingClientRect();
      const wave = document.createElement("span");
      wave.className = "liquid-press-wave";
      wave.setAttribute("aria-hidden", "true");
      wave.style.setProperty("--press-x", `${event.clientX - box.left}px`);
      wave.style.setProperty("--press-y", `${event.clientY - box.top}px`);
      control.append(wave);
      const pulse = wave.animate([{ opacity: .7, scale: ".45" }, { opacity: 0, scale: "1.5" }], { duration: 620, easing: "cubic-bezier(.16,1,.3,1)" });
      const spring = control.animate([
        { scale: "1" }, { scale: ".94 1.02", offset: .16 },
        { scale: "1.025 .985", offset: .5 }, { scale: ".996 1.004", offset: .76 }, { scale: "1" }
      ], { duration: 520, easing: "ease-out" });
      const cancel = () => { pulse.cancel(); spring.cancel(); wave.remove(); presses.delete(control); };
      presses.set(control, cancel);
      pulse.onfinish = cancel;
    };
    document.addEventListener("pointermove", move, { passive: true });
    document.addEventListener("pointerdown", press, { passive: true });
    document.addEventListener("pointerleave", reset);
    document.addEventListener("pointercancel", reset);
    document.addEventListener("visibilitychange", reset);
    for (const media of [reduced, transparent, contrast]) media.addEventListener("change", reset);
    let unlisten: (() => void) | undefined;
    void listen("main-window-hidden", reset).then(off => { if (disposed) off(); else unlisten = off; }).catch(console.error);
    return () => {
      disposed = true; reset(); unlisten?.();
      document.removeEventListener("pointermove", move);
      document.removeEventListener("pointerdown", press);
      document.removeEventListener("pointerleave", reset);
      document.removeEventListener("pointercancel", reset);
      document.removeEventListener("visibilitychange", reset);
      for (const media of [reduced, transparent, contrast]) media.removeEventListener("change", reset);
    };
  }, [theme]);

  if (theme !== "liquid-glass") return null;
  return <svg className="liquid-filter-definitions" aria-hidden="true" width="0" height="0">
    <defs>
      <filter id="liquid-glass-refraction" x="-20%" y="-40%" width="140%" height="180%" colorInterpolationFilters="sRGB">
        <feTurbulence type="fractalNoise" baseFrequency=".012 .018" numOctaves="1" seed="11" result="flow" />
        <feGaussianBlur in="flow" stdDeviation="2" result="smooth-flow" />
        <feDisplacementMap ref={displacement} in="SourceGraphic" in2="smooth-flow" scale={refraction} xChannelSelector="R" yChannelSelector="G" />
      </filter>
    </defs>
  </svg>;
}
