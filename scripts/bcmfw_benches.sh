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
# bcmfw benches (#60): our VideoCore II image and its tools, run against
# vc2sim (docs/research/bcm2722/bringup.md, "Gate wiring").  Called by
# scripts/validate.sh; also usable by hand.
#
#   vc2cc_tests      the compiler's run tests
#   hello_test       CPU-push image: boot, frames, rects, sleep/wake
#   dma_test         DMA image
#   hostproto_test   video host protocol skeleton
#   realframes_test  UI goldens through the CPU and DMA push
#   devtest_test     the device test's ARM-side stages (0-4, 4b) and the
#                    boot path
#   bench_test       the timing bench image (stage 4's BCMFW_BENCH=1)
#   forms_test       every embedded image: only instruction kinds Apple's
#                    code has or the device verified (no stray (--rs))
#   xfade_test       the drift kernels (stage 5b's mode B) against the
#                    shell's cross-fade and half steps
#   silicon_test     the silicon words of tests/silicon/ read as modelled;
#                    vc2sim's sitting-2 behaviours and strict traps
#   proven_forms     scripts/proven_forms.py: every form of every shipped
#                    image proven, guarded, exempt or trapped (E2)
#   halfload_test, jobs, jobs_image, lab, peek, r3d, vpu_fit, hibwake, disp,
#   jscan, jskip, jpeg_oob, jidct, decode_image, jpeg, video, viz: every
#   other bcmfw/tests/*_test.py (E3, hardware-closure-plan.md: none may
#   go unrun); the JPEG and video corpora are built when missing, a
#   "corpus missing" FAILS, and viz needs numpy (missing FAILS)
#   vc2sim --selftest, vc2dis --roundtrip   (Apple's 1.3 corpus only)
#
# The long benches (over 5 minutes: jpeg, video, devtest; the table and
# their inputs are in scripts/bench_cache.py) are skipped when the hash of
# everything they read matches a PASS recorded in the shared cache
# (~/.cache/prokbox-bench-cache/<name>/<hash>, every worktree's) ("ok   bcmfw
# <name> (cached <hash12>)").  GATE_FORCE=1
# runs them anyway.  A failure never records.  BCMFW_ONLY="<names>" runs
# just those bcmfw/tests/<name>_test.py (no corpora, vc2cc or Apple refs;
# BCMFW_TESTS points elsewhere): for tests/bench_cache and by hand.
#
# Each bench skips (exit 0, "skip:" line) without vc4-elf-gcc; this script
# turns that into a "skip" note, so machines without the VideoCore
# toolchain or the reference corpus pass with a printed reason.  The
# benches run in parallel (single-threaded Python, about 90 s wall: the
# device test's stage 4 rows are the longest).
# Exit 1 if any bench fails.
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

cd "$(dirname "$0")/.." || exit 1
# vc2sim traps any access whose silicon behaviour is unknown (memory-access.md
# section 11): the default, made explicit here
export VC2SIM_STRICT_ALIGN="${VC2SIM_STRICT_ALIGN:-trap}"
CORPUS=${BCM_CORPUS:-$HOME/Git/re/bcm2722/extract/1.3/vc/VideoCore}
TMP=$(mktemp -d "${TMPDIR:-/tmp}/bcmfw-benches.XXXXXX") || exit 1
trap 'rm -rf "$TMP"' EXIT

