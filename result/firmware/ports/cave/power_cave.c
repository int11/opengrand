/* OpenGrand cave port - power policy, partial standby, BT audio APO rule.  See power_cave.h. */
#include "power_cave.h"

#include <string.h>

#include "ca79ng/power.h"
#include "ca79ng/standby.h"
#include "ca79ng/storage.h"
#include "ca79ng/system.h"
#include "hal_cave.h"
#include "power_stock.h"
#include "bt_cave.h"
#include "ca79ng/recview.h"
#include "sysex_cave.h"
#if PWC_STARTUP
#include "ca79ng/kpstart.h"
#include "startup_cave.h"
#define OFF_WAIT_MS 3000u    /* APO off waits this long at most for the startup snapshot (append: tens of ms) */
#endif

#define GUARD_LO 0x9E5A1C79u
#define GUARD_HI 0xC79E5A1Cu
#define NOR_WINDOW 0x8000u   /* the stock copier moves one 32 KB NOR window at a time */
#define NOR_PAGE 0x200u      /* stock write-buffer routine: <= 512 B, never across a page */
#define NOR_TRIES 200u       /* x dly_tsk(1) while a stock writer owns the chip (writes)       */
#define NOR_READ_TRIES 3000u /* reads wait longer: a failed header read must not look "blank"
                              * (kv_mount would format) - a stock UserMemory save takes ~1-2 s */
#define REQ_MAX 32u
#define BT_RING 0x800u
#define BT_B_PER_MS 16u      /* > UART1 at 114.7 kbaud (11.5 B/ms [추론]): the most a
                              * ring index can move between two passes without a reset            */
#define BT_FLAG_LATCH 0x80u  /* stk_bt_rx_flag: latched after a UART error, ISR drops every byte */
#define BT_FLAG_RESET 0x07u  /* overflow / error seen: the stock assembler resets the ring next  */
#define BT_UNLATCH_MS 500u   /* TV wake: a latch this old (ring empty, module on) is cleared     */
#define KEY_DOWN(b) (((b) & 0xE0u) == 0xE0u)
#define NEED_FOR_STANDBY (PW_HK_HP | PW_HK_EXEC | PW_HK_APO)
/* F-A1: the stock 20 ms body FUN_400ABD74 sets the press counter 0x4139E7F4 to 1 at the press
 * edge, adds 1 per tick and sets the state byte to 1 once it is > 15 (0x400ABFA0-B8, ~0.3 s); the release with state 1 is
 * the real off.  The timer hook runs right before that body: a counter held at 14 here is 15 after the body's +1 - never
 * > 15.  14 and 15 are values the body itself writes on its way up. */
#define BTN_CLAMP 14u
#define BTN_LONG_TICKS 200u  /* 4 s of 20 ms ticks: from here the clamp lets go - the stock state byte 1, real off at release */

enum { KV_UNKNOWN = 0, KV_BLANK = 1, KV_MOUNTED = 2, KV_ERROR = 3 };

/* ---- RW state in R1 (core RW region, zeroed on the first tick, never loaded) -------------- */
typedef struct {
    uint32_t guard_lo;
    standby_t sb;
    bt_audio_t bt;
    apo_hold_t hold;
    power_policy_t pol;
    power_timer_t apo_view;       /* only feeds apo_tick() the stock APO setting */
    kvstore_t kv;
    hal_flash_t flash;
    uint8_t nbuf[NOR_PAGE];       /* NOR read-back / blank check */
    uint32_t guard_hi;
} pw_rw_t;

CAVE_RW static pw_rw_t rw;

/* ---- small state in the payload .data (copied fresh with the payload) --------------------- */
/* shared with the kernel-timer hook / task 19: single aligned stores, one writer each */
static volatile uint8_t ctl_armed = 0;   /* task 24 -> timer: the engine loop owns the decision */
static volatile uint8_t ctl_hold = 0;    /* task 24 -> timer: keep the stock counter at 1 (not ON) */
static volatile uint8_t ctl_stream = 0;  /* task 24 -> timer: the BT stream hold - the stock counter runs (activity
                                          * resets it), only its expiry is stopped      */
static volatile uint8_t rel_req = 0;     /* task 24 -> timer: bumped when the stream hold ends before its cap */
static volatile uint8_t rel_taken = 0;   /* timer only */
static volatile uint8_t ctl_off = 0;     /* task 24 -> timer: real off wanted (state byte 2)    */
static volatile uint8_t ctl_btn = 0;     /* task 24 -> timer: F-A1 - hold the stock press counter at BTN_CLAMP */
static volatile uint8_t btn_rel = 0;     /* timer -> task 24: bumped at the release of a clamped press <= 4 s */
static uint16_t btn_ticks = 0;           /* timer only: 20 ms ticks of the current press */
static uint8_t btn_track = 0;            /* timer only: the press was first seen below the clamp (state byte still 0) */
static uint8_t btn_clamped = 0;          /* timer only: the clamp wrote the counter in this press */
static volatile uint8_t silenced = 0;    /* task 24 -> task 24 hooks: amp / mute are ours       */
static volatile uint32_t apo_expiries = 0; /* timer -> task 24 */
static volatile uint32_t timer_ticks = 0;
static volatile uint32_t timer_writes = 0; /* timer -> task 19: the hook wrote the stock counter (hold / expiry) */
static volatile uint8_t touch_req = 0;   /* task 19 -> task 24 */
static volatile uint16_t touches = 0;
static volatile uint8_t req_pending = 0; /* task 19 -> task 24 */
static uint8_t req_port = 0;
static uint8_t req_len = 0;
static uint8_t req_buf[REQ_MAX] = {0};
/* APO test shortcut 19 04: task 24 asks, the timer hook moves the stock counter (the only place that
 * writes it besides the stock body and the pump tail), the pump tail takes the moved counter as its new base */
static volatile uint32_t jump_left = 0;     /* task 24 -> timer: menu limit - counter wanted, 20 ms ticks (0 = full time) */
static volatile uint8_t jump_req = 0;       /* task 24 -> timer: bumped per accepted 19 04 */
static volatile uint8_t jump_taken = 0;     /* timer only */
static volatile uint32_t jump_seq = 0;      /* timer -> task 19: jumps done */
static volatile uint32_t jump_counter = 0, jump_at = 0, jump_writes = 0;   /* timer -> task 19: the base after a jump */
/* activity frames that had ended when the engine loop accepted the 19 04 (task 24 reads mq_real_seq, the timer hands it to the
 * tail with the jump): a pump pass whose only activity frames ended before that - the request itself - keeps the jump
 * (a poll in the same pass as the 19 04, after the jump, otherwise became the accepted base) */
static volatile uint32_t jump_real_req = 0;  /* task 24 -> timer */
static volatile uint32_t jump_real = 0;      /* timer -> task 19 (with jump_counter, under jump_seq) */
/* task 19 only (the frame ends classify, the pump tail decides): what the current pump pass carried */
static volatile uint8_t mq_quiet = 0;    /* a poll / read frame ended in this pass */
static volatile uint8_t mq_real = 0;     /* an activity frame ended in this pass */
static volatile uint32_t mq_real_seq = 0; /* activity frames ended, never saturates (task 19 writes, task 24 reads) */
static struct {
    uint8_t valid, keys;
    uint8_t sx_state[4];
    uint16_t sx_len[4];
    uint32_t counter, ticks, writes;      /* the stock counter at the last accepted pass, the timer ticks and the
                                           * timer hook's counter writes then */
    uint32_t jumps;                       /* jump_seq taken as the base (19 04) */
    uint16_t undone, quiet, real;         /* op 0x1A 54..59 */
} mq = {0};

