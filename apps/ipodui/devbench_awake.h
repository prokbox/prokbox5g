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

/* The "Battery savings" bench rows (P19 pwr-awake): the device side of
 * the awake rows taken from Apple 1.3 (firmware/export/pwr_awake.h;
 * docs/pp5022-reference/apple-power-catalog.md section 3 and its T-*
 * specs). Developer builds only (HAVE_IPOD_UI_DEVMODE, and devbench.c
 * itself only in the device test builds): the release has none of it.
 *
 * Two uses of this header, as devbench_pwr.h:
 *  - included normally: the prototypes and the picks;
 *  - included inside devbench.c's steps[] with DEVBENCH_AWAKE_ROWS
 *    defined: the rows, as table initializers (the one hook):
 *        #define DEVBENCH_AWAKE_ROWS
 *        #include "devbench_awake.h"
 *
 * The rows, basic to advanced (bench_awake_picks below); owner time in
 * brackets (hands-off wait / owner's own):
 *   awxm  read-only: XMB_RAM_CFG around the USB reset's OR (AW-8) and the
 *         PMU ADC's done-bit counts (AW-9) [0 / 0]
 *   awd2  music on the headphones: the accessory supply (D2REGC1) off for
 *         5 s and back (AW-3, T-D2REG): "Did the music keep playing?"
 *         [5 s / 20 s]
 *   awd2c optional (skip if no adapter): the same with a passive USB-C
 *         audio adapter plugged in [5 s / 40 s]
 *   awlg  hands-off, dark, about 20 min: the drive's release linger
 *         (AW-1, T-LINGER) forced on; pass: at least two linger cuts,
 *         no rewake, no underrun [20 min / 30 s]
 *   awus  a wall charger for about 30 s: the controller parks (AW-2,
 *         T-USBPROBE); then the question is asked after the session,
 *         once plugged into the computer: "Did the drive appear?"
 *         [30 s / 1 min]
 *   awld  the screen dark for 12 s and back, five times, with the LCD
 *         supply in mode 010 (AW-4, T-LCDLDO): "Did the screen come back
 *         normal each time?" May leave the screen dark [70 s / 20 s]
 *   awi2  music paused: the codec clock gated in DAC standby (variant a),
 *         then with I2S (variant b), three resumes each (AW-5,
 *         T-I2SGATE): "Did the music resume cleanly each time?" May
 *         freeze [2 min / 1 min]
 * Recovery from a freeze is always MENU+SELECT held; the rows switch
 * nothing on for normal use (each force is undone at the row's end).
 *
 * A pass writes "token pwraw-<row> <build> <date>" to the results file:
 * copy the row's bit into firmware/export/pwr_awake_tokens.h to make it
 * the daily default. The simulator refuses every row ("Not run (sim)"). */

#ifdef DEVBENCH_AWAKE_ROWS
#undef DEVBENCH_AWAKE_ROWS
#ifdef HAVE_IPOD_UI_DEVMODE
    /* ---- Battery savings (devbench_awake.h; picked runs only:
       bench_awake_picks) ---- */
    { "awxm", "Memory setting log", NULL, NULL, devbench_aw_xmb, NULL,
      { NULL }, BENCH_F_AUTO | BENCH_F_PICKED, 10, 0, 0 },
    { "awd2", "Accessory supply",
      "About 30 s. Headphones on the headphone socket; the album you "
      "played last plays. The supply for accessories goes off for 5 s "
      "and comes back.",
      "Listen for the 5 s; the question follows.",
      devbench_aw_d2reg,
      "Did the music keep playing?\n"
      "Yes: no change at all.\n"
      "It stopped: silent for a moment.\n"
      "It crackled: noise or clicks.",
      { "Yes", "It stopped", "It crackled" }, BENCH_F_PICKED, 0, 25, 5 },
    { "awd2c", "Accessory supply, USB-C sound",
      "Only if you have a passive USB-C audio adapter: plug it in with "
      "headphones on it and press SELECT. The supply goes off for 5 s "
      "and comes back while the album plays.",
      "Listen to the adapter for the 5 s; the question follows. No "
      "adapter: choose Skip.",
      devbench_aw_d2reg,
      "If you have a passive USB-C audio adapter, plug it in: do you hear "
      "music from it?\n"
      "Yes: music all the way through.\n"
      "It stopped: silent while the supply was off.\n"
      "No sound: nothing from the adapter at all.",
      { "Yes", "It stopped", "No sound" }, BENCH_F_PICKED, 0, 45, 5 },
    { "awlg", "Drive rest after reading",
      "Hands off, about 20 min, in the dark. It plays the album you "
      "played last. The screen goes dark: switch Hold on and leave the "
      "iPod alone until the screen lights again.",
      "Nothing to watch. At the end, switch Hold off and press SELECT.",
      devbench_aw_linger, NULL, { NULL }, BENCH_F_PICKED, 0, 1204, 1200 },
    { "awus", "Charger only",
      "About 1 min. Plug the iPod into a wall charger (not a computer) "
      "and press SELECT. Leave it for about 30 s.",
      "Then unplug it and plug it into the computer. When the drive has "
      "appeared (or after a minute), unplug, open Bench session and pick "
      "Go on: the question comes first.",
      devbench_aw_usb,
      "Did the drive appear on the computer as usual?\n"
      "Yes: it appeared and opened.\n"
      "Late: only after a minute or a replug.\n"
      "No: it never appeared.",
      { "Yes", "Late", "No" }, BENCH_F_PICKED | BENCH_F_LEAVES, 0, 90, 30 },
    { "awld", "Screen supply at rest",
      "About 90 s. The screen goes dark for 12 s and comes back, five "
      "times. If it stays dark, hold MENU+SELECT to restart.",
      "Watch the screen each time it comes back; the question follows.",
      devbench_aw_lcdldo,
      "Did the screen come back normal each time?\n"
      "Yes: normal every time.\n"
      "Marks: lines, colours or noise.\n"
      "Slow: it took longer to light.",
      { "Yes", "Marks", "Slow" }, BENCH_F_PICKED, 0, 90, 0 },  /* watched */
    { "awi2", "Sound clock at rest",
      "About 3 min. Headphones on; the album you played last plays. The "
      "music pauses "
      "and plays again, six times, about 20 s apart. The iPod may freeze; "
      "if so, hold MENU+SELECT to restart.",
      "Listen at each restart of the music; the question follows.",
      devbench_aw_i2s,
      "Did the music resume cleanly each time?\n"
      "Yes: clean every time.\n"
      "A pop: a click or pop at a restart.\n"
      "No sound: it stayed silent once.",
      { "Yes", "A pop", "No sound" }, BENCH_F_PICKED, 0, 180, 0 },  /* listened to */
