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

# Settings > Audiophile host tests (#268, #270):
#   1. the output-rate rule's goldens for every source rate 8..384 kHz with
#      the high rates off, on, and each alone; the depth rule; a road-test
#      config's saved "on" through the 0.8.3 build (nothing allowed: 48 kHz
#      24-bit) and the 0.9.0 one (96 kHz);
#   2. bit-exact 24-bit passthrough at unity through warble's real codecs
#      and DSP output stage (depth=24): 24-bit WAV and FLAC come out as the
#      source in the top 24 bits of each word; a 16-bit source as its
#      samples << 16.
# Needs a built warble (scripts/test.sh); FLAC cases need ffmpeg.
set -u
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
cd "$ROOT"
fail=0

if [ -z "${HOSTCC:-}" ]; then
    HOSTCC=cc
    for v in 19 18 17 16 15 14; do
        if command -v gcc-$v >/dev/null 2>&1; then HOSTCC=gcc-$v; break; fi
    done
fi
T=build/audiophile
rm -rf "$T"
mkdir -p "$T"
# Twice: as 0.8.3 builds it (IPODUI_AUDIOPHILE off, no HAVE_HIRATE_OUT:
# a saved road-test "on" allows nothing) and as 0.9.0 will (on)
for arm in off on; do
    defs=-DAUDIOPHILE_HOST_TEST
    [ $arm = on ] && defs="$defs -DHAVE_HIRATE_OUT"
    # shellcheck disable=SC2086
    if ! "$HOSTCC" -std=gnu99 -O1 -Wall -Wextra -Werror -Iapps $defs \
            -o "$T/test_rate_$arm" tests/audiophile/test_rate.c \
            apps/audiophile_core.c 2>"$T/cc.log"; then
        echo "FAIL: compiling test_rate ($arm)"; cat "$T/cc.log"; exit 1
    fi
    if out=$("$T/test_rate_$arm"); then
        echo "ok   output-rate and depth rules, Audiophile $arm: $(echo "$out" | tail -1)"
    else
        echo "$out"; echo "FAIL output-rate rule (Audiophile $arm)"; fail=1
    fi
done

WARBLE=${WARBLE:-}
if [ -z "$WARBLE" ]; then
    for w in build/warble/warble.*; do [ -x "$w" ] && WARBLE=$w && break; done
fi
if [ -z "$WARBLE" ] || [ ! -x "$WARBLE" ]; then
    echo "FAIL: warble not found; build it first (scripts/test.sh)"; exit 1
fi
BP="python3 tests/bitperfect/bp.py"
AP="python3 tests/audiophile/ap.py"

check() {
    name=$1; shift
    if out=$("$@"); then
        echo "ok   $name: $out"
    else
        echo "FAIL $name: $out"; fail=1
    fi
}

$BP gen "$T/n24.wav" 44100 24 2 24
$BP gen "$T/n24_48.wav" 48000 24 2 48
$BP gen "$T/n16.wav" 44100 16 2 16
inputs="$T/n24.wav $T/n24_48.wav $T/n16.wav"
if command -v ffmpeg >/dev/null 2>&1; then
    for f in n24 n24_48; do
        ffmpeg -nostdin -v error -y -i "$T/$f.wav" -c:a flac \
            -sample_fmt s32 "$T/$f.flac"
        inputs="$inputs $T/$f.flac"
    done
else
    echo "note: ffmpeg not found; FLAC cases skipped"
fi
n=0
for in in $inputs; do
    base=$(basename "$in" | tr . _)
    src=$T/$(basename "$in" | sed 's/\.flac$/.wav/')
    "$WARBLE" -c "depth=24:outrate=0" "$in" "$T/$base.s24.wav" \
        2>>"$T/warble.log"
    check "$base 24-bit out at unity" $AP exact "$src" "$T/$base.s24.wav"
    n=$((n + 1))
done
[ $n -ge 3 ] || { echo "FAIL: only $n passthrough cases ran"; fail=1; }
exit $fail
