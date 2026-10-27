#!/bin/bash
#***************************************************************************
#               __________                   __    __________
#    Open       \______   \_______    ____  |  | __\______   \  ____  ___  ___
#    Source      |     ___/\_  __ \  /  _ \ |  |/ / |    |  _/ /  _ \ \  \/  /
#    Player      |    |     |  | \/ (  <_> )|    <  |    |   \(  <_> ) >    <
#    Firmware    |____|     |__|     \____/ |__|_ \ |______  / \____/ /__/\_ \
#                                                \/        \/               \/
#
# Custom firmware for the iPod Video, 5th generation (5G and 5.5G).
# Not affiliated with or endorsed by Apple Inc.
#
# Copyright (C) 2026 prokopto-dev
#
# This program is free software; you can redistribute it and/or
# modify it under the terms of the GNU General Public License
# as published by the Free Software Foundation; either version 2
# of the License, or (at your option) any later version.
#
# This software is distributed on an "AS IS" basis, WITHOUT WARRANTY OF ANY
# KIND, either express or implied.
#
#***************************************************************************

# Self-test for scripts/risk/reach.py (Phase 9, T).
#
# Builds a tiny ARM7TDMI image from sources written below (into
# build/reach-selftest/, regenerated every run) and checks reach.py's
# verdicts against what the fixture was written to contain: direct calls,
# a function-pointer table, a callback registered through a literal, a
# dead static next to live code in one .text, an unreferenced table and
# what only it points at, bss reached through a section anchor, ARM ->
# Thumb interworking through a linker stub, a long call into a far
# section through a veneer, an asm label referenced only by .word, an asm
# function that falls through into the next one, and live/dead callers.
# Then the same sources with -ffunction-sections -fdata-sections are
# linked twice (no GC, and --gc-sections --print-gc-sections): every
# section the linker drops must be dead to reach.py on the no-GC image,
# and every function reach.py calls dead must be dropped by the linker
# (the fixture has no hidden references, so the two must agree exactly).
#
#   scripts/risk/reach_selftest.sh        prints ok/FAIL lines, exit 0/1
set -u
cd "$(dirname "$0")/../.."
ROOT=$(pwd)
D=$ROOT/build/reach-selftest
REACH=$ROOT/scripts/risk/reach.py
CC=${CC:-arm-none-eabi-gcc}
command -v "$CC" >/dev/null || { echo "reach_selftest: $CC not found"; exit 1; }
rm -rf "$D"; mkdir -p "$D"
fail=0
ok()  { echo "ok   $*"; }
bad() { echo "FAIL $*"; fail=1; }

cat > "$D/crt0.S" <<'EOF'
    .section .vectors, "ax"
    .global vectors
vectors:
    ldr pc, =start
    ldr pc, =irq_handler
    ldr pc, =fiq_handler
    .ltorg

    .section .init.text, "ax"
    .global start
    .type start, %function
start:
    ldr sp, =0x00100000
    bl main
    ldr r0, =asm_label_target   @ only reference: a literal
    mov lr, pc
    bx r0
    bl asm_fall_a
1:  b 1b
    .ltorg

EOF

cat > "$D/asm.S" <<'EOF'
    .text
    .global asm_label_target
asm_label_target:               @ NOTYPE global, no .type
    mov r0, #1
    bx lr

    .global asm_fall_a
    .type asm_fall_a, %function
asm_fall_a:                     @ falls through into asm_fall_b
    add r0, r0, #1
    .global asm_fall_b
    .type asm_fall_b, %function
asm_fall_b:
    add r0, r0, #2
    bx lr

    .global asm_dead
    .type asm_dead, %function
asm_dead:
    bx lr
EOF

cat > "$D/main.c" <<'EOF'
typedef int (*fp)(int);
extern int thumb_fn(int);
extern int far_fn(int);
extern void *scratch_get(long *size);
extern void *scratch_live(long *size);
int counter;                       /* bss, reached through an anchor */
static int hidden_state;           /* same anchor block */
static int (*volatile registered)(int);
static void register_cb(int (*cb)(int)) { registered = cb; }
int cb_fn(int x) { return x + 3; }
int via_table(int x) { return x * 2; }
int via_table2(int x) { return x * 4; }
int dead_target(int x) { return x - 7; }
const fp live_table[] = { via_table, via_table2 };
const fp dead_table[] = { dead_target };
static __attribute__((noinline)) int dead_static(int x) { return x ^ 0x55; }
int dead_global_uses_static(int x) { return dead_static(x) + 1; }
__attribute__((noinline)) int live_direct(int x) { return x + counter; }
int dead_caller(void) { long n; return scratch_get(&n) != 0; }
int live_caller(void) { long n; return scratch_live(&n) != 0; }
void irq_handler(void) { counter++; }
void fiq_handler(void) { hidden_state++; }
volatile int sink;
int main(void)
{
    register_cb(cb_fn);
    sink = live_direct(1) + live_table[sink & 1](2) + registered(3)
         + thumb_fn(4) + far_fn(5) + live_caller() + hidden_state;
    return 0;
}
EOF

