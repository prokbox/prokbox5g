# Input: clickwheel, buttons, hold switch, click

The iPod Video's five buttons and the touch wheel sit behind a small wheel controller that talks to the PP5022 over a dedicated serial interface (Rockbox calls it the "opto" interface; device enable bit `DEV_OPTO`). The hold switch is a plain GPIO. The click sound is a PWM-driven piezo. [rockbox] `firmware/target/arm/ipod/button-clickwheel.c`, shared with the iPod 4G/Color/mini 2G/nano.

## 1. The wheel interface registers

| Address | Name (ours) | Use | Evidence |
|---|---|---|---|
| `0x7000_C100` | `WHEEL_CTL` | control: Rockbox writes `0xC00A_1F00` at init, `0x400A_1F00` to re-arm after each packet; bits `[30:29]` (`0x6000_0000`) are cleared and set again to resynchronise | [rockbox] `button-clickwheel.c:105, 268–270, 324` |
| `0x7000_C104` | `WHEEL_STAT` | status/control: bit 26 (`0x0400_0000`) = packet available (and written 1 to acknowledge); bits `[27:26]` (`0x0C00_0000`) written 1 when re-arming; init writes `0x0100_0000` | [rockbox] `:106, 116, 323` |
| `0x7000_C120` | `WHEEL_TX` | data to send **to** the controller (commands, §3.1) | [external] siggi `10` (Apple's driver under emulation); not used by Rockbox |
| `0x7000_C140` | `WHEEL_DATA` | the 32-bit packet received | [rockbox] `:57` |

Per the emulator notes, `WHEEL_CTL` bit 31 starts a transmission and bits `[30:29]` arm the receiver; `WHEEL_STAT` bit 31 is "transmit busy" and bits 26/27 are receive-ready flags cleared by writing 1 [external] siggi `10`. The meanings of the remaining control bits (`0x000A_1F00` looks like a clock divider and frame length) are unknown. The block shares the `0x7000_C000` page with the I2C controller and **raises the I2C interrupt** (high source 40); there is no separate wheel IRQ. [rockbox]

## 2. Bring-up [rockbox] `opto_i2c_init`, `:97–107`, `:340–356`

```
DEV_EN  |= DEV_OPTO            (0x0001_0000)
DEV_RS  |= DEV_OPTO; udelay(5); DEV_RS &= ~DEV_OPTO
DEV_INIT1 |= INIT_BUTTONS      (0x0004_0000; needed for hold detection)
0x7000_C100 = 0xC00A_1F00
0x7000_C104 = 0x0100_0000
GPIOA bit 5 (0x20): input  (hold)
GPIOA bit 7 (0x80): input  (headphone detect)
CPU_HI_INT_EN = I2C_MASK (1<<8);  CPU_INT_EN = HI_MASK (1<<30)
```

## 3. Reading a packet [rockbox] `ipod_4g_button_int`, `:116–271, :313–327`

In the interrupt:

1. `CPU_HI_INT_DIS = I2C_MASK`.
2. `udelay(50)`. (iPodLinux used 250 µs; 50 µs is tested on the Video.)
3. If `WHEEL_STAT & 0x0400_0000`: read `WHEEL_DATA`.
4. Re-arm: `WHEEL_STAT |= 0x0C00_0000`; `WHEEL_CTL = 0x400A_1F00`.
5. `CPU_HI_INT_EN = I2C_MASK`.

### Packet format

A button/wheel packet satisfies `(pkt & 0x8000_00FF) == 0x8000_001A`.

| Bits | Meaning |
|---|---|
| `[7:0]` | `0x1A` packet type (button/wheel) |
| 8 (`0x100`) | SELECT (centre) |
| 9 (`0x200`) | RIGHT (next / fast-forward) |
| 10 (`0x400`) | LEFT (previous / rewind) |
| 11 (`0x800`) | PLAY/PAUSE |
| 12 (`0x1000`) | MENU |
| `[22:16]` | wheel position, 0–95 (`0x00`–`0x5F`); clockwise increases |
| 30 (`0x4000_0000`) | finger on the wheel |
| 31 | always 1 in a valid packet |

Buttons are 1 while pressed. Packet type `0x3A` is also seen and ignored by Rockbox (meaning unknown). `0xFFFF_FFFF` is ignored. **Any other value means the link is out of step**; resynchronise: `udelay(2000)`, `WHEEL_CTL &= ~0x6000_0000`, `WHEEL_STAT |= 0x0400_0000`, `WHEEL_CTL |= 0x6000_0000`. [rockbox]

Apple's firmware validates packets with a stricter mask, `0xBC00_00FF` (bits `[29:26]` must also be 0), and treats bit 31 as "hold is off" [external] siggi `10`.

### 3.1 Commands to the controller [external] siggi `07`, `10`

The controller also accepts commands (written to `WHEEL_TX`, not used by Rockbox, which relies on the controller's default streaming):

| Command | Meaning |
|---|---|
| `0x8000_023A` | query buttons; the reply has the same shape, with the buttons in bits `[20:16]`. Apple's ROM sends it three times at a cold boot to read the boot-key chord |
| `0x8001_052A` | turn on reporting (payload in bits `[23:16]`); no reply |
| `0x8000_052A` | turn reporting off |

Apple's driver allows 1,500 µs for a reply, with 5 retries.

### Turning positions into steps [rockbox] (policy, not hardware)

The position is absolute (96 per revolution). Rockbox computes the signed difference from the last position, wrapping at ±48, and emits one scroll step per 4 positions (the nano uses 6). It treats the wheel as released 150 ms after the last touched packet. There is no velocity from the hardware; acceleration is software.

## 4. Power

While the hold switch is on, Rockbox turns the wheel controller off (`DEV_EN &= ~DEV_OPTO`) and, on release, turns it on and repeats the full init (§2). [rockbox] `:458–477`. The controller's own current draw is not known.

## 5. Hold switch and headphone detection

| Signal | Pin | Sense | Evidence |
|---|---|---|---|
| Hold | GPIOA bit 5 (`0x20`) | **low = hold on** | [rockbox] `:340–351` |
| Headphones | GPIOA bit 7 (`0x80`) | high = plugged | [rockbox] `:353–356` |

`DEV_INIT1.INIT_BUTTONS` (`0x7000_0010` bit 18) must be set for the hold input to read correctly [rockbox]. Neither pin is used as an interrupt by Rockbox (polled from the tick).

**Hold as an interrupt (Phase 18-1, [hypothesis] until D2 row iii).** While the tick is stopped in a soft sleep, `power_sleep()` arms GPIOA bit 5 as a level interrupt on the opposite of the switch's current level (the VBUS handler's polarity flip, `usb-fw-pp502x.c` [rockbox]), enabled alone among ports A–D (they share high source 32), and re-arms it at each edge. On Hold-off it runs the full bring-up (§2) and keeps sleeping, so the next press wakes by the wheel's own interrupt. Whether a GPIOA edge wakes the core from `PROC_SLEEP` is untested; the fallback keeps the controller powered for the whole sleep (the Sleep lab's "wheel kept on" row) [ours].

