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
# tests/fatlink helpers: scratch FAT32 images, mount/unmount, hashes.
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
# Only ever touches images under $WORK (default build/fatlink).  Never the
# iPod: every device node used here comes from our own `hdiutil attach`.

ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)
WORK=${WORK:-$ROOT/build/fatlink}
FL="python3 $ROOT/tools/fatlink/fatlink.py"
# DRIVER=fskit: diskutil (DiskArbitration, the FSKit msdos driver: what a
#               plugged-in iPod gets), 512-byte BPB sectors.
# DRIVER=kext:  mount -t msdos (the legacy kernel driver), 2048-byte BPB
#               sectors like the owner's volume.
DRIVER=${DRIVER:-fskit}
IMG=$WORK/img-$DRIVER.img
MNT=$WORK/mnt-$DRIVER
DEV=
mkdir -p "$WORK" "$MNT"

log() { echo "$*" | tee -a "$WORK/results-$DRIVER.log"; }

attach() {
    DEV=$(hdiutil attach -imagekey diskimage-class=CRawDiskImage -nomount "$IMG" | awk 'NR==1{print $1}')
    case "$DEV" in /dev/disk*) ;; *) echo "attach failed" >&2; exit 2;; esac
}

detach() {
    [ -n "$DEV" ] && hdiutil detach "$DEV" >/dev/null 2>&1
    DEV=
}

mkimg() {
    detach
    rm -f "$IMG"
    mkfile -n 2g "$IMG"
    attach
    if [ "$DRIVER" = kext ]; then
        newfs_msdos -F 32 -S 2048 -c 8 -v PKBTEST "$DEV" >/dev/null 2>&1
    else
        newfs_msdos -F 32 -S 512 -c 32 -v PKBTEST "$DEV" >/dev/null 2>&1
    fi
    detach
}

mnt() {
    attach
    if [ "$DRIVER" = kext ]; then
        mount -t msdos "$DEV" "$MNT" || { echo "mount failed" >&2; exit 2; }
    else
        diskutil mount -mountPoint "$MNT" "$DEV" >/dev/null || { echo "mount failed" >&2; exit 2; }
    fi
}

umnt() {
    sync
    if [ "$DRIVER" = kext ]; then umount "$MNT"; else diskutil unmount "$DEV" >/dev/null; fi
    detach
}

h() { [ -f "$1" ] && shasum -a 1 "$1" | awk '{print substr($1,1,12)}' || echo "-"; }
himg() { $FL "$IMG" cat "$1" 2>/dev/null | shasum -a 1 | awk '{print substr($1,1,12)}'; }

fsckn() {  # fsck_msdos -n on the image file; one-line summary
    fsck_msdos -n "$IMG" 2>&1 | grep -v -E '^\*\* (/|Phase)' | tr '\n' ' ' | sed 's/  */ /g'
}

cleanup_all() {
    if [ -n "$DEV" ]; then
        if [ "$DRIVER" = kext ]; then umount "$MNT" 2>/dev/null; else diskutil unmount force "$DEV" >/dev/null 2>&1; fi
        detach
    fi
}
trap cleanup_all EXIT
