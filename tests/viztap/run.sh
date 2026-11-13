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

# The visualizer's PCM tap (docs/research/bcm2722/visualizer.md section
# 11): host test of apps/ipodui/viz_tap_core.c and viz_tap.c (over
# tests/viztap/stub), with bcmfw/jobs/jviz.c as the CPU path.
set -e
cd "$(dirname "$0")/../.."
mkdir -p build/viztap
# #295: the stub's top rate must be the device's (a 48 kHz stub hid the
# 96 kHz ring, twice the size, from this test and the arena's bound)
grep -q 'HW_SAMPR_CAPS   (HW_SAMPR_CAPS_STD | SAMPR_CAP_96 | SAMPR_CAP_88)' \
    firmware/export/config/ipodvideo.h \
    && grep -q '^#define HW_SAMPR_MAX 96000' tests/viztap/stub/pcm_sampr.h \
    || { echo "viztap: stub HW_SAMPR_MAX differs from ipodvideo.h's top rate"; exit 1; }
cc -std=gnu99 -O1 -Wall -Wextra -Werror -Wno-unused-parameter \
   -Itests/viztap/stub -Iapps/ipodui -Ibcmfw/jobs \
   tests/viztap/viztap_test.c apps/ipodui/viz_tap.c bcmfw/jobs/jviz.c -lm \
   -o build/viztap/viztap_test
build/viztap/viztap_test
