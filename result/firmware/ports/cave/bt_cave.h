/* OpenGrand cave port - Bluetooth robustness and read-only BT module diagnostics on the stock CA79 V2.19
 * firmware (slot payload ota_bt = ota_power2 + this module, feat2 0x01).  No hook site of its own: everything runs from the
 * engine-loop tick (task 24) after the power port, the op arrives through the cave SysEx path.
 *
 *   F1  BLE-MIDI port 2 RX 0x80 latch auto-clear: a latch that lasted 0.5 s (ring empty) is cleared with
 *       the stock reset FUN_40145DFC, IRQ masked around it; a latch that comes back within 5 s doubles
 *       the wait (0.5 / 1 / 2 / 4 / 8 s).
 *   F1b BLE-MIDI port 2 RX ring check (same switch as F1): count == (wr - rd) mod 2 KB must hold; a stock reset
 *       run with IRQs enabled can break it (a byte counted away -> every frame dispatched one frame late, the
 *       ota_main M4 field failure) - repaired with the IRQ masked.
 *   F2b each module command goes out as its first byte, the rest 16 ms later; `SYNTAX ERROR` -> sent once more.
 *   F5  port 2 active-sensing guard: an armed AS counter (1..0xFE) is put back to 0xFF ("never saw 0xFE",
 *       the stock reset value), so a 0xFE sender's 0.34 s gap never unbinds the controller.
 *   F2  BT audio module read-only query: the fixed iWRAP commands `SET` (list the configuration), `LIST`
 *       (active links), `INFO` (version) go out through the stock UART1 TX FUN_4014512C; the lines that
 *       come back are copied from the stock UART1 RX ring (own read index, read only) into a 2 KB buffer
 *       the app reads in chunks.  There is no way to send any other text.
 *   F6  UART1 send discipline (2026-11-01 engine-loop stall
 *       section 1): every module byte goes out as a one-byte FUN_4014512C call made only while the stock TX state is
 *       idle (count 0, rd == wr, IER bit 1 off) - the stock writer's kick race cannot happen then, and the stock
 *       busy-wait is never reached.  A command that cannot go out within 1 s is dropped (counted), never waited on.
 *   F7  BT audio source manager (src/bt/btsrc.c): the module's link / stream lines -> source table -> runtime
 *       commands LIST / SET (read) / CLOSE <l> / CALL <addr> 19 A2DP / SET <l> SELECT + AVRCP PAUSE|PLAY /
 *       A2DP STREAMING STOP; policy RAM only, default SELECT (every source stays connected, the selected one plays -
 *       user requirement 2026-11-02); every module line and command in a line log (op 1D 08);
 *       restarts a wedged module (stock power off / on) only with the RECOVER flag.  The module SETTINGS it can
 *       write are AUTOCALL, only from op 1D 09 (explicit, bound controller, off / restore the line read before),
 *       the class of device, only from op 1D 0A (explicit, bound controller, an audio sink class / restore the class
 *       read before), the AVRCP controller features, only from op 1D 0C (explicit, bound controller, without the
 *       browsing bit / restore the features read before), and the audio routing line, only from op 1D 0D (explicit,
 *       bound controller, one of two fixed EVENT lines / restore the line read before).
 * F1 / F5 can be switched off at run time (op 1D 03, RAM only - both on after every power-on / apply).
 *
 * 0x7D op 0x1D (REMOTE_PROTOCOL.md section 10.6), answered by the next engine-loop pass:
 *   1D 00            status            -> 5D 00 00 <pack7 raw BT_STATUS_RAW>
 *   1D 01 <which>    query the module  -> 5D 01 <rc> <which>   (0 = SET, LIST, INFO in turn; 1 / 2 / 3 one)
 *   1D 02 <off:2>    read text         -> 5D 02 <rc> <state> <total:2> <off:2> <text, <= 48 B, 7-bit>
 *   1D 03 <sw>       switches          -> 5D 03 <rc> <sw>       (bit0 F1, bit1 F5; bound controller only)
 *   1D 04            audio sources     -> 5D 04 00 <pack7 raw BT_SRC_RAW>
 *   1D 04 layout 2 (BT_SRC_RAW): + per device state2 / refusals, the selection, the switch phase / result / time,
 *                                         AUTOCALL state, module reboots / loop guard, what the switch tried, the
 *                                         codec, each device's play status - see btc_sources
 *   1D 05 <act> <pack7 addr:7>         -> 5D 05 <rc> <act>      (0 close, 1 use = select + switch (the one playing
 *                                         is paused, never closed; CALL, resume /
 *                                         PLAY / sink START), 2 call, 3 only this one (close the others), 4 pause, 5
 *                                         play, 6 mute = close its A2DP link only; bound only)
 *   1D 06 [<mode> <flags> <hold_ds> <sweep_s> <has_pref> <pack7 pref:7> [<strategy>]] -> 5D 06 <rc> <the policy, same
 *                                         form + strategy + reconnect + NOR store bits> (strategy: 0 only
 *                                         - another value is refused, rc 1, nothing changes; the reply always 0;
 *                                         without it nothing changes; then <reconnect> BTS_RC_* 0 off / 1 preferred / 2
 *                                         preferred, else last (default) - without it it stays; then <xopt> BTS_X_* (bit 0
 *                                         late stop; at power-on BTS_LATE_STOP_DEFAULT) - without it it stays; then
 *                                         <xwait> the late stop's wait after the paused report, 100 ms units (at
 *                                         power-on BTS_XWAIT_DEFAULT = 10), 0 = that default, max 100
 *                                         - without it it stays.  Reconnect +
 *                                         preferred device are kept on NOR (kvstore KV_KEY_BT_POLICY), the rest is RAM;
 *                                         reply + xopt + xwait = 17 B after rc)
 *   1D 07            restart the module (stock power off, 0.6 s, on)   -> 5D 07 <rc>  (bound only)
 *   1D 08 [<from:2>] module line log    -> 5D 08 00 <first:2> <next:2> <now:4> <records>  (developer view; record =
 *                                         seq:2 t:4 (10 ms) flags len text, flags 1 sent / 2 cut / 4 event)
 *   1D 09 [<op>]     AUTOCALL         -> 5D 09 <rc> <op> <state> <busy op> <last rc> <saved> <len> <restore line>
 *                                         (op 0 state only, 1 read (`SET`), 2 off = `SET CONTROL AUTOCALL`, 3 restore
 *                                         the line read before / the factory line; 2 / 3 bound only - a module setting)
 *   1D 0A [<op> [<class:4>]] class of device -> 5D 0A <rc> <op> <state> <busy op> <last rc> <saved> <class:4>
 *                                         <restore:4> <write:4>  (op 0 state only, 1 read (`SET`), 2 write `SET BT
 *                                         CLASS <hex6>` = <class:4> if given (Audio/Video major + Rendering bit, else rc
 *                                         1), else 240414; 3 restore the class read before / 001f00; then `SET`; 2 / 3
 *                                         bound only; one op at a time with 1D 09; <x:4> = 24 bits as 4 x 7, high first)
 *   1D 0B            the piano's record bus level (read only, any port) -> 5D 0B 00 <pack7 raw BT_LVL_RAW>: one scan
 *                                         of the DSP's two 1152-sample capture buffers in DSP L2 (bt_stock.h) - the
 *                                         mix of piano + BT audio + LINE IN + DSP playback after the record gain, the
 *                                         last ~26 ms.  layout 1: 0 layout, 1 flags BT_LVL_F_*, 2-3 peak slot 0 (L),
 *                                         4-5 peak slot 1 (R), 6-7 mean |x|, 8-9 samples with |x| >= BT_LVL_FLOOR (of
 *                                         2304), 10-11 record gain, 12-13 write pointer - buffer 0, 14-15 samples in
 *                                         the current buffer (u16 little-endian).
 *   1D 0C [<op> [<features>]] AVRCP controller features -> 5D 0C <rc> <op> <state> <busy op> <last rc> <saved>
 *                                         <now:2> <restore:2> <write:2>  (op 0 state only, 1 read (`SET`), 2 write
 *                                         `SET PROFILE AVRCP CONTROLLER <hex2>` = <features> if given (1..0f: categories
 *                                         only, browsing bit 4 clear, else rc 1), else 0f; 3 restore the features read
 *                                         before / 1f (stock factory line); then `SET`; 2 / 3 bound only, only while the
 *                                         module lists the controller-only line (state 1); one op at a time with 1D 09 /
 *                                         1D 0A; applied by the module after its restart (1D 07)
 *                                         <x:2> = 8 bits as 2 x 7, high first)
 *   1D 0D [<op> [<flags>]] audio routing -> 5D 0D <rc> <op> <state> <busy op> <last rc> <saved> <now> <restore> <write>
 *                                         <route> <route dev>  (op 0 state only, 1 read (`SET`), 2 write
 *                                         `SET CONTROL AUDIO INTERNAL I2S_SLAVE EVENT` (flags 1, the default) or
 *                                         `… EVENT KEEPALIVE` (flags 3) - any other flags rc 1; 3 restore the flags read
 *                                         before / 2 (`… KEEPALIVE`, the stock factory line); then `SET`; 2 / 3 bound
 *                                         only, only while the module lists the plain line (state 1); one op at a time
 *                                         with 1D 09 / 0A / 0C; applied by the module after its restart (1D 07); flags
 *                                         bit 0 EVENT, bit 1 KEEPALIVE; route = 1 + link id of the last `AUDIO ROUTE`
 *                                         event (0 none) | 0x40 A2DP, route dev = the device index on that link (0x7F
 *                                         none)
 *   1D 0F            heartbeat, answered AT ONCE on task 19 (readable while the engine loop is stuck)
 *                                      -> 5D 0F 00 <pack7 raw BT_HB_RAW>
 * (<x:2> = 14 bits, high 7 first.) */
