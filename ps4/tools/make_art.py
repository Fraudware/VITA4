#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""VITA4 (PS4) art: reuses the PS5 generator's scene parts (art/generate_art.py).

Outputs:
  (ps4/sce_sys/icon0.png, pic0.png and pic1.png are the owner's own art since
   1.02-1.04 and are not generated here; pic1/launch-background-source.png =
   the splash picture)
  ps4/assets/backdrop.png 1920x1080 in-app backdrop (emblem on the right, no text)

    python3 ps4/tools/make_art.py [path/to/Inter-Bold.otf]
"""
import os
import shutil
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
PS4 = os.path.dirname(HERE)
REPO = os.path.dirname(PS4)
sys.path.insert(0, os.path.join(REPO, "art"))

import generate_art as ga  # noqa: E402
from PIL import Image, ImageDraw  # noqa: E402

FONT = sys.argv[1] if len(sys.argv) > 1 else "/usr/share/fonts/opentype/inter/Inter-Bold.otf"
ga.FONT_BOLD = FONT
ga.FONT_REG = FONT


def wordmark4(size, xy, px, spacing=8, scale=1):
    """'vitaDock' in white + '4' in cyan (the PS4 twin of the PS5 wordmark)."""
    layer = Image.new("RGBA", size, (0, 0, 0, 0))
    d = ImageDraw.Draw(layer)
    font = ga.ImageFont.truetype(ga.FONT_BOLD, px)
    parts = [("vitaDock", ga.WHITE), ("4", ga.CYAN)]
    widths = [sum(d.textlength(ch, font=font) for ch in t) + spacing * (len(t) - 1)
              for t, _ in parts]
    gap = int(px * 0.03)
    total = sum(widths) + gap
    x = xy[0] - total / 2
    y = xy[1] - px * 0.62
    for (t, col), wdt in zip(parts, widths):
        cx = x
        for ch in t:
            d.text((cx, y), ch, font=font, fill=col + (255,))
            cx += d.textlength(ch, font=font) + spacing
        x += wdt + gap
    return layer


ga.wordmark = wordmark4


def emblem(img, cx, cy, ew, eh):
    w, h = img.size
    base = Image.new("RGBA", (w, h), (0, 0, 0, 0))
    d = ImageDraw.Draw(base)
    d._image = base
    screen = [cx - ew, cy - eh * 1.05, cx + ew, cy - eh * 0.12]
    dev = [cx - ew * 0.64, cy + eh * 0.14, cx + ew * 0.64, cy + eh * 1.02]
    ga.bigscreen(d, screen, frame=ga.CYAN)
    beam = Image.new("RGBA", (w, h), (0, 0, 0, 0))
    ImageDraw.Draw(beam).polygon(
        [(cx - ew * 0.35, cy - eh * 0.14), (cx + ew * 0.35, cy - eh * 0.14),
         (cx + ew * 0.58, cy + eh * 0.26), (cx - ew * 0.58, cy + eh * 0.26)],
        fill=ga.CYAN + (190,))
    img = ga.add_glow(img, beam, radius=22, opacity=0.85)
    img = ga.add_glow(img, beam, radius=70, opacity=0.4)
    ga.device(d, dev)
    img = ga.add_glow(img, base, radius=4, opacity=1.0)
    crisp = Image.new("RGBA", (w, h), (0, 0, 0, 0))
    cd = ImageDraw.Draw(crisp)
    cd._image = crisp
    ga.bigscreen(cd, screen, frame=ga.CYAN)
    ga.device(cd, dev)
    return Image.alpha_composite(img.convert("RGBA"), crisp).convert("RGB")


def gen_backdrop(path):
    w, h = 1920, 1080
    img = ga.make_backdrop(w, h, seed=31)
    img = emblem(img, w * 0.745, h * 0.43, w * 0.12, h * 0.18)
    sp = Image.new("RGBA", (w, h), (0, 0, 0, 0))
    ga.particles(ImageDraw.Draw(sp), w, h, 110, 55,
                 [ga.CYAN, ga.VIOLET, ga.MAGENTA, ga.WHITE], rmin=1, rmax=3)
    img = ga.add_glow(img, sp, radius=5, opacity=0.55)
    img = ga.vignette(img, 0.42)
    img.save(path, optimize=True)


def main():
    os.makedirs(os.path.join(PS4, "sce_sys"), exist_ok=True)
    os.makedirs(os.path.join(PS4, "assets"), exist_ok=True)
    gen_backdrop(os.path.join(PS4, "assets", "backdrop.png"))
    print("art written to ps4/sce_sys and ps4/assets")


if __name__ == "__main__":
    main()
