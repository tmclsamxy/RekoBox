# -*- coding: utf-8 -*-
"""生成 RekoBox 启动图标：legacy ic_launcher.png + adaptive ic_launcher_foreground.png"""
import os
from PIL import Image, ImageDraw, ImageFont

FONT = "C:/Windows/Fonts/arialbd.ttf"
RES = "app/src/main/res"
DENSITIES = {"mdpi": 1.0, "hdpi": 1.5, "xhdpi": 2.0, "xxhdpi": 3.0, "xxxhdpi": 4.0}
SS = 4

BG_TOP, BG_BOT = (11, 18, 32), (16, 58, 84)
ACCENT, WHITE = (34, 211, 238), (255, 255, 255)


def gradient(size):
    img = Image.new("RGB", (size, size))
    d = ImageDraw.Draw(img)
    for y in range(size):
        t = y / max(1, size - 1)
        d.line([(0, y), (size, y)],
               fill=tuple(int(BG_TOP[i] + (BG_BOT[i] - BG_TOP[i]) * t) for i in range(3)))
    return img


def draw_glyph(img, size, ratio=1.0):
    d = ImageDraw.Draw(img)
    font = ImageFont.truetype(FONT, int(size * 0.52 * ratio))
    l, t, r, b = d.textbbox((0, 0), "R", font=font)
    tw, th = r - l, b - t
    x, y = (size - tw) / 2 - l, (size - th) / 2 - t - size * 0.04
    d.text((x, y), "R", font=font, fill=WHITE)
    bar_w, bar_h = int(size * 0.34 * ratio), max(2, int(size * 0.032 * ratio))
    bx, by = (size - bar_w) / 2, y + th + size * 0.14
    d.rounded_rectangle([bx, by, bx + bar_w, by + bar_h], radius=bar_h / 2, fill=ACCENT)
    return img


def rounded_mask(size, ratio=0.22):
    m = Image.new("L", (size, size), 0)
    ImageDraw.Draw(m).rounded_rectangle([0, 0, size - 1, size - 1],
                                        radius=int(size * ratio), fill=255)
    return m


for dens, scale in DENSITIES.items():
    out = os.path.join(RES, "mipmap-" + dens)
    os.makedirs(out, exist_ok=True)

    px = int(48 * scale)
    big = draw_glyph(gradient(px * SS), px * SS).convert("RGBA")
    big.putalpha(rounded_mask(px * SS))
    big.resize((px, px), Image.LANCZOS).save(os.path.join(out, "ic_launcher.png"))

    fpx = int(108 * scale)
    fg = draw_glyph(Image.new("RGBA", (fpx * SS, fpx * SS), (0, 0, 0, 0)), fpx * SS, 0.78)
    fg.resize((fpx, fpx), Image.LANCZOS).save(os.path.join(out, "ic_launcher_foreground.png"))

print("icons generated")
