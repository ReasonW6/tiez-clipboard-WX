use std::sync::atomic::Ordering;
use tauri::{AppHandle, Emitter, Manager};
#[cfg(target_os = "windows")]
use windows::Win32::Foundation::{HINSTANCE, HWND, LPARAM, LRESULT, RECT, WPARAM};
#[cfg(target_os = "windows")]
use windows::Win32::System::LibraryLoader::GetModuleHandleW;
#[cfg(target_os = "windows")]
use windows::Win32::UI::Input::KeyboardAndMouse::{
    GetAsyncKeyState, RegisterHotKey, UnregisterHotKey, MOD_NOREPEAT, MOD_WIN, VK_CONTROL, VK_LWIN,
    VK_MENU, VK_RWIN, VK_SHIFT,
};
#[cfg(target_os = "windows")]
use windows::Win32::UI::WindowsAndMessaging::{
    CallNextHookEx, IsWindowVisible, PostThreadMessageW, SetWindowsHookExW, UnhookWindowsHookEx,
    HHOOK, KBDLLHOOKSTRUCT, MSLLHOOKSTRUCT, WH_MOUSE_LL, WM_APP, WM_KEYDOWN, WM_KEYUP,
    WM_LBUTTONDOWN, WM_MBUTTONDOWN, WM_RBUTTONDOWN, WM_SYSKEYDOWN, WM_SYSKEYUP,
};

use crate::app::window_manager::{hide_window_cmd, toggle_window};
use crate::app_state::SettingsState;
use crate::global_state::*;
use crate::infrastructure::windows_ext::WindowExt;

// Store registered hotkey IDs for cleanup
static BLOCKED_HOTKEY_IDS: std::sync::Mutex<Vec<i32>> = std::sync::Mutex::new(Vec::new());
#[cfg(target_os = "windows")]
static RECORDING_WIN_KEYS: std::sync::atomic::AtomicU32 = std::sync::atomic::AtomicU32::new(0);
#[cfg(target_os = "windows")]
static WIN_V_PRESSED: std::sync::atomic::AtomicBool = std::sync::atomic::AtomicBool::new(false);

#[cfg(target_os = "windows")]
#[derive(Debug, PartialEq)]
enum WinVAction {
    Pass,
    Suppress,
    Toggle,
}

#[cfg(target_os = "windows")]
fn win_v_action(pressed: bool, is_down: bool, enabled: bool, win_down: bool, other_modifiers: bool) -> WinVAction {
    if pressed {
        WinVAction::Suppress
    } else if is_down && enabled && win_down && !other_modifiers {
        WinVAction::Toggle
    } else {
        WinVAction::Pass
    }
}

#[cfg(target_os = "windows")]
fn recording_win_keys_after_event(pressed: u32, vk: u32, is_down: bool) -> u32 {
    let bit = match vk {
        0x5B => 1,
        0x5C => 2,
        _ => return pressed,
    };
    if is_down {
        pressed | bit
    } else {
        pressed & !bit
    }
}

#[cfg(target_os = "windows")]
fn quick_paste_index_from_vk(vk: u32) -> Option<usize> {
    match vk {
        0x31 | 0x61 => Some(0),
        0x32 | 0x62 => Some(1),
        0x33 | 0x63 => Some(2),
        0x34 | 0x64 => Some(3),
        0x35 | 0x65 => Some(4),
        0x36 | 0x66 => Some(5),
        0x37 | 0x67 => Some(6),
        0x38 | 0x68 => Some(7),
        0x39 | 0x69 => Some(8),
        0x30 | 0x60 => Some(9),
        _ => None,
    }
}

#[cfg(target_os = "windows")]
fn quick_paste_modifier_from_settings() -> String {
    if let Some(handle) = GLOBAL_APP_HANDLE.get() {
        let settings = handle.state::<SettingsState>();
        return settings
            .quick_paste_modifier
            .lock()
            .unwrap()
            .clone()
            .to_ascii_lowercase();
    }

    "disabled".to_string()
}

