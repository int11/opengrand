/* OpenGrand cave port - startup settings owned by the core.  See startup_cave.h. */
#include "startup_cave.h"

#include <stdbool.h>
#include <string.h>

#include "ca79ng/kpstart.h"
#include "ca79ng/util.h"
#include "hal_cave.h"
#include "sysex_cave.h"
#include "sysex_stock.h"

#define GUARD_LO 0x57A27C79u
#define GUARD_HI 0xC79A2757u
#define SCAN_PER_PASS 16u   /* records compared per pump-tail pass (store reads only)        */
#define ROUNDS 2u           /* restore rounds: the second for what a set changed in passing    */
#define BARRIER() __asm__ volatile("" ::: "memory")

enum { PH_WAIT = 0, PH_RESTORE, PH_RESYNC, PH_READY, PH_CAPTURE, PH_OFF };
/* requests task 19 -> task 24 (t24_req) */
enum { T24_NONE = 0, T24_MODE, T24_SAVE, T24_CLEAR };

/* ---- RW state in R1 (core RW region; only what task 24 fills before `loaded`, or task 19 before `wreq`) ---- */
typedef struct {
    uint32_t guard_lo;
    uint8_t rbuf[KSU_SNAP_MAX];   /* the snapshot to restore (task 24 fills it before loaded = 1) */
    uint8_t wbuf[KSU_SNAP_MAX];   /* the snapshot to write (task 19 fills it before wreq != 0)    */
    uint8_t ng[KSU_NG_MAX];       /* the NG part of the mode's snapshot (task 24, before loaded)  */
    uint8_t wng[KSU_NG_MAX];      /* the NG part being written (task 24)                          */
    uint32_t guard_hi;
} su_rw_t;

CAVE_RW static su_rw_t rw;

/* ---- small state in the payload .data (copied fresh with the payload) --------------------- */
/* task 24 -> task 19 */
static volatile uint8_t loaded = 0;      /* mode / rbuf known                                   */
static volatile uint8_t mode = 0;        /* ksu_mode_t, task 24 is the writer                   */
static volatile uint8_t have[2] = {0};   /* snapshot C / P on NOR                               */
static volatile uint8_t flush_req = 0;   /* standby / off ahead: save now                       */
static volatile uint8_t redirty = 0;     /* a P write did not happen (no room / failed)         */
static volatile uint8_t kv_ready = 0;
static volatile uint32_t gens[2] = {0};
static volatile uint16_t sizes[2] = {0};
/* the NG part - task 24 */
static volatile uint8_t mode_all = 0;    /* the mode was chosen by a build with the NG part (KV_KEY_STARTUP_ALL)    */
static volatile uint8_t have_ng[2] = {0};/* snapshot C / P carries an NG part                           */
static volatile uint8_t ng_kind = 0xFF;  /* ksu_ng_t of this power-on, 0xFF = not decided               */
static volatile uint8_t ng_applied = 0;  /* SUC_NG_* bits: the ports changed their values               */
static volatile uint32_t ng_changes = 0; /* user changes of an NG setting (task 24 -> task 19)          */
static uint32_t ngn = 0, crc_p = 0;
/* both tasks: the first of them to run decides whether this is a fresh boot (no controller bound) */
static volatile uint8_t boot_seen = 0, at_boot = 0;
/* task 19 -> task 24 */
static volatile uint8_t wreq = 0;        /* 1 + slot: wbuf holds a snapshot to write            */
static volatile uint8_t wallow = 0;      /* that write may compact                              */
static volatile uint8_t t24_req = 0;     /* T24_*                                               */
static uint8_t t24_arg = 0, t24_port = 0, t24_sub = 0;
static uint32_t wn = 0, rn = 0;
/* task 19 only */
static uint8_t ph = PH_WAIT, round_no = 0;
static uint8_t cap_slot = 0, cap_allow = 0, cap_for_op = 0;
static uint8_t walk_on = 0;
static uint32_t walk_passes = 0, pos = 0, last_changes = 0;
static ksu_deb_t deb = {0};
static uint8_t deb_on = 0;
/* op queue (task 19 ng_sysex -> task 19 pump tail) */
static uint8_t op_pending = 0, op_port = 0, op_sub = 0, op_arg = 0;
/* counters (op 0x1C 00) */
static uint16_t recs = 0, applied = 0, left = 0, unknown_keys = 0, saves = 0, save_fails = 0, deferred = 0;
static uint16_t skipped_same = 0, flash_waits = 0;
static uint8_t stage_waits = 0, last_rc = 0, skipped = 0;

