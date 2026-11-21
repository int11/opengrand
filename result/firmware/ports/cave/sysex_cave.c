/* OpenGrand cave port - Phase 3-1 SysEx front on the stock firmware.  See sysex_cave.h. */
#include "sysex_cave.h"

#include <stdbool.h>
#include <string.h>

#include "ca79ng/kpfront.h"
#include "ca79ng/util.h"
#include "hal_cave.h"
#include "sysex_stock.h"
#if CA79NG_SESSION
#include "ca79ng/kpmetro.h"
#include "ca79ng/kpsess.h"
#include "ca79ng/kpstate.h"
#include "power_cave.h"
#include "power_stock.h"
#include "session_stock.h"
#include "fread_cave.h"
#include "startup_cave.h"
#include "recctl_cave.h"
#include "ca79ng/kpchan.h"
#include "cave_stock.h"
#include "chan_stock.h"
#include "ca79ng/kpgate.h"
#endif
/* CA79NG_SESSION is the one build switch left in this port: 1 = the slot payload ota_main (the session core and
 * everything on it - startup settings, playback / record control, MIDI channel, intake gate, S3 / S4, the BT port's
 * pwc_on), 0 = the host test of the bare Phase 3-1 front (tests/test_sysex_cave.c).  The per-feature switches
 * CA79NG_STARTUP / REC / REC3 / MIDICH / GATE / S3 / S4 and PWC_BTSRC were folded into it after their field tests;
 * fold step 3 (2026-11-21, ngver 3) removed the runtime off-switches too: S3 / S4 / the MIDI channel judge are on from
 * init() and no op turns them off (the set forms 1B 05 04 / 1B 05 06 / 1B 07 <x> answer rc 1 with the record). */

#define S4_FRAME_MAX (16u + KP_PACKED_MAX)   /* F0 40 cc 10 08 02 5F 00 part + packed body (<= 0xF5) + F7, room to spare */

#define GUARD_LO 0x5E1F0B31u
#define GUARD_HI 0x31B0F1E5u

/* ---- RW state in R1 (core RW region, zeroed on the first tick, never loaded) -------------- */
typedef struct {
    uint32_t port_seen[4];   /* Kawai frames per stock port (all of them, owned or passed) */
    uint16_t connects[4];    /* 7E 01 01 per port */
    uint8_t disconnects[4];  /* 7E 01 00 per port (saturating) */
    uint8_t last_connect;    /* stock port + 1 of the last CONNECT, 0 none */
    uint8_t binding_at;      /* the stock binding when that CONNECT arrived (before the stock handled it) */
} sx_sess_t;

typedef struct {
    uint32_t guard_lo;
    param_store_t store;   /* the mirror: no authority until Phase 3-4 */
    kpf_t front;
    sx_sess_t sess;        /* op 0x1B 02: who talks on which port (second field failure) */
#if CA79NG_SESSION
    kps_t core;            /* Phase 3-2 stage 1: bindings, CONNECT, requests, ACK assignment */
    kps_clock_t clk;       /* 32-bit ms from the 2 ms counter                                             */
    uint32_t pumps;        /* pump-tail calls                                                             */
    /* stage 2: the stock binding's TX ring as far as the core has read it */
    uint8_t tap_rep;       /* stock binding being followed (port + 1), 0 none                            */
    uint16_t tap_pos;      /* next ring index to read                                                     */
    uint32_t tap_bytes;    /* stock bytes read                                                            */
    /* Phase 3-4 S1: the store filled from the stock getter, compare passes on request */
    kst_t state;
    /* Phase 3-5 stage 1: metronome sets judged by the core, BeatCounter made by the core */
    kpm_t metro;
    uint32_t metro_sent;   /* BeatCounter frames the core sent (one per port)                             */
    uint32_t metro_miss;   /* judged block != the stock block after the apply                             */
    /* fn 0x31 / 0x32 commands of any bound controller: run through the stock apply */
    uint32_t cmds;         /* commands the core ran                                                        */
    uint32_t cmds_other;   /* of them, from a bound controller that is not the stock binding (the stock drops) */
    /* Phase 3-4 S2: periodic compare passes, paused while something changes the settings */
    struct {
        uint32_t next;     /* core ms: the next pass is due                                                 */
        uint32_t pass0;    /* kst passes when our walk started                                              */
        uint8_t walking;   /* our walk is the running one                                                   */
        uint8_t pause;     /* S2_P_* bits of the last check                                                 */
        uint8_t rec_prev;  /* recorder state seen last (0x4191EF79)                                         */
        uint8_t on_prev;   /* piano ON (not standby) seen last                                              */
        uint32_t passes;   /* periodic passes finished                                                      */
        uint32_t aborts;   /* periodic walks stopped by a pause                                             */
        uint32_t postponed;/* due passes put off by a pause                                                 */
    } s2;
    /* Phase 4: MIDI channel messages judged by the core */
    struct {
        uint32_t pc[3];    /* Program Change judged: kch_act_t drop / stock / setting                         */
        uint32_t cc[3];    /* Control Change judged                                                            */
        uint32_t applies;  /* delayed applies the core made (FUN_4016053C(c, x, 0x46))                         */
        uint32_t passes;   /* delayed-apply passes the core ran (armed)                                        */
        uint8_t last[3];   /* the last setting key read back (v1 v2 part)                                      */
        uint8_t msg[3];    /* the last judged message: status, data 1, data 2                                  */
        uint8_t act;       /* its judgement                                                                    */
    } mc;
    /* Phase 3-4 S4: the set as the core judged it, the frame the stock applies */
    struct {
        kst_s4_set_t set;
        uint8_t buf[S4_FRAME_MAX];
        uint32_t staged;   /* sets left to the S3 path: staging flag on (the stock keeps them unapplied, fn 0x03 later) */
        uint32_t big;      /* sets left to the S3 path: longer than buf                                               */
        uint32_t unread;   /* listed own keys the stock gave no value for after the apply                             */
    } s4;
#endif
#if CA79NG_SESSION
    kpg_t gate;            /* Phase 4 A intake gate: the rest of FUN_4014A2C8's functions */
#endif
    uint32_t guard_hi;
} sx_rw_t;

CAVE_RW static sx_rw_t rw;

/* ---- small state in the payload .data (copied fresh with the payload) --------------------- */
static volatile uint8_t ready = 0;     /* task 24 -> task 19: rw initialised */
static uint8_t hooked = 0;
static volatile unsigned char *inflight = 0;
#if CA79NG_SESSION
static uint8_t cur_port = 0;           /* the port of the frame the front judges (task 19 only)   */
static uint8_t pump_hooked = 0;
static uint8_t resumed = 0;            /* kps_resume done (first pump pass after init) */
static uint8_t mc_hooked;              /* Phase 4 MIDI channel sites attached (defined with mcc_on below) */
#endif

/* ---- kpf_io_t on the stock firmware ---------------------------------------------------------- */
#if CA79NG_SESSION
/* the front's "bound" = the session core's table for the frame's port */
static uint8_t io_binding(void *c) { (void)c; return kps_bound(&rw.core, cur_port) ? (uint8_t)(cur_port + 1) : 0; }
#else
static uint8_t io_binding(void *c) { (void)c; return stk_binding; }
#endif
static uint32_t io_device(void *c) { (void)c; return stk_dev_id; }
static bool io_forward(void *c) { (void)c; return stk_port3_fwd != 0; }
static bool io_staging(void *c) { (void)c; return stk_stage_flag != 0; }

#if CA79NG_SESSION
/* ---- stage 2: the stock binding's TX ring -------------------------------- */
static uint16_t tx_wr(uint8_t p)
{
    switch (p) {
    case 0: return (uint16_t)(stk_txwr0 & STK_TXRING_MASK);
    case 1: return (uint16_t)(stk_txwr1 & STK_TXRING_MASK);
    case 2: return (uint16_t)(stk_txwr2 & STK_TXRING_MASK);
    default: return (uint16_t)(stk_txwr3 & STK_TXRING_MASK);
    }
}

static uint8_t tx_byte(uint8_t p, uint16_t i)
{
    switch (p) {
    case 0: return stk_txring0[i];
    case 1: return stk_txring1[i];
    case 2: return stk_txring2[i];
    default: return stk_txring3[i];
    }
}

/* hand the stock's bytes [tap_pos, end) of the followed port to the session core */
static void tap_upto(uint16_t end)
{
    uint8_t p = (uint8_t)(rw.tap_rep - 1), b[32];
    while (rw.tap_pos != end) {
        uint32_t n = 0;
        while (rw.tap_pos != end && n < sizeof b) {
            b[n++] = tx_byte(p, rw.tap_pos);
            rw.tap_pos = (uint16_t)((rw.tap_pos + 1u) & STK_TXRING_MASK);
        }
        rw.tap_bytes += n;
        kps_tap(&rw.core, p, b, n);
    }
}

/* task 19 (pump tail): follow the stock binding; a new one is read from its write index on */
static void tap_follow(void)
{
    uint8_t b = stk_binding;
    if (b > KPS_PORTS)
        b = 0;
    if (b != rw.tap_rep) {
        rw.tap_rep = b;
        if (b)
            rw.tap_pos = tx_wr((uint8_t)(b - 1));
        kps_tap_reset(&rw.core);
        return;
    }
    if (b)
        tap_upto(tx_wr((uint8_t)(b - 1)));
}
#endif

#if CA79NG_SESSION
/* ---- Phase 3-5 metronome: the stock RAM kpmetro.h names (KPM_A_*, REA literal pools), read only ----------- */
#define KST_CALLS 4u    /* stock value reads (getter + builder) per pump-tail pass */
#define S2_PERIOD_MS 5000u   /* Phase 3-4 S2: one compare pass every 5 s (a pass ~0.4 s) */
#define S2_RETRY_MS 1000u    /* a pass put off / stopped by a pause: try again after 1 s                  */
#define S3_SOON_MS 200u      /* S3 on and the store not trusted: the next pass this soon  */
/* op 1B 05 02 byte 2: why the periodic compare waits */
enum { S2_P_OFF = 0x01, S2_P_FAN = 0x02, S2_P_STAGE = 0x04, S2_P_STARTUP = 0x08, S2_P_FLASH = 0x10, S2_P_REC = 0x20,
       S2_P_POWER = 0x40, S2_P_WALK = 0x80 };

