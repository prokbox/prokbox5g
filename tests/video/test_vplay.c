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

/* Host simulation of the video player's engine (apps/video/vplay.c, #64
   piece 6) against a model of the audio clock and of the BCM in stage A:
   a FIFO of bounded size, records decoded one at a time in order, each
   picture on screen a panel latency after its decode.  The engine runs on
   the 10 ms kernel tick as it will on the PP.  Per scenario it checks the
   A/V error of every picture shown (the audio clock when it went up minus
   its PTS), decode order (no P frame decoded without its predecessor),
   and the scenario's own rule (pause, seek, speed, drops, stalls).

     test_vplay [FILE.vix]

   With a .vix (written by tests/video/run.sh from a corpus clip) the
   'real' scenario plays that index; the rest use synthetic clips. */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "video/vplay.h"
#include "video/vix.h"

#define TICK_US     10000
#define MAXS        (30 * 60 * 6)       /* 6 minutes at 30 fps */

struct clip
{
    uint32_t n;
    struct vp_sample s[MAXS];
    struct vp_key keys[MAXS];
    uint32_t nkeys;
};

/* the model BCM and its environment */
struct sim
{
    const struct clip *c;
    /* audio clock: audio = base + (t - t_base) * speed / 100 */
    int64_t t, t_base, a_base;
    int speed;
    bool paused;
    /* FIFO */
    int64_t fifo_used, fifo_cap;
    int stall_pct;              /* push answers VP_AGAIN this often */
    /* decoder */
    int64_t decode_us, decode_key_us, panel_us;
    /* silicon's fit (stage 6, sitting 2, the Sintel clip at 150 MHz):
       a FEED's time grows with its packet, decode_us + size * per_kb /
       1024 when per_kb is set; the PP learns a picture is up only at
       its next tick (stamp_tick) */
    int64_t per_kb_us;
    bool stamp_tick;
    int64_t last_up, frame_us;
    uint32_t hitches, late_n;
    int64_t late_max, early_max, ival_dev_sum;
    uint32_t ival_n;
    uint32_t queue[64];
    int64_t posted_at[64];
    bool show[64];
    int qh, qn;
    int64_t busy_until;
    int64_t last_decoded;       /* sample decoded last in this session */
    /* status */
    struct vp_status st;
    /* results */
    uint32_t shown, order_errors, posts_while_paused, shown_before_target;
    int64_t err_max, err_sum, settle_until, target;
    uint32_t err_n;
    uint32_t first_after_restart_key_bad, params_missing;
    bool want_params;
    uint32_t shown_keys_only_bad;
    bool keys_only_check;
};

static struct clip clip;
static int fails;

static void check(int cond, const char *what)
{
    printf("%s %s\n", cond ? "ok  " : "FAIL", what);
    fails += !cond;
}

static int64_t audio_at(const struct sim *m, int64_t t)
{
    if (m->paused)
        return m->a_base;
    return m->a_base + (t - m->t_base) * m->speed / 100;
}

static void rebase(struct sim *m)
{
    m->a_base = audio_at(m, m->t);
    m->t_base = m->t;
}

static int op_sample(void *ctx, uint32_t i, struct vp_sample *s)
{
    struct sim *m = ctx;
    if (i >= m->c->n)
        return -1;
    *s = m->c->s[i];
    return 0;
}

static int op_push(void *ctx, uint32_t i, bool params)
{
    struct sim *m = ctx;
    int64_t size = m->c->s[i].size + (params ? 32 : 0);
    if (m->stall_pct && rand() % 100 < m->stall_pct)
        return VP_AGAIN;
    if (m->fifo_used + size > m->fifo_cap)
        return VP_AGAIN;
    if (m->want_params && !params)
        m->params_missing++;
    m->want_params = false;
    m->fifo_used += size;
    return 0;
}

static void op_post(void *ctx, uint32_t i, int64_t pts_us, bool show)
{
    struct sim *m = ctx;
    int q = (m->qh + m->qn) % 64;
    (void)pts_us;
    if (m->paused)
        m->posts_while_paused++;
    m->queue[q] = i;
    m->posted_at[q] = m->t;
    m->show[q] = show;
    m->qn++;
    m->st.in_flight = m->qn;
}

static void op_skip(void *ctx, uint32_t i)
{
    struct sim *m = ctx;
    m->fifo_used -= m->c->s[i].size;
}

