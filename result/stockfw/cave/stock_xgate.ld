/* Stock CA79 V2.19 (CAx9_P06.SYS, sha256 09ad8570...d933c8) executors the Phase 4 executor gate tail-calls
 * (result/firmware/ports/cave/xgate_cave.h, src/power_hooks.c, slot payload ota_main feat3 0x02).  Linked into the
 * power-family payloads (main_payload.ld).  The `bl` targets of ota_loader_v3 sites 13 / 16 - evidence:
 * (REA decompile + the site words 0xEBFF8EB2 / 0xEBFF80D5). */
stk_hk_media = 0x40127594; /* FUN_40127594(): USB memory mount / unmount executor (engine loop 0x40143AC4) */
stk_hk_xport = 0x40123E34; /* FUN_40123E34(): audio-file transport executor (engine loop 0x40143AD8)       */
