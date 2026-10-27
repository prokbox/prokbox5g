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
# Tests for the AAC-LC decoder on the VideoCore (bcmfw/aac, #60):
#  1. the host reference: Rockbox's libfaad (LC, fixed point) + our adapter,
#     built from bcmfw/aac/SOURCES, decodes every clip (sanitizer build:
#     address, undefined) and writes the PCM and libfaad's 32-bit output;
#     an optimised build must agree byte for byte; aac_reset is checked;
#     a sanity check against ffmpeg's own (float) decode;
#  2. the same sources through vc2cc in vc2sim (aac_sim_test.py), compared
#     byte for byte with the host (skips without vc4-elf-gcc).
# Builds the clips first if they are missing (mkclips.sh).
#
#   bcmfw/aac/test/run.sh             host, then vc2sim (SIM_AUS per clip)
#   bcmfw/aac/test/run.sh --host      host only
#   SIM_AUS=0 bcmfw/aac/test/run.sh   vc2sim on every AU (slow)
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
set -e
ROOT=$(cd "$(dirname "$0")/../../.." && pwd)
TEST="$ROOT/bcmfw/aac/test"
OUT="$ROOT/build/aac"
CLIPS="$OUT/clips"
CC=${CC:-cc}
# Apple clang's AddressSanitizer hangs at start-up on this macOS (see
# bcmfw/h264/test/run.sh); Homebrew's LLVM works.
SANCC=${SANCC:-/opt/homebrew/opt/llvm/bin/clang}
[ -x "$SANCC" ] || SANCC=$CC
SIM_AUS=${SIM_AUS:-12}
mkdir -p "$OUT/san" "$OUT/opt"
[ -f "$CLIPS/sine_st128.au" ] || "$TEST/mkclips.sh"

FAAD="$ROOT/lib/rbcodec/codecs/libfaad"
LIB="$ROOT/lib/rbcodec/codecs/lib"
# the flags vc2cc gets too (aac_sim_test.py): char is unsigned on VC II
CFG="-std=gnu99 -funsigned-char -include $ROOT/bcmfw/aac/aac_config.h \
-I$ROOT/bcmfw/aac/sys -I$FAAD -I$LIB -I$ROOT/lib/rbcodec -I$ROOT/bcmfw/aac"
SRCS=$(grep -v '^#' "$ROOT/bcmfw/aac/SOURCES" | sed "s|^|$ROOT/|")

# build DIR CC FLAGS...: every decoder file with the decoder's environment,
# the harness with the host's own headers
build() {
    dir=$1 cc=$2
    shift 2
    objs=""
    for s in $SRCS; do
        o="$dir/$(basename "$s" .c).o"
        # upstream files are compiled as they are: their warnings are not ours
        case "$s" in
            */bcmfw/*) w="-Wall -Wextra -Werror" ;;
            *) w="-w" ;;
        esac
        # shellcheck disable=SC2086
        $cc $CFG $w "$@" -c "$s" -o "$o"
        objs="$objs $o"
    done
    # shellcheck disable=SC2086
    $cc -std=c99 -Wall -Wextra -Werror "$@" -I"$ROOT/bcmfw/aac" \
        "$TEST/aac_test.c" $objs -o "$dir/aac_test"
}
# Shifts are not checked: libfaad left-shifts negative values (two's
# complement in GCC, documented) and specrec.c's inverse quantiser shifts
# by 32 (exp = -32 in zero/IS/PNS bands); every such shift in the clips
# is of 0 (counted, audio-spike.md M2), so its result is 0 whatever the
# hardware does with the count.
build "$OUT/san" "$SANCC" -O1 -g -fno-omit-frame-pointer \
    -fsanitize=address,undefined -fno-sanitize=shift-base,shift-exponent \
    -fno-sanitize-recover=all
build "$OUT/opt" "$CC" -O2

fail=0
for au in "$CLIPS"/*.au; do
    name=$(basename "$au" .au)
    "$OUT/san/aac_test" "$au" "$OUT/$name.s16" "$OUT/$name.raw" || fail=1
    "$OUT/opt/aac_test" "$au" "$OUT/$name.opt.s16" >/dev/null || fail=1
    cmp -s "$OUT/$name.s16" "$OUT/$name.opt.s16" || {
        echo "$name: -O2 build differs from the sanitizer build"; fail=1; }
done
"$OUT/san/aac_test" --reset "$CLIPS/sine_st128.au" || fail=1
python3 -I "$TEST/refcheck.py" "$OUT" "$CLIPS" || fail=1
[ $fail -eq 0 ] && echo "AAC HOST TESTS PASSED" || { echo "AAC HOST TESTS FAILED"; exit 1; }
[ "$1" = "--host" ] && exit 0

python3 -I "$TEST/aac_sim_test.py" --aus "$SIM_AUS" | tee "$OUT/sim.log" | tail -n 40
tail -n 1 "$OUT/sim.log" | grep -q 'PASSED\|skipped' || { echo "AAC TESTS FAILED"; exit 1; }
echo "AAC TESTS PASSED"
