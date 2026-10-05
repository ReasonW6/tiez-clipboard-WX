//! A separate, bounded process supplies a real foreign-window scope obstacle.
//! It draws only a solid fixture tile and never captures or reads another UI.
use windows::{core::w, Win32::{Foundation::{COLORREF, HINSTANCE, HWND, LPARAM, LRESULT, WPARAM},
    Graphics::Gdi::{CreateSolidBrush, DeleteObject}, System::LibraryLoader::GetModuleHandleW,
    UI::WindowsAndMessaging::*}};

unsafe extern "system" fn window_proc(window: HWND, message: u32, wparam: WPARAM, lparam: LPARAM) -> LRESULT {
    match message {
        WM_TIMER => { let _ = DestroyWindow(window); LRESULT(0) }
        WM_DESTROY => {
            let time = std::time::SystemTime::now().duration_since(std::time::UNIX_EPOCH)
                .map(|duration| duration.as_millis()).unwrap_or_default();
            eprintln!("Material QA blocker_closed pid={} time_ms={}", std::process::id(), time);
            PostQuitMessage(0); LRESULT(0)
        }
        WM_MOUSEACTIVATE => LRESULT(MA_NOACTIVATE as isize),
        _ => DefWindowProcW(window, message, wparam, lparam),
    }
}

pub fn run() -> Result<(), String> {
    let values = std::env::args().skip(2).map(|value| value.parse::<i32>())
        .collect::<Result<Vec<_>, _>>().map_err(|error| error.to_string())?;
    if values.len() != 5 || !(1..=200).contains(&values[2]) || !(1..=200).contains(&values[3])
        || !(200..=5000).contains(&values[4]) { return Err("Invalid bounded blocker geometry/duration".into()); }
    unsafe {
        let instance = HINSTANCE(GetModuleHandleW(None).map_err(|error| error.to_string())?.0);
        let brush = CreateSolidBrush(COLORREF(0x009c42bc));
        let class = WNDCLASSW { hInstance: instance, lpszClassName: w!("TieZ.ScopeRecoveryFixture"),
            lpfnWndProc: Some(window_proc), hbrBackground: brush, ..Default::default() };
        if RegisterClassW(&class) == 0 { return Err(windows::core::Error::from_win32().to_string()); }
        let window = CreateWindowExW(WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE,
            class.lpszClassName, w!("TieZ scope recovery fixture"), WS_POPUP,
            values[0], values[1], values[2], values[3], None, None, Some(instance), None)
            .map_err(|error| error.to_string())?;
        if SetTimer(Some(window), 1, values[4] as u32, None) == 0 {
            let _ = DestroyWindow(window);
            return Err("Cannot arm fixture lifetime".into());
        }
        let _ = ShowWindow(window, SW_SHOWNOACTIVATE);
        println!("blocker.ready pid={} hwnd={:#x}", std::process::id(), window.0 as usize);
        let mut message = MSG::default();
        loop {
            let result = GetMessageW(&mut message, None, 0, 0).0;
            if result == -1 { return Err(windows::core::Error::from_win32().to_string()); }
            if result == 0 { break; }
            let _ = TranslateMessage(&message);
            DispatchMessageW(&message);
        }
        let _ = UnregisterClassW(class.lpszClassName, Some(instance));
        let _ = DeleteObject(brush.into());
    }
    Ok(())
}