/* ---- replies (0x7D framing, 7-in-8 packing as the other cave ports) ---------------------------- */
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

static void put16(uint8_t *p, uint32_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }
static void put32(uint8_t *p, uint32_t v) { put16(p, v); put16(p + 2, v >> 16); }

void suc_status(uint8_t raw[SU_RAW])
{
    memset(raw, 0, SU_RAW);
    raw[0] = 1;
    raw[1] = mode;
    raw[2] = (uint8_t)((loaded ? 0x01 : 0) | (have[0] ? 0x02 : 0) | (have[1] ? 0x04 : 0) |
                       (ph >= PH_READY ? 0x08 : 0) | (ph == PH_RESTORE || ph == PH_RESYNC ? 0x10 : 0) |
                       (wreq || ph == PH_CAPTURE ? 0x20 : 0) | (deb.dirty ? 0x40 : 0) | (kv_ready ? 0x80 : 0));
    raw[3] = ph;
    put16(raw + 4, sizes[0]);
    put16(raw + 6, sizes[1]);
    put32(raw + 8, gens[0]);
    put32(raw + 12, gens[1]);
    put16(raw + 16, recs);
    put16(raw + 18, applied);
    put16(raw + 20, left);
    put16(raw + 22, unknown_keys);
    put16(raw + 24, saves);
    put16(raw + 26, save_fails);
    put16(raw + 28, deferred);
    raw[30] = stage_waits;
    raw[31] = (uint8_t)((last_rc & 0x0F) | (rw.guard_lo == GUARD_LO && rw.guard_hi == GUARD_HI ? 0x80 : 0) |
                        (skipped ? 0x40 : 0) | (at_boot ? 0x20 : 0));
    put16(raw + 32, skipped_same);
    put16(raw + 34, flash_waits);
    raw[36] = (uint8_t)((mode_all ? 0x01 : 0) | (have_ng[0] ? 0x02 : 0) | (have_ng[1] ? 0x04 : 0));
    raw[37] = ng_kind;
    raw[38] = ng_applied;
    raw[39] = (uint8_t)ngn;
}

/* restore only into a fresh boot: a payload applied live ("apply now") finds a controller bound and keeps the settings
 * the piano has now (kpsess.h kps_resume uses the same sign) - decided once, by whichever task runs first */
static void boot_check(void)
{
    if (!boot_seen) {
        at_boot = stk_binding == 0;
        BARRIER();
        boot_seen = 1;
    }
}

static void reply(uint8_t stock_port, uint8_t sub, uint8_t rc)
{
    uint8_t raw[SU_RAW], m[8 + 2 + SU_RAW + (SU_RAW + 6) / 7];
    size_t o = 7;
    suc_status(raw);
    m[0] = 0xF0;
    m[1] = 0x7D;
    m[2] = 0x43;
    m[3] = 0x41;
    m[4] = 0x37;
    m[5] = 0x39;
    m[6] = SU_OP | 0x40;
    m[o++] = sub & 0x7F;
    m[o++] = rc;
    o += pack7(raw, sizeof raw, m + o);
    m[o++] = 0xF7;
    hal_midi_tx(cave_port_from_stock(stock_port & 3), m, (uint16_t)o);
}

static bool bound(uint8_t port)
{
    int b = sxc_ctrl_bound(port);
    return b < 0 ? stk_binding == (uint8_t)(port + 1) : b == 1;
}

