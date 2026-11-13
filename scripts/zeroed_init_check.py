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

"""No initialiser in a section the boot zeroes (#313).

usage: scripts/zeroed_init_check.py [--sections LIST] BUILD_DIR...

SHAREDBSS_ATTR / NOCACHEBSS_ATTR / XCORE_BSS put a variable in .ncbss, and
IBSS_ATTR in .ibss.  GCC does not treat those names as BSS, so a variable
given a non-zero initialiser there compiles without a word: the object's
section carries the bytes, but the linker script makes the output section
NOLOAD and crt0 zeroes it, so the variable boots as 0
(wmcodec_gate_mask = DEV_EXTCLOCKS | DEV_I2S, the AW-5 gate that never
closed).  An initialised variable belongs in .ncdata (SHAREDDATA_ATTR,
NOCACHEDATA_ATTR, XCORE_DATA) or .idata (IDATA_ATTR).

This reads every ARM object (*.o) under each BUILD_DIR and fails on any
zeroed section (.ncbss*, .ibss*; --sections a,b to change) that holds a
non-zero byte or a relocation (a pointer initialiser is zero bytes plus a
relocation), naming the symbol, its object and the bytes.

Exit 0: objects read, none found (the count is printed); 1: found; 2: it
could not run (no build dir, no objects, no zeroed section in any object:
a scan that saw nothing must not pass)."""
import os
import struct
import sys

DEFAULT_SECTIONS = (".ncbss", ".ibss")

# Known findings that are harmless, each with its reason: (object path
# suffix, symbol) -> reason.  A listed finding is printed as "allowed", not
# counted; a stale entry (nothing matches it) fails the check, so the list
# cannot outlive its code.
ALLOWED = {
    ("apps/plugins/zxbox/spmain.o", "showframe"):
        "upstream plugin, stock fw only (no shell build has plugins); "
        "spmain.c:121 sets it from the settings before first use; "
        "an upstream backport candidate (docs/upstream-backports.md)",
}
SHT_PROGBITS, SHT_SYMTAB, SHT_NOBITS, SHT_REL, SHT_RELA = 1, 2, 8, 9, 4


def zeroed(name, prefixes):
    return any(name == p or name.startswith(p + ".") for p in prefixes)


def scan(path, prefixes):
    """(sections seen, findings) for one object; None if not ELF32 LE."""
    with open(path, "rb") as f:
        data = f.read()
    if data[:4] != b"\x7fELF" or data[4] != 1 or data[5] != 1:
        return None
    shoff, = struct.unpack_from("<I", data, 0x20)
    shentsize, shnum, shstrndx = struct.unpack_from("<HHH", data, 0x2E)
    secs = []
    for i in range(shnum):
        secs.append(struct.unpack_from("<IIIIIIIIII", data,
                                       shoff + i * shentsize))
    strtab = secs[shstrndx]

    def cstr(base, off):
        end = data.index(b"\0", base + off)
        return data[base + off:end].decode("latin-1")

    names = [cstr(strtab[4], s[0]) for s in secs]
    targets = {i for i, n in enumerate(names) if zeroed(n, prefixes)}
    if not targets:
        return 0, []

    # symbols, to name what sits at an offset
    syms = {}
    for s in secs:
        if s[1] != SHT_SYMTAB:
            continue
        link = secs[s[6]]
        for k in range(s[5] // 16):
            st_name, value, size, info, other, shndx = struct.unpack_from(
                "<IIIBBH", data, s[4] + k * 16)
            if shndx in targets and st_name and (info & 0xF) in (0, 1):
                name = cstr(link[4], st_name)
                if not name.startswith("$"):    # ARM mapping symbols
                    syms.setdefault(shndx, []).append((value, size, name))

    def who(idx, off):
        for value, size, name in syms.get(idx, []):
            if value <= off < value + max(size, 1):
                return name
        return "%s+0x%x" % (names[idx], off)

    # one finding a symbol: a relocation (a pointer initialiser; ARM REL
    # keeps its addend in the bytes too) says more than its bytes
    found = {}
    for idx in sorted(targets):
        s = secs[idx]
        if s[1] == SHT_NOBITS:
            continue
        blob = data[s[4]:s[4] + s[5]]
        hits = {}
        for off, b in enumerate(blob):
            if b:
                hits.setdefault(who(idx, off), []).append(off)
        for name, offs in hits.items():
            found[(idx, name)] = (path, name, "%s: %s in %s holds a non-zero initialiser "
                                  "(%d byte(s) from offset 0x%x)"
                                  % (path, name, names[idx], len(offs),
                                     offs[0]))
    for i, s in enumerate(secs):
        if s[1] in (SHT_REL, SHT_RELA) and s[7] in targets:
            ent = 8 if s[1] == SHT_REL else 12
            for k in range(s[5] // ent):
                off, = struct.unpack_from("<I", data, s[4] + k * ent)
                name = who(s[7], off)
                found[(s[7], name)] = (path, name, "%s: %s in %s holds an address "
                                       "initialiser (a relocation at 0x%x)"
                                       % (path, name, names[s[7]], off))
    return len(targets), [found[k] for k in sorted(found)]


def main(argv):
    prefixes = DEFAULT_SECTIONS
    args = argv[1:]
    if len(args) >= 2 and args[0] == "--sections":
        prefixes = tuple(p for p in args[1].split(",") if p)
        args = args[2:]
    if not args:
        sys.stderr.write(__doc__)
        return 2
    objs = secs = 0
    findings = []
    for d in args:
        if not os.path.isdir(d):
            print("zeroed-init: FAILED TO RUN: no build dir %s" % d)
            return 2
        for root, _dirs, files in os.walk(d):
            for fn in files:
                if not fn.endswith(".o"):
                    continue
                r = scan(os.path.join(root, fn), prefixes)
                if r is None:
                    continue
                objs += 1
                secs += r[0]
                findings += r[1]
    if objs == 0 or secs == 0:
        print("zeroed-init: FAILED TO RUN: %d ARM objects, %d zeroed "
              "sections (%s) under %s" % (objs, secs, ",".join(prefixes),
                                          " ".join(args)))
        return 2
    bad = []
    used = set()
    for path, sym, text in findings:
        key = next((k for k in ALLOWED
                    if path.endswith("/" + k[0]) and sym == k[1]), None)
        if key:
            used.add(key)
            print("zeroed-init: allowed: %s (%s)" % (text, ALLOWED[key]))
        else:
            bad.append(text)
            print("zeroed-init: " + text)
    stale = [k for k in ALLOWED if k not in used and any(
        os.path.exists(os.path.join(d, k[0])) for d in args)]
    for k in stale:
        print("zeroed-init: STALE allow entry %s %s: its object no longer "
              "holds that initialiser; remove the entry" % k)
    print("zeroed-init: %d objects, %d zeroed sections (%s): %d with an "
          "initialiser, %d allowed" % (objs, secs, ",".join(prefixes),
                                       len(bad), len(findings) - len(bad)))
    return 1 if bad or stale else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
