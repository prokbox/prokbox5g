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

/* Host test: the target's module load path (apps/ipodui/modload.c built
 * with IPODUI_MODLOAD) against a real modules.pak.
 *
 * The simulator links modules statically, so before this test the pack
 * open, the index compaction, the reads, the relocation and the module
 * header check ran only on the iPod. This runs modload.c itself, with
 * host stand-ins (host/modhost.h) for the storage thread (jobs run at
 * once), core_alloc and file I/O, and the boot order of ui_main.c:
 *  1. a warm-up before mod_init() (ui_theme_init() -> ui_settings_init()
 *     -> refresh() -> mod_warm(MOD_SETTINGS)): must change nothing;
 *  2. mod_init(), then every module in the pack loaded through
 *     mod_export() (the blocking path), so the arena's room policy
 *     evicts, moves and places as on the device: slot 0 must be the
 *     module header the pack entry describes, every other export a run
 *     address inside the module (image or .bss);
 *  3. the pack-open log line reports the real count and size;
 *  4. a USB session (mod_usb_connect/release) and a reload: the pack
 *     reopens;
 *  5. the pack missing: loads fail, then it comes back and the next
 *     attempt works (no state sticks).
 *
 * Usage: test_modload_pak <modules.pak> */
#include <stdio.h>
#include <stdlib.h>
#include <signal.h>
#include "modhost.h"
#include "ipodui/modload.h"
#include "ipodui/extras.h"
#include "ipodui/theme.h"
#include "ipodui/screen_stack.h"

static int fails;
#define CHECK(c) do { if (!(c)) { fails++; \
    fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)

/* -- the host stand-ins ---------------------------------------------------- */
uint8_t *modhost_arena_base;
static size_t arena_block;
static long ticks;
long modhost_tick(void) { return ticks++; }  /* every read moves time on */
struct user_settings global_settings;
struct frame_buffer_t lcd_framebuffer_default;
const struct extra_ent extras_table[XT__N];
const struct ui_theme *ui_theme;

static const char *pak_path;            /* NULL: the pack is missing */
static char tmp_pak[512];

#undef open
int modhost_open(const char *path, int flags)
{
    if (!strcmp(path, ROCKBOX_DIR "/modules.pak"))
        return pak_path ? open(pak_path, flags) : -1;
    return -1;                          /* snap.pak and the rest: absent */
}

const void *modhost_ptr(uint32_t a)
{
    if (a < MODHOST_RUN_BASE || a >= MODHOST_RUN_BASE + arena_block)
        return (const void *)(uintptr_t)a;  /* not in the arena: as is */
    return modhost_arena_base + (a - MODHOST_RUN_BASE);
}

void semaphore_init(struct semaphore *s, int max, int start)
{ (void)max; s->count = start; }
int semaphore_wait(struct semaphore *s, int timeout)
{
    ticks += timeout > 0 ? timeout : 1;
    if (s->count > 0) { s->count--; return 0; }
    return -1;
}
void semaphore_release(struct semaphore *s) { s->count = 1; }
void modhost_sleep(int t) { ticks += t; }
unsigned int thread_self(void) { return 1; }
int thread_get_debug_info(unsigned int id, struct thread_debug_info *info)
{ (void)id; info->stack_usage = 42; return 0; }
void commit_discard_idcache(void) {}
void cpu_boost(bool on) { (void)on; }

static int allocs;
int core_alloc(size_t size)
{
    if (allocs++)
        return -1;                      /* the arena is the only client */
    arena_block = size;
    modhost_arena_base = malloc(size);
    memset(modhost_arena_base, 0xa5, size);
    return 1;
}
void *core_get_data(int h) { return h == 1 ? modhost_arena_base : NULL; }
bool core_pin(int h) { (void)h; return true; }

static int jobs;
bool storage_post_job(void (*fn)(void)) { jobs++; fn(); return true; }
bool storage_disk_is_active(void) { return true; }

void lcd_set_viewport(struct viewport *vp) { (void)vp; }
void lcd_set_drawmode(int m) { (void)m; }
void lcd_set_foreground(unsigned c) { (void)c; }
void lcd_fillrect(int x, int y, int w, int h) { (void)x; (void)y; (void)w; (void)h; }
void lcd_update(void) {}
void lcd_update_rect(int x, int y, int w, int h) { (void)x; (void)y; (void)w; (void)h; }
struct tm *get_time(void) { return NULL; }

