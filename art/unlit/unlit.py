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

"""Generate the unlit clock face art and mockups (stdlib only).

    art/unlit/unlit.py [OUTDIR]          # default docs/design/unlit

The unlit clock screen (issue #51, spec/system-screens.md) is what the
shell shows once the backlight times out: the time in large block
digits, a play or pause glyph below it and a battery icon, black on
white so that it reads by ambient light alone. Everything here is an
original procedural drawing: unions of axis-aligned rectangles,
rounded rectangles, hexagonal bars, triangles and circles, in design
units where one unit is one pixel on the 320 x 240 screen.

Four digit variants (each a monospaced cell, so a changed digit is
redrawn in place; metrics per variant in M below):

    stock    tuned to the owner's reference photo of a real unlit 7G:
             condensed 30 x 68 blocks, 9-unit strokes, 3-unit corner
             radius, dark-gray (56) body with a lighter (150) 1 px rim,
             large play triangle and battery (36 units tall)
    slab     48 x 80 solid squared blocks, 16-unit strokes, square
             counters, pure black: the boldest
    segment  48 x 80, seven separate hexagon-ended bars with 2-unit gaps;
             the unlit bars of the "8" show as a pale ghost (one gray)
    rounded  48 x 80, 14-unit strokes, every exposed corner rounded
             (radius 7): a softer block

Shared glyphs: colon, AM/PM letters (12-hour mode only), play, pause,
battery (any fill, optional charging bolt knocked out of the fill),
padlock (Hold). AM/PM letters are generated but hidden by default
(AMPM_SHOWN): the reference photo shows "4:01" with no marker.

Output (all PNG):

    <variant>-<scene>-lit.png       320 x 240, as the RGB565 panel shows
                                    it with the backlight on; glyphs
                                    1-bit (what lcd_mono_bitmap draws)
    <variant>-<scene>-unlit.png     the same, simulated without the
                                    backlight: contrast squeezed into a
                                    warm-gray range
    <variant>-<scene>-{lit,unlit}@2x.png
                                    drawn at 640 x 480 from the vectors
                                    (anti-aliased), for inspection
    glyphs-<variant>.png            the 1-bit glyph set, magnified 2x
    contact.png                     every variant x scene, lit row above
                                    unlit row, then the glyph sheets

and prints the storage cost of each variant's glyph set.
"""
import math
import os
import struct
import sys
import zlib

LCD_W, LCD_H = 320, 240
SS = 4                          # supersampling per axis

PAPER = 255
INK = 0
GHOST = 200                     # the one mid-gray (segment ghosts)

# ---------------------------------------------------------------- shapes
# A shape is a predicate f(x, y) in design units (y grows downwards).


def rect(x0, y0, x1, y1):
    return lambda x, y: x0 <= x < x1 and y0 <= y < y1


def rrect(x0, y0, x1, y1, r):
    r = min(r, (x1 - x0) / 2.0, (y1 - y0) / 2.0)

    def f(x, y):
        if not (x0 <= x < x1 and y0 <= y < y1):
            return False
        cx = min(max(x, x0 + r), x1 - r)
        cy = min(max(y, y0 + r), y1 - r)
        return (x - cx) ** 2 + (y - cy) ** 2 <= r * r
    return f


def circle(cx, cy, r):
    return lambda x, y: (x - cx) ** 2 + (y - cy) ** 2 <= r * r


def hbar(x0, x1, cy, h):
    """Horizontal bar with 45-degree pointed ends, half-thickness h."""
    def f(x, y):
        d = max(0.0, (x0 + h) - x, x - (x1 - h))
        return abs(y - cy) + d <= h and x0 <= x <= x1
    return f


def vbar(y0, y1, cx, h):
    g = hbar(y0, y1, cx, h)
    return lambda x, y: g(y, x)


