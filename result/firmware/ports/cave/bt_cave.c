/* OpenGrand cave port - BLE-MIDI port 2 latch clear (F1), active-sensing guard (F5), BT audio module
 * read-only query (F2), UART1 send discipline (F6), BT audio source manager (F7).  See bt_cave.h. */
#include "bt_cave.h"

#include <stdbool.h>
#include <string.h>

#include "bt_stock.h"
#include "ca79ng/btsrc.h"
#include "hal_cave.h"
#include "power_cave.h"
#include "power_stock.h"
#include "sysex_cave.h"

/* the BT policy's NOR part (recon + the preferred device, user decision 2026-11-09): through the power port's kvstore
 * (startup_cave.h pwc_kv - ota_main, PWC_STARTUP); the host test build sets BTC_KV with its own stand-ins */
#ifndef BTC_KV
#define BTC_KV PWC_STARTUP
#endif
#if BTC_KV
#include "ca79ng/storage.h"
kvstore_t *pwc_kv(int mount);
int pwc_kv_known(void);
int pwc_flash_idle(void);
/* the startup settings (startup_cave.h): what the kept part starts from at this power-on */
int suc_ng(const uint8_t **bt, uint8_t *btn, const uint8_t **pw, uint8_t *pwn);
void suc_ng_applied(uint8_t bit);
void suc_ng_changed(void);
#define SUC_NG_WAIT_BT (-1)   /* startup_cave.h SUC_NG_WAIT */
#endif

#define REQ_MAX 32u
#define RING 0x800u            /* UART1 RX ring */
#define B_PER_MS 16u           /* > UART1 at 114.7 kbaud (11.5 B/ms): most an index moves per ms */
#define UART1_PENDING 0x87u    /* stk_bt_rx_flag: overflow / error (the assembler resets) or latched */
#define P2_LATCH 0x80u
#define P2_UNLATCH_MS 500u
#define P2_RELATCH_MS 5000u    /* latched again this soon after our clear: wait twice as long */
#define P2_STREAK_MAX 4u       /* 0.5 s << 4 = 8 s */
#define AS_OFF 0xFFu
#define HIER_IRQ 0x02u
#define Q_IDLE_MS 500u         /* a reply is over after this much silence ... */
#define Q_FIRST_MS 2000u       /* ... or when nothing but the command's echo came at all */
#define Q_MAX_MS 4000u         /* hard cap per command */
#define Q_GAP_MS 16u           /* after a command's first byte: the rest only this much later */
#define P2_RING 0x800u         /* port 2 RX ring: 2 KB, 11-bit indices */
#define P2_KEEP_MAX 32u        /* a count this much short of the ring's bytes: the bytes are real (kept) */
#define TX_BUF (BTS_OUT_MAX + 2u) /* one module command (the longest: bts_next's out) + CR LF */
#define TX_BURST 8u            /* bytes per pass at most (each only after the stock TX state is idle again) */
#define TX_GIVE_UP_MS 1000u    /* the stock TX state not idle for this long: the command is dropped */
#define MOD_OFF_MS 600u        /* module restart: off this long */

/* The only module commands this payload can send: read-only iWRAP queries. */
static const char CMD_SET[] = "SET\r\n";
static const char CMD_LIST[] = "LIST\r\n";
static const char CMD_INFO[] = "INFO\r\n";
static const char *const CMDS[3] = {CMD_SET, CMD_LIST, CMD_INFO};
static const uint8_t CMD_LEN[3] = {sizeof CMD_SET - 1, sizeof CMD_LIST - 1, sizeof CMD_INFO - 1};

/* ---- RW in R1 (core RW region, zeroed on the first tick) ------------------------------------ */
typedef struct {
    uint8_t cap[BT_CAP];
    bts_t src;
} bt_rw_t;
CAVE_RW static bt_rw_t rw;

/* ---- payload .data --------------------------------------------------------------------------- */
static volatile uint8_t req_pending = 0;   /* task 19 -> task 24 */
static uint8_t req_port = 0;
static uint8_t req_len = 0;
static uint8_t req_buf[REQ_MAX] = {0};

static struct {
    uint8_t inited, sw;
    /* F2 query */
    uint8_t q_state, q_flags, q_which, q_cmd, q_end, q_sent, q_got, q_reset_seen, q_part, q_retried;
    uint16_t q_len, q_rd, q_wr_last, q_cmd_at;
    uint32_t q_ms, q_idle_ms;
    /* F1 */
    uint8_t p2_was_latched, p2_streak;
    uint16_t p2_unlatches;
    uint32_t p2_latch_ms, p2_since_ms;
    /* F1b: port 2 RX ring check */
    uint8_t g_seen;
    uint16_t g_rd, g_wr, g_cnt, g_repairs;
    int16_t g_last;
    /* F5 */
    uint16_t as_disarms;
    uint32_t last_us, acc_us;
    /* F6: UART1 sender */
    uint8_t tx_len, tx_pos, tx_owner;
    uint8_t tx_buf[TX_BUF];
    uint32_t tx_gap_ms, tx_wait_ms;
    uint16_t tx_drops;
    /* F7: source manager */
    uint16_t s_rd, s_wr_last;
    uint8_t s_reset_seen, s_started, rec_phase;
    uint32_t rec_ms;
    /* the policy's NOR part */
    uint8_t kv_loaded, kv_dirty, kv_flags, kv_ng;   /* kv_ng: the startup settings' answer taken */
    uint8_t kv_last[BTS_SAVED_RAW];   /* what NOR holds (read back / written), to write only a change */
    /* request accounting (heartbeat) */
    uint16_t req_seen, req_busy;
    /* 1D 0B: the DSP write position at the previous scan */
    uint8_t lvl_seen;
    uint16_t lvl_n;
    uint32_t lvl_wp;
} st = {0};
/* live apply handover (btsrc.h): BTS_HO_RAW bytes at a fixed RAM address that the apply does
 * not touch - the top of the core RW window, above every payload's core_rw_end (ota_payload.ld bt_handover, asserted
 * there).  This payload writes it when it says it is idle (btc_handover, from ng_quiesce) and the next one reads and clears
 * it before its first tick; the host test build gives its own array. */
extern uint8_t bt_handover[];
static uint8_t ho_written = 0;   /* payload .data: 1 = this payload wrote the block (an apply asked it to be idle) */
static volatile uint32_t hb_passes = 0;   /* engine-loop passes through btc_tick (task 24 writes, task 19 reads) */
static volatile uint8_t hb_stage = 0;     /* BT_STAGE_*: set around every stock call that could block */
enum { TX_NONE = 0, TX_QUERY = 1, TX_SRC = 2 };

/* ---- 0x7D replies ------------------------------------------------------------------------------ */
static const uint8_t TAG[4] = {0x43, 0x41, 0x37, 0x39};

#define REPLY_MAX 140u   /* reply data after the op byte (1D 08: 10 + BT_LOG_CHUNK) */

static void reply(uint8_t stock_port, const uint8_t *d, uint32_t n)
{
    uint8_t m[8 + REPLY_MAX];
    if (n > sizeof m - 8)
        return;
    m[0] = 0xF0;
    m[1] = 0x7D;
    memcpy(m + 2, TAG, 4);
    m[6] = (uint8_t)(BT_OP | 0x40);
    for (uint32_t i = 0; i < n; i++)
        m[7 + i] = d[i] & 0x7F;
    m[7 + n] = 0xF7;
    hal_midi_tx(cave_port_from_stock(stock_port), m, (uint16_t)(8 + n));
}

