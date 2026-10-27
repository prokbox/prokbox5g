#!/usr/bin/env python3
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

"""Rename the shell's internal "ipodui" names to the ProkBox brand (package F).

Owner, 2026-10-24 (docs/legal.md section 1, "Internal names"):

    HAVE_IPOD_UI*   -> HAVE_PROKBOX_UI*     LANG_IPODUI_*  -> LANG_PB_*
    IPODUI*         -> PROKBOX_UI*          --ipodui*      -> --prokbox-ui*
    ipodui          -> prokbox              "iPod UI shell" -> "ProkBox shell"

and every tracked path component "ipodui" -> "prokbox" (apps/ipodui/,
tests/ipodui/, tests/ui/ipodui-*.script, golden dirs). Upstream's hardware
names (ipod targets, IPOD_VIDEO, ipodpatcher) don't match and stay.

String literals change too, so names stored on a unit change with them
(the data folder under the hidden folder, the log file, settings' config
names). The owner chose to migrate those on the first boot of the renamed
build: the report's "on-device names" section is the exact old -> new list
the migration needs (folder rename, old config names read once as
aliases, log restarted under the new name).

Usage:
  scripts/rename_shell.py                 # dry run, report to build/rename-shell-report.txt
  scripts/rename_shell.py --apply         # git mv + rewrite (run on a clean tree)
After --apply: reconfigure every build dir with --prokbox-ui, then
scripts/validate.sh must print GATE PASSED before the commit.
"""
import argparse
import collections
import os
import re
import subprocess
import sys

RULES = [
    ("LANG_IPODUI_", "LANG_PB_"),
    ("HAVE_IPOD_UI", "HAVE_PROKBOX_UI"),
    ("IPODUI", "PROKBOX_UI"),
    ("--ipodui", "--prokbox-ui"),
    ("ipodui", "prokbox"),
    ("iPod UI shell", "ProkBox shell"),
]
RULE_RE = re.compile("|".join(re.escape(a) for a, _ in RULES))
RULE_MAP = dict(RULES)
C_EXT = {".c", ".h", ".S", ".cpp", ".m"}
STRING_RE = re.compile(r'"(?:[^"\\\n]|\\.)*"')
# literal shapes that name something stored on the unit
ONDISK_RE = re.compile(r'ROCKBOX_DIR\s*"[^"]*ipodui|"/?ipodui[/"]|ipodui\.log|'
                       r'"ipodui [^"]*"|\.rockbox/ipodui')
IDENT_RE = re.compile(r"[A-Za-z_][A-Za-z0-9_]*")


def git(*args, check=True):
    return subprocess.run(["git"] + list(args), check=check,
                          capture_output=True, text=True).stdout


def rewrite(text, counts):
    def sub(m):
        counts[m.group(0)] += 1
        return RULE_MAP[m.group(0)]
    return RULE_RE.sub(sub, text)