bool extras_module_live(int mod) { (void)mod; return false; }
int extras_top_module(void) { return -1; }
void modclass_top(int m) { (void)m; }
void modclass_pass(void) {}
void ui_boost_burst_poll(void) {}
int sdk_screen_mod(const struct ui_screen_ops *ops) { (void)ops; return -1; }
bool xindex_busy(void) { return false; }
void xindex_cancel(void) {}
struct ui_screen *ui_top(void) { return NULL; }
void ui_invalidate(unsigned d) { (void)d; }
void ui_clear_dirty(void) {}
uint16_t ui_arena_mark(void) { return 0; }
void ui_arena_reset(uint16_t m) { (void)m; }
void ui_extras_page_backdrop(int y0, int h, int w) { (void)y0; (void)h; (void)w; }
void ui_prompt_draw(int h, int b) { (void)h; (void)b; }

/* ui_log: kept, so the test reads the lines the device would log */
static char last_open[128];
static int verbose;
void ui_log(const char *tag, const char *fmt, ...)
{
    char line[160];
    va_list ap;

    va_start(ap, fmt);
    vsnprintf(line, sizeof(line), fmt, ap);
    va_end(ap);
    if (verbose)
        fprintf(stderr, "  log: %s %s\n", tag, line);
    if (!strncmp(line, "job_open ", 9))
        snprintf(last_open, sizeof(last_open), "%s", line);
}

/* modload.c's build-ID word: the pack copy carries the same value */
extern const volatile uint32_t mod_core_build_id[2];

static void on_segv(int sig)
{
    (void)sig;
    fprintf(stderr, "FAIL: crashed (a read through a NULL arena: the index "
            "stage before mod_init()?)\n");
    _exit(1);
}

/* -- the test ------------------------------------------------------------------ */
static const char *const names[MOD__N] = {
    "settings", "devloader", "clocks", "alarms", "stopwatch", "lockset",
    "notes", "contacts", "calendars", "xindex", "solitaire", "badgame",
    "quadrus", "bricks", "chute", "chess",
};

static struct modpak_hdr hdr;
static long pak_size;

/* A module in the arena: slot 0 is the header its pack entry describes,
 * every other export lies inside it (after its load and after every
 * move, which relocates it again) */
static void check_module(const struct modpak_ent *e, const char *pass)
{
    int id = (int)e->id, s;
    const struct mod_desc *d = mod_export_now((enum mod_id)id, MOD_SLOT_DESC);
    uintptr_t lo = 0, hi = 0;

    if (!d || !mod_image_range((enum mod_id)id, &lo, &hi))
    {
        fprintf(stderr, "FAIL %s: %s has no header\n", pass, names[id]);
        fails++;
        return;
    }
    CHECK(mod_desc_check(d, e) == MODPAK_OK);
    CHECK(hi - lo == e->image + e->bss);    /* image, then .bss */
    CHECK((uintptr_t)d >= lo && (uintptr_t)d + sizeof(*d) <= lo + e->image);
    for (s = MOD_SLOT_FIRST; s < (int)e->nexport; s++)
    {
        const void *x = mod_export_now((enum mod_id)id, s);

        if (!x || (uintptr_t)x < lo || (uintptr_t)x >= hi)
        {
            fprintf(stderr, "FAIL %s: %s export %d %p outside "
                    "[%#lx, %#lx)\n", pass, names[id], s, x,
                    (unsigned long)lo, (unsigned long)hi);
            fails++;
        }
    }
}

/* every pack entry, through the blocking path; returns how many loaded */
static int load_all(const char *pass)
{
    int i, loaded = 0;

    for (i = 0; i < hdr.count; i++)
    {
        const struct modpak_ent *e = &hdr.ent[i];
        int id = (int)e->id;
        int why = mod_why_hidden(e, MOD_RAM_64, MOD_CLASSES_BUILT);
        const struct mod_desc *d;
        const struct mod_rt *r;

        if (id >= MOD__N)
        {
            fprintf(stderr, "FAIL %s: entry %d has id %d\n", pass, i, id);
            fails++;
            continue;
        }
        d = mod_export((enum mod_id)id, MOD_SLOT_DESC);
        r = mod_rt_get((enum mod_id)id);
        if (why != MOD_WHY_OK)
        {
            /* hidden here: never loaded, never a row */
            if (d || !r || !(r->flags & MOD_RT_HIDDEN))
            {
                fprintf(stderr, "FAIL %s: %s hidden (why %d) but loaded\n",
                        pass, names[id], why);
                fails++;
            }
            continue;
        }
        if (!d || !r || r->state != MOD_READY)
        {
            fprintf(stderr, "FAIL %s: %s did not load (state %d)\n", pass,
                    names[id], r ? r->state : -1);
            fails++;
            continue;
        }
        check_module(e, pass);
        loaded++;
    }
    /* the survivors, moved by the later loads' compactions */
    for (i = 0; i < hdr.count; i++)
        if (hdr.ent[i].id < MOD__N
            && mod_rt_get((enum mod_id)hdr.ent[i].id)->state == MOD_READY)
            check_module(&hdr.ent[i], pass);
    return loaded;
}

