# WGC optical feasibility probe

This isolated C++/WinRT program creates two native windows. The rear window draws an animated 48 px grid, moving checkerboard, marker and test text. WGC captures only that owned HWND. The front window samples that texture with a rounded rectangular bevel, refraction and small wavelength-dependent offsets, then presents it through D3D11 and DirectComposition. Neither window uses capture protection. No Tauri application state, real clipboard, other application window, monitor capture or installation is involved.

Build without launching:

```powershell
powershell.exe -NoProfile -ExecutionPolicy Bypass -File src-tauri/examples/build-glass-capture-probe.ps1
```

The script uses the existing Visual Studio x64 C++ tools and Windows SDK, checks both HLSL entry points with `fxc`, and compiles the executable. It does not modify `Cargo.toml`. Build evidence and output go to `artifacts/material-redesign/probe`.

After project-window observation has been authorized, launch `artifacts/material-redesign/probe/glass_capture_probe.exe`. Window titles start with `TieZ Glass Capture QA`. Focus either window for keyboard controls:

| Key | Action |
| --- | --- |
| F2 (R also supported) | Toggle refraction. With it disabled, use the grid to check alignment. |
| F3 (C also supported) | Toggle the small chromatic dispersion. |
| F4 | Pause / resume the generated source animation at its current time. |
| F5 (B also supported) | Toggle the session's `IsBorderRequired` property and log the setter result. |
| Escape | Close both owned windows and release the capture / GPU resources. |

Move or resize the front window using its standard title bar and edges. Keep it over the owned background window, since this probe deliberately has no pixels from any other source. The sample position is the front client origin in screen coordinates minus the source's `DWMWA_EXTENDED_FRAME_BOUNDS` origin, plus the front fragment's pixel position. Source texture dimensions establish the pixel ratio. The source is not fit or stretched into the front window. The shader's center has no displacement; only its continuous bevel changes the sampling position. A small uniform transmission adjustment remains when refraction is off.

Use F4 before F2 for a static before / after comparison. Pausing fixes the generated animation time and stops periodic source invalidation, while capture polling and front-window rendering continue. Mode changes repaint only the status labels; they do not advance paused geometry. Function keys avoid Chinese IME composition intercepting letter shortcuts. Both window titles and the source label show the actual refraction, dispersion and animation state.

`evidence.jsonl` is created beside the executable at each launch. It contains initialization, source ownership, both `GetWindowDisplayAffinity` results, capture dimensions, first frame, cumulative frames, WGC source timestamps, frame age, settings and shutdown. Progress also records source screen bounds, front client screen origin, output dimensions and source pixel ratios, so coordinate alignment can be checked. It contains no pixel data. `build-evidence.json` records C++ / shader compilation and SHA-256. Compilation is not a claim that WGC or visual quality has passed.

The default capture border is retained. F5 / B does not request capture permission or alter a system setting. Microsoft documents that `IsBorderRequired(false)` can return success even when the system ignores it because borderless access was not granted; another capture session can also require the border. Observe the actual source window border independently and record that result. The probe never calls `GraphicsCaptureAccess.RequestAccessAsync`.[Microsoft border semantics](https://learn.microsoft.com/en-us/uwp/api/windows.graphics.capture.graphicscapturesession.isborderrequired?view=winrt-26100)

## First runtime evidence, 2026-09-14

The main agent ran the first build under the user's explicit authorization to inspect this project's test windows. Its preserved `artifacts/material-redesign/probe/first-run-evidence.jsonl` records a first frame at approximately 500 ms, a 1026 × 783 source texture, 3872 source frames over approximately 125 seconds, and 4452 output presentations. Both window affinities remained zero. The main agent's window capture showed the DirectComposition output and visible edge bending while the output window obscured its source. Escape closed both windows and the log contains `closed` / `shutdown`.

The first run did not establish strict optical A/B or pixel alignment: Chinese IME intercepted letter shortcuts, and no refraction toggle was logged. F2–F5 controls and static source pause were added for that follow-up. This first result verifies the basic public, owned-window WGC → D3D11 → capturable-output chain on this machine; it is not a completed desktop material, universal capture compatibility result, or Apple / iOS equivalence claim.

## Second runtime evidence, 2026-09-14

The main agent verified F4 source pause and F2 optical switching in the second build. With the same frozen source, the enabled view showed curved edge sampling and disabling refraction restored straight lines. The saved comparisons are `artifacts/material-redesign/qa/native-refraction-on.jpg` and `native-refraction-off.jpg`. Moving the output by +100, +50 changed the logged client origin from 418, 206 to 518, 256; source X/Y scales stayed 1, and the observed grid remained continuous. `native-moved-alignment.jpg` preserves that comparison. `second-run-evidence.jsonl` contains the controls, geometry and clean shutdown. Computer Use returns JPEG; these images are visual evidence, not lossless pixel identity evidence.

This confirms the single-owned-window optical / coordinate path on the test machine. The static first implementation still submitted repeated identical frames at roughly 35 presentations per second. The separate two-source probe adds a condition to skip unchanged presentations, with its own runtime validation required. This is not a measured 60 fps, production power-use or complete-desktop equivalence claim.

Acceptance for this probe is limited to: a changing captured texture from the owned window while obscured by the front window; coordinate alignment; real edge displacement; small controlled dispersion; both affinities equal zero; and the front output remaining visible in ordinary window / monitor capture. Multi-window desktop reconstruction, minimized-window freshness, HDR, final Apple visual equivalence, content contrast and production integration remain outside this probe's result. Full research and candidate limitations are recorded in `docs/WINDOWS-GLASS-RESEARCH.md`.
