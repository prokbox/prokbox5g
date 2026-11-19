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

"""ShellCJK companion fonts (#235, #266): the sparse form.

    apps/ipodui/fonts/gen_cjk.py <dir with NotoSansCJK{jp,sc,tc}-{Bold,Regular}.otf> <outdir>

Writes 14-ShellCJK-Bold.fnt (companion of 15-ShellSans-Bold),
11-ShellCJK-Bold.fnt (of 12-ShellSans-Bold) and 11-ShellCJK.fnt (of
12-ShellSans) in the sparse form of firmware/export/font_sparse.h: the
glyphs of cjk-cut-A.txt inside its three windows, a presence bitmap, a
count every 256 codes and fixed-stride records (4-bit anti-aliased), so
a glyph miss is one read of one record. Each file takes the HEIGHT and
ASCENT of its ShellSans partner, so firmware/font.c draws a glyph of
either font on the same line (the companion path, HAVE_IPOD_UI). All
three hold the same set, so they share one resident bitmap.

Also 35-ShellCJK-Index.fnt, the companion of 35-ShellSans-Letters for the
quick-scroll letter cards (design 4.2): the 10 gojuon row heads and the
14 basic Hangul initials, in the stock in-memory RB12 form (24 glyphs).

Source: Noto Sans CJK JP 2.004 (SIL OFL 1.1), from the noto-cjk
project's Sans/OTF/Japanese; the 2,065 GB2312/Big5 level 1 hanzi outside
cut A come from the SC (GB2312) and TC (Big5-only) faces of the same
release, so they have the Chinese shapes (cjk_cut.region_of()). "Noto" is a reserved name, so the output is
called ShellCJK. Needs Pillow with FreeType (python3 -m pip install pillow).
"""
import os
import struct
import sys
import zlib

from PIL import Image, ImageDraw, ImageFont

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import cjk_cut  # noqa: E402

# Basic layout on every machine: with libraqm present (Debian's Pillow always
# has it) the combining marks U+302A-302F and U+3099/309A measure wide, so the
# same sources gave other advances and twice the file size. Pinning BASIC
# gives byte-identical fonts wherever this runs (release toolchain r2 and the Mac).
LAYOUT = ImageFont.Layout.BASIC

# firmware/export/font_sparse.h
SPARSE_DEPTH = 0x100
FIRST, SIZE = 0x3000, 0xFFF0 - 0x3000
WINDOWS = ((0x3000, 0xD7A4, 0), (0xF900, 0xFB00, 42916),
           (0xFF00, 0xFFF0, 43428))
NBITS, BITMAP, BLOCKS = 43668, 5460, 172
MAGIC = 0x53524853
DATA = 52 + BITMAP + 2 * BLOCKS
DEFAULT = 0x3000                      # ideographic space: blank

# (output, source face, pixel size, ShellSans partner)
FONTS = (
    ('14-ShellCJK-Bold.fnt', 'NotoSansCJKjp-Bold.otf', 14, '15-ShellSans-Bold.fnt'),
    ('11-ShellCJK-Bold.fnt', 'NotoSansCJKjp-Bold.otf', 11, '12-ShellSans-Bold.fnt'),
    ('11-ShellCJK.fnt', 'NotoSansCJKjp-Regular.otf', 11, '12-ShellSans.fnt'),
)
# the letter cards (design 4.2): gojuon row heads, basic Hangul initials
INDEX = ('35-ShellCJK-Index.fnt', 'NotoSansCJKjp-Bold.otf', 30,
         '35-ShellSans-Letters.fnt')
INDEX_CHARS = ('\u3042\u304b\u3055\u305f\u306a\u306f\u307e\u3084\u3089\u308f'
               '\u3131\u3134\u3137\u3139\u3141\u3142\u3145\u3147\u3148'
               '\u314a\u314b\u314c\u314d\u314e')


def bit_of(c):
    for lo, hi, base in WINDOWS:
        if lo <= c < hi:
            return base + c - lo
    return -1


