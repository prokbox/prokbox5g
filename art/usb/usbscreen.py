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

"""Generate the native USB screen's art (stdlib only).

    art/usb/usbscreen.py [--c HEADER] [--png OUTDIR]
        defaults: apps/ipodui/usbscreen_data.h, docs/design/usb

The 7G's "Connected" screen (docs/design/spec/system-screens.md, owner
photos 1165 / photo 1226): a dark header, a slate-to-navy body, a
glossy orange disc with a connector pictogram while idle and two sync
arrows spinning while the host moves data, then "Connected" / "Eject
before disconnecting." Every shape here is an original drawing.

This script is the visual source of truth. It emits:

  * apps/ipodui/usbscreen_data.h: the header rows, the body gradient
    stops, the disc's per-row colour table, the two connector glyphs
    (4-bit alpha, run-length coded) and the arrow geometry as integer
    half-planes. The C side (apps/ipodui/usbscreen.c) draws the disc and
    the arrows procedurally from these numbers with exactly the integer
    arithmetic below, so its frames are pixel-identical to the PNGs.
  * docs/design/usb/*.png: the screen without its text (the text is the
    shell font, drawn by the firmware), for both idle glyphs and the 12
    arrow frames, plus a magnified sheet.

Geometry: disc 92 px at (160, 99), i.e. the 92 x 92 box x 114..205,
y 53..144, as measured on the owner photos and usb.png (the frozen
default U1, 80 px at (160, 108), was overturned by the coordinator). Coordinates
inside the box are "doubled": X = 2x - 91, Y = 2y - 91 (odd integers, the
box centre is 0), so the disc and the arrows are symmetric to the pixel
and a 90-degree rotation is an index remap.
"""
import math
import os
import struct
import sys
import zlib

LCD_W, LCD_H = 320, 240
HDR_H = 22                      # dark header, y 0..21
DISC_D = 92                     # mirrors usbscreen.h UI_USB_DISC_*
DISC_CX, DISC_CY = 160, 99
BOX_X = DISC_CX - DISC_D // 2   # 114
BOX_Y = DISC_CY - DISC_D // 2   # 53
FRAMES = 12                     # 30-degree steps
BASES = 3                       # rendered angles; the rest are rotations
SS = 8                          # glyph supersampling per axis

# ---------------------------------------------------------------- colours

def hexrgb(s):
    return tuple(int(s[i:i + 2], 16) for i in (1, 3, 5))


def rgb565(c):
    r, g, b = c
    return (r >> 3) << 11 | (g >> 2) << 5 | b >> 3


