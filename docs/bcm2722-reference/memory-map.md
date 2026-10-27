# 2. Memory map

Addresses as the VideoCore core sees them. How each memory is reached (widths, alignment, the host's paths, ownership, speed) is in [`memory-access.md`](memory-access.md). All peripheral accesses known so far are 32-bit loads and stores [apple: every access to the LCD, GPIO and timer registers in `vmcs.bin` is a word access].

## Top level

| Range | Size | What | Evidence |
|---|---|---|---|
| `0x0000_0000–0x0013_FFFF` | 1.25 MB | Internal SRAM. Code runs from it; images load at 0 | [apple] `vmcs.bin`'s heap table: internal heap from the end of its image to `0x140000` (words at `0x1fc10`/`0x1fc14`); [external] brief; [silicon] all images |
| `0x0014_0000–0x0FFF_FFFF` | | Nothing known | — |
| `0x1000_0000–0x1000_FFFF` | 64 KB | Peripherals, in blocks of `0x400` (table below) | [apple] |
| `0xC000_0000–0xC03F_FFFF` | 4 MB | SDRAM **as the host addresses it** through the host port. Not known to be a core address | [rockbox] `BCMA_SDRAM_BASE`; [silicon] S10 (host writes here read back at `0xFE00_0000`) |
| `0xFE00_0000–0xFE3F_FFFF` | 4 MB | SDRAM as the core addresses it | [apple] `vmcs.bin` pool table (`0x1fc20`/`0x1fc24`); [silicon] S10, S11 |

Whether `0xFE00_0000` is a cached or uncached alias, and whether other aliases exist (VideoCore IV has several per-cache-policy aliases), is unknown [hypothesis: an alias scheme like VC IV's may exist].

## Internal SRAM conventions

None of these is hardware; they are the conventions every image so far follows.

| Range | Use | Evidence |
|---|---|---|
| `0x000–0x017` | Exception vectors 0–5 (one word each: handler address) | [apple]; [silicon] S2 (vector 1) |
| `0x080–0x0FF` | Interrupt vectors 32–63 (`4 × (32 + source)`) | [apple] |
| `0x018–0x07F` | Unused vector slots 6–31 | [apple] |
| `0x1F0–0x1FF` | Host mailbox area. Rockbox's command word at `0x1F8`; its status word `0x1FC`. Apple's disk `vmcs.bin` stores its host-interface block address at `0x1FC` | [rockbox]; [apple] @d4e6 |
| `0x200` | Entry point of a host-started image | [silicon] S1; [apple] both boot images start here |
| `0xE0000–0x1057FF` | Rockbox's framebuffer / Apple's command-parameter buffer (`BCMA_CMDPARAM`): 320 × 240 RGB565, 640-byte rows | [rockbox]; [external] siggifly note 14 |

## Our video image's layout [ours]

A convention of our images (`bcmfw/config.h`, `bcmfw/video/video.h`; the design is `docs/research/bcm2722/hostproto.md` §7), not hardware. Since M4 of the AAC port (2026-10-13), the video image is laid out as follows.

| Range | Use |
|---|---|
| `0x00_0000–0x03_0033` | the image: vectors, code, tables, data, `.bss` (H.264, display, protocol, and the AAC decoder's code, SRAM tables and hot buffers) |
| `0x03_1000–0x0C_BFFF` | the H.264 decoder's arena (context, per-MB info, frames that fit). Until the first H.264 `OPEN`, its top 48 KB holds the AAC decoder's SDRAM tables, parked there by `crt0` from the load image |
| `0x0C_C000–0x0C_FFFF` | stack |
| `0x0D_0000–0x0D_2FFF` | the host protocol's rings |
| `0x0D_C000–0x0D_F0DF` | the scaler's strip, the self-test's areas, the guard block |
| `0x0E_0000–0x10_57FF` | the host's framebuffer (Rockbox's contract) |
| `0x10_5800–0x12_AFFF` | the converted picture |
| `0x12_B000–0x13_FFFF` | free (84 KB) |

SDRAM offsets (add `0xFE00_0000` for the core, `0xC000_0000` for the host): `0x00_0000` the bitstream FIFO (1 MB); `0x10_0000` the DPB for streams too large for SRAM (2 MB less 16 KB); `0x30_0000` the wrap copy (256 KB); `0x34_0000–0x37_FFFF` the audio rings; `0x38_0000–0x39_5FB3` the AAC decoder's far data (its long windows, inverse-quantisation and bit-reversal tables, then its channel elements; window to `0x39_C000`); `0x39_C000–0x39_FFFF` the AAC test's I/O; `0x3A_0000–0x3F_FFFF` the diagnostic dumps. SDRAM is set up once per boot, at the first H.264 `OPEN`, and keeps its contents across sessions; a diagnostic `OPEN`'s memory test overwrites everything (the AAC decoder then refuses to open until the image is booted again).

## Peripheral blocks

Each block is `0x400` bytes. "pw" = writes need `0xA5A5` in bits `[31:16]`.

| Base | Block | Registers known | Evidence | Details |
|---|---|---|---|---|
| `0x1000_0400` | Run control | `+0x00` (pw): host writes `0x0002` to start the core; Apple's start-up clears bit 0 and later writes `0x0020` twice before the clock switch. `+0x08` (pw): Apple writes `0x0002` at start-up | [rockbox] start; [apple] @b4e, @14506; meanings [hypothesis] | `boot-and-clocks.md` |
| `0x1000_0800` | DMA engine, 8 channels × `0x80` | `+0x00` config/state, `+0x04` status (write 1 to clear), `+0x38 + 0x20·k` sub-channel k control-block address | [apple]; [silicon] S4, S5 | `dma.md` |
| `0x1000_0C00` | Cache control | `+0x00`: bit 31 enable, bit 30 flush request, bit 0 flush done, bit 16 set after SDRAM set-up | [apple]; [rockbox] | `boot-and-clocks.md`, `sdram.md` |
| `0x1000_1000` | Interrupt controller | `+0x00` current level `[2:0]`; `+0x04` pending `(priority << 8) \| vector`; `+0x10–0x1C` 4-bit priority per source, 8 per word | [apple] | `interrupts-timers-gpio.md` |
| `0x1000_1400` | Core regulator / power | `+0x00` (pw) `[3:0]` level, `[7:4]` preserved by Apple, cleared by Rockbox at power-down | [apple]; [rockbox]; [silicon] S1, S7 readbacks | `boot-and-clocks.md` |
| `0x1000_1800` | System timer | `+0x00` match status; `+0x04` 1 MHz counter; `+0x08–0x14` compares 0–3 | [apple]; [silicon] S4 | `interrupts-timers-gpio.md` |
| `0x1000_1C00` | Clocks, PLLs (all pw) | `+0x00–0x0C` PLL A–D divider; `+0x14–0x20` PLL A–D control; `+0x28` core clock select; `+0x2C–0x58` block clocks, `+0x4C`/`+0x54` the counter's clock | [apple]; [silicon] S4, S7 | `boot-and-clocks.md` |
| `0x1000_2800` | GPIO, 52 pins | function select, set, clear, level, event status, edge enables, pulls | [apple]; [silicon] S1–S4 | `interrupts-timers-gpio.md` |
| `0x1000_2C00` | TV out: timing generator | `+0x00` control; `+0x04` FIFO (DMA channel 6 destination); `+0x08` input size (lines per field, samples per line); `+0x0C` output size; `+0x10`, `+0x14` horizontal and vertical timing; `+0x18` mode, bit 31 the field on read | [apple] (run in vc2sim); meanings [hypothesis] | `brief-map-peripherals.md` §2 |
| `0x1000_3400` | SDRAM controller | `+0x10`, `+0x14` timing; `+0x20` command/status; `+0x24[31:16]` refresh interval; `+0x28` mode word; `+0x30` control | [apple]; [silicon] S9–S11 | `sdram.md` |
| `0x1000_3800` | Host interface (BCM side) | `+0x00`: write 1 to acknowledge the host doorbell (interrupt source 23) | [apple] @d56a; [hypothesis] meaning | `host-interface.md` |
| `0x1000_4000` | LCD interface | `+0x00` control; `+0x04` run length; `+0x08` run format; `+0x0C` data FIFO; `+0x10 + 8k`, `+0x14 + 8k` bus timing for chip select k; `+0x178` index; `+0x17C` data | [apple]; [silicon] S4–S7 | `lcd.md` |
| `0x1000_4800` | Audio | `+0x00` control (bit 0, bit 23 polled), `+0x04` FIFO (DMA channel 5 destination), `+0x10`, `+0x14` divider (password `0xA508`) | [apple] | `audio.md` |
| `0x1000_5400` | Video DAC | `+0x04`: bit 0 read (present/ready), bit 5 on, bit 6 625-line, bit 7 S-Video; `+0x84`–`+0xDC` 23 Macrovision parameter words | [apple]; meanings [hypothesis] | `brief-map-peripherals.md` §2 |
| `0x1000_6400` | USB 1.1 device | `+0x00` bit 24 read by `usb_inserted` | [apple] | `brief-map-peripherals.md` §6.2 |
| `0x1000_4C00` | Unknown | `+0x30` written 0 at Apple's start-up | [apple] @1433e | — |

Unlisted blocks between `0x1000_0000` and `0x1000_FFFF` are unknown. Apple's start-up touches no others [apple: `vc2sim --boot` device log of the first 400,000 instructions].

## A special register

`sys19 rd` (a 16-bit system-group instruction, `isa-scalar.md`) reads a value into rd. Apple's clock code reads it before choosing the regulator level and forces level 15 if its low nibble is ≥ 4 [apple]. Meaning unknown [hypothesis: a chip revision or speed grade].
