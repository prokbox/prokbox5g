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
"""Split an iPod Video (5G/5.5G) firmware partition image for RE work.

Input is the Firmware-25.* file from Apple's iPod_25.*.ipsw (a zip).
Output, in OUT_DIR:
  <id>.bin   each directory entry as stored (2 KB header + payload)
  osos.code  the ARM OS payload, load address 0x10000000
  rsrc.fat   the resource volume (FAT16; boot sector at offset 0x600)
and prints name, offsets and SHA-256 for docs/research/bcm2722/artefacts.md.

The VideoCore files live in rsrc.fat under /Resources/VideoCore; copy them
out with mtools:  mcopy -s -n -i OUT_DIR/rsrc.fat@@1536 ::/Resources/VideoCore DEST

Never commit anything this script writes: it is Apple's code.

Usage: fwextract.py Firmware-25.x.y OUT_DIR
"""
import hashlib
import os
import struct
import sys

DIR_OFFSET = 0x4200      # image directory, 40-byte '!ATA' entries
IMAGE_HEADER = 0x800     # per-image header before the payload


def main():
    if len(sys.argv) != 3:
        sys.exit(__doc__)
    src, out = sys.argv[1], sys.argv[2]
    data = open(src, 'rb').read()
    os.makedirs(out, exist_ok=True)
    off = DIR_OFFSET
    while data[off:off + 4] == b'!ATA':
        (_, ident, _, dev, length, addr, entry,
         _, _, _) = struct.unpack_from('<4s4sIIIIIIII', data, off)
        name = ident[::-1].decode('ascii')
        blob = data[dev:dev + length]
        with open(os.path.join(out, name + '.bin'), 'wb') as f:
            f.write(blob)
        print(f'{name} dev=0x{dev:x} len=0x{length:x} addr=0x{addr:x} '
              f'entry=0x{entry:x} sha256={hashlib.sha256(blob).hexdigest()}')
        if name == 'osos':
            with open(os.path.join(out, 'osos.code'), 'wb') as f:
                f.write(blob[IMAGE_HEADER:])
        elif name == 'rsrc':
            with open(os.path.join(out, 'rsrc.fat'), 'wb') as f:
                f.write(blob[IMAGE_HEADER:])
        off += 40


if __name__ == '__main__':
    main()
