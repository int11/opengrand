/* OpenGrand cave port - Phase 3-6 R1: playback control of the stock song player by the core (slot payload
 * ota_main, Identify feat2 0x10).  Core module src/recorder/recctl.c (decides) + recview.c (reads).
 *
 * Where it runs (task 19 only, the allow-listed hooks of ota_main - no new hook):
 *   front end 0x40147E94  sysex_cave.c hands every bound controller's playback frame to rcc_kawai() - fn 0x10
 *                         sets through the front's io_apply (after its ACK), fn 0x32 commands through sx_cmd:
 *                         Music 10|32 52 08 (0 pause, 1 play, 2 stop), SongSelect 10 52 00 (numType 0),
 *                         IntSongPart 10 52 01, MusicTempo 10 52 02, MusicProgress 10|32 52 03 (bar form,
 *                         data[0] = data[5] = 0).  The core judges it (rc_cmd) and runs it through the same
 *                         stock routine the stock apply calls (rec_stock.h) - the stock apply never sees it.
 *                         Refused -> the sender gets the stock's own refusal bytes: Result F0 40 7F 7E 04 1E 03
 *                         00 01 6C F7 (no data, 0xEC) or Busy F0 40 7F 7E 04 1E 03 01 00 00 F7 (the rest), the
 *                         same for every controller (panel, app, op 0x1E).  Left to the stock unchanged: another
 *                         MusicMode (3 USB, 4-6), the record transports (Music 3-8), any Music in a recording
 *                         state 3-6 (R2 owns recording: Stop there ends the recording), SongSelect numType != 0,
 *                         MusicProgress in MusicMode 0 or not in bar form, packed sets.
 *   0x7D op 0x1E          rcc_sysex() (task 19, ng_sysex): 1E 00 status / 1E 01 song list answered at once,
 *                         1E 02 <cmd> <hi7> <lo7> queued for the pump tail (bound controllers only).
 *   pump tail 0x401539AC  rcc_pump() from ssc_pump(): reads the stock recorder (recview), runs a queued op
 *                         command (5E 02 <rc> <cmd>), confirms commands (rc_poll -> 5E 03 <ev> <cmd> to the op's
 *                         sender), and tells every subscribed bound controller about changes (5E 00 status,
 *                         5E 01 song list).  A controller subscribes by sending any op 0x1E request while bound;
 *                         a Kawai-only controller (the stock panel app) is never sent a 0x7D frame - it gets the
 *                         stock's own Kawai notices (Music status / MusicProgress / MaxTime on the binding's TX
 *                         ring, fanned out by the session core).
 * R2 (Identify feat3 0x01, every cave build since the fold): Music 3-6 and Stop / Pause in
 * the recording states 3-6 are the core's too (recctl rc_enable_rec), op 1E 02 takes cmd 8-11.  Even then the
 * recording, its serialisation and the RecorderData write stay the stock's: the core only queues Music values.
 * R3 (Identify feat3 0x04, every cave build since the fold): the save
 * decision recsave - write windows around every song-table write (commit seen as state 4 -> not 4, InternalSongDelete
 * 32 52 21 judged and written here, the file executor), checked after the NOR job; Kawai deletes refused
 * with the stock's Busy while a take / play / job / window runs, 24 08 / 24 09 refused through rcc_file_veto, op 1E 04 =
 * the save record 5E 04 (also sent to the subscribers when a window closes).
 * InternalSongDelete (every build): the stock delete re-serialises the last
 * take instead of deleting, so the core builds the song (recdel.c) and writes it with the stock writer FUN_40168408 (+ its
 * NOR job), state 0 / 2 only.  REMOTE_PROTOCOL.md §10.9.  No other stock RAM write, no commit path. */
#ifndef CA79NG_RECCTL_CAVE_H
#define CA79NG_RECCTL_CAVE_H

#include <stdint.h>

#define RCC_OP 0x1E
#define RCC_STATUS_RAW 48
#define RCC_LIST_RAW 70
/* op 0x1E rc values beyond recctl's RC_* (0-8) */
#define RCC_NOT_BOUND 9
/* 5E 03 events beyond RC_EV_DONE (1) / RC_EV_TIMEOUT (2): a Stop / Pause took the command's place */
#define RCC_EV_REPLACED 3
/* notification spacing (ms): state changes / bar-only changes */
#define RCC_NOTE_MS 50u
#define RCC_BAR_MS 250u

/* task 19, front end (sysex_cave.c): a frame of a bound controller on stock port `port`.  1 = the core owned it
 * (ran or refused it - the stock apply must not run), 0 = the stock's (apply it as before). */
int rcc_kawai(uint8_t port, const uint8_t *m, uint32_t n);
/* task 19, ng_sysex: 0x7D op 0x1E (the cave checked the tag, F7 and len >= 8) */
void rcc_sysex(uint8_t port, const uint8_t *buf, uint32_t len);
/* task 19, pump tail (ssc_pump, inflight held): view, queued command, confirmation, notifications */
void rcc_pump(uint32_t now_ms);
/* OTA apply-now: 0 = idle (no op command waiting) */
int rcc_quiesce(void);
/* the raw op 0x1E 00 status record / 1E 01 song list */
void rcc_status(uint8_t raw[RCC_STATUS_RAW]);
void rcc_list(uint8_t raw[RCC_LIST_RAW]);
/* host tests: forget everything */
void rcc_restart(void);
/* host tests only (RCC_HOST builds define it): R2 / R3 on / off (always on in the cave), taken at the next init */
void rcc_host_rec2(int on);
void rcc_host_rec3(int on);
/* kawai_map's code for InternalSongDelete 32 52 21 (not a recctl command - recdel.h) */
#define RCC_K_DELETE 0x80
/* R3, task 19 before the session core: 1 = refuse this fn 0x24 request 24 08 / 24 09 with the stock's ACK + Busy (a take
 * in progress / a song-table write window / a NOR job - recsave.h rs_file), 0 = the usual path */
int rcc_file_veto(uint8_t d0, uint8_t d1);

/* sysex_cave.c (session core): the bound controllers (bit p = stock port p) and a frame to one of them through the
 * session core's TX path (keeps the core's bytes out of the stage-2 ring tap) */
uint8_t sxc_bound_mask(void);
void sxc_tx(uint8_t port, const uint8_t *f, uint16_t n);

#endif
