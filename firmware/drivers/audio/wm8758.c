/***************************************************************************
 *             __________               __   ___.
 *   Open      \______   \ ____   ____ |  | _\_ |__   _______  ___
 *   Source     |       _//  _ \_/ ___\|  |/ /| __ \ /  _ \  \/  /
 *   Jukebox    |    |   (  <_> )  \___|    < | \_\ (  <_> > <  <
 *   Firmware   |____|_  /\____/ \___  >__|_ \|___  /\____/__/\_ \
 *                     \/            \/     \/    \/            \/
 *
 * Driver for WM8758 audio codec - based on datasheet for WM8983
 *
 * Based on code from the ipodlinux project - http://ipodlinux.org/
 * Adapted for Rockbox in December 2005
 *
 * Original file: linux/arch/armnommu/mach-ipod/audio.c
 *
 * Copyright (c) 2003-2005 Bernard Leach (leachbj@bouncycastle.org)
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

#include "system.h"
#include "kernel.h"
#include "string.h"
#include "audio.h"

#include "wmcodec.h"
#include "audiohw.h"
#include "sound.h"

#ifdef HAVE_IPOD_UI
#include "xcore.h"                     /* ProkBox P19 S0: XCORE_BSS */
/* ProkBox P19 (ucop-fix, 2026-10-26): every shadow and every word of the
 * standby state machine is uncached. Both cores drive this driver under
 * dac_mtx (the volume and the EQ from the UI, which may run on the COP;
 * dac_wake() before each PCM start and the idle polls from the audio
 * thread and the UI): a mutex orders the cores but does not make their
 * caches agree, so a cached shadow read on one core could be the other's
 * stale copy (a full standby wake replayed over live audio). These words
 * change a few times a second at most. */
#define WM_XBSS  XCORE_BSS
#define WM_XDATA XCORE_DATA
#else
#define WM_XBSS
#define WM_XDATA
#endif

/* shadow registers */
static unsigned short eq1_reg WM_XDATA = EQ1_EQ3DMODE | EQ_GAIN_VALUE(0);
static unsigned short eq5_reg WM_XDATA = EQ_GAIN_VALUE(0);

#ifdef HAVE_IPOD_UI
/* ProkBox (docs/phase15-brief.md sections 2.1, 3.1, 3.2): the 5-band EQ
 * driven directly (dac_ext.h), DAC standby while PCM is stopped, and a
 * register-write ring for the S1 spike. Every write goes through
 * wm_write(), which keeps the shadows, applies the standby mask and logs
 * the write, under one recursive codec mutex (nests over i2c_mtx). Never
 * called from an IRQ, FIQ or tick task: I2C sleeps. */
#include "pcm.h"
#include "pcm-internal.h"              /* pcm_is_playing() */
#include "dac_ext.h"
#include "dac_gain.h"

static struct mutex dac_mtx XCORE_BSS;  /* P19 S0: volume from either core */
static struct dac_stats dac_state WM_XBSS;
static unsigned short pwr_reg[3] WM_XBSS;       /* PWRMGMT1..3 as last requested */
static unsigned short dacctrl_reg WM_XDATA = DACCTRL_SOFTMUTE;
static unsigned short addctrl_reg WM_XBSS;      /* without M128ENB */
/* EQ1..EQ5 as written; 0xffff: unknown, so the first dac_eq_set() writes
 * all five */
static unsigned short eq_reg[DAC_EQ_BANDS] WM_XDATA =
    { 0xffff, 0xffff, 0xffff, 0xffff, 0xffff };
static int dac_precut WM_XBSS;                  /* whole dB, 0..12 */
static int dac_vol_l WM_XBSS, dac_vol_r WM_XBSS;        /* LDACVOL/RDACVOL before precut */
/* hardware Sound Check: the volume as requested (tenth dB) and the trim */
static int dac_req_l WM_XDATA = DAC_GAIN_MUTE, dac_req_r WM_XDATA = DAC_GAIN_MUTE;
static int dac_gain_ceil WM_XDATA = DAC_GAIN_VOL_MAX;