**Packets read with IRQs off (Phase 18-1).** Inside a soft sleep nothing runs `ipod_4g_button_int()`; `power_sleep()` repeats its steps (§3: mask, 50 µs, read if bit 26, re-arm) after each source-40 wake and after each raw PMU read, and wakes only on a valid packet with a button bit newly set (`sleep_wheel_packet()`, host-tested). That the wheel's interrupt wakes the core from `PROC_SLEEP` is [rockbox-derived] for an awake idle (it is how every press arrives today) and [hypothesis] at 24 MHz with the tick stopped (D2).

## 6. Boot-time keys

The boot code reads the buttons through this same interface (the bootloader polls a packet without interrupts). The key combinations Apple's ROM and Rockbox's bootloader honour are in `boot.md` §6.

## 7. The click (piezo) [rockbox] `firmware/target/arm/ipod/piezo.c`

| Item | Value |
|---|---|
| Init | `DEV_INIT1 &= ~0xC`; `DEV_EN |= DEV_PWM` (`0x0002_0000`) |
| Register | `PWM0_CTRL` `0x7000_A000` |
| Start | `PWM0_CTRL = 0x8000_0000 \| (form << 16) \| period` |
| Stop | `PWM0_CTRL = 0` |
| Period | `period ≈ 91225 / f_Hz`; rough measurements in the source: period 10 → 8,547 Hz, 20 → 4,465 Hz, 50 → 1,846 Hz, 100 → 928 Hz |
| Wheel click | period 91, form `0x80`, for 4 ms |
| Beep | period 40, form `0x80`, for 200 ms |

Both Apple's firmware and Rockbox always set bit 23 (`form = 0x80`); Apple's click is `0x8080_0055` for 3 ms, timed with Timer 2 [external] siggi `05`. Note the bit conflict: Rockbox's header gives `DEV_PIEZO` the same value as `DEV_OPTO` (`0x1_0000`); Apple's firmware enables the PWM with bit 17 of `DEV_EN`, which is what Rockbox's piezo driver also sets (`DEV_PWM`), so `DEV_PIEZO` is a misnomer [external] siggi `05`, [rockbox]. `form` is otherwise a byte whose meaning is unknown beyond `0x80` being what Rockbox uses. The PWM clock source is not known; the measured periods imply about 91 kHz per unit [hypothesis: the 24 MHz reference divided by 263]. A haptic motor fitted in place of the piezo (our unit) is driven by the same line [ours].
