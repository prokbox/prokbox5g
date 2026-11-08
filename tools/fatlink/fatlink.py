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
# fatlink: shared-cluster "links" on an unmounted FAT32 image (research, #137).
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
"""Shared-cluster links on a FAT32 image (host rehearsal for #137).

A "link" is a second directory entry whose first cluster and size equal an
existing file's, so one copy of the data has two names.  FAT has no link
support: this is a deliberate cross-link, managed through a sidecar table.

Works only on an UNMOUNTED raw image (or an MBR image; partition 1 is used).
Standard library only.

  fatlink.py IMG ls [DIR]
  fatlink.py IMG link ORIG LINKPATH [--attr RHS] [--table PATH]
  fatlink.py IMG unlink LINKPATH [--table PATH]     raw entry removal, FAT untouched
  fatlink.py IMG verify [--table PATH]              report only, exit 1 if not all OK
  fatlink.py IMG cleanup [--table PATH] [--no-repair]
  fatlink.py IMG park                               before USB: raw-remove every link, keep the table
  fatlink.py IMG unpark                             after USB: recreate links from the table's originals
  fatlink.py IMG scan                               whole-volume cross-link / free-in-chain scan
  fatlink.py IMG chain PATH                         print a file's cluster chain
  fatlink.py IMG cat PATH                           file bytes to stdout (by size, via chain)
  fatlink.py IMG dirty [set|clear]                  FAT[1] clean-shutdown bit
  fatlink.py IMG attr PATH [RHSA|-]                 show or set attributes

The sidecar table (default /.rockbox/fatlinks.tsv) holds one line per link:
  link_path <TAB> orig_path <TAB> first_cluster <TAB> size <TAB> extents
extents = "start+count,..." of the original's chain when the link was made;
it lets the cleanup rebuild a chain the host freed (hazard H3).
"""

import math
import os
import struct
import sys

ATTR_RO, ATTR_HID, ATTR_SYS, ATTR_VOL, ATTR_DIR, ATTR_ARC = 1, 2, 4, 8, 0x10, 0x20
ATTR_LFN = 0x0F
EOC = 0x0FFFFFF8
BAD = 0x0FFFFFF7
DEFAULT_TABLE = "/.rockbox/fatlinks.tsv"


class FatError(Exception):
    pass


class Entry:
    """A directory entry: SFN fields plus the byte offsets of its LFN+SFN slots."""

    def __init__(self, name, sfn, attr, fst, size, slots, raw):
        self.name, self.sfn, self.attr = name, sfn, attr
        self.fst, self.size, self.slots, self.raw = fst, size, slots, raw

    @property
    def is_dir(self):
        return bool(self.attr & ATTR_DIR)

    def __repr__(self):
        return "Entry(%r fst=%d size=%d attr=%#x)" % (self.name, self.fst, self.size, self.attr)


def _lfn_checksum(sfn11):
    s = 0
    for b in sfn11:
        s = (((s & 1) << 7) + (s >> 1) + b) & 0xFF
    return s


