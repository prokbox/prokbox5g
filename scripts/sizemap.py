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

"""Size attribution by subsystem from a linker map (docs/phase9-brief.md
3.1, 8: the size waterfall). Phase 9, T.

Every input section in the map's "Linker script and memory map" part is
charged to its object, and each object to a subsystem by the first rule
in RULES (path globs over the object name relative to the build dir; an
archive member reads "firmware/libfirmware.a(usb_hid.o)"). Image bytes
are input sections in loaded output sections (what objcopy puts in
rockbox.bin); bss bytes are those in NOBITS output sections (resident
only). The ELF next to the map (or --elf) says which output sections are
NOBITS and gives _end; without it, names decide (.bss*, .ibss, .ncbss,
.stack, .idle_stacks).

  sizemap.py MAP [--elf ELF] [--base MAP [--base-elf ELF]] [--by object]
             [--top N] [--json FILE]

Output (stdout, one record per line, stable for scripts):
  total image=<b> bss=<b> end=<_end> iram=<b> init=<b> fill=<b>
  subsys <name> image=<b> bss=<b> objects=<n>
  object <name> image=<b> bss=<b> subsys=<name>            (--by object)
With --base, every line also carries base_image=, base_bss=,
delta_image=, delta_bss= (current minus base), subsystems sorted by
delta_image (largest cut first), and a closing
  delta image=<+-b> bss=<+-b> end=<+-b>
iram is the core IRAM in use (.iram + .ibss + .idle_stacks + .stack, the
49,152 B budget); init is .init (copied to the audio buffer and freed).
Exit: 0 done, 1 bad input.
"""

import argparse
import fnmatch
import json
import os
import sys

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)),
                                "risk"))
import reach  # noqa: E402  (map and ELF readers)

# (subsystem, globs). First match wins; keep the trim rows (brief 3.2)
# ahead of the broad directory rules below them.
RULES = [
    ("utf8proc", ["lib/libutf8proc.a(*", "*utf8proc*"]),
    ("unicode-nfc", ["*(nfc.o)", "*(unicode.o)", "*nfc_table*"]),
    ("skin", ["apps/gui/skin_engine/*", "lib/libskin_parser.a(*",
              "apps/gui/wps.o", "apps/gui/statusbar-skinned.o",
              "apps/gui/theme_settings.o", "apps/gui/backdrop.o",
              "apps/recorder/albumart.o"]),
    ("stock-menus", ["apps/menus/*", "apps/root_menu.o", "apps/menu.o"]),
    ("voice", ["apps/talk.o", "apps/voice_thread.o", "lib/libspeex*",
               "*speex*"]),
    ("iap", ["*(iap*.o)", "*/iap/*", "*(serial*.o)", "*(uart*.o)"]),
    ("browser", ["apps/tree.o", "apps/filetree.o", "apps/filetypes.o",
                 "apps/onplay.o", "apps/playlist_viewer.o",
                 "apps/playlist_catalog.o", "apps/shortcuts.o",
                 "apps/fileop.o", "apps/gui/folder_select.o",
                 "apps/bookmark.o"]),
    ("usb-classes", ["*(usb_hid*.o)", "*(usb_audio*.o)", "*(usb_iap*.o)",
                     "*(usb_serial*.o)", "*(usb_charging_only*.o)"]),
    ("recording", ["apps/recorder/recording.o", "apps/recorder/peakmeter.o",
                   "*(pcm_record*.o)", "apps/recorder/pcm_record.o",
                   "apps/enc_config.o", "*(enc_*.o)",
                   "apps/recorder/recording*"]),
    ("tagtree", ["apps/tagtree.o", "apps/tagnavi*"]),
    ("radio", ["apps/radio/*", "*(tuner*.o)", "*(fmradio*.o)", "*(rds*.o)",
               "*(ipod_remote_tuner.o)", "*(si47*.o)", "*(lv24020*.o)"]),
    ("dircache", ["*(dircache*.o)"]),
    ("plugins", ["apps/plugin.o", "apps/open_plugin.o", "apps/scratchbuf.o"]),
    ("keyboard", ["apps/recorder/keyboard.o", "*morse*"]),
    ("quickscreen", ["apps/gui/quickscreen.o", "apps/gui/pitchscreen.o",
                     "apps/hotkey*"]),
    ("shell", ["apps/ipodui/*"]),
    ("audiobook", ["apps/audiobook*.o"]),
    ("playback", ["apps/playback.o", "apps/buffering.o", "apps/pcmbuf.o",
                  "apps/codecs.o", "apps/codec_thread.o",
                  "apps/audio_thread.o", "apps/playlist.o",
                  "apps/cuesheet.o", "apps/rbcodec_helpers.o",
                  "apps/abrepeat.o"]),
    ("tagcache", ["apps/tagcache.o"]),
    ("rbcodec", ["lib/librbcodec.a(*", "lib/rbcodec/*"]),
    ("bitmaps", ["apps/bitmaps/*", "apps/recorder/icons.o"]),
    ("lang", ["apps/lang/*", "apps/language.o", "lang/*"]),
    ("devmode-stock-ui", ["apps/debug_menu.o", "apps/screens.o",
                          "apps/gui/*", "apps/recorder/*"]),
    ("apps-core", ["apps/*"]),
    ("usb-stack", ["*(usb_core.o)", "*(usb_storage.o)", "*(usb-drv*.o)",
                   "*(usb.o)", "*(usb_class_driver*.o)"]),
    ("storage", ["*(ata*.o)", "*(fat.o)", "*(disk*.o)", "*(file*.o)",
                 "*(dir*.o)", "*(storage.o)", "*(pathfuncs.o)",
                 "*(rbpaths.o)", "*(fileobj_mgr.o)", "*(mv.o)"]),
    ("lcd-fonts", ["*(lcd*.o)", "*(font*.o)", "*(sysfont.o)", "*(scroll*.o)",
                   "*(bidi.o)", "*(diacritic.o)"]),
    ("firmware", ["firmware/*"]),
    ("fixedpoint", ["lib/libfixedpoint.a(*", "lib/fixedpoint/*"]),
    ("libgcc", ["*libgcc.a(*"]),
    ("lib-other", ["lib/*"]),
    ("linker", ["linker stubs", "<linker>"]),
]

