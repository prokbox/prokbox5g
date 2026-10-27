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
 * $Id$
 *
 * Scripted UI input for the simulator (--script FILE)
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

/*
 * Script format, one command per line ('#' starts a comment):
 *
 *   wait <ms>            sleep
 *   key <NAME> [ms]      press and release a key (held 'ms', default 100)
 *   hold <NAME> <ms>     long press
 *   wheel <+n|-n> [ms]   n scroll wheel clicks (+ = forward/down), one
 *                        every 'ms' (default 40, i.e. 25 clicks/s; min 10),
 *                        posted with the clickwheel driver's speed data
 *                        (bit 31), computed from 'ms', not host timing
 *   chord <A>+<B>[+..] <ms>
 *                        press keys together (each one 50 ms after the
 *                        previous, like real fingers), hold them all for
 *                        'ms', then release in reverse order
 *   shot <name>          dump the LCD to <shotdir>/<name>.bmp
 *   wait_db [ms]         block until the database (tagcache) is usable:
 *                        tagcache_is_usable() and no commit running
 *                        (default timeout 30000 ms). Logs "wait_db ready
 *                        after N ms", or "wait_db timed out" / "wait_db
 *                        commit delayed" (scanned, but the commit needs a
 *                        reboot), which scripts/shots.sh turns into a
 *                        failure. State comes from sim_db.c.
 *   wait_commit [ms]     block until a commit has run and ended since the
 *                        last wait_commit (or the boot): a latch on the
 *                        commit counter, so one that starts and ends
 *                        between polls, or before the command, is never
 *                        missed; the next command lands right after it,
 *                        before the candidate publishes when it is still
 *                        running. Logs "wait_commit done after N ms
 *                        commits=K" or "wait_commit timed out".
 *   build_db             start a full database build when there is none,
 *                        like the stock "Initialize Now" without its
 *                        prompt; waits up to 10 s for the boot-time check
 *                        first, and does nothing when a database is usable
 *                        or a build is already running
 *   update_db            start a database update (tagcache_update(): scan
 *                        for new, changed and deleted files, commit, reload
 *                        the RAM cache) and return at once
 *   update_db_wait [ms]  the same, then block until the reloaded database
 *                        is usable (default timeout 30000 ms). Logs
 *                        "update_db_wait ready after N ms" or
 *                        "update_db_wait timed out" (a failure for
 *                        scripts/shots.sh). An update that changes nothing
 *                        returns after 3 s ("(no change)"): for no-op
 *                        updates only
 *   update_db_wait_change [ms]
 *                        the same, for an update that must change the
 *                        database (add or drop files): no 3 s exit; logs
 *                        "update_db_wait_change ready after N ms" or
 *                        "update_db_wait_change failed" (no change within
 *                        the timeout: a failure for scripts/shots.sh)
 *   rebuild_db [ms]      rebuild the database from scratch (the stock
 *                        "Initialize Now", no prompt: the shell exports the
 *                        statistics first and imports them into the new
 *                        database) and wait (default 30000 ms) until it is
 *                        usable again; logs "rebuild_db ready after N ms"
 *                        or "rebuild_db timed out" (a failure)
 *   rebuild_db_start     start that rebuild and return at once (e.g.
 *                        while music plays: the commit waits for it to
 *                        stop)
 *   scan_slow <ticks>    every later scan sleeps <ticks> at each
 *                        directory it walks (0: off), so a test library's
 *                        update lasts seconds (the busy glyph, 3.20)
 *   dirty_db             mark the database on disk dirty, as a power cut
 *                        in the middle of a commit leaves it; logs
 *                        "dirty_db ok" or "dirty_db failed" (a failure).
 *                        The running boot keeps its RAM copy: follow it
 *                        with "reboot"
 *   expect_lib_updating <ms>
 *                        the shell's Music menu shows "Updating
 *                        Library..." (lib_state() == LIB_UPDATING) all
 *                        through 'ms': "expect_lib_updating ok", else
 *                        "expect_lib_updating failed"
 *   expect_lib_browsable <ms>
 *                        with library generations a rebuild or update
 *                        builds a candidate beside the active library:
 *                        the Music menu never says "Updating Library..."
 *                        and the active library stays usable all through
 *                        'ms': "expect_lib_browsable ok", else "...failed"
 *   db_gen_mark          remember the active library generation
 *   expect_db_gen_same <ms>  it stays that generation all through 'ms'
 *                        (an update waiting for playback to stop):
 *                        "expect_db_gen_same ok" / "... failed"
 *   expect_db_gen_change [ms]  wait (default 30000 ms) until another
 *                        generation is active and usable (the update
 *                        published): "expect_db_gen_change ok after N ms"
 *                        / "expect_db_gen_change failed"
 *   db_mark              remember the database's entry count
 *   expect_db_grew [ms]  wait (default 30000 ms) until the database is
 *                        usable with more entries than at db_mark; logs
 *                        "expect_db_grew ok" or "expect_db_grew failed"
 *   expect_db_same <ms>  watch the entry count for 'ms': "expect_db_same
 *                        ok" if it stayed at the db_mark count all along,
 *                        else "expect_db_same failed"
 *   expect_resume <path> [min_ms] [wait_ms]
 *                        the database's saved resume position
 *                        (lastelapsed) of <path> (%20 = space) is at least
 *                        min_ms (default 1); polled up to wait_ms (default
 *                        5000; an import may still be running). Logs
 *                        "expect_resume ok" or "expect_resume failed"
 *   import_changelog     run the changelog import on the script thread
 *                        and log "import_changelog N ms" (timing)
 *   reboot               end this boot here: scripts/shots.sh runs the
 *                        rest of the script in a new boot of the sim, on
 *                        the same disk (run directly, the sim just quits)
 *   expect_playing <ms>  watch audio_status() every 10 ms for 'ms': logs
 *                        "expect_playing ok" if it was "playing" all along,
 *                        else "expect_playing failed" (a stop, even one the
 *                        audio thread resumes from, counts)
 *   expect_paused <ms>   the same for "paused" (play + pause bits):
 *                        "expect_paused ok" / "expect_paused failed"
 *   copy_file <src> <dst> copy a file inside the sim disk (fresh mtime);
 *                        write a space in a path as %20
 *   copy <src> <dst>     copy host file <src> (relative to the script's
 *                        directory) to sim path <dst> (in simdisk; the
 *                        directory must exist), e.g. a track to add
 *   rm_file <path>       delete a file from the sim disk (%20 = space); a
 *                        /.rockbox/database_* file not at that path is
 *                        removed from the active library generation's
 *                        directory (where the shell keeps it)
 *   remove <path>        alias of rm_file
 *   quit                 shut the simulator down (also done at EOF)
 *   usb_connect          simulate SYS_USB_CONNECTED (sim_trigger_usb(),
 *                        the same path the SDL window's USB key toggle
 *                        uses). See uisimulator/common/sim_cmd_art.c
 *                        (docs/phase4b-brief.md) for this and the three
 *                        commands below
 *   usb_disconnect       simulate SYS_USB_DISCONNECTED
 *   art_wait [ms]        block until the art cache builder is idle
 *                        (default 10000 ms); logs "art_wait unavailable"
 *                        until the builder thread exists in this build
 *   wait_pos <ms> [timeout [name]]  block until the current track (whose
 *                        path contains <name>, if given) position
 *                        reaches <ms>; wait_index <n> [timeout] until the
 *                        queue's display index is <n> (sim_cmd_np.c):
 *                        playback progress, not wall clock. "... failed"
 *                        on timeout (default 60000 ms) fails the script
 *   ab_power_cut, ab_touch, ab_mark, ab_mem_mark, expect_ab_*, ab_pak_stat
 *                        Phase 6 books (sim_cmd_ab.c, tests/ui/README.md)
 *   art_request          ask the art builder for a pass (sim_cmd_art.c)
 *   disk_active <ms>     storage counts as awake for 'ms' (as during an
 *                        audio refill; sim_cmd_art.c)
 *   expect_art <manifest> every album's art cache state matches the
 *                        manifest (tests/ui/art_manifest.txt), none
 *                        pending: "expect_art ok" / "expect_art failed"
 *   power_off_now        sys_poweroff() on this thread: the same
 *                        SYS_POWEROFF broadcast a power-button press
 *                        makes, to hit the shutdown path mid-operation
 *                        rather than only at "quit"/EOF
 *   title_phase title|clock|auto  the shell header's Time in Title text,
 *                        frozen (a scripted run starts on "title") or
 *                        alternating; see sim_cmd_clock.c
 *   sim_clock HH:MM|rtc  the header clock's time (rtc: the host's)
 *   clock_format 12|24   the 24 Hour Clock setting
 *   drift_phase <ticks>  #38 (docs/phase-nav-brief.md): pins
 *                        apps/ipodui/preview.h's panel phase (lit ticks
 *                        since shell start) to an exact value, for
 *                        deterministic drift/Extras-clock goldens and
 *                        review videos; see sim_cmd_nav.c
 *   expect_shadow <label> [tol]  the split shadow at the panel seam, once;
 *                        see sim_cmd_shadow.h
 *   record <ms> <fps> <name>
 *                        dumps the LCD at a steady 'fps' (clamped 1..30)
 *                        for 'ms', into <shotdir>/<name>_frames/
 *                        frame%05d.bmp; scripts/make_video.sh stitches
 *                        them into a review video. A second "record"
 *                        with the same <name> continues the sequence
 *                        (several non-contiguous bursts, one video); see
 *                        sim_cmd_record.c
 *   audio_stop           #38 integration fix-up: calls audio_stop()
 *                        directly, the same stop a real "Stop" makes,
 *                        without holding a button (a held PLAY now
 *                        means sleep from the shell, Revision 3's global
 *                        buttons; see sim_cmd_audio.c)
 *   hold_switch on|off, backlight_expire, usb_glyph usbc|dock30|auto,
 *   splash_probe <text>, yesno_probe <text>, capture <name>, storage_idle
 *                        Phase 8a system-screen hooks (Hold switch with
 *                        key dropping, the real BACKLIGHT_OFF path, the
 *                        USB idle glyph pin, a stock splashf() and yes/no
 *                        prompt, the boot
 *                        capture call, the storage-idle notifies); see
 *                        sim_cmd_sys.h
 *   usb_eject [off], type <chars>, expect_queue <n> <first-path>,
 *   otg_dump, db_rebuild_keep_otg [ms]
 *                        Phase 11 (eject, the Search strip, the queue, the
 *                        On-The-Go store); see sim_cmd_p11.h
 *   stat_mark, expect_stat, expect_stat_delta, expect_in_db,
 *   expect_not_in_db
 *                        Phase 9 trim read-backs; see sim_cmd_p9a.h
 *   dbrec <sub> [args]   #85 library boot recovery: damage the test
 *                        disk's generations/manifests/journal and check
 *                        the next boot's choice; see sim_cmd_dbrec.h
 *   alloc_dump, open_count, run_rock <path> [param]
 *                        Phase 9 (buflib allocations, file opens, a rock
 *                        under --ipodui-plugins); see sim_cmd_p9.h
 *   alarm_boot <s>, pmu_arm, pmu_disarm, pmu_fire, pmu_dump, expect_pmu,
 *   rtc_at, piezo_mark, expect_piezo, press_hold <A> <B> <ms> [lead]
 *                        Phase 12 (the PMU alarm stub, the piezo trace,
 *                        two overlapping buttons); see sim_cmd_extras.h
 *
 * NAME is either a button alias (MENU, PLAY, LEFT/PREV, RIGHT/NEXT, SELECT,
 * SCROLL_FWD, SCROLL_BACK, HOLD; mapped to the SDL keys from the target's
 * uisimulator/buttonmap file) or any SDL key name, e.g. "Return", "F5".
 *
 * Keys are fed through the same button_event() path as real SDL key events.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <limits.h>
#include <SDL.h>
#include "config.h"
#include "system.h"
#include "kernel.h"
#include "thread.h"
#include "screendump.h"
#ifdef HAVE_BCM_JOBS_MODEL
#include "sim_bcm_job.h"
#endif
#include "button-sdl.h"
#include "button.h"
#include "backlight.h"
#include "powermgmt.h"
#include "sim_tasks.h"
#include "sim_db.h"
#include "sim_cmd_ab.h"
#include "sim_cmd_art.h"
#include "sim_cmd_audio.h"
#include "sim_cmd_clock.h"
#include "sim_cmd_floor.h"
#include "sim_cmd_maint.h"
#include "sim_cmd_mod.h"
#include "sim_cmd_extras.h"
#include "sim_cmd_nav.h"
#include "sim_cmd_shadow.h"
#include "sim_cmd_np.h"
#include "sim_cmd_video.h"
#include "sim_cmd_record.h"
#include "sim_cmd_sys.h"
#include "sim_cmd_p11.h"
#include "sim_cmd_p9a.h"
#include "sim_cmd_p9.h"
#include "sim_cmd_fit3a.h"
#include "sim_cmd_dbrec.h"
#include "sim_cmd_sleep.h"
#include "sim_cmd_storpwr.h"
#include "sim_cmd_bcm.h"
#include "sim_cmd_bench.h"

extern const char *sim_script_file;
extern const char *sim_shot_dir;

/* from filesystem-sim.c; file.h isn't included so rename() is the host's */
int sim_get_os_path(char *buffer, const char *path, size_t bufsize);

