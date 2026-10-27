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

"""Generate the Extras rows' panel icons and the games' box art (stdlib
only).

    apps/ipodui/art/extrasart.py > apps/ipodui/extrasart.h

Owner, 2026-10-24: on the 7G every Extras row shows its own large icon in
the split screen's panel (owner photos 1197, photo 1198). These are
original drawings in that glossy style, no reference pixels used:

  alarm      a modern round alarm clock: a slim glossy red case, a thin
             steel bezel, a white face, two low steel bell caps, slim
             feet, dark hands at 10:08 (the 12, 3, 6 and 9 are drawn live
             in the small font, panels.c)
  contacts   a leather address book, an embossed head and shoulders, a
             column of coloured index tabs
  games      a tabletop: an ivory chess pawn, a red six-sided die and a
             blue four-sided (pyramid) die
  notes      a yellow ruled note pad with a red margin and a bound top
  lock       a closed padlock, steel shackle, brass body, keyhole
  stopwatch  a steel stopwatch: crown and pusher, a white dial with 60
             ticks, the red hand at 12
  cv_chess   (#132) a white king and a black knight on a board in
             perspective under a night sky

One visual scale (owner, second pass): every icon is about 84 px tall
(the drawings span +-40 units at ICON / UNIT px a unit), centred
across the panel and standing on one baseline (BASE), so the reflection
panels.c mirrors under each starts on the same row. The Clocks (96 px
face) and Calendars (80 x 74 page) art are their own and set the scale.

The games' covers (cv_*) are box art in the spirit of late-80s and
early-90s 8-bit cartridge boxes: a chunky pixel scene (2 x 2 px
"pixels"), the title lettered in our own 3 x 5 pixel font with a drop
shadow, a coloured frame, a diagonal-stripe title band and a checker band
at the foot. No console maker's marks, no real game's box layout. The
title is part of the art (the Games list prints the game's name, from its
module, as the row's text beside it).

Format 'pal' (one per picture: its bounding box in an ICON x ICON px
square, the square's centre at the panel's (80, 117), where the Extras
clock sits):
  a     the alpha (coverage), RLE: each byte (run - 1) << 4 | alpha 0..15
  i     for every pixel whose alpha is not 0, in raster order, an index
        into the picture's palette, RLE: each byte (run - 1) << 5 | index
        (runs of 1..8)
  pal   32 RGB565 colours, chosen per picture (k-means over its own
        anti-aliased pixels in a perceptual space, deterministic)
The C side (panels.c cv_pal()) stores a palette colour where the alpha is
full and blends it over the panel where it is not; the reflection under
it walks the alpha stream again (cv_pal_refl()), nothing stored.
"""
import math
import os
import sys

SS = 4                  # samples per px per axis
ICON = 96               # icon box, px
UNIT = 92.0             # the drawings' own box: they scale to ICON
BASE = 90               # the baseline: every picture's bottom row + 1
NCOL = 32               # palette entries (5-bit indices)
PX = 2                  # the covers' chunky pixel, px


# -- shapes (units from the icon's centre, y down) ---------------------------

def disc(cx, cy, r):
    return lambda x, y: (x - cx) ** 2 + (y - cy) ** 2 <= r * r


def rrect(x0, y0, x1, y1, r):
    def f(x, y):
        if x < x0 or x > x1 or y < y0 or y > y1:
            return False
        dx = max(x0 + r - x, 0, x - (x1 - r))
        dy = max(y0 + r - y, 0, y - (y1 - r))
        return dx * dx + dy * dy <= r * r
    return f


def seg(ax, ay, bx, by, r):
    def f(x, y):
        vx, vy = bx - ax, by - ay
        t = ((x - ax) * vx + (y - ay) * vy) / float(vx * vx + vy * vy)
        t = max(0.0, min(1.0, t))
        dx, dy = x - ax - t * vx, y - ay - t * vy
        return dx * dx + dy * dy <= r * r
    return f


def mix(a, b, t):
    t = max(0.0, min(1.0, t))
    return tuple(a[k] + (b[k] - a[k]) * t for k in range(3))


def lit(c, k):
    """c brightened (k > 0, toward white) or darkened (k < 0)"""
    return mix(c, (255, 255, 255), k) if k >= 0 else mix(c, (0, 0, 0), -k)


def sphere(c, x, y, cx, cy, r, spec=.55):
    """c on a ball of radius r at cx, cy, lit from the top left"""
    nx, ny = (x - cx) / r, (y - cy) / r
    nz = math.sqrt(max(0.0, 1 - nx * nx - ny * ny))
    d = (-.45 * nx - .55 * ny + .70 * nz)
    s = max(0.0, d) ** 18 * spec
    return lit(lit(c, -.45 * (1 - max(0.0, d))), s)


def steel(k, base=(200, 204, 212)):
    """brushed steel across a rounded band: k -1 .. 1 (top/left .. bottom/
    right): a light crest just above the middle, darker edges"""
    return lit(base, .50 * (1 - min(1.0, abs(k + .25))) ** 2 - .28)


def tri(x, y, a, b, c):
    """barycentric (u, v, w) of x, y in the triangle a b c, or None"""
    det = (b[1] - c[1]) * (a[0] - c[0]) + (c[0] - b[0]) * (a[1] - c[1])
    u = ((b[1] - c[1]) * (x - c[0]) + (c[0] - b[0]) * (y - c[1])) / det
    v = ((c[1] - a[1]) * (x - c[0]) + (a[0] - c[0]) * (y - c[1])) / det
    w = 1 - u - v
    return (u, v, w) if min(u, v, w) >= 0 else None


