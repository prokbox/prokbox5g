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

"""#47 fixture: artist names that share a sort key but group apart, and
case variants that group together.

    tests/perf/artalb_fixture.py <simdisk>

Writes <simdisk>/ArtFix/<n>/<track>.mp3 (the tiny tagged MP3s of
synthetic_library.py). Expected Artists rows (ui_groupkey "elan" for the
first four: a run of four rows on one key):
  Elan      (Elan, ELAN)            Ashen Road, ashen road (one row: an
                                    artist's case variants), Coast
  The Elan                          Ashen Road (its own row and album)
  Élan      (Élan, élan, ÉLAN)      Dune, Ember
  Elán                              Frost
  Straße    (Straße, STRASSE)       Lied
  Ørjan     (Ørjan, ØRJAN, ørjan)   Nord
  Łukasz    (Łukasz, ŁUKASZ)        Wisła
  ǅemal     (Ǆemal, ǅemal, ǆemal)   Ext-B digraph case
  Ωmega     (Ωmega, ωMEGA)          Greek
  Север      (Север, СЕВЕР)            Cyrillic
plus "Various Artists" compilations whose track artists are the above (no
Artists row of their own: albums with no row, legitimately).

#230: composers and genres on the same tracks, for the composers' and
genres' album ranges (tests/perf/grpalb_check.sh): case and accent
variants of one name (one row: Dvořák, DVOŘÁK, dvořák; Rock, ROCK, rock),
names that share a sort key but group apart (Dvorak, The Dvořák), untagged
tracks (no row), and tracks of one album under different composers and
genres (a composer's album row then starts at its own first track, not
the album's). Composers cycle per track, genres per album. Then FILLER
albums of Elan's without a composer or a genre: the precompute builds the
ranges in its block's free room, about 20 B per track, which a library
of 30 tracks in 25 albums does not leave (it scans: cheap at that size).
"""
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from synthetic_library import id3, audio_frames   # noqa: E402

ALBUMS = [
    # (album artist, track artist, album, tracks)
    ('Elan', 'Elan', 'Ashen Road', 2),
    ('ELAN', 'ELAN', 'ashen road', 2),
    ('Elan', 'Elan', 'Coast', 1),
    ('The Elan', 'The Elan', 'Ashen Road', 2),
    ('Élan', 'Élan', 'Dune', 1),
    ('élan', 'élan', 'Dune', 1),
    ('ÉLAN', 'ÉLAN', 'Ember', 2),
    ('Elán', 'Elán', 'Frost', 2),
    ('Straße', 'Straße', 'Lied', 1),
    ('STRASSE', 'STRASSE', 'Lied', 1),
    ('Ørjan', 'Ørjan', 'Nord', 1),
    ('ØRJAN', 'ØRJAN', 'Nord', 1),
    ('ørjan', 'ørjan', 'Sør', 1),
    ('Łukasz', 'Łukasz', 'Wisła', 1),
    ('ŁUKASZ', 'ŁUKASZ', 'WISŁA', 1),
    ('Ǆemal', 'Ǆemal', 'Most', 1),
    ('ǅemal', 'ǅemal', 'Most', 1),
    ('ǆemal', 'ǆemal', 'Grad', 1),
    ('Ωmega', 'Ωmega', 'Alpha', 1),
    ('ωMEGA', 'ωMEGA', 'Beta', 1),
    ('Север', 'Север', 'Bereg', 1),
    ('СЕВЕР', 'СЕВЕР', 'Zvezda', 1),
    ('Various Artists', 'Elan', 'Mix One', 1),
    ('Various Artists', 'élan', 'Mix One', 1),
    ('Various Artists', 'Straße', 'Mix Two', 1),
]


COMPOSERS = ['Dvořák', 'DVOŘÁK', 'dvořák', 'Dvorak', 'The Dvořák', '',
             'Straße', 'STRASSE', 'Ωmega', 'ωMEGA', '']
GENRES = ['Rock', 'ROCK', 'rock', 'Röck', 'Jazz', 'The Rock', '']


FILLER = 10            # albums of 12 tracks


def main():
    if len(sys.argv) < 2:
        sys.exit(__doc__)
    top = os.path.join(sys.argv[1], 'ArtFix')
    audio = audio_frames()
    k = 0
    for n, (aa, ta, album, tracks) in enumerate(ALBUMS):
        folder = os.path.join(top, '%02d' % n)
        os.makedirs(folder, exist_ok=True)
        for t in range(1, tracks + 1):
            tags = [('TIT2', '%s %d' % (album, t)), ('TPE1', ta),
                    ('TALB', album), ('TPE2', aa),
                    ('TCON', GENRES[n % len(GENRES)]),
                    ('TCOM', COMPOSERS[k % len(COMPOSERS)]),
                    ('TRCK', str(t)), ('TPOS', '1')]
            k += 1
            if aa == 'Various Artists':
                tags.append(('TCMP', '1'))
            with open(os.path.join(folder, '%02d.mp3' % t), 'wb') as f:
                f.write(id3(tags))
                f.write(audio)
    for n in range(FILLER):
        folder = os.path.join(top, 'f%02d' % n)
        os.makedirs(folder, exist_ok=True)
        for t in range(1, 13):
            tags = [('TIT2', 'Filler %d %d' % (n, t)), ('TPE1', 'Elan'),
                    ('TALB', 'Filler %d' % n), ('TPE2', 'Elan'),
                    ('TRCK', str(t)), ('TPOS', '1')]
            with open(os.path.join(folder, '%02d.mp3' % t), 'wb') as f:
                f.write(id3(tags))
                f.write(audio)
    print('artalb fixture: %d albums in %s' % (len(ALBUMS) + FILLER, top))


if __name__ == '__main__':
    main()
