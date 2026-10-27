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

"""Generate a large synthetic library for library timing (Phase 4, F9).

    tests/perf/synthetic_library.py <simdisk> [tracks]

Writes <tracks> (default 50,000) tiny tagged MP3 files under
<simdisk>/Synth/<artist>/<album>/NN <title>.mp3: one second of audio (the
frames of tests/ui/fixtures/refresh.mp3) behind an ID3v2.3 tag with
title, artist, album, album artist, genre, composer, track and disc
numbers. Deterministic (fixed seed), idempotent (a stamp file holds the
track count), ~2.4 KB per file.

Shape, for 50k tracks: 10 tracks per album (5,000 albums), 1,250 artists
(4 albums each), 2 % of albums by "Various Artists" (compilations),
24 genres, 400 composers on half the tracks, 50 audiobooks (genre
Audiobook, 10 tracks each) and 20 podcasts (genre Podcast, 25 episodes),
titles from a word list so sort keys spread over A-Z and '#'.

Run it via tests/perf/lib50k.sh, which keeps it away from the test
library in build/sim-ui (its own simdisk, build/sim-50k).
"""
import os
import random
import struct
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(
    os.path.abspath(__file__))))
FIXTURE = os.path.join(ROOT, 'tests', 'ui', 'fixtures', 'refresh.mp3')

WORDS = ('amber anchor apple arrow aspen autumn basalt beacon birch bloom '
         'bright cedar cinder clover cobalt comet copper coral crimson '
         'crystal dawn delta drift dune echo ember emerald falcon faded '
         'feather ferry fjord forest frost garnet ghost glacier golden '
         'granite harbour hazel hollow horizon indigo iron island ivory '
         'jade jagged juniper kestrel lantern lunar maple marble meadow '
         'mellow mirror misty nectar noble north oasis ochre olive onyx '
         'opal orbit pale pebble pine quartz quiet raven river rust saffron '
         'sable signal silver slate solar spruce static summit tide timber '
         'topaz upper velvet violet willow winter xenon yarrow yellow zephyr '
         'zero 1999 42 7th').split()
GENRES = ('Ambient Blues Classical Country Dance Disco Electronic Folk Funk '
          'Gospel Hip-Hop House Indie Jazz Latin Metal Pop Punk Reggae Rock '
          'Soul Techno Trance World').split()


def name(rng, n):
    return ' '.join(rng.choice(WORDS).capitalize() for _ in range(n))


def frame(fid, text):
    data = b'\x03' + text.encode('utf-8')      # UTF-8
    return fid.encode() + struct.pack('>I', len(data)) + b'\0\0' + data


def id3(tags):
    body = b''.join(frame(k, v) for k, v in tags if v)
    n = len(body)
    size = bytes([(n >> 21) & 127, (n >> 14) & 127, (n >> 7) & 127, n & 127])
    return b'ID3\x03\x00\x00' + size + body


def audio_frames():
    d = open(FIXTURE, 'rb').read()
    if d[:3] == b'ID3':
        n = (d[6] << 21) | (d[7] << 14) | (d[8] << 7) | d[9]
        d = d[10 + n:]
    return d


def main():
    if len(sys.argv) < 2:
        sys.exit(__doc__)
    disk = sys.argv[1]
    total = int(sys.argv[2]) if len(sys.argv) > 2 else 50000
    top = os.path.join(disk, 'Synth')
    stamp = os.path.join(top, '.synthetic')
    if os.path.exists(stamp) and open(stamp).read().strip() == str(total):
        print('synthetic: %d tracks already in %s' % (total, top))
        return
    rng = random.Random(4)
    audio = audio_frames()
    artists = [name(rng, rng.choice((1, 2, 2, 3))) for _ in range(total // 40)]
    composers = [name(rng, 2) for _ in range(400)]
    n = 0
    album_no = 0
    while n < total:
        album_no += 1
        kind = ('book' if album_no % 100 == 1 else
                'podcast' if album_no % 250 == 2 else
                'various' if album_no % 50 == 3 else 'music')
        tracks = 25 if kind == 'podcast' else 10
        artist = artists[(album_no * 7) % len(artists)]
        album = name(rng, rng.choice((1, 2, 3)))
        genre = (rng.choice(GENRES) if kind in ('music', 'various') else
                 'Audiobook' if kind == 'book' else 'Podcast')
        albumartist = 'Various Artists' if kind == 'various' else artist
        folder = os.path.join(top, albumartist.replace('/', '_'),
                              '%s %05d' % (album, album_no))
        os.makedirs(folder, exist_ok=True)
        for t in range(1, tracks + 1):
            if n >= total:
                break
            n += 1
            title = name(rng, rng.choice((1, 2, 3)))
            tags = [('TIT2', title),
                    ('TPE1', name(rng, 2) if kind == 'various' else artist),
                    ('TALB', album), ('TPE2', albumartist),
                    ('TCON', genre), ('TRCK', str(t)), ('TPOS', '1'),
                    ('TCOM', rng.choice(composers) if n % 2 else '')]
            path = os.path.join(folder, '%02d %s.mp3' % (t, title))
            with open(path, 'wb') as f:
                f.write(id3(tags))
                f.write(audio)
    with open(stamp, 'w') as f:
        f.write('%d\n' % total)
    print('synthetic: %d tracks, %d albums in %s' % (n, album_no, top))


if __name__ == '__main__':
    main()