static uint32_t pack7(const uint8_t *in, uint32_t n, uint8_t *out)
{
    uint32_t o = 0;
    for (uint32_t i = 0; i < n; i += 7) {
        uint32_t g = n - i < 7 ? n - i : 7;
        uint8_t hi = 0;
        for (uint32_t k = 0; k < g; k++)
            hi |= (uint8_t)(((in[i + k] >> 7) & 1u) << k);
        out[o++] = hi;
        for (uint32_t k = 0; k < g; k++)
            out[o++] = in[i + k] & 0x7F;
    }
    return o;
}

static void put16(uint8_t *p, uint32_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }

/* ---- F1: port 2 RX 0x80 latch ------------------------------------------------------------------ */
/* Latched = after a UART error the pump left flag 0x80 with the ring empty: the ISR drops every byte
 * and nothing in the stock clears it before the next boot.  The stock reset leaves
 * a window between its flag store and its count store; with the count at 0 (as in a latch) a byte taken
 * by the ISR in that window would stay counted as 0 and the pump would run one byte behind for good -
 * so the IRQ line is masked around the call, exactly as the stock pump masks it around its own index
 * update.  Nothing preempts task 24 while IRQs are masked. */
static bool p2_latched(void)
{
    return stk_p2_rx_flag == P2_LATCH && stk_p2_rx_count == 0 && stk_p2_rx_rd == stk_p2_rx_wr;
}

static void p2_unlatch(uint32_t elapsed)
{
    bool done;
    if (st.p2_since_ms < 0x80000000u)
        st.p2_since_ms += elapsed;
    if (!(st.sw & BT_SW_P2_UNLATCH) || !p2_latched()) {
        st.p2_latch_ms = 0;
        st.p2_was_latched = 0;
        return;
    }
    if (!st.p2_was_latched) {   /* a new latch: soon after our last clear = a line that keeps failing */
        st.p2_was_latched = 1;
        if (st.p2_unlatches && st.p2_since_ms < P2_RELATCH_MS) {
            if (st.p2_streak < P2_STREAK_MAX)
                st.p2_streak++;
        } else {
            st.p2_streak = 0;
        }
    }
    st.p2_latch_ms += elapsed;
    if (st.p2_latch_ms < (P2_UNLATCH_MS << st.p2_streak))
        return;
    stk_irq_hier = stk_irq_hier & ~HIER_IRQ;
    done = p2_latched();   /* again, with nothing able to run in between */
    if (done)
        stk_p2_rx_reset();
    stk_irq_hier = stk_irq_hier | HIER_IRQ;
    st.p2_latch_ms = 0;
    st.p2_was_latched = 0;
    if (done) {
        st.p2_since_ms = 0;
        if (st.p2_unlatches < 0xFFFFu)
            st.p2_unlatches++;
    }
}

/* ---- F1b: port 2 RX ring check ------------------------------------------------------------------ */
/* The stock port 2 RX reset FUN_40145DFC stores wr 0, flag 0, rd 0, AS 0xFF, count 0 one after the other
 * (0x40145E1C..0x40145E30).  Three of its callers run it with IRQs enabled: the pump on an error flag (task 19,
 * 0x40149074), the watchdog FUN_40145E88 (priority 30, 0x40145EDC - any task preempts it between the stores) and
 * FUN_40147A20(2) (0x40147A58), which every CONNECT from another port reaches through FUN_4015DE98 - the panel
 * taking the binding from the app resets the app's port.  A byte the ISR takes between the flag store and the
 * count store is in the ring but not counted: the pump (it reads only while count != 0) then runs that many
 * bytes behind for good - each frame is dispatched only when the next one arrives (the M4 field
 * failure, 1B answered with 5A).  A count too high makes the pump read past the write index and replay the
 * previous lap.  Neither heals before the next reset of the port, and the stock resets it only on an error.
 * Invariant: count == (wr - rd) mod 2 KB whenever the IRQ line is masked and no task is inside a reset - the
 * ISR's wr / count stores and the pump's rd / count stores (0x40148FE4..0x40149030, IRQ masked) are atomic for a
 * task-24 reader that masks the line.  A broken invariant is repaired when it was seen unchanged on two passes
 * in a row (a priority-30 reset preempted this pass has finished by then) and the flag is 0 (error / latch: the
 * stock reset and F1 own that), with the line masked: a few uncounted bytes (<= P2_KEEP_MAX, read index not past
 * the write index) are counted - they are the frame that arrived inside the reset; anything else (read index
 * ahead, a count too high) moves the read index to the write index minus the count - the unexplained span is
 * dropped. */
static void p2_check(void)
{
    uint16_t rd, wr, cnt;
    uint32_t span;
    if (!(st.sw & BT_SW_P2_UNLATCH)) {
        st.g_seen = 0;
        return;
    }
    stk_irq_hier = stk_irq_hier & ~HIER_IRQ;
    rd = (uint16_t)(stk_p2_rx_rd & (P2_RING - 1));
    wr = (uint16_t)(stk_p2_rx_wr & (P2_RING - 1));
    cnt = stk_p2_rx_count;
    span = (uint32_t)((wr - rd) & (P2_RING - 1));
    if (stk_p2_rx_flag != 0 || cnt == span) {
        st.g_seen = 0;
    } else if (!st.g_seen || st.g_rd != rd || st.g_wr != wr || st.g_cnt != cnt) {
        st.g_seen = 1;   /* first sight: look again next pass */
        st.g_rd = rd;
        st.g_wr = wr;
        st.g_cnt = cnt;
    } else {
        if (span > cnt && span - cnt <= P2_KEEP_MAX && rd <= wr)
            stk_p2_rx_count = (uint16_t)span;
        else
            stk_p2_rx_rd = (uint16_t)((wr - cnt) & (P2_RING - 1));
        st.g_last = (int16_t)((int32_t)span - (int32_t)cnt);
        st.g_seen = 0;
        if (st.g_repairs < 0xFFFFu)
            st.g_repairs++;
    }
    stk_irq_hier = stk_irq_hier | HIER_IRQ;
}

/* ---- F5: port 2 active sensing --------------------------------------------------------------- */
/* The ISR arms the counter (0x11) on 0xFE; the 20 ms handler counts it down and at 0 the watchdog
 * releases the port's notes and binding.  Every pass puts an armed counter back to
 * 0xFF, the value the stock reset gives it: on port 2 the BLE link-down edge (GP4[6]) already runs
 * the same release, and a 0.34 s window is shorter than a BLE stall.  0 is left alone (the expiry has
 * already been flagged; the watchdog resets it to 0xFF itself). */
static void as_guard(void)
{
    uint8_t v;
    if (!(st.sw & BT_SW_AS_GUARD))
        return;
    v = stk_p2_as;
    if (v != AS_OFF && v != 0) {
        stk_p2_as = AS_OFF;
        if (st.as_disarms < 0xFFFFu)
            st.as_disarms++;
    }
}


/* ---- F6: UART1 send discipline ---------------------------------------------------------------- */
/* Evidence (REA ev_7835e9a3 FUN_4014512C, ev_0667da3a FUN_401450A0, ev_8a4f1bbc 0x401444E0,
 * ev_932bc7ac 0x40144550): FUN_4014512C puts a byte (IRQ masked: wr++, count++), unmasks, then calls the kick
 * FUN_401450A0, which - if UART1 IER bit 1 is off - sends ring[rd] WITHOUT checking that the ring holds a byte
 * (rd++, count--, IER bit 1 on).  The THRE handler 0x401444E0 (from 0x40144550 while LSR bit 5 and IER bit 1)
 * sends while rd != wr and clears IER bit 1 at rd == wr.  A task switch between the put and the kick while the
 * transmitter is busy (IER bit 1 on) lets the handler send the byte, empty the ring and clear IER bit 1 - the
 * kick then sends a stale byte and count wraps to 0xFFFF; the handler sends the stale ring (2047 bytes) and count
 * stops at 0xF800 > 0x7FE: the rest of that FUN_4014512C call busy-waits forever (0x40145160-0x40145174) and
 * task 24 never comes back.  With IER bit 1 off at the put there is no THRE interrupt, so the kick always sends
 * the byte just put: one byte per call, and only while the state is idle.  Nothing but the kick (stock writers,
 * which run only in the factory / test menu) switches IER bit 1 on, so the idle test cannot go stale. */
