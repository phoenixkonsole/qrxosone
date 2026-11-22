/* Reset code below 4 GB, everything else above. */
OUTPUT_FORMAT(elf64-m68k)
ENTRY(_start)
PHDRS { boot PT_LOAD; text PT_LOAD; data PT_LOAD; }
SECTIONS {
    . = 0x10000;
    .boot : { *(.boot) } :boot
    . = 0x120000000;
    .text : { *(.text .text.*) } :text
    .rodata : { *(.rodata .rodata.*) } :text
    . = ALIGN(4096);
    .data : { *(.data .data.*) } :data
    .bss : { *(.bss .bss.*) *(COMMON) } :data
    /DISCARD/ : { *(.eh_frame) *(.comment) *(.note.*) }
}
