/*
 * AROS puts the __startup entry in .aros.startup.  It must be the first
 * code in the Hunk: LoadSeg starts at offset 0, and starting at
 * odfs_aros_handler instead would pass it the AROS_PROCH register ABI
 * rather than a normal C stack argument, so the handler never answers
 * its startup packet and Mount/List wait forever.
 */
SECTIONS {
 .text : { *(.aros.startup) *(.text) *(.text.*) }
 .rodata : { *(.rodata .rodata.*) }
 .data : { *(.data .data.*) }
 .bss : { *(.bss .bss.*) *(COMMON) }
}
