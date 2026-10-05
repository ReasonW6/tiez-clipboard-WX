//! Native Liquid Glass with a neutral variable-radius fallback behind the WebView.
//! Foreground text remains in the WebView and never passes through native optics.
#[path = "material_effects.rs"]
mod material_effects;
use serde::{Deserialize, Serialize};
use std::{
    cell::{Cell, RefCell},
    collections::HashMap,
    ffi::c_void,
    sync::atomic::{AtomicU64, Ordering},
};
use tauri::{AppHandle, Emitter, Manager, WebviewWindow};
use windows::{
    core::{s, w, Interface, BOOL},
    System::{DispatcherQueue, DispatcherQueueController},
    Win32::{
        Foundation::HWND,
        Graphics::Dwm::{DwmSetWindowAttribute, DWMWA_USE_HOSTBACKDROPBRUSH},
        System::{
            LibraryLoader::{GetModuleHandleW, GetProcAddress},
            WinRT::{
                Composition::ICompositorDesktopInterop, CreateDispatcherQueueController,
                DispatcherQueueOptions, DQTAT_COM_NONE, DQTYPE_THREAD_CURRENT,
            },
        },
    },
    UI::Composition::{
        CompositionEffectBrush, CompositionEffectSourceParameter, Compositor,
        Desktop::DesktopWindowTarget,
    },
};

#[derive(Clone, Copy, Debug, PartialEq)]
pub enum Material {
    None,
    Mica,
    Frosted,
    Clear,
}

pub fn material_for(theme: &str, windows_11: bool) -> Material {
    if !windows_11 {
        return Material::None;
    }
    match theme {
        "mica" => Material::Mica,
        "acrylic" => Material::Frosted,
        "liquid-glass" => Material::Clear,
        _ => Material::None,
    }
}

pub fn blur_radius(material: Material, opacity: f64) -> f32 {
    let density = if opacity.is_finite() {
        opacity.clamp(0.0, 100.0) / 100.0
    } else {
        0.5
    };
    let (min, max) = match material {
        Material::Mica => (32.0, 56.0),
        Material::Frosted => (18.0, 38.0),
        Material::Clear => (1.5, 16.0),
        Material::None => (0.0, 0.0),
    };
    (min + (max - min) * density) as f32
}

#[derive(Clone, Debug, Deserialize)]
#[serde(rename_all = "camelCase")]
pub struct GlassGeometry {
    pub viewport_width: f32,
    pub viewport_height: f32,
    pub controls: Vec<GlassControl>,
}

#[repr(C)]
#[derive(Clone, Copy, Debug, Default, Deserialize)]
#[serde(rename_all = "camelCase")]
pub struct GlassTrack {
    pub x: f32,
    pub y: f32,
    pub width: f32,
    pub height: f32,
    pub radius: f32,
    pub progress: f32,
    pub active_rgba: [f32; 4],
    pub inactive_rgba: [f32; 4],
}

#[repr(u32)]
#[derive(Clone, Copy, Debug, Default, Deserialize, PartialEq)]
#[serde(rename_all = "lowercase")]
pub enum GlassSurfaceMaterial {
    #[default]
    Clear = 0,
    Regular = 1,
    Reading = 2,
}

#[repr(C)]
#[derive(Clone, Copy, Debug, Deserialize)]
pub struct GlassControl {
    pub x: f32,
    pub y: f32,
    pub width: f32,
    pub height: f32,
    pub radius: f32,
    pub pressure: f32,
    #[serde(default)]
    pub material: GlassSurfaceMaterial,
    #[serde(default)]
    pub track: GlassTrack,
}

impl GlassGeometry {
    fn validate(&self) -> Result<(), String> {
        if !self.viewport_width.is_finite()
            || !self.viewport_height.is_finite()
            || self.viewport_width < 1.0
            || self.viewport_height < 1.0
            || self.controls.len() > 64
        {
            return Err("Invalid glass viewport or control count".into());
        }
        for control in &self.controls {
            if ![
                control.x,
                control.y,
                control.width,
                control.height,
                control.radius,
                control.pressure,
            ]
            .into_iter()
            .all(f32::is_finite)
                || control.width <= 0.0
                || control.height <= 0.0
                || control.radius < 0.0
                || !(0.0..=1.0).contains(&control.pressure)
            {
                return Err("Invalid glass control geometry".into());
            }
            let track = &control.track;
            let enabled = track.width > 0.0 && track.height > 0.0;
            if ![track.x, track.y, track.width, track.height, track.radius, track.progress]
                .into_iter().all(f32::is_finite)
                || !track.active_rgba.into_iter().chain(track.inactive_rgba)
                    .all(|channel| channel.is_finite() && (0.0..=1.0).contains(&channel))
                || (!enabled && (track.width != 0.0 || track.height != 0.0))
                || track.radius < 0.0 || track.radius > 0.5 * track.width.min(track.height)
                || !(0.0..=1.0).contains(&track.progress)
            {
                return Err("Invalid glass track geometry or color".into());
            }
        }
        Ok(())
    }
}

