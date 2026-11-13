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

"""Writes arm_lpc_pair.inc: the 64-bit LPC filter two outputs at a time.

    python3 lib/rbcodec/codecs/libffmpegFLAC/arm_lpc_pair.py

arm.S includes the output; regenerate it after editing this file (the file
it writes says so at the top, and tests/flacarm/run.sh checks the two agree).

The filter (flac_lpc_32_arm's contract, see arm.S) is a recursion: output i
is a sum over the order N samples before it, so output i + 1 needs output i.
But all of its other N - 1 taps are samples output i reads too.  So one pass
reads each history sample and each coefficient once and feeds two
accumulators:

    A = sum_{j=0}^{N-1} c[j] x[i-1-j]       output i     = r[i]   + (A >> q)
    B = sum_{j=1}^{N-1} c[j] x[i-j]         output i + 1 = r[i+1] + ((B + c[0] x[i]) >> q)

and the one tap B lacks is added once output i exists.  The single-output
loops load every sample and coefficient again for every output; this halves
those loads (24 words a pair at order 12 instead of 48), and pays the loop
overhead once per two outputs.  The multiplies are the same, 2N per pair.

Registers in every loop:

    r0      history pointer: x[i-N] at the top of a pass
    r1      coefficient pointer, walking down from coeffs + N
    r2:r3   A (r2 low)              r4:r5   B
    r12     ((pairs left - 1) << 6) | (odd << 5) | qlevel
    r6..r11, lr   samples and coefficients in flight

Samples come in ascending, three to an ldm; coefficients descending, three
to an ldmdb.  Sample m (oldest first) multiplies c[N-1-m] into A and
c[N-m] into B, so each coefficient is used by two consecutive samples and
one of them is carried from one group to the next: three samples, three
coefficients and the carried one are the seven free registers exactly.  The
coefficient goes in Rs of the multiply, as in arm.S, for the ARM7TDMI's
early-terminating multiplier (two passes for a 15-bit coefficient).
"""
import os

ORDERS = range(5, 13)   # 2..4: the register-window loops in arm.S
POOL = ["r6", "r7", "r8", "r9", "r10", "r11", "lr"]
REGNUM = {r: i for i, r in enumerate(POOL)}   # lr sorts last, as ldm does


def reglist(regs):
    return "{ " + ", ".join(sorted(regs, key=REGNUM.get)) + " }"


def order_loop(n):
    out = []
    e = out.append
    e("@ order %d: %d multiplies, %d words loaded a pair" % (n, 2 * n, 2 * n + 2))
    e(".Lp_order%d:" % n)
    e("    add     r1, r1, #%d" % (4 * n))
    e(".Lp_loop%d:" % n)
    free = list(POOL)
    carried = None          # (register, coefficient index)
    m = 0
    while m < n:
        k = min(3, n - m)
        # samples m..m+k-1, ascending registers for ascending addresses
        sregs = sorted(free[:k], key=REGNUM.get)
        free = free[k:]
        # coefficients N-1-m down to N-k-m; the lowest index in the lowest
        # register (ldmdb fills ascending registers from the lowest address)
        cregs = sorted(free[:k], key=REGNUM.get)
        free = free[k:]
        cidx = list(range(n - k - m, n - m))       # ascending
        coef = dict(zip(cidx, cregs))
        if carried:
            coef[carried[1]] = carried[0]
        e("    ldmia   r0!, %s" % reglist(sregs))
        e("    ldmdb   r1!, %s" % reglist(cregs))
        for j in range(k):
            mm = m + j
            s = sregs[j]
            a = "smull" if mm == 0 else "smlal"
            e("    %s   r2, r3, %s, %s" % (a, s, coef[n - 1 - mm]))
            if mm >= 1:
                b = "smull" if mm == 1 else "smlal"
                e("    %s   r4, r5, %s, %s" % (b, s, coef[n - mm]))
        # free the samples and every coefficient but the last one loaded
        last = n - k - m                            # the lowest index
        for i, r in coef.items():
            if i != last:
                free.append(r)
        free += sregs
        carried = (coef[last], last)
        free = sorted(set(free), key=REGNUM.get)
        m += k
    c0 = carried[0]
    free = [r for r in POOL if r != c0]
    ra, rb, t, t2 = free[:4]
    e("    ldmia   r0, { %s, %s }" % (ra, rb) + "          @ r[i], r[i+1]")
    e("    and     %s, r12, #31" % t + "              @ qlevel")
    e("    rsb     %s, %s, #32" % (t2, t))
    # the two halves of A >> qlevel share no bits, so adding each one is
    # the OR of them plus the residual (a shift by 32 gives 0)
    e("    add     %s, %s, r2, lsr %s" % (ra, ra, t) + "      @ output i = r[i] + (A >> qlevel)")
    e("    add     %s, %s, r3, lsl %s" % (ra, ra, t2))
    e("    smlal   r4, r5, %s, %s" % (ra, c0) + "          @ B's last tap: c[0] * output i")
    e("    add     %s, %s, r4, lsr %s" % (rb, rb, t) + "      @ output i + 1")
    e("    add     %s, %s, r5, lsl %s" % (rb, rb, t2))
    e("    stmia   r0, { %s, %s }" % (ra, rb))
    e("    sub     r0, r0, #%d" % (4 * (n - 2)) + "             @ on two samples")
    e("    add     r1, r1, #%d" % (4 * n))
    e("    subs    r12, r12, #64")
    e("    bpl     .Lp_loop%d" % n)
    e("    mov     r2, #%d" % n)
    e("    b       .Lp_odd")
    return out


def main():
    here = os.path.dirname(os.path.abspath(__file__))
    lines = [
        "/* Generated by arm_lpc_pair.py -- do not edit; edit the generator and",
        "   run it again.  Included by arm.S, in flac_lpc_32_arm. */",
        "",
        "@ jump table for orders %d..%d, entered with r2 = order" % (ORDERS[0], ORDERS[-1]),
        ".Lp_table:",
    ]
    for n in ORDERS:
        lines.append("    b       .Lp_order%d" % n)
    lines.append("")
    for n in ORDERS:
        lines += order_loop(n)
        lines.append("")
    open(os.path.join(here, "arm_lpc_pair.inc"), "w").write("\n".join(lines))


if __name__ == "__main__":
    main()