def expand565(v):
    """RGB565 to 8 bits per channel as the simulator's screenshots do"""
    r, g, b = v >> 11 & 31, v >> 5 & 63, v & 31
    return ((r * 255 + 15) // 31, (g * 255 + 31) // 63, (b * 255 + 15) // 31)


# Dark header (MEASUREMENTS.md "USB/charge header": #333739 -> #191D1F,
# then the darker bottom rows of the 7G's own screen).
HDR_STOPS = [(0, '#33373A'), (15, '#191D20'), (20, '#101519'),
             (21, '#071017')]
# Body (MEASUREMENTS.md "USB/charge background": #889AA5 at y 24 ->
# #464F56 at y 239, slightly bowed): a 1 px shadow under the header,
# then the gradient, 2 x 2 dithered.
BODY_STOPS = [(22, '#5C6670'), (23, '#7A8893'), (24, '#889AA5'),
              (70, '#7B8898'), (118, '#66747F'), (166, '#545E68'),
              (214, '#485158'), (239, '#464F56')]
# Disc (owner photos, cast-corrected): cream at the top (the gloss),
# through orange to a deep red-orange at about two thirds, and a little
# bounce light at the bottom. Indexed by DISC_IDX(x, y) = y + (x-46)>>3,
# so the left side reads one shade lighter (the 7G's top-left light).
DISC_IDX_MIN = -6
DISC_IDX_MAX = 91 + 5
DISC_STOPS = [(-6, '#F8E8D4'), (0, '#F4DEC6'), (8, '#EECBAE'),
              (16, '#EDB88A'), (24, '#F2A167'), (32, '#F18645'),
              (44, '#F07038'), (53, '#EE622F'), (64, '#EC5B2A'),
              (78, '#EF6130'), (85, '#F26A36'), (91, '#F47843'),
              (96, '#F47843')]
INK = (0, 0, 0)


def tdiv(a, b):
    """C99 integer division (truncates towards zero)."""
    q = abs(a) // abs(b)
    return q if (a >= 0) == (b > 0) else -q


def interp(stops, y):
    """8.8 fixed-point channel values at row y (C: usb_interp())."""
    for i in range(len(stops) - 1):
        y0, c0 = stops[i]
        y1, c1 = stops[i + 1]
        if y0 <= y <= y1:
            c0, c1 = hexrgb(c0), hexrgb(c1)
            return tuple(c0[k] * 256 + tdiv((c1[k] - c0[k]) * 256 * (y - y0),
                                            y1 - y0) for k in range(3))
    raise ValueError(y)


BAYER = ((0, 2), (3, 1))


def dither565(v, x, y):
    """2 x 2 ordered dither of an 8.8 colour to RGB565 (C: usb_dither())."""
    thr = (2 * BAYER[y & 1][x & 1] + 1) * 32
    out = []
    for k, bits in enumerate((5, 6, 5)):
        mx = (1 << bits) - 1
        q = (v[k] * mx // 255 + thr) >> 8
        out.append(min(q, mx))
    return out[0] << 11 | out[1] << 5 | out[2]


def body_px(x, y):
    return dither565(interp(BODY_STOPS, y), x, y)


def hdr_rows():
    rows = []
    for y in range(HDR_H):
        v = interp(HDR_STOPS, y)
        rows.append(rgb565(tuple(min(255, (c + 128) >> 8) for c in v)))
    return rows


def disc_table():
    tab = []
    for i in range(DISC_IDX_MIN, DISC_IDX_MAX + 1):
        v = interp(DISC_STOPS, i)
        tab.append(rgb565(tuple(min(255, (c + 128) >> 8) for c in v)))
    return tab


def blend565(bg, fg, a):
    """a = 0..16 is fg's weight (C: usb_blend())."""
    r = ((fg >> 11 & 31) * a + (bg >> 11 & 31) * (16 - a)) >> 4
    g = ((fg >> 5 & 63) * a + (bg >> 5 & 63) * (16 - a)) >> 4
    b = ((fg & 31) * a + (bg & 31) * (16 - a)) >> 4
    return r << 11 | g << 5 | b

# ---------------------------------------------------------------- disc

R2_OUT = (DISC_D + 1) ** 2      # doubled radius + 1, squared
R2_IN = (DISC_D - 1) ** 2


def clean_disc(tab):
    """The disc over the body, 80 x 80 RGB565 (C: usb_draw_disc())."""
    px = []
    for y in range(DISC_D):
        Y = 2 * y - (DISC_D - 1)
        for x in range(DISC_D):
            X = 2 * x - (DISC_D - 1)
            d2 = X * X + Y * Y
            bg = body_px(BOX_X + x, BOX_Y + y)
            if d2 >= R2_OUT:
                px.append(bg)
                continue
            c = tab[y + ((x - DISC_D // 2) >> 3) - DISC_IDX_MIN]
            if d2 <= R2_IN:
                px.append(c)
            else:
                px.append(blend565(bg, c, (R2_OUT - d2) * 16
                                   // (R2_OUT - R2_IN)))
    return px

# ---------------------------------------------------------------- arrows
# Two arcs plus two heads (photo 1165): the ring centre line at radius RM,
# stroke ARC_W; arrow 1 runs clockwise from its square tail at TAIL_DEG
# (degrees clockwise from 12 o'clock) to its head's base at HEAD_DEG; the
# head is a triangle HEAD_HW to each side of the centre line, its tip
# HEAD_L ahead along the tangent. Arrow 2 is arrow 1 turned 180 degrees.
RM = 24.2
ARC_W = 10.4
TAIL_DEG = 215.0
HEAD_DEG = 357.0
HEAD_HW = 11.5
HEAD_L = 12.6
Q = 4096                        # half-plane normals, Q12
SH = 9                          # (Q12 * half px) >> 9 = 1/16 px


def unit(deg):
    t = math.radians(deg)
    return (math.sin(t), -math.cos(t)), (math.cos(t), math.sin(t))


def arrow_params(base):
    """Integer half-planes for base angle 'base' * 30 degrees."""
    off = 30.0 * base
    _, tt = unit(TAIL_DEG + off)
    u, th = unit(HEAD_DEG + off)
    b1 = [2 * (RM - HEAD_HW) * c for c in u]           # doubled coords
    b2 = [2 * (RM + HEAD_HW) * c for c in u]
    tip = [2 * (RM * u[k] + HEAD_L * th[k]) for k in range(2)]
    edges = []
    for p, q, other in ((b1, tip, b2), (b2, tip, b1)):
        dx, dy = q[0] - p[0], q[1] - p[1]
        n = math.hypot(dx, dy)
        nx, ny = -dy / n, dx / n
        if nx * (other[0] - p[0]) + ny * (other[1] - p[1]) < 0:
            nx, ny = -nx, -ny
        c = -(nx * p[0] + ny * p[1])
        edges += [round(nx * Q), round(ny * Q), round(c * Q)]
    return [round(tt[0] * Q), round(tt[1] * Q),
            round(th[0] * Q), round(th[1] * Q)] + edges


def ring_limits():
    """Squared doubled radii outside which no arrow pixel can be."""
    tip_r = math.hypot(RM, HEAD_L)
    r_in = max(0.0, min(RM - ARC_W / 2, RM - HEAD_HW) - 1)
    r_out = max(RM + ARC_W / 2, RM + HEAD_HW, tip_r) + 1
    return int((2 * r_in) ** 2), int(math.ceil((2 * r_out) ** 2))


RM16 = round(RM * 16)
HW16 = round(ARC_W / 2 * 16)


def clamp16(v):
    return 0 if v < 0 else (16 if v > 16 else v)


def arrow_alpha(p, lim):
    """80 x 80 coverage 0..16 for one base angle (C: usb_arrow_alpha())."""
    tx, ty, hx, hy, e1x, e1y, e1c, e2x, e2y, e2c = p
    rin2, rout2 = lim
    out = []
    for y in range(DISC_D):
        Y = 2 * y - (DISC_D - 1)
        for x in range(DISC_D):
            X = 2 * x - (DISC_D - 1)
            d2 = X * X + Y * Y
            if d2 < rin2 or d2 > rout2:
                out.append(0)
                continue
            r16 = math.isqrt(d2 << 6)
            radial = clamp16(HW16 + 8 - abs(r16 - RM16))
            cov = 0
            for s in (1, -1):
                px, py = s * X, s * Y
                dt = (tx * px + ty * py) >> SH
                dh = (hx * px + hy * py) >> SH
                arc = min(radial, clamp16(dt + 8), clamp16(8 - dh))
                d1 = (e1x * px + e1y * py + e1c) >> SH
                d2e = (e2x * px + e2y * py + e2c) >> SH
                head = clamp16(min(dh, d1, d2e) + 8)
                cov += arc + head
            out.append(min(16, cov))
    return out


def rotated(alpha, j):
    """alpha turned j * 90 degrees clockwise (C: the frame loop)."""
    n = DISC_D - 1
    out = []
    for y in range(DISC_D):
        for x in range(DISC_D):
            if j == 0:
                sx, sy = x, y
            elif j == 1:
                sx, sy = y, n - x
            elif j == 2:
                sx, sy = n - x, n - y
            else:
                sx, sy = n - y, x
            out.append(alpha[sy * DISC_D + sx])
    return out


def ink_over(disc, alpha):
    return [d if a == 0 else blend565(d, 0, a) for d, a in zip(disc, alpha)]

# ---------------------------------------------------------------- glyphs
# Unions of (rounded) rectangles in glyph pixels, then holes punched back
# out (the slots). Supersampled SS x SS into 4-bit alpha.


def in_rrect(px, py, r):
    x0, y0, x1, y1, rad = r
    if px < x0 or px >= x1 or py < y0 or py >= y1:
        return False
    rad_tl, rad_tr, rad_bl, rad_br = rad if isinstance(rad, tuple) \
        else (rad, rad, rad, rad)
    for cx, cy, rr, ok in ((x0 + rad_tl, y0 + rad_tl, rad_tl,
                            px < x0 + rad_tl and py < y0 + rad_tl),
                           (x1 - rad_tr, y0 + rad_tr, rad_tr,
                            px > x1 - rad_tr and py < y0 + rad_tr),
                           (x0 + rad_bl, y1 - rad_bl, rad_bl,
                            px < x0 + rad_bl and py > y1 - rad_bl),
                           (x1 - rad_br, y1 - rad_br, rad_br,
                            px > x1 - rad_br and py > y1 - rad_br)):
        if ok and rr > 0 and (px - cx) ** 2 + (py - cy) ** 2 > rr * rr:
            return False
    return True


def raster(w, h, ink, holes):
    a = []
    for y in range(h):
        row = []
        for x in range(w):
            n = 0
            for sy in range(SS):
                for sx in range(SS):
                    px = x + (sx + 0.5) / SS
                    py = y + (sy + 0.5) / SS
                    if any(in_rrect(px, py, r) for r in ink) and \
                            not any(in_rrect(px, py, r) for r in holes):
                        n += 1
            row.append((n * 15 + SS * SS // 2) // (SS * SS))
        a.append(row)
    return a


# 30-pin dock pictogram (HAVE_IPOD_DOCK30), 50 x 41: the connector's
# head with its pin groove, a 1 px gap, the wide body with a small port
# outline, then the strain relief and the cable.
DOCK30 = (50, 41, [
    (5, 0, 45, 11, (2, 2, 0, 0)),       # head
    (0, 12, 50, 27, (1, 1, 2, 2)),      # body
    (19, 27, 31, 30, 0),                # strain relief
    (20, 30, 30, 41, (0, 0, 1, 1)),     # cable
], [
    (9, 3, 41, 4, 0),                   # pin groove
    (18, 17, 32, 22, 1),                # port outline (ring) ...
])
DOCK30_FILL = [(20, 18, 30, 21, 0)]     # ... with its tongue left inked

# USB-C plug pictogram (this unit), 36 x 43: the metal shell with the
# oval opening and its tongue, a 1 px gap, the moulded grip with two
# lines, the strain relief and the cable.
USBC = (36, 43, [
    (7, 0, 29, 11, (3, 3, 0, 0)),       # metal shell
    (1, 12, 35, 30, (3, 3, 7, 7)),      # moulded grip
    (13, 30, 23, 33, 0),                # strain relief
    (14, 33, 22, 43, (0, 0, 1, 1)),     # cable
], [
    (10, 3, 26, 9, 2),                  # oval opening ...
    (8, 19, 28, 20, 0),                 # grip lines
    (8, 22, 28, 23, 0),
])
USBC_FILL = [(13, 5, 23, 7, 1)]         # ... with the tongue inside


def glyph(spec, fill):
    w, h, ink, holes = spec
    a = raster(w, h, ink, holes)
    f = raster(w, h, fill, [])
    return w, h, [[max(a[y][x], f[y][x]) for x in range(w)]
                  for y in range(h)]


def rle(rows):
    """(alpha << 4) | (run - 1), runs of 1..16 in raster order."""
    flat = [v for r in rows for v in r]
    out = []
    i = 0
    while i < len(flat):
        j = i
        while j < len(flat) and flat[j] == flat[i] and j - i < 16:
            j += 1
        out.append(flat[i] << 4 | (j - i - 1))
        i = j
    return out


def unrle(data, w, h):
    flat = []
    for b in data:
        flat += [b >> 4] * ((b & 15) + 1)
    assert len(flat) == w * h
    return flat


def glyph_over(disc, g):
    w, h, rows = g
    gx, gy = (DISC_D - w) // 2, (DISC_D - h) // 2
    out = list(disc)
    for y in range(h):
        for x in range(w):
            a4 = rows[y][x]
            if a4:
                i = (gy + y) * DISC_D + gx + x
                out[i] = blend565(out[i], 0, a4 + (a4 >> 3))
    return out

# ---------------------------------------------------------------- output


def write_png(path, w, h, px):
    raw = bytearray()
    for y in range(h):
        raw.append(0)
        for x in range(w):
            raw += bytes(expand565(px[y * w + x]))

    def chunk(tag, data):
        c = struct.pack('>I', len(data)) + tag + data
        return c + struct.pack('>I', zlib.crc32(tag + data) & 0xffffffff)
    png = b'\x89PNG\r\n\x1a\n'
    png += chunk(b'IHDR', struct.pack('>IIBBBBB', w, h, 8, 2, 0, 0, 0))
    png += chunk(b'IDAT', zlib.compress(bytes(raw), 9))
    png += chunk(b'IEND', b'')
    with open(path, 'wb') as fp:
        fp.write(png)


def screen(hdr, box):
    """Full 320 x 240 frame: header rows, dithered body, the disc box.
    Header text, battery and the two text lines are the firmware's."""
    px = []
    for y in range(LCD_H):
        for x in range(LCD_W):
            if y < HDR_H:
                px.append(hdr[y])
            elif BOX_Y <= y < BOX_Y + DISC_D and BOX_X <= x < BOX_X + DISC_D:
                px.append(box[(y - BOX_Y) * DISC_D + x - BOX_X])
            else:
                px.append(body_px(x, y))
    return px


def magnify(px, w, h, k):
    out = []
    for y in range(h * k):
        for x in range(w * k):
            out.append(px[(y // k) * w + x // k])
    return out


def fnv1a(words):
    h = 0x811c9dc5
    for v in words:
        for b in (v & 0xff, v >> 8 & 0xff, v >> 16 & 0xff, v >> 24 & 0xff):
            h = ((h ^ b) * 0x01000193) & 0xffffffff
    return h


def c_array(name, ctype, vals, per=12, fmt='%d'):
    s = 'static const %s %s[%d] = {\n' % (ctype, name, len(vals))
    for i in range(0, len(vals), per):
        s += '    ' + ', '.join(fmt % v for v in vals[i:i + per]) + ',\n'
    return s + '};\n'


def write_header(path, hdr, tab, params, lim, g_dock, g_usbc, d_dock, d_usbc,
                 gen):
    def stops(name, st):
        s = 'static const unsigned char %s[%d][4] = {\n' % (name, len(st))
        for y, c in st:
            r, g, b = hexrgb(c)
            s += '    { %3d, 0x%02x, 0x%02x, 0x%02x },\n' % (y, r, g, b)
        return s + '};\n'

    o = []
    o.append('/* Generated by art/usb/usbscreen.py: do not edit.\n'
             ' * The native USB screen\'s art (docs/phase8a-brief.md §3.2);\n'
             ' * reference PNGs in docs/design/usb/. Original artwork. */\n')
    o.append('#ifndef _IPODUI_USBSCREEN_DATA_H_\n'
             '#define _IPODUI_USBSCREEN_DATA_H_\n\n')
    o.append('#define USB_GEN           0x%08xUL /* FNV-1a of the tables */\n'
             % gen)
    o.append('#define USB_HDR_H         %d\n' % HDR_H)
    o.append('#define USB_DISC_D        %d\n' % DISC_D)
    o.append('#define USB_DISC_CX       %d\n' % DISC_CX)
    o.append('#define USB_DISC_CY       %d\n' % DISC_CY)
    o.append('#define USB_FRAMES        %d\n' % FRAMES)
    o.append('#define USB_BASES         %d\n' % BASES)
    o.append('#define USB_DISC_IDX_MIN  (%d)\n' % DISC_IDX_MIN)
    o.append('#define USB_R2_OUT        %d\n' % R2_OUT)
    o.append('#define USB_R2_IN         %d\n' % R2_IN)
    o.append('#define USB_RM16          %d\n' % RM16)
    o.append('#define USB_HW16          %d\n' % HW16)
    o.append('#define USB_RIN2          %d\n' % lim[0])
    o.append('#define USB_ROUT2         %d\n' % lim[1])
    o.append('#define USB_SH            %d\n\n' % SH)
    o.append('/* dark header, one RGB565 colour per row */\n')
    o.append(c_array('usb_hdr_rows', 'unsigned short', hdr, 8, '0x%04x'))
    o.append('\n/* body gradient stops: row, r, g, b (8-bit), 2x2 dithered */\n')
    o.append(stops('usb_body_stops', BODY_STOPS))
    o.append('#define USB_BODY_STOPS %d\n' % len(BODY_STOPS))
    o.append('\n/* disc colour by y + ((x - 46) >> 3), from USB_DISC_IDX_MIN */\n')
    o.append(c_array('usb_disc_tab', 'unsigned short', tab, 8, '0x%04x'))
    o.append('\n/* arrow half-planes per base angle (0, 30, 60 degrees), Q12 over\n'
             ' * doubled box coordinates: tail tangent (x, y), head tangent\n'
             ' * (x, y), then the head\'s two side edges (nx, ny, c) */\n')
    s = 'static const long usb_arrow_hp[USB_BASES][10] = {\n'
    for p in params:
        s += '    { ' + ', '.join('%d' % v for v in p) + ' },\n'
    o.append(s + '};\n')
    o.append('\n/* connector glyphs: 4-bit alpha, (alpha << 4) | (run - 1); the\n'
             ' * target compiles only its own (USB_BOTH_GLYPHS: sim, host test) */\n')
    o.append('#if defined(HAVE_IPOD_DOCK30) || defined(USB_BOTH_GLYPHS)\n')
    o.append('#define USB_DOCK30_W %d\n#define USB_DOCK30_H %d\n'
             % (g_dock[0], g_dock[1]))
    o.append(c_array('usb_glyph_dock30', 'unsigned char', d_dock, 12,
                     '0x%02x'))
    o.append('#endif\n#if !defined(HAVE_IPOD_DOCK30) || defined(USB_BOTH_GLYPHS)\n')
    o.append('#define USB_USBC_W %d\n#define USB_USBC_H %d\n'
             % (g_usbc[0], g_usbc[1]))
    o.append(c_array('usb_glyph_usbc', 'unsigned char', d_usbc, 12, '0x%02x'))
    o.append('#endif\n\n#endif /* _IPODUI_USBSCREEN_DATA_H_ */\n')
    with open(path, 'w') as fp:
        fp.write(''.join(o))
    prokbox_header.stamp(path)


def regeometry(d, cy):
    """--review: everything scaled to a d px disc centred at (160, cy),
    for owner-round mock-ups only (no header is written)."""
    global DISC_D, DISC_CY, BOX_X, BOX_Y, R2_OUT, R2_IN, RM, ARC_W
    global HEAD_HW, HEAD_L, RM16, HW16, DISC_IDX_MIN, DISC_IDX_MAX
    global DISC_STOPS, DOCK30, DOCK30_FILL, USBC, USBC_FILL
    k = d / float(DISC_D)

    def rr(rects):
        out = []
        for x0, y0, x1, y1, rad in rects:
            rad = tuple(round(v * k) for v in rad) if isinstance(rad, tuple) \
                else round(rad * k)
            out.append((round(x0 * k), round(y0 * k), round(x1 * k),
                        round(y1 * k), rad))
        return out
    DISC_D, DISC_CY = d, cy
    BOX_X, BOX_Y = DISC_CX - d // 2, cy - d // 2
    R2_OUT, R2_IN = (d + 1) ** 2, (d - 1) ** 2
    RM, ARC_W, HEAD_HW, HEAD_L = RM * k, ARC_W * k, HEAD_HW * k, HEAD_L * k
    RM16, HW16 = round(RM * 16), round(ARC_W / 2 * 16)
    DISC_STOPS = [(round(y * k), c) for y, c in DISC_STOPS]
    DISC_IDX_MIN, DISC_IDX_MAX = DISC_STOPS[0][0], DISC_STOPS[-1][0]
    DOCK30 = (round(DOCK30[0] * k), round(DOCK30[1] * k), rr(DOCK30[2]),
              rr(DOCK30[3]))
    DOCK30_FILL = rr(DOCK30_FILL)
    USBC = (round(USBC[0] * k), round(USBC[1] * k), rr(USBC[2]), rr(USBC[3]))
    USBC_FILL = rr(USBC_FILL)


def main(argv):
    root = os.path.normpath(os.path.join(os.path.dirname(__file__), '..', '..'))
    hpath = os.path.join(root, 'apps/ipodui/usbscreen_data.h')
    pdir = os.path.join(root, 'docs/design/usb')
    i = 1
    while i < len(argv):
        if argv[i] == '--c':
            hpath = argv[i + 1]
            i += 2
        elif argv[i] == '--png':
            pdir = argv[i + 1]
            i += 2
        elif argv[i] == '--review':     # --review D CY: mock-ups only
            regeometry(int(argv[i + 1]), int(argv[i + 2]))
            hpath = None
            i += 3
        else:
            sys.exit(__doc__)

    hdr = hdr_rows()
    tab = disc_table()
    params = [arrow_params(b) for b in range(BASES)]
    lim = ring_limits()
    g_dock = glyph(DOCK30, DOCK30_FILL)
    g_usbc = glyph(USBC, USBC_FILL)
    d_dock = rle(g_dock[2])
    d_usbc = rle(g_usbc[2])
    assert unrle(d_dock, g_dock[0], g_dock[1]) == \
        [v for r in g_dock[2] for v in r]
    stop_words = [y << 24 | rgb565(hexrgb(c)) for y, c in BODY_STOPS]
    gen = fnv1a(hdr + stop_words + tab + [v for p in params for v in p]
                + d_dock + d_usbc + [RM16, HW16, R2_OUT, R2_IN] + list(lim))
    if hpath:
        write_header(hpath, hdr, tab, params, lim, g_dock, g_usbc, d_dock,
                     d_usbc, gen)

    os.makedirs(pdir, exist_ok=True)
    disc = clean_disc(tab)
    alphas = [arrow_alpha(p, lim) for p in params]
    idle_dock = glyph_over(disc, g_dock)
    idle_usbc = glyph_over(disc, g_usbc)
    write_png(os.path.join(pdir, 'usb-idle-usbc.png'), LCD_W, LCD_H,
              screen(hdr, idle_usbc))
    write_png(os.path.join(pdir, 'usb-idle-dock30.png'), LCD_W, LCD_H,
              screen(hdr, idle_dock))
    frames = []
    for k in range(FRAMES):
        f = ink_over(disc, rotated(alphas[k % BASES], k // BASES))
        frames.append(f)
        if k < FRAMES // 2:     # k + 6 is bit-identical to k
            write_png(os.path.join(pdir, 'usb-io-f%02d.png' % k), LCD_W,
                      LCD_H, screen(hdr, f))
    # sheet: idle usbc, idle dock30, frames 0..11, each 80 px box, 3x
    k = 3
    cells = [idle_usbc, idle_dock] + frames
    cols = 7
    sw, sh = cols * DISC_D, ((len(cells) + cols - 1) // cols) * DISC_D
    sheet = [0x0000] * (sw * sh)
    for n, c in enumerate(cells):
        ox, oy = (n % cols) * DISC_D, (n // cols) * DISC_D
        for y in range(DISC_D):
            for x in range(DISC_D):
                sheet[(oy + y) * sw + ox + x] = c[y * DISC_D + x]
    write_png(os.path.join(pdir, 'usb-sheet-3x.png'), sw * k, sh * k,
              magnify(sheet, sw, sh, k))
    for j in range(FRAMES // 2):
        assert frames[j] == frames[j + FRAMES // 2]
    rodata = (2 * len(hdr) + 4 * len(BODY_STOPS) + 2 * len(tab)
              + 4 * 10 * BASES)
    print('usbscreen: gen 0x%08x; rodata %d B common + dock30 %d B, '
          'usbc %d B (glyphs %dx%d, %dx%d)'
          % (gen, rodata, len(d_dock), len(d_usbc), g_dock[0], g_dock[1],
             g_usbc[0], g_usbc[1]))


# the ProkBox banner for the generated file (scripts/prokbox_header.py)
import os as _os  # noqa: E402
import sys as _sys  # noqa: E402
_sys.path.insert(0, _os.path.join(_os.path.dirname(_os.path.abspath(__file__)), '..', '..', 'scripts'))
import prokbox_header  # noqa: E402


if __name__ == '__main__':
    main(sys.argv)