#[derive(Clone, Debug, Serialize)]
#[serde(rename_all = "camelCase")]
pub struct NativeGlassStatus {
    pub state: &'static str,
    pub revision: u64,
    pub geometry_revision: u64,
}
impl NativeGlassStatus {
    fn transition(&mut self, state: &'static str, geometry_revision: u64) -> bool {
        let geometry_revision = if state == "ready" { geometry_revision } else { 0 };
        if self.state == state && self.geometry_revision == geometry_revision {
            return false;
        }
        self.state = state;
        self.geometry_revision = geometry_revision;
        self.revision = REVISION.fetch_add(1, Ordering::Relaxed);
        true
    }
}

#[repr(C)]
struct NativeGlassState {
    viewport_width: f32,
    viewport_height: f32,
    blur_radius: f32,
    tint_rgba: [f32; 4],
    controls: *const GlassControl,
    control_count: u32,
    geometry_revision: u64,
}

#[link(name = "tiez_glass_renderer", kind = "static")]
unsafe extern "C" {
    fn tiez_glass_create(
        host: *mut c_void,
        qa_source: *mut c_void,
        context: *mut c_void,
        callback: extern "C" fn(*mut c_void, i32, i32, u64),
        result: *mut *mut c_void,
    ) -> i32;
    fn tiez_glass_update(renderer: *mut c_void, state: *const NativeGlassState) -> i32;
    fn tiez_glass_destroy(renderer: *mut c_void);
}

static GENERATION: AtomicU64 = AtomicU64::new(1);
static REVISION: AtomicU64 = AtomicU64::new(1);
static CALLBACK_SEQUENCE: AtomicU64 = AtomicU64::new(1);
// Process-wide receipts survive WebView reloads and cannot alias an old frame.
static GEOMETRY_REVISION: AtomicU64 = AtomicU64::new(1);

struct NativeCallback {
    window: WebviewWindow,
    id: usize,
    generation: u64,
    latest_sequence: AtomicU64,
}
struct NativeHandle {
    pointer: *mut c_void,
    _callback: Box<NativeCallback>,
}
impl Drop for NativeHandle {
    fn drop(&mut self) {
        // Native teardown unregisters callbacks before their context is freed.
        unsafe {
            tiez_glass_destroy(self.pointer);
        }
    }
}

extern "C" fn native_state_changed(context: *mut c_void, state: i32, result: i32, geometry_revision: u64) {
    let callback = unsafe { &*context.cast::<NativeCallback>() };
    let (id, generation) = (callback.id, callback.generation);
    let sequence = CALLBACK_SEQUENCE.fetch_add(1, Ordering::Relaxed);
    callback.latest_sequence.store(sequence, Ordering::Relaxed);
    let window = callback.window.clone();
    queue_native_state(window, id, generation, sequence, state, result, geometry_revision);
}

fn queue_native_state(
    window: WebviewWindow,
    id: usize,
    generation: u64,
    sequence: u64,
    state: i32,
    result: i32,
    geometry_revision: u64,
) {
    // run_on_main_thread may run inline when called from the UI thread. Queue
    // from the async runtime instead, after the C ABI call releases ENGINE.
    tauri::async_runtime::spawn(async move {
        let retry_window = window.clone();
        let _ = window.run_on_main_thread(move || {
            if EXIT_REQUESTED.with(Cell::get) {
                return;
            }
            let handled = ENGINE.with(|engine| {
                let Ok(mut engine) = engine.try_borrow_mut() else {
                    return false;
                };
                if let Some(engine) = engine.as_mut() {
                    engine.native_state(id, generation, sequence, state, result, geometry_revision);
                }
                true
            });
            // A native STA wait can pump messages before the outer FFI call
            // returns. Defer again instead of reentering the material engine.
            if !handled {
                queue_native_state(retry_window, id, generation, sequence, state, result, geometry_revision);
            }
        });
    });
}

struct WindowMaterial {
    window: WebviewWindow,
    material: Material,
    dark: bool,
    opacity: f64,
    geometry: Option<GlassGeometry>,
    geometry_revision: u64,
    qa_source: usize,
    native: Option<NativeHandle>,
    generation: u64,
    callback_sequence: u64,
    status: NativeGlassStatus,
}
impl WindowMaterial {
    fn publish(&mut self, state: &'static str) {
        self.publish_geometry(state, 0);
    }

