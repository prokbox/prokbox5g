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
# Host test of tools/modpak.py's P12 half (docs/phase12-brief.md 2.2, 2.3, 3.1):
# manifests, the v2 entry against modload_core.h, and (W1-F) modpak.py check
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

"""usage: test_modpak.py ENT_DUMP   (the compiled tests/modpak/ent_dump.c)"""
import glob
import os
import struct
import subprocess
import sys
import tempfile

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))
sys.path.insert(0, os.path.join(ROOT, "tools"))
import modpak  # noqa: E402

FIX = os.path.join(ROOT, "tests", "modpak", "fixtures")
fails = 0


def check(ok, what):
    global fails
    if not ok:
        fails += 1
        print("FAIL %s" % what)


def refused(path):
    try:
        modpak.manifest(path)
    except SystemExit:
        return True
    return False


def layout(dump):
    """modpak.ENT_FMT against the C struct's offsets and sizes"""
    out = subprocess.run([dump], capture_output=True, text=True,
                         check=True).stdout.split("\n")
    check(out[0] == "size %d" % modpak.ENT_SIZE,
          "entry size: C %s, modpak.py %d" % (out[0], modpak.ENT_SIZE))
    fmt = modpak.ENT_FMT[1:]
    sizes, offs, at = [], [], 0
    for ch in fmt.replace("8I", "IIIIIIII").replace("4B", "BBBB") \
                 .replace("2B", "BB"):
        n = struct.calcsize("<" + ch)
        offs.append(at)
        sizes.append(n)
        at += n
    rows = [l.split() for l in out[1:] if l]
    check(len(rows) == len(offs), "field count %d vs %d"
          % (len(rows), len(offs)))
    for (name, off, size), o, s in zip(rows, offs, sizes):
        check(int(off) == o and int(size) == s,
              "field %s: C at %s size %s, modpak.py at %d size %d"
              % (name, off, size, o, s))


def round_trip(dump):
    """manifest -> modpak.py's entry bytes -> the C struct: every field"""
    m = modpak.manifest(os.path.join(FIX, "ok_class_b_sdk.mod"))
    a = modpak.manifest(os.path.join(FIX, "ok_class_a.mod"))
    blob = modpak.ent_bytes(7, 4096, 1000, 1000, 24, 3, 2, 0xdeadbeef, m)
    blob += modpak.ent_bytes(2, 8192, 64, 64, 0, 0, 1, 0x1234, a)
    with tempfile.NamedTemporaryFile(delete=False) as fh:
        fh.write(blob)
    try:
        out = subprocess.run([dump, fh.name], capture_output=True,
                             text=True, check=True).stdout.split("\n")
    finally:
        os.unlink(fh.name)
    check(out[0] == "7 4096 1000 1000 24 3 2 3735928559 1 1 0 1 0 1 0 1024",
          "class B entry: %r" % out[0])
    check(out[1] == "2 8192 64 64 0 0 1 4660 0 0 0 0 4 0 1 0",
          "class A entry: %r" % out[1])


def manifests():
    for path in sorted(glob.glob(os.path.join(ROOT, "apps", "ipodui",
                                              "modules", "*.mod"))):
        check(not refused(path), "shipped manifest %s"
              % os.path.basename(path))
    for path in sorted(glob.glob(os.path.join(FIX, "ok_*.mod"))):
        check(not refused(path), "fixture %s parses"
              % os.path.basename(path))
    for path in sorted(glob.glob(os.path.join(FIX, "bad_*.mod"))):
        check(refused(path), "fixture %s is refused"
              % os.path.basename(path))
    s = modpak.manifest(os.path.join(FIX, "ok_sync.mod"))
    check(s["context"] == "sync", "context = sync")
    b = modpak.manifest(os.path.join(FIX, "ok_class_b_sdk.mod"))
    check(b["exit_policy"] == 0 and b["exit_ms"] == 400 and b["boost"] == 1
          and b["quota_kb"] == 64 and b["abi"] == (0, 1),
          "class B labels: %r" % b)
    g = modpak.manifest(os.path.join(FIX, "ok_sdk_a.mod"))
    check(g["state"] == 512 and g["quota_kb"] == 64 and g["kind"] == 1
          and g["strings"] == "solitaire", "SDK game labels: %r" % g)
    for name in ("solitaire", "badgame"):
        path = modpak.find_manifest(os.path.join(ROOT, "apps", "ipodui",
                                                 "games"), name)
        check(not refused(path), "shipped game manifest %s" % name)


