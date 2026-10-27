# 12. Audio

The BCM2722 has a serial audio output: a block at `0x1000_4800` fed by DMA channel 5, brought out on GPIO 1–3. Apple's iPod firmware drives it during video playback. Most facts here are [apple] (what Apple's code does) with meanings as [hypothesis]; our sessions have since run parts of it on silicon: the pins' wiring (S32–S34), a tone through the block and DMA channel 5 (S35), and a run where the block never became ready (S36c). §8 has those results. How GPIO 1 joins the headphone path is not known; see "Wiring" and `open-questions.md` #34–#38.

Addresses `@xxxx` are in `vmcs.bin` 1.3; `host 0x1xxx_xxxx` are in `osos` 1.3, loaded at `0x1000_0000`.

## 1. What Apple's firmware does with it

| Fact | Evidence |
|---|---|
| `MPLAYER.VLL` imports the eight `audio_play*` calls (`audio_playopen` @48AA, `audio_play` @450A, `audio_playstart` @49B4, `audio_playpause` @49A4, `audio_playclose` @47C8, `audio_playgetclock` @481E, `audio_playgetendclock` @485C, `audio_playgetlatency` @4890) and `univ_audiopresent`, and loads an audio decoder through `get_aud_dec_func_table` (exported by `AACDEC.VLL`, an Ittiam AAC decoder: `AACDecode_Ittiam`). Its strings include `noaudio`, `no audio decoder` and the `#audio.*` stream properties | [apple] export table at `0x2160C`; `.vll` symbol tables |
| No MIDI, synthesiser or wavetable code exists in any of Apple's VideoCore images; `.mid` appears only in `mplayer.vll`'s generic extension table. The brief's "64-polyphony MIDI player" was never shipped on the iPod (a design for our own is in `brief-map-peripherals.md` §4.4). No AC'97 or PCM-mode code exists either | [apple] strings and symbols of all eight images |
| `univ_audiopresent` (@138F8) returns 1 unconditionally. `univ_audiorecordpause` and `univ_audiorecordrestart` are bare returns; `univ_audiogetrecordposition` (@13710) returns 0. There is no audio input path | [apple] |
| The host command `audio_enable N` (string @BE80, handler @B8EC → @136A6): N ≠ 0 sets GPIO 1, 2 and 3 to function 2; N = 0 sets GPIO 1 to function 1 (output) and leaves 2 and 3 alone | [apple] |
| The host command `audio_control` (string @BEB0, handler @B8DE → @136A2) returns 1 for every argument and does nothing | [apple] |
| `osos` sends `audio_enable 1` before it starts or resumes the media player (host `0x101F_8EAC`, `0x101F_A3DC`) and `audio_enable 0` when it pauses or stops it (host `0x101F_91F0`, `0x101F_A2FC`, `0x101F_A56C`, `0x101F_A5D8`). The format string is at host `0x101F_9148`; the sender is host `0x101F_90F4` | [apple] |
| `osos` never sends `noaudio`, and its debug dump asks the player for `mp_get_stats audio` beside `mp_get_stats video` (host `0x101F_9754`) | [apple] |
| So in Apple's video playback the BCM demuxes and decodes the soundtrack and plays it through this block | [hypothesis, strong]: every piece of the path exists and is switched on and off with the player; no device measurement yet |
| The codec-control routine the BCM driver would use to set a DAC's registers (@13FC4) is an empty return, and the codec-ID read (@1411E) returns the constant `0x58`. The BCM never configures the iPod's audio codec; volume stays with the host | [apple]; meaning [hypothesis] |

## 2. The audio block (`0x1000_4800`)

| Offset | Apple's use | Evidence |
|---|---|---|
| `+0x00` | Control. Start-up writes `0x6001`, runs the driver's init, then writes 0 (@13900). Opening at a rate sets bit 0 and polls **bit 23** for up to 65,536 × 1 µs; a timeout fails the open (@14020). Starting playback ORs in `0x6209` (bits 0, 3, 9, 13, 14) after queueing a DMA block (@138E0). Closing writes 0 (@13954) | [apple]; bit meanings [hypothesis: bit 0 enable, bit 23 clock/ready] |
| `+0x04` | Data FIFO, the destination of DMA channel 5 | [apple] |
| `+0x10` | The driver's init writes 1 (@140F6) | [apple] |
| `+0x14` | Divider, password `0xA508` in bits `[31:16]` (not the usual `0xA5A5`). Init writes `0xA508_0010`. Opening at rate fs writes `0xA508_0000 \| clamp(⌊⌊6,144,000 / fs⌋ / 4⌋, 16, 0xFFFF)`: 32 at 48 kHz, 34 at 44.1 kHz | [apple] @14020; meaning [hypothesis: 24.576 MHz / (16·div) = fs, which is exact at 48 kHz and 2.4% fast at 44.1 kHz, so either the iPod runs the block as a clock slave or it resamples] |

