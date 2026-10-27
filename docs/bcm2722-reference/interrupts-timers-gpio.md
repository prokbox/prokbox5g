# 10. Exceptions, interrupts, the system timer and GPIO

## 1. Vector table

The vector table is a table of handler **addresses** in SRAM at address 0, written by software [apple @14328; silicon S2].

| Vector v | Word at | Use | Evidence |
|---|---|---|---|
| 0–5 | `0x00–0x14` | Exceptions | [apple] six vectors, all pointing at halt stubs in `vmcs.bin` |
| 1 | `0x04` | Misaligned memory access | [silicon] S2, S3 |
| others in 0–5 | | Unmeasured. VideoCore IV numbering would put undefined instruction at 3 and bus error at 6 [hypothesis] | — |
| 32 + n | `0x80 + 4n` | Interrupt source n (n = 0–31) | [apple]; not yet confirmed on silicon (see "Open") |

Keep every unused vector pointing at a stub that records the frame and halts; it makes stray exceptions visible [ours, proven S2 onward].

## 2. Entry and return

[apple] (the only frame consistent with Apple's interrupt code and RTOS; 4,983 of 4,983 interrupted contexts restored intact in simulation); [silicon] S2, S3 for exceptions.

- **Entry:** the hardware stores `sr` at `[sp − 8]` and the return pc at `[sp − 4]`, sets `sp −= 8`, clears `sr` bit 30 (interrupt enable) and jumps through the vector word.
- **`rti`:** `sr = [sp]`, `pc = [sp + 4]`, `sp += 8`.
- A handler must save every register it uses. Apple's full save is `push r0–r5, lr` then `push r6–r15`, undone by one `pop r0–r15` (`isa-scalar.md`, push order).
- Exceptions use the same frame [silicon S2].

### Exceptions on silicon

[silicon] S2 and S3 (identical results in both):

| Case | Result |
|---|---|
| Misaligned word load `ld r0, (r1)`, r1 = buffer + 1 | Vector 1. r0 receives the word at the address **rounded down** (no rotation or byte merge). Four later instructions executed before the exception was taken. Saved pc = the first instruction not executed |
| Misaligned pre-decrement load `ld r0, (--r5)`, r5 = `0x7F` | Vector 1. r5 written back (`0x7B`) although the access faulted; r0 = word at `0x78`. Two later instructions executed first |

So the exception is **imprecise**, `rti` resumes correctly with no adjustment, and a handler cannot identify the faulting instruction from the frame alone. Misaligned stores, odd halfword accesses, undefined instructions and bus errors have not been measured.

## 3. Interrupt controller (`0x1000_1000`)

[apple]; field meanings [hypothesis] where noted.

| Offset | Register | Use |
|---|---|---|
| `+0x00` | Level | `[2:0]` current level: sources at or below it are held off. Apple's RTOS writes it at every level change (@207A) |
| `+0x04` | Pending | `(priority << 8) \| vector` of the highest pending source, 0 if none. Read by Apple's generic handler (@BE2) |
| `+0x10–0x1C` | Priority | 4 bits per source, 8 sources per word (source n at word n/8, nibble n%8); 0 = disabled |

Sources [apple]:

| n | Source | Evidence |
|---|---|---|
| 0–7 | DMA channel n | [apple] @7DC0 |
| 11 | Display TE / vsync (handler measures the frame period; GPIO 49 is set up as an edge input beside it) | [apple] @12AE6, @CB1C |
| 12 | Unknown (handler @14780) | [apple] |
| 16–19 | Timer compare 0–3. Apple: 16 = 10 ms system tick, 18 = software timers, 19 = a 32 µs TV line poller | [apple] |
| 23 | Host doorbell; acknowledge by writing `0x1000_3800 = 1` | [apple] @D52A, @D56A; trigger [hypothesis] |
| 30 | Unknown (@1413E) | [apple] |

Enable an interrupt: write the vector word, set the source's priority nibble non-zero, keep the level below it, then `ei` (`isa-scalar.md`). To wait for one, `sleep`: Apple's idle loop is `ei; sleep; di` [apple].

## 4. System timer (`0x1000_1800`)

| Offset | Register | Evidence |
|---|---|---|
| `+0x00` | Match status: bit k = compare k matched; write 1 to clear | [apple] |
| `+0x04` | Free-running 32-bit counter, **1 MHz** once its clock is enabled | [apple] `univ_getstcfreq` returns 1,000,000; [silicon] S4 (agrees with the host clock to 0.1%) |
| `+0x08`, `+0x0C`, `+0x10`, `+0x14` | Compare 0–3: match when the counter equals the value; raises source 16 + k | [apple] |

**The counter does not run after a host start**; enable its clock first (`boot-and-clocks.md` §3) [silicon S2, S4]. Its rate does not change when the core switches to 150 MHz [silicon S7]. It is a good time base for delays; bound every wait on it by a spin count as well, in case it is stopped.

Periodic tick, Apple's way [apple]: `+0x00 = 0xF`; `C0 = counter + 10000`; source 16 priority 1; in the handler, clear bit 0 and add 10,000 to C0.

## 5. GPIO (`0x1000_2800`, 52 pins)

The layout matches the later BCM2835 GPIO block in shape [apple usage; the BCM2835 comparison is an analogy].

| Offset | Register | Evidence |
|---|---|---|
| `+0x00–0x0C` | Function select, 2 bits per pin, 16 pins per word: 0 = input, 1 = output, others unknown | [apple] @13E66; [silicon] S4 (GPIO 48 as output) |
| `+0x10` / `+0x14` | Set pins 0–31 / 32–51 (write 1s) | [apple]; [silicon] |
| `+0x18` / `+0x1C` | Clear pins 0–31 / 32–51 | [apple]; [silicon] |
| `+0x20` / `+0x24` | Level of pins 0–31 / 32–51 | [apple]; [silicon] S1 |
| `+0x28` / `+0x2C` | Event status, write 1 to clear | [apple] |
| `+0x30/+0x34`, `+0x38/+0x3C` | Rising- and falling-edge enables | [apple] writes; meanings [hypothesis] |
| `+0x40–0x5C` | Zeroed at start-up; unknown | [apple] |
| `+0x60` | Pull mode: 0 off, 1 down, 2 up | [apple] |
| `+0x64`, `+0x68` | Pull clock strobe (pin mask), with 150-iteration waits around it, as BCM2835's GPPUD/GPPUDCLK | [apple] @13F7A |

Pins with a known job:

| Pin | Job | Evidence |
|---|---|---|
| 1, 2, 3 | Audio output when set to function 2 (Apple's `audio_enable 1`); GPIO 1 back to output on `audio_enable 0`. Roles of the three pins unknown | [apple] @136A6 (`audio.md` §4) |
| 19 | Output, low at boot; **high only while the TV output is on** (Apple's TV update routine @5BCE) | [apple]; job [hypothesis: an external video buffer or switch] |
| 23 | Interrupt line to the host (to PP5022 GPIO C bit 6). Apple drives it high for at least 10 µs, at least 40 µs after it last fell | [apple] @1442E; [rockbox] host side |
| 48 | LCD reset, active low | [apple]; [silicon] S4 |
| 49 | LCD TE input | [apple] |
| 50, 51 | Panel ID (ID = GPIO 51 + 2 × GPIO 50) | [apple]; [silicon] S1 |

**Apple's boot pin map** [apple, `vmcs.bin` 1.3 start-up run in vc2sim]: outputs 1, 19, 23 and 48; every other pin an input. Pull-down on 0, 1 and 4–47; no pull on 2, 3, 49, 50 and 51 (the pins something outside drives); pull-up on 48. No pin but 1–3 is ever put on an alternate function. Details and what they say about the brief's serial interfaces: `brief-map-peripherals.md` §6.1.

On our unit, `+0x24` read `0x000D_0000` with Apple's firmware running (pins 48, 50, 51 high) [silicon S1].

## Open

- Interrupt delivery on silicon: no interrupt source has been confirmed on the device. Images that sleep on timer compare 1 (source 17) and acknowledge the doorbell were built and run in simulation; their device result is not recorded here.
- Which host write raises source 23.
- Sources 8–10, 12–15, 20–22, 24–31.
- The other exception vectors.
- GPIO function-select values 2 and 3 (alternate functions). Value 2 on GPIO 1–3 is the audio output [apple]; the rest unknown.
