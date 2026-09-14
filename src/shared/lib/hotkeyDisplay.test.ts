import { expect, it } from "vitest";
import { getHotkeyDisplayTokens } from "./hotkeyDisplay";

it("labels stored modifier aliases with Windows key names", () => {
  expect(getHotkeyDisplayTokens("Command+Control+Shift+Space", { preferMacSymbols: false }).map(t => t.label))
    .toEqual(["Win", "Ctrl", "Shift", "Space"]);
});
it("retains symbols when rendering macOS shortcuts", () => {
  expect(getHotkeyDisplayTokens("Command+Alt+Shift+V", { preferMacSymbols: true }).map(t => t.label))
    .toEqual(["⌘", "⌥", "⇧", "V"]);
});
