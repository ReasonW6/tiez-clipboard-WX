import { useEffect, useId, useRef, useState, type CSSProperties } from "react";
import { registerGlassLens, type GlassSurfaceMaterial } from "../lib/nativeGlass";

const lensMapCache = new Map<string, string>();
const lensMapCacheLimit = 16;

// RG stores the displacement and B stores optical coverage. The map follows
// the backdrop clip; its rim samples stay inside and never double back.
export function makeLensMap(width: number, height: number, radius: number) {
  // Fractional geometry affects the optical map even when canvas sizes match.
  const key = `${width}:${height}:${radius}`;
  const cached = lensMapCache.get(key);
  if (cached !== undefined) {
    lensMapCache.delete(key);
    lensMapCache.set(key, cached);
    return cached;
  }
  const canvas = document.createElement("canvas");
  canvas.width = Math.max(1, Math.ceil(width));
  canvas.height = Math.max(1, Math.ceil(height));
  const context = canvas.getContext("2d")!;
  const pixels = context.createImageData(canvas.width, canvas.height);
  const r = Math.min(radius, width / 2, height / 2);
  const edge = Math.min(9, r);
  const opticalWidth = Math.min(6, r);
  // The flat interior and uncovered corners encode the same neutral vector.
  // Initialize cheaply, then evaluate optics only along the actual boundary.
  for (let i = 0; i < pixels.data.length; i += 4) {
    pixels.data[i] = pixels.data[i + 1] = 128;
    pixels.data[i + 3] = 255;
  }
  for (let y = 0; y < canvas.height; y++) for (let x = 0; x < canvas.width; x++) {
    if (y + .5 >= edge && y + .5 <= height - edge
      && x + .5 >= r && x + .5 <= width - r) {
      x = Math.floor(width - r - .5);
      continue;
    }
    const px = x + .5 - width / 2, py = y + .5 - height / 2;
    const qx = Math.abs(px) - (width / 2 - r), qy = Math.abs(py) - (height / 2 - r);
    const ox = Math.max(qx, 0), oy = Math.max(qy, 0);
    const length = Math.hypot(ox, oy);
    const distance = length + Math.min(Math.max(qx, qy), 0) - r;
    const nx = Math.sign(px) * (length ? ox / length : qx > qy ? 1 : 0);
    const ny = Math.sign(py) * (length ? oy / length : qy >= qx ? 1 : 0);
    const t = Math.max(0, Math.min(1, -distance / edge));
    const depth = Math.max(0, -distance);
    const displacement = distance <= 0 ? .9 * depth * (1 - t) ** 2 : 0;
    const bend = displacement * 256 / 13;
    const i = (y * canvas.width + x) * 4;
    pixels.data[i] = Math.round(128 + nx * bend);
    pixels.data[i + 1] = Math.round(128 + ny * bend);
    pixels.data[i + 3] = 255;
    // A backdrop-filter is composited over an already painted, potentially
    // transparent WebView. Leave its flat center completely unpainted.
    pixels.data[i + 2] = distance <= 0 && depth < opticalWidth
      ? Math.round(255 * .35 * Math.sin(Math.PI * depth / opticalWidth) ** 2)
      : 0;
  }
  context.putImageData(pixels, 0, 0);
  const map = canvas.toDataURL();
  lensMapCache.set(key, map);
  if (lensMapCache.size > lensMapCacheLimit) lensMapCache.delete(lensMapCache.keys().next().value!);
  return map;
}

export default function GlassLens({ radius = 24, material = "clear" }: { radius?: number; material?: GlassSurfaceMaterial }) {
  const ref = useRef<HTMLSpanElement>(null);
  const id = `glass-${useId().replace(/:/g, "")}`;
  const [map, setMap] = useState("");
  const [geometry, setGeometry] = useState({ width: 1, height: 1, radius });
  const refractive = material !== "reading";
  useEffect(() => {
    const lens = ref.current;
    const host = lens?.parentElement;
    if (!lens || !host) return;
    const unregister = registerGlassLens(host, lens, radius, material);
    const observer = new ResizeObserver(([entry]) => {
      const { width, height } = entry.contentRect;
      if (!width || !height) return;
      const hostRadius = parseFloat(getComputedStyle(host).borderTopLeftRadius);
      const resolvedRadius = Math.min(Number.isFinite(hostRadius) ? hostRadius : radius, width / 2, height / 2);
      setGeometry({ width, height, radius: resolvedRadius });
      if (refractive) setMap(makeLensMap(width, height, resolvedRadius));
    });
    observer.observe(lens);
    return () => { observer.disconnect(); unregister(); };
  }, [radius, material, refractive]);
  return <span ref={ref} className="glass-lens" data-glass-material={material} aria-hidden="true" style={{ borderRadius: geometry.radius }}>
    {refractive && <svg className="glass-lens-defs" xmlns="http://www.w3.org/2000/svg">
      <defs><filter id={id} filterUnits="userSpaceOnUse" primitiveUnits="userSpaceOnUse" x="0" y="0" width={geometry.width} height={geometry.height} colorInterpolationFilters="sRGB">
        <feImage href={map} x="0" y="0" width={geometry.width} height={geometry.height} preserveAspectRatio="none" result="lens" />
        <feColorMatrix in="lens" type="matrix" values="0 0 0 0 0  0 0 0 0 0  0 0 0 0 0  0 0 1 0 0" result="coverage" />
        {/* Make byte value 128 exactly neutral instead of shifting the center. */}
        <feComponentTransfer in="lens" result="normal">
          <feFuncR type="linear" slope="0.99609375" />
          <feFuncG type="linear" slope="0.99609375" />
        </feComponentTransfer>
        <feDisplacementMap in="SourceGraphic" in2="normal" scale="8" xChannelSelector="R" yChannelSelector="G" result="r" />
        <feDisplacementMap in="SourceGraphic" in2="normal" scale="12.5" xChannelSelector="R" yChannelSelector="G" result="g" />
        <feDisplacementMap in="SourceGraphic" in2="normal" scale="17" xChannelSelector="R" yChannelSelector="G" result="b" />
        {/* Recombine straight RGB, then restore one displaced alpha channel. */}
        <feColorMatrix in="r" type="matrix" values="1 0 0 0 0  0 0 0 0 0  0 0 0 0 0  0 0 0 0 1" result="red" />
        <feColorMatrix in="g" type="matrix" values="0 0 0 0 0  0 1 0 0 0  0 0 0 0 0  0 0 0 0 1" result="green" />
        <feColorMatrix in="b" type="matrix" values="0 0 0 0 0  0 0 0 0 0  0 0 1 0 0  0 0 0 0 1" result="blue" />
        <feComposite in="red" in2="green" operator="arithmetic" k2="1" k3="1" result="rg" />
        <feComposite in="rg" in2="blue" operator="arithmetic" k2="1" k3="1" result="rgb" />
        <feComposite in="rgb" in2="g" operator="in" result="optical-color" />
        <feComposite in="optical-color" in2="coverage" operator="in" />
      </filter></defs>
    </svg>}
    {refractive && map && <span className="glass-lens-refraction" style={{ backdropFilter: `url(#${id})` } as CSSProperties} />}
    <span className="glass-lens-reflection" />
    <span className="glass-lens-shadow" />
  </span>;
}
