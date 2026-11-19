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

"""#318: what flac.c plays for a FLAC stream wider than 29 bits, from the
reference decoder's samples: each frame, channel by channel, reduced to 29
bits with the TPDF dither libffmpegFLAC/dither.h describes, written here a
second time from that description (not from the C) so that run.sh can
compare the decoder's out29.raw with it byte for byte.

    dither.py REF.raw IN.ana BITS CHANNELS OUT.raw

REF.raw: flac -d's samples (right-justified, little-endian, 4 bytes each,
interleaved); IN.ana: flac -a's analysis of the same stream, for the frame
sizes (the dither runs a frame at a time, channel 0's block first).

The generator: x' = 1664525 x + 1013904223 mod 2^32, from x = 0 at the
start of the stream, one step a sample.  With s = BITS - 29: r1 = the top s
bits of x', r2 = bits 24-s..23; a sample v = q 2^s + f becomes
q + floor((f + r1 + r2 - 2^(s-1)) / 2^s), clamped to 29 bits.
"""
import re
import struct
import sys


def main():
    ref, ana, bits, ch, out = sys.argv[1:6]
    bits, ch = int(bits), int(ch)
    s = bits - 29
    assert 1 <= s <= 3, bits
    mask = (1 << s) - 1
    hi, lo = (1 << 28) - 1, -(1 << 28)
    data = open(ref, "rb").read()
    n = len(data) // 4
    v = struct.unpack("<%di" % n, data)
    sizes = [int(m.group(1)) for m in
             re.finditer(r"^frame=\d+.*?blocksize=(\d+)", open(ana).read(), re.M)]
    if not sizes or sum(sizes) * ch != n:
        sys.exit("dither.py: %d frames for %d samples x %d channels"
                 % (len(sizes), n // ch, ch))
    res = [0] * n
    x = 0
    pos = 0
    for bs in sizes:
        for c in range(ch):
            for i in range(bs):
                k = (pos + i) * ch + c
                x = (x * 1664525 + 1013904223) & 0xffffffff
                r1 = x >> (32 - s)
                r2 = (x >> (24 - s)) & mask
                q = (v[k] >> s) + (((v[k] & mask) + r1 + r2 - (1 << (s - 1))) >> s)
                res[k] = max(lo, min(hi, q))
        pos += bs
    open(out, "wb").write(struct.pack("<%di" % n, *res))


if __name__ == "__main__":
    main()
