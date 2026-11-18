/* Stock CA79 V2.19 (CAx9_P06.SYS, sha256 09ad8570...d933c8) routine and globals the Phase 3-1 SysEx
 * port binds to (result/firmware/ports/cave/sysex_stock.h, slot payload ota_sysex / ota_main).  Linked
 * into the power-family payloads (main_payload.ld).  stk_binding / stk_port3_fwd are in stock.ld.
 * Evidence: REA analysis of the stock image, re-read by disassembly;
 * tools/roundtrip_sysex.py checks the words. */

stk_param_set  = 0x4014EBFC; /* FUN_4014EBFC(ctx, fn, msg + 6, len): lookup .. apply FUN_4016053C   */
stk_dev_id     = 0x413B5404; /* u32 receive device number (FUN_40147D60), read only                  */
stk_stage_flag = 0x419156A9; /* u8 fn 0x09 staging flag (FUN_4015C410), read only                     */
stk_sess_base  = 0x413CA3B0; /* u8[0x42] session globals, read only (op 0x1B 02) */
stk_tick2ms    = 0x4139E796; /* u16 2 ms counter (FUN_400AB2E8), read only                             */
/* settings block A in RAM, loaded from NOR 0x60460000 by FUN_400AA10C at boot (REA):
 * word 0 model code (FUN_400B39C4), word 3 destination (FUN_400B39D8), read only - the
 * startup port's factory AutoPowerOff rule (kpstart.h ksu_apo_factory) */
stk_blka_model = 0x413A7798;
stk_blka_dest  = 0x413A77A4;
