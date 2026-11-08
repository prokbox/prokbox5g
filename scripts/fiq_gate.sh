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

# rc.6 (layout A) gate step: walk the PCM FIQ's reach on the COP in each
# named build (scripts/cop_shared_check.py --fiq; docs/design/
# cop-shared-state.md, layout A).
#
# usage: scripts/fiq_gate.sh BUILD_DIR...
#
# A gate that inspected nothing must not pass: every named build must have
# its rockbox.elf and rockbox.map, and the walk must run on each.  Three
# outcomes per build, kept apart:
#   ok      the walk ran and found no cached static written and no
#           forbidden call (cop_shared_check.py exit 0);
#   FOUND   the walk ran and found some (exit 1 with its FAIL line): a
#           hazard;
#   NOT RUN the build or its image is missing, or the walk could not run
#           (exit 2: no map, no fiq_playback, no roots; any other exit: the
#           checker itself failed).
# Exit 0 only if every build is ok; prints how many builds and functions
# were checked.
set -u
[ $# -gt 0 ] || { echo "fiq gate: NOT RUN: no build named"; exit 2; }
nb=0; nf=0; found=0; notrun=0
for d in "$@"; do
    if [ ! -f "$d/rockbox.elf" ] || [ ! -f "$d/rockbox.map" ]; then
        echo "fiq gate: NOT RUN $d: rockbox.elf or rockbox.map missing"
        notrun=$((notrun + 1))
        continue
    fi
    out=$(python3 -I scripts/cop_shared_check.py --fiq "$d" 2>&1)
    rc=$?
    case $rc in
    0)
        n=$(echo "$out" | sed -n 's/.*: ok (\([0-9][0-9]*\) functions.*/\1/p' | tail -1)
        if [ -z "$n" ] || [ "$n" -eq 0 ]; then
            echo "$out" | tail -3
            echo "fiq gate: NOT RUN $d: the walk reached no function"
            notrun=$((notrun + 1))
            continue
        fi
        echo "fiq gate: ok      $d: $n functions, no cached static written, no forbidden call"
        nb=$((nb + 1)); nf=$((nf + n)) ;;
    1)
        # exit 1 is also Python's for an uncaught error: a hazard only
        # with the checker's own verdict line
        if echo "$out" | grep -q '^cop shared --fiq: FAIL'; then
            echo "$out" | tail -20
            echo "fiq gate: FOUND   $d: the FIQ reaches shared cached state or a kernel call"
            found=$((found + 1))
        else
            echo "$out" | tail -5
            echo "fiq gate: NOT RUN $d: the walk failed (exit 1, no verdict)"
            notrun=$((notrun + 1))
        fi ;;
    *)
        echo "$out" | tail -5
        echo "fiq gate: NOT RUN $d: the walk failed (exit $rc)"
        notrun=$((notrun + 1)) ;;
    esac
done
echo "fiq gate: $nb of $# builds checked, $nf functions walked; $found with hazards, $notrun not run"
[ $found -eq 0 ] && [ $notrun -eq 0 ]
