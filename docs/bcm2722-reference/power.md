# 13. Power

This chapter gathers, in one place, everything the manual says about the BCM's power: how it is switched, what each state costs to leave, and what has never been measured. It adds no new findings. The facts and their evidence tags are in the chapters named beside each item; where a line below restates a number, it carries that chapter's tag.

## 1. Who controls what

| Control | What it does | Where |
|---|---|---|
| `GPO32` bit 15 (host) | Apple raises it before clearing bit 14 when it hibernates the chip, and lowers it after bit 14 comes back. It is most likely a hibernate (retention) request [apple; meaning hypothesis]. Never driven by Rockbox or by us | [19](brief-map-power.md) §3 |
| `GPO32` bit 14 (host) | Powers the BCM. Clearing it loses all BCM state (SRAM, SDRAM, registers); the next use is a cold bootstrap [rockbox]; works with our images [silicon S4–S7] | [4. Boot and clocks](boot-and-clocks.md) §5 |
| Core regulator `0x10001400` | Core supply level, set from the core frequency; host clears bits `[7:4]` before a power cut. Apple's table gives level 5 up to 46.9 MHz; a host-started image sits at 11 [apple; silicon S7] | [4](boot-and-clocks.md) §4, [19](brief-map-power.md) §4 |
| Apple's power manager | `pm_set_policy min`: the clock follows load from 5 to 150 MHz in 1 MHz steps, with the regulator level following [apple] | [19](brief-map-power.md) §2 |
| `power_control videocore=N` | Apple's park-and-handshake through SRAM `0x1F0`: 1 = hibernate, 0 = power-down [apple] | [19](brief-map-power.md) §3 |
| Core clock select and PLL A | 27 MHz (crystal) or 150 MHz (PLL A). The switch up costs a 10 ms regulator wait and a 1 ms PLL wait [apple values] | [4](boot-and-clocks.md) §4 |
| Block clock controls (`0x1000_1C2C`–`0x50`) | Apple zeroes them all before the switch; consumers unknown | [4](boot-and-clocks.md) §4 |
| `sleep` instruction | Idles the core until an interrupt (timer compare, host doorbell). Apple's idle loop is `ei; sleep; di` with a 10 ms tick [apple] | [4](boot-and-clocks.md) §5, [10](interrupts-timers-gpio.md) |
| `LCD_SLEEP` (`BCM_CMD(8)`) | Runs the panel's off script | [9. LCD](lcd.md) §5; [5](host-interface.md) §3a |
| Backlight | The host's, not the BCM's | [17](host-driver.md) §6 |

## 2. The power-down order and the cost of waking

- Rockbox's order, which also works with our images: backlight off; regulator `&= ~0xF0`; `LCD_SLEEP`; `BCM_CMD(0xC)`; clear `GPO32` bit 14 ([4](boot-and-clocks.md) §5). Stay off at least 50 ms (ours 100 ms) ([17](host-driver.md) §3).
- Every wake is a full cold start: bootstrap, upload, start, panel power-on on the first update. Image ready about 2 ms after upload; first frame 436–458 ms after the upload began, of which the panel's own waits are about 265 ms [silicon S7] ([4](boot-and-clocks.md) §5).
- Measured session timings with our video image: image up 612–616 ms after the request with a power-down, 370 ms from an already-off BCM [silicon S30] ([17](host-driver.md) §3). Apple's firmware is back after a power cycle and reload in about 580 ms [silicon S7] ([5](host-interface.md) §1).
- The backlight-sleep case: Rockbox cuts the BCM's power with the backlight [rockbox], so the wake is a reload plus a seek to the audio ([17](host-driver.md) §3).

### Apple's hibernate (mapped, not run)

Apple's hibernate keeps SRAM by design, by parking the chip and then using `GPO32` bits 15 and 14 in order. It would replace the cold wake above with one of about 7–17 ms [estimate]. Stage PW5 tests it ([19](brief-map-power.md) §3, §9); it is built (Developer › Advanced › Video chip › Probes "BCM probe PW5: hibernate and wake", `HP_OP_HIB`), checked in vc2sim only, not run on silicon. The production path that would use it (a PP-uploaded stub, `HP_OP_PARK`'s hibernate form in the jobs images, a retained-memory sum before the image answers, and a break-even rule for how long to hold) is designed and built in vc2sim only, off until PW5 passes: [`../research/bcm2722/power-contract.md`](../research/bcm2722/power-contract.md) §6.6. The same document's §7 models the BCM's share of lit current [estimate].

## 3. Panel states other than "cut"