# at most BCM_JOBS benches at once (the decode and video benches are long
# and single-threaded; an unbounded fan-out overloads the machine)
BCM_JOBS=${BCM_JOBS:-4}
TESTS=${BCMFW_TESTS:-bcmfw/tests}
# Each bench gets BENCH_TIMEOUT seconds (default 40 min, room for the long
# benches on a loaded machine; the heartbeat below shows them): a bench that
# never ends fails as a TIMEOUT instead of holding the gate at its wait
# (a --quick gate hung over 30 min, 2026-10-26, with no trace of which
# bench).  perl's alarm: macOS has no timeout(1).  A failed or timed-out
# bench's log is kept in build/bench-logs/ (TMP goes at exit).
BENCH_TIMEOUT=${BENCH_TIMEOUT:-2400}
LOGS=build/bench-logs
run() { # name, command...
    n=$1; shift
    while [ "$(jobs -rp | wc -l)" -ge "$BCM_JOBS" ]; do
        sleep 1
    done
    date +%s >"$TMP/$n.start"
    # twice the bench's expected time (bench_cache.py's table), at least
    # BENCH_TIMEOUT
    lim=$(python3 -I scripts/bench_cache.py limit "$n" "$BENCH_TIMEOUT" 2>/dev/null) \
        || lim=$BENCH_TIMEOUT
    ( start=$(date +%s)
      perl -e 'alarm shift; exec @ARGV or exit 127' "$lim" "$@" \
          >"$TMP/$n.log" 2>&1
      echo "$? $(( $(date +%s) - start ))" >"$TMP/$n.rc" ) 2>/dev/null &
}
# Wait for every bench, saying every 2 min which still run and for how long
wait_all() {
    local last=$(date +%s) now f n left
    while [ -n "$(jobs -rp)" ]; do
        sleep 2
        now=$(date +%s)
        [ $((now - last)) -ge 120 ] || continue
        last=$now
        left=""
        for f in "$TMP"/*.start; do
            [ -f "$f" ] || continue
            n=$(basename "$f" .start)
            [ -f "$TMP/$n.rc" ] && continue
            left="$left $n ($((now - $(cat "$f"))) s)"
        done
        [ -n "$left" ] && echo "note bcmfw: still running:$left"
    done
    wait
}

# The reference corpora the decode and video benches read: built here when
# missing (both scripts are ours; media stays in build/, never committed).
# A bench that then reports "corpus missing" FAILS: no silent skips (E3,
# docs/research/hardware-closure-plan.md)
need_corpus=""
[ -n "${BCMFW_ONLY:-}" ] || for c in jpeg video; do
    if [ ! -d "build/$c/corpus" ] || [ -z "$(ls -A "build/$c/corpus" 2>/dev/null)" ]; then
        if ! sh "tests/$c/make_corpus.sh" >"$TMP/corpus-$c.log" 2>&1; then
            need_corpus="$need_corpus $c"
        fi
    fi
done
# jpeg_test's progressive cases (tests/jpeg/make_prog.sh, from the corpus's
# sources into build/jpeg-prog)
if [ -z "${BCMFW_ONLY:-}" ] && { [ ! -d build/jpeg-prog ] || [ -z "$(ls -A build/jpeg-prog 2>/dev/null)" ]; }; then
    if ! sh tests/jpeg/make_prog.sh >"$TMP/corpus-jpeg-prog.log" 2>&1; then
        need_corpus="$need_corpus jpeg-prog"
    fi
fi
# viz_test needs numpy, and jobs_image and decode_image import it for their
# BARS rows (a per-user install: python3 -I hides it, so these three run
# without -I; they read only our own files).  Missing numpy FAILS.
NUMPY_BENCHES="viz jobs_image decode_image"
numpy_ok=1
python3 -c 'import numpy' 2>/dev/null || numpy_ok=0

BENCHES="hello dma hostproto realframes devtest bench forms xfade halfload silicon \
    jobs jobs_image lab peek r3d vpu_fit hibwake disp \
    jscan jskip jpeg_oob jidct decode_image jpeg video viz vcheck_image"
# jpeg_test (the decoder against the reference on the whole corpus, about
# 23 min) runs on every full gate; validate.sh --quick (BCM_QUICK=1) runs it
# only with GATE_LONG=1, and says so
if [ "${BCM_QUICK:-0}" = 1 ] && [ "${GATE_LONG:-0}" != 1 ]; then
    keep=""
    for t in $BENCHES; do
        [ "$t" = jpeg ] || keep="$keep $t"
    done
    BENCHES=$keep
    echo "note bcmfw jpeg: not in a --quick gate (GATE_LONG=1 runs it; every full gate does)"
fi
[ -n "${BCMFW_ONLY:-}" ] && BENCHES=$BCMFW_ONLY
# The long benches' cache (scripts/bench_cache.py): a bench whose inputs
# hash as at its last PASS is reported and not run; its hash is taken
# before the run and recorded only on a PASS
CACHE_DIR=${BENCH_CACHE_DIR:-$HOME/.cache/prokbox-bench-cache}
export BENCH_CACHE_DIR="$CACHE_DIR"
cached=""
for t in $BENCHES; do
    [ -f "$TESTS/${t}_test.py" ] || continue
    h=$(python3 -I scripts/bench_cache.py hash "$t" 2>/dev/null) || continue
    echo "$h" >"$TMP/$t.hash"
    if [ "${GATE_FORCE:-0}" != 1 ] && [ -f "$CACHE_DIR/$t/$h" ]; then
        cached="$cached $t"
    fi
done
is_cached() { echo " $cached " | grep -q " $1 "; }

if [ -z "${BCMFW_ONLY:-}" ]; then
    run vc2cc python3 -I tools/bcmre/vc2cc_tests.py
    run proven python3 -I scripts/proven_forms.py
fi
for t in $BENCHES; do
    [ -f "$TESTS/${t}_test.py" ] || continue
    is_cached "$t" && continue
    if echo " $NUMPY_BENCHES " | grep -q " $t "; then
        [ "$numpy_ok" = 1 ] && run "$t" python3 "$TESTS/${t}_test.py"
    else
        run "$t" python3 -I "$TESTS/${t}_test.py"
    fi
done
if [ -n "${BCMFW_ONLY:-}" ]; then
    refs=""
elif [ -f "$CORPUS/Boot/vmcs.bin" ] && [ -d "$CORPUS/Library" ]; then
    run vc2sim python3 tools/bcmre/vc2sim.py --selftest "$CORPUS"
    run vc2dis python3 tools/bcmre/vc2dis.py --roundtrip "$CORPUS/Library/" \
        "$CORPUS/Boot/vmcs.bin" 0 0x200
    refs="vc2sim vc2dis"
else
    echo "skip vc2sim --selftest, vc2dis --roundtrip: Apple's 1.3 corpus not found ($CORPUS; set BCM_CORPUS)"
    refs=""
fi
wait_all

fail=0
for c in $need_corpus; do
    echo "FAIL bcmfw corpus $c: tests/$c/make_corpus.sh failed:"
    tail -5 "$TMP/corpus-$c.log"
    fail=1
done
needs_numpy=0
for t in $NUMPY_BENCHES; do
    echo " $BENCHES " | grep -q " $t " && needs_numpy=1
done
if [ "$numpy_ok" = 0 ] && [ "$needs_numpy" = 1 ]; then
    echo "FAIL bcmfw $NUMPY_BENCHES: numpy is missing (pip3 install --user numpy)"
    fail=1
fi
present=""
for t in $BENCHES; do
    [ -f "$TESTS/${t}_test.py" ] && present="$present $t"
done
pre="vc2cc proven"
[ -n "${BCMFW_ONLY:-}" ] && pre=""
for n in $pre $present $refs; do
    if is_cached "$n"; then
        echo "ok   bcmfw $n (cached $(cut -c1-12 "$TMP/$n.hash"))"
        continue
    fi
    echo " $NUMPY_BENCHES " | grep -q " $n " && [ "$numpy_ok" = 0 ] && continue
    set -- $(cat "$TMP/$n.rc" 2>/dev/null || echo "1 0")
    rc=$1 secs=$2
    if grep -qiE 'corpus missing|cases missing' "$TMP/$n.log"; then
        echo "FAIL bcmfw $n: its corpus is missing (no silent skip)"
        [ -f "$TMP/$n.hash" ] && python3 -I scripts/bench_cache.py drop "$n" "$(cat "$TMP/$n.hash")"
        grep -iE 'corpus missing|cases missing' "$TMP/$n.log" | head -2
        fail=1
        continue
    fi
    skip=$(grep -m1 -E '^skip:|: SKIP ' "$TMP/$n.log")
    if [ "$rc" = 0 ] && [ -n "$skip" ]; then
        echo "skip bcmfw $n: $(echo "$skip" | sed -E 's/^skip: //; s/^.*: SKIP //')"
        continue
    fi
    grep '^note:' "$TMP/$n.log" | sed "s/^note:/note bcmfw $n:/"
    case "$n" in
        vc2sim) sum=$(grep -E '^[0-9]+ rows,' "$TMP/$n.log" | tail -1) ;;
        vc2dis) sum=$(grep -E 'reassemble' "$TMP/$n.log" | tail -1) ;;
        forms)  sum=$(grep -E '^[0-9]+ failed' "$TMP/$n.log" | tail -1) ;;
        proven) sum=$(grep -E 'unproven, untrapped' "$TMP/$n.log" | tail -1) ;;
        *)      sum=$(grep -E '[0-9]+ rows, [0-9]+ cases' "$TMP/$n.log" | tail -1 \
                      | sed -E 's/; PNGs in .*//') ;;
    esac
    if [ "$rc" = 0 ]; then
        echo "ok   bcmfw $n: ${sum:-done} (${secs} s)"
        # a PASS (not a skip, not a missing corpus) records its inputs' hash
        [ -f "$TMP/$n.hash" ] && python3 -I scripts/bench_cache.py record "$n" "$(cat "$TMP/$n.hash")"
    else
        if [ "$rc" = 142 ]; then
            echo "FAIL bcmfw $n: TIMEOUT after ${secs} s (its limit: twice its expected time, at least BENCH_TIMEOUT=$BENCH_TIMEOUT): ${sum:-no summary}"
        else
            echo "FAIL bcmfw $n (exit $rc, ${secs} s): ${sum:-no summary}"
        fi
        mkdir -p "$LOGS" && cp "$TMP/$n.log" "$LOGS/$n.log" \
            && echo "    log kept: $LOGS/$n.log"
        tail -3 "$TMP/$n.log" | sed 's/^/    | /'
        # a failure is never cached, and drops a PASS at the same inputs
        [ -f "$TMP/$n.hash" ] && python3 -I scripts/bench_cache.py drop "$n" "$(cat "$TMP/$n.hash")"
        grep -E 'FAIL|Error|Traceback|UNPROVEN' "$TMP/$n.log" | head -8 | sed 's/^/    /'
        fail=1
    fi
done
exit $fail
