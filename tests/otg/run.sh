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

# Host tests of the P11 On-The-Go store (docs/phase11-brief.md 3.2, 4.4):
# apps/ipodui/otg.c with library.c over the fake tagcache of tests/ipodui
# (tests/otg/stub and tests/search/stub in front of its libstub), in a
# temporary directory: add, open, resolve (one pass), torn appends,
# remove, clear, save, play, the cap. See test_otg.c.
# Compiler: the newest Homebrew gcc-NN, else cc; override with HOSTCC=...
set -u
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
cd "$ROOT"
T=tests/otg

if [ -z "${HOSTCC:-}" ]; then
    HOSTCC=cc
    for v in 19 18 17 16 15 14; do
        if command -v gcc-$v >/dev/null 2>&1; then HOSTCC=gcc-$v; break; fi
    done
fi

TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT
SRC="apps/ipodui/otg.c apps/ipodui/library.c apps/ipodui/sortkey.c"
SRC="$SRC apps/ipodui/artkey.c apps/ipodui/artfmt.c apps/ipodui/search_core.c"
SRC="$SRC apps/ipodui/safefile.c"
# shellcheck disable=SC2086
if ! "$HOSTCC" -std=gnu99 -O1 -g -Wall -Wextra -Werror ${EXTRA_CFLAGS:-} \
        -I$T/stub -Itests/search/stub -Itests/ipodui/libstub -Itests/ipodui \
        -DARTFMT_HOST -DLIB_P11_HOST -DSAFEFILE_HOST -DOTG_PL_DIR='"pl"' -Iapps \
        -Iapps/ipodui -o "$TMP/test_otg" "$T/test_otg.c" \
        tests/ipodui/fake_tagcache.c $SRC 2>"$TMP/cc.log"; then
    echo "FAIL: compiling otg.c on the host ($HOSTCC)"
    cat "$TMP/cc.log"
    exit 1
fi
if (cd "$TMP" && mkdir -p db && ./test_otg db); then
    echo "ok   On-The-Go store (otg.c: add, resolve, torn, remove, save, play)"
else
    echo "FAIL: On-The-Go store (otg.c)"
    exit 1
fi