#[cfg(target_os = "windows")]
fn quick_paste_modifier_active(
    modifier: &str,
    ctrl_down: bool,
    alt_down: bool,
    shift_down: bool,
    win_down: bool,
) -> bool {
    match modifier {
        "disabled" => false,
        "ctrl" => ctrl_down && !alt_down && !shift_down && !win_down,
        "alt" => alt_down && !ctrl_down && !shift_down && !win_down,
        "shift" => shift_down && !ctrl_down && !alt_down && !win_down,
        "win" => win_down && !ctrl_down && !alt_down && !shift_down,
        _ => ctrl_down && !alt_down && !shift_down && !win_down,
    }
}

/// Posted to the hook thread to install or remove the mouse hook.
#[cfg(target_os = "windows")]
pub const WM_SYNC_MOUSE_HOOK: u32 = WM_APP + 0x51;

#[cfg(target_os = "windows")]
pub fn main_hwnd() -> Option<HWND> {
    match MAIN_HWND.load(Ordering::Relaxed) {
        0 => None,
        hwnd => Some(HWND(hwnd as _)),
    }
}

// A low-level mouse hook runs for every mouse event in the system, so it is
// installed only while mouse_proc has something to do: hide on clicks outside
// the visible window, a middle-button main hotkey, or recording a hotkey.
#[cfg(target_os = "windows")]
fn mouse_hook_needed(window_visible: bool, main_hotkey: &str, recording: bool) -> bool {
    window_visible
        || recording
        || main_hotkey.eq_ignore_ascii_case("MouseMiddle")
        || main_hotkey.eq_ignore_ascii_case("MButton")
}

/// Ask the hook thread to bring the mouse hook in line with the current state.
#[cfg(target_os = "windows")]
pub fn refresh_mouse_hook() {
    let thread_id = HOOK_THREAD_ID.load(Ordering::SeqCst);
    if thread_id != 0 {
        unsafe {
            let _ = PostThreadMessageW(thread_id, WM_SYNC_MOUSE_HOOK, WPARAM(0), LPARAM(0));
        }
    }
}

/// Install or remove the mouse hook. Call only on the hook thread, which owns it.
#[cfg(target_os = "windows")]
pub fn sync_mouse_hook() {
    let visible = main_hwnd().is_some_and(|hwnd| unsafe { IsWindowVisible(hwnd).as_bool() });
    let needed = mouse_hook_needed(
        visible,
        &HOTKEY_STRING.lock().unwrap(),
        IS_RECORDING.load(Ordering::SeqCst),
    );
    unsafe {
        if needed {
            if HOOK_MOUSE_HANDLE.load(Ordering::SeqCst).is_null() {
                let Ok(module) = GetModuleHandleW(None) else {
                    return;
                };
                if let Ok(hook) =
                    SetWindowsHookExW(WH_MOUSE_LL, Some(mouse_proc), Some(HINSTANCE(module.0)), 0)
                {
                    HOOK_MOUSE_HANDLE.store(hook.0 as _, Ordering::SeqCst);
                }
            }
        } else {
            let hook = HOOK_MOUSE_HANDLE.swap(std::ptr::null_mut(), Ordering::SeqCst);
            if !hook.is_null() {
                let _ = UnhookWindowsHookEx(HHOOK(hook as _));
                // The release of a button held now would never be seen.
                IS_MOUSE_BUTTON_DOWN.store(false, Ordering::SeqCst);
            }
        }
    }
}

