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

"""Generate the split-screen panel artwork (stdlib only).

    apps/ipodui/art/panelart.py > apps/ipodui/panelart.h

Original drawings in the 7G's style (#38, catalogue 7.1); no reference
pixels are used. Two formats, both run-length coded so the mostly empty
art stays small:

  shading   a 4-bit metallic shading mask (Phase 8b-3, option 2 of spec
            settings.md "Rendering approach"), RLE: each byte is
            (run - 1) << 4 | value, row-major over w x h. A value is ink
            coverage times the glyph's own shading: white bits lit from
            the top left, fading toward the bottom-right corner, a light
            bevel along the edges facing the light and a darker one along
            the others, ordered-dithered so the fade has no bands. The C
            side maps each value through a per-row 16-entry palette (the
            surface colour under the row toward the row's metal colour),
            so a pixel is a table lookup and a store, never a blend.
  rgba      the same RLE stream as the alpha, plus one RGB565 colour per
            pixel whose alpha is not 0 (in raster order): pre-shaded 3-D
            art (the metal wheel, the clock face) that sits on any
            background.

Tile glyphs (TILE_G px square, centred in the 76 px tile; EQ is a wide
EQ_W x EQ_H glyph in a wide tile, owner photo settings-10-eq):
  shuffle, repeat, menu, volume, timer, sun, book, eq, speaker, clicker,
  contacts, reset
Panel art:
  podcast   the broadcast mark (head, two arcs each side, tapered body)
  film      the Videos film strip (Phase 14a; HAVE_BCMFW_VIDEO only)
  corner    the tile's rounded-corner coverage (TILE_R x TILE_R)
  wheel     the chosen boot glyph (docs/design/boot-logo.md, "wheel3d",
            medium) in spun metal, scaled down, transparent background
  clock     the Extras clock (owner photo main-menu-custom-hold): a 3-D
            white face in a bevelled silver rim with a soft drop shadow,
            in the 'grey' format below (the numerals and the hands are
            drawn live)
  map       the Extras panel's world map, longitudes 0..180 E in a
            Mercator projection (worldmap.py), a 4-bit coverage mask the
            C side colours per row

  grey      the same alpha RLE stream, plus a second RLE stream of
            4-bit indices into a 16-entry grey palette, one per pixel
            whose alpha is not 0 (greyscale art with long runs, a third
            of the 'rgba' bytes)
"""
import math
import os
import sys

sys.path.insert(0, os.path.join(os.path.dirname(__file__), '..', '..', '..',
                                'art', 'boot'))
import bootlogo as BL   # noqa: E402  (the chosen wheel's shader)
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import worldmap as WM   # noqa: E402  (the Extras map's coastlines)

SS = 6
TILE_G = 56
TILE_R = 11


