/* OpenGrand — Bluetooth audio activity model and the APO hold rule.
 *
 * Feature 1 of the plan (기능 단계):
 * the piano must not switch itself off while Bluetooth audio is playing.
 *
 * Inputs (both come from the BT audio module on UART1, iWRAP dialect on this
 * instrument, 실기 결과 C4/C5):
 *   - module lines, CR/LF stripped:  READY.
 *                                    CONNECT <link> A2DP <psm>
 *                                    RING <link> <addr> <psm> A2DP|AVRCP
 *                                    A2DP STREAMING START <link> / STOP <link>
 *                                    NO CARRIER <link> ERROR <code> [text]
 *     every other line (AVRCP ..., A2DP CODEC ..., unknown) is ignored;
 *   - the stock A2DP link flag (0x413AE5B8, ANDed with module power 0x4139E7F0
 *     by the port): it follows the real link, but a TV keeps the link (and even
 *     the stream) open while silent, so it is only used for link loss.
 *
 * States: idle (no A2DP link), connected (link, no stream), streaming. */
#ifndef CA79NG_POWER_H
#define CA79NG_POWER_H

#include <stdbool.h>
#include <stdint.h>

#include "ca79ng/system.h"

typedef enum { BT_AUDIO_IDLE = 0, BT_AUDIO_CONNECTED = 1, BT_AUDIO_STREAMING = 2 } bt_audio_state_t;

#define BT_LINE_MAX 96

typedef struct {
    uint16_t a2dp_links;   /* bit per iWRAP link id (0..15) seen in CONNECT/RING ... A2DP */
    bool streaming;
    int8_t stream_link;    /* link id of the last STREAMING START, -1 unknown */
    bool link_flag;        /* last stock link flag (falling edge = link loss) */
    uint32_t starts;       /* STREAMING START count (edge for the APO hold) */
    int8_t play;           /* the streaming source's own player as its AVRCP play status says (bt_audio_play):
                            * 1 playing, 0 reports paused / stopped / other, -1 not known (no AVRCP status) */
    uint32_t lines, ignored;
    /* byte assembler (bt_audio_rx) */
    char buf[BT_LINE_MAX];
    uint8_t len;
    bool overflow;
} bt_audio_t;

void bt_audio_init(bt_audio_t *b);
/* One module line without CR/LF; returns true if it changed the model.
 * STREAMING STOP <link> for a link other than the one that started the stream (both known) is ignored: with several
 * sources connected (btsrc) a refused source's STOP must not end the playing one's hold. */
bool bt_audio_line(bt_audio_t *b, const char *line, uint16_t len);
/* Raw module bytes (RX ring); lines end at CR or LF, shorter than 2 chars are dropped
 * like the stock assembler (FUN_40145C44), longer than BT_LINE_MAX are dropped. */
void bt_audio_rx(bt_audio_t *b, const uint8_t *bytes, uint16_t n);
/* Stock A2DP link flag && BT audio module power. A 1 -> 0 edge drops links and stream. */
void bt_audio_link(bt_audio_t *b, bool up);
/* The AVRCP play status of the source that streams (the port reads it from the source manager, btsrc
 * bts_stream_playing; nothing sets it on a build without one): 1 playing, 0 paused / stopped / other, -1 not known.
 * Only the APO hold uses it. */
void bt_audio_play(bt_audio_t *b, int playing);
bt_audio_state_t bt_audio_state(const bt_audio_t *b);
const char *bt_audio_state_name(bt_audio_state_t s);

