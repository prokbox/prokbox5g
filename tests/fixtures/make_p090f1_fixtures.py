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
"""0.9.0 package F1 fixtures (tests/fixtures/f1/): sort tags (#235) and the
unplayable reason (#286). Needs ffmpeg (9.x; libx264, aac, ac3, flac).

  sort_v23.mp3   ID3v2.3, UTF-16: TPE1/TSOP, TALB/TSOA, TPE2/TSO2 (CJK)
  sort_txxx.mp3  ID3v2.4: TXXX ARTISTSORT before TSOP (the frame wins),
                 TXXX ALBUMSORT and ALBUM ARTIST SORT alone (the aliases)
  sort.flac      Vorbis ARTISTSORT, ALBUMSORT, ALBUMARTISTSORT
  sort.m4a       MP4 soar, soal, soaa
  plain.m4a      AAC, no sort tags: plays (reason 0)
  drm.m4p        plain.m4a with its sample entry renamed drms (reason 1)
  book.aax       plain.m4a renamed aavd, as .aax (reason 2)
  book.aa        a synthetic Audible .aa header and dictionary (reason 2)
  ac3.m4a        AC-3 in MP4: a codec we lack (reason 3)
  base.mp4       H.264 Baseline video: plays (0)
  high.mp4       H.264 High video: the player refuses it (3)
  w32_96.flac    32-bit 96 kHz stereo: too wide for the rate (3, #318)
  w32_48.flac    32-bit 48 kHz stereo: plays (0)
  w24_96.flac    24-bit 96 kHz stereo: plays (0)
"""
import os
import struct
import subprocess
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
OUT = os.path.join(HERE, 'f1')
FF = ['ffmpeg', '-nostdin', '-v', 'error', '-y']
BX = ['-fflags', '+bitexact', '-flags:a', '+bitexact']
SINE = ['-f', 'lavfi', '-i', 'sine=frequency=440:duration=2:sample_rate=%d']


def ff(args):
    subprocess.run(FF + args, check=True)


def sine(rate=8000):
    a = list(SINE)
    a[3] = a[3] % rate
    return a


def syncsafe(n):
    return bytes([(n >> 21) & 127, (n >> 14) & 127, (n >> 7) & 127, n & 127])


def frame23(fid, text):
    data = b'\x01' + '﻿'.encode('utf-16-le') + text.encode('utf-16-le') + b'\0\0'
    return fid.encode() + struct.pack('>I', len(data)) + b'\0\0' + data


def frame24(fid, data):
    return fid.encode() + syncsafe(len(data)) + b'\0\0' + data


def t24(text):
    return b'\x03' + text.encode('utf-8')


def txxx24(desc, value):
    return b'\x03' + desc.encode() + b'\0' + value.encode('utf-8')


def mp3_with_tag(name, version, frames):
    with tempfile.TemporaryDirectory() as tmp:
        raw = os.path.join(tmp, 'a.mp3')
        ff(sine() + ['-ac', '1', '-c:a', 'libmp3lame', '-b:a', '32k',
                     '-map_metadata', '-1', '-id3v2_version', '0',
                     '-write_xing', '0'] + BX + [raw])
        audio = open(raw, 'rb').read()
    body = b''.join(frames)
    head = b'ID3' + bytes([version, 0, 0]) + syncsafe(len(body))
    open(os.path.join(OUT, name), 'wb').write(head + body + audio)


def rename_entry(src, dst, old, new):
    b = open(src, 'rb').read()
    i = b.index(b'stsd')
    j = b.index(old, i)
    assert j - i < 64
    open(dst, 'wb').write(b[:j] + new + b[j + 4:])


def add_ilst(path, items):
    """Append iTunes string atoms (fourcc, text) to the ilst of an MP4 whose
    moov follows its mdat (ffmpeg's default), fixing the sizes of ilst and
    of the boxes around it; no chunk offset moves. ffmpeg 9.0.2 does not
    write soar/soal/soaa."""
    b = bytearray(open(path, 'rb').read())
    add = b''
    for fourcc, text in items:
        data = b'data' + struct.pack('>II', 1, 0) + text.encode('utf-8')
        data = struct.pack('>I', 4 + len(data)) + data
        add += struct.pack('>I', 8 + len(data)) + fourcc.encode() + data
    il = b.index(b'ilst') - 4
    assert b.index(b'moov') > b.index(b'mdat')
    end = il + struct.unpack('>I', b[il:il + 4])[0]
    for box in (b'moov', b'udta', b'meta', b'ilst'):
        at = b.index(box) - 4
        assert at <= il and at + struct.unpack('>I', b[at:at + 4])[0] >= end
        b[at:at + 4] = struct.pack('>I', struct.unpack('>I', b[at:at + 4])[0] + len(add))
    b[end:end] = add
    open(path, 'wb').write(bytes(b))