#[tauri::command]
pub fn set_recording_mode(app_handle: AppHandle, enabled: bool) -> Result<(), String> {
    let mut ids = BLOCKED_HOTKEY_IDS.lock().unwrap();
    IS_RECORDING.store(enabled, Ordering::SeqCst);
    #[cfg(target_os = "windows")]
    refresh_mouse_hook();

    #[cfg(target_os = "windows")]
    if enabled {
        if !ids.is_empty() {
            return Ok(());
        }
        unsafe {
            let left = (GetAsyncKeyState(VK_LWIN.0 as i32) as u16 & 0x8000 != 0) as u32;
            let right = (GetAsyncKeyState(VK_RWIN.0 as i32) as u16 & 0x8000 != 0) as u32;
            RECORDING_WIN_KEYS.store(left | (right << 1), Ordering::SeqCst);
        }
        // Register ALL Win+ combinations to block system from handling them
        if let Some(window) = app_handle.get_webview_window("main") {
            if let Ok(hwnd_raw) = window.hwnd() {
                let hwnd = HWND(hwnd_raw.0);
                let mut id_counter = 0x1000i32;

                // Block Win + A-Z
                for vk in 0x41u32..=0x5Au32 {
                    unsafe {
                        if RegisterHotKey(Some(hwnd), id_counter, MOD_WIN | MOD_NOREPEAT, vk)
                            .is_ok()
                        {
                            ids.push(id_counter);
                        }
                    }
                    id_counter += 1;
                }

                // Block Win + 0-9
                for vk in 0x30u32..=0x39u32 {
                    unsafe {
                        if RegisterHotKey(Some(hwnd), id_counter, MOD_WIN | MOD_NOREPEAT, vk)
                            .is_ok()
                        {
                            ids.push(id_counter);
                        }
                    }
                    id_counter += 1;
                }

                // Block special keys
                let special_keys = [0x20u32, 0x0D, 0x09, 0x1B, 0x2C]; // Space, Enter, Tab, Esc, PrintScreen
                for vk in special_keys {
                    unsafe {
                        if RegisterHotKey(Some(hwnd), id_counter, MOD_WIN | MOD_NOREPEAT, vk)
                            .is_ok()
                        {
                            ids.push(id_counter);
                        }
                    }
                    id_counter += 1;
                }
                println!("Recording mode ON: Blocked {} Win+ combinations", ids.len());
            }
        }
    } else {
        // Unregister all blocked hotkeys
        if let Some(window) = app_handle.get_webview_window("main") {
            if let Ok(hwnd_raw) = window.hwnd() {
                let hwnd = HWND(hwnd_raw.0);
                for id in ids.drain(..) {
                    unsafe {
                        let _ = UnregisterHotKey(Some(hwnd), id);
                    }
                }
                println!("Recording mode OFF: Released blocked hotkeys");
            }
        }
    }

    Ok(())
}

pub fn cancel_recording(app: &AppHandle) {
    let has_blocked_keys = !BLOCKED_HOTKEY_IDS.lock().unwrap().is_empty();
    if IS_RECORDING.load(Ordering::SeqCst) || has_blocked_keys {
        let _ = set_recording_mode(app.clone(), false);
        let _ = app.emit("recording-cancelled", ());
    }
}