NOBITS_NAMES = (".bss", ".ibss", ".ncbss", ".stack", ".idle_stacks",
                ".ncdata", ".audiobuf")
IRAM_OUT = (".iram", ".ibss", ".idle_stacks", ".stack")


def subsystem(rel):
    for name, globs in RULES:
        for g in globs:
            if fnmatch.fnmatchcase(rel, g):
                return name
    return "other"


def out_sections(path):
    """{output section: size} from the map."""
    res = {}
    lines = open(path, errors="replace").read().split("\n")
    i = lines.index("Linker script and memory map")
    pend = None
    for line in lines[i + 1:]:
        if pend:
            f = line.split()
            if len(f) >= 2 and f[0].startswith("0x") and f[1].startswith("0x"):
                res[pend] = int(f[1], 16)
            pend = None
            continue
        if line and line[0] not in " \t" and not line.startswith(
                ("LOAD ", "OUTPUT(", "START GROUP", "END GROUP")):
            f = line.split()
            if len(f) >= 3 and f[1].startswith("0x") and \
                    f[2].startswith("0x"):
                res[f[0]] = int(f[2], 16)
            elif len(f) == 1:
                pend = f[0]
    return res


def measure(map_path, elf_path):
    try:
        isecs = reach.read_map(map_path)
        outs = out_sections(map_path)
    except (OSError, ValueError, reach.BadInput) as e:
        raise reach.BadInput("%s: %s" % (map_path, e))
    nobits, end = None, None
    if elf_path and os.path.exists(elf_path):
        secs, syms, _ = reach.read_elf(elf_path)
        nobits = {s.name for s in secs if s.type == reach.SHT_NOBITS}
        loaded = {s.name for s in secs if s.flags & reach.SHF_ALLOC}
        for s in syms:
            if s.name == "_end":
                end = s.value
        if end is None:
            for s in secs:
                if s.name == ".audiobuf":
                    end = s.addr
    else:
        loaded = set(outs)
    if nobits is None:
        nobits = set(NOBITS_NAMES)
    # objects relative to the build dir (crt0's firmware/ parent)
    base = ""
    for i in isecs:
        if os.path.basename(i.obj).startswith("crt0") and \
                "/firmware/" in i.obj:
            base = i.obj[:i.obj.index("/firmware/") + 1]
            break
    objs = {}
    for i in isecs:
        if i.osec not in loaded or i.size == 0:
            continue
        rel = i.obj[len(base):] if base and i.obj.startswith(base) else i.obj
        o = objs.setdefault(rel, {"image": 0, "bss": 0})
        o["bss" if i.osec in nobits else "image"] += i.size
    image = sum(v for k, v in outs.items() if k in loaded and k not in nobits)
    bss = sum(v for k, v in outs.items() if k in loaded and k in nobits)
    attributed = sum(o["image"] for o in objs.values())
    subs = {}
    for rel, o in objs.items():
        s = subs.setdefault(subsystem(rel), {"image": 0, "bss": 0,
                                             "objects": 0})
        s["image"] += o["image"]
        s["bss"] += o["bss"]
        s["objects"] += 1
        o["subsys"] = subsystem(rel)
    return {"total": {"image": image, "bss": bss, "end": end or 0,
                      "iram": sum(outs.get(k, 0) for k in IRAM_OUT),
                      "init": outs.get(".init", 0),
                      "fill": image - attributed},
            "subsys": subs, "objects": objs}


