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

# Independent oracle for the chapter goldens (tests/audiobook/golden):
# prints the "Chapter N: <ms> ms "<title>"" lines warble -m should print
# for a fixture, from `ffprobe -show_chapters`, with the firmware's title
# rules applied in Python rather than taken from warble:
#   - titles cut to 63 bytes on a UTF-8 boundary (AB_MAX_CHAPTER_NAME);
#   - the packed table's pool (chapters.h, P6 Rev 1 item 15): 24,576 B,
#     the last 64 never handed out, pool[0] = "", each stored title costs
#     len + 1; title i may use (left / (n - i)) incl. its NUL, cut on a
#     UTF-8 boundary; an empty result stores "".
# and a "Titles cut: N (pool U of S B)" line when any title was cut.
#
#   tests/audiobook/ffprobe_chapters.py tests/fixtures/x.mp3 [cap]
# then diff against the chapter lines of the golden.
import json
import subprocess
import sys

POOL = 24576
NAME = 64


def utf8_cut(b, n):
    """largest n' <= n not splitting the character at b[n]"""
    while 0 < n < len(b) and (b[n] & 0xc0) == 0x80:
        n -= 1
    return n


def main():
    path = sys.argv[1]
    cap = int(sys.argv[2]) if len(sys.argv) > 2 else 1024
    out = subprocess.run(['ffprobe', '-v', 'error', '-show_chapters',
                          '-of', 'json', path], check=True,
                         capture_output=True).stdout
    chaps = json.loads(out)['chapters'][:cap]
    n = len(chaps)
    used, cut = 1, 0
    usable = POOL - NAME
    for i, c in enumerate(chaps):
        ms = int(round(float(c['start_time']) * 1000))
        t = c.get('tags', {}).get('title', '').encode('utf-8')
        if len(t) > NAME - 1:
            t = t[:utf8_cut(t, NAME - 1)]
        left = usable - used
        share = left // (n - i) if left > 0 else 0
        if t and len(t) + 1 > share:
            t = t[:utf8_cut(t, share - 1)] if share > 1 else b''
            cut += 1
        if t:
            used += len(t) + 1
        print('Chapter %d: %d ms "%s"' % (i + 1, ms, t.decode('utf-8')))
    if cut:
        print('Titles cut: %d (pool %d of %d B)' % (cut, used, POOL))


if __name__ == '__main__':
    main()
