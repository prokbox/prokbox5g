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

/* Host test of P19 19-1c (iii), review M1: lib/rbcodec/codecs/mpa.c's
 * synthesis switch. The shell's default (decoding pinned to the CPU)
 * runs MP3's synthesis inline (ci->cop_assist false) in a build that
 * has the second-core path compiled in; that path needs NUM_CORES > 1,
 * so the simulator never runs it. Here mpa.c and libmad build for the
 * host with two "cores" (the synthesis thread on a pthread) and with
 * one (upstream's plain path), and each scenario prints the PCM's hash
 * and length; run.sh requires the inline, threaded and plain outputs to
 * be bit-identical:
 *   plain    one run
 *   seek     a seek 1.5 s in, after the 30th frame
 *   pair     two runs back to back on one load (a gapless pair), with
 *            the switch flipped between them (threaded then inline, and
 *            inline then threaded): the thread's handshake survives
 * Usage: test_mpainline <file.mp3> <assist pattern, e.g. 1, 0, 10, 01> */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "codeclib.h"
#include "libasf/asf.h"

struct codec_api *ci;
static struct codec_api api;
static struct mp3entry id3;
static unsigned char *file;
static size_t file_len;
static int commands, seek_at = -1;
static long seek_ms;
static uint64_t hash = 1469598103934665603ULL;
static unsigned long samples;

/* ---- the stream ---------------------------------------------------- */

static void *request_buffer(size_t *realsize, size_t reqsize)
{
    size_t left = file_len - (size_t)api.curpos;
    *realsize = reqsize < left ? reqsize : left;
    return file + api.curpos;
}
static void advance_buffer(size_t n)
{
    api.curpos += n;
    if ((size_t)api.curpos > file_len)
        api.curpos = file_len;
}
static bool seek_buffer(size_t pos)
{
    if (pos > file_len)
        return false;
    api.curpos = pos;
    return true;
}
static void seek_complete(void) { }
static void set_elapsed(unsigned long v) { (void)v; }
static void set_offset(size_t v) { (void)v; }
static void configure(int s, intptr_t v) { (void)s; (void)v; }
static void strip_filesize(off_t v) { api.filesize = v; }
static long get_command(intptr_t *param)
{
    if (++commands == seek_at)
    {
        *param = seek_ms;
        return CODEC_ACTION_SEEK_TIME;
    }
    return CODEC_ACTION_NULL;
}

/* FNV-1a over both channels, sample by sample */
static void mix(int32_t v)
{
    int i;
    for (i = 0; i < 4; i++)
    {
        hash ^= (uint8_t)(v >> (8 * i));
        hash *= 1099511628211ULL;
    }
}
static void pcmbuf_insert(const void *ch1, const void *ch2, int count)
{
    const int32_t *a = ch1, *b = ch2;
    int i;
    for (i = 0; i < count; i++)
    {
        mix(a[i]);
        mix(b ? b[i] : 0);
    }
    samples += count;
}

/* ---- the kernel, on pthreads ----------------------------------------- */

static pthread_t threads[4];
static int nthreads;
struct start { void (*fn)(void); };
static struct start starts[4];
static void *trampoline(void *arg)
{
    ((struct start *)arg)->fn();
    return NULL;
}
static unsigned int create_thread(void (*fn)(void), void *stack, size_t size,
                                  unsigned flags, const void *name, int prio,
                                  int core)
{
    (void)stack; (void)size; (void)flags; (void)name; (void)prio;
    (void)core;
    starts[nthreads].fn = fn;
    if (pthread_create(&threads[nthreads], NULL, trampoline,
                       &starts[nthreads]))
        return 0;
    return ++nthreads;
}
static void thread_wait(unsigned int id)
{
    pthread_join(threads[id - 1], NULL);
}
static void sem_init_(struct semaphore *s, int max, int start)
{
    pthread_mutex_init(&s->m, NULL);
    pthread_cond_init(&s->c, NULL);
    s->max = max;
    s->count = start;
}
static int sem_wait_(struct semaphore *s, int timeout)
{
    (void)timeout;
    pthread_mutex_lock(&s->m);
    while (s->count == 0)
        pthread_cond_wait(&s->c, &s->m);
    s->count--;
    pthread_mutex_unlock(&s->m);
    return 1;
}
static void sem_release_(struct semaphore *s)
{
    pthread_mutex_lock(&s->m);
    if (s->count < s->max)
        s->count++;
    pthread_cond_signal(&s->c);
    pthread_mutex_unlock(&s->m);
}
static void nop(void) { }

int codec_init(void) { return 0; }
void codec_set_replaygain(const struct mp3entry *p) { (void)p; }
int asf_read_packet(uint8_t **b, int *s, int *l, asf_waveformatex_t *w)
{
    (void)b; (void)s; (void)l; (void)w;
    return -1;
}
int asf_get_timestamp(int *d) { (void)d; return 0; }
int asf_seek(int ms, asf_waveformatex_t *w) { (void)ms; (void)w; return -1; }

/* ---- the runs -------------------------------------------------------- */

static void one_run(bool assist)
{
    api.curpos = 0;
    api.filesize = file_len;
    id3.offset = id3.elapsed = 0;
    api.cop_assist = assist;
    hash = 1469598103934665603ULL;
    samples = 0;
    if (codec_run() != CODEC_OK)
    {
        fprintf(stderr, "codec_run failed\n");
        exit(1);
    }
    printf("%c:%016llx:%lu ", assist ? 'T' : 'I', (unsigned long long)hash,
           samples);
}

int main(int argc, char *argv[])
{
    FILE *f;
    const char *p;
    long n;

    if (argc < 3)
        return 2;
    f = fopen(argv[1], "rb");
    if (!f)
        return 2;
    fseek(f, 0, SEEK_END);
    n = ftell(f);
    fseek(f, 0, SEEK_SET);
    file = malloc(n);
    file_len = fread(file, 1, n, f);
    fclose(f);
    if (argc >= 4)
    {
        seek_at = 30;
        seek_ms = atol(argv[3]);
    }

    id3.frequency = 44100;
    id3.layer = 2;              /* MPEG layer III's row in mpa.c's tables */
    id3.lead_trim = id3.tail_trim = -1;
    id3.bitrate = 128;
    id3.filesize = file_len;
    id3.length = file_len * 8 / 128;
    api.id3 = &id3;
    api.pcmbuf_insert = pcmbuf_insert;
    api.set_elapsed = set_elapsed;
    api.request_buffer = request_buffer;
    api.advance_buffer = advance_buffer;
    api.seek_buffer = seek_buffer;
    api.seek_complete = seek_complete;
    api.set_offset = set_offset;
    api.configure = configure;
    api.get_command = get_command;
    api.strip_filesize = strip_filesize;
    api.memset = memset;
    api.create_thread = create_thread;
    api.thread_wait = thread_wait;
    api.semaphore_init = sem_init_;
    api.semaphore_wait = sem_wait_;
    api.semaphore_release = sem_release_;
    api.commit_discard_dcache = nop;
    ci = &api;

    if (codec_main(CODEC_LOAD) != CODEC_OK)
        return 1;
    for (p = argv[2]; *p; p++)
        one_run(*p == '1');
    codec_main(CODEC_UNLOAD);
    printf("threads=%d\n", nthreads);
    return 0;
}