Our panel standby (the display off with the panel's power circuits on, or the whole off script with reset high) is [ours] and **not yet run on silicon**; see [17](host-driver.md) §3, "Panel standby", and the power states and policy in [`../research/bcm2722/power-contract.md`](../research/bcm2722/power-contract.md), which is a [ours] design. Open questions #49 and #50 ([15](open-questions.md)) ask whether the panel wakes from standby with the right picture and whether SRAM holds there.

### Built in the simulator, not yet run on silicon (P19 batch 5)

| Mechanism | What the image does | Evidence | Open on silicon |
|---|---|---|---|
| SDRAM idle at `CLOSE` | Flag `HP_CLOSE_SDRAM_IDLE` (0x1) in `CLOSE`'s flags word: after the session has ended (blits waited for, the clock back on the crystal, the jobs pools back to the UI), the image runs Apple's idle itself (cache flushed, cache-control bits 30 and 16 cleared, `0x1000_3420` written 0, bit 15 polled up to 8,000 times [apple-RE @14892]) and marks SDRAM down, so the next `OPEN` or staged `ALLOC` runs the whole bring-up again and a hibernate `PARK` is accepted. The reply gains one word after its usual ones: 0 idled, 1 bit 15 never cleared (still marked down), 2 SDRAM was not up. Flags 0: unchanged. In the plain and merged images alike; resolves the "image believes SDRAM is up" half of `quirks.md` #55 | [sim] `hibwake_test.py` merged row: the write of 0 is the last SDRAM-side access, the next `OPEN` re-runs command 7 and decodes the same pictures, `PARK` hibernate then answers OK | Whether bit 15 clears as Apple's poll expects; whether SDRAM keeps its contents across the idle (`quirks.md` #55, #7; the AAC tables are placed once a boot and then fail their sum, refused with `HP_E_STATE`). No PP caller sends the flag yet |
| Adaptive idle tick | The idle tick (timer compare 1) re-arms with a word `idle_wait` sets before each sleep: 10 ms [apple-RE: Apple's RTOS tick] once the doorbell handler has run at least once since start and nothing BCM-timed is pending (a jobs animation, the overlay's slide, an open session), else 1 ms, exactly as before. A shrinking period re-arms the compare at once. `BCMFW_INFO_TICK_SLOW` (bit 3 of the info word `0x1E8`) says which runs. Video and jobs images only (`BCMFW_IDLE_ADAPT`); the device-test images keep 1 ms, since stage 5i counts ticks against time. The PP's job-driver liveness check rings the doorbell before it watches the heartbeat, which now steps every 10 ms while idle | [sim] `jobs_image_test.py` `idle_tick`: idle wakes 1,001/s before, 102/s after; 1 ms with no doorbell, during an animation and in a session | Whether the doorbell (source 23) wakes the core on silicon; with no doorbell seen the image never leaves 1 ms |

## 4. What is not known

- **Current draw in any state has not been measured** ([4](boot-and-clocks.md) §5; [15](open-questions.md) #12). The power-contract policy is written so that each future measurement changes a number, not the structure.
- Whether the idle-sleep build (timer compare every 1 ms, optionally the host doorbell) works on silicon: no session has run it ([17](host-driver.md) §7; [15](open-questions.md) #3).
- Whether the regulator's bits `[7:4]` and the block clocks matter for power ([4](boot-and-clocks.md) §4). The block clocks' owners are now mapped from Apple's code ([19](brief-map-power.md) §5); stage PW1 reads which ones each image leaves running.
- What `GPO32` bit 15 does on silicon, and whether SRAM and SDRAM survive Apple's hibernate (PW5).
- Whether our image holds level 5 at the crystal (row `l5`), sleeps inside a video session (row `vs`), runs with the audio and USB-side block clocks off (row `ck`), and survives Apple's SDRAM idle (row `sd`): built 2026-11-05 (`p19-vcpwr`), **not run** ([ours]; `../research/bcm2722/bench-p19.md` "The VC power rows").

## 5. Related designs

[`power-apple-vs-ours.md`](power-apple-vs-ours.md): Apple's power manager against ours for awake time (what to adopt, with bench rows). **Built** (2026-11-05, `p19-vcpwr`, not run): rows `s1`, `l5`, `ck`, `vs`, `sd`; the regulator write now waits Apple's 10 ms before its read-back in every build ([apple-RE] @5134, @141ba, @141f0); developer builds watch idle sleep and level 5 and latch either off on a failure ([ours]). A passing `s1`/`l5` saves `idle on`/`level 5` in `bcmres.cfg`, which every build with the resident image already honours. Gotchas from it: Apple's own menus ran 27 MHz at level 5 on a held PLL, not at its 5 MHz floor [silicon PW1 A]; the video session's idle gate is cleared by decision, not by test; our `lvl5` write skips Apple's 10 ms regulator settle.

[`../research/bcm2722/power-contract.md`](../research/bcm2722/power-contract.md) (states C0–C5 and policy), [`../research/bcm2722/p18-0-runbook-bcm.md`](../research/bcm2722/p18-0-runbook-bcm.md) (stages 5i and 5s, the idle-sleep and standby spikes), [`../hardware.md`](../hardware.md) (the project's power budget).
