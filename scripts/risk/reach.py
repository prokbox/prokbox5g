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

"""Reachability over the linked core ELF (docs/phase9-brief.md 3.1, 3.3.1, 8).

Phase 9 boot-safety artifact: it decides the scratch slot (the live
plugin_get_buffer callers, M8) and backs B's GC cross-check
(scripts/risk/gc_check.sh, L8). Self-test: scripts/risk/reach_selftest.sh.

Graph (brief 3.1): nodes are symbols. Edges are ARM b/bl targets, ADR
(add/sub rd, pc, #imm), any aligned 32-bit word equal to a function start,
and, for data, any word pointing into an object's input section (section
anchors at -Os hide which static is used, so the whole input section counts
as reached). Linker veneers are included. Default roots: crt0-pp.o, main,
irq_handler, fiq_handler. The result over-approximates reachability, so
savings from a cut are conservative.

Usage:
  reach.py ELF [--map MAP] [--root SYM]... [--objdump CMD] [--nm CMD]
           [--reachable] [--unreachable] [--callers SYM]... [--cut GLOB]...
           [--paths SYM]... [--json FILE]

  ELF            linked core image, e.g. build/fw-ui/rockbox.elf
  --map MAP      linker map (default: ELF with .map), attributes input
                 sections to objects
  --root SYM     extra root (repeatable), added to the defaults
  --reachable    print every reachable symbol
  --unreachable  print every symbol in the image that is not reachable
  --callers SYM  print the reachable callers of SYM (repeatable); Phase 9
                 uses it for plugin_get_buffer (the scratch slot, M8)
  --cut GLOB     remove the objects matching GLOB (path glob over the map's
                 object names, repeatable; all globs form one joint cut) and
                 report what else becomes unreachable
  --paths SYM    print one shortest root -> SYM reference path (keystones)
  --json FILE    also write everything printed as JSON

Output (stdout, one record per line, space separated, stable for scripts):
  summary image=<bytes> bss=<bytes> reachable=<bytes> unreachable=<bytes>
  reach <symbol> <section> <size> <object>          (--reachable)
  dead <symbol> <section> <size> <object>           (--unreachable)
  caller <callee> <caller> <object>                 (--callers; none -> no line)
  cut image=<bytes> bss=<bytes> objects=<n> also=<n> (--cut: bytes freed incl.
                                                      what became unreachable)
  path <symbol> <root> > <sym> > ... > <symbol>     (--paths)

Exit status: 0 done, 1 bad input, 2 not implemented (this skeleton).

Implementation notes (T, Phase 9). The interface above is the contract;
these pin down what the words mean.

- The ELF is read directly (no objdump/nm needed; the two options are kept
  for the interface and ignored). The map gives every input section
  (output section, name, address, size, object); bytes no input section
  covers (fill, linker stubs) become "<linker>" pieces, so the nodes tile
  every allocated section exactly.
- An input section is CODE when it holds a FUNC symbol or an ARM/Thumb
  mapping symbol ($a/$t), else DATA. A data input section is one node.
  A code input section is split at its FUNC, OBJECT and global NOTYPE
  symbols; each piece is one node (so an unreferenced static function in
  an object's .text is dead even without -ffunction-sections). A piece
  whose last instruction can fall through (not b, bx, ldm/pop {..pc},
  ldr pc or a data-processing op writing pc) gets an edge to the next
  piece of the same input section (asm functions that run into the next).
- Mapping symbols decide how bytes are read: $a words are decoded as ARM
  (b/bl/blx targets, ADR add/sub rd, pc, #imm, and ldr rd, [pc, #imm]
  whose literal is then resolved as a data word), $t halfwords as Thumb
  (bl/blx pairs, adr, ldr literal), $d words as data. A range with no
  mapping symbol before it is read both ways (conservative).
- A data word w is an edge to a code node when w (or w & ~1, Thumb) is
  exactly a code entry: a piece start, a FUNC symbol, or any named label
  in code (asm labels referenced by address). It is an edge to every data
  node whose input section contains w, and to a node whose input section
  ends at w (one-past pointers such as _end) unless another data node
  starts at w (then only that one: with -fdata-sections every pointer to
  an object is also the end of the previous one; counted as
  endptr_ambiguous= in the stats line). Words pointing into the middle of
  a function are not edges (counted as midcode=).
- Roots: every node of crt0*.o, main, irq_handler, fiq_handler, the ELF
  entry, every node in .vectors, every input section the map lists under
  a KEEP statement, plus --root.
- summary/reach/dead/caller/path describe the analysed graph: with --cut,
  the graph after the cut (the cut objects' nodes are gone). Sizes in
  reach/dead lines are the symbol's st_size, or the piece size when the
  symbol has none; anonymous pieces print as <isec>@<hex address>;
  linker fill (<gap>) counts in summary but is not listed. summary also
  carries trailing reachable_bss= and unreachable_bss=.
- --words auto (default): data words are classified by the relocations
  in the build's objects (.o, and .a members), read at the paths the map
  names. Only relocated words are pointers, and the relocation's own
  target picks the node (a data input section of the same object, or a
  global symbol), the value only choosing among a split code section's
  pieces: GCC folds &arr[-k] into literals that point into the previous
  object, and numeric tables are full of values that look like
  addresses (value-matching made atan_table "reach" set_bool_options).
  Input sections with no usable object info (linker stubs, fill, an
  object missing or ambiguous) fall back to the value rules above;
  value_isecs=/value_bytes= on the stats line say how much. --words
  value applies the value rules everywhere (the brief's literal method;
  over-reports).
- caller lines list the reachable nodes with a direct edge into a node
  that holds the symbol (a data node's name is its first symbol).
"""

import argparse
import bisect
import fnmatch
import json
import os
import re
import struct
import sys
from collections import deque

SHT_PROGBITS, SHT_SYMTAB, SHT_NOBITS = 1, 2, 8
SHF_ALLOC, SHF_EXECINSTR = 0x2, 0x4
STT_NOTYPE, STT_OBJECT, STT_FUNC, STT_SECTION, STT_FILE = 0, 1, 2, 3, 4
STB_LOCAL, STB_GLOBAL, STB_WEAK = 0, 1, 2
SHN_UNDEF, SHN_LORESERVE = 0, 0xff00
EM_ARM = 40


