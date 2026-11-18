/* OpenGrand — internal song delete owned by the core.
 *
 * The stock "InternalSongDelete" 32 52 21 7F [fmt hi lo part] (apply 0x40160AD4 -> FUN_4016C800 -> FUN_4016C7BC) does
 * not delete: FUN_4016C7BC re-serialises song N with FUN_40168664 -> FUN_4016804C from the RECORD track table
 * 0x4191DB14 (+0x44 data pointer, +0x50 length per track) - the tracks of the last take prepared by FUN_40168A70 - and
 * the `part` byte only picks where the +0x24 start-settings block comes from.  Nothing removes a track.  So on the piano
 * (field 2026-11-15) deleting the song just recorded rewrote it unchanged (OK reply, NOR job, nothing deleted), and in
 * the emulator deleting ANOTHER song overwrote it with the take; with no take since boot the track table pointers are 0.
 *
 * The core therefore builds the new song itself and hands it to the stock writer FUN_40168408 (the KSO import's
 * writer: RAM move of the following songs + one NOR job):
 *   part 0 / 1   remove track slot 0 / 1 (part 1 / 2): its bytes are cut out
 *                (up to the next block / track / data end; the song is zero-padded back to 4), later offsets move down, +0x70 (bars of the
 *                longest track) is counted again over the tracks left
 *   part 7F      the empty song of the boot template (ROM 0x400A7574 + 0x74 * song, the 0x74 B FUN_40168664 writes
 *                when FUN_40167CEC() != 1), as is the result of removing the last track
 * Pure functions: no stock memory here (the cave port passes pointers). */
#ifndef CA79NG_RECDEL_H
#define CA79NG_RECDEL_H

#include <stdint.h>

#include "ca79ng/recview.h"

#define RD_HDR 0x74u                   /* song header; an empty song is exactly this */
#define RD_A_TEMPLATE 0x400A7574u      /* ROM: 10 empty song headers of 0x74 B (FUN_40168664 0x401686B8) */
#define RD_A_BUF 0x40A00000u           /* the stock serialisation buffer (commit, KSO import) */
#define RD_BUF_MAX 0xFFFFFu            /* the import's size limit for that buffer */

/* rd_build() results */
enum {
    RD_OK = 0,       /* out / out_len hold the new song */
    RD_NOTHING = 1,  /* the song has no such part (or is empty): nothing to write, the delete is done */
    RD_E_SONG = 2,   /* the song's header / tracks do not follow the format: refuse, write nothing */
    RD_E_ROOM = 3    /* the output does not fit `cap` */
};

/* rd_judge() verdicts */
enum {
    RD_V_OWN = 0,    /* the core deletes (rd_build + the stock writer) */
    RD_V_STOCK = 1,  /* hand it to the stock apply: fmt not 04 / 7F (dropped), part not 0 / 1 / 7F (writes nothing),
                        or hi:lo 7F 7F = every song (FUN_4016C908 rebuilds the empty table - a real format) */
    RD_V_BUSY = 2,   /* refuse (Busy): playing / recording family, NOR job / worker / file operation, broken table */
    RD_V_ARG = 3     /* refuse (Busy): song >= 10 (the stock would take song 1) */
};
#define RD_ALL_SONGS 0x3FFFu

uint8_t rd_judge(const rv_view_t *v, uint8_t fmt, uint16_t song, uint8_t part);

/* song = the song's bytes [0, len) as stored; part 0 / 1 / 0x7F; empty = its 0x74 B template header */
uint8_t rd_build(const uint8_t *song, uint32_t len, uint8_t part, const uint8_t *empty, uint8_t *out, uint32_t cap,
                 uint32_t *out_len);

/* bars (FA events) of the track at t[0..n): 0 when it does not start with FA or runs past n without FC */
uint32_t rd_track_bars(const uint8_t *t, uint32_t n);

#endif