static void op_restart(void *ctx)
{
    struct sim *m = ctx;
    m->qn = 0;
    m->st.in_flight = 0;
    m->fifo_used = 0;
    m->busy_until = m->t;
    m->last_decoded = -1;
    m->want_params = true;
}

/* the decoder works through its records up to time t */
static void bcm_run(struct sim *m, int64_t t)
{
    while (m->qn)
    {
        int q = m->qh;
        uint32_t i = m->queue[q];
        const struct vp_sample *s = &m->c->s[i];
        int64_t start = m->posted_at[q] > m->busy_until ? m->posted_at[q]
                                                        : m->busy_until;
        int64_t done = start + (m->per_kb_us ? m->decode_us + (int64_t)s->size *
                                m->per_kb_us / 1024
                                : s->key ? m->decode_key_us : m->decode_us);
        if (done > t)
            break;
        m->busy_until = done;
        m->qh = (m->qh + 1) % 64;
        m->qn--;
        m->st.in_flight = m->qn;
        m->fifo_used -= s->size;

        if (m->last_decoded < 0 ? !s->key
                                : (!s->key && (int64_t)i != m->last_decoded + 1))
            m->order_errors++;
        if (m->last_decoded < 0 && !s->key)
            m->first_after_restart_key_bad++;
        m->last_decoded = i;

        if (m->show[q])
        {
            int64_t up = done + m->panel_us;
            int64_t a = audio_at(m, up);
            int64_t err = a - s->pts_us;
            m->shown++;
            m->st.shown_valid = true;
            m->st.shown_pts_us = s->pts_us;
            m->st.shown_at_us = m->stamp_tick ?
                audio_at(m, (up + TICK_US - 1) / TICK_US * TICK_US) : a;
            if (s->pts_us < m->target - 34000)
                m->shown_before_target++;
            if (m->keys_only_check && !s->key)
                m->shown_keys_only_bad++;
            if (a >= m->settle_until)
            {
                int64_t e = err < 0 ? -err : err;
                if (e > m->err_max)
                    m->err_max = e;
                m->err_sum += e;
                m->err_n++;
                /* vtrace.c's hitch: > half an interval and >= 15 ms late */
                if (err >= 15000 && m->frame_us && err > m->frame_us / 2)
                    m->hitches++;
                if (err > m->late_max)
                    m->late_max = err;
                if (-err > m->early_max)
                    m->early_max = -err;
                if (m->last_up && m->frame_us)
                {
                    int64_t d = up - m->last_up - m->frame_us;
                    m->ival_dev_sum += d < 0 ? -d : d;
                    m->ival_n++;
                }
            }
            {
                m->last_up = up;
            }
        }
    }
}

static void synth(uint32_t n, uint32_t gop)
{
    uint32_t i;
    clip.n = n;
    clip.nkeys = 0;
    for (i = 0; i < n; i++)
    {
        clip.s[i].pts_us = (int64_t)i * 1000000 / 30;
        clip.s[i].key = i % gop == 0;
        clip.s[i].size = clip.s[i].key ? 12000 : 2500 + (i * 37) % 1500;
        if (clip.s[i].key)
        {
            clip.keys[clip.nkeys].index = i;
            clip.keys[clip.nkeys++].pts_us = clip.s[i].pts_us;
        }
    }
}

static int load_vix(const char *path)
{
    FILE *f = fopen(path, "rb");
    uint8_t buf[VIX_HEADER_SIZE];
    struct vix_header h;
    uint32_t i;

    if (!f || fread(buf, 1, sizeof(buf), f) != sizeof(buf) ||
        vix_read_header(buf, &h) < 0 || h.count > MAXS)
        return -1;
    clip.n = h.count;
    clip.nkeys = 0;
    for (i = 0; i < h.count; i++)
    {
        uint8_t rec[VIX_RECORD_SIZE];
        struct mp4_sample s;
        if (fread(rec, 1, sizeof(rec), f) != sizeof(rec))
            return -1;
        vix_read_record(rec, &s);
        clip.s[i].pts_us = s.pts_us;
        clip.s[i].size = s.size;
        clip.s[i].key = s.key;
        if (s.key)
        {
            clip.keys[clip.nkeys].index = i;
            clip.keys[clip.nkeys++].pts_us = s.pts_us;
        }
    }
    fclose(f);
    return 0;
}

static const struct vp_ops ops = {
    NULL, op_sample, op_push, op_post, op_skip, op_restart
};

struct event
{
    int64_t at_us;              /* wall time */
    enum { EV_PAUSE, EV_RESUME, EV_SEEK, EV_SPEED } what;
    int64_t arg;
};

