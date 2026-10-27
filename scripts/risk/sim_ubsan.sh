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

# Risk tool 2: alignment/UB sanitizer pass (docs/risk-tooling.md).
#
#   scripts/risk/sim_ubsan.sh
#
# 1. Configures build/sim-ui-ubsan (--target=22 --type=s --ipodui
#    --with-ubsan, the existing configure option: GCCOPTS gets
#    -fsanitize=undefined -fPIC and build forces SDL threads -- this
#    script also appends -fsanitize=alignment -fsanitize-recover=all to
#    the generated (gitignored) Makefile, since -fsanitize=undefined's
#    default set already includes alignment checks but the brief asks
#    for it explicitly, and -fsanitize-recover=all so one run reports
#    every finding instead of aborting at the first). If it builds, runs
#    every tests/ui/ipodui-*.script against it and scans sim.log for
#    "runtime error:" lines.
# 2. Builds and runs the host tests (tests/ipodui/*, same sources as
#    tests/ipodui/run.sh -- a separate, parallel compile here rather
#    than editing run.sh, which scripts/test.sh also uses with no
#    sanitizer and must stay unaffected) with
#    -fsanitize=alignment,undefined -fsanitize-recover=all.
# 3. Reports every finding (file:line + message) for the orchestrator.
#    This never edits apps/, so it never fixes anything it finds.
#
# UBSAN_VARIANT=mod (P12 close): the same pass on build/sim-ui-mod-ubsan,
# the module-loader sim (-DIPODUI_WITH_MODULES appended to EXTRA_DEFINES,
# as validate.sh's sim-ui-mod), over the "# needs: modules" scripts (the
# default variant leaves them out: it has no loader); logs and shots under
# build/risk/ubsan-mod and out/shots-ubsan-mod; no host tests (step 2 runs
# in the default variant only).
#
# Strict findings: a misaligned access ("misaligned" in the runtime error)
# anywhere in apps/ipodui/, the iPod target code, firmware/common or bcmfw
# (default UBSAN_STRICT_RE; the sdk/games/modload sites were the first) is a
# STRICT: line instead of a FINDING: line; the sim itself aborts there
# (-fno-sanitize-recover=alignment), and a host test that hits one is STRICT too. The PP5022's ARM7TDMI aborts a
# misaligned halfword or word access where the host only goes slower (the
# P12 games crash: the SDK's state record at an odd address, quirks.md
# #51), so there the sanitizer stands in for the device. UBSAN_STRICT_RE
# overrides the path pattern.
#
# Exit status: 0 (the rest is a report, not a gate); 3 from the report
# (no argument, or --report) when there is a STRICT finding; 1 from a
# staged mode below whose own stage could not run.
#
# Staged modes (the gate's shared job pool, GATE_POOL=1, docs/gate-speed-brief.md):
#   sim_ubsan.sh --build     configure + build build/sim-ui-ubsan only
#   sim_ubsan.sh --prep      prime the shared disk once (UBSAN_LIST / UBSAN_SCRIPTS)
#   sim_ubsan.sh --job NAME  run one script on its own clone (what a pool slot runs)
#   sim_ubsan.sh --report    scan the logs + the sanitized host tests; prints FINDING: lines
# With no argument it does all of them in turn, exactly as before.
set -u
MODE=all; JOB=""
case "${1:-}" in
    "") ;;
    --build) MODE=build ;;
    --prep) MODE=prep ;;
    --job) MODE=job; JOB=${2:?usage: sim_ubsan.sh --job <script-name>} ;;
    --report) MODE=report ;;
    *) echo "usage: sim_ubsan.sh [--build|--prep|--job NAME|--report]" >&2; exit 2 ;;
esac
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
cd "$ROOT"
UBSAN_VARIANT=${UBSAN_VARIANT:-}
MOD_DEFINE=-DIPODUI_WITH_MODULES
case "$UBSAN_VARIANT" in
    "") B=build/sim-ui-ubsan; UB=build/risk/ubsan; SHOTS=out/shots-ubsan ;;
    mod) B=build/sim-ui-mod-ubsan; UB=build/risk/ubsan-mod; SHOTS=out/shots-ubsan-mod ;;
    *) echo "sim_ubsan: UBSAN_VARIANT must be empty or mod" >&2; exit 2 ;;
