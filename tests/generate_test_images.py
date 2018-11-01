#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (C) 2018 Marco Antônio Bueno da Silva <bueno.marco@gmail.com>
#
# generate_test_images.py
# Generates synthetic test images in multiple formats and resolutions.
# Run once to populate tests/testdata/images/

import os
import math
import struct
import zlib
from pathlib import Path

try:
    from PIL import Image, ImageDraw, ImageFont
    HAS_PILLOW = True
except ImportError:
    HAS_PILLOW = False
    print("Pillow not found — generating minimal raw images instead.")

BASE = Path(__file__).parent / "testdata" / "images"

# ── Color palettes for synthetic images ──────────────────────────────────────

PALETTES = [
    [(220,  60,  60), (255, 200, 100), (255, 255, 255)],  # warm
    [( 60, 120, 220), ( 30,  30,  80), (200, 220, 255)],  # cool/night
    [( 40, 160,  80), (200, 240, 180), (255, 255, 200)],  # nature
    [(180,  60, 220), ( 60,  20,  80), (255, 180, 255)],  # purple
    [(240, 140,  30), ( 80,  40,   0), (255, 220, 150)],  # sunset
]

# ── Image generators ──────────────────────────────────────────────────────────

def make_gradient(w, h, c1, c2):
    """Diagonal gradient from c1 to c2."""
    img = Image.new("RGB", (w, h))
    px  = img.load()
    for y in range(h):
        for x in range(w):
            t = (x + y) / (w + h)
            px[x, y] = tuple(int(c1[i] + (c2[i]-c1[i])*t) for i in range(3))
    return img

