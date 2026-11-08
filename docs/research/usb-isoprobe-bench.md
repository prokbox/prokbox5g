# Bench sheet: USB audio probe (#144 P1)

**What it answers:** can our USB driver stream sound from the iPod to a Mac, the way Apple's own firmware could? The iPod sends a test tone; the Mac records it. About 10 minutes.

**Build:** the developer bench build staged in `out/deploy-usbprobe/` (`BUILD-INFO.txt` there). It is the normal developer build plus this one probe. Installing it is a separate step that needs your go and a fresh backup (the `deploy-ipod` skill); nothing is installed by this sheet.

**Safe by design:** during the test the iPod does not offer its disk to the Mac and does not read or write its own disk. Nothing is saved until the cable is out. If anything freezes, you lose only that run's numbers.

**If it freezes:** unplug the cable, then hold **MENU + SELECT** until it restarts. Apple's firmware stays available as always (hold MENU at start-up).

## Steps

1. **Get to the probe.** Open the developer menu (Settings › About, hold SELECT and LEFT together for 3 seconds), then **Advanced › Labs › USB audio probe**. The top line says *Plug the cable into the Mac*. If music was playing, it stops.
2. **Leave the driver on "baseline"** for the first run. (SELECT changes it, only while unplugged.)
3. **Plug the USB-C cable into the Mac.** Within a few seconds the iPod should show *Connected: record now* and *Host: cfg 1*. The Mac does not show a disk this time; that is expected.
4. **On the Mac,** open QuickTime Player › File › **New Audio Recording**. Click the small arrow beside the red button and pick the input **ProkBox**. Press record and **record 60 seconds**, then stop and save the file (Desktop is fine).
   - On the iPod, *Packets/s* should sit near **1000** while recording, and *Sound* alternates: *1 kHz tone (20 s)*, then *silence (10 s)*, over and over.
   - You should hear (or see on the level meter) a steady beep for 20 s, then 10 s of nothing, repeating.
5. **Unplug the cable.** After a second the top line says *Logged*. Press MENU to leave; the probe switches itself off.
6. **Only if step 3 or 4 failed** (no *ProkBox* input, *Packets/s* stuck at 0, or a freeze): open the probe again, press SELECT until the driver reads **A+B+C**, and repeat steps 3 to 5 once.

## Send back

- The recording(s) from step 4.
- What the iPod screen showed at step 4 (a photo is ideal), and whether anything froze.
- `ipodui.log` from the hidden folder (the `isoprobe` lines), next time the iPod is in disk mode.

## What the results mean (for the developer)

| Seen | Meaning |
|---|---|
| ProkBox input appears, ~1000 packets/s, clean 20 s tone / 10 s silence cycles | Isochronous IN works under our driver at this speed: P2 can start |
| Enumerates but 0 packets/s | The host never opened the stream, or the first prime failed: check `alt1`, `rate`, `refused` |
| Packets flow but the tone has gaps | Missed (micro)frames: compare `txn`, `odd` frame steps and `IRQ max`; try variant B |
| Freeze when recording starts | podbox's hang reproduced: variants A/B/C tell which driver path (research §2.2) |

Log lines (all tagged `isoprobe`): the variant; `id`, `dci`, `dcc` and `eps` (DCCPARAMS bits 4:0, the real endpoint count); `portsc` and `epctrl` (type 1 = isochronous); host requests (`cfg`, `alt1`, `alt0`, `rate`); completions and errors; FRINDEX steps (8 = one millisecond); dTD status counts; ping-pong counts; prime timeouts and resets; the longest interrupt and completion handler in microseconds.
