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
#285: wide{1,2,3}.flac are the same but 32-bit (STREAMINFO), which the
codec refuses at open (flac.c: more bits than FLAC_OUTPUT_DEPTH), before
reading any frame; the library still indexes them.

#298 adds a mixed album, "Skip Gap": skipgap1.flac and skipgap3.flac play
(3 s of a quiet 441 Hz tone in verbatim FLAC frames, the same rate and
format as the broken one, so no rate switch), skipgap2.flac is broken as
above. Stdlib only; the same bytes on every run.
"""
import math
import os
import struct
import sys

SKIP_SECONDS = 3
BLOCK = 4096


def crc8(data):
    c = 0
    for b in data:
        c ^= b
        for _ in range(8):
            c = ((c << 1) ^ 0x07) & 0xFF if c & 0x80 else (c << 1) & 0xFF
    return c


def crc16(data):
    c = 0
    for b in data:
        c ^= b << 8
        for _ in range(8):
            c = ((c << 1) ^ 0x8005) & 0xFFFF if c & 0x8000 else (c << 1) & 0xFFFF
    return c


def utf8_num(n):
    """FLAC's UTF-8-style frame number (n < 2**31)"""
    if n < 0x80:
        return bytes([n])
    out = []
    while True:
        out.insert(0, 0x80 | (n & 0x3F))
        n >>= 6
        lead_bits = 6 - len(out)            # payload bits left in the lead
        if n < (1 << lead_bits):
            mark = (0xFF << (7 - len(out))) & 0xFF
            return bytes([mark | n] + out)


def frames(total):
    """verbatim stereo 16-bit frames of a quiet 441 Hz tone (100 periods
    per second at 44.1 kHz: whole periods, the same bytes on every run)"""
    out = b""
    fn = 0
    pos = 0
    while pos < total:
        n = min(BLOCK, total - pos)
        if n == BLOCK:
            hdr = bytes([0xFF, 0xF8, 0xC9, 0x18]) + utf8_num(fn)
        else:
            hdr = bytes([0xFF, 0xF8, 0x79, 0x18]) + utf8_num(fn) \
                + struct.pack(">H", n - 1)
        hdr += bytes([crc8(hdr)])
        sub = b""
        for _ in range(2):                          # left, right
            sub += bytes([0x02])                    # verbatim, no wasted bits
            for i in range(pos, pos + n):
                v = int(round(2000 * math.sin(2 * math.pi * (i % 100) / 100)))
                sub += struct.pack(">h", v)
        frame = hdr + sub
        frame += struct.pack(">H", crc16(frame))
        out += frame
        pos += n
        fn += 1
    return out


def flac(title, artist, album, track, bps=16, seconds=5, playable=False):
    rate, ch, total = 44100, 2, 44100 * seconds
    si = struct.pack(">HH", BLOCK, BLOCK)            # min/max block size
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
    if playable:
        return out + frames(total)
    return out + bytes(64 * 1024)                    # no frame: unplayable


def main():
    outdir = sys.argv[1] if len(sys.argv) > 1 else "build/unplayable-fixtures"
    os.makedirs(outdir, exist_ok=True)
    files = [(f"bad{i}.flac", flac(f"Silent {i}", "Nobody", "Unplayable", i))
             for i in (1, 2, 3)]
    files += [(f"wide{i}.flac", flac(f"Wide {i}", "Nobody", "Too Wide", i, 32))
              for i in (1, 2, 3)]
    for name, data in files:
        path = os.path.join(outdir, name)
        if not os.path.exists(path) or open(path, "rb").read() != data:
            with open(path, "wb") as f:
                f.write(data)
    for i in (1, 2, 3):
        path = os.path.join(outdir, f"skipgap{i}.flac")
        data = flac(f"Gap {i}", "Nobody", "Skip Gap", i,
                    seconds=SKIP_SECONDS, playable=i != 2)
        if not os.path.exists(path) or open(path, "rb").read() != data:
            with open(path, "wb") as f:
                f.write(data)
    print(outdir)


if __name__ == "__main__":
    main()
