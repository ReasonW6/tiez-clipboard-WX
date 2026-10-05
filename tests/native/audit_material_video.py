#!/usr/bin/env python3
"""Offline diagnostic candidates from recorded pixels; never a visual acceptance gate."""
from __future__ import annotations

import argparse
from collections import Counter
import hashlib
import json
import math
from pathlib import Path
import shutil
import subprocess
import sys
from typing import Callable


COLOR_RULES = {
    "magenta_strict": ((220, 255), (0, 40), (220, 255)),
    "magenta_broad": ((160, 255), (0, 110), (160, 255)),
    "cyan_strict": ((0, 40), (220, 255), (220, 255)),
    "cyan_broad": ((0, 110), (160, 255), (160, 255)),
    "near_black": ((0, 24), (0, 24), (0, 24)),
}
WHITE_RULE = {"minimum_p05": 225, "maximum_p95_minus_p05": 8, "maximum_mean_channel_spread": 20}


def finite(value: object, label: str) -> float:
    number = float(value)
    if not math.isfinite(number):
        raise ValueError(f"{label} must be finite")
    return number


def rectangle(value: str) -> tuple[int, int, int, int]:
    try:
        parts = tuple(int(part) for part in value.split(","))
        if len(parts) != 4 or min(parts[:2]) < 0 or min(parts[2:]) <= 0:
            raise ValueError()
        return parts
    except ValueError as exc:
        raise argparse.ArgumentTypeError("ROI must be x,y,width,height with positive dimensions") from exc


def check_rectangle(rect: tuple[int, ...], width: int, height: int) -> None:
    x, y, w, h = rect
    if x + w > width or y + h > height:
        raise ValueError(f"ROI {rect} is outside {width}x{height}; it will not be clipped")


def command_json(command: list[str]) -> dict:
    return json.loads(subprocess.check_output(command, stderr=subprocess.PIPE))


def file_sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def probe_video(ffprobe: str, video: Path) -> dict:
    result = command_json([ffprobe, "-v", "error", "-select_streams", "v:0", "-show_streams", "-show_format", "-show_frames",
                           "-show_entries", "stream=width,height:format=duration:frame=best_effort_timestamp_time,width,height", "-of", "json", str(video)])
    stream = result["streams"][0]
    width, height = stream["width"], stream["height"]
    frames = result.get("frames", [])
    if not frames:
        raise ValueError("The video has no decoded frames")
    timestamps = []
    for frame in frames:
        if (frame.get("width", width), frame.get("height", height)) != (width, height):
            raise ValueError("Resolution changes inside the video are not supported")
        pts = finite(frame["best_effort_timestamp_time"], "Frame PTS")
        if timestamps and pts <= timestamps[-1]:
            raise ValueError("Duplicate or decreasing PTS cannot be timed reliably by this audit")
        timestamps.append(pts)
    return {"width": width, "height": height, "pts": timestamps,
            "duration": finite(result["format"]["duration"], "Duration")}


def load_events(path: Path | None, anchor_ms: float | None, first_pts: float, last_pts: float) -> list[dict]:
    if path is None:
        return []
    events = json.loads(path.read_text(encoding="utf-8-sig"))
    if not isinstance(events, list) or not events:
        raise ValueError("The events file must be a nonempty JSON array")
    output = []
    labels = set()
    for event in events:
        label = event["label"]
        if not isinstance(label, str) or not label or label in labels:
            raise ValueError("Each event needs a unique nonempty label")
        labels.add(label)
        if "start_ms" in event or "end_ms" in event:
            if anchor_ms is None or "start" in event or "end" in event:
                raise ValueError("Wall-clock events require --anchor-ms and cannot also specify PTS")
            start = (finite(event["start_ms"], "Event start_ms") - anchor_ms) / 1000 + first_pts
            end = (finite(event["end_ms"], "Event end_ms") - anchor_ms) / 1000 + first_pts
        else:
            start, end = finite(event["start"], "Event start"), finite(event["end"], "Event end")
        if not first_pts <= start <= end <= last_pts:
            raise ValueError(f"Event {label!r} lies outside decoded PTS coverage or has reversed times")
        output.append({"label": label, "start_pts": start, "end_pts": end,
                       "event_duration_ms": (end - start) * 1000})
    return output


