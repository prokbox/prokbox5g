#!/usr/bin/env python3
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

# Contact sheet of the shell fonts' glyphs beyond ASCII, drawn from the .fnt
# files the way firmware/font.c draws them (font_remap(): the C1 slots of
# firmware/include/font_shell_map.h, the aliases, the Windows-1252 reading
# of raw C1 code points; anything else out of range is the default glyph).
#
#   apps/ipodui/fonts/fntsheet.py <font dir> <out.png> [scale]
#
# One block per text font (Bold 15, Bold 12, Regular 12): the glyph set at
# 1x (as on the LCD, white on black is not used: black on white) and at
# <scale>x (default 4), then sample strings at 1x. A missing glyph shows as
# the default character (a space), so a gap in the grid is a missing glyph.
import os
import re
import struct
import sys
import zlib

ROOT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..", "..")
FONTS = [("15-ShellSans-Bold.fnt", "Bold 15"), ("12-ShellSans-Bold.fnt", "Bold 12"),
         ("12-ShellSans.fnt", "Regular 12")]

# the characters this pass covers (README.md, "Coverage")
SET = ("♩♪♫♬ "
       "‘’‚“”„‹›«» "
       "–—…•·†‡′″ "
       "©®™°№€£¥¢§¶¡¿ "
       "×÷±½¼¾¹²³‰−←→ "
       "ĸŉŦŧĳĲﬁﬂȘșȚțŒœˆ˜")
SAMPLES = ["♪ Harbour Lights ♪  [MUSIC ♫]  ♩♬",
           "„Grüße“ ‘Ærø’ «Ca» – ﬁnal ﬂow…",
           "Jiří Łódź Ștefan Ţ Dvořák Åæø ığş",
           "€5 £3 ™ © № 5 • † ‡ 6′2″ ½ × ± →",
           "thin space|nbsp |narrow nbsp|en space"]


def load_map():
    t = open(os.path.join(ROOT, "firmware/include/font_shell_map.h"), encoding="utf-8").read()

    def table(name):
        body = re.search(name + r"\[[^]]*\]\s*=\s*{(.*?)};", t, re.S).group(1)
        body = re.sub(r"/\*.*?\*/", "", body, flags=re.S)
        return [int(x, 16) for x in re.findall(r"0x[0-9A-Fa-f]+|\b0\b", body)]
    slot = table("font_shell_slot")
    cp = table("font_shell_cp1252")
    al = table(r"font_shell_alias\[\]")
    alias = dict(zip(al[0::2], al[1::2]))
    assert len(slot) == 32 and len(cp) == 32 and alias, "map parse"
    return slot, cp, alias


SLOT, CP1252, ALIAS = load_map()


def remap(f, c):
    """firmware/font.c font_remap()"""
    if c < 0x80 or f["first"] > 0x80 or f["first"] + f["size"] > 0x250:
        return c
    if c <= 0x9f:
        c = CP1252[c - 0x80]
        if not c:
            return 0xffff
    if c <= 0x24f:
        return c
    if c in ALIAS:
        c = ALIAS[c]
        if c <= 0x24f and not 0x80 <= c <= 0x9f:
            return c
    if c in SLOT:
        return 0x80 + SLOT.index(c)
    return c


def load(path):
    d = open(path, "rb").read()
    mw, h, asc, depth, first, dflt, size, nbits, noff, nw = \
        struct.unpack("<4H6I", d[4:36])
    o = 36 + nbits
    if nbits >= 0xFFDB:
        o = (o + 3) & ~3
        offs = struct.unpack("<%dI" % noff, d[o:o + 4 * noff])
        o += 4 * noff
    else:
        o = (o + 1) & ~1
        offs = struct.unpack("<%dH" % noff, d[o:o + 2 * noff])
        o += 2 * noff
    return dict(h=h, first=first, dflt=dflt, size=size, offs=offs,
                wid=d[o:o + nw], bits=d[36:36 + nbits])