/* ---- task 19: ops ------------------------------------------------------------------------------ */
void suc_sysex(uint8_t stock_port, const uint8_t *buf, uint32_t len)
{
    /* the cave checked F0 7D C A 7 9 <op> .. F7 and len >= 8 */
    uint8_t sub = len >= 9 ? buf[7] : 0, port = stock_port & 3;
    if (len == 8 || (len == 9 && sub == 0)) {
        reply(port, 0, SU_OK);
        return;
    }
    if (!((len == 10 && sub == 1) || (len == 9 && (sub == 2 || sub == 3)))) {
        reply(port, sub, SU_BAD_VALUE);
        return;
    }
    if (sub == 1 && buf[8] >= KSU_MODES) {
        reply(port, sub, SU_BAD_VALUE);
        return;
    }
    if (!bound(port)) {
        reply(port, sub, SU_NOT_BOUND);
        return;
    }
    if (op_pending || !loaded) {
        reply(port, sub, SU_BUSY);
        return;
    }
    op_port = port;
    op_sub = sub;
    op_arg = sub == 1 ? buf[8] : 0;
    op_pending = 1;   /* the next pump-tail pass takes it */
}

void suc_cmd(const uint8_t *m, uint32_t n)
{
    /* F0 40 dev 32 08 02 51 34|36 7F d0 .. F7 */
    if (n < 11 || m[3] != 0x32 || m[6] != 0x51 || !loaded)
        return;
    if (m[7] == 0x36 && (m[9] == 0x07 || m[9] == 0x04) && t24_req == T24_NONE) {
        /* factory reset (startup only / everything): our snapshots go too, the next boot is the stock's (E12) */
        t24_port = 0xFF;
        t24_req = T24_CLEAR;
    } else if (m[7] == 0x34 && m[9] == 0x07 && mode == KSU_CURRENT && ph == PH_READY && !wreq) {
        cap_slot = KSU_C;   /* "Save settings to Piano" in mode current: snapshot C too (one button, two places) */
        cap_allow = 0;
        cap_for_op = 0;
        ph = PH_CAPTURE;
        walk_on = 0;
    }
}

/* ---- task 19: walks -------------------------------------------------------------------------- */
/* a compare walk of our own (store := stock): true once it has finished */
static bool walk(kst_t *st)
{
    if (!walk_on) {
        if (st->mode != KST_IDLE)
            return false;   /* somebody else's walk (op 1B 05 01) first */
        walk_passes = st->st.passes;
        kst_start(st, KST_COMPARE);
        walk_on = 1;
        return false;
    }
    if (st->mode != KST_IDLE || st->st.passes == walk_passes)
        return false;
    walk_on = 0;
    return true;
}

static bool fan_room(void);

/* one restore step: compare up to SCAN_PER_PASS records, apply at most one set */
static void restore_step(param_store_t *store, kst_t *st)
{
    const uint8_t *r;
    uint8_t f[KSU_FRAME_MAX];
    for (uint32_t k = 0; k < SCAN_PER_PASS; k++) {
        int l = ksu_rec(rw.rbuf, rn, pos, &r), d;
        if (l <= 0) {
            ph = PH_RESYNC;
            walk_on = 0;
            return;
        }
        d = ksu_rec_differs(r, store, st);
        if (d == 0) {
            pos += (uint32_t)l;
            continue;
        }
        if (stk_stage_flag != 0) {   /* fn 0x09 staging: a set would only reach the staging copy (E11) */
            if (stage_waits < 0xFF)
                stage_waits++;
            return;
        }
        if (!fan_room())
            return;   /* the controllers' fan-out queues catch up first */
        if (d < 0 && unknown_keys < 0xFFFF)
            unknown_keys++;
        sxc_core_set(f, ksu_set_frame(r, f));
        if (!kst_is_indexed(r[0], r[1], r[2])) {
            bool ch;
            (void)kst_put_frame(store, f, ksu_value_frame(r, f), &ch);   /* the store follows; the walk checks */
        }
        if (applied < 0xFFFF)
            applied++;
        pos += (uint32_t)l;
        return;
    }
}

static uint16_t count_left(const param_store_t *store, const kst_t *st)
{
    const uint8_t *r;
    uint16_t c = 0;
    int l;
    for (uint32_t p = KSU_SNAP_HEAD; (l = ksu_rec(rw.rbuf, rn, p, &r)) > 0; p += (uint32_t)l)
        if (ksu_rec_differs(r, store, st) != 0)
            c++;
    return c;
}

