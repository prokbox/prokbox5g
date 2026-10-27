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

"""Generate Now Playing's glyph maps (stdlib only).

    apps/ipodui/art/npglyphs.py > apps/ipodui/npglyphs.h

Original drawings, rasterised from simple geometry (polygons, circles,
stroked paths) with 8x8 supersampling. No reference pixels are used; the
shapes follow the owner's photos only in kind and size (photo 1203-1210,
1221, 1222): a diamond scrubber marker, five-point rating stars (large
glossy for the rating row, small dark for the info block), rating dots,
two speakers (with and without waves) for the volume bar, the repeat and
shuffle state glyphs under the header, a larger shuffle glyph for the
shelf and the double note of the no-art placeholder (a closed book for an
audiobook, P6 owner).

Every glyph is a 4-bit coverage map, two pixels per byte, high nibble
first, each row padded to a whole byte. Glossy glyphs (diamond, big star)
also get an "inner" map: the coverage of the shape shrunk by RIM px, which
np_draw.c uses to darken the rim towards an edge colour (the same rounded
3D look as the header's play glyph). The note and the book are stored as a 4-bit signed
distance field instead (7.5 + 4 px per level step, negative inside) so it
stays crisp when upscaled 5/2 into the 142 px box. Every table is < 1 KB.
"""
import math
import sys

SS = 8          # supersampling per axis
RIM = 1.0       # rim width of glossy glyphs, px
SS_SPEAKER = 4  # the speakers: 4x supersampled (spec "exact direction")


# ---- geometry ---------------------------------------------------------------

def dist_seg(px, py, a, b):
    ax, ay = a
    bx, by = b
    dx, dy = bx - ax, by - ay
    ll = dx * dx + dy * dy
    t = 0.0 if ll == 0 else max(0.0, min(1.0, ((px - ax) * dx + (py - ay) * dy) / ll))
    return math.hypot(px - ax - t * dx, py - ay - t * dy)


def in_poly(px, py, pts):
    inside = False
    n = len(pts)
    j = n - 1
    for i in range(n):
        xi, yi = pts[i]
        xj, yj = pts[j]
        if (yi > py) != (yj > py):
            x = (xj - xi) * (py - yi) / (yj - yi) + xi
            if px < x:
                inside = not inside
        j = i
    return inside


def poly_dist(px, py, pts):
    """signed distance: negative inside"""
    d = min(dist_seg(px, py, pts[i], pts[(i + 1) % len(pts)])
            for i in range(len(pts)))
    return -d if in_poly(px, py, pts) else d


def star_pts(cx, cy, ro, ri):
    pts = []
    for k in range(10):
        a = -math.pi / 2 + k * math.pi / 5
        r = ro if k % 2 == 0 else ri
        pts.append((cx + r * math.cos(a), cy + r * math.sin(a)))
    return pts


# Each shape is a signed-distance function f(x, y) (< 0 inside).

def shape_poly(pts):
    return lambda x, y: poly_dist(x, y, pts)


def shape_circle(cx, cy, r):
    return lambda x, y: math.hypot(x - cx, y - cy) - r


def shape_ellipse(cx, cy, rx, ry, ang):
    ca, sa = math.cos(ang), math.sin(ang)

    def f(x, y):
        dx, dy = x - cx, y - cy
        u = (dx * ca + dy * sa) / rx
        v = (-dx * sa + dy * ca) / ry
        return (math.hypot(u, v) - 1.0) * min(rx, ry)
    return f


def shape_stroke(path, w):
    def f(x, y):
        return min(dist_seg(x, y, path[i], path[i + 1])
                   for i in range(len(path) - 1)) - w / 2
    return f


def shape_arc(cx, cy, r, w, a0, a1):
    def f(x, y):
        a = math.atan2(y - cy, x - cx)
        if a0 <= a <= a1:
            return abs(math.hypot(x - cx, y - cy) - r) - w / 2
        e0 = (cx + r * math.cos(a0), cy + r * math.sin(a0))
        e1 = (cx + r * math.cos(a1), cy + r * math.sin(a1))
        return min(math.hypot(x - e0[0], y - e0[1]),
                   math.hypot(x - e1[0], y - e1[1])) - w / 2
    return f