def consecutive_groups(rows: list[dict], predicate: Callable[[dict], bool]) -> list[dict]:
    """Only observed neighboring non-hits can prove that a group is transient."""
    groups = []
    i = 0
    while i < len(rows):
        if not predicate(rows[i]):
            i += 1
            continue
        start = end = i
        while end + 1 < len(rows) and rows[end + 1]["frame"] == rows[end]["frame"] + 1 and predicate(rows[end + 1]):
            end += 1
        previous = rows[start - 1] if start and rows[start - 1]["frame"] + 1 == rows[start]["frame"] else None
        following = rows[end + 1] if end + 1 < len(rows) and rows[end]["frame"] + 1 == rows[end + 1]["frame"] else None
        groups.append({"first_frame": rows[start]["frame"], "last_frame": rows[end]["frame"],
                       "first_pts": rows[start]["pts"], "last_pts": rows[end]["pts"], "frames": end - start + 1,
                       "observed_end_pts": following["pts"] if following else None,
                       "observed_interval_ms": (following["pts"] - rows[start]["pts"]) * 1000 if following else None,
                       "left_censored": previous is None, "right_censored": following is None,
                       "bounded_by_non_hits": previous is not None and following is not None})
        i = end + 1
    return groups


def crop_rgb(raw: bytes, width: int, rect: tuple[int, ...]) -> bytes:
    x, y, w, h = rect
    return b"".join(raw[((y + row) * width + x) * 3:((y + row) * width + x + w) * 3] for row in range(h))


def color_counts(rgb: bytes) -> dict:
    channels = (rgb[0::3], rgb[1::3], rgb[2::3])
    result = {}
    for name, ranges in COLOR_RULES.items():
        masks = [int.from_bytes(channel.translate(bytes(int(low <= value <= high) for value in range(256))), "little")
                 for channel, (low, high) in zip(channels, ranges)]
        result[name] = (masks[0] & masks[1] & masks[2]).bit_count()
    return result


def white_candidate(rgb: bytes) -> dict:
    statistics = []
    for channel in (rgb[0::3], rgb[1::3], rgb[2::3]):
        histogram = Counter(channel)
        count = len(channel)
        def percentile(fraction: float) -> int:
            target, seen = max(1, math.ceil(count * fraction)), 0
            for value in range(256):
                seen += histogram[value]
                if seen >= target:
                    return value
            raise AssertionError("Missing channel pixels")
        statistics.append({"p05": percentile(.05), "p95": percentile(.95), "mean": sum(channel) / count})
    means = [channel["mean"] for channel in statistics]
    candidate = all(channel["p05"] >= WHITE_RULE["minimum_p05"] and channel["p95"] - channel["p05"] <= WHITE_RULE["maximum_p95_minus_p05"] for channel in statistics)
    candidate &= max(means) - min(means) <= WHITE_RULE["maximum_mean_channel_spread"]
    return {"candidate": candidate, "rgb_statistics": statistics}


def frame_selection(indices: list[int]) -> str:
    spans = []
    for index in indices:
        if not spans or index != spans[-1][1] + 1:
            spans.append([index, index])
        else:
            spans[-1][1] = index
    return "+".join(f"between(n\\,{start}\\,{end})" for start, end in spans)


def read_exact(stream, count: int) -> bytes:
    output = bytearray()
    while len(output) < count:
        part = stream.read(count - len(output))
        if not part:
            raise ValueError(f"Truncated raw frame: expected {count}, received {len(output)} bytes")
        output.extend(part)
    return bytes(output)


