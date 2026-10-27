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

# Phase 9 stock identity (docs/phase9-brief.md checklist 11, 7.7, Rev 1 M6).
#
# A .size-baseline match compares sizes, not bytes. This builds the stock
# firmware and the stock bootloader twice, from the merge-base with main and
# from HEAD, in clean build dirs with the same pinned VERSION= (so the
# version string cannot differ), and compares the SHA-256 of every loaded
# section (objcopy -O binary -j <section>) plus the whole objcopy -O binary
# image and the scrambled .ipod file. Every hash must match.
#
#   scripts/risk/stock_identity.sh [--base REF] [--head REF|--worktree]
#                                  [--what fw,boot] [--keep] [-j N]
#
#   --base REF   default: git merge-base HEAD main
#   --head REF   default: HEAD (both trees come from git archive, so
#                uncommitted edits never leak into the proof)
#   --worktree   use the working tree (incl. uncommitted edits) as "head"
#   --what LIST  fw, boot or both (default fw,boot)
#   --keep       keep the scratch trees and build dirs
#   -j N         make jobs per build (default 2; one build at a time)
#
# Output: one line per section, then
#   stock_identity PASS fw=<sha> boot=<sha>     exit 0
#   stock_identity FAIL <what> <section>...     exit 1
# Build dirs live under build/stock-identity/ (gitignored), logs there too.
set -u
cd "$(dirname "$0")/../.."
ROOT=$(pwd)

BASE=
HEAD_REF=HEAD
USE_WT=0
WHAT=fw,boot
KEEP=0
JOBS=2
VERSION_PIN=p9-stock-identity
while [ $# -gt 0 ]; do
    case "$1" in
    --base) BASE=$2; shift 2 ;;
    --head) HEAD_REF=$2; shift 2 ;;
    --worktree) USE_WT=1; shift ;;
    --what) WHAT=$2; shift 2 ;;
    --keep) KEEP=1; shift ;;
    -j) JOBS=$2; shift 2 ;;
    -j*) JOBS=${1#-j}; shift ;;
    -h|--help) awk 'NR==1{next} /^set -u/{print; exit} /^#\*+$/{b=!b; next} !b' "$0" | sed 's/^# \{0,1\}//'; exit 0 ;;
    *) echo "unknown option $1" >&2; exit 2 ;;
    esac
done
[ -n "$BASE" ] || BASE=$(git merge-base HEAD main) || exit 2
BASE=$(git rev-parse "$BASE") || exit 2
W=$ROOT/build/stock-identity
rm -rf "$W"
mkdir -p "$W"

tree_for() { # name ref -> prints the source dir
    if [ "$1" = head ] && [ "$USE_WT" = 1 ]; then
        echo "$ROOT"; return
    fi
    mkdir -p "$W/src-$1"
    git archive "$2" | tar -x -C "$W/src-$1" || return 1
    echo "$W/src-$1"
}

build() { # src kind(fw|boot) name
    local src=$1 kind=$2 dir=$W/$3-$2 t
    case "$kind" in fw) t=n ;; boot) t=b ;; esac
    mkdir -p "$dir"
    (cd "$dir" && "$src/tools/configure" --target=22 --type=$t \
        --compiler-prefix=arm-none-eabi- </dev/null > configure.log 2>&1) \
        || { echo "configure failed: $dir" >&2; return 1; }
    if [ "$kind" = fw ]; then
        make -C "$dir" -j"$JOBS" VERSION=$VERSION_PIN bin > "$dir/make.log" 2>&1
    else
        make -C "$dir" -j"$JOBS" VERSION=$VERSION_PIN > "$dir/make.log" 2>&1
    fi || { tail -20 "$dir/make.log" >&2; echo "build failed: $dir" >&2; return 1; }
}

hashes() { # build dir -> "section sha" lines
    local dir=$1 elf img s
    elf=$(ls "$dir"/rockbox.elf "$dir"/bootloader.elf 2>/dev/null | head -1)
    [ -n "$elf" ] || { echo "no elf in $dir" >&2; return 1; }
    for s in .vectors .init.text .text .rodata .data .ncdata .init .iram; do
        if arm-none-eabi-objdump -h "$elf" | awk '{print $2}' | grep -qx -- "$s"; then
            arm-none-eabi-objcopy -O binary -j "$s" "$elf" "$dir/sec$s.bin"
            echo "$s $(shasum -a 256 "$dir/sec$s.bin" | cut -d' ' -f1)"
        fi
    done
    arm-none-eabi-objcopy -O binary "$elf" "$dir/image.bin"
    echo "image $(shasum -a 256 "$dir/image.bin" | cut -d' ' -f1)"
    img=$(ls "$dir"/*.ipod 2>/dev/null | head -1)
    [ -n "$img" ] && echo "ipod $(shasum -a 256 "$img" | cut -d' ' -f1)"
    return 0
}

echo "stock_identity: base $BASE vs head $([ $USE_WT = 1 ] && echo worktree || git rev-parse "$HEAD_REF"), VERSION=$VERSION_PIN"
SB=$(tree_for base "$BASE") || exit 2
SH=$(tree_for head "$HEAD_REF") || exit 2
fail=
summary=
for kind in $(echo "$WHAT" | tr ',' ' '); do
    build "$SB" "$kind" base || exit 2
    build "$SH" "$kind" head || exit 2
    hashes "$W/base-$kind" > "$W/base-$kind.sha" || exit 2
    hashes "$W/head-$kind" > "$W/head-$kind.sha" || exit 2
    while read -r sec sha; do
        hsha=$(awk -v s="$sec" '$1 == s {print $2}' "$W/head-$kind.sha")
        if [ "$sha" = "$hsha" ]; then
            echo "  $kind $sec $sha same"
        else
            echo "  $kind $sec base=$sha head=${hsha:-missing} DIFFER"
            fail="$fail $kind:$sec"
        fi
    done < "$W/base-$kind.sha"
    # a section only HEAD has is a difference too
    while read -r sec sha; do
        grep -q "^$sec " "$W/base-$kind.sha" || { echo "  $kind $sec only in head"; fail="$fail $kind:$sec"; }
    done < "$W/head-$kind.sha"
    summary="$summary $kind=$(awk '$1 == "image" {print substr($2, 1, 16)}' "$W/head-$kind.sha")"
done
if [ "$KEEP" != 1 ]; then
    rm -rf "$W/src-base" "$W/src-head"
fi
if [ -n "$fail" ]; then
    echo "stock_identity FAIL$fail"
    exit 1
fi
echo "stock_identity PASS$summary"