class Volume:
    def __init__(self, path, writable=True):
        self.f = open(path, "r+b" if writable else "rb")
        self.base = 0
        b = self._pread(0, 512)
        if b[0] not in (0xEB, 0xE9) and b[510:512] == b"\x55\xaa":
            lba = struct.unpack_from("<I", b, 446 + 8)[0]
            self.base = lba * 512
            b = self._pread(self.base, 512)
        self.bps = struct.unpack_from("<H", b, 11)[0]
        self.spc = b[13]
        self.rsvd = struct.unpack_from("<H", b, 14)[0]
        self.nfats = b[16]
        self.totsec = struct.unpack_from("<I", b, 32)[0]
        self.fatsz = struct.unpack_from("<I", b, 36)[0]
        self.rootclus = struct.unpack_from("<I", b, 44)[0]
        self.fsinfo_sec = struct.unpack_from("<H", b, 48)[0]
        if self.fatsz == 0 or struct.unpack_from("<H", b, 22)[0] != 0:
            raise FatError("not FAT32")
        self.cs = self.bps * self.spc
        self.fat_off = self.base + self.rsvd * self.bps
        self.data_off = self.fat_off + self.nfats * self.fatsz * self.bps
        self.nclus = (self.totsec - self.rsvd - self.nfats * self.fatsz) // self.spc
        self.maxclus = self.nclus + 1
        raw = self._pread(self.fat_off, (self.maxclus + 1) * 4)
        self.fat = list(struct.unpack("<%dI" % (self.maxclus + 1), raw))

    # -- low level ---------------------------------------------------------
    def _pread(self, off, n):
        self.f.seek(off)
        return self.f.read(n)

    def _pwrite(self, off, data):
        self.f.seek(off)
        self.f.write(data)

    def close(self):
        self.f.flush()
        os.fsync(self.f.fileno())
        self.f.close()

    def get(self, c):
        return self.fat[c] & 0x0FFFFFFF

    def put(self, c, v):
        """Write FAT[c] in every FAT copy, keeping the top four bits."""
        v = (self.fat[c] & 0xF0000000) | (v & 0x0FFFFFFF)
        self.fat[c] = v
        for i in range(self.nfats):
            self._pwrite(self.fat_off + i * self.fatsz * self.bps + c * 4, struct.pack("<I", v))

    def coff(self, c):
        return self.data_off + (c - 2) * self.cs

    def read_cluster(self, c):
        return self._pread(self.coff(c), self.cs)

    def chain(self, c, limit=None):
        """Follow FAT from c.  Returns (clusters, end) where end is 'eoc',
        'free' (hit a 0 entry), 'bad', 'range', 'loop' or 'limit'."""
        out, seen = [], set()
        if c < 2 or c > self.maxclus:
            return out, ("empty" if c == 0 else "range")
        while True:
            if c in seen:
                return out, "loop"
            seen.add(c)
            out.append(c)
            if limit is not None and len(out) >= limit:
                nxt = self.get(c)
                return out, ("eoc" if nxt >= EOC else "limit")
            nxt = self.get(c)
            if nxt >= EOC:
                return out, "eoc"
            if nxt == 0:
                return out, "free"
            if nxt == BAD:
                return out, "bad"
            if nxt < 2 or nxt > self.maxclus:
                return out, "range"
            c = nxt

    def nclusters_for(self, size):
        return max(1, math.ceil(size / self.cs)) if size else 0

    # -- FSInfo / dirty bit ------------------------------------------------
    def _fsinfo(self):
        off = self.base + self.fsinfo_sec * self.bps
        b = self._pread(off, 512)
        if b[0:4] != b"RRaA" or b[484:488] != b"rrAa":
            return None, None, None
        return off, struct.unpack_from("<I", b, 488)[0], struct.unpack_from("<I", b, 492)[0]

    def _fsinfo_adjust(self, delta, hint=None):
        off, free, nxt = self._fsinfo()
        if off is None:
            return
        if free != 0xFFFFFFFF:
            self._pwrite(off + 488, struct.pack("<I", free + delta))
        if hint is not None:
            self._pwrite(off + 492, struct.pack("<I", hint))

    def dirty(self):
        return not (self.fat[1] & 0x08000000)

    def set_dirty(self, d):
        v = self.fat[1]
        v = (v & ~0x08000000) if d else (v | 0x08000000)
        self.fat[1] = v
        for i in range(self.nfats):
            self._pwrite(self.fat_off + i * self.fatsz * self.bps + 4, struct.pack("<I", v))

    # -- allocation (only used for directories and the sidecar table) -------
    def alloc(self, n):
        off, free, hint = self._fsinfo()
        start = hint if hint and 2 <= hint <= self.maxclus else 2
        got = []
        c = start
        for _ in range(self.nclus):
            if self.get(c) == 0:
                got.append(c)
                if len(got) == n:
                    break
            c = c + 1 if c < self.maxclus else 2
        if len(got) < n:
            raise FatError("volume full")
        for a, b in zip(got, got[1:]):
            self.put(a, b)
        self.put(got[-1], 0x0FFFFFFF)
        self._fsinfo_adjust(-n, got[-1])
        return got

    def free_chain(self, c):
        cl, _ = self.chain(c)
        for x in cl:
            self.put(x, 0)
        self._fsinfo_adjust(len(cl))

    # -- directories -------------------------------------------------------
    def dir_slots(self, dclus):
        """Yield (byte_offset, 32-byte slot) over the directory chain."""
        cl, _ = self.chain(dclus)
        for c in cl:
            data = self.read_cluster(c)
            base = self.coff(c)
            for i in range(0, self.cs, 32):
                yield base + i, data[i:i + 32]

    def read_dir(self, dclus):
        out, lfn, lfn_slots, lfn_sum = [], {}, [], None
        for off, s in self.dir_slots(dclus):
            if s[0] == 0:
                break
            if s[0] == 0xE5:
                lfn, lfn_slots = {}, []
                continue
            attr = s[11]
            if attr == ATTR_LFN:
                seq = s[0] & 0x1F
                if s[0] & 0x40:
                    lfn, lfn_slots, lfn_sum = {}, [], s[13]
                part = s[1:11] + s[14:26] + s[28:32]
                lfn[seq] = part
                lfn_slots.append(off)
                continue
            if attr & ATTR_VOL:
                lfn, lfn_slots = {}, []
                continue
            sfn11 = s[0:11]
            base_n = sfn11[0:8].decode("latin-1").rstrip()
            ext = sfn11[8:11].decode("latin-1").rstrip()
            if base_n.startswith("\x05"):
                base_n = "\xe5" + base_n[1:]
            sfn = base_n + ("." + ext if ext else "")
            name = sfn
            if lfn and lfn_sum == _lfn_checksum(sfn11):
                u = b"".join(lfn[k] for k in sorted(lfn))
                name = u.decode("utf-16-le", "replace").split("\x00")[0]
            else:
                lfn_slots = []
            fst = (struct.unpack_from("<H", s, 20)[0] << 16) | struct.unpack_from("<H", s, 26)[0]
            size = struct.unpack_from("<I", s, 28)[0]
            out.append(Entry(name, sfn, attr, fst, size, lfn_slots + [off], s))
            lfn, lfn_slots = {}, []
        return out

    def walk(self, dclus=None, prefix=""):
        dclus = dclus or self.rootclus
        for e in self.read_dir(dclus):
            if e.name in (".", ".."):
                continue
            p = prefix + "/" + e.name
            yield p, e
            if e.is_dir and e.fst >= 2:
                yield from self.walk(e.fst, p)

    def lookup(self, path):
        """Return (Entry, parent_dir_cluster) or (None, parent) / (None, None)."""
        parts = [p for p in path.split("/") if p]
        d = self.rootclus
        e = None
        for i, part in enumerate(parts):
            found = None
            for x in self.read_dir(d):
                if x.name.lower() == part.lower() or x.sfn.lower() == part.lower():
                    found = x
                    break
            if found is None:
                return None, (d if i == len(parts) - 1 else None)
            if i < len(parts) - 1:
                if not found.is_dir:
                    return None, None
                d = found.fst or self.rootclus
            e = found
        return e, d

    def _make_sfn(self, dclus, longname):
        existing = {x.sfn.upper() for x in self.read_dir(dclus)}
        stem, _, ext = longname.rpartition(".")
        if not stem:
            stem, ext = longname, ""
        keep = lambda s: "".join(ch for ch in s.upper() if ch.isalnum() or ch in "_-")
        stem, ext = keep(stem) or "LINK", keep(ext)[:3]
        if len(stem) <= 8 and stem + ("." + ext if ext else "") == longname.upper() \
                and stem + ("." + ext if ext else "") not in existing and longname == longname.upper():
            return stem.ljust(8) + ext.ljust(3), False
        for n in range(1, 1000000):
            tail = "~%d" % n
            s = stem[:8 - len(tail)] + tail
            if s + ("." + ext if ext else "") not in existing:
                return s.ljust(8) + ext.ljust(3), True
        raise FatError("no free short name")

    def _free_run(self, dclus, need):
        run = []
        last = None
        for off, s in self.dir_slots(dclus):
            if s[0] in (0, 0xE5):
                run.append(off)
                if len(run) == need:
                    return run
            else:
                run = []
            last = off
        # extend the directory by one cluster
        cl, _ = self.chain(dclus)
        nc = self.alloc(1)[0]
        self._pwrite(self.coff(nc), bytes(self.cs))
        self.put(cl[-1], nc)
        tail = [o for o in run]
        return (tail + [self.coff(nc) + 32 * i for i in range(need)])[:need]

    def add_entry(self, dclus, longname, attr, fst, size, template=None):
        sfn11, need_lfn = self._make_sfn(dclus, longname)
        sfn11 = sfn11.encode("latin-1")
        slots = []
        if need_lfn:
            u = longname.encode("utf-16-le")
            nch = len(longname)
            nent = (nch + 12) // 13
            u = u + b"\x00\x00" + b"\xff\xff" * (nent * 13)
            ck = _lfn_checksum(sfn11)
            for seq in range(nent, 0, -1):
                chunk = u[(seq - 1) * 26:(seq - 1) * 26 + 26]
                if (seq - 1) * 13 >= nch + 1:
                    chunk = b"\xff\xff" * 13
                e = bytearray(32)
                e[0] = seq | (0x40 if seq == nent else 0)
                e[1:11] = chunk[0:10]
                e[11] = ATTR_LFN
                e[13] = ck
                e[14:26] = chunk[10:22]
                e[28:32] = chunk[22:26]
                slots.append(bytes(e))
        e = bytearray(template if template is not None else bytes(32))
        e[0:11] = sfn11
        e[11] = attr
        e[12] = 0
        struct.pack_into("<H", e, 20, fst >> 16)
        struct.pack_into("<H", e, 26, fst & 0xFFFF)
        struct.pack_into("<I", e, 28, size)
        slots.append(bytes(e))
        where = self._free_run(dclus, len(slots))
        for off, data in zip(where, slots):
            self._pwrite(off, data)
        return where

    def raw_remove(self, entry):
        """Mark the entry's LFN and SFN slots deleted.  The FAT is NOT touched."""
        for off in entry.slots:
            self._pwrite(off, b"\xe5")

    def set_first_and_size(self, entry, fst, size):
        off = entry.slots[-1]
        self._pwrite(off + 20, struct.pack("<H", fst >> 16))
        self._pwrite(off + 26, struct.pack("<H", fst & 0xFFFF))
        self._pwrite(off + 28, struct.pack("<I", size))

    def set_attr(self, entry, attr):
        self._pwrite(entry.slots[-1] + 11, bytes([attr]))

    def mkdir_p(self, path):
        parts = [p for p in path.split("/") if p]
        d = self.rootclus
        for part in parts:
            hit = [x for x in self.read_dir(d) if x.name.lower() == part.lower()]
            if hit:
                d = hit[0].fst
                continue
            nc = self.alloc(1)[0]
            buf = bytearray(self.cs)
            dot = bytearray(32)
            dot[0:11] = b".          "
            dot[11] = ATTR_DIR
            struct.pack_into("<H", dot, 20, nc >> 16)
            struct.pack_into("<H", dot, 26, nc & 0xFFFF)
            dd = bytearray(32)
            dd[0:11] = b"..         "
            dd[11] = ATTR_DIR
            pc = 0 if d == self.rootclus else d
            struct.pack_into("<H", dd, 20, pc >> 16)
            struct.pack_into("<H", dd, 26, pc & 0xFFFF)
            buf[0:32], buf[32:64] = dot, dd
            self._pwrite(self.coff(nc), bytes(buf))
            attr = ATTR_DIR | (ATTR_HID if part.startswith(".") else 0)
            self.add_entry(d, part, attr, nc, 0)
            d = nc
        return d

    # -- whole files (sidecar table only) ----------------------------------
    def read_file(self, path):
        e, _ = self.lookup(path)
        if e is None:
            return None
        return self.read_entry(e)

    def read_entry(self, e):
        if e.size == 0:
            return b""
        cl, _ = self.chain(e.fst, self.nclusters_for(e.size))
        data = b"".join(self.read_cluster(c) for c in cl)
        return data[:e.size]

    def write_file(self, path, data):
        """Replace a small file (normal delete then create)."""
        dirpath, _, name = path.rpartition("/")
        e, d = self.lookup(path)
        if e is not None:
            if e.fst >= 2:
                self.free_chain(e.fst)
            self.raw_remove(e)
        d = self.mkdir_p(dirpath) if dirpath else self.rootclus
        fst = 0
        if data:
            cl = self.alloc(self.nclusters_for(len(data)))
            fst = cl[0]
            for i, c in enumerate(cl):
                self._pwrite(self.coff(c), data[i * self.cs:(i + 1) * self.cs].ljust(self.cs, b"\0"))
        self.add_entry(d, name, ATTR_ARC, fst, len(data))


