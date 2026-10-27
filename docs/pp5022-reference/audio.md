# Audio: the I2S block and the WM8758

The PP5022 sends PCM over I2S to a Wolfson **WM8758** codec, which it controls over I2C (address `0x1A`). On the iPod Video the **codec is the I2S clock master** and the PP is a slave. The BCM2722 has pins on the same I2S clock nets (§6).

## 1. I2S registers [rockbox] `pp5020.h`

| Address | Name | Meaning |
|---|---|---|
| `0x7000_2800` | `IISCONFIG` | configuration (below) |
| `0x7000_2808` | `IISCLK` | clock divider, bits `[8:0]` (master mode only) |
| `0x7000_280C` | `IISFIFO_CFG` | FIFO levels and clears (below) |
| `0x7000_2840` | `IISFIFO_WR` | TX FIFO write (32- or 16-bit access) |
| `0x7000_2880` | `IISFIFO_RD` | RX FIFO read |
| `0x6000_6080` | `IISDIV` | clock divider in the clock block (master mode only) |

`IISCONFIG`:

| Bits | Name | Meaning |
|---|---|---|
| 31 | `RESET` | soft reset (set, then clear) |
| 29 | `TXFIFOEN` | transmitter on |
| 28 | `RXFIFOEN` | receiver on |
| 25 | `MASTER` | 1 = PP generates BCLK/LRCK. **Leave 0 on the iPod** |
| `[11:10]` | format | 0 = I2S (one-bit delay), 2 = left-justified, 1 and 3 unknown |
| `[9:8]` | size | 0 = 16-bit; others unknown |
| `[6:4]` | FIFO format | 0 = LE halfword; 3 = 32-bit MSB-aligned LE; 4 = 16-bit LE; 7 = 16-bit LE (variant used by Rockbox: one 32-bit write = one stereo frame, **left in the low halfword**); 1, 2 big-endian forms; 5, 6 seem equal to 4 |
| 1 | `IRQTX` | TX FIFO interrupt enable |
| 0 | `IRQRX` | RX FIFO interrupt enable |

`IISFIFO_CFG`:

| Bits | Meaning |
|---|---|
| `[29:24]` | RX FIFO entries full |
| `[21:16]` | TX FIFO entries free (16 = empty) |
| 12 | `RXCLR` — clear RX FIFO |
| 8 | `TXCLR` — clear TX FIFO |
| `[5:4]` | RX attention level: 1 = 4, 2 = 8, 3 = 12 entries |
| `[1:0]` | TX attention (DMA request) level: 1 = 4, 2 = 8, 3 = 12 free |

The FIFO is 16 entries deep (32-bit entries) [rockbox].

**Register values read on hardware under Rockbox** [external] siggi `08`: `0x7000_2800` = `0x2000_0070` (TX on, FIFO format 7, I2S, 16-bit, slave), `0x7000_2804` = `0xA000_000A`, `0x7000_2808` = `0x0000_001F`, `0x7000_280C` = `0x0010_0031`, `0x7000_2810`–`0x7000_283C` zero. `0x7000_2804` is not named by Rockbox.

## 2. Bring-up (PP side) [rockbox] `wmcodec-pp.c`, `i2s-pp.c`

```
DEV_INIT2 &= ~0x300                 ; normal CDI and I2S pin groups
DEV_INIT1 &= ~0x0300_0000
DEV_RS |= DEV_I2S; DEV_RS &= ~DEV_I2S
DEV_EN |= DEV_I2S | DEV_EXTCLOCKS   ; 0x800 | 0x2
0x7000_0018 &= ~0xC                 ; external device clock = 24 MHz (to the codec's MCLK)
IISCONFIG |= RESET; IISCONFIG &= ~RESET
IISCONFIG: format I2S, size 16, FIFO format 7, MASTER clear
IISFIFO_CFG |= RX_LVL_12 | TX_LVL_4 ; DMA request when 4 slots free
IISFIFO_CFG |= RXCLR | TXCLR
IISCONFIG |= TXFIFOEN               ; at PCM init
```

Apple's driver, by comparison, clears only `DEV_INIT2` bit 8, and starts playback with `TXCLR`, 16 zero words into the FIFO, then sets bits 1 and 29; it stops by clearing bits 29 and 1 [apple] (osos 1.3 `0x1018_C55C`, `0x1018_C488`).

