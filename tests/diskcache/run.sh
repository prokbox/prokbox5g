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

# firmware/common/disk_cache.c's defensive checks (int4-dcache, #355):
# a probe or peek never hands out an entry that is not a cached sector,
# whatever the map says, and dc_dirty_buf() refuses (counts; developer
# builds panic) a buffer the cache does not hold in use.  Built for the
# host with tests/fatcrash's shim, as the shipping shell build (HAVE_IPOD_UI)
# and as a developer build (HAVE_IPOD_UI_DEVMODE).  Negative control: the
# cache before the checks (commit 2ddb5ef6ba) must fail both.
# Compiler: the newest Homebrew gcc-NN, else cc; override with HOSTCC=...
set -u
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
cd "$ROOT"
T=tests/diskcache

if [ -z "${HOSTCC:-}" ]; then
    HOSTCC=cc
    for v in 19 18 17 16 15 14; do
        if command -v gcc-$v >/dev/null 2>&1; then HOSTCC=gcc-$v; break; fi
    done
fi

TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT
mkdir -p "$TMP/inc"
for d in firmware/export firmware/include firmware/kernel/include; do
    cp "$d"/*.h "$TMP/inc/"
done
cp tests/fatcrash/shim/*.h "$TMP/inc/"
CFLAGS="-std=gnu99 -O1 -g -w -I$TMP/inc -DMAX_VIRT_SECTOR_SIZE=2048 \
 -DDEFAULT_VIRT_SECTOR_SIZE=2048"
fail=0

build() {   # out source extra-flags...
    out=$1; src=$2; shift 2
    # shellcheck disable=SC2086
    "$HOSTCC" $CFLAGS "$@" -DDCTEST_SOURCE="\"$src\"" \
        -o "$TMP/$out" "$T/dctest.c" firmware/common/linked_list.c \
        2>"$TMP/$out.cc" || { echo "FAIL: compiling $out"; cat "$TMP/$out.cc"; exit 1; }
}

build dc_rel "$ROOT/firmware/common/disk_cache.c"
build dc_dev "$ROOT/firmware/common/disk_cache.c" -DHAVE_IPOD_UI_DEVMODE
for v in dc_rel dc_dev; do
    if "$TMP/$v" >"$TMP/$v.log" 2>&1; then
        echo "ok   diskcache $v: $(tail -n 1 "$TMP/$v.log")"
    else
        echo "FAIL: diskcache $v"; cat "$TMP/$v.log"; fail=1
    fi
done

if git cat-file -e 2ddb5ef6ba 2>/dev/null; then
    git show 2ddb5ef6ba:firmware/common/disk_cache.c >"$TMP/disk_cache_0.c"
    for f in "" -DHAVE_IPOD_UI_DEVMODE; do
        build dc_ctl "$TMP/disk_cache_0.c" -DDCTEST_CONTROL $f
        if "$TMP/dc_ctl" >"$TMP/dc_ctl.log" 2>&1; then
            echo "FAIL: diskcache negative control ${f:-release} passed: no teeth"
            fail=1
        else
            echo "ok   diskcache negative control ${f:-release} caught:" \
                 "$(grep "^FAIL " "$TMP/dc_ctl.log" | tr "\n" ";")"
        fi
    done
else
    echo "FAIL: diskcache negative control: commit 2ddb5ef6ba not found"
    fail=1
fi
exit $fail
