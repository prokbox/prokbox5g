# Digital audio over USB on the iPod Video (5G/5.5G)

Research note, 2026-11-05 (branch `research-usbaudio`). Questions: could ProkBox stream digital audio over USB, in which direction, and to what? Is the PP5022's ARC/ChipIdea USB core able to do it, or is it a hardware limit?

Evidence tags as in the hardware manual: **[silicon]** measured on our unit; **[datasheet]**; **[rockbox-derived]** read from Rockbox code or history; **[apple-RE]** static reading of Apple's firmware (no code copied); **[external]** third-party documents; **[hypothesis]** not yet proven.

## Verdict

| Case | What it is | Who is USB host | Auth needed? | Verdict |
|---|---|---|---|---|
| **(a)** iPod as a USB DAC for a computer | UAC1 *sink*: isochronous OUT + feedback IN; the WM8758 plays the stream | Computer | No | **Firmware limitation, could be overcome.** Rockbox's `usb_audio.c` already does this direction. It has never been shown to work on the PP5022 [hypothesis]. Isochronous OUT is a standard part of the ARC device core, but nobody has proven it on this chip, Apple included. |
| **(b)** iPod as a digital source to a computer | UAC1 *source*: isochronous IN; the computer sees an audio input | Computer | No | **Firmware limitation, and the most likely of all to work.** Apple's own 5G firmware ships exactly this descriptor set: isochronous IN, 16-bit stereo, 8 to 48 kHz [apple-RE]. So the silicon has done isochronous IN. |
| **(c)** Digital audio to car head units and docks | UAC1 source, plus iAP over USB HID (Digital Audio lingo 0x0A) | Accessory | The *accessory* proves itself to the iPod. The iPod holds no secret | **Firmware limitation, and lawful as far as we can tell.** Apple's 5G firmware did this for the Wadia 170iTransport, in "extended interface" mode [external]. It needs case (b)'s transport plus an iAP-over-HID stack (RockPod has one for the 6G/7G). It also needs the head unit to accept the 5G's product ID [hypothesis]. |
| **(d)** iPod as USB host driving a USB-C headphone dongle | The iPod is the host; the dongle is a UAC sink | iPod | No | **Hardware-limited on this mod as built.** The ARC core can be a host: Apple's 5G firmware has a USB host stack for the camera connector [apple-RE]. But the USB-C board almost certainly only presents a sink's Rd on CC and supplies no VBUS [hypothesis]. A host stack would also be large new code. The owner's passive adapter is not a USB device at all. |
| **(e)** Analogue line-out on the USB-C port (Audio Adapter Accessory Mode) | No USB: analogue L/R on D+/D− | None | No | **Depends on the mod board, not on firmware.** The makers say audio is wired out over USB-C [external, owner]. If so, the board does the switching itself. Firmware's only part is the WM8758 OUT2 line-out, which **ProkBox currently powers off on USB-C builds**. The owner's no-sound test is therefore inconclusive. |

Short answer: nothing found makes USB digital audio impossible on the 5G. Apple shipped isochronous-IN audio on this exact SoC. The 5G hang that podbox saw (below) is most likely a Rockbox software or driver fault, not silicon [hypothesis]. A small bench module (section 7) settles it.

## Decision (owner, 2026-11-05; #144)

> "We'll present whatever ID we need to, in order to simulate this feature. If we can derive and get that working, lets do it."

- **Go.** USB digital audio is ticket #144. The USB product ID is chosen for accessory interoperability: `05AC:1209` today, the classic's `05AC:1261` if head units need it (`../pp5022-reference/usb.md` §7.1). No Apple keys or code are used; in iAP the accessory authenticates to the iPod. Names and on-screen text still never use Apple marks. Recorded in `../legal.md` §7.1.
- **Phases.** P0 host tests (`tests/usbaudio`: the descriptor builder `firmware/usbstack/uac1_source.h`, the packet sizer, the ARC rules `firmware/target/arm/usb-arc-iso.h`): done. P1 the bench probe of §7 (`usb_isoprobe`, `--usb-isoprobe` builds only; bench sheet `usb-isoprobe-bench.md`): built, not yet run. Later: P2 source to a computer, P3 accessories (iAP over HID), P4 sink, P5 composite with mass storage (`../ROADMAP.md` row 24a).
- **Release placement** is an open owner question (`../release-plan.md` §1.3): after 1.0 unless the bench shows it is cheap.
- **Case (e)**, the analogue USB-C line-out, is #145 and waits for the owner's test under Apple's firmware.

