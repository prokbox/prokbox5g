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

# Phase 9 GC cross-check (docs/phase9-brief.md 3.3.1, 7.9, Rev 1 L8).
# Mandatory at hand-back for any change to the core section flags.
#
# What the linker's --gc-sections drops from a --core-sections build must be
# dead according to scripts/risk/reach.py, which sees references the linker
# does not (literal words equal to a function start, ADR, veneers). A dropped
# symbol that reach.py calls reachable FAILS the check.
#
#   scripts/risk/gc_check.sh [BUILD_DIR] [--out DIR] [--reach PATH]
#
#   BUILD_DIR   a configured target build with core sections on
#               (default build/fw-ui; its Makefile must export a non-empty
#               CORE_SECTIONS_CFLAGS)
#   --out DIR   where the artifacts go (default out/p9/gc_check)
#   --reach P   reach.py to use (default scripts/risk/reach.py)
#
# Steps (one build dir, no compile: both are relinks of the same objects):
#   1. link without --gc-sections (make CORE_GC_OFF=1): the reference image,
#      every function and datum in its own section, nothing dropped, no
#      section anchors, so reach.py sees per-symbol references;
#   2. relink normally with --print-gc-sections (make CORE_GC_PRINT=1); this
#      also leaves the build dir with its normal image again;
#   3. map each dropped input section to the symbols it held in the
#      reference image (map file + nm), and check them against
#      reach.py --reachable over the reference image;
#   4. boot anchors: start, cop_main, irq_handler, fiq_handler, main,
#      _initstart/_initend and .vectors are present, and the osos signature
#      ("Rockbox" + version 1, crt0-pp.S) is at the same offset from start
#      in both images.
#
# Output: OUT/gc-sections.txt (linker list), OUT/dropped.txt (symbol, kind,
# size, section, object), OUT/report.txt (summary + every FAIL), and the
# last lines on stdout:
#   gc_check PASS dropped=<n> bytes=<b> reach_conflicts=0
#   gc_check FAIL ...            (exit 1)
#   gc_check INCOMPLETE ...      (exit 3: reach.py not implemented; the
#                                 anchor checks still ran)
set -u
cd "$(dirname "$0")/../.."
ROOT=$(pwd)

B=build/fw-ui
OUT=out/p9/gc_check
REACH=scripts/risk/reach.py
while [ $# -gt 0 ]; do
    case "$1" in
    --out) OUT=$2; shift 2 ;;
    --reach) REACH=$2; shift 2 ;;
    -h|--help) awk 'NR==1{next} /^set -u/{print; exit} /^#\*+$/{b=!b; next} !b' "$0" | sed 's/^# \{0,1\}//'; exit 0 ;;
    *) B=$1; shift ;;
    esac
