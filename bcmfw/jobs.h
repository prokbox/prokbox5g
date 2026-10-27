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

#ifndef BCMFW_JOBS_H
#define BCMFW_JOBS_H

#include <stdint.h>

/* The display job interface ("JB"), contract rev 0.2 (P19 draft).
 *
 * Small drawing jobs the PP sends instead of pixels: blit, fill, fade,
 * average, scroll, present, chip-timed animations; picture slots the
 * PP uploads once and names by content key.  Carried on the video
 * protocol's rings (hostproto.h: same records, opcodes 0x20-0x3F), so
 * the PP needs one transport.  Constants only, shared by the BCM image,
 * the PP driver, the reference executor (bcmfw/jobs/) and the tests.
 * The contract in words: docs/research/bcm2722/jobs.md.
 *
 * Every job has a CPU fallback with the same result (jobs.md section 6);
 * the reference executor is that fallback and the tests' oracle. */

/* ---- Version and discovery ------------------------------------------- */
#define J_MAGIC              0x4A42      /* 'JB', bits 31:16 of J_VERSION */
#define J_MAJOR              0           /* incompatible changes */
#define J_MINOR              4           /* additions only (in the P19
                                            drafts, also revisions: below) */
/* Revisions:
 *   0.1  the first draft.
 *   0.2  DECODE (0x30) and its input ring; COMMIT's sum is F16w (two
 *        words, A and B: "COMMIT's sum", below) in place of FNV-1a.  A
 *        0.1 COMMIT (slot, FNV-1a: two words) is refused, HP_E_INVAL.
 *   0.3  PAN's stepped form (J_PAN_STEP: the marquee, three more
 *        words), and an animation's frames at its own interval (the
 *        stepped PAN's step; every other animation's J_ANIM_FRAME_US, as
 *        before).  An older image would run the stepped record as a
 *        linear pan, once: the PP sends it only to an image whose
 *        J_VERSION minor is 3 or more (the CPU path otherwise).
 *   Proposed, sim only (#126), not a revision: SPECTRUM (0x33) and BARS
 *        (0x34), the visualizer's, and J_VPU_FFT.  No shipped image
 *        offers them: a PP finds them by their J_OPS bits (19 and 20),
 *        and J_MINOR moves when an image that offers them ships.
 *   0.4  the retained screen (19-4): a SLIDE draws into J_SURF_FRONT, and
 *        its 'from' may be J_SURF_FRONT itself, the frame as it stood when
 *        the ANIM was taken (moved on in place, frame to frame); its
 *        offsets never decrease (J_E_RANGE); at most J_SLIDE_MAX = 48, so
 *        the record fits HP_MAX_PAYLOAD (50 never could).
 *   0.5  (candidate images only, open question #67: no shipped image
 *        offers it, so J_MINOR stays 4 until one ships; a PP finds it by
 *        J_CAPS, below) the staged upload.  ALLOC takes an optional
 *        sixth word, J_ALLOC_F_STAGE: the image may then answer with the
 *        address of its staging window in SDRAM (the host port's view)
 *        and J_ALLOC_STAGED, and the PP writes the slot's rows there, at
 *        the same pitch; COMMIT first moves the window into the slot (the
 *        BCM's own DMA, else its core) and then checks the slot as
 *        before.  COMMIT's reply also says how the pixels came (J_HOW_*),
 *        and its J_E_SUM reply carries the BCM's sums (J_SUMREP_*).  An
 *        image without the form ignores the sixth word and answers as
 *        rev 0.4, so the PP's path is the same either way: it writes
 *        where ALLOC said. */
#define J_VERSION_WORD       (J_MAGIC << 16 | J_MAJOR << 8 | J_MINOR)

/* The video protocol header's spare word: the job block's address, 0 if
 * the image offers no jobs.  (0x140-0x1DF, once reserved for this, is
 * taken by the clock, idle, timing, DMA and probe words of the test and
 * video images: jobs.md section 2.) */
#define HP_JOBS              0x138

/* ---- The job block (BCM-written unless marked) ----------------------- */
#define J_VERSION            0x00   /* written last: J_VERSION_WORD */
#define J_OPS                0x04   /* bit n: op 0x20 + n implemented */
#define J_EPOCH              0x08   /* HP_ENC: +1 whenever slot contents
                                       are lost (start, video OPEN, a game
                                       session, J_OP_RESET) */
#define J_FENCE              0x0C   /* HP_ENC: the last fence retired */
#define J_MODE               0x10   /* J_MODE_*: who owns the arenas */
#define J_SLOT_TAB           0x14   /* the slot table's address */
#define J_SLOT_N             0x18   /* its entries */
#define J_FREE_SRAM          0x1C   /* bytes free in the SRAM pools */
#define J_FREE_SDRAM         0x20   /* bytes free in SDRAM (0: not up) */
#define J_ERRORS             0x24   /* jobs failed since start (of J_DONE) */
#define J_LAST_ERR           0x28   /* op | code << 8 | tag << 16 */
#define J_BUSY_US            0x2C   /* time spent in jobs, 1 MHz, wraps */
#define J_DONE               0x30   /* jobs run since start, each counted
                                       when it ends, whatever its status
                                       (J_ERRORS of them failed): the PP
                                       sees its sent jobs drained when
                                       J_DONE has moved by their number */
#define J_POWER              0x34   /* J_PWR_* | core MHz << 16 */
/* J_DEC_IN_RD 0x38, J_DEC_IN_WR 0x3C: DECODE's input ring (below) */
#define J_CUR_OP             0x40   /* HP_ENC: the op running now (0x20-
                                       0x3F), 0 between jobs.  Set before
                                       a job starts and cleared when it
                                       ends, so a PP that times a job out
                                       knows which op was slow (the
                                       deadlines, jobs.md section 6) */
#define J_VPU                0x44   /* the vector unit's start-up checks,
                                       written once by jobs_init: J_VPU_*
                                       (0 in images before them) */
