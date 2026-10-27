# Interrupts and timers

## 1. Exceptions and vectors

The cores are ARM7TDMI (ARMv4T): the standard eight vectors at address 0 (`reset, undef, swi, prefetch abort, data abort, reserved, IRQ, FIQ`), no vectored interrupt controller in the ARM sense, no high-vector option. [rockbox] `crt0-pp.S` `.vectors`

Two ways to supply vectors:

| Method | How | Evidence |
|---|---|---|
| Remap SDRAM to 0 | MMAP0 maps SDRAM at logical 0 (`memory-map.md` §3); copy a vector page (`ldr pc,[pc,#24]` ×8 plus eight handler addresses) to logical 0 | [rockbox] `crt0-pp.S` — the main firmware's method |
| Local vector table | Set `CACHE_CTL_VECT_REMAP` (bit 4 of `CACHE_CTL`, `0x6000_C000`); the core then takes its vectors from `INT_VECTOR_ENTRY(v)` = `0x6000_F100 + 4·v` | [rockbox] `pp5020.h`; used by the USB-mode bootloader (`system-pp502x.c` `system_init`, `HAVE_BOOTLOADER_USB_MODE`). Whether the table holds handler addresses or instructions is not stated by the code — see `open-questions.md` |

**Gotcha** [rockbox]: some Apple/OEM images do not clear `CACHE_CTL_VECT_REMAP` themselves and hang if it is left set. Clear it before jumping to foreign code (`system_prepare_fw_start()`).

## 2. The interrupt controller (`0x6000_4000`)

Two banks of 32 sources: "low" (sources 0–31, registers at `0x6000_40xx`) and "high" (sources 32–63, at `0x6000_41xx`). The whole high bank appears as **one** low source, `HI_IRQ` = 30. Each core has its own enable mask and its own IRQ/FIQ selection; the raw status is common. [rockbox] `pp5020.h`

### Register map

| Low bank | High bank | Name | Access | Meaning |
|---|---|---|---|---|
| `0x6000_4000` | `0x6000_4100` | `CPU_(HI_)INT_STAT` | R | sources pending **and enabled for the CPU as IRQ** |
| `0x6000_4004` | `0x6000_4104` | `COP_(HI_)INT_STAT` | R | same for the COP |
| `0x6000_4008` | `0x6000_4108` | `CPU_(HI_)FIQ_STAT` | R | sources pending and routed to the CPU's FIQ |
| `0x6000_400C` | `0x6000_410C` | `COP_(HI_)FIQ_STAT` | R | same for the COP |
| `0x6000_4010` | `0x6000_4110` | `(HI_)INT_STAT` | R | raw status of every source |
| `0x6000_4014` | `0x6000_4114` | `(HI_)INT_FORCED_STAT` | R | software-forced sources |
| `0x6000_4018` | `0x6000_4118` | `(HI_)INT_FORCED_SET` | W1S | force a source pending |
| `0x6000_401C` | `0x6000_411C` | `(HI_)INT_FORCED_CLR` | W1C | un-force |
| `0x6000_4020` | `0x6000_4120` | `CPU_(HI_)INT_EN_STAT` | R | the CPU's current enable mask |
| `0x6000_4024` | `0x6000_4124` | `CPU_(HI_)INT_EN` | W1S | enable sources for the CPU |
| `0x6000_4028` | `0x6000_4128` | `CPU_(HI_)INT_DIS` | W1C | disable sources for the CPU |
| `0x6000_402C` | `0x6000_412C` | `CPU_(HI_)INT_PRIORITY` | RW | **a 1 bit routes the source to FIQ** instead of IRQ for the CPU |
| `0x6000_4030`–`0x6000_403C` | `0x6000_4130`–`0x6000_413C` | `COP_…` | | the same four registers for the COP |

The meanings of the `*_STAT` registers are inferred from how Rockbox's handlers use them; `INT_PRIORITY` = "FIQ select" is how Rockbox uses it (it sets the DMA bit, or the I2S bit, to get the audio FIQ) [rockbox] `pcm-pp.c`. Read-modify-write is safe on `*_PRIORITY` only; use the set/clear registers for enables.

**Masking at start-up.** crt0 writes `0xFFFF_FFFF` to `CPU_INT_DIS` (`0x6000_4028`) or `COP_INT_DIS` (`0x6000_4038`) before anything else; `disable_all_interrupts()` later clears both cores' low and high enables, both forced sets, and every GPIO port's `INT_EN`. [rockbox]

### Source numbers

| # | Name | Device | Evidence |
|---|---|---|---|
| 0 | `TIMER1_IRQ` | Timer 1 (the kernel tick) | [rockbox] |
| 1 | `TIMER2_IRQ` | Timer 2 (user timer) | [rockbox] |
| 4 | `MAILBOX_IRQ` | inter-core mailbox | [rockbox] (defined, unused by Rockbox) |
| 10 | `IIS_IRQ` | I2S FIFO | [rockbox] |
| 20 | `USB_IRQ` | USB OTG controller | [rockbox] |
| 23 | `IDE_IRQ` | ATA controller | [rockbox] (Rockbox polls instead, `storage.md`) |
| 25 | `FIREWIRE_IRQ` | FireWire | [rockbox] |
| 26 | `DMA_IRQ` | the general DMA engine (any channel) | [rockbox] |
| 27 | – | the second DMA engine (`0x6000_8000`) | [external] siggi `10`; [hypothesis] from Apple's interrupt numbers 27/28 for the two engines, which become 26/27 if Apple's numbers are the source plus one (`dma.md` §6.4) |
| 30 | `HI_IRQ` | the high bank (any of 32–63) | [rockbox] |
| 32 | `GPIO0_IRQ` | GPIO ports A–D | [rockbox] |
| 33 | `GPIO1_IRQ` | GPIO ports E–H | [rockbox] |
| 34 | `GPIO2_IRQ` | GPIO ports I–L | [rockbox] |
| 36 | `SER0_IRQ` | UART 0 (dock serial) | [rockbox] |
| 37 | `SER1_IRQ` | UART 1 | [rockbox] |
| 40 | `I2C_IRQ` | I2C block — and, on the iPod 4G-style pad (the Video included), **the clickwheel** | [rockbox] `irq_handler`: `CPU_HI_INT_STAT & I2C_MASK` → `ipod_4g_button_int()` |

