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

"""Phase 6 (Audiobooks) test fixtures, generated under build/ab-fixtures/.

    tests/fixtures/ab/make_ab_fixtures.py [outdir]   (default build/ab-fixtures)

Idempotent (a stamp file holds VERSION).  Needs ffmpeg with libmp3lame.
Scripts copy them into the sim disk with the `copy` command, then run
`update_db_wait_change`, e.g.
    mkdir /Audiobooks/AB
    copy ../../build/ab-fixtures/pref.m4b /Audiobooks/AB/pref.m4b

  pref.m4b      "AB Preface Book", 90 s, chapters Preface / Chapter 1 / 2 / 3
                (entry 2 is "Chapter 1": a Preface before Chapter 1)
  nochap.m4b    "AB No Chapters", 60 s, no chapters
  short.m4b     "AB Short Book", 40 s with 2 chapters (finished rule)
  multi/        "AB Multi", 4 MP3 files of 25 s (genre Audiobook, track
                01..04); files 1 and 3 carry ID3v2.3 CHAP frames, 2 and 4 none
  ffctoc.mp3    "AB FFmpeg CTOC", 60 s, ID3v2.4 written by ffmpeg: 4 CHAP +
                CHAP frames and a CTOC whose sizes are not synchsafe (#50)
  many.m4b      "AB Many Chapters", 1100 chapters of 1 s; titles are 23 B of
                3-byte characters
  utf8.m4b      "AB Utf8 Cut", 3 chapters, long 2/3/4-byte titles (> 63 B,
                so the cut must land on a UTF-8 boundary)
  sort/         #59: "Zebra Crossing" and "Middle March" by Ada Quill (album
                artist), "Apple Orchard" by Zed Marlowe (artist only), 8 s
  store_full.dat  a full books.dat (2,048 records, the 2 oldest FINISHED)
Original sine tones only, no third-party art.
"""
import os, shutil, subprocess, sys

VERSION = 6
ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), '..', '..', '..'))
OUT = os.path.abspath(sys.argv[1] if len(sys.argv) > 1 else
                      os.path.join(ROOT, 'build', 'ab-fixtures'))
if not OUT.startswith(os.path.join(ROOT, 'build') + os.sep):
    sys.exit('make_ab_fixtures: refusing to write outside build/: ' + OUT)
stamp = os.path.join(OUT, '.stamp')
if os.path.exists(stamp) and open(stamp).read().strip() == str(VERSION):
    sys.exit(0)
# Generate aside, then swap in, so parallel gate workers never see (or
# write into) a half-built directory.
FINAL = OUT
OUT = '%s.tmp%d' % (FINAL, os.getpid())
shutil.rmtree(OUT, ignore_errors=True)
os.makedirs(os.path.join(OUT, 'multi'))
os.makedirs(os.path.join(OUT, 'multi_acc'))
os.makedirs(os.path.join(OUT, 'sort'))


def ff(*a):
    subprocess.run(['ffmpeg', '-v', 'error', '-y', *a], check=True)


def meta_file(name, title, chapters, album=None, genre='Audiobook',
              artist='AB Author', album_artist='AB Author'):
    """chapters: [(title, start_ms, end_ms)]"""
    p = os.path.join(OUT, name.replace('/', '_') + '.ffmeta')
    L = [';FFMETADATA1', 'title=%s' % title, 'artist=%s' % artist]
    if album_artist:
        L += ['album_artist=%s' % album_artist]
    L += ['album=%s' % (album or title), 'genre=%s' % genre, 'date=2026']
    for t, a, b in chapters:
        L += ['[CHAPTER]', 'TIMEBASE=1/1000', 'START=%d' % a, 'END=%d' % b,
              'title=%s' % t]
    with open(p, 'w', encoding='utf-8') as f:
        f.write('\n'.join(L) + '\n')
    return p


def encode(out, meta, chapters, secs, hz, sr=22050, mp3=False, v24=False,
           extra=()):
    cmd = ['-f', 'lavfi', '-i', 'sine=frequency=%d:sample_rate=%d:duration=%g'
           % (hz, sr, secs), '-i', meta, '-map', '0:a', '-map_metadata', '1',
           '-map_chapters', '1' if chapters else '-1', '-ac', '1']
    if mp3:
        cmd += ['-c:a', 'libmp3lame', '-b:a', '32k',
                '-id3v2_version', '4' if v24 else '3', '-write_id3v1', '0']
        cmd += list(extra) + ['-bitexact', out]
    else:
        cmd += ['-c:a', 'aac', '-b:a', '16k', '-bitexact', '-brand', 'M4B ',
                '-f', 'ipod', out]
    ff(*cmd)


def P(n):
    return os.path.join(OUT, n)


def book(name, title, secs, chapters, hz, **kw):
    encode(P(name), meta_file(name, title, chapters), chapters, secs, hz, **kw)


