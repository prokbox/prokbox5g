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

/* The bench session in the simulator: devbench.c's device calls, mapped to
 * devbench_sim.c's stubs (devbench_sim.h says what is modelled and what is
 * real).  Included by devbench.c only, after every other header, and only
 * when DEVBENCH_SIM is set: the device build never sees it.
 *
 * The runner itself (the table, the session, the results file, the replay,
 * resume, the auto-skips, the summary) is not touched: what devbench.c
 * calls that the simulator has for real (the job model's self-test, the
 * simplelist screens, ui_log, the PCM underrun count, the UI boundary's
 * counters, the clocks, the file system) stays real; the results file's
 * open goes through dbs_open() only for its fault switch. */
#ifndef _IPODUI_DEVBENCH_SIM_CALLS_H_
#define _IPODUI_DEVBENCH_SIM_CALLS_H_

#include <stdbool.h>
#include <stdint.h>
#include "devbench_sim.h"
#include "ipodui/fe_be.h"

#ifndef DEVBENCH_SIM
#error "devbench_sim_calls.h is for the simulator's bench session only"
#endif

struct bvp;
struct simplelist_info;

/* the rows (devbcm.c, devbcm_probes.c on the device): a modelled pass
   (or fail, with DBS_FAIL<id>), their log and summary lines through the
   bench hook, as the device's rows send them */
bool dbs_row_c1(void);
bool dbs_row_c2(void);
bool dbs_row_c3(void);
bool dbs_row_pb1(void);
bool dbs_row_pb2(void);
bool dbs_row_pb3(void);
bool dbs_row_pb4(void);
bool dbs_row_pb6(void);
bool dbs_row_pb7(void);
bool dbs_row_pb8(void);
bool dbs_row_pb9(void);
bool dbs_row_pw1(void);
bool dbs_row_pw2(void);
bool dbs_row_pw3(void);
bool dbs_row_pw4(void);
bool dbs_row_pw5(void);
bool dbs_row_pw6(void);
bool dbs_row_pw7(void);
bool dbs_row_pw8(void);
bool dbs_row_s6(void);
bool dbs_row_s7(void);
bool dbs_row_vfeed(void);
bool dbs_row_b13(void);
bool dbs_row_b5(void);
bool dbs_row_cr1(void);
bool dbs_row_rstat(void);
bool dbs_boot_dma(void);
int dbs_vclk_verify(void);
int dbs_vclk_gov(void);


/* devbcm.c's session helpers: the same screens (simplelist, SELECT on the
   last line or on an answer), the bench hook, the step log */
void dbs_bench_mode(bool batch, void (*hook)(const char *line));
unsigned dbs_bench_stages(unsigned stages);
void dbs_log(const char *line);
bool dbs_show(struct simplelist_info *info);
bool dbs_lines_go(const char *title, const char *const *lines, int n,
                  const char *menu, const char *go);
int dbs_lines_choose(const char *title, const char *const *lines, int n,
                     const char *const *opts, int nopts);


/* the device test's flags (bcm-devtest.c): the stage pass flags as the
   model holds them (DBS_NOFLAGS) */
bool dbs_4b_allowed(const char **why);
bool dbs_4c_allowed(const char **why);
bool dbs_5i_allowed(const char **why);
void dbs_forget_4c(void);


/* the chip: the probe image's port reads (the merged image's job block as
   the model answers it), the probe (the image's block for real), the COP
   worker, the job driver's state */
int dbs_video_rd(uint32_t addr, uint32_t *val);
int dbs_vplayer_probe(int (*pre)(void *ctx),
                      int (*fn)(struct bvp *b, uint32_t fast, void *ctx),
                      void *ctx);
int dbs_vplayer_peek(int (*pre)(void *ctx), void *ctx);
bool dbs_cop_stuck(void);
const char *dbs_off_why(bool *restart);
int dbs_inject_fail(uint32_t op, uint32_t n);
int dbs_selftest(void);


/* the UI on the COP: the simulator has one core and runs the split on its
   CPU thread; asked for the COP (ucop), the model reports COP from the
   next start (the real switch is set too) */
enum fe_mode dbs_fe_mode(void);
void dbs_fe_mode_set_next(enum fe_mode m);
enum fe_mode dbs_fe_mode_next(void);
void dbs_fe_mode_clear_next(void);
/* the boundary's counters; bench_sim set fail_ucop2: a stall in the soak */
void dbs_fe_stats_get(struct fe_be_stats *st);


/* the session's switches (devstatus.c on the device): none to set */
bool dbs_switch_begin(int mode);
void dbs_switch_end(void);


/* the watchdog's restart: logged (a script fails on it), never taken */
void dbs_panic_record(const char *msg);
void dbs_reboot(void);


