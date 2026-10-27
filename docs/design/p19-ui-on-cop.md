# P19 19-8 and 19-9: the UI shell on the COP

*Names: technical names throughout: the **PP5022** and its cores, the **CPU** and the **COP**; the **BCM2722**; the **WM8758**. In conversation the owner calls them PP1 (CPU), PP2 (COP), VC and DAC.*

> **Status: S0 merged; S1 and the start of S3/S8 built on `p19-ui-cop` (2026-10-25, "As built" below). Design text from 2026-10-24.** Branch `p19-uicop`, cut from `p19-1c` (19-1a's quiet pollers and 19-1c's codec placement). Packages **19-8** (back-end services off the UI thread, and the boundary) and **19-9** (the UI on the COP) of `docs/phase19-brief.md` (branch `p19-brief`). **Owner decision, 2026-10-24: "Pin [decode] to PP1, UI to PP2 next."** Decode stays on the CPU for good; the UI front end moves to the COP. This is the standing split: CPU = playback back end, COP = UI front end, BCM2722 = display.
>
> **The BCM session's audit of 19-9 is folded in** (§6.1, Appendix A). They audited `forge/p19-bench-p12b` `fc9222f324`, with the docs on `p19-bcm-prep`. Their file:line references point into that tree's `bcmjob-*.c`, `e2-port.c` and `bcmfw-boot.c`, some of which are not on this branch. They are tagged **[bcm]**.

## As built (branch `p19-ui-cop`, from `p19` `799ff0f8d7`, 2026-10-25)

Pulled forward by the owner (2026-10-25: Now Playing lagged during MP3 playback, decode inline on the CPU beside the UI; "I'd rather not regress, I'd rather progress"). What landed, step by step, and where it stopped:

| Step | As built | State |
|---|---|---|
| **S0** | merged earlier (`2.29.5`); plus `p19-bcmjob-owner` `90b73ba334` merged here: `bcmjob_claim_owner()`, bcmjob-dev's init statics in `XCORE_BSS` (BCM session) | done |
| **S1** | `apps/ipodui/fe_be.h` (the contract: three ways across, the modes), `fe_be_core.{c,h}` (pure: the seqlock, the be_call slot and its single writers, the cache-operation table reusing `copj_cache_ops()`, generations, the canary sum), `fe_be.c` (the now record in `XCORE_BSS`, the playback events as back-end code, `be_post`/`be_call`, `be_svc`, the watchdog, the switch), `tests/febe/` (slot/gen/table tests, a two-thread seqlock fuzz, **the two-core cache-incoherence model**), `scripts/febe_link_check.py` (warn mode) | **done** |
| **S1 moves** | three families crossed for real: `queue_stopped` (Now Playing's row) is the now record's `FE_NOW_STOPPED`, written only by the CPU's playback events; pause/resume are `be_post`; all 20 `settings_save()` sites in the shell are `fe_settings_save()` (`be_call`) | done |
| **S3 (start)** | `HAVE_UI_THREAD_SPLIT` (developer builds): `ipodui_main()` splits at the job driver. Part one (log, boot records, sleep switch, clock policy, the BCM switch, `bcmjob_init()`) stays on main (CPU); `fe_be_start()` runs the rest of the shell (`shell_main()`) on a new `ui` thread (8 KB pinned `core_alloc` stack), which claims the job ring's owner first thing; main becomes `be_svc` | done, switch default off (`FE_INLINE`: today's shell, byte-for-byte the same order except `batcal_boot()`, which moved to the ui thread so its flush owner is the thread that flushes) |
| **S8 (mechanics)** | `FE_COP`: the ui thread created `IF_COP(, COP)` with a one-second start handshake under a corelock (a COP that never starts cannot run a second shell later), the fallback to a CPU thread, the cross-core cache operations live at each `be_call` | compiled; **refused at runtime** by `fe_cop_refusal()` with a log line, until S2–S4 and S10 land |
| **Watchdog (R7)** | a CPU tick task watches `button_queue`: input waiting 20 s untaken = the front end stalled; logs `fe stall`; in `FE_COP` it also sets the switch back to the CPU for the next boot (`FALLBACK`) | done (log only on the CPU) |
| **S2** (2026-10-26) | the per-pass services (log rotation, power log, battery session and calibration, the drained latch, the clock policy, the view pool's size) are one `be_call(BE_PASS)`: they run on `be_svc` while the ui thread waits, in their old order. `ui_log_flush()` and the storage-idle flushes route themselves to `be_svc` (`fe_be_route()`). `batcal_boot()` back on main; batcal's and ab_store's "flush on the owner thread" test is now "a service thread" (`fe_be_is_service_thread()`: `be_svc`, or the inline UI) | done |
| **S3** (2026-10-26) | **Remote calls for every direct back-end call**: `apps/ipodui/febe_rpc.list` (161 functions, generated from the build by `scripts/febe_rpc_gen.py --from-build`), the linker's `--wrap` (`febe_rpc.opts`, added by `apps/apps.make` to developer iPod Video firmware only) and a 5-instruction stub per function (`febe_rpc_stubs.S`): on the CPU it jumps straight to the real function (registers and stack untouched); on the COP `febe_remote` packs r0–r3 and four stack words, `be_call(BE_RCALL)` runs the function on `be_svc` (`febe_invoke`), and r0:r1 come back. Limits: ≤ 8 argument words, ≤ 64-bit result. **Every call from the COP is a full sync point on both cores** (COP: write back before, drop after; CPU: write back and drop before the handler, write back after). **Data:** the shell's cached `.data`/`.bss` on whole cache lines of its own; the 14 shell objects a CPU thread writes asynchronously (callbacks, timeouts, the art thread, tick tasks, the player's hooks) linked uncached; `global_settings` and `global_status` uncached (`apps/settings.c`; `firmware/target/arm/pp/app-pp.lds`), all under `HAVE_UI_THREAD_SPLIT` | done (device-only mechanics; the simulator links no stubs) |
| **S4** (2026-10-26) | `scripts/febe_link_check.py --strict`: **0 direct back-end references** (654 cross by remote call, 0 reviewed direct); `scripts/xcore_line_check.py`: **0 cache lines** shared between shell data and other data (4 before the placement); `scripts/febe_rpc_gen.py --check`: stubs in date | done (the checks are scripts; wiring them into `validate.sh` is the tooling owner's) |
| **Sim, S2–S4** | switch at `cpu`: 38/38 scripts (playback, USB, sleep, settings, OTG, vlimit, lock) pass on fresh disk clones; inline: 21/22 (the miss is `ipodui-p14a-settings`, a `sim-ui-video` script); `tests/febe`, `tests/copjob` pass | done |
| **S7 (part)** | the COP's log ring 4 → 16 lines (1,024 B), drained by `be_svc` on the first line after a drain (`fe_be_log_kick()`, `ui_log_drain()`) | done |
| S5, S6, S9, S10 | K-1 (owner A); the bench; the week; S10 is the BCM session's (`p19-s10-handoff`, not pushed at this writing) | open |

**Refusal reason 1 cleared (2026-10-26):**
- **The art cache is CPU-only in COP mode:** every front-end call into `artcache.o` (26 functions: the reads into the caller's buffer, the Now Playing bundle's take, the panel and window reads, the builder's controls) crosses by remote call, so the builder's buffers (`npf_buf`, the decode block, the chapter tables) are only ever touched on the CPU, and each read ends in a full sync point (the CPU writes back, the COP drops). The same for the services whose callbacks and timeouts run on CPU threads: `bootbatt`, `batcal`, `sc_hw`, `maint`, `lib_shuffle`, `ab_store`, `powerlog`, `devbench_vclk` (`febe_link_check.py` `SHELL_BE_OBJECTS`; 208 stubs in all). Pure helpers and uncached single-word reads stay direct (`SHELL_DIRECT`: the key functions, the size helpers, the three sequence counters).
- **Pins:** every block allocated from the COP is pinned at once by `be_svc` (`BE_RCALL` pins any `core_alloc*` result; `core_free()` ignores the pin; the shell's own pin/unpin pairs nest above it), which covers `maint`, `menu`, `qscroll`, `stopwatch` and every other shell block without touching them; the fonts are `font_lock()`ed at load (`theme.c`, `qscroll.c`), since `font.c` allocates them on the CPU.
- **Drawing data:** `font.o` and `lcd-16bit.o` (the glyph cache, the framebuffer, the viewport state) join the shell's cache-line group; `xcore_line_check.py` now reads archive members too: 0 shared lines in `fw-ui`, `fw-ui-mod` and the developer build.
- **The inline-mode tax (a):** only four objects stay uncached: `np_playback.o` (59 B of statics; the Now Playing screen calls it often, so routing it would cost a round trip per frame in COP mode), `sdk_cold.o` (1 B), and the developer labs `devbench.o` (2.6 KB, the bench runner) and `devbcm.o` (127 B). [estimate] A Now Playing pass touches at most a few dozen of those words: at ~10 extra cycles per uncached access, under 1,000 cycles per pass against the ~300k-instruction frame budget (< 0.3 %); the labs only while a bench runs. The rest of the earlier 6.9 KB is cached again.
- **Sim (reason 1 build):** switch at `cpu` 38/38 (the one first-run miss, `ipodui-hold-np`, was the parallel fixture build racing; alone it passes, as do the NP scripts after the S10 merge); inline 23/23 (set 1).
- **The gate:** `validate.sh` step "UI boundary": `febe_rpc_gen.py --check` always; `febe_link_check.py --strict` and `xcore_line_check.py --strict` on `fw-ui` when built.

**S10 merged (2026-10-26):** `p19-s10-handoff` `bb620db556` (the DECODE call record in the uncached request block, published piece buffers, read-back rows through the owner's uncached alias, the acknowledged withdraw); its owner hooks (`jd_cross()` and friends) follow the core of the thread that claimed the ring, so the ui thread's claim on the COP turns them on. Refusal reason 2 is gone.

**Silicon prep (2026-10-26):** `fe_fb_publish(rect, timeout_ms)` (`firmware/export/fe_fb.h`; LOCAL / CLEAN / PUBLISHED / TIMEOUT; the ui thread writes its cache back before its input wait and before every call to the back end, so a CPU push usually finds it CLEAN; else a fence through `button_queue` answered after `commit_dcache()`; a weak default in `firmware/common/fe_fb_stub.c`). MF-4 (`p19-mf4`, as built below) calls it from `lcd_update_rect`'s CPU path and `lcd_awake`, publishes the overlay on the COP before the post, reads through the uncached alias, and defines `HAVE_FE_FB_HANDOFF` (`config/ipodvideo.h`, developer firmware only): **with it, `fe_cop_refusal()` returns NULL in developer builds** (the switch still defaults to inline). A COP attempt sets the next start back to the CPU until it has run 120 s with no stall. The bench rows `c1` (B10), `ucop` and `ucop2` (2 min soak; bench-p19.md "The UI on the COP"). np-lag (`35e18fb464`, the `uilat` probe) merged: `uilat cpu` names the UI thread and reports the COP's idle; the wheel's post stamp is uncached; `clockpol.o` routed (playback events write it on the CPU).

**MF-4 done (2026-10-26, `p19-mf4`), as built.** `firmware/target/arm/ipod/video/lcd-fb-xcore.c`, contract in `xcore.h`:
- **Producer:** the ui thread claims the frame (`lcd_fb_claim()`, `shell_main()`, after `bcmjob_claim_owner()`); on the COP it drops its own stale lines once. The CPU's earlier lines (the boot frame) were written back by `create_thread()` on the other core. The predicate everywhere is `lcd_fb_cross()`: the caller's core is not the claimed one (the S10 `jd_cross()` pattern).
- **Consumers and their points:**
  - `lcd_update_rect()` from a CPU thread (the devtest's `bcm_video_hand_back`, any other CPU caller): `fe_fb_publish()` for the rect **before** the port lock, only with interrupts on; then every read through `lcd_fb_read()` (the uncached alias of the default frame; the CPU's own cache written back first). Engine 2 gets the same uncached address.
  - `lcd_awake()` (the wake frame; the shell reaches it by remote call, so it runs on `be_svc` with the ui thread blocked and CLEAN): the fence before `lcd_awake` takes its port lock, then `lcd_update_x(false)`, so no second fence under the lock.
  - vplayer's overlay: the shell (COP) `lcd_fb_publish()`es the rect before `queue_post(VQ_OVERLAY)` (it then sleeps until `ovl_done`, so it could not answer a fence); the video thread (CPU) reads through `lcd_fb_read()`. `ovl_done`, `ovl_rc`, `ovl_rect` and `ovl_slide` are now `XCORE_BSS` (the COP wrote the request and polled a cached `ovl_done` the CPU wrote).
  - `bcmjob-cpu.c` `PRESENT`: on the owner's core, the producer itself: no hand-off.
  - `lcd_blit_yuv()`: reads the caller's own YUV buffer, not the frame: no hand-off.
  - `power_off()`'s clear and push (`power-ipod.c`) is compiled out on the iPod Video (`HAVE_LCD_SHUTDOWN`), so no site there; `lcd_fb_take()` (fence, drop, become the producer) is the API for any CPU code that draws, host-tested, linked by nobody today. The shell build draws no panic screen.
  - The devtest's boot upload (`dt_upload_fb`) runs before the ui thread exists; its lab uploads (`dt_fb_upload`) run on the caller's thread, the producer.
- **Edges:** the frame is 16-byte aligned with 640-byte rows (40 lines), so full-width rects (all engine 2 pushes) are line aligned at both ends; a narrower rect's edge lines are only read, uncached, so neighbours do no harm. `lcd_fb_lines()` rounds a rect out to lines for the record (the PP5022's write-back is the whole cache).
- **Timeout:** a modal loop on the ui thread (the USB screen, prompts) does not serve the fence: `FE_FB_TIMEOUT` after 50 ms, counted (`lcd_fb_fence_fails`), noted (`fb fence timeout: pushed uncached (MF-4)`), and the push goes on uncached: stale pixels for that push at worst.
- **Cost:** every shell firmware since `p19-relsplit` (2026-10-26; developer firmware only before), the simulator none of it. With the UI on the CPU, a push pays `lcd_fb_cross()` (8 instructions: one `PROCESSOR_ID` read and one uncached word) and nothing else. Text about 0.5 KB, `.ncbss` 8 B.
- **Host test** `tests/lcdxcore` (the code as built, in a two-core incoherent-cache model, 300 seeds each): CPU push of the COP's frame, overlay, take, and the fence timeout clean; negative controls caught: no write-back in the fence 300/300, no fence 300/300, cached read 286/300 (overlay: 285/300; timeout row: 300/300), no publish 300/300, be_call without write-back 300/300, take without the drop 300/300.
- **`scripts/xcore_objects.txt`:** `lfx_core`, `lcd_fb_fence_fails` and the four overlay words must link uncached.

**What still keeps the COP from being the default:** silicon only (the stubs; engine 2 from the COP, B10; the bench rows `c1`, `ucop`, `ucop2`). Modules: Class A modules run on the ui thread and resolve symbols against the firmware without the stubs; their deny list (no file or allocation calls) is what keeps them safe.

**Bench sitting 3 and the fixes (2026-10-26, branch `p19-ucop-fix`).** Build `7e8c140e40-261026`, the UI on the COP through `ucop`/`ucop2`. **c1 PASS** (the COP ran 8160/8160, 3,338 us). **ucop2 FAIL**: `s=216 calls=28387 rcalls=27469 max_us=629711 stalls=0 coh=0 fbto=0`, answered "Glitches" [silicon]. The owner: (1) a tone while scrolling with no music, changing a little each detent and ending about a second after the wheel stops, in the developer menu and the main UI; (2) minor audio artefacts after starting music and going back to the menus; (3) slightly less responsive than inline, the developer menu worse; (4) with music, fast scrolling in the split menu leaves the right panel behind the cursor.

| # | Root cause | Evidence | Fix (forward: the UI stays on the COP) |
|---|---|---|---|
| 1 | **Each click started the mixer from the COP.** `input.c` `ui_click()` → `keyclick_click()` → `system_sound_play()` → `ipodui_click_play()` → `mixer_channel_stop()`/`mixer_channel_play_data()`, all on the ui thread, and nothing in the boundary covered it (no "audio out" family). The mixer is CPU-only code: the playback DMA interrupts the CPU's FIQ, `pcm_play_lock()` masks that FIQ with a plain depth increment (`pcm-pp.c`: "dual core support will require other measures"), and `downmix_index`, `next_size` and `idle_counter` are cached `.bss` the FIQ keeps hot. `mixer_activate_channel()` reset `idle_counter` in the COP's cache only, so the FIQ skipped the idle frames' clearing (`idle_counter <= 3`) and **replayed the click's 256-sample frame** (~172 Hz at 44.1 kHz) until its idle tail ran out; each new detent rewrote the frame (the change in tone). With the PCM stopped the COP ran `mixer_buffer_callback()` and `pcm_play_data()` itself on its own stale words | [ours] code reading; [measured] host model `tests/audioxcore`: as built 300/300 seeds wrong, up to 43 frames replayed; routed 0/300 | `system_sound_play()` asks `ipodui_sound_route()` first: from the split-off ui thread the sound is a `be_post(BE_SOUND)`, played by `be_svc` on the CPU (async: the ui thread never waits; at most 4 such posts in flight, since `be_queue` overwrites its oldest event when full and that could be a `BE_Q_CALL`: extras are dropped and counted, `post_drops` in the ucop2 line). `click.o` is CPU-only (`febe_link_check.py`); the link check has an "audio out" family (`mixer_*`, `pcm_play_*`, `pcm_set_*`, `beep_play`); `mixer_channel_play_data()`/`_stop()` carry `XCORE_ASSERT_CPU` (`xcore_offcore`, now in the ucop2 line). The developer lab's own mixer calls (`devbcm.o`) cross by remote call; `mixer_channel_status` is reviewed direct (`mixer_set_tap` was too, wrongly: ucop-fix2 S1) |
| 2 | **The DAC driver's state was cached on both cores.** The UI's loop on the COP polls the codec's standby (`dac_wait()` → `dac_idle_poll()`) and each click ran `dac_wake()` there; the CPU's `dac_wake()` runs before every PCM start. `dac_mtx` is uncached (S0) but `dac_state`, `pwr_reg`, `dac_busy_tick` and the shadows were not: after the COP put the codec in standby and the CPU woke it for music, the COP's cached `standby` still read 1, so the first clicks in the menus **replayed the standby wake** (PLL, 2 ms settle, DAC and output power, unmute) over live music. The mixer edits racing the FIQ from the other core add torn frames | [ours] code reading; [measured] the same model: cached flag 289/300 seeds replay a wake, uncached 0/300 | Every WM8758 shadow and standby word in `XCORE_BSS`/`XCORE_DATA` (`wm8758.c`; 120 B `.ncdata`); the piezo's `duration` and `keyclick_click()`'s statics uncached; clicks on the CPU (row 1) |
| 3 | **The COP ran unboosted, and the boundary made the CPU wait.** (a) The input boost keys on "decode on the UI's core" (`cp_input_boost()`), so with the UI on the COP it never fired while lit and playing; the inline shell had 80 MHz for a second after each input, the split ran at the lit floor on both cores (one clock). (b) `be_svc` at 16, below the codec band (13, `HAVE_SCHED_BANDS`): each call from the COP waited for the codec to yield, up to `SCHED_AGE_TICKS` (20 ms), while it decoded. (c) Sync points per pass and per key: `audio_status()` (once or twice a pass), `artcache_note_input()` and `clockpol_note_input()` (each key) were round trips, each a whole-cache write-back and drop on both cores; 27,469 remote calls in 216 s = 127/s. Without music only (c) applies: the developer menu's lists call more back-end functions per row (unprofiled: the profile below names them) | [silicon] the ucop2 counters; [ours] code reading (`clockpol.c` said "stays off by itself") | (a) `ui_competes_with_decode()`: the boost applies with the UI on the COP; (b) `be_svc` at `PRIORITY_CODEC` while split: the two take turns at every yield, neither starves; (c) `audio_status()` and `pcm_is_playing()` direct (`play_status`, `quiet_restart`, `pcm_playing` are uncached words with one writer; a build with recording refuses to compile), the input notes one `be_post(BE_INPUT_NOTE)` per event |
| 4 | **Panel lag with music:** the panel's art reads are remote calls (`artcache_panel_get`/`_get_cw`/`_keys`/`_count`/`_window`), each paying 3(a) and 3(b). Not a hand-off fault: the list and the panel are drawn by the same thread on the same core into the same frame, and ucop2 saw `coh=0 fbto=0` | [silicon] counters; [ours] code reading | rows 3(a), 3(b) |
| — | **The 630 ms call** (and 599 and 569 ms): `kind=8` (a remote call), each followed in `ipodui.log` by `stor pwr on ms=550..610 reason=access` and `m0 disk wake by=main rd`: the call's function read storage while the flash adapter was powered off (`HAVE_STORAGE_POWEROFF`). Inline, the same read blocks the UI as long; the boundary's own cost was not in it | [silicon] `ipodui.log` lines 137–139, 176–178, 202–204 | `call slow` now logs `fn=` (the function's address; map it with the build's `rockbox.map`) and `stor=`; `be_svc` notes a storage power-up per call; ucop2 judges `nostor_max_us` (calls without a power-up) and reports `stor_calls`, and asks `be_svc` for the **remote-call profile** (`fe rcall fn= n= ms= max_us=`, the 8 most called of 16 tracked) |

The rule, for any code the shell calls: **CPU-only unless it was written for two cores** (an interrupt on one core, a lock that masks one core's interrupts, cached state it keeps hot); **a cross-core mutex does not make guarded data coherent**. `docs/data-paths.md` R7 item 8 and D15; `docs/pp5022-reference/quirks.md` 54, 55.

Still open: the remaining per-pass round trip `clockpol_input_wait()`; the developer menu's hot calls (the profile names them); M-4 (the cost of a drop) unmeasured. (`sound_set_*()` and the DSP's setters from the COP: done in ucop-fix2, below.)

**Verified here:** builds `sim-ui`, `fw-ui`, the developer combined build + modules and stock `fw` with no new warnings; `febe_link_check.py --strict` 0 direct references; `febe_rpc_gen.py --check` in date (218 stubs); `xcore_line_check.py --strict` 0 shared lines; `xcore_map_check.py` all needs met (the one unlisted object, `modload.c:work`, predates this branch); `scripts/test.sh` all passed, `tests/audioxcore` included; sim scripts with the split at `cpu` 15/15 (`ipodui-p19-ucop-click`: all 13 clicks played by `be_svc`, `expect_playing ok` around the clicks over music) and inline 6/6. Silicon: none yet.

**Review fixes before the owner's bench (branch `p19-ucop-fix2`, 2026-10-26).** An adversarial review of the fixes above found the split safe as a developer switch and listed what had to change before the owner benches the UI on the COP:

| # | Finding | Fix | Evidence |
|---|---|---|---|
| S1 | `febe_rpc.list` had `direct: mixer_set_tap`, but its PCM lock (`pcm-pp.c` `sink_lock()`) does a plain `++locked` and masks only the calling core's FIQ: from the COP a lost increment races the CPU (silence until reboot), and `viz_tap_close()` could free the ring while the CPU's FIQ was still in the tap. `viz_tap_open/close` also assert the CPU, so the visualizer failed ucop2 (`offcore`) | `mixer_set_tap` crosses by remote call; so do `viz_tap_open` and `viz_tap_close` (`viz_tap.o` is a CPU-only shell object), so the hook, the clean of the ring's lines and the free all run on the CPU, where `pcm_play_lock()` preempts the FIQ: once it is held no callback is in the tap, and with the hook NULL none enters again. `viz_tap_read()` stays direct (the ring and head behind the uncached alias; the reader's `vt_n`, `vt_last_head`, `vt_track`, `vt_book` now `XCORE_BSS`). `mixer_set_tap()` asserts the CPU. `apath.c`'s tap state is uncached (the FIQ writes it, the UI reads it) | [code]; sim `ipodui-p19-viz-split` (split at `cpu`: the tap opens, reopens and closes; bars drawn from the tap; music plays throughout); `xcore_map_check` needs `viz_tap.c:vt_*` |
| S2 | The DSP's and `sound.c`'s state written from the COP: `ab_speed.c` `dsp_set_timestretch()`, `screens/eq.c` `dsp_set_eq_coefs/precut`, `dsp_eq_enable()`, `apath.c` `dsp_eq_enable()`, `sound_set_volume/pitch()` (`sound_prescaler` written from both cores) | Every such setter the shell calls crosses by remote call; the EQ's whole set is one call, `ui_eq_dsp_set()` (`audio_out.c`, CPU-only), which be_svc runs with no yield between the bands. The link check's "audio out" family gains `dsp_*`, `sound_set*` and `sound_settings_apply`, so a new direct call fails `--strict`; `tests/febe/test_policy.py` checks the lists | [measured] host model `tests/audioxcore`: the EQ set written on the COP 300/300 seeds torn or stale; one remote call per band 300/300 torn (the codec runs between calls); one call per set 0/300. The prescaler written on both cores: 226/300 seeds lose a write; on the CPU only 0/300 |
| S3 | be_svc served every remote call at the codec's priority, heavy ones too (playlist inserts, tagcache walks, `artcache_panel_run`) | be_svc waits for, and serves, calls (`BE_Q_CALL`) and clicks (`BE_SOUND`) at the codec's priority and everything else (input notes, pause and resume, the log drain, the stall report) at its own; `svc_prio()` changes it only on a change. ucop2 now reports `underruns=` (the PCM's, read by remote call) and fails on any | Sim `ipodui-p19-svc-prio` (split at `cpu`, music playing, four settings saves served by be_svc and fast wheel notes: both priorities seen, `expect_playing ok`, no `pcm underrun` line). A heavy remote call cannot be staged in the simulator: it has one core, so the split at `cpu` makes the shell's direct back-end calls on its own thread (no stubs); the settings save is the heaviest call be_svc serves there. On the device: the ucop2 row's `underruns=` |
| S4 | `playback_status()` read the status, then the flag: a status from inside the quiet restart's window (the stop) paired with the flag read after it closed said "stopped" | The status, the flag, the status again; retried (at most 4 reads) when the status changed. Comment fixed | [code] |
| N1 | `system_sound_play()` posted a click even with the Clicker off | Returns first when the sound's setting is off | [code]; `ipodui-p19-ucop-click` |
| N2 | Input notes used the four bounded post slots, so a fast wheel could drop clicks | Latched: one `BE_INPUT_NOTE` in `be_queue` at most (`note_posted`/`note_taken`; taken before the notes run, so none is lost) | [code]; `ipodui-p19-svc-prio`, `-ucop-click` |
| N3 | +472 B in the release build for code it can never run | Without `HAVE_UI_THREAD_SPLIT` the split's machinery is compiled out (the start, be_svc's loop, the watchdog, the posts' bookkeeping, `ipodui_sound_route()`), and `play_status`/`quiet_restart`, keyclick's statics and the piezo's `duration` stay cached; the WM8758's shadows stay uncached (both cores drive it) | [measured] release `_end` −4,464 B (`.text` −2,936 B) against the merge base; changes-from-rockbox 2.36; **superseded 2026-10-26** (`p19-relsplit`): the release build carries the split again, since 19-9 ships in 0.8.0 (release `_end` +10,816 B net, `docs/hardware.md`) |
| N5 | The ledger | `docs/changes-from-rockbox.md` 2.35 (the release cost, the playback.c guard) and 2.36 | — |

**Bench rows for sitting 4** (each with the UI on the COP after `ucop`; Clicker On):

| Row | What the owner does | Question (answers) | Confirms |
|---|---|---|---|
| `uck1` | Music stopped: scroll Songs up and down for 20 s, then stop and listen for 5 s | "Besides the clicks, did you hear a tone or hum?" (No / Faint / Clear) | Fix 1 |
| `uck2` | Play a song from Songs, MENU back to the main menu, scroll for 20 s | "Did the music glitch, pop or crackle while you scrolled?" (Never / Once or twice / Often) | Fix 2 (and 1 with music) |
| `ufeel` | Scroll Songs fast, then the developer menu | "Compared with the normal UI, scrolling felt…" (Faster / The same / A little slower / Much slower) | Fixes 3(a)–(c) |
| `upanel` | Music playing, main menu, spin the wheel fast for 10 s | "Did the right-hand panel keep up with the highlighted row?" (Kept up / A little behind / Far behind) | Fix 4 |
| `ucop2` (automatic, as before) | the 2 min soak | — | `offcore=0`, `underruns=0` (ucop-fix2), `nostor_max_us` < 250 ms, no stall or coherence failure; the `fe rcall` profile and any `call slow fn=` lines go to the log |

**Costs (developer combined build + modules):** `_end` 1,901,744 B (+6,816 against the base, 1,894,928; S1 alone was +8,736, mostly alignment); `.ncbss` 16,632 B (+10.4 KB: the settings, the 14 objects); the 161 stubs are ~4.5 KB of text; on the CPU each wrapped call costs 6 instructions and one peripheral read; 6.9 KB of the shell's statics are now uncached in developer builds (a speed cost on both cores for those 14 objects, unmeasured: M-4).

**Why the COP flip still refuses** (`fe_be.c`, `fe_cop_refusal()`): the shell's loop still calls the back end directly (`scripts/febe_link_check.py` counts them; see the numbers in the hand-off) — buflib, file I/O, the playlist and the database from the COP would put dirty COP lines over CPU-written structures (the open-file table, the FAT cache, buflib's handle table): silent filesystem or database corruption, not a crash. And the BCM job driver's DECODE call record, result and read-back rows sit in the art thread's cached memory (S10). A switch the owner may flip must not be able to do that, so `cop` in the switch runs the ui thread on the CPU and says so in the log (`fe cop refused: ...`).

**The switch.** Read once at boot (`fe_be.c` `mode_read()`), first match wins: (1) the **boot guard**, `/.rockbox/ui_guard`, holding this build's version string: a COP attempt of this build has not yet held for 120 s, stalled, or the COP never started, so the ui thread runs on the CPU (a new build ignores and removes another's guard, so an update or the 0.8.0 flip tries the COP afresh); (2) developer builds only: Developer › "UI thread: inline / cpu / cop (now …)", a marker file `/.rockbox/ui_split` (`inl`, `cpu` or `cop`; absent = follow the next rules; choosing clears the guard); (3) the hidden `config.cfg` key `ipodui ui core` (0 the build default, 1 inline, 2 cpu, 3 cop; no menu shows it); (4) the build default `IPODUI_UI_CORE_DEFAULT` (`firmware/export/config/ipodvideo-variant.h`: 0 inline today). **Release builds** (`--ipodui-release`, no developer menu) have the split since `p19-relsplit` (2026-10-26) and choose by (1), (3), (4): **the 0.8.0 flip, once bench 4's 19-9 rows pass, is the one line `IPODUI_UI_CORE_DEFAULT 0` → `2`.** A COP boot writes the guard at its start and removes it once it has held (two small writes in the boot window, as the developer marker did before); a stall re-arms it. Log lines: `fe be_svc up mode=`, `fe shell up mode=`, `fe cop refused:`, `fe FALLBACK ...`, `fe stall ...`, `fe call slow kind= us=` (> 50 ms round trip: the latency probe for the boundary), `fe call stall` (a reply not in 10 s; the call is never withdrawn), `fe coh fail what=now|slot|call` (the canaries).

**Measured** (`arm-none-eabi-size -A`, against the base `p19` + `p19-bcmjob-owner`): `fw-ui-mod` `.text` +3,696, `.rodata` +564, `.ncbss` +528, `.bss` −160, `_end` +4,624 B (1,298,304). Developer combined build (`--ipodui --bcmfw-devtest --bcmfw --bcmfw-video --bcmfw-l1 --bcmfw-merged` + modules): `.text` +3,736, `.rodata` +572, `.ncbss` +544, `.bss` +3,888 (symbols add ~600 B; the rest is alignment padding after the shift), `_end` +8,736 B (1,903,664). No new warnings. Host: `tests/febe` (seqlock 0 torn over ~40k reads against a hammering writer; cache model: three protocols clean over 300 seeds each, four omissions caught 300/300). Sim (`sim-ui`, scripts run directly, not through the gate): 23 playback/USB/sleep/settings/OTG scripts pass with the switch at `cpu` (goldens, trace rules); with it at `cop` the log says `fe cop refused: one core` and the shell comes up on the CPU thread; inline (the default) 39/41 of the same plus 18 settings scripts. The misses: `ipodui-p14a-settings` (needs `sim-ui-video`, not a regression) and `ipodui-p11-vlimit-flow/-menu`: disk state, not this branch. My runner reused one disk clone per worker across scripts (the gate gives each script a fresh primed clone); on one such used clone the base build (`p19` + `p19-bcmjob-owner`) and this branch fail `limit_unlocked_max` identically (90.833 %), and `-menu` passes in both modes when run on its own.

**Device validation (the practical way).** It ships behind the switch, default off. The owner sets "UI thread: cpu" and uses the unit; the log shows `fe be_svc up mode=cpu`, `fe shell up mode=cpu`, and any `fe stall`, `fe call slow`, `fe coh fail` or `FALLBACK`. That proves the split on silicon (stack depth on the 8 KB DRAM stack, thread slots, be_svc ordering) before any COP step. **The one bench viability row, when the COP flip is unblocked:** engine 2 started and polled from the COP (B10, §6.4 q-1; the BCM session's first condition) — one COP job that pushes a 64 KB piece and reads it back. Until it passes, Q-U2's default (`BE_PUSH` on the CPU) stands.

**For the flip (BCM session, S10 on `p19-s10-handoff`, local and WIP at hand-off, not merged here):** start the owner on the COP and call `bcmjob_claim_owner()` before its first job call with no art call in flight; the art builder keeps its read buffers alive (a LEAKED read may get one more row); check on silicon that `jd_uncached` covers the art builder's buffers.

**Owner hooks for the job driver** (BCM session, 2026-10-25): `bcmjob_init()` on the CPU at start (it allocates and pins), `bcmjob_claim_owner()` first thing on the ui thread. `ui_log` from the COP is safe (`uilog.c`'s COP ring, 4 lines, `XCORE_BSS`; overflow counted as `cop_lost`): **S7 must grow it** (design 2 KB) before a COP UI, which logs far more than 4 lines between drains.

**Evidence tags.**
- **[code]**: read in this tree (`p19-uicop`, from `p19-1c`), with file:line.
- **[silicon]**: measured on our unit, quoted from the reference manuals or logs.
- **[rockbox-derived]**: a register or behaviour fact taken from Rockbox's code.
- **[bcm]**: the BCM session's audit or contracts.
- **[hypothesis]** and **[estimate]**: as they say.

**Read with:**
- `docs/phase19-brief.md` (branch `p19-brief`): §3.5 and §3.6, 19-1d, 19-8, 19-9, Q6, M-4, M-5 and B10;
- `docs/design/p19-placement.md` §3 (branch `p19-brief`);
- `docs/data-paths.md` R7;
- `docs/pp5022-reference/cores-and-threads.md` and `quirks.md`;
- `firmware/export/cop_job.h`.

---

## 0. Summary

1. **The kernel already supports the move.** These pieces already work across cores [code]:
   - two cores (`config.h:1286-1300`);
   - Peterson corelocks (`firmware/asm/arm/corelock.c:30-100`);
   - queues, mutexes and semaphores, each with its own corelock (`mutex.h:32`, `semaphore.h:31`, `queue.h:148`);
   - immediate cross-core wakeups: `core_schedule_wakeup()` calls `core_wake()` (`thread.c:420-428`);
   - a cross-core-safe `cpu_boost` (`system.c:35-36`, `:198-244`, under `cpufreq_cl`);
   - a working COP worker channel, `cop_job` (`firmware/common/cop_job.c`), which ran FLAC decode on silicon for over an hour (S36).

   **What is missing is the shell's discipline, not the kernel's.** All but one of the shell's statics are cached `.bss` (§2). The shell makes several hundred direct calls into back-end code (§4). Several kernel objects that both cores will touch sit in cached memory (§3.2).
2. **The plan splits the thread first, on one core.**
   - **19-8** turns today's main thread into the back end's **service thread `be_svc`**. It keeps the 8 KB IRAM stack and stays on the CPU.
   - The shell's loop moves to a **new `ui` thread**, still on the CPU, behind the boundary header `apps/ipodui/fe_be.h`.
   - Every back-end call crosses that boundary as one of three things: an async post, a synchronous `be_call()`, or a read of published state.
   - **19-9** is then one argument: the `ui` thread is created `IF_COP(, COP)`, with the cache operations the boundary already names.
   - Regressions from the split show up on one core as ordinary bugs (brief 19-8).
3. **Coherency comes from one rule set** (§2):
   - one writer per structure;
   - small state in one seqlocked "now" record in uncached memory;
   - bulk tables handed over by generation;
   - every boundary crossing is a *sync point*: the COP writes back before a post and writes back and drops after a reply, and the arguments are copied into an uncached record;
   - the COP never holds a raw buflib pointer to a movable block;
   - nothing is allocated, pinned or freed on the COP.
4. **The BCM2722 port** (§6): one cross-core lock, held in uncached memory. Power transitions (`lcd_awake`, `lcd_sleep`, `bcm_init`, the boot ladder) stay on the CPU. The COP owns the drawing ring; the CPU's requests reach it as flags it applies at `enter()`. A COP-side engine-2 start is unproven (Apple refuses it), so the default keeps a CPU push service as the fallback.
5. **Power** (§5): one clock serves both cores. Dark playback is unchanged from 19-1: the COP stays parked by K-1, because the UI waits with `TIMEOUT_BLOCK` (`ui_main.c:1068-1073`). The lit gain is mainly **smoothness** (UI frames no longer compete with decode on one core); the current gain is uncertain, 0 to 2 mA [estimate]. That puts Q6's "drop it under ~2 mA" gate at risk, so §8 Q-U1 proposes a default gate that also counts smoothness.
6. **Cost** [estimate] (§9):
   - about **+12 to 13 KB resident**: the 8 KB `ui` stack plus about 4 to 5 KB of shared records (§9);
   - **+8 to 14 KB of code**;
   - no IRAM change;
   - well inside the 1.5 MB `_end` cap and the audio-buffer floors.

---

## 1. What runs where

### 1.1 Threads

The "today" column is the `HAVE_IPOD_UI` firmware for the iPod Video, as of `p19-1c`. Stack sizes and priorities are [code].

| Thread | Stack (where) | Prio | Today | After 19-8 | After 19-9 | Notes |
|---|---|---|---|---|---|---|
| **main** (`ipodui_main()`, `apps/main.c:278`) | 8 KB IRAM `.stack` (`app-pp.lds:135-141`) | 16 | CPU: the whole shell | CPU: **`be_svc`**, the back-end service loop | CPU: `be_svc` | Keeps its IRAM stack: no new CPU stack, and no IRAM change |
| **ui** (new) | 8 KB, pinned `core_alloc`, DRAM (§3.4) | 16 | — | CPU | **COP** | The shell's loop, input, screens, rendering, `ui_fb_settle()` |
| audio | 0x1400, DRAM (`audio_thread.c:65,181`) | 15 | CPU | CPU | CPU | |
| codec | 0x2400, IRAM (`codec_thread.c:115,709`) | 13 | CPU | CPU | CPU | Decode is pinned here (owner, 2026-10-24) |
| "codec cop" worker | 0x2400 in the codec slot (`codec_cop.c:76,618`) | 13 | COP when lit (19-1c) | same | **not started** | `HAVE_COP_CODEC` leaves the default build under `HAVE_COP_FRONTEND` (BCM-audit default) |
| "mp3dec" synthesis | 0x400, IRAM (`mpa.c:289,351`) | 16 | COP when lit (19-1c iii) | same | **inline on the CPU** | 19-1c (iii)'s switch is forced to "inline" |
| buffering | 0x2400 (`buffering.c:187,1869`) | 15 | CPU | CPU | CPU | |
| tagcache | 0x4400 (`tagcache.c:346,12109`) | 20 | CPU | CPU | CPU | Also runs `UI_COMMIT` work (`:12105-12108`) |
| artcache | 0x2400, pinned block (`artcache.c:726,7790`) | 20 | CPU | CPU, plus the NP and row-art reads (placement §3.2) | CPU | The only `create_thread` in the shell |
| storage ("ata") | `storage.c:76-92` | 16 | CPU | CPU, plus the log, powerlog, batcal and bootbatt services on its idle callback | CPU | |
| backlight | 0x400 (`backlight.c:130,931`) | 16 | CPU | CPU | CPU | Calls `lcd_awake`/`lcd_sleep`, so its port work stays on the CPU (§6) |
| power | 0x200 (`powermgmt.c:1179`) | 18 | CPU | CPU, plus `clockpol_service()` | CPU | Also the dark deadline for the alarm (placement F12) |
| usb | `usb.c:106-110,780` | 18 | CPU | CPU | CPU | |
| scroll | 0xC00 (`scroll_engine.c:213,220`) | 16 | CPU | CPU | CPU | The shell has its own marquee; `scroll_queue` is SHAREDBSS (`scroll_engine.c:65`) |
| piezo | 0x400 (`piezo.c:28,190`) | 4 | CPU | CPU | CPU | **`piezo_queue` is cached (`piezo.c:30`)**, so it must move to SHAREDBSS (§3.2) |
| iap | 0x2000 (`iap-core.c:99,464`) | 18 | CPU (unless `IPODUI_TRIM_IAP`) | CPU | CPU | |
| vplayer | `vplayer.c:1286` | 14 | CPU | CPU | CPU | Holds the port lock per piece (§6) |
| voice, dircache, playlist cachectrl, usb prefetch | — | — | CPU, when compiled | CPU | CPU | Off in the shell build, or developer-only |
| idle | 0x80 per core, IRAM (`app-pp.lds:121-133`) | — | both | both | both | |

**Thread slots:** +1 (`ui`) against `BASETHREADS` 16/17 (`thread.h:72-78`). In 19-9 the COP worker is not started, which frees a slot back.

### 1.2 Interrupts: every source and its core

This is the PP5022's dispatch [code]. The single `irq_handler` branches on `CURRENT_CORE` (`system-pp502x.c:62-210`); the COP branch handles only Timer 2 (`:206-209`).

| Source | Core | Handler, and how its result reaches the UI | Changes with 19-9? |
|---|---|---|---|
| 0 Timer 1 (the tick) | CPU (`kernel-pp.c:50`) | `call_tick_tasks()`: `button_tick`, `lcd_tick`, backlight and timeouts. Then `core_wake(COP)` on every tick (`kernel-pp.c:36`) | K-1 turns the pulse into "wake when the COP's deadline is due" (§3.1) |
| 1 Timer 2 | either (`timer-pp.c:68-77`) | user timer, unused by the shell | no |
| 20 USB | CPU (`system-pp502x.c:74`) | usb thread, then `SYS_USB_*` broadcast to `button_queue` | no |
| 23 IDE | CPU (`system-pp502x.c:79`) | the ATA DMA wait | no |
| 26 DMA (PCM) and 10 I2S; FIQ | CPU (`pcm-pp.c:197,256,715`) | audio | no |
| 40 I2C: **the clickwheel** | CPU (`button-clickwheel.c:375-376`; handler `system-pp502x.c:93-96`) | `ipod_4g_button_int()` reads the wheel (`button-clickwheel.c:344-357`) and posts wheel events with `button_queue_post()` (`:265,269`) into SHAREDBSS `button_queue` (`button_queue.c:31`). Button state goes through `button_tick` | no: the post already wakes a COP waiter at once (`queue.c:410-431`, then `thread.c:420-428`) |
| 32-34 GPIO (USB insert, GPIOL bit 4) | CPU (`system-pp502x.c:104-108`) | usb | no |
| 36/37 serial | CPU | dock serial | no |
| 4 mailbox | none (unused by Rockbox) | — | Optional doorbell (§8, hardware features); not needed |
| 27 engine 2 (hypothesis) | none: polled (`e2port_busy`) [code; bcm] | — | Stays polled. Who polls follows who starts (§6) |

**No interrupt moves to the COP.** The COP takes no IRQ, as in `fit2-brief.md` §3.1. The clickwheel stays a CPU IRQ.

### 1.3 How the UI receives events across cores

| Event | Producer (core) | Path to the `ui` thread | Already cross-core safe? |
|---|---|---|---|
| Wheel, buttons | I2C IRQ, then `button_tick` (CPU) | `button_queue_post()` into `button_queue` (SHAREDBSS) | **yes** [code] |
| Hold, the backlight going off | backlight thread (CPU): `SYS_IPODUI_BACKLIGHT_OFF`, `SYS_IPODUI_HOLD_CHANGED` (`backlight.c:540,1051`) | `button_queue` | yes |
| Sleep timer, idle off | power thread: `SYS_SLEEP_REQUEST` | `button_queue` | yes |
| USB connect and disconnect | usb thread: `queue_broadcast` (`usb.c:940,970`) | `button_queue` (made with `queue_init(q, true)`) | yes. The ack (`usb_acknowledge`) posts to SHAREDBSS `usb_queue` (`usb.c:125`) |
| Timers (`header_wake`, NP clock, glides, the alarm lit) | the `ui` thread's own deadline | `queue_wait_w_tmo` timeout; the COP sees it at the next pulse (≤ 10 ms) or, after K-1, at its armed deadline | yes. After K-1 a COP timeout depends on the CPU's tick checking the COP's deadline word |
| Playback events (`TRACK_CHANGE`, `STOP_OR_PAUSE`, `TRACK_FINISH`, `BUFFER_FILL_DONE`, `REBUFFER`) | `send_event` on the **poster's** thread (audio or buffering, CPU) | **today: shell callbacks that write cached shell statics on the CPU** (`ui_main.c:218-228,1217-1218`; `np_playback.c:140-160,226-230`; `lib_shuffle.c:141-142`; `maint.c:359`; `sc_hw.c:99`; `artcache.c:7784-7787`) | **no.** These callbacks become **back-end code**: they stay on the CPU, write only `be_svc`-owned state or the now record, and wake the UI with one post `SYS_FE_STATE` (§2.3) |
| Disk spin-up (`DISK_EVENT_SPINUP`) and storage idle | storage thread | `uilog.c:499`, `powerlog.c:788`, `batcal.c:376`, `ab_store.c:858` | back-end services after 19-8; never the UI's |
| `timeout_register` callbacks | the tick ISR (CPU) | `bootbatt.c:377`, `batcal.c:377`, `powerlog.c:799` | back-end services after 19-8 |
| Replies from `be_call()` | `be_svc` (CPU) | a per-request semaphore, then `core_wake` | new (§2.3) |
| The art builder's results | artcache thread (CPU) | generation word plus `SYS_FE_STATE` | new (§2.2) |

**`default_event_handler()`** (`apps/misc.c:829-902`) runs on the `ui` thread. It calls the USB screen (`gui_usb_screen_run`, `:868`), which draws, acknowledges and blocks; that is front-end work and fine on the COP. For `SYS_POWEROFF`, `SYS_REBOOT` and the charger events (`:878-902`) it runs shutdown work (settings save, storage flush, `power_off`). **Under the boundary those become `be_call(BE_SHUTDOWN, …)`** (§4).

**`action.c`'s input-thread check** (`action.c:1248-1262`, `action_set_input_thread()` at `ui_main.c:1143`) keys on `thread_self()`. Thread ids are global, so it works on the COP once the `ui` thread calls it [code].

---

## 2. Coherency

### 2.1 Facts that bind every choice

- **No coherency between the cores, or with DMA.** Maintenance is whole-cache only: `commit_dcache()` writes back, `commit_discard_idcache()` writes back and drops (`system-pp502x.c:238-294`), and each core runs it on its own cache [code; rockbox-derived] (quirks 8-10).
- **Uncached memory:** `UNCACHED_ADDR(a) = a | 0x1000_0000` (`system-target.h:161-165`). `SHAREDBSS_ATTR` is `.ncbss`, linked at the uncached alias (`config.h:1282-1291`, `app-pp.lds:62-72,153-160`). IRAM is uncached [code].
- **Kernel objects are coherent only if the object itself is uncached.** `queue.h:114`: "Must be statically allocated to put in non-cached ram". A corelock in cached memory is a broken Peterson lock [code].
- **No cache work on an ordinary `switch_thread`.** The kernel cleans only:
  - when it creates a thread on the other core (`thread.c:1235-1238`);
  - when a thread changes core (`thread-pp.c:128,136`);
  - at a COP thread's exit (`thread-pp.c:93`) [code].
- **Uncached is not atomic.** Each shared word has one writer; anything with two writers takes a corelock (brief §3.5 item 6). Counters that both cores write are per-core words, summed by the reader (D10: `b4e8fc5194`).
- **A dirty COP line over a DMA target is a corruption bomb.** If the COP holds dirty lines over a buffer that ATA DMA then fills, a later eviction writes the stale line over the new data. So **the COP writes back before handing any buffer to the back end**, not only before the back end reads it.
- **Whether the two cores share one 8 KB data array is unknown** (`cores-and-threads.md` §3) [bcm: open]. The design assumes the worst case (separate arrays, fully incoherent). If they share it, some write-backs become redundant. None becomes wrong.

### 2.2 Every shared structure and its strategy

"FE" is the `ui` thread (the COP after 19-9). "BE" is any CPU thread.

| Structure | Where it lives today [code] | Writers, then readers | Strategy |
|---|---|---|---|
| **Play state:** status, elapsed, length, volume, repeat and shuffle, buffer state, `queue_stopped` (`ui_main.c:223,227`), NP's `pb_track`/`pb_seq` (`np_playback.c:146-148`) | cached shell statics, set by CPU callbacks | BE, then FE | **The now record** (`fe_now`, §2.3): SHAREDBSS, about 128 B, one writer (`be_svc` and the event callbacks, serialised through `be_svc`), and a seqlock. The callbacks stop writing shell statics |
| **The playing track's id3** (`audio_current_track()`, 38 sites; `static_id3_entries`, `playback.c:196`, cached; `id3_mutex` SHAREDBSS, `playback.c:205`) | cached `mp3entry` (about 1.3 KB each, with inline strings) | BE (audio, buffering), then FE | **A snapshot copy:** `fe_track[2]`, a double buffer of the fields the shell shows (title, artist, album, album artist, composer, genre, year, length, track and disc numbers, the chapter count, the art key, the database index), about 640 B each, in the shared block (§3.4). `be_svc` fills the back buffer under `id3_mutex` on `TRACK_CHANGE` and metadata changes, then flips the generation. **FE never dereferences an `mp3entry`.** The `te->id3` uses in shell callbacks stay CPU-side |
| **Next and previous track** (`audio_next_track()`, `artcache.c:936,978`) | as above | BE, then FE (art prefetch) | Moves into the art builder (CPU), which already owns the read-ahead |
| **The playlist** (`current_playlist`, `playlist.c:238`, cached) | cached | BE (audio, `be_svc`), then FE: about 127 sites (`playlist_amount` 22, `playlist_get_current` 23) | Counts and the current index go in the now record. Track names for a list screen come by **`be_call(BE_PL_RANGE, first, n)`**, copied into the FE's buffer (one call per screen, never one per row). Edits (`playlist_insert_*`, `_start`, `_create`, `_delete_range_ipodui`, `_reorder_ipodui`) are `be_call`s |
| **`global_settings`** (`settings.c:86`) | cached `.bss` | FE (about 48 write sites), and BE reads it (`playback.c` 22 reads, `pcmbuf.c` 9, `codec_thread.c` 1) | **FE owns it; changes cross as an apply.** FE writes, then `be_call(BE_SETTINGS_APPLY, mask)`. The call's sync point carries the write-back; `be_svc` reads through the uncached alias or after its drop, then runs `settings_apply` or the part named by the mask, and `settings_save` (a file write) runs on `be_svc`. **The BE never writes `global_settings`.** Audit that in 19-8a: a grep plus the link check. None of the BE reads is in a per-sample loop [code: the counts above; hypothesis that all are set-time reads, to be checked file by file in 19-8a]. Fallback if the audit finds BE writers: link `global_settings` into `.ncbss` under `HAVE_COP_FRONTEND` (about 3 to 4 KB of uncached reads; measure the cost first) |
| **`global_status`** (`settings.c:87`) | cached | BE (resume info), and FE writes one site (`np_playback.c:712`) | **BE owns it.** The FE write becomes `be_post(BE_STATUS_SET, …)`. FE reads the fields it needs from the now record |
| **The tagcache RAM structures and view pool** (`tagcache_ui_*`, about 330 calls; contexts `tagcache_context_acquire`/`_release`) | cached, built by the tagcache thread | BE builds, then FE reads | **Generation hand-off** (placement §3.1): BE builds a candidate, writes back, and bumps `fe_gen.lib`. FE sees the bump, writes back and drops once, then reads. Writers never run alongside readers of the same region (#85's candidate and commit already work this way). `lib_open()` view builds become a BE job (placement §3.2) that publishes a view by generation. **Searches that grow the RAM cache, and `tagcache_search`/`_retrieve` (file reads), are `be_call`s**, batched by screen |
| **The art cache:** row-art LRU, `pv.slot`/`pv.slot2`, `npf_buf`, the panel pool | pinned `core_alloc` (`preview.c:537,746`; `artcache.c:7763`) | BE (artcache thread) writes, then FE reads. FE writes the panel compose | **Generation per slot,** plus a pin for life. The art thread writes back after filling a slot and bumps that slot's generation word in `fe_gen`. FE drops before the first read of a new generation. D-4's bank swap (`bank_swap()`, `preview.c:744-770`) becomes a generation flip. **FE resolves each pointer once, at pin time** (§2.4) |
| **buflib and `core_alloc`** (no lock [bcm, must-fix 5]) | — | — | **All allocation, freeing, pinning and unpinning on the CPU** (`be_call(BE_ALLOC …)`). FE holds only pinned blocks, never movable handles (§2.4) |
| **The strip** (`UI_STRIP_SIZE` 180,000 B, `anim.c:133`; `ui_strip_claim()` pins, `:158-159`; `ui_strip_put()` unpins, `:170`) | `core_alloc` | FE only (nine owners, all front end) | **Pinned for the shell's life** under `HAVE_COP_FRONTEND`. The claim/put pairs stay as ownership markers but no longer unpin. The USB screen frames and On-The-Go I/O over 64 KB (`otg.c:95`) do file I/O into the strip: the COP writes back before the `be_call`, which reads through the uncached alias (or by ATA DMA), then the COP drops after the reply (the `be_call` rules) |
| **The framebuffer** (`lcd_static_framebuffer`, cached `.bss`, `lcd-color-common.c:38-39`) | static | **FE writes.** BE reads: the wake frame in `lcd_awake` (`lcd-video.c:1156`, `lcd_update()` on the backlight thread); vplayer's overlay (`vplayer.c:1038` [bcm]); module snapshots read `fb_ptr` (`modload.c:48,1594,1667`, front end) | **Pixel hand-off contract** (§6.3, BCM must-fix 4). FE writes back after composing any frame that a CPU reader can consume (the cost is folded into the push, which already calls `commit_dcache()`, `e2-port.c:118`). **CPU readers read through the uncached alias** (`UNCACHED_ADDR(FBADDR(…))`) so they cannot hit their own stale lines. `lcd_awake`'s first push (`lcd-video.c:1153-1156`) runs only after a FE write-back fence (`fe_fb_publish()`, which posts and waits) |
| **The event-callback table** (`firmware/events.c:91,127`) | cached, edited by `add_event` | registration on whichever thread calls it, dispatch on the poster's | **Registration only on the CPU**, at shell start, before the `ui` thread starts (most already happen there: `ui_main.c:1217`, `np_playback.c:226`). Any registration made later moves into `be_svc`. The table is never edited from the COP |
| **The tick-task list** (`tick_funcs`, cached; `tick_add_task`/`_remove` mask only the caller's IRQs, `tick.c:32-67`) [bcm] | cached | — | **Edits only on the CPU** (BCM must-fix 3). The shell's one direct user is via `lcd_awake`/`lcd_sleep` (`lcd-video.c:1163,1192`), which stay on the CPU |
| **`ui_log` ring** (drops COP callers, `uilog.c:143-153`) | `core_alloc` ring, CPU-only | both cores write | **A per-core ring:** a COP ring of about 2 KB in the shared block, single writer (COP) and single reader (the CPU's log service), Lamport single-producer single-consumer indices in uncached memory. The service drains it into the same file with a `c=1` marker. `dropped` stays a per-core counter |
| **`powerlog`** (`powerlog_mark` returns on the COP, `powerlog.c:328-336`; `disable_irq_save` is per core) | CPU ring | CPU only today | **FE marks go through a mark word per kind,** a per-core counter in uncached memory. The CPU's sampler folds them in. The ring stays CPU-only |
| **`devstats`** (`idle_us[COP]`, `powerlog.c:209`) | — | per-core words | Already per core. Check each field's writer in 19-9a |
| **The panic record** (`HAVE_PANIC_RECORD`) | uncached alias | either | Unchanged (it is already written through the uncached alias). COP panics: §3.6 |
| **The dbqueue, maint and modload kernel objects** (`dbq_mtx`, `persist_mtx`, `dbqueue.c:86`; `take_mtx`, `maint.c:66`; modload's `done` semaphore, `modload.c:408`) | cached `.bss` | — | 19-8 moves their users to the BE. Any that FE still touches moves to SHAREDBSS (§3.2) |
| **Module (Extras) code and data** (`modload.c`) | arena, `core_alloc` | the CPU loads, the FE runs Class A modules | `COPJ_CODE`-style: the CPU writes back after loading, then the COP writes back and drops both caches before the first call (placement §3.1). Class A modules run on the `ui` thread; their file and allocation calls are already on P12's deny list |
| **Stacks** | own core | own core | cached, but the CPU-side stack scan (Developer › OS stacks) reads the `ui` stack's high-water mark stale. Under `HAVE_COP_FRONTEND` it reads through the uncached alias |

### 2.3 The boundary mechanics

**The now record** (`fe_now`). It lives in SHAREDBSS, about 128 B, written only by `be_svc`, through the seqlock writer `fe_now_begin()`/`fe_now_end()` (uncached stores of the odd and even `seq`). The FE reads `seq` (retrying while odd), copies the fields, then reads `seq` again (retrying on a change).
- *Identity is separate from validity* (`process.md` item 3): `track_id` and `pl_index` are identity; `seq` is only validity.
- Contents:
  - status bits;
  - elapsed and length in ms;
  - `track_id`, the generation of the `fe_track` snapshot, `pl_index` and `pl_amount`;
  - volume, the buffer state, the battery %, the charger, `queue_stopped`;
  - the sleep, alarm and "resume visible" flags;
  - the generation words `lib`, `art[n]` and `settings`.
- **Elapsed** changes every frame of playback, so the CPU writes it **only when the FE asks**: `fe_now.want_elapsed` is a front-end word, and the codec's elapsed update copies it when the flag is set. That adds no CPU wake in the dark (nobody asks) [design].

**The wake.** The BE posts `SYS_FE_STATE` to `button_queue` after a change the FE must act on. Posts coalesce: `be_svc` keeps a pending bit and posts only when it is clear, and the FE clears it on receipt.

**Commands, async** (`be_post(kind, a, b)`). For play, pause, resume, skip, seek, volume, `BE_STATUS_SET` and the moved idle services' kicks: a post to `be_svc`'s queue (SHAREDBSS). No pointers: small values only, or an uncached argument record.

**Commands, sync** (`be_call(kind, struct be_args *a)`).

What the FE does:
1. copies the arguments (paths up to `MAX_PATH`, small structs) into **its own slot's uncached argument record** (a ring of 4 slots of 384 B in the shared block; one slot per caller, normally the `ui` thread only);
2. **writes back its cache** (`commit_dcache()`), so no dirty FE line overlaps a buffer the BE or DMA will write, and the BE sees any bulk input buffer;
3. posts with `queue_send` semantics (or the slot's semaphore) and blocks;
4. on the reply, **writes back and drops** its cache (`commit_discard_dcache()`) if the reply says the BE wrote FE memory (`BE_WROTE`: bulk output such as a read, a playlist range or a tag list), else nothing;
5. copies the results out of the uncached record.

What the BE does: reads the arguments from the uncached record (never from FE pointers) and writes bulk output **through the uncached alias** of the FE's buffer (or by ATA DMA, which writes physical memory), so the CPU never needs a whole-cache drop of its own to serve the FE. That matters because a CPU drop throws away the codec's hot lines.

This is the `cop_job` call table (`data-paths.md` R7.2) turned around, with the CPU as the worker and the COP as the poster. **Reuse `cop_job_core.c`'s `copj_cache_ops()` table and its host test**; do not write a second one.

**Bulk tables** cross by generation (§2.2). One FE drop per generation, never one per element.

**Rule for every shell file:** a function is either FE or BE, by file. `fe_be.h` lists the BE entry points the FE may call. The link check (brief §3.5 item 10) fails `validate.sh` when an FE object references a BE symbol outside that list. The list is generated from the objects' undefined-symbol tables (`arm-none-eabi-nm -u`) against an allowlist.

### 2.4 `core_alloc` compaction: the key hazard

**The hazard.** buflib compacts on the CPU, inside `core_alloc` or a shrink, and calls move callbacks there [code]. If the COP holds a raw pointer into a movable block, three things break:
1. the CPU's `memmove` reads SDRAM and **misses the COP's dirty lines**, so the moved copy is stale;
2. the COP keeps writing at the **old** address, into what is now another tenant's memory;
3. `core_get_data()` from the COP reads a **stale handle table** (cached, written by the CPU).

That is silent corruption of the audio buffer's neighbours. buflib has no lock [bcm, must-fix 5], so even a pin from the COP races the CPU.

**The rule, enforced in code:**
1. **The COP never calls `core_alloc`, `core_free`, `core_pin`, `core_unpin`, `core_get_data` or `core_shrink`.** The link check enforces it: those symbols are not in `fe_be.h`. In debug builds they also `panicf` off the CPU.
2. **Every block the FE touches is pinned for its life, by the CPU, before the FE first sees it.** The CPU passes the pointer to the FE in the shared block, resolved at pin time. That covers:
   - the strip, the row-art LRU, `pv.slot`/`slot2`/`scratch`, `npf_buf`, the module arena;
   - **the four fonts** (loaded movable today: `font_load_ex(path, 0, 0)`, `theme.c:129`, `qscroll.c:159`; under `HAVE_COP_FRONTEND` they load and then pin, which costs nothing because they load once, before playback, `hardware.md` budget row "Fonts");
   - the `ui` stack;
   - `chap_bytes` and the queue-by-index tables the FE reads.
3. **A block the FE drops goes back by `be_post(BE_UNPIN_FREE, h)`** after the FE has written back. The CPU frees it.
4. **Transient FE buffers** (an On-The-Go read buffer, a search result) are carved from the strip or a fixed pinned FE arena, never freshly allocated.
5. **Movable allocations stay entirely BE:** the audio buffer, the tagcache RAM cache (handed to the FE only by generation, pinned for the generation's life: the commit takes the new one's pin and drops the old one after the FE acknowledges, `fe_gen_ack`), and the database candidate.
6. **The cost in fragmentation:** about 0. These blocks are already pinned for the shell's life today, except the fonts and the view pool [code: `anim.c:158`, `preview.c:537,746`, `artcache.c:7763`]. The fonts are taken at shell start before playback, so pinning them blocks no compaction that mattered.

---

## 3. Kernel requirements

### 3.1 What exists against what is new

| Item | Status | Where |
|---|---|---|
| `NUM_CORES 2`, `HAVE_CORELOCK_OBJECT`, `IF_COP`, `CURRENT_CORE` | **exists** | `config.h:1286-1300`; `system-target.h:61` |
| Peterson corelock | **exists** | `corelock.h:41-46`, `firmware/asm/arm/corelock.c:30-100` |
| Cross-core queue post, semaphore release and immediate wake | **exists** | `queue.c:410-431` → `thread.c:420-428` → `core_wake()` (`thread-pp.c:318-345`) |
| `queue_send` across cores (sender blocks, `queue_reply` wakes it) | **exists** (same wake path) [code; hypothesis on the COP until the 19-8 host and sim tests] | `queue.c` |
| COP timeouts | **exists, tick-pulsed:** the COP checks its own `tmo` list in `switch_thread` (`thread.c:1050`) when the per-tick pulse wakes it (`kernel-pp.c:36`) | |
| Thread creation on the COP, writing back the creator's cache | **exists** (`thread.c:1235-1238`). The new core's stale lines over the stack are not dropped; harmless, because a stack is written before it is read, except the high-water scan (§2.2) | |
| `cpu_boost` from either core | **exists:** a global counter, SHAREDBSS (`system.c:35-36`), under `cpufreq_cl` (`system-pp502x.c:55`). Never from an ISR (the lock is not IRQ-safe) | |
| **K-1: park the COP** (19-1d) | **new**, owner A | §3.3 |
| **K-2: kernel objects in uncached memory** | **new audit** | §3.2 |
| **K-3: per-core boost accounting** (diagnostic) | **new:** two single-writer words of boosted µs per core; the `m0` lines report them | M-4 needs it |
| **K-4: the COP panic path** | **new** | §3.6 |
| **K-5: tick-list and event-table edits CPU-only** | **new assert** | §2.2 |

### 3.2 K-2: the uncached kernel-object audit

Every `struct mutex`, `semaphore`, `event_queue`, `corelock` or `wakeup` that a thread on the other core can touch must be in `.ncbss`, `.ncdata` or IRAM. **Found so far:**

| Object | Today | Touched by the COP after 19-9 because | Fix |
|---|---|---|---|
| `lcd_port_mtx` (`lcd-video.c:563`) | cached static | the FE's pushes (`lcd_update_rect` from the COP) against the backlight thread's `lcd_awake`/`lcd_sleep` and vplayer | **SHAREDBSS** (BCM must-fix 1; `ata_mutex` is the model, `ata-common.c:31` [bcm]) |
| `bcm_port_depth`/`bcm_port_mtx` (`bcmjob-dev.c:44-45` [bcm]) | cached | the job ring | SHAREDBSS, or folded into `lcd_state` (IRAM) |
| e2-port's owner and claim (`e2-port.c:64-84` [bcm]) | cached; `disable_irq_save` only | the LCD (COP) and video (CPU) share engine 2 channel 0 | SHAREDBSS, plus a corelock on the claim |
| `bcm_devtest_owned`, `bcmfw_active` (`bcm-devtest-hal.c:517-545` [bcm]); `bcmfw_hung`, `bcmfw_kick_tick` (`lcd-video.c:159-160`) | cached | read by FE pushes, written by the CPU tick and devtest | SHAREDBSS or `lcd_state` |
| `bcmjob_gen` [bcm] | cached | ring epoch | SHAREDBSS |
| `piezo_queue` (`piezo.c:30`) | **cached** | FE clicks (placement rule 4's exception) | SHAREDBSS |
| `dbq_mtx`, `persist_mtx` (`dbqueue.c:86`), `take_mtx` (`maint.c:66`), modload's `done` (`modload.c:408`) | cached | only if FE code still takes them after 19-8 | **19-8 moves their users to the BE.** If any FE user remains, SHAREDBSS |
| `art_queue` (`artcache.c:336`), `button_queue`, `audio_queue`, `backlight_queue`, `storage_queue`, `usb_queue`, `scroll_queue`, `tagcache_queue`, `id3_mutex` | SHAREDBSS | — | already right [code] |

**The gate:** a `validate.sh` step (owner T) that lists every kernel object symbol in `build/fw-ui/rockbox.map` outside `.ncbss`, `.ncdata` and `.ibss`, against an allowlist of objects proven single-core. A new cached one fails the gate.

### 3.3 K-1 (19-1d) as this design needs it

- **Today** the CPU's tick wakes the COP 100 times a second (`kernel-pp.c:34-37`).
- **K-1:** the COP publishes its earliest timeout as an uncached word, `cop_next_tmo`. The COP writes it in `switch_thread` before it sleeps (single writer), and `0` means none.
- **The CPU's tick:** `if (cop_next_tmo && TIME_AFTER_EQ(current_tick, cop_next_tmo)) core_wake(COP);`.
- **Every other wake is already explicit** (queue posts, semaphores and `queue_reply` all call `core_wake` through `core_schedule_wakeup`, `thread.c:420-428`) [code].
- **The ordering:**
  - only the COP arms its own timeouts, and it is awake when it does so;
  - it writes `cop_next_tmo` before `core_sleep`;
  - **new for 19-9:** under `HAVE_COP_FRONTEND` the COP writes its cache back (`commit_dcache()`) before it parks, so a parked COP holds no dirty line and a CPU reader of FE memory (the wake frame, §6.3) needs no fence while the COP is parked. One 8 KB write-back per park, at most a few per lit minute [estimate];
  - `core_sleep`'s mailbox handshake (`thread-pp.c:289-311`) closes the race with a concurrent `core_wake`.
- **For the UI this gives:**
  - in the dark, `ui_main.c:1068-1073` already waits with `TIMEOUT_BLOCK` when nothing is due, so `cop_next_tmo = 0` and the COP is parked for the whole dark spell;
  - lit, the COP wakes on its own deadlines (glides at the frame rate, the NP clock at 1 Hz, the header swap) with no idle pulses.
- **Prerequisites** (19-1d):
  - the host test "a post to a sleeping COP thread wakes it within one tick";
  - the same for a timeout and for a `queue_reply`;
  - a device soak.
- **Hardware alternative** [hypothesis, not the default]: the COP sleeps with its own countdown (`PROC_WAIT_CNT` with `PROC_CNT_MSEC`, `cores-and-threads.md` §2) instead of relying on the CPU's tick. It is unused on silicon by Rockbox, so it stays a bench item.

### 3.4 Stacks and the shared block

- **`ui` stack:** 8 KB (the measured deepest path is 5,112 B, 62 % of 8 KB, `hardware.md` row "Thread stacks with a splash").
  - It is a pinned `core_alloc` taken at shell start, before playback, in DRAM, cached. Only the COP touches it.
  - IRAM is full (45,808/49,152 B core IRAM by `hardware.md`; the primary tree's map shows ~8.7 KB free, a disagreement to settle in 19-8a with a fresh map), and rule 3 keeps IRAM for codecs.
  - Not a static (the lean rule against large statics).
- **`be_svc`** keeps the main thread's 8 KB IRAM `.stack`, so IRAM does not change. Once the shell's deep paths split between `ui` and `be_svc`, re-measure both with `scripts/risk/lib/stack_depth.py`. If `be_svc`'s worst path is under 4 KB, propose shrinking `.stack` to give IRAM back to codecs (a separate change).
- **The shared block:** about 3 KB, one pinned `core_alloc` at shell start, used **only through its uncached alias** (it is over 1 KB, so not a static). It holds:
  - `fe_track[2]`;
  - the `be_call` argument ring (4 × 384 B);
  - the COP log ring (2 KB, shrinkable to 1 KB).

  The now record and the generation words are small (under 256 B), so they sit in `.ncbss` as statics.

### 3.5 Priority and yield on the COP

- After 19-9 the COP runs only `ui` (16) and idle; the COP worker and the synthesis thread are gone (§1.1).
- Long FE loops (list builds, text layout) can no longer starve decode, which is the main smoothness win.
- They still `yield()` at their current points, so that a `be_call` reply or a `SYS_FE_STATE` on the COP is not delayed. `yield()` with only one runnable thread returns at once [code: `switch_thread` with an empty run list].
- **Fit-1's bands** (codec 13, video feed 14, buffering 15, UI 16) and aging stay as they are on the CPU. `be_svc` takes the UI's old band (16).
- The BCM audit asks whether yield-polling on the COP keeps it out of `PROC_SLEEP`. It does: a polling loop that yields is always runnable. **Rule:** no FE wait polls; every FE wait blocks on a queue, a semaphore or a timeout. This covers the job layer's `FENCE` wait (§6) and the engine-2 wait when the COP starts it.

### 3.6 K-4: panics and `logf` from the COP

- **Today a COP `panicf` would draw from the COP while the CPU keeps running** [hypothesis; not exercised].
- **Under `HAVE_COP_FRONTEND`** the COP's `panicf`:
  1. writes the panic record (uncached, as today);
  2. sets an IRAM flag;
  3. halts itself (`PROC_CTL = 0x4000_0000` with IRQs off, `system_exception_wait()`).

  The CPU's tick sees the flag and runs the ordinary panic path. That path logs (rule 1: no panic screen shown to users), and the developer build shows it.
- **`logf` from the COP** goes into the COP log ring (§2.2). `logf`'s own ring is CPU-only.

---

## 4. Rockbox APIs the UI calls, and what each needs

The counts are call sites in `apps/ipodui/` (excluding games, modules and sdk), from the survey regex; treat them as ± a few.

| API family (sites) | Assumes the CPU because | After 19-8 (UI on its own CPU thread) | Safe on the COP after 19-9? |
|---|---|---|---|
| `audio_status` (114) | reads playback state | `fe_now.status` | yes, through the now record |
| `audio_current_track` (38) | returns a pointer into cached `mp3entry`s | `fe_track` snapshot | yes, through the snapshot |
| `audio_play/pause/resume/next/prev/stop/ff_rewind/pre_ff_rewind/hard_stop` (about 60) | queue posts plus some state reads | `be_post(BE_AUDIO_*)` | yes |
| `audio_buffer_size/regrow/get_filebuflen`, `audio_flush_and_reload_tracks` (about 30) | allocator and buffering | `be_call` | yes, as a `be_call` |
| `playlist_*` (about 127) | cached `current_playlist`, file I/O | now record for counts; `be_call(BE_PL_*)` for edits and ranges | yes |
| `tagcache_*`, tagtree (about 330) | RAM cache in cached memory, file reads, contexts | generation hand-off for reads of the published view; `be_call` for searches, retrieves and context acquire/release | yes |
| `settings_save` (17), `global_settings` writes (about 48) | file I/O; BE readers | FE writes, then `be_call(BE_SETTINGS_APPLY/SAVE)` | yes |
| File I/O: `open` 193, `read` 130, `write` 90, `close` 220, `rename` 27, `remove` 55, `opendir` 7, `fsync` 33, `file_exists` 38 | storage, DMA, cached buffers | **all to the BE.** The services in placement §3.2 move whole to CPU threads. The remaining FE file uses (screens that read lists, OTG) become `be_call(BE_FILE_*)`, in bulk reads | yes, as `be_call`s; **never a stream of small reads** (bulk I/O rule) |
| `storage_*` (about 111) | storage thread state | the now record for `disk_is_active` and friends; `be_call` for `set_poweroff`, `spindown`, `post_job` | yes |
| `lcd_*` drawing (about 680) | none: they draw into the framebuffer | — | **yes.** FE-local |
| `lcd_update_rect` (via `ui_lcd_push`, `anim.c:107-118`), `lcd_update` (7) | the port, the tick block, E2 | unchanged | **yes, with the K-2 fixes** (port mutex in SHAREDBSS). The COP flushes its own cache before E2 (`e2-port.c:118` runs on the caller). Needs **B10** (the COP touching the port) and the engine-2-from-the-COP check (§6). Fallback: a CPU push service under the hand-off contract |
| `lcd_sleep`/`lcd_awake` (7, `sleep.c`, `sleeplab.c`) | tick-list edits, `bcm_init`, file reads | `be_call(BE_LCD_SLEEP/AWAKE)` | yes, as `be_call`s (BCM must-fix 3) |
| `backlight_on/off/set_timeout/set_brightness/sleep_hold` (about 22) | posts to SHAREDBSS `backlight_queue`; `backlight_hw_off` touches hardware | posts as today; `backlight_hw_off` (`bootbatt.c:346`) moves with bootbatt to the BE | yes (posts); the hardware call is BE |
| `button_get_w_tmo`, `button_get`, `button_queue_post`, `button_queue_count`, `button_status`, `button_hold`, `button_get_data` (about 44) | SHAREDBSS queue; `button_status` reads a driver word | unchanged | **yes.** `button_status()` reads a single word the CPU's tick writes (one writer). `button_set_sw_poweroff_state` (`sleep.c:218`) moves to `be_call` |
| `pcm_is_playing` (3) | reads a pcm word | now record | yes |
| `cpu_boost` (33; `anim.c:70,81` plus library, OTG, sleep, USB screen, xindex, search) | — | unchanged | **yes** (global counter under a corelock). Balance is per call site as today. Most of the library and xindex boosts move to the BE with their work |
| `sys_poweroff` (5), `sys_reboot`, `rolo_load`, `power_off`, `system_reboot` (sdk) | shutdown work, IRAM clear for Apple's firmware (quirk 6) | `be_call(BE_SHUTDOWN/REBOOT/ROLO)`. The FE then blocks for ever | yes, as `be_call`s. `rolo.c` already parks the COP (`rolo.c:112`) |
| `power_sleep()` (P18 soft sleep) | **refuses off the CPU** (`power_sleep-pp502x.c:180`); checks `cop_parked()` (`:107-120,198`) | `be_call(BE_SOFT_SLEEP)`. The FE blocks on the reply, so the COP has no runnable thread and parks, which is what `cop_parked()` checks | yes |
| `core_alloc` family (about 243) | buflib has no lock | `be_call(BE_ALLOC/FREE/PIN)` for the few FE uses left; most move with their services | **never from the COP** (§2.4) |
| `ui_log` (about 274) | ring CPU-only | unchanged | via the COP ring (§2.2) |
| `powerlog_*` (about 46) | ring CPU-only | unchanged | marks via per-core words |
| `queue_post` to `art_queue` (5) | SHAREDBSS | unchanged | yes. Pointers in event data: no; values or generations only |
| `storage_post_job` (`xindex.c:225`, `modload.c:506`) | posts a job struct by pointer | job structs in uncached memory, or `be_call` | yes, once the job struct is uncached |
| `thread_self` ownership (`ab_store.c:204,842`, `batcal.c:327,342`, `modclass.c:139,236`) | single-thread ownership | the owners move to the BE with their services; `modclass` follows the `ui` thread | yes (ids are global) |
| `timeout_register` (3) | tick ISR on the CPU | moves to the BE with bootbatt, batcal and powerlog | not from the FE |
| `add_event` | the table is cached | registered on the CPU before `ui` starts | not from the FE |
| `default_event_handler` | USB screen (FE), shutdown (BE) | split: the USB part on the FE, shutdown as a `be_call` | yes |
| `codec_cop_set_place` (`powerlog.c:831`) | — | unchanged | moot: the COP worker is off |

**The 19-8 back-end request kinds** (`fe_be.h`):
- `BE_AUDIO_*`, `BE_PL_*`, `BE_TC_*` (search, retrieve, context, view build);
- `BE_SETTINGS_APPLY/SAVE`, `BE_STATUS_SET`;
- `BE_FILE_*` (bulk read, write, list, stat, rename, remove);
- `BE_ALLOC/PIN/UNPIN_FREE`;
- `BE_LCD_SLEEP/AWAKE`, `BE_SOFT_SLEEP`, `BE_SHUTDOWN/REBOOT/ROLO`;
- `BE_SVC_KICK` (the moved idle services);
- `BE_PUSH` (the CPU push service, §6.3 fallback).

**Each kind's handler is a BE function in a BE file.** The FE sees only the kind and its argument struct.

---

## 5. Power and boost

- **One clock for both cores.** `CLOCK_SOURCE` and the PLL are shared [rockbox-derived] (`clocks-power.md`; `cores-and-threads.md` §7). `set_cpu_frequency()` works from either core: `scale_suspend_core()` holds the *other* core with `PROC_CTL(other) = 0x4000_0000` only around the `CLOCK_SOURCE` write, with the caller's IRQ and FIQ off (`system-pp502x.c:336-355`) [code].
  - So a COP-side boost stalls the CPU for a few cycles per switch, including a pending audio FIQ, exactly as a CPU boost stalls the COP today. The PLL's relock wait runs at 24 MHz with the other core free.
  - `oldstatus` is a single IBSS word (`:341`), safe because `cpufreq_cl` serialises callers.
- **There are no per-core boost counts.** The counter is global, so a COP boost raises the CPU's clock too. That is what we want: the UI's glides boosted, decode unaffected. K-3 adds per-core *accounting* only.
- **Dark playback** (unchanged from 19-1's end state):
  - the UI is blocked in `button_get_w_tmo(TIMEOUT_BLOCK)`;
  - with K-1 there is no COP deadline, so the COP is parked in `PROC_SLEEP`, with no pulses;
  - the CPU decodes alone (FLAC, with 19-1c D);
  - the BCM2722 is cut (C0).
  - **19-9 adds nothing to the dark wake budget** (`m0 cop wakes n=0`, brief 19-1 acceptance). The `fe_now` updates in the dark are uncached stores by the CPU, with no wake. `SYS_FE_STATE` posts are suppressed while the backlight is off (track changes are read at the next lit entry, from the now record).
  - **The one risk:** an FE path that arms a timeout in the dark (today the unlit face's minute, `m0 wakes main n=1` per minute [silicon]). It wakes the COP once a minute. The face is not drawn dark (placement §3.4), so its timer must be disarmed; that is a 19-9 check.
- **Lit:**
  - **What changes:** UI frames (text, glides on the CPU fallback, NP) run on the COP while decode runs on the CPU.
  - **Expected effects** [estimate]:
    - fewer boosted milliseconds per lit minute (the UI no longer waits behind decode; FLAC is ~55 % of the CPU at 24 MHz [silicon, session 36]);
    - fewer missed frames;
    - more cycles in all, for the boundary: about 2 to 4 k cycles per `be_call` with write-backs, plus one COP drop per generation, measured by M-5.
  - **Current:** two cores active part of the time, against one core active longer at the same clock. The ARM7TDMI's per-core active current is unknown (M-1). **Net 0 to 2 mA either way** [estimate, ±]. The clear win is smoothness, and a lit 24 MHz (19-3) becomes easier to hold.
- **Wake effects:**
  - each input event costs a CPU wake (the IRQ, as today) plus a COP wake;
  - each `be_call` costs a COP-to-CPU-to-COP round trip;
  - lit wake budgets (placement §5.1) gain a COP column;
  - target: COP wakes per lit second ≤ the UI's current main-thread wakes (~6 /s at rest, 30 /s in glides) + 10 %.

---

## 6. The BCM2722 ring and port

### 6.1 The BCM session's audit, folded in

All five must-fixes are taken as given. Each has a place in the step list (§7).

| BCM item | Design answer | Step |
|---|---|---|
| **MF-1** Coherent shared state: `lcd_port_mtx`, `bcm_port_depth`/`bcm_port_mtx`, e2-port's owner and claim, `bcm_devtest_owned`/`bcmfw_active`, `bcmfw_hung`/`bcmfw_kick_tick`, `bcmjob_gen` | SHAREDBSS, or move into `lcd_state` (IRAM). The engine-2 claim gets a corelock (§3.2) | **19-9 step 0** (PP coherence package) |
| **MF-2** One writer for the driver mirror `b` (`bcmjob-core.c:42-67`): the CPU writes it through `bcmjob_suspend` (vplayer, devtest `dt_take`/`dt_restore`) and `bjc_dark` (backlight thread) | **The COP is the mirror's only writer.** CPU-side wants become request bits in a SHAREDBSS word (`bcm_req`: suspend, resume, dark, lit; per-bit single writer per requester) that the COP applies at `enter()`. A CPU requester that must know the request took effect waits on a SHAREDBSS semaphore the COP signals | step 0 |
| **MF-3** Tick-list edits, `lcd_awake`/`lcd_sleep`/`bcm_init` and the boot ladder (`jd_power`/`jd_restart`/`bcmfw_fallback`, then `bcm_init` plus file reads in `bcmfw-boot.c:113…688`) stay on the CPU | **The power transitions are CPU services.** The COP's job layer asks with `be_call(BE_LCD_AWAKE/SLEEP/BCM_RESTART)` (a `cop_job_call`-style crossing). The backlight thread's dark/lit decisions and `jd_power(false)` then meet in one place (the CPU service, under the port lock), which closes the dark-against-lit race | step 0, and 19-9b |
| **MF-4** The pixel hand-off contract | §6.3 | step 0 (contract plus host test), 19-9b |
| **MF-5** Allocations and pins on the CPU only; callers pin upload pixels for the whole upload (the engine-2 wait yields, so a move mid-upload is possible **today**) | §2.4. `bcmjob_cpu_init`/`bcmjob_init` (`bcmjob-cpu.c:60`, `bcmjob-dev.c:186`) run on `be_svc` at shell start. **The mid-upload move is a live bug on one core today:** fix it in step 0 regardless of 19-9 | step 0 |
| `ui_log` drops COP callers | the COP ring (§2.2) | 19-9a |
| `power_sleep()` refuses off the CPU | `be_call(BE_SOFT_SLEEP)` (§4) | 19-8 |
| `cpu_boost`, `USEC_TIMER`, `current_tick`, IRAM | fine [bcm; code] | — |
| Default: the COP codec worker leaves the default build | taken (§1.1) | 19-9a |
| Default: vplayer holds the port lock per piece | taken | step 0 |
| Default: items 1, 2 and 5 become the PP-side "19-9 step 0" before any UI move | taken; step 0 is mergeable on its own, on one core (§7) | step 0 |

**Their silicon questions** (each goes on a bench card, run only with the owner's go): see §6.4 and Appendix A.

### 6.2 Who owns what, after 19-9

| Function | Owner | Path |
|---|---|---|
| **The job ring** (`bcmjob`): posting records, `FENCE` waits, `J_EPOCH`, `ANIM_STOP` | **COP** (brief §3.5 item 9) | FE-local; the waits block (§3.5) |
| **The heartbeat** (`HP_STATE`; the fault word `0x1F8`) | **whoever is waiting.** The COP checks it inside its own job waits; the CPU checks it inside vplayer's and `lcd_tick`'s waits. No standing poller in any state (contract rule: in C0 the job API answers "chip off" at once and never polls) | |
| **`lcd_tick`** (the CPU tick task, `lcd-video.c:372-434`) | CPU; **only for the CPU-path updates** (UPDATERECT kicks) | today the COP's `lcd_update_rect` still goes through `lcd_state` + `lcd_tick`. That is safe: `lcd_state` is IRAM with its own corelock (`lcd-video.c:110-127`), and `lcd_block_tick()` excludes the tick during a push. **19-9b keeps that; a later step** (after jobs land) moves completion to the COP's `FENCE` wait |
| **The doorbell** (BCM source 23, `IDLE_DOORBELL`) | the COP rings it after posting a record (a port write, under the port lock) | unconfirmed on silicon (BCM open questions #3, #28) |
| **Dark-want calls** (`bjc_dark` from the backlight thread) | a CPU request bit (MF-2) | applied by the COP at `enter()`. If the COP is parked (dark), the request bit plus `core_wake(COP)` lets the COP apply it and park again. **Better:** in C0 the mirror needs no update until the next lit entry, so the CPU leaves the bit and does not wake the COP (the default) |
| **Power transitions** (C0 cut, cold wake, the boot ladder, the image upload) | **CPU** (MF-3) | `be_call` from the COP; the backlight thread directly |
| **The video feed** (vplayer) | CPU | per-piece port lock; hands records to the COP's ring only if a video job needs the job ring (brief §3.5 item 9) |
| **Engine 2** (channel 0, shared by the LCD and video) | the core that starts a piece polls it (no IRQ) | needs the COP-start check (§6.4). Claim under a corelock (MF-1) |

### 6.3 The pixel hand-off contract (BCM MF-4)

1. **The producer cleans; a consumer on the other core reads uncached or drops first.** Buffers are 16-byte aligned (one cache line) at both ends, so no line is shared with an unrelated neighbour.
2. **An E2 piece:** started by the core that wrote the pixels (it already runs `commit_dcache()` on its own cache, `e2-port.c:118`). If the COP cannot start engine 2 (§6.4), the COP writes back and then `be_call(BE_PUSH, rect)`. The CPU starts the piece from the uncached alias. **That is the fallback**, behind a developer switch and a marker file.
3. **A CPU copy of COP pixels** (the CPU fallback push, `lcd_awake`'s wake frame at `lcd-video.c:1156`, vplayer's overlay at `vplayer.c:1038`) reads `UNCACHED_ADDR(src)` after a COP write-back fence (`fe_fb_publish()`). `lcd_awake` asks for the fence with `be_call`'s mirror: the CPU posts `SYS_FE_FB_FENCE` and waits on a semaphore the FE signals after `commit_dcache()`. If the COP is parked (dark wake), its cache holds nothing dirty (it wrote back before it parked, part of K-1's park step), so the wait is skipped when `cop_parked()`.
4. **The CPU-side push paths** take an `uncached` flag under `HAVE_COP_FRONTEND`: `lcd_write_data` (the 2-register loop) reads through the alias. Cost: uncached SDRAM reads for the push. A push is port-bound (about 10.5 PP cycles per 16-bit transfer, quirk 17), so the uncached read is hidden [estimate]; measure it in M-4.
5. **Host test:** the cache model (§7.2) checks every crossing.

**As built (MF-4, `p19-mf4`):** see "MF-4 done" at the top. Items 1–3 as written, with the overlay published by the COP before its post instead of fenced (the COP sleeps on the CPU's answer); item 4 is `lcd_fb_read()` keyed on `lcd_fb_cross()`, not a flag; item 5 is `tests/lcdxcore`.

### 6.4 Questions for the BCM session

1. **Engine 2 from the COP:** Apple's transfer object refuses to run on the COP (`data-paths.md` §7.1 risks). Do they have the reason from their RE (a register only the CPU's bus view decodes, or just Apple's policy)? Their bench card should pair with our **B10**: one COP job that starts and polls a 64 KB piece into SRAM and reads it back.
2. **The shared cache data array:** can their bench answer it? Write a known word on the CPU without a write-back, then read it on the COP through the cached address. If the COP sees it, the array is shared. We will run it with M-5 if they cannot.
3. **The mirror request bits (MF-2):** is "applied at the COP's next `enter()`" soon enough for `bcmjob_suspend` before vplayer's first port write, or must vplayer wait for an acknowledgement? Default: it waits on the semaphore; that costs at most one COP wake.
4. **`lcd_tick` after jobs land:** do they want the tick task gone in the job build (completion only by `FENCE`)? Default: keep it for the CPU path only, as the brief says.
5. **The doorbell:** if 5i shows source 23 wakes the BCM's core, may the COP ring it from inside the port lock while the CPU's tick is blocked, or must the ring and doorbell be one port piece?
6. **`J_T_SMALL` waits on the COP:** may `ANIM_STOP`'s wait (in `ui_fb_settle()`, brief §3.6) block on a timeout instead of polling? That needs no BCM change if the PP re-reads at each COP wake, but they own the timing contract.
7. **`jmodel` in the shell simulator:** can the model run the job layer on a thread other than the shell's main thread? The sim's 19-8 split puts the shell's loop on a new `ui` thread (§7.2).
8. **The resident image's upload** (L2) on a COP-initiated wake: the upload is a CPU service under MF-3. Is a 210 KB upload with the COP blocked acceptable, or do they want the frame upload overlapped (Q-3) with the COP composing the first frame meanwhile?
9. **Their audit table's file:line refs** are against `fc9222f324`. Please re-anchor them when `p19-bench-p12b` merges into the P19 line, so step 0 has exact sites.

---

## 7. Staging

**Two flags.** Each one's off state restores the previous step.
- **`HAVE_UI_THREAD_SPLIT`** (19-8): the UI runs on its own CPU thread, and main becomes `be_svc`.
- **`HAVE_COP_FRONTEND`** (19-9, implies the split): the `ui` thread is created on the COP.

Both are config flags in `ipodvideo-variant.h` (documented in `hardware.md`'s flags list by its owner). A **boot marker file** (`.prokbox/cpu_frontend`) forces the `ui` thread onto the CPU at boot, for a device fallback with no reinstall.

### 7.1 Steps

| # | Step | Content | Mergeable alone? | Tests: host / sim / device |
|---|---|---|---|---|
| **S0** | **19-9 step 0: PP coherence package** (BCM MF-1, MF-2, MF-5; K-2) | Kernel objects to SHAREDBSS (§3.2): `lcd_port_mtx`, the bcmjob lock and depth, e2-port's owner plus a corelock claim, the BCM flags, `bcmjob_gen`, `piezo_queue`. The mirror-request bits. bcmjob allocations on the CPU at start. **The upload-pin fix** (a live one-core bug). vplayer's per-piece lock. The K-2 map gate | yes: no behaviour change on one core | host: the mirror request state machine (`bcmjob-core.c`'s host test). Sim: unchanged goldens. Device: a video plus browse soak (the port lock path) |
| **S1** | **19-8a: the boundary, no moves** | `apps/ipodui/fe_be.h` (the contract), `fe_be_core.c` (seqlock, request ring, generations: pure logic, host-tested), the now record, `fe_track`, `be_svc`'s loop in main, the shared block; the link check in **warn** mode | yes | host: `tests/febe/` (seqlock torn-read fuzz with two pthreads; ring order; generation ack; **the cache model**, §7.2). Sim: goldens unchanged |
| **S2** | **19-8b: the services leave the UI thread** (placement §3.2) | `lib_shuffle_idle`, `ab_store_idle`, `lib_book_resumed_idle` to `be_svc`. `ui_log_service`, `powerlog_service`, `batcal_service`, `bootbatt_service` to the storage idle callback. `clockpol_service` to the power thread. NP and row-art reads to the art thread. `lib_open` view builds as a BE job with a generation | yes | host: the existing `tests/{batcal,bootbatt,powerlog,clockpol,artcache}`. Sim: all `ipodui-*` scripts, plus a new `ipodui-p19-febe-services.script` (play, skip, a book resume, shuffle refill, an update while browsing). Device: the M-1 lit row, a resume check |
| **S3** | **19-8c: the thread split** (`HAVE_UI_THREAD_SPLIT`) | `ui` thread on the CPU with its pinned DRAM stack; event callbacks write the now record only; the FE reads `fe_now`/`fe_track`; command wrappers (`be_post`/`be_call`) for every family in §4; default-event split; fonts pinned | yes (flag on by default after a device soak; off restores S2) | sim: the full gate, plus `ipodui-p19-split-*.script` (USB in and out, soft sleep and wake, power-off, alarm ring, Extras open, settings change, OTG save). **The sim runs the split for real** (two Rockbox threads), so thread-identity and boundary bugs show up. Device: a one-day lit and dark soak; stack high-water marks |
| **S4** | **19-8d: enforcement** | the link check **strict**; debug `panicf` on BE symbols off the CPU; a `validate.sh` grep for raw `core_get_data` in FE files | yes | gate |
| **S5** | **19-1d: K-1, the COP park** (owner A, already planned) | §3.3 | yes (19-1's own review) | host: the wake-within-a-tick tests (post, timeout, reply). Device: soak with the COP decode on (before 19-9 removes it) |
| **S6** | **Bench: M-5, B10, the engine-2-from-COP card, the shared-array card** | timed loops in the sleep lab; one COP job reading and writing the port | — | device only, with the owner's go. **The results decide the §6.3 default** |
| **S7** | **19-9a: COP readiness, still on the CPU** | the COP log ring; powerlog marks; K-3 boost accounting; K-4 COP panic; the COP worker off and MPA synthesis inline under `HAVE_COP_FRONTEND`; the uncached CPU-side fb reads and `fe_fb_publish()`; the unlit face's dark timer disarmed | yes | host: ring tests. Device: `m0 cop wakes` 0 in the dark |
| **S8** | **19-9b: the flip** (`HAVE_COP_FRONTEND`) | `create_thread(ui, …, IF_COP(, COP))`; `be_call`'s sync-point cache ops become live (on one core they compile to nothing, as in `cop_job.c`'s `cross` flag); FE pushes via `lcd_update_rect` from the COP, or `BE_PUSH` if S6 says so; the boot marker fallback | yes, **off by default** until S9 | sim: unchanged from S3 (one core). Device: M-4 (row render at 24 MHz on the COP, frame counters), coherency canaries (below), lit A/B |
| **S9** | **19-9c: a device week** | Q6's rule: a week with no corruption, then on by default (or Q-U1's gate) | — | device: the owner's daily use plus nightly canary logs |
| **S10** | **19-9d (later, with 19-2 jobs): the COP owns the ring** | the job ring and `FENCE` waits FE-local; the CPU's video records handed over; `lcd_tick` for the CPU path only | with 19-2 | BCM's tests plus `ipodui-p19-truth-*` |

**S10 also: the DECODE hand-off goes cross-core** (BCM session, 2026-10-24; branch `p19-decode-driver`). **Done 2026-10-25: see "DONE, as built" at the end of this list; the plan below is kept as written.** Today the art thread (CPU) and the ring's owner share `struct bjc_req` (uncached: the caller writes only `a_*`, the owner only `o_*`), but the call (`struct bjc_call`), its result and the read-back buffer live in the caller's cached memory. When the owner is the COP:
- the caller **publishes** the call before kicking the owner (`xcore_publish()`: clean the call's lines), and the owner **acquires** it before reading (`xcore_acquire()`: invalidate);
- the owner publishes the result and the read-back rows before it sets `o_seq`, and the caller acquires them after it sees `o_seq`;
- the **withdrawal** (an owner that never took the call within `BJC_ACK_MS`) becomes a **compare-and-swap** on the take word, so a COP that takes the call at the same instant can't run a call the caller has already abandoned;
- host tests: the two-thread cases with the cache model's per-core views, a take racing a withdrawal.
- **From the driver review (2026-10-24), what goes wrong today if the owner is on the COP:** the caller's check-then-write withdrawal races the owner's accept, so the owner can write `ret` into a returned (dead) stack frame and DMA from a freed piece buffer; `a_seq` can roll back (ABA, fixed now: it never decreases); and `bjc_call`, its queue, the result, the piece buffer and the read-back pixels sit in cached memory. The fixes for S10: a **withdraw word the owner acknowledges** (the caller waits for that acknowledgement, bounded, before it reuses its stack or frees the buffer), the **monotonic seq** (done), and the call struct **uncached** (`XCORE_BSS`) or explicitly flushed at each hand-off, with the buffers published/acquired as above.
- **Also single-core today (the decode driver, `p19-decode-driver-2`):** `o_busy` (the owner is inside a pass of this call, so the art thread must not leave) and `a_quit` (the art thread left; the owner ends the call without touching the caller's memory) are plain flag words read and written in one core's order; across cores they need the same publish/acquire pairing and a defined order (the owner sets `o_busy` before it touches the call and clears it after its last write; the caller reads `o_busy` after setting `a_quit`, with a barrier between). `own_pass_fn`, the pointer the first decode sets so the owner's pass is linked in, is written by the caller and read by the owner: publish it once at init (before the owner can run a pass) rather than lazily.
- **DONE, as built (S10 hand-off, branch `p19-s10-handoff`, 2026-10-25).** PP-side only, no BCM image change. The full state table is in `bcmjob-core.c`; the rules are in `host-driver.md` § "The decode hand-off across cores".
  - **The call** (`struct bjc_call`) now lives in `struct bjc_req`, so it is `XCORE_BSS` (`jd_req`) on the device. Nothing points into the caller's frame:
    - the caller fills the parameters before `a_seq` and copies `res` out after `o_seq`;
    - the owner copies the call into its own `j` once at the take, and writes `ret`/`res` before `o_seq`.
  - **The piece buffer** is 16-byte aligned at both ends (`jd_dec_buf`).
    - The caller pads the file's last word (that write used to be the owner's) and `own->publish`es the piece before `a_got`/`a_have`.
    - Engine 2 reads memory; the CPU's copy reads through `own->uncached`. The owner never writes the piece buffer.
  - **The read-back `px`:** the caller publishes the span before the post. The owner writes the rows through `own->uncached`, except the span's two edge lines when they hold other memory: those pixels go to `r->c.edge`, and the caller copies them in after `own->acquire`. **No alignment is needed from the art builder's call sites**, so `art_bcm.c` and `artcache.c` are unchanged.
  - **Withdraw:** acknowledged by Dekker on two single-writer words (the owner: `o_busy = s`, fence, check `a_quit`; the caller: `a_quit = s`, fence, wait for `o_busy != s`). This replaces the planned compare-and-swap.
    - The take re-checks `a_seq` too.
    - The owner checks `a_quit` at each read row and at each pass start (one piece per pass).
    - An answer that lands during the wait stands (DONE).
    - With no acknowledgement within `BJC_ACK_MS` the call is LEAKED: TIMEOUT, traced `bcmjob withdraw unacked`, counted by `bjc_leaks()`. The piece buffer stays pinned and every call answers BUSY until the owner acknowledges; then the buffer goes back (`bcmjob withdraw acked late`).
  - **`own_pass_fn`** is now `r->pass`, in the uncached record. It is still set lazily, but before the post, so linker GC still drops it.
  - **The owner's statics:** `b`, `j` and the caller's `ac` are each line-aligned, and so is the slots allocation.
    - `bcmjob_init` ends with `xcore_publish()`.
    - `bcmjob_claim_owner()` runs `xcore_acquire()` and records the owner's core.
    - `jd_owner_core` and `jd_art` are now in `xcore_objects.txt`.
  - **On one core** (`own->cross()` false, today's build) none of the hand-off runs.
    - Per piece, about 6–10 Thumb instructions are added: the `cross` tests and the pad moved from owner to caller. The fences are compiler barriers on ARMv4T.
    - Per call, the record copy is about 0.3 KB of uncached stores and loads.
    - `bcmjob-core.o` grows by 1,377 B of text and 363 B of bss (most of the bss is `req0`'s grown record).
  - **Tests (`tests/bcmjob`, `t_cross_core`):** a two-core cache model with a caller view and a memory per buffer, line-granular publish and acquire, the owner reading and writing only memory, and engine 2 reading memory.
    - **Schedule points:** baton switches at random at every fence, port call and hook.
    - **Negative controls:** dropping publish, or dropping acquire, each makes the run fail.
    - **One-core run:** no hook is called.
    - **Races:** 240 randomised runs reach every race: taken and OK, withdrawn before the take, left mid-piece and mid-row, a give-up waiting on a pass, and the answer winning the race.
    - **Checks:** canaries on the dead result frame and on `px` after return; neighbour bytes in the edge lines dirtied during the call; no dead or cached reads.
    - **The frozen owner:** a leak, then BUSY, then a late acknowledgement and recovery.
  - **Left for the flip (the COP agent):**
    - start the owner on the COP and call `bcmjob_claim_owner()` before its first job call, with no art call in flight;
    - check, on silicon, that engine 2 runs from the COP (§6.4, B10) before ring pieces go by engine 2 from the COP;
    - a LEAKED **read** cannot pin the art builder's `px`. The owner resumes inside its pass and may write rows of that buffer once more (the counter and the trace say when). The art builder should keep its read buffers alive for the art thread's lifetime (today's `dbuf` and band are), or treat a TIMEOUT read's buffer as possibly written;
    - on silicon, check that `jd_uncached` covers the art builder's buffers (cached SDRAM below `UNCACHED_BASE_ADDR`).

**Coherency canaries** (S8, developer builds):
- `fe_now` carries a CRC of its fields, which the FE checks on every read;
- every `be_call` argument record carries a sequence number and a CRC;
- the generation hand-off checks a 32-bit pattern at both ends of each published block;
- the art slots carry their key at the head and the tail;
- the strip's guard words are checked on every claim.

Each mismatch logs `fe coh fail what=… gen=…` and counts. **S9's pass criterion is 0.**

### 7.2 Testing without two cores in the simulator

The simulator is single-core: `config/sim.h:4-5` undefines `CONFIG_CPU`, so `NUM_CORES` is 1 and `IF_COP` is empty [code]. `cop_job.c` runs jobs inline in the sim (`cop_job.c:35-39`). So:

1. **The sim tests the boundary, not the caches.** With `HAVE_UI_THREAD_SPLIT` the sim runs the `ui` thread and `be_svc` as two real Rockbox threads. `thread-sdl` lets only one run at a time, but the switch points are real. That catches:
   - FE code calling BE code directly (the link check, plus a runtime `fe_assert_thread()` in sim builds);
   - thread-identity assumptions (`thread_self` owners, the input thread);
   - deadlocks in `be_call` ordering;
   - every visible regression (goldens).
2. **A host cache model tests the caches** (`tests/febe/cachemodel.c`). Two pthreads, each with a software write-back cache (direct-mapped, 512 lines of 16 B) over a shared "SDRAM" array, and an uncached window that bypasses both. `fe_be_core.c`'s operations (seqlock, ring, generation, `be_call` sync points) run against it through the same `FE_LOAD`/`FE_STORE`/`FE_WRITEBACK`/`FE_DROP` macros that compile to plain accesses and `commit_*` calls on target. **A missing write-back or drop shows up as a stale read** (asserted), deterministically, under a seeded scheduler. This is the test the COP move most needs, and the sim cannot give it.
3. **Device-only:** real timing, engine 2 from the COP, the port from the COP (B10), the cache drop's cost (M-5), whether the data array is shared, and the parked COP's current (M-1).

---

## 8. Risks, hardware features, state of the art, open questions

### 8.1 Risks, ranked

| # | Risk | Likelihood / impact | Mitigation |
|---|---|---|---|
| R1 | **Silent corruption from a missed sync point** (a stale read, or a dirty line evicted over DMA data) | medium / **severe** (the database, the playlist, the audio buffer) | One writer per structure; uncached argument records; `be_call` write-backs; the cache-model host test; the canaries; the link check; a week's soak; `HAVE_COP_FRONTEND` off by default until S9 |
| R2 | **buflib movement under the COP** | medium / severe | §2.4: nothing allocated or pinned on the COP; pinned-for-life blocks; fonts pinned; the link check forbids `core_*` in FE objects |
| R3 | **The port from the COP doesn't work, or engine 2 refuses to start from the COP** (Apple's refusal; DP-6 and PP #31 unknown) | medium / high (no lit pushes from the COP) | S6's bench card before S8. **The `BE_PUSH` fallback** (§6.3) keeps every push on the CPU with an FE write-back fence. That costs CPU push time again but keeps the split |
| R4 | **The boundary's size**: about 1,000 call sites (§4), with 19-8 regressions in resume, shuffle, OTG and settings | high / medium | Done on one core (S1-S4) behind `HAVE_UI_THREAD_SPLIT`; the per-family wrappers keep call sites readable; the full sim gate and the new scripts; P12's deny list (Class A) reused as the FE allowlist |
| R5 | **No power gain** (Q6 fails at ~2 mA) | high / low | §5: the gain is mostly smoothness; Q-U1 proposes the gate. S1-S7 are worth keeping anyway (quiet UI thread, services off it, coherence fixes, one live bug fixed in S0) |
| R6 | **The cost of a whole-cache drop at the UI's event rate** (M-5 unmeasured) | medium / medium | Uncached arguments and outputs mean the **CPU never drops for the FE**. The COP drops once per bulk reply and once per generation, and `be_call`s are batched per screen. If M-5 is bad, more of the bulk outputs are read uncached |
| R7 | **A COP panic or hang leaves the CPU playing with a dead UI** | low / medium | K-4; a CPU watchdog on `fe_alive` (an FE heartbeat word, checked lit only) that logs and reboots in the developer build, and logs and restarts the `ui` thread on the CPU (the marker fallback) in release |
| R8 | **Lit FE wakes push the COP pulses back up** | low / low | K-1's deadline wakes; the lit wake table gets a COP column |
| R9 | **Uncached CPU-side reads of the framebuffer slow the CPU fallback push** | low / low | A port-bound push hides them [estimate]; measured in M-4 |
| R10 | **Stack overflow on the 8 KB DRAM `ui` stack** | low / medium | It is the same size as today. Re-measure with the call-graph tool after S3, and check the DEADBEEF high-water mark uncached |

### 8.2 Hardware features (`docs/hardware-features.md`, brief item 6)

| Row | Feature | Use here | Decision |
|---|---|---|---|
| P-7 | COP as a compute engine | **the subject**: the COP becomes the front end, no longer a decode worker | used |
| P-5 | COP clock skipping (`COP_CTL` skip counts) | a lit COP at a fraction of the shared clock while the CPU needs 30 MHz | **later.** Measure M-4 first; skipping slows UI frames, so it fits only lit rest, which is already blocked and parked |
| P-8 | IRAM cross-bar banking | FE hot data in its own 32 KB bank | no: IRAM is full and for codecs |
| — | **Mailbox IRQ** (source 4, `CPU_QUEUE`/`COP_QUEUE`; PP open question #7; Apple's doorbell) | a hardware doorbell instead of `core_wake`'s `PROC_CTL` write | **not needed:** `core_wake` already wakes at once. Keep as a bench item only if K-1 shows a lost-wake race |
| — | **`PROC_WAIT_CNT` countdown** (ms or s unit, PP5022) | the COP sleeps to its own deadline with no CPU tick involvement | §3.3 alternative; bench only |
| X-2 | The COP doing the BCM's feeding | the COP owns the ring (S10) | used, gated by S6 |
| B-11 | BCM idle sleep plus the 1 ms tick or doorbell | lit idle power | BCM session (5i) |
| — | **Engine 2** (`0x6000_8000`, S37: 11 MB/s, CPU 77-97 % free [silicon]) | pushes without CPU time | used. Who starts it: S6 |

### 8.3 State of the art (brief item 7)

How this design relates to current practice for asymmetric two-core systems with no hardware coherency [memory; the citations are given so they can be checked]:

- **Asymmetric multiprocessing with message passing.**
  - OpenAMP/RPMsg (virtio vrings plus a doorbell interrupt, used on Cortex-A plus M SoCs) and Zephyr's IPM: one core per subsystem, shared-memory rings and an inter-processor wake.
  - Our `be_post`/`be_call` over SHAREDBSS queues plus `core_wake` is the same shape. The rings are uncached rather than cache-maintained per buffer, because maintenance here is whole-cache only.
  - We do not adopt RPMsg itself: it adds a name service and endpoints we don't need, and its virtio layout assumes per-range cache operations.
- **Seqlocks** (Linux, Hemminger and Lameter, about 2002): one writer, readers retry on an odd or changed sequence. They suit the now record exactly. On ARMv4 with uncached memory the barriers are compiler barriers only: there is no out-of-order write buffer between uncached stores on one core [hypothesis; to be checked with the stress host test plus device canaries].
- **Lamport's single-producer single-consumer ring** (1983, "Specifying concurrent program modules"): wait-free with one writer per index. Used for the COP log ring and the `be_call` slots.
- **RCU-style publication** (McKenney): publish a new version, retire the old one after every reader has acknowledged. That is our generation hand-off plus `fe_gen_ack` before a block is freed or reused.
- **Software release consistency** (Gharachorloo et al. 1990; TreadMarks' lazy release consistency, Keleher et al. 1992): coherence is enforced only at acquire and release points. Our `be_call` write-back on post and drop on reply is release consistency with whole-cache operations, made cheap by copying the arguments into uncached records, so the serving core never drops.
- **Not adopted:**
  - lock-free multi-writer structures (they need atomic read-modify-write; `SWP` across cores is unproven, PP #8);
  - hardware transactional or coherent designs (not on this silicon).

### 8.4 Open questions, each with its default

The owner is away; the defaults are taken.

| # | Question | Default |
|---|---|---|
| **Q-U1** (Q6) | Q6 keeps 19-9 only if M-1 shows ≥ ~2 mA lit gain. The owner's 2026-10-24 decision ("UI to PP2 next") wants the move; the gain is mostly smoothness (§5) | **Keep 19-9 if, in the M-1/B8 lit arms:** (a) lit current is not worse beyond the A-A spread (≤ +0.5 mA), **and** (b) boost time per lit minute falls ≥ 25 % or missed glide frames fall ≥ 50 %; **and** (c) the week's canaries read 0. Otherwise keep S0-S7 and leave `HAVE_COP_FRONTEND` off. Flagged `needs-owner` |
| Q-U2 | Who serves pushes when engine 2 cannot start from the COP (S6) | `BE_PUSH` on the CPU, with an FE fence |
| Q-U3 | `global_settings`: FE-owned with an apply, or uncached | FE-owned with `BE_SETTINGS_APPLY`; uncached only if 19-8a's audit finds BE writers |
| Q-U4 | Shrink `be_svc`'s IRAM stack after the split | measure after S3; propose separately if the worst path is under 4 KB |
| Q-U5 | Release fallback if the `ui` thread dies on the COP | log and restart the `ui` thread on the CPU (R7); developer builds reboot |
| Q-U6 | The dark `SYS_FE_STATE` posts | suppressed while the backlight is off; read at the lit entry |
| Q-U7 | The FE's `be_call` slots: one (the `ui` thread) or four (Class A modules on their own thread later) | four slots, one used now |

---

## 9. Sizes

| Item | Bytes | Where | Notes |
|---|---|---|---|
| `ui` stack | 8,192 | pinned `core_alloc`, DRAM | taken at shell start, before playback |
| `be_svc` stack | 0 new | main `.stack`, IRAM | unchanged |
| Now record plus generation words | ~192 | `.ncbss` | static, under 1 KB |
| `fe_track[2]` | ~1,280 | shared block (pinned, uncached alias) | |
| `be_call` argument ring | 4 × 384 = 1,536 | shared block | |
| COP log ring | 2,048 (1,024 floor) | shared block | |
| `be_svc` queue (`event_queue`, 16 events) | ~160 | `.ncbss` | |
| **Total resident** | **~13.4 KB** (~12.4 KB with the 1 KB log ring) | | [estimate] |
| Code | +8 to 14 KB text [estimate] | `fe_be_core.c` ~2 KB, wrappers ~4-8 KB, `be_svc` handlers ~2-4 KB | |
| IRAM | 0 | | no change |
| COP worker stack (19-1c) | −9,216 when lit | codec slot | not taken under `HAVE_COP_FRONTEND` (not resident; from the codec slot) |

**Against the budgets** (`hardware.md`):
- **`_end`:** P16's `_end` is 1,157,744 B against the **1,500,000 B** cap. About +2 KB of statics and +8 to 14 KB of text leaves well over 300 KB of room.
- **Audio-buffer floors:** the pinned blocks (~13 KB) come out of the audio buffer. Against the npfix arithmetic floor (11,558,368 B on 32 MB against 7,500,000; 44,391,824 B on 64 MB against 39,000,000) that leaves about 4.05 and 5.38 MB of headroom. **The floor gate re-measures at S3.**
- **The memory floor rule** (resident cap 1.5 MB; audio ≥ 7.5/39 MB): met.
- **The firmware file:** 880,240 B + ~14 KB against the ~1,250,000 B backstop: met.
- **Statics:** none over 1 KB (the shared block is a `core_alloc`).

---

## 10. Device-reality checklist (brief §7, for these packages)

| Item | Answer |
|---|---|
| Compiled features | two flags, off by default until S9; `HAVE_COP_CODEC` off under `HAVE_COP_FRONTEND` |
| Memory during playback | +~12 to 13 KB pinned, from the audio buffer (§9) |
| CPU at 50k tracks | view builds and searches run as BE jobs as today (they move off the UI thread); the FE reads the published views only |
| Storage wake-ups | none added: the FE does no I/O; `be_call` file kinds are bulk |
| LCD | partial pushes as today; the uncached CPU-side reads only on the fallback paths |
| Threads and locks | +1 thread; the kernel objects moved to uncached (§3.2); no new lock order. The port lock is outermost, then `lcd_state.cl` (as today) |
| Splashes | none added; COP panics logged (K-4) |
| Persistence | settings saves and resume writes run on `be_svc` as today's code, unchanged in order |
| Scale | the `be_call` batching is per screen, not per row |
| Stock firmware | untouched; no bootloader change |
| Dark wakes | 0 COP wakes (K-1, `TIMEOUT_BLOCK`); the dark face timer disarmed (S7) |
| Boost balance | `cpu_boost` sites unchanged in pairing; K-3 accounts per core |

---

## Appendix A. The BCM session's audit table (merge point)

*The BCM session's full table (`fc9222f324`, `forge/p19-bench-p12b`; docs on `p19-bcm-prep`) merges here when it is re-anchored to the P19 line (§6.4 item 9). The summary as received, 2026-10-24, is folded into §6.1; the rows below hold its file:line references until then.*

| # | Item | Refs [bcm] | Design section | Step |
|---|---|---|---|---|
| MF-1 | Coherent shared state | `lcd-video.c:572` (`lcd_port_mtx`; `:563` on this branch), `ata-common.c:31` (model), `bcmjob-dev.c:44-45`, `e2-port.c:64-84`, `bcm-devtest-hal.c:517-545`, `lcd-video.c:159-160`, `bcmjob_gen` | §3.2 | S0 |
| MF-2 | One writer for the driver mirror `b` | `bcmjob-core.c:42-67`; `bcmjob_suspend`, `dt_take`/`dt_restore`, `bjc_dark` | §6.1, §6.2 | S0 |
| MF-3 | Tick-list edits and power transitions on the CPU | `tick.c:32-67`, `kernel-pp.c:25-37`, `bcmfw-boot.c:113…688` | §2.2, §6.2 | S0, S8 |
| MF-4 | The pixel hand-off contract | `e2-port.c:118`, `lcd-video.c:1172` (`:1156` on this branch), `vplayer.c:1038` | §6.3 | S0, S7, S8 |
| MF-5 | Allocations and pins on the CPU | `bcmjob-cpu.c:60`, `bcmjob-dev.c:186` | §2.4 | S0 |
| s-1 | `ui_log` drops COP callers | `uilog.c:22` (`:143-153` on this branch) | §2.2 | S7 |
| s-2 | `power_sleep()` refuses off the CPU | `power_sleep-pp502x.c:180` | §4 | S3 |
| s-3 | `cpu_boost`, `USEC_TIMER`, `current_tick`, IRAM fine | — | §5 | — |
| q-1 | Engine 2 from the COP | Apple's refusal; DP-6 | §6.4.1 | S6 |
| q-2 | One shared cache data array? | `cores-and-threads.md` §3 | §2.1, §6.4.2 | S6 |
| q-3 | Yield-polling on the COP keeps it out of `PROC_SLEEP` | — | §3.5 (no FE wait polls) | S7 |
| (rows to come) | heartbeat poll ownership; CPU_INT against COP_INT routing; engine 2 against the CPU's audio DMA; `logf` from the COP | | §6.2, §1.2, §3.6 | |
