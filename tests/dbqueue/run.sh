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

# Host tests of the queue-by-index core (#109, docs/design/queue-by-
# index.md section 7): apps/ipodui/dbqueue_core.c. See test_dbqueue.c.
# Compiler: the newest Homebrew gcc-NN, else cc; override with HOSTCC=...
set -u
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
cd "$ROOT"
T=tests/dbqueue

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
        -I$T/stub -Iapps/ipodui -o "$TMP/test_dbqueue" "$T/test_dbqueue.c" \
        apps/ipodui/dbqueue_core.c 2>"$TMP/cc.log"; then
    echo "FAIL: compiling dbqueue_core.c on the host ($HOSTCC)"
    cat "$TMP/cc.log"
    exit 1
fi
if "$TMP/test_dbqueue"; then
    echo "ok   queue by index core (seals, T tag, modes, H1 sentinel, H2 rebase, H4 reserve, cuts, M5)"
else
    echo "FAIL: queue by index core (dbqueue_core.c)"
    exit 1
fi
