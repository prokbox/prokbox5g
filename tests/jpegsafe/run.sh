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

# B4 (RockWheel review, 2026-11-14): four ways one bad cover hung or
# corrupted the album-art JPEG decoder (apps/recorder/jpeg_load.c) on the
# device path. Our p19 fixes for the same file are under JPEG_JDEC, which
# only bcmfw/jpeg/env/jenv.h defines, so the ARM decoder still had them.
# The four upstream commits, hand-merged:
#   6b6642fcbe  an over-full DHT (162 one-bit codes) writes ~80 KB past
#               fix_huff_tbl()'s lookahead table          -> dht_over.jpg
#   162241602a  a marker length past the end of an Ogg picture spins the
#               base64 skip for ever                        -> spin.ogg
#   3be791bc1d  a segment shorter than what was read of it skips backwards
#               and parsing goes on from a misplaced position -> neglen.jpg
#   473b827ea1  a sampling factor of 0 divides by zero      -> samp0.jpg
#
# tests/jpegsafe/craft.py makes the files from one small baseline JPEG
# (PIL) and the Ogg art fixtures' writer; tests/jpegsafe/jpegsafe.c calls
# clip_jpeg_fd() as the art readers do (size probe, then a decode into a
# buffer of exactly that size), for a plain file and for the Ogg base64
# picture. jpeg_load.c is built twice, as tests/oggart does: the stock
# 16-byte reads and the shell's 64 KiB bulk reads (HAVE_IPOD_UI), with
# UBSan (array bounds included) and, with Homebrew's llvm clang, ASan too,
# errors fatal (Apple clang's ASan hangs in its own start-up on macOS 26,
# see bcmfw/tests/jpeg_oob_test.py; without llvm the run says so and is
# UBSan only). The driver also keeps a 64 KiB guard after the decode
# buffer and checks it. SANCC/SANFLAGS override. Each run has 20 s.
#
# Every crafted file must come back refused (probe rc <= 0) with no
# sanitizer report, no write past the buffer and no timeout, in both
# builds; base.jpg and base.ogg must decode to 64x48. Before the fixes
# (p090-int3): dht_over.jpg writes look_nbits[256] (UBSan bounds), samp0.jpg
# divides by zero, neglen.jpg reads buf[-152] in the 16-byte build (in the
# bulk build it is refused, -7, after parsing on from inside the Huffman
# table), spin.ogg times out in both.
#
# UBSan's shift-base check is off: HUFF_EXTEND's (-1 << s) is undefined in
# C on every file, upstream's too, and defined on the ARM compiler; it is
# not one of these bugs (listed in docs/upstream-backports.md).
#
# Needs clang with -fsanitize=undefined (Homebrew llvm for ASan), python3
# with PIL, ffmpeg (the Ogg audio) and a built warble (scripts/test.sh).
set -u
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
cd "$ROOT" || exit 1
WARBLEDIR=${WARBLEDIR:-$ROOT/build/warble}
LLVM=/opt/homebrew/opt/llvm/bin/clang
if [ -n "${SANCC:-}" ]; then CC=$SANCC; ASAN=
elif [ -x "$LLVM" ]; then CC=$LLVM; ASAN="-fsanitize=address"
else CC=cc; ASAN=; echo "note: no Homebrew llvm clang: UBSan and the guard only, no ASan"
fi
T=$ROOT/build/jpegsafe
rm -rf "$T"
mkdir -p "$T"
fail=0
ok()  { echo "ok   $*"; }
bad() { echo "FAIL $*"; fail=1; }

[ -f "$WARBLEDIR/lib/librbcodec.a" ] || { echo "FAIL: no warble build at $WARBLEDIR"; exit 1; }
python3 tests/jpegsafe/craft.py "$T/in" >"$T/craft.log" 2>&1 \
    || { echo "FAIL: crafting the files:"; cat "$T/craft.log"; exit 1; }

printf 'printcf:\n\t@echo CF: $(CFLAGS)\n' >"$T/print.mk"
# warble's flags, less what clang does not take and the two string defines
FL=$(make --no-print-directory -C "$WARBLEDIR" -f Makefile -f "$T/print.mk" printcf \
     | sed -n 's/^CF: //p' | sed -E -e 's#-DTARGET_NAME=[^ ]*##' -e 's#-DCODECDIR=[^ ]*##' \
       -e 's#-Wimplicit-fallthrough=0##' -e 's#-Wno-override-init##')
