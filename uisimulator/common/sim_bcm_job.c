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


#include "config.h"

#ifdef HAVE_BCM_JOBS_MODEL

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "system.h"
#include "kernel.h"
#include "lcd.h"
#include "core_alloc.h"
#include "bcmjob_pp.h"
#include "bcmjob.h"
#include "jmodel.h"
#include "sim_bcm_job.h"

/* The host model of the display job interface, for the shell simulator:
 * bcmfw/jobs/jmodel.c compiled into this file under other names, and the
 * real bcmjob.h entry points as wrappers that first bring the model's
 * clock and lit state up to the simulator's.  The model's surfaces and
 * pools are host memory (BCM memory); only the resident image's
 * reservation (BCM_RESIDENT_BYTES) is PP memory, for the floor gate. */

extern bool sim_lcd_sleeping(void);

/* function-like: "struct bcmjob_slot" must keep its name */
#define bcmjob_have(...) jm_bcmjob_have(__VA_ARGS__)
#define bcmjob_submit(...) jm_bcmjob_submit(__VA_ARGS__)
#define bcmjob_fence(...) jm_bcmjob_fence(__VA_ARGS__)
#define bcmjob_wait(...) jm_bcmjob_wait(__VA_ARGS__)
#define bcmjob_slot(...) jm_bcmjob_slot(__VA_ARGS__)
#define bcmjob_upload(...) jm_bcmjob_upload(__VA_ARGS__)
#define bcmjob_resync(...) jm_bcmjob_resync(__VA_ARGS__)
#define bcmjob_suspend(...) jm_bcmjob_suspend(__VA_ARGS__)
#define bcmjob_epoch(...) jm_bcmjob_epoch(__VA_ARGS__)
#define bcmjob_on_epoch(...) jm_bcmjob_on_epoch(__VA_ARGS__)
#define bcm_port_lock(...) jm_bcm_port_lock(__VA_ARGS__)
#define bcm_port_unlock(...) jm_bcm_port_unlock(__VA_ARGS__)
#define bcm_power_want(...) jm_bcm_power_want(__VA_ARGS__)
#define bcm_power_release(...) jm_bcm_power_release(__VA_ARGS__)
#define bcmjob_dark_ups(...) jm_bcmjob_dark_ups(__VA_ARGS__)
#define bcmjob_errors(...) jm_bcmjob_errors(__VA_ARGS__)
#define bcmjob_decode(...) jm_bcmjob_decode(__VA_ARGS__)
#define bcmjob_read(...) jm_bcmjob_read(__VA_ARGS__)
#define bcmjob_upload_rect(...) jm_bcmjob_upload_rect(__VA_ARGS__)

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wmissing-prototypes"
#pragma GCC diagnostic ignored "-Wmissing-declarations"
#include "jmodel.c"
#pragma GCC diagnostic pop

#undef bcmjob_have
#undef bcmjob_submit
#undef bcmjob_fence
#undef bcmjob_wait
#undef bcmjob_slot
#undef bcmjob_upload
#undef bcmjob_resync
#undef bcmjob_suspend
#undef bcmjob_epoch
#undef bcmjob_on_epoch
#undef bcm_port_lock
#undef bcm_port_unlock
#undef bcm_power_want
#undef bcm_power_release
#undef bcmjob_dark_ups
#undef bcmjob_errors
#undef bcmjob_decode
#undef bcmjob_read
#undef bcmjob_upload_rect

static bool model_up, lit_known, lit_now;
static long tick0;                  /* current_tick at the model's start */
static void (*trace_fn)(const char *line);
static void *mem[J_CLASSES];
static uint16_t *surf[3];

/* Model time follows the sim's clock (10 ms per tick); a wait inside the
 * model may have run it ahead, then the sync does nothing until the sim
 * catches up.  One sync moves at most an hour. */
static void sync_model(void)
{
    uint32_t target, now;
    bool lit;

    if (!model_up)
        return;
    target = (uint32_t)(current_tick - tick0) * 10;
    now = jmodel_now();
    lit = !sim_lcd_sleeping();
    if (target > now)
    {
        uint32_t d = target - now;

        jmodel_advance(d > 3600000 ? 3600000 : d);
    }
    if (!lit_known || lit != lit_now)
    {
        lit_known = true;
        lit_now = lit;
        jmodel_set_lit(lit);
    }
}

#define CALL(call) do { sync_model(); return call; } while (0)

