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

# The shipping shape of a ProkBox5g release: ONE place, sourced by
# scripts/release_build.sh (the release zip, locally and in CI), by
# scripts/release_tree.py (the public tree's configurations) and by
# scripts/validate.sh's shipping-shape gate, so they can't drift apart.
# POSIX sh; defines variables only.

# Every release build (add --compiler-prefix=... for the target toolchain).
# --ipodui-release implies the module loader (Extras and games load on
# demand from modules.pak); the --bcmfw* flags are the video chip: video
# player, VC display driver, VC-drawn visualiser, art decode.
RELEASE_SHAPE="--target=22 --type=n --ipodui --ipodui-release --bcmfw --bcmfw-default --bcmfw-video --bcmfw-l1 --bcmfw-merged"

# One release image for every unit since 0.8.3 (owner, 2026-11-11): the
# RAM (#204) and the storage kind (#79) are read at start-up, the fitted
# battery is asked once at first start (#205), and the dock's iAP code is
# resident everywhere (#236). No variant, storage or RAM flags: one zip,
# ProkBox5g-<ver>.zip (scripts/release_build.sh).

# The ProkBox start-up program (bootloader), shipped in the zip.
RELEASE_BOOT="--target=22 --type=b --ipodui"

# Files the release zip must carry (paths inside the zip).
RELEASE_REQUIRED="bootloader-ipodvideo.ipod .rockbox/modules.pak .rockbox/bcmvideo.bin .rockbox/dec-ovl.bin .rockbox/viz-ovl-m.bin .rockbox/vcheck.bin"

# The resident cap: _end in rockbox.elf, decimal bytes (validate.sh
# REL_BCM_END_MAX, the same backstop).
RELEASE_END_MAX=1750000   # 0.9.0 (owner, 2026-11-07): 1.75 MB, held until 1.0; 2.0 MB is a safety reserve
