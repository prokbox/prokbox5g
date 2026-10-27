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

"""Writes the long, repetitive Phase 6 scripts (many navigation blocks):

    python3 tests/fixtures/ab/gen_p6_scripts.py

  tests/ui/ipodui-p6-powercut.script        books.dat recovery (books.tmp,
                                            one header lost, both lost)
  tests/ui/ipodui-p6-powercut-torn.script   torn sector, truncation, junk
  tests/ui/ipodui-p6-store-evict.script     past 2,048 books
  tests/ui/ipodui-p6-pak-build.script       books.pak for every book (ready)
  tests/ui/ipodui-p6-pak-build-fresh.script ... on a fresh database
  tests/ui/ipodui-p6-pak-build-dirty.script ... after a dirty database
  tests/ui/ipodui-p6-pool-cap.script        no pool growth while playing

Edit this file, not the generated scripts. Rows in the Audiobooks list with
the fixtures a script copies: the library's A Field Guide to Tones is row 0,
then the "AB ..." books alphabetically (a book's row = 1 + its rank among
the AB books copied), then Lighthouse, Mini, Winter.
"""
import os

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), '..', '..', '..'))
FIX = '../../build/ab-fixtures/'
BASE_BOOKS = 4      # library: A Field Guide, Lighthouse, Mini Audiobook, Winter Stories

TOBOOKS = """key SELECT
wait 800
wheel +7 300
wait 400
key SELECT
wait 900
"""
RESET = "hold MENU 1000\nwait 1200\n"


def open_row(row, first=False):
    """main menu -> Audiobooks -> the page of <row>"""
    s = '' if first else RESET
    s += TOBOOKS
    if row:
        s += 'wheel +%d 300\nwait 400\n' % row
    s += 'key SELECT          # the book page\nwait 1000\n'
    return s


def play_entry(secs_ms=3500, down=0):
    """from a page without Resume: entry <down>, play, pause (record)"""
    s = ''
    if down:
        s += 'wheel +%d 300\nwait 300\n' % down
    s += 'key SELECT\nwait %d\nkey PLAY            # pause: the record is written now\nwait 800\n' % secs_ms
    return s


def boot(books):
    s = 'wait 4000\nwait_db 45000\nmkdir /Audiobooks/AB\n'
    for b in books:
        s += 'copy %s%s /Audiobooks/AB/%s\n' % (FIX, b, b)
    s += 'update_db_wait_change 60000\nart_wait 60000\n'
    return s


def unbook(books):
    return ''.join('remove /Audiobooks/AB/%s\n' % b for b in books)


def write(name, text):
    with open(os.path.join(ROOT, 'tests', 'ui', 'ipodui-p6-%s.script' % name),
              'w') as f:
        f.write(text)


BOOKS3 = ['nochap.m4b', 'pref.m4b', 'short.m4b']     # rows 1, 2, 3
HEAD = '# needs: library\n# needs: ab\n# timeout: 150\n# trace: ab\n'

# --------------------------------------------------------------- powercut
t = """# Phase 6 (checklist item 1, D10): books.dat survives a power cut. Three
# books get a spot; then, AFTER the clean shutdown's own flush (the disk the
# power cut leaves), the file is damaged and the next boot must recover:
#   1. books.dat removed, books.tmp left (an insert died between its remove
#      and its rename): the spots come back from books.tmp
#   2. the first header lost: the second header copy is used, no spot lost
#   3. both headers lost: an empty store, logged; no Resume anywhere; the
#      next save rebuilds the file
# Damage is injected by sim_cmd_ab.c ("ab_power_cut exit <kind>", README).
#   SIM_BUILD=build/sim-ui scripts/shots.sh tests/ui/ipodui-p6-powercut.script
""" + HEAD + """# log: ab store w=now
# golden: skip page_tmp page_hdr0 page_empty
# DB state: ready. Needs B (ab_store.c recovery). Rows: 1 AB No Chapters,
# 2 AB Preface Book, 3 AB Short Book.

"""
t += boot(BOOKS3)
for i, r in enumerate((1, 2, 3)):
    t += open_row(r, first=(i == 0)) + play_entry()
t += "expect_ab_store 3 0 3000\nab_power_cut exit store_tmp\nreboot\n\n"
# boot 2: books.tmp only
t += "wait 4000\nwait_db 45000\n" + open_row(2, first=True)
t += "shot page_tmp       # Resume is first: recovered from books.tmp\n"
t += "key SELECT          # Resume\nwait 2500\nkey PLAY\nwait 800\n"
t += "expect_ab_file store present 3000   # the next write recreated books.dat\n"
t += "expect_ab_store 3 0 3000\nab_power_cut exit store_hdr0\nreboot\n\n"
# boot 3: first header lost
t += "wait 4000\nwait_db 45000\n" + open_row(3, first=True)
t += "shot page_hdr0       # Resume still shown: the second header copy\n"
t += "key SELECT\nwait 2500\nkey PLAY\nwait 800\nexpect_ab_store 3 0 3000\n"
t += "ab_power_cut exit store_hdr_both\nreboot\n\n"
# boot 4: both lost
t += "wait 4000\nwait_db 45000\n" + open_row(2, first=True)
t += "shot page_empty      # no Resume: an empty store, logged, nothing shown\n"
t += play_entry(2500)
t += "expect_ab_store 1 0 3000       # a new store holding only this book\n"
t += unbook(BOOKS3) + "quit\n"
write('powercut', t)