/* APO hold policy (OpenGrand setting, owner decision).
 * The rule (owner decision 2026-11-02): while the BT audio stream is open
 * (A2DP STREAMING START .. STOP) the piano does not auto-off - up to max_hold_ms; when the stream closes the normal
 * APO (menu time) counts from then; when max_hold_ms is reached the APO acts right away (standby / off as the policy
 * says), unless the piano was used within the menu time (keys, panel, app commands reset the APO as always - the
 * hold stops the expiry, not the clock).  Each new STREAMING START restarts the max_hold_ms window.
 * Play status (owner decision 2026-11-03): a stream whose source reports via AVRCP that it is not
 * playing (paused, stopped, ...) does not hold - Windows keeps its stream open for minutes after a pause.  That report
 * ends the hold exactly as a STOP (the full menu time from then); PLAYING again with the stream still open opens a new
 * hold like a new START (a new max_hold_ms window).  No status known (no AVRCP, no notification support): the stream
 * alone holds, as before.
 *   max_hold_ms   0                 rule off: stock behaviour, BT audio never holds APO
 *                 APO_HOLD_FOREVER  hold as long as the stream is open (a paused TV never lets go)
 *                 otherwise         hold at most this long per STREAMING START
 *   hold_connected                  also hold while only connected (no stream) — off by default,
 *                                   the link flag alone keeps a TV-connected piano on forever */
#define APO_HOLD_FOREVER 0xFFFFFFFFu
#define APO_MAX_HOLD_DEFAULT_MS (5u * 60u * 60u * 1000u) /* 5 h (owner decision 2026-11-02) */

typedef struct {
    uint32_t max_hold_ms;
    bool hold_connected;
} apo_policy_t;

typedef struct {
    apo_policy_t pol;
    bool holding;          /* this tick the hold kept the APO from expiring */
    bool capped;           /* max hold used up for the current stream */
    bool released;         /* this tick the hold ended before its cap (STOP, link loss, rule off): the APO
                            * counter was reset - the full menu time counts from here */
    uint32_t held_ms;      /* time held since the current stream (or link) started */
    uint32_t seen_starts;  /* bt_audio_t.starts at the current hold */
} apo_hold_t;

void apo_policy_default(apo_policy_t *pol);
void apo_hold_init(apo_hold_t *h, const apo_policy_t *pol);
/* Replaces power_timer_tick(). While held the idle counter keeps counting (activity resets it as before:
 * keys, MIDI, pedals, PowerManagementTimerReset call power_timer_activity()) but is kept at the menu limit
 * instead of expiring; at STREAMING STOP / link loss it is reset (the full APO time from there); at the cap the
 * normal APO applies at once - an idle piano expires on that tick.  Returns true when APO expires. */
bool apo_tick(apo_hold_t *h, power_timer_t *t, const bt_audio_t *bt, uint32_t elapsed_ms);

/* ---- OpenGrand power policy: the user setting ----
 * Owner decisions 2026-10-31: APO expiry enters partial standby instead of the real
 * off, max hold 2 h (5 h since 2026-11-02), "connected only" does not hold, standby cap 8 h. Kept in the
 * kvstore (KV_KEY_POWER_POLICY) and set over the 0x7D op 0x18 (REMOTE_PROTOCOL §10.4).
 *
 * Wire / flash form, POWER_POLICY_RAW bytes:
 *   0     layout version (POWER_POLICY_VERSION)
 *   1     flags: bit0 APO expiry -> standby (0 = real off, stock), bit1 hold while only connected,
 *         bit2 standby switches the BT audio module off too (0 = it stays on and an A2DP STREAMING
 *         START wakes the piano: "TV wake"; ota_power ignores the bit
 *         and always switches it off), bit3 the piano's POWER button keeps the stock behaviour (a press
 *         released after ~0.3 s really powers off) - 0 (default) = the button press (F-A1): released after 0.3-4 s from ON -> partial standby, held > 4 s -> the stock
 *         real off; payloads before F-A1 ignore the bit (and answer it as 0)
 *   2..3  max hold, minutes u16 LE: 0 = rule off (stock), 1..POWER_HOLD_MAX_MIN, 0xFFFF = unlimited
 *   4..5  standby cap, minutes u16 LE: POWER_CAP_MIN_MIN..POWER_CAP_MAX_MIN, POWER_CAP_NONE (0xFFFF,
 *         the same convention as the unlimited hold) = no cap: standby lasts until something wakes
 *         the piano (owner decision 2026-11-01 - the piano is played every ~3 days and must stay
 *         remotely wakeable). Payloads before ota_power3
 *         reject 0xFFFF (rc 1, nothing changed)
 *   6..7  reserved, 0 */
