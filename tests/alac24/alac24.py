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

"""24-bit ALAC bit-exact test helper (tests/alac24/run.sh).

    alac24.py gen OUT.raw BITS CH N    little-endian PCM, CH channels,
                                       N frames, full-scale with noise in
                                       the low byte (so a lost low byte
                                       shows in nearly every sample)
    alac24.py same SRC.raw BITS CH OUT.raw
                                       OUT is warble -r output (int32,
                                       interleaved stereo, << (29 - BITS);
                                       a mono source comes out on both
                                       channels): exit 0
                                       when every sample equals SRC;
                                       otherwise prints the first mismatch
                                       and how many samples differ
    alac24.py samepcm SRC.raw REF.raw  byte-identical reference decode
"""
import math
import struct
import sys


def gen(path, bits, ch, n):
    full = (1 << (bits - 1)) - 1
    w = bits // 8
    x = 2024
    out = bytearray()
    for i in range(n):
        base = math.sin(i * 0.013) * 0.45
        for c in range(ch):
            x = (x * 1103515245 + 12345) & 0x7FFFFFFF
            noise = ((x >> 8) % 2001 - 1000) / 1000.0
            # the two channels partly correlated (ALAC's weighted
            # mid-side stereo) and partly not
            amp = 0.3 if (i // 4096) % 2 else 0.01
            v = int((base * (1.0 - 0.2 * c) + amp * noise) * full)
            v = max(-full - 1, min(full, v))
            out += (v & ((1 << bits) - 1)).to_bytes(w, "little")
    open(path, "wb").write(out)


def same(src_path, bits, ch, out_path):
    src = open(src_path, "rb").read()
    out = open(out_path, "rb").read()
    w = bits // 8
    n = len(src) // w
    dup = 2 // ch                       # mono: each sample on both channels
    if len(out) != n * dup * 4:
        print("length %d samples, want %d" % (len(out) // 4, n * dup))
        return 1
    shift = 29 - bits
    got = struct.unpack("<%di" % (n * dup), out)
    first = None
    bad = 0
    for j in range(n * dup):
        i = j // dup
        want = int.from_bytes(src[i * w:i * w + w], "little", signed=True)
        if got[j] != want << shift:
            bad += 1
            if first is None:
                first = (j, got[j] >> shift, want)
    if bad:
        print("%d of %d samples differ; first at %d: %d, want %d"
              % ((bad, n * dup) + first))
        return 1
    print("%d samples bit-exact" % (n * dup))
    return 0


def main():
    cmd = sys.argv[1]
    if cmd == "gen":
        gen(sys.argv[2], int(sys.argv[3]), int(sys.argv[4]), int(sys.argv[5]))
        return 0
    if cmd == "same":
        return same(sys.argv[2], int(sys.argv[3]), int(sys.argv[4]),
                    sys.argv[5])
    if cmd == "samepcm":
        a = open(sys.argv[2], "rb").read()
        b = open(sys.argv[3], "rb").read()
        if a == b and a:
            print("%d bytes identical" % len(a))
            return 0
        print("differs (%d vs %d bytes)" % (len(a), len(b)))
        return 1
    print(__doc__)
    return 2


if __name__ == "__main__":
    sys.exit(main())
