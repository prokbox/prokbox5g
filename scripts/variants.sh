#!/bin/bash
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

# Phase 9 variant runner (docs/phase9-brief.md section 3.5): the pairwise
# covering set of the iPod Video hardware variants. Every pair of flag values
# appears in at least one row.
#
#   scripts/variants.sh --list        print each row's configure line
#   scripts/variants.sh [options]     build the rows (orchestrator only)
#
# Options:
#   --rows LIST     comma list of row numbers (default: all 6)
#   --trim LIST     extra --ipodui-trim=LIST for every row (development:
#                   the trim set is the header's default otherwise)
#   --all           full make (core, codecs, plugins if any) instead of
#                   "make bin" (the core image only, what the size gate needs)
#   --keep          keep the build dirs (default: keep; --clean removes each
#                   row's dir after it is measured, to save disk)
#   --clean
#   -j N            make jobs (default 4)
#
# Rules (Rev 1, M7): never part of validate.sh and never under the gate
# lock. It takes its own lock (~/.cache/rockbox-p9-batch.lock, shared with
# scripts/trim-revert.sh), refuses to start while the machine gate lock
# (~/.cache/rockbox-gate.lock) is held, builds one dir at a time on the
# shared ccache, and stops itself after P9_BATCH_MAX_MINUTES (default 120),
# logging which rows ran.
#
# Output: one line per row in out/p9/variants.log and on stdout:
#   row N ok|FAIL|SKIP variant=<name> ipod=<bytes> end=<bytes> args=<...>
# then "variants PASS rows=N" or "variants FAIL ..." (exit 1). A row over
# the 914,813 B ceiling (docs/hardware.md) is a FAIL.
set -u
cd "$(dirname "$0")/.."
ROOT=$(pwd)

# row  storage ram battery dock   haptics   (row 1 = fw-ui, row 2 = fw-alt)
ROWS="1 flash 64 3000  usbc   on
2 hdd   32 3000  usbc   off
3 hdd   64 stock usbc   off
4 hdd   64 3000  dock30 on
5 flash 32 stock dock30 off
6 flash 32 stock dock30 on"

# docs/hardware.md budget; P9_CEILING=0 reports sizes without the check
# (e.g. while the trim set is still off, before the Part 1 integration)
CEILING=${P9_CEILING:-914813}

row_args() { # storage ram battery dock haptics -> configure args
    local a="--target=22 --type=n --compiler-prefix=arm-none-eabi- --ipodui"
    a="$a --storage=$1 --ram=$2 --battery=$3"
    [ "$4" = dock30 ] && a="$a --dock30"
    [ "$5" = off ] && a="$a --no-haptics"
    echo "$a"
}

if [ "${1:-}" = --list ]; then
    echo "$ROWS" | while read -r n storage ram battery dock haptics; do
        echo "row $n: $(row_args "$storage" "$ram" "$battery" "$dock" "$haptics")"
    done
    exit 0
fi

SEL=
TRIM=
TARGET=bin
CLEAN=0
JOBS=4
while [ $# -gt 0 ]; do
    case "$1" in
    --rows) SEL=",$2,"; shift 2 ;;
    --trim) TRIM=$2; shift 2 ;;
    --all) TARGET=; shift ;;
    --keep) CLEAN=0; shift ;;
    --clean) CLEAN=1; shift ;;
    -j) JOBS=$2; shift 2 ;;
    -j*) JOBS=${1#-j}; shift ;;
    -h|--help) awk 'NR==1{next} /^set -u/{print; exit} /^#\*+$/{b=!b; next} !b' "$0" | sed 's/^# \{0,1\}//'; exit 0 ;;
    *) echo "usage: $0 [--list] [--rows LIST] [--trim LIST] [--all] [--clean] [-j N]" >&2
       exit 2 ;;
    esac
done

# --- the P9 batch lock (shared with trim-revert.sh) and the gate check ---
BATCH_LOCK=${P9_BATCH_LOCK:-$HOME/.cache/rockbox-p9-batch.lock}
GATE_LOCK=${SIMPOOL_LOCK:-$HOME/.cache/rockbox-gate.lock}
MAX_MIN=${P9_BATCH_MAX_MINUTES:-120}
lock_live() { # dir -> 0 if held by a running pid
    local p
    [ -d "$1" ] || return 1
    p=$(cat "$1/pid" 2>/dev/null || true)
    [ -n "$p" ] && kill -0 "$p" 2>/dev/null
}
if lock_live "$GATE_LOCK"; then
    echo "variants: the gate lock is held (pid $(cat "$GATE_LOCK/pid"), $(cat "$GATE_LOCK/where" 2>/dev/null)); not starting (M7)" >&2
    exit 2