#define J_OVL                0x48   /* the overlay region (J_OP_OVERLAY;
                                       proposed, sim only): its base |
                                       its size >> 12 (base and size whole
                                       4 KB), written once by jobs_init;
                                       valid only if J_OPS bit 21 is set */
#define J_CAPS               0x4C   /* rev 0.5 (candidate): J_CAPS_MAGIC |
                                       J_CAP_* bits, written once by
                                       jobs_init; older images never write
                                       the word, so it counts only with
                                       the magic in its high half */
/* MERGED IMAGE, next re-verify batch (album-render.md section 5.3):
 * DECODE from the merged image's overlay (video/dec_ovl.c) has no
 * decode_prepare(), so for a ring source the PP sends len clipped to the
 * ring's bytes (jref's bound) and writes the file's own length here,
 * plain (not HP_ENC'd: whole before the DECODE record is written); 0: the
 * source slot holds the whole file.  The display-only image ignores it */
#define J_DEC_LEN            0x50
/* Batch 4, information for the bench only (video/bschk.c; the merged
 * image; 0 in images before it).  J_VPU2: the start-up checks of the
 * vector forms a vector loop filter would need (decoder.md, "The loop
 * filter in vectors"): J_VPU2_CHECKED, and J_VPU2_BIT(f) for each form f
 * that agreed with the C on every lane, f = 1 BSL1 (vldp16 + vldq16 at a
 * line phase), 2 BSF1 (vsub discard H16 - H16 setf), 3 BSM1 (vmov H8 #imm
 * ifn), 4 BSR1 (vsub discard H8 - H8 setf), 5 BSX1 (vmax H8), 6 BSS1
 * (vst8 d00 single), 7 BSZ1 (vmov H8 r ifz).  The first form to fail:
 * J_VPU2_FAIL = f | its first differing lane << 8, with that lane's got
 * and expected halfwords.  No code uses the forms: a failure costs
 * nothing.  J_LAT_*: the first OPEN's load timings (open question #7),
 * ns x 10 a load (8 loads an iteration, the loop's overhead included),
 * bytes in the low half, words in the high half; J_LAT_MHZ the core's
 * MHz then, with J_LAT_DONE once measured (0 before the first OPEN) */
#define J_VPU2               0x54
#define J_VPU2_FAIL          0x58
#define J_VPU2_GOT           0x5C
#define J_VPU2_EXP           0x60
#define J_LAT_SDRAM          0x64
#define J_LAT_SRAM           0x68
#define J_LAT_MHZ            0x6C
#define J_LAT_SINK           0x70   /* the loads' sum (keeps them) */
#define J_VPU2_CHECKED       0x1
#define J_VPU2_BIT(f)        (1u << (f))
#define J_VPU2_FORMS         7
#define J_LAT_DONE           0x80000000u
#define J_BLOCK_SIZE         0x80

/* Arena owners (J_MODE).  Video and games are exclusive of each other and
 * of music; entering VIDEO or GAME evicts the UI's slots (epoch + 1). */
#define J_MODE_UI            1
#define J_MODE_VIDEO         2      /* a VP session owns the video arena */
#define J_MODE_GAME          3      /* a Class C game owns the pools */

/* J_VPU bits.  CHECKED: the image ran its lane-shift self-check at start
 * (one out-of-phase gather through jv_group16 at phases 5 and 13, against
 * the C result).  SHIFT: it agreed, so groups and blocks whose source is
 * out of phase with the destination gather on the vector unit; clear
 * (CHECKED alone), they take the C path, exact either way (jobs.md
 * section 10, "The lane shift's self-check").  MAC: the image decodes
 * (J_OP_DECODE) and its IDCTs (4, 8 and 16 points) run on the vector
 * unit, the 32-bit multiply-accumulate having agreed with the C on four
 * known blocks at each size at start; clear, jdec's IDCTs are the C,
 * exact either way (jpeg-spike.md sections 13-14).  The PP driver logs the word once per BCM start. */
#define J_VPU_CHECKED        0x1
#define J_VPU_SHIFT          0x2
#define J_VPU_MAC            0x4
/* Proposed, sim only: the image offers SPECTRUM and its FFT runs
 * on the vector unit, its start-up check (viz_vpu.c: four signals at each
 * size, kernel against the C) having agreed; clear, the C reference,
 * exact either way */
#define J_VPU_FFT            0x8
/* Proposed, sim only: the image offers BARS and draws it on the vector
 * unit (viz_vpu.c viz_bars, gen_viz.py jv_bars16), its start-up check (two
 * frames, kernel against jviz_bars_draw) having agreed; clear, the C,
 * exact either way */
#define J_VPU_BARS           0x10
/* The image decodes and jdec's colour stage (YCbCr to dithered RGB565,
 * jdec_vpu.c yuv565) runs on the vector unit, its start-up check (64
 * pixels at two dither rows, kernel against the C) having agreed; clear,
 * the C, exact either way (2026-10-25: the stage had no check before) */
#define J_VPU_YUV            0x20
/* Proposed, sim only: BARS honours J_BARS_FLIP (the visualizer's Mirror
 * on the BCM).  Set by the overlay (or the BCMFW_VIZ image) that offers
 * BARS, whichever path draws it (the vector one's start-up check covers
 * the flip, J_VPU_BARS; the C's is the reference): an older overlay
 * ignores the flag bit and draws upright, so the PP sends J_BARS_FLIP
 * only with this bit (bcmjob_bars_flip()) */
#define J_VPU_FLIP           0x40

/* J_CAPS (rev 0.5, candidate) */
#define J_CAPS_MAGIC         0x43410000  /* 'CA' << 16 */
#define J_CAPS_VALID(v)      (((v) & 0xFFFF0000u) == J_CAPS_MAGIC)
#define J_CAP_STAGED         0x1    /* ALLOC's J_ALLOC_F_STAGE is honoured */
#define J_CAP_SDRAM_FAIL     0x4    /* the window's SDRAM didn't come up:
                                       staged ALLOCs answer as rev 0.4 */
