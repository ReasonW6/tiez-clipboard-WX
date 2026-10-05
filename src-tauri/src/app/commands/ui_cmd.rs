use crate::app_state::SettingsState;
use crate::database::DbState;
use crate::error::{AppError, AppResult};
use crate::infrastructure::repository::settings_repo::SettingsRepository;
use serde::Serialize;
use tauri::{AppHandle, Emitter, State, Theme, WebviewWindow};
use tauri_plugin_notification::NotificationExt;

#[derive(Debug, Serialize)]
pub struct PlatformInfo {
    pub platform: String,
    pub is_windows_10: bool,
    pub is_windows_11: bool,
}

#[tauri::command]
pub fn get_platform_info() -> PlatformInfo {
    #[cfg(target_os = "windows")]
    {
        let build = windows_version::OsVersion::current().build;
        let is_windows_11 = build >= 22000;
        let is_windows_10 = build >= 10240 && build < 22000;
        PlatformInfo {
            platform: "windows".to_string(),
            is_windows_10,
            is_windows_11,
        }
    }

    #[cfg(target_os = "macos")]
    {
        PlatformInfo {
            platform: "macos".to_string(),
            is_windows_10: false,
            is_windows_11: false,
        }
    }

    #[cfg(all(not(target_os = "windows"), not(target_os = "macos")))]
    {
        PlatformInfo {
            platform: "other".to_string(),
            is_windows_10: false,
            is_windows_11: false,
        }
    }
}

#[tauri::command]
pub fn send_system_notification(app: AppHandle, title: String, body: String) -> AppResult<()> {
    app.notification()
        .builder()
        .title(title)
        .body(body)
        .show()
        .map_err(|err| AppError::Internal(format!("发送系统通知失败: {}", err)))?;

    Ok(())
}