[ -n "$FL" ] || { echo "FAIL: no CFLAGS from $WARBLEDIR"; exit 1; }
SAN=${SANFLAGS:-"$ASAN -fsanitize=undefined -fno-sanitize=shift-base -fno-sanitize-recover=all -fno-omit-frame-pointer -O1 -D_FORTIFY_SOURCE=0"}
export ASAN_OPTIONS=detect_leaks=0
echo 'void yield(void);' >"$T/yield.h"
W=$WARBLEDIR
# shellcheck disable=SC2086
eval "$CC" $FL $SAN -I"$ROOT/apps/recorder" -w -c tests/jpegsafe/jpegsafe.c -o "$T/drv.o" \
    || { echo "FAIL: compiling the driver"; exit 1; }
for f in resize bmp; do
    # shellcheck disable=SC2086
    eval "$CC" $FL -I"$ROOT/apps/recorder" -w -c "apps/recorder/$f.c" -o "$T/$f.o" \
        || { echo "FAIL: compiling $f.c"; exit 1; }
done
for v in small bulk; do
    X=; [ $v = bulk ] && X=-DHAVE_IPOD_UI
    # shellcheck disable=SC2086
    eval "$CC" $FL $X $SAN -I"$ROOT/apps/recorder" -w -include "$T/yield.h" \
        -c apps/recorder/jpeg_load.c -o "$T/jpeg-$v.o" \
        || { echo "FAIL: compiling jpeg_load.c ($v)"; exit 1; }
    "$CC" $SAN -o "$T/jpegsafe-$v" "$T/drv.o" "$T/jpeg-$v.o" "$T/resize.o" \
        "$T/bmp.o" "$W/firmware/common/strmemccpy.o" "$W/firmware/common/strlcpy.o" \
        "$W/firmware/common/unicode.o" "$W/firmware/common/pathfuncs.o" \
        "$W/lib/librbcodec.a" "$W/lib/libfixedpoint.a" 2>"$T/ld-$v.log" \
        || { echo "FAIL: linking ($v)"; cat "$T/ld-$v.log"; exit 1; }
done

# run NAME ARGS...: prints the outcome word: hang, sanitizer, crash,
# refused, decoded:WxH (from the decode line), or harness
run() {
    log=$1; shift
    timeout 20 "$@" >"$log" 2>&1
    rc=$?
    if [ $rc = 124 ]; then echo hang
    elif grep -qE "ERROR: AddressSanitizer|runtime error:" "$log"; then echo sanitizer
    elif grep -q "^GUARD" "$log"; then echo overrun
    elif [ $rc != 0 ]; then
        [ $rc = 2 ] && echo harness || echo "crash(rc=$rc)"
    elif grep -q "^probe rc=-\|^probe rc=0 " "$log"; then echo refused
    elif d=$(sed -n 's/^decode rc=[1-9][0-9]* \([0-9]*x[0-9]*\)$/\1/p' "$log") && [ -n "$d" ]; then
        echo "decoded:$d"
    else echo "refused-late"
    fi
}

checked=0
for v in small bulk; do
    B=$T/jpegsafe-$v
    for c in base.jpg dht_over.jpg samp0.jpg neglen.jpg base.ogg spin.ogg; do
        case $c in *.ogg) args="-o $T/in/$c" ;; *) args="$T/in/$c" ;; esac
        # shellcheck disable=SC2086
        got=$(run "$T/$v-$c.log" "$B" $args)
        case $c in base.*) want=decoded:64x48 ;; *) want=refused ;; esac
        detail=$(grep -m1 -E "^(probe|decode) rc=|^GUARD|ERROR: AddressSanitizer|runtime error:" "$T/$v-$c.log" | sed 's#.*/apps/recorder/##' | cut -c1-140)
        if [ "$got" = "$want" ]; then
            ok "$v $c: $got ($detail)"
        else
            bad "$v $c: $got, want $want ($detail)"
        fi
        checked=$((checked + 1))
    done
done
echo "jpegsafe: $checked runs checked"
[ "$checked" = 12 ] || bad "expected 12 runs, checked $checked"
[ $fail -eq 0 ] && echo "jpegsafe: all passed" || { echo "jpegsafe: FAILED"; exit 1; }
