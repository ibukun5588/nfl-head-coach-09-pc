#!/usr/bin/env python3
"""Draw the release icon res/hc09-release.ico: an original design (a coach's clipboard with a play diagram and
"HC09"), nothing of EA's art, no NFL or team marks, no real person (2026-10-23). Each size is drawn on its own
at 4x and scaled down: 64 px and up show the whole design, 32-48 px leave out the lettering, 16-24 px keep the
clipboard and the route only, so the small icons stay readable.

    py -3.11 tools/make_icon.py [--out res/hc09-release.ico] [--preview preview.png]

Needs Pillow and Windows' Bahnschrift font (the lettering is drawn into the image; no font ships with the icon).
"""
import argparse
import os

from PIL import Image, ImageDraw, ImageFont

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SIZES = [16, 20, 24, 32, 40, 48, 64, 96, 128, 256]
NAVY_TOP, NAVY_BOTTOM = (31, 69, 128), (9, 28, 58)
SILVER, SILVER_DARK = (198, 206, 218), (120, 132, 150)
PAPER, INK, ROUTE = (250, 251, 253), (14, 37, 74), (232, 86, 48)
FONT = r'C:\Windows\Fonts\bahnschrift.ttf'


def draw(size):
    k = 4
    s = size * k
    img = Image.new('RGBA', (s, s), (0, 0, 0, 0))
    d = ImageDraw.Draw(img)
    detail = 2 if size >= 64 else 1 if size >= 32 else 0
    u = s / 100.0   # one percent of the icon

    # The tile: a navy gradient in a rounded square.
    grad = Image.new('RGBA', (s, s))
    gd = ImageDraw.Draw(grad)
    for y in range(s):
        t = y / (s - 1)
        gd.line([(0, y), (s, y)], fill=tuple(round(a + (b - a) * t) for a, b in zip(NAVY_TOP, NAVY_BOTTOM)) + (255,))
    mask = Image.new('L', (s, s), 0)
    pad = 2 * u if size >= 32 else 0
    ImageDraw.Draw(mask).rounded_rectangle([pad, pad, s - pad, s - pad], radius=18 * u, fill=255)
    img.paste(grad, (0, 0), mask)

    # The clipboard: a silver board, the white sheet, the clip on top.
    bx0, bx1, by0, by1 = (20, 80, 14, 94) if detail else (16, 84, 12, 96)
    d.rounded_rectangle([bx0 * u, by0 * u, bx1 * u, by1 * u], radius=7 * u, fill=SILVER)
    m = 5 if detail else 6
    px0, px1, py0, py1 = bx0 + m, bx1 - m, by0 + 8, by1 - m
    d.rounded_rectangle([px0 * u, py0 * u, px1 * u, py1 * u], radius=2.5 * u, fill=PAPER)
    cw = 26 if detail else 32
    d.rounded_rectangle([(50 - cw / 2) * u, (by0 - 6) * u, (50 + cw / 2) * u, (by0 + 9) * u], radius=4 * u, fill=SILVER_DARK)
    if detail:
        d.ellipse([46 * u, (by0 - 3.5) * u, 54 * u, (by0 + 2.5) * u], fill=NAVY_BOTTOM)

    # The play: line of scrimmage, the offense (O) and the defense (X), one receiver's route.
    w = (2.2 if detail == 2 else 3.2 if detail else 5) * u
    los = 46 if detail == 2 else 56
    if detail:
        x = px0 + 3
        while x < px1 - 3:   # dashed line of scrimmage
            d.line([(x * u, los * u), (min(x + 4, px1 - 3) * u, los * u)], fill=SILVER_DARK, width=round(w * 0.6))
            x += 7
        r = 4.2 if detail == 2 else 5.5
        for ox in ((34, 50, 66) if detail == 2 else (36, 64)):
            d.ellipse([(ox - r) * u, (los + 4) * u, (ox + r) * u, (los + 4 + 2 * r) * u], outline=INK, width=round(w))
        for xx, yy in (((34, 34), (50, 34)) if detail == 2 else ((40, 41),)):
            e = r * 0.85
            d.line([((xx - e) * u, (yy - e) * u), ((xx + e) * u, (yy + e) * u)], fill=INK, width=round(w))
            d.line([((xx - e) * u, (yy + e) * u), ((xx + e) * u, (yy - e) * u)], fill=INK, width=round(w))
        # a corner route: the right receiver runs up the field, then breaks to the outside, clear of the X's
        pts = [(66, los + 3), (66, los - 9), (71.5, los - 18)] if detail == 2 else \
              [(64, los + 2), (64, los - 10), (70.5, los - 21)]
    else:
        pts = [(36, 82), (40, 56), (64, 34)]
    pts = [(x * u, y * u) for x, y in pts]
    d.line(pts, fill=ROUTE, width=round(w * 1.15), joint='curve')
    # the arrowhead along the last segment
    (x1, y1), (x2, y2) = pts[-2], pts[-1]
    vx, vy = x2 - x1, y2 - y1
    n = (vx * vx + vy * vy) ** 0.5
    vx, vy = vx / n, vy / n
    h = (7 if detail == 2 else 9 if detail else 14) * u
    tip = (x2 + vx * h * 0.5, y2 + vy * h * 0.5)
    d.polygon([tip, (tip[0] - vx * h - vy * h * 0.6, tip[1] - vy * h + vx * h * 0.6),
               (tip[0] - vx * h + vy * h * 0.6, tip[1] - vy * h - vx * h * 0.6)], fill=ROUTE)

    # "HC09" on the sheet.
    if detail == 2:
        f = ImageFont.truetype(FONT, round(19 * u))
        try:
            f.set_variation_by_name('Bold')
        except Exception:
            pass
        d.text((50 * u, 79 * u), 'HC09', font=f, fill=INK, anchor='mm')
    return img.resize((size, size), Image.LANCZOS)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--out', default=os.path.join(ROOT, 'res', 'hc09-release.ico'))
    ap.add_argument('--preview', help='also a PNG: every size, on light and dark backgrounds')
    a = ap.parse_args()
    imgs = {n: draw(n) for n in SIZES}
    imgs[256].save(a.out, format='ICO', sizes=[(n, n) for n in SIZES], append_images=[imgs[n] for n in SIZES[:-1]])
    print(f'{a.out}: {", ".join(map(str, SIZES))} px')
    if a.preview:
        gap, x = 16, 16
        sheet = Image.new('RGBA', (sum(SIZES) + gap * (len(SIZES) + 1), 2 * (256 + 2 * gap)), (240, 240, 240, 255))
        ImageDraw.Draw(sheet).rectangle([0, 256 + 2 * gap, sheet.width, sheet.height], fill=(32, 32, 32, 255))
        for n in SIZES:
            for row in (0, 1):
                sheet.alpha_composite(imgs[n], (x, row * (256 + 2 * gap) + gap + 256 - n))
            x += n + gap
        sheet.save(a.preview)


if __name__ == '__main__':
    main()