class BadInput(Exception):
    pass


# ---------------------------------------------------------------- ELF
class Section:
    __slots__ = ("idx", "name", "type", "flags", "addr", "size", "data")

    def __init__(self, idx, name, typ, flags, addr, size, data):
        self.idx, self.name, self.type, self.flags = idx, name, typ, flags
        self.addr, self.size, self.data = addr, size, data


class Sym:
    __slots__ = ("name", "value", "size", "type", "bind", "shndx")

    def __init__(self, name, value, size, typ, bind, shndx):
        self.name, self.value, self.size = name, value, size
        self.type, self.bind, self.shndx = typ, bind, shndx


def read_elf(path):
    try:
        raw = open(path, "rb").read()
    except OSError as e:
        raise BadInput("cannot read ELF %s: %s" % (path, e))
    if raw[:4] != b"\x7fELF":
        raise BadInput("%s is not an ELF file" % path)
    if raw[4] != 1 or raw[5] != 1:
        raise BadInput("%s: only ELF32 little-endian is supported" % path)
    (e_type, e_machine, _ver, e_entry, _phoff, e_shoff, _flags, _ehsize,
     _phentsize, _phnum, e_shentsize, e_shnum,
     e_shstrndx) = struct.unpack_from("<HHIIIIIHHHHHH", raw, 16)
    if e_machine != EM_ARM:
        raise BadInput("%s: machine %d is not ARM" % (path, e_machine))
    hdrs = []
    for i in range(e_shnum):
        hdrs.append(struct.unpack_from("<IIIIIIIIII", raw,
                                       e_shoff + i * e_shentsize))
    shstr_off = hdrs[e_shstrndx][4]

    def cstr(off):
        end = raw.index(b"\0", off)
        return raw[off:end].decode("latin-1")

    secs = []
    for i, h in enumerate(hdrs):
        name = cstr(shstr_off + h[0])
        data = raw[h[4]:h[4] + h[5]] if h[1] != SHT_NOBITS else None
        secs.append(Section(i, name, h[1], h[2], h[3], h[5], data))
    syms = []
    for i, h in enumerate(hdrs):
        if h[1] != SHT_SYMTAB:
            continue
        stroff = hdrs[h[6]][4]
        for off in range(h[4], h[4] + h[5], 16):
            st_name, value, size, info, _other, shndx = struct.unpack_from(
                "<IIIBBH", raw, off)
            syms.append(Sym(cstr(stroff + st_name), value, size, info & 15,
                            info >> 4, shndx))
    if not syms:
        raise BadInput("%s has no symbol table (stripped?)" % path)
    return secs, syms, e_entry


# ---------------------------------------------------------------- map
class ISec:
    """One input section placed in the image."""
    __slots__ = ("osec", "name", "addr", "size", "obj", "keep", "ptrs")

    def __init__(self, osec, name, addr, size, obj, keep=False, ptrs=None):
        self.osec, self.name, self.addr, self.size = osec, name, addr, size
        self.obj, self.keep = obj, keep
        # None: no relocation info (words are value-matched); else the
        # sorted [(image address, kind, hint)] of the words the object
        # file relocates as data (kind "abs", "rel" or "prel31")
        self.ptrs = ptrs


MAP_OSEC = re.compile(r"^(\S+)\s+0x([0-9a-fA-F]+)\s+0x([0-9a-fA-F]+)")
MAP_OSEC_NAME = re.compile(r"^(\S+)\s*$")
MAP_ISEC = re.compile(r"^ (\S+)\s+0x([0-9a-fA-F]+)\s+0x([0-9a-fA-F]+)\s+(\S.*)$")
MAP_ISEC_NAME = re.compile(r"^ (\S+)\s*$")
MAP_TAIL = re.compile(r"^\s+0x([0-9a-fA-F]+)\s+0x([0-9a-fA-F]+)\s+(\S.*)$")
MAP_STMT = re.compile(r"^ (\*|KEEP|SORT|EXCLUDE_FILE)")


def read_map(path):
    try:
        lines = open(path, errors="replace").read().split("\n")
    except OSError as e:
        raise BadInput("cannot read map %s: %s" % (path, e))
    try:
        start = lines.index("Linker script and memory map")
    except ValueError:
        raise BadInput("%s: no 'Linker script and memory map' section "
                       "(not a GNU ld map?)" % path)
    isecs = []
    osec = None
    pend_osec = None
    pend_isec = None
    keep = False
    for line in lines[start + 1:]:
        if not line:
            pend_osec = pend_isec = None
            continue
        if pend_isec is not None:
            m = MAP_TAIL.match(line)
            if m:
                isecs.append(ISec(osec, pend_isec, int(m.group(1), 16),
                                  int(m.group(2), 16), m.group(3).strip(),
                                  keep))
                pend_isec = None
                continue
            pend_isec = None
        if pend_osec is not None:
            m = re.match(r"^\s+0x([0-9a-fA-F]+)\s+0x([0-9a-fA-F]+)", line)
            if m:
                osec = pend_osec
                pend_osec = None
                keep = False
                continue
            pend_osec = None
        if line[0] not in " \t":
            if line.startswith(("LOAD ", "OUTPUT(", "START GROUP",
                                "END GROUP")):
                continue
            m = MAP_OSEC.match(line)
            if m:
                osec, keep = m.group(1), False
                continue
            m = MAP_OSEC_NAME.match(line)
            if m:
                pend_osec = m.group(1)
            continue
        m = MAP_STMT.match(line)
        if m:
            keep = line.lstrip().startswith("KEEP")
            continue
        m = MAP_ISEC.match(line)
        if m:
            name = m.group(1)
            obj = m.group(4).strip()
            if name.startswith("*") or osec is None:
                continue
            # "load address 0x..." trails an output section, never an
            # input one, but guard against odd lines anyway
            if obj.startswith("load address"):
                continue
            isecs.append(ISec(osec, name, int(m.group(2), 16),
                              int(m.group(3), 16), obj, keep))
            continue
        m = MAP_ISEC_NAME.match(line)
        if m and not m.group(1).startswith("*") and osec is not None:
            pend_isec = m.group(1)
    if not isecs:
        raise BadInput("%s: no input sections parsed" % path)
    return isecs


