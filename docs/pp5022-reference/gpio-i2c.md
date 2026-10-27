# GPIO, GPO32, device control and I2C

## 1. GPIO ports

Twelve 8-bit ports, A to L, in three modules of four. [rockbox] `pp5020.h`

Port `p` (A = 0 … L = 11) has eight registers. With `m = p >> 2` (module) and `k = p & 3`:

`base(p) = 0x6000_D000 + 0x80·m + 4·k`

| Offset from `base(p)` | Name | Meaning |
|---|---|---|
| `+0x00` | `ENABLE` | 1 = pin is a GPIO (0 = the pin's peripheral function) |
| `+0x10` | `OUTPUT_EN` | 1 = output |
| `+0x20` | `OUTPUT_VAL` | output level |
| `+0x30` | `INPUT_VAL` | pin level |
| `+0x40` | `INT_STAT` | interrupt pending |
| `+0x50` | `INT_EN` | interrupt enable |
| `+0x60` | `INT_LEV` | the level (polarity) that triggers |
| `+0x70` | `INT_CLR` | write 1 to clear |

Examples: `GPIOA_ENABLE` `0x6000_D000`, `GPIOD_OUTPUT_VAL` `0x6000_D02C`, `GPIOL_INPUT_VAL` `0x6000_D13C`.

Interrupts: module 0 (A–D) raises high source 32 (`GPIO0_IRQ`), module 1 (E–H) 33, module 2 (I–L) 34 (`interrupts-timers.md`). Whether triggering is level or edge is not settled; Rockbox re-arms by flipping `INT_LEV` to the opposite level after each event and clearing `INT_STAT`, which works either way. [rockbox] `usb-fw-pp502x.c`

### Atomic bit access (PP502x)

Every port register has a **masked-write alias at `+0x800`**: bits `[15:8]` of the written word select which bits change, bits `[7:0]` give their new values. [rockbox] `pp5020.h` (`GPIO_SET_BITWISE`, `GPIO_CLEAR_BITWISE`, `GPIO_WRITE_BITWISE`)

- set bits `m`: write `(m<<8) | m` to `reg + 0x800`
- clear bits `m`: write `m<<8`

Use it whenever both cores, or an ISR and a thread, touch the same port. Plain read-modify-write on the base address is not atomic.

## 2. GPO32 (`0x7000_0080`)

A 32-bit general-purpose output port separate from the GPIO ports. [rockbox] `pp5020.h`

| Address | Name |
|---|---|
| `0x7000_0080` | `GPO32_VAL` |
| `0x7000_0084` | `GPO32_ENABLE` (1 = the pin is driven by `GPO32_VAL`) |

No atomic alias is known; protect read-modify-write.

## 3. iPod Video pin assignments

| Pin | Dir | Function | Sense | Evidence |
|---|---|---|---|---|
| GPIOA bit 2 (`0x04`) | out | LTC4066 charger HPWR: USB current limit | 1 = 500 mA, 0 = 100 mA | [rockbox] `power-ipod.c:201–224` |
| GPIOA bit 5 (`0x20`) | in | hold switch | 0 = hold on | [rockbox] `button-clickwheel.c` |
| GPIOA bit 7 (`0x80`) | in | headphone detect | 1 = plugged | [rockbox] |
| GPIOB bit 0 (`0x01`) | in | charger status | 0 = charging | [rockbox] `power-ipod.c:90` |
| GPIOB bit 3 (`0x08`) | out | backlight circuit enable | 1 = on | [rockbox] `backlight-nano_video.c` |
| GPIOC bit 6 (`0x40`) | in | BCM2722 interrupt line (per Rockbox comment; never used as an interrupt) | – | [rockbox] `lcd-video.c:548–550` |
| GPIOC bit 7 (`0x80`) | – | taken from GPIO function by the LCD init (`ENABLE` bit cleared) | – | [rockbox] |
| GPIOD bit 7 (`0x80`) | out | backlight brightness pulse line (§3.1) | idle high | [rockbox] |
| GPIOG, GPIOH (all) | – | ATA data/control pins: `ENABLE = 0` (peripheral function) while the drive is powered, `0xFF` when off | – | [rockbox] `power-ipod.c:119–140` |
| GPIOI bits `0xBF`, GPIOK bits `0x1F` | – | more ATA pins, same treatment | – | [rockbox] |
| GPIOL bit 2 (`0x04`) | out | LTC4066 SUSP | 1 = draw no USB current | [rockbox] |
| GPIOL bit 3 (`0x08`) | in | external (dock/FireWire) power present | 0 = present | [rockbox] `power-ipod.c:53` |
| GPIOL bit 4 (`0x10`) | in | USB VBUS present (also the USB-insert interrupt) | 1 = present | [rockbox] `usb-fw-pp502x.c`, `power-ipod.c:56` |
| GPIOL bit 7 (`0x80`) | out | backlight LED switch (software PWM for fades) | 1 = on | [rockbox] |
| GPO32 bit 2–3 (`0x0C`) | – | cleared in `GPO32_ENABLE` to route the dock UART pins | – | [rockbox] `uart-pp.c:62–86` |
| GPO32 bit 14 (`0x4000`) | out | **BCM2722 power** | 1 = on | [rockbox] `lcd-video.c` |
| GPO32 bit 15 (`0x8000`) | out | enabled with bit 14 by the LCD init [rockbox]. **Apple's BCM hibernate pin:** after the BCM parks itself, Apple sets bit 15, waits 5 ms and clears bit 14; to wake it sets bit 14, waits 5 ms and clears bit 15. Off is both low, run is bit 14 alone (`osos` host `0x1026_668C`, `0x1028_92D4`) [apple]. Meaning (hibernate/retention request, or bypass) [hypothesis]; never driven by Rockbox | 1 = hibernate/retain (Apple's order only) | [apple]; `bcm2722-reference/brief-map-power.md` §3 |
| GPO32 bit 0 | – | `GPO32_ENABLE` bit cleared by the LCD init; function unknown | – | [rockbox] |
| GPO32 bit 28 (`0x1000_0000`) | out | LTC4066 CLDIS (allow up to 1.5 A from a wall supply); Rockbox leaves it alone as "not safe" | – | [rockbox] |
| GPO32 bit 30 (`0x4000_0000`) | out | **drive (ATA) power**, active low | 0 = on | [rockbox] `power-ipod.c:119–154` |

