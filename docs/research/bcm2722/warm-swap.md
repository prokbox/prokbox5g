# The warm swap: a new image on a running BCM, panel kept on (G9, bench row B13)

**Status:**
- Designed and built 2026-10-23. It passes in vc2sim (`bcmfw/tests/jobs_image_test.py`, row "warm swap").
- **On silicon (2026-10-25, three builds): never parked** (§8). The park ran its stub without the cache flush Apple puts between writing code and running it; every image now flushes (branch `p19-b13`). Bench row B13 decides it at the next sitting.

**Tags:** [silicon] measured on the device, [sim] vc2sim only, [ours] our design, [hypothesis], [estimate].

**What it's for:**
- G9's display-only resident image (`jobs.md` §10) costs only 19 KB of PP RAM, but a video start must then load the video image.
- Without a warm swap, that load is a cold start: a power cut, at least 50 ms off, a bootstrap, an upload and the panel's power-on. That's 0.37–0.62 s of dark screen [silicon, `independence.md` §3].
- With it, the panel stays lit and keeps its last frame. Only the upload and a jump remain.

## 1. Why a software park, not a second hardware start

- **Restarting the core through the start register** (`0x10000400 = 0xA5A5_0002` a second time, with no power cut) is untested (`host-interface.md`, "Starting an image").
- **What it might reset is unknown.** If it reset the GPIO block, GPIO 48 (the panel's reset line) would drop and the panel would need its full power-on.
- **A park needs nothing unknown.** The running image moves its own core into a small stub that lives outside every image. The host overwrites the image while the stub spins. The stub then jumps to the new image's entry.
- **Nothing in the path touches a GPIO or a clock**, and the panel's controller keeps its GRAM, so the screen shows the last frame throughout.

## 2. The sequence [ours]

| Step | Who | What |
|---|---|---|
| 1 | host | Ends any video session (`CLOSE`). A session in progress gets `HP_E_BUSY`. |
| 2 | host | Sends `HP_OP_PARK` (0x11) with `HP_PARK_MAGIC` (`"PARK"`). The image must advertise `HP_CAP_PARK` (0x2000), as every protocol image now does. |
| 3 | BCM | Checks the state, then replies `HP_OK` with the warm flags: `BCMFW_WARM_PANEL_ON` (1), plus `BCMFW_WARM_TIMING1` (2) when the panel's second timing word is in use. It refuses with `HP_E_STATE` on the fast clock or in a panel standby. |
| 4 | BCM | Its next `hp_poll` parks: it waits out any LCD DMA, disables interrupts, turns every interrupt source off (`IC_PRIO` = 0), copies the 30-byte stub (`bcmfw/park.S`) to `BCMFW_PARK_STUB` (0xDE900), **flushes the cache as Apple does after writing code** (`vmcs.bin` @25a; `BCMFW_PARK_FLUSH`, §8) and calls it. Each step goes into `0x138` (`HP_WARM_PROG`: `'W'` << 24 \| step << 16 \| detail) for the host's diagnostics. |
| 5 | stub | Writes `BCMFW_WARM_PARKED` (`"PRK"` << 8) \| flags to `BCMFW_MB_WARM` (0x13C, the last word of the protocol header), then spins reading that word. |
| 6 | host | Polls 0x13C until it reads `PRK` (expected within 1 ms) and keeps its flags. |
| 7 | host | Uploads the new image exactly as at a cold start (step 4 of "Starting an image": write address 0, stream the image). This zeroes 0x13C; the stub keeps spinning. |
| 8 | host | Writes `0x1F8 = 0` (the ready word) and flushes the cache as at a cold start (`0x10000C00 = 0xC000_0000`, wait for bit 0, `= 0`). Our images never turn the cache on, so the flush only mirrors the proven sequence. |
| 9 | host | Writes `BCMFW_WARM_GO` (`"WGO"` << 8) \| the flags from step 6 to 0x13C. |
| 10 | stub | Sees `WGO` and branches to `BCMFW_ENTRY` (0x200). |
| 11 | new image | Its `bcmfw_main` reads 0x13C before anything else. On `WGO` with `PANEL_ON`, it calls `lcd_warm()`: it re-applies the interface and bus timing the last image used, and sets the panel as on. There is no reset pulse, no power-on script and no bus resync, and GPIO 48 is never written. The image sets `BCMFW_INFO_WARM` (0x0800) in the info word and writes `BCMFW_WARM_UP` (`"WUP"` << 8) \| flags to 0x13C. |
| 12 | host | Waits for the ready word (`0x1F8 ≠ 0`), as at a cold start, then sends `HELLO`. The next `LCD_UPDATE` draws with no power-on. |

**Without `PANEL_ON`** (the panel was off when parked), the new image starts as at a cold start: its first update runs the full power-on.

**A cold start can't be mistaken for a warm one.** The upload always leaves 0x13C at 0, the image's own zero word, so only a host that writes `WGO` after an upload gets a warm start.

**The host's fallback, from any step:**
- no `PRK` within 10 ms, or no ready word within 100 ms of `WGO`: a cold start (power cut, ≥ 50 ms off, bootstrap, upload). That's today's path, and the image's normal start handles it.
- The PP driver logs `bcm warm swap fail step=N`.

## 3. Rules and layout [ours]

- **The stub's place:** `0xDE900–0xDE9FF`. It sits above the rings (`0xD0000–0xDC000`), below the framebuffer (0xE0000) and past the job block (`0xDE000`, 64 bytes).
- **The checks:** `build.py` checks, for every protocol image, that the stub fits its 256 bytes and that the load image ends below it. Only the upload must not reach the stub; the new image's `.bss` and data may use that SRAM after the jump.
- **The video self-test scratch** (`0xDDE00–0xDF000`) overlaps the stub's place. That's harmless: the self-test never runs while parked, and a self-test that runs later only overwrites a stub that is no longer used.
- **The new image inherits:**
  - the LCD interface on, with the old image's timing words;
  - the timer running;
  - interrupts off, with every source masked;
  - no DMA running;
  - the core's registers and stack pointer, which `crt0.S` resets;
  - its vector registers as they were. No kernel is known to rely on their contents at the start [hypothesis]. The self-test's clean mode clears them first (dev22).
- **The clock:** the park is refused on the 150 MHz clock, so the new image always starts on the boot clock, the one its bus timing assumes.
- **Sizes after this change** [measured, `build.py`, 2026-10-23]:

| Image | Size |
|---|---|
| Display-only | 40,800 B with `J_OP_DECODE` (`album-render.md`); 21,764 B without it (the stub, `PARK` and the warm start added 572 B; the vector kernels about 2 KB) |
| Video image | 206,916 B |
| Merged image | 218,116 B; image and `.bss` end at 0x3512c, inside the arena (from 0x35400; 724 B spare after jref's DECODE) |

## 4. What vc2sim shows [sim]

The row "warm swap" in `jobs_image_test.py` checks 38 cases: a round trip from the display-only image into the shipped video image (`mkvideo.py`'s flags: idle sleep, gated, doorbell) and back. It boots the display-only image with panel model 3 and draws a frame. Then it runs:
1. `PARK` without its magic: refused with `HP_E_INVAL`.
2. `PARK` with the magic: `HP_OK` with `PANEL_ON`. The core runs inside the stub, and 0x13C reads `PRK`.
3. The 206,916-byte video image is uploaded in 4 KB pieces while the core runs. The core stays in the stub.
4. The ready word is cleared, the cache flushed, and `WGO` written.
5. Checks after the start:
   - the video image says ready;
   - 0x13C reads `WUP` with the flags;
   - the info word has `WARM` and `PANEL_ON`;
   - the start register was never written again;
   - the panel saw no reset edge;
   - the panel's frame is unchanged across the swap.
6. `HELLO` reports the video image's capabilities, and a full-frame update reaches the panel with no reset pulse.
7. `OPEN` (the null codec) works on the warm-started image. `PARK` with the session open is refused with `HP_E_BUSY`. After `CLOSE`, the same swap runs back into the display-only image, and the same checks pass.

**A finding from the round trip** [sim]: the first version copied the stub from a 2-byte-aligned address in the video image (0x1735e; 4-aligned in the display-only image only by chance), and the word copy read garbage. The core then ran it and slept with nothing to wake it. The stub is now `.align 2`, and `build.py` refuses an unaligned stub. **Rule:** anything copied by words must be 4-aligned in every image that contains it. How the silicon itself treats an unaligned `ld` is not recorded; the simulator's answer was not the bytes.

**A mutation check:** with `lcd_warm()` removed, the row fails twice: no `WARM` or `PANEL_ON` in the info word, and the first update resets the panel. So the row does detect a cold power-on.

## 5. What only silicon can answer [hypothesis]

1. **Does the core run the stub's spin loop unharmed** while the host streams about 207 KB into SRAM below it? FEED already writes the FIFO while the core runs, so this is expected.
2. **Does any peripheral** the old image left on matter to the new one? Candidates: the LCD interface's FIFO, a pending timer match, the doorbell. Every source stays masked until the new image's `irq_init` sets its own; an idle-sleep image also clears its timer match there.
3. **Does the panel keep its frame and its state** through about 20–40 ms with no bus traffic? Expected: the idle case today is longer.
4. **Is the cache flush needed, or harmful,** on a running core? It's done as at a cold start; B13 can try it without.

## 6. Timing [estimate]

| Part | Time |
|---|---|
| `PARK` round trip and park | < 1 ms |
| Upload of the 206,916-byte video image, PP CPU copy at 80 MHz | ~13 ms |
| The same, at 30 MHz | ~37 ms |
| The same, by engine 2 (~11 MB/s) | ~19 ms |
| The video image's own start to ready | ~2 ms |

- **The CPU copy times** scale [silicon S8]'s 150 KB in 9.6 ms (at 80 MHz) and 27 ms (at 30 MHz).
- **The start time** is from the 8 KB image's 1.9 ms [silicon]; the video image's start does no more before its ready word.
- **A video start's warm swap:** about 15–40 ms, against 0.37–0.62 s cold, with no dark interval. That's within the "about 20–40 ms" `jobs.md` G9 assumed.
- **The swap back** to the display-only image at video end: 19 KB, about 1–3 ms of upload.

## 7. Bench row B13 [ours]

**The entry:** the Bench session's step `b13` (Developer › **Bench session**; until 2026-10-26 also the Debug row "BCM test: warm swap (B13, optional)"). It's in builds with `--bcmfw-devtest --bcmfw-video`, from `402e5996b3` on `p19-bcm-prep`. It needs a stage 5 pass flag and the video image file. The display-only image (`jobs_display`, 21,344 B) is embedded by `mkdevtest.py`; the other embedded images are byte-identical, so earlier pass flags still match.

**What it does** (`apps/video/vplayer.c`, `vplayer_warm_lab`; about 1 minute):
1. Boots the display-only image cold, the device test's way, with today's screen as its first frame.
2. Makes 20 swaps, alternating into the video image and back:
   - Rounds 0, 1, 4, 5, … upload by CPU copy; rounds 2, 3, 6, 7, … by engine 2. If engine 2 can't take a buffer, the port's own fallback copies by CPU and the line can't tell the difference.
   - Rounds 16–19 skip the cache flush (§5 item 4).
   - After each swap into the video image: `HELLO`, `OPEN` of a 320×240 session and `CLOSE`. This proves the SDRAM set-up after a warm start.
   - After each swap back: `HELLO`, plus a 16×16 square (green or red) drawn by `FILL` + `PRESENT` + `FENCE`, one place further right each round. The owner sees the squares step along a row near the bottom of the screen.
3. **The injected fault:** `PARK` and an upload, but no go word. After 100 ms, the ready word must still be 0. The host then cuts the power (60 ms off) and cold-boots the display-only image. That restart is dark, as expected.
4. Hands the BCM back the stock way.

**The log** (`bcmtest.log`, prefix `warm`):
- `=== BCM test, warm swap (B13): build … ===`
- `warm: display-only image N bytes, video image N bytes, cold boot ok|FAILED`
- `warm <round>: to video|display by CPU|engine 2[, no cache flush]: PASS|<why>; park N us, upload N us, ready N us, all N us, flags F, info XXXXXXXX`
  - `<why>` is one of: `PARK refused`, `never parked`, `upload`, `cache flush`, `never ready`, `warm word`, `info word`, `attach`, `use after the swap`.
  - The first failing round ends the swaps.
- `warm fault: no go word, nothing started|an image STARTED anyway; cold restart ok|FAILED in N us`
- `warm: PASS|FAIL (K of N swaps; park max …, ready max …, into video max … CPU, … engine 2; panel off N)`

**The summary screen** shows the same figures, plus the first failure's round and step.

**Passes when:**
- `warm: PASS` (20 of 20, the fault handled);
- the owner saw no blink, blank or flicker during the swaps;
- go → ready is at most 5 ms;
- `panel off 0`.

**Then:**
- If B13 passes, G9's display-only image becomes L2's resident image (Q2), and the video image is loaded from disk at a video start.
- If it fails, the merged image stays resident, and the failure (its round, step and line) goes into `quirks.md`.

## 8. Silicon, 2026-10-25: never parked, and the fix

**What B13 logged** [silicon] (`bench-20261025-1430`, `-1819` and `-2257`, `bcmtest.log`; builds `4e57b3b9a1`, `caaa9890b0`, `799ff0f8d7`): `PARK` answered `HP_OK` with flags 3 (panel on, timing word 1), and the warm word stayed 0 (10 ms on the first two builds, 60 ms on the third: `warm 0: PARK st=0 n=1 f=3, warm word 00000000 after 60008 us`). The image then ignored the stock power-down, so the PP cut the power (the owner's black flash). Nothing else was known: the lab had no diagnostics.

**The candidates, checked** [ours, from the code, the listing and vc2sim]:

| Candidate | Finding |
|---|---|
| The stub's target overlaps something live in the display-only layout (batch 2) | No. The image and `.bss` end at `0xC6C0`, the decode work area is `0xCC00–0x46000`, the stack `0xCC000–0xD0000`, the rings `0xD0000–0xDC000`, the job block `0xDE000–0xDE07F`; `0xDE900–0xDE9FF` is free, and the copy (8 words) stays inside it |
| `park()` never runs in the jobs image | No. The jobs main loop calls `hp_poll()`, and the listing has the park inline after `publish_state()` (the "PRK" store at the stub's first instruction) |
| The stub writes `0x13C` but the host reads elsewhere | No. The PP reads `0x13C` through the main bank like every mailbox poll that works (the ready word); nothing on either side writes it between the reply and "PRK" |
| `lcd_blit_wait()` never returns | No blit is pending at that point (the first frame went by the synchronous `LCD_UPDATE`), so it returns at once; even a pending one is bounded (100 ms, then a CPU push) |
| `di` and `IC_PRIO` | `di` runs at every image's start on silicon; the display-only image never enables a source, so the writes change nothing |
| **The stub runs as code straight after the core wrote it** | **Never done on silicon before, and never done by Apple without a flush.** Apple flushes the whole cache after writing code and before running it (@88a0 after a library's relocations, @91a4 at a library's start; the flush @25a). Our park copied the stub with data stores and called it at once |
| The stub runs from `0xDE900` | Also never done before (our code had run only below about `0x36000`). Not excluded; the diagnostics below tell the two apart |

**Apple's sequence against ours:**

| Step | Apple (`vmcs.bin` 1.3) [apple-RE] | Ours before | Ours now |
|---|---|---|---|
| Interrupts | off for the flush (sr saved, `di`) | off (`di`), every source off | the same |
| Writing code | the dynamic linker's relocation stores | the stub's 8 words by data stores | the same |
| Between writing and running | whole-cache flush (@25a): the request over the word with the enables cleared, again if bit 0 already reads set, poll bit 0 up to 4,096 reads, the enables cleared | **nothing** | **@25a's flush** (our re-request stops after 15; bit 0 reads set at idle on silicon, PW1) |
| Running it | call into the library | `bl 0xDE900` | the same |
| Parked | (hibernate, @14288) a busy poll of a mailbox word, no `sleep` | busy poll of `0x13C` | the same |

**The fix** [ours, following Apple's sequence]: `park()` runs @25a's flush (`hp_code_flush`) between the copy and the call, in every image, and writes each step to `0x138`. The same rule now covers the two other places a jobs image runs code it did not load at its start: the PP's hibernate stub (`HP_PARK_HIB`, before its call) and the overlay (`J_OP_OVERLAY`, before its `init`). `BCMFW_PARK_FLUSH=0` exists only for the sim test that shows the old park refused. The park, `PARK` and the flush are compiled `-Os` out of line, which keeps the merged image inside its arena (image and `.bss` to `0x3533C`). Images: display-only `jobs_display` `f069a36a` → `05c1e914` (46,848 B); merged `80bc8611` → `88e710cb` (218,564 B); plain video/probe `948c5a70` → `7b65c853` (220,260 B).

**The model gap and the sim test** [sim]: vc2sim decoded code from memory as it was at the first fetch, so it ran the just-written stub as if the instruction side were coherent. Its **strict code coherence** (`vc2sim.StaleCode`, on in `hello_test.Bench` and so in every image test) takes memory at each cache flush (the host's or the core's bit 30) as what fetches may read, and refuses a first fetch of anything written since. `jobs_image_test.py` row "a park without Apple's flush is refused" builds the display-only image with `BCMFW_PARK_FLUSH=0` and expects that refusal at `0xDE900`; row "warm swap" passes only with the flush, and now also checks the progress word and the stub's copy; row "the park's flush with bit 0 stuck at 1" parks with the bit never clearing (detail `0xF000`: 15 re-requests, no poll). The same model caught the hibernate stub and the overlay running PP-written code unflushed (`hibwake_test.py`, `decode_image_test.py`, `jobs_image_test.py` overlay rows), fixed the same way.

**The diagnostics** (the PP, `vplayer.c` `warm_diag`): after a `never parked`, two lines:
- `warm N: diag prog <0x138> (step S) state <0x130> then <0x130 5 ms later>, ready <0x1F8>, fault pc <0x1E0>, limit <0x180>`
- `warm N: diag stub <0xDE900..0xDE90C>, warm word <0x13C>, cache <0x10000C00>, lcd dma cs <0x10000880> int <0x10000884>`

After a good park of rounds 0 and 1: `warm N: diag parked, prog <0x138>` (the flush's retries << 12 | polls in the detail). How to read them: `bench-p19.md`, B13.


## 9. Silicon, 2026-10-26 (sitting 3): it parks; one swap in ten never ready

**What B13 logged** [silicon] (`bench-20261026-1029/bcmtest.1.log`, build `7e8c140e40`): the park works in both directions (the §8 fix holds). Rounds 0–8 passed; round 9, "to display by CPU", parked and uploaded, and no ready word came within 100 ms of the go word. The round line's `info` was the previous round's (not re-read on that path), and no diagnostics were written (only a park failure wrote them).

**Reading** [hypothesis]: `quirks.md` #41. The host's flush before the go word was the cold start's form on a running core, with bit 0 still set by the park's own flush; if it did not run, the merged image's park tail (`0x2548–0x25FF`) stays in the instruction side, where the display-only image's `jobs_init` calls `publish` (`0x259A`) before its ready word.

**Changed** [ours, following Apple's @25a for a running core]: step 8 of §2 is now: the request over the word with bits 31 and 30 cleared, the request again while bit 0 reads 1 straight after it (at most 15), bit 0 polled (5 ms), then `0` (the word every image starts with). Every failed round now logs `warm N: diag` (prog, state, ready, fault pc, limit, info; stub, warm word, cache, LCD DMA), and past the park `warm N: diag park` and `warm N: diag flush word <before> -> <after>, re-requests, polls, us; upload read back K of N words wrong`. Rounds 0 and 1 add the host flush to their `diag parked` line. The display rounds' squares now alternate green and red, one place along each.

**Next if it fails again:** the core's own @25a flush in the stub after `WGO`, before the jump (an image change: batched with the next merged-image change).
