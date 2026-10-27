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

# Host tests for the album art cache (phase4b-brief.md, agent B):
#  - artcache_key() against the golden keys in tests/artcache/keys.txt, and
#    keys.txt against its reference generator gen_keys.py (the spec the
#    Phase 7b Mac tool must match);
#  - the on-disk formats (apps/ipodui/artfmt.c with -DARTFMT_HOST, on real
#    files): CRC convention, headers, index merge/lookup/GC, torn and
#    corrupt files, the album table.
# Compiler: the newest Homebrew gcc-NN, else cc; override with HOSTCC=...
set -u
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
cd "$ROOT"
T=tests/artcache

if [ -z "${HOSTCC:-}" ]; then
    HOSTCC=cc
    for v in 19 18 17 16 15 14; do
        if command -v gcc-$v >/dev/null 2>&1; then HOSTCC=gcc-$v; break; fi
    done
fi

if command -v python3 >/dev/null 2>&1; then
    HOSTCC="$HOSTCC" python3 "$T/test_books_persistence.py" || exit 1
    HOSTCC="$HOSTCC" python3 "$T/test_gc_policy.py" || exit 1
fi

TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT
FLAGS="-std=gnu99 -O1 -g -Wall -Wextra -Werror -Iapps/ipodui"

if command -v python3 >/dev/null 2>&1; then
    if python3 $T/gen_keys.py --check $T/keys.txt; then
        echo "ok   keys.txt matches gen_keys.py"
    else
        echo "FAIL: keys.txt is stale (python3 $T/gen_keys.py > $T/keys.txt)"
        exit 1
    fi
fi

# shellcheck disable=SC2086
if ! "$HOSTCC" $FLAGS -o "$TMP/test_artkey" $T/test_artkey.c \
        apps/ipodui/artkey.c apps/ipodui/sortkey.c 2>"$TMP/cc.log"; then
    echo "FAIL: compiling apps/ipodui/artkey.c on the host"
    cat "$TMP/cc.log"
    exit 1
fi
if "$TMP/test_artkey" $T/keys.txt; then
    echo "ok   art cache keys (apps/ipodui/artkey.c)"
else
    echo "FAIL: art cache keys (apps/ipodui/artkey.c)"
    exit 1
fi

if command -v python3 >/dev/null 2>&1; then
    if python3 $T/gen_keys.py --chapters --check $T/chapter_keys.txt; then
        echo "ok   chapter_keys.txt matches gen_keys.py --chapters"
    else
        echo "FAIL: chapter_keys.txt is stale (python3 $T/gen_keys.py --chapters > $T/chapter_keys.txt)"
        exit 1
    fi
fi
# shellcheck disable=SC2086
if ! "$HOSTCC" $FLAGS -o "$TMP/test_chapkey" $T/test_chapkey.c \
        apps/ipodui/artkey.c apps/ipodui/sortkey.c 2>"$TMP/cc.log" \
        || ! "$TMP/test_chapkey" $T/chapter_keys.txt; then
    echo "FAIL: chapter art keys (apps/ipodui/artkey.c)"
    cat "$TMP/cc.log"
    exit 1
fi
echo "ok   chapter art keys (apps/ipodui/artkey.c)"

# shellcheck disable=SC2086
if ! "$HOSTCC" $FLAGS -DARTFMT_HOST -o "$TMP/test_artfmt" $T/test_artfmt.c \
        apps/ipodui/artfmt.c apps/ipodui/artkey.c apps/ipodui/sortkey.c 2>"$TMP/cc.log"; then
    echo "FAIL: compiling apps/ipodui/artfmt.c on the host"
    cat "$TMP/cc.log"
    exit 1
fi
mkdir -p "$TMP/cache"
if "$TMP/test_artfmt" "$TMP/cache"; then
    echo "ok   art cache formats (apps/ipodui/artfmt.c)"
else
    echo "FAIL: art cache formats (apps/ipodui/artfmt.c)"
    exit 1
fi
