#***************************************************************************
#               __________                   __    __________
#    Open       \______   \_______    ____  |  | __\______   \  ____  ___  ___
#    Source      |     ___/\_  __ \  /  _ \ |  |/ / |    |  _/ /  _ \ \  \/  /
#    Player      |    |     |  | \/ (  <_> )|    <  |    |   \(  <_> ) >    <
#    Firmware    |____|     |__|     \____/ |__|_ \ |______  / \____/ /__/\_ \
#                                                \/        \/               \/
#
# Custom firmware for the iPod Video, 5th generation (5G and 5.5G).
# Not affiliated with or endorsed by Apple Inc.
#
# Copyright (C) 2026 prokopto-dev
#
# This program is free software; you can redistribute it and/or
# modify it under the terms of the GNU General Public License
# as published by the Free Software Foundation; either version 2
# of the License, or (at your option) any later version.
#
# This software is distributed on an "AS IS" basis, WITHOUT WARRANTY OF ANY
# KIND, either express or implied.
#
#***************************************************************************

"""Crafted JPEGs for tests/jpegsafe/run.sh (B4, upstream jpeg_load fixes).

    craft.py OUTDIR

Writes, from one small baseline JPEG drawn here (PIL, fixed seed):

    base.jpg      the valid picture (control: must decode, 64x48)
    dht_over.jpg  an extra DHT before SOS redefining AC table 0 with 162
                  codes of length 1 (the AC length check allows 162):
                  fix_huff_tbl() runs past its lookahead table, ~80 KB
                  (upstream 6b6642fcbe)
    samp0.jpg     the luma sampling byte 0x02 (H = 0, V = 2): fix_headers()
                  divides by 8 * H (upstream 473b827ea1)
    neglen.jpg    an extra DHT after SOF0 whose segment length (20) covers
                  only the first 2 of its table's 162 symbols: the skip of
                  what is left is -161 bytes, and parsing goes on from
                  inside the symbols, which hold a fake SOF0 of 4000x4000
                  (upstream 3be791bc1d)
    base.ogg      the valid picture as a Vorbis METADATA_BLOCK_PICTURE
                  (control for the base64 path)
    spin.ogg      as base.ogg with an APP1 inserted after SOI whose length
                  (0xfff0) runs past the end of the picture: the base64
                  reader's skip refills to nothing for ever (upstream
                  162241602a)
"""

import io
import os
import random
import struct
import sys

from PIL import Image

out = sys.argv[1]
os.makedirs(out, exist_ok=True)
here = os.path.dirname(os.path.abspath(__file__))

random.seed(4)
im = Image.new("RGB", (64, 48))
px = im.load()
for y in range(48):
    for x in range(64):
        px[x, y] = ((x * 4) & 255, (y * 5) & 255, random.randrange(256))
b = io.BytesIO()
im.save(b, "JPEG", quality=85, subsampling=2, optimize=False,
        progressive=False)
base = b.getvalue()


def segments(d):
    # (marker, offset of 0xFF, total length) up to and including SOS
    assert d[:2] == b"\xff\xd8"
    p, seg = 2, []
    while True:
        assert d[p] == 0xFF
        m = d[p + 1]
        ln = struct.unpack(">H", d[p + 2:p + 4])[0]
        seg.append((m, p, 2 + ln))
        if m == 0xDA:
            return seg
        p += 2 + ln


seg = segments(base)
sos = [s for s in seg if s[0] == 0xDA][0][1]
sof = [s for s in seg if s[0] == 0xC0][0]


def dht(cls_idx, counts, symbols, length=None):
    body = bytes([cls_idx]) + bytes(counts) + bytes(symbols)
    ln = length if length is not None else 2 + len(body)
    return b"\xff\xc4" + struct.pack(">H", ln) + body


files = {"base.jpg": base}

# 162 codes of length 1 for AC table 0
files["dht_over.jpg"] = (base[:sos] + dht(0x10, [162] + [0] * 15, range(162))
                         + base[sos:])

# luma sampling 0x02: SOF0 = FF C0 len(2) P(1) Y(2) X(2) Nf(1), comp 1 at +10
d = bytearray(base)
d[sof[1] + 10 + 1] = 0x02
files["samp0.jpg"] = bytes(d)

# a valid table (162 codes of length 8) in a segment that covers 2 of its
# symbols; the symbols open with a fake SOF0 the old parser lands in
fake = (b"\xff\xc0" + struct.pack(">HBHHB", 11, 8, 4000, 4000, 1)
        + bytes([1, 0x11, 0]))
syms = bytes(2) + fake
syms += bytes(162 - len(syms))
after_sof = sof[1] + sof[2]
files["neglen.jpg"] = (base[:after_sof]
                       + dht(0x10, [0] * 7 + [162] + [0] * 8, syms,
                             length=2 + 1 + 16 + 2)
                       + base[after_sof:])

for n, data in files.items():
    open(os.path.join(out, n), "wb").write(data)

# the Ogg pictures, by the Ogg art fixtures' own writer (its definitions
# only: the module builds its own fixtures at the end, which is cut off)
spin = base[:2] + b"\xff\xe1" + struct.pack(">H", 0xFFF0) + b"Exif\0\0" + base[2:]
src = open(os.path.join(here, "..", "fixtures",
                        "make_ogg_art_fixtures.py")).read()
cut = src.index("\nsmall, s_side = cover(")
oggfix = {"__name__": "oggfix", "__file__": os.path.join(out, "x.py")}
saved = sys.argv
sys.argv = [saved[0], out]
exec(compile(src[:cut], "make_ogg_art_fixtures.py", "exec"), oggfix)
sys.argv = saved
oggfix["build"]("base", "vorbis", base, 64, 255)
oggfix["build"]("spin", "vorbis", spin, 64, 255)