Everything else is unassigned in Rockbox. `system_init()` on the Video keeps only bit 14 of `GPO32_VAL` (`GPO32_VAL &= 0x4000`), so the BCM stays powered across Rockbox's start while everything else in GPO32 goes low — including bit 30, which turns the drive **on**. [rockbox]

### 3.1 Backlight [rockbox] `backlight-nano_video.c` (shared with the nano)

- Enable: GPIOB3 = 1, GPIOD7 = 1, wait 10 ms; the controller is now at level 16 of 32.
- One level up: GPIOD7 low for 10 µs, then high, wait 10 µs. One level down: low for **200 µs**, then high, wait 10 µs. Interrupts off during the low pulse. There is no absolute set; count steps from the known level 16.
- Disable: GPIOD7 = 0, GPIOB3 = 0, wait 50 ms.
- GPIOL7 switches the LED itself; Rockbox toggles it from a timer for PWM fades.

The backlight controller's part number is not known.

## 4. Device enable and reset (`0x6000_6000`)

| Address | Name | Meaning |
|---|---|---|
| `0x6000_6004` | `DEV_RS` | 1 = hold the block in reset |
| `0x6000_6008` | `DEV_RS2` | second reset word |
| `0x6000_600C` | `DEV_EN` | 1 = clock/enable the block |
| `0x6000_6010` | `DEV_EN2` | second enable word |

Bits (same position in `DEV_RS` and `DEV_EN`) [rockbox] `pp5020.h`:

| Bit | Mask | Block |
|---|---|---|
| 1 | `0x0000_0002` | `EXTCLOCKS` (external device clock output; needed for audio) |
| 2 | `0x0000_0004` | `SYSTEM` — **setting it in `DEV_RS` reboots the chip** |
| 3 | `0x0000_0008` | `USB0` |
| 6 | `0x0000_0040` | `SER0` |
| 7 | `0x0000_0080` | `SER1` |
| 11 | `0x0000_0800` | `I2S` |
| 12 | `0x0000_1000` | `I2C` |
| 14 | `0x0000_4000` | `ATA` |
| 16 | `0x0001_0000` | `OPTO` (wheel) — Rockbox's name `DEV_PIEZO` shares this value |
| 17 | `0x0002_0000` | `PWM` (piezo) |
| 22 | `0x0040_0000` | `USB1` |
| 23 | `0x0080_0000` | `FIREWIRE` |
| 25 | `0x0200_0000` | `IDE0` |
| 26 | `0x0400_0000` | `LCD` |

Reset pulse idiom: `DEV_RS |= bit; DEV_RS &= ~bit` (optionally with a few µs between).

**Start-up values on the iPod Video** [rockbox] `system_init()`: `DEV_EN = 0xC200_0124`, `DEV_EN2 = 0`, `CACHE_PRIORITY = 0x3F`, `GPO32_VAL &= 0x4000`, `DEV_INIT1 = 0`, `DEV_INIT2 = 0x4000_0000` (`INIT_PLL`), then `DEV_RS = 0x3DFF_FEF8`, `DEV_RS2 = 0xFFFF_FFFF`, `DEV_RS = 0`, `DEV_RS2 = 0` (reset everything resettable, then release). `0xC200_0124` = bits 2, 5, 8, 25, 30, 31: SYSTEM, IDE0 and four unnamed bits.

## 5. Chip and pin-mux registers (`0x7000_0000`)

