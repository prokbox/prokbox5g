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

# Regenerate tests/fixtures/opusstart/tone_art.opus for tests/opusstart/run.sh
# (p083-codecload: an Opus start that took 15 s on the device).
#
# The file: 2 s of a two-tone stereo signal from ffmpeg's libopus (64 kbps),
# whose comment header is replaced by art_2page.opus's (tests/fixtures/oggart/,
# a ~60 KB cover in base64 across two full 255-segment pages, the second a
# continuation page). So the codec's start has ~113 KB of header to pass
# before the first audio page, and real audio after it to check the decode
# against (the oggart fixtures are silence).
#
# Pages are re-serialised (one serial, sequence numbers renumbered, CRCs
# recomputed). Needs python3 and ffmpeg with libopus.
# Usage: make_opus_start_fixture.py [OUTFILE]
import os
import struct
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
OUT = sys.argv[1] if len(sys.argv) > 1 else os.path.join(
    HERE, 'opusstart', 'tone_art.opus')
ART = os.path.join(HERE, 'oggart', 'art_2page.opus')


def _crc_table():
    t = []
    for i in range(256):
        r = i << 24
        for _ in range(8):
            r = ((r << 1) ^ 0x04C11DB7) if r & 0x80000000 else (r << 1)
        t.append(r & 0xFFFFFFFF)
    return t


CRC = _crc_table()


def ogg_crc(data):
    c = 0
    for b in data:
        c = ((c << 8) ^ CRC[((c >> 24) ^ b) & 0xFF]) & 0xFFFFFFFF
    return c


def read_pages(data):
    pages, p = [], 0
    while p < len(data):
        assert data[p:p + 4] == b'OggS', p
        nseg = data[p + 26]
        lacing = data[p + 27:p + 27 + nseg]
        body = p + 27 + nseg
        size = sum(lacing)
        pages.append(dict(flags=data[p + 5],
                          granule=struct.unpack('<q', data[p + 6:p + 14])[0],
                          lacing=bytes(lacing),
                          body=data[body:body + size]))
        p = body + size
    return pages


def page_bytes(pg, serial, seq):
    h = bytearray(b'OggS' + bytes([0, pg['flags']])
                  + struct.pack('<qIII', pg['granule'], serial, seq, 0)
                  + bytes([len(pg['lacing'])]) + pg['lacing'])
    h[22:26] = struct.pack('<I', ogg_crc(bytes(h) + pg['body']))
    return bytes(h) + pg['body']


def tone():
    fd, path = tempfile.mkstemp(suffix='.opus')
    os.close(fd)
    subprocess.check_call([
        'ffmpeg', '-loglevel', 'error', '-y', '-f', 'lavfi', '-t', '2',
        '-i', 'sine=f=440:r=48000', '-f', 'lavfi', '-t', '2',
        '-i', 'sine=f=660:r=48000',
        '-filter_complex', '[0:a][1:a]join=inputs=2:channel_layout=stereo',
        '-map_metadata', '-1', '-fflags', '+bitexact', '-flags:a', '+bitexact',
        '-c:a', 'libopus', '-b:a', '64k', path])
    data = open(path, 'rb').read()
    os.unlink(path)
    return data


def main():
    tp = read_pages(tone())
    ap = read_pages(open(ART, 'rb').read())
    # tone: page 0 OpusHead, page 1 OpusTags, then audio
    assert tp[0]['body'][:8] == b'OpusHead' and tp[1]['body'][:8] == b'OpusTags'
    # art: page 0 OpusHead, then the comment packet's pages (the first one
    # fresh, the rest continuations), then audio
    assert ap[1]['body'][:8] == b'OpusTags'
    tags = [ap[1]]
    for pg in ap[2:]:
        if not pg['flags'] & 1:
            break
        tags.append(pg)
    assert len(tags) >= 2, 'the comment header must span pages'
    serial = 0x50726f6b
    out, seq = [], 0
    for pg in [tp[0]] + tags + tp[2:]:
        out.append(page_bytes(pg, serial, seq))
        seq += 1
    os.makedirs(os.path.dirname(OUT), exist_ok=True)
    with open(OUT, 'wb') as f:
        f.write(b''.join(out))
    print('%s: %d bytes, %d comment pages' % (OUT, sum(map(len, out)),
                                               len(tags)))


main()
