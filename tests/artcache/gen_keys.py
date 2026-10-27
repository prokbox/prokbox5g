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

"""Reference implementation of the album art cache key (apps/ipodui/artkey.h,
docs/design/artcache.md §5) and generator of the golden file keys.txt.

The host-side Mac tool (Phase 7b) must produce the same keys: it should
import artcache_key() from here or match keys.txt byte for byte.

  gen_keys.py            print keys.txt for the fixture tags below
  gen_keys.py --check F  exit 1 unless F equals what this script prints

keys.txt: one album per line, five TAB-separated fields
  albumartist  artist  album  first_track_path  key (16 hex digits)
Escapes inside fields: \\t TAB, \\n LF, \\r CR, \\\\ backslash; a field that is
exactly \\N is NULL (no tag / no path). Lines starting with # are comments.
"""
import sys

FNV64_OFFSET = 0xcbf29ce484222325
FNV64_PRIME = 0x100000001b3
WS = b" \t\n\v\f\r"
SEP = b"\x1f"


def _cf_tables():
    """cf_alt / cf_one of apps/ipodui/sortkey.c (the single source)."""
    import os
    import re
    src = open(os.path.join(os.path.dirname(os.path.abspath(__file__)),
                            '..', '..', 'apps', 'ipodui', 'sortkey.c')).read()

    def table(name):
        body = re.search(name + r'\[\]\[2\] = \{(.*?)\n\};', src, re.S).group(1)
        return [(int(a, 16), int(b, 16)) for a, b in
                re.findall(r'\{\s*0x([0-9a-f]+),\s*0x([0-9a-f]+)\s*\}', body)]
    return table('cf_alt'), table('cf_one')


CF_ALT, CF_ONE = _cf_tables()


def casefold_char(c):
    """ui_casefold_char() of sortkey.c."""
    if c < 0x41:
        return c
    if c <= 0x5a:
        return c + 0x20
    if c < 0xb5 or 0xdf <= c <= 0xff:
        return c
    if 0xc0 <= c <= 0xde and c != 0xd7:
        return c + 0x20
    if 0x430 <= c <= 0x45f or 0x3ac <= c <= 0x3c1:
        return c
    if 0x52f < c < 0x1e00 or c > 0x1eff:
        return c
    if 0x388 <= c <= 0x38a:
        return c + 0x25
    if 0x391 <= c <= 0x3a1 or 0x3a3 <= c <= 0x3ab or 0x410 <= c <= 0x42f:
        return c + 0x20
    if 0x400 <= c <= 0x40f:
        return c + 0x50
    for a, b in CF_ALT:
        if a <= c <= b:
            return c if (c ^ a) & 1 else c + 1
    for a, b in CF_ONE:
        if c == a:
            return b
    return c


def _utf8_at(s, i):
    """artkey.c utf8_at(): (code point, length) or (0, 0)."""
    c = s[i]
    if 0xc2 <= c <= 0xdf and i + 1 < len(s) and s[i + 1] & 0xc0 == 0x80:
        return ((c & 0x1f) << 6) | (s[i + 1] & 0x3f), 2
    if c & 0xf0 == 0xe0 and i + 2 < len(s) and s[i + 1] & 0xc0 == 0x80 \
            and s[i + 2] & 0xc0 == 0x80:
        u = ((c & 0x0f) << 12) | ((s[i + 1] & 0x3f) << 6) | (s[i + 2] & 0x3f)
        return (u, 3) if u >= 0x800 else (0, 0)
    return 0, 0


def norm(s: bytes, ascii_only=False) -> bytes:
    """Trim, collapse whitespace runs to one space, fold ASCII A-Z and
    (#47) other UTF-8 characters through casefold_char()."""
    out, gap, any_ = bytearray(), False, False
    i = 0
    while i < len(s):
        c = s[i]
        i += 1
        if c in WS:
            gap = any_
            continue
        if gap:
            out.append(0x20)
        gap, any_ = False, True
        if c < 0x80 or ascii_only:
            out.append(c + 32 if 0x41 <= c <= 0x5a else c)
            continue
        u, n = _utf8_at(s, i - 1)
        if not u or casefold_char(u) == u:
            out.append(c)
            continue
        out += chr(casefold_char(u)).encode('utf-8')
        i += n - 1
    return bytes(out)


def untagged(s):
    return s is None or norm(s) in (b"", b"<untagged>")


def key_string(albumartist, artist, album, path):
    if untagged(album):
        if path is None:
            return None
        d = norm(path[:path.rfind(b"/")]) if b"/" in path else b""
        return b"dir" + SEP + d
    a = albumartist if not untagged(albumartist) else \
        artist if not untagged(artist) else b""
    return norm(a) + SEP + norm(album)


def artcache_key(albumartist, artist, album, path):
    ks = key_string(albumartist, artist, album, path)
    if ks is None:
        return 0
    h = FNV64_OFFSET
    for c in ks:
        h = ((h ^ c) * FNV64_PRIME) & 0xffffffffffffffff
    return h or 1


def u(s):
    return None if s is None else s.encode("utf-8")


