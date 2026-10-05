//! Run with a local Vite server to inspect production DWM materials over a fixed
//! test background. Uses mock clipboard/settings and an isolated WebView profile.
#[cfg(target_os = "windows")]
#[path = "../src/infrastructure/window_material.rs"]
mod window_material;

#[cfg(target_os = "windows")]
#[path = "material_probe/scope_blocker.rs"]
mod scope_blocker;

#[cfg(target_os = "windows")]
mod probe {
use super::window_material;
use tauri::{Listener, Manager, WebviewUrl, WebviewWindow, WebviewWindowBuilder};
use serde_json::{json, Value};
use std::sync::{Arc, Mutex};
use std::path::PathBuf;
use std::time::{Duration, Instant, SystemTime};

const QA_WINDOW_TOP: f64 = 440.0;

fn qa_time_ms() -> u128 {
    std::time::SystemTime::now().duration_since(std::time::UNIX_EPOCH)
        .map(|duration| duration.as_millis()).unwrap_or_default()
}

#[derive(Default)]
struct AutorunState {
    input_ready: bool,
    cancel_reason: Option<String>,
    background_ack: Option<(u64, String)>,
    interaction_pending: Option<(u64, String)>,
    interaction_ack: Option<Value>,
    motion_pending: Option<(u64, String)>,
    motion_started: Option<Value>,
    motion_finished: Option<Value>,
    geometry_count: u64,
    geometry_receipt: u64,
    geometry_max_pressure: f64,
    geometry_track_count: usize,
}

impl AutorunState {
    fn accept_geometry(&mut self, receipt: u64, max_pressure: f64, track_count: usize) {
        self.geometry_count += 1;
        self.geometry_receipt = receipt;
        self.geometry_max_pressure = max_pressure;
        self.geometry_track_count = track_count;
    }

    fn geometry_submitted(&self, status: &window_material::NativeGlassStatus) -> bool {
        status.state == "ready" && self.geometry_receipt != 0
            && status.geometry_revision == self.geometry_receipt
    }

    fn pressure_settled(&self, status: &window_material::NativeGlassStatus, previous_count: u64, pressed: bool) -> bool {
        self.geometry_submitted(status) && self.geometry_count > previous_count && if pressed {
            self.geometry_max_pressure >= 0.99
        } else { self.geometry_max_pressure == 0.0 }
    }
}

#[cfg(test)]
mod geometry_tests {
    use super::*;

    fn status(state: &'static str, receipt: u64) -> window_material::NativeGlassStatus {
        window_material::NativeGlassStatus { state, revision: 100, geometry_revision: receipt }
    }

    #[test]
    fn accepted_geometry_waits_for_its_own_frame() {
        let mut state = AutorunState::default();
        state.accept_geometry(42, 0.0, 1);
        assert!(!state.geometry_submitted(&status("ready", 41)));
        assert!(!state.geometry_submitted(&status("ready", 1)));
        assert!(!state.geometry_submitted(&status("ready", 43)));
        assert!(state.geometry_submitted(&status("ready", 42)));
        state.accept_geometry(43, 0.0, 1);
        assert!(!state.geometry_submitted(&status("ready", 42)));
        assert!(state.geometry_submitted(&status("ready", 43)));
    }

    #[test]
    fn empty_geometry_is_not_a_confirmed_surface() {
        assert!(!AutorunState::default().geometry_submitted(&status("ready", 0)));
    }

    #[test]
    fn nonforeground_states_cannot_confirm_geometry() {
        let mut state = AutorunState::default();
        state.accept_geometry(42, 0.0, 1);
        for phase in ["inactive", "pending", "resizing", "suspended", "failed"] {
            assert!(!state.geometry_submitted(&status(phase, 42)), "{phase}");
        }
    }

    #[test]
    fn pressure_waits_for_the_matching_new_submission() {
        let mut state = AutorunState::default();
        state.accept_geometry(41, 0.0, 1);
        state.accept_geometry(42, 1.0, 1);
        assert!(!state.pressure_settled(&status("ready", 41), 1, true));
        assert!(state.pressure_settled(&status("ready", 42), 1, true));
        assert!(!state.pressure_settled(&status("ready", 42), 2, true));
        assert!(!state.pressure_settled(&status("resizing", 42), 1, true));
        state.accept_geometry(43, 0.0, 1);
        assert!(!state.pressure_settled(&status("ready", 42), 2, false));
        assert!(state.pressure_settled(&status("ready", 43), 2, false));
        assert!(!state.pressure_settled(&status("ready", 43), 2, true));
    }
}

struct Autorun {
    state: Mutex<AutorunState>,
    mode: &'static str,
    born: SystemTime,
    clock: Instant,
    start_file: PathBuf,
    stop_file: PathBuf,
}

impl Autorun {
    fn new(mode: &'static str) -> Self {
        let target = std::path::Path::new(env!("CARGO_MANIFEST_DIR")).join("target");
        Self {
            state: Mutex::new(AutorunState::default()), mode, born: SystemTime::now(), clock: Instant::now(),
            start_file: target.join("material-probe-autorun-start"),
            stop_file: target.join("material-probe-autorun-stop"),
        }
    }

    fn log(&self, event: &str, detail: Value) {
        println!("Material QA autorun {}", json!({
            "event": event, "timeMs": qa_time_ms(), "elapsedMs": self.clock.elapsed().as_millis(), "detail": detail
        }));
    }

    fn signal(&self, path: &std::path::Path) -> Result<bool, String> {
        match std::fs::symlink_metadata(path) {
            Ok(metadata) if metadata.is_file() => metadata.modified()
                .map(|modified| modified >= self.born).map_err(|error| error.to_string()),
            Ok(_) => Err(format!("QA handshake is not a regular file: {}", path.display())),
            Err(error) if error.kind() == std::io::ErrorKind::NotFound => Ok(false),
            Err(error) => Err(format!("QA handshake read failed: {error}")),
        }
    }

    fn cancel(&self, reason: impl Into<String>) {
        let mut state = self.state.lock().unwrap();
        if state.cancel_reason.is_none() { state.cancel_reason = Some(reason.into()); }
    }

    fn check_running(&self) -> Result<(), String> {
        if self.signal(&self.stop_file)? { return Err("recording_stop_requested".into()); }
        match self.state.lock().unwrap().cancel_reason.clone() {
            Some(reason) => Err(reason), None => Ok(()),
        }
    }

    fn snapshot(&self, main: &WebviewWindow) -> Result<Value, String> {
        let position = main.outer_position().map_err(|error| error.to_string())?;
        let inner = main.inner_size().map_err(|error| error.to_string())?;
        let outer = main.outer_size().map_err(|error| error.to_string())?;
        let status = window_material::native_glass_status(main)?;
        let state = self.state.lock().unwrap();
        let submission_confirmed = state.geometry_submitted(&status);
        Ok(json!({"positionPhysical": {"x": position.x, "y": position.y},
            "innerSizePhysical": {"width": inner.width, "height": inner.height},
            "outerSizePhysical": {"width": outer.width, "height": outer.height},
            "native": status, "acceptedGeometry": {
                "count": state.geometry_count, "receipt": state.geometry_receipt,
                "maxPressure": state.geometry_max_pressure, "trackCount": state.geometry_track_count,
                "frameSubmissionConfirmed": submission_confirmed
            }}))
    }