#define POWER_POLICY_RAW 8
#define POWER_POLICY_VERSION 1
#define POWER_FLAG_APO_STANDBY 0x01
#define POWER_FLAG_HOLD_CONNECTED 0x02
#define POWER_FLAG_STANDBY_BT_OFF 0x04
#define POWER_FLAG_BUTTON_STOCK 0x08
#define POWER_HOLD_MAX_MIN 1440u
#define POWER_HOLD_UNLIMITED 0xFFFFu
#define POWER_CAP_MIN_MIN 30u
#define POWER_CAP_MAX_MIN 1440u
#define POWER_CAP_NONE 0xFFFFu
#define POWER_CAP_DEFAULT_MS (8u * 60u * 60u * 1000u) /* = STANDBY_CAP_DEFAULT_MS */

typedef struct {
    apo_policy_t hold;
    bool apo_standby;        /* APO expiry enters partial standby instead of the real off */
    uint32_t standby_cap_ms; /* standby length before the real off, 0 = no cap (POWER_CAP_NONE) */
    bool standby_bt_off;     /* standby switches the BT audio module off (no TV wake) */
    bool button_stock;       /* the POWER button keeps the stock real off (no F-A1 standby) */
} power_policy_t;

void power_policy_default(power_policy_t *pol);
void power_policy_encode(const power_policy_t *pol, uint8_t raw[POWER_POLICY_RAW]);
/* Validates (version, ranges); on false *pol is left unchanged. */
bool power_policy_decode(power_policy_t *pol, const uint8_t *raw, uint16_t n);

/* ---- remote off (feature 2 of the plan) ----
 * 0x7D op 19 03 from a bound controller sends the piano where an APO expiry sends it (policy bit0: partial
 * standby, else the real off). It is refused - never queued - while something would be cut short:
 *   recording   the recorder is armed, counting in or recording (stock 0x4191EF79 = 3..6): the standby cleanup
 *               (FUN_400B0EA8) would commit the take and queue its RecorderData job behind the user's back;
 *   file op     a file-executor operation (KSO import / USB song load, 0x413CA3EA != 0) is running;
 *   flash       a NOR job is queued or the worker is programming (FUN_401342AC / 0x4139DAC4, or a stock writer
 *               owns the chip) - this piano's model class 1 executor does not wait for the job queue.
 * The first reason found, in that order (what the user can act on first). */
enum { POWER_OFF_OK = 0, POWER_OFF_RECORDING = 1, POWER_OFF_FILE = 2, POWER_OFF_FLASH = 3 };
uint8_t power_remote_off_check(bool recording, bool file_op, bool flash_busy);

/* ---- APO test shortcut 19 04 ----
 * Field tests of the auto power-off without waiting for the menu time: the APO is put where it expires in s seconds, as if
 * the menu time were almost over, and everything after that is the normal path (activity resets it to the full menu
 * time, the BT stream hold holds it, the expiry goes where the policy says).  RAM only - nothing is saved.  s = 0 puts
 * the full menu time back. */
#define POWER_APO_TEST_MIN_S 10u
#define POWER_APO_TEST_MAX_S 600u
static inline bool power_apo_test_secs_ok(uint32_t s)
{
    return s == 0 || (s >= POWER_APO_TEST_MIN_S && s <= POWER_APO_TEST_MAX_S);
}

