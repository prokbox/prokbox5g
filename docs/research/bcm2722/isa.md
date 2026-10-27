# VideoCore II (BCM2722) instruction set: recovery notes

Workstream 0 of `approach.md` (issue #60). Goal: an instruction-set description good enough to write our own VideoCore II code. Apple's binaries are only the reference corpus (`artefacts.md`); nothing from them is shipped or loaded.

Tool: `tools/bcmre/vc2dis.py` (table-driven disassembler; every encoding row carries its confidence and evidence, `vc2dis.py --table` prints them).

```sh
tools/bcmre/vc2dis.py --check ~/Git/re/bcm2722/extract/1.3/vc/VideoCore/Library/   # regression numbers
tools/bcmre/vc2dis.py ~/Git/re/.../h264dec.vll h264_readue                          # one function
tools/bcmre/vc2dis.py --trace ~/Git/re/.../Boot/vmcs.bin 0 0x200                       # raw image
tools/bcmre/vc2dis.py --unknown ~/Git/re/.../Library/                                 # what is still undecoded
tools/bcmre/vc2dis.py --roundtrip ~/Git/re/.../Library/ .../Boot/vmcs.bin 0 0x200     # disassemble -> assemble -> compare
echo 'mov r3, #0x3c6ef35f' | tools/bcmre/vc2dis.py --asm 0x1000                        # assembler
```

## Status (2026-10-10)

`--check` over the six 1.3 `.vll` libraries (483 sized functions, 98,916 bytes):

| File | Functions | Consistent | Clean | Bytes in identified insns | Vector bytes | Vector: fields / named / full |
|---|---|---|---|---|---|---|
| aacdec | 51 | 51 | 51 | 99.4% | 0.4% | 100% / 100% / 81.5% |
| h264dec | 214 | 214 | 213 | 96.8% | 32.5% | 100% / 95.3% / 87.4% |
| mpg4dec | 110 | 110 | 110 | 98.3% | 0.2% | 100% / 100% / 100% |
| mplayer | 67 | 67 | 67 | 98.9% | 0.0% | — |
| passthruhandler | 6 | 6 | 6 | 94.7% | 0.0% | — |
| slideshow | 35 | 35 | 35 | 98.7% | 25.2% | 100% / 90.8% / 84.8% |
| **Total** | **483** | **483 (100%)** | **482 (99.8%)** | **97.8%** | **15.4%** | **100% / 95.0% / 87.2%** |

"Bytes in identified insns" excludes switch tables (2.2%). Vector columns, as % of vector bytes: *fields* = every bit of the instruction sits in a decoded field (no raw words); *named* = the operation has a name (by confidence: 80.8% high, 13.7% medium, 0.5% low; before the simulator experiments 64.3% / 16.5% / 13.0%); *full* = named and no open sub-field printed (`x8`, `x15`, `sru0`, raw `bx=`/`x9=`). Before this step the vector ops were length-only (82.4% identified, 390 clean functions).

- **Consistent:** the linear sweep from the symbol start ends exactly at the symbol size, every relocation (1,866) falls on the expected field of a decoded instruction, every call (3,500) lands on a symbol, a static function (push prologue) or a decoded instruction, and every branch (3,772) lands on a decoded instruction start. 1.2.1 and 1.2.3 give the same 483/483.
- **Clean:** consistent, and no instruction is "length only". 97.0% of bytes have a named operation (95.1% before the 48-bit memory and `2dxx` decoding) (no `aluN`/`extN`/`gpN`/`memN`/`vopNNN` placeholders); 2.2% are switch tables.
- The check is not vacuous. Negative controls: making the 32-bit `add` 48 bits long drops consistency to 365/483; the 16-bit ALU form to 32 bits, 230/483; 80-bit vector ops to 48 bits, 405/483.
- **Assembler (step 2):** the table is declarative (syntax plus bit fields per row) and drives both directions. Disassemble → assemble → compare is **byte-identical for all 28,657 instructions** in the 1.3 `.vll` functions and the 56,934 instructions traced in vmcs.bin and RenderServer.bin (0 failures; 1.2.1/1.2.3 likewise), **including all 4,881 vector instructions**, which now go through their decoded fields rather than raw words. So no bit is lost in the text form. The assembler picks the shortest encoding; 445 instructions (0.5%) were emitted longer by Apple's compiler (R11-relocated `lea`, which must stay 48-bit, a few `#imm16` ALU ops and branches), and the disassembler marks those with a `.32`/`.48` suffix so its listings reassemble exactly. (Before the pre-decrement fix, 646: the 16-bit `cmp #imm5` the table wrongly offered made Apple's 32-bit `cmp`s look non-minimal.)
- Scalar code is fully decoded structurally, and so is the vector unit at the field level (section "Vector unit"). The VRF operand mapping is now settled by experiment (section "VRF operands", `sim.md` "Vector experiments"). What remains is (a) naming the rest of the vector ops and a few modifier/type bits, (b) the 6-tap's `bx=38` loader form, and (c) a handful of scalar ALU ops and sub-word widths (table below, "low").

### Device evidence, session 1: `7000`–`77ff` is pre-decrement

Until session 1 the table decoded `7000`–`77ff` as ALU ops 16–22 with a 5-bit immediate (`cmp`/`max`/`adds2`/`adds8 #imm5`), by analogy with `6000`–`6fff`. Apple's code never uses that reading. Its only halfwords in the range are the four pre-decrement byte moves above. The assembler still emitted the 16-bit `cmp rd, #imm5`, so every image we built had 8–13 of them. On the device, the probe's `7050` (meant as `cmp r0, #5`, with r5 = 0x7f) raised an exception. Read as `ld r0, (--r5)`, that's a word load at 0x7b: unaligned. The exception frame's pc was 6 bytes past the faulting instruction.

What this proves: `0111 0www ssss dddd` is a memory access with rs as its base (the twin of the `(rs)++` row), a word access at an unaligned address raises an exception (it also completes from the rounded-down address: sessions 2–3, below), and exceptions push the frame our handler reads. A side effect confirms it: Apple's compiler emitted `cmp rd, #small` in the 32-bit form, which `--roundtrip` used to mark as non-minimal (`.32`). With the fix, those suffixes disappear: 59 → 0 in the 1.3 `.vll` files, 107 → 4 in vmcs.bin and 42 → 3 in RenderServer.bin. `--check` stays at 483/483 in every version, and the roundtrip is still byte-identical.

### Exceptions on silicon (device sessions 2 and 3)

Probe v2 (`bringup.md`, "Probe v2") ran on the owner's unit twice on 2026-10-10 (image fnv `4d981526`), with identical results. Its `P2` lines, decoded by `bcmfw/bench/readlog.py`:

| Check | Instruction | Result on silicon |
|---|---|---|
| 9 | `7050` = `ld r0, (--r5)`, r5 = 0x7f (session 1's fault, replayed) | vector 1; r5 = **0x7b**; r0 = **0x338**, the word at 0x78; saved pc = load + 6 (the two following 16-bit `nop`s ran) |
| 10 | `ld r0, (--r5)`, aligned | pass: rs −= 4, then the load at the new rs |
| 11 | `ldb`/`stb (--rs)` | pass, as modelled |
| 12 | `ld r0, (r1)`, r1 = buffer + 1, then four 32-bit `add`s | vector 1; r0 = **0x0badf00d**, the word at buffer + 0; all four adds ran (r2–r5 = 0x111…0x444); saved pc = load + 18, the first instruction not run |

What this settles:
1. **Misaligned access → exception vector 1** [H] (word at 0x04). Bus-error and undefined-instruction vectors are still VC IV's numbering, unmeasured [L].
2. **A misaligned load still completes** [H]: it reads the word at the address rounded down to the access size and writes rd. It does not rotate or merge bytes.
3. **The exception is imprecise** [H for these two cases; M as a rule]. Later instructions run before it is taken: 4 after a plain `ld (rs)`, 2 after `ld (--rs)`. The saved pc is the **first instruction not run**, so `rti` resumes correctly without any offset. Session 1's "+6" was this, not a fixed offset. A handler can't tell the faulting instruction from the frame alone. The probe records its address separately.
4. **Pre-decrement writes rs back even when the access faults** [H]: r5 went 0x7f → 0x7b.
5. Required checks 0–8 passed in both sessions, so their kinds are now on `tools/bcmre/device_verified.txt`, with `ld (--rs)` (check 10) and `ldb`/`stb (--rs)` (check 11).

Still open: whether misaligned **stores**, halfword accesses at odd addresses, bus errors and undefined instructions are precise, and the other pre-decrement widths (`ldh`, `st`, …), which neither Apple's code nor the probe has run.

vc2sim models this when `Sim.exceptions` is on (`HYP['exc_mis_load_rounds']`, `exc_defer_insns` = 4, `exc_defer_insns_wb` = 2, `exc_pc_offset` = 0; `sim.md`). Its unit row "misaligned load as on silicon" replays checks 9 and 12 and expects the device's values, and `hello_test` checks that the probe's records in vc2sim equal the device's `P2` lines word for word.

vc2cc doesn't emit pre-decrement. Native code that wants it must first put the kind on `tools/bcmre/device_verified.txt` (`compiler.md`, "Conservative mode").

### Raw images: load address found

`vmcs.bin` and `RenderServer.bin` are linked to run at **0x0**, entry **0x200** (the word table at 0x0 holds exception vectors; 0x80–0xff interrupt vectors). Evidence: at 0x200 both set gp then sp (`mov r24, #0x1fc40; mov sp, #0x316a3; and sp, sp, #-4` in vmcs 1.3), then call init; the vector targets (0xb36…0xb4a) are `bkpt; rti` stubs; the interrupt handler at 0xbe2 ends `pop r0-r5; ld lr,(sp); add sp,#4; rti`; and init code writes `0xa5a5000x` to `0x10000400`, the same register and password Rockbox's `bcm_init()` uses. `--trace` from 0x200 and the vectors, plus every push-with-lr that follows a return, reaches 56% of vmcs.bin (35,445 instructions) and 61% of RenderServer.bin with no undecodable code; vmcs has no overlapping decodes, RenderServer has 2 overlaps and 4 unparsed switch tables (to look at). Call targets overwhelmingly start with `push`.

## Main finding: VC IV semantics, VC II encoding

VC II is **not** binary-compatible with VideoCore IV (gate result below), but it is clearly the same architecture re-encoded: addcmpb with VC IV's four operand modes and identical bit fields; b/bl with the same 23/27-bit offset split; push/pop with the same `L bb mmmmm` byte (base r0/r6/r16/r24, `r24+15` meaning "lr/pc only"); the 32-bit 3-operand ALU with an `Rd = Ra op Rb|#u5` form and a second op group that matches VC IV's `mulhd/div/adds/subs/addscale` block; `btst/bmask/msb/brev/adds8` style ops; r24 = gp, r25 = sp, r30 = sr, r31 = pc. The opcode map (which top bits mean what) and the ALU op numbering are VC II's own.

### Opcode map (first halfword)

| hw1 | Length | Class |
|---|---|---|
| `0xxx` | 32 | addcmpb |
| `1xxx` | 32 | b.cc / bl |
| `20xx`–`21xx` | 80 | vector unit |
| `22xx`–`23xx` | 48 | vector unit |
| `28xx`–`2bxx` | 32 | ld/st rd, (rs + simm16) |
| `2cxx` | 32 | add rd, rs, #simm16 |
| `2dxx` | 32 | gp/sp-relative access / lea |
| `2e0x`–`2e7x` | 32 | cmp/mul/add/mov rd5, #simm16 |
| `2e8x`–`2ebx` | 32 | 3-operand ALU |
| `2fxx` | 48 | 32-bit immediates and 27-bit-offset memory/lea |
| `3xxx` | 16 | short branches, scc, `add sp` |
| `4xxx`–`5xxx` | 16 | sp/pc-relative, system ops, rts, switch |
| `6000`–`6fff` | 16 | ALU rd, #imm5 (ops 2q, q = 0–7) |
| `7000`–`77ff` | 16 | ld/st rd, (--rs) |
| `7800`–`7fff` | 16 | ld/st rd, (rs)++ |
| `8000`–`bfff` | 16 | ld/st rd, (rs [+ rI]) |
| `cxxx`–`dxxx` | 16 | ALU rd, rs |
| `e0xx` / `f0xx` | 16 | pop / push |
| `exxx` / `fxxx` | 16 | ld / st rd, (rs + 4·u4) |

### Encoding table

Bit strings are msb first; `d`/`s`/`a`/`b` are register fields, `i`/`u`/`o` immediates. "Conf." is how sure we are of the *meaning*; lengths are high confidence everywhere (see the consistency check).

| Encoding | Meaning | Conf. | Evidence |
|---|---|---|---|
| `0000 cccc aaaa dddd` `mm…` | `addcmpb<cc> rd, ra/#a, rb/#u6, target` (rd += a; compare rd with b; branch). m=00: ra, rb, o10; 01: #a(s4), rb, o10; 10: ra, #u6, o8; 11: #a, #u6, o8. Offset in halfwords from the instruction | high | counted loops (`0cf2 c0fd` = dec r2, loop while >0, four bytes of AES SubWord); null checks; all targets land on instruction starts |
| `0001 cccc 0ooo oooo` `o16` | `b<cc>` target = pc + 2·s23 (o16:o7) | high | same split as VC IV `0x9xxx`; tail branches land on symbols |
| `0001 oooo 1ooo oooo` `o16` | `bl` target = pc + 2·s27 (o4:o16:o7) | high | every R23 (import) site; local targets are symbol starts |
| `0011 cccc 0ooo oooo` | `b<cc>.s` pc + 2·s7 (cc=1110: always) | high | aes_init key-size dispatch (`cmp r1,#192; beq; bgt; cmp #128; bne`), tolower range checks |
| `0011 cccc 1111 dddd` | `s<cc> rd` (rd = cond ? 1 : 0) | medium | mp_fifo_empty: `cmp; 30f0; rts` returns rd == wr |
| `0011 1111 00ii iiii` | `add sp, #4·s6` | high | prologue/epilogue pairs (3f3f/3f01); frame size matches the sp offsets used |
| `010o oooo ooo0 dddd` | `lea rd, (pc & ~3) + 4·u8` | high | string literals after mplayer `codec_name` ("enc", "dec", ".vll") |
| `010o oooo pp01 dddd` | p=0 `ld rd,(sp+4·u5)`, 1 `st`, 2 `lea rd, sp+4·u5` | high | aes_init stores, takes the address of and reloads a stack word around RotWord/SubWord; offsets ≥ 64 only in functions with frames ≥ 64 bytes |
| `010s ssss 0011 dddd` | system/register group: s=2 `nop`, 3 `sleep`, 4 `rts` (d=0), 7 `bl rd`, 10 `ei`, 11 `di`, 12 `tbb rd`, 13 `tbh rd`, 16 `cmp rd,#0`, 18 `bkpt`, 24 `rti` | high: rts, tbb/tbh, nop; high: rti (pops sr then pc; sim boot, hardware.md); medium: bl rd, ei, di, sleep, cmp #0; low: bkpt | rts ends 86+ functions; tables follow tbb/tbh (bounded by the guarding compare); vmcs.bin: nop runs after control-register writes, `mov rN, sr; di` … `mov sr, rN; ei; rts`, `sleep; b self`, interrupt handler ends in rti |
| `0110 qqqi iiii dddd` | ALU op 2q (q = 0–7: btst bmask asr lsl lsr add mul mov), rd, #imm5 (signed for add/mov/mul) | high | `6eIR` = mov (calloc sizes, AES Nk/Nr); `6df0` = add #-1 (ue - 1); `6881` = lsr #8 (RotWord); `6232` = bmask #3 (bit index) |
| `0111 0www ssss dddd` | `ld/st<w> rd, (--rs)`: rs -= access size, then the access at rs | high (`ldb`/`stb`); medium (other widths, by analogy with `(rs)++`) | memmove's backward tail in vmcs.bin and RenderServer.bin, every version (vmcs 1.3 `0x1d208`): `7415 7505` = `ldb r5,(--r1); stb r5,(--r0)` looping on `addcmpbhi r0,#0,r4`, the mirror of the forward tail `7c15 7d05` = `ldb r5,(r1)++; stb r5,(r0)++` at `0x1d1be`; 4 sites per boot image, none in the `.vll` files. Device: see "Device evidence, session 1" and "Exceptions on silicon"; `ld`, `ldb`, `stb` verified on the device (sessions 2–3) |
| `0111 1www ssss dddd` | `ld/st<w> rd, (rs)++` | high | aes_subword: `ldb; ldb (r4+r3); stb (r0)++` loop; AES key schedule walk |
| `10ii iwww ssss dddd` | `ld/st<w> rd, (rs)` if iii=7, else `(rs + r_iii)` | high (iii=7); medium (index; scaling unknown) | mp_fifo_init/size, RotWord; aes_subword `ldb r1,(r4+r3)` = sbox[b]; calcmvpredictor neighbour MVs |
| `110p pppp ssss dddd` | ALU op p: rd = rd op rs | see ALU table | |
| `1110 0000 Lbbm mmmm` | `pop rb–rb+m [, pc]` | high | pairs with push; `e0ef` = pop pc; `e02X; rts` |
| `1111 0000 Lbbm mmmm` | `push rb–rb+m [, lr]`: the highest register at the lowest address, lr on top | high | prologues; order: vmcs.bin's interrupt save (`push r0-r5, lr`, then `push r6-r15`) is undone by one `pop r0-r15` (sim.md, semantics 11) |
| `1110 oooo ssss dddd` | `ld rd, (rs + 4·u4)` (o ≠ 0) | high | struct field reads |
| `1111 oooo ssss dddd` | `st rd, (rs + 4·u4)` (o ≠ 0) | high | Bitstream_Set fills a struct |
| `0010 1000 ssss dddd` `o16` | `ld` (o&3 = 0) / `st` (o&3 = 2) word at rs + (o & ~3); o&1 = 1: `ldsh` at rs + (o & ~1) | high (ld/st), low (ldsh) | word offsets are all ≥ 64 (the 16-bit form covers 0–60); stores follow the computation of rd; the rare odd codes include `2810 0003` (offset 2, rd = rs), so a sub-word load |
| `0010 1001 ssss dddd` `o16` | `ldh` (o&1 = 1) / `sth` (o&1 = 0) at rs + (o & ~1) | medium | AACDecoderInit zeroes halfword fields with sth |
| `0010 1010` / `1011 ssss dddd` `o16` | `ldb` / `stb` at rs + s16 | high | ADTS header parser: `stb r2,(r0+2)`, `(r0+3)`… field by field; bit reader `ldb (r8+1)` |
| `0010 1100 ssss dddd` `i16` | `add rd, rs, #s16` | high | struct member addresses, `+0x200` loop stride, `'A'..'Z' + 0x20` |
| `0010 1101 oooo dddd` `u16` | o 0–3: gp-relative, o 4–7: sp-relative access; o & 3 = width class of the 28/29/2a/2b rows (0: `ld`/`st`/`ldsh` by offset low bits, 1: `ldh`/`sth`, 2: `ldb`, 3: `stb`); o 12: `lea rd, gp + u16`; o 13: `lea rd, sp + u16`; offsets unsigned | high (gp/sp split, sim control: 334 failures with gp); medium (widths) | vmcs.bin interrupt handler loads globals; `codec_string` `lea r4,(sp+3)`; `psnprintf` `ldb (sp+20)`; `AACDecode_Ittiam` sp+0x224 |
| `0010 1110 0ood dddd` `i16` | o: 0 `cmp`, 1 `mul`, 2 `add`, 3 `mov` rd5, #s16 | high | `cmp r1,#192` (AES key bits), `mul r5,#120` (struct stride), `add sp,#-532/#532` (frame), mov |
| `0010 1110 10Xd dddd` `I ooooo bbbbb aaaaa` | rd5 = ra5 op rb5 (I=0) or #u5 (I=1). X=1: ALU op table; X=0: extended ops (0–3 `mulhd.ss/su/us/uu`, 4–7 `div.ss/su/us/uu`, 8 `adds`, 9 `subs`, 10 `shls`, 11 `clamp16`, 12–15 `addscale` <<5…<<8) | high (X=1), medium (X=0) | my_memset: `asr r6,r8,#8; lsr #23; add; asr #9; lsl #9` (signed round to 512); RotWord `lsl r2,r1,#24`; `mulhd.uu` by `0x028f5c28` (= x/100) in vmcs; `div.uu` of i by Nk in the AES key schedule |
| `0010 1111 011d dddd` `lo16` `hi16` | `mov rd5, #imm32` | high | `0x3c6ef35f` (Numerical Recipes LCG) in pns_decode; `0x10000400`, `0xa5a50002` in vmcs |
| `0010 1111 001d dddd` `o16` `bbbb booo oooo oooo` | `lea rd5, (rb5 + s27)`; rb = 31 (pc) in the `.vll` files | medium | every R11 (data address) site; the RELA addend is the absolute target |
| `0010 1111 0o0d dddd`, `0010 1111 1ood dddd` `o16` `bbbb b…` | memory op at rb5 + offset. Opcode = hw0[7] : hw1[1:0] → `ld st ldh sth` / `ldb stb ldsh ldsb`. Offset = s27 with its low two bits replaced by hw0[6] (bit 1) and, if hw0[7], hw0[5] (bit 0); rb = 31 is pc-relative from the instruction's address rounded down to a word (the 48-bit `lea` uses the address itself) | high (sim: 660 failures without the offset bits) | `h264_readue` `ldh (r16-30)`; `predresidual` `ldh (r16+2)`; `vc_readzerosdc1` `ldb/ldsb` at +0/+2; `adts_error_check` `ldsb (r0+4)`; R16/R17/R18 = byte/halfword/word `.sdata` sites |
| `0010 00xx` (80-bit), `0010 001x` (48-bit) | vector unit, see "Vector unit" below | length high; fields high/medium per field | vc_inverseinteger is a run of 80-bit ops ending in rts; consistency drops when the lengths change; field evidence below |

### ALU op numbering (16-bit `110p pppp`, 32-bit 3-operand, imm forms use even ops)

| p | Op | Conf. | p | Op | Conf. |
|---|---|---|---|---|---|
| 0 | btst | medium (readse sign test before `beq; neg`) | 16 | cmp | high |
| 1 | cmn | medium (always `cmn a, b; add a, b; scs c` = 64-bit add carry) | 17 | ? | |
| 2 | bmask | high (bit reader `& 7`) | 18 | max | medium (min/max pair in the MV median; order assumed) |
| 3 | bic | high (frame-buffer bit clear, vmcs `0x10000400 &= ~1`) | 19 | min | medium |
| 4 | asr | high | 20 | adds2 | low |
| 5 | eor | high (AES) | 21 | adds4 | medium (int-array indexing) |
| 6 | lsl | high | 22 | adds8 | medium (bytes·8 + bits in aac_get_processed_bits) |
| 7 | and | high (cbp masks) | 23 | adds16 | low |
| 8 | lsr | high | 24 | neg | medium |
| 9 | ? | | 25 | msb | medium (Exp-Golomb leading zeros) |
| 10 | mul | high (LCG `seed*1664525 + 1013904223`) | 26 | sxtb | low-medium (copies a value just stored with `stb`; sim.md) |
| 11 | rsub | medium (fifo end - start) | 27 | sxth | low-medium (follows `ldh`/`sth`, precedes signed tests) |
| 12 | add | high | 28 | brev | low (`brev; msb; not; bmask #5` = ctz) |
| 13 | sub | high | 29 | ? | |
| 14 | mov | high | 30 | abs | medium (`sub; abs; rsub; abs; addcmpbge` = \|a−b\| ≥ \|c−b\| in mpg4dec) |
| 15 | or | high | 31 | not | low |

Condition codes are VC IV's: 0 eq, 1 ne, 2 cs, 3 cc, 4 mi, 5 pl, 6 vs, 7 vc, 8 hi, 9 ls, 10 ge, 11 lt, 12 gt, 13 le, 14 always (confirmed by the AES key-size dispatch, loop counters and tolower bounds).

## Vector unit (VPU)

VC IV's vector model carries over: 16 lanes, a 2-D vector register file (VRF) addressed in rows of 64 bytes, REP counts, per-lane flags with SETF and IFxx predication, an accumulator and a scalar-result unit. The encodings are VC II's own: hw1 holds only the operation (no REP or scalar-register field), and the operand fields are packed differently. Every bit of every vector instruction in the corpus is now assigned to a field, and the text form reassembles byte-identically.

Disassembler syntax: `vadd.3 d00.10+r1, a200+r0, b02.1.t+r0`. This reads op`.w`, then D (coordinate `.`t5 `+r`), A (11-bit field `+r`), and B (coordinate `.`type `.t` `+r` `++`, or `rN±off`, or `#imm`). The modifiers follow: `inc rep16 setf ifnz clra acc sum r3`. Memory ops look like `vld8 d20, (r3-16)+=r1 rep16` (offsets in bytes). Coordinates are printed as raw field values; their (y, x) meaning, settled by the simulator experiments, is in "VRF operands" below (`.w` = D's direction and width, t5 = A's block bits). Fields with an open meaning print only when they differ from their usual value (`x8`, `x15`, `bx=`, `x9=`).

### Formats

Bit strings msb first. `L` = 1 for 48-bit. op9 = hw1[8:0].

| Halfword | 80-bit ALU (op9 ≥ 0x20) | 48-bit ALU | Memory (op9 < 0x20) |
|---|---|---|---|
| hw1 | `0010 000o oooo oooo` | `0010 001o oooo oooo` | same, op9 < 0x20 |
| hw2 | D: `dddd dddd` coordinate, `sss` +r(sss) (7 = none), `ttttt` t5 = A's block/direction bits | D, same | D coordinate in [15:8]; [7:0]: 80-bit = +r / t5 as for ALU (`.1f` = none); 48-bit = offset bits 14:7 |
| hw3 | `aaaaa` A low · `I` imm flag · `bbbbbbbbbb` B (or imm low 10) | `aaaaa` A · `I` · B10, or `I`=1: `F RRR iiiiii` | `ooooooo` offset (80-bit: signed bytes; 48-bit: low bits of a 15-bit offset in **8-byte units**) · `RRR` REP · `bbb` base r0–r7 · `sss` stride r0–r6 (7 = none) |
| hw4 | `BBBBBB` B ext (or imm high 6) · `AAAAAA` A high · `sss` A +r(sss) · `n` inc (D steps per REP) | — | 80-bit: `.1f a3f` loads: hw4[12] = load one byte further on, hw4[0] = D steps; other forms open |
| hw5 | `h F PPP Cc 0 RRR SS rrr` | — | as ALU |

- **imm16 = hw4[15:10]:hw3[9:0]** when hw3[10] = 1. **High.** The 6-tap luma filter (`vc_interp16xN_hoz/vert`) has exactly `#-5, #20, #20, #-5` (`07fb fe00`, `1414 0200`), with the rounding constant `#16`. `vbitplanes` masks read `#0x00f0`, `#0x0f00`, `#0xf000` (one 4×4 row each, `vc_intra_4x4_*`), `#0x0f0f` and `#0x5555`.
- **B ext (hw4[15:10]) = `S sss t i`.** **High.** If S = 1, B is the scalar register r(sss) plus a signed 12-bit offset in t:i:hw3[9:0] (only 0, −1 and −2 occur). Examples: `vmov d00, r1` … `d0a, r5` in `vc_macroblock_init` (its five arguments). The chroma bilinear taps use r1 = 8−dx, r3 = dx, r0 = 8−dy and r4 = dy (tested). Otherwise B is a vector: +r(sss), `t` = hw4[11] = **16-bit** (`.t`), `i` = B steps per REP (`++`). **High** (tested).
- **B vector coordinate = hw3[9:2]** (d8 layout), **hw3[0] = V** (`.1`). hw3[1] (`.2`/`.3`): in a 48-bit op, "B takes the +r" (below, "The 32-bit accumulator"); in an 80-bit op it appears only on B operands that already name a register (35 sites), meaning still open. **High** (sim control `v_b_v_bit0`: 542 failures swapped). The residual is read as H16 rows (`ba0.t++`, vertical prediction) or V16 columns (`ba0.1.t++`, horizontal prediction, `combinepred`).
- **D = hw2: coordinate d8 = hw2[15:8], +r = hw2[7:5]; D's direction and width come from the op's `.w` suffix** (op9[2:1]: 0 H8, 1 H16, 2 V8, 3 V16). **High** (all vector rows; 430 failures if taken from t5 bit 4 instead). The old reading "t5 bit 4 = V for D" fails `vc_ChromaEdges_edge`: Δ is written as `d40.11`, `d40.02` and `d40` and must be one vector; `vop1a4 d2f.04+r6` (t5 bit 4 clear) writes the p0 *column*. A 16-bit D in x block 1 (`d10`) is the discard destination (338 dummies in acc chains, `setf`, `sum`; never a real 16-bit D in blocks 1/3); an 8-bit `d10` is real (`alpha_blt_block`).
- **t5 = hw2[4:0] belongs to A**: bit 0 x += 32, bit 1 y += 16, bit 2 y += 32 (A's 16×32 block), bit 3: in a **48-bit** op, "A takes the +r" and no block offset (below); in an 80-bit op x += 16 is assumed (only `xbit_expand_remove3`), bit 4 = V (A modes 0/2) or "A steps per REP" (modes 4–7). **High** for bits 0–2 and 4 (843 / 133 failures in the controls).
- **+rN offsets.** **High.** The register holds a VRF offset y·64 + x, **added field by field**: x wraps mod 64 without carrying into y (`vc_ChromaEdges` calls the edge filter with r6 = 32 so that columns 46–49 become 14–17 of the transposed block; 33 failures with a linear add). The 6-tap loops step r0 by −64 per row and start from 32 + 15·64. `vc_inverseinteger` is called with r0 = 0 or 1024 and r1 = 32. The 3-bit fields only reach r0–r6, with 7 meaning none.
- **A = 11 bits hw4[9:4]:hw3[15:11] (80-bit), 5 bits hw3[15:11] (48-bit), +r = hw4[3:1].** Decoding in "VRF operands" below. **High.**
- **hw4[0] (`inc`)** steps **D** per repetition (next row for H, next column for V). A and B have their own step bits. **High** (618 failures with D fixed).
- **48-bit immediate form**: hw3[5:0] = imm6 (unsigned), hw3[8:6] = REP, hw3[9] = SETF. **High** for SETF: every `vbitplanes #imm6` with this bit is followed by `ifnz` ops (tested in the intra rows). **Medium** for REP: same codes as the memory form, and zeroing ops use `rep4`.

### Modifiers (hw5 of 80-bit ops)

| Bits | Field | Conf. | Evidence |
|---|---|---|---|
| [7:5] | REP: 0 none, 1 rep2, 2 rep4, 3 rep8, 4 rep16, 5 rep32, 6 rep64, 7 rep r0 (VC IV's codes) | high | `rep16` in the 16-row loops (`contour_threshold`, `h264_invert` post-scale), `rep8` in `alpha_blt_block`'s 8-row loop, `rep16`/`rep8` for luma/chroma in `h264_combinepred`, `rep4` on the second pass of the 4×4 transform |
| [14] | SETF | high | on `vbitplanes` before every `ifnz`; on the `vsub … alpha/beta` tests in deblocking |
| [13:11] | predication, VC IV codes: none, ifz, ifnz, ifn, ifnn, ifc, ifnc | high (ifnz, ifn), medium (rest) | `vbitplanes #0x000f setf` then stores `ifnz` write 4 pixels per row (`vc_intra_4x4_storeback`). Deblocking narrows the lane set with successive `setf ifn` tests, then filters `ifn`. |
| [10] / [9] | `clra` / `acc` (clear the accumulator first; add into it, result = accumulator) | high | 6-tap: `vmov #16 clra acc`, then `vmul #-5 acc` … and the last tap `vadd … acc` writes the sum. Chroma pairs: `vmul (8-dx) clra acc`, `vmul dx acc`. DDR: `vadd #2 clra acc; vshl #1 acc; vadd acc` = a + 2b + c + 2. |
| [4:3] / [2:0] | scalar-result unit: function / destination r0–r6 (7 = none): 1 `sum`, 2 `min`, 3 `max`, 0 `sru0` (open) | high (sum), medium (min/max) | `vc_intra_4x4_dc`: `vbitplanes #15 setf; vadd … ifnz sum r3; add r3,#2; lsr r3,#2` is DC = (Σ4 + 2) >> 2, and the 8-pixel case uses `+4, >> 3`. `expand_range_16s` reduces its min/max rows with `min r4` / `max r5`. |
| [15] | `x15`: accumulate into the **low half** of the 32-bit accumulator (adds the result's low 16 bits zero-extended, or the signed result with `acc` too; writes the accumulator's low half). Plain `acc` adds to the high half and writes it | high [apple-RE, sim] | section "The 32-bit accumulator" below: eight sites, `jhoz`/`jvert` tested against H.264, Apple's VRF context save/restore |
| [8] | open (`x8`) | — | never set in the corpus |

### The 32-bit accumulator (P19, 2026-10-24)

**Finding** [apple-RE, sim]: each lane's accumulator is **32 bits, a high and a low half**.
- A plain `acc` op adds its result to the high half (bit 16 up) and writes the high half, A >> 16, to D. With the low half at 0, that is the old model, so every earlier kernel is unchanged.
- An `x15` op adds at bit 0 and writes the low half, A & 0xffff. Alone it adds its result's low 16 bits **zero-extended**; with `acc` as well it adds the result **signed**. Carries and borrows pass into the high half.
- `clra` clears both halves.
- The partners are the floored high-half multiplies: ops 18 (`vmulhd`), 19 and 51. So one 16 × 16 → 32 product is `vmul … x15` (its low 16 bits) plus `vmulhd … acc` (its high half), and a chain of such pairs is an exact 32-bit multiply-accumulate.

| Site (Apple, 1.3) | What it computes | What it pins |
|---|---|---|
| `vc_interp16xN_jhoz` @109d00 | H.264 centre half-pel j: the 6-tap over six 6-tap rows (b1 + 16 each), 21 bits. `vshl #6 clra x15` + `vasr #10 acc` for the ±1 taps, `vmul #-320 x15` + `vmulhd #-320 acc`, `vmul #1280 x15` + `vmulhd #1280 acc`: 64·Σ read as A >> 16 = (j1 + 512) >> 10, the +512 coming from the rows' +16 (the taps sum to 32). Then `vclip #255` | zero extension, the carry, floored `vmulhd`, the high-half readout, `vclip`'s lower bound. **Tested**: 60 cases (half high-contrast), all pass |
| `vc_interp16xN_jvert` @109dac | the same j, unclipped, as 16 bits, and the vertical half-pel h (or m) beside it | as above, and a 16-bit D's +r x (below). **Tested**: 60 cases |
| `expand_range_16s` @1015be (slideshow) | y = (x · s) >> 16 + o, s a 16.16 scale from `div.uu`: `vop19 x·s_lo clra x15`, `vmul x·s_hi x15` → D | an `x15` op without `acc` still accumulates; its D is the low half; op 19 reads B unsigned |
| vmcs @153f8 (RenderServer @e298) | a 16.16 ramp, start + lane · step: low halves with `x15` (the fraction written by `vmov … r4 x15`), then `vmulhdsu … acc`, `vmov r4hi acc`, `vmul … acc` → the integer part | the carry out of the low half into the high half |
| vmcs @1d448 / @1d4a8 | the VPU context save and restore: `vmov d00, #0 acc` and `vmov d00, #0 x15` stored; restored by `vmov b00.t clra acc` and `vmov b00.t x15` | the state is exactly two 16-bit halves (a wider one would not survive Apple's own task switch); `x15` alone zero-extends |
| vmcs @10730 | a 32-tap audio FIR: low halves `vmul … x15` (`sru0 r0`), high halves `vmulhd … clra acc x15` / `acc x15 sum r5`, then `(r0 >> 11) + (r5 << 5)` in scalar code | `vmulhd` floors (an exact split); the scalar-result unit reads the whole accumulator (r0 needs the 17-bit low-half sums); `acc x15` sums signed high halves [M] |
| RenderServer @29f4 | a 32-bit planar multiply-accumulate: `vop19 … clra x15`, `vmul … acc x15` → the product's high word; `vadd … setf` on the low words, `vop112` (add with carry) on the high words | `acc x15` adds at bit 0 (either sign rule fits mod 2^16) |
| `vc_distort` @105dae (slideshow) | a 16.16 × 8-bit product: `vmul clra x15` → low word, `vop51 acc`, `vmul acc` → high word | op 51 reads A unsigned |

vc2sim models it (`HYP['v_x15'] = 'lo16'`, `'v_mulhd_round' = 'floor'`, `'v_clip_lo' = 'zero'`), and the self-test runs `jhoz` and `jvert` against H.264 8.4.2.2.1: 120 cases, 0 failures; the 22,163 earlier cases are untouched. Controls: `x15` sign-extended alone fails 105 of 120, `x15` read as plain `acc` 120, a rounded `vmulhd` 45 of 60 (`jhoz`), `vclip` without its lower bound 60 of 60 (`jhoz`). Bench rows X15M, X15S, X15Z, MHDU and VCLP check it on silicon (`bench-p19.md`).

**Confidence:** the `x15` semantics, the 32-bit width, zero extension alone and the floored `vmulhd`: high. The signed add of `acc x15`: medium (no site tells the two apart; X15S does). Ops 19 and 51's signedness: medium. Whether the high half wraps at 16 bits (vc2sim keeps it unbounded): open (Apple's save keeps only 16 bits, so it probably does).

**Two operand facts found on the way:**
- **48-bit ops: t5 bit 3 and B's hw3[1] say where the +r goes** [apple-RE, high]. The 48-bit form has one +r field (hw2[7:5]). In every non-`x` 48-bit op in the corpus that names a register (17, in vmcs.bin, RenderServer.bin and h264dec), t5 bit 3 or hw3[1] is set, and neither is set in any of the 694 that name none. `jhoz`'s pair sums (`vadd.1 d47.09+r0, a05, b20.2`) need both sources at +r0 and D fixed (row 23, read back by the `x15` chain): bit 3 gives the register to A (and adds no x offset), hw3[1] to B, and D takes none. The resampler's `vadd.1 d4f.0a+r0, a00, b4f` (vmcs `0x10724`) adds a third coefficient row at +r0 the same way. vc2sim `HYP['v_r48'] = 'enables'`; the old reading fails all 120 `jhoz`/`jvert` cases. The `x` forms (op9 bit 0, the edge replications) keep the old reading; their rule is open. Bench row R48E.
- **A 16-bit H D's +r x aligns down to 16** [apple-RE, medium; one site]. `jvert` writes h to `d25.01+r0` with r0's x = 2 or 3 (the column of h or m), and its callers read it back at x 32 (`vmulms.0 d20.01, a344` after `vclip … a344`); taken as given, the write would also overwrite j's high bytes. So D behaves like a horizontal B operand (quirks #17b). vc2sim `HYP['v_d16_reg_x_align'] = 16` (60 of 60 `jvert` cases fail without it); no 8-bit D, and none of our own kernels, writes such a D. **It bears on ASHF/ASHG**: if silicon aligns a 16-bit **A** operand's +r x the same way, the jobs' lane shift fails. Bench row D16R.

### Memory operations

`vld8 d20, (r3-16)+=r1 rep16`: the address is r(base) + offset. Each repetition advances the address by r(stride) and steps the VRF destination by one row (H) or column (V). **op9 = kind · w · 0**: kind = op9[4:3] (0 load, 1 store, 2 `vldp`, 3 `vldq`), w = op9[2:1] with the ALU `.w` code (0 H8, 1 H16, 2 V8: a V load puts memory byte i in row y + i of one column). The **48-bit offset counts 8 bytes** (plane weights at `(r4+32)`; chroma deblocking loads columns from `(r0-16)` so that src−2/src−1 land in columns 46/47 at any alignment). `vldp`/`vldq` load the lanes in the first / second 16-byte line of a misaligned access; Apple issues `vldq` exactly when the needed bytes cross a line. All of this is **high** (sim.md "Vector experiments"). The older evidence follows. **Medium/high:** `vc_interp16x16_loadall` loads 21 rows as rep16, then rep4, then rep1, from r3 = src − 2·stride, r4 = r3 + 16·stride and r2 = r4 + 4·stride. The offsets are −2 and +14 (the 6-tap needs x−2 … x+18). In `contour_threshold`, `vld16 (r1)+=r5 rep16` with r5 = 32, then `vst8 (r0)+=r4 rep16` with r4 = 16, and the pointers advance by 512 and 256 per iteration. Store data comes from the D coordinate field (48-bit) or from A (`vstr`, 2170).

| op9 | Name | Conf. | Evidence |
|---|---|---|---|
| 0x00 / 0x02 / 0x04 | vld8 / vld16 / vld8v (V8 column, printed `vld32` before) | high | `alpha_blt_block` 32-byte rows (vld16); 16x16 plane/DC: `vld8v d41` = above row in V column 1 |
| 0x08 / 0x0a / 0x0c | vst8 / vst16 / vst8v | high (vst8/vst16) | `vc_storeback` rows; `h264_clearnzcoeffs` 32 bytes (vst16) |
| 0x10–0x14 / 0x18–0x1c | vldp* / vldq*: first / second 16-byte line of a misaligned load | high (pair), medium (split) | chroma bilinear at every alignment; 6-tap dx = 0 |
| 0x170 | vstr: store A's enabled lanes (bytes); D's t5 gives A's block | high | `vc_intra_4x4_storeback` (4 rows, `ifnz`), `vc_storeback` (chroma, stride/2), tested |
| 80-bit `.1f a3f` (`vldp8 d40.1f, (r0-2)+=r1 rep16, a3f bx=3f inc`) | plain load at base + off7 bytes; hw4[12] (`bx=3f`) = one byte further on; hw4[0] = D steps | high | left neighbours for the 4x4 and 16x16 intra predictions, tested; the `bx=38 D+r0/A+r0` 6-tap loader form is open (it must load from src + 14 into column 48) |
| others (0x01, 0x06, 0x09, 0x0d, 0x11, 0x19, 0x178) | `vmemNN` | — | rare |

### Operations (op9 = op6 · w · x)

op6 = op9[8:3]; w = op9[2:1] is printed as a `.w` suffix and x = op9[0] (48-bit only) as `x`. Neither is the plain element width: `vc_inverseinteger` uses `.3` and its 16-bit post-scale `.1`. Odd op6 is the saturating twin of the even one for 9/8 and 11/10 (deblocking); the same pairing is assumed for 17/16 and 21/20.

| op6 | Name | Conf. | Evidence |
|---|---|---|---|
| 4 | vmov | high | `invzigzag`; rows from scalar arguments; accumulator seed `#16` |
| 8 / 9 | vadd / vadds | high | inverse transform butterflies; deblocking `p0' = clip(p0 + Δ)` |
| 10 / 11 | vsub / vsubs | high | butterflies; `q0' = clip(q0 − Δ)`; alpha/beta tests (`vsub setf ifn`) |
| 12 | vrsub (B − A) | high | 0xffff − alpha (`#-1`) in `alpha_blt_block`, tested; −tc = 0 − tc |
| 14 | vmul | high | 6-tap taps, chroma bilinear weights, with `acc` |
| 16 / 17 | vmulm / vmulms ((A·B) >> 8, saturating) | high | alpha blend; the 6-tap ends with `#8` on (Σ + 16) = Σ >> 5 clipped to 0…255; tested |
| 20 / 21 | vmulhr / vmulhrs ((A·B) / 65536 rounded, ties away from zero (session 6), B unsigned; op 21 does not clamp an 8-bit D (session 5)) | high (tested) | chroma `#1024` = (x + 32) >> 6; half-pel average `#0x8000` = (a + b + 1) >> 1 |
| 24 | vshl | high | DDR/DDL `+ (b << 1)`, tested |
| 26 | vasr | high | transform `d1 >> 1`, `d3 >> 1`; `(x + 32) >> 6` in `h264_invert` |
| 27 | vlsr (logical on A's width) | high | `(a + 2b + c + 2) >> 2` in the diagonal intra predictions, tested |
| 28 / 29 | vmin / vmax | high | deblocking clip `max(Δ, −tc)` then `min(Δ, tc)`; running min/max in `expand_range_16s` |
| 30 | vdist (|a − b|) | high | deblocking \|p0 − q0\|, \|p1 − p0\|, \|q1 − q0\| each followed by the threshold test |
| 37 | vclip: clamp A to 0..B | high [apple-RE, sim] | Clip1 of the centre half-pel j in `jhoz` (tested; min-only fails 60/60); Clip3(0, 51, qp + offset) of the QP index in `vc_calc_alpha_beta`; `#255` in `contour_threshold` |
| 18 | vmulhd: (A · B) >> 16, floored, both signed | high [apple-RE, sim] | the high half of the split products in `jhoz`/`jvert` and the audio FIR at vmcs `0x10730`; tested (a rounded high half fails 45/60) |
| 19 | vmulhdsu: (A · unsigned B) >> 16, floored | medium [apple-RE] | the low word of a 16.16 scale in `expand_range_16s` and the 16.16 ramp at vmcs `0x153f8`, where B is ≥ 0x8000 as often as not |
| 51 | vmulhduu: (unsigned A · unsigned B) >> 16, floored | medium [apple-RE] | the low word of a 16.16 product in `vc_distort` (`0x105db8`); B there is an 8-bit lane index, so B's sign is not seen |
| 44 | vbitplanes | high | 16-bit lane mask to flags (see predication) |
| 52 | vmulhru ((A·B + 0x8000) >> 16) | medium | bS = 4 chroma deblocking `#0x4000` on (2p1 + p0 + q1) = (Σ + 2) >> 2, tested on non-negative A; how it differs from op6 20 is not seen. 53 unnamed |
| 5, 6, 13, 31, 34, 42, 43, 53, 59, 61, 62 | unnamed (`vopNNN`) | — | 59 (`vop1da`/`vop1d8`) computes tc in the chroma normal filter. 34 (`vop112`) is probably **add with carry** (`vaddc`): it always follows a 16-bit `vadd … setf` on the low words of a 32-bit pair (RenderServer `0x2a18`, `vc_distort` `0x105e9e`) [apple-RE, medium]. 42 (`vop150`) reads a table: `vc_calc_alpha_beta` looks up alpha/beta with it right after `vclip`, and in `vc_distort` it follows `x15` adds of offsets [apple-RE, low] |

The multiply family lines up with the scalar `mulhd.ss/.su/.uu`: **op6 bit 0 = B unsigned** (18/19, 20/21) and bit 5 = A unsigned (51). That also explains silicon: `vmulhr` (20) reads B signed and `vmulhrs` (21) unsigned (quirks #20, session 26), and 21 does not clamp (session 5): its "s" is the unsigned B, not saturation.

### VRF operands (settled by experiment, 2026-10-10)

The simulator (`vc2vpu.py`) runs Apple's kernels against their H.264 definitions with this decoding: 1,326 cases, 0 failures, and every choice below has a control that breaks between 1 and 8 kernels (`sim.md`, "Vector experiments"). Each was first found by reading the kernels by hand; the conflicts the previous version of this section listed came from reading t5 as D's type and from taking mode 2's row bits in order.

- **Layout (VC IV's).** VRF of 64 × 64 bytes. Element i, byte k of an H vector at (y, x) is (y, x + i + 16k); of a V vector, (y + i, x + 16k). 8-bit elements read unsigned, 16-bit signed.
- **D/B coordinate d8 = `y5 y4 x5 x4 c3 c2 c1 c0`**, c = y for H and x for V (as before). D's direction and width come from `.w`; B's from hw3[0] (V) and hw4[11] (16-bit).
- **A, 80-bit, A11 = hw4[9:4]:hw3[15:11]**, plus t5's block (x + 32·t5[0] + 16·t5[3], y + 16·t5[1] + 32·t5[2]) and +r(hw4[3:1]):

  | A11 | Operand | Steps per REP |
  |---|---|---|
  | `1 dddd s xxxxx` (modes 4–7) | H8 at (dy = A[9:6], dx = A[4:0]), element i at x + i; s (A[5]) never set | iff t5 bit 4 |
  | `000 00 s xxxxx`, t5 bit 4 | V8 column x = A[4:0] | iff s (A[5]) |
  | `000 0000 0000`, t5 bit 4 clear | H8 at the block origin (in practice: A unused, `vmov`/`vbitplanes`) | no |
  | `01m 00 s 0 xxxx`, t5 bit 4 | V16 column A[3:0] (m = 0) | iff s |
  | `01m yy 00 YY 00`, t5 bit 4 clear | H16 row YY·4 + yy | iff m (mode 3) |

  So `a410` with t5 `.02` is row 16, column 16 (`vc_interp_chr8x8`); `a400 + k` with r0 = (15, 32) is row 15 (6-tap); `a284`/`a2c4` with t5 `.04` are rows 38/39, the sums written by `d86`/`d87` (chroma bS = 4); `a7d0` with `.01` is row 15, column 48, the above row of a 4x4 block; `a300` is row 0 (16-bit) stepping, which is why it "reads D's own location" in in-place loops.
- **A, 48-bit**: 5 bits, the row (H) or column (V) index in D's direction and width, plus the t5 block (`vasr.1 d40.02, a00, #3` reads Δ at row 16; `vrsub.1 d4e.02, a05` reads tc at row 21).
- **H/V aliasing is real and used.** `vc_intra_4x4_diagonal_down_left` writes p[3,3] with a V8 write to column 63 and reads it back as lane 15 of the H row (16, 48); `vc_interp_chr8x8` writes the horizontal pass as V16 columns and reads them as H16 rows. `vc_intra_4x4_diagonal_down_right` loads five single rows as V8 columns (`vldp8v`) so that row 0 reads L3 L2 L1 L0 M.
- Open: hw3[1] of B, t5 bit 3, the A forms the corpus never uses (mode 1, A[5] in modes 4–7, mode 2 V with A[7:6]), 32-bit vectors (no instruction is known to use one) and memory w = 3 (`vmem06`, probably a V16 load).

### Vector stores on silicon (device sessions 4 and 5, 2026-10-11)

The bench's 24 vector checks (`bringup.md`, "Vector diagnostics") gave `52/ffffff`: SUMR, ALUX and COPY pass, every other check left its output at the `0xA5A5A5A5` fill, also on the re-read 2 ms later. vc2sim reproduces all 24 `B4V`/`B4VL` lines **word for word** with exactly two changes to the model, and with neither alone (`bcmfw/tests/bench_test.py`, row "silicon profile"; controls: unsigned imm6 gives CSUM `0x340`, the old store model passes 22 checks):

1. **The 48-bit immediate `imm6` is signed [H].** `vmov.1 d08, a00, #52` wrote `-12` (`0xFFF4`) to every lane: CSUM's r5 (the sum of row 8's low bytes) is `0xF40` = 16 × `0xF4`. `HYP['v_imm6_signed']` (default on). The corpus has two 48-bit immediates above 31 (`vsub.1 … #63 setf` in h264dec, `vop079 … #51` in vmcs); under this reading they are −1 and −13. vc2sim's self-test is unchanged by it (66 rows, 0 failures). The disassembler still prints the raw field.
2. **A 48-bit `vst16` from any D but `d00` does not write at its modelled address [H for d08; M for "any D ≠ d00"].** COP8 (`vld16 d08` then `vst16 d08`, no ALU op) left the fill, while COPY (the same through `d00`) passed. ALU results do reach the VRF: MXSU's `vmax` into row 8 read back through `sum r5` is right, and so is CSUM's constant. So the earlier verdicts "ALU results lost" and "the store after an ALU op is lost" are withdrawn; what fails is the store's source or address encoding. `HYP['v_st48_nonzero'] = 'drop'` is the silicon profile the bench replays; the default stays `'model'` because Apple's corpus uses such stores and the self-test needs them.

**Session 5 settled it: a 48-bit store lays its fields out differently from a load [H for vst16].** The store-locating check (dev4) loaded rows 0, 8, 16 and 32 with tagged halfwords and stored each with the load-style encoding. Every store wrote **row 0's** 32 bytes, at **+0, +256, +2,048 and +4,096 bytes**: `d8 × 32`. So for a `vst16`:

| Field | A load reads it as | A store uses it as |
|---|---|---|
| hw2[15:8] | D (`d8`) | the address offset, in 32-byte units (0..8,160; 0x80 gave +4,096, so not signed 8-bit) |
| hw2[4:0] | offset bits 14:10 | the source's t5 block bits (y + 16, y + 32, x + 32; bit 3 x + 16) [M: Apple's code only] |
| hw3[15:11] | offset bits 6:2 | the source row within its block |
| hw3[10:9] | offset bits 1:0 | unseen (0 in every store we emit) |

This also explains every other session-5 failure. The bench stored its second and later rows as `vst16 d00, (r1+32·m)`: under this layout that stores **row m to `r1 + 0`**, over the first row. ADIR, MULR and MU8R (four rows) ended with row 3 at `out[0]`; CSUM, MXSU, SUBZ and LD8R with row 1 there (LD8R's row 1 was SUBZ's leftover data, hence the identical words); COP8's `vst16 d08` wrote row 0 at `out + 256`, past the buffer. **vc2sim with this layout reproduces dev4's device run word for word**: 23 of 24 `B4V`/`B4VL` records and all four `B4S` records (`HYP['v_st48_nonzero'] = 'swap'`, now the default; the 24th, CSUM, differs only in a VRF row the check never loaded, which holds power-on contents on the chip). Apple's corpus agrees: at vmcs `0x1e6b8` three stores with hw2 `0006/0106/0206` and offsets 0/32/64 store rows 48, 49 and 50 to `r4 + 0/32/64`, right after ALU writes to those rows; at `0x11c94`/`0x11c9a` two `rep16` stores write rows 48–63 x 0 and x 32 to the two halves of each 64-byte line. The self-test (66 rows) still passes under the new default. vc2dis still prints the load-style text; `bench_gen.S` comments each store with the row and offset it really means.

**MHRN: `vmulhrs` into an 8-bit D does not clamp [H].** Lane values −2,550, 10,710 and 8,176 (×2048, rounded, >> 16 = −80, 335, 256) wrote `0xb0`, `0x4f` and `0x00`: the low 8 bits. The model clamped to 0..255. `HYP['v_mulhrs_sat'] = False` (default); vc2sim reproduces MHRN's words exactly with it and fails MHRN alone without it. `vadds` into an 8-bit D does clamp (ADSN passes). What op 21's "s" means is open (signed B, or a 16-bit clamp before the 8-bit write).

**Ties round away from zero (device session 6, 2026-10-11) [H for `vmulhrs`, M for `vmulhr`/`vmulhru`].** dev5's MHRN failed in one lane only: lane 8, input −16, where `vmulhrs #2048` gave −1 (`0xff`) and the model (add `0x8000`, floor) gave 0. −16·2048 = −32,768 is exactly −0.5 after the shift. Lane 4 (+16, exactly +0.5) gave 1, so the rule is not round-half-down or round-half-even: a tie rounds **away from zero** (the product rounds as a magnitude and takes its sign back). The other known lanes (0–7) are not ties and agree with both readings; lanes 9–15 were not in the record and hold no ties. `HYP['v_mulhr_round'] = 'away'` (default; `'up'` is the old reading) applies it to ops 20, 21 and 52; that `vmulhr` and `vmulhru` share the multiplier's rounding is a hypothesis. dev6's bench checks it: MHRN now expects `0xff` in lane 8, and the informational RNDT (`vmulhrs`) and RNDU (`vmulhr`), both into 16 bits, put −16, −48, −80, −2,032, 16, 48, −15 and −17 in lanes 0–7, the lanes the record shows; readlog names the rule silicon follows. CLRQ (`vmulhrs #0x1000`) carries a −8 tie too. A control in `bench_test.py` shows the old reading fails exactly MHRN, CLRQ, RNDT and RNDU.

**What generators must do now.**
- `vpu_asm.mem('vst16', H16(y, x), base, off)` encodes the store with the silicon layout (`st16_fields`): `off` is a multiple of 32 in 0..8,160, rows 0–15 of block 0 directly; other blocks need t5 and are refused unless `unproven=True` (the bench's informational STBK check tests them). No staging through `d00` is needed any more.
- `vst8` stays `d00` at offset 0 only: its layout is not device-checked (Apple's `vst8` at `0x11b2a` has hw3 offset field `0x42`, which fits neither reading cleanly).
- **p14-h264's decoder and display path** (merged with dev5, 2026-10-11): every 48-bit store was already `vst8`/`vst16 d00` at offset 0, so the stores obey the layout. The `vmulhrs` into an 8-bit D that relied on the clamp (the 6-tap MC rounding, the intra Plane rows, and the display conversion's `clip((q + 8) >> 4)`) are now `vmulhr` into 16 bits followed by `vadds` D8 = zero + B16, the clamping form `ADSN` checked. The averages (`vmulhrs #0x8000` of two 8-bit samples, `(s + 2) >> 2` box sums) stay: their results never pass 255. All 14 clips are bit-exact again under the silicon default, and the bench's `CLRQ` check now expects the wrap (`decoder.md`, `pipeline.md`).

## Register model and ABI (hypotheses)

- 32 registers. 16-bit forms reach r0–r15; 32/48-bit forms take 5-bit fields. r24 = gp (set at reset), r25 = sp, r26 = lr (`mov lr, r0`), r30 = sr (`mov rN, sr; di` … `mov sr, rN; ei`), r31 = pc (base of pc-relative 48-bit lea/ld/st; ld/st round it down to a word). [H for sp/gp/pc, M for lr/sr]
- Interrupts: entry pushes sr then the return pc (sr at the lower address) and clears sr bit 30; `rti` pops both. Vectors are words at 4·v, v = 32 + source for interrupts, 0–5 exceptions. [M; 4,983 interrupted contexts restored intact in the simulated vmcs.bin boot, hardware.md] Exceptions push the same frame; a misaligned access is vector 1, imprecise, saved pc = first instruction not run [H, device sessions 2–3, "Exceptions on silicon"].
- Arguments in r0–r5, result in r0; r6–r15 callee-saved (push r6…rN, lr). Arguments beyond six probably go on the stack (`ld r4,(sp+48)` above a 16-byte frame). [M for r0–r5/r6–r15, L for stack arguments]
- `push lr` (`f0ef`) is VC IV's "base r24 + 15" trick, so the push/pop byte is identical to VC IV's. Within the block the highest register is stored lowest, lr on top (settled by the interrupt save/restore, sim.md).
- `sys19 rd` reads a value into rd (vmcs.bin's clock setup tests its low nibble against 4; a chip revision?). [L]
- r16 is used by the H.264 bitstream readers as a cursor (`ldh r0,(r16+0)` peeks, `add r16,#62`, `sub r16,r16,r3`); the units and whether r16 is backed by hardware are open.

## Open questions

1. **Vector unit:** operands, `.w`, t5, B types, memory direction/offsets are settled (see "VRF operands"); hw5[15] (`x15`), ops 18/19/51/37 and the 48-bit +r enables are decoded ("The 32-bit accumulator", silicon pending). Open: hw5[8], sru function 0 outside `x15` chains, hw3[1] of an 80-bit B, t5 bit 3 in 80-bit ops, the unnamed ops (34 add with carry?, 42 a table read?, 43, 59, the `vop1xx` group), the 6-tap `bx=38 D+r0/A+r0` loader form, `vc_calc_strength_load`'s form and the 80-bit stores, the `vadd.2x`/`.0x` edge-replication ops.
2. ALU ops 9, 17, 29 and the min/max order; adds2/adds16 need a confirming example; 26/27 named sxtb/sxth on weak evidence.
3. Sub-word widths: `2dxx` o 8–11, 14, 15 (unseen in the libraries).
4. The bitstream cursor r16 (units, hardware assist), which sets the shape of our own CAVLC reader.
5. RenderServer.bin: 2 overlapping decodes and 4 switch tables the tracer can't size.
6. Index scaling for `(rs + rI)` loads (byte offset or scaled by width).

## Method

0. `CeilLog2` is a good warning about semantics: it computes the bit length of its argument, not JM's ceil(log2(x)), because the compiler folded the caller's `+1` into it (RestOfSliceHeader calls it on `len`, not `len+1`). Check callers before concluding an op is wrong.
1. Lengths first: a linear sweep must end exactly at each `.dynsym` size, relocation sites must sit on the right field (R23 at the `bl`, R18/R17/R16 at the 48-bit memory op, R11 at the halfword after `2f2r`), and every branch must land on an instruction start. The length of each class was fixed by those constraints (and the negative controls above).
2. Semantics from functions whose C is known or obvious: JM H.264 names (`CeilLog2`, `h264_readue/se`, `calcmvpredictor` median), AES (`aes_rotword`, `aes_subword`, `aes_init` key schedule), the AAC ADTS/ADIF bit readers, mplayer's fifo and string helpers, and vmcs.bin's reset code.
3. `--unknown` tallies what is left; attack the most frequent.

## Gate result (2026-10-10): not binary-compatible; we recover the ISA ourselves

- **VC IV tools don't decode VC II.** The VC IV binutils (`puppeh/vc4-toolchain`, built locally to `~/Git/re/vc4-prefix`) disassembles `h264dec.vll` `.text` into nonsense: `bkpt`, `swi`, about 17% `*unknown*` and no consistent prologues. Byte-swapped input is just as bad.
- **The ISA is regular and labelled, so recovering it is tractable.** The `.vll` files keep `.dynsym` and `.rela.text`; `tools/bcmre/vllinfo.py` prints the statistics. Toolchain origin: MetaWare ("MetaWare Universal Linker v5.3h" in `.comment`).

## Next steps

1. Done: VRF operand mapping, by experiment in the simulator (`sim.md`). Remaining vector ops and forms: open question 1, each against its H.264 definition (`jhoz`/`jvert`, the deblocking normal path, the 6-tap loaders).
2. Name the remaining scalar ops and widths (open questions 2–3), each with a corpus example.
3. Our own code: the bring-up image (`BCMCMD_LCD_UPDATE`) in vc2dis assembly, run in vc2sim against modelled LCD/host-port registers; then our decoder against ffmpeg output.
4. C compiler: done (2026-10-10). VC IV GCC's assembly is translated to VC II by `tools/bcmre/vc2cc.py`. Our C tests and 1,278 GCC torture tests pass in vc2sim (`compiler.md`).

## Tooling locations (not in git)
- `~/Git/re/vc4-toolchain`: upstream VC IV toolchain sources (third-party, for reference and as a possible GCC base).
- `~/Git/re/vc4-build`, `~/Git/re/vc4-prefix`: local binutils build. Configure with `--target=vc4-elf --disable-gdb --disable-sim --disable-nls --disable-werror`, then `make MAKEINFO=true all-binutils all-gas all-ld`.
- `~/Git/re/bcm2722`: Apple reference artefacts (`artefacts.md`).
