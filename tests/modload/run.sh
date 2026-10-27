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

# Host test for the shell module loader (#106):
#  - its pure half, apps/ipodui/modload_core.c, and, when build/fw-ui-mod
#    (or build/fw-ui) has a modules.pak, the real pack against modpak.py's
#    two links (test_modload.c);
#  - the target's load path itself, modload.c built with IPODUI_MODLOAD
#    against host stand-ins (host/), on that real pack, in the developer
#    and the release arena (test_modload_pak.c). The simulator links the
#    modules statically, so this is the only coverage the pack open,
#    compaction, reads, moves and header checks get off the device.
set -u
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
cd "$ROOT"
T=tests/modload
if [ -z "${HOSTCC:-}" ]; then
    HOSTCC=cc
    for v in 19 18 17 16 15 14; do
        if command -v gcc-$v >/dev/null 2>&1; then HOSTCC=gcc-$v; break; fi
    done
fi
TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT
if ! "$HOSTCC" -std=gnu99 -O1 -g -Wall -Wextra -Werror ${EXTRA_CFLAGS:-} \
        -Iapps -o "$TMP/t" "$T/test_modload.c" apps/ipodui/modload_core.c \
        2>"$TMP/cc.log"; then
    echo "FAIL: compiling test_modload ($HOSTCC)"; cat "$TMP/cc.log"; exit 1
fi
FL=$(python3 -c "
import sys; sys.path.insert(0, 'tools'); import modpak
print('%08x' % modpak.fletcher32(bytes((i * 7 + 3) & 255 for i in range(1001))))")
set -- "$FL"
B=build/fw-ui-mod  # a modules build (-DIPODUI_WITH_MODULES)
[ -f "$B/modules/modules.pak" ] || B=build/fw-ui
if [ -f "$B/modules/modules.pak" ] && command -v arm-none-eabi-objcopy >/dev/null; then
    ID=$(python3 - "$B" <<'PY'
import struct, subprocess, sys
b = sys.argv[1]
nm = subprocess.run(['arm-none-eabi-nm', b + '/rockbox.elf'], capture_output=True, text=True).stdout
a = [int(l.split()[0], 16) for l in nm.splitlines() if l.endswith(' mod_core_build_id')][0]
print('%08x' % struct.unpack('<I', open(b + '/rockbox.bin', 'rb').read()[a + 4:a + 8])[0])
PY
)
    set -- "$@" "$B/modules/modules.pak" "$ID"
    # every module, in the pack's own order (entry ids, named as
    # modload.c's mod_names[]): the test pairs entry i with ELF pair i
    ORDER=$(python3 - "$B/modules/modules.pak" apps/ipodui/modload.c <<'PY'
import re, struct, sys
pak = open(sys.argv[1], 'rb').read()
src = open(sys.argv[2]).read()
names = re.findall(r'"(\w+)"', src[src.index('mod_names[MOD__N]'):].split('};')[0])
count = struct.unpack_from('<H', pak, 6)[0]
print(' '.join(names[struct.unpack_from('<I', pak, 32 + 44 * i)[0]]
               for i in range(count)))
PY
)
    for m in $ORDER; do
        [ -f "$B/modules/$m.a.elf" ] || break
        arm-none-eabi-objcopy -O binary --only-section=.text "$B/modules/$m.a.elf" "$TMP/$m.a"
        arm-none-eabi-objcopy -O binary --only-section=.text "$B/modules/$m.b.elf" "$TMP/$m.b"
        set -- "$@" "$TMP/$m.a" "$TMP/$m.b"
    done
fi
if "$TMP/t" "$@"; then :; else echo "FAIL: module loader core"; exit 1; fi

if [ -f "$B/modules/modules.pak" ]; then
    # every firmware header modload.c includes becomes host/modhost.h
    mkdir -p "$TMP/inc"
    for h in config system kernel xcore file core_alloc storage lcd lang \
             settings rbpaths gcc_extensions logf debug; do
        echo '#include "modhost.h"' >"$TMP/inc/$h.h"
    done
    for v in -DHAVE_IPOD_UI_DEVMODE ""; do
        if ! "$HOSTCC" -std=gnu99 -O1 -g -Wall -Wextra -Werror \
                -Wno-pointer-sign -Wno-unused-parameter -Wno-format-truncation \
                ${EXTRA_CFLAGS:-} $v -I$T/host -I"$TMP/inc" -Iapps -o "$TMP/p" \
                "$T/test_modload_pak.c" apps/ipodui/modload.c \
                apps/ipodui/modload_core.c 2>"$TMP/cc.log"; then
            echo "FAIL: compiling test_modload_pak $v ($HOSTCC)"
            cat "$TMP/cc.log"; exit 1
        fi
        if [ -n "$v" ]; then printf '[developer] '; else printf '[release] '; fi
        "$TMP/p" "$B/modules/modules.pak" "$TMP" || {
            echo "FAIL: module load path $v"; exit 1; }
    done
else
    echo "skip module load path: no modules.pak (build fw-ui-mod)"
fi