def glyph(f, c):
    """(width, rows of ink 0..15, missing) as font_get_bits draws c"""
    r = remap(f, c)
    if r < f["first"] or r >= f["first"] + f["size"]:
        r = f["dflt"]
    i = r - f["first"]
    d = f["dflt"] - f["first"]
    if r == f["dflt"]:          # out of range, or the default itself
        missing = c != f["dflt"] and ALIAS.get(c) != f["dflt"]
    else:                       # a hole the converter filled with it
        missing = f["offs"][i] == f["offs"][d] and f["wid"][i] == f["wid"][d]
    w, off, h = f["wid"][i], f["offs"][i], f["h"]
    ink = [15 - ((f["bits"][off + k // 2] >> (4 * (k % 2))) & 15) for k in range(w * h)]
    return w, [ink[y * w:(y + 1) * w] for y in range(h)], missing


class Canvas:
    def __init__(self, w, h):
        self.w, self.h = w, h
        self.px = bytearray([255]) * (w * h * 3)

    def dot(self, x, y, v, col=(0, 0, 0)):
        if 0 <= x < self.w and 0 <= y < self.h and v:
            k = (y * self.w + x) * 3
            for j in range(3):
                bg = self.px[k + j]
                self.px[k + j] = (bg * (15 - v) + col[j] * v) // 15

    def rect(self, x, y, w, h, col):
        for yy in range(y, y + h):
            for xx in range(x, x + w):
                if 0 <= xx < self.w and 0 <= yy < self.h:
                    k = (yy * self.w + xx) * 3
                    self.px[k:k + 3] = bytes(col)

    def text(self, f, x, y, s, sc=1, col=(0, 0, 0), mark=False):
        for ch in s:
            w, rows, missing = glyph(f, ord(ch))
            if mark and missing:
                self.rect(x, y, max(w, 2) * sc, f["h"] * sc, (255, 200, 200))
            for yy, row in enumerate(rows):
                for xx, v in enumerate(row):
                    for a in range(sc):
                        for b in range(sc):
                            self.dot(x + xx * sc + b, y + yy * sc + a, v, col)
            x += w * sc
        return x

    def png(self, path):
        raw = b"".join(b"\0" + bytes(self.px[y * self.w * 3:(y + 1) * self.w * 3])
                       for y in range(self.h))

        def chunk(t, d):
            c = struct.pack(">I", len(d)) + t + d
            return c + struct.pack(">I", zlib.crc32(t + d) & 0xffffffff)
        with open(path, "wb") as fp:
            fp.write(b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(
                ">IIBBBBB", self.w, self.h, 8, 2, 0, 0, 0)) +
                chunk(b"IDAT", zlib.compress(raw, 9)) + chunk(b"IEND", b""))


def main():
    if len(sys.argv) < 3:
        sys.exit("usage: fntsheet.py <font dir> <out.png> [scale]")
    sc = int(sys.argv[3]) if len(sys.argv) > 3 else 4
    fonts = [(load(os.path.join(sys.argv[1], n)), label) for n, label in FONTS]
    lab = fonts[2][0]
    W = 1240
    H = 2 * sum(f["h"] * (3 + sc) + 12 + len(SAMPLES) * f["h"] for f, _ in fonts)
    cv = Canvas(W, H)
    y = 6
    total_missing = 0
    for f, label in fonts:
        cv.text(lab, 6, y, "%s  (%s)" % (label, os.path.basename(sys.argv[1].rstrip("/"))),
                col=(60, 60, 160))
        y += lab["h"] + 2
        cv.text(f, 6, y, SET, mark=True)
        y += f["h"] + 4
        x = 6
        for ch in SET:
            if ch == " ":
                x += 3 * sc
                continue
            w, _, missing = glyph(f, ord(ch))
            total_missing += missing
            if x + max(w, 3) * sc > W - 6:
                x = 6
                y += f["h"] * sc + 4
            x = cv.text(f, x, y, ch, sc=sc, mark=True) + sc
        y += f["h"] * sc + 6
        for s in SAMPLES:
            cv.text(f, 6, y, s, mark=True)
            y += f["h"]
        y += 10
    cv.h = min(cv.h, y)
    cv.px = cv.px[:cv.w * cv.h * 3]
    cv.png(sys.argv[2])
    checked = len(fonts) * sum(1 for ch in SET if ch != " ")
    # Latin-1 Supplement and Latin Extended-A in full (not drawn above)
    for f, label in fonts:
        for c in range(0xA1, 0x180):
            checked += 1
            if glyph(f, c)[2]:
                total_missing += 1
                print("%s: U+%04X missing" % (label, c))
    print("%s: %d of %d glyphs of the set missing (pink) over %d fonts" %
          (sys.argv[2], total_missing, checked, len(fonts)))
    assert checked > 0
    return 1 if total_missing else 0


if __name__ == "__main__":
    sys.exit(main())
