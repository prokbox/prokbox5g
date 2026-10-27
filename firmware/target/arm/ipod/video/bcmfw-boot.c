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
 * The BCM2722 boot path (HAVE_BCMFW): our VideoCore image in place of
 * Apple's vmcs, its gates (toggle, pass flags, device-verified list), the
 * attempt marker, the strikes and the run-time fallback; with
 * HAVE_BCMFW_L1 also the early swap, the fallback ladder and the bounded
 * bootstrap (docs/research/bcm2722/independence.md, level 1).
 *
 * #included by bcm-devtest-hal.c (which lcd-video.c #includes), because it
 * needs that file's host-port helpers and lcd-video.c's statics.
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
/* ---- The boot path (configure --bcmfw) -------------------------------- *
 *
 * Policy (pipeline.md, "The everyday path"): our image drives the display
 * only when the toggle (/.rockbox/bcmfw.cfg) and a pass flag allow it.
 *   "cpu" / "dma"  opt-in: the stage 2 / stage 3 (or stage 5) pass flag
 *                  must name an embedded image whose size and FNV-1a
 *                  still match
 *   no file        the build's default: off, or with HAVE_BCMFW_DEFAULT
 *                  (configure --bcmfw-default) the DMA image, if a stage 5
 *                  pass flag names it or the build lists its FNV-1a as
 *                  device-verified (bcmfw/verified.txt)
 *   "apple"        Apple's NOR vmcs, whatever the build (the developer
 *                  switch)
 * Before the swap, /.rockbox/bcmfw.try is written; only a confirmed first
 * frame removes it, and while it exists no boot swaps (choosing a mode in
 * the debug menu re-arms).  Once active, flash_vmcs_* point at our image,
 * so the stock sleep/wake (lcd_sleep/lcd_awake) reloads it; bcm_init()
 * falls back to Apple's NOR vmcs if it never gets ready or faults,
 * lcd_tick() marks it hung on the fault word or 2 s busy, and the next
 * update then falls back.  Each such fallback is a strike
 * (/.rockbox/bcmfw.strikes, written with the idle flush); at
 * BCMFW_STRIKES no boot swaps until a mode is chosen again, and a boot
 * that runs our image BCMFW_CLEAN_MIN minutes without one clears them.
 * Those later events are kept in RAM and flushed to bcmtest.log when the
 * disk goes idle (no extra spin-up).
 *
 * Without the device test (HAVE_BCMFW alone: the release build) there are
 * no pass flags, no CPU-push image and no debug-menu rows: "dma" or the
 * default (HAVE_BCMFW_DEFAULT) takes the DMA image only if verified.txt
 * lists it, "cpu" is refused, and the ladder is the resident video image,
 * the DMA image, Apple's vmcs.  Everything else above is the same. */

#include "version.h"            /* rbversion: the build ID (re-arm) */

#define BCMFW_CFG  ROCKBOX_DIR "/bcmfw.cfg"
#define BCMFW_TRY  ROCKBOX_DIR "/bcmfw.try"
#define BCMFW_STRIKE_FILE ROCKBOX_DIR "/bcmfw.strikes"
#ifdef BCMDT_LADDER
#define BCMFW_NOTES 8           /* a rung-down drop notes up to 3 */
#else
#define BCMFW_NOTES 4
#endif
#define BCMFW_STRIKES BCMDT_STRIKES /* runtime fallbacks before it stops */
#define BCMFW_CLEAN_MIN 30      /* minutes on our image that clear them */

static const fb_data *bcmfw_nor_offset;
static unsigned bcmfw_nor_length;
/* Events after boot, waiting for the idle flush: a string literal and a
 * tick each.  Noting never formats: bcm_init() reaches it on the
 * backlight thread's 1 KB stack. */
static struct { const char *why; long tick; } bcmfw_notes[BCMFW_NOTES];
static int bcmfw_nnotes;
static bool bcmfw_flushing, bcmfw_registered;
static char bcmfw_state[48] = "not tried this boot";
static const char *bcmfw_last;      /* the latest note, for the status */
static int bcmfw_strikes = -1;      /* on disk at boot; -1 not read */
static bool bcmfw_struck;           /* this boot fell back at run time */
static int bcmfw_real, bcmfw_asked; /* this boot's fallbacks, by kind */
static bool bcmfw_by_default;       /* no toggle file: the build's default */
#ifdef HAVE_BCMFW_L1
/* Level 1 (independence.md); bcmfw_rung_next is in bcm-devtest-hal.c */
static bool bcmfw_early_done;       /* the swap ran (or was skipped) */
static int bcmfw_inject_boot;       /* debug: the next n bootstraps fail */
#define BCMFW_INJECT ROCKBOX_DIR "/bcmfw.inject"
#endif
#ifdef HAVE_BCMFW_RESIDENT
/* p14-resident: the video image as the display driver (bcmfw_res_img and
 * bcmfw_res_idle are in bcm-devtest-hal.c).  The setting: no file = on,
 * idle sleep off.  The failure marker: the resident rung failed at a
 * boot; it is skipped until the setting is chosen again. */
#define BCMFW_RES_CFG  ROCKBOX_DIR "/bcmres.cfg"
#define BCMFW_RES_FAIL ROCKBOX_DIR "/bcmres.fail"
/* bcmfw/config.h: the gate of a clock build's idle sleep, its counters */
#define BCMA_IDLE_EN        0x16C
#define BCMA_IDLE_EN_MAGIC  0x534C5031  /* "SLP1" */
#define BCMA_IDLE_TICKS     0x1D4
#define BCMA_IDLE_BELLS     0x1D8
#define BCMA_IDLE_SLEEPS    0x1DC
#define BCMA_IDLE_BAD       0x1FC
/* p19-battery, lever PW3 (brief-map-power.md section 4): the core's
 * regulator at Apple's level for 27 MHz instead of the 11 a host start
 * leaves.  Bits [7:4] are always kept; never below level 5; only while
 * the core runs from the crystal (the clock word's source field 0) */
