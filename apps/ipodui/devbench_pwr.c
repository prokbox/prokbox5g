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

/* The battery probes (devbench_pwr.h): the device side, and the
 * simulator's refusals.  Called from devbench.c's step table on the UI
 * thread (the CPU); each returns an enum bench_res and logs through
 * devbcm_log() (bcmtest.log and the results file).
 *
 * Evidence: every register form here is Apple's (apple-power.md 2.3,
 * [apple-RE]) or Rockbox's (pp5020.h, pcf50605.c, [rockbox-derived]);
 * every meaning tested is [hypothesis] until its row has run.  Every row
 * saves what it changes first (struct pwrp_saved), logs the saved words,
 * puts them back, reads them back, and logs "restored" or the word that
 * didn't come back. */
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include "config.h"
#include "devbench_sim.h"           /* DEVBENCH_SIM in the simulator */

#if defined(HAVE_IPOD_UI_DEVMODE) && !defined(SIMULATOR) && \
    defined(HAVE_BCMFW_DEVTEST) && \
    defined(HAVE_BCMFW_VIDEO)   /* devbcm_log, devbench_ask (devbcm.h) */
#include <time.h>
#include "system.h"
#include "kernel.h"
#include "thread.h"
#include "button.h"
#include "lcd.h"
#include "backlight.h"
#include "audio.h"
#include "playlist.h"
#include "pcm.h"
#include "pcm-internal.h"         /* pcm_is_playing() */
#include "metadata.h"
#include "storage.h"
#include "usb.h"
#include "rtc.h"
#include "crc32.h"
#include "pcf50605.h"
#include "dac_ext.h"
#include "e2-port.h"
#include "devbcm.h"
#include "devbench.h"
#include "devbench_pwr.h"

#ifdef __thumb__
#error "devbench_pwr.c: the self-refresh routine is ARM code"
#endif