def tri_right(x0, y0, x1, y1):
    """Triangle pointing right inside the box."""
    cy = (y0 + y1) / 2.0
    hh = (y1 - y0) / 2.0

    def f(x, y):
        if not (x0 <= x < x1):
            return False
        return abs(y - cy) <= hh * (x1 - x) / (x1 - x0)
    return f


def poly(pts):
    """Even-odd point-in-polygon."""
    def f(x, y):
        inside = False
        j = len(pts) - 1
        for i in range(len(pts)):
            xi, yi = pts[i]
            xj, yj = pts[j]
            if (yi > y) != (yj > y) and \
                    x < (xj - xi) * (y - yi) / (yj - yi) + xi:
                inside = not inside
            j = i
        return inside
    return f


def union(*fs):
    return lambda x, y: any(f(x, y) for f in fs)


def minus(a, *bs):
    return lambda x, y: a(x, y) and not any(b(x, y) for b in bs)


# ---------------------------------------------------------------- glyphs
# A glyph is (w, h, layers); layers = [(shape, ink)], painted in order.
# Every size comes from the variant's metrics, M[variant].

SEGS = {
    '0': 'abcdef', '1': 'bc', '2': 'abged', '3': 'abgcd', '4': 'fgbc',
    '5': 'afgcd', '6': 'afgedc', '7': 'abc', '8': 'abcdefg',
    '9': 'abcdfg',
}

VARIANTS = ('stock', 'slab', 'segment', 'rounded')

_BOLD = dict(dw=48, dh=80, t=16, r=0, cw=16, dgap=8, cgap=6, clock_y=52,
             row_y=160, gh=24, tri=21, bw=46, bh=24, bo=4, nub=10,
             rowgap=18, colon=(0.32, 0.70), lh=26, lt=6, bat_r=1,
             ink=0, edge=None)
M = {
    # measured from the owner's reference photo (see docs/design/unlit.md):
    # condensed 30 x 68 digits at y 62..130, dark-gray body with a lighter
    # 1 px edge, a large play triangle and battery centred on y ~179
    'stock': dict(dw=30, dh=68, t=9, r=3, cw=11, dgap=7, cgap=5,
                  clock_y=62, row_y=161, gh=36, tri=30, bw=54, bh=32,
                  bo=4, nub=12, rowgap=38, colon=(0.42, 0.88), lh=20,
                  lt=5, bat_r=4, ink=56, edge=150),
    'slab': dict(_BOLD),
    'segment': dict(_BOLD, t=13),
    'rounded': dict(_BOLD, t=14, r=7, bat_r=5),
}


def erode(shape, d):
    return lambda x, y: (shape(x - d, y) and shape(x + d, y) and
                         shape(x, y - d) and shape(x, y + d))


def inked(v, shape):
    """Body in the variant's ink; with an edge tone, a 1 px lighter rim."""
    m = M[v]
    if m['edge'] is None:
        return [(shape, m['ink'])]
    return [(shape, m['edge']), (erode(shape, 1.0), m['ink'])]


def R(v, x0, y0, x1, y1, r=None):
    r = M[v]['r'] if r is None else r
    return rrect(x0, y0, x1, y1, r) if r else rect(x0, y0, x1, y1)


def block_segs(v, W, H, T, r=None):
    """Seven overlapping (rounded) rectangles: joined block digits."""
    m0, m1 = (H - T) / 2.0, (H + T) / 2.0
    box = {
        'a': (0, 0, W, T), 'd': (0, H - T, W, H), 'g': (0, m0, W, m1),
        'f': (0, 0, T, m1), 'b': (W - T, 0, W, m1),
        'e': (0, m0, T, H), 'c': (W - T, m0, W, H),
    }
    return {k: R(v, *b, r=r) for k, b in box.items()}