def new_path(p):
    return "/".join(c.replace("ipodui", "prokbox") for c in p.split("/"))


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--apply", action="store_true")
    ap.add_argument("--report", default="build/rename-shell-report.txt")
    a = ap.parse_args()
    os.chdir(git("rev-parse", "--show-toplevel").strip())

    if a.apply and git("status", "--porcelain", "--untracked-files=no").strip():
        sys.exit("tracked changes present: commit or stash first")

    tracked = git("ls-files").splitlines()
    text_files = set(git("grep", "-I", "-l", "", check=False).splitlines())
    hits = git("grep", "-I", "-l", "-e", "ipodui", "-e", "IPODUI",
               "-e", "HAVE_IPOD_UI", "-e", "iPod UI shell",
               check=False).splitlines()

    moves = [(p, new_path(p)) for p in tracked if "ipodui" in p]
    tracked_set = set(tracked)
    clashes = [n for _, n in moves if n in tracked_set or os.path.exists(n)]

    counts = collections.Counter()
    per_file = {}
    literals = collections.Counter()
    ondisk = collections.Counter()
    new_text = {}
    old_idents, new_idents = set(), set()
    for f in hits:
        if f not in text_files:
            continue
        with open(f, encoding="utf-8", errors="surrogateescape",
                  newline="") as fh:
            t = fh.read()
        c = collections.Counter()
        n = rewrite(t, c)
        if n == t:
            continue
        per_file[f] = sum(c.values())
        counts.update(c)
        new_text[f] = n
        if os.path.splitext(f)[1] in C_EXT:
            for s in STRING_RE.findall(t):
                if RULE_RE.search(s) and not s.endswith('.h"') \
                        and "#include" not in s:
                    ns = rewrite(s, collections.Counter())
                    (ondisk if ONDISK_RE.search(s) else literals)[(s, ns)] += 1
        old_idents.update(i for i in IDENT_RE.findall(t)
                          if RULE_RE.search(i))
    # a renamed identifier must not already exist with another meaning
    all_new = {rewrite(i, collections.Counter()) for i in old_idents}
    present = set(git("grep", "-I", "-h", "-o", "-w", "-E",
                      r"[A-Za-z0-9_]*(PROKBOX_UI|LANG_PB_|prokbox)[A-Za-z0-9_]*",
                      "--", "*.c", "*.h", "*.S", "*.make", "*Makefile",
                      "*SOURCES", "apps/features.txt", "tools/configure",
                      "*.lang", check=False).split())
    existing = all_new & present
    # the bare word also appears in file names (prokbox-header.txt,
    # prokbox-convert.py); only a use as a name of its own clashes
    if "prokbox" in existing and not re.search(
            r"\bprokbox\b(?![-.])",
            git("grep", "-I", "-h", "-w", "prokbox", "--", "*.c", "*.h",
                "*.S", "*.make", "*SOURCES", "apps/features.txt",
                "tools/configure", "*.lang", check=False)):
        existing.discard("prokbox")

    os.makedirs(os.path.dirname(a.report) or ".", exist_ok=True)
    with open(a.report, "w") as r:
        r.write("# rename_shell.py report (%s)\n\n" %
                ("APPLIED" if a.apply else "dry run"))
        r.write("## Rules\n")
        for old, new in RULES:
            r.write("%7d  %s -> %s\n" % (counts[old], old, new))
        r.write("\n## Path moves (%d)\n" % len(moves))
        for o, n in moves:
            r.write("%s -> %s\n" % (o, n))
        r.write("\n## Clashes: target path exists (%d)\n" % len(clashes))
        r.write("".join(c + "\n" for c in clashes))
        r.write("\n## Clashes: new identifier already in the tree (%d)\n"
                % len(existing))
        r.write("".join(sorted(e + "\n" for e in existing)))
        r.write("\n## On-device names: the first-boot migration list (%d)\n"
                % len(ondisk))
        for (o, n), k in sorted(ondisk.items()):
            r.write("%s -> %s   (x%d)\n" % (o, n, k))
        r.write("\n## Other changed C string literals (%d; log prefixes etc.)\n"
                % len(literals))
        for (o, n), k in sorted(literals.items()):
            r.write("%s -> %s   (x%d)\n" % (o, n, k))
        r.write("\n## Files (%d)\n" % len(per_file))
        for f, k in sorted(per_file.items(), key=lambda x: -x[1]):
            r.write("%6d  %s\n" % (k, f))

    print("rules: " + ", ".join("%s x%d" % (o, counts[o]) for o, _ in RULES))
    print("%d files rewritten, %d paths moved, %d on-device names, "
          "%d other literals" % (len(per_file), len(moves), len(ondisk),
                                 len(literals)))
    print("clashes: %d paths, %d identifiers" % (len(clashes), len(existing)))
    print("report: " + a.report)
    if clashes or existing:
        print("RENAME BLOCKED: resolve the clashes first")
        return 1
    if not a.apply:
        print("dry run; --apply moves and rewrites")
        return 0

    for o, n in moves:
        os.makedirs(os.path.dirname(n) or ".", exist_ok=True)
        subprocess.run(["git", "mv", o, n], check=True)
    for f, t in new_text.items():
        with open(new_path(f), "w", encoding="utf-8",
                  errors="surrogateescape", newline="") as fh:
            fh.write(t)
    left = git("grep", "-I", "-n", "-i", "-e", "ipodui", "-e", "ipod_ui",
               check=False)
    print("applied. Leftovers (case-insensitive ipodui/ipod_ui): %d lines"
          % len(left.splitlines()))
    print("next: add the first-boot migration for the on-device names, "
          "reconfigure build dirs with --prokbox-ui, run scripts/validate.sh")
    return 0


if __name__ == "__main__":
    sys.exit(main())
