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
# scripts/release_build.sh (the release zips, locally and in CI) and by
# scripts/validate.sh's shipping-shape gate, so they can't drift apart.
# POSIX sh; defines variables only.

# Every release build (add --compiler-prefix=... for the target toolchain).
# --ipodui-release implies the module loader (Extras and games load on
# demand from modules.pak); the --bcmfw* flags are the video chip: video
# player, VC display driver, VC-drawn visualiser, art decode.
RELEASE_SHAPE="--target=22 --type=n --ipodui --ipodui-release --bcmfw --bcmfw-default --bcmfw-video --bcmfw-l1 --bcmfw-merged"

# The public variants (owner, 2026-10-26), each for --storage=flash|hdd and
# --ram=64|32: stock = stock battery, 30-pin dock, no haptics; modded = the
# owner's mods, 3000 mAh, haptics, USB-C (no dock).
RELEASE_STOCK="--battery=stock --dock30 --no-haptics"
RELEASE_MODDED="--battery=3000 --haptics"

# The ProkBox start-up program (bootloader) shipped in every zip.
RELEASE_BOOT="--target=22 --type=b --ipodui"

# Files every release zip must carry (paths inside the zip).
RELEASE_REQUIRED="bootloader-ipodvideo.ipod .rockbox/modules.pak .rockbox/bcmvideo.bin .rockbox/dec-ovl.bin .rockbox/viz-ovl-m.bin .rockbox/vcheck.bin"

# The resident cap: _end in rockbox.elf, decimal bytes (validate.sh
# REL_BCM_END_MAX, the same backstop).
RELEASE_END_MAX=1500000