/* S1: the last DAC_REGLOG_LEN writes, (reg << 9) | value */
static uint16_t dac_reglog[DAC_REGLOG_LEN] WM_XBSS;
static unsigned int dac_reglog_seq WM_XBSS;

#ifdef HAVE_DAC_STANDBY
static long dac_busy_tick WM_XBSS;              /* last time PCM was seen running or
                                         * about to start */
#define DAC_IDLE_TICKS      (DAC_IDLE_MS * HZ / 1000)
/* PLL settle before the DACs come back. No datasheet lock time was at
 * hand (see dac_standby_wake()); last_wake_us reports the real cost. */
#define DAC_PLL_SETTLE_US   2000

/* What standby powers down, per register: the PLL (R1), the OUT1
 * headphone amps (R2), the DACs, mixers and OUT2 (R3); DACCTRL stays soft
 * muted. VMID (500 k), BIASEN and BUFIOEN stay on. */
static unsigned short standby_mask(int reg, unsigned short val)
{
    switch (reg)
    {
    case PWRMGMT1:
        return val & ~PWRMGMT1_PLLEN;
    case PWRMGMT2:
        return val & ~(PWRMGMT2_LOUT1EN | PWRMGMT2_ROUT1EN);
    case PWRMGMT3:
        return val & ~(PWRMGMT3_DACENL | PWRMGMT3_DACENR
                       | PWRMGMT3_LMIXEN | PWRMGMT3_RMIXEN
                       | PWRMGMT3_LOUT2EN | PWRMGMT3_ROUT2EN);
    case DACCTRL:
        return val | DACCTRL_SOFTMUTE;
    default:
        return val;
    }
}
#endif /* HAVE_DAC_STANDBY */

/* One write to the chip, logged; caller holds dac_mtx */
static void wm_write_raw(int reg, int val)
{
    dac_reglog[dac_reglog_seq++ % DAC_REGLOG_LEN] = (reg << 9) | (val & 0x1ff);
    wmcodec_write(reg, val);
}

static void wm_write(int reg, int val)
{
    mutex_lock(&dac_mtx);
    if (reg >= PWRMGMT1 && reg <= PWRMGMT3)
        pwr_reg[reg - PWRMGMT1] = val;
    else if (reg == DACCTRL)
        dacctrl_reg = val;
#ifdef HAVE_DAC_STANDBY
    if (dac_state.standby)
        val = standby_mask(reg, val);   /* applied in full on wake */
#endif
    wm_write_raw(reg, val);
    mutex_unlock(&dac_mtx);
}

/* LDACVOL/RDACVOL with the EQ precut as extra DAC attenuation (0.5 dB
 * register steps; 0 stays digital mute, nothing goes below 1) */
static int dac_precut_vol(int dac)
{
    if (dac == 0)
        return 0;
    dac -= 2 * dac_precut;
    return dac < 1 ? 1 : dac;
}

static void dac_write_vol(void)
{
    wm_write(LDACVOL, dac_precut_vol(dac_vol_l));
    wm_write(RDACVOL, dac_precut_vol(dac_vol_r) | RDACVOL_DACVU);
}
#else
#define wm_write wmcodec_write
#endif /* HAVE_IPOD_UI */

/* convert tenth of dB volume (-89..6) to master volume register value */
static int vol_tenthdb2hw(int db)
{
    /*   att  DAC  AMP  result
        +6dB    0   +6     96
         0dB    0    0     90
       -57dB    0  -57     33
       -58dB   -1  -57     32
       -89dB  -32  -57      1
       -90dB  -oo  -oo      0 */
    if (db <= -900) {
        return 0;
    } else {
        return db / 10 - -90;
    }
}

/* helper function that calculates the register setting for amplifier and
   DAC volume out of the input from tenthdb2master() */
static void get_volume_params(int db, int *dac, int *amp)
{
    /* should never happen, set max volume for amp and dac */
    if      (db > 96) {
        *dac = 255;
        *amp = 63;
    }
    /* set dac to max and set volume for amp (better snr) */
    else if (db > 32) {
        *dac = 255;
        *amp = (db-90)+57;
    }
    /* set amp to min and reduce dac output */
    else if (db >  0) {
        *dac = (db-33)*2 + 255;
        *amp = 0;
    }
    /* mute all */
    else {
        *dac = 0x00;
        *amp = 0x40;
    }
}

