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

"""Compile the actual shutdown/cancellation/event path, not a behavior model."""
from pathlib import Path
import os
import subprocess
import tempfile

source = Path("apps/tagcache.c").read_text()


def function(signature):
    start = source.index(signature)
    brace = source.index("{", start)
    depth = 1
    end = brace + 1
    while depth:
        depth += (source[end] == "{") - (source[end] == "}")
        end += 1
    return source[start:end]


event = source.split("            case Q_FLUSH_STATS:\n", 1)[1]
event = event.split("                break;\n", 1)[0]
harness = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#define UI_COMMIT 1
#define HZ 100
@DEADLINE@
#define TIME_BEFORE(a,b) ((long)((a)-(b)) < 0)
#define TIME_AFTER_EQ(a,b) (!TIME_BEFORE(a,b))
#define SYS_USB_CONNECTED 100
#define Q_FLUSH_STATS 101
struct queue_event { long id; };
static int tagcache_queue;
static bool ui_shutdown_intent, ui_shutdown_flush_pending;
static bool ui_shutdown_flush_done, ui_shutdown_flushing;
static bool tagcache_usb_quiescing, ui_commit_cancel, ui_stats_recovering;
static long current_tick, ui_shutdown_flush_deadline;
static bool queue_empty = true, block_worker;
static int posted, syncs, compactions, ordinary_flushes, sleeps;
static volatile bool ui_drain_force;
static volatile bool ui_worker_usb_parked;     /* #118 */
static long peek_event;
static bool usb_behind_stop;
#define COMMAND_QUEUE_IS_EMPTY queue_empty
#define ui_log(...) ((void)0)
static void do_timed_yield(void) {}
static bool check_event_queue(void) { return peek_event || ui_commit_cancel; }
#define QPEEK_FILTER1(a) (&(const long [2]){ (a), (a) })
static bool queue_peek_ex(int *q, struct queue_event *ev, unsigned flags,
                          const long (*filter)[2])
{
    (void)q; (void)ev; assert(flags == 0 && (*filter)[0] == SYS_USB_CONNECTED);
    return peek_event == SYS_USB_CONNECTED || usb_behind_stop;
}
static void queue_post(int *q, long id, intptr_t data)
{ (void)q; (void)data; assert(id == Q_FLUSH_STATS); posted++; }
static __attribute__((unused)) void command_queue_sync_callback(void)
{ assert(ui_shutdown_flushing); syncs++; if (!ui_stats_recovering) queue_empty = true; }
static __attribute__((unused)) void ui_journal_compact_worker(void)
{ compactions++; assert(!"a compaction during shutdown (B-M5)"); }
static int appends, pathqs;
/* one drain: the queue, then any RAM-coalesced rows, as frames (B-M5) */
static void ui_stats_drain(void)
{ assert(ui_shutdown_flushing); appends++; ui_stats_recovering = false; queue_empty = true; }
static void ui_pathq_run(void) { pathqs++; }
/* #89: the boot stamp, only after the flush and never at the USB handover */
static int stamps;
static void ui_boot_stamp_clean(void)
{ assert(ui_shutdown_flush_done && !tagcache_usb_quiescing); stamps++; }
static void run_command_queue(bool force)
{ assert(force); ordinary_flushes++; }
static void process_flush(void);
static void sleep(int ticks)
{
    current_tick += ticks;
    sleeps++;
    if (!block_worker && ui_shutdown_flush_pending) process_flush();
}
'''
deadline = next(l for l in source.splitlines()
                if l.startswith("#define UI_STATS_SHUTDOWN_TICKS"))
harness = harness.replace("@DEADLINE@", deadline)
harness += function("static bool ui_generation_cancel(void *opaque)")
harness += "\nstatic void process_flush(void) {\n" + event + "\n}\n"
harness += function("void tagcache_shutdown(void)")
harness += r'''
static void reset(void)
{
    ui_shutdown_intent = ui_shutdown_flush_pending = false;
    ui_shutdown_flush_done = ui_shutdown_flushing = false;
    tagcache_usb_quiescing = ui_commit_cancel = ui_stats_recovering = false;
    current_tick = ui_shutdown_flush_deadline = 0;
    queue_empty = true; block_worker = false; peek_event = 0; usb_behind_stop = false;
    ui_worker_usb_parked = false;
    posted = syncs = compactions = ordinary_flushes = sleeps = 0;
    appends = pathqs = 0;
}
int main(void)
{
    /* no shutdown intent (other system_flush() callers): the worker is
     * asked to drain, at most a second's wait, nothing on this thread */
    reset(); tagcache_shutdown(); assert(ordinary_flushes == 0 && posted == 0);
    assert(!ui_drain_force);        /* nothing queued: nothing to ask */
    reset(); queue_empty = false; tagcache_shutdown();
    assert(ui_drain_force && ordinary_flushes == 0 && posted == 0);
    /* #118: the worker has acked USB (it drains after the unplug): no
     * wait at all, the request stays for it */
    reset(); queue_empty = false; ui_worker_usb_parked = true;
    tagcache_shutdown();
    assert(ui_drain_force && sleeps == 0 && posted == 0);
    reset(); ui_shutdown_intent = true; queue_empty = false;
    ui_stats_recovering = true; peek_event = 1; /* shutdown Q_STOP_SCAN */
    ui_commit_cancel = true; tagcache_shutdown();
    /* B-M5: appends only (queue, then RAM-coalesced rows), no compaction */
    assert(posted == 1 && syncs == 0 && appends == 1 && compactions == 0);
    assert(pathqs == 1);
    assert(UI_STATS_SHUTDOWN_TICKS <= 3 * HZ);
    assert(ui_shutdown_flush_done && !ui_shutdown_flush_pending && queue_empty);
    reset(); ui_shutdown_intent = true; tagcache_usb_quiescing = true;
    tagcache_shutdown(); assert(posted == 0 && syncs == 0 && ordinary_flushes == 0);
    reset(); ui_shutdown_intent = true; ui_shutdown_flush_pending = true;
    ui_shutdown_flush_done = true; tagcache_shutdown();
    assert(posted == 0 && ui_shutdown_flush_done); /* do not clear a completion */
    reset(); ui_shutdown_intent = true; block_worker = true;
    tagcache_shutdown(); assert(posted == 1 && !ui_shutdown_flush_done);
    assert(current_tick == UI_STATS_SHUTDOWN_TICKS + HZ);
    assert(ui_shutdown_flush_pending); /* still worker-owned after timeout */
    reset(); ui_shutdown_flush_pending = true; process_flush();
    assert(syncs == 0 && compactions == 0 && ui_shutdown_flush_done);
    reset(); ui_shutdown_flushing = true; ui_shutdown_flush_deadline = 100;
    peek_event = 1; ui_commit_cancel = true; assert(!ui_generation_cancel(NULL));
    usb_behind_stop = true; assert(ui_generation_cancel(NULL)); usb_behind_stop = false;
    peek_event = SYS_USB_CONNECTED; assert(ui_generation_cancel(NULL));
    peek_event = 0; current_tick = 100; assert(ui_generation_cancel(NULL));
    current_tick = 0; tagcache_usb_quiescing = true; assert(ui_generation_cancel(NULL));
    puts("tagcache shutdown: actual-C event/cancellation/USB/deadline checks pass");
    return 0;
}
'''

with tempfile.TemporaryDirectory(prefix="tagcache-shutdown-") as temporary:
    cfile = Path(temporary) / "shutdown.c"
    binary = Path(temporary) / "shutdown"
    cfile.write_text(harness)
    subprocess.run([os.environ.get("CC", "cc"), "-std=c99", "-Wall", "-Wextra",
                    "-Werror", str(cfile), "-o", str(binary)], check=True)
    subprocess.run([str(binary)], check=True)
