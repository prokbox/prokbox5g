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

# Half-band polyphase IIR designer for lib/rbcodec/dsp/resample_hb.c
# (P19 package 19-13, docs/design/resample-plan.md).
#
#   tests/resample/hb_design.py          print the C coefficient tables
#   tests/resample/hb_design.py --specs  print the designed attenuation
#
# Each half-band is two parallel chains of first-order allpass sections in
# z^-2 (Valenzuela and Constantinides 1983; the elliptic design follows the
# closed form used by Laurent de Soras' HIIR library):
#     H(z) = 0.5 * (A0(z^2) + z^-1 * A1(z^2)),
#     Ai(z) = prod_k (a_k + z^-1) / (1 + a_k z^-1).
# Coefficients alternate between the chains (a0 -> A0, a1 -> A1, ...).
# The tables store them deinterleaved, Q31, chain 0 in c[0..3] and chain 1
# in c[4..7]. Standard library only.
import math
import sys

# name, coefficient count, transition width (fraction of the high rate),
# what it is for
DESIGNS = [
    ("hb_t44", 7, 0.5 - 2 * 20000.0 / 88200,
     "tight, 44.1 kHz family: flat to 0.2268 fs (20 kHz at 88.2 kHz)"),
    ("hb_t48", 6, 0.5 - 2 * 20000.0 / 96000,
     "tight, 48 kHz family: flat to 0.2083 fs (20 kHz at 96 kHz)"),
    ("hb_r", 3, 0.27,
     "relaxed outer stage of a cascade: flat to 0.115 fs"),
]


def _transition(tbw):
    k = math.tan((1 - tbw * 2) * math.pi / 4)
    k *= k
    kk = (1 - k * k) ** 0.25
    e = 0.5 * (1 - kk) / (1 + kk)
    e4 = e ** 4
    q = e * (1 + e4 * (2 + e4 * (15 + 150 * e4)))
    return k, q


def attenuation(n, tbw):
    _, q = _transition(tbw)
    a = 4 * math.exp((2 * n + 1) * 0.5 * math.log(q))
    return -10 * math.log10(a / (1 + a))


def coefs(n, tbw):
    k, q = _transition(tbw)
    order = 2 * n + 1
    out = []
    for c in range(1, n + 1):
        num, i, j = 0.0, 0, 1
        while True:
            t = q ** (i * (i + 1)) * math.sin((i * 2 + 1) * c * math.pi / order) * j
            num += t
            i += 1
            j = -j
            if abs(t) < 1e-100:
                break
        den, i, j = 0.0, 1, -1
        while True:
            t = q ** (i * i) * math.cos(i * 2 * c * math.pi / order) * j
            den += t
            i += 1
            j = -j
            if abs(t) < 1e-100:
                break
        ww = num * q ** 0.25 / (den + 0.5)
        w2 = ww * ww
        x = math.sqrt((1 - w2 * k) * (1 - w2 / k)) / (1 + w2)
        out.append((1 - x) / (1 + x))
    return out


def q31(a):
    return int(round(a * 2147483648.0))


def main():
    if "--specs" in sys.argv:
        for name, n, tbw, what in DESIGNS:
            print("%-7s n=%d tbw=%.4f stopband %.1f dB  (%s)"
                  % (name, n, tbw, attenuation(n, tbw), what))
        return
    for name, n, tbw, what in DESIGNS:
        c = [q31(a) for a in coefs(n, tbw)]
        b0, b1 = c[0::2], c[1::2]
        print("/* %s; %d coefficients, designed stopband %.1f dB */"
              % (what, n, attenuation(n, tbw)))
        print("static const struct hb_coefs %s =" % name)
        print("{")
        print("    { %s," % ", ".join("%d" % v for v in b0 + [0] * (4 - len(b0))))
        print("      %s }," % ", ".join("%d" % v for v in b1 + [0] * (4 - len(b1))))
        print("    %d, %d" % (len(b0), len(b1)))
        print("};")


if __name__ == "__main__":
    main()