# -- link table -------------------------------------------------------------
def extents(clusters):
    out = []
    for c in clusters:
        if out and out[-1][0] + out[-1][1] == c:
            out[-1][1] += 1
        else:
            out.append([c, 1])
    return ",".join("%d+%d" % (a, n) for a, n in out)


def expand(ext):
    out = []
    for part in ext.split(","):
        if part:
            a, n = part.split("+")
            out.extend(range(int(a), int(a) + int(n)))
    return out


def load_table(v, table):
    data = v.read_file(table)
    recs = []
    if data:
        for line in data.decode("utf-8").splitlines():
            if line and not line.startswith("#"):
                lp, op, fst, size, ext = line.split("\t")
                recs.append({"link": lp, "orig": op, "fst": int(fst), "size": int(size), "ext": ext})
    return recs


def save_table(v, table, recs):
    lines = ["# link\torig\tfirst_cluster\tsize\textents"]
    lines += ["%s\t%s\t%d\t%d\t%s" % (r["link"], r["orig"], r["fst"], r["size"], r["ext"]) for r in recs]
    v.write_file(table, ("\n".join(lines) + "\n").encode("utf-8"))


def parse_attr(s):
    a = ATTR_ARC
    for ch in (s or "").upper():
        a |= {"R": ATTR_RO, "H": ATTR_HID, "S": ATTR_SYS, "A": ATTR_ARC}[ch]
    return a


