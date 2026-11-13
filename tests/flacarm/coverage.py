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

"""Coverage of tests/flacarm/run.sh, from the encoder's own analysis.

    coverage.py DIR/in.ana...     (each DIR also holds in.flac)

The decoder's paths, as libffmpegFLAC/decoder.c chooses them, must all be
exercised by the streams that were checked bit for bit: every LPC order
1..32 and every shift 0..15 on the 64-bit ("wide") filter, which
decode_subframe_lpc takes unless sum(|qlp_coeff|) < 2^(32 - bps) for the
channel's own bps (one more on a side channel, less any wasted bits), and
orders 1..12 on the 32-bit one; subframes that bound sends to the 32-bit
filter although bps + qlp_coeff_precision + floor(log2(order)) exceeds 32
(the old test), and the bound's edge from both sides (sum(|c|) of exactly
2^(32 - bps) - 1 and 2^(32 - bps), on 16- and 24-bit streams); the
register-window loops of the wide filter (orders 2..4) with every count of
outputs left over (outputs % order), and order 3's with every shift 0..15
(one loop each); escaped partitions of every width 26..31, past the bit
reader's 25 bits a read; the wide filter with a 16-bit stream (the
remodulation pass); fixed orders 0..4; verbatim and constant subframes;
wasted bits; escaped Rice partitions; both Rice codings; partition orders 0
and above; all four channel assignments; block sizes 4096, 16384 and 65535.
Prints what it counted; exits 1 when anything is missing or nothing was
read.
"""
import re
import sys


def streaminfo_bps(path):
    d = open(path, "rb").read(42)
    assert d[:4] == b"fLaC"
    s = d[8:42]
    return ((s[12] & 1) << 4 | s[13] >> 4) + 1


def main():
    wide, narrow, fixed, wideq = set(), set(), set(), set()
    kinds, assigns, rice, bsz = set(), set(), set(), set()
    porders, wide16, wasted, escapes, subframes = set(), 0, 0, 0, 0
    bound_only, edges, winrest, w3q = 0, set(), set(), set()
    wide_esc = set()
    pending = None

    def lpc_done(p):
        nonlocal wide16, bound_only
        o, prec, q, cb, coef, bs, sbps = p
        if len(coef) != o:
            raise SystemExit("FAIL coverage: %d coefficients for order %d"
                             % (len(coef), o))
        a = sum(abs(c) for c in coef)
        if a == (1 << (32 - cb)) - 1:
            edges.add(("below", sbps))
        elif a == 1 << (32 - cb):
            edges.add(("at", sbps))
        if a >= 1 << (32 - cb):
            wide.add(o)
            wideq.add(q)
            wide16 += sbps <= 16
            if 2 <= o <= 4:
                winrest.add((o, (bs - o) % o))
            if o == 3 and bs - o >= 3:
                w3q.add(q)
        else:
            narrow.add(o)
            if sbps + prec + (o.bit_length() - 1) > 32:
                bound_only += 1

    for ana in sys.argv[1:]:
        bps = streaminfo_bps(ana[:-4] + ".flac")
        for line in open(ana):
            if pending and not line.startswith("\t\tqlp_coeff["):
                lpc_done(pending)
                pending = None
            m = re.match(r"frame=\d+.*blocksize=(\d+).*channel_assignment=(\w+)",
                         line)
            if m:
                bs = int(m.group(1))
                bsz.add(bs)
                assigns.add(m.group(2))
                side = {"LEFT_SIDE": 1, "RIGHT_SIDE": 0,
                        "MID_SIDE": 1}.get(m.group(2), -1)
                continue
            if pending:
                pending[4].append(int(line.split("=")[1]))
                continue
            if line.startswith("\tsubframe="):
                subframes += 1
                f = dict(kv.split("=", 1) for kv in line.split()
                         if "=" in kv)
                kinds.add(f["type"])
                if int(f["wasted_bits"]):
                    wasted += 1
                if "residual_type" in f:
                    rice.add(f["residual_type"])
                    porders.add(int(f["partition_order"]) > 0)
                if f["type"] == "FIXED":
                    fixed.add(int(f["order"]))
                elif f["type"] == "LPC":
                    cb = (bps + (int(f["subframe"]) == side)
                          - int(f["wasted_bits"]))
                    pending = (int(f["order"]), int(f["qlp_coeff_precision"]),
                               int(f["quantization_level"]), cb, [], bs, bps)
            elif "ESCAPE" in line:
                escapes += 1
                m = re.search(r"raw_bits=(\d+)", line)
                if m and int(m.group(1)) > 25:
                    wide_esc.add(int(m.group(1)))
        if pending:
            lpc_done(pending)
            pending = None
    missing = []
    need = [
        ("wide LPC orders", set(range(1, 33)) - wide),
        ("32-bit LPC orders", set(range(1, 13)) - narrow),
        ("wide LPC shifts", set(range(16)) - wideq),
        ("fixed orders", set(range(5)) - fixed),
        ("subframe types", {"CONSTANT", "VERBATIM", "FIXED", "LPC"} - kinds),
        ("channel assignments",
         {"INDEPENDENT", "LEFT_SIDE", "RIGHT_SIDE", "MID_SIDE"} - assigns),
        ("Rice codings", {"RICE", "RICE2"} - rice),
        ("partition orders (0, >0)", {False, True} - porders),
        ("block sizes", {4096, 16384, 65535} - bsz),
        ("bound edges", {(e, b) for e in ("below", "at") for b in (16, 24)}
         - edges),
        ("order-3 window shifts", set(range(16)) - w3q),
        ("escaped partitions of 26..31 bits", set(range(26, 32)) - wide_esc),
        ("window leftovers (order, outputs % order)",
         {(o, r) for o in (2, 3, 4) for r in range(o)} - winrest),
    ]
    for what, gone in need:
        if gone:
            missing.append("%s %s" % (what, sorted(gone)))
    for what, n in (("16-bit wide-LPC subframes", wide16),
                    ("wasted-bits subframes", wasted),
                    ("escaped partitions", escapes),
                    ("32-bit by the bound only", bound_only)):
        if not n:
            missing.append(what)
    print("coverage: %d streams, %d subframes; wide orders %d/32, 32-bit "
          "orders %d/12, 16-bit wide %d, wasted %d, escapes %d, 32-bit by "
          "the bound only %d, bound edges %d/4, window leftovers %d/9, order-3 "
          "window shifts %d/16, %s, %s"
          % (len(sys.argv) - 1, subframes, len(wide & set(range(1, 33))),
             len(narrow & set(range(1, 13))), wide16, wasted, escapes,
             bound_only, len(edges), len(winrest), len(w3q),
             "/".join(sorted(kinds)), "/".join(sorted(assigns))))
    if not subframes or missing:
        print("FAIL coverage: missing " + ("; ".join(missing) or "everything"))
        return 1
    print("ok   coverage complete")
    return 0


if __name__ == "__main__":
    sys.exit(main())
