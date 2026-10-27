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
# Host test of the Chute rules (P12 W4, docs/design/sdk.md 13.3): apps/ipodui/games/chute/chute_core.c
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
T=tests/chute
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
CFLAGS="-std=gnu99 -O1 -g -Wall -Wextra -Werror ${EXTRA_CFLAGS:-} -Iapps -Iapps/ipodui/games/chute -I$TMP/inc"
# The contract (W0): the header and the cases always compile
# shellcheck disable=SC2086
if ! "$HOSTCC" $CFLAGS -c -o "$TMP/t.o" "$T/test_chute.c" 2>"$TMP/cc.log"; then
    echo "FAIL: compiling test_chute.c against its header ($HOSTCC)"
    cat "$TMP/cc.log"; exit 1
fi
for f in apps/ipodui/games/chute/chute_core.c; do
    if [ ! -f "$f" ]; then
        echo "SKIP Chute rules: $f is not written yet (W4); header and cases compile"
        exit 0
    fi
done
# shellcheck disable=SC2086
if ! "$HOSTCC" $CFLAGS -o "$TMP/t" "$T/test_chute.c" apps/ipodui/games/chute/chute_core.c 2>"$TMP/cc.log"; then
    echo "FAIL: linking test_chute.c ($HOSTCC)"
    cat "$TMP/cc.log"; exit 1
fi
"$TMP/t" || { echo "FAIL: Chute rules"; exit 1; }
