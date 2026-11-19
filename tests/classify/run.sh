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

# #146 host test: audiobook/podcast classification rules
# (apps/ipodui/bookclass.h, used by ui_classify() in apps/tagcache.c).
# Compiler: the newest Homebrew gcc-NN, else cc; override with HOSTCC=...
set -u
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
cd "$ROOT"
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
        -Iapps/ipodui -o "$TMP/test_bookclass" tests/classify/test_bookclass.c \
        2>"$TMP/cc.log"; then
    echo "FAIL: compiling tests/classify/test_bookclass.c ($HOSTCC)"
    cat "$TMP/cc.log"
    exit 1
fi
"$TMP/test_bookclass" || { echo "FAIL: bookclass"; exit 1; }

# #163: parser rows. The genre comes from the real ID3 parser (warble -m on
# tests/fixtures/mv/, made by make_multivalue_fixtures.py), then the same
# rules classify it: ID3v2.3 TCON "(183)" is Audiobook, so a book anywhere.
if [ -z "${WARBLE:-}" ]; then
    for w in build/warble/warble.*; do
        [ -x "$w" ] && WARBLE=$w && break
    done
fi
if [ -z "${WARBLE:-}" ] || [ ! -x "$WARBLE" ]; then
    echo "FAIL: warble not found (scripts/test.sh builds it)"
    exit 2
fi
fail=0
n=0
while read -r fixture want; do
    [ -n "$fixture" ] || continue
    if ! "$WARBLE" -m "tests/fixtures/mv/$fixture" >"$TMP/m.txt" 2>/dev/null; then
        echo "FAIL: warble -m tests/fixtures/mv/$fixture"
        fail=1
        continue
    fi
    genre=$(sed -n 's/^Genre: //p' "$TMP/m.txt")
    got=$("$TMP/test_bookclass" --one "$genre" "/Music/a/$fixture")
    n=$((n + 1))
    if [ "$got" != "$want" ]; then
        echo "FAIL: $fixture: genre '$genre' is $got, want $want"
        fail=1
    fi
done <<EOF
g183.mp3 book
g148.mp3 music
EOF
[ "$n" = 2 ] || { echo "FAIL: parser rows ran $n of 2"; exit 1; }
[ "$fail" = 0 ] || exit 1
echo "ok   bookclass parser rows: $n (g183.mp3 '(183)' is a book)"