#[tauri::command]
pub fn set_theme(
    window: WebviewWindow,
    state: State<'_, SettingsState>,
    db_state: State<'_, DbState>,
    theme: String,
    color_mode: Option<String>,
    show_app_border: Option<bool>,
    surface_opacity: Option<f64>,
) -> AppResult<()> {
    let mut effective_color_mode = color_mode.clone();
    if effective_color_mode
        .as_deref()
        .map(|v| v.trim().is_empty())
        .unwrap_or(true)
    {
        effective_color_mode = db_state
            .settings_repo
            .get("app.color_mode")
            .unwrap_or(Some("system".to_string()));
    }
    let mut effective_show_app_border = show_app_border;
    if effective_show_app_border.is_none() {
        effective_show_app_border = db_state
            .settings_repo
            .get("app.show_app_border")
            .unwrap_or(Some("true".to_string()))
            .map(|v| v != "false");
    }
    let show_border = effective_show_app_border.unwrap_or(true);

    // Set Tao's preferred theme too. A DWM attribute alone is overwritten by
    // WM_SETTINGCHANGE (including the system theme refresh during logon).
    let preferred_theme = match effective_color_mode.as_deref() {
        Some("light") => Some(Theme::Light),
        Some("dark") => Some(Theme::Dark),
        _ => None,
    };
    window.set_theme(preferred_theme).map_err(|e| AppError::Internal(e.to_string()))?;

    if let Ok(mut guard) = state.theme.lock() {
        *guard = theme.clone();
    }

    #[cfg(target_os = "windows")]
    use windows::core::BOOL;
    #[cfg(target_os = "windows")]
    use windows::Win32::Foundation::HWND;
    #[cfg(target_os = "windows")]
    use windows::Win32::Graphics::Dwm::{
        DwmSetWindowAttribute, DWMWA_BORDER_COLOR, DWMWA_USE_IMMERSIVE_DARK_MODE,
        DWMWA_WINDOW_CORNER_PREFERENCE, DWMWCP_ROUND, DWM_WINDOW_CORNER_PREFERENCE,
    };

    #[cfg(target_os = "windows")]
    {
        let hwnd = window
            .hwnd()
            .map_err(|e| AppError::Internal(e.to_string()))?;
        let hwnd = HWND(hwnd.0 as _);

        let is_dark = match effective_color_mode.as_deref() {
            Some("light") => false,
            Some("dark") => true,
            _ => window.theme().unwrap_or(Theme::Dark) == Theme::Dark,
        };

        let dark_mode = BOOL::from(is_dark);
        unsafe {
            let _ = DwmSetWindowAttribute(
                hwnd,
                DWMWA_USE_IMMERSIVE_DARK_MODE,
                &dark_mode as *const _ as _,
                std::mem::size_of::<BOOL>() as u32,
            );
            // Toggle native DWM border visibility while preserving the window frame/corners.
            const DWMWA_COLOR_DEFAULT: u32 = 0xFFFFFFFF;
            const DWMWA_COLOR_NONE: u32 = 0xFFFFFFFE;
            let border_color: u32 = if show_border && theme != "liquid-glass" {
                DWMWA_COLOR_DEFAULT
            } else {
                DWMWA_COLOR_NONE
            };
            let _ = DwmSetWindowAttribute(
                hwnd,
                DWMWA_BORDER_COLOR,
                &border_color as *const _ as _,
                std::mem::size_of::<u32>() as u32,
            );
            // Keep rounded corners even when border/shadow are disabled.
            let corner_pref = DWM_WINDOW_CORNER_PREFERENCE(DWMWCP_ROUND.0);
            let _ = DwmSetWindowAttribute(
                hwnd,
                DWMWA_WINDOW_CORNER_PREFERENCE,
                &corner_pref as *const _ as _,
                std::mem::size_of::<DWM_WINDOW_CORNER_PREFERENCE>() as u32,
            );
        }

        let build = windows_version::OsVersion::current().build;
        let is_win11 = build >= 22000;
        let is_win10 = build >= 10240 && build < 22000;

        let opacity = surface_opacity.or_else(|| db_state.settings_repo.get("app.surface_opacity").ok().flatten().and_then(|value| value.parse().ok())).unwrap_or(50.0);
        // Undecorated shadows reserve a non-client inset in Tao. A glass
        // client surface cannot cover it; configure the frame before capture.
        let _ = window.set_shadow(show_border && is_win11 && !is_win10 && theme != "liquid-glass");
        crate::infrastructure::window_material::apply(&window, &theme, is_dark, opacity)
            .map_err(AppError::Internal)?;
    }

    #[cfg(not(target_os = "windows"))]
    {
        let is_dark = match effective_color_mode.as_deref() {
            Some("light") => false,
            Some("dark") => true,
            _ => window.theme().unwrap_or(Theme::Dark) == Theme::Dark,
        };

        let _ = window_vibrancy::clear_vibrancy(&window);
        if matches!(theme.as_str(), "mica" | "acrylic" | "liquid-glass") {
            let _ = window_vibrancy::apply_vibrancy(
                &window,
                window_vibrancy::NSVisualEffectMaterial::HudWindow,
                None,
                None,
            );
        }
    }

    let _ = window.emit("theme-changed", theme);
    Ok(())
}

#[tauri::command]
pub fn set_material_opacity(window: WebviewWindow, opacity: f64) -> AppResult<()> {
    #[cfg(target_os = "windows")]
    crate::infrastructure::window_material::set_opacity(&window, opacity).map_err(AppError::Internal)?;
    Ok(())
}

#[tauri::command]
pub fn set_glass_geometry(window: WebviewWindow, geometry: serde_json::Value) -> AppResult<u64> {
    #[cfg(target_os = "windows")]
    {
        let geometry = serde_json::from_value(geometry)
            .map_err(|error| AppError::Internal(format!("Invalid glass geometry: {error}")))?;
        crate::infrastructure::window_material::submit_geometry(&window, geometry)
            .map_err(AppError::Internal)
    }
    #[cfg(not(target_os = "windows"))]
    {
        let _ = (window, geometry);
        Ok(0)
    }
}

#[tauri::command]
pub fn get_native_glass_status(window: WebviewWindow) -> AppResult<serde_json::Value> {
    #[cfg(target_os = "windows")]
    {
        let status = crate::infrastructure::window_material::native_glass_status(&window)
            .map_err(AppError::Internal)?;
        serde_json::to_value(status).map_err(|error| AppError::Internal(error.to_string()))
    }
    #[cfg(not(target_os = "windows"))]
    {
        let _ = window;
        Ok(serde_json::json!({ "state": "inactive", "revision": 0, "geometryRevision": 0 }))
    }
}