/* Runs a whole clip; events at wall times.  Returns the engine. */
static void play(const char *name, struct sim *m, struct vp *p,
                 const struct event *ev, int nev, int64_t lead_us)
{
    struct vp_ops o = ops;
    int e = 0;
    bool more = true;

    o.ctx = m;
    m->c = &clip;
    if (!m->speed)
        m->speed = 100;
    if (!m->fifo_cap)
        m->fifo_cap = 1 << 20;
    m->last_decoded = -1;
    m->want_params = true;
    m->target = INT64_MIN / 2;
    m->settle_until = 1000000;
    vp_open(p, &o, clip.n, clip.keys, clip.nkeys, lead_us);

    while (more && m->t < 20LL * 60 * 1000000)
    {
        m->t += TICK_US;
        bcm_run(m, m->t);
        while (e < nev && ev[e].at_us <= m->t)
        {
            rebase(m);
            switch (ev[e].what)
            {
                case EV_PAUSE:
                    m->paused = true;
                    vp_pause(p, true);
                    break;
                case EV_RESUME:
                    m->paused = false;
                    vp_pause(p, false);
                    m->settle_until = m->a_base + 1000000;
                    break;
                case EV_SEEK:
                    m->a_base = ev[e].arg;
                    m->target = ev[e].arg;
                    vp_seek(p, ev[e].arg);
                    m->settle_until = ev[e].arg + 1000000;
                    break;
                case EV_SPEED:
                    m->speed = (int)ev[e].arg;
                    vp_set_speed(p, (int)ev[e].arg);
                    m->settle_until = audio_at(m, m->t) + 1000000;
                    break;
            }
            e++;
        }
        more = vp_tick(p, audio_at(m, m->t), &m->st);
    }
    /* let the decoder finish */
    bcm_run(m, m->t + 10 * 1000000);
    printf("     %s: %u frames, %u shown, A/V error after settling: max %lld "
           "us, mean %lld us; lead %lld us (%u changes); %u skipped, %u "
           "GOPs dropped, %u late posts, %u push waits\n", name, clip.n,
           m->shown, (long long)m->err_max,
           (long long)(m->err_n ? m->err_sum / m->err_n : 0),
           (long long)p->lead_us, p->stats.lead_changes, p->stats.skipped,
           p->stats.dropped_gops, p->stats.late_posts, p->stats.push_waits);
}

static struct sim *fresh(int64_t decode_us, int64_t panel_us)
{
    static struct sim m;
    memset(&m, 0, sizeof(m));
    m.decode_us = decode_us;
    m.decode_key_us = decode_us * 3 / 2;
    m.panel_us = panel_us;
    return &m;
}

