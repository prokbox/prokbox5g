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

# The track for the Now Playing format preview (tests/ui/docs/preview-npinfo.script):
# a generated 24-bit, 96 kHz FLAC (a soft two-tone chord, 30 s) with made-up
# tags and the synthetic cover tests/fixtures/sim-art/music-cover.jpg embedded.
# No owner media, no real names. Writes build/preview-fixtures/; needs ffmpeg.
#   sh tests/ui/docs/make_preview_fixtures.sh
set -eu
ROOT=$(cd "$(dirname "$0")/../../.." && pwd -P)
OUT="$ROOT/build/preview-fixtures"
VERSION=1
command -v ffmpeg >/dev/null || { echo "make_preview_fixtures: ffmpeg not found" >&2; exit 1; }
[ "$(cat "$OUT/.version" 2>/dev/null || true)" = "$VERSION" ] && exit 0
mkdir -p "$OUT"
T="$OUT/.tmp.$$"
mkdir -p "$T"
ffmpeg -v error -y \
    -f lavfi -i "aevalsrc=0.2*sin(2*PI*440*t)+0.15*sin(2*PI*660*t):s=96000:d=30:c=stereo" \
    -i "$ROOT/tests/fixtures/sim-art/music-cover.jpg" \
    -map 0:a -map 1:v -c:a flac -sample_fmt s32 -bits_per_raw_sample 24 \
    -c:v copy -disposition:v attached_pic \
    -metadata title="Studio Master" -metadata artist="Test Ensemble" \
    -metadata album="High Rate Sessions" "$T/studio96.flac"
cp "$ROOT/tests/fixtures/sim-art/music-cover.jpg" "$T/cover.jpg"
mv "$T/studio96.flac" "$T/cover.jpg" "$OUT"/
rmdir "$T"
echo "$VERSION" > "$OUT/.version"