/* ---- which received SysEx frames are APO activity ----
 * Only real user activity keeps the piano on.  A controller that stays connected polls (status, lists, gets, ACKs);
 * those frames are not activity.  The stock firmware resets the APO counter on every MIDI byte < 0xF8
 * (FUN_40148B24 -> FUN_400ACB84), so the cave undoes the reset of a pump pass that carried only such frames
 * (power_cave.c pwc_midi_tail); the host core never counts them in the first place (piano.c: sets, commands,
 * channel messages and keys only).  Inline and call-free: the cave runs it inside the task-19 leaf bodies.
 *
 * power_frame_quiet(m, n): m = a whole frame F0 .. F7 as the stock end handler hands it on.  true = a poll / read:
 *   Kawai (F0 40 dev fn ...): fn 02 request (get), 03 data end, 04 / 05 wait / cancel, 06 / 07 NACK / ACK, 60
 *     identity; fn 7E with v1 v2 = 01 01 CONNECT, 01 00 DISCONNECT, 05 40 / 41 / 42 model / region / language,
 *     04 41 version; fn 24 with v1 = 11 list, 7C wait, 7E NAK, 7F ACK (the files tab asks for the list when it
 *     opens).  Everything else (fn 10 sets, 31 / 32 commands incl. the panel
 *     touch 32 51 39, other 24 file operations, 09 staging, other 7E) is activity.
 *   OpenGrand (F0 7D 43 41 37 39 op [sub] ...): 01 identify, 03 status, 17 (GPIO diagnostic, every form), 18 00, 19 00, 1A, 1B reads (all but 1B 01),
 *     1C 00, 1D 00 / 01 / 02 / 04 / 08 / 0B / 0F, 1D 06 without arguments and 1D 09 [00 | 01] (reads), 1E 00 / 01, the panel's relay
 *     replies 70-7F.  Everything else (policy / standby / wake / remote off, 1B 01, 1C / 1D / 1E changes, relay
 *     0x30-0x3F, unknown ops) is activity.  A bare op (no sub byte) is its read form where one exists.
 *   Any other frame: activity. */
static inline __attribute__((always_inline)) bool power_frame_quiet(const uint8_t *m, uint32_t n)
{
    if (n < 5 || m[0] != 0xF0)
        return false;
    if (m[1] == 0x40) {
        uint8_t fn = m[3];
        if (fn == 0x02 || fn == 0x03 || fn == 0x04 || fn == 0x05 || fn == 0x06 || fn == 0x07 || fn == 0x60)
            return true;
        if (fn == 0x7E && n >= 9) {
            uint8_t v1 = m[6], v2 = m[7];
            return (v1 == 0x01 && (v2 == 0x01 || v2 == 0x00)) || (v1 == 0x05 && v2 >= 0x40 && v2 <= 0x42) ||
                   (v1 == 0x04 && v2 == 0x41);
        }
        if (fn == 0x24 && n >= 8) {
            uint8_t v1 = m[6];
            return v1 == 0x11 || v1 == 0x7C || v1 == 0x7E || v1 == 0x7F;
        }
        return false;
    }
    if (m[1] == 0x7D && n >= 8 && m[2] == 0x43 && m[3] == 0x41 && m[4] == 0x37 && m[5] == 0x39) {
        uint8_t op = m[6];
        bool bare = n == 8;                       /* F0 7D C A 7 9 op F7 */
        uint8_t sub = bare ? 0 : m[7];
        bool only_sub = n == 9;                   /* F0 .. op sub F7: no argument after the sub byte */
        if (op == 0x01 || op == 0x03 || op == 0x17 || op == 0x1A || (op >= 0x70 && op <= 0x7F))
            return true;
        if (op == 0x18 || op == 0x19 || op == 0x1C || op == 0x1E)
            return sub == 0x00 || (op == 0x1E && sub == 0x01);
        if (op == 0x1B)
            return sub != 0x01;
        if (op == 0x1D)
            return sub == 0x00 || sub == 0x01 || sub == 0x02 || sub == 0x04 || sub == 0x08 || sub == 0x0B || sub == 0x0F ||
                   (sub == 0x06 && only_sub) || ((sub == 0x09 || sub == 0x0A || sub == 0x0C || sub == 0x0D) && (only_sub || (n == 10 && m[8] <= 0x01)));
        return false;
    }
    return false;
}

#endif
