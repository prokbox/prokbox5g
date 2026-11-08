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

# The long-play soak's episode (day 4c: pictures stopped about 10 minutes
# into an episode, the audio on).  One file, never committed:
#   build/soak-fixtures/soak.m4v   12 min 9 s, 320x240 H.264 Baseline L1.3
#                                  600k/768k max, GOP 3 s, AAC-LC 128k
#                                  (scripts/prokbox-convert.py's settings)
# made by looping the CC BY clip bcmfw/video/stage6-real.mp4 (13 s, the
# Sintel trailer excerpt, credited in README.md) 56 times and converting
# the loop with the converter itself, so the file is what an owner's
# episode looks like to the player: about 92 KB/s, 67 MB, larger than a
# 64 MB unit's audio buffer.  Idempotent (a stamp over this script, the
# converter and the clip).  Requires ffmpeg (libx264) and ffprobe.
set -eu

ROOT=$(cd "$(dirname "$0")/../../.." && pwd -P)
OUT=$ROOT/build/soak-fixtures
SRC=$ROOT/bcmfw/video/stage6-real.mp4
CONV=$ROOT/scripts/prokbox-convert.py
[ -f "$SRC" ] || { echo "mksoak: missing $SRC" >&2; exit 1; }
mkdir -p "$OUT"

hash_tool() { command -v sha256sum >/dev/null 2>&1 && sha256sum || shasum -a 256; }
KEY=$(cat "$0" "$CONV" "$SRC" | hash_tool | awk '{print $1}')
STAMP=$OUT/.stamp
if [ -f "$STAMP" ] && [ "$(cat "$STAMP")" = "$KEY" ] && [ -f "$OUT/soak.m4v" ]; then
    exit 0
fi
command -v ffmpeg >/dev/null && command -v ffprobe >/dev/null \
    || { echo "mksoak: ffmpeg/ffprobe not found" >&2; exit 1; }

WORK=$OUT/.work
rm -rf "$WORK"; mkdir -p "$WORK"
trap 'rm -rf "$WORK"' EXIT
ffmpeg -nostdin -v error -y -stream_loop 55 -i "$SRC" -c copy "$WORK/loop.mp4"
python3 "$CONV" --tv --show "Soak Show" --season 1 --episode 1 \
    --title "Soak Episode" -j 1 -o "$WORK/out" "$WORK/loop.mp4" >/dev/null
mv "$WORK/out/TV Shows/Soak Show/Season 1/Soak Show S01E01.m4v" "$OUT/soak.m4v"
echo "$KEY" >"$STAMP"