def sdk_allow():
    """W4-0: an SDK module links only against sdk.sym (default deny)"""
    allow = modpak.sdk_allow(os.path.join(ROOT, "apps", "ipodui", "sdk",
                                          "sdk.sym"))
    g = modpak.manifest(os.path.join(FIX, "ok_sdk_a.mod"))
    deny = {"A": modpak.deny_list(os.path.join(ROOT, "apps", "ipodui",
                                               "modules", "class_a.deny"))}
    good = {"sdk_fill", "sdk_text", "sdk_state", "memcpy", "__aeabi_idiv",
            "__aeabi_uidivmod", "__gnu_thumb1_case_uqi", "_call_via_r3"}

    def run(undef, d=None, al=allow):
        return modpak.check_module("game", g, set(undef), 8192, 600, 8800,
                                   desc(g) if d is None else d, deny,
                                   40 * 1024, al)

    check(run(good) == [], "an SDK module of sdk.sym + libgcc passes: %r"
          % run(good))
    for sym in ("lcd_update", "core_alloc", "ui_push", "rb", "open",
                "cpu_boost", "audio_stop", "sdk_secret"):
        e = run(good | {sym})
        check(len(e) == 1 and sym in e[0] and "sdk.sym" in e[0],
              "the allow list refuses %s: %r" % (sym, e))
    check(any("--sdk-sym" in x for x in run(good, al=None)),
          "an SDK module without the table is refused")
    check(any("state" in x for x in run(good, d=desc(g, state=64))),
          "the header's state differs from the manifest")
    # relocated words: inside image + .bss only
    img = struct.pack("<4I", 0x03000010, 0x03000400, 0x02000000, 7)
    check(modpak.reloc_strays(img, [0, 1], 0x03000000, 0x03000500) == [],
          "relocated words inside the module")
    check(modpak.reloc_strays(img, [0, 2], 0x03000000, 0x03000500)
          == [(2, 0x02000000)], "a relocated word into the core")


def desc(m, magic=modpak.HDR_MAGIC, **over):
    """struct mod_desc bytes for manifest m (fields overridable)"""
    f = {"abi": m["abi"], "mclass": m["mclass"], "min_ram": m["min_ram"],
         "kind": m["kind"], "exit_policy": m["exit_policy"],
         "exit_ms": m["exit_ms"], "work_kb": m["work_kb"],
         "quota_kb": m["quota_kb"], "borrow_kb": m["borrow_kb"],
         "boost": m["boost"], "pages": m["pages"], "state": m["state"]}
    f.update(over)
    return struct.pack(modpak.DESC_FMT, magic, f["abi"][0], f["abi"][1],
                       f["mclass"], f["min_ram"], f["kind"],
                       f["exit_policy"], f["exit_ms"], f["work_kb"],
                       f["quota_kb"], f["borrow_kb"], f["boost"],
                       f["pages"], f["state"], 0, 0)


def check_cmd():
    """W1-F: modpak.py check, one fixture per rule (brief 2.3)"""
    mods = os.path.join(ROOT, "apps", "ipodui", "modules")
    deny = {"A": modpak.deny_list(os.path.join(mods, "class_a.deny")),
            "B": modpak.deny_list(os.path.join(mods, "class_b.deny"))}
    a = modpak.manifest(os.path.join(FIX, "ok_class_a.mod"))
    b = modpak.manifest(os.path.join(FIX, "ok_class_b_sdk.mod"))
    sync = modpak.manifest(os.path.join(FIX, "ok_sync.mod"))
    arena = 40 * 1024
    ok = {"lcd_fillrect", "ui_push", "str", "ui_arena_alloc",
          "mod_work_alloc", "ui_boost_burst"}

    def run(name, m, undef=ok, image=4096, bss=256, foot=4608, d=None,
            ar=arena):
        return modpak.check_module(name, m, set(undef), image, bss, foot,
                                   desc(m) if d is None else d, deny, ar)

    check(run("clean", a) == [], "a clean Class A module passes")
    # the deny list, a hit per class rule
    for sym in ("core_alloc_maximum", "buflib_alloc", "create_thread",
                "tick_add_task", "timeout_register", "audio_stop",
                "playlist_insert_track", "tagcache_search", "lib_open",
                "font_load", "opendir", "readdir", "storage_sleep",
                "bcm_send", "cpu_boost", "cpu_boost_"):
        e = run("deny_" + sym, a, ok | {sym})
        check(len(e) == 1 and sym in e[0], "Class A refuses %s: %r"
              % (sym, e))
    # the deny lists hold shell modules; an SDK module (b) is allow-listed
    # instead (sdk_allow() below), so the Class B rules use a shell twin
    bs = dict(b, kind=0, abi=(0, 0))
    check(run("b_alloc", bs, ok | {"core_alloc", "mod_borrow"}) == [],
          "Class B may borrow and allocate")
    check(len(run("b_stop", bs, ok | {"audio_stop"})) == 1,
          "Class B refuses audio_stop")
    check(run("sync_walk", sync, ok | {"opendir", "readdir"}) == [],
          "context = sync may walk directories")
    check(len(run("sync_alloc", sync, ok | {"core_alloc"})) == 1,
          "context = sync is still Class A otherwise")
    # sizes
    check(len(run("big_image", a, image=33 * 1024, foot=34 * 1024)) == 1,
          "Class A image over 32 KB")
    w = dict(a, work_kb=30)
    check(any("image + bss + work" in x for x in
              run("big_total", w, image=30 * 1024, foot=31 * 1024,
                  d=desc(w), ar=128 * 1024)),
          "Class A image + bss + work over 56 KB")
    check(any("arena" in x for x in run("no_room", a, foot=39 * 1024)),
          "a module over the arena minus the index stage")
    check(run("fits", a, foot=arena - modpak.IDX_STAGE
              - a["work_kb"] * 1024) == [],
          "a module exactly at the arena minus the stage fits")
    # the header against the manifest
    check(any("no module header" in x for x in
              modpak.check_module("nodesc", a, ok, 64, 0, 64, None, deny,
                                  arena)), "no header at slot 0")
    check(any("magic" in x for x in run("badmagic", a, d=desc(a, magic=1))),
          "a header with the wrong magic")
    for k, v in (("mclass", 1), ("work_kb", 9), ("exit_policy", 0),
                 ("min_ram", 1), ("kind", 1), ("boost", 1), ("pages", 8)):
        e = run("hdr_" + k, a, d=desc(a, **{k: v}))
        check(len(e) >= 1, "header %s differs from the manifest" % k)
    # W3-K: a paged list declares its page loads a second (Class A only)
    pg = dict(a, pages=8)
    check(run("paged", pg) == [], "pages = 8 matches its header")
    check(len(run("paged_hdr", pg, d=desc(pg, pages=0))) == 1,
          "pages differs from the header")
    real_modules()


