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

# scripts/simpool.sh -- sourced helpers for running several simulator
# scripts at once, each on its own cloned copy of a primed simdisk.
# Used by golden_capture.sh and risk/sim_ubsan.sh (the gate's own
# validate.sh keeps its inline equivalent). Source it from the repo root.
#
#   simpool_jobs                     default parallelism: min(16, 2*ncpu, mem GiB)
#   simpool_machine_lock             wait for, then hold, the machine-wide
#                                    gate lock (one gate or capture at a time)
#   simpool_clone <src> <dst>        cp -c clone on APFS, else cp -R
#   simpool_prime <sim_build> <tmp> <script...>
#                                    prime <sim_build>/simdisk ONCE, serially,
#                                    with whatever the scripts need (music,
#                                    library + built database, np fixtures)
#
# Why clones: a "fresh-db", "rebuild_db" or "reboot" script mutates its
# disk, so concurrent runs must never share one, and every run must start
# from the same primed state (the same rule as the gate's workers).

simpool_jobs() {
    _n=$({ sysctl -n hw.ncpu 2>/dev/null || nproc 2>/dev/null; } || echo 4)
    [ "$_n" -ge 1 ] 2>/dev/null || _n=4
    _j=$(( _n * 2 ))
    [ "$_j" -gt 16 ] && _j=16
    # Memory cap: about 1 GiB per simulator worker (sim + shots.sh +
    # ffmpeg/convert). 16 workers on an 8 GB Mac, twice over (two agents'
    # gates at once), froze the machine on 2026-10-09.
    _m=$({ sysctl -n hw.memsize 2>/dev/null || \
        awk '/MemTotal/ {print $2 * 1024}' /proc/meminfo 2>/dev/null; } || echo 0)
    _g=$(( _m / 1073741824 ))
    [ "$_g" -ge 1 ] && [ "$_j" -gt "$_g" ] && _j=$_g
    [ "$_j" -lt 1 ] && _j=1
    echo "$_j"
}

# Machine-wide lock: only one gate (validate.sh) or golden capture runs at
# a time across ALL worktrees, because each one already sizes its pool to
# the whole machine. mkdir is atomic; the pid file lets a lock left by a
# dead process be reclaimed. Re-entrant through ROCKBOX_GATE_LOCK_HELD so
# a nested call (gate-run -> validate.sh) does not wait on itself.
SIMPOOL_LOCK=${SIMPOOL_LOCK:-$HOME/.cache/rockbox-gate.lock}
simpool_machine_lock() {
    [ "${ROCKBOX_GATE_LOCK_HELD:-}" = 1 ] && return 0
    mkdir -p "$(dirname "$SIMPOOL_LOCK")"
    _said=0
    while :; do
        if mkdir "$SIMPOOL_LOCK" 2>/dev/null; then
            echo $$ >"$SIMPOOL_LOCK/pid"
            pwd >"$SIMPOOL_LOCK/where"
            date +%s >"$SIMPOOL_LOCK/start"
            break
        fi
        _lp=$(cat "$SIMPOOL_LOCK/pid" 2>/dev/null || true)
        if [ -n "${_lp:-}" ] && ! kill -0 "$_lp" 2>/dev/null; then
            echo "note: reclaiming a stale machine gate lock (pid $_lp is not running)" >&2
            rm -rf "$SIMPOOL_LOCK"
            continue
        fi
        _st=$(cat "$SIMPOOL_LOCK/start" 2>/dev/null || echo 0)
        _age=$(( ($(date +%s) - ${_st:-0}) / 60 ))
        if [ $_said = 0 ]; then
            echo "waiting for gate lock (pid ${_lp:-?} in $(cat "$SIMPOOL_LOCK/where" 2>/dev/null || echo '?'), held ${_age} min)" >&2
            _said=1
        fi
        sleep 5
    done
    ROCKBOX_GATE_LOCK_HELD=1; export ROCKBOX_GATE_LOCK_HELD
    SIMPOOL_LOCK_OWNER=$$
    # Watchdog (2026-10-09: a finished agent's gate held the lock for 2 h
    # while others queued). A gate or capture that runs past
    # GATE_MAX_MINUTES (default 75; the full gate outgrew 45) is aborted: its process tree gets TERM,
    # the trap below releases the lock, and the log says why.
    _gate_pid=$$
    # Double fork: the watchdog must NOT be a child of the gate shell, or
    # a plain `wait` in validate.sh waits for it too and the gate hangs
    # until the limit (2026-10-09). It polls once a minute and exits as
    # soon as the gate is gone.
    ( ( _n=0
        while [ $_n -lt ${GATE_MAX_MINUTES:-75} ]; do
            sleep 60
            kill -0 "$_gate_pid" 2>/dev/null || exit 0
            _n=$((_n + 1))
        done
        if [ "$(cat "$SIMPOOL_LOCK/pid" 2>/dev/null)" = "$_gate_pid" ]; then
            echo "FAIL: gate watchdog: still running after ${GATE_MAX_MINUTES:-75} min; aborting (pid $_gate_pid)" >&2
            simpool_kill_tree "$_gate_pid"
        fi ) >&2 & )
    SIMPOOL_WATCHDOG=
    trap 'simpool_machine_unlock' EXIT
    trap 'simpool_machine_unlock; exit 130' INT TERM
}
simpool_kill_tree() { # pid: TERM the pid and all its descendants
    for _c in $(pgrep -P "$1" 2>/dev/null); do simpool_kill_tree "$_c"; done
    kill -TERM "$1" 2>/dev/null
}
simpool_machine_unlock() {
    [ "${SIMPOOL_LOCK_OWNER:-}" = $$ ] || return 0
    [ -n "${SIMPOOL_WATCHDOG:-}" ] && simpool_kill_tree "$SIMPOOL_WATCHDOG"
    [ "$(cat "$SIMPOOL_LOCK/pid" 2>/dev/null)" = $$ ] && rm -rf "$SIMPOOL_LOCK"
    SIMPOOL_LOCK_OWNER=
}