    fn wait_ready(&self, main: &WebviewWindow, need_input: bool) -> Result<(), String> {
        let deadline = Instant::now() + Duration::from_secs(20);
        loop {
            self.check_running()?;
            let status = window_material::native_glass_status(main)?;
            if status.state == "failed" { return Err("native_failed".into()); }
            let ready = {
                let state = self.state.lock().unwrap();
                (!need_input || state.input_ready) && state.geometry_submitted(&status)
            };
            if ready { return Ok(()); }
            if Instant::now() >= deadline { return Err("native_geometry_or_input_ready_timeout".into()); }
            std::thread::sleep(Duration::from_millis(100));
        }
    }

    fn hold(&self, duration: Duration) -> Result<(), String> {
        let deadline = Instant::now() + duration;
        while Instant::now() < deadline {
            self.check_running()?;
            std::thread::sleep(Duration::from_millis(50));
        }
        Ok(())
    }

    fn background(&self, backdrop: &WebviewWindow, mode: &str, generation: u64) -> Result<(), String> {
        self.check_running()?;
        self.state.lock().unwrap().background_ack = None;
        let detail = json!({"mode": mode, "generation": generation});
        backdrop.eval(&format!("window.dispatchEvent(new CustomEvent('tiez:qa-background', {{detail: {detail}}}));"))
            .map_err(|error| error.to_string())?;
        let deadline = Instant::now() + Duration::from_secs(10);
        loop {
            self.check_running()?;
            if self.state.lock().unwrap().background_ack.as_ref()
                .is_some_and(|ack| ack.0 == generation && ack.1 == mode) {
                self.log("background_applied", json!({"mode": mode, "generation": generation}));
                return Ok(());
            }
            if Instant::now() >= deadline { return Err(format!("background_ack_timeout:{mode}")); }
            std::thread::sleep(Duration::from_millis(50));
        }
    }

    fn step(&self, main: &WebviewWindow, index: u32, name: &str, position: Option<f64>, width: Option<f64>, hold_secs: u64) -> Result<(), String> {
        self.check_running()?;
        self.wait_ready(main, false)?;
        self.log("step_start", json!({"index": index, "name": name, "targetX": position, "targetWidth": width,
            "snapshot": self.snapshot(main)?}));
        if let Some(x) = position {
            main.set_position(tauri::LogicalPosition::new(x, QA_WINDOW_TOP)).map_err(|error| error.to_string())?;
        }
        if let Some(width) = width {
            main.set_size(tauri::LogicalSize::new(width, 589.0)).map_err(|error| error.to_string())?;
        }
        self.log("step_applied", json!({"index": index, "name": name, "snapshot": self.snapshot(main)?}));
        self.wait_ready(main, false)?;
        self.hold(Duration::from_secs(hold_secs))?;
        self.wait_ready(main, false)?;
        self.log("step_finished", json!({"index": index, "name": name, "snapshot": self.snapshot(main)?}));
        Ok(())
    }

    fn sequence(&self, main: &WebviewWindow, backdrop: &WebviewWindow) -> Result<(), String> {
        self.log("waiting_ready", json!({"candidate": "replace_scene", "startFile": self.start_file, "stopFile": self.stop_file}));
        self.wait_ready(main, true)?;
        self.background(backdrop, "gray", 1)?;
        self.log("armed", json!({"snapshot": self.snapshot(main)?, "background": "gray"}));
        let deadline = Instant::now() + Duration::from_secs(120);
        while !self.signal(&self.start_file)? {
            self.check_running()?;
            if Instant::now() >= deadline { return Err("recording_start_timeout".into()); }
            std::thread::sleep(Duration::from_millis(50));
        }
        self.check_running()?;
        self.log("recording_started", json!({"snapshot": self.snapshot(main)?}));
        self.step(main, 1, "gray_left_baseline", Some(450.0), None, 3)?;
        self.step(main, 2, "gray_right", Some(750.0), None, 3)?;
        self.step(main, 3, "gray_left", Some(450.0), None, 3)?;
        self.step(main, 4, "gray_narrow", None, Some(280.0), 2)?;
        self.step(main, 5, "gray_wide", None, Some(425.0), 2)?;
        self.background(backdrop, "pattern", 2)?;
        self.step(main, 6, "pattern_left_baseline", None, None, 3)?;
        self.step(main, 7, "pattern_right", Some(750.0), None, 3)?;
        self.step(main, 8, "pattern_left", Some(450.0), None, 3)?;
        Ok(())
    }

    fn interaction(&self, main: &WebviewWindow, index: u64, action: &str, hold_secs: u64) -> Result<(), String> {
        self.check_running()?;
        self.wait_ready(main, false)?;
        let previous_geometry = {
            let mut state = self.state.lock().unwrap();
            state.interaction_pending = Some((index, action.into()));
            state.interaction_ack = None;
            state.geometry_count
        };
        self.log("interaction_start", json!({"index": index, "action": action,
            "inputMethod": "synthetic DOM events", "snapshot": self.snapshot(main)?}));
        // The included script contains a closed set of fixture actions. No
        // page-exposed function accepts selectors, source text or OS commands.
        let command = json!({"index": index, "action": action});
        main.eval(&format!("(() => {{ const step = {command}; {} }})()",
            include_str!("material_probe_optics.js"))).map_err(|error| error.to_string())?;
        let deadline = Instant::now() + Duration::from_secs(15);
        let acknowledgement = loop {
            self.check_running()?;
            if let Some(ack) = self.state.lock().unwrap().interaction_ack.clone() { break ack; }
            if Instant::now() >= deadline { return Err(format!("interaction_ack_timeout:{action}")); }
            std::thread::sleep(Duration::from_millis(25));
        };
        if acknowledgement["ok"] != true {
            return Err(format!("interaction_dom_failed:{action}:{}", acknowledgement["error"]));
        }
        self.log("interaction_dom_applied", json!({"index": index, "action": action,
            "dom": acknowledgement, "snapshot": self.snapshot(main)?}));
        // Check the matching native frame submission as well as the pointer
        // parameters. Neither DOM acknowledgement nor IPC acceptance proves it.
        let pressure_target = if action.ends_with("_press") { Some(true) }
            else if action.ends_with("_release") { Some(false) } else { None };
        if let Some(pressed) = pressure_target {
            let deadline = Instant::now() + Duration::from_secs(3);
            loop {
                self.check_running()?;
                let status = window_material::native_glass_status(main)?;
                if status.state == "failed" { return Err("native_failed".into()); }
                let reached = self.state.lock().unwrap().pressure_settled(&status, previous_geometry, pressed);
                if reached { break; }
                if Instant::now() >= deadline { return Err(format!("interaction_pressure_timeout:{action}")); }
                std::thread::sleep(Duration::from_millis(25));
            }
            self.log("interaction_pressure_settled", json!({"index": index, "action": action,
                "pressed": pressed, "snapshot": self.snapshot(main)?}));
        }
        self.hold(Duration::from_secs(hold_secs))?;
        self.wait_ready(main, false)?;
        self.state.lock().unwrap().interaction_pending = None;
        self.log("interaction_finished", json!({"index": index, "action": action,
            "snapshot": self.snapshot(main)?}));
        Ok(())
    }

