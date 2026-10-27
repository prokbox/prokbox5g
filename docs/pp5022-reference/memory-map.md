# Memory map

All addresses are **physical** PP5022 addresses unless marked "logical". The chip has no MMU; a small table of remap windows (`MMAP0`–`MMAP7`) in the cache controller provides the only address translation, and Rockbox uses it to move SDRAM down to logical address 0.

## 1. The physical map

| Range | Size | What | Cached? | Evidence |
|---|---|---|---|---|
| `0x0000_0000`–… | – | At reset: the boot flash ROM (also visible at `0x2000_0000`); Apple's ROM runs from here and IRAM. After a remap: SDRAM, logical (MMAP0) | per `CACHE_MASK` | [external] siggi `03`, `07` (the ROM reads its own configuration at `0x405C` while running); [rockbox] `hwcompat.h` reads `0x0000_405C` before the remap, `0x2000_405C` after |
| `0x1000_0000`–`0x13FF_FFFF` | 64 MB max (32 MB on 30 GB units) | SDRAM, physical. Rockbox treats this range as the **uncached alias** of logical `0x0000_0000` | no | [rockbox] `pp5020.h` `DRAM_START`, `NOCACHE_BASE`; `system-target.h` `UNCACHED_BASE_ADDR` |
| `0x2000_0000`–`0x200F_FFFF` | 1 MB | Boot flash ROM (NOR). `ROM_START 0`, `FLASH_SIZE 0x100000` in the iPod Video config | yes (within `0x2000_0000`–`0x23FF_FFFF` cached window) | [rockbox] `config/ipodvideo.h`, `system-pp502x.c` |
| `0x3000_0000`–… | – | BCM2722 host port (the LCD/video bridge), a 16-bit memory-mapped interface | no | [rockbox] `lcd-video.c`; detail in `display.md` and `../bcm2722-reference/host-interface.md` |
| `0x4000_0000`–`0x4001_FFFF` | **128 KB** (PP5022/PP5024); 96 KB on PP5020 | IRAM (on-chip SRAM), single-cycle, **shared by both cores** | no | [rockbox] `boot-pp.lds` `IRAMSIZE 0x20000` (PP5022), `0x18000` (PP5020) |
| `0x6000_0000`–`0x6000_FFFF` | – | Core-local and system control: processor ID, mailboxes, interrupt controller, timers, device enable/reset, clocks, processor control, DMA, GPIO, cache control | no | [rockbox] `pp5020.h` |
| `0x7000_0000`–`0x7000_FFFF` | – | Peripherals: chip ID and strap, device init, GPO32, I2S, LCD bridges, UARTs, PWM, I2C, clickwheel serial | no | [rockbox] `pp5020.h` |
| `0xC300_0000`–`0xC300_0FFF` | – | IDE/ATA controller | no | [rockbox] `pp5020.h`, `ata-target.h` |
| `0xC500_0000`–… | – | USB OTG controller (ARC/ChipIdea) | no | [rockbox] `pp5020.h` `USB_BASE` |
| `0xC600_0000`–… | – | FireWire controller (present in silicon; unused on the iPod Video, which has no FireWire PHY wired for data) | no | [rockbox] `pp5020.h`; [hypothesis] for "unused" |
| `0xF000_0000`–`0xF000_FFFF` | – | Cache controller: data array, status arrays, flush/invalidate windows, `MMAPn` remap registers, `CACHE_MASK`/`CACHE_OPERATION` | no | [rockbox] `pp5020.h` |