#define SCRIPT_SHOT_TMP     "/.sim_script_shot.bmp"
#define KEY_DEFAULT_MS      100
#define WHEEL_CLICK_MS      40
#define CHORD_STAGGER_MS    50
#define CHORD_MAX_KEYS      4
#define DB_POLL_MS          100
#define DB_WAIT_DEFAULT_MS  30000
#define DB_BOOT_CHECK_MS    10000
#define UPDATE_NOCHANGE_MS  3000

static long script_stack[DEFAULT_STACK_SIZE/sizeof(long)];
static const char script_thread_name[] = "sim_script";

static const struct
{
    const char *name;
    int key;
} key_aliases[] =
{
    { "MENU",        SDLK_ESCAPE },
    { "PLAY",        SDLK_SPACE  },
    { "LEFT",        SDLK_LEFT   },
    { "PREV",        SDLK_LEFT   },
    { "RIGHT",       SDLK_RIGHT  },
    { "NEXT",        SDLK_RIGHT  },
    { "SELECT",      SDLK_RETURN },
    { "SCROLL_FWD",  SDLK_DOWN   },
    { "SCROLL_BACK", SDLK_UP     },
#ifdef HAS_BUTTON_HOLD
    { "HOLD",        SDLK_h      }, /* toggles the hold switch */
#endif
};

