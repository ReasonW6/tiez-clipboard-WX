//! Mica and acrylic: the compositor blurs the desktop behind the WebView.
#[path = "material_effects.rs"]
mod material_effects;
use std::{
    cell::{Cell, RefCell},
    collections::HashMap,
    ffi::c_void,
};
use tauri::{AppHandle, Manager, WebviewWindow};
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
}

pub fn material_for(theme: &str, windows_11: bool) -> Material {
    if !windows_11 {
        return Material::None;
    }
    match theme {
        "mica" => Material::Mica,
        "acrylic" => Material::Frosted,
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
        Material::Mica => (40.0, 64.0),
        Material::Frosted => (18.0, 38.0),
        Material::None => (0.0, 0.0),
    };
    (min + (max - min) * density) as f32
}

struct WindowMaterial {
    material: Material,
    opacity: f64,
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
                material: Material::None,
                opacity: 50.0,
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

    fn reconcile(&mut self, id: usize) -> Result<(), String> {
        let window = &self.windows[&id];
        let (material, opacity) = (window.material, window.opacity);
        self.baseline(id, material, opacity)
            .map_err(|error| error.to_string())
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
        Ok(Some((_, backdrop))) => {
            // WUC destructors can pump STA messages. Never invoke them while
            // ENGINE is borrowed, including the Destroyed path.
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

pub fn apply(window: &WebviewWindow, theme: &str, opacity: f64) -> Result<(), String> {
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
            entry.opacity = opacity;
            engine.reconcile(id)
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
                    engine.reconcile(id)?;
                }
            }
            Ok(())
        })
    })
}

#[cfg(test)]
mod tests {
    use super::*;
    #[test]
    fn materials_keep_distinct_clear_ends_without_wallpaper_tint() {
        assert_eq!(blur_radius(Material::Frosted, 0.0), 18.0);
        assert_eq!(blur_radius(Material::Mica, 0.0), 40.0);
        for theme in ["mica", "acrylic"] {
            assert_ne!(material_for(theme, true), Material::None);
            assert_eq!(material_for(theme, false), Material::None);
        }
    }
    #[test]
    fn blur_changes_radius_continuously_without_fading_the_material() {
        for material in [Material::Frosted, Material::Mica] {
            for opacity in 0..100 {
                let step = blur_radius(material, opacity as f64 + 1.0)
                    - blur_radius(material, opacity as f64);
                assert!(step > 0.0 && step < 0.4);
            }
            assert!(blur_radius(material, 100.0) > blur_radius(material, 0.0) + 10.0);
            assert_eq!(blur_radius(material, f64::NAN), blur_radius(material, 50.0));
        }
    }
}
