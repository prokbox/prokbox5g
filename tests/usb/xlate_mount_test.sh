#!/bin/bash
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

# End-to-end check of the USB 512-byte view (#63): a FAT32 disk laid out
# like the iPod 5.5G's (MBR and FAT in 2048-byte sectors), shown through
# firmware/usbstack/usb_storage_xlate.c, must mount on this Mac as an
# ordinary 512-byte disk; a file written there must be on the 2048-byte
# disk afterwards, readable with the disk's own geometry (mtools).
#   tests/usb/xlate_mount_test.sh            (macOS; needs mtools)
set -euo pipefail
cd "$(dirname "$0")/../.."
B=build/xlate; mkdir -p "$B"; W=$(cd "$(mktemp -d "$B/run.XXXX")" && pwd)
cc -O2 -Wall -Wextra -Werror -Ifirmware/usbstack \
   firmware/usbstack/usb_storage_xlate.c tests/usb/xlate_tool.c -o "$B/xlate_tool"
T="$B/xlate_tool"
DEV=; cleanup() { [ -n "$DEV" ] && hdiutil detach -quiet "$DEV" 2>/dev/null; rm -rf "$W"; }
trap cleanup EXIT

START=32                        # partition start, in 2048-byte sectors
PSIZE=$((640 * 512))            # 640 MiB in 2048-byte sectors
# the FAT32 volume itself: 2048-byte sectors, 8 KiB clusters
mkfile -n $((PSIZE * 2048)) "$W/part.img"
# (mtools: -S 4 is 128 << 4 = 2048 bytes per sector)
mformat -i "$W/part.img" -F -S 4 -c 4 -T $PSIZE -H $START -h 255 -s 63 -v PROKTEST ::
# the disk: an MBR in 2048-byte units (as Apple's 5.5G layout), then the volume
python3 - "$W/disk.img" "$W/part.img" $START $PSIZE <<'PY'
import struct, sys
disk, part, start, size = sys.argv[1], sys.argv[2], int(sys.argv[3]), int(sys.argv[4])
mbr = bytearray(2048)
mbr[0x1be:0x1ce] = bytes([0, 0, 0, 0, 0x0b, 0, 0, 0]) + struct.pack('<II', start, size)
mbr[510:512] = b'\x55\xaa'
with open(disk, 'wb') as d, open(part, 'rb') as p:
    d.write(mbr); d.write(bytes(2048 * (start - 1)))
    while True:
        c = p.read(1 << 20)
        if not c: break
        d.write(c)
PY
rm "$W/part.img"
OFF=$((START * 2048))
echo "hello from the 2048 side" > "$W/a.txt"
mcopy -i "$W/disk.img@@$OFF" "$W/a.txt" ::a.txt

"$T" selftest "$W/disk.img" 4
"$T" view "$W/disk.img" "$W/view.img" 4
cp "$W/disk.img" "$W/disk.orig"
"$T" back "$W/disk.img" "$W/view.img" 4
cmp -s "$W/disk.img" "$W/disk.orig" && echo "ok   view written back unchanged is identical" || { echo "FAIL round trip"; exit 1; }
rm "$W/disk.orig"

DEV=$(hdiutil attach -imagekey diskimage-class=CRawDiskImage -nomount "$W/view.img" | awk 'NR==1{print $1}')
echo "attached $DEV (512-byte blocks: $(diskutil info "$DEV" | awk -F: '/Device Block Size/{gsub(/ /,"",$2); print $2}'))"
diskutil mount -mountPoint "$W/mnt" "${DEV}s1" >/dev/null 2>&1 || { mkdir -p "$W/mnt"; diskutil mount -mountPoint "$W/mnt" "${DEV}s1"; }
echo "ok   macOS mounted the 512-byte view"
grep -q "hello from the 2048 side" "$W/mnt/a.txt" && echo "ok   a file written on the 2048 side reads on the Mac"
echo "hello from the Mac" > "$W/mnt/b.txt"
mkdir "$W/mnt/dir"; head -c $((24 << 20)) /dev/urandom > "$W/mnt/dir/big.bin"
SUM=$(md5 -q "$W/mnt/dir/big.bin")
diskutil unmount "$W/mnt" >/dev/null
fsck_msdos -n "${DEV}s1" >/dev/null 2>&1 && echo "ok   fsck_msdos on the 512-byte view is clean" || { echo "FAIL fsck on the view"; exit 1; }
hdiutil detach -quiet "$DEV"; DEV=

"$T" back "$W/disk.img" "$W/view.img" 4
echo "ok   the Mac's writes went back through the translation"
mtype -i "$W/disk.img@@$OFF" ::b.txt | grep -q "hello from the Mac" && echo "ok   the Mac's file reads with the 2048-byte geometry"
mcopy -i "$W/disk.img@@$OFF" ::dir/big.bin "$W/big.out"
[ "$(md5 -q "$W/big.out")" = "$SUM" ] && echo "ok   24 MiB file intact on the 2048 side"
python3 - "$W/disk.img" $START <<'PY'
import struct, sys
d = open(sys.argv[1], 'rb'); start = int(sys.argv[2])
mbr = d.read(512); assert struct.unpack('<I', mbr[0x1c6:0x1ca])[0] == start
d.seek(start * 2048); bs = d.read(512)
assert struct.unpack('<H', bs[11:13])[0] == 2048 and bs[13] == 4, (bs[11:14])
print("ok   the disk's MBR and boot sector still state 2048-byte sectors")
PY
echo "XLATE MOUNT TEST PASSED"
