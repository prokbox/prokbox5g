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

/* Host test of bufpeek_at() (apps/buffering.c, HAVE_BCMFW_VIDEO; #64
   piece 5).  It builds the real buffering.c against the stand-ins in
   tests/video/bufshim and plays the buffering thread itself: a "codec"
   reads a file through a small ring with bufread()/bufadvance() while the
   "video player" peeks ahead of it, the ring wraps several times, and
   reclaimed data goes away.  Checked:
     - every peek inside the buffered window returns the file's bytes,
       including peeks across the ring's wrap,
     - a peek past the buffered data is ERR_HANDLE_NOT_DONE and one before
       the window (reclaimed) ERR_INVALID_VALUE, and neither waits,
     - peeks never move the codec's read position,
     - an unknown handle is ERR_HANDLE_NOT_FOUND. */

/* a copy of apps/buffering.c (run.sh copies it next to the build, so its
   quoted includes find these stand-ins before the real apps/ headers) */
#include "buffering.c"

#define FILE_SIZE   (1024 * 1024 + 777)
#define RING        (96 * 1024)
#define CODEC_STEP  3000

static int fails;

static void check(int cond, const char *what)
{
    printf("%s %s\n", cond ? "ok  " : "FAIL", what);
    fails += !cond;
}

intptr_t test_queue_send(long id, intptr_t data)
{
    switch (id)
    {
        case Q_BUFFER_HANDLE:
            return buffer_handle((int)data, 0);
        case Q_START_FILL:
            shrink_buffer();
            return buffer_handle((int)data, 0);
        case Q_CLOSE_HANDLE:
            return close_handle((int)data);
        case Q_REBUFFER_HANDLE:
        {
            struct buf_message_data *p = (struct buf_message_data *)data;
            rebuffer_handle(p->handle_id, p->data);
            return 1;
        }
    }
    return 0;
}

static uint8_t pattern(off_t i)
{
    uint32_t x = (uint32_t)i * 2654435761u;
    return (uint8_t)(x >> 13 ^ x >> 24);
}

/* the buffering thread's idle work: reclaim, then fill */
static void thread_step(void)
{
    shrink_buffer();
    update_data_counters(NULL);
    if (data_counters.remaining > 0)
        fill_buffer();
}

static int same(const uint8_t *p, off_t pos, size_t n)
{
    size_t i;
    for (i = 0; i < n; i++)
        if (p[i] != pattern(pos + (off_t)i))
            return 0;
    return 1;
}

int main(int argc, char **argv)
{
    static uint8_t mem[RING + GUARD_BUFSIZE], dst[64 * 1024];
    const char *path = argc > 1 ? argv[1] : "bufpeek.bin";
    unsigned peeks = 0, wraps = 0, not_done = 0, reclaimed = 0;
    unsigned bad_data = 0, moved = 0, waited = 0;
    off_t i;
    int id;

    {
        FILE *f = fopen(path, "wb");
        for (i = 0; i < FILE_SIZE; i++)
            fputc(pattern(i), f);
        fclose(f);
    }

    check(buffering_reset((char *)mem, sizeof(mem)), "buffering_reset");
    id = bufopen(path, 0, TYPE_PACKET_AUDIO, NULL);
    check(id >= 0, "bufopen (packet audio, as a codec opens its file)");
    thread_step();

    srand(2722);
    while (1)
    {
        struct memory_handle *h = find_handle(id);
        off_t start = h->start, end = h->end, pos = h->pos;
        size_t ridx = h->ridx;
        int k;

        /* the video player's peeks, around and inside the window */
        for (k = 0; k < 40; k++)
        {
            size_t n = 1 + rand() % 20000;
            off_t at = start - 5000 + rand() % (end - start + 10000);
            ssize_t rc;
            if (at < 0)
                at = 0;
            rc = bufpeek_at(id, at, n, dst);
            peeks++;
            h = find_handle(id);
            if (h->pos != pos || h->ridx != ridx)
                moved++;
            if (h->end != end)
                waited++;       /* a peek must never fill or wait */
            if (at < start)
                reclaimed += rc == ERR_INVALID_VALUE;
            else if (at + (off_t)n > end)
                not_done += rc == ERR_HANDLE_NOT_DONE;
            else
            {
                uintptr_t idx = ringbuf_add(h->data, at - start);
                wraps += idx + n > buffer_len;
                if (rc != (ssize_t)n || !same(dst, at, n))
                    bad_data++;
            }
            if ((at < start && rc != ERR_INVALID_VALUE) ||
                (at >= start && at + (off_t)n > end &&
                 rc != ERR_HANDLE_NOT_DONE))
                bad_data++;
        }

        /* the codec reads on */
        if (pos >= FILE_SIZE)
            break;
        {
            ssize_t got = bufread(id, CODEC_STEP, dst);
            if (got <= 0 || !same(dst, pos, (size_t)got))
                bad_data++;
            bufadvance(id, got);
        }
        thread_step();
    }

    check(bad_data == 0, "every peek inside the window returned the file's "
                         "bytes, every refusal the right code");
    check(wraps > 0, "some peeks crossed the ring's wrap");
    check(not_done > 0, "peeks past the buffered data: ERR_HANDLE_NOT_DONE");
    check(reclaimed > 0, "peeks before the window: ERR_INVALID_VALUE");
    check(moved == 0, "peeks never moved the codec's read position");
    check(waited == 0, "peeks never filled or waited");
    check(bufpeek_at(id + 100, 0, 1, dst) == ERR_HANDLE_NOT_FOUND,
          "unknown handle: ERR_HANDLE_NOT_FOUND");
    printf("     %u peeks: %u across the wrap, %u not yet buffered, "
           "%u reclaimed\n", peeks, wraps, not_done, reclaimed);
    bufclose(id);
    remove(path);
    printf("bufpeek_at: %d failures\n", fails);
    return fails ? 1 : 0;
}
