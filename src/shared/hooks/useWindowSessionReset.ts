import { useEffect, useRef, useState } from "react";
import { listen } from "@tauri-apps/api/event";
import type { AppState } from "../../features/app/types";

// Native shortcut, close button, blur and paste all use the same hide event.
// Blur alone is not a close: pinned windows and dialogs must keep their state.
export const useWindowSessionReset = (state: AppState, closeOverlays: () => void) => {
  const current = useRef({ state, closeOverlays });
  current.current = { state, closeOverlays };
  const [session, setSession] = useState(0);

  useEffect(() => {
    let disposed = false;
    const off = listen("main-window-hidden", () => {
      if (disposed) return;
      const { state: s, closeOverlays: close } = current.current;
      s.setShowSettings(false);
      s.setSettingsSubpage("home");
      s.setShowTagManager(false);
      s.setShowEmojiPanel(false);
      s.setShowAppSelector(null);
      s.setSearch("");
      s.setTypeFilter(null);
      s.setIsComposing(false);
      s.setSearchIsFocused(false);
      s.setShowTagFilter(false);
      s.setTagInput("");
      s.setEditingTagsId(null);
      s.setRevealedIds(new Set());
      s.setSelectedIndex(0);
      s.setIsKeyboardMode(false);
      s.setIsRecording(false);
      s.setIsRecordingSequential(false);
      s.setIsRecordingRich(false);
      s.setIsRecordingSearch(false);
      close();
      setSession(value => value + 1);
    });
    return () => {
      disposed = true;
      void off.then(unlisten => unlisten());
    };
  }, []);
  return session;
};
