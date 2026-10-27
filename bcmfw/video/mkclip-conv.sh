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

# The device test's converted clip (bcmfw/video/stage7-conv.m4v): what
# scripts/prokbox-convert.py makes of real footage, so stage 7 (the
# player's controls) and the bench's pace row (vfeed) play exactly the
# converter's recipe.  40 s (4.0-44.0 s) of the Sintel trailer,
# (c) Blender Foundation, durian.blender.org, licensed CC BY 3.0
# (https://creativecommons.org/licenses/by/3.0/); credited in README.md
# and tests/video/README.md.  The excerpt is cut at near-lossless quality,
# then converted as a movie with --fill (centre-cropped to 4:3, as
# clip2.mp4) and --bitexact-test (one thread, bit-exact flags): 320x240,
# the source's 24 fps, H.264 Constrained Baseline, about 570 kbit/s, AAC.
# buildzip.pl copies it to .rockbox/bcmtest/stage7.m4v.
#
# The source is never committed.  Fetch it into a scratch directory of
# its own and pass its path; the script checks its hash first:
#   https://download.blender.org/durian/trailer/sintel_trailer-480p.mp4
#
# Usage: bcmfw/video/mkclip-conv.sh sintel_trailer-480p.mp4 [out.m4v]
#
set -e
SRC=$1
DIR=$(cd "$(dirname "$0")" && pwd)
OUT=${2:-$DIR/stage7-conv.m4v}
SHA=b670602fa00934ca27c4351bb0efe7ea7a07fae57284e44226025eeed7c51254
[ -f "$SRC" ] || { echo "usage: $0 sintel_trailer-480p.mp4 [out.m4v]"; exit 1; }
[ "$(shasum -a 256 "$SRC" | cut -d' ' -f1)" = "$SHA" ] \
    || { echo "mkclip-conv: $SRC is not the expected trailer"; exit 1; }
TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT
ffmpeg -v error -y -ss 4 -t 40 -i "$SRC" \
    -c:v libx264 -crf 12 -preset medium -threads 1 \
    -c:a aac -b:a 160k -map_metadata -1 \
    -fflags +bitexact -flags:v +bitexact -flags:a +bitexact \
    "$TMP/sintel-excerpt.mp4"
python3 "$DIR/../../scripts/prokbox-convert.py" --movie --fill \
    --bitexact-test --title "Stage 7" -j 1 -o "$TMP/out" \
    "$TMP/sintel-excerpt.mp4" >/dev/null
cp "$TMP/out/Movies/Stage 7.m4v" "$OUT"