static struct {
    uint8_t inited;
    uint8_t hooks;
    uint8_t hp_applied;
    uint8_t kv_state;
    uint8_t keys_were_down;
    uint8_t press_was;
    uint8_t btn_on_press;   /* F-A1: the current power-switch press began while ON */
    uint8_t btn_rel_seen;   /* btn_rel taken */
    uint8_t btn_standbys;   /* F-A1 standby entries (saturating, host tests) */
    uint8_t bt_started;
    uint8_t bt_reset_seen;
    uint16_t bt_rd;
    uint16_t bt_wr_last;
    uint16_t bt_resets, unlatches;
    uint32_t latch_ms;
    uint32_t last_us, acc_us;
    uint32_t expiries_seen;
    uint16_t hp_skips, exec_clears, nor_errors, relight;
    uint8_t remote_offs, remote_refusals;   /* 19 03 done / refused (busy) */
    uint8_t apo_tests;                      /* 19 04 accepted */
#if PWC_STARTUP
    uint8_t off_waiting;
    uint32_t off_wait_ms;
    uint8_t ng_done, pol_dirty;   /* the startup settings' answer taken; a start value to write as the live one */
#endif
} st = {0};

/* ---- 0x7D replies (ngext.c framing and 7-in-8 packing) ------------------------------------ */
static const uint8_t TAG[4] = {0x43, 0x41, 0x37, 0x39};

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

static size_t unpack7(const uint8_t *in, size_t n, uint8_t *out, size_t cap)
{
    size_t o = 0;
    for (size_t i = 0; i < n;) {
        uint8_t hi = in[i++];
        for (size_t k = 0; k < 7 && i < n && o < cap; k++, i++)
            out[o++] = (uint8_t)(in[i] | (((hi >> k) & 1u) << 7));
    }
    return o;
}

static void reply(uint8_t stock_port, uint8_t op, const uint8_t *d, size_t n)
{
    uint8_t m[8 + 72];   /* op 0x1A: 60 raw bytes -> 69 packed */
    if (n > sizeof m - 8)
        return;
    m[0] = 0xF0;
    m[1] = 0x7D;
    memcpy(m + 2, TAG, 4);
    m[6] = (uint8_t)(op | 0x40);
    for (size_t i = 0; i < n; i++)
        m[7 + i] = d[i] & 0x7F;
    m[7 + n] = 0xF7;
    hal_midi_tx(cave_port_from_stock(stock_port), m, (uint16_t)(8 + n));
}

static void policy_reply(uint8_t port, uint8_t sub, uint8_t rc)
{
    uint8_t raw[POWER_POLICY_RAW], d[2 + POWER_POLICY_RAW + 2];
    power_policy_encode(&rw.pol, raw);
    d[0] = sub;
    d[1] = rc;
    reply(port, PW_OP_POLICY, d, 2 + pack7(raw, sizeof raw, d + 2));
}

static void standby_reply(uint8_t port, uint8_t sub, uint8_t rc)
{
    const uint8_t d[3] = {sub, rc, (uint8_t)rw.sb.state};
    reply(port, PW_OP_STANDBY, d, sizeof d);
}

#ifdef PWC_HOST
#define CORE_BOUND(p) pwc_host_core_bound(p)
#else
#define CORE_BOUND(p) sxc_ctrl_bound(p)
#endif
/* any controller the session core has bound (the app stays allowed while the panel holds the stock binding), else
 * the stock binding - the same rule as the BT port's 1D changes (bt_cave.h btc_ctrl_bound) */
static bool bound(uint8_t stock_port) { return btc_ctrl_bound(CORE_BOUND(stock_port), stk_binding, stock_port) != 0; }

/* 59 7F 00 <state> to the bound controllers except the panel (ngext.c ng_standby_notice: the stock panel app's reaction
 * to a 0x7D frame is unverified).  Every stock port 0..2 bound() counts - the session
 * core's table plus the stock binding, the same rule as 19 03 - so the app hears it while the panel holds the stock
 * binding. */
static void notice(void)
{
    const uint8_t d[3] = {0x7F, PW_OK, (uint8_t)rw.sb.state};
    for (uint8_t p = 0; p < 3; p++)   /* stock port 3 = the panel is left out */
        if (bound(p))
            reply(p, PW_OP_STANDBY, d, sizeof d);
}

/* ---- NOR: kvstore over unused_3, guarded --------------------------------------------------- */
/* The whole access inside unused_3 (PW_KV_LO..PW_KV_HI), aligned.  Checked first in every write wrapper (the loader's
 * write service guards again; this keeps the port's own promise: the kvstore sectors only). */
static bool nor_guard(uint32_t a, uint32_t n, uint32_t align)
{
    if (n != 0 && (a & (align - 1)) == 0 && a >= PW_KV_LO && a < PW_KV_HI && n <= PW_KV_HI - a)
        return true;
    st.nor_errors++;
    return false;
}

static bool flash_idle(void) { return !stk_flash_busy() && !stk_flash_jobs(); }

/* Window-bounded reads, the stock way: dispatch off, busy check, window copier.  While a stock
 * writer owns the chip, wait up to max_tries x dly_tsk(1) (0 = one attempt, no wait). */
static bool nor_read_n(uint32_t a, void *dst, uint32_t len, uint32_t max_tries)
{
    uint8_t *d = dst;
    while (len) {
        uint32_t n = NOR_WINDOW - (a & (NOR_WINDOW - 1));
        uint32_t tries = 0;
        bool ok = false;
        if (n > len)
            n = len;
        while (!ok) {
            stk_dis_dsp();
            if (flash_idle()) {
                stk_flash_copy(a, d, n);
                ok = true;
            }
            stk_ena_dsp();
            if (!ok) {
                if (++tries > max_tries) {
                    if (max_tries)
                        st.nor_errors++;
                    return false;
                }
                stk_dly_tsk(1);
            }
        }
        a += n;
        d += n;
        len -= n;
    }
    return true;
}

static bool nor_read(uint32_t a, void *dst, uint32_t len) { return nor_read_n(a, dst, len, NOR_READ_TRIES); }

/* NOR writes: one call of the loader's guarded write service (ota_loader_v4 stage-0 - it owns the chip for the call:
 * dispatch off, stock idle check, the stock busy flag around the stock writer).  -1 = busy (chip untouched), else 0
 * done / non-zero failed or refused.  No service bound (an older loader): a failure, the kvstore stays read-only. */
enum { OWN_BUSY = -1, OWN_NONE = 2 };
static pwc_nor_erase_fn nor_erase_fn = 0;
static pwc_nor_prog_fn nor_prog_fn = 0;

void pwc_nor_bind(pwc_nor_erase_fn erase, pwc_nor_prog_fn prog)
{
    nor_erase_fn = erase;
    nor_prog_fn = prog;
}

