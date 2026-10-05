"""Summarize the bounded native scope fixture and export original video frames.

State recovery and visual comparison remain separate evidence. This does not
certify iOS parity or uninterrupted material during capture reconstruction.
"""
import argparse
import json
import re
import subprocess
from pathlib import Path

import numpy as np
from PIL import Image
from audit_material_video import probe_video, read_exact


def audit(run_path, events_path, video_path, output, ffmpeg):
    text = run_path.read_text(encoding="utf-8-sig")
    decoder = json.JSONDecoder()
    steps = [decoder.raw_decode(line.split("Material QA autorun ", 1)[1])[0]
             for line in text.splitlines() if "Material QA autorun " in line]
    statuses = [json.loads(line) for line in events_path.read_text(encoding="utf-8-sig").splitlines() if line.strip()]
    destroyed = {int(pid): int(time) for pid, time in re.findall(r"Material QA blocker_closed pid=(\d+) time_ms=(\d+)", text)}
    rounds = []
    for step in steps:
        if step["event"] != "scope_blocker_visible":
            continue
        detail = step["detail"]
        closed = destroyed.get(detail["childPid"])
        if closed is None:
            closed = next((item["timeMs"] for item in steps if item["event"] == "scope_blocker_closed"
                           and item["detail"]["round"] == detail["round"]), None)
        ready = next((item["timeMs"] for item in statuses if closed is not None and item["timeMs"] >= closed
                      and item["payload"]["state"] == "ready"), None)
        rounds.append({"round": detail["round"], "pid": detail["childPid"], "closedTimeMs": closed,
                       "readyTimeMs": ready, "recoveryMs": None if ready is None else ready - closed,
                       "closeClock": "WM_DESTROY" if detail["childPid"] in destroyed else "parent process-exit observation"})
    output.mkdir(parents=True, exist_ok=True)
    ffprobe = str(Path(ffmpeg).with_name("ffprobe.exe" if Path(ffmpeg).suffix.lower() == ".exe" else "ffprobe"))
    info = probe_video(ffprobe, video_path)
    if (info["height"], info["width"]) != (680, 850):
        raise RuntimeError("Expected the authorized 850x680 fixture recording")
    video = subprocess.Popen([ffmpeg, "-v", "error", "-i", str(video_path), "-map", "0:v:0",
                              "-vsync", "0", "-f", "rawvideo", "-pix_fmt", "rgb24", "-"], stdout=subprocess.PIPE)
    frames = []
    segments = []
    active = None
    index = 0
    reference = None
    last = None
    for pts in info["pts"]:
        frame = np.frombuffer(read_exact(video.stdout, 680 * 850 * 3), dtype=np.uint8).reshape((680, 850, 3))
        # The native child paints one opaque 40x40 tile inside this fixed ROI.
        tile = frame[119:151, 174:206].astype(np.int16)
        present = float(np.mean(np.max(np.abs(tile - [188, 66, 156]), axis=2) < 12)) > .8
        if present and active is None:
            active = {"startPts": pts, "startFrame": index}
        if not present and active is not None:
            active.update(endPts=pts, endFrame=index)
            segments.append(active)
            active = None
        if reference is None and pts >= 3:
            reference = frame.copy()
            Image.fromarray(frame).save(output / "initial.png")
            frames.append({"file": "initial.png", "pts": pts, "frame": index})
        for number, segment in enumerate(segments):
            if "evidence" not in segment and pts >= segment["endPts"] + 1:
                # A topmost/focus transition may occlude the tile before the
                # child closes. This is a visibility landmark, not recovery.
                filename = f"after-tile-hidden-{number + 1}.png"
                Image.fromarray(frame).save(output / filename)
                segment["evidence"] = {"file": filename, "pts": pts, "frame": index}
        last = frame
        last_pts = pts
        index += 1
    if video.stdout.read(1) or video.wait() != 0:
        raise RuntimeError("Decoded frame count or ffmpeg exit disagrees with ffprobe")
    if last is None:
        raise RuntimeError("Recording has no frames")
    Image.fromarray(last).save(output / "final.png")
    frames.append({"file": "final.png", "pts": last_pts, "frame": index - 1})
    # These differences are useful only when the backdrop is explicitly fixed.
    roi = np.s_[41:574, 58:483]
    difference = np.abs(reference[roi].astype(np.int16) - last[roi].astype(np.int16))
    fixed = any(item["event"] == "armed" and "disabled" in item["detail"].get("backgroundAnimation", "") for item in steps)
    report = {"run": str(run_path), "events": str(events_path), "video": str(video_path),
              "sequenceFinished": any(item["event"] == "sequence_finished" for item in steps),
              "aborts": [item for item in steps if item["event"] == "aborted"],
              "failedStatusCount": sum(item["payload"]["state"] == "failed" for item in statuses),
              "rounds": rounds, "videoBlockerSegments": segments, "frames": frames,
              "backgroundFixed": fixed, "initialFinalMeanDifference255": float(difference.mean()) if fixed else None,
              "initialFinalP99Difference255": float(np.percentile(difference, 99)) if fixed else None,
              "visualPass": None, "note": "Review original frames. Tile disappearance can mean occlusion rather than window destruction; it is not a native recovery timestamp. State alone does not prove optics or iOS fidelity."}
    (output / "audit.json").write_text(json.dumps(report, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
    print(json.dumps({key: report[key] for key in ["sequenceFinished", "failedStatusCount", "rounds", "backgroundFixed", "initialFinalMeanDifference255"]}, ensure_ascii=False))


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    for name in ["run", "events", "video", "out"]:
        parser.add_argument(f"--{name}", type=Path, required=True)
    parser.add_argument("--ffmpeg", default="ffmpeg")
    args = parser.parse_args()
    audit(args.run, args.events, args.video, args.out, args.ffmpeg)