**Mirrors** [external] siggi `08`, `16` (read on hardware under Rockbox): most register pages repeat inside their 4 KB page (the timer block every `0x20`, `0x6000_6000` and `0x6000_7000` every `0x100`, GPIO every `0x200`), and unused words read a filler pattern `0xCACA_D0D0`. The whole `0x6000_0000` window is also reachable at `0x6400_0000` (iPodLinux's kernel reads the interrupt controller there). Don't rely on mirrors; decode them only to recognise a stray pointer.

Ranges not listed are unknown. Nothing is known about what an access to an unmapped address does (abort, wrap or silent read of 0) — see `open-questions.md`.

## 2. SDRAM

| Item | Value | Evidence |
|---|---|---|
| Physical base | `0x1000_0000` | [rockbox] |
| Size | 32 MB (5G/5.5G 30 GB) or 64 MB (60 GB / 80 GB). The owner's unit (MA450, 5.5G 80 GB) has 64 MB | [rockbox] `crt0-pp.S`; [ours] `docs/hardware.md` |
| Size detection | aliasing test before the remap: write `64` to the last byte of the first 32 MB bank (`0x11FF_FFFF`), then `32` to the last byte of the second bank (`0x13FF_FFFF`); read `0x11FF_FFFF` back. On a 32 MB unit the second write aliases onto the first, so the byte reads 32 | [rockbox] `crt0-pp.S` (`#if defined(IPOD_VIDEO)`); read back as `probed_ramsize` from logical `0x01FF_FFFF` in `system_init()` |
| Controller | Apple's boot ROM configures the SDRAM controller before any loaded image runs. Rockbox leaves its geometry alone, with one exception: USB init ORs `0x47A` into `XMB_RAM_CFG` (`0x7000_003C`) after a 100 ms wait (`usb_init_device()`, `firmware/target/arm/pp/usb-fw-pp502x.c` :120; the bits' meaning is undocumented) [rockbox]. (Corrected 2026-10-22, P18 brief review: the earlier "never touches it" was wrong.) Per the emulator notes, the ROM's routine (only for "memory type 2") clears bits `[11:8]` of `0x7000_0030` and sets `0x1_0400`, writes `XMB_RAM_CFG` (`0x7000_003C`) = `0x2007_16D0` (geometry), sets bit 24 (go) and polls bit 31 (done), then probes for address aliasing by writing `0x1000_0040`, `0x1000_0840`, `0x1000_0440`, `0x1000_0240`, puts the result in bits `[17:16]`, and repeats go/done [external] siggi `04` ("Retiring #2"), from Apple's ROM under emulation. Not verified on silicon by anyone we know of |
| Parts | Samsung K4M51163PC (64 MB, photographed), K4M56163PG (32 MB, unverified), Hynix parts also reported | [external] siggi `05` |

Rockbox uses the 32/64 MB difference for one other thing: it picks the stock battery capacity (400 mAh thin 30 GB, 600 mAh thick 60/80 GB) from the probed RAM size [rockbox] `config/ipodvideo.h`, `battery_default_capacity()`.

## 3. The remap windows (MMAP)

Eight windows, each a pair of registers:

| Register | Address |
|---|---|
| `MMAPn_LOGICAL` | `0xF000_F000 + 8n` |
| `MMAPn_PHYSICAL` | `0xF000_F004 + 8n` |

`n` = 0..7; `MMAP_FIRST` = `0xF000_F000`, `MMAP_LAST` = `0xF000_F03C`. [rockbox] `pp5020.h`

**Field layout** as far as Rockbox's use reveals it [rockbox] (meanings partly [hypothesis]):

- `LOGICAL`: bits `[31:16]` logical base (64 KB granularity is implied); bits `[15:0]` an address **mask** for the window size. Rockbox writes `0x0000_3C00` (MMAP0, 64 MB build) and `0x0000_3E00` (32 MB build); for MMAP1 `0x2000_3C00`.
- `PHYSICAL`: bits `[31:16]` physical base; low bits hold flags:

| Bit | Name (Rockbox) | Meaning |
|---|---|---|
| 8 (`0x0100`) | `MMAP_PHYS_READ_MASK` | reads allowed through the window |
| 9 (`0x0200`) | `MMAP_PHYS_WRITE_MASK` | writes allowed |
| 10 (`0x0400`) | `MMAP_PHYS_DATA_MASK` | data accesses |
| 11 (`0x0800`) | `MMAP_PHYS_CODE_MASK` | instruction fetches |
| `[7:0]` | – | Rockbox always writes `0x84` here; meaning unknown |

**What Rockbox programs** [rockbox]:

| Window | `LOGICAL` | `PHYSICAL` | Effect | Who |
|---|---|---|---|---|
| MMAP0 | `0x0000_3C00` (64 MB) / `0x0000_3E00` (32 MB) | `0x1000_0F84` (= `0x0000_0F84` ORed with the execution address' top byte, `0x10`) | SDRAM appears at logical `0x0000_0000`; physical `0x1000_0000` stays reachable uncached | `crt0-pp.S`, run by **each core in turn** from IRAM |
| MMAP1 | `0x2000_3C00` | `0x0000_3084` + R/W/D/C flags = `0x0000_3F84` | per the source comment, keeps the flash ROM mapped for the CPU and hidden from the COP; the exact effect of a physical base of 0 here is not understood | `system_init()`, CPU only |

The remap registers are **per core** in effect: crt0 makes each core write MMAP0 for itself (CPU first, then COP), and the MMAP1 comment says it is set "on CPU, keep hidden from COP". [rockbox] Whether the registers are banked per core or the windows carry a core-select bit is not known ([hypothesis]: banked, as the cache status arrays are).

**Rules for doing the remap yourself** [rockbox] `crt0-pp.S`:

1. Never rewrite MMAP0 while executing from SDRAM. Copy the few instructions that write the two registers to IRAM (`0x4000_0000`) and jump there.
2. Code before the remap runs at its physical address (`0x1000_0000 + offset`); it must be position-independent or compute addresses from `pc & 0xFF00_0000`.
3. After the remap, jump to the logical (link) address.
4. Before handing control to another image (Rockbox's `rolo`), disable the cache and **zero all sixteen MMAP registers**, then jump to the physical `0x1000_0000`.

## 4. IRAM

| Item | Value | Evidence |
|---|---|---|
| Base / size | `0x4000_0000`, 128 KB (PP5022) | [rockbox] `boot-pp.lds` |
| Cached? | No: outside both `CACHE_MASK` windows | [rockbox] `init_cache()` comment |
| Shared? | Yes, one array seen identically by CPU and COP | [rockbox]: `IBSS_ATTR` data is used for CPU/COP exchange (e.g. the MP3 codec's synth buffers on the COP, `mpa.c`; `cpu_message`/`cpu_reply` in `rolo.c`) |
| Speed | single-cycle (no wait states) [external] iPodLinux; Rockbox puts its hottest loops (FIQ handler, codec inner loops, `cache_invalidate_special`) there | [rockbox] `ICODE_ATTR` use |

**The last 256 bytes** (`0x4001_FF00`–`0x4001_FFFF`) are the boot ROM's hand-off area: the `IsyS` system-information pointer at `0x4001_FF18`/`0x4001_FF1C` and the disk-mode request string at `0x4001_FF00` (`boot.md` §3). Rockbox's power-off clears `0x4000_C000`–`0x4001_FFFF` so Apple's firmware sees no stale boot-failure flag [rockbox] `power-ipod.c`. On 96 KB parts the same area is at `0x4001_7F00` [external] siggi `02`.

How Rockbox splits it (a convention, not hardware) [ours]/[rockbox]: core 48 KB at `0x4000_0000`–`0x4000_BFFF` (`app-pp.lds` `IRAMSIZE 0xc000`: vectors are *not* here, `.ibss`, `.iram`, idle stacks, an 8 KB main stack); codecs and plugins 80 KB at `0x4000_C000`–`0x4001_FFFF` (`plugin.lds`). Rockbox's bootloader is linked to run **from IRAM** (`boot-pp.lds`: `.text` and `.data` at `0x4000_0000`, an 8 KB stack after them, `.bss` at physical SDRAM `0x1100_0000`, 16 MB in), so all its addresses are uncached (`UNCACHED_ADDR(a) = a` in bootloader builds); `crt0-pp-bl.S` copies it there (`boot.md`).

## 5. Rockbox's logical layout (64 MB, for orientation) [rockbox] `app-pp.lds`

| Logical | What |
|---|---|
| `0x0000_0000` | exception vectors (copied by crt0 from the image) then `.text`, `.rodata`, `.data`, `.bss` |
| `_end` … `ENDAUDIOADDR` | audio buffer |
| `MEMORYSIZE·1 MB − PLUGIN − CODEC` | codec slot (1 MB) |
| top | plugin slot |
| `0x1000_0000 + x` | `.ncdata`/`.ncbss`: data placed at the uncached alias of its own logical address, so both cores see the same bytes without cache maintenance |

**ProkBox shell (`HAVE_PANIC_RECORD`)**: the last `PANIC_REC_RESERVE` (256) bytes before the codec buffer are not part of the audio buffer; they hold the panic record (`firmware/export/panic_record.h`), accessed only through the uncached alias (fw-ui: physical `0x13EF_FF00`; on a 32 MB unit `0x11EF_FF00`). Nothing in crt0, the bootloader or the ROM's `osos` load writes there ([rockbox], code reading); survival across each reset is [hypothesis] (`quirks.md` 48). [ours]

## 6. The cache-controller window (`0xF000_0000`)

| Range | What | Evidence |
|---|---|---|
| `0xF000_0000`–`0xF000_1FFF` | cache data array (8 KB) | [rockbox] `CACHE_DATA_BASE` |
| `0xF000_2000`–`0xF000_3FFF` | mirror of the data array | [rockbox] |
| `0xF000_4000`–`0xF000_5FFF` | CPU cache line status words | [rockbox] |
| `0xF000_6000`–`0xF000_7FFF` | COP cache line status words | [rockbox] |
| `0xF000_8000`–… | "flush" window | [rockbox] `CACHE_FLUSH_BASE` (unused by Rockbox) |
| `0xF000_C000`–… | "invalidate" window | [rockbox] `CACHE_INVALID_BASE` (unused) |
| `0xF000_F000`–`0xF000_F03C` | `MMAP0..7` | [rockbox] |
| `0xF000_F040` | `CACHE_MASK` | [rockbox] |
| `0xF000_F044` | `CACHE_OPERATION` | [rockbox] |
| `0xF000_F048` | `CACHE_FLUSH_MASK` | [rockbox] (unused) |

The cache itself (`CACHE_CTL` at `0x6000_C000`) is described in `cores-and-threads.md` §3.