static void capture(uint8_t slot, uint8_t allow, uint8_t for_op)
{
    cap_slot = slot;
    cap_allow = allow;
    cap_for_op = for_op;
    walk_on = 0;
    ph = PH_CAPTURE;
}

static void captured(param_store_t *store, kst_t *st, uint32_t now)
{
    int n = ksu_snap_build(store, st, rw.wbuf, sizeof rw.wbuf);
    ph = PH_READY;
    if (n <= 0) {
        if (save_fails < 0xFFFF)
            save_fails++;
        if (cap_for_op) {
            reply(op_port, op_sub, SU_SAVE_FAILED);
            op_pending = 0;
        }
        flush_req = 0;
        return;
    }
    if (cap_slot == KSU_P)
        ksu_deb_saved(&deb, now);   /* the same-snapshot check is task 24's (it adds the NG part) */
    wn = (uint32_t)n;
    wallow = cap_allow;
    BARRIER();
    wreq = (uint8_t)(1 + cap_slot);   /* task 24 writes it (+ mode current for op 1C 02) */
    flush_req = 0;
}

static void serve_op(void)
{
    if (op_sub == 2) {
        capture(KSU_C, 1, 1);   /* reply from task 24 once written */
        return;
    }
    t24_port = op_port;
    t24_sub = op_sub;
    t24_arg = op_arg;
    BARRIER();
    t24_req = op_sub == 1 ? T24_MODE : T24_CLEAR;
}

void suc_pump(param_store_t *store, kst_t *st, uint32_t changes, uint32_t now)
{
    static uint8_t deb_init = 0;
    boot_check();
    if (!deb_init) {
        deb_init = 1;
        ksu_deb_init(&deb, KSU_QUIET_MS, KSU_GAP_MS);
    }
    changes += ng_changes;   /* our own settings are part of the snapshot too */
    if (op_pending && ph == PH_READY && t24_req == T24_NONE && !wreq)
        serve_op();
    switch (ph) {
    case PH_WAIT:
        if (!loaded || !st->synced)
            return;
        last_changes = changes;
        round_no = 1;
        pos = KSU_SNAP_HEAD;
        if (rn && at_boot) {   /* the mode's snapshot, or in mode stock the block A factory values */
            uint16_t c = 0;
            (void)ksu_snap_check(rw.rbuf, rn, &c);
            recs = c;
            ph = PH_RESTORE;
        } else {
            skipped = mode != KSU_STOCK && rn && !at_boot;
            ph = PH_READY;
        }
        return;
    case PH_RESTORE:
        restore_step(store, st);
        return;
    case PH_RESYNC:
        if (!walk(st))
            return;
        if (round_no < ROUNDS) {
            round_no++;
            pos = KSU_SNAP_HEAD;
            ph = PH_RESTORE;
            return;
        }
        left = count_left(store, st);
        last_changes = changes;   /* our own sets are not a change to save */
        ph = PH_READY;
        return;
    case PH_CAPTURE:
        if (walk(st))
            captured(store, st, now);
        return;
    case PH_READY:
        break;
    default:
        return;
    }
    /* PH_READY: watch for changes (mode power-off) */
    if (redirty) {
        redirty = 0;
        deb.dirty = true;   /* keeps last_change: due again after the gap */
    }
    if (changes != last_changes) {
        last_changes = changes;
        if (mode == KSU_POWEROFF)
            ksu_deb_change(&deb, now);
    }
    if (mode != KSU_POWEROFF) {
        deb.dirty = false;
        flush_req = 0;
        return;
    }
    if (wreq)
        return;
    if (flush_req) {
        if (deb.dirty || !have[1])
            capture(KSU_P, 1, 0);
        else
            flush_req = 0;
        return;
    }
    if (ksu_deb_due(&deb, now) || (deb_on == 0 && !have[1])) {
        deb_on = 1;   /* mode power-off without a P yet: take one now */
        capture(KSU_P, 0, 0);
    }
}