/* the results file's open: the fault switch (DBS_PUTFAIL) */
int dbs_open(const char *path, int flags, ...);


/* ---- The mapping (devbench.c only; devbench_sim.c defines the stubs) --- */
#ifndef DEVBENCH_SIM_IMPL
#define devbcm_c1               dbs_row_c1
#define devbcm_c2               dbs_row_c2
#define devbcm_c3               dbs_row_c3
#define devbcm_pb1              dbs_row_pb1
#define devbcm_pb2              dbs_row_pb2
#define devbcm_pb3              dbs_row_pb3
#define devbcm_pb4              dbs_row_pb4
#define devbcm_pb6              dbs_row_pb6
#define devbcm_pb7              dbs_row_pb7
#define devbcm_pb8              dbs_row_pb8
#define devbcm_pb9              dbs_row_pb9
#define devbcm_pw1              dbs_row_pw1
#define devbcm_pw2              dbs_row_pw2
#define devbcm_pw3              dbs_row_pw3
#define devbcm_pw4              dbs_row_pw4
#define devbcm_pw5              dbs_row_pw5
#define devbcm_pw6              dbs_row_pw6
#define devbcm_pw7              dbs_row_pw7
#define devbcm_pw8              dbs_row_pw8
#define devbcm_cpu_test6        dbs_row_s6
#define devbcm_cpu_test7        dbs_row_s7
#define devbcm_vfeed            dbs_row_vfeed
#define devbcm_warmswap         dbs_row_b13
#define devbcm_window           dbs_row_b5
#define devbcm_commit           dbs_row_cr1
#define devbcm_boot_status      dbs_row_rstat
#define devbcm_boot_dma         dbs_boot_dma
#define devbench_vclk_verify    dbs_vclk_verify
#define devbench_vclk_gov       dbs_vclk_gov
#define devbcm_bench_mode       dbs_bench_mode
#define devbcm_bench_stages     dbs_bench_stages
#define devbcm_log              dbs_log
#define devbcm_show             dbs_show
#define devbcm_lines_go         dbs_lines_go
#define devbcm_lines_choose     dbs_lines_choose
#define bcm_devtest_4b_allowed  dbs_4b_allowed
#define bcm_devtest_4c_allowed  dbs_4c_allowed
#define bcm_devtest_5i_allowed  dbs_5i_allowed
#define bcm_devtest_forget_4c   dbs_forget_4c
#define bcm_video_rd            dbs_video_rd
#define vplayer_probe           dbs_vplayer_probe
#define vplayer_peek            dbs_vplayer_peek
#define bcm_cop_stuck           dbs_cop_stuck
#define bcmjob_off_why          dbs_off_why
#define bcmjob_dev_inject_fail  dbs_inject_fail
#define bcmjob_selftest         dbs_selftest
#define fe_be_mode              dbs_fe_mode
#define fe_be_mode_set_next     dbs_fe_mode_set_next
#define fe_be_mode_next         dbs_fe_mode_next
#define fe_be_mode_clear_next   dbs_fe_mode_clear_next
#define fe_be_stats_get         dbs_fe_stats_get
#define devswitch_bench_begin   dbs_switch_begin
#define devswitch_bench_end     dbs_switch_end
#define panic_record_save(a, b, c, d, msg) dbs_panic_record(msg)
#define system_reboot           dbs_reboot
#undef open
#define open                    dbs_open
#endif /* !DEVBENCH_SIM_IMPL */

/* the watchdog's screen thread (it only draws the give-up screen): not
   made in the sim, where devbench.c's "no thread" path runs.  A host
   thread needs a bigger stack than the device's 2 KB (make_context fails
   and the sim retries for ever), and the sim's thread_wait() never returns
   for a thread that ends before it first runs (a session resumed with
   nothing left to run: start, then stop at once) */
#ifndef DEVBENCH_SIM_IMPL
#define create_thread(...)      0
#endif

/* the image the session records: the sim's player has no image of its own
   (the model needs none), so the session models the bench builds' merged
   image (its size and FNV-1a in the session, verified and token lines) */
#ifndef DEVBENCH_SIM_IMPL
#undef BCMVIDEO_IMAGE_BYTES
#undef BCMVIDEO_IMAGE_FNV
#undef BCMVIDEO_IMAGE_KIND
#define BCMVIDEO_IMAGE_BYTES    BCMVIDEO_MERGED_BYTES
#define BCMVIDEO_IMAGE_FNV      BCMVIDEO_MERGED_FNV
#define BCMVIDEO_IMAGE_KIND     "merged"
#endif

/* the soak the sim can afford (the device's: 100 s) */
#define UCOP_MIN_S              5

#endif /* _IPODUI_DEVBENCH_SIM_CALLS_H_ */