static bool tx_idle(void)
{
    return stk_bt_tx_cnt == 0 && (stk_bt_tx_rd & (RING - 1)) == (stk_bt_tx_wr & (RING - 1)) && !(stk_uart1_ier & 0x02);
}

static bool tx_busy(void) { return st.tx_len != 0; }

static bool tx_begin(uint8_t owner, const char *s, uint32_t n)
{
    if (st.tx_len || n == 0 || n + 2 > TX_BUF)
        return false;
    memcpy(st.tx_buf, s, n);
    st.tx_buf[n] = '\r';
    st.tx_buf[n + 1] = '\n';
    st.tx_len = (uint8_t)(n + 2);
    st.tx_pos = 0;
    st.tx_owner = owner;
    st.tx_gap_ms = 0;
    st.tx_wait_ms = 0;
    return true;
}

/* the payload's only call of FUN_4014512C: one byte */
__attribute__((noinline)) static void tx_put1(const uint8_t *b)
{
    hb_stage = BT_STAGE_TX;
    stk_bt_tx(b, 1);
    hb_stage = BT_STAGE_IDLE;
}

/* M6 field: the module answered `INFO` as `IFO SYNTAX ERROR` (its second byte lost while the iWRAP UART woke on
 * the first [inference]) - the first byte of every command, the rest Q_GAP_MS
 * later. */
static void tx_pump(uint32_t elapsed)
{
    if (!st.tx_len)
        return;
    if (stk_bt_power == 0) {
        st.tx_len = 0;   /* module off: nothing to send to */
        return;
    }
    if (st.tx_pos == 1 && st.tx_gap_ms < Q_GAP_MS) {
        st.tx_gap_ms += elapsed;
        if (st.tx_gap_ms < Q_GAP_MS)
            return;
    }
    for (uint32_t k = 0; k < TX_BURST && st.tx_pos < st.tx_len; k++) {
        if (!tx_idle()) {
            if (k == 0) {
                st.tx_wait_ms += elapsed;
                if (st.tx_wait_ms >= TX_GIVE_UP_MS) {
                    st.tx_len = 0;   /* never wait on the stock writer: drop the command */
                    if (st.tx_drops < 0xFFFFu)
                        st.tx_drops++;
                }
            }
            return;
        }
        st.tx_wait_ms = 0;
        tx_put1(st.tx_buf + st.tx_pos);
        st.tx_pos++;
        if (st.tx_pos == 1) {
            st.tx_gap_ms = 0;
            return;
        }
    }
    if (st.tx_pos >= st.tx_len)
        st.tx_len = 0;
}

/* ---- UART1 RX ring reader (read only, own index) ----------------------------------------------- */
/* A stock ring reset (overflow / error, FUN_40145C00) is followed as in power_cave.c bt_feed: nothing is read
 * while the flag shows it, and reading restarts at 0. */
typedef struct {
    uint16_t *rd, *wr_last;
    uint8_t *reset_seen;
} rdr_t;

static uint32_t rdr_left(rdr_t r, uint32_t elapsed, uint8_t *flags)
{
    uint16_t wr = (uint16_t)(stk_bt_rx_wr & (RING - 1));
    uint32_t left;
    if (stk_bt_rx_flag & UART1_PENDING) {
        *flags |= BT_QF_RESET;
        *r.reset_seen = 1;
        *r.rd = wr;
        left = 0;
    } else {
        uint32_t adv = (uint32_t)((wr - *r.wr_last) & (RING - 1));
        if (*r.reset_seen) {
            *r.rd = 0;
        } else if (wr < *r.wr_last && adv > 32u + elapsed * B_PER_MS) {
            *r.rd = 0;
            *flags |= BT_QF_RESET;
        }
        *r.reset_seen = 0;
        if (32u + elapsed * B_PER_MS >= RING)
            *flags |= BT_QF_LAP;   /* the pass came late enough for the ring to have wrapped past us */
        left = (uint32_t)((wr - *r.rd) & (RING - 1));
    }
    *r.wr_last = wr;
    return left;
}

/* ---- F2: BT audio module query --------------------------------------------------------------- */
static void cap_put(uint8_t ch)
{
    if (st.q_len < BT_CAP)
        rw.cap[st.q_len++] = ch;
    else
        st.q_flags |= BT_QF_TRUNC;
}

/* New bytes of the stock UART1 RX ring since the command went out (read only, own index - the stock
 * assembler keeps reading its own). */
static uint32_t q_read(uint32_t elapsed)
{
    rdr_t r = {&st.q_rd, &st.q_wr_last, &st.q_reset_seen};
    uint32_t left = rdr_left(r, elapsed, &st.q_flags), got = 0;
    while (left--) {
        uint8_t ch = stk_bt_rx_ring[st.q_rd];
        st.q_rd = (uint16_t)((st.q_rd + 1) & (RING - 1));
        if (ch == 0)
            continue;
        got++;
        cap_put(ch >= 0x80 ? (uint8_t)'?' : ch);
    }
    return got;
}

static uint8_t q_refuse(void)
{
    if (stk_special_mode == 1)
        return BT_TEST_MODE;   /* the stock line parser hands every line to the test menu then */
    if (stk_bt_power == 0)
        return BT_MODULE_OFF;
    if (stk_bt_rx_flag & UART1_PENDING)
        return BT_RX_LATCHED;  /* the reply would be dropped by the ISR */
    return BT_OK;
}

static void q_send(void)
{
    const char *c = CMDS[st.q_cmd];
    if (q_refuse() != BT_OK) {
        st.q_flags |= BT_QF_ABORT;
        st.q_state = BT_Q_DONE;
        st.q_part = 0;
        return;
    }
    if (st.q_part) {   /* handed to the sender: collecting starts when it is all out */
        if (tx_busy())
            return;
        st.q_part = 0;
        st.q_sent++;
        st.q_ms = 0;
        st.q_idle_ms = 0;
        st.q_got = 0;
        st.q_state = BT_Q_COLLECT;
        return;
    }
    if (tx_busy())
        return;        /* a source-manager command is still going out */
    cap_put('>');
    cap_put(' ');
    for (uint32_t i = 0; i < CMD_LEN[st.q_cmd]; i++)
        cap_put((uint8_t)c[i]);
    st.q_cmd_at = st.q_len;
    st.q_rd = (uint16_t)(stk_bt_rx_wr & (RING - 1));
    st.q_wr_last = st.q_rd;
    st.q_reset_seen = 0;
    st.q_ms = 0;
    tx_begin(TX_QUERY, c, CMD_LEN[st.q_cmd] - 2u);
    bts_log(&rw.src, BTS_L_TX, c, CMD_LEN[st.q_cmd] - 2u);
    st.q_part = 1;
}

/* where the module's answer starts: after the command's own echo (SET CONTROL ECHO 7 - field M6 text
 * `> SET\r\nSET\r\n...`) when the captured text begins with it */
