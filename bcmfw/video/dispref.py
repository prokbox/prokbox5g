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
"""The display path's exact reference: YUV 4:2:0 -> RGB565 and the 2:1 box.

This module DEFINES what bcmfw's video display path must produce, bit for
bit (docs/research/bcm2722/pipeline.md, "Colour conversion").  The vector
kernels (bcmfw/video/gen_disp.py) are checked against it in vc2sim, and
the end-to-end bench (bcmfw/tests/video_test.py) converts ffmpeg's
decode through it and compares the panel with the result.

Colour: BT.601, limited range ("TV levels", what H.264 encoders for the
iPod produce), integer coefficients scaled by 4096 = 16 * 256, all of
them even so the -128 chroma offsets fold into constants exactly:

    1.164383 * 4096 = 4769      Y
    1.596027 * 4096 = 6538      V -> R
    0.391762 * 4096 = 1604      U -> G (subtracted)
    0.812968 * 4096 = 3330      V -> G (subtracted)
    2.017232 * 4096 = 8262      U -> B

per pixel (>> is an arithmetic shift, i.e. floor):

    yt = (Y * 4769) >> 8
    rq = yt + ((V * 6538) >> 8)                         - 3567
    gq = yt + ((U * -1604) >> 8) + ((V * -3330) >> 8)   + 2169
    bq = yt + ((U * 8262) >> 8)                         - 4429
    R, G, B = clip((q + 8) >> 4, 0, 255)
    rgb565 = (R >> 3) << 11 | (G >> 2) << 5 | (B >> 3)

(-3567 = -6538/2 - 298, 2169 = 1604/2 + 3330/2 - 298, -4429 = -8262/2 -
298; 298 = (16 * 4769) >> 8.)  Every intermediate fits a signed 16-bit
lane.  Black (16, 128, 128) -> 0x0000, white (235, 128, 128) -> 0xFFFF.
Chroma is upsampled by repetition: pixel (x, y) uses chroma sample
(x >> 1, y >> 1), with x and y counted from the cropped picture's origin
(which is even, so this is the decoder's own siting).

Scaling (2:1 box, for pictures above 320x240): each output sample of
each plane is (a + b + c + d + 2) >> 2 of the 2x2 block it covers, Y,
U and V alike; the result is converted as above.
"""
import struct

KY, KRV, KGU, KGV, KBU = 4769, 6538, 1604, 3330, 8262
CR, CG, CB = -3567, 2169, -4429


def clip8(v):
    return 0 if v < 0 else 255 if v > 255 else v


def pixel(y, u, v):
    """One pixel: 8-bit Y, U, V -> RGB565."""
    yt = (y * KY) >> 8
    r = clip8((yt + ((v * KRV) >> 8) + CR + 8) >> 4)
    g = clip8((yt + ((u * -KGU) >> 8) + ((v * -KGV) >> 8) + CG + 8) >> 4)
    b = clip8((yt + ((u * KBU) >> 8) + CB + 8) >> 4)
    return (r >> 3) << 11 | (g >> 2) << 5 | (b >> 3)


def _tables():
    """Per-sample terms, so a whole frame converts quickly in Python."""
    yt = [(y * KY) >> 8 for y in range(256)]
    rv = [((v * KRV) >> 8) + CR + 8 for v in range(256)]
    gu = [((u * -KGU) >> 8) for u in range(256)]
    gv = [((v * -KGV) >> 8) + CG + 8 for v in range(256)]
    bu = [((u * KBU) >> 8) + CB + 8 for u in range(256)]
    return yt, rv, gu, gv, bu


_T = None