// Low-level Keyboard Hook Procedure
#[cfg(target_os = "windows")]
pub unsafe extern "system" fn keyboard_proc(
    n_code: i32,
    w_param: WPARAM,
    l_param: LPARAM,
) -> LRESULT {
    let msg = w_param.0 as u32;
    let is_down = msg == WM_KEYDOWN || msg == WM_SYSKEYDOWN;
    let is_up = msg == WM_KEYUP || msg == WM_SYSKEYUP;

    if n_code >= 0 && (is_down || is_up) {
        let kbd_struct = *(l_param.0 as *const KBDLLHOOKSTRUCT);
        let vk = kbd_struct.vkCode;

        // Handle Recording Mode - Black Hole Logic
        if IS_RECORDING.load(Ordering::SeqCst) {
            // Win events are swallowed, so Windows' async key state cannot track them.
            let _ = RECORDING_WIN_KEYS.fetch_update(Ordering::SeqCst, Ordering::SeqCst, |pressed| {
                Some(recording_win_keys_after_event(pressed, vk, is_down))
            });
            let ctrl_down = GetAsyncKeyState(VK_CONTROL.0 as i32) as u16 & 0x8000 != 0;
            let shift_down = GetAsyncKeyState(VK_SHIFT.0 as i32) as u16 & 0x8000 != 0;
            let alt_down = GetAsyncKeyState(VK_MENU.0 as i32) as u16 & 0x8000 != 0;
            let win_down = RECORDING_WIN_KEYS.load(Ordering::SeqCst) != 0;

            // ESC to cancel
            if vk == 0x1B && is_down {
                IS_RECORDING.store(false, Ordering::SeqCst);
                if let Some(handle) = GLOBAL_APP_HANDLE.get() {
                    let _ = handle.emit("recording-cancelled", ());
                }
                return LRESULT(1);
            }

            let is_win = vk == 0x5B || vk == 0x5C;
            let is_other_modifier = (vk >= 0x10 && vk <= 0x12) || (vk >= 0xA0 && vk <= 0xA5);

            if is_other_modifier {
                return CallNextHookEx(None, n_code, w_param, l_param);
            }

            if !is_win && is_down {
                if let Some(handle) = GLOBAL_APP_HANDLE.get() {
                    let key_name = match vk {
                        0x20 => "Space".to_string(),
                        0x0D => "Enter".to_string(),
                        0x09 => "Tab".to_string(),
                        0x08 => "Backspace".to_string(),
                        0x2E => "Delete".to_string(),
                        0x2D => "Insert".to_string(),
                        0x21 => "PageUp".to_string(),
                        0x22 => "PageDown".to_string(),
                        0x23 => "End".to_string(),
                        0x24 => "Home".to_string(),
                        0x25 => "Left".to_string(),
                        0x26 => "Up".to_string(),
                        0x27 => "Right".to_string(),
                        0x28 => "Down".to_string(),
                        0xBB => "Plus".to_string(),
                        0xBC => "Comma".to_string(),
                        0xBD => "Minus".to_string(),
                        0xBE => "Period".to_string(),
                        0xBF => "/".to_string(),
                        0xC0 => "`".to_string(),
                        0xBA => ";".to_string(),
                        0xDB => "[".to_string(),
                        0xDC => "\\".to_string(),
                        0xDD => "]".to_string(),
                        0xDE => "'".to_string(),
                        k if k >= 0x70 && k <= 0x87 => format!("F{}", k - 0x6F),
                        k if (k >= 0x30 && k <= 0x39) || (k >= 0x41 && k <= 0x5A) => {
                            format!("{}", char::from_u32(k).unwrap())
                        }
                        _ => format!("Key_{}", vk),
                    };

                    let final_hotkey = if vk == 0x08 || vk == 0x2E {
                        String::new()
                    } else {
                        format!(
                            "{}{}{}{}{}",
                            if ctrl_down { "Ctrl+" } else { "" },
                            if shift_down { "Shift+" } else { "" },
                            if alt_down { "Alt+" } else { "" },
                            if win_down { "Win+" } else { "" },
                            key_name
                        )
                    };

                    println!("Recorded Hotkey: {}", final_hotkey);
                    let _ = handle.emit("hotkey-recorded", final_hotkey);
                    IS_RECORDING.store(false, Ordering::SeqCst);
                }
            }
            return LRESULT(1);
        }

        // Win+V is reserved by the shell, so consume it before Windows sees it.
        if vk == 0x56 {
            let enabled = TARGET_HOTKEY.lock().unwrap().is_some();
            let win_down = (GetAsyncKeyState(VK_LWIN.0 as i32) as u16 & 0x8000 != 0)
                || (GetAsyncKeyState(VK_RWIN.0 as i32) as u16 & 0x8000 != 0);
            let other_modifiers = [VK_CONTROL, VK_SHIFT, VK_MENU].iter()
                .any(|key| GetAsyncKeyState(key.0 as i32) as u16 & 0x8000 != 0);
            let action = win_v_action(WIN_V_PRESSED.load(Ordering::Relaxed), is_down,
                enabled, win_down, other_modifiers);
            if action != WinVAction::Pass {
                WIN_V_PRESSED.store(is_down, Ordering::Relaxed);
                if action == WinVAction::Toggle {
                    if let Some(handle) = GLOBAL_APP_HANDLE.get() {
                        let app = handle.clone();
                        let _ = handle.run_on_main_thread(move || toggle_window(&app));
                    }
                }
                return LRESULT(1);
            }
        }

        // 3. Global Paste Sound Trigger (Ctrl+V)
        {
            let ctrl_down = (GetAsyncKeyState(VK_CONTROL.0 as i32) as u16 & 0x8000) != 0;
            let alt_down = (GetAsyncKeyState(VK_MENU.0 as i32) as u16 & 0x8000) != 0;
            let shift_down = (GetAsyncKeyState(VK_SHIFT.0 as i32) as u16 & 0x8000) != 0;
            let win_down = (GetAsyncKeyState(VK_LWIN.0 as i32) as u16 & 0x8000 != 0)
                || (GetAsyncKeyState(VK_RWIN.0 as i32) as u16 & 0x8000 != 0);

            if vk == 0x56 && ctrl_down && !alt_down && !shift_down && !win_down {
                if is_down {
                    if let Some(handle) = GLOBAL_APP_HANDLE.get() {
                        let settings = handle.state::<SettingsState>();
                        if settings.sound_enabled.load(Ordering::Relaxed) {
                            std::thread::spawn(move || {
                                let _ = handle.emit("play-sound", "paste");
                            });
                        }
                    }
                }
            }
        }

        // 4. Quick Paste by Modifier+Number
        {
            let ctrl_down = (GetAsyncKeyState(VK_CONTROL.0 as i32) as u16 & 0x8000) != 0;
            let alt_down = (GetAsyncKeyState(VK_MENU.0 as i32) as u16 & 0x8000) != 0;
            let shift_down = (GetAsyncKeyState(VK_SHIFT.0 as i32) as u16 & 0x8000) != 0;
            let win_down = (GetAsyncKeyState(VK_LWIN.0 as i32) as u16 & 0x8000 != 0)
                || (GetAsyncKeyState(VK_RWIN.0 as i32) as u16 & 0x8000 != 0);
            let quick_paste_modifier = quick_paste_modifier_from_settings();

            if is_up
                && matches!(
                    vk,
                    0x11 | 0xA2 | 0xA3 | 0x12 | 0xA4 | 0xA5 | 0x10 | 0xA0 | 0xA1 | 0x5B | 0x5C
                )
            {
                QUICK_PASTE_DIGIT_MASK.store(0, Ordering::SeqCst);
            }

            if let Some(index) = quick_paste_index_from_vk(vk) {
                let bit = 1u32 << index;

                if is_up {
                    let pressed_mask = QUICK_PASTE_DIGIT_MASK.fetch_and(!bit, Ordering::SeqCst);
                    if pressed_mask & bit != 0 {
                        return LRESULT(1);
                    }
                }

                if is_down
                    && quick_paste_modifier_active(
                        &quick_paste_modifier,
                        ctrl_down,
                        alt_down,
                        shift_down,
                        win_down,
                    )
                {
                    let pressed_mask = QUICK_PASTE_DIGIT_MASK.fetch_or(bit, Ordering::SeqCst);
                    if pressed_mask & bit != 0 {
                        return LRESULT(1);
                    }

                    if let Some(handle) = GLOBAL_APP_HANDLE.get() {
                        let handle_clone = handle.clone();
                        tauri::async_runtime::spawn(async move {
                            if let Err(err) =
                                crate::services::clipboard_ops::paste_history_item_by_index(
                                    handle_clone,
                                    index,
                                )
                                .await
                            {
                                eprintln!(
                                    "[ERROR] Quick paste by {}+{} failed: {}",
                                    quick_paste_modifier,
                                    index + 1,
                                    err
                                );
                            }
                        });
                    }

                    return LRESULT(1);
                }
            }
        }

        // 5. Global Navigation Keys (Up/Down, Enter, Esc)
        if NAVIGATION_ENABLED.load(Ordering::SeqCst) && !IS_RECORDING.load(Ordering::SeqCst) {
            if IS_HIDDEN.load(Ordering::Relaxed) {
                return CallNextHookEx(None, n_code, w_param, l_param);
            }
            let allow_navigation = if let Some(handle) = GLOBAL_APP_HANDLE.get() {
                let settings = handle.state::<SettingsState>();
                settings.arrow_key_selection.load(Ordering::Relaxed)
            } else {
                true
            };

            if !allow_navigation {
                return CallNextHookEx(None, n_code, w_param, l_param);
            }

            let is_navigation_key = vk == 0x26 || vk == 0x28 || vk == 0x0D || vk == 0x1B;
            let is_enter = vk == 0x0D;
            let _is_escape = vk == 0x1B;

            if is_navigation_key && is_down {
                // Only Enter requires navigation mode to be active
                // Escape can always close the window when it's visible
                if is_enter && !NAVIGATION_MODE_ACTIVE.load(Ordering::Relaxed) {
                    return CallNextHookEx(None, n_code, w_param, l_param);
                }
                let ctrl_down = (GetAsyncKeyState(VK_CONTROL.0 as i32) as u16 & 0x8000) != 0;
                let alt_down = (GetAsyncKeyState(VK_MENU.0 as i32) as u16 & 0x8000) != 0;
                let win_down = (GetAsyncKeyState(VK_LWIN.0 as i32) as u16 & 0x8000 != 0)
                    || (GetAsyncKeyState(VK_RWIN.0 as i32) as u16 & 0x8000 != 0);

                if !ctrl_down && !alt_down && !win_down {
                    if let Some(handle) = GLOBAL_APP_HANDLE.get() {
                        let action = match vk {
                            0x26 => "up",
                            0x28 => "down",
                            0x0D => "enter",
                            0x1B => "escape",
                            _ => "",
                        };

                        if !action.is_empty() {
                            if vk == 0x26 || vk == 0x28 {
                                NAVIGATION_MODE_ACTIVE.store(true, Ordering::Relaxed);
                            } else if vk == 0x1B {
                                NAVIGATION_MODE_ACTIVE.store(false, Ordering::Relaxed);
                            }
                            if action == "escape" {
                                let handle_clone = handle.clone();
                                tauri::async_runtime::spawn(async move {
                                    let _ = handle_clone.emit("navigation-action", "escape");
                                    toggle_window(&handle_clone);
                                });
                            } else {
                                let _ = handle.emit("navigation-action", action);
                            }
                            return LRESULT(1);
                        }
                    }
                }
            }
        }
    }
    CallNextHookEx(None, n_code, w_param, l_param)
}

