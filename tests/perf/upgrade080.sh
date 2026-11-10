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

# The upgrade fixture: a stress library as the 0.8.0 release left it, for
# the floor gate's "0.8.0 library, first boot of this firmware" case
# (p081-fixture; every user's first 0.8.1 boot):
#
#   tests/perf/upgrade080.sh 50|65        prints the disk's path
#
# Makes build/sim-<K>k-080/simdisk ON PURPOSE, never by accident: the
# 0.8.0 release's own simulator (FROM below, rc-0.8.0-final, "release:
# ProkBox5g 0.8.0"; its source exported with git archive to
# build/src-080, built once as build/src-080/build/sim-ui, --ipodui
# 64 MB) installed on a disk holding this tree's synthetic library
# (tests/perf/synthetic_library.py, the same files the steady disk
# holds), boots it through its first build (tests/perf/prime-080.script)
# and quits: the tag files, master index, library file, manifest and
# boot stamp are 0.8.0's. The disk keeps 0.8.0's installed files too; a
# caller installs the current ones on its clone (tests/perf/floor_gate.sh,
# as an upgrade replaces them and keeps the database).
# Key: build/sim-<K>k-080/db080.key = FROM, the generator, the prime
# script and the track count; a missing or different key re-makes the
# disk. The 0.8.0 master index must carry 0.8.0's magic (checked).
# Exit 1 when the 0.8.0 build or its first build fails.
set -u
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
FROM=f1f8870be8904c30cc3d20f45244db99afcba080
K=${1:-}
case "$K" in
    50|65) ;;
    *) echo "upgrade080: usage: upgrade080.sh 50|65" >&2; exit 2 ;;
esac
N=$((K * 1000))
SRC=$ROOT/build/src-080
SIM=$SRC/build/sim-ui
B=$ROOT/build/sim-${K}k-080
say() { echo "upgrade080: $*" >&2; }

GEN=$(shasum "$ROOT/tests/perf/synthetic_library.py" | cut -d' ' -f1)
PS=$(shasum "$ROOT/tests/perf/prime-080.script" | cut -d' ' -f1)
KEY=$(printf 'from %s\ngen %s\nprime %s\nn %s\n' "$FROM" "$GEN" "$PS" "$N" | shasum | cut -d' ' -f1)
if [ "$(cat "$B/db080.key" 2>/dev/null)" = "$KEY" ] && [ -f "$B/simdisk/Synth/.synthetic" ]; then
    say "${K}k 0.8.0 disk current (key ${KEY%${KEY#????????}})"
    echo "$B/simdisk"
    exit 0
fi

# the 0.8.0 simulator, from the release commit's own source
if [ "$(cat "$SRC/.from" 2>/dev/null)" != "$FROM" ] || [ ! -x "$SIM/rockboxui" ]; then
    say "building the 0.8.0 simulator (${FROM%${FROM#??????????}}, build/src-080; minutes)"
    git -C "$ROOT" cat-file -e "$FROM^{commit}" 2>/dev/null \
        || { say "FAIL the 0.8.0 commit $FROM is not in this repository"; exit 1; }
    rm -rf "$SRC"
    mkdir -p "$SRC"
    # bsdtar warns about two .gitattributes' metadata and exits 1: judge
    # the export by its files and the archive's own status instead
    git -C "$ROOT" archive -o "$SRC.tar" "$FROM" \
        || { say "FAIL git archive $FROM"; exit 1; }
    tar -x -C "$SRC" -f "$SRC.tar" 2>"$SRC.tar.log"
    NT=$(tar -tf "$SRC.tar" | grep -vc '/$')
    NX=$(cd "$SRC" && find . ! -type d | wc -l | tr -d ' ')
    rm -f "$SRC.tar"
    [ -x "$SRC/tools/configure" ] && [ "$NX" -ge "$NT" ] \
        || { say "FAIL the 0.8.0 export has $NX of $NT files ($SRC.tar.log)"; exit 1; }
    mkdir -p "$SIM"
    (cd "$SIM" && ../../tools/configure --target=22 --type=s --ipodui </dev/null \
        >configure.log 2>&1 && make -j"${UPGRADE_JOBS:-6}" >make.log 2>&1 \
        && make install >install.log 2>&1) \
        || { say "FAIL the 0.8.0 simulator did not build ($SIM/make.log)"; exit 1; }
    printf '%s\n' "$FROM" >"$SRC/.from"
fi

say "making the ${K}k 0.8.0 disk ($B; its first build in the 0.8.0 simulator, minutes)"
rm -rf "$B"
mkdir -p "$B/simdisk"
ln -s "$SIM/rockboxui" "$B/rockboxui"
[ -f "$SIM/UI256.bmp" ] && ln -s "$SIM/UI256.bmp" "$B/UI256.bmp"
rsync -a --exclude 'database*' "$SIM/simdisk/.rockbox/" "$B/simdisk/.rockbox/"
# the device's playlist size, as tests/perf/lib65k.sh
echo "max files in playlist: 2000" >>"$B/simdisk/.rockbox/config.cfg"
python3 "$ROOT/tests/perf/synthetic_library.py" "$B/simdisk" "$N" >/dev/null \
    || { say "FAIL the synthetic library"; exit 1; }
SIM_BUILD=$B SHOTS_TIMEOUT=${SHOTS_TIMEOUT:-1800} \
    "$ROOT/scripts/shots.sh" "$ROOT/tests/perf/prime-080.script" "$B/prime" \
    >"$B/prime.out" 2>&1
grep -q "sim_script: wait_db ready" "$B/prime.out" \
    || { say "FAIL the 0.8.0 first build did not finish ($B/prime.out)"; exit 1; }
# 0.8.0's master index: TAGCACHE_MAGIC for 0.8.0's SORTKEY_VERSION
SV0=$(sed -n 's/^#define SORTKEY_VERSION \([0-9]*\).*/\1/p' "$SRC/apps/ipodui/sortkey.h")
WANT=$(printf '%02x484354' $((0xa0 + ${SV0:-0})))
NIDX=0
for IDX in "$B"/simdisk/.rockbox/database_gen*/database_idx.tcd; do
    [ -f "$IDX" ] || continue
    NIDX=$((NIDX + 1))
    GOT=$(od -An -tx1 -N4 "$IDX" | tr -d ' \n')
    [ "$GOT" = "$WANT" ] || { say "FAIL $IDX starts '$GOT', want 0.8.0's '$WANT'"; exit 1; }
done
[ "$NIDX" -ge 1 ] || { say "FAIL no master index after the 0.8.0 first build"; exit 1; }
printf '%s\n' "$KEY" >"$B/db080.key"
say "${K}k 0.8.0 disk made (key ${KEY%${KEY#????????}}; $(ls "$B/simdisk/.rockbox" | grep -c '^database_gen') generation(s), library file $(ls -l "$B"/simdisk/.rockbox/database_gen*/database_lib.dat 2>/dev/null | awk '{s+=$5} END {print s+0}') B)"
echo "$B/simdisk"