cat > "$D/scratch.c" <<'EOF'
static char buf[64];
void *scratch_get(long *size) { *size = sizeof buf; return buf; }
void *scratch_live(long *size) { *size = 1; return buf + 1; }
EOF

cat > "$D/thumb.c" <<'EOF'
__attribute__((noinline)) int thumb_callee(int x) { return x + 11; }
int thumb_fn(int x) { return thumb_callee(x) * 3; }
int thumb_dead(int x) { return x - 1; }
EOF

cat > "$D/far.c" <<'EOF'
__attribute__((section(".icode"))) int far_fn(int x) { return x + 5; }
__attribute__((section(".icode"))) int far_dead(int x) { return x + 6; }
EOF

cat > "$D/link.lds" <<'EOF'
ENTRY(start)
SECTIONS
{
    .text 0x00000000 : {
        *(.init.text)
        *(.text*)
        *(.glue_7) *(.glue_7t)
    }
    .rodata : { *(.rodata*) }
    .data : { *(.data*) }
    .vectors 0x00000000 : AT(LOADADDR(.data) + SIZEOF(.data)) { KEEP(*(.vectors)) }
    .iram 0x40000000 : AT(LOADADDR(.vectors) + SIZEOF(.vectors)) { *(.icode) }
    .bss (LOADADDR(.iram) + SIZEOF(.iram)) (NOLOAD) : { *(.bss*) *(COMMON) }
    /DISCARD/ : { *(.ARM.exidx*) *(.comment) }
}
EOF

FLAGS="-mcpu=arm7tdmi -Os -ffreestanding -fno-builtin -nostdlib -fno-common -fsection-anchors -mthumb-interwork"
build() { # outdir extra-cflags extra-ldflags
    local o=$1; mkdir -p "$o"
    $CC $FLAGS $2 -marm -c "$D/crt0.S" -o "$o/crt0.o" &&
    $CC $FLAGS $2 -marm -c "$D/asm.S" -o "$o/asm.o" &&
    $CC $FLAGS $2 -marm -c "$D/main.c" -o "$o/main.o" &&
    $CC $FLAGS $2 -marm -c "$D/scratch.c" -o "$o/scratch.o" &&
    $CC $FLAGS $2 -mthumb -c "$D/thumb.c" -o "$o/thumb.o" &&
    $CC $FLAGS $2 -marm -c "$D/far.c" -o "$o/far.o" &&
    $CC $FLAGS -T "$D/link.lds" $3 -Wl,-Map,"$o/fixture.map" \
        "$o/crt0.o" "$o/asm.o" "$o/main.o" "$o/scratch.o" "$o/thumb.o" "$o/far.o" \
        -o "$o/fixture.elf" 2>"$o/link.err"
}
if ! build "$D/plain" "" ""; then
    cat "$D/plain/link.err"; echo "FAIL reach_selftest: fixture does not build"; exit 1
fi

R=$(python3 -I "$REACH" "$D/plain/fixture.elf" --reachable --unreachable \
    --callers scratch_get --callers scratch_live --paths thumb_callee \
    --paths far_fn --json "$D/plain/reach.json" 2>"$D/plain/reach.err")
rc=$?
[ $rc = 0 ] || { cat "$D/plain/reach.err"; bad "reach.py exit $rc on the fixture"; exit 1; }
echo "$R" > "$D/plain/reach.txt"

expect() { # live|dead symbol why
    if echo "$R" | grep -qE "^$( [ "$1" = live ] && echo reach || echo dead ) $2 "; then
        ok "$1 $2 ($3)"
    else
        bad "$2 should be $1 ($3): $(echo "$R" | grep -E "^(reach|dead) $2 " || echo 'not listed')"
    fi
}
expect live start        "ELF entry / crt0 root"
expect live vectors      ".vectors root"
expect live main         "called from crt0"
expect live irq_handler  "default root and vector literal"
expect live fiq_handler  "default root"
expect live live_direct  "bl from main"
expect live via_table    "pointer in a live table"
expect live via_table2   "pointer in a live table"
expect live live_table   "anchor/literal from main"
expect live cb_fn        "callback passed through a literal"
expect live counter      "bss through the anchor"
expect live thumb_fn     "ARM -> Thumb through the interworking stub"
expect live thumb_callee "Thumb bl"
expect live far_fn       "long call through a veneer"
expect live asm_label_target "asm label referenced only by .word"
expect live asm_fall_a   "bl from crt0"
expect live asm_fall_b   "fall-through from asm_fall_a"
expect live scratch_live "called from live_caller"
expect dead dead_static  "unreferenced static in a live .text"
expect dead dead_global_uses_static "unreferenced"
# without -fdata-sections main.o's .rodata is one input section, so the
# unreferenced table rides along with the live one (the brief's anchor
# rule: conservative); with data sections it is dead (GC agreement below)
expect live dead_table   "shares main.o's .rodata with live_table"
expect live dead_target  "pointed at from that shared .rodata"
expect dead thumb_dead   "unreferenced Thumb function"
expect dead far_dead     "unreferenced far function"
expect dead asm_dead     "unreferenced asm function"
expect dead dead_caller  "unreferenced"
expect dead scratch_get  "only caller is dead"

