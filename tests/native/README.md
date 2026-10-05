# Native glass GPU regression

Run on Windows with the MSVC x64 Build Tools and Windows SDK installed:

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File tests/native/run-glass-gpu-regression.ps1
```

This builds the current `glass_renderer.hlsl` with the production shader entries,
strictness and optimization flags, then executes them on D3D11 WARP. It creates
no window, does not capture the desktop, and uses only synthetic pixels. Source
and final output use BGRA8; the horizontal blur retains premultiplied colour and
coverage in RGBA16F until the vertical pass normalizes them. The
constant-buffer layout is checked at compile time and against compiled shader
reflection: 6208 bytes, track metadata at byte 2096, and 64 tracks of 64 bytes
starting at byte 2112. The metadata count controls which slots participate;
zero count must ignore a fully populated trailing array.

Checks cover disabled and transparent tracks, straight 0/50/100 progress,
rounded capsule extent, antialias color bounds, a single source-over operation,
track displacement at the thumb shoulder, its flat center, duplicate control
overlap, opaque output, and normalized blur beside missing source coverage.
Three independent slider tracks and real-sized thumbs are rendered together;
every thumb must displace its own track, retain its color/progress and leave
other tracks unchanged when muted. The 64-track case verifies each slot's
colors/progress, the final slot and count truncation. Coincident tracks must
perform exactly one source-over operation per track, in array order; overlapping
controls must still evaluate glass only once. All-transparent arrays are exact
identities, both with and without glass controls. A separate 64-control/64-track
frame verifies that control 63 bends track 63; removing the last control must
eliminate that displacement while leaving the last straight track present.
The capsule oracle uses distance to a line segment and geometric interior and
exterior regions; it does not reimplement the optical equations. Boundary tests
check geometry and color bounds, not pixel equality with a browser's rasterizer.

The negative control runs the actual GPU pipeline with the lens first and an
unrefracted track afterward. It must fail the same outside-bar displacement gate
that the new pipeline passes. A passed negative-control check means the old
ordering was detected; it does not mean that old ordering was accepted.

The report also contains a bounded WARP timing sample for track counts 3 and 64.
Both samples use the same 256x160 source, populated track array and one fixed
control. Only the active track count changes. Two warmup frames precede 12
measured frames; measurement includes draw submission, staging-texture
allocation and synchronous BGRA8 readback. Texture setup and shader compilation
are excluded. These software-renderer timings have no pass threshold and do not
represent production hardware FPS, desktop compositing or a frame-budget claim.

Dispersion checks use neutral black/white line and bitmap-glyph sources. They
measure RGB separation on straight portions of a 20 px curved shoulder, retain
source coordinates in the neutral flat center, reverse color polarity when source contrast
is reversed, keep uniform black/gray/white inputs neutral, and require alpha one.
A pointwise normal-incidence environment-reflection oracle accounts for the
flat face's new color response without allowing shifted glyphs or tracks.
A separate monotone edge measures the area between its red and blue responses
as an equivalent separation in pixels. These are sharp-input optical isolation
checks; production background blur and tint can reduce their visible signal.
The previous coefficient is rejected by the same harness; measurements and
threshold rationale are in
[the paired dispersion report](../../artifacts/material-redesign/qa/glass-dispersion-warp-comparison.md).

The report includes source hashes. Build outputs stay under the ignored Cargo
target directory. This is a shader regression, not evidence of Apple pixel
parity, WebView/Win32 integration, animation timing, capture visibility, or
hardware GPU compatibility. It deliberately exercises a track without an
owning lens for primitive isolation; the production bridge does not accept that
state. Run the actual TieZ fixture separately for those integration properties.

## Full material pipeline

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File tests/native/run-glass-material-pipeline-regression.ps1
```

This separate suite executes both production blur passes with the actual Rust
light/dark tint and opacity values, then enables layered optics (`options.z=1`).
The raw premultiplied scene has a complete GPU-generated mip chain at `t1`;
the blurred/tinted window base is at `t0`. Readback of checkerboard mip levels
1, 3, and 7 verifies that mips contain averaged pixels, rather than merely an
SRV being bound. The runner fails if Rust parameter mappings drift.

The suite covers three control sizes, monochrome glyphs and lines, four source
phases, light/dark modes, maximum and midpoint displayed transparency. It uses
the unlayered background path (`options.z=0` over the same blurred/tinted `t0`)
and a deliberately misbound control texture as negative controls. The unlayered
mutation retains the current optical model; stronger projection can make it
cross the absolute visibility gate. The independent raw source must add at
least three percentage points of visible shoulder area in each paired case.
The misbound input must still fail the absolute gate. Separate shader copies with zero
dispersion and zero offsets isolate sampling from tint and reflections.

Additional checks retain the window base outside controls, require an unchanged
uniform surface relative to the old pipeline, reject the previous variable rim
tint that produced a plastic bevel, check foreground contrast in a defined
safe inset (primary, secondary, muted, links/quick-paste and danger), verify uniform surface filtering, normalize premultiplied mip
coverage without black halos, and preserve track ordering/alpha in the layered
path. Three tracks with distinct progress and colors are additionally exercised
in the same material frame in light/dark modes and maximum/midpoint displayed
transparency. Every thumb must bend its track, reject post-optics ordering,
retain the other tracks when muted, and preserve alpha; transparent arrays and
duplicate controls must leave the corresponding material bitmap unchanged.
Foreground ink is not included in the GPU input; actual WebView layout
and text rendering remain integration checks.