    fn publish_geometry(&mut self, state: &'static str, geometry_revision: u64) {
        if !self.status.transition(state, geometry_revision) {
            return;
        }
        let _ = self
            .window
            .emit_to(self.window.label(), "native-glass-status", &self.status);
    }

    fn stop_native(&mut self) {
        self.generation = GENERATION.fetch_add(1, Ordering::Relaxed);
        self.native.take();
    }

    fn parameters(&self) -> NativeGlassState {
        let geometry = self.geometry.as_ref().expect("Native geometry initialized");
        NativeGlassState {
            viewport_width: geometry.viewport_width,
            viewport_height: geometry.viewport_height,
            blur_radius: blur_radius(self.material, self.opacity),
            tint_rgba: liquid_tint(self.dark, self.opacity),
            controls: geometry.controls.as_ptr(),
            control_count: geometry.controls.len() as u32,
            geometry_revision: self.geometry_revision,
        }
    }
}

fn liquid_tint(dark: bool, opacity: f64) -> [f32; 4] {
    let density = if opacity.is_finite() {
        opacity.clamp(0.0, 100.0) / 100.0
    } else {
        0.5
    };
    let color = if dark {
        [23.0, 31.0, 43.0]
    } else {
        [245.0, 248.0, 252.0]
    };
    [
        color[0] / 255.0,
        color[1] / 255.0,
        color[2] / 255.0,
        (0.74 + density * 0.16) as f32,
    ]
}

struct Backdrop {
    target: DesktopWindowTarget,
    brush: CompositionEffectBrush,
}
impl Drop for Backdrop {
    fn drop(&mut self) {
        let _ = self.target.Close();
    }
}
struct Engine {
    // Child resources must be released before their compositor and queue.
    windows: HashMap<usize, WindowMaterial>,
    backdrops: HashMap<usize, Backdrop>,
    compositor: Compositor,
}

thread_local! {
    static ENGINE: RefCell<Option<Engine>> = const { RefCell::new(None) };
    // Keep ownership even if Compositor::new fails after queue creation.
    static OWNED_QUEUE: RefCell<Option<DispatcherQueueController>> = const { RefCell::new(None) };
    static EXIT_REQUESTED: Cell<bool> = const { Cell::new(false) };
    static EXIT_READY: Cell<bool> = const { Cell::new(false) };
}

impl Engine {
    fn new() -> windows::core::Result<Self> {
        if DispatcherQueue::GetForCurrentThread().is_err() {
            let queue = unsafe {
                CreateDispatcherQueueController(DispatcherQueueOptions {
                    dwSize: std::mem::size_of::<DispatcherQueueOptions>() as u32,
                    threadType: DQTYPE_THREAD_CURRENT,
                    apartmentType: DQTAT_COM_NONE,
                })?
            };
            OWNED_QUEUE.with(|owned| *owned.borrow_mut() = Some(queue));
        }
        Ok(Self {
            compositor: Compositor::new()?,
            windows: HashMap::new(),
            backdrops: HashMap::new(),
        })
    }

    fn baseline(
        &mut self,
        id: usize,
        material: Material,
        opacity: f64,
    ) -> windows::core::Result<()> {
        if material == Material::None {
            self.backdrops.remove(&id);
            return Ok(());
        }
        if let Some(backdrop) = self.backdrops.get_mut(&id) {
            backdrop
                .brush
                .Properties()?
                .InsertScalar(&"Blur.BlurAmount".into(), blur_radius(material, opacity))?;
        } else {
            let interop: ICompositorDesktopInterop = self.compositor.cast()?;
            let target = unsafe { interop.CreateDesktopWindowTarget(HWND(id as _), false)? };
            let visual = self.compositor.CreateSpriteVisual()?;
            let mut relative_size = visual.RelativeSizeAdjustment()?;
            relative_size.X = 1.0;
            relative_size.Y = 1.0;
            visual.SetRelativeSizeAdjustment(relative_size)?;
            let source = CompositionEffectSourceParameter::Create(&"Backdrop".into())?;
            let effect = material_effects::gaussian_blur(source.cast()?);
            let properties: windows_collections::IIterable<windows::core::HSTRING> =
                vec!["Blur.BlurAmount".into()].into();
            let brush = self
                .compositor
                .CreateEffectFactoryWithProperties(&effect, &properties)?
                .CreateBrush()?;
            brush
                .SetSourceParameter(&"Backdrop".into(), &self.compositor.CreateBackdropBrush()?)?;
            brush
                .Properties()?
                .InsertScalar(&"Blur.BlurAmount".into(), blur_radius(material, opacity))?;
            visual.SetBrush(&brush)?;
            target.SetRoot(&visual)?;
            self.backdrops.insert(id, Backdrop { target, brush });
        }
        Ok(())
    }

