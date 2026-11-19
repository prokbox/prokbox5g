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

# #318: 32-bit FLAC plays.  Until 0.9.0 the codec refused any stream wider
# than 29 bits at open (#285); now it decodes it at its own width and
# reduces it to 29 bits with TPDF dither (libffmpegFLAC/dither.h).  With
# build/warble's real codec (flac.c, the host's C paths of decoder.c):
#  1. 32-bit stereo (flac -8, music and noise), 32-bit mono, and 30- and
#     31-bit streams (tests/flacarm/synth.py) come out of the codec exactly
#     as tests/flacarm/dither.py reduces flac -d's samples (the reference
#     decoder up to the reduction, then the reduction written a second
#     time, independently);
#  2. a 24-bit stream is untouched: flac -d's samples shifted up 5 bits;
#  3. a 32-bit stream above the real-time limit (96 kHz stereo) is refused
#     at open with its log line and no PCM;
#  4. dither_stats.c: the dither's statistics (TPDF), extremes and seed.
# tests/flacarm checks the same decoder bit for bit on the device's ARM code.
#   WARBLEDIR  a built warble (default build/warble; scripts/test.sh builds it)
set -u
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
cd "$ROOT" || exit 1
WARBLEDIR=${WARBLEDIR:-$ROOT/build/warble}
W=$WARBLEDIR/warble.ipodvideo
[ -x "$W" ] || { echo "FAIL: no warble at $W"; exit 1; }
command -v flac >/dev/null 2>&1 || { echo "FAIL: flac32 needs flac (the reference decoder)"; exit 1; }
TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT

fail=0
checked=0
bad() { echo "FAIL flac32: $*"; fail=1; }

enc() {     # NAME BITS RATE CHANNELS SECONDS KIND: flac -8 of gen.py's signal
    python3 tests/flacarm/gen.py "$TMP/$1.st" "$2" "$3" "$5" "$6" || return 1
    if [ "$4" = 1 ]; then       # the left channel alone
        python3 -I -c '
import sys
d = open(sys.argv[1], "rb").read()
w = int(sys.argv[3]) // 8
open(sys.argv[2], "wb").write(b"".join(d[i:i + w] for i in range(0, len(d), 2 * w)))
' "$TMP/$1.st" "$TMP/$1.src" "$2" || return 1
    else
        mv "$TMP/$1.st" "$TMP/$1.src"
    fi
    flac -s -f -8 --force-raw-format --endian=little --sign=signed \
        --channels="$4" --bps="$2" --sample-rate="$3" -o "$TMP/$1.flac" "$TMP/$1.src"
}

ref() {     # NAME BITS: flac -d's samples, 4 bytes each
    case $2 in
    32) flac -s -f -d --force-raw-format --endian=little --sign=signed \
            -o "$TMP/$1.ref" "$TMP/$1.flac" 2>/dev/null ;;
    *)  flac -s -f -d -o "$TMP/$1.wav" "$TMP/$1.flac" 2>/dev/null \
            && python3 tests/flacarm/wav2raw.py "$TMP/$1.wav" "$TMP/$1.ref" "$2" ;;
    esac
}

# 1. wide streams: the codec's PCM against dither.py of the reference
wide() {    # NAME BITS CHANNELS
    ref "$1" "$2" || { bad "$1: reference decode"; return; }
    flac -s -f -a -o "$TMP/$1.ana" "$TMP/$1.flac" 2>/dev/null || { bad "$1: analyze"; return; }
    python3 tests/flacarm/dither.py "$TMP/$1.ref" "$TMP/$1.ana" "$2" "$3" "$TMP/$1.exp" \
        || { bad "$1: dither.py"; return; }
    rm -f "$TMP/$1.out"
    "$W" -r "$TMP/$1.flac" "$TMP/$1.out" >"$TMP/$1.log" 2>&1
    if [ ! -s "$TMP/$1.out" ]; then
        bad "$1: no PCM ($(grep -E 'flac|error' "$TMP/$1.log" | head -2 | tr '\n' ' '))"
    elif ! cmp -s "$TMP/$1.out" "$TMP/$1.exp"; then
        bad "$1: PCM differs from dither.py ($(cmp "$TMP/$1.out" "$TMP/$1.exp" 2>&1 | head -1))"
    else
        n=$(( $(wc -c <"$TMP/$1.out") / 4 / $3 ))
        echo "     $1: $n samples x $3, as the reference reduced with dither.py"
        checked=$((checked + 1))
    fi
}