simpool_clone() {
    rm -rf "$2"
    if [ "$(uname)" = Darwin ] && /bin/cp -c -R "$1" "$2" 2>/dev/null; then  # /bin/cp: a GNU cp earlier in PATH has no -c
        return 0
    fi
    rm -rf "$2"
    cp -R "$1" "$2"
}

simpool_prime() {
    _b=$1; _t=$2; shift 2
    _m=0; _l=0; _np=0; _ab=0; _v=0
    for _s in "$@"; do
        case "$(basename "$_s")" in ipodui-*) _m=1 ;; esac
        # Phase 14a: the video fixtures and a database that indexed them
        # (shots.sh does both for "# needs: video"), primed once like a
        # library, not rebuilt on every clone
        case "$(basename "$_s")" in ipodui-p14a-*) _v=1 ;; esac
        grep -qE '^#[[:space:]]*needs:[[:space:]]*video' "$_s" && _v=1
        grep -qE '^#[[:space:]]*needs:[[:space:]]*music' "$_s" && _m=1
        grep -qE '^#[[:space:]]*needs:[[:space:]]*library' "$_s" && _l=1
        grep -qE '^#[[:space:]]*needs:[[:space:]]*np' "$_s" && _np=1
        grep -qE '^#[[:space:]]*needs:[[:space:]]*ab' "$_s" && _ab=1
    done
    mkdir -p "$_t"
    # Start from a clean disk: whatever ad-hoc runs left on the shared one
    # (fixture dirs like NPArt in the database, a half-built art cache,
    # settings, resume state) must never reach a golden or a clone.
    if [ -f "$_b/Makefile" ]; then
        rm -rf "$_b/simdisk"
        make -C "$_b" install >"$_t/install.log" 2>&1 || {
            echo "FAIL: simpool: make install $_b (log: $_t/install.log)" >&2
            return 1
        }
    fi
    {
        [ $_m = 1 ] && echo '# needs: music'
        [ $_l = 1 ] && echo '# needs: library'
        [ $_np = 1 ] && echo '# needs: np'
        [ $_ab = 1 ] && echo '# needs: ab'
        [ $_v = 1 ] && echo '# needs: video'
        { [ $_l = 1 ] || [ $_v = 1 ]; } && printf 'wait_db 45000\nart_wait 60000\n'
        printf 'shot prime\nquit\n'
    } >"$_t/simpool-prime.script"
    # shots.sh installs .rockbox, runs the generators and (library) builds
    # the database in a priming boot, all on the base disk
    SHOTS_TIMEOUT=${SHOTS_TIMEOUT:-120} SIM_BUILD="$_b" \
        scripts/shots.sh "$_t/simpool-prime.script" "$_t/prime-out" \
        >"$_t/prime.log" 2>&1 || {
        echo "FAIL: simpool: priming $_b/simdisk failed (log: $_t/prime.log)" >&2
        tail -8 "$_t/prime.log" >&2
        return 1
    }
}
