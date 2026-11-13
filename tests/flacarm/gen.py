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

"""Test signals for tests/flacarm/run.sh (deterministic, stdlib only).

    gen.py OUT.raw BITS RATE SECONDS KIND

writes stereo signed little-endian PCM.  KIND:

  music  something shaped like a recording, so the encoder makes the
         choices it makes on real music: a chord of decaying partials that
         changes every 0.25 s, a noise burst at each change (transients:
         short-order and fixed subframes), left and right mostly shared with
         a little independent content (left/side, mid/side), and a noise
         floor of about 2^(BITS-17) LSB rms -- what a good 24-bit recording
         carries in its low bits (-96 dB), and under 1 LSB at 16 bits.  The Rice
         parameters land where real files put them.
  noise  loud wide-band noise over a sine: big residuals, high Rice
         parameters, escape partitions and verbatim subframes.
  quiet  a near-silent stretch, then digital silence: constant subframes,
         Rice parameter 0, wasted bits.
"""
import math
import random
import sys


def main():
    out, bits, rate, secs, kind = sys.argv[1:6]
    bits, rate = int(bits), int(rate)
    n = int(float(secs) * rate)
    full = (1 << (bits - 1)) - 1
    rnd = random.Random(20261110 + bits * 7 + rate)
    w = bits // 8
    buf = bytearray()
    floor = 2.0 ** (bits - 17)
    seg = rate // 4
    partials = []
    burst = 0.0
    lp = [0.0, 0.0]
    for i in range(n):
        if i % seg == 0:
            base = 110.0 * 2 ** (rnd.randrange(24) / 12.0)
            partials = [(base * k * (1 + 0.002 * rnd.random()),
                         0.5 / k ** 1.3 * rnd.random(), rnd.random() * 6.28)
                        for k in range(1, 9)]
            burst = 0.05
        t = i / rate
        env = math.exp(-3.0 * (i % seg) / rate)
        if kind == "music":
            s = sum(a * math.sin(2 * math.pi * f * t + ph)
                    for f, a, ph in partials) * env * 0.6
            burst *= 0.999
            v = []
            for c in range(2):
                x = rnd.gauss(0, 1)
                lp[c] = 0.7 * lp[c] + 0.3 * x
                v.append(s * (1.0 if c == 0 else 0.85)
                         + burst * lp[c] * 0.5
                         + (rnd.gauss(0, 1) * floor + lp[c] * floor) / full)
        elif kind == "noise":
            s = math.sin(i * 0.031) * 0.5
            v = [s + 0.25 * rnd.uniform(-1, 1),
                 s * 0.8 + (0.4 if i % 50000 < 25000 else 0.02)
                 * rnd.uniform(-1, 1)]
        else:  # quiet
            a = 0.0 if i > n // 2 else 0.001 * math.sin(i * 0.01)
            v = [a, a * 0.5]
        for x in v:
            q = max(-full - 1, min(full, int(round(x * full))))
            if kind == "quiet" and bits > 16:
                q &= ~0xff  # 8 wasted bits
            buf += (q & ((1 << bits) - 1)).to_bytes(w, "little")
    open(out, "wb").write(buf)


if __name__ == "__main__":
    main()
