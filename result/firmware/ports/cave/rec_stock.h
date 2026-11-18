/* OpenGrand cave port - the stock CA79 V2.19 routines the recorder playback port binds to (recctl_cave.c,
 * slot payload ota_main, Identify feat2 0x10).
 *
 * On the piano the addresses come from the linker (result/stockfw/cave/stock_rec.ld); on the host the unit
 * test (tests/test_recctl_cave.c) defines them.  Every routine is one the stock SysEx apply FUN_4016053C
 * itself calls for the same parameter, from the same task (19) - evidence: REA (Ghidra 12.1.4) + objdump
 * (the apply cases 0x401636CC Music / 0x40160F68
 * SongSelect / 0x40160F40 IntSongPart / 0x4016372C MusicTempo / 0x40160A9C MusicProgress in MusicMode 2). */
#ifndef CA79NG_REC_STOCK_H
#define CA79NG_REC_STOCK_H

#include <stdint.h>

/* FUN_4015D70C(kind, value): Music / MusicMode queue for task 20 (kind 1 Music, 0 MusicMode) */
void stk_rec_post(uint32_t kind, uint32_t value);
/* FUN_4015B4D8(0): the current settings block (the apply's `sl`) */
uint32_t stk_rec_block(uint32_t zero);
/* FUN_401687E4(block, song): SongSelect - state 1/6 stop, (4 commits: never called there), load */
uint32_t stk_rec_select(uint32_t block, uint32_t song);
/* FUN_4016C9B8(v): IntSongPart (7F both, 0 part 1, 1 part 2) - reloads the song with that part */
uint32_t stk_rec_part(uint32_t v);
/* FUN_401694E4(rc): the MaxTime notice 52 18 (queued for the notify task) */
void stk_rec_maxtime(uint32_t rc);
/* FUN_4014DC0C(rc): Result F0 40 7F 7E 04 1E 03 00 <rc hi7> <rc lo7> F7 to the stock binding */
void stk_rec_result(uint32_t rc);
/* FUN_40167294(bpm): song tempo (no range check; ratio = bpm << 8 / original tempo) */
void stk_rec_tempo(uint32_t bpm);
/* FUN_40169448(bar s16, b, block): seek (MusicProgress bar form, data[5] = b) */
void stk_rec_seek(uint32_t bar, uint32_t b, uint32_t block);

/* InternalSongDelete owned by the core: the routines the stock's
 * own song writes call after building the song - KSO import FUN_4014F1A8 (0x4014F258..) and FUN_4016C7BC - task 19 */
/* FUN_40168408(song, src, len): song `song` := len B at src, the songs after it move, one NOR job (slot 1); 0 or 0xEA */
uint32_t stk_rec_store_write(uint32_t song, uint32_t src, uint32_t len);
/* FUN_401686E8(): *(u16 *)0x41915710 = 0x100 (after every song write) */
void stk_rec_store_changed(void);
/* FUN_401674FC(song): the player loader (the song's tracks / settings -> play state; 0xEC when it is empty) */
uint32_t stk_rec_load(uint32_t song);
/* FUN_40167128(): *(u8 *)0x4191EF7B = 0 */
void stk_rec_load_done(void);
/* FUN_4014F294(0): the edit answer 32 51 7E 7F 00 00 to the stock binding */
void stk_rec_edit_ok(uint32_t rc);

#endif
