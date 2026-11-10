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

# The 50k library's database key (the 32 MB stress ceiling, #229):
#
#   tests/perf/ensure50k.sh [SOURCE_SIMDISK]   (default build/sim-50k/simdisk)
#
# tests/perf/ensure65k.sh on the 50k disk; it re-primes it with
# tests/perf/lib50k.sh when stale.
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
exec "$ROOT/tests/perf/ensure65k.sh" "${1:-$ROOT/build/sim-50k/simdisk}"
