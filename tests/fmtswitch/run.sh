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

# Host test of the seamless output-format switch (#214, b16): the real
# apps/pcmbuf.c on tests/fencepcm's stubs (test_fmtswitch.c). Then each
# mutant (one b16 fix taken back) must turn it red.
# Compiler: the newest Homebrew gcc-NN, else cc; override with HOSTCC=...
set -u
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
cd "$ROOT"
T=tests/fmtswitch

if [ -z "${HOSTCC:-}" ]; then
    HOSTCC=cc
    for v in 19 18 17 16 15 14; do
        if command -v gcc-$v >/dev/null 2>&1; then HOSTCC=gcc-$v; break; fi
    done
fi

TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT
mkdir -p "$TMP/src/ipodui"
cp apps/pcmbuf.h apps/codec_place_core.h firmware/export/pcm_xcore.h "$TMP/src/"
cp apps/ipodui/fadecurve.h "$TMP/src/ipodui/"

# mutant name -> python replace (old, new) on pcmbuf.c; each must apply
mutate() {
    python3 - "$1" "$TMP/src/pcmbuf_fmt.c" <<'EOF'
import sys
m, path = sys.argv[1], sys.argv[2]
s = open(path).read()
subs = {
    # b16 1: the codec's DSP at the output's rate (the b15 bug)
    "dsp_rate_is_output": ("twice as fast) */\n    return FMT_RATE(pcmbuf_wfmt);",
                           "twice as fast) */\n    return mixer_get_frequency();"),
    # b16 2: a flush leaves the request flag set
    "flag_kept_by_flush": ("    fmt_switch_posted = false;\n#endif\n}",
                           "#endif\n}"),
}
old, new = subs[m]
if s.count(old) != 1:
    sys.exit("mutant %s: %d matches" % (m, s.count(old)))
open(path, "w").write(s.replace(old, new))
EOF
}

build() {
    cp apps/pcmbuf.c "$TMP/src/pcmbuf_fmt.c"
    if [ -n "${2:-}" ]; then mutate "$2" || return 2; fi
    # shellcheck disable=SC2086
    "$HOSTCC" -std=gnu99 -O1 -g -Wall -Wextra -Werror -Wno-unused-function \
        -Wno-unused-variable -Wno-unused-but-set-variable -Wno-type-limits \
        -DHAVE_PLAY_FREQ ${EXTRA_CFLAGS:-} -I"$TMP/src" -Itests/fencepcm/stub \
        -o "$TMP/$1" $T/test_fmtswitch.c apps/codec_place_core.c \
        2>"$TMP/cc.log" || { cat "$TMP/cc.log"; return 2; }
}

rc=0
if ! build base; then
    echo "FAIL: compiling test_fmtswitch ($HOSTCC)"
    exit 1
fi
if "$TMP/base" >"$TMP/out" 2>&1 && grep -q '^VERDICT: PASS$' "$TMP/out"; then
    grep -v '^VERDICT' "$TMP/out"
    echo "ok   output-format switch (real pcmbuf.c: the next track's DSP rate, the switch, a flush between, a dropped request)"
else
    cat "$TMP/out"
    echo "FAIL: output-format switch"
    exit 1
fi

n=0; red=0
for m in dsp_rate_is_output flag_kept_by_flush; do
    n=$((n + 1))
    if ! build "mut_$m" "$m"; then
        echo "FAIL: mutant $m does not apply or build"; rc=1; continue
    fi
    "$TMP/mut_$m" >"$TMP/mout" 2>&1
    if grep -q '^VERDICT: FAIL' "$TMP/mout"; then
        red=$((red + 1))
        echo "     red  $m: $(grep '^VERDICT: FAIL' "$TMP/mout" | sed 's/^VERDICT: //')"
    elif grep -q '^VERDICT: PASS$' "$TMP/mout"; then
        echo "FAIL: mutant $m survived (the test cannot see it)"; rc=1
    else
        echo "FAIL: mutant $m ended without a verdict line"
        tail -5 "$TMP/mout"; rc=1
    fi
done
[ "$red" -eq "$n" ] && [ $rc -eq 0 ] \
    && echo "ok   output-format switch mutants ($n of $n red)"
exit $rc
