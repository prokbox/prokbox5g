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

# tests/fatcrash/powercut.sh: power cuts at the named write points of the
# crash-safe saves (#177, sim-fidelity S2), on the real code paths.
# For each case: a simulator session (scripts/shots.sh, file trace on)
# runs until "powercut at <point> [file]" kills it, then boots again on
# the same disk and checks the art; tests/fatcrash/fsreplay "cut" replays
# the trace on the firmware's FAT code and cuts the power after every
# storage write since the point before (fsck -n with no chain fault, every
# hidden file reads to its size, after the cut and after the next boot).
#   tests/fatcrash/powercut.sh [case...]    (default: every case)
# A case is <point>[:<file>][@<flow>]; flows: art (a fresh art build, the
# default), regroup (an ungrouped panels.pak: the whole-file rewrite),
# db (a fresh database build), resume (a playlist's resume save).
# SIM_BUILD (default build/sim-ui). Needs a built simulator.
set -e
here=$(cd "$(dirname "$0")" && pwd)
top=$(cd "$here/../.." && pwd)
out=${POWERCUT_OUT:-$top/build/powercut}
SIM_BUILD=${SIM_BUILD:-build/sim-ui}
export SIM_BUILD
mkdir -p "$out/fatcrash/inc"
fs="$top/firmware/common"
for d in firmware/export firmware/include firmware/kernel/include; do
    cp "$top/$d"/*.h "$out/fatcrash/inc/"
done
cp "$here"/shim/*.h "$out/fatcrash/inc/"
# shellcheck disable=SC2086
cc -std=gnu99 -O1 -w -I"$out/fatcrash/inc" -DMAX_VIRT_SECTOR_SIZE=2048 \
   -DDEFAULT_VIRT_SECTOR_SIZE=2048 -o "$out/fsreplay" "$here/fsreplay.c" \
   "$fs/fat.c" "$fs/disk_cache.c" "$fs/linked_list.c" "$fs/file.c" \
   "$fs/file_internal.c" "$fs/fileobj_mgr.c" "$fs/dir.c" "$fs/pathfuncs.c" \
   "$fs/rb_namespace.c"

cases=${*:-"art.panels.sealed:panels.pak art.moved:panels.pak \
art.renamed:panels.pak art.panels.slot:panels.pak \
art.panels.commit:panels.pak art.renamed:index.dat \
art.moved:panels.pak@regroup art.renamed:panels.pak@regroup \
db.removed@db db.renamed@db \
cfg.moved:.resume.cfg@resume cfg.renamed:.resume.cfg@resume"}

# the cut boot's session per flow; ARM marks where the point is armed.
# regroup: an ungrouped set, then the paused idle window that rewrites
# panels.pak whole through its private copy (ipodui-p16-panels-stopped)
flow_lines() {
    case "$1" in
    art) printf '%s\n' "wait_db 60000" ARM "art_wait 240000" ;;
    regroup) printf '%s\n' "wait_db 60000" "art_wait 240000" ARM \
                 art_panels_ungroup "wheel +4" "wait 500" "key SELECT" \
                 "wait 2000" "key PLAY" "wait 1500" backlight_expire \
                 "art_wait 60000" "pause_age 200" "wait 3000" \
                 "art_wait 120000" ;;
    db) printf '%s\n' "wait_db 60000" ARM "rebuild_db 120000" ;;
    resume) printf '%s\n' "wait_db 60000" ARM "key SELECT" "wait 800" \
                "wheel +2 300" "wait 400" "key SELECT" "wait 900" \
                "key SELECT" "wait 900" "key SELECT" "wait 3000" \
                "key PLAY" "wait 1500" "key PLAY" "wait 3000" ;;
    esac
}

# the checker's teeth: a live file truncated in place (the chain's tail
# freed before the size follows) must fail; the same file written whole
# and closed must pass. Both before any simulator run.
printf 'boot\t0\t1\nopen\t3\t/.rockbox/a.dat\t601\t1\nwrite\t3\t300000\t1\nclose\t3\t1\npoint\t0\tA\tx\t1\nopen\t3\t/.rockbox/a.dat\t1\t1\nftruncate\t3\t50000\t1\nfsync\t3\t1\nclose\t3\t1\npoint\t0\tB\tx\t1\npowercut\t0\tB\tx\t1\n' \
    >"$out/control-trunc.trace"
if "$out/fsreplay" cut "$out/control-trunc.trace" 8 >"$out/control.log" 2>&1; then
    echo "powercut: negative control (in-place truncate) passed: no teeth"
    exit 1
fi
grep -q 'should at most be\|UNREADABLE' "$out/control.log" \
    || { echo "powercut: negative control failed for another reason"; cat "$out/control.log"; exit 1; }
echo "powercut: negative control (in-place truncate) caught:" \
     "$(grep -m1 UNREADABLE "$out/control.log" | sed 's/^ *//')"