| Address | Name | Known use | Evidence |
|---|---|---|---|
| `0x7000_0000` | `PP_VER1` | chip version, ASCII (read with `PP_VER2` as a string, e.g. the part number) | [rockbox] `debug-pp.c` |
| `0x7000_0004` | `PP_VER2` | | [rockbox] |
| `0x7000_0008` | `STRAP_OPT_A` | strap options; bits `[11:8]` cleared before the BCM bootstrap; on the Sansa a magic value here plus `DEV_RS.SYSTEM` selects the reboot mode | [rockbox] |
| `0x7000_000C` | `STRAP_OPT_B` | bits: `0x10` bus width, `0xC0` RAM type, `0x08` ROM type (names only) | [rockbox] |
| `0x7000_0010` | `DEV_INIT1` | pin-mux/peripheral init: bit 18 `INIT_BUTTONS`; bits `[25:24]` cleared for I2S; bits `[3:2]` cleared for the piezo | [rockbox] |
| `0x7000_0018` | – | bits `[3:2]` cleared = "external device clock 24 MHz" (audio) | [rockbox] `wmcodec-pp.c` |
| `0x7000_0020` | `DEV_INIT2` | bit 30 `INIT_PLL` (PLL power), bit 31 `INIT_USB`, bits `[9:8]` cleared = normal CDI/I2S pin groups | [rockbox] |
| `0x7000_0028` | – | USB PHY status/control: bit 7 polled ready, bit 1 set (`usb.md`) | [rockbox] |
| `0x7000_002C` | – | bits `[25:24]` set to enable the USB module | [rockbox] |
| `0x7000_0034` | `DEV_TIMING1` | `0x0303` below 66 MHz, `0x0808` at 80 MHz (wait states; `clocks-power.md`) | [rockbox] |
| `0x7000_0038` | `XMB_NOR_CFG` | name only | [rockbox] |
| `0x7000_003C` | `XMB_RAM_CFG` | `|= 0x47A` during USB init | [rockbox] |
| `0x7000_0040` | – | `0x1313` written before the BCM bootstrap | [rockbox] `lcd-video.c` |
| `0x7000_008C` | – | bits `0x0C` cleared to route the dock UART | [rockbox] `uart-pp.c` |

## 6. I2C controller (`0x7000_C000`)

A simple byte-oriented master that moves **1 to 4 bytes per transaction**. [rockbox] `i2c-pp.c`

| Address | Name | Width | Meaning |
|---|---|---|---|
| `0x7000_C000` | `I2C_CTRL` | 8 | bit 7 `SEND` (start), bit 5 read (1) / write (0), bits `[2:1]` byte count − 1 |
| `0x7000_C004` | `I2C_ADDR` | 8 | `(addr7 << 1) \| R/W` |
| `0x7000_C00C`–`0x7000_C018` | `I2C_DATA(0..3)` | 8 | data bytes, 4 bytes apart |
| `0x7000_C01C` | `I2C_STATUS` | 8 | bit 6 `BUSY` |
| `0x7000_C020`, `0x7000_C02C`, `0x7000_C030` | – | written by other PP502x players' init (`|= 3`, `0x55`, `0x54`); not on the iPod | [rockbox] |
| `0x6000_60A4` | – | I2C clock/config: the iPod writes `0`, then `0x80` | [rockbox] `i2c_init()` |

**Init** [rockbox]: `DEV_EN |= DEV_I2C`; pulse `DEV_RS.I2C`; `0x6000_60A4 = 0`; `0x6000_60A4 = 0x80`; then one dummy read (PMU register 0).

**Write** (1–4 bytes): wait `!BUSY`; `I2C_ADDR = addr<<1`; `I2C_CTRL &= ~0x20`; fill `I2C_DATA(i)`; `I2C_CTRL = (I2C_CTRL & ~6) | ((n−1)<<1)`; `I2C_CTRL |= 0x80`.

**Read** (1–4 bytes): wait `!BUSY`; `I2C_ADDR = (addr<<1) | 1`; `I2C_CTRL |= 0x20`; set the count; `I2C_CTRL |= 0x80`; wait `!BUSY`; read `I2C_DATA(i)`. A register read is a 1-byte write of the register number followed by a separate read transaction (no repeated start). Longer transfers are split into 4-byte pieces.

Rockbox never sees a NAK or error indication; only a busy timeout (1 s) is detected. Bus speed is unknown.

**Gotcha:** the wheel interface (`input.md`) raises the same interrupt line as I2C (source 40); Rockbox's I2C driver polls and never enables that interrupt for itself.

### Devices on the bus (iPod Video)

| 7-bit address | Device | Evidence |
|---|---|---|
| `0x08` | PCF50605-family PMU (RTC, regulators, ADC, power-on/off) | [rockbox] `pcf50605.c` |
| `0x1A` | Wolfson WM8758 audio codec (write-only) | [rockbox] `wmcodec-pp.c` |

No other device is addressed by Rockbox on the Video. The BCM2722 is not on I2C.
