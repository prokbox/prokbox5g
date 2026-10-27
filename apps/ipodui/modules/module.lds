/* ProkBox shell modules (#106, apps/ipodui/modload.h).
 * Linked twice, at __mod_base = MOD_LINK_BASE and MOD_LINK_BASE + 1 MB,
 * with --just-symbols=rockbox.elf: every call into the core then goes
 * through an absolute long veneer, and the only words that differ between
 * the two links are the module's own ABS32 words (tools/modpak.py turns
 * them into the relocation list and refuses anything else). */
OUTPUT_FORMAT(elf32-littlearm)
OUTPUT_ARCH(arm)
ENTRY(mod_exports)

SECTIONS
{
    . = __mod_base;
    .text : {
        KEEP(*(.modexports))
        __mod_exports_end = .;
        *(.text .text.*)
        *(.glue_7 .glue_7t .v4_bx .vfp11_veneer)
        *(.rodata .rodata.*)
        . = ALIGN(4);
    }
    __mod_image_end = .;
    /* No writable data: a relocated word written at run time would be
     * wrong after a move. Make it const, or bss. */
    .data : { *(.data .data.*) }
    ASSERT(SIZEOF(.data) == 0, "module: writable .data (make it const or bss)")
    /* No IRAM: IRAM is the codec's */
    .iram : { *(.icode .icode.* .irodata .irodata.* .idata .idata.* .ibss .ibss.*) }
    ASSERT(SIZEOF(.iram) == 0, "module: IRAM section")
    .bss (NOLOAD) : {
        __mod_bss_start = .;
        *(.bss .bss.* COMMON)
        . = ALIGN(4);
        __mod_bss_end = .;
    }
    /DISCARD/ : { *(.ARM.exidx*) *(.ARM.extab*) *(.comment) *(.note*) *(.ARM.attributes) }
}