    fn ensure_window(&mut self, id: usize, window: WebviewWindow) {
        if self.windows.contains_key(&id) {
            return;
        }
        let app = window.app_handle().clone();
        window.on_window_event(move |event| {
            if matches!(event, tauri::WindowEvent::Destroyed) {
                retire_window(app.clone(), id);
            }
        });
        self.windows.insert(
            id,
            WindowMaterial {
                window,
                material: Material::None,
                dark: false,
                opacity: 50.0,
                geometry: None,
                geometry_revision: 0,
                qa_source: 0,
                native: None,
                generation: 0,
                callback_sequence: 0,
                status: NativeGlassStatus {
                    state: "inactive",
                    revision: 0,
                    geometry_revision: 0,
                },
            },
        );
    }

    fn retire(mut self) {
        eprintln!(
            "[window-material] phase=retire_started windows={} backdrops={}",
            self.windows.len(),
            self.backdrops.len()
        );
        self.windows.clear();
        self.backdrops.clear();
        match self.compositor.Close() {
            Ok(()) => eprintln!("[window-material] phase=compositor_closed hr=0x00000000"),
            Err(error) => eprintln!(
                "[window-material] phase=compositor_close_failed hr={:#010x}",
                error.code().0
            ),
        }
    }

    fn reconcile(&mut self, id: usize, retry: bool) -> Result<(), String> {
        let mut window = self
            .windows
            .remove(&id)
            .expect("Registered material window");
        let result = self.reconcile_window(id, &mut window, retry);
        self.windows.insert(id, window);
        result
    }

    fn reconcile_window(
        &mut self,
        id: usize,
        window: &mut WindowMaterial,
        retry: bool,
    ) -> Result<(), String> {
        if window.material != Material::Clear || window.geometry.is_none() {
            window.stop_native();
            self.baseline(id, window.material, window.opacity)
                .map_err(|error| error.to_string())?;
            window.publish(if window.material == Material::Clear {
                "pending"
            } else {
                "inactive"
            });
            return Ok(());
        }
        if window.native.is_none() {
            if window.status.state == "failed" && !retry {
                return self
                    .baseline(id, window.material, window.opacity)
                    .map_err(|error| error.to_string());
            }
            // WUC and DComp cannot both own this HWND's below-child target.
            self.backdrops.remove(&id);
            window.generation = GENERATION.fetch_add(1, Ordering::Relaxed);
            window.callback_sequence = 0;
            window.publish("pending");
            let mut callback = Box::new(NativeCallback {
                window: window.window.clone(),
                id,
                generation: window.generation,
                latest_sequence: AtomicU64::new(0),
            });
            let mut pointer = std::ptr::null_mut();
            let result = unsafe {
                tiez_glass_create(
                    id as _,
                    window.qa_source as _,
                    (&mut *callback as *mut NativeCallback).cast(),
                    native_state_changed,
                    &mut pointer,
                )
            };
            if result < 0 || pointer.is_null() {
                if !pointer.is_null() {
                    unsafe {
                        tiez_glass_destroy(pointer);
                    }
                }
                self.fail_native(id, window, result)?;
                return Ok(());
            }
            window.native = Some(NativeHandle {
                pointer,
                _callback: callback,
            });
        }
        let native = window.native.as_ref().unwrap();
        let result = unsafe { tiez_glass_update(native.pointer, &window.parameters()) };
        if result < 0 {
            self.fail_native(id, window, result)?;
        }
        Ok(())
    }

    fn fail_native(
        &mut self,
        id: usize,
        window: &mut WindowMaterial,
        result: i32,
    ) -> Result<(), String> {
        window.stop_native();
        window.publish("failed");
        eprintln!("Native glass stopped for HWND {id:#x}: HRESULT {result:#010x}");
        self.baseline(id, window.material, window.opacity)
            .map_err(|error| error.to_string())
    }

    fn native_state(&mut self, id: usize, generation: u64, sequence: u64, state: i32, result: i32, geometry_revision: u64) {
        let Some(mut window) = self.windows.remove(&id) else {
            return;
        };
        if window.generation == generation
            && window.native.as_ref().is_some_and(|native|
                native._callback.latest_sequence.load(Ordering::Relaxed) == sequence)
            && sequence > window.callback_sequence
        {
            window.callback_sequence = sequence;
            match state {
                1 => window.publish_geometry("ready", geometry_revision),
                3 => window.publish("resizing"),
                2 => {
                    if let Err(error) = self.fail_native(id, &mut window, result) {
                        eprintln!("Material fallback failed: {error}");
                    }
                }
                _ => window.publish(if window.status.state == "pending" {
                    "pending"
                } else {
                    "suspended"
                }),
            }
        }
        self.windows.insert(id, window);
    }
}

