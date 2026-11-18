/* Stock CA79 V2.19 (CAx9_P06.SYS, sha256 09ad8570...d933c8) routines and globals the Phase 3-2
 * session port binds to (result/firmware/ports/cave/session_stock.h, slot payload ota_session /
 * ota_main).  Linked into the power-family payloads (main_payload.ld).
 * Evidence: REA analysis of the stock image, re-read by disassembly;
 * tools/roundtrip_session.py checks the words. */

/* request answers: FUN_40152A34 / FUN_401526DC without the sender */
stk_desc        = 0x4014C034; /* descriptor i = 0x400FAA2C + 10 i                                  */
stk_req_match   = 0x4015205C; /* (i, v1, v2) -> 0 / -1                                             */
stk_part_x      = 0x4014EAF4; /* part byte -> getter slot                                          */
stk_desc_has    = 0x401520B4; /* (i, slot) -> nonzero when the descriptor has it                   */
stk_ctx_of      = 0x4015B470; /* binding -> ctx (table 0x400A710E)                                 */
stk_slot16      = 0x4015B188; /* j -> slot (table 0x400A7120)                                      */
stk_slot5       = 0x4015B00C; /* j -> slot (table 0x400A711B)                                      */
stk_getter      = 0x4015E960; /* (ctx, slot, i, buf) -> 0 ok                                       */
stk_frame_build = 0x4014B0DC; /* (cc, {desc, key, buf}, out, n) -> length                          */
stk_key80       = 0x400FD8CB;
stk_key81       = 0x400FD8D0;
stk_key82       = 0x400FD8E0;

/* CONNECT and the binding */
stk_remote_get  = 0x40151BB8;
stk_remote_set  = 0x40151B90;
stk_conn_mode   = 0x400B1414;
stk_conn_sub    = 0x400B1428;
stk_conn_kind   = 0x400B143C;
stk_conn_post_a = 0x400B0FBC;
stk_conn_post_b = 0x400B13AC;
stk_ctx_copy    = 0x4015DE98;
stk_ctx_clean   = 0x4015C33C;
stk_bind        = 0x40151B5C;
stk_unbind      = 0x4014B55C;
stk_reset_a     = 0x4014B5A0;
stk_reset_b     = 0x4014B5D0;
stk_reset_c     = 0x4014B610;
stk_link_state  = 0x4139E7F8; /* u8, read only                                                     */
stk_file        = 0x413CA3E5; /* fn 0x24 job state bytes +0 / +5..+8, read only */

/* Phase 4 A intake gate (sysex_cave.c): the data behind
 * FUN_4014A2C8's tests and its 7E 04 / 05 info answers (getters and literal pools read by REA), read only */
stk_gate_media       = 0x41359E45; /* FUN_4012795C (literal 0x401284EC)                                 */
stk_gate_model       = 0x419156A7; /* FUN_4015BA64 (literal 0x4015C39C)                                 */
stk_gate_info_d      = 0x418CE0EC; /* literal 0x4014ACD4                                                */
stk_gate_model_idx   = 0x400B39B8;
stk_gate_model_entry = 0x400B389C;
stk_gate_version     = 0x400B3894;
stk_gate_info_b      = 0x400B39F0;
stk_gate_info_c      = 0x400B39D8;

/* stage 2 fan-out: the per-port SysEx TX rings the
 * writers FUN_4014492C / FUN_401445AC / FUN_40145284 / FUN_40144CD0 fill (2 KB, write index u16 & 0x7FF,
 * literal pools 0x40144BA4/0x40144CC8, 0x40144DA8/0x40144E94, 0x40144EA8/0x40144F70, 0x40145098/0x40145124).
 * Read only: the core reads what the stock wrote to its binding's port. */
stk_txring0     = 0x413ACAFC;
stk_txwr0       = 0x413AE5A6;
stk_txring1     = 0x413ADAFC;
stk_txwr1       = 0x413AE5B2;
stk_txring2     = 0x413A9AFC;
stk_txwr2       = 0x413AE584;
stk_txring3     = 0x413ABAFC;
stk_txwr3       = 0x413AE59A;

/* the stock target of the fifth hook site (MIDI pump tail, task 19) */
stk_hk_midi     = 0x4012A7D4; /* site 0x401539AC                                                   */
