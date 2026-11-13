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
# PP5022's incoherent caches (test_dspcop.c). A click started on the
# COP must show the tone (the negative control); a click routed to the CPU
# and an uncached DAC flag must be exact over 300 seeds.
# Compiler: the newest Homebrew gcc-NN, else cc; override with HOSTCC=...
set -u
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
cd "$ROOT"
T=tests/dspcop

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
if ! "$HOSTCC" $CF -o "$TMP/test_dspcop" "$T/test_dspcop.c" \
        2>"$TMP/cc.log"; then
    echo "FAIL: compiling test_dspcop ($HOSTCC)"; cat "$TMP/cc.log"; exit 1
fi
if "$TMP/test_dspcop" >"$TMP/out.log"; then
    echo "ok   DSP on the COP (the lock and the window hand-offs; each fence taken away is caught)"; sed "s/^/     /" "$TMP/out.log"
else
    cat "$TMP/out.log"
    echo "FAIL: DSP on the COP"; exit 1
fi
exit 0