# -------------------------------------------------------- powercut-torn
t = """# Phase 6 (Rev 1 item 7): a sector write is not atomic. After three spots
# are saved, books.dat is damaged after the last flush:
#   1. half of record sector 1 zeroed (a torn write): at most that sector's
#      records are lost (they read as unplayed, logged), the boot goes on
#      and the next save works
#   2. the file truncated to half: same
#   3. every byte garbage: an empty store, rebuilt by the next save
#   SIM_BUILD=build/sim-ui scripts/shots.sh tests/ui/ipodui-p6-powercut-torn.script
""" + HEAD + """# golden: skip page_torn page_trunc page_junk
# DB state: ready. Needs B. Rows: 1 AB No Chapters, 2 AB Preface, 3 AB Short.

"""
t += boot(BOOKS3)
for i, r in enumerate((1, 2, 3)):
    t += open_row(r, first=(i == 0)) + play_entry()
t += "expect_ab_store 3 0 3000\nab_power_cut exit store_tear 1\nreboot\n\n"
t += "wait 4000\nwait_db 45000\n" + open_row(1, first=True)
t += "shot page_torn       # no crash; Resume only if that record survived\n"
t += "expect_ab_store -1 -1 1000\n"
t += "key SELECT\nwait 2500\nkey PLAY\nwait 800\n"
t += "expect_ab_store ge1 -1 3000   # the next save works on a torn file\n"
t += "ab_power_cut exit store_trunc\nreboot\n\n"
t += "wait 4000\nwait_db 45000\n" + open_row(1, first=True)
t += "shot page_trunc\nexpect_ab_store -1 -1 1000\n"
t += "key SELECT\nwait 2500\nkey PLAY\nwait 800\nexpect_ab_store ge1 -1 3000\n"
t += "ab_power_cut exit store_junk\nreboot\n\n"
t += "wait 4000\nwait_db 45000\n" + open_row(1, first=True)
t += "shot page_junk        # empty store: no Resume\n"
t += play_entry(2500)
t += "expect_ab_store 1 0 3000\n" + unbook(BOOKS3) + "quit\n"
write('powercut-torn', t)

# ---------------------------------------------------------- store-evict
t = """# Phase 6 (Rev 1 item 11, D10): past 2,048 books the store evicts the oldest
# FINISHED record, then the oldest STARTED. A full books.dat (2,048 records,
# the 2 oldest FINISHED; tests/fixtures/ab/mkstore.c, sealed with the real
# ab_store_core.c) is installed after the first boot's shutdown; each of three
# new books then evicts one record: 00a00000 and 00a00003 (FINISHED), then
# 00a00006 (the oldest STARTED). The valid-record count stays 2,048.
#   SIM_BUILD=build/sim-ui scripts/shots.sh tests/ui/ipodui-p6-store-evict.script
""" + HEAD + """# log: ab store evict book=00a00000
# log: ab store evict book=00a00003
# log: ab store evict book=00a00006
# golden: skip
# DB state: ready. Needs B. Rows: 1 AB No Chapters, 2 AB Preface, 3 AB Short.

wait 4000
wait_db 45000
ab_copy_at_exit """ + FIX + """store_full.dat /.rockbox/artcache/books.dat
reboot

"""
t += boot(BOOKS3)
t += "expect_ab_store 2046 2 3000      # the installed store was loaded\n"
t += open_row(1, first=True) + play_entry()
t += "expect_ab_store 2047 1 3000      # evicted a00000 (FINISHED)\n"
t += open_row(2) + play_entry()
t += "expect_ab_store 2048 0 3000      # evicted a00003 (FINISHED)\n"
t += open_row(3) + play_entry()
t += "expect_ab_store 2048 0 3000      # evicted a00006 (the oldest STARTED)\n"
t += unbook(BOOKS3) + "quit\n"
write('store-evict', t)

# -------------------------------------------------------------- pak-build
PAKBOOKS = ['pref.m4b', 'nochap.m4b', 'short.m4b']
MULTI = ['01.mp3', '02.mp3', '03.mp3', '04.mp3']


