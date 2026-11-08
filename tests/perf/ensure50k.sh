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

# The 50k library's database key (gate fixtures need tool keys):
#
#   tests/perf/ensure50k.sh [SOURCE_SIMDISK]
#
# build/sim-50k/simdisk holds a database primed by one firmware. The
# firmware rejects a database whose format or sort-key version it does not
# know and rebuilds it from scratch at the first boot (apps/tagcache.c
# TAGCACHE_MAGIC = base + SORTKEY_VERSION), so every measurement script
# that clones the disk then meets a full scan where it expects a library
# ("lib open songs n=0", "expect_snapshot_play failed"). This script keys
# the disk on every VERSION / MAGIC define in apps/ipodui/sortkey.h,
# apps/tagcache.c and apps/tagcache.h (build/sim-50k/db.key, outside the
# simdisk so clones do not carry it). A missing or different key removes
# the old database and re-primes it (tests/perf/lib50k.sh; minutes), then
# checks the new master index's magic against SORTKEY_VERSION. A SOURCE
# other than the default 50k disk is never re-primed: it fails loudly.
# Exit 0 = the disk is current; 1 = stale and not re-primable, or the
# re-prime failed; 2 = the key could not be computed.
set -u
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
DEF=$ROOT/build/sim-50k/simdisk
SOURCE=${1:-$DEF}
KEYFILE=$(dirname "$SOURCE")/db.key
LINES=$(cat "$ROOT/apps/ipodui/sortkey.h" "$ROOT/apps/tagcache.c" "$ROOT/apps/tagcache.h" \
    | grep -E '^#[[:space:]]*define[[:space:]]+[A-Z0-9_]*(VERSION|MAGIC)[[:space:]]' | sort)
N=$(printf '%s\n' "$LINES" | grep -c .)
[ "$N" -ge 3 ] || { echo "ensure50k: FAIL only $N VERSION/MAGIC defines found (apps/ipodui/sortkey.h, apps/tagcache.[ch])" >&2; exit 2; }
KEY=$(printf '%s\n' "$LINES" | shasum | cut -d' ' -f1)
SV=$(sed -n 's/^#define SORTKEY_VERSION \([0-9]*\).*/\1/p' "$ROOT/apps/ipodui/sortkey.h")
[ -n "$SV" ] || { echo "ensure50k: FAIL no SORTKEY_VERSION in apps/ipodui/sortkey.h" >&2; exit 2; }
if [ -f "$SOURCE/Synth/.synthetic" ] && [ "$(cat "$KEYFILE" 2>/dev/null)" = "$KEY" ]; then
    echo "ensure50k: 50k database current (key ${KEY%${KEY#????????}}, $N version/magic defines, SORTKEY_VERSION $SV)" >&2
    exit 0
fi
if [ "$SOURCE" != "$DEF" ]; then
    echo "ensure50k: FAIL $SOURCE has no matching db.key ($KEYFILE: '$(cat "$KEYFILE" 2>/dev/null)', want $KEY, SORTKEY_VERSION $SV); re-prime it with tests/perf/lib50k.sh" >&2
    exit 1
fi
echo "ensure50k: 50k database key '$(cat "$KEYFILE" 2>/dev/null)' != '$KEY' (SORTKEY_VERSION $SV): re-priming (tests/perf/lib50k.sh, minutes)" >&2
rm -f "$KEYFILE"
rm -rf "$SOURCE"/.rockbox/database*
"$ROOT/tests/perf/lib50k.sh" >"$ROOT/build/ensure50k.out" 2>&1 || {
    echo "ensure50k: FAIL lib50k.sh could not prime the 50k library ($ROOT/build/ensure50k.out)" >&2
    exit 1; }
# the new master index must carry this firmware's magic: 0x544348a0 + SORTKEY_VERSION
# (the active generation's index, database_gen*/database_idx.tcd)
WANT=$(printf '%02x484354' $((0xa0 + SV)))
NIDX=0
for IDX in "$SOURCE"/.rockbox/database_gen*/database_idx.tcd "$SOURCE/.rockbox/database_idx.tcd"; do
    [ -f "$IDX" ] || continue
    NIDX=$((NIDX + 1))
    GOT=$(od -An -tx1 -N4 "$IDX" | tr -d ' \n')
    [ "$GOT" = "$WANT" ] || {
        echo "ensure50k: FAIL primed $IDX starts '$GOT', want '$WANT' (TAGCACHE_MAGIC for SORTKEY_VERSION $SV)" >&2
        exit 1; }
done
[ "$NIDX" -ge 1 ] || { echo "ensure50k: FAIL no database_idx.tcd under $SOURCE/.rockbox after the prime" >&2; exit 1; }
printf '%s\n' "$KEY" >"$KEYFILE"
echo "ensure50k: 50k database re-primed (key ${KEY%${KEY#????????}}, SORTKEY_VERSION $SV)" >&2
