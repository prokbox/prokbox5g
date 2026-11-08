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

"""The COP's reach (p19-copfileio): the checks that would have caught bench 4f.

1. scripts/febe_reach_check.py on a small ARM firmware linked here: a path
   from shell_main into the file layer (file.o) is found, one through a
   callback in a table too; a call through a __wrap_ stub, or a callback
   handed to tick_add_task, is not; a static the COP writes outside the
   front end's objects is reported.
2. scripts/xcore_line_check.py on a map shaped like the bench build's
   (disk_cache.o's cache_lru and bidi.o's bidi_buf on one 16-byte line):
   strict fails; with the file layer on lines of its own it passes.

Skips (1) when arm-none-eabi-gcc is not on PATH.
"""
import os
import shutil
import subprocess
import sys
import tempfile

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(
    os.path.abspath(__file__))))
sys.path.insert(0, os.path.join(ROOT, 'scripts'))
import febe_reach_check as rc  # noqa: E402

FILE_C = '''
int dc_lru;
int rbfs_open(const char *p) { dc_lru++; return p[0]; }
int rbfs_read(int fd) { dc_lru--; return fd; }
'''
WRAP_C = '''
extern int __real_rbfs_read(int fd);
int __wrap_rbfs_read(int fd) { return __real_rbfs_read(fd); }
'''
APP_C = '''
int rbfs_open(const char *p);
int rbfs_read(int fd);
void tick_add_task(void (*f)(void)) { (void)f; }
static void on_tick(void) { rbfs_open("t"); }       /* runs on the CPU */
static void cb(void) { rbfs_open("c"); }            /* runs on the COP */
struct item { void (*fn)(void); };
const struct item menu[] = { { cb } };
char other_buf[24];
void draw(char *b) { b[0] = 1; }
int helper(int v) { return rbfs_read(v); }          /* wrapped below */
#ifdef DIRECT
int direct(void) { return rbfs_open("d"); }
#endif
const struct item *shell_menu(void) { return menu; }
void shell_main(void)
{
    tick_add_task(on_tick);
    helper(1);
    draw(other_buf);
#ifdef DIRECT
    direct();
#endif
#ifdef TABLE
    shell_menu()->fn();
#endif
}
void _start(void) { shell_main(); }
'''


def build(tmp, direct, table):
    cc = 'arm-none-eabi-gcc'
    flags = ['-O1', '-marm', '-mcpu=arm7tdmi', '-ffunction-sections',
             '-fdata-sections', '-fno-inline', '-c']
    srcs = {'file.c': FILE_C, 'wrap.c': WRAP_C, 'app.c': APP_C}
    objs = []
    for n, src in srcs.items():
        p = os.path.join(tmp, n)
        open(p, 'w').write(src)
        o = os.path.join(tmp, n[:-2] + '.o')
        extra = (['-DDIRECT'] if direct else []) + \
            (['-DTABLE'] if table else [])
        subprocess.check_call([cc] + flags + extra + [p, '-o', o])
        objs.append(o)
    out = os.path.join(tmp, 'rockbox.elf')
    ld = ['-nostdlib', '-Wl,-Map=' + os.path.join(tmp, 'rockbox.map'),
          '-Wl,--wrap=rbfs_read', '-o', out]
    subprocess.check_call([cc] + objs + ld)


def reach_case(direct, table):
    tmp = tempfile.mkdtemp()
    try:
        build(tmp, direct, table)
        fs, line = rc.check(tmp)
        return [w for w, _ in fs], [w for w, _ in line]
    finally:
        shutil.rmtree(tmp)


def line_map(tmp, fs_isolated):
    """a map: disk_cache.o's cache_lru (4 B) then bidi.o's bidi_buf"""
    lru = 0x15d010
    buf = 0x15d020 if fs_isolated else 0x15d014
    m = ('Linker script and memory map\n\n'
         ' .bss.cache_entry\n'
         '                0x%08x      0x600 x/libfirmware.a(disk_cache.o)\n'
         ' .bss.cache_lru\n'
         '                0x%08x        0x4 x/libfirmware.a(disk_cache.o)\n'
         ' .bss.bidi_buf.0\n'
         '                0x%08x      0xba4 x/libfirmware.a(bidi.o)\n'
         % (lru - 0x600, lru, buf))
    p = os.path.join(tmp, 'rockbox.map')
    open(p, 'w').write(m)
    return p