static void sleep_ms(long ms)
{
    if (ms > 0)
        sleep((ms * HZ + 999) / 1000);
}

static int lookup_key(const char *name)
{
    for (unsigned i = 0; i < ARRAYLEN(key_aliases); i++)
    {
        if (!strcasecmp(name, key_aliases[i].name))
            return key_aliases[i].key;
    }

    SDL_Keycode key = SDL_GetKeyFromName(name);
    return key == SDLK_UNKNOWN ? -1 : (int)key;
}

/* sim_cmd_extras.c (press_hold) */
int sim_script_key(const char *name)
{
    return lookup_key(name);
}

/* A key's release on the tick, not on the script thread (p19-gatefix4):
 * a "key X" press is a fixed number of ticks, as a finger's is on the
 * device, however late a loaded host wakes this thread. Before, the
 * release came when the script thread next ran: in the gate's pool a
 * 100 ms SELECT at boot (ipodui-p4-update) was held past button.c's
 * REPEAT_START (300 ms) and input.c's long press (500 ms), so it opened
 * the main menu's long-press menu instead of Music, in three gates. The
 * tick task and button_tick() run in the same tick dispatch, so the
 * driver sees the key down for the same count of ticks on any host. */
static volatile int key_rel_key = -1;   /* the key to release, or -1 */
static volatile long key_rel_tick;      /* at this tick */

static void key_release_tick(void)
{
    int key = key_rel_key;

    if (key >= 0 && TIME_AFTER(current_tick, key_rel_tick - 1))
    {
        /* as before: a release while asleep goes to the sleep (a wake
         * packet; ipodui-p18-sleep-play-release's late PLAY release), else
         * to the driver. The script thread posts nothing meanwhile: it
         * waits in press_key() for this release */
        if (!sim_sleep_key(key, false))
            sim_inject_key(key, false);
        key_rel_key = -1;
    }
}

static void press_key(int key, long ms)
{
    static bool rel_task;
    long ticks = (ms * HZ + 999) / 1000;

    /* while asleep (Phase 18) the sleep takes the press and the release
     * as wheel packets; a release after the wake goes to the driver */
    if (sim_sleep_key(key, true))
    {
        sleep_ms(ms);
        if (!sim_sleep_key(key, false))
            sim_inject_key(key, false);
        return;
    }
    if (!rel_task)
    {
        tick_add_task(key_release_tick);  /* idle while key_rel_key < 0 */
        rel_task = true;
    }
    if (ticks < 1)
        ticks = 1;
    sim_inject_key(key, true);
    key_rel_tick = current_tick + ticks;
    key_rel_key = key;                  /* armed after its tick is set */
    sleep_ms(ms);
    while (key_rel_key >= 0)            /* the next command after the release */
        sleep(1);
}