def union(*fs):
    return lambda x, y: min(f(x, y) for f in fs)


def minus(f, g):
    return lambda x, y: max(f(x, y), -g(x, y))


def raster(f, w, h, shrink=0.0, ss=SS):
    rows = []
    for y in range(h):
        row = []
        for x in range(w):
            n = 0
            for sy in range(ss):
                for sx in range(ss):
                    if f(x + (sx + .5) / ss, y + (sy + .5) / ss) < -shrink:
                        n += 1
            row.append((n * 15 + ss * ss // 2) // (ss * ss))
        rows.append(row)
    return rows


# ---- the glyphs -------------------------------------------------------------

def diamond():
    c = 7.5
    return shape_poly([(c, 0.6), (14.4, c), (c, 14.4), (0.6, c)]), 15, 15


def star_big():
    """the rating row's star (owner, 2026-10-08, photo 1206): a classic fat
    pentagram star (inner radius 0.382 of the outer), 19 px wide, 18 tall
    (photo 1206: ~19 px, pitch ~21.5), soft antialiased edges,
    no outline, no facets (the gloss is a per-row colour at run time)"""
    return shape_poly(star_pts(10.0, 10.3, 10.0, 0.382 * 10.0)), 20, 19


def star_small():
    return shape_poly(star_pts(5.0, 5.3, 5.0, 2.05)), 10, 10


def dot():
    return shape_circle(2.5, 2.5, 1.55), 5, 5


def repeat(one=False):
    """a rounded rectangular loop, arrows top-right (pointing right) and
    bottom-left (pointing left); 'one' adds a small 1 inside"""
    W, H = 17, 13
    x0, y0, x1, y1, r, sw = 1.6, 2.6, 15.4, 10.4, 2.6, 1.7

    def rrect(x, y):
        qx = abs(x - (x0 + x1) / 2) - ((x1 - x0) / 2 - r)
        qy = abs(y - (y0 + y1) / 2) - ((y1 - y0) / 2 - r)
        out = math.hypot(max(qx, 0), max(qy, 0)) + min(max(qx, qy), 0) - r
        return abs(out) - sw / 2

    def gaps(x, y):
        # the loop is open where the arrowheads sit
        if x > 9.6 and y < 5.0:
            return 1.0
        if x < 7.4 and y > 8.0:
            return 1.0
        return -1.0

    def loop(x, y):
        return 1.0 if gaps(x, y) > 0 else rrect(x, y)

    head_r = shape_poly([(9.4, -0.2), (9.4, 5.4), (13.4, 2.6)])
    head_l = shape_poly([(7.6, 7.6), (7.6, 13.2), (3.6, 10.4)])
    parts = [loop, head_r, head_l]
    if one:
        parts.append(shape_poly([(7.9, 4.6), (9.5, 4.6), (9.5, 8.9),
                                 (8.3, 8.9), (8.3, 6.0), (7.2, 6.2)]))
    return union(*parts), W, H


def shuffle(scale=1.0, bold=1.0):
    """two strands crossing, each ending in an arrowhead at the right.
    Overpass (owner, 2026-10-08): where they cross, the rising strand is
    split on either side of the falling one, so one visibly passes over
    the other. Round stroke ends (capsule strokes)."""
    s = scale
    W, H = int(round(18 * s)), int(round(13 * s))
    sw = 1.7 * s * bold
    gap = 0.85 * s                  # the split on each side of the overpass
    a = [(0.9 * s, 3.0 * s), (4.6 * s, 3.0 * s), (10.4 * s, 10.0 * s),
         (13.4 * s, 10.0 * s)]
    b = [(0.9 * s, 10.0 * s), (4.6 * s, 10.0 * s), (10.4 * s, 3.0 * s),
         (13.4 * s, 3.0 * s)]
    ha = shape_poly([(13.0 * s, 7.0 * s), (13.0 * s, 13.0 * s),
                     (17.4 * s, 10.0 * s)])
    hb = shape_poly([(13.0 * s, 0.0 * s), (13.0 * s, 6.0 * s),
                     (17.4 * s, 3.0 * s)])
    over = shape_stroke(a, sw)
    under = minus(shape_stroke(b, sw), shape_stroke(a, sw + 2 * gap))
    return union(over, under, ha, hb), W, H


def speaker(waves=False):
    """spec now-playing.md "Speaker glyphs: exact direction" (orchestrator,
    16x comparison of the rectified photo 1222, 2026-10-08), at 1x device
    pixels, built from a 4x supersampled vector (SS_SPEAKER): a 3 w x 5 h
    magnet body, vertically centred; a cone flaring with straight edges
    from the body's 5 px to an 11 px mouth over 4 px; the mouth's top and
    bottom 3 rows reach one more column right (the rim peeking round the
    dust cap), with a 1 px background slit (5 h, centred) between them;
    a 2 w x 3 h rounded dust cap right after the slit. ~10 w x 11 h. The
    loud one adds two thin arcs centred on the dust cap, radii 5 and 8,
    +-50 deg, a 1.2 px stroke antialiased (~1.5 px visually; 1.0 read dotted at 8x), a clear 2 px
    gap between them; the outer arc ~13 px tall. Near-black at run time
    (#1c1c1c, C_SPEAKER), placed ~4 px from the bar's ends."""
    H = 13 if waves else 11
    cy = H / 2.0
    body = [
        shape_poly([(0.0, cy - 2.5), (3.0, cy - 2.5), (3.0, cy + 2.5),
                    (0.0, cy + 2.5)]),                          # magnet
        shape_poly([(3.0, cy - 2.5), (7.0, cy - 5.5), (7.0, cy + 5.5),
                    (3.0, cy + 2.5)]),                          # cone
        shape_poly([(7.0, cy - 5.5), (8.0, cy - 5.5), (8.0, cy - 2.5),
                    (7.0, cy - 2.5)]),                          # rim, top
        shape_poly([(7.0, cy + 2.5), (8.0, cy + 2.5), (8.0, cy + 5.5),
                    (7.0, cy + 5.5)]),                          # rim, bottom
        shape_ellipse(9.0, cy, 1.0, 1.5, 0.0),                  # dust cap
    ]
    if not waves:
        return union(*body), 10, H
    a = math.radians(50)
    body.append(shape_arc(9.0, cy, 5.0, 1.2, -a, a))
    body.append(shape_arc(9.0, cy, 8.0, 1.2, -a, a))
    return union(*body), 18, H


def note():
    """the placeholder's double (beamed) note, 36 x 42; upscaled 5/2 into
    the 142 px art box at run time"""
    head_l = shape_ellipse(8.6, 34.4, 6.6, 4.9, -0.38)
    head_r = shape_ellipse(27.4, 30.2, 6.6, 4.9, -0.38)
    stem_l = shape_poly([(12.2, 9.0), (15.0, 8.4), (15.0, 34.0), (12.2, 34.0)])
    stem_r = shape_poly([(31.0, 4.6), (33.8, 4.0), (33.8, 30.0), (31.0, 30.0)])
    beam = shape_poly([(12.2, 8.2), (33.8, 2.2), (33.8, 9.4), (12.2, 15.4)])
    return union(head_l, head_r, stem_l, stem_r, beam), 36, 42


def book():
    """the audiobook placeholder: a closed hardback from the front (spine
    band, a title window, the page block), 36 x 42 like the note; our own
    drawing, the same shapes as the list-row book (rowglyphs.py)"""
    rad = 2.6
    inner = shape_poly([(3.0 + rad, 2.0 + rad), (33.0 - rad, 2.0 + rad),
                        (33.0 - rad, 40.0 - rad), (3.0 + rad, 40.0 - rad)])
    cover = lambda x, y: inner(x, y) - rad
    crease = shape_poly([(9.6, 0.0), (11.0, 0.0), (11.0, 42.0), (9.6, 42.0)])
    pages = shape_poly([(11.0, 33.0), (36.0, 33.0), (36.0, 34.4), (11.0, 34.4)])
    window = shape_poly([(16.5, 9.5), (27.5, 9.5), (27.5, 16.5), (16.5, 16.5)])
    return minus(minus(minus(cover, crease), pages), window), 36, 42


GLYPHS = [
    # name, factory, glossy (inner map)
    ("DIAMOND", diamond, True),
    ("STAR_BIG", star_big, False),
    ("STAR_SMALL", star_small, False),
    ("DOT", dot, False),
    ("REPEAT", lambda: repeat(False), False),
    ("REPEAT_ONE", lambda: repeat(True), False),
    ("SHUFFLE", lambda: shuffle(1.0), False),
    ("SHUFFLE_BIG", lambda: shuffle(1.45, 1.25), False),
    ("SPEAKER", lambda: speaker(False), False),
    ("SPEAKER_WAVES", lambda: speaker(True), False),
    ("NOTE", note, False),
    ("BOOK", book, False),
]



def raster_sdf(f, w, h):
    """4-bit signed distance at pixel centres: 7.5 + 4 * d (d in px,
    negative inside), clamped. Upscaled bilinearly it keeps crisp edges
    (np_draw.c np_placeholder_render())."""
    rows = []
    for y in range(h):
        rows.append([max(0, min(15, int(round(7.5 + 4 * f(x + .5, y + .5)))))
                     for x in range(w)])
    return rows


def pack(rows, w):
    out = []
    for row in rows:
        r = row + [0] * (w % 2)
        for i in range(0, len(r), 2):
            out.append((r[i] << 4) | r[i + 1])
    return out


def emit_table(name, data):
    lines = ["static const unsigned char %s[%d] = {" % (name, len(data))]
    for i in range(0, len(data), 12):
        lines.append("    " + ", ".join("0x%02x" % v for v in data[i:i + 12]) + ",")
    lines.append("};")
    return "\n".join(lines)


def main():
    out = ["/* Generated by apps/ipodui/art/npglyphs.py -- do not edit.",
           " * 4-bit coverage, two pixels per byte, high nibble first, rows",
           " * padded to a byte. Included by np_draw.c only. */",
           "#ifndef _IPODUI_NPGLYPHS_H_",
           "#define _IPODUI_NPGLYPHS_H_", ""]
    entries = []
    for name, fac, glossy in GLYPHS:
        f, w, h = fac()
        if name in ("NOTE", "BOOK"):
            cov = pack(raster_sdf(f, w, h), w)
        elif name.startswith("SPEAKER"):
            cov = pack(raster(f, w, h, ss=SS_SPEAKER), w)
        else:
            cov = pack(raster(f, w, h), w)
        assert len(cov) < 1024, name
        out.append(emit_table("npg_%s_cov" % name.lower(), cov))
        inner = "0"
        if glossy:
            inn = pack(raster(f, w, h, RIM), w)
            out.append(emit_table("npg_%s_inner" % name.lower(), inn))
            inner = "npg_%s_inner" % name.lower()
        entries.append("    [NPG_%s] = { %d, %d, npg_%s_cov, %s }," %
                       (name, w, h, name.lower(), inner))
        out.append("")
    out.append("static const struct np_glyph np_glyphs[NPG_COUNT] = {")
    out.extend(entries)
    out.append("};")
    out.append("")
    out.append("#endif /* _IPODUI_NPGLYPHS_H_ */")
    sys.stdout.write(prokbox_header.convert("\n".join(out) + "\n", ".h")[0])
    # the enum lives in np_draw.h; keep the order in step
    sys.stderr.write("order: " + " ".join(n for n, _, _ in GLYPHS) + "\n")


# the ProkBox banner for the generated file (scripts/prokbox_header.py)
import os as _os  # noqa: E402
import sys as _sys  # noqa: E402
_sys.path.insert(0, _os.path.join(_os.path.dirname(_os.path.abspath(__file__)), '..', '..', '..', 'scripts'))
import prokbox_header  # noqa: E402


if __name__ == "__main__":
    main()