def main(argv):
    p = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    p.add_argument("map")
    p.add_argument("--elf")
    p.add_argument("--base")
    p.add_argument("--base-elf")
    p.add_argument("--by", choices=("subsystem", "object"),
                   default="subsystem")
    p.add_argument("--top", type=int, default=0)
    p.add_argument("--json")
    a = p.parse_args(argv)
    try:
        cur = measure(a.map, a.elf or os.path.splitext(a.map)[0] + ".elf")
        base = measure(a.base, a.base_elf or
                       os.path.splitext(a.base)[0] + ".elf") \
            if a.base else None
    except (reach.BadInput, OSError) as e:
        sys.stderr.write("sizemap.py: %s\n" % e)
        return 1
    t = cur["total"]
    print("total image=%d bss=%d end=%d iram=%d init=%d fill=%d" % (
        t["image"], t["bss"], t["end"], t["iram"], t["init"], t["fill"]))
    key = "subsys" if a.by == "subsystem" else "objects"
    rows = []
    names = set(cur[key]) | (set(base[key]) if base else set())
    for n in names:
        c = cur[key].get(n, {"image": 0, "bss": 0, "objects": 0})
        b = base[key].get(n, {"image": 0, "bss": 0, "objects": 0}) \
            if base else None
        rows.append((n, c, b))
    if base:
        rows.sort(key=lambda r: (r[1]["image"] - r[2]["image"], r[0]))
    else:
        rows.sort(key=lambda r: (-r[1]["image"], r[0]))
    if a.top:
        rows = rows[:a.top]
    out = []
    for n, c, b in rows:
        if a.by == "subsystem":
            s = "subsys %s image=%d bss=%d objects=%d" % (
                n, c["image"], c["bss"], c.get("objects", 0))
        else:
            s = "object %s image=%d bss=%d subsys=%s" % (
                n, c["image"], c["bss"],
                c.get("subsys") or (b or {}).get("subsys") or subsystem(n))
        if b is not None:
            s += " base_image=%d base_bss=%d delta_image=%+d delta_bss=%+d" % (
                b["image"], b["bss"], c["image"] - b["image"],
                c["bss"] - b["bss"])
        print(s)
        out.append(s)
    if base:
        bt = base["total"]
        print("delta image=%+d bss=%+d end=%+d" % (
            t["image"] - bt["image"], t["bss"] - bt["bss"],
            t["end"] - bt["end"]))
    if a.json:
        with open(a.json, "w") as f:
            json.dump({"current": cur, "base": base, "lines": out}, f,
                      indent=1)
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
