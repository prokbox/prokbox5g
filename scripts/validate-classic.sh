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

# CLASSIC COPY of validate.sh as of df33f15bae (before the gate speed-up), kept
# only so the verification in docs/gate-speed-brief.md section 8 can compare the
# old and the new gate on one tree; plus the GATE_ONLY hook. Do not use for real gates.
#
# Phase gate: every phase must pass this before it is merged/closed.
#
#   scripts/validate.sh                    full gate (always everything)
#   scripts/validate.sh --quick            skip bootloader + stock sim
#   scripts/validate.sh --quick --changed  --quick, and only the ipodui-*
#                                           scripts scripts/gate_map.txt
#                                           maps to files changed vs main
#                                           (anything unmapped, or an "ALL"
#                                           rule, runs every ipodui-*
#                                           script; --changed is ignored
#                                           without --quick, since the
#                                           plain full gate always runs
#                                           everything)
#
# Builds (configuring missing build dirs) fw-ui, fw, sim-ui[, sim, boot, boot-ui, fw-rel]
# concurrently (JOBS make -j per dir, default ncpu split across them),
# runs host tests, then runs every tests/ui/*.script against the right
# simulator, with GATE_JOBS parallel workers (default min(16, 2*ncpu,
# mem GiB); one gate or capture per machine at a time (simpool.sh lock);
# each worker gets its own simdisk copy, cloned after the shared disk is
# primed once with the music/test library and a built database, so no
# worker rebuilds it; that priming itself is keyed on a hash of the
# generators and the tagcache/library/sortkey/artcache code and reused
# from $ROCKBOX_SIMDISK_CACHE (default $HOME/.cache/rockbox-simdisk)
# across every worktree's gate with the same inputs, same as the
# ccache wiring in tools/configure -- see simdisk_cache_key() below).
# A script that fails in the parallel run is re-run
# alone, serially, once the parallel phase is done (a sim under worker
# contention can be flakier than one running alone); "flaky-retry ok" if
# that run passes, a real FAIL only if it fails alone too — the retry
# count is printed, and is a smell worth investigating if it keeps
# happening. Finally prints a size report against build/fw/.size-baseline.
# Exit code is non-zero on any failure.
#
# For ipodui-* scripts, each shot with a committed golden image
# tests/ui/golden/<script>/<shot>.png is compared pixel by pixel
# (scripts/pngdiff.py --batch, one process per script covering all its
# goldens); more than GOLDEN_THRESHOLD percent differing pixels (default
# 0.5) fails the gate. Stock scripts are not compared (status-bar clock).
# The full gate also refuses a branch with a TEMP- commit: it fails if
# `git log main..HEAD --format=%s` has a subject starting with "TEMP-"
# (docs/process.md "Contracts as real code": contracts are committed
# headers before fan-out, not per-agent TEMP stubs). GATE_ALLOW_TEMP=1
# overrides it, for the orchestrator reconciling a TEMP commit on its
# way out. --quick never checks this (a running agent's own branch is
# expected to still have them until the orchestrator's integration).
# Shots named in a "# golden: skip <shot>..." line of the script
# (timing-sensitive, e.g. mid-animation) are never compared.
# The LCD/animation trace in out/shots/<script>/sim.log is checked by
# scripts/check_trace.py, the installed shell fonts by
# scripts/check_fonts.sh. Scripts named in tests/ui/PENDING (one per line)
# are skipped with a note: they wait for code that has not been merged.
# Full gate only (not --quick, its own separate sanitizer sim build):
# scripts/risk/sim_ubsan.sh (#38, docs/phase-nav-brief.md Revision 1/
# "The gate runs scripts/risk/sim_ubsan.sh on the #38 scripts" -- it
# already covers every tests/ui/ipodui-*.script, #38's included), a
# report (never fails the gate itself; read its findings).
# The budget-reviewer agent is run separately by the orchestrator.
set -u
ROOT=$(cd "$(dirname "$0")/.." && pwd)
cd "$ROOT"

QUICK=0; CHANGED=0
for a in "$@"; do
    case "$a" in
        --quick) QUICK=1 ;;
        --changed) CHANGED=1 ;;
        *) echo "usage: $0 [--quick] [--quick --changed]" >&2; exit 2 ;;
    esac
done
if [ -n "${GATE_ONLY:-}" ] && [ $QUICK = 0 ]; then
    echo "FAIL: GATE_ONLY is a reduced mode and needs --quick" >&2; exit 2
fi
NCPU=$({ sysctl -n hw.ncpu 2>/dev/null || nproc 2>/dev/null; } || echo 4)
[ "$NCPU" -ge 1 ] 2>/dev/null || NCPU=4
# One gate or golden capture at a time on this machine (all worktrees):
# each sizes its pool to the whole machine. See scripts/simpool.sh.
. scripts/simpool.sh
simpool_machine_lock
GOLDEN_THRESHOLD=${GOLDEN_THRESHOLD:-0.5}
P=--compiler-prefix=arm-none-eabi-
# Which simulator build runs a tests/ui script: the Phase 14a video scripts
# (ipodui-p14a-*) need the sim with the video player (sim-ui-video:
# --ipodui --bcmfw-video); every other ipodui-* script runs on sim-ui; the
# rest on the stock sim.
script_sim() {
    case "$1" in
        ipodui-p14a-*) echo build/sim-ui-video ;;
        ipodui-*) echo build/sim-ui ;;
        *) echo build/sim ;;
    esac
}
fail=0
flaky=0
LIST_ROWS=9,5,4       # rows per view: theme.c list_rows, _two, _art
HEADER_H=20           # theme.c header_h
GATE_TMP=build/.gate  # scratch for this run only; never committed
rm -rf "$GATE_TMP"
mkdir -p "$GATE_TMP"
step() { printf '\n== %s\n' "$*"; }
bad()  { echo "FAIL: $*"; fail=1; }

step "TEMP- commits (full gate only)"
if [ $QUICK = 1 ]; then
    echo "skip (quick mode; checked at integration)"
elif ! git rev-parse --verify main >/dev/null 2>&1; then
    echo "note: no local 'main' ref; skipping"
else
    temps=$(git log main..HEAD --format=%s 2>/dev/null | grep -E '^TEMP-' || true)
    if [ -n "$temps" ]; then
        if [ "${GATE_ALLOW_TEMP:-0}" = 1 ]; then
            echo "note: TEMP- commit(s) present, allowed by GATE_ALLOW_TEMP=1:"
            echo "$temps" | sed 's/^/  /'
        else
            echo "$temps" | sed 's/^/  /'
            bad "TEMP- commit(s) on this branch (docs/process.md 'Contracts as real code': no per-agent TEMP stubs; override with GATE_ALLOW_TEMP=1 for the orchestrator reconciling them on the way out)"
        fi
    else
        echo "none"
    fi
fi

# clone_disk <src-simdisk> <dst>: a fast copy-on-write clone on APFS
# (cp -c), falling back to a plain recursive copy elsewhere.
clone_disk() {
    rm -rf "$2"
    if [ "$(uname)" = Darwin ] && /bin/cp -c -R "$1" "$2" 2>/dev/null; then  # /bin/cp: a GNU cp earlier in PATH has no -c
        return 0
    fi
    cp -R "$1" "$2"
}

# Shared simdisk cache (process audit 2 #8): the generated library/music
# content and the built+committed database are expensive (ffmpeg,
# python, and for the database a real sim boot doing a full tagcache
# scan+commit) and, for one flavor (sim vs sim-ui -- their database
# commit timing differs structurally, so the two are never mixed) and
# the same inputs, byte-for-byte reproducible no matter which worktree
# primed them -- the same reasoning as the ccache wiring in
# tools/configure. Keyed on every file whose content changes what gets
# generated or how it is indexed; any other worktree's gate with an
# identical key skips regenerating and reuses this one's result.
# ROCKBOX_SIMDISK_CACHE overrides the location (default: one shared dir
# per machine, like CCACHE_DIR).
SIMDISK_CACHE_ROOT=${ROCKBOX_SIMDISK_CACHE:-$HOME/.cache/rockbox-simdisk}
# Everything that changes what is generated (setup_*, fixtures) or how it
# is indexed (tagcache, tagtree, the settings defaults that gate it, the
# shell's library/sort/art code, the sim's db hook). The key also covers
# the configure flavour via the cache dir name (sim / sim-ui).
SIMDISK_CACHE_INPUTS="tests/ui/setup_music.sh tests/ui/setup_library.py
tests/ui/setup_library.sh apps/tagcache.c apps/tagcache.h apps/tagtree.c
apps/settings_list.c apps/ipodui/library.c apps/ipodui/library.h
apps/ipodui/libfile.h apps/ipodui/sortkey.c apps/ipodui/artcache.c
apps/ipodui/dbgen.c apps/ipodui/dbgen.h apps/ipodui/dbjournal.c apps/ipodui/dbjournal.h
apps/ipodui/artfmt.c apps/ipodui/artkey.c apps/ipodui/lib_shuffle.c
apps/ipodui/np_playback.c uisimulator/common/sim_db.c
uisimulator/common/sim_cmd_np.c"
# The subtrees worth caching/restoring; only the ones actually present
# (music-only priming has no database) are copied either way.
SIMDISK_CACHE_PATHS="Music Library Podcasts Audiobooks Playlists Movies Misc
.rockbox/config.cfg .rockbox/database_changelog.txt .rockbox/artcache
.rockbox/database_gen0 .rockbox/database_gen1 .rockbox/database_gen2
.rockbox/database_manifest0.dat .rockbox/database_manifest1.dat
.rockbox/database_stats.jnl .rockbox/database_stats.prev"
# Bump when what a primed disk holds changes without any input file
# changing (2: the priming boot also builds the art cache, art_wait, so
# every clone starts with the covers the goldens show instead of an empty
# cache that fills during the script)
SIMDISK_CACHE_SALT=3

