/* OpenGrand slot payload entry table (images built with PAYLOAD=1, tools/images.py OTA_PAYLOADS).
 *
 * A slot payload is the ordinary cave (ca79ng.c, + the core when FEAT1_CORE) linked to run
 * at OTA_RUN_BASE (ota_payload.ld) instead of the stock-copied cave region.  It has no hook
 * entry code of its own: the resident stage of ota_loader (src/ota.c) keeps the two stock
 * hooks, verifies the slot, copies it to OTA_RUN_BASE and calls through this table.
 * Layout: include/ota.h; contract: REMOTE_PROTOCOL section 10.3.                          */
#include "stock.h"
#include "ota.h"

void ng_sysex(u32 port, const u8 *buf, u32 len);
void ng_tick(void);
int ng_quiesce(void);

#if (CA79NG_FEAT1) & 0x10
/* ota_power (feat1 0x10): the power port's runtime hooks are registered in attach()
 * (src/power_hooks.c, ota_loader_v2); on ota_loader (v1) it is never called. */
void pw_attach(const struct ota_svc *svc);

#if defined(__thumb__)
/* Thumb payload (cave Makefile THUMB=1): the resident checks every entry
 * pointer with in_run(p, len, 4) - a Thumb address (bit 0 set) is refused and the frozen ota_loader_v2 cannot change.
 * So the table holds four ARM entry points: `ldr pc, [pc, #-4]` loads the next word, the Thumb function's address
 * with bit 0 set (R_ARM_ABS32 of a Thumb symbol), and an ARMv5T load to pc switches state on bit 0.  r0-r3 and lr
 * are untouched: the C function returns straight to the resident (`pop {pc}` / `bx lr` interwork on ARMv5T). */
void ota_arm_sysex(u32 port, const u8 *buf, u32 len);
void ota_arm_tick(void);
int ota_arm_quiesce(void);
void ota_arm_attach(const struct ota_svc *svc);
#define OTA_ARM_ENTRY(name, fn) \
    "    .global " #name "\n    .type   " #name ", %function\n" #name ":\n" \
    "    ldr     pc, [pc, #-4]\n    .word   " #fn "\n    .size   " #name ", . - " #name "\n"
__asm__(
    "    .pushsection .text.ota_arm_entry, \"ax\", %progbits\n"
    "    .arm\n"
    "    .align  2\n"
    OTA_ARM_ENTRY(ota_arm_sysex, ng_sysex)
    OTA_ARM_ENTRY(ota_arm_tick, ng_tick)
    OTA_ARM_ENTRY(ota_arm_quiesce, ng_quiesce)
    OTA_ARM_ENTRY(ota_arm_attach, pw_attach)
    "    .popsection\n");
#define E_SYSEX ota_arm_sysex
#define E_TICK ota_arm_tick
#define E_QUIESCE ota_arm_quiesce
#define E_ATTACH ota_arm_attach
#else
#define E_SYSEX ng_sysex
#define E_TICK ng_tick
#define E_QUIESCE ng_quiesce
#define E_ATTACH pw_attach
#endif

__attribute__((section(".ota_entry"), used))
const struct ota_entry_x ota_entry = {
    { OTA_ENTRY_MAGIC, OTA_ABI, E_SYSEX, E_TICK,
      (CA79NG_FEAT0) | ((CA79NG_FEAT1) << 8) | OTA_EF_ATTACH, E_QUIESCE },
    E_ATTACH
};
#else
__attribute__((section(".ota_entry"), used))
const struct ota_entry ota_entry = {
    OTA_ENTRY_MAGIC, OTA_ABI, ng_sysex, ng_tick,
    (CA79NG_FEAT0) | ((CA79NG_FEAT1) << 8), ng_quiesce
};
#endif