#define J_CAP_DEC_LENT       0x8    /* DECODE is the merged image's overlay
                                       (video/dec_ovl.c): the PP lends its
                                       work slot (J_DEC_WORK_KEY) and writes
                                       J_DEC_LEN; set by the overlay's init,
                                       gone with it at an epoch */

/* Power words (J_POWER bits 7:0), see power-contract.md */
#define J_PWR_ACTIVE         1
#define J_PWR_IDLE_POLL      2      /* busy-polling between jobs */
#define J_PWR_IDLE_SLEEP     3      /* core asleep between jobs (5i) */

/* ---- Slots ------------------------------------------------------------- */
/* One table entry, 32 bytes, BCM-written */
#define J_SLOT_KEY_LO        0x00   /* the content key (the PP's hash) */
#define J_SLOT_KEY_HI        0x04
#define J_SLOT_ADDR          0x08   /* pixel 0, in the BCM's map (ALLOC) */
#define J_SLOT_BYTES         0x0C
#define J_SLOT_WH            0x10   /* w | h << 16 */
#define J_SLOT_INFO          0x14   /* fmt | class << 8 | state << 16 */
#define J_SLOT_EPOCH         0x18   /* the epoch it was filled in */
#define J_SLOT_SUM           0x1C   /* its F16w B (COMMIT) */
#define J_SLOT_ENTRY         0x20

#define J_FMT_RGB565         1      /* the only format in rev 0.1 */
#define J_FMT_CLUT8          2      /* reserved: emergency only (owner) */

/* Slot classes and their pools (memory-contract.md) */
#define J_CLS_DRIFT          1      /* the drift's covers, 300x300 */
#define J_CLS_PANEL          2      /* the panel pool, 300x300 */
#define J_CLS_COVERFLOW      3      /* Cover Flow textures, 128x128 */
#define J_CLS_STRIP          4      /* marquee and text strips */
#define J_CLS_THUMB          5      /* thumbnail atlases */
#define J_CLS_SCRATCH        6      /* work surfaces, one owner at a time */
#define J_CLS_GAME           7      /* a Class C game's (J_MODE_GAME only) */
#define J_CLS_APP            8      /* a Class B module's, beside music (P12
                                       section 2.1; gap G7) */
#define J_CLASSES            9

/* Class budgets (memory-contract.md section 4; gap G8): bytes and slots.
 * The shell can static_assert its requests against these. */
#define J_BUDGET_DRIFT       (2 * 182400)   /* 2 x 300x300 at J_DRIFT_PITCH */
#define J_SLOTS_DRIFT        2
#define J_BUDGET_PANEL       (8 * 180224)   /* SDRAM, on demand */
#define J_SLOTS_PANEL        8
#define J_BUDGET_COVERFLOW   (32 * 32768)   /* SDRAM, while Cover Flow runs */
#define J_SLOTS_COVERFLOW    32
#define J_BUDGET_STRIP       (212 * 1024)   /* SRAM arena + 0x12B000 */
#define J_SLOTS_STRIP        16
#define J_BUDGET_THUMB       (64 * 1024)
#define J_SLOTS_THUMB        16
#define J_BUDGET_SCRATCH     (32 * 1024)
#define J_SLOTS_SCRATCH      1
#define J_BUDGET_APP         (51 * 1024 + 512)  /* the arena's spare (56 KB
                                       until the drift's 304 pitch) */
#define J_SLOTS_APP          8
/* J_CLS_GAME: the arena and SDRAM pools, J_MODE_GAME only */

/* A slot's row pitch, in pixels (ALLOC replies it in bytes): the width
 * rounded up to 16 pixels, so every row of every class starts 32-byte
 * aligned, as Apple's own images are laid out [apple-RE].  A 300-wide
 * cover's rows are 608 bytes apart and BLIT, FADE and AVG to and from it
 * reach the BCM's vector unit (jobs.md section 10) whenever the window's
 * x lines up with the frame's (x mod 16 equal).  Every class since
 * 2026-10-25 (the drift's only before): the host port's write address
 * ignores bits 1:0 [silicon, CR1: a 301-wide strip at pitch 602 wrote
 * its odd rows one pixel early], so no row may start mid-word.  The PP
 * writes a slot row by row at this pitch (cls is kept for the callers) */
#define J_SLOT_PITCH(cls, w) ((void)(cls), ((uint32_t)(w) + 15) & ~15u)
#define J_DRIFT_PITCH        304    /* a 300-wide cover's: J_SLOT_PITCH */

#define J_ST_EMPTY           0
#define J_ST_ALLOCATED       1      /* the PP may write its pixels */
#define J_ST_READY           2      /* committed: jobs may read it */

/* Surface ids in job records: a slot number, or one of these */
#define J_SURF_FRONT         0xF0   /* the frame the panel shows next */
#define J_SURF_BACK          0xF1   /* the second frame */
#define J_SURF_IS_SLOT(s)    ((s) < 0xF0)

/* ---- Records ----------------------------------------------------------- */
/* hostproto.h's: op | n << 8 | tag << 16, n payload words.  A job replies
 * only if its tag has J_TAG_REPLY, or if it fails (always, with its
 * status), or if it is a FENCE (always).  Jobs run in ring order. */
#define J_TAG_REPLY          0x8000

/* Packed geometry words */
#define J_XY(x, y)           ((uint32_t)(x) | (uint32_t)(y) << 16)
#define J_X(w)               ((w) & 0xFFFF)
#define J_Y(w)               ((w) >> 16)

/* ---- Jobs (opcodes 0x20-0x3F) ---------------------------------------- */
#define J_OP_FENCE           0x20   /* seq -> seq.  Retires every earlier
                                       job; posts J_FENCE */
