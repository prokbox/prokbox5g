# C for VideoCore II (`vc2cc`)

Issue #60. **Question:** what does it take to write our VideoCore II firmware's control code, and a portable-C H.264/MPEG-4 baseline, in C? VC II has VC IV semantics with a different encoding (`isa.md`), so the obvious move is to reuse an existing VC IV GCC.

**Answer: C works now.** An unmodified VC IV GCC (puppeh/gcc-vc4, GCC 6.2.1) produces the code. Our `tools/bcmre/vc2cc.py` translates its assembly text into VC II instructions and assembles the whole program into a flat image through `vc2dis`'s encoding table. `vc2sim` then runs it.

- **Spike result:** our own C tests pass in `vc2sim`: 1,787 cases, 0 failures, with an ABI check on every call. So do **1,278 of the 1,281 integer GCC c-torture/execute tests**. The remaining 3 call `sprintf`/`strrchr`, which the harness doesn't stub.
- **Code size:** the VC II code is **+2.6%** larger than the same code in VC IV encoding.
- **Recommendation:** route (b). The remaining work was about **3–5 days**. Milestone 3 (2026-10-10) did the firmware part, "Firmware support" below, and about 1.5–2.5 days remain.

## Routes compared

| Route | What changes | Effort | Verdict |
|---|---|---|---|
| **(a) Retarget VC IV binutils** (gas/ld/objdump), GCC unchanged | `binutils-vc4` is CGEN-based. We'd have to rewrite `cpu/vc4.cpu` (2,526 lines) and `vc4-vector.cpu` (1,313) for VC II's encodings, regenerate `opcodes/` (CGEN needs Guile; it was written for Guile 1.8, and only Guile 3.0.11 is installed, which is untested with this CGEN), redo the relaxation in `tc-vc4.c` and the 28 `R_VC4_*` relocations in `elf32-vc4.c`, and still handle the VC IV instructions VC II lacks (below) with GCC patches or gas macro-insns | 10–15 days, plus the CGEN/Guile risk | Gives standard ELF objects, `ld` and `objdump`. Nothing we need yet requires them. |
| **(b) Translate GCC's assembly text** (done in this spike) | `vc2cc.py` (about 1,000 lines of Python) does instruction translation and expansion, whole-program layout with branch relaxation, `.data`/`.bss`/`.rodata`, switch tables, library pull-in (libgcc) and inline-asm pass-through. There's no object format and no linker, because the whole firmware is assembled at once. | Spike ≈ 1 day; **3–5 days** to production (below) | **Recommended.** It already runs real C correctly, the encoding comes from the same table the disassembler and simulator use, and any ISA correction propagates automatically. |
| (c) LLVM | The toolchain repo has no LLVM backend, and upstream LLVM has no VideoCore target. | ≥ 20 days | No |
| (d) Fork GCC's `config/vc4` into a VC II port | Rewrite the output templates and constraints so GCC emits VC II syntax directly. This moves (b)'s expansion layer into GCC. | 5–8 days | Only if (b)'s limits ever bite (r15 reservation, no ELF). Not now. |

## How (b) works

```
C ──vc4-elf-gcc -O2 -S + flags──▶ VC IV asm ──vc2cc translate──▶ VC II text (vc2dis syntax)
  ──whole-program layout/relaxation, vc2dis.assemble──▶ flat image + symbol map ──▶ vc2sim / device
```

- **GCC flags** (`vc2cc.GCC_FLAGS`): `-ffixed-r15 -ffixed-r16 … -ffixed-r23 -ffixed-lr -ffreestanding -fno-common -fno-builtin`.
  - Generated code uses only r0–r14, so nearly every instruction has a 16-bit VC II form. That's the reason compares never need the unverified 3-operand `cmp` with high registers.
  - r15 and r22 are the translator's scratch registers.
  - r16–r21 stay free for hand-written kernels' global state, as in Apple's MetaWare code (bitstream cursors in r16/r19/r20, `sim.md`). r23 is GCC's static chain (no nested functions).
  - `lr` isn't used as a temporary: GCC still saves it in non-leaf functions (checked).
  - The register restriction **shrinks** the code by 0.3% (380 torture files).