def aa_file(path):
    """Audible .aa as FFmpeg's aadec.c reads it: size, magic, TOC count,
    an unknown word, TOC entries (index, offset, size), a 24-byte
    terminator, then the dictionary (1 byte, key length, value length,
    key, value)."""
    toc = [(0, 0, 0), (1, 0, 0)]
    dic = [('title', 'Protected Book'), ('author', 'Some Author'),
           ('codec', 'mp332')]
    body = struct.pack('>I', len(toc)) + b'\0\0\0\0'
    for e in toc:
        body += struct.pack('>III', *e)
    body += b'\0' * 24 + struct.pack('>I', len(dic))
    for k, v in dic:
        body += b'\0' + struct.pack('>II', len(k), len(v)) + k.encode() + v.encode()
    body += b'\0' * 64
    data = struct.pack('>I', 8 + len(body)) + struct.pack('>I', 0x57907536) + body
    open(path, 'wb').write(data)


def main():
    os.makedirs(OUT, exist_ok=True)
    o = lambda n: os.path.join(OUT, n)
    # ID3v2.3, as iTunes and MusicBee write it (v2.4 IDs, UTF-16)
    mp3_with_tag('sort_v23.mp3', 3, [
        frame23('TIT2', 'First Love'),
        frame23('TPE1', '宇多田ヒカル'), frame23('TSOP', 'うただひかる'),
        frame23('TALB', '初恋'), frame23('TSOA', 'はつこい'),
        frame23('TPE2', '宇多田ヒカル'), frame23('TSO2', 'うただひかる')])
    mp3_with_tag('sort_txxx.mp3', 4, [
        frame24('TIT2', t24('Alias')),
        frame24('TPE1', t24('The Beatles')),
        frame24('TXXX', txxx24('ARTISTSORT', 'Alias Loses')),
        frame24('TSOP', t24('Beatles, The')),
        frame24('TALB', t24('Abbey Road')),
        frame24('TXXX', txxx24('ALBUMSORT', 'Abbey Road Alias')),
        frame24('TXXX', txxx24('ALBUM ARTIST SORT', 'Beatles Alias'))])
    ff(sine() + ['-ac', '1', '-c:a', 'flac', '-map_metadata', '-1',
                 '-metadata', 'title=Flac Sort', '-metadata', 'artist=椎名林檎',
                 '-metadata', 'album=無罪モラトリアム',
                 '-metadata', 'album_artist=椎名林檎'] + BX + [o('tmp.flac')])
    ff(['-i', o('tmp.flac'), '-c', 'copy', '-map_metadata', '0',
        '-metadata', 'ARTISTSORT=しいなりんご',
        '-metadata', 'ALBUMSORT=むざいもらとりあむ',
        '-metadata', 'ALBUMARTISTSORT=しいなりんご'] + BX + [o('sort.flac')])
    os.remove(o('tmp.flac'))
    ff(sine() + ['-ac', '1', '-c:a', 'aac', '-b:a', '24k', '-map_metadata', '-1',
                 '-metadata', 'title=M4a Sort', '-metadata', 'artist=東京事変',
                 '-metadata', 'album=教育', '-metadata', 'album_artist=東京事変']
       + BX + [o('sort.m4a')])
    add_ilst(o('sort.m4a'), [('soar', 'とうきょうじへん'), ('soal', 'きょういく'),
                             ('soaa', 'とうきょうじへん')])
    ff(sine() + ['-ac', '1', '-c:a', 'aac', '-b:a', '24k', '-map_metadata', '-1',
                 '-metadata', 'title=Plain', '-metadata', 'artist=Plain Artist']
       + BX + [o('plain.m4a')])
    rename_entry(o('plain.m4a'), o('drm.m4p'), b'mp4a', b'drms')
    rename_entry(o('plain.m4a'), o('book.aax'), b'mp4a', b'aavd')
    aa_file(o('book.aa'))
    ff(sine(48000) + ['-ac', '2', '-c:a', 'ac3', '-b:a', '96k', '-map_metadata', '-1',
                      '-metadata', 'title=AC3'] + BX + [o('ac3.m4a')])
    for name, prof in (('base.mp4', 'baseline'), ('high.mp4', 'high')):
        ff(['-f', 'lavfi', '-i', 'testsrc=size=64x48:rate=10:duration=1']
           + sine(48000) + ['-shortest', '-c:v', 'libx264', '-profile:v', prof,
                            '-pix_fmt', 'yuv420p', '-c:a', 'aac', '-b:a', '24k',
                            '-map_metadata', '-1', '-metadata', 'title=' + name]
           + ['-fflags', '+bitexact'] + [o(name)])
    for name, fmt, rate in (('w32_96.flac', 's32', 96000),
                            ('w32_48.flac', 's32', 48000),
                            ('w24_96.flac', 's32', 96000)):
        bits = '24' if name.startswith('w24') else '32'
        ff(sine(rate) + ['-ac', '2', '-sample_fmt', fmt, '-bits_per_raw_sample', bits,
                         '-c:a', 'flac', '-strict', 'experimental',
                         '-map_metadata', '-1', '-metadata', 'title=' + name]
           + BX + [o(name)])


if __name__ == '__main__':
    main()
