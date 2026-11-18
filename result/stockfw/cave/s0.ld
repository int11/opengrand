/* OpenGrand OTA loader v4 STAGE-0 (src/s0.c, image ota_loader_v4): file 0x113A14 <-> SDRAM 0x40173A14, loaded by the
 * stock loader copy #1 (extended to 0x40176000 by the manifest).  Frozen layout (include/s0.h):
 *   0x40173A14 - 0x40175FDF  stage-0 code + .data (.data is the copy from NOR: fresh at every power-on)
 *   0x40175FE0 - 0x40175FFF  root key (NOT part of the blob - build_images.py patches it from secrets/ota_key.bin)
 *   0x40176000 -             the stage-1 window (40 KB), loaded by stage-0                                      */
INCLUDE stock.ld
INCLUDE stock_ota.ld
INCLUDE stock_norw.ld

CAVE_BASE = 0x40173A14;
s0_key    = 0x40175FE0;

MEMORY {
  S0 (rwx) : ORIGIN = 0x40173A14, LENGTH = 0x40175FE0 - 0x40173A14
}

ENTRY(s0_h1)

SECTIONS {
  .cave : {
    *(.text.entry)
    *(.text .text.*)
    *(.rodata .rodata.*)
    *(.data .data.*)
    . = ALIGN(4);
    cave_end = .;
  } > S0

  .bss (NOLOAD) : { *(.bss .bss.*) *(COMMON) } > S0

  /DISCARD/ : { *(.comment) *(.ARM.attributes) *(.note*) }
}

ASSERT(SIZEOF(.bss) == 0, "stage-0: .bss must be empty - build with -fno-zero-initialized-in-bss")
ASSERT(s0_h1 == CAVE_BASE, "stage-0: s0_h1 must be first")
ASSERT(cave_end <= s0_key, "stage-0: code runs into the root key")