/* P19 19-6: the merged image's DECODE (album-render.md 5.3; the device's
 * load: bcmjob-dev.c bcmjob_ovl_read() and _put(), bcmjob-core.c
 * bjc_ovl_*()).  The image offers OVERLAY and lacks DECODE: dec-ovl.bin,
 * read by the art thread and put piece by piece on the job owner's
 * pass, adds it until the epoch
 * ends (a cut, a restart: loaded again at the next ask), and each decode
 * then borrows a J_CLS_DRIFT work slot (J_CAP_DEC_LENT), freed behind
 * it.  The model holds the same gate by default (sitting 3: the shell
 * decoded on the PP because nothing loaded the overlay before the panel's
 * decode, and the model, offering DECODE at once, never showed it).
 * "bcm_decode ovl off": the display-only image's way (DECODE resident, no
 * slot lent); "bcm_decode ovl refuse": the file missing (why=nofile, a
 * line once a boot; still due, as the device's loader) */
static int ovl_mode = SIM_BCM_OVL_ON;
static bool ovl_up;                 /* loaded in ovl_epoch */
static uint32_t ovl_epoch, ovl_try_epoch;
static int ovl_tries;
static bool ovl_missing;
static volatile bool dec_running;   /* a DECODE in the model now */
static bool viz_gated(uint32_t op);

static bool ovl_loaded(void)
{
    return ovl_up && ovl_epoch == jm_bcmjob_epoch();
}

/* DECODE not offered: the overlay not loaded in this epoch */
static bool dec_gated(void)
{
    return ovl_mode != SIM_BCM_OVL_OFF && !ovl_loaded();
}

static void sim_trace(const char *fmt, ...)
{
    char line[128];
    va_list ap;

    if (!trace_fn)
        return;
    va_start(ap, fmt);
    vsnprintf(line, sizeof(line), fmt, ap);
    va_end(ap);
    trace_fn(line);
}

bool bcmjob_have(uint32_t op)
{
    if (!model_up)
        return false;
    if (op == J_OP_DECODE && dec_gated())
        return false;
    if (viz_gated(op))
        return false;
    CALL(jm_bcmjob_have(op));
}

int bcmjob_submit(uint32_t op, uint32_t tag, const uint32_t *payload,
                  uint32_t n)
{
    if (!model_up)
        return BCMJOB_OFF;
    if (viz_gated(op))
    {
        /* the overlay's op before its load: the device's driver answers
           NOJOB before any traffic (a caller that skipped the ask) */
        sim_trace("bcmjob fault op=0x%02lx why=nojob", (unsigned long)op);
        return BCMJOB_NOJOB;
    }
    CALL(jm_bcmjob_submit(op, tag, payload, n));
}

int bcmjob_fence(uint32_t *seq)
{
    if (!model_up)
        return BCMJOB_OFF;
    CALL(jm_bcmjob_fence(seq));
}

int bcmjob_wait(uint32_t seq, uint32_t ms)
{
    if (!model_up)
        return BCMJOB_OFF;
    CALL(jm_bcmjob_wait(seq, ms));
}

int bcmjob_slot(uint32_t cls, uint32_t w, uint32_t h, uint32_t key_lo,
                uint32_t key_hi, struct bcmjob_slot *out)
{
    if (!model_up)
        return BCMJOB_OFF;
    CALL(jm_bcmjob_slot(cls, w, h, key_lo, key_hi, out));
}

int bcmjob_upload(const struct bcmjob_slot *s, const uint16_t *px)
{
    if (!model_up)
        return BCMJOB_OFF;
    CALL(jm_bcmjob_upload(s, px));
}

int bcmjob_resync(uint32_t x, uint32_t y, uint32_t w, uint32_t h)
{
    if (!model_up)
        return BCMJOB_OFF;
    CALL(jm_bcmjob_resync(x, y, w, h));
}

void bcmjob_suspend(bool on)
{
    if (!model_up)
        return;
    sync_model();
    jm_bcmjob_suspend(on);
}

uint32_t bcmjob_epoch(void)
{
    if (!model_up)
        return 0;
    CALL(jm_bcmjob_epoch());
}

void bcmjob_on_epoch(void (*fn)(uint32_t epoch))
{
    if (model_up)
        jm_bcmjob_on_epoch(fn);
}

void bcm_port_lock(void)
{
}

void bcm_port_unlock(void)
{
}

int bcm_power_want(enum bcm_want why, uint32_t hold_ms)
{
    if (!model_up)
        return BCMJOB_OFF;
    CALL(jm_bcm_power_want(why, hold_ms));
}

void bcm_power_release(enum bcm_want why)
{
    if (!model_up)
        return;
    sync_model();
    jm_bcm_power_release(why);
}

uint32_t bcmjob_dark_ups(enum bcm_want why)
{
    if (!model_up)
        return 0;
    CALL(jm_bcmjob_dark_ups(why));
}