static int own_chip_erase(uint32_t a)
{
    int rc = nor_erase_fn ? nor_erase_fn(a) : OWN_NONE;
    return rc == 1 ? OWN_BUSY : rc;
}

static int own_chip_prog(uint32_t a, const void *src, uint32_t n)
{
    int rc = nor_prog_fn ? nor_prog_fn(a, src, n) : OWN_NONE;
    return rc == 1 ? OWN_BUSY : rc;
}

static bool blank(uint32_t a, uint32_t n)
{
    for (uint32_t o = 0; o < n; o += NOR_PAGE) {
        if (!nor_read(a + o, rw.nbuf, NOR_PAGE))
            return false;
        for (uint32_t i = 0; i < NOR_PAGE; i++)
            if (rw.nbuf[i] != 0xFF)
                return false;
    }
    return true;
}

static bool f_erase(void *ctx, uint32_t sector)
{
    (void)ctx;
    if (!nor_guard(sector, PW_KV_SECTOR, PW_KV_SECTOR))
        return false;
    if (blank(sector, PW_KV_SECTOR))
        return true;   /* already erased (this piano's unused_3 is): no erase cycle */
    for (uint32_t tries = 0;; tries++) {
        int rc = own_chip_erase(sector);
        if (rc == 0)
            return blank(sector, PW_KV_SECTOR);
        if (rc != OWN_BUSY || tries >= NOR_TRIES) {
            st.nor_errors++;
            return false;
        }
        stk_dly_tsk(1);
    }
}

static bool f_program(void *ctx, uint32_t sector, uint32_t off, const void *data, uint32_t len)
{
    const uint8_t *s = data;
    uint32_t a = sector + off;
    (void)ctx;
    if (!nor_guard(a, len, 2) || (len & 1))
        return false;
    while (len) {
        uint32_t n = NOR_PAGE - (a & (NOR_PAGE - 1));
        uint32_t tries = 0;
        int rc;
        if (n > len)
            n = len;
        memcpy(rw.nbuf, s, n);   /* the stock writer reads its source from RAM we own */
        while ((rc = own_chip_prog(a, rw.nbuf, n)) == OWN_BUSY && tries++ < NOR_TRIES)
            stk_dly_tsk(1);
        if (rc != 0) {
            st.nor_errors++;
            return false;
        }
        if (!nor_read(a, rw.nbuf, n) || memcmp(rw.nbuf, s, n) != 0) {
            st.nor_errors++;
            return false;
        }
        a += n;
        s += n;
        len -= n;
    }
    return true;
}

static bool f_read(void *ctx, uint32_t sector, uint32_t off, void *data, uint32_t len)
{
    (void)ctx;
    return nor_read(sector + off, data, len);
}

/* -1 = the headers could not be read (stock writer busy too long), 0 = no store (neither sector
 * starts with the kvstore magic), 1 = a store exists. */
static int kv_exists(uint32_t max_tries)
{
    static const uint32_t magic = 0x3153564Bu;   /* "KVS1", kvstore.c */
    uint32_t h0 = 0, h1 = 0;
    if (!nor_read_n(PW_KV_LO, &h0, 4, max_tries) || !nor_read_n(PW_KV_LO + PW_KV_SECTOR, &h1, 4, max_tries))
        return -1;
    return h0 == magic || h1 == magic;
}

/* Mount only a store that exists: two blank sectors stay unmounted (defaults) until the first
 * save, so a boot never erases anything.  One attempt per engine-loop pass: while a stock writer
 * owns the chip the state stays unknown (defaults) and the next pass asks again - no waiting. */
static void kv_probe(void)
{
    int e = kv_exists(0);
    if (e < 0)
        return;
    if (e == 0) {
        st.kv_state = KV_BLANK;
        return;
    }
    st.kv_state = kv_mount(&rw.kv, &rw.flash, PW_KV_LO, PW_KV_LO + PW_KV_SECTOR) ? KV_MOUNTED : KV_ERROR;
    if (st.kv_state == KV_MOUNTED) {
        uint8_t raw[POWER_POLICY_RAW];
        int n = kv_get(&rw.kv, KV_KEY_POWER_POLICY, raw, sizeof raw);
        if (n > 0)
            (void)power_policy_decode(&rw.pol, raw, (uint16_t)n);   /* bad record: defaults */
    }
}

static bool kv_save(void)
{
    uint8_t raw[POWER_POLICY_RAW];
    if (st.kv_state != KV_MOUNTED) {
        /* never mount on an unreadable header: kv_mount formats when it sees no valid header */
        if (kv_exists(NOR_READ_TRIES) < 0)
            return false;
        st.kv_state = kv_mount(&rw.kv, &rw.flash, PW_KV_LO, PW_KV_LO + PW_KV_SECTOR) ? KV_MOUNTED : KV_ERROR;
        if (st.kv_state != KV_MOUNTED)
            return false;
    }
    power_policy_encode(&rw.pol, raw);
    return kv_put(&rw.kv, KV_KEY_POWER_POLICY, raw, sizeof raw);
}

#if PWC_STARTUP
/* startup_cave.c (task 24): the same kvstore, mounted the way kv_save() does (never on an unreadable header) */
int pwc_kv_known(void) { return st.inited && st.kv_state != KV_UNKNOWN; }

/* no stock writer owns the chip (no UserMemory / RecorderData job): a snapshot write waits for this instead of
 * failing after NOR_TRIES (a stock UserMemory save takes ~1-4 s) */
int pwc_flash_idle(void) { return flash_idle(); }

kvstore_t *pwc_kv(int mount)
{
    if (st.kv_state == KV_MOUNTED)
        return &rw.kv;
    if (!mount || kv_exists(NOR_READ_TRIES) < 0)
        return 0;
    st.kv_state = kv_mount(&rw.kv, &rw.flash, PW_KV_LO, PW_KV_LO + PW_KV_SECTOR) ? KV_MOUNTED : KV_ERROR;
    return st.kv_state == KV_MOUNTED ? &rw.kv : 0;
}
#endif

/* ---- standby ops -> stock routines (task 24) ----------------------------------------------- */
static void o_cleanup(void *c) { (void)c; stk_cleanup(); }

static void o_amp(void *c, bool on)
{
    (void)c;
    if (!on)
        stk_amp_off();
    else if (stk_gp3_level != 1)
        stk_amp_on();          /* GP3[8] alarm: the stock event 1 switches it on when it clears */
}

static void o_mute(void *c, bool on)
{
    (void)c;
    if (on)
        stk_mute();
    else
        stk_unmute();
}

static bool o_bt_enabled(void *c) { (void)c; return stk_bt_audio_cfg != 0; }

static void o_bt(void *c, bool on)
{
    (void)c;
    if (on) {
        stk_bt_audio_on();
        stk_bt_flags_clear();
    } else {
        stk_bt_audio_off();
    }
}

/* wake step 4 re-reads and applies once (the poll was skipped while silenced), step 8 only reads */
static bool o_headphones(void *c)
{
    (void)c;
    if (!st.hp_applied) {
        st.hp_applied = 1;
        stk_hp_apply();
    }
    return stk_hp_present() != 0;
}

