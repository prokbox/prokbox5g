# 8. DMA engine (`0x1000_0800`)

Eight channels, `0x80` apart (channel n at `0x1000_0800 + 0x80·n`), each with up to three sub-channels that start chains of 32-byte control blocks. Transfers can be 1-D or 2-D and can be paced by a peripheral's request line (DREQ). The shape resembles the later BCM2835's DMA control block, but the register layout and completion reporting differ; BCM2835 names are analogies only.

> **See also [`../data-paths.md`](../data-paths.md)**: which master reaches which memory on this board, what works on silicon, the recipes, and the dead ends (including the PP's DMA into the host port).

## Channel registers

| Offset | Register | Use | Evidence |
|---|---|---|---|
| `+0x00` | Config / state | Written with a word in transfer-info style: LCD `0x0903_2101`, TV `0x0200_FF01`, memcpy `0x0000_E802`. Bit 0 = enable; bits `[20:16]` = DREQ number (3 = LCD); bits `[28:24]` identify the owner when a channel is shared; bits `[31:29]` read back a state (Apple's TV code waits for state 2) | [apple] values; [silicon] S4 read `0x4903_2101` after a completed LCD run (written `0x0903_2101`: state 2); field meanings [hypothesis] |
| `+0x04` | Status | Write 1s to clear. Apple's handler reads it, writes the same value back, and dispatches to registered users by mask | [apple] @7B7C; [silicon] S4, S5 |
| `+0x38 + 0x20·k` | Sub-channel k (k = 0–2) | Write a control-block address to start the chain; reads back the current block (0 when done) | [apple] write; [silicon] S4 (read 0 after completion) |

Channel assignments in Apple's firmware [apple]: 1 = LCD pixels, 5 = audio (FIFO `0x1000_4804`, config `0x0607_6002`: DREQ 7; `audio.md` §3), 6 = TV (FIFO `0x1000_2C04`), 7 sub-channel 1 = `dma_memcpy`. Channels 0–7 raise interrupt sources 0–7 [apple @7DC0; hypothesis for the numbering].

### Status bits

| Bit | Meaning | Evidence |
|---|---|---|
| 8 | LCD pixel run finished | [apple] the LCD push registers its completion callback with mask `0x100` (@145E2); [silicon] S5: every frame and rectangle completed on bit 8 |
| 9 | Set together with 8 after an LCD run; meaning unknown | [silicon] S4, S5 (`0x2300`) |
| 11 | Part of the generic transfers' mask | [apple] @804C (mask `0x800 \| 1 << (13 + k)`) |
| 13 + k | Sub-channel k's chain ended | [apple] @804C; [silicon] S4, S5 (bit 13 after a sub-channel 0 transfer) |
| 0–7 | **Not** completion bits (an early reading that was wrong) | [silicon] S4 |

After a full LCD frame on channel 1, sub-channel 0, the status reads `0x0000_2300` [silicon S4, S5]. Clear the whole word (`0xFFFF_FFFF`) before arming, so a stale bit cannot end the next wait [ours, proven S5–S16].

## Control block

32 bytes, 32-byte aligned [apple @7FC4; silicon S5].

| Offset | Field | Evidence |
|---|---|---|
| `+0x00` | Next block address, or 0 | [apple] |
| `+0x04` | Source address | [apple]; [silicon] |
| `+0x08` | Destination address | [apple]; [silicon] |
| `+0x0C` | Length in units (below). 2-D: `rows << 16 \| units per row` | [apple]; [silicon] S5 (1-D and 2-D) |
| `+0x10` | Transfer information | [apple] |
| `+0x14` | 2-D skips in units: source `[15:0]`, destination `[31:16]`. Each is the gap after a row (pitch − row width), **not** the pitch | [apple] @816E; [silicon] S5 (a sub-rectangle drew unsheared) |
| `+0x18`, `+0x1C` | 0 | [apple] |

### Transfer information bits

| Bits | Meaning | Evidence |
|---|---|---|
| 0 | Always set | [apple]; meaning [hypothesis] |
| `[6:4]` | Source width, log2 bytes (2 = 4 bytes, 5 = 32-byte bursts) | [apple]; [silicon] S5 (both used) |
| `[14:12]` | Destination width, log2 bytes | [apple] |
| 15 | Destination paced by DREQ | [apple]; [hypothesis] meaning |
| 16 | 2-D | [apple]; [silicon] S5 |
| 17 | Destination increments | [hypothesis] from Apple's memcpy words |
| 24 | Set only on a chain's last block: interrupt when done | [apple] per-row chains clear 24 and 25 on all but the last (@831E); [hypothesis] meaning |
| 25 | Cleared with 24 on non-final blocks | [apple] |
| 27 | Uncached path (`dma_transfer_setup_memcpy` clears it, the `_uncached` variant keeps it) | [apple]; [hypothesis] meaning |
| 28 | Set on peripheral destinations | [apple]; [hypothesis] meaning |

**Length unit:** the smaller of the source and destination widths [apple: the 2-D set-up divides the row width by the smallest alignment; silicon S5: a full frame of 38,400 four-byte units with 32-byte source bursts transferred exactly 76,800 pixels]. Apple chooses each width as the largest power of two up to 32 that divides the length and both addresses.

## Examples

**Apple's `dma_memcpy` of 4 KB** [apple]: channel 7, sub-channel 1, block `{0, src, dst, 0x80, 0x0102_5151, 0, 0, 0}` (32-byte units both sides), then sleep on a semaphore the channel interrupt releases.

**Full 320×240 RGB565 frame to the LCD** [silicon S5–S7; ours, following Apple's words]:

```
cb = { next 0, src frame (32-byte aligned), dst 0x1000400C,
       len 38400, ti = 0x1900A001 | src width 5 << 4, stride 0, 0, 0 }
ch1 +0x00 = 0x09032101        ; enable, DREQ 3
ch1 +0x04 = 0xFFFFFFFF        ; clear status
ch1 +0x38 = &cb               ; sub-channel 0: start
then arm the LCD interface (lcd.md §6); wait for ch1 +0x04 bit 8
```

Apple's own LCD transfer-info words: `0x1900_A051` (1-D) and `0x1901_A021` (2-D) [apple].

**Sub-rectangle** w × h at (x, y) of a 320-pixel-wide frame [silicon S5]: `len = h << 16 | w/2`, source width 4 bytes, 2-D bit set, stride word `(320 − w)/2` (source skip, in 4-byte units). x and w must be even.

## Open

- Bits 9 and 11 of the status; the state field's other values; what bit 0 and bits 25/27/28 of the transfer information do.
- Whether sub-channels 1 and 2 run concurrently with 0, and their arbitration.
- DMA from and to SDRAM (Apple's TV and memcpy paths use it; not exercised on silicon here).
- The DREQ numbers of peripherals other than the LCD (3).
- Interrupt delivery: channel n → interrupt source n is Apple's registration, not confirmed on silicon.