simdisk_has_database() {
    [ -f "$1/.rockbox/database_idx.tcd" ] \
        || [ -f "$1/.rockbox/database_manifest0.dat" ] \
        || [ -f "$1/.rockbox/database_manifest1.dat" ]
}
# plus every .rockbox/database_*.tcd actually present (idx AND the per-tag
# files and database_tmp.tcd: caching only idx left a restored database
# without its tag files, "database not usable")
cache_db_files() { # <dir> -> names relative to <dir>
    local f
    for f in "$1"/.rockbox/database_*.tcd; do
        [ -e "$f" ] && echo ".rockbox/$(basename "$f")"
    done
}

# One path per line (a path may hold a space): the word-split list above,
# the database files present in <dir>, and the video fixtures' category
# folder (Phase 14a: tests/fixtures/videos/mkvideos.sh).
cache_paths() { # <dir>
    local p
    for p in $SIMDISK_CACHE_PATHS; do echo "$p"; done
    cache_db_files "$1"
    echo "TV Shows"
}

hash_tool() { command -v sha256sum >/dev/null 2>&1 && sha256sum || shasum -a 256; }
# Phase 14a: the sim-ui-video flavour's disk also holds the video fixtures
# and is indexed by the video classifier, so its key adds their inputs (the
# other flavours' keys, and so their cached disks, are unchanged).
SIMDISK_VIDEO_INPUTS="tests/fixtures/videos/mkvideos.sh bcmfw/video/stage6-real.mp4
apps/ipodui/vclass_core.c apps/ipodui/vclass.h apps/ipodui/vixpass.c apps/ipodui/vixpass_core.c
lib/rbcodec/metadata/mp4.c lib/rbcodec/metadata/metadata.c"
simdisk_cache_key() { # [base]
    { echo "salt $SIMDISK_CACHE_SALT"; cat $SIMDISK_CACHE_INPUTS 2>/dev/null
      [ "${1:-}" = sim-ui-video ] && cat $SIMDISK_VIDEO_INPUTS 2>/dev/null
      true; } | hash_tool | awk '{print $1}'
}
copy_tree() { # <src> <dst>: cp -c clone on APFS, else a plain copy
    if [ "$(uname)" = Darwin ] && /bin/cp -c -R "$1" "$2" 2>/dev/null; then  # /bin/cp: a GNU cp earlier in PATH has no -c
        return 0
    fi
    cp -R "$1" "$2"
}
simdisk_cache_restore() { # <simdisk> <cachedir> -- 0 on a usable hit
    local disk=$1 cache=$2 p got=0
    [ -f "$cache/.ready" ] || return 1
    rm -f "$disk"/.rockbox/database_*.tcd
    rm -f "$disk"/.rockbox/database_gen[012]/database* \
          "$disk"/.rockbox/database_manifest[01].* \
          "$disk"/.rockbox/database_stats.jnl \
          "$disk"/.rockbox/database_stats.prev \
          "$disk"/.rockbox/database_stats.tmp
    while IFS= read -r p; do
        [ -e "$cache/$p" ] || continue
        mkdir -p "$(dirname "$disk/$p")"
        rm -rf "$disk/$p"
        copy_tree "$cache/$p" "$disk/$p" && got=1
    done < <(cache_paths "$cache")
    [ $got = 1 ]
}
simdisk_cache_save() { # <simdisk> <cachedir>
    local disk=$1 cache=$2 tmp="${2}.tmp.$$" p
    rm -rf "$tmp"; mkdir -p "$tmp"
    while IFS= read -r p; do
        [ -e "$disk/$p" ] || continue
        mkdir -p "$(dirname "$tmp/$p")"
        copy_tree "$disk/$p" "$tmp/$p"
    done < <(cache_paths "$disk")
    touch "$tmp/.ready"
    mkdir -p "$(dirname "$cache")"
    rm -rf "$cache"
    mv -f "$tmp" "$cache"
}

step "builds"
# Which builds run. --full builds everything. --quick builds fw-ui and
# sim-ui only; stock fw is built when a file outside apps/ipodui, art/,
# tests/, docs/ or scripts/ changed (it is what proves guarded upstream
# edits keep stock byte-identical); boot only when bootloader/ or
# firmware/ changed. The stock sim is full-gate only. No merge-base with
# main, or an empty change list, builds what --quick always built.
BUILD_DIRS="fw-ui fw sim-ui sim boot boot-ui fw-rel sim-ui-video"
FWV=1   # compile-only fw-ui-video, built after the parallel builds (below)
if [ $QUICK = 1 ]; then
    BUILD_DIRS="fw-ui sim-ui"
    FWV=0
    QMB=$(git merge-base main HEAD 2>/dev/null || true)
    if [ -z "$QMB" ]; then
        BUILD_DIRS="fw-ui fw sim-ui sim-ui-video"
        FWV=1
        echo "note build fw: no merge-base with main, cannot tell what changed"
        echo "skip build boot (--quick: no merge-base to tell bootloader/ firmware/ changed)"
    else
        QFILES=$( {
            git diff --name-only "$QMB" HEAD 2>/dev/null
            git diff --name-only HEAD 2>/dev/null
            git status --porcelain 2>/dev/null | sed -n 's/^?? //p'
        } | sort -u )
        # Phase 14a video builds (sim-ui-video for the ipodui-p14a-* scripts,
        # compile-only fw-ui-video for the device path) in --quick only when
        # something they compile changed; GATE_FW_VIDEO=1 forces
        # fw-ui-video, GATE_SIM_VIDEO=0 drops sim-ui-video
        if printf '%s\n' "$QFILES" | grep -qE '^(apps/|uisimulator/|firmware/|lib/|bcmfw/|tools/configure|tests/ui/ipodui-p14a-|tests/fixtures/videos/|scripts/(validate|shots)\.sh|scripts/gate_map\.txt)'; then
            [ "${GATE_SIM_VIDEO:-1}" = 0 ] || BUILD_DIRS="$BUILD_DIRS sim-ui-video"
        else
            echo "skip build sim-ui-video (--quick: no apps/, uisimulator/, firmware/, lib/, bcmfw/ or p14a test file changed)"
        fi
        if [ "${GATE_FW_VIDEO:-0}" = 1 ] \
                || printf '%s\n' "$QFILES" | grep -qE '^(apps/|firmware/|lib/|bcmfw/|tools/configure)'; then
            FWV=1
        else
            FWV=0
            echo "skip build fw-ui-video (--quick: no apps/, firmware/, lib/ or bcmfw/ change; GATE_FW_VIDEO=1 forces it)"
        fi
        qstock=$(printf '%s\n' "$QFILES" | grep -vE '^(apps/ipodui/|art/|tests/|docs/|scripts/|$)' | head -3 || true)
        if [ -n "$qstock" ]; then
            BUILD_DIRS="fw fw-ui sim-ui $(echo $BUILD_DIRS | sed 's/^fw-ui sim-ui *//')"
            echo "note build fw: stock-affecting file(s) changed: $(echo $qstock | tr '\n' ' ')..."
        else
            echo "skip build fw (--quick: only apps/ipodui, art/, tests/, docs/, scripts/ changed)"
        fi
        if printf '%s\n' "$QFILES" | grep -qE '^(bootloader|firmware)/'; then
            BUILD_DIRS="$BUILD_DIRS boot boot-ui"
            echo "note build boot, boot-ui: bootloader/ or firmware/ changed"
        else
            echo "skip build boot (--quick: bootloader/ and firmware/ unchanged)"
        fi
    fi
    echo "skip build sim (--quick: stock sim is --full only)"
fi
NBUILDS=$(echo $BUILD_DIRS | wc -w | tr -d ' ')
# Builds run concurrently: about ncpu+2 compilers in total, split across
# the builds (min 2 each), so a gate never oversubscribes an 8 GB machine
# (5 builds at -j4 plus 16 sims froze it on 2026-10-09). JOBS=N overrides.
if [ -z "${JOBS:-}" ]; then
    JOBS=$(( (NCPU + 2) / NBUILDS )); [ "$JOBS" -lt 2 ] && JOBS=2
fi

BOOT_IDENTITY_VERSION=ipodui-boot-identity
build() { # dir, configure args...
    local d=build/$1; shift
    # configure again when the Makefile is missing, or older than
    # apps/features.txt or tools/configure (a stale build dir does not pick
    # up a feature-list change by itself)
    if [ ! -f "$d/Makefile" ] || [ "$d/Makefile" -ot apps/features.txt ] \
        || [ "$d/Makefile" -ot tools/configure ]; then
        [ -f "$d/Makefile" ] && echo "reconfigure $d (Makefile older than apps/features.txt or tools/configure)"
        mkdir -p "$d"
        (cd "$d" && ../../tools/configure "$@" </dev/null >configure.log 2>&1) \
            || { bad "configure $d"; return 1; }
    fi
    # build/boot: the version string pinned, so its bootloader.bin can be
    # compared byte for byte with scripts/stock_boot.sha256 (main's)
    local mkv=""
    [ "$d" = build/boot ] && mkv="VERSION=$BOOT_IDENTITY_VERSION"
    if make -C "$d" -j"$JOBS" $mkv >"$d/make.log" 2>&1; then
        echo "ok   $d"
    else
        bad "build $d"; grep -nE "error" "$d/make.log" | head -10; return 1
    fi
}