static uint32_t q_body_at(void)
{
    uint32_t n = CMD_LEN[st.q_cmd] - 2u, at = st.q_cmd_at;
    if (st.q_len >= at + n && memcmp(rw.cap + at, CMDS[st.q_cmd], n) == 0 &&
        (st.q_len == at + n || rw.cap[at + n] == '\r' || rw.cap[at + n] == '\n')) {
        at += n;
        while (at < st.q_len && (rw.cap[at] == '\r' || rw.cap[at] == '\n'))
            at++;
    }
    return at;
}

/* the text captured since the command went out holds `SYNTAX ERROR` */
static bool q_syntax_error(void)
{
    static const char E[] = "SYNTAX ERROR";
    for (uint32_t i = st.q_cmd_at; i + (sizeof E - 1) <= st.q_len; i++)
        if (memcmp(rw.cap + i, E, sizeof E - 1) == 0)
            return true;
    return false;
}

static void q_tick(uint32_t elapsed)
{
    if (st.q_state == BT_Q_SEND) {
        q_send();
    } else if (st.q_state == BT_Q_COLLECT) {
        uint32_t got = q_read(elapsed);
        bool answered = st.q_len > q_body_at();
        st.q_ms += elapsed;
        if (got) {
            st.q_got = 1;
            st.q_idle_ms = 0;
        } else {
            st.q_idle_ms += elapsed;
        }
        if ((answered && st.q_idle_ms >= Q_IDLE_MS) || (!answered && st.q_ms >= Q_FIRST_MS) || st.q_ms >= Q_MAX_MS) {
            if (!st.q_got)
                st.q_flags |= BT_QF_SILENT;
            else if (!answered)
                st.q_flags |= BT_QF_ECHO_ONLY;
            if (answered && !st.q_retried && q_syntax_error()) {
                st.q_retried = 1;
                st.q_flags |= BT_QF_RETRY;
                st.q_state = BT_Q_SEND;   /* the same command once more, next pass */
            } else if (st.q_cmd < st.q_end) {
                st.q_retried = 0;
                st.q_cmd++;
                st.q_state = BT_Q_SEND;   /* next pass */
            } else {
                st.q_state = BT_Q_DONE;
            }
        }
    }
}

static bool q_active(void) { return st.q_state == BT_Q_SEND || st.q_state == BT_Q_COLLECT; }

static uint8_t q_start(uint8_t which)
{
    uint8_t rc;
    if (which > 3)
        return BT_BAD_VALUE;
    if (q_active())
        return BT_BUSY;
    rc = q_refuse();
    if (rc != BT_OK)
        return rc;
    st.q_which = which;
    st.q_cmd = which ? (uint8_t)(which - 1) : 0;
    st.q_end = which ? (uint8_t)(which - 1) : 2;
    st.q_flags = 0;
    st.q_len = 0;
    st.q_sent = 0;
    st.q_part = 0;
    st.q_retried = 0;
    st.q_state = BT_Q_SEND;   /* sent by the next pass: the reply to this op goes out first */
    return BT_OK;
}

/* ---- F7: BT audio source manager --------------------------------------------------------------- */
static bool src_can_act(void)
{
    return stk_bt_power != 0 && stk_special_mode != 1 && !(stk_bt_rx_flag & UART1_PENDING) && pwc_on() &&
           st.rec_phase == 0;
}

static void src_feed(uint32_t elapsed)
{
    uint8_t chunk[32], dummy = 0;
    uint32_t left, n = 0;
    rdr_t r = {&st.s_rd, &st.s_wr_last, &st.s_reset_seen};
    if (!st.s_started) {   /* start at the write index: only lines from now on; LIST learns what is there */
        st.s_started = 1;
        st.s_rd = st.s_wr_last = (uint16_t)(stk_bt_rx_wr & (RING - 1));
        st.s_reset_seen = 0;
        rw.src.list_want = 1;
    }
    left = rdr_left(r, elapsed, &dummy);
    while (left--) {
        uint8_t ch = stk_bt_rx_ring[st.s_rd];
        st.s_rd = (uint16_t)((st.s_rd + 1) & (RING - 1));
        if (ch == 0)
            continue;
        chunk[n++] = ch;
        if (n == sizeof chunk) {
            bts_rx(&rw.src, chunk, n);
            n = 0;
        }
    }
    if (n)
        bts_rx(&rw.src, chunk, n);
}

/* restart a wedged module: the stock off / on routines the standby uses (FUN_400AD030 / FUN_400AD098 +
 * FUN_40146810, REA ev_9513099b), only while ON and the user setting Bluetooth Audio is on */
static void src_restart_begin(void)
{
    if (st.rec_phase || stk_bt_power == 0 || stk_bt_audio_cfg == 0 || !pwc_on())
        return;
    st.tx_len = 0;
    bts_log(&rw.src, BTS_L_EVENT, "module restart", 14);
    hb_stage = BT_STAGE_MODULE_OFF;
    stk_bt_audio_off();
    hb_stage = BT_STAGE_IDLE;
    st.rec_phase = 1;
    st.rec_ms = 0;
}

static void src_tick(uint32_t elapsed)
{
    char cmd[BTS_OUT_MAX];
    bool on;
    if (st.rec_phase) {
        st.rec_ms += elapsed;
        if (st.rec_ms >= MOD_OFF_MS) {
            st.rec_phase = 0;
            if (stk_bt_audio_cfg != 0 && pwc_on()) {
                hb_stage = BT_STAGE_MODULE_ON;
                stk_bt_audio_on();
                stk_bt_flags_clear();
                hb_stage = BT_STAGE_IDLE;
            }
        }
    }
    on = stk_bt_power != 0;
    bts_module(&rw.src, on);
    if (on)
        src_feed(elapsed);
    else
        st.s_started = 0;
    bts_tick(&rw.src, elapsed, src_can_act() && !q_active());
    if (bts_take_recover(&rw.src))
        src_restart_begin();
    if (!tx_busy() && !q_active() && src_can_act()) {
        uint32_t n = bts_next(&rw.src, cmd, sizeof cmd);
        if (n)
            tx_begin(TX_SRC, cmd, n);
    }
}

uint16_t btc_a2dp_links(void) { return st.inited ? bts_a2dp_links(&rw.src) : 0; }
int btc_stream_playing(int link) { return st.inited ? bts_stream_playing(&rw.src, link) : -1; }

/* ---- ops ------------------------------------------------------------------------------------- */
void btc_status(uint8_t raw[BT_STATUS_RAW])
{
    uint32_t p = hb_passes;
    memset(raw, 0, BT_STATUS_RAW);
    raw[0] = BT_STATUS_LAYOUT;
    raw[1] = st.sw;
    raw[2] = st.q_state;
    raw[3] = st.q_flags;
    put16(raw + 4, st.q_len);
    raw[6] = st.q_sent;
    raw[7] = st.q_which;
    raw[8] = stk_p2_rx_flag;
    raw[9] = stk_p2_as;
    put16(raw + 10, stk_p2_rx_count);
    put16(raw + 12, st.p2_unlatches);
    put16(raw + 14, st.as_disarms);
    raw[16] = st.p2_streak;
    raw[17] = stk_bt_rx_flag;
    raw[18] = (uint8_t)((stk_bt_power != 0 ? 0x01 : 0) | (stk_special_mode == 1 ? 0x02 : 0) |
                        (p2_latched() ? 0x04 : 0));
    raw[19] = (uint8_t)(st.p2_latch_ms / 100u > 255u ? 255u : st.p2_latch_ms / 100u);
    put16(raw + 20, stk_p2_rx_rd);
    put16(raw + 22, stk_p2_rx_wr);
    put16(raw + 24, st.g_repairs);
    put16(raw + 26, (uint16_t)st.g_last);
    put16(raw + 28, (uint16_t)p);
    put16(raw + 30, (uint16_t)(p >> 16));
    put16(raw + 32, st.tx_drops);
    put16(raw + 34, st.req_seen);
    put16(raw + 36, st.req_busy);
    raw[38] = hb_stage;
    raw[39] = stk_uart1_ier;
}