esac
STRICT_RE=${UBSAN_STRICT_RE:-(apps/ipodui/|firmware/target/arm/ipod/|firmware/common/|bcmfw/|lib/rbcodec/metadata/chapters)}
strict=0
SANFLAGS="-fsanitize=alignment,undefined -fsanitize-recover=all -fno-sanitize-recover=alignment"
mkdir -p "$UB" "$SHOTS"
findings=0
report() { printf '\nFINDING: %s\n' "$*"; findings=$((findings + 1)); }
step() { printf '\n== %s\n' "$*"; }

SIM_OK=0
if [ $MODE = all ] || [ $MODE = build ]; then
step "1. $B (--with-ubsan, --ipodui${UBSAN_VARIANT:+, $MOD_DEFINE})"
# the mod variant: a dir without the define (configured by hand) is wiped
if [ "$UBSAN_VARIANT" = mod ] && [ -f "$B/Makefile" ] \
        && ! grep -q "^export EXTRA_DEFINES=.* $MOD_DEFINE" "$B/Makefile"; then
    rm -rf "$B"
fi
if [ ! -f "$B/Makefile" ]; then
    mkdir -p "$B"
    (cd "$B" && ../../tools/configure --target=22 --type=s --ipodui \
        --with-ubsan </dev/null > configure.log 2>&1)
    if [ "$UBSAN_VARIANT" = mod ]; then
        sed -i.bak "/^export EXTRA_DEFINES=/s/\$/ $MOD_DEFINE/" "$B/Makefile" 2>/dev/null
    fi
fi
# alignment never recovers: the first misaligned access aborts the sim (the
# device data-aborts there, quirks.md #51) and its "runtime error: ...
# misaligned" line is a STRICT finding; every other check still recovers
if ! grep -q 'fno-sanitize-recover=alignment' "$B/Makefile" 2>/dev/null; then
    sed -i.bak 's/^export GCCOPTS=.*/& -fsanitize=alignment -fsanitize-recover=all -fno-sanitize-recover=alignment/' \
        "$B/Makefile" 2>/dev/null
    make -C "$B" clean >/dev/null 2>&1   # flags changed: rebuild every object
fi
if make -C "$B" -j"${UBSAN_MAKE_J:-10}" > "$B/make.log" 2>&1; then
    echo "ok   $B built"
    SIM_OK=1
