#!/bin/bash
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

# Where a first database build's commit spends its time and storage
# commands (#245, 2026-11-09).
#
#   tests/perf/commit_trace.sh [tracks] [label]   (default 10000 run)
#
# Makes build/sim-committrace-<tracks>-<label> from build/sim-ui (or the
# simulator build COMMIT_TRACE_SIM names) with a synthetic
# library of <tracks> (tests/perf/synthetic_library.py, generated once into
# build/committrace-lib/<tracks> and cloned with its mtimes, so every run
# indexes the same files) and builds the database from nothing, twice:
#   1. plain: the devmode "committrace" lines of apps/tagcache.c
#      (COMMIT_TRACE): per stage, summed over the tags, the tagcache
#      thread's own CPU ms and its file calls (the simulator counts each
#      open, read and write as one); then the sha256 of every database
#      file (the byte-identity check: compare two runs' "db digest");
#   2. traced (IPODUI_FSTRACE + IPODUI_FSTRACE_READS; COMMIT_TRACE_COUNT=0
#      skips it): the session replayed on the firmware's own file.c and
#      fat.c (tests/fatcrash/fsreplay "count": 2048-byte sectors, 16 KiB
#      clusters as the 5.5G), which prints each stage's storage commands
#      as the device's file layer issues them (one sector cache per open
#      file, the FAT cache): rd_cmds/wr_cmds, 512-byte sectors, and the
#      file calls (api_rd, api_wr, opens).
# The traced run's tagcache thread pays for the trace lines, so its times
# are not used.
#   COMMIT_TRACE_ADD=<k>: the incremental case: the last k artist folders
#     are held back from a first build and added for a second boot's
#     update, whose commit is the one measured and hashed (it loads the
#     old tag files and rewrites the master index entries).
#   COMMIT_TRACE_READD=<k>: built whole, the last k artist folders removed
#     (an update marks their entries deleted) and put back: the measured
#     update resurrects those entries (their statistics carried over).
#   COMMIT_TRACE_EXPECT=<digest>: any other "db digest" fails.
# The library's file times are fixed (below), so a digest is comparable
# across machines and worktrees at the same "lib fingerprint": the 10k
# first build is 7ef3ee99f8db190b at fingerprint 1acc4fee0396cb7b on
# both aa900bccad (#245) and 97c28676a0 (p081-floor).
# Results: build/committrace/<tracks>-<label>.txt; exit 1 on a FAIL line.
set -e
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
N=${1:-10000}
LABEL=${2:-run}
UI=${COMMIT_TRACE_SIM:-$ROOT/build/sim-ui}
B=$ROOT/build/sim-committrace-$N-$LABEL
LIB=$ROOT/build/committrace-lib/$N
OUT=$ROOT/build/committrace
RES=$OUT/$N-$LABEL.txt
[ -x "$UI/rockboxui" ] || { echo "build build/sim-ui first" >&2; exit 2; }
[ -d "$UI/simdisk/.rockbox/codecs" ] || make -C "$UI" install >/dev/null
mkdir -p "$OUT"
if [ ! -d "$LIB/Synth" ]; then
    rm -rf "$LIB"
    mkdir -p "$LIB"
    python3 "$ROOT/tests/perf/synthetic_library.py" "$LIB" "$N" >/dev/null
fi
n_lib=$(find "$LIB/Synth" -name '*.mp3' | wc -l | tr -d ' ')
[ "$n_lib" -ge "$N" ] || { echo "library has $n_lib tracks, under $N" >&2; exit 2; }
# Fixed file times (p081-fixture): the generator leaves each file's mtime
# at the moment it ran, and the database stores it (tag_mtime in every
# master index entry; the library file's MTIME columns, its album table
# and panel set, and their sums), so two libraries generated at different
# times give different digests from the same firmware (#245's 10k
# 318ae16b4f50605d and a later 653c2d3e9f1ee3bb were one firmware output
# on two libraries). Every file and folder gets a time from its sorted
# path (2026-01-01 + 2 s apart: FAT's resolution), the same on every
# machine and worktree. "lib fingerprint" (paths, sizes, times) names the
# input: compare digests only between runs with the same fingerprint.
if [ ! -f "$LIB/.fixed-mtimes" ]; then
    python3 - "$LIB/Synth" <<'EOF_MT'
import os, sys
root = sys.argv[1]
paths = []
for d, dirs, files in os.walk(root):
    paths += [os.path.join(d, f) for f in files]
    paths += [os.path.join(d, s) for s in dirs]