/* P19 19-6: the model's DECODE (bcmjob.h), its clock and lit state first.
 * The device splits a decode between the art thread (file reads) and the
 * ring owner (every wire step, budgeted by the frame-due hook); the model
 * needs no split: it is one host data structure with no port, it yields
 * nowhere but in the caller's own fetch and cancel hooks (which touch only
 * the caller's state), and the simulator's threads run one at a time, so
 * a call from any thread sees the model whole and answers as the device
 * does.  (The simulator's model offers no DECODE today: NOJOB, the CPU
 * path.)  The owner's budget is the device's alone (the hook is kept,
 * unused). */
static bool (*frame_due_fn)(void);

/* P19 19-6: the model's DECODE (off until a script asks: bcm_decode on)
 * runs jdec itself (uisimulator.make's sim_jdec.o, the display-only
 * image's decoder) over the file the shell streamed through the ring, in
 * the image's work area.  The model's decode runs whole in one call, in
 * modelled time; "bcm_decode pace N" makes it take real time too (N
 * ticks slept for each of its ms, while nobody has said stop), so a
 * script can plug USB in the middle of one.  A stop (the shell's cancel hook, or
 * bcmjob_decode_stop() as the device's owner calls it on USB and a
 * power-off) ends the pacing: the rest runs at once, as the device's
 * driver releases the art thread at once. */
static unsigned dec_pace;
static volatile bool dec_stopped;
static bool (*dec_cancel)(void *ctx);
static void *dec_ctx;

void bcmjob_decode_stop(void)
{
    dec_stopped = true;
}

#ifdef HAVE_BCMFW_VIDEO
static void *dec_work;

static int sim_jdec(const uint8_t *jpg, uint32_t len,
                    const struct jdec_out *out, int n, struct jdec_info *info)
{
    struct jdec_src src;
    struct jdec_mem mm;

    if (!dec_work && !(dec_work = malloc(bcm_dec_work())))
        return JDEC_E_NOMEM;
    jdec_mem_src(&src, &mm, jpg, len);
    return jdec_run(&src, out, n, dec_work, bcm_dec_work(), info);
}
#endif

bool sim_bcm_decode(bool on, unsigned pace)
{
    if (!model_up)
        return false;
    dec_pace = pace;
#ifdef HAVE_BCMFW_VIDEO
    m.cfg.decode = on ? sim_jdec : NULL;
    m.ref.decode = on ? model_decode : NULL;
    return true;
#else
    return !on;                 /* no decoder linked */
#endif
}

static bool paced_cancel(void *ctx)
{
    (void)ctx;
    if (dec_stopped || (dec_cancel && dec_cancel(dec_ctx)))
    {
        dec_stopped = true;
        return true;
    }
    if (dec_pace)
        sleep(dec_pace);        /* real time passes: other threads run */
    return false;
}

void bcmjob_frame_due_hook(bool (*fn)(void))
{
    frame_due_fn = fn;
}

int bcmjob_decode(const struct bcmjob_dec_req *req, struct bcmjob_dec_res *res)
{
    struct bcmjob_dec_req q;
    struct bcmjob_slot ws;
    uint32_t ep;
    bool lent;
    int r;

    if (!model_up)
    {
        memset(res, 0, sizeof(*res));
        return BCMJOB_OFF;
    }
    sync_model();
    /* the merged image without its overlay: DECODE unknown, as the
     * device's driver answers it before any traffic */
    if (jm_bcmjob_have(J_OP_DECODE) && dec_gated())
    {
        memset(res, 0, sizeof(*res));
        sim_trace("bcmjob fault op=0x%02x why=nojob", J_OP_DECODE);
        return BCMJOB_NOJOB;
    }
    /* the lent work slot (J_CAP_DEC_LENT): the set within it, the slot
     * ALLOCed before the DECODE (the device: after the outputs; the shell
     * frees the pool for both first), FREEd behind it whatever the answer */
    lent = ovl_mode != SIM_BCM_OVL_OFF && jm_bcmjob_have(J_OP_DECODE);
    ep = jm_bcmjob_epoch();
    if (lent && req->n >= 1
        && bcmjob_dec_work(req->out, req->n) > J_DEC_WORK_LENT)
    {
        memset(res, 0, sizeof(*res));
        res->err = HP_E_NOMEM;
        return BCMJOB_FAILED;
    }
    if (lent && jm_bcmjob_slot(J_CLS_DRIFT, J_DEC_WORK_SIDE, J_DEC_WORK_SIDE,
                               J_DEC_WORK_KEY, 0, &ws) != BCMJOB_OK)
    {
        memset(res, 0, sizeof(*res));
        sim_trace("bcmjob decode why=nowork");  /* the pool was full */
        res->err = J_E_NOSLOT;
        return BCMJOB_FAILED;
    }
    q = *req;
    dec_cancel = req->cancel;
    dec_ctx = req->ctx;
    dec_stopped = false;
    q.cancel = paced_cancel;
    dec_running = true;
    r = jm_bcmjob_decode(&q, res);
    dec_running = false;
    dec_cancel = NULL;
    if (lent && !ws.hit && jm_bcmjob_epoch() == ep)
    {
        uint32_t p[1] = { ws.slot };

        jm_bcmjob_submit(J_OP_FREE, 0, p, 1);
    }
    return r;
}