#define BCMA_REGULATOR      0x10001400
#define BCMA_CLK_CORE       0x10001C28
#define BCMA_PW(v)          (0xA5A50000u | (v))
#define BCMFW_LEVEL_LOW     5           /* Apple's table up to 46.9 MHz */
static bool bcmfw_res_level5;       /* the setting, read at the offer */
static int8_t bcmfw_res_level_rb = -1;  /* the level read back last; -1 */
static bool bcmfw_res_offered;      /* bcmres.c handed the image over */
static uint32_t bcmfw_res_read_us;  /* its read from the disk */
static const char *bcmfw_res_why = "no image offered";
static uint32_t bcmfw_swap_us;      /* this boot's swap, all rungs */
#endif

static int bcmfw_read_strikes(void)
{
    char c[8];

    dt_read(BCMFW_STRIKE_FILE, c, sizeof(c));
    return c[0] >= '0' && c[0] <= '9' ? c[0] - '0' : 0;
}

static void bcmfw_set_state(const char *s)
{
    strlcpy(bcmfw_state, s, sizeof(bcmfw_state));
    bcmfw_last = NULL;
}

/* Thread context only (never the tick); why must be a string literal */
static void bcmfw_note(const char *why)
{
    if (bcmfw_nnotes < BCMFW_NOTES)
    {
        bcmfw_notes[bcmfw_nnotes].why = why;
        bcmfw_notes[bcmfw_nnotes++].tick = current_tick;
    }
    bcmfw_last = why;
}

/* Storage thread (2 KB stack), disk about to sleep, and at power-off:
 * write the waiting lines.  A line noted meanwhile goes out next time. */
static void bcmfw_idle_flush(unsigned short id, void *data, void *user)
{
    char line[96];
    int n = bcmfw_nnotes, k;

    (void)id;
    (void)data;
    (void)user;
    if (bcmfw_flushing)
        return;
    if (n == 0)
    {
        /* a long clean run on our image clears the strikes */
        if (bcmfw_active && !bcmfw_struck && bcmfw_strikes > 0 &&
            current_tick > BCMFW_CLEAN_MIN * 60 * HZ)
        {
            bcmfw_flushing = true;
            remove(BCMFW_STRIKE_FILE);
            bcmfw_strikes = 0;
            dt_log("boot path: strikes cleared after a clean run");
            bcmfw_flushing = false;
        }
        return;
    }
    bcmfw_flushing = true;
    if (bcmfw_struck && bcmfw_strikes >= 0 &&
        bcmdt_strike_next(bcmfw_strikes, bcmfw_real, bcmfw_asked) > 0)
    {
        char c[4] = { (char)('0' + bcmdt_strike_next(bcmfw_strikes, bcmfw_real,
                                                     bcmfw_asked)), '\n', 0, 0 };

        /* one strike per boot, however many notes it had */
        if (dt_write(BCMFW_STRIKE_FILE, c))
        {
            snprintf(line, sizeof(line), "boot path: strike %d of %d",
                     bcmfw_strikes + 1, BCMFW_STRIKES);
            dt_log(line);
        }
        bcmfw_strikes = -1;     /* written: not again this boot */
    }
    for (k = 0; k < n; k++)
    {
#ifdef BCMDT_LADDER
        /* a fallback may be one rung down, not Apple's */
        snprintf(line, sizeof(line), "%6lu boot path: fell back: %s",
                 (unsigned long)(bcmfw_notes[k].tick / HZ), bcmfw_notes[k].why);
#else
        snprintf(line, sizeof(line), "%6lu boot path: fell back to Apple's "
                 "vmcs: %s", (unsigned long)(bcmfw_notes[k].tick / HZ),
                 bcmfw_notes[k].why);
#endif
        dt_log(line);
    }
    memmove(&bcmfw_notes[0], &bcmfw_notes[n],
            (bcmfw_nnotes - n) * sizeof(bcmfw_notes[0]));
    bcmfw_nnotes -= n;
    bcmfw_flushing = false;
}

/* Our image is no longer the one to load: Apple's NOR vmcs again, for
 * the rest of this boot.  No formatting, no I/O (backlight thread). */
static void bcmfw_drop(const char *why)
{
    bcmfw_hung = false;
    if (!bcmfw_active)
        return;
    bcmfw_active = NULL;
    flash_vmcs_offset = bcmfw_nor_offset;
    flash_vmcs_length = bcmfw_nor_length;
#ifdef BCMDT_LADDER
    if (bcmfw_rung_next)
    {
        /* one rung down: our DMA image below the resident video image,
         * our CPU-push image below that (level 1), then Apple's vmcs */
        bcmfw_active = bcmfw_rung_next;
        bcmfw_rung_next = bcmfw_rung_after;
        bcmfw_rung_after = NULL;
        flash_vmcs_offset = (const fb_data *)bcmfw_active->data;
        flash_vmcs_length = ((bcmfw_active->size + 3) >> 1) & ~1;
#ifdef HAVE_BCMFW_DEVTEST
        bcmfw_note(bcmfw_active == &bcmdt_hello_image ||
                   bcmfw_active == &bcmdt_hello_rb_image ?
                   "next rung, our CPU-push image" :
                   "next rung, our DMA image");
#else
        bcmfw_note("next rung, our DMA image");    /* no CPU-push rung */
#endif
    }
#endif
    bcmfw_note(why);
    /* asked for (test fallback, the device test): the same path, no strike */
    if (bcmfw_deliberate)
    {
        bcmfw_asked++;
        bcmfw_note("test fallback: no strike");
    }
    else
    {
        bcmfw_real++;
        bcmfw_struck = true;
    }
    bcmfw_deliberate = false;
}

