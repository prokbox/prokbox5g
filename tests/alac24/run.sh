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

# B2 (RockWheel review, 2026-11-14): 24-bit ALAC decoded at about 16-bit
# precision. A 24-bit ALAC frame stores the low byte of every sample
# uncompressed ahead of the Rice data; libalac read those bytes into the
# output buffers and the entropy decoder then wrote over them. Upstream
# 15d73677bc keeps them in a buffer of their own.
#
# For 24-bit stereo and mono (ffmpeg's ALAC encoder; it writes the low
# byte uncompressed and uses weighted mid-side stereo on the correlated
# stretches) and a 16-bit stereo control, warble's raw codec output must
# equal the encoder's input sample for sample, and ffmpeg's own ALAC
# decode must give the input back too (so the fixture is lossless and the
# reference agrees). Before the fix nearly every 24-bit sample's low byte
# differs.
#
# Needs ffmpeg and ffprobe (the fixtures' encoder and checker) and a built
# warble (scripts/test.sh).
set -e
cd "$(dirname "$0")/../.."

WARBLE=${WARBLE:-}
if [ -z "$WARBLE" ]; then
    for w in build/warble/warble.*; do
        [ -x "$w" ] && WARBLE=$w && break
    done
fi
[ -n "$WARBLE" ] && [ -x "$WARBLE" ] || {
    echo "warble not found; build it first (scripts/test.sh)" >&2; exit 1; }
for t in ffmpeg ffprobe; do
    command -v $t >/dev/null 2>&1 || {
        echo "alac24: FAILED: $t not found (the fixtures' encoder)" >&2; exit 1; }
done

T=build/alac24
rm -rf "$T"
mkdir -p "$T"
A="python3 tests/alac24/alac24.py"
fail=0
checked=0
ok()  { echo "ok   $*"; }
bad() { echo "FAIL $*"; fail=1; }

# name bits channels rate frames (several 4096-sample ALAC frames, a short last)
for spec in "24st 24 2 44100 30000" "24mono 24 1 48000 20000" \
            "24st96 24 2 96000 30000" "16st 16 2 44100 20000"; do
    set -- $spec
    name=$1 bits=$2 ch=$3 rate=$4 n=$5
    src=$T/$name.raw
    f=$T/$name.m4a
    [ "$bits" = 16 ] && pcm=s16le || pcm=s24le
    $A gen "$src" "$bits" "$ch" "$n"
    ffmpeg -nostdin -v error -y -f $pcm -ar "$rate" -ac "$ch" -i "$src" \
        -c:a alac "$f"
    info=$(ffprobe -v error -select_streams a:0 -show_entries \
        stream=codec_name,bits_per_raw_sample,channels,sample_rate \
        -of default=nw=1:nk=1 "$f" | tr '\n' ' ')
    case "$info" in
        "alac $rate $ch $bits "*) ;;
        *) bad "$name: the fixture is not what it should be: $info"; continue ;;
    esac
    ffmpeg -nostdin -v error -y -i "$f" -f $pcm "$T/$name.ref"
    if r=$($A samepcm "$src" "$T/$name.ref"); then
        ok "$name reference (ffmpeg decode): $r"
    else
        bad "$name reference (ffmpeg decode): $r"
    fi
    if "$WARBLE" -r "$f" "$T/$name.out" >"$T/$name.log" 2>&1 \
            && r=$($A same "$src" "$bits" "$ch" "$T/$name.out"); then
        ok "$name ($info): $r"
    else
        bad "$name ($info): ${r:-$(tail -2 "$T/$name.log" | tr '\n' ' ')}"
    fi
    checked=$((checked + 1))
done

echo "alac24: $checked streams checked"
[ "$checked" = 4 ] || bad "expected 4 streams, checked $checked"
[ $fail -eq 0 ] && echo "alac24: all passed" || { echo "alac24: FAILED"; exit 1; }
