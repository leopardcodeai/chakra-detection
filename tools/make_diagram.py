#!/usr/bin/env python3
"""Draws docs/how-it-works.png: from the bowl's sound to the room's colour.

What: one picture of the chain the README explains, drawn with Pillow in the app's colours and
      its own fonts (no Mermaid: GitHub failed to render one on 24.10.2026).
Usage: tools/make_diagram.py [--dry-run]
"""
import sys
from pathlib import Path

from PIL import Image, ImageDraw, ImageFont

HERE = Path(__file__).resolve().parent.parent
FONTS = HERE / "components/app_chakra/ui/fonts"
OUT = HERE / "docs/how-it-works.png"
BG, CARD, INK, SUB = "#141219", "#24212C", "#FFFFFF", "#B9B4C6"
RGB = ["#E0262B", "#F47A1F", "#F2B705", "#2FA84F", "#1E88E5", "#4B3FB5", "#9B3FC8"]
STEPS = [
    ("Listen", "ES8311 microphone\n16 kHz, 64 ms hops"),
    ("Find the tone", "4096-point FFT, 3.9 Hz\nharmonics, interpolation"),
    ("Is it a tone?", "loud enough, 45 % of the\nenergy on its harmonics,\npitch steady for 0.4 s"),
    ("Which chakra?", "note (A = 440 or 432),\nSolfeggio, or your\nown learned bowls"),
    ("Colour the room", "screen fades to the colour,\nHue, Home Assistant, WiZ\nfollow; silence keeps it"),
]


def main():
    if "--dry-run" in sys.argv:
        print(f"would write {OUT.relative_to(HERE)}")
        return
    W, H, S = 1600, 520, 2
    img = Image.new("RGB", (W * S, H * S), BG)
    d = ImageDraw.Draw(img)
    title = ImageFont.truetype(str(FONTS / "Inter-ExtraBold.ttf"), 30 * S)
    head = ImageFont.truetype(str(FONTS / "Inter-Bold.ttf"), 26 * S)
    body = ImageFont.truetype(str(FONTS / "Inter-Medium.ttf"), 19 * S)
    d.text((60 * S, 40 * S), "From a singing bowl to the colour of the room", font=title, fill=INK)
    bw, bh, gap, y = 270, 250, 37, 130
    for i, (h, b) in enumerate(STEPS):
        x = 60 + i * (bw + gap)
        d.rounded_rectangle([x * S, y * S, (x + bw) * S, (y + bh) * S], radius=26 * S, fill=CARD)
        d.ellipse([(x + 24) * S, (y + 26) * S, (x + 60) * S, (y + 62) * S], fill=RGB[i + 1])
        d.text(((x + 42) * S, (y + 44) * S), str(i + 1), font=head, fill=INK, anchor="mm")
        d.text(((x + 24) * S, (y + 84) * S), h, font=head, fill=INK)
        d.multiline_text(((x + 24) * S, (y + 128) * S), b, font=body, fill=SUB, spacing=8 * S)
        if i < len(STEPS) - 1:
            ax = (x + bw + 8) * S
            d.polygon([(ax, (y + bh / 2 - 10) * S), (ax + 20 * S, (y + bh / 2) * S), (ax, (y + bh / 2 + 10) * S)],
                      fill=SUB)
    names = ["Root", "Sacral", "Solar plexus", "Heart", "Throat", "Third eye", "Crown"]
    for i, c in enumerate(RGB):  # the seven colours as a band along the bottom, named below
        x0, x1 = 60 + i * 211, 60 + (i + 1) * 211 - 6
        d.rectangle([x0 * S, 420 * S, x1 * S, 440 * S], fill=c)
        d.text(((x0 + x1) / 2 * S, 470 * S), names[i], font=body, fill=SUB, anchor="mm")
    OUT.parent.mkdir(parents=True, exist_ok=True)
    img.resize((W, H), Image.LANCZOS).save(OUT, optimize=True)
    print(f"wrote {OUT.relative_to(HERE)}")


if __name__ == "__main__":
    main()
