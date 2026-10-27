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

# Bit-perfect playback host test (Phase 15, wave 2a; docs/phase15-brief.md
# 3.5). Decodes noise through warble's real codecs and DSP with the
# shell's DSP defaults (EQ off, ReplayGain off, speed 1, output at the
# file's own rate, automatic dither) and checks:
#   1. 16-bit WAV and FLAC at 44.1 and 48 kHz come out sample for sample
#      identical to the input, and forced dither would have changed them;
#   2. a 24-bit source is dithered: automatic == forced dither, and both
#      differ from plain rounding;
#   3. a 16-bit source with a DSP stage active (resampling, timestretch)
#      is dithered: automatic == forced dither.
# FLAC files come from ffmpeg's encoder when ffmpeg is installed; without
# it only the WAV cases run (noted in the output).
#
# Needs a built warble (build/warble, see scripts/test.sh). Note: warble
# runs the C output path; the device runs dsp_arm.S's copy, which does the
# same arithmetic (see sample_output_stereo there).
set -e
cd "$(dirname "$0")/../.."

WARBLE=${WARBLE:-}
if [ -z "$WARBLE" ]; then
    for w in build/warble/warble.*; do
        [ -x "$w" ] && WARBLE=$w && break
    done
fi
if [ -z "$WARBLE" ] || [ ! -x "$WARBLE" ]; then
    echo "warble not found; build it first (scripts/test.sh)" >&2
    exit 1
fi

T=build/bitperfect
rm -rf "$T"
mkdir -p "$T"
BP="python3 tests/bitperfect/bp.py"
fail=0

decode() { # decode IN OUT [config]
    if [ -n "$3" ]; then
        "$WARBLE" -c "$3" "$1" "$2" 2>>"$T/warble.log"
    else
        "$WARBLE" "$1" "$2" 2>>"$T/warble.log"
    fi
}

check() { # check NAME CMD...
    name=$1; shift
    if out=$("$@"); then
        echo "ok   $name: $out"
    else
        echo "FAIL $name: $out"
        fail=1
    fi
}

have_flac=0
if command -v ffmpeg >/dev/null 2>&1; then
    have_flac=1
else
    echo "note: ffmpeg not found; FLAC cases skipped, WAV only"
fi

flac_of() { # flac_of IN.wav OUT.flac
    ffmpeg -nostdin -v error -y -i "$1" -c:a flac "$2"
}

# 1. 16-bit sources at their own rate: bit-perfect
for rate in 44100 48000; do
    src=$T/n16_$rate.wav
    $BP gen "$src" $rate 16 3 $rate
    inputs=$src
    if [ $have_flac = 1 ]; then
        flac_of "$src" "$T/n16_$rate.flac"
        inputs="$inputs $T/n16_$rate.flac"
    fi
    for in in $inputs; do
        base=$(basename "$in" | tr . _)
        decode "$in" "$T/$base.auto.wav" outrate=0
        decode "$in" "$T/$base.dith.wav" outrate=0:dither=1
        check "$base bit-perfect" $BP same "$src" "$T/$base.auto.wav"
        check "$base forced dither changes it" \
              $BP differ "$src" "$T/$base.dith.wav" 30
    done
done

# 2. 24-bit sources: dithered
src=$T/n24_44100.wav
$BP gen "$src" 44100 24 3 24
$BP round24 "$src" "$T/n24_round.wav"
inputs=$src
if [ $have_flac = 1 ]; then
    flac_of "$src" "$T/n24_44100.flac"
    inputs="$inputs $T/n24_44100.flac"
fi
for in in $inputs; do
    base=$(basename "$in" | tr . _)
    decode "$in" "$T/$base.auto.wav" outrate=0
    decode "$in" "$T/$base.dith.wav" outrate=0:dither=1
    check "$base auto == dither" $BP same "$T/$base.dith.wav" "$T/$base.auto.wav"
    check "$base dithered, not rounded" \
          $BP differ "$T/n24_round.wav" "$T/$base.auto.wav" 30
done

# 3. 16-bit source with a DSP stage active: dithered
in=$T/n16_48000.wav
decode "$in" "$T/rs.auto.wav"                      # 48 -> 44.1 kHz resampler
decode "$in" "$T/rs.dith.wav" dither=1
check "16-bit resampled: auto == dither" $BP same "$T/rs.dith.wav" "$T/rs.auto.wav"
# timestretch copies whole stretches of input untouched (exact, kept
# bit-perfect) and crossfades between them (inexact, dithered): the
# output must hold both kinds of block
in=$T/n16_44100.wav
decode "$in" "$T/ts.auto.wav" outrate=0:tempo=1.25
decode "$in" "$T/ts.dith.wav" outrate=0:tempo=1.25:dither=1
check "16-bit timestretched: copies exact, dithers crossfades" \
      $BP mixed "$in" "$T/ts.auto.wav" "$T/ts.dith.wav"

[ $fail -eq 0 ] && echo "bitperfect: all passed" || { echo "bitperfect: FAILED"; exit 1; }
