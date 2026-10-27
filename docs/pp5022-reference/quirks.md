# Quirks and gotchas

One list of everything that bites. Each item names the file with the detail.

## Start-up and memory

1. **Both cores run the same entry code.** Branch on `PROCESSOR_ID` (`0x6000_0000`, low byte `0x55`/`0xAA`) before touching anything shared. [rockbox] (`boot.md` §4)
2. **The remap must run from IRAM**, once per core, CPU first. Rewriting MMAP0 while fetching from SDRAM pulls the floor out from under the code. [rockbox] (`memory-map.md` §3)
3. **Probe the RAM size before the remap**: after it the image has already assumed a size. [rockbox]
4. **Zero all MMAP registers and disable the cache before jumping to another image**, or it starts with your mapping. [rockbox] `rolo.c`
5. **`CACHE_CTL_VECT_REMAP` survives into the next image**; some images do not clear it and hang. [rockbox]
6. **The ROM leaves its own state in the last 256 bytes of IRAM** and Apple's firmware reads a boot-failure flag in upper IRAM; clear `0x4000_C000`–`0x4001_FFFF` before powering off if Apple's firmware must boot cleanly next. [rockbox] `power-ipod.c`
48. **What a reset leaves in SDRAM (for a record that must outlive one).** By the code: Rockbox's `crt0-pp.S` clears only `.ibss` and `.bss`/`.ncbss` (below `_end`, ~1.1 MB) and writes one byte each at physical `0x11FF_FFFF` and `0x13FF_FFFF` (the 32/64 MB probe); our bootloader loads the image at `0x1000_0000` (up to 8 MB) and puts its `.bss` at `0x1100_0000` (+204 KB); the ROM loads `osos` (Apple's OS plus the bootloader, under 8 MB) at `0x1000_0000`. So the 256 bytes under the codec buffer (fw-ui: logical `0x03EF_FF00`, physical `0x13EF_FF00`; a 32 MB unit `0x11EF_FF00`), which `HAVE_PANIC_RECORD` keeps out of the audio buffer, are written by none of them. [rockbox] (code reading, 2026-10-19). Whether the bytes **survive** is a separate question: a `DEV_RS` system reset stops SDRAM refresh until the ROM sets the controller up again, and the MENU+SELECT reset's mechanism is not known (it could cut power to the SDRAM). [hypothesis] until the device test in `docs/hardware.md` ("Panic record") is run. Holding MENU starts Apple's firmware, which uses all of SDRAM: a record is lost then. Write such a record only through the uncached alias, or a dirty cache line is lost at the reset. (`memory-map.md` §5)
51. **A misaligned halfword or word access to SDRAM is a data abort.** A Thumb `strh r3, [r4]` with `r4 = 0x0033_F319` (odd) aborted at that instruction, every time the game started (Chute's, then called Airdrop, state record handed out at `state_img + 17`). The simulator, the host tests and unicorn all accept the access silently, so only the device catches it: anything the firmware hands to code that casts it to a struct (a game's state record, a buffer) must be aligned to at least the struct's alignment; we use 8. [silicon] (P12 device bench 2026-10-24, `ipodui.log` boot 13:52:25: `panic pc=0033e266`, r0–r7 in the record; fix `SDK_REC_OFF`, `apps/ipodui/sdk/sdk_core.h`). **How we catch it before the device** (2026-10-25): the UBSan sim runs with `-fno-sanitize-recover=alignment` and a misaligned access anywhere in the shell, target or bcmfw code fails the gate; every host test is compiled through `scripts/align-shim` (alignment sanitizer, no recovery); `tests/aligncast` baselines byte-offset casts to 16/32-bit pointers. Rule: `docs/process.md` lean coding rule 12. The host-side findings so far were test models, not firmware: a chip-memory stub that cast a 2-mod-4 address (`tests/bcmjob`), and a shim config that took `fat.c` down its non-`UNICODE32` path (`tests/fatcrash`)
7. **Most register pages are mirrored** and unused words read `0xCACA_D0D0`; a wild pointer into `0x6000_xxxx` often reads plausible values. [external] siggi `08`

## Cache and coherency

