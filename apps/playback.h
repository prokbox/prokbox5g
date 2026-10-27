/***************************************************************************
 *             __________               __   ___.
 *   Open      \______   \ ____   ____ |  | _\_ |__   _______  ___
 *   Source     |       _//  _ \_/ ___\|  |/ /| __ \ /  _ \  \/  /
 *   Jukebox    |    |   (  <_> )  \___|    < | \_\ (  <_> > <  <
 *   Firmware   |____|_  /\____/ \___  >__|_ \|___  /\____/__/\_ \
 *                     \/            \/     \/    \/            \/
 *
 * Copyright (C) 2005 Miika Pekkarinen
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

#ifndef _PLAYBACK_H
#define _PLAYBACK_H

#include <stdbool.h>
#include <stdlib.h>
#include "config.h"

/* Including the code for fast previews is entirely optional since it
   does add two more mp3entry's - for certain targets it may be less
   beneficial such as flash-only storage */
#if MEMORYSIZE > 2
#define AUDIO_FAST_SKIP_PREVIEW
#endif

#ifdef HAVE_ALBUMART

#include "bmp.h"
#include "metadata.h"
/*
 * Returns the handle id of the buffered albumart for the given slot id
 **/
int playback_current_aa_hid(int slot);

/*
 * Hands out an albumart slot for buffering albumart using the size
 * int the passed dim struct, it copies the data of dim in order to
 * be safe to be reused for other code
 *
 * The slot may be reused if other code calls this with the same dimensions
 * in dim, so if you change dim release and claim a new slot
 *
 * Save to call from other threads */
int playback_claim_aa_slot(struct dim *dim);

/*
 * Releases the albumart slot with given id
 *
 * Save to call from other threads */
void playback_release_aa_slot(int slot);

/*
 * Tells playback to sync buffered album art dimensions
 *
 * Save to call from other threads */
void playback_update_aa_dims(void);

struct bufopen_bitmap_data {
    struct dim *dim;
    struct mp3_albumart *embedded_albumart;
};

#endif /* HAVE_ALBUMART */

/* Functions */
unsigned int audio_track_count(void);
long audio_filebufused(void);
void audio_pre_ff_rewind(void);
void audio_skip(int direction);

void audio_set_cuesheet(bool enable);
#ifdef HAVE_CROSSFADE
void audio_set_crossfade(int enable);
#endif
#ifdef HAVE_PLAY_FREQ
void audio_set_playback_frequency(unsigned int sample_rate_hz);
#endif
#ifdef HAVE_ALBUMART
void set_albumart_mode(int setting);
#endif

size_t audio_get_filebuflen(void);
#ifdef HAVE_IPOD_UI
/* A transient block was freed: the next playback start from stopped
 * sizes the audio buffer anew (an allocation while stopped shrinks it,
 * and an initialised buffer is otherwise kept). Any thread. */
void audio_buffer_regrow(void);
/* #88 (apps/ipodui/maint.c): paused only, from any thread but the audio
 * thread. One audio-thread event: frees 'bytes' at the front of the
 * audio buffer if it stays >= 'floor' (the stop happens before the buflib
 * shrink, never inside a buflib call), or with bytes 0 grows it back to
 * all the free memory (if at least 'floor' is free), and restarts the
 * paused song silently at its spot, reported paused throughout. Returns
 * 1 done, 0 refused, -1 the restart failed (stopped, logged). */
struct audio_give {
    size_t bytes, floor;
    int result;
    volatile int started, done;     /* (playback.c's handshake) */
    bool back;                      /* shrink at the back (buflib's hint) */
};
int audio_buffer_give(struct audio_give *g);
/* The restart is running. Meanwhile audio_status() reports paused, and
 * audio_pause() / audio_resume() post their event behind the restart
 * and return at once: after audio_pause() the status reads paused (it
 * was, and stays so); after audio_resume() it still reads paused until
 * the restart ends and the queued resume runs (callers must not take
 * that as the resume failing). Another thread's allocation that needs
 * the paused buffer is refused (shrink_callback) and a give is posted. */
bool audio_quiet_restart(void);
/* P6: the chapter-table slot (0..NP_AB_SLOTS-1) a table pointer (an
 * mp3entry's audiobook) refers to, or -1. RAM only, no lock, any thread. */
int audio_ab_slot_of(const void *ab);
#endif

unsigned int playback_status(void);

struct mp3entry* get_temp_mp3entry(struct mp3entry *free);

void allocate_playback_log(void);
void add_playbacklog(struct mp3entry *id3);

#endif /* _PLAYBACK_H */