/* P19 19-4: the model's upload into BACK (bcmjob.h rev 0.4) */
int bcmjob_upload_rect(uint32_t surf, uint32_t x, uint32_t y, uint32_t w,
                       uint32_t h, const uint16_t *px, uint32_t pitch)
{
    if (!model_up)
        return BCMJOB_OFF;
    CALL(jm_bcmjob_upload_rect(surf, x, y, w, h, px, pitch));
}

int bcmjob_read(uint32_t slot, uint32_t x, uint32_t y, uint32_t w, uint32_t h,
                uint16_t *px, uint32_t pitch)
{
    if (!model_up)
        return BCMJOB_OFF;
    CALL(jm_bcmjob_read(slot, x, y, w, h, px, pitch));
}

/* bcmjob_pp.h: the model has no owner thread (its ring steps inline) */
void bcmjob_claim_owner(void)
{
}

void bcmjob_init(void (*trace)(const char *line))
{
    static const struct { uint32_t cls, bytes; } pools[] = {
        { J_CLS_DRIFT, J_BUDGET_DRIFT }, { J_CLS_STRIP, J_BUDGET_STRIP },
        { J_CLS_THUMB, J_BUDGET_THUMB }, { J_CLS_SCRATCH, J_BUDGET_SCRATCH },
        { J_CLS_APP, J_BUDGET_APP },
    };
    struct jmodel_cfg c;
    size_t fbn = (size_t)LCD_WIDTH * LCD_HEIGHT * sizeof(uint16_t);
    unsigned i;
    int h;

    if (model_up)
        return;
    trace_fn = trace;
    /* the resident image's PP copy: pinned, kept for good */
    h = core_alloc(BCM_RESIDENT_BYTES);
    if (h <= 0)
        return;
    core_pin(h);
    if (!bcmjob_cpu_init())
        return;
    memset(&c, 0, sizeof(c));
    for (i = 0; i < 3; i++)
    {
        surf[i] = calloc(1, fbn);
        if (!surf[i])
            return;
    }
    for (i = 0; i < sizeof(pools) / sizeof(pools[0]); i++)
    {
        mem[pools[i].cls] = calloc(1, pools[i].bytes);
        if (!mem[pools[i].cls])
            return;
        c.pool_mem[pools[i].cls] = mem[pools[i].cls];
        c.pool_bytes[pools[i].cls] = pools[i].bytes;
    }
    c.front = (struct jref_surf){ surf[0], LCD_WIDTH, LCD_HEIGHT, LCD_WIDTH };
    c.back = (struct jref_surf){ surf[1], LCD_WIDTH, LCD_HEIGHT, LCD_WIDTH };
    c.panel = (struct jref_surf){ surf[2], LCD_WIDTH, LCD_HEIGHT, LCD_WIDTH };
    c.trace = trace;
    c.pp_fb = (const uint16_t *)FBADDR(0, 0);
    jmodel_init(&c);
    tick0 = current_tick;
    model_up = true;
    lit_known = true;
    lit_now = !sim_lcd_sleeping();
    jmodel_set_lit(lit_now);
    if (trace)
    {
        char line[64];

        snprintf(line, sizeof(line),
                 "bcmjob init model resident=%u state=%u",
                 (unsigned)BCM_RESIDENT_BYTES, (unsigned)sizeof(struct jref));
        trace(line);
    }
}