static void audiohw_mute(bool mute)
{
    if (mute) {
        wm_write(DACCTRL, DACCTRL_SOFTMUTE);
    } else {
        wm_write(DACCTRL, DACCTRL_DACOSR128);
    }
}

void audiohw_preinit(void)
{
#ifdef HAVE_IPOD_UI
    mutex_init(&dac_mtx);               /* pcm_init(): thread context */
#endif
    /* Set low bias mode */
    wm_write(BIASCTRL, BIASCTRL_BIASCUT);
    /* Enable HPCOM, LINECOM */
    wm_write(OUTCTRL, OUTCTRL_HP_COM | OUTCTRL_LINE_COM
                         | OUTCTRL_TSOPCTRL | OUTCTRL_TSDEN | OUTCTRL_VROI);
    /* Mute all Outputs and set PGAs minimum gain */
    wm_write(LOUT1VOL, 0x140);
    wm_write(ROUT1VOL, 0x140);
    wm_write(LOUT2VOL, 0x140);
    wm_write(ROUT2VOL, 0x140);
    wm_write(OUT3MIX,  0x40);
    wm_write(OUT4MIX,  0x40);
    /* Enable L/ROUT1 */
    wm_write(PWRMGMT2, PWRMGMT2_ROUT1EN | PWRMGMT2_LOUT1EN);
    /* Enable VMID independent current bias */
    wm_write(OUT4TOADC, OUT4TOADC_POBCTRL);
    /* Enable required DACs and mixers */
    wm_write(PWRMGMT3, PWRMGMT3_RMIXEN | PWRMGMT3_LMIXEN
                          | PWRMGMT3_DACENR | PWRMGMT3_DACENL);
    /* Enable VMIDSEL, BIASEN, BUFIOEN */
    wm_write(PWRMGMT1, PWRMGMT1_PLLEN | PWRMGMT1_BIASEN
                          | PWRMGMT1_BUFIOEN | PWRMGMT1_VMIDSEL_10K);
    /* Setup digital interface, input amplifiers, PLL, ADCs and DACs */
    wm_write(AINTFCE, AINTFCE_IWL_16BIT | AINTFCE_FORMAT_I2S);
    wm_write(CLKCTRL, CLKCTRL_MS); /* WM8758 is clock master */

    audiohw_set_frequency(HW_FREQ_44);

    wm_write(LOUTMIX, LOUTMIX_DACL2LMIX);
    wm_write(ROUTMIX, ROUTMIX_DACR2RMIX);
    /* Disable VMID independent current bias */
    wm_write(OUT4TOADC, 0);
}

void audiohw_postinit(void)
{
    wm_write(PWRMGMT1, PWRMGMT1_PLLEN | PWRMGMT1_BIASEN
                          | PWRMGMT1_BUFIOEN | PWRMGMT1_VMIDSEL_500K);
                          /* lower the VMID power consumption */
    wm_write(BIASCTRL, 0);
    audiohw_mute(false);
#ifdef HAVE_DAC_STANDBY
    dac_busy_tick = current_tick;       /* first standby DAC_IDLE_MS on */
#endif
}

#ifdef HAVE_IPOD_UI
/* The volume plus the Sound Check trim (dac_gain.h), the DAC part with the
 * hardware EQ's precut; caller holds dac_mtx */
static void dac_apply_volume(void)
{
    int amp_l, amp_r;

    dac_state.gain_applied_db10 =
        dac_gain_split(dac_req_l, dac_state.gain_db10, dac_gain_ceil,
                       &dac_vol_l, &amp_l);
    dac_gain_split(dac_req_r, dac_state.gain_db10, dac_gain_ceil,
                   &dac_vol_r, &amp_r);
    dac_write_vol();
    wm_write(LOUT1VOL, amp_l | LOUT1VOL_LOUT1ZC);
    wm_write(ROUT1VOL, amp_r | ROUT1VOL_ROUT1ZC | ROUT1VOL_OUT1VU);
}

