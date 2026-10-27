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

"""Generate the boot-glyph candidates (stdlib only).

    art/boot/bootlogo.py [--all|--flat|--3d] [OUTDIR]
                                        # default --all, OUTDIR out/logo
    art/boot/bootlogo.py --c ROOT       # the firmware's boot screen data

--c writes the chosen strength (BOOT_STRENGTH, medium) as C for the
bootloader and the firmware (Phase 8a, docs/phase8a-brief.md §3.1, §4.1):
ROOT/firmware/common/bootscreen_data.c and
ROOT/firmware/export/bootscreen_data.h. ROOT is normally the repo root;
the generated files are committed, so builds never need Python.
tests/bootscreen/run.sh regenerates them into a temp ROOT and requires
them to match the committed ones.

--flat writes the three flat candidates described next (unchanged
since the owner's pick); --3d writes "wheel3d", the chosen wheel in
spun metal on the boot screen's gradient, in three shading strengths
(wheel3d-<strength>-screen.png, -screen@2x.png, -glyph.png and
wheel3d-contact.png; see the 3d section below).

Three original, abstract glyphs for the boot screen (POL-1, issues #7
and #16). Each is a monochrome shape drawn white on black and centred
on the 320 x 240 screen, like the stock boot screen:

    wheel    a broad ring with four small holes at the compass points
             and an open centre: the click wheel, abstracted
    groove   a record seen face-on: a disc with concentric grooves that
             thin out and vanish along a diagonal sheen, and a spindle
    pbox     a soft geometric monogram for the working name: a "P"
             whose bowl is a rounded box

Every shape is a union/difference of circles and rounded rectangles in
design units (the glyph is about 100 units tall, origin at its centre),
rasterised at any scale with SS x SS supersampling. No reference pixels
and no third-party marks are used.

Per variant the script writes (all PNGs; the screens are exactly what
the RGB565 LCD shows):

    <v>-screen.png      320 x 240 boot screen, glyph coverage quantised
                        to the 16 levels of Rockbox's 4-bit alpha format
                        and the colours to RGB565
    <v>-screen@2x.png   the same drawn at 640 x 480 from the vectors,
                        for inspection
    <v>-glyph.png       the glyph alone, cropped (bootloader size)
    <v>-glyph-1bit.png  1-bit fallback (coverage >= 50 %), cropped
    contact.png         all three side by side: 2x screen, then the
                        1x glyph and the 1-bit glyph magnified 2x

and prints the byte cost of each bitmap encoding the firmware can draw.
"""
import math
import os
import struct
import sys
import zlib

LCD_W, LCD_H = 320, 240
SS = 6                          # supersampling per axis
GLYPH_UNITS = 100.0             # nominal glyph height, design units
GLYPH_PX = 100                  # its height on the 320 x 240 screen

# ---------------------------------------------------------------- shapes


def circle(r):
    return lambda x, y: x * x + y * y <= r * r


def ring(r_out, r_in):
    return lambda x, y: r_in * r_in <= x * x + y * y <= r_out * r_out


def rrect(x0, y0, x1, y1, r):
    """Rounded rectangle, corner radius r (y grows downwards)."""
    def f(x, y):
        if not (x0 <= x <= x1 and y0 <= y <= y1):
            return False
        cx = min(max(x, x0 + r), x1 - r)
        cy = min(max(y, y0 + r), y1 - r)
        return (x - cx) ** 2 + (y - cy) ** 2 <= r * r
    return f


def union(*fs):
    return lambda x, y: any(f(x, y) for f in fs)


def minus(a, *bs):
    return lambda x, y: a(x, y) and not any(b(x, y) for b in bs)


def wheel():
    """A broad ring (the wheel) with four button holes; the open centre
    stands for the select button, sized like the real wheel's."""
    body = ring(50.0, 21.0)
    rh = 35.5                               # mid-ring
    holes = [(lambda dx, dy: (lambda x, y: (x - dx) ** 2 + (y - dy) ** 2
                              <= 4.0 * 4.0))(dx, dy)
             for dx, dy in ((0, -rh), (rh, 0), (0, rh), (-rh, 0))]
    return minus(body, *holes), 100.0, 100.0