# Each build dir is independent (own Makefile, own object tree), so they
# run concurrently; each keeps its own build/<dir>/{configure,make}.log.
# make itself still stops at a dir's own first error (no -k); results are
# collected and printed in one block, sorted by dir, after every build
# finishes or fails — one build failing never kills the others early.
for d in $BUILD_DIRS; do
    case "$d" in
        fw-ui)  set -- --target=22 --type=n --ipodui $P ;;
        fw)     set -- --target=22 --type=n $P ;;
        sim-ui) set -- --target=22 --type=s --ipodui ;;
        sim)    set -- --target=22 --type=s ;;
        boot)   set -- --target=22 --type=b $P ;;
        boot-ui) set -- --target=22 --type=b --ipodui $P ;;
        fw-rel) set -- --target=22 --type=n --ipodui --ipodui-release $P ;;
        sim-ui-video) set -- --target=22 --type=s --ipodui --bcmfw-video ;;
    esac
    ( build "$d" "$@"; echo $? >"$GATE_TMP/build-$d.status" ) \
        >"$GATE_TMP/build-$d.log" 2>&1 &
done
wait
for d in $(printf '%s\n' $BUILD_DIRS | sort); do
    cat "$GATE_TMP/build-$d.log"
    st=$(cat "$GATE_TMP/build-$d.status" 2>/dev/null || echo 1)
    if [ "$st" != 0 ]; then fail=1; fi
done

# Phase 14a fw-ui-video (docs/phase14a-brief.md 6.1, 8): the device path of
# the video player (--ipodui --bcmfw-devtest --bcmfw --bcmfw-video
# --bcmfw-l1), compile-only ("make bin": the core image, no codecs or
# plugins), after the parallel builds so the memory peak doesn't grow. Its
# .text is the baseline for the brief's 24 KB budget (size step below).
if [ "$FWV" = 1 ]; then
    fwv_args="--target=22 --type=n --ipodui --bcmfw-devtest --bcmfw --bcmfw-video --bcmfw-l1 $P"
    if [ ! -f build/fw-ui-video/Makefile ] || [ build/fw-ui-video/Makefile -ot tools/configure ] \
            || [ build/fw-ui-video/Makefile -ot apps/features.txt ] \
            || [ "$(cat build/fw-ui-video/.gate-args 2>/dev/null)" != "$fwv_args" ]; then
        rm -rf build/fw-ui-video; mkdir -p build/fw-ui-video
        # shellcheck disable=SC2086
        (cd build/fw-ui-video && ../../tools/configure $fwv_args </dev/null >configure.log 2>&1) \
            && echo "$fwv_args" >build/fw-ui-video/.gate-args \
            || bad "configure build/fw-ui-video ($fwv_args): $(grep -E 'ERROR|rror' build/fw-ui-video/configure.log | head -2 | tr '\n' ' ')"
    fi
    if [ -f build/fw-ui-video/.gate-args ]; then
        if make -C build/fw-ui-video -j"$NCPU" bin >build/fw-ui-video/make.log 2>&1; then
            echo "ok   build/fw-ui-video (compile-only: $fwv_args)"
        else
            bad "build build/fw-ui-video ($fwv_args)"; grep -nE "error" build/fw-ui-video/make.log | head -10
        fi
    fi
else
    echo "skip build fw-ui-video (see above)"
fi

# Phase 9 fw-alt (docs/phase9-brief.md 3.5 row 2, 7.8): the other corner
# of the variant matrix (hdd, 32 MB, no haptics; trims and B's core
# function/data sections as configure gives them), compile-only ("make
# bin": the core image, no codecs or plugins). Full gate only, started
# after the parallel builds finish so the memory peak doesn't grow; its
# size is checked in the size step. While the header still guards the
# trim set (contract state) it passes --ipodui-trim=all, as fw-ui would
# need to.
if [ $QUICK = 0 ]; then
    alt_args="--target=22 --type=n --ipodui --storage=hdd --ram=32 --no-haptics $P"
    grep -q '^#ifdef IPODUI_TRIM_DEFAULTS' firmware/export/config/ipodvideo-variant.h \
        && alt_args="$alt_args --ipodui-trim=all"
    if [ ! -f build/fw-alt/Makefile ] || [ build/fw-alt/Makefile -ot tools/configure ] \
            || [ build/fw-alt/Makefile -ot firmware/export/config/ipodvideo-variant.h ] \
            || [ "$(cat build/fw-alt/.p9-args 2>/dev/null)" != "$alt_args" ]; then
        rm -rf build/fw-alt; mkdir -p build/fw-alt
        # shellcheck disable=SC2086
        (cd build/fw-alt && ../../tools/configure $alt_args </dev/null >configure.log 2>&1) \
            && echo "$alt_args" >build/fw-alt/.p9-args \
            || bad "configure build/fw-alt ($alt_args): $(grep -E 'ERROR|error' build/fw-alt/configure.log | head -2 | tr '\n' ' ')"
    fi
    if [ -f build/fw-alt/.p9-args ]; then
        if make -C build/fw-alt -j"$NCPU" bin >build/fw-alt/make.log 2>&1; then
            echo "ok   build/fw-alt (compile-only: $alt_args)"
        else
            bad "build build/fw-alt ($alt_args)"; grep -nE "error" build/fw-alt/make.log | head -10
        fi
    fi
else
    echo "skip build fw-alt (--quick: full gate only)"
fi

# #60 developer test build (--bcmfw-devtest --bcmfw): the BCM device test
# and the opt-in boot path are compiled only here, compile-only ("make
# bin"), full gate only, after the parallel builds like fw-alt.
if [ $QUICK = 0 ]; then
    bcm_args="--target=22 --type=n --ipodui --bcmfw-devtest --bcmfw $P"
    if [ ! -f build/fw-bcmfw/Makefile ] || [ build/fw-bcmfw/Makefile -ot tools/configure ] \
            || [ "$(cat build/fw-bcmfw/.gate-args 2>/dev/null)" != "$bcm_args" ]; then
        rm -rf build/fw-bcmfw; mkdir -p build/fw-bcmfw
        # shellcheck disable=SC2086
        (cd build/fw-bcmfw && ../../tools/configure $bcm_args </dev/null >configure.log 2>&1) \
            && echo "$bcm_args" >build/fw-bcmfw/.gate-args \
            || bad "configure build/fw-bcmfw ($bcm_args)"
    fi
    if [ -f build/fw-bcmfw/.gate-args ]; then
        if make -C build/fw-bcmfw -j"$NCPU" bin >build/fw-bcmfw/make.log 2>&1; then
            echo "ok   build/fw-bcmfw (compile-only: --bcmfw-devtest --bcmfw)"
            bw=$(grep -hE "warning:" build/fw-bcmfw/make.log | grep -E "bcm-devtest|lcd-video|devbcm" | sort -u)
            [ -n "$bw" ] && { echo "$bw"; bad "warnings in the BCM device test / boot path"; }
        else
            bad "build build/fw-bcmfw (--bcmfw-devtest --bcmfw)"; grep -nE "error" build/fw-bcmfw/make.log | head -10
        fi
    fi
else
    echo "skip build fw-bcmfw (--quick: full gate only)"
fi

step "fonts (sim-ui)"
# the shell's own fonts (apps/ipodui/fonts/MANIFEST) must be installed;
# refresh simdisk first so a stale install from an older build can't pass
if [ -f apps/ipodui/fonts/MANIFEST ] && [ -f build/sim-ui/Makefile ]; then
    make -C build/sim-ui install >build/sim-ui/install.log 2>&1 \
        || bad "make install build/sim-ui"
fi
scripts/check_fonts.sh build/sim-ui || bad "fonts"

step "new warnings in our code"
W=$(grep -hE "warning:" build/fw-ui/make.log build/sim-ui/make.log 2>/dev/null \
    | grep -E "apps/ipodui/|apps/audiobook|metadata/chapters|metadata/mp4\.c" | sort -u)
if [ -n "$W" ]; then echo "$W"; bad "warnings in project code"; else echo "none"; fi

step "host tests"
scripts/test.sh || bad "host tests"

step "trace rules self-test (Phase 8a)"
python3 tests/trace/test_p8a_rules.py >"$GATE_TMP/trace-selftest.log" 2>&1 \
    && echo "ok   check_trace.py p8a rules ($(grep -c '^ok' "$GATE_TMP/trace-selftest.log") cases)" \
    || { cat "$GATE_TMP/trace-selftest.log"; bad "check_trace.py p8a rules self-test"; }

step "trace rules self-test (idle-build, #58)"
python3 tests/trace/test_idle_rules.py >"$GATE_TMP/trace-idle.log" 2>&1 \
    && echo "ok   check_trace.py idle-build rules" \
    || { cat "$GATE_TMP/trace-idle.log"; bad "check_trace.py idle-build rules self-test"; }

step "trace rules self-test (Phase 11)"
python3 tests/trace/test_p11_rules.py >"$GATE_TMP/trace-selftest11.log" 2>&1 \
    && echo "ok   check_trace.py p11 rules ($(grep -c '^ok' "$GATE_TMP/trace-selftest11.log") cases)" \
    || { cat "$GATE_TMP/trace-selftest11.log"; bad "check_trace.py p11 rules self-test"; }

step "trace rules self-test (Phase 15)"
python3 tests/trace/test_p15_rules.py >"$GATE_TMP/trace-selftest15.log" 2>&1 \
    && echo "ok   check_trace.py p15 rules ($(grep -c '^ok' "$GATE_TMP/trace-selftest15.log") cases)" \
    || { cat "$GATE_TMP/trace-selftest15.log"; bad "check_trace.py p15 rules self-test"; }

