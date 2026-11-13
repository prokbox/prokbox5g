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

"""What the COP touches during a decode run (#197; docs/design/cop-shared-state.md).

usage: scripts/cop_shared_check.py [--refs] [--list] [--fiq] BUILD_DIR

--fiq (rc.6, layout A): walk from the PCM's FIQ on the COP instead, and
fail (exit 1) on any cached static it writes or any kernel or blocking
call it makes (FIQ_FORBIDDEN).

With the decode on the COP (0.8.0), any cached static the COP writes, or
reads while the CPU writes it, is a coherence question (quirks.md 8, 52, 54,
88, 90): the caches do not snoop and write back whole 16-byte lines.  This
walks the call graph of BUILD_DIR/rockbox.elf from what runs on the COP
during a run (the worker thread, the job, the codec's run and every function
the codec can reach through the codec API it is handed on the COP) with the
walker of scripts/febe_reach_check.py: direct branches, function addresses
in literal pools and in the tables a reached function names; a function
handed to cop_job_call() runs on the CPU, so it ends a path.  It prints
every cached, writable static (logical SDRAM below 0x1000_0000; the
uncached alias and IRAM are exempt) that reached code may write, with the
owning object and one path; --refs adds the cached statics reached code
only reads.  Each one is a row to review in cop-shared-state.md.

What it cannot see (reviewed by hand in the document): memory reached
through a pointer that is not a static's address (the codec slot, the ring,
heap blocks, the id3 handed in, stacks), code inside the .codec images
(their statics live in the codec slot), and calls through a pointer loaded
from a non-static place.
"""
import os
import re
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import febe_reach_check as fr      # noqa: E402

# What the COP runs during a decode run (codec_cop.c, cop_job.c, the codec
# API it hands the codec: ci_cop = ci with the playback callbacks replaced)
COP_ROOTS = [
    # cop_job.c: the worker; run_slot() calls the job through a pointer
    'worker_thread', 'run_job', 'codec_run_proc',
    # codec_cop.c: the callbacks the codec calls on the COP
    't_get_buffer', 't_pcmbuf_insert', 't_set_elapsed', 't_read_filebuf',
    't_request_buffer', 't_advance_buffer', 't_seek_buffer',
    't_seek_complete', 't_set_offset', 't_configure', 't_get_command',
    't_loop_track', 't_strip_filesize', 't_panicf', 't_logf',
    # codecs.c ci: the entries the COP calls as they are
    'sleep', 'yield', 'create_thread', 'thread_thaw', 'thread_wait',
    'semaphore_init', 'semaphore_wait', 'semaphore_release',
    'commit_dcache', 'commit_discard_dcache', 'commit_discard_idcache',
    'qsort', '__div0', 'debugf',
    # the COP's own kernel: its scheduler and idle
    'switch_thread', 'core_sleep', 'cop_main',
]


# rc.6 (layout A, --fiq): what runs in the PCM's FIQ on the COP.  The
# mixer's and the channels' callbacks are reached through pointers kept in
# non-static places (the channel table, pcm.c's callback words), so they
# are named here: every get_more and status callback a channel can carry
FIQ_ROOTS = [
    'fiq_by_core', 'fiq_playback', 'fiq_spurious',
    'mixer_buffer_callback', 'mixer_pcm_callback',
    'pcmbuf_pcm_callback', 'beep_get_more', 'voice_pcm_callback',
    'al_more', 'sink_dma_stop', 'ac_get_more', 'ac_status',
    # mixer_set_tap() targets (pcm_mixer.c calls mixer_tap from the mix)
    'vt_tap', 'ap_tap',
]

# kernel and blocking calls a FIQ must never make: the COP's thread it
# interrupted may hold the same corelock (a self-deadlock), and the
# CPU-only lists are not the COP's to touch (xcore.h)
FIQ_FORBIDDEN = re.compile(
    r'^(corelock_\w+|mutex_\w+|queue_\w+|semaphore_\w+|wakeup_\w+|'
    r'thread_\w+|switch_thread|sleep|yield|tick_\w+_task|'
    r'ui_log\w*|cpu_boost\w*|core_\w*alloc\w*|buflib_\w+|'
    r'iap_\w+)$')

