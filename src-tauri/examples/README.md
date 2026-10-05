# Native material probe

On Windows 11, start the frontend with `npm run dev`, then run:

```powershell
cargo run --manifest-path src-tauri/Cargo.toml --example material_probe
```

The probe opens the production frontend over a fixed test background, uses the production `window_material` implementation, and keeps settings and clipboard history in memory. It does not start the application's database, clipboard watcher, autostart, or shortcut setup. A theme update changes the window title after the native material succeeds.

The isolated WebView profile is stored in `src-tauri/target/material-probe-profile`, outside Vite's file watcher. Close either probe window to exit both. This profile is disposable after the probe exits.

Check all three materials in light and dark mode, the two opacity endpoints and intermediate stops, and switch repeatedly between materials. The current implementation uses neutral backdrop colors with distinct blur ranges for the three themes. Browser regressions for empty states and selection clipping live in `tests/ui/material-regression.spec.ts`.