# -- the icons: shade(x, y) -> colour, or None outside ------------------------

ALARM_CX, ALARM_CY = 0.0, 3.0           # the face's centre, units
ALARM_HM = (10, 8)                      # the hands' time
ALARM_K = 1.08                          # drawn small, scaled to the set


def alarm(x, y):
    return _alarm(x / ALARM_K, y / ALARM_K)


def _alarm(x, y):
    cx, cy = ALARM_CX, ALARM_CY
    R, B, F = 32.0, 29.0, 27.0           # case, bezel, face radii
    d = math.hypot(x - cx, y - cy)
    if d <= F:
        ha = math.radians(ALARM_HM[0] * 30 + ALARM_HM[1] * .5)
        ma = math.radians(ALARM_HM[1] * 6)
        hand = (34, 36, 42)
        if d <= 1.6:
            return (200, 40, 40)                        # the cap
        if d <= 2.9:
            return hand
        if seg(cx, cy, cx + 12.5 * math.sin(ha), cy - 12.5 * math.cos(ha),
               1.45)(x, y) or \
           seg(cx, cy, cx + 19.0 * math.sin(ma), cy - 19.0 * math.cos(ma),
               1.0)(x, y):
            return hand
        # the alarm-set hand: thin red, at 7 o'clock
        aa = math.radians(212)
        if seg(cx, cy, cx + 17 * math.sin(aa), cy - 17 * math.cos(aa),
               .55)(x, y):
            return (206, 44, 40)
        # the face: white, a soft fall to pale grey, an inner shadow under
        # the bezel at the top
        c = mix((252, 252, 253), (214, 216, 222), (y - cy + F) / (2 * F))
        if d > F - 4 and y < cy + 6:
            c = lit(c, -.22 * (d - (F - 4)) / 4 * min(1.0, (cy + 6 - y) / F))
        # the minute ticks between the numerals (the hours but 12, 3, 6, 9)
        a = (math.degrees(math.atan2(x - cx, cy - y)) + 360) % 360
        k = round(a / 30.0)
        if k % 3 and abs(a - k * 30) * math.pi / 180 * d < .8 and \
                F - 5 < d < F - 2:
            return (110, 114, 122)
        return c
    if d <= B:
        # the bezel: a thin steel ring, rounded across its width
        return steel(((d - F) / (B - F)) * 2 - 1, (214, 218, 226))
    if d <= R:
        # the case: glossy red, darker toward the bottom, a soft gloss
        # crescent over the top left, a crisp dark rim
        red = (196, 36, 40)
        c = mix(lit(red, .10), lit(red, -.40), (y - cy + R) / (2 * R))
        t = (d - B) / (R - B)
        nx, ny = (x - cx) / d, (y - cy) / d
        g = max(0.0, -.62 * nx - .78 * ny)
        c = lit(c, .55 * g ** 6 * (1 - abs(t - .45) * 1.6))
        if t > .82:
            c = lit(c, -.35)
        return c
    # the bells: low steel caps over the case's shoulders
    for s in (-1, 1):
        ux, uy = s * math.sin(math.radians(40)), -math.cos(math.radians(40))
        bx, by = cx + ux * 29.5, cy + uy * 29.5
        along = (x - cx) * ux + (y - cy) * uy
        if (x - bx) ** 2 + (y - by) ** 2 <= 11.5 ** 2 and along >= R + .8:
            return sphere((210, 214, 222), x, y, bx, by, 11.5, .8)
    # the handle stub between them
    if rrect(-2.2, cy - R - 4.5, 2.2, cy - R + 1, 1.2)(x, y):
        return steel(x / 2.2, (190, 194, 202))
    # the feet: slim, splayed
    for s in (-1, 1):
        if seg(s * 18, cy + 25, s * 24, cy + 37, 2.0)(x, y):
            return mix((92, 94, 102), (52, 54, 60), (y - cy - 25) / 12) \
                if s < 0 else mix((74, 76, 84), (40, 42, 48),
                                  (y - cy - 25) / 12)
    return None


def contacts(x, y):
    X0, X1, Y0, Y1 = -31.0, 27.0, -39.0, 39.0
    tabs = ((236, 92, 76), (244, 170, 64), (240, 214, 82), (120, 190, 98),
            (86, 150, 226))
    # the index tabs, on the cover's right edge
    for i, col in enumerate(tabs):
        ty = -31 + i * 13
        if rrect(X1 - 4, ty, X1 + 6, ty + 10, 2.5)(x, y):
            return lit(col, .25 - (y - ty) * .05)
    if not rrect(X0, Y0, X1, Y1, 5)(x, y):
        return None
    leather = (150, 90, 52)
    c = mix(lit(leather, .12), lit(leather, -.30), (y - Y0) / (Y1 - Y0))
    # the spine, a darker band with a light seam
    if x < X0 + 9:
        c = lit(c, -.30)
    elif x < X0 + 10.5:
        c = lit(c, .18)
    # edge bevel: lit top-left, shaded bottom-right
    e = min(x - X0, X1 - x, y - Y0, Y1 - y)
    if e < 2:
        c = lit(c, .25) if (x - X0 < 2 or y - Y0 < 2) else lit(c, -.30)
    # the embossed person: head and shoulders in pale tan, an inner shade
    px, py = 4.0, -1.0
    tan = (226, 190, 146)
    if (x - px) ** 2 + (y - (py - 11)) ** 2 <= 10.5 ** 2:
        return mix(lit(tan, .2), lit(tan, -.18), (y - (py - 21)) / 21)
    sx, sy = (x - px) / 21.0, (y - (py + 22)) / 15.0
    if sx * sx + sy * sy <= 1 and y <= py + 20:
        return mix(lit(tan, .15), lit(tan, -.22), (y - (py + 7)) / 13)
    return c