// Low-level Mouse Hook Procedure
#[cfg(target_os = "windows")]
pub unsafe extern "system" fn mouse_proc(n_code: i32, w_param: WPARAM, l_param: LPARAM) -> LRESULT {
    if n_code >= 0 {
        let msg = w_param.0 as u32;
        if msg == WM_MBUTTONDOWN
            || msg == WM_LBUTTONDOWN
            || msg == WM_RBUTTONDOWN
            || msg == windows::Win32::UI::WindowsAndMessaging::WM_LBUTTONUP
            || msg == windows::Win32::UI::WindowsAndMessaging::WM_RBUTTONUP
        {
            // Track mouse state globally
            if msg == WM_LBUTTONDOWN || msg == WM_RBUTTONDOWN {
                IS_MOUSE_BUTTON_DOWN.store(true, Ordering::SeqCst);
            } else if msg == windows::Win32::UI::WindowsAndMessaging::WM_LBUTTONUP
                || msg == windows::Win32::UI::WindowsAndMessaging::WM_RBUTTONUP
            {
                IS_MOUSE_BUTTON_DOWN.store(false, Ordering::SeqCst);
                return CallNextHookEx(None, n_code, w_param, l_param); // Return early for up events
            }

            // Handle Recording Mode
            if IS_RECORDING.load(Ordering::SeqCst) && msg == WM_MBUTTONDOWN {
                IS_RECORDING.store(false, Ordering::SeqCst);
                if let Some(handle) = GLOBAL_APP_HANDLE.get() {
                    let _ = handle.emit("hotkey-recorded", "MouseMiddle");
                }
                return LRESULT(1);
            }

            // Click Elsewhere to Hide Logic
            if msg == WM_LBUTTONDOWN || msg == WM_RBUTTONDOWN {
                if let Some(handle) = GLOBAL_APP_HANDLE.get() {
                    if let Some(window) = handle.get_webview_window("main") {
                        if !IGNORE_BLUR.load(Ordering::Relaxed) {
                            let mouse_struct = *(l_param.0 as *const MSLLHOOKSTRUCT);
                            let point = mouse_struct.pt;

                            if let Some(main_hwnd) = main_hwnd() {
                                if !WindowExt::is_window_visible(main_hwnd) {
                                    return CallNextHookEx(None, n_code, w_param, l_param);
                                }
                                let mut rect = RECT::default();
                                let _ = windows::Win32::UI::WindowsAndMessaging::GetWindowRect(
                                    main_hwnd, &mut rect,
                                );

                                // Boundary check: Is point outside the rect? (with 5px margin of safety)
                                let margin = 5;
                                let is_outside = point.x < rect.left - margin
                                    || point.x > rect.right + margin
                                    || point.y < rect.top - margin
                                    || point.y > rect.bottom + margin;

                                if is_outside {
                                    // Status check before hiding
                                    if !WindowExt::is_window_visible(main_hwnd) {
                                        return CallNextHookEx(None, n_code, w_param, l_param);
                                    }

                                    if WINDOW_PINNED.load(Ordering::Relaxed) {
                                        // Pinned: Just reset focusable state to ensure we don't retain focus
                                        let _ = window.set_focusable(false);
                                    } else {
                                        let _ = hide_window_cmd(handle.clone());
                                    }
                                }
                            }
                        }
                    }
                }
            }

            // Handle configured middle mouse hotkey
            if msg == WM_MBUTTONDOWN {
                let current = HOTKEY_STRING.lock().unwrap().to_lowercase();
                if current == "mousemiddle" || current == "mbutton" {
                    if let Some(handle) = GLOBAL_APP_HANDLE.get() {
                        let app = handle.clone();
                        let _ = handle.run_on_main_thread(move || toggle_window(&app));
                    }
                    return LRESULT(1);
                }
            }
        }
    }

    CallNextHookEx(None, n_code, w_param, l_param)
}

