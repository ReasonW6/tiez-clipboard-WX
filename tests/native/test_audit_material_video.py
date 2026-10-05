"""Standard-library checks for evidence timing, detector negatives and a VFR clip."""
import argparse
import importlib.util
import json
import os
from pathlib import Path
import shutil
import struct
import subprocess
import sys
import tempfile
import unittest

sys.dont_write_bytecode = True

SPEC = importlib.util.spec_from_file_location("audit_material_video", Path(__file__).with_name("audit_material_video.py"))
audit = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(audit)


class PixelAndTimingTests(unittest.TestCase):
    def test_uniform_color_negative_controls(self):
        for color in [(208, 208, 208), (255, 0, 0), (0, 255, 255), (255, 0, 255)]:
            with self.subTest(color=color):
                self.assertFalse(audit.white_candidate(bytes(color) * 120)["candidate"])
        self.assertTrue(audit.white_candidate(bytes([244, 245, 246]) * 120)["candidate"])
        textured = bytes([244, 244, 244]) * 60 + bytes([208, 208, 208]) * 60
        self.assertFalse(audit.white_candidate(textured)["candidate"])

    def test_vfr_group_uses_following_pts_and_censors_boundaries(self):
        rows = [{"frame": i, "pts": t, "hit": hit} for i, (t, hit) in enumerate([(4.0, False), (4.04, True), (4.16, True), (4.20, False)])]
        group = audit.consecutive_groups(rows, lambda r: r["hit"])[0]
        self.assertAlmostEqual(group["observed_interval_ms"], 160)
        self.assertTrue(group["bounded_by_non_hits"])
        truncated = audit.consecutive_groups(rows[:-1], lambda r: r["hit"])[0]
        self.assertIsNone(truncated["observed_interval_ms"])
        self.assertTrue(truncated["right_censored"])
        self.assertFalse(truncated["bounded_by_non_hits"])
        gap = [rows[1], {"frame": 9, "pts": 9.0, "hit": False}]
        self.assertTrue(audit.consecutive_groups(gap, lambda r: r["hit"])[0]["right_censored"])

    def test_roi_and_event_boundaries_reject_bad_inputs(self):
        audit.check_rectangle((10, 10, 54, 38), 64, 48)
        with self.assertRaises(ValueError):
            audit.check_rectangle((10, 10, 55, 38), 64, 48)
        for value in ["0,0,0,10", "-1,0,10,10", "0,0,10"]:
            with self.assertRaises(argparse.ArgumentTypeError):
                audit.rectangle(value)
        with tempfile.TemporaryDirectory() as temporary:
            path = Path(temporary) / "events.json"
            for events in [[], [{"label": "bad", "start": 2, "end": 1}], [{"label": "bad", "start": 1, "end": float("nan")}], [{"label": "bad", "start": 5, "end": 6}]]:
                path.write_text(json.dumps(events))
                with self.assertRaises(ValueError):
                    audit.load_events(path, None, 0, 4)
            path.write_text(json.dumps([{"label": "wall", "start_ms": 1020, "end_ms": 1140}]))
            event = audit.load_events(path, 1000, 5, 9)[0]
            self.assertAlmostEqual(event["start_pts"], 5.02)
            self.assertAlmostEqual(event["event_duration_ms"], 120)


