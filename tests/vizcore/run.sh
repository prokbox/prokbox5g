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

# The visualizer's pure core (apps/ipodui/viz_core.c, #126): band edges,
# the PP's smoothing, gain and peaks, end to end through the CPU path
# (bcmfw/jobs/jviz.c) on synthetic sines.
set -e
cd "$(dirname "$0")/../.."
mkdir -p build/vizcore
cc -std=gnu99 -O1 -Wall -Wextra -Werror -Iapps/ipodui -Ibcmfw/jobs \
   tests/vizcore/vizcore_test.c apps/ipodui/viz_core.c bcmfw/jobs/jviz.c -lm \
   -o build/vizcore/vizcore_test
build/vizcore/vizcore_test
