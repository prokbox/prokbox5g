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

/* The BCM2722 job driver's logic: bcmfw/jobs/bcmjob.h over a host port
 * (bcmjob-core.h has the rules).  No Rockbox headers: the PP build and
 * the host test (tests/bcmjob) compile this same file. */

#include <stdio.h>
#include <string.h>
#include "hostproto.h"
#include "bcmjob-core.h"

#define LCD_CMD_WORD    0x1F8           /* the LCD contract's command word */
#define LCD_FAULT       0xFA170000      /* our image's fault word there */
#define HELLO_US        20000           /* HELLO's reply (a round trip is µs) */
#define BEAT_US         3000            /* the heartbeat moves every 1.024 ms */
#define OPLOG           64              /* ops by job number, for a timeout */
#define HOUR_MS         3600000u
#define BOXES           3               /* replies awaited at once: the
                                           owner's call, a decode's and an
                                           overlay load's end (posted, its
                                           answer polled: bjc_ovl_end) */
#define BOX_WORDS       (1 + J_SPEC_BANDS_MAX / 2)  /* the longest reply
                                           kept: SPECTRUM's 32 levels */
/* the ops an overlay may add (jobs.md section 5, "Overlays"): the
 * visualizer's; HP_E_UNKNOWN for one of them means its overlay is gone */
#define OVL_OPS         (1u << (J_OP_SPECTRUM - 0x20) | 1u << (J_OP_BARS - 0x20))
/* ... and the line ops it may add beside them, each only if its start-up
 * check passed (POLYLINE, SEGMENTS, PALROW): never a reason to reload */
#define OVL_LINE        (1u << (J_OP_POLYLINE - 0x20) | 1u << (J_OP_SEGMENTS - 0x20) | \
                         1u << (J_OP_PALROW - 0x20))
/* the merged image's overlay (album-render.md 5.3): DECODE */
#define OVL_DEC         (1u << (J_OP_DECODE - 0x20))

enum { ST_NONE, ST_JOBS, ST_CPU, ST_OFF };

/* The hand-off's order (P19 S10, the state table at the DECODE section):
 * a store before BJC_FENCE() reaches the other side before a load after
 * it.  The PP5022's cores are in-order ARMv4T, and their uncached
 * accesses go out in program order (Rockbox's corelock, a Peterson lock
 * of plain uncached loads and stores, relies on the same [silicon]), so
 * only the compiler needs holding.  A host has to fence; the host test
 * also makes the fence its schedule point (tests/bcmjob). */
#if defined(BJC_TEST_FENCE)
void bjc_test_fence(void);
#define BJC_FENCE() bjc_test_fence()
#elif defined(__ARM_ARCH_4T__)
#define BJC_FENCE() __asm__ volatile ("" ::: "memory")
#else
#define BJC_FENCE() __sync_synchronize()
#endif
/* The owner's own state below (b, j) and the caller's (ac) each start and
 * end on a cache line: with the owner on the other core, a line shared
 * with the caller's writes would come back stale (xcore.h rule 3) */
#define BJC_OWN_LINES __attribute__((aligned(BJC_LINE)))

struct box;
/* One overlay's load state (bjc_ovl_*; an instance per overlay,
 * BJC_OVL_*): the ops it may add (want: all of them makes it loaded; opt:
 * offered beside them if their checks passed), the ops it added (gone
 * with the epoch), the epoch of the last load tried and its tries, a
 * refusal for the boot, the load under way (the region's base and size,
 * the file's words) and the build's record of the file (size, FNV-1a,
 * ABI word, the image's copy of it, the region it was linked at) */
struct ovl
{
    uint32_t want, opt;
    uint32_t ops, epoch, base, size, words;
    uint8_t tries;
    bool never;
    struct bjc_ovl_rec rec;
    /* the load's two waits, each posted and polled on later calls, never
     * waited for inside one (0.8.0, sitting 4c: a UI pass blocked on
     * them): OVL_PH_FENCE, the FENCE before the region is rewritten
     * (fseq, 0: none needed); OVL_PH_END, OVERLAY's reply (x, its box).
     * t0: the wait's start (us), its bound J_T_* as before */
    uint8_t ph;
    uint32_t fseq, t0;
    uint32_t sent;              /* b.sent once OVERLAY was put */
    struct box *x;
};
enum { OVL_PH_IDLE = 0, OVL_PH_FENCE, OVL_PH_END };

/* A reply awaited by tag.  drain() files it whoever drains, so a call on
 * one thread never loses its reply to another thread's drain (the art
 * thread's bcmjob_decode() beside the owner's calls; the scheduler is
 * cooperative on a core, so each step below runs whole) */
struct box
{
    uint32_t tag;               /* 0: free */
    uint32_t n;                 /* words filed (status first) */
    uint32_t rep[BOX_WORDS];
    bool got;
};

static struct
{
    const struct bjc_port *p;
    struct bjc_slot *slot;
    void (*trace)(const char *line);
    void (*on_epoch)(uint32_t epoch);
    uint32_t gen;               /* the power-up discovery ran for (0 none) */
    uint8_t st;                 /* ST_*: this power-up */
    bool suspended, verbose, epoch_due;
    uint32_t epoch;             /* the driver's own, from 1 */
    uint32_t block;             /* the job block's address */
    uint32_t ops;               /* J_OPS */
    uint8_t minor;              /* J_VERSION's minor (bcmjob_minor) */
    uint8_t vpu;                /* J_VPU's low bits as last read (discovery,
                                   an overlay load): J_VPU_FLIP for
                                   bcmjob_bars_flip() */
    uint16_t caps;              /* J_CAPS' bits (rev 0.5), 0 without the
                                   magic: J_CAP_STAGED asks ALLOC for the
                                   staged upload */
    uint32_t commit[3];         /* the last COMMIT sent: slot, A, B (its
                                   J_E_SUM line beside the BCM's sums) */
    uint32_t slow_once, slow, failed;   /* op bits, this boot */
    uint8_t fail_n[32];         /* counted failures per op, this boot
                                   (across restarts; bcmjob.h
                                   BCMJOB_FAIL_MARK) */
    uint32_t errs;              /* J_ERRORS last read (bcmjob_errors) */
    struct ovl ovl[BJC_OVL_N];  /* the overlays (bjc_ovl_*) */
    uint32_t region;            /* the image's overlay region (J_OVL's
                                   base; 0: it takes none) */
#ifdef HAVE_BCMFW_DEVTEST
    /* bjc_inject_fail, per op: jobs still to send asking for a reply, and
     * those replies, on their way, to read as failed */
    uint8_t inj_left[32], inj_due[32];
#endif
    uint32_t failures;          /* BCM failures this boot */
    uint32_t cmd_base, cmd_size, evt_base, evt_size;
    uint32_t cmd_wr, cmd_rd, evt_rd;    /* 16-bit ring indices */
    uint32_t seq;               /* the last fence's sequence */
    uint16_t tag;
    uint32_t sent, done_base;   /* jobs sent this power-up; J_DONE then */
    uint8_t oplog[OPLOG];
    uint8_t commit_slot;        /* the last COMMIT's slot + 1 (its fault
                                   trace; 0 none) */
    /* power accounting (power-contract.md, G11, G12); the dark record
     * itself is the backlight thread's (struct bjc_req) */
    uint32_t hour_next;
    uint32_t wants, want_until[BCM_WANT_N], ups[BCM_WANT_N];
    /* job mode (lcd_tick's hang check is the heartbeat's meanwhile):
     * on only while jobs are in flight, so a hang between job batches is
     * still the tick's to find (S0 review M2) */
    bool inflight;              /* records put and not yet seen done */
    bool put_after_fence;       /* a record put after the last fence */
    uint32_t fence_last;        /* the last fence's sequence put */
    bool jobs_mode;             /* the last mode told to own->mode */
    uint32_t mode_gen;          /* ... and the power-up it was told for */
    struct box box[BOXES];
    /* bcmjob_decode(): the input ring's slot this epoch */
    uint32_t ring_slot, ring_epoch;     /* ring_epoch 0: none yet */
    /* the DECODE record on the ring whose reply hasn't come (its tag, 0
       none), and after a timeout how long it may still hold the BCM */
    uint32_t dec_live, dec_live_until;
    /* the DECODE until its reply is drained (past a timeout too), and the
       jobs sent before it: resync waits for those only */
    uint32_t dec_pend, dec_pos;
    uint32_t stop_seq;          /* bjc_decode_stop(): calls up to it end */
    /* FREEs a full ring refused, put again at the owner's next passes */
    uint32_t fpend[2 * J_DEC_MAX], nfpend, fpend_epoch;
    bool (*frame_due)(void);    /* the owner's budget (bjc_frame_due) */
    bool in_pass;               /* own_pass() is running (no re-entry) */
} BJC_OWN_LINES b;

/* The decode caller's own state (the art thread's, on its own lines;
 * P19 S10, the state table at the DECODE section) */
static struct
{
    uint32_t leak_seq;          /* a call given up on and never
                                   acknowledged (LEAKED; 0 none) */
    bool leak_buf;              /* ... a decode: its piece buffer kept */
    uint32_t leaks;             /* bjc_leaks() */
} BJC_OWN_LINES ac;

/* P19 S0 (design MF-2): b above is the ring owner's alone.  Other threads
 * (and, after 19-9, the other core) reach it only through the request
 * record: the suspend requester's word and the backlight thread's dark
 * record, each with one writer.  Without bjc_set_owner() every caller is
 * the owner (the host test's default) and the record is this one. */
static struct bjc_req req0;
static struct bjc_req *r = &req0;
static const struct bjc_owner *own;
/* NOTE (S0 review L5; S10): b, r and own are cached statics.  bjc_dark()
 * (the backlight thread), a non-owner bjc_suspend_request() and the
 * decode's caller read b.p, b.trace, r and own, and never write b.  All
 * are written only by bjc_init() and bjc_set_owner(), before any other
 * thread calls in; the device glue writes them back after (bcmjob_init's
 * xcore_publish()) and a new owner on the other core drops its stale
 * lines before its first call (bcmjob_claim_owner's xcore_acquire()).
 * Everything else in b is the owner's alone, on its own cache lines. */

static const char *const want_name[BCM_WANT_N] = { "any", "decode", "game", "video" };

