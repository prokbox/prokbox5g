# Memory audit, 2026-10-20: the budget rule, where the bytes are, what we adopt

Synthesis of three read-only audits and two follow-up studies run on `headers` `eb2f96dea5`. The full reports are in `docs/research/memory-2026-10-20/`:
- `mem-audit-A.md`: static RAM;
- `mem-audit-B.md`: code size;
- `mem-audit-C.md`: on-demand loading and BCM memory;
- `loader-study.md`: the module loader;
- `hwfit-reaudit.md`: generic code against this hardware.

Decisions are in §1, the inventory in §2, and what is adopted, with phase and tracker issue, in §4.

## 1. The budget rule (owner, 2026-10-20)

**Old rule:** "trim, don't raise" at `P9_END_MAX` 1,100,000 B (`_end` of `fw-ui`). It forced byte-level work (Thumb churn, 4 KiB page-step games) while the phases ahead need 100+ KB. The owner replaced it:

| Rule | Value | Notes |
|---|---|---|
| **Audio buffer floor (binding)** | **≥ 7,500,000 B on 32 MB units, ≥ 39,000,000 B on 64 MB units** | The playing/paused **audio file buffer itself** (owner chose "option 1"). Measured in the sim at 50k tracks after every runtime reservation the shell makes. A library update's free-heap dip may defer the update, but it must never push the audio buffer below the floor |
| Resident `_end` backstop | ≤ 1,500,000 B | Owner: "if we moved the cap to 1.5MB, that'd be easily worth it" |
| Firmware file (`rockbox.ipod`) ceiling | ≈ 1,250,000 B | Was stock − 20 % (914,813 B) |
| Spirit | "We should still get clever with it to try to keep things up" | Prefer on-demand loading, snapshot-first screens, BCM memory, and no idle statics. Growth stays deliberate and every budget delta is still reported |