int bcmjob_selftest(void)
{
    static const uint16_t colour = 0x07E0;
    uint32_t p[4] = { J_SURF_BACK, 0, J_XY(32, 16), colour }, seq;
    const struct jref_surf *b;
    uint32_t x, y;
    int r;

    if (!bcmjob_have(J_OP_FILL))
    {
        if (trace_fn)
            trace_fn("bcmjob selftest unavailable");
        return 1;
    }
    r = bcmjob_submit(J_OP_FILL, 0, p, 4);
    if (r == BCMJOB_OK)
        r = bcmjob_fence(&seq);
    if (r == BCMJOB_OK)
        r = bcmjob_wait(seq, J_T_FENCE_BASE + J_T_DRAW);
    if (r == BCMJOB_OK)
    {
        b = &jmodel_ref()->back;
        for (y = 0; y < 16 && r == BCMJOB_OK; y++)
            for (x = 0; x < 32; x++)
                if (b->px[y * b->pitch + x] != colour)
                {
                    r = BCMJOB_FAILED;
                    break;
                }
    }
    if (trace_fn)
    {
        char line[48];

        if (r == BCMJOB_OK)
            trace_fn("bcmjob selftest ok");
        else
        {
            snprintf(line, sizeof(line), "bcmjob selftest failed rc=%d", r);
            trace_fn(line);
        }
    }
    return r == BCMJOB_OK ? 0 : -1;
}

/* Screen truth in the shots (19-2): the rect the BCM owns
 * (bcmjob_owned_get()) is the model's panel on the glass, not the PP
 * framebuffer.  A shot shows the glass: on, the framebuffer's rect is
 * saved and the model's panel put there; off, it is put back.  The
 * script thread runs alone (the sim's threads are cooperative). */
static uint16_t *glass_save;
static int gx, gy, gw, gh;

/* pixels of the poison pattern left in what a shot shows (a pair of
 * the checkerboard's colours side by side); 0 when the model is off */
static unsigned long poison_px(void)
{
    unsigned long n = 0;
    int x, y;

    for (y = 0; y < LCD_HEIGHT; y++)
    {
        const uint16_t *fb = (const uint16_t *)FBADDR(0, y);

        for (x = 0; x + 1 < LCD_WIDTH; x++)
            n += fb[x] == BCMJOB_POISON(x, y)
                 && fb[x + 1] == BCMJOB_POISON(x + 1, y);
    }
    return n;
}

static void glass(bool on)
{
    const struct jref_surf *p;
    int r;

    if (on)
    {
        if (!model_up || glass_save || !bcmjob_owned_get(&gx, &gy, &gw, &gh))
            return;
        if (gx < 0 || gy < 0 || gx + gw > LCD_WIDTH || gy + gh > LCD_HEIGHT)
            return;
        sync_model();           /* the queued frame is on the panel */
        /* screen truth's check: a framebuffer write into the owned rect
         * that skipped ui_fb_settle().  Parity: the CPU drew each frame
         * too, so the glass must equal it; on (the device's behaviour):
         * the rect must still be all poison */
        {
            unsigned long n = 0;
            int x;

            if (bcmjob_drift_mode() == BCMJOB_DRIFT_PARITY)
                n = bcmjob_model_diff(gx, gy, gw, gh);
            else
                for (r = 0; r < gh; r++)
                {
                    const uint16_t *fb = (const uint16_t *)FBADDR(gx, gy + r);

                    for (x = 0; x < gw; x++)
                        n += fb[x] != BCMJOB_POISON(gx + x, gy + r);
                }
            printf("sim_script: glass owned x=%d w=%d diff=%lu\n", gx, gw, n);
            if (n)
                printf("sim_script: glass owned STALE px=%lu (a write "
                       "that skipped the settle)\n", n);
        }
        glass_save = malloc((size_t)gw * gh * sizeof(uint16_t));
        if (!glass_save)
            return;
        p = &jmodel_ref()->panel;
        for (r = 0; r < gh; r++)
        {
            uint16_t *fb = (uint16_t *)FBADDR(gx, gy + r);

            memcpy(glass_save + r * gw, fb, gw * sizeof(uint16_t));
            memcpy(fb, p->px + (gy + r) * p->pitch + gx,
                   gw * sizeof(uint16_t));
        }
        return;     /* the poison check: below, after any compose */
    }
    if (!glass_save)
        return;
    for (r = 0; r < gh; r++)
        memcpy(FBADDR(gx, gy + r), glass_save + r * gw,
               gw * sizeof(uint16_t));
    free(glass_save);
    glass_save = NULL;
}

uint32_t bcmjob_power_gen(void)
{
    if (!model_up)
        return 0;
    sync_model();
    return jmodel_state() == JM_C0 ? 0 : jmodel_cold_starts();
}

/* the model runs this tree's definition (jobs.h J_MINOR); a script may
 * stand in an older image (bcm_minor) */
static uint32_t sim_minor = J_MINOR;

uint32_t bcmjob_minor(void)
{
    return model_up && jm_bcmjob_have(J_OP_FENCE) ? sim_minor : 0;
}

void sim_bcm_minor(uint32_t minor)
{
    sim_minor = minor;
}

