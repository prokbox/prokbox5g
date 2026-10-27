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

# gate_mutants.sh: verification of docs/gate-speed-brief.md section 8.2. Installs
# the script mutants of tests/gate/mutants as tests/ui/ipodui-zz-mut-*.script
# (plus goldens where they need one), runs the CLASSIC gate and the NEW gate
# (GATE_POOL=1) on just those scripts (--quick, GATE_ONLY: a reduced mode used
# only here), removes them again, and prints which rule caught each mutant in
# each gate. Every mutant but the control must FAIL in both; the control must
# pass in both. Logs: build/mutants-old.log, build/mutants-new.log.
set -u
ROOT=$(cd "$(dirname "$0")/.." && pwd)
cd "$ROOT" || exit 1
M=tests/gate/mutants
install_all() {
    for f in $M/m-*.script; do
        n=$(basename "$f" .script); n=${n#m-}
        cp "$f" tests/ui/ipodui-zz-mut-$n.script
        case "$n" in
            control)       mkdir -p tests/ui/golden/ipodui-zz-mut-$n; cp $M/golden-wrong/boot-real.png tests/ui/golden/ipodui-zz-mut-$n/boot.png ;;
            golden|golden-region) mkdir -p tests/ui/golden/ipodui-zz-mut-$n; cp $M/golden-wrong/wrong.png tests/ui/golden/ipodui-zz-mut-$n/boot.png ;;
            noshot)        mkdir -p tests/ui/golden/ipodui-zz-mut-$n; cp $M/golden-wrong/boot-real.png tests/ui/golden/ipodui-zz-mut-$n/ghost.png ;;
        esac
    done
}
remove_all() {
    rm -f tests/ui/ipodui-zz-mut-*.script
    rm -rf tests/ui/golden/ipodui-zz-mut-* out/shots/ipodui-zz-mut-*
}
trap remove_all EXIT
remove_all; install_all
export GATE_ONLY='ipodui-zz-mut-*'
export GATE_MAX_MINUTES=60
echo "== classic gate"; scripts/validate-classic.sh --quick >build/mutants-old.log 2>&1; echo "exit $?"
echo "== new gate (GATE_POOL=1)"; GATE_POOL=1 scripts/validate.sh --quick >build/mutants-new.log 2>&1; echo "exit $?"
echo; echo "per-mutant verdicts (old | new):"
python3 - <<'PY'
import subprocess, sys
sys.path.insert(0, 'scripts')
import gate_verdicts as gv
uo, oo, fo, _ = gv.parse('build/mutants-old.log')
un, on, fn, _ = gv.parse('build/mutants-new.log')
names = sorted(set(uo) | set(un))
bad = 0
for n in names:
    a = uo.get(n, ['missing', ''])
    b = un.get(n, ['missing', ''])
    want = 'ok' if n.endswith('-control') else 'FAIL'
    na = 'ok' if a[0] == 'flaky' else a[0]
    nb = 'ok' if b[0] == 'flaky' else b[0]
    good = na == want and nb == want
    bad += not good
    print('%s %-34s old=%-5s new=%-5s | %s' % ('ok  ' if good else 'BAD ', n, a[0], b[0], (b[1] or a[1])[:90]))
print('final: old', fo, 'new', fn)
print('MUTANTS %s' % ('ALL CAUGHT IDENTICALLY' if not bad and set(uo) == set(un) else 'MISMATCH'))
sys.exit(1 if bad else 0)
PY