## 1. How Apple's iPods did digital audio over USB

**Mechanism** [external; rockbox-derived].

- On the 30-pin connector, digital audio ran only over the **USB pins**. The accessory (dock, car, DAC) is the **USB host** and the iPod is the device.
- The iPod enumerates with a USB Audio Class 1.0 *streaming source* (isochronous IN) and an **iAP-over-USB-HID** interface.
- Over HID the accessory identifies itself (IdentifyDeviceLingoes, or IDPS on later models) and authenticates. It then uses the **Digital Audio lingo (0x0A)**:
  - the iPod asks for the accessory's sample rates (GetAccSampleRateCaps, 0x0A/0x02);
  - the iPod announces each track's format (TrackNewAudioAttributes);
  - the accessory selects the streaming alternate setting and the sampling frequency.

  Sources: RockPod README and commit `d6a3f4554b` (<https://github.com/nuxcodes/rockpod>), Rockbox `apps/iap/`.
- **Clocking.** The iPod is the source and the host makes the frames (SOF). So the iPod just sends one frame's worth of samples per (micro)frame, for example 44 or 45 samples at 44.1 kHz. The accessory recovers the clock with its own PLL or ASRC. Apple's endpoint declares synchronisation type **none** (below).

**Which models and firmware.**

- The Wadia 170iTransport manual (archive.org mirror of the PDF, "iPod Functionality", pp. 16-17) lists digital out for:
  - nano 1G-3G, classic and touch;
  - **"iPod video"**, which "can only output digital audio while in the 'extended interface' mode". The iPod shows the Wadia logo and the click wheel is locked out.
  
  Stereophile's review (<https://www.stereophile.com/digitalprocessors/1008wad/index.html>, as quoted in search results; the page refused automated fetches) says the same: touch, classic, "iPod with video" and nano 1-3 output a digital signal. [external]
- **Apple's 5G firmware 1.2.1 and 1.3 both contain the UAC1 source descriptors.** They are byte-identical, at `osos.bin` `0x69DFB8` (1.2.1) and `0x6A1FAC` (1.3), from the images in the BCM RE workspace. Decoded [apple-RE]:
  - Audio Control interface 0 (bcdADC 1.00). Its input terminal has type **0x0201 (microphone)** and feeds an output terminal of type **0x0101 (USB streaming)**: the host sees an audio *input*.
  - Audio Streaming interface 1: alt 0 has no endpoint; alt 1 has one endpoint. The format is PCM, Type I, 2 channels, 16-bit, with **nine discrete rates: 8, 11.025, 12, 16, 22.05, 24, 32, 44.1, 48 kHz**.
  - Endpoint descriptor `09 05 81 01 C0 00 01 00 00`: **EP1 IN, isochronous, synchronisation none, wMaxPacketSize 192, bInterval 1**. 192 bytes is 48 kHz × 4 bytes × 1 ms, i.e. full-speed framing.
  - Class-specific endpoint: sampling-frequency control.
  - Strings in both images: `USBAudioTask`, `MaximumSampleRate`, `CIapIncomingProcessThread`.
  - **No isochronous OUT descriptor was found**: Apple never used the 5G as a sink.
- **Authentication.**
  - Firmware 1.2.1 embeds "Apple Root Certificate Authority" and "Apple iPod Accessories Certificate Authority" X.509 names plus OpenSSL's X.509/RSA code. So it can verify an accessory's certificate, i.e. auth 2.0-style [apple-RE].
  - In 1.3 the X.509 code strings remain but those CA names are not found in plain text. Where 1.3 keeps them is open.
  - **The direction matters:** in iAP the *accessory* holds the Apple coprocessor and proves itself to the iPod. The iPod signs nothing. [external: RockPod README "MFi certificate exchange → challenge-response", whose iPod side only verifies; rockbox-derived: `apps/iap`]
- **When it arrived.** We only have images 1.2.1, 1.2.3 and 1.3, so whether 1.1 had it is unknown [hypothesis: it came with 1.2, Sept 2006]. The Wadia shipped in 2008 and asked for the latest firmware.
- **5G-era accessories.** The Wadia is the documented one. Most 5G docks and car kits took analogue line-out from the dock pins, and the manual itself treats the 5G as the limited case. No other 5G USB-digital accessory was confirmed in this research.

## 2. What the hardware needs, and whether the PP5022 has it

### 2.1 The ARC/ChipIdea core

**Isochronous endpoints.**

- The ARC device core (the same family as i.MX "USB-OH" and Linux `chipidea`) supports isochronous endpoints in hardware. Each endpoint has a queue head, and the queue head has a `Mult` field (1-3 packets per (micro)frame for isochronous). Each dTD has a `MultO` override for isochronous IN.
- Linux's `drivers/usb/chipidea/udc.c` [external, <https://github.com/torvalds/linux/blob/master/drivers/usb/chipidea/udc.c>]:
  - sets queue-head Mult = 3 for isochronous IN and the real packet count in each dTD's `MultO`;
  - refuses an isochronous request longer than `mult × maxpacket`;
  - on isochronous OUT, writes the packet count into the queue-head Mult;
  - treats a dTD **transaction error on an isochronous endpoint as a normal missed packet, not a failure**;
  - re-primes when a dTD is still active but `ENDPTSTAT` is clear (commits `47263478251b` "improve error recovery for ISO transfer" 2024, and `05735f0854e1` "make controller hardware endpoint primed" 2021, the ATDTW/prime race).

**On the PP5022.**

- Apple's firmware uses an isochronous IN endpoint on EP1 (above), so isochronous IN works on this silicon [apple-RE, indirect: we have not seen it run].
- Isochronous OUT: no evidence either way [hypothesis: present, as the core is generic].
- **Endpoint count:** Rockbox assumes 3 (EP0 + 2, `USB_NUM_ENDPOINTS` in `pp5020.h`) and never records `DCCPARAMS` [rockbox-derived]. RockPod's "3-EP ARC controller" repeats that assumption. Bits 4:0 of `DCCPARAMS` give the real count. Logging it is step 0 of the bench.
- Max packet: 1024 bytes for high-speed isochronous, 1023 for full speed.
- Microframe scheduling: the device side is host-driven, so the device only needs a primed dTD before the host's IN token.

**Rockbox history** [rockbox-derived].

- `c7517f5662` (Amaury Pouly, 2010-03-12): "experimental support for isochronous transfers in the usb ARC driver. There are several details to tweak however."
- `9ce66e088e` (Dana Conrad, 2022-09-09): USB Audio 1.0 *sink*. It lists ipodvideo, ipod4g, ipodcolor, sansae200 and others as "relevant devices", and warns "on devices with few endpoints, this may not work while HID and/or mass storage is enabled". It adds the fast completion handler, called from the driver's interrupt.
- `7c4293af64` (2025-10-08): asynchronous feedback. Tested hosts: Linux and macOS work, Windows feedback does not.
- `c533222851` (2025-11-02): DSP in the path.
- `a1837d2d80`: playback blocked while active.
- `config.h` turns on `USB_HAS_ISOCHRONOUS` and `USB_ENABLE_AUDIO` for every ARC target, and ipodvideo inherits them.
- No commit, forum post or tracker entry found reports it **working** on any PP502x. The testing named in those commits is on DWC2/X1000 hardware.
- ProkBox compiles it out (`IPODUI_TRIM_USB_CLASSES`).

### 2.2 The podbox hang: software, driver or silicon?

podbox (a local read-only clone) turned USB audio off on 2026-08-31 with `PODBOX_NO_USB_AUDIO` (`firmware/export/config.h`; `docs/podbox/upstream-commit-log.md` "USB audio is off"):

> 5G (ARC): Player hangs; needs a hard reset. Driver init never completes. Host binds a USB audio driver. ... that target has no logf.

**Reading.** The host *bound* the audio driver, so enumeration and SET_CONFIGURATION went through. That points to the moment the host opens the stream (SET_INTERFACE alt 1, then SET_CUR sampling frequency), or to the first isochronous completions. Candidates from our tree's code, all **[hypothesis]**, in rough order of likelihood:

1. **Heavy work in the wrong context.**
   - `usb_audio_init_connection()` runs in the USB thread at SET_CONFIGURATION and calls `audio_stop()` (a blocking `queue_send` to the audio thread), two `core_alloc()`s and `dsp_configure()`.
   - Then `usb_audio_fast_transfer_complete()` runs **`dsp_process()` inside the USB interrupt** for every packet, and calls into the mixer.
   - On a 30 MHz PP with a thread holding the audio DSP, a blocked send or an interrupt that never finishes looks exactly like "hangs; needs a hard reset".
2. **Our driver's error path for isochronous.**
   - `transfer_completed()` treats `DTD_STATUS_TRANSACTION_ERR` as fatal and calls `ep_flush()`, which spins while interrupts are off. On isochronous endpoints that flag is a normal missed (micro)frame (Linux ignores it).
   - Also, `usb_audio`'s handler returns on any error **without re-priming**, so the stream stops dead.
3. **`prime_transfer()` is not isochronous-safe.**
   - It always rebuilds the chain from the start of the endpoint's dTD slice and **flushes a still-primed endpoint first**.
   - It busy-waits with interrupts off for up to 2 ms × 3 attempts (`PRIME_SPIN_US`, `PRIME_ATTEMPTS`).
   - Called from the interrupt at 1,000 packets/s, that can starve the system. It also abandons the in-flight packet. Streaming needs an append-to-running-chain path (the ATDTW tripwire) or a two-dTD ping-pong.
4. **`usb_drv_ep_deinit()` writes endpoint type 0 (control) into the direction it disables.** The ARC manual's caution, quoted in our own `usb_drv_init()`, says an unused direction left as control makes data-PID tracking on the other direction undefined. This matters for a single-direction isochronous endpoint (RockPod's source-only EP1 IN) and on reconfiguration.
5. Max packet: `init_endpoint()` sets 1024 for isochronous at **both** speeds, but full-speed isochronous allows only 1023. This only matters on a full-speed link, and our unit has shown `hs=0` sessions (`usb.md` §5).

