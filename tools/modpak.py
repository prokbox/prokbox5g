#!/usr/bin/env python3
#             __________               __   ___.
#   Open      \______   \ ____   ____ |  | _\_ |__   _______  ___
#   Source     |       _//  _ \_/ ___\|  |/ /| __ \ /  _ \  \/  /
#   Jukebox    |    |   (  <_> )  \___|    < | \_\ (  <_> > <  <
#   Firmware   |____|_  /\____/ \___  >__|_ \|___  /\____/__/\_ \
#                     \/            \/     \/    \/            \/
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
"""ProkBox shell modules (#106): build-time half of apps/ipodui/modload.c.

  modpak.py keep -o keep.txt --nm NM obj...
      --undefined=SYM for every symbol the module objects use but do not
      define, so the core's --gc-sections link keeps it.

  modpak.py pack -o modules.pak --core rockbox.elf --base B --delta D \
                 --nm NM --objcopy OC [--manifests DIR] id:name:a.elf:b.elf ...
      Each module is linked at B (a.elf) and at B + D (b.elf). Words of the
      image that differ by exactly D are its relocations; any other
      difference is an error. Writes the pack (format in modload_core.h).
      The build ID is a CRC-32 over the core's loaded image (objcopy -O
      binary, the ID word zeroed) and every module body, so any change of
      code, data, struct layout or LANG_* values changes it.
      --arena N fails the build if the modules' footprints exceed N.
      --manifests DIR[:DIR...]: each module's labels (class, minimum RAM,
      kind, ABI, exit policy, work, borrow) come from DIR/<name>.mod (or
      DIR/<name>/<name>.mod, the games' layout) into its v2
      pack entry (docs/phase12-brief.md 2.2); without it, every module is
      a Class A shell module with no working block.

  modpak.py manifest FILE...
      Parses and checks manifests; prints each one's pack-entry labels.

  modpak.py check --nm NM --objcopy OC --base B --arena N --manifests DIR \
                  name:a.elf:b.elf:obj[,obj...] ...
      The class rules (docs/phase12-brief.md 2.3, W1-F); any failure
      refuses the build:
        - the class deny list (DIR/class_a.deny, class_b.deny; one glob a
          line) against every core symbol the module's objects use; a
          'context = sync' module (the indexer) may walk directories;
        - sizes: Class A image <= 32 KB and image + .bss + work <= 56 KB;
          any module's footprint + work <= the arena - MOD_IDX_STAGE;
        - export slot 0 (struct mod_desc) equal to the manifest;
        - SDK modules (kind = sdk, W4-0): an ALLOW list instead of the
          deny list (default deny): every undefined symbol of the module's
          objects must be an entry of --sdk-sym (apps/ipodui/sdk/sdk.sym)
          or a libgcc helper (LIBGCC_GLOBS), and every relocated word of
          the image must point inside its own image + .bss.
      Prints each module's figures and the arena the largest one needs
      (+ the index stage + 25 %).

  modpak.py patchbin --bin rockbox.bin --pak modules.pak
      After the objcopy: writes the pack's build ID into the core's
      mod_core_build_id word, found by its marker (rockbox.elf is never
      changed, so nothing relinks).
"""
import argparse
import struct
import subprocess
import sys
import tempfile
import os
import zlib

MAGIC = 0x444F4D50
VERSION = 2
INDEX = 2048
ALIGN = 2048
MAXMOD = 16
ENT_FMT = "<8I4BH2BI"           # struct modpak_ent, 44 B (modload_core.h)
ENT_SIZE = struct.calcsize(ENT_FMT)
SDK_ABI = (0, 1)                # SDK_ABI_MAJOR, SDK_ABI_MINOR
SDK_EXIT_MS_MAX = 1000
CLASSES = {"A": 0, "B": 1, "C": 2}
KINDS = {"shell": 0, "sdk": 1}
RAMS = {"32": 0, "64": 1}
MANIFEST_KEYS = {"class", "min_ram", "kind", "abi", "exit", "work", "quota",
                 "borrow", "boost", "exports", "context", "state", "strings",
                 "pages"}
