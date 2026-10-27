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

# Host tests for the Now Playing screen's pure drawing code (Phase 5, #4):
#   apps/ipodui/np_draw.c   -> test_draw.c (slant, fit, reflection, bars,
#                              labels, bar span; ticks via np_scrub.c)
#   apps/ipodui/np_lyrics.c -> test_lyrics.c (wrap)
# Pure C (no Rockbox headers). NPSCREEN_PREVIEW=<dir> also writes PPM
# previews of the slanted art (square, 16:9, 2:3, 4:1, placeholder).
# Compiler: the newest Homebrew gcc-NN, else cc; override with HOSTCC=...
set -u
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
cd "$ROOT"
T=tests/npscreen
S=apps/ipodui

if [ -z "${HOSTCC:-}" ]; then
    HOSTCC=cc
    for v in 19 18 17 16 15 14; do
        if command -v gcc-$v >/dev/null 2>&1; then HOSTCC=gcc-$v; break; fi
    done
fi

TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT
fail=0

run() {
    name=$1; shift
    # shellcheck disable=SC2068
    if ! "$HOSTCC" -std=gnu99 -O1 -g -Wall -Wextra -Werror ${EXTRA_CFLAGS:-} -I$S -I$T \
            -o "$TMP/$name" $@ -lm 2>"$TMP/cc.log"; then
        echo "FAIL: compiling $name ($HOSTCC)"
        cat "$TMP/cc.log"
        fail=1
        return
    fi
    if "$TMP/$name"; then
        echo "ok   $name"
    else
        echo "FAIL: $name"
        fail=1
    fi
}

run test_draw "$T/test_draw.c" "$S/np_draw.c" "$S/np_scrub.c"
run test_lyrics "$T/test_lyrics.c" "$S/np_lyrics.c"
# P6: the whole-book bar (snap union, ticks rescale, counter entry)
run test_book "$T/test_book.c" "$S/np_scrub.c" "$S/ab_book_core.c" lib/rbcodec/metadata/abpack.c \
    -Ilib/rbcodec/metadata -DWARBLE

exit $fail
