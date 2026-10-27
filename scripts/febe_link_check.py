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

"""The UI boundary's link check (P19 S1, warn mode; design 2.3, 7.1 S1/S4).

usage: scripts/febe_link_check.py [--strict] [--top N] [--nm NM] BUILD_DIR

Every apps/ipodui object is front end (FE) unless listed in BE_OBJECTS
(back-end services and the boundary itself).  An FE object that references
a back-end symbol directly (nm -u: playback, the playlist, the database,
file I/O, storage, buflib, power, the tick list and the event table:
design section 4) instead of crossing through fe_be.h is counted.  The UI
can move to the COP only when the count is 0 (fe_be.c's fe_cop_refusal()).

Warn mode (default) prints the families, the worst objects and the total,
and exits 0.  --strict (S4) exits 1 on any reference.

Either mode exits 1 when a name in febe_rpc.list (wrapped or direct:) is a
data object in the linked rockbox.elf: --wrap redirects every reference,
so a wrapped object reads the stub's code (p19-bssfix: fat_slot_fault's
"counters" were ARM opcodes).  A back-end datum crosses by a getter.
"""
import os
import re, subprocess, sys

# back-end families (design section 4); a prefix ends in '*'
BE_FAMILIES = {
    'playback':  ['audio_*', 'playlist_*', 'pcm_is_playing', 'pcmbuf_*'],
    'database':  ['tagcache_*', 'tagtree_*'],
    'file I/O':  ['open', 'read', 'write', 'close', 'rename', 'remove',
                  'creat', 'lseek', 'ftruncate', 'fsync', 'filesize',
                  'file_exists', 'dir_exists', 'opendir', 'readdir',
                  'closedir', 'mkdir', 'rmdir', 'open_utf8', 'fdprintf',
                  'read_line', 'copy_file*', 'rbfs_*', 'fat_*',
                  'dircache_*'],
    'storage':   ['storage_*', 'ata_*', 'disk_*'],
    'buflib':    ['core_alloc*', 'core_free', 'core_pin', 'core_unpin',
                  'core_get_data*', 'core_shrink', 'core_available',
                  'buflib_*'],
    'settings':  ['settings_save', 'settings_apply*', 'settings_load*'],
    'power':     ['sys_poweroff', 'sys_reboot', 'power_off', 'system_reboot',
                  'rolo_load', 'power_sleep', 'lcd_sleep', 'lcd_awake',
                  'backlight_hw_off'],
    # ucop-fix (2026-10-26): the mixer and the PCM driver are CPU-only code
    # (their lock masks the CPU's FIQ from one core, their control words
    # are cached): a sound from the COP is posted (click.h).  ucop-fix2: the
    # DSP's and sound.c's settings too (the codec reads them on the CPU;
    # sound.c's prescaler had writers on both cores): each crosses by
    # remote call, the EQ's set in one call (audio_out.c)
    'audio out': ['mixer_*', 'pcm_play_*', 'pcm_set_*', 'beep_play',
                  'dsp_*', 'sound_set*', 'sound_settings_apply'],
    'kernel lists': ['add_event', 'add_event_ex', 'remove_event',
                     'remove_event_ex', 'tick_add_task',
                     'tick_remove_task', 'timeout_register',
                     'timeout_cancel'],
}

