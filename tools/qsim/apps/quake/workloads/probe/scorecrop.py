#------------------------------------------------------------------------------
# SPDX-License-Identifier: Apache-2.0
# SPDX-FileType: SOURCE
# SPDX-FileCopyrightText: (c) 2026, ThinkElastic <Think@Elastic.com>
#------------------------------------------------------------------------------

import sys, os
from PIL import Image, ImageDraw
runs = sys.argv[2:]
tiles = []
for r in runs:
    ppms = sorted(os.listdir(os.path.join(r, "ppm")))
    im = Image.open(os.path.join(r, "ppm", ppms[-1])).convert("RGB")
    w, h = im.size
    c = im.crop((0, h - 22, 160, h - 11))
    t = Image.new("RGB", (230, 11))
    t.paste(c, (70, 0))
    ImageDraw.Draw(t).text((1, 0), r[-12:], fill=(255, 255, 255))
    tiles.append(t)
cols = 4
rows = (len(tiles) + cols - 1) // cols
out = Image.new("RGB", (cols * 232, rows * 13))
for i, t in enumerate(tiles):
    out.paste(t, ((i % cols) * 232, (i // cols) * 13))
out = out.resize((out.width * 2, out.height * 2))
out.save(sys.argv[1])