# the pawn: (y, radius) along its lathe profile, top to bottom, in the
# pawn's own units (PAWN_K units each)
PAWN = ((-21.0, 0.0), (-20.0, 4.5), (-19.0, 6.0),
        (-14.0, 6.5), (-13.0, 9.0), (-11.0, 9.0), (-10.0, 5.0),
        (-8.0, 5.0), (10.0, 9.5), (14.0, 12.5), (15.0, 15.0),
        (19.0, 16.0), (21.0, 16.0), (22.0, 15.0))
PAWN_X, PAWN_DY, PAWN_K, PAWN_HEAD = -16.0, 11.4, 1.3, 9.5


def _pawn(x, y):
    """an ivory pawn: a ball head on a lathe-turned body, its base on the
    baseline; lit from the left like a turned, polished piece"""
    ivory = (238, 230, 212)
    u, v = (x - PAWN_X) / PAWN_K, (y - PAWN_DY) / PAWN_K
    hy = -30.0
    if u * u + (v - hy) ** 2 <= PAWN_HEAD ** 2:
        return sphere(ivory, u, v, 0, hy, PAWN_HEAD, .75)
    for (y0, r0), (y1, r1) in zip(PAWN, PAWN[1:]):
        if y0 <= v <= y1:
            t = (v - y0) / (y1 - y0) if y1 > y0 else 0
            r = r0 + (r1 - r0) * t
            if r <= 0 or abs(u) > r:
                return None
            n = u / r                       # -1 .. 1 across the turning
            c = lit(ivory, -.48 * max(0.0, n + .35) ** 1.3)
            c = lit(c, .55 * max(0.0, 1 - abs(n + .45) * 3.2) ** 2)
            if y1 - y0 <= 2 and r1 > r0:    # the lip of a ring: in shade
                c = lit(c, -.18)
            if v >= 21.0:
                c = lit(c, -.25)
            return c
    return None


def _die(x, y, ox, oy, a, pips, body, ink):
    """an upright cube, 3 faces (top, left, right) of edge 'a' units, its
    nearest top corner at ox, oy; pips: counts on (top, left, right)"""
    c30, s30 = math.cos(math.radians(30)), math.sin(math.radians(30))
    R = (c30 * a, s30 * a)          # +u: to the right and down
    L = (-c30 * a, s30 * a)         # +v: to the left and down
    D = (0.0, a)                    # down
    top0 = (ox, oy - a * s30 * 2)   # the top face's far corner
    faces = (
        (top0, L, R, .18, pips[0]),
        ((ox - R[0], oy - R[1]), R, D, -.05, pips[1]),
        ((ox, oy), (R[0], -R[1]), D, -.38, pips[2]),
    )
    for (px, py), A, B, sh, n in faces:
        det = A[0] * B[1] - A[1] * B[0]
        dx, dy = x - px, y - py
        u = (dx * B[1] - dy * B[0]) / det
        v = (A[0] * dy - A[1] * dx) / det
        if not (0 <= u <= 1 and 0 <= v <= 1):
            continue
        # rounded edges and corners
        ru, rv = min(u, 1 - u), min(v, 1 - v)
        if ru < .12 and rv < .12 and (.12 - ru) ** 2 + (.12 - rv) ** 2 > .0144:
            continue
        c = lit(body, sh)
        # a gloss across each face's upper part, a darker rounded edge
        c = lit(c, .14 * max(0.0, 1 - v * 1.8))
        edge = min(ru, rv)
        if edge < .07:
            c = lit(c, .30 * (1 - edge / .07)) if sh > 0 else \
                lit(c, -.20 * (1 - edge / .07))
        spots = {1: ((.5, .5),), 2: ((.27, .27), (.73, .73)),
                 3: ((.25, .25), (.5, .5), (.75, .75)),
                 4: ((.27, .27), (.73, .27), (.27, .73), (.73, .73)),
                 5: ((.25, .25), (.75, .25), (.5, .5), (.25, .75),
                     (.75, .75)),
                 6: ((.27, .22), (.27, .5), (.27, .78), (.73, .22),
                     (.73, .5), (.73, .78))}[n]
        for su, sv in spots:
            pr2 = .022 if n == 1 else .0125
            if (u - su) ** 2 + (v - sv) ** 2 <= pr2:
                return lit(ink, sh * .5)
        return c
    return None


def _d4(x, y):
    """a four-sided die standing on its base: two faces seen, a pip at
    the foot of each (the up number of a pyramid die reads at its base)"""
    apex, lft, frt, rgt = (-5.0, 11.0), (-24.0, 37.0), (-7.0, 41.0), \
        (10.0, 35.5)
    blue = (54, 104, 196)
    for a, b, c, sh in ((apex, lft, frt, .10), (apex, frt, rgt, -.32)):
        w = tri(x, y, a, b, c)
        if w is None:
            continue
        col = lit(blue, sh)
        col = lit(col, .20 * max(0.0, w[0] - .45) / .55)    # toward the apex
        # the edges: the front ridge catches the light
        if min(w) < .045:
            col = lit(col, .35) if (w[1] if b == frt else w[2]) < .045 and \
                sh < 0 else lit(col, -.15)
        # a white pip low on each face
        mx = (b[0] + c[0]) / 2 * .78 + a[0] * .22
        my = (b[1] + c[1]) / 2 * .78 + a[1] * .22
        if (x - mx) ** 2 + (y - my) ** 2 <= 2.3 ** 2:
            return lit((246, 246, 250), sh * .6)
        return col
    return None


