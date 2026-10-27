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

# tests/fatcrash: crash-consistency model of firmware/common/fat.c.
# Builds the firmware's fat.c + disk_cache.c for the host, records each
# scenario's sector writes, then runs fsck_msdos -n on the image a halt
# after every k-th write leaves (and again after the next boot's first
# allocation). Usage: run.sh [scenario...]   (default: all of them)
# Output: one line per (scenario, crash point) with fsck's findings, and a
# tally. Needs macOS fsck_msdos. No device is touched.
set -e
here=$(cd "$(dirname "$0")" && pwd)
top=$(cd "$here/../.." && pwd)
out=${FATCRASH_OUT:-$top/build/fatcrash}
mkdir -p "$out/inc"
for d in firmware/export firmware/include firmware/kernel/include; do
    cp "$top/$d"/*.h "$out/inc/"
done
cp "$here"/shim/*.h "$out/inc/"
CFLAGS="-std=gnu99 -O1 -w -I$out/inc ${FATCRASH_CFLAGS:-}"
fs="$top/firmware/common"
# the renames through file.c (rename intent record) and "post" running
# the real boot recovery; fatcrash_nointent: the same without the record
# (the negative control of the next-boot column)
fl="$fs/file.c $fs/file_internal.c $fs/fileobj_mgr.c $fs/dir.c \
    $fs/pathfuncs.c $fs/rb_namespace.c"
# The one-sector records (.rename_intent, bootbatt.c's .bootbatt) on the
# 5.5G's geometry (2048-byte virtual sectors over 512-byte storage, as
# ipodvideo.h): each must be made a whole record_io() sector long, and a
# 0-byte one (what record_create() left on the device before 2026-10-23)
# made good. The old record_create() must fail it (negative control).
# fsreplay also replays a simulator session's file operations
# (IPODUI_FSTRACE) on this geometry; see fsreplay.c.
# shellcheck disable=SC2086
cc $CFLAGS -DMAX_VIRT_SECTOR_SIZE=2048 -DDEFAULT_VIRT_SECTOR_SIZE=2048 \
   -o "$out/fsreplay" "$here/fsreplay.c" "$fs/fat.c" "$fs/disk_cache.c" \
   "$fs/linked_list.c" $fl
"$out/fsreplay" records 32 2048 >"$out/records.txt" 2>&1 || {
    echo "fsreplay records FAILED"; cat "$out/records.txt"; exit 1; }
echo "fsreplay records (2048-byte sectors): $(tr '\n' ' ' <"$out/records.txt")"
if [ -z "$FATCRASH_NO_CONTROLS" ] && git -C "$top" cat-file -e 9507ca6c6e 2>/dev/null; then
    git -C "$top" show 9507ca6c6e:firmware/common/file.c >"$out/file_0b.c"
    # shellcheck disable=SC2086
    cc $CFLAGS -I"$fs" -DMAX_VIRT_SECTOR_SIZE=2048 \
       -DDEFAULT_VIRT_SECTOR_SIZE=2048 -o "$out/fsreplay_0b" \
       "$here/fsreplay.c" "$out/file_0b.c" "$fs/fat.c" "$fs/disk_cache.c" \
       "$fs/linked_list.c" $(echo $fl | sed "s#$fs/file.c##")
    if "$out/fsreplay_0b" records 32 2048 >/dev/null 2>&1; then
        echo "fsreplay negative control (0-byte records) passed: no teeth"
        exit 1
    fi
    echo "fsreplay negative control (9507ca6c6e record_create) caught"
fi
# a close whose short entry was emptied under it: an error, no panic
# (the 2026-10-23 freeze); 9507ca6c6e's fat.c must panic on it (control)
"$out/fsreplay" emptyslot 32 2048 >"$out/emptyslot.txt" 2>&1 || {
    echo "fsreplay emptyslot FAILED"; cat "$out/emptyslot.txt"; exit 1; }
echo "fsreplay $(cat "$out/emptyslot.txt")"
if [ -z "$FATCRASH_NO_CONTROLS" ] && git -C "$top" cat-file -e 9507ca6c6e 2>/dev/null; then
    git -C "$top" show 9507ca6c6e:firmware/common/fat.c >"$out/fat_0b.c"
    # shellcheck disable=SC2086
    cc $CFLAGS -I"$fs" -DMAX_VIRT_SECTOR_SIZE=2048 \
       -DDEFAULT_VIRT_SECTOR_SIZE=2048 -DFSREPLAY_NO_SLOT_FAULT \
       -o "$out/fsreplay_0bfat" "$here/fsreplay.c" "$out/fat_0b.c" \
       "$fs/disk_cache.c" "$fs/linked_list.c" $fl
    "$out/fsreplay_0bfat" emptyslot 32 2048 >/dev/null 2>&1 && rc=0 || rc=$?
    [ "$rc" = 3 ] || { echo "emptyslot control: no panic (rc $rc)"; exit 1; }
    echo "fsreplay negative control (9507ca6c6e fat.c panics) caught"
fi

# shellcheck disable=SC2086
cc $CFLAGS -DFATCRASH_FILE_LAYER -o "$out/fatcrash" "$here/fatcrash.c" \
   "$fs/fat.c" "$fs/disk_cache.c" "$fs/linked_list.c" $fl
# shellcheck disable=SC2086
cc $CFLAGS -DFATCRASH_FILE_LAYER -DRBFS_NO_RENAME_INTENT \
   -o "$out/fatcrash_nointent" "$here/fatcrash.c" \
   "$fs/fat.c" "$fs/disk_cache.c" "$fs/linked_list.c" $fl

# the file layer over the same FAT code: publish without renaming onto an
# existing name, one entry per name, open(missing) -> ENOENT; file_internal.c
# reaches fat_remove() through renametest.c's fault hook (case "victimfail")
cc $CFLAGS -Dfat_remove=rt_fat_remove -c -o "$out/file_internal_rt.o" \
   "$fs/file_internal.c"
cc $CFLAGS -o "$out/renametest" "$here/renametest.c" "$fs/file.c" \
   "$out/file_internal_rt.o" "$fs/fileobj_mgr.c" "$fs/dir.c" "$fs/pathfuncs.c" \
   "$fs/rb_namespace.c" "$fs/fat.c" "$fs/disk_cache.c" "$fs/linked_list.c"
for c in errno plain reader "fuzz 1" "fuzz 2" victimfail; do
    "$out/fatcrash" mkfs "$out/rt.img" 167936
    # shellcheck disable=SC2086
    if ! "$out/renametest" "$out/rt.img" $c >"$out/rt.log" 2>&1; then
        echo "renametest $c FAILED"; cat "$out/rt.log"; exit 1
    fi
    if /sbin/fsck_msdos -n "$out/rt.img" 2>&1 | grep -v ' files, ' \
            | grep -q Warning; then
        echo "renametest $c: fsck_msdos found damage"; exit 1
    fi
    echo "renametest $c ok"
done
[ -n "$FATCRASH_RENAME_ONLY" ] && exit 0

# #91 safe replaces: apps/ipodui/safefile.c and every site's sequence over
# the file layer; a power cut after every sector write and a failing
# victim removal at every remove, then the boot (file.c's rename intent
# record resolved), the site's boot recovery and a chain check of the raw
# image (strict: any failure fails); sites with a next save run it and
# its next boot too. Seeds vary the disk cache's slot order. Negative
# controls must fail: fat.c without the ordered directory entries (H2),
# recovery without the alias check and without the intent record (H1),
# the plain-rename sites without the intent record, and fat_closewrite()
# freeing an emptied file's cluster before its entry drops it (#91 M1,
# FAT_FREE_BEFORE_ENTRY: trunc-empty and otrunc-temp must fail the chain
# check). The safety net alone
# (the record loaded, never resolved) must keep every chain whole.
sfcore="$out/file_internal_rt.o $fs/fileobj_mgr.c $fs/dir.c \
    $fs/pathfuncs.c $fs/rb_namespace.c $fs/disk_cache.c $fs/linked_list.c"
# the settings files' save and recovery (sites cfg-*)
sfbase="$sfcore $top/apps/ipodui/cfgsafe.c"
sfsrc="$fs/file.c $sfbase"
cc $CFLAGS -DRBFS_NO_RENAME_INTENT -c -o "$out/file_nointent.o" "$fs/file.c"
cc $CFLAGS -DRBFS_INTENT_NO_RESOLVE -c -o "$out/file_netonly.o" "$fs/file.c"
# shellcheck disable=SC2086
cc $CFLAGS -o "$out/safetest" "$here/safetest.c" "$top/apps/ipodui/safefile.c" \
   "$fs/fat.c" $sfsrc
# shellcheck disable=SC2086
cc $CFLAGS -o "$out/safetest_linkfirst" "$here/safetest.c" \
   "$top/apps/ipodui/safefile.c" -DFAT_EXTEND_LINK_FIRST "$fs/fat.c" $sfsrc
# shellcheck disable=SC2086
cc $CFLAGS -o "$out/safetest_unordered" "$here/safetest.c" \
   "$top/apps/ipodui/safefile.c" -DFAT_UNORDERED_RENAME "$fs/fat.c" $sfsrc
cc $CFLAGS -Drbfs_remove_alias=rt_no_alias -c -o "$out/safefile_noalias.o" \
   "$top/apps/ipodui/safefile.c"
# shellcheck disable=SC2086
cc $CFLAGS -o "$out/safetest_noalias" "$here/safetest.c" \
   "$out/safefile_noalias.o" "$fs/fat.c" "$out/file_nointent.o" $sfbase
# shellcheck disable=SC2086
cc $CFLAGS -o "$out/safetest_nointent" "$here/safetest.c" \
   "$top/apps/ipodui/safefile.c" "$fs/fat.c" "$out/file_nointent.o" $sfbase
# shellcheck disable=SC2086
cc $CFLAGS -o "$out/safetest_netonly" "$here/safetest.c" \
   "$top/apps/ipodui/safefile.c" "$fs/fat.c" "$out/file_netonly.o" $sfbase
"$out/fatcrash" mkfs "$out/sf.img" 167936
if ! SAFETEST_SEEDS=${SAFETEST_SEEDS:-"1 2 3 4 5 6 7 8"} \
        "$out/safetest" "$out/sf.img" >"$out/sf.log" 2>&1; then
    echo "safetest FAILED"; grep -v ' ok$' "$out/sf.log" | head -40; exit 1
fi
echo "safetest: $(grep -c ': ok$' "$out/sf.log") site x seed runs ok," \
     "$(awk '/cut points/{n+=$4} END{print n}' "$out/sf.log") cut points"
# cfgsafe's negative control: trusting any existing file (the stock
# boot's assumption) must lose the settings to the device's empty .new
cc $CFLAGS -DCFGSAFE_TRUST_ANY -c -o "$out/cfgsafe_trust.o" \
   "$top/apps/ipodui/cfgsafe.c"
# shellcheck disable=SC2086
cc $CFLAGS -o "$out/safetest_cfgtrust" "$here/safetest.c" \
   "$top/apps/ipodui/safefile.c" "$fs/fat.c" "$fs/file.c" $sfcore \
   "$out/cfgsafe_trust.o"
"$out/fatcrash" mkfs "$out/sfn.img" 167936
if "$out/safetest_cfgtrust" "$out/sfn.img" cfg-legacy cfg-empty \
        >"$out/sfn.log" 2>&1; then
    echo "safetest negative control (cfgtrust) passed: the test has no teeth"
    exit 1
fi
for st in cfg-legacy cfg-empty; do
    grep -q "^$st seed .*FAIL" "$out/sfn.log" \
        || { echo "cfgtrust control: $st not caught"; exit 1; }
done
echo "safetest negative control (cfgtrust) caught:" \
     "$(grep -m1 -E 'holds|left behind' "$out/sfn.log")"
# shellcheck disable=SC2086
cc $CFLAGS -o "$out/safetest_freefirst" "$here/safetest.c" \
   "$top/apps/ipodui/safefile.c" -DFAT_FREE_BEFORE_ENTRY "$fs/fat.c" $sfsrc
plain="settings changelog move-file move-dir overwrite"
for neg in unordered noalias nointent linkfirst freefirst; do
    "$out/fatcrash" mkfs "$out/sfn.img" 167936
    sites=""
    [ "$neg" = nointent ] && sites=$plain
    [ "$neg" = linkfirst ] && sites=dir-grow
    [ "$neg" = freefirst ] && sites="trunc-empty otrunc-temp"
    # shellcheck disable=SC2086
    if SAFETEST_SEEDS="1 2 3" "$out/safetest_$neg" "$out/sfn.img" $sites \
            >"$out/sfn.log" 2>&1; then
        echo "safetest negative control ($neg) passed: the test has no teeth"
        exit 1
    fi
    echo "safetest negative control ($neg) caught:" \
         "$(grep -m1 -E 'CHAIN|holds|DUPLICATE|left behind|DOTDOT|want|not ours' "$out/sfn.log")"
    if [ "$neg" = nointent ]; then
        for st in $plain; do
            grep -q "^$st seed .*FAIL" "$out/sfn.log" \
                || { echo "nointent control: $st not caught"; exit 1; }
        done
        # and on the chains alone: a later remove()/O_TRUNC frees one
        "$out/fatcrash" mkfs "$out/sfn.img" 167936
        # shellcheck disable=SC2086
        SAFETEST_NETONLY=1 SAFETEST_SEEDS="1" "$out/safetest_nointent" \
            "$out/sfn.img" $plain >"$out/sfn.log" 2>&1 || true
        for st in $plain; do
            grep -q "^$st seed .*FAIL" "$out/sfn.log" \
                || { echo "nointent control (chains): $st not caught"; exit 1; }
        done
        echo "safetest negative control (nointent, chains only) caught:" \
             "$(grep -m1 -E 'CHAIN|DOTDOT' "$out/sfn.log")"
    fi
done
"$out/fatcrash" mkfs "$out/sfn.img" 167936
if ! SAFETEST_NETONLY=1 SAFETEST_SEEDS="1 2 3" "$out/safetest_netonly" \
        "$out/sfn.img" changelog move-file overwrite >"$out/sfn.log" 2>&1; then
    echo "safetest safety net alone FAILED"; grep -v ' ok$' "$out/sfn.log" | head; exit 1
fi
echo "safetest safety net alone (no boot resolution): $(grep -c ': ok$' "$out/sfn.log") runs ok"
# H2: the drive's volatile write cache. SAFETEST_REORDER keeps every
# write in a modelled drive cache until storage_flush(); at each cut every
# cached sector lands or is lost on its own (all, none, 4 random subsets),
# so only the orders fat.c/file.c enforce with fat_barrier() hold. Must
# pass with the barriers; must fail without any (FAT_NO_BARRIER), with
# only the automatic FAT/directory ones (FAT_BARRIER_AUTO_ONLY), and
# without the one after an overwrite's victim (RBFS_NO_VICTIM_BARRIER:
# overwrite-far must then show two entries of one name, H1).
"$out/fatcrash" mkfs "$out/sfr.img" 167936
if ! SAFETEST_REORDER=1 SAFETEST_SEEDS=${SAFETEST_SEEDS:-"1 2 3 4 5 6 7 8"} \
        "$out/safetest" "$out/sfr.img" >"$out/sfr.log" 2>&1; then
    echo "safetest (drive cache reorders) FAILED"
    grep -v ' ok$' "$out/sfr.log" | grep -v '^k=' | head -40; exit 1
fi
echo "safetest (drive cache reorders): $(grep -c ': ok$' "$out/sfr.log")" \
     "site x seed runs ok, $(awk '/cut points/{n+=$4} END{print n}' \
     "$out/sfr.log") cut points x 6 cache outcomes;" \
     "flushes per sequence: $(awk '/flushes/{for(i=1;i<=NF;i++) if($i ~ /flushes/) printf "%s=%s ", $1, $(i-1)}' "$out/sfr.log" | tr ' ' '\n' | sort -u | grep -E '^(overwrite|move-file|move-dir|settings)=' | tr '\n' ' ')"
for neg in FAT_NO_BARRIER FAT_BARRIER_AUTO_ONLY RBFS_NO_VICTIM_BARRIER; do
    cc $CFLAGS -D$neg -Dfat_remove=rt_fat_remove -c \
       -o "$out/fi_$neg.o" "$fs/file_internal.c"
    # shellcheck disable=SC2086
    cc $CFLAGS -D$neg -o "$out/safetest_$neg" "$here/safetest.c" \
       "$top/apps/ipodui/safefile.c" "$fs/fat.c" "$fs/file.c" "$out/fi_$neg.o" \
       "$fs/fileobj_mgr.c" "$fs/dir.c" "$fs/pathfuncs.c" "$fs/rb_namespace.c" \
       "$fs/disk_cache.c" "$fs/linked_list.c" "$top/apps/ipodui/cfgsafe.c"
    "$out/fatcrash" mkfs "$out/sfn.img" 167936
    sites=""
    [ "$neg" = RBFS_NO_VICTIM_BARRIER ] && sites=overwrite-far
    # shellcheck disable=SC2086
    if SAFETEST_REORDER=1 SAFETEST_SEEDS="1 2 3" "$out/safetest_$neg" \
            "$out/sfn.img" $sites >"$out/sfn.log" 2>&1; then
        echo "safetest negative control ($neg, drive cache reorders) passed:" \
             "the test has no teeth"; exit 1
    fi
    echo "safetest negative control ($neg, drive cache reorders) caught" \
         "$(grep -c 'FAIL$' "$out/sfn.log") runs, first:" \
         "$(grep -m1 -E 'CHAIN|holds|DUPLICATE|left behind|DOTDOT|want|not ours|dest' "$out/sfn.log")"
    if [ -n "$sites" ] && ! grep -q DUPLICATE "$out/sfn.log"; then
        echo "victim-barrier control: no duplicate name caught"; exit 1
    fi
done
if /sbin/fsck_msdos -n "$out/sf.img" 2>&1 | grep -v ' files, ' \
        | grep -q Warning; then
    echo "safetest: fsck_msdos found damage"; exit 1
fi
[ -n "$FATCRASH_SAFE_ONLY" ] && exit 0

# R1 (#107) directory bursts: the file layer built with bursts and without
# (-DFAT_NO_DIR_BURST); same listings, same image, fewer read commands;
# writes during an enumeration never yield a stale or duplicate name. A
# build whose slots are never dropped must fail the mutation test.
srcs="$here/dirburst.c $fs/file.c $fs/file_internal.c $fs/fileobj_mgr.c \
    $fs/dir.c $fs/pathfuncs.c $fs/rb_namespace.c $fs/fat.c \
    $fs/disk_cache.c $fs/linked_list.c"
# shellcheck disable=SC2086
cc $CFLAGS -o "$out/dirburst" $srcs
# shellcheck disable=SC2086
cc $CFLAGS -DFAT_NO_DIR_BURST -o "$out/dirburst_off" $srcs
# shellcheck disable=SC2086
cc $CFLAGS -DFATCRASH_BREAK_BURST_DROP -o "$out/dirburst_broken" $srcs
# shellcheck disable=SC2086
cc $CFLAGS -DFATCRASH_BREAK_BURST_DROP_ALL -o "$out/dirburst_broken_all" $srcs
tracks=${DIRBURST_TRACKS:-6000}
# geometry: storage sectors per cluster : BPB bytes per sector (2048 is the
# 5.5G's on-disk layout, scaled by fat.c's secmult = 4)
for g in 2:512 32:512 32:2048; do
    spc=${g%:*} bps=${g#*:}
    "$out/dirburst" lib "$tracks" $spc $bps >"$out/db_on.txt" 2>"$out/db_on.err" \
        || { echo "dirburst lib $g FAILED"; cat "$out/db_on.txt"; exit 1; }
    "$out/dirburst_off" lib "$tracks" $spc $bps >"$out/db_off.txt" 2>"$out/db_off.err" \
        || { echo "dirburst_off lib $g FAILED"; exit 1; }
    grep -v '^stat' "$out/db_on.txt" >"$out/db_on.cmp"
    grep -v '^stat' "$out/db_off.txt" >"$out/db_off.cmp"
    if ! cmp -s "$out/db_on.cmp" "$out/db_off.cmp"; then
        echo "dirburst lib $g: listings or image differ"
        diff "$out/db_on.txt" "$out/db_off.txt"; exit 1
    fi
    on=$(awk '/^stat walk/{print $3}' "$out/db_on.txt")
    off=$(awk '/^stat walk/{print $3}' "$out/db_off.txt")
    oon=$(awk '/^stat open/{print $3}' "$out/db_on.txt")
    ooff=$(awk '/^stat open/{print $3}' "$out/db_off.txt")
    echo "dirburst lib $tracks tracks spc:bps=$g: walk $off -> $on commands," \
         "opens $ooff -> $oon commands, listings identical"
    [ "$on" -lt "$off" ] || { echo "dirburst: no fewer commands"; exit 1; }
done
for g in 2:512 8:512 32:512 8:2048 32:2048; do
    spc=${g%:*} bps=${g#*:}
    for seed in 1 2 3; do
        "$out/dirburst" mut $seed $spc $bps >"$out/mut_on.txt" 2>&1 \
            || { echo "dirburst mut $seed $g FAILED"; cat "$out/mut_on.txt"; exit 1; }
        "$out/dirburst_off" mut $seed $spc $bps >"$out/mut_off.txt" 2>&1 \
            || { echo "dirburst_off mut $seed $g FAILED"; cat "$out/mut_off.txt"; exit 1; }
        cmp -s "$out/mut_on.txt" "$out/mut_off.txt" \
            || { echo "dirburst mut $seed $g: listings differ"; exit 1; }
    done
    echo "dirburst mut spc:bps=$g: 3 seeds ok, identical to upstream reads"
done
# read errors on bursts (single-sector retry, stream position, readdir
# retry) and a USB unmount/host write/remount in mid-enumeration
for g in 4:512 32:512 4:2048 32:2048; do
    spc=${g%:*} bps=${g#*:}
    for m in fault usb; do
        "$out/dirburst" $m - $spc $bps >"$out/db_$m.txt" 2>&1 \
            || { echo "dirburst $m $g FAILED"; cat "$out/db_$m.txt"; exit 1; }
    done
    echo "dirburst fault + usb spc:bps=$g: ok"
done
# writes never drop slots / mount and unmount never drop them
run_broken() { "$out/$1" "$2" "$3" 32; }
for m in "dirburst_broken mut 1" "dirburst_broken_all usb -"; do
    # shellcheck disable=SC2086
    if run_broken $m >"$out/db_broken.txt" 2>&1; then
        echo "dirburst negative control ($m) passed: the test has no teeth"
        exit 1
    fi
    echo "dirburst negative control ($m) caught: $(head -1 "$out/db_broken.txt")"
done
[ -n "$FATCRASH_DIRBURST_ONLY" ] && exit 0

base="$out/base.img"
rm -f "$base"
# FAT32, 1 KiB clusters, 2 FATs, ~83k clusters (FAT32's minimum is 65,525)
"$out/fatcrash" mkfs "$base" 167936
/sbin/fsck_msdos -n "$base" >"$out/mkfs.fsck" 2>&1 || {
    echo "fresh image not clean"; cat "$out/mkfs.fsck"; exit 1; }
"$out/fatcrash" setup "$base"
/sbin/fsck_msdos -n "$base" >"$out/base.fsck" 2>&1 || {
    echo "base image not clean"; cat "$out/base.fsck"; exit 1; }

classify() {   # fsck output -> short tags
    awk '
    /starts with free cluster/      {t["startfree"]=1}
    /cross-linked/                   {t["crosslink"]=1}
    /too many clusters allocated/    {t["toolong"]=1}
    /size of .* is .* should at most/ {t["toolong"]=1}
    /orphan cluster/                 {t["orphan"]=1}
    /Free space in FSInfo/           {t["fsinfo"]=1}
    /has too few clusters|is too short/ {t["tooshort"]=1}
    /[Dd]uplicate/                   {t["dupname"]=1}
    /Invalid long filename|invalid long/ {t["badlfn"]=1}
    END { s=""; for (k in t) s=s k " "; if (s=="") s="clean"; print s }'
}

# a halt inside a rename leaves one file under two names (same start
# cluster, same size: fat.c FAT_ORDERED_DIRENTRIES), which fsck calls a
# cross-link. Tag such an image "alias" when no other entries share a
# cluster. The next-boot column runs file.c's boot recovery (the rename
# intent record), which must leave no alias there.
alias_tag() {
    r=$("$out/fatcrash" aliases "$out/cut.img")
    case "$1 $r" in
    *crosslink*"alias 0 other"*) echo "$1" ;;
    *crosslink*"other 0") echo "$1" | sed 's/crosslink /alias /' ;;
    *) echo "$1" ;;
    esac
}

: >"$out/results.txt"
scenarios=${*:-"old-publish new-publish slot bundle resume all"}
seeds=${FATCRASH_SEEDS:-""}
# run_scenarios <fatcrash binary> <results file> <scenario...>
run_scenarios() {
  fc=$1 res=$2; shift 2
  : >"$res"
  for sc in "$@"; do
    for seed in "" $seeds; do
      n=$(env ${seed:+FATCRASH_SEED=$seed} "$fc" record "$base" "$out/$sc.log" "$sc")
      printf '== %s seed=%s: %s sector writes\n' "$sc" "$seed" "$n"
      k=0
      while [ "$k" -le "$n" ]; do
          "$fc" apply "$base" "$out/$sc.log" "$k" "$out/cut.img"
          a=$(/sbin/fsck_msdos -n "$out/cut.img" 2>&1 | classify)
          a=$(alias_tag "$a")
          "$fc" post "$out/cut.img" 2>/dev/null || true
          b=$(/sbin/fsck_msdos -n "$out/cut.img" 2>&1 | classify)
          b=$(alias_tag "$b")
          printf '%s k=%d halt: %s| next boot: %s\n' "$sc" "$k" "$a" "$b" \
              | tee -a "$res"
          k=$((k + 1))
      done
    done
  done
}
# shellcheck disable=SC2086
run_scenarios "$out/fatcrash" "$out/results.txt" $scenarios

# tally; with FATCRASH_ASSERT=1 any entry pointing at a free or shared
# cluster (now or after the next boot), or one file still under two names
# after the next boot's recovery, fails the run
pts=$(wc -l <"$out/results.txt" | tr -d ' ')
bad=$(grep -cE 'startfree|crosslink' "$out/results.txt" || true)
al=$(grep -c 'halt: [^|]*alias' "$out/results.txt" || true)
aln=$(grep -c 'next boot: .*alias' "$out/results.txt" || true)
echo "crash points: $pts, with a free/shared-cluster reference: $bad," \
     "one file under two names at the halt: $al, after the next boot's" \
     "recovery: $aln"
if [ -n "$FATCRASH_ASSERT" ] && { [ "$bad" -ne 0 ] || [ "$aln" -ne 0 ]; }; then
    echo "FAIL"; exit 1
fi
# negative control: without the record the next boot keeps the aliases
"$out/fatcrash_nointent" mkfs "$base" 167936
"$out/fatcrash_nointent" setup "$base"
seeds="" run_scenarios "$out/fatcrash_nointent" "$out/nointent.txt" resume \
    >/dev/null
nal=$(grep -c 'next boot: .*alias' "$out/nointent.txt" || true)
if [ "$nal" -eq 0 ]; then
    echo "fatcrash negative control (no intent record) kept no alias:" \
         "the next-boot column has no teeth"; exit 1
fi
echo "fatcrash negative control (no intent record): $nal of" \
     "$(wc -l <"$out/nointent.txt" | tr -d ' ') resume cuts keep an alias" \
     "after the next boot"
