# Appendix A. Sources

## Device sessions (evidence tag [silicon])

All on one iPod Video 5.5G (panel ID 3, model variant 1), the BCM started by Rockbox's host-side bootstrap from power-off, running our own VideoCore II images. Each session's raw log is kept with the unit's backups; the summaries below list only what each session established about the chip.

| ID | Date | What it established |
|---|---|---|
| S1 | 2026-10-09/10 | Entry at `0x200` works for a host-started image. Panel ID 3 read from GPIO. `0x10001400` reads `0xB5` with Apple's firmware running. The 16-bit range `0x7000–0x77FF` is a pre-decrement load/store, not a compare: an image using it faulted |
| S2, S3 | 2026-10-10 | The exception model: misaligned access → vector 1, imprecise, load completes from the rounded-down address, pre-decrement writes back. The 1 MHz counter does not run after a host start. Vector stores from rows other than 0 lost (later explained, S5). Core near the crystal's speed |
| S4 | 2026-10-11 | The counter's clock (`0x10001C4C`/`0x54`) starts it; it is a true 1 MHz. First picture drawn by our image (panel 3): colour, byte order, orientation, window. DMA completion status value `0x2300`. The 48-bit vector `imm6` is signed |
| S5 | 2026-10-11 | DMA to the LCD works (completion bit 8). The LCD run-length register counts up. The 48-bit `vst16` field layout. `vmulhrs` into 8 bits does not clamp |
| S6 | 2026-10-11 | The LCD bus runs at 27 MHz after a host start, one bus cycle per timing-word cycle, no per-pixel overhead. `vmulhrs` ties round away from zero. A 32-bit self-branch in the last word of a 16-byte line runs at half speed |
| S7 | 2026-10-11 | Apple's PLL sequence takes the core from 27 MHz to 150 MHz (×5.56 on every kernel). LCD frame time formula. Bench REP timings. Wake timings |
| S8 | 2026-10-11/12 | The 150 MHz switch with the panel running, and back down (pass flags written; detailed figures not in this manual) |
| S9 | 2026-10-12 | First SDRAM bring-up: failed (sequence differed from Apple's in four ways) |
| S10 | 2026-10-12 | SDRAM up on the second attempt (mode field 4), at 150 MHz; host-port write/readback through `0xC000_0000` correct |
| S11 | 2026-10-13 | SDRAM size 4 MB by address aliasing; 64 KB grid correct; vector row load from and store to SDRAM correct |
| S12 | 2026-10-13 | Host port under load: 0 handshake timeouts, longest wait 47 µs; a 32-bit word read through the port can be torn by a concurrent writer |
| S13, S14 | 2026-10-13 | Vector kernels overwrote low SRAM (`0x004–0x1FF`); traced in S15 |
| S15 | 2026-10-13 | A vector memory **base** field of 7 reads as "no register" (address 0). H.264 decode at about 7 ms per 320×240 frame with vector kernels at 150 MHz, 31–35 ms all in scalar C |
| S16 | 2026-10-13 | 16 scalar arithmetic cases (signed shifts, multiply, divide, sign-extending loads, compares, clipping) all correct. Decoded pictures still differ from the reference (open) |
| S17 | 2026-10-13 | The 32-bit offset `ldsh` loads zero-extended (`quirks.md` #16) |
| S18 | 2026-10-13 | All-C decode bit-exact against ffmpeg on silicon; the vector transform's rows all read row 0 |
| S19–S22 | 2026-10-13 | VRF and per-kernel dumps: plain vector loads align down to their size and horizontal B `+rN` to 16 bytes (#17); `vstr` stays in its line (#18); scalar B r7 reads 0 (#19). S22 also: audio started while the image booted, picture 0 about 520 ms late |
| S23 | 2026-10-13 | Colours right; picture 0 exactly 200 ms late from an audio clock extrapolated before the PCM started |
| S24, S25 | 2026-10-13 | Chroma exact (#19 confirmed), then every pass exact at about 8 ms a picture; the audio clock's start-up leap (about 430 ms, 100 ms in) |
| S26 | 2026-10-13 | `vmulhr` reads B signed (#20); the feeding thread starved by the codec's boost (`host-driver.md` §5) |
| S27 | 2026-10-13 | Every kernel on the vectors, bit-exact against ffmpeg: 7 ms a picture (synthetic), 18 ms (real footage); no start stall |
| S28 | 2026-10-13 | First real converted files from the menus: a first-NAL refusal, about 7.5 s with no picture at a file's start, pictures 1.5–3.5 s late after jumps (`host-driver.md` §8; `open-questions.md` #29, #30) |
| S29 | 2026-10-13 | Hidden catch-up pictures fix late jumps (drift 15–29 ms); sessions started at sample 0 show nothing. `session 29b` is a second copy of the same run's logs |
| S30 | 2026-10-13 | Stage 7, 8 of 8 steps passed; session start timings; a still at sample 0 with errors per packet; stalls cured only by a reload; a deterministic held frame (`open-questions.md` #30–#33) |
| S31 | 2026-10-13 | The feeding thread starved at the PP's 30 MHz; the CPU's copy into the SDRAM FIFO read back 0 wrong (8,869 words in one session). |
| S32 | 2026-10-13 | SDRAM up on attempt 2 at 27 MHz as well (`0x5344_0231`, also S33, S34). The PP's DMA feed into the SDRAM FIFO read back 77% wrong (first word of each block missing, later words shifted). GPIO 2 and 3 move with the PP's audio: the BCM sits on the WM8758's I²S clocks (`audio.md` §5) |
| S33 | 2026-10-13 | The PP's DMA: word 0 of every block still never landed after waiting for the port between blocks |
| S34 | 2026-10-13 | DMA lab: PP DMA into the SDRAM FIFO landed nothing at any size from 256 B to 32 KB; the CPU's copy 0 wrong at about 16 MB/s of port time (`memory-access.md` §7b, §9) |

### Bench sittings (2026-10-25 and after)

Since 2026-10-25 device work runs as numbered **bench sittings**: the shell's bench session steps through checks on the unit and writes a results file and a log per sitting. They are cited as "sitting N, date, build, session log"; the folders are never cited by path. The summaries list only what each sitting established about the chip.

| Sitting | Date | What it established |
|---|---|---|
| 1 | 2026-10-25 | The port write check: a write at 2 mod 4 lands 2 bytes low (`quirks.md` #34); the 27 MHz clock gate that looked like a decoder regression (`programming.md` §12) |
| 2 | 2026-10-25 | The alignment pass (ALGN): misaligned loads and stores, the port's halfword writes, DMA sub-word units (`quirks.md` #35–#38; `open-questions.md` #5, #68–#70); the merged image's job self-test, `J_VPU` 3 and the drift on the chip; `COMMIT` fixed (#67) |
| 3 | 2026-10-26 | The warm swap parks in both directions, 9 of 10 swaps ready (`quirks.md` #41); the UI on the second core (`../data-paths.md` D15) |
| stage 3d | 2026-10-26 | The software 3D renderer: frames CRC-equal to the simulator at 27 and 150 MHz; the tearing-effect line at 67.8 Hz (`quirks.md` #52; `../research/bcm2722/render3d.md`) |
| 4 | 2026-10-26 | The loop filter's seven vector forms agree with the C (`quirks.md` #51); three silent freezes, which led to the bounded-wait rule (`host-driver.md` §2a) and the hang record |
| 4b | 2026-10-26 | The first-use video check failed at every band: a late SDRAM write before a `FEED` (`quirks.md` #47); a stage play hands the chip to Apple's program (`quirks.md` #48) |

## Apple's reference corpus (evidence tag [apple])

From Apple's public iPod Video ("iPod_25") updater bundles. None of these files is redistributed; only names, addresses, constants and hashes appear in this manual.

| Version | Bundle | SHA-1 |
|---|---|---|
| 1.2.1 | `iPod_25.1.2.1.ipsw` | `47c59c9781f5fc5bf8c393d53f86ace6634c6c43` |
| 1.2.3 | `iPod_25.1.2.3.ipsw` | `6ef8101c4740b61da2b27cf46f14729e960a340d` |
| 1.3 | `iPod_25.1.3.ipsw` | `0f2dd984c5d56475bdf3c14b4bc7541b799f2c85` |

The firmware partition holds `osos` (the ARM OS, loaded at host `0x1000_0000`) and `rsrc` (a FAT16 volume). `rsrc:/Resources/VideoCore/` holds, for 1.3:

| File | Bytes | SHA-256 |
|---|---|---|
| `Boot/vmcs.bin` | 201,376 | `cde4b5a9c9a020b6e23de1be0492d9a853b7d974c4060462d5f57a80e47f55be` |
| `Boot/RenderServer.bin` | 104,540 | `a75c4c2060e6e9d4e85e31ac7ab6f15ce123805e06874c09fc3de3aea2e3e001` |
| `Library/h264dec.vll` | 106,960 | `3915ec9295dc0d9cf9405810a66788e5b544c819347e05a350df6857120a9e8f` |
| `Library/mpg4dec.vll` | 147,232 | `08ba2f33f267803746c3b3de025e615f4a4b899611d7a4e3eea819f39d3697fe` |
| `Library/aacdec.vll` | 52,664 | `0a8c63080120a8af36a2d68ed4ade7eae81eee15f1e344f66cd49a508162c2cc` |
| `Library/mplayer.vll` | 51,620 | `c100e0cb0782277f81d2b9bceb29340df70d2993e34f77248a8e3c1979bc1fc6` |
| `Library/passthruhandler.vll` | 6,528 | `0b657b1a4b50198da3ad3d55ecf9d239ffbae8d7da7b3aaca138dcdae5b6d019` |
| `Library/slideshow.vll` | 47,940 | `3d620d9c6b9bc6884a4cc597e5fad254c09d4f7e5c6275cba4faf2adceea303e` |

- `vmcs.bin` and `h264dec.vll` differ between 1.2.x and 1.3; the other files are identical. The start-up register sequence of `vmcs.bin` is identical in all three versions (access for access over the first 400,000 instructions in simulation).
- The `.vll` files are ELF32 little-endian, `e_machine` 95 (`EM_VIDEOCORE`), `ET_DYN`, linked by "MetaWare Universal Linker v5.3h", with `.dynsym` and `.rela` intact. They were the ground truth for instruction lengths (`isa-scalar.md`).
- A second, smaller `vmcs` (101,728 bytes on the unit siggifly examined) lives in the iPod's NOR flash (`flsh` directory at host `0x200F_FE00`, id `'vmcs'`). It is the image Apple's boot ROM and Rockbox upload at power-on. It was not examined for this manual beyond what the cited sources say.

## Rockbox (evidence tag [rockbox])

- `firmware/target/arm/ipod/video/lcd-video.c`: the host-side register map, the bootstrap, the upload and start sequence, the command word at `0x1F8`, `BCM_CMD()`, power-down. Upstream Rockbox, GPL v2+.
- Rockbox tracker **FS#9787** (2009), archived at <https://web.archive.org/web/20240228164605/https://www.rockbox.org/tracker/task/9787>: identifies the `.vll` files as ELF libraries loaded into the BCM and notes that the instruction encoding was unknown.
- Rockbox tracker **FS#9890**: powering the BCM down with the backlight to save battery (the `lcd_sleep()`/`lcd_awake()` path).
- FS#6721: the NOR flash directory reader used to find the `vmcs` section.

## External (evidence tag [external])

- **Broadcom, *BCM2722 Product Brief*, document 2722-PB01-R, 2005-10-18.** Copy at <https://www.curiousdog.org/Steve/assets/pdf/BCM2722_PB.pdf> (cited via the siggifly notes; facts used: 150 MHz dual-ALU VideoCore II, 128 Kbit instruction and data caches, 1.25 MB SRAM + 4 MB SDRAM (10 Mbit on-chip SRAM + 32 Mbit stacked SDRAM), memory-mapped host interface with a "dual software channel", main and sub LCD, PAL/NTSC composite/S-video output, software codecs, JPEG, DRM/AES).
- **Alphamosaic Ltd / Broadcom patents**, all priority 2001-10-31, describing the vector processor and its two-dimensional register file:
  - US 7,036,001 B2, *Vector processing system* (inventors Barlow, Bailey, Ramsdale, Plowman, Swann): 16 parallel 16-bit pixel units, 32 scalar 32-bit registers, a 2-D vector register file addressed in groups of 16; 80-bit full and 48-bit compact vector encodings; scalar encodings of 16 bits with 32- and 48-bit variants.
  - US 7,130,985 B2, *Parallel processor executing an instruction specifying any location first operand register and group configuration in two dimensional register file*: horizontal, vertical and neighbourhood reads of the 2-D file.
  - US 7,200,724 B2, *Two dimensional data access in a processor*: start cell plus layout addressing; diagonal bit lines for row and column access.
  - US 7,080,216 B2 and US 7,107,429 B2, *Data access in a processor*: row/column group access; coordinates that wrap at the array edges.
  - US 7,043,618 B2, *System for memory access in a data processor*: 2-D register memory with wrap-around coordinates and group access relative to a start cell.

  The patents describe the architecture family; their encodings are not VideoCore II's (`isa-vector.md`).
- **siggifly, ipod-emulator research notes** (CC BY-SA 4.0), especially note 03 (the host port, the boot ROM's upload of the NOR `vmcs`, the address latch), note 11 (the VideoCore runtime: the product brief, the `vmcs.bin` export table, `gencmd`, the transport header at `0x1F0`) and note 12 (Apple's ARM-side transport: channel directory, ring records, framing), plus note 14 (the boot ROM's rectangle command). These were derived by emulating the host side; the BCM itself was modelled, not executed. Paraphrased here, never copied.
- **VideoCore IV** public material (Raspberry Pi BCM2835 peripherals documentation; the community VC IV toolchain `puppeh/vc4-toolchain` and `gcc-vc4`): used only for analogies, each marked. VideoCore II is not binary-compatible with VideoCore IV.

## Tools

Our own, GPL: `vc2dis` (table-driven disassembler and assembler), `vc2sim` (scalar simulator with a peripheral model), `vc2vpu` (vector-unit model), `vc2cc` (translates VideoCore IV GCC output to VideoCore II). Simulator statements in this manual are always about Apple's code running in it, and carry the [apple] tag; the simulator's own behaviour is never cited as evidence about silicon.