class CompressedVideoTests(unittest.TestCase):
    @unittest.skipUnless(os.environ.get("FFMPEG_BINARY") or shutil.which("ffmpeg"), "Set FFMPEG_BINARY to an installed ffmpeg to run the compressed VFR case")
    def test_compressed_vfr_markers_roi_and_white_transition(self):
        ffmpeg = os.environ.get("FFMPEG_BINARY") or shutil.which("ffmpeg")
        with tempfile.TemporaryDirectory(prefix="tiez-video-audit-") as temporary:
            root = Path(temporary)
            for index in range(5):
                pixels = bytearray(bytes([128, 128, 128]) * 64 * 48)
                color = {1: (255, 0, 255), 2: (244, 244, 244), 3: (0, 0, 0), 4: (0, 255, 255)}.get(index)
                if color is not None:
                    for y in range(12, 36):
                        for x in range(16, 48):
                            pixels[(y * 64 + x) * 3:(y * 64 + x + 1) * 3] = bytes(color)
                (root / f"{index}.ppm").write_bytes(b"P6\n64 48\n255\n" + pixels)
            concat = "\n".join(f"file '{i}.ppm'\nduration {duration}" for i, duration in enumerate([.04, .12, .04, .08, .08])) + "\nfile '4.ppm'\n"
            (root / "frames.txt").write_text(concat)
            video = root / "source.mkv"
            subprocess.run([ffmpeg, "-v", "error", "-f", "concat", "-safe", "0", "-i", str(root / "frames.txt"), "-fps_mode", "passthrough", "-c:v", "libx264", "-crf", "18", "-pix_fmt", "yuv420p", str(video)], check=True)
            args = argparse.Namespace(video=video, output=root / "audit", ffmpeg=ffmpeg, ffprobe=None, roi=None,
                                      white_roi=["body:20,16,24,16"], padding=.5, max_witnesses=4, black_candidate_pixels=256, events=None, anchor_ms=None)
            result = audit.audit(args)
            rows = [json.loads(line) for line in (args.output / "frames.jsonl").read_text().splitlines()]
            self.assertEqual(result["scanned_frames"], 6)
            self.assertEqual(result["colors"]["magenta_strict"]["hit_frames"], 1)
            self.assertEqual(result["colors"]["cyan_strict"]["hit_frames"], 2)
            self.assertEqual(result["colors"]["near_black"]["hit_frames"], 1)
            self.assertEqual(result["white"]["body"]["hit_frames"], 1)
            self.assertTrue(result["white"]["body"]["groups"][0]["bounded_by_non_hits"])
            self.assertAlmostEqual(rows[2]["pts"] - rows[1]["pts"], .12)
            self.assertEqual(rows[0]["colors"]["magenta_strict"], 0)
            self.assertEqual(rows[0]["colors"]["cyan_strict"], 0)
            self.assertEqual({witness["frame"] for witness in result["witnesses"]}, {1, 2, 3, 4})
            self.assertGreater(result["witnesses_omitted_by_limit"], 0)
            # Marker pixels deliberately outside the chosen ROI must not count.
            args.output = root / "outside-roi"
            args.roi = (0, 0, 8, 8)
            args.white_roi = []
            args.max_witnesses = 0
            outside = audit.audit(args)
            self.assertEqual(outside["colors"]["magenta_strict"]["maximum_pixels"], 0)
            self.assertEqual(outside["colors"]["cyan_strict"]["maximum_pixels"], 0)
            self.assertEqual(outside["scanned_frames"], 6)
            # The magenta VFR frame begins before this window but is still
            # displayed during it; sampling only PTS-in-window would miss it.
            events = root / "held-frame-event.json"
            events.write_text(json.dumps([{"label": "held", "start": .10, "end": .13}]))
            args.output = root / "held-frame"
            args.events, args.padding, args.roi = events, 0, None
            held = audit.audit(args)
            self.assertEqual(held["scanned_frames"], 1)
            self.assertGreater(held["colors"]["magenta_strict"]["maximum_pixels"], 0)
            self.assertAlmostEqual(held["events"][0]["first_pts"], .04)
            # Preserve coded-pixel ROI orientation even with a display matrix.
            rotated = root / "rotated.mp4"
            subprocess.run([ffmpeg, "-v", "error", "-noautorotate", "-display_rotation:v:0", "90", "-i", str(video), "-c", "copy", str(rotated)], check=True)
            ffprobe = str(Path(ffmpeg).with_name("ffprobe.exe" if Path(ffmpeg).suffix == ".exe" else "ffprobe"))
            rotation_tags = audit.command_json([ffprobe, "-v", "error", "-show_entries", "stream_side_data=rotation", "-of", "json", str(rotated)])
            self.assertTrue(any(abs(side.get("rotation", 0)) == 90 for stream in rotation_tags["streams"] for side in stream.get("side_data_list", [])))
            args.video, args.output, args.events = rotated, root / "rotated-audit", None
            args.white_roi, args.max_witnesses = ["body:20,16,24,16"], 20
            rotation = audit.audit(args)
            self.assertEqual(rotation["white"]["body"]["hit_frames"], 1)
            self.assertEqual(rotation["colors"]["magenta_strict"]["maximum_pixels"], result["colors"]["magenta_strict"]["maximum_pixels"])
            png = (args.output / rotation["witnesses"][0]["file"]).read_bytes()
            self.assertEqual(struct.unpack(">II", png[16:24]), (64, 48))


if __name__ == "__main__":
    unittest.main()
