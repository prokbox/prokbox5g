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

# Compare two sets of shell fonts (.fnt, RB12, 4-bit) after a regeneration:
# headers, per-glyph advances, cap height / x-height / line height, the
# glyphs whose pixels changed, and ink clipped at the top of the cell.
# Optionally writes glyph sheets (PNG at 4x, one per font and side) for viewing.
#
#   apps/ipodui/fonts/fntcheck.py <before dir> <after dir> [sheet dir]
#
# Exit status 1 if any metric differs. Pixel changes are listed, not
# failures: they are what a regeneration is for.
import os
import struct
import sys
import zlib

FONTS = ["15-ShellSans-Bold.fnt", "12-ShellSans-Bold.fnt",
         "12-ShellSans.fnt", "35-ShellSans-Letters.fnt"]


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
    return dict(dflt=dflt, hdr=(d[:4], mw, h, asc, depth, first, dflt, size),
                h=h, asc=asc, first=first, size=size, offs=offs,
                wid=d[o:o + nw], bits=d[36:36 + nbits], bytes=len(d))


def glyph(f, code):
    i = code - f["first"]
    w, off, h = f["wid"][i], f["offs"][i], f["h"]
    ink = []
    for k in range(w * h):
        ink.append(15 - ((f["bits"][off + k // 2] >> (4 * (k % 2))) & 15))
    return w, [ink[r * w:(r + 1) * w] for r in range(h)]


def ink_rows(f, code, thr=8):
    """Rows (top, bottom) with a pixel at >= 50 % ink."""
    w, rows = glyph(f, code)
    lit = [r for r, row in enumerate(rows) if max(row or [0]) >= thr]
    return (lit[0], lit[-1]) if lit else None


def metrics(f):
    m = {"line height": f["h"], "ascent": f["asc"]}
    for name, c in (("cap (H)", "H"), ("cap (I)", "I"), ("x-height (x)", "x"),
                    ("x-height (n)", "n")):
        if f["first"] <= ord(c) < f["first"] + f["size"]:
            r = ink_rows(f, ord(c))
            m[name] = r[1] - r[0] + 1 if r else 0
    return m


def sheet(f, path, codes):
    cols = 32
    cw = max(f["wid"][c - f["first"]] for c in codes) + 2
    ch = f["h"] + 2
    rows = (len(codes) + cols - 1) // cols
    W, H = cols * cw, rows * ch
    img = bytearray([255] * (W * H))
    for n, c in enumerate(codes):
        w, g = glyph(f, c)
        x0, y0 = (n % cols) * cw + 1, (n // cols) * ch + 1
        for y in range(f["h"]):
            img[(y0 + y) * W + x0 - 1] = 200  # cell edge
            for x in range(w):
                img[(y0 + y) * W + x0 + x] = 255 - g[y][x] * 17
        img[(y0 + f["asc"]) * W + x0 - 1] = 0     # baseline tick
    write_png(path, img, W, H, 4)


def write_png(path, img, W, H, z):
    """8-bit grey PNG, each pixel z x z (nearest neighbour)."""
    raw = bytearray()
    for y in range(H * z):
        row = img[(y // z) * W:(y // z + 1) * W]
        raw.append(0)
        raw.extend(bytes(v for v in row for _ in range(z)))

    def chunk(t, d):
        c = t + d
        return struct.pack(">I", len(d)) + c + struct.pack(
            ">I", zlib.crc32(c) & 0xffffffff)
    with open(path, "wb") as fp:
        fp.write(b"\x89PNG\r\n\x1a\n")
        fp.write(chunk(b"IHDR", struct.pack(">IIBBBBB", W * z, H * z,
                                            8, 0, 0, 0, 0)))
        fp.write(chunk(b"IDAT", zlib.compress(bytes(raw), 9)))
        fp.write(chunk(b"IEND", b""))


def main():
    a_dir, b_dir = sys.argv[1], sys.argv[2]
    out = sys.argv[3] if len(sys.argv) > 3 else None
    bad = 0
    for name in FONTS:
        a, b = load(os.path.join(a_dir, name)), load(os.path.join(b_dir, name))
        print("%s: %d -> %d bytes (%+d)" % (name, a["bytes"], b["bytes"],
                                            b["bytes"] - a["bytes"]))
        # code points that were the default glyph before and are real
        # glyphs now (new coverage) are listed, not compared
        da, db = a["offs"][a["dflt"] - a["first"]], b["offs"][b["dflt"] - b["first"]]
        new = [b["first"] + i for i in range(min(a["size"], b["size"]))
               if a["offs"][i] == da and b["offs"][i] != db
               and b["first"] + i != b["dflt"]]
        old_mw = max(b["wid"][i] for i in range(b["size"])
                     if b["first"] + i not in new)
        if a["hdr"][:6] != b["hdr"][:6] and not (
                a["hdr"][2:6] == b["hdr"][2:6] and old_mw == a["hdr"][1]):
            print("  header differs: %s -> %s" % (a["hdr"], b["hdr"]))
            bad = 1
        ma, mb = metrics(a), metrics(b)
        for k in ma:
            flag = "" if ma[k] == mb.get(k) else "   <-- DIFFERS"
            bad |= bool(flag)
            print("  %-13s %3d -> %3d%s" % (k, ma[k], mb.get(k, -1), flag))
        n = min(a["size"], b["size"])
        adv = [a["first"] + i for i in range(n)
               if a["wid"][i] != b["wid"][i] and a["first"] + i not in new]
        if new:
            print("  new glyphs: %s (maxwidth %d -> %d)" % (
                " ".join("U+%04X" % c for c in new), a["hdr"][1], b["hdr"][1]))
        print("  advances changed: %d %s" % (len(adv),
              " ".join("U+%04X" % c for c in adv)))
        bad |= bool(adv)
        if b["size"] != a["size"]:
            print("  range U+%04X..U+%04X -> U+%04X..U+%04X" % (
                a["first"], a["first"] + a["size"] - 1,
                b["first"], b["first"] + b["size"] - 1))
        changed, clipped = [], []
        for i in range(b["size"]):
            c = b["first"] + i
            if i < n and c not in new and glyph(a, c) != glyph(b, c):
                changed.append(c)
            if any(glyph(b, c)[1][0]) and c != 0x20:
                clipped.append(c)
        print("  glyphs changed: %d %s" % (len(changed), "".join(
            chr(c) for c in changed)))
        if clipped:
            print("  ink on the cell's top row: %s" % "".join(
                chr(c) for c in clipped))
        if out and changed + new:
            base = os.path.join(out, name[:-4])
            sheet(a, base + "-before.png", changed + new)
            sheet(b, base + "-after.png", changed + new)
    print("METRICS UNCHANGED" if not bad else "METRICS DIFFER")
    return bad


if __name__ == "__main__":
    sys.exit(main())
