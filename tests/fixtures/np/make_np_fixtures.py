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

"""Phase 5 (Now Playing) test fixtures, generated under build/np-fixtures/.

    tests/fixtures/np/make_np_fixtures.py [outdir]     (default build/np-fixtures)

Idempotent (a stamp file holds VERSION).  Needs ffmpeg with libmp3lame.
Scripts copy them into the sim disk with the `copy` command, e.g.
    copy ../../build/np-fixtures/lyr23.mp3 /NPTest/lyr23.mp3

  long_title.mp3   30 s, an 90-char title (marquee), embedded square cover
  lyr23.mp3        30 s, ID3v2.3 USLT lyrics (24 lines)
  lyr24.mp3        30 s, ID3v2.4 USLT lyrics
  lyr.m4a          30 s, MP4 (c)lyr lyrics
  art_169.mp3      16:9 cover (bundle 142x80), album "NP Wide"
  art_23.mp3       2:3 cover (95x142), album "NP Tall"
  art_41.mp3       4:1 cover (142x36), album "NP Banner"
  art_big.mp3      1000x1000 detailed square cover (lines, digits), album
                   "NP Big" (main-menu Now Playing art sharpness)
  art_small.mp3    160x160 detailed square cover, album "NP Small" (a source
                   smaller than the panel: upscaled once, by the builder)
  book_23.mp3      20 s audiobook (genre Audiobook), 2:3 cover, album
                   "Tall Cover Book" (P6: fitted list thumbnail)
  book_169.mp3     the same with a 16:9 cover, album "Wide Cover Book"
  nolen.flac       40 s FLAC with total_samples 0 (length unknown)
  book_held.mp3    the same with its own title (a book whose chapter art no earlier script built)
  book_chart.mp3   90 s audiobook (genre Audiobook), ID3 CHAP frames with APIC
                   images: chapters 1 and 2 share one image, 3 has its own
Original gradients only (ffmpeg lavfi), no third-party art.
"""
import os, struct, subprocess, sys

VERSION = 6
ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), '..', '..', '..'))
OUT = os.path.abspath(sys.argv[1] if len(sys.argv) > 1 else
                      os.path.join(ROOT, 'build', 'np-fixtures'))
if not OUT.startswith(os.path.join(ROOT, 'build') + os.sep):
    sys.exit('make_np_fixtures: refusing to write outside build/: ' + OUT)
os.makedirs(OUT, exist_ok=True)
stamp = os.path.join(OUT, '.stamp')
if os.path.exists(stamp) and open(stamp).read().strip() == str(VERSION):
    sys.exit(0)

def ff(*a):
    subprocess.run(['ffmpeg', '-v', 'error', '-y', *a], check=True)

def cover(name, w, h, seed):
    p = os.path.join(OUT, name)
    ff('-f', 'lavfi', '-i',
       'gradients=s=%dx%d:c0=0x%06x:c1=0x%06x:c2=0x%06x:n=3:d=1:seed=%d'
       % (w, h, seed * 0x1f3a5 % 0xffffff, seed * 0x7c2b1 % 0xffffff,
          seed * 0x3e91d % 0xffffff, seed), '-frames:v', '1', '-q:v', '3', p)
    return p

def tone(name, secs, hz, meta, img=None, extra=()):
    p = os.path.join(OUT, name)
    cmd = ['-f', 'lavfi', '-i', 'sine=frequency=%d:duration=%d' % (hz, secs)]
    if img:
        cmd += ['-i', img, '-map', '0:a', '-map', '1:v', '-c:v', 'copy',
                '-disposition:v', 'attached_pic']
    cmd += ['-ac', '1', '-ar', '22050', '-b:a', '32k']
    for k, v in meta.items():
        cmd += ['-metadata', '%s=%s' % (k, v)]
    ff(*cmd, *extra, p)
    return p

LYRICS = '\n'.join('Line %02d of the test lyrics, long enough to wrap around '
                   'the screen at least once' % i for i in range(1, 25))

sq = cover('cover_sq.jpg', 300, 300, 3)
tone('long_title.mp3', 30, 330,
     {'title': 'A Remarkably Long Track Title That Has To Scroll Across The Screen Twice',
      'artist': 'Marquee Artist', 'album': 'NP Marquee'}, sq)
def synch(n):
    return bytes([(n >> 21) & 127, (n >> 14) & 127, (n >> 7) & 127, n & 127])

def strip_id3(raw):
    if raw[:3] == b'ID3':
        n = raw[6:10]
        raw = raw[10 + ((n[0] << 21) | (n[1] << 14) | (n[2] << 7) | n[3]):]
    return raw

def frame_v(ver, fid, body):
    size = struct.pack('>I', len(body)) if ver == 3 else synch(len(body))
    return fid.encode() + size + b'\0\0' + body