pub fn parse_hotkey_for_hook(hotkey: &str) -> Option<HookHotkey> {
    let parts: Vec<&str> = hotkey.split('+').collect();
    let mut vk = 0u32;
    let mut ctrl = false;
    let mut shift = false;
    let mut alt = false;
    let mut win = false;

    for part in parts {
        let part_upper = part.trim().to_uppercase();
        match part_upper.as_str() {
            "CTRL" | "CONTROL" => ctrl = true,
            "SHIFT" => shift = true,
            "ALT" | "MENU" => alt = true,
            "SUPER" | "WIN" | "COMMAND" | "META" => win = true,
            "SPACE" => vk = 0x20,
            "ENTER" | "RETURN" => vk = 0x0D,
            "TAB" => vk = 0x09,
            "BACKSPACE" => vk = 0x08,
            "DELETE" => vk = 0x2E,
            "INSERT" => vk = 0x2D,
            "PAGEUP" => vk = 0x21,
            "PAGEDOWN" => vk = 0x22,
            "END" => vk = 0x23,
            "HOME" => vk = 0x24,
            "LEFT" => vk = 0x25,
            "UP" => vk = 0x26,
            "RIGHT" => vk = 0x27,
            "DOWN" => vk = 0x28,
            "PLUS" | "=" => vk = 0xBB,
            "COMMA" | "," => vk = 0xBC,
            "MINUS" | "-" => vk = 0xBD,
            "PERIOD" | "." => vk = 0xBE,
            "/" | "SLASH" => vk = 0xBF,
            "`" | "TILDE" | "GRAVE" => vk = 0xC0,
            ";" | "SEMICOLON" => vk = 0xBA,
            "[" | "LBRACKET" => vk = 0xDB,
            "\\" | "BACKSLASH" => vk = 0xDC,
            "]" | "RBRACKET" => vk = 0xDD,
            "'" | "QUOTE" => vk = 0xDE,
            key if key.starts_with('F') && key.len() > 1 => {
                if let Ok(num) = key[1..].parse::<u32>() {
                    if (1..=24).contains(&num) {
                        vk = 0x6F + num;
                    }
                }
            }
            key => {
                if key.len() == 1 {
                    vk = key.chars().next().unwrap() as u32;
                }
            }
        }
    }

    if vk != 0 {
        Some(HookHotkey {
            vk,
            ctrl,
            shift,
            alt,
            win,
        })
    } else {
        None
    }
}

