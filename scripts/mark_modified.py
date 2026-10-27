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

"""GPL v2 section 2(a): mark every upstream Rockbox file we changed.

Each upstream file that differs from the merge-base with upstream/master
gets one line inside its existing header, right after the last copyright
line, in the header's own comment prefix:

    Modified for ProkBox5g (2026); see docs/changes-from-rockbox.md.

(the span becomes "2026-<year>" as years pass; reruns update it). The
original header lines are never touched. A file with no copyright header
gets the line as its first comment (after a shebang); a format with no
comment syntax we know is listed for a manual decision instead.

Usage:
  scripts/mark_modified.py            # dry run: what would change
  scripts/mark_modified.py --apply    # write the lines
  scripts/mark_modified.py --check    # exit 1 if a changed file lacks it
  scripts/mark_modified.py --ref p12  # another ref than HEAD

docs/legal.md L2 is the obligation; scripts/legal_audit.py item 4 checks it.
"""
import argparse
import os
import re
import subprocess
import sys

MARK = "Modified for ProkBox5g"
MARK_RE = re.compile(r"Modified for ProkBox5g \(2026(?:–\d{4})?\); "
                     r"see docs/changes-from-rockbox\.md\.")
FIRST_YEAR = 2026

# extension or basename -> (line prefix, line suffix) for a fresh comment
C_STYLE = ("/* ", " */")
HASH = ("# ", "")
SYNTAX = {
    ".c": C_STYLE, ".h": C_STYLE, ".S": C_STYLE, ".s": C_STYLE,
    ".cpp": C_STYLE, ".cc": C_STYLE, ".m": C_STYLE, ".lds": C_STYLE,
    ".ld": C_STYLE, ".java": C_STYLE, ".js": C_STYLE, ".css": C_STYLE,
    ".make": HASH, ".mk": HASH, ".sh": HASH, ".py": HASH, ".pl": HASH,
    ".pm": HASH, ".lang": HASH, ".cfg": HASH, ".pro": HASH, ".tcl": HASH,
    ".lua": ("-- ", ""), ".html": ("<!-- ", " -->"), ".xml": ("<!-- ", " -->"),
}
BASENAMES = {
    "Makefile": HASH, "configure": HASH,
    # run through the C preprocessor by the build
    "SOURCES": C_STYLE, "SUBDIRS": C_STYLE, "CATEGORIES": HASH,
    "features.txt": C_STYLE, ".gitignore": HASH, "tagnavi.config": HASH,
}
HEADER_SCAN = 60


def git(*args):
    return subprocess.run(["git"] + list(args), check=True,
                          capture_output=True, text=True).stdout


def notice(year):
    span = str(FIRST_YEAR) if year <= FIRST_YEAR else \
        "%d–%d" % (FIRST_YEAR, year)
    return "%s (%s); see docs/changes-from-rockbox.md." % (MARK, span)


def changed_upstream_files(ref):
    base = git("merge-base", ref, "upstream/master").strip()
    files = []
    binary = set()
    for line in git("diff", "--numstat", "-M", base, ref).splitlines():
        added, deleted, path = line.split("\t", 2)
        if added == "-":
            binary.add(path.split(" => ")[-1].strip("{}"))
    for line in git("diff", "--name-status", "-M", base, ref).splitlines():
        parts = line.split("\t")
        status = parts[0]
        if status == "M" or status.startswith("R"):
            files.append(parts[-1])
    return (base, [f for f in files if f not in binary],
            sorted(binary.intersection(files)))


def syntax_for(path):
    name = os.path.basename(path)
    if name in BASENAMES:
        return BASENAMES[name]
    return SYNTAX.get(os.path.splitext(name)[1])


def plan_file(path, text):
    """Return (kind, new_text) where kind is one of: ok, update, header,
    top, manual."""
    lines = text.split("\n")
    year = PLAN_YEAR
    want = notice(year)
    for i, l in enumerate(lines[:HEADER_SCAN * 2]):
        if MARK in l:
            new = MARK_RE.sub(want, l)
            if new == l:
                return "ok", None
            lines[i] = new
            return "update", "\n".join(lines)
    # last copyright line of the leading header comment
    last = None
    for i, l in enumerate(lines[:HEADER_SCAN]):
        if re.search(r"copyright", l, re.I):
            last = i
        elif last is not None and ("*/" in l or not l.strip()
                                   or not re.match(r"\s*(\*|#|//|;|--|dnl)", l)):
            break
    if last is not None:
        l = lines[last]
        prefix = l[:re.search(r"copyright", l, re.I).start()]
        if not re.fullmatch(r"\s*(/?\*+|#+|//|;+|--|dnl)?\s*", prefix):
            prefix = re.match(r"\s*(\*|#|//|;|--|dnl)?\s*", l).group(0)
        lines.insert(last + 1, prefix + want)
        return "header", "\n".join(lines)
    syn = syntax_for(path)
    if syn is None:
        return "manual", None
    at = 1 if lines and lines[0].startswith("#!") else 0
    lines.insert(at, syn[0] + want + syn[1])
    return "top", "\n".join(lines)


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--ref", default="HEAD")
    ap.add_argument("--apply", action="store_true")
    ap.add_argument("--check", action="store_true")
    ap.add_argument("-v", "--verbose", action="store_true")
    a = ap.parse_args()

    global PLAN_YEAR
    PLAN_YEAR = int(git("log", "-1", "--format=%cd", "--date=format:%Y",
                        a.ref).strip())
    os.chdir(git("rev-parse", "--show-toplevel").strip())
    if a.apply and a.ref != "HEAD":
        sys.exit("--apply works on the checked-out tree only (--ref HEAD)")

    base, files, binary = changed_upstream_files(a.ref)
    kinds = {"ok": [], "update": [], "header": [], "top": [], "manual": []}
    for f in files:
        try:
            text = git("show", "%s:%s" % (a.ref, f)) if a.ref != "HEAD" \
                else open(f, encoding="utf-8", newline="").read()
        except (UnicodeDecodeError, subprocess.CalledProcessError):
            kinds["manual"].append(f)
            continue
        kind, new = plan_file(f, text)
        kinds[kind].append(f)
        if a.apply and new is not None:
            with open(f, "w", encoding="utf-8", newline="") as fh:
                fh.write(new)

    print("# merge-base %s, %d changed upstream text files, %d binary "
          "(not marked; listed in docs/changes-from-rockbox.md)"
          % (base[:10], len(files), len(binary)))
    labels = {"ok": "already marked", "update": "year span updated",
              "header": "line added after the copyright",
              "top": "no copyright header: line added as first comment",
              "manual": "no known comment syntax: decide by hand"}
    for k in ("ok", "update", "header", "top", "manual"):
        print("%-55s %4d" % (labels[k], len(kinds[k])))
        if kinds[k] and (a.verbose or k in ("top", "manual")):
            for f in kinds[k]:
                print("    " + f)
    if not a.apply and not a.check:
        print("dry run; --apply writes")
    if a.check:
        missing = kinds["update"] + kinds["header"] + kinds["top"]
        if missing or kinds["manual"]:
            print("MODNOTICE FAILED: %d unmarked, %d manual"
                  % (len(missing), len(kinds["manual"])))
            return 1
        print("MODNOTICE PASSED")
    return 0


if __name__ == "__main__":
    sys.exit(main())
