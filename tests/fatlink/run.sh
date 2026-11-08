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
# tests/fatlink: host rehearsal of shared-cluster links (#137), hazards H1-H4.
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
# Usage: tests/fatlink/run.sh [scenario...]       (default: all)
#        DRIVER=kext tests/fatlink/run.sh ...      (legacy kernel driver, 2048-byte sectors)
# Every scenario builds a fresh 2 GB sparse FAT32 image (16 KiB clusters) under
# build/fatlink, so nothing here can reach a real device.  Prints one line per
# scenario and appends it to build/fatlink/results-$DRIVER.log.

. "$(dirname "$0")/lib.sh"

O="/Music/Album/01 Song One.mp3"          # the user's original
L="/iPod_Control/Music/PKB/01 Song One.mp3" # the link Apple's firmware plays
SIZE=200000                                 # 13 clusters of 16 KiB
FAIL=0

rnd() { head -c "$2" /dev/urandom > "$1"; }

# fresh volume: Music/Album with the original plus a neighbour, then a link
setup() {  # $1 = link attributes (e.g. RH), $2 = optional "nolink"
    mkimg
    mnt
    mkdir -p "$MNT/Music/Album"
    rnd "$MNT$O" $SIZE
    rnd "$MNT/Music/Album/02 Song Two.mp3" $SIZE
    HO=$(h "$MNT$O")
    umnt
    [ "${2:-}" = nolink ] || $FL "$IMG" link "$O" "$L" --attr "${1:-}" >/dev/null
    OEXT=$($FL "$IMG" chain "$O" | sed 's/.*clusters=\([^ ]*\).*/\1/')
}

st() { $FL "$IMG" verify 2>&1 | awk 'NR==1{print $1}'; }
after() {  # cleanup + the checks every scenario reports
    VS=$(st)
    CL=$($FL "$IMG" cleanup 2>&1 | tr '\n' ';' | sed 's/  */ /g')
    VS2=$(st)
    FK=$(fsckn)
    case "$FK" in *cross-linked*) FKS="crosslink";; *Truncate*|*Fix*|*Remove*|*Clear*|*Reconnect*) FKS="problems";; *) FKS="clean";; esac
    # a live link is supposed to be a cross-link; anything else is a problem
    HOA=$(himg "$O"); HLA=$(himg "$L")
}
report() {  # $1 scenario, $2 host-op note, $3 expectation
    local line="$1 | host: $2 | verify: $VS | cleanup: ${CL:0:160} | after: $VS2 | fsck -n: $FKS | orig $( [ "$HOA" = "$HO" ] && echo intact || echo "CHANGED($HOA)") | link $( [ -z "$HLA" ] || [ "$HLA" = "da39a3ee5e6b" ] && echo absent || ([ "$HLA" = "$HOA" ] && echo "=orig" || echo "DIFFERS")) | $3"
    log "$line"
}

# ---------------------------------------------------------------------------
s_base() {
    setup RH
    mnt; R=$(h "$MNT$L"); F=$(/bin/ls -lO "$MNT$L" | awk '{print $5}'); umnt
    after
    report base "host reads link: $( [ "$R" = "$HO" ] && echo same-bytes || echo DIFF ) flags=$F" "expect OK, link=orig"
}

s_h1_rm_orig() {
    setup RH
    mnt; rm "$MNT$O"; rnd "$MNT/Music/Album/03 New.mp3" $SIZE; HN=$(h "$MNT/Music/Album/03 New.mp3"); umnt
    NEXT=$($FL "$IMG" chain "/Music/Album/03 New.mp3" | sed 's/.*clusters=\([^ ]*\).*/\1/')
    after
    HO=gone
    report h1-rm-orig "rm orig, new file got $NEXT (orig was $OEXT); new file $( [ "$(himg "/Music/Album/03 New.mp3")" = "$HN" ] && echo intact || echo DAMAGED)" "expect ORPHAN, link removed, no FAT change"
}