    fn optics_sequence(&self, main: &WebviewWindow, backdrop: &WebviewWindow) -> Result<(), String> {
        self.log("waiting_ready", json!({"candidate": "optics_interaction",
            "startFile": self.start_file, "stopFile": self.stop_file}));
        self.wait_ready(main, true)?;
        // Freeze only the fixture's moving line at a reproducible phase; an
        // animated background would obscure press/release image differences.
        backdrop.eval(r#"(() => {
            const style = document.createElement('style');
            style.id = 'qa-optics-fixed-background';
            style.textContent = '#scene::after { animation:none !important; transform:translateX(300px) !important; }';
            document.head.append(style);
        })()"#).map_err(|error| error.to_string())?;
        self.background(backdrop, "pattern", 1)?;
        self.interaction(main, 0, "dom_ready", 0)?;
        self.log("armed", json!({"snapshot": self.snapshot(main)?, "background": "pattern",
            "backgroundAnimation": "disabled in fixture; white line fixed at x=620 CSS px",
            "inputMethod": "synthetic DOM PointerEvent and HTMLElement.click; no OS input"}));
        let deadline = Instant::now() + Duration::from_secs(120);
        while !self.signal(&self.start_file)? {
            self.check_running()?;
            if Instant::now() >= deadline { return Err("recording_start_timeout".into()); }
            std::thread::sleep(Duration::from_millis(50));
        }
        self.check_running()?;
        self.log("recording_started", json!({"snapshot": self.snapshot(main)?}));
        self.hold(Duration::from_secs(1))?;
        for (index, (action, seconds)) in [
            ("header_press", 1), ("header_release", 1),
            ("search_press", 1), ("search_release", 1),
            ("chip_press", 1), ("chip_release", 1),
            ("settings_open", 1), ("general_open", 2),
            ("appearance_open", 2), ("appearance_ranges", 2),
            ("appearance_surface", 2), ("clipboard_open", 2),
            ("clipboard_shortcut", 2), ("data_open", 2),
            ("settings_back", 1),
        ].into_iter().enumerate() {
            self.interaction(main, index as u64 + 1, action, seconds)?;
        }
        Ok(())
    }

    fn motion_segment(&self, main: &WebviewWindow, backdrop: &WebviewWindow,
        generation: u64, scene: &str, client_bounds: &Value) -> Result<(), String> {
        self.check_running()?;
        self.wait_ready(main, false)?;
        {
            let mut state = self.state.lock().unwrap();
            state.motion_pending = Some((generation, scene.into()));
            state.motion_started = None;
            state.motion_finished = None;
        }
        self.log("optics_motion_start", json!({"generation": generation, "scene": scene,
            "inputMethod": "fixed Web Animations source translation in the fixture backdrop; no main-window input",
            "requestedDurationMs": 4500, "clientBoundsBackdropCss": client_bounds,
            "snapshot": self.snapshot(main)?}));
        // A closed fixture event carries only a known scene and the relative
        // bounds of these two test windows. It accepts no source or selectors.
        let detail = json!({"generation": generation, "scene": scene, "clientBounds": client_bounds});
        backdrop.eval(&format!("window.dispatchEvent(new CustomEvent('tiez:qa-optics-motion', {{detail: {detail}}}));"))
            .map_err(|error| error.to_string())?;
        let deadline = Instant::now() + Duration::from_secs(10);
        let mut logged_started = false;
        loop {
            self.check_running()?;
            let (started, finished) = {
                let state = self.state.lock().unwrap();
                (state.motion_started.clone(), state.motion_finished.clone())
            };
            if !logged_started {
                if let Some(acknowledgement) = started {
                    self.log("optics_motion_started_ack", json!({"generation": generation, "scene": scene,
                        "acknowledgement": acknowledgement, "snapshot": self.snapshot(main)?}));
                    logged_started = true;
                }
            }
            if let Some(acknowledgement) = finished {
                if acknowledgement["ok"] != true {
                    return Err(format!("optics_motion_dom_failed:{scene}:{}", acknowledgement["error"]));
                }
                if !logged_started { return Err(format!("optics_motion_missing_start_ack:{scene}")); }
                self.wait_ready(main, false)?;
                self.state.lock().unwrap().motion_pending = None;
                self.log("optics_motion_finished_ack", json!({"generation": generation, "scene": scene,
                    "acknowledgement": acknowledgement, "snapshot": self.snapshot(main)?, "visualPass": false}));
                return Ok(());
            }
            if Instant::now() >= deadline { return Err(format!("optics_motion_ack_timeout:{scene}")); }
            std::thread::sleep(Duration::from_millis(25));
        }
    }

    fn optics_motion_sequence(&self, main: &WebviewWindow, backdrop: &WebviewWindow) -> Result<(), String> {
        self.log("waiting_ready", json!({"candidate": "optics_motion",
            "startFile": self.start_file, "stopFile": self.stop_file}));
        self.wait_ready(main, true)?;
        self.background(backdrop, "gray", 1)?;
        let main_position = main.inner_position().map_err(|error| error.to_string())?;
        let backdrop_position = backdrop.inner_position().map_err(|error| error.to_string())?;
        let main_size = main.inner_size().map_err(|error| error.to_string())?;
        let main_scale = main.scale_factor().map_err(|error| error.to_string())?;
        let backdrop_scale = backdrop.scale_factor().map_err(|error| error.to_string())?;
        if !main_scale.is_finite() || main_scale <= 0.0 || !backdrop_scale.is_finite() || backdrop_scale <= 0.0 {
            return Err("optics_motion_invalid_scale".into());
        }
        if (f64::from(main_size.width) / main_scale - 425.0).abs() > 1.0
            || (f64::from(main_size.height) / main_scale - 589.0).abs() > 1.0 {
            return Err("optics_motion_unexpected_window_size".into());
        }
        let client_bounds = json!({
            "x": (f64::from(main_position.x) - f64::from(backdrop_position.x)) / backdrop_scale,
            "y": (f64::from(main_position.y) - f64::from(backdrop_position.y)) / backdrop_scale,
            "width": f64::from(main_size.width) / backdrop_scale,
            "height": f64::from(main_size.height) / backdrop_scale,
        });
        self.log("armed", json!({"snapshot": self.snapshot(main)?, "background": "gray",
            "clientBoundsBackdropCss": client_bounds, "mainScaleFactor": main_scale,
            "backdropScaleFactor": backdrop_scale, "sourceScenes": ["text_y", "edge_x", "diagonal_xy"],
            "sourceColors": "neutral grayscale only", "nominalSequenceDurationMs": 13500,
            "inputMethod": "fixed backdrop animation; no pointer events, presses or setting changes"}));
        let deadline = Instant::now() + Duration::from_secs(120);
        while !self.signal(&self.start_file)? {
            self.check_running()?;
            if Instant::now() >= deadline { return Err("recording_start_timeout".into()); }
            std::thread::sleep(Duration::from_millis(50));
        }
        self.check_running()?;
        self.log("recording_started", json!({"snapshot": self.snapshot(main)?}));
        for (index, scene) in ["text_y", "edge_x", "diagonal_xy"].into_iter().enumerate() {
            self.motion_segment(main, backdrop, index as u64 + 1, scene, &client_bounds)?;
        }
        self.background(backdrop, "pattern", 2)?;
        self.log("optics_motion_restored", json!({"background": "pattern", "snapshot": self.snapshot(main)?,
            "visualPass": false}));
        Ok(())
    }

