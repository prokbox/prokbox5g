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

"""Phase 4 test library for the simulator (run through setup_library.sh).

    setup_library.py --generate <simdisk> <tmpdir>   write the library
    setup_library.py --config <simdisk>              only the config keys
    setup_library.py --stamp                         "<VERSION> <files>"
    setup_library.py --manifest                      non-music manifest
    setup_library.py --art-manifest                  expected art state

Layout (all paths relative to the simdisk root):

    Library/<Artist>/<Album>/NN <Title>.mp3
        Music: 47 artists, 59 albums, 8 genres, 6 composers, 2-5 s each.
    Library/Compilations/Summer Tones Vol. 1/NN <Title>.mp3
        The compilation: album artist "Various Artists", compilation flag
        (ID3 TCMP=1); each track's artist is one of the library artists.
    Podcasts/<Show>/<Episode>.mp3
        2 shows x 2 episodes, genre "Podcast", under /Podcasts/.
    Audiobooks/<Author>/<Book>.m4b
        3 books, AAC in an M4B ("-f ipod", brand M4B) with 3-5 chapters
        (Nero chpl + QuickTime). "The Lighthouse Keeper" and "A Field Guide
        to Tones" have genre Audiobook; "Winter Stories" has genre "Spoken
        Word", so only the .m4b / M4B brand / chapter rule classifies it.
    Library/Untagged Session/NN.mp3
        2 tracks with no album tag (a directory-grouped "album" for the
        untagged-folder art/grouping case, docs/phase4b-brief.md
        Revision 1 point 7). They do carry an existing artist ("Riverine")
        and titles that sort mid-list ("Field Recording ..."), so the
        Phase 2-4 golden views (Artists ends, Songs top and end) stay as
        the owner approved them: no "Unknown Artist" row, no "0N ..."
        titles in the '#' group.
    Podcasts/<Show>/<Episode>.mp3
        2 shows x 2 episodes, genre "Podcast", under /Podcasts/. "Signal
        Hour" has embedded show art (its episodes carry the same cover);
        "Tone Talk Weekly" has none.
    Audiobooks/<Author>/<Book>.m4b
        3 books, AAC in an M4B ("-f ipod", brand M4B) with 3-5 chapters
        (Nero chpl + QuickTime). "The Lighthouse Keeper" and "A Field Guide
        to Tones" have genre Audiobook; "Winter Stories" has genre "Spoken
        Word", so only the .m4b / M4B brand / chapter rule classifies it.
        Cover art (MP4 "covr"): baseline JPEG on "The Lighthouse Keeper"
        and "Winter Stories"; PNG (unsupported in 4b, FAILED) on "A Field
        Guide to Tones". Per-chapter images are not generated: ffmpeg has
        no simple way to author a QuickTime chapter image track, so this
        is skipped (docs/phase4b-brief.md item 1, "skip if hard").
    Playlists/*.m3u, *.m3u8
        "Road Trip.m3u" (8), "Focus.m3u8" (6, UTF-8 names), "Accents &
        Symbols.m3u" (7), "Empty.m3u" (no entries). Absolute paths.
    .rockbox/config.cfg
        "tagcache_ram: on" and "tagcache_autoupdate: on" (other lines kept).

No resume position is seeded: the tagcache keeps it in the database
(lastelapsed/lastoffset), which only the running firmware writes; the
audiobooks UI script plays a book briefly instead.

Sorting edge cases: "!!!", "10dd", "2Gem" (sort as '#'); "The Bellwethers",
"The Alder Trio", "The Zephyrs", "The The" ("The " ignored) next to
"Theory of Tides" (not stripped, sorts under T); Björvik, Silfur Rós, Émile
Ardent, Ångström Lane, Zoë Marchetti (accents fold); "aurora drift"
(lower case); song titles "99 Red Kites", "(Untitled)", "¿Dónde Estás?",
"Éclair", "the quiet one", "The End", "#9 Dream Machine", "Ölfus and the
Long Quiet Hours Before the Harbour Signal Fades" (sorts under O, mid
list; also the #38 marquee fixture: the one song title long enough to
overflow a Songs row, used by ipodui-p3-marquee.script and
ipodui-p3-motion-dev.script since Settings stopped being a plain list).

Every track is a stream copy ("ffmpeg -c copy", new tags) of one of 8 short
base clips (lavfi tones, mono 22.05 kHz 32k MP3, 2-5 s). Covers are
original procedural patterns with a big block letter (cover_ppm(),
textured so the #38 drift pan shows); the non-square and PNG fixtures
are abstract gradients drawn by ffmpeg.

Cover-art variety (Phase 4b, docs/phase4b-brief.md, Agent D; expected
state for each album is in tests/ui/art_manifest.txt, regenerate with
"python3 tests/ui/setup_library.py --art-manifest > tests/ui/art_manifest.txt"):
four albums (ART_NONE) have no art anywhere (NOART); ART_OVERRIDE gives
specific albums folder art ("folder.jpg" and "cover.jpg", baseline JPEG),
an embedded progressive JPEG (unsupported in 4b, FAILED), a folder-only
PNG (unsupported, FAILED), and a non-square 600x400 embedded cover (OK);
every other album keeps the default embedded baseline JPEG (ID3 APIC).
The progressive-JPEG fixture is made with jpegtran (preferred) or cjpeg
(libjpeg-turbo, both commonly installed alongside ffmpeg); without
either, generate() falls back to a plain baseline copy and prints a
warning, same as ffmpeg itself.
"""
import os
import random
import shutil
import subprocess
import sys
from concurrent.futures import ThreadPoolExecutor

