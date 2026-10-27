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

# USB storage host tests (#63, #81): the ATA partial-sector cache, the
# read-ahead and prefetch cache bookkeeping, the Bulk-Only Transport
# decisions (resets, failed WRITEs) and the 2048->512 sector view. The mount test needs macOS (hdiutil, diskutil)
# and mtools; elsewhere it is skipped with a note.
set -e
cd "$(dirname "$0")/../.."
mkdir -p build/xlate
cc -w -I. -DHAVE_USB_SECTOR_XLATE tests/usb/ata_cache_test.c \
   -o build/xlate/ata_cache_test
build/xlate/ata_cache_test
# the flash build keeps the drive's own multiplier (1): no partial path
cc -w -I. -DHAVE_USB_SECTOR_XLATE -DHAVE_FLASH_STORAGE \
   tests/usb/ata_cache_test.c -o build/xlate/ata_cache_test_flash
build/xlate/ata_cache_test_flash
cc -w -I. tests/usb/ra_test.c -o build/xlate/ra_test
cc -w -I. tests/usb/pf_test.c -o build/xlate/pf_test
build/xlate/pf_test
build/xlate/ra_test
cc -w -I. tests/usb/bot_test.c -o build/xlate/bot_test
build/xlate/bot_test
cc -Wall -Wextra -I. tests/usb/reset_test.c -o build/xlate/reset_test
build/xlate/reset_test
cc -Wall -Wextra -I. tests/usb/arc_fix_test.c -o build/xlate/arc_fix_test
build/xlate/arc_fix_test
# send_and_read_next(), cut from usb_storage.c as it stands, both paths
awk '/^static void send_and_read_next\(void\)$/ { on = 1 }
     on { print }
     on && /^}$/ { exit }' firmware/usbstack/usb_storage.c \
    > build/xlate/sarn.inc
[ -s build/xlate/sarn.inc ] || { echo "nooverlap_test: function not found"; exit 1; }
for d in -DUSB_STORAGE_NO_READ_OVERLAP -UUSB_STORAGE_NO_READ_OVERLAP; do
    cc -Wall -I. -Ibuild/xlate $d tests/usb/nooverlap_test.c \
       -o build/xlate/nooverlap_test
    build/xlate/nooverlap_test
done
if [ "$(uname)" = Darwin ] && command -v mformat >/dev/null 2>&1; then
    tests/usb/xlate_mount_test.sh
else
    echo "note: xlate mount test skipped (needs macOS and mtools)"
fi