    fn fixture_hwnd(&self, window: &WebviewWindow) -> Result<windows::Win32::Foundation::HWND, String> {
        if !matches!(window.label(), "main" | "backdrop") {
            return Err("foreground_check_requires_fixture_window".into());
        }
        let hwnd = window.hwnd().map_err(|error| error.to_string())?;
        let mut process = 0;
        let thread = unsafe {
            windows::Win32::UI::WindowsAndMessaging::GetWindowThreadProcessId(hwnd, Some(&mut process))
        };
        if thread == 0 || process != unsafe { windows::Win32::System::Threading::GetCurrentProcessId() } {
            return Err(format!("fixture_window_owner_mismatch:{}", window.label()));
        }
        Ok(hwnd)
    }

    fn stress_snapshot(&self, main: &WebviewWindow, backdrop: &WebviewWindow) -> Result<Value, String> {
        let main_hwnd = self.fixture_hwnd(main)?;
        let backdrop_hwnd = self.fixture_hwnd(backdrop)?;
        let main_snapshot = self.snapshot(main)?;
        let main_tauri_focused = main.is_focused().map_err(|error| error.to_string())?;
        let backdrop_tauri_focused = backdrop.is_focused().map_err(|error| error.to_string())?;
        let foreground = unsafe { windows::Win32::UI::WindowsAndMessaging::GetForegroundWindow() };
        Ok(json!({"main": main_snapshot,
            "mainTauriIsFocusedDiagnostic": main_tauri_focused,
            "backdropTauriIsFocusedDiagnostic": backdrop_tauri_focused,
            "mainForeground": foreground == main_hwnd,
            "backdropForeground": foreground == backdrop_hwnd}))
    }

    fn wait_fixture_foreground(&self, window: &WebviewWindow) -> Result<(), String> {
        let deadline = Instant::now() + Duration::from_secs(3);
        loop {
            self.check_running()?;
            let hwnd = self.fixture_hwnd(window)?;
            // Tao's cached top-level keyboard-focus flag can be false while
            // WebView2 owns child focus. Require this exact owned top-level
            // window to be foreground, not that diagnostic flag.
            if unsafe { windows::Win32::UI::WindowsAndMessaging::GetForegroundWindow() } == hwnd { return Ok(()); }
            if Instant::now() >= deadline { return Err(format!("fixture_foreground_timeout:{}", window.label())); }
            std::thread::sleep(Duration::from_millis(25));
        }
    }

    fn resize_stress_sequence(&self, main: &WebviewWindow, backdrop: &WebviewWindow) -> Result<(), String> {
        self.log("waiting_ready", json!({"candidate": "resize_stress",
            "startFile": self.start_file, "stopFile": self.stop_file}));
        self.wait_ready(main, true)?;
        main.set_position(tauri::LogicalPosition::new(450.0, QA_WINDOW_TOP)).map_err(|error| error.to_string())?;
        main.set_size(tauri::LogicalSize::new(425.0, 589.0)).map_err(|error| error.to_string())?;
        main.set_focus().map_err(|error| error.to_string())?;
        self.background(backdrop, "pattern", 1)?;
        self.wait_ready(main, false)?;
        self.log("armed", json!({"snapshot": self.stress_snapshot(main, backdrop)?,
            "background": "pattern", "resizeStepsPerDirection": 10, "stepIntervalMs": 50,
            "expected": "Inspect control shadows throughout resize and after focus return; verify fresh background without white flashes or stale optics"}));
        let deadline = Instant::now() + Duration::from_secs(120);
        while !self.signal(&self.start_file)? {
            self.check_running()?;
            if Instant::now() >= deadline { return Err("recording_start_timeout".into()); }
            std::thread::sleep(Duration::from_millis(50));
        }
        self.check_running()?;
        self.log("recording_started", json!({"snapshot": self.stress_snapshot(main, backdrop)?}));
        // Windows may deny the launch-time foreground request. Allow the
        // recorder operator to activate this fixture after armed, then require
        // actual focus before any recorded resize or focus test action.
        self.wait_fixture_foreground(main)?;
        self.hold(Duration::from_millis(500))?;

        for round in 1..=2 {
            for (direction, from, to) in [("narrow", 425.0, 280.0), ("wide", 280.0, 425.0)] {
                self.wait_ready(main, false)?;
                self.log("resize_stress_phase_start", json!({"phase": "resize", "round": round,
                    "direction": direction, "fromLogicalWidth": from, "toLogicalWidth": to,
                    "expected": "Control outer shadows persist; resizing background preserves current screen coordinates; final optics match the new viewport",
                    "snapshot": self.stress_snapshot(main, backdrop)?}));
                // Deliberately do not wait for native ready between these
                // requests: this exercises overlapping resize transitions.
                for step in 1..=10 {
                    self.check_running()?;
                    let width = from + (to - from) * f64::from(step) / 10.0;
                    main.set_size(tauri::LogicalSize::new(width, 589.0)).map_err(|error| error.to_string())?;
                    self.hold(Duration::from_millis(50))?;
                    self.log("resize_stress_step", json!({"round": round, "direction": direction,
                        "step": step, "requestedLogicalSize": {"width": width, "height": 589.0},
                        "snapshot": self.stress_snapshot(main, backdrop)?}));
                }
                self.wait_ready(main, false)?;
                self.hold(Duration::from_millis(200))?;
                self.log("resize_stress_phase_finished", json!({"phase": "resize", "round": round,
                    "direction": direction, "snapshot": self.stress_snapshot(main, backdrop)?}));
            }
        }

        // Both retained handles belong to this isolated fixture. Focusing its
        // fullscreen backdrop can cover the main window temporarily; only the
        // return-to-main footage should be judged for control shadow recovery.
        for round in 1..=2 {
            self.check_running()?;
            self.log("resize_stress_phase_start", json!({"phase": "focus", "round": round,
                "expected": "Only fixture windows gain focus; main controls retain their outer shadows when main is visible again",
                "snapshot": self.stress_snapshot(main, backdrop)?}));
            backdrop.set_focus().map_err(|error| error.to_string())?;
            self.wait_fixture_foreground(backdrop)?;
            self.log("resize_stress_focus_applied", json!({"round": round, "target": "backdrop",
                "snapshot": self.stress_snapshot(main, backdrop)?}));
            self.hold(Duration::from_millis(350))?;
            main.set_focus().map_err(|error| error.to_string())?;
            self.wait_fixture_foreground(main)?;
            self.log("resize_stress_focus_applied", json!({"round": round, "target": "main",
                "snapshot": self.stress_snapshot(main, backdrop)?}));
            self.wait_ready(main, false)?;
            self.hold(Duration::from_millis(500))?;
            self.log("resize_stress_phase_finished", json!({"phase": "focus", "round": round,
                "snapshot": self.stress_snapshot(main, backdrop)?}));
        }

        for (mode, generation) in [("gray", 2), ("pattern", 3)] {
            self.log("resize_stress_phase_start", json!({"phase": "background", "target": mode,
                "expected": "Native background follows the acknowledged fixture scene; control shadows remain visible",
                "snapshot": self.stress_snapshot(main, backdrop)?}));
            self.background(backdrop, mode, generation)?;
            self.wait_ready(main, false)?;
            self.hold(Duration::from_secs(2))?;
            self.wait_ready(main, false)?;
            self.log("resize_stress_phase_finished", json!({"phase": "background", "target": mode,
                "snapshot": self.stress_snapshot(main, backdrop)?}));
        }
        self.check_running()?;
        main.set_position(tauri::LogicalPosition::new(450.0, QA_WINDOW_TOP)).map_err(|error| error.to_string())?;
        main.set_size(tauri::LogicalSize::new(425.0, 589.0)).map_err(|error| error.to_string())?;
        main.set_focus().map_err(|error| error.to_string())?;
        self.wait_fixture_foreground(main)?;
        self.wait_ready(main, false)?;
        self.hold(Duration::from_millis(500))?;
        self.log("resize_stress_restored", json!({"expected": "425x589 logical client at the initial position; main focused and native ready",
            "snapshot": self.stress_snapshot(main, backdrop)?}));
        Ok(())
    }