done
case "$B" in /*) ;; *) B=$ROOT/$B ;; esac
case "$OUT" in /*) ;; *) OUT=$ROOT/$OUT ;; esac

if [ ! -f "$B/Makefile" ]; then
    echo "gc_check: $B is not a configured build dir" >&2; exit 2
fi
secflags=$(sed -n 's/^export CORE_SECTIONS_CFLAGS=//p' "$B/Makefile")
if [ -z "$secflags" ]; then
    echo "gc_check: $B has no core sections (configure --core-sections=off" \
         "or not --ipodui firmware): nothing to check" >&2
    exit 2
fi
mkdir -p "$OUT"
JOBS=${JOBS:-2}

echo "gc_check: $B ($secflags)"
# 1. reference link, no --gc-sections
rm -f "$B/rockbox.elf"
if ! make -C "$B" -j"$JOBS" CORE_GC_OFF=1 bin > "$OUT/link-nogc.log" 2>&1; then
    tail -20 "$OUT/link-nogc.log"
    echo "gc_check FAIL reference link (no --gc-sections) failed"; exit 1
fi
cp "$B/rockbox.elf" "$OUT/nogc.elf"
cp "$B/rockbox.map" "$OUT/nogc.map"

# 2. the real link, listing what it drops (restores the normal image)
rm -f "$B/rockbox.elf"
if ! make -C "$B" -j"$JOBS" CORE_GC_PRINT=1 bin > "$OUT/link-gc.log" 2>&1; then
    tail -20 "$OUT/link-gc.log"
    echo "gc_check FAIL normal link failed"; exit 1
fi
grep "removing unused section" "$OUT/link-gc.log" > "$OUT/gc-sections.txt"
cp "$B/rockbox.elf" "$OUT/gc.elf"

# 3. reachability over the reference image
reach_rc=0
python3 -I "$ROOT/$REACH" "$OUT/nogc.elf" --map "$OUT/nogc.map" --reachable \
    > "$OUT/reach.txt" 2> "$OUT/reach.err" || reach_rc=$?

# 3 + 4. compare
python3 -I - "$OUT" "$reach_rc" <<'EOF'
import re, subprocess, sys
out, reach_rc = sys.argv[1], int(sys.argv[2])

def nm(elf):
    syms = []
    r = subprocess.run(["arm-none-eabi-nm", "-S", "-n", elf],
                       capture_output=True, text=True, check=True)
    for line in r.stdout.splitlines():
        f = line.split()
        if len(f) == 4:
            syms.append((int(f[0], 16), int(f[1], 16), f[2], f[3]))
        elif len(f) == 3:
            syms.append((int(f[0], 16), 0, f[1], f[2]))
    return syms

def map_sections(path):
    """(section, object) -> (addr, size) for every input section placed."""
    res = {}
    lines = open(path, errors="replace").read().split("\n")
    try:
        i = lines.index("Linker script and memory map")
    except ValueError:
        i = 0
    pend = None
    full = re.compile(r"^ (\S+)\s+0x([0-9a-f]+)\s+0x([0-9a-f]+)\s+(\S.*)$")
    tail = re.compile(r"^\s+0x([0-9a-f]+)\s+0x([0-9a-f]+)\s+(\S.*)$")
    for line in lines[i:]:
        m = full.match(line)
        if m and not m.group(1).startswith("*"):
            res[(m.group(1), m.group(4).strip())] = (int(m.group(2), 16),
                                                   int(m.group(3), 16))
            pend = None
            continue
        if pend:
            m = tail.match(line)
            if m:
                res[(pend, m.group(3).strip())] = (int(m.group(1), 16),
                                                 int(m.group(2), 16))
            pend = None
            continue
        m = re.match(r"^ (\.\S+)$", line)
        if m:
            pend = m.group(1)
    return res

gcre = re.compile(r"removing unused section '([^']+)' in file '([^']+)'")
dropped = []
for line in open(out + "/gc-sections.txt"):
    m = gcre.search(line)
    # non-allocated sections (debug info of assembler objects, comments,
    # attributes) occupy no image or RAM bytes and have no symbols
    if m and not m.group(1).startswith((".debug", ".comment",
                                        ".ARM.attributes", ".note")):
        dropped.append((m.group(1), m.group(2)))

secs = map_sections(out + "/nogc.map")
ref = nm(out + "/nogc.elf")
gc = nm(out + "/gc.elf")
gcnames = {s[3] for s in gc}

rows, total, unmapped = [], 0, 0
for sec, obj in dropped:
    loc = secs.get((sec, obj))
    if not loc:
        unmapped += 1
        rows.append(("?", "?", 0, sec, obj))
        continue
    a, sz = loc
    total += sz
    names = [s for s in ref if a <= s[0] < a + max(sz, 1)
             and s[2] in "TtDdBbRrWwVv" and not s[3].startswith("$")]
    if not names:
        rows.append(("-", "-", sz, sec, obj))
    for s in names:
        rows.append((s[3], s[2], s[1], sec, obj))

with open(out + "/dropped.txt", "w") as f:
    for r in rows:
        f.write("%s %s %d %s %s\n" % r)

fails = []
# reach.py cross-check (a static of the same name kept in another object is
# not a conflict: the dropped copy is the unreferenced one)
reach_state = "ok"
if reach_rc == 2:
    reach_state = "not-implemented"
elif reach_rc != 0:
    reach_state = "error(%d)" % reach_rc
else:
    reach = set()
    for line in open(out + "/reach.txt"):
        f = line.split()
        if len(f) >= 2 and f[0] == "reach":
            reach.add(f[1])
    for r in rows:
        if r[0] in reach and r[0] not in gcnames:
            fails.append("FAIL reachable-but-dropped %s (%s, %s)" %
                         (r[0], r[3], r[4]))

# boot anchors
for need in ("start", "cop_main", "irq_handler", "fiq_handler", "main",
             "_initstart", "_initend", "_vectorsstart"):
    if need not in gcnames:
        fails.append("FAIL anchor missing: %s" % need)

def sig_offset(elf):
    syms = {s[3]: s[0] for s in nm(elf)}
    tmp = elf + ".text.bin"
    subprocess.run(["arm-none-eabi-objcopy", "-O", "binary", "-j",
                    ".text", elf, tmp], check=True)
    i = open(tmp, "rb").read().find(b"Rockbox\x01")
    return (i - syms.get("start", 0)) if i >= 0 else None

so_ref, so_gc = sig_offset(out + "/nogc.elf"), sig_offset(out + "/gc.elf")
if so_gc is None or so_ref != so_gc:
    fails.append("FAIL osos signature offset ref=%s gc=%s" % (so_ref, so_gc))

kinds = {}
for (sec, obj) in dropped:
    loc = secs.get((sec, obj))
    k = sec.split(".")[1] if sec.startswith(".") else sec
    kinds[k] = kinds.get(k, 0) + (loc[1] if loc else 0)

with open(out + "/report.txt", "w") as f:
    f.write("dropped sections=%d bytes=%d unmapped=%d\n" %
            (len(dropped), total, unmapped))
    f.write("by kind: %s\n" % " ".join("%s=%d" % kv for kv in
                                       sorted(kinds.items())))
    f.write("osos signature offset from start: %s\n" % so_gc)
    f.write("reach.py: %s\n" % reach_state)
    for x in fails:
        f.write(x + "\n")
print(open(out + "/report.txt").read(), end="")
if fails:
    print("gc_check FAIL dropped=%d bytes=%d conflicts=%d" %
          (len(dropped), total, len(fails)))
    sys.exit(1)
if reach_state != "ok":
    print("gc_check INCOMPLETE dropped=%d bytes=%d reach.py=%s "
          "(anchor checks passed)" % (len(dropped), total, reach_state))
    sys.exit(3)
print("gc_check PASS dropped=%d bytes=%d reach_conflicts=0" %
      (len(dropped), total))
EOF