/* bcmjob_pp.h: the overlays, as the device's loader (bcmjob-dev.c
 * bcmjob_ovl_*, bcmjob-core.c bjc_ovl_*).  The kinds' state: up (loaded
 * in epoch), the epoch of the last try and its tries, the file missing
 * said.  DECODE's gate is above (ovl_*); the visualizer's here: "bcm_viz
 * ovl on" (the default: the merged image, and the display-only one, take
 * SPECTRUM, BARS and the line ops only from viz-ovl-m.bin or viz-ovl.bin:
 * the model offers none of them until the shell has loaded it in this
 * epoch), "off" (the model's ops resident: the BCMFW_VIZ image's way),
 * "refuse" (no file: why=nofile).  The merged image's two overlays share
 * one region: a load evicts the other kind (its ops gone, its load due
 * again at once).  A load's first piece is refused BUSY while a DECODE
 * holds the BCM (the device's dec_holds()), no try spent. */
static int viz_mode = SIM_BCM_OVL_ON;
static bool viz_up, viz_missing;
static uint32_t viz_epoch, viz_try_epoch;
static int viz_tries;

static bool viz_loaded(void)
{
    return viz_up && viz_epoch == jm_bcmjob_epoch();
}

static bool viz_op(uint32_t op)
{
    return op == J_OP_SPECTRUM || op == J_OP_BARS || op == J_OP_POLYLINE
           || op == J_OP_SEGMENTS || op == J_OP_PALROW;
}

/* a visualizer op not offered: its overlay not loaded in this epoch */
static bool viz_gated(uint32_t op)
{
    return viz_op(op) && viz_mode != SIM_BCM_OVL_OFF && !viz_loaded();
}

static bool sim_ovl_due(int kind)
{
    uint32_t ep;

    if (!model_up)
        return false;
    sync_model();
    ep = jm_bcmjob_epoch();
    if (kind == BCMJOB_OVL_DEC)
        return ovl_mode != SIM_BCM_OVL_OFF && jm_bcmjob_have(J_OP_DECODE)
               && !ovl_loaded() && (ovl_try_epoch != ep || ovl_tries < 2);
    return viz_mode != SIM_BCM_OVL_OFF && jm_bcmjob_have(J_OP_SPECTRUM)
           && !viz_loaded() && (viz_try_epoch != ep || viz_tries < 2);
}

bool bcmjob_ovl_due(int kind, struct bcmjob_ovl_file *f)
{
    if (!f || !f->bytes || (kind != BCMJOB_OVL_DEC && kind != BCMJOB_OVL_VIZ))
        return false;
    return sim_ovl_due(kind);
}

/* the file's side (the art thread): the model has no file, so a piece is
 * a pattern (a caller whose buffer was someone else's shows it); a mode
 * "refuse": no file (why=nofile, a line once a boot; still due, as the
 * device's loader) */
int bcmjob_ovl_read(int kind, void *buf, uint32_t off, uint32_t n, uint32_t *size,
                    const struct bcmjob_ovl_file *f)
{
    bool *missing = kind == BCMJOB_OVL_DEC ? &ovl_missing : &viz_missing;

    if (!model_up || !f || !f->bytes || !buf || ((uintptr_t)buf & 3) ||
        (n & 3))
        return -1;
    if ((kind == BCMJOB_OVL_DEC ? ovl_mode : viz_mode) == SIM_BCM_OVL_REFUSE)
    {
        if (!*missing)
            sim_trace("bcmjob overlay refused why=nofile");
        *missing = true;
        return -1;
    }
    *size = f->bytes;
    if (off >= f->bytes)
        return -1;
    n = MIN(n, f->bytes - off);
    memset(buf, 0xA5, n);
    return (int)n;
}

/* ... the owner's side: the first piece starts the load (a try spent,
 * the other kind evicted), the last offers the kind's ops.  The device's
 * posted waits (bcmjob-core.h BJC_OVL_AGAIN, sitting 4c: no UI pass
 * blocks on a load) are modelled too: the first piece answers
 * BCMJOB_OVL_AGAIN once (its FENCE, SIM_OVL_FENCE ticks), the last for
 * SIM_OVL_END ticks (OVERLAY's checks, about 30 ms on the device), so the
 * shell's passes ask again as on the device; bcmjob_ovl_poll() looks for
 * a withdrawn last piece's answer */
#define SIM_OVL_FENCE   1
#define SIM_OVL_END     3
static uint32_t ovl_next;
static int ovl_next_kind;
static int ovl_pend;                /* 0, 1 the first piece's, 2 the last's */
static int ovl_pend_kind;
static long ovl_pend_t0;
static bool sim_ovl_have(int kind);