    fn observe_recovery(&self, main: &WebviewWindow, stage: &str, duration: Duration) -> Result<bool, String> {
        let deadline = Instant::now() + duration;
        let mut ready = true;
        loop {
            self.check_running()?;
            let snapshot = self.snapshot(main)?;
            ready &= snapshot["acceptedGeometry"]["frameSubmissionConfirmed"] == true;
            self.log("scope_recovery_observed", json!({"stage": stage, "snapshot": snapshot}));
            if Instant::now() >= deadline { return Ok(ready); }
            self.hold(Duration::from_millis(250))?;
        }
    }

    fn scope_recovery_sequence(&self, main: &WebviewWindow, backdrop: &WebviewWindow) -> Result<(), String> {
        use std::io::BufRead;
        use std::os::windows::process::CommandExt;
        use std::process::{Command, Stdio};
        self.log("waiting_ready", json!({"candidate": "scope_recovery", "startFile": self.start_file, "stopFile": self.stop_file}));
        self.wait_ready(main, true)?;
        backdrop.eval(r#"(() => {
            const style = document.createElement('style');
            style.id = 'qa-recovery-fixed-background';
            style.textContent = '#scene::after { animation:none !important; transform:translateX(300px) !important; }';
            document.head.append(style);
        })()"#).map_err(|error| error.to_string())?;
        self.background(backdrop, "pattern", 1)?;
        self.wait_ready(main, false)?;
        self.log("armed", json!({"snapshot": self.snapshot(main)?, "background": "pattern",
            "backgroundAnimation": "disabled; white line fixed at x=620 CSS px"}));
        let deadline = Instant::now() + Duration::from_secs(120);
        while !self.signal(&self.start_file)? {
            self.check_running()?;
            if Instant::now() >= deadline { return Err("recording_start_timeout".into()); }
            self.hold(Duration::from_millis(50))?;
        }
        self.log("recording_started", json!({"snapshot": self.snapshot(main)?}));
        if !self.observe_recovery(main, "initial_static", Duration::from_secs(4))? {
            return Err("static_source_lost_ready".into());
        }
        for round in 1..=3 {
            let position = main.outer_position().map_err(|error| error.to_string())?;
            let mut child = Command::new(std::env::current_exe().map_err(|error| error.to_string())?)
                .args(["--scope-blocker".into(), (position.x + 120).to_string(), (position.y + 75).to_string(),
                    "40".into(), "40".into(), if round == 3 { "2800".into() } else { "1400".into() }])
                .creation_flags(0x08000000).stdout(Stdio::piped()).stderr(Stdio::inherit())
                .spawn().map_err(|error| error.to_string())?;
            let output = child.stdout.take().ok_or("Blocker stdout missing")?;
            let (sender, receiver) = std::sync::mpsc::sync_channel(1);
            std::thread::spawn(move || {
                let mut line = String::new();
                let result = std::io::BufReader::new(output).read_line(&mut line).map(|_| line);
                let _ = sender.send(result);
            });
            let acknowledgement = receiver.recv_timeout(Duration::from_secs(3));
            if !matches!(&acknowledgement, Ok(Ok(line)) if line.starts_with("blocker.ready ")) {
                let _ = child.kill(); let _ = child.wait();
                return Err(format!("Blocker did not become visible: {acknowledgement:?}"));
            }
            self.log("scope_blocker_visible", json!({"round": round, "childPid": child.id(),
                "acknowledgement": acknowledgement.unwrap().unwrap(), "snapshot": self.snapshot(main)?}));
            if round == 3 {
                main.minimize().map_err(|error| error.to_string())?;
                self.log("scope_recovery_minimized", json!({"snapshot": self.snapshot(main)?}));
                self.hold(Duration::from_millis(600))?;
                main.unminimize().map_err(|error| error.to_string())?;
                main.set_focus().map_err(|error| error.to_string())?;
                self.log("scope_recovery_restored_while_blocked", json!({"snapshot": self.snapshot(main)?}));
            }
            let deadline = Instant::now() + Duration::from_secs(5);
            while child.try_wait().map_err(|error| error.to_string())?.is_none() {
                if let Err(error) = self.check_running() { let _ = child.kill(); let _ = child.wait(); return Err(error); }
                if Instant::now() >= deadline { let _ = child.kill(); let _ = child.wait(); return Err("Blocker lifetime exceeded".into()); }
                self.log("scope_blocker_present", json!({"round": round, "snapshot": self.snapshot(main)?}));
                self.hold(Duration::from_millis(200))?;
            }
            self.log("scope_blocker_closed", json!({"round": round, "snapshot": self.snapshot(main)?}));
            self.observe_recovery(main, "no_input_after_blocker", Duration::from_secs(3))?;
            let automatically_recovered = window_material::native_glass_status(main)?.state == "ready";
            // Record ordinary movement/resize even on the broken baseline, to
            // distinguish a persistent failure from a brief colour transition.
            main.set_position(tauri::LogicalPosition::new(570.0, QA_WINDOW_TOP)).map_err(|error| error.to_string())?;
            self.observe_recovery(main, "after_move", Duration::from_secs(1))?;
            main.set_size(tauri::LogicalSize::new(280.0, 589.0)).map_err(|error| error.to_string())?;
            self.observe_recovery(main, "after_resize", Duration::from_secs(1))?;
            main.set_size(tauri::LogicalSize::new(425.0, 589.0)).map_err(|error| error.to_string())?;
            main.set_position(tauri::LogicalPosition::new(450.0, QA_WINDOW_TOP)).map_err(|error| error.to_string())?;
            main.set_focus().map_err(|error| error.to_string())?;
            self.observe_recovery(main, "after_restore", Duration::from_secs(2))?;
            if !automatically_recovered { return Err("scope_did_not_recover_without_input".into()); }
            self.wait_ready(main, false)?;
        }
        if !self.observe_recovery(main, "final_static", Duration::from_secs(5))? { return Err("static_source_lost_ready".into()); }
        Ok(())
    }

    fn run(self: Arc<Self>, main: WebviewWindow, backdrop: WebviewWindow) {
        let result = if self.mode == "optics_interaction" {
            self.optics_sequence(&main, &backdrop)
        } else if self.mode == "optics_motion" {
            self.optics_motion_sequence(&main, &backdrop)
        } else if self.mode == "resize_stress" {
            self.resize_stress_sequence(&main, &backdrop)
        } else if self.mode == "scope_recovery" {
            self.scope_recovery_sequence(&main, &backdrop)
        } else { self.sequence(&main, &backdrop) };
        let exit_code = if let Err(reason) = result {
            self.cancel(reason.clone());
            self.log("aborted", json!({"reason": reason, "snapshot": self.snapshot(&main).ok()}));
            1
        } else {
            self.log("sequence_finished", json!({"visualPass": false, "snapshot": self.snapshot(&main).ok()}));
            0
        };
        // Keep both fixture windows alive until the recorder acknowledges stop.
        // This also applies to scope failures, timeouts and user close requests.
        let mut reported_signal_error = false;
        loop {
            match self.signal(&self.stop_file) {
                Ok(true) => break,
                Err(error) if !reported_signal_error => {
                    self.log("stop_signal_error", json!({"error": error}));
                    reported_signal_error = true;
                }
                _ => {}
            }
            std::thread::sleep(Duration::from_millis(100));
        }
        self.log("recording_stopped", json!({"exitCode": exit_code}));
        main.app_handle().exit(exit_code);
    }
}