/* Wheel clicks posted the way the iPod clickwheel driver posts them
 * (firmware/target/arm/ipod/button-clickwheel.c: bit 31 = the data is
 * the driver's filtered speed in degrees/s, 15 per click), so the speed
 * the shell sees is the nominal one, not the host's event timing (fix
 * round M3: a loaded host made the fast-wheel snap flaky). The speed
 * follows the same 15/16 filter apps/ipodui/input.c applies to bare key
 * events, over the nominal spacing; it restarts on a reversal or after
 * a 250 ms pause, as there. */
static void wheel_clicks(int n, long click_ms)
{
    static long last_tick = -HZ;
    static int last_dir;
    static unsigned est;              /* clicks/s */
    int dir = n < 0 ? -1 : 1;
    long dt = click_ms * HZ / 1000;
    int btn = dir < 0 ? BUTTON_SCROLL_BACK : BUTTON_SCROLL_FWD;
    bool first = true;

    if (dt < 1)
        dt = 1;
    if (sim_sleep_wheel(n))
    {
        sleep_ms(click_ms * abs(n));
        return;
    }
    if (dir != last_dir || current_tick - last_tick > HZ / 4)
        est = 0;
    else
        est = (15 * est + HZ / dt) / 16;
    for (n = abs(n); n > 0; n--)
    {
        if (!first)
            est = (15 * est + HZ / dt) / 16;
        first = false;
        backlight_on();
        reset_poweroff_timer();
        if (!button_queue_full())
            button_queue_post(btn, (1u << 31) | (1 << 24) | (est * 15));
        sleep_ms(click_ms);
    }
    last_tick = current_tick;
    last_dir = dir;
}

static void press_chord(char *names, long ms, int lineno)
{
    int keys[CHORD_MAX_KEYS];
    int n = 0;
    char *save;

    for (char *tok = strtok_r(names, "+", &save); tok;
         tok = strtok_r(NULL, "+", &save))
    {
        if (n >= CHORD_MAX_KEYS)
        {
            fprintf(stderr, "sim_script:%d: too many keys in chord\n", lineno);
            return;
        }
        keys[n] = lookup_key(tok);
        if (keys[n] < 0)
        {
            fprintf(stderr, "sim_script:%d: unknown key '%s'\n", lineno, tok);
            return;
        }
        n++;
    }

    if (n < 2)
    {
        fprintf(stderr, "sim_script:%d: chord needs two or more keys\n",
                lineno);
        return;
    }

    for (int i = 0; i < n; i++)
    {
        if (i > 0)
            sleep_ms(CHORD_STAGGER_MS);
        sim_inject_key(keys[i], true);
    }
    sleep_ms(ms);
    for (int i = n - 1; i >= 0; i--)
        sim_inject_key(keys[i], false);
}

static void take_shot(const char *name)
{
    char ospath[PATH_MAX];
    char dest[PATH_MAX];
    const char *ext = strrchr(name, '.');

    snprintf(dest, sizeof(dest), "%s/%s%s", sim_shot_dir, name,
             (ext && !strcasecmp(ext, ".bmp")) ? "" : ".bmp");

#ifdef HAVE_BCM_JOBS_MODEL
    sim_bcm_glass(true);        /* P19 screen truth: what the glass shows */
#endif
    if (!screen_dump_to(SCRIPT_SHOT_TMP)
        || sim_get_os_path(ospath, SCRIPT_SHOT_TMP, sizeof(ospath)) < 0
        || rename(ospath, dest) != 0)
    {
#ifdef HAVE_BCM_JOBS_MODEL
        sim_bcm_glass(false);
#endif
        fprintf(stderr, "sim_script: failed to write %s\n", dest);
        return;
    }
#ifdef HAVE_BCM_JOBS_MODEL
    sim_bcm_glass(false);
#endif

    printf("sim_script: shot %s\n", dest);
    fflush(stdout);
}

static void wait_db(long timeout_ms, int lineno)
{
    long waited = 0;
    enum sim_db_state st;

    while (((st = sim_db_state()) == SIM_DB_BUSY || st == SIM_DB_MISSING)
           && waited < timeout_ms)
    {
        sleep_ms(DB_POLL_MS);
        waited += DB_POLL_MS;
    }

    if (st == SIM_DB_READY)
        fprintf(stderr, "sim_script: wait_db ready after %ld ms\n", waited);
    else if (st == SIM_DB_DELAYED)
        fprintf(stderr, "sim_script:%d: wait_db commit delayed to the next "
                "boot after %ld ms\n", lineno, waited);
    else if (st == SIM_DB_NONE)
        fprintf(stderr, "sim_script:%d: wait_db: no database in this build\n",
                lineno);
    else
        fprintf(stderr, "sim_script:%d: wait_db timed out after %ld ms\n",
                lineno, waited);
}

/* wait_commit [ms]: until a commit runs, then until it ends; the command
 * after it lands between the commit and its candidate's publication
 * (tests/perf/resort-play-50k.script) */
static long commits_seen;       /* the count the last wait_commit saw */

static void wait_commit(long timeout_ms, int lineno)
{
    long waited = 0, n;

    while ((n = sim_db_commits()) >= 0 && n <= commits_seen
           && waited < timeout_ms)
    {
        sleep_ms(10);
        waited += 10;
    }
    if (n > commits_seen)
    {
        commits_seen = n;
        fprintf(stderr, "sim_script: wait_commit done after %ld ms "
                "commits=%ld\n", waited, n);
    }
    else
        fprintf(stderr, "sim_script:%d: wait_commit timed out after %ld ms%s\n",
                lineno, waited, n < 0 ? " (no commit counter)" : "");
}

