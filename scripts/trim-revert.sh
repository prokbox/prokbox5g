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

# Phase 9 trim reversibility (docs/phase9-brief.md sections 4.1 and 7.6).
#
# Each IPODUI_TRIM_* line in firmware/export/config/ipodvideo-variant.h must
# be removable together with its dependency closure (scripts/trim-deps.txt)
# and still build fw-ui. This script owns that check.
#
#   scripts/trim-revert.sh --list    print each trim's closure and the
#                                    distinct closure builds (no build)
#   scripts/trim-revert.sh --check   preprocess the variant header for every
#                                    closure (must pass) and for every edge
#                                    left dangling (must hit its #error);
#                                    host cpp only, a few seconds, no build
#   scripts/trim-revert.sh [opts]    the closure builds (orchestrator only;
#                                    outside the gate lock, under the P9
#                                    batch lock and P9_BATCH_MAX_MINUTES,
#                                    section 3.5, Rev 1 M7)
#
# Build mode options:
#   --only LIST   comma list of trim names: build only the closures that
#                 revert them (default: every distinct closure)
#   --no-errors   skip exercising the #error checks through configure
#   --keep        keep each closure's build dir (default: removed after a
#                 pass, kept after a failure for diagnosis)
#   -j N          make jobs (default 4)
#   --dry         print each closure's configure line (and, in header
#                 mode, the lines it would #undef); no lock, no build
#
# How a closure is reverted. While the header still guards the set with
# IPODUI_TRIM_DEFAULTS (the contract state), every closure build passes
# configure --ipodui-trim=<all trims outside the closure>, which is the
# same set the header would give with the closure's lines deleted; no
# source changes. Once the guard is gone (Part 1 integration), the
# closure's "#define IPODUI_TRIM_X" lines become "#undef IPODUI_TRIM_X" in
# place (same effect as deleting them, comments kept intact) and the
# header is restored from build/trim-revert/variant.h.orig on exit; the
# script refuses to start if the header has uncommitted changes.
#
# Each closure build is a full make (core, codecs, and the plugins when
# PLUGINS is in the closure), so the derived switches are exercised too:
# UTF8PROC back must build lib/libutf8proc.a (configure NO_UTF8PROC
# empty), PLUGINS back must build the plugins (mpegplayer.rock: the
# Videos path, criterion 5). Then every edge's #error is exercised once:
# configure with that edge left dangling must fail naming the need.
#
# Output: out/p9/trim-revert.log and stdout, one line per closure:
#   closure <NAMES> ok|FAIL ipod=<b> end=<b> derived=<...> [undefined=...]
#   error <NAME> -> <NEED> ok|FAIL <message>
# then "trim-revert PASS builds=<n> errors=<n>" or "trim-revert FAIL ..."
# (exit 1). A failing build lists its first undefined references and the
# objects that want them: each is a missing edge for trim-deps.txt and
# the header.
set -eu
cd "$(dirname "$0")/.."

DEPS=scripts/trim-deps.txt
HDR=firmware/export/config/ipodvideo-variant.h
mode=build
case "${1:-}" in --list|--check) mode=$1 ;; esac

closures() { # prints "NAME: closure..." for every trim, then "BUILD: set" lines
    python3 -I - "$DEPS" <<'EOF'
import sys
deps = {}
for line in open(sys.argv[1]):
    line = line.split('#', 1)[0].strip()
    if not line:
        continue
    name, _, needs = line.partition(':')
    deps[name.strip()] = needs.split()
for n, ns in deps.items():
    for m in ns:
        if m not in deps:
            sys.exit("trim-deps.txt: %s needs unknown trim %s" % (n, m))
def closure(n):
    seen, todo = set(), [n]
    while todo:
        x = todo.pop()
        if x not in seen:
            seen.add(x)
            todo += deps[x]
    return seen
builds = []
for n in deps:
    c = closure(n)
    print("%s: %s" % (n, " ".join(sorted(c))))
    if c not in builds:
        builds.append(c)
for c in builds:
    print("BUILD: %s" % " ".join(sorted(c)))
print("ALL: %s" % " ".join(deps))
EOF
}