# ---------------------------------------------------------- relocations
SHT_REL, SHT_RELA = 9, 4
R_ARM_ABS32, R_ARM_REL32, R_ARM_TARGET1, R_ARM_PREL31 = 2, 3, 38, 42
R_ARM_ABS32_NOI = 55
RELOC_KIND = {R_ARM_ABS32: "abs", R_ARM_TARGET1: "abs",
              R_ARM_ABS32_NOI: "abs", R_ARM_REL32: "rel",
              R_ARM_PREL31: "prel31"}
# data relocations we can't classify: an input section holding one is
# value-matched as a whole (ABS16, ABS8, TARGET2, REL32_NOI, GOT ones)
RELOC_UNSURE = {5, 8, 41, 56, 26, 95, 96, 97, 98}


def obj_sections(raw):
    """{section name: [(size, [(offset, kind, hint)] or None)]} for an
    ELF32 LE relocatable object; None marks a section with an unsure
    relocation. hint names the relocation's target: ('sec', name) for a
    symbol of this object, ('sym', name) for an undefined one."""
    if raw[:4] != b"\x7fELF" or raw[4] != 1 or raw[5] != 1:
        return None
    e_type, = struct.unpack_from("<H", raw, 16)
    if e_type != 1:                                # ET_REL
        return None
    e_shoff, = struct.unpack_from("<I", raw, 32)
    e_shentsize, e_shnum, e_shstrndx = struct.unpack_from("<HHH", raw, 46)
    hdrs = [struct.unpack_from("<IIIIIIIIII", raw, e_shoff + i * e_shentsize)
            for i in range(e_shnum)]
    stroff = hdrs[e_shstrndx][4]

    def cstr(off):
        return raw[off:raw.index(b"\0", off)].decode("latin-1")

    def symbol(symsec, idx):
        """('sec', section name) for a symbol defined in this object,
        ('sym', name) for an undefined one, None if unreadable."""
        h = hdrs[symsec]
        off = h[4] + idx * 16
        if idx == 0 or off + 16 > h[4] + h[5]:
            return None
        st_name, _v, _s, info, _o, shndx = struct.unpack_from(
            "<IIIBBH", raw, off)
        if shndx == SHN_UNDEF:
            return ("sym", raw[hdrs[h[6]][4] + st_name:
                               raw.index(b"\0", hdrs[h[6]][4] + st_name)
                               ].decode("latin-1"))
        if shndx >= SHN_LORESERVE:
            return None
        return ("sec", cstr(stroff + hdrs[shndx][0]))

    rel = {}
    for h in hdrs:
        if h[1] not in (SHT_REL, SHT_RELA):
            continue
        ent = 8 if h[1] == SHT_REL else 12
        lst = rel.setdefault(h[7], [])             # sh_info: target section
        for off in range(h[4], h[4] + h[5], ent):
            r_off, r_info = struct.unpack_from("<II", raw, off)
            typ = r_info & 0xFF
            hint = symbol(h[6], r_info >> 8) if typ in RELOC_KIND else None
            lst.append((r_off, typ, hint))
    res = {}
    for i, h in enumerate(hdrs):
        if not h[2] & SHF_ALLOC:
            continue
        words = []
        unsure = False
        for r_off, typ, hint in rel.get(i, ()):
            if typ in RELOC_KIND:
                words.append((r_off, RELOC_KIND[typ], hint))
            elif typ in RELOC_UNSURE:
                unsure = True
        words.sort()
        res.setdefault(cstr(stroff + h[0]), []).append(
            (h[5], None if unsure else words))
    return res


def ar_members(raw):
    """{member name: [bytes]} of a GNU or BSD ar archive (None if not)."""
    if raw[:8] != b"!<arch>\n":
        return None
    pos, longnames, out = 8, b"", {}
    while pos + 60 <= len(raw):
        hdr = raw[pos:pos + 60]
        name = hdr[:16].decode("latin-1").rstrip()
        size = int(hdr[48:58].decode("latin-1").strip() or 0)
        body = raw[pos + 60:pos + 60 + size]
        pos += 60 + size + (size & 1)
        if name == "//":
            longnames = body
            continue
        if name in ("/", "/SYM64/", "__.SYMDEF", "__.SYMDEF SORTED"):
            continue
        if name.startswith("#1/"):                 # BSD long name
            n = int(name[3:])
            name, body = body[:n].rstrip(b"\0").decode("latin-1"), body[n:]
        elif name.startswith("/") and name[1:].isdigit():
            o = int(name[1:])
            name = longnames[o:longnames.index(b"/\n", o)].decode("latin-1")
        elif name.endswith("/"):
            name = name[:-1]
        out.setdefault(name, []).append(body)
    return out


class RelocDB:
    """Relocation info per map object, read from the build's .o / .a."""
    def __init__(self):
        self.archives = {}
        self.objs = {}

    def sections(self, obj):
        """[{name: [(size, words)]}] for every candidate file of obj."""
        if obj in self.objs:
            return self.objs[obj]
        cands = []
        m = re.match(r"^(.*\.a)\((.*)\)$", obj)
        try:
            if m:
                arc = self.archives.get(m.group(1))
                if arc is None:
                    arc = ar_members(open(m.group(1), "rb").read()) or {}
                    self.archives[m.group(1)] = arc
                for body in arc.get(m.group(2), ()):
                    secs = obj_sections(body)
                    if secs is not None:
                        cands.append(secs)
            elif obj.endswith(".o"):
                secs = obj_sections(open(obj, "rb").read())
                if secs is not None:
                    cands.append(secs)
        except (OSError, ValueError, struct.error):
            cands = []
        self.objs[obj] = cands
        return cands

    def words(self, obj, name, size):
        """[(offset, kind)] of the data words relocated in input section
        name of obj, [] if it has none, None if unknown or ambiguous."""
        found = []
        for secs in self.sections(obj):
            for sz, words in secs.get(name, ()):
                if sz == size:
                    found.append(words)
        if not found:
            # merged sections (strings) change size; they never carry
            # data relocations, so an object whose every same-named
            # section is relocation-free is still exact
            allw = [w for secs in self.sections(obj)
                    for _, w in secs.get(name, ())]
            if allw and all(w == [] for w in allw):
                return []
            return None
        if any(f is None for f in found) or \
                any(f != found[0] for f in found):
            return None
        return found[0]


