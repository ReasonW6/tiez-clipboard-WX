"""Prepare fictional pixels / audit browser composites. Never captures the desktop."""
import argparse
import json
import math
from pathlib import Path
import re

import numpy as np
from PIL import Image, ImageDraw, ImageFont


def luminance(rgb):
    rgb = np.asarray(rgb) / 255.0
    return np.where(rgb <= .04045, rgb / 12.92, ((rgb + .055) / 1.055) ** 2.4) @ [.2126, .7152, .0722]


def prepare(directory, manifest):
    width, height = manifest["viewport"].values()
    pattern = Image.new("RGBA", (width, height), "#ececec")
    draw = ImageDraw.Draw(pattern)
    # Neutral source: colour separation in the output cannot come from rainbow paint.
    font = ImageFont.truetype("C:/Windows/Fonts/arial.ttf", 24)
    for y in range(-10, height, 42):
        draw.text((8, y), "AaBb 0123456789 TEST GLASS", font=font, fill="#101010")
    for x in range(26, width, 91):
        draw.rectangle((x, 0, x + 5, height), fill="#101010")
    pattern.save(directory / "source-pattern.png")
    for stem in manifest["snapshots"]:
        state = (directory / f"{stem}.state").read_bytes()
        for kind in ["pattern", "extreme"]:
            source = pattern if kind == "pattern" else Image.new("RGBA", (width, height), "white" if stem.startswith("dark") else "black")
            (directory / f"{stem}-{kind}.state").write_bytes(state)
            (directory / f"{stem}-{kind}.rgba").write_bytes(source.tobytes())
        # The renderer enumerates only prepared .state files with matching pixels.
        (directory / f"{stem}.state").replace(directory / f"{stem}.state-export")


def audit(directory, manifest, kinds=("extreme", "pattern")):
    width, height = manifest["viewport"].values()
    report = {"scope": "Static headless Chromium foreground over production WARP shader; no native capture/presentation or iOS equivalence proof", "cases": []}
    for stem in manifest["snapshots"]:
        layout = json.loads((directory / f"{stem}.json").read_text(encoding="utf-8"))
        depth = np.zeros((height, width), dtype=np.uint8)
        yy, xx = np.mgrid[:height, :width] + .5
        for control in layout["geometry"]["controls"]:
            x, y, w, h, r = [control[k] for k in ["x", "y", "width", "height", "radius"]]
            # Independent rounded rectangle membership via nearest point in inner box.
            cx = np.clip(xx, x + r, x + w - r)
            cy = np.clip(yy, y + r, y + h - r)
            depth += ((xx - cx) ** 2 + (yy - cy) ** 2 <= r ** 2)
        for kind in kinds:
            front = np.array(Image.open(directory / f"{stem}-{kind}.composed.png").convert("RGB"))
            paper = np.array(Image.open(directory / f"{stem}-{kind}.textless.png").convert("RGB"))
            glyphs = np.max(np.abs(front.astype(int) - paper.astype(int)), axis=2) > 2
            runs = []
            for run in layout["texts"]:
                if run["disabled"] or not run["opacity"]:
                    continue
                values = [float(x) for x in re.findall(r"[\d.]+", run["color"])]
                ink = np.array(values[:3])
                alpha = (values[3] if len(values) > 3 else 1) * run["opacity"]
                selected = np.zeros((height, width), dtype=bool)
                for rect in run["rects"]:
                    x, y, w, h = [rect[k] for k in ["x", "y", "width", "height"]]
                    selected[max(0, math.floor(y)):min(height, math.ceil(y+h)), max(0, math.floor(x)):min(width, math.ceil(x+w))] = True
                selected &= glyphs
                if not selected.any():
                    continue  # Fully clipped, occluded, or no rendered text pixels.
                backgrounds = paper[selected].astype(float)
                fg = luminance(alpha * ink + (1-alpha) * backgrounds)
                bg = luminance(backgrounds)
                contrast = (np.maximum(fg,bg)+.05)/(np.minimum(fg,bg)+.05)
                threshold = 3 if run["fontSize"] >= 24 or (run["fontSize"] >= 18.6667 and int(run["fontWeight"]) >= 700) else 4.5
                index = int(np.argmin(contrast))
                coordinates = np.argwhere(selected)
                runs.append({"text": run["text"], "color": run["color"], "minContrast": float(contrast[index]),
                    "required": threshold, "minAtYX": coordinates[index].tolist(), "maxGlassDepth": int(depth[selected].max()),
                    "samplePixels": int(selected.sum()), "passes": bool(contrast[index] >= threshold)})
            case = {"name": f"{stem}-{kind}", "maxDepth": int(depth.max()), "textRuns": runs,
                "failures": sum(not r["passes"] for r in runs), "minimum": min((r["minContrast"] for r in runs), default=None)}
            report["cases"].append(case)
    report["passes"] = all(case["failures"] == 0 for case in report["cases"])
    (directory / "contrast.json").write_text(json.dumps(report, ensure_ascii=False, indent=2), encoding="utf-8")
    failures = [(case["name"], r["text"], round(r["minContrast"], 4), r["maxGlassDepth"]) for case in report["cases"] for r in case["textRuns"] if not r["passes"]]
    print(json.dumps({"cases": len(report["cases"]), "failingRuns": len(failures), "failures": failures}, ensure_ascii=False, indent=2))


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("mode", choices=["prepare", "audit"])
    parser.add_argument("directory", type=Path)
    args = parser.parse_args()
    data = json.loads((args.directory / "manifest.json").read_text(encoding="utf-8"))
    (prepare if args.mode == "prepare" else audit)(args.directory, data)