VERSION = 9   # 3: Phase 4b cover-art variety (folder cover.jpg/folder.jpg,
              # embedded progressive JPEG, folder-only PNG, non-square
              # cover, an untagged-folder "album", podcast show art);
              # database rebuilt (tests/ui/art_manifest.txt)
              # 4: the untagged-folder tracks get an existing artist and
              # titles (still no album tag), keeping the golden views
              # 5: #38 (docs/phase-nav-brief.md) retargets the Phase 3
              # marquee scripts from the Settings stub to Music > Songs;
              # SPECIAL_TITLES' "Ölfus" grew into a marquee fixture (the
              # only title long enough to overflow a song row). Same
              # track (seeded rnd), same 'O' sort bucket: songs_top (A)
              # and songs_end ('#') in ipodui-p4-sort/-p4-qscroll keep
              # their goldens; total Songs count is unchanged (replaced,
              # not added)
              # 6: #38 end review: the six embedded covers are textured
              # (original procedural patterns + a large block letter,
              # cover_ppm()) instead of flat gradients, so the drift's
              # 1 px / 150 ms pan is visible in review videos; art
              # goldens recaptured
              # 7: the same, with a JFIF APP0 (setsar=1): 6's covers had
              # none, so MP4 covr art read as NOART
              # 8/9: every image encode is bitexact (no "Lavc<version>"
              # JPEG COM marker) with fixed, SIMD-independent swscale
              # flags (IMG_EXACT), so bytes and pixels do not depend on
              # the ffmpeg build; every file gets a pinned mtime
              # (pin_mtimes()): the panel set's "newest albums"
              # (tag_mtime) followed ffmpeg job completion order

FF = ['ffmpeg', '-nostdin', '-v', 'error', '-y']
BITEXACT = ['-fflags', '+bitexact', '-flags:a', '+bitexact']
# Image encodes: no encoder-version comment, and a fixed, exact RGB->YUV
# conversion (swscale's default picks SIMD paths that may round
# differently per CPU/ffmpeg build)
IMG_EXACT = ['-fflags', '+bitexact', '-flags:v', '+bitexact',
             '-sws_flags', 'bicubic+bitexact+accurate_rnd+full_chroma_int']
MP3 = ['-c:a', 'libmp3lame', '-b:a', '32k', '-ac', '1', '-ar', '22050']
AAC = ['-c:a', 'aac', '-b:a', '16k', '-ac', '1', '-ar', '22050']

# (artist, [(album, genre, tracks, composer or None)...])
ARTISTS = [
    ("!!!", [("Strange Weather", "Electronic", 5, None)]),
    ("10dd", [("Sheet Tones", "Rock", 5, None)]),
    ("2Gem", [("Low Signal", "Hip-Hop", 5, None)]),
    ("Amber Coast", [("Harbour Glass", "Pop", 6, None),
                     ("Tidewater", "Pop", 4, None)]),
    ("aurora drift", [("slow light", "Ambient", 5, None)]),
    ("Ångström Lane", [("Units of Light", "Electronic", 5, None)]),
    ("Basalt", [("Columns", "Rock", 5, None)]),
    ("Björvik", [("Glacier Tones", "Electronic", 6, None)]),
    ("Cedar & Pine", [("Woodgrain", "Folk", 5, None)]),
    ("Clara Ostrander", [("Preludes for Sine", "Classical", 6,
                          "Clara Ostrander")]),
    ("Copper Moth", [("Patina", "Rock", 5, None)]),
    ("Delta Static", [("Interference", "Electronic", 5, None),
                      ("Carrier Wave", "Electronic", 4, None)]),
    ("Echo Harbor", [("Fog Signals", "Ambient", 5, None)]),
    ("Émile Ardent", [("Nocturnes", "Classical", 5, "Frédéric Lamoureux")]),
    ("Fennel", [("Seedlings", "Folk", 4, None)]),
    ("Glass Orchard", [("Prism Fruit", "Pop", 5, None)]),
    ("Harbor Lights", [("Night Ferry", "Jazz", 5, None)]),
    ("Indigo Rail", [("Night Train", "Jazz", 5, None),
                     ("Platform Nine", "Jazz", 4, None)]),
    ("Juniper Hall", [("Chamber Pieces", "Classical", 5, "J. Halvorsen")]),
    ("Kepler Strings", [("Orbits", "Classical", 5, "J. Halvorsen")]),
    ("Kestrel", [("Hover", "Rock", 5, None)]),
    ("Lumen Field", [("Daybreak", "Pop", 5, None),
                     ("Noonday", "Pop", 4, None)]),
    ("Marble Arch Trio", [("Standards in Sine", "Jazz", 6, "Ödön Weiss")]),
    ("Mosaic Unit", [("Tesserae", "Hip-Hop", 5, None)]),
    ("Nightjar", [("Dusk Calls", "Folk", 5, None)]),
    ("Northbound", [("Compass", "Hip-Hop", 4, None)]),
    ("Orchid Engine", [("Hothouse", "Electronic", 5, None)]),
    ("Ostinato", [("Repeat Figures", "Classical", 5, "Clara Ostrander")]),
    ("Paper Lanterns", [("Festival", "Pop", 5, None)]),
    ("Quartz Lake", [("Clear Water", "Ambient", 5, None)]),
    ("Riverine", [("Meanders", "Folk", 5, None),
                  ("Oxbow", "Folk", 4, None)]),
    ("Saltmarsh", [("Low Tide", "Ambient", 4, None)]),
    ("Silfur Rós", [("Ísjaki Tones", "Ambient", 5, None),
                   ("Hljóð", "Ambient", 4, None)]),
    ("The Alder Trio", [("Branches", "Jazz", 5, None)]),
    ("The Bellwethers", [("Flock Together", "Rock", 6, "Lennart Vik"),
                         ("Shepherd's Delight", "Rock", 5, "Lennart Vik"),
                         ("Wool & Wire", "Rock", 4, "Lennart Vik")]),
    ("The The", [("Infected Sines", "Rock", 4, None)]),
    ("The Zephyrs", [("Westerly", "Pop", 5, None)]),
    ("Theory of Tides", [("Moon Pull", "Electronic", 5, None)]),
    ("Tin Sparrow", [("Small Birds", "Folk", 5, None)]),
    ("Umber", [("Earth Tones", "Ambient", 4, None)]),
    ("Velvet Static", [("Soft Noise", "Electronic", 5, None),
                       ("Hiss Theory", "Electronic", 4, None)]),
    ("Willow Creek", [("Bends", "Folk", 5, None)]),
    ("Xylem", [("Sap Rising", "Electronic", 4, None)]),
    ("Yarrow", [("Field Notes", "Folk", 4, None)]),
    ("Zephyr Lane", [("Crosswind", "Pop", 5, None),
                     ("Leeward", "Pop", 4, None)]),
    ("Zoë Marchetti", [("Ritratti", "Classical", 5, "Zoë Marchetti"),
                       ("Ritratti II", "Classical", 4, "Zoë Marchetti")]),
    ("Kite Theory", [("Updraft", "Pop", 5, None),
                     ("Tailwind", "Pop", 5, None)]),
]

