# 6. Scalar instruction set

## How this was recovered, and how far to trust it

VideoCore II has VideoCore IV's semantics in a **different encoding**: VC IV binutils decode VC II code as nonsense (about 17% unknown, no consistent prologues) [apple]. The encoding was recovered from Apple's six `.vll` libraries, whose ELF symbols and relocations fix every instruction's length and position:

- a linear sweep of all 483 functions (98,916 bytes) ends exactly at each symbol's size; every relocation (1,866) falls on the expected field; every call (3,500) and branch (3,772) lands on an instruction start [apple]. Negative controls (deliberately wrong lengths) break this for hundreds of functions;
- disassembly → reassembly is byte-identical for all 28,657 instructions in the libraries and 56,934 traced in `vmcs.bin` and `RenderServer.bin` [apple];
- semantics were checked by running Apple's functions in a simulator against independent models (C library, AES, H.264 bit readers…): 20,576 scalar cases, 0 failures, each semantic choice paired with a control that breaks it [apple];
- device checks of individual instruction kinds [silicon S1–S3, S16].

**Lengths are certain everywhere. Meanings are tagged per row.** Kinds that Apple's code never contains should be treated as unverified even if they decode.

## Instruction lengths and the opcode map

Instructions are 16, 32, 48 or 80 bits, a sequence of little-endian halfwords; the first halfword (hw1 below, written msb first) selects the class [apple].

| hw1 | Length | Class |
|---|---|---|
| `0xxx` | 32 | `addcmpb` (add, compare, branch) |
| `1xxx` | 32 | `b<cc>` / `bl` |
| `20xx`–`21xx` | 80 | vector |
| `22xx`–`23xx` | 48 | vector |
| `24xx`–`27xx` | — | not seen |
| `28xx`–`2Bxx` | 32 | `ld`/`st` rd, (rs + s16) by width |
| `2Cxx` | 32 | `add rd, rs, #s16` |
| `2Dxx` | 32 | gp/sp-relative access; `lea` |
| `2E0x`–`2E7x` | 32 | `cmp`/`mul`/`add`/`mov` rd5, #s16 |
| `2E8x`–`2EBx` | 32 | three-operand ALU |
| `2Fxx` | 48 | 32-bit immediates, 27-bit-offset memory ops and `lea` |
| `3xxx` | 16 | short branches, `s<cc>`, `add sp` |
| `4xxx`–`5xxx` | 16 | sp/pc-relative, system ops, `rts`, table branches |
| `6000`–`6FFF` | 16 | ALU rd, #imm5 |
| `7000`–`77FF` | 16 | `ld`/`st` rd, (--rs) **pre-decrement** |
| `7800`–`7FFF` | 16 | `ld`/`st` rd, (rs)++ |
| `8000`–`BFFF` | 16 | `ld`/`st` rd, (rs [+ rI]) |
| `Cxxx`–`Dxxx` | 16 | ALU rd, rs |
| `E000`–`E0FF` / `F000`–`F0FF` | 16 | `pop` / `push` |
| other `Exxx` / `Fxxx` | 16 | `ld` / `st` rd, (rs + 4·u4) |

## Encodings

Bit strings msb first; `d`, `s`, `a`, `b` register fields, `i`/`u`/`o` immediates. 16-bit forms reach r0–r15; 32/48-bit forms have 5-bit register fields.

