# 5. Host interface

The BCM2722 appears to its host as a memory-mapped peripheral with a 16-bit data bus and two independent register banks, which the product brief calls a "dual software channel" [external]. Through it the host can read and write **any** BCM address: SRAM, SDRAM and peripheral registers.

## 1. Host side (PP5022)

### Register banks

[rockbox] `lcd-video.c`; the bank layout also appears in Apple's `osos` driver (about host `0x1028_78F0–0x1028_7DD0` in 1.3) [apple].

| Host address | Main bank | Host address | ALT bank |
|---|---|---|---|
| `0x3000_0000` | `DATA` | `0x3004_0000` | `ALT_DATA` |
| `0x3001_0000` | `WR_ADDR` | `0x3005_0000` | `ALT_WR_ADDR` |
| `0x3002_0000` | `RD_ADDR` | `0x3006_0000` | `ALT_RD_ADDR` |
| `0x3003_0000` | `CONTROL` | `0x3007_0000` | `ALT_CONTROL` |

- The BCM register is selected by host address bits 16–18; the low address bits are not decoded, so a 32-bit host access (and `ldm`/`stm` bursts) to `DATA` works, arriving as two 16-bit transfers [rockbox].
- A 32-bit address written to `WR_ADDR`/`RD_ADDR` arrives as two halfwords to the same register, **low half first**; the halves are told apart by order [external: siggifly note 03, from the boot ROM's traffic; consistent with Rockbox's 32-bit writes].
- `DATA` auto-increments: after one address write the host streams consecutive halfwords [rockbox; silicon: every upload].
- Rockbox uses only the main bank after the bootstrap. Apple's `osos` selects a bank per call and keeps **one lock per bank**. It sends the display's bulk pixel writes through the ALT bank and everything else (bootstrap, mailbox, cache flush, start, gencmd rings) through the main bank [apple] (`brief-map-power.md` §7). So the two banks are two software channels with their own address latches. Whether the latches are independent in hardware is stage PW7.
- Apple's write path does not wait for the write-ready bit (`CONTROL & 0x02`) after setting the address; Rockbox does [apple; rockbox]. Whether the wait is needed is untested.
- The brief's "3-bit address": the register select on host address bits 16–18, two banks × four registers [hypothesis, strong].

### Handshake bits

| Register | Bit | Meaning | Evidence |
|---|---|---|---|
| `CONTROL` | `0x02` | Write-ready: the address just written to `WR_ADDR` has been accepted | [rockbox] |
| `CONTROL` | `0x10` | Read-ready: data for the address written to `RD_ADDR` is available in `DATA` | [rockbox]; [apple] `osos`'s read loop (host `0x1028_7A6C`) waits for it per word |
| `RD_ADDR` (read) | `0x01` | Ready to accept a read address | [rockbox] |
| `ALT_CONTROL` | `0x80` | Busy: wait for clear during the bootstrap | [rockbox] |
| `ALT_CONTROL` | `0x40` | Ready: wait for set during the bootstrap | [rockbox] |
| `CONTROL` (write) | `0x31` | Rockbox writes this after each command word ("kick") | [rockbox]; whether it raises the BCM's doorbell interrupt is [hypothesis] |

### Primitive accesses

[rockbox] (the reference implementation; unbounded waits in stock Rockbox, bounded in ours):

```
write32(addr, v):  WR_ADDR32 = addr;  wait CONTROL & 0x02;  DATA32 = v
read32(addr):      wait RD_ADDR & 0x01;  RD_ADDR32 = addr;  wait CONTROL & 0x10;  return DATA32
block write:       WR_ADDR32 = addr;  wait CONTROL & 0x02;  stream halfwords/words to DATA
```

Silicon notes:

- **Torn words.** A 32-bit word read through the port while the BCM is writing it can mix old and new halves [silicon S12]. Any shared word that changes while the other side reads it needs a torn-read check (for example store a 16-bit value as `~v << 16 | v` and reject words that fail the check, as Rockbox's `BCM_CMD()` encoding does).
- A dead or unpowered port reads `0xFFFF` per halfword [rockbox comment; silicon: used as the dead-port signal].
- Under sustained load (packets streamed while the BCM decodes) the longest handshake wait seen was 47 µs; 0 timeouts [silicon S12].
- Upload rate from an unboosted PP5022 (~30 MHz) with a 2-register `ldm`/`stm` loop: about 0.25 µs per pixel, about 19 ms for a 150 KB frame [silicon S7, inferred from per-row times].
- Apple's bulk write (host `0x1028_7BE8`) takes a lock, moves 256 bytes and up by the PP5022's own DMA in 64 KB chunks, and writes only the tail by halfword [apple]. Its reads wait for `CONTROL & 0x10` per word as above [apple].

### Starting an image

[rockbox] `bcm_init()`; device-proven with our images [silicon S1–S16].

1. **Power:** `GPO32_VAL |= 0x4000`; wait 50 ms. (`lcd_init_device()` also sets `GPO32_ENABLE |= 0xC000`, makes GPIO C bit 6 an input for the BCM interrupt, and clears GPIO C enable bit 7 once.)
2. **Bootstrap stage 1:** `STRAP_OPT_A &= ~0xF00`; write `0x1313` to host `0x7000_0040`.
3. **Bootstrap stage 2:** wait `ALT_CONTROL & 0x80` clear and `& 0x40` set; write the 8 bytes `A1 81 91 02 12 22 72 62` one at a time to `CONTROL`; write bytes 3–7 (`02 12 22 72 62`) to `ALT_CONTROL`; wait until both `RD_ADDR & 1` and `ALT_RD_ADDR & 1`; read `WR_ADDR` and `ALT_WR_ADDR` once each (values discarded).
4. **Stage 3:** wait `ALT_CONTROL & 0x80` clear and `& 0x40` set again; set the write address to 0 and stream the image (an even number of halfwords).
5. Write `0x1F8 = 0` (Rockbox's ready word, cleared).
6. Flush the BCM cache: `0x10000C00 = 0xC000_0000`; wait for `0x10000C00 & 1`; `0x10000C00 = 0`.
7. Start: `0x10000400 = 0xA5A5_0002`. The core begins at `0x200`.
8. Wait for the image's ready signal (Rockbox: `0x1F8 ≠ 0`).

The meaning of the bootstrap bytes is unknown; they are answered by the BCM's internal boot ROM [hypothesis]. What happens if the host writes a different image size, or starts the core twice, is untested.

**Replacing a running image without a power cut** [ours; vc2sim only, untried on silicon]: the warm swap (`docs/research/bcm2722/warm-swap.md`). The running image parks its core in a stub outside every image (`HP_OP_PARK`). The host uploads the next image as in step 4, does steps 5 and 6, and writes a go word at `0x13C`; the stub then jumps to `0x200`. The start register is not written again, and no GPIO is touched, so the panel stays lit. **On silicon, 2026-10-25:** it never parked on three builds (`quirks.md` #40): the stub, written by the core, ran without the cache flush Apple puts between writing code and running it. Every image now flushes first (Apple's @25a, `boot-and-clocks.md` §2); B13 at the next sitting decides it.

Timing: our 8 KB image reported ready about 2 ms after its upload [silicon S7]; Apple's firmware is back after a power cycle and reload in about 580 ms [silicon S7, fallback timing].

### Interrupt from the BCM

The BCM drives an interrupt line to the host: BCM GPIO 23 [apple] → host GPIO C bit 6 [rockbox]. Apple's firmware drives it high for at least 10 µs, at least 40 µs after it last fell (@1442e) [apple]. Rockbox leaves it unused (its interrupt set-up is commented out). Not yet exercised on silicon.

## 2. BCM side

| Item | What | Evidence |
|---|---|---|
| Host writes | Land in BCM memory at the address given; the core sees them directly with its cache off | [silicon] |
| Host doorbell | Interrupt source 23 (vector 55). Apple's handler acknowledges it by writing `0x10003800 = 1`; Apple also polls the same path from a 10 ms software timer | [apple] @d52a, @d56a; which host write raises it is [hypothesis] (probably `CONTROL = 0x31`) |
| BCM → host | GPIO 23 pulse (above) | [apple] |

No status register on the BCM side for "host is accessing" is known. The host can read and write peripheral registers while the core runs (Rockbox reads `0x10001400`; GPIO, DMA and LCD registers have all been read this way) [rockbox; silicon].

## 3. Protocols on top of the port

The port carries memory traffic only; every protocol is a software convention in BCM memory. Three are known.

### 3a. Rockbox's command word (works with the NOR `vmcs` and with our images)

[rockbox]

| Address | Use |
|---|---|
| `0x1F8` (`BCMA_COMMAND`) | Command word. Commands are written as `BCM_CMD(x) = (~x << 16) \| x` |
| `0x1FC` (`BCMA_STATUS`) | Status (unused by Rockbox) |
| `0xE0000` (`BCMA_CMDPARAM`) | Parameters and data: the 320×240 RGB565 frame |
| `0xC000_0000` | SDRAM; `BCMA_TV_FB`, and `0xC020_0000` `BCMA_TV_BMPDATA` |

| Command | x | Notes |
|---|---|---|
| `LCD_UPDATE` | 0 (`0xFFFF_0000`) | Push the whole frame at `0xE0000`. The first after power-up also powers the panel on (Rockbox allows 500 ms) |
| `SELFTEST` | 1 | never sent by Rockbox |
| `TV_PALBMP`, `TV_NTSCBMP` | 2, 3 | — |
| `LCD_UPDATERECT` | 5 | parameters below |
| `LCD_SLEEP` | 8 | panel off |
| (shutdown) | 0xC | sent before power is cut |
| `TV_MVOFF` | 14 | — |

Busy convention: the host treats `0xFFFF_0000` (its own `LCD_UPDATE` still unconsumed) or `0xFFFF` as busy; after 50 ms of busy it rewrites the command and kicks again. After each command word the host writes `CONTROL = 0x31`.

**`LCD_UPDATERECT` with the NOR `vmcs`** [external: siggifly note 14, from the boot ROM's own traffic in an emulator]: an 8-word header at `0xE0000`, then the pixels at `0xE0020`, tightly packed:

| Word | Value seen | Meaning |
|---|---|---|
| `+0x00` | `0x34` | unknown |
| `+0x04`, `+0x08` | x0, y0 | top-left |
| `+0x0C`, `+0x10` | x1, y1 | bottom-right, inclusive |
| `+0x14`, `+0x18` | 0 | unknown |
| `+0x1C` | w × h × 2 | byte count |

The boot ROM issues commands `0x13`, `0xA`, `0x5`, `0x5` (the last two: a full black fill, then a centred 62×78 logo) [external]. `0x13` and `0xA` are unidentified.

### 3b. Apple's run-time transport (disk `vmcs.bin`)

[apple] plus [external: siggifly notes 11–12, from Apple's ARM side]. Apple's media software uploads the larger `vmcs.bin` from the firmware partition and talks to it through rings, not the command word.

- `vmcs.bin` stores the address of its host-interface block at `0x1FC` (@d4e6) [apple]. The ARM side reads 16 bytes at `0x1F0`, requires word 2 (`0x1F8`) to be exactly 1 and word 3 (`0x1FC`) to be a non-zero, 4-aligned address of a channel directory [external].
- The directory has eight 16-bit slots; a non-zero slot is the byte offset of a 0x50-byte channel record; the slot index is the channel number [external]. A record holds a service tag (1 = `gencmd` text commands, 2 = display, 7 = unknown), TX and RX ring bounds, and four ring pointers each in its own 16-byte block [external].
- On the BCM side, eight channels dispatch callbacks (@d464); host-toggled channel bits sit at block `+16`, the BCM's acknowledgements at `+32`; the BCM → host direction uses `+17`/`+33` and a GPIO 23 pulse [apple].
- Messages: a 16-byte header (magic `0xF1A5_5A1F`, a per-channel 31-bit sequence number, an opcode, a 16-bit unpadded length), then the payload padded to 16 bytes [external].
- The text service carries commands such as `display_control %d model=%d power=%d`, `mp_selectplay …`, `set_vll_dir %s`, with `key=value` replies [apple strings].

How the two descriptions (ours from the BCM side, siggifly's from the ARM side) map onto each other field by field has not been reconciled (`open-questions.md`). The NOR `vmcs`'s use of `0x1F8`/`0x1FC` differs from the disk image's.

### 3c. A worked example: our shared-memory protocol

[ours] Not a property of the chip, but a tested pattern for this port, device-proven in S10–S30. The full video session on top of it (lifecycle, FIFO, sync, failure signatures) is in `host-driver.md`.

- A 64-byte header at `0x100` (version word written last, ring addresses and sizes, indices), a command ring (host → BCM) and an event ring (BCM → host) in SRAM, and a bitstream FIFO in SDRAM.
- Every shared word has exactly one writer. Indices are 16-bit values stored as `~v << 16 | v`, so a torn read is detected and retried.
- The host never waits without a deadline; the BCM never waits on the host.
- Records: a header word `opcode | n << 8 | tag << 16` and n payload words.

## 4. Things a host programmer must not do

- Read a word the BCM may be updating without a torn-read check [silicon S12].
- Use the stock unbounded waits on a port that might be unpowered: the host spins forever [rockbox code; only a reset recovers the host].
- Assume the BCM keeps state across the backlight's sleep: Rockbox cuts its power [rockbox].
- Run the thread that feeds the BCM below the codec thread's priority. Rockbox's pcmbuf boosts the codec (to about 10) while it refills the PCM buffer after every start. A video thread at `PRIORITY_PLAYBACK` (16) then barely ran for about 0.5 s, so pictures were posted late and a GOP was dropped at every start. Keep the feeding thread level with the codec, as upstream keeps the voice thread [silicon S26 trace, fix confirmed S27: the longest loop gap fell from ~500 ms to 12–13 ms]. Only through the refill, and not above priority 8, so a starved PCM still wins (`host-driver.md` §5).