COMPILATION = ("Summer Tones Vol. 1", "Pop", [
    "Amber Coast", "Björvik", "The Zephyrs", "10dd", "Glass Orchard",
    "Lumen Field", "Paper Lanterns", "Silfur Rós", "Kestrel", "Fennel",
    "Echo Harbor", "!!!"])

SPECIAL_TITLES = ["99 Red Kites", "(Untitled)", "¿Dónde Estás?", "Éclair",
                  "the quiet one", "The End", "#9 Dream Machine",
                  "Ölfus and the Long Quiet Hours Before the Harbour "
                  "Signal Fades"]
ADJ = ["Amber", "Blue", "Copper", "Distant", "Early", "Faded", "Golden",
       "Hollow", "Idle", "Jagged", "Kind", "Late", "Mellow", "Narrow",
       "Open", "Pale", "Quiet", "Restless", "Silver", "Tender", "Upper",
       "Velvet", "Wide", "Young", "Zero"]
NOUN = ["Harbour", "Signal", "Meadow", "Engine", "Lantern", "River",
        "Window", "Orbit", "Static", "Garden", "Ferry", "Mirror", "Summit",
        "Valley", "Ember", "Comet", "Tide", "Bridge", "Canyon", "Echo"]

PODCASTS = [
    ("Signal Hour", "Signal Hour Network",
     ["Episode 1: Hello Sine", "Episode 2: Square Deal"]),
    ("Tone Talk Weekly", "Tone Talk Collective",
     ["Ep. 41 Listener Mail", "Ep. 42 The Noise Floor"]),
]

# (author, title, genre, [(chapter title, seconds)...])
BOOKS = [
    ("Imogen Thrale", "The Lighthouse Keeper", "Audiobook",
     [("Opening Credits", 3), ("Chapter 1: The Lamp", 4),
      ("Chapter 2: The Storm", 4), ("End Credits", 3)]),
    ("Osric Pell", "A Field Guide to Tones", "Audiobook",
     [("Introduction", 4), ("Part One: Sines", 4), ("Part Two: Squares", 4)]),
    ("Maren Solvik", "Winter Stories", "Spoken Word",
     [("Frost", 3), ("Thaw", 3), ("Ice Road", 3), ("Snowmelt", 3),
      ("Spring", 3)]),
]

# Labels from the always-present mini library (tests/ui/setup_music.sh)
MINI_BOOK = ("Fixture Author", "Mini Audiobook")

BASES = [(440, 2), (523, 3), (587, 4), (659, 5), (330, 2), (392, 3),
         (494, 4), (262, 5)]
COVERS = [("0x1d3557", "0xe76f51"), ("0x2a9d8f", "0xe9c46a"),
          ("0x6d597a", "0xeaac8b"), ("0x003049", "0xfcbf49"),
          ("0x264653", "0xf4a261"), ("0x5f0f40", "0x0f4c5c")]

# Block letters (5 x 7) for the textured covers' big initial.
LETTERS = {
    'A': ["01110", "10001", "10001", "11111", "10001", "10001", "10001"],
    'B': ["11110", "10001", "10001", "11110", "10001", "10001", "11110"],
    'C': ["01111", "10000", "10000", "10000", "10000", "10000", "01111"],
    'D': ["11110", "10001", "10001", "10001", "10001", "10001", "11110"],
    'E': ["11111", "10000", "10000", "11110", "10000", "10000", "11111"],
    'F': ["11111", "10000", "10000", "11110", "10000", "10000", "10000"],
}


