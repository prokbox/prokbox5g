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
 * bcmfw LCD: panel detection, power-on and power-off, windows and the
 * CPU pixel path from the host framebuffer at 0xE0000.
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
#include "bcmfw.h"
#include "hw.h"
#include "dma.h"
#include "clock.h"

#define FB ((const uint16_t *)BCMFW_FB)

static const struct panel *panel;
static int panel_on;
#if BCMFW_LCD_STANDBY
static int panel_id;
static uint32_t standby;        /* BCMFW_STANDBY_* while dark, else 0 */
static uint32_t standby_refused;
#endif

/* a video transfer still running ends before any other panel traffic */
#if BCMFW_VIDEO || BCMFW_R3D || BCMFW_JOBS
void lcd_blit_wait(void);
#define BLIT_WAIT() lcd_blit_wait()
#else
#define BLIT_WAIT() do { } while (0)
#endif

static void lcd_index(uint32_t r)
{
    REG(LCD_INDEX) = r;
}

static void lcd_data(uint32_t v)
{
    REG(LCD_DATA) = v;
}

static void lcd_reg(uint32_t r, uint32_t v)
{
    REG(LCD_INDEX) = r;
    REG(LCD_DATA) = v;
}

static int timing_k;            /* the second word in use (0 or 1) */
static uint32_t timing_sel;     /* the host's selection the words follow */

static const uint32_t sweep_khz[BCMFW_SWEEP_N] =
{
    BCMFW_SWEEP_KHZ_0, BCMFW_SWEEP_KHZ_1, BCMFW_SWEEP_KHZ_2
};

/* Interface on and bus timing; k selects the panel's second timing word.
 * Our words (BCMFW_LCD_BUS_KHZ) unless the host asked for Apple's or for
 * one of the sweep's clocks. */
static void lcd_timing(int k)
{
    uint32_t t0, t1, khz, sw;

    timing_k = k & 1;
    timing_sel = MB(BCMFW_MB_TIMING_SEL);
    sw = timing_sel ^ BCMFW_TIMING_SWEEP_MAGIC;
    if (timing_sel == BCMFW_TIMING_APPLE_MAGIC)
    {
        t0 = panel->apple0;
        t1 = panel->apple1[timing_k];
        khz = 0;
    }
    else if (sw < BCMFW_SWEEP_N)
    {
        t0 = panel->sweep[sw][0];
        t1 = panel->sweep[sw][1 + timing_k];
        khz = sweep_khz[sw];
    }
#if BCMFW_CLOCK
    else if (clock_is_fast())
    {
        /* On PLL A: Apple's own 150 MHz words, the ones Apple's firmware
         * drives this panel with.  Whether the LCD bus follows the core
         * off the crystal is stage 4c's question; if it stays at 27 MHz
         * these words are only slower (hardware.md, "The 150 MHz clock") */
        t0 = panel->apple0;
        t1 = panel->apple1[timing_k];
        khz = 0;
    }
#endif
    else
    {
        t0 = panel->timing0;
        t1 = panel->timing1[timing_k];
        khz = BCMFW_LCD_BUS_KHZ;
    }
    REG(LCD_CTRL) = 1;
    REG(LCD_TIMING0) = t0;
    REG(LCD_TIMING1) = t1;
    MB(BCMFW_MB_TIMING0) = t0;
    MB(BCMFW_MB_TIMING1) = t1;
    MB(BCMFW_MB_TIMING_KHZ) = khz;
}

/* Before a frame: the host changed its choice of words */
static void lcd_timing_check(void)
{
    if (panel_on && timing_sel != MB(BCMFW_MB_TIMING_SEL))
        lcd_timing(timing_k);
}

#if BCMFW_CLOCK
/* After a core clock switch: the words for the new clock, if the panel
 * is on (lcd_power_on applies them otherwise).  No transfer may run. */
void lcd_clock_changed(void)
{
    if (panel_on)
        lcd_timing(timing_k);
}
#endif

static void gpio_reset(int high)
{
    REG(high ? GPIO_SET1 : GPIO_CLR1) = 1 << (GPIO_LCD_RESET - 32);
}

/* DCS colour table: red and blue 0, 3, 5 .. 63 (2i + 1 after the first),
 * green 0 .. 63 */
static void lcd_dcs_lut(void)
{
    int i;

    lcd_index(0x2D);
    for (i = 0; i < 32; i++)
        lcd_data(i ? 2 * i + 1 : 0);
    for (i = 0; i < 64; i++)
        lcd_data(i);
    for (i = 0; i < 32; i++)
        lcd_data(i ? 2 * i + 1 : 0);
}

