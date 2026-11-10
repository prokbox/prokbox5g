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

# Storage commands of a synced library (iPod_Control/Music/Fnn, hashed
# 8.3 names) against the drag-and-drop tree, on the firmware's own file
# layer over a RAM FAT32 image (tests/fatcrash/dirburst.c "ipc"/"tree"):
# the scan's walk, an open of every track, and the art builder's stock
# image-file search per album (#225).
#
#   tests/perf/ipodctl_fat.sh [tracks...]      (default 5400 65000)
#
# Geometry 32:2048 (16 KiB clusters, the 5.5G's 2048-byte BPB sectors).
# Prints one block per (layout, tracks): with bursts as shipped (8 slots of
# 16 KiB since #225; 2 before), without (-DFAT_NO_DIR_BURST, upstream's
# one sector per command), and with $SLOTS slots (default 2 4 6;
# FAT_DIR_BURST_SLOTS: 2 is 0.8.0's count, the "before" column).
set -e
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
here=$ROOT/tests/fatcrash
out=$ROOT/build/tmp/ipodctl-fat
mkdir -p "$out/inc"
for d in firmware/export firmware/include firmware/kernel/include; do
    cp "$ROOT/$d"/*.h "$out/inc/"
done
cp "$here"/shim/*.h "$out/inc/"
CFLAGS="-std=gnu99 -O1 -w -I$out/inc"
fs=$ROOT/firmware/common
srcs="$here/dirburst.c $fs/file.c $fs/file_internal.c $fs/fileobj_mgr.c \
    $fs/dir.c $fs/pathfuncs.c $fs/rb_namespace.c $fs/fat.c \
    $fs/disk_cache.c $fs/linked_list.c"
# shellcheck disable=SC2086
cc $CFLAGS -o "$out/dirburst" $srcs
# shellcheck disable=SC2086
cc $CFLAGS -DFAT_NO_DIR_BURST -o "$out/dirburst_off" $srcs
SLOTS=${SLOTS:-2 4 6}
for k in $SLOTS; do
    # shellcheck disable=SC2086
    cc $CFLAGS -DFAT_DIR_BURST_SLOTS=$k -o "$out/dirburst_$k" $srcs
done
[ $# -gt 0 ] || set -- 5400 65000
for n in "$@"; do
    for lay in ipc tree; do
        echo "== $lay $n (bursts on)"
        "$out/dirburst" $lay "$n" 32 2048 | sed 1d
        echo "== $lay $n (bursts off)"
        "$out/dirburst_off" $lay "$n" 32 2048 | sed 1d
        for k in $SLOTS; do
            echo "== $lay $n ($k burst slots)"
            "$out/dirburst_$k" $lay "$n" 32 2048 | sed 1d
        done
    done
done