# ID3v2.3 / v2.4 USLT by hand (ffmpeg's "lyrics" key writes a TXXX, not USLT)
for n, ver in (('lyr23.mp3', 3), ('lyr24.mp3', 4)):
    base = tone('lyr_base.mp3', 30, 262, {'title': 'Lyrics %d' % ver})
    enc = b'\0' if ver == 3 else b'\3'          # latin-1 / UTF-8
    body = (frame_v(ver, 'TIT2', enc + b'Lyrics %d' % ver) +
            frame_v(ver, 'TPE1', enc + b'NP Lyrics') +
            frame_v(ver, 'TALB', enc + b'NP Lyrics') +
            frame_v(ver, 'USLT', enc + b'eng\0' +
                    LYRICS.replace('\n', '\r\n' if ver == 3 else '\n')
                    .encode()))
    raw = strip_id3(open(base, 'rb').read())
    open(os.path.join(OUT, n), 'wb').write(
        b'ID3' + bytes([ver, 0, 0]) + synch(len(body)) + body + raw)
    os.remove(base)
tone('lyr.m4a', 30, 262, {'title': 'Lyrics m4a', 'artist': 'NP Lyrics',
                          'album': 'NP Lyrics', 'lyrics': LYRICS})
for n, w, h, alb, seed in (('art_169.mp3', 640, 360, 'NP Wide', 5),
                           ('art_23.mp3', 400, 600, 'NP Tall', 7),
                           ('art_41.mp3', 800, 200, 'NP Banner', 11)):
    tone(n, 20, 300 + seed, {'title': alb + ' Song', 'artist': 'NP Art',
                             'album': alb}, cover('c_' + n + '.jpg', w, h, seed))
def detail(name, w, h):
    # lavfi's test pattern: fine lines and digits show any blur or blocks
    p = os.path.join(OUT, name)
    ff('-f', 'lavfi', '-i', 'testsrc2=s=%dx%d' % (w, h), '-frames:v', '1',
       '-q:v', '2', p)
    return p
for n, w, alb in (('art_big.mp3', 1000, 'NP Big'),
                  ('art_small.mp3', 160, 'NP Small')):
    tone(n, 20, 350 + w % 97, {'title': alb + ' Song', 'artist': 'NP Art',
                               'album': alb}, detail('c_' + n + '.jpg', w, w))
for n, w, h, alb, seed in (('book_23.mp3', 400, 600, 'Tall Cover Book', 13),
                           ('book_169.mp3', 640, 360, 'Wide Cover Book', 17)):
    tone(n, 20, 240 + seed, {'title': alb, 'artist': 'NP Author',
                             'album': alb, 'genre': 'Audiobook'},
         cover('c_' + n + '.jpg', w, h, seed))

# audiobook with CHAP/APIC frames: ID3v2.3 tag by hand
def frame(fid, body):
    return fid.encode() + struct.pack('>I', len(body)) + b'\0\0' + body

def apic(path):
    return frame('APIC', b'\0image/jpeg\0\3\0' + open(path, 'rb').read())

img_a = cover('ch_a.jpg', 300, 300, 21)
img_b = cover('ch_b.jpg', 400, 225, 22)
chaps = [(0, 30000, 'One', img_a), (30000, 60000, 'Two', img_a),
         (60000, 90000, 'Three', img_b)]

def synch(n):
    return bytes([(n >> 21) & 127, (n >> 14) & 127, (n >> 7) & 127, n & 127])

def book(fname, title):
    """A 90 s book with CHAP/APIC chapter frames. A second title gives a
    book with its own art keys: a script that needs a book whose chapter
    art is not built yet (chapart-held) uses its own, so what another
    script built earlier on the same disk cannot change it."""
    base = tone('book_base.mp3', 90, 220, {'title': title,
                'artist': 'NP Author', 'album': title, 'genre': 'Audiobook'})
    body = b''
    for i, (s, e, t, im) in enumerate(chaps):
        sub = frame('TIT2', b'\0' + t.encode() + b'\0') + apic(im)
        body += frame('CHAP', b'ch%d\0' % i + struct.pack('>IIII', s, e,
                      0xffffffff, 0xffffffff) + sub)
    body += frame('CTOC', b'toc\0\3\3ch0\0ch1\0ch2\0')
    body += frame('TCON', b'\0Audiobook\0') + frame('TIT2', b'\0' + title.encode() + b'\0')
    body += frame('TPE1', b'\0NP Author\0') + frame('TALB', b'\0' + title.encode() + b'\0')
    raw = open(base, 'rb').read()
    if raw[:3] == b'ID3':          # drop ffmpeg's own tag
        n = raw[6:10]
        sz = (n[0] << 21) | (n[1] << 14) | (n[2] << 7) | n[3]
        raw = raw[10 + sz:]
    open(os.path.join(OUT, fname), 'wb').write(
        b'ID3\3\0\0' + synch(len(body)) + body + raw)
    os.remove(base)

book('book_chart.mp3', 'Chapter Art Book')
book('book_held.mp3', 'Chapter Held Book')
# FLAC streamed to a pipe: STREAMINFO carries total_samples 0, so the
# length is unknown (np snap len=0)
d = subprocess.run(['ffmpeg', '-v', 'error', '-y', '-f', 'lavfi', '-i',
                    'sine=frequency=196:duration=40', '-ac', '1', '-ar',
                    '22050', '-c:a', 'flac', '-f', 'flac', 'pipe:1'],
                   check=True, stdout=subprocess.PIPE).stdout
open(os.path.join(OUT, 'nolen.flac'), 'wb').write(d)
open(stamp, 'w').write(str(VERSION))