void audiohw_set_volume(int vol_l, int vol_r)
{
    mutex_lock(&dac_mtx);
    dac_req_l = vol_l;
    dac_req_r = vol_r;
    dac_apply_volume();
    mutex_unlock(&dac_mtx);
}

void dac_set_gain_db10(int db10, int ceil_db10)
{
    mutex_lock(&dac_mtx);
    if (db10 != dac_state.gain_db10 || ceil_db10 != dac_gain_ceil)
    {
        dac_state.gain_db10 = db10;
        dac_gain_ceil = ceil_db10;
        dac_apply_volume();
    }
    mutex_unlock(&dac_mtx);
}
#else
void audiohw_set_volume(int vol_l, int vol_r)
{
    int dac_l, amp_l, dac_r, amp_r;

    vol_l = vol_tenthdb2hw(vol_l);
    vol_r = vol_tenthdb2hw(vol_r);

    get_volume_params(vol_l, &dac_l, &amp_l);
    get_volume_params(vol_r, &dac_r, &amp_r);

    /* set DAC
       Important: DAC is global and will also affect lineout */
    wm_write(LDACVOL, dac_l);
    wm_write(RDACVOL, dac_r | RDACVOL_DACVU);

    /* set headphone amp OUT1 */
    wm_write(LOUT1VOL, amp_l | LOUT1VOL_LOUT1ZC);
    wm_write(ROUT1VOL, amp_r | ROUT1VOL_ROUT1ZC | ROUT1VOL_OUT1VU);
}
#endif /* HAVE_IPOD_UI */

void audiohw_set_lineout_volume(int vol_l, int vol_r)
{
    int dac_l, amp_l, dac_r, amp_r;

    vol_l = vol_tenthdb2hw(vol_l);
    vol_r = vol_tenthdb2hw(vol_r);

    get_volume_params(vol_l, &dac_l, &amp_l);
    get_volume_params(vol_r, &dac_r, &amp_r);

    /* set lineout amp OUT2 */
    wm_write(LOUT2VOL, amp_l | LOUT2VOL_LOUT2ZC);
    wm_write(ROUT2VOL, amp_r | ROUT2VOL_ROUT2ZC | ROUT2VOL_OUT2VU);
}

void audiohw_enable_lineout(bool enable)
{
    /* Initialize data without lineout enabling. */
    int pwrmgmt3_data  = PWRMGMT3_RMIXEN  | PWRMGMT3_LMIXEN
                       | PWRMGMT3_DACENR  | PWRMGMT3_DACENL;
    /* Set lineout (OUT2), if enabled. */
    if (enable)
        pwrmgmt3_data |= PWRMGMT3_LOUT2EN | PWRMGMT3_ROUT2EN;

    /* Set register. */
    wm_write(PWRMGMT3, pwrmgmt3_data);
}

#ifdef HAVE_IPOD_UI
/* Bass and treble own EQ1/EQ5 only while the hardware EQ is off; while it
 * is on they update their shadows, which dac_eq_set(NULL, 0) restores */
static void eq_shelf_write(int band, unsigned short val)
{
    mutex_lock(&dac_mtx);
    if (!dac_state.hw_eq && eq_reg[band] != val)
    {
        eq_reg[band] = val;
        wm_write(EQ1 + band, val);
    }
    mutex_unlock(&dac_mtx);
}
#define EQ1_WRITE() eq_shelf_write(0, eq1_reg)
#define EQ5_WRITE() eq_shelf_write(4, eq5_reg)
#else
#define EQ1_WRITE() wm_write(EQ1, eq1_reg)
#define EQ5_WRITE() wm_write(EQ5, eq5_reg)
#endif

void audiohw_set_bass(int value)
{
    eq1_reg = (eq1_reg & ~EQ_GAIN_MASK) | EQ_GAIN_VALUE(value);
    EQ1_WRITE();
}