int main(int argc, char **argv)
{
    FILE *f, *o;
    uint8_t *buf;
    int n, runnable = 0, i;
    char want[64];

    if (argc < 2)
    {
        fprintf(stderr, "usage: %s modules.pak [tmpdir]\n", argv[0]);
        return 2;
    }
    verbose = getenv("MODLOAD_VERBOSE") != NULL;
    signal(SIGSEGV, on_segv);
    signal(SIGBUS, on_segv);

    /* the pack, with this core's build ID (modpak.py patches the real
     * core's word; here the core is this test) */
    f = fopen(argv[1], "rb");
    if (!f)
    {
        fprintf(stderr, "FAIL: no pack %s\n", argv[1]);
        return 1;
    }
    fseek(f, 0, SEEK_END);
    pak_size = ftell(f);
    fseek(f, 0, SEEK_SET);
    buf = malloc(pak_size);
    if (fread(buf, 1, pak_size, f) != (size_t)pak_size)
        return 1;
    fclose(f);
    memcpy(&hdr, buf, sizeof(hdr));
    CHECK(modpak_check(&hdr, (uint32_t)pak_size, hdr.build_id) == MODPAK_OK);
    hdr.build_id = mod_core_build_id[1];
    memcpy(buf, &hdr, sizeof(hdr));
    snprintf(tmp_pak, sizeof(tmp_pak), "%s/modules.pak",
             argc > 2 ? argv[2] : ".");
    o = fopen(tmp_pak, "wb");
    if (!o || fwrite(buf, 1, pak_size, o) != (size_t)pak_size)
    {
        fprintf(stderr, "FAIL: can't write %s\n", tmp_pak);
        return 1;
    }
    fclose(o);
    free(buf);
    pak_path = tmp_pak;
    for (i = 0; i < hdr.count; i++)
        runnable += mod_why_hidden(&hdr.ent[i], MOD_RAM_64,
                                   MOD_CLASSES_BUILT) == MOD_WHY_OK;

    /* 1. the boot's warm-up, before the arena exists (ui_main.c:
     * ui_theme_init() runs before mod_init()) */
    mod_warm(MOD_SETTINGS);
    CHECK(jobs == 0);
    CHECK(last_open[0] == 0);           /* no pack "opened" with no arena */

    /* 2. shell start, then every module */
    mod_init();
    mod_pass();
    n = load_all("boot");
    CHECK(n == runnable);
    /* 3. the open line: the index's real count and the file's size */
    snprintf(want, sizeof(want), "n=%u bytes=%ld ", hdr.count, pak_size);
    if (!strstr(last_open, want))
    {
        fprintf(stderr, "FAIL: open line \"%s\" wants \"%s\"\n", last_open,
                want);
        fails++;
    }

    /* 4. USB: the pack closes, the next load reopens it (generation 2) */
    mod_usb_connect();
    mod_usb_release();
    mod_pass();
    last_open[0] = 0;
    n = load_all("after usb");
    CHECK(n == runnable);
    CHECK(strstr(last_open, "gen=2") != NULL);

    /* 5. the pack gone: a load fails (the error page), nothing sticks */
    mod_usb_connect();
    pak_path = NULL;
    mod_usb_release();
    for (i = 0; i < hdr.count; i++)
    {
        int id = (int)hdr.ent[i].id;
        const struct mod_rt *r = mod_rt_get((enum mod_id)id);

        if (r->state == MOD_READY)      /* still in RAM: same build, usable */
            CHECK(mod_export((enum mod_id)id, MOD_SLOT_DESC) != NULL);
        else if (!(r->flags & MOD_RT_HIDDEN))
        {
            CHECK(mod_export((enum mod_id)id, MOD_SLOT_DESC) == NULL);
            CHECK(r->state == MOD_FAILED);
        }
    }
    pak_path = tmp_pak;
    mod_pass();
    n = load_all("pack back");
    CHECK(n == runnable);

    remove(tmp_pak);
    if (fails)
    {
        fprintf(stderr, "FAIL: module load path (%d)\n", fails);
        return 1;
    }
    printf("ok   module load path: %d of %u modules loaded and checked "
           "(%d hidden here), pack reopened after USB and after a missing "
           "pack, %d storage jobs\n", runnable, hdr.count,
           hdr.count - runnable, jobs);
    return 0;
}
