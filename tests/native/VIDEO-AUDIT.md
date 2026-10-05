# Offline recorded-material diagnostics

`audit_material_video.py` uses Python's standard library and an already installed
FFmpeg/ffprobe pair. It reads a supplied video and writes evidence into a **new or
empty output directory**. It does not capture the screen, operate UI, install
dependencies or change application state.

It detects diagnostic color matches and nearly uniform white-body candidates.
It is not an iOS comparison, star detector, motion-quality test or automatic
acceptance gate. Existing reports are never overwritten.

## Run

```powershell
python -X utf8 tests/native/audit_material_video.py path/to/recording.mp4 `
  --ffmpeg D:/Environment_variables/ffmpeg/bin/ffmpeg.exe `
  --output artifacts/material-redesign/qa/example-audit `
  --roi 0,0,800,640 `
  --white-roi left-body:80,540,160,30 `
  --white-roi right-body:380,540,160,30 `
  --events path/to/events.json --padding 0.5
```

The example ROI values must be replaced or omitted for a different recording.
All coordinates are **pixels inside the encoded video**, not desktop screen
coordinates. Display-matrix auto-rotation is disabled consistently for scanning
and PNG extraction. The default color ROI is the full frame; repeated
`--white-roi NAME:x,y,width,height` rectangles are independently measured in
full-frame coordinates. Select empty material body regions, away from text,
controls, window corners and intentional white source content. Two named body
regions can cover the expected locations before and after a move, but the tool
cannot determine which region belongs to the active window.

Omit `--events` to scan **every decoded frame**. With events, only frame display
intervals overlapping each padded event window are scanned. A held VFR frame
starting before the requested window is included when it is still displayed
inside it. FFmpeg decodes in order with `-fps_mode passthrough`; per-frame PTS is
probed and selected frame counts are checked. No downsampling or FPS conversion
is applied. Duplicate/decreasing/missing PTS and resolution changes fail with an
explicit error rather than producing a misleading timing report.

Events can use absolute container PTS seconds:

```json
[
  {"label": "move-right", "start": 12.25, "end": 12.42},
  {"label": "narrow", "start": 25.50, "end": 25.70}
]
```

Alternatively, events can use `start_ms` and `end_ms` epoch milliseconds, with
`--anchor-ms` specifying the externally established wall-clock time of the first
decoded frame. The tool adds the video's first PTS when converting this anchor.
It never automatically treats file CreationTime as exact synchronization.
If file timing is the only available anchor, record that uncertainty when
interpreting the resulting candidate times. Event labels must be unique and
events must fit within decoded PTS coverage; empty or invalid event files fail.

## Outputs and interpretation

- `frames.jsonl`: every scanned frame's original index, PTS, color counts and
  named white-ROI RGB statistics.
- `summary.json`: input SHA-256, dimensions, ROIs, exact scanned coverage,
  supplied event times, thresholds, candidate groups, tool versions, witness
  reasons/hashes, and any witness frames omitted by the cap.
- `report.md`: a compact candidate summary with explicit limitations.
- `witness-0001.png`, etc.: full-frame RGB24 PNG decodes at original encoded
  dimensions. These are not annotated or recolored. Match names to frame/PTS
  using the summary.
- `decode-stderr.txt`: FFmpeg errors, if any. A failed run can leave partial
  measurements; only a successful exit with a summary establishes that the
  requested scan completed, and it still does not establish visual acceptance.

Strict and broad magenta/cyan predicates are reported separately. Broad cyan
can match the ordinary teal test backdrop, and any colored application content
can produce legitimate matches. No matched pixel is automatically assigned to
the helper/cover window. The default black candidate threshold is 256 near-black
pixels (`--black-candidate-pixels`); the maximum count is reported even below
that threshold. This is an area screen, not a connected-block classifier, and
dark themes or text can legitimately match it.

White-body candidates require RGB fifth percentiles at least 225, each channel's
95th–5th percentile spread at most 8, and mean inter-channel spread at most 20.
The purpose is to nominate nearly uniform bright regions for inspection. A
uniform gray/red region is not white; a white backdrop or intentionally opaque
surface can still match. Groups are called bounded transient candidates only
when neighboring non-hit frames were observed on both sides. Groups cut by a
scan boundary are explicitly censored. Their missing end is not estimated from
an assumed frame rate.

`--max-witnesses` defaults to 32. Every detector's maximum/representative is
considered before group-boundary context. A very low cap can still omit a
detector's evidence; omitted frame indices and reasons are retained explicitly.
Set the cap to 0 to save measurements only. Human review of candidates, source
identity, expected material state and any foreground-occlusion confounds remains
necessary. The tool does not infer “no flicker,” freeze-free rendering or Apple
pixel equivalence from zero color candidates.

## Verify the tool

```powershell
$env:FFMPEG_BINARY = 'D:/Environment_variables/ffmpeg/bin/ffmpeg.exe'
python -X utf8 tests/native/test_audit_material_video.py
```

The standard-library tests generate disposable 64 × 48 PPM frames, encode a
compressed VFR clip with the installed FFmpeg, and clean their temporary output.
They verify compressed magenta/cyan and black positives, ROI exclusions, textured
and uniform-color white negatives, bounded/censored timing, a held VFR frame
overlapping an event start, actual 90-degree display-matrix metadata, coded-pixel
ROI orientation, and representative preservation under the witness cap. If
FFmpeg is unavailable, the compressed-video test is explicitly skipped; such a
run is not complete integration validation.
