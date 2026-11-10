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

# The stress libraries' freshness check (gate fixtures need tool keys): the
# 65k disk (build/sim-65k) and, through tests/perf/ensure50k.sh, the 32 MB
# unit's 50k disk (build/sim-50k; #229).
#
#   tests/perf/ensure65k.sh [SOURCE_SIMDISK]
#
# build/sim-65k/simdisk holds a library primed by one firmware: the tag
# files and master index (database_gen<n>/), the shell's library file
# (database_lib.dat), the generation manifest, the grouping mark and the
# boot stamp. The measurement scripts that clone it measure the STEADY
# state (a device after its first boot of this firmware). A disk primed by
# older firmware is not that: the first boot rewrites the library file,
# publishes a new generation, writes a stamp, and the gate silently
# measures an upgrade (0.8.1: a 50k disk primed at 11:11 with no boot stamp
# and a 729,348 B library file, where this firmware writes 1,199,312 B,
# passed the old key, which looked only at VERSION/MAGIC defines; neither
# change bumped one). The upgrade is measured on purpose, by its own
# labelled case (tests/perf/upgrade080.sh, tests/perf/floor_gate.sh).
#
# Three checks, all needed:
#   1. the key (build/sim-65k/db.key, outside the simdisk so clones do not
#      carry it): every VERSION / MAGIC define in apps/ipodui/sortkey.h,
#      apps/ipodui/dbgen.c and apps/tagcache.[ch], every #define of the
#      library file's and the boot stamp's headers (apps/ipodui/libfile.h,
#      apps/ipodui/bootstamp.h: section layout and size rules too), the
#      library generator (tests/perf/synthetic_library.py) and FIXTURE_REV
#      below (bump it when these rules change);
#   2. the files: Synth/.synthetic, a boot stamp of BOOT_STAMP_VERSION,
#      the grouping mark at UI_GROUPING_VERSION, every master index at
#      this TAGCACHE_MAGIC, every library file at LIBFILE_VERSION (the
#      simulator's non-video value), SORTKEY_VERSION and LIBFILE_GROUPING;
#   3. the steady probe: build/sim-ui boots a clone once
#      (tests/perf/floor-settle.script) and must change no database file
#      (no new generation, no rewritten library file or manifest; the
#      stamp is rewritten at every simulator boot and is not compared).
#      This catches what no define names: a writer whose output changed.
#      Its verdict is cached in db.probe against the key and the
#      simulator binary's sha1, so a gate pays it once per firmware build
#      (~20 s at 50k).
# A default disk that fails any check is wiped (its whole simdisk) and
# re-primed (tests/perf/lib65k.sh; ~1.5 min at 50k), and the new disk must
# then pass checks 2 and 3. A SOURCE other than the two default disks is
# never re-primed: it is refused with the reason.
# Exit 0 = the disk is current; 1 = stale and not re-primable, or the
# re-prime failed; 2 = the key could not be computed.
set -u
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
SOURCE=${1:-$ROOT/build/sim-65k/simdisk}
FIXTURE_REV=2       # 2: p081-fixture (files + steady probe)
# the two stress disks (#229: 32 MB at 50k, 64 MB at 65k) re-prime with
# their own generator; any other SOURCE never does
case "$SOURCE" in
    "$ROOT/build/sim-50k/simdisk") DEF=$SOURCE; LIB=lib50k.sh; SZ=50k ;;
    *) DEF=$ROOT/build/sim-65k/simdisk; LIB=lib65k.sh; SZ=65k ;;
esac
KEYFILE=$(dirname "$SOURCE")/db.key
PROBEFILE=$(dirname "$SOURCE")/db.probe
PROBE_SIM=${ENSURE_PROBE_SIM:-$ROOT/build/sim-ui}
say() { echo "ensure65k: $*" >&2; }

# -- 1. the key ---------------------------------------------------------------
VM='^#[[:space:]]*define[[:space:]]+[A-Z0-9_]*(VERSION|MAGIC)[[:space:]]'
AD='^#[[:space:]]*define[[:space:]]'
for f in apps/ipodui/sortkey.h apps/ipodui/dbgen.c apps/tagcache.c apps/tagcache.h \
         apps/ipodui/libfile.h apps/ipodui/bootstamp.h tests/perf/synthetic_library.py; do
    [ -f "$ROOT/$f" ] || { say "FAIL $f is missing: no key"; exit 2; }
