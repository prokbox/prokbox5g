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

# scripts/gatelib.sh -- sourced by validate.sh (and tests/gate/run.sh):
# the gate's bookkeeping (docs/gate-speed-brief.md).
#
#   gl_header                  print the modes in force (first lines of the log)
#   gl_phase <name>            close the running phase, open <name>
#   gl_phase_end               close the running phase, print the table
#   gl_abort_on_build_failure  exit 1 after a build failure unless
#                              GATE_KEEP_GOING=1
#   gl_script_row ...          one per-script timing row
#   gl_pool_init/submit/wait   the dynamic job pool (GATE_POOL=1), bash 3.2
#
# Needs from the caller: GATE_TMP (scratch dir), and for the abort test
# bfail / nbad / nbad_builds.

GL_PHASE_NAME=""
GL_PHASE_T0=0

gl_header() {
    local mode=full
    [ "${QUICK:-0}" = 1 ] && mode=quick
    [ "${QUICK:-0}" = 1 ] && [ "${CHANGED:-0}" = 1 ] && mode="quick --changed"
    echo "gate: mode=$mode pool=${GATE_POOL:-0} keep-going=${GATE_KEEP_GOING:-0} max-minutes=${GATE_MAX_MINUTES:-?} id=${GATE_ID:-?} commit=${GATE_COMMIT:-?}"
    local n=""
    [ "${GATE_UBSAN:-1}" = 0 ] && n="$n GATE_UBSAN=0"
    [ "${FLOOR_GATE:-1}" = 0 ] && n="$n FLOOR_GATE=0"
    [ "${GATE_KEEP_GOING:-0}" = 1 ] && n="$n GATE_KEEP_GOING=1"
    [ -n "${GATE_ONLY:-}" ] && n="$n GATE_ONLY=$GATE_ONLY"
    [ -n "$n" ] && echo "gate: reduced/changed behaviour in force:$n"
    return 0
}

gl_phase() {
    gl_phase_close
    GL_PHASE_NAME=$1
    GL_PHASE_T0=$(date +%s)
}

gl_phase_close() {
    if [ -n "$GL_PHASE_NAME" ]; then
        local t1; t1=$(date +%s)
        printf '%s\t%s\t%s\t%s\n' "$GL_PHASE_NAME" "$GL_PHASE_T0" "$t1" $((t1 - GL_PHASE_T0)) \
            >>"${GATE_TMP:-/tmp}/phases.tsv"
        GL_PHASE_NAME=""
    fi
}

# Print the table and keep the file under build/gate-timing/ (the gate's
# scratch dir is wiped at the next start).
gl_phase_end() {
    gl_phase_close
    [ -f "${GATE_TMP:-/tmp}/phases.tsv" ] || return 0
    mkdir -p build/gate-timing
    cp "${GATE_TMP:-/tmp}/phases.tsv" build/gate-timing/phases.tsv
    python3 scripts/gate_tools.py phases build/gate-timing/phases.tsv
}

gl_abort_on_build_failure() {
    if { [ "${bfail:-0}" = 1 ] || [ "${nbad:-0}" -gt "${nbad_builds:-0}" ]; } \
            && [ "${GATE_KEEP_GOING:-0}" != 1 ]; then
        echo
        echo "FAIL: a build failed; aborting before fonts, tests and simulator scripts"
        echo "      (they cannot turn this into GATE PASSED; GATE_KEEP_GOING=1 runs on)"
        gl_phase_end
        echo "GATE FAILED"
        exit 1
    fi
    return 0
}

# gl_script_row name worker start sim trace golden total attempt kind
gl_script_row() {
    printf '%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\n' "$@" >>"${GATE_TMP:-/tmp}/scripts-rows.tsv"
}

# The dynamic job pool: a token FIFO, so at most $1 jobs run at once and a
# freed slot starts the next job at once (no static worker partition).
# bash 3.2 compatible (no wait -n). Jobs are shell functions or commands.
gl_pool_init() {
    GL_POOL_FIFO=${GATE_TMP:-/tmp}/pool.fifo.$$
    rm -f "$GL_POOL_FIFO"
    mkfifo "$GL_POOL_FIFO"
    exec 9<>"$GL_POOL_FIFO"
    rm -f "$GL_POOL_FIFO"
    local i=0
    while [ $i -lt "$1" ]; do echo x >&9; i=$((i + 1)); done
}
gl_pool_submit() {
    local tok
    read -r -u 9 tok
    ( "$@"; echo x >&9 ) &
}
gl_pool_wait() { wait; exec 9>&-; }

# ---- flake ledger (scripts/gate_tools.py ledger-*) -----------------------
GL_LEDGER_REASON=""
GL_LEDGER_KIND=flaky
# gl_ledger_first <script> <first-attempt-out-file>
gl_ledger_first() {
    GL_LEDGER_REASON=$(grep -m1 -E '^FAIL' "$2" 2>/dev/null | cut -c1-200)
    [ -n "$GL_LEDGER_REASON" ] || GL_LEDGER_REASON="(no FAIL line)"
    GL_LEDGER_KIND=flaky
    case "$GL_LEDGER_REASON" in *starved*) GL_LEDGER_KIND=starved ;; esac
}
# gl_ledger_result <script> <retry-status>
gl_ledger_result() {
    local kind=$GL_LEDGER_KIND retry=ok
    if [ "$2" != 0 ]; then kind=real; retry=fail; fi
    python3 scripts/gate_tools.py ledger-add --gate-id "${GATE_ID:-?}" \
        --commit "${GATE_COMMIT:-?}" --script "$1" --kind "$kind" \
        --retry "$retry" --reason "$GL_LEDGER_REASON" 2>/dev/null || true
}

# End of the gate: phase table, per-script timing, ledger, learned durations.
gl_finish() {
    gl_phase_end
    if [ -s "${GATE_TMP:-/tmp}/scripts-rows.tsv" ]; then
        mkdir -p build/gate-timing
        cp "$GATE_TMP/scripts-rows.tsv" build/gate-timing/scripts.tsv
        python3 scripts/gate_tools.py slowest build/gate-timing/scripts.tsv 8
        python3 scripts/gate_tools.py update build/gate-timing/scripts.tsv || true
    fi
    python3 scripts/gate_tools.py ledger-report --gate-id "${GATE_ID:-?}" || true
}
