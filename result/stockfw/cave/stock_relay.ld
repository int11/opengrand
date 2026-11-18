/* Stock CA79 V2.19 (CAx9_P06.SYS, sha256 09ad8570...d933c8) globals the panel-UI relay reads
 * (result/firmware/ports/cave/relay_stock.h, slot payload ota_main, feat2 0x04).  Linked into the
 * power-family payloads (main_payload.ld).  Read only.
 * Evidence: REA disassembly of the four TX ring writers (same body):
 * `ldr r1, [pool]` at writer + 0x30 = the u16 byte count the writer spins on (>= 0x7FE). */
stk_txcnt0 = 0x413AE5A8; /* FUN_4014492C, pool 0x40144B98 */
stk_txcnt1 = 0x413AE5B4; /* FUN_401445AC, pool 0x40144D9C */
stk_txcnt2 = 0x413AE586; /* FUN_40145284, pool 0x40144E9C */
stk_txcnt3 = 0x413AE59C; /* FUN_40144CD0, pool 0x40144F78 */
