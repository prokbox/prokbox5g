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

# Host test of #196: the codec's id3 written by both cores (apps/codec_id3.c)
# in a line-granular model of the CPU's cache. Built twice: with the fix it
# must keep every write; with -DCODEC_ID3_OLD (0.8.0-rc.4's placement: a
# plain member reached through the cache) the same checks must fail, so the
# model is shown able to catch the hazard on every run.
# Compiler: the newest Homebrew gcc-NN, else cc; override with HOSTCC=...
set -u
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
cd "$ROOT"
T=tests/codecid3

if [ -z "${HOSTCC:-}" ]; then
    HOSTCC=cc
    for v in 19 18 17 16 15 14; do
        if command -v gcc-$v >/dev/null 2>&1; then HOSTCC=gcc-$v; break; fi
    done
fi

TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT
CF="-std=gnu99 -O1 -g -Wall -Wextra -Werror -I$T/stub -Iapps"
# shellcheck disable=SC2086
if ! "$HOSTCC" $CF ${EXTRA_CFLAGS:-} -o "$TMP/new" "$T/test_codecid3.c" \
        apps/codec_id3.c 2>"$TMP/cc.log" \
   || ! "$HOSTCC" $CF -DCODEC_ID3_OLD -o "$TMP/old" "$T/test_codecid3.c" \
        2>>"$TMP/cc.log"; then
    echo "FAIL: compiling test_codecid3 ($HOSTCC)"
    cat "$TMP/cc.log"
    exit 1
fi
if ! "$TMP/new"; then
    echo "FAIL: codec id3 across cores (codec_id3.c)"
    exit 1
fi
if "$TMP/old" >"$TMP/old.log" 2>&1; then
    echo "FAIL: codec id3 across cores: the model no longer catches 0.8.0-rc.4's placement"
    cat "$TMP/old.log"
    exit 1
fi
echo "     the rc.4 placement, as a check of the model: $(grep -o 'COP write lost [0-9]*' "$TMP/old.log")"
echo "ok   codec id3 across cores (codec_id3.c: own lines, uncached for both, the CPU's stale lines dropped; no write of either core lost over 2000 runs)"

# #197: the codec slot's hand-off between the cores (copj_cache_ops() at
# every phase), the one object both cores reach cached
# shellcheck disable=SC2086
if ! "$HOSTCC" -std=gnu99 -O1 -g -Wall -Wextra -Werror -Ifirmware/export \
        ${EXTRA_CFLAGS:-} -o "$TMP/handoff" "$T/test_handoff.c" \
        firmware/common/cop_job_core.c 2>"$TMP/cc2.log"; then
    echo "FAIL: compiling test_handoff ($HOSTCC)"
    cat "$TMP/cc2.log"
    exit 1
fi
if ! "$TMP/handoff"; then
    echo "FAIL: codec slot hand-off (copj_cache_ops)"
    exit 1
fi
# every fence must matter: with any one phase's operations dropped
# (post, start, finish, collect, call, return) the model loses data
for ph in 0 1 2 3 4 5; do
    if "$TMP/handoff" "$ph" >"$TMP/drop$ph.log" 2>&1; then
        echo "FAIL: codec slot hand-off: dropping phase $ph loses nothing (the model can't see it)"
        cat "$TMP/drop$ph.log"
        exit 1
    fi
    echo "     phase $ph dropped:$(sed -n 's/.*dropped)://p' "$TMP/drop$ph.log")"
done
echo "ok   codec slot hand-off (copj_cache_ops at post/start/finish/collect/call/return: 2000 runs clean; each of the six fences dropped loses data)"
