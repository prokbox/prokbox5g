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

# Host tests for the Now Playing pure logic (Phase 5 prep, issue #4) and
# the library-backed shuffle core (#27):
#   apps/ipodui/np_scrub.c     -> test_np_scrub.c
#   apps/ipodui/np_state.c     -> test_np_state.c (with np_scrub.c)
#   apps/ipodui/shuffle_core.c -> test_shuffle.c
# All three are pure C (no Rockbox headers), so they compile on their own.
# Compiler: the newest Homebrew gcc-NN (same probe as tests/ipodui/run.sh),
# else cc; override with HOSTCC=... Extra flags (e.g. sanitizers):
# EXTRA_CFLAGS="-fsanitize=address,undefined" HOSTCC=cc tests/nowplaying/run.sh
set -u
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
cd "$ROOT"
T=tests/nowplaying
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
            -o "$TMP/$name" $@ 2>"$TMP/cc.log"; then
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

run test_np_scrub "$T/test_np_scrub.c" "$S/np_scrub.c"
run test_np_state "$T/test_np_state.c" "$S/np_state.c" "$S/np_scrub.c"
run test_shuffle "$T/test_shuffle.c" "$S/shuffle_core.c"
# Phase 5 contracts (docs/phase5-brief.md, "Contracts (committed)")
run test_pbcore "$T/test_pbcore.c" "$S/np_pbcore.c" "$S/np_scrub.c" \
    "$S/np_state.c"
run test_shuffle_rec "$T/test_shuffle_rec.c" "$S/lib_shuffle_rec.c" \
    "$S/shuffle_core.c"
run test_np_layout "$T/test_np_layout.c"

exit $fail
