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

# Host tests for the iPod UI shell's pure-C parts (docs/phase3-brief.md).
#
# Compiles apps/ipodui/scroll.c (the scroll.h contract) with the host
# compiler and runs tests/ipodui/test_scroll.c against it, then
# apps/ipodui/screen_stack.c against tests/ipodui/test_bands.c. While
# apps/ipodui/scroll.c does not exist yet, the throwaway reference
# implementation tests/ipodui/ref_scroll.c is tested instead (the output
# says which). Then the sortkey.h contract (docs/phase4-brief.md):
# tests/ipodui/test_sortkey.c and test_sortkey_version.c against
# apps/ipodui/sortkey.c, and the library logic (test_library.c against
# apps/ipodui/library.c over fake_tagcache.c). Compiler: the newest Homebrew gcc-NN (same probe as
# tools/configure), else cc; override with HOSTCC=...
set -u
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
cd "$ROOT"
T=tests/ipodui

if [ -z "${HOSTCC:-}" ]; then
    HOSTCC=cc
    for v in 19 18 17 16 15 14; do
        if command -v gcc-$v >/dev/null 2>&1; then HOSTCC=gcc-$v; break; fi
    done
fi

if [ -f apps/ipodui/scroll.c ] && [ -f apps/ipodui/scroll.h ]; then
    SRC=apps/ipodui/scroll.c
    HDR='"ipodui/scroll.h"'
    INC="-Iapps -Iapps/ipodui"
else
    SRC=$T/ref_scroll.c
    HDR='"ref_scroll.h"'
    INC="-I$T"
fi
echo "   scroll: testing $SRC ($HOSTCC)"

TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT
# shellcheck disable=SC2086
if ! "$HOSTCC" -std=gnu99 -O1 -g -Wall -Wextra -Werror $INC \
        -DSCROLL_H="$HDR" -o "$TMP/test_scroll" "$T/test_scroll.c" "$SRC" \
        2>"$TMP/cc.log"; then
    echo "FAIL: compiling $SRC on the host"
    cat "$TMP/cc.log"
    exit 1
fi
if "$TMP/test_scroll"; then
    echo "ok   scroll contract ($SRC)"
else
    echo "FAIL: scroll contract ($SRC)"
    exit 1
fi

# Dirty-band coalescing of screen_stack.c, with stub config.h/system.h/
# logf.h (tests/ipodui/stub) in front of apps/
if ! "$HOSTCC" -std=gnu99 -O1 -g -Wall -Wextra -Werror -I$T/stub -Iapps \
        -o "$TMP/test_bands" "$T/test_bands.c" apps/ipodui/screen_stack.c \
        2>"$TMP/cc.log"; then
    echo "FAIL: compiling apps/ipodui/screen_stack.c on the host"
    cat "$TMP/cc.log"
    exit 1
fi
if "$TMP/test_bands"; then
    echo "ok   band coalescing (apps/ipodui/screen_stack.c)"
else
    echo "FAIL: band coalescing (apps/ipodui/screen_stack.c)"
    exit 1
fi

# sortkey.h contract (docs/phase4-brief.md): apps/ipodui/sortkey.c. The
# stub config.h/system.h/logf.h come first in case sortkey.c includes them.
SRC=apps/ipodui/sortkey.c
HDR='"ipodui/sortkey.h"'
INC="-I$T/stub -Iapps -Iapps/ipodui"
echo "   sortkey: testing $SRC ($HOSTCC)"
# shellcheck disable=SC2086
if ! "$HOSTCC" -std=gnu99 -O1 -g -Wall -Wextra -Werror $INC \
        -DSORTKEY_H="$HDR" -o "$TMP/test_sortkey" "$T/test_sortkey.c" "$SRC" \
        2>"$TMP/cc.log"; then
    echo "FAIL: compiling $SRC on the host"
    cat "$TMP/cc.log"
    exit 1
fi
if "$TMP/test_sortkey"; then
    echo "ok   sortkey contract ($SRC)"
else
    echo "FAIL: sortkey contract ($SRC)"
    exit 1
fi

# SORTKEY_VERSION guard: the database magic follows SORTKEY_VERSION, so a
# change to ui_sortkey() output must bump it (test_sortkey_version.c).
# shellcheck disable=SC2086
if ! "$HOSTCC" -std=gnu99 -O1 -g -Wall -Wextra -Werror $INC \
        -DSORTKEY_H="$HDR" -o "$TMP/test_sortkey_version" \
        "$T/test_sortkey_version.c" "$SRC" 2>"$TMP/cc.log"; then
    echo "FAIL: compiling $T/test_sortkey_version.c on the host"
    cat "$TMP/cc.log"
    exit 1
fi
if "$TMP/test_sortkey_version"; then
    echo "ok   sortkey version guard ($SRC)"
else
    echo "FAIL: sortkey version guard ($SRC)"
    exit 1
fi

# Library logic (Phase 4 review, F16): apps/ipodui/library.c with
# apps/ipodui/sortkey.c over the fake tagcache of fake_tagcache.c (stub
# headers in tests/ipodui/libstub, in front of apps/): classification,
# grouping and order, the 2,000-track cap, book progress, stale views.
# the art key: agent B's apps/ipodui/artkey.c, or the TEMP stand-in
# and the art builder's album table reader (artfmt.c), which reads the
# precompute file back: library.c's keys == artcache_key() (keys.txt)
SRC="apps/ipodui/library.c apps/ipodui/sortkey.c apps/ipodui/artkey.c"
SRC="$SRC apps/ipodui/artfmt.c"
echo "   library: testing $SRC ($HOSTCC)"
# shellcheck disable=SC2086
if ! "$HOSTCC" -std=gnu99 -O1 -g -Wall -Wextra -Werror -I$T/libstub -I$T \
        -DARTFMT_HOST -DLIB_PIN_HOST -Iapps -Iapps/ipodui -Ilib/rbcodec/metadata \
        -o "$TMP/test_library" "$T/test_library.c" \
        "$T/fake_tagcache.c" $SRC 2>"$TMP/cc.log"; then
    echo "FAIL: compiling apps/ipodui/library.c on the host"
    cat "$TMP/cc.log"
    exit 1
fi
if (cd "$TMP" && mkdir -p db && ./test_library db "$ROOT/tests/artcache/keys.txt"); then
    echo "ok   library logic (apps/ipodui/library.c)"
else
    echo "FAIL: library logic (apps/ipodui/library.c)"
    exit 1
fi