def real_modules():
    """modpak.py check's verdict on a real modules build (fw-ui-mod).

    GATE_MOD_DIR picks the modules dir (default build/fw-ui-mod/modules).
    GATE_BUILD_STAMP (set by validate.sh's "module pack" step) makes it
    strict: check.ok must exist, be newer than that build marker (written
    this run, not left over from an earlier one) and newer than everything
    it checks; any of those is a FAIL. Without it, a missing check.ok is a
    note and a stale one a note that it was not read (a developer's old
    build dir does not fail scripts/test.sh)."""
    b_dir = os.environ.get("GATE_MOD_DIR") or \
        os.path.join(ROOT, "build", "fw-ui-mod", "modules")
    if not os.path.isabs(b_dir):
        b_dir = os.path.join(ROOT, b_dir)
    stamp = os.environ.get("GATE_BUILD_STAMP")
    strict = bool(stamp)
    ok_file = os.path.join(b_dir, "check.ok")
    rel = os.path.relpath(ok_file, ROOT)
    if not os.path.exists(ok_file):
        if strict:
            check(False, "real modules: %s missing (fw-ui-mod built this "
                  "run, so modpak.py check did not write it)" % rel)
        else:
            print("note modpak.py check on real modules: no %s "
                  "(fixtures only)" % rel)
        return
    t_ok = os.path.getmtime(ok_file)
    if strict:
        t_stamp = os.path.getmtime(stamp)
        check(t_ok >= t_stamp,
              "real modules: %s is from an earlier run (mtime %.0f, this "
              "run's build marker %s %.0f)" % (rel, t_ok, stamp, t_stamp))
    # its inputs (the Makefile rule's prerequisites): the module ELFs,
    # modpak.py, the manifests and the deny lists
    inputs = glob.glob(os.path.join(b_dir, "*.elf"))
    inputs += [os.path.join(ROOT, "tools", "modpak.py")]
    inputs += glob.glob(os.path.join(ROOT, "apps", "ipodui", "modules",
                                     "*.mod"))
    inputs += glob.glob(os.path.join(ROOT, "apps", "ipodui", "games", "*",
                                     "*.mod"))
    inputs += glob.glob(os.path.join(ROOT, "apps", "ipodui", "modules",
                                     "*.deny"))
    newer = sorted(os.path.relpath(f, ROOT) for f in inputs
                   if os.path.getmtime(f) > t_ok)
    if newer:
        what = ("real modules: %s is stale, older than %d input(s): %s"
                % (rel, len(newer), ", ".join(newer[:4])))
        if strict:
            check(False, what)
        else:
            print("note %s; not read (rebuild fw-ui-mod)" % what)
        return
    with open(ok_file) as fh:
        body = fh.read()
    check("FAIL" not in body and "arena need=" in body,
          "%s: %r" % (rel, body))
    print("ok   modpak.py check on %s:\n     " % os.path.dirname(rel)
          + body.strip().replace("\n", "\n     "))


def main():
    dump = sys.argv[1]
    layout(dump)
    round_trip(dump)
    manifests()
    check_cmd()
    sdk_allow()
    if fails:
        return 1
    print("ok   modpak.py v2 entry = modload_core.h layout, manifest round "
          "trip, %d fixtures, modpak.py check rules"
          % len(glob.glob(os.path.join(FIX, "*.mod"))))
    return 0


if __name__ == "__main__":
    sys.exit(main())