def cover_ppm(i, path, size=240):
    """Cover i (0-5) as a binary PPM: an original procedural pattern
    (stripes, rings, checks, sunburst, dots, zigzag; one per cover) over
    the COVERS gradient, a frame, and a large block letter 'A'-'F' with a
    shadow. Sharp detail everywhere, so a 1 px pan shows."""
    import math
    c0, c1 = [tuple(int(c[2 + k:4 + k], 16) for k in (0, 2, 4))
              for c in COVERS[i % len(COVERS)]]
    letter = LETTERS['ABCDEF'[i % 6]]
    sc = size // 10                      # letter cell: 24 px at 240
    lw, lh = 5 * sc, 7 * sc
    lx, ly = (size - lw) // 2, (size - lh) // 2
    half = size / 2.0
    out = bytearray()
    for y in range(size):
        for x in range(size):
            t = (x + y) / (2.0 * (size - 1))
            base = [c0[k] + (c1[k] - c0[k]) * t for k in range(3)]
            dx, dy = x - half, y - half
            kind = i % 6
            if kind == 0:
                on = ((x + y) // 12) % 2 == 0
            elif kind == 1:
                on = int(math.hypot(dx, dy)) // 10 % 2 == 0
            elif kind == 2:
                on = (x // 20 + y // 20) % 2 == 0
            elif kind == 3:
                on = int((math.atan2(dy, dx) + math.pi) * 12 / math.pi) % 2 == 0
            elif kind == 4:
                on = math.hypot(x % 20 - 10, y % 20 - 10) < 5
            else:
                on = (y + abs((x % 32) - 16)) // 10 % 2 == 0
            f = 1.25 if on else 0.75
            px = [min(255, max(0, int(v * f))) for v in base]
            if x < 6 or y < 6 or x >= size - 6 or y >= size - 6:
                px = [245, 245, 240]         # a light frame
            for (ox, oy, col) in ((6, 6, (20, 20, 24)), (0, 0, (250, 248, 240))):
                gx, gy = x - lx - ox, y - ly - oy
                if 0 <= gx < lw and 0 <= gy < lh \
                        and letter[gy // sc][gx // sc] == '1':
                    px = list(col)
            out += bytes(px)
    with open(path, 'wb') as f:
        f.write(b'P6\n%d %d\n255\n' % (size, size))
        f.write(out)


# Cover-art variety by album_no (Phase 4b, docs/phase4b-brief.md Agent D).
# Albums not named below get the default: embedded baseline JPEG (ID3
# APIC), or no art at all when album_no % 7 == 6 (ART_NONE lists those
# four that keep that default; the others in that set get an override).
# tests/ui/art_manifest.txt (--art-manifest) documents the expected
# art_state for every one of these.
ART_NONE = {34, 41, 48, 55}        # Riverine/Meanders, Bellwethers/
                                    # Shepherd's Delight, Velvet Static/
                                    # Soft Noise, Zoë Marchetti/Ritratti:
                                    # NOART (see art_manifest() for the
                                    # authoritative path per album_no)
ART_OVERRIDE = {
    6:  'folder-jpg',   # Ångström Lane / Units of Light: folder.jpg (OK)
    13: 'progressive',  # Delta Static / Carrier Wave: embedded, FAILED
    20: 'png-folder',   # Indigo Rail / Platform Nine: folder.png, FAILED
    27: 'nonsquare',    # Mosaic Unit / Tesserae: embedded 600x400, OK
    42: 'cover-jpg',    # Bellwethers / Wool & Wire: cover.jpg (OK)
}
# Folder-art filename for the 'folder-jpg'/'cover-jpg'/'png-folder' kinds
ART_FOLDER_NAME = {'folder-jpg': 'folder.jpg', 'cover-jpg': 'cover.jpg',
                   'png-folder': 'folder.png'}

# A folder of tracks without an album tag: the directory-grouped "album"
# case (Revision 1 point 7). Folder art (OK) shows the grouping and the
# art lookup both work without an album tag. Artist and titles are set
# (UNTAGGED_ARTIST is an existing artist) only to keep the golden views
# of Phases 2-4 unchanged.
UNTAGGED_DIR = 'Library/Untagged Session'
UNTAGGED_TITLES = ['Field Recording One', 'Field Recording Two']
UNTAGGED_ARTIST = 'Riverine'


def art_kind(album_no):
    """One of 'embedded', 'progressive', 'nonsquare', 'folder-jpg',
    'cover-jpg', 'png-folder', 'none' for a music album_no (None for
    podcasts/compilation-less items, which the caller decides for
    itself)."""
    if album_no in ART_OVERRIDE:
        return ART_OVERRIDE[album_no]
    return 'none' if album_no % 7 == 6 else 'embedded'


def safe(name):
    return name.replace('/', '-').replace(':', '-')


def tracks():
    """Every music track as a dict, deterministic."""
    rnd = random.Random(4)
    special = list(SPECIAL_TITLES)
    out = []
    album_no = 0
    used = set()

    def title():
        if special and rnd.random() < 0.05:
            return special.pop(0)
        while True:
            t = '%s %s' % (rnd.choice(ADJ), rnd.choice(NOUN))
            if t not in used:
                used.add(t)
                return t

    for artist, albums in ARTISTS:
        for album, genre, n, composer in albums:
            year = 1970 + (album_no * 7) % 55
            for i in range(n):
                out.append(dict(
                    dir='Library/%s/%s' % (safe(artist), safe(album)),
                    title=title(), artist=artist, album_artist=artist,
                    album=album, genre=genre, track='%d/%d' % (i + 1, n),
                    date=str(year), composer=composer, album_no=album_no,
                    compilation=False))
            album_no += 1
    album, genre, artists = COMPILATION
    for i, artist in enumerate(artists):
        out.append(dict(
            dir='Library/Compilations/%s' % safe(album), title=title(),
            artist=artist, album_artist='Various Artists', album=album,
            genre=genre, track='%d/%d' % (i + 1, len(artists)), date='2024',
            composer=None, album_no=album_no, compilation=True))
    # leftover special titles go on the first tracks that have plain ones
    for t in out:
        if not special:
            break
        if t['title'] not in SPECIAL_TITLES:
            t['title'] = special.pop(0)
    for t in out:
        num = int(t['track'].split('/')[0])
        t['path'] = '%s/%02d %s.mp3' % (t['dir'], num, safe(t['title']))
    return out


def podcast_files():
    out = []
    for show, author, eps in PODCASTS:
        for i, ep in enumerate(eps):
            out.append(dict(
                path='Podcasts/%s/%s.mp3' % (safe(show), safe(ep)),
                title=ep, artist=author, album_artist=author, album=show,
                genre='Podcast', track='%d/%d' % (i + 1, len(eps)),
                date='2026', composer=None, album_no=None,
                compilation=False,
                show_art=(show == 'Signal Hour')))  # one show with art,
    return out                                      # one without


def book_paths():
    return ['Audiobooks/%s/%s.m4b' % (safe(a), safe(t)) for a, t, _, _ in BOOKS]


def file_count():
    return (len(tracks()) + len(podcast_files()) + len(BOOKS)
            + len(UNTAGGED_TITLES))


def manifest():
    """kind<TAB>label lines: everything that must never appear in a music
    list (Artists, Albums, Songs, Genres, Composers, Compilations, Shuffle
    Songs)."""
    rows = []
    for author, title, genre, chapters in BOOKS:
        rows += [('audiobook', title), ('audiobook-author', author)]
    rows += [('audiobook', MINI_BOOK[1]), ('audiobook-author', MINI_BOOK[0])]
    for show, author, eps in PODCASTS:
        rows += [('podcast', show), ('podcast-author', author)]
        rows += [('podcast', ep) for ep in eps]
    rows += [('genre', 'Audiobook'), ('genre', 'Podcast'),
             ('genre', 'Spoken Word')]
    lines = ['# Labels of the non-music items in the Phase 4 test library',
             '# (tests/ui/setup_library.py --manifest). scripts/check_trace.py',
             '# fails if one shows up as a "lib row <music cat> <label>" line.',
             '# kind<TAB>label']
    return '\n'.join(lines + ['%s\t%s' % r for r in rows]) + '\n'


# Human-readable note for each art_kind()/override outcome, keyed the same
# way art_kind() resolves them (plus 'show-art'/'no-show-art' for
# podcasts and literal strings for the three audiobooks and the
# untagged folder, which aren't picked by album_no at all).
ART_STATE = {
    'embedded': ('OK', 'embedded baseline JPEG (ID3 APIC)'),
    'nonsquare': ('OK', 'embedded baseline JPEG, non-square 600x400'),
    'folder-jpg': ('OK', 'folder.jpg (baseline JPEG), no embedded art'),
    'cover-jpg': ('OK', 'cover.jpg (baseline JPEG), no embedded art'),
    'progressive': ('FAILED', 'embedded progressive JPEG (unsupported '
                             'in 4b; the Mac tool, Phase 7b, adds it)'),
    'png-folder': ('FAILED', 'folder.png only (PNG unsupported in 4b)'),
    'none': ('NOART', 'no cover anywhere'),
}


def art_manifest():
    """<path><TAB><state><TAB><note> lines: the expected art_state (OK,
    NOART or FAILED, docs/phase4b-brief.md Agent B) for every album with
    cover-art variety worth asserting on. Regenerate with
    "python3 tests/ui/setup_library.py --art-manifest >
    tests/ui/art_manifest.txt" whenever the variety above changes."""
    by_album = {}
    for t in tracks():
        by_album.setdefault(t['album_no'], t['dir'])
    rows = []
    for album_no in sorted(ART_NONE) + sorted(ART_OVERRIDE):
        kind = art_kind(album_no)
        state, note = ART_STATE[kind]
        rows.append((by_album[album_no], state, note))
    rows.sort()
    rows.append((UNTAGGED_DIR, 'OK',
                'folder cover.jpg; no album tag (directory-grouped, '
                'not a tagged album)'))
    for show, author, eps in PODCASTS:
        state, note = ('OK', 'embedded baseline JPEG (show art)') \
            if show == 'Signal Hour' else ('NOART', 'no show art')
        rows.append(('Podcasts/%s' % safe(show), state, note))
    for author, title, genre, chapters in BOOKS:
        path = 'Audiobooks/%s/%s' % (safe(author), safe(title))
        if title == 'A Field Guide to Tones':
            rows.append((path, 'FAILED', 'MP4 covr, PNG (unsupported in '
                        '4b; the Mac tool, Phase 7b, adds it)'))
        else:
            rows.append((path, 'OK', 'MP4 covr, baseline JPEG'))
    lines = ['# Expected cover-art state per album/show/book',
             '# (tests/ui/setup_library.py --art-manifest).',
             '# OK: a thumbnail should build. NOART: no source anywhere,',
             '# the note/podcast glyph forever. FAILED: a source exists',
             '# but 4b cannot decode it (same glyph; the Mac tool, Phase',
             '# 7b, adds PNG and progressive JPEG). Per-chapter audiobook',
             '# images are not generated (no easy way to author a',
             '# QuickTime chapter image track with ffmpeg): not a case',
             '# here, skipped outright (docs/phase4b-brief.md item 1).',
             '# path<TAB>state<TAB>note']
    return '\n'.join(lines + ['%s\t%s\t%s' % r for r in rows]) + '\n'


def set_config(disk):
    """Set the tagcache keys in .rockbox/config.cfg, keeping other lines."""
    rb = os.path.join(disk, '.rockbox')
    if not os.path.isdir(rb):
        # don't create it: shots.sh runs "make install" when it is missing
        print('setup_library: no %s yet ("make install" first); config '
              'keys not written' % rb, file=sys.stderr)
        return
    cfg = os.path.join(rb, 'config.cfg')
    want = {'tagcache_ram': 'on', 'tagcache_autoupdate': 'on'}
    lines = []
    if os.path.exists(cfg):
        lines = open(cfg, encoding='utf-8', errors='replace').read().splitlines()
    out = [l for l in lines if l.split(':', 1)[0].strip() not in want]
    out += ['%s: %s' % kv for kv in want.items()]
    if out != lines:
        with open(cfg, 'w', encoding='utf-8') as f:
            f.write('\n'.join(out) + '\n')


def run(cmd):
    r = subprocess.run(cmd, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    if r.returncode:
        raise SystemExit('setup_library: ffmpeg failed: %s\n%s'
                         % (' '.join(cmd), r.stderr.decode(errors='replace')))


def make_progressive(src, dst):
    """A genuine progressive-scan JPEG (SOF2), for the one album whose
    art is meant to come back FAILED (4b decodes baseline JPEG only;
    ffmpeg's own mjpeg encoder can't write progressive). Tries jpegtran
    (lossless baseline->progressive transcode), then cjpeg via an
    intermediate PPM (both libjpeg-turbo, commonly installed alongside
    ffmpeg); falls back to a plain baseline copy with a warning if
    neither tool is on PATH (the FAILED case then won't actually
    trigger, same spirit as the per-chapter-image art that is skipped
    outright, docs/phase4b-brief.md item 1)."""
    if shutil.which('jpegtran'):
        r = subprocess.run(['jpegtran', '-progressive', '-outfile', dst,
                            src], stdout=subprocess.PIPE,
                           stderr=subprocess.PIPE)
        if r.returncode == 0:
            return
    if shutil.which('cjpeg'):
        ppm = dst + '.ppm'
        run(FF + ['-i', src, ppm])
        r = subprocess.run(['cjpeg', '-progressive', '-outfile', dst, ppm],
                           stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        os.remove(ppm)
        if r.returncode == 0:
            return
    print('setup_library: warning: no jpegtran/cjpeg on PATH; the '
          'progressive-JPEG art fixture is a plain baseline copy (its '
          'FAILED case will not trigger)', file=sys.stderr)
    shutil.copyfile(src, dst)


def tags(t):
    md = ['title', 'artist', 'album_artist', 'album', 'genre', 'track',
          'date', 'composer']
    args = []
    for k in md:
        if t.get(k):
            args += ['-metadata', '%s=%s' % (k, t[k])]
    if t.get('compilation'):
        args += ['-metadata', 'compilation=1']
    return args


def generate(disk, tmp):
    for d in ('Library', 'Podcasts', 'Audiobooks', 'Playlists'):
        shutil.rmtree(os.path.join(disk, d), ignore_errors=True)
    rb = os.path.join(disk, '.rockbox')
    if os.path.isdir(rb):           # stale database: rebuild from scratch
        for f in os.listdir(rb):
            if f.startswith('database_') and f.endswith('.tcd'):
                os.remove(os.path.join(rb, f))
            elif f in ('database_gen0', 'database_gen1', 'database_gen2'):
                shutil.rmtree(os.path.join(rb, f), ignore_errors=True)
            elif f in ('database_manifest0.dat', 'database_manifest1.dat',
                       'database_manifest0.tmp', 'database_manifest1.tmp',
                       'database_stats.jnl', 'database_stats.prev',
                       'database_stats.tmp', 'database_lib.dat'):
                os.remove(os.path.join(rb, f))
    set_config(disk)

    jobs = []
    for i, (freq, dur) in enumerate(BASES):
        jobs.append(FF + ['-f', 'lavfi', '-i',
                          'sine=frequency=%d:sample_rate=22050:duration=%d'
                          % (freq, dur)] + MP3 + BITEXACT +
                    ['-map_metadata', '-1', '%s/base%d.mp3' % (tmp, i)])
    for i in range(len(COVERS)):
        cover_ppm(i, '%s/cover%d.ppm' % (tmp, i))
        # setsar: a PPM carries no aspect ratio, and without one the
        # encoder writes no JFIF APP0 (the MP4 covr check wants FFD8FFE0)
        jobs.append(FF + ['-i', '%s/cover%d.ppm' % (tmp, i),
                          '-vf', 'setsar=1,format=yuvj420p', '-frames:v', '1',
                          '-c:v', 'mjpeg', '-q:v', '5',
                          ] + IMG_EXACT + [
                          '%s/cover%d.jpg' % (tmp, i)])
    # Non-square (600x400, 'nonsquare') and PNG ('png-folder' folder art,
    # and the MP4-covr-PNG audiobook) covers, original gradients as above.
    c0, c1 = COVERS[0]
    jobs.append(FF + ['-f', 'lavfi', '-i',
                      'gradients=s=600x400:c0=%s:c1=%s:x0=0:y0=0:x1=600:'
                      'y1=400:d=1:seed=90' % (c0, c1),
                      '-vf', 'format=yuvj420p', '-frames:v', '1',
                      '-c:v', 'mjpeg', '-q:v', '5',
                      ] + IMG_EXACT + [
                      '%s/cover_nonsquare.jpg' % tmp])
    c0, c1 = COVERS[1]
    jobs.append(FF + ['-f', 'lavfi', '-i',
                      'gradients=s=240x240:c0=%s:c1=%s:x0=0:y0=0:x1=240:'
                      'y1=240:d=1:seed=91' % (c0, c1),
                      '-frames:v', '1', '-c:v', 'png',
                      ] + IMG_EXACT + [
                      '%s/cover.png' % tmp])
    workers = min(16, (os.cpu_count() or 4) * 2)
    with ThreadPoolExecutor(workers) as ex:
        list(ex.map(run, jobs))
    # A genuine progressive JPEG (SOF2) for the 'progressive' album,
    # derived from cover0.jpg (needs it to already exist, so it runs
    # after the batch above, not inside it).
    make_progressive('%s/cover0.jpg' % tmp, '%s/cover_progressive.jpg' % tmp)

    jobs = []
    items = tracks() + podcast_files()
    for n, t in enumerate(items):
        out = os.path.join(disk, t['path'])
        os.makedirs(os.path.dirname(out), exist_ok=True)
        cmd = FF + ['-i', '%s/base%d.mp3' % (tmp, n % len(BASES))]
        a = t['album_no']
        if t.get('show_art'):
            kind = 'embedded'
            art_src = '%s/cover0.jpg' % tmp
        elif a is not None:
            kind = art_kind(a)
            art_src = {'embedded': '%s/cover%d.jpg' % (tmp, a % len(COVERS)),
                      'progressive': '%s/cover_progressive.jpg' % tmp,
                      'nonsquare': '%s/cover_nonsquare.jpg' % tmp,
                      }.get(kind)
        else:
            kind, art_src = 'none', None
        if art_src:
            cmd += ['-i', art_src, '-map', '0:a', '-map', '1:v',
                    '-disposition:v', 'attached_pic',
                    '-metadata:s:v', 'title=Album cover',
                    '-metadata:s:v', 'comment=Cover (front)']
        else:
            cmd += ['-map', '0:a']
        cmd += ['-c', 'copy', '-map_metadata', '-1', '-id3v2_version', '3'] \
            + BITEXACT + tags(t) + [out]
        jobs.append(cmd)

    for (author, title, genre, chapters), rel in zip(BOOKS, book_paths()):
        out = os.path.join(disk, rel)
        os.makedirs(os.path.dirname(out), exist_ok=True)
        meta = os.path.join(tmp, safe(title) + '.txt')
        lines = [';FFMETADATA1', 'title=%s' % title, 'artist=%s' % author,
                 'album_artist=%s' % author, 'album=%s' % title,
                 'genre=%s' % genre, 'date=2026']
        t0 = 0
        for name, sec in chapters:
            lines += ['[CHAPTER]', 'TIMEBASE=1/1000', 'START=%d' % t0,
                      'END=%d' % (t0 + sec * 1000), 'title=%s' % name]
            t0 += sec * 1000
        with open(meta, 'w', encoding='utf-8') as f:
            f.write('\n'.join(lines) + '\n')
        # "A Field Guide to Tones" gets a PNG covr (unsupported in 4b,
        # FAILED); the others keep the default baseline JPEG covr (OK).
        if title == 'A Field Guide to Tones':
            cover = '%s/cover.png' % tmp
        else:
            cover = '%s/cover%d.jpg' % (tmp, len(jobs) % len(COVERS))
        jobs.append(FF + ['-f', 'lavfi', '-i',
                          'sine=frequency=%d:sample_rate=22050:duration=%g'
                          % (300 + 20 * len(chapters), t0 / 1000.0),
                          '-i', meta, '-i', cover,
                          '-map', '0:a', '-map', '2:v', '-map_metadata', '1',
                          '-map_chapters', '1'] + AAC +
                    ['-c:v', 'copy', '-disposition:v', 'attached_pic'] +
                    BITEXACT + ['-brand', 'M4B ', '-f', 'ipod', out])

    # An untagged-folder "album": no album tag, so the tagcache has to
    # group it by directory rather than by tag (Revision 1 point 7).
    for i, title in enumerate(UNTAGGED_TITLES):
        out = os.path.join(disk, UNTAGGED_DIR, '%02d %s.mp3' % (i + 1, title))
        os.makedirs(os.path.dirname(out), exist_ok=True)
        jobs.append(FF + ['-i', '%s/base%d.mp3' % (tmp, i % len(BASES)),
                          '-map', '0:a', '-c', 'copy', '-map_metadata', '-1',
                          '-metadata', 'artist=%s' % UNTAGGED_ARTIST,
                          '-metadata', 'title=%s' % title,
                          '-id3v2_version', '3'] + BITEXACT + [out])

    with ThreadPoolExecutor(workers) as ex:
        list(ex.map(run, jobs))
    jobs_all = jobs

    # Folder-only art (no embedded cover in any track of these albums):
    # 'folder-jpg'/'cover-jpg' write a baseline JPEG, 'png-folder' a PNG
    # (unsupported in 4b, FAILED). The untagged folder gets one too, to
    # show the folder-art lookup also works when grouped by directory.
    by_album = {}
    for t in tracks():
        by_album.setdefault(t['album_no'], t['dir'])
    for album_no, kind in ART_OVERRIDE.items():
        name = ART_FOLDER_NAME.get(kind)
        if name:
            src = '%s/cover.png' % tmp if kind == 'png-folder' \
                else '%s/cover0.jpg' % tmp
            shutil.copyfile(src, os.path.join(disk, by_album[album_no], name))
    shutil.copyfile('%s/cover0.jpg' % tmp,
                    os.path.join(disk, UNTAGGED_DIR, 'cover.jpg'))

    write_playlists(disk)
    pin_mtimes(disk, [j[-1] for j in jobs_all])
    with open(os.path.join(disk, 'Library', '.test-library'), 'w') as f:
        f.write(stamp() + '\n')
    print('setup_library: %d files written to %s' % (file_count(), disk),
          file=sys.stderr)


# 2026-01-02 00:00:00 UTC: after setup_music.sh's pinned range, so the
# test library is always the newer one
MTIME_BASE = 1767312000
MTIME_STEP = 10         # > the 2 s FAT timestamp granularity


def pin_mtimes(disk, ordered):
    """Pins every generated file's mtime: 'ordered' (the audio outputs, in
    the order they were queued) first, then any other file under the
    generated trees in sorted path order, MTIME_STEP apart from
    MTIME_BASE. Left alone, mtimes would be generation wall-clock times
    whose order follows parallel ffmpeg completion, and the shell's panel
    set ("newest albums" by tag_mtime, library.c) would pick a different
    set of covers on every fresh disk."""
    seen = set()
    order = []
    for f in ordered:
        if f.startswith(disk) and os.path.isfile(f) and f not in seen:
            seen.add(f)
            order.append(f)
    rest = []
    for d in ('Library', 'Podcasts', 'Audiobooks', 'Playlists'):
        for root, _dirs, files in os.walk(os.path.join(disk, d)):
            for name in files:
                f = os.path.join(root, name)
                if f not in seen:
                    rest.append(f)
    for i, f in enumerate(order + sorted(rest)):
        t = MTIME_BASE + i * MTIME_STEP
        os.utime(f, (t, t))


def write_playlists(disk):
    pl = os.path.join(disk, 'Playlists')
    os.makedirs(pl, exist_ok=True)
    all_t = tracks()

    def by(pred, n):
        return ['/' + t['path'] for t in all_t if pred(t)][:n]

    lists = {
        'Road Trip.m3u': [all_t[i]['path'] for i in range(0, 240, 30)],
        'Focus.m3u8': by(lambda t: t['genre'] == 'Ambient', 6),
        'Accents & Symbols.m3u': [
            by(lambda t, a=a: t['artist'] == a and not t['compilation'], 1)[0]
            for a in ('!!!', '10dd', '2Gem', 'Björvik', 'Silfur Rós',
                      'Émile Ardent', 'Zoë Marchetti')],
        'Empty.m3u': [],
    }
    for name, paths in lists.items():
        with open(os.path.join(pl, name), 'w', encoding='utf-8') as f:
            f.write('#EXTM3U\n')
            for p in paths:
                f.write('/' + p.lstrip('/') + '\n')


def stamp():
    return '%d %d' % (VERSION, file_count())


def main():
    a = sys.argv[1:]
    if a == ['--stamp']:
        print(stamp())
    elif a == ['--manifest']:
        sys.stdout.write(manifest())
    elif a == ['--art-manifest']:
        sys.stdout.write(art_manifest())
    elif len(a) == 2 and a[0] == '--config':
        set_config(a[1])
    elif len(a) == 3 and a[0] == '--generate':
        generate(a[1], a[2])
        here = os.path.dirname(os.path.abspath(__file__))
        for name, want in (('library_manifest.txt', manifest()),
                           ('art_manifest.txt', art_manifest())):
            committed = os.path.join(here, name)
            if not os.path.exists(committed) or \
                    open(committed, encoding='utf-8').read() != want:
                print('setup_library: warning: tests/ui/%s is out of date '
                      '(regenerate with --%s)' % (name, 'manifest' if
                      name == 'library_manifest.txt' else 'art-manifest'),
                      file=sys.stderr)
    else:
        print(__doc__, file=sys.stderr)
        return 2
    return 0


if __name__ == '__main__':
    sys.exit(main())