8. **No coherency between the cores' caches**, and none with DMA. Shared data goes in IRAM or the uncached alias (`0x1000_0000 | logical`). [rockbox] (`cores-and-threads.md` §4)
9. **Do not invalidate with `CACHE_OPERATION`'s invalidate bit.** Rockbox rewrites every status word to "valid, clean, unused address" with interrupts off, because invalid lines corrupted memory under heavy writes plus instruction fetches. Initialise the cache by reading 8 KB so every line is valid. [rockbox]
10. **Cache maintenance is whole-cache only** (8 KB flush per operation). [rockbox]
11. **Cacheability is by address window** (`CACHE_MASK`): logical SDRAM `0x0`–`0x03FF_FFFF` and flash `0x2000_0000`–`0x23FF_FFFF` are cached; physical SDRAM `0x1000_0000` is not. [rockbox]
51. **Open: does each core have its own cache data array?** There is one data window but two status windows. [hypothesis] one array per core; the bench row **COP row C2** decides it (`cores-and-threads.md` §3; `../research/bcm2722/bench-p19.md` C2).
52. **A cache write-back is per core.** `commit_dcache()` writes back only the calling core's cache. So a DMA start on the COP (`e2port_start()`) does not write back the CPU's dirty lines of its source: the producer core must write them back itself. [ours] (code reading, 2026-10-24). Whether engine 2 runs correctly when started and polled from the COP is open until **COP row C1** runs. [hypothesis]
53. **Open: polling with `yield()` keeps a core awake.** The yielding thread is still runnable, so the scheduler never reaches `core_sleep()`. On the COP, where the poller is often the only thread, the core never sleeps. [hypothesis] until **COP row C3** runs (`cores-and-threads.md` §7).
54. **A cross-core mutex does not make the data it guards coherent.** Both cores take the lock in turn, but each still reads its own cached copy of the guarded words: one core's write stays in its cache (or lands in SDRAM under the other's stale line). Guarded state that both cores use goes in the uncached alias as well. [silicon] symptom, bench sitting 3 (2026-10-26: the DAC's standby wake replayed over music with the UI on the COP); [ours] mechanism by code reading and the host model `tests/audioxcore` (`../data-paths.md` R7 item 8, D15)
55. **The PCM driver and mixer are single-core.** The playback DMA interrupts the CPU's FIQ only; `pcm_play_lock()` masks that FIQ (from either core, but its depth counter is a plain increment) and the mixer's control words are cached. A COP that starts a channel or the PCM leaves the CPU's FIQ playing stale frames (the replayed click: a tone). Requests go to the CPU. [silicon] symptom (sitting 3); [rockbox-derived] driver (`pcm-pp.c`: "dual core support will require other measures"); [ours] the rule (`../data-paths.md` R7 item 8)

## Clocks

12. **The PP5022's PLL must run at ≥ 96 MHz before its post-divider**; get 30 MHz as 120/4, not 30/1. [rockbox]
13. **Every `CLOCK_SOURCE` write needs both cores stalled** (`scale_suspend_core`) and interrupts off. Step to 24 MHz before retuning the PLL. [rockbox] (`clocks-power.md` §4)
14. **`DEV_TIMING1` must follow the clock** (`0x0808` at 80 MHz, `0x0303` below): raise before speeding up, lower after slowing down. [rockbox]
15. **Clock frequency changes stall both cores** (one clock for both). [rockbox]
16. **UDMA above mode 2 needs 80 MHz**; Rockbox boosts per transfer. [rockbox]
17. **The host port's throughput is bound by the PP clock** (~10.5 PP cycles per 16-bit transfer): a full LCD frame costs ~27 ms of CPU at 30 MHz. [silicon] S8

## Interrupts and timers

18. **Reading `TIMERn_VAL` acknowledges the timer**; reading a DMA channel's `STATUS` acknowledges it too — read it anywhere else and the handler never sees the interrupt. [rockbox]
19. **The wheel controller raises the I2C interrupt** (high source 40). An I2C driver that enables that source will receive wheel events. [rockbox]
20. **The COP gets no tick of its own** in Rockbox's design; the CPU wakes it from the Timer 1 handler. [rockbox]
21. **An interrupt enable followed by a disable must take effect in order** (Apple's and Rockbox's code depend on it). [external] siggi `06`
56. **A branch to address 0 after boot used to spin for ever.** After `cpu_init` copies the vectors to 0, address 0 holds `ldr pc, [pc, #24]` and the reset word it loads was `start`, which links at 0 as well: a NULL function call (or a `--wrap` whose real body was never linked, the 2026-10-26 freezes) looped on that one instruction with the IRQ mask as it was, so with IRQs masked nothing (no tick, no watchdog task) ever ran again and no record was kept. The reset word now points at `reset_reentry_handler` (`crt0-pp.S`), which reports "Jump to 0" through `UIE()` (record, restart). No real start uses that word: the ROM, our bootloader and ROLO all enter the image at `DRAM_START` before the remap, and a restart is a `DEV_RS` reset. [rockbox] (code reading) + [build] (`.vectors` dump: word 0 = `reset_reentry_handler`); the device has not yet taken this path.
57. **The interrupted pc is the IRQ stack's top word.** `irq_handler` (a GCC `interrupt("IRQ")` function) starts `sub lr, lr, #4; push {r0-r3, ip, lr}`, and IRQs never nest here, so inside a tick task the word just below `irq_stack` (crt0-pp.S, now global) is the pc the tick interrupted; System mode's sp/lr are the interrupted thread's, readable by a brief `msr cpsr_c, #0xdf`. The hang watcher uses both (`firmware/common/hang_watch.c`). [build] (fw-ui disassembly, gcc 16, 2026-10-26); re-check after a compiler change.

## Peripherals

22. **I2C moves at most 4 bytes per transaction** and has no visible NAK. [rockbox] (`gpio-i2c.md` §6)
23. **The WM8758 is write-only**; keep a shadow of every register. [rockbox]
24. **The codec is the I2S master**; set `IISCONFIG.MASTER` and the PP fights it. [rockbox] (`audio.md`)
25. **Stopping I2S DMA does not stop the transmitter**; Rockbox leaves `TXFIFOEN` set and the PP keeps shifting an empty FIFO. [rockbox]; [silicon] S34
26. **Never read the ATA status register during a DMA transfer**: it hangs; wait on `IDE0_CFG` bit 3. [rockbox]
27. **DMA engines (general, IDE, USB) use physical addresses**: add `0x1000_0000` to logical SDRAM addresses. [rockbox]
28. **ATA DMA reads need 16-byte-aligned buffers**; DMA writes are slower than PIO on hard disks. [rockbox]
29. **The 5.5G's partitions are in 2048-byte units on a 512-byte drive.** [rockbox] (`storage.md` §4)
30. **Our flash adapter negotiates UDMA 2**, because it leaves IDENTIFY words 76 and 93 at 0. [silicon]
31. **Disabling the USB block in a bootloader stops Apple's firmware from loading.** [rockbox]
32. **The PP5022 needs its USB PHY sequence after every controller reset.** [rockbox] (`usb.md` §2)
33. **The backlight has no absolute brightness register**: count pulses from the known level 16 after enabling. [rockbox]
34. **A 32-bit read through the BCM host port is two 16-bit reads** and can be torn by the BCM writing between them. [silicon] S11, S12
35. **Rockbox's host-port waits are unbounded**; an unpowered BCM reads `0xFFFF` and the waits hang. Bound them. [rockbox]; [ours]
36. **PP DMA into the BCM's host port has never delivered a byte.** S8 reported SRAM clean, but the CPU had just written the same frame there, so that readback was invalid; S32–S33 lost and shifted words in SDRAM, and S34/S35 landed nothing in SDRAM or SRAM (S35). DMA was never faster than the CPU copy either. [silicon] (`dma.md` §4; `../data-paths.md` D1)
37. **After a DMA block to the port, wait for the port's write-ready bit** before setting a new address: the engine finishing is not the port accepting. [ours] after S32
38. **Rockbox's `DEV_PIEZO` equals `DEV_OPTO`** (`0x1_0000`, the wheel); the piezo's PWM is bit 17 (`DEV_PWM`). [external] siggi `05`; [rockbox]
39. **Standby without `CHGWAK` or `EXTONWAK` in the PMU's `OOCC1` leaves the unit unable to wake.** [rockbox] (quoting iPodLinux)
40. **The PMU's interrupt flags are cleared by Apple's boot ROM**, so firmware cannot tell why it woke. [rockbox]
41. **The drive's write cache is on and flushed only at standby and shutdown** (upstream `ata.c`): the order write commands complete in is not the order sectors reach the media at a power cut [hypothesis]. Our shell build issues FLUSH CACHE at every write-order point of the file layer (`../data-paths.md` R11; #91). Whether our flash adapter reports or honours the cache is open question #32. [rockbox]; [ours]

## Scheduling (measured on our unit)

41. **At 30 MHz, priority beats frequency**: a feeding thread below the codec's (boosted) priority ran up to 2.1 s late; with the right priority it held 16 ms drift unboosted. [silicon] S31, S34
42. **The ROM changes nothing back**: a kernel cannot rely on any peripheral being in its reset state at entry (the BCM is running, the wheel block enabled, the IDE controller initialised). [rockbox]; [external]
## USB (measured on our unit)

43. **A USB INQUIRY reply in a movable or cached buffer may never retire.** Use a static, page-aligned buffer through the uncached alias. [silicon] (`usb.md` §6)
44. **USBMODE's stream-disable bit does not exist on the PP5022**; it reads back unset. [silicon] (`usb.md` §6)
45. **Offset `0x164` (TXFILLTUNING on other ARC cores) reads back 0 here**, and writing it correlated with dead enumeration. Leave it alone. [silicon] (`usb.md` §6)
46. **A bus reset leaves queue-head RAM as it was**; clear each endpoint's overlay, halt and toggle at configuration. [silicon] (`usb.md` §6)
47. **A bulk-IN dTD can sit ACTIVE for ~304 ms with no error bit** until the host resets the bus; ATA DMA running at the same time is a material cause. [silicon] (`usb.md` §6; `../data-paths.md` D12)
49. **Our bootloader (`bootloader/ipod.c`) never reads the battery.** Its `adc_init()` sits in `#if 0`. It brings up the LCD and backlight, initialises ATA, mounts and loads the firmware whatever the cell's voltage. A flat cell that has recovered at rest therefore reaches the firmware, which must make the battery decision itself (the boot battery guard, `docs/hardware.md`). [ours] (source reading, 2026-10-22; not changed: bootloader edits need the owner's sign-off)
50. **The FAT layer's sector is not the BPB's on the 5.5G.** With `MAX_VIRT_SECTOR_SIZE` the file layer works in 512-byte sectors (`fat_file_sector_size()`, `LOG_SECTOR_SIZE`) while `fat_get_bytes_per_sector()` returns the BPB's 2,048; code that sized a one-sector file by the latter skipped its write, and the owner's unit carried 0-byte `.rename_intent` and `.bootbatt` files. [silicon] (device backup 2026-10-23; host model `tests/fatcrash/fsreplay records`)
