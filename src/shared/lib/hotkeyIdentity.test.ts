import { expect, it } from "vitest";
import { hotkeyIdentity } from "./hotkeyIdentity";

it("identifies the default rich paste chord regardless of modifier order", () => {
  expect(hotkeyIdentity("Alt+Shift+V")).toBe(hotkeyIdentity("Shift+Alt+V"));
});

it("recognizes stored modifier aliases and keeps distinct chords separate", () => {
  expect(hotkeyIdentity(" Control + Super + v ")).toBe(hotkeyIdentity("Win+Ctrl+V"));
  expect(hotkeyIdentity("Ctrl+Alt+V")).not.toBe(hotkeyIdentity("Ctrl+Shift+V"));
  expect(hotkeyIdentity("")).toBe("");
});