static int sim_ovl_end(int kind)
{
    if (ovl_pend != 2)
    {
        ovl_pend = 2;
        ovl_pend_kind = kind;
        ovl_pend_t0 = current_tick;
    }
    if (TIME_BEFORE(current_tick, ovl_pend_t0 + SIM_OVL_END))
        return BCMJOB_OVL_AGAIN;
    ovl_pend = 0;
    if (jm_bcmjob_epoch() != (kind == BCMJOB_OVL_DEC ? ovl_try_epoch
                                                     : viz_try_epoch))
    {
        sim_trace("bcmjob overlay refused why=wire rc=%d", BCMJOB_OFF);
        return BCMJOB_OFF;          /* a cut meanwhile: the epoch's gone */
    }
    if (kind == BCMJOB_OVL_DEC)
    {
        ovl_up = true;
        ovl_epoch = jm_bcmjob_epoch();
        sim_trace("bcmjob overlay ok k=1 ops=%08lx vpu=0x27 mac=vector "
                  "yuv=vector lent=1", 0x0021ffffUL);
    }
    else
    {
        viz_up = true;
        viz_epoch = jm_bcmjob_epoch();
        sim_trace("bcmjob overlay ok ops=%08lx vpu=0x3f fft=vector "
                  "bars=vector flip lines=PSR", 0x01e3bfffUL);
    }
    return sim_ovl_have(kind) ? BCMJOB_OK : BCMJOB_NOJOB;
}

int bcmjob_ovl_poll(void)
{
    if (ovl_pend == 2)
        return sim_ovl_end(ovl_pend_kind);
    if (ovl_pend == 1)
    {
        ovl_pend = 0;
        return BCMJOB_OFF;
    }
    return BCMJOB_NOJOB;
}

static bool sim_ovl_have(int kind)
{
    return kind == BCMJOB_OVL_DEC ? bcmjob_have(J_OP_DECODE)
                                  : bcmjob_have(J_OP_SPECTRUM) &&
                                    bcmjob_have(J_OP_BARS);
}

int bcmjob_ovl_put(int kind, const void *buf, uint32_t off, uint32_t n,
                   uint32_t size, const struct bcmjob_ovl_file *f)
{
    uint32_t ep;

    if (!buf || ((uintptr_t)buf & 3) || (n & 3) || !n || !f || !f->bytes ||
        (kind != BCMJOB_OVL_DEC && kind != BCMJOB_OVL_VIZ))
        return BCMJOB_OFF;
    if (ovl_pend == 2 && ovl_pend_kind == kind && off + n == size &&
        ovl_next == size)
        return sim_ovl_end(kind);   /* the last piece's answer */
    if (ovl_pend == 1 && ovl_pend_kind == kind && off == 0)
    {
        if (TIME_BEFORE(current_tick, ovl_pend_t0 + SIM_OVL_FENCE))
            return BCMJOB_OVL_AGAIN;
        ovl_pend = 0;               /* its FENCE retired: the load's */
        if (jm_bcmjob_epoch() != (kind == BCMJOB_OVL_DEC ? ovl_try_epoch
                                                         : viz_try_epoch))
            return BCMJOB_OFF;      /* a cut meanwhile */
        sim_trace("bcmjob overlay load k=%d bytes=%lu at=%08lx epoch=%lu",
                  kind, (unsigned long)f->bytes, (unsigned long)f->base,
                  (unsigned long)jm_bcmjob_epoch());
        ovl_next = 0;
        ovl_next_kind = kind;
        goto piece;
    }
    ovl_pend = 0;                   /* another load: the posted wait ends */
    if (sim_ovl_have(kind))
        return BCMJOB_OK;
    if (off == 0)
    {
        if (!sim_ovl_due(kind))
            return BCMJOB_NOJOB;
        if (dec_running)
        {
            sim_trace("bcmjob fault op=0x%02x why=busy-decode", J_OP_OVERLAY);
            return BCMJOB_BUSY;
        }
        ep = jm_bcmjob_epoch();
        if (kind == BCMJOB_OVL_DEC)
        {
            if (ovl_try_epoch != ep)
                ovl_tries = 0;
            ovl_try_epoch = ep;
            ovl_tries = 2;          /* loaded: none left this epoch */
            if (viz_loaded())
            {
                viz_up = false;     /* the region rewritten: due again */
                viz_try_epoch = 0;
                sim_trace("bcmjob overlay evict k=0 by=1");
            }
        }
        else
        {
            if (viz_try_epoch != ep)
                viz_tries = 0;
            viz_try_epoch = ep;
            viz_tries = 2;
            if (ovl_loaded())
            {
                ovl_up = false;
                ovl_try_epoch = 0;
                sim_trace("bcmjob overlay evict k=1 by=0");
            }
        }
        ovl_pend = 1;               /* the FENCE: answered at a later pass */
        ovl_pend_kind = kind;
        ovl_pend_t0 = current_tick;
        return BCMJOB_OVL_AGAIN;
    }
piece:
    if (off != ovl_next || kind != ovl_next_kind || off + n > size ||
        size != f->bytes)
    {
        sim_trace("bcmjob overlay refused why=read");
        return BCMJOB_FAILED;
    }
    ovl_next = off + n;
    if (ovl_next < size)
        return BCMJOB_OK;
    return sim_ovl_end(kind);
}

