#!/usr/bin/env python3
"""make_app_icon.py: make the app icon of the desktop frontend.

The icon is a pixel P on a pale screen in a dark tile. The PokketSX icon has the same
shapes in different colors.

All shapes are on a grid of 64 units. Each edge of the P is a multiple of 4 units. Thus
the P is sharp at 16 pixels and at each larger power of two.

To change the icon, change a shape or a color in this file. Then run the script again.
It writes app-icon.svg and app.ico in frontends/desktop.

    python3 tools/make_app_icon.py

The script needs Pillow.
"""

from pathlib import Path

from PIL import Image, ImageDraw

INK = "#1e2a44"
SCREEN = "#cfe9d8"
LETTER = INK

TILE = (0, 0, 64, 64, 14)  # x0, y0, x1, y1, corner radius
SCREEN_RECT = (8, 8, 56, 56, 6)
# A grid of 3 x 5 blocks of 8 units: the stem, the top bar, the right side, the middle bar.
LETTER_RECTS = [(20, 12, 28, 52), (28, 12, 44, 20), (36, 20, 44, 28), (28, 28, 44, 36)]

ICO_SIZES = [16, 32, 48, 256]
OUT_DIR = Path(__file__).resolve().parents[1] / "frontends" / "desktop"


def svg():
    def rect(r, fill):
        x0, y0, x1, y1 = r[:4]
        radius = f' rx="{r[4]}"' if len(r) > 4 else ""
        return f'  <rect x="{x0}" y="{y0}" width="{x1 - x0}" height="{y1 - y0}"{radius} fill="{fill}"/>\n'

    body = rect(TILE, INK) + rect(SCREEN_RECT, SCREEN) + "".join(rect(r, LETTER) for r in LETTER_RECTS)
    return f'<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 64 64">\n{body}</svg>\n'


def render(size):
    """Draw at 8 times the size, then reduce with a box filter.

    A box filter keeps an edge on a pixel boundary sharp and makes a curve smooth.
    """
    canvas = size * 8
    unit = canvas / 64
    image = Image.new("RGBA", (canvas, canvas), (0, 0, 0, 0))
    draw = ImageDraw.Draw(image)

    def box(r):
        return [round(r[0] * unit), round(r[1] * unit), round(r[2] * unit) - 1, round(r[3] * unit) - 1]

    draw.rounded_rectangle(box(TILE), radius=TILE[4] * unit, fill=INK)
    draw.rounded_rectangle(box(SCREEN_RECT), radius=SCREEN_RECT[4] * unit, fill=SCREEN)
    for r in LETTER_RECTS:
        draw.rectangle(box(r), fill=LETTER)
    return image.resize((size, size), Image.Resampling.BOX)


def main():
    (OUT_DIR / "app-icon.svg").write_text(svg(), encoding="utf-8", newline="\n")
    images = [render(s) for s in ICO_SIZES]
    images[-1].save(OUT_DIR / "app.ico", sizes=[(s, s) for s in ICO_SIZES], append_images=images[:-1])
    print(f"wrote app-icon.svg and app.ico {ICO_SIZES} in {OUT_DIR}")


if __name__ == "__main__":
    main()