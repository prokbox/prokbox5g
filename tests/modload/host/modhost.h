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

/* Host stand-ins for the firmware headers apps/ipodui/modload.c includes,
 * so tests/modload/test_modload_pak.c can run the target's real load path
 * (IPODUI_MODLOAD: pack open, index compaction, read, settle, module
 * header check, export lookup) against a real modules.pak. Every
 * firmware header modload.c names (config.h, system.h, kernel.h, file.h,
 * ...) is a one-line file run.sh writes that includes this one.
 *
 * Addresses: module words are 32-bit run addresses; the host's pointers
 * are 64-bit and live above 4 GB. MOD_RUN_ADDR()/MOD_EXPORT_AT() (the
 * hooks modload.c keeps for this) map the host arena to a 32-bit run
 * window at MODHOST_RUN_BASE. */
#ifndef MODHOST_H
#define MODHOST_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <stdarg.h>
#include <string.h>
#include <sys/types.h>
#include <fcntl.h>
#include <unistd.h>

/* -- config.h ------------------------------------------------------------ */
#define HAVE_IPOD_UI
#define IPODUI_MODULES                  /* ipodvideo.h, IPODUI_WITH_MODULES */
#define HAVE_STORAGE_JOBS
#define HZ              100
#define LCD_WIDTH       320
#define LCD_HEIGHT      240
#define LCD_BLACK       0
#define NUM_CORES       1
#define MEMORYSIZE      64
#define TIMEOUT_BLOCK   (-1)
#define ROCKBOX_DIR     "/.rockbox"
#define ATTRIBUTE_PRINTF(f, a) __attribute__((format(printf, f, a)))
#define MAX(a, b)       ((a) > (b) ? (a) : (b))
#define MIN(a, b)       ((a) < (b) ? (a) : (b))
#define ARRAYLEN(a)     (sizeof(a) / sizeof((a)[0]))
#define TIME_AFTER(a, b)  ((long)(b) - (long)(a) < 0)
#define TIME_BEFORE(a, b) TIME_AFTER(b, a)
#define DEBUGF(...)     do { } while (0)
#define logf(...)       do { } while (0)

/* -- run addresses (modload.c's host hooks) -------------------------------- */
#define MODHOST_RUN_BASE 0x10000000u
extern uint8_t *modhost_arena_base;     /* the core_alloc block's data */
#define MOD_RUN_ADDR(p) \
    ((uint32_t)((const uint8_t *)(p) - modhost_arena_base) + MODHOST_RUN_BASE)
const void *modhost_ptr(uint32_t run_addr);
#define MOD_EXPORT_AT(base, slot) \
    modhost_ptr(((const uint32_t *)(const void *)(base))[slot])

/* -- kernel.h / system.h --------------------------------------------------- */
long modhost_tick(void);           /* every read moves time on: no
                                       wait loop can spin forever */
#define current_tick (modhost_tick())
struct semaphore { int count; };
struct thread_debug_info { unsigned stack_usage; };
void semaphore_init(struct semaphore *s, int max, int start);
int  semaphore_wait(struct semaphore *s, int timeout);
void semaphore_release(struct semaphore *s);
void modhost_sleep(int ticks);
#define sleep(t) modhost_sleep(t)
unsigned int thread_self(void);
int thread_get_debug_info(unsigned int id, struct thread_debug_info *info);
void commit_discard_idcache(void);
void cpu_boost(bool on);

/* -- core_alloc.h ------------------------------------------------------------ */
int   core_alloc(size_t size);
void *core_get_data(int handle);
bool  core_pin(int handle);

/* -- storage.h ----------------------------------------------------------------- */
bool storage_post_job(void (*fn)(void));
bool storage_disk_is_active(void);

/* -- file.h: the pack is a host file; ROCKBOX_DIR maps to a test folder ------- */
int   modhost_open(const char *path, int flags);
#define open(p, f, ...) modhost_open(p, f)

/* -- lcd.h ---------------------------------------------------------------------- */
typedef uint16_t fb_data;
struct frame_buffer_t { void *fb_ptr; };
struct viewport { int x, y, width, height, font, drawmode; };
#define DRMODE_SOLID    3
#define DRMODE_FG       2
#define DRMODE_BG       1
#define DRMODE_COMPLEMENT 0
#define DRMODE_INVERSEVID 4
void lcd_set_viewport(struct viewport *vp);
void lcd_set_drawmode(int mode);
void lcd_set_foreground(unsigned c);
void lcd_fillrect(int x, int y, int w, int h);
void lcd_update(void);
void lcd_update_rect(int x, int y, int w, int h);

/* -- lang.h / settings.h ---------------------------------------------------------- */
enum { LANG_IPODUI_MUSIC_MENU = 1, LANG_IPODUI_MOD_ERR_TITLE,
       LANG_IPODUI_MOD_ERR_HEAD, LANG_IPODUI_MOD_ERR_BODY,
       LANG_IPODUI_MAIN_MENU, LANG_IPODUI_LEGAL, LANG_IPODUI_DATE_TIME };
#define str(id) ((const char *)"")
#define ID2P(id) ((const char *)"")
struct user_settings { char lang_file[32]; };
extern struct user_settings global_settings;
struct tm;
struct tm *get_time(void);

/* xcore.h (P19 S0): one core on the host, nothing to place uncached */
#define XCORE_BSS
#define XCORE_DATA

#endif /* MODHOST_H */