Apple's firmware also uses source 13 as a software interrupt: its DMA handler sets it through `INT_FORCED_SET` to defer work [external] siggi `10`. Observed enable masks: Apple's firmware `CPU_INT_EN_STAT = 0xC000_0017`; Rockbox `0x4000_0003` plus whatever drivers add [external] siggi `06`.

Other numbers are unassigned in Rockbox. High-bank sources are tested by masks relative to 32 (`GPIO2_MASK = 1<<2` in the high registers).

### Dispatch, iPod Video [rockbox] `system-pp502x.c` `irq_handler`

On the CPU, in order: Timer 1 → Timer 2 → USB → high bank: clickwheel (`I2C`) → `GPIO2` with `GPIOL_INT_STAT & 0x10` (USB cable insert) → `SER0` (dock serial, when compiled). On the COP only Timer 2 is handled. The handler is a plain `if/else if` chain: one source per entry, the IRQ re-enters for the next.

**FIQ** is reserved for audio: the FIQ vector jumps through a patchable pointer (`fiq_function`) to the PCM DMA completion handler (or the recording handler). [rockbox] `pcm-pp.c`

## 3. Timers (`0x6000_5000`)

| Address | Name | Meaning | Evidence |
|---|---|---|---|
| `0x6000_5000` | `TIMER1_CFG` | bit 31 enable, bit 30 repeat (auto-reload), bits `[28:0]` period − 1 in **µs** | [rockbox] `kernel-pp.c`, `timer-pp.c` (`0xC000_0000 \| (n−1)`, max accepted period `0x2000_0000`) |
| `0x6000_5004` | `TIMER1_VAL` | **reading acknowledges** the timer's interrupt | [rockbox] |
| `0x6000_5008` | `TIMER2_CFG` | as `TIMER1_CFG` | [rockbox] |
| `0x6000_500C` | `TIMER2_VAL` | read to acknowledge | [rockbox] |
| `0x6000_5010` | `USEC_TIMER` | free-running 32-bit microsecond counter, wraps every ~71.6 min | [rockbox] `udelay()`, `TIMER_FREQ 1000000` |
| `0x6000_5014` | `RTC` | named "RTC" by iPodLinux; Rockbox does not use it (the wall clock is the PMU's, `clocks-power.md`). Believed a seconds-like counter [hypothesis] | [rockbox] |

On hardware the timer block repeats every `0x20` bytes through its page (`0x6000_5020` reads as `TIMER1_CFG`) [external] siggi `08`. Apple's firmware runs Timer 1 at `0xC000_03FC` (≈1 kHz tick) [external].

The timer clock is independent of the CPU clock: Rockbox writes µs values and never rescales them when it changes `CLOCK_SOURCE` between 24, 30 and 80 MHz [rockbox]. What clocks it during the 32 kHz sleep setting is unknown.

**Kernel tick** [rockbox] `kernel-pp.c`: `TIMER1_CFG = 0` (stop), read `TIMER1_VAL` (clear), `TIMER1_CFG = 0xC000_0000 | (interval_ms·1000 − 1)`, `CPU_INT_EN = TIMER1_MASK`. Rockbox's HZ is 100, so 10 ms. Each tick, after the tick tasks, the CPU **pulses the COP awake** (`core_wake(COP)`); the COP has no tick of its own.

**Changing a running timer's period** [rockbox] `timer-pp.c`: writing `TIMERn_CFG` from outside the ISR restarts the count; Rockbox defers the new period to the next ISR to keep the phase.

## 4. Processor sleep as a timer (`PROC_CTL`)

`CPU_CTL` (`0x6000_7000`) and `COP_CTL` (`0x6000_7004`) can put a core to sleep until an interrupt **or** until a countdown ends (`PROC_WAIT_CNT`, with a unit of clocks, µs, ms, or — on PP5022 and later only — seconds). Full bit table in `cores-and-threads.md` §2. Rockbox uses only "sleep until woken" and the countdown-with-clocks form in `scale_suspend_core()` (`clocks-power.md`).

## 5. GPIO interrupts

Each GPIO port has `INT_STAT`, `INT_EN`, `INT_LEV` (level/polarity), `INT_CLR` (`gpio-i2c.md`). The port's group raises `GPIO0/1/2_IRQ` in the high bank. iPod Video uses one: `GPIOL` bit 4 (USB cable) [rockbox]. The level register is used as a polarity toggle: on each edge the handler flips `INT_LEV` to catch the opposite edge (the USB-insert handler does this) [rockbox] `usb-fw-pp502x.c` — see `usb.md`.
