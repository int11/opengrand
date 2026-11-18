/* OpenGrand slot payload: the cave linked to run at OTA_RUN_BASE (include/ota.h), loaded there
 * by the ota_loader resident stage after CRC + HMAC verification.  The entry table
 * (src/ota_payload.c) comes first.  Same .bss rule as cave.ld: nothing zeroes it.
 * Core RW at 0x40180000 (NOLOAD), shared with the built-in cave - only one of them is ever
 * active in a boot.                                                                       */
INCLUDE stock.ld
INCLUDE stock_core.ld

OTA_RUN_BASE    = 0x401C0000;
OTA_PAYLOAD_MAX = 0x1FDC0;      /* 0x20000 sector - 0x200 state area - 64 B header */

MEMORY {
  RUN     (rwx) : ORIGIN = 0x401C0000, LENGTH = 0x1FDC0
  CORE_RW (rw)  : ORIGIN = 0x40180000, LENGTH = 0x401A0000 - 0x40180000
}

ENTRY(ota_entry)

SECTIONS {
  .cave : {
    KEEP(*(.ota_entry))
    *(.text .text.*)
    *(.rodata .rodata.*)
    *(.data .data.*)
    . = ALIGN(4);
    cave_end = .;
  } > RUN

  .bss (NOLOAD) : { *(.bss .bss.*) *(COMMON) } > RUN

  .core_rw (NOLOAD) : ALIGN(8) {
    core_rw_start = .;
    *(.core_rw .core_rw.*)
    . = ALIGN(8);
    core_rw_end = .;
  } > CORE_RW

  /DISCARD/ : { *(.comment) *(.ARM.attributes) *(.note*) }
}

ASSERT(SIZEOF(.bss) == 0, "payload: .bss must be empty - build with -fno-zero-initialized-in-bss")
ASSERT(ota_entry == OTA_RUN_BASE, "payload: the entry table must be first")
ASSERT(cave_end <= OTA_RUN_BASE + OTA_PAYLOAD_MAX, "payload: too big for a slot")
ASSERT(core_rw_end <= 0x401A0000, "core RW overflows into the OTA staging buffer")

/* BT live-apply handover block (firmware/ports/cave/bt_cave.c, btsrc.h BTS_HO_RAW = 52 B): the
 * top 64 B of the core RW window, kept by the apply (it copies to OTA_RUN_BASE and stages at OTA_STAGE, both above) and
 * written by no payload's core RW - so the old payload's block is still there when the new one starts.  Fixed for every
 * payload generation from here on. */
PROVIDE(bt_handover = 0x4019FFC0);   /* defined only in the payloads that link the BT port */
ASSERT(core_rw_end <= 0x4019FFC0, "core RW reaches the BT handover block at 0x4019FFC0")