- **Translation** maps each VC IV instruction to a list of alternative VC II sequences. The first one that `vc2dis.assemble` accepts at the current address wins. Sizes only grow between passes, so branch relaxation converges.
- **Layout:** sections in the order `.text`, `.rodata`, `.data`, `.bss` from `--base` (default 0x200000).
  - Static symbols are scoped per input file.
  - Library `.s` files that define a still-undefined symbol are pulled in, with archive semantics. Our case is libgcc, recompiled with the same flags by `tools/bcmre/vc2libgcc.py`.
  - Whatever is still undefined goes to a callback (the `vc2sim` stubs in tests). In firmware it is an error.
- **Inline assembly:** a line starting with `!` passes through as one native VC II instruction, e.g. `asm("! msb %0, %1" : "=r"(n) : "r"(x))`. Vector kernels use this route, or native `.s` input (next steps).

## The spike's evidence

Commands (toolchain at `~/Git/re/vc4-prefix`, see "Building"):

```sh
tools/bcmre/vc2cc_tests.py                 # our C snippets vs Python models, ~1 s
tools/bcmre/vc2cc_tests.py --torture ~/Git/re/vc4-toolchain/gcc-vc4/gcc/testsuite/gcc.c-torture/execute
tools/bcmre/vc2cc.py file.c --run FUNC 1 2 3      # build at 0x200000 and call in vc2sim
tools/bcmre/vc2cc.py file.c --list                # VC II listing (vc2dis)
tools/bcmre/vc2cc.py --stats *.s                  # translation gaps over VC IV .s files
```

**`vc2cc_tests.py`** compiles one C file of our own (`SRC` in the test) and checks every function against a Python model on random inputs. Each call runs with `check_abi`: r6–r23 are randomised and must come back intact, apart from the scratch registers r15 and r22.

| Row | Cases | Pass |
|---|---|---|
| memcpy loop (post-increment `ldb`/`stb`, `addcmpb`) | 60 | 60 |
| switch: `tbb` jump table with code per case; `.rodata` value table | 71 | 71 |
| 64-bit: `mul`/`mulhd.uu` 64×64, 32×32→64 signed and unsigned, libgcc `__divdi3`/`__umoddi3`/shifts, compare, `cmn` carry add | 360 | 360 |
| 9 arguments: 3 on the stack, called from C and from the simulator | 120 | 120 |
| struct copy (36-byte `*d = *s`) and struct by value | 80 | 80 |
| CAVLC-style bit reader: 32-bit cache, `clz` Exp-Golomb, se(v) streams up to 40 codes | 40 | 40 |
| `.data`/`.bss`/string literal, recursion (`fib`), function pointers (insertion sort with callbacks) | 56 | 56 |
| conditional execution, `short`/`signed char`/`unsigned short` indexed loads, rotates, bit set/clear/flip, sign extension, `clz`/`ctz`/popcount, divide, bswap, 32-bit constants | 840 | 840 |
| varargs (8 variadic arguments, from C and from the simulator), struct return, inline VC II asm | 160 | 160 |
| **Total** | **1,787** | **1,787** (111,958 instructions simulated) |

**GCC's own c-torture/execute suite** has 1,389 tests, each compiled at `-O2` through vc2cc and run from `main` in vc2sim (`abort()` fails, `exit(0)` or a return of 0 passes, `dg-options` honoured):

| Result | Tests |
|---|---|
| Pass | **1,278** |
| Fail (abort or nonzero exit) | **0** |
| Stopped in the simulator: library function without a harness stub (`sprintf` ×2, `strrchr`) | 3 |
| Not translatable: VC IV hardware single-float instructions | 31 |
| Not built: need libc headers (no newlib), x86 options, x86 inline asm | 77 |

Bugs this suite found and fixed during the spike: `.fill` defaults, switch-table base, `.set` aliases in libgcc, number/identifier tokenising, indirect-jump syntax. Three tests needed a larger step limit (1M-iteration VLA loops, about 22M instructions each), and the runner allows 50M.

**Translation coverage.** `vc2cc.py --stats` over the 1,319 torture files GCC compiles with our flags: all 1,286 integer files translate and lay out. The 33 others use hardware float.