def games(x, y):
    # front to back: the pyramid die, the cube, the pawn
    c = _d4(x, y)
    if c is not None:
        return c
    c = _die(x, y, 14.0, 17.0, 23.0, (5, 3, 2), (204, 38, 42),
             (250, 248, 246))
    if c is not None:
        return c
    return _pawn(x, y)


def notes(x, y):
    X0, X1, Y0, Y1 = -30.0, 30.0, -38.0, 40.0
    if not rrect(X0, Y0, X1, Y1, 3)(x, y):
        return None
    # the bound top: a dark red band with a soft highlight
    if y < Y0 + 10:
        band = (150, 40, 34)
        c = mix(lit(band, .25), lit(band, -.30), (y - Y0) / 10)
        if y > Y0 + 8.6:
            c = lit(c, -.45)
        return c
    paper = (252, 236, 136)
    c = mix(lit(paper, .10), lit(paper, -.12), (y - Y0) / (Y1 - Y0))
    if x > X1 - 1.5 or y > Y1 - 1.5:
        c = lit(c, -.25)
    if -20.2 <= x <= -18.8:
        return (214, 70, 60)                # the margin
    ry = (y - (Y0 + 21)) % 8
    if y > Y0 + 16 and ry < 1.0:
        return mix(c, (110, 150, 214), .75)  # the rules
    return c


def lock(x, y):
    # the brass body
    X0, X1, Y0, Y1 = -27.0, 27.0, -4.0, 40.0
    if rrect(X0, Y0, X1, Y1, 7)(x, y):
        brass = (206, 150, 64)
        t = (y - Y0) / (Y1 - Y0)
        c = mix(lit(brass, .35), lit(brass, -.38), t)
        if 4 < y - Y0 < 9:
            c = lit(c, .22)                 # a sheen band under the top
        e = min(x - X0, X1 - x, y - Y0, Y1 - y)
        if e < 1.8:
            c = lit(c, .22) if y - Y0 < 1.8 else lit(c, -.30)
        # the keyhole
        if disc(0, 13, 5)(x, y) or (13 <= y <= 27
                                    and abs(x) <= 1.6 + (y - 13) * .12):
            return mix((64, 40, 16), (24, 14, 6), (y - 8) / 19)
        return c
    # the steel shackle: a thick U into the body
    cx, cy, r, w = 0.0, -18.0, 18.0, 4.0
    d = math.hypot(x - cx, y - cy)
    inside = (y <= cy and abs(d - r) <= w) or \
             (cy < y <= Y0 + 2 and abs(abs(x) - r) <= w)
    if not inside:
        return None
    k = ((d - r) if y <= cy else (abs(x) - r)) / w     # -1 inner .. 1 outer
    c = steel(k, (196, 200, 210))
    return lit(c, -.30 * max(0.0, min(1.0, (x + 4) / (2 * r))))


def stopwatch(x, y):
    cx, cy = 0.0, 5.0
    R, B, F = 35.0, 32.5, 30.5          # case, bezel, dial radii
    d = math.hypot(x - cx, y - cy)
    if d <= F:
        red = (210, 34, 34)
        # the hand at 12, its counterweight tail, the hub
        if d <= 1.4:
            return (240, 240, 244)
        if d <= 3.2:
            return red
        if abs(x - cx) <= .75 and cy - 26 <= y <= cy + 7:
            return red
        # the dial: white, a soft fall to pale grey, an inner shadow at the
        # top under the bezel
        c = mix((252, 252, 253), (216, 218, 224), (y - cy + F) / (2 * F))
        if d > F - 4 and y < cy + 6:
            c = lit(c, -.24 * (d - (F - 4)) / 4 * min(1.0, (cy + 6 - y) / F))
        # 60 ticks: every 5th long and dark
        a = (math.degrees(math.atan2(x - cx, cy - y)) + 360) % 360
        k = round(a / 6.0)
        off = abs(a - k * 6) * math.pi / 180 * d
        if k % 5 == 0:
            if off < .85 and F - 7.5 < d < F - 1.5:
                return (36, 38, 44)
        elif off < .45 and F - 4.5 < d < F - 1.5:
            return (124, 128, 136)
        return c
    if d <= B:
        return steel(((d - F) / (B - F)) * 2 - 1, (216, 220, 228))
    if d <= R:
        # the case: polished steel, light at the top left
        nx, ny = (x - cx) / d, (y - cy) / d
        g = -.62 * nx - .78 * ny            # -1 .. 1 toward the light
        c = lit((176, 180, 190), .35 * g)
        t = (d - B) / (R - B)
        if t > .8:
            c = lit(c, -.30)
        return c
    # the crown: a stem and a ribbed button on top
    if rrect(-3.0, cy - R - 5, 3.0, cy - R + 1, 1)(x, y):
        return steel(x / 3.0)
    if rrect(-7.0, cy - R - 10.5, 7.0, cy - R - 4.5, 2.5)(x, y):
        c = steel((y - (cy - R - 7.5)) / 3.0, (206, 210, 218))
        return lit(c, -.12) if int((x + 7) * 1.2) % 2 else c
    # the pusher at 1:30
    ux, uy = math.sin(math.radians(45)), -math.cos(math.radians(45))
    if seg(cx + ux * (R - 1), cy + uy * (R - 1), cx + ux * (R + 5),
           cy + uy * (R + 5), 2.6)(x, y):
        return steel(((x - cx) * uy - (y - cy) * ux) / 2.6, (196, 200, 208))
    return None