# (albumartist, artist, album, first_track_path); None = untagged / no path
FIXTURES = [
    ("Signalfire", "Signalfire", "OK Compass", "/Music/Signalfire/OK Compass/01 Opener.mp3"),
    # case and whitespace variants of the same album: one key
    (None, "signalfire", "ok compass", "/Music/x/01.mp3"),
    (None, "  SIGNALFIRE ", "OK   Compass\t", None),
    ("<Untagged>", "Signalfire", "OK\nCompass", None),
    # album artist wins over track artist
    ("Various Artists", "Tidal Drift", "Now 100", "/Music/Comp/Now 100/03.mp3"),
    (" various  artists ", "Harbour Lights", "NOW 100", None),
    # no album artist: track artist; both untagged: empty artist
    ("", "Björvik", "Homestead", "/Music/Björvik/Homestead/01.flac"),
    ("", "BJÖRVIK", "Homestead", None),           # #47: folded, one key
    ("Élan", None, "Dune", None),
    ("ÉLAN", None, "DUNE", None),               # = Élan / Dune
    ("Elan", None, "Dune", None),               # accents kept: another
    ("Straße", None, "Lied", None),             # sharp s kept: not STRASSE
    ("Север", None, "Тихий берег", None),
    ("СЕВЕР", None, "ТИХИЙ БЕРЕГ", None),       # = Север
    ("ΩMEGA", None, "Ἀλφα", None),              # Greek; 4-byte-free
    ("ĐẠI", None, "Việt", None),
    ("đại", None, "VIỆT", None),                # Vietnamese: = ĐẠI
    (None, None, "Mystery Album", None),
    ("<untagged>", "<UNTAGGED>", "Mystery Album", None),
    # untagged album: the first track's directory
    ("Someone", "Someone", None, "/Music/Rips/Disc 1/track01.mp3"),
    (None, None, "", "/music/rips/disc 1/track02.mp3"),
    (None, None, "<Untagged>", "/MUSIC/RIPS/DISC 1/x.wav"),
    (None, None, "  ", "/root.mp3"),
    (None, None, None, "noslash.mp3"),
    (None, None, None, None),                    # no key at all: 0
    # podcasts group by album only (no artists passed)
    (None, None, "The Daily Show Podcast", "/Podcasts/Daily/ep1.mp3"),
    # separators and odd bytes inside tags
    ("AC/DC", "AC/DC", "Back in Black", "/Music/AC_DC/Back in Black/01.mp3"),
    ("A\x1fB", None, "C", None),
    ("日本語アーティスト", None, "アルバム", None),
    ("Silfur Rós", None, "( )", None),
    ("The Beckons", None, "Ashen Road", None),
    ("Beckons, The", None, "Ashen Road", None),
]


def esc(s):
    if s is None:
        return "\\N"
    return (s.replace("\\", "\\\\").replace("\t", "\\t")
             .replace("\n", "\\n").replace("\r", "\\r"))


def render():
    lines = ["# albumartist\tartist\talbum\tfirst_track_path\tkey "
             "(tests/artcache/gen_keys.py; do not edit by hand)"]
    for aa, ar, al, p in FIXTURES:
        k = artcache_key(u(aa), u(ar), u(al), u(p))
        lines.append("\t".join([esc(aa), esc(ar), esc(al), esc(p),
                                "%016x" % k]))
    return "\n".join(lines) + "\n"


def chapter_key(book, chapter):
    """artcache_key_chapter() (artkey.h, #39): FNV-1a 64 over the book key
    (8 bytes LE), 0x1F "ch", the chapter (4 bytes LE); 0 -> 1; none -> 0."""
    if book == 0 or chapter < 0:
        return 0
    h = FNV64_OFFSET
    for c in (book.to_bytes(8, "little") + SEP + b"ch"
              + chapter.to_bytes(4, "little")):
        h = ((h ^ c) * FNV64_PRIME) & 0xffffffffffffffff
    return h or 1


# (book key, chapter): the first fixtures' keys, edge chapters, no book
CHAPTER_FIXTURES = [
    (0xb60d10106ed9f8ce, 0), (0xb60d10106ed9f8ce, 1),
    (0xb60d10106ed9f8ce, 72), (0xb60d10106ed9f8ce, 647),
    (0xb60d10106ed9f8ce, 1023), (0xb60d10106ed9f8ce, 65535),
    (0x8b5681cc8200af1d, 0), (0x8b5681cc8200af1d, 2),
    (1, 0), (0xffffffffffffffff, 7), (0, 3), (0xb60d10106ed9f8ce, -1),
]


def render_chapters():
    lines = ["# book_key\tchapter\tkey "
             "(tests/artcache/gen_keys.py --chapters; do not edit by hand)"]
    for book, ch in CHAPTER_FIXTURES:
        lines.append("%016x\t%d\t%016x" % (book, ch, chapter_key(book, ch)))
    return "\n".join(lines) + "\n"


if __name__ == "__main__":
    chapters = "--chapters" in sys.argv
    if chapters:
        sys.argv.remove("--chapters")
    out = render_chapters() if chapters else render()
    if len(sys.argv) == 3 and sys.argv[1] == "--check":
        with open(sys.argv[2], encoding="utf-8") as f:
            if f.read() != out:
                print("keys.txt differs from gen_keys.py output")
                sys.exit(1)
        sys.exit(0)
    sys.stdout.write(out)
