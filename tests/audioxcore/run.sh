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

# Host test of the audio output path across the two cores (P19 ucop-fix):
# a model of the mixer's frame logic and the DAC's standby flag with the
# PP5022's incoherent caches (test_audioxcore.c). A click started on the
# COP must show the tone (the negative control); a click routed to the CPU
# and an uncached DAC flag must be exact over 300 seeds.
# Compiler: the newest Homebrew gcc-NN, else cc; override with HOSTCC=...
set -u
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
cd "$ROOT"
T=tests/audioxcore

if [ -z "${HOSTCC:-}" ]; then
    HOSTCC=cc
    for v in 19 18 17 16 15 14; do
        if command -v gcc-$v >/dev/null 2>&1; then HOSTCC=gcc-$v; break; fi
    done
fi

TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT
CF="-std=gnu99 -O1 -g -Wall -Wextra -Werror ${EXTRA_CFLAGS:-}"
# shellcheck disable=SC2086
if ! "$HOSTCC" $CF -o "$TMP/test_audioxcore" "$T/test_audioxcore.c" \
        2>"$TMP/cc.log"; then
    echo "FAIL: compiling test_audioxcore ($HOSTCC)"; cat "$TMP/cc.log"; exit 1
fi
if "$TMP/test_audioxcore" >"$TMP/out.log"; then
    echo "ok   audio path across cores (clicks on the CPU, DAC flag uncached, the EQ set and the prescaler on the CPU; the as-built faults caught)"
else
    cat "$TMP/out.log"
    echo "FAIL: audio path across cores"; exit 1
fi
exit 0