static uint8_t rd8(uint32_t a) { return *(const volatile uint8_t *)a; }
static int16_t rd16(uint32_t a) { return (int16_t)(rd8(a) | (uint16_t)rd8(a + 1u) << 8); }

/* settings block +0x49..+0x50: on, mode, rhythm, beat index, pad, tempo u16 (LE), volume */
static void metro_cfg(kpm_cfg_t *c)
{
    uint32_t a = KPM_A_CFG;
    __asm__("" : "+r"(a));   /* keep one base register + byte offsets (GCC would load each address separately) */
    const volatile uint8_t *b = (const volatile uint8_t *)a;
    memset(c, 0, sizeof *c);
    c->on = b[0];
    c->mode = b[1];
    c->rhythm = b[2];
    c->beat = b[3];
    c->tempo = (uint16_t)(b[5] | (uint16_t)b[6] << 8);
    c->volume = b[7];
}

/* BeatCounter to every core-bound controller the stock did not send it to (FUN_4014BF24 -> FUN_4014AD04: the
 * stock binding's port, + 0..2 when binding 4 forwards).  fn 0x32 is never ACKed: at once, not queued. */
static void metro_send(const uint8_t f[KPM_FRAME_LEN])
{
    uint8_t ex = kpm_stock_ports(stk_binding, stk_port3_fwd != 0);
    rw.metro_sent += kps_relay(&rw.core, ex, f, KPM_FRAME_LEN);
}

/* task 19 pump tail: the stock run state -> the beats task 20 sent its binding since the last pass */
static void metro_poll(void)
{
    kpm_run_t r;
    uint8_t out[2][KPM_FRAME_LEN];
    uint32_t n;
    r.pos = rd16(KPM_A_POS);
    r.len = rd16(KPM_A_LEN);
    r.half = rd16(KPM_A_HALF);
    r.beat = (int8_t)rd8(KPM_A_BEAT);
    r.bpb = rd8(KPM_A_BPB);
    r.on = rd8(KPM_A_ON);
    r.reload = rd8(KPM_A_RELOAD);
    r.rec_state = rd8(KPM_A_REC_STATE);
    n = kpm_poll(&rw.metro, &r, out);
    for (uint32_t k = 0; k < n; k++)
        metro_send(out[k]);
}

/* before a judged set: the poll first, then the block as the stock has it now.  The set arrives in task 19 ahead
 * of this pass's pump tail, so a beat task 20 made since the last pass has not been polled yet - without this the
 * set's resynchronisation would swallow it (field order: beat n, then the set's BeatCounter 1) */
static void metro_begin(void)
{
    metro_poll();
    metro_cfg(&rw.metro.cfg);
}

/* a set the front is about to apply (frame m, n bytes): judge its metronome keys in order (single or packed
 * 10 5F 00) on the stock block as it is now.  fn 0x31 / 0x32 commands: only the two that are metronome keys -
 * 32 56 08 (descriptor 0xB1) and 31 56 0A (0xB2), whose apply cases are 0x70's and 0x75's (REA: jump table
 * 0x40160890, case 0xB1 0x4016370C = strb d0,[blk+0x49]; bl FUN_4016E6BC, case 0xB2 0x40160B84 = 0x75's); the
 * other fn 0x31 / 0x32 keys of 56 have no descriptor (the stock lookup drops them, no apply).
 * 0 = no metronome key; else bit 31 + the number of BeatCounter 1 frames the apply will send. */
/* noinline (with metro_after): their buffers must not sit in io_apply's frame while the stock apply runs -
 * roundtrip_sysex.py 5 bounds the payload's extra stack at FUN_4014EBFC to 128 B */
__attribute__((noinline)) static uint32_t metro_judge(const uint8_t *m, uint32_t n)
{
    uint32_t beat1 = 0, end = n - 1, i = 9;
    kpm_result_t r;
    uint8_t f[KPM_FRAME_LEN];
    bool any = false;
    if (n < 10)
        return 0;
    if (m[3] != 0x10) {
        if (!((m[3] == 0x32 && m[7] == 0x08) || (m[3] == 0x31 && m[7] == 0x0A)) || !kpm_is_key(m[6], m[7], m[8]))
            return 0;
        metro_begin();
        return kpm_set(&rw.metro, m[7], m + 9, n - 10, &r, f) ? 0x80000001u : 0x80000000u;
    }
    if (m[6] == 0x5F && m[7] == 0x00) {
        while (i < end) {
            uint8_t l = m[i];
            if (l < 4 || i + l > end)
                break;
            if (kpm_is_key(m[i + 1], m[i + 2], m[i + 3])) {
                if (!any)
                    metro_begin();
                any = true;
                if (kpm_set(&rw.metro, m[i + 2], m + i + 4, (uint32_t)l - 4, &r, f))
                    beat1++;
            }
            i += l;
        }
    } else if (kpm_is_key(m[6], m[7], m[8])) {
        any = true;
        metro_begin();
        if (kpm_set(&rw.metro, m[7], m + 9, n - 10, &r, f))
            beat1++;
    }
    return any ? (beat1 | 0x80000000u) : 0;
}
#endif

#if CA79NG_SESSION
/* after the stock apply of a set with metronome keys (metro = metro_judge's answer) */
__attribute__((noinline)) static void metro_after(uint32_t metro)
{
    kpm_cfg_t now;
    const kpm_cfg_t *j = &rw.metro.cfg;
    uint8_t f[KPM_FRAME_LEN];
    metro_cfg(&now);
    if (now.on != j->on || now.mode != j->mode || now.rhythm != j->rhythm || now.beat != j->beat ||
        now.tempo != j->tempo || now.volume != j->volume)   /* field by field: no struct padding compared */
        rw.metro_miss++;   /* the judgement and the stock apply disagree (field check, op 1B 05) */
    /* the store holds what the getter answers, not the raw set bytes the front mirrored (refused beat / rhythm) */
    kpm_store(&rw.metro.cfg, &rw.store);
    kpm_beat_frame(1, f);
    for (uint32_t k = metro & 0xFFu; k; k--)
        metro_send(f);   /* the apply's own BeatCounter 1 went to the stock binding (FUN_4016E668 / FUN_4016EF68) */
}
#endif

static void io_tx(void *c, uint8_t stock_port, const uint8_t *msg, uint16_t len)
{
#if CA79NG_SESSION
    /* the core's own frames on the followed port are not the stock's: skip exactly them.  The writer
     * holds the port's semaphore for the whole call and task 19 is the highest writer, so they end at
     * the write index read right after it (a stock frame half written when we started is completed
     * first, inside our wait for the semaphore). */
    bool tapped = ready && rw.tap_rep == (uint8_t)(stock_port + 1);
    uint16_t w0 = tapped ? tx_wr(stock_port) : 0, w1;
    (void)c;
    hal_midi_tx(cave_port_from_stock(stock_port), msg, len);
    if (!tapped || rw.tap_rep != (uint8_t)(stock_port + 1))
        return;
    w1 = tx_wr(stock_port);
    if (((uint32_t)(w1 - w0) & STK_TXRING_MASK) >= len) {   /* else nothing of ours went in */
        tap_upto((uint16_t)((w1 - len) & STK_TXRING_MASK));
        rw.tap_pos = w1;
    }
#else
    (void)c;
    hal_midi_tx(cave_port_from_stock(stock_port), msg, len);
#endif
}

#if CA79NG_SESSION
static int ss_key_value(void *c, uint8_t v1, uint8_t v2, uint8_t part, uint8_t k, uint8_t out[KPS_FRAME_MAX]);

/* S4 after the apply: every listed key read back from the stock (the stage-2 notify bytes, getter in the stock binding's
 * ctx) - own keys must equal what the core wrote, refused ones give the store back the stock's value */
__attribute__((noinline)) static void s4_follow(void)
{
    uint8_t f[KPS_FRAME_MAX];
    kst_s4_set_t *r = &rw.s4.set;
    for (uint32_t k = 0; k < r->n; k++) {
        int fn = ss_key_value(0, r->key[k][0], r->key[k][1], r->key[k][2], 0, f);
        if (fn > 0) {
            kst_s4_follow(&rw.state, r, k, f, (uint32_t)fn);
        } else if (r->own[k]) {
            rw.s4.unread++;
            r->pure = false;   /* nothing to check it against: the S3 rule */
        }
    }
}

/* S4 write owner: the set judged value by value first; the stock applies what
 * the core accepted (the set itself, or the set without its refused values), then the read-back and the trust decision.
 * Everything else as io_apply below (metronome, stage-2 fan-out, recorder commands, the stock binding's ctx).  noinline:
 * its locals stay out of io_apply's frame (roundtrip_sysex.py 5 / 8 bound the extra stack at FUN_4014EBFC) */
__attribute__((noinline)) static void s4_apply(uint8_t ctx_id, const uint8_t *m, uint16_t n)
{
    uint32_t len = kst_s4_judge(&rw.state, m, n, rw.s4.buf, &rw.s4.set), metro;
    const uint8_t *f = rw.s4.buf;
    if (len == 0) {   /* every value refused: nothing reaches the stock; the read-back undoes the front's mirror */
        s4_follow();
        kst_s4_done(&rw.state, &rw.s4.set);
        return;
    }
    metro = metro_judge(f, len);
    kps_set_begin(&rw.core, (uint8_t)(ctx_id - 1), f, len);
    if (stk_binding != 0)
        ctx_id = stk_binding;
    if (!rcc_kawai(cur_port, f, len))
    stk_param_set(ctx_id, 0x10, f + 6, len);
    kps_set_end(&rw.core);
    if (metro)
        metro_after(metro);
    s4_follow();
    kst_s4_done(&rw.state, &rw.s4.set);
}
#endif