def make_link(v, orig, linkpath, attr, table, recs=None):
    o, _ = v.lookup(orig)
    if o is None or o.is_dir:
        raise FatError("original not found: %s" % orig)
    cl, end = v.chain(o.fst, v.nclusters_for(o.size))
    if end != "eoc" or len(cl) != v.nclusters_for(o.size):
        raise FatError("original chain unhealthy (%s): %s" % (end, orig))
    dirpath, _, name = linkpath.rpartition("/")
    d = v.mkdir_p(dirpath) if dirpath else v.rootclus
    if v.lookup(linkpath)[0] is not None:
        raise FatError("link path exists: %s" % linkpath)
    v.add_entry(d, name, attr, o.fst, o.size, template=o.raw)
    own = recs is None
    if own:
        recs = load_table(v, table)
    recs = [r for r in recs if r["link"].lower() != linkpath.lower()]
    recs.append({"link": linkpath, "orig": orig, "fst": o.fst, "size": o.size, "ext": extents(cl)})
    if own:
        save_table(v, table, recs)
    return recs


def owners_map(v, skip=()):
    """cluster -> list of paths (whole-tree walk).  Directory chains included."""
    own = {}
    for c in v.chain(v.rootclus)[0]:
        own.setdefault(c, []).append("/")
    for p, e in v.walk():
        if e.fst < 2:
            continue
        n = None if e.is_dir else v.nclusters_for(e.size)
        cl, _ = v.chain(e.fst, n)
        for c in cl:
            own.setdefault(c, []).append(p)
    return own


