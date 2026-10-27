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

"""Generate the list-row glyph coverage tables (stdlib only).

    apps/ipodui/art/rowglyphs.py > apps/ipodui/rowglyphs.h

Original drawings in the 7G's style, rasterised with 8x8 supersampling
into 4-bit coverage (0 = empty, 15 = full ink), two pixels per byte (high
nibble first), row-major:

  note     a pair of beamed eighth notes, for "All Songs" and for albums
           whose art is missing or not built yet. The shell colours it
           with a glossy blue gradient on a dark tile (rowart.c).
  podcast  a broadcast mark (a round head with two arcs either side over
           a tapered mast), for shows without art.
  booktile a light grey closed hardback, for audiobooks without art
           (P6 owner: "grey like the 7G"). Our own drawing, shaded:
           alpha plus a grey-level plane (see below). Now Playing keeps
           its own book glyph (npglyphs.py).
  dot      the "unplayed" disc of podcast shows and episodes.
  srch_glyph  (P11) Search's grey type icons (artist bust, disc, note,
           book, broadcast mark, list with a note) and the magnifier of
           its top row, 30 x 34, in Rockbox's alpha format.
  rcornerN (P11) anti-aliased rounded-rect corners, radius 3, 4, 5, for
           the Search letter strip, its text field and the letter box.

Only the proportions come from the owner's photos (artist-albums-
allsongs-b: the note fills ~65% x 80% of its 55 px tile); no reference
pixels are used.
"""
import math

SS = 8


