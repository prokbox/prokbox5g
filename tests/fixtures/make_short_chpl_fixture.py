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

"""Make short_chpl.m4b: nero_chapters.m4b with a truncated 'chpl' atom.

A 'udta' holding a 12-byte 'chpl' (4 payload bytes, under the 9 the
parser needs for version/flags/reserved/count) is put in 'moov' right
after 'mvhd', ahead of 'trak'. Upstream's `size -= 9` on that atom
wrapped to ~4 GB and the next seek skipped 'trak', so the file lost its
codec and length. The fixed parser skips the atom and reads the rest.
'mdat' sits before 'moov', so no chunk offsets move.

Usage: make_short_chpl_fixture.py [src] [dst]   (run from tests/fixtures)
"""
import struct
import sys

src = sys.argv[1] if len(sys.argv) > 1 else "nero_chapters.m4b"
dst = sys.argv[2] if len(sys.argv) > 2 else "short_chpl.m4b"
data = open(src, "rb").read()

off = 0
while off + 8 <= len(data):
    size, kind = struct.unpack(">I4s", data[off:off + 8])
    if kind == b"moov":
        break
    off += size
else:
    sys.exit("no moov in " + src)

mvhd_size = struct.unpack(">I", data[off + 8:off + 12])[0]
insert_at = off + 8 + mvhd_size

chpl = struct.pack(">I4s", 12, b"chpl") + b"\x01\x00\x00\x00"
udta = struct.pack(">I4s", 8 + len(chpl), b"udta") + chpl

out = bytearray(data[:insert_at] + udta + data[insert_at:])
struct.pack_into(">I", out, off, size + len(udta))
open(dst, "wb").write(out)
print("wrote %s (%d bytes)" % (dst, len(out)))
