/* The images are only measured, never run (as in wire_weaver's shrink_wrap/size_bench). */
MEMORY { FLASH : ORIGIN = 0, LENGTH = 256K }
ENTRY(_start)
SECTIONS {
  .text : { KEEP(*(.text.start)) *(.text .text.*) } > FLASH
  .rodata : { *(.rodata .rodata.* .srodata .srodata.*) } > FLASH
  .data : { *(.data .data.* .sdata .sdata.*) } > FLASH
  .bss (NOLOAD) : ALIGN(4) { *(.bss .bss.* .sbss .sbss.* COMMON) } > FLASH
  /DISCARD/ : { *(.eh_frame*) *(.comment) *(.ARM.exidx*) *(.ARM.attributes) *(.riscv.attributes) }
}