def write_report(path: Path, summary: dict) -> None:
    lines = ["# Recorded material diagnostic candidates", "", "This report does not determine iOS fidelity, absence of flicker, renderer correctness, or the cause of a candidate.", "",
             f"Video SHA-256: `{summary['video_sha256']}`.", "",
             f"Scanned {summary['scanned_frames']} of {summary['total_frames']} original-resolution frames; analysis ROI: {summary['roi']}. No resizing or frame-rate conversion was used.", "",
             "| Detector | Maximum ROI pixels | Candidate hit frames | Minimum candidate pixels |", "| --- | ---: | ---: | ---: |"]
    for name, item in summary["colors"].items():
        lines.append(f"| {name} | {item['maximum_pixels']} | {item['hit_frames']} | {item['candidate_threshold_pixels']} |")
    lines += ["", "| Event | Actual scanned PTS | Frames | Supplied event interval |", "| --- | --- | ---: | --- |"]
    for event in summary["events"]:
        supplied = f"{event['start_pts']:.6f}–{event['end_pts']:.6f} s" if "start_pts" in event else "whole clip"
        lines.append(f"| {event['label']} | {event['first_pts']:.6f}–{event['last_pts']:.6f} s | {event['frames']} | {supplied} |")
    lines += ["", "Color matches identify candidate pixels, not their owning window. Broad cyan/magenta can match legitimate fixture colors. Small near-black text is not automatically a black block.", "",
              "White candidates require bright, nearly uniform RGB in explicitly selected empty-body ROIs. A white source, control or intentionally opaque material can also match. Only groups with observed non-white neighbors on both sides are bounded transient candidates.", ""]
    for name, item in summary["white"].items():
        lines.append(f"- `{name}`: {item['hit_frames']} candidate frames, {len(item['groups'])} groups; {sum(g['bounded_by_non_hits'] for g in item['groups'])} bounded on both sides.")
        for group in item["groups"][:8]:
            end = f"{group['observed_end_pts']:.6f}" if group["observed_end_pts"] is not None else "unobserved"
            lines.append(f"  - Frame {group['first_frame']}–{group['last_frame']}; starts {group['first_pts']:.6f} s, observed non-hit end {end}; bounded on both sides: {group['bounded_by_non_hits']}.")
        if len(item["groups"]) > 8:
            lines.append("  - Additional groups are retained in summary.json.")
    lines += ["", f"Saved {len(summary['witnesses'])} full-frame decoded PNG candidates. {summary['witnesses_omitted_by_limit']} requested candidate frames were omitted by the witness limit; their measurements remain in frames.jsonl.", "",
              "See summary.json for exact PTS coverage, event intervals, censoring, thresholds, tool versions, detector ROIs and witness hashes. A group ending at a scan boundary has no inferred end time. Wall-clock alignment, when supplied, is an external anchor rather than video-proven synchronization.", ""]
    path.write_text("\n".join(lines), encoding="utf-8")