void audiohw_set_bass_cutoff(int value)
{
    eq1_reg = (eq1_reg & ~EQ_CUTOFF_MASK) | EQ_CUTOFF_VALUE(value);
    EQ1_WRITE();
}

void audiohw_set_treble(int value)
{
    eq5_reg = (eq5_reg & ~EQ_GAIN_MASK) | EQ_GAIN_VALUE(value);
    EQ5_WRITE();
}

void audiohw_set_treble_cutoff(int value)
{
    eq5_reg = (eq5_reg & ~EQ_CUTOFF_MASK) | EQ_CUTOFF_VALUE(value);
    EQ5_WRITE();
}

/* Nice shutdown of WM8758 codec */
void audiohw_close(void)
{
    /* ProkBox: from DAC standby the same order runs; wm_write()'s standby
     * mask keeps the PLL, DACs, mixers and outputs off, so the steps
     * standby already did are skipped and the PLL does not come back on
     * for the VMID discharge (outputs already tied to VMID, OUTCTRL_VROI) */
    audiohw_mute(true);

    /* Disable Thermal shutdown */
    wm_write(OUTCTRL, OUTCTRL_HP_COM | OUTCTRL_VROI);
    /* Enable VMIDTOG */
    wm_write(OUT4TOADC, OUT4TOADC_VMIDTOG);
    /* Disable VMIDSEL and BUFIOEN */
    wm_write(PWRMGMT1, PWRMGMT1_PLLEN | PWRMGMT1_BIASEN
                          | PWRMGMT1_VMIDSEL_OFF);
    /* Wait for VMID to discharge */
    sleep(3*HZ/10);
    /* Power off registers */
    wm_write(PWRMGMT2, 0);
    wm_write(PWRMGMT3, 0);
    wm_write(PWRMGMT1, 0);
}

/* Note: Disable output before calling this function */
void audiohw_set_frequency(int fsel)
{
    /* CLKCTRL_MCLKDIV_MASK and ADDCTRL_SR_MASK don't overlap,
       so they can both fit in one byte.  Bit 0 selects PLL
       configuration via pll_setups.
     */
    static const unsigned char freq_setups[HW_NUM_FREQ] =
    {
        [HW_FREQ_48] = CLKCTRL_MCLKDIV_2 | ADDCTRL_SR_48kHz | 1,
        [HW_FREQ_44] = CLKCTRL_MCLKDIV_2 | ADDCTRL_SR_48kHz,
        [HW_FREQ_32] = CLKCTRL_MCLKDIV_3 | ADDCTRL_SR_32kHz | 1,
        [HW_FREQ_24] = CLKCTRL_MCLKDIV_4 | ADDCTRL_SR_24kHz | 1,
        [HW_FREQ_22] = CLKCTRL_MCLKDIV_4 | ADDCTRL_SR_24kHz,
        [HW_FREQ_16] = CLKCTRL_MCLKDIV_6 | ADDCTRL_SR_16kHz | 1,
        [HW_FREQ_12] = CLKCTRL_MCLKDIV_8 | ADDCTRL_SR_12kHz | 1,
        [HW_FREQ_11] = CLKCTRL_MCLKDIV_8 | ADDCTRL_SR_12kHz,
        [HW_FREQ_8] = CLKCTRL_MCLKDIV_12 | ADDCTRL_SR_8kHz | 1
    };

    /* Each PLL configuration is an array consisting of
       { PLLN, PLLK1, PLLK2, PLLK3 }.  The WM8983 datasheet requires
       5 < PLLN < 13, and states optimum is PLLN = 8, f2 = 90 MHz
     */
    static const unsigned short pll_setups[2][4] =
    {
        /* f1 = 12 MHz, R = 7.5264, f2 = 90.3168 MHz, fPLLOUT = 22.5792 MHz */
        { PLLN_PLLPRESCALE | 0x7, 0x21, 0x161, 0x26 },
        /* f1 = 12 MHz, R = 8.192, f2 = 98.304 MHz, fPLLOUT = 24.576 MHz */
        { PLLN_PLLPRESCALE | 0x8, 0xC, 0x93, 0xE9 }
    };

    int i;

    /* PLLN, PLLK1, PLLK2, PLLK3 are contiguous (at 0x24 to 0x27) */
    for (i = 0; i < 4; i++)
        wm_write(PLLN + i, pll_setups[freq_setups[fsel] & 1][i]);

    /* CLKCTRL_MCLKDIV divides fPLLOUT to get SYSCLK (256 * sample rate) */
    wm_write(CLKCTRL, CLKCTRL_CLKSEL
                         | (freq_setups[fsel] & CLKCTRL_MCLKDIV_MASK)
                         | CLKCTRL_BCLKDIV_2 | CLKCTRL_MS);

    /* set ADC and DAC filter characteristics according to sample rate */
#ifdef HAVE_IPOD_UI
    /* shadowed, so the S1 spike's M128ENB survives a rate change */
    mutex_lock(&dac_mtx);
    addctrl_reg = (freq_setups[fsel] & ADDCTRL_SR_MASK) | ADDCTRL_SLOWCLKEN;
    wm_write(ADDCTRL, addctrl_reg | (dac_state.m128 ? ADDCTRL_M128ENB : 0));
    mutex_unlock(&dac_mtx);
#else
    wm_write(ADDCTRL, (freq_setups[fsel] & ADDCTRL_SR_MASK)
                         | ADDCTRL_SLOWCLKEN);
#endif
    /* SLOWCLK enabled for zero cross timeout to work */
}