Apple's driver is a table of nine function pointers at `0x295E0` (init @140F6, set rate @14020, mute/volume @13FC6 and @14086 through the stub codec writer, close @1411C, four empty slots), installed by @1410A when a stream opens [apple].

### Clocks

Opening the audio path asks Apple's clock manager (@52B6) for two clocks: id 2 at **24,576,000 Hz** (`0x0177_0000`, 512 × 48 kHz) and id 3 at **2,048,000 Hz** (`0x001F_4000`), both mode 3; closing returns both to mode 1, 0 Hz (@13C32) [apple]. The clock manager's id → register switch (@5334) sends **id 2 to `0x1000_1C34` and id 3 to `0x1000_1C38`** (password writes) [apple, 2026-10-23; the full table is in `brief-map-peripherals.md` §4.2]. In simulation the mode 3 requests wrote nothing, so their source (most likely a PLL) and encoding are still open (#65) [sim]. The same code confirms the crystal: @1490A returns 27,000,000 [apple].

## 3. DMA channel 5

[apple] @1373E, @1378C, @136CE; meanings per `dma.md` [hypothesis].

- **Config** `+0x00 = 0x0607_6002` when a stream opens: DREQ 7 (`[20:16]`), owner field 6; bit 0 clear, unlike the LCD's `0x0903_2101`.
- **Control blocks** live in a 32-entry ring of 32-byte blocks (`gp + 0x11260` = `0x30EA0`), index masked to 5 bits.
- **A silence block** heads every chain: source = a 16-byte zeroed buffer (`0x2EA00`), destination `0x1000_4804`, length 16, transfer information `0x1000_A040`, **next = itself**. An underrun therefore loops on silence instead of stopping.
- **Data blocks:** source = the PCM buffer, length = its size, transfer information `0x1000_A041`, plus bit 24 (interrupt on completion) when the caller asks. Bit 28 (peripheral destination), bit 15 (destination paced by DREQ), destination width 4 bytes (`[14:12]` = 2), source width 16 bytes (`[6:4]` = 4). Each new block is linked by rewriting the previous block's `next`; a block flagged "loop" points at itself.
- **Start:** if the channel is idle, write the first block's address to sub-channel 0 (`+0x38`), then set the block's control bits (`+0x00 |= 0x6209`).
- **Position:** `audio_playgetclock` reads the current block (`+0x38`, re-read until stable) and `+0x24`, and maps the block back to its ring entry; `audio_play` also reads `+0x20` and `+0x2C[23:0]`. The play clock is therefore the DMA's progress through the PCM [apple]; that `+0x24`/`+0x2C` hold the bytes remaining is [hypothesis].
- The length unit: the silence block's source is 16 bytes and its length 16, so for this channel the length counts **bytes** [hypothesis], unlike the LCD channel, where it counts units of the narrower width (`dma.md`).

## 4. Pins

| BCM pin | Function 2 when audio is enabled | Evidence |
|---|---|---|
| GPIO 1 | Audio; set back to plain output (function 1) when disabled | [apple] @136A6 |
| GPIO 2, 3 | Audio; left in function 2 when disabled | [apple] @136A6 |

GPIO 3 carries the frame clock (88,198 edges a second = 2·fs at 44.1 kHz) and GPIO 2 the bit clock, both from the WM8758 [silicon S32]; GPIO 1 is the data out on a net of its own [silicon S34, 8a-0b]. On function 2, GPIO 2 and 3 are the block's clock inputs (§5, "Clock direction") [hypothesis, strong]. Function select works as in `interrupts-timers-gpio.md` §5 (2 bits per pin at `0x1000_2800`).

## 5. Wiring on the iPod Video

**[silicon S32] The BCM's GPIO 2 and 3 are on the WM8758's audio bus.** Our image sampled GPIO 1–3 as inputs (stage 8a-0, `HP_OP_PINS`, 631 k samples a second). With the PP's audio stopped there were 0 edges a second on all three. While the PP played a soundtrack, GPIO 1 showed 0 edges a second, GPIO 2 284,726 and GPIO 3 88,198. 88 k edges a second is 2·fs at 44.1 kHz: a frame clock (LRCK), or data. GPIO 2 moves far faster than we sample; a 64·fs bit clock (2.8 MHz) aliases, so it reads as a clock. GPIO 1 stays idle. So the BCM's clock pins sit in parallel on the codec's I²S clocks [silicon]. GPIO 1 is most likely the BCM's data on its own net, not the PP's data line [hypothesis, medium–strong]: Apple drives GPIO 1 as a plain output whenever audio is off, which would fight the PP during every song if the two shared a wire (next subsection). Stage 8a-0b's pull test settles it before anything drives the pin (`open-questions.md` #39).


Not established. What is known:

- The host side: the WM8758 codec is the I²S clock master (`CLKCTRL_MS`) and the PP5022's I²S is a slave [rockbox `wm8758.c`, `i2s-pp.c`]. The WM8758 has a single digital audio interface.
- The BCM side: three pins switched to an audio function only while video plays [apple].
- [hypothesis] The BCM's three pins are wired to the WM8758's audio interface in parallel with the PP5022's I²S, the BCM taking the WM8758's bit and frame clocks as a slave and driving the data line while the PP's I²S is idle. This would explain why Apple's divider is not exact at 44.1 kHz (it would not be used) and why the BCM never writes codec registers. `osos` does not release the PP's data pin when it sends `audio_enable 1`; GPIO 1 is probably on a net of its own (below).
- The alternative, a TV-out-only path (the line or dock output), is weaker: `audio_enable` is sent for every video, on the LCD as well as on TV.

### Clock direction: the block is a clock slave

| Fact | Evidence |
|---|---|
| Apple's whole audio driver touches the block at six places: `+0x00` (`0x6001` then 0 at start-up @13900, `|= 1` at set-rate @14056, `|= 0x6209` at start @138E0, 0 at close @1397A), `+0x10 = 1` and `+0x14 = 0xA508_0010` (init @140F6), `+0x14 = 0xA508_0000 | div` (@14054). There is no other reference to `0x1000_48xx` in `vmcs.bin`; `+0x08` and `+0x0C` are never written. So no control bit outside these values selects the clock direction, and Apple's block runs in whatever mode these values give | [apple] every `mov rN, #0x100048xx` in `vmcs.bin` 1.3 |
| `audio_enable 1` (@136A6) puts GPIO 1, 2 and 3 on function 2 through the fsel helper @13E66 (`fsel &= ~(3 << 2n); fsel |= f << 2n`); `audio_enable 0` puts only GPIO 1 on function 1 (output) | [apple] |
| `osos` leaves the WM8758 as the I²S clock master around `audio_enable` (it calls nothing on the codec or the PP's I²S there; `audio.md` §5, "The PP's side") | [apple] |
| Session 35 (dev41; GPIO 1–3 on function 2, the PP stopped seconds earlier): bit 23 set within the 65 ms wait, control word `0x0092_0001`, the tone played (a loud screech) | [silicon S35] |
| Session 36c (dev42; GPIO 1 on function 2, GPIO 2 and 3 inputs): bit 23 never set in any of four tries, control word `0x0013_0001` | [silicon S36c] |
| Rockbox's DAC standby turns the WM8758's PLL off 5 s after PCM stops (`DAC_IDLE_MS`), which stops BCLK and LRCK (`wm8758.c` `dac_standby_enter` step 5). Stage 8a-0's "quiet" probe, after a stop and long idle, saw 0 edges on GPIO 2 and 3 | [rockbox] ours; [silicon S32, S35] |
| Our image never asks for Apple's clock-manager clocks (ids 2 and 3, @13C32), so the block has no clock of its own in our tests | [ours] |
| **So the block takes its bit and frame clocks from GPIO 2 and 3 (function 2 makes them the block's clock inputs), and bit 23 means it sees them: a clock slave to the WM8758, as Apple's wiring needs.** In S35 the codec was still awake, so the clocks ran; in S36c they never reached the block | [hypothesis, strong]: no master/slave bit exists in Apple's writes, ready followed the pin mux, and we gave the block no clock of its own. The control-word difference (bit 16 set and bit 23 clear without the pins; bit 23 set and bit 16 clear with them) fits a clock-present status pair |
| Session 35's screech was then the format or the level (that build's tone was not yet capped at −40 dBFS), not two drivers on the clock lines | [hypothesis] |

How we drive it (dev45, `bcmfw/hostproto.c` `cmd_tone`) [ours]: GPIO 1–3 as inputs, then count edges on GPIO 2 and 3 for 3 ms. Only if GPIO 3 shows the codec's frame clock does the image put GPIO 2 and 3 on function 2, run Apple's start-up and set-rate, and wait for bit 23. Only after bit 23 does GPIO 1 (the data, which floats: 8a-0b) go to function 2 and the DMA start. The host keeps the codec awake for the probe (`dac_standby_allow(false)`) so its clocks run. The reply carries the control word before, after the rate and while playing, `GPIO_FSEL0`, and the edge counts. The start bits are an optional argument, limited to subsets of Apple's `0x6209`.

### The PP's side, and whether its data pin must be released

Traced in `osos` 1.3 and Rockbox; details in `docs/research/bcm2722/audio-spike.md` §8.

| Fact | Evidence |
|---|---|
| No register is known that makes the PP5022's I²S data-out pin an input or high-Z. Rockbox and `osos` only start or stop the transmitter (`IISCONFIG` bit 29 `TXFIFOEN`, bit 1 `IRQTX`) and enable or reset the block (`DEV_EN`/`DEV_RS` bit 11) | [rockbox] `pp5020.h`, `i2s-pp.c`, `pcm-pp.c`; [apple] I²S driver at `0x149D_D48C`, methods `0x1018_BD70`–`0x1018_C85C` |
| Rockbox's playback stop leaves `TXFIFOEN` set: only the DMA stops, so the transmitter keeps clocking out an empty FIFO whenever the WM8758's clocks run | [rockbox] `pcm-pp.c` `dma_tx_stop()`, `sink_dma_init()` |
| Pin-group bits: both firmwares clear `DEV_INIT1` bits 24–25 and `DEV_INIT2` bit 8 at I²S init ("normal outputs"; Rockbox also clears bit 9, its "CDI" group). Nothing in `osos` ever sets `DEV_INIT2` bit 8 or 9. What setting bit 8 does to the pins is unknown | [rockbox] `wmcodec-pp.c` 51–55; [apple] `0x100C_8748`, every `DEV_INIT2` writer in the image |
| Apple's WM8758 driver can make either chip the clock master: WM8758 R6 bit 0 and the PP's `IISCONFIG` bit 25 are always set opposite | [apple] `0x1015_BFF0`, `0x1015_D614` |
| `osos` calls nothing on the PP's I²S, pin groups or GPIO around `audio_enable 1` or `0` | [apple] `0x101F_8E84`, `0x101F_91BC` (an indirect route through the player manager is not ruled out) |
| Apple parks GPIO 1 as a driven output after every video (`audio_enable 0`), while the PP goes on playing music through the same codec | [apple] @136C2 |
| GPIO 1 shows no edges while the PP plays; GPIO 3 toggles at 2·fs; GPIO 2 is faster than the sampling resolves | [silicon S32] |
| So GPIO 1 is the BCM's data output **on a net of its own**, and GPIO 2 and 3 are bit- and frame-clock inputs. The two data sources meet outside both chips: a selector, a gate, or the PP's I²S input. The PP's data pin need not be released | [hypothesis, medium–strong] |

*Settle on silicon* (nothing driven): GPIO 1 as an input, pull-up then pull-down (`+0x60`–`+0x68`), with the PP's audio off, playing, and stopped with its clocks running. A floating net follows the pull in all three; then driving GPIO 1 cannot fight the PP (`open-questions.md` #35, #39).

*Settle on silicon* (no output driven): with GPIO 1–3 as inputs, sample the level register (`+0x20`) in a tight core loop while the host plays a tone. A frame clock reads at fs, a bit clock at 32–64 × fs, data as noise; nothing toggling means the pins do not see the host's I²S bus. Pulls (`+0x60`–`+0x68`) set opposite ways while the host's I²S is stopped show whether the data line floats (`docs/research/bcm2722/audio-spike.md`, stage 8a).

## 6. Using it from your own image (proposed, not yet run)

```
; listen first: GPIO 1-3 inputs (fsel bits [7:2] = 0), sample 0x10002820
; then, only if the data line floats with the host's I2S stopped:
0x10002800 fsel: GPIO 1, 2, 3 = 2                      ; Apple's audio_enable 1
0x10004800 = 0x6001 ; +0x10 = 1 ; +0x14 = 0xA5080010 ; +0x00 = 0   ; Apple's start-up
+0x14 = 0xA5080000 | div ; +0x00 |= 1 ; wait +0x00 bit 23 (<= 65 ms)   ; set rate
ch5 +0x00 = 0x06076002 ; build {silence-self-loop} -> {PCM blocks} chain
ch5 +0x38 = &first block ; 0x10004800 |= 0x6209                         ; start
stop: 0x10004800 = 0 ; ch5 +0x00 = 0 ; GPIO 1 = function 1
```

If bit 23 never sets, the block is probably missing a clock (§2, "Clocks"); log `+0x00` and stop.

## 7. Our tests [ours]

- **`HP_OP_PINS` (0x0D, dev39).** Our image samples GPIO levels as inputs in a tight loop and counts edges per pin. Stage 8a-0 uses it on GPIO 1–3 with the PP's audio stopped and then playing, to see whether the WM8758's I²S clocks reach these pins. Nothing is driven. Driving them (the tone, 8a-1) waits for this result and the owner's go, because a second output on a shared data line could fight the PP's.
- **`HP_OP_AUDIO` (0x0E, dev39, `HP_CAP_AUDIO`).** It configures two PCM rings in free SDRAM (`0x340000`–`0x3A0000`). It then passes a block from one to the other at a Q16 gain and returns the core's time. Stage 8b sends a 440 Hz tone through it during playback, reads it back word by word and plays it on the PP's mixer voice channel. This is path B of `audio-spike.md`: it proves the read-back plumbing and measures its cost, but not the chip's audio output.

## 8. Silicon results (S32–S36c)

| Session | Set-up | Result | Evidence |
|---|---|---|---|
| S32–S34 | GPIO 1–3 as inputs, sampled while the PP played | GPIO 3 at 88,198 edges/s (LRCK), GPIO 2 at 257,000–352,000 (BCLK, aliased), GPIO 1 none | [silicon] `bcmtest.log` stage 8a-0 |
| S34, S36c | GPIO 1 pull test with the PP playing, its TX stopped, audio off | GPIO 1 follows the BCM's pulls in all three: it floats, so it is not on the PP's data-out net | [silicon] stage 8a-0b |
| S35 | Apple's sequence: GPIO 1–3 on function 2, start-up, rate, DMA channel 5 looping an SRAM tone; codec awake | Bit 23 set (control word `0x0092_0001`); played 3,000 ms twice (PP stopped, then PP playing). The owner heard a **full-scale screech** in the headphones | [silicon] `session 35 bcmtest.log`; commit `2588e7146a` |
| S36c | dev42: GPIO 1 only on function 2, GPIO 2/3 left as inputs, tone capped at −40 dBFS, four sample layouts | Bit 23 never set (`0x0013_0001`); nothing played | [silicon] `session 36c bcmtest.log` |
| S36c | Stage 8a-0 "playing" probes | 0 edges on GPIO 2/3: the soundtrack had not started (the COP decode switch was on), so the codec's clocks were off; not a wiring change | [silicon]; `../data-paths.md` D6 |

So the BCM's audio data **does reach the WM8758's output** by some route (S35) [silicon]; how GPIO 1 joins the PP's data line is still #39.

**Two readings of the S35 screech, neither proven:**
- *dev42's* (commit `2588e7146a`): the screech came with Apple's mux on GPIO 2/3, which carry the codec's clocks, so mux GPIO 1 only. S36c then showed the block never becomes ready that way.
- *dev45's* (commit `8a0f88f33d`, branch `p14-dev45`): Apple writes no master/slave bit, and ready followed the mux, so function 2 makes GPIO 2/3 the block's **clock inputs** and they must be muxed; the screech came from the uncapped level or the sample format [hypothesis].

Safe either way: never drive GPIO 2/3 as outputs, keep the codec awake while testing (its standby stops the clocks), and cap the level. Clock direction and the dev45 order are in `../data-paths.md` R6 and D6.

