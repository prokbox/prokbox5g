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

# 0.9.0 package F1, the parsers: sort values (#235) and the unplayable
# reason (#286, #318's too-wide FLAC) through the real metadata parsers,
# compiled as the shell compiles them (HAVE_IPOD_UI). Fixtures:
# tests/fixtures/f1 (tests/fixtures/make_p090f1_fixtures.py; values
# confirmed with ffprobe 9.0.2). Builds build/warble-ui (the host warble
# with -DHAVE_IPOD_UI) when it is missing.
#
#   tests/p090f1/parsers.sh
set -u
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
cd "$ROOT" || exit 2
B=build/warble-ui
if [ ! -f "$B/Makefile" ]; then
    mkdir -p "$B"
    (cd "$B" && ../../tools/configure --target=22 --type=w </dev/null >/dev/null) || exit 2
fi
make -C "$B" -j4 EXTRA_DEFINES="-DDEBUG -DWARBLE -DHAVE_IPOD_UI" \
    >"$B/make.log" 2>&1 || { grep -n error "$B/make.log" | head; exit 2; }
W=$(ls "$B"/warble.* 2>/dev/null | head -1)
[ -x "$W" ] || { echo "FAIL: no $B/warble.*" >&2; exit 2; }
TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT

# reason: 0 plays, 1 DRM, 2 PROTECTED, 3 CODEC (metadata.h)
cat >"$TMP/want" <<'EOF'
sort_v23.mp3|Sort artist: うただひかる
sort_v23.mp3|Sort album: はつこい
sort_v23.mp3|Sort album artist: うただひかる
sort_v23.mp3|Artist: 宇多田ヒカル
sort_v23.mp3|Unplayable: 0
sort_txxx.mp3|Sort artist: Beatles, The
sort_txxx.mp3|Sort album: Abbey Road Alias
sort_txxx.mp3|Sort album artist: Beatles Alias
sort.flac|Sort artist: しいなりんご
sort.flac|Sort album: むざいもらとりあむ
sort.flac|Sort album artist: しいなりんご
sort.m4a|Sort artist: とうきょうじへん
sort.m4a|Sort album: きょういく
sort.m4a|Sort album artist: とうきょうじへん
sort.m4a|Unplayable: 0
plain.m4a|Unplayable: 0
plain.m4a|Codec: AAC
drm.m4p|Unplayable: 1
drm.m4p|Codec: ???
drm.m4p|Title: Plain
book.aax|Unplayable: 2
book.aax|Codec: ???
book.aa|Unplayable: 2
book.aa|Title: Protected Book
book.aa|Artist: Some Author
ac3.m4a|Unplayable: 3
ac3.m4a|Codec: ???
base.mp4|Unplayable: 0
high.mp4|Unplayable: 3
w32_96.flac|Unplayable: 3
w32_48.flac|Unplayable: 0
w24_96.flac|Unplayable: 0
EOF
N_WANT=32

fail=0
for f in tests/fixtures/f1/*; do
    n=${f##*/}
    "$W" -m "$f" >"$TMP/$n.txt" 2>/dev/null || { echo "FAIL $n: warble failed" >&2; fail=1; }
done
n=0
while IFS='|' read -r fx line; do
    n=$((n + 1))
    if ! grep -qxF "$line" "$TMP/$fx.txt" 2>/dev/null; then
        field=${line%%: *}
        echo "FAIL $fx: want '$line'; got: $(grep "^$field: " "$TMP/$fx.txt" 2>/dev/null || echo none)" >&2
        fail=1
    fi
done <"$TMP/want"
[ "$n" -eq "$N_WANT" ] || { echo "FAIL: checked $n of $N_WANT lines" >&2; exit 1; }
[ "$fail" = 0 ] || exit 1
echo "ok   p090f1 parsers: $n values on $(ls tests/fixtures/f1 | wc -l | tr -d ' ') fixtures"