cpp_hdr() { # extra -D flags...; returns cpp's status, prints its stderr
    if grep -q '^#ifdef IPODUI_TRIM_DEFAULTS' "$HDR"; then
        printf '#include "%s"\n' "$HDR" | cc -E -x c - -o /dev/null -DHAVE_IPOD_UI "$@" 2>&1
        return
    fi
    # Unguarded header (after the Part 1 integration): the set is on by
    # default, so each -DIPODUI_TRIM_X= names a trim that stays on and every
    # other trim's "#define" becomes "#undef" in a scratch copy (the same
    # edit build mode makes in place).
    local tmp rc keep="" a
    for a in "$@"; do
        case "$a" in -DIPODUI_TRIM_*=) a=${a#-DIPODUI_TRIM_}; keep="$keep ${a%=}" ;; esac
    done
    tmp=$(mktemp "${TMPDIR:-/tmp}/trimchk.XXXXXX")
    # shellcheck disable=SC2086
    python3 -I -c '
import re, sys
s = open(sys.argv[1]).read()
keep = set(sys.argv[3:])
def sub(m):
    return m.group(0) if m.group(2) in keep else "#undef " + m.group(1)
s = re.sub(r"^#define (IPODUI_TRIM_([A-Z0-9_]+))\b", sub, s, flags=re.M)
open(sys.argv[2], "w").write(s)
' "$HDR" "$tmp" $keep
    rc=0
    printf '#include "%s"\n' "$tmp" | cc -E -x c - -o /dev/null -DHAVE_IPOD_UI 2>&1 || rc=$?
    rm -f "$tmp"
    return $rc
}