paths.sort()
base = 1767225600                       # 2026-01-01 00:00:00 UTC
for i, p in enumerate(paths):
    os.utime(p, (base + 2 * i, base + 2 * i), follow_symlinks=False)
os.utime(root, (base, base))
EOF_MT
    touch "$LIB/.fixed-mtimes"
fi
LIB_FP=$(python3 - "$LIB/Synth" <<'EOF_FP'
import hashlib, os, sys
root = sys.argv[1]
h = hashlib.sha256()
n = 0
for d, dirs, files in os.walk(root):
    dirs.sort()
    files.sort()
    for f in dirs + files:
        p = os.path.join(d, f)
        st = os.lstat(p)
        h.update(("%s %d %d\n" % (os.path.relpath(p, root),
                  st.st_size if f in files else 0, int(st.st_mtime))).encode())
        n += 1
print(h.hexdigest()[:16] if n else "")
EOF_FP
)
[ -n "$LIB_FP" ] || { echo "could not fingerprint the library" >&2; exit 2; }

# the counter, built as tests/fatcrash/powercut.sh builds fsreplay
FC=$OUT/fatcrash
mkdir -p "$FC/inc"
fs="$ROOT/firmware/common"
for d in firmware/export firmware/include firmware/kernel/include; do
    cp "$ROOT/$d"/*.h "$FC/inc/"
done
cp "$ROOT/tests/fatcrash/shim/"*.h "$FC/inc/"
cc -std=gnu99 -O1 -w -I"$FC/inc" -DMAX_VIRT_SECTOR_SIZE=2048 \
   -DDEFAULT_VIRT_SECTOR_SIZE=2048 -o "$FC/fsreplay" \
   "$ROOT/tests/fatcrash/fsreplay.c" "$fs/fat.c" "$fs/disk_cache.c" \
   "$fs/linked_list.c" "$fs/file.c" "$fs/file_internal.c" \
   "$fs/fileobj_mgr.c" "$fs/dir.c" "$fs/pathfuncs.c" "$fs/rb_namespace.c"

S=$OUT/commit_trace.script
printf '%s\n' "# timeout: 3000" "wait 3000" "wait_db 2400000" "wait 2000" \
    "shot end" "quit" >"$S"
# the incremental case's second boot: the held-back folders are back
S2=$OUT/commit_trace_update.script
printf '%s\n' "# timeout: 3000" "wait 3000" "wait_db 2400000" \
    "update_db_wait 2400000" "wait 2000" "shot end" "quit" >"$S2"
ADD=${COMMIT_TRACE_ADD:-0}
READD=${COMMIT_TRACE_READD:-0}

fresh() {
    rm -rf "$B"
    mkdir -p "$B/simdisk"
    ln -sf "$UI/rockboxui" "$B/rockboxui"
    if [ -f "$UI/UI256.bmp" ]; then ln -sf "$UI/UI256.bmp" "$B/UI256.bmp"; fi
    rsync -a --exclude 'database*' "$UI/simdisk/.rockbox/" "$B/simdisk/.rockbox/"
    # a clone that keeps the mtimes (APFS: no data copied)
    /bin/cp -cRp "$LIB/Synth" "$B/simdisk/Synth" 2>/dev/null \
        || cp -Rp "$LIB/Synth" "$B/simdisk/Synth"
    if [ "$ADD" -gt 0 ]; then
        # the incremental case: the last ADD artist folders come later
        mkdir -p "$B/held"
        ls "$B/simdisk/Synth" | sort | tail -n "$ADD" | while read -r d; do
            mv "$B/simdisk/Synth/$d" "$B/held/"
        done
    fi
}

shots() { # name, script, extra env...
    local name=$1 script=$2 rc=0
    shift 2
    env "$@" SIM_BUILD="$B" SHOTS_TIMEOUT=3000 "$ROOT/scripts/shots.sh" "$script" \
        "$OUT/shots-$N-$LABEL-$name" >"$OUT/$N-$LABEL-$name.shots.out" 2>&1 || rc=$?
    cp "$OUT/shots-$N-$LABEL-$name/sim.log" "$OUT/$N-$LABEL-$name.sim.log"
    return $rc
}

run() { # name, extra env (for the measured commit's session)...
    local name=$1 rc=0
    shift
    fresh
    if [ "$ADD" -gt 0 ]; then
        shots "$name-first" "$S" || rc=$?
        if [ "$rc" != 0 ]; then echo "$rc"; return; fi
        mv "$B/held/"* "$B/simdisk/Synth/"
        shots "$name" "$S2" "$@" || rc=$?
    elif [ "$READD" -gt 0 ]; then
        # built whole, then the last READD artist folders go (an update
        # marks their entries deleted) and come back: the measured
        # update's commit resurrects their entries' statistics
        shots "$name-first" "$S" || rc=$?
        if [ "$rc" != 0 ]; then echo "$rc"; return; fi
        mkdir -p "$B/held"
        ls "$B/simdisk/Synth" | sort | tail -n "$READD" | while read -r d; do
            mv "$B/simdisk/Synth/$d" "$B/held/"
        done
        shots "$name-gone" "$S2" || rc=$?
        if [ "$rc" != 0 ]; then echo "$rc"; return; fi
        mv "$B/held/"* "$B/simdisk/Synth/"
        shots "$name" "$S2" "$@" || rc=$?
    else
        shots "$name" "$S" "$@" || rc=$?
    fi
    echo "$rc"
}

{
    echo "== commit trace, $N tracks (COMMIT_TRACE_ADD=$ADD," \
         "COMMIT_TRACE_READD=$READD), label $LABEL," \
         "$(git -C "$ROOT" rev-parse --short HEAD)$(git -C "$ROOT" diff --quiet || echo +dirty)"
    echo "lib fingerprint $LIB_FP (compare db digests only at the same one)"
    rc=$(run plain)
    LOG=$OUT/$N-$LABEL-plain.sim.log
    echo "-- plain run (shots rc=$rc)"
    [ "$rc" = 0 ] || echo "FAIL: the plain run's session failed (rc=$rc)"
    grep -E "committrace|commit ms=" "$LOG" | sed 's/^.*\(committrace\)/\1/' || true
    if ! grep -q "committrace ms=" "$LOG"; then
        echo "FAIL: no committrace line (the commit never ran?)"
    fi
    # the tag files and master index of the generation published last
    # (database_gen<n>/), and the shell's library file made from them
    G=$(cd "$B/simdisk/.rockbox" && ls -td database_gen*/ 2>/dev/null | head -1)
    G=${G%/}
    nf=$(ls "$B/simdisk/.rockbox/$G" 2>/dev/null | grep -cE '^database_.*\.(tcd|dat)$' || true)
    echo "-- db sha256 ($nf files in $G)"
    if [ "$nf" -lt 11 ]; then
        echo "FAIL: only $nf database files: nothing to compare"
    else
        (cd "$B/simdisk/.rockbox/$G" && shasum -a 256 database_*.tcd database_*.dat) | sed 's/^/db sha256 /'
        dig=$(cd "$B/simdisk/.rockbox/$G" && cat database_*.tcd database_*.dat | shasum -a 256 | cut -c1-16)
        echo "db digest $dig"
        # what the master index holds (24-byte header, 96-byte entries:
        # 23 tag words and the flags), so a case's data is seen to have
        # its feature (deleted entries, resurrected ones)
        python3 - "$B/simdisk/.rockbox/$G/database_idx.tcd" "$READD" <<'EOF2'