fn retire_window(app: AppHandle, id: usize) {
    let retired = ENGINE.with(|state| {
        let Ok(mut state) = state.try_borrow_mut() else {
            return Err(());
        };
        Ok(state
            .as_mut()
            .map(|engine| (engine.windows.remove(&id), engine.backdrops.remove(&id))))
    });
    match retired {
        Ok(Some((window, backdrop))) => {
            // Native and WUC destructors can pump STA messages. Never invoke
            // them while ENGINE is borrowed, including the Destroyed path.
            drop(window);
            drop(backdrop);
        }
        Ok(None) => {}
        Err(()) => {
            tauri::async_runtime::spawn(async move {
                let target = app.clone();
                let _ = app.run_on_main_thread(move || retire_window(target, id));
            });
        }
    }
}

/// Retire the UI-thread engine before Tauri stops its message loop. The queue
/// was created with DQTYPE_THREAD_CURRENT, so ShutdownQueueAsync needs that
/// original loop to keep pumping until completion.
pub fn handle_run_event(app: &AppHandle, event: &tauri::RunEvent) {
    let tauri::RunEvent::ExitRequested { api, code, .. } = event else {
        return;
    };
    if EXIT_READY.with(Cell::get) {
        return;
    }
    api.prevent_exit();
    if EXIT_REQUESTED.with(|closing| closing.replace(true)) {
        return;
    }
    retire_for_exit(app.clone(), code.unwrap_or(0));
}

fn retire_for_exit(app: AppHandle, exit_code: i32) {
    let retired = ENGINE.with(|state| {
        state
            .try_borrow_mut()
            .map(|mut state| state.take())
            .map_err(|_| ())
    });
    let engine = match retired {
        Ok(engine) => engine,
        Err(()) => {
            tauri::async_runtime::spawn(async move {
                let target = app.clone();
                let _ = app.run_on_main_thread(move || retire_for_exit(target, exit_code));
            });
            return;
        }
    };
    // ENGINE is now empty and no RefCell guard is alive while resources close.
    if let Some(engine) = engine {
        engine.retire();
    }
    // Only a queue created by this module belongs to us. An existing
    // dispatcher remains the responsibility of its original owner.
    let queue = OWNED_QUEUE.with(|owned| owned.borrow_mut().take());
    let Some(queue) = queue else {
        release_exit(app, exit_code);
        return;
    };
    match queue.ShutdownQueueAsync() {
        Ok(shutdown) => {
            eprintln!("[window-material] phase=queue_shutdown_started");
            // This windows-future build exposes get(), but not IntoFuture.
            // Wait on a blocking worker while the original UI loop stays active.
            // Retain the owned controller, then release it on its UI thread.
            tauri::async_runtime::spawn_blocking(move || {
                let result = shutdown.get();
                let target = app.clone();
                let _ = app.run_on_main_thread(move || {
                    match result {
                        Ok(()) => eprintln!(
                            "[window-material] phase=queue_shutdown_completed hr=0x00000000"
                        ),
                        Err(error) => eprintln!(
                            "[window-material] phase=queue_shutdown_failed hr={:#010x}",
                            error.code().0
                        ),
                    }
                    drop(queue);
                    release_exit(target, exit_code);
                });
            });
        }
        Err(error) => {
            eprintln!(
                "[window-material] phase=queue_shutdown_failed hr={:#010x}",
                error.code().0
            );
            drop(queue);
            release_exit(app, exit_code);
        }
    }
}

fn release_exit(app: AppHandle, exit_code: i32) {
    EXIT_READY.with(|ready| ready.set(true));
    eprintln!("[window-material] phase=exit_released code={exit_code}");
    app.exit(exit_code);
}

fn on_main_thread<T: Send + 'static>(
    window: &WebviewWindow,
    operation: impl FnOnce() -> Result<T, String> + Send + 'static,
) -> Result<T, String> {
    let (sender, receiver) = std::sync::mpsc::sync_channel(1);
    window
        .run_on_main_thread(move || {
            let result = if EXIT_REQUESTED.with(Cell::get) {
                Err("The window material is shutting down".into())
            } else {
                operation()
            };
            let _ = sender.send(result);
        })
        .map_err(|error| error.to_string())?;
    receiver.recv().map_err(|error| error.to_string())?
}

