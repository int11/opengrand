/* Stock CA79 V2.19 (CAx9_P06.SYS, sha256 09ad8570...d933c8) routines and globals of the Phase 4 MIDI channel port
 * (result/firmware/ports/cave/chan_stock.h, ota_main feat3 0x10).  Linked into the power-family payloads
 * (main_payload.ld).  Evidence: REA decompile + disassembly;
 * tools/roundtrip_midich.py runs them. */

/* the hook targets (ota_loader_v3 allow-list 6 / 7 / 8) */
stk_mc_pc       = 0x401475CC; /* FUN_401475CC(port, status, program): Program Change      (site 0x40148C4C) */
stk_mc_cc       = 0x40148218; /* FUN_40148218(port, status, cc, value): Control Change    (site 0x40148C60) */
stk_mc_defer    = 0x401638E0; /* FUN_401638E0(elapsed ms): the delayed apply timers       (site 0x40153998) */

/* what both channel routines read first (FUN_40146D04 / FUN_40146D70), read only */
stk_mc_run      = 0x40135980; /* FUN_40135980(): s8 run-state byte (*0x4139DF75, else *0x4139DF76)        */
stk_mc_sub      = 0x401593B0; /* FUN_401593B0(ctx): u8                                                    */
stk_mc_block    = 0x4015B4D8; /* FUN_4015B4D8(ctx): the settings block (+0x32 omni)                        */
stk_mc_mode     = 0x413B540C; /* u8: the path of an unbound port (literal pool 0x40147A10)                 */

/* FUN_401638E0's tables (literal pools 0x401639B8-0x401639C0; written only by it, the arm FUN_401604E8 and the
 * init 0x40160364) */
stk_mc_tm       = 0x41033850; /* int32 [9][5] countdowns, ms                                                */
stk_mc_val      = 0x41033B24; /* u8 [9][5] value bytes, >= 0x80 = nothing to apply                         */
stk_mc_any      = 0x41033C40; /* u32 any slot armed                                                         */