static void io_apply(void *c, uint8_t ctx_id, const uint8_t *body, uint16_t n)
{
#if CA79NG_SESSION
    uint32_t metro;
#endif
    (void)c;
#if CA79NG_SESSION
    /* S4 write owner (always on): staged / oversized sets stay on the S3 path below */
    if (stk_stage_flag != 0)
        rw.s4.staged++;   /* the stock holds staged values unapplied (FUN_4015C410): nothing to read back yet */
    else if (n > S4_FRAME_MAX)
        rw.s4.big++;
    else {
        s4_apply(ctx_id, body - 6, n);
        return;
    }
#endif
#if CA79NG_SESSION
    /* Phase 3-5: the core judges the metronome keys before the stock applies them (kpmetro.h) */
    metro = metro_judge(body - 6, n);
    /* stage 2: the values this set changes go to the other bound controllers (body = frame + 6) */
    kps_set_begin(&rw.core, (uint8_t)(ctx_id - 1), body - 6, n);
    /* applied in the stock binding's ctx (the controller that CONNECTed last), whoever sent it */
    if (stk_binding != 0)
        ctx_id = stk_binding;
#endif
#if CA79NG_SESSION
    /* Phase 3-6 R1: playback commands are the core's (recctl_cave.h) - the same stock routines, judged first */
    if (!rcc_kawai(cur_port, body - 6, n))   /* cur_port = the sender (sxc_kawai) */
#endif
    stk_param_set(ctx_id, 0x10, body, n);
#if CA79NG_SESSION
    kps_set_end(&rw.core);
    if (metro)
        metro_after(metro);
    kst_event(&rw.state, KST_WHY_SET);   /* S2: what the apply changed besides the set's own keys */
#endif
}

#if CA79NG_SESSION
/* a command F0 40 dev 31|32 08 02 v1 v2 part .. F7 the stock would run for its binding only:
 * FUN_4014A2C8 (0x4014A2C8, fn 0x31 / 0x32 branch) = `if (FUN_40151BCC(port) == 0) return;
 * FUN_4014EBFC(FUN_4015B470(port), fn, msg + 6, len)` - no ACK, silent for every other port.  VT edit (d0 0x5A,
 * d0 0x50 + 0C/0E/10/17) is judged before the function code and stays the stock's. */
static bool cmd_frame(const uint8_t *m, uint32_t n)
{
    if (n < 10 || m[0] != 0xF0 || m[1] != 0x40 || (m[3] != 0x31 && m[3] != 0x32) || m[4] != 0x08 || m[5] != 0x02)
        return false;
    if (m[6] == 0x5A || (m[6] == 0x50 && (m[7] == 0x0C || m[7] == 0x0E || m[7] == 0x10 || m[7] == 0x17)))
        return false;
    return stk_dev_id == m[2] || m[2] == 0x7F;   /* 0x4014A304: else the stock ignores it too */
}

/* run a bound controller's command as the stock runs its binding's: the same apply
 * FUN_4014EBFC in the stock binding's ctx (as io_apply does for fn 0x10 - the stock binding never moves), the
 * metronome judged first (kpmetro), and what it changed (run state 56 08, ...) told to the other bound
 * controllers by the stage-2 set compare (the stock has no notice for it).  noinline: roundtrip_sysex.py 5 */
__attribute__((noinline)) static void sx_cmd(uint8_t port, const uint8_t *m, uint32_t n)
{
    uint32_t metro = metro_judge(m, n);
    uint8_t ctx_id = stk_binding != 0 ? stk_binding : (uint8_t)(port + 1);
    rw.cmds++;
    if (stk_binding != (uint8_t)(port + 1))
        rw.cmds_other++;
    kps_set_begin(&rw.core, port, m, n);
    if (!rcc_kawai(port, m, n))   /* Phase 3-6 R1: Music / MusicProgress commands are the core's */
    stk_param_set(ctx_id, m[3], m + 6, n);
    kps_set_end(&rw.core);
    if (metro)
        metro_after(metro);
    kst_event(&rw.state, KST_WHY_SET);
    suc_cmd(m, n);   /* 32 51 34 / 36: the startup module follows the stock save / factory reset */
}

/* startup restore (startup_cave.c, task 19 pump tail): a set no controller sent.  The same apply as io_apply, but in
 * ctx 0 - FUN_4016053C uses ctx for the settings block (FUN_4015B4D8: ctx 0..5 = the same block 0x418F19E4) and,
 * at its common tail 0x4016383C (`sub ip, r4, #1; cmp ip, #3; bhi`), sets the stock binding's remote-change flag
 * (FUN_40151B90) only for ctx 1..4 - and the stage-2 fan-out with no excluded port (KPS_FROM_CORE). */
void sxc_core_set(const uint8_t *f, uint32_t n)
{
    uint32_t metro;
    if (!ready || n < 10)
        return;
    metro = metro_judge(f, n);
    kps_set_begin(&rw.core, KPS_FROM_CORE, f, n);
    stk_param_set(0, 0x10, f + 6, n);
    kps_set_end(&rw.core);
    if (metro)
        metro_after(metro);
    kst_event(&rw.state, KST_WHY_RESTORE);
}

/* the restore waits while a bound controller's fan-out queue is more than half full */
int sxc_fan_room(void)
{
    for (uint8_t p = 0; p < KPS_PORTS; p++)
        if (kps_bound(&rw.core, p) && kps_fan_pending(&rw.core, p) > KPS_FANQ / 2)
            return 0;
    return 1;
}

/* ---- kps_io_t on the stock firmware (task 19) --------------------------------------------- */
static uint8_t ss_binding(void *c) { (void)c; return stk_binding; }

static bool ss_match(void *c, uint8_t i, uint8_t v1, uint8_t v2)
{
    (void)c;
    return stk_req_match(i, v1, v2) != -1;
}

/* Phase 3-4 S3: the store answers frame j of descriptor i with key byte k instead of the getter when
 * kst_s3_frame says it may (settings descriptor, trusted store, value held); else the stock path below runs */
#define S3_TRY(k)                                                                                 \
    do {                                                                                          \
        if (s3) {                                                                                 \
            const uint8_t kk_[3] = {d[1], d[2], (uint8_t)(k)};                                    \
            int r_ = kst_s3_frame(&rw.state, i, j, cc, kk_, out);                                 \
            if (r_ > 0)                                                                           \
                return r_;                                                                        \
        }                                                                                         \
    } while (0)

/* FUN_401526DC without the sender: frame j of descriptor i, the getter in the stock binding's ctx.  s3: a request answer
 * (kps io.value) - the store may answer it; the store's own walks and the stage-2 snapshots always read the stock. */
static int value_of(uint8_t port, uint8_t cc, uint8_t i, uint8_t part, uint8_t j, uint8_t out[KPS_FRAME_MAX], bool s3)
{
    const uint8_t *d = stk_desc(i);
    uint8_t buf[48], key;
    uint32_t ctx = stk_ctx_of(stk_binding ? stk_binding : (uint32_t)port + 1u), x;
    stk_valsrc_t src;
    (void)s3;
    memset(buf, 0, sizeof buf);
    if (part == 0 || (uint8_t)(part - 8) <= 3 || part == 0x0F) {
        if (j)
            return -1;
        x = stk_part_x(part);
        if (x != 0x7F && stk_desc_has(i, x) == 0)
            return -1;
        S3_TRY(part);
        if (stk_getter(ctx, x, i, buf) != 0)
            return -1;
        key = part;
    } else if ((uint8_t)(part - 0x20) < 0x10 || (uint8_t)(part - 0x30) < 0x10) {
        uint8_t base = part < 0x30 ? 0x20 : 0x30, want = part < 0x30 ? 0x8F : 0x5F;
        if (j || i != want)
            return -1;
        S3_TRY(part);
        if (stk_getter(ctx, stk_slot16(part - base), want, buf) != 0)
            return -1;
        key = part;
    } else if (part == 0x7F) {
        uint8_t b = d[3];
        if (b == 0x80) {
            if (j >= 5)
                return -1;
            x = stk_slot5(j);
            if (stk_desc_has(i, x) == 0)
                return 0;
            S3_TRY(stk_key80[x]);
            if (stk_getter(ctx, x, i, buf) != 0)
                return 0;
            key = stk_key80[x];
        } else if (b == 0x81 || b == 0x82) {
            if (j >= 16)
                return -1;
            x = stk_slot16(j);
            key = b == 0x82 ? stk_key82[x] : (x == 0 ? 0x0F : stk_key81[x]);
            S3_TRY(key);
            if (stk_getter(ctx, x, i, buf) != 0)
                return 0;
        } else if ((uint8_t)(i - 0x77) <= 2) {
            if (j >= 4)
                return -1;
            S3_TRY(b);
            if (stk_getter(ctx, stk_slot16(j), i, buf) != 0)
                return 0;
            key = b;
        } else {
            if (j || b >= 0x80)
                return -1;
            x = stk_part_x(b);
            if (x != 0x7F && stk_desc_has(i, x) == 0)
                return -1;
            S3_TRY(b);
            if (stk_getter(ctx, x, i, buf) != 0)
                return -1;
            key = b;
        }
    } else {
        return -1;
    }
    src.desc = d;
    src.key = key;
    src.pad[0] = src.pad[1] = src.pad[2] = 0;
    src.buf = buf;
    memset(out, 0, KPS_FRAME_MAX);
    return (int)(uint8_t)stk_frame_build(cc, &src, out, d[8]);
}

static int ss_value(void *c, uint8_t port, uint8_t cc, uint8_t i, uint8_t part, uint8_t j, uint8_t out[KPS_FRAME_MAX])
{
    (void)c;
    return value_of(port, cc, i, part, j, out, false);
}

