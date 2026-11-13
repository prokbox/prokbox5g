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

"""flac -d's WAV of a stream whose bits per sample is not 8/16/24/32 (the
widths its raw output takes), as the raw PCM tests/flacarm's driver writes:
each sample right-justified, little-endian, in (bps + 7) / 8 bytes.  WAV
holds it left-justified in the container, so it is shifted back down.

    wav2raw.py IN.wav OUT.raw BPS
"""
import struct
import sys


def main():
    src, dst, bps = sys.argv[1], sys.argv[2], int(sys.argv[3])
    d = open(src, "rb").read()
    i = d.find(b"fmt ")
    container = struct.unpack("<H", d[i + 22:i + 24])[0]
    i = d.find(b"data")
    n = struct.unpack("<I", d[i + 4:i + 8])[0]
    x = d[i + 8:i + 8 + n]
    cb = container // 8
    w = (bps + 7) // 8
    out = bytearray()
    for k in range(0, len(x), cb):
        v = int.from_bytes(x[k:k + cb], "little", signed=True)
        if v & ((1 << (container - bps)) - 1):
            sys.exit("wav2raw: sample %d not left-justified" % (k // cb))
        v >>= container - bps
        out += (v & ((1 << (8 * w)) - 1)).to_bytes(w, "little")
    if not x:
        sys.exit("wav2raw: no samples")
    open(dst, "wb").write(out)


if __name__ == "__main__":
    main()