#define J_OP_ALLOC           0x21   /* class, w|h<<16, fmt, key lo, key hi
                                       [, J_ALLOC_F_* (rev 0.5)]
                                       -> slot, addr, pitch bytes, flags
                                       (J_ALLOC_HIT: it already holds this
                                       key in this epoch: no upload).
                                       addr: the slot's first pixel as an
                                       address in the BCM's map (its pool's
                                       base, config.h BCMFW_JOBS_*, plus
                                       the slot's offset), where the PP's
                                       port writes go; pitch: J_SLOT_PITCH
                                       x 2 */
#define J_OP_COMMIT          0x22   /* slot, A, B [, flags] -> A, B: the PP
                                       wrote the pixels, READY.  A, B: the
                                       pixels' F16w (below), always checked
                                       (an all-black slot's 0, 0 too),
                                       unless flags has J_COMMIT_NOCHECK
                                       (a ring or scratch slot); the reply
                                       has the BCM's sums (rev 0.5 images:
                                       then J_HOW_*; on J_E_SUM the words
                                       J_SUMREP_*) */
#define J_COMMIT_NOCHECK     0x1
/* Rev 0.5 (candidate): ALLOC's optional sixth word, and its reply's flag.
 * J_ALLOC_F_STAGE asks for the staged upload; the reply's J_ALLOC_STAGED
 * says the addr word is the image's window (the host port's address, the
 * slot's pitch), else the slot itself as before (an image without the
 * form, a hit, a slot larger than the window, or SDRAM that didn't come
 * up).  The window holds one upload: the last staged ALLOC's, until its
 * COMMIT (or the slot's FREE, or an epoch change); a COMMIT of a slot
 * whose window was taken since checks the slot as it stands (J_E_SUM,
 * J_HOW_DIRECT). */
#define J_ALLOC_F_STAGE      0x1
#define J_ALLOC_STAGED       0x2
/* How a COMMIT's pixels came (rev 0.5, candidate): the third reply word
 * after A and B; on J_E_SUM, J_SUMREP_HOW */
#define J_HOW_DIRECT         0      /* the PP wrote the slot */
#define J_HOW_DMA            1      /* the window, by the BCM's DMA */
#define J_HOW_CORE           2      /* the window, by the core (the DMA
                                       timed out) */
/* J_E_SUM's reply words (rev 0.5, candidate; status first): what the BCM
 * read, so a sum fault on its side can be told from a lost upload.  A
 * window B equal to the PP's B with a slot B that differs: the move
 * failed; a window B that differs too: the pixels never arrived whole (or
 * the sums disagree, when the first word is right) */
#define J_SUMREP_A           1      /* the slot's F16w A, as the BCM sums */
#define J_SUMREP_B           2      /* ... and B */
#define J_SUMREP_W0          3      /* the slot's first word (pixels 0, 1) */
#define J_SUMREP_HOW         4      /* J_HOW_* */
#define J_SUMREP_WIN_B       5      /* the window's B (0: J_HOW_DIRECT) */
#define J_SUMREP_N           6      /* words, status included */
#define J_OP_FREE            0x23   /* slot */
#define J_OP_BLIT            0x24   /* src, sx|sy<<16, w|h<<16, dst,
                                       dx|dy<<16, flags (J_BLIT_*) */
#define J_OP_FILL            0x25   /* dst, x|y<<16, w|h<<16, rgb565 */
#define J_OP_FADE            0x26   /* dst, dx|dy<<16, w|h<<16, a, ax|ay<<16,
                                       b, bx|by<<16, weight 1..256: dst =
                                       a + (b - a) * weight, the shell's
                                       xfade_rect() rounding exactly;
                                       [flags (J_FADE_SHADE_B | n), then n
                                       alpha bytes, 4 to a word]: b's first
                                       n columns shaded (as J_OP_SHADE)
                                       before the blend (the drift's fade
                                       toward a raw cover, ui_panel_fade) */
#define J_OP_AVG             0x27   /* dst, dx|dy<<16, w|h<<16, a, ax|ay<<16,
                                       b, bx|by<<16: the shell's AVG2 */
#define J_OP_SCROLL          0x28   /* surf, x|y<<16, w|h<<16, dy (signed
                                       16): rows move, the rows uncovered
                                       keep their old pixels */
#define J_OP_PRESENT         0x29   /* surf, x|y<<16, w|h<<16, flags
                                       (J_PRES_*) -> us waited.  The rect
                                       to the panel */
#define J_OP_ANIM            0x2A   /* kind (J_ANIM_*), then its words:
                                       a chip-timed animation; ends with
                                       J_EV_ANIM (tag) */
#define J_OP_ANIM_STOP       0x2B   /* tag of the ANIM -> frames shown.
                                       One animation runs at a time: a new
                                       ANIM replaces a running one (which
                                       sends no J_EV_ANIM).  The panel
                                       keeps the last frame drawn */
#define J_OP_SHADE           0x2D   /* surf, x|y<<16, w|h<<16, then w alpha
                                       bytes (w <= J_SHADE_MAX), 4 to a
                                       word, low byte first: column c of
                                       the rect darkened by alpha[c]/256,
                                       ui_shadow_apply() exactly (the split
                                       shadow, 8b-3) */
#define J_OP_STATS           0x2C   /* -> J_DONE, J_ERRORS, J_BUSY_US,
                                       free SRAM, free SDRAM, epoch */
#define J_OP_RESET           0x2F   /* every slot empty, epoch + 1 */
/* Rev 0.2 draft (jpeg-spike.md section 8): JPEG to up to J_DEC_MAX
 * outputs, the art builder's decode (bcmfw/jobs/jdec.h, one definition
 * with the CPU path).  Payload: the source slot (its first len bytes are
 * the file), len, n, then per output: dw|dh<<16 (resized size),
 * cx|cy<<16 and cw|ch<<16 (the window kept), the destination surface
 * and x|y<<16 in it.  -> sw|sh<<16.  Errors: J_E_DEC_* (the caller tries
 * its next source), J_E_RANGE, HP_E_NOMEM.  J_T_DECODE per source Mpx. */