void audiohw_enable_recording(bool source_mic)
{
    (void)source_mic; /* We only have a line-in (I think) */

    wm_write(PWRMGMT2, PWRMGMT2_ROUT1EN | PWRMGMT2_LOUT1EN
                          | PWRMGMT2_INPGAENR | PWRMGMT2_INPGAENL
                          | PWRMGMT2_ADCENR | PWRMGMT2_ADCENL);

    wm_write(INCTRL, INCTRL_R2_2INPGA | INCTRL_L2_2INPGA);

    wm_write(LADCBOOST, LADCBOOST_L2_2BOOST(5));
    wm_write(RADCBOOST, RADCBOOST_R2_2BOOST(5));

    /* Enable monitoring */
    wm_write(LOUTMIX, LOUTMIX_BYP2LMIXVOL(5)
                         | LOUTMIX_BYPL2LMIX | LOUTMIX_DACL2LMIX);
    wm_write(ROUTMIX, ROUTMIX_BYP2RMIXVOL(5)
                         | ROUTMIX_BYPR2RMIX | ROUTMIX_DACR2RMIX);
}

void audiohw_disable_recording(void)
{
    wm_write(LOUTMIX, LOUTMIX_DACL2LMIX);
    wm_write(ROUTMIX, ROUTMIX_DACR2RMIX);

    wm_write(PWRMGMT2, PWRMGMT2_ROUT1EN | PWRMGMT2_LOUT1EN);
}

/* volume in 0 .. 63, corresponds to -12dB .. +35.25dB in 0.75dB steps */
void audiohw_set_recvol(int left, int right, int type)
{
    switch (type)
    {
    case AUDIO_GAIN_MIC:
        right = left;
        /* fall through */
    case AUDIO_GAIN_LINEIN:
        wm_write(LINPGAVOL, LINPGAVOL_INPGAZCL
                               | (left & LINPGAVOL_INPGAVOL_MASK));
        wm_write(RINPGAVOL, RINPGAVOL_INPGAVU | RINPGAVOL_INPGAZCR
                               | (right & RINPGAVOL_INPGAVOL_MASK));
        break;
    default:
        return;
    }
}

void audiohw_set_monitor(bool enable)
{
    (void)enable;
}

#ifdef HAVE_IPOD_UI
/* dac_ext.h (docs/phase15-brief.md sections 2.1, 3.1, 3.2) */