static void say(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static void say(const char *fmt, ...)
{
    char line[160];
    va_list ap;

    va_start(ap, fmt);
    vsnprintf(line, sizeof(line), fmt, ap);
    va_end(ap);
    devbcm_log(line);
}

/* A still screen: the title and up to 8 lines */
static void show(const char *title, const char *const *lines, int n)
{
    int j;

    lcd_clear_display();
    lcd_puts(0, 0, title);
    for (j = 0; j < n && j < 8; j++)
        lcd_puts(0, 2 + j, lines[j]);
    lcd_update();
}

/* ---- the restore bookkeeping ------------------------------------------ */

static uint32_t rd(uint32_t addr)
{
    if (addr & 1)
    {
        int v = pcf50605_read((int)(addr >> 8));

        return v < 0 ? 0xFFFFFFFFu : (uint32_t)v;
    }
    return *(volatile uint32_t *)addr;
}

/* Logs the saved words, and "restored" or the first that didn't come
   back.  true: all back */
static bool check_restored(const char *id, const struct pwrp_saved *s)
{
    int i = pwrp_unrestored(s, rd);

    if (i < 0)
    {
        say("%s: restored (%d word%s read back as found)", id, s->n,
            s->n == 1 ? "" : "s");
        return true;
    }
    say("%s: NOT RESTORED: %s 0x%lx was 0x%08lx now 0x%08lx (mask 0x%08lx)",
        id, (s->addr[i] & 1) ? "PMU" : "reg",
        (unsigned long)((s->addr[i] & 1) ? s->addr[i] >> 8 : s->addr[i]),
        (unsigned long)s->was[i], (unsigned long)rd(s->addr[i]),
        (unsigned long)s->mask[i]);
    return false;
}

/* ---- the playback around a row ------------------------------------------ */

static bool music_playing(void)
{
    return (audio_status() & AUDIO_STATUS_PLAY) &&
           !(audio_status() & AUDIO_STATUS_PAUSE);
}

static unsigned long music_elapsed(void)
{
    struct mp3entry *id3 = audio_current_track();

    return id3 ? id3->elapsed : 0;
}

/* Sitting 4gh: in the combined session pldo's codec part may find the
   music stopped or paused by an earlier step; it starts the current
   playlist at its resume track, or resumes a pause (as devbench_awake.c
   and the duty row do).  true: it plays */
static bool music_start(void)
{
    long end;
    int index = 0;

    if (music_playing())
        return true;
    if (audio_status() & AUDIO_STATUS_PAUSE)
        audio_resume();
    else if (playlist_amount() > 0)
    {
        playlist_get_resume_info(&index);
        playlist_start(index < 0 ? 0 : index, 0, 0);
    }
    else
        return false;
    end = current_tick + 5 * HZ;
    while (TIME_BEFORE(current_tick, end))
    {
        if (music_playing() && pcm_is_playing())
            return true;
        sleep(HZ / 10);
    }
    return music_playing();
}

/* Pauses, and waits up to 3 s for the PCM to stop.  true: stopped */
static bool music_hush(void)
{
    long end = current_tick + 3 * HZ;

    if (music_playing())
        audio_pause();
    while (pcm_is_playing() && TIME_BEFORE(current_tick, end))
        sleep(HZ / 20);
    return !pcm_is_playing();
}

/* psrf (sitting 4gh: "the disk or the music started again"): a pause
   keeps the playback engine's buffering alive, so its refill can spin
   the disk inside the row's window.  The row stops the music instead,
   noting where it was, and starts it there again after */
struct music_spot
{
    bool was;
    int index;
    unsigned long elapsed, offset;
};

static bool music_stop_noted(struct music_spot *m)
{
    struct mp3entry *id3 = audio_current_track();
    long end = current_tick + 3 * HZ;

    m->was = (audio_status() & AUDIO_STATUS_PLAY) && id3;
    m->elapsed = m->was ? id3->elapsed : 0;
    m->offset = m->was ? id3->offset : 0;
    m->index = 0;
    playlist_get_resume_info(&m->index);
    audio_stop();
    while (pcm_is_playing() && TIME_BEFORE(current_tick, end))
        sleep(HZ / 20);
    return !pcm_is_playing();
}

static void music_start_noted(const struct music_spot *m)
{
    if (m->was && playlist_amount() > 0)
        playlist_start(m->index < 0 ? 0 : m->index, m->elapsed, m->offset);
}

/* ---- pgrd ----------------------------------------------------------- */

int devbench_pwrp_grade(void)
{
    uint32_t strap = STRAP_OPT_A;

    say("pgrd: STRAP_OPT_A=0x%08lx grade=%u max=%u MHz (Apple's table, "
        "mac-and-dvs.md 3.2; the meaning is [hypothesis]) STRAP_OPT_B="
        "0x%08lx", (unsigned long)strap, pwrp_grade(strap),
        pwrp_grade_mhz(strap), (unsigned long)STRAP_OPT_B);
    return BENCH_DONE;
}

/* ---- preg ----------------------------------------------------------- */

int devbench_pwrp_regs(void)
{
    static const struct { uint32_t a; const char *n; } r[] =
    {
        { 0x70000000, "PP_VER1" },      { 0x70000004, "PP_VER2" },
        { 0x6000600c, "DEV_EN" },       { 0x60006010, "DEV_EN2" },
        { 0x60006044, "DEV_EN3" },      { 0x60006004, "DEV_RS" },
        { 0x60006008, "DEV_RS2" },      { 0x70000010, "DEV_INIT1" },
        { 0x70000014, "pad14" },        { 0x70000018, "pad18" },
        { 0x7000001c, "pad1C" },        { 0x70000020, "DEV_INIT2" },
        { 0x70000034, "DEV_TIMING1" },  { 0x7000003c, "XMB_RAM_CFG" },
        { 0x60006020, "CLOCK_SOURCE" }, { 0x60006034, "PLL_CONTROL" },
        { 0x6000603c, "PLL_STATUS" },   { 0x60006048, "PLL2" },
        { 0x60007000, "CPU_CTL" },      { 0x60007004, "COP_CTL" },
        { 0x60005000, "TIMER1_CFG" },   { 0x60005008, "TIMER2_CFG" },
        { 0x70000028, "USB_PHY28" },
        /* the power catalogue's read-only checks (apple-power-catalog.md
           section 3): T-MLCD's divider, 0x7000_0030 (Apple sets [15:12]
           to 6), T-LOWBATT-PIN's GPIOA bit 3 */
        { 0x6000602c, "MLCD_DIV" },     { 0x70000030, "x7030" },
        { 0x6000d030, "GPIOA_IN" },
    };
    /* the PMU's supplies: D1-D3REGC1, DCDC1, DCDC2, DCUDC1, LPREGC1 */
    static const uint8_t pmu[] = { 0x24, 0x25, 0x26, 0x1b, 0x1c, 0x1e, 0x21 };
    char line[160];
    int i, n = 0;

    for (i = 0; i < (int)ARRAYLEN(r); i++)
    {
        n += snprintf(line + n, sizeof(line) - n, "%s%s=%08lx",
                      n ? " " : "", r[i].n,
                      (unsigned long)*(volatile uint32_t *)r[i].a);
        if (n > (int)sizeof(line) - 30 || i == (int)ARRAYLEN(r) - 1)
        {
            say("preg: %s", line);
            n = 0;
        }
    }
    for (i = 0; i < (int)ARRAYLEN(pmu); i++)
        n += snprintf(line + n, sizeof(line) - n, "%s%02x=%02x", n ? " " : "",
                      pmu[i], pcf50605_read(pmu[i]) & 0xff);
    say("preg: PMU %s (Apple: 0x24/0x26 mode 111, the voltage code kept; "
        "apple-power.md 3.3)", line);
    say("preg: cpu=%ld Hz boost=%d usb=%d playing=%d", cpu_frequency,
        get_cpu_boost_counter(), usb_inserted(), music_playing());
    /* sitting 4gh-b5: the count, so a fast row reads as done, not skipped */
    say("preg: checked %d PP registers and %d PMU supplies (read-only)",
        (int)ARRAYLEN(r), (int)ARRAYLEN(pmu));
    return BENCH_DONE;
}

/* ---- prtc (S-6) ------------------------------------------------------ */

#define RTC5014     (*(volatile uint32_t *)0x60005014)

/* The next change of 0x6000_5014, within bound_us (yielding: the UI
   thread holds no lock here).  true: it changed */
static bool rtc_edge(uint32_t *v, uint32_t *us, uint32_t bound_us)
{
    uint32_t v0 = RTC5014, t0 = USEC_TIMER;

    for (;;)
    {
        uint32_t v1 = RTC5014, t = USEC_TIMER;

        if (v1 != v0 || t - t0 > bound_us)
        {
            *v = v1;
            *us = t;
            return v1 != v0;
        }
        yield();
    }
}

static int32_t pmu_secs(void)
{
    struct tm tm;

    if (rtc_read_datetime(&tm) < 0)
        return -1;
    return (int32_t)mktime(&tm);
}

int devbench_pwrp_rtc(void)
{
    static const char *const l[] = { "Hands off, about 12 s.",
                                     "Comparing three clocks." };
    uint32_t v0, v1, u0, u1;
    int32_t p0, p1, ppm;
    bool e0, e1;
    enum bench_res r;

    show("Seconds counter", l, 2);
    e0 = rtc_edge(&v0, &u0, 1500000);
    p0 = pmu_secs();
    sleep(PWRP_RTC_SECS * HZ - HZ / 2);
    e1 = rtc_edge(&v1, &u1, 1500000);
    p1 = pmu_secs();
    r = pwrp_rtc_verdict(e0 && e1 ? pwrp_5014_secs(v1) - pwrp_5014_secs(v0)
                                  : (v1 != v0), u1 - u0, p1 - p0, &ppm);
    say("prtc: 0x60005014 %08lx -> %08lx (%lu s) edges=%d/%d us=%lu "
        "pmu=%ld s ppm=%ld: %s", (unsigned long)v0, (unsigned long)v1,
        (unsigned long)(pwrp_5014_secs(v1) - pwrp_5014_secs(v0)), e0, e1,
        (unsigned long)(u1 - u0), (long)(p1 - p0), (long)ppm,
        r == BENCH_PASS ? "counts at 1 Hz (S-6 awake: yes)"
        : v1 == v0 ? "does not count (S-6: no)" : "counts, other rate");
    return r;
}

/* ---- pstl (S-8) ------------------------------------------------------ */

int devbench_pwrp_stall(void)
{
    static const char *const l[] = { "Hands off, a few seconds.",
                                     "Timed pauses of the core." };
    bool was = music_playing();
    int i, rep, bad = 0;
    enum bench_res r = BENCH_PASS;

    show("Timed pauses", l, 2);
    if (!music_hush())
    {
        say("pstl: Not run: the music didn't stop");
        return BENCH_SKIP;
    }
    say("pstl: cpu=%ld Hz, IRQ and FIQ off for each form, %d reps",
        cpu_frequency, PWRP_STALL_REPS);
    for (i = 0; i < PWRP_NSTALLS && !bad; i++)
    {
        uint32_t got[PWRP_STALL_REPS], ctl = 0, st0 = 0, st1 = 0;

        /* a unit runs only if the forms before it held */
        for (rep = 0; rep < PWRP_STALL_REPS; rep++)
        {
            int irq = disable_interrupt_save(IRQ_FIQ_STATUS);
            uint32_t t0, t1;

            st0 = CPU_INT_STAT;
            t0 = USEC_TIMER;
            CPU_CTL = pwrp_stalls[i].word;
            t1 = USEC_TIMER;
            ctl = CPU_CTL;
            st1 = CPU_INT_STAT;
            wake_core(CPU);         /* clear the control word */
            restore_interrupt(irq);
            got[rep] = t1 - t0;
            if (!pwrp_stall_ok(pwrp_stalls[i].want_us, got[rep]) &&
                pwrp_stalls[i].want_us)
                bad++;
            if (pwrp_stalls[i].want_us == 0 && got[rep] > 8)
                bad++;
            sleep(HZ / 50);         /* the tick and the PCM catch up */
        }
        say("pstl: CPU_CTL=%08lx want=%lu us got=%lu/%lu/%lu us ctl_after="
            "%08lx int_stat %08lx->%08lx: %s",
            (unsigned long)pwrp_stalls[i].word,
            (unsigned long)pwrp_stalls[i].want_us, (unsigned long)got[0],
            (unsigned long)got[1], (unsigned long)got[2],
            (unsigned long)ctl, (unsigned long)st0, (unsigned long)st1,
            bad ? "OFF (later forms not run)" : "held");
    }
    if (bad)
        r = BENCH_FAIL;
    say("pstl: %s", r == BENCH_PASS ? "every form held as stated (S-8: yes)"
                                    : "a form did not hold (S-8: no)");
    if (was)
        audio_resume();
    return r;
}

/* ---- pldo (S-4) ------------------------------------------------------ */

#define LDO_HOLD_S  5

static bool screen_dark(void)
{
    long end;

    backlight_off();
    sleep(HZ / 2);
    if (lcd_active())
        lcd_sleep();
    end = current_tick + 2 * HZ;
    while (lcd_active() && TIME_BEFORE(current_tick, end))
        sleep(HZ / 20);
    return !lcd_active();
}

static void screen_light(void)
{
    backlight_on();
    sleep(HZ);
    if (!lcd_active())
        lcd_awake();
    sleep(HZ / 2);
}

/* One LDO to Apple's sleep form for LDO_HOLD_S and back.  BENCH_PASS:
   written, held, restored and read back; BENCH_SKIP: not in the expected
   on state (nothing written); BENCH_FAIL: not restored */
static enum bench_res ldo_cycle(const char *part, int reg)
{
    struct pwrp_saved sv;
    int v = pcf50605_read(reg), in;

    if (v >= 0 && reg == PWRP_PMU_D3REGC1 &&
        pwrp_ldo_mode((uint8_t)v) == PWRP_LDO_SLEEP)
    {
        /* sitting 4gh-b5: the lcdldo token already put the screen's
           supply in Apple's sleep form while dark: that is this part's
           test, run by the daily path; the sound part runs anyway */
        say("pldo: %s 0x%02x=0x%02x: already mode 010 (the lcdldo token, "
            "dark): the screen part is the token's; the sound part runs",
            part, reg, v & 0xff);
        return BENCH_DONE;
    }
    if (v < 0 || pwrp_ldo_mode((uint8_t)v) != PWRP_LDO_ON)
    {
        say("pldo: %s 0x%02x=0x%02x: Not run (not mode 111, the start-up "
            "it)", part, reg, v & 0xff);
        return BENCH_SKIP;
    }
    pwrp_saved_init(&sv);
    pwrp_save(&sv, PWRP_PMU(reg), (uint32_t)v, 0xFF);
    pcf50605_write(reg, pwrp_ldo_sleep_val((uint8_t)v));
    in = pcf50605_read(reg);
    say("pldo: %s 0x%02x: 0x%02x (mode 111, %u.%02u V) -> 0x%02x (mode 010) "
        "read 0x%02x, %d s", part, reg, v, pwrp_ldo_cv((uint8_t)v) / 100,
        pwrp_ldo_cv((uint8_t)v) % 100, pwrp_ldo_sleep_val((uint8_t)v),
        in & 0xff, LDO_HOLD_S);
    sleep(LDO_HOLD_S * HZ);
    pcf50605_write(reg, (unsigned char)v);
    if (pwrp_unrestored(&sv, rd) >= 0)
        pcf50605_write(reg, (unsigned char)v);  /* once more */
    return check_restored("pldo", &sv) ? BENCH_PASS : BENCH_FAIL;
}

int devbench_pwrp_ldo(void)
{
    static const char *const yes_garbled[] =
        { "Yes", "Garbled", "Blank", "Flickers" };
    static const char *const yes_sound[] =
        { "Yes", "Silent", "Distorted", "Clicks" };
    enum bench_res lcd, codec = BENCH_SKIP;
    struct dac_stats ds;
    long end;
    int a;

    /* part 1: the LCD's supply, the screen dark */
    if (!screen_dark())
    {
        say("pldo: lcd Not run: the screen didn't go dark");
        screen_light();
        return BENCH_SKIP;
    }
    lcd = ldo_cycle("lcd", PWRP_PMU_D3REGC1);
    screen_light();
    if (lcd == BENCH_PASS)
    {
        a = devbench_ask("Screen and sound supply",
                         "Does the screen look normal?\n"
                         "Yes: as before.\n"
                         "Garbled: noise, stripes or wrong colours.\n"
                         "Blank: nothing shows.\n"
                         "Flickers: it shakes or flashes.",
                         yes_garbled, 4);
        say("pldo: lcd answer=%s", a >= 0 ? yes_garbled[a] : "MENU");
        if (a != 0)
            lcd = a < 0 ? BENCH_SKIP : BENCH_FAIL;
    }
    if (lcd != BENCH_PASS && lcd != BENCH_DONE)
    {
        say("pldo: codec Not run: the screen part didn't pass");
        return pwrp_ldo_verdict(lcd, BENCH_SKIP);
    }

    /* part 2: the codec's supply, the DAC in standby, the music paused */
    if (!music_start())
        say("pldo: codec Not run: no music to play");
    else
    {
        say("pldo: codec headphones=%d", headphones_inserted());
        music_hush();
        end = current_tick + (DAC_IDLE_MS / 1000 + 4) * HZ;
        do
        {
            sleep(HZ / 4);
            dac_get_stats(&ds);
        } while (!ds.standby && TIME_BEFORE(current_tick, end));
        if (!ds.standby)
        {
            say("pldo: codec Not run: the DAC didn't reach standby");
            audio_resume();
        }
        else
        {
            codec = ldo_cycle("codec", PWRP_PMU_D1REGC1);
            audio_resume();
            sleep(3 * HZ);
            if (codec == BENCH_PASS)
            {
                a = devbench_ask("Screen and sound supply",
                                 "Do you hear the music normally after "
                                 "resume?\n"
                                 "Yes: as before.\n"
                                 "Silent: no sound.\n"
                                 "Distorted: noisy or wrong.\n"
                                 "Clicks: pops or crackles.",
                                 yes_sound, 4);
                say("pldo: codec answer=%s", a >= 0 ? yes_sound[a] : "MENU");
                if (a != 0)
                    codec = a < 0 ? BENCH_SKIP : BENCH_FAIL;
            }
        }
    }
    a = pwrp_ldo_verdict(lcd, codec);
    if (lcd == BENCH_DONE)
        say("pldo: %s", a == BENCH_PASS ? "the sound supply held mode 010 "
            "(S-4: sound back; the screen's is the lcdldo token's)"
            : a == BENCH_DONE ? "neither part run here (the screen's "
            "supply already in mode 010 by the token)"
            : "the sound part failed (S-4)");
    else
        say("pldo: %s", a == BENCH_PASS ? "both supplies held mode 010 "
            "(S-4: screen and sound back)" : a == BENCH_DONE ? "screen "
            "part only (S-4: the screen comes back)"
            : "a part failed (S-4)");
    return a;
}

/* ---- pden (S-3, awake) ------------------------------------------------ */

#define DEN_WAIT_S  10      /* for the key press */
#define DEN_MIN_US  2000000 /* each bit off at least this long */

/* SELECT within DEN_WAIT_S: the wheel answered */
static bool wheel_answers(const char *name)
{
    const char *l[] = { "Switched off:", name, "", "Press SELECT now.",
                        "(Nothing happens? Wait 10 s.)" };
    long end = current_tick + DEN_WAIT_S * HZ;

    show("Unused blocks off", l, 5);
    button_clear_queue();
    while (TIME_BEFORE(current_tick, end))
    {
        long b = button_get_w_tmo(HZ / 4);

        if (b == BUTTON_SELECT)
            return true;
    }
    return false;
}

int devbench_pwrp_den(void)
{
    uint32_t was = DEV_EN, cand;
    bool playing = music_playing();
    int i, tested = 0;
    enum bench_res r = BENCH_PASS;

    cand = pwrp_den_candidates(was, usb_inserted(), false);
    say("pden: DEV_EN=%08lx DEV_EN2=%08lx usb=%d playing=%d candidates=%08lx",
        (unsigned long)was, (unsigned long)DEV_EN2, usb_inserted(), playing,
        (unsigned long)cand);
    for (i = 0; i < PWRP_NDENS && r == BENCH_PASS; i++)
    {
        uint32_t bit = pwrp_dens[i].bit, t0, u0, us, ticks;
        unsigned long e0;
        struct pwrp_saved sv;
        bool wheel, ok;
        int irq;

        if (!(cand & bit))
            continue;
        tested++;
        pwrp_saved_init(&sv);
        pwrp_save(&sv, 0x6000600c, DEV_EN, bit);
        e0 = music_elapsed();
        t0 = current_tick;
        u0 = USEC_TIMER;
        irq = disable_interrupt_save(IRQ_FIQ_STATUS);
        DEV_EN &= ~bit;             /* that bit only: the drivers own the rest */
        restore_interrupt(irq);
        wheel = wheel_answers(pwrp_dens[i].name);
        while (USEC_TIMER - u0 < DEN_MIN_US)
            sleep(HZ / 10);
        us = USEC_TIMER - u0;
        ticks = current_tick - t0;
        irq = disable_interrupt_save(IRQ_FIQ_STATUS);
        DEV_EN |= bit;
        restore_interrupt(irq);
        ok = pwrp_den_healthy(ticks, us, wheel, playing,
                              music_elapsed() - e0);
        say("pden: %s (bit %d) off %lu us: ticks=%lu wheel=%d played=%lu ms "
            "playing=%d: %s", pwrp_dens[i].name, __builtin_ctz(bit),
            (unsigned long)us, (unsigned long)ticks, wheel,
            music_elapsed() - e0, music_playing(),
            ok ? "healthy" : "NOT healthy (later bits not tested)");
        if (!check_restored("pden", &sv) || !ok)
            r = BENCH_FAIL;
    }
    say("pden: %d of %d named-unused blocks were on; DEV_EN now %08lx "
        "(Apple's sleep set 0xC0010108 not used awake; its PROC_SLEEP test is "
        "not in this build)", tested, PWRP_NDENS, (unsigned long)DEV_EN);
    return tested ? r : BENCH_DONE;
}

/* ---- psrf (S-1) -------------------------------------------------------- */

static struct pwrp_srf_io srf_io IBSS_ATTR;

/* From IRAM, IRQ and FIQ off, the COP parked: no SDRAM access between the
 * request (bit 22 set) and the acknowledge clearing after the release:
 * registers only, the block above in IRAM, no stack.  Apple's request and
 * acknowledge (apple-power.md 2.3 steps 9, 19, 21); the hold is a
 * USEC_TIMER loop, not Apple's 10 ms CPU stall (S-8 is pstl's).  A
 * timed-out acknowledge still releases.  216 B of code */
static void __attribute__((naked, noinline)) ICODE_ATTR
srf_core(struct pwrp_srf_io *io)
{
    (void)io;
    asm volatile(
        "stmfd sp!, {r4-r6, lr}         \n" /* before the window */
        "ldr   r1, =0x7000003c          \n" /* XMB_RAM_CFG */
        "ldr   r2, =0x60005010          \n" /* USEC_TIMER */
        "ldr   r3, [r1]                 \n"
        "str   r3, [r0, #20]            \n" /* cfg_before */
        "orr   r3, r3, #0x00400000      \n"
        "str   r3, [r1]                 \n" /* the request */
        "ldr   r4, [r2]                 \n"
        "ldr   r6, [r0, #4]             \n" /* bound_us */
        "1:                             \n"
        "ldr   r3, [r1]                 \n"
        "tst   r3, #0x40000000          \n"
        "bne   2f                       \n"
        "ldr   r5, [r2]                 \n"
        "sub   r5, r5, r4               \n"
        "cmp   r5, r6                   \n"
        "bls   1b                       \n"
        "mvn   r5, #0                   \n" /* no acknowledge */
        "str   r5, [r0, #8]             \n"
        "str   r3, [r0, #24]            \n"
        "mov   r5, #0                   \n"
        "str   r5, [r0, #12]            \n"
        "b     4f                       \n"
        "2:                             \n"
        "ldr   r5, [r2]                 \n"
        "sub   r5, r5, r4               \n"
        "str   r5, [r0, #8]             \n" /* ack_us */
        "str   r3, [r0, #24]            \n" /* cfg_in */
        "ldr   r4, [r2]                 \n"
        "ldr   r6, [r0, #0]             \n" /* hold_us */
        "3:                             \n"
        "ldr   r5, [r2]                 \n"
        "sub   r5, r5, r4               \n"
        "cmp   r5, r6                   \n"
        "blo   3b                       \n"
        "str   r5, [r0, #12]            \n" /* held_us */
        "4:                             \n"
        "ldr   r3, [r1]                 \n"
        "bic   r3, r3, #0x00400000      \n"
        "str   r3, [r1]                 \n" /* the release */
        "ldr   r4, [r2]                 \n"
        "ldr   r6, [r0, #4]             \n"
        "5:                             \n"
        "ldr   r3, [r1]                 \n"
        "tst   r3, #0x40000000          \n"
        "beq   6f                       \n"
        "ldr   r5, [r2]                 \n"
        "sub   r5, r5, r4               \n"
        "cmp   r5, r6                   \n"
        "bls   5b                       \n"
        "mvn   r5, #0                   \n" /* never cleared */
        "b     7f                       \n"
        "6:                             \n"
        "ldr   r5, [r2]                 \n"
        "sub   r5, r5, r4               \n"
        "7:                             \n"
        "str   r5, [r0, #16]            \n" /* rel_us */
        "str   r3, [r0, #28]            \n" /* cfg_after */
        "ldmfd sp!, {r4-r6, pc}         \n"
        ".ltorg                         \n"
    );
}

/* The COP in its scheduler's idle sleep, seen twice 50 us apart within
   100 ms (power_sleep-pp502x.c's park check) */
static bool cop_parked(void)
{
    uint32_t t0 = USEC_TIMER;

    while (USEC_TIMER - t0 < 100000)
    {
        if (COP_CTL & PROC_SLEEP)
        {
            udelay(50);
            if (COP_CTL & PROC_SLEEP)
                return true;
        }
    }
    return false;
}

extern char _loadaddress[], _text_end[];

static uint32_t code_crc(void)
{
    uint32_t len = (uint32_t)(_text_end - _loadaddress);

    if (len > PWRP_SRF_CRC_BYTES)
        len = PWRP_SRF_CRC_BYTES;
    return crc_32(UNCACHED_ADDR((const char *)_loadaddress), len & ~3u, 0xFFFFFFFFu);
}

int devbench_pwrp_srf(void)
{
    static const char *const l[] = { "Hands off, about 5 s.",
                                     "The memory rests for 1 s.", "",
                                     "Frozen? Hold MENU+SELECT",
                                     "to restart." };
    struct music_spot ms = { false, 0, 0, 0 };
    uint32_t crc0, crc1, cop_lo, cop_hi;
    long end;
    int irq;
    enum bench_res r;
    const char *why = NULL;
    bool disk = false, pcm = false;

    show("Memory rest", l, 5);
    if (usb_inserted())
        why = "a USB cable is in (its controller masters memory)";
    else if (!music_stop_noted(&ms))
        why = "the music didn't stop";
    /* logged first: a file write returns with its transfer done, so the
       log's own write is through before the disk is idled */
    crc0 = code_crc();
    say("psrf: XMB_RAM_CFG=%08lx crc0=%08lx dma_cmd=%08lx/%08lx/%08lx/%08lx "
        "e2_cmd=%08lx", (unsigned long)XMB_RAM_CFG, (unsigned long)crc0,
        (unsigned long)DMA0_CMD,
        (unsigned long)*(volatile uint32_t *)0x6000b020,
        (unsigned long)*(volatile uint32_t *)0x6000b040,
        (unsigned long)*(volatile uint32_t *)0x6000b060,
        (unsigned long)*(volatile uint32_t *)0x60009000);
    if (!why)
    {
        storage_sleepnow();
        end = current_tick + 5 * HZ;
        while (storage_disk_is_active() && TIME_BEFORE(current_tick, end))
            sleep(HZ / 10);
        if (storage_disk_is_active())
            why = "the disk stayed busy";
    }
    if (!why)
    {
        bool got;

        /* a screen push in flight ends first; then the engine is ours
           (the screen uses its CPU copy meanwhile) */
        end = current_tick + HZ / 2;
        while (!(got = e2port_claim(E2PORT_VIDEO)) &&
               TIME_BEFORE(current_tick, end))
            sleep(HZ / 50);
        if (!got)
            why = "the screen engine stayed in use";
    }
    if (why)
    {
        say("psrf: Not run: %s", why);
        music_start_noted(&ms);
        return BENCH_SKIP;
    }
    end = current_tick + HZ / 10;
    while (e2port_busy() && TIME_BEFORE(current_tick, end))
        yield();
    sleep(HZ / 2);                  /* the COP's last decode pass ends */
    srf_io.hold_us = PWRP_SRF_HOLD_US;
    srf_io.bound_us = PWRP_SRF_BOUND_US;
    srf_io.ack_us = srf_io.held_us = srf_io.rel_us = 0;

    irq = disable_interrupt_save(IRQ_FIQ_STATUS);
    cop_lo = COP_INT_EN_STAT;
    cop_hi = COP_HI_INT_EN_STAT;
    COP_INT_DIS = 0xFFFFFFFF;
    COP_HI_INT_DIS = 0xFFFFFFFF;
    /* sitting 4gh-b5 read only "the disk or the music": which one, and
       both flags, so the next sitting knows what to quiet */
    disk = storage_disk_is_active();
    pcm = pcm_is_playing();
    if (e2port_busy())
        why = "the screen engine stayed busy";
    else if (disk && pcm)
        why = "the disk spun up and the music restarted";
    else if (disk)
        why = "the disk spun up again";
    else if (pcm)
        why = "the music restarted";
    else if (!cop_parked())
        why = "the second core didn't park";
    else
    {
        commit_dcache();
        srf_core(&srf_io);
    }
    COP_HI_INT_EN = cop_hi;
    COP_INT_EN = cop_lo;
    core_wake(COP);                 /* CPU first, then the COP */
    restore_interrupt(irq);
    e2port_release(E2PORT_VIDEO);

    if (why)
    {
        say("psrf: Not run: %s (disk=%d pcm=%d audio=%d)", why, (int)disk,
            (int)pcm, audio_status());
        music_start_noted(&ms);
        return BENCH_SKIP;
    }
    crc1 = code_crc();
    r = pwrp_srf_verdict(&srf_io, crc0, crc1);
    say("psrf: before=%08lx in=%08lx after=%08lx ack=%ld us held=%lu us "
        "release=%ld us crc %08lx/%08lx: %s",
        (unsigned long)srf_io.cfg_before, (unsigned long)srf_io.cfg_in,
        (unsigned long)srf_io.cfg_after, (long)srf_io.ack_us,
        (unsigned long)srf_io.held_us, (long)srf_io.rel_us,
        (unsigned long)crc0, (unsigned long)crc1,
        crc0 != crc1 ? "MEMORY CHANGED (S-1: no; restart before use)"
        : r == BENCH_PASS ? "acknowledged, released, memory kept (S-1: yes)"
        : srf_io.ack_us == PWRP_SRF_TIMEOUT ? "no acknowledge (S-1: no)"
        : "the acknowledge didn't clear or the word didn't come back");
    music_start_noted(&ms);
    return r;
}

#elif defined(DEVBENCH_SIM) && defined(HAVE_IPOD_UI_DEVMODE)
/* The simulator has none of these registers, regulators or the SDRAM
 * controller: each row is refused (Not run, a skip), never modelled as a
 * pass.  DBS_FAIL<id> fails one instead, for the runner's failure path */
#include "file.h"
#include "devbench.h"
#include "devbench_pwr.h"

void dbs_log(const char *line);     /* devbench_sim.c */

static int sim_row(const char *id, const char *what)
{
    char path[MAX_PATH], text[160];

    snprintf(path, sizeof(path), DBS_FAIL "%s", id);
    if (file_exists(path))
    {
        snprintf(text, sizeof(text), "%s: FAIL: %s (sim fault switch)", id,
                 what);
        dbs_log(text);
        return BENCH_FAIL;
    }
    snprintf(text, sizeof(text), "%s: Not run (sim): %s is PP5022 silicon; "
             "[hypothesis] until the bench", id, what);
    dbs_log(text);
    return BENCH_SKIP;
}

int devbench_pwrp_grade(void) { return sim_row("pgrd", "the speed grade"); }
int devbench_pwrp_regs(void)  { return sim_row("preg", "the power registers"); }
int devbench_pwrp_rtc(void)   { return sim_row("prtc", "the seconds counter"); }
int devbench_pwrp_stall(void) { return sim_row("pstl", "CPU_CTL's stalls"); }
int devbench_pwrp_ldo(void)   { return sim_row("pldo", "the LDO sleep mode"); }
int devbench_pwrp_den(void)   { return sim_row("pden", "DEV_EN's gates"); }
int devbench_pwrp_srf(void)   { return sim_row("psrf", "SDRAM self-refresh"); }
#endif
