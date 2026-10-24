#!/usr/bin/env python3
"""Draws the seven chakra symbols and the idle mark as white PNGs with alpha.

What: components/app_chakra/ui/art/chakra_<0..6>.png and idle.png, SIZE px square, plus
      docs/symbols.png (all seven on their colours) for the README.
Why:  the app's own artwork, drawn from geometry (petal count and inner figure of each chakra
      as tradition names them), so nothing third-party ships with the public repository.
Pitfalls:
  * Drawn at 4x and scaled down for smooth edges; Pillow's polygon fill has no antialiasing.
  * GSP shows images at their native size; SIZE must match the scene (make_scene.py).
Usage:  tools/make_symbols.py [--dry-run]
"""
import math
import sys
from pathlib import Path

from PIL import Image, ImageDraw

HERE = Path(__file__).resolve().parent.parent
ART = HERE / "components/app_chakra/ui/art"
DOCS = HERE / "docs"
SIZE = 168
SS = 4
W = SIZE * SS
C = W / 2
WHITE = (255, 255, 255, 255)
# petals, inner figure; colours only for the README sheet (chakra_detect.c holds the real ones)
CHAKRAS = [
    (4, "square", "#E0262B"),
    (6, "crescent", "#F47A1F"),
    (10, "triangle_down", "#F2B705"),
    (12, "hexagram", "#2FA84F"),
    (16, "circle_triangle", "#1E88E5"),
    (2, "eye", "#4B3FB5"),
    (32, "crown", "#9B3FC8"),
]


def petal(draw, angle, r_in, r_out, width, fill):
    """A lens-shaped petal from radius r_in to r_out, pointing at angle (radians)."""
    pts = []
    for i in range(41):
        t = i / 40
        r = r_in + (r_out - r_in) * t
        w = width * math.sin(math.pi * t) ** 0.8
        pts.append((r, w))
    poly = [(C + r * math.cos(angle) - w * math.sin(angle), C + r * math.sin(angle) + w * math.cos(angle)) for r, w in pts]
    poly += [(C + r * math.cos(angle) + w * math.sin(angle), C + r * math.sin(angle) - w * math.cos(angle)) for r, w in reversed(pts)]
    draw.polygon(poly, fill=fill)


def ring(draw, r, width, fill=WHITE):
    draw.ellipse([C - r, C - r, C + r, C + r], outline=fill, width=width)


def polygon(draw, n, r, rotation, width, fill=WHITE):
    pts = [(C + r * math.cos(rotation + 2 * math.pi * i / n), C + r * math.sin(rotation + 2 * math.pi * i / n)) for i in range(n)]
    draw.line(pts + [pts[0]], fill=fill, width=width, joint="curve")


def symbol(petals, inner):
    img = Image.new("RGBA", (W, W), (0, 0, 0, 0))
    d = ImageDraw.Draw(img)
    line = int(W * 0.022)
    r_disc = W * 0.30
    r_out = W * 0.49
    n = petals
    width = min(W * 0.11, math.pi * r_disc / n * 1.15) if n > 2 else W * 0.13
    if n == 2:  # Ajna: two wide wings
        for a in (0.0, math.pi):
            petal(d, a, r_disc * 0.85, r_out, width * 1.25, WHITE)
    else:
        for i in range(n):
            a = -math.pi / 2 + 2 * math.pi * (i + 0.5) / n
            petal(d, a, r_disc * 0.82, r_out if n < 30 else r_out * 0.96, width, WHITE)
    # inner disc cut out, then a ring, so the petals read as a lotus around an open centre
    d.ellipse([C - r_disc, C - r_disc, C + r_disc, C + r_disc], fill=(0, 0, 0, 0))
    ring(d, r_disc, line)
    r = r_disc * 0.72
    if inner == "square":
        polygon(d, 4, r * 1.05, math.pi / 4, line)
    elif inner == "crescent":
        d.ellipse([C - r * 0.75, C - r * 0.55, C + r * 0.75, C + r * 0.95], fill=WHITE)
        d.ellipse([C - r * 0.75, C - r * 0.85, C + r * 0.75, C + r * 0.65], fill=(0, 0, 0, 0))
    elif inner == "triangle_down":
        polygon(d, 3, r, math.pi / 2, line)
    elif inner == "hexagram":
        polygon(d, 3, r, math.pi / 2, line)
        polygon(d, 3, r, -math.pi / 2, line)
    elif inner == "circle_triangle":
        polygon(d, 3, r, math.pi / 2, line)
        ring(d, r * 0.48, line)
    elif inner == "eye":
        polygon(d, 3, r * 0.9, math.pi / 2, line)
        d.ellipse([C - r * 0.2, C - r * 0.2, C + r * 0.2, C + r * 0.2], fill=WHITE)
    elif inner == "crown":
        ring(d, r * 0.62, line)
        d.ellipse([C - r * 0.22, C - r * 0.22, C + r * 0.22, C + r * 0.22], fill=WHITE)
    return img.resize((SIZE, SIZE), Image.LANCZOS)


def idle():
    """Seven small dots on a ring: no tone yet."""
    img = Image.new("RGBA", (W, W), (0, 0, 0, 0))
    d = ImageDraw.Draw(img)
    ring(d, W * 0.30, int(W * 0.016), (255, 255, 255, 170))
    for i in range(7):
        a = -math.pi / 2 + 2 * math.pi * i / 7
        x, y = C + W * 0.30 * math.cos(a), C + W * 0.30 * math.sin(a)
        rr = W * 0.035
        d.ellipse([x - rr, y - rr, x + rr, y + rr], fill=WHITE)
    rr = W * 0.05
    d.ellipse([C - rr, C - rr, C + rr, C + rr], fill=WHITE)
    return img.resize((SIZE, SIZE), Image.LANCZOS)


def sheet(images):
    tile = SIZE + 40
    out = Image.new("RGB", (tile * 7, tile), "#111014")
    for i, (img, (_, _, colour)) in enumerate(zip(images, CHAKRAS)):
        bg = Image.new("RGB", (tile, tile), colour)
        bg.paste(img, (20, 20), img)
        out.paste(bg, (i * tile, 0))
    return out


def main():
    dry = "--dry-run" in sys.argv
    targets = [ART / f"chakra_{i}.png" for i in range(7)] + [ART / "idle.png", DOCS / "symbols.png"]
    if dry:
        print("would write:", *[str(t.relative_to(HERE)) for t in targets], sep="\n  ")
        return
    ART.mkdir(parents=True, exist_ok=True)
    DOCS.mkdir(parents=True, exist_ok=True)
    images = [symbol(p, inner) for p, inner, _ in CHAKRAS]
    for i, img in enumerate(images):
        img.save(ART / f"chakra_{i}.png", optimize=True)
    idle().save(ART / "idle.png", optimize=True)
    sheet(images).save(DOCS / "symbols.png", optimize=True)
    print(f"wrote 8 symbols ({SIZE} px) and docs/symbols.png")


if __name__ == "__main__":
    main()