STATE_MAX = 4096                # docs/design/sdk.md 4.1: the state record
QUOTA_SDK_DEFAULT = 64          # KB, games (sdk.md 8.4)
# libgcc's helpers, linked statically into an SDK module (sdk.md 11.1)
LIBGCC_GLOBS = ("__aeabi_*", "__gnu_*", "__call_via_*", "_call_via_*",
                "__interwork_*", "__*si3", "__*di3", "__*ti3", "__*si2",
                "__*di2", "__*sf*", "__*df*", "__clz*", "__ctz*", "__ffs*",
                "__popcount*", "__parity*", "__bswap*")
IDX_STAGE = 2048                # MOD_IDX_STAGE (modload_core.h)
HDR_MAGIC = 0x484D4B50          # MOD_HDR_MAGIC
# struct mod_desc, 28 B: ... boost, pages (W3-K), state (SDK_DESC_A in
# apps/ipodui/sdk/sdk.h), name_lang, names_off
DESC_FMT = "<I6BHHHIBBHHH"
A_IMAGE_MAX = 32 * 1024
A_TOTAL_MAX = 56 * 1024
PLACEHOLDER = 0x4D4F4442
MARKER = struct.pack('<II', 0x6B6F7250, PLACEHOLDER)


def run(cmd):
    return subprocess.run(cmd, check=True, capture_output=True,
                          text=True).stdout