def audit(args: argparse.Namespace) -> dict:
    video, output = args.video.resolve(), args.output.resolve()
    if not video.is_file():
        raise ValueError("Input video does not exist")
    if output.exists() and any(output.iterdir()):
        raise ValueError("Output directory must be new or empty; existing evidence is not overwritten")
    ffmpeg = shutil.which(args.ffmpeg) or args.ffmpeg
    ffprobe = args.ffprobe or str(Path(ffmpeg).with_name("ffprobe.exe" if Path(ffmpeg).suffix.lower() == ".exe" else "ffprobe"))
    probe = probe_video(ffprobe, video)
    width, height, timestamps = probe["width"], probe["height"], probe["pts"]
    roi = args.roi or (0, 0, width, height)
    check_rectangle(roi, width, height)
    white_rois = {}
    for value in args.white_roi:
        name, separator, coordinates = value.partition(":")
        if not separator or not name or name in white_rois:
            raise ValueError("--white-roi must have a unique NAME:x,y,width,height")
        rect = rectangle(coordinates)
        check_rectangle(rect, width, height)
        white_rois[name] = rect
    padding = finite(args.padding, "Padding")
    if padding < 0 or args.max_witnesses < 0 or args.black_candidate_pixels < 1:
        raise ValueError("Padding/witness limit must be nonnegative and black candidate pixels positive")
    anchor = finite(args.anchor_ms, "Anchor") if args.anchor_ms is not None else None
    events = load_events(args.events, anchor, timestamps[0], timestamps[-1])
    selected = []
    for index, pts in enumerate(timestamps):
        # A VFR frame can start before the requested interval and still be
        # displayed inside it. Include that overlap without inventing an FPS.
        following_pts = timestamps[index + 1] if index + 1 < len(timestamps) else None
        labels = [event["label"] for event in events
                  if pts <= event["end_pts"] + padding
                  and (following_pts is None or following_pts > event["start_pts"] - padding)]
        selected.append({"frame": index, "pts": pts, "next_video_pts": following_pts, "events": labels})
    selected = [row for row in selected if not events or row["events"]]
    if not selected:
        raise ValueError("No decoded frames fall inside the requested intervals")
    output.mkdir(parents=True, exist_ok=True)
    command = [ffmpeg, "-v", "error", "-noautorotate", "-i", str(video), "-map", "0:v:0", "-vf", "select=" + frame_selection([row["frame"] for row in selected]),
               "-fps_mode", "passthrough", "-pix_fmt", "rgb24", "-f", "rawvideo", "pipe:1"]
    rows = []
    with (output / "decode-stderr.txt").open("wb") as error_stream:
        process = subprocess.Popen(command, stdout=subprocess.PIPE, stderr=error_stream)
        try:
            for spec in selected:
                raw = read_exact(process.stdout, width * height * 3)
                rows.append({**spec, "colors": color_counts(crop_rgb(raw, width, roi)),
                             "white": {name: white_candidate(crop_rgb(raw, width, rect)) for name, rect in white_rois.items()}})
            if process.stdout.read(1):
                raise ValueError("Decoder returned more frames than the probed selection")
            if process.wait() != 0:
                raise ValueError("FFmpeg failed; inspect decode-stderr.txt")
        finally:
            if process.poll() is None:
                process.kill()
                process.wait()
            process.stdout.close()
    with (output / "frames.jsonl").open("w", encoding="utf-8") as stream:
        for row in rows:
            stream.write(json.dumps(row) + "\n")
    colors = {}
    candidates = {}
    context_candidates = []
    available = {row["frame"] for row in rows}
    def candidate(index: int, reason: str) -> None:
        if index in available:
            reasons = candidates.setdefault(index, [])
            if reason not in reasons:
                reasons.append(reason)
    for name in COLOR_RULES:
        peak = max(rows, key=lambda row: row["colors"][name])
        threshold = args.black_candidate_pixels if name == "near_black" else 1
        groups = consecutive_groups(rows, lambda row: row["colors"][name] >= threshold)
        colors[name] = {"maximum_pixels": peak["colors"][name], "maximum_fraction": peak["colors"][name] / (roi[2] * roi[3]),
                        "peak_frame": peak["frame"], "peak_pts": peak["pts"], "hit_frames": sum(row["colors"][name] >= threshold for row in rows),
                        "candidate_threshold_pixels": threshold, "groups": groups}
        if groups:
            candidate(peak["frame"], f"{name}:maximum")
            for group in groups:
                for index in [group["first_frame"] - 1, group["first_frame"], group["last_frame"], group["last_frame"] + 1]:
                    context_candidates.append((index, f"{name}:group_boundary"))
    white = {}
    for name, rect in white_rois.items():
        groups = consecutive_groups(rows, lambda row: row["white"][name]["candidate"])
        white[name] = {"roi": rect, "hit_frames": sum(row["white"][name]["candidate"] for row in rows), "groups": groups}
        hits = [row for row in rows if row["white"][name]["candidate"]]
        if hits:
            peak = max(hits, key=lambda row: sum(channel["mean"] for channel in row["white"][name]["rgb_statistics"]))
            candidate(peak["frame"], f"white:{name}:brightest_candidate")
        for group in groups:
            for index in [group["first_frame"] - 1, group["first_frame"], group["last_frame"], group["last_frame"] + 1]:
                context_candidates.append((index, f"white:{name}:group_boundary"))
    coverage = []
    for event in events or [{"label": "whole_clip"}]:
        subset = [row for row in rows if not events or event["label"] in row["events"]]
        if not subset:
            raise ValueError(f"No frames cover event {event['label']}")
        coverage.append({**event, "first_frame": subset[0]["frame"], "last_frame": subset[-1]["frame"],
                         "first_pts": subset[0]["pts"], "last_pts": subset[-1]["pts"], "frames": len(subset)})
        context_candidates.append((subset[0]["frame"], "coverage_start"))
        context_candidates.append((subset[-1]["frame"], "coverage_end"))
    # Add every detector's representative before any group-boundary context.
    for index, reason in context_candidates:
        candidate(index, reason)
    requested = list(candidates)
    chosen = sorted(requested[:args.max_witnesses])
    witnesses = []
    if chosen:
        subprocess.run([ffmpeg, "-v", "error", "-noautorotate", "-i", str(video), "-map", "0:v:0", "-vf", "select=" + frame_selection(chosen),
                        "-fps_mode", "passthrough", "-pix_fmt", "rgb24", str(output / "witness-%04d.png")], check=True, stderr=subprocess.PIPE)
        for number, index in enumerate(chosen, 1):
            path = output / f"witness-{number:04d}.png"
            if not path.is_file():
                raise ValueError("FFmpeg did not produce all requested witnesses")
            witnesses.append({"file": path.name, "frame": index, "pts": timestamps[index], "reasons": candidates[index],
                              "sha256": file_sha256(path)})
    summary = {"schema_version": 1, "scope": "Diagnostic pixel candidates; no automatic acceptance or root-cause claim",
               "audit_source_sha256": file_sha256(Path(__file__)),
               "video": str(video), "video_sha256": file_sha256(video), "dimensions": [width, height],
               "roi": roi, "duration": probe["duration"], "first_video_pts": timestamps[0], "last_video_pts": timestamps[-1],
               "orientation_policy": "Encoded pixel orientation; display-matrix rotation is disabled for measurements and witnesses",
               "selection_policy": "Frame display intervals overlap the padded event interval; a VFR predecessor may start before that interval",
               "total_frames": len(timestamps), "scanned_frames": len(rows), "events": coverage, "event_padding_seconds": padding,
               "supplied_anchor_ms": anchor, "color_thresholds": COLOR_RULES, "white_thresholds": WHITE_RULE,
               "colors": colors, "white": white, "witnesses": witnesses, "witnesses_omitted_by_limit": len(requested) - len(chosen),
               "omitted_witnesses": [{"frame": index, "pts": timestamps[index], "reasons": candidates[index]} for index in requested[args.max_witnesses:]],
               "tool_versions": {"python": sys.version.split()[0], "ffmpeg": subprocess.check_output([ffmpeg, "-version"], text=True).splitlines()[0],
                                 "ffprobe": subprocess.check_output([ffprobe, "-version"], text=True).splitlines()[0]}}
    (output / "summary.json").write_text(json.dumps(summary, indent=2), encoding="utf-8")
    write_report(output / "report.md", summary)
    return summary


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("video", type=Path)
    parser.add_argument("--output", type=Path, required=True, help="New or empty evidence directory")
    parser.add_argument("--ffmpeg", default="ffmpeg")
    parser.add_argument("--ffprobe", help="Defaults to ffprobe beside ffmpeg")
    parser.add_argument("--roi", type=rectangle, help="Color scan x,y,w,h inside the decoded video; default full frame")
    parser.add_argument("--white-roi", action="append", default=[], metavar="NAME:x,y,w,h", help="Repeat for empty-body regions; independent of the color ROI")
    parser.add_argument("--events", type=Path, help="JSON event array; omit to scan every frame")
    parser.add_argument("--anchor-ms", type=float, help="Externally supplied epoch milliseconds of the first decoded frame")
    parser.add_argument("--padding", type=float, default=.5, help="Seconds before/after each event interval")
    parser.add_argument("--max-witnesses", type=int, default=32)
    parser.add_argument("--black-candidate-pixels", type=int, default=256)
    args = parser.parse_args()
    try:
        result = audit(args)
    except (ValueError, argparse.ArgumentTypeError, KeyError, IndexError, OSError, subprocess.SubprocessError) as error:
        print(f"Audit failed: {error}", file=sys.stderr)
        return 1
    print(f"Scanned {result['scanned_frames']}/{result['total_frames']} frames; saved {len(result['witnesses'])} diagnostic witnesses in {args.output}.")
    print("No visual acceptance decision is implied; inspect report.md and the original decoded witnesses.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