#endif
#else /* the header proper */

#ifndef _IPODUI_DEVBENCH_AWAKE_H_
#define _IPODUI_DEVBENCH_AWAKE_H_

#include "devbench.h"

/* the rows (devbench_awake.c; each an enum bench_res) */
int devbench_aw_xmb(void);
int devbench_aw_d2reg(void);
int devbench_aw_linger(void);
int devbench_aw_usb(void);
int devbench_aw_lcdldo(void);
int devbench_aw_i2s(void);

/* The group's run order and needs, for the mode that runs it (the BCM
 * session's integrator wires it into the combined sitting). Basic to
 * advanced: the read-only row, the plain I2C toggle, the hands-off
 * policy rows, then the two that may freeze. awd2c needs awd2 (the same
 * toggle, proven on the headphone socket first); the others stand alone.
 * None earns a verified line (ALSO); none restarts. */
static const struct bench_pick bench_awake_picks[] =
{
    { "awxm",   BENCH_R_ALSO, { NULL } },
    { "awd2",   BENCH_R_ALSO, { NULL } },
    { "awd2c",  BENCH_R_ALSO, { "awd2" } },
    { "awlg",   BENCH_R_ALSO, { NULL } },
    { "awus",   BENCH_R_ALSO, { NULL } },
    { "awld",   BENCH_R_ALSO, { NULL } },
    { "awi2",   BENCH_R_ALSO, { NULL } },
};
#define BENCH_AWAKE_NPICKS \
    ((int)(sizeof(bench_awake_picks) / sizeof(bench_awake_picks[0])))

/* The group as a picked run: table indices in order, roles, needs; an id
 * the table lacks is left out. The run's count */
static inline int bench_awake_select(const struct bench_step *t, int n,
                                     struct bench_run *run)
{
    int p, d;

    run->n = 0;
    for (p = 0; p < BENCH_AWAKE_NPICKS && run->n < BENCH_MAX_STEPS; p++)
    {
        int k = bench_find(t, n, bench_awake_picks[p].id);

        if (k < 0 || k >= BENCH_MAX_STEPS)
            continue;
        run->k[run->n] = (uint8_t)k;
        for (d = 0; d < BENCH_NEEDS; d++)
        {
            int kd = bench_awake_picks[p].need[d]
                     ? bench_find(t, n, bench_awake_picks[p].need[d]) : -1;

            run->need[run->n][d] = (int8_t)(kd < BENCH_MAX_STEPS ? kd : -1);
        }
        run->role[run->n++] = bench_awake_picks[p].role;
    }
    return run->n;
}

/* The tokens (firmware/export/pwr_awake_tokens.h): the step whose final
 * result (the automatic part and the owner's answer) earns each. The
 * integrator's end-of-session hook, beside bench_clock150_ok(): for each
 * k < BENCH_AWAKE_NTOKENS with bench_awake_token_ok(), a "token <line>"
 * from bench_awake_fmt_token() */
static const struct { const char *step, *token, *row; } bench_awake_tokens[] =
{
    { "awlg", "pwraw-linger",  "PWRAW_LINGER" },
    { "awus", "pwraw-usboff",  "PWRAW_USBOFF" },
    { "awld", "pwraw-lcdldo",  "PWRAW_LCDLDO" },
    { "awi2", "pwraw-i2sgate", "PWRAW_I2SGATE" },
};
#define BENCH_AWAKE_NTOKENS \
    ((int)(sizeof(bench_awake_tokens) / sizeof(bench_awake_tokens[0])))

static inline bool bench_awake_token_ok(const struct bench_step *t, int n,
                                        const struct bench_state *st, int k)
{
    int i = bench_find(t, n, bench_awake_tokens[k].step);

    return i >= 0 && st->res[i] == BENCH_PASS;
}

static inline int bench_awake_fmt_token(char *buf, size_t n, int k,
                                        const char *build, const char *date)
{
    return snprintf(buf, n, "%s %s PASS on the owner's 5.5G: session %s, "
                    "build %s (set %s in pwr_awake_tokens.h)",
                    bench_awake_tokens[k].token, bench_awake_tokens[k].step,
                    date && *date ? date : "-", build,
                    bench_awake_tokens[k].row);
}

#endif /* _IPODUI_DEVBENCH_AWAKE_H_ */
#endif /* DEVBENCH_AWAKE_ROWS */