void btc_heartbeat(uint8_t raw[BT_HB_RAW])
{
    uint32_t p = hb_passes;
    memset(raw, 0, BT_HB_RAW);
    raw[0] = 1;   /* layout */
    raw[1] = hb_stage;
    put16(raw + 2, (uint16_t)p);
    put16(raw + 4, (uint16_t)(p >> 16));
    put16(raw + 6, stk_bt_tx_cnt);
    put16(raw + 8, stk_bt_tx_rd);
    put16(raw + 10, stk_bt_tx_wr);
    raw[12] = stk_uart1_ier;
    raw[13] = (uint8_t)((req_pending ? 0x01 : 0) | (st.tx_len ? 0x02 : 0) | (q_active() ? 0x04 : 0) |
                        (stk_bt_power != 0 ? 0x08 : 0));
    raw[14] = st.q_state;
    raw[15] = (uint8_t)(st.req_busy > 255 ? 255 : st.req_busy);
}

/* 1D 0B: the DSP's record-bus capture, read only (bt_stock.h).  32-bit reads of uncached DSP L2 (~1152 of them);
 * the DSP keeps writing meanwhile - a torn sample only moves a peak by one sample's worth. */
void btc_level(uint8_t raw[BT_LVL_RAW])
{
    uint32_t wp = stk_dsp_rec_wp, sum = 0, over = 0, i;
    uint16_t n = stk_dsp_rec_n, peak[2] = {0, 0};
    uint32_t off = wp - (uint32_t)(uintptr_t)stk_dsp_rec_buf;
    uint8_t f = 0;
    for (i = 0; i < STK_DSP_REC_WORDS; i++) {
        uint32_t w = stk_dsp_rec_buf[i];
        for (int k = 0; k < 2; k++) {
            int32_t x = (int16_t)(uint16_t)(k ? w >> 16 : w);
            uint32_t a = (uint32_t)(x < 0 ? -x : x);
            if (a > 0x7FFFu)
                a = 0x7FFFu;
            if (a > peak[k])
                peak[k] = (uint16_t)a;
            sum += a;
            if (a >= BT_LVL_FLOOR)
                over++;
        }
    }
    if (!st.lvl_seen)
        f |= BT_LVL_F_FIRST;
    else if (wp != st.lvl_wp || n != st.lvl_n)
        f |= BT_LVL_F_MOVED;
    if (stk_dsp_rec_state != 0)
        f |= BT_LVL_F_REC;
    if (off >= STK_DSP_REC_WORDS * 4u)
        f |= BT_LVL_F_WP_ODD;
    st.lvl_seen = 1;
    st.lvl_wp = wp;
    st.lvl_n = n;
    memset(raw, 0, BT_LVL_RAW);
    raw[0] = BT_LVL_LAYOUT;
    raw[1] = f;
    put16(raw + 2, peak[0]);
    put16(raw + 4, peak[1]);
    put16(raw + 6, (uint16_t)(sum / BT_LVL_SAMPLES));
    put16(raw + 8, (uint16_t)over);
    put16(raw + 10, stk_dsp_rec_gain);
    put16(raw + 12, (uint16_t)(off > 0xFFFFu ? 0xFFFFu : off));
    put16(raw + 14, n);
}

void btc_sources(uint8_t raw[BT_SRC_RAW])
{
    const bts_t *b = &rw.src;
    memset(raw, 0, BT_SRC_RAW);
    raw[0] = BT_SRC_LAYOUT;
    raw[1] = b->pol.mode;
    raw[2] = b->pol.flags;
    raw[3] = b->pol.hold_ds;
    raw[4] = b->pol.sweep_s;
    raw[5] = (uint8_t)((stk_bt_power != 0 ? 0x01 : 0) | (st.inited && src_can_act() ? 0x02 : 0) |
                       (st.rec_phase ? 0x04 : 0) | (b->listing ? 0x08 : 0) |
                       0x10);   /* bits 4-6: the switching strategy + 1 (0 = older payload) - always 0 */
    raw[116] = (uint8_t)(((b->pol.recon + 1u) & 3u) | (uint32_t)st.kv_flags << 2 |   /* 2026-11-09 */
                         0x40u | (b->pol.xopt & BTS_X_LATE_STOP ? 0x80u : 0));      /* the options known / late stop on */
    raw[117] = (uint8_t)(b->n.garbled > 255u ? 255u : b->n.garbled);
    raw[118] = (uint8_t)(b->n.recovered > 255u ? 255u : b->n.recovered);
    raw[119] = b->pol.xwait;   /* the late stop's wait, 100 ms units (BTS_XWAIT_DEFAULT at power-on; 0 = that default) */
    raw[6] = (uint8_t)bts_streams(b);
    raw[7] = b->active < 0 ? 0xFF : (uint8_t)b->active;
    put16(raw + 8, b->n.closes);
    put16(raw + 10, b->n.unanswered);
    put16(raw + 12, b->n.recoveries);
    put16(raw + 14, b->n.dual);
    put16(raw + 16, b->n.lines);
    raw[18] = b->pol.has_pref;
    memcpy(raw + 19, b->pol.pref, 6);
    put16(raw + 25, b->n.calls);
    raw[27] = b->q_n;
    for (int i = 0; i < BTS_DEV_MAX; i++) {
        uint8_t *o = raw + 28 + 14 * i;
        const bts_dev_t *v = &b->dev[i];
        if (!v->used)
            continue;
        o[0] = (uint8_t)(bts_dev_state(b, i) | 0x80);   /* bit 7: slot used */
        memcpy(o + 1, v->addr, 6);
        put16(o + 7, v->links);
        put16(o + 9, v->a2dp);
        o[11] = (uint8_t)(v->stream_ms / 1000u > 255u ? 255u : v->stream_ms / 1000u);
        o[12] = bts_dev_state2(b, i);
        o[13] = v->refuse_n;
    }
    {   /* layout 2 tail: selection, the switch, AUTOCALL */
        uint8_t *t = raw + 28 + 14 * BTS_DEV_MAX;
        int s = bts_sel_dev(b);
        uint32_t ms = b->sw_phase ? b->sw_total : b->sw_last_ms;
        t[0] = s < 0 ? 0xFF : (uint8_t)s;
        t[1] = b->sw_phase;
        t[2] = b->sw_result;
        t[3] = (uint8_t)(ms / 100u > 255u ? 255u : ms / 100u);
        t[4] = b->ac_state;
        t[5] = b->ac_op;
        t[6] = b->ac_rc;
        put16(t + 7, b->n.refusals);
        put16(t + 9, b->n.suspends);
        t[11] = b->has_sel;
        memcpy(t + 12, b->sel_addr, 6);
        /* 102..: module health (BTS_MH_*), reboots, s since the last one, the switch's tries (BTS_SWT_*), the last
         * codec event (BTS_CODEC_*, its device slot or 0xFF, rate 0 44.1k / 1 48k / 2 other), AVRCP opens, reopens */
        t[18] = bts_health(b);
        put16(t + 19, b->n.reboots);
        t[21] = !b->reboot_seen ? 0xFF : (uint8_t)(b->reboot_ago / 1000u > 254u ? 254u : b->reboot_ago / 1000u);
        t[22] = b->sw_tried;
        t[23] = b->codec;
        t[24] = b->codec_dev;
        t[25] = b->codec_rate;
        t[26] = (uint8_t)(b->n.avrcp_calls > 255u ? 255u : b->n.avrcp_calls);
        /* t[27] reopens: 0 (raw is zeroed) - none since a12c870a, the counter since removed */
        /* 112..115 (2026-11-02 night): each device's play status (BTS_PS_* | BTS_PSB_*), 0 = unknown / empty slot */
        for (int i = 0; i < BTS_DEV_MAX; i++)
            t[28 + i] = bts_dev_ps(b, i);
    }
}