#define J_OP_DECODE          0x30
#define J_DEC_MAX            4
#define J_DEC_ARGS(n)        (3 + 5 * (n))
/* S3b, the input ring (album-render.md section 4).  The source word's
 * J_DEC_RING bit: the slot is a ring the PP streams the file through,
 * and len is the file's (it may be longer than the slot).  The ring: a
 * J_CLS_SCRATCH slot (128 x 128 = 32 KB), committed with sum 0; its bytes
 * a power of two, 4-128 KB.  Two job block words, HP_ENC'd, each
 * J_DEC_IDX(pass, 64-byte units mod 4096):
 *   J_DEC_IN_RD (BCM): the pass it reads, the units it has consumed.  A
 *     new pass means the BCM went back to byte 0: the PP starts the file
 *     again at ring byte 0.  A file of at most the ring's bytes streams
 *     once, whatever the passes;
 *   J_DEC_IN_WR (PP): the pass it writes, (pass, 0) first; the units
 *     written, the last partial, at most a ring ahead of J_DEC_IN_RD; or
 *     J_DEC_ABORT when it can't read the file.  The BCM sets it to
 *     J_DEC_IDLE at a DECODE's start, before the first pass; the PP
 *     writes it only after it sees a new pass.
 * A stall of J_DEC_STALL_MS or an abort fails the job (J_E_DEC_CORRUPT).
 * An image that offers DECODE has a work area of at least J_DEC_WORK_MIN
 * bytes.  A job needing more (jdec.h jdec_work(): the decoder's state, its
 * rows, and JDEC_WORK_OUT for each output) fails HP_E_NOMEM, and the PP's
 * twin decodes it instead.  J_DEC_WORK_MIN was rev 2's NP box (236,592
 * B), whose whole output sat in the work area; jdec now streams its
 * outputs (jpeg-spike.md section 15), so the minimum is the promise the
 * PP decides by, set (the owner, 2026-10-24) to the 16:9 art set: a
 * 534 x 300 fill plus the NP box and the thumbnail, 184,428 B, which
 * covers wide art and leaves room for video later.  The work areas stay
 * as laid out (236,544 B in the display-only image), so 52,116 B above
 * the promise could go back to the pools once one needs them; a single
 * fill wider than about 623 x 300 (2.08:1) now goes to the twin. */
#define J_DEC_RING           0x100
/* The merged image's DECODE overlay has no work area of its own: the PP
 * lends one, a J_CLS_DRIFT slot of this epoch keyed J_DEC_WORK_KEY ("WORK";
 * hi 0), 300 x 300 (182,400 B: the drift's own cover's place, uploaded
 * again after), freed after the job.  A set needing more fails
 * HP_E_NOMEM: the PP checks bcmjob_dec_work() against the lent slot's
 * bytes first (a 16:9 fill alone, 171,404 B, fits; the 16:9 art set in
 * one job, 184,428 B, goes out as single decodes) */
#define J_DEC_WORK_KEY       0x4B524F57
#define J_DEC_WORK_SIDE      300    /* the lent slot: 300 x 300 RGB565 */
#define J_DEC_WORK_LENT      (J_DEC_WORK_SIDE * J_DRIFT_PITCH * 2)  /* 182,400 */
#define J_DEC_IN_RD          0x38
#define J_DEC_IN_WR          0x3C
#define J_DEC_IDX(p, u)      ((uint32_t)(p) << 12 | ((u) & 0xFFF))
#define J_DEC_PASS(v)        ((v) >> 12 & 0xF)
#define J_DEC_UNITS(v)       ((v) & 0xFFF)
#define J_DEC_ABORT          0xFFFF
#define J_DEC_IDLE           0xF000 /* J_DEC_IN_WR as the BCM leaves it at a
                                       DECODE's start (pass 15: none) */
#define J_DEC_UNIT           64
#define J_DEC_PASSES         15
#define J_DEC_STALL_MS       1500
#define J_DEC_WORK_MIN       184428  /* the 16:9 art set: jdec_work() of a
                                       534 x 300 fill + the NP box + the
                                       thumbnail (build.py checks it holds
                                       the NP box) */
/* Reserved, not in rev 0.1 (numbers fixed now so they never move) */
#define J_OP_GLYPHS          0x31   /* glyph runs from an atlas (item 7) */
#define J_OP_TRIS            0x32   /* r3d triangle lists (Cover Flow) */
/* PROPOSED, SIM ONLY (#126; docs/research/bcm2722/visualizer.md; J_OPS bits
 * 19 and 20; not a revision: J_MINOR moves when an image offering them ships):
 * the Now Playing visualizer, one definition with the PP's CPU path
 * (bcmfw/jobs/jviz.h).  Built only into the visualizer image (BCMFW_VIZ),
 * the display-only image's overlay (J_OP_OVERLAY) and the host model; no
 * shipped image offers them.
 *
 * SPECTRUM: src (a READY slot whose first 2 n bytes hold n 16-bit samples
 * in split order: x[0], x[2] .. x[n-2], x[1], x[3] .. x[n-1]; the PP
 * rewrites them between jobs, so it commits the slot once with
 * J_COMMIT_NOCHECK), n | nb << 16 (n 256 or 512, 1 <= nb <= 32 bands),
 * then nb + 1 band edges e[0] <= ... <= e[nb] <= n / 2, two halfwords to a
 * word (low first): band b is bins e[b] .. e[b+1] - 1.  -> nb levels,
 * two halfwords to a word: band b's largest power as jviz_l2 (16 log2,
 * 0-511, 0.19 dB a step).  The PP keeps the smoothing, the peaks, the
 * auto-gain and the beat (they are state; slots are a cache).  Errors:
 * HP_E_INVAL, J_E_SLOT, J_E_RANGE (the slot holds fewer than 2 n bytes).
 *
 * BARS: dst surface, x|y<<16, w|h<<16 (the rect), n | barw << 8 | gap << 16
 * | cap_h << 24 (1 <= n <= 32), bot | top << 16 (the gradient's RGB565
 * ends, bottom to top), cap | bg << 16, flags (J_BARS_*), then n heights
 * and n peaks in pixels from the rect's bottom, a byte each, four to a
 * word (low first): n bars of barw pixels gap apart, centred in the rect
 * (jviz_bars_draw).  J_BARS_FLIP (J_VPU_FLIP images only) draws the same
 * frame upside down: rect row r at row h - 1 - r, by addressing the rows
 * bottom up (no second pass, no extra cycles).  Errors: HP_E_INVAL, J_E_SLOT, J_E_RANGE (the rect, or
 * bars wider than it). */