static void trace(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
#include <stdarg.h>
static void trace(const char *fmt, ...)
{
    char line[112];
    va_list ap;

    if (!b.trace)
        return;
    va_start(ap, fmt);
    vsnprintf(line, sizeof(line), fmt, ap);
    va_end(ap);
    b.trace(line);
}

static int fault(uint32_t op, int r)
{
    static const char *const why[] = { "busy", "off", "nojob", "timeout",
                                       "failed", "refused", "aborted" };

    trace("bcmjob fault op=0x%02lx why=%s", (unsigned long)op, why[-r - 1]);
    return r;
}

void bjc_init(const struct bjc_port *port, struct bjc_slot *slots,
              void (*tr)(const char *line))
{
    memset(&b, 0, sizeof(b));
    memset(&ac, 0, sizeof(ac));
    memset(&req0, 0, sizeof(req0));
    r = &req0;
    own = NULL;
    b.p = port;
    b.slot = slots;
    b.ovl[BJC_OVL_VIZ].want = OVL_OPS;
    b.ovl[BJC_OVL_VIZ].opt = OVL_LINE;
    b.ovl[BJC_OVL_DEC].want = OVL_DEC;
    b.trace = tr;
    b.epoch = 1;
    b.hour_next = HOUR_MS;
}

void bjc_verbose(bool on) { b.verbose = on; }

int bjc_state(void) { return b.failures >= 2 ? ST_OFF : b.st; }
#ifdef HAVE_BCMFW_VIDEO
uint32_t bjc_chip_failures(void) { return b.failures; }    /* the player's */
#endif

/* ---- dark accounting ----------------------------------------------------- */

/* the dark record, read whole (a seqlock: its writer is another thread) */
static uint32_t dark_now(void)
{
    uint32_t seq, dark, since, total;

    do
    {
        seq = r->dseq;
        dark = r->dark;
        since = r->since;
        total = r->total;
    }
    while ((seq & 1) || seq != r->dseq);
    return total + (dark ? b.p->now_ms() - since : 0);
}

/* the hourly line, once per hour of dark time (lazily: at the next call) */
static void hours(void)
{
    while (dark_now() >= b.hour_next)
    {
        trace("bcm power ups dark=%lu decode=%lu game=%lu video=%lu",
              (unsigned long)b.ups[0], (unsigned long)b.ups[BCM_WANT_DECODE],
              (unsigned long)b.ups[BCM_WANT_GAME],
              (unsigned long)b.ups[BCM_WANT_VIDEO]);
        b.hour_next += HOUR_MS;
    }
}

/* the backlight thread: the dark record's only writer */
void bjc_dark(bool dark)
{
    uint32_t now;

    if (!b.p || (uint32_t)dark == r->dark)
        return;
    now = b.p->now_ms();
    r->dseq++;                  /* odd: a reader retries */
    if (dark)
        r->since = now;
    else
        r->total += now - r->since;
    r->dark = dark;
    r->dseq++;
}

/* ---- the requests (P19 S0, MF-2) ----------------------------------------- */

static void suspend_apply(bool on);

/* The owner: apply what the requesters asked, acknowledge */
static void apply_requests(void)
{
    uint32_t w = r->want;

    if (w == r->done)
        return;
    suspend_apply(w & 1);
    r->done = w;
    if (own && own->ack)
        own->ack();
}

void bjc_set_owner(const struct bjc_owner *o)
{
    own = o;
    r = o && o->req ? o->req : &req0;
}

/* The owner's pass of a posted decode or read: r->pass, set by the first
 * bcmjob_decode()/bcmjob_read() call before its post, so an image that
 * never calls them links none of it.  In the request record (uncached on
 * the device), so an owner on the other core reads the word itself */
#define own_pass_fn (r->pass)

void bjc_apply(void)
{
    if (b.p && (!own || !own->is_owner || own->is_owner()))
    {
        r->o_kick = r->a_kick;  /* the kick is answered: the next may go */
        apply_requests();
        if (own_pass_fn)
            own_pass_fn();
    }
}

static uint32_t next_want(bool on)
{
    return (((r->want >> 1) + 1) << 1) | (on ? 1 : 0);
}

int bjc_suspend_request(bool on, uint32_t ms)
{
    static bool fault_said;     /* the FAULT line, once a boot */
    uint32_t want, t0, spent;

    if (!b.p)
        return BCMJOB_OK;       /* no driver: nothing to suspend */
    want = next_want(on);
    r->want = want;
    if (!own || !own->is_owner || own->is_owner())
    {
        apply_requests();       /* the owner itself: at once */
        return BCMJOB_OK;
    }
    if (own->kick)
        own->kick();            /* its queue: applied promptly */
    /* ms in all (the freeze audit): a stale acknowledgement from an
       earlier request ends a wait early, never restarts the bound */
    t0 = b.p->now_ms();
    while (r->done != want)
    {
        spent = b.p->now_ms() - t0;
        if (spent >= ms || !own->wait || own->wait(ms - spent) < 0)
        {
            if (r->done == want)
                break;          /* acknowledged as the bound ran out */
            /* never acknowledged: withdrawn (the state the owner last
             * applied asked for again), and the caller refuses */
            r->want = next_want(r->done & 1);
            trace("bcmjob suspend on=%d ack timeout", on ? 1 : 0);
            if (!fault_said)
            {
                fault_said = true;
                trace("FAULT bjc_suspend_request wait=owner-ack ms=%lu",
                      (unsigned long)(b.p->now_ms() - t0));
            }
            return BCMJOB_TIMEOUT;
        }
    }
    return BCMJOB_OK;
}

/* ---- the port ------------------------------------------------------------ */

static int call(uint32_t op, const uint32_t *a, uint32_t n, uint32_t *rep,
                uint32_t cap, uint32_t us);

/* One word, under the port lock */
static int rd1(uint32_t a, uint32_t *v)
{
    int r;

    b.p->lock();
    r = b.p->rd(a, v);
    b.p->unlock();
    return r;
}

/* An encoded index word: its value, or -1 (torn or the port gave up) */
static int32_t index_at(uint32_t a)
{
    uint32_t w;

    if (b.p->rd(a, &w) < 0 || !HP_VALID(w))
        return -1;
    return (int32_t)(w & 0xFFFF);
}

/* Every overlay's ops go (an epoch, a RESET), DECODE's lent work slot
 * with its overlay.  due: a load is due again at once (not only at the
 * next epoch) */
static void ovl_drop_all(bool due)
{
    int k;

    for (k = 0; k < BJC_OVL_N; k++)
    {
        b.ops &= ~b.ovl[k].ops;
        b.ovl[k].ops = 0;
        if (due)
            b.ovl[k].epoch = 0;
    }
    b.caps &= ~J_CAP_DEC_LENT;
}

/* The epoch moves: slots are gone; on_epoch at the next API entry */
static void epoch_lost(void)
{
    ovl_drop_all(false);        /* the overlays went with it (jobs.md 5) */
    b.epoch++;
    b.epoch_due = true;
    b.dec_live = 0;             /* a DECODE in flight went with it */
    b.dec_pend = 0;
}

/* Is the heartbeat moving, and no fault word?  Called only after a wait
 * ran out (it takes up to BEAT_US) */
static bool alive(void)
{
    uint32_t a, c, t0 = b.p->now_us();

    if (rd1(LCD_CMD_WORD, &c) < 0 || c == LCD_FAULT || rd1(HP_STATE, &a) < 0)
        return false;
    while (b.p->now_us() - t0 < BEAT_US)
    {
        b.p->yield();
        if (rd1(HP_STATE, &c) < 0)
            return false;
        if ((c >> 8 & 0xFF) != (a >> 8 & 0xFF))
            return true;
    }
    return false;
}

/* A BCM failure (gap G2): restart at once, the PP framebuffer first; after
 * the second in a boot, jobs stay off */
static void chip_failed(const char *why)
{
    b.failures++;
    trace("bcmjob fault op=0x00 why=%s", why);
    trace("bcm power state=C0 why=fault");
    epoch_lost();
    b.gen = 0;
    b.st = ST_NONE;
    if (b.failures >= 2)
        trace("bcmjob off why=second failure");
    b.p->restart();             /* the panel comes back either way */
}

uint32_t bjc_vpu_expected(uint32_t ops)
{
    uint32_t v = J_VPU_CHECKED | J_VPU_SHIFT;

    if (ops & 1u << (J_OP_DECODE - 0x20))
        v |= J_VPU_MAC;         /* an image that decodes checks its IDCT */
    return v;
}

/* Discovery (jobs.md section 2): the header, HELLO, the job block */
/* The per-start field line (scripts/bench_ingest.py reads it): the merged
 * image's J_VPU2 and J_LAT_* beside J_VPU, e.g.
 *   bcmjob start vpu=0x3 expect=0x3 vpu2=0xff lat=286/136@150 latw=210/98
 * (lat: ns x 10 a byte load, SDRAM/SRAM, at the core MHz; latw: the word
 * loads).  Only words the image publishes, a few port reads: J_VPU2;
 * J_VPU2_FAIL and its got/exp words only when a form did not agree;
 * J_LAT_MHZ, then the timings only once measured (the first OPEN: usually
 * after the start, so mostly absent here).  An older image leaves them 0:
 * vpu2 logs 0 and the rest is left out. */
static void log_start_extras(uint32_t blk, uint32_t vpu, uint32_t expect)
{
    uint32_t v2 = 0, f = 0, got = 0, exp = 0, mhz = 0, lsd = 0, lsr = 0;
    char fb[56], lb[56];

    if (rd1(blk + J_VPU2, &v2) < 0)
        v2 = 0;
    fb[0] = lb[0] = '\0';
    if (v2 && v2 != (J_VPU2_CHECKED | (((1u << J_VPU2_FORMS) - 1) << 1)) &&
        rd1(blk + J_VPU2_FAIL, &f) == 0 && f)
    {
        if (rd1(blk + J_VPU2_GOT, &got) < 0)
            got = 0;
        if (rd1(blk + J_VPU2_EXP, &exp) < 0)
            exp = 0;
        snprintf(fb, sizeof(fb), " fail=%lu lane=%lu got=0x%lx exp=0x%lx",
                 (unsigned long)(f & 0xFF), (unsigned long)(f >> 8),
                 (unsigned long)got, (unsigned long)exp);
    }
    if (rd1(blk + J_LAT_MHZ, &mhz) == 0 && (mhz & J_LAT_DONE) &&
        rd1(blk + J_LAT_SDRAM, &lsd) == 0 && rd1(blk + J_LAT_SRAM, &lsr) == 0)
        snprintf(lb, sizeof(lb), " lat=%lu/%lu@%lu latw=%lu/%lu",
                 (unsigned long)(lsd & 0xFFFF), (unsigned long)(lsr & 0xFFFF),
                 (unsigned long)(mhz & ~J_LAT_DONE),
                 (unsigned long)(lsd >> 16), (unsigned long)(lsr >> 16));
    trace("bcmjob start vpu=0x%lx expect=0x%lx vpu2=0x%lx%s%s",
          (unsigned long)vpu, (unsigned long)expect, (unsigned long)v2, fb, lb);
}

static uint8_t discover(void)
{
    uint32_t h[(HP_EVT_SIZE - HP_VERSION) / 4 + 1], v, k;
    int32_t a, c, e;

    for (k = 0; k < sizeof(h) / sizeof(h[0]); k++)
        if (rd1(HP_VERSION + 4 * k, &h[k]) < 0)
            return ST_CPU;
    if (h[0] >> 16 != HP_MAGIC || (h[0] >> 8 & 0xFF) != HP_MAJOR)
    {
        trace("bcmjob cpu why=protocol %08lx", (unsigned long)h[0]);
        return ST_CPU;
    }
    if (!(h[1] & HP_CAP_JOBS_2D))
    {
        trace("bcmjob cpu why=no jobs caps=%04lx", (unsigned long)h[1]);
        return ST_CPU;
    }
    b.cmd_base = h[2];
    b.cmd_size = h[3];
    b.evt_base = h[4];
    b.evt_size = h[5];
    b.p->lock();
    a = index_at(HP_CMD_WR);
    c = index_at(HP_CMD_RD);
    e = index_at(HP_EVT_RD);
    b.p->unlock();
    if (!b.cmd_size || b.cmd_size & (b.cmd_size - 1) || b.cmd_size > 0x8000 ||
        !b.evt_size || b.evt_size & (b.evt_size - 1) || b.evt_size > 0x8000 ||
        a < 0 || c < 0 || e < 0)
    {
        trace("bcmjob cpu why=rings");
        return ST_CPU;
    }
    b.cmd_wr = a;
    b.cmd_rd = c;
    b.evt_rd = e;
    b.st = ST_JOBS;             /* for the HELLO below */
    k = HP_MAJOR << 8 | HP_MINOR;
    a = call(HP_OP_HELLO, &k, 1, h, 3, HELLO_US);
    if (a != HP_OK)
    {
        trace("bcmjob cpu why=hello %ld", (long)a);
        return ST_CPU;
    }
    if (rd1(HP_JOBS, &b.block) < 0 || !b.block ||
        rd1(b.block + J_VERSION, &v) < 0 ||
        v >> 16 != J_MAGIC || (v >> 8 & 0xFF) != J_MAJOR ||
        rd1(b.block + J_OPS, &b.ops) < 0 || rd1(b.block + J_DONE, &b.done_base) < 0)
    {
        trace("bcmjob cpu why=job block %08lx", (unsigned long)b.block);
        return ST_CPU;
    }
    b.minor = v & 0xFF;
    /* rev 0.5 (candidate images): the staged upload.  Older images never
       write the word, so it counts only with its magic */
    b.caps = rd1(b.block + J_CAPS, &k) == 0 && J_CAPS_VALID(k) ? k & 0xFFFF : 0;
    b.caps &= ~J_CAP_DEC_LENT;  /* only from an overlay this side loaded */
    /* the overlay region (J_OVL, valid with OVERLAY offered): which
       instance's record may load here (bjc_ovl_due) */
    b.region = 0;
    if ((b.ops & 1u << (J_OP_OVERLAY - 0x20)) && rd1(b.block + J_OVL, &k) == 0)
        b.region = k & ~0xFFFu;
    b.sent = 0;
#ifdef HAVE_BCMFW_DEVTEST
    memset(b.inj_due, 0, sizeof(b.inj_due));    /* a new ring: gone */
#endif
    trace("bcmjob up epoch=%lu ops=%08lx", (unsigned long)b.epoch,
          (unsigned long)b.ops);
    if (b.caps)
        trace("bcmjob caps=0x%x%s", (unsigned)b.caps,
              b.caps & J_CAP_STAGED ? " staged" : "");
    /* the vector unit's start-up checks (jobs.h J_VPU), once per power-up:
     * the image's only record of them (0 in images before the word) */
    b.vpu = 0;
    if (rd1(b.block + J_VPU, &v) == 0)
    {
        b.vpu = (uint8_t)v;
        k = bjc_vpu_expected(b.ops);
        /* fewer bits: the image took its C paths (exact, only slower);
         * not a fault, the jobs still go */
        trace("bcmjob vpu=0x%lx expect=0x%lx%s", (unsigned long)v,
              (unsigned long)k, v == k ? "" : !v ? " why=unchecked"
                                                 : " why=degraded");
        log_start_extras(b.block, v, k);
    }
    return ST_JOBS;
}

/* Every API entry: the power-up's state; on_epoch delivered here.
 * Returns ST_JOBS, or ST_CPU / ST_OFF / ST_NONE (all "no jobs now"). */
static uint8_t enter_st(void)
{
    uint32_t g;

    apply_requests();
    hours();
    if (b.failures >= 2)
        return ST_OFF;
    g = b.suspended ? 0 : b.p->gen();
    if (!g)
    {
        if (b.gen)
        {
            b.gen = 0;
            b.st = ST_NONE;
            b.inflight = false;
            epoch_lost();
            trace("bcm power state=C0 why=%s", b.p->lit() ? "fault" : "dark");
        }
    }
    else if (g != b.gen)
    {
        if (b.gen)
            epoch_lost();       /* restarted under us */
        b.gen = g;
        b.st = ST_NONE;
        b.inflight = false;     /* a new ring */
        trace("bcm power state=C1 why=%s", b.p->lit() ? "lit" : "want");
        b.st = discover();
        if (b.st != ST_JOBS)
            b.st = ST_CPU;
    }
    if (b.epoch_due)
    {
        b.epoch_due = false;
        if (b.on_epoch)
            b.on_epoch(b.epoch);
    }
    return g ? b.st : ST_NONE;
}

/* Tell the device whether jobs are in flight on this power-up.  Told
 * again for every new power-up (the device keys job mode to it: S0
 * review M1). */
static void mode_tell(void)
{
    bool jobs = b.inflight && b.gen && b.st == ST_JOBS && b.failures < 2;

    if (jobs != b.jobs_mode || (jobs && b.gen != b.mode_gen))
    {
        b.jobs_mode = jobs;
        b.mode_gen = b.gen;
        if (own && own->mode)
            own->mode(jobs);    /* lcd_tick's hang check: the heartbeat's */
    }
}

static uint8_t enter(void)
{
    uint8_t st = enter_st();

    mode_tell();
    return st;
}

/* Owner decision (2026-10-24): while a DECODE is on the ring, a call
 * that would queue behind it or wait for the BCM answers BUSY at once
 * instead of timing out behind it (and being marked slow).  Never
 * counted: no failed or slow mark.  Traced "why=busy-decode".  After a
 * timeout the job may hold the BCM until its abort or stall ends it:
 * twice J_DEC_STALL_MS at most. */
static int drain(void);

static bool dec_holds(void)
{
    /* on the owner, a pass first: a call left or ended whose kick was
       dropped is finished here, so its hold can't outlive it */
    if (own_pass_fn && (!own || !own->is_owner || own->is_owner()))
        own_pass_fn();
    if (b.dec_live && b.dec_live_until)
    {
        /* timed out: no pass drains for it any more, so look here for
           its late reply (one read of the event index when none came) */
        drain();
        if (b.dec_live && (int32_t)(b.p->now_us() - b.dec_live_until) >= 0)
            b.dec_live = 0;
    }
    return b.dec_live != 0;
}

static int busy_decode(uint32_t op)
{
    trace("bcmjob fault op=0x%02lx why=busy-decode", (unsigned long)op);
    return BCMJOB_BUSY;
}

static bool op_ok(uint32_t op)
{
    uint32_t bit = 1u << (op - 0x20);

    return op >= 0x20 && op < 0x40 && (b.ops & bit) &&
           !(b.slow & bit) && !(b.failed & bit);
}

/* ---- the rings ------------------------------------------------------------ */

/* One record into the command ring: 0, BCMJOB_BUSY (no room), or
 * BCMJOB_OFF (the port gave up).  Never waits. */
static int put(uint32_t op, uint32_t tag, const uint32_t *a, uint32_t n)
{
    uint32_t w[1 + HP_MAX_PAYLOAD], k = 0, m = b.cmd_size - 1;
    int r = 0;

    if (n > HP_MAX_PAYLOAD)
        return BCMJOB_FAILED;
#ifdef HAVE_BCMFW_DEVTEST
    if (op >= 0x20 && op < 0x40 && b.inj_left[op - 0x20])
        tag |= J_TAG_REPLY;     /* bjc_inject_fail: its reply will fail */
#endif
    w[0] = HP_REC(op, n, tag);
    if (n)
        memcpy(&w[1], a, 4 * n);
    b.p->lock();
    if (b.cmd_size - ((b.cmd_wr - b.cmd_rd) & 0xFFFF) < n + 1)
    {
        /* the cached read index says full: look again (one read) */
        int32_t rd = index_at(HP_CMD_RD);

        if (rd >= 0)
            b.cmd_rd = rd;
        if (rd < 0 || b.cmd_size - ((b.cmd_wr - b.cmd_rd) & 0xFFFF) < n + 1)
        {
            b.p->unlock();
            return BCMJOB_BUSY;
        }
    }
    while (k < n + 1)
    {
        uint32_t pos = (b.cmd_wr + k) & m, run = n + 1 - k;

        if (run > b.cmd_size - pos)
            run = b.cmd_size - pos;
        if (b.p->wr_block(b.cmd_base + 4 * pos, (const uint16_t *)&w[k],
                          2 * run) < 0)
        {
            r = BCMJOB_OFF;
            break;
        }
        k += run;
    }
    /* the data before the index: the port keeps writes in order */
    if (!r && b.p->wr(HP_CMD_WR, HP_ENC((b.cmd_wr + n + 1) & 0xFFFF)) < 0)
        r = BCMJOB_OFF;
    if (!r)
    {
        b.cmd_wr = (b.cmd_wr + n + 1) & 0xFFFF;
        b.p->doorbell();
        if (op >= 0x20)         /* a job (not HELLO): in flight */
        {
            b.inflight = true;
            if (op == J_OP_FENCE && n)
            {
                b.fence_last = a[0];
                b.put_after_fence = false;
            }
            else
                b.put_after_fence = true;
        }
        if (op >= 0x20)
            b.oplog[b.sent++ % OPLOG] = (uint8_t)op;
        if (op == J_OP_RESET)
            ovl_drop_all(true);     /* the BCM drops the overlays' ops: a
                                       load is due again */
        if (op == J_OP_COMMIT && n && a[0] < BJC_SLOTS)
            b.commit_slot = (uint8_t)(a[0] + 1);
#ifdef HAVE_BCMFW_DEVTEST
        if (op >= 0x20 && op < 0x40 && b.inj_left[op - 0x20])
        {
            b.inj_left[op - 0x20]--;
            b.inj_due[op - 0x20]++;
        }
#endif
    }
    b.p->unlock();
    if (!r && op >= 0x20)
        mode_tell();
    return r;
}

/* A job failed (bcmjob.h, the failed mark): the caller's normal outcomes
 * don't count; BCMJOB_FAIL_MARK counted failures in a boot give the op
 * its CPU path.  The line has the status (open question #67: it once
 * didn't), and a COMMIT's J_E_SUM the BCM's words beside the PP's sums
 * (rev 0.5 images: jobs.h J_SUMREP_*) */
static void job_fail(uint32_t op, uint32_t st, const uint32_t *body, uint32_t n)
{
    /* the status word (bench 2026-10-25: COMMIT failed, cause unknown);
     * a COMMIT's with the last one's slot (its fault comes back in order) */
    if (op == J_OP_COMMIT && b.commit_slot && b.slot)
    {
        const struct bjc_slot *t = &b.slot[b.commit_slot - 1];

        trace("bcmjob fault op=0x%02lx st=%ld why=failed slot=%u %ux%u",
              (unsigned long)op, (long)st, b.commit_slot - 1u,
              (unsigned)t->w, (unsigned)t->h);
    }
    else
        trace("bcmjob fault op=0x%02lx st=%ld why=failed", (unsigned long)op,
              (long)st);
    /* the sums on a line of their own (a trace line holds 111 characters):
       the PP's, then the BCM's, its first word, how and the window's B */
    if (op == J_OP_COMMIT && st == J_E_SUM)
        trace("bcmjob sum slot=%lu pp=%08lx,%08lx bcm=%08lx,%08lx w0=%08lx "
              "how=%lu win=%08lx", (unsigned long)b.commit[0],
              (unsigned long)b.commit[1], (unsigned long)b.commit[2],
              (unsigned long)(n >= J_SUMREP_N ? body[J_SUMREP_A] : 0),
              (unsigned long)(n >= J_SUMREP_N ? body[J_SUMREP_B] : 0),
              (unsigned long)(n >= J_SUMREP_N ? body[J_SUMREP_W0] : 0),
              (unsigned long)(n >= J_SUMREP_N ? body[J_SUMREP_HOW] : 9),
              (unsigned long)(n >= J_SUMREP_N ? body[J_SUMREP_WIN_B] : 0));
    /* an overlay's op the BCM no longer knows: its overlay went with an
       epoch change this side hasn't drained yet (its J_EV_EPOCH comes
       first in the ring, or a restart); never counted, the ops dropped
       until the next load */
    if (st == HP_E_UNKNOWN && op >= 0x20 && op < 0x40)
    {
        int k;

        for (k = 0; k < BJC_OVL_N; k++)
            if (b.ovl[k].ops & 1u << (op - 0x20))
            {
                b.ops &= ~b.ovl[k].ops;
                b.ovl[k].epoch = 0;     /* a load is due again */
                b.ovl[k].ops = 0;
                if (k == BJC_OVL_DEC)
                    b.caps &= ~J_CAP_DEC_LENT;
                trace("bcmjob overlay gone op=0x%02lx", (unsigned long)op);
                return;
            }
    }
    if (!(st == J_E_NOSLOT || st == J_E_SLOT || st == J_E_SUM
          || st == J_E_MODE || (st >= J_E_DEC_UNSUPPORTED
                                && st <= J_E_DEC_TRUNCATED))
        && op >= 0x20 && op < 0x40 && b.fail_n[op - 0x20] < 255
        && ++b.fail_n[op - 0x20] >= BCMJOB_FAIL_MARK)
        b.failed |= 1u << (op - 0x20);
}

/* Read the event ring: a reply whose tag a box awaits is filed there
 * (status first); other failures mark their op; J_EV_EPOCH moves the
 * epoch.  0, or -1 the port gave up. */
static int drain(void)
{
    int32_t wr;

    /* never the port while cut, suspended or restarted under us (its new
       ring is rediscovered first: a stale evt_rd must not be written) */
    if (!b.gen || b.p->gen() != b.gen || b.suspended)
        return -1;
    b.p->lock();
    wr = index_at(HP_EVT_WR);
    if (wr < 0)
    {
        b.p->unlock();
        return 0;               /* torn: next time */
    }
    while (b.evt_rd != (uint32_t)wr)
    {
        uint32_t hdr, body[HP_MAX_PAYLOAD], n, i, op;

        if (b.p->rd(b.evt_base + 4 * (b.evt_rd & (b.evt_size - 1)), &hdr) < 0)
            goto port;
        n = HP_REC_LEN(hdr);
        if (n > HP_MAX_PAYLOAD || n + 1 > (((uint32_t)wr - b.evt_rd) & 0xFFFF))
            break;
        for (i = 0; i < n; i++)
            if (b.p->rd(b.evt_base + 4 * ((b.evt_rd + 1 + i) & (b.evt_size - 1)),
                        &body[i]) < 0)
                goto port;
        b.evt_rd = (b.evt_rd + 1 + n) & 0xFFFF;
        op = HP_REC_OP(hdr);
        if (op & HP_REPLY)
        {
            op &= ~HP_REPLY;
#ifdef HAVE_BCMFW_DEVTEST
            if (op >= 0x20 && op < 0x40 && n && b.inj_due[op - 0x20])
            {
                /* bjc_inject_fail: the reply as the BCM's error, then the
                 * real path below (the caller's status, or the mark) */
                b.inj_due[op - 0x20]--;
                if (body[0] == HP_OK)
                    body[0] = BJC_INJECT_ERR;
            }
#endif
            for (i = 0; i < BOXES; i++)
                if (b.box[i].tag && b.box[i].tag == HP_REC_TAG(hdr) &&
                    !b.box[i].got)
                    break;
            if (b.dec_live && HP_REC_TAG(hdr) == b.dec_live)
                b.dec_live = 0;         /* the DECODE ended (a late reply
                                           after a timeout, too) */
            if (b.dec_pend && HP_REC_TAG(hdr) == b.dec_pend)
                b.dec_pend = 0;
            if (i < BOXES)
            {
                struct box *x = &b.box[i];

                x->n = n < BOX_WORDS ? n : BOX_WORDS;
                memcpy(x->rep, body, 4 * x->n);
                x->got = true;
            }
            else if (op == J_OP_ALLOC && n >= 5 && body[0] == HP_OK &&
                     !(body[4] & J_ALLOC_HIT) && (HP_REC_TAG(hdr) & J_TAG_REPLY))
            {
                /* an ALLOC whose wait timed out (its box went): a new
                   slot is nobody's, freed behind it; a hit is another
                   holder's and stays */
                trace("bcmjob free late alloc slot=%lu", (unsigned long)body[1]);
                put(J_OP_FREE, 0, &body[1], 1);
            }
            else if (n && body[0] != HP_OK && op >= 0x20 && op < 0x40)
                job_fail(op, body[0], body, n);
        }
        else if (op == J_EV_EPOCH)
            epoch_lost();
    }
    if (b.p->wr(HP_EVT_RD, HP_ENC(b.evt_rd)) < 0)
        goto port;
    b.p->unlock();
    return 0;
port:
    b.p->unlock();
    return -1;
}

/* After a wait ran out with the BCM still powered: slow or dead */
static int timed_out(void)
{
    uint32_t done, op = 0, bit;

    if (!alive())
    {
        chip_failed("dead");
        return BCMJOB_TIMEOUT;
    }
    if (rd1(b.block + J_DONE, &done) == 0 && done - b.done_base < b.sent &&
        b.sent - (done - b.done_base) <= OPLOG)
        op = b.oplog[(done - b.done_base) % OPLOG];
    /* a DECODE ahead of this wait is not this wait's culprit: only its
       own deadline (dec_timed_out) marks it */
    if (op >= 0x20 && op < 0x40 && op != J_OP_DECODE)
    {
        bit = 1u << (op - 0x20);
        if (b.slow_once & bit)
            b.slow |= bit;      /* missed twice: its CPU path for the boot */
        b.slow_once |= bit;
    }
    return fault(op, BCMJOB_TIMEOUT);
}

/* A free box armed for a new tag (with J_TAG_REPLY for a job), or NULL */
static struct box *box_arm(uint32_t op)
{
    struct box *x;
    int k;

    for (k = 0; k < BOXES && b.box[k].tag; k++)
        ;
    if (k == BOXES)
        return NULL;
    x = &b.box[k];
    b.tag = (b.tag + 1) & 0x7FFF;
    if (!b.tag)
        b.tag = 1;
    x->tag = b.tag | (op >= 0x20 ? J_TAG_REPLY : 0);
    x->n = 0;
    x->got = false;
    return x;
}

/* A command with a reply (HELLO, ALLOC, COMMIT), waiting at most us */
static int call(uint32_t op, const uint32_t *a, uint32_t n, uint32_t *rep,
             uint32_t cap, uint32_t us)
{
    uint32_t t0 = b.p->now_us();
    struct box *x = box_arm(op);
    int r;

    if (!x)
        return BCMJOB_BUSY;
    r = put(op, x->tag, a, n);
    while (!r)
    {
        r = drain();
        if (x->got)
        {
            b.inflight = false; /* in ring order: all before it ran */
            mode_tell();
            if (cap > x->n)
                cap = x->n;
            memcpy(rep, x->rep, 4 * cap);
            r = x->n ? (int)x->rep[0] : HP_E_INVAL;
            break;
        }
        if (r < 0 || b.p->gen() != b.gen)
            r = BCMJOB_OFF;
        else if (b.p->now_us() - t0 > us)
            r = op >= 0x20 ? timed_out() : BCMJOB_TIMEOUT;
        else
            b.p->yield();
    }
    x->tag = 0;
    return r;
}

/* ---- bcmjob.h ------------------------------------------------------------- */

bool bcmjob_have(uint32_t op)
{
    return enter() == ST_JOBS && op_ok(op);
}

int bcmjob_submit(uint32_t op, uint32_t tag, const uint32_t *payload, uint32_t n)
{
    uint8_t s = enter();
    int r;

    if (s != ST_JOBS)
        return fault(op, BCMJOB_OFF);
    if (!op_ok(op))
        return fault(op, BCMJOB_NOJOB);
    if (dec_holds())
        return busy_decode(op);
    r = put(op, tag & 0x7FFF, payload, n);     /* J_TAG_REPLY is the
                                                  driver's (its boxes) */
    if (r)
        return fault(op, r);
    if (b.verbose)
        trace("bcmjob submit op=0x%02lx tag=%lu n=%lu", (unsigned long)op,
              (unsigned long)tag, (unsigned long)n);
    return BCMJOB_OK;
}

int bcmjob_fence(uint32_t *seq)
{
    uint32_t s = (b.seq + 1) & 0xFFFF;
    int r;

    if (!s)
        s = 1;
    r = bcmjob_submit(J_OP_FENCE, 0, &s, 1);
    if (r)
        return r;
    b.seq = s;
    *seq = s;
    if (b.verbose)
        trace("bcmjob fence seq=%lu", (unsigned long)s);
    return BCMJOB_OK;
}

#ifdef HAVE_BCMFW_DEVTEST
/* The op's failed mark and failure count go (the developer's "off": the
 * return to the chip without a reboot) */
void bjc_inject_forgive(uint32_t op)
{
    if (op < 0x20 || op >= 0x40)
        return;
    b.failed &= ~(1u << (op - 0x20));
    b.fail_n[op - 0x20] = 0;
}

void bjc_inject_fail(uint32_t op, uint32_t n)
{
    if (op < 0x20 || op >= 0x40)
        return;
    b.inj_left[op - 0x20] = n > 255 ? 255 : n;
    if (!n)
        b.inj_due[op - 0x20] = 0;
    trace("bcmjob inject op=0x%02lx n=%lu", (unsigned long)op,
          (unsigned long)b.inj_left[op - 0x20]);
}
#endif

/* bcmjob.h: the block's J_ERRORS.  Read only while this power-up's
 * discovery holds (never a port access while cut or starting); else the
 * last value read */
uint32_t bcmjob_errors(void)
{
    uint32_t v;

    if (b.st == ST_JOBS && b.gen && b.gen == b.p->gen() && b.block
        && rd1(b.block + J_ERRORS, &v) == 0)
        b.errs = v;
    return b.errs;
}

/* One look at J_FENCE, never a wait: 1 the fence seq retired (the
 * events drained; the last fence: nothing in flight), 0 not yet */
static int fence_poll(uint32_t seq)
{
    int32_t f;

    b.p->lock();
    f = index_at(b.block + J_FENCE);
    b.p->unlock();
    if (f < 0 || (((uint32_t)f - seq) & 0x8000))
        return 0;
    drain();                            /* failures and epoch events */
    if (!b.put_after_fence && !(((uint32_t)f - b.fence_last) & 0x8000))
    {
        b.inflight = false;             /* the last fence: all drained */
        mode_tell();
    }
    return 1;
}

int bcmjob_wait(uint32_t seq, uint32_t ms)
{
    uint32_t t0 = b.p->now_us(), us = ms * 1000;

    if (enter() != ST_JOBS)
        return BCMJOB_OFF;
    for (;;)
    {
        if (fence_poll(seq))
            return BCMJOB_OK;
        if (b.p->gen() != b.gen)
            return BCMJOB_OFF;
        if (b.p->now_us() - t0 > us)
            return timed_out();
        b.p->yield();
    }
}

/* ALLOC and wait: the reply's status (HP_E_INVAL for a slot past the
 * table), or a BCMJOB_* < 0; the slot's facts kept for upload and read */
static int alloc(uint32_t cls, uint32_t w, uint32_t h, uint32_t key_lo,
                 uint32_t key_hi, struct bcmjob_slot *out)
{
    uint32_t a[6], rep[5];
    int st;

    a[0] = cls;
    a[1] = J_XY(w, h);
    a[2] = J_FMT_RGB565;
    a[3] = key_lo;
    a[4] = key_hi;
    /* rev 0.5: the staged upload when the image offers it (its reply then
       names the window, which the upload writes as it would the slot; the
       first staged ALLOC brings the BCM's SDRAM up, so its deadline has
       the bring-up's 4 attempts of 5 ms more) */
    a[5] = J_ALLOC_F_STAGE;
    /* in ring order: behind everything queued (a fence's worth) */
    st = call(J_OP_ALLOC, a, (b.caps & J_CAP_STAGED) ? 6 : 5, rep, 5,
              1000 * (J_T_FENCE_BASE + J_T_SMALL +
                      ((b.caps & J_CAP_STAGED) ? J_T_STAGE_UP : 0)));
    if (st != HP_OK)
        return st;
    if (rep[1] >= BJC_SLOTS)
        return HP_E_INVAL;
    out->slot = rep[1];
    out->hit = (rep[4] & J_ALLOC_HIT) != 0;
    out->epoch = b.epoch;
    b.slot[rep[1]].addr = rep[2];
    b.slot[rep[1]].pitch = rep[3];
    b.slot[rep[1]].w = w;
    b.slot[rep[1]].h = h;
    b.slot[rep[1]].ep = (uint16_t)b.epoch;
    b.slot[rep[1]].staged = (rep[4] & J_ALLOC_STAGED) != 0;
    if ((b.caps & J_CAP_STAGED) && !b.slot[rep[1]].staged && !out->hit)
        trace("bcmjob alloc slot=%lu addr=%08lx why=not staged",
              (unsigned long)rep[1], (unsigned long)rep[2]);
    return HP_OK;
}

int bcmjob_slot(uint32_t cls, uint32_t w, uint32_t h, uint32_t key_lo,
                uint32_t key_hi, struct bcmjob_slot *out)
{
    int st;

    if (enter() != ST_JOBS)
        return fault(J_OP_ALLOC, BCMJOB_OFF);
    if (!op_ok(J_OP_ALLOC) || !op_ok(J_OP_COMMIT))
        return fault(J_OP_ALLOC, BCMJOB_NOJOB);
    if (dec_holds())
        return busy_decode(J_OP_ALLOC);
    st = alloc(cls, w, h, key_lo, key_hi, out);
    if (st < 0)
        return st == BCMJOB_TIMEOUT ? st : fault(J_OP_ALLOC, st);
    if (st != HP_OK)
        return fault(J_OP_ALLOC, BCMJOB_FAILED);
    return BCMJOB_OK;
}

static int write_row(uint32_t a, const uint16_t *s, uint32_t w);

int bcmjob_upload(const struct bcmjob_slot *s, const uint16_t *px)
{
    const struct bjc_slot *t;
    uint32_t bytes, done = 0, r, a[3];
    int err = 0;

    if (s->hit)
        return BCMJOB_OK;
    if (enter() != ST_JOBS || s->epoch != b.epoch || s->slot >= BJC_SLOTS)
        return fault(J_OP_COMMIT, BCMJOB_OFF);
    if (dec_holds())
        return busy_decode(J_OP_COMMIT);       /* before any pixel */
    t = &b.slot[s->slot];
    bytes = (uint32_t)t->w * t->h * 2;
    b.p->lock();
    if (t->pitch == 2 * t->w && !(bytes & 3))
    {
        /* one run: engine 2 for 16 rows or more, the CPU for the rest */
        if (t->h >= 16 && b.p->upload)
            done = b.p->upload(t->addr, px, bytes);
        if (done < bytes)
            err = b.p->wr_block(t->addr + done, px + done / 2, (bytes - done) / 2);
    }
    else
        /* row by row: an odd width at an odd pitch starts every other row
         * mid-word, and the port writes whole words at the address
         * rounded down (CR1 [silicon] 2026-10-25: a 301-wide slot's row 1
         * landed one pixel early, over row 0's last); write_row merges
         * the odd first and last pixels into their words */
        for (r = 0; r < t->h && !err; r++)
            err = write_row(t->addr + r * t->pitch, px + r * t->w, t->w);
    b.p->unlock();
    if (err)
        return fault(J_OP_COMMIT, BCMJOB_OFF);
    a[0] = s->slot;     /* the w x h pixels' F16w (jobs.h rev 0.2): px is
                           packed, so one run is the row-by-row sum */
    a[1] = a[2] = 0;
    j_f16w(&a[1], &a[2], px, (uint32_t)t->w * t->h);
    memcpy(b.commit, a, sizeof(b.commit));
    return bcmjob_submit(J_OP_COMMIT, 0, a, 3);
}

/* jobs.md section 6: the resync comes after the jobs already queued, so
 * a SCROLL ahead of a BUSY PRESENT can't change FRONT after the PP's
 * truth went there.  Waits (bounded: a fence's deadline and a PRESENT's)
 * until J_DONE shows every job sent this power-up has run; a timeout, a
 * cut or a new epoch meanwhile pushes anyway (truth wins), logged. */
#define RESYNC_US (1000u * (J_T_FENCE_BASE + J_T_PRESENT))

static void resync_drain(void)
{
    uint32_t t0 = b.p->now_us(), g = b.gen, e = b.epoch, done, upto;
    const char *why = NULL;

    for (;;)
    {
        if (drain() < 0 || b.p->gen() != g || b.epoch != e)
        {
            why = "epoch";
            break;
        }
        if (rd1(b.block + J_DONE, &done) < 0)
        {
            why = "port";
            break;
        }
        /* everything queued has run; with a DECODE still on the BCM (past
           its deadline), what was queued ahead of it: never a wait for
           the DECODE itself */
        upto = b.dec_pend ? b.dec_pos : b.sent;
        if (done - b.done_base >= upto)
            return;
        if (b.p->now_us() - t0 > RESYNC_US)
        {
            why = "timeout";
            break;
        }
        b.p->yield();
    }
    trace("bcmjob resync forced why=%s", why);
}

int bcmjob_resync(uint32_t x, uint32_t y, uint32_t w, uint32_t h)
{
    enter();
    if (!b.p->gen())
        return BCMJOB_OFF;
    /* while a DECODE holds the BCM, the jobs queued ahead of it still run
       first (jobs.md section 6: a PRESENT ahead of it must not land on
       the settled frame): wait for those, bounded, then push through the
       LCD contract, which the BCM serves inside a DECODE (S3c) */
    dec_holds();
    if (b.st == ST_JOBS && b.gen && b.gen == b.p->gen() && b.sent)
        resync_drain();
    trace("bcmjob resync x=%lu y=%lu w=%lu h=%lu", (unsigned long)x,
          (unsigned long)y, (unsigned long)w, (unsigned long)h);
    return b.p->push(x, y, w, h) < 0 ? BCMJOB_OFF : BCMJOB_OK;
}

/* bcmfw/jobs/bcmjob.h's call: a request (the result is
 * bjc_suspend_request()'s, for callers that must refuse on a timeout) */
void bcmjob_suspend(bool on)
{
    (void)bjc_suspend_request(on, BJC_ACK_MS);
}

/* The owner only (apply_requests) */
static void suspend_apply(bool on)
{
    if (on == b.suspended)
        return;
    b.suspended = on;
    if (on)
    {
        if (b.gen)
            epoch_lost();
        b.gen = 0;              /* resume runs HELLO again (a new power-up) */
        b.st = ST_NONE;
        b.inflight = false;
        mode_tell();
        trace("bcmjob suspend");
    }
    else
        trace("bcmjob resume");
}

uint32_t bcmjob_minor(void)
{
    return enter() == ST_JOBS ? b.minor : 0;
}

bool bcmjob_bars_flip(void)
{
    return bcmjob_have(J_OP_BARS) && (b.vpu & J_VPU_FLIP);
}

uint32_t bcmjob_epoch(void)
{
    enter();
    return b.epoch;
}

void bcmjob_on_epoch(void (*fn)(uint32_t epoch))
{
    b.on_epoch = fn;
}

/* ---- the visualizer (#126): SPECTRUM's reply, the overlay load ---------- */

/* bcmjob.h: SPECTRUM and its reply (jobs.h J_OP_SPECTRUM): the nb levels,
 * two halfwords to a word after the status */
int bcmjob_spectrum(uint32_t slot, uint32_t n, const uint16_t *edges, uint32_t nb,
                    uint16_t *l2)
{
    uint32_t a[J_SPEC_ARGS(J_SPEC_BANDS_MAX)], rep[BOX_WORDS], k;
    int st;

    if (enter() != ST_JOBS)
        return fault(J_OP_SPECTRUM, BCMJOB_OFF);
    if (!op_ok(J_OP_SPECTRUM))
        return fault(J_OP_SPECTRUM, BCMJOB_NOJOB);
    if (nb < 1 || nb > J_SPEC_BANDS_MAX || (n != 256 && n != 512))
        return BCMJOB_FAILED;
    if (dec_holds())
        return busy_decode(J_OP_SPECTRUM);
    a[0] = slot;
    a[1] = n | nb << 16;
    for (k = 0; k <= nb; k += 2)
        a[2 + k / 2] = edges[k] | (k + 1 <= nb ? (uint32_t)edges[k + 1] << 16 : 0);
    /* in ring order: behind the frame's jobs before it (a fence's worth) */
    st = call(J_OP_SPECTRUM, a, J_SPEC_ARGS(nb), rep, 1 + (nb + 1) / 2,
              1000 * (J_T_FENCE_BASE + J_T_SPECTRUM));
    if (st < 0)
        return st == BCMJOB_TIMEOUT ? st : fault(J_OP_SPECTRUM, st);
    if (st != HP_OK)
    {
        job_fail(J_OP_SPECTRUM, st, rep, 1);
        return BCMJOB_FAILED;
    }
    for (k = 0; k < nb; k++)
        l2[k] = (uint16_t)(rep[1 + k / 2] >> (16 * (k & 1)));
    return BCMJOB_OK;
}

/* A load's posted wait given up: its reply box freed (a late reply is
 * drain()'s: an OK dropped, a failure counted as any job's) */
static void ovl_idle(struct ovl *o)
{
    if (o->x)
        o->x->tag = 0;
    o->x = NULL;
    o->ph = OVL_PH_IDLE;
}

void bjc_ovl_set(int k, const struct bjc_ovl_rec *rec)
{
    struct ovl *o;

    if (k < 0 || k >= BJC_OVL_N)
        return;
    o = &b.ovl[k];
    if (!memcmp(&o->rec, rec, sizeof(*rec)))
        return;                 /* the same file: its state stands */
    ovl_idle(o);
    o->rec = *rec;
    o->never = false;
    o->epoch = 0;
    o->tries = 0;
    o->words = 0;
}

uint32_t bjc_ovl_region(void)
{
    return enter() == ST_JOBS && op_ok(J_OP_OVERLAY) ? b.region : 0;
}

bool bjc_ovl_due(int k)
{
    const struct ovl *o = &b.ovl[k < 0 || k >= BJC_OVL_N ? 0 : k];

    if (k < 0 || k >= BJC_OVL_N || enter() != ST_JOBS ||
        !op_ok(J_OP_OVERLAY) || o->never || !o->rec.bytes ||
        o->rec.base != b.region || (b.ops & o->want) == o->want)
        return false;
    /* a try an epoch, two after a torn upload (J_E_SUM) */
    return o->epoch != b.epoch || o->tries < 2;
}

/* a refusal: logged; never again this boot (the file or the image's
 * build), or not this epoch */
static int ovl_refuse(struct ovl *o, const char *why, bool boot)
{
    trace("bcmjob overlay refused why=%s", why);
    if (boot)
        o->never = true;
    o->tries = 2;
    return BCMJOB_FAILED;
}

/* The begin's second half: the FENCE the first call posted has retired
 * (one read a call), then the region is the load's.  OFF: the epoch
 * moved or the BCM went; TIMEOUT: past the FENCE's bound, as the wait's
 * was */
static int ovl_fenced(int k, struct ovl *o)
{
    if (enter() != ST_JOBS || o->epoch != b.epoch)
    {
        ovl_idle(o);
        return BCMJOB_OFF;
    }
    if (o->fseq && !fence_poll(o->fseq))
    {
        if (b.p->gen() != b.gen)
        {
            ovl_idle(o);
            return BCMJOB_OFF;
        }
        if (b.p->now_us() - o->t0 > 1000 * (J_T_FENCE_BASE + J_T_PRESENT))
        {
            ovl_idle(o);
            return timed_out();
        }
        return BJC_OVL_AGAIN;
    }
    o->ph = OVL_PH_IDLE;
    o->words = o->rec.bytes / 4;
    trace("bcmjob overlay load k=%d bytes=%lu at=%08lx epoch=%lu", k,
          (unsigned long)o->rec.bytes, (unsigned long)o->base,
          (unsigned long)b.epoch);
    return BCMJOB_OK;
}

int bjc_ovl_begin(int k, const uint32_t *hdr, uint32_t bytes)
{
    uint32_t v, base, size, w0, w1, sh, seq;
    const struct bjc_ovl_rec *rec;
    struct ovl *o;
    int r;

    if (k >= 0 && k < BJC_OVL_N && b.ovl[k].ph == OVL_PH_FENCE)
        return ovl_fenced(k, &b.ovl[k]);    /* the same load, polled */
    if (!bjc_ovl_due(k))
        return BCMJOB_NOJOB;
    if (dec_holds())
        return busy_decode(J_OP_OVERLAY);
    o = &b.ovl[k];
    ovl_idle(o);                /* an end never answered: this load's now */
    rec = &o->rec;
    if (o->epoch != b.epoch)
        o->tries = 0;
    o->epoch = b.epoch;
    o->tries++;
    /* the file is the build's (its size, magic and ABI word) */
    if (bytes != rec->bytes || (bytes & 3) || bytes < 4 * BJC_OVL_HDR_WORDS ||
        hdr[0] != J_OVL_MAGIC || hdr[1] != rec->abi)
        return ovl_refuse(o, "file", true);
    /* the header against the region (J_OVL: base | size >> 12) */
    if (rd1(b.block + J_OVL, &v) < 0)
        return BCMJOB_OFF;
    base = v & ~0xFFFu;
    size = (v & 0xFFFu) << 12;
    o->size = size;
    if (hdr[2] != base || hdr[3] != base + bytes || hdr[4] < hdr[3] ||
        hdr[4] > base + size)
        return ovl_refuse(o, "region", false);
    /* the running image is the build the file was linked against: its
       own copy of the ABI word (the stub's compare), read in place */
    if (rec->abi_at)
    {
        if (rd1(rec->abi_at & ~3u, &w0) < 0 || rd1((rec->abi_at & ~3u) + 4, &w1) < 0)
            return BCMJOB_OFF;
        sh = 8 * (rec->abi_at & 3);
        if ((sh ? w0 >> sh | w1 << (32 - sh) : w0) != rec->abi)
            return ovl_refuse(o, "image", false);
    }
    /* the region is rewritten: whatever another instance loaded there
       goes with it (the BCM drops its ops at OVERLAY too), and its load
       is due again at once (the swap: the merged image's DECODE and the
       visualizer share one region; visualizer.md 8.5).  Dropped here,
       before the FENCE below, so none of its ops is queued after the
       FENCE while the region is rewritten; a load of it under way ends */
    for (r = 0; r < BJC_OVL_N; r++)
        if (r != k)
        {
            ovl_idle(&b.ovl[r]);
            b.ovl[r].words = 0;
            if (b.ovl[r].ops)
            {
                b.ops &= ~b.ovl[r].ops;
                b.ovl[r].ops = 0;
                b.ovl[r].epoch = 0;
                if (r == BJC_OVL_DEC)
                    b.caps &= ~J_CAP_DEC_LENT;
                trace("bcmjob overlay evict k=%d by=%d", r, k);
            }
        }
    o->base = base;
    o->words = 0;
    /* no job in flight while the region is rewritten: a FENCE posted,
       its retirement polled at this and the next calls (BJC_OVL_AGAIN),
       never waited for inside one (sitting 4c: the UI pass stalled) */
    o->fseq = 0;
    o->t0 = b.p->now_us();
    if (b.inflight)
    {
        r = bcmjob_fence(&seq);
        if (r)
            return r;
        o->fseq = seq;
    }
    o->ph = OVL_PH_FENCE;
    return ovl_fenced(k, o);
}

int bjc_ovl_piece(int k, uint32_t off, const void *src, uint32_t bytes)
{
    uint32_t done = 0;
    const struct ovl *o;
    int err = 0;

    if (k < 0 || k >= BJC_OVL_N)
        return BCMJOB_FAILED;
    o = &b.ovl[k];
    if (enter() != ST_JOBS || o->epoch != b.epoch || !o->words)
        return BCMJOB_OFF;
    if ((off | bytes) & 3 || off + bytes > 4 * o->words)
        return BCMJOB_FAILED;
    b.p->lock();
    /* engine 2 in its whole pieces, the CPU for the rest (as a slot) */
    if (bytes >= 1024 && b.p->upload)
        done = b.p->upload(o->base + off, (const uint16_t *)src, bytes & ~63u);
    if (done < bytes)
        err = b.p->wr_block(o->base + off + done,
                            /* done: 0 or whole 64-byte pieces */
                            (const uint16_t *)((const uint8_t *)src + done), /* align-ok: src word-aligned, done %64 */
                            (bytes - done) / 2);
    b.p->unlock();
    return err ? BCMJOB_OFF : BCMJOB_OK;
}

/* The end's second half: OVERLAY's reply, looked for (one drain a call)
 * within the call's old bound (J_T_FENCE_BASE + J_T_OVERLAY) */
static int ovl_answer(int k, struct ovl *o)
{
    uint32_t ops, vpu = 0, caps = 0;
    int st;

    if (drain() < 0 || b.p->gen() != b.gen || o->epoch != b.epoch)
        st = BCMJOB_OFF;
    else if (o->x->got)
    {
        st = o->x->n ? (int)o->x->rep[0] : HP_E_INVAL;
        if (b.sent == o->sent)
        {
            b.inflight = false; /* in ring order: all before it ran, and
                                   nothing was put since (the call's) */
            mode_tell();
        }
    }
    else if (b.p->now_us() - o->t0 <= 1000 * (J_T_FENCE_BASE + J_T_OVERLAY))
        return BJC_OVL_AGAIN;
    else
        st = timed_out();
    ovl_idle(o);
    if (st < 0)
    {
        trace("bcmjob overlay refused why=wire rc=%d", st);
        if (st == BCMJOB_TIMEOUT)
            o->tries = 2;       /* not again this epoch (a BUSY may) */
        return st;
    }
    if (st != HP_OK)
    {
        trace("bcmjob overlay refused st=%d why=%s", st,
              st == J_E_SUM ? "sum" : st == HP_E_VERSION ? "version" : "inval");
        if (st == HP_E_VERSION)
            o->never = true;    /* another build: never retried (jobs.md) */
        else if (st != J_E_SUM)
            o->tries = 2;       /* J_E_SUM: one more try this epoch */
        return BCMJOB_FAILED;
    }
    /* the ops it added, read back (J_OPS), its start-up checks and its
       caps (DECODE's lent work slot) */
    if (rd1(b.block + J_OPS, &ops) < 0)
        return BCMJOB_OFF;
    rd1(b.block + J_VPU, &vpu);
    b.vpu = (uint8_t)vpu;
    if (rd1(b.block + J_CAPS, &caps) == 0 && J_CAPS_VALID(caps) &&
        k == BJC_OVL_DEC)
        b.caps |= caps & J_CAP_DEC_LENT;
    o->ops = ops & ~b.ops & (o->want | o->opt);
    b.ops |= o->ops;
    o->tries = 2;
    if (k == BJC_OVL_DEC)
        trace("bcmjob overlay ok k=1 ops=%08lx vpu=0x%lx mac=%s yuv=%s lent=%d",
              (unsigned long)ops, (unsigned long)vpu,
              vpu & J_VPU_MAC ? "vector" : "c", vpu & J_VPU_YUV ? "vector" : "c",
              (b.caps & J_CAP_DEC_LENT) != 0);
    else
        trace("bcmjob overlay ok ops=%08lx vpu=0x%lx fft=%s bars=%s%s lines=%s%s%s",
              (unsigned long)ops, (unsigned long)vpu, vpu & J_VPU_FFT ? "vector" : "c",
              vpu & J_VPU_BARS ? "vector" : "c", vpu & J_VPU_FLIP ? " flip" : "",
              ops & 1u << (J_OP_POLYLINE - 0x20) ? "P" : "-",
              ops & 1u << (J_OP_SEGMENTS - 0x20) ? "S" : "-",
              ops & 1u << (J_OP_PALROW - 0x20) ? "R" : "-");
    return (b.ops & o->want) == o->want ? BCMJOB_OK : BCMJOB_NOJOB;
}

int bjc_ovl_end(int k, uint32_t fnv, uint32_t a, uint32_t bb)
{
    uint32_t w[3];
    struct ovl *o;
    int st;

    if (k < 0 || k >= BJC_OVL_N)
        return BCMJOB_FAILED;
    o = &b.ovl[k];
    if (o->ph == OVL_PH_END)
        return ovl_answer(k, o);    /* the same load, polled */
    if (enter() != ST_JOBS || o->epoch != b.epoch || !o->words)
        return BCMJOB_OFF;
    w[0] = o->words;
    w[1] = a;
    w[2] = bb;
    o->words = 0;
    if (fnv != o->rec.fnv)
        return ovl_refuse(o, "file", true);
    /* OVERLAY posted with a reply tag, its answer looked for at this and
       the next calls (BJC_OVL_AGAIN), never waited for inside one (the
       BCM's sums, init and start-up checks: about 30 ms at 27 MHz) */
    o->x = box_arm(J_OP_OVERLAY);
    st = o->x ? put(J_OP_OVERLAY, o->x->tag, w, 3) : BCMJOB_BUSY;
    if (st)
    {
        ovl_idle(o);
        trace("bcmjob overlay refused why=wire rc=%d", st);
        return st;
    }
    o->t0 = b.p->now_us();
    o->sent = b.sent;
    o->ph = OVL_PH_END;
    return ovl_answer(k, o);
}

bool bjc_dec_lends(void)
{
    return enter() == ST_JOBS && (b.caps & J_CAP_DEC_LENT) && op_ok(J_OP_DECODE);
}

/* ---- power (G11, G12) ---------------------------------------------------- */

int bcm_power_want(enum bcm_want why, uint32_t hold_ms)
{
    uint32_t now = b.p->now_ms();

    hours();
    if (why < 1 || why >= BCM_WANT_N)
        return BCMJOB_FAILED;
    if (b.p->lit())
        return BCMJOB_OK;       /* lit is up anyway: nothing to hold */
    if (why == BCM_WANT_DECODE && !b.p->charging())
        return fault(J_OP_DECODE, BCMJOB_REFUSED);
    trace("bcm power want why=%s hold=%lu", want_name[why], (unsigned long)hold_ms);
    if (!b.wants && !b.p->gen())
    {
        b.ups[why]++;
        b.ups[0]++;
        if (b.p->power(true) < 0)
            return fault(0, BCMJOB_OFF);
        trace("bcm power state=C1 why=want-%s", want_name[why]);
    }
    b.wants |= 1u << why;
    b.want_until[why] = now + hold_ms;
    return BCMJOB_OK;
}

void bcm_power_release(enum bcm_want why)
{
    if (why < 1 || why >= BCM_WANT_N || !(b.wants & (1u << why)))
        return;
    b.wants &= ~(1u << why);
    if (!b.wants && !b.p->lit())
    {
        b.p->power(false);
        trace("bcm power state=C0 why=dark");
    }
}

uint32_t bcmjob_dark_ups(enum bcm_want why)
{
    uint32_t k, now = b.p->now_ms();

    /* a hold that ran out is released here (no timer: nothing polls) */
    for (k = 1; k < BCM_WANT_N; k++)
        if ((b.wants & (1u << k)) && (int32_t)(now - b.want_until[k]) >= 0)
            bcm_power_release((enum bcm_want)k);
    hours();
    return why < BCM_WANT_N ? b.ups[why] : 0;
}

/* ---- DECODE and read-back (P19 19-6; album-render.md 4.1, 11, 15) --------
 * Two threads, one writer (S0, MF-2):
 *  - the caller (the art thread) only reads the file into its pinned piece
 *    buffer when the owner asks, polls its cancel hook, and waits; its
 *    words in the request record are a_*.  The call (struct bjc_call, in
 *    the record) is read once at acceptance; the owner writes the answer
 *    there at the end, and reads the piece buffer and writes a read's
 *    rows only inside a pass (o_busy), never once the caller has had its
 *    acknowledgement (the state table below);
 *  - the ring owner runs every wire step in its passes (bjc_apply(), on
 *    its queue's kick).  A pass never waits for the BCM: the ALLOCs,
 *    COMMITs and the record are posted with reply tags and collected in
 *    later passes; FREEs a full ring refuses are put again later.  A pass
 *    writes at most one ring piece, or about 2 ms of rows, and none while
 *    a frame is due (bjc_frame_due()).
 * Every end (an answer, a cancel, a deadline, a cut or new epoch, a
 * suspend) finishes the call.  The caller has its own bound too: a kick
 * that went unanswered is posted again after BJC_KICK_STALE_MS, a call the
 * owner never took is withdrawn after BJC_ACK_MS, and a call it took but
 * never finished is left after the decode's deadline plus DEC_MARGIN_MS
 * (TIMEOUT: the CPU path), each once the owner acknowledges (S10, below).
 * Called on the owner itself (or with no owner set: the host tests), the
 * same passes run inline in the caller's loop.
 *
 * Power: a DECODE never powers the BCM (owner, 2026-10-24).  Dark on
 * battery answers REFUSED; any other time the BCM is not up, OFF: the
 * shell retries later or takes its CPU twin. */

#define RING_BYTES  (BCMJOB_DEC_RING_W * BCMJOB_DEC_RING_W * 2)
#define DEC_NONE    16          /* no pass yet (passes are 0-14) */
#define DEC_E2_MIN  2048        /* smaller ring pieces: the CPU's copy */
#define PASS_US     2000        /* an owner pass's own time, at most */
#define ART_WAIT_MS 10          /* the caller's wait, then a kick again */
#define DEC_MARGIN_MS 1000      /* the caller's bound past the deadline:
                                   the abort, the COMMITs */

enum { CALL_DECODE = 1, CALL_READ };
enum { PH_START = 1, PH_RING, PH_RING_COMMIT, PH_OUT, PH_WORK, PH_RECORD,
       PH_RUN, PH_COMMIT, PH_READ };
enum { ASK_NONE, ASK_OUT, ASK_HELD };   /* the piece: none, asked, in hand */

/* ---- the hand-off across cores (P19 S10) ----------------------------------
 * The call itself (struct bjc_call, bcmjob-core.h) is in the request record
 * r: uncached on the device, never in the caller's frame.  What stays in
 * the caller's cached memory, and how each crosses (xcore.h rule 3), when
 * c.cross says the owner runs on the other core:
 *
 *   object          writer   hand-off
 *   r->c (params)   caller   uncached; written before a_seq (fence)
 *   r->c (answer)   owner    uncached; ret, res, edges before o_seq (fence)
 *   piece buffer    caller   fetched, its last word padded, published
 *                            (own->publish) before a_got/a_have; the owner
 *                            reads it by DMA (engine 2 reads memory) or,
 *                            on the CPU's copy, through own->uncached; it
 *                            never writes it.  BJC_LINE-aligned, both ends
 *   read's px       owner    the caller publishes the span before the
 *                            post (no dirty line of its own can land over
 *                            the rows); the owner writes the rows through
 *                            own->uncached, except the span's first and
 *                            last lines when they hold memory outside the
 *                            read: those pixels go to r->c.edge; the
 *                            caller acquires the span after the end, then
 *                            copies the edge bytes in on its own core
 *
 * On one core (cross false: today's build, and the owner's inline calls)
 * none of it runs and the owner uses the caller's pointers as before.
 *
 * Giving up (the withdraw).  The caller sets a_quit, fences, and then
 * waits, at most BJC_ACK_MS, for the owner's acknowledgement: o_busy no
 * longer naming the call.  The owner, at every pass, sets o_busy before
 * it looks at a_quit (fenced), and clears o_busy only after its last
 * touch of the caller's memory.  Of two such store-then-load pairs, at
 * least one side sees the other's store (Dekker), so once the caller has
 * seen o_busy differ after its a_quit, the owner can never again pass its
 * a_quit check for that call.  States of call s, as the caller sees them
 * after its a_quit (all words uncached):
 *
 *   o_busy  o_take  o_seq  state      caller                 owner
 *   == s    any     any    IN PASS    waits (BJC_ACK_MS)     finishes the
 *                                                            pass, clears
 *   != s    any     == s   DONE       the answer stands      -
 *                                     (copied out: it won
 *                                     the race)
 *   != s    != s    != s   WITHDRAWN  OFF; may post again    at its take
 *                                     at once (the block is  sees a_quit:
 *                                     never read again)      o_seq = s,
 *                                                            reads nothing
 *   != s    == s    != s   LEFT       TIMEOUT; BUSY until    at its next
 *                                     the owner ends it      pass ends it
 *                                                            unseen
 *   == s past BJC_ACK_MS   LEAKED     TIMEOUT, traced loud   stuck inside
 *                                     (bjc_leaks); the       a pass; ends
 *                                     piece buffer stays     it later
 *                                     pinned and every call
 *                                     BUSY until o_busy
 *                                     clears
 *
 * The take is the same handshake: o_busy = s, fence, then a_seq still s
 * and a_quit not s, or nothing is read.  Inside a pass the owner checks
 * a_quit again at each read row (a piece is one per pass: its boundary is
 * the pass's start).  Every word keeps one writer, so no compare-and-swap
 * is needed. */

/* The owner's side of the call it runs (the owner's alone) */
static struct
{
    uint32_t seq;               /* the call accepted (0: none) */
    bool cross;                 /* the caller is on the other core */
    uint8_t *buf;               /* a decode's piece buffer (the caller's) */
    struct
    {
        uint32_t slot, x, y, w, h, pitch;
        uint16_t *px;           /* the caller's */
        uintptr_t edge_at[2];   /* r->c.edge_at, the owner's copy */
    } rd;                       /* a read */
    uint8_t kind, ph, ask, k;   /* k: the output (PH_OUT, PH_COMMIT) */
    bool cancelled, abort_due, aborted, start_ask, wrote;
    bool have[J_DEC_MAX];
    int n;
    struct bcmjob_dec_out out[J_DEC_MAX];
    struct bcmjob_dec_res res;  /* the answer, copied back at the end */
    uint32_t epoch, dl, t0, t_feed, t_svc;
    uint32_t len, ring;         /* the file's bytes; the ring's address */
    uint32_t cur, last;         /* the pass fed (DEC_NONE), the last seen */
    uint32_t written;           /* bytes of the file in this pass */
    uint32_t consumed;          /* the BCM's units this pass (absolute) */
    uint32_t ask_no, ask_n, ask_at, ask_pass;
    uint32_t row;               /* CALL_READ: rows done */
    uint32_t work;              /* the lent work slot (J_CAP_DEC_LENT) */
    bool lent;                  /* this decode lends one (PH_WORK) */
    bool work_held;             /* ... ALLOCated: FREEd at the end */
    struct box *x;              /* the reply awaited (one at a time) */
    uint32_t op, op_t0, op_us, op_w, op_h;      /* ... its op, deadline */
} BJC_OWN_LINES j;

void bjc_frame_due(bool (*fn)(void))
{
    b.frame_due = fn;
}

static bool frame_due(void)
{
    return b.frame_due && b.frame_due();
}

static bool art_inline(void)
{
    return !own || !own->is_owner || own->is_owner();
}

/* The caller's wake (the owner asked for a piece, or finished) */
static void art_wake(void)
{
    if (!art_inline() && own->art_wake)
        own->art_wake();
}

/* The file is wholly in the ring: later passes read it in place */
static bool dec_in_ring(void)
{
    return j.len <= RING_BYTES && j.cur != DEC_NONE && j.written == j.len;
}

/* J_DEC_IN_WR: (pass, units written) or J_DEC_ABORT; the lock held */
static int dec_publish(uint32_t v)
{
    return b.p->wr(b.block + J_DEC_IN_WR, HP_ENC(v));
}

static void dec_abort(void)
{
    if (j.aborted || j.cur == DEC_NONE)
    {
        j.abort_due = true;     /* at the job's first pass: its start sets
                                   the word to J_DEC_IDLE */
        return;
    }
    b.p->lock();
    dec_publish(J_DEC_ABORT);
    b.p->unlock();
    j.aborted = true;
}

/* FREE behind everything queued (ring order: after the DECODE ends); a
 * full ring keeps it for the owner's next passes, never a wait */
static void dec_free(uint32_t slot)
{
    if (b.fpend_epoch != b.epoch)
        b.nfpend = 0;           /* those slots went with their epoch */
    b.fpend_epoch = b.epoch;
    if (put(J_OP_FREE, 0, &slot, 1) == BCMJOB_BUSY &&
        b.nfpend < sizeof(b.fpend) / sizeof(b.fpend[0]))
        b.fpend[b.nfpend++] = slot;
}

static void free_pending(void)
{
    if (b.fpend_epoch != b.epoch || !b.gen || b.p->gen() != b.gen)
        b.nfpend = 0;
    while (b.nfpend && put(J_OP_FREE, 0, &b.fpend[b.nfpend - 1], 1) == 0)
        b.nfpend--;
}

/* Past the DECODE's deadline (jobs.md section 6): a dead heartbeat is the
 * BCM's failure; a live one makes the op running (J_CUR_OP, else the
 * DECODE) slow, marked on its second miss this boot */
static int dec_timed_out(void)
{
    uint32_t op = J_OP_DECODE, bit;
    int32_t c;

    if (!alive())
    {
        chip_failed("dead");
        return BCMJOB_TIMEOUT;
    }
    b.p->lock();
    c = index_at(b.block + J_CUR_OP);
    b.p->unlock();
    if (c >= 0x20 && c < 0x40)
        op = (uint32_t)c;
    bit = 1u << (op - 0x20);
    if (b.slow_once & bit)
        b.slow |= bit;
    b.slow_once |= bit;
    return fault(op, BCMJOB_TIMEOUT);
}

/* The default deadline's clock: J_POWER's MHz (27 if the word has none) */
static uint32_t dec_mhz(void)
{
    uint32_t v;

    if (rd1(b.block + J_POWER, &v) < 0 || (v >> 16) < 10 || (v >> 16) > 400)
        return 27;
    return v >> 16;
}

/* ---- one awaited op at a time, never waited for in a pass --------------- */

/* Post op with a reply tag: 0, or BUSY / OFF */
static int op_post(uint32_t op, const uint32_t *a, uint32_t n, uint32_t ms)
{
    int e;

    j.x = box_arm(op);
    if (!j.x)
        return BCMJOB_BUSY;
    e = put(op, j.x->tag, a, n);
    if (e)
    {
        j.x->tag = 0;
        j.x = NULL;
        return e;
    }
    j.op = op;
    j.op_t0 = b.p->now_us();
    j.op_us = 1000 * ms;
    return 0;
}

static void op_drop(void)
{
    if (j.x)
        j.x->tag = 0;
    j.x = NULL;
}

/* The awaited reply: 1 came (status in j.x->rep[0]), 0 not yet, < 0 the
 * answer (OFF: cut, restarted, a new epoch; TIMEOUT past its deadline:
 * the box goes, and a late ALLOC is freed by drain()) */
static int op_poll(void)
{
    if (drain() < 0 || b.p->gen() != b.gen || b.epoch != j.epoch)
        return BCMJOB_OFF;
    if (j.x->got)
        return 1;
    if (b.p->now_us() - j.op_t0 > j.op_us)
    {
        op_drop();
        return timed_out();
    }
    return 0;
}

/* The awaited reply's status (and the box free for the next) */
static int op_status(uint32_t *rep, uint32_t cap)
{
    int st = j.x->n ? (int)j.x->rep[0] : HP_E_INVAL;

    if (cap > j.x->n)
        cap = j.x->n;
    memcpy(rep, j.x->rep, 4 * cap);
    op_drop();
    if (st > 0)
        job_fail(j.op, st, rep, cap);
    return st;
}

/* ---- the call's end ------------------------------------------------------ */

/* The call is over: its answer into the record (the driver's memory, so
 * written whether or not the caller is still there; a caller that left
 * posts nothing new until o_seq says this one ended), then o_seq */
static void job_end(int ret)
{
    if (j.kind == CALL_DECODE)
        r->c.res = j.res;
    r->c.ret = ret;
    BJC_FENCE();
    r->o_seq = j.seq;
    j.seq = 0;
    art_wake();
}

/* A decode's end: FREEs unless OK, the trace */
static void dec_end(int ret)
{
    uint32_t k;

    op_drop();
    /* no partial pixels left READY: a FREE per output behind the job
       (ring order: after it ends, even past a timeout); gone anyway with
       their epoch */
    if (ret != BCMJOB_OK && b.gen && b.p->gen() == b.gen && b.epoch == j.epoch)
        for (k = 0; k < (uint32_t)j.n; k++)
            if (j.have[k])
                dec_free(j.res.out[k].slot);
    /* the lent work slot back to the pool, whatever the answer, behind
       the DECODE in ring order (the drift uploads its cover again) */
    if (j.work_held && b.gen && b.p->gen() == b.gen && b.epoch == j.epoch)
        dec_free(j.work);
    j.work_held = false;
    j.res.stalled = j.res.stalled && ret == BCMJOB_FAILED;
    trace("bcmjob decode done st=%d err=%lu passes=%lu bytes=%lu ms=%lu", ret,
          (unsigned long)j.res.err, (unsigned long)j.res.passes,
          (unsigned long)j.res.bytes,
          (unsigned long)((b.p->now_us() - j.t0) / 1000));
    job_end(ret);
}

/* A wire step's status: < 0 a BCMJOB_* answer, else FAILED with it */
static void dec_failed(int st)
{
    if (st < 0)
        dec_end(st == BCMJOB_TIMEOUT ? st : fault(J_OP_DECODE, st));
    else
    {
        j.res.err = st;
        dec_end(BCMJOB_FAILED);
    }
}

/* ---- the setup and the COMMITs: one step per call, posted or collected -- */

/* ALLOC's reply: the slot's facts (alloc()'s, without the wait) */
static int alloc_took(const uint32_t *rep, uint32_t w, uint32_t h,
                      uint32_t *slot, bool *hit)
{
    if (rep[1] >= BJC_SLOTS)
        return HP_E_INVAL;
    *slot = rep[1];
    *hit = (rep[4] & J_ALLOC_HIT) != 0;
    b.slot[rep[1]].addr = rep[2];
    b.slot[rep[1]].pitch = rep[3];
    b.slot[rep[1]].w = w;
    b.slot[rep[1]].h = h;
    b.slot[rep[1]].ep = (uint16_t)b.epoch;
    b.slot[rep[1]].staged = false;
    return HP_OK;
}

static int post_alloc(uint32_t cls, uint32_t w, uint32_t h, uint32_t lo,
                      uint32_t hi)
{
    uint32_t a[5] = { cls, J_XY(w, h), J_FMT_RGB565, lo, hi };

    j.op_w = w;
    j.op_h = h;
    return op_post(J_OP_ALLOC, a, 5, J_T_FENCE_BASE + J_T_SMALL);
}

static int post_commit(uint32_t slot)
{
    uint32_t a[4] = { slot, 0, 0, J_COMMIT_NOCHECK };
    const struct bjc_slot *t = &b.slot[slot < BJC_SLOTS ? slot : 0];

    return op_post(J_OP_COMMIT, a, 4, J_T_FENCE_BASE +
                   J_T_COMMIT((uint32_t)t->w * t->h * 2));
}

/* The record: the pass the BCM's word names now (the next one is this
 * job's), then DECODE with a reply tag.  1 sent, 0 not yet (a torn word),
 * < 0 the answer */
static int dec_record(void)
{
    uint32_t pay[J_DEC_ARGS(J_DEC_MAX)], k;
    int32_t w;
    int e;

    b.p->lock();
    w = index_at(b.block + J_DEC_IN_RD);
    b.p->unlock();
    if (w < 0)
        return 0;
    j.ring = b.slot[b.ring_slot].addr;
    j.cur = DEC_NONE;
    j.last = J_DEC_PASS((uint32_t)w);
    if (!j.dl)
    {
        j.dl = BCMJOB_T_DECODE(j.len, dec_mhz());
        if (j.dl > BCMJOB_DEC_CAP_MS)
            j.dl = BCMJOB_DEC_CAP_MS;   /* bcmjob.h: one decode, a few s */
    }
    /* the merged image's overlay (J_CAP_DEC_LENT) has no
       decode_prepare(): the file's own length in J_DEC_LEN, plain and
       whole before the record, the record's len clipped to the ring
       (jref's bound; jobs.h) */
    if (j.lent)
    {
        b.p->lock();
        e = b.p->wr(b.block + J_DEC_LEN, j.len);
        b.p->unlock();
        if (e < 0)
            return fault(J_OP_DECODE, BCMJOB_OFF);
    }
    /* the misses only, in request order (bcmjob.h): a hit is never a
       destination */
    pay[0] = b.ring_slot | J_DEC_RING;
    pay[1] = j.lent && j.len > RING_BYTES ? RING_BYTES : j.len;
    pay[2] = j.res.miss_n;
    for (k = 0; k < j.res.miss_n; k++)
    {
        const struct bcmjob_dec_out *o = &j.out[j.res.miss[k]];
        uint32_t *v = pay + 3 + 5 * k;

        v[0] = J_XY(o->dw, o->dh);
        v[1] = J_XY(o->cx, o->cy);
        v[2] = J_XY(o->cw, o->ch);
        v[3] = j.res.out[j.res.miss[k]].slot;
        v[4] = J_XY(0, 0);
    }
    j.x = box_arm(J_OP_DECODE);
    e = j.x ? put(J_OP_DECODE, j.x->tag, pay, J_DEC_ARGS(j.res.miss_n)) :
        BCMJOB_BUSY;
    if (e)
    {
        op_drop();
        return fault(J_OP_DECODE, e);
    }
    j.op = J_OP_DECODE;
    b.dec_pend = j.x->tag;
    b.dec_pos = b.sent - 1;     /* the jobs ahead of it */
    b.dec_live = j.x->tag;      /* owner calls answer busy-decode now */
    b.dec_live_until = 0;
    if (b.verbose)
        trace("bcmjob submit op=0x%02x tag=%lu n=%lu", J_OP_DECODE,
              (unsigned long)(j.x->tag & 0x7FFF),
              (unsigned long)J_DEC_ARGS(j.res.miss_n));
    j.t0 = j.t_feed = j.t_svc = b.p->now_us();  /* the deadline: from the
                                                   queueing */
    return 1;
}

/* One step of the setup or the COMMITs.  true: it moved (try another
 * this pass); false: wait for a later pass, or the call ended */
static bool dec_step(void)
{
    uint32_t rep[5];
    int st;
    bool hit = false;

    switch (j.ph)
    {
    case PH_START:
        st = enter();
        if (st != ST_JOBS)
        {
            /* never a power-up for a decode */
            dec_end(fault(J_OP_DECODE, !b.p->lit() && !b.p->charging() ?
                          BCMJOB_REFUSED : BCMJOB_OFF));
            return false;
        }
        if (!op_ok(J_OP_DECODE) || !op_ok(J_OP_ALLOC) || !op_ok(J_OP_COMMIT))
        {
            dec_end(fault(J_OP_DECODE, BCMJOB_NOJOB));
            return false;
        }
        j.epoch = b.epoch;
        j.res.epoch = j.epoch;
        /* the merged image's overlay: its work area is a slot we lend */
        j.lent = (b.caps & J_CAP_DEC_LENT) != 0;
        j.ph = b.ring_epoch == b.epoch ? PH_OUT : PH_RING;
        return true;
    case PH_RING:               /* the input ring, once per epoch */
    case PH_RING_COMMIT:
    case PH_OUT:
    case PH_WORK:               /* the lent work slot, after the outputs */
    case PH_COMMIT:
        if (!j.x)
        {
            if (j.ph == PH_RING)
                st = post_alloc(J_CLS_SCRATCH, BCMJOB_DEC_RING_W,
                                BCMJOB_DEC_RING_W, BCMJOB_DEC_RING_KEY, 0);
            else if (j.ph == PH_RING_COMMIT)
                st = post_commit(b.ring_slot);
            else if (j.ph == PH_OUT)
                st = post_alloc(j.out[j.k].cls, j.out[j.k].cw, j.out[j.k].ch,
                                j.out[j.k].key_lo, j.out[j.k].key_hi);
            else if (j.ph == PH_WORK)
                st = post_alloc(J_CLS_DRIFT, J_DEC_WORK_SIDE, J_DEC_WORK_SIDE,
                                J_DEC_WORK_KEY, 0);
            else
                st = post_commit(j.res.out[j.k].slot);
            if (st)
            {
                dec_failed(st);
                return false;
            }
        }
        st = op_poll();
        if (st <= 0)
        {
            if (st < 0)
                dec_failed(st);
            return false;
        }
        st = op_status(rep, 5);
        if (st == HP_OK && (j.ph == PH_RING || j.ph == PH_OUT || j.ph == PH_WORK))
            st = alloc_took(rep, j.op_w, j.op_h,
                            j.ph == PH_RING ? &b.ring_slot :
                            j.ph == PH_WORK ? &j.work : &j.res.out[j.k].slot,
                            &hit);
        if (st != HP_OK)
        {
            if (j.ph == PH_WORK && st == J_E_NOSLOT)
                trace("bcmjob decode why=nowork");  /* the pool was full */
            dec_failed(st);
            return false;
        }
        if (j.ph == PH_RING)
            j.ph = hit ? PH_OUT : PH_RING_COMMIT;
        else if (j.ph == PH_RING_COMMIT)
            j.ph = PH_OUT;
        else if (j.ph == PH_WORK)
        {
            /* a hit is a work slot an earlier decode's FREE hasn't reached
               yet: used, and left to that FREE */
            j.work_held = !hit;
            j.ph = PH_RECORD;
        }
        else if (j.ph == PH_OUT)
        {
            j.res.out[j.k].hit = hit;
            j.have[j.k] = !hit; /* a hit is another holder's: never
                                   freed by this call */
            if (!hit)           /* the reduced set: the DECODE's out[] */
                j.res.miss[j.res.miss_n++] = j.k;
            if (++j.k < j.n)
                return true;
            j.k = 0;
            /* all of them held already: their sums, nothing decoded */
            j.ph = !j.res.miss_n ? PH_COMMIT : j.lent ? PH_WORK : PH_RECORD;
            if (j.res.miss_n)
            {
                struct bcmjob_dec_out set[J_DEC_MAX];

                /* the work of the set sent, the misses (bcmjob.h): never
                   sent over; the misses freed, the hits kept.  A lent
                   work slot is smaller than the promise (jobs.h) */
                if (bcmjob_dec_work(set, bcmjob_dec_set(j.out, &j.res, set)) >
                    (j.lent ? J_DEC_WORK_LENT : J_DEC_WORK_MIN))
                {
                    j.res.err = HP_E_NOMEM;
                    dec_end(BCMJOB_FAILED);
                    return false;
                }
            }
        }
        else
        {
            j.res.out[j.k].a = rep[1];
            j.res.out[j.k].b = rep[2];
            if (++j.k < j.n)
                return true;
            dec_end(BCMJOB_OK);
            return false;
        }
        if (j.ph == PH_OUT || j.ph == PH_COMMIT)
            j.k = 0;
        if (j.ph != PH_RING_COMMIT)
            b.ring_epoch = b.epoch;
        return true;
    case PH_RECORD:
        st = dec_record();
        if (st < 0)
            dec_end(st);
        else if (st > 0)
            j.ph = PH_RUN;
        return false;           /* the feed starts at the next pass */
    }
    return false;
}

/* ---- the feed ------------------------------------------------------------- */

/* Ask the caller for the next piece, if the ring has room for one */
static void dec_ask(void)
{
    uint32_t room = RING_BYTES - (j.written - j.consumed * J_DEC_UNIT);
    uint32_t at = j.written & (RING_BYTES - 1), n = j.len - j.written;

    if (n > BCMJOB_DEC_PIECE)
        n = BCMJOB_DEC_PIECE;
    if (n > room)
        n = room;
    if (n > RING_BYTES - at)
        n = RING_BYTES - at;
    if (!n || (n < J_DEC_UNIT && j.written + n < j.len))
        return;                 /* wait for room */
    j.ask_n = n;
    j.ask_at = j.written;
    j.ask_pass = j.cur;
    r->o_want_n = n;
    r->o_want_start = j.start_ask;
    j.ask_no = r->o_want + 1;   /* counts on across calls */
    BJC_FENCE();                /* the ask's size before its number */
    r->o_want = j.ask_no;
    j.ask = ASK_OUT;
    art_wake();
}

/* The caller's memory as the owner reads or writes it: the uncached
 * alias across cores (S10), else the pointer itself */
static void *own_alias(void *p)
{
    return j.cross ? own->uncached(p) : p;
}

/* The piece in hand into the ring (engine 2, the CPU for small pieces and
 * the rest), then the index.  0, or -1 the port gave up.  The caller
 * padded the file's last word and published the piece (S10); the owner
 * only reads it: engine 2 from memory, the CPU's copy through the alias */
static int dec_write(void)
{
    uint8_t *buf = j.buf;       /* the caller is waiting: o_busy */
    const uint8_t *cpu = own_alias(buf);
    uint32_t n = j.ask_n, at = j.written & (RING_BYTES - 1);
    uint32_t m = (n + 3) & ~3u, done = 0;
    int err = 0;

    b.p->lock();
    if (m >= DEC_E2_MIN && b.p->upload_any)
        done = b.p->upload_any(j.ring + at, buf, m) & ~3u;
    if (done < m)
        err = b.p->wr_block(j.ring + at + done, (const uint16_t *)(cpu + done),
                            (m - done) / 2);
    j.written += n;
    if (!err)
        err = dec_publish(J_DEC_IDX(j.cur, (j.written + J_DEC_UNIT - 1) /
                                           J_DEC_UNIT));
    b.p->unlock();
    j.res.bytes += n;
    if (j.start_ask)
        j.res.passes++;
    j.start_ask = false;
    j.ask = ASK_NONE;
    j.wrote = true;
    return err ? -1 : 0;
}

/* Serve the ring once (Stream.service()).  0, or -1 the port gave up. */
static int dec_service(void)
{
    uint32_t p, u, wu, now;
    int32_t w;

    b.p->lock();
    w = index_at(b.block + J_DEC_IN_RD);
    b.p->unlock();
    if (w < 0)
        return 0;               /* torn: next time */
    p = J_DEC_PASS((uint32_t)w);
    u = J_DEC_UNITS((uint32_t)w);
    if (p != j.cur && p != j.last)
    {
        /* the BCM went back to byte 0 (its J_DEC_IN_WR is J_DEC_IDLE
           until this); a piece of the old pass is dropped */
        j.cur = j.last = p;
        j.written = j.consumed = 0;
        j.start_ask = true;
        if (j.ask == ASK_HELD)
            j.ask = ASK_NONE;
        if (!j.abort_due)
        {
            int e;

            b.p->lock();
            e = dec_publish(J_DEC_IDX(p, 0));
            b.p->unlock();
            if (e)
                return -1;
        }
    }
    if (j.abort_due && !j.aborted && j.cur != DEC_NONE)
        dec_abort();
    if (j.aborted || p != j.cur)
        return 0;
    wu = (j.written + J_DEC_UNIT - 1) / J_DEC_UNIT;
    j.consumed = wu - ((wu - u) & 0xFFF);       /* absolute units */
    /* the PP has owed the BCM bytes (it has read all there are) for
       J_DEC_STALL_MS: it may have given up (its CORRUPT is not the
       file's) */
    now = b.p->now_us();
    if (j.written >= j.len || j.consumed * J_DEC_UNIT < j.written)
        j.t_feed = now;
    else if (now - j.t_feed >= J_DEC_STALL_MS * 1000u)
        j.res.stalled = true;
    /* the caller's piece came: a read error aborts; a stale one (asked
       for an earlier pass) is dropped */
    if (j.ask == ASK_OUT && r->a_have == j.ask_no)
    {
        BJC_FENCE();            /* a_got and the piece after a_have */
        j.ask = ASK_HELD;
        if (j.ask_pass != j.cur || j.ask_at != j.written)
            j.ask = ASK_NONE;
        else if (r->a_got != (int32_t)j.ask_n)
        {
            j.res.read_err = true;
            j.ask = ASK_NONE;
            dec_abort();
            return 0;
        }
    }
    /* the budget: one piece a pass, none while a frame is due */
    if (j.ask == ASK_HELD && !j.wrote && !frame_due())
    {
        if (dec_write() < 0)
            return -1;
        j.t_feed = now;
    }
    if (j.ask == ASK_NONE && j.written < j.len)
        dec_ask();
    return 0;
}

/* The reply came: the status, then sw | sh << 16; OK goes on to the
 * COMMITs (later passes) */
static void dec_reply(void)
{
    uint32_t rep[2];
    int st = op_status(rep, 2);

    b.dec_live = 0;
    if (j.cancelled)
    {
        dec_end(BCMJOB_ABORTED);
        return;
    }
    if (st != HP_OK)
    {
        j.res.err = st;
        dec_end(BCMJOB_FAILED);
        return;
    }
    j.res.sw = J_X(rep[1]);
    j.res.sh = J_Y(rep[1]);
    j.k = 0;
    j.ph = PH_COMMIT;
}

/* One owner pass of a streaming decode */
static void dec_run(void)
{
    /* no look at the ring for J_DEC_STALL_MS (a pass that came late, or
       the file's read held it): the BCM may have given up for want of
       bytes, so its CORRUPT is not the file's */
    if (j.cur != DEC_NONE && !j.aborted && j.written < j.len &&
        b.p->now_us() - j.t_svc >= J_DEC_STALL_MS * 1000u)
        j.res.stalled = true;
    j.t_svc = b.p->now_us();
    if (drain() < 0 || b.p->gen() != b.gen || b.suspended || b.epoch != j.epoch)
    {
        /* the job is gone with its epoch; a BCM still up stops waiting
           for bytes at once rather than after its stall */
        if (b.gen && b.p->gen() == b.gen && !b.suspended && !j.aborted &&
            j.cur != DEC_NONE && !dec_in_ring())
            dec_abort();
        dec_end(fault(J_OP_DECODE, BCMJOB_OFF));
        return;
    }
    if (j.x->got)
    {
        dec_reply();
        return;
    }
    if (!b.inflight)
    {
        b.inflight = true;      /* job mode while it runs (another reply
                                   may have cleared it) */
        mode_tell();
    }
    if (!j.aborted && dec_service() < 0)
    {
        dec_end(fault(J_OP_DECODE, BCMJOB_OFF));
        return;
    }
    if (!j.cancelled && r->a_stop == j.seq)
    {
        /* a file still streaming is aborted; one wholly in the ring runs
           to its end, waited for, and is dropped */
        j.cancelled = true;
        fault(J_OP_DECODE, BCMJOB_ABORTED);
        if (!dec_in_ring())
            dec_abort();
    }
    if (b.p->now_us() - j.t0 > j.dl * 1000u)
    {
        if (!dec_in_ring())
            dec_abort();
        /* its abort or stall ends it soon; owner calls stay busy-decode
           until then, at most this long */
        b.dec_live_until = b.p->now_us() + 2000u * J_DEC_STALL_MS;
        j.x->tag = 0;           /* its late reply clears dec_live */
        j.x = NULL;
        dec_end(dec_timed_out());
    }
}

/* The caller left past its own bound: end without its memory */
static void dec_leave(int ret)
{
    if (j.ph == PH_RUN)
    {
        if (!dec_in_ring())
            dec_abort();
        b.dec_live_until = b.p->now_us() + 2000u * J_DEC_STALL_MS;
        j.x->tag = 0;
        j.x = NULL;
    }
    fault(J_OP_DECODE, ret);
    dec_end(ret);
}

/* ---- the read-back --------------------------------------------------------- */

/* One row of w pixels from a (a halfword address) by word reads */
static int read_row(uint32_t a, uint16_t *d, uint32_t w)
{
    uint32_t v, k = 0;

    if (a & 2)
    {
        if (b.p->rd(a - 2, &v) < 0)
            return -1;
        d[k++] = (uint16_t)(v >> 16);
        a += 2;
    }
    for (; k + 2 <= w; k += 2, a += 4)
    {
        if (b.p->rd(a, &v) < 0)
            return -1;
        d[k] = (uint16_t)v;
        d[k + 1] = (uint16_t)(v >> 16);
    }
    if (k < w)
    {
        if (b.p->rd(a, &v) < 0)
            return -1;
        d[k] = (uint16_t)v;
    }
    return 0;
}

/* A read's span in bytes: px to the end of its last row (0: no rows) */
static uint32_t read_span(uint32_t w, uint32_t h, uint32_t pitch)
{
    return w && h && pitch >= w ? 2 * ((h - 1) * pitch + w) : 0;
}

/* Across cores (S10): the first and last cache lines of px's span that
 * hold bytes outside it.  The owner's pixels for them go to r->c.edge;
 * the caller copies them in on its own core after the end */
static void read_edges(void)
{
    uintptr_t s = (uintptr_t)j.rd.px;
    uintptr_t e = s + read_span(j.rd.w, j.rd.h, j.rd.pitch);
    uintptr_t l0 = s & ~(uintptr_t)(BJC_LINE - 1);
    uintptr_t l1 = (e - 1) & ~(uintptr_t)(BJC_LINE - 1);

    j.rd.edge_at[0] = (s & (BJC_LINE - 1)) || e < l0 + BJC_LINE ? l0 : 0;
    j.rd.edge_at[1] = (e & (BJC_LINE - 1)) && l1 != l0 ? l1 : 0;
    r->c.edge_at[0] = j.rd.edge_at[0];
    r->c.edge_at[1] = j.rd.edge_at[1];
    r->c.edge_set[0] = r->c.edge_set[1] = 0;
}

/* Accept a read: OFF in another epoch, FAILED outside the slot */
static void read_begin(void)
{
    const struct bjc_slot *t;

    if (enter() != ST_JOBS)
    {
        job_end(BCMJOB_OFF);
        return;
    }
    if (j.rd.slot >= BJC_SLOTS || !b.slot[j.rd.slot].w)
    {
        job_end(BCMJOB_FAILED);
        return;
    }
    t = &b.slot[j.rd.slot];
    if (t->ep != (uint16_t)b.epoch)
        job_end(BCMJOB_OFF);
    else if (!j.rd.w || !j.rd.h || j.rd.x + j.rd.w > t->w ||
             j.rd.y + j.rd.h > t->h || j.rd.pitch < j.rd.w ||
             t->staged)     /* a staged slot's own address is the BCM's
                               alone */
        job_end(BCMJOB_FAILED);
    else
    {
        j.epoch = b.epoch;
        j.row = 0;
        j.ph = PH_READ;
        if (j.cross)
            read_edges();
    }
}

/* One row of a read into the caller's d (from a, a halfword address).
 * Across cores through the alias, and the pixels that fall in an edge
 * line into r->c.edge (S10) */
static int read_out(uint32_t a, uint16_t *d, uint32_t w)
{
    uintptr_t p = (uintptr_t)d, e = p + 2 * w;

    if (!j.cross)
        return read_row(a, d, w);
    while (p < e)
    {
        uintptr_t end = e;
        uint16_t *to = NULL;
        int k, in = -1;

        for (k = 0; k < 2; k++)
        {
            uintptr_t l = j.rd.edge_at[k];

            if (l && p >= l && p < l + BJC_LINE)
                in = k;
        }
        if (in >= 0)
        {
            uintptr_t l = j.rd.edge_at[in];

            if (end > l + BJC_LINE)
                end = l + BJC_LINE;
            to = (uint16_t *)(void *)(r->c.edge[in] + (p - l)); /* align-ok: p is a pixel's (even), l a line's, edge[] word-aligned */
        }
        else
        {
            for (k = 0; k < 2; k++)
            {
                uintptr_t l = j.rd.edge_at[k];

                if (l && l > p && l < end)
                    end = l;
            }
            to = own->uncached((void *)p);
        }
        if (read_row(a + (uint32_t)(p - (uintptr_t)d), to,
                     (uint32_t)(end - p) / 2) < 0)
            return -1;
        if (in >= 0)
        {
            uint32_t lo = (uint32_t)(p - j.rd.edge_at[in]);
            uint32_t hi = (uint32_t)(end - j.rd.edge_at[in]);

            r->c.edge_set[in] |= (uint16_t)(((1u << hi) - 1) & ~((1u << lo) - 1));
        }
        p = end;
    }
    return 0;
}

/* Rows for about PASS_US, none while a frame is due; the port lock per
 * row; a_quit checked at each row (S10); the epoch checked after the
 * last */
static void read_pass(void)
{
    const struct bjc_slot *t = &b.slot[j.rd.slot];
    uint32_t t0 = b.p->now_us();
    int e;

    if (!b.gen || b.p->gen() != b.gen || b.suspended || b.epoch != j.epoch)
    {
        job_end(BCMJOB_OFF);
        return;
    }
    while (j.row < j.rd.h && !frame_due())
    {
        if (r->a_quit == j.seq)
            return;             /* the caller gave up: the next pass ends
                                   it, the rows untouched from here */
        b.p->lock();
        e = read_out(t->addr + (j.rd.y + j.row) * t->pitch + 2 * j.rd.x,
                     j.rd.px + j.row * j.rd.pitch, j.rd.w);
        b.p->unlock();
        if (e < 0)
        {
            job_end(BCMJOB_OFF);
            return;
        }
        j.row++;
        if (b.p->now_us() - t0 >= PASS_US)
            break;
    }
    if (j.row < j.rd.h)
        return;
    /* the epoch after: a cut, a restart or an epoch event meanwhile */
    job_end(drain() < 0 || enter() != ST_JOBS || b.epoch != j.epoch ?
            BCMJOB_OFF : BCMJOB_OK);
}

/* ---- the owner's pass -------------------------------------------------------- */

/* Take call s: the record copied once into the owner's own state */
static void own_take(uint32_t s)
{
    const struct bjc_call *c = &r->c;

    memset(&j, 0, sizeof(j));
    j.seq = s;
    j.kind = c->kind;
    j.cross = c->cross && own && own->uncached;
    r->o_take = s;
    if (j.kind == CALL_READ)
    {
        j.rd.slot = c->slot;
        j.rd.x = c->x;
        j.rd.y = c->y;
        j.rd.w = c->w;
        j.rd.h = c->h;
        j.rd.pitch = c->pitch;
        j.rd.px = c->px;
        read_begin();
    }
    else
    {
        j.buf = c->buf;
        j.n = c->n <= J_DEC_MAX ? (int)c->n : J_DEC_MAX;
        memcpy(j.out, c->out, j.n * sizeof(j.out[0]));
        j.len = c->len;
        j.dl = c->ms;           /* 0: the default, capped (setup) */
        j.t0 = b.p->now_us();
        j.ph = PH_START;
        trace("bcmjob decode len=%lu n=%d", (unsigned long)j.len, j.n);
    }
}

static void own_pass(void)
{
    uint32_t s, t0;

    if (b.in_pass || !b.p)
        return;
    b.in_pass = true;
    free_pending();
    s = r->a_seq;
    if (!j.seq && s != r->o_seq)
    {
        /* the take (the state table above): o_busy before the caller's
           words, so a give-up and a take can't both go through */
        r->o_busy = s;
        BJC_FENCE();
        if (r->a_seq != s)
            ;                   /* withdrawn and posted again meanwhile:
                                   the next pass takes the new one */
        else if (r->a_quit == s)
            r->o_seq = s;       /* withdrawn before it was taken: the
                                   record is not read */
        else
            own_take(s);
    }
    if (j.seq)
    {
        r->o_busy = j.seq;
        BJC_FENCE();
        j.wrote = false;
        t0 = b.p->now_us();
        if (r->a_quit == j.seq || b.stop_seq == j.seq)
        {
            /* the caller left (TIMEOUT), or USB or a shutdown came
               (bjc_decode_stop: ABORTED, DEFER) */
            int ret = r->a_quit == j.seq ? BCMJOB_TIMEOUT : BCMJOB_ABORTED;

            if (j.kind == CALL_DECODE)
                dec_leave(ret);
            else
                job_end(ret);
        }
        else if (j.kind == CALL_READ)
        {
            if (j.ph == PH_READ)
                read_pass();
        }
        else if (j.ph == PH_RUN)
            dec_run();
        else
            while (j.seq && dec_step() && b.p->now_us() - t0 < PASS_US)
                ;
    }
    BJC_FENCE();                /* every touch of the caller's memory
                                   before the acknowledgement */
    r->o_busy = 0;
    b.in_pass = false;
}

/* ---- the caller's side (the art thread) ---------------------------------- */

/* Kick the owner: one post at a time, posted again when it went
 * unanswered for BJC_KICK_STALE_MS (a loop that dropped it) */
static void art_kick(void)
{
    uint32_t now;

    if (art_inline() || !own->kick)
        return;
    now = b.p->now_us();
    if (r->a_kick == r->o_kick ||
        now - r->a_kick_t > BJC_KICK_STALE_MS * 1000u)
    {
        r->a_kick++;
        r->a_kick_t = now;
        own->kick();
    }
}

/* What the caller does for the owner: a piece of the file when asked
 * (padded to its last word and, across cores, published before a_have:
 * S10), the cancel hook */
static void art_serve(const struct bcmjob_dec_req *q, uint8_t *buf,
                      bool cross, uint32_t seq)
{
    const struct jdec_src *src = q->src;
    uint32_t want = r->o_want;

    if (q->cancel && r->a_stop != seq && q->cancel(q->ctx))
    {
        r->a_stop = seq;
        art_kick();
    }
    if (want != r->a_have)
    {
        uint32_t n;
        int32_t got;

        BJC_FENCE();            /* the ask's size after its number */
        n = r->o_want_n;
        if (n > BCMJOB_DEC_PIECE)
            n = BCMJOB_DEC_PIECE;
        got = r->o_want_start && src->start(src->ctx) < 0 ? -1 :
              (int32_t)src->fetch(src->ctx, buf, n);
        if (got > 0 && (uint32_t)got <= n)
        {
            uint32_t m = ((uint32_t)got + 3) & ~3u;

            memset(buf + got, 0, m - (uint32_t)got);    /* the file's last
                                                           word */
            if (cross)
                own->publish(buf, m);
        }
        r->a_got = got;
        BJC_FENCE();            /* the piece and its count before a_have */
        r->a_have = want;
        art_kick();
    }
}

/* One call at a time: one still running, or taken and not finished (a
 * withdrawn call the owner never took may be replaced).  Busy: the owner
 * is nudged first (a pass here on the owner, else a kick), so a left
 * call whose kick was dropped can't stay busy for good */
static bool call_busy(void)
{
#define BUSY_NOW() (r->a_seq != r->o_seq && \
                    (r->a_quit != r->a_seq || r->o_take == r->a_seq))
    if (!BUSY_NOW())
        return false;
    if (art_inline())
        own_pass();
    else
        art_kick();
    return BUSY_NOW();
#undef BUSY_NOW
}

