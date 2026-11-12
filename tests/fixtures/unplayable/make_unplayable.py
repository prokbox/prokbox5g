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

"""#281: unplayable FLAC tracks for the Shuffle-storm UI scripts.

    tests/fixtures/unplayable/make_unplayable.py [outdir]
                                         (default build/unplayable-fixtures)

Each file is a valid FLAC header (STREAMINFO: 44.1 kHz, 16-bit stereo,
5 s; a Vorbis comment with title, artist, album and track number), so the
library indexes it as a 5 s song, followed by zero bytes where the frames
should be: the decoder fails on the first frame header and inserts no PCM.
Stdlib only; the same bytes on every run.
"""
import os
import struct
import sys


def flac(title, artist, album, track):
    rate, ch, bps, total = 44100, 2, 16, 44100 * 5
    si = struct.pack(">HH", 4096, 4096)              # min/max block size
    si += (0).to_bytes(3, "big") + (0).to_bytes(3, "big")  # frame sizes
    # 20 bits rate, 3 bits channels-1, 5 bits bps-1, 36 bits total samples
    v = (rate << 44) | ((ch - 1) << 41) | ((bps - 1) << 36) | total
    si += v.to_bytes(8, "big") + bytes(16)           # + MD5 (unknown)
    assert len(si) == 34
    vendor = b"prokbox-test"
    tags = [f"TITLE={title}", f"ARTIST={artist}", f"ALBUM={album}",
            f"TRACKNUMBER={track}"]
    vc = struct.pack("<I", len(vendor)) + vendor + struct.pack("<I", len(tags))
    for t in tags:
        b = t.encode()
        vc += struct.pack("<I", len(b)) + b
    out = b"fLaC"
    out += bytes([0]) + len(si).to_bytes(3, "big") + si
    out += bytes([0x80 | 4]) + len(vc).to_bytes(3, "big") + vc
    return out + bytes(64 * 1024)                    # no frame: unplayable


def main():
    outdir = sys.argv[1] if len(sys.argv) > 1 else "build/unplayable-fixtures"
    os.makedirs(outdir, exist_ok=True)
    for i in (1, 2, 3):
        path = os.path.join(outdir, f"bad{i}.flac")
        data = flac(f"Silent {i}", "Nobody", "Unplayable", i)
        if not os.path.exists(path) or open(path, "rb").read() != data:
            with open(path, "wb") as f:
                f.write(data)
    print(outdir)


if __name__ == "__main__":
    main()