def groove():
    """A record: grooves taper to nothing along a diagonal sheen."""
    R = 50.0
    grooves = (45.0, 39.5, 34.0, 28.5)      # groove centre radii
    gw = 1.25                               # groove half-width at full
    label = 20.0                            # label edge (one wider gap)
    hole = 3.4                              # spindle
    sheen = math.radians(-35.0)             # sheen axis (up-right)

    def f(x, y):
        d2 = x * x + y * y
        if d2 > R * R or d2 < hole * hole:
            return False
        d = math.sqrt(d2)
        # angular distance to the sheen axis (both directions), 0..pi/2
        a = abs((math.atan2(y, x) - sheen + math.pi / 2) % math.pi
                - math.pi / 2)
        t = min(max((a - 0.18) / 0.55, 0.0), 1.0)
        w = gw * (t * t * (3 - 2 * t))      # smoothstep taper
        for g in grooves:
            if abs(d - g) < w:
                return False
        if abs(d - label) < 1.6:            # label edge never fades
            return False
        return True
    return f, 100.0, 100.0


def pbox():
    """'P' monogram: capsule stem plus a rounded-box bowl."""
    st = 17.0                               # stroke
    x0, x1 = -31.0, 31.0                    # bowl outer, 62 wide
    y0, y1 = -50.0, 12.0                    # bowl outer, 62 tall
    ro = 23.0                               # outer corner radius
    bowl = minus(rrect(x0, y0, x1, y1, ro),
                 rrect(x0 + st, y0 + st, x1 - st, y1 - st, ro - st))
    # the stem's rounded top hides inside the bowl's left side, so the
    # bowl's corner stays the only curve at the top-left
    stem = rrect(x0, y0 + ro, x0 + st, 50.0, st / 2)
    return union(bowl, stem), 62.0, 100.0


VARIANTS = (("wheel", wheel), ("groove", groove), ("pbox", pbox))

# ---------------------------------------------------------------- raster


def rasterise(shape, scale, w, h, cx, cy, half_w, half_h):
    """Coverage 0..1 per pixel of a w x h image; glyph centre at cx, cy px.

    Only the glyph's box (half_w x half_h design units around the centre,
    plus a pixel) is sampled; the rest of the field is empty."""
    inv = 1.0 / scale
    offs = [(i + 0.5) / SS for i in range(SS)]
    n = float(SS * SS)
    bx0 = max(0, int(cx - half_w * scale) - 1)
    bx1 = min(w, int(math.ceil(cx + half_w * scale)) + 1)
    by0 = max(0, int(cy - half_h * scale) - 1)
    by1 = min(h, int(math.ceil(cy + half_h * scale)) + 1)
    cov = []
    for py in range(h):
        row = [0.0] * w
        cov.append(row)
        if not by0 <= py < by1:
            continue
        for px in range(bx0, bx1):
            c = 0
            for oy in offs:
                y = (py + oy - cy) * inv
                for ox in offs:
                    if shape((px + ox - cx) * inv, y):
                        c += 1
            row[px] = c / n
    return cov


def quant_alpha4(c):
    """Coverage -> the nearest of Rockbox's 16 alpha levels (0..15)."""
    return int(round(c * 15))


def rgb565(v):
    """8-bit grey -> what an RGB565 panel shows (per channel)."""
    r = (v >> 3) << 3 | v >> 5
    g = (v >> 2) << 2 | v >> 6
    return (r, g, r)


def grey_from_alpha(a):
    return rgb565(round(255 * a / 15.0))

# ---------------------------------------------------------------- png


def write_png(path, w, h, rows):
    """rows: list of rows of (r, g, b) tuples."""
    raw = bytearray()
    for row in rows:
        raw.append(0)
        for r, g, b in row:
            raw += bytes((r, g, b))

    def chunk(tag, data):
        c = struct.pack('>I', len(data)) + tag + data
        return c + struct.pack('>I', zlib.crc32(tag + data) & 0xffffffff)
    png = b'\x89PNG\r\n\x1a\n'
    png += chunk(b'IHDR', struct.pack('>IIBBBBB', w, h, 8, 2, 0, 0, 0))
    png += chunk(b'IDAT', zlib.compress(bytes(raw), 9))
    png += chunk(b'IEND', b'')
    with open(path, 'wb') as fp:
        fp.write(png)

