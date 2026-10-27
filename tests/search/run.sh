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

# Host tests of Phase 11 Search (docs/phase11-brief.md 3.1, 4.2, 4.3):
#  1. apps/ipodui/search_core.c alone: the fold, word-prefix matching over
#     a corpus (accents, "The", multi-word, UTF-8), srch_extends();
#  2. lib_search() and lib_stats() in apps/ipodui/library.c, over the
#     fake tagcache of tests/ipodui (tests/search/stub in front of its
#     libstub: tag_bitrate, ROCKBOX_DIR, the strip): order across kinds,
#     refine in place, the cap and "More...", generation aborts, no pool
#     growth while playing, the stats section and its sequence counter.
# Compiler: the newest Homebrew gcc-NN, else cc; override with HOSTCC=...
set -u
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
cd "$ROOT"
T=tests/search

if [ -z "${HOSTCC:-}" ]; then
    HOSTCC=cc
    for v in 19 18 17 16 15 14; do
        if command -v gcc-$v >/dev/null 2>&1; then HOSTCC=gcc-$v; break; fi
    done
fi

TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT
# shellcheck disable=SC2086
if ! "$HOSTCC" -std=gnu99 -O1 -g -Wall -Wextra -Werror ${EXTRA_CFLAGS:-} \
        -Iapps -Iapps/ipodui -o "$TMP/test_match" "$T/test_match.c" \
        apps/ipodui/search_core.c apps/ipodui/sortkey.c 2>"$TMP/cc.log"; then
    echo "FAIL: compiling search_core.c on the host ($HOSTCC)"
    cat "$TMP/cc.log"
    exit 1
fi
if "$TMP/test_match"; then
    echo "ok   search matching (search_core.c: fold, word prefix, corpus)"
else
    echo "FAIL: search matching (search_core.c)"
    exit 1
fi

SRC="apps/ipodui/library.c apps/ipodui/sortkey.c apps/ipodui/artkey.c"
SRC="$SRC apps/ipodui/artfmt.c apps/ipodui/search_core.c"
# shellcheck disable=SC2086
if ! "$HOSTCC" -std=gnu99 -O1 -g -Wall -Wextra -Werror ${EXTRA_CFLAGS:-} \
        -I$T/stub -Itests/ipodui/libstub -Itests/ipodui -DARTFMT_HOST \
        -DLIB_P11_HOST -Iapps -Iapps/ipodui -o "$TMP/test_libsearch" \
        "$T/test_libsearch.c" tests/ipodui/fake_tagcache.c $SRC \
        2>"$TMP/cc.log"; then
    echo "FAIL: compiling library.c search on the host ($HOSTCC)"
    cat "$TMP/cc.log"
    exit 1
fi
if (cd "$TMP" && mkdir -p db && ./test_libsearch db); then
    echo "ok   lib_search / lib_stats (library.c over the fake tagcache)"
else
    echo "FAIL: lib_search / lib_stats (library.c)"
    exit 1
fi