step "trace rules self-test (Phase 12)"
python3 tests/trace/test_p12_rules.py >"$GATE_TMP/trace-selftest12.log" 2>&1 \
    && echo "ok   check_trace.py p12 rules ($(grep -c '^ok' "$GATE_TMP/trace-selftest12.log") cases)" \
    || { cat "$GATE_TMP/trace-selftest12.log"; bad "check_trace.py p12 rules self-test"; }

step "bcmfw benches (#60)"
# Our VideoCore image and its tools against vc2sim (scripts/bcmfw_benches.sh,
# about 55 s wall, in parallel). Full gate: always. --quick: only when
# bcmfw/, tools/bcmre/ or the BCM device-test files changed, or when the
# change list is unknown. Each bench skips with a printed note on a machine
# without vc4-elf-gcc or Apple's reference corpus.
if [ $QUICK = 0 ] || [ -z "${QMB:-}" ] || printf '%s\n' "${QFILES:-}" \
        | grep -qE '^(bcmfw/|tools/bcmre/|firmware/target/arm/ipod/video/(bcm-|lcd-video\.c)|apps/ipodui/devbcm|scripts/bcmfw_benches\.sh)'; then
    scripts/bcmfw_benches.sh || bad "bcmfw benches"
else
    echo "skip bcmfw benches (--quick: bcmfw/, tools/bcmre/ and the BCM files unchanged)"
fi

step "ui scripts: selecting"
# --changed (opt-in, --quick only): narrow the ipodui-* selection to what
# scripts/gate_map.txt maps the changed files (vs the merge-base with
# main, plus uncommitted changes) to. Anything unmapped, or matching an
# "ALL" rule, means every ipodui-* script still runs — the safe default.
CHANGED_ALL=1
if [ $CHANGED = 1 ] && [ $QUICK = 1 ]; then
    MERGE_BASE=$(git merge-base main HEAD 2>/dev/null || true)
    if [ -z "$MERGE_BASE" ]; then
        echo "note --changed: no merge-base with main; running every ipodui-* script"
    else
        CHANGED_FILES=$( {
            git diff --name-only "$MERGE_BASE" HEAD 2>/dev/null
            git diff --name-only HEAD 2>/dev/null
            git status --porcelain 2>/dev/null | sed -n 's/^?? //p'
        } | sort -u )
        if [ -z "$CHANGED_FILES" ]; then
            echo "note --changed: no changed files vs main; running every ipodui-* script"
        else
            CHANGED_ALL=0
            : >"$GATE_TMP/changed-patterns"
            printf '%s\n' "$CHANGED_FILES" | while IFS= read -r path; do
                [ -n "$path" ] || continue
                matched=0
                while read -r glob pats; do
                    case "$glob" in ''|'#'*) continue ;; esac
                    case "$path" in
                        $glob)
                            matched=1
                            if [ "$pats" = ALL ]; then
                                echo ALL >>"$GATE_TMP/changed-all"
                            else
                                printf '%s\n' $pats >>"$GATE_TMP/changed-patterns"
                            fi
                            ;;
                    esac
                done <scripts/gate_map.txt
                [ $matched -eq 0 ] && echo ALL >>"$GATE_TMP/changed-all"
            done
            if [ -f "$GATE_TMP/changed-all" ]; then
                CHANGED_ALL=1
                echo "note --changed: an unmapped or core-rule file changed; running every ipodui-* script"
            else
                sort -u "$GATE_TMP/changed-patterns" -o "$GATE_TMP/changed-patterns"
                nfiles=$(printf '%s\n' "$CHANGED_FILES" | wc -l | tr -d ' ')
                npat=$(wc -l <"$GATE_TMP/changed-patterns" | tr -d ' ')
                echo "note --changed: $npat script pattern(s) from $nfiles changed file(s)"
            fi
        fi
    fi
elif [ $CHANGED = 1 ]; then
    echo "note --changed ignored without --quick; running everything"
fi

# Build the job list: every tests/ui/*.script that should run this gate,
# skipping fonts* (manual comparison), tests/ui/PENDING entries, stock
# scripts under --quick, and (under --quick --changed, when narrowed)
# ipodui-* scripts gate_map.txt doesn't cover.
job_names=""
for s in tests/ui/*.script; do
    n=$(basename "$s" .script)
    case "$n" in fonts*) continue ;; esac
    if [ -f tests/ui/PENDING ] && sed 's/#.*//' tests/ui/PENDING \
            | tr -s ' \t' '\n' | grep -qx "$n"; then
        echo "skip $n (listed in tests/ui/PENDING)"
        continue
    fi
    # "# pending: <why>" in a script's header (tests/ui/README.md): written
    # ahead of the code it tests; skipped with its reason until the line is
    # deleted. GATE_PENDING=run runs such scripts anyway (to see how far
    # they get).
    if [ "${GATE_PENDING:-skip}" != run ] \
            && pend=$(sed -n 's/^#[[:space:]]*pending:[[:space:]]*//p' "$s" | head -1) \
            && [ -n "$pend" ]; then
        echo "skip $n (pending: $pend)"
        continue
    fi
    # GATE_ONLY="pattern ..." (verification only, --quick only): run just the
    # scripts matching these name globs; a named reduced mode, printed in the header
    if [ -n "${GATE_ONLY:-}" ]; then
        only=0
        for pat in $GATE_ONLY; do case "$n" in $pat) only=1 ;; esac; done
        [ $only = 1 ] || continue
    fi
    b=$(script_sim "$n")
    # "# needs: modules": only on a sim built with the module loader
    # (-DIPODUI_WITH_MODULES, off by default since #106)
    if grep -qE '^#[[:space:]]*needs:[[:space:]]*modules' "$s" \
            && ! grep -q 'IPODUI_WITH_MODULES' "$b/Makefile" 2>/dev/null; then
        echo "skip $n (needs: modules; $b is the resident build)"
        continue
    fi
    if [ $QUICK = 1 ] && [ "$b" = build/sim ]; then
        continue
    fi
    if [ "$b" = build/sim-ui-video ]; then
        case " $BUILD_DIRS " in
            *" sim-ui-video "*) ;;
            *) echo "skip $n (sim-ui-video not built this run: no video-relevant file changed)"
               continue ;;
        esac
    fi
    if [ $QUICK = 1 ] && [ $CHANGED = 1 ] && [ $CHANGED_ALL = 0 ] \
            && [ "$b" != build/sim ]; then
        hit=0
        while IFS= read -r pat; do
            case "$n" in $pat) hit=1; break ;; esac
        done <"$GATE_TMP/changed-patterns"
        if [ $hit -eq 0 ]; then
            echo "skip $n (--changed: not affected)"
            continue
        fi
    fi
    job_names="$job_names $n"
done

# Distinct base sim dirs this gate actually needs.
dirs_in_use=""
for n in $job_names; do
    d=$(script_sim "$n")
    case " $dirs_in_use " in
        *" $d "*) ;;
        *) dirs_in_use="$dirs_in_use $d" ;;
    esac
done

# prime_build <dir> <base> <music?> <db?> <video?> <library?>: generates the
# content and builds+commits the database on the shared disk; sets $broken.
# db? = a database is needed (library scripts and the video scripts);
# library? = the Phase 4 test library goes on the disk; video? = the Phase
# 14a video fixtures (tests/fixtures/videos/mkvideos.sh) go on the disk.
prime_build() {
    local d=$1 base=$2 wm=$3 wl=$4 wv=${5:-0} wlib=${6:-$4} out scan_rc
    broken=0
    if [ $wm = 1 ]; then
        tests/ui/setup_music.sh "$d/simdisk" || { bad "prime $d: setup_music.sh"; broken=1; }
    fi
    if [ $broken = 0 ] && [ $wlib = 1 ]; then
        tests/ui/setup_library.sh "$d/simdisk" \
            || { bad "prime $d: setup_library.sh"; broken=1; }
    fi
    if [ $broken = 0 ] && [ $wv = 1 ]; then
        tests/fixtures/videos/mkvideos.sh --install "$d/simdisk" \
            || { bad "prime $d: mkvideos.sh (video fixtures)"; broken=1; }
    fi
    # Build (and, for the stock sim, commit) the database once on the
    # shared disk, exactly as shots.sh's own priming boot would for the
    # first library script, so no worker clone has to redo it.
    if [ $broken = 0 ] && [ $wl = 1 ] \
            && ! simdisk_has_database "$d/simdisk"; then
        # shots.sh requires at least one "shot"; it is thrown away.
        if [ ! -f "$d/simdisk/.rockbox/database_tmp.tcd" ]; then
            printf 'build_db\nwait_db 45000\nart_wait 60000\nshot prime\nquit\n' \
                >"$GATE_TMP/prime-$base.script"
        else
            printf 'wait_db 45000\nart_wait 60000\nshot prime\nquit\n' \
                >"$GATE_TMP/prime-$base.script"
        fi
        out=$(SIM_BUILD="$d" scripts/shots.sh "$GATE_TMP/prime-$base.script" \
                  "$GATE_TMP/prime-$base-out" 2>&1)
        scan_rc=$?
        # Pre-existing bug fixed here: shots.sh itself treats "commit
        # delayed" as a run FAILURE (exit 1; see its own "wait_db ...
        # commit delayed" check), but that is exactly the EXPECTED
        # outcome of a stock sim's first scan (no dircache buffer to
        # commit into in this boot -- see tests/ui/README.md "Database
        # in the sim"). database_tmp.tcd existing confirms the scan
        # itself succeeded; treat that combination as success too, same
        # as a boot that returned 0, so the follow-up wait_db-only boot
        # below still runs. Any OTHER failure (scan error, timeout with
        # no scan result) still fails priming.
        if [ $scan_rc -ne 0 ] && [ -f "$d/simdisk/.rockbox/database_tmp.tcd" ] \
                && echo "$out" | grep -q "commit delayed"; then
            scan_rc=0
        fi
        if [ $scan_rc -eq 0 ]; then
            # stock sim: that boot may only commit at the *next* boot
            # ("commit delayed"); one more wait_db-only boot forces it so
            # every worker clone inherits a ready database_idx.tcd.
            if ! simdisk_has_database "$d/simdisk"; then
                printf 'wait_db 45000\nart_wait 60000\nshot prime2\nquit\n' \
                    >"$GATE_TMP/prime2-$base.script"
                if ! out=$(SIM_BUILD="$d" scripts/shots.sh \
                               "$GATE_TMP/prime2-$base.script" \
                               "$GATE_TMP/prime-$base-out" 2>&1); then
                    bad "prime $d: commit database"; echo "$out" | tail -5
                    broken=1
                fi
            fi
        else
            bad "prime $d: build database"; echo "$out" | tail -5
            broken=1
        fi
    fi
}