bool sim_bcm_viz_ovl(int mode)
{
    if (mode < SIM_BCM_OVL_OFF || mode > SIM_BCM_OVL_REFUSE)
        return false;
    viz_mode = mode;
    viz_missing = false;
    return true;
}

bool bcmjob_dec_lends(void)
{
    return model_up && ovl_mode != SIM_BCM_OVL_OFF && ovl_loaded()
           && bcmjob_have(J_OP_DECODE);
}

bool sim_bcm_dec_ovl(int mode)
{
    if (mode < SIM_BCM_OVL_OFF || mode > SIM_BCM_OVL_REFUSE)
        return false;
    ovl_mode = mode;
    ovl_missing = false;
    return true;
}

/* the model runs this tree's jref_viz.c: J_BARS_FLIP with BARS; a script
 * may stand in an overlay before the flag (sim_bcm_bars_flip) */
static bool sim_flip = true;

bool bcmjob_bars_flip(void)
{
    return sim_flip && bcmjob_have(J_OP_BARS);
}

void sim_bcm_bars_flip(bool on)
{
    sim_flip = on;
}

uint32_t bcmjob_errors(void)
{
    if (!model_up)
        return 0;
    CALL(jm_bcmjob_errors());
}

uint32_t bcmjob_model_diff(int x, int y, int w, int h)
{
    const struct jref_surf *p;
    uint32_t n = 0;
    int r, c;

    if (!model_up || x < 0 || y < 0 || x + w > LCD_WIDTH || y + h > LCD_HEIGHT)
        return 0;
    sync_model();
    p = &jmodel_ref()->panel;
    for (r = y; r < y + h; r++)
    {
        const uint16_t *fb = (const uint16_t *)FBADDR(x, r);
        const uint16_t *m = p->px + r * p->pitch + x;

        for (c = 0; c < w; c++)
            n += fb[c] != m[c];
    }
    return n;
}

void sim_bcm_glass(bool on)
{
    unsigned long n;

    glass(on);
    if (!on || !model_up)
        return;
    n = poison_px();
    if (n)
        printf("sim_script: shot POISONED px=%lu (the framebuffer's copy "
               "of a BCM-owned rect reached the screen)\n", n);
}

bool sim_bcm_ready(void)
{
    return model_up;
}

bool sim_bcm_stats(struct sim_bcm_stats *o)
{
    unsigned i;

    if (!model_up)
        return false;
    sync_model();
    for (i = 0; i < 3; i++)
        o->state_ms[i] = jmodel_state_ms((enum jmodel_state)i);
    o->cold_starts = jmodel_cold_starts();
    o->jobs_done = jmodel_jobs_done();
    o->epoch = jm_bcmjob_epoch();
    for (i = 0; i < 4; i++)
        o->dark_ups[i] = jm_bcmjob_dark_ups((enum bcm_want)i);
    o->anim_tag = jmodel_anim_tag();
    return true;
}

bool sim_bcm_fault(int kind, uint32_t op)
{
    if (!model_up || kind < 0 || kind > SIM_BCM_FAULT_NONE)
        return false;
    sync_model();
    jmodel_fault((enum jmodel_fault)kind, op);
    return true;
}

#ifdef HAVE_BCMJOB_INJECT
int bcmjob_dev_inject_fail(uint32_t op, uint32_t n)
{
    if (!model_up)
        return -1;
    sync_model();
    jmodel_fail_next(op, n);
    if (!n)
        jmodel_forgive(op);     /* "off": the op is forgiven too */
    sim_trace("bcmjob inject op=0x%02lx n=%lu", (unsigned long)op,
              (unsigned long)n);
    return 0;
}
#endif

bool sim_bcm_charger(bool on)
{
    if (!model_up)
        return false;
    sync_model();
    jmodel_set_charger(on);
    return true;
}

bool sim_bcm_advance(uint32_t ms)
{
    if (!model_up)
        return false;
    sync_model();
    jmodel_advance(ms);
    return true;
}

#endif /* HAVE_BCM_JOBS_MODEL */