/* A panel script; the step word names each record (base + index) before
 * it runs, so a panel access that stalls the core is named exactly */
static void run_script(const uint16_t *s, uint32_t base)
{
    uint32_t rec;

    for (rec = 0; ; rec++)
    {
        uint32_t w = *s++;
        uint32_t arg = w & 0xFFF;
        uint32_t n;

        bcmfw_step(base + (rec & 0xFFF));
        switch (w >> 12)
        {
        case 0:
            lcd_reg(arg, *s++);
            break;
        case 1:
            lcd_index(arg);
            break;
        case 2:
            lcd_index(arg & 0xFF);
            for (n = arg >> 8; n; n--)
                lcd_data(*s++);
            break;
        case 3:
            mdelay(arg);
            break;
        case 4:
            lcd_timing(arg);
            break;
        case 5:
            lcd_dcs_lut();
            break;
        default:
            return;
        }
    }
}

int lcd_detect(void)
{
    int id = BCMFW_PANEL_ID;

    if (id < 0)
    {
        uint32_t lev = REG(GPIO_LEV1);

        id = ((lev >> (GPIO_PANEL_ID0 - 32)) & 1)
           + 2 * ((lev >> (GPIO_PANEL_ID1 - 32)) & 1);
    }
    panel = panel_get(id, BCMFW_PANEL_MODEL);
#if BCMFW_LCD_STANDBY
    panel_id = id;
#endif
    return id;
}

int lcd_is_on(void)
{
    return panel_on;
}

#if BCMFW_HOSTPROTO
/* The warm swap: the second timing word in use (the park's flags) */
int lcd_timing_word(void)
{
    return timing_k;
}

/* Started by a warm swap with the panel on (BCMFW_WARM_PANEL_ON): the
 * interface and the bus timing the last image left, and nothing else.
 * GPIO 48 is not touched (still an output, high), so there is no reset
 * pulse, no power-on script and no bus resync: the panel keeps its
 * frame until the first update. */
void lcd_warm(int k)
{
    lcd_timing(k);
    panel_on = 1;
}
#endif

#if BCMFW_LCD_STANDBY
/* Out of standby: no reset pulse, no wait after it, no bus resync */
static void standby_wake(void)
{
    uint32_t t0 = REG(TIMER_CLO);

    if (standby == BCMFW_STANDBY_LIGHT)
        run_script(panel_light(panel_id, 1), BCMFW_STEP_SCRIPT + 0x7000);
    else
    {
        lcd_timing(0);
        run_script(panel->init, BCMFW_STEP_SCRIPT);
        run_script(panel->on, BCMFW_STEP_SCRIPT + 0x1000);
    }
    standby = 0;
    panel_on = 1;
    MB(BCMFW_MB_PON_US) = ((REG(TIMER_CLO) - t0) & ~BCMFW_PON_FROM_SBY)
                        | BCMFW_PON_FROM_SBY;
}

void lcd_standby(uint32_t depth)
{
    BLIT_WAIT();
    standby_refused = 0;
    if (!panel_on || (depth != BCMFW_STANDBY_LIGHT &&
                      depth != BCMFW_STANDBY_DEEP) ||
        (depth == BCMFW_STANDBY_LIGHT && !panel_light(panel_id, 0)))
    {
        standby_refused = BCMFW_STANDBY_REFUSED;
        return;
    }
    if (depth == BCMFW_STANDBY_LIGHT)
        run_script(panel_light(panel_id, 0), BCMFW_STEP_SCRIPT + 0x6000);
    else
    {
        run_script(panel->init, BCMFW_STEP_SCRIPT);
        run_script(panel->off, BCMFW_STEP_SCRIPT + 0x2000);
    }
    panel_on = 0;               /* reset stays high */
    standby = depth;
}

uint32_t lcd_standby_state(void)
{
    return standby | standby_refused;
}
#endif