# ---------------------------------------------------------------- graph
class Node:
    __slots__ = ("id", "sec", "start", "end", "isec", "obj", "code", "syms",
                 "image", "bss", "name")

    def __init__(self, nid, sec, start, end, isec, obj, code):
        self.id, self.sec, self.start, self.end = nid, sec, start, end
        self.isec, self.obj, self.code = isec, obj, code
        self.syms = []
        size = end - start
        self.image = size if sec.type != SHT_NOBITS else 0
        self.bss = size if sec.type == SHT_NOBITS else 0
        self.name = None


def is_mapping(name):
    return name[:1] == "$" and name[1:2] in ("a", "t", "d") and \
        (len(name) == 2 or name[2] == ".")


def sym_rank(s):
    # primary name preference for a node
    return ({STT_FUNC: 0, STT_OBJECT: 1}.get(s.type, 2),
            0 if s.bind != STB_LOCAL else 1)


def sign24(v):
    return v - (1 << 24) if v & 0x800000 else v


def arm_imm(w):
    rot = ((w >> 8) & 0xF) * 2
    imm = w & 0xFF
    return ((imm >> rot) | (imm << (32 - rot))) & 0xFFFFFFFF if rot else imm


def arm_terminates(w):
    """True when the ARM word can't fall through to the next one."""
    cond = w >> 28
    if cond != 0xE:
        return False
    if (w & 0x0F000000) == 0x0A000000:          # b
        return True
    if (w & 0x0FFFFFF0) == 0x012FFF10:          # bx rm
        return True
    if (w & 0x0E108000) == 0x08108000:          # ldm ..., {..., pc}
        return True
    if (w & 0x0C10F000) == 0x0410F000:          # ldr pc, [...]
        return True
    if (w & 0x0C00F000) == 0x0000F000:          # dp op, rd = pc
        op = (w >> 21) & 0xF
        if not (8 <= op <= 11):                 # tst/teq/cmp/cmn: no rd
            return True
    return False


def thumb_terminates(h):
    if (h & 0xF800) == 0xE000:                  # b (unconditional)
        return True
    if (h & 0xFF87) == 0x4700:                  # bx rm
        return True
    if (h & 0xFF00) == 0xBD00:                  # pop {..., pc}
        return True
    if (h & 0xFF87) == 0x4687:                  # mov pc, rm
        return True
    return False