/* ---- task 24 ---------------------------------------------------------------------------------- */
/* the snapshot + NG part as one CRC: a power-off snapshot that equals the last one is not written again */
static uint32_t pair_crc(const uint8_t *snap, uint32_t n, const uint8_t *ng, uint32_t m)
{
    return ca_crc32(ca_crc32(0, snap, n), ng, m);
}

static void load(kvstore_t *kv)
{
    uint32_t g = 0;
    int n, m = -1;
    mode = KSU_STOCK;
    rn = 0;
    boot_check();
    if (kv) {
        kv_ready = 1;
        mode = (uint8_t)ksu_kv_mode(kv);
        mode_all = ksu_kv_mode_all(kv);
        for (int s = 0; s < 2; s++) {
            n = ksu_kv_load(kv, (ksu_slot_t)s, rw.wbuf, sizeof rw.wbuf, &g);   /* wbuf / wng: idle until loaded */
            m = n > 0 ? ksu_kv_load_ng(kv, (ksu_slot_t)s, rw.wng, sizeof rw.wng) : -1;
            have[s] = n > 0;
            have_ng[s] = m > 0;
            sizes[s] = (uint16_t)(n > 0 ? n : 0);
            gens[s] = n > 0 ? g : 0;
            if (s == KSU_P && n > 0)
                crc_p = pair_crc(rw.wbuf, (uint32_t)n, rw.wng, m > 0 ? (uint32_t)m : 0);
        }
        m = -1;
        if (mode != KSU_STOCK) {
            ksu_slot_t s = mode == KSU_CURRENT ? KSU_C : KSU_P;
            n = ksu_kv_load(kv, s, rw.rbuf, sizeof rw.rbuf, NULL);
            rn = n > 0 ? (uint32_t)n : 0;
            m = rn ? ksu_kv_load_ng(kv, s, rw.ng, sizeof rw.ng) : -1;
        }
    }
    ngn = m > 0 ? (uint32_t)m : 0;
    ng_kind = (uint8_t)ksu_ng_decide((ksu_mode_t)mode, mode_all, at_boot, ngn > 0);
    if (ng_kind == KSU_NG_DEFAULTS) {   /* mode stock: AutoPowerOff / LockMode back to the stock's factory values */
        n = ksu_snap_factory(ksu_apo_factory(stk_blka_model, stk_blka_dest), rw.rbuf, sizeof rw.rbuf);
        rn = n > 0 ? (uint32_t)n : 0;
    }
    BARRIER();
    loaded = 1;
}

int suc_ng(const uint8_t **bt, uint8_t *btn, const uint8_t **pw, uint8_t *pwn)
{
    if (!loaded)
        return SUC_NG_WAIT;
    *btn = *pwn = 0;
    if (ng_kind == KSU_NG_SAVED && !ksu_ng_parts(rw.ng, ngn, bt, btn, pw, pwn))
        return KSU_NG_KEEP;
    return ng_kind;
}

void suc_ng_applied(uint8_t bit) { ng_applied |= bit; }

void suc_ng_changed(void) { ng_changes++; }

static void t24_reply(uint8_t rc)
{
    last_rc = rc;
    if (t24_port != 0xFF)
        reply(t24_port, t24_sub, rc);
}