**Code size** (380 torture files): VC IV encoding 70,574 bytes with GCC's default registers, 70,350 with vc2cc's flags. VC II after translation: **72,190 (+2.6%)**. Most of the extra bytes come from VC II's shorter immediate ranges (`mov r15, #imm` + op) and from the lack of conditional ALU ops.

**Instruction kinds the compiled C relies on that Apple's code never exercises in the simulator self-test.** Coverage (`vc2sim.COVER`) over the unit rows and the torture run, re-measured after the pre-decrement fix (`isa.md`, "Device evidence, session 1"): compiled C executes 157 instruction kinds (encoding class + operation + condition), and **20 of them never run in `vc2sim --selftest`** (first measurement: 159 and 37). Their VC II semantics rest on VC IV analogy plus vc2sim's model, so the torture run passing shows only consistency with that model. Since session 1 there's a second question, whether the encoding exists at all. `vc2forms.py` answers it from Apple's static code. The decision for each:

| Kinds | In Apple's static code | Decision |
|---|---|---|
| 16-bit `cmp rd, #imm5`, `max #imm5` (on the first list) | no: `7000`–`77ff` is `ld/st (--rs)` | **Removed.** The assembler no longer has them. `cmp`/`max` with an immediate use the 32-bit forms, which the self-test runs |
| `bhi` at 16 and 32 bits | yes (36 and 75 sites) | **Kept** (`APPLE_STATIC_ONLY`): condition `hi` runs as `addcmpb hi`. Still a probe check (bits 8, 9) |
| 32-bit `bgt`/`blt`/`ble`/`bls`, `asr rd, rs`, indexed `ldsb` | yes | Allowed by the gate. On-chip checks in the bench (`bench_scalar.S`) |
| `abs`, `mulhd.ss`, `addscale5`/`6`, post-increment `ldh`, 48-bit pc-relative `st`/`ldh`/`sth` | yes | Allowed by the gate. `--conservative` avoids all but `mulhd.ss`; the probe checks `abs`, `mulhd.ss`, `addscale6`, `ldh (rs)++` and pc-relative `ld`/`st` |
| 48-bit pc-relative `ldb`, `ldsb`, `ldsh`, `stb` | **no** | Not allowed until device-verified. `--conservative` already turns them into `mov r15, #addr` plus an indirect access |

These should go first on a device check: one test image that runs `vc2cc_tests`' functions and reports results through the host port.

## VC IV instructions GCC emits, and their VC II form

Measured over the torture corpus. "Direct" means the same operation with VC II's encoding (`vc2dis` picks the shortest form).

