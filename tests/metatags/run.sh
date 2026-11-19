#!/bin/sh
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

# Tag values from the real parsers (warble -m) on tests/fixtures/mv/
# (tests/fixtures/make_multivalue_fixtures.py):
#   #163 ID3 numeric genres 148-191 (Winamp 5.6 list; names checked with
#        ffprobe, FFmpeg's ff_id3v1_genre_str): v2.4 "148", v2.3 "(183)",
#        ID3v1 byte 191.
#   #167 multi-valued artist / album artist / composer: Vorbis comments
#        (repeated fields, adjacent and not) and ID3v2.4 (NUL-separated in
#        one frame, UTF-8, UTF-16 with BOMs, Latin-1 with a trailing NUL)
#        join with ", "; the "... key:" lines are what the library indexes
#        (metadata_keep_primary(), called by tagcache): the first value only.
#        ffprobe shows the Vorbis values joined with ";" and only the first
#        ID3v2.4 value (it reads one); the ID3 bytes are in the generator.
# Needs a built warble (scripts/test.sh builds it); WARBLE= overrides.
set -u
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
cd "$ROOT"
if [ -z "${WARBLE:-}" ]; then
    for w in build/warble/warble.*; do
        [ -x "$w" ] && WARBLE=$w && break
    done
fi
if [ -z "${WARBLE:-}" ] || [ ! -x "$WARBLE" ]; then
    echo "warble not found; build it first (scripts/test.sh)" >&2
    exit 2
fi
TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT

A1='Aardvark Multi'
A2='Aardwolf Second'
A3='Abacus Third With A Rather Long Name'
cat >"$TMP/want" <<EOF
g148.mp3|Genre: Abstract
g183.mp3|Genre: Audiobook
g191.mp3|Genre: Psybient
mv.flac|Artist: $A1, $A2
mv.flac|Album artist: $A1, $A2
mv.flac|Composer: Comp One, Comp Two
mv.flac|Genre: Rock
mv.flac|Artist key: $A1
mv.flac|Album artist key: $A1
mv.flac|Composer key: Comp One
mv_v24.mp3|Artist: $A1, $A2, $A3
mv_v24.mp3|Album artist: $A1, $A2
mv_v24.mp3|Composer: Comp One, Comp Two
mv_v24.mp3|Genre: Rock
mv_v24.mp3|Artist key: $A1
mv_v24.mp3|Album artist key: $A1
mv_v24.mp3|Composer key: Comp One
EOF

fail=0
n=0
for fixture in g148.mp3 g183.mp3 g191.mp3 mv.flac mv_v24.mp3; do
    if ! "$WARBLE" -m "tests/fixtures/mv/$fixture" >"$TMP/$fixture.txt" \
            2>"$TMP/err.txt"; then
        echo "FAIL $fixture: warble exited non-zero" >&2
        cat "$TMP/err.txt" >&2
        fail=1
    fi
done
while IFS='|' read -r fixture line; do
    n=$((n + 1))
    if ! grep -qxF "$line" "$TMP/$fixture.txt" 2>/dev/null; then
        field=${line%%: *}
        echo "FAIL $fixture: want '$line'; got: $(grep "^$field: " \
            "$TMP/$fixture.txt" 2>/dev/null || echo none)" >&2
        fail=1
    fi
done <"$TMP/want"
[ "$n" -eq 17 ] || { echo "FAIL: checked $n of 17 lines" >&2; exit 1; }
[ "$fail" = 0 ] || exit 1
echo "ok   metatags: $n tag values on 5 fixtures"
