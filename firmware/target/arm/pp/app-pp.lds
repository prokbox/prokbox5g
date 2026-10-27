#define __ASSEMBLER__
#include "cpu.h"

/* Will have been included from app.lds */
ENTRY(start)

OUTPUT_FORMAT(elf32-littlearm)
OUTPUT_ARCH(arm)
STARTUP(target/arm/pp/crt0-pp.o)

#define PLUGINSIZE PLUGIN_BUFFER_SIZE
#define CODECSIZE CODEC_SIZE

#ifdef HAVE_UI_THREAD_SPLIT
/* ProkBox P19 S3: shell objects whose statics a CPU thread writes
 * asynchronously (playback events, tick tasks, the video player's hooks)
 * while the shell, which calls them often, may run on the COP: their
 * .data and .bss are linked uncached (.ncdata/.ncbss).  The other shell
 * objects with CPU callbacks are routed instead (febe_link_check.py
 * SHELL_BE_OBJECTS) and stay cached. */
#define FEBE_ASYNC_OBJS *np_playback.o *sdk_cold.o *devbench.o *devbcm.o
#define FEBE_EX EXCLUDE_FILE(FEBE_ASYNC_OBJS)
#else
#define FEBE_EX
#endif

#define DRAMSIZE (MEMORYSIZE * 0x100000) - PLUGINSIZE - CODECSIZE

#define DRAMORIG 0x00000000
#define IRAMORIG 0x40000000
#define IRAMSIZE 0xc000

/* End of the audio buffer, where the codec buffer starts */
#define ENDAUDIOADDR  (DRAMORIG + DRAMSIZE)

/* Where the codec buffer ends, and the plugin buffer starts */
#define ENDADDR (ENDAUDIOADDR + CODECSIZE)

MEMORY
{
    DRAM : ORIGIN = DRAMORIG,     LENGTH = DRAMSIZE
    IRAM : ORIGIN = IRAMORIG,     LENGTH = IRAMSIZE
}

SECTIONS
{
    .text :
    {
        loadaddress = .;
        _loadaddress = .;
        . = ALIGN(0x200);
        *(.init.text)
        *(.text*)
        *(.glue_7)
        *(.glue_7t)
        . = ALIGN(0x4);
        _text_end = .;  /* ProkBox: panic.c's stack scan */
    } > DRAM

    .rodata :
    {
        *(.rodata)  /* problems without this, dunno why */
        *(.rodata*)
        *(.rodata.str1.1)
        *(.rodata.str1.4)
        . = ALIGN(0x4);
    } > DRAM

    .data :
    {
#ifdef HAVE_UI_THREAD_SPLIT
        /* ProkBox P19 (docs/design/p19-ui-on-cop.md): the UI shell's cached
         * data on whole cache lines of its own, so a line never holds both
         * a shell static and another object's (the cores' caches are not
         * coherent: the last eviction would win the whole line) */
        . = ALIGN(CACHEALIGN_SIZE);
        *apps[/]ipodui[/]*(FEBE_EX .data*)
        *libfirmware.a:font.o(.data*)       /* the shell's drawing state */
        *libfirmware.a:lcd-16bit.o(.data*)
        . = ALIGN(CACHEALIGN_SIZE);
#endif
        *(FEBE_EX .data*)
        . = ALIGN(0x4);
    } > DRAM

#if NOCACHE_BASE != 0
    /* .ncdata section is placed at uncached physical alias address and is
     * loaded at the proper cached virtual address - no copying is
     * performed in the init code */
    .ncdata . + NOCACHE_BASE :
    {
        . = ALIGN(CACHEALIGN_SIZE);
#ifdef HAVE_UI_THREAD_SPLIT
        *apps[/]ipodui[/]*(.data*)  /* FEBE_ASYNC_OBJS: the rest went above */
#endif
        *(.ncdata*)
        . = ALIGN(CACHEALIGN_SIZE);
    } AT> DRAM
#endif
    
    /DISCARD/ :
    {
        *(.eh_frame)
    }

    .vectors 0x0 :
    {
        _vectorsstart = .;
        KEEP(*(.vectors));
        _vectorsend = .;
    } AT> DRAM

    _vectorscopy = LOADADDR(.vectors);
    _noloaddram  = LOADADDR(.vectors);

    .ibss IRAMORIG (NOLOAD) :
    {
        _iedata = .;
        *(.qharray)
        *(.ibss*)
        . = ALIGN(0x4);
        _iend = .;
    } > IRAM

    .iram _iend :
    {
        _iramstart = .;
        *(.icode*)
        *(.irodata*)
        *(.idata*)
        . = ALIGN(0x4);
        _iramend = .;
    } > IRAM AT> DRAM

    _iramcopy = LOADADDR(.iram);


    .init ENDAUDIOADDR : 
    {
        . = ALIGN(4);
        _initstart = .;
        *(.init*)
        _initend = .;
    } AT> DRAM

    _initcopy = LOADADDR(.init);

    .idle_stacks (NOLOAD) :
    {
       *(.idle_stacks)
       . = ALIGN(8);
#if NUM_CORES > 1
       cpu_idlestackbegin = .;
       . += IDLE_STACK_SIZE;
       cpu_idlestackend = .;
#endif
       cop_idlestackbegin = .;
       . += IDLE_STACK_SIZE;
       cop_idlestackend = .;
    } > IRAM

    .stack (NOLOAD) :
    {
       *(.stack)
       stackbegin = .;
       . += 0x2000;
       stackend = .;
    } > IRAM
    
    /* .bss and .ncbss are treated as a single section to use one init loop to
     * zero it - note "_edata" and "_end" */
    .bss _noloaddram (NOLOAD) :
    {
       _edata = .;
#ifdef HAVE_UI_THREAD_SPLIT
        /* ProkBox P19: the shell's zeroed data, as .data above */
        . = ALIGN(CACHEALIGN_SIZE);
        *apps[/]ipodui[/]*(FEBE_EX .bss* FEBE_EX COMMON)
        *libfirmware.a:font.o(.bss* COMMON)
        *libfirmware.a:lcd-16bit.o(.bss* COMMON)
        . = ALIGN(CACHEALIGN_SIZE);
#endif
        *(FEBE_EX .bss*)
        *(FEBE_EX COMMON)
        . = ALIGN(0x4);
    } > DRAM

#if NOCACHE_BASE != 0
    .ncbss . + NOCACHE_BASE (NOLOAD):
    {
    	. = ALIGN(CACHEALIGN_SIZE);
#ifdef HAVE_UI_THREAD_SPLIT
        *apps[/]ipodui[/]*(.bss* COMMON) /* FEBE_ASYNC_OBJS */
#endif
        *(.ncbss*)
    	. = ALIGN(CACHEALIGN_SIZE);
    } AT> DRAM
#endif

    /* This will be aligned by preceding alignments */
    .endaddr . - NOCACHE_BASE (NOLOAD) :
    {
        _end = .;
    } > DRAM

    .audiobuf _end (NOLOAD) :
    {
        _audiobuffer = .;
        . = ALIGN(0x4);
        audiobuffer = .;
    } > DRAM
    
    .audiobufend ENDAUDIOADDR (NOLOAD) :
    {
#ifdef IPOD_VIDEO
        audiobufend_lds = .;
#else
        audiobufend = .;
#endif
        _audiobufend = .;
    } > DRAM

    .codec ENDAUDIOADDR (NOLOAD) :
    {
        codecbuf = .;
        _codecbuf = .;
    }

    .plugin ENDADDR (NOLOAD) :
    {
        _pluginbuf = .;
        pluginbuf = .;
    }
}
