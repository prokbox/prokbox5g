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

# The 0.9.0 format upgrade in the simulator (docs/changes-from-rockbox.md
# 3.1): a library built by the 0.8.3 simulator, then this tree's.
#
#   tests/p090/upgrade.sh OLD_SIM_BUILD NEW_SIM_BUILD
#
# OLD_SIM_BUILD: a --ipodui simulator of rel-0.8.3 (SORTKEY_VERSION 2),
# made and installed (make install); NEW_SIM_BUILD: this tree's. Not part
# of scripts/test.sh (it needs the 0.8.3 build). Steps, on one disk
# (build/p090-upgrade/simdisk):
#   A  the old simulator builds the test library and rates two tracks
#      (upgrade/a-083.script); the master index must carry 0.8.x's magic;
#   B  this tree's files are installed over it, the database kept (as an
#      upgrade does), and it boots (upgrade/b-090.script): the log must say
#      "upgrade owed from=none to=a3/k3/s1 why=missing" (no
#      library stamp: a 0.8.x library), an export, exactly one first build and
#      the ratings must survive; the master carries the new magic; the
#      boot shows the library page (p090-upgpage: "library page at boot:
#      no library (other version)", "upd push mode=boot"), as a first
#      build does, never the menus over a silent rebuild;
#   C  it boots again: no "upgrade owed", no new build, same ratings, no
#      library page.
# Exit 0 only when every check passed; each check prints one line.
set -u
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
OLD=${1:?usage: upgrade.sh OLD_SIM_BUILD NEW_SIM_BUILD}
NEW=${2:?usage: upgrade.sh OLD_SIM_BUILD NEW_SIM_BUILD}
case "$OLD" in /*) ;; *) OLD=$ROOT/$OLD ;; esac
case "$NEW" in /*) ;; *) NEW=$ROOT/$NEW ;; esac
W=$ROOT/build/p090-upgrade
D=$W/simdisk
T=$ROOT/tests/p090/upgrade
fail=0
ok() { echo "  ok   $*"; }
bad() { echo "  FAIL $*"; fail=1; }
magic() { # the newest generation's master index, first 4 bytes
    f=$(ls -t "$D"/.rockbox/database_gen*/database_idx.tcd 2>/dev/null | head -1)
    [ -n "$f" ] && od -An -tx1 -N4 "$f" | tr -d ' \n'
}
run() { # SIM SCRIPT OUT
    SIM_BUILD=$1 SIMDISK=$D "$ROOT/scripts/shots.sh" "$2" "$3" >"$3.out" 2>&1
}
[ -x "$OLD/rockboxui" ] && [ -x "$NEW/rockboxui" ] \
    || { echo "upgrade: FAIL no simulator in $OLD or $NEW"; exit 2; }
rm -rf "$W"; mkdir -p "$D"
rsync -a "$OLD/simdisk/" "$D/"

# A: the 0.8.3 library
run "$OLD" "$T/a-083.script" "$W/a"
grep -q "FAIL\|failed" "$W/a.out" && bad "A: the 0.8.3 run failed ($W/a.out)"
m=$(magic)
[ "$m" = a2484354 ] && ok "A: 0.8.3 master index magic $m" \
    || bad "A: master index magic '$m', want a2484354"
cp "$D/.rockbox/ipodui.log" "$W/a.ipodui.log" 2>/dev/null

# B: this firmware installed over it, the database kept
rsync -a --exclude 'database*' --exclude 'ipodui.log*' --exclude '*.cfg' \
    "$NEW/simdisk/.rockbox/" "$D/.rockbox/"
: >"$D/.rockbox/ipodui.log"
run "$NEW" "$T/b-090.script" "$W/b"
grep -q "FAIL\|failed" "$W/b.out" && bad "B: a check failed ($W/b.out)" \
    || ok "B: rating 7 came over; library published"
L=$D/.rockbox/ipodui.log
cp "$L" "$W/b.ipodui.log"
grep -q "upgrade owed from=none to=a3/k3/s1 why=missing" "$L" \
    && ok "B: log: upgrade owed from=none to=a3/k3/s1 why=missing" \
    || bad "B: no 'upgrade owed from=none to=a3/k3/s1 why=missing' line"