**Verdict on the hang: most likely software or driver, not a silicon limit** [hypothesis, strong]. Reasons:
- Apple drove isochronous IN on this chip.
- The ChipIdea core is used for isochronous audio on i.MX under Linux.
- Each candidate above is visible in the code.

The bench in section 7 separates them: a tiny class driver with no audio thread, no allocator and no DSP, before the full `usb_audio`.

### 2.3 CPU, DMA and clocks

- **Bandwidth.** 44.1 kHz/16-bit stereo is 176.4 KB/s. That is 1 % of what we measured on mass storage (7.5-18.5 MB/s, `usb.md` §5) [silicon].
- **Packet rate.** At 1 packet/ms (high speed with bInterval 4, or full speed with bInterval 1), there are 1,000 completions/s. If the handler only swaps a pointer and re-arms a dTD, that is roughly 1-2 % of a 30 MHz CPU [hypothesis].
- **DMA.** The ARC core masters SDRAM itself. It is given physical addresses (`| 0x1000_0000`), and the CPU writes back its cache first (quirks 27, 52). None of the PP's general DMA engines is needed.
- **Where the audio goes in source mode (b, c).** The mixer's output goes to a USB sink instead of the I2S DMA. Rockbox's PCM path is single-core: CPU FIQ (quirk 55). So the USB pull should run on the CPU, fed by the existing PCM buffer. The codec can stay on the COP (Fit-2), and the WM8758 can be put in standby while streaming, which saves power.
  - **Clock drift does not arise for the iPod** if packets are sized by counting host frames, not by the I2S clock: 44,100 samples per 1,000 SOFs exactly, the host's own clock. RockPod's "~44,117 vs 44,100 Hz drift" comes from keeping the I2S DMA as the timebase.