/* Thread context: drop our image and, if it is running, power-cycle the
 * BCM onto Apple's vmcs through the stock wake path */
static void bcmfw_fallback(const char *why)
{
    bool on = lcd_state.display_on;

    bcmfw_drop(why);
    if (!on)
        return;                 /* the next wake loads Apple's */
#ifdef HAVE_PP_GOV_DVS
    pgov_port_cap(true);        /* off 100 MHz: this owner drives the port
                                 * without lcd_awake() (review S4) */
#endif
    bcm_devtest_owned = true;   /* lcd_awake()/lcd_sleep() step aside */
    lcd_state.display_on = false;
    tick_remove_task(&lcd_tick);
    _backlight_hw_enable(false);
    GPO32_VAL &= ~0x4000;       /* no commands to an image that stopped */
    sleep(HZ/10);
    lcd_state.update_timeout = current_tick - HZ;
    lcd_awake_body();
    bcm_devtest_owned = false;
    dt_light(true);
}

/* The toggle; *by_default set when no file chose it (the build's own) */
static enum bcmfw_mode bcmfw_mode_of(bool *by_default)
{
    char c[8];

    *by_default = false;
    dt_read(BCMFW_CFG, c, sizeof(c));
    if (!strncmp(c, "cpu", 3))
        return BCMFW_MODE_CPU;
    if (!strncmp(c, "dma", 3))
        return BCMFW_MODE_DMA;
    if (!strncmp(c, "apple", 5))
        return BCMFW_MODE_APPLE;
    *by_default = true;
#ifdef HAVE_BCMFW_DEFAULT
    return BCMFW_MODE_DMA;
#else
    return BCMFW_MODE_OFF;
#endif
}

#ifdef HAVE_BCMFW_DEVTEST
/* ---- The debug menu's rows (apps/ipodui/devbcm.c) ---------------------- */
enum bcmfw_mode bcm_boot_get_mode(void)
{
    bool d;

    return bcmfw_mode_of(&d);
}

static const char *bcmfw_mode_name(enum bcmfw_mode mode)
{
    return mode == BCMFW_MODE_CPU ? "CPU push" : mode == BCMFW_MODE_DMA ? "DMA"
         : mode == BCMFW_MODE_APPLE ? "Apple vmcs (forced)" : "off";
}

void bcm_boot_set_mode(enum bcmfw_mode mode)
{
    char line[80];

    if (mode == BCMFW_MODE_OFF)
        remove(BCMFW_CFG);      /* the build's default */
    else
        dt_write(BCMFW_CFG, mode == BCMFW_MODE_CPU ? "cpu\n" :
                            mode == BCMFW_MODE_DMA ? "dma\n" : "apple\n");
    remove(BCMFW_TRY);
    remove(BCMFW_STRIKE_FILE);
    snprintf(line, sizeof(line), "boot path toggle: %s (attempt marker and "
             "strikes cleared)", mode == BCMFW_MODE_OFF ? "build default"
                                                        : bcmfw_mode_name(mode));
    dt_log(line);
}

static const struct bcmdt_image *const bcmfw_cpu_images[2] =
    { &bcmdt_hello_image, &bcmdt_hello_rb_image };
static const struct bcmdt_image *const bcmfw_dma_images[2] =
    { &bcmdt_hello_dma_image, &bcmdt_hello_dma_rb_image };
#endif /* HAVE_BCMFW_DEVTEST */

/* Is this embedded image one the build lists as device-verified? */
static bool bcmfw_verified(const struct bcmdt_image *img)
{
    int k;

    for (k = 0; k < bcmdt_n_verified; k++)
        if (bcmdt_verified_fnv[k] == img->fnv &&
            bcmdt_fnv(img->data, img->size) == img->fnv)
            return true;
    return false;
}

#ifdef HAVE_BCMFW_RESIDENT
/* P19 W0-6 (memory-contract.md section 6, item 4): the resident image's
 * FNV-1a in verified.txt too, as the embedded images'; the default path's
 * job rung needs it.  Checked once a boot (the image is ~210 KB). */
static int8_t bcmfw_res_ver = -1;
static bool bcmfw_res_verified(void)
{
    if (bcmfw_res_ver < 0 && bcmfw_res_offered)
        bcmfw_res_ver = bcmfw_verified(&bcmfw_res_img);
    return bcmfw_res_ver > 0;
}

/* p19-battery: the regulator of the resident image's core to `level`
 * (5..15; bits [7:4] kept), only while the core runs from the crystal:
 * a video session's clock_fast() (bcmfw/clock.c) raises it to 15 before
 * its PLL and clock_slow() puts back what it found, so the image keeps
 * a level set here across a session.  A power cut resets it (a host
 * start finds 11, [silicon] S7, PW1), so every wake applies it again.
 * Returns the level read back, -1 if refused or unread.  No formatting:
 * the wake reaches it on the backlight thread's small stack. */
static int bcmfw_res_level(int (*rd)(uint32_t, uint32_t *),
                           int (*wr)(uint32_t, uint32_t), int level)
{
    uint32_t clk, r;

    if (level < BCMFW_LEVEL_LOW || level > 15)
        return -1;
    if (rd(BCMA_CLK_CORE, &clk) < 0 || (clk & ~0xFFFFu) || (clk & 0x700))
        return -1;              /* unread, or not on the crystal */
    if (rd(BCMA_REGULATOR, &r) < 0 || (r & ~0xFFu))
        return -1;              /* a dead port reads 0xFFFF */
    if ((int)(r & 0xF) != level &&
        (wr(BCMA_REGULATOR, BCMA_PW((r & 0xF0) | (uint32_t)level)) < 0 ||
         rd(BCMA_REGULATOR, &r) < 0))
        return -1;
    bcmfw_res_level_rb = (int8_t)(r & 0xF);
    return (int)(r & 0xF);
}

