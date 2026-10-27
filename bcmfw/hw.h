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
 * bcmfw: BCM2722 register addresses we use, as VideoCore II sees them.
 * Meanings and confidence: docs/research/bcm2722/hardware.md.
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
#ifndef BCMFW_HW_H
#define BCMFW_HW_H

/* Interrupt controller */
#define IC_LEVEL        0x10001000  /* bits 2:0: current level */
#define IC_PRIO(n)      (0x10001010 + 4 * (n))  /* 4 bits per source, 8/word */

/* System timer: free-running 1 MHz counter, four compares */
#define TIMER_CS        0x10001800  /* match status, write 1 to clear */
#define TIMER_CLO       0x10001804
#define TIMER_C(k)      (0x10001808 + 4 * (k))
#define TIMER_IRQ(k)    (16 + (k))  /* interrupt source of compare k */

/* Clock block (hardware.md, "Clocks"): writes carry the 0xA5A5 password.
 * 0x4C = 0x51B looks like the counter's prescaler: 27 MHz / 0x1B */
#define CLK_PW(v)       (0xA5A50000 | (v))
#define CLK_4C          0x10001C4C
#define CLK_54          0x10001C54
#define CLK_58          0x10001C58

/* GPIO (52 pins) */
#define GPIO_FSEL(k)    (0x10002800 + 4 * (k))  /* 2 bits per pin */
#define GPIO_SET1       0x10002814  /* pins 32-51 */
#define GPIO_CLR1       0x1000281C
#define GPIO_LEV1       0x10002824
#define GPIO_LCD_RESET  48          /* active low */
#define GPIO_PANEL_ID0  51          /* panel ID = GPIO51 + 2 * GPIO50 */
#define GPIO_PANEL_ID1  50

/* LCD "MPU" interface */
#define LCD_CTRL        0x10004000  /* bit 0: interface on */
#define LCD_TIMING0     0x10004010  /* bus timing, chip select 0 */
#define LCD_TIMING1     0x10004014
#define LCD_INDEX       0x10004178  /* command / register index */
#define LCD_DATA        0x1000417C  /* parameter / register value / pixel */

#ifndef __ASSEMBLER__
#define REG(a) (*(volatile uint32_t *)(a))
#endif

#endif /* BCMFW_HW_H */
