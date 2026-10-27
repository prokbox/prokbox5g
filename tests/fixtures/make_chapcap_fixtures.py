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

# Regenerate the chapter-capacity fixtures used by tests/audiobook/run.sh
# (issue #48: AB_MAX_CHAPTERS is 1024 in the iPod UI shell and the host
# tests, 255 in stock).
#
#   chapters_648.m4b       648 chapters, as ffmpeg writes them: a Nero
#                          'chpl' cut at 255 entries *and* a QuickTime
#                          chapter text track with all 648. The parser must
#                          take the longer text track.
#   chapters_1100.m4b      1100 chapters (QuickTime track; 'chpl' cut at
#                          255): the table stops cleanly at 1024.
#   chapters_648_mp4v2.m4b chapters_648.m4b with its 'chpl' replaced by an
#                          mp4v2-style one (version 1, one reserved byte, a
#                          32-bit count) holding all 648, and the audio
#                          track's tref 'chap' renamed 'free', so 'chpl' is
#                          the only chapter list.
#   chapters_300.mp3       300 ID3v2.3 CHAP frames (ffmpeg's mp3 muxer).
#   chapters_300_v24.mp3   the same as ID3v2.4: ffmpeg 9 writes the v2.4
#                          CTOC frame size unsynchsafed (0x6a0), which hid
#                          every later frame from id3tags.c (#50).
#   chapters_400_long.m4b  400 chapters with 60-byte titles (ffmpeg: 'chpl'
#                          cut at 255 + the text track): the text track's
#                          titles must get the whole pool, not what the
#                          'chpl' left (P6 Rev 1 item 15): none is cut.
#   chapters_450_utf8.mp3  450 ID3v2.3 CHAPs with 60-byte UTF-8 titles
#                          (27,450 B > the 24,512 B pool): every title gets
#                          the same fair share, cut on a UTF-8 boundary.
#
# Chapter i (0-based) starts at 2*i ms and is titled "C<i+1>"; the audio is
# silence just long enough to hold them. Requires ffmpeg (aac, libmp3lame)
# and python3. Run from anywhere; writes next to this script.
import os
import struct
import subprocess
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
FF = ['ffmpeg', '-nostdin', '-v', 'error', '-y']
BITEXACT = ['-fflags', '+bitexact', '-flags:a', '+bitexact']
STEP_MS = 2


def title_short(i):
    return 'C%d' % (i + 1)


def title_long(i):
    return ('C%03d ' % (i + 1)) + 'x' * 55          # 60 B


def title_utf8(i):
    return ('C%03d ' % (i + 1)) + '\u00e9' * 27     # 6 + 54 = 60 B


def ffmeta(path, n, title=title_short):
    with open(path, 'w', encoding='utf-8') as f:
        f.write(';FFMETADATA1\ntitle=Chapter Cap %d\nalbum=Chapter Cap %d\n'
                % (n, n))
        for i in range(n):
            f.write('[CHAPTER]\nTIMEBASE=1/1000\nSTART=%d\nEND=%d\n'
                    'title=%s\n' % (i * STEP_MS, (i + 1) * STEP_MS, title(i)))


def encode(tmp, n, out, codec, title=title_short):
    meta = os.path.join(tmp, 'meta%d.txt' % n)
    ffmeta(meta, n, title)
    secs = '%.3f' % (n * STEP_MS / 1000.0 + 0.2)
    args = ['-f', 'lavfi', '-i', 'anullsrc=r=8000:cl=mono', '-i', meta,
            '-t', secs, '-map', '0:a', '-map_metadata', '1',
            '-map_chapters', '1'] + codec + BITEXACT + [out]
    subprocess.run(FF + args, check=True)


# ---------------------------------------------------------- MP4 boxes -----

def boxes(data, pos, end):
    while pos + 8 <= end:
        size, typ = struct.unpack('>I4s', data[pos:pos + 8])
        if size < 8 or pos + size > end:
            raise ValueError('bad box at %d' % pos)
        yield pos, size, typ
        pos += size


def find(data, path, pos=0, end=None):
    """offset and size of the first box at 'path' (list of fourccs)"""
    end = len(data) if end is None else end
    for p, s, t in boxes(data, pos, end):
        if t == path[0]:
            return (p, s) if len(path) == 1 else find(data, path[1:], p + 8,
                                                      p + s)
    raise KeyError(path)


def mp4v2_chpl(n):
    body = struct.pack('>B3sBI', 1, b'\0\0\0', 0, n)
    for i in range(n):
        title = ('C%d' % (i + 1)).encode()
        body += struct.pack('>QB', i * STEP_MS * 10000, len(title)) + title
    return struct.pack('>I4s', 8 + len(body), b'chpl') + body


def make_mp4v2(src, dst, n):
    data = bytearray(open(src, 'rb').read())
    moov, moov_size = find(data, [b'moov'])
    if moov + moov_size != len(data):
        raise ValueError('moov must be last (no faststart)')
    # the audio track's tref 'chap' -> 'free' (same size, nothing moves)
    for p, s, t in boxes(data, moov + 8, moov + moov_size):
        if t != b'trak':
            continue
        try:
            tp, _ = find(data, [b'tref', b'chap'], p + 8, p + s)
            data[tp + 4:tp + 8] = b'free'
        except KeyError:
            pass
    udta, udta_size = find(data, [b'moov', b'udta'])
    chpl, chpl_size = find(data, [b'chpl'], udta + 8, udta + udta_size)
    new = mp4v2_chpl(n)
    delta = len(new) - chpl_size
    data[chpl:chpl + chpl_size] = new
    struct.pack_into('>I', data, udta, udta_size + delta)
    struct.pack_into('>I', data, moov, moov_size + delta)
    open(dst, 'wb').write(data)


def main():
    aac = ['-c:a', 'aac', '-b:a', '8k', '-f', 'ipod']
    mp3 = ['-c:a', 'libmp3lame', '-b:a', '8k', '-id3v2_version', '3']
    with tempfile.TemporaryDirectory() as tmp:
        m648 = os.path.join(HERE, 'chapters_648.m4b')
        encode(tmp, 648, m648, aac)
        encode(tmp, 1100, os.path.join(HERE, 'chapters_1100.m4b'), aac)
        make_mp4v2(m648, os.path.join(HERE, 'chapters_648_mp4v2.m4b'), 648)
        encode(tmp, 300, os.path.join(HERE, 'chapters_300.mp3'), mp3)
        mp3v24 = mp3[:-1] + ['4']
        encode(tmp, 300, os.path.join(HERE, 'chapters_300_v24.mp3'), mp3v24)
        encode(tmp, 400, os.path.join(HERE, 'chapters_400_long.m4b'), aac,
               title_long)
        encode(tmp, 450, os.path.join(HERE, 'chapters_450_utf8.mp3'), mp3,
               title_utf8)
    print('chapter i (1-based) starts at %d*(i-1) ms, title "C<i>"'
          % STEP_MS)
    print('expected: 648, 1024 of 1100, 648, 300, 300, 400, 450 chapters')


if __name__ == '__main__':
    main()