# apps/ipodui objects that are back end (services, the boundary)
BE_OBJECTS = {
    'fe_be.o', 'fe_be_core.o',
}
# Shell objects that run on the CPU only once the shell is on the COP:
# every function of theirs the front end calls crosses by remote call
# (febe_rpc.list), so their memory and statics never meet the COP's cache.
# artcache.o: the art thread's buffers (the Now Playing bundle, the decode
# block, the chapter tables) and its reads into the caller's buffer.
SHELL_BE_OBJECTS = {
    'artcache.o',
    # services whose callbacks and timeouts run on CPU threads: routed, so
    # their statics stay cached (not linked uncached, app-pp.lds)
    'bootbatt.o', 'batcal.o', 'sc_hw.o', 'maint.o', 'lib_shuffle.o',
    'ab_store.o', 'powerlog.o', 'devbench_vclk.o',
    # clockpol: playback events (the track's needs) write it on the CPU
    'clockpol.o',
    # click: the headphone click, played on the CPU only (be_svc's
    # BE_SOUND; system_sound_play() routes through ipodui_sound_route())
    'click.o',
    # ucop-fix2: the DSP's EQ set in one call on the CPU (the codec never
    # sees half a set), and the visualizer's tap open and close (the FIQ
    # writes the ring on the CPU: the hook, the clean and the free run
    # there; viz_tap_read() is built for either core, below)
    'audio_out.o', 'viz_tap.o',
}
# pure functions or uncached single-word reads, called directly
SHELL_DIRECT = {
    'artcache_key', 'artcache_key_for_id3', 'artcache_key_chapter',
    'artcache_key_ascii', 'artcache_dim', 'artcache_fit_rect',
    'artcache_size_fits', 'artcache_panel_seq', 'artcache_npcover_seq',
    'artcache_commit_seq',
    # the tap's reader side: the ring and the reader's words uncached
    # (viz_tap.c), its back-end reads cross by their own stubs
    'viz_tap_read', 'viz_tap_is_open', 'viz_tap_bytes',
}


def match(sym, pats):
    for p in pats:
        if p.endswith('*'):
            if sym.startswith(p[:-1]):
                return True
        elif sym == p:
            return True
    return False


def objects(build):
    root = os.path.join(build, 'apps', 'ipodui')
    objs = []
    for d, _, files in os.walk(root):
        for f in files:
            if f.endswith('.o') and f not in BE_OBJECTS \
                    and f not in SHELL_BE_OBJECTS:
                objs.append(os.path.join(d, f))
    return sorted(objs)


def undefined(o, nm):
    out = subprocess.run([nm, '-u', o], capture_output=True, text=True)
    return [l.split()[-1] for l in out.stdout.splitlines() if l.strip()]


def family(sym):
    for fam, pats in BE_FAMILIES.items():
        if match(sym, pats):
            return fam
    return None


def shell_be_defined(build, nm):
    """functions defined by SHELL_BE_OBJECTS (the shell's CPU-only half)"""
    out = set()
    root = os.path.join(build, 'apps', 'ipodui')
    for d, _, files in os.walk(root):
        for f in files:
            if f in SHELL_BE_OBJECTS:
                r = subprocess.run([nm, '--defined-only', os.path.join(d, f)],
                                   capture_output=True, text=True).stdout
                for l in r.splitlines():
                    p = l.split()
                    if len(p) == 3 and p[1] == 'T':
                        out.add(p[2])
    return out - SHELL_DIRECT


def data_objects(build, readelf=None):
    """names the linked firmware defines as data (STT_OBJECT); empty when
    BUILD_DIR has no rockbox.elf"""
    elf = os.path.join(build, 'rockbox.elf')
    if not os.path.exists(elf):
        return set()
    readelf = readelf or os.environ.get('READELF', 'arm-none-eabi-readelf')
    out = subprocess.run([readelf, '-sW', elf], capture_output=True,
                         text=True).stdout
    objs = set()
    for l in out.splitlines():
        p = l.split()
        if len(p) >= 8 and p[0].endswith(':') and p[3] == 'OBJECT':
            objs.add(p[7][len('__real_'):] if p[7].startswith('__real_')
                     else p[7])
    return objs


def wrapped_data(build, names):
    """the listed names that are data objects in BUILD_DIR's firmware"""
    return sorted(set(names) & data_objects(build))


def referenced(build, nm=None):
    """every back-end symbol the shell's objects reference (febe_rpc_gen):
    the back-end families, and the functions of the shell's CPU-only
    objects"""
    nm = nm or os.environ.get('NM', 'arm-none-eabi-nm')
    shell_be = shell_be_defined(build, nm)
    data = data_objects(build)
    return {s for o in objects(build) for s in undefined(o, nm)
            if (family(s) or s in shell_be) and s not in data}


