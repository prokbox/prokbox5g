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

"""A synthetic library in the layout Apple's sync software writes (#225).

    tests/perf/ipodctl_library.py <simdisk> [tracks] [--art PCT] [--art-first]

Writes <tracks> (default 5,400) tiny tagged MP3s (the frames of
tests/ui/fixtures/refresh.mp3 behind an ID3v2.3 tag, as
tests/perf/synthetic_library.py) to <simdisk>/iPod_Control/Music/F00..F49
under random four-letter names (ABCD.mp3), each track in a random folder,
so an album's tracks are scattered and a folder holds tracks/50 names.
No image files anywhere under Music, as on a synced player. Also makes the
empty iPod_Control/iTunes and iPod_Control/Artwork folders.

--art PCT    embed a cover (APIC, a 600x600 baseline JPEG drawn here from
             the album number: gradients and bars, our own pixels) in PCT %
             of the albums (default 30)
--art-first  embed it only in each such album's first track (keeps a 50k
             disk small; the art builder reads only that track)
--synced PCT give PCT % of the albums (default 70, drawn independently of
             --art, so some albums have both) a cover in a synthetic cover
             database + song database (iPod_Control/Artwork, iTunes/
             iTunesDB), written by tests/syncart's mkfixture (--mkfixture
             PATH, required when PCT > 0; #225 S3). Every track of such an
             album names the album's one stored cover.

Shape: 10 tracks per album, 4 albums per artist, 24 genres; fixed seed,
idempotent (a stamp file holds the arguments). Synthetic only: no real
music, names or artwork.
"""
import io
import os
import random
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from synthetic_library import (GENRES, audio_frames, frame,  # noqa: E402
                               name)


def cover(n):
    from PIL import Image, ImageDraw
    rng = random.Random(n)
    c0 = tuple(rng.randrange(256) for _ in range(3))
    c1 = tuple(rng.randrange(256) for _ in range(3))
    im = Image.new('RGB', (1, 2))
    im.putpixel((0, 0), c0)
    im.putpixel((0, 1), c1)
    im = im.resize((600, 600), Image.BILINEAR)
    d = ImageDraw.Draw(im)
    for _ in range(12):
        x, y = rng.randrange(600), rng.randrange(600)
        d.rectangle((x, y, x + rng.randrange(40, 200), y + rng.randrange(8, 60)),
                    fill=tuple(rng.randrange(256) for _ in range(3)))
    out = io.BytesIO()
    im.save(out, 'JPEG', quality=85, progressive=False)
    return out.getvalue()


def apic(jpeg):
    data = b'\x00' + b'image/jpeg\x00' + b'\x03' + b'\x00' + jpeg
    return b'APIC' + struct.pack('>I', len(data)) + b'\0\0' + data


def tag(tags, art):
    body = b''.join(frame(k, v) for k, v in tags if v) + (apic(art) if art else b'')
    n = len(body)
    size = bytes([(n >> 21) & 127, (n >> 14) & 127, (n >> 7) & 127, n & 127])
    return b'ID3\x03\x00\x00' + size + body