/* kps io.value: a request answer (fn 0x02 of a bound controller) - the store's when kst_s3_frame allows it (S3 always on) */
static int ss_value_req(void *c, uint8_t port, uint8_t cc, uint8_t i, uint8_t part, uint8_t j, uint8_t out[KPS_FRAME_MAX])
{
    (void)c;
    return value_of(port, cc, i, part, j, out, true);
}

/* stage 2: the k-th value frame of key v1 v2 part as the stock would notify it (cc 0x7F, the notify
 * path's FUN_4014C048 -> FUN_4014C010 -> FUN_4014B0DC).  The descriptors of the key are the ones whose
 * bytes 1..2 are v1 v2 (descriptor = fn v1 v2 kind .., table 0x400FAA2C); each is read as a request of
 * that part, or of part 0x7F when the part's own read gives nothing (FUN_401526DC's branches), and only
 * frames that carry this very key count. */
static int ss_key_value(void *c, uint8_t v1, uint8_t v2, uint8_t part, uint8_t k, uint8_t out[KPS_FRAME_MAX])
{
    uint8_t f[KPS_FRAME_MAX], found = 0;
    for (uint32_t i = 0; i < KPS_DESC_N; i++) {
        const uint8_t *d = stk_desc(i);
        if (i == 0x69 || d[0] != 0x10 || d[1] != v1 || d[2] != v2)
            continue;
        for (int pass = 0; pass < 2; pass++) {
            uint8_t rp = pass ? 0x7F : part;
            bool any = false;
            if (pass && part == 0x7F)
                break;
            for (uint8_t j = 0; j < 16; j++) {
                int r = ss_value(c, 3, 0x7F, (uint8_t)i, rp, j, f);
                if (r < 0)
                    break;
                if (r < 10 || f[6] != v1 || f[7] != v2 || (v1 != 0x70 && f[8] != part))
                    continue;
                any = true;
                if (found++ == k) {
                    memcpy(out, f, (size_t)r);
                    return r;
                }
            }
            if (any)
                break;
        }
    }
    return 0;
}

static void ss_connect_info(void *c, uint8_t port, uint8_t *mode, uint8_t *sub)
{
    uint32_t flag = stk_remote_get(port) & 0xFFu, md, sb;
    (void)c;
    stk_remote_set(port, 0);
    md = stk_conn_mode();
    sb = stk_conn_sub();
    if ((flag | md) == 0)
        sb = (sb | 0x40u) & 0xFFu;
    *mode = (uint8_t)md;
    *sub = (uint8_t)sb;
}

static void ss_take_rep(void *c, uint8_t port)
{
    (void)c;
    stk_ctx_copy(port);   /* the stock's other-port CONNECT, minus FUN_40151BA0 and 7E 01 00 */
    stk_ctx_clean();
    stk_bind(port);
}

static void ss_connect_post(void *c)
{
    uint32_t k = stk_conn_kind();
    (void)c;
    if (k - 1u <= 2u)
        stk_conn_post_a();
    else if (k == 5u)
        stk_conn_post_b();
}

static void ss_stock_unbind(void *c)
{
    (void)c;
    stk_unbind();
    stk_reset_a();
    stk_reset_b();
    stk_reset_c();
}

static void ss_remote_clear(void *c, uint8_t port) { (void)c; stk_remote_set(port, 0); }

/* file lease (kpsess.h): every fn 0x24 reply goes out through FUN_4014AD04 to
 * *0x413CA3DE as it is at that send, so the requester is made the stock binding for its job - the stock's own bind
 * FUN_40151B5C (a table byte store, nothing else).  The tap follows: what the stock wrote to the old binding's ring up to
 * now is read first (its notices still reach the others), the new one is read from its write index on. */
static void ss_file_bind(void *c, uint8_t port)
{
    (void)c;
    tap_follow();
    stk_bind(port);
    tap_follow();
}

static uint8_t ss_file_state(void *c)
{
    uint8_t st = 0;
    (void)c;
    if (stk_file[STK_FILE_LIST] || stk_file[STK_FILE_READY])
        st |= KPS_FS_HARD;
    if (stk_file[STK_FILE_JOB] || stk_file[STK_FILE_WRITE] || stk_file[STK_FILE_DATA])
        st |= KPS_FS_SOFT;
    return st;
}

static bool ss_link_up(void *c, uint8_t port)
{
    (void)c;
    if (port == 2)
        return stk_link_state == 2;
    if (port == 1)
        return stk_link_state == 1;
    return true;
}

static uint32_t ss_now(void)
{
    return kps_clock(&rw.clk, stk_tick2ms);
}

static void ss_sync_enable(void)
{
    if (rw.core.enabled != rw.front.enabled)
        kps_enable(&rw.core, rw.front.enabled);
}

/* ---- Phase 3-4 S2: explained / unexplained compare differences ------------------------- */
/* a stock notification on the binding's TX ring (kpsess tap): a difference of that key is the stock's */
static void ss_tapped(void *c, const uint8_t *f, uint8_t n)
{
    (void)c;
    if (n >= 10)
        kst_note(&rw.state, f + 6);
}

/* an unexplained difference (fixed in the store already): every bound controller gets the stock's value as the
 * stock's own notification frame - nobody was told of this change, the stock binding included */
static void st_fan(void *c, const uint8_t *f, uint32_t n)
{
    (void)c;
    if (rw.core.enabled && n <= KPS_FRAME_MAX)
        kps_fanout(&rw.core, 0, f, (uint16_t)n);
}

static uint8_t piano_on(void)
{
    return pwc_on() ? 1 : 0;
}

/* recorder states 0 (none) and 2 (song stopped) are at rest; 1 play, 3, 4 rec, 5 rec standby, 6 count-in, 7, 8 not
 * (FUN_40166E74 -> 1, FUN_401694B0 -> 2, FUN_401693CC -> 3, FUN_40169F00 -> 5, FUN_40169EDC -> 6) */
static bool rec_busy(uint8_t r) { return r != 0 && r != 2; }

/* why the periodic compare must not run now (S2_P_* bits, 0 = it may) */
static uint8_t s2_pause(bool starting)
{
    uint8_t p = 0;
    if (!rw.core.enabled || !rw.state.synced)
        p |= S2_P_OFF;
    if (rw.core.set_on)
        p |= S2_P_FAN;
    if (starting)   /* a running walk is not stopped by its own fan-out (an unexplained difference queues frames) */
        for (uint8_t q = 0; q < KPS_PORTS; q++)
            if (kps_fan_pending(&rw.core, q) || (rw.core.port[q].busy && rw.core.port[q].fan))
                p |= S2_P_FAN;
    if (stk_stage_flag != 0)
        p |= S2_P_STAGE;
    if (suc_settling())
        p |= S2_P_STARTUP;
    if (stk_flash_busy() || stk_flash_jobs())
        p |= S2_P_FLASH;
    if (rec_busy(rd8(KPM_A_REC_STATE)))
        p |= S2_P_REC;
    if (!piano_on())
        p |= S2_P_POWER;
    return p;
}

/* task 19 pump tail, before the walk step: events the store cannot see, then the periodic pass */
static void s2_tick(uint32_t now)
{
    uint8_t r = rd8(KPM_A_REC_STATE), on = piano_on();
    bool ours;
    if (r != rw.s2.rec_prev) {   /* song start loads settings, rec / count-in stop the metronome */
        rw.s2.rec_prev = r;
        kst_event(&rw.state, KST_WHY_REC);
    }
    if (on != rw.s2.on_prev) {   /* standby: stop-all FUN_400B0EA8, wake: headphone re-read FUN_400ACC7C */
        rw.s2.on_prev = on;
        kst_event(&rw.state, KST_WHY_POWER);
    }
    if (rw.s2.walking) {
        ours = rw.state.st.passes == rw.s2.pass0;
        if (!ours || rw.state.mode == KST_IDLE) {
            rw.s2.walking = 0;
            rw.s2.passes++;
            rw.s2.next = now + S2_PERIOD_MS;
            return;
        }
        rw.s2.pause = s2_pause(false);
        if (rw.s2.pause) {
            kst_abort(&rw.state);   /* the window stays where it was: the next pass starts over */
            rw.s2.walking = 0;
            rw.s2.aborts++;
            rw.s2.next = now + S2_RETRY_MS;
        }
        return;
    }
    /* S3: until a clean walk ran, requests read the stock - make that walk come soon (still paused by S2's rules) */
    if (rw.state.synced && !kst_s3_trusted(&rw.state) && (int32_t)(rw.s2.next - (now + S3_SOON_MS)) > 0)
        rw.s2.next = now + S3_SOON_MS;
    if ((int32_t)(now - rw.s2.next) < 0)
        return;
    rw.s2.pause = (uint8_t)(s2_pause(true) | (rw.state.mode != KST_IDLE ? S2_P_WALK : 0));
    if (rw.s2.pause) {
        rw.s2.postponed++;
        rw.s2.next = now + S2_RETRY_MS;
        return;
    }
    rw.s2.pass0 = rw.state.st.passes;
    kst_start(&rw.state, KST_COMPARE);
    rw.s2.walking = 1;
}
#endif

#if CA79NG_SESSION
/* ---- Phase 4 A intake gate (kpgate.h) ------------------------------ */
static bool gt_bound(void *c, uint8_t port) { (void)c; return kps_bound(&rw.core, port); }
static uint8_t gt_file(void *c, uint8_t which)
{
    (void)c;
    return which == KPG_FILE_JOB ? stk_file[STK_FILE_JOB] : stk_file[STK_FILE_LIST];
}
static bool gt_media(void *c) { (void)c; return (stk_gate_media & 0x7Fu) != 0; }   /* FUN_4012795C */
static uint8_t gt_mode(void *c) { (void)c; return (uint8_t)stk_conn_kind(); }       /* FUN_400B143C */
static bool gt_test(void *c) { (void)c; return stk_gate_model != 0; }               /* FUN_4015BA64 */