def glyph_bytes(height, width):
    """firmware/font.c glyph_bytes() for a 4-bit font"""
    return (((height * width + 1) // 2) + 1) & ~1


def partner_metrics(path):
    d = open(path, 'rb').read(36)
    _maxw, height, ascent = struct.unpack('<3H', d[4:10])
    return height, ascent


def render(face, ch, height, ascent):
    adv = int(round(face.getlength(ch)))
    if adv <= 0:
        return 0, b''
    img = Image.new('L', (adv, height), 0)
    ImageDraw.Draw(img).text((0, ascent), ch, font=face, fill=255,
                             anchor='ls')
    px = img.tobytes()
    out = bytearray((len(px) + 1) // 2)
    for k, a in enumerate(px):
        v = 15 - (a * 15 + 127) // 255          # 15 = paper, 0 = full ink
        if k & 1:
            out[k // 2] |= v << 4
        else:
            out[k // 2] |= v
    return adv, bytes(out)


def build(src, out, size, partner, cut):
    height, ascent = partner_metrics(partner)
    # the Chinese-only hanzi come from the SC/TC faces of the same weight
    faces = {'jp': ImageFont.truetype(src, size, layout_engine=LAYOUT)}
    for reg in ('sc', 'tc'):
        faces[reg] = ImageFont.truetype(src.replace('CJKjp', 'CJK' + reg), size,
                                         layout_engine=LAYOUT)
    glyphs = {}
    for c in sorted(cut):
        b = bit_of(c)
        if b < 0:
            continue                    # outside the windows: ShellSans's
        w, g = render(faces[cjk_cut.region_of(c)], chr(c), height, ascent)
        if c == DEFAULT:
            w = size
            g = bytes([0xFF]) * ((size * height + 1) // 2)
        if w == 0:
            continue                    # not in the face: stays a hole
        glyphs[b] = (w, g)
    maxw = max(w for w, _ in glyphs.values())
    stride = 1 + glyph_bytes(height, maxw)
    bitmap = bytearray(BITMAP)
    for b in glyphs:
        bitmap[b >> 3] |= 1 << (b & 7)
    counts, n = [], 0
    for blk in range(BLOCKS):
        counts.append(n)
        n += sum(1 for b in range(blk * 256, blk * 256 + 256) if b in glyphs)
    recs = bytearray()
    for b in sorted(glyphs):
        w, g = glyphs[b]
        rec = bytes([w]) + g
        recs += rec + bytes(stride - len(rec))
    count = len(glyphs)
    hdr = struct.pack('<4s4H6I', b'RB12', maxw, height, ascent,
                      1 | SPARSE_DEPTH, FIRST, DEFAULT, SIZE,
                      count * stride, 0, 0)
    sub = struct.pack('<3I', MAGIC, count, stride)
    font_id = zlib.crc32(hdr + sub + bytes(bitmap) + bytes(recs)) & 0xffffffff
    body = (hdr + sub + struct.pack('<I', font_id) + bytes(bitmap)
            + struct.pack('<%dH' % BLOCKS, *counts))
    assert len(body) == DATA
    body += recs
    open(out, 'wb').write(body)
    print('%s: %d glyphs, height %d ascent %d maxwidth %d stride %d, '
          'id %08x, %d B' % (os.path.basename(out), count, height, ascent,
                             maxw, stride, font_id, len(body)))


def render_card(face, ch, height, ascent):
    """a letter-card glyph: ink bottom on the baseline, top clipped to
    the cell (the Letters font is caps only: 1 row below the baseline)"""
    adv = int(round(face.getlength(ch)))
    img = Image.new('L', (adv, height * 2), 0)
    ImageDraw.Draw(img).text((0, height), ch, font=face, fill=255,
                             anchor='ls')
    top, bottom = img.getbbox()[1], img.getbbox()[3]
    y0 = bottom - (ascent + 1) if bottom - top <= ascent + 1 else top
    img = img.crop((0, y0, adv, y0 + height))
    px = img.tobytes()
    out = bytearray((len(px) + 1) // 2)
    for k, a in enumerate(px):
        v = 15 - (a * 15 + 127) // 255
        out[k // 2] |= v << 4 if k & 1 else v
    return adv, bytes(out)


def build_index(src, out, size, partner):
    """the letter cards' companion: stock in-memory RB12 over U+3042..
    U+314E, holes drawing the default (blank) glyph"""
    height, ascent = partner_metrics(partner)
    face = ImageFont.truetype(src, size, layout_engine=LAYOUT)
    first, last = 0x3042, 0x314E
    n = last - first + 1
    bits = bytearray()
    offs, wids = [0] * n, [0] * n
    blank_w = size // 2
    bits += bytes([0xFF]) * glyph_bytes(height, blank_w)
    wids = [blank_w] * n
    maxw = blank_w
    for ch in INDEX_CHARS:
        w, g = render_card(face, ch, height, ascent)
        g += bytes(glyph_bytes(height, w) - len(g))
        offs[ord(ch) - first] = len(bits)
        wids[ord(ch) - first] = w
        bits += g
        maxw = max(maxw, w)
    hdr = struct.pack('<4s4H6I', b'RB12', maxw, height, ascent, 1, first,
                      first, n, len(bits), n, n)
    body = bytearray(hdr) + bits
    body += struct.pack('<%dH' % n, *offs)
    body += bytes(wids)
    open(out, 'wb').write(body)
    print('%s: %d glyphs, height %d ascent %d maxwidth %d, %d B'
          % (os.path.basename(out), len(INDEX_CHARS), height, ascent, maxw,
             len(body)))


def main():
    if len(sys.argv) != 3:
        print(__doc__)
        return 2
    srcdir, outdir = sys.argv[1], sys.argv[2]
    here = os.path.dirname(os.path.abspath(__file__))
    cut = cjk_cut.read_cut()
    os.makedirs(outdir, exist_ok=True)
    for name, face, size, partner in FONTS:
        build(os.path.join(srcdir, face), os.path.join(outdir, name), size,
              os.path.join(here, partner), cut)
    name, face, size, partner = INDEX
    build_index(os.path.join(srcdir, face), os.path.join(outdir, name), size,
                os.path.join(here, partner))
    return 0


if __name__ == '__main__':
    sys.exit(main())