pub fn is_win_v_hotkey(hotkey: &str) -> bool {
    let parts: Vec<String> = hotkey
        .split('+')
        .map(|p| p.trim().to_uppercase())
        .filter(|p| !p.is_empty())
        .collect();

    if parts.is_empty() {
        return false;
    }

    let mut has_win = false;
    let mut has_v = false;

    for part in &parts {
        match part.as_str() {
            "WIN" | "SUPER" | "COMMAND" | "META" => has_win = true,
            "V" => has_v = true,
            _ => return false,
        }
    }

    has_win && has_v
}

#[cfg(all(test, target_os = "windows"))]
mod recording_tests {
    use super::{
        is_win_v_hotkey, mouse_hook_needed, recording_win_keys_after_event, win_v_action,
        WinVAction,
    };

    #[test]
    fn mouse_hook_is_needed_only_while_something_consumes_clicks() {
        assert!(!mouse_hook_needed(false, "Ctrl+Shift+V", false));
        assert!(!mouse_hook_needed(false, "", false));
        assert!(mouse_hook_needed(true, "Ctrl+Shift+V", false));
        assert!(mouse_hook_needed(false, "MouseMiddle", false));
        assert!(mouse_hook_needed(false, "mbutton", false));
        assert!(mouse_hook_needed(false, "Win+V", true));
    }

