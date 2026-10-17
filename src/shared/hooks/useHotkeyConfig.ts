import { useCallback, useEffect, useRef } from "react";
import { invoke } from "@tauri-apps/api/core";
import { listen, type UnlistenFn } from "@tauri-apps/api/event";
import { hotkeyIdentity } from "../lib/hotkeyIdentity";

type HotkeyMode = "main" | "sequential" | "rich" | "search";

interface UseHotkeyConfigOptions {
  hotkey: string;
  setHotkey: (val: string) => void;
  sequentialHotkey: string;
  setSequentialHotkey: (val: string) => void;
  richPasteHotkey: string;
  setRichPasteHotkey: (val: string) => void;
  searchHotkey: string;
  setSearchHotkey: (val: string) => void;
  sequentialMode: boolean;
  isRecording: boolean;
  setIsRecording: (val: boolean) => void;
  isRecordingSequential: boolean;
  setIsRecordingSequential: (val: boolean) => void;
  isRecordingRich: boolean;
  setIsRecordingRich: (val: boolean) => void;
  isRecordingSearch: boolean;
  setIsRecordingSearch: (val: boolean) => void;
  saveAppSetting: (type: string, value: string) => void;
  t: (key: string) => string;
  pushToast: (msg: string, duration?: number) => number;
}

export const useHotkeyConfig = ({
  hotkey, setHotkey, sequentialHotkey, setSequentialHotkey,
  richPasteHotkey, setRichPasteHotkey, searchHotkey, setSearchHotkey,
  sequentialMode, isRecording, setIsRecording,
  isRecordingSequential, setIsRecordingSequential,
  isRecordingRich, setIsRecordingRich, isRecordingSearch, setIsRecordingSearch,
  saveAppSetting, t, pushToast
}: UseHotkeyConfigOptions) => {
  const mode: HotkeyMode | null = isRecording ? "main" : isRecordingSequential ? "sequential"
    : isRecordingRich ? "rich" : isRecordingSearch ? "search" : null;
  const activeMode = useRef(mode);
  activeMode.current = mode;

  // Serialize transitions so a late enable cannot outlive a cancel or a field switch.
  const nativeQueue = useRef(Promise.resolve());
  const setNativeRecording = useCallback((enabled: boolean) => {
    const next = nativeQueue.current.then(() => invoke<void>("set_recording_mode", { enabled }));
    nativeQueue.current = next.catch(() => {});
    return next;
  }, []);

  const stopRecording = useCallback(() => {
    activeMode.current = null;
    setIsRecording(false);
    setIsRecordingSequential(false);
    setIsRecordingRich(false);
    setIsRecordingSearch(false);
    return setNativeRecording(false);
  }, [setIsRecording, setIsRecordingSequential, setIsRecordingRich, setIsRecordingSearch, setNativeRecording]);

  const checkHotkeyConflict = useCallback((newHotkey: string, target: HotkeyMode) => {
    if (!newHotkey) return false;
    const matches = (configured: string) => hotkeyIdentity(newHotkey) === hotkeyIdentity(configured);
    const conflicts = [];
    if (target !== "main" && matches(hotkey)) conflicts.push(t("global_hotkey"));
    if (target !== "sequential" && sequentialMode && matches(sequentialHotkey)) conflicts.push(t("sequential_paste_hotkey_label"));
    if (target !== "rich" && matches(richPasteHotkey)) conflicts.push(t("rich_paste_hotkey_label"));
    if (target !== "search" && matches(searchHotkey)) conflicts.push(t("search_hotkey_label"));
    if (!conflicts.length) return false;
    pushToast(t("hotkey_conflict_toast").replace("{name}", conflicts[0]), 5000);
    return true;
  }, [hotkey, sequentialMode, sequentialHotkey, richPasteHotkey, searchHotkey, t, pushToast]);

  const update = useCallback(async (target: HotkeyMode, newHotkey: string): Promise<boolean> => {
    try {
      // Release the recorder's temporary Win registrations before probing availability.
      await stopRecording();
      if (checkHotkeyConflict(newHotkey, target)) return false;
      const config = {
        main: { value: hotkey, set: setHotkey, key: "hotkey", command: "register_hotkey" },
        sequential: { value: sequentialHotkey, set: setSequentialHotkey, key: "sequential_hotkey", command: "set_sequential_hotkey" },
        rich: { value: richPasteHotkey, set: setRichPasteHotkey, key: "rich_paste_hotkey", command: "set_rich_paste_hotkey" },
        search: { value: searchHotkey, set: setSearchHotkey, key: "search_hotkey", command: "set_search_hotkey" }
      }[target];
      if (hotkeyIdentity(newHotkey) === hotkeyIdentity(config.value)) return true;
      const usesWinVHook = target === "main" && hotkeyIdentity(newHotkey) === hotkeyIdentity("Win+V");
      if (newHotkey && !usesWinVHook && !await invoke<boolean>("test_hotkey_available", { hotkey: newHotkey, currentHotkey: config.value })) {
        throw new Error(t("hotkey_unavailable"));
      }
      await invoke(config.command, { hotkey: newHotkey });
      config.set(newHotkey);
      saveAppSetting(config.key, newHotkey);
      return true;
    } catch (error) {
      pushToast(t("hotkey_register_failed") + String(error), 5000);
      return false;
    }
  }, [stopRecording, checkHotkeyConflict, hotkey, setHotkey, sequentialHotkey, setSequentialHotkey,
    richPasteHotkey, setRichPasteHotkey, searchHotkey, setSearchHotkey, saveAppSetting, pushToast, t]);
  const currentUpdate = useRef(update);
  currentUpdate.current = update;

  const listenersReady = useRef<Promise<UnlistenFn[]>>(Promise.resolve([]));
  useEffect(() => {
    let disposed = false;
    listenersReady.current = Promise.all([
      listen<string>("hotkey-recorded", event => {
        const target = activeMode.current;
        if (disposed || !target) return;
        const value = /(^|\+)(Backspace|Delete)$/i.test(event.payload) ? "" : event.payload;
        void currentUpdate.current(target, value);
      }),
      listen("recording-cancelled", () => {
        if (!disposed) void stopRecording().catch(console.error);
      })
    ]);
    return () => {
      disposed = true;
      activeMode.current = null;
      void listenersReady.current.then(listeners => listeners.forEach(off => off())).catch(console.error);
      void setNativeRecording(false).catch(console.error);
    };
  }, [stopRecording, setNativeRecording]);

  useEffect(() => {
    let disposed = false;
    if (mode) {
      // Listening must finish before the hook is allowed to consume the first key.
      void listenersReady.current.then(() => {
        if (!disposed) return setNativeRecording(true);
      }).catch(error => {
        console.error(error);
        if (!disposed) void stopRecording().catch(console.error);
      });
    }
    return () => {
      disposed = true;
      void setNativeRecording(false).catch(console.error);
    };
  }, [mode, setNativeRecording, stopRecording]);

  return {
    checkHotkeyConflict,
    updateHotkey: useCallback((value: string) => update("main", value), [update]),
    updateSequentialHotkey: useCallback((value: string) => update("sequential", value), [update]),
    updateRichPasteHotkey: useCallback((value: string) => update("rich", value), [update]),
    updateSearchHotkey: useCallback((value: string) => update("search", value), [update])
  };
};
