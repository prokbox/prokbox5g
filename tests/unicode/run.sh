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

# NFC golden (docs/phase9-brief.md 3.3.2, Rev 1 H4):
#  - firmware/common/nfc_table.h is what tools/gen_nfc.c generates from the
#    in-tree lib/utf8proc (no drift);
#  - every case of `gen_nfc cases` (about 210k: all pairs x every combining
#    class blocked/unblocked, chains, all Hangul, every canonical
#    decomposition incl. exclusions/singletons/non-BMP, invalid UTF-8, the
#    2,047/2,048 code-point limit, random mark soup, a smoke set) is
#    byte-identical between firmware/common/nfc.c and the in-tree utf8proc
#    called as unicode.c calls it. No TAGCACHE_MAGIC fallback: a difference
#    is a bug in nfc.c.
# The corpus (~5 MB) is regenerated into a temp dir on every run, not
# checked in. Compiler: the newest Homebrew gcc-NN, else cc (HOSTCC=...).
set -u
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
cd "$ROOT"
T=tests/unicode

if [ -z "${HOSTCC:-}" ]; then
    HOSTCC=cc
    for v in 19 18 17 16 15 14; do
        if command -v gcc-$v >/dev/null 2>&1; then HOSTCC=gcc-$v; break; fi
    done
fi

TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT

# utf8proc and the generator that #includes it are third-party code: build
# them without -Werror; nfc.c and the test are built strict.
if ! "$HOSTCC" -std=gnu99 -O1 -w -Ilib/utf8proc -o "$TMP/gen_nfc" \
        tools/gen_nfc.c 2>"$TMP/cc.log"; then
    echo "FAIL: compiling tools/gen_nfc.c"; cat "$TMP/cc.log"; exit 1
fi
if ! "$TMP/gen_nfc" table >"$TMP/nfc_table.h" 2>"$TMP/gen.log"; then
    echo "FAIL: gen_nfc table"; cat "$TMP/gen.log"; exit 1
fi
if cmp -s "$TMP/nfc_table.h" firmware/common/nfc_table.h; then
    echo "ok   nfc_table.h matches gen_nfc ($(sed 's/^gen_nfc: //' "$TMP/gen.log"))"
else
    echo "FAIL: firmware/common/nfc_table.h is stale (gen_nfc table > firmware/common/nfc_table.h)"
    exit 1
fi
if ! "$TMP/gen_nfc" cases >"$TMP/nfc_cases.txt" 2>"$TMP/gen.log"; then
    echo "FAIL: gen_nfc cases"; cat "$TMP/gen.log"; exit 1
fi

if ! "$HOSTCC" -std=gnu99 -O1 -g -w -Ilib/utf8proc -c -o "$TMP/utf8proc.o" \
        lib/utf8proc/utf8proc.c 2>"$TMP/cc.log" \
   || ! "$HOSTCC" -std=gnu99 -O1 -g -Wall -Wextra -Werror -Ifirmware/include \
        -Ifirmware/common -Ilib/utf8proc -o "$TMP/test_nfc" $T/test_nfc.c \
        firmware/common/nfc.c "$TMP/utf8proc.o" 2>"$TMP/cc.log"; then
    echo "FAIL: compiling firmware/common/nfc.c / test_nfc.c on the host"
    cat "$TMP/cc.log"; exit 1
fi
if "$TMP/test_nfc" "$TMP/nfc_cases.txt"; then
    echo "ok   nfc.c byte-identical to utf8proc ($(grep 'cases total' "$TMP/gen.log" | sed 's/^gen_nfc: //'))"
else
    echo "FAIL: nfc.c differs from utf8proc"
    exit 1
fi