#ifndef CA79NG_BT_CAVE_H
#define CA79NG_BT_CAVE_H

#include <stdint.h>

#define BT_OP 0x1D
#define BT_STATUS_LAYOUT 1
#define BT_STATUS_RAW 40      /* layout 1: 20 bytes, then port 2 rd / wr / ring repairs / last difference (28),
                               * then engine-loop passes, TX drops, requests seen / dropped busy, stage, IER (40) */
#define BT_SRC_LAYOUT 2
#define BT_SRC_RAW 120        /* op 1D 04 layout 2: 28-byte header + 4 x 14-byte devices + 35-byte tail (bytes 102.. =
                                * module health / the switch's tries / the codec, 2026-11-02 evening; 112..115 = the play
                                * status of each device, 2026-11-02 night; 116 = reconnect choice + 1 (bits 0-1) | NOR
                                * store bits << 2 (BT_KV_*), 117 garbled status lines, 118 lines read after a garbage
                                * prefix (both stop at 255), 2026-11-09; 119 the late stop's wait (policy xwait);
                                * older apps read 102 / 112 / 116 / 119) */
/* the BT policy on NOR (1D 06 reply byte 16, 1D 04 byte 116 bits 2-5) */
enum { BT_KV_KEPT = 0x01 /* this build keeps recon + the preferred device on NOR */, BT_KV_LOADED = 0x02 /* read back at
       power-on */, BT_KV_PENDING = 0x04 /* a change waits to be written */, BT_KV_FAILED = 0x08 /* the last write failed */ };
