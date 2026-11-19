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
 * The codec EQ's band model (pure): the WM8758's 5 bands, their frequency
 * choices, an approximate response curve, the precut and the saved form.
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
#ifndef IPODUI_EQLAB_CORE_H
#define IPODUI_EQLAB_CORE_H

#include <stdbool.h>
#include <stdint.h>
#include "dac_ext.h"

/* The developer EQ Lab's model, written so 0.9.0's custom EQ (#82: the
 * chip's 5 bands, 3 slots) can take it as it is. A band is dac_ext.h's
 * struct dac_eq_band: gain -12..12 dB, freq 0..3, wide 0/1 (bands 1..3).
 *
 * Frequencies (Hz, nominal at a 48 kHz codec rate): the WM8978 datasheet
 * table for the same EQ block (firmware/export/wm8978.h WMC_EQnC_*), also
 * tools/eqfit hw_hz[] and docs/wm8758-reference/README.md. The codec runs
 * its filters at the output rate with the 48 kHz coefficients (wm8758.c
 * audiohw_set_frequency(): ADDCTRL SR 48 kHz for 44.1, 88.2 and 96 kHz),
 * so the real corner is nominal * fs / 48000 (eqfit assumption 2). */
#define EQLAB_BANDS     DAC_EQ_BANDS
#define EQLAB_FREQS     4
#define EQLAB_GAIN_MAX  12
extern const unsigned short eqlab_hz[EQLAB_BANDS][EQLAB_FREQS];

/* Band boundaries for the screen (Hz): the rounded geometric mean of the
 * neighbouring bands' outer choices (175|230, 500|650, 1400|1800,
 * 4100|5300), so every band's four choices sit inside its own column */
extern const unsigned short eqlab_edge_hz[EQLAB_BANDS - 1];

/* The register defaults (wm8758.h: EQ1 0x12c, EQ2..5 0x02c): freq 1,
 * narrow, 0 dB */
#define EQLAB_FREQ_DEF  1

static inline bool eqlab_has_width(int band)
{
    return band > 0 && band < EQLAB_BANDS - 1;
}

/* log2(v) in Q8 (v >= 1), integer only */
int eqlab_log2_q8(unsigned long v);

/* The 5 bands' combined response at frequency 2^(lf_q8 / 256) Hz, in dB
 * Q8, with every corner shifted by shift_q8 (log2(fs / 48000) in Q8, 0 for
 * the nominal curve). APPROXIMATE: each band is a tabulated shape (RBJ
 * shelf Q 0.707 and peaks Q 1.41 narrow / 0.67 wide, eqfit's assumption
 * 3, sampled at +12 dB every quarter octave and scaled linearly by the
 * gain), summed in dB. Good to about 1 dB for a preview, not a fit. */
int eqlab_db_q8(const struct dac_eq_band *b, int lf_q8, int shift_q8);

/* log2(20 Hz) and log2(20 kHz) in Q8: the screen's frequency axis */
#define EQLAB_LF_MIN    1106
#define EQLAB_LF_MAX    3658

/* The precut for a curve: its highest point over 20 Hz..20 kHz (nominal
 * corners, so it does not move with the rate), rounded up to whole dB,
 * 0..12 (dac_eq_set()'s range) */
int eqlab_precut(const struct dac_eq_band *b);

/* #203: the level plan for one curve (presets and custom slots alike, on
 * every curve change, never per sample).
 *  - precut_db: eqlab_precut(), the summed response's peak, whole dB
 *    0..12, applied as DAC digital attenuation so a boost can't clip in
 *    the codec;
 *  - loud_db10: the curve's loudness change in tenth dB, pink noise
 *    through the BS.1770 K-weighting (a power average on the model's
 *    1/8-octave grid, 20 Hz..20 kHz), so +60 is "6 dB louder";
 *  - makeup_db10 = precut*10 - loud, clamped to 0..precut*10: the
 *    analogue gain that gives the loudness back. Never more than the
 *    precut, so the EQ never plays louder than EQ off at the same volume;
 *    a curve that only cuts stays as much quieter as it cuts (owner
 *    question in the hand-back). Fixed point, integer only. */
struct eqlab_level {
    int precut_db;
    int loud_db10;
    int makeup_db10;
};
void eqlab_level(const struct dac_eq_band *b, struct eqlab_level *lv);

/* #82: a custom slot as one line of text, the storage and the export
 * format alike (a straight dump): "eq1" then per band "<gain>@<Hz>",
 * gain signed whole dB, Hz the nominal datasheet value (eqlab_hz), "w"
 * after it for a wide peak, space-separated:
 *   "eq1 +4@105 -2@300w +0@850 +3@2400 -1@6900"
 * The empty string is a flat slot. format returns the length (it fits in
 * EQLAB_TEXT_LEN); parse refuses (false, *b untouched) anything else. */
#define EQLAB_TEXT_LEN  64
int eqlab_format(const struct dac_eq_band *b, char *buf, int len);
bool eqlab_parse(const char *s, struct dac_eq_band *b);
/* a flat curve at the register defaults (freq EQLAB_FREQ_DEF, narrow) */
void eqlab_flat(struct dac_eq_band *b);

/* "80 Hz", "2.4 kHz", "11.7 kHz" (sep: the decimal mark) */
void eqlab_fmt_hz(char *buf, int len, unsigned hz, const char *sep);
/* the axis labels, Hz implied: "80", "1.1k", "9k", "11.7k" */
void eqlab_fmt_hz_short(char *buf, int len, unsigned hz,
                        const char *sep);

/* The saved form (EQLAB_PACKED bytes): a magic, the 5 bands, a check
 * byte. unpack refuses (false, *b untouched) anything out of range */
#define EQLAB_PACKED    20
void eqlab_pack(const struct dac_eq_band *b, uint8_t *out);
bool eqlab_unpack(const uint8_t *in, int len, struct dac_eq_band *b);

#endif /* IPODUI_EQLAB_CORE_H */