- **Sink mode (a).** The host's SOF clock and the WM8758's I2S clock (the codec is the I2S master, quirk 24) differ. Upstream solves this with an asynchronous feedback endpoint (`bRefresh`, 10.14 / 16.16 rate words). Windows' UAC1 driver ignores the feedback. An alternative is to pull the WM8758 PLL by fractions of a ppm and declare the endpoint adaptive [hypothesis]. The DSP should move out of the interrupt into a thread.

## 3. Authentication and the law

| Case | Who demands auth | Our position |
|---|---|---|
| (a) sink to a computer, (b) source to a computer | Nobody. Class-compliant UAC1; macOS, Linux and Windows load their own drivers | No protocol beyond USB-IF class specs |
| (c) cars and docks | The accessory runs iAP; **the iPod verifies the accessory** (auth 1.0 or 2.0) | We hold no Apple key and need none. To interoperate, we accept the accessory's identification and answer the Digital Audio lingo |

Points for the owner (not legal advice, recorded for `docs/legal.md`):

- Rockbox's `apps/iap/` (GPL) and RockPod show the protocol has been implemented from public reverse engineering. Clean-room interoperability work is the normal route; no Apple secret is copied or circumvented.
- **The USB identity is the delicate part.**
  - We already enumerate as `05AC:1209` (Apple's vendor ID; Rockbox's default), and head units recognise an iPod by it.
  - RockPod presents `05AC:1261`, the classic's ID, which some head units may need for digital audio.
  - Rule 2 forbids Apple marks in names but allows naming the hardware. Product-ID spoofing is an owner decision.
