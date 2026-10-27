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

# Phase 6 contract round-trip tests (docs/phase6-brief.md, "Contracts
# (committed)"):
#   lib/rbcodec/metadata/abpack.c  -> test_abpack (twice: packed, and the
#                                     stock accessor macros)
#   apps/ipodui/ab_store_core.c    -> test_abstore (books.dat)
#   apps/ipodui/ab_book_core.c     -> test_abbook (books.pak/idx, rules)
# Pure C, compiled on their own. Compiler: the newest Homebrew gcc-NN,
# else cc; override with HOSTCC=...; EXTRA_CFLAGS for sanitizers.
set -u
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
cd "$ROOT"
T=tests/abbook
S=apps/ipodui
M=lib/rbcodec/metadata

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
    if ! "$HOSTCC" -std=gnu99 -O1 -g -Wall -Wextra -Werror ${EXTRA_CFLAGS:-} \
            -I$T/stub -I$S -I$M -Itests/nowplaying -o "$TMP/$name" $@ \
            2>"$TMP/cc.log"; then
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

run test_abpack_packed -DWARBLE "$T/test_abpack.c" "$M/abpack.c"
run test_abpack_stock "$T/test_abpack.c" "$M/abpack.c"
run test_abstore -DWARBLE "$T/test_abstore.c" "$S/ab_store_core.c" \
    "$S/ab_book_core.c" "$M/abpack.c"
run test_abstore_rt -DWARBLE -DAB_HOST_TEST -DSAFEFILE_HOST \
    -DSAFEFILE_HOST_INCLUDE='"ab_host.h"' -I$T "$T/test_abstore_rt.c" \
    "$S/ab_store.c" "$S/ab_store_core.c" "$S/ab_book_core.c" "$M/abpack.c" \
    "$S/safefile.c"
run test_abbook -DWARBLE "$T/test_abbook.c" "$S/ab_book_core.c" \
    "$M/abpack.c"

exit $fail