/* A LEAKED call (the state table above) whose acknowledgement has come
 * at last: its piece buffer goes back (on the caller's thread, as it was
 * taken).  false: the owner is still inside its pass: BUSY */
static bool leak_settled(void)
{
    if (!ac.leak_seq)
        return true;
    BJC_FENCE();
    if (r->o_busy == ac.leak_seq)
    {
        art_kick();
        return false;
    }
    trace("bcmjob withdraw acked late seq=%lu", (unsigned long)ac.leak_seq);
    if (ac.leak_buf && b.p->dec_buf)
        b.p->dec_buf(false);
    ac.leak_seq = 0;
    ac.leak_buf = false;
    return true;
}

enum { Q_DONE, Q_WITHDRAWN, Q_LEFT, Q_LEAKED };

/* The caller gives up on call seq: a_quit, then the owner's
 * acknowledgement (o_busy no longer seq), at most BJC_ACK_MS; the state
 * the call is then in (the table above) */
static int art_quit(uint32_t seq)
{
    uint32_t t0;

    r->a_stop = seq;
    r->a_quit = seq;
    BJC_FENCE();                /* a_quit before the look at o_busy */
    art_kick();
    t0 = b.p->now_us();
    while (r->o_busy == seq)
    {
        if (b.p->now_us() - t0 > BJC_ACK_MS * 1000u)
            return Q_LEAKED;
        if (own->art_wait)
            own->art_wait(1);
        else
            b.p->yield();
    }
    BJC_FENCE();                /* o_seq and o_take as of the ack */
    if (r->o_seq == seq)
        return Q_DONE;
    return r->o_take == seq ? Q_LEFT : Q_WITHDRAWN;
}

