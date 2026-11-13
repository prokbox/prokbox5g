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

/* #290: the COP co-location's remote reader in the REAL apps/buffering.c
 * (buf_remote_attach/detach, remote_publish/apply/drop and their call
 * sites), on a two-cache model.
 *
 * The model:
 *  - the ring the CPU writes (read() into the cached address) is the CPU's
 *    view; the COP sees only SDRAM (sdram[]), which changes only at the
 *    CPU's commit_dcache() (a whole-cache write-back; no eviction is
 *    modelled: the worst case). SDRAM starts poisoned.
 *  - the struct buf_remote_reader is uncached (SHAREDBSS on the device):
 *    each store is visible to the COP at once.
 *  - the COP's reader runs at every cross-core point the code passes
 *    (commit_dcache's entry, before the write-back; the list mutex; yield;
 *    queue replies; events) and at random consumes [pos, end): every byte
 *    it may read must be in SDRAM and be the file's. While live, its words
 *    must be whole (the handle's id, the ring, a map that holds).
 *
 * Driven synchronously as tests/video/test_bufpeek.c does: the test plays
 * the buffering thread (fills, reclaim) and the CPU's codec thread
 * (attach/detach around a seek, as apps/codec_cop.c does). Seeds explore
 * where the reader runs between the CPU's steps.
 *
 * run.sh --mutate deletes one fence or reorders one publish in a copy of
 * buffering.c per run; every mutant must fail here. */

/* a copy of apps/buffering.c beside the build (run.sh): its quoted
   includes find tests/fencebuf/stub before the real headers */
#include "buffering.c"

#define FILE_SIZE   (700 * 1024 + 333)
#define RING        (64 * 1024)
#define POISON      0xA5u

static int fails, checks;
static unsigned long violations;
static char viol_first[200];

static uint8_t mem[RING + GUARD_BUFSIZE];
static uint8_t sdram[RING + GUARD_BUFSIZE];

/* the reader under test (one at a time, like codec_cop.c's rin) */
static struct buf_remote_reader rd;
static int expect_hid;
static unsigned rng = 1;
static unsigned long consumed, reads_seen, points;
static int commits_locked, commits_unlocked;
static bool in_point;
static bool reader_reads;     /* test_play only: the reader consumes */

static void check(int cond, const char *what)
{
    checks++;
    printf("%s %s\n", cond ? "ok  " : "FAIL", what);
    fails += !cond;
}

static unsigned rnd(void)
{
    rng = rng * 1103515245u + 12345u;
    return rng >> 8;
}

static uint8_t pattern(off_t i)
{
    uint32_t x = ((uint32_t)i + 7u) * 2654435761u;
    uint8_t b = (uint8_t)(x >> 13 ^ x >> 24);
    return b == POISON ? (uint8_t)~POISON : b;
}

static void violation(const char *where, const char *what)
{
    if (!violations++)
        snprintf(viol_first, sizeof(viol_first), "%s: %s", where, what);
}

/* The COP's reader: what it may read at this instant */
static void cop_observe(const char *where)
{
    uint32_t pos, end, fs, mp, mi, len, o;
    const uint8_t *base;

    points++;
    if (!rd.live)
        return;
    /* the words, as the COP would load them (each a single store) */
    pos = rd.pos;
    end = rd.end;
    fs = rd.filesize;
    mp = rd.map_pos;
    mi = rd.map_idx;
    len = rd.len;
    base = (const uint8_t *)rd.base;
    if (rd.hid != expect_hid)
        return violation(where, "live with another handle's id");
    if (base != (const uint8_t *)buffer || len != buffer_len)
        return violation(where, "live with no ring (base/len)");
    if (end > fs)
        return violation(where, "live with end past filesize");
    if (pos < mp || pos > end)
        return violation(where, "live with pos outside map_pos..end");
    if (mi >= len)
        return violation(where, "live with map_idx outside the ring");
    /* every byte it may read: in SDRAM, the file's */
    for (o = pos; o < end; o++)
    {
        uint32_t idx = (mi + (o - mp)) % len;
        if (sdram[idx] != pattern(o))
            return violation(where, "a byte under end not written back "
                                    "(stale SDRAM)");
    }
    reads_seen++;
    /* and at random it reads some (one store: pos) */
    if (reader_reads && end > pos && (rnd() & 3) == 0)
    {
        uint32_t n = 1 + rnd() % (end - pos);
        rd.pos = pos + n;
        consumed += n;
    }
}

