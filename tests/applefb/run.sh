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

# Host tests for the Apple fallback (#137):
#  - test_core: apps/ipodui/afb_core.c, the database writer byte for byte
#    against the reference writer (tools/itunesdb/mkitdb.py --golden on
#    the same spec), the verifier (cut, flipped, fuzzed), the link verify
#    rule, the park cycle's plan with a power cut at every step, records;
#  - linktest: the firmware's fat.c + file layer + apps/ipodui/afb_fat.c
#    on a FAT32 image (tests/fatcrash's shim): links, the remove() guard,
#    the park, FREED and EMPTIED repairs; then fsck_msdos -n (macOS) as
#    the independent oracle: a live link is a cross-link it reports, the
#    parked volume is clean (docs/research/apple-fallback-links.md H4).
set -eu
here=$(cd "$(dirname "$0")" && pwd)
top=$(cd "$here/../.." && pwd)
out=${APPLEFB_OUT:-$top/build/applefb}
mkdir -p "$out/inc"
HOSTCC=${HOSTCC:-cc}

"$HOSTCC" -std=gnu99 -Wall -Wextra -Werror -O1 -I"$top/apps/ipodui" \
    -o "$out/test_core" "$here/test_core.c" "$top/apps/ipodui/afb_core.c"
"$out/test_core" --spec "$out/spec.json"
python3 "$top/tools/itunesdb/mkitdb.py" --golden "$out/spec.json" \
    "$out/golden.itdb" >/dev/null
"$out/test_core" --golden "$out/golden.itdb"

for d in firmware/export firmware/include firmware/kernel/include; do
    cp "$top/$d"/*.h "$out/inc/"
done
cp "$top"/tests/fatcrash/shim/*.h "$out/inc/"
fs="$top/firmware/common"
CFLAGS="-std=gnu99 -O1 -w -I$out/inc -I$top/apps -DHAVE_IPOD_UI_DEVMODE"
fl="$fs/file.c $fs/file_internal.c $fs/fileobj_mgr.c $fs/dir.c \
    $fs/pathfuncs.c $fs/rb_namespace.c $fs/fat.c $fs/disk_cache.c \
    $fs/linked_list.c"
# shellcheck disable=SC2086
"$HOSTCC" $CFLAGS -DFATCRASH_FILE_LAYER -o "$out/mkfs" \
    "$top/tests/fatcrash/fatcrash.c" $fl
# shellcheck disable=SC2086
"$HOSTCC" $CFLAGS -o "$out/linktest" "$here/linktest.c" \
    "$top/apps/ipodui/afb_fat.c" $fl
"$out/mkfs" mkfs "$out/link.img" 167936 >/dev/null
"$out/linktest" "$out/link.img" "$out"

if [ -x /sbin/fsck_msdos ]; then
    live=$(/sbin/fsck_msdos -n "$out/live.img" 2>&1 || true)
    parked=$(/sbin/fsck_msdos -n "$out/parked.img" 2>&1 || true)
    if ! printf '%s\n' "$live" | grep -qi "cross-linked"; then
        echo "FAIL fsck: a live link is not reported as a cross-link"
        printf '%s\n' "$live"; exit 1
    fi
    if printf '%s\n' "$parked" | grep -qiE "cross-linked|lost|free cluster|should"; then
        echo "FAIL fsck: the parked volume is not clean"
        printf '%s\n' "$parked"; exit 1
    fi
    echo "applefb fsck: live link reported, parked volume clean"
else
    echo "applefb fsck: skipped (no fsck_msdos)"
fi
echo "applefb: PASS"