/* A read across cores, at its end: the caller's stale lines of the span
 * dropped, then the edge lines' pixels copied in on this core */
static void read_end(const struct bjc_call *c, uint32_t span, bool edges)
{
    int k, i;

    own->acquire(c->px, span);
    for (k = 0; edges && k < 2; k++)
        if (c->edge_at[k])
            for (i = 0; i < BJC_LINE; i++)
                if (c->edge_set[k] >> i & 1)
                    ((uint8_t *)c->edge_at[k])[i] = c->edge[k][i];
}

/* Post the call the caller filled in r->c (having seen it free) and wait
 * for its end; inline (passes in this loop) on the owner itself.  The
 * answer, with a decode's res copied out and a read's rows in the
 * caller's view; or the caller's give-up: OFF (never taken), TIMEOUT
 * (left, or LEAKED: ac.leak_seq) */
static int art_call(const struct bcmjob_dec_req *q, struct bcmjob_dec_res *res)
{
    struct bjc_call *c = &r->c;
    bool inl = art_inline();
    uint32_t seq, t0, span = 0;
    int st;

    c->cross = !inl && own->cross && own->publish && own->acquire &&
               own->uncached && own->cross();
    c->ret = BCMJOB_OFF;
    if (c->kind == CALL_READ)
    {
        span = read_span(c->w, c->h, c->pitch);
        if (c->cross && span)
            own->publish(c->px, span);  /* no dirty line of ours can land
                                           over the owner's rows */
    }
    seq = r->a_seq + 1 ? r->a_seq + 1 : 1;      /* never backward */
    BJC_FENCE();                /* the call before its number */
    r->a_seq = seq;
    BJC_FENCE();
    art_kick();
    t0 = b.p->now_us();
    for (;;)
    {
        if (inl)
            own_pass();
        if (r->o_seq == seq)
            break;
        if (!inl && b.p->now_us() - t0 > (r->o_take != seq ? BJC_ACK_MS :
                                          c->bound_ms) * 1000u)
        {
            /* never taken: withdrawn (OFF); taken and never finished:
               left (TIMEOUT, the CPU path); each once acknowledged */
            st = art_quit(seq);
            if (st == Q_DONE)
                break;          /* the answer came first: it stands */
            if (st == Q_LEAKED)
            {
                ac.leak_seq = seq;
                ac.leaks++;
                trace("bcmjob withdraw unacked kind=%s seq=%lu: its memory "
                      "kept", c->kind == CALL_READ ? "read" : "decode",
                      (unsigned long)seq);
                if (ac.leaks == 1)  /* the freeze audit's line, once */
                    trace("FAULT art_call wait=owner-ack ms=%lu",
                          (unsigned long)(b.p->now_us() - t0) / 1000);
                return BCMJOB_TIMEOUT;
            }
            if (c->cross && span)
                read_end(c, span, false);   /* rows half written: our view
                                               coherent again */
            return st == Q_LEFT ? BCMJOB_TIMEOUT : BCMJOB_OFF;
        }
        if (q && r->o_take == seq)
            art_serve(q, c->buf, c->cross, seq);
        if (r->o_seq == seq)
            break;
        if (inl || !own->art_wait)
            b.p->yield();
        else
            own->art_wait(ART_WAIT_MS);
        art_kick();             /* the owner polls the BCM at least so */
    }
    BJC_FENCE();                /* the answer after o_seq */
    if (c->kind == CALL_DECODE)
        *res = c->res;
    else if (c->cross && span)
        read_end(c, span, true);
    return c->ret;
}