fi
mkdir -p "$(dirname "$BATCH_LOCK")"
if ! mkdir "$BATCH_LOCK" 2>/dev/null; then
    if lock_live "$BATCH_LOCK"; then
        echo "variants: another P9 batch holds $BATCH_LOCK (pid $(cat "$BATCH_LOCK/pid"), $(cat "$BATCH_LOCK/what" 2>/dev/null))" >&2
        exit 2
    fi
    rm -rf "$BATCH_LOCK"
    mkdir "$BATCH_LOCK" || exit 2
fi
echo $$ >"$BATCH_LOCK/pid"
echo "variants.sh $ROOT" >"$BATCH_LOCK/what"
trap '[ "$(cat "$BATCH_LOCK/pid" 2>/dev/null)" = $$ ] && rm -rf "$BATCH_LOCK"' EXIT
trap 'exit 130' INT TERM

START=$(date +%s)
elapsed_min() { echo $(( ($(date +%s) - START) / 60 )); }

kill_tree() { local c; for c in $(pgrep -P "$1" 2>/dev/null); do kill_tree "$c"; done; kill -TERM "$1" 2>/dev/null; }

run_budgeted() { # log cmd... : runs cmd, killed when the batch budget is spent
    local log=$1; shift
    "$@" >"$log" 2>&1 &
    local pid=$!
    while kill -0 "$pid" 2>/dev/null; do
        if [ "$(elapsed_min)" -ge "$MAX_MIN" ]; then
            echo "variants: P9_BATCH_MAX_MINUTES=$MAX_MIN spent; stopping" >&2
            kill_tree "$pid"
            wait "$pid" 2>/dev/null
            return 124
        fi
        sleep 5
    done
    wait "$pid"
}

mkdir -p out/p9
LOG=out/p9/variants.log
: >"$LOG"
say() { echo "$*"; echo "$*" >>"$LOG"; }
say "variants: $(git rev-parse --short HEAD) $(date -u +%FT%TZ) target=${TARGET:-all} trim=${TRIM:-header-default} budget=${MAX_MIN}min"

fail=
ran=0
while read -r n storage ram battery dock haptics; do
    case "$SEL" in ""|*,$n,*) ;; *) continue ;; esac
    args=$(row_args "$storage" "$ram" "$battery" "$dock" "$haptics")
    [ -n "$TRIM" ] && args="$args --ipodui-trim=$TRIM"
    if [ "$(elapsed_min)" -ge "$MAX_MIN" ]; then
        say "row $n SKIP budget spent args=$args"
        fail="$fail row$n:skipped"
        continue
    fi
    d=build/variant-$n
    rm -rf "$d"; mkdir -p "$d"
    # shellcheck disable=SC2086
    if ! (cd "$d" && ../../tools/configure $args </dev/null >configure.log 2>&1); then
        say "row $n FAIL configure args=$args"
        fail="$fail row$n:configure"
        continue
    fi
    variant=$(sed -n 's/^#define IPOD_VARIANT "\(.*\)"/\1/p' "$d/autoconf.h")
    if ! run_budgeted "$d/make.log" make -C "$d" -j"$JOBS" $TARGET; then
        say "row $n FAIL build variant=$variant args=$args"
        grep -nE "error" "$d/make.log" | head -5 | sed 's/^/    /' | tee -a "$LOG"
        fail="$fail row$n:build"
        continue
    fi
    ran=$((ran + 1))
    ipod=$(wc -c <"$d/rockbox.ipod" | tr -d ' ')
    end=$(printf '%d' "0x$(arm-none-eabi-nm "$d/rockbox.elf" | awk '$3 == "_end" {print $1}')")
    st=ok
    if [ "$CEILING" -gt 0 ] && [ "$ipod" -gt "$CEILING" ]; then
        st=FAIL; fail="$fail row$n:ceiling"
    fi
    say "row $n $st variant=$variant ipod=$ipod end=$end args=$args"
    [ "$CLEAN" = 1 ] && rm -rf "$d"
done <<EOF
$ROWS
EOF

say "elapsed $(elapsed_min) min"
if [ -n "$fail" ]; then
    say "variants FAIL$fail (ceiling $CEILING B)"
    exit 1
fi
say "variants PASS rows=$ran"