book('pref.m4b', 'AB Preface Book', 90,
     [('Preface', 0, 15000), ('Chapter 1', 15000, 45000),
      ('Chapter 2', 45000, 70000), ('Chapter 3', 70000, 90000)], 330)
book('nochap.m4b', 'AB No Chapters', 60, [], 350)
book('short.m4b', 'AB Short Book', 40,
     [('Opening', 0, 20000), ('Closing', 20000, 40000)], 370)

# multi-file MP3 book: 4 x 25 s, CHAP in files 1 and 3 (ID3v2.3)
for i in range(1, 5):
    ch = []
    if i in (1, 3):
        ch = [('Part %d.1' % i, 0, 12000), ('Part %d.2' % i, 12000, 25000)]
    m = meta_file('multi%d' % i, 'AB Multi %d' % i, ch, album='AB Multi')
    encode(P('multi/%02d.mp3' % i), m, ch, 25, 300 + 25 * i, mp3=True,
           extra=['-metadata', 'track=%d/4' % i])

# #47: the same book with accented capitals in album and author (its art
# key changed with the shared case folding): "ÉTÉ Multi" by "ÉMILE Auteur"
for i in range(1, 5):
    ch = []
    if i in (1, 3):
        ch = [('Part %d.1' % i, 0, 12000), ('Part %d.2' % i, 12000, 25000)]
    m = meta_file('multi_acc%d' % i, '\u00c9T\u00c9 Multi %d' % i, ch,
                  album='\u00c9T\u00c9 Multi', artist='\u00c9MILE Auteur',
                  album_artist='\u00c9MILE Auteur')
    encode(P('multi_acc/%02d.mp3' % i), m, ch, 25, 400 + 25 * i, mp3=True,
           extra=['-metadata', 'track=%d/4' % i])

# ffmpeg ID3v2.4 + CTOC (#50)
# (150-char titles: every CHAP frame is > 127 B, so its plain big-endian
# size has a high bit in the low byte and is not a valid synchsafe size)
book('ffctoc.mp3', 'AB FFmpeg CTOC', 60,
     [('Intro ' + 'a' * 144, 0, 10000), ('Part A ' + 'b' * 143, 10000, 30000),
      ('Part B ' + 'c' * 143, 30000, 45000),
      ('Part C ' + 'd' * 143, 45000, 60000)], 390, mp3=True, v24=True)

# 1,100 chapters; titles of 4 digits + 19 three-byte characters
many = [(('%04d' % i) + '日' * 19, i * 1000, (i + 1) * 1000)
        for i in range(1100)]
book('many.m4b', 'AB Many Chapters', 1100, many, 410, sr=8000)

book('utf8.m4b', 'AB Utf8 Cut', 30,
     [('é' * 40, 0, 10000), ('日本' * 30, 10000, 20000),
      ('\U0001f3a7' * 20, 20000, 30000)], 430)

# #59 Sort By: three books whose title order and author order differ;
# one author with two books (ties by title), one book with an artist but
# no album artist (the author falls back to the artist)
#   title order:  Apple Orchard, Middle March, Zebra Crossing
#   author order: Ada Quill (Middle March, Zebra Crossing), Zed Marlowe
for name, title, artist, aa, hz in (
        ('sort/zebra.m4b', 'Zebra Crossing', 'Ada Quill', 'Ada Quill', 450),
        ('sort/apple.m4b', 'Apple Orchard', 'Zed Marlowe', None, 470),
        ('sort/middle.m4b', 'Middle March', 'Ada Quill', 'Ada Quill', 490)):
    encode(P(name), meta_file(name, title, [], artist=artist,
                              album_artist=aa), [], 8, hz)

# a full books.dat (2,048 records, the 2 oldest FINISHED) for the eviction
# script, sealed with the real ab_store_core.c; keys 0x00a00000 + 3*i, so
# the evictions name 00a00000, 00a00003 (FINISHED) then 00a00006 (STARTED)
mk = os.path.join(OUT, 'mkstore')
subprocess.run(['cc', '-std=gnu99', '-DWARBLE', '-I' + ROOT + '/apps/ipodui',
                '-I' + ROOT + '/lib/rbcodec/metadata', '-o', mk,
                ROOT + '/tests/fixtures/ab/mkstore.c',
                ROOT + '/apps/ipodui/ab_store_core.c',
                ROOT + '/apps/ipodui/ab_book_core.c',
                ROOT + '/lib/rbcodec/metadata/abpack.c'], check=True)
subprocess.run([mk, P('store_full.dat'), '2048', '2'], check=True,
               stdout=subprocess.DEVNULL)
os.remove(mk)

with open(os.path.join(OUT, '.stamp'), 'w') as f:
    f.write(str(VERSION))
try:
    if os.path.exists(FINAL):
        shutil.rmtree(FINAL, ignore_errors=True)
    os.rename(OUT, FINAL)
except OSError:                 # another worker won the race
    shutil.rmtree(OUT, ignore_errors=True)
