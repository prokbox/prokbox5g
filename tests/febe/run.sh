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

# Host tests of the UI front end / back end boundary (P19 S1):
# apps/ipodui/fe_be_core.c, which has no Rockbox calls.
#  - test_febe: the slot states, the cache-operation table, generations,
#    and the seqlock under two real threads (torn reads never accepted);
#  - cachemodel: two cores with incoherent write-back caches over one
#    SDRAM (design section 7.2): every protocol clean over 300 seeds, and
#    each protocol with one cache operation left out caught.
# Compiler: the newest Homebrew gcc-NN, else cc; override with HOSTCC=...
set -u
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
cd "$ROOT"
T=tests/febe

if [ -z "${HOSTCC:-}" ]; then
    HOSTCC=cc
    for v in 19 18 17 16 15 14; do
        if command -v gcc-$v >/dev/null 2>&1; then HOSTCC=gcc-$v; break; fi
    done
fi

TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT
CF="-std=gnu99 -O1 -g -Wall -Wextra -Werror -pthread ${EXTRA_CFLAGS:-} -Iapps/ipodui -Ifirmware/export"
# shellcheck disable=SC2086
if ! "$HOSTCC" $CF -DFEBE_HOST_SMP -o "$TMP/test_febe" "$T/test_febe.c" \
        apps/ipodui/fe_be_core.c firmware/common/cop_job_core.c 2>"$TMP/cc.log"; then
    echo "FAIL: compiling test_febe ($HOSTCC)"; cat "$TMP/cc.log"; exit 1
fi
# shellcheck disable=SC2086
if ! "$HOSTCC" $CF -DFEBE_MEM_MODEL -o "$TMP/cachemodel" "$T/cachemodel.c" \
        apps/ipodui/fe_be_core.c firmware/common/cop_job_core.c 2>"$TMP/cc2.log"; then
    echo "FAIL: compiling cachemodel ($HOSTCC)"; cat "$TMP/cc2.log"; exit 1
fi
rc=0
if "$TMP/test_febe"; then
    echo "ok   UI boundary (fe_be_core.c: slots, cache table, generations, seqlock)"
else
    echo "FAIL: UI boundary (fe_be_core.c)"; rc=1
fi
if "$TMP/cachemodel"; then
    echo "ok   UI boundary cache model (two incoherent cores: clean, omissions caught)"
else
    echo "FAIL: UI boundary cache model"; rc=1
fi
if python3 "$T/test_policy.py"; then
    echo "ok   UI boundary audio-out rule (mixer, DSP, sound.c writers cross; the tap on the CPU)"
else
    echo "FAIL: UI boundary audio-out rule"; rc=1
fi
exit $rc
