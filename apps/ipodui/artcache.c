/***************************************************************************
 *              __________                   __    __________
 *   Open       \______   \_______    ____  |  | __\______   \  ____  ___  ___
 *   Source      |     ___/\_  __ \  /  _ \ |  |/ / |    |  _/ /  _ \ \  \/  /
 *   Player      |    |     |  | \/ (  <_> )|    <  |    |   \(  <_> ) >    <
 *   Firmware    |____|     |__|     \____/ |__|_ \ |______  / \____/ /__/\_ \
 *                                               \/        \/               \/
 *
 * Custom firmware for the iPod Video, 5th generation (5G and 5.5G).
 * Not affiliated with or endorsed by Apple Inc.
 *
 * iPod UI shell: album art cache (see artcache.h, artfmt.h and
 * docs/design/artcache.md)
 *
 * Copyright (C) 2026 prokopto-dev
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This software is distributed on an "AS IS" basis, WITHOUT WARRANTY OF ANY
 * KIND, either express or implied.
 *
 ****************************************************************************/
#include "config.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "string-extra.h"
#include <limits.h>
#include <errno.h>
#include "system.h"
#include "kernel.h"
#include "thread.h"
#include "core_alloc.h"
#include "file.h"
#include "dir.h"
#include "lcd.h"
#include "metadata.h"
#include "metadata/chapters.h"
#include "tagcache.h"
#include "settings.h"
#include "albumart.h"
#include "jpeg_load.h"
#include "bmp.h"
#include "resize.h"
#include "jpeg_common.h"           /* yuv_to_rgb() */
#include "storage.h"
#include "power.h"
#include "powermgmt.h"
#include "button.h"
#include "audio.h"
#include "playback.h"
#include "usb.h"
#include "events.h"
#include "appevents.h"
#include "logf.h"
#include "debug.h"
#include "crc32.h"
#include "ipodui/anim.h"
#include "ipodui/sortkey.h"
#include "ipodui/artcache.h"
#include "ipodui/maint.h"
#include "ipodui/header.h"            /* UI_BATT_RED_PCT */
#include "ipodui/artblur.h"
#include "ipodui/safefile.h"
#include "mv.h"                     /* volume_size(): the pool's cap (Q9) */
#include "timefuncs.h"              /* the rotation's clock (#100) */
#include "ipodui/align_check.h"
#include "ipodui/ab_book.h"
#include "ipodui/library.h"             /* lib_book_tracks() */
#include "ipodui/np_draw.h"             /* QW2: the composed NP art */
#include "ipodui/unlit.h"               /* ui_unlit_active(): idle build */
#include "ipodui/uilog.h"               /* pass and wake lines (P10) */
#include "hang_watch.h"                  /* the hang watcher */
#include "ipodui/np_playback.h"          /* np-sharp: the playing track */
#include "ipodui/np_pbcore.h"           /* np_seq_stable() */
#include "ipodui/art_bcm.h"             /* P19 19-6: DECODE on the BCM */
#ifdef SIMULATOR
#include "ipodui/simclock.h"            /* art_decode_spin (npfix) */
#endif
#include "backlight.h"
#include "devstats.h"   /* Fit-3 step 0: empty unless HAVE_DEVSTATS */

/* Tunables (artcache.md §8) */
#define COMMIT_EVERY    32          /* albums per index commit ... */
#define COMMIT_TICKS    (30*HZ)     /* ... or this long, whichever first */
#define INPUT_QUIET     (2*HZ)      /* no work this long after a key
                                       (stopped, paused; playing on the
                                       charger) */
#define GAP_TICKS       (HZ/10)     /* sleep between albums */
#define GAP_CHARGING    (HZ/50)     /* ... on external power */
#define RETRY_TICKS     (HZ/10)     /* a maintenance pass, after a refusal */
#define VIZ_TICKS       (2*HZ)      /* "viz": the view's close wakes the
                                       builder; this only a net */
#define POLL_TICKS      (HZ/4)      /* re-check while waiting (not while
                                       playing: that waits for an event) */
#define STOP_HOLD       (25*HZ)     /* alive this long after a power-off:
                                       it was aborted (powermgmt forces the
                                       power-off after 20 s) */
#define LOW_BATTERY     UI_BATT_RED_PCT     /* percent, when not charging */
#define SRC_CRC_BYTES   (16*1024)
#define FILL_MAX        4           /* fill size at most 4x the box side */
#define DECODE_BOOST    (10*HZ)     /* a decode boosted with audio stopped
                                       (or paused on the charger): at most
                                       this long, the rest unboosted */
#define TCHUNK          64          /* album table records read at once */
#define DELTA_MAX       COMMIT_EVERY
#define FREEMAP_WORDS   (ARTIDX_MAX_RECS / 32)

#define F_VERSION   ARTCACHE_DIR "/VERSION"
#define F_INDEX     ARTCACHE_DIR "/index.dat"
#define F_INDEX_TMP ARTCACHE_DIR "/index.tmp"
#define F_INDEX_PREV ARTCACHE_DIR "/index.prev"
#define F_PAK       ARTCACHE_DIR "/thumbs.pak"
#define F_PAK_TMP   ARTCACHE_DIR "/thumbs.pak.tmp"
#define F_PANELS    ARTCACHE_DIR "/panels.pak"
#define F_PANELS_TMP ARTCACHE_DIR "/panels.pak.tmp"
#define F_PANELS_PREV ARTCACHE_DIR "/panels.pak.prev"
/* #100: the panel pool (artfmt.h): pixels written in place, the index
 * replaced by safefile_replace() */
#define F_POOL      ARTCACHE_DIR "/panelpool.pak"
#define F_POOLX     ARTCACHE_DIR "/panelpool.idx"
#define F_POOLX_TMP ARTCACHE_DIR "/panelpool.idx.tmp"
#define F_POOLX_PREV ARTCACHE_DIR "/panelpool.idx.prev"
/* np-sharp: the Now Playing row's sharp cover for an album no panel set
 * holds (artfmt.h: two slots, written in place) */
#define F_NPCOVER   ARTCACHE_DIR "/npcover.pak"
/* ... rendered for an album played this long (artcache_rotate_poll()) */
#define NPR_PLAYED_MS   30000
/* Q8: the shown set is re-picked at every restart and this often (wall
 * clock: the RTC counts time asleep, P18 Q8) */
#define PANEL_ROTATE_SECS  (3 * 60 * 60)
#define PANEL_ROTATE_TICKS (PANEL_ROTATE_SECS * HZ)
/* Q9: the pool's covers outside the default set, by free space */
#define POOL_FREE_FULL  (1024ull * 1024 * 1024)     /* >= 1 GB: all */
#define POOL_FREE_SOME  (256ull * 1024 * 1024)      /* >= 256 MB: 32 */
/* top-up builds per pass (the window, an event or a key end it sooner;
 * each pass in a window walks the album table once, so few passes) */
#define POOL_STEP       32
/* P6 books (ab_book.h; Rev 1 item 4): independent of VERSION and of the
 * art index, never wiped with them */
#define F_BPAK      ARTCACHE_DIR "/books.pak"
#define F_BPAK_TMP  ARTCACHE_DIR "/books.pak.tmp"
#define F_BIDX      ARTCACHE_DIR "/books.idx"
#define F_BIDX_TMP  ARTCACHE_DIR "/books.idx.tmp"
#define F_BIDX_PREV ARTCACHE_DIR "/books.idx.prev"
#define BOOKS_BATCH 8               /* built books per books.idx rewrite */
#define BK_SKIP_MAX 16              /* unbuildable books remembered (L2) */
#define CHAP_SKIP_MAX 8             /* books whose chapter art failed */
#define CHAP_TRIES  3               /* passes a book's chapter art may fail
                                       before it is left (the cover) until
                                       a request, USB or the next boot */
#if LCD_PIXELFORMAT != RGB565
#error "artblur_fill() reads and writes RGB565"
#endif
#define VERSION_STR "IPAC 5\n"    /* 2: 55 px thumbs; 3: Now Playing
                                     bundles (Phase 5); 4: non-square
                                     thumbnails fitted (Phase 6); 5: on
                                     a blurred-cover fill (P6 owner) */

#define ART_Q_BUILD 1
#define ART_Q_IDLE  3               /* playback stopped or paused, or the
                                       user went idle (face up, backlight
                                       off), with the builder waiting for
                                       it (wakes the wait) */
#define ART_Q_NP    4               /* the Now Playing bundle read: NP's
                                       request, or a fill's end (np_serve()) */
#define ART_Q_N     5               /* the ids art_post() keeps one of */

/* Builder trace in sim.log (UPDATE_TRACE; D's trace rules read it):
 *   art build start total=N sweep=0|1
 *   art build wait <why>         off / paused / input / db / battery /
 *                                lit (playing, the user not idle) / -
 *                                (on each change)
 *   art build album key=K state=S src=R ms=T boost=B noboost=N
 *   art build commit gen=G recs=N
 *   art build done albums=N ms=T | art build stop <why>
 *   art chapter block alloc bytes=N filebuf=F | art chapter block freed
 *                                playing=P (the chapter pass's transient
 *                                block, stopped only)
 *   art build chapters book=K chapters=N images=I bundles=B recs=R rc=C
 *                                ms=T boost=B (one book; rc 0 done, -1
 *                                skipped, -2 deferred, -3 cap)
 *   art build chapters deferred | cap recs=N | done books=N ms=T
 *   art chapter block giveup at=alloc|art|crc|write
 *                                (finding 4: audio started while the
 *                                block was held: freed at once, the
 *                                audio buffer regrown, the book deferred)
 *   art chapter dedupe images=I copies=C crc_reads=R
 *                                (finding 8: C byte-identical copies found
 *                                by size + CRC-32, R images read for it)
 *   art alloc failed what=chapters|panel|blocks bytes=N
 *   art io index write-failed|unreadable|torn|invalid
 *   art io read|write key=K playing=P active=A idle=I
 *                                (before each album's source reads and
 *                                each slot write: P audio playing, not
 *                                paused, A storage_disk_is_active(), I
 *                                the user idle (user_idle()); the
 *                                "artio" rule: never P=1 with I=0)
 *   art wake ev=E                the finished builder woke (E: the
 *                                pump() events; the "idlebuild" scripts:
 *                                none over a long idle once done)
 * B is the firmware's boost count on target (0 in the simulator, which
 * has no CPU frequency control); N counts the boosts resize_on_load()
 * skipped on this thread, so N > 0 shows the guard was reached.
 * UI side: art get key=K res=R (one per artcache_get()). */
#if defined(HAVE_ADJUSTABLE_CPU_FREQ) && !defined(SIMULATOR)
#define ART_BOOST() get_cpu_boost_counter()
#else
#define ART_BOOST() 0
#endif
#define KEYFMT "%08lx%08lx"
#define KEYARG(k) (unsigned long)((k) >> 32), (unsigned long)((k) & 0xffffffffu)
/* Storage awake (an audio refill): on the target the ATA/flash driver's
 * own state. The simulator (hostfs) has none, so it is modelled on the
 * ATA driver: a fill's end (BUFFER_EVENT_FILL_DONE, then storage_sleep())
 * leaves the disk on for HZ/5 (ata.c: Q_STORAGE_SLEEP), and every access
 * keeps it on for the spin-down time after it (keep_disk_active()). The
 * script command "disk_active <ms>" (uisimulator/common/sim_cmd_art.c)
 * sets it directly. */
#ifdef SIMULATOR
#ifdef HAVE_FLASH_STORAGE
#define SIM_SPINDOWN    (1*HZ)      /* ata.c ATA_SLEEP_TIMEOUT_DEFAULT (C1) */
#else
#define SIM_SPINDOWN    (5*HZ)      /* ata.c sleep_timeout default */
#endif
extern long sim_disk_active_until;
static bool disk_active(void)
{
    return sim_disk_active_until
        && TIME_BEFORE(current_tick, sim_disk_active_until);
}
static void disk_touch(void)
{
    long t = current_tick + SIM_SPINDOWN;

    if (!sim_disk_active_until || TIME_AFTER(t, sim_disk_active_until))
        sim_disk_active_until = t ? t : 1;
}
#else
#define disk_active() storage_disk_is_active()
#define disk_touch()  do { } while (0)
#endif

bool artcache_disk_awake(void)
{
    return disk_active();
}

/* QW2: the record tag of the current Now Playing layout (artfmt.h) */
#define NP_TAG      artrec_np_tag_for(NP_LAYOUT_HASH)
#define NP_TAGGED(f) (((f) & ~ARTREC_F_NPTAG_MASK) \
                      | (NP_TAG << ARTREC_F_NPTAG_SHIFT))
typedef char np_one_read_ok[(ARTCACHE_NP_ONE_READ == ARTBUNDLE_HDR_BYTES
                             + NP_COMPOSED_W * NP_COMPOSED_H * 2) ? 1 : -1];

/* Audio playing, not paused: the builder only works while the user is
 * idle then (user_idle()). A pause spins the disk down like a stop, so it
 * gets normal throttling. */
static bool playing(void)
{
    return (audio_status() & (AUDIO_STATUS_PLAY | AUDIO_STATUS_PAUSE))
           == AUDIO_STATUS_PLAY;
}

static volatile long last_input = -INPUT_QUIET;  /* artcache_note_input() */

/* #58 idle build (owner, 2026-10-09). While audio plays the builder does
 * nothing while the user interacts (no UI hitch, no storage contention
 * with a refill), and runs continuously, waking storage when it needs to
 * (the backlog is finite), while the user is idle: the unlit clock face
 * is up or the backlight is off, no key held; on the charger also when
 * no key came for INPUT_QUIET (the backlight may stay on there). Read by
 * the builder only; the face and backlight states are single words. */
static bool user_idle(void)
{
    if (button_status())
        return false;
    if (ui_unlit_active() || !is_backlight_on(false))
        return true;                /* (false: "Off" counts as lit) */
    return charger_inserted()
           && !TIME_BEFORE(current_tick, last_input + INPUT_QUIET);
}

/* May the builder touch storage now? Always while stopped or paused
 * (the old throttle, blocked()); while playing only with the user idle. */
bool artcache_user_idle(void)
{
    return user_idle();
}

static bool io_ok(void)
{
    return !playing() || user_idle();
}

/* The chapter art and the books (owner, #58, 2026-10-19: the 7G is hardly
 * ever stopped, so nothing waits for a stop): they wait while audio plays,
 * and while paused with the user active (user_idle()) */
static bool art_hold(void)
{
    int st = audio_status();

    return st && (!(st & AUDIO_STATUS_PAUSE) || !user_idle());
}

static void io_trace(const char *rw, artcache_key_t key, bool play,
                     bool idle)
{
    UPDATE_TRACE("art io %s key=" KEYFMT " playing=%d active=%d idle=%d",
                 rw, KEYARG(key), (int)play, (int)disk_active(), (int)idle);
    (void)rw; (void)key; (void)play; (void)idle;
    disk_touch();
}
#define IO_TRACE(rw, key) io_trace(rw, key, playing(), user_idle())
struct bwork;
static bool io_go(struct bwork *w, const char *rw, artcache_key_t key);
static bool panel_stage(struct bwork *w);
#ifdef SIMULATOR
void sim_read_count2_begin(void);   /* uisimulator/common/filesystem-sim.c */
int sim_read_count2_end(unsigned long *bytes);
#endif
static bool regroup_window(void);       /* #92 */
static const char *blocked(void);
static bool art_q_waiting(void);
static bool regroup_owed;               /* #92: panels.pak not grouped */
static bool pool_owed;                  /* #100: the pool's top-up */
static bool rot_boot_done;              /* #100: this boot's rotation */
static bool rot_no_pool;                /* P18 Q8: no panels.pak to rotate */
static volatile bool rot_ready;         /* the builder waits, no pass owed:
                                         * a due rotation would run now */
static volatile uint32_t rot_last;      /* its clock (pool_now()) */
#ifdef SIMULATOR
static volatile long sim_rot_warp;      /* panel_rotate_warp (s) */
#endif
static bool sig_due(void);              /* #90 */
static long sig_epoch_input = -1, sig_epoch_pause = -2;
static bool sig_owed;           /* #90: the last pass left records unsigned */
static int sig_win_n;           /* #90 review: fills (or tries) this window */
static int pump(int ticks);
#define ALBUM_LIT (-3)              /* album(): the user came back */

/* --- shared state (small; everything big is in the blocks) ------------- */
static int dec_h, stack_h;
static struct bwork *bw;            /* in the stack block: pinned for good */
static struct rblock *rblk;         /* the UI's read block: there too */
static struct event_queue art_queue SHAREDBSS_ATTR;
static unsigned int art_thread_id;
static bool idle_seen;               /* builder only: an ART_Q_IDLE pumped */

/* npfix review M1: one of each ART_Q_* in the queue at most. They come
 * from several threads (fills, idles, polls, requests) while a builder
 * held in a park or a long step takes events late, and a full queue is a
 * panic. Cleared when the builder takes the event (art_taken()); a lost
 * race posts one more, never an unbounded run. */
static volatile bool q_posted[ART_Q_N];

static void art_post(int id)
{
    if (!q_posted[id])
    {
        q_posted[id] = true;
        queue_post(&art_queue, id, 0);
    }
}

static void art_taken(long id)
{
    if (id > 0 && id < ART_Q_N)
        q_posted[id] = false;
}
static volatile int pause_count;
static volatile bool idle_wait;     /* the builder waits for the user to go
                                       idle (or playback to stop): the UI
                                       posts ART_Q_IDLE then, else no post */
/* Armed before the GUI acknowledges USB exclusive storage. This is earlier
 * than the builder's queued SYS_USB_CONNECTED event. */
static volatile bool usb_quiescing;
/* P18 (#86, review M1): sleep entry: no new filesystem operation (io_go()
 * is the builder's safe point) until the wake */
static volatile bool sleep_hold;
/* USB owns storage, or is about to: the GUI armed the handover, or a host
 * has addressed the device (SET_ADDRESS comes just before the
 * SET_CONFIGURATION that requests storage). Not usb_inserted(): that is
 * also VBUS alone (USB_POWERED), which a wall charger or a charge-only
 * cable keeps for as long as it is plugged in, so no art was ever built
 * on a charger. */
static inline bool usb_owns_disk(void)
{
    return usb_quiescing || usb_storage_host_present();
}

#ifdef HAVE_ADJUSTABLE_CPU_FREQ
/* A maintenance pass runs boosted, but never while it waits (a low
 * battery, the database, USB): the waits drop the boost and take it
 * back before the work goes on. One boost at most, so it stays balanced
 * however the pass ends. */
static bool fast_boosted;
static void fast_boost(bool on)
{
    if (on != fast_boosted)
    {
        fast_boosted = on;
        cpu_boost(on);
    }
}
#define FAST_BOOSTED() fast_boosted
#else
#define fast_boost(on) ((void)(on))
#define FAST_BOOSTED() false
#endif
static volatile int commit_seq;     /* + 1 after every index rename */
static volatile bool reader_stale;  /* USB: the UI reopens its fds */
static volatile bool stopped;       /* power-off / reboot under way */
static long stopped_tick;           /* when (STOP_HOLD) */
static volatile bool want_build;    /* a request came before the thread */
static volatile bool maintenance;   /* explicit Music rebuild: no idle gaps */
static bool build_fast;             /* builder's current scheduling mode */
static int pass_events;             /* builder-only: events seen inside I/O */
/* #58: the albums the UI asked for and found PENDING (list rows on
 * screen, the Now Playing album), newest first: the builder does them
 * before the rest of its pass (run_hints()). Written by the UI thread,
 * read by the builder: a torn 64-bit key is just a key not found. */
#define ART_HINTS   8
/* A damaged slot is a local repair, never permission to erase the cache.
 * UI producer/builder consumer, like hint_keys (a torn key only retries an
 * unrelated album, which is harmless). Cleared only after publication. */
static volatile uint64_t repair_keys[ART_HINTS];
#define ART_HMISS   16
/* albums built out of order this pass (the panel set, hints): the walk
 * skips them, a sweep would check their sources twice */
#define ART_EARLY   (ARTPANEL_SLOTS + 16)
static volatile uint64_t hint_keys[ART_HINTS];
static volatile int hint_seq;       /* + 1 per new key */
#ifdef SIMULATOR
static volatile bool sim_blocked;   /* the thread waits for a request */
static volatile long sim_panel_hold; /* art_panel_hold (ms) */
static volatile int sim_done_gen = -1; /* db generation of the last full
                                          pass */
static unsigned sim_progress_seen, sim_progress_advanced;
static bool sim_progress_bad;
#endif
static struct artcache_build_status status;

/* --- the slice meter (p19-art-yield) ------------------------------------ *
 * The kernel is cooperative: it switches only where a thread yields or
 * blocks, so the builder's longest run between two switches is the
 * longest a woken UI thread can wait for it (on the device: devstats'
 * slice_max_us, powerlog's "m0 slice max" line, 68-166 ms for the
 * builder at bench-20261025-2257). The simulator measures the builder's
 * own thread CPU time between two switches (firmware/kernel/thread.c's
 * sim_run_hook()); the device's figure is roughly SIM_FACTOR (x400,
 * scripts/check_trace.py) times it for CPU work, so SLICE_OVER is the
 * 10 ms budget (the host's file calls are no model of the device's).
 *   art slices phase=P max_us=N over=K n=M from=A to=B
 *       a phase's slices, at its end and at the pass end: the longest
 *       (N), the ones over the budget (K) of all (M), and the source
 *       lines of the ART_PACE() points the longest began and ended
 *       after (0: none yet) */
static unsigned long pace_t0;       /* ART_PACE(): the run's start */
#ifdef SIMULATOR
#define SLICE_OVER  25              /* us of host CPU: ~10 ms on the ARM */
extern void (*sim_run_hook)(unsigned int id, bool out);
static unsigned long slice_t0;      /* this run's start (thread CPU us) */
static unsigned long slice_max, slice_n, slice_over;
static int pace_line;               /* the last ART_PACE() passed */
static int slice_from, max_from, max_to; /* its lines: began, ended after */

static void slice_hook(unsigned int id, bool out)
{
    unsigned long now, d;

    if (!art_thread_id || id != art_thread_id)
        return;
    now = sim_clock_cpu_us();
    if (!out)
    {
        slice_t0 = now ? now : 1;
        pace_t0 = now;
        slice_from = pace_line;
        return;
    }
    if (!slice_t0)
        return;
    d = now - slice_t0;
    slice_t0 = 0;
    slice_n++;
    if (d > SLICE_OVER)
        slice_over++;
    if (d > slice_max)
    {
        slice_max = d;
        max_from = slice_from;
        max_to = pace_line;
    }
}

static void slice_report(void)
{
    if (slice_n)
        UPDATE_TRACE("art slices phase=%d max_us=%lu over=%lu n=%lu "
                     "from=%d to=%d", (int)status.phase, slice_max,
                     slice_over, slice_n, max_from, max_to);
    slice_max = slice_n = slice_over = 0;
    if (slice_t0)
        slice_t0 = sim_clock_cpu_us();  /* the report is not the builder's */
}
#else
#define slice_report() do { } while (0)
#endif

/* Only the builder writes progress. UI readers take a best-effort snapshot,
 * as with done/total. A zero total means unknown, so no counter is shown. */
static void build_phase(enum artcache_build_phase phase, int total)
{
    slice_report();
    status.phase_done = 0;
    status.phase_total = total;
    status.phase = phase;
#ifdef SIMULATOR
    sim_progress_seen |= 1u << phase;
#endif
    UPDATE_TRACE("art phase=%d total=%d", (int)phase, total);
    ui_log("art", "phase=%d total=%d", (int)phase, total);
}

static void build_advance(void)
{
    status.phase_done++;
#ifdef SIMULATOR
    sim_progress_advanced |= 1u << status.phase;
    if (status.phase_done > status.phase_total)
        sim_progress_bad = true;
#endif
}
static struct artcache_dim thumb;   /* artcache_sizes[ARTCACHE_THUMB] */
static uint32_t slot_bytes;
static size_t decode_bytes;
/* Panel art (H1): artcache_sizes[ARTCACHE_PANEL] and its slot. Decoded
 * in bands of the decode block since #58 (ps_decode()): no transient
 * block any more (it was 785,844 B on the target, stopped only). */
static struct artcache_dim panel;
static uint32_t panel_slot_bytes;
static volatile int panel_seq;      /* + 1 per panels.pak header write
                                       (and USB): readers reload it */
/* np-sharp: npcover.pak. npc_seq + 1 before a render's first write and
 * + 1 after its header (odd: one under way); the album queued for one
 * (artcache_npcover_queue(), 0 none); the albums whose render failed
 * (RAM only: again after USB or a boot) */
/* review L4: plain volatiles, no lock: the UI, tagcache (maint_poll())
 * and builder threads all run on the CPU today, so a 64-bit npr_key or
 * npc_seq is never torn or reordered against another core. Moving the UI
 * to the COP (#72) needs atomics or a lock here (and for npf_key). */
static volatile int npc_seq;
static volatile artcache_key_t npr_key;
/* builder only, while npr_run() renders: the render is under way (io_go()
 * and ps_flush() refuse without the disk awake), it was called off by
 * art_gate() (review M2/M3), when it began, and its source's fd */
static bool npr_busy, npr_abort;
static long npr_t0;
static int npr_src_fd = -1;
#define NPR_FAIL_MAX 4
static artcache_key_t npr_fail[NPR_FAIL_MAX];
static int npr_fail_at;
/* Work that waits for playback to stop (H1): the final commit's GC
 * (#41.1: not while audio plays). A pass runs again for it. (The panel
 * art waited too until #58: it now builds during playback too, while the
 * user is idle.) */
static bool gc_owed;
/* Chapter art (Phase 5, chapter-art.md): the pass after the panels, only
 * while stopped, waits for the stop like the GC; chap_stale: a book with
 * chapter records was rewritten for another mtime (its old chapter
 * records go at the GC that follows its chapter pass). chap_bytes: the
 * transient block (the chapter table, then the art table at
 * chap_tab_off): 69,640 + 8,192 = 77,832 B on the target. */
static bool chapters_owed, chap_stale;
static bool chap_later;             /* paused: books with chapter images
                                       wait for the stop (CHAP_LATER) */
static size_t chap_bytes, chap_tab_off;
/* books.pak / books.idx (P6, Rev 1 item 4): the pass after the chapter
 * art, only while stopped, for every book row whose index record is
 * missing or stale. It works inside the decode block (BOOKS_BYTES, see
 * artcache_reserve()). */
static bool books_owed;

/* --- builder work block ------------------------------------------------- *
 * Everything the builder reads or writes files into lives here, in the
 * thread's stack block (allocated with it at shell start and pinned like
 * any running thread's stack), so no I/O ever targets memory that buflib
 * may move while the read yields. The one big movable block, the decode
 * buffer (dec_h), is pinned only around a decode (decode()) and holds no
 * pointer across any other I/O: the GC re-fetches it after every read. */
struct bwork {
    struct artidx_reader rd;        /* the committed index */
    struct mp3entry id3;            /* never on the thread's stack */
    struct tagcache_search tcs;
    char path[MAX_PATH];            /* representative track */
    char src[MAX_PATH];             /* image file source */
    char album[MAX_PATH], artist[MAX_PATH], aartist[MAX_PATH];
    char tpath[MAX_PATH];           /* the album table (libfile.h) */
    struct artalb_hdr thdr;         /* album table being built */
    struct artalb_rec trec[TCHUNK];
    uint32_t tfirst;                /* table index of trec[0] */
    int tn, ti;                     /* records in trec, next one */
    bool sweep;                     /* re-check image-file sources */
    struct tagcache_context *context; /* retained namespace for this pass */
    int db_gen;                     /* tagcache_ui_generation() the table
                                       and its rep_idx belong to */
    bool gc_ok;                     /* the whole table was walked */
    bool failed;                    /* incomplete pass: never report success */
    bool deferred;                  /* work put off for playback (DEFER) */
    int fail_line;                  /* where it first failed (FAIL(w)) */
    bool panel_pending;             /* writes go to a private copy */
    bool panel_fresh;               /* the published file's chain is cut:
                                       the private copy starts empty */
    bool panel_live;                /* #117: panels.pak is published: a
                                       panel and a header change are
                                       written in place (panel_commit()) */
    uint32_t matched;               /* table albums found in the index */
    uint16_t sig_missing;           /* #90: OK records without a signature */
    uint16_t sig_tried;             /* ... of them filled (or tried) */
    uint16_t sig_kept;              /* tags changed, same source: kept */
    uint32_t start_count;           /* records at the start of the pass */
    uint32_t n_chap;                /* ... chapter records among them */
    struct artcache_rec delta[DELTA_MAX];
    int n_delta;
    long last_commit;
    struct artidx_hdr ihdr;         /* template for the next commit */
    uint32_t next_slot;             /* append point (== ihdr.n_slots) */
    uint32_t free_from;             /* free-slot search start */
    union {
        uint32_t freemap[FREEMAP_WORDS];/* slots used by committed records */
        uint32_t live[FREEMAP_WORDS];   /* final commit's GC: index records
                                           still in the table (no slot is
                                           allocated after the last album) */
    };
    uint32_t page[ARTIDX_PAGE_BYTES / 4];
    /* decode target (decode()): the thumbnail into the slot, from the
     * reserved decode block; or (stream) a panel, streamed in bands into
     * slot ps_slot of panels.pak (#58, ps_decode()) */
    int dec;                        /* core_alloc handle */
    size_t dec_bytes;
    int tw, th;
    bool stream;
    bool ps_too_wide;               /* stream: no room for a band */
    bool ps_io_err;                 /* stream: a write failed or the user
                                       came back while playing: again */
    bool lit;                       /* io_go() refused: the user came back
                                       while playing (album() redoes it) */
    bool source_terminal;           /* verified unsupported format, not I/O */
    bool source_retry;              /* at least one candidate was unreadable */
    uint32_t ps_slot;
    artcache_key_t ps_key;
#ifdef HAVE_BCMFW_VIDEO
    int ps_cw;                      /* stream: the picture's width in the
                                       slot, 0 = all (ARTPANEL_CW) */
    bool vid;                       /* the album being built is a video
                                       (LIBFILE_ALB_VIDEO): poster order */
#endif
#ifdef HAVE_ART_BCM
    bool bcm;                       /* P19 19-6: step 2 may go to the BCM
                                       (np-sharp's panel, the NP box) */
    bool ps_bcm_bad;                /* ... its rows failed: the CPU's */
    bool ps_resident;               /* np-sharp: decode on the BCM to be
                                       resident only (the file exists):
                                       nothing written, no CPU decode */
    bool ps_res_ok;                 /* ... and it is */
    artcache_key_t bcm_key;         /* the album's art key (np_write()) */
#endif
    size_t out_off;
    bool fit;                       /* decode to the fitted size (D10), no
                                       crop; the pixels at 'out_off' */
    struct artcache_fit fitted;     /* fit: the image (artcache_fit_rect) */
    struct artalb_rec pset[ARTPANEL_SLOTS];     /* the panel set */
    int hint_seen;                  /* hint_seq at the last look */
    int hmiss_n, hmiss_at;          /* hinted keys not in the table */
    uint64_t hmiss[ART_HMISS];
    int n_early;
    uint64_t early[ART_EARLY];      /* built by album_early() this pass */
    struct artpanel_hdr phdr;       /* panels.pak's header */
    const char *ps_path;            /* stream: the file the slot is in */
    bool ps_pool;                   /* ... panelpool.pak (#100) */
    bool ps_inplace;                /* ... the published panels.pak, in a
                                       slot its header does not name
                                       (#117): the old tag cleared first */
    bool no_boost;                  /* np-sharp: decode() never boosts */
    bool pidx_ok;                   /* #100: pidx loaded (this boot, USB) */
    struct artpool_idx pidx;        /* #100: panelpool.idx */
    /* books.idx as the books pass will write it (P6): the header on disk
     * (pak_size / dead_bytes ahead of it by the blobs appended since),
     * and the records of the books built since the last rewrite */
    struct ab_idx_hdr bhdr;
    bool bhdr_ok;                   /* books.idx valid (else a fresh pak) */
    const char *bpath;              /* current or retained previous index */
    bool books_defer;               /* unknown I/O state: no append */
    int n_bpend;
    struct ab_idx_rec bpend[BOOKS_BATCH];
    /* books that cannot be built as they are (P6 fix L2: more files than
     * a map holds, or a fallback file match that never equals the row's
     * count): not retried until their {max_mtime, count} change; kept
     * until the next boot, the oldest replaced when full */
    struct bk_skip {
        uint64_t key;
        uint32_t max_mtime, count;
    } bskip[BK_SKIP_MAX];
    int n_bskip;
    /* single-file books whose chapter pass failed, with the number of
     * passes it failed in: one book never holds up the others, and stops
     * failing the pass after CHAP_TRIES (cleared by a request or USB) */
    struct chap_skip {
        uint64_t key;
        uint32_t max_mtime;
        int tries;
    } chskip[CHAP_SKIP_MAX];
    int n_chskip;
    long area[];                    /* the slot being written */
};

static uint8_t *slot_buf(struct bwork *w) { return (uint8_t *)w->area; }

/* An incomplete pass: the first reason in ipodui.log, as the source line
 * of this build (the boot line names the build), the phase and errno */
#define FAIL(w) pass_fail((w), __LINE__)
/* Work put off while audio plays or the user is active: incomplete, but
 * not a failure; no reason logged, no retry, no count toward the cap.
 * The owed flags bring it back at the next idle (review MED-1). */
#define DEFER(w) ((w)->failed = (w)->deferred = true)
static void pass_fail(struct bwork *w, int line)
{
    w->failed = true;
    if (!w->fail_line)
    {
        w->fail_line = line;
        ui_log("art", "fail l=%d ph=%d e=%d", line, (int)status.phase, errno);
    }
}

/* Before an album's or a panel's source read and each of its writes:
 * false (w->lit set, no trace, no I/O) while audio plays and the user is
 * not idle (#58). The look and the trace are one step, so every "art io"
 * line with playing=1 has idle=1 (the "artio" rule). */
static bool io_go(struct bwork *w, const char *rw, artcache_key_t key)
{
    bool play = playing(), idle = user_idle();
    int ev, n;

    /* Do not begin another filesystem operation after USB was queued.  The
     * pump acknowledges it and blocks until disconnect, so the caller must
     * retry this album rather than touch a host-owned disk. */
    /* np-sharp review H1: every queued event, not just the first (a USB
     * session or a request behind a fill's ART_Q_NP post was missed until
     * the next check); bounded: the builder's own ids are posted once */
    ev = pump(0);
    for (n = 0; !ev && n < ART_Q_N + 4 && art_q_waiting(); n++)
        ev = pump(0);
    if (ev != 0)
    {
        pass_events |= ev;          /* wait_ready must not lose a restart */
        w->lit = true;
        return false;
    }
    /* The PP5022 sees VBUS before the USB stack reaches SET_CONFIGURATION
     * and broadcasts SYS_USB_CONNECTED.  That small window used to let an
     * art pass start one more (occasionally long) FAT operation, delaying
     * the later handover acknowledgement.  Refuse it once a host has
     * addressed the device (VBUS alone may be a charger: usb_owns_disk());
     * wait_ready() will consume the broadcast and sleep until unplugged. */
    if (usb_owns_disk() || sleep_hold
        || (npr_busy && (npr_abort || !disk_active())))
    {
        w->lit = true;              /* np-sharp: never a spin-up (M2) */
        return false;
    }
    /* A ready RAM library does not mean the directory scanner is idle.
     * Let that single storage walk finish before starting another image
     * read/write; the caller retries once wait_ready observes its release. */
    if (tagcache_scan_active() || (play && !idle))
    {
        w->lit = true;
        return false;
    }
    io_trace(rw, key, play, idle);
    return true;
}

/* --- decode gate (the Now Playing clock skips, npfix step 2) ------------ *
 * io_go() gates the builder's reads and writes, but a decode or a compose
 * that began while the user was idle ran to its end once they came back:
 * a large embedded cover is 10-17 s of decode at 50-80 % CPU (device log
 * run-1137), with the screen lit and the clock drawing. Now the decoders'
 * yield points (resize.c per line, resize_set_pause_hook(); jpeg_load.c
 * per MCU, jpeg_set_pause_hook()) and the composes (art_gate() between column
 * chunks) park the builder, blocked on its queue with the boost dropped,
 * while audio plays and the user is active or Now Playing is lit (review
 * M3). Nothing is redone: the decoder keeps its state and its pinned
 * block; its fd stays open, and no read happens while parked. USB, a
 * power-off, a reboot or a sleep entry releases the park at once (not a
 * battery or charger broadcast, review M2), so the pass can reach
 * pump(); the parked builder takes its queue (its own posts are folded
 * for pump(), review M1), so no post waits behind a park. Traces:
 * Shared decoder state (review): the core's JPEG decoder keeps its
 * struct jpeg in the caller's buffer (jpeg_load.c's static one is only
 * built with JPEG_FROM_MEM, the plugin library), so a decode parked here
 * for minutes shares nothing with another thread's decode (the buffering
 * thread's album art for the stock WPS in developer mode); the pause
 * hooks act on this thread only, and resize.c's boost bookkeeping stays
 * balanced (one cpu_boost count each way, released before a park).
 *   art park ms=N            a park, N ms long, once it ends
 *   art slice ms=N lit=1     a gap between two gate points over HZ/10
 *                            with the user active and audio playing
 *                            (wall time, other threads included; the
 *                            "npstall" rule: none) */
static long gate_last;              /* tick of the last gate point, 0 off */
/* review M1: a park takes the builder's queue (art_park()); what it took
 * that is pump()'s waits there (builder thread only) */
static struct queue_event park_ev;  /* a system event that ended a park */
static bool park_held;
static unsigned char park_fold;     /* 1 << ART_Q_BUILD | ART_Q_IDLE */
/* review L5: the builder is parked: what ends a park (a pause or stop,
 * the user idle, Now Playing gone, a sleep entry, the USB handover)
 * posts ART_Q_IDLE to end its wait; PARK_WAIT is the backstop (the
 * charger's INPUT_QUIET) */
static volatile bool park_wait;
#define PARK_WAIT   HZ

static void park_wake(void)
{
    if (park_wait && art_thread_id)
        art_post(ART_Q_IDLE);
}
#ifdef SIMULATOR
static volatile long sim_gate_spin; /* art_decode_spin (us per gate point) */
static volatile bool sim_parked;    /* expect_art_parked */
static volatile bool sim_npf_hold;  /* art_npf_hold */
#endif

static void np_serve(void);
static void aux_serve(void);

/* The art thread's stack, its high-water: a parked decode holds the
 * decoder's frames (JPEG, resize, the compose) for as long as the park
 * lasts. The device (developer builds) logs the high-water in percent of
 * the stack each time it grows ("art stack N% of B at park"); the sim
 * cannot (its threads run on host stacks) and traces the host depth
 * below the thread's entry instead ("art stack host=N"), an upper bound
 * (64-bit host frames). */
#define ART_STACK_SIZE (DEFAULT_STACK_SIZE + 0x2000)
#ifdef SIMULATOR
static uintptr_t sim_stack_base;    /* the art thread's entry frame */
#endif

static void stack_note(const char *at)
{
#ifdef SIMULATOR
    static unsigned long max;
    unsigned long d = sim_stack_base
        ? (unsigned long)(sim_stack_base
                          - (uintptr_t)__builtin_frame_address(0)) : 0;

    if (d > max && d < 0x100000)
    {
        max = d;
        UPDATE_TRACE("art stack host=%lu at=%s", d, at);
    }
    (void)at;
#elif defined(HAVE_IPOD_UI_DEVMODE)
    static unsigned int max;
    struct thread_debug_info ti;

    if (thread_get_debug_info(art_thread_id, &ti) > 0
        && ti.stack_usage > max)
    {
        max = ti.stack_usage;
        ui_log("art", "stack %u%% of %u at %s", max,
               (unsigned)ART_STACK_SIZE, at);
    }
#else
    (void)at;
#endif
}

/* an event waits for pump() (art_wait; io_go()'s drain) */
static bool art_q_waiting(void)
{
    return park_held || park_fold || !queue_empty(&art_queue);
}

/* review L3: an event that stops a panels.pak copy: any but ART_Q_NP
 * (a fill's end posts it while playing; the copy runs on through it) */
static const long q_not_np[2][2] = {
    { LONG_MIN, ART_Q_NP - 1 },
    { ART_Q_NP + 1, LONG_MAX },
};

static bool art_q_stops(void)
{
    return park_held || park_fold
           || queue_peek_ex(&art_queue, NULL, 1, q_not_np);
}

/* review M2: the system events that end a park (pump() acts on them);
 * any other (a battery update each poll, the charger, a file system
 * change) is taken and dropped, as pump() drops it */
static const long park_release[3][2] = {
    { SYS_USB_CONNECTED, SYS_USB_CONNECTED },
    { SYS_POWEROFF, SYS_POWEROFF },
    { SYS_REBOOT, SYS_REBOOT },
};

static bool park_releases(long id)
{
    return id == SYS_USB_CONNECTED || id == SYS_POWEROFF
           || id == SYS_REBOOT;
}

/* review M3: Now Playing's last pass (artcache_note_np()), 0: not up.
 * On a charger user_idle() turns true INPUT_QUIET after a key with the
 * screen still lit, so a park needs the screen itself: lit (not the
 * unlit face) with Now Playing up, its clock drawing every second. */
static volatile long np_seen;

static bool np_watched(void)
{
    long t = np_seen;

    return t && !TIME_AFTER(current_tick, t + 3 * HZ)
           && is_backlight_on(false) && !ui_unlit_active();
}

/* a decode now would show: the user is active, or Now Playing is lit */
static bool gate_watched(void)
{
    return !user_idle() || np_watched();
}

/* np-sharp: may the Now Playing cover's render run now? Storage already
 * awake (never a spin-up of its own: the same rule on a hard drive and
 * on flash), the user idle and Now Playing not lit; while playing only on
 * a charger (P19 D-3: no art work in dark playback on battery); never
 * through USB, a sleep hold or a pause request. blocked() is checked
 * with it, art_gate() parks the decode. Any thread: volatile reads. */
static bool npr_place(void)
{
    if (stopped || !disk_active() || np_watched() || usb_owns_disk()
        || sleep_hold || pause_count > 0)
        return false;
#ifdef HAVE_ART_BCM
    /* P19 19-6: the queued album is resident on the BCM and the Now
     * Playing row shows it: only its background read-back is left, which
     * never runs while the row is up (album-render.md section 14) */
    if (npr_key && art_bcm_np_is_shown()
        && art_bcm_resident_key() == npr_key)
        return false;
#endif
    if (playing())
        return user_idle() && charger_inserted();
    return !button_status()
           && !TIME_BEFORE(current_tick, last_input + INPUT_QUIET);
}

static bool npr_failed(artcache_key_t key)
{
    int i;

    for (i = 0; i < NPR_FAIL_MAX; i++)
        if (npr_fail[i] == key)
            return true;
    return false;
}

void artcache_npcover_queue(artcache_key_t key, int why)
{
    (void)why;                      /* trace only */
#ifdef HAVE_ART_BCM
    if (why == ARTCACHE_NPR_RESIDENT && !art_bcm_resident_wanted(key))
        return;
#endif
    if (!art_thread_id || !key || key == npr_key)
        return;
    if (npr_failed(key))
    {
        UPDATE_TRACE("art npcover drop key=" KEYFMT " state=fail why=%s",
                     KEYARG(key), why == ARTCACHE_NPR_PLAYED ? "played"
                                                             : "row");
        return;
    }
    npr_key = key;
    UPDATE_TRACE("art npcover queue key=" KEYFMT " why=%s", KEYARG(key),
                 why == ARTCACHE_NPR_PLAYED ? "played"
                 : why == ARTCACHE_NPR_RESIDENT ? "resident" : "row");
    if (npr_place() && !blocked())
        art_post(ART_Q_IDLE);
}

int artcache_npcover_seq(void)
{
    return npc_seq & ~1;            /* moves once a render is complete */
}

static bool gate_hold(void)
{
    return playing() && gate_watched() && !stopped && !usb_owns_disk()
           && !sleep_hold && !park_held
           && !queue_peek_ex(&art_queue, NULL, 2, park_release);
}

static void art_gate(void)
{
    long now = current_tick;
    bool parked = false;

    if (thread_self() != art_thread_id || !gate_last)
        return;                     /* another thread's decode, or none */
    hang_beat(HANG_ART);            /* a decode slice: progress */
#ifdef SIMULATOR
    if (sim_gate_spin)
    {
        /* the ARM's decode cost between two yield points: a busy wait
         * that holds the (cooperative) sim kernel like the device's CPU */
        unsigned long t = sim_clock_now_us();
        while (sim_clock_now_us() - t < (unsigned long)sim_gate_spin)
            ;
        now = current_tick;
    }
#endif
    if (TIME_AFTER(now, gate_last + HZ / 10) && playing() && gate_watched())
        UPDATE_TRACE("art slice ms=%ld lit=1", (now - gate_last) * 1000 / HZ);
    if (gate_hold())
    {
        bool fb = FAST_BOOSTED();
        long t0 = now;

        resize_boost_release();
        fast_boost(false);
#ifdef SIMULATOR
        sim_parked = true;
#endif
        park_wait = true;
        while (gate_hold())
        {
            struct queue_event ev;

            np_serve();             /* NP's bundle read is not held */
            aux_serve();            /* nor the visualizer's overlay (sitting
                                       4c: its view opened over a parked
                                       decode waited out VIZ_WAIT_MAX and
                                       drew on the CPU) */
            /* review M1: the queue is taken while parked (posts never
             * pile up): the builder's own posts are folded for pump(),
             * a system event ends the park and is pump()'s. L5: blocked
             * on it, not a poll */
            {
                int hw = hang_idle_begin();     /* parked: idle */
                queue_wait_w_tmo(&art_queue, &ev, PARK_WAIT);
                hang_idle_end(hw);
            }
            if (ev.id == SYS_TIMEOUT)
                continue;
            art_taken(ev.id);
            if (ev.id == ART_Q_NP)
                continue;           /* np_serve(), next turn */
            if (ev.id > 0)
            {
                if (ev.id < ART_Q_N)
                    park_fold |= 1u << ev.id;
                continue;
            }
            if (!park_releases(ev.id))
                continue;           /* review M2: dropped, as pump() would */
            park_ev = ev;
            park_held = true;
        }
        park_wait = false;
#ifdef SIMULATOR
        sim_parked = false;
#endif
        stack_note("park");
        if (fb)
            fast_boost(true);
        UPDATE_TRACE("art park ms=%ld", (current_tick - t0) * 1000 / HZ);
        (void)t0;
        parked = true;
    }
    /* np-sharp: the Now Playing render is called off (DEFERRED, queued
     * still) when the disk slept during a park (its next read would spin
     * it up, review M2), or, paused or stopped where gate_hold() does not
     * park, Now Playing lit or the user back since it began (M3). The
     * rest of the source reads as its end (no I/O), and ps_flush() and
     * io_go() write nothing more. */
    if (npr_busy && !npr_abort
        && ((parked && !disk_active()) || np_watched() || button_status()
            || TIME_AFTER(last_input, npr_t0)))
    {
        npr_abort = true;
        if (npr_src_fd >= 0)
            lseek(npr_src_fd, 0, SEEK_END);
        UPDATE_TRACE("art npcover abort disk=%d np=%d input=%d",
                     (int)disk_active(), (int)np_watched(),
                     (int)TIME_AFTER(last_input, npr_t0));
    }
    gate_last = current_tick ? current_tick : 1;
}

/* the JPEG decoder's abort hook: the Now Playing render was called off
 * (art_gate()): the rest of the decode is not done at all */
static bool npr_called_off(void)
{
    /* the builder's own decode only: the buffering thread and plugins
     * share the decoder (re-review M-a) */
    return thread_self() == art_thread_id && npr_busy && npr_abort;
}

/* artblur_fill()'s pause between its passes */
static void art_gate_yield(void)
{
    yield();
    art_gate();
}

static void gate_begin(void)
{
    gate_last = current_tick ? current_tick : 1;
}

static void gate_end(void)
{
    gate_last = 0;
}

/* --- the Now Playing bundle, read off the UI thread (npfix step 3) ------ *
 * A track change with the disk asleep made Now Playing read the new
 * track's bundle on the UI thread one pass later: a 450-590 ms power-up
 * (#114) with the clock stopped. Now the builder thread reads it into
 * npf_buf (ARTCACHE_NP_ONE_READ, in the pinned stack block):
 *  - ahead: at a fill's end (BUFFER_EVENT_FILL_DONE, storage still up) the
 *    playlist's next track's cover bundle, never waking storage itself;
 *  - on request: Now Playing found no copy (artcache_np_take()) and keeps
 *    its placeholder (art_defer) until this read lands (it may power the
 *    drive up, on this thread).
 * The UI copies it with one memcpy (artcache_np_take()). A commit or a USB
 * session makes the copy stale. Traces:
 *   art npf key=K res=R why=req|next disk=D ms=T */
enum { NPF_EMPTY = 0, NPF_BUSY, NPF_READY };
static char *npf_buf;               /* in the stack block, 4-aligned */
static volatile int npf_state;
static volatile artcache_key_t npf_key; /* what npf_buf holds (READY) */
static volatile int npf_res;        /* its read_bundle() result */
static volatile int npf_seq;        /* commit_seq when read */
static volatile int npf_usb;        /* panel_seq when read (USB bumps it) */
static int npf_w, npf_h;
static size_t npf_off;
static volatile artcache_key_t npf_req; /* NP is waiting for this one */
static volatile long npf_keep;      /* a requested copy: not replaced by a
                                       look-ahead before this tick */
static volatile bool npf_fill;      /* review L5: a fill ended; one
                                       look-ahead (audio_next_track()) */

static void npf_post(void)
{
    art_post(ART_Q_NP);             /* one in the queue at most */
}

static int read_bundle(artcache_key_t key, enum artcache_size size,
                       struct bitmap *bm, void *buf, size_t bufsize,
                       size_t *data_off);

/* builder thread: reads what is wanted, if anything */
static void np_serve(void)
{
    artcache_key_t want = npf_req;
    const char *why = "req";
    struct bitmap bm;
    size_t off = 0;
    long t0;
    int res;
    bool disk;

    if (!npf_buf || stopped || usb_owns_disk() || sleep_hold)
        return;
#ifdef SIMULATOR
    if (sim_npf_hold)
        return;                     /* art_npf_hold: the read is late */
#endif
    disk = disk_active();
    if (!want)
    {
        struct mp3entry *id3;

        /* the look-ahead: once per fill's end (review L5), only on
         * storage that is up anyway, and never over a requested copy
         * Now Playing has not taken yet */
        if (!npf_fill)
            return;
        npf_fill = false;
        if (!disk || !(audio_status() & AUDIO_STATUS_PLAY)
            || TIME_BEFORE(current_tick, npf_keep))
            return;
        id3 = audio_next_track();
        want = id3 ? artcache_key_for_id3(id3) : 0;
        why = "next";
        if (!want)
            return;
    }
    if (npf_state == NPF_READY && npf_key == want && npf_seq == commit_seq
        && npf_usb == panel_seq)
    {
        if (npf_req == want)
            npf_req = 0;
        return;                     /* already here */
    }
    npf_state = NPF_BUSY;
    npf_seq = commit_seq;
    npf_usb = panel_seq;
    t0 = current_tick;
    memset(&bm, 0, sizeof(bm));
    res = read_bundle(want, ARTCACHE_NOWPLAYING, &bm, npf_buf,
                      ARTCACHE_NP_ONE_READ, &off);
    disk_touch();
    npf_key = want;
    npf_res = res;
    npf_w = bm.width;
    npf_h = bm.height;
    npf_off = off;
    npf_state = NPF_READY;
    if (npf_req == want)
    {
        npf_req = 0;
        npf_keep = current_tick + 2 * HZ;
    }
    UPDATE_TRACE("art npf key=" KEYFMT " res=%d why=%s disk=%d ms=%ld",
                 KEYARG(want), res, why, (int)disk,
                 (current_tick - t0) * 1000 / HZ);
    (void)why; (void)t0; (void)disk;
}

int artcache_np_take(artcache_key_t key, struct bitmap *bm, void *buf,
                     size_t bufsize)
{
    int res;

    if (!key || !npf_buf || npf_state != NPF_READY || npf_key != key
        || npf_seq != commit_seq || npf_usb != panel_seq)
        return ARTCACHE_NP_NOTYET;
    res = npf_res;
    if (res == ARTCACHE_OK)
    {
        if (bufsize < ARTCACHE_NP_ONE_READ || !bm)
            return ARTCACHE_ERR;
        memcpy(buf, npf_buf, ARTCACHE_NP_ONE_READ);
        memset(bm, 0, sizeof(*bm));
        bm->width = npf_w;
        bm->height = npf_h;
        bm->format = FORMAT_NATIVE;
        bm->data = (unsigned char *)buf + npf_off;
    }
    npf_keep = 0;                   /* taken: a look-ahead may replace it */
    UPDATE_TRACE("art npf take key=" KEYFMT " res=%d", KEYARG(key), res);
    return res;
}

void artcache_note_np(bool shown)
{
    np_seen = shown ? (current_tick ? current_tick : 1) : 0;
    if (!shown)
        park_wake();                /* review L5 */
}

/* The visualizer's overlay (art_bcm_viz_ask(): visualizer.md 8.5): the
 * builder thread reads it, never the UI thread; this wakes its pump (an
 * ART_Q_NP, which a panels.pak copy runs on through), which serves it
 * between its steps (aux_serve) */
void artcache_aux_wake(void)
{
    if (art_thread_id)
        npf_post();
}

/* builder thread: the overlay owed, if any (not while storage is the
 * host's, nor past a power-off) */
static void aux_serve(void)
{
#ifdef HAVE_ART_BCM
    if (!stopped && !usb_owns_disk() && !sleep_hold)
        art_bcm_viz_serve();
#endif
}

void artcache_np_request(artcache_key_t key)
{
    if (!key || !npf_buf || !art_thread_id)
        return;
    npf_req = key;
    npf_post();                     /* again too (review M4): one queued */
}

/* --- UI read block ------------------------------------------------------ */
struct rblock {
    struct artidx_reader rd;
    int pak_fd;
    int seq;                        /* commit_seq when opened */
    bool tried;                     /* opened (or failed) for seq */
    int pseq;                       /* panel_seq when 'ph' was read */
    bool ph_ok;                     /* 'ph' is a valid header */
    const char *panel_path;         /* header and pixels from the same copy */
    long panel_retry;
    bool shown;                     /* #100: 'ph' is panelpool.idx's shown
                                       set (slot >= ARTPOOL_SLOT0: a slot
                                       of panelpool.pak) */
    struct artpanel_hdr ph;         /* panels.pak's header */
    uint64_t win_miss;              /* artcache_panel_window(): this key */
    int win_miss_seq;               /* has no built panel under this
                                       panel_seq (no I/O to ask again) */
    int win_miss_npc;               /* ... and this npc_seq */
};

/* ======================================================================= */
/* keys and sizes                                                          */
/* ======================================================================= */
artcache_key_t artcache_key_for_id3(const struct mp3entry *id3)
{
    if (!id3)
        return 0;
    return artcache_key(id3->albumartist, id3->artist, id3->album, id3->path);
}

void artcache_dim(enum artcache_size size, struct dim *out)
{
    if ((unsigned)size >= ARTCACHE_NUM_SIZES)
        size = ARTCACHE_THUMB;
    out->width = artcache_sizes[size].w;
    out->height = artcache_sizes[size].h;
}

/* --- pacing (p19-art-yield) -------------------------------------------- *
 * The builder ran 68-166 ms between two switches on the device (the slice
 * meter above): the steps between the decoders' own yield points (a
 * pass's table and index work, a panel's compose and save, the pool, the
 * books) ran straight through. ART_PACE() marks a step boundary: once
 * the builder has run ART_PACE_US since its last switch, or with input
 * pending, it yields there, so a woken UI waits for one bounded step at
 * most. Each step keeps its state where it is (its loop index, its open
 * fds, the decoder's state), so nothing is redone and the output is the
 * same. With input pending (a key queued or held, or one within
 * PACE_QUIET) the rest of the step is deferred: the builder sleeps until
 * the input goes quiet, waking for the events a park ends on (USB, a
 * power-off) and serving Now Playing's bundle read (np_serve()) meanwhile;
 * its boost is dropped for the sleep and taken back after (balanced). The
 * BCM paths (art_bcm_read(), the drift jobs) stay as they are: their
 * waits block and switch already.
 *   art pace defer ms=N      a deferral, N ms long, once it ends */
#ifdef SIMULATOR
#define ART_PACE_US 10              /* host us of thread CPU: ~4 ms on the
                                       ARM (SIM_FACTOR 400) */
#else
#define ART_PACE_US 4000
#endif
#define PACE_QUIET  (HZ / 5)

static unsigned long pace_now(void)
{
#ifdef SIMULATOR
    return sim_clock_cpu_us();
#else
    return USEC_TIMER;
#endif
}

static bool pace_input(void)
{
    return button_queue_count() > 0 || button_status()
           || TIME_BEFORE(current_tick, last_input + PACE_QUIET);
}

static void pace_defer(void)
{
    bool fb = FAST_BOOSTED();
    long t0 = current_tick;

    resize_boost_release();
    fast_boost(false);              /* never boosted while it waits */
    while (pace_input() && !stopped && !usb_owns_disk()
           && !queue_peek_ex(&art_queue, NULL, 2, park_release))
    {
        long left = last_input + PACE_QUIET - current_tick;

        if (npf_req)
            np_serve();
        sleep(MAX(left, HZ / 20));
    }
    if (fb)
        fast_boost(true);
    UPDATE_TRACE("art pace defer ms=%ld", (current_tick - t0) * 1000 / HZ);
    (void)t0;
}

/* the simulator's trace names the point by its line; the target passes
 * nothing (one call per point) */
#ifdef SIMULATOR
#define PACE_ARGS   int line
#define ART_PACE()  art_pace_at(__LINE__)
#else
#define PACE_ARGS   void
#define ART_PACE()  art_pace_at()
#endif

static void art_pace_at(PACE_ARGS)
{
    static bool busy;               /* np_serve() in a deferral: none */
    unsigned long start = pace_t0, ran;
    bool input;

    if (busy || thread_self() != art_thread_id)
        return;                     /* a shared helper on another thread */
#ifdef SIMULATOR
    pace_line = line;
#elif defined(HAVE_DEVSTATS)
    /* the core's last switch, when later than the last pace yield */
    if ((long)(devstats_last_us[CURRENT_CORE] - start) > 0)
        start = devstats_last_us[CURRENT_CORE];
#endif
    ran = pace_now() - start;
    if (ran < ART_PACE_US / 4)
        return;                     /* the cheap test, most points */
    input = pace_input();
    if (!input && ran < ART_PACE_US)
        return;
    busy = true;
    yield();
    if (input && pace_input())
        pace_defer();
    busy = false;
    pace_t0 = pace_now();
}

/* artfmt.c's walks (artfmt_pace): their lines from 100000 in the trace */
static void art_pace_fmt(int line)
{
#ifdef SIMULATOR
    art_pace_at(100000 + line);
#else
    (void)line;
    art_pace_at();
#endif
}

/* search_albumart_files()'s probes (albumart_set_probe_hook()): up to ~19
 * path lookups per album; 200000 in the trace */
static void art_pace_probe(void)
{
#ifdef SIMULATOR
    art_pace_at(200000);
#else
    art_pace_at();
#endif
}

/* ======================================================================= */
/* UI side                                                                 */
/* ======================================================================= */
static void usb_event(unsigned short id, void *data)
{
    (void)id; (void)data;
    reader_stale = true;            /* closed by the UI thread, not here */
    panel_seq++;                    /* (npf_buf is stale too) */
}

static void reader_close(struct rblock *r)
{
    artidx_close(&r->rd);
    if (r->pak_fd >= 0)
        close(r->pak_fd);
    r->pak_fd = -1;
}

/* Brings the reader up to date with the last commit. Pinned by caller. */
#define ART_INDEX_IO_TRIES 3     /* reader_sync(): then a rebuild */

static void reader_sync(struct rblock *r)
{
    struct artpak_hdr ph;
    enum artidx_open_result rc, first;

    if (reader_stale || r->seq != commit_seq)
    {
        reader_stale = false;
        reader_close(r);
        r->seq = commit_seq;
        r->tried = false;
    }
    if (r->tried)
        return;
    r->tried = true;
    rc = first = artidx_open(&r->rd, F_INDEX);
    if (rc != ARTIDX_OPENED)
        rc = artidx_open(&r->rd, F_INDEX_PREV);
    if (rc != ARTIDX_OPENED)
    {
        /* both copies unavailable (not invalid): no rebuild request the
         * first times (one per reader retry otherwise); tried again after
         * the next commit or USB. A read that keeps failing is damage a
         * rebuild must replace, so from the third time on it falls
         * through to the request below. Logged once per boot. */
        static unsigned char io_fails;
        if (first == ARTIDX_IO_ERROR && rc == ARTIDX_IO_ERROR
            && io_fails < ART_INDEX_IO_TRIES)
        {
            if (!io_fails++)
                ui_log("art", "index unavailable e=%d", errno);
            return;
        }
        io_fails = 0;
        if (file_exists(F_INDEX))
        {
            UPDATE_TRACE("art io index %s", "invalid");
            artcache_build_request();
        }
        return;
    }
    r->pak_fd = open(F_PAK, O_RDONLY);
    if (r->pak_fd < 0
        || read(r->pak_fd, &ph, sizeof(ph)) != (ssize_t)sizeof(ph)
        || !artpak_hdr_ok(&ph, thumb.w, thumb.h))
    {
        reader_close(r);            /* the builder starts a new pak */
        artcache_build_request();
    }
}

/* A key the UI found PENDING: the builder does it next (run_hints()) */
static void hint_add(artcache_key_t key)
{
    int i;

    if (!key || !art_thread_id)
        return;
    for (i = 0; i < ART_HINTS; i++)
        if (hint_keys[i] == key)
            return;
    for (i = ART_HINTS - 1; i > 0; i--)
        hint_keys[i] = hint_keys[i - 1];
    hint_keys[0] = key;
    hint_seq++;
}

/* Record lookup; *rec valid when it returns ARTCACHE_OK or NOART. */
static enum artcache_result lookup(struct rblock *r, artcache_key_t key,
                                   struct artcache_rec *rec)
{
    bool io_err;

    reader_sync(r);
    if (key == 0)
        return ARTCACHE_NOART;
    if (r->rd.fd < 0)
    {
        hint_add(key);
        return ARTCACHE_PENDING;
    }
    if (!artidx_lookup(&r->rd, key, rec, &io_err))
    {
        if (io_err)
        {
            reader_close(r);        /* e.g. after USB; reopened next time */
            r->tried = false;
            return ARTCACHE_ERR;
        }
        hint_add(key);
        return ARTCACHE_PENDING;
    }
    return rec->state == ARTREC_OK ? ARTCACHE_OK : ARTCACHE_NOART;
}

enum artcache_result artcache_state(artcache_key_t key)
{
    struct artcache_rec rec;
    enum artcache_result res;
    struct rblock *r;

    if (!rblk)
        return key ? ARTCACHE_PENDING : ARTCACHE_NOART;
    r = rblk;
    res = lookup(r, key, &rec);
    return res;
}

static int read_thumb(struct rblock *r, const struct artcache_rec *rec,
                      artcache_key_t key, void *buf)
{
    struct artpak_tag tag;
    size_t n = (size_t)thumb.w * thumb.h * 2;
    long off = artpak_slot_offset(slot_bytes, rec->slot);

    if (r->pak_fd < 0 || lseek(r->pak_fd, off, SEEK_SET) != off
        || read(r->pak_fd, buf, n) != (ssize_t)n
        || read(r->pak_fd, &tag, sizeof(tag)) != (ssize_t)sizeof(tag))
    {
        reader_close(r);
        r->tried = false;
        return ARTCACHE_ERR;
    }
    if (tag.magic != ARTPAK_TAG_MAGIC || tag.key != key
        || (tag.pix_crc && tag.pix_crc != artfmt_crc_end(
                artfmt_crc(ARTFMT_CRC_INIT, buf, n))))
    {
        /* an older index made current again by a cut rename */
        int i;

        UPDATE_TRACE("art repair slot=%lu key=" KEYFMT,
                     (unsigned long)rec->slot, KEYARG(key));
        for (i = 0; i < ART_HINTS && repair_keys[i] != key; i++)
            ;
        if (i == ART_HINTS)
        {
            for (i = ART_HINTS - 1; i > 0; i--)
                repair_keys[i] = repair_keys[i - 1];
            repair_keys[0] = key;
        }
        hint_add(key);
        artcache_build_request();
        return ARTCACHE_PENDING;
    }
    return ARTCACHE_OK;
}

/* "<dir>/XX/XXXXXXXX.ART" for 'key' into path (ART_BPATH bytes) */
#define ART_BPATH (sizeof(ARTCACHE_DIR) + 18)
static void bundle_path(artcache_key_t key, char *path)
{
    memcpy(path, ARTCACHE_DIR "/", sizeof(ARTCACHE_DIR));
    artbundle_name(key, path + sizeof(ARTCACHE_DIR));
}

/* Bundles (Phase 5: NOWPLAYING; COVERFLOW is Phase 7). The image is the
 * bundle's own w x h (a fit size's is smaller than its box on one side,
 * D10). A record that is OK whose bundle is missing (a name collision
 * overwrote it, or a cut write) answers PENDING. */
static int read_bundle(artcache_key_t key, enum artcache_size size,
                       struct bitmap *bm, void *buf, size_t bufsize,
                       size_t *data_off)
{
    char path[ART_BPATH];
    struct artcache_dim box = artcache_sizes[size];
    uint16_t w, h;
    int rc;

    bundle_path(key, path);
    *data_off = 0;
    if (size == ARTCACHE_NOWPLAYING && bufsize >= ARTCACHE_NP_ONE_READ)
    {
        /* QW2: header and pixels in one read(), either version */
        struct artbundle_img im;

        rc = artbundle_read_one(path, key, size, buf, bufsize, &im);
        if (rc <= 0)
            return rc < 0 ? ARTCACHE_ERR : ARTCACHE_PENDING;
        if (im.format == ARTBUNDLE_FMT_NPCOMPOSED)
        {
            if (im.w != NP_COMPOSED_W || im.h != NP_COMPOSED_H
                || im.reserved != NP_LAYOUT_HASH)
                return ARTCACHE_PENDING;    /* another layout: the builder
                                               composes it again */
        }
        else if (im.w > box.w || im.h > box.h)
            return ARTCACHE_ERR;
        bm->width = im.w;
        bm->height = im.h;
        *data_off = ARTBUNDLE_HDR_BYTES;
        return ARTCACHE_OK;
    }
    rc = artbundle_read(path, key, size, box.w, box.h, buf, bufsize, &w, &h);
    if (rc <= 0)
        return rc < 0 ? ARTCACHE_ERR : ARTCACHE_PENDING;
    bm->width = w;
    bm->height = h;
    return ARTCACHE_OK;
}

int artcache_get(artcache_key_t key, enum artcache_size size,
                 struct bitmap *bm, void *buf, size_t bufsize)
{
    struct artcache_rec rec;
    struct rblock *r;
    size_t off = 0;
    int res;
    bool bundle_recovered = false;

    if ((unsigned)size >= ARTCACHE_NUM_SIZES || !bm || !buf)
        return ARTCACHE_ERR;
    if (!rblk)
        return key ? ARTCACHE_PENDING : ARTCACHE_NOART;
    r = rblk;
    res = lookup(r, key, &rec);
    /* Bundles have their own key/header validation. A missing/torn index
     * must not hide a perfectly usable Now Playing cover. */
    if (res != ARTCACHE_OK && key && size == ARTCACHE_NOWPLAYING)
    {
        res = read_bundle(key, size, bm, buf, bufsize, &off);
        bundle_recovered = res == ARTCACHE_OK;
    }
    if (res == ARTCACHE_OK)
    {
        if (bundle_recovered)          /* independent bundle recovery */
            ;
        else if (size != ARTCACHE_THUMB) /* a chapter: its group's bundle */
            res = read_bundle(artrec_bundle_key(&rec), size, bm, buf,
                              bufsize, &off);
        else if (artrec_is_chapter(&rec))
            res = ARTCACHE_NOART;       /* Now Playing size only */
        else if (bufsize < (size_t)thumb.w * thumb.h * 2)
            res = ARTCACHE_ERR;
        else if ((res = read_thumb(r, &rec, key, buf)) == ARTCACHE_OK)
        {
            bm->width = thumb.w;
            bm->height = thumb.h;
        }
    }
    if (res == ARTCACHE_OK)
    {
        int bw = bm->width, bh = bm->height;

        memset(bm, 0, sizeof(*bm));
        bm->width = bw;
        bm->height = bh;
        bm->format = FORMAT_NATIVE;
        bm->data = (unsigned char *)buf + off;
    }
    UPDATE_TRACE("art get key=" KEYFMT " size=%d res=%d", KEYARG(key),
                 (int)size, res);
    return res;
}

int artcache_get_handle(artcache_key_t key, enum artcache_size size,
                        enum artcache_result *res)
{
    struct artcache_dim d;
    struct bitmap bm;
    void *p;
    int h, rc;

    if ((unsigned)size >= ARTCACHE_NUM_SIZES)
    {
        *res = ARTCACHE_ERR;
        return 0;
    }
    rc = artcache_state(key);
    if (rc != ARTCACHE_OK)
    {
        *res = rc;
        return 0;
    }
    if (audio_status())             /* never shrink the audio buffer */
    {
        *res = ARTCACHE_ERR;
        return 0;
    }
    d = artcache_sizes[size];
    h = core_alloc((size_t)d.w * d.h * 2);
    if (h <= 0)
    {
        *res = ARTCACHE_ERR;
        return h;
    }
    p = core_get_data_pinned(h);
    rc = artcache_get(key, size, &bm, p, (size_t)d.w * d.h * 2);
    core_put_data_pinned(p);
    *res = rc;
    if (rc != ARTCACHE_OK)
    {
        core_free(h);
        return 0;
    }
    return h;
}

/* --- panel art (H1) ----------------------------------------------------- */
/* panels.pak's header, re-read after every change (panel_seq). Pinned by
 * the caller. */
/* 1 read and valid, 0 no such file (nothing built there), -1 neither
 * (an I/O error or a torn or foreign header: not proof of absence) */
static int panel_header_ex(const char *path, struct artpanel_hdr *h)
{
    int fd = open(path, O_RDONLY);
    bool ok;

    if (fd < 0)
        return errno == ENOENT ? 0 : -1;
    ok = read(fd, h, sizeof(*h)) == (ssize_t)sizeof(*h)
         && artpanel_hdr_ok(h, panel.w, panel.h)
         && ffilesize(fd) >= (off_t)artpanel_slot_offset(panel_slot_bytes,
                                                        h->n_slots);
    return close(fd) == 0 && ok ? 1 : -1;
}

static bool panel_header(const char *path, struct artpanel_hdr *h)
{
    return panel_header_ex(path, h) > 0;
}

/* #100: the shown set (panelpool.idx's first 1 KB), or its previous copy
 * while a replace is between its renames */
static bool panel_shown(struct artpanel_hdr *h)
{
    static const char *const names[] = { F_POOLX, F_POOLX_PREV };
    unsigned i;

    for (i = 0; i < ARRAYLEN(names); i++)
    {
        int fd = open(names[i], O_RDONLY);
        bool ok;

        if (fd < 0)
            continue;
        ok = read(fd, h, sizeof(*h)) == (ssize_t)sizeof(*h)
             && artshown_ok(h, panel.w, panel.h);
        if (close(fd) == 0 && ok)
            return true;
    }
    return false;
}

/* The file and the slot entry e's pixels are in */
static const char *panel_file(const struct rblock *r,
                              const struct artpanel_ent *e,
                              struct artpanel_ent *at)
{
    *at = *e;
    if (r->shown && artshown_pool(e))
    {
        at->slot -= ARTPOOL_SLOT0;
        return F_POOL;
    }
    return r->panel_path;
}

static void panel_sync(struct rblock *r)
{
    struct artpanel_hdr h;
    int seq = panel_seq;
    const char *path = F_PANELS;
    bool previous = false;

    if (r->pseq == seq && r->ph_ok && !r->panel_retry)
        return;
    if (r->pseq == seq && r->panel_retry
        && TIME_BEFORE(current_tick, r->panel_retry))
        return;
    if (panel_shown(&h))
    {
        r->ph = h;
        r->panel_path = F_PANELS;
        r->ph_ok = true;
        r->shown = true;
        r->pseq = seq;
        r->panel_retry = 0;
        return;
    }
    r->shown = false;
    if (!panel_header(path, &h))
    {
        path = F_PANELS_PREV;
        previous = true;
        if (!panel_header(path, &h))
        {
            /* Do not poison a previously usable header on transient I/O. */
            r->pseq = seq;
            r->panel_retry = current_tick + HZ;
            return;
        }
    }
    r->ph = h;
    r->panel_path = path;
    r->ph_ok = true;
    r->pseq = seq;
    r->panel_retry = previous ? current_tick + HZ : 0;
}

/* The set of a panels.pak entry (#58): the entry's first pad byte, 0
 * for the music set (every pak written before #58), 1 for a book. Only
 * this file writes and reads it; it is inside the header's CRC. */
#define PENT_SET(e) ((e)->pad[0])
/* ... of a panel set album (library.c puts books in the set, #58) */
#ifdef HAVE_BCMFW_VIDEO
/* Phase 14a: and 2 for a video (movie or TV show; podcast show art, 8b,
 * adds its case here) */
#define PSET_OF(t)  (((t)->flags & LIBFILE_ALB_VIDEO) ? ARTCACHE_PSET_VIDEOS \
                     : ((t)->flags & LIBFILE_ALB_BOOK) ? ARTCACHE_PSET_BOOKS \
                                                       : ARTCACHE_PSET_MUSIC)
#else
#define PSET_OF(t)  (((t)->flags & LIBFILE_ALB_BOOK) ? ARTCACHE_PSET_BOOKS \
                                                     : ARTCACHE_PSET_MUSIC)
#endif

/* Entry of the i-th panel of 'set' with art, or -1 */
static int panel_entry(const struct rblock *r, int set, int i)
{
    int k;

    if (!r->ph_ok)
        return -1;
    for (k = 0; k < ARTPANEL_SLOTS; k++)
        if (r->ph.ent[k].state == ARTREC_OK && PENT_SET(&r->ph.ent[k]) == set
            && i-- == 0)
            return k;
    return -1;
}

int artcache_panel_keys(int set, uint64_t *keys, int max)
{
    struct rblock *r;
    int n = 0, k;

    if (!rblk)
        return 0;
    r = rblk;
    panel_sync(r);
    while ((k = panel_entry(r, set, n)) >= 0)
    {
        if (keys && n < max)
            keys[n] = r->ph.ent[k].key;
        n++;
    }
    return keys ? MIN(n, max) : n;
}

int artcache_panel_count(int set)
{
    return artcache_panel_keys(set, NULL, 0);
}

int artcache_panel_seq(void)
{
    return panel_seq;
}

#ifdef HAVE_BCMFW_VIDEO
int artcache_panel_get(int set, int i, struct bitmap *bm, void *buf,
                       size_t size, uint64_t *key)
{
    return artcache_panel_get_cw(set, i, bm, buf, size, key, NULL);
}

int artcache_panel_get_cw(int set, int i, struct bitmap *bm, void *buf,
                          size_t size, uint64_t *key, int *cw)
#else
int artcache_panel_get(int set, int i, struct bitmap *bm, void *buf,
                       size_t size, uint64_t *key)
#endif
{
    struct artpanel_ent e;
    struct rblock *r;
    size_t n = (size_t)panel.w * panel.h * sizeof(fb_data);
    int k, res = ARTCACHE_ERR;

    if (!rblk || !bm || !buf || size < n || i < 0)
        return ARTCACHE_ERR;
    r = rblk;
    panel_sync(r);
    k = panel_entry(r, set, i);
    if (k < 0)
        return ARTCACHE_ERR;
    e = r->ph.ent[k];
    /* the read yields: a rewrite of this slot (the same album, a new
     * source) may begin meanwhile; panel_seq tells (artpanel_read()) */
    {
        struct artpanel_ent at;
        const char *path = panel_file(r, &e, &at);

        if (artpanel_read(path, panel_slot_bytes, &at, buf, n, &panel_seq,
                          r->pseq))
            res = ARTCACHE_OK;
    }
    if (res != ARTCACHE_OK)
    {
        struct artpanel_hdr h;
        int j;

        r->panel_retry = current_tick + HZ;
        if (panel_header(F_PANELS_PREV, &h))
            for (j = 0; j < ARTPANEL_SLOTS; j++)
                if (h.ent[j].key == e.key && h.ent[j].state == ARTREC_OK
                    && artpanel_read(F_PANELS_PREV, panel_slot_bytes,
                                     &h.ent[j], buf, n, &panel_seq, r->pseq))
                {
                    e = h.ent[j];
                    r->ph = h;
                    r->shown = false;
                    r->panel_path = F_PANELS_PREV;
                    res = ARTCACHE_OK;
                    break;
                }
    }
    if (res == ARTCACHE_OK)
    {
        memset(bm, 0, sizeof(*bm));
        bm->width = panel.w;
        bm->height = panel.h;
        bm->format = FORMAT_NATIVE;
        bm->data = buf;
        if (key)
            *key = e.key;
#ifdef HAVE_BCMFW_VIDEO
        if (cw)
            *cw = ARTPANEL_CW(&e);
#endif
    }
    UPDATE_TRACE("art panel get set=%d i=%d key=" KEYFMT " res=%d", set, i,
                 KEYARG(e.key), res);
    return res;
}

/* The main menu's Now Playing row (np-art-quality): a window of 'key''s
 * 300 x 300 panel from wherever one is built though the shown set does
 * not name it: panels.pak's default set (behind a shown set, #100), else
 * panelpool.pak (its table in panelpool.idx, after the shown set's 1 KB).
 * The scratch takes that table, then the window's rows in runs (one read
 * per run; 103 KB of scratch: two), copied 'w' columns from x0 into dst;
 * then the tag, which must name 'key' with panel_seq unchanged (an
 * in-place rewrite zeroes the tag first, #117). A key with none is
 * remembered until panel_seq moves (asked again: no I/O), but only when
 * every lookup was conclusive: its table read and valid, or its file
 * absent. A failed or torn read is not "none built" (panel_sync()'s rule:
 * never poison on transient I/O), so it is asked again next time.
 * np-sharp: then npcover.pak (the playing album's own render, queued by
 * the row), under npc_seq as the others are under panel_seq; the memo
 * keeps both.
 * Traces: art panel window key=K from=default|pool|npcover|none|memo
 * res=R */
#ifdef SIMULATOR
/* test knobs (ipodui-np-artq-*): IPODUI_NP_NO_PANEL=pool, the default
 * set's lookup misses too (only the pool can answer); IPODUI_NP_WIN_RACE
 * set, the first window read that finds a panel sees panel_seq move
 * after its rows (a rewrite during the read) */
static bool win_knob(const char *name, const char *val)
{
    const char *v = getenv(name);

    return v && (!val || !strcmp(v, val));
}
/* "pool": only the pool answers; "built" (ipodui-np-sharp-*): only
 * npcover.pak answers, here and in the builder's look (npr_have()) */
#define WIN_NO_DEFAULT()    (win_knob("IPODUI_NP_NO_PANEL", "pool") \
                             || win_knob("IPODUI_NP_NO_PANEL", "built"))
#define WIN_NO_POOL()       win_knob("IPODUI_NP_NO_PANEL", "built")
#define WIN_NO_NPC()        win_knob("IPODUI_NP_NO_PANEL", "pool")
#else
#define WIN_NO_DEFAULT()    false
#define WIN_NO_POOL()       false
#define WIN_NO_NPC()        false
#endif

/* npcover.pak's header: 1 read and valid, 0 none (no file, or a front
 * not yet written: nothing rendered), -1 neither */
static int npc_header_ex(struct artpanel_hdr *h)
{
    int fd = open(F_NPCOVER, O_RDONLY);
    int got;

    if (fd < 0)
        return errno == ENOENT ? 0 : -1;
    if (read(fd, h, sizeof(*h)) != (ssize_t)sizeof(*h))
        got = -1;
    else if (h->magic == 0)
        got = 0;                    /* a zero front: nothing rendered */
    else if (artnpc_ok(h, panel.w, panel.h)
             && ffilesize(fd) >= (off_t)artpanel_slot_offset(
                                     panel_slot_bytes, h->n_slots))
        got = 1;
    else
        got = -1;
    return close(fd) == 0 ? got : -1;
}

int artcache_panel_window(uint64_t key, int x0, int y0, int w, int h,
                          void *dst, int stride, void *scratch,
                          size_t size)
{
    const size_t row = (size_t)panel.w * sizeof(fb_data);
    const off_t pool_at = offsetof(struct artpool_idx, pool);
    struct artpanel_ent e;
    struct artpak_tag tag;
    struct rblock *r;
    const char *path = NULL, *from = "none";
    char *sb;
    off_t off;
    int k, fd, y, seq, nseq, rows;
    bool ok, sure = true;           /* every lookup conclusive */

    if (!rblk || !key || !dst || !scratch || x0 < 0 || y0 < 0 || w <= 0
        || h <= 0 || x0 + w > panel.w || y0 + h > panel.h)
        return ARTCACHE_ERR;
    sb = (char *)ALIGN_UP((uintptr_t)scratch, 8);
    if (size < (size_t)(sb - (char *)scratch)
               + MAX(MAX(row, sizeof(struct artpool_hdr)),
                     sizeof(struct artpanel_hdr)))
        return ARTCACHE_ERR;
    size -= sb - (char *)scratch;
    r = rblk;
    panel_sync(r);
    seq = panel_seq;
    nseq = npc_seq;
    if (r->win_miss == key && r->win_miss_seq == seq
        && r->win_miss_npc == nseq)
    {
        UPDATE_TRACE("art panel window key=" KEYFMT " from=memo res=%d",
                     KEYARG(key), ARTCACHE_PENDING);
        return ARTCACHE_PENDING;
    }
    memset(&e, 0, sizeof(e));
    /* 1: the default set (behind a shown set, or with no header in RAM:
     * its header read; else the header in RAM, which the caller searched
     * by set already) */
    if (!WIN_NO_DEFAULT())
    {
        struct artpanel_hdr *ph = (struct artpanel_hdr *)sb;
        int got;

        if (!r->shown && r->ph_ok)
        {
            memcpy(ph, &r->ph, sizeof(*ph));
            got = 1;
        }
        else
            got = panel_header_ex(F_PANELS, ph);
        if (got < 0)
            sure = false;
        for (k = 0; got > 0 && k < ARTPANEL_SLOTS; k++)
            if (ph->ent[k].key == key && ph->ent[k].state == ARTREC_OK)
            {
                e = ph->ent[k];
                path = (!r->shown && r->ph_ok) ? r->panel_path : F_PANELS;
                from = "default";
                break;
            }
    }
    /* 2: the pool */
    if (!path && !WIN_NO_POOL())
    {
        struct artpool_hdr *p = (struct artpool_hdr *)sb;

        fd = open(F_POOLX, O_RDONLY);
        ok = false;
        if (fd < 0)
            sure = sure && errno == ENOENT;
        else
        {
            ok = lseek(fd, pool_at, SEEK_SET) == pool_at
                 && read(fd, p, sizeof(*p)) == (ssize_t)sizeof(*p)
                 && artpool_ok(p);
            ok = close(fd) == 0 && ok;
            sure = sure && ok;
        }
        for (k = 0; ok && k < ARTPOOL_ENTS; k++)
            if (p->ent[k].key == key && p->ent[k].state == ARTREC_OK)
            {
                e = p->ent[k];
                path = F_POOL;
                from = "pool";
                break;
            }
    }
    /* 3: npcover.pak (np-sharp) */
    if (!path && !WIN_NO_NPC())
    {
        struct artpanel_hdr *ph = (struct artpanel_hdr *)sb;
        int got = npc_header_ex(ph);

        if (got < 0)
            sure = false;
        k = got > 0 ? artnpc_find(ph, key) : -1;
        if (k >= 0)
        {
            e = ph->ent[k];
            path = F_NPCOVER;
            from = "npcover";
        }
    }
    if (!path || panel_seq != seq || npc_seq != nseq)
    {
        if (!path && sure && panel_seq == seq && npc_seq == nseq)
        {
            r->win_miss = key;
            r->win_miss_seq = seq;
            r->win_miss_npc = nseq;
        }
        UPDATE_TRACE("art panel window key=" KEYFMT " from=%s res=%d "
                     "sure=%d", KEYARG(key), path ? from : "none",
                     ARTCACHE_PENDING, (int)sure);
        return ARTCACHE_PENDING;
    }
    /* 3: the window, then the tag */
    off = artpanel_slot_offset(panel_slot_bytes, e.slot);
    rows = (int)(size / row);
    fd = open(path, O_RDONLY);
    ok = fd >= 0;
    for (y = 0; ok && y < h; y += rows)
    {
        int n = MIN(rows, h - y), i;
        off_t at = off + (off_t)(y0 + y) * row;

        ok = lseek(fd, at, SEEK_SET) == at
             && read(fd, sb, n * row) == (ssize_t)(n * row);
        for (i = 0; ok && i < n; i++)
            memcpy((fb_data *)dst + (y + i) * stride,
                   (fb_data *)(sb + i * row) + x0,
                   w * sizeof(fb_data));
    }
#ifdef SIMULATOR
    {
        static bool raced;

        if (ok && !raced && win_knob("IPODUI_NP_WIN_RACE", NULL))
        {
            raced = true;
            panel_seq++;            /* as a rewrite starting meanwhile */
            UPDATE_TRACE("art panel window race seq=%d", panel_seq);
        }
    }
#endif
    sure = false;
    if (ok)
    {
        off_t at = off + (off_t)panel.h * row;

        ok = lseek(fd, at, SEEK_SET) == at
             && read(fd, &tag, sizeof(tag)) == (ssize_t)sizeof(tag);
        /* review M1: a whole read whose tag names another album (or
         * none) under unchanged sequences is as sure as a miss */
        sure = ok;
        ok = ok && tag.magic == ARTPAK_TAG_MAGIC && tag.key == key;
    }
    if (fd >= 0)
        close(fd);
    if (!ok && sure && !strcmp(from, "npcover") && panel_seq == seq
        && npc_seq == nseq)
    {
        r->win_miss = key;
        r->win_miss_seq = seq;
        r->win_miss_npc = nseq;
    }
    ok = ok && panel_seq == seq && npc_seq == nseq;
    UPDATE_TRACE("art panel window key=" KEYFMT " from=%s res=%d",
                 KEYARG(key), from, ok ? ARTCACHE_OK : ARTCACHE_ERR);
    (void)from;                     /* trace only */
    return ok ? ARTCACHE_OK : ARTCACHE_ERR;
}

/* #92: up to n covers of 'set' from its i0-th (set order, wrapping), the
 * cover keyed 'skip' left out, into buf at a stride of
 * ARTCACHE_PANEL_SLOT_BYTES (a slot of panels.pak: pixels, tag, pad).
 * Covers in consecutive slots (a grouped file, artpanel_grouped()) are
 * one read; each run of them is one. */
int artcache_panel_run(int set, int i0, int n, uint64_t skip, void *buf,
                       size_t size, uint64_t *keys, int *cw, int *idx)
{
    const size_t npix = (size_t)panel.w * panel.h * sizeof(fb_data);
    int ent[ARTCACHE_PANEL_RUN_MAX], at[ARTCACHE_PANEL_RUN_MAX];
    struct rblock *r;
    int count, m = 0, i, j, reads = 0, got = 0;

    if (!rblk || !buf || n <= 0 || n > ARTCACHE_PANEL_RUN_MAX || i0 < 0
        || panel_slot_bytes != ARTCACHE_PANEL_SLOT_BYTES
        || size < (size_t)n * ARTCACHE_PANEL_SLOT_BYTES)
        return -1;
    r = rblk;
    panel_sync(r);
    count = artcache_panel_keys(set, NULL, 0);
    for (i = 0; i < count && m < n; i++)
    {
        int k = panel_entry(r, set, (i0 + i) % count);

        if (k >= 0 && r->ph.ent[k].key != skip)
        {
            at[m] = (i0 + i) % count;
            ent[m++] = k;
        }
    }
    for (i = 0; i < m; i = j)
    {
        struct artpanel_ent pe;
        const char *path = panel_file(r, &r->ph.ent[ent[i]], &pe);
        off_t off = artpanel_slot_offset(panel_slot_bytes, pe.slot);
        size_t bytes;
        int fd, l;
        bool ok;

        /* a run: consecutive slots of one file (#100: the shown set
         * mixes panels.pak and panelpool.pak) */
        for (j = i + 1; j < m && artpanel_run(&r->ph, ent + i, j - i + 1)
                        && artshown_pool(&r->ph.ent[ent[j]])
                           == artshown_pool(&r->ph.ent[ent[i]]);
             j++)
            ;
        bytes = (size_t)(j - i) * panel_slot_bytes;
        fd = open(path, O_RDONLY);
        if (fd < 0)
            break;
        ok = lseek(fd, off, SEEK_SET) == off
             && read(fd, (char *)buf + (size_t)got * panel_slot_bytes, bytes)
                == (ssize_t)bytes;
        close(fd);
        reads++;
        if (!ok || panel_seq != r->pseq)
            break;                  /* a rewrite began: keep what came */
        for (l = i; l < j; l++)
        {
            const struct artpak_tag *t = (const struct artpak_tag *)
                ((const char *)buf + (size_t)got * panel_slot_bytes + npix);

            if (t->magic != ARTPAK_TAG_MAGIC || t->key != r->ph.ent[ent[l]].key)
            {
                /* #100 review L1: a shown set behind panels.pak (its
                 * publish refused): look again soon */
                r->panel_retry = current_tick + HZ;
                break;
            }
            if (keys)
                keys[got] = t->key;
            if (idx)
                idx[got] = at[l];
            if (cw)
#ifdef HAVE_BCMFW_VIDEO
                cw[got] = ARTPANEL_CW(&r->ph.ent[ent[l]]);
#else
                cw[got] = 0;
#endif
            got++;
        }
        if (l < j)
            break;
    }
    (void)reads;                    /* trace only */
    UPDATE_TRACE("art panel run set=%d i0=%d want=%d got=%d reads=%d "
                 "grouped=%d", set, i0, n, got, reads,
                 (int)(r->ph_ok && artpanel_grouped(&r->ph)));
    return got;
}

/* ======================================================================= */
/* builder: control                                                        */
/* ======================================================================= */
/* Called from the tagcache thread (lib_committed()) as well as the UI:
 * queue_post() is the only cross-thread operation. A request before the
 * thread exists (the boot load can finish before the shell reserves) is
 * remembered and posted by artcache_reserve(). */
void artcache_build_request(void)
{
    want_build = true;
    if (art_thread_id)
        art_post(ART_Q_BUILD);
}

void artcache_build_maintenance_request(void)
{
    maintenance = true;
    artcache_build_request();
}

bool artcache_maintenance_pending(void)
{
    return maintenance;
}

void artcache_build_pause(bool pause)
{
    if (pause)
        pause_count++;
    else if (pause_count > 0)
        pause_count--;
}

void artcache_build_status(struct artcache_build_status *st)
{
    *st = status;
}

void artcache_note_input(void)
{
    last_input = current_tick;
    /* P19 19-1a: lit, the tagcache thread polls once a second (its
     * edges: user idle, the paused window, np-sharp's place); a key
     * that lights the screen ends a dark wait. With the backlight "Off"
     * no kick: a wheel turn would post a pass per click */
    if (is_backlight_on(true))
        tagcache_quiet_kick();
}

/* UI thread: the user went idle (the backlight went off, the face came
 * up). Wakes a builder waiting for it; no post otherwise, so a finished
 * builder sleeps through it. */
void artcache_note_idle(void)
{
    tagcache_quiet_kick();          /* P19 19-1a: the windows' idle edge */
    if (art_thread_id && (idle_wait || chapters_owed || books_owed
                          || (npr_key && npr_place())))
        art_post(ART_Q_IDLE);
    park_wake();                    /* review L5 */
}

void artcache_usb_prepare(void)
{
    usb_quiescing = true;
    park_wake();                    /* review L5 */
}

void artcache_sleep_hold(bool hold)
{
    if (hold == sleep_hold)
        return;
    sleep_hold = hold;
    artcache_build_pause(hold);     /* blocked(): "paused", unboosted */
    if (hold)
        park_wake();                /* review L5 */
}

void artcache_usb_finish(void)
{
    usb_quiescing = false;
}

/* ======================================================================= */
/* builder: waiting                                                        */
/* ======================================================================= */
enum { EV_REQ = 1, EV_USB = 2, EV_OFF = 4, EV_RETRY = 8 };

/* buffering thread, a fill's end: the Now Playing look-ahead read
 * (np_serve(), storage still up). In the sim also its model of
 * storage_sleep() after a fill (the Now Playing art read keys on
 * artcache_disk_awake()). The builder itself no longer listens for
 * fills (#58 idle build: no fill windows). */
static void fill_event(unsigned short id, void *data)
{
    (void)id; (void)data;
#ifdef SIMULATOR
    sim_disk_active_until = current_tick + HZ / 5;
    if (!sim_disk_active_until)
        sim_disk_active_until = 1;
#endif
    if (art_thread_id && npf_buf)
    {
        npf_fill = true;
        npf_post();
    }
}

/* Playback stopped or paused (audio thread): the work kept for the
 * stop can run, or the builder waiting for an idle user may run now; the
 * thread blocks until then instead of polling. No post otherwise. */
static void idle_event(unsigned short id, void *data)
{
    (void)id; (void)data;
    if (art_thread_id && (gc_owed || chapters_owed || books_owed
                          || chap_later || idle_wait
                          || ((regroup_owed || pool_owed) && regroup_window())
                          || sig_due() || (npr_key && npr_place())))
        art_post(ART_Q_IDLE);
    park_wake();                    /* review L5: a pause or stop */
}

/* #88: the paused window opened with books owed (maint_poll()) */
void artcache_maint_kick(void)
{
    idle_event(0, NULL);
}

/* #92 review M2: maint_poll() (once a second) while stopped: the
 * stopped-and-idle window (regroup_window()) opening with work owed
 * wakes the builder, once per opening (a post mid-copy would stop it) */
void artcache_maint_poll_stopped(void)
{
    static bool was;
    bool now = art_thread_id
               && (((regroup_owed || pool_owed) && regroup_window()) || sig_due());

    if (now && !was)
        art_post(ART_Q_IDLE);
    was = now;
}

/* #92/#90: maintenance the builder owes a window (the regroup) */
bool artcache_maint_owed(void)
{
    return regroup_owed || sig_owed || pool_owed;
}

/* #100: maint_poll() (once a second, any audio state): a rotation due
 * while storage is awake wakes the builder once per wake (it rotates
 * then; it never wakes storage for it). P18 Q8: the sleep-entry window
 * is the one other place it runs, storage awake or not. */
static bool rot_due(void);
static bool rot_place(void)
{
    return !sleep_hold
           && (artcache_disk_awake() || maint_sleep_window_open());
}

/* owed only when the builder would run it now (no pass pending, no retry
 * backoff): otherwise the sleep window would wait its whole cap */
bool artcache_rotate_owed(void)
{
    return art_thread_id && rot_ready && !rot_no_pool && rot_due();
}

/* np-sharp: an album played NPR_PLAYED_MS queues its Now Playing cover
 * (once per album: a run of shuffled skips queues none); a queued render
 * whose place opens wakes the builder, once per opening */
static artcache_key_t npr_played;     /* tagcache thread only */

static long npr_played_ms(void)
{
    long at = NPR_PLAYED_MS;
#ifdef SIMULATOR
    const char *v = getenv("IPODUI_NPR_PLAYED_MS");

    if (v)
        at = atol(v);
#endif
    return at;
}

/* P19 19-1a: playing or paused, the ms of the current track still to play before
 * its album queues (0: queue it now); -1: nothing to queue */
static long npr_played_left(artcache_key_t *keyp)
{
    struct mp3entry *id3;
    artcache_key_t key;
    long left;

    if (!art_thread_id || !(audio_status() & AUDIO_STATUS_PLAY))
        return -1;
    id3 = audio_current_track();
    if (!id3)
        return -1;
    key = np_pb_cover_key();
    if (!key || key == npr_played)
        return -1;
    left = npr_played_ms() - (long)id3->elapsed;
    *keyp = key;
    return left > 0 ? left : 0;
}

/* P19 19-1a: the tagcache thread's dark wait ends when the current
 * album has played long enough to queue its cover (npr_poll() runs at
 * that pass); at a faster speed it wakes again, a little later. Only on
 * the charger, the one place a playing render may run (npr_place()): in
 * dark battery playback no wake of its own, the album queues at the
 * thread's next pass (its cap, a disk wake, a key) */
bool artcache_poll_due(long *at)
{
    artcache_key_t key;
    long left;

    if ((audio_status() & AUDIO_STATUS_PAUSE) || !charger_inserted())
        return false;           /* the play length stands still, or no
                                   render could follow it */
    left = npr_played_left(&key);
    if (left <= 0)
        return false;           /* none, or due at this pass already */
    *at = current_tick + (left * HZ + 999) / 1000;
    return true;
}

/* P19 19-1a: a new track restarts the play length: on the charger the
 * tagcache thread's dark wait is cut so artcache_poll_due() sees it (on
 * battery no wake: no render could follow) */
static void npr_track_event(unsigned short id, void *data)
{
    (void)id; (void)data;
    if (charger_inserted())
        tagcache_quiet_kick();
}

static void npr_poll(void)
{
    static bool was;
    artcache_key_t key;
    bool now;

    if (npr_played_left(&key) == 0)
    {
        npr_played = key;
        artcache_npcover_queue(key, ARTCACHE_NPR_PLAYED);
    }
    now = npr_key && npr_place() && !blocked();
    if (now && !was)
        art_post(ART_Q_IDLE);
    was = now;
}

void artcache_rotate_poll(void)
{
    static bool was;
    bool now = art_thread_id && rot_place() && rot_due();

    if (now && !was)
        art_post(ART_Q_IDLE);
    was = now;
    if (art_thread_id)
        npr_poll();
}

/* Waits up to 'ticks' for one event and handles it. */
static int pump(int ticks)
{
    struct queue_event ev;

    hang_beat(HANG_ART);            /* each I/O and each wait: progress */
    if (npf_req)
        np_serve();
    aux_serve();
    if (park_held)
    {
        ev = park_ev;               /* what ended a park (review M1) */
        park_held = false;
    }
    else if (park_fold)
    {
        int id = (park_fold & (1u << ART_Q_BUILD)) ? ART_Q_BUILD
                                                   : ART_Q_IDLE;

        park_fold &= ~(1u << id);   /* what a park took, in turn */
        ev.id = id;
        ev.data = 0;
    }
    else
    {
        int hw = hang_idle_begin();         /* waiting for work: idle */
        queue_wait_w_tmo(&art_queue, &ev, ticks);
        hang_idle_end(hw);
        art_taken(ev.id);
    }
    switch (ev.id)
    {
    case ART_Q_BUILD:
        if (status.running && bw->db_gen == tagcache_ui_generation()
            && build_fast == maintenance)
            return 0;               /* coalesced: finish this valid pass */
        return EV_REQ;
    case ART_Q_IDLE:
        idle_seen = true;           /* re-review L-a: seen outside a pass */
        return 0;                   /* look again (gc_owed, idle, ...) */
    case ART_Q_NP:
        np_serve();
        aux_serve();
        return 0;
    case SYS_USB_CONNECTED:
        artidx_close(&bw->rd);      /* the only fd held across a wait */
        UPDATE_TRACE("art build stop %s", "usb");
        fast_boost(false);          /* never boosted through a USB session */
        usb_acknowledge(SYS_USB_CONNECTED_ACK, ev.data);
        {
            int hw = hang_idle_begin();     /* the host's: idle */
            usb_wait_for_disconnect(&art_queue);
            hang_idle_end(hw);
        }
        for (int i = 0; i < ART_Q_N; i++)
            q_posted[i] = false;    /* the wait may have taken them */
        usb_quiescing = false;
        bw->pidx_ok = false;        /* #100: the host may have changed it */
        memset(npr_fail, 0, sizeof(npr_fail));  /* np-sharp: or the art */
        /* review L1: the host may have replaced a cover: the Now Playing
         * renders go (rendered again when queued), one remove on the
         * way out of the session */
        if (remove(F_NPCOVER) == 0)
            UPDATE_TRACE("art npcover reset why=%s", "usb");
        npc_seq += 2;
        /* Resume an unfinished pass after the host releases storage, but do
         * not turn a USB session into a cache-wide source revalidation.
         * The host may alter art; an explicit library/art rebuild is the
         * deliberate way to request that expensive refresh. */
        return EV_USB;
    case SYS_POWEROFF:
    case SYS_REBOOT:
        artidx_close(&bw->rd);
        stopped = true;
        stopped_tick = current_tick;
        return EV_OFF;
    }
    return 0;                       /* timeout */
}

static const char *blocked(void)
{
    int batt;

    if (stopped)
        return "off";
    if (usb_owns_disk())
        return "usb";               /* io_go() refuses: never a spin */
    if (pause_count > 0)
        return "paused";
#ifdef HAVE_ART_BCM
    /* sitting 4c: the visualizer's view owns the BCM's overlay region
     * while open; the art work (most of it the BCM's decodes) waits for
     * the close, which wakes this thread (art_bcm_viz_view()) */
    if (art_bcm_viz_holds())
        return "viz";
#endif
    if (playing())
    {
        if (!user_idle())
            return "lit";           /* #58: nothing while the user is on */
    }
    else if (!maintenance
             && (TIME_BEFORE(current_tick, last_input + INPUT_QUIET)
                 || button_status()))
        return "input";
    if (!tagcache_is_usable() || tagcache_ui_busy() || tagcache_scan_active()
        || (maintenance && tagcache_maintenance_pending())
        || tagcache_get_commit_step() > 0 || tagcache_ui_count() < 0)
        return "db";
    batt = battery_level();
    if (!charger_inserted() && batt >= 0 && batt < LOW_BATTERY)
        return "battery";
    return NULL;
}

/* The progress page must distinguish an actual zero from a safe deferral.
 * status is deliberately just a best-effort cross-thread snapshot, like its
 * done/total fields: a stale value can only make the wording conservative. */
static void status_blocked(const char *why)
{
    if (!why)
        status.wait = ARTCACHE_BUILD_READY;
    else if (!strcmp(why, "db"))
        status.wait = ARTCACHE_BUILD_WAIT_DB;
    else if (!strcmp(why, "paused"))
        status.wait = ARTCACHE_BUILD_WAIT_PAUSE;
    else if (!strcmp(why, "lit") || !strcmp(why, "viz"))
        status.wait = ARTCACHE_BUILD_WAIT_PLAYBACK;
    else if (!strcmp(why, "battery"))
        status.wait = ARTCACHE_BUILD_WAIT_BATTERY;
    else
        status.wait = ARTCACHE_BUILD_WAIT_OFF;
}

/* How long a wait for 'why' sleeps before it looks again. "lit" blocks
 * until an event (the face, the backlight off, a stop or pause, a
 * request, USB, a power-off: never a poll), except on the charger, where
 * the INPUT_QUIET rule needs one wake when it runs out. */
static int wait_ticks(const char *why)
{
    long left;

    if (!strcmp(why, "viz"))
        return VIZ_TICKS;           /* the close wakes it (no poll) */
    if (strcmp(why, "lit"))
        return POLL_TICKS;
    if (!charger_inserted())
        return TIMEOUT_BLOCK;
    if (button_status())
        return POLL_TICKS;          /* a key held on the charger */
    left = last_input + INPUT_QUIET - current_tick;
    return left > 0 ? (int)left : 1;
}

/* Sleeps the gap between albums, then until nothing blocks the builder.
 * Returns the events that should end the pass (0: go on). */
static int wait_ready(bool gap)
{
    static const char *last_why;
    const char *why;
    bool unboosted = false;
    int ev;

    ev = pass_events;
    pass_events = 0;
    if (ev)
        return ev;
    if (gap && !maintenance)
        ev = pump(charger_inserted() ? GAP_CHARGING : GAP_TICKS);
    else
    {
        yield();                    /* the last album needed no I/O */
        ev = pump(0);
    }

    while (!ev)
    {
        idle_wait = true;           /* before the look: a post after it
                                       stays queued for the pump */
        why = blocked();
        status_blocked(why);
        if (why != last_why && (why || last_why))
            UPDATE_TRACE("art build wait %s", why ? why : "-");
#ifdef HAVE_ART_BCM
        if (why != last_why && why && !strcmp(why, "viz"))
            art_bcm_viz_defer("pass");
#endif
        last_why = why;
        if (!why)
            break;
        if (FAST_BOOSTED())
        {
            fast_boost(false);      /* never boosted while it waits */
            unboosted = true;
        }
        ev = pump(wait_ticks(why));
    }
    idle_wait = false;
    if (unboosted)
        fast_boost(true);
    return ev;
}

/* album() refused to start (ALBUM_LIT): the same album again once nothing
 * blocks. A maintenance pass has no gap between albums, so a refusal that
 * blocked() does not name would otherwise be retried in a boosted spin;
 * it sleeps a little first. */
static int wait_retry(void)
{
    if (maintenance)
    {
        bool boosted = FAST_BOOSTED();

        fast_boost(false);
        pass_events |= pump(RETRY_TICKS);
        if (boosted)
            fast_boost(true);
    }
    return wait_ready(true);
}

/* ======================================================================= */
/* builder: cache files                                                    */
/* ======================================================================= */
static bool version_ok(void)
{
    char buf[sizeof(VERSION_STR)];
    int fd = open(F_VERSION, O_RDONLY);
    bool ok = false;

    if (fd >= 0)
    {
        ok = read(fd, buf, sizeof(buf) - 1) == (ssize_t)sizeof(buf) - 1
             && !memcmp(buf, VERSION_STR, sizeof(buf) - 1);
        close(fd);
    }
    return ok;
}

static bool write_version(void)
{
    int fd = open(F_VERSION ".tmp", O_WRONLY | O_CREAT | O_TRUNC, 0666);
    bool ok;

    if (fd < 0)
        return false;
    ok = write(fd, VERSION_STR, sizeof(VERSION_STR) - 1)
         == (ssize_t)sizeof(VERSION_STR) - 1;
    ok = ok && fsync(fd) == 0;
    ok = close(fd) == 0 && ok;
    return ok && artfile_replace(F_VERSION ".tmp", F_VERSION) == 0;
}

static bool new_pak(struct bwork *w)
{
    struct artpak_hdr *h = (struct artpak_hdr *)w->page;
    struct artpak_hdr check;
    int fd;
    bool ok;

    ART_PACE();
    if (!artfile_tmp_clear(F_PAK_TMP, F_PAK))   /* #91 H1 */
        return false;
    fd = open(F_PAK_TMP, O_WRONLY | O_CREAT | O_TRUNC, 0666);
    if (fd < 0)
        return false;
    memset(w->page, 0, ARTPAK_HDR_BYTES);
    artpak_hdr_init(h, thumb.w, thumb.h);
    ok = write(fd, w->page, ARTPAK_HDR_BYTES) == ARTPAK_HDR_BYTES;
    ok = ok && fsync(fd) == 0;
    ok = close(fd) == 0 && ok;
    if (!ok)
        return false;
    ART_PACE();
    fd = open(F_PAK_TMP, O_RDONLY);
    if (fd < 0)
        return false;
    ok = read(fd, &check, sizeof(check)) == (ssize_t)sizeof(check)
         && artpak_hdr_ok(&check, thumb.w, thumb.h)
         && ffilesize(fd) == ARTPAK_HDR_BYTES;
    ok = close(fd) == 0 && ok;
    if (!ok)
        return false;
    /* Only the builder creates this file. Recheck absence; never replace
     * an existing pack after an ambiguous earlier failure. */
    fd = open(F_PAK, O_RDONLY);
    if (fd >= 0)
    {
        close(fd);
        return false;
    }
    return errno == ENOENT && rename(F_PAK_TMP, F_PAK) == 0;
}

/* thumbs.pak matches the size table and holds every slot the index
 * claims. */
static enum artidx_open_result pak_check(uint32_t n_slots, uint32_t *slots)
{
    struct artpak_hdr h;
    int fd = open(F_PAK, O_RDONLY);
    off_t bytes;
    ssize_t got;
    enum artidx_open_result rc;

    if (fd < 0)
        return errno == ENOENT ? ARTIDX_MISSING : ARTIDX_IO_ERROR;
    got = read(fd, &h, sizeof(h));
    bytes = ffilesize(fd);
    if (got < 0 || bytes < 0)
        rc = ARTIDX_IO_ERROR;
    else if (got != (ssize_t)sizeof(h)
             || !artpak_hdr_ok(&h, thumb.w, thumb.h)
             || bytes < (off_t)artpak_slot_offset(slot_bytes, n_slots))
        rc = ARTIDX_CORRUPT;
    else
    {
        *slots = MIN((uint32_t)((bytes - ARTPAK_HDR_BYTES) / slot_bytes),
                     ARTIDX_MAX_RECS);
        rc = ARTIDX_OPENED;
    }
    return close(fd) < 0 ? ARTIDX_IO_ERROR : rc;
}

static void mark_slot(void *ctx, const struct artcache_rec *rec)
{
    struct bwork *w = ctx;

    if (artrec_is_chapter(rec))
    {
        w->n_chap++;                /* no slot: 'slot' is a bundle key */
        return;
    }
    if (rec->state == ARTREC_OK && rec->slot < ARTIDX_MAX_RECS)
        w->freemap[rec->slot / 32] |= 1u << (rec->slot % 32);
}

static uint32_t alloc_slot(struct bwork *w)
{
    uint32_t s;

    for (s = w->free_from; s < w->ihdr.n_slots; s++)
    {
        if (!(w->freemap[s / 32] & (1u << (s % 32))))
        {
            w->freemap[s / 32] |= 1u << (s % 32);
            w->free_from = s + 1;
            return s;
        }
    }
    w->free_from = w->ihdr.n_slots;
    if (w->next_slot >= ARTIDX_MAX_RECS)
        return ARTREC_NO_SLOT;
    s = w->next_slot++;             /* appended: used from now on, also
                                       after a commit raises n_slots */
    w->freemap[s / 32] |= 1u << (s % 32);
    return s;
}

/* Opens the committed index (repairing what needs it) and the album
 * table. False: nothing to build. */
static bool pass_begin(struct bwork *w)
{
    enum artidx_open_result rc;
    uint32_t slots = 0, i;

    w->context = tagcache_context_acquire_passive();
    if (!w->context)
        return false;
    w->db_gen = tagcache_ui_generation_ctx(w->context);

    artidx_close(&w->rd);
    memset(w->freemap, 0, sizeof(w->freemap));
    w->n_chap = 0;
    w->n_delta = 0;
    w->tn = w->ti = 0;
    w->tfirst = 0;
    w->gc_ok = false;
    w->failed = false;
    w->deferred = false;
    w->fail_line = 0;
    w->matched = 0;
    w->sig_missing = w->sig_tried = w->sig_kept = 0;
    w->free_from = 0;
    w->last_commit = current_tick;
    w->hint_seen = hint_seq - 1;    /* look at the hints first */
    w->hmiss_n = w->hmiss_at = 0;
    w->n_early = 0;

    {
        struct artalb_stamp want = {
            tagcache_ui_commitid_ctx(w->context),
            tagcache_ui_count_ctx(w->context), SORTKEY_VERSION
        };

        snprintf(w->tpath, sizeof(w->tpath), "%s/%s",
                 tagcache_ui_db_path_ctx(w->context),
                 LIBFILE_NAME);
        rc = artalb_check(w->tpath, &want, &w->thdr, w->page);
    }
    if (rc != ARTIDX_OPENED)
    {
        /* missing / another database's: library.c writes the current one
         * and requests again. Torn: have it rewritten (silently). */
        UPDATE_TRACE("art build stop table=%d", (int)rc);
        if (rc == ARTIDX_CORRUPT)
            tagcache_ui_precompute_request();
        return false;
    }

    ART_PACE();
    mkdir(ARTCACHE_DIR);
    {
        static bool recovered;      /* #91 H1: once per boot */
        if (!recovered)
        {
            recovered = true;
            artfile_recover(F_INDEX, F_INDEX_TMP, F_INDEX_PREV);
            artfile_recover(F_PANELS, F_PANELS_TMP, F_PANELS_PREV);
            safefile_recover(F_POOLX, F_POOLX_TMP, F_POOLX_PREV);
            artfile_recover(F_BIDX, F_BIDX_TMP, F_BIDX_PREV);
        }
    }
    ART_PACE();
    rc = artidx_open_ex(&w->rd, F_INDEX, mark_slot, w);
    if (rc == ARTIDX_IO_ERROR)
        goto unavailable;
    if (rc != ARTIDX_OPENED)
    {
        enum artidx_open_result old;

        /* A published previous index is preferred to an uncommitted temp.
         * Recover the name without ever deleting the surviving copy. */
        memset(w->freemap, 0, sizeof(w->freemap));
        w->n_chap = 0;
        ART_PACE();
        old = artidx_open_ex(&w->rd, F_INDEX_PREV, mark_slot, w);
        if (old == ARTIDX_OPENED)
        {
            artidx_close(&w->rd);
            if (artfile_replace(F_INDEX_PREV, F_INDEX) < 0)
                goto unavailable;
            rc = artidx_open_ex(&w->rd, F_INDEX, NULL, NULL);
            if (rc != ARTIDX_OPENED)
                goto unavailable;
            commit_seq++;
            UPDATE_TRACE("art recover index %s", "previous");
        }
        else if (old == ARTIDX_IO_ERROR)
            goto unavailable;
        else
        {
            enum artidx_open_result tmp;

            ART_PACE();
            tmp = artidx_open(&w->rd, F_INDEX_TMP);
            if (tmp == ARTIDX_OPENED)
            {
                if (pak_check(w->rd.hdr.n_slots, &slots) != ARTIDX_OPENED)
                    goto unavailable;
                artidx_close(&w->rd);
                if (artfile_replace(F_INDEX_TMP, F_INDEX) < 0)
                    goto unavailable;
                memset(w->freemap, 0, sizeof(w->freemap));
                w->n_chap = 0;
                rc = artidx_open_ex(&w->rd, F_INDEX, mark_slot, w);
                if (rc != ARTIDX_OPENED)
                    goto unavailable;
                commit_seq++;
                UPDATE_TRACE("art recover index %s", "complete-temp");
            }
            else if (tmp == ARTIDX_IO_ERROR || rc == ARTIDX_CORRUPT
                     || old == ARTIDX_CORRUPT)
                goto unavailable;
        }
    }
    memset(&w->ihdr, 0, sizeof(w->ihdr));
    if (rc == ARTIDX_OPENED)
    {
        w->ihdr = w->rd.hdr;
        ART_PACE();
        if (pak_check(w->ihdr.n_slots, &slots) != ARTIDX_OPENED)
            goto unavailable;
        ART_PACE();
        /* Protect the previous generation too. Reuse the pinned reader
         * sequentially: no new permanent reader or shared scratch. */
        artidx_close(&w->rd);
        rc = artidx_open(&w->rd, F_INDEX_PREV);
        if (rc == ARTIDX_IO_ERROR)
            goto unavailable;
        if (rc == ARTIDX_OPENED)
        {
            struct artcache_rec r;

            for (i = 0; i < w->rd.hdr.count; i++)
            {
                if (!(i % ARTIDX_PAGE_RECS))
                    ART_PACE();     /* per page read */
                if (!artidx_get(&w->rd, i, &r))
                    goto unavailable;
                if (r.state == ARTREC_OK && !artrec_is_chapter(&r)
                    && r.slot < ARTIDX_MAX_RECS)
                    w->freemap[r.slot / 32] |= 1u << (r.slot % 32);
            }
        }
        ART_PACE();
        rc = artidx_open(&w->rd, F_INDEX);
        if (rc != ARTIDX_OPENED)
            goto unavailable;
    }
    else
    {
        ART_PACE();
        rc = pak_check(0, &slots);
        if (rc == ARTIDX_MISSING)
        {
            if (!new_pak(w))
                goto unavailable;
        }
        else if (rc != ARTIDX_OPENED)
            goto unavailable;
        /* An intact pack without an index is retained, never truncated.
         * Its old slots are unowned, so append rather than overwrite. */
        w->ihdr.n_slots = slots;
        for (i = 0; i < slots; i++)
            w->freemap[i / 32] |= 1u << (i % 32);
    }
    ART_PACE();
    /* Marker loss is not cache loss. Headers decide compatibility. */
    if (!version_ok() && !write_version())
        UPDATE_TRACE("art retain version %s", "marker-unavailable");
    ART_PACE();
    w->next_slot = w->ihdr.n_slots;
    w->start_count = w->ihdr.count;
    /* A full source re-check follows a library change. USB alone resumes an
     * interrupted pass without assuming that every cover changed. */
    w->sweep = w->ihdr.sweep_commit != (uint32_t)w->thdr.lib.commitid;
    return true;
unavailable:
    artidx_close(&w->rd);
    FAIL(w);
    UPDATE_TRACE("art retain index reason=%d", (int)rc);
    return false;
}

/* GC (#85 B-M4): records of albums no longer in the table are kept (a
 * library change, even a temporary one, is not permission to evict
 * artwork) until the index or the pack passes 3/4 of ARTIDX_MAX_RECS;
 * then the final commit of a pass that walked the whole table drops
 * them, only while stopped (#41.1: it streams the table and walks the
 * index). artidx_gc_mark() streams the table (folder order) through the
 * decode buffer (idle between albums; re-fetched after every read) in
 * sorted chunks merged with one sequential walk of the index each,
 * marking the survivors in w->live. A dropped record's slot stays
 * reserved while index.prev still names it (pass_begin()), and nothing is
 * deleted from inside the merge: .prev keeps working. */
#define ART_GC_MARK (ARTIDX_MAX_RECS / 4 * 3)
static bool art_gc_pressure(uint32_t count, uint32_t slots)
{
    return count >= ART_GC_MARK || slots >= ART_GC_MARK;
}

static uint64_t *gc_keys(void *ctx)
{
    (void)ctx;
    return core_get_data(dec_h);
}

static bool keep_live(void *ctx, const struct artcache_rec *rec, uint32_t i)
{
    struct bwork *w = ctx;
    (void)rec;
    return (w->live[i / 32] >> (i % 32)) & 1;
}

/* The delta in key order, one record per key (the last one wins): the
 * table is in folder order and may repeat a key. */
static void sort_delta(struct bwork *w)
{
    int i, j, n = 0;

    for (i = 1; i < w->n_delta; i++)
    {
        struct artcache_rec r = w->delta[i];

        for (j = i; j > 0 && w->delta[j - 1].key > r.key; j--)
            w->delta[j] = w->delta[j - 1];
        w->delta[j] = r;
    }
    for (i = 0; i < w->n_delta; i++)
    {
        if (n > 0 && w->delta[n - 1].key == w->delta[i].key)
            n--;
        w->delta[n++] = w->delta[i];
    }
    w->n_delta = n;
}

/* Writes index.tmp (committed index + delta, + GC when final), renames it
 * over index.dat and reopens. The GC (and the sweep's stamp, which needs
 * it) waits for playback to stop (#41.1): it streams the whole table and
 * walks the whole index (~6 reads of a 1.6 MB index at 50k), kept off
 * the playback path; until then the dropped albums' records stay (no
 * harm: nothing looks them up) and the pass runs again at the stop. */
/* index.tmp = the committed index + 'add' (sorted, unique), + GC; renamed
 * over index.dat and reopened. */
static bool index_write(struct bwork *w, const struct artcache_rec *add,
                        int n_add, bool gc, bool stamp)
{
    int n, i, j;
    struct artidx_hdr next = w->ihdr;

    next.generation++;
    next.n_slots = w->next_slot;
    if (stamp)
        next.sweep_commit = (uint32_t)w->thdr.lib.commitid;
    /* Without the GC, records for removed albums are retained; the merge
     * itself has no side effects either way. */
    if (!artfile_tmp_clear(F_INDEX_TMP, F_INDEX))  /* #91 H1 */
        goto failed;
    ART_PACE();
    n = artidx_write_merged(F_INDEX_TMP, &w->rd, add, n_add,
                            gc ? keep_live : NULL, w, &next, w->page);
    w->last_commit = current_tick;
    if (n < 0)
        goto failed;
    ART_PACE();
    artidx_close(&w->rd);
    if (artidx_open(&w->rd, F_INDEX_TMP) != ARTIDX_OPENED)
        goto failed;
    artidx_close(&w->rd);
    ART_PACE();
    if (artfile_publish(F_INDEX, F_INDEX_TMP, F_INDEX_PREV)
        != ARTFILE_PUBLISHED)
        goto failed;
    ART_PACE();
    if (artidx_open(&w->rd, F_INDEX) != ARTIDX_OPENED)
        goto failed;
    w->ihdr = w->rd.hdr;
    /* A requested local repair is fulfilled only by a published OK record. */
    for (i = 0; i < n_add; i++)
        if (add[i].state == ARTREC_OK)
            for (j = 0; j < ART_HINTS; j++)
                if (repair_keys[j] == add[i].key)
                    repair_keys[j] = 0;
    commit_seq++;
    UPDATE_TRACE("art build commit gen=%lu recs=%d gc=%d playing=%d",
                 (unsigned long)w->ihdr.generation, n, (int)gc,
                 (int)playing());
    return true;
failed:
    artidx_close(&w->rd);
    if (artidx_open(&w->rd, F_INDEX) != ARTIDX_OPENED)
        artidx_open(&w->rd, F_INDEX_PREV);
    FAIL(w);
    UPDATE_TRACE("art retain index %s", "publication-failed");
    return false;
}

/* force: GC even if no album went (the chapter pass rewrote a book) */
static bool commit_ex(struct bwork *w, bool final, bool force)
{
    bool done = final && w->gc_ok && !w->failed;
    bool stamp = done && w->sweep;  /* the source re-check completed */
    bool gc = done && (force || w->sweep
                       || w->matched + w->n_chap < w->start_count)
              && art_gc_pressure(w->ihdr.count, w->next_slot);
    bool ok;

    if (gc && playing())
    {
        gc = false;
        gc_owed = true;             /* a pass runs again at the stop */
        UPDATE_TRACE("art build gc deferred recs=%lu",
                     (unsigned long)w->ihdr.count);
    }
    if (!w->n_delta && !gc && !stamp)
        return true;                /* nothing changed, nothing to drop */
    ART_PACE();
    sort_delta(w);
    ART_PACE();
    if (gc && !artidx_gc_mark(&w->rd, w->tpath, &w->thdr, w->trec, TCHUNK,
                              gc_keys, NULL, decode_bytes / sizeof(uint64_t),
                              w->live))
        gc = false;                 /* replaced, I/O error: keep all */
    /* chapter records (no album key): live while their book is live,
     * done and of the same mtime; past the buffer's cap, all kept */
    if (gc && !artidx_gc_mark_chapters(&w->rd, gc_keys, NULL,
                                       decode_bytes / sizeof(uint64_t),
                                       w->live))
        gc = false;
    ART_PACE();
    ok = index_write(w, w->delta, w->n_delta, gc, stamp);
    if (ok)
        w->n_delta = 0;
    return ok;
}

static bool commit(struct bwork *w, bool final)
{
    return commit_ex(w, final, false);
}

/* ======================================================================= */
/* builder: one album                                                      */
/* ======================================================================= */
static void copy_tag(struct bwork *w, char *dst, const int32_t *seeks, int tag)
{
    const char *s = tagcache_ui_str_ctx(w->context, tag, seeks[tag]);

    strmemccpy(dst, s ? s : "", MAX_PATH);
}

/* Path of the representative track; album strings for the file search
 * (copied: RAM cache pointers die at the first yield). */
static bool resolve(struct bwork *w, const struct artalb_rec *t)
{
    int32_t flag;
    const int32_t *s = tagcache_ui_seeks_ctx(w->context, t->rep_idx, &flag);
    bool ok;

    if (!s)
        return false;
#ifdef HAVE_BCMFW_VIDEO
    w->vid = (t->flags & LIBFILE_ALB_VIDEO) != 0;
#endif
    copy_tag(w, w->album, s, tag_album);
    copy_tag(w, w->artist, s, tag_artist);
    copy_tag(w, w->aartist, s, tag_albumartist);
    if (!tagcache_search_ctx(w->context, &w->tcs, tag_filename))
        return false;
    ok = tagcache_retrieve(&w->tcs, t->rep_idx, tag_filename, w->path,
                           sizeof(w->path));
    tagcache_search_finish(&w->tcs);
    return ok;
}

#ifdef HAVE_BCMFW_VIDEO
/* Phase 14a (docs/phase14a-brief.md 3.5): a video's poster file that the
 * stock search does not look for, poster.jpg beside it (the stock search
 * finds its own <stem>.jpg first). One lookup, only for a video with no
 * other image file. */
static bool poster_file(struct bwork *w)
{
    static const char name[] = "poster.jpg";
    char *slash = strrchr(w->path, '/');
    size_t dir = slash ? (size_t)(slash - w->path) + 1 : 0;

    if (dir + sizeof(name) > sizeof(w->src))
        return false;
    memcpy(w->src, w->path, dir);
    memcpy(w->src + dir, name, sizeof(name));
    return file_exists(w->src);
}
#endif

/* The stock image-file search into w->src. from_tags: over the tagcache
 * strings, without opening the audio file (w->id3 is only a carrier then);
 * else over the w->id3 get_metadata() filled. */
static bool find_file(struct bwork *w, bool from_tags)
{
    if (from_tags)
    {
        memset(&w->id3, 0, sizeof(w->id3));
        strmemccpy(w->id3.path, w->path, sizeof(w->id3.path));
        w->id3.album = w->album[0] ? w->album : NULL;
        w->id3.artist = w->artist[0] ? w->artist : NULL;
        w->id3.albumartist = w->aartist[0] ? w->aartist : NULL;
    }
#ifdef HAVE_BCMFW_VIDEO
    if (search_albumart_files(&w->id3, "", w->src, sizeof(w->src)))
        return true;
    return w->vid && poster_file(w);
#else
    return search_albumart_files(&w->id3, "", w->src, sizeof(w->src));
#endif
}

/* CRC-32 of 'len' bytes of a source from 'off', continuing 'crc' */
static uint32_t src_crc_run(struct bwork *w, int fd, off_t off, uint32_t len,
                            uint32_t crc)
{
    if (lseek(fd, off, SEEK_SET) != off)
    {
        w->source_retry = true;
        return crc;
    }
    while (len)
    {
        int n;

        ART_PACE();
        n = read(fd, w->page, MIN(len, sizeof(w->page)));
        if (n <= 0)
            break;
        crc = artfmt_crc(crc, w->page, n);
        len -= n;
    }
    if (len)
        w->source_retry = true;
    return crc;
}

/* A source's signature CRC (artfmt.h ARTREC_T_SIG2), from 'off': with
 * 'sig2' its first and last SRC_CRC_BYTES / 2 (all of it up to
 * SRC_CRC_BYTES), else its first SRC_CRC_BYTES (records from before). */
static uint32_t src_crc(struct bwork *w, int fd, off_t off, uint32_t size,
                        bool sig2)
{
    uint32_t crc = ARTFMT_CRC_INIT, half = SRC_CRC_BYTES / 2;

    if (!sig2 || size <= SRC_CRC_BYTES)
        crc = src_crc_run(w, fd, off, MIN(size, SRC_CRC_BYTES), crc);
    else
    {
        crc = src_crc_run(w, fd, off, half, crc);
        crc = src_crc_run(w, fd, off + (off_t)(size - half), half, crc);
    }
    return artfmt_crc_end(crc);
}

/* The image at 'off' of 'fd' (JPEG or BMP) into 'bm' at bw x bh (0 x 0:
 * its own size) from 'dbuf', the decode block: the decoder's result, -1
 * if the seek failed. */
static int img_decode(void *dbuf, struct bitmap *bm,
                      int fd, off_t off, unsigned long len, int type,
                      int aaflags, int bw, int bh, int fmt,
                      const struct custom_format *cf)
{
    memset(bm, 0, sizeof(*bm));
    bm->width = bw;
    bm->height = bh;
    bm->data = dbuf;
    if (lseek(fd, off, SEEK_SET) != off)
        return -1;
    if (type == AA_TYPE_JPG)
        return clip_jpeg_fd(fd, aaflags, len, bm, decode_bytes, fmt, cf);
    return read_bmp_fd(fd, bm, decode_bytes, fmt, cf);
}

/* A thumbnail (the slot, W x H) of art whose fitted size leaves bars
 * wider than 1 px a side: decoded to that size (aspect kept, never
 * cropped or stretched, P6 owner), centred on a darkened, blurred copy of
 * itself stretched to the whole slot (owner, from the cover-fill mock-up:
 * the art reads as one tile in the list's square art column). Done once
 * at build time, from the pixels already decoded and with the decode
 * block's free tail as scratch (no allocation, no second read); the list
 * blits it like any thumbnail. Returns false (nothing done) for
 * square-enough art, which keeps the centre crop; else true, *rc the
 * decoder's. */
static bool thumb_fitted(struct bwork *w, void *dbuf, int fd, off_t off,
                         unsigned long len, int type, int aaflags,
                         int sw, int sh, int *rc)
{
    const int W = w->tw, H = w->th;
    const int fmt = FORMAT_NATIVE | FORMAT_DITHER | FORMAT_RESIZE;
    struct artcache_fit f;
    struct bitmap bm;
    fb_data *out = (fb_data *)slot_buf(w), *pix;
    uint8_t *scratch;
    int y;

    artcache_fit_rect(sw, sh, W, H, &f);
    if (W - f.w <= 2 && H - f.h <= 2)
        return false;
    *rc = img_decode(dbuf, &bm, fd, off, len, type, aaflags, f.w, f.h,
                     fmt, NULL);
    if (*rc <= 0 || bm.width != f.w || bm.height != f.h)
    {
        *rc = -1;
        return true;
    }
    pix = (fb_data *)bm.data;
    scratch = (uint8_t *)(pix + f.w * f.h);
    if (scratch + ARTBLUR_SCRATCH(W, H) <= (uint8_t *)dbuf + w->dec_bytes)
        artblur_fill(out, W, H, pix, f.w, f.h, scratch, art_gate_yield);
    else    /* never with decode_bytes (8 W H + ...): black bars */
    {
        DEBUGF("art fit: no blur scratch\n");
        memset(out, 0, (size_t)W * H * sizeof(fb_data));
    }
    for (y = 0; y < f.h; y++)
        memcpy(out + (f.y + y) * W + f.x, pix + y * f.w,
               f.w * sizeof(fb_data));
    return true;
}

/* --- panels, decoded in bands (#58) ------------------------------------ *
 * A 300 x 300 panel used to need a transient ~786 KB block (the whole
 * fill-size bitmap), so it could only be built while audio was stopped,
 * and on a fresh install the drift showed one or two covers for hours.
 * Now the decoder's rows go through a custom output format: each row of
 * the fill-size image that falls inside the centre crop is converted
 * straight into a band of whole panel rows at the start of the reserved
 * decode block (the decoder's own state follows the band), and each full
 * band is written to the panel's slot in panels.pak: one lseek + write
 * per band. Rows outside the crop are never converted. No allocation at
 * all, so a panel builds like a thumbnail: during playback too, while
 * the user is idle (every band write checks they still are, #58).
 *
 * Band size: what the decode block has left after the decoder (JPEG
 * state, resize line buffers, slack): on the target (98,588 B block) a
 * square cover gets 48 rows (28,800 B): 7 writes per panel; a 2:1 cover
 * 16 rows (19 writes). Wider than ~2.6:1 (a fill width over ~780 px)
 * does not fit: the panel is FAILED (its album shows in the lists, just
 * not in the drift).
 * The rows go to native RGB565 the way the stock converters do it (same
 * rounding and dither). */
#define PS_ALIGN    16              /* rows: the dither period */

struct pstream {
    int fd;                         /* panels.pak, open for writing */
    off_t off;                      /* the slot's offset */
    fb_data *band;                  /* at the decode block's start */
    int rows;                       /* band rows (multiple of PS_ALIGN) */
    int b0;                         /* panel row of band row 0, -1 none */
    int x0, y0;                     /* the crop in the fill-size image */
#ifdef HAVE_BCMFW_VIDEO
    int cx, cw;                     /* the picture's columns in the panel
                                       (0, panel.w: all); the rest black */
#endif
    int dir;                        /* +1 rows ascending, -1 descending */
    int last;                       /* the last panel row converted */
    int writes;
    uint32_t crc;
    bool err;
    bool lit;                       /* a band refused: the user came back */
};
static struct pstream *ps_cur;      /* the builder thread's, while decoding */

/* Writes the band's rows (all of them that are inside the panel) */
/* The file reaches 'at' (zeros from its end): the device's lseek refuses
 * a position past the end (firmware/common/file.c), and a bottom-up
 * source writes its last band first into an appended slot (#100 review
 * M2). Only such a source pays the extra writes. */
static bool ps_reach(int fd, off_t at)
{
    char zero[512];
    off_t end = lseek(fd, 0, SEEK_END);

    if (end < 0)
        return false;
    memset(zero, 0, sizeof(zero));
    while (end < at)
    {
        size_t k = MIN((off_t)sizeof(zero), at - end);

        if (write(fd, zero, k) != (ssize_t)k)
            return false;
        end += k;
    }
    return true;
}

/* #117: a slot of the published panels.pak is written in place, and
 * only a slot its header does not name; but a shown set that has not
 * followed the header yet (panelpool.idx, pool_follow()) may still name
 * it with the cover it held before. Its tag goes first (zeros, on disk
 * before any pixel), so neither a reader meanwhile nor a power cut
 * mid-slot ever finds new pixels under the old cover's tag: the slot
 * reads as no cover until its own tag is written last. */
static bool ps_untag(int fd, off_t at)
{
    struct artpak_tag zero;
    off_t end = lseek(fd, 0, SEEK_END);

    if (end < 0)
        return false;
    if (end <= at)
        return true;                /* an append: no old tag */
    memset(&zero, 0, sizeof(zero));
    return lseek(fd, at, SEEK_SET) == at
           && write(fd, &zero, sizeof(zero)) == (ssize_t)sizeof(zero)
           && fsync(fd) == 0;
}

static bool ps_flush(struct pstream *s)
{
    int n;
    off_t at;
    size_t bytes;

    if (s->b0 < 0 || s->err)
        return !s->err;
    n = MIN(s->rows, panel.h - s->b0);
    at = s->off + (off_t)s->b0 * panel.w * sizeof(fb_data);
    bytes = (size_t)n * panel.w * sizeof(fb_data);
    /* during playback only while the user is idle (#58): they may have
     * come back while the decoder ran */
    if (!io_ok() || usb_owns_disk()
        || (npr_busy && (npr_abort || !disk_active())))
    {
        s->lit = true;
        s->err = true;
        return false;
    }
    if (!ps_reach(s->fd, at) || lseek(s->fd, at, SEEK_SET) != at
        || write(s->fd, s->band, bytes) != (ssize_t)bytes)
    {
        s->err = true;
        return false;
    }
    ART_PACE();
    if (s->dir > 0)
        s->crc = artfmt_crc(s->crc, s->band, bytes);
    s->writes++;
    s->b0 = -1;
    return true;
}

/* The band row of panel row y (a new band when y leaves this one), or
 * -1 */
static int ps_place(struct pstream *s, int y)
{
    if (s->last >= 0 && !s->dir)
        s->dir = y > s->last ? 1 : -1;
    s->last = y;
    if (s->b0 >= 0 && (y < s->b0 || y >= s->b0 + s->rows))
        ps_flush(s);
    if (s->err)
        return -1;
    if (s->b0 < 0)
        s->b0 = y / s->rows * s->rows;
    return y - s->b0;
}

#ifdef HAVE_BCMFW_VIDEO
/* Phase 14a: the black columns either side of a portrait picture in
 * panel row 'd' */
static void ps_bars(const struct pstream *s, fb_data *d)
{
    memset(d, 0, s->cx * sizeof(fb_data));
    memset(d + s->cx + s->cw, 0,
           (panel.w - s->cx - s->cw) * sizeof(fb_data));
}
#endif

/* 8-bit rows (no resize: the source is the fill size): bmp.c's
 * output_row_8_native() conversion (same rounding), for the crop's
 * columns only.  The dither runs at the pixel's place in the whole
 * dw x dh picture (column x0 + col, row 'row'), as a full decode's does,
 * so a streamed crop equals that decode's window bit for bit, and the
 * BCM's (album-render.md section 13, open item 7) */
static void ps_row_8(uint32_t row, void *row_in, struct scaler_context *ctx)
{
    struct pstream *s = ps_cur;
    const struct uint8_rgb *qp;
    const uint8_t dy = DITHERY(row);
    int y = (int)row - s->y0, j, col, cw = panel.w;
    fb_data *dest;

    if (usb_owns_disk())
    {
        if (!s->err)
            UPDATE_TRACE("art panel abort %s", "usb");
        s->lit = true;
        s->err = true;
        return;
    }
    if (y < 0 || y >= panel.h || (j = ps_place(s, y)) < 0)
        return;
    qp = (const struct uint8_rgb *)row_in + s->x0;
    dest = s->band + j * panel.w;
#ifdef HAVE_BCMFW_VIDEO
    ps_bars(s, dest);               /* nothing for a whole-slot cover */
    dest += s->cx;
    cw = s->cw;
#endif
    for (col = 0; col < cw; col++, qp++)
    {
        int delta = ctx->dither ? DITHERXDY(s->x0 + col, dy) : 127;
        unsigned r = qp->red, g = qp->green, b = qp->blue;

        r = (31 * r + (r >> 3) + delta) >> 8;
        g = (63 * g + (g >> 2) + delta) >> 8;
        b = (31 * b + (b >> 3) + delta) >> 8;
        *dest++ = FB_RGBPACK_LCD(r, g, b);
    }
}

/* Scaled rows: resize.c's RGB565 conversion (output_row_32_native and
 * its _fromyuv twin are static there), for the crop's columns only; the
 * dither at the whole picture's column (ps_row_8()) */
static void ps_row_32x(uint32_t row, void *row_in, struct scaler_context *ctx,
                       bool yuv)
{
    struct pstream *s = ps_cur;
    const struct uint32_argb *qp;
    const uint8_t dy = DITHERY(row);
    int y = (int)row - s->y0, j, col;
    fb_data *dest;

    if (usb_owns_disk())
    {
        if (!s->err)
            UPDATE_TRACE("art panel abort %s", "usb");
        s->lit = true;
        s->err = true;
        return;
    }
    if (y < 0 || y >= panel.h || (j = ps_place(s, y)) < 0)
        return;
    qp = (const struct uint32_argb *)row_in + s->x0;
    dest = s->band + j * panel.w;
#ifdef HAVE_BCMFW_VIDEO
    ps_bars(s, dest);               /* nothing for a whole-slot cover */
    dest += s->cx;
    for (col = 0; col < s->cw; col++, qp++)
#else
    for (col = 0; col < panel.w; col++, qp++)
#endif
    {
        int delta = ctx->dither ? DITHERXDY(s->x0 + col, dy) : 127;
        unsigned r, g, b;

        if (yuv)
            yuv_to_rgb(SC_OUT(qp->b, ctx), SC_OUT(qp->g, ctx),
                       SC_OUT(qp->r, ctx), &r, &g, &b);
        else
        {
            r = SC_OUT(qp->r, ctx);
            g = SC_OUT(qp->g, ctx);
            b = SC_OUT(qp->b, ctx);
        }
        r = (31 * r + (r >> 3) + delta) >> 8;
        g = (63 * g + (g >> 2) + delta) >> 8;
        b = (31 * b + (b >> 3) + delta) >> 8;
        *dest++ = FB_RGBPACK_LCD(r, g, b);
    }
}

static void ps_row_32(uint32_t row, void *row_in, struct scaler_context *ctx)
{
    ps_row_32x(row, row_in, ctx, false);
}

static void ps_row_32_yuv(uint32_t row, void *row_in,
                          struct scaler_context *ctx)
{
    ps_row_32x(row, row_in, ctx, true);
}

static unsigned int ps_size(struct bitmap *bm)
{
    (void)bm;
    return (unsigned int)ps_cur->rows * panel.w * sizeof(fb_data);
}

static const struct custom_format ps_format = {
    .output_row_8 = ps_row_8,
    .output_row_32 = { ps_row_32, ps_row_32_yuv },
    .get_size = ps_size,
};

#ifdef HAVE_BCMFW_VIDEO
/* Phase 14a (docs/phase14a-brief.md 3.5): the width of sw x sh art in a
 * panel. 0, the whole slot (centre-cropped to fill it), unless the art
 * is portrait enough that the crop would lose over a tenth of its
 * height; then the width it has at the slot's full height, even, at
 * least 2:3 (200 of 300: the 160 px window keeps 40 px to pan in). By
 * aspect only: a poster, or a podcast's or an album's portrait art. */
static int panel_cw(int sw, int sh)
{
    long cw = ((long)sw * panel.h / sh) & ~1L;

    if (cw >= (long)panel.w * 9 / 10)
        return 0;
    return MAX((int)cw, panel.h * 2 / 3);
}
#endif

#ifdef HAVE_ART_BCM
static int ps_bcm_rows(struct bwork *w, struct pstream *s);
#endif

/* Decodes the image at 'off' of 'fd' to the fill size dw x dh and streams
 * its centre crop into panel slot w->ps_slot of panels.pak, then the tag
 * and the padding. 'dbuf': the decode block, pinned by decode(). 'bcm'
 * (19-6): the rows come from the BCM's decoded slot instead. */
static bool ps_decode(struct bwork *w, void *dbuf, int fd, off_t off,
                      unsigned long len, int type, int aaflags,
                      int dw, int dh, bool bcm)
{
    const size_t n = (size_t)panel.w * panel.h * sizeof(fb_data);
    const int fmt = FORMAT_NATIVE | FORMAT_DITHER | FORMAT_RESIZE;
    struct pstream s;
    struct artpak_tag tag;
    struct bitmap bm;
    long room;
    int rc;
    bool ok;

    (void)bcm;                      /* without HAVE_ART_BCM: trace only */
    room = (long)w->dec_bytes - (long)JPEG_DECODE_OVERHEAD
         - 3 * (long)sizeof(struct uint32_argb) * dw - 4096;
    memset(&s, 0, sizeof(s));
    s.rows = room > 0 ? (int)(room / (panel.w * (long)sizeof(fb_data))) : 0;
    s.rows = MIN(s.rows, ALIGN_UP(panel.h, PS_ALIGN)) / PS_ALIGN * PS_ALIGN;
    if (s.rows < PS_ALIGN)
    {
        UPDATE_TRACE("art panel stream no room dw=%d dh=%d", dw, dh);
        w->ps_too_wide = true;
        return false;
    }
    s.band = (fb_data *)dbuf;
    s.b0 = -1;
    s.last = -1;
#ifdef HAVE_BCMFW_VIDEO
    /* a portrait picture (w->ps_cw, panel_cw()) fills cw columns,
     * centred; dw x dh is then its fill size in cw x panel.h */
    s.cw = w->ps_cw ? w->ps_cw : panel.w;
    s.cx = (panel.w - s.cw) / 2;
    s.x0 = (dw - s.cw) / 2;
#else
    s.x0 = (dw - panel.w) / 2;
#endif
    s.y0 = (dh - panel.h) / 2;
    s.crc = ARTFMT_CRC_INIT;
    s.off = artpanel_slot_offset(panel_slot_bytes, w->ps_slot);
    if (!io_go(w, "write", w->ps_key))
    {
        w->ps_io_err = true;        /* the user came back: deferred */
        return false;
    }
    s.fd = open(w->ps_path, O_WRONLY);
    if (s.fd < 0)
    {
        w->ps_io_err = true;
        return false;
    }
    if (w->ps_inplace && !ps_untag(s.fd, s.off + (off_t)n))
    {
        close(s.fd);
        w->ps_io_err = true;
        return false;
    }

#ifdef HAVE_ART_BCM
    if (bcm)
        rc = ps_bcm_rows(w, &s);    /* the rows from the BCM's slot */
    else
#endif
    {
        ps_cur = &s;
        rc = img_decode(dbuf, &bm, fd, off, len, type, aaflags, dw, dh,
                        fmt, &ps_format);
        ps_cur = NULL;
    }
    ok = rc > 0 && !s.err && s.last >= 0 && ps_flush(&s);
    if (s.err)
        w->ps_io_err = true;        /* the user came back, or a write */
    if (s.lit)
        w->lit = true;

    /* the tag (key, CRC of the pixels: computed for top-down sources;
     * bottom-up BMPs write 0, nothing checks a panel's CRC) and the
     * padding, right after the pixels */
    if (ok && (!io_ok() || usb_owns_disk()))
        w->lit = true;
    if (ok && !w->lit)
    {
        memset(w->page, 0, panel_slot_bytes - n);
        tag.key = w->ps_key;
        tag.pix_crc = s.dir > 0 ? artfmt_crc_end(s.crc) : 0;
        tag.magic = ARTPAK_TAG_MAGIC;
        memcpy(w->page, &tag, sizeof(tag));
        /* the pixels' CRC: the same on either side (19-6) */
        UPDATE_TRACE("art panel crc key=" KEYFMT " crc=%08lx bcm=%d",
                     KEYARG(w->ps_key), (unsigned long)tag.pix_crc,
                     (int)bcm);
        ok = lseek(s.fd, s.off + (off_t)n, SEEK_SET) == s.off + (off_t)n
             && write(s.fd, w->page, panel_slot_bytes - n)
                == (ssize_t)(panel_slot_bytes - n);
        s.writes++;
        if (!ok)
            w->ps_io_err = true;
    }
    else
    {
        if (ok)
            w->ps_io_err = true;    /* the user came back before the tag */
        ok = false;
    }
    /* #100: a pool slot is written in place: on disk before the index
     * names it; #117: a panels.pak slot too, before its header does */
    if (ok && (w->ps_pool || w->ps_inplace) && fsync(s.fd) != 0)
    {
        w->ps_io_err = true;
        ok = false;
    }
    if (close(s.fd) != 0 && ok)
    {
        w->ps_io_err = true;
        ok = false;
    }
    UPDATE_TRACE("art panel stream key=" KEYFMT " rows=%d writes=%d ok=%d",
                 KEYARG(w->ps_key), s.rows, s.writes, (int)ok);
    return ok;
}

#ifdef HAVE_ART_BCM
/* ---- step 2 on the BCM2722 (P19 19-6) ----------------------------------- *
 * album-render.md section 14: the splice is decode_run()'s step 2, where
 * the final dw x dh is known; everything above it (the source order, the
 * signature, the probe, the size rules) stays one definition for both
 * sides.  One output a call (n = 1): the BCM's pixels are then the
 * builder's own single decode, cropped (bcmjob.h, the exactness promise),
 * so the box and npcover.pak are the same bits whichever side decoded.
 * Read back in bands, each checked against COMMIT's F16w; only on the
 * art thread, in the background (host-driver.md: never on a path the
 * user waits on); any doubt is the CPU code below the splice. */

/* bcmjob_decode()'s cancel hook (art thread, about every millisecond):
 * USB or a power-off queued for this thread (it acks at once), the host
 * on the disk, or np-sharp called off as art_gate() would call it off
 * (Now Playing watched, a key down or pressed since the render began) */
static bool bcm_cancel(void *ctx)
{
    (void)ctx;
    if (usb_owns_disk() || queue_peek_ex(&art_queue, NULL, 2, park_release))
        return true;
    if (art_bcm_viz_holds())
        return true;                /* the visualizer's view opened: the
                                       BCM's region and cadence are its */
    if (!npr_busy)
        return false;
    if (!npr_abort && (np_watched() || button_status()
                       || TIME_AFTER(last_input, npr_t0)))
    {
        npr_abort = true;
        UPDATE_TRACE("art npcover abort disk=%d np=%d input=%d",
                     (int)disk_active(), (int)np_watched(),
                     (int)TIME_AFTER(last_input, npr_t0));
    }
    return npr_abort;
}

/* np-sharp's panel from its BCM slot, in bands of the stream's rows (the
 * producer in place of the decoder's rows): 1, or -1 (s->err: a write
 * refused or failed, as the CPU's; w->ps_bcm_bad: the read-back failed,
 * the CPU decodes it) */
static int ps_bcm_rows(struct bwork *w, struct pstream *s)
{
    int y, n = 0, j;
#ifdef HAVE_BCMFW_VIDEO
    int r;
#endif

    s->dir = 1;                     /* top-down: the tag's CRC */
    art_bcm_rewind();
    for (y = 0; y < panel.h; y += n)
    {
        n = MIN(s->rows, panel.h - y);
        /* the background read-back: never while the Now Playing row is
         * up (it shows the resident slot), stopped between bands; the
         * next idle reads it again from the slot (a hit, no decode) */
        if (bcm_cancel(w) || art_bcm_np_is_shown())
        {
            s->lit = true;          /* the user came back: deferred */
            s->err = true;
            return -1;
        }
        if ((j = ps_place(s, y)) < 0)
            return -1;              /* the last band's write failed */
        s->last = y + n - 1;
#ifdef HAVE_BCMFW_VIDEO
        if (!art_bcm_read(y, n, s->band + j * panel.w + s->cx, panel.w))
            break;
        for (r = 0; r < n; r++)
            ps_bars(s, s->band + (j + r) * panel.w);
#else   /* no portrait bars without the player's posters: full width */
        if (!art_bcm_read(y, n, s->band + j * panel.w, panel.w))
            break;
#endif
    }
    if (y < panel.h || !art_bcm_sums_ok())
    {
        w->ps_bcm_bad = true;
        return -1;
    }
    return 1;
}

static bool bcm_eligible(const struct bwork *w, int type, int aaflags)
{
    /* JPEG as stored (an unsynchronised or base64 picture is not a JPEG
     * stream), the work marked, the job there and the BCM free: the
     * merged image's DECODE overlay loaded first (art_bcm_ready_ovl()) */
    return w->bcm && type == AA_TYPE_JPG && aaflags == 0
           && art_bcm_ready_ovl(bcm_cancel, (void *)w);
}

/* The BCM's work while the visualizer's view holds the BCM (sitting 4c):
 * deferred to its next chance (w->lit: the album, panel or render again
 * once the pass resumes), never the PP's (a panel there is 1-20 s of CPU
 * under the visualizer).  true: deferred */
static bool bcm_viz_defer(struct bwork *w, const char *what)
{
    if (!art_bcm_viz_holds())
        return false;
    w->lit = true;
    art_bcm_viz_defer(what);
    return true;
}

/* 1 the BCM decodes it, 0 deferred (w->lit), -1 the CPU's */
static int bcm_gate(struct bwork *w, int type, int aaflags, const char *what)
{
    bool jpeg = w->bcm && type == AA_TYPE_JPG && aaflags == 0;

    if (jpeg && bcm_viz_defer(w, what))
        return 0;
    if (bcm_eligible(w, type, aaflags))
        return 1;
    /* the view opened during the overlay's load (its cancel) */
    return jpeg && bcm_viz_defer(w, what) ? 0 : -1;
}

/* A verdict other than OK, as the CPU path's: 0 (decode_run()'s false),
 * or -1 for the CPU code below the splice */
static int bcm_other(struct bwork *w, enum art_bcm_verdict v, bool read_err)
{
    if (v == ART_BCM_BAD)
    {
        if (read_err)
            w->source_retry = true;     /* the PP's own read failed */
        return 0;                       /* the next source */
    }
    if (v == ART_BCM_DEFER)
    {
        w->lit = true;                  /* its next chance */
        return 0;
    }
    return -1;                          /* TWIN */
}

static uint32_t bcm_len(int fd, unsigned long len)
{
    return len ? (uint32_t)len : (uint32_t)ffilesize(fd);
}

/* The Now Playing box (fit): into the decode block at 0, where the
 * CPU's decode would leave it (np_compose() reads it there) */
static int bcm_fit(struct bwork *w, void *dbuf, int fd, off_t off,
                   unsigned long len, int type, int aaflags, int sw, int sh)
{
    struct art_bcm_out o;
    enum art_bcm_verdict v;
    bool rerr;
    int k;

    if ((k = bcm_gate(w, type, aaflags, "box")) <= 0)
        return k;
    memset(&o, 0, sizeof(o));
    o.dw = o.cw = w->fitted.w;
    o.dh = o.ch = w->fitted.h;
    o.kind = ART_BCM_BOX;
    o.key = w->bcm_key;
    v = art_bcm_decode(fd, off, bcm_len(fd, len), sw, sh, &o, bcm_cancel,
                       w, &rerr);
    if (v != ART_BCM_OK)
        return bcm_other(w, v, rerr);
    for (k = 0; k < 2; k++)         /* a mismatch: read once more */
    {
        art_bcm_rewind();
        if (!art_bcm_read(0, o.ch, (uint16_t *)dbuf, o.cw))
            break;
        if (art_bcm_sums_ok())
        {
            art_bcm_release();
            w->out_off = 0;
            UPDATE_TRACE("art bcm box key=" KEYFMT " w=%d h=%d reads=%d",
                         KEYARG(w->bcm_key), o.cw, o.ch, k + 1);
            return 1;
        }
    }
    art_bcm_release();
    UPDATE_TRACE("art bcm readback bad kind=%s", "box");
    return -1;
}

/* np-sharp's panel (fill, or a portrait picture's columns): decoded on
 * the BCM, then streamed into its slot as ps_decode() streams the CPU's
 * rows; -1: the CPU's ps_decode() */
static int bcm_panel(struct bwork *w, void *dbuf, int fd, off_t off,
                     unsigned long len, int type, int aaflags, int dw,
                     int dh, int sw, int sh)
{
    struct art_bcm_out o;
    enum art_bcm_verdict v;
    bool rerr, ok;

    long t0 = current_tick, t1;
    int g = art_bcm_panel_on() ? bcm_gate(w, type, aaflags, "panel") : -1;

    if (g == 0)
        return 0;                   /* the visualizer's: its next chance */
    if (g < 0)
    {
        if (w->ps_resident)
        {
            w->lit = true;          /* resident only: the BCM's or none */
            return 0;
        }
        return -1;                  /* the CPU decodes the panel */
    }
    memset(&o, 0, sizeof(o));
    o.dw = dw;
    o.dh = dh;
#ifdef HAVE_BCMFW_VIDEO
    o.cw = w->ps_cw ? w->ps_cw : panel.w;
#else
    o.cw = panel.w;
#endif
    o.ch = panel.h;
    o.cx = (dw - o.cw) / 2;         /* ps_decode()'s crop */
    o.cy = (dh - panel.h) / 2;
    o.kind = ART_BCM_PANEL;
    o.key = w->ps_key;
    if (w->ps_resident && o.cw != panel.w)
    {
        w->lit = true;              /* portrait: never resident (its bars) */
        return 0;
    }
    v = art_bcm_decode(fd, off, bcm_len(fd, len), sw, sh, &o, bcm_cancel,
                       w, &rerr);
    if (v != ART_BCM_OK)
    {
        if (w->ps_resident)
        {
            w->lit = true;          /* neither side: the file is there */
            return 0;
        }
        return bcm_other(w, v, rerr);
    }
    t1 = current_tick;
    /* the end state: the slot stays, the resident panel the drift and the
     * Now Playing row BLIT (no read-back on their path) */
    art_bcm_keep();
    if (w->ps_resident)
    {
        art_bcm_release();
        w->ps_res_ok = true;
        ui_log("art", "panel resident dec_ms=%ld",
               (long)(t1 - t0) * (1000 / HZ));
        return 1;
    }
    if (art_bcm_np_is_shown())
    {
        /* the row shows it now: the file's read-back waits for the
         * background (npr_place()), the render stays queued */
        art_bcm_release();
        w->lit = true;
        ui_log("art", "panel bcm dec_ms=%ld read=later",
               (long)(t1 - t0) * (1000 / HZ));
        return 0;
    }
    w->ps_bcm_bad = false;
    ok = ps_decode(w, dbuf, fd, off, len, type, aaflags, dw, dh, true);
    art_bcm_release();
    /* the device's A/B (bench-p19.md, "Later"): decode, read-back + write */
    ui_log("art", "panel bcm dec_ms=%ld read_ms=%ld ok=%d",
           (long)(t1 - t0) * (1000 / HZ),
           (long)(current_tick - t1) * (1000 / HZ), (int)ok);
    if (!ok && w->ps_bcm_bad)
    {
        w->ps_bcm_bad = false;
        UPDATE_TRACE("art bcm readback bad kind=%s", "panel");
        return -1;
    }
    return ok;
}
#endif /* HAVE_ART_BCM */

/* A panel: on the BCM when the work is marked and it can, else the
 * CPU's stream (ps_decode()) */
static bool ps_run(struct bwork *w, void *dbuf, int fd, off_t off,
                   unsigned long len, int type, int aaflags, int dw, int dh,
                   int sw, int sh)
{
#ifdef HAVE_ART_BCM
    int r;
    long t0;
    bool ok;

    if (w->lit)
        return false;               /* deferred already: no other source */
    r = bcm_panel(w, dbuf, fd, off, len, type, aaflags, dw, dh, sw, sh);
    if (r >= 0)
        return r > 0;
    if (w->ps_resident)
    {
        w->lit = true;              /* never the CPU's: nothing to write */
        return false;
    }
#else
    long t0;
    bool ok;

    (void)sw; (void)sh;
#endif
    t0 = current_tick;
    ok = ps_decode(w, dbuf, fd, off, len, type, aaflags, dw, dh, false);
    /* the CPU's decode + write, the A/B's other half (bench-p19.md) */
    ui_log("art", "panel cpu ms=%ld ok=%d",
           (long)(current_tick - t0) * (1000 / HZ), (int)ok);
    return ok;
}

/* Decodes the image at 'off' of 'fd' (len: embedded blob size, 0 for a
 * whole file) to the fill size of the target box (w->tw x w->th) and
 * centre-crops it into the slot, or (w->stream: a panel) streams the crop
 * into its panels.pak slot (ps_decode()). Baseline JPEG and BMP only.
 * 'dbuf': the decode buffer, pinned by decode() for this step only. */
static bool decode_run(struct bwork *w, void *dbuf, int fd, off_t off,
                       unsigned long len, int type, int aaflags);

/* decode_run() between gate points (art_gate()) */
static bool decode_pinned(struct bwork *w, void *dbuf, int fd, off_t off,
                          unsigned long len, int type, int aaflags)
{
    bool ok;

    gate_begin();
    ok = decode_run(w, dbuf, fd, off, len, type, aaflags);
    gate_end();
    return ok;
}

static bool decode_run(struct bwork *w, void *dbuf, int fd, off_t off,
                       unsigned long len, int type, int aaflags)
{
    struct bitmap bm;
    int sw, sh, dw, dh, rc, y;
    const int W = w->tw, H = w->th;
    const int fmt = FORMAT_NATIVE | FORMAT_DITHER | FORMAT_RESIZE;
    fb_data *pix;

    w->source_terminal = false;
    if (type != AA_TYPE_JPG && type != AA_TYPE_BMP)
    {
        w->source_terminal = true;
        return false;               /* PNG and unknown */
    }
    if (type == AA_TYPE_BMP && off != 0)
        return false;               /* read_bmp_fd() reads from 0 */

    /* 1: source size */
    rc = img_decode(dbuf, &bm, fd, off, len, type, aaflags, 0, 0,
                    FORMAT_NATIVE | FORMAT_RETURN_SIZE, NULL);
    sw = bm.width;
    sh = bm.height;
    if (type == AA_TYPE_JPG && rc == -4)
        w->source_terminal = true;   /* decoder's explicit unsupported SOF */
    if (rc <= 0 || sw <= 0 || sh <= 0)
        return false;               /* progressive JPEG, corrupt, ... */
    ART_PACE();                     /* (the decode block is pinned) */

    /* 2 (fit, D10): the long side fills the box, aspect kept, no crop;
     * the pixels stay where the decoder put them */
    if (w->fit)
    {
        artcache_fit_rect(sw, sh, W, H, &w->fitted);
#ifdef HAVE_ART_BCM
        if ((rc = bcm_fit(w, dbuf, fd, off, len, type, aaflags, sw, sh))
            >= 0)
            return rc > 0;
#endif
        rc = img_decode(dbuf, &bm, fd, off, len, type, aaflags,
                        w->fitted.w, w->fitted.h, fmt, NULL);
        if (rc <= 0 || bm.width != w->fitted.w || bm.height != w->fitted.h)
            return false;
        w->out_off = (char *)bm.data - (char *)dbuf;
        return true;
    }

    /* 2 (a thumbnail of non-square art, P6 owner): fitted on bars */
    if (!w->stream && thumb_fitted(w, dbuf, fd, off, len, type, aaflags,
                                     sw, sh, &rc))
        return rc > 0;

#ifdef HAVE_BCMFW_VIDEO
    /* 2 (a panel of portrait art, Phase 14a): fitted to the slot's height
     * in cw columns, centred; art narrower than 2:3 fills 2:3, cropped
     * top and bottom */
    if (w->stream && (w->ps_cw = panel_cw(sw, sh)) != 0)
    {
        dw = w->ps_cw;
        dh = MIN(MAX((long)H, ((long)sh * dw + sw - 1) / sw),
                 (long)FILL_MAX * H);
        return ps_run(w, dbuf, fd, off, len, type, aaflags, dw, dh, sw, sh);
    }
#endif

    /* 2: fill the box (crop, never letterbox), at most FILL_MAX x it */
    if ((long)sw * H >= (long)sh * W)
    {
        dh = H;
        dw = MIN(((long)sw * H + sh - 1) / sh, FILL_MAX * W);
    }
    else
    {
        dw = W;
        dh = MIN(((long)sh * W + sw - 1) / sw, FILL_MAX * H);
    }
    if (w->stream)
        return ps_run(w, dbuf, fd, off, len, type, aaflags, dw, dh, sw, sh);
    rc = img_decode(dbuf, &bm, fd, off, len, type, aaflags, dw, dh, fmt,
                    NULL);
    if (rc <= 0 || bm.width != dw || bm.height != dh)
        return false;

    /* 3: centre crop into the slot */
    pix = (fb_data *)bm.data + (dh - H) / 2 * dw + (dw - W) / 2;
    for (y = 0; y < H; y++)
        memcpy(slot_buf(w) + y * W * sizeof(fb_data), pix + y * dw,
               W * sizeof(fb_data));
    return true;
}

#ifdef HAVE_DEVSTATS
/* Fit-3 step 0: one decoded image into DEVSTATS_ART. The decoders read
 * through read() themselves, so "reads" are this thread's storage
 * commands (devstats_io), "read" time is the wall time off the core (the
 * DMA waits) and "decode" time the time on it (target: devstats' CPU
 * time, kept current to now); the simulator has neither clock and
 * records the wall ticks as decode time. */
struct art_ds { uint32_t us, cpu, cmds; int boost; };
static uint32_t art_ds_cpu(unsigned int slot)
{
#ifdef SIMULATOR
    (void)slot;
    return 0;
#else
    return slot >= DEVSTATS_SLOTS ? 0 : devstats.t[slot].cpu_us
           + (USEC_TIMER - devstats_last_us[CURRENT_CORE]);
#endif
}
static void art_ds_take(struct art_ds *d, unsigned int slot)
{
#ifdef SIMULATOR
    d->us = (uint32_t)current_tick * (1000000 / HZ);
#else
    d->us = USEC_TIMER;
#endif
    d->cpu = art_ds_cpu(slot);
    d->cmds = slot < DEVSTATS_SLOTS ? devstats.t[slot].cmds : 0;
    d->boost = ART_BOOST();
}
static void art_ds_add(const struct art_ds *a, unsigned int slot, int fd,
                       unsigned long len)
{
    struct art_ds b;
    uint32_t wall, cpu;

    if (slot >= DEVSTATS_SLOTS)
        return;
    art_ds_take(&b, slot);
    wall = b.us - a->us;
    cpu = b.cpu - a->cpu;
#ifdef SIMULATOR
    cpu = wall;
#endif
    if (cpu > wall)
        cpu = wall;
    DEVSTATS_ART(len ? len : (uint32_t)ffilesize(fd), b.cmds - a->cmds,
                 wall - cpu, cpu, b.boost > a->boost ? b.boost - a->boost : 0);
}
#endif /* HAVE_DEVSTATS */

static bool decode(struct bwork *w, int fd, off_t off, unsigned long len,
                   int type, int aaflags)
{
    void *dbuf = core_get_data_pinned(w->dec);
#ifdef HAVE_DEVSTATS
    unsigned int ds_slot = thread_self() & 0xff;    /* THREAD_ID_SLOT */
    struct art_ds ds;
    art_ds_take(&ds, ds_slot);
#endif
    int st = audio_status();
    bool ok;

    /* stopped, or paused on the charger or with the user idle (short
     * bursts that let the disk sleep, owner 2026-10-19): never while
     * audio plays (#58) */
    resize_boost_cap(!w->no_boost
                     && (!st || ((st & AUDIO_STATUS_PAUSE)
                                 && (charger_inserted() || user_idle())))
                     ? DECODE_BOOST : 0);
    if (npr_busy)
        npr_src_fd = fd;            /* art_gate() may call the render off */
    ok = decode_pinned(w, dbuf, fd, off, len, type, aaflags);
    npr_src_fd = -1;
    resize_boost_cap(0);

#ifdef HAVE_DEVSTATS
    if (ok)
        art_ds_add(&ds, ds_slot, fd, len);
#endif
    core_put_data_pinned(dbuf);
    return ok;
}

static bool write_slot(struct bwork *w, artcache_key_t key, uint32_t slot)
{
    size_t n = (size_t)thumb.w * thumb.h * 2;
    struct artpak_tag tag;
    off_t off = artpak_slot_offset(slot_bytes, slot);
    int fd;
    bool ok;

    /* the tag follows the pixels at w*h*2 (6,050 for 55 px: 2-byte
     * aligned), so it is copied in, never stored through a struct pointer
     * (an ARM7TDMI word store to an unaligned address writes the aligned
     * word below it: UBSan, H1) */
    memset(slot_buf(w) + n, 0, slot_bytes - n);
    tag.key = key;
    tag.pix_crc = artfmt_crc_end(artfmt_crc(ARTFMT_CRC_INIT, slot_buf(w), n));
    tag.magic = ARTPAK_TAG_MAGIC;
    memcpy(slot_buf(w) + n, &tag, sizeof(tag));
    ART_PACE();
    if (!io_go(w, "write", key))
        return false;
    fd = open(F_PAK, O_WRONLY);
    if (fd < 0)
        return false;
    ok = lseek(fd, off, SEEK_SET) == off
         && write(fd, slot_buf(w), slot_bytes) == (ssize_t)slot_bytes;
    return close(fd) == 0 && ok;    /* close() flushes: complete on disk
                                       before any index points at it */
}

/* Tries one source; fills the fingerprint. */
static bool try_file(struct bwork *w, struct artcache_rec *rec)
{
    const char *ext = strrchr(w->src, '.');
    bool ok;
    int fd;

    ART_PACE();
    fd = open(w->src, O_RDONLY);
    if (fd < 0)
    {
        w->source_retry = true;
        return false;
    }
    rec->src_kind = ARTSRC_FILE;
    rec->src_type = (ext && !strcasecmp(ext, ".bmp")) ? AA_TYPE_BMP
                                                      : AA_TYPE_JPG;
    {
        off_t bytes = ffilesize(fd);

        if (bytes < 0 || (uint64_t)bytes > UINT32_MAX)
        {
            close(fd);
            w->source_retry = true;
            return false;
        }
        rec->src_size = bytes;
    }
    rec->src_hash = artkey_fnv32(w->src);
    rec->src_crc = src_crc(w, fd, 0, rec->src_size, true);
    ART_PACE();
    ok = decode(w, fd, 0, 0, rec->src_type, 0);
    rec->src_type |= ARTREC_T_SIG2;
    if (!ok && !w->source_terminal)
        w->source_retry = true;
    if (close(fd) < 0)
    {
        w->source_retry = true;
        ok = false;
    }
    return ok;
}

static bool try_embedded(struct bwork *w, int fd, struct artcache_rec *rec)
{
    const struct mp3_albumart *aa = &w->id3.albumart;

    rec->src_kind = ARTSRC_EMBEDDED;
    rec->src_type = aa->type & AA_CLEAR_FLAGS_MASK;
    rec->src_size = aa->size;
    rec->src_hash = artkey_fnv32(w->path);
    rec->src_crc = src_crc(w, fd, aa->pos, aa->size, true);
    ART_PACE();
    {
        bool ok = decode(w, fd, aa->pos, aa->size, rec->src_type,
                          aa->type & ~AA_CLEAR_FLAGS_MASK);

        rec->src_type |= ARTREC_T_SIG2;

        if (!ok && !w->source_terminal)
            w->source_retry = true;
        return ok;
    }
}

/* Unchanged image-file source (or still none)? For albums whose tags did
 * not change: embedded art can't have changed, a folder image can. */
static bool source_unchanged(struct bwork *w, const struct artcache_rec *old)
{
    bool prefer_emb = global_settings.album_art == AA_PREFER_EMBEDDED;
    bool found;

#ifdef HAVE_BCMFW_VIDEO
    if (w->vid)
        prefer_emb = true;          /* a poster: its own covr first */
#endif
    int fd;
    off_t size;

    ART_PACE();
    found = find_file(w, true);
    ART_PACE();
    if (old->src_kind == ARTSRC_EMBEDDED)
        return prefer_emb || !found;    /* a new file would win */
    if (old->src_kind != ARTSRC_FILE)
        return !found;                  /* NOART: still none */
    if (!found || artkey_fnv32(w->src) != old->src_hash)
        return false;
    fd = open(w->src, O_RDONLY);
    if (fd < 0)
        return false;
    size = ffilesize(fd);
    close(fd);
    return size == (off_t)old->src_size;
}

/* #90: the lazy signature fill's cap per maintenance window. A window
 * is one stretch of regroup_window() (the paused window, or stopped and
 * idle after the boot's first minutes) between two user actions or two
 * pauses: its epoch is the pair (last input, pause start). */
#define SIG_FILL_PER_WINDOW 16

static bool sig_new_window(void)
{
    return last_input != sig_epoch_input
           || maint_paused_since() != sig_epoch_pause;
}

static bool sig_fill_ok(void)
{
    if (!regroup_window())
        return false;
    if (sig_new_window())
    {
        sig_epoch_input = last_input;   /* a new window: a new cap */
        sig_epoch_pause = maint_paused_since();
        sig_win_n = 0;
    }
    return sig_win_n < SIG_FILL_PER_WINDOW;
}

/* a walk is owed for the fill, and a new window is open */
static bool sig_due(void)
{
    return sig_owed && regroup_window() && sig_new_window();
}

static bool np_stale(const struct artcache_rec *r);
static bool np_bundle(struct bwork *w, artcache_key_t key,
                      const struct artcache_rec *rec);

/* #90: the signature of the source a build would try first (the order
 * of source_decode()) into 'now' (kind, hash, size, and the CRC in the
 * style 'sig2' asks, src_crc()): one metadata read of the audio file and
 * 16 KB of reads, no
 * decode. Leaves w->src / w->id3 as source_open() wants them. False when
 * the track can't be read or that first source is not of kind 'want'
 * (another source would win: the caller rebuilds). */
static bool source_sig(struct bwork *w, int want, bool sig2,
                       struct artcache_rec *now)
{
    bool file, emb, file_first, ok = true;
    int fd;

    ART_PACE();
    fd = open(w->path, O_RDONLY);
    w->source_retry = false;
    if (fd < 0)
        return false;
    if (!get_metadata(&w->id3, fd, w->path))
    {
        close(fd);
        return false;
    }
    ART_PACE();
    file = find_file(w, false);
    ART_PACE();
    emb = w->id3.has_embedded_albumart;
    file_first = global_settings.album_art != AA_PREFER_EMBEDDED;
#ifdef HAVE_BCMFW_VIDEO
    if (w->vid)
        file_first = false;
#endif
    memset(now, 0, sizeof(*now));
    if (file && (file_first || !emb))
    {
        int sfd;
        off_t bytes;

        now->src_kind = ARTSRC_FILE;
        if (want == ARTSRC_FILE)
        {
            sfd = open(w->src, O_RDONLY);
            bytes = sfd >= 0 ? ffilesize(sfd) : -1;
            ok = bytes > 0 && (uint64_t)bytes <= UINT32_MAX;
            if (ok)
            {
                now->src_size = bytes;
                now->src_hash = artkey_fnv32(w->src);
                now->src_crc = src_crc(w, sfd, 0, now->src_size, sig2);
            }
            if (sfd >= 0)
                close(sfd);
        }
    }
    else if (emb)
    {
        now->src_kind = ARTSRC_EMBEDDED;
        if (want == ARTSRC_EMBEDDED)
        {
            now->src_size = w->id3.albumart.size;
            now->src_hash = artkey_fnv32(w->path);
            now->src_crc = src_crc(w, fd, w->id3.albumart.pos,
                                   now->src_size, sig2);
        }
    }
    close(fd);
    if (sig2)
        now->src_type = ARTREC_T_SIG2;
    return ok && now->src_kind == want && !w->source_retry;
}

/* #90: the album's tags changed (another max_mtime) but its first source
 * has the record's signature: the record stays (thumbnail, bundle, panel
 * images all come from that source), with the new mtime. A book's
 * chapter pass runs again (its chapters follow the tags). 1 = kept (in
 * *rec), 0 = rebuild, -1 / ALBUM_LIT as album_try(). */
static int sig_keep(struct bwork *w, const struct artcache_rec *old,
                    const struct artalb_rec *t, struct artcache_rec *rec)
{
    struct artcache_rec now;

    if (!source_sig(w, old->src_kind, (old->src_type & ARTREC_T_SIG2) != 0,
                    &now)
        || !artrec_sig_same(old, &now))
        return 0;
    *rec = *old;
    rec->mtime = t->max_mtime;
    if (old->flags & ARTREC_F_CHAPTERS)
    {
        rec->flags &= ~ARTREC_F_CHAPTERS;
        chap_stale = true;          /* its chapter pass runs again */
    }
    if (np_stale(old))
    {
        if (!np_bundle(w, old->key, old))
        {
            if (w->lit)
                return ALBUM_LIT;
            w->failed = true;
            return -1;
        }
        rec->flags = NP_TAGGED(rec->flags);
    }
    w->sig_kept++;
    UPDATE_TRACE("art sig keep key=" KEYFMT " src=%d", KEYARG(old->key),
                 old->src_kind);
    return 1;
}

/* #90: a record that should carry a signature and does not: its source's
 * signature, read now (lazy fill, maintenance windows only, at most
 * SIG_FILL_PER_WINDOW a window). 1 = filled (in *rec), 0 = left as it is
 * (trusted: another source would win, or unreadable), -1 / ALBUM_LIT. */
static int sig_fill(struct bwork *w, const struct artcache_rec *old,
                    const struct artalb_rec *t, struct artcache_rec *rec)
{
    struct artcache_rec now;

    w->sig_tried++;
    sig_win_n++;
    if (!io_go(w, "read", t->key))
        return ALBUM_LIT;
    if (!resolve(w, t))
        return -1;
    *rec = *old;
    memset(&now, 0, sizeof(now));
    if (!source_sig(w, old->src_kind, true, &now) && w->source_retry)
        return 0;                   /* a read failed (transient: L5): a
                                       later window tries again */
    if (now.src_kind != old->src_kind
        || now.src_hash != old->src_hash || !now.src_size || !now.src_crc
        || (old->src_size && now.src_size != old->src_size))
    {
        /* #90 review M1: never signable as it stands (moved, unreadable,
         * another source would win): tried, trusted, never walked for
         * again (sig_owed counts only artrec_sig_missing() records) */
        rec->src_type |= ARTREC_T_UNSIGNED;
        UPDATE_TRACE("art sig unsigned key=" KEYFMT " src=%d",
                     KEYARG(old->key), old->src_kind);
        return 1;
    }
    rec->src_size = now.src_size;
    rec->src_crc = now.src_crc;
    rec->src_type |= ARTREC_T_SIG2;
    UPDATE_TRACE("art sig fill key=" KEYFMT " src=%d", KEYARG(old->key),
                 old->src_kind);
    return 1;
}

/* A folder image the stock search does not look for (PNG: Phase 7b's Mac
 * tool converts it): FAILED, not NOART. Only asked when there is no
 * other source; two lookups. */
static bool png_folder(struct bwork *w)
{
    static const char *const names[] = { "cover.png", "folder.png" };
    char *slash = strrchr(w->path, '/');
    size_t dir = slash ? (size_t)(slash - w->path) + 1 : 0;
    unsigned i;

    for (i = 0; i < ARRAYLEN(names); i++)
    {
        if (dir + strlen(names[i]) >= sizeof(w->src))
            return false;
        memcpy(w->src, w->path, dir);
        strcpy(w->src + dir, names[i]);
        ART_PACE();
        if (file_exists(w->src))
            return true;
    }
    return false;
}

/* Opens the representative track (w->path), finds its art in the stock
 * order (if the first source won't decode, the other one) and decodes it
 * to the current target (decode()), filling rec's source fingerprint.
 * Returns -1 if the track can't be opened, else the state: ARTREC_OK
 * (decoded), ARTREC_NOART or ARTREC_FAILED. */
static int source_decode(struct bwork *w, struct artcache_rec *rec)
{
    bool file_first, ok = false, file, emb;
    int fd;

    w->source_terminal = false;
    w->source_retry = false;
    ART_PACE();
    fd = open(w->path, O_RDONLY);
    if (fd < 0)
        return -1;
    if (!get_metadata(&w->id3, fd, w->path))
    {
        close(fd);
        return -1;                  /* a metadata read failure is not absence */
    }
    ART_PACE();
    file = find_file(w, false);
    ART_PACE();
    emb = w->id3.has_embedded_albumart;
    file_first = global_settings.album_art != AA_PREFER_EMBEDDED;
#ifdef HAVE_BCMFW_VIDEO
    /* Phase 14a (3.5): a video's own covr first, then the image files (a
     * folder.jpg in a shared Movies folder never hides a poster) */
    if (w->vid)
        file_first = false;
#endif
    if (file && file_first)
        ok = try_file(w, rec);
    if (!ok && emb)
        ok = try_embedded(w, fd, rec);
    if (!ok && file && !file_first)
        ok = try_file(w, rec);
    close(fd);
    if (!ok && w->source_retry)
        return -1;
    if (!file && !emb)
    {
        w->source_terminal = png_folder(w);
        return w->source_terminal ? ARTREC_FAILED : ARTREC_NOART;
    }
    return ok ? ARTREC_OK : ARTREC_FAILED;
}

/* Opens the source 'rec' names (the one source_decode() used: w->src,
 * or w->path's embedded art in w->id3). Returns the fd, < 0 if gone. */
static int source_open(struct bwork *w, const struct artcache_rec *rec,
                       off_t *off, unsigned long *len, int *aaflags)
{
    *off = 0;
    *len = 0;
    *aaflags = 0;
    ART_PACE();
    if (rec->src_kind == ARTSRC_FILE)
        return open(w->src, O_RDONLY);
    if (rec->src_kind != ARTSRC_EMBEDDED)
        return -1;
    *off = w->id3.albumart.pos;
    *len = w->id3.albumart.size;
    *aaflags = w->id3.albumart.type & ~AA_CLEAR_FLAGS_MASK;
    return open(w->path, O_RDONLY);
}

/* QW2 (docs/phase11-brief.md §3.9): the fitted image at 'fit' composed
 * the way Now Playing shows it, the slanted card and its reflection
 * against white (np_art_render(), the same code the screen ran per track
 * before), into the decode block next to it: before it if it starts far
 * enough in, else after it (fitted 40,328 + composed 51,238 = 91,566 B
 * of the >= 98,588 B block). NULL if neither fits (never on the target). */
#define NP_COMPOSE_COLS 32          /* slant columns between gate points */
static np_px *np_compose(struct bwork *w, char *dbuf)
{
    const size_t comp_bytes = NP_COMPOSED_W * NP_COMPOSED_H * sizeof(np_px);
    size_t fit_bytes = (size_t)w->fitted.w * w->fitted.h * sizeof(fb_data);
    size_t at = w->out_off >= comp_bytes ? 0
              : ALIGN_UP(w->out_off + fit_bytes, 4);
    struct np_canvas cv;
    np_px *comp;
    int c;

    if (at + comp_bytes > w->dec_bytes)
    {
        UPDATE_TRACE("art np compose no room off=%lu fit=%lu dec=%lu",
                     (unsigned long)w->out_off, (unsigned long)fit_bytes,
                     (unsigned long)w->dec_bytes);
        return NULL;
    }
    comp = (np_px *)(dbuf + at);
    memset(comp, 0xff, comp_bytes);             /* NP_WHITE */
    np_comp_canvas(&cv, comp);
    /* in column chunks, a gate point between them (art_gate()) */
    gate_begin();
    for (c = 0; c < NP_SLANT_W; c += NP_COMPOSE_COLS)
    {
        np_art_render_cols(&cv, (const np_px *)(dbuf + w->out_off),
                           w->fitted.w, w->fitted.h, NP_WHITE, c,
                           MIN(c + NP_COMPOSE_COLS, NP_SLANT_W));
        yield();
        art_gate();
    }
    gate_end();
    return comp;
}

/* Decodes the open source at the Now Playing fit size (D10) into the
 * decode block, composes it (np_compose()) and writes that as the bundle
 * of 'key', version 2 (header scratch: the slot buffer, free once the
 * thumbnail is written). The block stays pinned across the write, which
 * yields. */
static bool np_write(struct bwork *w, artcache_key_t key, int fd, off_t off,
                     unsigned long len, int type, int aaflags)
{
    struct artcache_dim box = artcache_sizes[ARTCACHE_NOWPLAYING];
    struct artbundle_img im;
    char path[ART_BPATH];
    void *dbuf;
    np_px *comp = NULL;
    bool ok;

    w->fit = true;
    w->tw = box.w;
    w->th = box.h;
#ifdef HAVE_ART_BCM
    w->bcm = true;                  /* 19-6: the box may decode on the BCM */
    w->bcm_key = key;
#endif
    dbuf = core_get_data_pinned(w->dec);
    ok = decode_pinned(w, dbuf, fd, off, len, type, aaflags);
    ART_PACE();
    /* the box's CRC: the same on either side (19-6; trace only) */
    if (ok)
        UPDATE_TRACE("art np box key=" KEYFMT " w=%d h=%d crc=%08lx",
                     KEYARG(key), w->fitted.w, w->fitted.h,
                     (unsigned long)artfmt_crc_end(artfmt_crc(
                         ARTFMT_CRC_INIT, (char *)dbuf + w->out_off,
                         (size_t)w->fitted.w * w->fitted.h * 2)));
    ART_PACE();
    if (ok)
        ok = (comp = np_compose(w, dbuf)) != NULL;
    ART_PACE();
    if (ok)
    {
        bundle_path(key, path);
        path[sizeof(ARTCACHE_DIR) + 2] = '\0';     /* "<dir>/XX" */
        if (!dir_exists(path))
            mkdir(path);
        path[sizeof(ARTCACHE_DIR) + 2] = '/';
        ok = io_go(w, "write", key);
    }
    if (ok)
    {
        memset(&im, 0, sizeof(im));
        im.size_id = ARTCACHE_NOWPLAYING;
        im.format = ARTBUNDLE_FMT_NPCOMPOSED;
        im.w = NP_COMPOSED_W;
        im.h = NP_COMPOSED_H;
        im.reserved = NP_LAYOUT_HASH;
        ART_PACE();
        ok = artbundle_write_img_buffered(path, key,
                ARTBUNDLE_VERSION_COMPOSED, &im, comp, slot_buf(w), slot_bytes);
    }
    core_put_data_pinned(dbuf);
#ifdef HAVE_ART_BCM
    w->bcm = false;
#endif
    w->fit = false;
    w->tw = thumb.w;
    w->th = thumb.h;
    return ok;
}

/* The album's Now Playing bundle, from its thumbnail's source. A failed
 * replacement retains the old bundle and its retry obligation. */
static bool np_bundle(struct bwork *w, artcache_key_t key,
                      const struct artcache_rec *rec)
{
    off_t off;
    unsigned long len;
    int aaflags, fd;
    bool ok;

    if (!io_go(w, "read", key))
    {
        w->lit = true;              /* the album is redone once idle */
        return false;
    }
    fd = source_open(w, rec, &off, &len, &aaflags);
    if (fd < 0)
        return false;
    ok = np_write(w, key, fd, off, len, artrec_src_type(rec), aaflags);
    close(fd);
    if (!ok && !w->lit)
    {
        w->failed = true;
        UPDATE_TRACE("art retain bundle key=" KEYFMT, KEYARG(key));
    }
    return ok;
}

/* QW2, the upgrade predicate (checklist item 13): an album record with
 * art whose Now Playing bundle is not composed for this layout (a fitted
 * version 1 bundle from before Phase 11, tag 0, or another layout's).
 * Chapter bundles follow their book (np_upgrade()). */
static bool np_stale(const struct artcache_rec *r)
{
    return r->state == ARTREC_OK && !artrec_is_chapter(r)
           && artrec_np_tag(r) != NP_TAG;
}

/* QW2: composes the bundle of an album whose source is unchanged
 * (source_unchanged() found it: w->src for a file; the audio file's
 * embedded art is located again here): one source read, one bundle write,
 * no thumbnail work. The record is the old one, tagged; a book's
 * ARTREC_F_CHAPTERS goes, so its chapter pass (only while stopped)
 * composes its chapter bundles too. Returns 1 (a delta) or -1. */
static int np_upgrade(struct bwork *w, const struct artcache_rec *old,
                      struct artcache_rec *rec)
{
    if (old->src_kind == ARTSRC_EMBEDDED)
    {
        int fd = open(w->path, O_RDONLY);
        bool ok;

        if (fd < 0)
            return -1;
        ok = get_metadata(&w->id3, fd, w->path)
             && w->id3.has_embedded_albumart;
        close(fd);
        if (!ok)
            return -1;
    }
    *rec = *old;
    if (!np_bundle(w, old->key, old))
    {
        if (w->lit)
            return ALBUM_LIT;
        w->failed = true;
        return -1;                  /* do not mark a failed layout current */
    }
    rec->flags = NP_TAGGED(rec->flags & ~ARTREC_F_CHAPTERS);
    UPDATE_TRACE("art np compose key=" KEYFMT " src=%d",
                 KEYARG(old->key), old->src_kind);
    return 1;
}

/* A slot album() took and no record will name (a failed or refused
 * write): free again, and never claim an unwritten append */
static void slot_unclaim(struct bwork *w, uint32_t slot)
{
    w->freemap[slot / 32] &= ~(1u << (slot % 32));
    if (slot + 1 == w->next_slot)
        w->next_slot--;
}

static int album_try(struct bwork *w, const struct artalb_rec *t,
                     struct artcache_rec *rec)
{
    struct artcache_rec old;
    bool have_old, local_repair = false, io_err;
    int st;

    for (st = 0; st < ART_HINTS; st++)
        if (repair_keys[st] == t->key)
            local_repair = true;
    ART_PACE();
    have_old = artidx_lookup(&w->rd, t->key, &old, &io_err);
    if (io_err)
    {
        w->failed = true;
        return -1;
    }
    if (have_old)
        w->matched++;
    ART_PACE();
    if (have_old && !local_repair && old.mtime == t->max_mtime && !w->sweep
        && !np_stale(&old))
    {
        /* #90: a record without a signature is trusted; a few get one
         * per maintenance window, never at boot */
        if (artrec_sig_missing(&old) && w->sig_missing++ < UINT16_MAX - 1
            && sig_fill_ok())
            return sig_fill(w, &old, t, rec);
        return 0;                               /* up to date, no I/O */
    }
#ifdef HAVE_ART_BCM
    if (bcm_viz_defer(w, "album"))
        return ALBUM_LIT;                       /* after the visualizer */
#endif
    if (!io_go(w, "read", t->key))
        return ALBUM_LIT;                       /* again once idle */
    if (!resolve(w, t))
        return -1;                              /* database moved on */
    ART_PACE();
    if (have_old && !local_repair && old.mtime != t->max_mtime
        && artrec_sig_valid(&old))
    {
        /* #90: tags changed; the art only if its signature did */
        st = sig_keep(w, &old, t, rec);
        if (st)
            return st;
    }
    if (have_old && !local_repair && old.mtime == t->max_mtime
        && source_unchanged(w, &old))
        return np_stale(&old) ? np_upgrade(w, &old, rec)
                              : 2;              /* checked: unchanged */

    memset(rec, 0, sizeof(*rec));
    rec->key = t->key;
    rec->mtime = t->max_mtime;
    rec->slot = ARTREC_NO_SLOT;
    if (have_old && (old.flags & ARTREC_F_CHAPTERS))
    {
        if (old.mtime == t->max_mtime)
            rec->flags = ARTREC_F_CHAPTERS; /* only the cover file changed */
        else
            chap_stale = true;      /* its chapter pass runs again */
    }
    ART_PACE();
    st = source_decode(w, rec);
    ART_PACE();
    if (st < 0 || (st == ARTREC_FAILED && !w->source_terminal))
    {
        w->failed = true;
        return -1;
    }
    if (st != ARTREC_OK && have_old && old.state == ARTREC_OK)
    {
        UPDATE_TRACE("art retain source key=" KEYFMT " state=%d",
                     KEYARG(t->key), st);
        return 2;                   /* prefer a usable old cover to none */
    }
    if (st != ARTREC_OK)
        rec->state = st;
    else
    {
        rec->slot = alloc_slot(w);
        if (rec->slot == ARTREC_NO_SLOT)
        {
            w->failed = true;
            return -1;
        }
        if (!write_slot(w, t->key, rec->slot))
        {
            slot_unclaim(w, rec->slot);         /* nothing points at it */
            if (!w->lit)
                w->failed = true;
            return w->lit ? ALBUM_LIT : -1;
        }
        rec->state = ARTREC_OK;
        ART_PACE();
        if (!np_bundle(w, t->key, rec))
        {
            if (w->lit)
            {
                /* the user came back: no record, the slot reused, the
                 * whole album again once idle (never a record without
                 * its bundle tried) */
                slot_unclaim(w, rec->slot);
                return ALBUM_LIT;
            }
            w->failed = true;
        }
        else
            rec->flags = NP_TAGGED(rec->flags);
    }
    if (rec->state != ARTREC_OK)
    {
        if (!io_ok())
        {
            w->lit = true;          /* no record: again once idle */
            return ALBUM_LIT;
        }
    }
    return 1;
}

/* One album: 1 a new record in *rec, 2 checked (unchanged), 0 up to
 * date, -1 failed or the database moved on (the next pass), or ALBUM_LIT:
 * the user came back while audio plays, before its source read or a
 * write (#58 idle build): nothing kept, the caller waits for the next
 * idle and does it again. */
static int album(struct bwork *w, const struct artalb_rec *t,
                 struct artcache_rec *rec)
{
    uint32_t matched = w->matched;
    int rc;

    w->lit = false;
    rc = album_try(w, t, rec);
    if (rc == ALBUM_LIT)
        w->matched = matched;       /* counted again on the retry */
    return rc;
}

/* Next table record with a key, NULL at the end; *err on a replaced
 * table. */
static const struct artalb_rec *next_album(struct bwork *w, bool *err)
{
    *err = false;
    for (;;)
    {
        if (w->ti >= w->tn)
        {
            int n;

            w->tfirst += w->tn;
            w->tn = w->ti = 0;
            ART_PACE();
            n = artalb_read(w->tpath, &w->thdr, w->tfirst, TCHUNK, w->trec);
            ART_PACE();
            if (n < 0)
                *err = true;
            if (n <= 0)
                return NULL;
            w->tn = n;
        }
        if (w->trec[w->ti].key)
            return &w->trec[w->ti++];
        w->ti++;                    /* no key: the note glyph, no art */
        status.done++;
    }
}

/* ======================================================================= */
/* builder: panel art (H1)                                                 */
/* ======================================================================= */
/* A bad read is never an instruction to truncate panels.pak. The previous
 * published copy may be recovered, while new writes use a private file.
 * #117: a published file is then written in place (w->panel_live): a
 * header whose own fields are this build's but whose CRC fails is a
 * header write a power cut tore (panel_commit()); with no previous copy
 * to fall back on, the set starts empty over the same file (no
 * truncation: its slots are rewritten as the pass rebuilds them). */
/* The file's last byte reads: its cluster chain reaches the size its
 * directory entry gives. Sitting 4c's panels.pak had a good header and a
 * size of 5,407,744 B on a chain cut after 32 KB (a cut write: the size
 * updated, the chain never completed; the device's read-only fsck), so
 * every slot past the cut failed to read or write and the PP's panel
 * builds failed (ok=0) pass after pass. One seek and one read; the FAT
 * walks the chain to it (a few FAT sectors). */
static bool file_tail_reads(int fd, off_t bytes)
{
    char c;

    return bytes <= 0
           || (lseek(fd, bytes - 1, SEEK_SET) == bytes - 1
               && read(fd, &c, 1) == 1);
}

static bool panel_load(struct bwork *w)
{
    struct artpanel_hdr *h = &w->phdr;
    int fd = open(F_PANELS, O_RDONLY);
    bool ok = false, torn = false, cut = false;
    int error = fd < 0 ? errno : 0;

    w->panel_pending = false;
    w->panel_live = false;
    w->panel_fresh = false;
    if (fd >= 0)
    {
        ssize_t got = read(fd, h, sizeof(*h));
        off_t bytes = ffilesize(fd);
        int closed;

        ok = got == (ssize_t)sizeof(*h)
             && artpanel_hdr_ok(h, panel.w, panel.h)
             && bytes >= (off_t)artpanel_slot_offset(panel_slot_bytes,
                                                    h->n_slots);
        cut = ok && !file_tail_reads(fd, bytes);
        closed = close(fd);
        if (cut)
        {
            ok = false;
            UPDATE_TRACE("art panels %s", "chain cut: tail unreadable");
            ui_log("art", "panels: file shorter on disk than its size "
                   "(cut chain): recovered or rebuilt");
        }
        torn = !ok && !cut && got == (ssize_t)sizeof(*h)
               && h->magic == ARTPANEL_MAGIC
               && h->version == ARTPANEL_VERSION
               && h->w == panel.w && h->h == panel.h
               && h->slot_bytes == panel_slot_bytes;
        if (got < 0 || bytes < 0 || closed < 0
            || (got != (ssize_t)sizeof(*h) && bytes >= (off_t)sizeof(*h)))
            goto failed;
    }
    if (ok)
    {
        w->panel_live = true;
        return true;
    }
    if (error && error != ENOENT)
        goto failed;
    ART_PACE();
    fd = open(F_PANELS_PREV, O_RDONLY);
    if (fd >= 0)
    {
        off_t bytes = ffilesize(fd);

        ok = read(fd, h, sizeof(*h)) == (ssize_t)sizeof(*h)
             && artpanel_hdr_ok(h, panel.w, panel.h)
             && bytes >= (off_t)artpanel_slot_offset(
                    panel_slot_bytes, h->n_slots)
             && file_tail_reads(fd, bytes);
        ok = close(fd) == 0 && ok;
        if (ok && artfile_replace(F_PANELS_PREV, F_PANELS) == 0)
        {
            panel_seq++;
            w->panel_live = true;
            if (cut)
                UPDATE_TRACE("art panels %s", "chain cut: prev recovered");
            return true;
        }
        if (!cut)
            goto failed;
    }
    if (cut)
    {
        /* no good previous copy: the set starts over in a new file (the
         * private copy, empty), published over the cut one; never
         * written in place, never copied from */
        artpanel_hdr_init(h, panel.w, panel.h);
        w->panel_fresh = true;
        UPDATE_TRACE("art panels %s", "chain cut: set rebuilt");
        return true;
    }
    if (torn && errno == ENOENT)
    {
        artpanel_hdr_init(h, panel.w, panel.h);
        w->panel_live = true;       /* the next save rewrites the header */
        UPDATE_TRACE("art panels header %s", "torn: rebuilt in place");
        ui_log("art", "panels header torn: set rebuilt");
        return true;
    }
    if (error == ENOENT && errno == ENOENT)
    {
        artpanel_hdr_init(h, panel.w, panel.h);
        return true;                /* lazy creation, no published file */
    }
failed:
    FAIL(w);
    UPDATE_TRACE("art retain panels %s", "unavailable");
    return false;
}

/* Copy only when something changes, using existing pinned scratch. Warm
 * checks do no writes. Hold descriptors locally and observe USB/cancellation
 * without acknowledging ownership until both descriptors have been closed. */
static bool panel_stage(struct bwork *w)
{
    int in, out;
    bool ok;
    ssize_t n = 0;
    long copied = 0;

    (void)copied;                   /* trace only */
    if (w->panel_pending)
        return true;
    if (!io_go(w, "write", 0))
        return false;
    ART_PACE();
    in = w->panel_fresh ? -1 : open(F_PANELS, O_RDONLY);
    if (in < 0 && !w->panel_fresh && errno != ENOENT)
        goto failed;
    ART_PACE();
    out = artfile_tmp_clear(F_PANELS_TMP, F_PANELS)     /* #91 H1 */
        ? open(F_PANELS_TMP, O_WRONLY | O_CREAT | O_TRUNC, 0666) : -1;
    if (out < 0)
    {
        if (in >= 0)
            close(in);
        goto failed;
    }
    ok = true;
    if (in >= 0)
    {
        while (ok)
        {
            if (!io_ok() || usb_quiescing || usb_inserted()
                || art_q_stops())
            {
                w->lit = true;
                ok = false;
                break;
            }
            ART_PACE();
            n = read(in, w->page, sizeof(w->page));
            if (n <= 0)
                break;
            ok = write(out, w->page, n) == n;
            copied += n;
        }
        ok = close(in) == 0 && n == 0 && ok;
    }
    else
    {
        memset(w->page, 0, ARTPANEL_HDR_BYTES);
        memcpy(w->page, &w->phdr, sizeof(w->phdr));
        ok = write(out, w->page, ARTPANEL_HDR_BYTES) == ARTPANEL_HDR_BYTES;
    }
    ok = close(out) == 0 && ok;
    if (!ok)
        goto failed;
    UPDATE_TRACE("art panel stage copied=%ld", copied);
    w->panel_pending = true;
    return true;
failed:
    if (!w->lit)
        FAIL(w);
    return false;
}

static void pool_follow(struct bwork *w);
/* #117: the header of the published panels.pak rewritten in place (its
 * 792 bytes at 0, then fsync), after the slot it names is on disk
 * (ps_decode() fsyncs it). Replaces the copy of the whole file (up to
 * 5.77 MB) that every built panel used to cost. A power cut inside this
 * write leaves a header whose CRC fails: readers then use
 * panels.pak.prev, and panel_load() recovers it, or starts the set over
 * when there is none. Every entry names a slot whose pixels and tag were
 * on disk before the header named it, so no state of the file a reader
 * can pass names a slot that is half written. */
static bool panel_commit(struct bwork *w)
{
    int fd;
    bool ok;

    if (!io_go(w, "write", 0))
        return false;               /* the user came back: the slot stays
                                       unnamed, the next pass redoes it */
    ART_PACE();
    fd = open(F_PANELS, O_WRONLY);
    if (fd < 0)
        goto failed;
    w->phdr.version = ARTPANEL_VERSION;
    artpanel_hdr_seal(&w->phdr);
    ok = write(fd, &w->phdr, sizeof(w->phdr)) == (ssize_t)sizeof(w->phdr)
         && fsync(fd) == 0;
    ok = close(fd) == 0 && ok;
    if (!ok)
        goto failed;
    UPDATE_TRACE("art panel commit in place bytes=%d slots=%lu",
                 (int)sizeof(w->phdr), (unsigned long)w->phdr.n_slots);
    panel_seq++;
    ART_PACE();
    pool_follow(w);                 /* #100: the shown set's slots */
    return true;
failed:
    FAIL(w);
    UPDATE_TRACE("art retain panels %s", "commit-failed");
    return false;
}

static bool panel_save(struct bwork *w)
{
    int fd;
    struct artpanel_hdr h;
    bool ok;

    if (w->panel_live && !w->panel_pending)
        return panel_commit(w);     /* #117: in place */
    if (!panel_stage(w))
        return false;
    ART_PACE();
    fd = open(F_PANELS_TMP, O_WRONLY);
    if (fd < 0)
        goto failed;
    w->phdr.version = ARTPANEL_VERSION;  /* a dev build's 2: 1 (review) */
    artpanel_hdr_seal(&w->phdr);
    ok = write(fd, &w->phdr, sizeof(w->phdr)) == (ssize_t)sizeof(w->phdr)
         && fsync(fd) == 0;
    ok = close(fd) == 0 && ok;
    if (!ok)
        goto failed;
    ART_PACE();
    fd = open(F_PANELS_TMP, O_RDONLY);
    if (fd < 0)
        goto failed;
    ok = read(fd, &h, sizeof(h)) == (ssize_t)sizeof(h)
         && artpanel_hdr_ok(&h, panel.w, panel.h)
         && ffilesize(fd) >= (off_t)artpanel_slot_offset(
                panel_slot_bytes, h.n_slots);
    ok = close(fd) == 0 && ok;
    ART_PACE();
    if (!ok || artfile_publish(F_PANELS, F_PANELS_TMP, F_PANELS_PREV)
               != ARTFILE_PUBLISHED)
        goto failed;
    w->panel_pending = false;
    w->panel_fresh = false;
    w->panel_live = true;           /* #117: in place from now on */
    panel_seq++;
    ART_PACE();
    pool_follow(w);                 /* #100: the shown set's slots */
    return true;
failed:
    FAIL(w);
    UPDATE_TRACE("art retain panels %s", "publication-failed");
    return false;
}

/* Decodes one panel (album t, whose thumbnail record is 'r') straight into
 * slot 's' of panels.pak (ps_decode(): bands of the reserved decode
 * block, no allocation, #58). Returns its state, -1 to retry on the next
 * pass, or PANEL_DEFERRED: playing, and the user came back before or
 * while it was written (no storage I/O of the builder's own from then
 * on, #58): the caller waits for the next idle and does it again. */
#define PANEL_DEFERRED (-2)
/* The decode of panel_build() and of the Now Playing cover (np-sharp,
 * npr_decode_pp()): the source of the track at w->path (source_decode())
 * streamed into slot 's' of 'path' (ps_decode(); 'pool' or 'inplace': a
 * slot written in place, fsynced, 'inplace' clearing its old tag first).
 * Same returns as panel_build(). */
static int panel_stream(struct bwork *w, artcache_key_t key,
                        const char *path, uint32_t s, bool pool,
                        bool inplace, struct artcache_rec *r)
{
    int st;

    w->ps_path = path;
    w->ps_pool = pool;
    w->ps_inplace = inplace;
    w->tw = panel.w;
    w->th = panel.h;
    w->stream = true;
    w->ps_slot = s;
    w->ps_key = key;
    w->ps_too_wide = false;
    w->ps_io_err = false;
#ifdef HAVE_BCMFW_VIDEO
    w->ps_cw = 0;
#endif
    st = source_decode(w, r);
    if (w->ps_io_err || w->lit)
        st = w->lit ? PANEL_DEFERRED : -1;
    w->stream = false;
    w->ps_inplace = false;
    w->tw = thumb.w;
    w->th = thumb.h;
    return st;
}

static int panel_build(struct bwork *w, const struct artalb_rec *t,
                       uint32_t s, struct artcache_rec *r, bool pool)
{
    bool inplace;

#ifdef SIMULATOR
    if (sim_panel_hold)
    {
        long ms = sim_panel_hold;

        sim_panel_hold = 0;
        UPDATE_TRACE("art panel build held ms=%ld filebuf=%lu", ms,
                     (unsigned long)audio_get_filebuflen());
        sleep(ms * HZ / 1000);
    }
#endif
    w->lit = false;
    if (!io_go(w, "read", t->key))
        return PANEL_DEFERRED;
    /* #117: a published panels.pak takes the slot in place (the caller
     * chose one its header does not name, so panel_seq moves only at
     * the commit; ps_untag() covers a shown set still naming it); only
     * its first creation goes through the private copy (a header and
     * nothing else) */
    inplace = !pool && w->panel_live && !w->panel_pending;
    if (!pool && !inplace && !panel_stage(w))
        return w->lit ? PANEL_DEFERRED : -1;
    if (!resolve(w, t))
        return -1;
    return panel_stream(w, t->key, pool ? F_POOL : inplace ? F_PANELS
                                                           : F_PANELS_TMP,
                        s, pool, inplace, r);
}

/* #92 (M5): each set's covers in consecutive slots of panels.pak
 * (artpanel_regroup_plan(); the version stays 1, review M1), so the
 * drift's bank refresh is one read. A
 * copy of the published file's slots in the new order (no decode, no
 * allocation: the work page), then the header, published as every save.
 * Maintenance only (regroup_window()): never at boot, never while
 * playing. */
#define REGROUP_BOOT_TICKS MAINT_PAUSE_TICKS

static bool regroup_window(void)
{
    if (!maint_window_open())
        return false;               /* playing, or paused < 3 min, lit */
    if (audio_status())
        return true;                /* the paused window (#88) */
    /* stopped: idle, and not the boot's passes */
    return user_idle() && TIME_AFTER(current_tick, (long)REGROUP_BOOT_TICKS);
}

static bool panel_regroup(struct bwork *w)
{
    struct artpanel_hdr *ph = &w->phdr;
    uint8_t plan[ARTPANEL_SLOTS];
    long t0 = current_tick;
    int in, out, k, n, s, from = ph->version;
    bool ok = true;

    (void)t0; (void)from;           /* trace only */
    if (w->panel_pending || !io_go(w, "write", 0))
        return false;
    n = artpanel_regroup_plan(ph, plan);
    in = open(F_PANELS, O_RDONLY);
    if (in < 0)
        return false;               /* nothing published yet */
    out = artfile_tmp_clear(F_PANELS_TMP, F_PANELS)     /* #91 H1 */
        ? open(F_PANELS_TMP, O_WRONLY | O_CREAT | O_TRUNC, 0666) : -1;
    if (out < 0)
    {
        close(in);
        FAIL(w);
        return false;
    }
    memset(w->page, 0, ARTPANEL_HDR_BYTES);   /* the header: last */
    ok = write(out, w->page, ARTPANEL_HDR_BYTES) == ARTPANEL_HDR_BYTES;
    for (s = 0; ok && s < n; s++)
    {
        off_t at;
        uint32_t left = panel_slot_bytes;

        for (k = 0; k < ARTPANEL_SLOTS && plan[k] != s; k++)
            ;
        if (k == ARTPANEL_SLOTS || !regroup_window())
        {
            /* (never) / the window closed (review M3): stop cleanly,
             * the private copy is dropped, again at the next window */
            w->lit = k < ARTPANEL_SLOTS;
            ok = false;
            break;
        }
        at = artpanel_slot_offset(panel_slot_bytes, ph->ent[k].slot);
        ok = lseek(in, at, SEEK_SET) == at;
        while (ok && left)
        {
            size_t c = MIN(left, sizeof(w->page));

            if (!io_ok() || usb_quiescing || usb_inserted()
                || art_q_stops())
            {
                w->lit = true;      /* again at the next window */
                ok = false;
                break;
            }
            ART_PACE();
            ok = read(in, w->page, c) == (ssize_t)c
                 && write(out, w->page, c) == (ssize_t)c;
            left -= c;
        }
    }
    ok = close(in) == 0 && ok;
    ok = close(out) == 0 && ok;
    if (!ok)
    {
        if (!w->lit)
            FAIL(w);
        return false;
    }
    for (k = 0; k < ARTPANEL_SLOTS; k++)
        if (plan[k] != 0xff)
            ph->ent[k].slot = plan[k];
    ph->n_slots = n;
    ph->version = ARTPANEL_VERSION;
    w->panel_pending = true;        /* the copy is the private file */
    if (!panel_save(w))
        return false;
    regroup_owed = false;
    UPDATE_TRACE("art panel regroup v=%d->%d slots=%d ms=%ld", from,
                 ARTPANEL_VERSION, n, (current_tick - t0) * (1000 / HZ));
    ui_log("art", "panel regroup v=%d slots=%d", from, n);
    return true;
}

/* ======================================================================= */
/* builder: the panel pool (#100)                                          */
/* ======================================================================= */
/* The rotation's clock: the RTC (it runs while the unit sleeps, P18 Q8),
 * in seconds; a unit without one counts uptime (the rotation at every
 * restart still happens) */
static uint32_t pool_now(void)
{
    uint32_t t;
#if CONFIG_RTC
    t = (uint32_t)mktime(get_time());
#else
    t = (uint32_t)(current_tick / HZ);
#endif
#ifdef SIMULATOR
    t += (uint32_t)sim_rot_warp;
#endif
    return t;
}

/* A rotation is due: none yet this boot, or PANEL_ROTATE_SECS since the
 * last (a clock set back counts as due). Any thread: two volatile reads
 * and the clock. */
static bool rot_due(void)
{
    return !rot_boot_done
           || (uint32_t)(pool_now() - rot_last) >= PANEL_ROTATE_SECS;
}

/* panelpool.idx into w->pidx, or an empty one (the first boot with the
 * pool: migration needs nothing, panels.pak's covers are candidates as
 * they are). Entries naming slots the pak does not hold are dropped. */
static void pool_load(struct bwork *w)
{
    struct artpool_idx *x = &w->pidx;
    bool ok = false;
    int fd;

    ART_PACE();
    fd = open(F_POOLX, O_RDONLY);
    if (fd >= 0)
    {
        ok = read(fd, x, sizeof(*x)) == (ssize_t)sizeof(*x)
             && artshown_ok(&x->shown.h, panel.w, panel.h)
             && artpool_ok(&x->pool);
        close(fd);
    }
    if (ok)
    {
        off_t bytes = 0;
        uint32_t k, have;

        ART_PACE();
        fd = open(F_POOL, O_RDONLY);
        if (fd >= 0)
        {
            bytes = ffilesize(fd);
            close(fd);
        }
        have = bytes > ARTPANEL_HDR_BYTES
               ? (uint32_t)((bytes - ARTPANEL_HDR_BYTES) / panel_slot_bytes)
               : 0;
        if (have < x->pool.n_slots)
        {
            for (k = 0; k < ARTPOOL_ENTS; k++)
                if (x->pool.ent[k].state == ARTREC_OK
                    && x->pool.ent[k].slot >= have)
                    memset(&x->pool.ent[k], 0, sizeof(x->pool.ent[k]));
            x->pool.n_slots = have;
        }
        rot_last = x->pool.rot_time;
    }
    else
        artpool_init(x, panel.w, panel.h);
    w->pidx_ok = true;
    UPDATE_TRACE("art pool load ok=%d slots=%lu", (int)ok,
                 (unsigned long)x->pool.n_slots);
}

/* w->pidx published (safefile_replace(): the old index or the new one
 * after a cut, never neither); readers re-read the shown set */
static bool pool_save(struct bwork *w)
{
    int fd;
    bool ok;

    if (!io_go(w, "write", 0))
        return false;
    ART_PACE();
    artpool_seal(&w->pidx);
    if (!safefile_remove(F_POOLX_TMP, F_POOLX))
        return false;
    ART_PACE();
    fd = open(F_POOLX_TMP, O_WRONLY | O_CREAT | O_TRUNC, 0666);
    if (fd < 0)
        return false;
    ok = write(fd, &w->pidx, sizeof(w->pidx)) == (ssize_t)sizeof(w->pidx)
         && fsync(fd) == 0;
    ok = close(fd) == 0 && ok;
    ART_PACE();
    if (!ok || safefile_replace(F_POOLX_TMP, F_POOLX, F_POOLX_PREV) != 0)
    {
        UPDATE_TRACE("art pool save %s", "failed");
        return false;
    }
    panel_seq++;
    UPDATE_TRACE("art pool save bytes=%lu", (unsigned long)sizeof(w->pidx));
    return true;
}

/* The shown set chosen again (artpool_pick(): no image work) from
 * panels.pak's published header and the pool, and published when it
 * changed or 'force' (the pool's table changed). 'rotate': fresh random
 * picks, the rotation's clock moves. */
static bool pool_publish(struct bwork *w, bool rotate, bool force)
{
    struct artpool_idx *x = &w->pidx;
    struct artpanel_hdr was = x->shown.h;
    uint16_t nr = x->pool.n_rand;
    uint32_t now = pool_now(), count = x->pool.rot_count;
    int n;

    ART_PACE();
    n = artpool_pick(x, &w->phdr, (count + rotate) * 2654435761u ^ now,
                     rotate);
    if (rotate)
    {
        x->pool.rot_count = count + 1;
        x->pool.rot_time = now;
    }
    else if (!force && !memcmp(&was, &x->shown.h, sizeof(was))
             && nr == x->pool.n_rand)
        return true;                /* the same set: no write */
    if (!pool_save(w))
    {
        x->pool.rot_count = count;  /* again at the next look */
        return false;
    }
    if (rotate)
    {
        rot_last = now;
        rot_boot_done = true;
    }
    UPDATE_TRACE("art panel %s shown=%d rand=%d count=%lu",
                 rotate ? "rotate" : "refresh", n, (int)x->pool.n_rand,
                 (unsigned long)x->pool.rot_count);
    if (rotate)
        ui_log("art", "panel rotate shown=%d rand=%d", n,
               (int)x->pool.n_rand);
    return true;
}

/* panels.pak was published (panel_save()): the shown set follows its
 * slots */
static bool follow_owed;         /* #100 review L1: a refused publish */
static void pool_follow(struct bwork *w)
{
    rot_no_pool = false;            /* a set to rotate again (Q8) */
    if (w->pidx_ok)
        follow_owed = !pool_publish(w, false, follow_owed);
}

/* The rotation outside a pass: storage awake (a natural wake), never a
 * spin-up of its own; while playing only with the user idle (io_go()) */
static void pool_rotate(struct bwork *w)
{
    rot_no_pool = !panel_load(w);   /* the sleep window owes none */
    if (rot_no_pool)
        return;
    if (!w->pidx_ok)
        pool_load(w);
    follow_owed = !pool_publish(w, rot_due(), true);
}

/* Q9: how many covers the pool may keep outside the default set */
static int pool_cap(void)
{
    sector_t free_kib = 0;
    uint64_t bytes;

    volume_size(IF_MV(0,) NULL, &free_kib);
    bytes = (uint64_t)free_kib * 1024;
    return bytes >= POOL_FREE_FULL ? ARTPOOL_ENTS
         : bytes >= POOL_FREE_SOME ? ARTPANEL_SLOTS : 0;
}

/* The entry of 'key' in the pool (any state), or -1 */
static int pool_find(const struct artpool_hdr *p, uint64_t key)
{
    int k;

    for (k = 0; k < ARTPOOL_ENTS; k++)
        if (p->ent[k].state && p->ent[k].key == key)
            return k;
    return -1;
}

/* Album t is in the default set (panels.pak builds it) */
static bool pool_in_default(const struct bwork *w, int npset, uint64_t key)
{
    int i;

    for (i = 0; i < npset; i++)
        if (w->pset[i].key == key)
            return true;
    return false;
}

/* Builds album t's cover into a free slot of panelpool.pak (in place,
 * fsync), then publishes the index naming it (the entry 'k' it replaces,
 * if any, freed in the same write). 1 built, 0 nothing to do, -1 stop. */
static int pool_build(struct bwork *w, const struct artalb_rec *t, int k)
{
    struct artpool_hdr *p = &w->pidx.pool;
    struct artcache_rec r;
    struct artpanel_ent *e;
    uint32_t s;
    int st, j;

    if (!artidx_lookup(&w->rd, t->key, &r, NULL))
        return 0;                   /* no thumbnail record yet */
    for (j = 0; j < ARTPOOL_ENTS && p->ent[j].state; j++)
        ;
    if (k >= 0 && ((p->ent[k].state == ARTREC_OK && artrec_sig_valid(&r)
                    && p->ent[k].src_crc == r.src_crc
                    && p->ent[k].src_size == r.src_size)
                   || (p->ent[k].state == ARTREC_OK && r.state != ARTREC_OK)))
    {
        /* #90: tags changed, same source (or no art now: an old usable
         * cover is kept, as panels.pak keeps one): the new mtime only */
        p->ent[k].mtime = t->max_mtime;
        return pool_publish(w, false, true) ? 1 : -1;
    }
    if (r.state != ARTREC_OK)
    {
        /* no art: remembered, no slot, no retry until the tags change */
        e = k >= 0 ? &p->ent[k] : j < ARTPOOL_ENTS ? &p->ent[j] : NULL;
        if (!e)
            return 0;
        memset(e, 0, sizeof(*e));
        e->key = t->key;
        e->mtime = t->max_mtime;
        e->src_crc = r.src_crc;
        e->src_size = r.src_size;
        e->state = r.state;
        PENT_SET(e) = PSET_OF(t);
        return pool_publish(w, false, true) ? 1 : -1;
    }
    if (j == ARTPOOL_ENTS)
        return 0;                   /* no entry for the new copy */
    s = artpool_free_slot(p);
    if (s > ARTPOOL_ENTS)
        return 0;
    if (s == p->n_slots)
    {
        /* an append: the file's front (ARTPANEL_HDR_BYTES, unused) first */
        int fd = open(F_POOL, O_WRONLY | O_CREAT, 0666);
        bool ok = fd >= 0;

        if (ok && ffilesize(fd) < ARTPANEL_HDR_BYTES)
        {
            memset(w->page, 0, ARTPANEL_HDR_BYTES);
            ok = lseek(fd, 0, SEEK_SET) == 0
                 && write(fd, w->page, ARTPANEL_HDR_BYTES)
                    == ARTPANEL_HDR_BYTES;
        }
        if (fd >= 0)
            ok = close(fd) == 0 && ok;
        if (!ok)
            return -1;
    }
    panel_seq++;                    /* a reader of a stale index: its
                                       tag check and this tell */
    memset(&r, 0, sizeof(r));
    r.key = t->key;
    ART_PACE();
    st = panel_build(w, t, s, &r, true);
    ART_PACE();
    UPDATE_TRACE("art pool build key=" KEYFMT " slot=%lu state=%d",
                 KEYARG(t->key), (unsigned long)s, st);
    if (st == PANEL_DEFERRED || w->lit)
        return -2;                  /* the user came back: not a failure */
    if (st != ARTREC_OK && st >= 0)
    {
        /* undecodable (or none after all): no slot, no retry until the
         * tags change (#100 review M2: never a decode every pass) */
        e = k >= 0 ? &p->ent[k] : &p->ent[j];
        memset(e, 0, sizeof(*e));
        e->key = t->key;
        e->mtime = t->max_mtime;
        e->state = st;
        PENT_SET(e) = PSET_OF(t);
        return pool_publish(w, false, true) ? 1 : -1;
    }
    if (st != ARTREC_OK)
        return -1;                  /* I/O */
    e = &p->ent[j];
    memset(e, 0, sizeof(*e));
    e->key = t->key;
    e->mtime = t->max_mtime;
    e->src_crc = r.src_crc;
    e->src_size = r.src_size;
    e->state = ARTREC_OK;
    e->slot = (uint8_t)s;
    PENT_SET(e) = PSET_OF(t);
#ifdef HAVE_BCMFW_VIDEO
    ARTPANEL_SET_CW(e, w->ps_cw);
#endif
    if (k >= 0)
        memset(&p->ent[k], 0, sizeof(p->ent[k]));   /* its old slot */
    if (s == p->n_slots)
        p->n_slots++;
    return pool_publish(w, false, true) ? 1 : -1;
}

/* ======================================================================= */
/* builder: the Now Playing cover (np-sharp, owner 2026-10-24)             */
/* ======================================================================= */
/* "Keep the stretched if it's worse, but make sure the sharp is queued":
 * the playing album with no built 300 px panel anywhere (panels.pak,
 * panelpool.pak) gets one in npcover.pak (two slots, most recent first),
 * so the main menu's Now Playing row turns sharp; the 55 px thumbnail
 * stands in until then. Queued by the row (artcache_npcover_queue(),
 * why=row) or by a play of NPR_PLAYED_MS (why=played); run by the idle
 * builder only in npr_place(), outside a pass, never boosted, between
 * art_gate() points. Traces:
 *   art npcover queue|have|drop|build key=K [from= slot= state= ms=] */

/* The decode's entry point. P19 (19-6): the BCM2722's DECODE job
 * (bcmjob_decode) takes this place, with this one as its fallback. */
static int npr_decode_pp(struct bwork *w, artcache_key_t key, uint32_t s,
                         struct artcache_rec *r)
{
    int st;

#ifdef HAVE_BCMFW_VIDEO
    w->vid = false;                 /* a music cover: square */
#endif
    w->no_boost = true;             /* resize_boost_cap(0) in decode() */
    st = panel_stream(w, key, F_NPCOVER, s, false, true, r);
    w->no_boost = false;
    return st;
}
#ifdef HAVE_ART_BCM
/* P19 19-6 (album-render.md section 14.1): the same decode, its step 2
 * marked for the BCM (decode_run(), bcm_panel()); the CPU's whenever the
 * BCM has no answer */
static int npr_decode_bcm(struct bwork *w, artcache_key_t key, uint32_t s,
                          struct artcache_rec *r)
{
    int st;

    w->bcm = true;
    st = npr_decode_pp(w, key, s, r);
    w->bcm = false;
    return st;
}
static int (*npr_decode)(struct bwork *w, artcache_key_t key, uint32_t s,
                         struct artcache_rec *r) = npr_decode_bcm;
#else
static int (*npr_decode)(struct bwork *w, artcache_key_t key, uint32_t s,
                         struct artcache_rec *r) = npr_decode_pp;
#endif

/* The playing track's album key, its path in w->path; 0 if none or it
 * changed meanwhile (np_pb_cover_key()'s check) */
static artcache_key_t npr_track(struct bwork *w)
{
    uint32_t before = np_pb_seq();
    struct mp3entry *id3;
    artcache_key_t key;

    if (!np_seq_stable(before) || !(audio_status() & AUDIO_STATUS_PLAY))
        return 0;
    id3 = audio_current_track();
    if (!id3 || !id3->path[0])
        return 0;
    strmemccpy(w->path, id3->path, sizeof(w->path));
    key = artcache_key_for_id3(id3);
    return np_pb_seq() == before ? key : 0;
}

/* npcover.pak's slot 's' ends in key's tag (16 bytes) */
static bool npc_tag_ok(uint32_t s, artcache_key_t key)
{
    off_t at = artpanel_slot_offset(panel_slot_bytes, s)
               + (off_t)panel.w * panel.h * sizeof(fb_data);
    struct artpak_tag tag;
    int fd = open(F_NPCOVER, O_RDONLY);
    bool ok = fd >= 0
              && lseek(fd, at, SEEK_SET) == at
              && read(fd, &tag, sizeof(tag)) == (ssize_t)sizeof(tag)
              && tag.magic == ARTPAK_TAG_MAGIC && tag.key == key;

    if (fd >= 0)
        close(fd);
    return ok;
}

typedef char npr_page_fits[sizeof(((struct bwork *)0)->page)
                           >= sizeof(struct artpool_hdr) ? 1 : -1];
/* Where key's 300 px panel is built already (through w->page): the
 * default set, the pool, npcover.pak; NULL if none */
static const char *npr_have(struct bwork *w, artcache_key_t key)
{
    const off_t pool_at = offsetof(struct artpool_idx, pool);
    struct artpanel_hdr *ph = (struct artpanel_hdr *)w->page;
    struct artpool_hdr *p = (struct artpool_hdr *)w->page;
    int k, fd;
    bool ok;

    if (!WIN_NO_DEFAULT() && panel_header_ex(F_PANELS, ph) > 0)
        for (k = 0; k < ARTPANEL_SLOTS; k++)
            if (ph->ent[k].key == key && ph->ent[k].state == ARTREC_OK)
                return "default";
    if (!WIN_NO_POOL() && (fd = open(F_POOLX, O_RDONLY)) >= 0)
    {
        ok = lseek(fd, pool_at, SEEK_SET) == pool_at
             && read(fd, p, sizeof(*p)) == (ssize_t)sizeof(*p)
             && artpool_ok(p);
        close(fd);
        for (k = 0; ok && k < ARTPOOL_ENTS; k++)
            if (p->ent[k].key == key && p->ent[k].state == ARTREC_OK)
                return "pool";
    }
    if (npc_header_ex(ph) > 0 && (k = artnpc_find(ph, key)) >= 0
        && npc_tag_ok(ph->ent[k].slot, key))
        return "npcover";           /* (a slot whose tag is not key's:
                                       rendered again, its entry replaced) */
    return NULL;
}

/* npcover.pak's header from w->page (1 KB, the bytes past the struct
 * zeroed by the caller), in place, on disk before it returns */
static bool npc_write_hdr(struct bwork *w)
{
    int fd = open(F_NPCOVER, O_WRONLY);
    bool ok = fd >= 0;

    ok = ok && lseek(fd, 0, SEEK_SET) == 0
         && write(fd, w->page, ARTPANEL_HDR_BYTES) == ARTPANEL_HDR_BYTES
         && fsync(fd) == 0;
    if (fd >= 0)
        ok = close(fd) == 0 && ok;
    return ok;
}

static void npr_forget(artcache_key_t key, bool failed)
{
    if (failed)
    {
        npr_fail[npr_fail_at] = key;
        npr_fail_at = (npr_fail_at + 1) % NPR_FAIL_MAX;
    }
    if (npr_key == key)
        npr_key = 0;                /* (unless another one came meanwhile) */
}

#ifdef HAVE_ART_BCM
/* P19 19-6 (album-render.md section 14, the end state): album key's panel
 * is built ('from'), but not resident on the BCM this epoch (a cut, a
 * video, a restart lost it): the same source decoded there again, into
 * the resident slot, nothing written and nothing read back (never an
 * upload of the file's pixels).  The BCM's or nothing: no CPU decode.
 * Kept queued when the user came back, else done */
static void npr_resident(struct bwork *w, artcache_key_t key,
                         const char *from)
{
    struct artcache_rec r;
    long t0 = current_tick;
    int st;

    memset(&r, 0, sizeof(r));
    r.key = key;
    w->ps_resident = true;
    w->ps_res_ok = false;
    st = npr_decode(w, key, 0, &r);
    w->ps_resident = false;
    UPDATE_TRACE("art npcover resident key=" KEYFMT " from=%s ok=%d st=%d "
                 "ms=%ld", KEYARG(key), from, (int)w->ps_res_ok, st,
                 (current_tick - t0) * 1000 / HZ);
    (void)from; (void)t0; (void)st;     /* (the trace only) */
    if (!w->ps_res_ok && npr_abort)
        return;                     /* the user came back: again later */
    npr_forget(key, false);
}
#endif

/* The queued render (npr_key), once: dropped when that album no longer
 * plays or has a panel; kept when the user came back (again at the next
 * place); remembered as failed otherwise */
static void npr_run_impl(struct bwork *w)
{
    struct artpanel_hdr *h = (struct artpanel_hdr *)w->page;
    artcache_key_t key = npr_key;
    struct artpanel_ent e;
    struct artcache_rec r;
    const char *from;
    uint32_t s;
    long t0;
    int st, fd, k;
    bool ok;

    if (!key)
        return;
    if (npr_track(w) != key)
    {
        UPDATE_TRACE("art npcover drop key=" KEYFMT " state=%s",
                     KEYARG(key), "track");
        npr_forget(key, false);
        return;
    }
    w->lit = false;
    if (!io_go(w, "read", key))
        return;                     /* the user came back: still queued */
    from = npr_have(w, key);
#ifdef HAVE_ART_BCM
    if (from && art_bcm_resident_wanted(key) && bcm_viz_defer(w, "resident"))
        return;                     /* still queued: after the view */
    if (from && art_bcm_resident_wanted(key)
        && art_bcm_ready_ovl(bcm_cancel, w))
    {
        npr_resident(w, key, from);
        return;
    }
#endif
    if (from)
    {
        UPDATE_TRACE("art npcover have key=" KEYFMT " from=%s",
                     KEYARG(key), from);
        npr_forget(key, false);
        return;
    }
    /* no art, or art the device cannot decode: known from the album's
     * thumbnail record, no source I/O (the builder's index, as committed) */
    if (artidx_lookup(&w->rd, key, &r, NULL) && r.state != ARTREC_OK)
    {
        UPDATE_TRACE("art npcover drop key=" KEYFMT " state=%d",
                     KEYARG(key), (int)r.state);
        npr_forget(key, true);
        return;
    }
    /* the file's 1 KB front first, as pool_build()'s append; then the
     * slot (the oldest's when both are taken) */
    if (!io_go(w, "write", key))
        return;
    fd = open(F_NPCOVER, O_WRONLY | O_CREAT, 0666);
    ok = fd >= 0;
    if (ok && ffilesize(fd) < ARTPANEL_HDR_BYTES)
    {
        memset(w->page, 0, ARTPANEL_HDR_BYTES);
        ok = lseek(fd, 0, SEEK_SET) == 0
             && write(fd, w->page, ARTPANEL_HDR_BYTES) == ARTPANEL_HDR_BYTES;
    }
    if (fd >= 0)
        ok = close(fd) == 0 && ok;
    if (!ok)
    {
        UPDATE_TRACE("art npcover build key=" KEYFMT " slot=%d state=%d "
                     "ms=0", KEYARG(key), -1, -1);
        npr_forget(key, true);
        return;
    }
    if (npc_header_ex(h) <= 0)
        artnpc_init(h, panel.w, panel.h);
    s = artnpc_slot(h);
    npc_seq++;                      /* odd: readers of the old header */
    /* review M1: the album whose slot this is leaves the header first
     * (fsync), so a cut, a failed decode or a deferral leaves it absent
     * (rendered again when queued), never named over an untagged slot */
    for (k = 0; k < ARTNPC_SLOTS; k++)
        if (h->ent[k].state == ARTREC_OK && h->ent[k].slot == s)
        {
            UPDATE_TRACE("art npcover evict key=" KEYFMT " slot=%lu",
                         KEYARG(h->ent[k].key), (unsigned long)s);
            memset(&h->ent[k], 0, sizeof(h->ent[k]));
            artpanel_hdr_seal(h);
            memset((char *)w->page + sizeof(*h), 0,
                   ARTPANEL_HDR_BYTES - sizeof(*h));
            if (!io_go(w, "write", key) || !npc_write_hdr(w))
            {
                npc_seq++;
                return;             /* again at the next place */
            }
        }
    memset(&r, 0, sizeof(r));
    r.key = key;
    t0 = current_tick;
    npr_t0 = t0;
    npr_abort = false;
    UPDATE_TRACE("art npcover start key=" KEYFMT " slot=%lu", KEYARG(key),
                 (unsigned long)s);
    st = npr_decode(w, key, s, &r);
    ok = st == ARTREC_OK && !w->lit && !npr_abort;
    if (ok)
    {
        if (npc_header_ex(h) <= 0)
            artnpc_init(h, panel.w, panel.h);
        memset(&e, 0, sizeof(e));
        e.key = key;
        e.src_crc = r.src_crc;
        e.src_size = r.src_size;
        e.state = ARTREC_OK;
        e.slot = (uint8_t)s;
        artnpc_put(h, &e);
        memset((char *)w->page + sizeof(*h), 0,
               ARTPANEL_HDR_BYTES - sizeof(*h));
#ifdef SIMULATOR
        if (sim_panel_hold)
        {
            /* art_panel_hold (ipodui-np-sharp-events): a request or USB
             * arrives before the header's write (review H1) */
            long ms = sim_panel_hold;

            sim_panel_hold = 0;
            UPDATE_TRACE("art npcover held ms=%ld", ms);
            sleep(ms * HZ / 1000);
        }
#endif
        ok = io_go(w, "write", key) && npc_write_hdr(w);
    }
    npc_seq++;                      /* even: the render landed (or not) */
    UPDATE_TRACE("art npcover build key=" KEYFMT " slot=%lu state=%d ms=%ld",
                 KEYARG(key), (unsigned long)s, ok ? ARTREC_OK
                 : (st == ARTREC_OK || npr_abort) ? PANEL_DEFERRED : st,
                 (current_tick - t0) * 1000 / HZ);
    if (ok)
        ui_log("art", "npcover build slot=%lu ms=%ld", (unsigned long)s,
               (current_tick - t0) * 1000 / HZ);
    if (st == PANEL_DEFERRED || w->lit || npr_abort)
        return;                     /* the user came back: again later */
    npr_forget(key, !ok);
}

static void npr_run(struct bwork *w)
{
    npr_busy = true;
    npr_abort = false;
    npr_t0 = current_tick;
    npr_run_impl(w);
    npr_busy = false;
}

/* After panels.pak: the pool follows the precompute's pool order (col 1):
 * the pinned keys, members dropped (gone, now in the default set, past
 * the cap Q9), the shown set rotated when due (no image work) or
 * refreshed; then, inside a maintenance window only (regroup_window(),
 * maint_take()), up to POOL_STEP missing or changed covers built. */
/* #100 review M1: a top-up that failed (I/O, the index, the floor) is not
 * tried again in the same window: the window's epoch (last input, pause
 * start), as the signature fill's */
static long pool_fail_input = -1, pool_fail_pause = -2;

static bool pool_window_failed(void)
{
    return last_input == pool_fail_input
           && maint_paused_since() == pool_fail_pause;
}

static void pool_window_fail(void)
{
    pool_fail_input = last_input;
    pool_fail_pause = maint_paused_since();
    UPDATE_TRACE("art pool window %s", "failed");
}

/* run_pool()'s first half (its own frame: the decode below runs on a
 * shallower stack): the pinned keys and the members kept; true if the
 * pool's table changed. *total: the pool order's length, -1 on error. */
static bool __attribute__((noinline))
pool_scan(struct bwork *w, int npset, int cap, int *total)
{
    struct artpool_hdr *p = &w->pidx.pool;
    struct artalb_rec chunk[8];
    uint8_t pin[8];
    uint32_t keep[(ARTPOOL_ENTS + 31) / 32];
    uint64_t pinned[ARTPOOL_PINNED];
    int first = 0, got, i, k, extra = 0, npin = 0;
    bool dirty = false;

    memset(keep, 0, sizeof(keep));
    memset(pinned, 0, sizeof(pinned));
    do
    {
        ART_PACE();
        got = artalb_pool(w->tpath, &w->thdr, first, ARRAYLEN(chunk), chunk,
                          pin, total);
        if (got < 0)
        {
            *total = -1;
            return false;           /* replaced or I/O: the next pass */
        }
        for (i = 0; i < got; i++)
        {
            if (pin[i] && npin < ARTPOOL_PINNED)
                pinned[npin++] = chunk[i].key;
            if (pool_in_default(w, npset, chunk[i].key) || extra++ >= cap)
                continue;
            k = pool_find(p, chunk[i].key);
            if (k >= 0)
                keep[k / 32] |= 1u << (k % 32);
        }
        first += got;
    } while (got > 0 && first < *total);
    if (npin != p->n_pinned || memcmp(pinned, p->pinned, sizeof(pinned)))
    {
        memcpy(p->pinned, pinned, sizeof(pinned));
        p->n_pinned = npin;
        dirty = true;
    }
    for (k = 0; k < ARTPOOL_ENTS; k++)
        if (p->ent[k].state && !(keep[k / 32] & (1u << (k % 32))))
        {
            memset(&p->ent[k], 0, sizeof(p->ent[k]));
            dirty = true;
        }
    return dirty;
}

static int run_pool(struct bwork *w, int npset)
{
    struct artpool_hdr *p = &w->pidx.pool;
    struct artalb_rec chunk[8];
    int total = 0, first, got, i, k, cap, extra, built = 0, ev = 0;
    bool dirty, taken = false, owed = false;
    long t0 = current_tick;

    (void)t0;
    if (!w->pidx_ok)
        pool_load(w);
    ART_PACE();
    cap = pool_cap();
    dirty = pool_scan(w, npset, cap, &total);
    if (total < 0)
        return 0;
    ART_PACE();
    if (rot_due())
        follow_owed = !pool_publish(w, true, true);
    else
        follow_owed = !pool_publish(w, false, dirty || follow_owed);
    /* L4: space is short (Q9): the file's free tail goes, once the
     * published index no longer names it */
    if (cap < ARTPOOL_ENTS && !follow_owed)
    {
        uint32_t hi = 0;

        for (k = 0; k < ARTPOOL_ENTS; k++)
            if (p->ent[k].state == ARTREC_OK && p->ent[k].slot + 1u > hi)
                hi = p->ent[k].slot + 1u;
        if (hi < p->n_slots)
        {
            uint32_t was = p->n_slots;

            p->n_slots = hi;
            if (pool_publish(w, false, true))
            {
                int fd = open(F_POOL, O_WRONLY);

                if (fd >= 0)
                {
                    ftruncate(fd, hi ? artpanel_slot_offset(panel_slot_bytes,
                                                            hi) : 0);
                    close(fd);
                }
                UPDATE_TRACE("art pool shrink slots=%lu->%lu cap=%d",
                             (unsigned long)was, (unsigned long)hi, cap);
            }
            else
                p->n_slots = was;
        }
    }

    /* the top-up */
    for (first = 0, extra = 0; first < total && built >= 0
                               && built < POOL_STEP; first += got)
    {
        ART_PACE();
        got = artalb_pool(w->tpath, &w->thdr, first, ARRAYLEN(chunk), chunk,
                          NULL, &total);
        if (got <= 0)
            break;
        for (i = 0; i < got && built >= 0 && built < POOL_STEP; i++)
        {
            int rc;

            if (pool_in_default(w, npset, chunk[i].key) || extra++ >= cap)
                continue;
            k = pool_find(p, chunk[i].key);
            if (k >= 0 && p->ent[k].mtime == chunk[i].max_mtime)
                continue;           /* built (or no art) for these tags */
            /* owed only while a window would get somewhere: a cover that
             * cannot be built now (no record yet, no room) is not owed,
             * so a window never loops passes over it */
            if (!regroup_window() || !io_ok())
            {
                owed = true;
                break;
            }
            if (pool_window_failed())
                break;              /* once a window (M1): not owed */
            /* another job owns this window (the library commit): it goes
             * first; its publish requests a pass, which tops up after */
            if (!taken && (maint_wanted() || maint_active()))
                break;
            if (!taken)
            {
                if (!maint_take(MAINT_PANELS, 0)
                    || (audio_status() && !maint_fits(0)))
                {
                    /* the floor (Q11): the next window, not this one */
                    pool_window_fail();
                    break;
                }
                taken = true;
            }
            ART_PACE();
            rc = pool_build(w, &chunk[i], k);
            if (rc == -2)
            {
                built = -1;
                owed = true;        /* the user came back: the next window */
            }
            else if (rc < 0)
            {
                built = -1;         /* I/O, the index: not this window */
                pool_window_fail();
            }
            else
            {
                built += rc;
                ev = pump(0);
                if (ev)
                {
                    built = -1;     /* an event: handled by the caller */
                    owed = true;
                }
                else if (built == POOL_STEP)
                    owed = true;    /* the next step */
            }
        }
    }
    if (taken)
        maint_done(MAINT_PANELS, built >= 0);
    pool_owed = owed;
    UPDATE_TRACE("art pool pass total=%d cap=%d built=%d owed=%d ms=%ld",
                 total, cap, built, (int)owed,
                 (current_tick - t0) * (1000 / HZ));
    /* npfix (device log sleeptest-1457): which pool top-ups cost the
     * decodes of an idle window, on the device too */
    if (built != 0)
        ui_log("art", "pool built=%d owed=%d cap=%d", built, (int)owed, cap);
    return ev;
}

/* Brings panels.pak up to the table's panel set: drops albums no longer
 * in it, builds the missing and changed ones (thumbnail record OK, other
 * mtime or source). During playback only while the user is idle, like
 * the albums (wait_ready(), #58); a panel the user came back on is done
 * again at the next idle (the same album: each retry needs the user to
 * come back mid-panel, so this ends). Returns the events that ended it
 * (0: done). */
static int run_hints(struct bwork *w);
static int run_panels(struct bwork *w)
{
    struct artpanel_hdr *ph = &w->phdr;
    struct artcache_rec r;
    long t0 = current_tick;
    int n, i, ev, built = 0;
    uint32_t s;
    bool dirty = false;

    (void)t0; (void)built;          /* trace only */
    ev = wait_ready(true);          /* playing: the user idle */
    if (ev)
        return ev;
    n = artalb_panels(w->tpath, &w->thdr, w->pset, ARTPANEL_SLOTS);
    ART_PACE();
    if (n < 0 || !panel_load(w))
        return 0;                   /* replaced (a request follows), I/O */
    ART_PACE();
    if (!w->pidx_ok)
        pool_load(w);               /* #100: the shown set follows */
    ART_PACE();
    build_phase(ARTCACHE_BUILD_PHASE_PANELS, n);
    for (s = 0; s < ARTPANEL_SLOTS; s++)
    {
        if (!ph->ent[s].state)
            continue;
        for (i = 0; i < n && w->pset[i].key != ph->ent[s].key; i++)
            ;
        if (i == n)
        {
            memset(&ph->ent[s], 0, sizeof(ph->ent[s]));
            dirty = true;
        }
        else if (PENT_SET(&ph->ent[s]) != PSET_OF(&w->pset[i]))
        {
            PENT_SET(&ph->ent[s]) = PSET_OF(&w->pset[i]);
            dirty = true;           /* a pak from before the book set */
        }
    }
    if (dirty && !panel_save(w))
        return 0;
    for (i = 0; i < n; i++)
    {
        const struct artalb_rec *t = &w->pset[i];
        struct artpanel_ent *e = NULL;
        uint32_t used = 0;
        int st, k;

        ART_PACE();
        for (k = 0; k < ARTPANEL_SLOTS; k++)
            if (ph->ent[k].state && ph->ent[k].key == t->key)
                e = &ph->ent[k];
        if (!artidx_lookup(&w->rd, t->key, &r, NULL))
        {
            build_advance();
            continue;               /* no thumbnail record yet */
        }
        /* #90: tags changed, same source signature: the same image
         * (the entry's mtime stays old; nothing is written for it) */
        if (e && (e->mtime == t->max_mtime
                  || (e->state == ARTREC_OK && artrec_sig_valid(&r)))
            && e->src_crc == r.src_crc && e->src_size == r.src_size)
        {
            build_advance();
            continue;               /* up to date (whatever its state) */
        }
        if (!e)
        {
            for (k = 0; k < ARTPANEL_SLOTS && ph->ent[k].state; k++)
                ;
            if (k == ARTPANEL_SLOTS)
                break;              /* never: the set has <= 32 albums */
            e = &ph->ent[k];
        }
        ev = wait_ready(true);
        if (!ev)
            ev = run_hints(w);      /* what the user saw first (the next
                                       idle's first work, #58) */
        if (ev)
            return ev;
        if (r.state != ARTREC_OK)
        {
            if (e->state == ARTREC_OK)
            {
                build_advance();    /* retain an old usable panel */
                continue;
            }
            /* no art (or undecodable): no slot, no retry */
            e->key = t->key;
            e->mtime = t->max_mtime;
            e->src_crc = r.src_crc;
            e->src_size = r.src_size;
            e->state = r.state;
            PENT_SET(e) = PSET_OF(t);
#ifdef HAVE_BCMFW_VIDEO
            ARTPANEL_SET_CW(e, 0);
#endif
            if (!panel_save(w))
                return 0;
            build_advance();
            continue;
        }
        /* the lowest slot no entry uses (the file grows by appends
         * only: s <= n_slots). #117: a changed cover too, so its old
         * panel stays shown (and on a failure or a deferral, kept) until
         * the new one is on disk; the next maintenance window regroups.
         * With every slot taken it reuses its own, written in place only
         * after the header stops naming it (the cover is missing from
         * the drift meanwhile: 32 covers, one of them changed). */
        for (k = 0; k < ARTPANEL_SLOTS; k++)
            if (ph->ent[k].state == ARTREC_OK)
                used |= 1u << ph->ent[k].slot;
        for (s = 0; s < ARTPANEL_SLOTS && (used & (1u << s)); s++)
            ;
        if (e->state == ARTREC_OK && s == ARTPANEL_SLOTS)
        {
            s = e->slot;
            if (w->panel_live && !w->panel_pending)
            {
                e->state = 0;       /* unnamed before it is rewritten */
                if (!panel_save(w))
                    return 0;
            }
        }
        memset(&r, 0, sizeof(r));
        r.key = t->key;
        ART_PACE();
        st = panel_build(w, t, s, &r, false);
        ART_PACE();
        if (st == PANEL_DEFERRED)
        {
            w->panel_pending = false; /* discard partial private pixels */
            UPDATE_TRACE("art build panel %s", "deferred");
            i--;                    /* the same album, next idle */
            continue;
        }
        UPDATE_TRACE("art build panel key=" KEYFMT " slot=%lu state=%d "
                     "boost=%d", KEYARG(t->key), (unsigned long)s, st,
                     (int)ART_BOOST());
        if (st != ARTREC_OK)
        {
            w->panel_pending = false;
            FAIL(w);
            build_advance();        /* checked, retry on the next pass */
            continue;
        }
        e->key = t->key;
        e->mtime = t->max_mtime;
        e->src_crc = r.src_crc;
        e->src_size = r.src_size;
        e->state = st;
        e->slot = s;
        PENT_SET(e) = PSET_OF(t);
#ifdef HAVE_BCMFW_VIDEO
        ARTPANEL_SET_CW(e, st == ARTREC_OK ? w->ps_cw : 0);
        if (w->ps_cw)
            UPDATE_TRACE("art panel cw key=" KEYFMT " cw=%d",
                         KEYARG(t->key), w->ps_cw);
#endif
        if (st == ARTREC_OK && s == ph->n_slots)
            ph->n_slots++;
        if (!panel_save(w))
            return 0;
        built++;
        build_advance();
    }
    UPDATE_TRACE("art build panels set=%d built=%d ms=%ld", n, built,
                 (current_tick - t0) * (1000 / HZ));
    /* #92: a set refresh is one read once each set is grouped */
    regroup_owed = ph->n_slots && !artpanel_grouped(ph);
    ART_PACE();
    if ((regroup_owed || pool_owed) && regroup_window())
        panel_regroup(w);
    ART_PACE();
    return run_pool(w, n);          /* #100 */
}

/* ======================================================================= */
/* builder: chapter art (Phase 5, docs/design/chapter-art.md)              */
/* ======================================================================= */
#define CHAP_DEFERRED (-2)          /* playback started: at the stop */
#define CHAP_CAP      (-3)          /* the index is full of chapters */
#define CHAP_IO       (-5)          /* the index: not the book's fault */
#define CHAP_LATER    (-6)          /* paused: images wait for a stop */

/* artchap_plan()'s view of the art table: a decode failure (or a PNG,
 * which the device can't decode) is type AB_ART_NONE with its size kept */
static int chap_img(void *ctx, int i, uint32_t *pos, uint8_t *src_type)
{
    const struct ab_chapter_art *a = (const struct ab_chapter_art *)ctx + i;

    *pos = a->pos;
    *src_type = AA_TYPE_JPG;
    if (!a->size)
        return 0;
    return a->type == AB_ART_JPEG ? 1 : -1;
}

/* Between two images, with the block held: the gap, then until nothing
 * blocks the builder, but never while audio plays (then the book waits
 * for the stop: wait_ready() would hold the block until an idle). */
static int chap_wait(void)
{
    int ev = pump(maintenance ? 0 : (charger_inserted() ? GAP_CHARGING
                                                         : GAP_TICKS));
    bool unboosted = false;

    while (!ev)
    {
        if (art_hold())
        {
            ev = CHAP_DEFERRED;
            break;
        }
        if (!blocked())
            break;
        if (FAST_BOOSTED())
        {
            fast_boost(false);      /* never boosted while it waits */
            unboosted = true;
        }
        ev = pump(POLL_TICKS);
    }
    if (unboosted)
        fast_boost(true);
    return ev;
}

/* Finding 4: the transient block is never kept while audio plays. Called
 * right after the allocation (and the sim's hold) and before every read
 * into it: true (with the trace line) when playback has started. */
static bool chap_give_up(const char *at)
{
    (void)at;                       /* trace only */
    if (!art_hold())
        return false;
    UPDATE_TRACE("art chapter block giveup at=%s", at);
    return true;
}

static struct ab_chapter_art *chap_art(int h)
{
    return (struct ab_chapter_art *)((char *)core_get_data(h) + chap_tab_off);
}

/* Finding 8: ID3 CHAP frames embed one APIC copy per chapter, so a book
 * whose chapters share a picture holds byte-identical copies at separate
 * positions. Candidates are found by size and type (no I/O), confirmed by
 * a CRC-32 of their bytes (read only for an image whose size an earlier
 * image has, each image at most once), and pointed at the first copy's
 * position: artchap_plan() and chap_decode() group by position, so one
 * bundle is decoded and stored for all of them. The CRCs, their "done"
 * bits and the read buffer live in the chapter table's room (free once
 * the art table is read). The block is pinned for one read at a time and
 * the audio status checked before each. fd: the book, open. Returns 0 or
 * CHAP_DEFERRED. */
#define CHAP_CRC_CHUNK  16384

static int chap_crc(int h, int fd, int i, artcache_key_t book,
                    uint32_t *out, int *reads)
{
    struct ab_chapter_art a = chap_art(h)[i];
    size_t done_off = ALIGN_UP(AB_MAX_CHAPTERS * sizeof(uint32_t), 8);
    size_t buf_off = done_off + ALIGN_UP(AB_MAX_CHAPTERS / 8, 8);
    size_t chunk = MIN((size_t)CHAP_CRC_CHUNK, chap_tab_off - buf_off);
    uint8_t *done = (uint8_t *)core_get_data(h) + done_off;
    uint32_t left = a.size, crc = 0xffffffff;

    if (done[i >> 3] & (1 << (i & 7)))
    {
        *out = ((uint32_t *)core_get_data(h))[i];
        return 0;
    }
    (*reads)++;
    if (lseek(fd, a.pos, SEEK_SET) != (off_t)a.pos)
        return -1;
    while (left)
    {
        size_t n = MIN(left, chunk);
        char *blk;
        ssize_t got;

        ART_PACE();                 /* (nothing of the block held) */
        if (chap_give_up("crc"))
            return CHAP_DEFERRED;
        IO_TRACE("read", book);
        blk = core_get_data_pinned(h);
        got = read(fd, blk + buf_off, n);
        if (got == (ssize_t)n)
            crc = crc_32(blk + buf_off, n, crc);
        core_put_data_pinned(blk);
        if (got != (ssize_t)n)
            return -1;
        left -= n;
    }
    ((uint32_t *)core_get_data(h))[i] = crc;
    ((uint8_t *)core_get_data(h) + done_off)[i >> 3] |= 1 << (i & 7);
    *out = crc;
    return 0;
}

static int chap_dedupe(int h, int fd, artcache_key_t book, int count,
                       int *dups, int *reads)
{
    size_t done_off = ALIGN_UP(AB_MAX_CHAPTERS * sizeof(uint32_t), 8);
    int i, j, rc;

    memset((char *)core_get_data(h) + done_off, 0, AB_MAX_CHAPTERS / 8);
    for (i = 1; i < count; i++)
    {
        struct ab_chapter_art a = chap_art(h)[i];
        uint32_t ci = 0, cj = 0;
        bool have_i = false;

        ART_PACE();                 /* (a is a copy) */
        if (!a.size)
            continue;
        for (j = 0; j < i; j++)
        {
            struct ab_chapter_art b = chap_art(h)[j];

            if (b.size != a.size || b.type != a.type)
                continue;
            if (b.pos == a.pos)
                break;              /* already one group (MP4 samples) */
            if (!have_i)
            {
                rc = chap_crc(h, fd, i, book, &ci, reads);
                if (rc == CHAP_DEFERRED)
                    return rc;
                if (rc < 0)
                    return rc;      /* storage failure: retain old records */
                have_i = true;
            }
            if ((rc = chap_crc(h, fd, j, book, &cj, reads)) == CHAP_DEFERRED)
                return rc;
            if (rc < 0)
                return rc;
            if (rc == 0 && cj == ci)
            {
                chap_art(h)[i].pos = b.pos;
                (*dups)++;
                break;
            }
        }
    }
    return 0;
}

/* Decodes each group's first image (artchap_plan()'s grouping: one
 * position, adjacent or not) into its bundle; a group whose image fails
 * is marked so no record names it. The art table is re-read from the
 * (unpinned) block after every yield. The source is opened per image (no
 * fd is held across a wait). */
static int chap_decode(struct bwork *w, artcache_key_t book, int h,
                       int count, int *bundles)
{
    uint32_t group_pos = 0;
    bool in_group = false, ok = false;
    int i, ev, fd, owner;

    for (i = 0; i < count; i++)
    {
        struct ab_chapter_art *art = chap_art(h), a = art[i];
        artcache_key_t ck;

        if (!a.size)
            continue;
        if (in_group && a.pos == group_pos)
        {
            if (!ok)
                art[i].type = AB_ART_NONE;
            continue;
        }
        in_group = true;
        group_pos = a.pos;
        owner = artchap_owner(chap_img, art, i, a.pos);
        if (owner < i)
        {
            /* a copy of an earlier group's image (the dedupe) */
            ok = art[owner].type != AB_ART_NONE;
            if (!ok)
                art[i].type = AB_ART_NONE;
            continue;
        }
        ok = false;
        if (a.type != AB_ART_JPEG)
        {
            /* PNG (the Mac tool, 7b): never decodable here, so the cover
             * (chap_img() -1); failing the book for it failed every pass
             * on the device (91fead48e1: "art phase=4", then a retry) */
            art[i].type = AB_ART_NONE;
            continue;
        }
        ev = chap_wait();
        if (ev)
            return ev;
        ck = artcache_key_chapter(book, i);
        IO_TRACE("read", ck);
        fd = open(w->path, O_RDONLY);
        w->lit = false;
        if (fd >= 0)
        {
            ok = np_write(w, ck, fd, a.pos, a.size, AA_TYPE_JPG, 0);
            close(fd);
        }
        if (ok)
            (*bundles)++;
        else if (w->lit)
            return CHAP_DEFERRED;   /* its BCM decode deferred (the
                                       visualizer's view, a cancel): no
                                       try spent */
        else
        {
            UPDATE_TRACE("art retain chapter key=" KEYFMT, KEYARG(ck));
            return -1;              /* keep old records; retry the book
                                       (run_chapters(): CHAP_TRIES) */
        }
    }
    return 0;
}

/* One single-file book (its record 'b', no chapter pass yet for its
 * mtime): reads its chapters and their images into one transient block,
 * decodes each distinct image once and commits the chapter records with
 * the book's record marked done (and the done marks gathered in
 * w->delta). A book without chapters only gets the mark (w->delta, no
 * block). The block is pinned for one read (or the index write) at a
 * time, and given up (freed, the audio buffer regrown) as soon as audio
 * plays (finding 4). Returns 0, -1 (skipped: again next pass),
 * CHAP_DEFERRED, CHAP_CAP or the events that ended it. */
static int chapter_book(struct bwork *w, const struct artalb_rec *t,
                        const struct artcache_rec *b)
{
    struct audiobook_chapters *ab;
    struct artcache_rec *recs;
    char *blk;
    long t0 = current_tick;
    int h, fd, i, count = 0, imgs = 0, bundles = 0, n = 0, rc;
    int dups = 0, reads = 0;
    bool lent = false;              /* paused: the decode block, lent */

    (void)t0;                       /* trace only */
    IO_TRACE("read", t->key);
    if (!resolve(w, t))
        return -1;
    ART_PACE();
    fd = open(w->path, O_RDONLY);
    if (fd < 0)
        return -1;
    if (!get_metadata(&w->id3, fd, w->path))
    {
        close(fd);
        return -1;                  /* unreadable is not "no chapters" */
    }
    ART_PACE();                     /* (before the hold check below) */
    if (w->id3.chapter_count <= 0)
    {
        close(fd);
        w->delta[w->n_delta] = *b;
        w->delta[w->n_delta++].flags |= ARTREC_F_CHAPTERS;
        return 0;
    }
    /* checked here, with no yield before the allocation */
    if (art_hold())
    {
        close(fd);
        return CHAP_DEFERRED;
    }
    if (audio_status())
    {
        /* paused: an allocation would stop and restart the paused
         * queue (playback.c shrink), so the tables go in the idle
         * decode block; a book with images waits for a stop, or for
         * the paused window (#88): there its own block is taken, by one
         * partial shrink if the free heap lacks it (the paused song
         * restarts silently), the floor kept (maint.c) */
        if (decode_bytes >= chap_bytes && !maint_window_open())
        {
            h = dec_h;
            lent = true;
        }
        else if (maint_take(MAINT_CHAPTERS, chap_bytes))
        {
            h = core_alloc(chap_bytes);
            UPDATE_TRACE("art chapter block window bytes=%lu h=%d "
                         "filebuf=%lu", (unsigned long)chap_bytes, h,
                         (unsigned long)audio_get_filebuflen());
        }
        else
        {
            close(fd);
            return CHAP_LATER;
        }
    }
    else
    {
        UPDATE_TRACE("art chapter block alloc bytes=%lu filebuf=%lu",
                     (unsigned long)chap_bytes,
                     (unsigned long)audio_get_filebuflen());
        h = core_alloc(chap_bytes);
    }
    if (h <= 0)
    {
        close(fd);
        UPDATE_TRACE("art alloc failed what=%s bytes=%lu", "chapters",
                     (unsigned long)chap_bytes);
        return -1;
    }
#ifdef SIMULATOR
    if (sim_panel_hold)             /* art_panel_hold: the next block */
    {
        long ms = sim_panel_hold;

        sim_panel_hold = 0;
        UPDATE_TRACE("art chapter block held ms=%ld filebuf=%lu", ms,
                     (unsigned long)audio_get_filebuflen());
        sleep(ms * HZ / 1000);
    }
#endif
    ART_PACE();                     /* (the block is checked below) */
    rc = chap_give_up("alloc") ? CHAP_DEFERRED : 0;
    if (rc == 0)
    {
        /* pinned for this read only */
        blk = core_get_data_pinned(h);
        ab_ch_init((struct audiobook_chapters *)blk, AB_MAX_CHAPTERS,
                   AB_CH_POOL);
        if (metadata_read_chapters(fd, &w->id3,
                                   (struct audiobook_chapters *)blk) > 0)
            count = MIN(((struct audiobook_chapters *)blk)->chapter_count,
                        AB_MAX_CHAPTERS);
        if (count <= 0)
            rc = -1;                /* promised chapters could not be read */
        core_put_data_pinned(blk);
    }
    ART_PACE();
    if (rc == 0 && count > 0 && chap_give_up("art"))
        rc = CHAP_DEFERRED;
    if (rc == 0 && count > 0)
    {
        struct ab_chapter_art *art;

        blk = core_get_data_pinned(h);
        ab = (struct audiobook_chapters *)blk;
        art = (struct ab_chapter_art *)(blk + chap_tab_off);
        CHECK_ALIGN(art);
        if (metadata_read_chapter_art(fd, &w->id3, ab, art) < 0)
            rc = -1;                /* do not publish an empty chapter set */
        core_put_data_pinned(blk);
    }
    for (i = 0; rc == 0 && i < count; i++)
        imgs += chap_art(h)[i].size != 0;
    if (rc == 0 && imgs && lent)
        rc = CHAP_LATER;
    ART_PACE();
    if (rc == 0 && imgs > 1)
        rc = chap_dedupe(h, fd, t->key, count, &dups, &reads);
    if (close(fd) != 0)
        rc = -1;
    if (rc == 0 && imgs)
        UPDATE_TRACE("art chapter dedupe images=%d copies=%d crc_reads=%d",
                     imgs, dups, reads);
    if (rc == 0 && !imgs)
    {
        /* chapters without images (most books): a success, the done mark
         * only, batched like a book without chapters (no index rewrite
         * per book, no room needed: it replaces the book's own record) */
        w->delta[w->n_delta] = *b;
        w->delta[w->n_delta++].flags |= ARTREC_F_CHAPTERS;
    }
    else if (rc == 0 && !artchap_fits(w->rd.hdr.count + w->n_delta,
                                      w->thdr.n_albums, imgs + 1))
        rc = CHAP_CAP;              /* checked before any decode: no
                                       bundle is written for nothing */
    else if (rc == 0)
        rc = chap_decode(w, t->key, h, count, &bundles);
    ART_PACE();
    if (rc == 0 && imgs && chap_give_up("write"))
        rc = CHAP_DEFERRED;         /* no index write while playing */
    if (rc == 0 && imgs)
    {
        /* pinned for the index write only */
        blk = core_get_data_pinned(h);
        recs = (struct artcache_rec *)blk;  /* over the chapter table */
        CHECK_ALIGN(recs);
        n = artchap_plan(t->key, t->max_mtime, count, chap_img,
                         blk + chap_tab_off, recs, AB_MAX_CHAPTERS);
        recs[n] = *b;
        recs[n++].flags |= ARTREC_F_CHAPTERS;
        memcpy(recs + n, w->delta, w->n_delta * sizeof(*recs));
        n += w->n_delta;
        w->n_delta = 0;
        n = artrec_sort_unique(recs, n);
        if (!index_write(w, recs, n, false, false))
            rc = CHAP_IO;
        core_put_data_pinned(blk);
    }
    if (!lent)
    {
        core_free(h);
        /* the next cold start sizes the audio buffer anew (as panels do) */
        audio_buffer_regrow();
        UPDATE_TRACE("art chapter block freed playing=%d",
                     (int)(audio_status() != 0));
    }
    UPDATE_TRACE("art build chapters book=" KEYFMT " chapters=%d images=%d "
                 "bundles=%d recs=%d rc=%d ms=%ld boost=%d", KEYARG(t->key),
                 count, imgs, bundles, n, rc,
                 (current_tick - t0) * (1000 / HZ), (int)ART_BOOST());
    return rc;
}

/* The chapter ledger (bwork.chskip) entry of book t, NULL if none */
static struct chap_skip *chap_skip_find(struct bwork *w,
                                        const struct artalb_rec *t)
{
    int i;

    for (i = 0; i < w->n_chskip; i++)
        if (w->chskip[i].key == t->key
            && w->chskip[i].max_mtime == t->max_mtime)
            return &w->chskip[i];
    return NULL;
}

/* Book t's chapter pass failed: its count of failed passes (at once
 * CHAP_TRIES when 'left': it can never fit) */
static int chap_skip_note(struct bwork *w, const struct artalb_rec *t,
                          bool left)
{
    struct chap_skip *e = chap_skip_find(w, t);

    if (!e)
    {
        if (w->n_chskip == CHAP_SKIP_MAX)   /* full: drop the oldest */
            memmove(&w->chskip[0], &w->chskip[1],
                    --w->n_chskip * sizeof(w->chskip[0]));
        e = &w->chskip[w->n_chskip++];
        e->key = t->key;
        e->max_mtime = t->max_mtime;
        e->tries = 0;
    }
    e->tries = left ? CHAP_TRIES : e->tries + 1;
    return e->tries;
}

/* After the panels, only while audio is stopped: every single-file book
 * of the table whose record exists for its mtime and is not marked done
 * gets its chapter pass. Playback start: chapters_owed, the rest at the
 * stop. A book rewritten for another mtime (chap_stale) has its old
 * chapter records dropped by a GC at the end. Returns the events that
 * ended it (0: done or deferred). */
/* A single-file book of the table whose record exists for its mtime and
 * has no chapter pass yet (an index read error sets *io_err). */
static bool chap_owed(struct bwork *w, const struct artalb_rec *t,
                      struct artcache_rec *b, bool *io_err)
{
    if (!t->key || !(t->flags & LIBFILE_ALB_BOOK) || t->count != 1)
        return false;               /* (multi-file books: the cover) */
    return artidx_lookup(&w->rd, t->key, b, io_err)
           && !artrec_is_chapter(b) && b->mtime == t->max_mtime
           && !(b->flags & ARTREC_F_CHAPTERS);
}

/* The books run_chapters() will visit, for the progress counter: one walk
 * of the album table, no reads beyond it and the index (0: unknown). */
static int chap_owed_count(struct bwork *w)
{
    struct artcache_rec b;
    uint32_t first = 0;
    int got, i, n = 0;
    bool io_err = false;

    while ((got = artalb_read(w->tpath, &w->thdr, first, TCHUNK,
                              w->trec)) > 0)
    {
        first += got;
        for (i = 0; i < got; i++)
        {
            ART_PACE();
            n += chap_owed(w, &w->trec[i], &b, &io_err);
        }
        if (io_err)
            return 0;
    }
    return n;
}

static int run_chapters(struct bwork *w)
{
    struct artcache_rec b;
    uint32_t first = 0;
    long t0 = current_tick;
    int got = 0, i, rc = 0, books = 0;
    struct chap_skip *sk;

    (void)t0; (void)books;          /* trace only */
    build_phase(ARTCACHE_BUILD_PHASE_CHAPTERS,
                art_hold() || w->rd.fd < 0 ? 0 : chap_owed_count(w));
    if (art_hold())
    {
        chapters_owed = true;
        DEFER(w);
        return 0;
    }
    if (w->rd.fd < 0)
    {
        FAIL(w);
        chapters_owed = true;
        return EV_RETRY;
    }
    while (rc == 0
           && (got = artalb_read(w->tpath, &w->thdr, first, TCHUNK,
                                 w->trec)) > 0)
    {
        first += got;
        for (i = 0; i < got && rc == 0; i++)
        {
            const struct artalb_rec *t = &w->trec[i];
            bool io_err = false;

            ART_PACE();
            if (!chap_owed(w, t, &b, &io_err))
            {
                if (io_err)
                    rc = CHAP_IO;
                continue;           /* absent/done is distinct from read error */
            }
            sk = chap_skip_find(w, t);
            if (sk && sk->tries >= CHAP_TRIES)
            {
                if (status.phase_done < status.phase_total)
                    build_advance();
                continue;           /* left (the cover) until a request */
            }
            if (!artchap_fits(w->rd.hdr.count + w->n_delta,
                              w->thdr.n_albums, 1))
            {
                rc = CHAP_CAP;
                break;
            }
            if (art_hold())
                rc = CHAP_DEFERRED;
            else if ((rc = wait_ready(true)) == 0)
                rc = art_hold() ? CHAP_DEFERRED   /* (an idle wait) */
                                : chapter_book(w, t, &b);
            if (status.phase_done < status.phase_total)
                build_advance();
            if (rc == CHAP_LATER)
            {
                chap_later = true;      /* at the stop or in a paused
                                           window (#88); not a failure */
                maint_want(MAINT_CHAPTERS);
                rc = 0;
                continue;
            }
            if (rc == -1 || rc == CHAP_CAP)
            {
                /* this book only: the others go on. A book that keeps
                 * failing stops failing the pass after CHAP_TRIES */
                int n = chap_skip_note(w, t, rc == CHAP_CAP);

                ui_log("art", "chap fail k=%08lx rc=%d n=%d",
                       (unsigned long)(t->key >> 32), rc, n);
                if (n < CHAP_TRIES)
                    FAIL(w);
                rc = 0;
                continue;
            }
            if (rc == 0)
                books++;
            if (rc == 0 && w->n_delta >= DELTA_MAX && !commit(w, false))
            {
                FAIL(w);
                chapters_owed = true;
                return EV_RETRY;
            }
        }
    }
    if (w->n_delta && (!io_ok() || !commit(w, false)))
    {
        FAIL(w);
        chapters_owed = true;
        return rc > 0 ? rc : EV_RETRY;
    }
    w->n_delta = 0;
    if (rc == CHAP_DEFERRED)
    {
        chapters_owed = true;
        DEFER(w);
        UPDATE_TRACE("art build chapters %s", "deferred");
        return 0;
    }
    if (rc == CHAP_CAP)
    {
        /* a design limit, not a failure: the rest keep the cover */
        UPDATE_TRACE("art build chapters cap recs=%lu",
                     (unsigned long)w->rd.hdr.count);
        rc = 0;
    }
    if (rc == CHAP_IO || got < 0)
    {
        FAIL(w);
        chapters_owed = true;
        return EV_RETRY;
    }
    if (rc)
        return rc;                  /* an event: the pass restarts */
    if (chap_stale && !playing())
    {
        /* the old chapter records of rewritten books (another mtime) */
        if (commit_ex(w, true, true))
            chap_stale = false;
        else
        {
            FAIL(w);
            chapters_owed = true;
            return EV_RETRY;
        }
    }
    UPDATE_TRACE("art build chapters done books=%d ms=%ld", books,
                 (current_tick - t0) * (1000 / HZ));
    return 0;
}

/* ======================================================================= */
/* builder: books.pak / books.idx (P6 §3.3, Rev 1 item 4, D7, D8)          */
/* ======================================================================= */
/* Every LIBFILE_ALB_BOOK row of the album table gets a blob in books.pak
 * (append-only) and a record in books.idx (rewritten whole: books.idx.tmp
 * + rename, generation + 1) whenever the index is missing, of another
 * version, or its {max_mtime, file_count} for the book is stale; this is
 * independent of the art records and of the chapter-art done bit. Only
 * while audio is stopped (books_owed otherwise, like the chapter art).
 *
 * The block, per book (8-aligned base): the builder's own decode block
 * (dec_h, idle during this pass; fix round L1: no core_alloc, so no
 * audio_buffer_regrow() either):
 *   X  [0, BK_X)          the per-file parse table, then the blob
 *   M  [BK_M, +BK_MAP)    inside X, behind the parse table: starts, crcs,
 *                         firsts (AB_MAP_FILES_MAX each); crcs[] first
 *                         holds the files' master indices (disc/track
 *                         order). ab_blob_build() copies the map to the
 *                         blob's front before the table overwrites it.
 *   T  [BK_X, +BK_T)      the book's entry table (book-relative starts)
 *   S  [BOOKS_BYTES, end) what is left of the decode block (the larger
 *                         of the 78,284 B thumbnail fill and the 98,588 B
 *                         Now Playing fit on the target: ~22.7 KB): the
 *                         chapter read window (fix 2)
 * Storage per book: one tagcache file-name read + one open and the
 * metadata/chapter reads per file; one blob append; one books.idx
 * rewrite per BOOKS_BATCH books (a read and a write of the index).
 *
 * Crash safety: always append at the actual pack end; orphaned and previous
 * generation blobs remain untouched. Flush the pack before a validated index
 * is published, retaining books.idx.prev. Two-file compaction is disabled
 * until pack/index generations can be switched as one transaction. */
#define BK_X        ALIGN_UP(AB_BLOB_MAX_SIZE, 8)
#define BK_T        ALIGN_UP(AB_CH_TABLE_SIZE, 8)
#define BK_MAP      ((size_t)AB_MAP_FILES_MAX * 10)
#define BK_M        ALIGN_UP(AB_CH_TABLE_SIZE, 8)
#define BOOKS_BYTES (BK_X + BK_T)       /* 75,832 */
/* the map fits behind the parse table, and lands in the blob below it */
typedef char bk_map_fits[(BK_M + BK_MAP <= BK_X
                          && sizeof(struct ab_blob_hdr)
                             + AB_BLOB_MAP_SIZE(AB_MAP_FILES_MAX) <= BK_M)
                         ? 1 : -1];
#define BIDX_HDR    ((off_t)sizeof(struct ab_idx_hdr))
#define BIDX_PAGE   ((int)(ARTIDX_PAGE_BYTES / sizeof(struct ab_idx_rec)))

struct bkmem {
    struct audiobook_chapters *parse, *tab;
    void *blob;
    uint32_t *starts, *crcs;
    uint16_t *firsts;
    void *scratch;                      /* chapter read window, or NULL */
    size_t scratch_size;
};

static void bk_layout(uint8_t *blk, size_t bytes, struct bkmem *m)
{
    uint8_t *b = (uint8_t *)ALIGN_UP((uintptr_t)blk, 8);
    size_t used = (size_t)(b - blk) + BOOKS_BYTES;

    m->parse = (struct audiobook_chapters *)b;
    m->blob = b;
    m->starts = (uint32_t *)(b + BK_M);
    m->crcs = m->starts + AB_MAP_FILES_MAX;
    m->firsts = (uint16_t *)(m->crcs + AB_MAP_FILES_MAX);
    m->tab = (struct audiobook_chapters *)(b + BK_X);
    m->scratch = b + BOOKS_BYTES;
    m->scratch_size = bytes > used ? bytes - used : 0;
}

static enum artidx_open_result bk_read_exact(int fd, void *buf, size_t n)
{
    char *p = buf;
    while (n)
    {
        ssize_t got = read(fd, p, n);
        if (got < 0)
            return ARTIDX_IO_ERROR;
        if (!got)
            return ARTIDX_CORRUPT;
        p += got;
        n -= got;
    }
    return ARTIDX_OPENED;
}

/* ab_word_sum() of a run that starts at word *wi of the summed whole,
 * without the seed (added with the header's part) */
static uint32_t bk_sum_add(uint32_t s, const void *p, size_t n, uint32_t *wi)
{
    const uint32_t *v = p;
    size_t i;

    for (i = 0; i < n / 4; i++)
        s += v[i] ^ (*wi)++;
    return s;
}

/* seals h: ab_word_sum() over the header (sum = 0) and the records, whose
 * part (from word 8 on) is recs_part */
static void bk_hdr_seal(struct ab_idx_hdr *h, uint32_t recs_part)
{
    h->magic = AB_IDX_MAGIC;
    h->version = AB_PAK_VERSION;
    h->rec_size = sizeof(struct ab_idx_rec);
    h->spare = 0;
    h->sum = 0;
    h->sum = ab_word_sum(h, sizeof(*h)) + recs_part;
}

static off_t bk_size(int fd)
{
    return lseek(fd, 0, SEEK_END);
}

/* Validate before publication/recovery; failed storage access is not proof
 * of corruption. Uses the builder's existing page, no allocation. */
static enum artidx_open_result books_check(struct bwork *w, const char *path,
                                           struct ab_idx_hdr *hdr)
{
    struct ab_idx_hdr h;
    uint32_t part = 0, wi = 8, left, sum;
    ab_key_t last = 0;
    enum artidx_open_result rc;
    int fd = open(path, O_RDONLY);
    off_t bytes;

    if (fd < 0)
        return errno == ENOENT ? ARTIDX_MISSING : ARTIDX_IO_ERROR;
    rc = bk_read_exact(fd, &h, sizeof(h));
    if (rc != ARTIDX_OPENED)
        goto out;
    if (h.magic != AB_IDX_MAGIC || h.version != AB_PAK_VERSION
        || h.rec_size != sizeof(struct ab_idx_rec) || h.count > 0xffff
        || h.dead_bytes > h.pak_size)
        goto corrupt;
    bytes = bk_size(fd);
    if (bytes < 0 || lseek(fd, BIDX_HDR, SEEK_SET) != BIDX_HDR)
        goto io_error;
    if (bytes != BIDX_HDR + (off_t)h.count * h.rec_size)
        goto corrupt;
    for (left = h.count * sizeof(struct ab_idx_rec); left; )
    {
        struct ab_idx_rec *r = (struct ab_idx_rec *)w->page;
        uint32_t n = MIN(left, BIDX_PAGE * sizeof(*r)), i;
        rc = bk_read_exact(fd, w->page, n);
        if (rc != ARTIDX_OPENED)
            goto out;
        for (i = 0; i < n / sizeof(*r); i++)
        {
            if (!r[i].book_key || r[i].book_key <= last || !r[i].size
                || (r[i].offset & 7) || (r[i].size & 7)
                || (uint64_t)r[i].offset + r[i].size > h.pak_size)
                goto corrupt;
            last = r[i].book_key;
        }
        part = bk_sum_add(part, w->page, n, &wi);
        left -= n;
    }
    sum = h.sum;
    bk_hdr_seal(&h, part);
    if (h.sum != sum)
        goto corrupt;
    if (close(fd) != 0)
        return ARTIDX_IO_ERROR;
    fd = open(F_BPAK, O_RDONLY);
    if (fd < 0)
        return errno == ENOENT ? ARTIDX_CORRUPT : ARTIDX_IO_ERROR;
    bytes = bk_size(fd);
    if (bytes < 0)
        goto io_error;
    if (bytes < (off_t)h.pak_size)
        goto corrupt;
    *hdr = h;
    rc = ARTIDX_OPENED;
    goto out;
corrupt:
    rc = ARTIDX_CORRUPT;
    goto out;
io_error:
    rc = ARTIDX_IO_ERROR;
out:
    if (close(fd) != 0)
        rc = ARTIDX_IO_ERROR;
    return rc;
}

static void books_load(struct bwork *w)
{
    struct ab_idx_hdr h;
    enum artidx_open_result primary = books_check(w, F_BIDX, &h), previous;
    w->bpath = F_BIDX;
    previous = ARTIDX_MISSING;
    if (primary != ARTIDX_OPENED)
    {
        previous = books_check(w, F_BIDX_PREV, &h);
        if (previous == ARTIDX_OPENED)
            w->bpath = F_BIDX_PREV;
    }
    w->bhdr_ok = primary == ARTIDX_OPENED || previous == ARTIDX_OPENED;
    w->books_defer = primary == ARTIDX_IO_ERROR || previous == ARTIDX_IO_ERROR;
    if (!w->bhdr_ok)
        memset(&h, 0, sizeof(h));
    w->bhdr = h;
    w->n_bpend = 0;
    UPDATE_TRACE("art books load primary=%d previous=%d defer=%d", primary,
                 previous, w->books_defer);
}

/* key's record in books.idx (open fd), by binary search: true if found */
static bool books_lookup(struct bwork *w, int fd, ab_key_t key,
                         struct ab_idx_rec *out, bool *io_err)
{
    int lo = 0, hi = (int)w->bhdr.count - 1;

    *io_err = false;
    if (fd < 0 || !w->bhdr_ok)
    {
        *io_err = w->bhdr_ok && w->bhdr.count;
        return false;
    }
    while (lo <= hi)
    {
        int mid = lo + (hi - lo) / 2;
        off_t off = BIDX_HDR + (off_t)mid * sizeof(*out);

        if (lseek(fd, off, SEEK_SET) != off
            || read(fd, out, sizeof(*out)) != (ssize_t)sizeof(*out))
        {
            *io_err = true;
            return false;
        }
        if (out->book_key == key)
            return true;
        if (out->book_key < key)
            lo = mid + 1;
        else
            hi = mid - 1;
    }
    return false;
}

/* writes n records to fd, adding them to the sum */
static bool bk_write_recs(int fd, const struct ab_idx_rec *r, int n,
                          uint32_t *part, uint32_t *wi)
{
    ssize_t want = (ssize_t)n * sizeof(*r);

    if (n <= 0)
        return true;
    *part = bk_sum_add(*part, r, want, wi);
    return write(fd, r, want) == want;
}

/* The books built since the last rewrite go into books.idx: a merge of
 * the old records (read through w->page) with w->bpend (sorted here; a
 * pending record replaces the old one of its key) into books.idx.tmp,
 * then the rename. Never while audio plays (the built books are then
 * dropped: their blobs are orphans, rebuilt at the stop). */
static bool books_flush(struct bwork *w)
{
    struct ab_idx_hdr h = w->bhdr;
    struct ab_idx_rec *p = w->bpend;
    uint32_t part = 0, wi = 8, k = 0, count = 0;
    int i, j, np = w->n_bpend, in = -1, out;
    bool ok;

    if (!np)
        return true;
    w->n_bpend = 0;
    if (art_hold() || !io_ok() || w->books_defer)
    {
        if (w->books_defer)
            FAIL(w);
        else
            DEFER(w);
        UPDATE_TRACE("art build books dropped n=%d", np);
        books_owed = true;
        books_load(w);                  /* pak_size as on disk */
        return false;
    }
    for (i = 1; i < np; i++)            /* insertion sort by key */
    {
        struct ab_idx_rec t = p[i];

        for (j = i; j > 0 && p[j - 1].book_key > t.book_key; j--)
            p[j] = p[j - 1];
        p[j] = t;
    }
    ART_PACE();
    out = artfile_tmp_clear(F_BIDX_TMP, F_BIDX)         /* #91 H1 */
        ? open(F_BIDX_TMP, O_WRONLY | O_CREAT | O_TRUNC, 0666) : -1;
    ok = out >= 0 && write(out, &h, sizeof(h)) == (ssize_t)sizeof(h);
    if (ok && w->bhdr.count)
    {
        in = open(w->bpath, O_RDONLY);
        ok = in >= 0 && lseek(in, BIDX_HDR, SEEK_SET) == BIDX_HDR;
    }
    i = 0;
    while (ok && in >= 0 && k < w->bhdr.count)
    {
        struct ab_idx_rec *r = (struct ab_idx_rec *)w->page;
        int n = MIN((int)(w->bhdr.count - k), BIDX_PAGE), run = 0;

        ART_PACE();
        if (read(in, r, n * sizeof(*r)) != (ssize_t)(n * sizeof(*r)))
        {
            ok = false;
            break;
        }
        for (j = 0; ok && j < n; j++)
        {
            bool same = false;

            while (ok && i < np && p[i].book_key <= r[j].book_key)
            {
                same |= p[i].book_key == r[j].book_key;
                ok = bk_write_recs(out, r + run, j - run, &part, &wi)
                     && bk_write_recs(out, &p[i++], 1, &part, &wi);
                count += j - run + 1;
                run = j;
            }
            if (same)
                run = j + 1;            /* the old record is replaced */
        }
        ok = ok && bk_write_recs(out, r + run, n - run, &part, &wi);
        count += n - run;
        k += n;
    }
    if (in >= 0)
        close(in);
    ok = ok && bk_write_recs(out, p + i, np - i, &part, &wi);
    count += np - i;
    h.count = count;
    h.generation++;
    bk_hdr_seal(&h, part);
    ok = ok && lseek(out, 0, SEEK_SET) == 0
         && write(out, &h, sizeof(h)) == (ssize_t)sizeof(h)
         && fsync(out) == 0;
    if (out >= 0 && close(out) < 0)
        ok = false;
    ART_PACE();
    if (ok)
    {
        struct ab_idx_hdr checked;
        ok = books_check(w, F_BIDX_TMP, &checked) == ARTIDX_OPENED
             && checked.generation == h.generation;
    }
    ART_PACE();
    if (ok)
    {
        struct ab_idx_hdr old;
        enum artidx_open_result rc = books_check(w, F_BIDX, &old);
        /* An absent/damaged primary must never displace the validated prev.
         * Revalidate: a fresh append also sets bhdr_ok before any index
         * exists, so bhdr_ok alone cannot authorize rotating the primary. */
        if (rc == ARTIDX_IO_ERROR)
            ok = false;
        else if (rc == ARTIDX_OPENED)
            ok = artfile_publish(F_BIDX, F_BIDX_TMP, F_BIDX_PREV)
                 == ARTFILE_PUBLISHED;
        else
            ok = artfile_replace(F_BIDX_TMP, F_BIDX) == 0;
    }
    if (!ok)
    {
        FAIL(w);
        UPDATE_TRACE("art io books %s", "publication-failed-retained");
        books_owed = true;
        books_load(w);                  /* what is on disk */
        return false;
    }
    w->bhdr = h;
    w->bhdr_ok = true;
    w->bpath = F_BIDX;
    UPDATE_TRACE("art build books commit gen=%lu recs=%lu pak=%lu dead=%lu",
                 (unsigned long)h.generation, (unsigned long)h.count,
                 (unsigned long)h.pak_size, (unsigned long)h.dead_bytes);
    return true;
}

/* Compaction needs a manifest transaction for the pack and its index. Until
 * then keep append-only blobs, including those used by the retained index. */
static void books_compact(struct bwork *w)
{
    (void)w;
}

/* -- a book's files, as lib_open(LIB_AUDIOBOOKS, album filter) lists them:
 * the audiobook entries of the RAM index with the row's album (case-
 * insensitive) and effective artist (album artist, else artist); a book
 * without an album tag is its representative alone. Disc, then track
 * (missing last), then title (ui_sortkey()), then master index. The
 * queue lib_play() builds may still differ (a tie broken otherwise):
 * the map's path CRC catches that at play time (ab map mismatch). */
static const char *bk_str(const int32_t *s, int tag)
{
    return tagcache_ui_str_ctx(bw->context, tag, s[tag]);
}

static const char *bk_artist(const int32_t *s)
{
    const char *a = bk_str(s, tag_albumartist);

    return a ? a : bk_str(s, tag_artist);
}

static bool bk_eq(const char *a, const char *b)  /* b: copy, "" = none */
{
    return a ? !ui_casefold_cmp(a, b) : !b[0];     /* as library.c */
}

static int bk_cmp(int a, int b)
{
    const int32_t *sa = tagcache_ui_seeks_ctx(bw->context, a, NULL);
    const int32_t *sb = tagcache_ui_seeks_ctx(bw->context, b, NULL);
    char ka[128], kb[128];
    long ta, tb;
    int r;

    if (!sa || !sb)
        return (a > b) - (a < b);
    if (sa[tag_discnumber] != sb[tag_discnumber])
        return sa[tag_discnumber] < sb[tag_discnumber] ? -1 : 1;
    ta = sa[tag_tracknumber] > 0 ? sa[tag_tracknumber] : LONG_MAX;
    tb = sb[tag_tracknumber] > 0 ? sb[tag_tracknumber] : LONG_MAX;
    if (ta != tb)
        return ta < tb ? -1 : 1;
    ui_sortkey(bk_str(sa, tag_title), ka, sizeof(ka));
    ui_sortkey(bk_str(sb, tag_title), kb, sizeof(kb));
    r = strcmp(ka, kb);
    return r ? r : (a > b) - (a < b);
}

/* Fills idx[] (at most max) and returns the book's file count (may be
 * > max: the caller skips the book), or -1 (none, or the database
 * changed while it yielded). P6 fix L2: the library's own file list
 * first (the precompute file's LIB_AUDIOBOOKS TRACKS, read with 'buf'
 * as the search room: one open and a few reads, no database walk, the
 * grouping and order lib_play() queues); *walked is set when that is
 * missing and the album + artist walk below decided instead. */
static int book_files(struct bwork *w, const struct artalb_rec *t,
                      uint32_t *idx, int max, void *buf, size_t size,
                      bool *walked)
{
    int32_t flag;
    const int32_t *s = tagcache_ui_seeks_ctx(w->context, t->rep_idx, &flag);
    const char *p;
    int gen = w->db_gen, n = 0;
    int total = tagcache_ui_count_ctx(w->context), e, i, j;

    *walked = false;
    if (!s)
        return -1;
    struct tagcache_context *operation = tagcache_context_acquire();
    if (operation != w->context)
    {
        if (operation)
            tagcache_context_release(operation);
        return -1;
    }
    n = lib_book_tracks(t->rep_idx, idx, max, buf, size);
    tagcache_context_release(operation);
    if (n > 0)
        return n;
    /* lib_book_tracks() may yield while loading prepared lists. The
     * retained namespace is still valid, but its RAM block may have moved. */
    s = tagcache_ui_seeks_ctx(w->context, t->rep_idx, &flag);
    if (!s)
        return -1;
    n = 0;
    *walked = true;
    p = bk_str(s, tag_album);
    if (!p)
    {
        idx[0] = t->rep_idx;            /* a book of its own, by title */
        return 1;
    }
    strmemccpy(w->album, p, MAX_PATH);
    p = bk_artist(s);
    strmemccpy(w->aartist, p ? p : "", MAX_PATH);
    for (e = 0; e < total; e++)
    {
        if ((e & 511) == 511)
        {
            yield();
            if (tagcache_ui_generation() != gen)
                return -1;
        }
        s = tagcache_ui_seeks_ctx(w->context, e, &flag);
        if (!s || !(flag & TAGCACHE_UI_AUDIOBOOK)
            || !(p = bk_str(s, tag_album)) || ui_casefold_cmp(p, w->album)
            || !bk_eq(bk_artist(s), w->aartist))
            continue;
        if (n < max)
            idx[n] = e;
        n++;
    }
    if (n > max || n == 0)
        return n ? n : -1;
    for (i = 1; i < n; i++)             /* insertion, no yield */
    {
        uint32_t v = idx[i];

        for (j = i; j > 0 && bk_cmp(idx[j - 1], v) > 0; j--)
            idx[j] = idx[j - 1];
        idx[j] = v;
    }
    return n;
}

static bool bk_skipped(const struct bwork *w, const struct artalb_rec *t)
{
    int i;

    for (i = 0; i < w->n_bskip; i++)
        if (w->bskip[i].key == t->key)
            return w->bskip[i].max_mtime == t->max_mtime
                   && w->bskip[i].count == t->count;
    return false;
}

static void bk_skip(struct bwork *w, const struct artalb_rec *t)
{
    int i;

    for (i = 0; i < w->n_bskip && w->bskip[i].key != t->key; i++)
        ;
    if (i == BK_SKIP_MAX)               /* full: drop the oldest */
    {
        memmove(&w->bskip[0], &w->bskip[1],
                (BK_SKIP_MAX - 1) * sizeof(w->bskip[0]));
        i = BK_SKIP_MAX - 1;
    }
    else if (i == w->n_bskip)
        w->n_bskip++;
    w->bskip[i].key = t->key;
    w->bskip[i].max_mtime = t->max_mtime;
    w->bskip[i].count = t->count;
}

/* One book: its files' chapters (or the files) into a blob, appended to
 * books.pak; its record into w->bpend. 0 built, -1 skipped (again next
 * pass), BK_UNBUILDABLE (remembered: not again until it changes),
 * CHAP_DEFERRED (playback started: given up at once). The work happens
 * in the decode block (fix round L1: no allocation of its own). */
#define BK_UNBUILDABLE (-4)         /* distinct from CHAP_DEFERRED */
static int book_build(struct bwork *w, const struct artalb_rec *t,
                      const struct ab_idx_rec *old)
{
    struct bkmem m;
    struct ab_idx_rec rec;
    uint8_t *blk;
    uint32_t len = 0;
    size_t size = 0;
    long t0 = current_tick;
    int n, q, entries = 0, chaps = 0, rc = 0, fd;
    bool walked = false;

    (void)t0; (void)chaps;              /* trace only */
    if (t->count > AB_MAP_FILES_MAX)
    {
        UPDATE_TRACE("art build books skip book=" KEYFMT " files=%lu",
                     KEYARG(t->key), (unsigned long)t->count);
        return BK_UNBUILDABLE;
    }
    if (art_hold())
        return CHAP_DEFERRED;
    /* pinned for the book: every step below reads into it */
    blk = core_get_data_pinned(dec_h);
    bk_layout(blk, decode_bytes, &m);
    CHECK_ALIGN(m.tab);
    ab_ch_init(m.tab, AB_MAX_CHAPTERS, AB_CH_POOL);
    /* the parse table's room is free until the first file is read */
    n = book_files(w, t, m.crcs, AB_MAP_FILES_MAX, m.parse, BK_M, &walked);
    if (n < 1 || n > AB_MAP_FILES_MAX || (uint32_t)n != t->count)
    {
        /* the walk disagreeing with the row's count on a database that
         * did not change is for good (the grouping differs); anything
         * else is a stale table: the next pass */
        rc = walked && n > 0 && tagcache_ui_generation() == w->db_gen
             ? BK_UNBUILDABLE : -1;
        UPDATE_TRACE("art build books files book=" KEYFMT " n=%d count=%lu "
                     "walked=%d", KEYARG(t->key), n,
                     (unsigned long)t->count, (int)walked);
    }
    for (q = 0; rc == 0 && q < n; q++)
    {
        int c = 0, i;
        const char *title;

        ART_PACE();                 /* (blk is pinned) */
        if (chap_give_up("books"))
        {
            rc = CHAP_DEFERRED;
            break;
        }
        if (!tagcache_search_ctx(w->context, &w->tcs, tag_filename))
        {
            rc = -1;
            break;
        }
        if (!tagcache_retrieve(&w->tcs, m.crcs[q], tag_filename, w->path,
                               sizeof(w->path)))
            rc = -1;
        tagcache_search_finish(&w->tcs);
        if (rc)
            break;
        memset(&w->id3, 0, sizeof(w->id3));
        ART_PACE();
        fd = open(w->path, O_RDONLY);
        if (fd < 0 || !get_metadata(&w->id3, fd, w->path))
            rc = -1;
        else if (w->id3.chapter_count > 0)
        {
            ab_ch_init(m.parse, AB_MAX_CHAPTERS, AB_CH_POOL);
            if (metadata_read_chapters_buf(fd, &w->id3, m.parse, m.scratch,
                                           m.scratch_size) > 0)
                c = m.parse->chapter_count;
            if (c <= 0)
                rc = -1;            /* failed chapter read is not no chapters */
        }
        if (fd >= 0 && close(fd) != 0)
            rc = -1;
        if (rc)
            break;                  /* retain the previous complete blob */
        m.starts[q] = len;
        m.crcs[q] = ab_path_crc(w->path);
        m.firsts[q] = (uint16_t)MIN(entries, AB_MAX_CHAPTERS - 1);
        /* fair share: this file's chapters + at least one per file left */
        ab_ch_expect(m.tab, entries + MAX(c, 1) + (n - 1 - q));
        for (i = 0; i < c && entries < AB_MAX_CHAPTERS; i++)
        {
            const char *s = ab_ch_title_ptr(m.parse, i);

            ab_ch_put(m.tab, entries++, len + ab_ch_start(m.parse, i), s,
                      strlen(s));
        }
        chaps += c;
        if (c <= 0 && entries < AB_MAX_CHAPTERS)
        {
            title = w->id3.title;
            if (!title || !title[0])
            {
                title = strrchr(w->path, '/');
                title = title ? title + 1 : w->path;
            }
            ab_ch_put(m.tab, entries, len, title, strlen(title));
            ab_ch_ent(m.tab)[entries++].flags |= AB_CH_F_FILE;
        }
        m.tab->chapter_count = entries;
        len += w->id3.length;
    }
    if (rc == 0 && tagcache_ui_generation() != w->db_gen)
        rc = -1;
    if (rc == 0)
    {
        size = ab_blob_build(m.blob, BK_X, t->key, t->max_mtime, len, n,
                             m.starts, m.crcs, m.firsts, m.tab);
        if (!size)
            rc = -1;
    }
    if (rc == 0 && chap_give_up("books-write"))
        rc = CHAP_DEFERRED;             /* no pak write while playing */
    if (rc == 0)
    {
        uint32_t off = 0;
        off_t end;

        ART_PACE();
        fd = w->books_defer ? -1 : open(F_BPAK, O_WRONLY | O_CREAT, 0666);
        end = fd >= 0 ? bk_size(fd) : -1;
        if (end < 0 || (uint64_t)end + 7 + size > UINT32_MAX)
            rc = -1;
        else
        {
            off = ALIGN_UP((uint32_t)end, 8);
            if (lseek(fd, off, SEEK_SET) != (off_t)off
                || write(fd, m.blob, size) != (ssize_t)size
                || fsync(fd) != 0)
                rc = -1;
        }
        if (fd >= 0 && close(fd) < 0)
            rc = -1;
        if (rc == 0)
        {
            rec.book_key = t->key;
            rec.max_mtime = t->max_mtime;
            rec.file_count = (uint16_t)n;
            rec.flags = ((const struct ab_blob_hdr *)m.blob)->flags;
            rec.offset = off;
            rec.size = size;
            if (!w->bhdr_ok)
            {
                w->bhdr_ok = true;      /* new index, existing pak retained */
                w->bhdr.count = 0;
                w->bhdr.dead_bytes = 0;
                old = NULL;
            }
            w->bhdr.pak_size = off + size;
            if (old)
                w->bhdr.dead_bytes += old->size;
            w->bpend[w->n_bpend++] = rec;
        }
        else
            UPDATE_TRACE("art io books %s", "append-failed");
    }
    core_put_data_pinned(blk);
    if (rc == -1)                       /* BK_UNBUILDABLE: no art to build,
                                         * done (run_books() logs it) */
        FAIL(w);
    UPDATE_TRACE("art build books book=" KEYFMT " files=%d entries=%d "
                 "chapters=%d bytes=%lu rc=%d ms=%ld files_from=%s",
                 KEYARG(t->key), n, entries, chaps, (unsigned long)size, rc,
                 (current_tick - t0) * (1000 / HZ),
                 walked ? "walk" : "tracks");
    return rc;
}

/* After the chapter art, only while stopped: every book row owed a blob.
 * The rows of one album-table chunk are checked against books.idx with
 * the file open (no wait in between), then each owed one is built after
 * wait_ready(). Returns the events that ended it (0: done or deferred). */
static int run_books(struct bwork *w)
{
    struct ab_idx_rec old[TCHUNK];
    uint64_t owed, had;
    uint32_t first = 0;
    long t0 = current_tick;
    int got = 0, i, fd, rc = 0, built = 0, skipped = 0;

    (void)t0;                           /* trace only */
    build_phase(ARTCACHE_BUILD_PHASE_BOOKS, 0);
    if (art_hold())
    {
        books_owed = true;
        DEFER(w);
        return 0;
    }
    ART_PACE();
    books_load(w);
    ART_PACE();
    if (w->books_defer)
    {
        FAIL(w);
        books_owed = true;
        return EV_RETRY;
    }
    while (rc == 0
           && (got = artalb_read(w->tpath, &w->thdr, first, TCHUNK,
                                 w->trec)) > 0)
    {
        first += got;
        owed = had = 0;
        ART_PACE();
        fd = w->bhdr_ok && w->bhdr.count ? open(w->bpath, O_RDONLY) : -1;
        for (i = 0; i < got; i++)
        {
            const struct artalb_rec *t = &w->trec[i];
            bool io_err = false;

            if (!t->key || !(t->flags & LIBFILE_ALB_BOOK))
                continue;
            if (bk_skipped(w, t))
            {
                /* known unbuildable and unchanged: nothing to build, so
                 * done for the status (owner session 2026-10-19: a
                 * permanent "Artwork Update Pending" otherwise) */
                skipped++;
                continue;
            }
            ART_PACE();
            if (books_lookup(w, fd, t->key, &old[i], &io_err))
            {
                had |= 1ull << i;
                if (ab_idx_valid(&old[i], t->max_mtime, t->count))
                    continue;
            }
            if (io_err)
            {
                rc = -1;
                break;
            }
            owed |= 1ull << i;
        }
        if (fd >= 0)
            close(fd);
        for (i = 0; i < got && rc == 0; i++)
        {
            if (!(owed & (1ull << i)))
                continue;
            if (art_hold())
                rc = CHAP_DEFERRED;
            else if ((rc = wait_ready(true)) == 0)
                rc = art_hold() ? CHAP_DEFERRED
                     : book_build(w, &w->trec[i],
                                  (had & (1ull << i)) ? &old[i] : NULL);
            if (rc == BK_UNBUILDABLE)
            {
                /* for good until the book changes: remembered, logged
                 * once, and not a failure (no art can come of it) */
                ui_log("art", "book skip k=%08lx files=%lu",
                       (unsigned long)(w->trec[i].key >> 32),
                       (unsigned long)w->trec[i].count);
                bk_skip(w, &w->trec[i]);
                skipped++;
                rc = 0;
            }
            else if (rc == 0)
                built++;
            if (rc == 0 && w->n_bpend >= BOOKS_BATCH && !books_flush(w))
                rc = -1;
        }
    }
    if (!books_flush(w))
    {
        FAIL(w);
        books_owed = true;
        return rc > 0 ? rc : EV_RETRY;
    }
    if (rc == CHAP_DEFERRED)
    {
        books_owed = true;
        DEFER(w);
        UPDATE_TRACE("art build books %s", "deferred");
        return 0;
    }
    if (rc == -1 || got < 0 || w->failed)
    {
        FAIL(w);
        books_owed = true;
        return rc > 0 ? rc : EV_RETRY;
    }
    if (rc)
        return rc;                      /* an event: the pass restarts */
    books_compact(w);
    (void)built; (void)skipped;         /* trace only */
    UPDATE_TRACE("art build books done built=%d skipped=%d ms=%ld", built,
                 skipped, (current_tick - t0) * (1000 / HZ));
    return 0;
}

/* ======================================================================= */
/* builder: what the user sees first (#58)                                 */
/* ======================================================================= */
/* The table records of the keys want[0..nw-1] into out[] (in table
 * order), by one walk of the album table in chunks as large as the
 * decode block (98 KB: ~4,100 records per read(), so 5,000 albums are 2
 * reads). Returns how many were found. */
static int table_find(struct bwork *w, const uint64_t *want, int nw,
                      struct artalb_rec *out)
{
    const size_t fixed = offsetof(struct libfile_hdr, sec);
    const size_t esz = sizeof(struct libfile_album);
    ARTFMT_LIBHDR_FIXED(h);
    uint32_t first = 0, done = 0;
    int fd, nf = 0, reads = 0;

    fd = open(w->tpath, O_RDONLY);
    if (fd < 0)
        return 0;
    /* the same file the pass checked (library.c may have replaced it) */
    if (read(fd, &h, fixed) != (ssize_t)fixed
        || memcmp(&h, &w->thdr.lib, fixed))
        first = w->thdr.n_albums;
    while (first < w->thdr.n_albums && nf < nw)
    {
        uint32_t n = MIN(w->thdr.n_albums - first, w->dec_bytes / esz), k;
        off_t at = w->thdr.offset + (off_t)first * esz;
        char *buf = core_get_data_pinned(w->dec);
        bool ok = lseek(fd, at, SEEK_SET) == at
                  && read(fd, buf, n * esz) == (ssize_t)(n * esz);

        reads++;
        for (k = 0; ok && k < n && nf < nw; k++)
        {
            const struct libfile_album *e =
                (const struct libfile_album *)(buf + k * esz);
            uint64_t key = (uint64_t)e->key_hi << 32 | e->key_lo;
            int j;

            if (!(k & 255))
                ART_PACE();         /* (buf is pinned) */

            for (j = 0; j < nw; j++)
                if (!(done & (1u << j)) && key && key == want[j])
                {
                    done |= 1u << j;
                    out[nf].key = key;
                    out[nf].rep_idx = e->rep;
                    out[nf].max_mtime = e->mtime;
                    out[nf].flags = e->flags;
                    out[nf].count = e->count;
                    if (e->rep >= 0 && e->rep < w->thdr.lib.entries)
                        nf++;
                    break;
                }
        }
        core_put_data_pinned(buf);
        if (!ok)
            break;
        first += n;
    }
    close(fd);
    (void)reads;                    /* trace only */
    UPDATE_TRACE("art build hints want=%d found=%d reads=%d", nw, nf, reads);
    return nf;
}

/* True if album_early() already did 'key' this pass */
static bool early_done(const struct bwork *w, artcache_key_t key)
{
    int i;

    for (i = 0; i < w->n_early; i++)
        if (w->early[i] == key)
            return true;
    return false;
}

/* Builds one album out of order (a hint, the panel set): like the walk,
 * but the walk's count of index records it found stays as it was (it
 * meets the album again later, and only counts it: early_done()).
 * Returns album()'s result; *ev: the events that ended a wait for the
 * next idle (the user came back mid-album, #58), else 0. */
static int album_early(struct bwork *w, const struct artalb_rec *t,
                       const char *why, int *ev)
{
    struct artcache_rec rec;
    uint32_t matched = w->matched;
    long a0 = current_tick;
    int rc;

    (void)why; (void)a0;            /* trace only */
    *ev = 0;
    if (w->n_delta >= DELTA_MAX && !commit(w, false))
    {
        *ev = EV_RETRY;
        return -1;
    }
    while ((rc = album(w, t, &rec)) == ALBUM_LIT
           && !(*ev = wait_retry()))
        a0 = current_tick;
    ART_PACE();
    w->matched = matched;
    if (rc >= 0 && w->n_early < ART_EARLY)
        w->early[w->n_early++] = t->key;   /* done: the walk skips it */
    if (rc == 1)
    {
        w->delta[w->n_delta++] = rec;
        UPDATE_TRACE("art build album key=" KEYFMT " state=%d src=%d "
                     "ms=%ld boost=%d first=%s", KEYARG(rec.key), rec.state,
                     rec.src_kind, (current_tick - a0) * (1000 / HZ),
                     (int)ART_BOOST(), why);
        if (w->n_delta >= DELTA_MAX && io_ok())
            commit(w, false);
    }
    return rc;
}

/* The albums the UI found PENDING since the last look (hint_add(): list
 * rows on screen, the Now Playing album), not built yet: found in the
 * table, built, and committed at once so the UI shows them. A key the
 * table does not have (a chapter key, a row of another database) is
 * remembered for the pass and not looked for again. Returns the events
 * that ended it (0: done). */
static int run_hints(struct bwork *w)
{
    uint64_t want[ART_HINTS];
    struct artalb_rec rec[ART_HINTS];
    struct artcache_rec r;
    int seq = hint_seq, nw = 0, nf, i, j, ev, rc = 1;
    enum artcache_build_phase saved_phase;
    int saved_done, saved_total;

    if (seq == w->hint_seen)
        return 0;
    w->hint_seen = seq;
    for (i = 0; i < ART_HINTS; i++)
    {
        uint64_t k = hint_keys[i];
        bool skip = !k;

        for (j = 0; !skip && j < w->hmiss_n; j++)
            skip = w->hmiss[j] == k;
        for (j = 0; !skip && j < w->n_delta; j++)
            skip = w->delta[j].key == k;
        for (j = 0; !skip && j < nw; j++)
            skip = want[j] == k;
        if (!skip && !artidx_lookup(&w->rd, k, &r, NULL))
            want[nw++] = k;
    }
    if (!nw)
        return 0;
    ART_PACE();
    nf = table_find(w, want, nw, rec);
    ART_PACE();
    for (i = 0; i < nw; i++)
    {
        for (j = 0; j < nf && rec[j].key != want[i]; j++)
            ;
        if (j == nf)
        {
            /* not in the table: not again this pass */
            w->hmiss[w->hmiss_at++ % ART_HMISS] = want[i];
            w->hmiss_n = MIN(w->hmiss_n + 1, ART_HMISS);
        }
    }
    if (!nf)
        return 0;
    saved_phase = status.phase;
    saved_done = status.phase_done;
    saved_total = status.phase_total;
    build_phase(ARTCACHE_BUILD_PHASE_REQUESTED, nf);
    for (i = 0; i < nf; i++)
    {
        ev = wait_ready(rc != 0);
        if (!ev)
            rc = album_early(w, &rec[i], "hint", &ev);
        if (ev)
            return ev;
        build_advance();            /* checked, including a failed source */
    }
    if (w->n_delta && io_ok())
        commit(w, false);
    status.phase_done = saved_done;
    status.phase_total = saved_total;
    status.phase = saved_phase;
    return 0;
}

/* The start of every pass: the hinted albums, then the panel set's
 * albums (thumbnails), committed, then their panels, so the drift fills
 * first (owner, device feedback #58). Returns the events that ended it. */
static int run_early(struct bwork *w)
{
    int n, i, ev, rc = 1;

    /* The main counter deliberately counts the single album-table walk.
     * Report the equally real priority work separately, otherwise a screen
     * can decode covers for minutes while appearing frozen at 0 / total. */
    ART_PACE();
    n = artalb_panels(w->tpath, &w->thdr, w->pset, ARTPANEL_SLOTS);
    if (n < 0)
        return 0;
    build_phase(ARTCACHE_BUILD_PHASE_PREPARE, n);
    ev = run_hints(w);
    if (ev)
        return ev;
    for (i = 0; i < n; i++)
    {
        ev = wait_ready(rc != 0);
        if (!ev)
            ev = run_hints(w);
        if (!ev)
            rc = album_early(w, &w->pset[i], "panelset", &ev);
        if (ev)
            return ev;
        build_advance();            /* checked; failures retry next pass */
    }
    if (w->n_delta && io_ok())
        commit(w, false);
    return run_panels(w);
}

/* ======================================================================= */
/* builder: thread                                                         */
/* ======================================================================= */
/* The database generation whose album walk a pass completed (every album
 * verified, the final commit published) before a later phase failed: the
 * retry resumes after the albums instead of walking them again. -1: none
 * (a request, USB or a completed pass clears it). */
static int resume_gen = -1;

/* After the album walk: the panels, the chapter art, the books. */
static int run_late(struct bwork *w)
{
    int ev;

    chapters_owed = false;          /* (again, if these defer) */
    chap_later = false;
    books_owed = false;
    ev = run_panels(w);
    if (!ev)
    {
        ev = run_chapters(w);
        if (!chap_later)        /* every book done: the window's job too */
            maint_done(MAINT_CHAPTERS, !ev);
    }
    if (!ev)
        ev = run_books(w);
    if (ev)
    {
        status.running = false;
        UPDATE_TRACE("art build stop ev=%d done=%d", ev, status.done);
    }
    return ev;
}

static int run_pass_impl(void)
{
    struct bwork *w = bw;
    const struct artalb_rec *t;
    struct artcache_rec rec;
    long t0 = current_tick;
    bool err;
    int ev, rc = 1;

    (void)t0;                       /* trace only */
    ev = wait_ready(true);
    if (ev)
        return ev;
#ifdef SIMULATOR
    sim_read_count2_begin();        /* "art build done ... reads=" (#90) */
#endif
    if (!pass_begin(w))
    {
        if (w->failed)
            return EV_RETRY;
        maintenance = false;
        return 0;
    }
#ifdef SIMULATOR
    sim_done_gen = -1;              /* an old success cannot mask a failed retry */
#endif
    status.total = w->thdr.n_albums;
    status.done = 0;
    build_phase(ARTCACHE_BUILD_PHASE_PREPARE, 0);
    status.running = true;
    UPDATE_TRACE("art build start total=%lu sweep=%d",
                 (unsigned long)w->thdr.n_albums, (int)w->sweep);
    /* ipodui.log (flushed at a spin-up the disk makes anyway): the fill
     * time and the wakes once done are P10 device checks (#58) */
    ui_log("art", "pass start total=%lu up=%lu play=%d paused=%d r=%d",
           (unsigned long)w->thdr.n_albums,
           (unsigned long)(current_tick / HZ), (int)playing(),
           (int)((audio_status() & AUDIO_STATUS_PAUSE) != 0),
           (int)(resume_gen == w->db_gen && !w->sweep));
    if (resume_gen == w->db_gen && !w->sweep)
    {
        /* a retry after a later phase failed: the albums were all
         * verified and published for this database by the last pass */
        status.done = status.total;
        w->gc_ok = true;
        err = false;
        ev = run_late(w);
        if (ev)
            return ev;
        goto finish;
    }
    ev = run_early(w);              /* #58: what the user sees first */
    if (!ev)
        build_phase(ARTCACHE_BUILD_PHASE_ALBUMS, status.total);

    for (;;)
    {
        long a0;
        unsigned long nb;

        if (!ev)
            ev = wait_ready(rc != 0);
        if (!ev)
            ev = run_hints(w);
        if (ev)
        {
            /* a request: keep what is done (if storage may be used now;
             * else it is redone), and restart */
            if (ev == EV_REQ && io_ok())
                commit(w, false);
            status.running = false;
            UPDATE_TRACE("art build stop ev=%d done=%d", ev, status.done);
            return ev;
        }
        if (w->db_gen != tagcache_ui_generation())
        {
            /* a new RAM cache: rep_idx means other tracks now. Keep what
             * is done (if storage may be used now; else it is redone);
             * library.c writes a new table and requests again */
            if (io_ok())
                commit(w, false);
            status.running = false;
            UPDATE_TRACE("art build stop %s done=%d", "db", status.done);
            return 0;
        }
        ART_PACE();
        if (w->n_delta >= DELTA_MAX && !commit(w, false))
        {
            status.running = false;
            return EV_RETRY;         /* never overrun the pending delta */
        }
        a0 = current_tick;
        nb = resize_noboost_skips();
        (void)nb; (void)a0;         /* trace only */
        t = next_album(w, &err);
        if (!t && !io_ok() && (ev = wait_ready(false)) != 0)
            continue;               /* the stop above */
        if (!t)
        {
            w->gc_ok = !err;
            if (err)
                FAIL(w);            /* the table walk */
            gc_owed = false;        /* (again, if this one defers it) */
            chapters_owed = false;
            books_owed = false;
            /* #90: unsigned records this pass could not reach (its cap
             * or no window): a walk at the next window */
            sig_owed = !err && w->sig_missing > w->sig_tried;
            if (w->sig_missing || w->sig_kept)
                UPDATE_TRACE("art sig pass missing=%d filled_or_tried=%d "
                             "kept=%d", w->sig_missing, w->sig_tried,
                             w->sig_kept);
            build_phase(ARTCACHE_BUILD_PHASE_FINALIZE, 0);
            ART_PACE();
            if (!commit(w, true))
            {
                status.running = false;
                return EV_RETRY;
            }
            if (!err)
            {
                if (!w->failed)
                    resume_gen = w->db_gen;
                ev = run_late(w);
                if (ev)
                    return ev;
            }
            break;
        }
        if (early_done(w, t->key))
        {
            /* built (or checked) before the walk: counted, no I/O */
            if (artidx_lookup(&w->rd, t->key, &rec, NULL))
                w->matched++;
            rc = 0;
        }
        else
        {
            /* the user came back while playing: the same album at the
             * next idle (#58) */
            while ((rc = album(w, t, &rec)) == ALBUM_LIT
                   && !(ev = wait_retry()))
                a0 = current_tick;
            if (ev)
                continue;           /* the stop above */
            if (w->failed && !w->fail_line)
            {
                FAIL(w);            /* album(): its key on the next line */
                ui_log("art", "fail k=%08lx%08lx", KEYARG(t->key));
            }
        }
        if (rc == 1)
        {
            w->delta[w->n_delta++] = rec;
            UPDATE_TRACE("art build album key=" KEYFMT " state=%d src=%d "
                         "ms=%ld boost=%d noboost=%lu", KEYARG(rec.key),
                         rec.state, rec.src_kind,
                         (current_tick - a0) * (1000 / HZ), (int)ART_BOOST(),
                         resize_noboost_skips() - nb);
        }
        ART_PACE();
        if ((w->n_delta >= DELTA_MAX
             || (w->n_delta && TIME_AFTER(current_tick,
                                          w->last_commit + COMMIT_TICKS)))
            && io_ok())
            commit(w, false);
        status.done++;
    }
finish:
    build_phase(ARTCACHE_BUILD_PHASE_FINALIZE, 0);
    status.running = false;
    if (w->failed || err)
    {
        if (!w->fail_line && !w->deferred)
            FAIL(w);                /* (early albums, hints) */
        UPDATE_TRACE("art retain pass %s", "incomplete");
        return EV_RETRY;
    }
#ifdef SIMULATOR
    sim_done_gen = w->db_gen;
    {
        unsigned long rb;
        int rn = sim_read_count2_end(&rb);

        UPDATE_TRACE("art build done albums=%d ms=%ld reads=%d "
                     "read_kb=%lu", status.done,
                     (current_tick - t0) * (1000 / HZ), rn, rb / 1024);
    }
#else
    UPDATE_TRACE("art build done albums=%d ms=%ld", status.done,
                 (current_tick - t0) * (1000 / HZ));
#endif
    ui_log("art", "pass done albums=%d s=%ld up=%lu play=%d", status.done,
           (current_tick - t0) / HZ, (unsigned long)(current_tick / HZ),
           (int)playing());
    maintenance = false;
    return 0;
}

static int run_pass(void)
{
    bw->context = NULL;
    int result = run_pass_impl();
    slice_report();
    if (bw->context)
        tagcache_context_release_passive(bw->context);
    bw->context = NULL;
    return result;
}

/* The backoff of a failed pass's retries; after the last one the builder
 * waits for a request (a library change), USB or the next boot, with the
 * progress page on "Artwork Update Pending" (WAIT_RETRY) meanwhile */
static const short retry_seconds[] = { 60, 300, 900 };
#define RETRY_CAPPED ((int)ARRAYLEN(retry_seconds) + 1)

static void art_thread(void)
{
    bool want = false;
    int retries = 0;
    long retry_at = 0;

#ifdef SIMULATOR
    sim_stack_base = (uintptr_t)__builtin_frame_address(0);
    sim_run_hook = slice_hook;
#endif
    resize_set_noboost_thread(thread_self(), true);
#ifdef SIMULATOR
    if (WIN_NO_POOL())
        remove(F_NPCOVER);          /* ipodui-np-sharp-*: from none */
#endif
    resize_set_pause_hook(art_gate);
    jpeg_set_pause_hook(art_gate);
    artfmt_pace = art_pace_fmt;     /* p19-art-yield */
    albumart_set_probe_hook(art_pace_probe);
    jpeg_set_abort_hook(npr_called_off);
    hang_watch_register(HANG_ART);  /* the hang watcher (bound 10 s) */
    for (;;)
    {
        int ev;
        bool fast;

        hang_beat(HANG_ART);

        if (stopped && TIME_AFTER(current_tick, stopped_tick + STOP_HOLD))
        {
            /* Back here nothing runs. Still alive this long after a
             * power-off: it was aborted (a busy database refuses it,
             * misc.c clean_shutdown()); build again, or nothing would
             * until a reboot (the #40 pattern of ui_commit_cancel) */
            stopped = false;
            want = true;
            UPDATE_TRACE("art build resume %s", "power-off aborted");
        }
        /* a pass that only put work off (DEFER) is no failure: its owed
         * work runs at the next idle even past the retry cap */
        if (!want && !stopped
            && (retries != RETRY_CAPPED || (bw->deferred && !bw->fail_line))
            && ((gc_owed && !playing())
                || (chap_later && maint_window_open()
                    && !maint_declined(MAINT_CHAPTERS))
                || ((regroup_owed || pool_owed) && regroup_window())
                || sig_due()
                || ((chapters_owed || books_owed) && !art_hold())))
        {
            if (sig_due())
                resume_gen = -1;    /* #90: the fill needs the walk */
            /* work kept for the stop (H1): a pass does it (the albums
             * are up to date: no source I/O) */
            want = true;
            /* npfix: why a pass follows (device log) */
            ui_log("art", "again owed gc=%d chl=%d rg=%d pool=%d sig=%d "
                   "ch=%d bk=%d", (int)gc_owed, (int)chap_later,
                   (int)regroup_owed, (int)pool_owed, (int)sig_owed,
                   (int)chapters_owed, (int)books_owed);
            UPDATE_TRACE("art build resume %s", "stopped");
        }
        /* #100: the panel sets' rotation, at a natural disk wake only
         * (the boot's, a refill's, a read's) or the sleep-entry window
         * (P18 Q8), never a wake of its own; while playing only with the
         * user idle. rot_ready: what the sleep window may owe (Q8) */
        rot_ready = !want && !stopped;
        idle_seen = false;
        if (!want && !stopped && (rot_due() || follow_owed)
            && rot_place()
            && !usb_owns_disk() && (!playing() || user_idle()))
            pool_rotate(bw);
        /* np-sharp: the Now Playing cover queued, storage already awake,
         * the user idle, Now Playing dark (npr_place()) */
        if (!want && !stopped && !pass_events && npr_key && npr_place()
            && !blocked())
            npr_run(bw);
        /* review H1: io_go()'s pump(0) in the rotation or the render took
         * a request or a USB session meant for the pass (wait_ready()
         * reads pass_events only inside one): it runs now */
        if (pass_events & (EV_REQ | EV_USB))
        {
            want = true;
            retry_at = 0;
            retries = 0;
            resume_gen = -1;
            bw->n_chskip = 0;
            UPDATE_TRACE("art build resume %s ev=%d", "idle-io",
                         pass_events);
        }
        pass_events = 0;
        /* re-review L-a: an ART_Q_IDLE drained by the rotation's or the
         * render's io_go() was a wake-up for this loop (the posts are
         * edge-triggered): look again once before blocking */
        if (idle_seen && !want && !stopped)
        {
            idle_seen = false;
            continue;
        }
        if (!want || stopped || (retry_at
            && TIME_BEFORE(current_tick, retry_at)))
        {
#ifdef SIMULATOR
            sim_blocked = true;
#endif
            /* done (#58: no poll, no wake until a request, USB or a
             * power-off), or work kept for the stop: woken by
             * idle_event() */
            ev = pump(stopped ? POLL_TICKS : retry_at
                      ? MAX(1, retry_at - current_tick) : TIMEOUT_BLOCK);
#ifdef SIMULATOR
            sim_blocked = false;
#endif
            if (!stopped)
            {
                UPDATE_TRACE("art wake ev=%d", ev);
                ui_log("art", "wake ev=%d up=%lu", ev,
                       (unsigned long)(current_tick / HZ));
            }
            if (ev & (EV_REQ | EV_USB))
            {
                want = true;        /* after a stop: once it is cleared */
                retry_at = 0;
                retries = 0;
                resume_gen = -1;    /* the files may have changed */
                bw->n_chskip = 0;
            }
            else if (retry_at && !TIME_BEFORE(current_tick, retry_at))
            {
                retry_at = 0;
                want = true;
            }
            continue;
        }
        want = false;
        want_build = false;
        fast = maintenance;
        build_fast = fast;
        if (fast)
        {
            fast_boost(true);
            resize_set_noboost_thread(thread_self(), false);
        }
        ev = run_pass();
        if (fast)
            resize_set_noboost_thread(thread_self(), true);
        fast_boost(false);          /* (a no-op unless a fast pass) */
        /* after USB the post-USB rescan requests again; a request during
         * a pass restarts it */
        if (ev != EV_RETRY)
            resume_gen = -1;        /* a full pass next time */
        if (ev & (EV_REQ | EV_USB))
        {
            want = true;
            retries = 0;
            bw->n_chskip = 0;
            ui_log("art", "again ev=%d", ev);   /* npfix: a request or USB
                                                   during the pass */
        }
        else if ((ev & EV_RETRY) && bw->deferred && !bw->fail_line)
        {
            /* only put off for playback: the owed trigger above runs it
             * at the next idle; no retry timer, nothing counted */
            status.wait = ARTCACHE_BUILD_WAIT_PLAYBACK;
            ui_log("art", "deferred%s", "");
        }
        else if (ev & EV_RETRY)
        {
            status.wait = ARTCACHE_BUILD_WAIT_RETRY;
            if (retries < (int)ARRAYLEN(retry_seconds))
            {
                ui_log("art", "retry n=%d in=%ds resume=%d", retries + 1,
                       retry_seconds[retries], (int)(resume_gen >= 0));
                retry_at = current_tick + retry_seconds[retries++] * HZ;
                want = true;
            }
            else
            {
                /* no more: not even for the owed work (that restarted it
                 * at once); a request, USB or a boot tries again */
                retries = RETRY_CAPPED;
                ui_log("art", "retry cap fail l=%d", bw->fail_line);
            }
            UPDATE_TRACE("art retry attempt=%d pending=%d", retries,
                         (int)maintenance);
        }
        else
        {
            retries = 0;
            if (want_build)
                want = true;
        }
    }
}

#ifdef SIMULATOR
/* ======================================================================= */
/* simulator script hooks (uisimulator/common/sim_cmd_art.c)               */
/* ======================================================================= */
/* art_wait: 1 once the builder has finished a pass over the current
 * database and waits for the next request, 0 on timeout, -1 without a
 * builder. */
int sim_art_builder_idle(long timeout_ms)
{
    long end = current_tick + timeout_ms * HZ / 1000;

    if (!art_thread_id)
        return -1;
    do
    {
        if (sim_blocked && !stopped && !art_q_waiting()
            && sim_done_gen == tagcache_ui_generation()
            && !(gc_owed && !playing())
            && !(chap_later && maint_window_open()
                    && !maint_declined(MAINT_CHAPTERS))
            && !((regroup_owed || pool_owed) && regroup_window())
            && !sig_due()
            && !((chapters_owed || books_owed) && !art_hold()))
            return 1;
        sleep(HZ / 20);
    } while (TIME_BEFORE(current_tick, end));
    return 0;
}

/* art_panels_ungroup (#92): panels.pak as an 8b unit may have it, sets
 * interleaved: the first music cover and the first book cover trade
 * slots (pixels and header, in place); the next maintenance window
 * regroups it. expect_panels_grouped: the version when grouped. */
/* #100 (tests/ui): the rotation's clock moved on; the shown set's random
 * picks remembered, and compared */
static uint64_t sim_rand_mark[ARTPOOL_RAND];
static int sim_rand_n = -1;
static uint32_t sim_rot_mark;

static bool sim_pool_read(struct artpool_idx *x)
{
    int fd = open(F_POOLX, O_RDONLY);
    bool ok = fd >= 0 && read(fd, x, sizeof(*x)) == (ssize_t)sizeof(*x)
              && artpool_ok(&x->pool);

    if (fd >= 0)
        close(fd);
    return ok;
}

/* expect_panels_valid (#100 rollback): panels.pak as an 8b build reads it
 * (version 1, its header sound, every slot it names present): its OK
 * covers, or -1 */
int sim_art_panels_valid(void)
{
    struct artpanel_hdr h;
    int k, n = 0;

    if (!panel_header(F_PANELS, &h) || h.version != ARTPANEL_VERSION)
        return -1;
    for (k = 0; k < ARTPANEL_SLOTS; k++)
        if (h.ent[k].state == ARTREC_OK)
            n++;
    return n;
}

void sim_art_rotate_warp(long seconds)
{
    sim_rot_warp += seconds;
}

/* the shown set: count, random picks, book random picks; mark: remember */
int sim_art_pool_shown(bool mark, int *nrand, int *brand, int *overlap,
                       uint32_t *rotations, int *bcand)
{
    static struct artpool_idx x;    /* sim only */
    int k, j, n = 0;

    if (!sim_pool_read(&x))
        return -1;
    *nrand = x.pool.n_rand;
    *brand = 0;
    *bcand = 0;
    for (k = 0; k < ARTPOOL_ENTS; k++)      /* books a random pick may be */
    {
        const struct artpanel_ent *e = &x.pool.ent[k];

        if (e->state == ARTREC_OK
            && ARTPANEL_ENT_SET(e) == ARTCACHE_PSET_BOOKS)
        {
            for (j = 0; j < x.pool.n_pinned && x.pool.pinned[j] != e->key;
                 j++)
                ;
            if (j == x.pool.n_pinned)
                (*bcand)++;
        }
    }
    *overlap = 0;
    for (k = 0; k < ARTPANEL_SLOTS; k++)
        if (x.shown.h.ent[k].state == ARTREC_OK)
        {
            n++;
            if (ARTPANEL_ENT_SET(&x.shown.h.ent[k]) == ARTCACHE_PSET_BOOKS)
                for (j = 0; j < x.pool.n_rand; j++)
                    if (x.pool.rand[j] == x.shown.h.ent[k].key)
                        (*brand)++;
        }
    for (k = 0; k < x.pool.n_rand; k++)
        for (j = 0; j < sim_rand_n; j++)
            if (x.pool.rand[k] == sim_rand_mark[j])
                (*overlap)++;
    *rotations = x.pool.rot_count - sim_rot_mark;
    if (mark)
    {
        memcpy(sim_rand_mark, x.pool.rand, sizeof(sim_rand_mark));
        sim_rand_n = x.pool.n_rand;
        sim_rot_mark = x.pool.rot_count;
    }
    return n;
}

void sim_art_panels_ungroup(void)
{
    struct artpanel_hdr h;
    int fd = open(F_PANELS, O_RDWR), k, a = -1, b = -1;
    bool ok = false;

    if (fd >= 0)
    {
        if (read(fd, &h, sizeof(h)) == (ssize_t)sizeof(h)
            && artpanel_hdr_ok(&h, panel.w, panel.h))
        {
            for (k = 0; k < ARTPANEL_SLOTS; k++)
                if (h.ent[k].state == ARTREC_OK)
                {
                    if (a < 0 && ARTPANEL_ENT_SET(&h.ent[k]) == 0)
                        a = k;
                    else if (b < 0 && ARTPANEL_ENT_SET(&h.ent[k]) == 1)
                        b = k;
                }
        }
        if (a >= 0 && b >= 0)
        {
            size_t n = panel_slot_bytes;
            char *pa = malloc(n), *pb = malloc(n);
            off_t oa = artpanel_slot_offset(n, h.ent[a].slot);
            off_t ob = artpanel_slot_offset(n, h.ent[b].slot);
            uint8_t t = h.ent[a].slot;

            ok = pa && pb && lseek(fd, oa, SEEK_SET) == oa
                 && read(fd, pa, n) == (ssize_t)n
                 && lseek(fd, ob, SEEK_SET) == ob
                 && read(fd, pb, n) == (ssize_t)n
                 && lseek(fd, oa, SEEK_SET) == oa
                 && write(fd, pb, n) == (ssize_t)n
                 && lseek(fd, ob, SEEK_SET) == ob
                 && write(fd, pa, n) == (ssize_t)n;
            h.ent[a].slot = h.ent[b].slot;
            h.ent[b].slot = t;
            artpanel_hdr_seal(&h);
            ok = ok && lseek(fd, 0, SEEK_SET) == 0
                 && write(fd, &h, sizeof(h)) == (ssize_t)sizeof(h);
            free(pa);
            free(pb);
        }
        close(fd);
    }
    panel_seq++;
    regroup_owed = ok && !artpanel_grouped(&h);
    fprintf(stderr, "sim_script: art_panels_ungroup %s\n",
            regroup_owed ? "ok" : "failed");
}

int sim_art_panels_grouped(void)
{
    struct artpanel_hdr h;

    if (!panel_header(F_PANELS, &h))
        return -1;
    return artpanel_grouped(&h) ? (int)h.version : 0;
}

/* art_panel_hold: the next panel block is held 'ms' longer */
void sim_art_panel_hold(long ms)
{
    sim_panel_hold = ms;
}

/* npfix review M1: n raw posts of the builder's own events (a parked
 * builder must take them: QUEUE_LENGTH is 32), then n through
 * art_post() (one of each lands) */
void sim_art_post_flood(long n)
{
    long i;

    if (!art_thread_id)
        return;
    for (i = 0; i < n; i++)
    {
        queue_post(&art_queue, ART_Q_NP, 0);
        queue_post(&art_queue, ART_Q_IDLE, 0);
        if ((i & 3) == 3)
            sleep(1);               /* a fill's or a poll's pace */
    }
    for (i = 0; i < n; i++)
    {
        art_post(ART_Q_IDLE);
        art_post(ART_Q_BUILD);
        art_post(ART_Q_NP);
    }
    UPDATE_TRACE("art flood n=%ld queued=%d", n, queue_count(&art_queue));
}

/* art_npf_hold <0|1> (review M4): Now Playing's bundle read waits */
void sim_art_npf_hold(bool hold)
{
    sim_npf_hold = hold;
    if (!hold && art_thread_id && npf_req)
        npf_post();
}

/* expect_art_parked: 1 while the builder is parked (art_gate()) */
int sim_art_parked(void)
{
    return art_thread_id ? (int)sim_parked : -1;
}

/* art_decode_spin <us> (npfix): a busy wait at each decoder yield point */
void sim_art_decode_spin(long us)
{
    sim_gate_spin = us;
}

/* art_request: what a database load or precompute does (any thread) */
void sim_art_request(void)
{
    artcache_build_request();
}

/* Start an explicit cold-cache pass with a requested cover. Called only
 * while the builder is idle; the script removes its index before this. */
void sim_art_progress_request(void)
{
    struct artalb_rec rec;

    sim_progress_seen = sim_progress_advanced = 0;
    sim_progress_bad = false;
    if (artalb_read(bw->tpath, &bw->thdr, 0, 1, &rec) == 1 && rec.key)
        hint_add(rec.key);
    artcache_build_maintenance_request();
}

int sim_art_progress_check(void)
{
    unsigned phases = (1u << ARTCACHE_BUILD_PHASE_FINALIZE) - 1u;
    unsigned counters = (1u << ARTCACHE_BUILD_PHASE_PREPARE)
                      | (1u << ARTCACHE_BUILD_PHASE_REQUESTED)
                      | (1u << ARTCACHE_BUILD_PHASE_PANELS);

    phases |= 1u << ARTCACHE_BUILD_PHASE_FINALIZE;
    return !sim_progress_bad && (sim_progress_seen & phases) == phases
        && (sim_progress_advanced & counters) == counters
        && status.done == status.total && !status.running;
}

/* Persistence faults live only on a cloned simulator disk. Each probe owns
 * its reader/buffer; it never borrows the UI or builder's scratch. */
struct sim_persist_item {
    struct artcache_rec rec;
    uint32_t thumb_sum, bundle_sum;
};
static struct {
    struct sim_persist_item *item;
    uint32_t count, pack_sum;
    artcache_key_t damaged;
} sim_persist;

static bool sim_persist_sum(const char *path, off_t offset, size_t bytes,
                            void *buf, uint32_t *sum)
{
    int fd = open(path, O_RDONLY);
    if (fd < 0)
        return false;
    off_t end = lseek(fd, 0, SEEK_END);
    bool ok = end >= offset && lseek(fd, offset, SEEK_SET) == offset;
    if (bytes == 0 && ok)
        bytes = end - offset;
    if (ok && (uint64_t)offset + bytes > (uint64_t)end)
        ok = false;
    uint32_t crc = ARTFMT_CRC_INIT;
    while (ok && bytes)
    {
        size_t n = MIN(bytes, 16384);
        ok = read(fd, buf, n) == (ssize_t)n;
        if (ok) crc = artfmt_crc(crc, buf, n);
        bytes -= n;
    }
    if (close(fd) < 0) ok = false;
    if (ok) *sum = artfmt_crc_end(crc);
    return ok;
}

static bool sim_persist_item_sum(const struct artcache_rec *rec, void *buf,
                                 uint32_t *tsum, uint32_t *bsum)
{
    *tsum = *bsum = 0;
    if (rec->state != ARTREC_OK)
        return true;
    if (!artrec_is_chapter(rec)
        && !sim_persist_sum(F_PAK, artpak_slot_offset(slot_bytes, rec->slot),
                            slot_bytes, buf, tsum))
        return false;
    if (artrec_owns_bundle(rec))
    {
        char path[ART_BPATH];
        bundle_path(rec->key, path);
        if (!sim_persist_sum(path, 0, 0, buf, bsum))
            return false;
    }
    return true;
}

int sim_art_persistence(const char *op)
{
    struct artidx_reader *rd = calloc(1, sizeof(*rd));
    struct rblock *probe = calloc(1, sizeof(*probe));
    void *buf = malloc(16384);
    bool ok = rd && probe && buf && sim_art_builder_idle(0) == 1;
    if (!ok) { free(rd); free(probe); free(buf); return 0; }
    artidx_init(rd);
    artidx_init(&probe->rd);
    probe->pak_fd = -1;
    if (!strcmp(op, "damage_index"))
    {
        int src = open(F_INDEX, O_RDONLY);
        int dst = open(F_INDEX_PREV, O_WRONLY | O_CREAT | O_TRUNC, 0666);
        ssize_t n = 0;
        ok = src >= 0 && dst >= 0;
        while (ok && (n = read(src, buf, 16384)) > 0)
            ok = write(dst, buf, n) == n;
        ok = ok && n == 0 && fsync(dst) == 0;
        if (src >= 0 && close(src) < 0) ok = false;
        if (dst >= 0 && close(dst) < 0) ok = false;
        dst = ok ? open(F_INDEX, O_WRONLY) : -1;
        uint32_t bad = 0;
        ok = ok && dst >= 0 && write(dst, &bad, sizeof(bad)) == sizeof(bad)
             && fsync(dst) == 0;
        if (dst >= 0 && close(dst) < 0) ok = false;
    }
    else if (!strcmp(op, "check_fallback"))
    {
        struct artcache_rec rec;
        artcache_key_t key = 0;
        for (uint32_t i = 0; sim_persist.item && i < sim_persist.count; i++)
        {
            if (sim_persist.item[i].rec.state == ARTREC_OK
                && !artrec_is_chapter(&sim_persist.item[i].rec))
            { key = sim_persist.item[i].rec.key; break; }
        }
        ok = key && lookup(probe, key, &rec) == ARTCACHE_OK
             && read_thumb(probe, &rec, key, buf) == ARTCACHE_OK;
    }
    else
    {
        ok = artidx_open(rd, F_INDEX) == ARTIDX_OPENED;
        if (ok && !strcmp(op, "mark"))
        {
            free(sim_persist.item);
            sim_persist.count = rd->hdr.count;
            sim_persist.damaged = 0;
            sim_persist.item = calloc(rd->hdr.count, sizeof(*sim_persist.item));
            ok = sim_persist.item && rd->hdr.count >= 2
                 && sim_persist_sum(F_PAK, 0, 0, buf, &sim_persist.pack_sum);
            for (uint32_t i = 0; ok && i < rd->hdr.count; i++)
            {
                struct sim_persist_item *s = &sim_persist.item[i];
                ok = artidx_get(rd, i, &s->rec)
                     && sim_persist_item_sum(&s->rec, buf,
                                             &s->thumb_sum, &s->bundle_sum);
            }
        }
        else if (ok && !strcmp(op, "damage_thumb"))
        {
            struct artcache_rec rec = { 0 };
            ok = sim_persist.item != NULL;
            bool found = false;
            for (uint32_t i = 0; ok && i < rd->hdr.count && !found; i++)
            {
                ok = artidx_get(rd, i, &rec);
                found = ok && rec.state == ARTREC_OK && !artrec_is_chapter(&rec);
            }
            ok = ok && found;
            int fd = ok ? open(F_PAK, O_RDWR) : -1;
            off_t off = artpak_slot_offset(slot_bytes, rec.slot)
                        + (size_t)thumb.w * thumb.h * 2;
            struct artpak_tag tag = { 0 };
            ok = ok && fd >= 0 && lseek(fd, off, SEEK_SET) == off
                 && read(fd, &tag, sizeof(tag)) == sizeof(tag);
            tag.key ^= 1;
            ok = ok && lseek(fd, off, SEEK_SET) == off
                 && write(fd, &tag, sizeof(tag)) == sizeof(tag) && fsync(fd) == 0;
            probe->pak_fd = fd;
            if (ok)
            {
                sim_persist.damaged = rec.key;
                ok = read_thumb(probe, &rec, rec.key, buf) == ARTCACHE_PENDING;
            }
        }
        else if (ok && (!strcmp(op, "check") || !strcmp(op, "check_repair")))
        {
            bool repair = !strcmp(op, "check_repair");
            uint32_t pack_sum;
            ok = sim_persist.item && rd->hdr.count == sim_persist.count
                 && (repair || (sim_persist_sum(F_PAK, 0, 0, buf, &pack_sum)
                                && pack_sum == sim_persist.pack_sum));
            for (uint32_t i = 0; ok && i < sim_persist.count; i++)
            {
                struct artcache_rec rec;
                uint32_t tsum, bsum;
                struct sim_persist_item *s = &sim_persist.item[i];
                ok = artidx_lookup(rd, s->rec.key, &rec, NULL);
                if (repair && s->rec.key == sim_persist.damaged)
                {
                    probe->pak_fd = open(F_PAK, O_RDONLY);
                    ok = ok && probe->pak_fd >= 0 && rec.state == ARTREC_OK
                         && read_thumb(probe, &rec, rec.key, buf) == ARTCACHE_OK;
                    reader_close(probe);
                    continue;
                }
                ok = ok && !memcmp(&rec, &s->rec, sizeof(rec))
                     && sim_persist_item_sum(&rec, buf, &tsum, &bsum)
                     && tsum == s->thumb_sum && bsum == s->bundle_sum;
            }
        }
        else ok = false;
    }
    artidx_close(rd);
    reader_close(probe);
    free(rd);
    free(probe);
    free(buf);
    return ok;
}

/* art_poweroff_abort: a power-off only the builder hears (then aborted) */
void sim_art_poweroff_abort(void)
{
    if (art_thread_id)
        queue_post(&art_queue, SYS_POWEROFF, 0);
}

/* expect_art: calls fn(ctx, path, state) for every album of the table
 * (path: its representative track; state: "OK", "NOART", "FAILED" or
 * "PENDING", read from index.dat with a reader of its own). Returns the
 * number of albums, < 0 if there is no current table. Script thread. */
int sim_art_report(void (*fn)(void *ctx, const char *path,
                              const char *state), void *ctx)
{
    static const char *const names[] = {
        "PENDING", "OK", "NOART", "FAILED"
    };
    struct artalb_stamp want = {
        tagcache_ui_commitid(), tagcache_ui_count(), SORTKEY_VERSION
    };
    struct {
        struct artidx_reader rd;
        struct artalb_hdr th;
        struct artalb_rec rec[TCHUNK];
        struct tagcache_search tcs;
        char tpath[MAX_PATH], path[MAX_PATH];
        uint32_t page[ARTIDX_PAGE_BYTES / 4];
    } *s = malloc(sizeof(*s));
    uint32_t first = 0;
    int n = -1, got, i;

    if (!s)
        return -1;
    artidx_init(&s->rd);
    snprintf(s->tpath, sizeof(s->tpath), "%s/%s", tagcache_ui_db_path(),
             LIBFILE_NAME);
    if (artalb_check(s->tpath, &want, &s->th, s->page) != ARTIDX_OPENED)
        goto out;
    artidx_open(&s->rd, F_INDEX);
    n = 0;
    while ((got = artalb_read(s->tpath, &s->th, first, TCHUNK, s->rec)) > 0)
    {
        for (i = 0; i < got; i++)
        {
            struct artcache_rec r;
            int st = 0;

            if (!s->rec[i].key)
                continue;
            if (s->rd.fd >= 0 && artidx_lookup(&s->rd, s->rec[i].key, &r, NULL)
                && r.state <= ARTREC_FAILED)
                st = r.state;
            s->path[0] = '\0';
            if (tagcache_search(&s->tcs, tag_filename))
            {
                tagcache_retrieve(&s->tcs, s->rec[i].rep_idx, tag_filename,
                                  s->path, sizeof(s->path));
                tagcache_search_finish(&s->tcs);
            }
            fn(ctx, s->path, names[st]);
            n++;
        }
        first += got;
    }
out:
    artidx_close(&s->rd);
    free(s);
    return n;
}
#endif /* SIMULATOR */

/* ======================================================================= */
/* reserve                                                                 */
/* ======================================================================= */
bool artcache_reserve(void)
{
    size_t stack_size = ART_STACK_SIZE;
    size_t work_size, read_size, npf_size;
    struct rblock *r;
    uint8_t *blk;

    if (stack_h > 0)
        return true;
    thumb = artcache_sizes[ARTCACHE_THUMB];
    slot_bytes = artpak_slot_bytes(thumb.w, thumb.h);
    /* JPEG state + the fill-size bitmap + resize line buffers + slack,
     * for the thumbnail (fill, up to FILL_MAX x the box: 78,284 B on the
     * target) or the Now Playing fit (at most the 142 px box, D10: 47,348
     * + 40,328 + 6,816 + 4,096 = 98,588 B), whichever is larger */
    {
        struct artcache_dim np = artcache_sizes[ARTCACHE_NOWPLAYING];
        size_t np_bytes = JPEG_DECODE_OVERHEAD
                        + (size_t)np.w * np.h * sizeof(fb_data)
                        + 3 * sizeof(struct uint32_argb) * FILL_MAX * np.w
                        + 4096;

        decode_bytes = JPEG_DECODE_OVERHEAD
                     + (size_t)FILL_MAX * thumb.w * thumb.h * sizeof(fb_data)
                     + 3 * sizeof(struct uint32_argb) * FILL_MAX * thumb.w
                     + 4096;
        if (decode_bytes < np_bytes)
            decode_bytes = np_bytes;
    }
    work_size = ALIGN_UP(sizeof(struct bwork) + slot_bytes, 8);
    read_size = ALIGN_UP(sizeof(struct rblock), 8);
    npf_size = ALIGN_UP(ARTCACHE_NP_ONE_READ, 8);   /* 51,752 B */
    /* a panel: streamed through bands of the decode block (#58) */
    panel = artcache_sizes[ARTCACHE_PANEL];
    panel_slot_bytes = artpak_slot_bytes(panel.w, panel.h);

    /* chapter art: the packed chapter table (AB_CH_TABLE_SIZE = 32,784 B,
     * chapters.h), then the art table (AB_MAX_CHAPTERS * 8 = 8,192 B),
     * transient, per book, only while stopped. The records (32 B each,
     * <= AB_MAX_CHAPTERS + 1 + DELTA_MAX) reuse the chapter table, so the
     * first part is the larger of the two (P6 contracts; B resizes it for
     * books.pak, Rev 1 item 4). */
    chap_tab_off = ALIGN_UP(MAX(AB_CH_TABLE_SIZE,
                                (AB_MAX_CHAPTERS + 1 + DELTA_MAX)
                                * sizeof(struct artcache_rec)), 8);
    chap_bytes = chap_tab_off
               + AB_MAX_CHAPTERS * sizeof(struct ab_chapter_art);
    /* books.pak (P6, fix round L1): the blob (AB_BLOB_MAX_SIZE = 43,048
     * B; first, the per-file parse table, 32,784 B, with the file map,
     * 10 B x AB_MAX_FILES = 10,240 B, behind it) and the book's table
     * (32,784 B), + 8 to align: 75,840 B, inside the decode block (never
     * a block of its own; the rest of it is the chapter read window) */
    if (decode_bytes < BOOKS_BYTES + 8 + 1024)
        decode_bytes = BOOKS_BYTES + 8 + 1024;

    dec_h = core_alloc(decode_bytes);
    /* the work block, the UI's read block and the stack: pinned together,
     * for good, each 8-byte aligned (both blocks hold structs with 64-bit
     * members; buflib aligns to 4 bytes only on the target, so the base
     * is rounded up within 7 bytes of slack: H1) */
    stack_h = core_alloc(7 + work_size + read_size + npf_size + stack_size);
    if (dec_h <= 0 || stack_h <= 0)
    {
        UPDATE_TRACE("art alloc failed what=%s bytes=%lu", "blocks",
                     (unsigned long)(decode_bytes + 7 + work_size + read_size
                                     + npf_size + stack_size));
        if (dec_h > 0) dec_h = core_free(dec_h);
        if (stack_h > 0) stack_h = core_free(stack_h);
        dec_h = stack_h = 0;
        return false;
    }
    core_pin(stack_h);              /* a running thread's stack */
    blk = (uint8_t *)ALIGN_UP((uintptr_t)core_get_data(stack_h), 8);
    bw = (struct bwork *)blk;
    r = (struct rblock *)(blk + work_size);
    CHECK_ALIGN(bw);
    CHECK_ALIGN(r);
    memset(r, 0, sizeof(*r));
    artidx_init(&r->rd);
    r->pak_fd = -1;
    r->seq = -1;
    r->tried = false;
    r->pseq = -1;
    r->ph_ok = false;
    rblk = r;

    memset(bw, 0, sizeof(*bw));
    artidx_init(&bw->rd);
    bw->dec = dec_h;
    bw->dec_bytes = decode_bytes;
    bw->tw = thumb.w;
    bw->th = thumb.h;
    add_event(SYS_EVENT_USB_INSERTED, usb_event);
    add_event(BUFFER_EVENT_FILL_DONE, fill_event);  /* npf (the sim: and
                                                       its disk model) */
    add_event(PLAYBACK_EVENT_STOP_OR_PAUSE, idle_event);
    add_event(PLAYBACK_EVENT_TRACK_CHANGE, npr_track_event);
    queue_init(&art_queue, true);   /* receives USB / power-off broadcasts */
    npf_buf = (char *)(blk + work_size + read_size);
    art_thread_id = create_thread(art_thread,
                                  blk + work_size + read_size + npf_size,
                                  stack_size, 0,
                                  "artcache" IF_PRIO(, PRIORITY_BACKGROUND)
                                  IF_COP(, CPU));
    if (want_build && art_thread_id)
        art_post(ART_Q_BUILD);
    logf("artcache: reserved read %u decode %u work %u stack %u; "
         "chapter transient %u",
         (unsigned)read_size, (unsigned)decode_bytes,
         (unsigned)work_size, (unsigned)stack_size, (unsigned)chap_bytes);
    return art_thread_id != 0;
}

/* #38 F9: pollers (the album list's art fill) key on this, not on build
 * progress. */
int artcache_commit_seq(void)
{
    return commit_seq;
}