# ---------------------------------------------------------------- main


def crop_box(cov):
    ys = [y for y, r in enumerate(cov) if any(c > 0 for c in r)]
    xs = [x for x in range(len(cov[0])) if any(r[x] > 0 for r in cov)]
    return xs[0], ys[0], xs[-1] + 1, ys[-1] + 1


def main_flat(out):
    scale1 = GLYPH_PX / GLYPH_UNITS
    sheets = []
    print('%-7s %9s %8s %8s %8s %8s' % ('variant', 'glyph', 'rgb565',
                                        'alpha4', 'mono1', 'screen'))
    for name, fn in VARIANTS:
        shape, uw, uh = fn()
        # 1x screen; the glyph centre on whole pixels keeps it symmetric
        cx, cy = LCD_W / 2.0, LCD_H / 2.0
        cov = rasterise(shape, scale1, LCD_W, LCD_H, cx, cy,
                        uw / 2, uh / 2)
        a4 = [[quant_alpha4(c) for c in r] for r in cov]
        screen = [[grey_from_alpha(a) for a in r] for r in a4]
        write_png(os.path.join(out, name + '-screen.png'),
                  LCD_W, LCD_H, screen)

        # 2x, re-rasterised from the vectors (not upscaled)
        cov2 = rasterise(shape, 2 * scale1, 2 * LCD_W, 2 * LCD_H,
                         2 * cx, 2 * cy, uw / 2, uh / 2)
        screen2 = [[grey_from_alpha(quant_alpha4(c)) for c in r]
                   for r in cov2]
        write_png(os.path.join(out, name + '-screen@2x.png'),
                  2 * LCD_W, 2 * LCD_H, screen2)

        # cropped glyph (4-bit alpha) and the 1-bit fallback
        x0, y0, x1, y1 = crop_box([[c if a else 0 for c, a in zip(r, ar)]
                                   for r, ar in zip(cov, a4)])
        gw, gh = x1 - x0, y1 - y0
        glyph = [r[x0:x1] for r in screen[y0:y1]]
        write_png(os.path.join(out, name + '-glyph.png'), gw, gh, glyph)
        mono = [[(255, 255, 255) if c >= 0.5 else (0, 0, 0)
                 for c in r[x0:x1]] for r in cov[y0:y1]]
        write_png(os.path.join(out, name + '-glyph-1bit.png'), gw, gh, mono)

        rgb = gw * gh * 2                   # bmp2rb -f 4, native RGB565
        alpha = (gw + (gw & 1)) * gh // 2   # 4-bit alpha, even rows
        mono1 = gw * ((gh + 7) // 8)        # bmp2rb -f 0, 8 rows per byte
        print('%-7s %4dx%-4d %8d %8d %8d %8d' % (name, gw, gh, rgb, alpha,
                                                 mono1, LCD_W * LCD_H * 2))
        sheets.append((screen2, glyph, mono, gw, gh))

    # contact sheet: 2x screens in a row; under each, the 1x glyph and
    # the 1-bit glyph magnified 2x (nearest), so stair-steps show
    pad, gap = 24, 24
    bg = (40, 40, 40)
    gh_max = max(s[4] for s in sheets)
    sw = 3 * 2 * LCD_W + 2 * gap + 2 * pad
    sh = pad + 2 * LCD_H + gap + 2 * gh_max + gap + 12 + pad
    sheet = [[bg] * sw for _ in range(sh)]

    def blit(img, x, y, mag=1):
        for j, r in enumerate(img):
            for i, p in enumerate(r):
                for dy in range(mag):
                    row = sheet[y + j * mag + dy]
                    for dx in range(mag):
                        row[x + i * mag + dx] = p

    for k, (screen2, glyph, mono, gw, gh) in enumerate(sheets):
        x = pad + k * (2 * LCD_W + gap)
        blit(screen2, x, pad)
        y = pad + 2 * LCD_H + gap
        half = LCD_W                         # each column is 640 wide
        blit(glyph, x + (half - 2 * gw) // 2, y, 2)
        blit(mono, x + half + (half - 2 * gw) // 2, y, 2)
        # variant number as 1..3 dots, centred under the column
        dy = y + 2 * gh_max + gap
        for d in range(k + 1):
            dx = x + LCD_W - (k * 16) // 2 + d * 16
            for j in range(-4, 5):
                for i in range(-4, 5):
                    if i * i + j * j <= 16:
                        sheet[dy + 4 + j][dx + i] = (230, 230, 230)
    write_png(os.path.join(out, 'contact.png'), sw, sh, sheet)


# ---------------------------------------------------------------- 3d
#
# "wheel3d": the chosen wheel glyph rendered as polished, spun metal on
# the boot screen's blue-grey gradient (owner, 2026-10-07). Everything is
# procedural: the ring's signed distance field gives a bevel normal, a
# top light shades it, a conic term gives the spun-metal sheen, the dots
# are dished-in bowls, the centre is cut through, and a soft shadow falls
# on the background (below the ring and inside the top of the centre).
#
# The glyph is pre-composited onto the fixed gradient, so the firmware
# draws an opaque RGB565 block; see docs/design/boot-logo.md.

# Background: vertical gradient, sampled from the owner's photo of a 7G
# booting (centre columns, camera white balance and saturation boost
# corrected, see boot-logo.md). (row, (r, g, b)), linear in between.
BG_STOPS = ((0, (87, 150, 180)),
            (60, (72, 134, 168)),
            (120, (57, 109, 147)),
            (180, (38, 72, 103)),
            (239, (38, 70, 98)))

W3D_PX = 68                     # ring diameter on the 320 x 240 screen
W3D_CX, W3D_CY = 160.0, 117.0   # its centre (pixel corners: symmetric)
W3D_SS = 5                      # supersampling per axis

# Ring geometry in units of the outer radius (same as wheel() above).
W3D_RIN = 21.0 / 50.0
W3D_RMID = 35.5 / 50.0
W3D_RDOT = 4.5 / 50.0           # a touch larger than flat (4.0): reads
                                # as a dish at 68 px

# Metal base colour down the glyph (t = 0 top .. 1 bottom): warm white,
# neutral silver, cool blue-grey (the "lower, cooler" shading).
METAL = ((0.00, (250, 247, 238)),
         (0.45, (204, 210, 219)),
         (1.00, (122, 140, 166)))
ENV_BLUE = (64, 112, 156)       # what a down-facing bevel reflects

STRENGTHS = (
    # name         bevel slope, width px, diffuse, spec, sheen, gloss,
    #              inner AO, env, shadow, shadow dy, blur, metal contrast
    ('subtle', dict(slope=1.1, width=1.5, kd=0.55, ks=0.45, sheen=0.12, grain=0.012,
                    gloss=0.07, ao=0.16, env=0.22, shadow=0.30, sdy=1.0,
                    blur=1.4, contrast=0.80, bowl=0.9)),
    ('medium', dict(slope=1.5, width=1.8, kd=0.70, ks=0.70, sheen=0.18, grain=0.02,
                    gloss=0.13, ao=0.24, env=0.32, shadow=0.42, sdy=1.4,
                    blur=1.7, contrast=1.00, bowl=1.2)),
    ('pronounced', dict(slope=2.0, width=2.1, kd=0.85, ks=1.00, sheen=0.26, grain=0.028,
                        gloss=0.22, ao=0.32, env=0.42, shadow=0.55,
                        sdy=1.8, blur=2.0, contrast=1.15, bowl=1.5)),
)


def lerp_stops(stops, t):
    if t <= stops[0][0]:
        return stops[0][1]
    for (t0, c0), (t1, c1) in zip(stops, stops[1:]):
        if t <= t1:
            f = (t - t0) / float(t1 - t0)
            return tuple(a + (b - a) * f for a, b in zip(c0, c1))
    return stops[-1][1]


def bg8(row):
    """Background colour (floats) of screen row `row` (may be fractional)."""
    return lerp_stops(BG_STOPS, row)


def _norm(x, y, z):
    n = math.sqrt(x * x + y * y + z * z)
    return x / n, y / n, z / n


LIGHT = _norm(-0.30, -0.85, 0.75)           # top, slightly left
HALF = _norm(LIGHT[0], LIGHT[1], LIGHT[2] + 1.0)
SPEC_POW = 40


def smoothstep(a, b, x):
    t = min(max((x - a) / (b - a), 0.0), 1.0)
    return t * t * (3 - 2 * t)


def ring_sdf(x, y, R):
    """Signed distance (px, + inside) to the ring, and its inward
    gradient (unit vector towards increasing distance)."""
    r = math.sqrt(x * x + y * y) or 1e-9
    d_out = R - r
    d_in = r - R * W3D_RIN
    if d_out < d_in:
        return d_out, -x / r, -y / r, d_in
    return d_in, x / r, y / r, d_in


def shade3d(x, y, R, u, P):
    """Colour of the metal at (x, y) px from the ring centre, or None
    outside it. u: render scale (1 = LCD pixels)."""
    d, gx, gy, d_in = ring_sdf(x, y, R)
    if d <= 0:
        return None
    nx, ny, nz = 0.0, 0.0, 1.0
    w = P['width'] * u
    if d < w:                               # rounded bevel
        s = P['slope'] * 2.0 * (1.0 - d / w)
        nx, ny = -s * gx, -s * gy
    occl = 1.0
    rm, rd = R * W3D_RMID, R * W3D_RDOT
    for cx, cy in ((0, -rm), (rm, 0), (0, rm), (-rm, 0)):
        qx, qy = x - cx, y - cy
        if qx * qx + qy * qy < rd * rd:     # dished-in dot
            a = P['bowl']
            # a dish: its upper wall faces down (dark), its lower wall up
            nx, ny, nz = -a * qx / rd, -a * qy / rd, 1.0
            occl = 0.86
            break
    nx, ny, nz = _norm(nx, ny, nz)
    # base metal: vertical, with the contrast setting about the middle
    t = (y + R) / (2 * R)
    base = lerp_stops(METAL, t)
    mid = METAL[1][1]
    base = [m + (b - m) * P['contrast'] for b, m in zip(base, mid)]
    diff = (nx * LIGHT[0] + ny * LIGHT[1] + nz * LIGHT[2]) / LIGHT[2]
    k = (1.0 + P['kd'] * (diff - 1.0)) * occl
    # spun-metal sheen: two bright lobes (upper left, lower right)
    th = math.atan2(y, x)
    sh = math.cos(2.0 * (th + 3.0 * math.pi / 4.0))
    sh = 0.55 * sh + 0.45 * sh ** 9         # broad tone + narrow streak
    k *= 1.0 + P['sheen'] * sh
    # spun grain: concentric bands of slightly varying brightness
    band = int(math.sqrt(x * x + y * y) * 1.5 / u)
    k *= 1.0 + P['grain'] * (((band * 2654435761) >> 7 & 255) / 127.5 - 1)
    # darker inner edge (occlusion towards the cut-through centre)
    k *= 1.0 - P['ao'] * max(0.0, 1.0 - d_in / (3.0 * u))
    c = [min(255.0, max(0.0, v * k)) for v in base]
    # down-facing bevel reflects the blue surroundings
    if ny > 0:
        e = P['env'] * min(1.0, ny * 1.6)
        c = [v + (b - v) * e for v, b in zip(c, ENV_BLUE)]
    # specular on the bevel (zero on the flat face)
    nh = max(0.0, nx * HALF[0] + ny * HALF[1] + nz * HALF[2])
    sp = P['ks'] * max(0.0, nh ** SPEC_POW - HALF[2] ** SPEC_POW)
    # glossy cap over the upper half (Aqua-style), soft-edged
    ex, ey = x / (0.98 * R), (y + 0.50 * R) / (0.62 * R)
    gl = P['gloss'] * (1.0 - smoothstep(0.80, 1.0, ex * ex + ey * ey)) \
        * (1.0 - smoothstep(-0.2 * R, 0.1 * R, y))
    a = min(1.0, sp + gl)
    return [v + (255.0 - v) * a for v in c]


def render3d(P, u, w, h, cx, cy, row_of):
    """8-bit float image (w x h) of the screen at scale u: gradient
    background, shadow, metal. row_of(py) -> LCD row for the gradient."""
    R = W3D_PX / 2.0 * u
    img = [[bg8(row_of(py + 0.5))] * w for py in range(h)]
    m = int(math.ceil((P['sdy'] + P['blur']) * u)) + 2
    x0, x1 = int(cx - R) - m, int(math.ceil(cx + R)) + m
    y0, y1 = int(cy - R) - m, int(math.ceil(cy + R)) + m
    offs = [(i + 0.5) / W3D_SS for i in range(W3D_SS)]
    n = float(W3D_SS * W3D_SS)
    sdy, blur = P['sdy'] * u, P['blur'] * u
    for py in range(max(0, y0), min(h, y1)):
        row = img[py] = list(img[py])
        for px in range(max(0, x0), min(w, x1)):
            acc = [0.0, 0.0, 0.0]
            for oy in offs:
                y = py + oy - cy
                bg = bg8(row_of(py + oy))
                for ox in offs:
                    x = px + ox - cx
                    c = shade3d(x, y, R, u, P)
                    if c is None:           # background, maybe in shadow
                        sd = ring_sdf(x, y - sdy, R)[0]
                        f = 1.0 - P['shadow'] * smoothstep(-blur, blur, sd)
                        c = [v * f for v in bg]
                    acc[0] += c[0]
                    acc[1] += c[1]
                    acc[2] += c[2]
            row[px] = tuple(v / n for v in acc)
    return img


# 2 x 2 ordered dither (screen coordinates), as the C side will draw it
BAYER2 = ((0.125, 0.625), (0.875, 0.375))


def to565(c, x, y, dither=True):
    t = BAYER2[y & 1][x & 1] if dither else 0.5
    r = min(31, int(c[0] * 31 / 255.0 + t))
    g = min(63, int(c[1] * 63 / 255.0 + t))
    b = min(31, int(c[2] * 31 / 255.0 + t))
    return (r << 11) | (g << 5) | b


def from565(v):
    r, g, b = v >> 11, (v >> 5) & 63, v & 31
    return (r << 3 | r >> 2, g << 2 | g >> 4, b << 3 | b >> 2)


def quantise(img):
    return [[to565(c, x, y) for x, c in enumerate(r)]
            for y, r in enumerate(img)]


def main3d(out):
    bgq = quantise([[bg8(y + 0.5)] * LCD_W for y in range(LCD_H)])
    cols = []
    print('\nwheel3d: %d px ring at (%g, %g); background %d stops'
          % (W3D_PX, W3D_CX, W3D_CY, len(BG_STOPS)))
    for y, c in BG_STOPS:
        v = to565(c, 0, 0, dither=False)
        print('  row %3d  rgb(%3d,%3d,%3d)  rgb565 0x%04x -> (%d,%d,%d)'
              % ((y,) + c + (v,) + from565(v)))
    print('%-11s %9s %8s %10s %8s' % ('strength', 'block', 'opaque',
                                      'rgb+alpha4', 'bg rows'))
    for name, P in STRENGTHS:
        img = render3d(P, 1, LCD_W, LCD_H, W3D_CX, W3D_CY, lambda py: py)
        q = quantise(img)
        write_png(os.path.join(out, 'wheel3d-%s-screen.png' % name), LCD_W,
                  LCD_H, [[from565(v) for v in r] for r in q])
        # the block the firmware stores: every pixel that differs from
        # the bare gradient
        diff = [[1 if a != b else 0 for a, b in zip(r, br)]
                for r, br in zip(q, bgq)]
        bx0, by0, bx1, by1 = crop_box(diff)
        bw, bh = bx1 - bx0, by1 - by0
        block = [[from565(v) for v in r[bx0:bx1]] for r in q[by0:by1]]
        write_png(os.path.join(out, 'wheel3d-%s-glyph.png' % name),
                  bw, bh, block)
        img2 = render3d(P, 2, 2 * LCD_W, 2 * LCD_H, 2 * W3D_CX, 2 * W3D_CY,
                        lambda py: py / 2.0)
        q2 = quantise(img2)
        shot2 = [[from565(v) for v in r] for r in q2]
        write_png(os.path.join(out, 'wheel3d-%s-screen@2x.png' % name),
                  2 * LCD_W, 2 * LCD_H, shot2)
        opaque = bw * bh * 2
        alpha = opaque + (bw + (bw & 1)) * bh // 2
        print('%-11s %3dx%-3d@%3d,%-3d %6d %10d %8d'
              % (name, bw, bh, bx0, by0, opaque, alpha, LCD_H * 2 * 2))
        cols.append((shot2, block, bw, bh,
                     [[from565(v) for v in r] for r in q]))

    # contact sheet: 2x screens in a row; under each, the 1x screen and
    # the 1x block magnified 3x (nearest) so the pixels show
    pad, gap, mag = 24, 24, 3
    bgc = (40, 40, 40)
    bh_max = max(c[3] for c in cols)
    sw = 3 * 2 * LCD_W + 2 * gap + 2 * pad
    sh = pad + 2 * LCD_H + gap + max(LCD_H, mag * bh_max) + gap + 12 + pad
    sheet = [[bgc] * sw for _ in range(sh)]

    def blit(img, x, y, k=1):
        for j, r in enumerate(img):
            for dy in range(k):
                row = sheet[y + j * k + dy]
                for i, p in enumerate(r):
                    for dx in range(k):
                        row[x + i * k + dx] = p

    for k, (shot2, block, bw, bh, shot1) in enumerate(cols):
        x = pad + k * (2 * LCD_W + gap)
        blit(shot2, x, pad)
        y = pad + 2 * LCD_H + gap
        blit(shot1, x, y)
        blit(block, x + LCD_W + (LCD_W - mag * bw) // 2, y, mag)
        dy = y + max(LCD_H, mag * bh_max) + gap
        for d in range(k + 1):
            dx = x + LCD_W - (k * 16) // 2 + d * 16
            for j in range(-4, 5):
                for i in range(-4, 5):
                    if i * i + j * j <= 16:
                        sheet[dy + 4 + j][dx + i] = (230, 230, 230)
    write_png(os.path.join(out, 'wheel3d-contact.png'), sw, sh, sheet)


# ---------------------------------------------------------------- --c

BOOT_STRENGTH = 'medium'        # owner, 2026-10-07

C_HEAD = r"""/***************************************************************************
 *             __________               __   ___.
 *   Open      \______   \ ____   ____ |  | _\_ |__   _______  ___
 *   Source     |       _//  _ \_/ ___\|  |/ /| __ \ /  _ \  \/  /
 *   Jukebox    |    |   (  <_> )  \___|    < | \_\ (  <_> > <  <
 *   Firmware   |____|_  /\____/ \___  >__|_ \|___  /\____/__/\_ \
 *                     \/            \/     \/    \/            \/
 *
 * GENERATED by art/boot/bootlogo.py --c (wheel3d, %s). Do not edit:
 * regenerate and commit. Phase 8a, docs/phase8a-brief.md section 4.1.
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This software is distributed on an "AS IS" basis, WITHOUT WARRANTY OF ANY
 * KIND, either express or implied.
 *
 ****************************************************************************/
"""


def fnv1a16(words, h=0x811c9dc5):
    """32-bit FNV-1a over 16-bit words as little-endian bytes."""
    for w in words:
        for b in (w & 0xff, w >> 8):
            h = ((h ^ b) * 0x01000193) & 0xffffffff
    return h


def boot_screen_data():
    """(bg [(even, odd)] * 240, block rows, x, y, w, h, gen) for
    BOOT_STRENGTH, quantised exactly as main3d() writes the screen PNG."""
    P = dict(STRENGTHS)[BOOT_STRENGTH]
    img = render3d(P, 1, LCD_W, LCD_H, W3D_CX, W3D_CY, lambda py: py)
    q = quantise(img)
    bgq = quantise([[bg8(y + 0.5)] * LCD_W for y in range(LCD_H)])
    for r in bgq:                       # the C side relies on this
        assert all(v == r[x & 1] for x, v in enumerate(r))
    bg = [(r[0], r[1]) for r in bgq]
    diff = [[1 if a != b else 0 for a, b in zip(r, br)]
            for r, br in zip(q, bgq)]
    bx0, by0, bx1, by1 = crop_box(diff)
    block = [r[bx0:bx1] for r in q[by0:by1]]
    words = [v for pair in bg for v in pair] + [v for r in block for v in r]
    return bg, block, bx0, by0, bx1 - bx0, by1 - by0, fnv1a16(words)


def main_c(root):
    bg, block, x, y, w, h, gen = boot_screen_data()
    cdir = os.path.join(root, 'firmware', 'common')
    hdir = os.path.join(root, 'firmware', 'export')
    os.makedirs(cdir, exist_ok=True)
    os.makedirs(hdir, exist_ok=True)
    head = C_HEAD % BOOT_STRENGTH
    hl = [head,
          '#ifndef BOOTSCREEN_DATA_H',
          '#define BOOTSCREEN_DATA_H',
          '',
          '/* the pre-composited glyph block: every pixel that differs from '
          'the bare',
          ' * gradient; ring centre (%g, %g) */' % (W3D_CX, W3D_CY),
          '#define BOOTSCREEN_X   %d' % x,
          '#define BOOTSCREEN_Y   %d' % y,
          '#define BOOTSCREEN_W   %d' % w,
          '#define BOOTSCREEN_H   %d' % h,
          '/* FNV-1a (32-bit) over bootscreen_bg, then bootscreen_block, as',
          ' * little-endian bytes: the identity of the art */',
          '#define BOOTSCREEN_GEN 0x%08xUL' % gen,
          '',
          '/* per row: the RGB565 colour of even x, then of odd x (2 x 2 '
          'ordered dither) */',
          'extern const unsigned short bootscreen_bg[%d][2];' % LCD_H,
          'extern const unsigned short '
          'bootscreen_block[BOOTSCREEN_W * BOOTSCREEN_H];',
          '',
          '#endif /* BOOTSCREEN_DATA_H */',
          '']
    with open(os.path.join(hdir, 'bootscreen_data.h'), 'w') as fp:
        fp.write('\n'.join(hl))
    prokbox_header.stamp(os.path.join(hdir, 'bootscreen_data.h'))
    cl = [head,
          '#include "config.h"',
          '#include "bootscreen.h"',
          '',
          'const unsigned short bootscreen_bg[%d][2] BOOTSCREEN_DATA_ATTR = {'
          % LCD_H]
    for i in range(0, LCD_H, 4):
        cl.append('   ' + ''.join(' { 0x%04x, 0x%04x },' % p
                                  for p in bg[i:i + 4]))
    cl += ['};', '',
           'const unsigned short bootscreen_block[BOOTSCREEN_W * BOOTSCREEN_H]',
           '    BOOTSCREEN_DATA_ATTR = {']
    for j, r in enumerate(block):
        cl.append('    /* y %d */' % (y + j))
        for i in range(0, w, 9):
            cl.append('   ' + ''.join(' 0x%04x,' % v for v in r[i:i + 9]))
    cl += ['};', '']
    with open(os.path.join(cdir, 'bootscreen_data.c'), 'w') as fp:
        fp.write('\n'.join(cl))
    prokbox_header.stamp(os.path.join(cdir, 'bootscreen_data.c'))
    print('bootscreen: %s %dx%d at (%d, %d), block %d B + bg %d B, gen 0x%08x'
          % (BOOT_STRENGTH, w, h, x, y, w * h * 2, LCD_H * 4, gen))


def main():
    args = sys.argv[1:]
    if args and args[0] == '--c':
        if len(args) != 2:
            sys.exit('usage: bootlogo.py --c ROOT')
        main_c(args[1])
        return
    which = 'all'
    if args and args[0] in ('--flat', '--3d', '--all'):
        which = args.pop(0)[2:]
    out = args[0] if args else 'out/logo'
    os.makedirs(out, exist_ok=True)
    if which in ('all', 'flat'):
        main_flat(out)
    if which in ('all', '3d'):
        main3d(out)


# the ProkBox banner for the generated files (scripts/prokbox_header.py)
import sys as _sys  # noqa: E402
_sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', '..', 'scripts'))
import prokbox_header  # noqa: E402


if __name__ == '__main__':
    main()
