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

"""Rewrite an MP4 with its 'moov' ahead of the media data (p081).

    tests/audiobook/faststart.py <in.m4b> <out.m4b>

The chapter fixtures all keep 'moov' at the end (ffmpeg's default). The
first metadata parse stops at the 'mdat' that follows a 'moov' it has read,
so the chapter count it infers (lib/rbcodec/metadata/mp4.c, p081) must be
checked in both layouts. Moves the top-level 'moov' in front of the first
'mdat' and shifts each 'stco'/'co64' chunk offset that points before the
old 'moov' by its size (media data behind it, as in rich_chapters.m4b's
second 'mdat', does not move); nothing else changes. Exits 1 on a file it can't rewrite.
"""
import struct
import sys

CONTAINERS = {b'moov', b'trak', b'mdia', b'minf', b'stbl', b'udta', b'edts'}


def atoms(buf, pos, end):
    while pos + 8 <= end:
        size, typ = struct.unpack('>I4s', buf[pos:pos + 8])
        if size == 1 or size < 8 or pos + size > end:
            raise ValueError('unsupported atom size at %d' % pos)
        yield pos, size, typ
        pos += size


def shift(buf, pos, end, delta, below):
    """Add delta to each chunk offset under 'below' (data that moves)."""
    for p, size, typ in atoms(buf, pos, end):
        if typ in CONTAINERS:
            shift(buf, p + 8, p + size, delta, below)
        elif typ in (b'stco', b'co64'):
            n = struct.unpack('>I', buf[p + 12:p + 16])[0]
            w, f = (8, '>Q') if typ == b'co64' else (4, '>I')
            for i in range(n):
                o = p + 16 + i * w
                v = struct.unpack(f, buf[o:o + w])[0]
                if v < below:
                    buf[o:o + w] = struct.pack(f, v + delta)


def main():
    data = bytearray(open(sys.argv[1], 'rb').read())
    top = list(atoms(data, 0, len(data)))
    first_mdat = next(p for p, _, t in top if t == b'mdat')
    moovs = [(p, s) for p, s, t in top if t == b'moov' and p > first_mdat]
    if len(moovs) != 1:
        sys.exit('want one moov after the media data, found %d' % len(moovs))
    moved = sum(s for _, s in moovs)
    out = bytearray()
    for p, s, t in top:
        if p == first_mdat:
            for mp, ms in moovs:
                m = bytearray(data[mp:mp + ms])
                shift(m, 8, ms, moved, mp)   # data behind it stays
                out += m
        if t == b'moov' and p > first_mdat:
            continue
        out += data[p:p + s]
    open(sys.argv[2], 'wb').write(out)


if __name__ == '__main__':
    main()
