# Two-source WGC composition probe

This isolated capture test is separate from the single-source executable. It creates exactly three owned windows: a large blue source A, an overlapping orange source B, and a D3D11 / DirectComposition glass output. Both source windows are opaque, square and borderless; no user application, monitor or clipboard is captured. Both probes now use the same original `native/glass_optics.hlsli` module. HLSL is compiled and embedded at build time; the executable does not load shader source from its working directory.

Build without launching:

```powershell
powershell.exe -NoProfile -ExecutionPolicy Bypass -File src-tauri/examples/build-glass-capture-probe.ps1 -ProbeKind multi
```

Output is `artifacts/material-redesign/probe/glass_multi_capture_probe.exe`, with `multi-build-evidence.json`. Launch only within the user's authorized project-test observation scope. Runtime metadata goes to `multi-evidence.jsonl`; each launch replaces that file, so preserve a completed run before starting another.

## What the shader samples

Each source has its own `CreateForWindow` capture item, frame pool, session and retained GPU texture. Every displaced R/G/B sampling position is converted to screen coordinates. The shader tests that position against both source rectangles, samples the relevant texture coordinates, and resolves overlap in the sources' observed Z order. It does not choose one background for the entire lens and does not shrink either complete source into the lens. Window enumeration compares only the two already-known source handles; it does not read or capture other windows.

F6 exchanges the actual source-window Z order and keeps the output above them. Source A can fully cover B, so continued B frames while its window is covered test whether each WGC stream remains independent. The same per-pixel lookup works across the A/B boundary inside the lens. Keep the output inside source A's rectangle: no genuine background exists outside the owned fixtures; the shader deliberately uses a synthetic neutral fallback there.

The shader uses premultiplied alpha composition, matching OBS's WGC rendering convention, but initial fixtures are opaque so this probe does not claim arbitrary transparency support. OBS's WGC output uses `ONE / INVSRCALPHA`, and the standard draw effect returns sampled color directly.[OBS capture rendering](https://github.com/obsproject/obs-studio/blob/master/libobs-winrt/winrt-capture.cpp), [OBS draw shader](https://github.com/obsproject/obs-studio/blob/master/libobs/data/default.effect)

## Controls and verification

| Key | Action |
| --- | --- |
| F2 / R | Toggle refraction. |
| F3 / C | Toggle dispersion. |
| F4 | Pause / resume both source animations at their current common animation time. |
| F5 / B | Toggle the border-required property for both capture sessions; log each result independently. |
| F6 | Exchange source A / B Z order. |
| F7 | Switch between one large panel and seven Control Center geometry fixtures. |
| F8 | In controls layout, compare refraction on/off by lossless GPU readback using exactly the same retained source textures. |
| Escape | Close all three windows and both capture sessions. |

1. Confirm both first-frame events, dimensions, source ownership and all three affinities equal zero. Confirm the front output appears in independent window capture.
2. Freeze with F4. Turn refraction off with F2 and compare the lens against the actual source seam, grids and text. Move only the output; source scales should remain 1 and output origin should follow physical movement.
3. Use F6. The area that previously sampled orange B should now correctly show blue A when A covers B; switch back and verify B returns. Toggle refraction with the same frozen fixtures to confirm the seam is sampled after displacement.
4. Resume with F4 and verify both source frame counters and QPC timestamps advance, including when A fully covers B. Observe the output and compare `source_timestamp_skew_ms`; independent sessions are not an atomic snapshot of a single compositor frame.
5. Freeze again and leave windows unchanged. After pending frames settle, `presented` should stop advancing while `unchanged_present_skips` grows. The timer still polls captures; this is a limited improvement over repeatedly submitting the same image, not a zero-CPU or power-consumption claim.
6. Observe the source capture indicators separately. F5 records setters and readback, but no borderless permission request is made. Finally close and verify `closed` / `shutdown` plus absence of all three probe windows.

The rendering loop presents only after a new source frame, a changed source/output rectangle, changed Z order, changed optical switches or an output resize. It retains previous textures and compositor content otherwise. Runtime progress records each source's frames, drained frames, source timestamp, frame age, physical bounds, texture dimensions, scale, output origin, output dimensions, three affinities and skipped presents. No pixels are written into the log.

## Capture border and production boundary

Two WGC sessions may produce two capture indicators. Affinity zero only controls whether the windows are excluded from external capture; it does not disable those indicators. Microsoft requires consent for borderless capture, and `IsBorderRequired(false)` may succeed while being ignored. Other capture sessions may still require a border for the same source. This probe retains the default at startup and never calls `GraphicsCaptureAccess.RequestAccessAsync` or changes privacy settings.[Microsoft border semantics](https://learn.microsoft.com/en-us/uwp/api/windows.graphics.capture.graphicscapturesession.isborderrequired?view=winrt-26100)

There is no atomic multi-session capture guarantee in the reviewed API. `SystemRelativeTime` is the compositor's QPC timestamp for an individual frame, so two latest textures may represent different instants. The probe measures their skew; deterministic spatial comparison uses paused fixtures.[Frame timestamps](https://learn.microsoft.com/en-us/uwp/api/windows.graphics.capture.direct3d11captureframe.systemrelativetime?view=winrt-26100)

Passing this test establishes only the owned, opaque-window composition path. Microsoft capture-sample maintainers distinguish ordinary alpha, which may be preserved, from acrylic / window backdrop effects that sample content behind a window and cannot be retained as independent capture content. Consequently, replaying arbitrary HWND textures by Z order is not established as a faithful reconstruction of the real DWM desktop.[Alpha discussion](https://github.com/microsoft/Windows.UI.Composition-Win32-Samples/issues/42#issuecomment-537376661), [Backdrop limitation](https://github.com/robmikh/Win32CaptureSample/issues/41#issuecomment-1024991777)

Further unresolved production concerns include native shadows, transparent window dependencies, protected / uncapturable windows, transient popups outside their owner, changing HWND lifetimes, source geometry versus frame timing, HDR / color spaces, mixed DPI / monitor transitions and capture cost for many sources. This probe neither imports those sources nor declares those problems solved. The user's selected Control Center material direction still requires calibrated appearance, foreground contrast, interaction and comparison against Apple references.

## Shared optics verification, 2026-09-14

The updated executable was built with strict `fxc /Ges /WX` compilation and MSVC. `multi-build-evidence.json` records the executable, source, optics and bytecode hashes. F7 rendered circles, rounded tiles, a horizontal capsule and a vertical slider with actual displacement of the owned backgrounds. Computer Use images in `qa/native-controls-refraction-*.jpg` are lossy JPEG visual evidence; their block compression must not be mistaken for shader errors or used to prove pixel identity.

F8 directly compared the rendered BGRA swap-chain buffers before presentation, using the same two retained textures for both renders. All seven 3-by-3 center patches had maximum RGBA difference **0**. Across the output, **22,278 pixels** changed by more than 10 levels and the maximum difference was **241**, confirming an active optical effect alongside unchanged centers. This is a point-sample integration regression, not a claim that every interior pixel or the Apple appearance has been proven identical. The sources and output retained affinity 0. The run closed normally; `controls-gpu-evidence.jsonl` preserves the result and shutdown.