def hex_segs(W, H, T, gap):
    """Classic separate bars with pointed ends and gaps."""
    h = T / 2.0
    m = H / 2.0
    xa, xb = h + gap, W - h - gap
    return {
        'a': hbar(xa - h, xb + h, h, h),
        'g': hbar(xa - h, xb + h, m, h),
        'd': hbar(xa - h, xb + h, H - h, h),
        'f': vbar(gap, m - gap + h, h, h),
        'b': vbar(gap, m - gap + h, W - h, h),
        'e': vbar(m + gap - h, H - gap, h, h),
        'c': vbar(m + gap - h, H - gap, W - h, h),
    }


def digit(v, ch):
    m = M[v]
    W, H, T = m['dw'], m['dh'], m['t']
    if v == 'segment':
        segs = hex_segs(W, H, T, 2.0)
        on = [segs[s] for s in SEGS[ch]]
        off = [segs[s] for s in 'abcdefg' if s not in SEGS[ch]]
        layers = [(union(*off), GHOST)] if off else []
        return (W, H, layers + [(union(*on), INK)])
    if ch == '1':
        # a centred bar with a flag reads better than a lone right bar
        x0 = (W - T) / 2.0 + W * 0.1
        shape = union(R(v, x0, 0, x0 + T, H),
                      R(v, x0 - W * 0.25, 0, x0 + T, T))
        return (W, H, inked(v, shape))
    segs = block_segs(v, W, H, T)
    return (W, H, inked(v, union(*[segs[s] for s in SEGS[ch]])))


def colon(v):
    m = M[v]
    W, H, T = m['cw'], m['dh'], m['t'] - (2 if m['t'] > 10 else 0)
    x0 = (W - T) / 2.0
    ys = [H * f - T / 2.0 for f in m['colon']]
    if v == 'rounded':
        dots = [circle(W / 2.0, y + T / 2.0, T / 2.0) for y in ys]
    elif v == 'segment':
        dots = [hbar(x0, x0 + T, y + T / 2.0, T / 2.0) for y in ys]
    else:
        dots = [R(v, x0, y, x0 + T, y + T) for y in ys]
    return (W, H, inked(v, union(*dots)))


def letter(v, ch):
    m = M[v]
    H, T = m['lh'], m['lt']
    r = 3 if v == 'rounded' else (1 if v == 'stock' else 0)
    if ch == 'M':
        W = round(T * 4.7)
        parts = [R(v, 0, 0, T, H, r), R(v, W - T, 0, W, H, r),
                 R(v, 0, 0, W, T, r),
                 R(v, (W - T) / 2.0, 0, (W + T) / 2.0, H * 0.62, r)]
        return (W, H, inked(v, union(*parts)))
    W = round(T * 3.2)
    segs = block_segs(v, W, H, T, r)
    s = {'A': 'abcefg', 'P': 'abfge'}[ch]
    return (W, H, inked(v, union(*[segs[k] for k in s])))


def play(v):
    m = M[v]
    return (m['tri'], m['gh'], inked(v, tri_right(0, 0, m['tri'], m['gh'])))


def pause(v):
    m = M[v]
    bw = round(m['gh'] / 3.0)
    W = 2 * bw + round(bw * 0.9)
    return (W, m['gh'], inked(v, union(rect(0, 0, bw, m['gh']),
                                       rect(W - bw, 0, W, m['gh']))))


def battery(v, level, charging=False):
    """level 0..1. Outline, 2-unit air, proportional fill."""
    m = M[v]
    W, H, o, a, r = m['bw'], m['bh'], m['bo'], 2, m['bat_r']
    body = minus(R(v, 0, 0, W, H, r), R(v, o, o, W - o, H - o, max(0, r - o)))
    nub = rect(W, (H - m['nub']) / 2.0, W + 4, (H + m['nub']) / 2.0)
    fx0, fx1 = o + a, W - o - a
    fill_w = round((fx1 - fx0) * level)
    fill = rect(fx0, o + a, fx0 + max(fill_w, 1 if level > 0 else 0),
                H - o - a)
    if charging:
        # a bolt knocked out of the fill (XOR): white over the fill,
        # black over the empty part, so it reads at any level
        sx, sy = W / 46.0, (H - 2 * o) / 16.0
        bolt = poly([(27 * sx, o), (16 * sx, o + 9 * sy),
                     (22 * sx, o + 9 * sy), (18 * sx, H - o),
                     (31 * sx, o + 6.5 * sy), (25 * sx, o + 6.5 * sy),
                     (32 * sx, o)])
        inner = rect(o, o, W - o, H - o)
        plain = fill
        fill = lambda x, y: inner(x, y) and (plain(x, y) != bolt(x, y))
    return (W + 4, H, inked(v, union(body, nub, fill)))