- Whether a given car accepts a 5G ID for digital audio is unknown; many 2008-2012 units listed the 5G for control only [hypothesis].

## 4. Case (d): the iPod as USB host to a headphone dongle

- **Host mode exists.** The ARC core is OTG: `USBMODE` = 3 selects host mode (EHCI-style). Apple's 5G firmware 1.2.1 and 1.3 contain the strings "USB 2.0 EHCI Root Hub Simulation", `USBRoleManager`, `HostOSTask`, a hub driver, `ICAPTPCameraIOTask`/`ICAType4CameraDriver` and "Import Photos". That is the host stack for the camera connector [apple-RE]. Our `docs/hardware-features.md` P-12/X-5 rows only cite [datasheet] so far. (`tests/otg` is the On-The-Go *playlist* test, not USB OTG.)
- **Pins.** The same D+/D− as device mode. On the 30-pin connector there was no ID pin, so the role must have been chosen by the accessory-ID resistor or by iAP [hypothesis].
  - On USB-C, roles come from the **CC pins**. A sink presents Rd (5.1 kΩ to ground); a source or host presents Rp and drives VBUS.
  - A dongle is also a sink (Rd). So **iPod Rd + dongle Rd = no attach**, and neither side powers VBUS.
- **VBUS.** The modded unit's USB-C board feeds VBUS *into* the LTC4066 charger (`usb.md` §4). There is no sign of a 5 V boost or load switch to drive VBUS outwards [hypothesis, needs the board's schematic or a meter]. An active dongle draws roughly 10-100 mA. Feeding it would need a boost converter (3.7 → 5 V), a load switch, Rp on CC, and a GPIO to enable them: a hardware change. A powered OTG Y-adapter would only help if CC were also sorted.
- **Software cost (if the hardware existed).** Rockbox has no USB host stack. We would need:
  - an EHCI-style host driver: async schedule for control, periodic frame list for isochronous, iTDs for a high-speed dongle or siTDs through the core's built-in TT for a full-speed one;
  - enumeration;
  - a UAC1 **and** UAC2 class driver, since many USB-C dongles, Apple's included, are UAC2 [hypothesis];
  - isochronous OUT scheduling: one packet per (micro)frame, with feedback honoured for asynchronous dongles.

  As host the iPod generates SOF, so it owns the clock and the dongle adapts. That is several thousand lines, well beyond the device-side cases.
- **The owner's adapter.** Plugged into the Mac, it does not enumerate at all and shows no CoreAudio output. So it is a **passive** Audio Adapter Accessory Mode adapter (Ra on both CC pins, analogue on D+/D−), not a USB device. It is no use for (d).
  - (d) would need an *active* UAC dongle, such as Apple's USB-C to 3.5 mm adapter, which shows up on a Mac as a USB audio device.
  - To tell them apart: an active dongle appears in System Information › USB and in Audio MIDI Setup; a passive one appears nowhere.
- **It is not a tool for (a)-(c) either.** In those cases the iPod is the USB device and the other end is a computer or a car. The test kit is a USB-C cable to a Mac or Linux PC.

## 5. Case (e): analogue audio on the USB-C port (Audio Adapter Accessory Mode)

How it works [external: USB Type-C specification, "Audio Adapter Accessory Mode"; [hypothesis] for this board]:

- **Detection.** A passive analogue adapter pulls **both CC1 and CC2 to ground through Ra** (≤ 1.2 kΩ). A port that supports the mode sees Ra/Ra and switches the connector over:
  - **D+ → right, D− → left** (pins A6/B6 and A7/B7, joined because the plug can go either way);
  - **SBU1/SBU2 → microphone and ground**, swapped by orientation;
  - optionally VBUS for charge-through.