static void update_db_wait(long timeout_ms, int lineno)
{
    int gen = sim_db_generation(), g;
    long waited = 0;

    sim_db_update();
    /* a new RAM cache generation (a reload after a commit, or an entry
     * deleted in place), then usable again. An update that finds nothing
     * to add or drop changes nothing: give it a few seconds (the 300-track
     * sim library scans in ~0.1 s), as without a generation at all. */
    for (;;)
    {
        g = sim_db_generation();
        if (sim_db_state() == SIM_DB_READY && g >= -1
            && (g != gen || waited >= UPDATE_NOCHANGE_MS))
            break;
        if (waited >= timeout_ms)
        {
            fprintf(stderr, "sim_script:%d: update_db_wait timed out after "
                    "%ld ms\n", lineno, waited);
            return;
        }
        sleep_ms(DB_POLL_MS);
        waited += DB_POLL_MS;
    }
    fprintf(stderr, "sim_script: update_db_wait ready after %ld ms%s\n",
            waited, g == gen ? " (no change)" : "");
}

/* Host path of 'name' relative to the script's directory */
static void script_relative(char *buf, size_t size, const char *name)
{
    const char *slash = strrchr(sim_script_file, '/');
    int dir = slash ? (int)(slash - sim_script_file) + 1 : 0;
    /* scripts/shots.sh runs a script with "reboot" in parts, from its
     * output directory: SIM_SCRIPT_DIR is the original's directory */
    const char *base = getenv("SIM_SCRIPT_DIR");

    if (name[0] == '/')
        snprintf(buf, size, "%s", name);
    else if (base && *base)
        snprintf(buf, size, "%s/%s", base, name);
    else
        snprintf(buf, size, "%.*s%s", dir, sim_script_file, name);
}

static void copy_host_file(const char *src, const char *dst, int lineno)
{
    char from[PATH_MAX], to[PATH_MAX];
    char buf[4096];
    FILE *in, *out;
    size_t n;

    script_relative(from, sizeof(from), src);
    if (sim_get_os_path(to, dst, sizeof(to)) < 0)
    {
        fprintf(stderr, "sim_script:%d: copy: bad path %s\n", lineno, dst);
        return;
    }
    in = fopen(from, "rb");
    out = in ? fopen(to, "wb") : NULL;
    if (!in || !out)
    {
        fprintf(stderr, "sim_script:%d: copy %s -> %s failed\n", lineno,
                from, to);
        if (in)
            fclose(in);
        return;
    }
    while ((n = fread(buf, 1, sizeof(buf), in)) > 0)
        fwrite(buf, 1, n, out);
    fclose(in);
    fclose(out);
    fprintf(stderr, "sim_script: copied %s to %s\n", src, dst);
}

static void build_db(int lineno)
{
    long waited = 0;
    enum sim_db_state st;

    /* the tagcache thread checks the headers about 1 s after boot */
    while ((st = sim_db_state()) == SIM_DB_BUSY && waited < DB_BOOT_CHECK_MS)
    {
        sleep_ms(DB_POLL_MS);
        waited += DB_POLL_MS;
    }

    if (st == SIM_DB_MISSING)
    {
        fprintf(stderr, "sim_script: build_db: building\n");
        sim_db_build();
    }
    else
        fprintf(stderr, "sim_script:%d: build_db: nothing to do (%s)\n",
                lineno, st == SIM_DB_READY ? "database present" :
                        st == SIM_DB_DELAYED ? "commit delayed" :
                        st == SIM_DB_NONE ? "no database in this build" :
                        "build running");
}

static void update_db_wait_change(long timeout_ms, int lineno)
{
    int gen = sim_db_generation(), g;
    long waited = 0;

    sim_db_update();
    for (;;)
    {
        g = sim_db_generation();
        if (sim_db_state() == SIM_DB_READY && g >= -1 && g != gen)
            break;
        if (waited >= timeout_ms)
        {
            fprintf(stderr, "sim_script:%d: update_db_wait_change failed: "
                    "no change after %ld ms\n", lineno, waited);
            return;
        }
        sleep_ms(DB_POLL_MS);
        waited += DB_POLL_MS;
    }
    fprintf(stderr, "sim_script: update_db_wait_change ready after %ld ms\n",
            waited);
}

static void rebuild_db(long timeout_ms, int lineno)
{
    int gen = sim_db_generation();
    long waited = 0;

    sim_db_rebuild();
    /* the files go first (not usable), then the new database loads */
    while (waited < timeout_ms
           && (sim_db_state() != SIM_DB_READY || sim_db_generation() < 0
               || sim_db_generation() == gen))
    {
        sleep_ms(DB_POLL_MS);
        waited += DB_POLL_MS;
    }
    if (waited < timeout_ms)
        fprintf(stderr, "sim_script: rebuild_db ready after %ld ms\n",
                waited);
    else
        fprintf(stderr, "sim_script:%d: rebuild_db timed out after %ld ms\n",
                lineno, waited);
}

static int db_mark_entries = -1;

static void expect_lib_updating(long ms, int lineno)
{
    long t;

    for (t = 0; t < ms && sim_db_lib_updating(); t += DB_POLL_MS)
        sleep_ms(DB_POLL_MS);
    if (t >= ms && sim_db_lib_updating())
        fprintf(stderr, "sim_script: expect_lib_updating ok (%ld ms)\n", ms);
    else
        fprintf(stderr, "sim_script:%d: expect_lib_updating failed after "
                "%ld ms\n", lineno, t);
}