def lock(v):
    k = M[v]['gh'] / 24.0
    W, H = 20 * k, 24 * k
    shackle = minus(union(circle(W / 2, 8 * k, 8 * k),
                          rect(2 * k, 8 * k, W - 2 * k, 13 * k)),
                    union(circle(W / 2, 8 * k, 4 * k),
                          rect(6 * k, 8 * k, W - 6 * k, 13 * k)))
    body = rect(0, 11 * k, W, H)
    hole = rect(W / 2 - 2 * k, 15 * k, W / 2 + 2 * k, 20 * k)
    return (W, H, inked(v, minus(union(shackle, body), hole)))

# ---------------------------------------------------------------- raster

_cache = {}


def raster(glyph, scale, key=None):
    """-> (pw, ph, [(coverage rows, ink)]) at the given scale."""
    if key is not None and (key, scale) in _cache:
        return _cache[(key, scale)]
    w, h, layers = glyph
    pw, ph = int(math.ceil(w * scale)), int(math.ceil(h * scale))
    offs = [(i + 0.5) / SS for i in range(SS)]
    n = float(SS * SS)
    out = []
    for shape, ink in layers:
        cov = []
        for py in range(ph):
            row = [0.0] * pw
            for px in range(pw):
                c = 0
                for oy in offs:
                    y = (py + oy) / scale
                    for ox in offs:
                        if shape((px + ox) / scale, y):
                            c += 1
                row[px] = c / n
            cov.append(row)
        out.append((cov, ink))
    res = (pw, ph, out)
    if key is not None:
        _cache[(key, scale)] = res
    return res


def blit(img, r, x, y, crisp):
    pw, ph, layers = r
    for cov, ink in layers:
        for py in range(ph):
            row = img[y + py]
            crow = cov[py]
            for px in range(pw):
                a = crow[px]
                if crisp:
                    a = 1.0 if a >= 0.5 else 0.0
                if a > 0:
                    v = row[x + px]
                    row[x + px] = int(round(v * (1 - a) + ink * a))