#define BT_HB_RAW 16          /* op 1D 0F */
#define BT_LVL_LAYOUT 1
#define BT_LVL_RAW 16         /* op 1D 0B */
#define BT_LVL_SAMPLES 2304u  /* the two DSP capture buffers, int16 each */
#define BT_LVL_FLOOR 16       /* |x| from here counts as signal (-66 dBFS; LINE IN's ADC noise stays below [inferred]) */
/* 1D 0B flags (raw byte 1) */
enum { BT_LVL_F_MOVED = 0x01 /* the DSP's write position changed since the previous 1D 0B: the capture runs */,
       BT_LVL_F_FIRST = 0x02 /* no previous 1D 0B since power-on / apply: MOVED is not known yet */,
       BT_LVL_F_REC = 0x04 /* the DSP record state is not idle (a USB audio recording runs) */,
       BT_LVL_F_WP_ODD = 0x08 /* the write pointer is outside the two buffers: the DSP is not running this code */ };
#define BT_CAP 2048u          /* captured module text */
#define BT_CHUNK 48u          /* text bytes per 1D 02 reply */
#define BT_LOG_CHUNK 128u     /* line-log record bytes per 1D 08 reply */

enum { BT_SW_P2_UNLATCH = 0x01, BT_SW_AS_GUARD = 0x02, BT_SW_ALL = 0x03 };
/* rc of op 0x1D (the power ops' numbering where it overlaps) */
enum { BT_OK = 0, BT_BAD_VALUE = 1, BT_NOT_BOUND = 2, BT_BUSY = 3, BT_MODULE_OFF = 4, BT_RX_LATCHED = 5, BT_TEST_MODE = 6,
       BT_NO_DEVICE = 7, BT_NOT_ON = 8, BT_NO_AVRCP = 9,
       BT_BAD_STATE = 10 /* AUTOCALL op: off needs AUTOCALL read ON, restore needs it read OFF; class op: read the
                          * class first, and it is not already the value to write; AVRCP op: the same, and the
                          * listed line is controller-only; audio op: the same, and the listed line is the plain
                          * form */ };