e=$(sed -n 's/.*upgrade export rc=\(-*[0-9]*\).*/\1/p' "$L" | head -1)
# 1: rows with statistics written from the 0.8.3 master; 2: the master held
# none (they were all still in the journal, which the rebuild replays)
case "$e" in
    1) ok "B: log: upgrade export rc=1 (statistics written from the 0.8.3 master)" ;;
    2) ok "B: log: upgrade export rc=2 (the 0.8.3 master read; no folded statistics in it)" ;;
    *) bad "B: upgrade export rc='$e', want 1 or 2" ;;
esac
n=$(grep -c "upgrade owed" "$L")
[ "$n" = 1 ] && ok "B: one upgrade this boot" || bad "B: $n upgrade lines"
# p090-upgpage: the old library cannot be shown, so the boot is a first
# build's: the library page (its progress and build hold), not the menus
# with a silent rebuild behind them
S=$W/b/sim.log
if [ ! -s "$S" ]; then
    bad "B: no sim.log ($S)"
else
    grep -q "library page at boot: no library (other version)" "$S" \
        && ok "B: log: library page at boot: no library (other version)" \
        || bad "B: boot page decision: '$(grep -o 'library page at boot: .*' "$S" | head -1)', want 'no library (other version)'"
    grep -q "upd push mode=boot" "$S" && ok "B: log: upd push mode=boot" \
        || bad "B: no 'upd push mode=boot' (the library page did not come up)"
    grep -q "upd done ms=" "$S" && ok "B: log: upd done (the page ended with the library)" \
        || bad "B: no 'upd done' (the page did not finish)"
    grep -q "upd dedicated depth=0" "$S" && ! grep -q "upd dedicated depth=-" "$S" \
        && ok "B: the page's build hold given back (depth=0)" \
        || bad "B: the page's build hold not balanced"
fi
m=$(magic)
[ "$m" = a3484354 ] && ok "B: master index magic now $m" \
    || bad "B: master index magic '$m', want a3484354"
# the published manifest carries this firmware's library stamp (dbgen.c
# struct manifest, offset 264: the format magic, key order 3, schema 1)
st=
for mf in "$D"/.rockbox/database_manifest[01].dat; do
    [ -f "$mf" ] || continue
    v=$(od -An -tx1 -j264 -N8 "$mf" | tr -d ' \n')
    [ "$v" = a348435403000100 ] && st=$v
done
[ -n "$st" ] && ok "B: a manifest carries the library stamp ($st)" \
    || bad "B: no manifest with the stamp a3484354/k3/s1 at offset 264"
gb=$(ls -d "$D"/.rockbox/database_gen* | wc -l | tr -d ' ')

# C: the next boot
: >"$L"
run "$NEW" "$T/b-090.script" "$W/c"
cp "$L" "$W/c.ipodui.log"
grep -q "FAIL\|failed" "$W/c.out" && bad "C: a check failed ($W/c.out)" \
    || ok "C: rating still 7"
grep -q "upgrade owed" "$L" && bad "C: upgrade owed again" \
    || ok "C: no upgrade owed"
grep -q "gen select rc=0" "$L" && ok "C: log: gen select rc=0 (library selected as is)" \
    || bad "C: no 'gen select rc=0' line"
grep -q "library page at boot: library" "$W/c/sim.log" 2>/dev/null \
    && ! grep -q "upd push mode=boot" "$W/c/sim.log" \
    && ok "C: no library page (the library is there)" \
    || bad "C: the library page came up again, or no boot page decision in $W/c/sim.log"
m=$(magic)
[ "$m" = a3484354 ] && ok "C: master index magic $m" || bad "C: magic '$m'"
echo "upgrade: logs in $W (a/b/c .out, *.ipodui.log); generation dirs after B: $gb"
[ $fail = 0 ] && echo "ok   p090 upgrade" || echo "FAIL: p090 upgrade"
exit $fail