def check_link(v, r):
    """Classify one record.  Returns (status, detail, link_entry, orig_entry)."""
    L, _ = v.lookup(r["link"])
    O, _ = v.lookup(r["orig"])
    want = expand(r["ext"])
    if O is None:
        return "ORPHAN", "original gone", L, None
    if L is not None and (L.fst != r["fst"] or L.size != r["size"]):
        return "FOREIGN", "link path now holds another file", L, O
    if O.fst == 0 and O.size == 0 and L is not None and L.fst == r["fst"] and L.size == r["size"]:
        lc, lend = v.chain(L.fst, len(want))
        if lend == "eoc" and lc == want:
            # a disk checker (fsck_msdos -y, chkdsk) cut the cross-link by
            # emptying the original; the data survives under the link.
            return "EMPTIED", "original emptied by a disk checker; data intact under the link", L, O
    n = v.nclusters_for(O.size)
    cl, end = v.chain(O.fst, n)
    healthy = end == "eoc" and len(cl) == n
    if O.fst != r["fst"] or O.size != r["size"]:
        if healthy:
            return "STALE", "original changed (fst %d->%d, size %d->%d)" % (
                r["fst"], O.fst, r["size"], O.size), L, O
        return "DAMAGED", "original changed and its chain is broken (%s)" % end, L, O
    if not healthy:
        # same first cluster and size, but the chain is free or diverges:
        # somebody freed the shared clusters (the link was deleted, H3).
        free = [c for c in want if v.get(c) == 0]
        if len(free) == len(want):
            return "FREED", "original's whole chain is free (link deleted by host); repairable", L, O
        return "DAMAGED", "original's chain %s after %d of %d clusters; %d/%d recorded clusters free" % (
            end, len(cl), n, len(free), len(want)), L, O
    if cl != want:
        # healthy chain, same fst/size, different clusters: rewritten in place
        return "STALE", "original's chain moved (same fst/size)", L, O
    if L is None:
        return "LINK_GONE", "link entry missing, original intact (moved or renamed?)", None, O
    return "OK", "", L, O


