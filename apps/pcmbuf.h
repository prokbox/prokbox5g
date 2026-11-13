/***************************************************************************
 *             __________               __   ___.
 *   Open      \______   \ ____   ____ |  | _\_ |__   _______  ___
 *   Source     |       _//  _ \_/ ___\|  |/ /| __ \ /  _ \  \/  /
 *   Jukebox    |    |   (  <_> )  \___|    < | \_\ (  <_> > <  <
 *   Firmware   |____|_  /\____/ \___  >__|_ \|___  /\____/__/\_ \
 *                     \/            \/     \/    \/            \/
 *
 * Copyright (C) 2005 by Miika Pekkarinen
 * Modified for ProkBox5g (2026); see docs/changes-from-rockbox.md.
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
#ifndef PCMBUF_H
#define PCMBUF_H

#include <sys/types.h>
#include <stdint.h>

/* Commit PCM data */
void *pcmbuf_request_buffer(int *count);
void pcmbuf_write_complete(int count, unsigned long elapsed, off_t offset);
/* #282 step 4: changes whenever the write side is reset */
unsigned int pcmbuf_write_epoch(void);
bool pcmbuf_window_may_hold(void);
#if defined(HAVE_COP_CODEC) && defined(HAVE_CORE_LAYOUT_A)
/* 0.8.3 colocate (docs/data-paths.md R7 item 12): the write side's lease.
 * While the COP holds it, the COP requests and commits itself (its DSP
 * is the producer, its FIQ the consumer) and no CPU code touches the
 * write side: every CPU entry point that does reclaims it first, waiting
 * for the COP to finish the frame it is on (at most one frame or one
 * full-buffer slice, PCMBUF_LEASE_SLICE). */
#define PCMBUF_LEASE_SLICE  (HZ/50)     /* the COP's full-buffer sleep */
enum { PCMBUF_LEASE_FULL = 1, PCMBUF_LEASE_BACK };
/* CPU (the codec thread, after a full-buffer sleep): hand it over if
 * playing with no crossfade, low latency, sync or rescue under way */
bool pcmbuf_lease_grant(void);
/* CPU: take it back (any thread; may sleep a slice) */
void pcmbuf_lease_reclaim(void);
/* COP: a window (NULL: *why is FULL, or BACK: give the lease back) */
void *pcmbuf_lease_window(int *count, int *why);
/* COP: count samples written to the window */
void pcmbuf_lease_commit(int count, unsigned long elapsed, off_t offset);
/* COP: ticks to sleep on a full buffer (at most PCMBUF_LEASE_SLICE) */
long pcmbuf_lease_full_ticks(void);
/* COP: the chunk the FIQ is on (progress while full: a pause stops it) */
size_t pcmbuf_lease_ridx(void);
/* COP: give it back (its cache written back; the CPU reclaims) */
void pcmbuf_lease_release(void);
/* counters since boot: grants, reclaims by the CPU, commits, COP sleeps */
void pcmbuf_lease_stats(uint32_t *grants, uint32_t *revokes,
                        uint32_t *commits, uint32_t *sleeps);
#endif

/* Init */
size_t pcmbuf_size_reqd(void);
size_t pcmbuf_init(void *bufend);

/* Playback */
void pcmbuf_play_start(void);
void pcmbuf_play_stop(void);
void pcmbuf_pause(bool pause);

/* Track change */

/* Track change origin type */
enum pcm_track_change_type
{
    TRACK_CHANGE_NONE = 0,     /* No track change pending */
    TRACK_CHANGE_MANUAL,       /* Manual change (from user) */
    TRACK_CHANGE_AUTO,         /* Automatic change (from codec) */
    TRACK_CHANGE_AUTO_PILEUP,  /* Auto change during pending change */
    TRACK_CHANGE_END_OF_DATA,  /* Expect no more data (from codec) */
};
void pcmbuf_monitor_track_change(bool monitor);
void pcmbuf_start_track_change(enum pcm_track_change_type type);

/* Crossfade */
#ifdef HAVE_CROSSFADE
void pcmbuf_request_crossfade_enable(int setting);
bool pcmbuf_is_same_size(void);
#else
/* Dummy functions with sensible returns */
static FORCE_INLINE void pcmbuf_request_crossfade_enable(bool on_off)
    { return; (void)on_off; }
static FORCE_INLINE bool pcmbuf_is_same_size(void)
    { return true; }
#endif

/* Debug menu, other metrics */
size_t pcmbuf_free(void);
size_t pcmbuf_get_bufsize(void);
int pcmbuf_used_descs(void);
int pcmbuf_descs(void);

/* Fading and channel volume control */
void pcmbuf_fade(bool fade, bool in);
bool pcmbuf_fading(void);
void pcmbuf_soft_mode(bool shhh);

/* Time and position */
unsigned int pcmbuf_get_position_key(void);
void pcmbuf_sync_position_update(void);

/* Misc */
bool pcmbuf_is_lowdata(void);
void pcmbuf_set_low_latency(bool state);

#if defined(HAVE_BCMFW_VIDEO) || defined(HAVE_IPOD_UI)
/* times the playing PCM ran dry: a real underrun */
unsigned long pcmbuf_underruns(void);
/* planned drains at a sample-rate switch between tracks (the full
 * re-buffer HAVE_PLAY_FREQ "auto" does), not underruns */
unsigned long pcmbuf_rate_gaps(void);
/* playback.c: buffering stopped for a rate switch; the playing track's end
 * is a rate gap */
void pcmbuf_rate_gap_expected(bool on);
#endif
#ifdef HAVE_IPOD_UI
/* times the playing PCM fell under the boost watermark (S36, C6) */
unsigned long pcmbuf_rescues(void);
/* 0.8.3 road-test fix: mid-track underruns, underrun lines logged, and
   the lowest level in ms since the last call (-1: none) */
unsigned long pcmbuf_mid_underruns(void);
unsigned long pcmbuf_underrun_lines(void);
long pcmbuf_level_min_take(void);
/* 0.8.3: the tick the playing pause began (0: not paused since), and its
   clear once the shell logged the pause's standby */
long pcmbuf_pause_tick(void);
void pcmbuf_pause_tick_clear(void);
void pcmbuf_note_pause(bool pause);
/* P19 19-1c (D): how long the codec may sleep on a full buffer, in ticks:
 * until the level drains to the low watermark (codec_place_core.c) */
long pcmbuf_full_wait_ticks(void);
/* #298: the time since the last track change the PCM played out (the
 * outgoing track's last chunk done), in us; *dry: nothing was queued
 * after it, so the audio stopped there */
unsigned long pcmbuf_track_change_lag_us(bool *dry);
#endif

#endif /* PCMBUF_H */