static void write_snapshot(int allow_compact)
{
    uint8_t slot = (uint8_t)(wreq - 1), rc = SU_OK, bt[KSU_NG_BT], pw[KSU_NG_PW];
    uint32_t m, crc;
    kvstore_t *kv;
    int r;
    /* the NG part from the ports' RAM, here in task 24 where they change */
    btc_ng_get(bt);
    pwc_ng_get(pw);
    m = ksu_ng_build(bt, sizeof bt, pw, sizeof pw, rw.wng);
    crc = pair_crc(rw.wbuf, wn, rw.wng, m);
    if (slot == KSU_P && have[1] && crc == crc_p) {   /* nothing of the startup settings changed (or went back) */
        if (skipped_same < 0xFFFF)
            skipped_same++;
        BARRIER();
        wreq = 0;
        return;
    }
    kv = pwc_kv(1);
    r = kv ? ksu_kv_save(kv, (ksu_slot_t)slot, rw.wbuf, wn, rw.wng, m, wallow || allow_compact) : KSU_KV_FAIL;
    if (kv)
        kv_ready = 1;
    if (r == KSU_KV_OK) {
        have_ng[slot] = m > 0;
        if (slot == KSU_P)
            crc_p = crc;
        have[slot] = 1;
        sizes[slot] = (uint16_t)wn;
        gens[slot]++;
        if (saves < 0xFFFF)
            saves++;
        if (cap_for_op && slot == KSU_C && (mode != KSU_CURRENT || !mode_all)) {
            if (ksu_kv_set_mode(kv, KSU_CURRENT)) {
                mode = KSU_CURRENT;
                mode_all = 1;
            } else {
                rc = SU_SAVE_FAILED;
            }
        }
    } else {
        rc = SU_SAVE_FAILED;
        if (r == KSU_KV_NOROOM) {
            if (deferred < 0xFFFF)
                deferred++;
        } else if (save_fails < 0xFFFF) {
            save_fails++;
        }
        if (slot == KSU_P)
            redirty = 1;   /* the next standby / off flush (which may compact) takes it */
    }
    last_rc = (uint8_t)r;
    if (cap_for_op) {
        cap_for_op = 0;
        reply(op_port, op_sub, rc);
        op_pending = 0;
    }
    BARRIER();
    wreq = 0;
}

static void serve_t24(void)
{
    kvstore_t *kv = pwc_kv(1);
    uint8_t rc = SU_SAVE_FAILED;
    if (kv) {
        kv_ready = 1;
        if (t24_req == T24_MODE) {
            if (ksu_kv_set_mode(kv, (ksu_mode_t)t24_arg)) {
                mode = t24_arg;
                mode_all = 1;
                if (mode == KSU_POWEROFF)
                    redirty = 1;   /* a fresh P soon (the one on NOR may be old) */
                rc = SU_OK;
            }
        } else if (ksu_kv_clear(kv)) {
            mode = KSU_STOCK;
            have[0] = have[1] = 0;
            have_ng[0] = have_ng[1] = 0;
            mode_all = 1;   /* ksu_kv_clear: stock chosen by this build (factory everything) */
            sizes[0] = sizes[1] = 0;
            rc = SU_OK;
        }
    }
    t24_reply(rc);
    if (t24_port != 0xFF)
        op_pending = 0;
    BARRIER();
    t24_req = T24_NONE;
}

void suc_tick(int allow_compact)
{
    if (!loaded) {
        if (!pwc_kv_known())
            return;   /* the first probe has not answered: defaults until it does */
        if (rw.guard_lo != GUARD_LO) {
            memset(&rw, 0, sizeof rw);   /* R1 is not zeroed for us */
            rw.guard_lo = GUARD_LO;
            rw.guard_hi = GUARD_HI;
        }
        load(pwc_kv(0));
        return;
    }
    if (!pwc_flash_idle()) {   /* a stock UserMemory / recorder job: our writes wait for it (E10) */
        if (flash_waits < 0xFFFF && (t24_req != T24_NONE || wreq))
            flash_waits++;
        return;
    }
    if (t24_req != T24_NONE)
        serve_t24();
    if (wreq)
        write_snapshot(allow_compact);
}

void suc_flush(void)
{
    if (loaded && mode == KSU_POWEROFF)
        flush_req = 1;
}

int suc_busy(void) { return flush_req || wreq; }

int suc_quiesce(void) { return op_pending || wreq || t24_req != T24_NONE; }

/* Phase 3-4 S2 (sysex_cave.c): the periodic compare waits while the boot restore / its resync walks / a capture walk
 * run (they own the walk) or an op is queued */
int suc_settling(void)
{
    return ph == PH_RESTORE || ph == PH_RESYNC || ph == PH_CAPTURE || (ph == PH_WAIT && loaded) || op_pending;
}

/* defined in sysex_cave.c; declared here to keep the session core's types out of this file */
int sxc_fan_room(void);
static bool fan_room(void) { return sxc_fan_room() != 0; }
