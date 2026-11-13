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

# #79, #204 (one build): the four cells of the pixel-identical rule,
# 64/32 MB x flash/hdd on the one sim binary. Runs
# tests/ui/ipodui-onebuild-disk-flash.script (the sim's flash model) and
# ipodui-onebuild-disk-hdd.script (IPODUI_SIM_DISK=hdd), same body, each
# with its own log checks (its "if (solid)" rows), once per RAM
# (IPODUI_SIM_RAM; the run's "sim: ram <N> MB" line must name it), then
# requires every shot pixel-identical to the 64 MB flash cell's.
# Every cell boots a fresh clone of the same primed disk
# (build/tmp/onebuild-cells/disk-<cell>, SIMDISK): the panel's cover
# rotation and the art cache persist on the disk, so cells sharing one
# disk drew different covers by run order.
#
#   SIM_BUILD=build/sim-ui tests/onebuild/disk_cells.sh
#   RAMS=64 ... (one RAM only)
#
# Exit 0 only when every run passes, all made the same non-empty set of
# shots, and every pair has 0 differing pixels.
cd "$(dirname "$0")/../.." || exit 2
export SIM_BUILD=${SIM_BUILD:-build/sim-ui}
out=out/shots/onebuild-disk-cells
rams=${RAMS:-64 32}
rc=0
mkdir -p "$out"
base=$SIM_BUILD/simdisk
disks=build/tmp/onebuild-cells    # setup_music.sh writes under build/ only
mkdir -p "$disks"
[ -d "$base/.rockbox/codecs" ] || make -C "$SIM_BUILD" install >/dev/null || exit 2
tests/ui/setup_music.sh "$base" >/dev/null || exit 2
cells=""
for ram in $rams; do
    for disk in flash hdd; do
        cell=$disk-$ram
        cells="$cells $cell"
        script="tests/ui/ipodui-onebuild-disk-$disk.script"
        rm -rf "${out:?}/$cell" "${disks:?}/disk-$cell"   # no stale shots or disk
        if ! { [ "$(uname)" = Darwin ] && /bin/cp -c -R "$base" "$disks/disk-$cell" 2>/dev/null; }; then
            cp -pR "$base" "$disks/disk-$cell" || exit 2
        fi
        SIMDISK=$disks/disk-$cell IPODUI_SIM_RAM=$ram \
            scripts/shots.sh "$script" "$out/$cell" \
            > "$out/$cell-shots.log" 2>&1
        r=$?
        echo "cell $cell: shots.sh exit $r"
        [ $r -eq 0 ] || rc=1
        # the script's own log rules (its "if (solid)" rows); a missing
        # log is a failure, not a pass
        if [ ! -s "$out/$cell/sim.log" ]; then
            echo "FAIL cell $cell: no sim.log"
            rc=1
            continue
        fi
        if ! grep -q "^sim: ram $ram MB" "$out/$cell/sim.log"; then
            echo "FAIL cell $cell: the run is not the $ram MB unit"
            rc=1
        fi
        python3 scripts/check_trace.py "$out/$cell/sim.log" "$script" \
            > "$out/$cell-trace.log" 2>&1
        r=$?
        nrules=$(grep -cE '^ok +(no)?log(range)? ' "$out/$cell-trace.log")
        echo "cell $cell: check_trace exit $r, $nrules log rules ok"
        grep -E '^(FAIL|bad|BAD)' "$out/$cell-trace.log"
        [ $r -eq 0 ] && [ "$nrules" -ge 6 ] || rc=1
    done
done
ref=${cells# }
ref=${ref%% *}
nref=$(ls "$out/$ref"/*.png 2>/dev/null | wc -l | tr -d ' ')
n=0
for cell in $cells; do
    [ "$cell" = "$ref" ] && continue
    nc=0
    for a in "$out/$ref"/*.png; do
        [ -e "$a" ] || continue
        b="$out/$cell/$(basename "$a")"
        if [ ! -e "$b" ]; then
            echo "FAIL $cell $(basename "$a"): no shot"
            rc=1
            continue
        fi
        n=$((n + 1))
        nc=$((nc + 1))
        if ! scripts/pngdiff.py "$a" "$b" > /dev/null; then
            echo "FAIL $cell $(basename "$a"): $(scripts/pngdiff.py "$a" "$b")"
            rc=1
        fi
    done
    nb=$(ls "$out/$cell"/*.png 2>/dev/null | wc -l | tr -d ' ')
    echo "cell $cell: $nc of $nb shots compared to $ref ($nref)"
    if [ "$nc" -eq 0 ] || [ "$nb" -ne "$nref" ]; then
        echo "FAIL shot sets: $ref has $nref, $cell has $nb"
        rc=1
    fi
done
echo "compared $n shot pairs against $ref"
[ $rc -eq 0 ] && echo "DISK CELLS PASSED" || echo "DISK CELLS FAILED"
exit $rc