# prime_verify <dir> <base>: boots the primed disk once and requires a
# usable database (the check the workers' scripts rely on) with the
# index files present; a stale or torn database fails here, not in 80
# scripts with "wait_db timed out".
prime_verify() {
    local d=$1 base=$2
    simdisk_has_database "$d/simdisk" || return 1
    printf 'wait_db 30000\nshot verify\nquit\n' >"$GATE_TMP/verify-$base.script"
    SIM_BUILD="$d" scripts/shots.sh "$GATE_TMP/verify-$base.script" \
        "$GATE_TMP/verify-$base-out" >"$GATE_TMP/verify-$base.log" 2>&1 \
        || return 1
    # tagcache heals a torn database by rescanning ("scan done"); a good
    # primed disk boots straight into the committed one, so a rescan means
    # the cached/primed state was stale or broken
    if grep -q 'tagcache: scan done' "$GATE_TMP/verify-$base-out/sim.log" 2>/dev/null; then
        echo "note prime $d: verify boot had to rescan (stale or torn database)"
        return 1
    fi
    return 0
}

step "ui scripts: priming shared disk(s) once"
for d in $dirs_in_use; do
    base=$(basename "$d")
    if [ ! -d "$d/simdisk/.rockbox/codecs" ]; then
        if [ ! -f "$d/Makefile" ]; then
            bad "prime $d: not built"
            touch "$GATE_TMP/broken-$base"
            continue
        fi
        make -C "$d" install >"$GATE_TMP/install-$base.log" 2>&1 \
            || { bad "make install $d"; cat "$GATE_TMP/install-$base.log"; \
                 touch "$GATE_TMP/broken-$base"; continue; }
    fi
    # music/library needed by any job assigned to this dir (ipodui-*
    # always wants the mini music library, same rule as shots.sh).
    # setup_music.sh/setup_library.sh are plain host-side generators (no
    # sim boot); only a database build needs one.
    wm=0; wl=0; wv=0; wlib=0
    for n in $job_names; do
        nb=$(script_sim "$n")
        [ "$nb" = "$d" ] || continue
        case "$n" in ipodui-*) wm=1 ;; esac
        s=tests/ui/$n.script
        grep -qE '^#[[:space:]]*needs:[[:space:]]*music' "$s" && wm=1
        grep -qE '^#[[:space:]]*needs:[[:space:]]*library' "$s" && { wl=1; wlib=1; }
        # video scripts (ipodui-p14a-*, or "# needs: video") want the video
        # fixtures on the disk and a database that indexed them
        case "$n" in ipodui-p14a-*) wl=1; wv=1 ;; esac
        grep -qE '^#[[:space:]]*needs:[[:space:]]*video' "$s" && { wl=1; wv=1; }
    done
    if [ $wm = 0 ] && [ $wl = 0 ]; then
        echo "ok   prime $d (no music/library needed)"
        continue
    fi
    simkey=$(simdisk_cache_key "$base")
    simcache="$SIMDISK_CACHE_ROOT/$base-$simkey"
    attempt=0; broken=0; origin=built
    while :; do
        # Hermetic: a hit restores onto a freshly installed disk, so a stray
        # file a developer left in $d/simdisk (an ad-hoc episode, a test
        # song) can never be indexed or counted. The disk equals install
        # + cache, exactly as in the built path below.
        if [ $attempt = 0 ] && [ -f "$simcache/.ready" ] \
           && rm -rf "$d/simdisk" \
           && make -C "$d" install >"$GATE_TMP/install-$base.log" 2>&1 \
           && simdisk_cache_restore "$d/simdisk" "$simcache"; then
            origin="simdisk cache hit $simkey"
        else
            origin=built
            [ $attempt -gt 0 ] && rm -rf "$simcache"
            # A built entry is cached for every worktree, so it must come
            # from a clean disk, never from whatever earlier ad-hoc runs
            # left on this shared one (fixture dirs such as NPArt in the
            # database, a half-filled art cache, settings, resume state):
            # wipe it and reinstall before generating.
            rm -rf "$d/simdisk"
            if ! make -C "$d" install >"$GATE_TMP/install-$base.log" 2>&1; then
                bad "make install $d"; cat "$GATE_TMP/install-$base.log"
                broken=1; break
            fi
            prime_build "$d" "$base" "$wm" "$wl" "$wv" "$wlib"
        fi
        [ $broken = 1 ] && break
        if [ $wl = 1 ] && ! prime_verify "$d" "$base"; then
            echo "note prime $d: database from [$origin] not usable; rebuilding"
            attempt=$((attempt + 1))
            if [ $attempt -ge 2 ]; then
                bad "prime $d: database not usable even after a rebuild"
                broken=1; break
            fi
            continue
        fi
        [ "$origin" = built ] && simdisk_cache_save "$d/simdisk" "$simcache" 2>/dev/null
        break
    done
    if [ $broken = 1 ]; then
        touch "$GATE_TMP/broken-$base"
    else
        echo "ok   prime $d (music=$wm library=$wl) [$origin; verified]"
    fi
done

step "ui scripts: cloning per-worker disks"
GATE_JOBS=${GATE_JOBS:-$(simpool_jobs)}   # min(16, 2*ncpu, mem GiB)
[ "$GATE_JOBS" -ge 1 ] 2>/dev/null || GATE_JOBS=1
echo "GATE_JOBS=$GATE_JOBS (ncpu=$NCPU)"
for d in $dirs_in_use; do
    base=$(basename "$d")
    [ -f "$GATE_TMP/broken-$base" ] && continue
    for w in $(seq 1 "$GATE_JOBS"); do
        clone_disk "$d/simdisk" "$d/simdisk.w$w"
    done
done