void dac_eq_set(const struct dac_eq_band *band, int precut_db)
{
    unsigned short regs[DAC_EQ_BANDS];
    int i;

    if (band)
    {
        for (i = 0; i < DAC_EQ_BANDS; i++)
        {
            int g = MIN(MAX(band[i].gain, -12), 12);
            regs[i] = EQ_GAIN_VALUE(g)
                    | EQ_CUTOFF_VALUE((band[i].freq & 3) + 1);
            if (i == 0)
                regs[i] |= EQ1_EQ3DMODE;    /* EQ stays on the DAC path */
            else if (i < 4 && band[i].wide)
                regs[i] |= EQ_BW_WIDE;
        }
        precut_db = MIN(MAX(precut_db, 0), 12);
    }
    else
    {
        /* off: the peaks flat at their reset value, EQ1/EQ5 back to bass
         * and treble (0 dB in the shell, so flat) */
        regs[0] = eq1_reg;
        for (i = 1; i < 4; i++)
            regs[i] = EQ_GAIN_VALUE(0) | EQ_CUTOFF_VALUE(2);    /* 0x02c */
        regs[4] = eq5_reg;
        precut_db = 0;
    }

    /* EQ registers keep their values in standby: no wake needed */
    mutex_lock(&dac_mtx);
    for (i = 0; i < DAC_EQ_BANDS; i++)
    {
        if (eq_reg[i] != regs[i])
        {
            eq_reg[i] = regs[i];
            wm_write(EQ1 + i, regs[i]);
            dac_state.eq_writes++;
        }
    }
    dac_state.hw_eq = band != NULL;
    if (dac_precut != precut_db)
    {
        dac_precut = precut_db;
        dac_write_vol();
        dac_state.eq_writes += 2;
    }
    mutex_unlock(&dac_mtx);
}

void dac_eq_set_m128(bool on)
{
    mutex_lock(&dac_mtx);
    dac_state.m128 = on;
    wm_write(ADDCTRL, addctrl_reg | (on ? ADDCTRL_M128ENB : 0));
    mutex_unlock(&dac_mtx);
}

int dac_reglog_read(unsigned int *seq, uint16_t *out, int max)
{
    unsigned int s;
    int n = 0;

    mutex_lock(&dac_mtx);
    s = *seq;
    if (dac_reglog_seq - s > DAC_REGLOG_LEN)
        s = dac_reglog_seq - DAC_REGLOG_LEN;    /* overwritten: oldest kept */
    while (s != dac_reglog_seq && n < max)
        out[n++] = dac_reglog[s++ % DAC_REGLOG_LEN];
    *seq = s;
    mutex_unlock(&dac_mtx);
    return n;
}

#ifdef HAVE_DAC_STANDBY
/* Anti-pop order. The WM8758/WM8983 datasheet was not at hand when this
 * was written (no copy could be fetched, 2026-10-11), so the reference is
 * the documented WM8978 sequence in drivers/audio/wm8978.c, the sibling
 * part with the same power blocks (its audiohw_preinit() steps 2-6 and
 * audiohw_close() steps 1-4 follow that datasheet's power-up/down
 * sequence): outputs muted and disabled before the mixers and DACs go,
 * enabled last on the way up, VMID and the bias untouched in between.
 * Here VMID (500 k), BIASEN and BUFIOEN stay on throughout, and
 * OUTCTRL_VROI (set in preinit) ties a disabled output to VMID through
 * 30 k, so the headphone pins stay at mid-rail. Caller holds dac_mtx. */
static void dac_standby_enter(void)
{
    unsigned short r3 = pwr_reg[2];

    /* 1. soft mute (PCM has been stopped for DAC_IDLE_MS, so the I2S data
     *    is already silence and no ramp needs waiting for) */
    wm_write_raw(DACCTRL, dacctrl_reg | DACCTRL_SOFTMUTE);
    dac_state.standby = true;
    /* 2. OUT1 headphone amps off (and OUT2, the line out, if on) */
    wm_write_raw(PWRMGMT2, standby_mask(PWRMGMT2, pwr_reg[1]));
    r3 &= ~(PWRMGMT3_LOUT2EN | PWRMGMT3_ROUT2EN);
    if (r3 != pwr_reg[2])
        wm_write_raw(PWRMGMT3, r3);
    /* 3. output mixers off */
    r3 &= ~(PWRMGMT3_LMIXEN | PWRMGMT3_RMIXEN);
    wm_write_raw(PWRMGMT3, r3);
    /* 4. DACs off */
    wm_write_raw(PWRMGMT3, standby_mask(PWRMGMT3, pwr_reg[2]));
    /* 5. PLL off: BCLK/LRCLK stop, safe only with PCM stopped */
    wm_write_raw(PWRMGMT1, standby_mask(PWRMGMT1, pwr_reg[0]));
    dac_state.sleeps++;
}