static bool o_alarm(void *c) { (void)c; return stk_gp3_level == 1; }

static bool o_busy(void *c) { (void)c; return !flash_idle(); }

static bool any_key_down(void)
{
    if (!stk_key_scan_on)
        return false;
    for (int i = 0; i < 88; i++)
        if (KEY_DOWN(stk_key_state[i]))
            return true;
    return false;
}

static void o_notify(void *c, standby_note_t n)
{
    (void)c;
    if (n == SB_NOTE_STANDBY) {
        silenced = 1;
        st.hp_applied = 0;
        st.keys_were_down = any_key_down();   /* wake on a new press only */
        st.press_was = stk_power_press != 0;
        touch_req = 0;
    }
    notice();
}

static void o_power_off(void *c)
{
    (void)c;
    ctl_off = 1;   /* the stock executor does the rest: 7E 00 00, cleanup, final cut */
    stk_power_req = 2;
}

/* ---- init ---------------------------------------------------------------------------------- */
static void apply_policy(void)
{
    apo_hold_init(&rw.hold, &rw.pol.hold);
    rw.sb.cap_ms = rw.pol.standby_cap_ms;
    rw.sb.keep_bt = !rw.pol.standby_bt_off;   /* takes effect at the next standby */
}

static void init(uint32_t now)
{
    static const standby_ops_t ops = {
        .ctx = 0, .cleanup = o_cleanup, .amp = o_amp, .mute = o_mute, .bt_audio_enabled = o_bt_enabled,
        .bt_audio = o_bt, .codec = 0, .headphones = o_headphones, .amp_alarm = o_alarm,
        .flash_busy = o_busy, .notify = o_notify, .power_off = o_power_off};
    memset(&rw, 0, sizeof rw);
    rw.guard_lo = GUARD_LO;
    rw.guard_hi = GUARD_HI;
    rw.flash.ctx = 0;
    rw.flash.sector_size = PW_KV_SECTOR;
    rw.flash.erase = f_erase;
    rw.flash.program = f_program;
    rw.flash.read = f_read;
    power_policy_default(&rw.pol);
    standby_init(&rw.sb, &ops, rw.pol.standby_cap_ms, false);
    bt_audio_init(&rw.bt);
    power_timer_init(&rw.apo_view, 0);
    apply_policy();
    st.last_us = now;
    st.acc_us = 0;
    st.expiries_seen = apo_expiries;
    st.inited = 1;
    ctl_armed = 1;
}

/* ---- inputs -------------------------------------------------------------------------------- */
/* BT module lines from the stock UART1 RX ring, with our own read index (the stock assembler
 * keeps its own).  The first pass replays the ring, oldest byte first, so a payload applied in
 * the middle of a stream still sees its START.  NUL bytes (never sent by the module) are skipped:
 * the ring is zero until the module has written it.
 * Ring resets: the stock assembler resets the ring (FUN_40145C00: write index 0)
 * after an overflow or a UART error; read on from the old index and the old bytes come back as
 * new lines (a stale STREAMING START).  A reset is taken (a) while the flag shows a pending reset
 * or the 0x80 latch - nothing is read, and once it is clear reading restarts at 0 - and (b) when
 * the write index moved back further than the module can have sent since the last pass (the
 * flag pulse is too short to see every time).  The partial line is dropped at a reset. */
static void bt_ring_reset(uint16_t rd)
{
    st.bt_rd = rd;
    rw.bt.len = 0;
    rw.bt.overflow = false;
    if (st.bt_resets < 0xFFFFu)
        st.bt_resets++;
}

static void bt_feed(uint32_t elapsed_ms)
{
    uint8_t chunk[32];
    uint32_t n = 0, left;
    uint16_t wr = (uint16_t)(stk_bt_rx_wr & (BT_RING - 1));
    uint8_t flag = stk_bt_rx_flag;
    if (!st.bt_started) {
        st.bt_started = 1;
        st.bt_rd = (uint16_t)((wr + 1) & (BT_RING - 1));
        left = BT_RING - 1;
    } else if (flag & (BT_FLAG_LATCH | BT_FLAG_RESET)) {
        if (!st.bt_reset_seen)
            bt_ring_reset(wr);
        st.bt_reset_seen = 1;
        st.bt_rd = wr;
        left = 0;
    } else {
        uint32_t adv = (uint32_t)((wr - st.bt_wr_last) & (BT_RING - 1));
        if (st.bt_reset_seen)
            st.bt_rd = 0;                       /* (a) the reset is done: bytes 0..wr are new */
        else if (wr < st.bt_wr_last && adv > 32u + elapsed_ms * BT_B_PER_MS)
            bt_ring_reset(0);                   /* (b) */
        st.bt_reset_seen = 0;
        left = (uint32_t)((wr - st.bt_rd) & (BT_RING - 1));
    }
    st.bt_wr_last = wr;
    while (left--) {
        uint8_t ch = stk_bt_rx_ring[st.bt_rd];
        st.bt_rd = (uint16_t)((st.bt_rd + 1) & (BT_RING - 1));
        if (ch == 0)
            continue;
        chunk[n++] = ch;
        if (n == sizeof chunk) {
            bt_audio_rx(&rw.bt, chunk, (uint16_t)n);
            n = 0;
        }
    }
    if (n)
        bt_audio_rx(&rw.bt, chunk, (uint16_t)n);
    /* With the BT audio source manager (bt_cave.c) the policy closes old sources: the stock A2DP flag keeps one
     * link id only (the first one) and drops to 0 when exactly that link closes, although
     * the newer source is still connected and streaming.  The manager's link table says whether any A2DP link
     * is left. */
    bt_audio_link(&rw.bt, (stk_a2dp_flag != 0 || btc_a2dp_links() != 0) && stk_bt_power != 0);
}

/* bt_cave.c: commands to the module only while the piano is on (never in standby, while waking, before off) */
int pwc_on(void)
{
    return st.inited && rw.sb.state == SB_ON;
}

/* One UART1 error leaves the RX flag at 0x80 - the ISR drops every byte and the
 * assembler never clears it (only when it takes a byte out of the ring), so the BT lines stop until
 * the next boot.  A latch that has lasted BT_UNLATCH_MS with the ring empty and the module on is
 * cleared with the stock reset itself (the boot path's FUN_40145C00, same store order: write index,
 * flag, read index - the ISR writes nothing while the flag is non-zero, and the indices are already
 * equal, so neither the ISR nor the assembler can see a half-reset ring). */
static void bt_unlatch(uint32_t elapsed_ms)
{
    if (stk_bt_rx_flag != BT_FLAG_LATCH || stk_bt_rx_rd != stk_bt_rx_wr || stk_bt_power == 0) {
        st.latch_ms = 0;
        return;
    }
    st.latch_ms += elapsed_ms;
    if (st.latch_ms < BT_UNLATCH_MS)
        return;
    st.latch_ms = 0;
    stk_bt_rx_reset();
    st.bt_reset_seen = 1;                       /* bt_feed: read from 0 */
    if (st.unlatches < 0xFFFFu)
        st.unlatches++;
}

static bool standby_possible(void) { return (st.hooks & NEED_FOR_STANDBY) == NEED_FOR_STANDBY; }