build_mode() {
    local only="" errs=1 keep=0 jobs=4 dry=0
    while [ $# -gt 0 ]; do
        case "$1" in
        --only) only=$(echo "$2" | tr 'a-z,-' 'A-Z _'); shift 2 ;;
        --no-errors) errs=0; shift ;;
        --keep) keep=1; shift ;;
        --dry) dry=1; shift ;;
        -j) jobs=$2; shift 2 ;;
        -j*) jobs=${1#-j}; shift ;;
        -h|--help) awk 'NR==1{next} /^set -eu/{print; exit} /^#\*+$/{b=!b; next} !b' "$0" | sed 's/^# \{0,1\}//'; exit 0 ;;
        *) echo "usage: $0 [--list|--check] | [--only LIST] [--no-errors] [--keep] [-j N]" >&2
           exit 2 ;;
        esac
    done
    local ROOT; ROOT=$(pwd)
    local out; out=$(closures)
    local all; all=$(echo "$out" | sed -n 's/^ALL: //p')
    guarded=0      # global: the EXIT trap reads it
    grep -q '^#ifdef IPODUI_TRIM_DEFAULTS' "$HDR" && guarded=1
    if [ $guarded = 0 ] && ! git diff --quiet -- "$HDR"; then
        echo "trim-revert: $HDR has uncommitted changes; commit or stash them first" >&2
        exit 2
    fi

    if [ $dry = 1 ]; then
        local c t args
        while read -r c; do
            args=""
            for t in $all; do
                case " $c " in *" $t "*) ;; *) args="$args,$(echo "$t" | tr 'A-Z_' 'a-z-')" ;; esac
            done
            if [ $guarded = 1 ]; then
                echo "closure $c: configure --ipodui${args:+ --ipodui-trim=${args#,}}"
            else
                echo "closure $c: configure --ipodui; header #undef $(for t in $c; do printf 'IPODUI_TRIM_%s ' "$t"; done)"
            fi
        done < <(echo "$out" | sed -n 's/^BUILD: //p')
        return 0
    fi

    # --- the P9 batch lock (shared with variants.sh) and the gate check ---
    BATCH_LOCK=${P9_BATCH_LOCK:-$HOME/.cache/rockbox-p9-batch.lock}
    GATE_LOCK=${SIMPOOL_LOCK:-$HOME/.cache/rockbox-gate.lock}
    MAX_MIN=${P9_BATCH_MAX_MINUTES:-120}
    lock_live() { local p; [ -d "$1" ] || return 1
        p=$(cat "$1/pid" 2>/dev/null || true)
        [ -n "$p" ] && kill -0 "$p" 2>/dev/null; }
    if lock_live "$GATE_LOCK"; then
        echo "trim-revert: the gate lock is held (pid $(cat "$GATE_LOCK/pid"), $(cat "$GATE_LOCK/where" 2>/dev/null)); not starting (M7)" >&2
        exit 2
    fi
    mkdir -p "$(dirname "$BATCH_LOCK")"
    if ! mkdir "$BATCH_LOCK" 2>/dev/null; then
        if lock_live "$BATCH_LOCK"; then
            echo "trim-revert: another P9 batch holds $BATCH_LOCK (pid $(cat "$BATCH_LOCK/pid"), $(cat "$BATCH_LOCK/what" 2>/dev/null))" >&2
            exit 2
        fi
        rm -rf "$BATCH_LOCK"; mkdir "$BATCH_LOCK" || exit 2
    fi
    echo $$ >"$BATCH_LOCK/pid"
    echo "trim-revert.sh $ROOT" >"$BATCH_LOCK/what"
    W=build/trim-revert
    mkdir -p "$W" out/p9
    [ $guarded = 0 ] && cp "$HDR" "$W/variant.h.orig"
    cleanup() {
        [ $guarded = 0 ] && [ -f "$W/variant.h.orig" ] && cp "$W/variant.h.orig" "$HDR"
        [ "$(cat "$BATCH_LOCK/pid" 2>/dev/null)" = $$ ] && rm -rf "$BATCH_LOCK"
        return 0
    }
    trap cleanup EXIT
    trap 'exit 130' INT TERM
    START=$(date +%s)
    elapsed_min() { echo $(( ($(date +%s) - START) / 60 )); }
    kill_tree() { local c; for c in $(pgrep -P "$1" 2>/dev/null); do kill_tree "$c"; done; kill -TERM "$1" 2>/dev/null || true; }
    run_budgeted() { # log cmd...
        local log=$1; shift
        "$@" >"$log" 2>&1 &
        local pid=$!
        while kill -0 "$pid" 2>/dev/null; do
            if [ "$(elapsed_min)" -ge "$MAX_MIN" ]; then
                echo "trim-revert: P9_BATCH_MAX_MINUTES=$MAX_MIN spent; stopping" >&2
                kill_tree "$pid"; wait "$pid" 2>/dev/null || true
                return 124
            fi
            sleep 5
        done
        wait "$pid"
    }
    LOG=out/p9/trim-revert.log
    : >"$LOG"
    say() { echo "$*"; echo "$*" >>"$LOG"; }
    say "trim-revert: $(git rev-parse --short HEAD) $(date -u +%FT%TZ) mode=$([ $guarded = 1 ] && echo configure-trim || echo header-undef) budget=${MAX_MIN}min -j$jobs"

    # set_closure <closure names>: make the header / args revert exactly it
    TRIM_ARGS=""
    set_closure() {
        local c=" $* " t keep_list=""
        for t in $all; do
            case "$c" in *" $t "*) ;; *) keep_list="$keep_list,$(echo "$t" | tr 'A-Z_' 'a-z-')" ;; esac
        done
        if [ $guarded = 1 ]; then
            TRIM_ARGS=${keep_list#,}
            [ -n "$TRIM_ARGS" ] && TRIM_ARGS="--ipodui-trim=$TRIM_ARGS"
        else
            TRIM_ARGS=""
            cp "$W/variant.h.orig" "$HDR"
            python3 -I - "$HDR" "$@" <<'EOF'
import re, sys
p, names = sys.argv[1], sys.argv[2:]
s = open(p).read()
for n in names:
    s, k = re.subn(r"^#define (IPODUI_TRIM_%s)\b" % n, r"#undef \1", s,
                   flags=re.M)
    if k == 0:
        sys.exit("trim-revert: no '#define IPODUI_TRIM_%s' line in %s" % (n, p))
open(p, "w").write(s)
EOF
        fi
    }
    CFG="--target=22 --type=n --ipodui --compiler-prefix=arm-none-eabi-"

    local fail="" nb=0 ne=0 c
    while read -r c; do
        [ -n "$c" ] || continue
        if [ -n "$only" ]; then
            local hit=0 t
            for t in $only; do case " $c " in *" $t "*) hit=1 ;; esac; done
            [ $hit = 1 ] || continue
        fi
        local tag nw; nw=$(echo "$c" | wc -w | tr -d ' ')
        tag=$(echo "$c" | awk '{print tolower($1)}')
        [ "$nw" -gt 1 ] && tag="$tag+$((nw - 1))"
        if [ "$(elapsed_min)" -ge "$MAX_MIN" ]; then
            say "closure $c SKIP budget spent"; fail="$fail $tag:skipped"; continue
        fi
        local d=$W/$tag
        rm -rf "$d"; mkdir -p "$d"
        if ! set_closure $c; then
            say "closure $c FAIL header edit"; fail="$fail $tag:edit"; continue
        fi
        # shellcheck disable=SC2086
        if ! (cd "$d" && "$ROOT/tools/configure" $CFG $TRIM_ARGS </dev/null >configure.log 2>&1); then
            say "closure $c FAIL configure: $(grep -E 'error|ERROR' "$d/configure.log" | head -2 | tr '\n' ' ')"
            fail="$fail $tag:configure"; continue
        fi
        local rc=0
        run_budgeted "$d/make.log" make -C "$d" -j"$jobs" || rc=$?
        local derived="" st=ok
        case " $c " in *" UTF8PROC "*)
            if grep -q '^export NO_UTF8PROC=$' "$d/Makefile" && [ -f "$d/lib/libutf8proc.a" ]; then
                derived="$derived utf8proc=built"
            else derived="$derived utf8proc=MISSING"; st=FAIL; fi ;;
        esac
        case " $c " in *" PLUGINS "*)
            local nrock; nrock=$(find "$d/apps/plugins" -name '*.rock' 2>/dev/null | wc -l | tr -d ' ')
            if [ -f "$d/apps/plugins/mpegplayer/mpegplayer.rock" ]; then
                derived="$derived plugins=$nrock mpegplayer=$(wc -c <"$d/apps/plugins/mpegplayer/mpegplayer.rock" | tr -d ' ')"
            else derived="$derived plugins=$nrock mpegplayer=MISSING"; st=FAIL; fi ;;
        esac
        if [ $rc != 0 ]; then
            local und
            und=$(grep -oE "undefined reference to \`[^']+'" "$d/make.log" | sed "s/.*\`//; s/'//" | sort | uniq -c | sort -rn | head -8 | awk '{printf "%s ", $2}')
            say "closure $c FAIL build$([ $rc = 124 ] && echo ' (budget)') undefined=[${und% }]"
            grep -E "error:|undefined reference" "$d/make.log" | head -8 | sed 's/^/    /' | tee -a "$LOG"
            fail="$fail $tag:build"; continue
        fi
        nb=$((nb + 1))
        local ipod end
        ipod=$(wc -c <"$d/rockbox.ipod" | tr -d ' ')
        end=$(printf '%d' "0x$(arm-none-eabi-nm "$d/rockbox.elf" | awk '$3 == "_end" {print $1}')")
        [ "$st" = ok ] || fail="$fail $tag:derived"
        say "closure $c $st ipod=$ipod end=$end derived=${derived# } dir=$d"
        [ $keep = 1 ] || [ "$st" != ok ] || rm -rf "$d"
    done < <(echo "$out" | sed -n 's/^BUILD: //p')

    if [ $errs = 1 ]; then
        # each edge left dangling must stop configure with the #error
        # naming the need (the --check cases, through the real configure)
        local name needs need clo t
        while IFS=: read -r name needs; do
            clo=$(echo "$out" | sed -n "s/^$name: //p")
            for need in $needs; do
                # revert name's closure except need, which stays trimmed
                local revert=""
                for t in $clo; do [ "$t" = "$need" ] || revert="$revert $t"; done
                set_closure $revert
                local d=$W/errcheck
                rm -rf "$d"; mkdir -p "$d"
                # shellcheck disable=SC2086
                if (cd "$d" && "$ROOT/tools/configure" $CFG $TRIM_ARGS </dev/null >configure.log 2>&1); then
                    say "error $name -> $need FAIL configure accepted the incoherent set"
                    fail="$fail err:$name-$need"
                elif grep '#error\|error:' "$d/configure.log" | grep -q "$need"; then
                    ne=$((ne + 1))
                    say "error $name -> $need ok $(grep -o '#error.*\|error: .*' "$d/configure.log" | head -1)"
                else
                    say "error $name -> $need FAIL wrong message: $(grep -iE 'error' "$d/configure.log" | head -2 | tr '\n' ' ')"
                    fail="$fail err:$name-$need"
                fi
                rm -rf "$d"
            done
        done < <(grep -v '^#' "$DEPS" | grep ': *[A-Z]')
    fi
    say "elapsed $(elapsed_min) min"
    if [ -n "$fail" ]; then
        say "trim-revert FAIL$fail"
        exit 1
    fi
    say "trim-revert PASS builds=$nb errors=$ne"
}

