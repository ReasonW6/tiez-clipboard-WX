import { useEffect } from "react";
import { invoke } from "@tauri-apps/api/core";

interface UseSettingsSyncOptions {
  settingsLoaded: boolean;
  deduplicate: boolean;
  saveAppSetting: (type: string, value: string) => void;
  captureFiles: boolean;
  captureRichText: boolean;

  persistent: boolean;
  soundVolume: number;
  arrowKeySelection: boolean;
  setIsKeyboardMode: (val: boolean) => void;
  setSelectedIndex: (val: number) => void;
}

export const useSettingsSync = ({
  settingsLoaded,
  deduplicate,
  saveAppSetting,
  captureFiles,
  captureRichText,
  persistent,
  soundVolume,
  arrowKeySelection,
  setIsKeyboardMode,
  setSelectedIndex
}: UseSettingsSyncOptions) => {
  useEffect(() => {
    if (settingsLoaded) {
      invoke("set_deduplication", { enabled: deduplicate });
      saveAppSetting("deduplicate", String(deduplicate));
    }
  }, [deduplicate, saveAppSetting, settingsLoaded]);

  useEffect(() => {
    if (settingsLoaded) {
      invoke("set_capture_files", { enabled: captureFiles });
    }
  }, [captureFiles, settingsLoaded]);

  useEffect(() => {
    if (settingsLoaded) {
      invoke("set_capture_rich_text", { enabled: captureRichText });
    }
  }, [captureRichText, settingsLoaded]);

  useEffect(() => {
    if (settingsLoaded) {
      invoke("set_persistence", { enabled: persistent });
    }
  }, [persistent, settingsLoaded]);

  useEffect(() => {
    if (settingsLoaded) {
      saveAppSetting("sound_volume", String(soundVolume));
    }
  }, [saveAppSetting, settingsLoaded, soundVolume]);

  useEffect(() => {
    invoke("set_arrow_key_selection", { enabled: arrowKeySelection }).catch(console.error);
    if (!arrowKeySelection) {
      setIsKeyboardMode(false);
      setSelectedIndex(0);
    }
  }, [arrowKeySelection, setIsKeyboardMode, setSelectedIndex]);
};