printf 'boot\t0\t1\nopen\t3\t/.rockbox/a.dat\t601\t1\nwrite\t3\t300000\t1\nfsync\t3\t1\nclose\t3\t1\npoint\t0\tA\tx\t1\nopen\t3\t/.rockbox/b.dat\t601\t1\nwrite\t3\t70000\t1\nclose\t3\t1\npoint\t0\tB\tx\t1\npowercut\t0\tB\tx\t1\n' \
    >"$out/control-write.trace"
FSREPLAY_REORDER=1 "$out/fsreplay" cut "$out/control-write.trace" 8 >"$out/control.log" 2>&1 \
    || { echo "powercut: positive control (a new file) failed"; cat "$out/control.log"; exit 1; }

# prime the library disk first (shots.sh's priming boot builds the art
# too), so each case's own boot is the one that builds it
printf '%s\n' "# needs: library" "# golden: skip" "wait 1000" \
    "wait_db 60000" "shot end" "quit" >"$out/ipodui-powercut-prime.script"
"$top/scripts/shots.sh" "$out/ipodui-powercut-prime.script" \
    "$out/shots-prime" >"$out/prime.log" 2>&1 \
    || { echo "powercut: priming the library disk failed ($out/prime.log)"; exit 1; }

pass=0 fail=0
for c in $cases; do
    flow=art
    case "$c" in *@*) flow=${c#*@}; c=${c%@*} ;; esac
    point=${c%%:*} file=
    case "$c" in *:*) file=${c#*:} ;; esac
    tag=$(echo "$point-$file-$flow" | tr -c 'A-Za-z0-9.\n-' '_')
    trace="$out/$tag.trace"
    script="$out/ipodui-powercut-$tag.script"
    rm -f "$trace" "$trace.img"
    {
        echo "# needs: library"
        echo "# golden: skip"
        echo "# timeout: 700"
        echo "# log: powercut hit $point"
        echo "# nolog: expect_panels_valid failed"
        echo "wait 1000"
        flow_lines "$flow" | sed "s|^ARM\$|powercut at $point $file|"
        echo "wait 2000"
        echo "reboot"
        echo "wait 1000"
        echo "wait_db 60000"
        echo "art_wait 240000"
        echo "expect_panels_valid"
        echo "shot end"
        echo "quit"
    } >"$script"
    # art flows start from no art cache, so the build (and its saves) runs
    case "$flow" in art|regroup)
        rm -rf "$top/$SIM_BUILD/simdisk/.rockbox/artcache" ;;
    esac
    log="$out/$tag.shots.log"
    if ! IPODUI_FSTRACE="$trace" IPODUI_FSTRACE_APPEND=1 \
            "$top/scripts/shots.sh" "$script" "$out/shots-$tag" >"$log" 2>&1; then
        echo "powercut $c@$flow: simulator run FAILED ($log)"
        grep -E 'FAIL|failed|missing|not reached' "$log" | head -5
        fail=$((fail + 1))
        continue
    fi
    # in order, then with the drive cache's reorders (FSREPLAY_REORDER)
    if "$out/fsreplay" cut "$trace" >"$out/$tag.cut.log" 2>&1 \
        && FSREPLAY_REORDER=1 "$out/fsreplay" cut "$trace" \
               >>"$out/$tag.cut.log" 2>&1; then
        echo "powercut $c@$flow: ok: $(grep -c '^fsreplay cut .*cut points' "$out/$tag.cut.log") runs," \
             "$(tail -1 "$out/$tag.cut.log")"
        pass=$((pass + 1))
    else
        echo "powercut $c@$flow: FAILED"
        grep -E 'FAIL|UNREADABLE|fsck:|no powercut|nothing' "$out/$tag.cut.log" | head -8
        fail=$((fail + 1))
    fi
done
echo "powercut: $pass passed, $fail failed"
[ "$fail" -eq 0 ] && [ "$pass" -gt 0 ]