int bcmjob_decode(const struct bcmjob_dec_req *q, struct bcmjob_dec_res *res)
{
    struct bjc_call *c;
    uint8_t *buf;
    uint32_t len, k;
    int n = q->n, bad, ret;

    memset(res, 0, sizeof(*res));
    if (!b.p)
        return BCMJOB_OFF;
    len = q->src ? q->src->len : 0;
    own_pass_fn = own_pass;
    if (!leak_settled() || call_busy())
        return fault(J_OP_DECODE, BCMJOB_BUSY);
    /* what jdec_run() would refuse, and what the work area can't take
       whichever outputs hit (none fits alone; bcmjob_dec_work()): never
       sent (a NOMEM sent would count toward the failed mark) */
    bad = n < 1 || n > J_DEC_MAX || len < 4 || !q->out;
    for (k = 0; !bad && k < (uint32_t)n; k++)
    {
        const struct bcmjob_dec_out *o = &q->out[k];

        bad = !o->dw || !o->dh || !o->cw || !o->ch || o->cx + o->cw > o->dw ||
              o->cy + o->ch > o->dh;
    }
    if (bad)
    {
        res->err = J_E_RANGE;
        return BCMJOB_FAILED;
    }
    for (k = 0, bad = 1; bad && k < (uint32_t)n; k++)
        bad = bcmjob_dec_work(&q->out[k], 1) > J_DEC_WORK_MIN;
    if (bad)
    {
        res->err = HP_E_NOMEM;
        return BCMJOB_FAILED;
    }
    buf = b.p->dec_buf ? b.p->dec_buf(true) : NULL;
    if (!buf)
    {
        res->err = HP_E_NOMEM;
        return BCMJOB_FAILED;
    }
    /* the record is free (call_busy): the owner reads it only after the
       post */
    c = &r->c;
    memset(c, 0, sizeof(*c));
    c->kind = CALL_DECODE;
    c->n = (uint32_t)n;
    memcpy(c->out, q->out, (uint32_t)n * sizeof(c->out[0]));
    c->len = len;
    c->ms = q->ms;
    /* the deadline at the slowest clock, then the margin */
    c->bound_ms = (q->ms ? q->ms : BCMJOB_DEC_CAP_MS) + DEC_MARGIN_MS;
    c->buf = buf;
    ret = art_call(q, res);
    if (ac.leak_seq)
        ac.leak_buf = true;     /* kept pinned until the owner acknowledges */
    else
        b.p->dec_buf(false);
    return ret;
}