#define J_OP_SPECTRUM        0x33
#define J_OP_BARS            0x34
#define J_SPEC_ARGS(nb)      (2 + ((nb) + 2) / 2)
#define J_BARS_ARGS(n)       (7 + 2 * (((n) + 3) / 4))
#define J_SPEC_BANDS_MAX     32
#define J_BARS_BG            0x1    /* the rect filled with bg first */
#define J_BARS_CAP           0x2    /* a cap of cap_h rows at each peak */
#define J_BARS_ROUND         0x4    /* each bar's top row inset 1 pixel */
#define J_BARS_FLIP          0x8    /* upside down (J_VPU_FLIP): Mirror */
/* PROPOSED, SIM ONLY (#126; visualizer.md section 13; J_OPS bits 22, 23
 * and 24; not a revision): the visualizer's Scope, Radial and Spectrogram,
 * from the display-only image's overlay (each offered only if its
 * start-up check agrees: viz_vpu.c viz_draw_init) and the host model.
 * Plain C on every runner (jviz.h jviz_poly_draw, jviz_segs_draw,
 * jviz_pal_draw: the PP's CPU path draws the same pixels).  No reply.
 *
 * POLYLINE: dst surface, x|y<<16, w|h<<16 (the rect, w <= 320), src (a
 * READY slot), off | step << 16 (elements), n | flags << 16 (2 <= n <=
 * 1024 points; J_POLY_U8: the elements are bytes, each its point's row,
 * else int16 samples, each row h/2 - (v gain >> 16)), gain | half << 16
 * (gain Q16; half: the elements are a split-order window, element e at
 * index e/2 if even, else half + e/2: SPECTRUM's slot as it stands; 0,
 * at index e), rgb565 | width << 16 (1-3): point i's row from element off
 * + i step, at column i (w-1) / (n-1); the connected line under a
 * width-square brush.  Errors: HP_E_INVAL (n, width), J_E_SLOT, J_E_RANGE
 * (the rect, or an element past the slot).
 *
 * SEGMENTS: dst, x|y<<16, w|h<<16, cx|cy<<16 (the centre in the rect,
 * signed), n | a0 << 8 | astep << 16 | width << 24 | flags << 28 (1 <=
 * n <= 48; segment i at angle index (a0 + i astep) mod 48, 0 up,
 * clockwise; width 1-3; J_SEG_*), r0 | lmax << 8 | rring << 16 | capd <<
 * 24 | ringd << 28 (radii and lengths in pixels: the base, the longest
 * segment and the gradient's span, the dot ring's radius; dot sizes),
 * c0 | c1 << 16 (the gradient, base to tip), cap | ring << 16 (the dots'
 * colours), then n lengths and n peaks, a byte each, four to a word (low
 * first).  The whole frame, 48 segments: 32 words.  Errors: HP_E_INVAL,
 * J_E_SLOT, J_E_RANGE (the rect).
 *
 * PALROW: dst, x|y<<16, w|h<<16 (the rows to draw: the spectrogram's
 * newest step, after a SCROLL), lut (a READY slot holding 256 RGB565
 * entries, uploaded once), nb (1 <= nb <= 96, nb <= w), then nb levels, a
 * byte each, four to a word: cell b, columns (b w)/nb .. ((b+1) w)/nb - 1
 * of every row, in lut[level b].  Errors: HP_E_INVAL, J_E_SLOT, J_E_RANGE
 * (the rect, or a LUT slot under 256 entries). */
#define J_OP_POLYLINE        0x36
#define J_OP_SEGMENTS        0x37
#define J_OP_PALROW          0x38
#define J_POLY_ARGS          8
#define J_SEGS_ARGS(n)       (8 + 2 * (((n) + 3) / 4))
#define J_PAL_ARGS(nb)       (5 + ((nb) + 3) / 4)
#define J_SEG_N_MAX          48
#define J_PAL_N_MAX          96
#define J_POLY_U8            0x1    /* bytes: each point's row */
#define J_SEG_CAP            0x1    /* a dot at each peak */
#define J_SEG_RING           0x2    /* a dot ring at rring */
#define J_SEG_ROUND          0x4    /* dots of 3 or more lose their corners */
/* PROPOSED, SIM ONLY (#126; visualizer.md section 8, jobs.md section 5,
 * "Overlays"; J_OPS bit 21; not a revision): load an overlay, ops this
 * image lacks, into its overlay region (J_OVL).  The display-only image
 * offers it; its one overlay so far is the visualizer's (SPECTRUM and
 * BARS: bcmfw/video/viz_ovl.c).  The PP first writes the overlay's
 * image (a build's viz-ovl.bin, whole words) at the region's base
 * through the port, as it writes a slot's pixels, then sends
 *   OVERLAY: words, A, B
 * A and B: the image's F16w as halfwords (COMMIT's sum, j_f16w).  The
 * BCM checks the length (the 7 header words to the region's size), the
 * sum, then the header (jref.h struct jref_ovl: the ABI word, and the
 * load end = base + 4 words), then runs the overlay's init (the
 * visualizer's runs its FFT's start-up check, about 0.5 M instructions,
 * 20 ms at 27 MHz, and sets J_VPU_FFT if it agrees) and sets the J_OPS
 * bits of the ops it offers that the image lacks: the PP reads them
 * there.  Every load drops the overlay's ops first, so a failed one
 * leaves none.  They last until the epoch moves (J_OP_RESET, a mode
 * change, a restart): the region is a cache like the slots, and the PP
 * loads the overlay again after any epoch change, as it re-uploads its
 * slots.  Errors: HP_E_INVAL (the length), J_E_SUM (a torn or corrupt
 * upload), HP_E_VERSION (the header: another build of the image, another
 * interface (J_OVL_ABI), or a truncated image).  The ABI word is vc2cc's
 * __vc2_abi: the overlay is linked at the region's base against this
 * image's map (bcmfw/build.py OVL_IMPORTS), so only the image build it
 * was linked against accepts it. */
