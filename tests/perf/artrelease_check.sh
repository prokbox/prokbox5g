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

# The art pass gives way to a newer library before PLAY (p090-artrelease):
#
#   tests/perf/artrelease_check.sh [32|64 ...]      (default: 32 64)
#
# Runs tests/perf/artrelease.script on a clone of each unit's stress disk
# (32 MB: 50k, 64 MB: 65k; tests/perf/stress_size.sh), with the device's
# pool (IPODUI_SIM_DEVICE_POOL / _PINNED, which tests/perf/floor_gate.sh
# exports; run standalone, export them first: ARTREL_DEVICE=0 runs with
# the simulator's own pool, a diagnostic arm that never passes). In the
# log, from the update's publication to the PLAY's start ("audio start
# ms=", after its buffer was sized):
#   - an art pass was under way on the old generation when it published
#     ("art build start" before it, no "art build done" between): else
#     the run proves nothing and fails;
#   - "art pass yield gen old=N new=M" came after the publication and
#     before the start, and "db retired ram free" too;
#   - the buffer after that PLAY is at least the buffer of a PLAY before
#     the update, less ARTREL_SLACK B (default 262,144).
# ARTREL_BUILD: another simulator build dir (a red run on an older
# binary). Exit 0 = every unit held.
set -u
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
. "$ROOT/tests/perf/stress_size.sh"
RC=0
SL=${ARTREL_SLACK:-262144}
BUILD=${ARTREL_BUILD:-$ROOT/build/sim-ui}
if [ "${ARTREL_DEVICE:-1}" = 0 ]; then
    echo "artrelease: NOTE ARTREL_DEVICE=0: the simulator's own pool (diagnostic; this run fails)"
    unset IPODUI_SIM_DEVICE_POOL IPODUI_SIM_DEVICE_PINNED
elif [ -z "${IPODUI_SIM_DEVICE_POOL:-}" ] || [ -z "${IPODUI_SIM_DEVICE_PINNED:-}" ]; then
    echo "artrelease: FAIL no device pool (export IPODUI_SIM_DEVICE_POOL and IPODUI_SIM_DEVICE_PINNED as tests/perf/floor_gate.sh does)" >&2
    exit 2
fi
[ -x "$BUILD/rockboxui" ] || { echo "artrelease: FAIL no $BUILD/rockboxui" >&2; exit 2; }
for RAM in ${*:-32 64}; do
    SOURCE=${FLOOR_SOURCE:-$(stress_disk "$RAM")} \
        || { echo "artrelease: RAM must be 32 or 64" >&2; exit 2; }
    "$ROOT/tests/perf/ensure65k.sh" "$SOURCE" || exit 1
    SET=$("$ROOT/tests/perf/settle65k.sh" "$SOURCE") || exit 1
    export IPODUI_SIM_RAM=$RAM
    RUN=$(mktemp -d "$ROOT/build/artrelease-$RAM.XXXXXX")
    [ "$SET" != "$SOURCE" ] && mv "$(dirname "$SET")" "$RUN/settled"
    [ "$SET" != "$SOURCE" ] && SET=$RUN/settled/simdisk
    mkdir -p "$RUN/simdisk"
    cp -cpR "$SET/." "$RUN/simdisk/" 2>/dev/null || cp -pR "$SET/." "$RUN/simdisk/"
    OUT=$ROOT/out/shots/artrelease-$RAM
    SIM_BUILD=$BUILD SIMDISK=$RUN/simdisk SHOTS_TIMEOUT=${SHOTS_TIMEOUT:-900} \
        "$ROOT/scripts/shots.sh" "$ROOT/tests/perf/artrelease.script" "$OUT" \
        >"$RUN/shots.out" 2>&1
    ST=$?
    L=$OUT/sim.log
    V=$(awk -v slack="$SL" '
        /^ipodui: art build start / { started = NR; done_since = 0 }
        /^ipodui: art build done / { done_since = NR }
        /^ipodui: db gen [0-9]+ n=/ { pub = NR; pub_started = started;
                                      pub_done = done_since; y = 0; r = 0 }
        /^ipodui: art pass yield gen / && pub && !y { y = NR }
        /^ipodui: db retired ram free / && pub && !r { r = NR }
        /^sim_script: update_db$/ { upd = NR }
        /^sim_script: expect_db_grew ok/ { grew = NR }
        /^ipodui: audio start ms=/ && grew && !s { s = NR; sy = y; sr = r
                                                   gp = pub; gs = pub_started
                                                   gd = pub_done }
        /^sim_script: floor base_play / { for (i = 1; i <= NF; i++)
                                          if ($i ~ /^audio=/) base = substr($i, 7) }
        /^sim_script: floor artrelease_play / { for (i = 1; i <= NF; i++)
                                          if ($i ~ /^audio=/) after = substr($i, 7) }
        END {
            why = ""
            if (!grew) why = "no expect_db_grew ok"
            else if (!s) why = "no audio start after the update"
            else if (!gp || gp < upd) why = "no publication between the update and the PLAY"
            else if (!gs || gd > gs) why = "no art pass under way at the publication (start " gs ", done " gd ")"
            else if (!sy || sy > s) why = "no art pass yield gen between the publication and the PLAY"
            else if (!sr || sr > s) why = "no retired ram free between the publication and the PLAY"
            else if (base == "" || after == "") why = "no floor lines"
            else if (after + slack < base) why = "the buffer after the update " after " B is " base - after " B under the one before it " base " B"
            printf "%s|base=%s after=%s publish=%d yield=%d free=%d start=%d\n",
                   why, base, after, gp, sy, sr, s
        }' "$L" 2>/dev/null)
    WHY=${V%%|*}
    grep -q "^sim: ram $RAM MB" "$L" 2>/dev/null || WHY=${WHY:-not a $RAM MB run}
    if [ -n "${IPODUI_SIM_DEVICE_POOL:-}" ]; then
        D_=$(sed -n 's/^sim: device pool \([0-9]*\) B.*pinned \([0-9]*\) B$/\1 \2/p' "$L" 2>/dev/null | sort -u | tr '\n' ' ')
        [ "$D_" = "$IPODUI_SIM_DEVICE_POOL $IPODUI_SIM_DEVICE_PINNED " ] \
            || WHY=${WHY:-device pool '${D_:-none}', not '$IPODUI_SIM_DEVICE_POOL $IPODUI_SIM_DEVICE_PINNED'}
    fi
    [ "${ARTREL_DEVICE:-1}" = 0 ] && WHY=${WHY:-diagnostic arm (ARTREL_DEVICE=0)}
    grep "^ipodui: art pass yield gen\|^sim_script: floor \(base_play\|artrelease_play\)\|play release" "$L" 2>/dev/null \
        | sed 's/^sim_script: //; s/^ipodui: //; s/^/  /'
    if [ $ST -eq 0 ] && [ -z "$WHY" ]; then
        echo "artrelease: ok ${RAM} MB (${V#*|})"
        rm -rf "$RUN"
    else
        echo "artrelease: FAIL ${RAM} MB: exit=$ST ${WHY} (${V#*|}; $L, $RUN/shots.out)" >&2
        RC=1
    fi
done
exit $RC