/* the data the stock's info answer carries, filled as FUN_4014A2C8 fills its local buffer (0x4014A45C..0x4014A6A0) */
static int gt_info(void *c, uint8_t sub, uint8_t sel, uint8_t out[KPG_INFO_MAX + 1u])
{
    uint32_t v;
    int k = 0;
    (void)c;
    if (sub == 4 && sel == 0) {          /* FUN_400B389C(FUN_400B39B8() & 0xFF): 6 bytes of the model table -> FUN_40149978 */
        const uint8_t *e = stk_gate_model_entry(stk_gate_model_idx() & 0xFFu);
        for (; k < 6; k++)
            out[k] = e[k];
    } else if (sub == 4 && sel == 1) {   /* FUN_400B3894: the version string, its length up to 0x12 -> FUN_40149B54 */
        const uint8_t *e = stk_gate_version();
        while (k < 0x12 && e[k]) {
            out[k] = e[k];
            k++;
        }
    } else if (sub == 5 && sel == 0) {   /* FUN_40149904(FUN_400B39B8() & 0xFF): (v >> 7) & 0x7F, v */
        v = stk_gate_model_idx() & 0xFFu;
        out[k++] = (uint8_t)((v >> 7) & 0x7Fu);
        out[k++] = (uint8_t)v;
    } else if (sub == 5 && sel == 1) {   /* FUN_401498D4(FUN_400B39F0() ? FUN_400B39D8() & 0xFF : 0): 0, v */
        v = stk_gate_info_b() ? stk_gate_info_c() & 0xFFu : 0;
        out[k++] = 0;
        out[k++] = (uint8_t)v;
    } else if (sub == 5 && sel == 2) {   /* FUN_401498AC(*0x418CE0EC): v */
        out[k++] = stk_gate_info_d;
    } else {
        return 0;
    }
    out[k] = 0xFF;
    return k;
}
#endif

static void init(void)
{
    static const kpf_io_t io = {
        .ctx = 0, .binding = io_binding, .device = io_device, .forward = io_forward,
        .staging = io_staging, .tx = io_tx, .apply = io_apply,
    };
    memset(&rw, 0, sizeof rw);
    rw.guard_lo = GUARD_LO;
    rw.guard_hi = GUARD_HI;
    param_store_defaults(&rw.store);
    kpf_init(&rw.front, &io, &rw.store);
#if CA79NG_SESSION
    {
        static const kps_io_t sio = {
            .ctx = 0, .binding = ss_binding, .device = io_device, .forward = io_forward, .tx = io_tx,
            .match = ss_match, .value = ss_value_req, .connect_info = ss_connect_info, .take_rep = ss_take_rep,
            .connect_post = ss_connect_post, .stock_unbind = ss_stock_unbind, .remote_clear = ss_remote_clear,
            .link_up = ss_link_up, .key_value = ss_key_value, .tapped = ss_tapped,
            .file_bind = ss_file_bind, .file_state = ss_file_state,
        };
        kps_init(&rw.core, &sio, ss_now());
    }
    {
        static const kpg_io_t gio = {
            .ctx = 0, .binding = ss_binding, .device = io_device, .forward = io_forward, .bound = gt_bound, .tx = io_tx,
            .file = gt_file, .media = gt_media, .mode = gt_mode, .test_ok = gt_test, .info = gt_info,
        };
        kpg_init(&rw.gate, &gio);
    }
    {
        static const kst_io_t kio = {.ctx = 0, .match = ss_match, .value = ss_value, .fan = st_fan};
        kpm_cfg_t cfg;
        kst_init(&rw.state, &rw.store, &kio);
        /* S3 / S4 always on (boot-on 2026-11-16, off-switches removed in fold step 3): S3 serves nothing until a clean
         * walk makes the store trusted; S4's gen bump lands before the fill walk starts */
        kst_s3_enable(&rw.state, true);
        kst_s4_enable(&rw.state, true);
        kst_start(&rw.state, KST_FILL);   /* walked KST_CALLS value reads per pump-tail pass */
        metro_cfg(&cfg);
        kpm_init(&rw.metro, &cfg);
        rw.s2.next = ss_now() + S2_PERIOD_MS;
        rw.s2.rec_prev = rd8(KPM_A_REC_STATE);
        rw.s2.on_prev = piano_on();
    }
    /* the MIDI channel judge on once ready, when attach() put the ota_loader_v3 sites in place (attach() runs before the
     * first tick; without them - ota_loader_v2 - nothing is hooked and the stubs never run) */
    mcc_on = mc_hooked ? 1 : 0;
#endif
    ready = 1;
}

void sxc_tick(void)
{
    if (!ready)
        init();
}

void sxc_attached(int ok, volatile unsigned char *flag)
{
    hooked = ok ? 1 : 0;
    inflight = flag;
}

void sxc_restart(void)
{
    ready = 0;
    hooked = 0;
    inflight = 0;
#if CA79NG_SESSION
    pump_hooked = 0;
    resumed = 0;
#endif
}

#if CA79NG_SESSION
void ssc_attached(int ok)
{
    pump_hooked = ok ? 1 : 0;
}

/* task 19, MIDI pump tail 0x401539AC (every 2 ms): resends, link-down check, binding sync */
void ssc_pump(void)
{
    if (!ready)
        return;
    rw.pumps++;
    ss_sync_enable();
    if (inflight)
        *inflight = 1;   /* the sender may block in the TX writer */
    rw.core.now = ss_now();
    if (!resumed) {
        /* a live payload swap starts with only the stock binding in the table: the panel, whose USB
         * link never dropped, gets the stock's 7E 01 01 and CONNECTs again (kpsess.h, kps_resume) */
        resumed = 1;
        kps_resume(&rw.core);
    }
    tap_follow();        /* stage 2: the stock's notifications to its binding -> the other controllers */
    kps_tick(&rw.core, ss_now());
    metro_poll();        /* Phase 3-5: the beats task 20 sent the stock binding -> the other controllers */
    s2_tick(rw.core.now);   /* Phase 3-4 S2: periodic compare (paused while settings change) */
    if (rw.state.mode != KST_IDLE)
        kst_step_calls(&rw.state, KST_CALLS);   /* Phase 3-4 S1: fill / compare walk (getter reads only) */
    rcc_pump(rw.core.now);   /* Phase 3-6 R1: recorder view, op 1E commands, confirmations, notifications */
    /* startup settings: boot restore, snapshots; changes = sets the front mirrored +
     * commands + stock notifications on the binding's ring */
    suc_pump(&rw.store, &rw.state, rw.front.st.mirrored + rw.cmds + rw.core.st.tap_frames, rw.core.now);
    if (inflight)
        *inflight = 0;
}
#endif

#if CA79NG_SESSION
/* ---- Phase 4: MIDI channel messages + delayed apply -------------
 * ota_loader_v3 sites 0x40148C4C (Program Change), 0x40148C60 (Control Change), 0x40153998 (delayed apply), task 19.
 * The stubs (src/power_hooks.c) tail-call the stock target while mcc_on is 0 (before init()).  On: the stub asks mcc_*_judge (reads
 * only, never blocks) - KCH_DROP returns to the parser, KCH_STOCK tail-calls the stock routine with the registers the
 * parser's bl left, KCH_SETTING enters mcc_*_run, which runs the stock routine under the resident's in-flight flag and
 * reads the key it may have changed back into the settings store before returning (the same task-19 call). */
static void put32(uint8_t *p, uint32_t v);
volatile uint8_t mcc_on = 0;      /* init() sets it when the sites are attached; nothing clears it (fold step 3) */
static uint8_t mc_hooked = 0;     /* bit 0 PC, 1 CC, 2 delayed apply: attached                */
static kch_dec_t mc_dec = {0, {0, 0, 0}, 0, 0};   /* the judgement of the message mcc_*_run executes (task 19) */

static void hold(int on)
{
    if (inflight)
        *inflight = (unsigned char)on;
}

static void mc_in(uint32_t port, kch_in_t *in)
{
    uint32_t ctx = stk_ctx_of(port);                 /* FUN_4015B470(port), as FUN_401475CC / FUN_40148218 */
    in->bound = stk_binding != 0 && stk_binding == (uint8_t)(port + 1u);   /* FUN_40151BCC */
    in->run = stk_mc_run() != 0;
    in->sub3 = stk_mc_sub(ctx) == 3;
    in->mode = stk_mc_mode;
    in->omni = stk_mc_block(ctx)[0x32];
    in->rx_ch = stk_dev_id;
}

static uint32_t judged(uint32_t *cnt, uint32_t st, uint32_t d1, uint32_t d2)
{
    if (mc_dec.act <= KCH_SETTING)
        cnt[mc_dec.act]++;
    rw.mc.msg[0] = (uint8_t)st;
    rw.mc.msg[1] = (uint8_t)d1;
    rw.mc.msg[2] = (uint8_t)d2;
    rw.mc.act = mc_dec.act;
    return mc_dec.act;
}

/* the key the stock routine just ran may have changed: its value frame into the store now (kst_absorb fans it out by
 * the compare pass's rule); a Sound apply also changes the sound's effects - an S2 / S3 event like a set */
static void mc_after(void)
{
    static const uint8_t parts[] = {0x00, 0x08, 0x09, 0x0A, 0x0B};   /* stk_key80: the part key of slots 0..4 */
    uint8_t f[KPS_FRAME_MAX];
    if (mc_dec.act != KCH_SETTING)
        return;
    if (mc_dec.side)
        kst_event(&rw.state, KST_WHY_SET);
    for (unsigned k = 0; k < sizeof parts; k++) {
        uint8_t part = mc_dec.key[2] == KCH_PART_ALL ? parts[k] : mc_dec.key[2];
        int n = ss_key_value(0, mc_dec.key[0], mc_dec.key[1], part, 0, f);
        if (n > 0 && kst_absorb(&rw.state, f, (uint32_t)n)) {
            rw.mc.last[0] = f[6];
            rw.mc.last[1] = f[7];
            rw.mc.last[2] = f[8];
        }
        if (mc_dec.key[2] != KCH_PART_ALL)
            break;
    }
}

