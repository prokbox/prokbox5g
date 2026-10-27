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

# Library scan cost on an unchanged and on a slightly changed library
# (scan speed, 2026-10-19).
#
#   tests/perf/scan_misses.sh [tracks] [junk]
#
# Makes build/sim-scan: the build/sim-ui simulator (symlinked) with its own
# simdisk holding the synthetic library of tests/perf/synthetic_library.py
# (default 300 tracks), a macOS AppleDouble sidecar ("._<name>.mp3", 4 KB,
# the header Finder writes on FAT) next to every track, and <junk> (default
# 30) unparseable "junk-NN.mp3" files: never indexed, so every scan misses
# their lookups. Runs the same script twice, once as the scan was
# (SIM_TC_SCAN_LEGACY=1: sequential lookups, an open of the master per
# track, the clone up front) and once as it is:
#   boot 1  the first build;
#   boot 2  the boot-time update of the unchanged library (a read-only probe
#           now: no candidate, no clone);
#   update  a no-op update with its deleted-file pass;
#   update  one file added (the probe finds it; candidate, scan, publish).
# Each scan prints "tagcache: probe|scan done, N new, M missed, L lookups,
# R reads, O master opens". Passes when, without legacy mode:
#   - no scan line has a missed lookup that read the tag file (R <= L, the
#     hits' own verification reads), while legacy reads many more;
#   - a candidate scan opens the master at most twice (legacy: ~n);
#   - the unchanged boot and the no-op update prepare no candidate
#     ("expect_prepares 0"), and the added file still publishes.
set -e
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
N=${1:-300}
J=${2:-30}
UI=$ROOT/build/sim-ui
B=$ROOT/build/sim-scan
[ -x "$UI/rockboxui" ] || { echo "build build/sim-ui first" >&2; exit 2; }
[ -d "$UI/simdisk/.rockbox/codecs" ] || make -C "$UI" install >/dev/null

setup() {
    rm -rf "$B"
    mkdir -p "$B/simdisk"
    ln -sf "$UI/rockboxui" "$B/rockboxui"
    [ -f "$UI/UI256.bmp" ] && ln -sf "$UI/UI256.bmp" "$B/UI256.bmp"
    rsync -a --exclude 'database*' "$UI/simdisk/.rockbox/" "$B/simdisk/.rockbox/"
    python3 "$ROOT/tests/perf/synthetic_library.py" "$B/simdisk" "$N" >/dev/null
    python3 - "$B/simdisk/Synth" "$J" <<'EOF'
import os, struct, sys
hdr = struct.pack('>II16sH', 0x00051607, 0x00020000, b'Mac OS X        ', 2)
body = hdr + b'\0' * (4096 - len(hdr))
for dp, dn, fn in os.walk(sys.argv[1]):
    for f in fn:
        if f.endswith('.mp3') and not f.startswith('._'):
            open(os.path.join(dp, '._' + f), 'wb').write(body)
for i in range(int(sys.argv[2])):
    open(os.path.join(sys.argv[1], 'junk-%02d.mp3' % i), 'wb').write(body)
EOF
    cp "$ROOT/tests/ui/fixtures/refresh.mp3" "$B/added.mp3"
}

S=$B/../scan_misses.script
cat >"$S" <<'EOF'
# timeout: 900
wait 3000
wait_db 600000
wait 20000
reboot
wait 3000
wait_db 600000
wait 20000
dbrec expect_prepares PREP
update_db_wait 600000
dbrec expect_prepares PREP
copy added.mp3 /Synth/zz-added.mp3
update_db_wait_change 600000
shot end
quit
EOF

run() {   # $1: legacy|new
    setup
    if [ "$1" = legacy ]; then
        sed 's/^dbrec expect_prepares PREP$/dbrec expect_prepares 9/' "$S" >"$B/scan_misses.script"
        export SIM_TC_SCAN_LEGACY=1
    else
        sed 's/^dbrec expect_prepares PREP$/dbrec expect_prepares 0/' "$S" >"$B/scan_misses.script"
        unset SIM_TC_SCAN_LEGACY
    fi
    rc=0
    SIM_BUILD=$B SHOTS_TIMEOUT=900 "$ROOT/scripts/shots.sh" "$B/scan_misses.script" \
        >"$B/shots.out" 2>&1 || rc=$?
    unset SIM_TC_SCAN_LEGACY
    LOG=$ROOT/out/shots/scan_misses/sim.log
    cp "$LOG" "$B/../scan_misses.$1.log"
    echo "== $1 (shots rc=$rc)"
    grep -E "tagcache: (scan|probe) (done|stopped)|expect_prepares|update_db_wait" "$LOG" || true
    return $rc
}

fail=0
run legacy || true
LEG=$B/../scan_misses.legacy.log
run new || fail=1
NEW=$B/../scan_misses.new.log

# the boot update: legacy reads the tag file for every miss, new does not
python3 - "$LEG" "$NEW" "$N" <<'EOF' || fail=1
import re, sys
pat = re.compile(r'tagcache: (scan|probe) (done|stopped), (\d+) new, (\d+) missed, '
                 r'(\d+) lookups, (\d+) reads, (\d+) master opens')
def rows(p):
    return [(m.group(1), m.group(2)) + tuple(int(x) for x in m.groups()[2:])
            for m in map(pat.search, open(p, errors='replace')) if m]
leg, new, n = rows(sys.argv[1]), rows(sys.argv[2]), int(sys.argv[3])
ok = True
print('legacy:', leg)
print('new:   ', new)
upd = [r for r in new if r[4] > 0]          # scans with lookups
if not upd:
    print('FAIL: no update scan line'); sys.exit(1)
for kind, st, nw, miss, look, reads, opens in upd:
    if reads > look - miss:
        print('FAIL: %s read %d entries for %d hits' % (kind, reads, look - miss)); ok = False
    if kind == 'scan' and opens > 2:
        print('FAIL: candidate scan opened the master %d times' % opens); ok = False
if not any(r[0] == 'probe' and r[2] == 0 for r in upd):
    print('FAIL: no unchanged probe'); ok = False
if not any(r[0] == 'scan' and r[2] >= 1 for r in upd):
    print('FAIL: the added file was not scanned in'); ok = False
lupd = [r for r in leg if r[4] > 0]
if lupd and max(r[5] for r in lupd) <= max(r[5] for r in upd):
    print('FAIL: legacy did not read more'); ok = False
sys.exit(0 if ok else 1)
EOF
if [ $fail = 0 ]; then echo "PASS scan_misses ($N tracks, $J junk)"; else echo "FAIL scan_misses"; exit 1; fi