/* what an APO expiry does from ON (and the remote off 19 03): policy bit0 + the hooks -> partial standby, else the
 * real off (the stock executor) - after the startup snapshot when the core owns it, never under a pending flash job */
static void apo_action(void)
{
#if PWC_STARTUP
    suc_flush();   /* mode power-off: the snapshot first */
    if (rw.pol.apo_standby && standby_possible()) {
        standby_enter(&rw.sb);
    } else {
        st.off_waiting = 1;   /* the real off once the snapshot is written (or OFF_WAIT_MS) */
        st.off_wait_ms = 0;
    }
#else
    if (rw.pol.apo_standby && standby_possible())
        standby_enter(&rw.sb);
    else
        standby_hard_off(&rw.sb);   /* the stock APO, but never under a pending flash job */
#endif
}

/* F-A1: the timer hook saw the release of a press it held below the stock threshold (0.3-4 s).  Begun ON (and still
 * ON, no stock state byte, no off on its way) -> partial standby, whatever the APO policy says - the button is the
 * owner switching off by hand; the 19 01 path (startup snapshot first).  Begun in standby: the press edge woke the
 * piano (wake_inputs) and the release does nothing more. */
static void btn_release(void)
{
    if (!st.btn_on_press || rw.sb.state != SB_ON || stk_power_req != 0 || !standby_possible())
        return;
#if PWC_STARTUP
    if (st.off_waiting)
        return;
    suc_flush();
#endif
    if (standby_enter(&rw.sb) && st.btn_standbys < 0xFFu)
        st.btn_standbys++;
}

static bool do_wake(standby_wake_t src)
{
    if (!standby_wake(&rw.sb, src))
        return false;
    silenced = 0;   /* after steps 1-6: the poll and the GP3[8] events are the stock's again */
    return true;
}

static void wake_inputs(void)
{
    bool keys = any_key_down(), press = stk_power_press != 0, touch = touch_req != 0;
    touch_req = 0;
    if (press && !st.press_was)
        st.btn_on_press = rw.sb.state == SB_ON;   /* F-A1: only a press begun ON may end in standby */
    if (rw.sb.state == SB_STANDBY) {
        if (touch)
            do_wake(SB_WAKE_PANEL_TOUCH);
        else if (press && !st.press_was)
            do_wake(SB_WAKE_POWER_BUTTON);
        else if (keys && !st.keys_were_down)
            do_wake(SB_WAKE_KEY);
    }
    st.keys_were_down = keys;
    st.press_was = press;
}

/* ---- ops ----------------------------------------------------------------------------------- */
static void op_policy(uint8_t port, const uint8_t *a, uint32_t n)
{
    uint8_t sub;
    if (n < 1)
        return;
    sub = a[0];
    if (sub == 0x00) {
        policy_reply(port, sub, PW_OK);
    } else if (sub == 0x01 || sub == 0x02) {
        power_policy_t pol = rw.pol;
        if (!bound(port)) {
            policy_reply(port, sub, PW_NOT_BOUND);
            return;
        }
        if (sub == 0x01) {
            uint8_t raw[POWER_POLICY_RAW + 7];
            size_t got = unpack7(a + 1, n - 1, raw, sizeof raw);
            if (!power_policy_decode(&pol, raw, (uint16_t)got)) {
                policy_reply(port, sub, PW_BAD_VALUE);
                return;
            }
        } else {
            power_policy_default(&pol);
        }
#if PWC_STARTUP
        {
            uint8_t was[POWER_POLICY_RAW], now_[POWER_POLICY_RAW];
            power_policy_encode(&rw.pol, was);
            power_policy_encode(&pol, now_);
            if (memcmp(was, now_, sizeof was) != 0)
                suc_ng_changed();   /* the power-off snapshot follows */
        }
#endif
        rw.pol = pol;
        apply_policy();
        policy_reply(port, sub, kv_save() ? PW_OK : PW_SAVE_FAILED);
    }
}

#if PWC_STARTUP
/* the policy now, for the startup snapshot's NG part (startup_cave.h; task 24) */
void pwc_ng_get(uint8_t *out) { power_policy_encode(&rw.pol, out); }

/* the startup settings' answer for the policy, once the store is probed (rw.pol = the live
 * value on NOR): defaults (mode stock), the snapshot's policy (current / power-off) or the live value (a live apply, a
 * mode / snapshot of a build before); a changed start value is written back as the live value when the chip is free */
static void ng_tick(void)
{
    if (!st.ng_done && st.kv_state != KV_UNKNOWN) {
        const uint8_t *bt = 0, *pw = 0;
        uint8_t btn = 0, pwn = 0, was[POWER_POLICY_RAW], now_[POWER_POLICY_RAW];
        power_policy_t pol = rw.pol;
        int k = suc_ng(&bt, &btn, &pw, &pwn);
        if (k == SUC_NG_WAIT)
            return;
        st.ng_done = 1;
        if (k == KSU_NG_DEFAULTS)
            power_policy_default(&pol);
        else if (k == KSU_NG_SAVED && pwn && !power_policy_decode(&pol, pw, pwn))
            pol = rw.pol;   /* a bad part keeps the live value */
        power_policy_encode(&rw.pol, was);
        power_policy_encode(&pol, now_);
        if (memcmp(was, now_, sizeof was) != 0) {
            rw.pol = pol;
            apply_policy();
            st.pol_dirty = 1;
            suc_ng_applied(SUC_NG_PW);
        }
    }
    if (st.pol_dirty && pwc_flash_idle()) {
        st.pol_dirty = 0;
        if (!kv_save() && st.nor_errors < 0xFFFF)
            st.nor_errors++;
    }
}
#endif

/* ---- remote off (feature 2) ------------------------------------------- */
#ifdef PWC_HOST
static void mem_read(void *ctx, uint32_t addr, void *dst, uint32_t n)
{
    (void)ctx;
    pwc_host_read(addr, dst, n);
}
#else
static void mem_read(void *ctx, uint32_t addr, void *dst, uint32_t n)
{
    const volatile uint8_t *src = (const volatile uint8_t *)(uintptr_t)addr;
    uint8_t *d = dst;
    (void)ctx;
    for (uint32_t i = 0; i < n; i++)
        d[i] = src[i];
}
#endif

/* why the piano cannot go off now (POWER_OFF_*): the stock recorder state, its file executor and the NOR jobs, read
 * through the core's recorder view (recview.h - the same reads the playback control refuses on) + the chip owner */
static uint8_t off_blocker(void)
{
    static const rv_mem_t mem = {mem_read, 0};
    rv_view_t v;
    rv_read_live(&mem, &v);
    return power_remote_off_check((v.busy & RV_BUSY_REC) != 0, (v.busy & RV_BUSY_FILEOP) != 0,
                                  (v.busy & (RV_BUSY_JOB | RV_BUSY_WORKER)) != 0 || !flash_idle());
}

/* 19 03: bound controller (core table) -> refused while recording / a file operation / a flash job (rc 6 + reason),
 * not ON -> rc 3, else what an APO expiry does.  59 03 <rc> <state> <reason>; the standby notice follows as usual. */