uint32_t mcc_pc_judge(uint32_t port, uint32_t st, uint32_t prog)
{
    kch_in_t in;
    if (!ready)
        return KCH_STOCK;
    mc_in(port, &in);
    mc_dec = kch_pc(&in, (uint8_t)(st & 0x0Fu));
    return judged(rw.mc.pc, st, prog, 0);
}

void mcc_pc_run(uint32_t port, uint32_t st, uint32_t prog)
{
    hold(1);   /* the stock apply may block (FUN_4016053C) */
    stk_mc_pc(port, st, prog);
    mc_after();
    hold(0);
}

uint32_t mcc_cc_judge(uint32_t port, uint32_t st, uint32_t cc, uint32_t val)
{
    kch_in_t in;
    if (!ready)
        return KCH_STOCK;
    mc_in(port, &in);
    mc_dec = kch_cc(&in, (uint8_t)(st & 0x0Fu), (uint8_t)cc, (uint8_t)val);
    return judged(rw.mc.cc, st, cc, val);
}

void mcc_cc_run(uint32_t port, uint32_t st, uint32_t cc, uint32_t val)
{
    hold(1);
    stk_mc_cc(port, st, cc, val);
    mc_after();
    hold(0);
}

static void mc_apply(void *c, uint8_t ctx, uint8_t x, uint8_t v)
{
    uint8_t b[4] = {v, 0, 0, 0};
    (void)c;
    hold(1);
    stk_param_apply(ctx, x, KCH_DEF_ID, b);
    hold(0);
    if (ready) {
        rw.mc.applies++;
        kst_event(&rw.state, KST_WHY_SET);   /* 50 05 of ctx c: what the store holds is the binding's view */
    }
}

/* the stub enters here instead of FUN_401638E0 (r0 = ms since the last pass): the same walk over the stock tables */
void mcc_defer(int32_t elapsed)
{
    if (ready && stk_mc_any)
        rw.mc.passes++;
    kch_defer(stk_mc_tm, stk_mc_val, &stk_mc_any, elapsed, mc_apply, 0);
}

void mcc_attached(uint8_t mask)
{
    mc_hooked = mask;
}

/* op 0x1B 07: the record (layout 9) */
void sxc_chan(uint8_t raw[SX_DIAG_RAW])
{
    memset(raw, 0, SX_DIAG_RAW);
    raw[0] = 9;
    raw[1] = (uint8_t)(0x01 | (mcc_on ? 0x02 : 0) | (ready ? 0x04 : 0) | (mc_hooked & 7u) << 4);
    if (!ready)
        return;
    memcpy(raw + 2, rw.mc.last, 3);
    memcpy(raw + 5, rw.mc.msg, 3);
    raw[8] = rw.mc.act;
    for (int i = 0; i < 3; i++) {
        put32(raw + 12 + 4 * i, rw.mc.pc[i]);
        put32(raw + 24 + 4 * i, rw.mc.cc[i]);
    }
    put32(raw + 36, rw.mc.applies);
    put32(raw + 40, rw.state.abs.changed);
    put32(raw + 44, rw.state.abs.fanned);
    raw[9] = (uint8_t)(rw.mc.passes > 0xFF ? 0xFF : rw.mc.passes);
    raw[10] = (uint8_t)(rw.state.abs.bad > 0xFF ? 0xFF : rw.state.abs.bad);
}

#endif

/* counts only - reads the frame, never blocks, never changes what the stock or the front does */
static void sess_count(uint32_t port, const uint8_t *m, uint32_t n)
{
    sx_sess_t *s = &rw.sess;
    if (port > 3)
        return;
    s->port_seen[port]++;
    if (n >= 9 && m[1] == 0x40 && m[3] == 0x7E && m[4] == 0x08 && m[5] == 0x02 && m[6] == 0x01) {
        if (m[7] == 0x01) {
            if (s->connects[port] != 0xFFFFu)
                s->connects[port]++;
            s->last_connect = (uint8_t)(port + 1);
            s->binding_at = stk_binding;
        } else if (m[7] == 0x00 && s->disconnects[port] != 0xFFu) {
            s->disconnects[port]++;
        }
    }
}

#if CA79NG_SESSION
/* out of line: sxc_kawai's frame (task 19 stack, roundtrip_sysex check 5) stays as it was.  It only rewrites the
 * counter / checksum bytes of the frame in the receive buffer (RAM) and the stock's upload length word - no call, no
 * send, no wait; the frame always goes on to the stock (fread_cave.h frc_rx) */
__attribute__((noinline)) static void fw_rx(const uint8_t *buf, uint32_t len)
{
    if (len >= 8 && buf[3] == 0x24 && (buf[6] == 0x01 || buf[6] == 0x02 || buf[6] == 0x7B))
        (void)frc_rx(cur_port, (uint8_t *)(uintptr_t)buf, len);
}
#endif

int sxc_kawai(uint32_t port, const uint8_t *buf, uint32_t len)
{
    bool owned;
    if (!ready)
        return 0;   /* before the first engine-loop pass: the stock handler, as without the hook */
    sess_count(port, buf, len);
#if CA79NG_SESSION
    ss_sync_enable();
    /* Phase 3-6 R3 (recctl_cave.h rcc_file_veto): a song import 24 09 / USB load 24 08 while a take, a song-table write or
     * a NOR job runs gets the stock's own second-job answer (request ACK + Busy, 0x4014A9B0) and never reaches the stock */
    if (len >= 9 && buf[1] == 0x40 && buf[3] == 0x24 && (buf[6] == 0x08 || buf[6] == 0x09) &&
        kps_bound(&rw.core, (uint8_t)port) && rcc_file_veto(buf[6], buf[7])) {
        static const uint8_t ack[] = {0xF0, 0x40, 0x7F, 0x24, 0x04, 0x1E, 0x7F, 0xF7};
        static const uint8_t busy[] = {0xF0, 0x40, 0x7F, 0x7E, 0x04, 0x1E, 0x03, 0x01, 0x00, 0x00, 0xF7};
        sxc_tx((uint8_t)port, ack, sizeof ack);
        sxc_tx((uint8_t)port, busy, sizeof busy);
        return 1;
    }
    if (inflight)
        *inflight = 1;   /* the session core may answer / send here */
    owned = kps_rx(&rw.core, (uint8_t)port, buf, len, ss_now()) == KPS_DONE;
    if (inflight)
        *inflight = 0;
    if (len >= 4 && buf[3] == 0x7E) {
        /* S2: CONNECT / DISCONNECT (stop-all FUN_400B0EA8 via FUN_40151BA0).  S3: FUN_4014A2C8's 7E
         * 01 xx touches run state only (the metronome run byte, recorder / player stop, VT ctx copy FUN_4015DE98, MIDI
         * channel reset FUN_4015B3D8), 7E 04 / 05 only answer - the store stays trusted; 7E 02 03 (FUN_4013D614) does not */
        if (len >= 8 && (buf[6] == 0x01 || buf[6] == 0x04 || buf[6] == 0x05))
            kst_event_run(&rw.state, KST_WHY_STOCK);
        else
            kst_event(&rw.state, KST_WHY_STOCK);
    }
    if (owned)
        return 1;
    /* Phase 4 A: the rest of FUN_4014A2C8's functions - the core accepts (the stock runs it), refuses or answers */
    rw.gate.enabled = rw.core.enabled;
    if (inflight)
        *inflight = 1;   /* the gate's own answers go through the TX writers */
    owned = kpg_rx(&rw.gate, (uint8_t)port, buf, len) != KPG_PASS;
    if (inflight)
        *inflight = 0;
    if (owned)
        return 1;
    /* fn 0x24 upload DataPackets of the binding: the packet index past the 7-bit counter - the frame is rewritten for the
     * stock receiver, which still gets it and answers (fread_cave.c frc_rx) */
    cur_port = (uint8_t)port;
    fw_rx(buf, len);
    if (rw.core.enabled && cmd_frame(buf, len) && kps_bound(&rw.core, (uint8_t)port)) {
        if (inflight)
            *inflight = 1;   /* the stock apply path may block */
        sx_cmd((uint8_t)port, buf, len);
        if (inflight)
            *inflight = 0;
        return 1;
    }
#endif
    if (kpf_judge(&rw.front, (uint8_t)port, buf, len) == KPF_PASS) {
#if CA79NG_SESSION
        /* S2: the stock handler acts on it - VT edit, staging, fn 0x31 / 0x32 of an unbound port, FUN_4014A1B4 ..
         * (not the ACK / NAK / Cancel / Wait and request bytes it only answers) */
        if (len >= 4 && (buf[3] < 0x02 || buf[3] > 0x07) && buf[3] != 0x7E)   /* 7E: counted above */
            kst_event(&rw.state, KST_WHY_STOCK);
#endif
        return kpf_handle(&rw.front, (uint8_t)port, buf, len) ? 1 : 0;   /* counts it; never blocks */
    }
    if (inflight)
        *inflight = 1;   /* from here the front may block (TX writer, stock apply path) */
    owned = kpf_handle(&rw.front, (uint8_t)port, buf, len);
    if (inflight)
        *inflight = 0;   /* no payload frame blocks after this point (the stub only returns / tail-calls) */
    return owned ? 1 : 0;
}

int sxc_ctrl_bound(uint8_t port)
{
#if CA79NG_SESSION
    /* the stock binding is always one of the core's controllers (kpsess sync() adds it on the next frame / pump
     * pass) - counted here too, so a read between the stock's bind and that pass does not refuse it */
    if (ready && rw.core.enabled && port < KPS_PORTS)
        return kps_bound(&rw.core, port) || stk_binding == (uint8_t)(port + 1) ? 1 : 0;
#endif
    (void)port;
    return -1;
}