done
LINES=$( { cat "$ROOT/apps/ipodui/sortkey.h" "$ROOT/apps/ipodui/dbgen.c" \
               "$ROOT/apps/tagcache.c" "$ROOT/apps/tagcache.h" | grep -E "$VM"
           cat "$ROOT/apps/ipodui/libfile.h" "$ROOT/apps/ipodui/bootstamp.h" | grep -E "$AD"
         } | sort)
N=$(printf '%s\n' "$LINES" | grep -c .)
NL=$(grep -cE "$AD" "$ROOT/apps/ipodui/libfile.h")
NB=$(grep -cE "$AD" "$ROOT/apps/ipodui/bootstamp.h")
[ "$N" -ge 10 ] && [ "$NL" -ge 5 ] && [ "$NB" -ge 3 ] || {
    say "FAIL only $N defines found ($NL in libfile.h, $NB in bootstamp.h)"; exit 2; }
GEN=$(shasum "$ROOT/tests/perf/synthetic_library.py" | cut -d' ' -f1)
KEY=$(printf '%s\nrev %s\ngen %s\n' "$LINES" "$FIXTURE_REV" "$GEN" | shasum | cut -d' ' -f1)
KS=${KEY%${KEY#????????}}

def() { # FILE NAME -> the last "#define NAME <number>" (the non-video value)
    sed -n "s/^#define $2[[:space:]][[:space:]]*\([0-9xa-fA-FuU]*\).*/\1/p" "$ROOT/$1" \
        | tail -1 | tr -d 'uU'
}
SV=$(def apps/ipodui/sortkey.h SORTKEY_VERSION)
GV=$(def apps/ipodui/sortkey.h UI_GROUPING_VERSION)
BV=$(def apps/ipodui/bootstamp.h BOOT_STAMP_VERSION)
LV=$(def apps/ipodui/libfile.h LIBFILE_VERSION)
LG=$(def apps/ipodui/libfile.h LIBFILE_GROUPING)
for v in "SORTKEY_VERSION=$SV" "UI_GROUPING_VERSION=$GV" "BOOT_STAMP_VERSION=$BV" \
         "LIBFILE_VERSION=$LV" "LIBFILE_GROUPING=$LG"; do
    [ -n "${v#*=}" ] || { say "FAIL could not read ${v%=*}"; exit 2; }
done

# -- 2. the files ---------------------------------------------------------------
le32() { od -An -tu4 -j "$2" -N4 "$1" 2>/dev/null | tr -d ' \n'; }
files_why() { # DISK -> prints the first reason it is not this firmware's, or nothing
    d=$1
    [ -f "$d/Synth/.synthetic" ] || { echo "no synthetic library"; return; }
    st=$d/.rockbox/boot_stamp.dat
    [ -f "$st" ] || { echo "no boot stamp (primed before a first build left one)"; return; }
    [ "$(le32 "$st" 0)" = $((0x54534250)) ] && [ "$(le32 "$st" 4)" = "$BV" ] \
        || { echo "boot stamp magic/version $(le32 "$st" 0)/$(le32 "$st" 4), want PBST/$BV"; return; }
    [ "$(le32 "$d/.rockbox/database_order.dat" 0)" = "$GV" ] \
        || { echo "grouping mark '$(le32 "$d/.rockbox/database_order.dat" 0)', want $GV"; return; }
    want=$(printf '%02x484354' $((0xa0 + SV)))
    ni=0; nl=0
    for g in "$d"/.rockbox/database_gen*; do
        [ -d "$g" ] || continue
        if [ -f "$g/database_idx.tcd" ]; then
            ni=$((ni + 1))
            got=$(od -An -tx1 -N4 "$g/database_idx.tcd" | tr -d ' \n')
            [ "$got" = "$want" ] || { echo "${g##*/}/database_idx.tcd magic $got, want $want"; return; }
        fi
        lf=$g/database_lib.dat
        if [ -f "$lf" ]; then
            nl=$((nl + 1))
            [ "$(le32 "$lf" 0)" = $((0x424c5049)) ] && [ "$(le32 "$lf" 4)" = "$LV" ] \
                && [ "$(le32 "$lf" 8)" = "$SV" ] && [ "$(le32 "$lf" 28)" = "$LG" ] \
                || { echo "${g##*/}/database_lib.dat version/sortkey/grouping $(le32 "$lf" 4)/$(le32 "$lf" 8)/$(le32 "$lf" 28), want $LV/$SV/$LG"; return; }
        fi
    done
    [ "$ni" -ge 1 ] || { echo "no database_gen*/database_idx.tcd"; return; }
    [ "$nl" -ge 1 ] || { echo "no database_gen*/database_lib.dat"; return; }
}

# -- 3. the steady probe --------------------------------------------------------
db_sums() { # DISK -> sha1 of every database file under .rockbox (two levels)
    (cd "$1/.rockbox" && find . -maxdepth 2 -type f -name 'database*' | sort \
        | while read -r f; do shasum "$f"; done)
}
probe_why() { # DISK -> the files the current firmware changed at a boot, or nothing
    [ -x "$PROBE_SIM/rockboxui" ] || { echo "no $PROBE_SIM/rockboxui for the steady probe"; return; }
    sim_sha=$(shasum "$PROBE_SIM/rockboxui" | cut -d' ' -f1)
    if [ "$(cat "$PROBEFILE" 2>/dev/null)" = "steady $KEY $sim_sha" ]; then
        return
    fi
    pd=$(mktemp -d "$ROOT/build/fixture-probe.XXXXXX")
    mkdir -p "$pd/simdisk"
    cp -cpR "$1/." "$pd/simdisk/" 2>/dev/null || cp -pR "$1/." "$pd/simdisk/"
    db_sums "$pd/simdisk" >"$pd/before"
    SIM_BUILD=$PROBE_SIM SIMDISK=$pd/simdisk SHOTS_TIMEOUT=${SHOTS_TIMEOUT:-600} \
        "$ROOT/scripts/shots.sh" "$ROOT/tests/perf/floor-settle.script" "$pd/out" \
        >"$pd/shots.out" 2>&1
    if ! grep -q "sim_script: done, quitting" "$pd/shots.out"; then
        echo "the probe boot did not finish ($pd/shots.out)"; return
    fi
    db_sums "$pd/simdisk" >"$pd/after"
    if [ ! -s "$pd/before" ]; then
        echo "the probe saw no database files"; return
    fi
    ch=$(diff "$pd/before" "$pd/after" | sed -n 's/^[<>] [0-9a-f]* *\.\///p' | sort -u | tr '\n' ' ')
    if [ -n "$ch" ]; then
        echo "a boot of this firmware changed: $ch($pd)"; return
    fi
    rm -rf "$pd"
    printf 'steady %s %s\n' "$KEY" "$sim_sha" >"$PROBEFILE"
}

why=
if [ "$(cat "$KEYFILE" 2>/dev/null)" != "$KEY" ]; then
    why="key '$(cat "$KEYFILE" 2>/dev/null)' != '$KEY'"
else
    why=$(files_why "$SOURCE")
    [ -n "$why" ] || why=$(probe_why "$SOURCE")
fi
if [ -z "$why" ]; then
    say "$SZ database current (key $KS, $N defines, SORTKEY_VERSION $SV, stamp v$BV, library file v$LV; steady at this simulator)"
    exit 0
fi
if [ "$SOURCE" != "$DEF" ]; then
    say "FAIL $SOURCE is not this firmware's steady state: $why; re-prime it with tests/perf/$LIB (or use the upgrade case, tests/perf/upgrade080.sh)"
    exit 1
fi
say "$SZ disk stale ($why): wiping and re-priming (tests/perf/$LIB, minutes)"
rm -f "$KEYFILE" "$PROBEFILE"
rm -rf "$SOURCE"
"$ROOT/tests/perf/$LIB" >"$ROOT/build/ensure$SZ.out" 2>&1 || {
    say "FAIL $LIB could not prime the $SZ library ($ROOT/build/ensure$SZ.out)"
    exit 1; }
why=$(files_why "$SOURCE")
[ -n "$why" ] || why=$(probe_why "$SOURCE")
[ -z "$why" ] || { say "FAIL the re-primed $SZ disk is not steady: $why"; exit 1; }
printf '%s\n' "$KEY" >"$KEYFILE"
say "$SZ database re-primed (key $KS, SORTKEY_VERSION $SV, stamp v$BV, library file v$LV; steady at this simulator)"