static void op_remote_off(uint8_t port)
{
    uint8_t rc, why = POWER_OFF_OK;
    if (!bound(port)) {
        rc = PW_NOT_BOUND;
    } else if ((why = off_blocker()) != POWER_OFF_OK) {
        rc = PW_BUSY;
    } else if (rw.sb.state != SB_ON) {
        rc = PW_BAD_STATE;
    }
#if PWC_STARTUP
    else if (st.off_waiting) {
        rc = PW_BAD_STATE;   /* an off is already on its way */
    }
#endif
    else {
        apo_action();
        rc = PW_OK;
        if (st.remote_offs < 0xFFu)
            st.remote_offs++;
    }
    if (rc == PW_BUSY && st.remote_refusals < 0xFFu)
        st.remote_refusals++;
    {
        const uint8_t d[4] = {PW_SB_REMOTE_OFF, rc, (uint8_t)rw.sb.state, why};
        reply(port, PW_OP_STANDBY, d, sizeof d);
    }
}

/* 19 04 <s_hi7> <s_lo7> (or 19 04 00): the APO test shortcut.  Bound controller -> the APO must count
 * (menu 15 / 60 / 120 min, no special mode; else rc 7) -> s = 0 or 10..600 (rc 1) -> ON (rc 3) -> the timer hook moves
 * the stock counter to menu limit - s x 50 on its next tick (RAM only).  Everything after is the normal path: an activity
 * reset gives the full menu time back, the stream hold stops its expiry (STOP gives the full menu time back), the expiry
 * goes to apo_action().  59 04 <rc> <state> <s_hi7> <s_lo7> <menu setting> <flags: 1 = the BT hold holds now>. */
static void op_apo_test(uint8_t port, const uint8_t *a, uint32_t n)
{
    uint32_t secs = n >= 3 ? ((uint32_t)(a[1] & 0x7Fu) << 7) | (a[2] & 0x7Fu) : n == 2 ? (uint32_t)(a[1] & 0x7Fu) : 0xFFFFu;
    uint8_t set = stk_apo_setting, rc;
    if (!bound(port)) {
        rc = PW_NOT_BOUND;
    } else if (set == 0 || set >= 4 || stk_special_mode == 1) {
        rc = PW_APO_OFF;
    } else if (!power_apo_test_secs_ok(secs)) {
        rc = PW_BAD_VALUE;
    } else if (rw.sb.state != SB_ON
#if PWC_STARTUP
               || st.off_waiting
#endif
    ) {
        rc = PW_BAD_STATE;
    } else {
        jump_left = secs * 50u;   /* 20 ms ticks (FUN_400ABFE8 counts one per 20 ms kernel-timer tick) */
        jump_real_req = mq_real_seq;   /* the request's own frame (and any before it) is behind the jump */
        jump_req = (uint8_t)(jump_req + 1u);
        rc = PW_OK;
        if (st.apo_tests < 0xFFu)
            st.apo_tests++;
    }
    {
        const uint8_t d[7] = {PW_SB_APO_TEST, rc, (uint8_t)rw.sb.state, (uint8_t)((secs >> 7) & 0x7Fu),
                              (uint8_t)(secs & 0x7Fu), set, (uint8_t)(rw.hold.holding ? 0x01 : 0)};
        reply(port, PW_OP_STANDBY, d, sizeof d);
    }
}

static void op_standby(uint8_t port, const uint8_t *a, uint32_t n)
{
    uint8_t sub;
    if (n < 1)
        return;
    sub = a[0];
    if (sub == 0x00) {
        standby_reply(port, sub, PW_OK);
    } else if (sub == 0x01) {
        uint8_t rc;
#if PWC_STARTUP
        if (bound(port) && standby_possible())
            suc_flush();   /* the startup snapshot is written on the next passes (the piano stays powered) */
#endif
        rc = !bound(port) ? PW_NOT_BOUND
             : !standby_possible() ? PW_NO_HOOKS
             : standby_enter(&rw.sb) ? PW_OK : PW_BAD_STATE;
        standby_reply(port, sub, rc);
    } else if (sub == 0x02) {
        standby_reply(port, sub, do_wake(SB_WAKE_BLE_OP) ? PW_OK : PW_BAD_STATE);
    }
    else if (sub == PW_SB_REMOTE_OFF) {
        op_remote_off(port);
    } else if (sub == PW_SB_APO_TEST) {
        op_apo_test(port, a, n);
    }
}

static void put32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24);
}

static void put16(uint8_t *p, uint32_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }

void pwc_diag(uint8_t raw[PW_DIAG_RAW])
{
    memset(raw, 0, PW_DIAG_RAW);
    raw[0] = PW_DIAG_LAYOUT;
    raw[1] = st.hooks;
    raw[2] = (uint8_t)rw.sb.state;
    raw[3] = (uint8_t)((silenced ? 0x01 : 0) | (ctl_hold ? 0x02 : 0) | (ctl_off ? 0x04 : 0) |
                       (rw.hold.holding ? 0x08 : 0) | (rw.hold.capped ? 0x10 : 0) |
                       (stk_gp3_level == 1 ? 0x20 : 0) | (stk_hp_present() ? 0x40 : 0) |
                       (st.inited && rw.guard_lo == GUARD_LO && rw.guard_hi == GUARD_HI ? 0x80 : 0));
    raw[4] = (uint8_t)bt_audio_state(&rw.bt);
    raw[5] = st.kv_state;
    raw[6] = stk_apo_setting;
    raw[7] = (uint8_t)rw.sb.last_wake;
    put32(raw + 8, stk_apo_counter);
    put32(raw + 12, timer_ticks);
    put32(raw + 16, apo_expiries);
    put32(raw + 20, rw.sb.t_ms);
    put32(raw + 24, rw.hold.held_ms);
    put32(raw + 28, rw.kv.generation);
    put32(raw + 32, rw.kv.write_off);
    put16(raw + 36, st.hp_skips);
    put16(raw + 38, st.exec_clears);
    put16(raw + 40, touches);
    put16(raw + 42, st.nor_errors);
    put16(raw + 44, (uint16_t)rw.bt.lines);
    raw[46] = (uint8_t)rw.bt.starts;
    raw[47] = (uint8_t)(st.relight > 255 ? 255 : st.relight);
    raw[48] = (uint8_t)((stk_bt_power != 0 ? 0x01 : 0) | (rw.sb.keep_bt ? 0x02 : 0) |
                        (stk_bt_rx_flag == BT_FLAG_LATCH ? 0x04 : 0) | (ctl_btn ? 0x08 : 0) | (btn_clamped ? 0x10 : 0));
    raw[49] = stk_bt_rx_flag;
    raw[50] = (uint8_t)(st.unlatches > 255 ? 255 : st.unlatches);
    raw[51] = (uint8_t)(st.bt_resets > 255 ? 255 : st.bt_resets);
    raw[52] = st.remote_offs;
    raw[53] = st.remote_refusals;
    put16(raw + 54, mq.undone);
    put16(raw + 56, mq.quiet);
    put16(raw + 58, mq.real);
}

static void op_diag(uint8_t port)
{
    uint8_t raw[PW_DIAG_RAW], d[PW_DIAG_RAW + (PW_DIAG_RAW + 6) / 7];
    pwc_diag(raw);
    reply(port, PW_OP_DIAG, d, pack7(raw, sizeof raw, d));
}