def cmd_verify(v, table, fix, repair=True, log=print):
    recs = load_table(v, table)
    out, bad = [], 0
    for r in recs:
        st, det, L, O = check_link(v, r)
        log("%-9s %s -> %s %s" % (st, r["link"], r["orig"], ("(" + det + ")") if det else ""))
        if st != "OK":
            bad += 1
        if not fix:
            out.append(r)
            continue
        if st == "OK":
            out.append(r)
        elif st == "ORPHAN":
            if L is not None and L.fst == r["fst"]:
                v.raw_remove(L)
            log("  removed link entry (FAT untouched)")
        elif st == "STALE":
            if L is not None:
                v.raw_remove(L)
            out = make_link(v, r["orig"], r["link"], L.attr if L else ATTR_ARC | ATTR_RO | ATTR_HID, table, out)
            log("  rebuilt link")
        elif st == "FOREIGN":
            log("  link path reused by another file; record dropped, entry left alone")
        elif st == "LINK_GONE":
            own = owners_map(v)
            aliases = [p for p in own.get(r["fst"], []) if p.lower() != r["orig"].lower()]
            for p in aliases:
                e, _ = v.lookup(p)
                if e and e.fst == r["fst"] and not e.is_dir:
                    v.raw_remove(e)
                    log("  removed stray alias %s (FAT untouched)" % p)
            out = make_link(v, r["orig"], r["link"], ATTR_ARC | ATTR_RO | ATTR_HID, table, out)
            log("  recreated link")
        elif st == "FREED":
            if repair:
                want = expand(r["ext"])
                for a, b in zip(want, want[1:]):
                    v.put(a, b)
                v.put(want[-1], 0x0FFFFFFF)
                v._fsinfo_adjust(-len(want))
                log("  REPAIRED original's chain from the table (%d clusters)" % len(want))
                out = make_link(v, r["orig"], r["link"], ATTR_ARC | ATTR_RO | ATTR_HID, table, out)
                log("  recreated link")
            else:
                log("  not repaired (--no-repair); record kept")
                out.append(r)
        elif st == "EMPTIED":
            v.set_first_and_size(O, r["fst"], r["size"])
            log("  restored original's first cluster and size from the link")
            out.append(r)
        elif st == "DAMAGED":
            if L is not None and L.fst == r["fst"]:
                v.raw_remove(L)
            log("  ORIGINAL DAMAGED: link removed, original left for the user/fsck; logged")
    if fix:
        save_table(v, table, out)
    return bad


def cmd_park(v, table, log=print):
    """Remove every live link entry (raw, FAT untouched) so the host never
    sees a cross-link.  Records stay; unpark recreates them."""
    recs = load_table(v, table)
    n = 0
    for r in recs:
        L, _ = v.lookup(r["link"])
        if L is not None and L.fst == r["fst"] and L.size == r["size"]:
            v.raw_remove(L)
            n += 1
    log("parked %d of %d links" % (n, len(recs)))
    return n


def cmd_unpark(v, table, log=print):
    """Recreate links for originals that are still present and healthy, using
    their CURRENT first cluster and size; drop the rest."""
    recs = load_table(v, table)
    out, made, dropped = [], 0, 0
    for r in recs:
        L, _ = v.lookup(r["link"])
        if L is not None:
            if L.fst == r["fst"] and L.size == r["size"]:
                v.raw_remove(L)          # still live (park missed): rebuild
            else:
                log("  link path taken by another file, record dropped: %s" % r["link"])
                dropped += 1
                continue
        try:
            out = make_link(v, r["orig"], r["link"], ATTR_ARC | ATTR_RO | ATTR_HID, table, out)
            made += 1
        except FatError as ex:
            log("  dropped %s (%s)" % (r["link"], ex))
            dropped += 1
    save_table(v, table, out)
    log("unparked %d links, dropped %d" % (made, dropped))
    return made, dropped