/* a 24-bit class of device as four 7-bit bytes, high first (op 1D 0A) */
static void put_cod(uint8_t *o, uint32_t v)
{
    o[0] = (uint8_t)((v >> 21) & 0x7F);
    o[1] = (uint8_t)((v >> 14) & 0x7F);
    o[2] = (uint8_t)((v >> 7) & 0x7F);
    o[3] = (uint8_t)(v & 0x7F);
}

static void unpack_addr(const uint8_t *in, uint8_t out[6])
{
    uint8_t hi = in[0];
    for (int k = 0; k < 6; k++)
        out[k] = (uint8_t)((in[1 + k] & 0x7F) | (((hi >> k) & 1u) << 7));
}

int btc_ctrl_bound(int core, uint8_t binding, uint8_t port)
{
    if (core >= 0)
        return core != 0;
    return binding == (uint8_t)(port + 1);
}

/* the session core's table when it runs (sysex_cave.c), else the stock binding */
static bool bound(uint8_t port) { return btc_ctrl_bound(sxc_ctrl_bound(port), stk_binding, port) != 0; }

/* what the module-setting ops (1D 09 / 0A / 0C / 0D) check before op <o> runs: BT_OK = go on (op 0 = state only) */
static uint8_t set_guard(uint8_t port, uint8_t o)
{
    if (o >= 2 && !bound(port))   /* BTS_AC_SET_OFF / BTS_CO_WRITE / BTS_AV_WRITE / BTS_AU_WRITE and the restores */
        return BT_NOT_BOUND;
    if (o && stk_bt_power == 0)
        return BT_MODULE_OFF;
    if (o && !pwc_on())
        return BT_NOT_ON;
    if (o && (q_active() || st.rec_phase))
        return BT_BUSY;
    return BT_OK;
}

static uint8_t set_rc(int rc)
{
    return rc == BTS_OK ? BT_OK : rc == BTS_STATE ? BT_BAD_STATE : rc == BTS_BAD ? BT_BAD_VALUE : BT_BUSY;
}

static void policy_out(uint8_t *d)
{
    const bts_policy_t *p = &rw.src.pol;
    d[0] = p->mode;
    d[1] = p->flags;
    d[2] = p->hold_ds & 0x7F;
    d[3] = p->sweep_s & 0x7F;
    d[4] = p->has_pref;
    pack7(p->pref, 6, d + 5);
    d[12] = 0;          /* after the pack7 address (7 B): the strategy - always 0 */
    d[13] = p->recon;   /* 2026-11-09 */
    d[14] = st.kv_flags;
    d[15] = p->xopt;
    d[16] = p->xwait;
}

/* ---- the policy's NOR part (user decision 2026-11-09) ------------------------------------ */
static void kv_note_change(void)
{
#if BTC_KV
    uint8_t raw[BTS_SAVED_RAW];
    bts_saved_encode(&rw.src.pol, raw);
    if (memcmp(raw, st.kv_last, sizeof raw) != 0) {
        st.kv_dirty = 1;
        st.kv_flags |= BT_KV_PENDING;
    }
#endif
}

/* the startup settings' answer for the kept part, once, after the NOR load: the factory
 * defaults (mode stock), the snapshot's part (current / power-off), or the live value just loaded (a live apply, or a
 * mode / snapshot of a build before).  A change is written back as the new live value by the normal path. */
static int kv_take_ng(void)
{
#if BTC_KV
    const uint8_t *bt = 0, *pw = 0;
    uint8_t btn = 0, pwn = 0, before[BTS_SAVED_RAW], raw[BTS_SAVED_RAW];
    int k = suc_ng(&bt, &btn, &pw, &pwn);
    if (k == SUC_NG_WAIT_BT)
        return 0;
    bts_saved_encode(&rw.src.pol, before);
    if (k == 1) {   /* KSU_NG_DEFAULTS */
        bts_policy_t d;
        bts_policy_default(&d);
        bts_saved_encode(&d, raw);
        (void)bts_saved_apply(&rw.src.pol, raw, sizeof raw);
    } else if (k == 2 && btn) {   /* KSU_NG_SAVED */
        (void)bts_saved_apply(&rw.src.pol, bt, btn);   /* a bad part keeps the live value */
    }
    bts_saved_encode(&rw.src.pol, raw);
    if (memcmp(raw, before, sizeof raw) != 0)
        suc_ng_applied(0x01);   /* SUC_NG_BT */
#endif
    return 1;
}

/* load once the power port's store probe answered; write a change once no stock writer owns the chip (task 24, like
 * the power policy op and the startup snapshot) */
static void kv_tick(void)
{
#if BTC_KV
    kvstore_t *kv;
    uint8_t raw[BTS_SAVED_RAW + 6];
    st.kv_flags |= BT_KV_KEPT;
    if (st.kv_loaded && !st.kv_ng) {
        if (!kv_take_ng())
            return;
        st.kv_ng = 1;
        kv_note_change();   /* the start value differs from NOR: it becomes the live value */
        return;
    }
    if (!st.kv_loaded) {
        if (!pwc_kv_known())
            return;
        st.kv_loaded = 1;
        bts_saved_encode(&rw.src.pol, st.kv_last);   /* the defaults: nothing on NOR to differ from */
        kv = pwc_kv(0);
        if (kv) {
            int n = kv_get(kv, KV_KEY_BT_POLICY, raw, sizeof raw);
            if (n > 0 && bts_saved_apply(&rw.src.pol, raw, (uint32_t)n)) {
                bts_saved_encode(&rw.src.pol, st.kv_last);
                st.kv_flags |= BT_KV_LOADED;
            }
        }
        kv_note_change();   /* an op before the load: still to be written */
        if (!st.kv_dirty)
            st.kv_flags &= (uint8_t)~BT_KV_PENDING;
        return;
    }
    if (!st.kv_ng)
        return;
    if (!st.kv_dirty || !pwc_flash_idle())
        return;
    st.kv_dirty = 0;
    st.kv_flags &= (uint8_t)~BT_KV_PENDING;
    bts_saved_encode(&rw.src.pol, raw);
    kv = pwc_kv(1);
    if (kv && kv_put(kv, KV_KEY_BT_POLICY, raw, BTS_SAVED_RAW)) {
        memcpy(st.kv_last, raw, BTS_SAVED_RAW);
        st.kv_flags &= (uint8_t)~BT_KV_FAILED;
    } else {
        st.kv_flags |= BT_KV_FAILED;   /* the next change tries again */
    }
#endif
}

