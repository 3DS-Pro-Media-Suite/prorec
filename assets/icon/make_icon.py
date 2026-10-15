#!/usr/bin/env python3
"""Draws icon.png in the repo root: the 48x48 HOME Menu icon, a studio microphone in the style of Pro Cam's.

One flat off-white glyph on black (capsule, yoke, stem, base) with the same corner radii and stroke weights as Pro
Cam's 3DS glyph. Shapes are built with shapely, drawn at 16x and filtered down to 48 px.

Run: python assets/icon/make_icon.py [preview.png]   (needs Pillow and shapely)
The optional preview shows the icon at 8x, 48 px and 24 px.
"""
import os
import sys

from PIL import Image, ImageDraw
from shapely.geometry import MultiPolygon, Point, box
from shapely.ops import unary_union

HERE = os.path.dirname(os.path.abspath(__file__))
OUT = os.path.join(HERE, "..", "..", "icon.png")

N = 48            # icon size
S = 16            # supersampling
W = N * S

BG = (12, 12, 14)
INK = (238, 235, 228)

CAP = (17.5, 5, 30.5, 28)     # capsule: x0, y0, x1, y1 (fully rounded ends)
YOKE_R, YOKE_W = 10.2, 2.6    # the U around the capsule: radius to its centre line, stroke
YOKE_CY = 20.5                # centre of the U's arc
STEM = (22.6, 25.4)           # stem x range
BASE = (14, 39.5, 34, 42.5)   # base plate
SLOTS = (10.5, 14.5, 18.5)    # grille slots (y) on the capsule, cut out
SLOT = (20.6, 27.4, 1.25)     # slot x range and half height


def rrect(x0, y0, x1, y1, r):
    return box(x0 + r, y0 + r, x1 - r, y1 - r).buffer(r, quad_segs=48)


def glyph():
    cap = rrect(*CAP, (CAP[2] - CAP[0]) / 2)
    ring = Point(24, YOKE_CY).buffer(YOKE_R + YOKE_W / 2, quad_segs=64).difference(
        Point(24, YOKE_CY).buffer(YOKE_R - YOKE_W / 2, quad_segs=64))
    yoke = ring.intersection(box(0, YOKE_CY - 2.5, 48, 48))  # the lower half, arms a little higher than the centre
    arm_tips = unary_union([Point(24 - YOKE_R, YOKE_CY - 2.5).buffer(YOKE_W / 2, quad_segs=32),
                            Point(24 + YOKE_R, YOKE_CY - 2.5).buffer(YOKE_W / 2, quad_segs=32)])
    stem = box(STEM[0], YOKE_CY + YOKE_R, STEM[1], BASE[1])
    base = rrect(*BASE, (BASE[3] - BASE[1]) / 2)
    g = unary_union([cap, yoke, arm_tips, stem, base])
    for y in SLOTS:
        g = g.difference(rrect(SLOT[0], y - SLOT[2], SLOT[1], y + SLOT[2], SLOT[2]))
    return g


def raster(g):
    m = Image.new("L", (W, W), 0)
    d = ImageDraw.Draw(m)
    for p in (g.geoms if isinstance(g, MultiPolygon) else [g]):
        d.polygon([(x * S, y * S) for x, y in p.exterior.coords], fill=255)
        for h in p.interiors:
            d.polygon([(x * S, y * S) for x, y in h.coords], fill=0)
    return m


def main():
    img = Image.new("RGBA", (W, W), (0, 0, 0, 0))
    tile = Image.new("L", (W, W), 0)
    ImageDraw.Draw(tile).rounded_rectangle([0, 0, W, W], radius=8 * S, fill=255)
    img.paste(Image.new("RGBA", (W, W), BG + (255,)), (0, 0), tile)
    img.paste(Image.new("RGBA", (W, W), INK + (255,)), (0, 0), raster(glyph()))

    icon = img.resize((N, N), Image.LANCZOS)
    icon.save(OUT)
    print("wrote", os.path.normpath(OUT))
    if len(sys.argv) > 1:
        big = Image.new("RGBA", (N * 8 + 24 + N * 4 + 24 + 24 * 4 + 24, N * 8), (210, 210, 206, 255))
        big.alpha_composite(img.resize((N * 8, N * 8), Image.LANCZOS), (0, 0))
        big.alpha_composite(icon.resize((N * 4, N * 4), Image.NEAREST), (N * 8 + 24, 0))
        small = icon.resize((24, 24), Image.LANCZOS)
        big.alpha_composite(small.resize((24 * 4, 24 * 4), Image.NEAREST), (N * 12 + 48, 0))
        big.alpha_composite(icon, (N * 8 + 24, N * 4 + 24))
        big.alpha_composite(small, (N * 12 + 48, N * 4 + 24))
        big.save(sys.argv[1])
        print("wrote", sys.argv[1])


if __name__ == "__main__":
    main()
