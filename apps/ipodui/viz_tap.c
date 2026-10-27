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

/* The Now Playing visualizer's PCM tap: viz_tap.h */

#include <string.h>
#include "config.h"
#include "system.h"
#include "kernel.h"
#include "core_alloc.h"
#include "pcm.h"
#include "pcm_mixer.h"
#include "pcm_sampr.h"
#include "audio.h"
#include "metadata.h"
#include "xcore.h"
#include "viz_tap.h"
#include "np_playback.h"

#ifdef HAVE_VIZ_TAP

/* The largest mixer frame, in frames (pcm_mixer.c's MAX_MIX_FRAME_SAMPLES:
 * its FRAME_SIZE_MULT is this decimation for the top rate) */
#define VT_FMAX     (MIX_FRAME_SAMPLES * viz_tap_dec(HW_SAMPR_MAX))

/* The PP's uncached alias (both cores and the FIQ see one copy); the
 * simulator has none and needs none */
#if (CONFIG_PLATFORM & PLATFORM_NATIVE) && defined(CPU_PP)
#define VT_UNCACHED(a)  UNCACHED_ADDR(a)
#define VT_CLEAN()      commit_discard_dcache()
#else
#define VT_UNCACHED(a)  (a)
#define VT_CLEAN()      do { } while (0)
#endif
#define VT_ALIGN(x)     (((x) + XCORE_LINE - 1) & ~(uintptr_t)(XCORE_LINE - 1))
#define VT_HDR          VT_ALIGN(sizeof(struct viz_ring))

/* The ring as both sides see it (the uncached alias on the PP: the PCM
 * callback writes it on the CPU and the reader may be on the COP).  The
 * callback loads the pointer once a frame; NULL is "closed". */
static struct viz_ring *volatile vt_ring XCORE_BSS;

/* The opener's (CPU: the UI thread inline, be_svc with the UI split off,
 * by remote call: febe_rpc.list).  vt_handle: open and close only */
static int vt_handle;                   /* 0: the caller's memory */
/* written by open (CPU) and by the reader (the ui thread, either core;
 * one reader at a time): uncached, so the reader on the COP never reads
 * its own stale copy of what open set (P19 ucop-fix2) */
static uint32_t vt_n XCORE_BSS, vt_last_head XCORE_BSS, vt_track XCORE_BSS;
static bool vt_book XCORE_BSS;

/* The mixer's tap (pcm_mixer.c, the PCM callback: FIQ on the PP5022):
 * one bounded copy of the frame queued behind the one now playing */
static void vt_tap(const void *frames, size_t size)
{
    struct viz_ring *r = vt_ring;

    if (r)
        viz_ring_write(r, (const uint32_t *)frames, size / 4);
}

static bool vt_playing_book(void)
{
    struct mp3entry *id3;
    bool podcast;

    if (!(audio_status() & AUDIO_STATUS_PLAY))
        return false;
    id3 = audio_current_track();
    return id3 && id3->path[0] && np_pb_is_book(id3, &podcast);
}

size_t viz_tap_bytes(unsigned n)
{
    return XCORE_LINE - 1 + VT_HDR
           + 4 * viz_ring_frames(n, viz_tap_dec(HW_SAMPR_MAX), VT_FMAX);
}

int viz_tap_open(unsigned n, void *mem, size_t bytes)
{
    size_t need;
    struct viz_ring *r;
    uintptr_t p;

    /* On the CPU (a remote call from the COP, as close) */
    XCORE_ASSERT_CPU("viz_tap_open");
    if (vt_ring)
        return VIZ_TAP_E_BUSY;
    if (n != 256 && n != 512)
        return VIZ_TAP_E_SIZE;
    vt_track = np_pb_track();
    if (vt_playing_book())
        return VIZ_TAP_E_BOOK;

    need = viz_tap_bytes(n);
    if (mem)
    {
        if (bytes < need)
            return VIZ_TAP_E_NOMEM;
    }
    else
    {
        /* never from playback (above); re-checked with no yield */
        if (audio_status() && core_available() < need + 64)
            return VIZ_TAP_E_NOMEM;
        vt_handle = core_alloc(need);
        if (vt_handle <= 0)
        {
            vt_handle = 0;
            return VIZ_TAP_E_NOMEM;
        }
        core_pin(vt_handle);            /* the PCM callback writes it */
        mem = core_get_data(vt_handle);
    }
    p = VT_ALIGN((uintptr_t)mem);
    /* no line of ours may sit dirty in the CPU's cache while the alias is
     * written (an eviction would overwrite the ring): clean and drop once */
    VT_CLEAN();
    r = (struct viz_ring *)VT_UNCACHED(p);
    viz_ring_init(r, (uint32_t *)VT_UNCACHED(p + VT_HDR),
                  viz_ring_frames(n, viz_tap_dec(HW_SAMPR_MAX), VT_FMAX),
                  VT_FMAX);
    vt_n = n;
    vt_last_head = 0;
    vt_book = false;
    vt_ring = r;
    mixer_set_tap(vt_tap);
    return VIZ_TAP_OK;
}

unsigned viz_tap_read(int16_t *out, unsigned opts, struct viz_tap_frame *f)
{
    struct viz_tap_frame tmp;
    struct viz_ring *r = vt_ring;
    uint32_t dec, rate, track;
    unsigned flags;

    if (!f)
        f = &tmp;
    if (!r)
    {
        memset(out, 0, 2 * (vt_n ? vt_n : 256));
        memset(f, 0, sizeof(*f));
        return VIZ_TAP_CLOSED;
    }
    track = np_pb_track();              /* one load: re-check on a change */
    if (track != vt_track)
    {
        vt_track = track;
        vt_book = vt_playing_book();
    }
    rate = mixer_get_frequency();
    dec = viz_tap_dec(rate);
    if (vt_book)
    {
        memset(out, 0, 2 * vt_n);
        memset(f, 0, sizeof(*f));
        f->rate = rate / dec;
        return VIZ_TAP_BOOK | VIZ_TAP_SILENT;
    }
    flags = viz_ring_read(r, out, vt_n, dec, vt_last_head, opts, f);
    f->rate = rate / dec;
    vt_last_head = f->head;
    return flags;
}

void viz_tap_close(void)
{
    /* On the CPU, always: with the UI on the COP this is a remote call
     * (febe_rpc.list), so be_svc runs it.  That matters for the free: the
     * PCM callback is the CPU's FIQ, and only a lock taken on the CPU
     * keeps it out (the lock masks the caller's core's FIQ) */
    XCORE_ASSERT_CPU("viz_tap_close");
    if (!vt_ring)
        return;
    /* mixer_set_tap() takes the PCM lock on this core (the CPU): the FIQ
     * preempts this thread, so once the lock is held no callback is
     * inside the tap, and once it is dropped with the hook NULL none
     * enters it again (the simulator's audio thread mixes holding the
     * lock).  Only then is the ring freed */
    mixer_set_tap(NULL);
    vt_ring = NULL;
    VT_CLEAN();                         /* no stale line over the memory */
    if (vt_handle > 0)
        core_free(vt_handle);
    vt_handle = 0;
    vt_n = 0;
}

bool viz_tap_is_open(void)
{
    return vt_ring != NULL;
}

#endif /* HAVE_VIZ_TAP */