static void expect_lib_browsable(long ms, int lineno)
{
    long t;

    for (t = 0; t < ms && !sim_db_lib_updating() && sim_db_entries() >= 0;
         t += DB_POLL_MS)
        sleep_ms(DB_POLL_MS);
    if (t >= ms)
        fprintf(stderr, "sim_script: expect_lib_browsable ok (%ld ms)\n", ms);
    else
        fprintf(stderr, "sim_script:%d: expect_lib_browsable failed after "
                "%ld ms\n", lineno, t);
}

static int db_gen_marked = -1;

static void expect_db_gen(bool change, long ms, int lineno)
{
    long t;

    for (t = 0; t < ms; t += DB_POLL_MS)
    {
        bool moved = sim_db_active_generation() != db_gen_marked;
        if (change ? moved && sim_db_entries() >= 0 : moved)
            break;
        sleep_ms(DB_POLL_MS);
    }
    if (change ? t < ms : t >= ms)
        fprintf(stderr, "sim_script: expect_db_gen_%s ok after %ld ms\n",
                change ? "change" : "same", t);
    else
        fprintf(stderr, "sim_script:%d: expect_db_gen_%s failed after %ld ms\n",
                lineno, change ? "change" : "same", t);
}

static void expect_db_same(long ms, int lineno)
{
    long t;
    int n = sim_db_entries();

    for (t = 0; t < ms && n == db_mark_entries; t += DB_POLL_MS)
    {
        sleep_ms(DB_POLL_MS);
        n = sim_db_entries();
    }
    if (n == db_mark_entries)
        fprintf(stderr, "sim_script: expect_db_same ok: %d entries for "
                "%ld ms\n", n, ms);
    else
        fprintf(stderr, "sim_script:%d: expect_db_same failed: %d -> %d "
                "entries after %ld ms\n", lineno, db_mark_entries, n, t);
}

static void expect_db_grew(long timeout_ms, int lineno)
{
    long waited = 0;
    int n = -1;

    while (waited < timeout_ms)
    {
        n = sim_db_entries();
        if (sim_db_state() == SIM_DB_READY && n > db_mark_entries)
            break;
        sleep_ms(DB_POLL_MS);
        waited += DB_POLL_MS;
    }
    if (waited < timeout_ms)
        fprintf(stderr, "sim_script: expect_db_grew ok: %d -> %d entries "
                "after %ld ms\n", db_mark_entries, n, waited);
    else
        fprintf(stderr, "sim_script:%d: expect_db_grew failed: %d -> %d "
                "entries after %ld ms\n", lineno, db_mark_entries, n, waited);
}

static char *unescape(char *path);

static void expect_resume(char *path, long min_ms, long wait_ms, int lineno)
{
    long waited = 0, e;

    unescape(path);
    while ((e = sim_db_lastelapsed(path)) < min_ms && waited < wait_ms)
    {
        sleep_ms(DB_POLL_MS);
        waited += DB_POLL_MS;
    }
    if (e >= min_ms)
        fprintf(stderr, "sim_script: expect_resume ok: %s at %ld ms\n",
                path, e);
    else
        fprintf(stderr, "sim_script:%d: expect_resume failed: %s at %ld "
                "(-1 none, -2 not in the database)\n", lineno, path, e);
}

static void import_changelog(void)
{
    long start = current_tick;
    bool ok = sim_db_import();

    fprintf(stderr, "sim_script: import_changelog %s in %ld ms\n",
            ok ? "done" : "nothing", (current_tick - start) * 1000 / HZ);
}

static void expect_playing(long ms, int lineno)
{
    long t;
    int st = sim_audio_status();

    for (t = 0; t < ms && st == 1; t += 10)
    {
        sleep_ms(10);
        st = sim_audio_status();
    }
    if (st == 1)
        fprintf(stderr, "sim_script: expect_playing ok (%ld ms)\n", ms);
    else
        fprintf(stderr, "sim_script:%d: expect_playing failed: audio status "
                "%d after %ld ms\n", lineno, st, t);
}

/* paused: AUDIO_STATUS_PLAY | AUDIO_STATUS_PAUSE (3) all along */
static void expect_paused(long ms, int lineno)
{
    long t;
    int st = sim_audio_status();

    for (t = 0; t < ms && st == 3; t += 10)
    {
        sleep_ms(10);
        st = sim_audio_status();
    }
    if (st == 3)
        fprintf(stderr, "sim_script: expect_paused ok (%ld ms)\n", ms);
    else
        fprintf(stderr, "sim_script:%d: expect_paused failed: audio status "
                "%d after %ld ms\n", lineno, st, t);
}

/* script paths can't hold spaces: "%20" stands for one */
static char *unescape(char *path)
{
    char *r = path, *w = path;

    while (*r)
    {
        if (r[0] == '%' && r[1] == '2' && r[2] == '0')
        {
            *w++ = ' ';
            r += 3;
        }
        else
            *w++ = *r++;
    }
    *w = '\0';
    return path;
}

static void copy_file(char *src, char *dst, int lineno)
{
    char from[PATH_MAX], to[PATH_MAX], buf[8192];
    FILE *in = NULL, *out = NULL;
    size_t n;
    bool ok = false;

    unescape(src);
    unescape(dst);
    if (sim_get_os_path(from, src, sizeof(from)) >= 0
        && sim_get_os_path(to, dst, sizeof(to)) >= 0
        && (in = fopen(from, "rb")) && (out = fopen(to, "wb")))
    {
        ok = true;
        while ((n = fread(buf, 1, sizeof(buf), in)) > 0)
            ok = ok && fwrite(buf, 1, n, out) == n;
    }
    if (in)
        fclose(in);
    if (out && fclose(out) != 0)
        ok = false;
    if (ok)
        fprintf(stderr, "sim_script: copy_file %s -> %s\n", src, dst);
    else
        fprintf(stderr, "sim_script:%d: copy_file %s -> %s failed\n",
                lineno, src, dst);
}