class Graph:
    def __init__(self, elf_path, map_path, root_syms, words="auto"):
        self.elf_path = elf_path
        self.words_mode = words
        secs, syms, entry = read_elf(elf_path)
        self.entry = entry
        self.secs = [s for s in secs if s.flags & SHF_ALLOC and s.size > 0]
        isecs = read_map(map_path)
        self.objbase = self._obj_prefix(elf_path, isecs)
        self._build_nodes(isecs, syms)
        self._build_index()
        self.stats = {"edges": 0, "midcode": 0, "fallthrough": 0,
                      "endptr_ambiguous": 0, "unknown_state_bytes": 0,
                      "reloc_isecs": 0, "value_isecs": 0,
                      "hint_outside": 0,
                      "value_bytes": 0}
        self._attach_relocs()
        self._build_edges()
        self._roots(root_syms)

    # -- object names -------------------------------------------------
    @staticmethod
    def _obj_prefix(elf_path, isecs):
        # objects are printed relative to the build dir: the ELF's own dir
        # when the map names it, else the dir holding firmware/ in crt0's
        # path (an ELF copied elsewhere for analysis)
        d = os.path.dirname(os.path.abspath(elf_path))
        for i in isecs:
            if i.obj.startswith(d + "/"):
                return d + "/"
        for i in isecs:
            b = os.path.basename(i.obj)
            if b.startswith("crt0") and "/firmware/" in i.obj:
                return i.obj[:i.obj.index("/firmware/") + 1]
        return ""

    def rel(self, obj):
        if self.objbase and obj.startswith(self.objbase):
            return obj[len(self.objbase):]
        return obj

    # -- nodes ----------------------------------------------------------
    def _build_nodes(self, isecs, syms):
        bysec = {}
        for s in self.secs:
            bysec.setdefault(s.name, s)
        persec_syms = {}
        persec_map = {}
        for sy in syms:
            if sy.shndx == SHN_UNDEF or sy.shndx >= SHN_LORESERVE:
                continue
            if sy.type in (STT_SECTION, STT_FILE):
                continue
            if is_mapping(sy.name):
                persec_map.setdefault(sy.shndx, []).append(
                    (sy.value & ~1, sy.name[1]))
                continue
            if not sy.name:
                continue
            persec_syms.setdefault(sy.shndx, []).append(sy)
        self.mapping = {}
        for k, v in persec_map.items():
            v.sort()
            self.mapping[k] = ([a for a, _ in v], [t for _, t in v])
        self.has_mapping = bool(persec_map)

        by_osec = {}
        for i in isecs:
            if i.size > 0:
                by_osec.setdefault(i.osec, []).append(i)
        self.nodes = []
        self.keep_nodes = []
        for sec in self.secs:
            items = sorted(by_osec.get(sec.name, []), key=lambda i: i.addr)
            lo, hi = sec.addr, sec.addr + sec.size
            pieces = []
            cur = lo
            for it in items:
                a, b = max(it.addr, lo), min(it.addr + it.size, hi)
                if b <= a or a < cur:
                    if b > cur and a < cur:
                        a = cur          # overlapping map entries: clip
                    else:
                        continue
                if a > cur:
                    pieces.append(ISec(sec.name, "<gap>", cur, a - cur,
                                       "<linker>"))
                piece = ISec(sec.name, it.name, a, b - a, it.obj, it.keep)
                piece.ptrs = it   # original map entry, resolved later
                pieces.append(piece)
                cur = b
            if cur < hi:
                pieces.append(ISec(sec.name, "<gap>" if items else sec.name,
                                   cur, hi - cur,
                                   "<linker>" if items else "?"))
            ssyms = sorted(persec_syms.get(sec.idx, []),
                           key=lambda s: s.value)
            svals = [s.value & ~1 if s.type == STT_FUNC else s.value
                     for s in ssyms]
            maddrs, mtypes = self.mapping.get(sec.idx, ([], []))
            for p in pieces:
                a, b = p.addr, p.addr + p.size
                i0 = bisect.bisect_left(svals, a)
                i1 = bisect.bisect_left(svals, b)
                inside = ssyms[i0:i1]
                j0 = bisect.bisect_left(maddrs, a)
                j1 = bisect.bisect_left(maddrs, b)
                code = any(s.type == STT_FUNC for s in inside) or \
                    any(t in "at" for t in mtypes[j0:j1])
                if sec.type == SHT_NOBITS:
                    code = False
                if not code:
                    n = self._new(sec, a, b, p, False)
                    n.syms = list(inside)
                    if p.keep:
                        self.keep_nodes.append(n)
                    continue
                cuts = sorted({a} | {
                    (s.value & ~1) for s in inside
                    if s.type in (STT_FUNC, STT_OBJECT)
                    or (s.type == STT_NOTYPE and s.bind != STB_LOCAL)})
                cuts.append(b)
                k = 0
                for c0, c1 in zip(cuts, cuts[1:]):
                    if c1 <= c0:
                        continue
                    n = self._new(sec, c0, c1, p, True)
                    while k < len(inside) and \
                            (inside[k].value & ~1 if inside[k].type ==
                             STT_FUNC else inside[k].value) < c1:
                        n.syms.append(inside[k])
                        k += 1
                    if p.keep:
                        self.keep_nodes.append(n)
        for n in self.nodes:
            named = [s for s in n.syms
                     if not (s.type == STT_NOTYPE and s.bind == STB_LOCAL)]
            at = [s for s in named if (s.value & ~1) == n.start]
            pool = at or named
            if pool:
                n.name = sorted(pool, key=sym_rank)[0].name
            else:
                n.name = "%s@%x" % (n.isec.name, n.start)

    def _attach_relocs(self):
        """Turn each piece's original map entry into its relocated data
        words (image addresses), or None (value-match its words)."""
        db = RelocDB() if self.words_mode == "auto" else None
        self.relocw = {}
        done = set()
        for n in self.nodes:
            p = n.isec
            if id(p) in done:
                continue
            done.add(id(p))
            orig = p.ptrs
            p.ptrs = None
            if n.sec.type == SHT_NOBITS:
                p.ptrs = []
                continue
            if db is None or not isinstance(orig, ISec):
                self.stats["value_isecs"] += 1
                self.stats["value_bytes"] += p.size
                continue
            words = db.words(orig.obj, orig.name, orig.size)
            if words is None:
                self.stats["value_isecs"] += 1
                self.stats["value_bytes"] += p.size
                continue
            self.stats["reloc_isecs"] += 1
            lst = []
            for off, kind, hint in words:
                a = orig.addr + off
                if p.addr <= a and a + 4 <= p.addr + p.size:
                    lst.append((a, kind, hint))
                    self.relocw[(n.sec.idx, a)] = (kind, hint, orig.obj)
            p.ptrs = lst

    def reloc_targets(self, addr, w, kind, hint=None, obj=None):
        """Nodes a relocated data word points at. The relocation's own
        target decides (what the linker resolved): a data input section
        of the same object is that node whatever the value (GCC folds
        &arr[-k] into the literal, so the value can sit in the previous
        object); a code section split into pieces, or a global symbol,
        narrows to its nodes, the value picking the piece. Without a
        usable hint: containment by value (code nodes count anywhere
        inside), the Thumb bit, and on a boundary (end of X == start of
        Y) both; linker fill is never a target when anything else is."""
        if kind == "rel":
            t = (addr + w) & 0xFFFFFFFF
        elif kind == "prel31":
            v = w & 0x7FFFFFFF
            if v & 0x40000000:
                v -= 0x80000000
            t = (addr + v) & 0xFFFFFFFF
        else:
            t = w
        group = None
        if hint is not None:
            if hint[0] == "sec":
                group = self.by_isec.get((obj, hint[1]))
            else:
                group = self.by_gsym.get(hint[1])
        if group:
            if len(group) == 1:
                return {group[0]}
            pick = [i for i in group
                    if self.nodes[i].start <= (t & ~1) < self.nodes[i].end]
            if not pick:
                pick = [i for i in group if self.nodes[i].end == t]
            if pick:
                return set(pick)
            self.stats["hint_outside"] += 1
            return set(group)
        inside, ends = [], []
        for n in self.nodes_at(t, inclusive_end=True):
            (inside if n.start <= t < n.end else ends).append(n.id)
        if not inside and t & 1:
            inside = [n.id for n in self.nodes_at(t & ~1)]
        real = [i for i in inside + ends
                if self.nodes[i].isec.obj != "<linker>"]
        if not ends:
            return set(real or inside)
        self.stats["endptr_ambiguous"] += 1
        return set(real or inside + ends)

    def literal_targets(self, addr, sec):
        """Targets of the word at addr (in sec when VMAs overlap) read by
        an ldr-literal."""
        m = self.node_at(addr, sec)
        if m is not None and m.isec.ptrs is not None:
            rw = self.relocw.get((m.sec.idx, addr))
            if rw is None:
                return set()
            w = self.read_word(addr, m.sec)
            return self.reloc_targets(addr, w, rw[0], rw[1], rw[2]) \
                if w is not None else set()
        w = self.read_word(addr, sec)
        return self.word_targets(w) if w is not None else set()

    def _new(self, sec, a, b, isec, code):
        n = Node(len(self.nodes), sec, a, b, isec, isec.obj, code)
        self.nodes.append(n)
        return n

    def _build_index(self):
        # relocation targets: (object, input section) and global symbols
        self.by_isec = {}
        self.by_gsym = {}
        for n in self.nodes:
            self.by_isec.setdefault((n.isec.obj, n.isec.name), []).append(
                n.id)
            for sy in n.syms:
                if sy.bind != STB_LOCAL:
                    self.by_gsym.setdefault(sy.name, []).append(n.id)
        # address -> nodes (pages of 4 KB; overlapping VMAs allowed)
        self.page = {}
        for n in self.nodes:
            for pg in range(n.start >> 12, ((max(n.end, n.start + 1) - 1)
                                            >> 12) + 1):
                self.page.setdefault(pg, []).append(n)
        # code entries: piece starts, FUNC symbols and any named label in
        # code (asm labels referenced by address)
        self.entry_nodes = {}
        for n in self.nodes:
            if not n.code:
                continue
            self.entry_nodes.setdefault(n.start, set()).add(n.id)
            for s in n.syms:
                self.entry_nodes.setdefault(s.value & ~1, set()).add(n.id)
        self.thumb_entries = set()
        for n in self.nodes:
            for s in n.syms:
                if s.type == STT_FUNC and s.value & 1:
                    self.thumb_entries.add(s.value & ~1)
        self.secbyidx = {s.idx: s for s in self.secs}

    def nodes_at(self, addr, inclusive_end=False):
        res = []
        for n in self.page.get(addr >> 12, ()):
            if n.start <= addr < n.end or (inclusive_end and addr == n.end):
                res.append(n)
        if inclusive_end and (addr & 0xFFF) == 0:
            for n in self.page.get((addr >> 12) - 1, ()):
                if addr == n.end and n not in res:
                    res.append(n)
        return res

    def node_at(self, addr, prefer_sec):
        cand = self.nodes_at(addr)
        if not cand:
            return None
        for n in cand:
            if n.sec is prefer_sec:
                return n
        return cand[0]

    def word_targets(self, w):
        """Nodes a data word w points at (the brief's word rules)."""
        out = set()
        ids = self.entry_nodes.get(w)
        if ids:
            out |= ids
        elif w & 1 and (w & ~1) in self.entry_nodes:
            out |= self.entry_nodes[w & ~1]
        else:
            hit_code = False
            inside, ends = [], []
            for n in self.nodes_at(w, inclusive_end=True):
                if n.code:
                    if n.start <= w < n.end:
                        hit_code = True
                    continue
                (inside if n.start <= w < n.end else ends).append(n.id)
            out.update(inside)
            if ends:
                # a one-past-end pointer (w == end of X) reaches X only when
                # no other data node starts at w: with -fdata-sections every
                # pointer to an object is also the end of the one before it
                if any(self.nodes[i].start == w and
                       self.nodes[i].isec.obj != "<linker>"
                       for i in inside):
                    self.stats["endptr_ambiguous"] += len(ends)
                else:
                    out.update(ends)
            if hit_code and not out:
                self.stats["midcode"] += 1
        return out

    # -- edges ----------------------------------------------------------
    def read_word(self, addr, prefer=None):
        cand = self.nodes_at(addr)
        cand.sort(key=lambda n: n.sec is not prefer)
        for n in cand:
            sec = n.sec
            if sec.data is not None and addr + 4 <= sec.addr + sec.size:
                o = addr - sec.addr
                return struct.unpack_from("<I", sec.data, o)[0]
        return None

    def _states(self, n):
        """Yield (start, end, state) ranges inside node n; state is
        'a', 't', 'd' or '?' (no mapping symbol before it)."""
        maddrs, mtypes = self.mapping.get(n.sec.idx, ([], []))
        i = bisect.bisect_right(maddrs, n.start) - 1
        st = mtypes[i] if i >= 0 else "?"
        if not self.has_mapping:
            st = "?"
        pos = n.start
        j = i + 1
        while pos < n.end:
            nxt = maddrs[j] if j < len(maddrs) and maddrs[j] < n.end \
                else n.end
            if nxt > pos:
                yield pos, nxt, st
            if nxt >= n.end:
                break
            st = mtypes[j]
            pos = nxt
            j += 1

    def _build_edges(self):
        self.succ = [set() for _ in self.nodes]
        for n in self.nodes:
            data = n.sec.data
            if data is None:
                continue
            base = n.sec.addr
            out = self.succ[n.id]
            last_arm = None        # (addr, word) of the last $a/? word
            last_thumb = None
            last_kind = None
            ptrs = n.isec.ptrs
            if ptrs is not None:
                i = bisect.bisect_left(ptrs, (n.start,))
                while i < len(ptrs) and ptrs[i][0] < n.end:
                    a, kind, hint = ptrs[i]
                    w = struct.unpack_from("<I", data, a - base)[0]
                    out |= self.reloc_targets(
                        a, w, kind, hint, self.relocw[(n.sec.idx, a)][2])
                    i += 1
            for a, b, st in self._states(n):
                if st == "?":
                    self.stats["unknown_state_bytes"] += b - a
                if st in ("d", "?") and ptrs is None:
                    p = (a + 3) & ~3
                    while p + 4 <= b:
                        w = struct.unpack_from("<I", data, p - base)[0]
                        out |= self.word_targets(w)
                        p += 4
                if st in ("a", "?"):
                    p = (a + 3) & ~3
                    while p + 4 <= b:
                        w = struct.unpack_from("<I", data, p - base)[0]
                        self._arm(n, p, w, out)
                        last_arm, last_kind = w, "a"
                        p += 4
                if st == "t":
                    p = (a + 1) & ~1
                    while p + 2 <= b:
                        h = struct.unpack_from("<H", data, p - base)[0]
                        if (h & 0xF800) == 0xF000 and p + 4 <= b:
                            h2 = struct.unpack_from("<H", data,
                                                    p + 2 - base)[0]
                            if (h2 & 0xE800) == 0xE800:   # bl / blx pair
                                off = ((h & 0x7FF) << 12) | \
                                    ((h2 & 0x7FF) << 1)
                                if off & 0x400000:
                                    off -= 0x800000
                                t = p + 4 + off
                                if (h2 & 0xF800) == 0xE800:
                                    t &= ~3                 # blx -> ARM
                                self._edge_to(n, t, out)
                                last_thumb, last_kind = h2, "t"
                                p += 4
                                continue
                        self._thumb(n, p, h, out)
                        last_thumb, last_kind = h, "t"
                        p += 2
            out.discard(n.id)
            # fall-through into the next piece of the same input section
            if n.code:
                term = True
                if last_kind == "a":
                    term = arm_terminates(last_arm)
                elif last_kind == "t":
                    term = thumb_terminates(last_thumb)
                if not term:
                    nxt = self.nodes[n.id + 1] if n.id + 1 < len(self.nodes) \
                        else None
                    if nxt is not None and nxt.isec is n.isec:
                        out.add(nxt.id)
                        self.stats["fallthrough"] += 1
            self.stats["edges"] += len(out)
        self.pred = [set() for _ in self.nodes]
        for i, s in enumerate(self.succ):
            for j in s:
                self.pred[j].add(i)

    def _edge_to(self, n, t, out):
        m = self.node_at(t, n.sec)
        if m is not None:
            out.add(m.id)

    def _arm(self, n, p, w, out):
        cond = w >> 28
        if (w & 0x0E000000) == 0x0A000000:            # b / bl / blx imm
            off = sign24(w & 0xFFFFFF) << 2
            t = p + 8 + off
            if cond == 0xF:
                t += 2 if w & 0x01000000 else 0       # blx: H bit, Thumb
            self._edge_to(n, t & 0xFFFFFFFF, out)
            return
        if cond == 0xF:
            return
        if (w & 0x0FEF0000) == 0x028F0000:            # add rd, pc, #imm
            self._edge_to(n, (p + 8 + arm_imm(w)) & 0xFFFFFFFF, out)
            return
        if (w & 0x0FEF0000) == 0x024F0000:            # sub rd, pc, #imm
            self._edge_to(n, (p + 8 - arm_imm(w)) & 0xFFFFFFFF, out)
            return
        if (w & 0x0F7F0000) == 0x051F0000:            # ldr rd, [pc, #+-imm]
            imm = w & 0xFFF
            t = p + 8 + (imm if w & 0x00800000 else -imm)
            self._edge_to(n, t, out)
            out |= self.literal_targets(t, n.sec)

    def _thumb(self, n, p, h, out):
        if (h & 0xF800) == 0xA000:                    # add rd, pc, #imm
            self._edge_to(n, ((p + 4) & ~3) + (h & 0xFF) * 4, out)
        elif (h & 0xF800) == 0x4800:                  # ldr rd, [pc, #imm]
            t = ((p + 4) & ~3) + (h & 0xFF) * 4
            self._edge_to(n, t, out)
            out |= self.literal_targets(t, n.sec)
        elif (h & 0xF000) == 0xD000 and (h & 0x0F00) < 0x0E00:   # b<cond>
            off = h & 0xFF
            off = off - 0x100 if off & 0x80 else off
            self._edge_to(n, p + 4 + off * 2, out)
        elif (h & 0xF800) == 0xE000:                  # b
            off = h & 0x7FF
            off = off - 0x800 if off & 0x400 else off
            self._edge_to(n, p + 4 + off * 2, out)

    # -- roots and reachability -----------------------------------------
    def nodes_named(self, name):
        return [n for n in self.nodes if any(s.name == name for s in n.syms)]

    def _roots(self, extra):
        roots = set()
        why = {}
        for n in self.nodes:
            base = os.path.basename(n.obj)
            if base.startswith("crt0") and base.endswith(".o"):
                roots.add(n.id); why.setdefault(n.id, n.name)
            if n.sec.name == ".vectors":
                roots.add(n.id); why.setdefault(n.id, n.name)
        for n in self.keep_nodes:
            roots.add(n.id); why.setdefault(n.id, n.name)
        for m in self.nodes_at(self.entry):
            if m.code:
                roots.add(m.id); why.setdefault(m.id, m.name)
        self.missing_roots = []
        for name in ("main", "irq_handler", "fiq_handler"):
            ns = self.nodes_named(name)
            if not ns:
                self.missing_roots.append(name)
            for m in ns:
                roots.add(m.id); why.setdefault(m.id, name)
        self.bad_roots = []
        for name in extra:
            ns = self.nodes_named(name)
            if not ns:
                self.bad_roots.append(name)
            for m in ns:
                roots.add(m.id); why.setdefault(m.id, name)
        self.roots = roots
        self.root_name = why

    def reach(self, removed=frozenset()):
        parent = {}
        q = deque()
        for r in sorted(self.roots):
            if r not in removed:
                parent[r] = None
                q.append(r)
        while q:
            x = q.popleft()
            for y in sorted(self.succ[x]):
                if y not in parent and y not in removed:
                    parent[y] = x
                    q.append(y)
        return parent


