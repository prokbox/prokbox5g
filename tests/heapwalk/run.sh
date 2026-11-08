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
# Host test of buflib's heap walk (developer builds, sitting 4gh-b5).
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

# firmware/buflib_mempool.c and firmware/common/crc32.c as built, with
# HAVE_BUFLIB_HEAPCHECK (tests/heapwalk/stub/config.h) and a panicf that
# the test catches.  Only buflib's and crc32's own headers are copied in
# (firmware/include also holds libc look-alikes the host must not see).
# Compiler: the newest Homebrew gcc-NN, else cc; override with HOSTCC=...
set -u
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
cd "$ROOT"
T=tests/heapwalk

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
# shellcheck disable=SC2086
# upstream calls abs() on intptr_t (32 bits on the iPod): quiet on a
# 64-bit host
if ! "$HOSTCC" -std=gnu11 -O1 -g -Wall -Wextra -Werror -Wno-absolute-value ${EXTRA_CFLAGS:-} \
        -I$T/stub -I"$TMP/inc" -o "$TMP/test_heapwalk" "$T/test_heapwalk.c" \
        firmware/buflib_mempool.c firmware/common/crc32.c 2>"$TMP/cc.log"; then
    echo "FAIL: compiling test_heapwalk ($HOSTCC)"
    cat "$TMP/cc.log"
    exit 1
fi
if "$TMP/test_heapwalk"; then
    echo "ok   heap walk: good heaps pass; each header corruption caught, the block before named (buflib_walk)"
else
    echo "FAIL: heap walk"
    exit 1
fi