/* engine-loop stage (heartbeat): what task 24 was doing in the BT port when it last stopped */
enum { BT_STAGE_IDLE = 0, BT_STAGE_TX = 1, BT_STAGE_MODULE_ON = 2, BT_STAGE_MODULE_OFF = 3 };
enum { BT_Q_IDLE = 0, BT_Q_SEND = 1, BT_Q_COLLECT = 2, BT_Q_DONE = 3 };
/* query flags (status byte 3) */
enum { BT_QF_TRUNC = 0x01, BT_QF_RESET = 0x02, BT_QF_LAP = 0x04, BT_QF_ABORT = 0x08, BT_QF_SILENT = 0x10, BT_QF_RETRY = 0x20,
       BT_QF_ECHO_ONLY = 0x40 /* a command came back as its echo only (the module is alive, no answer) */ };

/* task 19 (cave ng_sysex): queue an op 0x1D request; answered by the next btc_tick */
void btc_sysex(uint8_t stock_port, const uint8_t *buf, uint32_t len);
/* task 24 (engine loop, every ~8 ms, after pwc_tick) */
void btc_tick(void);
/* OTA apply-now: 0 = idle (no queued request, no module query on the wire) */
int btc_quiesce(void);
/* OTA apply-now, after every port said idle: hand the connected devices and the selection to the next payload */
void btc_handover(void);
/* the policy's kept part now (bts_saved_encode, BTS_SAVED_RAW bytes) - the startup snapshot's NG part (task 24) */
void btc_ng_get(uint8_t *out);

/* host tests: forget everything (re-initialised on the next btc_tick) */
void btc_restart(void);
#ifdef BTC_HOST
/* host tests only: set the source manager's power-on pin phase (BTS_PIN_*) after the first tick */
void btc_host_pin(uint8_t phase);
#endif
/* host tests / diagnostics: the raw status record of 1D 00, the source table of 1D 04, the heartbeat of 1D 0F */
void btc_status(uint8_t raw[BT_STATUS_RAW]);
void btc_sources(uint8_t raw[BT_SRC_RAW]);
void btc_heartbeat(uint8_t raw[BT_HB_RAW]);
/* op 1D 0B: one scan of the DSP record-bus capture (reads DSP L2 only; remembers the write position for MOVED) */
void btc_level(uint8_t raw[BT_LVL_RAW]);
/* power_cave.c: A2DP links in the source table (bit per link id) */
uint16_t btc_a2dp_links(void);
/* power_cave.c: the AVRCP play status of the source streaming on A2DP link `link` (bts_stream_playing):
 * 1 playing, 0 reports paused / stopped / anything else, -1 not known (also before the first btc_tick) */
int btc_stream_playing(int link);
/* who may change the BT audio (1D 03 / 05 / 06 / 07): the controller bound on `port` (stock port 0..3).
 * core = sxc_ctrl_bound(port): 1 / 0 = the session core's binding table (any bound controller - the app stays
 * allowed while the panel holds the stock binding, field 2026-11-01), < 0 = no session core: the stock binding
 * (binding = port + 1) as before.  Pure */
int btc_ctrl_bound(int core, uint8_t binding, uint8_t port);

#endif
