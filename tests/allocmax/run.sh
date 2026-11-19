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
# Host test: buflib_alloc_maximum() over a hole before a pinned block (0.9.0 OOM panic).
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

# firmware/buflib_mempool.c and firmware/common/crc32.c as built, twice:
# the developer build's 5-word header (HAVE_BUFLIB_HEAPCHECK,
# tests/heapwalk/stub/config.h) and the release build's 4-word one
# (tests/allocmax/stub-release/config.h, found first), with a panicf that
# the test catches.  Only buflib's and crc32's own headers are copied in
# (firmware/include also holds libc look-alikes the host must not see).
# Compiler: the newest Homebrew gcc-NN, else cc; override with HOSTCC=...
set -u
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
cd "$ROOT"
T=tests/allocmax

if [ -z "${HOSTCC:-}" ]; then
    HOSTCC=cc
    for v in 19 18 17 16 15 14; do
        if command -v gcc-$v >/dev/null 2>&1; then HOSTCC=gcc-$v; break; fi
    done
fi

TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT
mkdir -p "$TMP/inc"
cp firmware/include/buflib.h firmware/include/buflib_mempool.h \
   firmware/include/crc32.h "$TMP/inc/"
fail=0
ran=0
for name in developer release; do
    if [ "$name" = developer ]; then
        fields=5 inc=""
    else
        fields=4 inc="-I$T/stub-release"
    fi
    # upstream calls abs() on intptr_t (32 bits on the iPod): quiet on a
    # 64-bit host
    # shellcheck disable=SC2086
    if ! "$HOSTCC" -std=gnu11 -O1 -g -Wall -Wextra -Werror -Wno-absolute-value \
            ${EXTRA_CFLAGS:-} -DEXPECT_FIELDS="$fields" $inc \
            -Itests/heapwalk/stub -I"$TMP/inc" -o "$TMP/test_$name" \
            "$T/test_allocmax.c" firmware/buflib_mempool.c \
            firmware/common/crc32.c 2>"$TMP/cc.log"; then
        echo "FAIL: compiling test_allocmax, $name header ($HOSTCC)"
        cat "$TMP/cc.log"
        fail=1
        continue
    fi
    ran=$((ran + 1))
    if "$TMP/test_$name" >"$TMP/$name.log" 2>&1 \
            && grep -q '^ok$' "$TMP/$name.log"; then
        echo "ok   alloc_maximum, $name header ($fields words): holes before and around a pinned block still give a block (0.9.0 OOM panic)"
    else
        cat "$TMP/$name.log"
        echo "FAIL: alloc_maximum, $name header"
        fail=1
    fi
done
[ "$ran" -eq 2 ] || { echo "FAIL: allocmax ran $ran of 2 header shapes"; fail=1; }
exit $fail
