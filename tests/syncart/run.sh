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

# Host tests for the cover database reader (#225 S3):
#  - test_artdb: apps/ipodui/artdb_core.c against fixtures from our own
#    test-only writer (fixwrite.c): write -> read back -> compare with the
#    spec; every cut length; nesting, tag and magic damage; bad images
#    skipped and counted (wrong size, the 720x480 TV-out size, names with
#    folders or separators); the song walk (UTF-16 surrogates, UTF-8
#    locations, +352 only in a 0x184 header); 20,000 random byte damages
#    under UBSan;
#  - mkfixture: the fixture CLI end to end, read back by test_artdb's
#    reader through the survey mode.
set -eu
here=$(cd "$(dirname "$0")" && pwd)
top=$(cd "$here/../.." && pwd)
out=${SYNCART_OUT:-$top/build/syncart}
mkdir -p "$out/fx"
HOSTCC=${HOSTCC:-cc}
# ASan hangs in its own start-up on this Mac (macOS 26): UBSan (with array
# bounds) by default; SYNCART_SAN="-fsanitize=address,undefined" to try it
SAN=${SYNCART_SAN--fsanitize=undefined -fno-sanitize-recover=all}

# shellcheck disable=SC2086
"$HOSTCC" -std=gnu99 -Wall -Wextra -Werror -O1 -g $SAN \
    -I"$top/apps/ipodui" -I"$here" -o "$out/test_artdb" \
    "$here/test_artdb.c" "$here/fixwrite.c" "$top/apps/ipodui/artdb_core.c"
"$HOSTCC" -std=gnu99 -Wall -Wextra -Werror -O1 -I"$here" \
    -o "$out/mkfixture" "$here/mkfixture.c" "$here/fixwrite.c"
"$out/test_artdb" "$out/fx"

rm -rf "$out/mk"
printf '%s\n' "/iPod_Control/Music/F00/AAAA.mp3	A" \
    "/iPod_Control/Music/F01/BBBB.mp3	A" \
    "/iPod_Control/Music/F02/CCCC.m4a	B" \
    "/iPod_Control/Music/F03/DDDD.mp3	-" | "$out/mkfixture" "$out/mk"
s=$("$out/test_artdb" --survey "$out/mk/iPod_Control/Artwork")
echo "$s" | grep -q "rc=6 images=6 skipped=0" \
    || { echo "FAIL mkfixture: cover db"; echo "$s"; exit 1; }
echo "$s" | grep -q "format 1029: 200x200, 80000 B, 3 images" \
    || { echo "FAIL mkfixture: 200 px images"; echo "$s"; exit 1; }
echo "$s" | grep -q "rc=4 songs=4 linked(+352)=3 has_art 1/2=3/1" \
    || { echo "FAIL mkfixture: song db"; echo "$s"; exit 1; }
n=$(wc -c <"$out/mk/iPod_Control/Artwork/F1029_1.ithmb" | tr -d ' ')
[ "$n" = 160000 ] || { echo "FAIL mkfixture: F1029 is $n bytes"; exit 1; }
"$out/mkfixture" --ref 1 200 200 "$out/ref1.raw"
cmp -s "$out/ref1.raw" "$out/mk/iPod_Control/Artwork/F1029_1.ithmb" \
    && { echo "FAIL mkfixture: group 1 equals group 0"; exit 1; }
dd if="$out/mk/iPod_Control/Artwork/F1029_1.ithmb" bs=80000 skip=1 \
    count=1 2>/dev/null | cmp -s - "$out/ref1.raw" \
    || { echo "FAIL mkfixture: group 1's pixels differ from --ref"; exit 1; }
echo "syncart mkfixture: 4 songs, 2 covers shared, pixels match --ref"
# the synced-cover map (apps/ipodui/syncart.c) over our own fixtures
# shellcheck disable=SC2086
"$HOSTCC" -std=gnu99 -Wall -Wextra -Werror -O1 -g $SAN -DSYNCART_HOST -DROCKBOX_DIR='"/.rockbox"' \
    -DARTFMT_HOST -I"$top/apps/ipodui" -I"$here" -o "$out/test_map" \
    "$here/test_map.c" "$here/fixwrite.c" "$top/apps/ipodui/syncart.c" \
    "$top/apps/ipodui/artdb_core.c" "$top/apps/ipodui/artfmt.c" \
    "$top/apps/ipodui/artkey.c" "$top/apps/ipodui/sortkey.c"
"$out/test_map" "$out/map"
echo "syncart: PASS"
