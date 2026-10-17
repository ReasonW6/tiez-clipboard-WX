import { useEffect, useRef } from "react";
import { invoke } from "@tauri-apps/api/core";
import { getHotkeyDisplayTokens } from "../../../shared/lib/hotkeyDisplay";
import { isMacPlatform } from "../../../shared/lib/platform";

interface HotkeyRecorderProps {
  hotkey: string;
  isRecording: boolean;
  setIsRecording: (value: boolean) => void;
  onRecord: (hotkey: string) => void;
  t: (key: string) => string;
  disabled?: boolean;
}

export default function HotkeyRecorder({ hotkey, isRecording, setIsRecording, onRecord, t, disabled }: HotkeyRecorderProps) {
  const field = useRef<HTMLDivElement>(null);
  const captured = useRef(false);
  const tokens = getHotkeyDisplayTokens(hotkey);
  const label = tokens.map(token => token.label).join(isMacPlatform() ? " " : " + ");

  useEffect(() => () => setIsRecording(false), [setIsRecording]);

  useEffect(() => {
    if (!isRecording) return;
    captured.current = false;
    const cancel = () => setIsRecording(false);
    const outside = (event: PointerEvent) => {
      if (!field.current?.contains(event.target as Node)) cancel();
    };
    window.addEventListener("blur", cancel);
    document.addEventListener("pointerdown", outside, true);
    return () => {
      window.removeEventListener("blur", cancel);
      document.removeEventListener("pointerdown", outside, true);
    };
  }, [isRecording, setIsRecording]);

  return <div
    ref={field}
    className={`key-group ${isRecording ? "recording" : ""}`}
    tabIndex={disabled ? -1 : 0}
    aria-disabled={disabled || undefined}
    style={disabled ? { cursor: "not-allowed", opacity: 0.6 } : undefined}
    onClick={async event => {
      if (disabled) return;
      const target = event.currentTarget;
      target.focus();
      try {
        await invoke("focus_clipboard_window");
        if (target.isConnected && document.activeElement === target) setIsRecording(true);
      } catch (error) {
        console.error(error);
      }
    }}
    onBlur={() => setIsRecording(false)}
    onKeyDown={event => {
      if (!isRecording || captured.current || event.nativeEvent.isComposing) return;
      event.preventDefault();
      event.stopPropagation();
      if (event.key === "Escape") {
        setIsRecording(false);
        return;
      }
      if (["Control", "Shift", "Alt", "Meta", "OS"].includes(event.key)) return;
      captured.current = true;
      if (event.key === "Backspace" || event.key === "Delete") {
        onRecord("");
        return;
      }
      const modifiers = [];
      if (event.ctrlKey) modifiers.push("Ctrl");
      if (event.shiftKey) modifiers.push("Shift");
      if (event.altKey) modifiers.push("Alt");
      if (event.metaKey) modifiers.push(isMacPlatform() ? "Command" : "Win");
      const aliases: Record<string, string> = {
        " ": "Space", ArrowLeft: "Left", ArrowUp: "Up", ArrowRight: "Right", ArrowDown: "Down",
        "=": "Plus", "+": "Plus", ",": "Comma", "-": "Minus", ".": "Period"
      };
      // Physical letter keys remain stable under IME and shifted keyboard layouts.
      const key = /^Key[A-Z]$/.test(event.code) ? event.code.slice(3)
        : /^Digit[0-9]$/.test(event.code) ? event.code.slice(5)
        : aliases[event.key] || event.key.toUpperCase();
      onRecord([...modifiers, key].join("+"));
    }}
  >
    {isRecording ? <div className="key-cap" style={{ width: "8em" }}>{t("waiting_for_input")}</div>
      : label ? <div className="key-cap key-cap-chord" title={label}>{label}</div>
        : <div className="key-cap" style={{ width: "8em", opacity: 0.5 }}>{t("not_set")}</div>}
  </div>;
}
