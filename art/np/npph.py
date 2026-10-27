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

"""Pre-render the Now Playing placeholders (QW2; stdlib only).

    art/np/npph.py [--c HEADER] [--raw DIR]
        default: apps/ipodui/npph_data.h

Now Playing shows a placeholder card while a track has no art (or its
art is not read yet): the slate-blue square with the note glyph (music)
or the book glyph (audiobooks), slanted and reflected like real art. On
the device that used to be np_placeholder_render_glyph() +
np_art_render() at runtime (~281k instructions per coverless track).
This script runs the SAME integer arithmetic at build time and stores
the composed result (x 9-145, y 33-219 of the screen, 137 x 187,
against white), so the device only decodes it.

It mirrors, operation for operation, apps/ipodui/np_draw.c:
  np_placeholder_render_glyph()  (the 142 px card, glyph upscaled 5/2)
  np_art_render()                (slant, antialiased edges, reflection)
reading the glyph coverage from apps/ipodui/npglyphs.h and the slant
tables from np_draw.c itself, so there is one source for each. The host
test (tests/npscreen/test_draw.c, test_composed) decodes the
header and compares it pixel for pixel with the runtime render; any
change to np_draw.c's render must be followed by re-running this script.

Coding: one palette (<= 256 RGB565 colours, most frequent first) and
PackBits over the palette indices of the whole image in row order
(control byte n < 128: n + 1 literal indices follow; n >= 128: the next
index repeated n - 126 times, 2..129). Target <= 16 KB per glyph
(docs/phase11-brief.md §3.9, Rev 1 M-6); the script fails above it.
"""
import argparse
import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
SRC = os.path.join(ROOT, 'apps', 'ipodui')

M32 = 0xffffffff
MASK = 0x07e0f81f
WHITE = 0xffff
BOX = 142
# np_draw.h (NP_SLANT_*, NP_REFL_*): keep in step; the host test
# (test_composed) fails if they drift
SLANT_X, SLANT_W = 9, 137
TOP_L = 33
BOT_R = 162
REFL_A0, REFL_K_Q8, REFL_LIMIT, REFL_FEATHER = 90, 1280, 220, 12
REFL_LEN = REFL_LIMIT - BOT_R - 1
COMP_X, COMP_Y, COMP_W, COMP_H = 9, 33, 137, 187
LIMIT_BYTES = 16384


