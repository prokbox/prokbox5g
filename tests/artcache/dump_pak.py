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

"""Dump an art cache (.rockbox/artcache/) for inspection: the index records
and a contact sheet PNG of every thumbnail in thumbs.pak.

  dump_pak.py <artcache dir> [sheet.png]

Formats: apps/ipodui/artfmt.h. Prints one line per index record (key,
state, slot, source kind/type/size/crc) and checks each OK record's slot
tag (key, pixel CRC). Exit 1 on any inconsistency. No third-party modules.
"""
import struct
import sys
import zlib

STATES = {1: "OK", 2: "NOART", 3: "FAILED"}


def png(path, w, h, rows):
    def chunk(t, d):
        c = struct.pack(">I", len(d)) + t + d
        return c + struct.pack(">I", zlib.crc32(t + d) & 0xffffffff)
    raw = b"".join(b"\0" + r for r in rows)
    with open(path, "wb") as f:
        f.write(b"\x89PNG\r\n\x1a\n")
        f.write(chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 2, 0, 0, 0)))
        f.write(chunk(b"IDAT", zlib.compress(raw, 9)))
        f.write(chunk(b"IEND", b""))


def main():
    d = sys.argv[1]
    bad = 0
    idx = open(d + "/index.dat", "rb").read()
    magic, ver, gen, count, n_slots, rec_crc, sweep, hcrc = \
        struct.unpack_from("<8I", idx, 0)
    ok_hdr = (magic == 0x58415049 and
              hcrc == zlib.crc32(idx[:28]) & 0xffffffff and
              rec_crc == zlib.crc32(idx[512:512 + 32 * count]) & 0xffffffff)
    print("index: gen %d, %d records, %d slots, sweep_commit %d, %s" %
          (gen, count, n_slots, sweep, "valid" if ok_hdr else "INVALID"))
    bad += not ok_hdr
    pak = open(d + "/thumbs.pak", "rb").read()
    pm, pv, w, h, slot_bytes, pcrc = struct.unpack_from("<IIHHII", pak, 0)
    print("pak: %dx%d, slot %d bytes, %d bytes" % (w, h, slot_bytes, len(pak)))
    thumbs = []
    for i in range(count):
        key, slot, state, kind, typ, _, mtime, size, crc, h32 = \
            struct.unpack_from("<QIBBBBIIII", idx, 512 + 32 * i)
        line = "%016x %-6s slot=%-5s src=%d type=%d size=%d crc=%08x" % (
            key, STATES.get(state, "?%d" % state),
            slot if slot != 0xffffffff else "-", kind, typ, size, crc)
        if state == 1:
            off = 512 + slot * slot_bytes
            pix = pak[off:off + w * h * 2]
            tkey, tcrc, tmagic = struct.unpack_from("<QII", pak, off + w * h * 2)
            good = (tkey == key and tmagic == 0x53545049 and
                    tcrc == zlib.crc32(pix) & 0xffffffff)
            if not good:
                line += "  BAD SLOT"
                bad += 1
            thumbs.append(pix)
        print(line)
    if len(sys.argv) > 2 and thumbs:
        cols = 8
        rows_n = (len(thumbs) + cols - 1) // cols
        W, H = cols * (w + 2), rows_n * (h + 2)
        img = [bytearray(W * 3) for _ in range(H)]
        for n, pix in enumerate(thumbs):
            ox, oy = (n % cols) * (w + 2), (n // cols) * (h + 2)
            for y in range(h):
                for x in range(w):
                    v = pix[2 * (y * w + x)] | pix[2 * (y * w + x) + 1] << 8
                    r, g, b = (v >> 11) << 3, ((v >> 5) & 63) << 2, (v & 31) << 3
                    img[oy + y][3 * (ox + x):3 * (ox + x) + 3] = bytes((r, g, b))
        png(sys.argv[2], W, H, [bytes(r) for r in img])
        print("sheet: %s (%d thumbs)" % (sys.argv[2], len(thumbs)))
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
