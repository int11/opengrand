/* Stock CA79 V2.19 (CAx9_P06.SYS, sha256 09ad8570...d933c8) routines the recorder playback port calls
 * (result/firmware/ports/cave/rec_stock.h, slot payload ota_main, feat2 0x10).  Linked into the power-family
 * payloads (main_payload.ld).  The same routines the stock SysEx apply FUN_4016053C calls for these parameters,
 * from the same task 19 - evidence: REA + objdump of the apply
 * cases 0x401636CC / 0x40160F68 / 0x40160F40 / 0x4016372C / 0x40160A9C). */
stk_rec_post    = 0x4015D70C; /* FUN_4015D70C(kind, value): Music / MusicMode queue -> task 20   */
stk_rec_block   = 0x4015B4D8; /* FUN_4015B4D8(0): current settings block                          */
stk_rec_select  = 0x401687E4; /* FUN_401687E4(block, song): SongSelect                            */
stk_rec_part    = 0x4016C9B8; /* FUN_4016C9B8(v): IntSongPart                                     */
stk_rec_maxtime = 0x401694E4; /* FUN_401694E4(rc): MaxTime notice 52 18                           */
stk_rec_result  = 0x4014DC0C; /* FUN_4014DC0C(rc): Result 7E 03 00 hi lo to the binding           */
stk_rec_tempo   = 0x40167294; /* FUN_40167294(bpm): song tempo                                    */
stk_rec_seek    = 0x40169448; /* FUN_40169448(bar, b, block): seek                                */
/* InternalSongDelete by the core: the calls of FUN_4016C7BC / the KSO import */
stk_rec_store_write   = 0x40168408; /* FUN_40168408(song, src, len): song write + NOR job          */
stk_rec_store_changed = 0x401686E8; /* FUN_401686E8(): 0x41915710 = 0x100                         */
stk_rec_load          = 0x401674FC; /* FUN_401674FC(song): player loader                          */
stk_rec_load_done     = 0x40167128; /* FUN_40167128(): 0x4191EF7B = 0                             */
stk_rec_edit_ok       = 0x4014F294; /* FUN_4014F294(0): 32 51 7E 7F 00 00                         */