int bcmjob_read(uint32_t slot, uint32_t x, uint32_t y, uint32_t w, uint32_t h,
                uint16_t *px, uint32_t pitch)
{
    struct bjc_call *c;

    if (!b.p)
        return BCMJOB_OFF;
    own_pass_fn = own_pass;
    if (!leak_settled() || call_busy())
        return BCMJOB_BUSY;
    c = &r->c;
    memset(c, 0, sizeof(*c));
    c->kind = CALL_READ;
    c->slot = slot;
    c->x = x;
    c->y = y;
    c->w = w;
    c->h = h;
    c->px = px;
    c->pitch = pitch;
    /* every word at the port's slowest (85 us under load), and a second */
    c->bound_ms = (uint32_t)((uint64_t)w * h / 2 * 85 / 1000) + 1000;
    return art_call(NULL, NULL);
}

uint32_t bjc_leaks(void)
{
    return ac.leaks;
}

/* ---- The retained screen's upload (P19 19-4, jobs rev 0.4) ---------------- */

/* One row of w pixels to a (a halfword address): word writes; an odd
 * first or last pixel by a read-modify-write of its word.  The lock held */
static int write_row(uint32_t a, const uint16_t *s, uint32_t w)
{
    uint32_t v, k = 0, n;

    if (a & 2)
    {
        if (b.p->rd(a - 2, &v) < 0 ||
            b.p->wr(a - 2, (v & 0xFFFF) | (uint32_t)s[0] << 16) < 0)
            return -1;
        k = 1;
        a += 2;
    }
    n = (w - k) & ~1u;
    if (n && b.p->wr_block(a, s + k, n) < 0)
        return -1;
    k += n;
    a += 2 * n;
    if (k < w && (b.p->rd(a, &v) < 0 ||
                  b.p->wr(a, (v & 0xFFFF0000u) | s[k]) < 0))
        return -1;
    return 0;
}