Uniform filtering is checked against a separately compiled constant-LOD oracle:
only the production LOD call is replaced by a fixture-supplied constant in the
reserved `shape.w` test field. All production blur passes, raw mips, RGB offsets,
reflection, coverage, tracks and alpha still execute. Complete rendered bitmaps,
edge ROIs and interior ROIs must agree within one BGRA8 quantization step across
four radii/sizes, light/dark, DPI 1/1.5/2, clarity endpoints/midpoint, and explicit
clear/regular material roles. A float32 GPU probe also checks a fixed numerical
LOD contract table. The rejected depth-dependent clear-rim/frosted-center shader
must fail the same bitmap gate; its actual old profile is preserved in a separate
compiled mutation. At maximum clarity, clear material must match the raw level-0
source path exactly. Identical repeated source patches verify that same-material
controls filter equally, while clear and regular surfaces in one draw remain
distinct. Regular filtering retains the previous minimum fine-detail attenuation
gate, and both roles run foreground contrast and premultiplied-mip halo checks.
The harness is `glass_surface_filter_regression.h`.

The former center-only frost and size-dependent softness requirements are
intentionally removed: they enforced the rejected clear-rim/frosted-center design.
Their historical [body comparison](../../artifacts/material-redesign/qa/body-scatter-experiment-20260922/evaluation.md)
remains evidence about that previous design, not acceptance for uniform glass.
Independent spectral visibility, wrong-source rejection, pressure, contrast,
coverage, alpha and multi-track thresholds are unchanged.

The ordinary isolated suite remains useful for optical math and track
primitives, but its sharp-input dispersion result cannot establish full material
visibility. See [the full-pipeline evaluation](../../artifacts/material-redesign/qa/glass-material-pipeline-evaluation.md)
for the measured improvement, rejected candidate, numerical gates, and remaining
visual limits.

The pressure extension observes actual production `GlassEvaluate` green sample
coordinates in an RGBA32F GPU target, with a separately compiled previous fixed
height cap as its negative control. Complete material frames are measured in a
lower/right shoulder region where neither pressure endpoint has a directional
highlight. Release must restore the bitmap, static pixels must match the old
cap, and coverage/alpha must remain unchanged. Float32 pixel-quad Jacobian spots
supplement the independent CPU analysis rather than claiming a continuous proof.
See [the pressure GPU evaluation](../../artifacts/material-redesign/qa/glass-pressure-gpu-evaluation.md).

## Projection and resize compatibility

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File tests/native/run-glass-projection-regression.ps1
powershell -NoProfile -ExecutionPolicy Bypass -File tests/native/run-glass-resize-regression.ps1
```

Both GPU suites include the shared 6208-byte `State` and verify the current
shader layout. The float32 projection probe uses the reserved `trackMeta.w`
for its test-only wavelength selection; `trackMeta.x` remains the track count.
The resize GPU suite still runs the production blur passes without optical
controls or tracks, including the copied intersection, average raw-color fill,
single tint and premultiplied-alpha negative controls. Its runner also executes
the resize policy and detached swap-chain lifetime checks without a HWND or
desktop capture. Multi-track changes do not replace those regressions.

For reusable offline color/white-body candidate analysis of recorded fixture
videos, see [VIDEO-AUDIT.md](VIDEO-AUDIT.md). That tool is separate from the GPU
regression and does not perform visual acceptance or iOS comparison.

## Redundant Z-order requests

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File tests/native/run-glass-zorder-noop-regression.ps1
```

This runs the production no-op classifier and bounded message gate without
creating/querying HWNDs or capturing pixels. An independent full-order
permutation oracle checks accepted insertion requests. Activation, topmost
promotion/demotion, rewritten requests, reentrant/late messages and a fixed
deadline must not inherit the unchanged-order exception. Three mutated policy
copies must fail the same checks: unconditional withdrawal, ignoring order and
band changes, and completing before the downstream window procedure returns.

The renderer still validates its actual capture scope and geometry separately.
These policy checks and a successful native build do not prove the fast path's
runtime hit rate or eliminate focus/resize flicker; that requires authorized
native visual testing. Actual helper reordering and transitions across other
windows do not enter this no-op path.

## Offscreen frame cost and presentation regressions

```powershell
cargo check --manifest-path src-tauri/Cargo.toml --bin tiez-app
powershell -NoProfile -ExecutionPolicy Bypass -File tests/native/run-glass-frame-regression.ps1
```

The fixture executes production D3D11 methods on the hardware adapter with
synthetic changing backgrounds, 0/8/32/64 overlapping controls, slider tracks,
pressure and 150% DPI. A frozen full-viewport algorithm checks pixel equivalence;
GPU timestamps measure continuous full-pipeline bursts separately from isolated
frames and CPU submission. Gaussian kernel quantization may differ by one BGRA8
step. Hidden Win32 fixtures check the rounded mask at five DPIs. Injected DXGI
backpressure and compositor-fence completion verify retained output, geometry
acknowledgment, coalesced retargets and discarded shutdown notifications.

To composite the production GPU output beneath the real headless UI:

```powershell
$env:TIEZ_GLASS_OFFSCREEN_RENDERER = (Resolve-Path 'src-tauri/target/glass-frame-regression/frames.exe').Path
npx playwright test tests/ui/glass-native-render.spec.ts
```

These checks use fictional clipboard content and do not show a native window,
capture the desktop or control input. They cover deterministic pixels, geometry,
timing and handoff policy; actual WGC/DWM screen presentation remains a separate
integration boundary.