/* bcm_read32()/bcm_write32() as the level's port (the wake path) */
static int bcmfw_rd_port(uint32_t addr, uint32_t *val)
{
    *val = bcm_read32(addr);
    return 0;
}

static int bcmfw_wr_port(uint32_t addr, uint32_t val)
{
    bcm_write32(addr, val);
    return 0;
}
#endif

#ifdef HAVE_BCMFW_DEVTEST
/* A pass flag of this stage that names one of imgs[2], or NULL */
static const struct bcmdt_image *bcmfw_flag(const char *path, int stage,
                                            const struct bcmdt_image *const *imgs,
                                            const char **why)
{
    char text[48];

    dt_read(path, text, sizeof(text));
    if (!text[0])
    {
        *why = "no pass flag";
        return NULL;
    }
    return bcmdt_flag_match(text, stage, imgs, 2, why);
}
#endif

/* The image this mode allows, or NULL with *why.  Opt-in DMA takes a
 * stage 5 flag or, as before, a stage 3 one; the default takes a stage 5
 * flag or a device-verified image only. */
static const struct bcmdt_image *bcmfw_allowed(enum bcmfw_mode mode,
                                               bool by_default, const char **why)
{
#ifdef HAVE_BCMFW_DEVTEST
    const struct bcmdt_image *img;

    if (mode == BCMFW_MODE_CPU)
        return bcmfw_flag(DT_PASS2, 2, bcmfw_cpu_images, why);
#endif
    if (mode != BCMFW_MODE_DMA)
    {
#ifdef HAVE_BCMFW_DEVTEST
        *why = "toggle off";
#else
        *why = mode == BCMFW_MODE_CPU ? "CPU push: not in this build"
                                      : "toggle off";
#endif
        return NULL;
    }
#ifdef HAVE_BCMFW_DEVTEST
    if ((img = bcmfw_flag(DT_PASS5, 5, bcmfw_dma_images, why)))
        return img;
    if (!by_default)
        return bcmfw_flag(DT_PASS3, 3, bcmfw_dma_images, why);
#else
    (void)by_default;           /* no pass flags: verified.txt alone */
#endif
    if (bcmfw_verified(&bcmdt_hello_dma_image))
    {
        *why = "device-verified build";
        return &bcmdt_hello_dma_image;
    }
#ifdef HAVE_BCMFW_DEVTEST
    *why = "no stage 5 pass flag, image not device-verified";
#else
    *why = "image not device-verified";
#endif
    return NULL;
}

#ifdef HAVE_BCMFW_L1
/* The debug menu's injected failures: the HAL's bootstrap of the next n
 * rungs reports a handshake that never answered (the bounded wait's own
 * error), after the real power cut, so the ladder runs as it would */
static int dt_boot_inject(const uint8_t *img, uint32_t bytes)
{
    if (bcmfw_inject_boot > 0)
    {
        bcmfw_inject_boot--;
        GPO32_VAL |= 0x4000;
        sleep(HZ/20 + HZ/50);   /* the power-up, then the 20 ms wait */
        return -1;
    }
    return dt_boot(img, bytes);
}

#ifdef HAVE_BCMFW_DEVTEST
/* The CPU-push rung: a stage 2 pass flag naming it, or device-verified */
static const struct bcmdt_image *bcmfw_cpu_rung(void)
{
    const char *why;
    const struct bcmdt_image *img = bcmfw_flag(DT_PASS2, 2, bcmfw_cpu_images,
                                               &why);

    if (img)
        return img;
    return bcmfw_verified(&bcmdt_hello_image) ? &bcmdt_hello_image : NULL;
}
#endif

#endif /* HAVE_BCMFW_L1 */

#ifdef HAVE_BCMFW_RESIDENT
/* The resident rung's gates: the setting, bcmres.c's offer, no failure
 * at an earlier boot, and without the device test (a release build) the
 * image's FNV-1a in verified.txt: an unverified image leaves the DMA image
 * and Apple's vmcs.  NULL with bcmfw_res_why set otherwise. */
static const struct bcmdt_image *bcmfw_res_rung(void)
{
    bool idle, setting = bcm_boot_resident_setting(&idle);
    bool failed = setting && bcmfw_res_offered && file_exists(BCMFW_RES_FAIL);
#ifdef HAVE_BCMFW_DEVTEST
    const bool need_verified = false;   /* the bench verifies it */
#else
    const bool need_verified = true;
#endif
    const char *why = bcmdt_res_gate(setting, bcmfw_res_offered, failed,
                                     need_verified,
                                     need_verified && setting &&
                                     bcmfw_res_offered && !failed &&
                                     bcmfw_res_verified());

    if (why)
    {
        bcmfw_res_why = why;
        return NULL;
    }
    bcmfw_res_why = "tried";
    return &bcmfw_res_img;
}
#endif

#ifdef BCMDT_LADDER
/* The resident video image (p14-resident), our DMA image, our CPU-push
 * image (level 1); the winner, or NULL (the BCM is off: the caller
 * restores Apple's vmcs) */
