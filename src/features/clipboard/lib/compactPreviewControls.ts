type CompactPreviewControls = { forceHide: () => void };
let controls: CompactPreviewControls | null = null;

export const registerCompactPreviewControls = (next: CompactPreviewControls) => {
  controls = next;
};

export const forceHideCompactPreviewWindow = () => {
  controls?.forceHide();
};
