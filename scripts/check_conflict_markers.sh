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

# Fails if any tracked text file still carries merge-conflict markers
# (a "<<<<<<< " or ">>>>>>> " line, or a bare "=======" line between them).
# Run by validate.sh. Prints the count of files it checked.
cd "$(git rev-parse --show-toplevel)" || exit 2
n=$(git ls-files | wc -l | tr -d ' ')
[ "$n" -gt 0 ] || { echo "conflict markers: no tracked files (refusing to pass)" >&2; exit 2; }
hits=$(git grep -n -I -E '^(<<<<<<< |>>>>>>> )' -- . ':!scripts/check_conflict_markers.sh')
rc=$?
if [ $rc -eq 0 ]; then
    echo "$hits"
    echo "FAIL: merge-conflict markers in tracked files ($n files checked)"
    exit 1
elif [ $rc -ne 1 ]; then
    echo "conflict markers: git grep failed (rc=$rc)" >&2
    exit 2
fi
echo "ok   no merge-conflict markers ($n files checked)"
