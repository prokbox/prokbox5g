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
# Host test of the sync-time index format (P12, docs/phase12-brief.md 3.5): apps/ipodui/xindex_core.c
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

set -u
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
cd "$ROOT"
T=tests/xindex
if [ -z "${HOSTCC:-}" ]; then
    HOSTCC=cc
    for v in 19 18 17 16 15 14; do
        if command -v gcc-$v >/dev/null 2>&1; then HOSTCC=gcc-$v; break; fi
    done
fi
TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT
mkdir -p "$TMP/inc"
cp firmware/include/crc32.h "$TMP/inc/"   # alone: firmware/include
                                            # shadows libc headers
CFLAGS="-std=gnu99 -O1 -g -Wall -Wextra -Werror ${EXTRA_CFLAGS:-} -Iapps -I$TMP/inc"
# The contract (W0): the header and the cases always compile
# shellcheck disable=SC2086
if ! "$HOSTCC" $CFLAGS -c -o "$TMP/t.o" "$T/test_xindex.c" 2>"$TMP/cc.log"; then
    echo "FAIL: compiling test_xindex.c against its header ($HOSTCC)"
    cat "$TMP/cc.log"; exit 1
fi
for f in apps/ipodui/xindex_core.c apps/ipodui/xindex_build.c; do
    if [ ! -f "$f" ]; then
        echo "SKIP index format: $f is not written yet (W1-N); header and cases compile"
        exit 0
    fi
done
# shellcheck disable=SC2086
if ! "$HOSTCC" $CFLAGS -o "$TMP/t" "$T/test_xindex.c" apps/ipodui/xindex_core.c apps/ipodui/xindex_build.c 2>"$TMP/cc.log"; then
    echo "FAIL: linking test_xindex.c ($HOSTCC)"
    cat "$TMP/cc.log"; exit 1
fi
"$TMP/t" || { echo "FAIL: index format"; exit 1; }
# W3-K: the vCard parser and the Contacts index (xcontact_core.c)
# shellcheck disable=SC2086
if ! "$HOSTCC" $CFLAGS -o "$TMP/tc" "$T/test_xcontact.c" apps/ipodui/xcontact_core.c apps/ipodui/xindex_core.c apps/ipodui/xindex_build.c 2>"$TMP/cc.log"; then
    echo "FAIL: linking test_xcontact.c ($HOSTCC)"
    cat "$TMP/cc.log"; exit 1
fi
"$TMP/tc" || { echo "FAIL: vCard parser"; exit 1; }