def raster(w, h, inside, ss=SS):
    rows = []
    for py in range(h):
        row = []
        for px in range(w):
            n = 0
            for sy in range(ss):
                for sx in range(ss):
                    if inside(px + (sx + .5) / ss, py + (sy + .5) / ss):
                        n += 1
            row.append(min(15, (n * 15 + ss * ss // 2) // (ss * ss)))
        rows.append(row)
    return rows


# The metallic shading (8b-3): light from the top left.
SH_FADE = .22           # interior brightness lost toward the far corner
SH_HI = .10             # bevel highlight on edges facing the light
SH_LO = .26             # bevel shade on edges facing away
SH_D = 7                # bevel offset, in subsamples (~1.2 px)
BAYER4 = ((0, 8, 2, 10), (12, 4, 14, 6), (3, 11, 1, 9), (15, 7, 13, 5))


def shaded(w, h, inside, ss=SS):
    """4-bit metallic shading mask of 'inside' (px coordinates) over
    w x h: coverage x shade, dithered (docstring, 'shading')."""
    W, H = w * ss, h * ss
    grid = [[inside((i + .5) / ss, (j + .5) / ss) for i in range(W)]
            for j in range(H)]
    # summed-area table for block coverage at any subsample offset
    sat = [[0] * (W + 1) for _ in range(H + 1)]
    for j in range(H):
        acc = 0
        for i in range(W):
            acc += grid[j][i]
            sat[j + 1][i + 1] = sat[j][i + 1] + acc

    def cov(px, py, dx, dy):
        x0 = max(0, min(W, px * ss + dx))
        y0 = max(0, min(H, py * ss + dy))
        x1 = max(0, min(W, px * ss + ss + dx))
        y1 = max(0, min(H, py * ss + ss + dy))
        if x1 <= x0 or y1 <= y0:
            return 0.0
        return (sat[y1][x1] - sat[y0][x1] - sat[y1][x0]
                + sat[y0][x0]) / float(ss * ss)

    rows = []
    for py in range(h):
        row = []
        for px in range(w):
            c = cov(px, py, 0, 0)
            if c <= 0:
                row.append(0)
                continue
            hi = max(0.0, c - cov(px, py, -SH_D, -SH_D))
            lo = max(0.0, c - cov(px, py, SH_D, SH_D))
            diag = (px / max(w - 1, 1) + py / max(h - 1, 1)) / 2
            shade = (1 - SH_FADE * diag) * (1 + SH_HI * hi) * (1 - SH_LO * lo)
            v = 15 * c * min(1.0, shade)
            v = int(v + BAYER4[py & 3][px & 3] / 16.0)
            row.append(max(1 if c > .5 / 15 else 0, min(15, v)))
        rows.append(row)
    return rows


# -- shape helpers (coordinates in a 0..1 unit square, y down) -------------

def disc(cx, cy, r):
    return lambda x, y: (x - cx) ** 2 + (y - cy) ** 2 <= r * r


def ring(cx, cy, r0, r1):
    return lambda x, y: r0 * r0 <= (x - cx) ** 2 + (y - cy) ** 2 <= r1 * r1


def box(x0, y0, x1, y1):
    return lambda x, y: x0 <= x <= x1 and y0 <= y <= y1


def poly(pts):
    def f(x, y):
        inside = False
        n = len(pts)
        for i in range(n):
            x1, y1 = pts[i]
            x2, y2 = pts[(i + 1) % n]
            if (y1 > y) != (y2 > y):
                if x < x1 + (y - y1) * (x2 - x1) / (y2 - y1):
                    inside = not inside
        return inside
    return f


def seg(ax, ay, bx, by, r):
    def f(x, y):
        dx, dy = bx - ax, by - ay
        t = max(0.0, min(1.0, ((x - ax) * dx + (y - ay) * dy) /
                         (dx * dx + dy * dy)))
        return (x - ax - t * dx) ** 2 + (y - ay - t * dy) ** 2 <= r * r
    return f


def arc(cx, cy, r, w, a0, a1):
    """Stroke of width w along a circle, angles in degrees (0 = right,
    counter-clockwise, y up on screen)."""
    def f(x, y):
        dx, dy = x - cx, cy - y
        d = math.hypot(dx, dy)
        if abs(d - r) > w / 2:
            return False
        a = math.degrees(math.atan2(dy, dx)) % 360
        lo, hi = a0 % 360, a1 % 360
        return lo <= a <= hi if lo <= hi else (a >= lo or a <= hi)
    return f


def U(*fs):
    return lambda x, y: any(f(x, y) for f in fs)


def M(a, *bs):
    return lambda x, y: a(x, y) and not any(b(x, y) for b in bs)


def unit(shape, size):
    return lambda x, y: shape(x / size, y / size)


# -- tile glyphs ------------------------------------------------------------

def arrow_head(tx, ty, dx, dy, s):
    """Filled triangle pointing along (dx, dy) with its tip at tx, ty."""
    px, py = -dy, dx
    return poly([(tx, ty), (tx - dx * s + px * s * .75, ty - dy * s + py * s * .75),
                 (tx - dx * s - px * s * .75, ty - dy * s - py * s * .75)])


def g_shuffle():
    """Shuffle Off (owner photo settings-02-shuffle): two parallel right
    arrows, square-ended shafts, large heads; spans the glyph's width
    (the photo's are ~61 of 76 tile px, ours 56: same proportions)."""
    t, s = .141, .245               # shaft thickness, head length
    rows = (.2875, .7125)
    parts = []
    for y in rows:
        parts.append(box(0.0, y - t / 2, 1.0 - s + .01, y + t / 2))
        parts.append(arrow_head(1.0, y, 1, 0, s))
    return U(*parts)


def _poly_stroke(pts, r):
    return U(*[seg(pts[i][0], pts[i][1], pts[i + 1][0], pts[i + 1][1], r)
               for i in range(len(pts) - 1)])


def g_shuffle_on():
    """Shuffle Songs/Albums: the same two arrows, crossed (criss-cross);
    the one drawn under has a gap where the other passes over it."""
    t, s = .141, .245
    y0, y1 = .2875, .7125
    xe = 1.0 - s + .01
    over = _poly_stroke([(0.0, y1), (.30, y1), (.58, y0), (xe, y0)], t / 2)
    under = _poly_stroke([(0.0, y0), (.30, y0), (.58, y1), (xe, y1)], t / 2)
    # the gap: the over-stroke widened by a margin, cut from the under one
    over_wide = M(_poly_stroke([(.30, y1), (.58, y0)], t / 2 + .06),
                  lambda x, y: not disc(.44, .5, .2)(x, y))
    ends = box(-1, -1, 0.0, 2)      # square tails at the left edge
    return U(M(over, ends), M(under, over_wide, ends),
             arrow_head(1.0, y0, 1, 0, s), arrow_head(1.0, y1, 1, 0, s))


def g_repeat():
    """A loop of two arrows (owner photo settings-03-repeat): the top one
    runs up the left side and right along the top, its head at the top
    right; the bottom one runs down the right side and left along the
    bottom, its head at the bottom left. Each head stops short of the
    other arrow's tail: a small diagonal gap, the arrows never touch."""
    w, s, g = .11, .22, .035          # stroke, head length, gap
    head_r = arrow_head(.86, .30, 1, 0, s)
    head_l = arrow_head(.14, .70, -1, 0, s)
    # each head, grown by the gap: cut from the other arrow's tail
    cut_r = arrow_head(.86 + g * 1.6, .30, 1, 0, s + g * 2.6)
    cut_l = arrow_head(.14 - g * 1.6, .70, -1, 0, s + g * 2.6)
    return U(M(arc(.32, .5, .2, w, 90, 270), cut_l),   # left side, up
             seg(.32, .30, .66, .30, w / 2), head_r,
             M(arc(.68, .5, .2, w, 270, 90), cut_r),   # right side, down
             seg(.34, .70, .68, .70, w / 2), head_l)


def g_menu():
    parts = []
    for y in (.26, .5, .74):
        parts.append(box(.12, y - .07, .66, y + .07))
        parts.append(seg(.76, y - .07, .84, y, .03))
        parts.append(seg(.84, y, .76, y + .07, .03))
    return U(*parts)


def speaker(x0, s):
    return U(box(x0, .40, x0 + .14 * s, .60),
             poly([(x0 + .12 * s, .40), (x0 + .34 * s, .20),
                   (x0 + .34 * s, .80), (x0 + .12 * s, .60)]))


def ellipse(cx, cy, rx, ry):
    return lambda x, y: ((x - cx) / rx) ** 2 + ((y - cy) / ry) ** 2 <= 1


def g_volume():
    """Owner photo settings-06-volume-limit: a speaker seen a little
    from the side (a rounded magnet, the cone widening to a big
    elliptic rim with its dust cap) and a level bar, filled below the
    limit, with a small pointer either side at the limit."""
    magnet = M(ellipse(.15, .432, .10, .109), box(.15, 0, 1, 1))
    cone = poly([(.172, .30), (.466, .052), (.466, .839), (.172, .563)])
    rim = ellipse(.466, .446, .10, .394)
    speaker_ = U(magnet,
                 M(U(cone, rim), ellipse(.466, .446, .052, .336)),
                 ellipse(.47, .44, .036, .09))
    bar = M(box(.744, .134, .866, .771), box(.770, .160, .840, .323))
    marks = U(poly([(.663, .29), (.712, .33), (.663, .37)]),
              poly([(.955, .29), (.906, .33), (.955, .37)]))
    return U(speaker_, bar, marks)


def g_timer():
    """Backlight (photo settings-07): a thick ring round a dark face, a
    lit wedge from twelve to four o'clock."""
    def wedge(x, y):
        a = math.degrees(math.atan2(x - .5, .5 - y)) % 360
        return (x - .5) ** 2 + (y - .5) ** 2 <= .33 ** 2 and a <= 120
    return U(ring(.5, .5, .38, .47), wedge)


def g_sun():
    rays = [seg(.5 + .30 * math.cos(a), .5 + .30 * math.sin(a),
                .5 + .44 * math.cos(a), .5 + .44 * math.sin(a), .035)
            for a in [i * math.pi / 4 for i in range(8)]]
    return U(disc(.5, .5, .2), *rays)


def g_book():
    left = poly([(.08, .28), (.46, .34), (.46, .80), (.08, .74)])
    right = poly([(.54, .34), (.92, .28), (.92, .74), (.54, .80)])
    lines = [box(.14, y, .40, y + .025) for y in (.42, .50, .58)] + \
            [box(.60, y, .86, y + .025) for y in (.42, .50, .58)]
    return M(U(left, right), *lines)


def g_eq():
    hs = (.28, .38, .44, .56, .60, .64, .62, .56)
    return U(*[box(.06 + i * .113, .80 - h, .06 + i * .113 + .085, .80)
               for i, h in enumerate(hs)])


EQ_W, EQ_H = 116, 40     # the wide EQ tile's glyph (settings-10-eq)
EQ_TILE_W = 128


def g_eq_wide():
    """EQ (owner photo settings-10-eq, #52): eight upright bars across a
    wide tile, rising toward the right of centre and falling a little at
    the end; our own heights. Pixel coordinates over EQ_W x EQ_H."""
    hs = (18, 25, 28, 33, 36, 38, 35, 31)
    bw, gap = 11.5, (EQ_W - 8 * 11.5) / 7.0
    return U(*[box(i * (bw + gap), EQ_H - hv, i * (bw + gap) + bw, EQ_H)
               for i, hv in enumerate(hs)])


def g_speaker():
    return U(speaker(.14, 1.1), arc(.52, .5, .16, .06, -50, 50),
             arc(.52, .5, .28, .06, -50, 50))


def g_clicker():
    body = M(poly([(.30, .10), (.70, .10), (.70, .90), (.30, .90)]),
             box(.35, .16, .65, .42), ring(.5, .66, 0, .07))
    wheel = ring(.5, .66, .11, .15)
    waves = [arc(.5, .5, r, .045, a0, a1) for r in (.30, .40)
             for a0, a1 in ((-40, 40), (140, 220))]
    return U(M(body, wheel), *waves)


def g_contacts():
    cover = M(box(.22, .10, .82, .90), box(.30, .16, .76, .84))
    rings = [box(.16, y, .30, y + .05) for y in (.24, .42, .60, .76)]
    head = disc(.53, .40, .09)
    shoulders = M(disc(.53, .70, .17), box(0, .70, 1, 1))
    return U(cover, *rings, head, shoulders)


def g_reset():
    return U(arc(.5, .52, .30, .10, 120, 420 - 60),
             arrow_head(.5 + .30 * math.cos(math.radians(120)) - .02,
                        .52 - .30 * math.sin(math.radians(120)) + .03,
                        .55, .83, .19))


GLYPHS = (('shuffle', g_shuffle), ('shuffle_on', g_shuffle_on),
          ('repeat', g_repeat), ('menu', g_menu),
          ('volume', g_volume), ('timer', g_timer), ('sun', g_sun),
          ('book', g_book), ('eq', g_eq), ('speaker', g_speaker),
          ('clicker', g_clicker), ('contacts', g_contacts),
          ('reset', g_reset))


def g_podcast():
    head = disc(.5, .40, .11)
    body = poly([(.40, .56), (.60, .56), (.55, .96), (.45, .96)])
    body = U(body, disc(.5, .58, .10))
    arcs = [arc(.5, .42, r, .055, -40, 220) for r in (.24, .37)]
    gaps = [M(a, lambda x, y: y > .52) for a in arcs]
    return U(head, body, *gaps)


FILM_W, FILM_H = 64, 70


def g_film():
    """The Videos panel's film strip (Phase 14a, our own drawing): an
    upright strip, a rail of sprocket holes down each side, two frames.
    Pixel coordinates over FILM_W x FILM_H."""
    w, h, t = FILM_W, FILM_H, 2.6          # stroke
    rail = 12.0                            # rail width (outer edge to frame)
    outer = M(box(.5, .5, w - .5, h - .5), box(.5 + t, .5 + t,
                                               w - .5 - t, h - .5 - t))
    lines = U(box(rail - t / 2, .5, rail + t / 2, h - .5),
              box(w - rail - t / 2, .5, w - rail + t / 2, h - .5),
              box(rail, h / 2 - t / 2 - 2.4, w - rail, h / 2 - t / 2),
              box(rail, h / 2 + t / 2, w - rail, h / 2 + t / 2 + 2.4))
    holes = []
    n = 7
    pitch = (h - 2 * t) / n
    for i in range(n):
        y0 = t + i * pitch + pitch * .28
        y1 = y0 + pitch * .44
        holes.append(box(t + 2.2, y0, rail - t / 2 - 2.2, y1))
        holes.append(box(w - rail + t / 2 + 2.2, y0, w - t - 2.2, y1))
    return U(outer, lines, *holes)


# -- encoders -----------------------------------------------------------------

def rle(rows):
    out, flat = [], [v for r in rows for v in r]
    i = 0
    while i < len(flat):
        v, n = flat[i], 1
        while i + n < len(flat) and flat[i + n] == v and n < 16:
            n += 1
        out.append((n - 1) << 4 | v)
        i += n
    return out


def emit_bytes(name, data):
    print('static const unsigned char %s[%d] = {' % (name, len(data)))
    for i in range(0, len(data), 12):
        print('    ' + ', '.join('0x%02x' % b for b in data[i:i + 12]) + ',')
    print('};')


def emit_shorts(name, data):
    print('static const unsigned short %s[%d] = {' % (name, len(data)))
    for i in range(0, len(data), 8):
        print('    ' + ', '.join('0x%04x' % b for b in data[i:i + 8]) + ',')
    print('};')


def rgba_art(w, h, shade):
    """shade(x, y) -> colour tuple or None, x/y in px from the centre.
    Returns (alpha rows, colours) with SS x SS supersampling."""
    alpha, cols = [], []
    offs = [(i + .5) / SS for i in range(SS)]
    for py in range(h):
        row = []
        for px in range(w):
            acc, n = [0.0, 0.0, 0.0], 0
            for oy in offs:
                for ox in offs:
                    c = shade(px + ox - w / 2.0, py + oy - h / 2.0)
                    if c is not None:
                        n += 1
                        for k in range(3):
                            acc[k] += c[k]
            a = (n * 15 + SS * SS // 2) // (SS * SS)
            row.append(a)
            if a:
                c = [v / n for v in acc]
                cols.append(BL.to565(c, px, py, dither=False))
        alpha.append(row)
    return alpha, cols


WHEEL_PX = 60


def wheel_shade():
    P = dict(BL.STRENGTHS)['medium']
    R = WHEEL_PX / 2.0 - 1
    u = WHEEL_PX / float(BL.W3D_PX)
    return lambda x, y: BL.shade3d(x, y, R, u, P)


CLOCK_PX = 96          # the shadow's margin included
CLOCK_R = 45.5         # rim outer radius (~91 px across, as the photo)
CLOCK_RIM = 5.5        # rim width
CLOCK_SH_DY = 1.5      # drop shadow offset down
# 16 greys: 0 is the shadow's black; the rest are spaced closer in the
# rim's range so its vertical sweep shows no steps
CLOCK_GREYS = (0, 96, 120, 140, 156, 170, 182, 193, 203, 212, 220, 228,
               236, 243, 249, 255)


def clock_shade(x, y):
    """(grey 0..255, is_shadow) of a sample at x, y px from the centre,
    or None outside the art."""
    R = CLOCK_R
    d = math.hypot(x, y)
    if d <= R:
        if d > R - .8:                      # the rim's dark outer line
            return 118
        if d > R - CLOCK_RIM:               # bevelled silver, lit above
            t = (y / R + 1) / 2             # 0 top .. 1 bottom
            u = (d - (R - CLOCK_RIM)) / CLOCK_RIM
            return 246 - 92 * t + 18 * (u - .5) * (1 - 2 * t)
        if d > R - CLOCK_RIM - 1.6:         # the lip: shadowed above
            t = (y / R + 1) / 2
            return 150 + 90 * t
        # the face: white, an inner shadow under the top of the rim, a
        # faint fall toward the lower right
        v = 252 - 6 * max(0.0, (x + y) / (R * 1.4))
        inner = R - CLOCK_RIM - 1.6 - d     # px inside the lip
        if y < 0 and inner < 5:
            v -= (5 - inner) * 5 * (-y / R)
        return v
    ds = math.hypot(x, y - CLOCK_SH_DY)
    if ds < R + 3:
        return -max(0.0, min(1.0, (R + 3 - ds) / 4.0)) * .45   # shadow
    return None


def grey_art(px, shade):
    """(alpha rows, grey index stream) of a px x px greyscale object,
    SS x SS samples; shade(x, y) -> grey 0..255 (opaque) or -a (black at
    alpha a, a shadow) or None."""
    alpha, idx = [], []
    offs = [(i + .5) / SS for i in range(SS)]
    for py in range(px):
        row = []
        for qx in range(px):
            acc, n, sh = 0.0, 0, 0.0
            for oy in offs:
                for ox in offs:
                    v = shade(qx + ox - px / 2.0, py + oy - px / 2.0)
                    if v is None:
                        continue
                    if v < 0:
                        sh += -v
                    else:
                        acc += v
                        n += 1
            body = n / float(SS * SS)
            a = body + sh / float(SS * SS)
            a = int(a * 15 + .5)
            if body < .5 / 15:
                # shadow only: three alpha steps keep its runs long
                a = min(15, int(a / 2 + .5) * 2)
                row.append(a)
                if a:
                    idx.append(0)
                continue
            row.append(max(1, min(15, a)))
            g = acc / n
            idx.append(min(range(1, 16),
                           key=lambda k: abs(CLOCK_GREYS[k] - g)))
        alpha.append(row)
    return alpha, idx


MAP_Y0 = 20            # panel row of the mask's first row
MAP_H = 180
MAP_EQ = 148           # the equator's panel row


def map_mask(west=False):
    """The east half (lon 0..180, the Extras panel), or with west the
    Clocks page's west half (lon -180..0, clockart.py), same geometry."""
    R = 160 / math.pi                       # 180 degrees over 160 px
    lon0 = -180.0 if west else 0.0

    def proj(lon, lat):
        lat = max(-85.0, min(85.0, lat))
        return ((lon - lon0) * 160 / 180.0, MAP_EQ - MAP_Y0 -
                R * math.log(math.tan(math.pi / 4 + math.radians(lat) / 2)))
    cov = WM.rasterise(160, MAP_H, proj,
                       rings=WM.outlines_west() if west else None)
    return [[min(15, int(c * 15 + .5)) for c in r] for r in cov]


def main():
    print('/* Generated by apps/ipodui/art/panelart.py -- do not edit.')
    print(' * Original artwork; formats in the generator\'s docstring. */')
    print('#ifndef _IPODUI_PANELART_H_')
    print('#define _IPODUI_PANELART_H_\n')
    print('#define PANELART_TILE_G %d' % TILE_G)
    print('#define PANELART_TILE_R %d\n' % TILE_R)
    print('#define PANELART_EQ_TILE_W %d\n' % EQ_TILE_W)
    total = 0
    names, dims = [], []
    for name, fn in GLYPHS:
        if name == 'eq':
            w, h, shape = EQ_W, EQ_H, g_eq_wide()
        else:
            w, h, shape = TILE_G, TILE_G, unit(fn(), TILE_G)
        data = rle(shaded(w, h, shape))
        emit_bytes('glyph_%s' % name, data)
        names.append(name)
        dims.append((w, h))
        total += len(data)
    print('\nenum panelart_glyph {')
    for n in names:
        print('    PANELART_%s,' % n.upper())
    print('    PANELART_GLYPHS\n};')
    print('static const unsigned char *const panelart_glyphs[] = {')
    for n in names:
        print('    glyph_%s,' % n)
    print('};')
    print('/* each glyph\'s size (the tile is wider for a wider glyph) */')
    print('static const unsigned char panelart_glyph_wh[][2] = {')
    for (w, h), n in zip(dims, names):
        print('    { %d, %d },   /* %s */' % (w, h, n))
    print('};\n')

    # rounded corner: coverage of a quarter disc, the corner at 0,0
    r = TILE_R
    corner = raster(r, r, lambda x, y: (r - x) ** 2 + (r - y) ** 2 <= r * r
                    or x >= r or y >= r)
    print('static const unsigned char panelart_corner[%d] = {' % (r * r))
    for row in corner:
        print('    ' + ', '.join('%2d' % v for v in row) + ',')
    print('};\n')

    pod = 80
    data = rle(shaded(pod, pod, unit(g_podcast(), pod)))
    print('#define PANELART_PODCAST_W %d' % pod)
    emit_bytes('panelart_podcast', data)
    total += len(data)

    # Phase 14a: only video builds draw it (the default build's objects
    # stay as they were)
    data = rle(shaded(FILM_W, FILM_H, g_film()))
    print('\n#ifdef HAVE_BCMFW_VIDEO')
    print('#define PANELART_FILM_W %d' % FILM_W)
    print('#define PANELART_FILM_H %d' % FILM_H)
    emit_bytes('panelart_film', data)
    print('#endif')
    total += len(data)

    alpha, cols = rgba_art(WHEEL_PX, WHEEL_PX, wheel_shade())
    a = rle(alpha)
    print('\n#define PANELART_WHEEL_W %d' % WHEEL_PX)
    emit_bytes('panelart_wheel_a', a)
    emit_shorts('panelart_wheel_rgb', cols)
    total += len(a) + 2 * len(cols)

    alpha, idx = grey_art(CLOCK_PX, clock_shade)
    a, g = rle(alpha), rle([idx])
    print('\n#define PANELART_CLOCK_W %d' % CLOCK_PX)
    emit_bytes('panelart_clock_a', a)
    emit_bytes('panelart_clock_g', g)
    emit_shorts('panelart_clock_pal',
                [BL.to565((v, v, min(255, v + 4)), 0, 0, dither=False)
                 for v in CLOCK_GREYS])
    total += len(a) + len(g) + 32

    data = rle(map_mask())
    print('\n#define PANELART_MAP_Y0 %d' % MAP_Y0)
    print('#define PANELART_MAP_H %d' % MAP_H)
    emit_bytes('panelart_map', data)
    total += len(data)
    print('\n/* %d bytes of art */' % total)
    print('#endif /* _IPODUI_PANELART_H_ */')


# the ProkBox banner for the generated file (scripts/prokbox_header.py)
import os as _os  # noqa: E402
import sys as _sys  # noqa: E402
_sys.path.insert(0, _os.path.join(_os.path.dirname(_os.path.abspath(__file__)), '..', '..', '..', 'scripts'))
import prokbox_header  # noqa: E402


if __name__ == '__main__':
    prokbox_header.run_stdout(main)
