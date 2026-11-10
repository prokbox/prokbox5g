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

# p081-bootload: apps/ipodui/crcfast.c gives exactly firmware's crc_32()
# and crc_32r() (firmware/common/crc32.c as the device builds it, the
# nibble tables), at every length, alignment and starting value.
set -eu
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
cd "$ROOT"
TASK_TMP=$(mktemp -d)
trap 'rm -rf "$TASK_TMP"' EXIT
"${HOSTCC:-cc}" -std=gnu99 -O2 -Wall -Wextra -Werror \
    -Iapps/ipodui -Ifirmware/include -Itests/crcfast \
    tests/crcfast/test_crcfast.c apps/ipodui/crcfast.c \
    firmware/common/crc32.c -o "$TASK_TMP/test_crcfast"
"$TASK_TMP/test_crcfast"