#define J_OP_OVERLAY         0x35
#define J_OVL_MAGIC          0x4F564C31  /* 'OVL1' */
#define J_OVL_ABI            1      /* the interface version in __vc2_abi:
                                       struct jref_ovl, the import list,
                                       struct jref (bump on any change) */

#define J_ALLOC_HIT          0x1

#define J_FADE_SHADE_B       0x100  /* FADE flags: | n columns (bits 7:0) */
#define J_SHADE_MAX          32

#define J_BLIT_OPAQUE        0x0
#define J_BLIT_KEY           0x1    /* skip pixels equal to the key colour
                                       (bits 31:16 of flags) */

#define J_PRES_TE            0x1    /* start on the panel's next TE edge */
#define J_PRES_WAIT          0x2    /* reply when the transfer is done */

/* J_OP_ANIM kinds */
#define J_ANIM_SLIDE         1      /* from, to (surfaces), x|y<<16, w|h<<16,
                                       dir (0 left 1 right 2 up 3 down),
                                       frame ms, count, then count offsets
                                       in pixels, two to a word (low half
                                       first; at most J_SLIDE_MAX), never
                                       decreasing: frame k shows offset k,
                                       so the shell's own easing decides
                                       every frame (gap G6).  Drawn into
                                       J_SURF_FRONT; from = J_SURF_FRONT is
                                       the frame as the ANIM found it (rev
                                       0.4: the retained page, jobs.md
                                       "Shell recipe") */
#define J_SLIDE_MAX          48     /* 8 + 24 words: HP_MAX_PAYLOAD */
#define J_ANIM_PAN           2      /* slot, x0|y0<<16, x1|y1<<16, w|h<<16,
                                       dst xy, period ms, loop: a window
                                       walked across a slot (the drift's
                                       pan), x0 -> x1 linearly over the
                                       period; loop 0 once, 1 again from
                                       the start, 2 back and forth.
                                       With J_PAN_STEP in loop: the
                                       stepped form (below) */
/* PAN's stepped form (rev 0.3; the marquee, jobs.md "Shell recipe: the
 * marquee").  loop = J_PAN_STEP | repeat (0: one lap, then the rest
 * window, and the animation ends; else lap after lap until ANIM_STOP),
 * period = the step's ms, and three more words:
 *   [8]  step px | pause ms << 16
 *   [9]  rest x|y<<16: the window shown while paused (w x h, in the slot)
 *   [10] start ms: where in the timeline the first frame is (0: the lap's
 *        start, its pause; pause + (k - 1) x step: step k)
 * A lap, t ms into it: the rest window while t < pause, then step
 * k = (t - pause) / step ms + 1 at (x0 + k x step px, y0), for k = 1 ...
 * K - 1, where K x step px is the first multiple at or past x1 - x0;
 * then the next lap (pause, steps), so a lap lasts pause + (K - 1) x step
 * ms.  y1 is not used (y0's).  t is start plus the time since the job
 * ran; frames come every step ms from then.  Refused (J_E_RANGE) unless
 * step px and ms are not 0, the step ms below 65,536, x1 - x0 > step px
 * (one step at least), and the rest window lies in the slot.  (Words are
 * counted in the payload: [0] is the kind.)  Each frame is presented
 * widened to whole pixel pairs (an even x and width: the LCD DMA's
 * unit), so the front frame's column either side must hold what the
 * panel shows (the shell's push of the row; no unpresented drawing
 * there).  The shell's
 * F2 marquee: step 2 px every 50 ms, pause 1000 ms, x1 - x0 = the
 * label's width + its 40 px gap.  A strip's pitch is its width: rounded
 * up to 16 px, its rows stay 32-byte aligned and the frames reach the
 * vector unit (jobs.md section 10). */
#define J_PAN_STEP           0x100
#define J_PAN_ARGS           11     /* the stepped form's payload words */
#define J_ANIM_FRAME_US      33333  /* SLIDE's and the linear PAN's frame
                                       interval on the BCM (30 fps) */

/* Events (BCM -> PP, tag = the job's) */
#define J_EV_ANIM            0x68   /* frames shown, frames late */
#define J_EV_EPOCH           0x69   /* new epoch: slots are gone */

/* Errors: hostproto.h's HP_E_* (HP_E_INVAL, HP_E_BUSY, HP_E_NOMEM,
 * HP_E_UNKNOWN ...), and these */
#define J_E_NOSLOT           20     /* the class's pool is full */
#define J_E_SLOT             21     /* not a slot, not READY, or of an
                                       older epoch */
#define J_E_RANGE            22     /* a rect outside its surface */
#define J_E_SUM              23     /* COMMIT's F16w differs */
#define J_E_MODE             24     /* the pool belongs to VIDEO or GAME */
#define J_E_FORMAT           25
#define J_E_DEC_UNSUPPORTED  26     /* arithmetic, 12-bit, odd subsampling,
                                       a progressive store past
                                       JDEC_PROG_MAX, or progressive in an
                                       image built without the store (the
                                       display-only image): the PP's twin
                                       decides (sticky only for the first
                                       three) */