enc s32_44 32 44100 2 1 music && wide s32_44 32 2 || bad "s32_44: encoder"
enc s32_48n 32 48000 2 0.5 noise && wide s32_48n 32 2 || bad "s32_48n: encoder"
enc m32_44 32 44100 1 0.5 music && wide m32_44 32 1 || bad "m32_44: encoder"
python3 tests/flacarm/synth.py "$TMP/syn31.flac" 31 1152 12 31 rate=48000 && wide syn31 31 2 || bad "syn31: synth.py"
python3 tests/flacarm/synth.py "$TMP/syn30.flac" 30 1152 12 30 rate=44100 && wide syn30 30 2 || bad "syn30: synth.py"

# 2. a 24-bit stream: unchanged, the reference's samples << 5
if enc s24_44 24 44100 2 0.5 music && ref s24_44 24; then
    "$W" -r "$TMP/s24_44.flac" "$TMP/s24_44.out" >"$TMP/s24_44.log" 2>&1
    python3 -I -c '
import struct, sys
a = open(sys.argv[1], "rb").read()
b = open(sys.argv[2], "rb").read()
n = len(b) // 3
r = [int.from_bytes(b[3 * i:3 * i + 3], "little", signed=True) << 5 for i in range(n)]
sys.exit(0 if n and a == struct.pack("<%di" % n, *r) else 1)
' "$TMP/s24_44.out" "$TMP/s24_44.ref" \
        && { echo "     s24_44: the 24-bit path is unchanged (reference << 5)"; checked=$((checked + 1)); } \
        || bad "s24_44: 24-bit PCM is not the reference << 5"
else
    bad "s24_44: encoder"
fi

# 3. above the limit: refused at open, logged, no PCM
if enc s32_96 32 96000 2 0.25 music; then
    rm -f "$TMP/s32_96.out"
    "$W" -r "$TMP/s32_96.flac" "$TMP/s32_96.out" >"$TMP/s32_96.log" 2>&1
    if [ -s "$TMP/s32_96.out" ]; then
        bad "s32_96: 96 kHz 32-bit stereo played (over FLAC_WIDE_MAX_RATE)"
    elif ! grep -q "flac: 32-bit at 96000 Hz x2 too slow" "$TMP/s32_96.log"; then
        bad "s32_96: no refusal log line ($(grep -E 'flac|error' "$TMP/s32_96.log" | head -2 | tr '\n' ' '))"
    else
        echo "     s32_96: refused at open, logged: $(grep 'too slow' "$TMP/s32_96.log")"
        checked=$((checked + 1))
    fi
else
    bad "s32_96: encoder"
fi

# 4. the dither's statistics
if cc -O2 -Wall -Wextra -I lib/rbcodec/codecs/libffmpegFLAC -o "$TMP/dither_stats" \
        tests/flac32/dither_stats.c -lm; then
    out=$("$TMP/dither_stats") && { echo "$out" | sed 's/^/  /'; checked=$((checked + 1)); } \
        || { echo "$out"; bad "dither statistics"; }
else
    bad "dither_stats.c does not build"
fi

[ "$checked" = 8 ] || bad "$checked of 8 checks ran to a verdict"
[ $fail = 0 ] || exit 1
echo "ok   32-bit FLAC: decoded and reduced with dither, as the reference (tests/flac32)"
