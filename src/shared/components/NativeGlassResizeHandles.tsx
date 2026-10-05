import { useEffect, useState, type PointerEvent } from "react";
import { getCurrentWindow } from "@tauri-apps/api/window";

const DIRECTIONS = ["North", "NorthEast", "East", "SouthEast", "South", "SouthWest", "West", "NorthWest"] as const;

// The native glass mask removes the invisible non-client resize margins.
// Restore those mouse targets within the client area using Tauri's public API.
export default function NativeGlassResizeHandles() {
  const [canResize, setCanResize] = useState(false);
  useEffect(() => {
    let disposed = false;
    let revision = 0;
    let timer: ReturnType<typeof setTimeout> | undefined;
    let unlisten: (() => void) | undefined;
    const appWindow = getCurrentWindow();
    const refresh = async () => {
      const request = ++revision;
      try {
        const [resizable, maximized, fullscreen] = await Promise.all([
          appWindow.isResizable(), appWindow.isMaximized(), appWindow.isFullscreen()
        ]);
        if (!disposed && request === revision) setCanResize(resizable && !maximized && !fullscreen);
      } catch (error) {
        if (!disposed && request === revision) setCanResize(false);
        console.error("Failed to read window resize state", error);
      }
    };
    void refresh();
    void appWindow.onResized(() => {
      if (disposed) return;
      clearTimeout(timer);
      timer = setTimeout(() => { void refresh(); }, 80);
    }).then(off => { if (disposed) off(); else unlisten = off; }).catch(console.error);
    return () => { disposed = true; clearTimeout(timer); unlisten?.(); };
  }, []);

  const startResize = (event: PointerEvent<HTMLSpanElement>, direction: typeof DIRECTIONS[number]) => {
    if (event.button !== 0 || !canResize) return;
    event.preventDefault();
    event.stopPropagation();
    void getCurrentWindow().startResizeDragging(direction).catch(error => console.error("Failed to start window resize", error));
  };

  if (!canResize) return null;
  return <div className="native-glass-resize-handles window-no-drag" aria-hidden="true">
    {DIRECTIONS.map(direction => <span key={direction} data-resize-direction={direction} onPointerDown={event => startResize(event, direction)} />)}
  </div>;
}
