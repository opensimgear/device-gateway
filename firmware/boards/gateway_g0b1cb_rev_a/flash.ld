ENTRY(Reset_Handler)
MEMORY {
  /* Last two 2 KiB pages are exclusively owned by the registry journal. */
  FLASH (rx) : ORIGIN = 0x08000000, LENGTH = 124K
  STORAGE (r) : ORIGIN = 0x0801F000, LENGTH = 4K
  /* 128 KiB is available without opting into the additional parity SRAM. */
  RAM (xrw) : ORIGIN = 0x20000000, LENGTH = 128K
}
_estack = ORIGIN(RAM) + LENGTH(RAM);
__registry_page0 = ORIGIN(STORAGE);
__registry_page1 = ORIGIN(STORAGE) + 0x800;
SECTIONS {
  .isr_vector : { KEEP(*(.isr_vector)) } > FLASH
  .text : { *(.text*) *(.rodata*) KEEP(*(.init)) KEEP(*(.fini)) . = ALIGN(4); } > FLASH
  .ARM.extab : { *(.ARM.extab*) } > FLASH
  .ARM.exidx : { __exidx_start = .; *(.ARM.exidx*) __exidx_end = .; } > FLASH
  .preinit_array : { PROVIDE_HIDDEN(__preinit_array_start = .); KEEP(*(.preinit_array*)) PROVIDE_HIDDEN(__preinit_array_end = .); } > FLASH
  .init_array : { PROVIDE_HIDDEN(__init_array_start = .); KEEP(*(SORT(.init_array.*))) KEEP(*(.init_array*)) PROVIDE_HIDDEN(__init_array_end = .); } > FLASH
  .fini_array : { PROVIDE_HIDDEN(__fini_array_start = .); KEEP(*(SORT(.fini_array.*))) KEEP(*(.fini_array*)) PROVIDE_HIDDEN(__fini_array_end = .); } > FLASH
  _sidata = LOADADDR(.data);
  .data : { . = ALIGN(4); _sdata = .; *(.data*) . = ALIGN(4); _edata = .; } > RAM AT > FLASH
  .bss (NOLOAD) : { . = ALIGN(4); _sbss = .; *(.bss*) *(COMMON) . = ALIGN(4); _ebss = .; } > RAM
  .heap_stack (NOLOAD) : { . = ALIGN(8); end = .; _end = .; . += 0x200; . += 0x4000; } > RAM
  ASSERT(LOADADDR(.data) + SIZEOF(.data) <= ORIGIN(STORAGE), "Firmware overlaps registry storage")
  ASSERT(_ebss + 0x4200 <= _estack, "RAM exhausted: heap/stack reserved")
}