static const struct bcmdt_image *bcmfw_ladder(const struct bcmdt_image *img,
                                              struct bcmdt_result *r)
{
    struct bcmdt_hal hal = dt_hal;
    const struct bcmdt_image *rungs[3] = { NULL, NULL, NULL };
    bool injected = false;
    char line[96];
    int n = 0, ours, k;

#ifdef HAVE_BCMFW_RESIDENT
    if ((rungs[n] = bcmfw_res_rung()))
        n++;
#endif
    ours = n;                   /* the rung the mode chose */
    rungs[n++] = img;
#ifdef HAVE_BCMFW_L1
#ifdef HAVE_BCMFW_DEVTEST
    if (img != &bcmdt_hello_image && img != &bcmdt_hello_rb_image)
        rungs[n] = bcmfw_cpu_rung();
#endif
    injected = bcmfw_inject_boot > 0;
    if (injected)
    {
        hal.boot = dt_boot_inject;
        snprintf(line, sizeof(line), "boot path: debug: the next %d "
                 "bootstrap(s) fail", bcmfw_inject_boot);
        dt_log(line);
    }
#endif
    k = bcmdt_boot_ladder(&hal, rungs, 3, r);
#ifdef HAVE_BCMFW_L1
    bcmfw_inject_boot = 0;
#endif
    /* the rungs below the winner, for a run-time fallback */
    bcmfw_rung_next = bcmfw_rung_after = NULL;
    if (k >= 0 && k + 1 < 3)
        bcmfw_rung_next = rungs[k + 1];
    if (k >= 0 && k + 2 < 3)
        bcmfw_rung_after = rungs[k + 2];
    if (!bcmfw_rung_next)
    {
        bcmfw_rung_next = bcmfw_rung_after;
        bcmfw_rung_after = NULL;
    }
#ifdef HAVE_BCMFW_RESIDENT
    if (ours && k != 0 && !injected)
    {
        /* the resident rung failed: the boot image this boot, and no
         * resident rung at later boots until the setting is chosen again
         * (never a strike: the boot path itself still works) */
        bcmfw_res_why = "failed at this boot";
        if (k > 0)
            dt_write(BCMFW_RES_FAIL, "failed\n");
    }
#endif
    if (k <= 0)
        return k == 0 ? rungs[0] : NULL;
    snprintf(line, sizeof(line), "boot path: active: %s (fallback rung); "
             "%s failed", rungs[k]->name, rungs[0]->name);
    dt_log(line);
    if (!injected && k > ours)
    {
        /* our boot image failed (the CPU-push rung won): a strike,
         * written with the idle flush */
        bcmfw_real++;
        bcmfw_struck = true;
        bcmfw_note("boot: our DMA image failed; CPU-push rung");
    }
    return rungs[k];
}
#endif /* BCMDT_LADDER */

#ifdef HAVE_BCMFW_L1
static void bcmfw_read_inject(void)
{
    char c[16];

    dt_read(BCMFW_INJECT, c, sizeof(c));
    if (!c[0])
        return;
    remove(BCMFW_INJECT);       /* one-shot */
    bcmfw_inject_boot = !strncmp(c, "swap-all", 8) ? 3 :
                        !strncmp(c, "swap", 4) ? 1 : 0;
}

void bcm_boot_early(bool escape)
{
    char line[80];

    dt_log_roll();          /* before this boot's first line */

    if (!bcmfw_registered)
        bcmfw_registered = add_event_ex(DISK_EVENT_SPINUP, false,
                                        bcmfw_idle_flush, NULL);
    snprintf(line, sizeof(line), "boot: BCM %s at entry",
             bcm_entry_powered ? "powered (the ROM's vmcs)"
                               : "unpowered (ROLO: Apple's vmcs loaded)");
    dt_log(line);
    if (escape)
    {
        /* the Developer list draws on Apple's vmcs; the shell's call
         * swaps afterwards, with whatever mode was chosen there (a sealed
         * build has no escape) */
#ifdef HAVE_IPOD_UI_DEVMODE
        dt_log("boot path: early swap skipped: the Developer escape is held");
#endif
        return;
    }
    bcmfw_read_inject();
    bcm_boot_switch();
}

void bcm_boot_inject(enum bcmfw_inject what)
{
    if (what == BCMFW_INJECT_WAKE)
    {
        if (bcmfw_active)
        {
            dt_log("boot path: debug: the next wake's handshake never answers");
            bcmfw_deliberate = true;
            bcmfw_inject_wake = true;
        }
        return;
    }
    dt_write(BCMFW_INJECT, what == BCMFW_INJECT_SWAP_ALL ? "swap-all\n"
                                                         : "swap\n");
    dt_log(what == BCMFW_INJECT_SWAP_ALL ?
           "boot path: debug: the next boot's every rung fails" :
           "boot path: debug: the next boot's DMA rung fails");
}
#endif

#ifndef HAVE_BCMFW_DEVTEST
#define BCMFW_BUILD ROCKBOX_DIR "/bcmfw.build"
/* A build without the device test has no debug menu to re-arm the swap
 * (choosing a mode clears the marker and the strikes): the first boot of
 * a new build does it once.  The build ID is written first, so a build
 * whose swap brings the boot down never clears them again; if it can't be
 * written nothing is cleared.  bcm_boot_switch()'s context: the disk just
 * mounted, before any USB session, as the attempt marker's own write. */
static void bcmfw_rearm(void)
{
    char c[64];                 /* rbversion is ~20 characters */

    dt_read(BCMFW_BUILD, c, sizeof(c));
    if (!bcmdt_rearm_due(c[0] ? c : NULL, rbversion))
        return;
    if (!dt_write(BCMFW_BUILD, rbversion))
    {
        dt_log("boot path: new build, not re-armed: build ID not written");
        return;
    }
    remove(BCMFW_TRY);
    remove(BCMFW_STRIKE_FILE);
    dt_log("boot path: new build: attempt marker and strikes cleared");
}
#endif