static void serve_request(void)
{
    uint8_t port = req_port, sub;
    const uint8_t *a = req_buf + 7;
    uint32_t n = (uint32_t)req_len - 8;
    uint8_t d[REPLY_MAX];
    sub = n ? a[0] : 0;
    d[0] = sub;
    if (sub == 0x00) {
        uint8_t raw[BT_STATUS_RAW];
        btc_status(raw);
        d[1] = BT_OK;
        reply(port, d, 2 + pack7(raw, sizeof raw, d + 2));
    } else if (sub == 0x01) {
        d[1] = n >= 2 ? q_start(a[1]) : q_start(0);
        d[2] = n >= 2 ? a[1] : 0;
        reply(port, d, 3);
    } else if (sub == 0x02) {
        uint32_t off = n >= 3 ? ((uint32_t)a[1] << 7) | a[2] : 0, k = 0;
        d[1] = off > st.q_len ? BT_BAD_VALUE : BT_OK;
        d[2] = st.q_state;
        d[3] = (uint8_t)((st.q_len >> 7) & 0x7F);
        d[4] = (uint8_t)(st.q_len & 0x7F);
        d[5] = (uint8_t)((off >> 7) & 0x7F);
        d[6] = (uint8_t)(off & 0x7F);
        if (d[1] == BT_OK)
            for (; k < BT_CHUNK && off + k < st.q_len; k++)
                d[7 + k] = rw.cap[off + k];
        reply(port, d, 7 + k);
    } else if (sub == 0x03) {
        if (n < 2 || a[1] > BT_SW_ALL) {
            d[1] = BT_BAD_VALUE;
        } else if (!bound(port)) {
            d[1] = BT_NOT_BOUND;
        } else {
            st.sw = a[1];
            d[1] = BT_OK;
        }
        d[2] = st.sw;
        reply(port, d, 3);
    } else if (sub == 0x04) {
        uint8_t raw[BT_SRC_RAW];
        btc_sources(raw);
        d[1] = BT_OK;
        reply(port, d, 2 + pack7(raw, sizeof raw, d + 2));
    } else if (sub == 0x05) {
        uint8_t addr[6];
        int rc;
        d[2] = n >= 2 ? a[1] : 0;
        if (n < 9 || a[1] > BTS_ACT_MAX) {
            d[1] = BT_BAD_VALUE;
        } else if (!bound(port)) {
            d[1] = BT_NOT_BOUND;
        } else if (stk_bt_power == 0) {
            d[1] = BT_MODULE_OFF;
        } else if (!pwc_on()) {
            d[1] = BT_NOT_ON;
        } else {
            unpack_addr(a + 2, addr);
            rc = bts_action(&rw.src, a[1], addr);
            d[1] = rc == BTS_OK ? BT_OK : rc == BTS_UNKNOWN ? BT_NO_DEVICE : rc == BTS_NO_AVRCP ? BT_NO_AVRCP
                 : rc == BTS_BAD ? BT_BAD_VALUE : BT_BUSY;
        }
        reply(port, d, 3);
    } else if (sub == 0x06) {
        if (n >= 13) {
            bts_policy_t p;
            p.mode = a[1];
            p.flags = a[2];
            p.hold_ds = a[3];
            p.sweep_s = a[4];
            p.has_pref = a[5];
            unpack_addr(a + 6, p.pref);
            p.recon = n >= 15 ? a[14] : rw.src.pol.recon;   /* ... nor a reconnect choice (2026-11-09) */
            p.xopt = n >= 16 ? a[15] : rw.src.pol.xopt;     /* ... nor the experimental options */
            p.xwait = n >= 17 ? a[16] : rw.src.pol.xwait;   /* ... nor the late stop's wait */
            if (bts_policy_check(&p) != 0 || (n >= 14 && a[13] != 0))   /* strategy 0 only */
                d[1] = BT_BAD_VALUE;
            else if (!bound(port))
                d[1] = BT_NOT_BOUND;
            else {
                uint8_t was[BTS_SAVED_RAW], now_[BTS_SAVED_RAW];
                bts_saved_encode(&rw.src.pol, was);
                rw.src.pol = p;   /* RAM; recon + the preferred device + follow also go to NOR (kv_tick) */
                bts_saved_encode(&rw.src.pol, now_);
#if BTC_KV
                if (memcmp(was, now_, sizeof was) != 0)
                    suc_ng_changed();   /* the power-off snapshot follows */
#endif
                kv_note_change();
                d[1] = BT_OK;
            }
        } else {
            d[1] = n == 1 ? BT_OK : BT_BAD_VALUE;
        }
        policy_out(d + 2);
        reply(port, d, 2 + 5 + 7 + 5);
    } else if (sub == 0x07) {
        if (!bound(port))
            d[1] = BT_NOT_BOUND;
        else if (stk_bt_power == 0 || stk_bt_audio_cfg == 0)
            d[1] = BT_MODULE_OFF;
        else if (!pwc_on())
            d[1] = BT_NOT_ON;
        else if (st.rec_phase || q_active())
            d[1] = BT_BUSY;
        else {
            src_restart_begin();
            d[1] = BT_OK;
        }
        reply(port, d, 2);
    } else if (sub == 0x09) {
        /* AUTOCALL: state / read (`SET`) / off / restore - the module SETTING writes only from here, bound only */
        uint8_t o = n >= 2 ? a[1] : 0;
        const char *line = bts_autocall_line(&rw.src);
        uint32_t len = (uint32_t)strlen(line), k;
        if (o > BTS_AC_RESTORE)
            d[1] = BT_BAD_VALUE;
        else if ((d[1] = set_guard(port, o)) == BT_OK && o)
            d[1] = set_rc(bts_autocall(&rw.src, o));
        d[2] = o;
        d[3] = rw.src.ac_state;
        d[4] = rw.src.ac_op;
        d[5] = rw.src.ac_rc;
        d[6] = rw.src.ac_saved;
        d[7] = (uint8_t)len;
        for (k = 0; k < len && 8 + k < REPLY_MAX; k++)
            d[8 + k] = (uint8_t)line[k];
        reply(port, d, 8 + k);
    } else if (sub == 0x0A) {
        /* class of device: state / read (`SET`) / write an audio sink class / restore - with 1D 09 the only module
         * SETTING writes, bound only (field 2026-11-09: an Android TV does not list a class-001f00 sink) */
        uint8_t o = n >= 2 ? a[1] : 0;
        uint32_t want = n >= 6 ? ((uint32_t)a[2] << 21) | ((uint32_t)a[3] << 14) | ((uint32_t)a[4] << 7) | a[5] : 0;
        if (o > BTS_CO_RESTORE || (n >= 3 && n != 6) || (n == 6 && (o != BTS_CO_WRITE || !bts_cod_ok(want))))
            d[1] = BT_BAD_VALUE;
        else if ((d[1] = set_guard(port, o)) == BT_OK && o)
            d[1] = set_rc(bts_class(&rw.src, o, want));
        d[2] = o;
        d[3] = rw.src.cod_state;
        d[4] = rw.src.cod_op;
        d[5] = rw.src.cod_rc;
        d[6] = rw.src.cod_saved;
        put_cod(d + 7, rw.src.cod);
        put_cod(d + 11, bts_class_restore(&rw.src));
        put_cod(d + 15, rw.src.cod_want ? rw.src.cod_want : BTS_COD_AUDIO_SINK);
        reply(port, d, 19);
    } else if (sub == 0x0C) {
        /* AVRCP controller features: state / read (`SET`) / write without the browsing bit / restore - with 1D 09 /
         * 1D 0A the only module SETTING writes, bound only (field 2026-11-10: a Windows PC's lone browsing channel
         * 0x1b rebooted the module) */
        uint8_t o = n >= 2 ? a[1] : 0;
        uint8_t want = n >= 3 ? a[2] : 0;
        if (o > BTS_AV_RESTORE || n > 3 || (n == 3 && (o != BTS_AV_WRITE || !bts_avf_ok(want))))
            d[1] = BT_BAD_VALUE;
        else if ((d[1] = set_guard(port, o)) == BT_OK && o)
            d[1] = set_rc(bts_avrcp_features(&rw.src, o, want));
        d[2] = o;
        d[3] = rw.src.avf_state;
        d[4] = rw.src.avf_op;
        d[5] = rw.src.avf_rc;
        d[6] = rw.src.avf_saved;
        d[7] = (uint8_t)(rw.src.avf >> 7);
        d[8] = (uint8_t)(rw.src.avf & 0x7F);
        d[9] = (uint8_t)(bts_avf_restore(&rw.src) >> 7);
        d[10] = (uint8_t)(bts_avf_restore(&rw.src) & 0x7F);
        want = rw.src.avf_want ? rw.src.avf_want : (uint8_t)BTS_AVF_NO_BROWSE;
        d[11] = (uint8_t)(want >> 7);
        d[12] = (uint8_t)(want & 0x7F);
        reply(port, d, 13);
    } else if (sub == 0x0D) {
        /* audio routing `SET CONTROL AUDIO INTERNAL I2S_SLAVE …`: state / read (`SET`) / write one of the two fixed
         * EVENT lines / restore - with 1D 09 / 1D 0A / 1D 0C the only module SETTING writes, bound only (field
         * 2026-11-11: phone (aptX) -> PC silent) */
        uint8_t o = n >= 2 ? a[1] : 0;
        uint8_t want = n >= 3 ? a[2] : 0;
        int8_t rd;
        if (o > BTS_AU_RESTORE || n > 3 || (n == 3 && (o != BTS_AU_WRITE || !bts_aud_ok(want))))
            d[1] = BT_BAD_VALUE;
        else if ((d[1] = set_guard(port, o)) == BT_OK && o)
            d[1] = set_rc(bts_audio(&rw.src, o, want));
        d[2] = o;
        d[3] = rw.src.aud_state;
        d[4] = rw.src.aud_op;
        d[5] = rw.src.aud_rc;
        d[6] = rw.src.aud_saved;
        d[7] = rw.src.aud;
        d[8] = bts_aud_restore(&rw.src);
        d[9] = rw.src.aud_want ? rw.src.aud_want : (uint8_t)BTS_AUD_EVENT;
        /* the last `AUDIO ROUTE` event: 1 + link id (0 none) | 0x40 A2DP, the link's device index (0x7F none) */
        rd = rw.src.route ? rw.src.link_dev[rw.src.route - 1] : -1;
        d[10] = (uint8_t)(rw.src.route | (rw.src.route_a2dp ? 0x40u : 0));
        d[11] = rd < 0 ? 0x7F : (uint8_t)rd;
        reply(port, d, 12);
    } else if (sub == 0x0B) {
        /* the record-bus level (read only, any port): DSP L2 reads, nothing written anywhere but st */
        uint8_t raw[BT_LVL_RAW];
        btc_level(raw);
        d[1] = BT_OK;
        reply(port, d, 2 + pack7(raw, sizeof raw, d + 2));
    } else if (sub == 0x08) {
        /* module line log (developer view): records from sequence <from> on (any port, read only) */
        uint16_t first, next, from = n >= 3 ? (uint16_t)(((uint16_t)a[1] << 7) | a[2]) : 0;
        uint32_t t = rw.src.now_ms / 10u, k;
        k = bts_log_read(&rw.src, n >= 3 ? from : rw.src.log.first, d + 10, BT_LOG_CHUNK, &first, &next);
        d[1] = BT_OK;
        d[2] = (uint8_t)((first >> 7) & 0x7F);
        d[3] = (uint8_t)(first & 0x7F);
        d[4] = (uint8_t)((next >> 7) & 0x7F);
        d[5] = (uint8_t)(next & 0x7F);
        d[6] = (uint8_t)((t >> 21) & 0x7F);
        d[7] = (uint8_t)((t >> 14) & 0x7F);
        d[8] = (uint8_t)((t >> 7) & 0x7F);
        d[9] = (uint8_t)(t & 0x7F);
        reply(port, d, 10 + k);
    }
    req_pending = 0;
}