static void serve_request(void)
{
    uint8_t port = req_port, op = req_buf[6];
    const uint8_t *a = req_buf + 7;
    uint32_t n = (uint32_t)req_len - 8;
    if (op == PW_OP_POLICY)
        op_policy(port, a, n);
    else if (op == PW_OP_STANDBY)
        op_standby(port, a, n);
    else if (op == PW_OP_DIAG)
        op_diag(port);
    req_pending = 0;
}

/* ---- entry points -------------------------------------------------------------------------- */
void pwc_sysex(uint8_t stock_port, const uint8_t *buf, uint32_t len)
{
    /* the cave checked F0 7D C A 7 9 and len >= 8; the request runs on the next engine-loop pass,
     * so every piece of power state is touched by task 24 only.  Busy: dropped (silent). */
    if (req_pending || len < 8 || len > REQ_MAX || buf[len - 1] != 0xF7)
        return;
    memcpy(req_buf, buf, len);
    req_len = (uint8_t)len;
    req_port = stock_port & 3;
    req_pending = 1;
}

void pwc_hooks_attached(uint8_t mask) { st.hooks = mask; }

void pwc_tick(void)
{
    uint32_t now = hal_time_us(), elapsed;
    if (!st.inited) {
        init(now);
        elapsed = 0;
    } else {
        st.acc_us += now - st.last_us;
        st.last_us = now;
        elapsed = st.acc_us / 1000u;
        st.acc_us -= elapsed * 1000u;
    }
    if (st.kv_state == KV_UNKNOWN) {
        kv_probe();
        if (st.kv_state != KV_UNKNOWN)
            apply_policy();
    }
    bt_unlatch(elapsed);
    bt_feed(elapsed);
    if (standby_bt_stream(&rw.sb, rw.bt.starts))
        silenced = 0;                           /* TV wake: as do_wake() */
    wake_inputs();
    if (btn_rel != st.btn_rel_seen) {
        st.btn_rel_seen = btn_rel;
        btn_release();
    }
    if (req_pending)
        serve_request();

    /* the APO rule on the stock counter: hold while streaming (policy) and while not ON */
    rw.apo_view.setting = stk_apo_setting < 4 ? stk_apo_setting : 0;
    /* the streaming source's AVRCP play status from the source manager: a source that reports
     * paused / stopped does not hold although its stream stays open (Windows) - one engine-loop pass old */
    bt_audio_play(&rw.bt, btc_stream_playing(rw.bt.stream_link));
    (void)apo_tick(&rw.hold, &rw.apo_view, &rw.bt, elapsed);
    power_timer_activity(&rw.apo_view);
    if (rw.hold.released)
        rel_req = (uint8_t)(rel_req + 1u);   /* STOP / link loss before the cap: the full menu time from here */
    if (apo_expiries != st.expiries_seen) {
        st.expiries_seen = apo_expiries;
        if (rw.sb.state == SB_ON)
            apo_action();
    }
#if PWC_STARTUP
    suc_tick(rw.sb.state != SB_ON || st.off_waiting);   /* load / write the startup settings (may compact off ON) */
    ng_tick();                                          /* ... and the policy's start value */
    if (st.off_waiting) {
        st.off_wait_ms += elapsed;
        if (!suc_busy() || st.off_wait_ms >= OFF_WAIT_MS) {
            st.off_waiting = 0;
            if (rw.sb.state == SB_ON)
                standby_hard_off(&rw.sb);   /* the stock APO, but never under a pending flash job */
        }
    }
#endif
    standby_tick(&rw.sb, elapsed);
    ctl_stream = (uint8_t)rw.hold.holding;
    ctl_hold = (uint8_t)(rw.sb.state != SB_ON);
    /* F-A1 needs the standby hooks and the saved policy (bit3 = the stock button) */
    ctl_btn = (uint8_t)(st.kv_state != KV_UNKNOWN && !rw.pol.button_stock && standby_possible());
    if (ctl_off)
        stk_power_req = 2;   /* re-asserted until the executor takes it (activity resets clear it) */
}

int pwc_quiesce(void)
{
    if (req_pending)
        return 1;
#if PWC_STARTUP
    if (st.off_waiting)
        return 1;
#endif
    if (!st.inited)
        return 0;
    if (rw.sb.state == SB_STANDBY)
        do_wake(SB_WAKE_BLE_OP);   /* never hand a silenced piano to the next cave */
    return rw.sb.state != SB_ON;
}

void pwc_restart(void)
{
    ctl_armed = 0;
    ctl_hold = 0;
    ctl_stream = 0;
    rel_req = 0;
    rel_taken = 0;
    ctl_off = 0;
    ctl_btn = 0;
    btn_rel = 0;
    btn_ticks = 0;
    btn_track = 0;
    btn_clamped = 0;
    silenced = 0;
    req_pending = 0;
    touch_req = 0;
    touches = 0;
    apo_expiries = 0;
    timer_ticks = 0;
    timer_writes = 0;
    memset(&st, 0, sizeof st);
    jump_left = 0;
    jump_req = 0;
    jump_taken = 0;
    jump_seq = 0;
    jump_counter = 0;
    jump_at = 0;
    jump_writes = 0;
    jump_real_req = 0;
    jump_real = 0;
    mq_quiet = 0;
    mq_real = 0;
    mq_real_seq = 0;
    memset(&mq, 0, sizeof mq);
}

/* ---- hook bodies --------------------------------------------------------------------------- */
/* F-A1, timer context, before FUN_400ABD74 (nothing runs in between, no other code touches 0x4139E7F4):
 * count the press, hold the stock counter at BTN_CLAMP while it is shorter than 4 s, report the release. */
static inline __attribute__((always_inline)) void btn_timer(void)
{
    uint32_t c = stk_power_press;
    if (c == 0) {
        if (btn_clamped && btn_ticks <= BTN_LONG_TICKS)   /* tick 200 itself was not clamped: released on it = state 0 */
            btn_rel = (uint8_t)(btn_rel + 1u);
        btn_ticks = 0;
        btn_track = 0;
        btn_clamped = 0;
        return;
    }
    if (btn_ticks == 0)
        btn_track = c <= BTN_CLAMP;   /* a press found above it (hook armed mid-press) is the stock's */
    if (btn_ticks < 0xFFFFu)
        btn_ticks++;
    if (!btn_track || !ctl_btn || btn_ticks >= BTN_LONG_TICKS)
        return;                       /* hook off / policy bit3 / no standby hooks / held 4 s: the stock goes on */
    if (c > BTN_CLAMP) {
        stk_power_press = BTN_CLAMP;
        btn_clamped = 1;
    }
}