# ---------------------------------------------------------------- output
def parse_args(argv):
    p = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    p.add_argument("elf")
    p.add_argument("--map")
    p.add_argument("--root", action="append", default=[])
    p.add_argument("--objdump", default="arm-none-eabi-objdump")
    p.add_argument("--nm", default="arm-none-eabi-nm")
    p.add_argument("--reachable", action="store_true")
    p.add_argument("--unreachable", action="store_true")
    p.add_argument("--callers", action="append", default=[])
    p.add_argument("--cut", action="append", default=[])
    p.add_argument("--paths", action="append", default=[])
    p.add_argument("--json")
    p.add_argument("--words", choices=("auto", "value"), default="auto")
    return p.parse_args(argv)


def sym_lines(g, nodes):
    """(symbol, section, size, object) for each listed symbol of nodes,
    or one anonymous entry per node without one; address order."""
    rows = []
    for n in nodes:
        listed = [s for s in n.syms
                  if not (s.type == STT_NOTYPE and s.bind == STB_LOCAL)]
        if listed:
            for s in listed:
                size = s.size if s.size else n.end - n.start
                rows.append((s.value & ~1, n.sec.idx, s.name, n.isec.name,
                             size, g.rel(n.obj)))
        elif n.isec.name != "<gap>":    # linker fill: summary bytes only
            rows.append((n.start, n.sec.idx, n.name, n.isec.name,
                         n.end - n.start, g.rel(n.obj)))
    rows.sort(key=lambda r: (r[1], r[0], r[2]))
    return [r[2:] for r in rows]


