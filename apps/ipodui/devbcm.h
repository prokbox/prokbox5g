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
 * Developer > Advanced > Video chip: the BCM2722 device test's rows and
 * the opt-in boot path (HAVE_BCMFW_DEVTEST / HAVE_BCMFW, issue #60), as
 * tables of struct devbcm_row that devmode.c lists (devmode menu
 * re-layout, owner 2026-10-26).  The stages the Bench session runs
 * (0-5, 4b, 4c, 6, 7, B5, B13, CR1) are its steps, not rows here.
 * Runbook: docs/research/bcm2722/bench-p19.md.
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
#ifndef _IPODUI_DEVBCM_H_
#define _IPODUI_DEVBCM_H_

#include <stdbool.h>

/* One row: its text (a literal, or ID2P(LANG_...) cast) and its screen,
 * true if USB was attached (the stock debug screens' shape) */
struct devbcm_row
{
    const char *desc;
    bool (*fn)(void);
};

bool devbcm_test5b(void);
bool devbcm_test5i(void);
bool devbcm_test5s(void);
bool devbcm_test3d(void);

/* The optional stages the Bench session doesn't run */
#define DEVBCM_STAGE_ROWS \
        { "BCM test: stage 5b (drift A/B, optional)", devbcm_test5b }, \
        { "BCM test: stage 5i (idle sleep, optional)", devbcm_test5i }, \
        { "BCM test: stage 5s (sleep spikes, optional)", devbcm_test5s }, \
        { "BCM test: stage 3d (3D render, optional)", devbcm_test3d },

#ifdef HAVE_BCMFW
bool devbcm_boot_status(void);
bool devbcm_boot_off(void);
bool devbcm_boot_apple(void);
bool devbcm_boot_cpu(void);
bool devbcm_boot_dma(void);
bool devbcm_boot_fallback(void);
#ifdef HAVE_BCMFW_L1
bool devbcm_inject_swap(void);
bool devbcm_inject_swap_all(void);
bool devbcm_inject_wake(void);

/* #62 level 1, session 9 (independence.md section 5): Fault injection */
#define DEVBCM_L1_ROWS \
        { "BCM boot path: fail next boot's DMA rung", devbcm_inject_swap }, \
        { "BCM boot path: fail next boot's every rung", devbcm_inject_swap_all }, \
        { "BCM boot path: next wake never answers", devbcm_inject_wake },
#endif

#ifdef HAVE_BCMFW_VIDEO
bool devbcm_test6(void);
bool devbcm_test7(void);
bool devbcm_test8(void);
bool devbcm_test8b(void);
bool devbcm_test8a1(void);
bool devbcm_dmalab(void);
bool devbcm_warmswap(void);
#ifdef HAVE_BCM_JOBS
bool devbcm_jobselftest(void);
#define DEVBCM_JOB_ROW \
        { "BCM jobs: driver self-test", devbcm_jobselftest },
#else
#define DEVBCM_JOB_ROW
#endif
bool devbcm_window(void);
bool devbcm_commit(void);
bool devbcm_fetch(void);
bool devbcm_bandwidth(void);
bool devbcm_fullvlog(void);
bool devbcm_test9(void);
/* The stages that play audio, with the codec on the main core for the
 * run (session 36c: with the COP decode switch on, the PP's audio did
 * not run under stage 7 and 8a-0); the switch is restored after */
bool devbcm_cpu_test6(void);
bool devbcm_vfeed(void);            /* bench row vfeed: the video pace */
bool devbcm_cpu_test7(void);
bool devbcm_cpu_test8(void);
bool devbcm_cpu_test8b(void);
/* The second DMA engine lab (E2-0 ... E2-7; its E2-7 plays a soundtrack,
 * so it runs with the codec on the main core too) */
bool devbcm_cpu_e2lab(void);
/* The brief probes (devbcm_probes.c): brief-map-peripherals.md section
 * 11 (PB-1 to PB-4) and brief-map-power.md section 9 (PW1, PW2, PW7,
 * PW8); read-only or reversible, nothing driven */
bool devbcm_pb1(void);
bool devbcm_pb2(void);
bool devbcm_pb3(void);
bool devbcm_pb4(void);
bool devbcm_pw1(void);
bool devbcm_pw2(void);
bool devbcm_pw7(void);
bool devbcm_pw8(void);
/* The TV-out probes (devbcm_probes.c, devbcm_pb6.h, devbcm_tv.h;
 * bench-p19.md PB-6 to PB-9): Apple's own pattern from its NOR vmcs (PB-6,
 * no lock), the timing generator alone, then composite output to a TV
 * over the AV cable; PB-8 and PB-9 are locked until the row before passed */
bool devbcm_pb6(void);
bool devbcm_pb7(void);
bool devbcm_pb8(void);
bool devbcm_pb9(void);
/* The power probes (devbcm_probes.c, devbcm_pw.h; bench-p19.md PW3 and
 * PW4): the core's regulator at Apple's levels on the crystal, then
 * Apple's clock ladder with its levels; PW4 is locked until PW3 passed */
bool devbcm_pw3(void);
bool devbcm_pw4(void);
/* The hibernate probes (devbcm_probes.c, devbcm_hib.h; bench-p19.md PW5
 * and PW6): Apple's hibernate pins around the image's park, locked until
 * PW1 passed; then the port and the panel with the chip cut (read only),
 * locked until PW5 drove its pins */
bool devbcm_pw5(void);
bool devbcm_pw6(void);
/* P19 19-9's COP rows (devbcm_probes.c; bench-p19.md C1-C3): engine 2
 * from the COP, the shared cache data array, the COP's sleep under
 * yield() polling */
bool devbcm_c1(void);
bool devbcm_c2(void);
bool devbcm_c3(void);
/* devbcm.c's helpers for them */
struct simplelist_info;
bool devbcm_lab_open(struct simplelist_info *info, char *title,
                     const char *const *lines, int n);
bool devbcm_confirm(struct simplelist_info *info, char *title,
                    const char *const *lines, int n);
void devbcm_log(const char *line);
bool devbcm_show(struct simplelist_info *info);
/* The bench session (devbench.c): batch skips the rows' own confirm
   screens and summary displays; hook sees their log and summary lines */
void devbcm_bench_mode(bool batch, void (*hook)(const char *line));
unsigned devbcm_bench_stages(unsigned stages);
bool devbench_session(void);
/* devbench.c: a question with its text word-wrapped to the screen; the
   answer picked (0 ...), or -1 for MENU (devbench_power.c's questions) */
int devbench_ask(const char *title, const char *text,
                 const char *const *opts, int n);
bool devbcm_cue(const char *title, const char *l1, const char *l2);
int devbcm_choose(const char *title, const char *l1, const char *l2,
                  const char *const *opts, int n);
/* The bench session's screens: wrapped lines, then the buttons */
bool devbcm_lines_go(const char *title, const char *const *lines, int n,
                     const char *menu, const char *go);
int devbcm_lines_choose(const char *title, const char *const *lines, int n,
                        const char *const *opts, int nopts);
void devbcm_pcm_silence(bool on);
/* Video chip > Probes: the optional stages and labs, then the probes and
 * the COP rows (the Bench session runs the probes as steps too) */
#define DEVBCM_PROBE_ROWS DEVBCM_STAGE_ROWS \
        { "BCM test: stage 8a-0/0b (audio pins, listen, pulls)", devbcm_cpu_test8 }, \
        { "BCM test: stage 8a-1 (a tone on the chip's pin; locked)", devbcm_test8a1 }, \
        { "BCM test: stage 8b (a tone through the chip)", devbcm_cpu_test8b }, \
        { "BCM test: DMA lab (PP DMA into the chip, read back)", devbcm_dmalab }, \
        { "BCM test: texel fetch (B6, optional)", devbcm_fetch }, \
        { "BCM test: memory bandwidth (B7, optional)", devbcm_bandwidth }, \
        { "BCM test: second DMA engine (E2)", devbcm_cpu_e2lab }, \
        { "BCM test: stage 9 (AAC on the chip)", devbcm_test9 }, \
        { "BCM probe PB-1: register census", devbcm_pb1 }, \
        { "BCM probe PB-2: pin levels (on cue)", devbcm_pb2 }, \
        { "BCM probe PB-3: USB status (cable on cue)", devbcm_pb3 }, \
        { "BCM probe PB-4: pin pulls", devbcm_pb4 }, \
        { "BCM probe PB-6: TV with Apple's own pattern", devbcm_pb6 }, \
        { "BCM probe PB-7: TV timing (no output)", devbcm_pb7 }, \
        { "BCM probe PB-8: TV output (AV cable and TV)", devbcm_pb8 }, \
        { "BCM probe PB-9: TV width 640 vs 704", devbcm_pb9 }, \
        { "BCM probe PW1: power census", devbcm_pw1 }, \
        { "BCM probe PW2: caches", devbcm_pw2 }, \
        { "BCM probe PW3: core voltage ladder", devbcm_pw3 }, \
        { "BCM probe PW4: clock and voltage ladder", devbcm_pw4 }, \
        { "BCM probe PW5: hibernate and wake", devbcm_pw5 }, \
        { "BCM probe PW6: LCD with the BCM off", devbcm_pw6 }, \
        { "BCM probe PW7: second port bank", devbcm_pw7 }, \
        { "BCM probe PW8: DMA copies", devbcm_pw8 }, \
        { "COP row C1: engine 2 from the COP", devbcm_c1 }, \
        { "COP row C2: shared cache data array", devbcm_c2 }, \
        { "COP row C3: COP sleep under yield", devbcm_c3 },
/* Video chip's own rows: the job driver's self-test, the full log */
#define DEVBCM_JOB_ROWS DEVBCM_JOB_ROW \
        { "Video: full log on every play (toggle)", devbcm_fullvlog },
#else
#define DEVBCM_PROBE_ROWS DEVBCM_STAGE_ROWS
#define DEVBCM_JOB_ROWS
#endif

/* Video chip > Boot path */
#define DEVBCM_BOOT_ROWS \
        { "BCM boot path: status", devbcm_boot_status }, \
        { "BCM boot path: build default", devbcm_boot_off }, \
        { "BCM boot path: Apple vmcs (forced)", devbcm_boot_apple }, \
        { "BCM boot path: CPU push (stage 2)", devbcm_boot_cpu }, \
        { "BCM boot path: DMA (stage 3 or 5)", devbcm_boot_dma }, \
        { "BCM boot path: test fallback", devbcm_boot_fallback },
#else
#define DEVBCM_PROBE_ROWS DEVBCM_STAGE_ROWS
#define DEVBCM_JOB_ROWS
#endif

#endif /* _IPODUI_DEVBCM_H_ */
