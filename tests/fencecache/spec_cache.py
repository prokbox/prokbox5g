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

# tests/fencecache: srcpatch.py spec for firmware/target/arm/pp/system-pp502x.c.
#
# SLICES: the cache section of the file, verbatim (the line markers must
# each match once): the status-word defines, and commit_dcache() through
# commit_discard_dcache() (cache_invalidate_special(), the COP's
# cop_commit_discard() of 0ebecb0db6, its stats, commit_discard_idcache()).
# The rest of the file (interrupt dispatch, clocks, init) needs the whole
# PP register map and is not under test.
#
# INSTRUMENT: Mach-O has no symbol aliases, so the alias declaration gets
# a one-line definition after it that calls the same function (the
# declaration itself is commented out by REPLACE below).
#
# MUTANTS: one fence deleted, one mask or window changed each; run.sh
# requires every one of them to turn the test red.

SLICES = [
    (r"^/\* Bit 0 - 20: Cached Address \*/$",
     r"^/\*Initial memory address used to prime cache"),
    (r"^void ICODE_ATTR commit_dcache\(void\)$",
     r"^static void init_cache\(void\)$"),
]

INSTRUMENT = [
    ('void commit_discard_dcache(void) __attribute__((alias("commit_discard_idcache")));',
     "void commit_discard_dcache(void) { commit_discard_idcache(); }"),
]

_ALIAS = ('void commit_discard_dcache(void) __attribute__((alias('
          '"commit_discard_idcache")));')
REPLACE = [(_ALIAS, "/* (alias: see the next line) */")]

_BULK = "    commit_dcache();                /* FIQ open: the bulk */\n"
_MASKED = "    commit_dcache();                /* what was dirtied since */\n"
_OPEN = ("    open = (istat & FIQ_STATUS) ? (istat | IRQ_FIQ_STATUS)\n"
         "                                : ((istat | IRQ_STATUS) & ~FIQ_STATUS);\n")
_LOOP_END = ("    {\n        register uint32_t t = USEC_TIMER - t0;\n\n"
             "        if (t > tmax)\n            tmax = t;\n    }\n"
             "    restore_interrupt(istat);\n    cds_n++;\n")

MUTANTS = {
    "cop_no_masked_writeback": (
        "cop_commit_discard(): no write-back with the FIQ masked before "
        "the status rewrite",
        [(_MASKED, "")]),
    "cop_rewrite_before_writeback": (
        "cop_commit_discard(): the masked write-back after the rewrite",
        [(_MASKED, ""),
         (_LOOP_END, _LOOP_END.replace("    restore_interrupt(istat);\n",
                                       "    commit_dcache();\n"
                                       "    restore_interrupt(istat);\n"))]),
    "cop_no_bulk_writeback": (
        "cop_commit_discard(): no bulk write-back with the FIQ open "
        "(the whole write-back masked again)",
        [(_BULK, "")]),
    "cop_block_512": (
        "cop_commit_discard(): one block, the FIQ never opens",
        [("#define CDS_BLOCK_LINES 64", "#define CDS_BLOCK_LINES 512")]),
    "cop_block_32": (
        "cop_commit_discard(): blocks of 32 lines",
        [("#define CDS_BLOCK_LINES 64", "#define CDS_BLOCK_LINES 32")]),
    "cop_window_irq_open": (
        "cop_commit_discard(): the window restores the entry state "
        "(the IRQ opens too)",
        [("            restore_interrupt(open);    /* a pending FIQ runs here */",
          "            restore_interrupt(istat);")]),
    "cop_no_remask": (
        "cop_commit_discard(): the FIQ stays open after a window",
        [("            restore_interrupt(istat | IRQ_FIQ_STATUS);\n", "")]),
    "cop_open_ignores_entry": (
        "cop_commit_discard(): opens the FIQ though it was masked at the call",
        [(_OPEN, "    open = (istat | IRQ_STATUS) & ~FIQ_STATUS;\n")]),
    "cop_masks_fiq_only": (
        "cop_commit_discard(): the IRQ not masked",
        [("    istat = disable_interrupt_save(IRQ_FIQ_STATUS);\n"
          "    /* the FIQ may open",
          "    istat = disable_interrupt_save(FIQ_STATUS);\n"
          "    /* the FIQ may open")]),
    "cop_dispatch_removed": (
        "commit_discard_idcache(): the COP takes the upstream path "
        "(the FIQ masked throughout)",
        [("        if (CURRENT_CORE == COP)\n        {\n"
          "            cop_commit_discard();\n            return;\n        }\n",
          "")]),
    "cpu_masks_irq_only": (
        "commit_discard_idcache(), CPU: the FIQ open during the rewrite",
        [("        register int istat = disable_interrupt_save(IRQ_FIQ_STATUS);",
          "        register int istat = disable_interrupt_save(IRQ_STATUS);")]),
    "cpu_no_writeback": (
        "commit_discard_idcache(), CPU: no write-back before the rewrite",
        [("        commit_dcache();\n        cache_invalidate_special();\n",
          "        cache_invalidate_special();\n")]),
}