void lcd_power_on(void)
{
    uint32_t fsel;
    int i;
#if BCMFW_LCD_STANDBY
    uint32_t t0 = REG(TIMER_CLO);

    if (standby)
    {
        standby_wake();
        return;
    }
#endif

    bcmfw_step(BCMFW_STEP_POWER + 0);   /* interface on, bus timing */
    lcd_timing(0);
    mdelay(1);

    /* GPIO 48 (reset) as output; 49-51 (TE, panel ID) stay inputs */
    bcmfw_step(BCMFW_STEP_POWER + 1);
    fsel = REG(GPIO_FSEL(3)) & ~3;
    REG(GPIO_FSEL(3)) = fsel;
    REG(GPIO_FSEL(3)) = fsel | 1;

    bcmfw_step(BCMFW_STEP_POWER + 2);   /* reset pulse */
    gpio_reset(0);
    mdelay(2);
    gpio_reset(1);
    mdelay(2);
    gpio_reset(0);
    mdelay(2);
    gpio_reset(1);
    bcmfw_step(BCMFW_STEP_POWER + 3);   /* the wait after reset */
    mdelay(panel->reset_ms);

    /* resynchronise the bus with six empty commands */
    for (i = 0; i < 6; i++)
    {
        bcmfw_step(BCMFW_STEP_POWER + 0x10 + i);
        lcd_index(0);
        mdelay(2);
    }

    run_script(panel->init, BCMFW_STEP_SCRIPT);
    run_script(panel->on, BCMFW_STEP_SCRIPT + 0x1000);
    panel_on = 1;
#if BCMFW_LCD_STANDBY
    MB(BCMFW_MB_PON_US) = (REG(TIMER_CLO) - t0) & ~BCMFW_PON_FROM_SBY;
#endif
}

void lcd_power_off(void)
{
    BLIT_WAIT();
#if BCMFW_LCD_STANDBY
    if (standby)
    {
        /* dark already; light standby left the power circuits on, so
         * they still go down the stock way before the reset line */
        if (standby == BCMFW_STANDBY_LIGHT)
        {
            run_script(panel->init, BCMFW_STEP_SCRIPT);
            run_script(panel->off, BCMFW_STEP_SCRIPT + 0x2000);
        }
        standby = 0;
        gpio_reset(0);
        return;
    }
#endif
    if (!panel_on)
        return;
    run_script(panel->init, BCMFW_STEP_SCRIPT);
    run_script(panel->off, BCMFW_STEP_SCRIPT + 0x2000);
    gpio_reset(0);
    panel_on = 0;
}

static void dcs_range(uint32_t cmd, uint32_t a, uint32_t b)
{
    lcd_index(cmd);
    lcd_data(a >> 8);
    lcd_data(a & 0xFF);
    lcd_data(b >> 8);
    lcd_data(b & 0xFF);
}

/* Window x0..x1, y0..y1 (inclusive), then the RAM-write command.  The
 * full-frame values are Apple's; the meaning of each register for a
 * sub-rectangle is our reading of the controllers ([M] DCS, [L] others). */
static void lcd_window(uint32_t x0, uint32_t y0, uint32_t x1, uint32_t y1)
{
    switch (panel->window)
    {
    case WIN_DCS:
        dcs_range(0x2B, y0, y1);
        dcs_range(0x2A, x0, x1);
        lcd_index(0x2C);
        break;
    case WIN_R2XX:
        lcd_reg(0x210, x0);
        lcd_reg(0x211, x1);
        lcd_reg(0x212, y0);
        lcd_reg(0x213, y1);
        lcd_reg(0x200, x0);
        lcd_reg(0x201, y0);
        lcd_index(0x202);
        break;
    default:
        /* a portrait controller on its side: 0x44/0x45 the 320 axis,
         * 0x46 the 240 axis (end << 8 | start), 0x20 = y, 0x21 = x */
        lcd_reg(0x45, x0);
        lcd_reg(0x44, x1);
        lcd_reg(0x46, y1 << 8 | y0);
        lcd_reg(0x20, y0);
        lcd_reg(0x21, x0);
        lcd_index(0x22);
        break;
    }
}

/* n pixels from src to the panel */
static void push(const uint16_t *src, uint32_t n)
{
#if BCMFW_PIXEL_PACE
    while (n--)
    {
        REG(LCD_DATA) = *src++;
        spin(BCMFW_PIXEL_PACE);
    }
#else
    bcmfw_push(src, n, (volatile uint32_t *)LCD_DATA);
#endif
}

/* h rows of w pixels, pitch pixels apart; step word every 16 rows */
static void push_rows(const uint16_t *src, uint32_t w, uint32_t h,
                      uint32_t pitch)
{
    uint32_t y;

    for (y = 0; y < h; y++, src += pitch)
    {
        if (!(y & 15))
            bcmfw_step(BCMFW_STEP_FRAME + y);
        push(src, w);
    }
}

#if BCMFW_LCD_DMA
/* The window is set: send w x h pixels from src by DMA.  If DMA cannot
 * (or did not finish), set the window again and send them by CPU; after
 * a timeout, paint the first BCMFW_DMA_MARK rows black so that a photo
 * shows the CPU drew this one. */