# run_script_job <name> <build-dir> <simdisk>: runs one tests/ui script
# (against its own simdisk, worker or retry) and writes
# build/.gate/out-<name> (the exact text validate.sh used to print
# inline) and build/.gate/status-<name> (0 ok, 1 fail).
run_script_job() {
    local n=$1 b=$2 simdisk=$3 jfail=0
    rm -f "$GATE_TMP/fails-$n.tsv"
    local s=tests/ui/$n.script
    {
        if out=$(SIM_BUILD="$b" SIMDISK="$simdisk" scripts/shots.sh "$s" 2>&1); then
            cnt=$(echo "$out" | grep -c '\.png$')
            echo "ok   $n ($cnt shots, $b)"
            case "$n" in
                ipodui-*)
                    log=out/shots/$n/sim.log
                    if [ -f "$log" ]; then
                        if tr=$(scripts/check_trace.py "$log" "$s" \
                                    --rows "$LIST_ROWS" --header-h "$HEADER_H"); then
                            echo "$tr" | sed "s/\$/ [$n]/"
                        else
                            echo "$tr" | sed "s/\$/ [$n]/"
                            jfail=1
                        fi
                    fi
                    skips=$(sed -n 's/^#[[:space:]]*golden:[[:space:]]*skip[[:space:]]*//p' "$s")
                    pairs="$GATE_TMP/pairs-$n"
                    : >"$pairs"
                    any=0
                    for g in tests/ui/golden/"$n"/*.png; do
                        [ -f "$g" ] || continue
                        gname=$(basename "$g" .png)
                        shotf=out/shots/$n/$(basename "$g")
                        if echo "$skips" | tr ' ' '\n' | grep -qx "$gname"; then
                            echo "skip golden $n/$gname (golden: skip)"
                        elif [ ! -f "$shotf" ]; then
                            echo "FAIL: golden $n/$(basename "$g"): shot not taken"
                            jfail=1
                        else
                            regs=$(sed -n "s/^#[[:space:]]*golden-region:[[:space:]]*$gname[[:space:]]\{1,\}\([0-9]*\)[[:space:]]\{1,\}\([0-9]*\)[[:space:]]\{1,\}\([0-9]*\)[[:space:]]\{1,\}\([0-9]*\).*/\1,\2,\3,\4/p" "$s")
                            if [ -z "$regs" ]; then
                                printf 'golden %s/%s\t%s\t%s\n' "$n" "$gname" "$shotf" "$g" >>"$pairs"
                            else
                                for reg in $regs; do
                                    printf 'golden %s/%s [%s]\t%s\t%s\t%s\n' "$n" "$gname" "$reg" "$shotf" "$g" "$reg" >>"$pairs"
                                done
                            fi
                            any=1
                        fi
                    done
                    if [ $any = 1 ]; then
                        if ! gout=$(scripts/pngdiff.py --batch "$pairs" \
                                         --threshold "$GOLDEN_THRESHOLD"); then
                            jfail=1
                            # keep the failing pairs for the failure report
                            printf '%s\n' "$gout" | sed -n 's/^FAIL: \(.*\): [0-9.]*% pixels.*/\1/p; s/^FAIL: \(.*\): size .*/\1/p' \
                                >"$GATE_TMP/faillabels-$n"
                            awk -F'\t' 'NR==FNR{l[$1]=1;next} ($1 in l)' \
                                "$GATE_TMP/faillabels-$n" "$pairs" >"$GATE_TMP/fails-$n.tsv"
                        fi
                        echo "$gout"
                    fi
                    rm -f "$pairs"
                    # Phase 8a shot relations (tests/ui/README.md):
                    #   "# same: <shotA> <shotB>"  pixel-identical (before a
                    #       USB session / face / devmode and after it)
                    #   "# ref: <shot> <repo-path>"  compare with a
                    #       reference image at RGB565 (pngdiff --rgb565: the
                    #       references are host renders); a report (the
                    #       deviation list needs sign-off), a FAIL only with
                    #       GATE_REF_STRICT=1
                    while read -r sa sb _rest; do
                        [ -n "$sa" ] || continue
                        fa=out/shots/$n/$sa.png; fb=out/shots/$n/$sb.png
                        if [ ! -f "$fa" ] || [ ! -f "$fb" ]; then
                            echo "FAIL: same $n: shot not taken ($sa or $sb)"; jfail=1
                        elif rel=$(scripts/pngdiff.py "$fa" "$fb" --threshold 0 2>&1); then
                            echo "ok   same $n: $sa == $sb"
                        else
                            echo "FAIL: same $n: $sa != $sb ($rel)"; jfail=1
                        fi
                    done < <(sed -n 's/^#[[:space:]]*same:[[:space:]]*//p' "$s")
                    while read -r sa rp _rest; do
                        [ -n "$sa" ] || continue
                        fa=out/shots/$n/$sa.png
                        if [ ! -f "$fa" ]; then
                            echo "FAIL: ref $n: shot $sa not taken"; jfail=1
                        elif [ ! -f "$rp" ]; then
                            echo "FAIL: ref $n: no reference file $rp"; jfail=1
                        elif rel=$(scripts/pngdiff.py "$fa" "$rp" --threshold 0 --rgb565 2>&1); then
                            echo "ok   ref $n: $sa == $rp (at RGB565)"
                        elif [ "${GATE_REF_STRICT:-0}" = 1 ]; then
                            echo "FAIL: ref $n: $sa != $rp ($rel)"; jfail=1
                        else
                            echo "note ref $n: $sa != $rp ($rel); needs sign-off (GATE_REF_STRICT=1 makes it fail)"
                        fi
                    done < <(sed -n 's/^#[[:space:]]*ref:[[:space:]]*//p' "$s")
                    ;;
            esac
        else
            echo "FAIL: ui script $n"; echo "$out" | tail -5
            jfail=1
        fi
    } >"$GATE_TMP/out-$n" 2>&1
    echo $jfail >"$GATE_TMP/status-$n"
}

# run_worker <w>: this worker's static share of job_names (index i runs
# on worker ((i-1) % GATE_JOBS) + 1), each on a fresh clone of the primed
# disk at build/<dir>/simdisk.w<w>.
run_worker() {
    local w=$1 i=0 n b base
    for n in $job_names; do
        i=$((i + 1))
        if [ $(( (i - 1) % GATE_JOBS + 1 )) -eq "$w" ]; then
            b=$(script_sim "$n")
            base=$(basename "$b")
            if [ -f "$GATE_TMP/broken-$base" ]; then
                echo "FAIL: ui script $n (skipped: priming $b failed, see 'prime $b' above)" \
                    >"$GATE_TMP/out-$n"
                echo 1 >"$GATE_TMP/status-$n"
            else
                # a fresh clone per SCRIPT, not per worker: state one
                # script leaves in the database (an audiobook's saved
                # resume offset, play counts, ratings) must not reach the
                # next one on the same worker (p5-audiobook started its
                # book where an earlier book script stopped it and ran
                # off the end mid-script, about 1 run in 3)
                clone_disk "$b/simdisk" "$b/simdisk.w$w"
                run_script_job "$n" "$b" "$b/simdisk.w$w"
            fi
        fi
    done
}

step "ui scripts: running ($(echo $job_names | wc -w | tr -d ' ') scripts, $GATE_JOBS workers)"
for w in $(seq 1 "$GATE_JOBS"); do
    run_worker "$w" &
done
wait

first_failed=""
for n in $job_names; do
    st=$(cat "$GATE_TMP/status-$n" 2>/dev/null || echo 1)
    if [ "$st" != 0 ]; then first_failed="$first_failed $n"; fi
done

step "ui scripts: flake guard"
nfailed=$(echo $first_failed | wc -w | tr -d ' ')
if [ "$nfailed" = 0 ]; then
    echo "none (every script passed in the parallel run)"
else
    echo "$nfailed script(s) failed under parallel workers; retrying each alone"
fi
real_failed=""
for n in $(printf '%s\n' $first_failed | sort); do
    [ -n "$n" ] || continue
    b=$(script_sim "$n")
    rd="$b/simdisk.retry"
    clone_disk "$b/simdisk" "$rd"
    mv -f "$GATE_TMP/out-$n" "$GATE_TMP/out-$n.first-attempt" 2>/dev/null || true
    run_script_job "$n" "$b" "$rd"
    st2=$(cat "$GATE_TMP/status-$n")
    if [ "$st2" = 0 ]; then
        echo "flaky-retry ok: $n (failed once under worker contention, passed alone)"
        flaky=$((flaky + 1))
    else
        echo "FAIL: $n failed again running alone (not flaky)"
        real_failed="$real_failed $n"
    fi
done
if [ -n "$real_failed" ]; then fail=1; fi

step "ui scripts: results"
for n in $(printf '%s\n' $job_names | sort); do
    [ -n "$n" ] || continue
    cat "$GATE_TMP/out-$n"
done
njobs=$(echo $job_names | wc -w | tr -d ' ')
nreal=$(echo $real_failed | wc -w | tr -d ' ')
echo
echo "ui scripts summary: $njobs run, $nfailed failed under parallel workers, $flaky flaky-retry ok, $nreal real FAIL"
if [ -n "$real_failed" ]; then
    echo
    echo "failure summary (one page): $(echo $real_failed | wc -w | tr -d ' ') failing script(s)"
    cat "$GATE_TMP"/fails-*.tsv >"$GATE_TMP/fails-all.tsv" 2>/dev/null
    for n in $(printf '%s\n' $real_failed | sort); do
        echo "  FAIL $n: $(grep -m1 -E '^FAIL' "$GATE_TMP/out-$n" | cut -c1-150)"
    done
    if [ -s "$GATE_TMP/fails-all.tsv" ]; then
        scripts/gate_report.py "$GATE_TMP/fails-all.tsv" --out out/gate-report \
            && echo "  [golden | actual | diff] composites: out/gate-report/ (index.md links each)"
    fi
fi

# Tidy the per-worker/retry disk clones (cheap to remake next run; the
# primed base disks under build/<dir>/simdisk are left alone).
for d in $dirs_in_use; do
    rm -rf "$d"/simdisk.w* "$d"/simdisk.retry
done

step "size"
cur=$(arm-none-eabi-size build/fw-ui/rockbox.elf | awk 'NR==2{print $1, $2, $3}')
base=$(awk 'NR==2{print $1, $2, $3}' build/fw/.size-baseline 2>/dev/null)
echo "fw-ui text/data/bss: $cur"
echo "baseline:            ${base:-none}"
ls -l build/fw-ui/rockbox.ipod 2>/dev/null | awk '{print "rockbox.ipod bytes:  " $5}'
# Phase 14a (brief 8): the video build's core image, against fw-ui's: the
# shell + classifier budget is 24 KB .text (a report; the figure to read
# is the difference)
if [ -f build/fw-ui-video/rockbox.elf ]; then
    arm-none-eabi-size build/fw-ui-video/rockbox.elf | awk 'NR==2{print "fw-ui-video text/data/bss: " $1, $2, $3}'