int bcmjob_upload_rect(uint32_t surf, uint32_t x, uint32_t y, uint32_t w,
                       uint32_t h, const uint16_t *px, uint32_t pitch)
{
    bool run = x == 0 && w == BJC_FRAME_W && pitch == BJC_FRAME_W;
    uint32_t t0, t1, g, row = 0, n, k, done, piece_us = 0;
    int err = 0;

    if (!art_inline())
        return BCMJOB_FAILED;   /* the ring owner's alone (S0, MF-2) */
    if (enter() != ST_JOBS)
        return BCMJOB_OFF;
    if (surf != J_SURF_BACK || !w || !h || x >= BJC_FRAME_W ||
        w > BJC_FRAME_W - x || y >= BJC_FRAME_H || h > BJC_FRAME_H - y ||
        pitch < w)
        return BCMJOB_FAILED;
    /* a DECODE in flight has the port's time (its ring): BUSY, uncounted */
    if (dec_holds() || (j.seq && j.kind == CALL_DECODE))
        return busy_decode(0);
    g = b.gen;
    t0 = b.p->now_us();
    while (row < h)
    {
        /* the budget before each piece: the time spent and one more
           piece like the last must fit */
        if (row && b.p->now_us() - t0 + piece_us > BJC_RECT_US)
        {
            trace("bcmjob upload_rect deferred rows=%lu/%lu", (unsigned long)row,
                  (unsigned long)h);
            return BCMJOB_BUSY;
        }
        /* a piece: 16 whole-width rows (the engine's minimum, 10 KB), or
           rows of at most BCMJOB_DEC_PIECE bytes */
        n = run ? 16 : BCMJOB_DEC_PIECE / (2 * w);
        if (!n)
            n = 1;
        if (n > h - row)
            n = h - row;
        t1 = b.p->now_us();
        b.p->lock();
        if (run)
        {
            uint32_t a = BJC_BACK_ADDR + (y + row) * BJC_FRAME_W * 2;
            uint32_t bytes = n * BJC_FRAME_W * 2;
            const uint16_t *s = px + row * pitch;

            done = n == 16 && b.p->upload ? b.p->upload(a, s, bytes) & ~3u : 0;
            if (done < bytes)
                err = b.p->wr_block(a + done, s + done / 2, (bytes - done) / 2);
        }
        else
            for (k = 0; k < n && !err; k++)
                err = write_row(BJC_BACK_ADDR + ((y + row + k) * BJC_FRAME_W + x) * 2,
                                px + (row + k) * pitch, w);
        b.p->unlock();
        piece_us = b.p->now_us() - t1;
        row += n;
        /* the epoch: a BCM cut or restarted between pieces has no BACK */
        if (err || b.p->gen() != g || b.suspended)
            return BCMJOB_OFF;
    }
    return BCMJOB_OK;
}

/* USB or a shutdown (owner, 2026-10-24: USB never waits on a decode):
 * the call posted or running now ends at once, ABORTED (the shell's
 * DEFER), its abort written if it was streaming, its own (not hit)
 * outputs freed behind it; the caller wakes within its wait step.  The
 * owner's thread only (the default event handler's SYS_USB_CONNECTED and
 * SYS_POWEROFF); elsewhere nothing. */
void bjc_decode_stop(void)
{
    if (!b.p || !own_pass_fn || !art_inline() || r->a_seq == r->o_seq)
        return;
    b.stop_seq = r->a_seq;
    own_pass_fn();
}
