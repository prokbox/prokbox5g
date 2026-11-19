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

# Regenerate the multi-value tag (#167) and numeric genre (#163) fixtures
# under tests/fixtures/mv/, used by tests/metatags/run.sh, tests/classify/
# run.sh and tests/ui/ipodui-167-multi-artist.script.
#
#   mv.flac      Vorbis comments: ARTIST x2 (not adjacent: an ALBUMARTIST
#                sits between them), ALBUMARTIST x2, COMPOSER x2 (adjacent)
#   mv_v24.mp3   ID3v2.4: TPE1 UTF-8 with three NUL-separated values (long:
#                Now Playing scrolls it), TPE2 UTF-16 with a BOM per value,
#                TCOM Latin-1 with a trailing NUL
#   g148.mp3     ID3v2.4 TCON "148"   (Abstract)
#   g183.mp3     ID3v2.3 TCON "(183)" (Audiobook)
#   g191.mp3     ID3v1 only, genre byte 191 (Psybient)
#
# The audio comes from ffmpeg (libmp3lame, flac); every tag is written here
# byte by byte, so each case is exactly what it says. Check the result with
# ffprobe -show_entries format_tags (ffmpeg joins repeated values with ";").
import os
import struct
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
OUT = os.path.join(HERE, 'mv')
FF = ['ffmpeg', '-nostdin', '-v', 'error', '-y']
BITEXACT = ['-fflags', '+bitexact', '-flags:a', '+bitexact']

A1, A2 = 'Aardvark Multi', 'Aardwolf Second'
A3 = 'Abacus Third With A Rather Long Name'


def audio(path, hz, codec):
    subprocess.run(FF + ['-f', 'lavfi', '-i',
                         'sine=frequency=%d:duration=8:sample_rate=8000' % hz,
                         '-ac', '1'] + codec + BITEXACT + ['-map_metadata',
                         '-1', path], check=True)


def synchsafe(n):
    return bytes([(n >> 21) & 0x7f, (n >> 14) & 0x7f, (n >> 7) & 0x7f,
                  n & 0x7f])


def frame(fid, data, version):
    size = synchsafe(len(data)) if version == 4 else struct.pack('>I',
                                                                 len(data))
    return fid.encode() + size + b'\0\0' + data


def id3v2(frames, version):
    body = b''.join(frame(f, d, version) for f, d in frames)
    return b'ID3' + bytes([version, 0, 0]) + synchsafe(len(body)) + body


def utf16(*values):
    return b'\x01' + b'\0\0'.join(b'\xff\xfe' + v.encode('utf-16-le')
                                  for v in values)


def mp3(name, hz, tag=b'', v1=b''):
    with tempfile.TemporaryDirectory() as tmp:
        raw = os.path.join(tmp, 'a.mp3')
        audio(raw, hz, ['-c:a', 'libmp3lame', '-b:a', '8k',
                        '-write_xing', '0', '-id3v2_version', '0',
                        '-write_id3v1', '0'])
        with open(raw, 'rb') as f:
            data = f.read()
    with open(os.path.join(OUT, name), 'wb') as f:
        f.write(tag + data + v1)


def flac(name, hz, comments):
    with tempfile.TemporaryDirectory() as tmp:
        raw = os.path.join(tmp, 'a.flac')
        audio(raw, hz, ['-c:a', 'flac'])
        with open(raw, 'rb') as f:
            data = f.read()
    assert data[:4] == b'fLaC'
    pos, blocks = 4, []
    while True:
        hdr = data[pos]
        size = int.from_bytes(data[pos + 1:pos + 4], 'big')
        btype = hdr & 0x7f
        if btype not in (1, 4):          # drop padding and ffmpeg's comment
            blocks.append((btype, data[pos + 4:pos + 4 + size]))
        pos += 4 + size
        if hdr & 0x80:
            break
    vendor = b'prokbox-fixture'
    vc = struct.pack('<I', len(vendor)) + vendor
    vc += struct.pack('<I', len(comments))
    for c in comments:
        b = c.encode()
        vc += struct.pack('<I', len(b)) + b
    blocks.insert(1, (4, vc))
    out = b'fLaC'
    for i, (btype, body) in enumerate(blocks):
        last = 0x80 if i == len(blocks) - 1 else 0
        out += bytes([last | btype]) + len(body).to_bytes(3, 'big') + body
    with open(os.path.join(OUT, name), 'wb') as f:
        f.write(out + data[pos:])


def utf8(*values, trailing=False):
    return b'\x03' + b'\0'.join(v.encode() for v in values) + \
        (b'\0' if trailing else b'')


def latin1(*values, trailing=False):
    return b'\x00' + b'\0'.join(v.encode('latin-1') for v in values) + \
        (b'\0' if trailing else b'')


def id3v1(genre):
    def pad(s, n):
        return s.encode().ljust(n, b'\0')
    return (b'TAG' + pad('Genre 191', 30) + pad('Genre Test', 30) +
            pad('Genre Album', 30) + b'2026' + pad('', 30) + bytes([genre]))


def main():
    os.makedirs(OUT, exist_ok=True)
    flac('mv.flac', 523, [
        'TITLE=Multi One Song', 'ALBUM=Multi Album',
        'ARTIST=' + A1, 'ALBUMARTIST=' + A1, 'ARTIST=' + A2,
        'ALBUMARTIST=' + A2, 'COMPOSER=Comp One', 'COMPOSER=Comp Two',
        'GENRE=Rock', 'TRACKNUMBER=1'])
    mp3('mv_v24.mp3', 587, id3v2([
        ('TIT2', utf8('Multi Two Song')),
        ('TALB', utf8('Multi Album')),
        ('TPE1', utf8(A1, A2, A3)),
        ('TPE2', utf16(A1, A2)),
        ('TCOM', latin1('Comp One', 'Comp Two', trailing=True)),
        ('TCON', utf8('Rock')),
        ('TRCK', utf8('2')),
    ], 4))
    mp3('g148.mp3', 659, id3v2([('TIT2', utf8('Genre 148')),
                                ('TCON', utf8('148'))], 4))
    mp3('g183.mp3', 698, id3v2([('TIT2', latin1('Genre 183')),
                                ('TCON', latin1('(183)'))], 3))
    mp3('g191.mp3', 740, v1=id3v1(191))
    for n in sorted(os.listdir(OUT)):
        print(n, os.path.getsize(os.path.join(OUT, n)))
    return 0


if __name__ == '__main__':
    sys.exit(main())
