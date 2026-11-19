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
"""Crafted MP3s for tests/mp3skip, and the WAV sample count.

  lametag.py set IN.mp3 OUT.mp3 DELAY [PADDING]
      copy IN with the LAME tag's encoder delay (and padding) replaced:
      the 12-bit fields Rockbox reads as lead_trim/tail_trim
      (lib/rbcodec/metadata/mp3data.c get_xing_info()). Prints the old
      delay and padding. Refuses a file without a Xing/Info + LAME tag.
  lametag.py frames OUT.wav
      the WAV's sample frames (0 for warble's bare placeholder: no PCM)
"""
import struct
import sys
import wave

FRAMES, BYTES, TOC, QUALITY = 1, 2, 4, 8


def tag_offset(data):
    """the byte offset of the delay/padding field in the first frame"""
    if data[:3] == b'ID3':
        raise SystemExit('lametag: write the MP3 without ID3v2 (-id3v2_version 0)')
    if data[0] != 0xFF or (data[1] & 0xE0) != 0xE0:
        raise SystemExit('lametag: no frame sync at offset 0')
    for magic in (b'Xing', b'Info'):
        at = data.find(magic, 4, 64)
        if at >= 0:
            break
    else:
        raise SystemExit('lametag: no Xing/Info tag in the first frame')
    flags = struct.unpack('>I', data[at + 4:at + 8])[0]
    i = at + 8
    i += 4 if flags & FRAMES else 0
    i += 4 if flags & BYTES else 0
    i += 100 if flags & TOC else 0
    i += 4 if flags & QUALITY else 0
    enc = data[i:i + 4]
    if enc not in (b'LAME', b'Lavc', b'Lavf'):
        raise SystemExit('lametag: no LAME extension (encoder %r)' % enc)
    return i + 21


def main(argv):
    if len(argv) >= 3 and argv[1] == 'frames':
        with open(argv[2], 'rb') as f:
            head = f.read(64)
        if not head.startswith(b'RIFF'):
            # warble writes its header with the first PCM: without any,
            # the file is only the 46-byte placeholder (warble.c
            # WAVE_HEADER_SIZE, sizes patched at quit)
            if len(head) <= 46:
                print(0)
                return 0
            raise SystemExit('lametag: %s is no WAV' % argv[2])
        with wave.open(argv[2], 'rb') as w:
            print(w.getnframes())
        return 0
    if len(argv) < 5 or argv[1] != 'set':
        print(__doc__, file=sys.stderr)
        return 2
    data = bytearray(open(argv[2], 'rb').read())
    o = tag_offset(data)
    old = (data[o] << 16) | (data[o + 1] << 8) | data[o + 2]
    delay = int(argv[4])
    pad = int(argv[5]) if len(argv) > 5 else old & 0xFFF
    if not (0 <= delay <= 2880 and 0 <= pad <= 2304):
        raise SystemExit('lametag: out of the range Rockbox accepts')
    v = (delay << 12) | pad
    data[o:o + 3] = bytes(((v >> 16) & 0xFF, (v >> 8) & 0xFF, v & 0xFF))
    open(argv[3], 'wb').write(data)
    print('%d %d' % (old >> 12, old & 0xFFF))
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv))
