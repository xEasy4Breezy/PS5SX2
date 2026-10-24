#!/usr/bin/env python3
# PS5 port frontend: the pictures test-coverfit.sh decodes (2.11, AI-assisted): covers, a back and spines of several shapes, each with a red frame and a
# yellow circle (a circle that stays round shows the picture was not stretched), and disc art as a round, an elliptical, a margined and a double disc.
# Needs Pillow.
#
#   ps5/frontend/host/make_fit_fixtures.py <output dir>
#
# Copyright (C) 2026 swordpdf
# SPDX-License-Identifier: GPL-3.0-or-later
import os
import sys

from PIL import Image, ImageDraw

d = sys.argv[1]
os.makedirs(d, exist_ok=True)


def marked(w, h, base=(30, 90, 160)):
    im = Image.new("RGB", (w, h), base)
    dr = ImageDraw.Draw(im)
    dr.rectangle([0, 0, w - 1, h - 1], outline=(255, 0, 0), width=max(4, min(w, h) // 60))
    r = min(w, h) * 0.38
    dr.ellipse([w / 2 - r, h / 2 - r, w / 2 + r, h / 2 + r], outline=(255, 255, 0), width=max(4, min(w, h) // 40))
    return im


marked(712, 1000).save(d + "/front_ok.jpg", quality=92)
marked(2000, 2800).save(d + "/front_big_ok.jpg", quality=90)
marked(500, 1000).save(d + "/front_tall.png")
marked(1000, 1000).save(d + "/front_square.png")
marked(1600, 900).save(d + "/front_wide.png")
marked(4096, 1024).save(d + "/front_banner.png")
marked(640, 1000).save(d + "/front_a064.png")
marked(760, 1000).save(d + "/front_crop.png")
marked(1500, 2100).save(d + "/back_big.jpg", quality=90)
marked(175, 2151, (150, 90, 20)).save(d + "/spine_ok.png")
marked(300, 1000, (150, 90, 20)).save(d + "/spine_wide.png")


def disc(path, w, h, margin=0.0):
    im = Image.new("RGBA", (w, h), (0, 0, 0, 0))
    dr = ImageDraw.Draw(im)
    mx, my = w * margin, h * margin
    dr.ellipse([mx, my, w - 1 - mx, h - 1 - my], fill=(236, 232, 246, 255))
    dr.ellipse([w * 0.4, h * 0.4, w * 0.6, h * 0.6], fill=(0, 0, 0, 0))
    im.save(path)


disc(d + "/disc_round.png", 700, 700)
disc(d + "/disc_ellipse.png", 900, 700)
disc(d + "/disc_margin.png", 1000, 1000, 0.15)
disc(d + "/disc_pair.png", 1400, 700)
# a disc photographed on an opaque background (no transparent surround): 900x700, the disc round inside it. Not an elliptical scan: its proportions are kept.
rect = Image.new("RGBA", (900, 700), (250, 250, 250, 255))
ImageDraw.Draw(rect).ellipse([100, 0, 799, 699], fill=(236, 232, 246, 255), outline=(40, 70, 150, 255), width=12)
rect.save(d + "/disc_rect.png")
print("ok")
