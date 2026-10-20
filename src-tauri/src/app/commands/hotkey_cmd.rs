use crate::app_state::SettingsState;
use crate::error::{AppError, AppResult};
use crate::global_state::HOTKEY_STRING;
use std::sync::atomic::Ordering;
use tauri::{AppHandle, Manager};
use tauri_plugin_global_shortcut::{GlobalShortcutExt, Shortcut};

fn register_shortcut(app_handle: &AppHandle, hotkey: &str) {
    if hotkey.is_empty()
        || hotkey.eq_ignore_ascii_case("MouseMiddle")
        || hotkey.eq_ignore_ascii_case("MButton")
    {
        return;
    }

    let normalized = hotkey.replace("Win", "Super");
    if let Ok(shortcut) = normalized.parse::<Shortcut>() {
        let _ = app_handle.global_shortcut().register(shortcut);
    }
}

pub(crate) fn sync_registered_hotkeys(app_handle: &AppHandle) -> AppResult<()> {
    let _ = app_handle.global_shortcut().unregister_all();

    let Some(settings) = app_handle.try_state::<SettingsState>() else {
        return Ok(());
    };

    let main_hotkey = settings.main_hotkey.lock().unwrap().clone();
    let hook_handles_main = cfg!(target_os = "windows") && crate::app::hooks::is_win_v_hotkey(&main_hotkey);
    *crate::global_state::TARGET_HOTKEY.lock().unwrap() = if hook_handles_main {
        crate::app::hooks::parse_hotkey_for_hook(&main_hotkey)
    } else {
        None
    };
    if !hook_handles_main {
        register_shortcut(app_handle, &main_hotkey);
    }

    let sequential_mode = settings.sequential_mode.load(Ordering::Relaxed);
    let sequential_hotkey = settings.sequential_paste_hotkey.lock().unwrap().clone();
    if sequential_mode {
        register_shortcut(app_handle, &sequential_hotkey);
    }

    let rich_hotkey = settings.rich_paste_hotkey.lock().unwrap().clone();
    register_shortcut(app_handle, &rich_hotkey);

    let search_hotkey = settings.search_hotkey.lock().unwrap().clone();
    register_shortcut(app_handle, &search_hotkey);

    // A middle-button main hotkey keeps the mouse hook installed while the window is hidden.
    #[cfg(target_os = "windows")]
    crate::app::hooks::refresh_mouse_hook();

    Ok(())
}

#[tauri::command]
pub fn register_hotkey(app_handle: AppHandle, hotkey: String) -> AppResult<()> {
    {
        let mut guard = HOTKEY_STRING.lock().unwrap();
        *guard = hotkey.clone();
    }

    if let Some(settings) = app_handle.try_state::<SettingsState>() {
        let mut guard = settings.main_hotkey.lock().unwrap();
        *guard = hotkey.clone();
    }

    sync_registered_hotkeys(&app_handle)
}

#[tauri::command]
pub fn test_hotkey_available(app_handle: AppHandle, hotkey: String, current_hotkey: Option<String>) -> AppResult<bool> {
    if hotkey.is_empty()
        || hotkey.eq_ignore_ascii_case("MouseMiddle")
        || hotkey.eq_ignore_ascii_case("MButton")
    {
        return Ok(true);
    }

    let normalized = hotkey.replace("Win", "Super");
    let shortcut = normalized
        .parse::<Shortcut>()
        .map_err(|_| AppError::Validation("快捷键格式无效".to_string()))?;

    // Re-recording one of our own shortcuts must not unregister it during the probe.
    if app_handle.global_shortcut().is_registered(shortcut) {
        if current_hotkey.and_then(|value| value.replace("Win", "Super").parse::<Shortcut>().ok()) == Some(shortcut) {
            return Ok(true);
        }
        return Err(AppError::Validation("该快捷键已分配给其他功能".to_string()));
    }

    match app_handle.global_shortcut().register(shortcut.clone()) {
        Ok(_) => {
            let _ = app_handle.global_shortcut().unregister(shortcut);
            Ok(true)
        }
        Err(e) => {
            let err_str = format!("{:?}", e);
            let user_msg = if err_str.contains("AlreadyRegistered") {
                "该快捷键已被其他程序占用".to_string()
            } else {
                "快捷键不可用".to_string()
            };
            Err(AppError::Internal(user_msg))
        }
    }
}
