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

# Host test of #283 (b): firmware/pcm_mixer.c in layout A on stubs. The
# mixer and its headers are copied beside the stubs, so the quoted
# includes find the stubs before the firmware's own headers.
# Compiler: the newest Homebrew gcc-NN, else cc; override with HOSTCC=...
# MIXER=path tests another pcm_mixer.c, BEEP=path another beep.c (red runs).
set -u
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
cd "$ROOT"
T=tests/mixerfirst

if [ -z "${HOSTCC:-}" ]; then
    HOSTCC=cc
    for v in 19 18 17 16 15 14; do
        if command -v gcc-$v >/dev/null 2>&1; then HOSTCC=gcc-$v; break; fi
    done
fi

TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT
mkdir -p "$TMP/asm"
cp "$T"/stub/*.h "$TMP/" || exit 1
cp "${MIXER:-firmware/pcm_mixer.c}" "$TMP/pcm_mixer.c" || exit 1
cp firmware/export/pcm_mixer.h firmware/export/pcm_xcore.h "$TMP/" || exit 1
cp firmware/asm/pcm-mixer.c firmware/asm/beep.c "$TMP/asm/" || exit 1
cp "${BEEP:-apps/beep.c}" "$TMP/beep.c" || exit 1
cp "$T/test_mixerfirst.c" "$TMP/" || exit 1
# shellcheck disable=SC2086
if ! "$HOSTCC" -std=gnu99 -O1 -g -Wall -Wextra -Werror \
        -Wno-unused-function -I"$TMP" ${EXTRA_CFLAGS:-} \
        -o "$TMP/t" "$TMP/test_mixerfirst.c" 2>"$TMP/cc.log"; then
    echo "FAIL: compiling test_mixerfirst ($HOSTCC)"
    cat "$TMP/cc.log"
    exit 1
fi
if "$TMP/t"; then
    echo "ok   mixer first frame (#283: a channel started with no buffer is fetched by the mix on the COP, never on the CPU; a stopped PCM starts on two silent frames and the COP mixes from the third; beep.c makes no frame on the CPU)"
else
    echo "FAIL: mixer first frame"
    exit 1
fi
