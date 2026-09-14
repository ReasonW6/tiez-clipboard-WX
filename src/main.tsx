import React from "react";
import ReactDOM from "react-dom/client";
import "./index.css";
import "./styles/components/index.css";
import "./styles/themes/load";
import { initializeAppearance } from "./shared/lib/appearance";

initializeAppearance();

const params = new URLSearchParams(window.location.search);
const isCompactPreview = params.get("window") === "compact-preview";
const isAdvancedSettingsWindow = params.get("window") === "advanced-settings";

const RootWindow = isCompactPreview
  ? (await import("./features/clipboard/components/CompactPreviewWindow")).default
  : isAdvancedSettingsWindow
    ? (await import("./features/settings/components/AdvancedSettingsWindow")).default
    : (await import("./App")).default;

ReactDOM.createRoot(document.getElementById("root") as HTMLElement).render(
  <React.StrictMode><RootWindow /></React.StrictMode>
);