case "$mode" in
--list)
    closures
    echo "distinct closure builds: $(closures | grep -c '^BUILD:')"
    ;;
--check)
    out=$(closures)
    all=$(echo "$out" | sed -n 's/^ALL: //p')
    fail=0
    # every closure revert must preprocess cleanly
    while read -r name rest; do
        name=${name%:}
        defs=""
        for t in $all; do
            case " $rest " in *" $t "*) ;; *) defs="$defs -DIPODUI_TRIM_$t=" ;; esac
        done
        # shellcheck disable=SC2086
        if ! msg=$(cpp_hdr $defs); then
            echo "FAIL revert $name (closure: $rest): $msg"; fail=1
        else
            echo "ok   revert $name (closure: $rest)"
        fi
    done < <(echo "$out" | grep -v -E '^(BUILD|ALL):')
    # every edge left dangling must hit the #error that names it: revert
    # the closure of $name except $need, which stays trimmed
    while IFS=: read -r name needs; do
        clo=$(echo "$out" | sed -n "s/^$name: //p")
        for need in $needs; do
            defs=""
            for t in $all; do
                case " $clo " in
                *" $t "*) [ "$t" = "$need" ] && defs="$defs -DIPODUI_TRIM_$t=" ;;
                *) defs="$defs -DIPODUI_TRIM_$t=" ;;
                esac
            done
            # shellcheck disable=SC2086
            if msg=$(cpp_hdr $defs); then
                echo "FAIL edge $name -> $need: no #error"; fail=1
            elif ! echo "$msg" | grep '#error' | grep -q "$need"; then
                echo "FAIL edge $name -> $need: wrong #error: $msg"; fail=1
            else
                echo "ok   edge $name -> $need: $(echo "$msg" | grep -o '#error.*' | head -1)"
            fi
        done
    done < <(grep -v '^#' "$DEPS" | grep ': *[A-Z]')
    # the shipped default set and the contract default (off) both preprocess
    if grep -q '^#ifdef IPODUI_TRIM_DEFAULTS' "$HDR"; then
        cpp_hdr -DIPODUI_TRIM_DEFAULTS >/dev/null || { echo "FAIL default set"; fail=1; }
        cpp_hdr >/dev/null || { echo "FAIL contract default (off)"; fail=1; }
    else
        defs=""; for t in $all; do defs="$defs -DIPODUI_TRIM_$t="; done
        # shellcheck disable=SC2086
        cpp_hdr $defs >/dev/null || { echo "FAIL default set"; fail=1; }
    fi
    [ $fail = 0 ] && echo "trim-revert --check: PASS" || { echo "trim-revert --check: FAIL"; exit 1; }
    ;;
build)
    build_mode "$@"
    ;;
esac