def pak_script(state, hdr_extra, prefix, want_total):
    t = """# Phase 6 (Rev 1 item 4): books.pak / books.idx hold a blob for EVERY book
# (coverless, multi-file, single-file with and without chapters), built in
# the stopped pass only. A stale book (newer mtime) is rebuilt; a lost or
# torn index owes every book again. While a book plays the pak and its index
# do not change (the generation word stays put).
#   SIM_BUILD=build/sim-ui scripts/shots.sh tests/ui/ipodui-p6-pak-build%s.script
# needs: library
# needs: ab
# timeout: 240
%s# trace: ab
# log: ab pak miss
# log: ab pak hit
# golden: skip
# DB state: %s. Needs B (artcache.c books pass). The fixtures have no covers.
# Books: the library's 4 + 3 single-file fixtures + the 4-file AB Multi.

""" % (prefix, hdr_extra, state)
    t += 'wait 4000\nwait_db 45000\nmkdir /Audiobooks/AB\nmkdir /Audiobooks/ABMulti\n'
    for b in PAKBOOKS:
        t += 'copy %s%s /Audiobooks/AB/%s\n' % (FIX, b, b)
    for b in MULTI:
        t += 'copy %smulti/%s /Audiobooks/ABMulti/%s\n' % (FIX, b, b)
    t += 'update_db_wait_change 60000\nart_wait 60000\n'
    t += 'expect_ab_pak %d 60000          # every book has a blob, stopped\n' % want_total
    if state == 'dirty':
        return t, None
    t += 'expect_ab_file pak present 1000\nexpect_ab_file idx present 1000\n'
    t += 'ab_pak_mark\n'
    # playing: a stale book must not be rebuilt
    t += 'play_files /Audiobooks/AB/nochap.m4b\nwait 1500\n'
    t += 'ab_touch /Audiobooks/AB/pref.m4b      # stale: mtime moved\n'
    t += 'art_request\nwait 6000\n'
    t += 'expect_ab_pak_same 3000          # playing: nothing written\n'
    t += 'audio_stop\nwait 500\nart_wait 60000\n'
    t += 'expect_ab_pak_changed 30000      # stopped: the stale book rebuilt\n'
    t += 'expect_ab_pak %d 5000\n' % want_total
    # lost pak and idx: every book owed again
    t += 'ab_power_cut now pak_drop\nab_power_cut now idx_drop\n'
    t += 'art_request\nart_wait 60000\nexpect_ab_pak %d 60000\n' % want_total
    t += 'expect_ab_file pak present 3000\n'
    # a torn index (generation zeroed): rejected, rebuilt
    t += 'ab_power_cut now idx_flip\nart_request\nart_wait 60000\n'
    t += 'expect_ab_pak %d 60000\n' % want_total
    t += 'ab_power_cut now pak_tmp_left      # a compaction died mid-copy\n'
    t += 'ab_touch /Audiobooks/AB/short.m4b\nart_request\nart_wait 60000\n'
    t += 'expect_ab_pak %d 60000\n' % want_total
    return t, True


tail = ''.join('remove /Audiobooks/AB/%s\n' % b for b in PAKBOOKS) \
    + ''.join('remove /Audiobooks/ABMulti/%s\n' % b for b in MULTI) + 'quit\n'
total = BASE_BOOKS + 4
t, _ = pak_script('ready', '', '', total)
write('pak-build', t + tail)
t, _ = pak_script('fresh (rebuilt from nothing at boot)', '# fresh-db\n',
                  '-fresh', total)
write('pak-build-fresh', t + tail)
t, _ = pak_script('dirty', '', '-dirty', total)
t = t.replace('wait 4000\nwait_db 45000\nmkdir',
              'wait 4000\nwait_db 45000\ndirty_db            # a power cut mid-commit\nreboot\n\nwait 4000\nwait_db 60000\nmkdir', 1)
write('pak-build-dirty', t + tail.replace('quit', 'quit'))

# -------------------------------------------------------------- pool-cap
t = """# Phase 6 (Rev 1 item 2): no pool growth while audio plays. A book plays;
# opening ANOTHER book's page (the 1,100-chapter "AB Many Chapters") must not
# grow the library view pool: core_available() does not shrink
# (expect_ab_mem_same), and the chapter view builds with a smaller entry cap
# that fits the free pool ("ab view cap=N", N < 1,024 while playing). Stopped,
# the same page gets the full 1,024. No core_alloc is traced while playing
# (check_trace.py's aballoc rule).
#   SIM_BUILD=build/sim-ui scripts/shots.sh tests/ui/ipodui-p6-pool-cap.script
""" + HEAD + """# log: ab view cap=
# logrange: 16 1024 ab view cap=(\\d+)
# golden: skip page_many_playing page_many_stopped
# DB state: ready. Needs B (pool_reserve refuses to grow while playing) and
# U. Rows: 0 A Field Guide, 1 AB Many Chapters, 2 AB No Chapters.

"""
t += boot(['nochap.m4b', 'many.m4b'])
t += open_row(2, first=True)
t += "key SELECT          # play AB No Chapters from 0\nwait 2500\nab_mem_mark\n"
t += "ab_mark many_playing_begin\n"
t += open_row(1).replace('# the book page', '# AB Many Chapters, while playing')
t += "shot page_many_playing     # a shorter list than 1,100 rows is fine\n"
t += "expect_ab_mem_same\nab_mark many_playing_end\nexpect_playing 1500\n"
t += "audio_stop\nwait 800\n"
t += open_row(1)
t += "shot page_many_stopped\n"
t += "remove /Audiobooks/AB/nochap.m4b\nremove /Audiobooks/AB/many.m4b\nquit\n"
write('pool-cap', t)