void bcm_boot_switch(void)
{
    struct bcmdt_result r;
    enum bcmfw_mode mode = bcmfw_mode_of(&bcmfw_by_default);
    const struct bcmdt_image *img;
    const char *why;
    char line[96];
#ifdef HAVE_BCMFW_RESIDENT
    uint32_t t0;
#endif

#ifdef HAVE_BCMFW_L1
    if (bcmfw_early_done)
        return;                 /* level 1: ran right after the disk mounted */
    bcmfw_early_done = true;
#endif
    if (mode == BCMFW_MODE_OFF || mode == BCMFW_MODE_APPLE)
    {
        bcmfw_set_state(mode == BCMFW_MODE_OFF ? "toggle off"
                                               : "Apple vmcs (forced)");
        return;                 /* no write, no log */
    }
#ifndef HAVE_BCMFW_DEVTEST
    bcmfw_rearm();
#endif
    bcmfw_strikes = bcmfw_read_strikes();
    if (bcmdt_strike_blocks(bcmfw_strikes))
    {
        snprintf(line, sizeof(line), "skipped: %d run-time fallbacks",
                 bcmfw_strikes);
        bcmfw_set_state(line);
#ifdef HAVE_BCMFW_DEVTEST
        dt_log("boot path: skipped: too many run-time fallbacks "
               "(bcmfw.strikes); choose a mode in the debug menu to re-arm");
#else
        dt_log("boot path: skipped: too many run-time fallbacks "
               "(bcmfw.strikes); a new build re-arms");
#endif
        return;
    }
    if (file_exists(BCMFW_TRY))
    {
        bcmfw_set_state("skipped: attempt marker present");
#ifdef HAVE_BCMFW_DEVTEST
        dt_log("boot path: skipped: the last attempt did not finish "
               "(bcmfw.try); choose a mode in the debug menu to re-arm");
#else
        dt_log("boot path: skipped: the last attempt did not finish "
               "(bcmfw.try); a new build re-arms");
#endif
        return;
    }
    if (!(img = bcmfw_allowed(mode, bcmfw_by_default, &why)))
    {
        snprintf(line, sizeof(line), "boot path: not switched: %s%s",
                 bcmfw_by_default ? "default, " : "", why);
        dt_log(line);
        bcmfw_set_state(line + 11);
        return;
    }
    /* the guard first: on disk before anything touches the BCM */
    if (!dt_write(BCMFW_TRY, img->name))
    {
        bcmfw_set_state("not switched: marker not written");
        dt_log("boot path: not switched: attempt marker not written");
        return;
    }
    if (dt_take() < 0)
    {
        remove(BCMFW_TRY);
        bcmfw_set_state("not switched: display busy");
        dt_log("boot path: not switched: display busy");
        return;
    }
#ifdef HAVE_BCMFW_RESIDENT
    t0 = USEC_TIMER;
#endif
#ifdef BCMDT_LADDER
    if (!(img = bcmfw_ladder(img, &r)))
#else
    if (bcmdt_boot_swap(&dt_hal, img, &r) < 0)
#endif
    {
        dt_log("boot path: restore: Apple vmcs (stock wake path)");
        dt_restore(false);      /* flash_vmcs_* still point at the NOR */
        snprintf(line, sizeof(line), "boot path: fell back to Apple's vmcs "
                 "(%s); marker kept", r.why);
        dt_log(line);
        bcmfw_set_state("fell back at boot; marker kept");
        return;
    }
    /* our image showed the frame: it is the one every wake loads now */
    bcmfw_nor_offset = flash_vmcs_offset;
    bcmfw_nor_length = flash_vmcs_length;
    flash_vmcs_offset = (const fb_data *)img->data;
    flash_vmcs_length = ((img->size + 3) >> 1) & ~1;
    bcmfw_hung = false;
    bcmfw_kick_tick = current_tick;
    bcmfw_active = img;
#ifdef HAVE_BCMFW_RESIDENT
    if (img == &bcmfw_res_img)
    {
        bcmfw_res_why = "running";
        if (bcmfw_res_idle)
            dt_wr(BCMA_IDLE_EN, BCMA_IDLE_EN_MAGIC);
        if (bcmfw_res_level5)
            bcmfw_res_level(dt_rd, dt_wr, BCMFW_LEVEL_LOW);
    }
    bcmfw_swap_us = USEC_TIMER - t0;
#endif
    dt_restore(true);
    remove(BCMFW_TRY);
    if (!bcmfw_registered)
        bcmfw_registered = add_event_ex(DISK_EVENT_SPINUP, false,
                                        bcmfw_idle_flush, NULL);
    snprintf(line, sizeof(line), "boot path: active: %s (%s), first frame "
             "%lu us; marker cleared", img->name, bcmfw_by_default ? "default"
             : "toggle", (unsigned long)r.first_us);
    dt_log(line);
#ifdef HAVE_BCMFW_RESIDENT
    /* p14-resident: what the boot pays for it (the A/B's numbers) */
    snprintf(line, sizeof(line), "boot path: swap %lu ms (every rung "
             "tried), image read %lu ms, at %lu ms since start; idle "
             "sleep %s", (unsigned long)bcmfw_swap_us / 1000,
             (unsigned long)bcmfw_res_read_us / 1000,
             (unsigned long)current_tick * 1000 / HZ,
             img == &bcmfw_res_img && bcmfw_res_idle ? "on" : "off");
    dt_log(line);
    if (img == &bcmfw_res_img)
    {
        /* p19-battery: the PW3 lever; -1: unread or refused */
        snprintf(line, sizeof(line), "boot path: resident core level %d "
                 "(level 5 setting %s)", bcmfw_res_level_rb,
                 bcmfw_res_level5 ? "on" : "off");
        dt_log(line);
    }
#endif
    snprintf(line, sizeof(line), "active: %s", img->name);
    bcmfw_set_state(line);
}

#ifdef HAVE_BCMFW_RESIDENT
bool bcm_boot_resident_setting(bool *idle)
{
    char c[40];

    dt_read(BCMFW_RES_CFG, c, sizeof(c));
    *idle = strstr(c, "idle on") != NULL;
    return strstr(c, "resident off") == NULL;
}