**Measured today** (sim, 50k tracks, `docs/hardware.md` #85 rows):
- The audio file buffer is **15,079,816 B (32 MB)** and **48,634,248 B (64 MB)**. Against the floors that leaves about 7.6 MB and 9.6 MB.
- The figures 8,269,688 / 41,824,120 B are the **minimum free core heap at the commit peak** (two library snapshots), **not** the audio buffer. They were once mislabelled as "the audio buffer"; corrected here and in `hardware.md`.
- The commit peak sets *whether an update can finish without waiting for a stop*, not the floor. That makes the library RAM layout (R6) a responsiveness lever, not a floor emergency.

The 4 KiB step: `inquiry_static` (the static INQUIRY buffer, 4 KiB-aligned) makes `_end` move in 4 KiB steps; savings before it count only in multiples of 4,096. Under the 1.5 MB backstop this no longer binds. `validate.sh` should still print the gap (it was 524 B at `eb2f96dea5`) so that size reports read honestly.

## 2. Where the memory is (`fw-ui`, `_end` 1,092,208 B)

| Part | Bytes | Notes |
|---|---|---|
| text + rodata | 779,224 | shell 347 KB, rbcodec 82 KB, firmware 82 KB, tagcache 46 KB, playback 42 KB (sizemap groups) |
| data | 8,488 | |
| bss | 304,496 | LCD framebuffer 153,600; disk cache 32,768; tagcache stack 17,408; dir `open_streams` 11,520; buffering stack 9,216; `static_id3` 8,352; thread stacks 44,160 in total |
| padding | 910 | 524 before `inquiry_static`, 386 in small alignment gaps |

Other findings:
- **Release (`fw-rel`, no developer mode):** `_end` is 1,050,656 B, so developer mode costs 41,552 B.
- **The developer stock UI is smaller than it looked.** sizemap charged it the linker's merged 21.6 KB string pool. Real stock-UI code is about 20.8 KB, already Thumb.
- **Almost no dead code.** The only unreachable code is `lcd_write_yuv420_lines` (584 B), kept for Videos.
- **IRAM has no room for new code.** Core: 3.3-6.1 KB free (the two measurements disagree; re-measure). Codec window: ~272 B free.
- **The BCM2722 has its own 1.25 MB SRAM + 4 MB SDRAM,** mostly idle outside video. It loses everything when the BCM sleeps.

**Runtime reservations from the audio buffer** (audit A §6; sizes from docs and constants, to re-measure on the device):

**npfix (2026-10-23):** the art builder's pinned stack block grows by 51,752 B for good (the Now Playing read-ahead copy). Against the P16 close minimums (11,610,120 B on 32 MB, 44,443,576 B on 64 MB, `docs/hardware.md`) that gives, by arithmetic and not yet measured by the floor gate, **11,558,368 B against the 7,500,000 B floor (32 MB)** and **44,391,824 B against 39,000,000 B (64 MB)**: 4.06 / 5.39 MB of headroom. The floor gate (`tests/perf/floor_gate.sh`) re-measures it at the next full gate.

| Reservation | Bytes | When |
|---|---|---|
| list strip | 184,000 | shell start |
| row-art LRU | 109,428 | shell start |
| art builder pinned + decode | 54,007 + ~164,120; **+51,752** since npfix (2026-10-23): the Now Playing bundle copy (`npf_buf`, `ARTCACHE_NP_ONE_READ` rounded to 8) in the pinned stack block, so the builder thread reads a track's cover off the UI thread | shell start |
| drift slots | 2 × 180,000 (one on a tight 32 MB) | shell start |
| fonts | ~102,100 | shell start |
| Now Playing block | 58,368 | NP open |
| playlist | ~65,536 | boot |
| chapter table | 69,640 | playback init |
| `AUDIO_BUFFER_RESERVE` | 262,144 | playback |
| PCM buffer | ~500,000 (est) | playback init |
| library precompute | 25 B × N + 6,688 | shell start |
| view pool | 12 B × N + 64 KB | library load |
| tagcache RAM cache | ~5.5-6.9 MB at 40-50k (est) | boot |
| commit buffer | takes over the RAM cache | scan/commit |
| panel transient | 785,844 | stopped only |
| resident BCM image (the release line, `--bcmfw-merged`; `apps/video/bcmres.c`; added 2026-10-26, budget review of 0.8.0) | 214,356 (`BCMVIDEO_MERGED_BYTES`; cap `BCM_RESIDENT_BYTES` 218,112) | boot, pinned for good while the resident setting is on (default on); the floor gate's stand-in reserves the cap (`tests/perf/floor_gate.sh`, slot 4) |
| USB prefetch ring | 8 MB (4/2 MB fallback) | USB mode, idle audio only |

## 3. Studies

- **Code loader:** don't adopt Rockbox's `plugin_load`/`lc_open`. Write our own (`docs/design/module-loader.md`).
- **Generic code vs this hardware:** refreshed ranking in `docs/hardware-fit-audit.md` §"2026-10-20 re-audit".
- **Owner rulings folded in:**
  - **The Rockbox UI will be retired entirely,** with ProkBox's own developer UI in a black-and-white disk-mode look (#103).
  - **Snapshot-first, stream-behind for heavy on-demand screens.** Never for the main menu or split-screen lists, which are always drawn live; right-side panel images may be pre-rendered.
  - **Snapshots are rebuilt at three moments:**
    - the boot hold (a few seconds is acceptable; stock takes 5-10 s);
    - the post-USB busy moment;
    - a visible "Updating Language…".
  - **Predictive spin-up while navigating near uncached destinations** (#104).

## 4. What is adopted, by phase

| Phase | Item | Saving / value | Tracker |
|---|---|---|---|
| **P16 (first)** | Gate on the floors: sim floor trace at 50k for 32 and 64 MB, `_end` ≤ 1.5 MB backstop, firmware-file ceiling, print the `inquiry_static` gap | correct, enforced rule | new P16 issue |
| P16 | Module loader spike: cold Settings pack (12.7 KB out of `_end`) + a ProkBox "Loader & storage" developer screen (first snapshot tenant) in a 64 KB arena counted in the floor | proves the loader; the floor is honestly −52 KB until more tenants move | new P16 issue |
| P16 | R1 burst directory reads | ÷8-32 directory commands; faster scans, opens, post-USB pass | new P16 issue |
| P16 | R5 late allocation pool, or a shrink without a stop | ends "allocate only at shell start" and stop/restart gaps | new P16 issue |
| P16 | R4 queue by database index | play/shuffle start in ms; no 2,000 cap | new P16 issue |
| P16 | R6 fit the RAM library | −2.4 to −7 MB at the commit peak (est); updates finish without waiting | new P16 issue |
| P10 | M1 measurement session: inline USB-C meter, per-command latency (HX-B6), the IDE IRQ (DP-4), per-thread wake counter, codec memory peaks, floor trace on the device | turns the estimates into facts; answers the battery question | new P10 issue |
| P17 | R3 last-parent directory cache; #104 predictive spin-up | next-track open without a path walk | new P17 issue, #104 |
| P18 | R10 24 MHz while lit (E2 pushes); A3 adapter power-off after long idle | battery | new P18 issues |
| P19 | L2 our BCM image as the default display driver, then BCM memory roles: codecs, a stream buffer grown into the idle DPB, Cover Flow textures (~512 KB), second frame #76, strip framebuffer (~−130 KB) | floor and responsiveness | new P19 issue (with the BCM session, #60) |
| P9b | RUI-0..5: retire the Rockbox UI | release ~35-39 KB, developer build ~75-80 KB | #103 |
| P9b | Low-risk statics: bidi buffers 745 → 384 chars (2,888 B); `MAX_OPEN_DIRS` 32 → 16 after a high-water log (7,168 B); load-on-demand rodata (~40 KB: NP placeholders, glyph sets, panel/calendar/roller art); drop unused DSP stages (~10 KB) | ~60 KB | #44, #45 notes |
| Not adopted | Hot-path Thumb; risky stack/buffer cuts (disk cache, ID3 buffer, buffering stack, tagcache stack, Mersenne Twister); removing diagnostic strings or the Videos path | not worth the risk under the new rule | |

## 5. Device measurements needed

These go on the P10/M1 checklist:
- codec peak memory per decoder;
- module load latency on flash and on a sleeping HDD (with zero underruns), including an `lc_open` A/B to price the COP hop;
- the snapshot batch time;
- the floor trace on 32 and 64 MB;
- the BCM SDRAM upload rate;
- the core IRAM free bytes (resolve 3.3 KB vs 6.1 KB).
