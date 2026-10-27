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

# Host test of Phase 18's pure sleep cores (wheel packet, wake
# classifier, poll deadline, tier, deep time, boot equivalent, swallow mask);
# no Rockbox calls.
# Compiler: the newest Homebrew gcc-NN, else cc; override with HOSTCC=...
set -u
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
cd "$ROOT"
T=tests/sleep

if [ -z "${HOSTCC:-}" ]; then
    HOSTCC=cc
    for v in 19 18 17 16 15 14; do
        if command -v gcc-$v >/dev/null 2>&1; then HOSTCC=gcc-$v; break; fi
    done
fi

TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT
# shellcheck disable=SC2086
if ! "$HOSTCC" -std=gnu99 -O1 -g -Wall -Wextra -Werror ${EXTRA_CFLAGS:-} \
        -Iapps -iquote firmware/export -o "$TMP/test_sleep" \
        "$T/test_sleep.c" firmware/common/sleep_wake_core.c apps/ipodui/sleep_core.c 2>"$TMP/cc.log"; then
    echo "FAIL: compiling test_sleep ($HOSTCC)"
    cat "$TMP/cc.log"
    exit 1
fi
if "$TMP/test_sleep"; then
    echo "ok   soft sleep policy (sleep_wake_core.c, sleep_core.c)"
else
    echo "FAIL: soft sleep policy"
    exit 1
fi
# 18-1: the raw PMU path against a scripted bus, and the RTC conversion
# shellcheck disable=SC2086
if ! "$HOSTCC" -std=gnu99 -O1 -g -Wall -Wextra -Werror ${EXTRA_CFLAGS:-} \
        -iquote "$T" -iquote firmware/export -o "$TMP/test_pmu_raw" \
        "$T/test_pmu_raw.c" firmware/common/sleep_wake_core.c 2>"$TMP/cc2.log"; then
    echo "FAIL: compiling test_pmu_raw ($HOSTCC)"
    cat "$TMP/cc2.log"
    exit 1
fi
if "$TMP/test_pmu_raw"; then
    echo "ok   raw PMU path (pmu_raw-pp502x.c, sleep_rtc_from_bcd)"
else
    echo "FAIL: raw PMU path"
    exit 1
fi
