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
# Host tests for our H.264 decoder (bcmfw/h264): every corpus clip against
# ffmpeg's decode, bit-exact, in a sanitizer build (address, undefined),
# then a fuzz pass; then the vector kernels against the C kernels in
# vc2sim (test/vpu_test.py).  Builds the corpus first if it is missing.
#
#   bcmfw/h264/test/run.sh            all clips + 3000 fuzz cases
#   FUZZ=20000 bcmfw/h264/test/run.sh
#   bcmfw/h264/test/run.sh --fast     optimised build, no sanitizers, no fuzz
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
SRC="$ROOT/bcmfw/h264"
OUT="$ROOT/build/h264"
CORPUS="$ROOT/build/video/corpus"
FUZZ=${FUZZ:-3000}
CC=${CC:-cc}
# Apple clang 17's AddressSanitizer hangs at start-up on this macOS
# (FindDynamicShadowStart); Homebrew's LLVM works.
SANCC=${SANCC:-/opt/homebrew/opt/llvm/bin/clang}
[ -x "$SANCC" ] || SANCC=$CC
mkdir -p "$OUT"

python3 "$SRC/gen_tables.py" --check >/dev/null
python3 "$SRC/gen_vpu.py" --check >/dev/null
[ -f "$CORPUS/a_ionly_qp20.264" ] || "$ROOT/tests/video/make_corpus.sh"

FILES="$SRC/h264.c $SRC/ps.c $SRC/slice.c $SRC/mb.c $SRC/cavlc.c \
$SRC/mvpred.c $SRC/intra.c $SRC/idct.c $SRC/mc.c $SRC/inter.c \
$SRC/deblock.c $SRC/tables.c $SRC/vlc_tables.c $SRC/test/h264_test.c"
WARN="-std=c99 -Wall -Wextra -Wno-unused-parameter -Werror"
if [ "$1" = "--fast" ]; then
    BIN="$OUT/h264_test_fast"
    # shellcheck disable=SC2086
    $CC $WARN -O2 -I"$SRC" $FILES -o "$BIN"
    FUZZ=0
else
    BIN="$OUT/h264_test_san"
    # shellcheck disable=SC2086
    $SANCC $WARN -O1 -g -fno-omit-frame-pointer \
        -fsanitize=address,undefined -fno-sanitize-recover=all \
        -I"$SRC" $FILES -o "$BIN"
fi

fail=0
for clip in "$CORPUS"/*.264; do
    "$BIN" "$clip" "${clip%.264}.yuv" >"$OUT/last.log" 2>&1 || fail=1
    tail -n 3 "$OUT/last.log"
done
# the split layout (context and frames apart, as bcmfw lays them out)
for clip in d_ipod_768k e_vga g_crop; do
    "$BIN" --split "$CORPUS/$clip.264" "$CORPUS/$clip.yuv" >"$OUT/last.log" 2>&1 || fail=1
    sed -n '$s/^/split: /p' "$OUT/last.log"
done
if [ "$FUZZ" -gt 0 ]; then
    "$BIN" --fuzz "$FUZZ" 2722 "$CORPUS"/*.264 || fail=1
fi
# the vector kernels against the C ones, in vc2sim (skips without vc4-elf-gcc)
python3 "$SRC/test/vpu_test.py" | tail -n 1 | grep -q 'PASSED\|skipped' || fail=1
[ $fail -eq 0 ] && echo "H264 TESTS PASSED" || { echo "H264 TESTS FAILED"; exit 1; }
