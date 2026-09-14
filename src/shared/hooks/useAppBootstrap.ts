import { useEffect, useRef } from "react";
import type { Dispatch, SetStateAction } from "react";
import type { DefaultAppsMap, InstalledAppOption } from "../../features/app/types";
import { invoke } from "@tauri-apps/api/core";
import { isTauriRuntime } from "../lib/tauriRuntime";

interface UseAppBootstrapOptions {
  setDataPath: Dispatch<SetStateAction<string>>;
  setInstalledApps: Dispatch<SetStateAction<InstalledAppOption[]>>;
  setAutoStart: Dispatch<SetStateAction<boolean>>;
  setDefaultApps: Dispatch<SetStateAction<DefaultAppsMap>>;
  setWinClipboardDisabled: Dispatch<SetStateAction<boolean>>;
  loadAppAssociations: boolean;
}

type ApplicationOptions = { apps: InstalledAppOption[]; defaults: DefaultAppsMap };

export const useAppBootstrap = ({
  setDataPath, setInstalledApps, setAutoStart, setDefaultApps, loadAppAssociations
}: UseAppBootstrapOptions) => {
  const applicationRequest = useRef<Promise<ApplicationOptions> | null>(null);

  useEffect(() => {
    if (!isTauriRuntime()) return;
    let disposed = false;
    void invoke<string>("get_data_path").then(path => { if (!disposed) setDataPath(path); }).catch(console.error);
    void invoke<boolean>("is_autostart_enabled").then(enabled => { if (!disposed) setAutoStart(enabled); }).catch(console.error);
    return () => { disposed = true; };
  }, [setDataPath, setAutoStart]);

  useEffect(() => {
    if (!loadAppAssociations || !isTauriRuntime()) return;
    let disposed = false;
    if (!applicationRequest.current) {
      applicationRequest.current = Promise.all([
        invoke<{ name: string; path: string }[]>("scan_installed_apps"),
        Promise.all(["text", "rich_text", "image", "video", "code", "url"].map(async contentType => {
          const name = await invoke<string>("get_system_default_app", { contentType });
          return [contentType, name] as const;
        }))
      ]).then(([apps, defaults]) => ({
        apps: apps.map(app => ({ label: app.name, value: app.path })).sort((a, b) => a.label.localeCompare(b.label)),
        defaults: Object.fromEntries(defaults)
      })).catch(error => {
        applicationRequest.current = null;
        throw error;
      });
    }
    void applicationRequest.current.then(result => {
      if (disposed) return;
      setInstalledApps(result.apps);
      setDefaultApps(result.defaults);
    }).catch(console.error);
    return () => { disposed = true; };
  }, [loadAppAssociations, setInstalledApps, setDefaultApps]);
};