def main(argv):
    a = parse_args(argv)
    mp = a.map or (os.path.splitext(a.elf)[0] + ".map")
    try:
        g = Graph(a.elf, mp, a.root, a.words)
    except BadInput as e:
        sys.stderr.write("reach.py: %s\n" % e)
        return 1
    if g.bad_roots:
        sys.stderr.write("reach.py: --root symbol(s) not in the image: %s\n"
                         % " ".join(g.bad_roots))
        return 1
    for name in g.missing_roots:
        sys.stderr.write("reach.py: note: default root %s not in the image\n"
                         % name)

    out = {}
    lines = []

    def emit(s):
        lines.append(s)
        print(s)

    removed = frozenset()
    cut_rec = None
    if a.cut:
        objs = sorted({n.obj for n in g.nodes})
        hit = set()
        for pat in a.cut:
            got = [o for o in objs if fnmatch.fnmatchcase(g.rel(o), pat)
                   or fnmatch.fnmatchcase(o, pat)]
            if not got:
                sys.stderr.write("reach.py: --cut %s matches no object in "
                                 "the map\n" % pat)
                return 1
            hit |= set(got)
        before = g.reach()
        removed = frozenset(n.id for n in g.nodes if n.obj in hit)
        after = g.reach(removed)
        also = [n for n in g.nodes
                if n.id in before and n.id not in after
                and n.id not in removed]
        img = sum(g.nodes[i].image for i in removed) + \
            sum(n.image for n in also)
        bss = sum(g.nodes[i].bss for i in removed) + sum(n.bss for n in also)
        cut_rec = {"image": img, "bss": bss, "objects": len(hit),
                   "also": len(also),
                   "cut_objects": sorted(g.rel(o) for o in hit),
                   "also_symbols": [list(r) for r in sym_lines(g, also)]}
        parent = after
    else:
        parent = g.reach()

    live = [n for n in g.nodes if n.id not in removed]
    image = sum(n.image for n in live)
    bss = sum(n.bss for n in live)
    r_img = sum(n.image for n in live if n.id in parent)
    r_bss = sum(n.bss for n in live if n.id in parent)
    summ = {"image": image, "bss": bss, "reachable": r_img,
            "unreachable": image - r_img, "reachable_bss": r_bss,
            "unreachable_bss": bss - r_bss}
    emit("summary image=%d bss=%d reachable=%d unreachable=%d "
         "reachable_bss=%d unreachable_bss=%d" % (
             image, bss, r_img, image - r_img, r_bss, bss - r_bss))
    out["summary"] = summ

    if a.reachable:
        rows = sym_lines(g, [n for n in live if n.id in parent])
        for r in rows:
            emit("reach %s %s %d %s" % r)
        out["reach"] = [list(r) for r in rows]
    if a.unreachable:
        rows = sym_lines(g, [n for n in live if n.id not in parent])
        for r in rows:
            emit("dead %s %s %d %s" % r)
        out["dead"] = [list(r) for r in rows]
    if a.callers:
        recs = []
        for name in a.callers:
            targets = [n for n in g.nodes_named(name) if n.id not in removed]
            if not targets:
                sys.stderr.write("reach.py: note: --callers %s: no such "
                                 "symbol in the image\n" % name)
            seen = set()
            dead = []
            for t in targets:
                for c in sorted(g.pred[t.id]):
                    if c in removed or c in seen or c == t.id:
                        continue
                    seen.add(c)
                    cn = g.nodes[c]
                    if c in parent:
                        emit("caller %s %s %s" % (name, cn.name,
                                                  g.rel(cn.obj)))
                        recs.append({"callee": name, "caller": cn.name,
                                     "object": g.rel(cn.obj), "live": True})
                    else:
                        dead.append({"callee": name, "caller": cn.name,
                                     "object": g.rel(cn.obj), "live": False})
            recs.extend(dead)
        out["callers"] = recs
    if a.paths:
        recs = []
        for name in a.paths:
            ts = [n for n in g.nodes_named(name)
                  if n.id not in removed and n.id in parent]
            if not ts:
                why = "unreachable" if g.nodes_named(name) else "not in image"
                sys.stderr.write("reach.py: note: --paths %s: %s\n"
                                 % (name, why))
                recs.append({"symbol": name, "path": None})
                continue

            def depth(i):
                d = 0
                while parent[i] is not None:
                    i = parent[i]
                    d += 1
                return d
            t = min(ts, key=lambda n: (depth(n.id), n.id))
            chain = []
            i = t.id
            while i is not None:
                chain.append(i)
                i = parent[i]
            chain.reverse()
            names = [g.root_name.get(chain[0], g.nodes[chain[0]].name)] + \
                [g.nodes[i].name for i in chain[1:-1]] + \
                ([name] if len(chain) > 1 else [])
            emit("path %s %s" % (name, " > ".join(names)))
            recs.append({"symbol": name, "path": names})
        out["paths"] = recs
    if cut_rec is not None:
        emit("cut image=%d bss=%d objects=%d also=%d" % (
            cut_rec["image"], cut_rec["bss"], cut_rec["objects"],
            cut_rec["also"]))
        out["cut"] = cut_rec

    st = g.stats
    sys.stderr.write("reach.py: stats nodes=%d edges=%d roots=%d "
                     "fallthrough=%d midcode=%d endptr_ambiguous=%d "
                     "unknown_state_bytes=%d reloc_isecs=%d value_isecs=%d "
                     "value_bytes=%d hint_outside=%d\n" % (
                         len(g.nodes), st["edges"], len(g.roots),
                         st["fallthrough"], st["midcode"],
                         st["endptr_ambiguous"], st["unknown_state_bytes"],
                         st["reloc_isecs"], st["value_isecs"],
                         st["value_bytes"], st["hint_outside"]))
    if a.json:
        out["stats"] = dict(st, nodes=len(g.nodes), roots=len(g.roots))
        out["lines"] = lines
        with open(a.json, "w") as f:
            json.dump(out, f, indent=1)
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