// Disable the accent compositor used by the previous material implementation.
fn clear_accent(hwnd: HWND) {
    #[repr(C)]
    struct Policy {
        state: u32,
        flags: u32,
        color: u32,
        animation: u32,
    }
    #[repr(C)]
    struct Attribute {
        attribute: u32,
        data: *mut c_void,
        size: usize,
    }
    type SetComposition = unsafe extern "system" fn(HWND, *mut Attribute) -> BOOL;
    unsafe {
        if let Ok(module) = GetModuleHandleW(w!("user32.dll")) {
            if let Some(address) = GetProcAddress(module, s!("SetWindowCompositionAttribute")) {
                let apply: SetComposition = std::mem::transmute(address);
                let mut policy = Policy {
                    state: 0,
                    flags: 0,
                    color: 0,
                    animation: 0,
                };
                let mut attribute = Attribute {
                    attribute: 19,
                    data: (&mut policy as *mut Policy).cast(),
                    size: std::mem::size_of::<Policy>(),
                };
                let _ = apply(hwnd, &mut attribute);
            }
        }
    }
}

pub fn apply(window: &WebviewWindow, theme: &str, dark: bool, opacity: f64) -> Result<(), String> {
    let material = material_for(theme, windows_version::OsVersion::current().build >= 22000);
    let id = window.hwnd().map_err(|error| error.to_string())?.0 as usize;
    let target = window.clone();
    on_main_thread(window, move || {
        let hwnd = HWND(id as _);
        clear_accent(hwnd);
        let _ = window_vibrancy::clear_acrylic(&target);
        let _ = window_vibrancy::clear_mica(&target);
        if material != Material::None {
            let enabled = BOOL(1);
            unsafe {
                DwmSetWindowAttribute(
                    hwnd,
                    DWMWA_USE_HOSTBACKDROPBRUSH,
                    (&enabled as *const BOOL).cast(),
                    std::mem::size_of::<BOOL>() as u32,
                )
            }
            .map_err(|error| error.to_string())?;
        }
        ENGINE.with(|state| {
            let mut state = state.borrow_mut();
            if state.is_none() {
                *state = Some(Engine::new().map_err(|error| error.to_string())?);
            }
            let engine = state.as_mut().unwrap();
            engine.ensure_window(id, target);
            let entry = engine.windows.get_mut(&id).unwrap();
            entry.material = material;
            entry.dark = dark;
            entry.opacity = opacity;
            engine.reconcile(id, true)
        })
    })
}

pub fn set_opacity(window: &WebviewWindow, opacity: f64) -> Result<(), String> {
    let id = window.hwnd().map_err(|error| error.to_string())?.0 as usize;
    on_main_thread(window, move || {
        ENGINE.with(|state| {
            let mut state = state.borrow_mut();
            if let Some(engine) = state.as_mut() {
                if let Some(window) = engine.windows.get_mut(&id) {
                    window.opacity = opacity;
                    engine.reconcile(id, false)?;
                }
            }
            Ok(())
        })
    })
}

/// Internal fixture injection used by material_probe, never an IPC setting.
pub fn set_qa_source(window: &WebviewWindow, source: usize) -> Result<(), String> {
    let id = window.hwnd().map_err(|error| error.to_string())?.0 as usize;
    let target = window.clone();
    on_main_thread(window, move || {
        ENGINE.with(|state| {
            let mut state = state.borrow_mut();
            if state.is_none() {
                *state = Some(Engine::new().map_err(|error| error.to_string())?);
            }
            let engine = state.as_mut().unwrap();
            engine.ensure_window(id, target);
            let window = engine.windows.get_mut(&id).unwrap();
            if window.qa_source != source {
                window.stop_native();
                window.qa_source = source;
            }
            engine.reconcile(id, true)
        })
    })
}

pub fn submit_geometry(window: &WebviewWindow, geometry: GlassGeometry) -> Result<u64, String> {
    geometry.validate()?;
    let id = window.hwnd().map_err(|error| error.to_string())?.0 as usize;
    let target = window.clone();
    on_main_thread(window, move || {
        ENGINE.with(|state| {
            let mut state = state.borrow_mut();
            if state.is_none() {
                *state = Some(Engine::new().map_err(|error| error.to_string())?);
            }
            let engine = state.as_mut().unwrap();
            engine.ensure_window(id, target);
            let revision = GEOMETRY_REVISION.fetch_add(1, Ordering::Relaxed);
            let window = engine.windows.get_mut(&id).unwrap();
            window.geometry = Some(geometry);
            window.geometry_revision = revision;
            engine.reconcile(id, false)?;
            Ok(revision)
        })
    })
}

