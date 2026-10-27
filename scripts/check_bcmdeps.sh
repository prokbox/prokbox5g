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

# Regression check: editing bcmfw/jobs/jref.h must rebuild every object that
# includes it (-idirafter dirs are system dirs, so -MMD/-MM never list them;
# firmware.make / uisimulator.make name them explicitly).  Dry run only
# ("make -n -W"), touches nothing.   usage: check_bcmdeps.sh build/<dir>...
# A dir without bcmfw objects (no bcmjob-ref.c compiled) is skipped.
rc=0
for d in "$@"; do
    [ -f "$d/Makefile" ] || { echo "check_bcmdeps: $d: no Makefile" >&2; rc=1; continue; }
    root=$(cd "$d/../.." && pwd)
    out=$(cd "$d" && make -n -W "$root/bcmfw/jobs/jref.h" 2>&1 | grep -E 'CC .*(bcmjob-ref|sim_bcm_job)\.c')
    if [ -z "$out" ]; then
        if [ -f "$d/firmware/target/arm/ipod/video/bcmjob-ref.o" ]; then
            echo "FAIL check_bcmdeps: $d: touching bcmfw/jobs/jref.h does not rebuild bcmjob-ref.o (stale-object risk, see firmware/firmware.make)" >&2
            rc=1
        fi
    else
        echo "ok   check_bcmdeps: $d"
    fi
done
exit $rc