#if CA79NG_SESSION
/* recctl_cave.h: the bound controllers, and a frame to one of them the way the core sends (out of the ring tap) */
uint8_t sxc_bound_mask(void)
{
    uint8_t b = 0;
    if (!ready || !rw.core.enabled)
        return 0;
    for (uint8_t p = 0; p < KPS_PORTS; p++)
        if (kps_bound(&rw.core, p))
            b |= (uint8_t)(1u << p);
    return b;
}

void sxc_tx(uint8_t port, const uint8_t *f, uint16_t n)
{
    io_tx(0, (uint8_t)(port & 3u), f, n);
}
#endif

/* ---- 0x7D op 0x1B ---------------------------------------------------------------------------- */
static void put32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24);
}

void sxc_diag(uint8_t raw[SX_DIAG_RAW])
{
    const kpf_stats_t *s = &rw.front.st;
    bool guards = rw.guard_lo == GUARD_LO && rw.guard_hi == GUARD_HI;
    memset(raw, 0, SX_DIAG_RAW);
    raw[0] = 1;
    raw[1] = (uint8_t)((ready && rw.front.enabled ? 0x01 : 0) | (hooked ? 0x02 : 0) | (ready ? 0x04 : 0) |
                       (ready && guards ? 0x08 : 0));
    if (!ready)
        return;
    memcpy(raw + 2, rw.front.last_key, 3);
    put32(raw + 6, s->seen);
    put32(raw + 10, s->passed);
    put32(raw + 14, s->owned);
    put32(raw + 18, s->silent);
    put32(raw + 22, s->mirrored);
    put32(raw + 26, s->unknown);
    put32(raw + 30, s->staged);
    put32(raw + 34, s->packed);
    put32(raw + 38, s->acks);
    put32(raw + 42, ca_crc32(0, &rw.store, sizeof rw.store));
}

static void put16(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8);
}

void sxc_session(uint8_t raw[SX_DIAG_RAW])
{
    const sx_sess_t *s = &rw.sess;
    memset(raw, 0, SX_DIAG_RAW);
    raw[0] = 2;
    raw[1] = stk_binding;
    raw[2] = stk_sess_base[0x2F];   /* previous binding */
    raw[3] = stk_sess_base[0x30];   /* request busy */
    for (int i = 0; i < 4; i++)
        raw[4 + i] = stk_sess_base[i];   /* remote-change flags */
    raw[8] = stk_sess_base[0x3E];   /* ACK */
    raw[9] = stk_sess_base[0x3F];   /* Cancel */
    raw[10] = stk_sess_base[0x40];  /* NAK */
    raw[11] = stk_sess_base[0x41];  /* Wait */
    put16(raw + 12, stk_tick2ms);
    raw[14] = (uint8_t)stk_port3_fwd;
    raw[15] = stk_stage_flag;
    if (!ready)
        return;
    for (int i = 0; i < 4; i++) {
        put32(raw + 16 + 4 * i, s->port_seen[i]);
        put16(raw + 32 + 2 * i, s->connects[i]);
        raw[40 + i] = s->disconnects[i];
    }
    raw[44] = s->last_connect;
    raw[45] = s->binding_at;
}

#if CA79NG_SESSION
static uint16_t sat16(uint32_t v) { return v > 0xFFFFu ? 0xFFFFu : (uint16_t)v; }
static uint8_t sat8(uint32_t v) { return v > 0xFFu ? 0xFFu : (uint8_t)v; }

#define SX_GATE_SUB(sub) ((sub) == 6)
/* op 0x1B 06 (gate build): the intake gate's counters */
static void sxc_gate(uint8_t raw[SX_DIAG_RAW])
{
    const kpg_stats_t *t = &rw.gate.st;
    memset(raw, 0, SX_DIAG_RAW);
    raw[0] = 8;
    raw[1] = (uint8_t)((ready && rw.gate.enabled ? 0x01 : 0) | (ready ? 0x04 : 0));
    raw[3] = stk_binding;
    if (!ready)
        return;
    raw[2] = rw.core.bound;
    put32(raw + 4, t->passed);
    put32(raw + 8, t->dropped);
    put16(raw + 12, t->info_own);
    put16(raw + 14, t->file_busy);
    put16(raw + 16, t->file_err);
    put16(raw + 18, t->file_ack);
    put16(raw + 20, t->test_drop);
    put16(raw + 22, t->other_drop);
}

void sxc_core(uint8_t raw[SX_DIAG_RAW], uint8_t req_port)
{
    const kps_t *k = &rw.core;
    const kps_stats_t *t = &k->st;
    const uint32_t st[13] = {t->requests, t->dropped, t->values, t->ends, t->resends, t->timeouts, t->naks,
                             t->waits, t->acks, t->to_stock, t->ack_drop, t->wait_drop, t->failed};
    memset(raw, 0, SX_DIAG_RAW);
    raw[0] = 3;
    raw[1] = (uint8_t)((ready && k->enabled ? 0x01 : 0) | (pump_hooked ? 0x02 : 0) | (ready ? 0x04 : 0));
    raw[3] = stk_binding;
    if (!ready)
        return;
    raw[2] = k->bound;
    for (int p = 0; p < 4; p++) {
        const kps_port_t *q = &k->port[p];
        raw[4 + p] = (uint8_t)((q->active ? 1 : 0) | (q->busy ? 2 : 0) | (q->end ? 4 : 0) | ((q->tries & 0x1F) << 3));
    }
    put16(raw + 8, sat16(rw.pumps));
    for (int i = 0; i < 13; i++)
        put16(raw + 10 + 2 * i, sat16(st[i]));
    raw[36] = sat8(t->conn_own);
    raw[37] = sat8(t->conn_pass);
    raw[38] = sat8(t->disc_own);
    raw[39] = sat8(t->unbinds);
    put32(raw + 40, k->now);
    raw[44] = sat8(t->cancels);
    raw[45] = (uint8_t)(0x80u | (req_port & 3u));   /* stage 2: the asking port (bit 7 = field present) */
    raw[46] = (uint8_t)((resumed ? 0x80u : 0u) | (t->resume_invites ? 0x01u : 0u));   /* live-swap panel invitation */
    raw[47] = sat8(t->file_busy);   /* file lease: requests the core answered Busy */
}

/* op 0x1B 04 (stage 2): the fan-out record */
void sxc_fan(uint8_t raw[SX_DIAG_RAW], uint8_t req_port)
{
    const kps_t *k = &rw.core;
    const kps_stats_t *t = &k->st;
    const uint32_t st[13] = {t->fan_sets, t->fan_same, t->fan_queued, t->fan_merged, t->fan_sent, t->fan_acks,
                             t->fan_drop, t->fan_fail, t->tap_frames, t->tap_fanned, t->tap_long, t->snap_over,
                             t->tap_relayed};
    memset(raw, 0, SX_DIAG_RAW);
    raw[0] = 4;
    raw[1] = (uint8_t)((ready && k->enabled ? 0x01 : 0) | (pump_hooked ? 0x02 : 0) | (ready ? 0x04 : 0));
    raw[3] = stk_binding;
    raw[4] = req_port & 3u;
    if (!ready)
        return;
    raw[2] = k->bound;
    raw[5] = rw.tap_rep;
    for (int p = 0; p < 4; p++)
        raw[6 + p] = kps_fan_pending(k, (uint8_t)p);
    put16(raw + 10, rw.tap_pos);
    for (int i = 0; i < 13; i++)
        put16(raw + 12 + 2 * i, sat16(st[i]));
    put32(raw + 38, rw.tap_bytes);
    put16(raw + 42, sat16(rw.cmds));        /* fn 0x31 / 0x32 commands the core ran */
    put16(raw + 44, sat16(rw.cmds_other));  /* of them from a controller that is not the stock binding */
    {   /* file lease: owner | holder << 4 (port + 1 each, 0 = no lease), leases taken */
        uint8_t owner = 0, holder = 0;
        (void)kps_file_lease(k, &owner, &holder);
        raw[46] = (uint8_t)((owner & 0x0Fu) | (holder & 0x0Fu) << 4);
        raw[47] = sat8(t->file_leases);
    }
}

/* op 0x1B 05: settings state S1 (kpstate) + metronome stage 1 (kpmetro) */
void sxc_state(uint8_t raw[SX_DIAG_RAW])
{
    const kst_t *s = &rw.state;
    const kpm_stats_t *m = &rw.metro.st;
    const uint32_t st[8] = {s->st.frames, s->st.stored, s->st.unknown, s->st.longval, s->st.bad, s->st.diffs,
                            s->st.indexed, s->st.passes};
    const uint32_t ms[7] = {m->sets, m->ignored, m->refused, m->beats, m->starts, m->reloads, m->resyncs};
    memset(raw, 0, SX_DIAG_RAW);
    raw[0] = 5;
    raw[1] = (uint8_t)((ready && rw.core.enabled ? 0x01 : 0) | (pump_hooked ? 0x02 : 0) | (ready ? 0x04 : 0));
    if (!ready)
        return;
    raw[1] |= (uint8_t)((s->synced ? 0x08 : 0) | (s->mode != KST_IDLE ? 0x10 : 0));
    raw[2] = (uint8_t)s->mode;
    raw[3] = s->i;
    for (int i = 0; i < 8; i++)
        put16(raw + 4 + 2 * i, sat16(st[i]));
    put16(raw + 20, sat16(s->diffs_total));
    memcpy(raw + 22, s->last_diff, 3);
    memcpy(raw + 25, s->last_unknown, 3);
    for (int i = 0; i < 7; i++)
        put16(raw + 28 + 2 * i, sat16(ms[i]));
    put16(raw + 42, sat16(rw.metro_sent));
    put16(raw + 44, sat16(rw.metro_miss));
    raw[46] = rw.metro.cfg.on;
    raw[47] = (uint8_t)rw.metro.last.beat;
}