def make_grid(w, h, c1, c2, cell=40):
    """Checkerboard pattern."""
    img = Image.new("RGB", (w, h))
    px  = img.load()
    for y in range(h):
        for x in range(w):
            px[x, y] = c1 if ((x//cell + y//cell) % 2 == 0) else c2
    return img

def make_circles(w, h, colors):
    """Concentric circles."""
    img  = Image.new("RGB", (w, h), colors[2])
    draw = ImageDraw.Draw(img)
    cx, cy = w//2, h//2
    for r in range(min(w,h)//2, 0, -20):
        col = colors[0] if (r // 20) % 2 == 0 else colors[1]
        draw.ellipse([cx-r, cy-r, cx+r, cy+r], fill=col)
    return img

def make_stripes(w, h, c1, c2, stripe_w=30):
    """Vertical stripes."""
    img = Image.new("RGB", (w, h))
    px  = img.load()
    for y in range(h):
        for x in range(w):
            px[x, y] = c1 if (x // stripe_w) % 2 == 0 else c2
    return img

def make_noise(w, h):
    """Random noise (stress test for compressors)."""
    import random
    img = Image.new("RGB", (w, h))
    px  = img.load()
    rng = random.Random(42)
    for y in range(h):
        for x in range(w):
            px[x, y] = (rng.randint(0,255), rng.randint(0,255), rng.randint(0,255))
    return img

def make_text_screenshot(w, h):
    """Simulates a screenshot with text (good for PNG lossless test)."""
    img  = Image.new("RGB", (w, h), (245, 245, 245))
    draw = ImageDraw.Draw(img)
    draw.rectangle([0, 0, w, 28], fill=(50, 50, 50))
    draw.text((10, 6),  "batchpress — Test Screenshot", fill=(255,255,255))
    lines = [
        "int main(int argc, char* argv[]) {",
        "    Config cfg = parse_args(argc, argv);",
        "    BatchReport r = run_batch(cfg);",
        "    return r.failed > 0 ? 1 : 0;",
        "}",
        "",
        "// Compressor: batchpress v2.0",
        "// Author: Marco Antônio Bueno da Silva",
    ]
    for i, line in enumerate(lines):
        draw.text((10, 40 + i*22), line, fill=(30,30,30))
    return img

def make_rgba_gradient(w, h, c1, c2):
    """RGBA image with transparency gradient."""
    img = Image.new("RGBA", (w, h))
    px  = img.load()
    for y in range(h):
        for x in range(w):
            t   = x / w
            r   = int(c1[0] + (c2[0]-c1[0])*t)
            g   = int(c1[1] + (c2[1]-c1[1])*t)
            b   = int(c1[2] + (c2[2]-c1[2])*t)
            a   = int(255 * (1 - y/h))
            px[x, y] = (r, g, b, a)
    return img

# ── Generation plan ───────────────────────────────────────────────────────────

SPECS = [
    # (subdir,             filename,              size,        generator,    format, kwargs)
    # -- photos: large, varied content
    ("photos", "photo_4k_gradient.jpg",       (3840,2160), "gradient",   "JPEG", {"quality":95}),
    ("photos", "photo_fullhd_circles.jpg",    (1920,1080), "circles",    "JPEG", {"quality":90}),
    ("photos", "photo_hd_stripes.jpg",        (1280,720),  "stripes",    "JPEG", {"quality":85}),
    ("photos", "photo_medium_gradient.jpg",   (800, 600),  "gradient",   "JPEG", {"quality":80}),
    ("photos", "photo_small_noise.jpg",       (400, 300),  "noise",      "JPEG", {"quality":75}),
    ("photos", "photo_square_circles.png",    (1024,1024), "circles",    "PNG",  {}),
    ("photos", "photo_wide_gradient.bmp",     (1920,400),  "gradient",   "BMP",  {}),
    ("photos", "photo_rgba_sunset.png",       (800, 600),  "rgba",       "PNG",  {}),

    # -- screenshots: flat content, great for PNG
    ("screenshots", "screen_1080p.png",       (1920,1080), "screenshot", "PNG",  {}),
    ("screenshots", "screen_720p.png",        (1280,720),  "screenshot", "PNG",  {}),
    ("screenshots", "screen_mobile.png",      (390, 844),  "screenshot", "PNG",  {}),
    ("screenshots", "screen_small.bmp",       (640, 480),  "screenshot", "BMP",  {}),

    # -- subdir/nested: test recursive traversal
    ("subdir",          "nested_top.jpg",     (640, 480),  "gradient",   "JPEG", {"quality":85}),
    ("subdir/nested",   "nested_deep.jpg",    (320, 240),  "circles",    "JPEG", {"quality":80}),
    ("subdir/nested",   "nested_png.png",     (320, 240),  "stripes",    "PNG",  {}),
]

def generate(spec):
    subdir, fname, (w, h), gen, fmt, kwargs = spec
    out_dir = BASE / subdir
    out_dir.mkdir(parents=True, exist_ok=True)
    out_path = out_dir / fname

    pal = PALETTES[hash(fname) % len(PALETTES)]

    if gen == "gradient":
        img = make_gradient(w, h, pal[0], pal[1])
    elif gen == "circles":
        img = make_circles(w, h, pal)
    elif gen == "stripes":
        img = make_stripes(w, h, pal[0], pal[1])
    elif gen == "noise":
        img = make_noise(w, h)
    elif gen == "screenshot":
        img = make_text_screenshot(w, h)
    elif gen == "rgba":
        img = make_rgba_gradient(w, h, pal[0], pal[1])
    else:
        img = make_gradient(w, h, pal[0], pal[1])

    img.save(str(out_path), fmt, **kwargs)
    size_kb = out_path.stat().st_size // 1024
    print(f"  ✓ {subdir}/{fname}  {w}x{h}  {size_kb} KB")

if __name__ == "__main__":
    if not HAS_PILLOW:
        print("ERROR: pip install Pillow")
        exit(1)

    print(f"\nGenerating {len(SPECS)} test images → {BASE}\n")
    for spec in SPECS:
        generate(spec)

    # Count totals
    total = sum(1 for _ in BASE.rglob("*") if _.is_file())
    size  = sum(f.stat().st_size for f in BASE.rglob("*") if f.is_file())
    print(f"\n✓ {total} files  {size//1024} KB total\n")