# -- the Games list's covers (owner, 2026-10-24, second pass): 8-bit era
# box art, drawn on a grid of PX x PX "pixels" ------------------------------

CV_W, CV_H = 34, 42                     # in PX pixels: 68 x 84 px

# our own 3 x 5 pixel letters (I is one wide), '#' ink
FONT = {
    'A': ('.#.', '#.#', '###', '#.#', '#.#'),
    'B': ('##.', '#.#', '##.', '#.#', '##.'),
    'C': ('.##', '#..', '#..', '#..', '.##'),
    'D': ('##.', '#.#', '#.#', '#.#', '##.'),
    'E': ('###', '#..', '##.', '#..', '###'),
    'H': ('#.#', '#.#', '###', '#.#', '#.#'),
    'I': ('#', '#', '#', '#', '#'),
    'K': ('#.#', '#.#', '##.', '#.#', '#.#'),
    'L': ('#..', '#..', '#..', '#..', '###'),
    'O': ('.#.', '#.#', '#.#', '#.#', '.#.'),
    'Q': ('.#.', '#.#', '#.#', '##.', '.##'),
    'R': ('##.', '#.#', '##.', '#.#', '#.#'),
    'S': ('.##', '#..', '.#.', '..#', '##.'),
    'T': ('###', '.#.', '.#.', '.#.', '.#.'),
    'U': ('#.#', '#.#', '#.#', '#.#', '.##'),
}


def title_mask(word):
    """the word's ink cells, {(col, row)}, and its width"""
    ink, x = set(), 0
    for ch in word:
        g = FONT[ch]
        for r, line in enumerate(g):
            for c, v in enumerate(line):
                if v == '#':
                    ink.add((x + c, r))
        x += len(g[0]) + 1
    return ink, x - 1


