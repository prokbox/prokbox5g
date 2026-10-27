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

## 4. What is not known

- **Current draw in any state has not been measured** ([4](boot-and-clocks.md) §5; [15](open-questions.md) #12). The power-contract policy is written so that each future measurement changes a number, not the structure.
- Whether the idle-sleep build (timer compare every 1 ms, optionally the host doorbell) works on silicon: no session has run it ([17](host-driver.md) §7; [15](open-questions.md) #3).
- Whether the regulator's bits `[7:4]` and the block clocks matter for power ([4](boot-and-clocks.md) §4). The block clocks' owners are now mapped from Apple's code ([19](brief-map-power.md) §5); stage PW1 reads which ones each image leaves running.
- What `GPO32` bit 15 does on silicon, and whether SRAM and SDRAM survive Apple's hibernate (PW5).

## 5. Related designs

[`../research/bcm2722/power-contract.md`](../research/bcm2722/power-contract.md) (states C0–C5 and policy), [`../research/bcm2722/p18-0-runbook-bcm.md`](../research/bcm2722/p18-0-runbook-bcm.md) (stages 5i and 5s, the idle-sleep and standby spikes), [`../hardware.md`](../hardware.md) (the project's power budget).