/* The file holds all three lines; each setter keeps the others */
static void bcmfw_res_write(bool on, bool idle, bool level5)
{
    char line[96];

    snprintf(line, sizeof(line), "resident %s\nidle %s\n%s",
             on ? "on" : "off", idle ? "on" : "off",
             level5 ? "level 5\n" : "");
    dt_write(BCMFW_RES_CFG, line);
    remove(BCMFW_RES_FAIL);     /* re-armed */
    snprintf(line, sizeof(line), "boot path: resident video image %s, idle "
             "sleep %s, core level %s (next boot; failure marker cleared)",
             on ? "on" : "off", idle ? "on" : "off", level5 ? "5" : "as found");
    dt_log(line);
}

void bcm_boot_resident_set(bool on, bool idle)
{
    bcmfw_res_write(on, idle, bcm_boot_level5_setting());
}

/* p19-battery (PW3): "level 5" in the same file; no line: off */
bool bcm_boot_level5_setting(void)
{
    char c[40];

    dt_read(BCMFW_RES_CFG, c, sizeof(c));
    return strstr(c, "level 5") != NULL;
}

void bcm_boot_level5_set(bool on)
{
    bool idle, res = bcm_boot_resident_setting(&idle);

    bcmfw_res_write(res, idle, on);
}

void bcm_boot_offer_resident(const uint8_t *data, uint32_t bytes,
                             uint32_t fnv, uint32_t read_us)
{
    bool idle;

    if (bcmfw_active)
        return;                 /* too late: the swap ran */
    bcm_boot_resident_setting(&idle);
    bcmfw_res_img.data = data;
    bcmfw_res_img.size = bytes;
    bcmfw_res_img.fnv = fnv;
    bcmfw_res_img.name = "bcmvideo";
    bcmfw_res_idle = idle;
    bcmfw_res_level5 = bcm_boot_level5_setting();
    bcmfw_res_read_us = read_us;
    bcmfw_res_offered = true;
}

bool bcm_boot_resident_active(void)
{
    return bcmfw_active && bcmfw_active == &bcmfw_res_img;
}

/* The resident image's FNV-1a is in verified.txt (Developer > Status) */
bool bcm_boot_resident_verified(void)
{
    return bcmfw_res_verified();
}

#if defined(HAVE_BCMFW_DEVTEST) || defined(HAVE_BCMFW_VIDEO)
/* The idle counters (ticks, doorbells, sleeps, bad) through rd; 1 if all
 * four were read (the status rows and the player's take) */
static int bcmfw_idle_read(int (*rd)(uint32_t, uint32_t *), uint32_t c[4])
{
    static const uint32_t at[4] = { BCMA_IDLE_TICKS, BCMA_IDLE_BELLS,
                                    BCMA_IDLE_SLEEPS, BCMA_IDLE_BAD };
    int k;

    for (k = 0; k < 4; k++)
        if (rd(at[k], &c[k]) < 0)
            return 0;
    return 1;
}
#endif

#ifdef HAVE_BCMFW_DEVTEST    /* bcm_boot_status() only */
/* A read while the LCD driver drives the image: the tick's own reads must
 * not land between the address and the data (thread context; the BCM is
 * powered while the display is on) */
static int bcmfw_rd_live(uint32_t addr, uint32_t *val)
{
    int old = disable_irq_save();

    *val = bcm_read32(addr);
    restore_irq(old);
    return 0;
}
#endif

/* lcd-video.c, after a wake loaded the resident image: idle sleep on
 * again (the upload cleared the gate) */
static void bcmfw_woke(void)
{
    if (bcmfw_active != &bcmfw_res_img)
        return;
    if (bcmfw_res_idle)
        bcm_write32(BCMA_IDLE_EN, BCMA_IDLE_EN_MAGIC);
    if (bcmfw_res_level5)       /* the cut reset it to 11 */
        bcmfw_res_level(bcmfw_rd_port, bcmfw_wr_port, BCMFW_LEVEL_LOW);
}

/* ---- p19-battery: the bench's A/B, live ----------------------------------
 * The port while the LCD driver owns it: under its lock (no push or job
 * in flight), each access with the tick's reads held off (thread context;
 * the BCM is powered while the display is on).  Only while the resident
 * image drives a lit display, outside a video session or a device test. */
static bool bcmfw_res_live_ok(void)
{
    return bcm_boot_resident_active() && lcd_state.display_on &&
           !bcm_devtest_owned && !bcmfw_hung;
}

static int bcmfw_rd_locked(uint32_t addr, uint32_t *val)
{
    int old = disable_irq_save();

    *val = bcm_read32(addr);
    restore_irq(old);
    return 0;
}

static int bcmfw_wr_locked(uint32_t addr, uint32_t val)
{
    int old = disable_irq_save();

    bcm_write32(addr, val);
    restore_irq(old);
    return 0;
}

int bcm_boot_res_live(int idle, int level, int *level_rb)
{
    bool locked;
    int rb = -1;

    if (!bcmfw_res_live_ok())
        return -1;
    locked = lcd_port_lock();
    if (idle >= 0)
    {
        /* RAM only: a wake meanwhile keeps the bench's choice */
        bcmfw_res_idle = idle > 0;
        bcmfw_wr_locked(BCMA_IDLE_EN, idle > 0 ? BCMA_IDLE_EN_MAGIC : 0);
    }
    if (level > 0)
    {
        rb = bcmfw_res_level(bcmfw_rd_locked, bcmfw_wr_locked, level);
        if (rb == level)
            bcmfw_res_level5 = level == BCMFW_LEVEL_LOW;
    }
    lcd_port_unlock(locked);
    if (level_rb)
        *level_rb = rb;
    return level > 0 && rb != level ? -2 : 0;
}