def mono_bytes(w, h):
    """lcd_mono_bitmap format: 8 vertical pixels per byte, row of pages."""
    return w * ((h + 7) // 8)

# ---------------------------------------------------------------- scenes

AMPM_SHOWN = False              # ⚠️ the reference photo shows "4:01" bare
AMPM_GAP = 8


def layout(v, hh, mm, ampm, state, level, charging, hold):
    """-> list of (glyph, key, x, y) in units for one face."""
    m = M[v]
    items = []
    clock = []
    for i, ch in enumerate(hh + mm):
        if i == len(hh):
            clock.append(('colon', colon(v), m['cw']))
        clock.append((ch, digit(v, ch), m['dw']))

    def gap(k, nxt):
        return m['cgap'] if 'colon' in (k, nxt) else m['dgap']
    width = sum(w for k, g, w in clock) + sum(
        gap(clock[i][0], clock[i + 1][0]) for i in range(len(clock) - 1))
    if ampm and AMPM_SHOWN:
        width += AMPM_GAP + sum(letter(v, c)[0] + 3 for c in ampm) - 3
    x = (LCD_W - width) // 2
    for i, (k, g, w) in enumerate(clock):
        items.append((g, (v, k), x, m['clock_y']))
        x += w
        if i + 1 < len(clock):
            x += gap(k, clock[i + 1][0])
    if ampm and AMPM_SHOWN:
        x += AMPM_GAP
        for c in ampm:
            g = letter(v, c)
            items.append((g, (v, 'L' + c), x,
                          m['clock_y'] + m['dh'] - m['lh']))
            x += g[0] + 3
    # status row: [play/pause] [lock] [battery], centred as a group
    row = []
    if state == 'play':
        row.append((play(v), (v, 'play'), m['rowgap']))
    elif state == 'pause':
        row.append((pause(v), (v, 'pause'), m['rowgap']))
    if hold:
        row.append((lock(v), (v, 'lock'), m['rowgap'] // 2))
    row.append((battery(v, level, charging), (v, 'bat', level, charging), 0))
    rw = sum(g[0] + gp for g, k, gp in row)
    x = (LCD_W - rw) // 2
    for g, k, gp in row:
        items.append((g, k, round(x), m['row_y'] + (m['gh'] - g[1]) // 2))
        x += g[0] + gp
    return items


SCENES = [
    # name, hh, mm, ampm, state, battery, charging, hold
    ('12h-playing', '4', '01', 'PM', 'play', 0.55, False, False),
    ('24h-paused', '09', '41', None, 'pause', 1.00, False, False),
    ('24h-stopped', '15', '03', None, 'stop', 0.08, False, False),
    ('12h-hold-charging', '12', '58', 'AM', 'play', 0.45, True, True),
]


def render(items, scale, crisp):
    w, h = int(LCD_W * scale), int(LCD_H * scale)
    img = [[PAPER] * w for _ in range(h)]
    for g, key, x, y in items:
        r = raster(g, scale, key)
        blit(img, r, int(round(x * scale)), int(round(y * scale)), crisp)
    return img

# ---------------------------------------------------------------- looks


def rgb565(v):
    r = (v >> 3) << 3 | v >> 5
    g = (v >> 2) << 2 | v >> 6
    return (r, g, r)


UNLIT_DARK = (60, 58, 52)       # ink with no backlight: not quite black
UNLIT_LIGHT = (182, 177, 165)   # paper: warm light gray (owner thumbnail)


def look(img, unlit):
    out = []
    for row in img:
        if unlit:
            o = []
            for v in row:
                t = v / 255.0
                o.append(tuple(int(round(d + (l - d) * t))
                               for d, l in zip(UNLIT_DARK, UNLIT_LIGHT)))
            out.append(o)
        else:
            out.append([rgb565(v) for v in row])
    return out


def write_png(path, rows):
    h, w = len(rows), len(rows[0])
    raw = bytearray()
    for row in rows:
        raw.append(0)
        for px in row:
            raw += bytes(px)

    def chunk(tag, data):
        c = struct.pack('>I', len(data)) + tag + data
        return c + struct.pack('>I', zlib.crc32(tag + data) & 0xffffffff)
    png = b'\x89PNG\r\n\x1a\n'
    png += chunk(b'IHDR', struct.pack('>IIBBBBB', w, h, 8, 2, 0, 0, 0))
    png += chunk(b'IDAT', zlib.compress(bytes(raw), 9))
    png += chunk(b'IEND', b'')
    with open(path, 'wb') as fp:
        fp.write(png)


def paste(dst, src, x, y, k=1):
    for py, row in enumerate(src):
        for ky in range(k):
            d = dst[y + py * k + ky]
            for px, c in enumerate(row):
                for kx in range(k):
                    d[x + px * k + kx] = c

# ---------------------------------------------------------------- main


def glyph_sheet(v):
    """1-bit glyph set at 1x on paper: digits, colon, letters, row glyphs."""
    m = M[v]
    top = [(digit(v, c), (v, c)) for c in '0123456789']
    top.append((colon(v), (v, 'colon')))
    top += [(letter(v, c), (v, 'L' + c)) for c in 'APM']
    bot = [(play(v), (v, 'play')), (pause(v), (v, 'pause')),
           (lock(v), (v, 'lock'))]
    bot += [(battery(v, lv), (v, 'bat', lv, False))
            for lv in (0.0, 0.08, 0.25, 0.5, 0.75, 1.0)]
    bot.append((battery(v, 0.45, True), (v, 'bat', 0.45, True)))
    w = max(sum(int(math.ceil(g[0])) + 6 for g, k in top),
            sum(int(math.ceil(g[0])) + 10 for g, k in bot)) + 6
    img = [[PAPER] * w for _ in range(m['dh'] + m['gh'] + 18)]
    x = 6
    for g, k in top:
        blit(img, raster(g, 1, k), x, 6 + m['dh'] - int(math.ceil(g[1])),
             True)
        x += int(math.ceil(g[0])) + 6
    x = 6
    for g, k in bot:
        blit(img, raster(g, 1, k), x, m['dh'] + 12, True)
        x += int(math.ceil(g[0])) + 10
    return img


def cost(v):
    """Bytes of the glyph set as 1-bit planes (lcd_mono_bitmap)."""
    m = M[v]
    planes = 2 if m['edge'] is not None else 1
    digits = 10 * mono_bytes(m['dw'], m['dh']) * planes
    lets = sum(mono_bytes(letter(v, c)[0], m['lh']) for c in 'APM') * planes
    ghost = mono_bytes(m['dw'], m['dh']) if v == 'segment' else 0
    row = sum(mono_bytes(int(math.ceil(g[0])), int(math.ceil(g[1])))
              for g in (play(v), pause(v), lock(v))) * planes
    alpha = 10 * m['dw'] * m['dh'] // 2
    return digits, lets, ghost, row, alpha


def main():
    out = sys.argv[1] if len(sys.argv) > 1 else 'docs/design/unlit'
    os.makedirs(out, exist_ok=True)
    gap = 8
    sheets = {v: glyph_sheet(v) for v in VARIANTS}
    cw = max(len(SCENES) * (LCD_W + gap) + gap,
             max(len(s[0]) * 2 for s in sheets.values()) + 2 * gap)
    ch = len(VARIANTS) * (2 * LCD_H + 3 * gap) + gap \
        + sum(len(s) * 2 + gap for s in sheets.values())
    sheet = [[(90, 90, 90)] * cw for _ in range(ch)]
    y = gap
    for v in VARIANTS:
        for si, sc in enumerate(SCENES):
            name, hh, mm, ap, st, lv, chg, hold = sc
            items = layout(v, hh, mm, ap, st, lv, chg, hold)
            one = render(items, 1, True)
            two = render(items, 2, False)
            for unlit in (False, True):
                tag = 'unlit' if unlit else 'lit'
                l1 = look(one, unlit)
                write_png(os.path.join(out, '%s-%s-%s.png' % (v, name, tag)),
                          l1)
                write_png(os.path.join(out, '%s-%s-%s@2x.png'
                                       % (v, name, tag)), look(two, unlit))
                paste(sheet, l1, gap + si * (LCD_W + gap),
                      y + (LCD_H + gap if unlit else 0))
        y += 2 * LCD_H + 3 * gap
        print('%s: scenes done' % v)
    for v in VARIANTS:
        s = look(sheets[v], False)
        write_png(os.path.join(out, 'glyphs-%s.png' % v), [
            [c for c in row for _ in (0, 1)] for row in s for _ in (0, 1)])
        paste(sheet, s, gap, y, 2)
        y += len(s) * 2 + gap
    write_png(os.path.join(out, 'contact.png'), sheet)

    print('\nstorage as lcd_mono_bitmap planes (8 px per byte vertically):')
    for v in VARIANTS:
        d, l, g, r, a = cost(v)
        m = M[v]
        print('  %-8s digits 10 x %dx%d = %d B, AM/PM %d B, ghost %d B, '
              'play/pause/lock %d B -> %d B (4-bit alpha digits: %d B)'
              % (v, m['dw'], m['dh'], d, l, g, r, d + l + g + r, a))


if __name__ == '__main__':
    main()