static void rm_file(char *path, int lineno)
{
    char ospath[PATH_MAX];

    unescape(path);
    if ((sim_get_os_path(ospath, path, sizeof(ospath)) >= 0
         && remove(ospath) == 0)
        || (!strncmp(path, "/.rockbox/database_", 19)
            && sim_db_remove_active(path + 10)))
        fprintf(stderr, "sim_script: rm_file %s\n", path);
    else
        fprintf(stderr, "sim_script:%d: rm_file %s: not removed\n",
                lineno, path);
}

/* Returns false when the script asks to quit */
static bool run_line(char *line, int lineno)
{
    char *argv[4] = { NULL, NULL, NULL, NULL };
    int argc = 0;
    char *save;

    char *hash = strchr(line, '#');
    if (hash)
        *hash = '\0';

    for (char *tok = strtok_r(line, " \t\r\n", &save);
         tok && argc < 4; tok = strtok_r(NULL, " \t\r\n", &save))
    {
        argv[argc++] = tok;
    }

    if (argc == 0)
        return true;

    const char *cmd = argv[0];

    if (!strcmp(cmd, "quit"))
        return false;
    else if (!strcmp(cmd, "reboot"))
    {
        fprintf(stderr, "sim_script: reboot\n");
        return false;   /* scripts/shots.sh boots again for the rest */
    }
    else if (!strcmp(cmd, "wait") && argc >= 2)
        sleep_ms(atol(argv[1]));
    else if ((!strcmp(cmd, "key") && argc >= 2) ||
             (!strcmp(cmd, "hold") && argc >= 3))
    {
        int key = lookup_key(argv[1]);
        if (key < 0)
            fprintf(stderr, "sim_script:%d: unknown key '%s'\n",
                    lineno, argv[1]);
        else
            press_key(key, argc >= 3 ? atol(argv[2]) : KEY_DEFAULT_MS);
    }
    else if (!strcmp(cmd, "wheel") && argc >= 2)
    {
        long click_ms = argc >= 3 ? atol(argv[2]) : WHEEL_CLICK_MS;
        if (click_ms < 10)
            click_ms = 10;      /* one tick: the wheel driver's resolution */
        wheel_clicks(atoi(argv[1]), click_ms);
    }
    else if (!strcmp(cmd, "chord") && argc >= 3)
        press_chord(argv[1], atol(argv[2]), lineno);
    else if (!strcmp(cmd, "shot") && argc >= 2)
        take_shot(argv[1]);
    else if (!strcmp(cmd, "wait_db"))
        wait_db(argc >= 2 ? atol(argv[1]) : DB_WAIT_DEFAULT_MS, lineno);
    else if (!strcmp(cmd, "wait_commit"))
        wait_commit(argc >= 2 ? atol(argv[1]) : DB_WAIT_DEFAULT_MS, lineno);
    else if (!strcmp(cmd, "build_db"))
        build_db(lineno);
    else if (!strcmp(cmd, "update_db"))
    {
        fprintf(stderr, "sim_script: update_db\n");
        sim_db_update();
    }
    else if (!strcmp(cmd, "update_db_wait"))
        update_db_wait(argc >= 2 ? atol(argv[1]) : DB_WAIT_DEFAULT_MS,
                       lineno);
    else if (!strcmp(cmd, "update_db_wait_change"))
        update_db_wait_change(argc >= 2 ? atol(argv[1]) : DB_WAIT_DEFAULT_MS,
                              lineno);
    else if (!strcmp(cmd, "rebuild_db"))
        rebuild_db(argc >= 2 ? atol(argv[1]) : DB_WAIT_DEFAULT_MS, lineno);
    else if (!strcmp(cmd, "scan_slow") && argc >= 2)
    {
        extern int sim_scan_slow_ticks;     /* sim_cmd_scan.c */
        sim_scan_slow_ticks = atoi(argv[1]);
        fprintf(stderr, "sim_script: scan_slow %d\n", sim_scan_slow_ticks);
    }
    else if (!strcmp(cmd, "rebuild_db_start"))
    {
        fprintf(stderr, "sim_script: rebuild_db_start\n");
        sim_db_rebuild();
    }
    else if (!strcmp(cmd, "dirty_db"))
    {
        if (sim_db_set_dirty())
            fprintf(stderr, "sim_script: dirty_db ok\n");
        else
            fprintf(stderr, "sim_script:%d: dirty_db failed\n", lineno);
    }
    else if (!strcmp(cmd, "expect_lib_updating") && argc >= 2)
        expect_lib_updating(atol(argv[1]), lineno);
    else if (!strcmp(cmd, "expect_db_same") && argc >= 2)
        expect_db_same(atol(argv[1]), lineno);
    else if (!strcmp(cmd, "expect_resume") && argc >= 2)
        expect_resume(argv[1], argc >= 3 ? atol(argv[2]) : 1,
                      argc >= 4 ? atol(argv[3]) : 5000, lineno);
    else if (!strcmp(cmd, "import_changelog"))
        import_changelog();
    else if (!strcmp(cmd, "expect_lib_browsable") && argc >= 2)
        expect_lib_browsable(atol(argv[1]), lineno);
    else if (!strcmp(cmd, "db_gen_mark"))
    {
        db_gen_marked = sim_db_active_generation();
        fprintf(stderr, "sim_script: db_gen_mark %d\n", db_gen_marked);
    }
    else if (!strcmp(cmd, "expect_db_gen_same") && argc >= 2)
        expect_db_gen(false, atol(argv[1]), lineno);
    else if (!strcmp(cmd, "expect_db_gen_change"))
        expect_db_gen(true, argc >= 2 ? atol(argv[1]) : 30000, lineno);
    else if (!strcmp(cmd, "db_mark"))
    {
        db_mark_entries = sim_db_entries();
        fprintf(stderr, "sim_script: db_mark %d entries\n", db_mark_entries);
    }
    else if (!strcmp(cmd, "expect_db_grew"))
        expect_db_grew(argc >= 2 ? atol(argv[1]) : DB_WAIT_DEFAULT_MS,
                       lineno);
    else if (!strcmp(cmd, "expect_playing") && argc >= 2)
        expect_playing(atol(argv[1]), lineno);
    else if (!strcmp(cmd, "expect_paused") && argc >= 2)
        expect_paused(atol(argv[1]), lineno);
    else if (!strcmp(cmd, "copy_file") && argc >= 3)
        copy_file(argv[1], argv[2], lineno);
    else if (!strcmp(cmd, "copy") && argc >= 3)
        copy_host_file(argv[1], unescape(argv[2]), lineno);  /* "%20" */
    else if ((!strcmp(cmd, "rm_file") || !strcmp(cmd, "remove"))
             && argc >= 2)
        rm_file(argv[1], lineno);
    else if (sim_cmd_ab(cmd, argv, argc, lineno))
        /* handled by uisimulator/common/sim_cmd_ab.c */;
    else if (sim_cmd_art(cmd, argv, argc, lineno))
        /* handled by uisimulator/common/sim_cmd_art.c */;
    else if (sim_cmd_audio(cmd, argv, argc, lineno))
        /* handled by uisimulator/common/sim_cmd_audio.c */;
    else if (sim_cmd_clock(cmd, argv, argc, lineno))
        /* handled by uisimulator/common/sim_cmd_clock.c */;
    else if (sim_cmd_floor(cmd, argv, argc, lineno))
        /* handled by uisimulator/common/sim_cmd_floor.c */;
    else if (sim_cmd_maint(cmd, argv, argc, lineno))
        /* handled by uisimulator/common/sim_cmd_maint.c */;
    else if (sim_cmd_mod(cmd, argv, argc, lineno))
        /* handled by uisimulator/common/sim_cmd_mod.c */;
    else if (sim_cmd_extras(cmd, argv, argc, lineno))
        /* handled by uisimulator/common/sim_cmd_extras.c (P12) */;
    else if (sim_cmd_shadow(cmd, argv, argc, lineno))
        /* handled by uisimulator/common/sim_cmd_shadow.c */;
    else if (sim_cmd_nav(cmd, argv, argc, lineno))
        /* handled by uisimulator/common/sim_cmd_nav.c */;
    else if (sim_cmd_np(cmd, argv, argc, lineno))
        /* handled by uisimulator/common/sim_cmd_np.c */;
    else if (sim_cmd_record(cmd, argv, argc, lineno))
        /* handled by uisimulator/common/sim_cmd_record.c */;
    else if (sim_cmd_sys(cmd, argv, argc, lineno))
        /* handled by uisimulator/common/sim_cmd_sys.c */;
    else if (sim_cmd_p11(cmd, argv, argc, lineno))
        /* handled by uisimulator/common/sim_cmd_p11.c */;
    else if (sim_cmd_p9a(cmd, argv, argc, lineno))
        /* handled by uisimulator/common/sim_cmd_p9a.c */;
    else if (sim_cmd_p9(cmd, argv, argc, lineno))
        /* handled by uisimulator/common/sim_cmd_p9.c */;
    else if (sim_cmd_video(cmd, argv, argc, lineno))
        /* handled by uisimulator/common/sim_cmd_video.c */;
    else if (sim_cmd_fit3a(cmd, argv, argc, lineno))
        /* handled by uisimulator/common/sim_cmd_fit3a.c */;
    else if (sim_cmd_dbrec(cmd, argv, argc, lineno))
        /* handled by uisimulator/common/sim_cmd_dbrec.c */;
    else if (sim_cmd_sleep(cmd, argv, argc, lineno))
        /* handled by uisimulator/common/sim_cmd_sleep.c */;
    else if (sim_cmd_storpwr(cmd, argv, argc, lineno))
        /* handled by uisimulator/common/sim_cmd_storpwr.c */;
    else if (sim_cmd_bcm(cmd, argv, argc, lineno))
        /* handled by uisimulator/common/sim_cmd_bcm.c */;
    else if (sim_cmd_bench(cmd, argv, argc, lineno))
        /* handled by uisimulator/common/sim_cmd_bench.c */;
    else
        fprintf(stderr, "sim_script:%d: bad command '%s'\n", lineno, cmd);

    return true;
}

static void script_thread(void)
{
    char line[256];
    int lineno = 0;
    FILE *f = fopen(sim_script_file, "r");

    if (!f)
    {
        fprintf(stderr, "sim_script: can't open %s\n", sim_script_file);
    }
    else
    {
        while (fgets(line, sizeof(line), f) && run_line(line, ++lineno));
        fclose(f);
    }

    printf("sim_script: done, quitting\n");
    sdl_sys_quit();

    while (1)
        sleep(HZ);
}

#ifdef HAVE_IPOD_UI
/* #118: the script thread plays the host (copy, rm_file, shot ...):
 * filesystem-sim.c leaves its writes out of the USB host window */
unsigned int sim_script_thread_id = ~0u;
#endif

void sim_load_start(void);      /* sim_cmd_load.c: host-jitter watcher */

void sim_script_init(void)
{
    sim_load_start();
    if (!sim_script_file)
        return;

#ifdef HAVE_IPOD_UI
    sim_script_thread_id =
#endif
    create_thread(script_thread, script_stack, sizeof(script_stack), 0,
                  script_thread_name IF_PRIO(, PRIORITY_BACKGROUND)
                  IF_COP(, CPU));
}