/* ---- entry points ---------------------------------------------------------------------------- */
void btc_sysex(uint8_t stock_port, const uint8_t *buf, uint32_t len)
{
    /* the cave checked F0 7D C A 7 9 and len >= 8 */
    if (len < 8 || len > REQ_MAX || buf[len - 1] != 0xF7)
        return;
    if (len == 9 && buf[7] == 0x0F) {
        /* heartbeat: answered here, on task 19, so it reads even while the engine loop (task 24) is stuck */
        uint8_t raw[BT_HB_RAW], d[2 + BT_HB_RAW + 3];
        btc_heartbeat(raw);
        d[0] = 0x0F;
        d[1] = BT_OK;
        reply(stock_port & 3, d, 2 + pack7(raw, sizeof raw, d + 2));
        return;
    }
    if (st.req_seen < 0xFFFFu)
        st.req_seen++;
    if (req_pending) {   /* busy: dropped (silent), the app asks again */
        if (st.req_busy < 0xFFFFu)
            st.req_busy++;
        return;
    }
    memcpy(req_buf, buf, len);
    req_len = (uint8_t)len;
    req_port = stock_port & 3;
    req_pending = 1;
}

void btc_tick(void)
{
    uint32_t now = hal_time_us(), elapsed;
    hb_passes++;
    if (!st.inited) {
        memset(&rw, 0, sizeof rw);
        bts_init(&rw.src, 0);
        /* a payload applied live: the one before handed its devices and the selection over (a cold boot: nothing) */
        if (bts_handover_load(&rw.src, bt_handover, BTS_HO_RAW, now) < 0 && stk_binding != 0) {
            /* a live apply without a block taken (from the built-in, from a payload before the handover block, a stale or bad block):
             * a controller is bound - the app that applied it; at a cold boot nothing is bound before the first tick
             * (startup_cave.c boot_check uses the same sign).  The module keeps its links over the apply: the pin
             * never restarts it */
            rw.src.live = 1;
            bts_log(&rw.src, BTS_L_EVENT, "live apply: no block", 20);
        }
        memset(bt_handover, 0, BTS_HO_RAW);
        st.inited = 1;
        st.sw = BT_SW_ALL;
        st.last_us = now;
        st.acc_us = 0;
        elapsed = 0;
    } else {
        st.acc_us += now - st.last_us;
        st.last_us = now;
        elapsed = st.acc_us / 1000u;
        st.acc_us -= elapsed * 1000u;
    }
    if (ho_written) {   /* ticked again after it said idle: that apply did not happen - the block is not for anyone */
        memset(bt_handover, 0, BTS_HO_RAW);
        ho_written = 0;
    }
    p2_unlatch(elapsed);
    p2_check();
    as_guard();
    q_tick(elapsed);
    kv_tick();
    src_tick(elapsed);
    tx_pump(elapsed);
    if (req_pending)
        serve_request();
}

int btc_quiesce(void)
{
    return req_pending || q_active() || tx_busy() || st.rec_phase || st.kv_dirty;
}

/* the kept part now, for the startup snapshot's NG part (startup_cave.h; task 24) */
void btc_ng_get(uint8_t *out) { bts_saved_encode(&rw.src.pol, out); }

void btc_handover(void)
{
    if (!st.inited)
        return;
    (void)bts_handover_save(&rw.src, hal_time_us(), bt_handover);
    ho_written = 1;
}

#ifdef BTC_HOST
void btc_host_pin(uint8_t phase) { rw.src.pin = phase; }
#endif

void btc_restart(void)
{
    ho_written = 0;
    req_pending = 0;
    hb_passes = 0;
    hb_stage = 0;
    memset(&st, 0, sizeof st);
}
