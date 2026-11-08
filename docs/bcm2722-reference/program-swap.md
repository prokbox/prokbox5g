# 21. Changing the program on a running core

*How to replace or add code on a BCM2722 whose core is already running, without a power cut: what the hardware does, how Apple's firmware changes the chip's program, how our warm swap does it, every failure we hit and what each one means. Written so it can be read without the rest of the manual; the cache word it depends on is in [`boot-and-clocks.md`](boot-and-clocks.md) §2, the host port in [`host-interface.md`](host-interface.md).*

**Evidence tags** are the manual's ([`index.md`](index.md#evidence-tags)): [silicon] measured on our unit, [apple] read from Apple's firmware (statically; "host" addresses are in Apple's ARM firmware `osos` 1.3, `@xxxx` are in `vmcs.bin` 1.3), [ours] our own design, [sim] vc2sim only, [hypothesis] not verified. Build maps (sizes, symbol addresses of our images) are [ours], read from `bcmfw/build.py` output for the commits named.

## 1. The hardware facts that matter

1. **Code the core runs is fetched through an instruction side that does not follow stores.** A 30-byte stub the core copied into SRAM with data stores and called at once never ran (three builds) [silicon, `quirks.md` #40]; the same stub ran after a whole-cache flush between the copy and the call [silicon, sitting 3]. This holds with the cache-enable bit (bit 31 of `0x1000_0C00`) **clear**, the state every one of our images runs in. So "cache off" does not mean "no stale code": some instruction-side state (a cache, a line buffer or a prefetcher) survives [silicon; which of these it is: hypothesis].
2. **The cache control word `0x1000_0C00`** [apple; silicon for bit 0]: bit 31 enable, bit 30 flush request, bit 16 set by Apple once SDRAM is up, bit 0 flush done. **Bit 0 is a status bit that stays set** once a flush has finished: a running image reads `0x0001_0001` or `0x0000_0001` long after any flush [silicon, PW1; sitting 4f read `1` before and after the host's flush]. Writing 0 does not clear it; reading `1` after writing `0` is the normal state, not a fault. A flush that requests and then polls at once can therefore read a stale "done".
3. **What a flush covers is not known.** Bit 30 is a whole-cache request; whether it writes back data, invalidates instructions, or both, whether it does anything with bit 31 clear, and whether a request from the host through the port acts like one from the core, are all open (§7). Fact 1 shows that a **core-issued** request with bit 31 clear (Apple's @25a form) is enough to make **core-written** code visible [silicon]. Nothing yet shows that a **host-issued** request removes **stale instruction lines** of code the core ran before.
4. **Host writes land in SRAM while the core runs.** The port's writes into SRAM below a spinning stub read back right, piece by piece, before and after the host's flush [silicon, sitting 4f: no wrong 1 KB piece out of 210]. The core's own data stores with the cache off are visible to the host too (§5.2: the new image's `.bss` clear was read back through the port) [silicon].
5. **Geometry is unknown:** 128 Kbit instruction and 128 Kbit data cache per the product brief [external]; line size, associativity and replacement policy unmeasured.

**The rule that follows** [apple practice; silicon for the core-written case]: *the core itself runs a whole-cache flush (Apple's @25a, interrupts off) after any code is written and before the first fetch from it, whoever wrote the code.* A host flush is safe only where nothing stale can be cached: on a core held in reset after a power cycle.

## 2. Apple's method

**Apple never replaces the program of a running BCM.** Every change of boot image is a cold restart; every change of code on a running core is a library load that the core performs and flushes itself [apple, static reading of `osos` 1.3 and `vmcs.bin` 1.3; calls through function pointers cannot be ruled out].

### 2.1 Changing the boot image: power cycle, upload, host flush, start

Apple's firmware ships two boot images, `vmcs.bin` (the media server: video, codecs, TV out) and `RenderServer.bin`, both linked at core address 0 with entry `0x200` [apple]. The host keeps one run-state byte (host `0x108C_BEC8`: 0 off, 1 hibernated, 2 booting, 3 running) [apple].

1. **"Make vmcs run"** (host `0x101B_02D8`) and **"make RenderServer run"** (host `0x101B_03EC`) look only at that state: if off, boot the wanted image; if hibernated, resume; if running, return success **without checking which image is running** [apple].
2. **So a switch first stops the chip.** Before asking for RenderServer the caller powers the chip off (host `0x1026_B154` then `0x1026_B158`); before asking for vmcs another caller resets the state byte to "off" (host `0x101B_03B4`, called at host `0x1026_B88C`) so the next request cold-boots [apple]. The video path powers off at its end (host `0x100B_4498`) and boots vmcs at its start (host `0x100B_45C0`) [apple].
3. **The boot itself** (host `0x101B_045C` → `0x101A_FE08` → `0x1028_7698`) [apple]:
   1. a cold power cycle (host `0x1026_611C`: both `GPO32` pins 14 and 15 low, 50 ms, pin 14 high, 50 ms);
   2. the image file read once into an ARM buffer and kept for later boots;
   3. the port bootstrap (host `0x1028_7998`, the byte sequence in `host-interface.md` "Starting an image");
   4. the whole image written at core address 0. (A second mode, never used by 1.3, writes a 16-byte name-and-size header at `0xFC000`, a parameter block at `0xFC010`, the payload at `0xFC100`, and 8 bytes at `0x200` that jump to `0xFC100`; its only caller passes the "address 0" mode);
   5. an optional word-by-word read-back compare from address 0 (a caller flag);
   6. `0x1F8 = 0` (the ready word);
   7. the host flush with the core held: `0x1000_0C00 = 0xC000_0000` (enable and flush together), poll until bit 0 reads 1, then `0x1000_0C00 = 0` (the two values are data words at host `0x1082_3584` and `0x1082_3580`);
   8. start: `0x1000_0400 = 0xA5A5_0002` (data word at host `0x1082_3588`);
   9. poll `0x1F8` until non-zero; state → 2; then → 3 (host `0x1028_92D4`, which on that step also sets up four host-side services whose roles are not traced).

   This is the only place Apple's host touches the cache word. **The host flush is always made on a freshly powered core that has not yet fetched anything** [apple]. Rockbox's `bcm_init()` uses the same values [rockbox].

### 2.2 Changing code on a running core: libraries the core loads itself

- After vmcs is up, the host sends `set_vll_dir <dir>` (host `0x101A_FC0C`) [apple]. Codecs and players (`aacdec.vll`, `h264dec.vll`, `mpg4dec.vll`, `mplayer.vll`, `slideshow.vll`, `passthruhandler.vll`) are ELF libraries vmcs loads at run time [apple]; the file bytes reach the chip through the host's file service ("Channel AppFileLoading" in `osos`) [apple strings; how the bytes are placed: hypothesis].
- **The core** links them: its dynamic linker patches the relocations, then runs @296 (flush @25a, then enable @224) before anything calls in (@88a0); at a library's start it flushes again (@91a4, @25a) [apple].
- **@25a, the running-core flush** [apple]: save `sr`, disable interrupts; `keep` = the word with bits 31 and 30 cleared; write `keep | bit 30`; read back, and while bit 0 already reads 1, write `keep` and request again (no bound); then poll bit 0 up to 4,096 reads (a breakpoint if it never sets); write `keep` (the cache is left **disabled**); restore `sr`.
- **@224, enable**: interrupts off, 7 `nop`s, read the word, clear bits 31–30, set bit 31, write, 5 `nop`s, `sr` back [apple].
- So on a running core, **only the core flushes, from code that is not being replaced, with interrupts off**, and it does so after the new code is in memory and before the first call [apple].

### 2.3 Other mode changes that are not program changes

- **TV out and display modes** are gencmd commands to the running vmcs (`display_control N dac=1 encoding=E`, `display_control N dac=0`, `mp_region …`); no image change [apple strings].
- **Hibernate** (`power_control videocore 1`, vmcs @14288; host side `0x1028_92D4`) parks the core **in vmcs's own code**: interrupts off, clocks parked, GPIO events saved and cleared, the stack moved, SDRAM idled after a cache flush, `0x1F0 = 0xA5A5_0000`, then a busy poll of `0x1F0` for `0xA5A5_0001`; the host then raises `GPO32` bit 15 and drops bit 14. Resume raises bit 14, drops bit 15, writes the wake word and waits for `0xA5A5_0002`; **the same program continues**, nothing is uploaded [apple; `brief-map-power.md` §3]. `osos` 1.3 has no static caller that asks for hibernate [apple]. It is the nearest thing Apple has to our park, and it is not a swap.

## 3. Our method: the warm swap (B13) [ours]

Purpose: start the video image while the display-only image drives a lit panel, with no dark gap (`../research/bcm2722/warm-swap.md`). Layout: the stub lives at `0xDE900–0xDE9FF`, outside every image; the warm word is `0x13C`; the park progress word `0x138`; the ready word `0x1F8`.

| Step | Who | What |
|---|---|---|
| 1 | host | `HP_OP_PARK` with its magic; the image replies `HP_OK` and its panel flags |
| 2 | old image | waits out LCD DMA, `di`, every interrupt source off, copies the stub to `0xDE900` with data stores, **@25a-form flush by the core** (`hp_code_flush`, at most 15 re-requests), calls the stub |
| 3 | stub | writes `PRK \| flags` to `0x13C`, spins reading it |
| 4 | host | polls `0x13C` for `PRK`; uploads the new image from address 0 over the old one (CPU copy or PP DMA engine 2) |
| 5 | host | `0x1F8 = 0`; 1 KB-piece read-back check (first and last word of each piece) |
| 6 | host | **flush from the host on the running core**, @25a's form: read the word, write `keep \| bit 30`, re-request while bit 0 reads 1 at once (≤ 15), poll bit 0 (≤ 5 ms), write 0 |
| 7 | host | the piece check again, rewriting wrong pieces (≤ 4 passes) |
| 8 | host | writes `WGO \| flags` to `0x13C` |
| 9 | stub | sees `WGO`, jumps to `0x200` (no flush of its own) |
| 10 | new image | `crt0` (the merged image first copies its 48 KB of AAC tables from the load image to `0x134000–0x140000`, then clears `.bss`), `bcmfw_main`, `lcd_warm()`, `hp_init` → `jobs_init` (lane-shift check; in the merged image since batch 4 also `bschk_run`), then the ready word |
| 11 | host | waits ≤ 100 ms for `0x1F8 ≠ 0`, checks `WUP` in `0x13C` and the info word's `WARM` bit |

Until 2026-10-26 (build `7e8c140e40`) step 6 was the cold start's form: `0xC000_0000`, wait for bit 0, `0`. From `caa718193c` on it is @25a's form above.

## 4. Apple against ours

| | Apple | Ours |
|---|---|---|
| Whole-program change | cold power cycle, bootstrap, upload, host flush on a held core, start | park in a stub, upload over the running image, host flush on a running core, go word |
| Who flushes after new code is written | the core (@88a0, @91a4), from resident code | the host, through the port (step 6); the stub jumps without flushing |
| Host flush on a running core | never | every swap |
| Flush form | core: @25a (bits 31–30 cleared during the request, re-request while bit 0 is stale, unbounded); host at boot: `0xC000_0000` then `0` | host: @25a's form at port speed (reads microseconds apart), ≤ 15 re-requests; before `caa718193c`: `0xC000_0000` then `0` |
| Where the core waits | its own resident code (hibernate) or not at all (reset) | a stub outside every image |
| Code at the same addresses before and after | never (a fresh power-up, or a library at a newly allocated address) | always: both images start at `0x200` and the old image's hottest loop sits where the new image's `bcmfw_main` is |
| Interrupts, DMA | off for the flush; hibernate clears GPIO events and idles SDRAM | off; LCD DMA waited out; sources masked |

The two ways ours departs from every proven sequence: **a host flush on a running core**, and **new code fetched at addresses where the old code was hot**. Apple never does either.

## 5. The B13 record and the round-0 regression

### 5.1 What silicon showed

| Sitting | Build | Host flush | Result |
|---|---|---|---|
| 2026-10-25, three builds | `4e57b3b9a1`, `caaa9890b0`, `799ff0f8d7` | cold form | never parked: the stub, written by the core, ran without a flush (#40) |
| 3 (2026-10-26) | `7e8c140e40` | cold form | rounds 0–8 pass both ways; round 9 (to the display-only image) never ready (#41) |
| later 2026-10-26, 4c, 4d, 4e, 4f | `fa3d8edb58` on | @25a form | **every run fails at round 0** (to the video image): parks, uploads, flushes, `WGO`, no ready |

[silicon] session logs `bcmtest.log` / `bcmtest.1.log` of each sitting; round lines and `warm N: diag` lines.

### 5.2 The 4f read-back is the new image's own start, not corruption

After the round-0 failure in sitting 4f the read-back found **45 of 211 sampled words wrong, from `0x28800` to the end**; 4c had seen the same range read back 0. That was read as the old image's dirty data lines written back over the new image. The build map says otherwise [ours, build maps; silicon read-back]:

- **211 samples identify the image.** The check reads every 1 KB plus the last word: 211 samples is a 214,356-byte image, the merged video image (`bcmvideo-jobs.bin`). The display-only image (46,944 B) gives 47. So the failing swap was **into the video image** (round 0), not into the display-only image.
- **The merged image's `.bss` is `0x28554–0x33B8C`.** Its load image carries 48 KB of AAC tables (`.xdata`) from `0x28554` to the end; `crt0` copies them up to `0x134000` and then clears `.bss` over the place they came from.
- **Exactly 45 sample addresses fall in that `.bss`**: `0x28800, 0x28C00 … 0x33800`. The four samples after it (`0x33C00`, `0x34000`, `0x34400` and the last word) are still the table bytes and read right, as do all 162 before it.

So the new image **started**: `crt0` ran its table copy and its `.bss` clear, and its stores reached SRAM. The failure is **between the `.bss` clear and the ready word**, in `bcmfw_main` or what it calls. The dirty-line write-back hypothesis is withdrawn: it would have to hit precisely the new image's `.bss` samples and nothing else. (The diagnostic compares memory with the file after the image has run; any image's `.bss` and moved data will always read "wrong" there.)

### 5.3 What changed between `7e8c140e40` and `fa3d8edb58`

`git log 7e8c140e40..fa3d8edb58 -- apps/video/vplayer.c bcmfw/`: 40 commits. The ones that touch the swap or the images it moves between [ours]:

| Change | Commit | Bearing on round 0 |
|---|---|---|
| Host flush on the running core changed from the cold form to @25a's form (bit 30 alone, bit 31 never set) | `caa718193c` | **the only change to the swap sequence itself** |
| Host flush refuses a bit 0 still set after 15 requests | `f182dc75c1` | not reached (the round got past the flush) |
| Merged image batch 4: h264 rewrites, load-timing probes at the first `OPEN`, the loop filter's vector form checks `bschk_run()` **in `jobs_init`, before the ready word** (`BCMFW_BSCHK 1`) | `9c63faff63`, `857a2ec0c1`, `364569c19b`, `cc0fc1677f` | adds new code to the start path |
| The display-only image | — | **unchanged**: 46,944 B, the same bytes in both builds |

The two merged images [ours, `build.py`]:

| | `7e8c140e40` | `fa3d8edb58` (and `p19-4g`: identical bytes) |
|---|---|---|
| Load image | 214,772 B (text 137,278, rodata 20,946, data 6,882) | 214,356 B (text 135,906, rodata 21,902, data 6,882) |
| `.bss` | `0x286F4–0x33C7C` | `0x28554–0x33B8C` |
| `.xdata` (48 KB) loaded at / parked at | `0x286F4` / `0x134000` | `0x28554` / `0x134000` |
| Start path below `0x50FA` (`crt0`, `bcmfw_main` `0x61C`, `lcd_*`, `timer_init`, `hp_init` `0x2472`, `hp_code_flush` `0x2548`, `park` `0x2590`) | — | **same addresses** in both; only literal constants differ |
| `jobs_init` | `0x1689E` | `0x15B42`, calls `bschk_run` `0x182E8` |

Neither layout reaches the stub (`0xDE900`), the job block (`0xDE000`) or the display-only image's overlay region (`0x13B000`); the table park at `0x134000–0x140000` is written only after the jump. **The display-only image's hot code** (what the core ran just before the park) is its main loop `bcmfw_main` `0x59A–0x72A`, `hp_poll` `0x1DDE`, `park` `0x1CF2`, `hp_code_flush` `0x1CAA`, `lcd_blit_wait` `0xB92` [ours, build map]: it overlaps the merged image's `bcmfw_main` (`0x61C–0x8D0`) and `lcd_*` start code, in **both** builds.

### 5.4 Reading [hypothesis]

**H1 (leading): the host's @25a-form flush does not clear the instruction side; the cold form did.**
- The new image's first code (`crt0`, `0x200–0x24E`) ran right: the display-only image ran its own `crt0` only once, at its boot, so nothing stale was held there.
- `bcmfw_main` at `0x61C` sits on the display-only image's hot main loop. If its lines are still held, the new image runs the old loop's instructions with its own data, and never reaches the ready word. That matches §5.2 exactly.
- With the cold form (bit 31 and bit 30 together, then 0) the same layout passed 9 swaps of 10 in sitting 3 (5 of them into the merged image); with @25a's form (bit 30 alone) it has failed every first swap since. The only sequence change between them is the flush form. One reading: a 0 → 1 → 0 edge on the enable bit, or a request with the cache enabled, is what discards instruction lines; a request with bit 31 clear only writes data back. That would also explain #40 (a data-side write-back is all a core-written stub needs) and #41 round 9 (the cold form read a stale bit 0 once and the flush never ran).
- The layout below `0x50FA` is identical in both merged builds, so H1 does not need the image change at all.

**H2 (second): the new start-path code fails only on a warm start.** `bschk_run` runs vector loads and stores before the ready word; on a cold start it passes on silicon (`J_VPU2 = 0xff`, `quirks.md` #51). A warm start inherits the old image's vector registers, timer and clocks. Nothing in `bschk_run` is known to depend on them; H2 is weaker, but it is the one image difference on the path.

**What tells them apart** (no image change; host reads after a "never ready"):
- step word `0x1C0`: 0 (the upload's zero) means `bcmfw_main` never ran its first line, which is H1; `0x5053_0005` or later means it got past `lcd_detect`;
- info word `0x1E8` (`warm_diag` already logs it): the `WARM` bit means `bcmfw_main` reached its warm-start branch;
- `HP_CAPS` (written by `hp_init`) and the job block's `J_VPU` (`0xDE000 + 0x44`: the merged image writes 3, the display-only image left 7) and `J_VPU2` (`+0x54`; 0 then the form bits): how far `jobs_init` and `bschk_run` got (H2);
- fault pc `0x1E0` (`warm_diag` logs it): an exception and where.

The 4f session log's existing `warm 0: diag` line already holds the info word, state and fault pc; read those first.

## 6. Candidate recipes for the next sitting, ranked

Each needs a vc2sim model that can show the fault first. Today vc2sim treats **any** write of bit 30, from host or core, as a perfect flush, and refuses (rather than serves) stale code (`vc2sim.StaleCode`), so it passes every form. **The model to add first** [sim, proposed]: an instruction-side line store (16 KB, line size a parameter) that keeps the lines the old image fetched and **serves them** after the swap, with a knob for what discards them: `core30` (a core-issued bit-30 request), `host30` (a host one too), `edge31` (only a 0 → 1 change of bit 31, from either side). The warm-swap row in `bcmfw/tests/jobs_image_test.py` must then **fail** with today's host @25a flush under `edge31` (it reproduces 4c–4f: `.bss` cleared, no ready) and pass under `host30`; a recipe is ready for silicon only when it passes under **every** knob.

1. **The core flushes in the stub after `WGO`, Apple's start-up form, then jumps** [ours, following @296 and @25a]. The stub (still running from bytes nobody overwrites) disables interrupts, runs @25a's flush (core speed, its own re-request guard), then @224's enable with its `nop`s, then @25a once more (which leaves bit 31 clear, the state our images expect), then jumps to `0x200`. This covers every knob: a core-issued bit-30 request and an enable edge, as Apple does at every start. Cost: about 40 bytes of stub, an image change in every protocol image (display-only, merged, plain), so it goes in the next batch of the frozen merged image. The host flush in step 6 becomes optional (keep it in round A/B arms for knowledge). *vc2sim:* passes under `core30`, `host30` and `edge31`; the mutation (stub without its flush) must fail under `core30` and `edge31`.
2. **The diagnostics above, with no image change, in the same sitting** [ours]: on any failure read `0x1C0`, `0x1E8`, `HP_CAPS`, the job block's `J_VPU`/`J_VPU2`/`J_VPU2_FAIL`, fault pc; count read-back mismatches only over the new image's text, rodata and data (below `__bss_start`), and report its `.bss` as "cleared" or not (a `crt0`-ran signature). *vc2sim:* the H1 model must leave `0x1C0 = 0`; an injected hang in `bschk_run` must leave `0x1C0` past `GPIO` and `J_VPU = 3`, `J_VPU2 = 0`.
3. **A/B the host form on silicon** (knowledge, not the design): within B13, alternate rounds of the cold form (`0xC000_0000`, wait for bit 0 to drop and then rise, `0`) and @25a's form, nothing else changed. If the cold form passes and @25a's fails, an enable edge is what discards instruction lines: record it in `boot-and-clocks.md` §2 and the hardware model. *vc2sim:* both arms under each knob; only `edge31` predicts a split.
4. **Change code the way Apple does: never replace the running image** [ours, Apple's model]. Keep one resident image and bring code in as overlays at addresses the old code never ran, the core flushing before the first call (our `J_OP_OVERLAY`, `dec-ovl.bin` and `viz-ovl-m.bin` already do this). For G9 that means the video decoder as an overlay or a resident image that grows, not a second whole image. Larger design change; the swap is then needed only for development.
5. **Restart through the start register without a power cut** (`0x1000_0400 = 0xA5A5_0002` on a running core, after the upload). Untested; what it resets (GPIO 48, the panel's reset line; the caches) is unknown, so it risks the panel. Only after recipe 1 fails.
6. **Cold restart** (Apple's method for a whole-image change): proven, 0.37–0.62 s dark. The host's fallback today and the baseline every recipe must beat.

## 7. Open questions only silicon can answer

1. Does a bit-30 request with bit 31 clear discard instruction lines? Does a 0 → 1 change of bit 31? Does it matter whether the core or the host makes the request? (Recipe 3; then `boot-and-clocks.md` §2.)
2. What holds stale code with bit 31 clear: a cache that stays on, a line buffer, or a prefetcher? (A stub that runs a known loop, has its bytes changed by the host, and reports which version ran, with and without each flush form.)
3. Did the 4c–4f round-0 failures stop inside `bcmfw_main` (H1) or later (H2)? (The 4f log's `diag` line; recipe 2 next time.)
4. Is the data side involved at all with bit 31 clear: are the core's stores ever held (the #40 mechanism), and are host-written data words ever read stale by the core?
5. Line size, associativity, and whether the vector unit's loads go through the data cache.
6. How long a flush takes at the core's clock, and whether bit 0 drops as soon as a new request is written (the stale-done hazard behind @25a's re-request). The B13 diag line's re-request and poll counts measure it from the host.
7. What a second write of the start register on a running core resets (recipe 5).

## See also

- [`quirks.md`](quirks.md) #40 (the unflushed stub), #41 (round 9; the 4f signature, re-read in §5.2 here), #47 (posted SDRAM writes), #51 (the vector form checks on silicon).
- [`boot-and-clocks.md`](boot-and-clocks.md) §2: the cache word and @25a in full.
- [`brief-map-power.md`](brief-map-power.md) §3 (hibernate) and §6 (caches).
- [`../research/bcm2722/warm-swap.md`](../research/bcm2722/warm-swap.md): the warm swap's design, B13 and its history.
- [`../data-paths.md`](../data-paths.md): the caches row.
