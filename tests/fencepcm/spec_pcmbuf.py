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

# tests/fencepcm: srcpatch.py spec for apps/pcmbuf.c.
#
# INSTRUMENT adds two #defines right after the declarations of the lease
# and chunk_widx, so every later access to either goes through a function
# of the test first (a scheduling point: the other core, or the PCM FIQ,
# may run there). No statement of pcmbuf.c changes.
#
# MUTANTS: one fence deleted or one publish reordered each; run.sh
# requires every one of them to turn the test red.
#
# Not a mutant here, because it is equivalent (no run can tell): moving
# lease_commit_chunks()' wrap copy after its write-back. The copy only
# fills the ring's start past the last chunk this call commits (a window
# ends at most PCMBUF_GUARD_SIZE past the ring, under a chunk), so those
# bytes are still uncommitted; the next commit's write-back, or the
# release's, sends them before chunk_widx covers them. The order in the
# source is the safe one should a call ever commit with COMMIT_ALL_DATA.

INSTRUMENT = [
    ("static struct pcmbuf_lease lease XCORE_BSS;",
     "#define lease (*fence_lease_at(&lease))"),
    ("static size_t chunk_widx XCORE_BSS;",
     "#define chunk_widx (*fence_widx_at(&chunk_widx))"),
]

_GRANT_WB = ("    commit_dcache();            /* any CPU write to the audio, "
             "to SDRAM */\n")
_GRANT_PUB = "    lease.owner = LEASE_COP;    /* last: the COP may write */\n"
_LC_COMMENT = ("    /* the audio and the wrap copy reach SDRAM before chunk_widx "
               "says so\n       (the FIQ reads them uncached) */\n")
_LC_WRAP = ("    if (end_index > pcmbuf_size)\n"
            "        memcpy(pcmbuf_buffer, pcmbuf_guardbuf, "
            "end_index - pcmbuf_size);\n")
_LC_LOOP_END = "    while (lease.waiting >= threshold);\n    lease.commits++;\n"
_REL_WB = ("    commit_dcache();            /* the partial chunk the CPU "
           "commits later */\n")
_REL_PUB = "    lease.owner = LEASE_BACK;\n"
_RC_DISCARD = ("        commit_discard_dcache();    /* no stale line of the "
               "audio here */\n")

MUTANTS = {
    "grant_no_writeback": (
        "pcmbuf_lease_grant(): the CPU's write-back before the COP takes "
        "the write side",
        [(_GRANT_WB, "")]),
    "grant_owner_first": (
        "pcmbuf_lease_grant(): owner = LEASE_COP published before the "
        "lease's fields",
        [(_GRANT_PUB, ""),
         (_GRANT_WB + "    lease.waiting = pcmbuf_bytes_waiting;\n",
          _GRANT_WB + "    lease.owner = LEASE_COP;\n"
          "    lease.waiting = pcmbuf_bytes_waiting;\n")]),
    "grant_no_count_move": (
        "pcmbuf_lease_grant(): the CPU's uncommitted count not handed over",
        [("    lease.waiting = pcmbuf_bytes_waiting;\n", "")]),
    "commit_no_writeback": (
        "lease_commit_chunks(): the COP's write-back before chunk_widx moves",
        [(_LC_COMMENT + "    commit_dcache();\n", _LC_COMMENT)]),
    "commit_publish_first": (
        "lease_commit_chunks(): chunk_widx published before the write-back",
        [(_LC_COMMENT + "    commit_dcache();\n", _LC_COMMENT),
         (_LC_LOOP_END, "    while (lease.waiting >= threshold);\n"
                        "    commit_dcache();\n    lease.commits++;\n")]),
    "release_no_writeback": (
        "pcmbuf_lease_release(): the COP's write-back of its partial chunk",
        [(_REL_WB, "")]),
    "release_owner_first": (
        "pcmbuf_lease_release(): LEASE_BACK published before the write-back",
        [(_REL_WB + _REL_PUB, _REL_PUB + _REL_WB)]),
    "reclaim_no_discard": (
        "pcmbuf_lease_reclaim(): the CPU's drop of its stale lines",
        [(_RC_DISCARD, "")]),
    "reclaim_no_wait": (
        "pcmbuf_lease_reclaim(): no wait for the COP to let go",
        [("        while (lease.owner == LEASE_COP)\n            sleep(1);\n",
          "")]),
    "reclaim_no_handback": (
        "pcmbuf_lease_reclaim(): the COP's uncommitted count not taken back",
        [("        pcmbuf_bytes_waiting = lease.waiting;\n", "")]),
    "window_ignores_revoke": (
        "pcmbuf_lease_window(): a window despite a pending revoke",
        [("    if (lease.owner != LEASE_COP || lease.revoke || "
          "!pcm_is_playing())",
          "    if (lease.owner != LEASE_COP || !pcm_is_playing())")]),
    "cpu_commit_no_writeback": (
        "commit_chunks() (rc.6, the CPU's own path): the write-back before "
        "chunk_widx moves",
        [("       chunk_widx publishes them */\n    commit_dcache();\n",
          "       chunk_widx publishes them */\n")]),
}