fi
# Phase 9 (docs/phase9-brief.md 7.2-7.4, 7.10): the image ceiling, the
# resident _end, the decoder set and the normaliser, on fw-ui (and fw-alt
# when this run built it). The ceiling and _end limits apply once the trim
# set is on: the header no longer guards it with IPODUI_TRIM_DEFAULTS (the
# Part 1 integration), or this build passed --ipodui-trim=all.
# Phase 16-A (owner, 2026-10-20; docs/hardware.md "Budget rules"): the
# binding rule is the audio-buffer floor (tests/perf/floor_gate.sh, the
# "audio buffer floor" step below). These are the backstops. Retired
# gates, kept as notes: P9_CEILING 914,813 B (stock - 20 %) and P9_END_MAX
# 1,100,000 B; P8B_T_END_MAX 1,072,000 B (8b-T interim, retired when 8b-2
# merged), P8B_END_MAX 1,096,000 B and P8B_IPOD_MAX 820,000 B (8b close).
P9_CEILING=1250000       # backstop, rockbox.ipod bytes
P9_END_MAX=1500000       # backstop, resident core (_end)
P9_DECODERS="a52 a52_rm aac aac_bsf adx aiff alac ape asap atrac3_oma atrac3_rm au ay cook flac gbs hes kss mod mpa mpc nsf opus raac sgc shorten sid smaf spc speex tta vgm vorbis vox wav wav64 wavpack wma wmapro"
p9_trims_on() { # build dir
    ! grep -q '^#ifdef IPODUI_TRIM_DEFAULTS' firmware/export/config/ipodvideo-variant.h \
        || grep -q -- '-DIPODUI_TRIM_DEFAULTS' "$1/Makefile" 2>/dev/null
}
p9_size_check() { # build dir, label
    local d=$1 l=$2 ipod end got miss extra enc
    if [ ! -f "$d/rockbox.ipod" ] || [ ! -f "$d/rockbox.elf" ]; then
        bad "p9 size: $d/rockbox.ipod or rockbox.elf missing"; return
    fi
    ipod=$(wc -c <"$d/rockbox.ipod" | tr -d ' ')
    end=$(printf '%d' "0x$(arm-none-eabi-nm "$d/rockbox.elf" | awk '$3 == "_end" {print $1}')")
    if p9_trims_on "$d"; then
        if [ "$ipod" -gt $P9_CEILING ]; then
            bad "p9 size ($l, scripts/validate.sh): rockbox.ipod $ipod B > backstop $P9_CEILING B (docs/hardware.md); scripts/sizemap.py $d/rockbox.map --base <older map> names the subsystem"
        else
            echo "ok   $l rockbox.ipod $ipod B <= backstop $P9_CEILING B"
        fi
        if [ "$end" -gt $P9_END_MAX ]; then
            bad "p9 size ($l): _end $end B > backstop $P9_END_MAX B (docs/hardware.md)"
        else
            echo "ok   $l _end $end B <= backstop $P9_END_MAX B"
        fi
    else
        echo "note $l rockbox.ipod $ipod B, _end $end B (trim set off: ceiling $P9_CEILING / _end $P9_END_MAX not enforced)"
    fi
    # decoders: the same 39 always; encoders only while RECORDING is back
    got=$(find "$d/lib/rbcodec/codecs" -name '*.codec' 2>/dev/null | sed 's|.*/||; s|\.codec$||' | sort)
    [ -n "$got" ] || return 0      # fw-alt: compile-only (make bin), no codecs
    miss=$(for c in $P9_DECODERS; do echo "$got" | grep -qx "$c" || printf '%s ' "$c"; done)
    enc=$(echo "$got" | grep '_enc$' | tr '\n' ' ')
    extra=$(echo "$got" | grep -v '_enc$' | grep -vxF "$(printf '%s\n' $P9_DECODERS)" | tr '\n' ' ')
    if [ -n "$miss$extra" ]; then
        bad "p9 decoders ($l): missing [${miss% }] unexpected [${extra% }] (criterion 4: the same 39)"
    elif p9_trims_on "$d" && [ -n "$enc" ]; then
        bad "p9 decoders ($l): encoders still built with RECORDING trimmed: ${enc% }"
    else
        echo "ok   $l decoders: 39$([ -n "$enc" ] && echo ", encoders ${enc% }")"
    fi
}
p9_size_check build/fw-ui fw-ui
# Phase 8b size gates (fw-ui) and the cold-Thumb list they rely on (brief
# 6.13: printed so a hot file can't join it silently)
p8b_size_check() {
    local d=build/fw-ui ipod end
    [ -f "$d/rockbox.ipod" ] && [ -f "$d/rockbox.elf" ] || return 0
    ipod=$(wc -c <"$d/rockbox.ipod" | tr -d ' ')
    end=$(printf '%d' "0x$(arm-none-eabi-nm "$d/rockbox.elf" | awk '$3 == "_end" {print $1}')")
    # P8B_T_END_MAX held only until 8b-2 merged (brief 4.T); since the
    # 2026-10-20 integration it is informational, the 8b close gate rules
    # the page-aligned inquiry_static: bss before it may grow by this gap
    # without moving _end; one byte more moves it (and everything after it)
    # by 4 KiB (a note; 8b close gap measured at 0x1e0 = 480 B)
    local iq pe
    iq=$(arm-none-eabi-nm -S -n "$d/rockbox.elf" | awk '
        $4 == "inquiry_static" { print "0x" $1, pa, pz; exit }
        NF == 4 && $3 ~ /^[bB]$/ { pa = "0x" $1; pz = "0x" $2 }')
    if [ -n "$iq" ]; then
        set -- $iq
        pe=$(( $2 + $3 ))
        echo "note fw-ui inquiry_static at $(printf '0x%x' "$1"): $(( $1 - pe )) B of bss before it free to the 4 KiB boundary (previous bss ends at $(printf '0x%x' "$pe")); _end $end B"
    else
        echo "note fw-ui: inquiry_static not found in rockbox.elf (gap not reported)"
    fi
    echo "note fw-ui _end $end B, rockbox.ipod $ipod B (8b gates P8B_* retired in 16-A: backstops above, floor below)"
    if grep -q '^export THUMB_COLD_LIST=.' "$d/Makefile" 2>/dev/null; then
        # every listed file exists and its object, if it has code at all
        # (eq_hw_presets is data only), holds Thumb code (a Thumb function
        # symbol has its low bit set)
        local f o
        for f in $(sed -e 's/#.*//' tools/thumb-cold.txt); do
            o="$d/${f%.c}.o"
            if [ ! -f "$f" ]; then
                bad "p8b thumb-cold: $f (tools/thumb-cold.txt) does not exist"
            elif [ ! -f "$o" ] && grep -qE "^$f[[:space:]]+#.*optional" tools/thumb-cold.txt; then
                :   # conditionally compiled ("# optional"), not in this build
            elif [ ! -f "$o" ]; then
                bad "p8b thumb-cold: $o not built"
            elif arm-none-eabi-readelf -sW "$o" | grep -q ' FUNC ' \
                && ! arm-none-eabi-readelf -sW "$o" | awk '$4 == "FUNC" {print $2}' \
                    | grep -q '[13579bdf]$'; then
                bad "p8b thumb-cold: $o has no Thumb code"
            fi
        done
        echo "thumb-cold (tools/thumb-cold.txt): $(sed -e 's/#.*//' tools/thumb-cold.txt | grep . | sed 's|^apps/||; s|\.c$||' | tr '\n' ' ')"
    else
        echo "note fw-ui built with --no-thumb-cold"
    fi
}
p8b_size_check
# the split shadow and the panel rectangle (8b close, brief 7): xfade_rect()
# and FBADDR(UI_PANEL_X ...) are called only inside the panel compose
# contract (apps/ipodui/preview.c) so no writer of the panel can skip the
# shadow or apply it twice: xfade_rect itself and ui_panel_fade; lcd_panel
# (the contract's accessor), half_window (the drift half step, which
# shadows what it wrote) and ps_step (the panel slide, which moves
# already-shadowed rows). Comments don't count.
p8b_panel_contract() {
    local hits
    hits=$(awk '
        FNR == 1 { f = "" }
        /^[a-zA-Z_][^=;(]*\(/ { f = $0; sub(/\(.*/, "", f); sub(/.* /, "", f); sub(/^\*+/, "", f) }
        /^[ \t]*(\/?\*|\/\/)/ { next }
        /xfade_rect\(|FBADDR\(UI_PANEL_X/ {
            ok = 0
            if (FILENAME ~ /apps\/ipodui\/preview\.c$/ &&
                (f == "xfade_rect" || f == "ui_panel_fade" || f == "lcd_panel" ||
                 f == "half_window" || f == "ps_step")) ok = 1
            if (!ok) print FILENAME ":" FNR " (in " f "): " $0
        }' apps/ipodui/*.c apps/ipodui/*.h apps/ipodui/*/*.c 2>/dev/null)
    if [ -n "$hits" ]; then
        printf '%s\n' "$hits" | sed 's/^/      /'
        bad "p8b panel contract (scripts/validate.sh): xfade_rect() / FBADDR(UI_PANEL_X ...) outside ui_panel_compose / ui_panel_fade and helpers (apps/ipodui/preview.c); route the write through them so the split shadow is applied exactly once"
    else
        echo "ok   panel contract: xfade_rect / FBADDR(UI_PANEL_X only in the panel compose functions"
    fi
}
p8b_panel_contract
# the normaliser stays wired (criterion 10): utf8_normalize is linked and
# metadata.c calls it, whether utf8proc or nfc.c provides it
if p9_norm=$(python3 -I scripts/risk/reach.py build/fw-ui/rockbox.elf \
        --callers utf8_normalize 2>"$GATE_TMP/reach-norm.err"); then
    if echo "$p9_norm" | grep -q '^caller utf8_normalize .*metadata\.o'; then
        echo "ok   utf8_normalize called from metadata.o (reach.py)"
    else
        bad "p9 normalise (reach.py --callers utf8_normalize): no live caller in metadata.o; HAVE_UTF8_NORMALIZE lost in fw-ui?"
    fi
    echo "reach: $(echo "$p9_norm" | grep '^summary')"
else
    cat "$GATE_TMP/reach-norm.err"; bad "p9: scripts/risk/reach.py failed on build/fw-ui/rockbox.elf"
fi
python3 -I scripts/sizemap.py build/fw-ui/rockbox.map --top 8 2>&1 | sed 's/^/sizemap: /'
if [ -f build/fw-alt/rockbox.ipod ] && [ "$QUICK" = 0 ]; then
    p9_size_check build/fw-alt fw-alt
fi

step "stock bootloader identity (Phase 8a)"
# The stock bootloader must stay byte-for-byte what main builds: P's
# bootloader/ and firmware/ edits are all guarded by HAVE_IPOD_BOOTSCREEN
# (set only by --ipodui), so build/boot's .text and .data must equal
# build/boot/.size-baseline, recorded from a fresh stock bootloader built
# on main:   arm-none-eabi-size build/boot/bootloader.elf \
#              > build/boot/.size-baseline
# Checked only when this run built build/boot (the full gate, or --quick
# with bootloader/ or firmware/ changed). No baseline: a note, no pass.
case " $BUILD_DIRS " in
    *" boot "*)
        bbase=$(awk 'NR==2{print $1, $2}' build/boot/.size-baseline 2>/dev/null)
        bcur=$(arm-none-eabi-size build/boot/bootloader.elf 2>/dev/null | awk 'NR==2{print $1, $2}')
        if [ -z "$bcur" ]; then
            bad "stock boot identity: build/boot/bootloader.elf missing"
        elif [ -z "$bbase" ]; then
            echo "note no build/boot/.size-baseline: record one from a main-built stock bootloader (see scripts/validate.sh)"
        elif [ "$bcur" = "$bbase" ]; then
            echo "ok   stock boot text/data identical to baseline: $bcur"
        else
            echo "stock boot text/data $bcur != baseline $bbase"
            bad "stock bootloader changed (text/data differ from build/boot/.size-baseline); a Phase 8a edit leaked out of HAVE_IPOD_BOOTSCREEN"
        fi ;;
    *) echo "skip stock boot identity (build/boot not built in this run)" ;;
esac
case " $BUILD_DIRS " in
    *" boot "*)
        # bytes, not sizes (fix round, finding 9): bootloader.bin built with
        # the pinned VERSION must hash to main's (scripts/stock_boot.sha256)
        bhash_base=$(awk '!/^#/ && NF {print $1; exit}' scripts/stock_boot.sha256 2>/dev/null)
        bhash=$(shasum -a 256 build/boot/bootloader.bin 2>/dev/null | awk '{print $1}')
        if [ -z "$bhash" ]; then
            bad "stock boot bytes: build/boot/bootloader.bin missing"
        elif [ -z "$bhash_base" ]; then
            bad "stock boot bytes: scripts/stock_boot.sha256 missing"
        elif [ "$bhash" = "$bhash_base" ]; then
            echo "ok   stock bootloader.bin byte-identical to main (sha256 ${bhash%"${bhash#????????????}"})"
        else
            echo "stock bootloader.bin sha256 $bhash != main's $bhash_base"
            bad "stock bootloader bytes changed (scripts/stock_boot.sha256); a Phase 8a edit leaked out of HAVE_IPOD_BOOTSCREEN"
        fi ;;
esac
case " $BUILD_DIRS " in
    *" boot-ui "*)
        echo "boot-ui text/data/bss: $(arm-none-eabi-size build/boot-ui/bootloader.elf 2>/dev/null | awk 'NR==2{print $1, $2, $3}')" ;;
esac

step "sealed release build (--ipodui-release, Phase 8a fix round)"
# D7: the sealed build compiles developer mode out, so no "developer"
# string may remain in the firmware image or in its built language files
# (Rev 1 L1: no stock phrase contains the word). Full gate only.
case " $BUILD_DIRS " in
    *" fw-rel "*)
        if [ ! -f build/fw-rel/rockbox.ipod ]; then
            bad "sealed release: build/fw-rel/rockbox.ipod missing"
        else
            ndev=$(strings build/fw-rel/rockbox.ipod | grep -ci developer || true)
            for l in build/fw-rel/apps/lang/*.lng; do
                [ -f "$l" ] || continue
                ndev=$(( ndev + $(strings "$l" | grep -ci developer || true) ))
            done
            echo "fw-rel text/data/bss: $(arm-none-eabi-size build/fw-rel/rockbox.elf 2>/dev/null | awk 'NR==2{print $1, $2, $3}')"
            if [ "$ndev" = 0 ]; then
                echo "ok   sealed release: 0 \"developer\" strings"
            else
                strings build/fw-rel/rockbox.ipod | grep -i developer | head -5
                bad "sealed release: $ndev \"developer\" string(s) in build/fw-rel"
            fi
        fi ;;
    *) echo "skip sealed release (full gate only)" ;;
esac

step "native illusion (user-visible strings in shell)"
if [ -d apps/ipodui ] && grep -rnI --include='*.c' -iE '"[^"]*rockbox[^"]*"' apps/ipodui; then
    bad "Rockbox string in apps/ipodui"
else
    echo "none"
fi

step "UBSan risk pass (#38, docs/phase-nav-brief.md Revision 1)"
# --full: every ipodui-* script, in parallel workers on cloned disks.
# --quick --changed (narrowed by gate_map.txt): only the scripts mapped
# to the changed files, the same list the UI pass just ran. Plain --quick,
# or --quick --changed that fell back to "every script": skipped as before.
# GATE_UBSAN=0 skips it in any mode.
ub_list=""; ub_run=1
if [ "${GATE_UBSAN:-1}" = 0 ]; then
    ub_run=0; echo "note skipped (GATE_UBSAN=0)"
elif [ $QUICK = 1 ]; then
    if [ $CHANGED = 1 ] && [ $CHANGED_ALL = 0 ]; then
        for n in $job_names; do
            # (not ipodui-p14a-*: sim-ui-ubsan has no video player)
            case "$n" in ipodui-p14a-*) ;; ipodui-*) ub_list="$ub_list $n" ;; esac
        done
        [ -n "$ub_list" ] || { ub_run=0; echo "note no mapped ipodui-* script; skipped"; }
    else
        ub_run=0
        echo "note skipped under --quick without a narrowed --changed (its own sim-ui-ubsan build; full gate runs it)"
    fi
fi
if [ $ub_run = 1 ] && [ -x scripts/risk/sim_ubsan.sh ]; then
    ub_out=$(UBSAN_LIST="$ub_list" UBSAN_JOBS=${UBSAN_JOBS:-$GATE_JOBS} scripts/risk/sim_ubsan.sh 2>&1)
    ub_n=$(echo "$ub_out" | grep -c '^FINDING:')
    # the full list is kept (#38 fix round); the log shows its tail
    mkdir -p build/risk/ubsan
    printf '%s\n' "$ub_out" > build/risk/ubsan/gate-findings.txt
    echo "$ub_out" | tail -20
    echo "note full UBSan output: build/risk/ubsan/gate-findings.txt"
    if [ "$ub_n" -gt 0 ]; then
        echo "note $ub_n finding(s) (scripts/risk/sim_ubsan.sh is a report, not a gate: see the full output above; does not fail this gate)"
    else
        echo "ok   no findings"
    fi
elif [ $ub_run = 1 ]; then
    echo "note scripts/risk/sim_ubsan.sh missing or not executable, skipped"
fi

step "audio buffer floor (16-A, #105; docs/hardware.md budget rules)"
# The binding rule: at 50k tracks the playing/paused audio file buffer stays
# >= 7,500,000 B (32 MB unit) and >= 39,000,000 B (64 MB unit) at every
# worst case (tests/perf/floor-50k.script, receipts "floor ..." in
# out/shots/floor-{32,64}/sim.log). --quick --changed runs it only when
# apps/, firmware/, lib/ or uisimulator/ changed; FLOOR_GATE=0 skips it.
# The first run builds the 50k library (tests/perf/lib50k.sh; minutes).
floor_run=1
if [ "${FLOOR_GATE:-1}" = 0 ]; then
    floor_run=0; echo "note floor gate skipped (FLOOR_GATE=0)"
elif [ $QUICK = 1 ] && [ $CHANGED = 1 ] && [ $CHANGED_ALL = 0 ] \
        && ! printf '%s\n' "${CHANGED_FILES:-}" | grep -qE '^(apps|firmware|lib|uisimulator|tests/perf)/'; then
    floor_run=0; echo "note --changed: no apps/, firmware/, lib/, uisimulator/ or tests/perf change; floor gate skipped"
fi
if [ $floor_run = 1 ]; then
    if [ ! -f build/sim-ui32/Makefile ]; then
        mkdir -p build/sim-ui32
        (cd build/sim-ui32 && ../../tools/configure --target=22 --type=s --ipodui --ram=32 </dev/null >configure.log 2>&1) \
            || bad "configure build/sim-ui32 (32 MB floor sim)"
    fi
    if [ -f build/sim-ui32/Makefile ] && [ -x build/sim-ui/rockboxui ]; then
        make -C build/sim-ui32 -j"$JOBS" >build/sim-ui32/make.log 2>&1 \
            || bad "build build/sim-ui32 (32 MB floor sim): see build/sim-ui32/make.log"
        [ -d build/sim-ui/simdisk/.rockbox/codecs ] || make -C build/sim-ui install >/dev/null 2>&1
        if tests/perf/floor_gate.sh 32 64 >build/floor-gate.log 2>&1; then
            grep -E "^floor gate: ok" build/floor-gate.log | sed 's/^/ok   /'
            grep -E "^floor_watch all_cases" build/floor-gate.log | sed 's/^/     /'
        else
            grep -E "expect_floor|floor gate" build/floor-gate.log
            bad "audio buffer floor (tests/perf/floor_gate.sh, build/floor-gate.log)"
        fi
    else
        bad "audio buffer floor: build/sim-ui or build/sim-ui32 missing"
    fi
fi

step "upstream dry-run"
if git fetch -q upstream 2>/dev/null; then
    behind=$(git rev-list --count HEAD..upstream/master)
    if out=$(git merge-tree --write-tree --name-only HEAD upstream/master 2>&1); then
        echo "ok   upstream: $behind new commit(s), merges cleanly"
    else
        echo "note upstream: $behind new commit(s), CONFLICTS in:"
        echo "$out" | sed -n '2,/^$/p' | sed 's/^/       /'
    fi
else
    echo "note upstream: fetch failed (offline?), skipped"
fi

echo
if [ $fail = 0 ]; then echo "GATE PASSED"; else echo "GATE FAILED"; exit 1; fi