def box(word, pal, scene):
    """a cover: frame, striped title band with the word, the scene, a
    checker band; pal: frame, stripe a, stripe b, letter top, letter
    bottom, letter shadow, checker a, checker b"""
    frame, sa, sb, lt, lb, sh, ka, kb = pal
    ink, tw = title_mask(word)
    tx, ty = (CV_W - tw) // 2, 4
    TOP, BOT = 12, 38                   # the scene's rows [TOP, BOT)

    def f(cx, cy):
        if cx in (0, CV_W - 1) or cy in (0, CV_H - 1) or cy in (TOP - 1, BOT):
            return frame
        if cy < 3:                      # the stripe band over the title
            return sa if ((cx + cy) // 2) % 2 else sb
        if cy < TOP - 1:                # the title on the frame's colour
            if (cx - tx, cy - ty) in ink:
                return lt if cy - ty < 2 else lb
            if (cx - tx - 1, cy - ty - 1) in ink or \
                    (cx - tx, cy - ty - 1) in ink:
                return sh
            return frame
        if cy > BOT:
            return ka if (cx // 2 + cy) % 2 else kb
        return scene(cx - 1, cy - TOP)  # 32 x 26, (0, 0) its top left
    return ('px', f)


def dither(x, y, a, b, t):
    """a 2-colour ordered dither: b's share t (0..1) at cell x, y"""
    m = ((0, 8, 2, 10), (12, 4, 14, 6), (3, 11, 1, 9), (15, 7, 13, 5))
    return b if t * 16 > m[y % 4][x % 4] + .5 else a


def bricks_scene(x, y):
    rows = ((236, 60, 52), (248, 152, 32), (252, 220, 56), (72, 196, 80))
    # the wall: 4 rows of 2-high bricks, staggered, two knocked out
    if 2 <= y < 10:
        r = (y - 2) // 2
        k = (x + (2 if r % 2 else 0)) % 5
        gone = (r == 3 and 13 <= x < 21) or (r == 2 and 18 <= x < 23)
        if not gone:
            if k == 4:
                return (24, 16, 48)
            col = rows[r]
            return lit(col, .45) if (y - 2) % 2 == 0 and k == 0 else \
                col if (y - 2) % 2 == 0 else lit(col, -.30)
    # the ball, its trail of sparks up and to the left
    bx, by = 19, 15
    if (x - bx) ** 2 + (y - by) ** 2 <= 2:
        return (255, 255, 255) if (x, y) != (bx + 1, by + 1) else \
            (180, 196, 255)
    for i, (sx, sy) in enumerate(((16, 13), (13, 12), (10, 10), (8, 8))):
        if (x, y) == (sx, sy) or (i < 2 and (x, y) == (sx - 1, sy)):
            return (255, 236, 120) if i % 2 == 0 else (255, 160, 64)
    # flying chips from the hole
    if (x, y) in ((22, 11), (24, 12), (14, 11), (25, 10)):
        return (252, 220, 56)
    # the paddle, bright, a highlight along its top
    if 22 <= y <= 23 and 8 <= x <= 19:
        return (240, 244, 255) if y == 22 else (120, 136, 200)
    if y == 22 and x in (7, 20):
        return (120, 136, 200)
    # the night: deep violet, dithered lighter toward the foot
    return dither(x, y, (36, 20, 84), (72, 36, 132), y / 26.0)


def chute_scene(x, y):
    # the sun
    if (x - 26) ** 2 + (y - 5) ** 2 <= 9:
        return (255, 236, 96) if (x - 26) ** 2 + (y - 5) ** 2 <= 4 else \
            (255, 176, 48)
    # the plane, top left, flying right
    plane = ('..#.......',
             '.###......',
             '##########',
             '.########.',
             '....##....')
    if 1 <= x < 11 and 2 <= y < 7 and plane[y - 2][x - 1] == '#':
        return (250, 250, 250) if y - 2 < 3 else (150, 160, 176)
    # the canopy, red and white panels, and the jumper
    can = ('...######...',
           '.##########.',
           '############',
           '#..#....#..#')
    if 11 <= x < 23 and 8 <= y < 12 and can[y - 8][x - 11] == '#':
        if y - 8 == 3:
            return (40, 40, 56)                 # the lines' ends
        return (232, 40, 40) if ((x - 11) // 3) % 2 == 0 else \
            (252, 252, 252)
    lines = ((12, 12), (13, 13), (21, 12), (20, 13))
    if (x, y) in lines:
        return (40, 40, 56)
    if x in (16, 17) and 13 <= y <= 17:
        if y == 13:
            return (252, 204, 160)              # the head
        return (40, 72, 200) if y < 16 else (40, 40, 56)
    if y == 15 and x in (15, 18):
        return (40, 72, 200)                    # arms
    # the sea, the boat on it
    if y >= 22:
        if y == 22 and 4 <= x <= 11:
            return (152, 88, 40)
        c = dither(x, y, (24, 96, 200), (16, 56, 140), (y - 22) / 5.0)
        if (x + 3 * y) % 7 == 0 and y < 25:
            return (180, 220, 255)
        return c
    if y == 21 and 3 <= x <= 12:
        return (200, 120, 56)
    if x == 8 and 16 <= y <= 20:
        return (120, 72, 32)
    if 9 <= x <= 11 and 17 <= y <= 20 and x - 9 <= y - 17:
        return (252, 252, 252)                  # the sail
    # the sky: light at the horizon, dithered
    return dither(x, y, (64, 140, 236), (148, 204, 252), y / 22.0)


def quadrus_scene(x, y):
    cols = {'I': (56, 212, 236), 'O': (252, 216, 48), 'T': (184, 80, 232),
            'S': (88, 208, 80), 'Z': (240, 64, 64), 'L': (252, 148, 32),
            'J': (64, 112, 240)}
    # cells are 4 x 4 pixels in a 8-wide well (x 0..31), rows from the foot
    grid = {}
    stack = (
        ((0, 0), (1, 0), (2, 0), (3, 0), 'I'),
        ((4, 0), (5, 0), (4, 1), (5, 1), 'O'),
        ((6, 0), (7, 0), (7, 1), (7, 2), 'J'),
        ((0, 1), (1, 1), (1, 2), (2, 1), 'T'),
        ((2, 2), (3, 2), (3, 1), (4, 2), 'S'),
        ((5, 2), (6, 2), (6, 1), (6, 3), 'L'),
    )
    for p in stack:
        for c in p[:-1]:
            grid[c] = p[-1]
    # the falling Z, tumbling in
    for c in ((2, 5), (3, 5), (3, 4), (4, 4)):
        grid[c] = 'Z'
    gx, gy = x // 4, (25 - y) // 4
    if (25 - y) >= 0 and (gx, gy) in grid:
        col = cols[grid[(gx, gy)]]
        i, j = x % 4, 3 - (25 - y) % 4
        if i == 0 or j == 0:
            return lit(col, .45)
        if i == 3 or j == 3:
            return lit(col, -.40)
        return col
    # a starfield
    if (x * 7 + y * 13) % 29 == 0:
        return (220, 220, 255) if (x + y) % 2 else (140, 140, 200)
    return dither(x, y, (24, 12, 56), (48, 24, 96), y / 26.0)


SPADE = ('..#..', '.###.', '#####', '#####', '.#.#.', '..#..')
HEART = ('.#.#.', '#####', '#####', '.###.', '..#..')


def _card(x, y, x0, y0, pip, red, big):
    """a 10 x 14 card at x0, y0 with a corner pip and a big centre pip"""
    if not (x0 <= x < x0 + 10 and y0 <= y < y0 + 14):
        return None
    i, j = x - x0, y - y0
    if (i in (0, 9) and j in (0, 13)):
        return None                             # rounded corners
    if i in (0, 9) or j in (0, 13):
        return (40, 40, 48)
    ink = (220, 32, 40) if red else (24, 24, 32)
    if i == 1 + (j == 3) and 1 <= j <= 3:
        return ink                              # the corner mark
    g = pip if big else None
    if g is not None:
        gx, gy = i - 3 + (5 - len(g[0])) // 2 - 0, j - 4
        if 0 <= gy < len(g) and 0 <= gx < len(g[0]) and g[gy][gx] == '#':
            return ink
    return (252, 252, 248) if j < 9 else (228, 228, 236)


def solitaire_scene(x, y):
    # four cards fanned across the felt, the front one an ace of spades
    for x0, y0, pip, red in ((20, 6, SPADE, False), (15, 4, HEART, True),
                             (10, 3, SPADE, False), (5, 4, HEART, True)):
        c = _card(x, y, x0, y0, pip, red, True)
        if c is not None:
            return c
    # a stack of face-down cards at the foot
    if 21 <= y <= 24 and 3 <= x <= 28:
        k = (x - 3) % 6
        if k == 5:
            return (40, 40, 48)
        return (40, 72, 200) if (x + y) % 2 else (88, 120, 232)
    if y == 20 and 3 <= x <= 28 and (x - 3) % 6 != 5:
        return (40, 40, 48)
    # the felt, lighter at the middle
    t = min(1.0, math.hypot(x - 16, (y - 12) * 1.3) / 20.0)
    return dither(x, y, (32, 148, 72), (16, 96, 48), t)


KING = ('...##...',
        '..####..',
        '...##...',
        '.######.',
        '########',
        '########',
        '.######.',
        '..####..',
        '..####..',
        '..####..',
        '.######.',
        '########',
        '########')
KNIGHT = ('...#.#..',
          '..#####.',
          '.#######',
          '##.#####',
          '########',
          '###.####',
          '.#..####',
          '...#####',
          '..######',
          '..#####.',
          '.######.',
          '########',
          '########')


def _piece(x, y, x0, y0, g, body, shade, edge):
    """an 8 x 13 piece at x0, y0: lit from the left, a dark edge"""
    i, j = x - x0, y - y0
    if not (0 <= i < 8 and 0 <= j < len(g)) or g[j][i] != '#':
        return None
    left = i == 0 or g[j][i - 1] != '#'
    right = i == 7 or g[j][i + 1] != '#'
    top = j == 0 or g[j - 1][i] != '#'
    if right or (j == len(g) - 1):
        return edge
    if left or top:
        return lit(body, .35)
    return body if i < 4 else shade


def chess_scene(x, y):
    # a white king and a black knight face off on a board in perspective
    c = _piece(x, y, 5, 3, KING, (236, 236, 244), (196, 200, 216),
               (96, 100, 128))
    if c is None:
        c = _piece(x, y, 19, 3, KNIGHT, (56, 56, 72), (32, 32, 44),
                   (8, 8, 16))
    if c is not None:
        return c
    if y >= 15:
        # the board: squares narrow toward the horizon
        t = (y - 15) / 10.0
        w = 3.0 + 2.6 * t
        col = int(math.floor((x - 15.5) / w + 8))
        row = int(math.floor((y - 15) / (2.0 + 1.2 * t)))
        light = (col + row) % 2 == 0
        base = (236, 220, 180) if light else (176, 96, 48)
        return lit(base, -.18 * (1 - t)) if y < 17 else base
    # the night sky over it, a few stars
    if (x * 11 + y * 7) % 31 == 0 and y < 12:
        return (232, 236, 255)
    return dither(x, y, (20, 28, 88), (48, 64, 156), y / 15.0)


cover_bricks = box('BRICKS', ((200, 28, 40), (252, 156, 32), (252, 212, 64),
                              (255, 255, 255), (255, 232, 96), (96, 8, 24),
                              (200, 28, 40), (36, 20, 84)), bricks_scene)
cover_chute = box('CHUTE', ((24, 72, 200), (120, 200, 252), (252, 252, 252),
                            (255, 236, 64), (255, 160, 32), (16, 32, 112),
                            (24, 72, 200), (252, 252, 252)), chute_scene)
cover_quadrus = box('QUADRUS', ((128, 32, 176), (232, 72, 200),
                                (252, 140, 224), (180, 252, 255),
                                (56, 212, 236), (48, 8, 80),
                                (128, 32, 176), (252, 216, 48)),
                    quadrus_scene)
cover_solitaire = box('SOLITAIRE', ((20, 112, 56), (232, 188, 48),
                                    (252, 232, 120), (255, 255, 255),
                                    (252, 216, 80), (8, 56, 24),
                                    (20, 112, 56), (232, 188, 48)),
                      solitaire_scene)
cover_chess = box('CHESS', ((32, 48, 136), (232, 232, 240), (120, 136, 200),
                            (255, 255, 255), (255, 216, 96), (8, 12, 48),
                            (32, 48, 136), (232, 232, 240)), chess_scene)

ICONS = (('alarm', alarm), ('contacts', contacts), ('games', games),
         ('notes', notes), ('lock', lock), ('stopwatch', stopwatch),
         ('cv_chute', cover_chute), ('cv_bricks', cover_bricks),
         ('cv_quadrus', cover_quadrus), ('cv_solitaire', cover_solitaire),
         ('cv_chess', cover_chess))


# -- rendering and quantising -------------------------------------------------

def render_px(f):
    """a cover: (alpha rows, colours), PX x PX per cell, the 4 corner px
    half covered"""
    w, h = CV_W * PX, CV_H * PX
    alpha, cols = [], []
    for py in range(h):
        row = []
        for qx in range(w):
            a = 7 if (qx in (0, w - 1) and py in (0, h - 1)) else 15
            row.append(a)
            cols.append(f(qx // PX, py // PX))
        alpha.append(row)
    return alpha, cols


def render(shade):
    """(alpha rows 0..15, colours of the pixels with alpha, raster order)"""
    if isinstance(shade, tuple):
        return render_px(shade[1])
    alpha, cols = [], []
    offs = [(i + .5) / SS for i in range(SS)]
    sc = UNIT / ICON
    for py in range(ICON):
        row = []
        for qx in range(ICON):
            acc, n = [0.0, 0.0, 0.0], 0
            for oy in offs:
                for ox in offs:
                    c = shade((qx + ox - ICON / 2.0) * sc,
                              (py + oy - ICON / 2.0) * sc)
                    if c is not None:
                        n += 1
                        for k in range(3):
                            acc[k] += c[k]
            a = (n * 15 + SS * SS // 2) // (SS * SS)
            row.append(a)
            if a:
                cols.append(tuple(v / n for v in acc))
        alpha.append(row)
    return alpha, cols


def kmeans(cols, k=NCOL, rounds=24):
    """k colours for cols: seeded by a deterministic farthest-point pick,
    then Lloyd rounds weighted by count"""
    hist = {}
    for c in cols:
        q = tuple(int(v) >> 2 for v in c)
        e = hist.setdefault(q, [0, 0.0, 0.0, 0.0])
        e[0] += 1
        for j in range(3):
            e[1 + j] += c[j]
    pts = sorted((e[0], (e[1] / e[0], e[2] / e[0], e[3] / e[0]))
                 for e in hist.values())
    pts.reverse()

    def g(v):
        # a perceptual lift: steps in the darks count as much as the eye
        # sees them (the case's shadows, the face)
        return 255 * (max(v, 0.0) / 255) ** .6

    def d2(a, b):
        # luminance-weighted, as the eye sees the steps
        return (2 * (g(a[0]) - g(b[0]))) ** 2 + \
               (3 * (g(a[1]) - g(b[1]))) ** 2 + (g(a[2]) - g(b[2])) ** 2
    cent = [pts[0][1]]
    while len(cent) < min(k, len(pts)):
        best = max(pts, key=lambda p: p[0] ** .5 *
                   min(d2(p[1], c) for c in cent))
        cent.append(best[1])
    for _ in range(rounds):
        sums = [[0.0, 0.0, 0.0, 0.0] for _ in cent]
        for w, p in pts:
            j = min(range(len(cent)), key=lambda i: d2(p, cent[i]))
            s = sums[j]
            s[0] += w
            for t in range(3):
                s[1 + t] += p[t] * w
        cent = [(s[1] / s[0], s[2] / s[0], s[3] / s[0]) if s[0] else c
                for s, c in zip(sums, cent)]
    while len(cent) < k:
        cent.append((0.0, 0.0, 0.0))
    return cent, d2


def to565(c):
    r = min(31, int(c[0] * 31 / 255.0 + .5))
    g = min(63, int(c[1] * 63 / 255.0 + .5))
    b = min(31, int(c[2] * 31 / 255.0 + .5))
    return (r << 11) | (g << 5) | b


def rle(flat, bits=4):
    out, i, mx = [], 0, 1 << (8 - bits)
    while i < len(flat):
        v, n = flat[i], 1
        while i + n < len(flat) and flat[i + n] == v and n < mx:
            n += 1
        out.append((n - 1) << bits | v)
        i += n
    return out


def build(shade):
    """(box, alpha RLE, index RLE, palette, dy); box = x, y, w, h of the
    art in the ICON square, centred across it and standing on BASE (only
    the box is coded: a transparent run costs a byte per 16 px); dy: how
    far the drawing moved down to stand there, px"""
    alpha, cols = render(shade)
    pal, d2 = kmeans(cols)
    idx = [min(range(NCOL), key=lambda i: d2(c, pal[i])) for c in cols]
    ys = [y for y, r in enumerate(alpha) if any(r)]
    xs = [x for x in range(len(alpha[0])) if any(r[x] for r in alpha)]
    x0, x1, y0, y1 = xs[0], xs[-1] + 1, ys[0], ys[-1] + 1
    a = [v for r in alpha[y0:y1] for v in r[x0:x1]]
    w, h = x1 - x0, y1 - y0
    return ((ICON - w) // 2, BASE - h, w, h), rle(a), rle(idx, 5), \
        [to565(c) for c in pal], (BASE - h) - y0


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


def main():
    print('/* Generated by apps/ipodui/art/extrasart.py -- do not edit.')
    print(' * Original artwork; format \'pal\' in the generator\'s'
          ' docstring. */')
    print('#ifndef _IPODUI_EXTRASART_H_')
    print('#define _IPODUI_EXTRASART_H_\n')
    print('#define EXTRASART_W %d' % ICON)
    total, boxes, dys = 0, [], {}
    out = []
    for name, fn in ICONS:
        box_, a, i, pal, dy = build(fn)
        boxes.append(box_)
        dys[name] = dy
        out.append((name, a, i, pal))
        total += len(a) + len(i) + 2 * len(pal) + 4
    sc = ICON / UNIT
    print('/* the alarm face\'s centre in the EXTRASART_W square, and its')
    print(' * numerals\' circle (px) */')
    print('#define EXTRASART_ALARM_CX %d' % round(ICON / 2 + ALARM_CX * ALARM_K * sc))
    print('#define EXTRASART_ALARM_CY %d'
          % round(ICON / 2 + ALARM_CY * ALARM_K * sc + dys['alarm']))
    print('#define EXTRASART_ALARM_NUM_R %d\n' % round(19.5 * ALARM_K * sc))
    for name, a, i, pal in out:
        emit_bytes('extrasart_%s_a' % name, a)
        emit_bytes('extrasart_%s_i' % name, i)
        emit_shorts('extrasart_%s_pal' % name, pal)
        print()
    print('enum extrasart_icon {')
    for name, _ in ICONS:
        print('    EXTRASART_%s,' % name.upper())
    print('    EXTRASART__N\n};')
    print('/* x, y: the art\'s box in the EXTRASART_W square (centred, its'
          ' foot on')
    print(' * one baseline); w x h: its size */')
    print('static const struct {')
    print('    const unsigned char *a, *i;')
    print('    const unsigned short *pal;')
    print('    unsigned char x, y, w, h;')
    print('} extrasart[EXTRASART__N] = {')
    for (name, _), (x, y, w, h) in zip(ICONS, boxes):
        print('    { extrasart_%s_a, extrasart_%s_i, extrasart_%s_pal,'
              % (name, name, name))
        print('      %d, %d, %d, %d },' % (x, y, w, h))
    print('};')
    print('\n/* %d bytes of art */' % total)
    print('#endif /* _IPODUI_EXTRASART_H_ */')


# the ProkBox banner for the generated file (scripts/prokbox_header.py)
sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)),
                                '..', '..', '..', 'scripts'))
import prokbox_header  # noqa: E402


if __name__ == '__main__':
    prokbox_header.run_stdout(main)