def rgb(r, g, b):
    return ((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3)


def spread(p):
    return (p | (p << 16)) & MASK


def blend32(bg, fg, a):
    b = spread(bg)
    f = spread(fg)
    r = (b + ((((f - b) & M32) * a & M32) >> 5)) & M32 & MASK
    return (r | (r >> 16)) & 0xffff


def blend256(bg, fg, a):
    br, bgn, bb = bg >> 11, (bg >> 5) & 63, bg & 31
    fr, fgn, fb = fg >> 11, (fg >> 5) & 63, fg & 31
    br += ((fr - br) * a) >> 8
    bgn += ((fgn - bgn) * a) >> 8
    bb += ((fb - bb) * a) >> 8
    return (br << 11) | (bgn << 5) | bb


def cdiv(a, b):
    """C integer division (truncates toward zero)."""
    q = abs(a) // abs(b)
    return q if (a >= 0) == (b >= 0) else -q


def c_array(text, name):
    m = re.search(r'%s\[[^\]]*\]\s*=\s*\{(.*?)\};' % re.escape(name), text, re.S)
    if not m:
        sys.exit('npph.py: %s not found' % name)
    return [int(v, 0) for v in re.findall(r'0x[0-9a-fA-F]+|\d+', m.group(1))]


def load_sources():
    with open(os.path.join(SRC, 'npglyphs.h')) as f:
        gl = f.read()
    with open(os.path.join(SRC, 'np_draw.c')) as f:
        dr = f.read()
    glyphs = {}
    for gid, sym in (('NPG_NOTE', 'npg_note_cov'), ('NPG_BOOK', 'npg_book_cov')):
        m = re.search(r'\[%s\]\s*=\s*\{\s*(\d+),\s*(\d+),\s*%s' % (gid, sym), gl)
        if not m:
            sys.exit('npph.py: %s not in np_glyphs[]' % gid)
        glyphs[gid] = (int(m.group(1)), int(m.group(2)), c_array(gl, sym))
    tabs = (c_array(dr, 'slant_u'), c_array(dr, 'slant_top'), c_array(dr, 'slant_h'))
    for t in tabs:
        if len(t) != SLANT_W:
            sys.exit('npph.py: slant table size %d' % len(t))
    return glyphs, tabs


def placeholder(glyph, box=BOX):
    """np_placeholder_render_glyph()"""
    gwid, ghei, cov = glyph
    rb = (gwid + 1) // 2
    gw, gh = gwid * 5 // 2, ghei * 5 // 2
    ox, oy = (box - gw) // 2 + 2, (box - gh) // 2
    top, bot = rgb(0x6c, 0x7e, 0x9c), rgb(0x3c, 0x4a, 0x66)
    ink = rgb(0xd6, 0xda, 0xe2)
    art = [0] * (box * box)
    for y in range(box):
        c = blend256(top, bot, y * 256 // box)
        gy = y - oy
        if y < box * 2 // 5:
            c = blend256(c, WHITE, 28 - y * 28 * 5 // (box * 2))
        base = y * box
        for x in range(box):
            art[base + x] = c
        if gy < 0 or gy >= gh:
            continue
        for x in range(gw):
            sxf = ((2 * x + 1) * 16) // 5 - 8
            syf = ((2 * gy + 1) * 16) // 5 - 8
            sx, sy, fx, fy = sxf >> 4, syf >> 4, sxf & 15, syf & 15
            acc = 0
            for j in range(2):
                for i in range(2):
                    px, py, d = sx + i, sy + j, 15
                    wx = fx if i else 16 - fx
                    wy = fy if j else 16 - fy
                    if 0 <= px < gwid and 0 <= py < ghei:
                        b = cov[py * rb + (px >> 1)]
                        d = (b & 15) if (px & 1) else (b >> 4)
                    acc += d * wx * wy
            a = 128 - cdiv(acc * 5 - 9600, 8)
            if a > 256:
                a = 256
            if a > 0:
                art[base + ox + x] = blend32(art[base + ox + x], ink, a >> 3)
    return art


def refl_blend(sb, s, a32):
    s = (sb + ((((s - sb) & M32) * a32 & M32) >> 5)) & M32 & MASK
    return (s | (s >> 16)) & 0xffff


def art_render(out, art, art_w, art_h, tabs, bg=WHITE):
    """np_art_render() into the composed image (screen coordinates)."""
    slant_u, slant_top, slant_h = tabs
    feather = (64, 54, 44, 36, 28, 22, 16, 11, 7, 4, 2, 1)
    fw, fh = art_w, art_h
    fx0, fy0 = (BOX - art_w) // 2, (BOX - art_h) // 2
    cv_w, cv_h = COMP_X + COMP_W, COMP_Y + COMP_H
    sb = spread(bg)
    x16c = REFL_K_Q8 * 65536

    def put(x, y, p):
        out[(y - COMP_Y) * COMP_W + (x - COMP_X)] = p

    for c in range(SLANT_W):
        x = SLANT_X + c
        u, top, h = slant_u[c], slant_top[c], slant_h[c]
        if u < fx0 or u >= fx0 + fw or x >= cv_w:
            continue
        col = u - fx0

        def px(row):
            return art[row * art_w + col]
        y0f = top + (fy0 * h) // BOX
        y1f = top + ((fy0 + fh) * h) // BOX
        y0, y1 = y0f >> 8, (y1f + 255) >> 8
        if y1 - y0 < 2:
            continue
        vstep = (BOX << 24) // h
        vmax = ((fh << 16) - 1) & M32
        t0 = y0 * 256 + 128 - y0f
        v = ((t0 if t0 > 0 else 0) * (vstep >> 8)) & M32
        y = y0
        p = px(v >> 16)
        put(x, y, blend32(bg, p, (256 - (y0f & 255)) >> 3) if (y0f & 255) else p)
        y += 1
        v = (v + vstep) & M32
        n = y1 - y0 - 2
        vv = v
        for _ in range(n):
            put(x, y, px(vv >> 16))
            y += 1
            vv = (vv + vstep) & M32
        v = (v + n * vstep) & M32
        if v > vmax:
            v = vmax
        p = px(v >> 16)
        a0 = (REFL_A0 + 4) >> 3
        put(x, y, blend32(blend32(bg, p, a0), p, (y1f & 255) >> 3) if (y1f & 255) else p)
        y += 1

        rl = REFL_LIMIT - y1
        if rl > REFL_LEN:
            rl = REFL_LEN
        if y1 + rl > cv_h:
            rl = cv_h - y1
        if rl > 0 and (rl * vstep) & M32 > vmax:
            rl = vmax // vstep
        if rl <= 0:
            continue
        x16 = x16c // (y1f - y0f)
        q = (65536 - x16 + ((x16 * x16) >> 17)) & M32
        v = (vmax - (vstep >> 1)) & M32
        blur = (((v - (rl - 1) * vstep) & M32) >> 16) >= 2
        dith = (x + y1) & 1

        # col_reflect()
        a = REFL_A0 << 8
        t = 1536 if dith else 512
        d = 0
        fd = REFL_LIMIT - REFL_FEATHER - y1
        fd = max(0, min(fd, rl))
        d1, d2 = min(rl, 15), min(rl, 29)
        if not blur:
            d1 = d2 = rl
        d1, d2 = min(d1, fd), min(d2, fd)

        def sp(row):
            return spread(px(row))
        while d < rl:
            r = v >> 16
            if d < d1:
                s, a32 = sp(r), (a + t) >> 11
            elif d < d2:
                s = ((2 * sp(r) + sp(r - 1) + sp(r + 1)) >> 2) & MASK
                a32 = (a + t) >> 11
            elif d < fd:
                s = ((2 * (sp(r) + sp(r - 1) + sp(r + 1)) + sp(r - 2)
                      + sp(r + 2)) >> 3) & MASK
                a32 = (a + t) >> 11
            else:
                s, a32 = sp(r), (((a + t) * feather[d - fd]) & M32) >> 17
            put(x, y, refl_blend(sb, s, a32))
            y += 1
            d += 1
            v = (v - vstep) & M32
            a = ((a * q) & M32) >> 16
            t ^= 512 ^ 1536


def compose(glyph, tabs):
    out = [WHITE] * (COMP_W * COMP_H)
    art_render(out, placeholder(glyph), BOX, BOX, tabs)
    return out


def encode(pix):
    freq = {}
    for p in pix:
        freq[p] = freq.get(p, 0) + 1
    pal = sorted(freq, key=lambda c: (-freq[c], c))
    if len(pal) > 256:
        sys.exit('npph.py: %d colours (> 256)' % len(pal))
    idx = {c: i for i, c in enumerate(pal)}
    ix = [idx[p] for p in pix]
    data = bytearray()
    lit = []

    def flush():
        while lit:
            chunk = lit[:128]
            del lit[:128]
            data.append(len(chunk) - 1)
            data.extend(chunk)
    i, n = 0, len(ix)
    while i < n:
        j = i
        while j < n and ix[j] == ix[i] and j - i < 129:
            j += 1
        if j - i >= 2:
            flush()
            data.append(j - i + 126)
            data.append(ix[i])
            i = j
        else:
            lit.append(ix[i])
            i += 1
    flush()
    return pal, bytes(data)


def decode(pal, data, n):
    out = []
    i = 0
    while len(out) < n:
        c = data[i]
        i += 1
        if c < 128:
            out.extend(pal[k] for k in data[i:i + c + 1])
            i += c + 1
        else:
            out.extend([pal[data[i]]] * (c - 126))
            i += 1
    return out


def c_bytes(name, ctype, vals, per, fmt):
    lines = ['static const %s %s[%d] = {' % (ctype, name, len(vals))]
    for k in range(0, len(vals), per):
        lines.append('    ' + ' '.join(fmt % v + ',' for v in vals[k:k + per]))
    lines.append('};')
    return '\n'.join(lines)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--c', default=os.path.join(SRC, 'npph_data.h'))
    ap.add_argument('--raw', help='also write <glyph>.raw (RGB565 LE) here')
    args = ap.parse_args()
    glyphs, tabs = load_sources()
    parts, total = [], 0
    for gid, nm in (('NPG_NOTE', 'note'), ('NPG_BOOK', 'book')):
        pix = compose(glyphs[gid], tabs)
        pal, data = encode(pix)
        if decode(pal, data, len(pix)) != pix:
            sys.exit('npph.py: %s does not round-trip' % nm)
        size = 2 * len(pal) + len(data)
        total += size
        print('npph %s: %d colours, %d B data, %d B rodata' % (nm, len(pal), len(data), size))
        if size > LIMIT_BYTES:
            sys.exit('npph.py: %s is %d B > %d: use the render-once fallback'
                     % (nm, size, LIMIT_BYTES))
        if args.raw:
            with open(os.path.join(args.raw, nm + '.raw'), 'wb') as f:
                f.write(b''.join(p.to_bytes(2, 'little') for p in pix))
        parts.append(c_bytes('npph_%s_pal' % nm, 'unsigned short', pal, 10, '0x%04x'))
        parts.append(c_bytes('npph_%s_data' % nm, 'unsigned char', list(data), 14, '0x%02x'))
    print('npph total %d B rodata' % total)
    hdr = ['/* Generated by art/np/npph.py -- do not edit.',
           ' * The composed Now Playing placeholders (QW2): x 10-147, y 34-204',
           ' * (137 x 187) against white, palette + PackBits over the whole',
           ' * image in row order. Included by np_draw.c only. */',
           '#ifndef _IPODUI_NPPH_DATA_H_',
           '#define _IPODUI_NPPH_DATA_H_',
           '',
           '#define NPPH_W %d' % COMP_W,
           '#define NPPH_H %d' % COMP_H,
           '']
    hdr.append('\n\n'.join(parts))
    hdr += ['', '#endif /* _IPODUI_NPPH_DATA_H_ */', '']
    with open(args.c, 'w') as f:
        f.write('\n'.join(hdr))
    prokbox_header.stamp(args.c)


# the ProkBox banner for the generated file (scripts/prokbox_header.py)
import os as _os  # noqa: E402
import sys as _sys  # noqa: E402
_sys.path.insert(0, _os.path.join(_os.path.dirname(_os.path.abspath(__file__)), '..', '..', 'scripts'))
import prokbox_header  # noqa: E402


if __name__ == '__main__':
    main()