void fb_point(const char *where)
{
    if (in_point)
        return;
    in_point = true;
    cop_observe(where);
    in_point = false;
}

void commit_dcache(void)
{
    if (llist_mutex.locked)
        commits_locked++;
    else
        commits_unlocked++;
    fb_point("commit_dcache (before the write-back)");
    memcpy(sdram, mem, sizeof(mem));
}

void fb_mutex_lock(struct mutex *m)
{
    fb_point("mutex_lock");
    if (m->locked++)
        panicf("mutex re-entered\n");
}

void fb_mutex_unlock(struct mutex *m)
{
    if (m->locked-- <= 0)
    {
        m->locked = 0;
        violation("mutex_unlock", "unlock without a lock");
    }
    fb_point("mutex_unlock");
}

void fb_queue_reply(intptr_t r)
{
    (void)r;
    fb_point("queue_reply");
}

void fb_send_event(const void *data)
{
    (void)data;
    fb_point("send_event");
}

void fb_yield(void)
{
    fb_point("yield");
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

static void poison_reader(void)
{
    memset((void *)&rd, 0xEE, sizeof(rd));
    rd.live = 0;
}

/* after each CPU step the handle's published state is the reader's */
static unsigned long stale_after;
static void published(int id)
{
    struct memory_handle *h = find_handle(id);
    if (!rd.live || !h)
        return;
    if (rd.end != (uint32_t)h->end || rd.filesize != (uint32_t)h->filesize)
        stale_after++;
}

static void thread_step(int id)
{
    shrink_buffer();
    update_data_counters(NULL);
    if (data_counters.remaining > 0)
        fill_buffer();
    published(id);
}

static void write_file(const char *path, off_t size)
{
    FILE *f = fopen(path, "wb");
    off_t i;
    for (i = 0; i < size; i++)
        fputc(pattern(i), f);
    fclose(f);
}

static void reset_ring(void)
{
    memset(mem, 0, sizeof(mem));
    memset(sdram, POISON, sizeof(sdram));
    buffering_reset((char *)mem, sizeof(mem));
}

/* 1. attach: the words before live, the write-back before end, under the
 *    list mutex */
static void test_attach(const char *path)
{
    int id;
    struct memory_handle *h;

    reset_ring();
    id = bufopen(path, 0, TYPE_PACKET_AUDIO, NULL);
    thread_step(id);
    h = find_handle(id);
    poison_reader();
    expect_hid = id;
    violations = 0;
    commits_locked = commits_unlocked = 0;
    check(buf_remote_attach(id, &rd), "attach: an audio handle attaches");
    check(violations == 0, viol_first[0] ? viol_first :
          "attach: no instant where the reader is live before its words "
          "and written-back data");
    viol_first[0] = 0;
    check(rd.live == 1 && rd.hid == id && rd.end == (uint32_t)h->end &&
          rd.filesize == (uint32_t)h->filesize &&
          rd.pos == (uint32_t)h->pos &&
          rd.map_pos == (uint32_t)h->start &&
          rd.map_idx == (uint32_t)h->data,
          "attach: live, end/filesize/pos/map are the handle's");
    check(commits_locked >= 1 && commits_unlocked == 0,
          "attach: its write-back runs under the list mutex");
    check(llist_mutex.locked == 0, "attach: mutex released");
    check(buf_remote_detach(&rd) == h->pos, "attach: detach returns pos");
    bufclose(id);
}

/* 2. detach applies the reader's pos before it drops the lease */
static void test_detach(const char *path)
{
    int id;
    struct memory_handle *h;
    off_t got;
    uint32_t want;

    reset_ring();
    id = bufopen(path, 0, TYPE_PACKET_AUDIO, NULL);
    thread_step(id);
    poison_reader();
    expect_hid = id;
    buf_remote_attach(id, &rd);
    want = rd.pos + (rd.end - rd.pos) / 2;
    rd.pos = want;                  /* the COP read this far */
    got = buf_remote_detach(&rd);
    h = find_handle(id);
    check(got == (off_t)want && h->pos == (off_t)want &&
          h->ridx == ringbuf_add(h->data, want - h->start),
          "detach: the reader's pos is applied to the handle and returned");
    check(rd.live == 0, "detach: the reader is no longer live");
    check(buf_remote_detach(&rd) == -1, "detach: a second detach is -1");
    /* a detach of a reader that is not the attached one still clears it */
    rd.live = 1;
    check(buf_remote_detach(&rd) == -1 && rd.live == 0,
          "detach: a stale reader's live is cleared");
    bufclose(id);
}

/* 3. the reader's progress reaches the handle at the thread's look
 *    (update_data_counters, shrink_buffer) */
static void test_apply(const char *path)
{
    int id;
    struct memory_handle *h;
    uint32_t want;

    reset_ring();
    id = bufopen(path, 0, TYPE_PACKET_AUDIO, NULL);
    thread_step(id);
    poison_reader();
    expect_hid = id;
    buf_remote_attach(id, &rd);
    h = find_handle(id);
    want = rd.pos + 1000;
    rd.pos = want;
    update_data_counters(NULL);
    check(find_handle(id)->pos == (off_t)want,
          "apply: update_data_counters takes the reader's pos");
    want += 1000;
    rd.pos = want;
    shrink_buffer();
    h = find_handle(id);
    check(h && h->pos == (off_t)want,
          "apply: shrink_buffer takes the reader's pos");
    buf_remote_detach(&rd);
    bufclose(id);
}

/* 4. close and rebuffer drop the lease */
static void test_drop(const char *path)
{
    int id;

    reset_ring();
    id = bufopen(path, 0, TYPE_PACKET_AUDIO, NULL);
    thread_step(id);
    poison_reader();
    expect_hid = id;
    buf_remote_attach(id, &rd);
    bufclose(id);
    check(rd.live == 0, "drop: closing the handle ends the lease");
    check(buf_remote_detach(&rd) == -1, "drop: nothing attached after it");

    reset_ring();
    id = bufopen(path, 0, TYPE_PACKET_AUDIO, NULL);
    thread_step(id);
    poison_reader();
    expect_hid = id;
    buf_remote_attach(id, &rd);
    violations = 0;
    viol_first[0] = 0;
    bufseek(id, FILE_SIZE - 5000);  /* off the buffer: a rebuffer */
    check(rd.live == 0, "drop: a rebuffer (seek off the buffer) ends the "
                        "lease");
    check(violations == 0, viol_first[0] ? viol_first :
          "drop: the reader never saw the moved data");
    viol_first[0] = 0;
    bufclose(id);
}

/* 5. each publish site: the reader sees end/filesize as the handle */
static void test_publish_sites(const char *path)
{
    int id;
    struct memory_handle *h;
    off_t cut;

    /* bufstripsize */
    reset_ring();
    id = bufopen(path, 0, TYPE_PACKET_AUDIO, NULL);
    thread_step(id);
    poison_reader();
    expect_hid = id;
    buf_remote_attach(id, &rd);
    h = find_handle(id);
    bufstripsize(id, FILE_SIZE - 100);
    check(rd.filesize == (uint32_t)(FILE_SIZE - 100),
          "publish: bufstripsize's new size reaches the reader");
    buf_remote_detach(&rd);
    bufclose(id);

    /* the file ends early (a read returns 0): filesize = end */
    write_file("fencebuf-short.bin", FILE_SIZE);
    reset_ring();
    id = bufopen("fencebuf-short.bin", 0, TYPE_PACKET_AUDIO, NULL);
    h = find_handle(id);
    buffer_handle(id, 1);           /* one chunk */
    poison_reader();
    expect_hid = id;
    buf_remote_attach(id, &rd);
    cut = h->end + 10;
    truncate("fencebuf-short.bin", cut);
    buffer_handle(id, 0);
    h = find_handle(id);
    check(h->filesize == cut && rd.filesize == (uint32_t)cut &&
          rd.end == (uint32_t)cut,
          "publish: a file ending early (read 0) reaches the reader");
    buf_remote_detach(&rd);
    bufclose(id);

    /* the file cannot be reopened: filesize = end */
    write_file("fencebuf-gone.bin", FILE_SIZE);
    reset_ring();
    id = bufopen("fencebuf-gone.bin", 0, TYPE_PACKET_AUDIO, NULL);
    h = find_handle(id);
    buffer_handle(id, 1);
    poison_reader();
    expect_hid = id;
    buf_remote_attach(id, &rd);
    close_fd(&h->fd);               /* closed between fills */
    remove("fencebuf-gone.bin");
    buffer_handle(id, 0);
    h = find_handle(id);
    check(h->filesize == h->end && rd.filesize == (uint32_t)h->filesize,
          "publish: a file that cannot be reopened reaches the reader");
    buf_remote_detach(&rd);
    bufclose(id);
    remove("fencebuf-short.bin");
    (void)path;
}

/* 6. a whole play, the reader running at every cross-core point, over
 *    seeds: attach/detach as codec_cop.c does, a seek, the close */
static void test_play(const char *path, int seeds)
{
    int s;
    unsigned long before_consumed = consumed;

    violations = 0;
    viol_first[0] = 0;
    stale_after = 0;
    reader_reads = true;
    for (s = 1; s <= seeds; s++)
    {
        int id, step = 0;
        rng = (unsigned)s * 2722u;
        reset_ring();
        id = bufopen(path, 0, TYPE_PACKET_AUDIO, NULL);
        expect_hid = id;
        poison_reader();
        thread_step(id);
        buf_remote_attach(id, &rd);
        while (1)
        {
            struct memory_handle *h = find_handle(id);
            if (!h || (h->pos >= FILE_SIZE && rd.pos >= FILE_SIZE))
                break;
            /* the reader's turn outside any CPU step */
            fb_point("idle");
            thread_step(id);
            step++;
            if (step % 7 == 0)
            {
                /* the CPU takes the stream back and gives it again */
                buf_remote_detach(&rd);
                buf_remote_attach(id, &rd);
            }
            if (step == 23 && s % 2)
            {
                /* a seek: detached first (codec_cop.c), then reattached */
                buf_remote_detach(&rd);
                bufseek(id, FILE_SIZE / 2 + (off_t)(rnd() % 4096));
                poison_reader();
                buf_remote_attach(id, &rd);
            }
            if (step > 20000)
            {
                violation("play", "no progress");
                break;
            }
        }
        buf_remote_detach(&rd);
        bufclose(id);
    }
    reader_reads = false;
    check(violations == 0, viol_first[0] ? viol_first :
          "play: at every cross-core point the reader saw only "
          "written-back file bytes under end, its words whole");
    check(stale_after == 0, "play: after every fill the reader's end and "
                            "filesize are the handle's");
    check(consumed - before_consumed >= (unsigned long)seeds * FILE_SIZE / 2,
          "play: the reader consumed the stream (not a vacuous run)");
    printf("     %d seeds, %lu cross-core points, %lu reader looks, %lu "
           "bytes read by the reader\n", seeds, points, reads_seen,
           consumed - before_consumed);
}

int main(int argc, char **argv)
{
    const char *path = argc > 1 ? argv[1] : "fencebuf.bin";

    write_file(path, FILE_SIZE);
    test_attach(path);
    test_detach(path);
    test_apply(path);
    test_drop(path);
    test_publish_sites(path);
    test_play(path, 24);
    remove(path);
    printf("fencebuf: %d checks, %d failures\n", checks, fails);
    return fails ? 1 : 0;
}
