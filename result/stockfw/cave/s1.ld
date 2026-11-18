/* OpenGrand OTA loader v4 STAGE-1 (src/s1.c, image ota_stage1): linked to run in the stage-1 window
 * 0x40176000-0x4017FFFF (40 KB, include/s0.h), where stage-0 loads it from a loader slot after CRC + signature.
 * Entry table first.  No stock NOR writer (stock_norw.ld is not included): NOR writes go through stage-0's service.
 * .data is part of the image (stage-0 loads it fresh every time); .bss must be empty.                          */
INCLUDE stock.ld
INCLUDE stock_core.ld
INCLUDE stock_ota.ld

MEMORY {
  WIN (rwx) : ORIGIN = 0x40176000, LENGTH = 0xA000
}

ENTRY(s1_entry)

SECTIONS {
  .cave : {
    KEEP(*(.rodata.s1_entry))
    *(.text .text.*)
    *(.rodata .rodata.*)
    *(.data .data.*)
    . = ALIGN(4);
    cave_end = .;
  } > WIN

  .bss (NOLOAD) : { *(.bss .bss.*) *(COMMON) } > WIN

  /DISCARD/ : { *(.comment) *(.ARM.attributes) *(.note*) }
}

ASSERT(SIZEOF(.bss) == 0, "stage-1: .bss must be empty - build with -fno-zero-initialized-in-bss")
ASSERT(s1_entry == 0x40176000, "stage-1: the entry table must be first")
ASSERT(cave_end <= 0x40180000, "stage-1: larger than the 40 KB window")