static void push_rect(const uint16_t *src, uint32_t x, uint32_t y,
                      uint32_t w, uint32_t h, uint32_t pitch)
{
    int r = dma_lcd_push(src, w, h, pitch);
    uint32_t n;

    if (r > 0)
        return;
    lcd_window(x, y, x + w - 1, y + h - 1);
    push_rows(src, w, h, pitch);
    if (r == 0 || !BCMFW_DMA_MARK)
        return;
    if (h > BCMFW_DMA_MARK)
        h = BCMFW_DMA_MARK;
    lcd_window(x, y, x + w - 1, y + h - 1);
    for (n = w * h; n; n--)
        lcd_data(0);
}
#endif

void lcd_update(void)
{
    BLIT_WAIT();
    lcd_timing_check();
    if (!panel_on)
        lcd_power_on();
    lcd_window(0, 0, BCMFW_LCD_WIDTH - 1, BCMFW_LCD_HEIGHT - 1);
#if BCMFW_LCD_DMA
    push_rect(FB, 0, 0, BCMFW_LCD_WIDTH, BCMFW_LCD_HEIGHT, BCMFW_LCD_WIDTH);
#else
    push_rows(FB, BCMFW_LCD_WIDTH, BCMFW_LCD_HEIGHT, BCMFW_LCD_WIDTH);
#endif
}

/* xy = x | y << 16, wh = w | h << 16 (mailbox words); clipped */
void lcd_update_rect(uint32_t xy, uint32_t wh)
{
    uint32_t x = xy & 0xFFFF, y = xy >> 16;
    uint32_t w = wh & 0xFFFF, h = wh >> 16;
    const uint16_t *src;

    BLIT_WAIT();
    if (!BCMFW_RECT_WINDOWED)
    {
        lcd_update();
        return;
    }
    if (x >= BCMFW_LCD_WIDTH || y >= BCMFW_LCD_HEIGHT || !w || !h)
        return;
    if (w > BCMFW_LCD_WIDTH - x)
        w = BCMFW_LCD_WIDTH - x;
    if (h > BCMFW_LCD_HEIGHT - y)
        h = BCMFW_LCD_HEIGHT - y;
    lcd_timing_check();
    if (!panel_on)
        lcd_power_on();
    lcd_window(x, y, x + w - 1, y + h - 1);
    src = FB + y * BCMFW_LCD_WIDTH + x;
#if BCMFW_LCD_DMA
    push_rect(src, x, y, w, h, BCMFW_LCD_WIDTH);
#else
    push_rows(src, w, h, BCMFW_LCD_WIDTH);
#endif
}

#if BCMFW_VIDEO || BCMFW_R3D || BCMFW_JOBS
/* The video path (and the 3D test's and the jobs' frames): w x h pixels from src (rows pitch pixels apart, any
 * buffer) to the panel at (x, y).  The DMA transfer runs while the core
 * decodes the next picture; lcd_blit_wait() ends it (the next blit, an
 * LCD command and the end of a session call it first). */
static struct
{
    const uint16_t *src;
    uint32_t pitch, x, y, w, h;
    int pending;
} blit;

void lcd_blit_wait(void)
{
    const uint16_t *src = blit.src;
    uint32_t h = blit.h;

    if (!blit.pending)
        return;
    blit.pending = 0;
    if (dma_lcd_wait() > 0)
        return;
    /* timed out: send it again by CPU */
    lcd_window(blit.x, blit.y, blit.x + blit.w - 1, blit.y + h - 1);
    push_rows(src, blit.w, h, blit.pitch);
}

void lcd_blit(const uint16_t *src, uint32_t pitch, uint32_t x, uint32_t y,
              uint32_t w, uint32_t h)
{
    lcd_blit_wait();
    if (x >= BCMFW_LCD_WIDTH || y >= BCMFW_LCD_HEIGHT || !w || !h)
        return;
    if (w > BCMFW_LCD_WIDTH - x)
        w = BCMFW_LCD_WIDTH - x;
    if (h > BCMFW_LCD_HEIGHT - y)
        h = BCMFW_LCD_HEIGHT - y;
    if (!panel_on)
        lcd_power_on();
    lcd_window(x, y, x + w - 1, y + h - 1);
    if (dma_lcd_start(src, w, h, pitch))
    {
        blit.src = src;
        blit.pitch = pitch;
        blit.x = x;
        blit.y = y;
        blit.w = w;
        blit.h = h;
        blit.pending = 1;
        return;
    }
    push_rows(src, w, h, pitch);
}
#endif
