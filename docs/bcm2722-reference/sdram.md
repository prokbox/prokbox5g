# 11. SDRAM

## Summary

| | | Evidence |
|---|---|---|
| Size | 4 MB (4,194,304 bytes): address bit 22 is not decoded, so `0xFE40_0000` aliases `0xFE00_0000` | [silicon] S11 (aliasing test); [external] brief "32 Mbit stacked SDRAM" |
| Core address | `0xFE00_0000–0xFE3F_FFFF` | [apple]; [silicon] S10, S11 |
| Host address | `0xC000_0000–0xC03F_FFFF` through the host port | [rockbox]; [silicon] S10 (64 host-written words read back by the core, 0 wrong), S11 (256 words over 4 MB, 0 wrong) |
| State after a host start | Not initialised | [hypothesis]: nothing configures it; Apple's runtime runs the sequence below |
| Controller | `0x1000_3400` | [apple]; [silicon] |
| Scalar access after set-up | Correct: a 64 KB grid over all 4 MB read back right | [silicon] S11 |
| Vector access | `vld`/`vst` of a row from and to SDRAM correct | [silicon] S11 |
| Speed | Not measured | — |

## Controller registers (`0x1000_3400`)

| Offset | Use | Values seen | Evidence |
|---|---|---|---|
| `+0x10` | Timing word 1 | `0x0105_008C` | [apple] @1410a; field meanings unknown |
| `+0x14` | Timing word 2 | `0x0105_000C` | [apple] @1410a; fields unknown |
| `+0x20` | Command / status. Write 7 = initialise with the mode word; write 0 = reset / idle. Bit 15 reads 1 when the command has completed | 7, 0 | [apple] @12d90, @14892 (bit 15 polled after 7, polled clear after 0); bit 15's meaning [hypothesis] |
| `+0x24` | `[31:16]` refresh interval in **core cycles**; `[15:0]` preserved | `0x0493` (1171 = 7.8 µs at 150 MHz) | [apple] @14406; 7.8 µs = 64 ms / 8192 rows [hypothesis] |
| `+0x28` | Mode word | `0x3057_0000 \| a << 12 \| b << 8 \| c << 2 \| 0x30`, Apple a = 12, c = 1, b = 8 (fallback 4) | [apple] |
| `+0x30` | Control | `0x80E8` before the command; `0x0064` when running; `0x0001` at shutdown | [apple]; meanings unknown |
| `+0x00–0x0C`, `+0x18`, `+0x1C`, `+0x2C`, `+0x34` | Never written by Apple | — | [apple] |

Apple sends no separate precharge, mode-register or refresh commands: command 7 appears to run the whole JEDEC initialisation in hardware [hypothesis].

## Bring-up sequence

Apple's order [apple, vmcs 1.3 @14406, @1410a, @147ca, @12d90, @14842, @14892], run after the switch to 150 MHz. With the deviations marked [ours], the same sequence brought SDRAM up on silicon at 150 MHz [silicon S10] and passed a full memory test [silicon S11].

1. **Refresh.** `+0x24 = (+0x24 & 0xFFFF) | R << 16`, R = 7.8 µs in core cycles: `0x0493` at 150 MHz, `0x00D2` (210) at 27 MHz [ours, from the same 7.8 µs].
2. **Timing.** `+0x10 = 0x0105_008C`; `+0x14 = 0x0105_000C`.
3. **Cache.** Flush (bit 30, wait for bit 0), write `0x10000C00 = 0`, flush again. Apple leaves the cache enabled during the command and probe; leaving it off also works [silicon S10].
4. `+0x30 = 0x80E8`.
5. [ours] Wait 200 µs (power-up settling; Apple has no explicit wait).
6. **Initialise.** `+0x28 = mode word`, then `+0x20 = 7`, then poll `+0x20` bit 15 (Apple: up to 12,000 polls, about 0.56 ms at 150 MHz). **A timeout is not an error**: Apple ignores it and lets the probe decide.
7. **Geometry probe.** Save the words at `0xFE00_0000` and `0xFE00_0200`; write `0x1234_5678` and `0x8765_4321` there; read both back (Apple reads the first, and checks twice). If the cache is on, flush before reading so the read reaches SDRAM.
8. **On failure,** reset the controller: flush; clear `0x10000C00` bits 30 and 16; `+0x20 = 0`; wait (up to 8,000 polls) for bit 15 to clear. Then retry from step 6 with the other mode field (8 → 4 → 8 …). Apple loops forever; bound it.
9. **Run.** `+0x30 = 0x0064`; `0x10000C00 = (v & ~bit 30) | 0x0001_0000` (Apple ends at `0x8001_0000`, cache on).

### Mode word fields

| Field | Apple's values | Notes |
|---|---|---|
| a (`[15:12]`) | 12 | [hypothesis] row address bits |
| b (`[11:8]`) | 8, then 4 on retry. Apple's mode-word helper turns a requested 7 into 4 | [apple]. On our unit the probe **passed with b = 4** (word `0x3057_C434`) on the second attempt, cache off, at 150 MHz [silicon S10]. What b encodes is unknown; reading it as column bits does not fit a 4 MB part [hypothesis] |
| c (`[3:2]`) | 1 | — |
| constant | `0x3057_0000 \| 0x30` | — |

## Silicon history (for anyone repeating this)

- **S9:** failed. The attempt treated a command-7 poll timeout as fatal, used field 7 instead of Apple's 4 for the fallback, had no controller reset between attempts, and wrote `0x10000C00 = 0` before the flush instead of after. Each now follows Apple.
- **S10:** up on attempt 2 (field 4, cache off), at 150 MHz. Host writes at `0xC000_0000` read back right from the core.
- **S11:** size 4 MB by aliasing, 64 KB grid 0 wrong, vector row load and store right (memory-test word `0x4D40_0083`: 64 × 64 KB, 0 grid errors, VLD and VST bits set, test ran).
- **S28–S30:** every video session's `OPEN` brought SDRAM up at 150 MHz on attempt 2 (field 4), bit 15 seen on both attempts (`OPEN` word `0x5344_0233`), and the pre-play readback over the 1 MB FIFO was 0 of 64 words wrong.
- A loss of contact during decoding first attributed to SDRAM in S10 was later explained by a host-side misreading of a torn index word (S12) and by a vector kernel addressing bug (S15), not by SDRAM.

## Open

- Self-refresh / power-down sequence: unknown (Apple's shutdown writes `+0x30 = 1`; nothing else is known).
- Whether field 4 is required on every unit (on ours field 8 fails at 27 MHz too: S32–S34, `OPEN` word `0x5344_0231`).
- Latency and bandwidth from the core, the vector unit and DMA, with the cache on and off.
- Whether bit 15 was seen on the passing attempt in S10 (the log word exists; not summarised here).
