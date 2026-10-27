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

# Real-file statistics journal tests, isolated from simulator and device.
set -eu
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
cd "$ROOT"
TASK_TMP=$(mktemp -d)
trap 'rm -rf "$TASK_TMP"' EXIT
# the safe replace, its renames through the journal's fault hooks
"${HOSTCC:-cc}" -std=gnu99 -O1 -g -Wall -Wextra -Werror -DSAFEFILE_HOST \
    -Drename=safefile_test_rename -c apps/ipodui/safefile.c \
    -o "$TASK_TMP/safefile.o"
"${HOSTCC:-cc}" -std=gnu99 -O1 -g -Wall -Wextra -Werror -DDBJOURNAL_HOST \
    -Iapps/ipodui tests/dbjournal/test_dbjournal.c apps/ipodui/dbjournal.c \
    -o "$TASK_TMP/test_dbjournal" "$TASK_TMP/safefile.o"
"$TASK_TMP/test_dbjournal" "$TASK_TMP"
python3 tests/dbjournal/test_recovery.py