int bcm_boot_res_read(uint32_t c[4], uint32_t *reg)
{
    static const uint32_t at[4] = { BCMA_IDLE_TICKS, BCMA_IDLE_BELLS,
                                    BCMA_IDLE_SLEEPS, BCMA_IDLE_BAD };
    bool locked;
    int k;

    if (!bcmfw_res_live_ok())
        return -1;
    locked = lcd_port_lock();
    for (k = 0; k < 4; k++)
        bcmfw_rd_locked(at[k], &c[k]);
    bcmfw_rd_locked(BCMA_REGULATOR, reg);
    lcd_port_unlock(locked);
    return 0;
}

int bcm_boot_res_bell(void)
{
    bool locked;

    if (!bcmfw_res_live_ok())
        return -1;
    locked = lcd_port_lock();
    dt_doorbell();
    lcd_port_unlock(locked);
    return 0;
}

void bcm_boot_res_now(bool *idle, bool *level5)
{
    *idle = bcmfw_res_idle;
    *level5 = bcmfw_res_level5;
}
#endif

#ifdef HAVE_BCMFW_DEVTEST
void bcm_boot_test_fallback(void)
{
    if (bcmfw_active)
    {
        dt_log("boot path: fallback test requested from the debug menu");
        bcmfw_deliberate = true;
        bcmfw_hung = true;
    }
}

/* the resident lines follow the last numbered one */
#ifdef HAVE_BCMFW_L1
#define BCMFW_ST_RES 10
#else
#define BCMFW_ST_RES 9
#endif

const char *bcm_boot_status(int k, char *buf, int len)
{
    const struct bcmdt_image *img;
    const char *why;
    bool d;
    enum bcmfw_mode mode;
#ifdef HAVE_BCMFW_RESIDENT
    bool res, idle;
    uint32_t c[4];
#endif

    switch (k)
    {
    case 0:
        mode = bcmfw_mode_of(&d);
        snprintf(buf, len, "Toggle: %s%s", bcmfw_mode_name(mode),
                 d ? " (build default)" : "");
        break;
    case 1:
        snprintf(buf, len, "Running: %s", bcmfw_active ? bcmfw_active->name
                                                       : "Apple vmcs");
        break;
    case 2:
        img = bcmfw_flag(DT_PASS2, 2, bcmfw_cpu_images, &why);
        snprintf(buf, len, "Stage 2 flag: %s%s%s", why, img ? ", " : "",
                 img ? img->name : "");
        break;
    case 3:
        img = bcmfw_flag(DT_PASS3, 3, bcmfw_dma_images, &why);
        snprintf(buf, len, "Stage 3 flag: %s%s%s", why, img ? ", " : "",
                 img ? img->name : "");
        break;
    case 4:
        img = bcmfw_flag(DT_PASS5, 5, bcmfw_dma_images, &why);
        snprintf(buf, len, "Stage 5 flag: %s%s%s", why, img ? ", " : "",
                 img ? img->name : "");
        break;
    case 5:
#ifdef HAVE_BCMFW_RESIDENT
        snprintf(buf, len, "Device-verified image: %s; resident %s",
                 bcmfw_verified(&bcmdt_hello_dma_image) ? "yes" : "no",
                 !bcmfw_res_offered ? "none" :
                 bcmfw_res_verified() ? "yes" : "no");
#else
        snprintf(buf, len, "Device-verified image: %s",
                 bcmfw_verified(&bcmdt_hello_dma_image) ? "yes" : "no");
#endif
        break;
    case 6:
        snprintf(buf, len, "Attempt marker: %s; strikes %d of %d",
                 file_exists(BCMFW_TRY) ? "present (no swap)" : "none",
                 bcmfw_read_strikes(), BCMFW_STRIKES);
        break;
    case 7:
        if (bcmfw_last)
            snprintf(buf, len, "This boot: fell back: %s", bcmfw_last);
        else
            snprintf(buf, len, "This boot: %s", bcmfw_state);
        break;
    case 8:
        snprintf(buf, len, "Takes effect at the next boot");
        break;
#ifdef HAVE_BCMFW_L1
    case 9:
        snprintf(buf, len, "Level 1: next rung %s; BCM %s at entry",
                 bcmfw_rung_next ? bcmfw_rung_next->name :
                 bcmfw_active ? "Apple vmcs" : "-",
                 bcm_entry_powered ? "powered" : "unpowered");
        break;
#endif
#ifdef HAVE_BCMFW_RESIDENT
    case BCMFW_ST_RES:
        res = bcm_boot_resident_setting(&idle);
        snprintf(buf, len, "Resident video: %s, idle sleep %s, level %s "
                 "(next boot)", res ? "on" : "off", idle ? "on" : "off",
                 bcm_boot_level5_setting() ? "5" : "as found");
        break;
    case BCMFW_ST_RES + 1:
        snprintf(buf, len, "Resident: %s; read %lu ms, swap %lu ms",
                 bcmfw_res_why, (unsigned long)bcmfw_res_read_us / 1000,
                 (unsigned long)bcmfw_swap_us / 1000);
        break;
    case BCMFW_ST_RES + 2:
        /* live, only while the resident image drives the display */
        if (bcm_boot_resident_active() && lcd_state.display_on &&
            !bcm_devtest_owned && !bcmfw_hung &&
            bcmfw_idle_read(bcmfw_rd_live, c))
            snprintf(buf, len, "Idle: sleeps %lu, ticks %lu, bells %lu, "
                     "bad %lu", (unsigned long)c[2], (unsigned long)c[0],
                     (unsigned long)c[1], (unsigned long)c[3]);
        else
            snprintf(buf, len, "Idle: -");
        break;
#endif
    default:
        return NULL;
    }
    return buf;
}
#endif /* HAVE_BCMFW_DEVTEST */
