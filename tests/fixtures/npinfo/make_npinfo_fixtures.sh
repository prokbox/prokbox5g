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

# Now Playing info page (#277) fixtures under build/npinfo-fixtures
# (idempotent, generated tones only):
#   cd16.mp3    30 s MP3, 44.1 kHz, 16-bit source, 256 kb/s
#   hires.flac  30 s FLAC, 192 kHz, 24-bit
#   aac.m4a     30 s AAC-LC, 44.1 kHz, 192 kb/s
# Run before scripts/shots.sh tests/ui/ipodui-np-info.script.
set -eu
ROOT=$(cd "$(dirname "$0")/../../.." && pwd -P)
OUT="$ROOT/build/npinfo-fixtures"
VERSION=1
command -v ffmpeg >/dev/null || { echo "make_npinfo_fixtures: ffmpeg not found" >&2; exit 1; }
[ "$(cat "$OUT/.version" 2>/dev/null || true)" = "$VERSION" ] && exit 0
mkdir -p "$OUT"
T="$OUT/.tmp.$$"
mkdir -p "$T"
src() { echo "sine=frequency=$1:duration=30:sample_rate=$2"; }
ffmpeg -v error -y -f lavfi -i "$(src 440 44100)" -ac 2 -c:a libmp3lame -b:a 256k \
    -metadata title="Sixteen Forty-Four" -metadata artist="Info Fixtures" -metadata album="Quality" "$T/cd16.mp3"
ffmpeg -v error -y -f lavfi -i "$(src 523 192000)" -ac 2 -sample_fmt s32 -c:a flac \
    -metadata title="Twenty-Four 192" -metadata artist="Info Fixtures" -metadata album="Quality" "$T/hires.flac"
ffmpeg -v error -y -f lavfi -i "$(src 660 44100)" -ac 2 -c:a aac -b:a 192k \
    -metadata title="Advanced Coding" -metadata artist="Info Fixtures" -metadata album="Quality" "$T/aac.m4a"
mv "$T"/cd16.mp3 "$T"/hires.flac "$T"/aac.m4a "$OUT"/
rmdir "$T"
echo "$VERSION" > "$OUT/.version"
