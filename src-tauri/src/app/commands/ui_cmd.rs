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
    backdrop_enabled: Option<bool>,
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
    let backdrop_enabled = backdrop_enabled.unwrap_or_else(|| {
        let opacity = db_state.settings_repo.get("app.surface_opacity").ok().flatten();
        saved_backdrop_enabled(opacity.as_deref())
    });

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
            let border_color: u32 = if show_border {
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

        match native_backdrop(&theme, backdrop_enabled, is_win11) {
            NativeBackdrop::Mica => {
                let _ = window_vibrancy::apply_mica(&window, Some(is_dark));
                let _ = window.set_shadow(show_border);
            }
            NativeBackdrop::Acrylic => {
                let _ = window_vibrancy::apply_acrylic(
                    &window,
                    Some(if is_dark {
                        (30, 30, 30, 40)
                    } else {
                        (240, 240, 240, 40)
                    }),
                );
                let _ = window.set_shadow(show_border);
            }
            NativeBackdrop::None => {
                // clear_vibrancy is macOS-only; explicitly clear both Windows
                // backdrops when choosing a solid theme or the clear endpoint.
                let _ = window_vibrancy::clear_acrylic(&window);
                let _ = window_vibrancy::clear_mica(&window);
                let _ = window.set_shadow(show_border && is_win11 && !is_win10);
            }
        }
    }

    #[cfg(not(target_os = "windows"))]
    {
        let is_dark = match effective_color_mode.as_deref() {
            Some("light") => false,
            Some("dark") => true,
            _ => window.theme().unwrap_or(Theme::Dark) == Theme::Dark,
        };

        let _ = window_vibrancy::clear_vibrancy(&window);
        if backdrop_enabled && matches!(theme.as_str(), "mica" | "acrylic" | "liquid-glass") {
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

fn saved_backdrop_enabled(opacity: Option<&str>) -> bool {
    let parse = |value: Option<&str>, fallback: f64| value
        .and_then(|s| s.parse::<f64>().ok()).filter(|v| v.is_finite()).unwrap_or(fallback);
    parse(opacity, 50.0) > 0.0
}

#[derive(Debug, PartialEq)]
enum NativeBackdrop { None, Mica, Acrylic }

fn native_backdrop(theme: &str, enabled: bool, is_win11: bool) -> NativeBackdrop {
    if !enabled || !is_win11 { return NativeBackdrop::None; }
    match theme {
        "mica" => NativeBackdrop::Mica,
        "acrylic" | "liquid-glass" => NativeBackdrop::Acrylic,
        _ => NativeBackdrop::None,
    }
}

#[cfg(test)]
mod material_tests {
    use super::*;

    #[test]
    fn clear_endpoint_survives_restart_and_really_disables_the_native_backdrop() {
        for theme in ["mica", "acrylic", "liquid-glass"] {
            let enabled = saved_backdrop_enabled(Some("0"));
            assert!(!enabled);
            assert_eq!(native_backdrop(theme, enabled, true), NativeBackdrop::None);
        }
        assert!(!saved_backdrop_enabled(Some("-1")));
        assert!(saved_backdrop_enabled(Some("50")));
    }

    #[test]
    fn switching_away_clears_glass_and_preserves_windows_10_fallback() {
        assert_eq!(native_backdrop("mica", true, true), NativeBackdrop::Mica);
        assert_eq!(native_backdrop("liquid-glass", true, true), NativeBackdrop::Acrylic);
        for theme in ["retro", "paper", "sticky-note", "sakura"] {
            assert_eq!(native_backdrop(theme, true, true), NativeBackdrop::None);
        }
        assert_eq!(native_backdrop("liquid-glass", true, false), NativeBackdrop::None);
        assert!(saved_backdrop_enabled(None));
        assert!(saved_backdrop_enabled(Some("NaN")));
    }
}