s_h1_rm_orig_fill() {  # orig deleted, then the host fills: does it reuse orig's clusters?
    setup RH
    mnt; rm "$MNT$O"
    for i in 1 2 3 4; do rnd "$MNT/Music/Album/n$i.mp3" 100000; done
    umnt
    WHERE=$(for i in 1 2 3 4; do $FL "$IMG" chain "/Music/Album/n$i.mp3" | sed 's/.*clusters=\([^ ]*\).*/\1/'; done | tr '\n' ' ')
    after; HO=gone
    report h1-rm-orig-fill "rm orig, 4 new files at $WHERE(orig was $OEXT)" "allocator reuse probe"
}

s_h2_append() {
    setup RH
    mnt; head -c 50000 /dev/urandom >> "$MNT$O"; HO=$(h "$MNT$O"); umnt
    after
    report h2-append "append 50 KB" "expect STALE, rebuilt, link=orig"
}

s_h2_truncate() {
    setup RH
    mnt; python3 -c "import os,sys; os.truncate(sys.argv[1], 60000)" "$MNT$O"; HO=$(h "$MNT$O"); umnt
    after
    report h2-truncate "truncate to 60 KB" "expect STALE, rebuilt"
}

s_h2_cpover() {
    setup RH
    mnt; rnd "$WORK/new.bin" $SIZE; cp "$WORK/new.bin" "$MNT$O"; HO=$(h "$MNT$O"); umnt
    N=$($FL "$IMG" chain "$O" | sed 's/.*clusters=\([^ ]*\).*/\1/')
    after
    report h2-cp-over "cp over orig (was $OEXT, now $N)" "expect STALE, rebuilt"
}

s_h2_inplace() {  # a tag editor patching bytes in place
    setup RH
    mnt; python3 -c "
import sys; f=open(sys.argv[1],'r+b'); f.seek(100); f.write(b'NEWTAG'*20); f.close()" "$MNT$O"; HO=$(h "$MNT$O"); umnt
    after
    report h2-inplace "rewrite 120 bytes in place" "expect OK (shared data)"
}

s_h2_rewrite_tmp() {  # a tag editor that writes a temp file and renames it over
    setup RH
    mnt; cp "$MNT$O" "$MNT$O.tmp"; printf 'TAG' | dd of="$MNT$O.tmp" bs=1 seek=10 conv=notrunc 2>/dev/null
    mv -f "$MNT$O.tmp" "$MNT$O"; HO=$(h "$MNT$O"); umnt
    after
    report h2-rename-over "temp file renamed over orig" "expect STALE, rebuilt"
}

s_h2_xattr() {
    setup RH
    mnt; xattr -w com.example.test hello "$MNT$O"; X=$(/bin/ls -a "$MNT/Music/Album" | grep -c '^\._'); umnt
    after
    report h2-xattr "xattr -w on orig (._ files: $X)" "expect OK"
}

s_h2_mv_orig() {
    setup RH
    mnt; mkdir -p "$MNT/Music/Other"; mv "$MNT$O" "$MNT/Music/Other/x.mp3"; umnt
    after; HOA=$(himg "/Music/Other/x.mp3")
    report h2-mv-orig "mv orig to another folder" "expect ORPHAN (library sync relinks the new path)"
}

h3_common() {  # $1 name, $2 attrs, $3 host op (shell, uses $MNT$L), $4 writes after (bytes or 0)
    setup "$2"
    mnt
    OUT=$(eval "$3" 2>&1 | head -1 | tr -d '\n' | cut -c1-90); [ -e "$MNT$L" ] && GONE=no || GONE=yes
    W=""
    if [ "${4:-0}" != 0 ]; then
        rnd "$MNT/Music/Album/03 New.mp3" "$4"
        W=", then wrote ${4}B -> $($FL "$IMG" chain "/Music/Album/03 New.mp3" 2>/dev/null | head -c0)"
    fi
    RO=$(h "$MNT$O")
    umnt
    [ "${4:-0}" != 0 ] && W=", then wrote ${4}B at $($FL "$IMG" chain "/Music/Album/03 New.mp3" | sed 's/.*clusters=\([^ ]*\).*/\1/') (orig $OEXT)"
    FK0=$(fsckn | grep -o -E 'starts with [a-z -]*cluster|ends with cluster marked free|cross-linked|should at most be [0-9]+' | sort -u | head -3 | tr '\n' ';')
    after
    report "$1" "[$OUT] link gone=$GONE; orig in-session: $( [ "$RO" = "$HO" ] && echo readable || ([ "$RO" = "-" ] && echo "not listed" || echo "BYTES CHANGED"))$W; fsck before cleanup: ${FK0:-clean}" "H3"
}