else
    report "$B does not build: -fsanitize=undefined -fPIC (the project's
  existing --with-ubsan option, confirmed independent of this tool's own
  -fsanitize=alignment addition -- removing that addition reproduces the
  exact same error) causes apps/ipodui/library.c:184's
      extern uint64_t clock_gettime_nsec_np(int clock_id);
  to conflict with this SDK's declaration
      __uint64_t clock_gettime_nsec_np(clockid_t __clock_id);
  (firmware/export/.../usr/include/_time.h:185, gated '#if
  !defined(_POSIX_C_SOURCE) || defined(_DARWIN_C_SOURCE)'). A plain
  (non-ubsan) --ipodui sim-ui build, with the SAME -D_THREAD_SAFE,
  compiles this file cleanly -- only adding -fsanitize=undefined -fPIC
  reproduces it (gcc 16.2.0 / macOS 26 SDK, 2026-10-06). Tried and
  rejected: defining -D_POSIX_C_SOURCE=200809L globally silences this
  declaration but then breaks apps/gui/skin_engine.c:391 (implicit
  strncasecmp -- a different Darwin extension hidden by the same
  feature-test macro). No fix is possible without editing apps/
  (out of scope here): either apps/ipodui/library.c:184 needs an
  #if/#elif on a feature-test macro or SDK version, or the extern needs
  removing in favor of including <time.h> directly and resolving
  whatever it originally clashed with (the comment at library.c:178
  says SDL.h and <time.h> clash with the apps headers, but does not
  say which symbol). BLOCKS the rest of this step: the ipodui-* script
  set cannot run against a UBSan build until apps/ipodui/library.c is
  fixed. See $B/make.log."
fi
else
    [ -x "$B/rockboxui" ] && SIM_OK=1
fi
[ $MODE = build ] && { [ $SIM_OK = 1 ] && exit 0; exit 1; }
if [ $SIM_OK != 1 ] && { [ $MODE = prep ] || [ $MODE = job ]; }; then
    echo "sim_ubsan: no $B/rockboxui (run --build first)" >&2; exit 1
fi

if [ $SIM_OK = 1 ]; then
    # Which scripts: UBSAN_LIST (space-separated script names, set by
    # validate.sh --quick --changed from gate_map.txt) or else UBSAN_SCRIPTS,
    # a glob under tests/ui (default ipodui-*), e.g. 'ipodui-nav-*'.
    # UBSAN_JOBS parallel workers (default min(4, ncpu/2)), each run on its
    # own cp -c clone of the ubsan sim's disk, primed once -- the gate's own
    # rule (scripts/simpool.sh). Findings are collected per script and
    # reported in script-name order, so the report is stable.
    . scripts/simpool.sh
    if [ -n "${UBSAN_LIST:-}" ]; then
        ub_scripts=""
        for n in $UBSAN_LIST; do ub_scripts="$ub_scripts tests/ui/$n.script"; done
    else
        pat=${UBSAN_SCRIPTS:-ipodui-*}
        # (the default sim has no module loader: "# needs: modules"
        # scripts run on the mod variant only, and only those there)
        if [ "$UBSAN_VARIANT" = mod ]; then
            ub_scripts=$(grep -lE '^#[[:space:]]*needs:[[:space:]]*modules' tests/ui/$pat.script 2>/dev/null)
        else
            ub_scripts=$(grep -LE '^#[[:space:]]*needs:[[:space:]]*modules' tests/ui/$pat.script 2>/dev/null)
        fi
    fi
    UJ=${UBSAN_JOBS:-$(simpool_jobs)}
    ub_job() { # <script path>: one run on its own clone of the primed disk
        s=$1; n=$(basename "$s" .script); out=$SHOTS/$n
        simpool_clone "$B/simdisk" "$UB/pool/$n.disk"
        if SIM_BUILD=$B SIMDISK="$UB/pool/$n.disk" scripts/shots.sh "$s" "$out" > "$out.log" 2>&1
        then echo ok > "$UB/pool/$n.status"; else echo fail > "$UB/pool/$n.status"; fi
        rm -rf "$UB/pool/$n.disk"
    }
    if [ $MODE = job ]; then
        mkdir -p "$SHOTS" "$UB/pool"
        ub_job "tests/ui/$JOB.script"
        exit 0
    fi
    if [ $MODE = all ] || [ $MODE = prep ]; then
    step "1b. $(echo $ub_scripts | wc -w | tr -d ' ') script(s) against $B ($UJ workers)"
    rm -rf "$SHOTS" "$UB/pool"; mkdir -p "$SHOTS" "$UB/pool"
    if simpool_prime "$B" "$UB/pool/prime" $ub_scripts; then
        if [ $MODE = all ]; then
        for s in $ub_scripts; do printf '%s\n' "$s"; done |
            xargs -P "$UJ" -I{} sh -c '
                s=$1; B=$2; UB=$3; SHOTS=$4
                n=$(basename "$s" .script); out=$SHOTS/$n
                . scripts/simpool.sh
                simpool_clone "$B/simdisk" "$UB/pool/$n.disk"
                if SIM_BUILD=$B SIMDISK="$UB/pool/$n.disk" scripts/shots.sh "$s" "$out" > "$out.log" 2>&1
                then echo ok > "$UB/pool/$n.status"; else echo fail > "$UB/pool/$n.status"; fi
                rm -rf "$UB/pool/$n.disk"' _ {} "$B" "$UB" "$SHOTS"
        fi
    else
        report "$B: priming the shared simdisk failed; ubsan scripts not run (see $UB/pool/prime/prime.log)"
        [ $MODE = prep ] && exit 1
    fi
    [ $MODE = prep ] && exit 0
    fi
    if [ $MODE = all ] || [ $MODE = report ]; then
    nran_ub=0
    for s in $ub_scripts; do
        n=$(basename "$s" .script)
        out=$SHOTS/$n
        if [ "$(cat "$UB/pool/$n.status" 2>/dev/null)" = ok ]; then
            echo "ran  $n"; nran_ub=$((nran_ub + 1))
        elif [ -f "$UB/pool/$n.status" ]; then
            echo "ran  $n (script run reported a failure -- see $out.log)"; nran_ub=$((nran_ub + 1))
        else
            echo "note $n: no ubsan result (did not run)"
        fi
        log=$out/sim.log
        if [ -f "$log" ] && grep -q 'runtime error:' "$log"; then
            # (no pipe into the loop: report() must count in this shell)
            # one line per site: the addresses differ between hits
            grep 'runtime error:' "$log" | sed -E 's/0x[0-9a-fA-F]+/0x../g' \
                | sort -u > "$UB/sim-findings.txt"
            while read -r line; do
                if printf '%s\n' "$line" | grep -q misaligned \
                        && printf '%s\n' "$line" | grep -qE "$STRICT_RE"; then
                    printf '\nSTRICT: %s: %s\n' "$n" "$line"
                    strict=$((strict + 1))
                else
                    report "$n: $line"
                fi
            done < "$UB/sim-findings.txt"
        fi
    done
    echo "ubsan scripts ran $nran_ub of $(echo $ub_scripts | wc -w | tr -d ' ')"
    fi
fi
[ $MODE = all ] || [ $MODE = report ] || exit 0
if [ "$UBSAN_VARIANT" = mod ]; then
    echo
    echo "$findings finding(s), $strict strict (misaligned access in $STRICT_RE)."
    [ $strict -gt 0 ] && exit 3
    exit 0
fi

step "2. host tests (tests/ipodui/*) with $SANFLAGS"
build_and_run() { # name, extra-includes, extra-defines, sources...
    # (RUNARGS: arguments for the test binary, run from $UB)
    name=$1; inc=$2; def=$3; shift 3
    # shellcheck disable=SC2086
    if cc -std=gnu99 -O1 -g $SANFLAGS $inc $def -o "$UB/$name" "$@" \
            2>"$UB/$name.cc.log"; then
        # shellcheck disable=SC2086
        out=$(cd "$UB" && ./"$name" ${RUNARGS:-} 2>&1); rc=$?
        if [ $rc -ne 0 ] || echo "$out" | grep -qi 'runtime error\|AddressSanitizer\|UndefinedBehaviorSanitizer'; then
            if echo "$out" | grep -q misaligned; then
                strict=$((strict + 1))
                printf '\nSTRICT: host test %s (exit %s):\n%s\n' "$name" "$rc" "$out"
            else
            report "host test $name (exit $rc):
$out"
            fi
        else
            echo "ok   $name: clean (exit $rc)$( [ -n "$out" ] && printf '\n%s' "$out" )"
        fi
    else
        report "host test $name: compile failed, see $UB/$name.cc.log"
    fi
    RUNARGS=
}
build_and_run test_scroll "-Iapps -Iapps/ipodui" '-DSCROLL_H="ipodui/scroll.h"' \
    tests/ipodui/test_scroll.c apps/ipodui/scroll.c
build_and_run test_bands "-Itests/ipodui/stub -Iapps" "" \
    tests/ipodui/test_bands.c apps/ipodui/screen_stack.c
build_and_run test_sortkey "-Itests/ipodui/stub -Iapps -Iapps/ipodui" \
    '-DSORTKEY_H="ipodui/sortkey.h"' \
    tests/ipodui/test_sortkey.c apps/ipodui/sortkey.c
build_and_run test_sortkey_version "-Itests/ipodui/stub -Iapps -Iapps/ipodui" \
    '-DSORTKEY_H="ipodui/sortkey.h"' \
    tests/ipodui/test_sortkey_version.c apps/ipodui/sortkey.c
# same sources and arguments as tests/ipodui/run.sh
rm -rf "$UB/db"; mkdir -p "$UB/db"
RUNARGS="db $ROOT/tests/artcache/keys.txt"
build_and_run test_library \
    "-Itests/ipodui/libstub -Itests/ipodui -Iapps -Iapps/ipodui" "-DARTFMT_HOST" \
    "$ROOT/tests/ipodui/test_library.c" "$ROOT/tests/ipodui/fake_tagcache.c" \
    apps/ipodui/library.c apps/ipodui/sortkey.c apps/ipodui/artkey.c \
    apps/ipodui/artfmt.c

step "2b. tests/artcache (same sources and arguments as tests/artcache/run.sh)"
RUNARGS="$ROOT/tests/artcache/keys.txt"
build_and_run test_artkey "-Iapps/ipodui" "" \
    tests/artcache/test_artkey.c apps/ipodui/artkey.c apps/ipodui/sortkey.c
rm -rf "$UB/cache"; mkdir -p "$UB/cache"
RUNARGS="cache"
build_and_run test_artfmt "-Iapps/ipodui" "-DARTFMT_HOST" \
    tests/artcache/test_artfmt.c apps/ipodui/artfmt.c apps/ipodui/artkey.c \
    apps/ipodui/sortkey.c

echo
echo "$findings finding(s), $strict strict (misaligned access in $STRICT_RE)."
[ $strict -gt 0 ] && exit 3
exit 0