| Encoding | Meaning | Evidence |
|---|---|---|
| `0000 cccc aaaa dddd` `mm…` | `addcmpb<cc> rd, ra\|#a, rb\|#u6, target`: rd += a; compare rd with b; branch if cc. m = 00: ra, rb, offset o10; 01: #a (s4), rb, o10; 10: ra, #u6, o8; 11: #a, #u6, o8. Offset in halfwords from the instruction | [apple] high |
| `0001 cccc 0ooo oooo` `o16` | `b<cc>` pc + 2·s23 | [apple] high |
| `0001 oooo 1ooo oooo` `o16` | `bl` pc + 2·s27 | [apple] high |
| `0011 cccc 0ooo oooo` | `b<cc>.s` pc + 2·s7 (cc 14 = always) | [apple] high |
| `0011 cccc 1111 dddd` | `s<cc> rd`: rd = cond ? 1 : 0 | [apple] medium |
| `0011 1111 00ii iiii` | `add sp, #4·s6` | [apple] high |
| `010o oooo ooo0 dddd` | `lea rd, (pc & ~3) + 4·u8` | [apple] high |
| `010o oooo pp01 dddd` | p 0 `ld rd, (sp + 4·u5)`, 1 `st`, 2 `lea rd, sp + 4·u5` | [apple] high |
| `010s ssss 0011 dddd` | system group: s 2 `nop`, 3 `sleep`, 4 `rts`, 7 `bl rd`, 10 `ei`, 11 `di`, 12 `tbb rd`, 13 `tbh rd`, 16 `cmp rd, #0`, 18 `bkpt`, 19 `sys19 rd`, 24 `rti` | [apple]: high for `nop`, `rts`, `tbb`/`tbh`, `rti`, `bl rd`; medium for `ei`, `di`, `sleep`, `cmp #0`; low for `bkpt`; `sys19` reads an unknown value |
| `0110 qqqi iiii dddd` | ALU op 2q (q 0–7: `btst bmask asr lsl lsr add mul mov`) rd, #imm5; signed for `add`/`mov`/`mul` | [apple] high |
| `0111 0www ssss dddd` | `ld/st<w> rd, (--rs)`: rs −= size, then access at rs | [silicon] S1–S3 for `ld`, `ldb`, `stb`; other widths by analogy [hypothesis] |
| `0111 1www ssss dddd` | `ld/st<w> rd, (rs)++` | [apple] high; `ldh` [silicon S2] |
| `10ii iwww ssss dddd` | `ld/st<w> rd, (rs)` if iii = 7, else `(rs + r_iii)` with the index **scaled by the access width** | [apple] high (index scaling: control breaks 852 cases) |
| `110p pppp ssss dddd` | ALU op p: rd = rd op rs (unary ops: rd = op rs) | ALU table |
| `1110 0000 Lbbm mmmm` | `pop rb … rb+m [, pc]` | [apple] high |
| `1111 0000 Lbbm mmmm` | `push rb … rb+m [, lr]` | [apple] high |
| `1110 oooo ssss dddd` (o ≠ 0) | `ld rd, (rs + 4·o)` | [apple] high |
| `1111 oooo ssss dddd` (o ≠ 0) | `st rd, (rs + 4·o)` | [apple] high |
| `0010 1000 ssss dddd` `o16` | o & 3 = 0 `ld`, 2 `st` at rs + (o & ~3); o & 1 = 1 **`ldh`**: a halfword load at rs + (o & ~1), **zero-extended** | [apple] high (`ld`/`st`); [silicon] S17 and 2026-10-25: the halfword form zero-extends (`quirks.md` #16; bench HWLD `H28O`) |
| `0010 1001 ssss dddd` `o16` | o & 1 = 1 **`ldsh`**: a halfword load at rs + (o & ~1), **sign-extended**; o & 1 = 0 `sth` | [apple] medium (`sth`); [silicon] 2026-10-25, the Port write check (session log): COMMIT's F16w, 12 of 12 logged sums fit only with this form sign-extending (`quirks.md` #33; bench HWLD `H29O`). Apple's code was read with the two halfword loads the other way round. **Read directly** [silicon] sitting 2 (session log, HWLD `H29O`: `0x8001`/`0xFFFF` back as `ffff8001`/`ffffffff`) |
| `0010 1010` / `1011 ssss dddd` `o16` | `ldb` / `stb` at rs + s16 | [apple] high |
| `0010 1100 ssss dddd` `i16` | `add rd, rs, #s16` | [apple] high |
| `0010 1101 oooo dddd` `u16` | o 0–3: gp-relative, o 4–7: sp-relative access, o & 3 = width class (0 `ld`/`st`/`ldh` by offset low bits, 1 `ldsh`/`sth`, 2 `ldb`, 3 `stb`); o 12 `lea rd, gp + u16`; o 13 `lea rd, sp + u16`. Offsets unsigned | [apple] high (control: 334 failures with all gp); the halfword loads' signedness as the 32-bit `0x28`/`0x29` forms, by the same class field: `2d4x`/o1 (sp + off) zero-extends, `2d5x`/o1 sign-extends [silicon] sitting 2 (session log, HWLD `HSP0`, `HSP1`; the gp-relative twins are not benched). vc2cc emits no halfword load in this form |
| `0010 1110 0ood dddd` `i16` | o 0 `cmp`, 1 `mul`, 2 `add`, 3 `mov` rd, #s16 | [apple] high |
| `0010 1110 10Xd dddd` `I ooooo bbbbb aaaaa` | rd = ra op rb (I = 0) or #u5 (I = 1). X = 1: ALU op table. X = 0: extended ops 0–3 `mulhd.ss/su/us/uu`, 4–7 `div.ss/su/us/uu`, 8 `adds`, 9 `subs`, 10 `shls`, 11 `clamp16`, 12–15 `addscale5…8` (ra + rb << 5…8) | [apple] high (X = 1), medium (X = 0) |
| `0010 1111 011d dddd` `lo16` `hi16` | `mov rd, #imm32` | [apple] high |
| `0010 1111 001d dddd` `o16` `bbbb b ooo…` | `lea rd, (rb + s27)`; rb = 31 = pc, relative to the instruction's own address | [apple] medium-high |
| `0010 1111 xo0d dddd`… (48-bit memory) | Opcode = hw1 bit 7 : hw2 bits 1:0 → `ld st ldh sth` (bit 7 = 0) / `ldb stb ldsh ldsb` (bit 7 = 1). Offset = s27 with its low two bits replaced by hw1 bit 6 (bit 1) and, when bit 7 = 1, hw1 bit 5 (bit 0). rb = 31: pc-relative from the instruction's address **rounded down to a word** | [apple] high (control: 660 failures); [silicon] S2 pc-relative `ld`/`st`; the halfword loads' signedness (`ldh` op 2, `ldsh` op 6) is Apple's naming only [hypothesis; bench HWLD `H48H`, `H48S`]: vc2cc emits no 48-bit halfword load |

## ALU operations

16-bit `110p pppp`, the 32-bit three-operand form (X = 1), and (even ops only) the `#imm5` forms share this numbering [apple].

| p | Op | Conf. | p | Op | Conf. |
|---|---|---|---|---|---|
| 0 | `btst` (Z set when the bit is clear) | [apple] high | 16 | `cmp` | [apple] high |
| 1 | `cmn` | [apple] medium | 17 | unknown (never used) | — |
| 2 | `bmask` (keep the low n bits) | [apple] high | 18 | `max` (signed) | [apple] medium |
| 3 | `bic` | [apple] high | 19 | `min` (signed) | [apple] medium |
| 4 | `asr` | [apple] high; [silicon] S16 | 20 | `adds2` (rd + rs << 1) | [apple] low |
| 5 | `eor` | [apple] high; [silicon] S2 | 21 | `adds4` | [apple] medium |
| 6 | `lsl` | [apple] high | 22 | `adds8` | [apple] medium |
| 7 | `and` | [apple] high | 23 | `adds16` | [apple] low |
| 8 | `lsr` | [apple] high | 24 | `neg` | [apple] medium |
| 9 | unknown (never used) | — | 25 | `msb` (index of highest set bit) | [apple] medium |
| 10 | `mul` | [apple] high; [silicon] S16 | 26 | `sxtb` | [apple] low-medium |
| 11 | `rsub` (rs − rd) | [apple] medium | 27 | `sxth` | [apple] low-medium |
| 12 | `add` | [apple] high | 28 | `brev` | [apple] low |
| 13 | `sub` | [apple] high | 29 | unknown (never used) | — |
| 14 | `mov` | [apple] high | 30 | `abs` | [apple] medium; [silicon] S2 |
| 15 | `or` | [apple] high | 31 | `not` | [apple] low; [silicon] S2 |

Ops 9, 17, 29 never occur in Apple's code; VideoCore IV's unused `ror`, `bset`, `bclr`, `bchg` are candidates [hypothesis]. No `ror` and no popcount instruction is known; no floating-point instruction is known.

## Conditions and flags

Condition codes are VideoCore IV's: 0 `eq`, 1 `ne`, 2 `cs`, 3 `cc`, 4 `mi`, 5 `pl`, 6 `vs`, 7 `vc`, 8 `hi`, 9 `ls`, 10 `ge`, 11 `lt`, 12 `gt`, 13 `le`, 14 always [apple]. `hi` at both branch lengths is verified [silicon S2].

Flag rules [apple; each with a control that breaks hundreds of cases]:

- **Only `cmp`, `cmn`, `btst` (and `cmp rd, #0`) write flags.** `add`, shifts, `addcmpb` and other ALU ops do not.
- `cmp`'s C is a **borrow**: `cs` = unsigned lower; `hi` = !C && !Z.
- `cmn`'s C is the carry out (used for 64-bit adds: `cmn a, b; add a, b; scs c`).
- `btst` sets Z when the tested bit is **clear**.
- There is no conditional execution of ALU operations (unlike VC IV); branch around them [apple: no such encoding found].

Unexercised edge cases: `bmask` with 0 or ≥ 32, `msb(0)`, shift counts ≥ 32 [hypothesis: VC IV behaviour; avoid].

## Registers

| Register | Role | Evidence |
|---|---|---|
| r0–r5 | Arguments and scratch; result in r0 (64-bit in r0 low, r1 high) | [apple] high |
| r6–r15 | Callee-saved | [apple] high (r6–r23 randomised around 20,576 calls, never clobbered) |
| r16–r23 | Not used by Apple's compiled code; hand-written codecs keep global state there (bitstream cursors) | [apple] |
| r24 | gp (set at reset by `vmcs.bin`; unused by the libraries) | [apple] high |
| r25 | sp, kept 4-byte aligned | [apple] high |
| r26 | lr; `bl` writes the return address | [apple] high |
| r30 | sr: bit 30 = interrupt enable. Flags assumed in VC IV's layout | [apple] medium |
| r31 | pc; reads give the current instruction's address | [apple] high |

### Calling convention

[apple] (verified by calling Apple's functions; compatible with the community VC IV GCC with the right flags):

- Arguments in r0–r5; argument 7 at `[sp]` at the call, 8 at `[sp + 4]`, … Varargs callees spill r0–r5 just below the caller's sp so the arguments form one array.
- Prologue `push r6–rN, lr`; epilogue `pop r6–rN, pc`.
- `char` is unsigned (`ldb` zero-extends).
- Leaf helpers in hand-written code may use private conventions.

### push and pop

- The 16-bit `push`/`pop` byte `L bb mmmmm` names base r0/r6/r16/r24 (bb), count m + 1, and L adds lr (push) / pc (pop). Base r24 with m = 15 means "lr/pc only" (`f0ef` = `push lr`) [apple].
- Within a block the **highest register is stored at the lowest address**, lr on top [apple: Apple's interrupt save of `r0–r5, lr` then `r6–r15` is undone by one `pop r0–r15` only in this order; the other order clobbers 2 of 4,983 interrupted contexts].

## Memory accesses

- Widths: `ld`/`st` 32-bit, `ldh`/`sth` 16-bit, `ldb`/`stb` 8-bit. `ldsb` sign-extends in every form checked on silicon (`(rs)`, `(rs+ri)`, `(rs+off)`) [silicon S4]. `ldsh` sign-extends only in its 16-bit `(rs)` form; the 32-bit `(rs+off)` form zero-extends [silicon S17, `quirks.md` #16]. Signed halfwords at an offset: compute the address, then `ldsh rd, (rs)`.
- Alignment: a misaligned access raises exception 1 (imprecise) after completing from the rounded-down address (`interrupts-timers-gpio.md` §2) [silicon S2, S3]. Keep every access naturally aligned.
- Pre-decrement writes rs back even if the access faults [silicon S2].
- 48-bit pc-relative loads and stores use the instruction address rounded down to a word; `lea` uses it unrounded [apple: 15 of `vmcs.bin`'s pc-relative word accesses sit at addresses ≡ 2 mod 4 and fault otherwise].
- Peripheral registers: 32-bit accesses only (the only width Apple uses) [apple].

## Divide and multiply

`mul` (low 32 bits), `mulhd.ss/su/us/uu` (high 32 bits of the 64-bit product), `div.ss/su/us/uu` (quotient) exist as single instructions [apple: `mulhd.uu` by `0x028F5C28` implements x/100 in `vmcs.bin`; `div.uu` in the AES key schedule]. Signed multiply and divide verified [silicon S16]; `mulhd.ss` [silicon S2]. Divide by zero: unknown.

## Timing (silicon)

All in internal SRAM, cache off [silicon S2–S7]:

| Measurement | Result | Session |
|---|---|---|
| Dependent 16-bit `add` chain | 1 instruction per core cycle, at both 27 and 150 MHz | S7 |
| Two or four independent `add` chains, `add`/`ld` pairs, independent `mul` chains | about 0.53 of the dependent chain's time: about 1.9 instructions per cycle | S6 |
| Every timing kernel, scalar and memory | scales exactly with the core clock (× 5.56 from 27 to 150 MHz) | S7 |
| A 32-bit branch to itself (`addcmpbne r0, #-1, #0, self`) whose instruction sits in the last word of a 16-byte line (address ≡ `0xC` mod 16) | takes twice as long as at any other alignment (12 of 12 cases fit) | S6 |

The doubling of independent work reads as dual issue [hypothesis], consistent with the product brief's "dual-ALU" [external]. Rule for loops: keep a hot 32-bit loop-closing branch out of the last word of a 16-byte line (`.align` the loop); whether the line is 16 or 32 bytes is unknown (every slow case was also ≡ 28 mod 32).

## Verified on silicon

Kinds checked for **correct results** (not merely executed) [silicon]:

| Kind | Session |
|---|---|
| `abs`, `not`, `mulhd.ss`, three-operand `eor`, `addscale6`, `ldh (rs)++`, 48-bit pc-relative `ld` and `st`, `bhi` (16- and 32-bit) | S2, S3 |
| `ld (--rs)`, `ldb (--rs)`, `stb (--rs)` | S2, S3 |
| `max` with an immediate (32-bit form), `asr rd, rs`, 32-bit `bgt`/`blt`/`ble`/`bls`, `ldsb` at `(rs)`, `(rs + ri)`, `(rs + off)` | S3 onward (timing bench scalar checks, 9 of 9) |
| Signed shifts, signed multiply and divide, sign-extending loads, compares, clipping: 16 cases computed from volatile inputs, all correct | S16 |
| The 32-bit `b<cc>` with `cs`/`cc` after `cmp`: `bcmfw/aac/brprobe.S`'s 16 pairs took `0x9DAD_6252`, the model's bits (`cs` exactly when a < b unsigned), at 150 and 27 MHz | S35, S36c |
| 32-bit `stb (rs+off)` at +1 and +6, `sth (rs+off)` at +8 and +14 (offset ≡ 2 mod 4), then `ldb (rs+off)` and `ldh (rs+off)` at +8 (offset ≡ 1 in the encoding's low bits), in SRAM and in SDRAM (`HP_AAC_MEMPROBE`) | S36c |
| 32-bit `ldh rd, (rs+off)` with the offset ≡ 2 mod 4 (the encoding's low bits 3): implicitly, by the H.264 decoder's bit-exact pictures (`h264_mv_pred`, `h264_residual` and `h264_finish_picture` use it). **That was the `0x29` form, which sign-extends** (2026-10-25): those loads read values below `0x8000` | S27 onward (implicit) |
| The 32-bit offset halfword loads with values of `0x8000` and up: `0x28`/o1 zero-extends (`ldh`), `0x29`/o1 sign-extends (`ldsh`), at offsets ≡ 0 and ≡ 2 mod 4; the 16-bit `ldh (rs)` zero-extends `0xFFFF`. From COMMIT's F16w (`jref_f16w`): 12 of 12 logged sums fit (`quirks.md` #33) | 2026-10-25 (session log); S17 for `0x28` |
| **Bench row HWLD, every halfword load form read directly** (`0x8001`, `0xFFFF`, `0xFFFF`, `0x8001` at +0…+6): the 16-bit `ldh (rs)` and `0x28`/o1, the 48-bit op 2 (`rs + off`) and the sp-relative `2d4x`/o1 zero-extend; the 16-bit `ldsh (rs)`, `0x29`/o1, the 48-bit op 6 and `2d5x`/o1 sign-extend. All eight as the corrected model, at 27 and 150 MHz | 2026-10-25 sitting 2 (session log, stage 4 and 4b, B4V 52–59) |
| The control code of the 4–90 KB images run in S4–S16 (compiled from C) behaved as intended wherever its effects were observed | S4–S16 (implicit; not a per-kind check) |

Kinds that occur in Apple's code but have never been checked on silicon: `brev`, `sxth`, `adds16`, `mulhd.su/us`, `div.su/us`, saturating `adds`/`subs`, `shls`, `clamp16`, `addscale5/7`, 48-bit stores at other widths, the gp-relative halfword loads (the 48-bit `ldh`/`ldsh` pair and the sp-relative ones are now read: HWLD, sitting 2), conditions `vs/vc/mi/pl`, and the 32-bit `b<cc>` with conditions `cs`/`cc` after `cmp` (now checked: the table above, S35/S36c). Compiled C emits these last two for unsigned compares (pointer loop bounds) that are not fused into `addcmpb`. The vc2sim self-test never runs them; the video image contains a few (in `hp_poll` and the SPS parser), and our AAC decoder executes them in its FFT and Huffman loops [ours]. **The check** [ours]: the video image's `HP_OP_AAC` `PROBE` (`bcmfw/aac/brprobe.S`, device stage 9) runs `cmp ra, rb` then a far `bcs` and a far `bcc` on 16 pairs (equal, 0 against 1 and all ones, either side of `0x8000_0000`, pointer-like pairs) and reports the taken bits against the model above (`cs` taken exactly when a < b unsigned; expected `0x9DAD_6252`). vc2sim agrees with the model. It passed in S35 and S36c [silicon], at both clocks; the row is in the table above. Kinds that do **not** occur in Apple's code (for example 48-bit pc-relative `ldb`/`ldsb`/`ldsh`/`stb`) should not be emitted until checked.

## A compiler route

The community VideoCore IV GCC (GCC 6.2.1) produces correct VC II code when its assembly output is translated instruction by instruction into VC II encodings [ours]: 1,278 of 1,281 integer GCC torture tests pass in simulation, code is 2.6% larger than VC IV, and the translator reserves r15 and r22 as scratch and leaves r16–r23 to hand-written code. VC IV's conditional ALU ops become branches around single instructions; `ror` and popcount become short sequences; floating point is not supported.
