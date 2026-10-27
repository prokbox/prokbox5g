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

/* The SDK's pure rules (docs/design/sdk.md rev 0), host-tested in
 * tests/sdk: the exit chord, the dirty-band merge, the .state file, the
 * slice judge, the kill/hide count and the rate limits. No Rockbox
 * headers. */
#ifndef IPODUI_SDK_CORE_H
#define IPODUI_SDK_CORE_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

/* -- the exit chord (section 3.1) ----------------------------------------
 * Fed the two buttons' state at every button event and every frame. The
 * moment SELECT and PREV are both down, both are withheld until both are
 * up again; held SDK_EXIT_CHORD_MS, the exit runs (once). */
struct sdk_chord {
    uint8_t  state;             /* SDK_CH_* below */
    uint32_t t0;                /* ms when both went down */
};
enum { SDK_CH_IDLE, SDK_CH_HELD, SDK_CH_SWALLOW };
#define SDK_CHR_ACTIVE  0x1     /* withhold SELECT and PREV events now */
#define SDK_CHR_BEGIN   0x2     /* the overlap just began: cancel a
                                   delivered press */
#define SDK_CHR_EXIT    0x4     /* held long enough: run the exit */
int sdk_chord_feed(struct sdk_chord *ch, bool sel, bool prev,
                   uint32_t now_ms, uint32_t hold_ms);
/* ms until the chord fires (for the loop's next wake), or -1 */
int sdk_chord_due(const struct sdk_chord *ch, uint32_t now_ms,
                  uint32_t hold_ms);

/* -- dirty bands (section 5.2) -------------------------------------------
 * Marks [y, y+h) merge into at most SDK_BANDS_MAX full-width bands:
 * overlapping or touching marks join; with every slot taken, the two
 * closest bands join (never more pixels than the screen). */
#define SDK_BANDS_MAX 4
struct sdk_bands { uint8_t n; int16_t y[SDK_BANDS_MAX], h[SDK_BANDS_MAX]; };
void sdk_bands_add(struct sdk_bands *b, int y, int h, int top, int bottom);
int  sdk_bands_rows(const struct sdk_bands *b);

/* -- the .state file (sections 8.2, 9.3) ---------------------------------
 * 16-byte header, the settings bytes, then the game's record. */
#define SDK_STATE_MAGIC   0x53474b50u   /* "PKGS" */
#define SDK_STATE_LAYOUT  1
#define SDK_STATE_HDR     16
struct sdk_state_hdr {
    uint32_t magic;
    uint8_t  layout, set_ver, set_n, rsv;
    uint16_t rec_len, rsv2;
    uint32_t crc;               /* CRC-32 over settings + record */
};
_Static_assert(sizeof(struct sdk_state_hdr) == SDK_STATE_HDR, "state hdr");
/* In RAM the game's record starts here, past the file image's header and
 * settings, rounded up to 8: a game casts it to its own struct, and the
 * ARM7TDMI aborts a misaligned halfword or word access (the P12 device
 * bench, 2026-10-24: Chute's strh at state_img + 17). The file layout is
 * unchanged; sdk_state_unpack() moves the record up (memmove). */
#define SDK_REC_ALIGN     8
#define SDK_REC_OFF(set_n) \
    ((SDK_STATE_HDR + (unsigned)(set_n) + SDK_REC_ALIGN - 1) \
     & ~(unsigned)(SDK_REC_ALIGN - 1))
uint32_t sdk_crc32(const void *p, size_t n, uint32_t crc);
/* Writes the file image into out (cap bytes); returns its length or -1 */
int sdk_state_pack(uint8_t *out, size_t cap, uint8_t set_ver,
                   const uint8_t *set, int set_n, const void *rec,
                   uint16_t rec_len);
/* Reads a file image of len bytes. Settings: copied into set (set_n
 * values, limits[i] = the value count of setting i); a stored version
 * other than set_ver or a value out of range resets all of them to defs
 * (returns SDK_SU_SET_RESET in the flags). The record: copied into rec
 * (up to rec_cap), *rec_len its length, 0 if none or the CRC failed.
 * Returns flags. */
#define SDK_SU_OK         0x0
#define SDK_SU_NONE       0x1   /* no file or a bad header: fresh */
#define SDK_SU_CRC        0x2   /* the CRC failed: fresh */
#define SDK_SU_SET_RESET  0x4   /* the settings went back to defaults */
int sdk_state_unpack(const uint8_t *in, size_t len, uint8_t set_ver,
                     uint8_t *set, int set_n, const uint8_t *limits,
                     const uint8_t *defs, void *rec, uint16_t rec_cap,
                     uint16_t *rec_len);

/* -- slices (section 6.2) ------------------------------------------------ */
enum sdk_judge { SDK_J_OK, SDK_J_VIOL, SDK_J_KILL };
/* used: game time of one call; budget: its slice; *viols: violations so
 * far this session (counted here). Over budget once: VIOL; again, or
 * over 10 x: KILL. */
int sdk_slice_judge(uint32_t used_us, uint32_t budget_us, uint8_t *viols);

/* -- the firmware's games file (section 8.5) -----------------------------
 * One record per module id: its play count and its kills in a row,
 * keyed on the module's version (a firmware or module update resets the
 * kills). */
#define SDK_KILLS_HIDE 3
struct sdk_gamerec {
    uint32_t key;               /* the module's version key */
    uint16_t plays;
    uint8_t  kills;             /* kills in a row */
    uint8_t  rsv;
};
void sdk_rec_sync(struct sdk_gamerec *r, uint32_t key);
void sdk_rec_launch(struct sdk_gamerec *r);
void sdk_rec_end(struct sdk_gamerec *r, bool killed);
bool sdk_rec_hidden(const struct sdk_gamerec *r, uint32_t key);

/* -- rate limits: at most per events in any window_ms ------------------- */
struct sdk_rate { uint32_t t0; uint8_t n; };
bool sdk_rate_take(struct sdk_rate *r, uint32_t now_ms, uint32_t window_ms,
                   uint8_t per);

/* -- the absolute wheel, polled (section 3.3) -----------------------------
 * Class A reads the finger's position with sdk_wheel_pos() (owner,
 * 2026-10-24, #132: Q30's default, a polled read-only position; the
 * event stream stays relative). The driver posts nothing when a finger
 * lands without turning, so for a game that reads it the SDK polls the
 * touch count and the position while the game is idle: every
 * SDK_WW_LIT_MS while lit, every SDK_WW_DARK_MS while dark (the driver
 * lights the screen on a touch; nothing tells the loop). sdk_ww_poll()
 * says when to step the game: a finger on the ring, a finger just lifted
 * or set down and lifted between two polls, or the screen lit again. */
#define SDK_WW_LIT_MS   50
#define SDK_WW_DARK_MS  500
#define SDK_WHEEL_POSITIONS 96
struct sdk_wwatch {
    uint32_t touches;           /* the driver's touch count, last poll */
    uint8_t  touched;           /* a finger was on the ring */
    uint8_t  dark;              /* the screen was dark */
    uint8_t  rsv[2];
};
#define SDK_WW_WAKE     0x1     /* step the game now */
#define SDK_WW_LIT      0x2     /* the screen lit again since the last poll */
int sdk_ww_poll(struct sdk_wwatch *w, bool lit, int pos, uint32_t touches);
/* the driver's raw value as the SDK reports it: 0..95, else -1 */
int sdk_wheel_norm(int raw);

#endif /* IPODUI_SDK_CORE_H */
