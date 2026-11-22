/* libstdc++ has one ELF section per function, and each would become an AROS hunk. A relocatable link
 * keeps them apart, so this second pass merges them. Other sections keep their places. */
SECTIONS
{
  .text : { *(.text .text.* .gnu.linkonce.t.*) }
  .rodata : { *(.rodata .rodata.* .gnu.linkonce.r.*) }
  .data : { *(.data .data.* .gnu.linkonce.d.*) }
  .bss : { *(.bss .bss.* COMMON) }
  .gcc_except_table : { *(.gcc_except_table .gcc_except_table.*) }
}