pub fn native_glass_status(window: &WebviewWindow) -> Result<NativeGlassStatus, String> {
    let id = window.hwnd().map_err(|error| error.to_string())?.0 as usize;
    on_main_thread(window, move || {
        ENGINE.with(|state| {
            Ok(state
                .borrow()
                .as_ref()
                .and_then(|engine| engine.windows.get(&id))
                .map(|window| window.status.clone())
                .unwrap_or(NativeGlassStatus {
                    state: "inactive",
                    revision: 0,
                    geometry_revision: 0,
                }))
        })
    })
}

#[cfg(test)]
mod tests {
    use super::*;
    #[test]
    fn materials_keep_distinct_clear_ends_without_wallpaper_tint() {
        assert_eq!(blur_radius(Material::Clear, 0.0), 1.5);
        assert_eq!(blur_radius(Material::Frosted, 0.0), 18.0);
        assert_eq!(blur_radius(Material::Mica, 0.0), 32.0);
        for theme in ["mica", "acrylic", "liquid-glass"] {
            assert_ne!(material_for(theme, true), Material::None);
            assert_eq!(material_for(theme, false), Material::None);
        }
    }
    #[test]
    fn blur_changes_radius_continuously_without_fading_the_material() {
        for material in [Material::Clear, Material::Frosted, Material::Mica] {
            for opacity in 0..100 {
                let step = blur_radius(material, opacity as f64 + 1.0)
                    - blur_radius(material, opacity as f64);
                assert!(step > 0.0 && step < 0.4);
            }
            assert!(blur_radius(material, 100.0) > blur_radius(material, 0.0) + 10.0);
            assert_eq!(blur_radius(material, f64::NAN), blur_radius(material, 50.0));
        }
    }

    #[test]
    fn geometry_rejects_invalid_ipc_without_losing_partially_visible_controls() {
        let mut geometry = GlassGeometry {
            viewport_width: 352.0,
            viewport_height: 380.0,
            controls: vec![GlassControl {
                x: -5.0,
                y: 8.0,
                width: 20.0,
                height: 20.0,
                radius: 10.0,
                pressure: 0.0,
                material: GlassSurfaceMaterial::Clear,
                track: GlassTrack::default(),
            }],
        };
        assert!(geometry.validate().is_ok());
        geometry.controls[0].pressure = f32::NAN;
        assert!(geometry.validate().is_err());
        geometry.controls[0].pressure = 0.5;
        geometry.controls.resize(65, geometry.controls[0]);
        assert!(geometry.validate().is_err());
        geometry.controls.clear();
        geometry.viewport_width = 1e-40;
        assert!(geometry.validate().is_err());
    }

    #[test]
    fn glass_track_ipc_is_optional_and_bounded() {
        let original = serde_json::json!({
            "viewportWidth": 425, "viewportHeight": 589,
            "controls": [{"x": 40, "y": 40, "width": 30, "height": 22, "radius": 11, "pressure": 0}]
        });
        let mut geometry: GlassGeometry = serde_json::from_value(original).unwrap();
        assert!(geometry.validate().is_ok());
        let track: GlassTrack = serde_json::from_value(serde_json::json!({
            "x": 20, "y": 47, "width": 300, "height": 8, "radius": 4,
            "progress": 0.5, "activeRgba": [0.2, 0.4, 0.8, 1], "inactiveRgba": [0.2, 0.4, 0.8, 0.1]
        })).unwrap();
        geometry.controls[0].track = track;
        for progress in [0.0, 0.5, 1.0] {
            geometry.controls[0].track.progress = progress;
            assert!(geometry.validate().is_ok());
        }
        geometry.controls[0].track.active_rgba[3] = 1.01;
        assert!(geometry.validate().is_err());
        geometry.controls[0].track = track;
        geometry.controls[0].track.width = 0.0;
        assert!(geometry.validate().is_err());
        geometry.controls[0].track = track;
        geometry.controls[0].track.x = f32::NAN;
        assert!(geometry.validate().is_err());
    }