| VC IV (GCC output) | VC II | Notes |
|---|---|---|
| `mov add sub rsub and or eor bic shl lsr asr mul max min neg not abs msb bmask cmp cmn btest` | direct (`shl` → `lsl`, `btest` → `btst`) | Immediates that don't fit a VC II form: `mov r15, #imm` + register form; `add` also uses `lea rd, (rs+s27)`; `and #2ⁿ−1` → `bmask`; `and #~u5` → `bic`; `rsub rd, #imm` → `neg` + `add` |
| `mulhd.ss/uu`, `div.ss/uu` | direct (EXT group) | |
| `addscale rd, ra, rb<<n` | `adds2/4/8/16` (n = 1–4), `addscale5–8` | |
| `subscale` | `lsl r15` + `sub` | |
| `ld st ldh sth ldb stb ldsh ldsb` | direct for `(rs)`, `(rs+off)`, `(rs)++`, `(rs+ri)` when the index shift equals the access width, and absolute `sym` (48-bit pc-relative). **Halfword loads** (2026-10-25, `isa-scalar.md`, `quirks.md` #16, #33): `ldh` only in forms proven to zero-extend (16-bit `(rs)`, `(rs)++`, `(rs+ri)`; 32-bit `0x28`/o1 `(rs+off)` with r0–r15 and an s16 offset); a gp/sp-relative or 48-bit `ldh` (unproven: bench HWLD) computes the address into r15 first. `ldsh` only as the 16-bit `ldsh rd, (rs)` (the address into r15 for any other operand), although `0x29`/o1 sign-extends on silicon; `bcmfw/tests/halfload_test.py` | `(--rs)` → `add` + `ld`: VC II has pre-decrement (`7000`–`77ff`, `isa.md`), but only `ldb`/`stb` occur in Apple's code and no width is device-verified; post-increment of a high register → `ld` + `add`; other scaled index → `lsl`/`add` into r15 |
| `stm …,(--sp)` / `ldm …,(sp++)` | `push` / `pop` (bases r0/r6/r16/r24, same byte as VC IV) | |
| `b`, `b<cc>`, `bl`, `bl rN`, `rts`, `nop` | direct | |
| `b<cc> ra, rb/#u6, L` | `addcmpb<cc> ra, #0, rb/#u6, L` | Out of range → `cmp` + `b<cc>` |
| `addcmpb<cc>` | direct | Out of range → `add` + `cmp` + `b<cc>` |
| `switch rN` + `.case` table | `tbb`/`tbh rN` + byte/halfword table | Halfword offsets from the end of `tbb` |
| `op.<cc>` (conditional execution) | `b<!cc>` around the op | VC II ALU ops don't write flags, so runs of conditional ops keep their condition |
| `signext rd, #n` | `lsl` + `asr` | `sxtb`/`sxth` (ops 26/27) are low confidence, so they're avoided |
| `bitset/bitclear/bitflip` | `or`/`bic`/`eor` with `1<<n` | ALU ops 9/17/29 (VC IV `bset/bclr/bchg` candidates) are unidentified |
| `ror` | 3–4 instructions via r15/r22 | No VC II `ror` is known |
| `count` (popcount) | 16-instruction SWAR sequence | Same as above |
| `b rN`, far `j` | `add sp, #-4; st rN, (sp+0); pop pc` | VC II `b rN` is unidentified |
| **`fadd fsub fmul fdiv fcmp flts fltu ftrunc fmin fmax`** | **gap** | VC II's FPU is unknown. `double` already goes through libgcc's soft-float; for `float`, either avoid it (decoders are fixed-point) or patch GCC to drop its SFmode hardware patterns (0.5–1 day) |
| GNU nested functions (trampolines) | **unsupported** | They write VC IV code to the stack at run time |

## ABI: GCC (as we run it) vs Apple's VC II code

| | VC IV GCC with vc2cc flags | Apple VC II (`sim.md`) | Compatible? |
|---|---|---|---|
| Arguments | r0–r5, then on the stack at the caller's sp (7th at `[sp]`) | same | yes: `many` and `vsum` called by `vc2sim` with Apple's convention |
| Return | r0, 64-bit in r0:r1; structs ≤ 8 bytes in r0:r1, larger ones via a hidden pointer | r0, r0:r1 | scalars yes; structs unverified |
| 64-bit arguments | register pairs (r0:r1, r2:r3) | `__lmul` (r0:r1, r2:r3) | yes |
| Callee-saved | r6–r14 (r16–r21 untouched) | r6–r15 (r16–r23 unused by compiled code) | **No for r15:** vc2cc's scratch register, clobbered without saving. This matters only if Apple code calls ours (callbacks), and we never run Apple code. |
| Varargs | callee spills r0–r5 below the caller's sp, so the arguments form one array | same shape (`sprintf`) | yes, in the varargs row |
| Frame | `push r6-rN, lr` / `pop r6-rN, pc`, sp 4-aligned | same instructions | yes |
| `char` | unsigned | unsigned (`ldb` zero-extends) | yes |
| gp (r24) | not used | set by `vmcs.bin`'s reset code, unused by the `.vll` libraries | — |

## Building the toolchain (macOS arm64, done 2026-10-10)

Only Homebrew packages that were already installed are needed: `gmp`, `mpfr`, `libmpc` and `isl` (Guile was installed by the owner for CGEN, which route (b) doesn't need). There's nothing more to install. The submodule is `~/Git/re/vc4-toolchain/gcc-vc4`, checked out with `git submodule update --init --depth 1 gcc-vc4`.

```sh
mkdir ~/Git/re/vc2-gcc-build && cd ~/Git/re/vc2-gcc-build
../vc4-toolchain/gcc-vc4/configure --target=vc4-elf --prefix=$HOME/Git/re/vc4-prefix \
  --disable-nls --disable-ssp --enable-languages=c --without-headers --with-newlib \
  --disable-decimal-float --disable-threads --disable-libatomic --disable-libitm \
  --disable-libsanitizer --disable-libquadmath --disable-lto --disable-werror \
  --with-system-zlib --with-gmp=/opt/homebrew --with-mpfr=/opt/homebrew \
  --with-mpc=/opt/homebrew --without-isl \
  CC=clang CXX="clang++ -std=gnu++98" CFLAGS="-O1 -w" CXXFLAGS="-O1 -w" </dev/null
make -j10 all-gcc MAKEINFO=true && make install-gcc MAKEINFO=true
make -j10 all-target-libgcc MAKEINFO=true > libgcc.log 2>&1
tools/bcmre/vc2libgcc.py ~/Git/re/vc2-gcc-build/libgcc.log ~/Git/re/vc2-gcc-build ~/Git/re/vc2-gcc-spike/libgcc
```

GCC 6 needs two local fixes to build on macOS arm64 with clang. Neither touches the code generator.
1. `gcc/fibonacci_heap.h:481`: `heapb->min->compare (heapa->min)` becomes `heapb->m_min->compare (heapa->m_min)`. Clang rejects the original; upstream GCC fixed it the same way.
2. `config.host` has no `aarch64-*-darwin` host entry, so `host-darwin.o` lacks `host_hooks`. Set `host_hook_obj=host-default.o` in the build tree's `gcc/Makefile` and re-run `make`. `--with-system-zlib` avoids the bundled zlib's `fdopen` macro clash with the macOS SDK.

`vc2cc` finds GCC through `VC4_GCC` (default `~/Git/re/vc4-prefix/bin/vc4-elf-gcc`) and libgcc through `VC2CC_LIB` (default `~/Git/re/vc2-gcc-spike/libgcc`). The tests skip cleanly when GCC is absent.

## Firmware support (milestone 3, done 2026-10-10)

`bcmfw/` (our first image, `bringup.md`) is built with these. `bcmfw/build.py` drives them.

- **Layout: `--entry ADDR`.** Section `.vectors` sits at `--base`, then zero padding up to the entry, then `.text` from the entry. vc2cc checks that `_start` is the first thing in `.text`. The other sections follow as before. Linker-style symbols `__S_start` and `__S_end` (word-rounded) exist for every section, so crt0 can clear `.bss`. `-o` writes the load image, up to the end of `.data`.
- **Map: `--mapfile`.** It lists the section table, the load size and every symbol by address. `vc2cc.listing()` gives the VC II listing.
- **Native assembly.** `.syntax vc2` switches a `.s` file to VC II in vc2dis syntax.
  - Instructions are assembled as written, in the shortest encoding, with branches relaxed.
  - Symbols may be a whole operand (`b loop`, `bl f`, or `ld r0, var`, which is 48-bit pc-relative) or an immediate (`#sym`, `#sym+4`).
  - Constant expressions are folded, so `#(0x10001808 + 4 * (1))` from a `#define` works.
  - Vector instructions pass through untouched.
  - `.rept`/`.endr`; comments start with `;` or `//`.
  - `.S` files go through the C preprocessor first (`-x assembler-with-cpp`, with the same `-I`/`-D`), so assembly shares `config.h` with the C.
- **Libraries on demand.** `libs=` takes directories of `.s` files and pulls in only what something references, as an archive would. bcmfw compiles `lib/*.c` (its `memcpy` and `memset`) into such a directory. The firmware build uses `-fno-tree-loop-distribute-patterns`, so a copy loop never becomes a call to itself.
- **Integer only: `--no-float`.** Any reference to a libgcc soft-float or complex routine is an error (`FloatBanned`), and so is a VC IV float instruction. bcmfw's `config.h` also has `#pragma GCC poison float double`, so a float is a compile error before vc2cc ever sees it.
- **`--conservative`.** Rules, version 2 (after device session 1):
  1. Every instruction's kind (`vc2sim.cover_key`) must occur in Apple's static code (every `.vll` function, plus vmcs.bin and RenderServer.bin traced from 0x200 and the vectors, all versions: 384 kinds) or be on `tools/bcmre/device_verified.txt` (empty today). A kind can be wrong in two ways: its semantics, or its encoding. Static occurrence settles only the encoding, and only the device settles the semantics. `bcmfw/tests/forms_test.py` enforces this for every instruction of every device image (check regions may hold the kinds they test). `vc2cc.listing()` marks any other instruction `; unproven form`.
  2. What the image executes must also run in the self-test, except `vc2forms.APPLE_STATIC_ONLY` (`bhi` at 16 and 32 bits: the encodings occur in Apple's code, condition `hi` runs as `addcmpb hi`) and device-verified kinds. The coverage rows of `hello_test`, `dma_test`, `hostproto_test` and `bench_test` enforce this.

  These forms become sequences of instruction kinds that Apple's own code exercises in the vc2sim self-test:
  - `abs`: `asr`, `eor`, `sub`;
  - `not`: `mov #-1`, `eor`;
  - `addscale5–7` and `adds16`: `lsl`, `add`;
  - post-increment `ldh` and other unproven post-increment widths: the access, then `add`;
  - 48-bit pc-relative `ld`/`st`: `mov r15, #addr` plus an indirect access.

  GCC uses `abs` for `x != 0`, `addscale6` for `y * 320`, and pc-relative stores for globals. In bcmfw the mode costs 76 bytes (3,944 → 4,020). With it, the image executes 70 instruction kinds, and only two of them don't run in the self-test: `bhi` at 16 and 32 bits. Until session 1 there was a third, the 16-bit `cmp rd, #imm5`. It was "proven in another encoding", but the encoding itself didn't exist: on the chip it is `ld (--rs)`, and the probe faulted on it. That's why rule 1 asks for the encoding itself in Apple's code.

vc2cc_tests are unchanged: 1,787 cases, 0 failures. bcmfw's bench covers the new paths: native `.S`, `.rept`, the entry layout, libraries on demand, the float ban and `--conservative`.

## Fixed-base links: overlays (2026-10-24, #126)

vc2cc has no object files or relocations, so a second program that runs beside a resident image is linked against the resident image's addresses, at its own fixed base. The first user is the display-only image's overlay, the visualizer (`visualizer.md` §8, `jobs.md` §5, "Overlays") [sim].

- **The overlay side: `build(base=ADDR, imports=Imports(symbols, names, version))`.** `symbols` is the resident `Program`'s `.symbols`, or `read_map()` of its `.map` file. Only `names` resolve into the resident image, at their addresses there. Every other undefined name is an error, except the layout's own `__S_start`/`__S_end`. Library code (libgcc, `lib/`) is pulled into the overlay for whatever it doesn't import, so an overlay can't depend on what the resident happened to link.
- **The resident side: `build(exports=(names, version))`.** The same list, in the same order: it is the interface.
- **The ABI word, `__vc2_abi`.** Both programs see this symbol. Its value is the CRC-32 of `vc2abi <version>` and one `name=address` line per listed name (`abi_hash()`). The overlay stores it in its header and the resident compares it with its own before it runs anything in the overlay. A rebuild of the resident that moves any export, or another `version`, changes it; a rebuild that moves nothing else keeps it. Bump the version for what the hash can't see: the layout of a shared struct, a calling convention, the header.
- **Command line:** `--import-map MAP --imports a,b --abi-version N` (the overlay), `--exports a,b --abi-version N` (the resident).
- **Placement:** `.vectors` sits at the base (`__attribute__((section(".vectors")))`), so an overlay's header comes first; without `--entry`, `.text` follows it.
- **Tests:** `vc2cc_tests.py` `t_fixed_base` (96 cases): a resident at `0x300000` and an overlay at `0x340000` in one vc2sim; calls and data both ways through the header's entry, the overlay's own `.data`, `.bss` and libgcc; the map-file round trip; another version, a moved export and a name off the list all refused; the same rebuild accepted.

## Remaining work for route (b), estimated

| Step | Days |
|---|---|
| ~~Firmware image: entry, vectors, crt0, `--entry`~~ | done |
| ~~Native VC II `.s` input~~ | done |
| ~~Minimal libc (`memcpy`, `memset`)~~; `str*` and a debug `printf` to the host mailbox when needed | 0.25 |
| ~~`float`: hard ban~~ | done |
| Toolchain build script with the two host patches; vc2cc tests and the bcmfw bench in `scripts/validate.sh` (lines proposed in `bringup.md`) | 0.5 |
| Source-line listings (`-g` `.loc` → map) and symbolised vc2sim traces | 0.5 |
| Optional size peepholes (e.g. `cmp` + branch fusion, reusing r15 constants) | 0–1 |
| Placement of sections in SDRAM, once an image needs SDRAM | 0.25 |

## Known GCC bugs

### `min.l` / `max.l` lose their source register (found 2026-10-23, the 3D renderer)

**What happens.** `vc4.md`'s `sminsi3` and `smaxsi3` (vc4-elf-gcc 6.2.1, the build in `~/Git/re/vc4-prefix`) end with an alternative that accepts any register as operand 1 (constraint `r`) and a 32-bit immediate (`i`), but prints the two-operand form `min.l %0,#%2`. So `y = x < 240 ? x : 240` with `x` and `y` in different registers compiles to `min.l rY,#240`, which takes the minimum of whatever `rY` held before, not of `x`. The compiler's own `-S` output already has the bug; vc2cc translates it faithfully.

**How it showed.** `bcmfw/r3d/r3d.c`'s triangle raster clamped its last row with `ye = min(y2, 240)`. At `-O2` GCC kept `y2` in one register and `ye` in r0, and r0 still held the last edge slope from a division just before. Most triangles then drew nothing, and the rest drew the wrong rows. Found by drawing hand-made triangles one at a time in vc2sim against a Python model of the same algorithm (`bcmfw/tests/r3d_test.py` keeps the hook, `r3d_sim_raster`). Only alternative 5 is affected. The other alternatives of every `define_insn` either tie operand 1 to the output or take it as a constant (checked with a script over `vc4.md`).

**Reach.** The small immediates (alternatives 3 and 4) are fine. Only bounds that need a 32-bit immediate trigger it: clamps to 240, 320, 2,490,368 and the like. **None of the existing VideoCore images contains `min.l` or `max.l`.** A scan of every C file in bcmfw (the video decoder, the AAC decoder, the drift, the hello, bench and probe images), compiled as their builds compile them, found 0 sites. So no image already run on the device was affected.

**The guard.** `vc2cc.compile_c` now refuses any `min.l`/`max.l` in GCC's output and names the file. Existing images are byte-identical (`mkdevtest --check`, `mkvideo --check`).

**The workaround in source.** Make the bound opaque to GCC so it picks the register form:
```c
static inline int32_t opaque(int32_t v) { __asm__("" : "+r"(v)); return v; }
...
ye = opaque(240);
if (y2 < ye)
    ye = y2;
```

**The real fix,** not yet done: tie operand 1 of alternative 5 to the output (`"0"` instead of `"r"`) in `gcc-vc4/gcc/config/vc4/vc4.md` and rebuild cc1. Then the guard can stay as a check that never fires.

**Lessons for chip code:**
- Test new C in vc2sim against an independent model, not only by looking at pictures.
- Build any new renderer or decoder for the Mac too. If its checksums match the simulator's, both the compiler and the simulator are confirmed for that code. `bcmfw/r3d/host` does this, and `r3d_test.py` checks it on every run.

## Risks

- **Semantics we can only take from VC IV:** the 20 instruction kinds listed above. One device run of the test image settles them. Until then, vc2sim is the only witness, and it was built from the same hypotheses.
- **pc-relative 48-bit memory base** (word-aligned or not) is open (`hardware.md`). Every global access depends on it, and vc2cc inherits whatever `vc2dis` decides. A fallback that's correct either way costs 2 bytes per access: `mov r15, #addr` + `ld rd, (r15)`. `--conservative` selects that fallback, and bcmfw uses it.
- **Code quality:** GCC 6 from 2016 with a VC IV cost model. VC II's encodings are slightly less dense, but +2.6% is acceptable. Hot loops (CAVLC, IDCT, interpolation, deblocking) will be hand-written vector code anyway (`approach.md` workstream 3).
- **Unmaintained GCC fork:** pinned at commit `0fe4b83` of `puppeh/gcc-vc4`. Since we use only `cc1`'s assembly output, a future host-compiler break can be fixed by building on Linux or in a container.
- **No ELF or debugger:** debugging goes through vc2sim traces and the symbol map. That's acceptable for a firmware this size.