# reviewed apart: what runs only once the COP has crashed
CRASH = ('UIE', 'panicf', 'panic_record_save')


def main(argv):
    refs_too = '--refs' in argv
    listing = '--list' in argv
    fiq = '--fiq' in argv
    args = [a for a in argv if a not in ('--refs', '--list', '--fiq')]
    if len(args) != 1 or not os.path.exists(
            os.path.join(args[0], 'rockbox.map')):
        print(__doc__)
        return 2
    build = args[0]
    elf = fr.Elf(os.path.join(build, 'rockbox.elf'))
    elf.path = os.path.join(build, 'rockbox.elf')
    owner_of = fr.owners(os.path.join(build, 'rockbox.map'))
    objdump = os.environ.get('OBJDUMP', 'arm-none-eabi-objdump')
    # the playback handlers are handed to cop_job_call(): CPU side
    fr.HANDOFF = set(fr.HANDOFF) | {'cop_job_call'}
    # the crash path (a divide by zero or an undefined instruction on the
    # COP): the system is going down; reviewed apart in the document
    fr.CPU_SIDE = dict(fr.CPU_SIDE, **{n: 'crash path' for n in CRASH})
    if fiq:
        # reviewed: reads the sink's frequency field (pcm.c), calls no op
        fr.TABLE_LOOKUP['pcm_get_frequency'] = 'reads sink->caps.samprs'
        # reviewed: calls sinks[cur_sink]->ops.stop alone, the builtin
        # sink's sink_dma_stop (a FIQ runs only for the builtin sink's
        # DMA), which FIQ_ROOTS names
        fr.TABLE_LOOKUP['pcm_play_stop_int'] = 'ops.stop: sink_dma_stop'
    edges, writes = fr.graph(elf, objdump)
    byname = {}
    for a, n in elf.funcs.items():
        byname.setdefault(n, a)
    want = FIQ_ROOTS if fiq else COP_ROOTS
    missing = [r for r in want if r not in byname]
    roots = [byname[r] for r in want if r in byname]
    if fiq and 'fiq_playback' not in byname:
        print('cop shared --fiq: no fiq_playback in %s' % build)
        return 2
    if not roots:
        print('cop shared: no COP roots in %s' % build)
        return 2
    prev = fr.walk(elf, edges, roots)
    # reads: the statics a reached function loads the address of
    _, _ = None, None
    rows = []
    seen = set()
    for f in prev:
        for a in writes.get(f, ()):
            name, size, writable = elf.objs[a]
            if not writable or a >= 0x10000000 or a in seen:
                continue
            seen.add(a)
            rows.append(('W', fr.objname(owner_of(a)), name, a, size,
                         fr.path(elf, prev, f)))
    print('cop shared: %d roots (%d absent in this build: %s), %d functions '
          'reached, %d cached writable statics written'
          % (len(roots), len(missing), ' '.join(missing) or '-', len(prev),
             len(rows)))
    for r in sorted(rows, key=lambda r: (r[1], r[2])):
        print('  %s %-22s %-32s 0x%08x %6d  %s'
              % (r[0], r[1], r[2], r[3], r[4],
                 r[5] if listing else r[5].split(' <- ')[0]))
    if fiq:
        bad = []
        for f in prev:
            for m in edges.get(f, ()):
                nm = fr.base(elf.funcs[m])
                if FIQ_FORBIDDEN.match(nm):
                    bad.append((nm, fr.path(elf, prev, f)))
        print('cop shared --fiq: %d forbidden calls from the FIQ' % len(bad))
        for nm, p in sorted(set(bad)):
            print('  X %-24s %s' % (nm, p if listing else p.split(' <- ')[0]))
        if rows or bad:
            print('cop shared --fiq: FAIL (%d cached statics written, %d '
                  'forbidden calls)' % (len(rows), len(bad)))
            return 1
        print('cop shared --fiq: ok (%d functions, no cached static '
              'written, no forbidden call)' % len(prev))
    if refs_too:
        print('reached functions:')
        for f in sorted(prev, key=lambda f: elf.funcs[f]):
            print('  F %-40s %s' % (elf.funcs[f], fr.objname(owner_of(f))))
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))