    #[test]
    fn multiple_tracks_accept_distinct_values_and_validate_every_control() {
        let controls = [0.0, 0.5, 1.0].into_iter().enumerate().map(|(index, progress)| {
            serde_json::json!({
                "x": 20, "y": 40 + index * 50, "width": 30, "height": 22, "radius": 11, "pressure": 0,
                "track": {"x": 20, "y": 47 + index * 50, "width": 300, "height": 8, "radius": 4,
                    "progress": progress, "activeRgba": [progress, 0.4, 0.8, 1], "inactiveRgba": [0.2, progress, 0.8, 0.1]}
            })
        }).collect::<Vec<_>>();
        let mut geometry: GlassGeometry = serde_json::from_value(serde_json::json!({
            "viewportWidth": 425, "viewportHeight": 1400, "controls": controls
        })).unwrap();
        assert!(geometry.validate().is_ok());
        for index in 0..geometry.controls.len() {
            let track = geometry.controls[index].track;
            assert_eq!(track.progress, index as f32 / 2.0);
            assert_eq!(track.active_rgba[0], track.progress);
            assert_eq!(track.inactive_rgba[1], track.progress);
            for invalid in [
                GlassTrack { width: 0.0, ..track },
                GlassTrack { height: 0.0, ..track },
                GlassTrack { radius: 5.0, ..track },
                GlassTrack { progress: -0.1, ..track },
                GlassTrack { progress: 1.01, ..track },
                GlassTrack { x: f32::NAN, ..track },
                GlassTrack { y: f32::INFINITY, ..track },
                GlassTrack { active_rgba: [f32::NAN, 0.4, 0.8, 1.0], ..track },
                GlassTrack { inactive_rgba: [0.2, 0.4, 0.8, 1.01], ..track },
            ] {
                geometry.controls[index].track = invalid;
                assert!(geometry.validate().is_err(), "invalid track at control {index}");
            }
            geometry.controls[index].track = track;
        }
        geometry.controls.resize(64, geometry.controls[0]);
        assert!(geometry.validate().is_ok());
        geometry.controls.push(geometry.controls[0]);
        assert!(geometry.validate().is_err());
    }

    #[test]
    fn surface_material_is_explicit_and_defaults_to_clear_for_older_ipc() {
        let original = serde_json::json!({
            "x": 20, "y": 40, "width": 300, "height": 200, "radius": 24, "pressure": 0
        });
        let clear: GlassControl = serde_json::from_value(original.clone()).unwrap();
        assert_eq!(clear.material, GlassSurfaceMaterial::Clear);
        for (name, expected) in [
            ("clear", GlassSurfaceMaterial::Clear),
            ("regular", GlassSurfaceMaterial::Regular),
            ("reading", GlassSurfaceMaterial::Reading),
        ] {
            let mut value = original.clone();
            value["material"] = serde_json::json!(name);
            let control: GlassControl = serde_json::from_value(value).unwrap();
            assert_eq!(control.material, expected);
            assert_eq!(control.track.width, 0.0);
        }
        for invalid in [serde_json::json!("frosted"), serde_json::json!(2), serde_json::json!(null)] {
            let mut value = original.clone();
            value["material"] = invalid;
            assert!(serde_json::from_value::<GlassControl>(value).is_err());
        }
        assert_eq!(std::mem::size_of::<GlassSurfaceMaterial>(), 4);
    }

    #[test]
    fn geometry_acknowledgements_survive_ready_to_ready_and_clear_on_suspension() {
        let mut status = NativeGlassStatus { state: "inactive", revision: 0, geometry_revision: 0 };
        assert!(status.transition("ready", 41));
        let first_revision = status.revision;
        assert!(!status.transition("ready", 41));
        assert!(status.transition("ready", 42));
        assert!(status.revision > first_revision);
        assert_eq!(serde_json::to_value(&status).unwrap()["geometryRevision"], 42);
        for state in ["resizing", "suspended", "failed", "inactive"] {
            assert!(status.transition(state, 42));
            assert_eq!(status.geometry_revision, 0);
        }
        assert!(status.transition("ready", 42));
        assert_eq!(status.geometry_revision, 42);
    }

    #[test]
    fn native_tint_matches_the_readable_liquid_surface_endpoints() {
        assert_eq!(
            liquid_tint(false, 0.0),
            [245.0 / 255.0, 248.0 / 255.0, 252.0 / 255.0, 0.74]
        );
        assert_eq!(
            liquid_tint(true, 100.0),
            [23.0 / 255.0, 31.0 / 255.0, 43.0 / 255.0, 0.90]
        );
        assert_eq!(liquid_tint(true, f64::NAN), liquid_tint(true, 50.0));
        assert_eq!(std::mem::size_of::<GlassTrack>(), 56);
        assert_eq!(std::mem::size_of::<GlassControl>(), 84);
        assert_eq!(std::mem::offset_of!(GlassControl, material), 24);
        assert_eq!(std::mem::offset_of!(GlassControl, track), 28);
        let revision_offset = if cfg!(target_pointer_width = "64") { 48 } else { 40 };
        assert_eq!(std::mem::offset_of!(NativeGlassState, geometry_revision), revision_offset);
        assert_eq!(std::mem::size_of::<NativeGlassState>(), revision_offset + 8);
        assert_eq!(
            std::mem::offset_of!(NativeGlassState, controls),
            if cfg!(target_pointer_width = "64") {
                32
            } else {
                28
            }
        );
    }
}