def rpc_list():
    """(wrapped, direct): the remote-call list and the reviewed direct
    calls (apps/ipodui/febe_rpc.list)"""
    p = os.path.join(os.path.dirname(os.path.dirname(
        os.path.abspath(__file__))), 'apps/ipodui/febe_rpc.list')
    wrapped, direct = set(), set()
    if os.path.exists(p):
        for l in open(p):
            l = l.split('#', 1)[0].strip()
            if l.startswith('direct:'):
                direct.add(l.split(':', 1)[1].strip())
            elif l:
                wrapped.add(l)
    return wrapped, direct


def main(argv):
    strict = '--strict' in argv
    top = 12
    nm = os.environ.get('NM', 'arm-none-eabi-nm')
    args = [a for a in argv if a != '--strict']
    if '--top' in args:
        i = args.index('--top'); top = int(args[i + 1]); del args[i:i + 2]
    if '--nm' in args:
        i = args.index('--nm'); nm = args[i + 1]; del args[i:i + 2]
    if len(args) != 1:
        print(__doc__); return 2
    objs = objects(args[0])
    if not objs:
        print('febe link: no apps/ipodui objects under %s' % args[0])
        return 2
    # the remote-call stubs count only where the build links them
    wrapped, direct = rpc_list()
    bad_data = wrapped_data(args[0], wrapped | direct)
    for s in bad_data:
        print('febe link: FAIL %s is a data object in febe_rpc.list: '
              '--wrap would point its readers at the stub\'s code; '
              'cross by a getter function' % s)
    # a kept stub whose real function is absent jumps to address 0: the
    # weak __real_X resolves to 0 (fdprintf, 207956fb84's hang)
    bad_zero = []
    elf = os.path.join(args[0], 'rockbox.elf')
    if os.path.exists(elf):
        defs = set(); wraps = set()
        for l in subprocess.run([nm, elf], capture_output=True,
                                text=True).stdout.splitlines():
            p = l.split()
            if len(p) == 3 and p[1] in 'TtWw':
                (wraps if p[2].startswith('__wrap_') else defs).add(p[2])
        for w in sorted(wraps):
            if w[7:] not in defs:
                bad_zero.append(w[7:])
                print('febe link: FAIL __wrap_%s is linked but %s is not: '
                      'its stub jumps to address 0' % (w[7:], w[7:]))
    if not os.path.exists(os.path.join(args[0], 'apps', 'ipodui',
                                       'febe_rpc_stubs.o')):
        wrapped = set()
    fam_n = {k: 0 for k in BE_FAMILIES}
    fam_syms = {k: set() for k in BE_FAMILIES}
    per_obj = []
    total = crossing = allowed = 0
    shell_be = shell_be_defined(args[0], nm)
    fam_n['shell back end'] = 0
    fam_syms['shell back end'] = set()
    for o in objs:
        n = 0
        for s in undefined(o, nm):
            fam = family(s) or ('shell back end' if s in shell_be else None)
            if not fam:
                continue
            if s in wrapped:
                crossing += 1
                continue
            if s in direct:
                allowed += 1
                continue
            fam_n[fam] += 1; fam_syms[fam].add(s); n += 1
        if n:
            per_obj.append((n, os.path.relpath(o, args[0])))
        total += n
    print('febe link: %d direct back-end references from %d front-end '
          'objects; %d cross by remote call, %d reviewed direct (%s)'
          % (total, len(per_obj), crossing, allowed,
             'strict' if strict else 'warn'))
    for fam in fam_n:
        if fam_n[fam]:
            print('  %-13s %4d  (%d symbols)' % (fam, fam_n[fam],
                                                len(fam_syms[fam])))
    for n, o in sorted(per_obj, reverse=True)[:top]:
        print('  %4d %s' % (n, o))
    return 1 if (bad_data or bad_zero or (strict and total)) else 0


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))