- **What the mod board must have.** A CC detector (a Type-C port controller, or comparators) plus an **analogue switch** that moves D+/D− from the PP5022's USB PHY to the line-out L/R, and SBU to mic and ground. Since the makers say audio is wired out over USB-C, they most likely routed the old dock line-out through such a switch. The board does it all by itself.
- **What firmware controls.**
  - Only the source signal: **WM8758 OUT2**, the line-out that fed the dock pins, plus its level. The line-out is fixed level on Apple's firmware; Rockbox scales it with volume unless set to fixed.
  - Firmware cannot see the adapter unless the board also routes a detect line to a spare GPIO. That would let the shell react to plug and unplug; without it, we can only leave OUT2 powered when it is wanted.
- **Interaction with USB data.** None while the adapter is in. D+/D− carry audio, a passive adapter supplies no VBUS, so GPIOL bit 4 stays low and the USB stack stays idle. When a USB cable is plugged in instead, CC shows Rp (a host), the switch returns D+/D− to the PHY, and normal USB follows. Line-out running into a USB host's data lines would only happen if the board mis-switched [hypothesis].
- **Why the owner heard nothing under ProkBox.** `apps/main.c` lines 554 and 954, under `HAVE_LINEOUT_POWEROFF` and `IPODUI_TRIM_IAP`:
  `lineout_set(false); /* no dock connector: OUT2 (dock line-out) off */`.
  ProkBox switches OUT2 off on this build because we believed the dock line-out was gone. **The test is inconclusive** until the owner retries under Apple's firmware, which drives OUT2.
- **If Apple's firmware gives sound:**
  - **First fix:** re-enable OUT2 on USB-C builds.
  - Measure the cost: the OUT2 driver and its bias, likely around 1 mA, against the battery budget in `docs/hardware.md`.
  - Ask the board makers, or trace, whether any accessory-detect signal reaches a PP GPIO. With one, OUT2 can be powered only while an adapter is in; without one, decide always-on versus a Settings switch.
  - The `docs/hardware.md` "Port" row now marks the "only audio output is the headphone jack" claim as [hypothesis] pending this test.

## 6. Prior art