s_h3_rm_plain()   { h3_common h3-rm-plain   ""   'rm "$MNT$L"' 0; }
s_h3_rm_plain_w() { h3_common h3-rm-plain+write "" 'rm "$MNT$L"' 200000; }
s_h3_rm_plain_big() { h3_common h3-rm-plain+bigwrite "" 'rm "$MNT$L"' 3000000; }
s_h3_rm_ro()      { h3_common h3-rm-RH      RH   'rm "$MNT$L"' 200000; }
s_h3_rmf_ro()     { h3_common h3-rm-f-RH    RH   'rm -f "$MNT$L"' 200000; }
s_h3_rmrf_dir()   { h3_common h3-rm-rf-dir  RHS  'rm -rf "$MNT/iPod_Control"' 200000; }
s_h3_trash()      { h3_common h3-trash-RH   RH   'mkdir -p "$MNT/.Trashes/501" && mv "$MNT$L" "$MNT/.Trashes/501/" && echo mv-ok' 0; }
s_h3_trash_empty(){ h3_common h3-trash+empty-RH RH 'mkdir -p "$MNT/.Trashes/501" && mv "$MNT$L" "$MNT/.Trashes/501/" && rm -rf "$MNT/.Trashes/501/"* && echo emptied' 200000; }
s_h3_chflags()    { h3_common h3-chflags-rm RH   'chflags nouchg "$MNT$L" && rm "$MNT$L" && echo unlocked-and-removed' 200000; }
s_h3_finder()     { h3_common h3-finder-RH  RH   'timeout 20 osascript -e "tell application \"Finder\" to delete (POSIX file \"$MNT$L\" as alias)"' 0; }
s_h3_rm_plain_norepair() {
    setup ""
    mnt; rm "$MNT$L"; umnt
    VS=$(st); CL=$($FL "$IMG" cleanup --no-repair 2>&1 | tr '\n' ';'); VS2=$(st); FK=$(fsckn)
    case "$FK" in *cross*) FKS=crosslink;; *Truncate*|*Fix*) FKS="problems: $(echo "$FK" | grep -o 'size of[^?]*' | head -1)";; *) FKS=clean;; esac
    HOA=$(himg "$O"); HLA=$(himg "$L")
    report h3-rm-no-repair "rm link" "what fsck sees without the repair"
}

h4_common() {  # $1 name, $2 link path
    setup RH nolink
    $FL "$IMG" link "$O" "$2" --attr RH >/dev/null
    N=$(fsck_msdos -n "$IMG" 2>&1 | grep -c -i cross)
    cp "$IMG" "$WORK/fsck-copy.img"
    Y=$(fsck_msdos -y "$WORK/fsck-copy.img" 2>&1 | grep -v -E '^\*\* (/|Phase)' | tr '\n' ' ' | sed 's/  */ /g' | cut -c1-400)
    SAVE=$IMG; IMG=$WORK/fsck-copy.img
    A=$($FL "$IMG" chain "$O" 2>&1 | tr '\n' ' '); B=$($FL "$IMG" chain "$2" 2>&1 | tr '\n' ' ')
    HA=$(himg "$O"); HB=$(himg "$2")
    IMG=$SAVE; rm -f "$WORK/fsck-copy.img"
    log "$1 | fsck -n cross-link lines: $N | fsck -y said: $Y | after -y: orig [$A] $( [ "$HA" = "$HO" ] && echo intact || echo "LOST($HA)") ; link [$B] $( [ "$HB" = "$HO" ] && echo intact || echo "LOST($HB)")"
}
s_h4_fsck_y_pkb()  { h4_common h4-fsck-y-link-in-iPod_Control "$L"; }
s_h4_fsck_y_root() { h4_common h4-fsck-y-link-in-/AAA "/AAA/01 Song One.mp3"; }

