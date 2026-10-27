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

# P19 19-1c's playback fixtures under build/p19c-fixtures (idempotent):
#   long.flac    120 s FLAC, 44.1 kHz, 16-bit stereo (a tone over noise)
#   hires.flac    60 s FLAC, 96 kHz, 24-bit stereo (hi-res: the LOAD hold)
#   long.mp3     120 s MP3, 44.1 kHz stereo, 192 kb/s (synthesis switch)
#   t1.flac, t2.flac  45 s each, one album, gapless (a track boundary)
# The ipodui-p19c-* scripts copy them into the sim disk with
# "copy ../../build/p19c-fixtures/<f> /<dst>". scripts/shots.sh runs this
# for every ipodui-p19c-* script (or "# needs: p19c"); parallel runs are
# safe: each writes a private directory and renames finished files in.
set -eu
ROOT=$(cd "$(dirname "$0")/../../.." && pwd -P)
OUT="$ROOT/build/p19c-fixtures"
VERSION=1
command -v ffmpeg >/dev/null || { echo "make_p19c_fixtures: ffmpeg not found" >&2; exit 1; }
[ "$(cat "$OUT/.version" 2>/dev/null || true)" = "$VERSION" ] && exit 0
mkdir -p "$OUT"
FIN="$OUT"
OUT="$FIN/.tmp.$$"
rm -rf "$OUT"; mkdir -p "$OUT"
trap 'rm -rf "$OUT"' EXIT
FF="ffmpeg -nostdin -v error -y"
# src <rate> <hz>: a tone over quiet noise (a real decode load, not silence)
src() {
    echo "sine=frequency=$2:sample_rate=$1,aformat=channel_layouts=stereo[a];anoisesrc=color=pink:amplitude=0.03:sample_rate=$1,aformat=channel_layouts=stereo[n];[a][n]amix=inputs=2:normalize=0"
}
$FF -f lavfi -i "$(src 44100 440)" -t 120 -c:a flac -sample_fmt s16 \
    -metadata title="Long FLAC" -metadata artist="P19" -metadata album="P19 Long" \
    "$OUT/long.flac"
$FF -f lavfi -i "$(src 96000 330)" -t 60 -c:a flac -sample_fmt s32 \
    -bits_per_raw_sample 24 \
    -metadata title="Hi-res" -metadata artist="P19" -metadata album="P19 Hires" \
    "$OUT/hires.flac"
$FF -f lavfi -i "$(src 44100 550)" -t 120 -c:a libmp3lame -b:a 192k \
    -id3v2_version 3 \
    -metadata title="Long MP3" -metadata artist="P19" -metadata album="P19 MP3" \
    "$OUT/long.mp3"
n=1
for f in 262 392; do
    $FF -f lavfi -i "$(src 44100 $f)" -t 45 -c:a flac -sample_fmt s16 \
        -metadata title="Track $n" -metadata artist="P19" \
        -metadata album="P19 Album" -metadata track="$n" "$OUT/t$n.flac"
    n=$((n + 1))
done
for f in "$OUT"/*; do mv -f "$f" "$FIN/"; done
echo "$VERSION" >"$OUT/.version"
mv -f "$OUT/.version" "$FIN/.version"