fn set_theme(window: WebviewWindow, theme: String, color_mode: String, opacity: f64) -> Result<(), String> {
    let dark = color_mode == "dark";
    window.set_theme(Some(if dark { tauri::Theme::Dark } else { tauri::Theme::Light })).map_err(|e| e.to_string())?;
    window_material::apply(&window, &theme, dark, opacity).inspect_err(|error| { let _ = std::fs::write(std::path::Path::new(env!("CARGO_MANIFEST_DIR")).join("target/material-probe-error.txt"), error); })?;
    window.set_title(&format!("TieZ Material QA | {theme} | {color_mode}")).map_err(|e| e.to_string())?;
    println!("Applied {theme} {color_mode}");
    Ok(())
}

pub fn run() {
    // The sequence drives only this fixture's own windows.
    let autorun = match std::env::var("TIEZ_MATERIAL_QA_AUTORUN").ok().as_deref() {
        Some("replace_scene") => Some(Arc::new(Autorun::new("replace_scene"))),
        Some("optics_interaction") => Some(Arc::new(Autorun::new("optics_interaction"))),
        Some("optics_motion") => Some(Arc::new(Autorun::new("optics_motion"))),
        Some("resize_stress") => Some(Arc::new(Autorun::new("resize_stress"))),
        Some("scope_recovery") => Some(Arc::new(Autorun::new("scope_recovery"))),
        _ => None,
    };
    let invoke_autorun = autorun.clone();
    let setup_autorun = autorun.clone();
    let mut context = tauri::generate_context!();
    context.config_mut().app.windows.clear();
    // A test-only port override avoids Windows-reserved development ports.
    let port = std::env::var("TIEZ_MATERIAL_QA_PORT").ok()
        .and_then(|value| value.parse::<u16>().ok()).filter(|port| *port > 0).unwrap_or(1420);
    let base_url = format!("http://127.0.0.1:{port}");
    context.config_mut().build.dev_url = Some(base_url.parse().expect("Loopback QA URL"));
    let initial_theme = std::env::var("TIEZ_MATERIAL_QA_THEME").ok()
        .filter(|value| matches!(value.as_str(), "liquid-glass" | "mica" | "acrylic"))
        .unwrap_or_else(|| "liquid-glass".into());
    let initial_mode = std::env::var("TIEZ_MATERIAL_QA_MODE").ok()
        .filter(|value| matches!(value.as_str(), "light" | "dark"))
        .unwrap_or_else(|| "light".into());
    let initial_opacity = std::env::var("TIEZ_MATERIAL_QA_OPACITY").ok()
        .and_then(|value| value.parse::<f64>().ok()).filter(|value| value.is_finite())
        .unwrap_or(0.0).clamp(0.0, 100.0);
    let settings = Arc::new(Mutex::new(json!({
        "app.theme": initial_theme, "app.color_mode": initial_mode,
        "app.surface_opacity": initial_opacity.to_string(), "app.language": "zh",
        "app.show_search_box": "true", "app.tag_manager_enabled": "true",
        "app.window_pinned": "true", "app.show_app_border": "true"
    })));
    let samples: Vec<Value> = [
        "材质验收示例\n检查标题、正文和时间在复杂背景上的可读性。",
        "npm run build",
        "待办\n检查曲面边缘\n检查移动与缩放\n检查普通截图",
        "https://example.com/design",
        "const greeting = 'Hello, world!';",
    ].iter().enumerate().map(|(index, content)| json!({
        "id": index + 1, "content_type": "text", "content": content,
        "preview": content, "source_app": "Material fixture",
        "timestamp": 1789430400000_i64 - index as i64 * 60000,
        "is_pinned": false, "tags": ["测试"], "use_count": index + 1
    })).collect();
    let application = tauri::Builder::default()
        .invoke_handler(move |invoke: tauri::ipc::Invoke<tauri::Wry>| {
            // The backdrop can finish an in-flight status request after the
            // main window closes. Reject it without unwinding through WebView2.
            let needs_main = matches!(invoke.message.command(),
                "set_glass_geometry" | "get_native_glass_status" | "set_theme" | "set_material_opacity")
                || invoke.message.command().starts_with("qa_");
            let qa_main = invoke.message.webview().app_handle().get_webview_window("main");
            if needs_main && qa_main.is_none() {
                invoke.resolver.reject("The material QA window has closed");
                return true;
            }
            if invoke.message.command() == "set_glass_geometry" {
                let result = if let tauri::ipc::InvokeBody::Json(args) = invoke.message.payload() {
                    let window = qa_main.expect("QA window checked above");
                    let result = serde_json::from_value(args["geometry"].clone())
                        .map_err(|error| error.to_string())
                        .and_then(|geometry| window_material::submit_geometry(&window, geometry));
                    if let (Ok(receipt), true, Some(control)) =
                        (&result, invoke.message.webview().label() == "main", &invoke_autorun) {
                        let max_pressure = args["geometry"]["controls"].as_array().map(|controls| {
                            controls.iter().filter_map(|item| item["pressure"].as_f64()).fold(0.0_f64, f64::max)
                        }).unwrap_or_default();
                        let track_count = args["geometry"]["controls"].as_array().map(|controls| {
                            controls.iter().filter(|item| item["track"]["width"].as_f64().unwrap_or_default() > 0.0).count()
                        }).unwrap_or_default();
                        control.state.lock().unwrap().accept_geometry(*receipt, max_pressure, track_count);
                    }
                    result
                } else { Err("Expected glass geometry".into()) };
                match result { Ok(revision) => invoke.resolver.resolve(revision), Err(error) => invoke.resolver.reject(error) }
                return true;
            }
            if invoke.message.command() == "get_native_glass_status" {
                let window = qa_main.expect("QA window checked above");
                match window_material::native_glass_status(&window) {
                    Ok(status) => invoke.resolver.resolve(status),
                    Err(error) => invoke.resolver.reject(error),
                }
                return true;
            }
            if invoke.message.command() == "qa_background_ready" {
                let acknowledgement = if invoke.message.webview().label() == "backdrop" {
                    if let tauri::ipc::InvokeBody::Json(args) = invoke.message.payload() {
                        args["generation"].as_u64().zip(args["mode"].as_str())
                            .filter(|(_, mode)| matches!(*mode, "gray" | "pattern"))
                    } else { None }
                } else { None };
                if let (Some(control), Some((generation, mode))) = (&invoke_autorun, acknowledgement) {
                    control.state.lock().unwrap().background_ack = Some((generation, mode.into()));
                    invoke.resolver.resolve(());
                } else {
                    invoke.resolver.reject("Expected an autorun background acknowledgement from this fixture's backdrop");
                }
                return true;
            }
            if invoke.message.command() == "qa_optics_interaction_ack" {
                let accepted = if invoke.message.webview().label() == "main" {
                    if let (Some(control), tauri::ipc::InvokeBody::Json(args)) = (&invoke_autorun, invoke.message.payload()) {
                        let mut state = control.state.lock().unwrap();
                        let expected = state.interaction_pending.as_ref().is_some_and(|(index, action)|
                            args["index"].as_u64() == Some(*index) && args["action"].as_str() == Some(action.as_str()));
                        if control.mode == "optics_interaction" && expected && args["ok"].is_boolean() {
                            state.interaction_ack = Some(args.clone());
                            true
                        } else { false }
                    } else { false }
                } else { false };
                if accepted { invoke.resolver.resolve(()); }
                else { invoke.resolver.reject("Expected the pending optics acknowledgement from this fixture's main window"); }
                return true;
            }
            if invoke.message.command() == "qa_optics_motion_ack" {
                let accepted = if invoke.message.webview().label() == "backdrop" {
                    if let (Some(control), tauri::ipc::InvokeBody::Json(args)) = (&invoke_autorun, invoke.message.payload()) {
                        let mut state = control.state.lock().unwrap();
                        let expected = state.motion_pending.as_ref().is_some_and(|(generation, scene)|
                            args["generation"].as_u64() == Some(*generation) && args["scene"].as_str() == Some(scene.as_str()));
                        if control.mode == "optics_motion" && expected && args["ok"].is_boolean() {
                            match args["phase"].as_str() {
                                Some("started") if args["ok"] == true && state.motion_started.is_none() => {
                                    state.motion_started = Some(args.clone()); true
                                },
                                Some("finished") if state.motion_finished.is_none() => {
                                    state.motion_finished = Some(args.clone()); true
                                },
                                _ => false,
                            }
                        } else { false }
                    } else { false }
                } else { false };
                if accepted { invoke.resolver.resolve(()); }
                else { invoke.resolver.reject("Expected the pending motion acknowledgement from this fixture's backdrop"); }
                return true;
            }
            if invoke.message.command().starts_with("qa_") {
                println!("Material QA command={} time_ms={}", invoke.message.command(), qa_time_ms());
                let window = qa_main.expect("QA window checked above");
                let result = match invoke.message.command() {
                    "qa_input_ready" => {
                        if invoke.message.webview().label() == "main" {
                            if let Some(control) = &invoke_autorun { control.state.lock().unwrap().input_ready = true; }
                        }
                        Ok(())
                    },
                    "qa_toggle_window" => window.is_visible().and_then(|visible| if visible { window.hide() } else { window.show().and_then(|()| window.set_focus()) }),
                    "qa_refocus_window" => window.set_focus(),
                    "qa_minimize_window" => window.minimize(),
                    "qa_restore_window" => window.unminimize().and_then(|()| window.show()).and_then(|()| window.set_focus()),
                    "qa_resize_window" => {
                        let narrow = if let tauri::ipc::InvokeBody::Json(args) = invoke.message.payload() { args["narrow"].as_bool().unwrap_or(false) } else { false };
                        window.set_size(tauri::LogicalSize::new(if narrow { 280.0 } else { 425.0 }, 589.0)).and_then(|()| window.set_focus())
                    }
                    "qa_move_window" => {
                        let right = if let tauri::ipc::InvokeBody::Json(args) = invoke.message.payload() { args["right"].as_bool().unwrap_or(false) } else { false };
                        window.set_position(tauri::LogicalPosition::new(if right { 750.0 } else { 450.0 }, QA_WINDOW_TOP)).and_then(|()| window.set_focus())
                    }
                    "qa_clear_window_region" => {
                        let target = window.clone();
                        window.run_on_main_thread(move || {
                            // Fault injection is compiled only into this fixture
                            // and can address only its own retained main window.
                            if let Ok(hwnd) = target.hwnd() {
                                let mut process = 0;
                                unsafe {
                                    windows::Win32::UI::WindowsAndMessaging::GetWindowThreadProcessId(hwnd, Some(&mut process));
                                    if process == windows::Win32::System::Threading::GetCurrentProcessId() {
                                        let result = windows::Win32::Graphics::Gdi::SetWindowRgn(hwnd, None, true);
                                        println!("Material QA clear_main_region result={result}");
                                        let _ = target.set_focus();
                                    }
                                }
                            }
                        })
                    }
                    "qa_exit" => {
                        if let Some(control) = &invoke_autorun { control.cancel("qa_exit_requested"); }
                        else { window.app_handle().exit(0); }
                        Ok(())
                    }
                    _ => { invoke.resolver.reject("Unknown QA command"); return true; }
                };
                match result {
                    Ok(()) => invoke.resolver.resolve(()),
                    Err(error) => {
                        if let Some(control) = &invoke_autorun { control.cancel(format!("qa_command_failed:{error}")); }
                        invoke.resolver.reject(error.to_string());
                    }
                }
                return true;
            }
            if invoke.message.command() == "set_theme" {
                let result = if let tauri::ipc::InvokeBody::Json(args) = invoke.message.payload() {
                    let window = qa_main.expect("QA window checked above");
                    set_theme(window, args["theme"].as_str().unwrap_or("mica").into(), args["colorMode"].as_str().unwrap_or("light").into(), args["surfaceOpacity"].as_f64().unwrap_or(50.0))
                } else { Err("Expected material settings".into()) };
                match result { Ok(()) => invoke.resolver.resolve(()), Err(error) => invoke.resolver.reject(error) }
                return true;
            }
            if invoke.message.command() == "set_material_opacity" {
                let result = if let tauri::ipc::InvokeBody::Json(args) = invoke.message.payload() {
                    let window = qa_main.expect("QA window checked above");
                    window_material::set_opacity(&window, args["opacity"].as_f64().unwrap_or(50.0))
                } else { Err("Expected opacity".into()) };
                match result { Ok(()) => invoke.resolver.resolve(()), Err(error) => invoke.resolver.reject(error) }
                return true;
            }
            // Run the production frontend with an in-memory clipboard backend.
            // No application database, clipboard watcher or startup hooks run.
            let response = match invoke.message.command() {
                "get_settings" => settings.lock().unwrap().clone(),
                "save_setting" => {
                    if let tauri::ipc::InvokeBody::Json(args) = invoke.message.payload() {
                        if let (Some(key), Some(value)) = (args["key"].as_str(), args["value"].as_str()) {
                            settings.lock().unwrap()[key] = json!(value);
                        }
                    }
                    Value::Null
                }
                "get_platform_info" => json!({ "platform": "windows", "is_windows_10": false, "is_windows_11": true }),
                "get_clipboard_history" | "search_clipboard_history" | "get_tag_items" => json!(samples),
                "get_all_tags" => json!(["测试"]),
                "get_all_tags_info" => json!({"测试": samples.len()}),
                "get_tag_colors" => json!({}),
                "scan_installed_apps" => json!([]),
                "get_paste_queue" => json!({ "items": [], "current_index": 0 }),
                "get_clipboard_count" => json!(samples.len()),
                "is_autostart_enabled" => json!(false),
                "get_data_path" => json!("isolated-material-test"),
                _ => Value::Null,
            };
            invoke.resolver.resolve(response);
            true
        })
        .setup(move |app| {
            let profile = std::path::Path::new(env!("CARGO_MANIFEST_DIR"))
                .join("target/material-probe-profile");
            let evidence = std::path::Path::new(env!("CARGO_MANIFEST_DIR"))
                .join("target/material-probe-events.jsonl");
            std::fs::write(&evidence, "")?;
            let status_autorun = setup_autorun.clone();
            app.listen_any("native-glass-status", move |event| {
                use std::io::Write;
                let payload = serde_json::from_str::<Value>(event.payload()).unwrap_or(Value::Null);
                if payload["state"] == "failed" {
                    if let Some(control) = &status_autorun { control.cancel("native_failed"); }
                }
                if let Ok(mut file) = std::fs::OpenOptions::new().append(true).open(&evidence) {
                    let elapsed = std::time::SystemTime::now().duration_since(std::time::UNIX_EPOCH)
                        .map(|duration| duration.as_millis()).unwrap_or_default();
                    let _ = writeln!(file, "{}", json!({"timeMs": elapsed, "event": "native-glass-status", "payload": payload}));
                }
            });
            let backdrop = WebviewWindowBuilder::new(app, "backdrop", WebviewUrl::External(format!("{base_url}/tests/ui/native-backdrop.html").parse()?))
                .title("TieZ 材质测试背景").always_on_top(true).data_directory(profile.clone())
                .decorations(false).fullscreen(true).build()?;
            let main = WebviewWindowBuilder::new(app, "main", WebviewUrl::External(base_url.parse()?))
                .title("TieZ Material QA").always_on_top(true).data_directory(profile)
                .transparent(true).decorations(false).shadow(true)
                // Fixture-only input keeps the main window foreground during
                // movement QA. Clicking the full-screen backdrop would cover
                // it and confound renderer flicker with test-panel activation.
                .initialization_script(r#"
                    window.addEventListener('DOMContentLoaded', () => {
                        const bar = document.createElement('nav');
                        bar.setAttribute('aria-label', '材质测试操作');
                        bar.style.cssText = 'position:fixed;right:10px;bottom:10px;z-index:20000;display:flex;gap:5px;padding:5px;border-radius:8px;background:#f1f4f8;color:#172333;font:12px system-ui';
                        const label = document.createElement('span');
                        label.textContent = '测试'; label.style.alignSelf = 'center'; bar.append(label);
                        for (const [label, command, args] of [
                            ['左移', 'qa_move_window', {right:false}],
                            ['右移', 'qa_move_window', {right:true}],
                            ['窄', 'qa_resize_window', {narrow:true}],
                            ['宽', 'qa_resize_window', {narrow:false}]
                        ]) {
                            const button = document.createElement('button');
                            button.textContent = label;
                            button.style.cssText = 'padding:4px 8px;border:1px solid #8795a7;border-radius:5px;background:white;color:#172333;font:inherit';
                            button.onclick = async () => {
                                try { await window.__TAURI_INTERNALS__.invoke(command, args); }
                                catch (error) { console.error('Material QA action failed', error); }
                            };
                            bar.append(button);
                        }
                        document.body.append(bar);
                        window.__TAURI_INTERNALS__.invoke('qa_input_ready');
                    }, {once: true});
                "#)
                .inner_size(425.0, 589.0).min_inner_size(250.0, 300.0).position(450.0, QA_WINDOW_TOP).build()?;
            window_material::set_qa_source(&main, backdrop.hwnd()?.0 as usize)?;
            if let Some(control) = setup_autorun {
                std::thread::spawn(move || control.run(main, backdrop));
            }
            Ok(())
        })
        .on_window_event(move |window, event| {
            if window.label() == "main" {
                match event {
                    tauri::WindowEvent::Resized(size) => println!("Material QA client_size={}x{} time_ms={}", size.width, size.height, qa_time_ms()),
                    tauri::WindowEvent::Moved(position) => println!("Material QA outer_position={},{} time_ms={}", position.x, position.y, qa_time_ms()),
                    _ => {}
                }
            }
            if let tauri::WindowEvent::CloseRequested { api, .. } = event {
                // Keep HWNDs alive until the shared exit hook retires the
                // compositor and finishes its current-thread queue shutdown.
                api.prevent_close();
                if let Some(control) = &autorun {
                    control.cancel(format!("close_requested:{}", window.label()));
                } else { window.app_handle().exit(0); }
            }
        })
        .build(context).expect("Material probe failed");
    application.run(|app, event| window_material::handle_run_event(app, &event));
}
}

#[cfg(target_os = "windows")]
fn main() {
    if std::env::args().nth(1).as_deref() == Some("--scope-blocker") {
        if let Err(error) = scope_blocker::run() { eprintln!("Scope blocker failed: {error}"); std::process::exit(1); }
    } else { probe::run(); }
}

#[cfg(not(target_os = "windows"))]
fn main() { eprintln!("The native material probe requires Windows 11."); }