def raster(w, h, inside):
    rows = []
    for py in range(h):
        row = []
        for px in range(w):
            n = 0
            for sy in range(SS):
                for sx in range(SS):
                    if inside(px + (sx + .5) / SS, py + (sy + .5) / SS):
                        n += 1
            row.append(min(15, (n * 15 + SS * SS // 2) // (SS * SS)))
        rows.append(row)
    return rows


def ellipse(x, y, cx, cy, rx, ry, ang):
    c, s = math.cos(ang), math.sin(ang)
    dx, dy = x - cx, y - cy
    u = dx * c + dy * s
    v = -dx * s + dy * c
    return (u / rx) ** 2 + (v / ry) ** 2 <= 1.0


def in_poly(x, y, pts):
    inside = False
    n = len(pts)
    for i in range(n):
        x1, y1 = pts[i]
        x2, y2 = pts[(i + 1) % n]
        if (y1 > y) != (y2 > y):
            xi = x1 + (y - y1) * (x2 - x1) / (y2 - y1)
            if x < xi:
                inside = not inside
    return inside


# -- note: 36 x 44 --------------------------------------------------------
NOTE_W, NOTE_H = 36, 44
HEAD = (7.6, 5.4, math.radians(-24))
HEADS = ((9.0, 36.5), (28.0, 32.0))
STEM_W = 3.2
BEAM = 6.5


def note(x, y):
    rx, ry, a = HEAD
    for cx, cy in HEADS:
        if ellipse(x, y, cx, cy, rx, ry, a):
            return True
    # stems rise from the right side of each head to the beam
    s1 = HEADS[0][0] + rx * 0.86
    s2 = HEADS[1][0] + rx * 0.86
    top1, top2 = 7.5, 3.0
    if s1 - STEM_W <= x <= s1 and top1 <= y <= HEADS[0][1]:
        return True
    if s2 - STEM_W <= x <= s2 and top2 <= y <= HEADS[1][1]:
        return True
    # the beam: a thick bar from stem to stem, rising to the right
    return in_poly(x, y, [(s1 - STEM_W, top1), (s2, top2),
                          (s2, top2 + BEAM), (s1 - STEM_W, top1 + BEAM)])


# -- podcast: 36 x 40 -----------------------------------------------------
POD_W, POD_H = 36, 40
POD_C = (18.0, 15.0)


def podcast(x, y):
    cx, cy = POD_C
    d = math.hypot(x - cx, y - cy)
    if d <= 5.0:
        return True
    ang = math.degrees(math.atan2(y - cy, x - cx))
    side = abs(ang) <= 42 or abs(ang) >= 138
    if side and (8.5 <= d <= 11.5 or 14.5 <= d <= 17.5):
        return True
    # the mast: from under the head, widening to the base
    return in_poly(x, y, [(cx - 2.2, cy + 4.0), (cx + 2.2, cy + 4.0),
                          (cx + 6.0, 39.0), (cx - 6.0, 39.0)])


# -- book tile: 32 x 38, grey, shaded --------------------------------------
# A closed hardback seen a little from above and to the right: the front
# cover (a soft top-to-bottom grey ramp, a hinge groove near the spine),
# the page block showing along the top and the right edge (fine page
# lines), a thin darker outline and a soft drop shadow. Unlike the
# coverage glyphs it carries its own tone: two 4-bit planes, alpha and
# grey level (BOOKTILE_LO .. BOOKTILE_HI), blended onto the list's
# background by rowart.c. Tones follow the 7G Audiobooks list frame
# (owner capture v2-t57.0, photo contrast mapped back so that the
# text's black is 0 and the row's white 255): the cover ~0xbc at the
# top to ~0xa8 at the bottom, the page edges ~0xe4, the tile ~27 x 35 px.
BT_W, BT_H = 32, 38
BT_LO, BT_HI = 0x80, 0xf0
BT_DX, BT_DY = 3.0, 2.5                          # page block offset
BT_COVER = (1.0, 27.0, 3.5, 37.0)                # front cover l r t b
BT_RAD = 1.3
BT_SHADOW = (1.0, 1.5, 0.32)                     # dx, dy, alpha


def rounded(x, y, l, r, t, b, rad):
    if not (l <= x <= r and t <= y <= b):
        return False
    cx = min(max(x, l + rad), r - rad)
    cy = min(max(y, t + rad), b - rad)
    return math.hypot(x - cx, y - cy) <= rad


def bt_hull(x, y, dx=0.0, dy=0.0):
    l, r, t, b = BT_COVER
    return in_poly(x - dx, y - dy, [(l, t), (l + BT_DX, t - BT_DY),
                                    (r + BT_DX, t - BT_DY),
                                    (r + BT_DX, b - BT_DY), (r, b), (l, b)])


def bt_sample(x, y):
    """(alpha, grey 0..255) of one sub-sample, layers composited."""
    l, r, t, b = BT_COVER
    if rounded(x, y, l, r, t, b, BT_RAD):
        if not rounded(x, y, l + .7, r - .7, t + .7, b - .7, BT_RAD - .5):
            return 1.0, 0x8c                     # cover outline
        f = (y - t) / (b - t)
        v = 0xc2 + (0xa6 - 0xc2) * f             # the ramp
        if y < t + 1.8:
            v += 14                              # lit top edge
        if l + 3.6 <= x < l + 4.5:
            v -= 16                              # hinge groove
        elif l + 4.5 <= x < l + 5.4:
            v += 10                              # ... and its lit side
        return 1.0, v
    if bt_hull(x, y):
        if not bt_hull_inner(x, y):
            return 1.0, 0x98                     # page block outline
        if x > r:                                # right edge: page lines
            return 1.0, 0xe8 if int(x) % 2 else 0xd6
        return 1.0, 0xe8 if int(y) % 2 else 0xd6   # top edge
    sx, sy, sa = BT_SHADOW
    if bt_hull(x, y, sx, sy):
        return sa, BT_LO
    return 0.0, 0


def bt_hull_inner(x, y):
    e = .7
    return all(bt_hull(x + ox, y + oy) for ox, oy in
               ((e, 0), (-e, 0), (0, e), (0, -e)))


BAYER4 = ((0, 8, 2, 10), (12, 4, 14, 6), (3, 11, 1, 9), (15, 7, 13, 5))


def raster_tone(w, h, sample):
    alpha, tone = [], []
    for py in range(h):
        ra, rt = [], []
        for px in range(w):
            sa = st = 0.0
            for sy in range(SS):
                for sx in range(SS):
                    a, v = sample(px + (sx + .5) / SS, py + (sy + .5) / SS)
                    sa += a
                    st += a * v
            a = sa / (SS * SS)
            v = st / sa if sa else BT_LO
            qa = min(15, int(a * 15 + .5))
            d = (BAYER4[py % 4][px % 4] + .5) / 16      # ordered dither
            qt = min(15, max(0, int((v - BT_LO) * 15 / (BT_HI - BT_LO) + d)))
            ra.append(qa)
            rt.append(qt if qa else 0)
        alpha.append(ra)
        tone.append(rt)
    return alpha, tone


# -- dot: 10 x 10 ---------------------------------------------------------
DOT_D = 10


def dot(x, y):
    return math.hypot(x - DOT_D / 2, y - DOT_D / 2) <= DOT_D / 2 - 0.6


# -- Phase 14a: the video marks --------------------------------------------
# vscreen 16 x 13: a small screen (outline, rounded corners, a foot), the
# mark of a video episode among the podcasts (brief 3.6, T4). film 36 x 36:
# a film frame (sprocket holes both sides, a play wedge between), the
# stand-in thumbnail of a movie or show without a poster.
VS_W, VS_H = 16, 13


def vscreen(x, y):
    if y <= 10.5:
        outer = rounded(x, y, 0.2, 15.8, 0.2, 10.8, 2.2)
        inner = rounded(x, y, 1.9, 14.1, 1.9, 9.1, 0.9)
        return outer and not inner
    return 5.0 <= x <= 11.0 and 11.4 <= y <= 12.8


FILM_W, FILM_H = 36, 36


def film(x, y):
    if not (2.0 <= x <= 34.0 and 4.0 <= y <= 32.0):
        return False
    for hx in (3.4, 29.4):
        for k in range(5):
            hy = 6.0 + k * 5.2
            if hx <= x <= hx + 3.2 and hy <= y <= hy + 3.0:
                return False
    if 9.5 <= x <= 26.5 and 8.0 <= y <= 28.0:
        # the window: a play wedge inside
        return in_poly(x, y, [(14.5, 12.0), (24.0, 18.0), (14.5, 24.0)])
    return True


# -- Search (P11): the type icons and the magnifier, 30 x 34 --------------
# Flat grey glyphs on the search rows' light tile (owner photos
# search-empty-b, search-results-b: a grey magnifier ~31 px, a grey note
# ~28 x 36 px, centred in the 55 px art box). Our own drawings; one per
# result kind (enum srch_kind order, search.h), then the magnifier. They
# are drawn with lcd_alpha_bitmap_part() in the list's grey, so they ship
# in Rockbox's alpha format (emit_alpha()).
SG_W, SG_H = 30, 34


def capsule(x, y, x1, y1, x2, y2, r):
    dx, dy = x2 - x1, y2 - y1
    t = ((x - x1) * dx + (y - y1) * dy) / (dx * dx + dy * dy)
    t = min(1.0, max(0.0, t))
    return math.hypot(x - (x1 + t * dx), y - (y1 + t * dy)) <= r


def sg_magnifier(x, y):
    # rectified search-empty-b: the ring ~22 px across, centred ~(25, 26)
    # in the row, the handle reaching ~(45, 47): low and right in the box
    # #52 re-measure: the ring larger (~26 px) and thinner, the handle a
    # slim rounded rod from the rim
    d = math.hypot(x - 13.6, y - 14.6)
    if 10.6 <= d <= 13.2:
        return True
    return capsule(x, y, 22.6, 23.6, 28.3, 30.6, 1.7)


def sg_song(x, y):
    s = 36.0 / 27.0                     # the row note, 3/4 size
    return note((x - 1.5) * s, (y - 0.5) * s)


def sg_episode(x, y):
    s = 36.0 / 29.0                     # the broadcast mark, 0.8 size
    return podcast((x - 0.5) * s, (y - 1.5) * s)


def sg_artist(x, y):
    # a bust: a round head over rounded shoulders, a gap between them
    if math.hypot(x - 15, y - 10.5) <= 6.6:
        return True
    if math.hypot(x - 15, y - 10.5) <= 8.4:
        return False
    return y <= 31.5 and ((x - 15) / 13.0) ** 2 + ((y - 33.5) / 14.5) ** 2 <= 1


def sg_album(x, y):
    # a disc: a solid face with a groove ring and a spindle hole
    d = math.hypot(x - 15, y - 17)
    if d > 13.6 or d < 2.6:
        return False
    return not (5.2 <= d <= 6.4)


def sg_book(x, y):
    # a closed hardback face-on: the cover, a hinge groove near the spine,
    # and the page block showing as a band at the bottom
    if not rounded(x, y, 4.5, 26.0, 2.0, 32.0, 1.6):
        return False
    if 8.6 <= x <= 9.8 and y <= 28.6:
        return False                    # hinge groove
    return not (27.6 <= y <= 28.8 and x >= 8.6)   # page block line


def sg_playlist(x, y):
    # list lines at the left, a single note at the right
    for ly in (7.0, 13.0, 19.0, 25.0):
        if 1.5 <= x <= (15.0 if ly > 18 else 19.0) and ly <= y <= ly + 2.8:
            return True
    if ellipse(x, y, 21.5, 27.0, 4.4, 3.2, math.radians(-24)):
        return True
    if 24.2 <= x <= 26.0 and 9.0 <= y <= 27.0:
        return True
    # the flag
    return in_poly(x, y, [(24.2, 9.0), (26.0, 9.0), (29.5, 14.5),
                          (29.0, 17.5), (26.0, 13.5)])


SG_ALL = (sg_artist, sg_album, sg_song, sg_book, sg_episode, sg_playlist,
          sg_magnifier)


# -- rounded corners (P11 Search strip, field and letter box) -------------
def corner(r, fx, fy):
    """Coverage of an r x r corner of a rounded rect (radius r); fx/fy
    mirror it into the right / bottom corner."""
    rows = raster(r, r, lambda x, y: math.hypot(x - r, y - r) <= r)
    if fx:
        rows = [row[::-1] for row in rows]
    if fy:
        rows = rows[::-1]
    return rows


def emit_alpha(name, w, h, rows_list):
    """Rockbox's alpha format (lcd_alpha_bitmap_part()): 4 bits per pixel,
    0 = opaque .. 15 = transparent, the first pixel in the low nibble, one
    continuous nibble stream (rows share a byte when w is odd)."""
    data = []
    for rows in rows_list:
        flat = [15 - v for r in rows for v in r]
        if len(flat) % 2:
            flat.append(15)
        data.append([flat[i] | (flat[i + 1] << 4)
                     for i in range(0, len(flat), 2)])
    n = len(data[0])
    print('#define %s_W %d' % (name.upper(), w))
    print('#define %s_H %d' % (name.upper(), h))
    print('static const unsigned char %s[%d][%d] = {' % (name, len(data), n))
    for d in data:
        print('    {')
        for i in range(0, n, 12):
            print('        ' + ' '.join('0x%02x,' % b for b in d[i:i + 12]))
        print('    },')
    print('};')
    print()


def emit(name, w, h, rows):
    flat = [v for r in rows for v in r]
    if len(flat) % 2:
        flat.append(0)
    packed = [(flat[i] << 4) | flat[i + 1] for i in range(0, len(flat), 2)]
    print('#define %s_W %d' % (name.upper(), w))
    print('#define %s_H %d' % (name.upper(), h))
    print('static const unsigned char %s_cov[%d] = {' % (name, len(packed)))
    for i in range(0, len(packed), 12):
        print('    ' + ' '.join('0x%02x,' % b for b in packed[i:i + 12]))
    print('};')
    print()


def main():
    print('/* Generated by apps/ipodui/art/rowglyphs.py -- do not edit.')
    print(' * 4-bit coverage, two pixels per byte, high nibble first. */')
    print('#ifndef _IPODUI_ROWGLYPHS_H_')
    print('#define _IPODUI_ROWGLYPHS_H_')
    print()
    emit('note', NOTE_W, NOTE_H, raster(NOTE_W, NOTE_H, note))
    emit('podcast', POD_W, POD_H, raster(POD_W, POD_H, podcast))
    alpha, tone = raster_tone(BT_W, BT_H, bt_sample)
    print('#define BOOKTILE_LO 0x%02x' % BT_LO)
    print('#define BOOKTILE_HI 0x%02x' % BT_HI)
    emit('booktile', BT_W, BT_H, alpha)
    emit('booktile_tone', BT_W, BT_H, tone)
    emit('dot', DOT_D, DOT_D, raster(DOT_D, DOT_D, dot))
    emit('vscreen', VS_W, VS_H, raster(VS_W, VS_H, vscreen))
    emit('film', FILM_W, FILM_H, raster(FILM_W, FILM_H, film))
    print('/* P11 Search: enum srch_kind order, then the magnifier;')
    print(' * Rockbox alpha format (lcd_alpha_bitmap_part()). */')
    emit_alpha('srch_glyph', SG_W, SG_H,
               [raster(SG_W, SG_H, f) for f in SG_ALL])
    print('/* rounded corners: top-left, top-right, bottom-left, bottom-right */')
    for r in (3, 4, 5):
        emit_alpha('rcorner%d' % r, r, r,
                   [corner(r, fx, fy) for fy in (0, 1) for fx in (0, 1)])
    print('#endif /* _IPODUI_ROWGLYPHS_H_ */')


# the ProkBox banner for the generated file (scripts/prokbox_header.py)
import os as _os  # noqa: E402
import sys as _sys  # noqa: E402
_sys.path.insert(0, _os.path.join(_os.path.dirname(_os.path.abspath(__file__)), '..', '..', '..', 'scripts'))
import prokbox_header  # noqa: E402


if __name__ == '__main__':
    prokbox_header.run_stdout(main)