Feeding: DMA channel 0 with request 2 (`dma.md` §2), or by FIQ on `IIS_IRQ` (source 10) writing the FIFO directly (Rockbox's PP5002 path and recording).

## 3. The WM8758 over I2C [rockbox] `wm8758.c`, `wm8758.h`

Write-only, 9-bit registers: byte 0 = `(reg << 1) | (value >> 8 & 1)`, byte 1 = `value & 0xFF`, to 7-bit address `0x1A`. Nothing can be read back. [rockbox]; Apple's helper uses the same format [external] siggi `17`.

The codec's MCLK is the PP's 24 MHz external device clock; the codec's PLL (with its /2 prescaler, so a 12 MHz input) makes the audio clocks, and the codec drives BCLK and LRCK.

### Power-up sequence [rockbox] `audiohw_preinit`, `audiohw_postinit`

| Step | Register (number) | Value | Purpose |
|---|---|---|---|
| 1 | BIASCTRL (`0x3D`) | `0x100` | low bias |
| 2 | OUTCTRL (`0x31`) | `0x187` | output common, thermal shutdown, VREF to outputs through 30 kΩ |
| 3 | LOUT1/ROUT1/LOUT2/ROUT2VOL (`0x34`–`0x37`) | `0x140` each | muted, minimum |
| 4 | OUT3MIX, OUT4MIX (`0x38`, `0x39`) | `0x40` each | muted |
| 5 | PWRMGMT2 (`0x02`) | `0x180` | headphone outputs on |
| 6 | OUT4TOADC (`0x2A`) | `0x004` | POBCTRL: bias independent of VMID |
| 7 | PWRMGMT3 (`0x03`) | `0x00F` | DACs and mixers |
| 8 | PWRMGMT1 (`0x01`) | `0x02F` | PLL, bias, buffers, VMID 10 kΩ (fast charge) |
| 9 | AINTFCE (`0x04`) | `0x010` | I2S, 16-bit |
| 10 | CLKCTRL (`0x06`) | `0x001` | codec is clock master |
| 11 | sample rate (below) | | |
| 12 | LOUTMIX, ROUTMIX (`0x32`, `0x33`) | `1` each | DAC to mixer |
| 13 | OUT4TOADC | `0` | |
| later | PWRMGMT1 | `0x02E` | VMID 500 kΩ (normal) |
| | BIASCTRL | `0` | |
| | DACCTRL (`0x0A`) | `0x008` | 128× oversampling, unmuted |

Rockbox places no delay between the steps in this file; the gap between pre- and post-init is the caller's. Mute: DACCTRL `0x040` (soft mute); unmute `0x008`.

### Sample rates [rockbox]

PLL settings `{PLLN (0x24), PLLK1 (0x25), PLLK2 (0x26), PLLK3 (0x27)}`:

| Family | Values | PLL output |
|---|---|---|
| 44.1 kHz | `{0x17 (prescale, N = 7), 0x21, 0x161, 0x26}` | 90.3168 MHz → 22.5792 MHz |
| 48 kHz | `{0x18 (prescale, N = 8), 0x0C, 0x93, 0xE9}` | 98.304 MHz → 24.576 MHz |

Then CLKCTRL = CLKSEL (PLL) | MCLKDIV | BCLKDIV /2 | master — `0x145` for 44.1/48 kHz — and ADDCTRL (`0x07`) = sample-rate field | SLOWCLKEN. SYSCLK = 256·fs. MCLKDIV per rate: /2 for 48 and 44.1; /3 for 32; /4 for 24 and 22.05; /6 for 16; /8 for 12 and 11.025; /12 for 8 kHz. Write these with the output muted.

Supported rates: 8, 11.025, 12, 16, 22.05, 24, 32, 44.1, 48 kHz (`HW_SAMPR_CAPS`).

### Power-down without a pop [rockbox] `audiohw_close`

Soft mute; OUTCTRL = `0x101`; OUT4TOADC = `0x10` (VMID toggle); PWRMGMT1 = `0x028` (VMID off); **wait 300 ms** for VMID to discharge; then PWRMGMT2, PWRMGMT3, PWRMGMT1 = 0.

Line out is OUT2 (PWRMGMT3 bits `0x40 | 0x20`); on our unit the dock line-out no longer exists [ours].

### Volume [rockbox]

The headphone amplifier (OUT1, 6-bit, with zero-cross bit 7 and the update bit 8 on the right channel) and the DAC digital volume are combined: above −57 dB the DAC stays at 0 dB and the amp moves; below, the amp sits at its minimum and the DAC attenuates.

## 4. Supplies and enables

Codec supply: PMU `D1REGC1` (Rockbox sets 2.5 V; Apple's default 3.0 V) [rockbox]. No GPIO enable for the codec is used by Rockbox.

## 5. Our measurements [silicon]

- The bus runs at 44.1 kHz when Rockbox plays CD-rate audio: the BCM sampled LRCK at 88,198 edges/s (= 2 × 44,099) (S32, repeated in S33 and S34).
- Waking the codec from our standby state (PLL, DACs, mixers, outputs off; VMID kept charged) takes 6.4 ms median, 12.0 ms worst over 657 wakes, including a fixed 2 ms PLL settle we chose without a datasheet figure [silicon]/[ours].

## 6. The I2S bus shared with the BCM2722 [silicon]

The BCM2722 has three GPIOs (its GPIO 1–3) wired near this bus. With the BCM sampling them as inputs (S32–S34):

| BCM pin | While the PP plays | Conclusion |
|---|---|---|
| GPIO 2 | ~285,000 edges/s (faster than the BCM's ~630 k samples/s can resolve) | on **BCLK** |
| GPIO 3 | 88,198 edges/s | on **LRCK** |
| GPIO 1 | 0 edges; followed the BCM's own pull-up/down in every PP state (playing, transmitter stopped, audio off) | **not** on the PP's data-out net (S34) |

After playback stops, the codec parks both clocks high (S34; S32 had seen them low before any playback).

So the BCM can see the codec's clocks, but how (or whether) its audio data can reach the WM8758 is unknown: there is no known PP register that makes the PP's I2S data pin high-impedance, and Rockbox's stop leaves `TXFIFOEN` set, so the PP keeps clocking out an empty FIFO while the codec runs. Apple's driver can swap the clock master (WM8758 R6 bit 0 and `IISCONFIG` bit 25 set opposite) [apple] (`0x1015_BFF0`). See `../bcm2722-reference/audio.md` and `open-questions.md`.