s_h4_dirty_mount() {  # dirty volume: does a DiskArbitration mount run fsck and repair it?
    setup RH
    $FL "$IMG" dirty set >/dev/null
    mnt; R=$(h "$MNT$O"); RL=$(h "$MNT$L"); umnt
    D=$($FL "$IMG" dirty)
    after
    report h4-dirty-mount "dirty flag set before mount; after unmount: $D; orig in-session $( [ "$R" = "$HO" ] && echo ok || echo BAD) link $( [ "$RL" = "$HO" ] && echo ok || echo BAD)" "auto-fsck probe"
}

s_h4_clean_mount() {
    setup RH
    mnt; umnt
    after
    report h4-clean-mount "clean volume" "auto-fsck probe"
}

s_h3_remount_write() {  # link deleted, volume ejected and plugged in again, then a write
    setup ""
    mnt; rm "$MNT$L"; umnt
    mnt; rnd "$MNT/Music/Album/03 New.mp3" 200000; umnt
    W=$($FL "$IMG" chain "/Music/Album/03 New.mp3" | sed 's/.*clusters=\([^ ]*\).*/\1/')
    after
    report h3-rm+remount+write "rm link, eject, remount, wrote 200000B at $W (orig $OEXT)" "allocator after remount"
}

s_h3_full() {  # nearly full volume: the allocator must wrap onto the freed clusters
    setup ""
    mnt
    FREE=$(df -k "$MNT" | awk 'NR==2{print $4}')
    head -c $(( (FREE - 1024) * 1024 )) /dev/zero > "$MNT/Music/fill.bin"
    rm "$MNT$L"
    rnd "$MNT/Music/Album/03 New.mp3" 1200000; RC=$?
    umnt
    W=$($FL "$IMG" chain "/Music/Album/03 New.mp3" | sed 's/.*clusters=\([^ ]*\).*/\1/')
    after
    report h3-full-volume "filled to ~1 MB free, rm link, wrote 1.2 MB (rc=$RC) at $W (orig $OEXT)" "reuse when the volume is full"
}

s_h4_fsck_y_restore() {  # fsck -y empties the original; the cleanup restores it from the link
    setup RH
    fsck_msdos -y "$IMG" >/dev/null 2>&1
    after
    report h4-fsck-y+cleanup "fsck_msdos -y on the image" "expect EMPTIED, restored"
}

s_park_cycle() {  # recommended design: links parked (absent) for the whole USB session
    setup RH
    $FL "$IMG" link "/Music/Album/02 Song Two.mp3" "/iPod_Control/Music/PKB/02 Song Two.mp3" --attr RH >/dev/null
    P=$($FL "$IMG" park)
    FKP=$(fsckn); case "$FKP" in *cross*|*Truncate*|*Fix*) FKP=problems;; *) FKP=clean;; esac
    $FL "$IMG" dirty set >/dev/null        # worst case: the host also finds it dirty
    mnt
    head -c 50000 /dev/urandom >> "$MNT$O"; HO=$(h "$MNT$O")
    rm "$MNT/Music/Album/02 Song Two.mp3"
    rnd "$MNT/Music/Album/03 New.mp3" 400000
    umnt
    mnt; rnd "$MNT/Music/Album/04 New.mp3" 400000; umnt   # second session: low-first allocator
    U=$($FL "$IMG" unpark | tail -1)
    after
    report park-cycle "$P; fsck -n while parked: $FKP; dirty mount, append orig, rm song 2, 2 writes over 2 sessions; $U" "expect 1 link OK, 1 dropped, nothing damaged"
}

ALL="base h1_rm_orig h1_rm_orig_fill h2_append h2_truncate h2_cpover h2_inplace h2_rewrite_tmp h2_xattr h2_mv_orig
h3_rm_plain h3_rm_plain_w h3_rm_plain_big h3_rm_plain_norepair h3_rm_ro h3_rmf_ro h3_rmrf_dir h3_chflags h3_trash h3_trash_empty
h3_remount_write h3_full h4_fsck_y_pkb h4_fsck_y_root h4_fsck_y_restore h4_clean_mount h4_dirty_mount park_cycle"
[ "${FINDER:-0}" = 1 ] && ALL="$ALL h3_finder"
log "=== $(date '+%Y-%m-%d %H:%M') DRIVER=$DRIVER macOS $(sw_vers -productVersion)"
for s in ${*:-$ALL}; do "s_$s"; done
rm -f "$IMG" "$WORK/new.bin"