/* 20 ms kernel timer, before FUN_400ABFE8: plain loads and stores, no call, no wait. */
void pwc_apo_timer(void)
{
    uint8_t s;
    timer_ticks++;
    if (!ctl_armed)
        return;
    if (ctl_off) {
        stk_power_req = 2;
        return;
    }
    btn_timer();
    if (rel_req != rel_taken) {     /* the stream hold ended before its cap: the menu time counts from now */
        rel_taken = rel_req;
        stk_apo_counter = 1;        /* = FUN_400ACB84's reset value */
        timer_writes++;
    }
    if (jump_req != jump_taken) {   /* 19 04: menu limit - counter = the asked time (dropped while not ON / not counting) */
        uint32_t left = jump_left;
        jump_taken = jump_req;
        s = stk_apo_setting;
        if (!ctl_hold && stk_special_mode != 1 && s >= 1 && s <= 3 && left < stk_apo_limits[s]) {
            uint32_t v = left ? stk_apo_limits[s] - left : 1u;   /* expires `left` ticks from now (test below) */
            stk_apo_counter = v;
            timer_writes++;
            jump_counter = v + 1u;                               /* FUN_400ABFE8 adds this tick's 1 right after */
            jump_at = timer_ticks;
            jump_writes = timer_writes;
            jump_real = jump_real_req;
            jump_seq++;
        }
    }
    if (ctl_hold) {
        stk_apo_counter = 1;   /* = FUN_400ACB84's reset value; the full APO time counts after */
        timer_writes++;
        return;
    }
    s = stk_apo_setting;
    if (stk_special_mode == 1 || s == 0 || s >= 4)
        return;
    if (stk_apo_limits[s] < stk_apo_counter + 1) {   /* the stock test at 0x400ABF7C-0x400ABF80 */
        if (ctl_stream) {
            /* the BT stream hold: no expiry - the stock body adds 1 right after, so the counter rests at the limit
             * (no underflow in its remaining-time display 0x400ABD9C-A4).  Not a timer write: the pump tail's base
             * stays good (a counter it puts back near the limit is clamped here again) */
            stk_apo_counter = stk_apo_limits[s] - 1u;
            return;
        }
        stk_apo_counter = 1;                         /* no stock expiry: the engine loop decides */
        apo_expiries++;
        timer_writes++;
    }
}

/* task 24, before the headphone poll FUN_400ACFA0 */
int pwc_hp_pass(void)
{
    if (!silenced)
        return 1;
    st.hp_skips++;
    return 0;
}

/* task 24, before the executor FUN_400ACD28: a GP3[8] event must not re-light the amp in standby
 * (the sample 0x4139E7FF stays current - wake step 5 reads it).  Also the safety net: an output
 * bit found on while silenced is switched off again and counted. */
void pwc_exec_pre(void)
{
    if (!silenced)
        return;
    if (stk_gp3_event) {
        stk_gp3_event = 0;
        st.exec_clears++;
    }
    if (stk_gpio_out67 & 0x8800u) {
        stk_amp_off();
        stk_mute();
        st.relight++;
    }
}

/* task 19, before FUN_4014A2C8: F0 40 <dev> 32 08 02 51 39 ... from the panel (stock port 3) =
 * PowerManagementTimerReset, the panel app's touch notice (standby.c standby_midi_wake). */
void pwc_kawai_peek(uint32_t port, const uint8_t *m, uint32_t n)
{
    if (power_frame_quiet(m, n)) {
        mq_quiet = 1;
        if (mq.quiet < 0xFFFFu)
            mq.quiet++;
    } else {
        mq_real = 1;
        mq_real_seq++;
        if (mq.real < 0xFFFFu)
            mq.real++;
    }
    if (port != 3 || n < 9 || m[0] != 0xF0 || m[1] != 0x40 || m[3] != 0x32 || m[6] != 0x51 || m[7] != 0x39)
        return;
    touches++;
    touch_req = 1;
}

/* ---- polls are not activity (quiet polls) ------------------- */
/* task 19, the payload's ng_sysex: every 0x7D frame the resident hands on (its own OTA ops 0x10-0x16 never get here) */
void pwc_frame_7d(const uint8_t *buf, uint32_t len)
{
    if (power_frame_quiet(buf, len)) {
        mq_quiet = 1;
        if (mq.quiet < 0xFFFFu)
            mq.quiet++;
    } else {
        mq_real = 1;
        mq_real_seq++;
        if (mq.real < 0xFFFFu)
            mq.real++;
    }
}

/* task 19, MIDI pump tail 0x401539AC: the four port pumps of this pass are done (FUN_4015395C: 0x4015399C-0x401539A8),
 * every byte they took went through FUN_40148B24, which reset the stock counter (FUN_400ACB84: counter >= 2 -> 1) for
 * each byte < 0xF8.  The pass is a poll-only pass when no activity frame ended in it and either a poll / read frame
 * ended or bytes arrived in a SysEx still open (a real frame resets again with its F7, when it is classified) - and no
 * key was down now or at the last tail (a key press / release in another task in the same ~2 ms is not undone).
 * Then the counter goes back to the value of the last accepted pass + the 20 ms ticks since (as if no byte had come);
 * otherwise this pass is accepted as it is (keys, pedals, channel messages, unclassified frames: activity).
 * Not when the power port holds the counter itself (standby, off; the stream hold lets it run), the setting does not count, or the
 * timer hook wrote the counter since the last accepted pass (a hold tick or an expiry: the base is stale).  The stock state byte is never written here.  Loads / stores only. */
void pwc_midi_tail(void)
{
    uint32_t seq = jump_seq;   /* first: a jump after this read is taken by the next tail */
    uint32_t cnt = stk_apo_counter, t = timer_ticks, wr = timer_writes, want;
    uint8_t q = mq_quiet, r = mq_real, s = stk_apo_setting, keys = 0, open = 0;
    mq_quiet = 0;
    mq_real = 0;
    if (stk_key_scan_on)
        for (int i = 0; i < 88; i++)
            if (KEY_DOWN(stk_key_state[i])) {
                keys = 1;
                break;
            }
    for (int p = 0; p < 4; p++) {
        uint8_t ps = stk_sx_state[p];
        uint16_t pl = stk_sx_len[p];
        if (ps == 0xF0 && (mq.sx_state[p] != 0xF0 || mq.sx_len[p] != pl))
            open = 1;
        mq.sx_state[p] = ps;
        mq.sx_len[p] = pl;
    }
    if (seq != mq.jumps) {   /* the timer hook moved the counter (19 04): that is the base now, not a stale one */
        uint32_t c = jump_counter, at = jump_at, w = jump_writes, jr = jump_real;
        if (seq == jump_seq) {
            if (jr == mq_real_seq)
                r = 0;   /* no activity frame ended since the 19 04 was accepted: this pass's activity is behind the jump */
            mq.jumps = seq;
            mq.counter = c;
            mq.ticks = at;
            mq.writes = w;
            mq.valid = 1;
        }
    }
    if (mq.valid && ctl_armed && !ctl_hold && !ctl_off && (q || open) && !r && !keys && !mq.keys &&
        wr == mq.writes && stk_special_mode != 1 && s >= 1 && s <= 3) {
        want = mq.counter + (t - mq.ticks);
        if (want > stk_apo_limits[s])
            want = stk_apo_limits[s];   /* never past the limit: the next timer tick expires / clamps it */
        if (cnt < want && timer_writes == wr) {
            stk_apo_counter = want;
            if (mq.undone < 0xFFFFu)
                mq.undone++;
        }
        return;   /* the base stays: the next accepted pass moves it */
    }
    mq.counter = cnt;
    mq.ticks = t;
    mq.writes = wr;
    mq.keys = keys;
    mq.valid = 1;
}
