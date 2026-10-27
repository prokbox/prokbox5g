# Display: the BCM2722 host port, from the PP side

The iPod Video's LCD is not attached to the PP5022. It belongs to the Broadcom BCM2722 video processor, which the PP reaches through a 16-bit memory-mapped port at `0x3000_0000`. The PP's own LCD bridges (`0x7000_3000`, `0x7000_8A00`) are unused on this model. Everything the PP must do to show a picture is here; the BCM side, its boot and its protocol are in `../bcm2722-reference/` (start with `host-interface.md` and `host-driver.md`).

## 1. The port [rockbox] `lcd-video.c:39–56`

The BCM bus is 16 bits wide. The BCM's three register-select lines are wired to PP address bits 16–18; low address bits are not decoded. A 32-bit access becomes two 16-bit cycles, low half first, so 32-bit loads/stores and `ldm/stm` bursts to the data register work.

| Address | Name | Width | Use |
|---|---|---|---|
| `0x3000_0000` | `BCM_DATA` | 16/32 | data, auto-incrementing |
| `0x3001_0000` | `BCM_WR_ADDR` | 16/32 | set the BCM-side write address (32-bit write) |
| `0x3002_0000` | `BCM_RD_ADDR` | 16/32 | set the read address; reading bit 0 = ready for a read address |
| `0x3003_0000` | `BCM_CONTROL` | 16 | handshake (below); writing `0x31` rings the BCM's doorbell |
| `0x3004_0000`–`0x3007_0000` | `BCM_ALT_*` | | a second set of the same four; used during the bootstrap |

`BCM_CONTROL` bits read: `0x02` write-ready, `0x10` read data ready; `ALT_CONTROL` `0x80` busy and `0x40` ready during the bootstrap. An unpowered or dead port reads `0xFFFF` per halfword.

Apple's diagnostic image drives the same register map through a second window at `0xB000_0000`, after writing `0x9801_6460` to `0x7000_0030`; whether that write switches the decode is not known [external] siggi `07`.

## 2. Transactions [rockbox] `lcd-video.c:233–271`

- **Write:** write the 32-bit BCM address to `BCM_WR_ADDR`; wait for `CONTROL & 0x02`; write data words to `BCM_DATA`.
- **Read:** wait for `RD_ADDR & 1`; write the address to `BCM_RD_ADDR`; wait for `CONTROL & 0x10`; read `BCM_DATA`.
- Rockbox's waits are unbounded and hang on a dead port; ours are bounded at 20 ms [ours].

Measured [silicon]: under load, read-address waits 35–38 µs, read-data waits 20–47 µs, write waits 2–6 µs, never a timeout (S12). A command round trip took 6–7 µs (S2). **A 32-bit word read while the BCM is writing it can be torn** between its two halves; re-read or encode values so a torn read is detectable (S11, S12; `../bcm2722-reference/quirks.md`).

## 3. Power and bootstrap [rockbox] `lcd-video.c`

| Step | Action |
|---|---|
| Pins | `GPO32_ENABLE |= 0xC000`; clear `GPIOC_ENABLE` bit 7; GPIOC bit 6 as input (BCM interrupt, unused); `GPO32_ENABLE &= ~1` |
| Power | `GPO32_VAL |= 0x4000` (bit 14); wait 50 ms. Keep it off at least 50 ms before powering again |
| Straps | `STRAP_OPT_A &= ~0xF00`; write `0x1313` to `0x7000_0040` |
| Handshake | wait `ALT_CONTROL` bit 7 clear then bit 6 set; write bytes `A1 81 91 02 12 22 72 62` to `BCM_CONTROL`, then `02 12 22 72 62` to `BCM_ALT_CONTROL`; wait for `RD_ADDR & 1` and `ALT_RD_ADDR & 1`; dummy-read both `WR_ADDR`s; wait the `ALT_CONTROL` bits again |
| Image | write address 0, stream the BCM boot image into its SRAM |
| Start | write BCM `0x1F8` (command word) = 0; write `0xC000_0000` to BCM `0x1000_0C00`; poll it for bit 0; write 0 to it, then `0xA5A5_0002` to BCM `0x1000_0400`; wait for the command word to become non-zero |

The boot image Rockbox uses is Apple's, found in the boot flash's directory (`boot.md` §2, image id `vmcs`). At a cold boot Apple's ROM has already powered and started the BCM before any disk image runs, so Rockbox finds `GPO32` bit 14 set and skips the bootstrap [rockbox], [silicon] (every session since S9). Our own BCM image replaces this step in our firmware [ours].

**Power-down** [rockbox]: backlight off; read BCM `0x1000_1400`, clear bits `0xF0`, write back (purpose unknown); command 8 (LCD sleep); command 12 (purpose unknown); `GPO32_VAL &= ~0x4000`. After that a full bootstrap is needed.

## 4. Drawing (Apple's BCM image) [rockbox]

Commands are written to BCM address `0x1F8` as `(~x << 16) | x`, followed by the doorbell (`CONTROL = 0x31`). The BCM is busy while the command word still reads the command or `0xFFFF`.

| x | Command |
|---|---|
| 0 | LCD update (whole frame from the frame buffer) |
| 1 | self-test (under 40 s) |
| 2, 3 | TV: PAL/NTSC bitmap |
| 5 | update rectangle (more unknown behaviour) |
| 8 | LCD sleep (any update wakes it) |
| 12 | shutdown-related (unknown) |
| 14 | TV: Macrovision off |

**Frame buffer:** BCM address `0xE_0000`, RGB565, 640 bytes per row. To update a rectangle: round x and width to even (the BCM ignores small unaligned writes), write the address `0xE_0000 + 640·y + 2·x`, stream the pixels (one address for a full-width band; otherwise one address per row), then command 0. Rockbox kicks updates from a tick task when the BCM is idle and treats an update still busy after 50 ms as stalled. The first update after a wake also initialises the panel and takes about 200 ms; ours measured 279.8 ms from power-up to the first frame (S30–S34) [silicon].

## 5. Throughput [silicon]

| Method | PP 30 MHz | PP 80 MHz | Source |
|---|---|---|---|
| CPU, 2-register `ldm/stm` | 26,957 µs per 153,600-byte frame (5.7 MB/s) | 9,629 µs (15.9 MB/s) | S8 |
| CPU, 8-register burst | 27,377 µs | 9,909 µs | S8 |
| DMA channel 3 | 27,601 µs | 10,375 µs | S8 |
| per-row overhead (address + wait) | 7.4 µs | – | S8 |

Into BCM **SRAM** with the BCM at 27 MHz (S8), the cost is about 10.5 PP cycles per 16-bit transfer at either PP clock: there the PP's side sets the rate. A full-screen update therefore costs about 27 ms of CPU at the normal 30 MHz — the reason Rockbox's rules prefer partial updates and full-width bands. Into BCM **SDRAM** during video, with the PP at 80 MHz, our player's CPU copy measured 17–18 MB/s of port busy time with the BCM at 150 MHz and 10–12 MB/s with the BCM at 27 MHz (S33, S34, boosted matrix rows, `video: port KB, MB/s` lines) [silicon]: so with an SDRAM target the BCM's clock matters too. Whether that comes from SDRAM against SRAM, from the decoder's load, or from the BCM clock alone is not settled (`../data-paths.md` DP-1; BCM `open-questions.md` #47). Unboosted matrix rows also logged 16 MB/s, but the PP had been boosted by the codec when each began, so they do not pin the 30 MHz rate. DMA details and its failures are in `dma.md` §4.

## 6. Backlight

Owned by the PP, not the BCM: `gpio-i2c.md` §3.1.
