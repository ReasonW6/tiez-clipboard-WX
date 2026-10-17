//! Let the main WebView sleep while its window is hidden.
//!
//! Tauri hides only the top-level window, so WebView2 still reports the page as
//! visible: requestAnimationFrame keeps running at the display refresh rate and
//! the renderer keeps its full working set. Mirror the window's visibility into
//! the controller and lower the memory target until the window is shown again.

use std::cell::{Cell, RefCell};
use tauri::WebviewWindow;
use webview2_com::Microsoft::Web::WebView2::Win32::{
    ICoreWebView2Controller, ICoreWebView2_19, COREWEBVIEW2_MEMORY_USAGE_TARGET_LEVEL_LOW,
    COREWEBVIEW2_MEMORY_USAGE_TARGET_LEVEL_NORMAL,
};
use windows::core::Interface;
use windows::Win32::Foundation::{HWND, LPARAM, LRESULT, WPARAM};
use windows::Win32::UI::Shell::{DefSubclassProc, RemoveWindowSubclass, SetWindowSubclass};
use windows::Win32::UI::WindowsAndMessaging::{
    IsWindowVisible, KillTimer, SetTimer, SWP_HIDEWINDOW, SWP_SHOWWINDOW, WINDOWPOS, WM_NCDESTROY,
    WM_TIMER, WM_WINDOWPOSCHANGED, WM_WINDOWPOSCHANGING,
};

const SUBCLASS_ID: usize = 0x5445_5a57;
const SLEEP_TIMER_ID: usize = 0x5445_5a58;
// Hiding resets the page (settings closed, search cleared). Its last painted
// frame is what appears first when the window is shown again, so let it paint
// that reset state, including the header's layout animation, before sleeping.
const SLEEP_DELAY_MS: u32 = 600;

thread_local! {
    // The controller and the subclass both live on the window's UI thread.
    static CONTROLLER: RefCell<Option<ICoreWebView2Controller>> = const { RefCell::new(None) };
    static ACTIVE: Cell<Option<bool>> = const { Cell::new(None) };
}

pub fn install(window: &WebviewWindow) {
    let Ok(hwnd) = window.hwnd() else { return };
    let hwnd = hwnd.0 as isize;
    let _ = window.with_webview(move |webview| {
        let hwnd = HWND(hwnd as _);
        CONTROLLER.with(|slot| *slot.borrow_mut() = Some(webview.controller()));
        unsafe {
            if SetWindowSubclass(hwnd, Some(subclass_proc), SUBCLASS_ID, 0).as_bool() {
                set_active(IsWindowVisible(hwnd).as_bool());
            }
        }
    });
}

fn set_active(active: bool) {
    if ACTIVE.with(|state| state.replace(Some(active))) == Some(active) {
        return;
    }
    let Some(controller) = CONTROLLER.with(|slot| slot.borrow().clone()) else {
        return;
    };
    unsafe {
        let level = if active {
            COREWEBVIEW2_MEMORY_USAGE_TARGET_LEVEL_NORMAL
        } else {
            COREWEBVIEW2_MEMORY_USAGE_TARGET_LEVEL_LOW
        };
        // Restore the memory target before the page paints again, and lower it
        // only after the page has stopped painting.
        let set_level = || {
            if let Ok(webview) = controller
                .CoreWebView2()
                .and_then(|w| w.cast::<ICoreWebView2_19>())
            {
                let _ = webview.SetMemoryUsageTargetLevel(level);
            }
        };
        if active {
            set_level();
        }
        let _ = controller.SetIsVisible(active);
        if !active {
            set_level();
        }
    }
}

unsafe extern "system" fn subclass_proc(
    hwnd: HWND,
    msg: u32,
    wparam: WPARAM,
    lparam: LPARAM,
    _id: usize,
    _data: usize,
) -> LRESULT {
    if msg == WM_WINDOWPOSCHANGING || msg == WM_WINDOWPOSCHANGED {
        let flags = (*(lparam.0 as *const WINDOWPOS)).flags;
        // Wake before the window appears so the first visible frame is current.
        if msg == WM_WINDOWPOSCHANGING && flags.contains(SWP_SHOWWINDOW) {
            let _ = KillTimer(Some(hwnd), SLEEP_TIMER_ID);
            set_active(true);
        }
        if msg == WM_WINDOWPOSCHANGED && flags.contains(SWP_HIDEWINDOW) {
            if SetTimer(Some(hwnd), SLEEP_TIMER_ID, SLEEP_DELAY_MS, None) == 0 {
                set_active(false);
            }
        }
    }
    if msg == WM_TIMER && wparam.0 == SLEEP_TIMER_ID {
        let _ = KillTimer(Some(hwnd), SLEEP_TIMER_ID);
        if !IsWindowVisible(hwnd).as_bool() {
            set_active(false);
        }
        return LRESULT(0);
    }
    if msg == WM_NCDESTROY {
        let _ = RemoveWindowSubclass(hwnd, Some(subclass_proc), SUBCLASS_ID);
        CONTROLLER.with(|slot| slot.borrow_mut().take());
    }
    DefSubclassProc(hwnd, msg, wparam, lparam)
}