/* The reverse: PLL on and settled, DACs, mixers, outputs, unmute. Caller
 * holds dac_mtx. Busy-waits (no sleep), so a click that wakes the codec
 * sees one short delay and no other thread runs in between. */
static void dac_standby_wake(void)
{
    unsigned short r3 = standby_mask(PWRMGMT3, pwr_reg[2]);
    unsigned long t0 = USEC_TIMER;

    /* 1. PLL on, then let it lock */
    wm_write_raw(PWRMGMT1, pwr_reg[0]);
    udelay(DAC_PLL_SETTLE_US);
    /* 2. DACs (EQ1_EQ3DMODE is still set: the EQ registers survive) */
    r3 |= pwr_reg[2] & (PWRMGMT3_DACENL | PWRMGMT3_DACENR);
    wm_write_raw(PWRMGMT3, r3);
    /* 3. mixers, then OUT2 if the line out was on */
    r3 |= pwr_reg[2] & (PWRMGMT3_LMIXEN | PWRMGMT3_RMIXEN);
    wm_write_raw(PWRMGMT3, r3);
    if (r3 != pwr_reg[2])
        wm_write_raw(PWRMGMT3, pwr_reg[2]);
    /* 4. OUT1 headphone amps */
    wm_write_raw(PWRMGMT2, pwr_reg[1]);
    dac_state.standby = false;
    /* 5. unmute (the shadow: still muted if a caller muted meanwhile) */
    wm_write_raw(DACCTRL, dacctrl_reg);
    dac_state.last_wake_us = USEC_TIMER - t0;
    dac_state.wakes++;
}

static bool dac_allow WM_XDATA = true;

void dac_standby_allow(bool allow)
{
    dac_allow = allow;
    if (!allow)
        dac_wake();
}

bool dac_standby_allowed(void)
{
    return dac_allow;
}

void dac_idle_poll(void)
{
    if (dac_state.standby || !dac_allow)
        return;
    if (pcm_is_playing())
    {
        dac_busy_tick = current_tick;   /* includes the mixer's idle tail */
        return;
    }
    if (TIME_BEFORE(current_tick, dac_busy_tick + DAC_IDLE_TICKS))
        return;

    /* re-check under the mutex: a dac_wake() that got there first moved
     * dac_busy_tick, so the PCM start behind it can't lose its clocks */
    mutex_lock(&dac_mtx);
    if (!dac_state.standby && !pcm_is_playing()
        && !TIME_BEFORE(current_tick, dac_busy_tick + DAC_IDLE_TICKS))
        dac_standby_enter();
    mutex_unlock(&dac_mtx);
}

bool dac_idle_pending(void)
{
    return !dac_state.standby && dac_allow;
}

void dac_busy_now(void)
{
    dac_busy_tick = current_tick;
}

void dac_wake(void)
{
    mutex_lock(&dac_mtx);
    dac_busy_tick = current_tick;
    if (dac_state.standby)
        dac_standby_wake();
    mutex_unlock(&dac_mtx);
}
#else
void dac_standby_allow(bool allow)
{
    (void)allow;
}

bool dac_standby_allowed(void)
{
    return false;
}

void dac_idle_poll(void)
{
}

bool dac_idle_pending(void)
{
    return false;
}

void dac_wake(void)
{
}

void dac_busy_now(void)
{
}
#endif /* HAVE_DAC_STANDBY */

void dac_get_stats(struct dac_stats *s)
{
    *s = dac_state;
}
#endif /* HAVE_IPOD_UI */