#define J_E_DEC_CORRUPT      27     /* no SOI, bad markers, decoder error */
#define J_E_DEC_TRUNCATED    28     /* the stream ends before its EOI */

/* Host-side deadlines, ms (jobs.md section 5) */
#define J_T_SMALL            5      /* ALLOC, FREE, STATS, ANIM (COMMIT:
                                       J_T_COMMIT, below) */
#define J_T_DRAW             8      /* one BLIT/FILL/FADE/AVG/SCROLL, full
                                       screen at 27 MHz */
#define J_T_PRESENT          40     /* a full frame plus two TE periods */
#define J_T_FENCE_BASE       20     /* plus the jobs it retires */
#define J_T_DECODE           2000
#define J_T_SPECTRUM         8      /* the C reference at 27 MHz: 4 ms for
                                       512 samples [sim]; the kernel 0.1 ms */
#define J_T_BARS             J_T_DRAW
#define J_T_OVERLAY          80     /* the load: its F16w and the overlay's
                                       start-up checks, 0.75 M instructions
                                       for the visualizer's, 28 ms at
                                       27 MHz [sim] */
/* COMMIT reads the whole slot for its F16w: 11.7 ms at 27 MHz for a
 * 300x300 cover [sim, jobs.md section 10's audit: 3.5 instructions a
 * pixel; FNV-1a took 40 ms].  Its deadline: J_T_SMALL plus 1 ms per
 * started 2 KB of the slot's bytes, set for FNV-1a and kept (a cover:
 * 5 + 89 = 94 ms) */
#define J_T_COMMIT(bytes)    (J_T_SMALL + ((uint32_t)(bytes) + 2047) / 2048)
/* Rev 0.5 (candidate): a staged ALLOC may first bring the BCM's SDRAM up
 * (4 attempts of at most 5 ms, video.c sdram_init), so the PP allows it
 * this much more; a staged COMMIT also moves the window (a cover: 6.4 ms
 * by DMA, 11.5 ms by the core [silicon PW8 rates]), inside J_T_COMMIT's
 * FNV-era slack (94 ms for a cover against 11.7 + 11.5) */
#define J_T_STAGE_UP         25

/* ---- COMMIT's sum: F16w --------------------------------------------- */
/* F16w (docs/research/bcm2722/algorithms.md section 7).  The input: the
 * slot's w x h pixels as 16-bit values h_0 ... h_N-1 (N = w x h; the
 * RGB565 pixel itself, which is the little-endian halfword in memory),
 * row by row from row 0, each row from x = 0.  The padding of a row
 * whose pitch is wider than w (J_CLS_DRIFT's) is not part of it.
 *   A = sum of h_i, mod 2^32;
 *   B = sum of (N - i) * h_i, mod 2^32: the running sum of A after each
 *       pixel.
 * All 1- and 2-pixel errors change A or B for any slot below 2^17
 * pixels, and 0x0000 and 0xFFFF differ (Fletcher-32's ones' complement
 * makes them equal).  It runs incrementally: start a = b = 0 and feed the
 * pixels in order, in pieces of any size (a row at a time, a row while
 * it is written).  An empty or all-black slot sums to 0, 0, and COMMIT
 * still checks it: "no check" is only the explicit J_COMMIT_NOCHECK flag,
 * so a torn upload of a black cover is caught like any other. */
static inline void j_f16w(uint32_t *a, uint32_t *b, const uint16_t *px,
                          uint32_t n)
{
    uint32_t sa = *a, sb = *b;

    /* eight at a time: 3.5 instructions a pixel on the BCM [sim], half
     * the plain loop's 7 (its counter and compare dominate) */
    for (; n >= 8; n -= 8, px += 8)
    {
        sa += px[0]; sb += sa;
        sa += px[1]; sb += sa;
        sa += px[2]; sb += sa;
        sa += px[3]; sb += sa;
        sa += px[4]; sb += sa;
        sa += px[5]; sb += sa;
        sa += px[6]; sb += sa;
        sa += px[7]; sb += sa;
    }
    while (n--)
    {
        sa += *px++;
        sb += sa;
    }
    *a = sa;
    *b = sb;
}

/* Test vectors (define J_F16W_VECTORS before including this): jtest.c
 * checks jref's COMMIT and j_f16w() against them; the PP driver's host
 * test and bcmfw/tests' Python f16w() check the same table. */
#ifdef J_F16W_VECTORS
struct j_f16w_vec
{
    const char *name;
    uint32_t n;
    uint16_t px[2];
    uint32_t a, b;
};

static const struct j_f16w_vec j_f16w_vec[] =
{
    { "empty",        0, { 0 },                0x00000000, 0x00000000 },
    { "one",          1, { 0x1234 },           0x00001234, 0x00001234 },
    { "0x0000",       1, { 0x0000 },           0x00000000, 0x00000000 },
    { "0xFFFF",       1, { 0xFFFF },           0x0000FFFF, 0x0000FFFF },
    { "pair",         2, { 0x1234, 0xABCD },   0x0000BE01, 0x0000D035 },
    { "pair swapped", 2, { 0xABCD, 0x1234 },   0x0000BE01, 0x000169CE },
};
#endif

/* The vector slots, J_CLS_DRIFT 300 x 300 (pitch J_DRIFT_PITCH; each
 * row's 4 padding pixels 0xDEAD, which are not summed):
 *   SLOT: pixel k (k = 0 ... 89999 in row order) = s_k+1 >> 16, where
 *         s_0 = J_F16W_SEED and s_k+1 = s_k * 1103515245 + 12345 mod 2^32;
 *   WHITE: every pixel 0xFFFF (both sums wrap). */
#define J_F16W_SEED          0x0F16C0DE
#define J_F16W_SLOT_A        0xAFF97700
#define J_F16W_SLOT_B        0x5949280B
#define J_F16W_WHITE_A       0x5F8EA070
#define J_F16W_WHITE_B       0xD6E137B8

#endif /* BCMFW_JOBS_H */
