/*
 * Linker script for the RISC-V 64-bit Mazu kernel.
 * Target: QEMU virt machine (DRAM at 0x80000000, OpenSBI at 0x80000000-0x80200000).
 * Initial mapping: identity (bare mode, satp=0, virt == phys).
 */

ENTRY(_start)

PHDRS
{
    text PT_LOAD FLAGS(5);   /* R + E: code and read-only data */
    data PT_LOAD FLAGS(6);   /* R + W: mutable data, BSS, embedded blobs */
}

SECTIONS
{
    . = 0x80200000;

    .text : {
        /* Trap entry must be first so stvec (direct mode) hits it near start. */
        *(.text.entry)
        *(.text*)
    } :text

    .rodata : {
        *(.rodata*)
        *(.srodata*)
    } :text

    . = ALIGN(8);

    .inithook : {
        PROVIDE(__inithook_start = .);
        KEEP(*(.inithook))
        PROVIDE(__inithook_end = .);
    } :text

    . = ALIGN(8);

    .initgraph : {
        PROVIDE(__initgraph_start = .);
        KEEP(*(.initgraph))
        PROVIDE(__initgraph_end = .);
    } :text

    . = ALIGN(8);

    .selftest : {
        PROVIDE(__selftest_start = .);
        KEEP(*(.selftest))
        PROVIDE(__selftest_end = .);
    } :text

    . = ALIGN(8);

    .data : {
        /* __global_pointer$ is placed 0x800 bytes into .data so that the
         * gp-relative addressing window (±2 KiB) covers the most-used globals.
         */
        PROVIDE(__global_pointer$ = . + 0x800);
        *(.sdata*)
        *(.data*)
    } :data

    .bss : {
        . = ALIGN(8);
        _bss_start = .;
        *(.bss*)
        *(COMMON)
        . = ALIGN(8);   /* ensure _bss_end is 8-byte aligned for sd zeroing */
        _bss_end = .;
    } :data

    . = ALIGN(8);

    /* Embedded rootfs archive. */
    .rootfs_archive : {
        _rootfs_archive_start = .;
        *(.rootfs_archive*);
        _rootfs_archive_end = .;
    } :data

    /* Linker symbols consumed by print.c for output column alignment.
     * These are conservative upper bounds; the x86 build computes exact values
     * via nm at build time. These are sufficient for the RISC-V port.
     */
    BASENAME_MAX_LEN = 40;
    LINE_MAX_LEN     = 4;
    FUNCNAME_MAX_LEN = 50;

    /DISCARD/ : { *(.eh_frame*) }
}