    #[test]
    fn win_v_takeover_handles_shell_reserved_aliases_only() {
        for shortcut in ["Win+V", "Super+V", "V+Meta", "command+v"] {
            assert!(is_win_v_hotkey(shortcut));
        }
        for shortcut in ["V", "Ctrl+V", "Win+Shift+V", "Win+C", ""] {
            assert!(!is_win_v_hotkey(shortcut));
        }
    }

    #[test]
    fn win_v_consumes_repeat_and_release_after_window_releases_win_keys() {
        assert_eq!(win_v_action(false, true, true, true, false), WinVAction::Toggle);
        assert_eq!(win_v_action(true, true, true, false, false), WinVAction::Suppress);
        assert_eq!(win_v_action(true, false, false, false, false), WinVAction::Suppress);
        assert_eq!(win_v_action(false, false, true, true, false), WinVAction::Pass);
    }

    #[test]
    fn win_v_does_not_capture_disabled_or_other_shortcuts() {
        assert_eq!(win_v_action(false, true, false, true, false), WinVAction::Pass);
        assert_eq!(win_v_action(false, true, true, false, false), WinVAction::Pass);
        assert_eq!(win_v_action(false, true, true, true, true), WinVAction::Pass);
    }

    #[test]
    fn swallowed_win_down_is_retained_until_key_up() {
        let pressed = recording_win_keys_after_event(0, 0x5B, true);
        let pressed = recording_win_keys_after_event(pressed, 0x56, true);
        assert_ne!(pressed, 0, "Win+V must retain its Win modifier");
        assert_eq!(recording_win_keys_after_event(pressed, 0x5B, false), 0);
    }

    #[test]
    fn releasing_one_win_key_keeps_the_other_pressed() {
        let pressed = recording_win_keys_after_event(0, 0x5B, true);
        let pressed = recording_win_keys_after_event(pressed, 0x5C, true);
        let pressed = recording_win_keys_after_event(pressed, 0x5B, false);
        assert_ne!(pressed, 0);
        assert_eq!(recording_win_keys_after_event(pressed, 0x5C, false), 0);
    }
}