/* op 0x1B 05 02: settings state S2 - periodic compare, explained / unexplained differences */
void sxc_state_s2(uint8_t raw[SX_DIAG_RAW], uint32_t now)
{
    const kst_t *s = &rw.state;
    kst_unexp_t u;
    memset(raw, 0, SX_DIAG_RAW);
    raw[0] = 6;
    if (!ready)
        return;
    raw[1] = (uint8_t)((rw.core.enabled ? 0x01 : 0) | (rw.s2.walking ? 0x02 : 0) | (s->synced ? 0x04 : 0) |
                       (s->mode != KST_IDLE ? 0x08 : 0));
    raw[2] = rw.s2.pause;
    raw[3] = s->last_why;
    put16(raw + 4, S2_PERIOD_MS / 100u);
    put32(raw + 6, s->ev);
    for (int i = 0; i < KST_WHY_N; i++)
        put16(raw + 10 + 2 * i, sat16(s->why[i]));
    put16(raw + 26, sat16(rw.s2.passes));
    put16(raw + 28, sat16(rw.s2.aborts));
    put16(raw + 30, sat16(rw.s2.postponed));
    put16(raw + 32, sat16(s->fanned));
    raw[34] = (uint8_t)kst_ring_count(s);
    put16(raw + 36, rw.s2.walking || (int32_t)(rw.s2.next - now) <= 0 ? 0 : sat16((rw.s2.next - now) / 100u));
    if (kst_ring_get(s, 0, &u))
        memcpy(raw + 38, u.key, 3);
}

/* op 0x1B 05 03 [k]: the unexplained ring, three entries from the k-th newest */
void sxc_state_ring(uint8_t raw[SX_DIAG_RAW], uint8_t k)
{
    const kst_t *s = &rw.state;
    kst_unexp_t u;
    memset(raw, 0, SX_DIAG_RAW);
    raw[0] = 7;
    if (!ready)
        return;
    raw[1] = (uint8_t)kst_ring_count(s);
    raw[2] = k;
    for (uint32_t e = 0; e < 3 && kst_ring_get(s, (uint32_t)k + e, &u); e++) {
        uint8_t *d = raw + 4 + 12 * e;
        raw[3]++;
        memcpy(d, u.key, 3);
        d[3] = u.lens;
        memcpy(d + 4, u.old, 4);
        memcpy(d + 8, u.now, 4);
    }
    put32(raw + 40, s->why[KST_WHY_NONE]);
    put32(raw + 44, s->ev);
}

/* op 0x1B 05 04 / 05: settings state S3 - the read owner */
void sxc_state_s3(uint8_t raw[SX_DIAG_RAW])
{
    const kst_t *s = &rw.state;
    const kst_s3_stats_t *t = &s->s3;
    const uint32_t st[6] = {t->served, t->stock, t->untrusted, t->miss, t->stale, t->fanned_key};
    memset(raw, 0, SX_DIAG_RAW);
    raw[0] = 8;
    raw[1] = 0x01;
    if (!ready)
        return;
    raw[1] |= (uint8_t)((s->s3_on ? 0x02 : 0) | (kst_s3_trusted(s) ? 0x04 : 0) | (s->synced ? 0x08 : 0) |
                        (s->clean ? 0x10 : 0) | (s->mode != KST_IDLE ? 0x20 : 0));
    for (int i = 0; i < 6; i++)
        put32(raw + 4 + 4 * i, st[i]);
    put32(raw + 28, s->gen);
    put32(raw + 32, s->clean_gen);
    put32(raw + 36, s->ev);
    put32(raw + 40, s->st.passes);
}


/* op 0x1B 05 06 / 07: settings state S4 - the write owner */
void sxc_state_s4(uint8_t raw[SX_DIAG_RAW])
{
    const kst_t *s = &rw.state;
    const kst_s4_stats_t *t = &s->s4;
    const uint32_t st[10] = {t->sets, t->values, t->own, t->refused, t->passed, t->followed, t->wmiss, t->kept, t->side,
                             t->dropped};
    memset(raw, 0, SX_DIAG_RAW);
    raw[0] = 9;
    raw[1] = 0x01;
    if (!ready)
        return;
    raw[1] |= (uint8_t)((s->s4_on ? 0x02 : 0) | (s->s3_on ? 0x04 : 0) | (kst_s3_trusted(s) ? 0x08 : 0) |
                        (stk_stage_flag != 0 ? 0x10 : 0));
    for (int i = 0; i < 10; i++)
        put16(raw + 4 + 2 * i, sat16(st[i]));
    memcpy(raw + 24, s->s4_last_refused, 3);
    memcpy(raw + 27, s->s4_last_miss, 3);
    memcpy(raw + 30, s->s4_last_side, 3);
    put16(raw + 33, sat16(rw.s4.staged));
    put16(raw + 35, sat16(rw.s4.big));
    raw[37] = sat8(rw.s4.unread);
    put32(raw + 40, s->gen);
    put32(raw + 44, s->ev);
}


/* op 0x1B 05 01: one compare pass of the whole store (task 19, KST_CALLS value reads per pump-tail pass) */
static uint8_t state_compare(void)
{
    if (!ready || !rw.state.synced || rw.state.mode != KST_IDLE)
        return SX_BAD_VALUE;
    kst_start(&rw.state, KST_COMPARE);
    return SX_OK;
}
#endif

/* 8 -> 7 bit, the 0x7D family's packing (prefix byte = bit 7 of up to 7 following bytes) */
static size_t pack7(const uint8_t *in, size_t n, uint8_t *out)
{
    size_t o = 0;
    for (size_t i = 0; i < n; i += 7) {
        size_t g = n - i < 7 ? n - i : 7;
        uint8_t hi = 0;
        for (size_t k = 0; k < g; k++)
            hi |= (uint8_t)(((in[i + k] >> 7) & 1u) << k);
        out[o++] = hi;
        for (size_t k = 0; k < g; k++)
            out[o++] = in[i + k] & 0x7F;
    }
    return o;
}

void sxc_sysex(uint8_t stock_port, const uint8_t *buf, uint32_t len)
{
    /* the cave checked F0 7D C A 7 9 <op> .. F7 and len >= 8 */
    uint8_t raw[SX_DIAG_RAW], m[8 + 2 + SX_DIAG_RAW + (SX_DIAG_RAW + 6) / 7];
    uint8_t sub = len >= 9 ? buf[7] : 0, rc = SX_OK;
    size_t o = 7;
    if (len == 10 && sub == 1) {   /* 1B 01 <0|1>: front off / on - the bound controller only */
        if (stk_binding != (uint8_t)(stock_port + 1))
            rc = SX_NOT_BOUND;
        else if (buf[8] > 1 || !ready)
            rc = SX_BAD_VALUE;
        else
            rw.front.enabled = buf[8] != 0;
#if CA79NG_SESSION
    } else if ((len == 10 || len == 11) && sub == 5) {
        /* 1B 05 01: start a compare pass of the settings store; 1B 05 02: S2 record; 1B 05 03 [k]: unexplained ring;
         * 1B 05 04 / 06 <x> (the S3 / S4 switches before fold step 3): rc 1 below, the record still answered */
        if (len == 10 && buf[8] == 1)
            rc = state_compare();
        else if (!(len == 10 && (buf[8] == 2 || buf[8] == 5 || buf[8] == 7)) && !(buf[8] == 3 && (len == 10 || buf[9] < KST_RING)))
            rc = SX_BAD_VALUE;
    } else if (sub == 7 && (len == 9 || len == 10)) {   /* 1B 07: Phase 4 MIDI channel record; 1B 07 <x>: rc 1 + record */
        if (len == 10)
            rc = SX_BAD_VALUE;
    } else if (!(len == 8 || (len == 9 && (sub == 0 || sub == 2 || sub == 3 || sub == 4 || sub == 5 || SX_GATE_SUB(sub))))) {
        rc = SX_BAD_VALUE;   /* + 1B 03 / 04 / 05 (+ 1B 06 in the gate build) */
    }
    if (sub == 7 && (len == 9 || len == 10))
        sxc_chan(raw);
    else
    if (len == 9 && sub == 6)
        sxc_gate(raw);
    else
    if (sub == 5 && len >= 10 && (buf[8] == 4 || buf[8] == 5))
        sxc_state_s3(raw);
    else if (sub == 5 && len >= 10 && (buf[8] == 6 || buf[8] == 7))
        sxc_state_s4(raw);
    else if (sub == 5 && len >= 10 && buf[8] == 2 && rc == SX_OK)
        sxc_state_s2(raw, ready ? ss_now() : 0);
    else if (sub == 5 && len >= 10 && buf[8] == 3 && rc == SX_OK)
        sxc_state_ring(raw, len == 11 ? buf[9] : 0);
    else if (sub == 5 && (len == 9 || len == 10 || len == 11))
        sxc_state(raw);
    else if (len == 9 && sub == 4)
        sxc_fan(raw, stock_port);
    else if (len == 9 && sub == 3)
        sxc_core(raw, stock_port);
    else if (len == 9 && sub == 2)
#else
    } else if (!(len == 8 || (len == 9 && (sub == 0 || sub == 2)))) {   /* 1B / 1B 00: read, 1B 02: session */
        rc = SX_BAD_VALUE;
    }
    if (len == 9 && sub == 2)
#endif
        sxc_session(raw);
    else
        sxc_diag(raw);
    m[0] = 0xF0;
    m[1] = 0x7D;
    m[2] = 0x43;
    m[3] = 0x41;
    m[4] = 0x37;
    m[5] = 0x39;
    m[6] = SX_OP_FRONT | 0x40;
    m[o++] = sub & 0x7F;
    m[o++] = rc;
    o += pack7(raw, sizeof raw, m + o);
    m[o++] = 0xF7;
    hal_midi_tx(cave_port_from_stock(stock_port & 3), m, (uint16_t)o);
}