int main(int argc, char **argv)
{
    static struct vp p;
    struct sim *m;
    const int64_t frame = 33334;

    srand(2722);

    /* 1. normal: 3 minutes, GOP 30, an 11 ms decode (the iPod clip at
       150 MHz), 17 ms to the panel; the lead starts at the decode budget */
    synth(30 * 180, 30);
    m = fresh(11000, 17000);
    play("normal", m, &p, NULL, 0, 35000);
    check(m->shown == clip.n && m->order_errors == 0 &&
          p.stats.skipped == 0 && m->params_missing == 0,
          "normal: every frame shown, in decode order, nothing skipped");
    check(m->err_max <= frame, "normal: A/V error within one frame");

    /* 2. a slower path to the screen than the lead assumes: the drift
       check moves the lead until pictures go up on time */
    m = fresh(11000, 90000);
    play("latency", m, &p, NULL, 0, 35000);
    check(p.stats.lead_changes > 0 && m->err_max <= frame + 20000,
          "latency: the lead follows a 90 ms panel path, error settles");
    check(m->order_errors == 0, "latency: decode order kept");

    /* 3. decode slower than real time (45 ms a frame): GOPs drop from
       their first late P frame to the next key, decode order kept, and
       the picture does not drift away from the audio */
    m = fresh(45000, 17000);
    play("slow", m, &p, NULL, 0, 35000);
    check(p.stats.dropped_gops > 0 && m->order_errors == 0,
          "slow: whole GOP tails dropped, never a P frame without its "
          "predecessor");
    check(m->err_max < 1000000, "slow: error stays under a second");

    /* 4. pause for 3 s at 20 s, then play on */
    {
        static const struct event ev[] = {
            { 20000000, EV_PAUSE, 0 }, { 23000000, EV_RESUME, 0 },
        };
        m = fresh(11000, 17000);
        play("pause", m, &p, ev, 2, 35000);
        check(m->posts_while_paused == 0, "pause: no records while paused");
        check(m->shown == clip.n && m->err_max <= frame,
              "pause: every frame shown, in sync after the pause");
    }

    /* 5. seek forward to 2:30, then back to 0:10 */
    {
        static const struct event ev[] = {
            { 30000000, EV_SEEK, 150000000 }, { 35000000, EV_SEEK, 10000000 },
        };
        m = fresh(11000, 17000);
        play("seek", m, &p, ev, 2, 35000);
        check(p.stats.restarts == 2 && m->first_after_restart_key_bad == 0 &&
              m->params_missing == 0,
              "seek: each restart feeds from a key sample, SPS/PPS first");
        check(m->shown_before_target == 0,
              "seek: nothing before the target is shown");
        check(m->order_errors == 0 && m->err_max <= frame,
              "seek: in sync a second after each seek");
    }

    /* 6. speed: 1.5x keeps every frame at an 11 ms decode; 2.5x is a
       slideshow of key frames */
    {
        static const struct event ev[] = { { 10000, EV_SPEED, 150 } };
        m = fresh(11000, 17000);
        play("speed 1.5x", m, &p, ev, 1, 35000);
        check(m->shown == clip.n && m->err_max <= frame &&
              m->order_errors == 0,
              "speed 1.5x: every frame, in sync with the faster audio");
    }
    {
        static const struct event ev[] = { { 10000, EV_SPEED, 250 } };
        m = fresh(11000, 17000);
        m->keys_only_check = true;
        play("speed 2.5x", m, &p, ev, 1, 35000);
        check(m->shown_keys_only_bad == 0 && m->shown == clip.nkeys,
              "speed 2.5x: key frames only");
    }

    /* 7. the buffer is not ready a third of the time, and the FIFO is
       small (256 KB): data still arrives ahead of the records */
    m = fresh(11000, 17000);
    m->stall_pct = 33;
    m->fifo_cap = 256 * 1024;
    play("stalls", m, &p, NULL, 0, 35000);
    check(m->shown == clip.n && m->err_max <= frame && m->order_errors == 0,
          "stalls: every frame shown in sync despite push waits");

    /* 8. a real index from the corpus */
    if (argc > 1)
    {
        if (load_vix(argv[1]) < 0)
            check(0, "real: reading the .vix");
        else
        {
            m = fresh(11000, 17000);
            play("real", m, &p, NULL, 0, 35000);
            check(m->shown == clip.n && m->order_errors == 0 &&
                  m->err_max <= frame,
                  "real: a corpus clip's index plays in sync");
        }
    }

    /* 9. real footage at silicon's pace: stage 6's Sintel clip (24 fps),
       each FEED 11.9 ms + 3.02 us a byte (the fit to sitting 2's 302
       records at 150 MHz, r 0.82; the I picture at 5.1 s 40 ms), 8 ms
       to the panel (stage 4c's PLL frame), the PP seeing each picture
       at its next tick */
    if (argc > 2)
    {
        if (load_vix(argv[2]) < 0)
            check(0, "footage: reading the .vix");
        else
        {
            m = fresh(11915, 8000);
            m->per_kb_us = 3097;
            m->stamp_tick = true;
            m->frame_us = clip.s[1].pts_us - clip.s[0].pts_us;
            m->settle_until = 1000000;
            play("footage", m, &p, NULL, 0, 35000);
            printf("     footage: %u hitches, late max %lld us, early max %lld us, "
                   "mean |interval - frame| %lld us over %u\n", m->hitches,
                   (long long)m->late_max, (long long)m->early_max,
                   (long long)(m->ival_n ? m->ival_dev_sum / m->ival_n : 0),
                   m->ival_n);
            check(m->shown == clip.n && m->order_errors == 0 &&
                  p.stats.skipped == 0, "footage: every picture shown, in order");
            /* the PP's pacing adds no stutter of its own: only the one
               picture whose FEED is near a whole frame (the I picture at
               5.1 s) jumps, and the screen keeps the clip's cadence */
            check(m->hitches <= 1 && m->ival_dev_sum <= 6000LL * m->ival_n,
                  "footage: at most the I picture's hitch, cadence within 6 ms");
        }
    }

    printf("vplay: %d failures\n", fails);
    return fails ? 1 : 0;
}
