#!/usr/bin/env python3
"""Draw the firmware's motion zone on a photo, and optionally score two photos
with the real firmware detector (firmware/test/zone_motion_test, host build).

    python3 scripts/zone_preview.py photo.jpg
    python3 scripts/zone_preview.py before.jpg --pair after.jpg

The zone comes from firmware/main/zone_config.h, so the picture always matches
what the device is using. Build the detector first:
    make -C firmware/test
Needs numpy and Pillow:
    uv run --with numpy --with pillow scripts/zone_preview.py photo.jpg
"""
import argparse
import re
import subprocess
import sys
import tempfile
from pathlib import Path

import numpy as np
from PIL import Image, ImageDraw

REPO = Path(__file__).resolve().parent.parent
HEADER = REPO / "firmware" / "main" / "zone_config.h"
PROBE = (160, 120)  # FRAMESIZE_QQVGA
BIN = REPO / "firmware" / "test" / "zone_motion_test"


def zone_config():
    src = HEADER.read_text()
    cfg = {}
    for name in ("ZONE_X0_PCT", "ZONE_X1_PCT", "ZONE_Y0_PCT", "ZONE_Y1_PCT", "ZONE_CELL"):
        m = re.search(rf"#define {name} (\d+)", src)
        if not m:
            sys.exit(f"{HEADER}: {name} not found")
        cfg[name] = int(m.group(1))
    return cfg


def probe_gray(path):
    im = Image.open(path).convert("L").resize(PROBE, Image.Resampling.BILINEAR)
    return np.asarray(im, dtype=np.uint8), im


def annotate(path, cfg, out):
    gray, small = probe_gray(path)
    photo = Image.open(path).convert("RGB")
    W, H = photo.size
    px, py = W / PROBE[0], H / PROBE[1]
    x0, x1 = cfg["ZONE_X0_PCT"] * PROBE[0] // 100, cfg["ZONE_X1_PCT"] * PROBE[0] // 100
    y0, y1 = cfg["ZONE_Y0_PCT"] * PROBE[1] // 100, cfg["ZONE_Y1_PCT"] * PROBE[1] // 100
    d = ImageDraw.Draw(photo)
    d.rectangle([x0 * px, y0 * py, x1 * px - 1, y1 * py - 1], outline=(0, 255, 80), width=3)
    cell = cfg["ZONE_CELL"]
    for gx in range(x0, x1 + 1, cell):
        d.line([gx * px, y0 * py, gx * px, y1 * py], fill=(0, 160, 60), width=1)
    for gy in range(y0, y1 + 1, cell):
        d.line([x0 * px, gy * py, x1 * px, gy * py], fill=(0, 160, 60), width=1)
    d.text((x0 * px + 6, y0 * py + 6), "motion zone", fill=(0, 255, 80))
    photo.save(out)
    print(f"zone {x0},{y0}..{x1},{y1} of {PROBE[0]}x{PROBE[1]}  grid {cell}px  -> {out}")
    return gray


def score(a, b):
    if not BIN.exists():
        sys.exit(f"{BIN} missing: run `make -C firmware/test`")
    with tempfile.TemporaryDirectory() as tmp:
        pa = Path(tmp) / "a.raw"
        pb = Path(tmp) / "b.raw"
        a.tofile(pa)
        b.tofile(pb)
        res = subprocess.run([str(BIN), "--raw", str(pa), str(pb)], capture_output=True, text=True)
    if res.returncode:
        sys.exit(res.stderr or res.stdout)
    for line in res.stdout.splitlines():
        if "hot=" in line:
            print(line.split("/")[-1])


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("photo")
    ap.add_argument("--pair", help="second photo: score photo -> pair with the device detector")
    ap.add_argument("--out", default="zone_preview.png")
    args = ap.parse_args()

    cfg = zone_config()
    gray = annotate(args.photo, cfg, args.out)
    if args.pair:
        gray2, _ = probe_gray(args.pair)
        score(gray, gray2)


if __name__ == "__main__":
    main()