- **RockPod** (<https://github.com/nuxcodes/rockpod>):
  - **6G/7G (DesignWare core):** case (c) works with an OPPO HA-2SE and docks. The stack:
    - UAC1 source;
    - iAP over USB HID (IDPS and legacy IdentifyDeviceLingoes);
    - Digital Audio lingo;
    - a 3 MB TX ring;
    - double-buffered isochronous IN;
    - a "PCM DMA inhibit" pull mode.
  - **5G:** branch `ipod5g-mfi-usb`, commit `40b84ef9ec` (2026-03-14, "v6.0-alpha.0"). It ports this to the 5G: config 2 = audio + iAP HID, EP1 isochronous IN, EP2 interrupt IN, and `pcm_dma_start_inhibit` in `pcm-pp.c`. Its release notes say the isochronous path is "**untested on hardware**". The README (master) still says digital audio is "iPod Classic only for now". [external]
- **podbox:** the 5G hangs (section 2.2); the 6G's audio function is never enumerated. Off since 2026-08-31. [external, untrusted clone]
- **Rockbox upstream:** sink only; never confirmed on a PP502x (section 2.1).

## 7. Bench experiment: settle isochronous on our unit

**Goal.** Prove or disprove isochronous IN (then OUT) on the PP5022 with the least code and no risk to storage.

**Build.** A developer-only bench build (never the shipped image), with a tiny class driver `usb_isoprobe`:

- Charge-only USB: **no mass storage, so no disk access during the test**.
- No audio thread, no `core_alloc`, no DSP.
- Two 192-byte buffers, static and uncached, in the bench build only.
- The descriptor shape is Apple's: AC interface (microphone input terminal → USB streaming output terminal); AS interface alt 0 / alt 1; **one isochronous IN endpoint**, 16-bit stereo, 48 kHz only, `wMaxPacketSize` 192, bInterval 1 at full speed or 4 at high speed, synchronisation none.

**Steps** (each skips the rest if it fails):

| # | Step | Pass | Logs (live on screen, counters in RAM) |
|---|---|---|---|
| 0 | Read the core's identity | always | `ID`, `DCIVERSION`, `DCCPARAMS` (endpoint count, bits 4:0), `PORTSC1` speed |
| 1 | Enumerate, alt 0 only | Mac lists an input device "ProkBox"; no hang | SET_CONFIGURATION seen, endpoints initialised, `ENDPTCTRL1` read back (type 1 = isochronous) |
| 2 | Host opens the stream (QuickTime "New Audio Recording" or `sox -t coreaudio`) | SET_INTERFACE alt 1, SET_CUR 48000; IN completions ~1,000/s | completions/s, `FRINDEX` delta between completions (8 microframes at high speed), dTD status bits (TR_ERR, buffer error), prime timeouts, misses and stale primes (existing `HAVE_USB_STATS` counters) |
| 3 | Silence, then a 1 kHz tone from a 48-entry table, 60 s | recording has the tone with no gaps (Audacity spectrum; count discontinuities) | the same counters plus the maximum interrupt time (`USEC_TIMER`) |
| 4 | Driver variants on the same probe | which changes fix a failure | A: TR_ERR on isochronous ignored, no flush; B: two-dTD ping-pong instead of the rebuild-and-prime; C: unused direction set to bulk on deinit |
| 5 | Case (a): upstream `usb_audio` sink in charge-only mode, DSP moved out of the interrupt | Mac plays to "ProkBox" glitch-free for 5 minutes | the upstream counters (buffers filled, frames dropped, feedback value) |
| 6 | Only after 1-5: composite with mass storage | both work together | the endpoint budget from step 0 |

**Recovery.**

- If the controller or the CPU hangs: unplug the cable, then **hold MENU+SELECT** to reset. Nothing is written during a test, so a reset loses only that run's counters.
- The probe writes its counters to `ipodui.log` only **after** the cable is out and the controller has been shut down cleanly.
- The CPU hang watcher (`hang_watch.c`) records a stuck USB thread at the next boot.
- Use a Mac or Linux host: Linux's `usbmon` and `arecord -D hw:N -f S16_LE -r 48000 -c 2` give the clearest view. Windows is not needed.

**Before the device.** The simulator has no USB controller model, so it cannot show silicon behaviour, and we should not build a fake that "passes" (see "Sim refuses unknown silicon"). What can be host-tested first:

1. The descriptor builder, checked byte for byte against an expected blob shaped like Apple's (above).
2. The packet sizer: at 44.1 kHz, 1,000 frames sum to exactly 44,100 samples (44/45 pattern), and at 48 kHz always 48.
3. The dTD/dQH builder rules from Linux: `MultO`, length ≤ mult × max packet, TR_ERR not fatal on isochronous, unused direction never left as control.
4. A completion-handler budget check with the align shim.

These go in `tests/` as a small host harness.

## Sources

- Apple firmware images 5G 1.2.1 and 1.3 (`osos.bin`, BCM RE workspace), static byte and string reading only; offsets above. [apple-RE]
- Wadia 170iTransport owner's manual, "iPod Functionality" (archive.org item `manualsonline-id-084b3f7c-658b-4c41-a1e9-5d7a4563f7f4`). [external]
- Stereophile, Wadia 170iTransport review, <https://www.stereophile.com/digitalprocessors/1008wad/index.html> (via search summary; the page blocks automated fetches). [external]
- RockPod: <https://github.com/nuxcodes/rockpod>, release <https://github.com/nuxcodes/rockpod/releases/tag/v6.0-alpha.0>; commits `40b84ef9ec`, `d6a3f4554b`. [external]
- Linux chipidea UDC: <https://github.com/torvalds/linux/blob/master/drivers/usb/chipidea/udc.c>; commits `47263478251b`, `05735f0854e1`. [external]
- Rockbox: `c7517f5662`, `9ce66e088e`, `7c4293af64`, `c533222851`, `a1837d2d80`, `bbdada7690`, `142e1864ef`; `firmware/usbstack/usb_audio.c`, `firmware/target/arm/usb-drv-arc.c`, `firmware/export/config.h`. [rockbox-derived]
- podbox clone (read only, untrusted): `firmware/export/config.h` around `PODBOX_NO_USB_AUDIO`; `docs/podbox/upstream-commit-log.md`, "USB audio is off". [external]
- USB Type-C Cable and Connector Specification, Audio Adapter Accessory Mode (USB-IF). [external]
