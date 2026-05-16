#!/usr/bin/env python
"""Procedural placeholder skydome texture generator.

Generates 8 equirectangular TGA files (1024x512, 24-bit RGB) with simple
colour ramps representing each skydome scene. These are v1 placeholders;
production-quality BC1 DDS assets are a follow-up PR.
"""

import os
import numpy as np
from PIL import Image

# Each tuple: (name, top_color RGB, mid_color, bottom_color)
SCENES = [
    ("space",      (0, 0, 8),         (0, 0, 16),        (0, 0, 8)),
    ("atmosphere", (50, 90, 180),     (140, 180, 220),   (200, 210, 230)),
    ("sunset",     (200, 80, 40),     (220, 140, 70),    (180, 100, 60)),
    ("dawn",       (200, 150, 200),   (240, 200, 180),   (200, 180, 180)),
    ("night",      (10, 10, 30),      (20, 20, 40),      (10, 15, 25)),
    ("overcast",   (140, 150, 160),   (170, 175, 180),   (150, 155, 160)),
    ("studio",     (180, 180, 180),   (200, 200, 200),   (160, 160, 160)),
    ("indoor",     (60, 55, 50),      (80, 75, 70),      (50, 45, 40)),
]

W, H = 1024, 512
out_dir = "src/Resources/skydomes"
os.makedirs(out_dir, exist_ok=True)

for name, top, mid, bot in SCENES:
    img = np.zeros((H, W, 3), dtype=np.uint8)
    for y in range(H):
        t = y / (H - 1)
        if t < 0.5:
            a = t * 2.0
            c = np.array(top) * (1.0 - a) + np.array(mid) * a
        else:
            a = (t - 0.5) * 2.0
            c = np.array(mid) * (1.0 - a) + np.array(bot) * a
        img[y, :, :] = c.astype(np.uint8)
    Image.fromarray(img, "RGB").save(os.path.join(out_dir, f"{name}.tga"))
    print(f"wrote {name}.tga")
