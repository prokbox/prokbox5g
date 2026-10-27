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

#ifndef BCMFW_JMODEL_H
#define BCMFW_JMODEL_H

/* The host model of the display job interface (P19 contract 4): the
 * bcmjob.h API over the reference executor, with modelled time, faults
 * and power states.  The simulator wraps it (uisimulator/common/
 * sim_bcm_job.c: the shell's pixels then come from the same definition
 * as the BCM's); the host tests drive it directly (jmtest.c).  Single
 * threaded: time moves only in jmodel_advance() and bcmjob_wait(). */

#include <stdbool.h>
#include <stdint.h>
#include "jref.h"
#include "bcmjob.h"

#ifndef JREF_DECODE
#error "the host model offers DECODE: build it, jref.c and their callers with -DJREF_DECODE"
#endif

struct jmodel_cfg
{
    struct jref_surf front, back, panel;
    void *pool_mem[J_CLASSES];          /* NULL: the class is not offered */
    uint32_t pool_bytes[J_CLASSES];
    /* each pool's base in the BCM's map, which ALLOC's reply adds the
       slot's offset to (jobs.h J_OP_ALLOC); 0: the BCM's own, config.h's
       BCMFW_JOBS_* (the display-only image's when decode is set, which
       only it offers; else the merged image's), and 0 for the SDRAM
       classes, which have no address yet */
    uint32_t pool_addr[J_CLASSES];
    void (*trace)(const char *line);    /* the gate's lines; NULL: none */
    const uint16_t *pp_fb;              /* the PP framebuffer (front's size):
                                           bcmjob_resync() and a restart's
                                           first frame come from it */
    /* J_OP_DECODE: jdec_run() with the caller's work area (jdec.h); NULL:
       not offered.  The model links no decoder itself. */
    int (*decode)(const uint8_t *jpg, uint32_t len, const struct jdec_out *out,
                  int n, struct jdec_info *info);
    /* the image's decode work area, bytes: a DECODE needing more (jdec.h
       jdec_work()) fails HP_E_NOMEM before the hook runs; 0: the
       display-only image's (config.h, 236,544 B) */
    uint32_t dec_work;
};

enum jmodel_state { JM_C0, JM_C1, JM_C2, JM_STATES };

enum jmodel_fault
{
    JM_FAULT_CLEAR_OP,      /* the image lacks op (its J_OPS bit clear) */
    JM_FAULT_SLOW_OP,       /* op takes 40 x its deadline */
    JM_FAULT_FAIL_OP,       /* op fails (HP_E_INVAL) */
    JM_FAULT_DEAD,          /* the heartbeat stops: nothing runs */
    JM_FAULT_CUT,           /* the BCM loses power now (epoch + 1) */
    JM_FAULT_RING_FULL,     /* the next 'op' submits answer BUSY */
    JM_FAULT_NONE           /* clear every fault */
};

void jmodel_init(const struct jmodel_cfg *cfg);
/* The platform's inputs: the backlight (lit: the BCM up; dark: cut
 * unless a want holds it) and the charger */
void jmodel_set_lit(bool lit);
void jmodel_set_charger(bool on);
/* Time: run the queued jobs that finish by now + ms */
void jmodel_advance(uint32_t ms);
uint32_t jmodel_now(void);
void jmodel_fault(enum jmodel_fault f, uint32_t op);
/* The next n jobs of op fail (as JM_FAULT_FAIL_OP, through the same
 * reply path), then it runs again; n = 0 disarms.  jmodel_forgive clears
 * the op's failed mark and count.  Developer hooks (the driver's
 * bjc_inject_fail, bcmjob_dev_inject_fail) */
void jmodel_fail_next(uint32_t op, uint32_t n);
void jmodel_forgive(uint32_t op);
/* Accounting: the state now, ms spent in each, cold starts, jobs run */
enum jmodel_state jmodel_state(void);
uint32_t jmodel_state_ms(enum jmodel_state s);
uint32_t jmodel_cold_starts(void);
uint32_t jmodel_jobs_done(void);
/* A BCM-timed animation running now (its tag), or -1 */
int jmodel_anim_tag(void);
/* Frames the last animation drew (ANIM_STOP's reply on the BCM) */
uint32_t jmodel_anim_frames(void);
/* The job block's J_CUR_OP, decoded: the op running now, 0 between jobs */
uint32_t jmodel_cur_op(void);
/* The executor underneath (tests read its surfaces and slots) */
struct jref *jmodel_ref(void);
/* The epoch now, and power-ups while dark by reason (0: any), for tests */
uint32_t jmodel_epoch(void);
uint32_t jmodel_dark_ups(enum bcm_want why);

/* Modelled costs, ms (deadline-scaled: well inside jobs.h's J_T_*) */
#define JM_COST_SMALL       1
#define JM_COST_DRAW        2
#define JM_COST_PRESENT     14      /* a full frame on the panel bus */
#define JM_COLD_START_MS    400     /* power-up to the first frame */
#define JM_E2_KB_PER_MS     11      /* engine 2, ~11 MB/s [silicon S37] */
#define JM_RECT_MS          4       /* bcmjob_upload_rect's budget a call (the
                                       driver's BJC_RECT_US) */
#define JM_RING_WORDS       2048    /* the BCM's command ring */
/* DECODE at 150 MHz [estimate: jpeg-spike.md section 7, 215 M
   instructions for a 1.9 MB photo-like cover's panel]: per MB of file,
   and per output; its deadline J_T_DECODE per started MB */
#define JM_DECODE_MS_PER_MB 750
#define JM_DECODE_MS_OUT    10
/* bcmjob_read(): port reads, about 1 us a word idle [hypothesis,
   data-paths.md R2] */
#define JM_READ_WORDS_PER_MS 1000

#endif /* BCMFW_JMODEL_H */
