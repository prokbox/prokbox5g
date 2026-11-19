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

# MP3 start skip of a whole number of frames (RockWheel 3a9f3e120): a LAME
# encoder delay of 623 makes mpa.c's start skip exactly one layer III frame
# (623 + 529 = 1152). With "framelength < 0" that frame left the skip whole
# and every later frame was skipped too: the track decoded to silence. The
# test crafts the delay into a real MP3's LAME tag (lametag.py) and decodes
# through warble: the metadata parser reads the tag, the codec skips.
# Every delay must lose exactly its own samples against delay 622, and
# none may decode to nothing.
set -u
cd "$(dirname "$0")/../.."

WARBLE=${WARBLE:-}
if [ -z "$WARBLE" ]; then
    for w in build/warble/warble.*; do
        [ -x "$w" ] && WARBLE=$w && break
    done
fi
if [ -z "$WARBLE" ] || [ ! -x "$WARBLE" ]; then
    echo "FAIL mp3skip: warble not found; build it first (scripts/test.sh)"
    exit 1
fi
if ! command -v ffmpeg >/dev/null 2>&1; then
    echo "skip mp3skip (no ffmpeg for the test MP3)"
    exit 0
fi

T=build/mp3skip
rm -rf "$T"
mkdir -p "$T"
LT="python3 -I tests/mp3skip/lametag.py"

# 2 s of a tone, CBR with an Info + LAME tag, no ID3v2
ffmpeg -nostdin -v error -y -f lavfi \
    -i "sine=frequency=440:sample_rate=44100" -ac 2 -t 2 \
    -c:a libmp3lame -b:a 128k -id3v2_version 0 -write_xing 1 "$T/src.mp3" \
    || { echo "FAIL mp3skip: making the test MP3"; exit 1; }

fail=0
frames() { # frames DELAY: the decoded sample frames at that encoder delay
    $LT set "$T/src.mp3" "$T/d$1.mp3" "$1" >/dev/null || return 1
    "$WARBLE" "$T/d$1.mp3" "$T/d$1.wav" >"$T/d$1.log" 2>&1 || return 1
    $LT frames "$T/d$1.wav"
}

ref=$(frames 622) || { echo "FAIL mp3skip: delay 622 did not decode"; exit 1; }
if [ -z "$ref" ] || [ "$ref" -lt 44100 ]; then
    echo "FAIL mp3skip: delay 622 decoded only '$ref' frames (want ~2 s)"
    exit 1
fi
# 623: exactly one frame; 1775: exactly two; 624 and 1200: neither
for d in 623 624 1200 1775; do
    n=$(frames $d) || { echo "FAIL mp3skip: delay $d did not decode"; fail=1; continue; }
    want=$((ref - (d - 622)))
    if [ "$n" != "$want" ]; then
        echo "FAIL mp3skip: delay $d decoded $n frames, want $want (delay 622: $ref)"
        fail=1
    fi
done
if [ $fail = 0 ]; then
    echo "ok   mp3 start skip of whole frames (delays 622-624, 1200, 1775 through warble; $ref frames at 622)"
else
    exit 1
fi