if echo "$R" | grep -q "^caller scratch_get "; then
    bad "scratch_get has no live caller: $(echo "$R" | grep '^caller scratch_get ')"
else ok "no live caller of scratch_get"; fi
if echo "$R" | grep -q "^caller scratch_live live_caller main.o$"; then
    ok "caller scratch_live live_caller main.o"
else bad "missing 'caller scratch_live live_caller main.o': $(echo "$R" | grep '^caller')"; fi
if python3 -I -c "import json,sys; d=json.load(open(sys.argv[1])); sys.exit(0 if any(c['caller']=='dead_caller' and not c['live'] for c in d['callers']) else 1)" "$D/plain/reach.json"; then
    ok "json lists dead_caller as a dead caller of scratch_get"
else bad "json: dead_caller missing from the dead callers"; fi
if echo "$R" | grep -qE "^path far_fn .*main > .*far_fn$"; then
    ok "path far_fn: $(echo "$R" | grep '^path far_fn')"
else bad "path far_fn: $(echo "$R" | grep '^path far_fn' || echo none)"; fi

# --cut: dropping scratch.o leaves live_caller calling into nothing
# (fine for the analysis) and frees scratch.o's bytes
C=$(python3 -I "$REACH" "$D/plain/fixture.elf" --cut 'scratch.o' 2>/dev/null)
sz=$(arm-none-eabi-size -A "$D/plain/scratch.o" | awk '$1 ~ /^\.(text|rodata|data)/ {s+=$2} END {print s+0}')
if echo "$C" | grep -qE "^cut image=$sz bss=64 objects=1 also=0$"; then
    ok "cut scratch.o: $(echo "$C" | grep '^cut')"
else bad "cut scratch.o: expected image=$sz bss=64 objects=1 also=0, got: $(echo "$C" | grep '^cut')"; fi
C=$(python3 -I "$REACH" "$D/plain/fixture.elf" --cut 'thumb.o' --unreachable 2>/dev/null)
if echo "$C" | grep -q "^cut .*objects=1"; then ok "cut thumb.o: $(echo "$C" | grep '^cut')"; else bad "cut thumb.o: $(echo "$C" | grep '^cut')"; fi
python3 -I "$REACH" "$D/plain/fixture.elf" --cut 'nothing*.o' >/dev/null 2>&1 \
    && bad "--cut with no matching object must exit 1" || ok "--cut with no match exits non-zero"
python3 -I "$REACH" "$D/plain/fixture.elf" --root no_such_symbol >/dev/null 2>&1 \
    && bad "--root unknown must exit 1" || ok "--root unknown exits non-zero"

# linker GC agreement (function/data sections, the fixture has no hidden refs)
if build "$D/nogc" "-ffunction-sections -fdata-sections" "" &&
   build "$D/gc" "-ffunction-sections -fdata-sections" "-Wl,--gc-sections -Wl,--print-gc-sections"; then
    python3 -I "$REACH" "$D/nogc/fixture.elf" --unreachable > "$D/nogc/reach.txt" 2>/dev/null
    dropped=$(sed -n "s/.*removing unused section '\.[a-z]*\.\([A-Za-z_0-9]*\)' in file.*/\1/p" "$D/gc/link.err" | sort -u)
    deadf=$(awk '$1=="dead" && $3 ~ /^\.(text|rodata|data|bss)\./ {print $2}' "$D/nogc/reach.txt" | sort -u)
    only_ld=$(comm -23 <(echo "$dropped") <(echo "$deadf") | tr '\n' ' ')
    only_reach=$(comm -13 <(echo "$dropped") <(echo "$deadf") | tr '\n' ' ')
    for s in dead_table dead_target dead_static asm_dead thumb_dead far_dead; do
        grep -q "^dead $s " "$D/nogc/reach.txt" || bad "function/data sections: $s should be dead"
    done
    if [ -z "${only_ld// }" ] && [ -z "${only_reach// }" ]; then
        ok "linker GC and reach.py agree on $(echo "$dropped" | wc -l | tr -d ' ') dropped sections"
    else
        bad "linker GC vs reach.py: dropped-but-live=[${only_ld}] dead-but-kept=[${only_reach}]"
    fi
else
    bad "function-sections fixture does not build: $(cat "$D/gc/link.err" "$D/nogc/link.err" 2>/dev/null | head -5)"
fi

[ $fail = 0 ] && echo "reach_selftest: PASS" || { echo "reach_selftest: FAIL"; exit 1; }