import struct, sys
d = open(sys.argv[1], 'rb').read()
n = struct.unpack_from('<i', d, 8)[0]
fl = [struct.unpack_from('<I', d, 24 + 96 * i + 92)[0] for i in range(n)]
res = sum(1 for f in fl if f & 0x10)
print("db master entries %d deleted %d resurrected %d" % (
    n, sum(1 for f in fl if f & 1), res))
if int(sys.argv[2]) > 0 and res == 0:
    print("FAIL: COMMIT_TRACE_READD but no entry was resurrected")
EOF2
        # COMMIT_TRACE_EXPECT: a reference build's digest (e.g. the
        # parent commit's); any other is a byte difference
        if [ -n "$COMMIT_TRACE_EXPECT" ] && [ "$dig" != "$COMMIT_TRACE_EXPECT" ]; then
            echo "FAIL: db digest $dig, expected $COMMIT_TRACE_EXPECT"
        fi
    fi
    if [ "${COMMIT_TRACE_COUNT:-1}" != 0 ]; then
        T=$OUT/$N-$LABEL.trace
        rm -f "$T"
        rc=$(run traced IPODUI_FSTRACE="$T" IPODUI_FSTRACE_READS=1)
        echo "-- traced run (shots rc=$rc): the device's file layer (fsreplay count)"
        "$FC/fsreplay" count "$T" 32 2048 || echo "FAIL: fsreplay count rc=$?"
        rm -f "$T"
    fi
} 2>&1 | tee "$RES"
if grep -q '^FAIL' "$RES"; then
    exit 1
fi
