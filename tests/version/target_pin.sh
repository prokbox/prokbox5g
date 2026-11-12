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

# The simulator pins its displayed version to "0.0.0-sim" (apps/ipodui/prodver.h);
# a target build (firmware, bench, road-test, emulator image) must never contain it.
#   tests/version/target_pin.sh <target-build-dir>... [--sim <sim-build-dir>...]
# Each target dir's rockbox.elf must NOT hold the literal; each sim dir's rockboxui
# MUST (positive control: proves the literal is what we search for).
PIN="0.0.0-sim"
rc=0; n=0; mode=target
while [ $# -gt 0 ]; do
    case "$1" in
        --sim) mode=sim ;;
        *)
            if [ $mode = sim ]; then f="$1/rockboxui"; else f="$1/rockbox.elf"; fi
            if [ ! -f "$f" ]; then echo "FAIL target_pin: $f missing (nothing inspected)"; rc=1
            elif LC_ALL=C grep -aq -- "$PIN" "$f"; then
                if [ $mode = sim ]; then echo "ok   sim binary $f holds $PIN"; n=$((n+1))
                else echo "FAIL target_pin: target binary $f contains \"$PIN\" (the simulator pin leaked)"; rc=1; fi
            else
                if [ $mode = sim ]; then echo "FAIL target_pin: sim binary $f lacks \"$PIN\" (control)"; rc=1
                else echo "ok   target binary $f has no $PIN"; n=$((n+1)); fi
            fi ;;
    esac
    shift
done
[ $n -gt 0 ] || { echo "FAIL target_pin: no binaries inspected"; rc=1; }
exit $rc