def main():
    import argparse
    ap = argparse.ArgumentParser(usage=__doc__)
    ap.add_argument('disk')
    ap.add_argument('tracks', nargs='?', type=int, default=5400)
    ap.add_argument('--art', type=int, default=30)
    ap.add_argument('--art-first', action='store_true')
    ap.add_argument('--synced', type=int, default=70)
    ap.add_argument('--mkfixture')
    # #225 tests: two albums that sort first ("0 Both": embedded art on
    # every track and a synced cover; "0 Synced": a synced cover only),
    # 3 tracks each, from their own random stream (the rest is unchanged)
    ap.add_argument('--front', action='store_true')
    # a re-sync: the same songs' cover database written again under this
    # root with mirrored pictures and new image ids (mkfixture --mirror)
    ap.add_argument('--resync-root')
    a = ap.parse_args()
    disk, total, pct, first_only = a.disk, a.tracks, a.art, a.art_first
    top = os.path.join(disk, 'iPod_Control')
    stamp = os.path.join(top, '.synthetic')
    key = '%d %d %d %d %d %s' % (total, pct, first_only, a.synced, a.front,
                                 a.resync_root)
    if os.path.exists(stamp) and open(stamp).read().strip() == key:
        print('ipodctl: %s already in %s' % (key, top))
        return
    for sub in ('iTunes', 'Artwork'):
        os.makedirs(os.path.join(top, sub), exist_ok=True)
    music = os.path.join(top, 'Music')
    for f in range(50):
        os.makedirs(os.path.join(music, 'F%02d' % f), exist_ok=True)
    rng = random.Random(225)
    audio = audio_frames()
    artists = [name(rng, rng.choice((1, 2, 2, 3))) for _ in range(max(1, total // 40))]
    used = set()
    srng = random.Random(2250)          # the synced covers: own stream, so
    synced_list = []                    # the layout above is unchanged
    n = album_no = with_art = with_syn = 0
    while n < total:
        album_no += 1
        artist = artists[(album_no * 7) % len(artists)]
        album = name(rng, rng.choice((1, 2, 3)))
        genre = rng.choice(GENRES)
        art = cover(album_no) if rng.randrange(100) < pct else None
        with_art += art is not None
        syn = srng.randrange(100) < a.synced
        with_syn += syn
        for t in range(1, 11):
            if n >= total:
                break
            n += 1
            while True:
                nm = ''.join(rng.choice('ABCDEFGHIJKLMNOPQRSTUVWXYZ')
                             for _ in range(4))
                rel = 'F%02d/%s.mp3' % (rng.randrange(50), nm)
                if rel.upper() not in used:
                    used.add(rel.upper())
                    break
            tags = [('TIT2', name(rng, rng.choice((1, 2, 3)))),
                    ('TPE1', artist), ('TALB', album), ('TPE2', artist),
                    ('TCON', genre), ('TRCK', '%d/10' % t), ('TPOS', '1/1')]
            emb = art if art and (t == 1 or not first_only) else None
            with open(os.path.join(music, rel), 'wb') as f:
                f.write(tag(tags, emb))
                f.write(audio)
            synced_list.append('/iPod_Control/Music/%s\t%s'
                               % (rel, ('a%d' % album_no) if syn else '-'))
    if a.front:
        frng = random.Random(2251)
        for title, emb_art in (('0 Both', True), ('0 Synced', False)):
            album_no += 1
            art = cover(album_no) if emb_art else None
            with_art += emb_art
            with_syn += 1
            for t in range(1, 4):
                n += 1
                while True:
                    nm = ''.join(frng.choice('ABCDEFGHIJKLMNOPQRSTUVWXYZ')
                                 for _ in range(4))
                    rel = 'F%02d/%s.mp3' % (frng.randrange(50), nm)
                    if rel.upper() not in used:
                        used.add(rel.upper())
                        break
                tags = [('TIT2', '%s %d' % (title, t)), ('TPE1', '0 Front'),
                        ('TALB', title), ('TPE2', '0 Front'),
                        ('TCON', 'Rock'), ('TRCK', '%d/3' % t),
                        ('TPOS', '1/1')]
                with open(os.path.join(music, rel), 'wb') as f:
                    f.write(tag(tags, art))
                    f.write(audio)
                synced_list.append('/iPod_Control/Music/%s\ta%d'
                                   % (rel, album_no))
    if a.synced > 0 or a.front:
        import subprocess
        if not a.mkfixture:
            sys.exit('ipodctl: --synced needs --mkfixture PATH')
        listing = ('\n'.join(synced_list) + '\n').encode()
        subprocess.run([a.mkfixture, disk], check=True, input=listing)
        if a.resync_root:
            subprocess.run([a.mkfixture, '--mirror', '--first-id', '5000',
                            a.resync_root], check=True, input=listing)
    with open(stamp, 'w') as f:
        f.write(key + '\n')
    print('ipodctl: %d tracks, %d albums (%d with embedded art, %d synced '
          'covers) in %s' % (n, album_no, with_art, with_syn, music))


if __name__ == '__main__':
    main()