def fletcher32(data):
    if len(data) & 1:
        data = data + b"\0"
    s1 = s2 = 0xFFFF
    words = struct.unpack("<%dH" % (len(data) // 2), data)
    for i in range(0, len(words), 359):
        for w in words[i:i + 359]:
            s1 += w
            s2 += s1
        s1 = (s1 & 0xFFFF) + (s1 >> 16)
        s2 = (s2 & 0xFFFF) + (s2 >> 16)
    s1 = (s1 & 0xFFFF) + (s1 >> 16)
    s2 = (s2 & 0xFFFF) + (s2 >> 16)
    return ((s2 << 16) | s1) & 0xFFFFFFFF


def symbols(nm, elf):
    out = {}
    for line in run([nm, elf]).splitlines():
        p = line.split()
        if len(p) == 3:
            out[p[2]] = (int(p[0], 16), p[1])
    return out


def sections(readelf, elf):
    """name -> (addr, offset, size)"""
    out = {}
    for line in run([readelf, "-SW", elf]).splitlines():
        line = line.strip()
        if not line.startswith("["):
            continue
        p = line[line.index("]") + 1:].split()
        if len(p) < 6:
            continue
        try:
            out[p[0]] = (int(p[2], 16), int(p[3], 16), int(p[4], 16))
        except ValueError:
            pass
    return out


def text_bytes(objcopy, elf):
    with tempfile.TemporaryDirectory() as d:
        f = os.path.join(d, "t.bin")
        run([objcopy, "-O", "binary", "--only-section=.text", elf, f])
        with open(f, "rb") as fh:
            return fh.read()


def core_image(objcopy, core):
    """rockbox.bin's bytes, as root.make's objcopy makes them"""
    with tempfile.TemporaryDirectory() as d:
        f = os.path.join(d, "core.bin")
        run([objcopy, "-O", "binary", core, f])
        with open(f, "rb") as fh:
            return fh.read()


def marker_at(img):
    at = img.find(MARKER)
    if at < 0 or img.find(MARKER, at + 1) >= 0:
        sys.exit("modpak: the build ID marker is missing or not unique")
    return at + 4


def build_id(core_img, bodies):
    at = marker_at(core_img)
    img = core_img[:at] + b"\0\0\0\0" + core_img[at + 4:]
    h = zlib.crc32(img)
    for body in bodies:
        h = zlib.crc32(body, h)
    h &= 0xFFFFFFFF
    if h in (0, 0xFFFFFFFF, PLACEHOLDER, 1):
        h ^= 0x5A5A5A5A
    return h


def relocs(a, b, delta, name):
    if len(a) != len(b) or len(a) % 4:
        sys.exit("modpak: %s: images differ in size" % name)
    out = []
    wa = struct.unpack("<%dI" % (len(a) // 4), a)
    wb = struct.unpack("<%dI" % (len(b) // 4), b)
    for i, (x, y) in enumerate(zip(wa, wb)):
        if x == y:
            continue
        if (y - x) & 0xFFFFFFFF != delta:
            sys.exit("modpak: %s: word %d moves by 0x%x, not the link delta"
                     " (a position dependency the loader cannot fix)"
                     % (name, i, (y - x) & 0xFFFFFFFF))
        out.append(i)
    if out and out[-1] > 0xFFFF:
        sys.exit("modpak: %s: image over 256 KB" % name)
    return out


def manifest(path):
    """A .mod manifest (key = value lines, '#' comments) -> a dict of the
    pack-entry labels; exits on a bad key or value (the build refuses)."""
    kv = {}
    with open(path) as fh:
        for n, line in enumerate(fh, 1):
            line = line.split("#", 1)[0].strip()
            if not line:
                continue
            if "=" not in line:
                sys.exit("modpak: %s:%d: not 'key = value'" % (path, n))
            k, v = (x.strip() for x in line.split("=", 1))
            if k not in MANIFEST_KEYS:
                sys.exit("modpak: %s:%d: unknown key %r" % (path, n, k))
            kv[k] = v

    def pick(key, table, default):
        v = kv.get(key, default)
        if v not in table:
            sys.exit("modpak: %s: %s = %r (one of %s)"
                     % (path, key, v, "|".join(table)))
        return table[v]

    def num(key):
        try:
            return int(kv.get(key, "0"), 0)
        except ValueError:
            sys.exit("modpak: %s: %s is not a number" % (path, key))

    m = {"mclass": pick("class", CLASSES, None),
         "min_ram": pick("min_ram", RAMS, "32"),
         "kind": pick("kind", KINDS, "shell"),
         "abi": (0, 0), "exit_policy": 1, "exit_ms": 0,
         "work_kb": (num("work") + 1023) // 1024,
         "quota_kb": num("quota"),
         "state": num("state"),
         "strings": kv.get("strings", ""),
         "borrow_kb": (num("borrow") + 1023) // 1024,
         "boost": {"burst": 0, "session": 1}.get(kv.get("boost", "burst")),
         "exports": kv.get("exports", "").split(),
         "context": kv.get("context", "screen"),
         "pages": num("pages")}
    if m["boost"] is None:
        sys.exit("modpak: %s: boost = burst|session" % path)
    ex = kv.get("exit", "immediate").split()
    if ex[0] == "graceful" and len(ex) == 2 and ex[1].isdigit():
        m["exit_policy"], m["exit_ms"] = 0, int(ex[1])
        if m["exit_ms"] > SDK_EXIT_MS_MAX:
            sys.exit("modpak: %s: exit time over %d ms"
                     % (path, SDK_EXIT_MS_MAX))
    elif ex != ["immediate"]:
        sys.exit("modpak: %s: exit = graceful <ms>|immediate" % path)
    if m["kind"] == KINDS["sdk"]:
        if "quota" not in kv:
            m["quota_kb"] = QUOTA_SDK_DEFAULT
        try:
            maj, mnr = (int(x) for x in kv.get("abi", "").split("."))
        except ValueError:
            sys.exit("modpak: %s: an SDK module needs abi = <major>.<minor>"
                     % path)
        if maj != SDK_ABI[0] or mnr > SDK_ABI[1]:
            sys.exit("modpak: %s: abi %d.%d, the core is %d.%d"
                     % (path, maj, mnr, SDK_ABI[0], SDK_ABI[1]))
        m["abi"] = (maj, mnr)
    elif "abi" in kv or "state" in kv or "strings" in kv:
        sys.exit("modpak: %s: abi, state and strings are for SDK modules"
                 " only" % path)
    if not 0 <= m["state"] <= STATE_MAX:
        sys.exit("modpak: %s: state over %d bytes" % (path, STATE_MAX))
    if m["mclass"] == CLASSES["A"] and m["min_ram"] != RAMS["32"]:
        sys.exit("modpak: %s: a Class A module runs on 32 MB units" % path)
    if m["borrow_kb"] and m["mclass"] != CLASSES["B"]:
        sys.exit("modpak: %s: only Class B borrows" % path)
    if m["pages"] and (m["mclass"] != CLASSES["A"] or m["pages"] > 30):
        sys.exit("modpak: %s: pages = <page loads a second, 1..30>, Class A"
                 % path)
    if m["work_kb"] > 0xFFFF or m["quota_kb"] > 1024:
        sys.exit("modpak: %s: work or quota out of range" % path)
    return m


def ent_bytes(mid, off, image, bss_off, bss, nreloc, nexp, fl, m):
    """One struct modpak_ent (v2)"""
    return struct.pack(ENT_FMT, mid, off, image, bss_off, bss, nreloc, nexp,
                       fl, m["mclass"], m["min_ram"], m["abi"][0],
                       m["abi"][1], m["work_kb"], m["kind"],
                       m["exit_policy"], m["borrow_kb"])


DEFAULT_LABELS = {"mclass": 0, "min_ram": 0, "kind": 0, "abi": (0, 0),
                  "exit_policy": 1, "exit_ms": 0, "work_kb": 0, "state": 0,
                  "strings": "",
                  "quota_kb": 0, "borrow_kb": 0, "boost": 0, "exports": [],
                  "context": "screen", "pages": 0}


def cmd_manifest(args):
    for path in args.files:
        m = manifest(path)
        sys.stdout.write("%s: class=%s min_ram=%s kind=%s abi=%d.%d "
                         "exit=%d/%dms work_kb=%d quota_kb=%d borrow_kb=%d "
                         "boost=%d pages=%d context=%s exports=%s\n"
                         % (os.path.basename(path), "ABC"[m["mclass"]],
                            ("32", "64")[m["min_ram"]],
                            ("shell", "sdk")[m["kind"]], m["abi"][0],
                            m["abi"][1], m["exit_policy"], m["exit_ms"],
                            m["work_kb"], m["quota_kb"], m["borrow_kb"],
                            m["boost"], m["pages"], m["context"],
                            ",".join(m["exports"])))
    return 0


def deny_list(path):
    """A deny list: one glob a line, '#' comments"""
    out = []
    with open(path) as fh:
        for line in fh:
            line = line.split("#", 1)[0].strip()
            if line:
                out.append(line)
    return out


SYNC_ALLOWED = ("opendir", "readdir")


def find_manifest(dirs, name):
    """name.mod in one of dirs ('a:b' or a list): DIR/name.mod, or
    DIR/name/name.mod (apps/ipodui/games/<name>/)"""
    if isinstance(dirs, str):
        dirs = [d for d in dirs.split(":") if d]
    for d in dirs:
        for p in (os.path.join(d, name + ".mod"),
                  os.path.join(d, name, name + ".mod")):
            if os.path.exists(p):
                return p
    sys.exit("modpak: no manifest for %s in %s" % (name, ":".join(dirs)))


def sdk_allow(path):
    """The SDK table's names (sdk.sym: one SDK_SYM(name) a line)"""
    import re
    out = set()
    with open(path) as fh:
        for line in fh:
            mt = re.match(r"\s*SDK_SYM\(\s*(\w+)\s*\)", line)
            if mt:
                out.add(mt.group(1))
    if not out:
        sys.exit("modpak: %s: no SDK_SYM entries" % path)
    out.add("sdk_tab_cell")         # the stub library's own table cell
    return out


def libgcc_helper(sym):
    import fnmatch
    return any(fnmatch.fnmatchcase(sym, g) for g in LIBGCC_GLOBS)


def check_module(name, m, undef, image, bss, foot, desc, deny, arena,
                 allow=None):
    """The class rules for one module; returns a list of failures (empty =
    ok). m: its manifest (manifest()); undef: the core symbols its objects
    use; image, bss, foot: bytes (foot: modpak_footprint()); desc: the 28
    bytes at export slot 0, or None; deny: {class: [globs]}; arena:
    MOD_ARENA_SIZE."""
    import fnmatch
    errs = []
    cls = "ABC"[m["mclass"]]
    if m["kind"] == KINDS["sdk"]:
        # default deny: the SDK table and libgcc, nothing else
        if allow is None:
            errs.append("%s: an SDK module needs the SDK table (--sdk-sym)"
                        % name)
            allow = set()
        for sym in sorted(undef):
            if sym not in allow and not libgcc_helper(sym):
                errs.append("%s: SDK module may not use %s (not in sdk.sym)"
                            % (name, sym))
        undef = ()
    for sym in sorted(undef):
        if m["context"] == "sync" and sym in SYNC_ALLOWED:
            continue
        for g in deny.get(cls, []):
            if fnmatch.fnmatchcase(sym, g):
                errs.append("%s: Class %s may not use %s (deny %s)"
                            % (name, cls, sym, g))
                break
    work = m["work_kb"] * 1024
    if cls == "A" and image > A_IMAGE_MAX:
        errs.append("%s: Class A image %d B > %d" % (name, image,
                                                     A_IMAGE_MAX))
    if cls == "A" and image + bss + work > A_TOTAL_MAX:
        errs.append("%s: Class A image + bss + work %d B > %d"
                    % (name, image + bss + work, A_TOTAL_MAX))
    if foot + work > arena - IDX_STAGE:
        errs.append("%s: needs %d B, the arena leaves %d (MOD_ARENA_SIZE"
                    " %d - the index stage)" % (name, foot + work,
                                                 arena - IDX_STAGE, arena))
    if desc is None or len(desc) != struct.calcsize(DESC_FMT):
        errs.append("%s: no module header at export slot 0" % name)
        return errs
    (magic, amaj, amin, mcl, ram, kind, ex, ex_ms, wk, quota, borrow,
     boost, pages, state, _lang, _names) = struct.unpack(DESC_FMT, desc)
    want = {"magic": HDR_MAGIC, "abi": m["abi"], "class": m["mclass"],
            "min_ram": m["min_ram"], "kind": m["kind"],
            "exit": m["exit_policy"], "exit_ms": m["exit_ms"],
            "work_kb": m["work_kb"], "quota_kb": m["quota_kb"],
            "borrow_kb": m["borrow_kb"], "boost": m["boost"],
            "pages": m["pages"], "state": m["state"]}
    have = {"magic": magic, "abi": (amaj, amin), "class": mcl,
            "min_ram": ram, "kind": kind, "exit": ex, "exit_ms": ex_ms,
            "work_kb": wk, "quota_kb": quota, "borrow_kb": borrow,
            "boost": boost, "pages": pages, "state": state}
    for k in want:
        if want[k] != have[k]:
            errs.append("%s: module header %s=%r, manifest %r"
                        % (name, k, have[k], want[k]))
    return errs


def undefined_syms(nm, objs):
    defined, undef = set(), set()
    for o in objs:
        for line in run([nm, o]).splitlines():
            p = line.split()
            if len(p) == 2 and p[0] == "U":
                undef.add(p[1])
            elif len(p) == 3 and p[1] in "TDRBCGSVW":
                defined.add(p[2])
    return undef - defined


def reloc_strays(img, rel, base, end):
    """Relocated words (link A) that point outside [base, end): an SDK
    module may only hold addresses of its own image and .bss"""
    out = []
    for i in rel:
        w = struct.unpack_from("<I", img, i * 4)[0]
        if not base <= w < end:
            out.append((i, w))
    return out


def cmd_check(args):
    base = int(args.base, 0)
    arena = int(args.arena, 0)
    mdirs = [d for d in args.manifests.split(":") if d]
    allow = sdk_allow(args.sdk_sym) if args.sdk_sym else None
    deny = {}
    for cls in "AB":
        f = os.path.join(mdirs[0], "class_%s.deny" % cls.lower())
        if os.path.exists(f):
            deny[cls] = deny_list(f)
    errs, need = [], 0
    for spec in args.mods:
        name, elf, elf_b, objs = spec.split(":", 3)
        m = manifest(find_manifest(mdirs, name))
        img = text_bytes(args.objcopy, elf)
        rel = relocs(img, text_bytes(args.objcopy, elf_b),
                     int(args.delta, 0), name)
        sym = symbols(args.nm, elf)
        bss = sym["__mod_bss_end"][0] - sym["__mod_bss_start"][0]
        # as cmd_pack (modpak_footprint())
        foot = max(sym["__mod_bss_start"][0] - base + bss, len(img))
        foot = (foot + 3) & ~3
        foot = (foot + ((len(rel) * 2 + 3) & ~3) + 15) & ~15
        desc = None
        if len(img) >= 4:
            at = struct.unpack("<I", img[:4])[0] - base
            if 0 <= at and at + struct.calcsize(DESC_FMT) <= len(img):
                desc = img[at:at + struct.calcsize(DESC_FMT)]
        e = check_module(name, m, undefined_syms(args.nm, objs.split(",")),
                         len(img), bss, foot, desc, deny, arena, allow)
        if m["kind"] == KINDS["sdk"]:
            end = sym["__mod_bss_start"][0] + bss
            for i, w in reloc_strays(img, rel, base, max(end, base
                                                         + len(img))):
                e.append("%s: relocated word %d = 0x%08x points outside the"
                         " module" % (name, i, w))
        errs += e
        need = max(need, foot + m["work_kb"] * 1024)
        sys.stdout.write("modpak check: %-10s class=%s image=%d bss=%d "
                         "work=%d footprint=%d %s\n"
                         % (name, "ABC"[m["mclass"]], len(img), bss,
                            m["work_kb"] * 1024, foot,
                            "ok" if not e else "FAIL"))
    want = ((need + IDX_STAGE) * 5 // 4 + 1023) & ~1023
    sys.stdout.write("modpak check: arena need=%d (largest + stage + 25 %%"
                     " = %d) have=%d\n" % (need + IDX_STAGE, want, arena))
    for e in errs:
        sys.stderr.write("modpak check: %s\n" % e)
    return 1 if errs else 0


def cmd_keep(args):
    defined, undef = set(), set()
    for o in args.objs:
        for line in run([args.nm, o]).splitlines():
            p = line.split()
            if len(p) == 2 and p[0] == "U":
                undef.add(p[1])
            elif len(p) == 3 and p[1] in "TDRBCGSVW":
                defined.add(p[2])
    need = sorted(undef - defined)
    with open(args.o, "w") as fh:
        for s in need:
            fh.write("--undefined=%s\n" % s)
    return 0


def cmd_pack(args):
    base, delta = int(args.base, 0), int(args.delta, 0)
    ents, blobs = [], []
    off = INDEX
    need = largest = 0
    for spec in args.mods:
        mid, name, ea, eb = spec.split(":")
        mid = int(mid)
        a, b = text_bytes(args.objcopy, ea), text_bytes(args.objcopy, eb)
        rel = relocs(a, b, delta, name)
        sym = symbols(args.nm, ea)
        img_end = sym["__mod_image_end"][0]
        if img_end - base != len(a):
            sys.exit("modpak: %s: image %d != linked %d" %
                     (name, len(a), img_end - base))
        bss_off = sym["__mod_bss_start"][0] - base
        bss = sym["__mod_bss_end"][0] - sym["__mod_bss_start"][0]
        nexp = (sym["__mod_exports_end"][0] - base) // 4
        rb = struct.pack("<%dH" % len(rel), *rel)
        rb += b"\0" * ((-len(rb)) % 4)
        body = a + rb
        foot = max(bss_off + bss, len(a))
        foot = (foot + 3) & ~3
        foot = (foot + len(rb) + 15) & ~15
        need += foot
        labels = DEFAULT_LABELS
        if args.manifests:
            labels = manifest(find_manifest(args.manifests, name))
        largest = max(largest, foot + labels["work_kb"] * 1024)
        ents.append(ent_bytes(mid, off, len(a), bss_off, bss, len(rel),
                              nexp, fletcher32(body), labels))
        blobs.append((off, body))
        sys.stdout.write("modpak: %-10s image=%d bss=%d relocs=%d exports=%d"
                         " footprint=%d\n" % (name, len(a), bss, len(rel),
                                              nexp, foot))
        off += len(body)
        off += (-off) % ALIGN
    if len(ents) > MAXMOD:
        sys.exit("modpak: too many modules")
    # P12: one module at a time must fit beside the index stage (the
    # tenants are evictable); modpak.py check holds the per-class rules
    if args.arena and largest > int(args.arena, 0) - IDX_STAGE:
        sys.exit("modpak: a module needs %d B, the arena leaves %d"
                 % (largest, int(args.arena, 0) - IDX_STAGE))
    bid = build_id(core_image(args.objcopy, args.core),
                   [body for _, body in blobs])
    total = blobs[-1][0] + len(blobs[-1][1]) if blobs else INDEX
    hdr = struct.pack("<IHHIII3I", MAGIC, VERSION, len(ents), bid, base,
                      total, 0, 0, 0)
    hdr += b"".join(ents)
    hdr += b"\0" * (ENT_SIZE * (MAXMOD - len(ents)))
    pak = bytearray(total)
    pak[0:len(hdr)] = hdr
    for o, body in blobs:
        pak[o:o + len(body)] = body
    with open(args.o, "wb") as fh:
        fh.write(pak)
    sys.stdout.write("modpak: build=%08x pak=%d bytes, all modules=%d B"
                     " (arena %s)\n" % (bid, total, need, args.arena))
    return 0


def cmd_patchbin(args):
    with open(args.pak, "rb") as fh:
        bid = struct.unpack("<I", fh.read(12)[8:12])[0]
    with open(args.bin, "rb") as fh:
        img = bytearray(fh.read())
    at = marker_at(bytes(img))
    img[at:at + 4] = struct.pack("<I", bid)
    with open(args.bin, "wb") as fh:
        fh.write(img)
    return 0


def main():
    ap = argparse.ArgumentParser()
    sub = ap.add_subparsers(dest="cmd", required=True)
    k = sub.add_parser("keep")
    k.add_argument("--nm", required=True)
    k.add_argument("-o", required=True)
    k.add_argument("objs", nargs="+")
    p = sub.add_parser("pack")
    for opt in ("--nm", "--objcopy", "--core", "--base", "--delta", "-o"):
        p.add_argument(opt, required=True)
    p.add_argument("--arena", default="")
    p.add_argument("--manifests", default="")
    p.add_argument("mods", nargs="+")
    q = sub.add_parser("patchbin")
    q.add_argument("--bin", required=True)
    q.add_argument("--pak", required=True)
    m = sub.add_parser("manifest")
    m.add_argument("files", nargs="+")
    c = sub.add_parser("check")
    for opt in ("--nm", "--objcopy", "--base", "--arena", "--manifests"):
        c.add_argument(opt, required=True)
    c.add_argument("--delta", default="0x100000")
    c.add_argument("--sdk-sym", default="",
                   help="apps/ipodui/sdk/sdk.sym: the SDK modules' allow "
                        "list")
    c.add_argument("mods", nargs="+")
    a = ap.parse_args()
    return {"keep": cmd_keep, "pack": cmd_pack, "patchbin": cmd_patchbin,
            "manifest": cmd_manifest, "check": cmd_check}[a.cmd](a)


if __name__ == "__main__":
    sys.exit(main())