# rc.6's pcmbuf_pcm_callback (fw-ship, 2026-11-08): gcc merged two
# counters' increments into one store, the first counter's address loaded
# with ldrne and the second load branched over, so a straight-line reading
# saw only the second. The walk must report both as written.
MERGED_STORE = """
00001000 <cb>:
    1000:\tcmp\tr1, #0
    1004:\tldrne\tr2, [pc, #16]\t@ 101c <cb+0x1c>
    1008:\tbne\t1010 <cb+0x10>
    100c:\tldr\tr2, [pc, #12]\t@ 1020 <cb+0x20>
    1010:\tldr\tr3, [r2]
    1014:\tadd\tr3, r3, #1
    1018:\tstr\tr3, [r2]
    101c:\t.word\t0x00002000
    1020:\t.word\t0x00002010
"""


class _MergedElf:
    def __init__(self, path):
        self.path = path
        self.funcs = {0x1000: 'cb'}
        self.objs = {0x2000: ('rate_gaps', 4, True),
                     0x2010: ('underruns', 4, True)}

    def word(self, a):
        return 0                    # the counters hold no addresses

    def obj_at(self, a):
        for o, (_, sz, _) in self.objs.items():
            if o <= a < o + sz:
                return o
        return None


def merged_store_case():
    """the writes graph() reports for MERGED_STORE, through a fake objdump
    that prints it"""
    tmp = tempfile.mkdtemp()
    try:
        lst = os.path.join(tmp, 'listing.txt')
        open(lst, 'w').write(MERGED_STORE)
        fake = os.path.join(tmp, 'objdump')
        open(fake, 'w').write('#!/bin/sh\ncat "%s"\n' % lst)
        os.chmod(fake, 0o755)
        _, writes = rc.graph(_MergedElf(os.path.join(tmp, 'x.elf')), fake)
        return set(writes.get(0x1000, ()))
    finally:
        shutil.rmtree(tmp)


def main():
    fails = 0
    got = merged_store_case()
    if got != {0x2000, 0x2010}:
        print('FAIL reach: a store through a register loaded under a '
              'condition (ldrne, then a branch over the other load) saw '
              'writes %s, want both 0x2000 and 0x2010'
              % sorted(hex(a) for a in got)); fails += 1
    if shutil.which('arm-none-eabi-gcc'):
        fs, line = reach_case(direct=True, table=True)
        names = ' '.join(fs)
        # the direct call and the menu's callback both enter file.o
        if 'rbfs_open' not in names:
            print('FAIL reach: direct/table path into file.o not found: %s'
                  % fs); fails += 1
        if 'rbfs_read' in names:
            print('FAIL reach: a wrapped call counted: %s' % fs); fails += 1
        if not any('other_buf' in w for w in line):
            print('FAIL reach: COP-written static not reported: %s' % line)
            fails += 1
        fs2, _ = reach_case(direct=False, table=True)
        if not any('rbfs_open' in w for w in fs2):
            print('FAIL reach: the table callback path not found: %s' % fs2)
            fails += 1
        fs3, _ = reach_case(direct=False, table=False)
        if fs3:
            print('FAIL reach: tick callback or wrapped call reported: %s'
                  % fs3); fails += 1
    else:
        print('     reach: skipped (no arm-none-eabi-gcc)')
    tmp = tempfile.mkdtemp()
    try:
        xl = os.path.join(ROOT, 'scripts', 'xcore_line_check.py')
        for iso, want in ((False, 1), (True, 0)):
            r = subprocess.run([sys.executable, xl, '--strict',
                                line_map(tmp, iso)], capture_output=True,
                               text=True)
            if r.returncode != want:
                print('FAIL line check (file layer %s): rc=%d\n%s'
                      % ('isolated' if iso else 'shared', r.returncode,
                         r.stdout)); fails += 1
    finally:
        shutil.rmtree(tmp)
    return 1 if fails else 0


if __name__ == '__main__':
    sys.exit(main())