def convert(yp, up, vp, w, h, ys=None, cs=None):
    """A w x h picture (planes as bytes, rows ys / cs apart; default
    tightly packed) -> list of h * w RGB565 values."""
    global _T
    if _T is None:
        _T = _tables()
    yt, rv, gu, gv, bu = _T
    ys = ys or w
    cs = cs or w // 2
    out = []
    for row in range(h):
        yr = yp[row * ys:row * ys + w]
        c0 = (row >> 1) * cs
        ur = up[c0:c0 + (w + 1) // 2]
        vr = vp[c0:c0 + (w + 1) // 2]
        for x in range(w):
            u, v = ur[x >> 1], vr[x >> 1]
            q = yt[yr[x]]
            R = clip8((q + rv[v]) >> 4)
            G = clip8((q + gu[u] + gv[v]) >> 4)
            B = clip8((q + bu[u]) >> 4)
            out.append((R >> 3) << 11 | (G >> 2) << 5 | (B >> 3))
    return out


def box2(plane, w, h, stride=None):
    """2:1 box of one plane (w, h even) -> (bytes, w/2, h/2)."""
    stride = stride or w
    ow, oh = w // 2, h // 2
    out = bytearray(ow * oh)
    for y in range(oh):
        a = plane[(2 * y) * stride:(2 * y) * stride + w]
        b = plane[(2 * y + 1) * stride:(2 * y + 1) * stride + w]
        o = y * ow
        for x in range(ow):
            out[o + x] = (a[2 * x] + a[2 * x + 1] + b[2 * x] + b[2 * x + 1] + 2) >> 2
    return bytes(out), ow, oh


def split_yuv(frame, w, h):
    """A tightly packed yuv420p frame (ffmpeg -f rawvideo) -> (y, u, v)."""
    n = w * h
    c = (w // 2) * (h // 2)
    return frame[:n], frame[n:n + c], frame[n + c:n + 2 * c]


def display(frame, w, h):
    """What the panel shows for one decoded yuv420p frame in FIT mode:
    (pixels, ox, oy, ow, oh), pixels the ow x oh rectangle placed at
    (ox, oy) on the 320x240 panel (bcmfw/video/video.c, fit())."""
    y, u, v = split_yuv(frame, w, h)
    if w > 320 or h > 240:
        y, ow, oh = box2(y, w, h)
        u, _, _ = box2(u, w // 2, h // 2)
        v, _, _ = box2(v, w // 2, h // 2)
        # chroma of an odd w/2 or h/2 loses its last column/row; even
        # picture sizes up to 640x480 keep w/2 and h/2 even only when
        # w and h are multiples of 4 (OPEN refuses the others)
    else:
        ow, oh = w, h
    ox, oy = ((320 - ow) // 2) & ~1, ((240 - oh) // 2)
    return convert(y, u, v, ow, oh), ox, oy, ow, oh


def selftest():
    assert pixel(16, 128, 128) == 0x0000
    assert pixel(235, 128, 128) == 0xFFFF
    # every intermediate fits a signed 16-bit lane
    lo, hi_ = 0, 0
    for s in range(256):
        t = (s * KY) >> 8
        for c in range(256):
            for q in (t + ((c * KRV) >> 8) + CR, t + ((c * KBU) >> 8) + CB):
                lo, hi_ = min(lo, q), max(hi_, q)
    assert -32768 <= lo and hi_ + 8 <= 32767, (lo, hi_)
    # the table path equals the per-pixel definition
    import random
    rng = random.Random(1)
    w, h = 6, 4
    yp = bytes(rng.randrange(256) for _ in range(w * h))
    up = bytes(rng.randrange(256) for _ in range(w * h // 4))
    vp = bytes(rng.randrange(256) for _ in range(w * h // 4))
    got = convert(yp, up, vp, w, h)
    want = [pixel(yp[r * w + x], up[(r >> 1) * 3 + (x >> 1)], vp[(r >> 1) * 3 + (x >> 1)])
            for r in range(h) for x in range(w)]
    assert got == want
    return True


def rgb565_bytes(pixels):
    return struct.pack('<%dH' % len(pixels), *pixels)


if __name__ == '__main__':
    print('dispref selftest: %s' % ('ok' if selftest() else 'FAILED'))
