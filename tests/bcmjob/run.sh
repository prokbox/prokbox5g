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

# Host test of the BCM2722 job driver (bcmjob-core.c) against a chip made of the reference executor (P19 W0-6)
# Compiler: the newest Homebrew gcc-NN, else cc; override with HOSTCC=...
# Extra flags (e.g. sanitizers): EXTRA_CFLAGS="-fsanitize=address,undefined"
set -u
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
cd "$ROOT"
T=tests/bcmjob

if [ -z "${HOSTCC:-}" ]; then
    HOSTCC=cc
    for v in 19 18 17 16 15 14; do
        if command -v gcc-$v >/dev/null 2>&1; then HOSTCC=gcc-$v; break; fi
    done
fi

TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT
# shellcheck disable=SC2086
# (HAVE_BCMFW_DEVTEST: the developer hooks too, e.g. bjc_inject_fail).
# The chip offers DECODE (jref.h JREF_DECODE, as bcmfw/tests/jobs_test.py
# builds jmtest) and runs jdec over the input ring as the display-only
# image does: jdec, jdec_ring.c and the shell's decoder and resizer
# (apps/recorder, bcmfw/jpeg/env), built first without -Werror (upstream
# code) and without UBSan's shift-base check (its IDCT's idiom)
JD="apps/recorder/jpeg_load.c apps/recorder/resize.c apps/recorder/bmp.c bcmfw/jobs/jdec.c bcmfw/jobs/jdec_ring.c"
JDO=
for f in $JD; do
    o="$TMP/$(basename "$f" .c).o"
    # shellcheck disable=SC2086
    if ! "$HOSTCC" -std=gnu99 -O1 -g ${EXTRA_CFLAGS:-} $(echo "${EXTRA_CFLAGS:-}" | grep -q undefined && echo -fno-sanitize=shift-base) \
            -Ibcmfw/jpeg/env -Iapps/recorder -Ibcmfw/jobs -Ibcmfw -Ibcmfw/jpeg -c "$f" -o "$o" 2>"$TMP/cc.log"; then
        echo "FAIL: compiling $f ($HOSTCC)"
        cat "$TMP/cc.log"
        exit 1
    fi
    JDO="$JDO $o"
done
# shellcheck disable=SC2086
if ! "$HOSTCC" -std=gnu99 -O1 -g -Wall -Wextra -Werror ${EXTRA_CFLAGS:-} \
        -DHAVE_BCMFW_DEVTEST -DJREF_DECODE -DJREF_EXT -DBJC_TEST_FENCE -Ifirmware/target/arm/ipod/video -Ibcmfw/jobs -Ibcmfw -o "$TMP/test_bcmjob" \
        "$T/test_bcmjob.c" firmware/target/arm/ipod/video/bcmjob-core.c \
        firmware/target/arm/ipod/video/bcmjob-viz.c -Ifirmware/export \
        bcmfw/jobs/jref.c bcmfw/jobs/jref_viz.c bcmfw/jobs/jviz.c $JDO -lpthread 2>"$TMP/cc.log"; then
    echo "FAIL: compiling test_bcmjob ($HOSTCC)"
    cat "$TMP/cc.log"
    exit 1
fi
if "$TMP/test_bcmjob" >"$TMP/out.log"; then
    echo "ok   BCM2722 job driver: $(tail -1 "$TMP/out.log") (bcmjob-core.c)"
else
    echo "FAIL: BCM2722 job driver"
    cat "$TMP/out.log"
    exit 1
fi
# contract 4's own host tests (brief section 9): the executor (jtest) and
# the host model the simulator wraps (jmtest), built as bcmfw/tests/
# jobs_test.py builds them (cc: jtest.c trips gcc's -Wparentheses)
J=bcmfw/jobs
for t in "jtest $J/jtest.c $J/jref.c $J/jscene.c" "jmtest -DJREF_DECODE $J/jmtest.c $J/jmodel.c $J/jref.c"; do
    # shellcheck disable=SC2086
    set -- $t
    name=$1; shift
    if ! cc -O2 -std=gnu99 -Wall -Wextra -Werror -Wno-unused-parameter \
            -Ibcmfw -I$J -o "$TMP/$name" "$@" 2>"$TMP/cc.log"; then
        echo "FAIL: compiling $name (cc)"
        cat "$TMP/cc.log"
        exit 1
    fi
    "$TMP/$name" >"$TMP/$name.log"
    if tail -1 "$TMP/$name.log" | grep -q ' 0 failed$'; then
        echo "ok   $(tail -1 "$TMP/$name.log")"
    else
        echo "FAIL: $name"
        grep FAIL "$TMP/$name.log" | head -20
        exit 1
    fi
done