def cmd_scan(v, log=print):
    own = owners_map(v)
    cross = {}
    for c, ps in own.items():
        if len(ps) > 1:
            cross.setdefault(tuple(sorted(ps)), []).append(c)
    for ps, cl in sorted(cross.items()):
        log("CROSS %d clusters shared by %s" % (len(cl), " | ".join(ps)))
    nbad = 0
    for p, e in v.walk():
        if e.is_dir or e.fst < 2:
            continue
        n = v.nclusters_for(e.size)
        cl, end = v.chain(e.fst, n)
        if end != "eoc" or len(cl) != n:
            nbad += 1
            log("BROKEN %s: chain %s after %d of %d clusters" % (p, end, len(cl), n))
    lost = sum(1 for c in range(2, v.maxclus + 1) if v.get(c) not in (0, BAD) and c not in own)
    log("summary: %d cross-linked groups, %d broken chains, %d lost clusters" % (len(cross), nbad, lost))
    return len(cross), nbad, lost


def main(argv):
    if len(argv) < 3:
        print(__doc__)
        return 2
    img, cmd, rest = argv[1], argv[2], argv[3:]
    table = DEFAULT_TABLE
    attr = ATTR_ARC | ATTR_RO | ATTR_HID
    repair = True
    args = []
    i = 0
    while i < len(rest):
        if rest[i] == "--table":
            table = rest[i + 1]
            i += 2
        elif rest[i] == "--attr":
            attr = parse_attr(rest[i + 1])
            i += 2
        elif rest[i] == "--no-repair":
            repair = False
            i += 1
        else:
            args.append(rest[i])
            i += 1
    v = Volume(img, writable=cmd not in ("ls", "verify", "scan", "chain", "cat"))
    rc = 0
    try:
        if cmd == "ls":
            e, _ = v.lookup(args[0]) if args and args[0] != "/" else (None, None)
            d = e.fst if e else v.rootclus
            for x in v.read_dir(d):
                print("%-40s %-12s attr=%02x fst=%-8d size=%d" % (x.name, x.sfn, x.attr, x.fst, x.size))
        elif cmd == "link":
            make_link(v, args[0], args[1], attr, table)
        elif cmd == "unlink":
            e, _ = v.lookup(args[0])
            if e is None:
                raise FatError("no such link")
            v.raw_remove(e)
            recs = [r for r in load_table(v, table) if r["link"].lower() != args[0].lower()]
            save_table(v, table, recs)
        elif cmd == "verify":
            rc = 1 if cmd_verify(v, table, fix=False) else 0
        elif cmd == "cleanup":
            cmd_verify(v, table, fix=True, repair=repair)
        elif cmd == "park":
            cmd_park(v, table)
        elif cmd == "unpark":
            cmd_unpark(v, table)
        elif cmd == "scan":
            x, y, z = cmd_scan(v)
            rc = 1 if (x or y or z) else 0
        elif cmd == "chain":
            e, _ = v.lookup(args[0])
            cl, end = v.chain(e.fst, v.nclusters_for(e.size) if not e.is_dir else None)
            print("fst=%d size=%d clusters=%s end=%s" % (e.fst, e.size, extents(cl), end))
        elif cmd == "cat":
            e, _ = v.lookup(args[0])
            sys.stdout.buffer.write(v.read_entry(e))
        elif cmd == "dirty":
            if args:
                v.set_dirty(args[0] == "set")
            print("dirty" if v.dirty() else "clean")
        elif cmd == "attr":
            e, _ = v.lookup(args[0])
            if len(args) > 1:
                v.set_attr(e, parse_attr(args[1]) if args[1] != "-" else 0)
            print("%#04x" % (e.attr if len(args) == 1 else (parse_attr(args[1]) if args[1] != "-" else 0)))
        else:
            print(__doc__)
            rc = 2
    except FatError as ex:
        print("fatlink: %s" % ex, file=sys.stderr)
        rc = 3
    finally:
        v.close() if v.f.writable() else v.f.close()
    return rc


if __name__ == "__main__":
    sys.exit(main(sys.argv))
