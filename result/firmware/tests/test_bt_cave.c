/* Tests for the cave BT port (ports/cave/bt_cave.c, slot payload ota_bt): BLE-MIDI port 2 RX latch
 * clear (F1), port 2 active-sensing guard (F5), BT audio module read-only query (F2) and op 0x1D.
 * The stock routines of bt_stock.h are recording stand-ins here; the UART1 ring, the module power word,
 * the special-mode byte and the binding are tests/test_power_cave.c's stand-ins (same binary). */
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>

#define BTC_HOST 1   /* btc_host_pin */
#include "../ports/cave/bt_cave.h"
#include "../ports/cave/sysex_cave.h"
#include "../ports/cave/power_cave.h"
#include "ca79ng/btsrc.h"
#include "ca79ng/standby.h"
#include "ca79ng/storage.h"
#include "../ports/cave/stock_host.h"
#include "test.h"

#define CHECK_STR(a, b)                                                                     \
    do {                                                                                    \
        t_checks++;                                                                         \
        if (strcmp((a), (b)) != 0) {                                                        \
            t_failures++;                                                                   \
            fprintf(stderr, "  FAIL %s:%d: \"%s\" vs \"%s\"\n", __FILE__, __LINE__, (a), (b)); \
        }                                                                                   \
    } while (0)

/* ---- stock stand-ins (bt_stock.h) ---- */
volatile uint8_t stk_p2_as;
volatile uint8_t stk_p2_rx_flag;
volatile uint16_t stk_p2_rx_count;
volatile uint16_t stk_p2_rx_rd;
volatile uint16_t stk_p2_rx_wr;
volatile uint32_t stk_irq_hier;
volatile uint16_t stk_bt_tx_cnt;   /* UART1 TX state: idle unless a test makes it busy */
volatile uint16_t stk_bt_tx_rd;
volatile uint16_t stk_bt_tx_wr;
volatile uint8_t stk_uart1_ier;
/* the DSP record-bus capture in DSP L2 (1D 0B) */
volatile uint32_t stk_dsp_rec_buf[0x480];   /* bt_stock.h STK_DSP_REC_WORDS */
volatile uint32_t stk_dsp_rec_wp;
volatile uint16_t stk_dsp_rec_n;
volatile uint16_t stk_dsp_rec_state;
volatile uint16_t stk_dsp_rec_gain;

/* defined by tests/test_power_cave.c (power_stock.h) */
extern volatile uint8_t stk_bt_rx_ring[0x800];
extern volatile uint16_t stk_bt_rx_wr;
extern volatile uint16_t stk_bt_rx_rd;
extern volatile uint8_t stk_bt_rx_flag;
extern volatile uint32_t stk_bt_power;
extern volatile uint8_t stk_special_mode;
extern volatile uint8_t stk_binding;

static struct {
    int resets, resets_masked;
    int tx_calls;      /* complete commands (a chunk ending in \n closes one) */
    int tx_chunks;     /* FUN_4014512C calls */
    uint32_t max_len;  /* longest FUN_4014512C call (must be 1) */
    char tx[20][64];
    char first[20][40]; /* the first chunk of each command */
} bs;

/* FUN_40145DFC, the stock store order */
void stk_p2_rx_reset(void)
{
    bs.resets++;
    if (!(stk_irq_hier & 0x02u))
        bs.resets_masked++;
    stk_p2_rx_wr = 0;
    stk_p2_rx_flag = 0;
    stk_p2_rx_rd = 0;
    stk_p2_as = 0xFF;
    stk_p2_rx_count = 0;
}

/* the module commands go out in pieces (first byte, then the rest): collected per command */
void stk_bt_tx(const uint8_t *buf, uint32_t len)
{
    bs.tx_chunks++;
    if (len > bs.max_len)
        bs.max_len = len;
    if (bs.tx_calls < 20) {
        char *t = bs.tx[bs.tx_calls];
        size_t have = strlen(t);
        uint32_t n = len < 63 - have ? len : (uint32_t)(63 - have);
        if (have == 0) {
            memcpy(bs.first[bs.tx_calls], buf, n);
            bs.first[bs.tx_calls][n] = 0;
        }
        memcpy(t + have, buf, n);
        t[have + n] = 0;
    }
    if (len && buf[len - 1] == '\n')
        bs.tx_calls++;
}

/* ---- the power port's kvstore as the BT port sees it (startup_cave.h; bt_cave.o is built with BTC_KV here) ---- */
#define KSEC 4096u
static uint8_t kmem[2][KSEC];
static int k_puts;          /* records programmed */
static int k_known = 1;     /* pwc_kv_known() */
static int k_idle = 1;      /* pwc_flash_idle() */
static int k_keep;          /* boot() keeps the store (a power cycle) */
static int k_late;          /* boot(): the store probe has not answered yet */
static bool k_erase(void *c, uint32_t sec) { (void)c; memset(kmem[sec], 0xFF, KSEC); return true; }
static bool k_prog(void *c, uint32_t sec, uint32_t off, const void *d, uint32_t n)
{
    (void)c;
    for (uint32_t i = 0; i < n; i++)
        kmem[sec][off + i] &= ((const uint8_t *)d)[i];
    k_puts++;
    return true;
}
static bool k_read(void *c, uint32_t sec, uint32_t off, void *d, uint32_t n) { (void)c; memcpy(d, kmem[sec] + off, n); return true; }
static const hal_flash_t KFLASH = {0, KSEC, k_erase, k_prog, k_read};
static kvstore_t kstore;
static int k_mounted;
kvstore_t *pwc_kv(int mount);
int pwc_kv_known(void);
int pwc_flash_idle(void);
kvstore_t *pwc_kv(int mount)
{
    if (!k_mounted) {
        bool blank = true;
        for (uint32_t i = 0; i < 2 * KSEC && blank; i++)
            blank = kmem[i / KSEC][i % KSEC] == 0xFF;
        if (blank && !mount)
            return NULL;   /* as power_cave.c: a blank store stays unmounted until the first write */
        if (!kv_mount(&kstore, &KFLASH, 0, 1))
            return NULL;
        k_mounted = 1;
    }
    return &kstore;
}
int pwc_kv_known(void) { return k_known; }
int pwc_flash_idle(void) { return k_idle; }

/* the startup settings as the BT port sees them (startup_cave.h suc_ng): by default the live
 * value stays (KSU_NG_KEEP) - the tests written before see the port as before */
static int s_ng = 0;              /* -1 wait, 0 keep, 1 defaults, 2 saved */
static uint8_t s_ng_bt[10];       /* the saved part (BTS_SAVED_RAW) */
static uint8_t s_ng_applied;
static int s_ng_changes;
int suc_ng(const uint8_t **bt, uint8_t *btn, const uint8_t **pw, uint8_t *pwn);
void suc_ng_applied(uint8_t bit);
void suc_ng_changed(void);
int suc_ng(const uint8_t **bt, uint8_t *btn, const uint8_t **pw, uint8_t *pwn)
{
    *btn = *pwn = 0;
    *pw = 0;
    if (s_ng == 2) {
        *bt = s_ng_bt;
        *btn = sizeof s_ng_bt;
    }
    return s_ng;
}
void suc_ng_applied(uint8_t bit) { s_ng_applied |= bit; }
void suc_ng_changed(void) { s_ng_changes++; }

static uint32_t now_ms;

static void run_ms(uint32_t ms)
{
    for (uint32_t t = 0; t < ms; t += 8) {
        now_ms += 8;
        stock_host.time_us = now_ms * 1000u;
        btc_tick();
    }
}

static int boot_pin;   /* boot(): 1 = the source manager's power-on pin runs (its own tests) */
static int boot_bound; /* boot(): 1 = a controller is bound before the first tick - a live apply without a handover block
                        * 0 = a cold boot: nothing bound until the app connects (after it) */

static void boot(void)
{
    if (!k_keep)
        memset(kmem, 0xFF, sizeof kmem);
    k_mounted = 0;   /* a power-on mounts again */
    k_known = !k_late;
    k_idle = 1;
    k_puts = 0;
    stock_host_reset();
    btc_restart();
    pwc_restart();   /* the power port not running: pwc_on() = 0, the source manager sends nothing */
    memset(&bs, 0, sizeof bs);
    stk_p2_as = 0xFF;
    stk_p2_rx_flag = 0;
    stk_p2_rx_count = 0;
    stk_p2_rx_rd = stk_p2_rx_wr = 0x123;
    stk_irq_hier = 0x03;
    stk_bt_tx_cnt = stk_bt_tx_rd = stk_bt_tx_wr = 0;
    stk_uart1_ier = 0;
    memset((void *)stk_bt_rx_ring, 0, sizeof stk_bt_rx_ring);
    stk_bt_rx_wr = stk_bt_rx_rd = 0x40;
    stk_bt_rx_flag = 0;
    stk_bt_power = 1;
    stk_special_mode = 0;
    stk_binding = boot_bound ? 3 : 0;   /* a cold boot: no controller bound at the first tick */
    now_ms = 0;
    run_ms(8);
    stk_binding = 3;   /* the app on BLE (stock port 2) */
    if (!boot_pin)   /* the tests written before the power-on pin check other paths on a factory module listing: no pin */
        btc_host_pin(BTS_PIN_DONE);
}

static void bt_line(const char *s)
{
    while (*s) {
        stk_bt_rx_ring[stk_bt_rx_wr & 0x7FF] = (uint8_t)*s++;
        stk_bt_rx_wr = (uint16_t)((stk_bt_rx_wr + 1) & 0x7FF);
    }
    stk_bt_rx_ring[stk_bt_rx_wr] = '\r';
    stk_bt_rx_wr = (uint16_t)((stk_bt_rx_wr + 1) & 0x7FF);
    stk_bt_rx_ring[stk_bt_rx_wr] = '\n';
    stk_bt_rx_wr = (uint16_t)((stk_bt_rx_wr + 1) & 0x7FF);
    stk_bt_rx_rd = stk_bt_rx_wr;   /* the stock assembler took it */
}

/* request 1D <args> on a stock port, one pass, the reply data after 5D (NULL if none) */
static const uint8_t *op(uint8_t port, const uint8_t *args, size_t n, uint32_t *len)
{
    uint8_t m[32] = {0xF0, 0x7D, 0x43, 0x41, 0x37, 0x39, BT_OP};
    memcpy(m + 7, args, n);
    m[7 + n] = 0xF7;
    int before = stock_host.tx_calls[0] + stock_host.tx_calls[1] + stock_host.tx_calls[2] + stock_host.tx_calls[3];
    btc_sysex(port, m, (uint32_t)(8 + n));
    run_ms(8);
    int after = stock_host.tx_calls[0] + stock_host.tx_calls[1] + stock_host.tx_calls[2] + stock_host.tx_calls[3];
    if (after == before || stock_host.last_tx_len < 8 || stock_host.last_tx[6] != (BT_OP | 0x40))
        return NULL;
    *len = stock_host.last_tx_len - 8;
    return stock_host.last_tx + 7;
}

static bool status(uint8_t raw[BT_STATUS_RAW])
{
    uint32_t n;
    const uint8_t a[1] = {0};
    const uint8_t *r = op(2, a, 1, &n);
    if (!r || n < 2 || r[0] != 0 || r[1] != 0)
        return false;
    /* unpack7 */
    size_t o = 0;
    for (size_t i = 2; i < n && o < BT_STATUS_RAW;) {
        uint8_t hi = r[i++];
        for (size_t k = 0; k < 7 && i < n && o < BT_STATUS_RAW; k++, i++)
            raw[o++] = (uint8_t)(r[i] | (((hi >> k) & 1u) << 7));
    }
    return o == BT_STATUS_RAW;
}

static uint8_t rc_of(const uint8_t *args, size_t n)
{
    uint32_t len;
    const uint8_t *r = op(2, args, n, &len);
    return r && len >= 2 ? r[1] : 0xFF;
}

/* the whole captured text through 1D 02 chunks */
static int read_text(char *out, int cap)
{
    int got = 0;
    for (int guard = 0; guard < 100; guard++) {
        uint32_t n;
        const uint8_t a[3] = {2, (uint8_t)((got >> 7) & 0x7F), (uint8_t)(got & 0x7F)};
        const uint8_t *r = op(2, a, 3, &n);
        if (!r || n < 7 || r[1] != 0)
            return -1;
        int total = (r[3] << 7) | r[4];
        int k = (int)n - 7;
        if (got + k >= cap)
            return -1;
        memcpy(out + got, r + 7, (size_t)k);
        got += k;
        if (got >= total || k == 0)
            break;
    }
    out[got] = 0;
    return got;
}

static void test_bt_status_default(void)
{
    uint8_t raw[BT_STATUS_RAW];
    boot();
    CHECK(status(raw));
    CHECK_EQ(raw[0], BT_STATUS_LAYOUT);
    CHECK_EQ(raw[1], BT_SW_ALL);       /* F1 + F5 on after every power-on / apply */
    CHECK_EQ(raw[2], BT_Q_IDLE);
    CHECK_EQ(raw[9], 0xFF);            /* port 2 AS counter */
    CHECK_EQ(raw[18] & 0x01, 0x01);    /* module on */
    CHECK_EQ(btc_quiesce(), 0);
}

static void test_bt_p2_unlatch(void)
{
    uint8_t raw[BT_STATUS_RAW];
    boot();
    /* the pump's error path: reset, then flag 0x80 with an empty ring */
    stk_p2_rx_rd = stk_p2_rx_wr = 0;
    stk_p2_rx_flag = 0x80;
    run_ms(400);
    CHECK_EQ(bs.resets, 0);
    CHECK(status(raw));
    CHECK_EQ(raw[18] & 0x04, 0x04);    /* latched */
    run_ms(200);
    CHECK_EQ(bs.resets, 1);
    CHECK_EQ(bs.resets_masked, 1);     /* FUN_40145DFC ran with the IRQ line masked ... */
    CHECK_EQ(stk_irq_hier, 0x03u);     /* ... and it is unmasked again, FIQ bit untouched */
    CHECK_EQ(stk_p2_rx_flag, 0);
    CHECK(status(raw));
    CHECK_EQ(raw[12], 1);              /* unlatches */
    /* latched again 1 s later: the wait doubles (1 s) */
    run_ms(1000);
    stk_p2_rx_flag = 0x80;
    run_ms(800);
    CHECK_EQ(bs.resets, 1);
    run_ms(300);
    CHECK_EQ(bs.resets, 2);
    CHECK(status(raw));
    CHECK_EQ(raw[16], 1);              /* streak */
    /* and again at once: 2 s */
    stk_p2_rx_flag = 0x80;
    run_ms(1800);
    CHECK_EQ(bs.resets, 2);
    run_ms(300);
    CHECK_EQ(bs.resets, 3);
    /* quiet for 6 s, then a latch: back to 0.5 s */
    run_ms(6000);
    stk_p2_rx_flag = 0x80;
    run_ms(600);
    CHECK_EQ(bs.resets, 4);
    CHECK(status(raw));
    CHECK_EQ(raw[16], 0);
}

static void test_bt_p2_unlatch_conditions(void)
{
    const uint8_t sw_f5_only[2] = {3, BT_SW_AS_GUARD};
    boot();
    /* bytes still counted, an error not yet handled by the pump (0x84), indices apart: never */
    stk_p2_rx_rd = stk_p2_rx_wr = 0;
    stk_p2_rx_flag = 0x80;
    stk_p2_rx_count = 1;
    run_ms(2000);
    stk_p2_rx_count = 0;
    stk_p2_rx_flag = 0x84;
    run_ms(2000);
    stk_p2_rx_flag = 0x80;
    stk_p2_rx_wr = 5;
    run_ms(2000);
    CHECK_EQ(bs.resets, 0);
    CHECK_EQ(stk_irq_hier, 0x03u);     /* never masked without a clear */
    /* switched off by the bound controller: never either */
    stk_p2_rx_wr = 0;
    CHECK_EQ(rc_of(sw_f5_only, 2), BT_OK);
    run_ms(2000);
    CHECK_EQ(bs.resets, 0);
    CHECK_EQ(stk_p2_rx_flag, 0x80);
}

static void test_bt_as_guard(void)
{
    uint8_t raw[BT_STATUS_RAW];
    const uint8_t sw_f1_only[2] = {3, BT_SW_P2_UNLATCH};
    boot();
    stk_p2_as = 0x11;                  /* the ISR saw 0xFE */
    run_ms(8);
    CHECK_EQ(stk_p2_as, 0xFF);
    stk_p2_as = 0x03;                  /* even late in the count-down */
    run_ms(8);
    CHECK_EQ(stk_p2_as, 0xFF);
    stk_p2_as = 0x00;                  /* expiry already flagged: the watchdog's to reset */
    run_ms(8);
    CHECK_EQ(stk_p2_as, 0x00);
    CHECK(status(raw));
    CHECK_EQ(raw[14], 2);              /* disarms */
    /* a 0xFE sender every 300 ms, 20 ms count-down: never reaches 0 */
    stk_p2_as = 0xFF;
    int expired = 0;
    for (int t = 0; t < 5000; t += 4) {
        if (t % 300 == 0)
            stk_p2_as = 0x11;
        if (t % 20 == 0 && stk_p2_as != 0xFF && stk_p2_as != 0 && --stk_p2_as == 0)
            expired++;
        if (t % 8 == 0)
            run_ms(8);
    }
    CHECK_EQ(expired, 0);
    /* switched off: the stock count-down runs (stock meaning) */
    CHECK_EQ(rc_of(sw_f1_only, 2), BT_OK);
    stk_p2_as = 0x11;
    run_ms(80);
    CHECK_EQ(stk_p2_as, 0x11);
}

static void test_bt_switches(void)
{
    const uint8_t sw0[2] = {3, 0}, bad[2] = {3, 4};
    boot();
    stk_binding = 4;                   /* the panel is bound */
    CHECK_EQ(rc_of(sw0, 2), BT_NOT_BOUND);
    stk_binding = 3;
    CHECK_EQ(rc_of(bad, 2), BT_BAD_VALUE);
    CHECK_EQ(rc_of(sw0, 2), BT_OK);
    stk_p2_as = 0x11;
    run_ms(40);
    CHECK_EQ(stk_p2_as, 0x11);
    btc_restart();                     /* a power-on / apply: both on again */
    run_ms(16);
    CHECK_EQ(stk_p2_as, 0xFF);
}

static void test_bt_query(void)
{
    uint8_t raw[BT_STATUS_RAW];
    const uint8_t all[2] = {1, 0};
    char text[BT_CAP + 1];
    boot();
    bt_line("A2DP STREAMING START 0");             /* before the query: not captured */
    CHECK_EQ(rc_of(all, 2), BT_OK);
    CHECK_EQ(bs.tx_calls, 0);                      /* the reply first, the command on the next pass */
    CHECK_EQ(btc_quiesce(), 1);
    run_ms(8);
    CHECK_EQ(bs.tx_calls, 0);                      /* the first byte alone ... */
    CHECK_EQ(bs.tx_chunks, 1);
    CHECK_STR(bs.tx[0], "S");
    run_ms(8);
    CHECK_EQ(bs.tx_calls, 0);
    run_ms(8);                                     /* ... the rest 16 ms later, one byte per FUN_4014512C call */
    CHECK_EQ(bs.tx_calls, 1);
    CHECK_EQ(bs.tx_chunks, 5);
    CHECK_EQ(bs.max_len, 1);
    CHECK_STR(bs.tx[0], "SET\r\n");
    CHECK_STR(bs.first[0], "S");
    run_ms(8);
    bt_line("SET");                                /* the module's echo (SET CONTROL ECHO 7) ... */
    run_ms(600);
    CHECK_EQ(bs.tx_calls, 1);                      /* ... is not an answer: still waiting for the list */
    bt_line("SET BT BDADDR 02:07:80:aa:bb:cc");
    run_ms(16);
    bt_line("SET BT NAME CA79");
    bt_line("SET BT PAGEMODE 3 2000 1");
    bt_line("SET PROFILE A2DP SINK");
    bt_line("SET");
    run_ms(400);
    CHECK_EQ(bs.tx_calls, 1);                      /* still listening: 500 ms of silence ends a reply */
    run_ms(200);
    CHECK_EQ(bs.tx_calls, 2);
    CHECK_STR(bs.tx[1], "LIST\r\n");
    run_ms(1800);                                  /* no reply to LIST (2 s) */
    CHECK_EQ(bs.tx_calls, 2);
    run_ms(300);
    CHECK_EQ(bs.tx_calls, 3);
    CHECK_STR(bs.tx[2], "INFO\r\n");
    CHECK_STR(bs.first[2], "I");
    run_ms(16);
    bt_line("WRAP THOR AI (4.1.0 build 1066)");
    run_ms(600);
    CHECK_EQ(btc_quiesce(), 0);
    CHECK(status(raw));
    CHECK_EQ(raw[2], BT_Q_DONE);
    CHECK_EQ(raw[6], 3);
    CHECK_EQ(raw[3], BT_QF_SILENT);
    CHECK(read_text(text, sizeof text) > 0);
    CHECK_STR(text, "> SET\r\nSET\r\nSET BT BDADDR 02:07:80:aa:bb:cc\r\nSET BT NAME CA79\r\nSET BT PAGEMODE 3 2000 1\r\n"
                    "SET PROFILE A2DP SINK\r\nSET\r\n> LIST\r\n> INFO\r\nWRAP THOR AI (4.1.0 build 1066)\r\n");
    /* only the three fixed commands ever went out */
    for (int i = 0; i < bs.tx_calls && i < 8; i++)
        CHECK(!strcmp(bs.tx[i], "SET\r\n") || !strcmp(bs.tx[i], "LIST\r\n") || !strcmp(bs.tx[i], "INFO\r\n"));
    /* one command: LIST */
    const uint8_t list[2] = {1, 2};
    CHECK_EQ(rc_of(list, 2), BT_OK);
    run_ms(32);
    CHECK_STR(bs.tx[3], "LIST\r\n");
    bt_line("LIST 1");
    bt_line("LIST 0 CONNECTED A2DP 668 0 0 4 8d 8d 12:22:33:44:55:66 19 OUTGOING ACTIVE SLAVE PLAIN 0");
    run_ms(600);
    CHECK(read_text(text, sizeof text) > 0);
    CHECK(strncmp(text, "> LIST\r\nLIST 1\r\nLIST 0 CONNECTED A2DP", 37) == 0);
    CHECK_EQ(bs.tx_calls, 4);
    /* INFO answered by its echo only (field 2026-11-01): flagged ECHO_ONLY after 2 s */
    const uint8_t info[2] = {1, 3};
    CHECK_EQ(rc_of(info, 2), BT_OK);
    run_ms(32);
    bt_line("INFO");
    run_ms(1500);
    CHECK(status(raw));
    CHECK_EQ(raw[2], BT_Q_COLLECT);
    run_ms(600);
    CHECK(status(raw));
    CHECK_EQ(raw[2], BT_Q_DONE);
    CHECK_EQ(raw[3] & BT_QF_ECHO_ONLY, BT_QF_ECHO_ONLY);
}

static void test_bt_query_refused(void)
{
    const uint8_t all[2] = {1, 0}, bad[2] = {1, 4};
    boot();
    CHECK_EQ(rc_of(bad, 2), BT_BAD_VALUE);
    stk_bt_power = 0;
    CHECK_EQ(rc_of(all, 2), BT_MODULE_OFF);
    stk_bt_power = 1;
    stk_bt_rx_flag = 0x80;
    CHECK_EQ(rc_of(all, 2), BT_RX_LATCHED);
    stk_bt_rx_flag = 0;
    stk_special_mode = 1;
    CHECK_EQ(rc_of(all, 2), BT_TEST_MODE);
    stk_special_mode = 0;
    CHECK_EQ(rc_of(all, 2), BT_OK);
    CHECK_EQ(rc_of(all, 2), BT_BUSY);
    run_ms(24);
    CHECK_EQ(bs.tx_calls, 1);
    /* the module switched off mid-query: the rest is not sent */
    stk_bt_power = 0;
    run_ms(2000);
    uint8_t raw[BT_STATUS_RAW];
    CHECK(status(raw));
    CHECK_EQ(raw[2], BT_Q_DONE);
    CHECK_EQ(raw[3] & BT_QF_ABORT, BT_QF_ABORT);
    CHECK_EQ(bs.tx_calls, 1);
    /* a cut chunk offset */
    uint32_t n;
    const uint8_t far_[3] = {2, 0x7F, 0x7F};
    const uint8_t *r = op(2, far_, 3, &n);
    CHECK(r && n == 7 && r[1] == BT_BAD_VALUE);
}

static void test_bt_query_ring_reset(void)
{
    const uint8_t set[2] = {1, 1};
    char text[BT_CAP + 1];
    uint8_t raw[BT_STATUS_RAW];
    boot();
    /* an old line in the ring past the write index (previous lap) */
    stk_bt_rx_wr = stk_bt_rx_rd = 0x700;
    bt_line("SET BT NAME OLDLAP");
    stk_bt_rx_wr = stk_bt_rx_rd = 0x600;
    CHECK_EQ(rc_of(set, 2), BT_OK);
    run_ms(40);
    bt_line("SET BT NAME CA79");
    run_ms(8);
    stk_bt_rx_flag = 0x01;                         /* overflow: the assembler resets the ring */
    run_ms(8);
    stk_bt_rx_wr = stk_bt_rx_rd = 0;
    stk_bt_rx_flag = 0;
    bt_line("SET");
    run_ms(600);
    CHECK(status(raw));
    CHECK_EQ(raw[3] & BT_QF_RESET, BT_QF_RESET);
    CHECK(read_text(text, sizeof text) > 0);
    CHECK_STR(text, "> SET\r\nSET BT NAME CA79\r\nSET\r\n");
}

/* M6 field: INFO answered `IFO SYNTAX ERROR` - the command goes out once more, once */
static void test_bt_query_retry(void)
{
    const uint8_t info[2] = {1, 3};
    char text[BT_CAP + 1];
    uint8_t raw[BT_STATUS_RAW];
    boot();
    CHECK_EQ(rc_of(info, 2), BT_OK);
    run_ms(40);
    CHECK_EQ(bs.tx_calls, 1);
    bt_line("IFO SYNTAX ERROR");
    run_ms(520);                                   /* 500 ms silence: the reply is over -> resend */
    run_ms(40);
    CHECK_EQ(bs.tx_calls, 2);
    CHECK_STR(bs.tx[1], "INFO\r\n");
    bt_line("SYNTAX ERROR");                       /* a second error: no third send */
    run_ms(600);
    run_ms(600);
    CHECK_EQ(bs.tx_calls, 2);
    CHECK(status(raw));
    CHECK_EQ(raw[2], BT_Q_DONE);
    CHECK_EQ(raw[3] & BT_QF_RETRY, BT_QF_RETRY);
    CHECK(read_text(text, sizeof text) > 0);
    CHECK_STR(text, "> INFO\r\nIFO SYNTAX ERROR\r\n> INFO\r\nSYNTAX ERROR\r\n");
    /* a good answer: no resend */
    boot();
    CHECK_EQ(rc_of(info, 2), BT_OK);
    run_ms(40);
    bt_line("WRAP THOR AI (6.2.0 build 1122)");
    run_ms(800);
    CHECK_EQ(bs.tx_calls, 1);
    CHECK(status(raw));
    CHECK_EQ(raw[3] & BT_QF_RETRY, 0);
}

/* F1b: the port 2 ring count must equal the bytes between the read and the write index */
static void test_bt_p2_ring_check(void)
{
    uint8_t raw[BT_STATUS_RAW];
    boot();
    /* the M4 state: a byte the ISR took inside a stock reset is in the ring but not counted */
    stk_p2_rx_rd = 0x10;
    stk_p2_rx_wr = 0x11;
    stk_p2_rx_count = 0;
    int masked_before = 0;
    run_ms(8);
    CHECK_EQ(stk_p2_rx_count, 0);                  /* first sight: wait one pass */
    run_ms(8);
    CHECK_EQ(stk_p2_rx_count, 1);                  /* counted: the pump reads it */
    CHECK_EQ(stk_p2_rx_rd, 0x10);
    CHECK_EQ(stk_irq_hier, 0x03);
    (void)masked_before;
    CHECK(status(raw));
    CHECK_EQ(raw[24] | raw[25] << 8, 1);
    CHECK_EQ((int16_t)(raw[26] | raw[27] << 8), 1);
    CHECK_EQ(raw[20] | raw[21] << 8, 0x10);
    CHECK_EQ(raw[22] | raw[23] << 8, 0x11);
    /* a whole frame counted away (9 bytes) */
    stk_p2_rx_rd = 0x20;
    stk_p2_rx_wr = 0x29;
    stk_p2_rx_count = 0;
    run_ms(16);
    CHECK_EQ(stk_p2_rx_count, 9);
    /* read index ahead of the write index (a replay of the previous lap): dropped */
    stk_p2_rx_rd = 0x40;
    stk_p2_rx_wr = 0x30;
    stk_p2_rx_count = 0;
    run_ms(16);
    CHECK_EQ(stk_p2_rx_rd, 0x30);
    CHECK_EQ(stk_p2_rx_count, 0);
    /* a count too high: the read index moves so the count matches */
    stk_p2_rx_rd = 0x50;
    stk_p2_rx_wr = 0x52;
    stk_p2_rx_count = 5;
    run_ms(16);
    CHECK_EQ(stk_p2_rx_rd, 0x4D);
    CHECK_EQ(stk_p2_rx_count, 5);
    /* a moving ring is not touched (the pump / ISR are working) */
    stk_p2_rx_rd = 0x60;
    stk_p2_rx_wr = 0x62;
    stk_p2_rx_count = 0;
    run_ms(8);
    stk_p2_rx_wr = 0x63;
    run_ms(8);
    CHECK_EQ(stk_p2_rx_count, 0);
    stk_p2_rx_count = 3;                           /* consistent again */
    run_ms(16);
    CHECK_EQ(stk_p2_rx_rd, 0x60);
    CHECK_EQ(stk_p2_rx_count, 3);
    /* error / latch flag: the stock reset and F1 own the ring */
    stk_p2_rx_rd = 0x70;
    stk_p2_rx_wr = 0x71;
    stk_p2_rx_count = 0;
    stk_p2_rx_flag = 0x04;
    run_ms(16);
    CHECK_EQ(stk_p2_rx_count, 0);
    stk_p2_rx_flag = 0;
    /* switched off with F1 (1D 03): left alone */
    const uint8_t sw0[2] = {3, 0x02};
    uint32_t n;
    CHECK(op(2, sw0, 2, &n) != NULL);
    run_ms(16);
    CHECK_EQ(stk_p2_rx_count, 0);
    CHECK_EQ(stk_irq_hier, 0x03);
}

/* F6: FUN_4014512C only with one byte and only while the stock TX state is idle; a TX
 * state that never gets idle drops the command after 1 s - the engine loop never waits */
static void test_bt_tx_discipline(void)
{
    uint8_t raw[BT_STATUS_RAW];
    const uint8_t set[2] = {1, 1};
    boot();
    stk_uart1_ier = 0x02;                          /* THRE interrupt on: a byte is still going out */
    CHECK_EQ(rc_of(set, 2), BT_OK);
    run_ms(500);
    CHECK_EQ(bs.tx_chunks, 0);                     /* nothing put while the transmitter is busy */
    stk_uart1_ier = 0;
    stk_bt_tx_cnt = 3;                             /* bytes left in the TX ring */
    run_ms(300);
    CHECK_EQ(bs.tx_chunks, 0);
    stk_bt_tx_cnt = 0;
    stk_bt_tx_rd = 5;                              /* indices apart */
    stk_bt_tx_wr = 6;
    run_ms(400);
    CHECK_EQ(bs.tx_chunks, 0);
    CHECK(status(raw));
    CHECK_EQ(raw[32] | (raw[33] << 8), 1);         /* dropped after 1 s, counted */
    CHECK_EQ(raw[38], BT_STAGE_IDLE);
    stk_bt_tx_rd = stk_bt_tx_wr = 0;
    run_ms(3000);
    CHECK_EQ(btc_quiesce(), 0);
    /* idle: every call is one byte */
    CHECK_EQ(rc_of(set, 2), BT_OK);
    run_ms(48);
    CHECK_EQ(bs.tx_calls, 1);
    CHECK_STR(bs.tx[0], "SET\r\n");
    CHECK_EQ(bs.max_len, 1);
    CHECK(status(raw));
    CHECK((raw[28] | (raw[29] << 8)) > 100);       /* engine-loop passes counted */
}

/* 1D 0F is answered inside btc_sysex (task 19): it reads while the engine loop does not run at all */
static void test_bt_heartbeat(void)
{
    boot();
    run_ms(80);
    uint8_t m[9] = {0xF0, 0x7D, 0x43, 0x41, 0x37, 0x39, BT_OP, 0x0F, 0xF7};
    int before = stock_host.tx_calls[0] + stock_host.tx_calls[1] + stock_host.tx_calls[2] + stock_host.tx_calls[3];
    btc_sysex(2, m, sizeof m);                     /* no btc_tick after it */
    CHECK_EQ(stock_host.tx_calls[0] + stock_host.tx_calls[1] + stock_host.tx_calls[2] + stock_host.tx_calls[3], before + 1);
    CHECK(stock_host.last_tx_len >= 10 && stock_host.last_tx[6] == (BT_OP | 0x40) && stock_host.last_tx[7] == 0x0F);
    uint8_t hb[BT_HB_RAW];
    btc_heartbeat(hb);
    CHECK_EQ(hb[0], 1);
    CHECK((hb[2] | (hb[3] << 8)) >= 10);           /* passes so far */
    /* the request counters: a second request while one is queued is dropped and counted */
    uint8_t s0[9] = {0xF0, 0x7D, 0x43, 0x41, 0x37, 0x39, BT_OP, 0x00, 0xF7};
    btc_sysex(2, s0, sizeof s0);
    btc_sysex(2, s0, sizeof s0);
    btc_heartbeat(hb);
    CHECK_EQ(hb[13] & 0x01, 0x01);
    CHECK_EQ(hb[15], 1);
    run_ms(8);
}

static void module_line(const char *s) { bt_line(s); }

static const uint8_t *src_list(uint8_t raw[BT_SRC_RAW])
{
    uint32_t n;
    const uint8_t a[1] = {4};
    const uint8_t *r = op(2, a, 1, &n);
    if (!r || n < 2 || r[1] != 0)
        return NULL;
    size_t o = 0;
    for (size_t i = 2; i < n && o < BT_SRC_RAW;) {
        uint8_t hi = r[i++];
        for (size_t k = 0; k < 7 && i < n && o < BT_SRC_RAW; k++, i++)
            raw[o++] = (uint8_t)(r[i] | (((hi >> k) & 1u) << 7));
    }
    return o == BT_SRC_RAW ? raw : NULL;
}

static void pack_addr(const uint8_t a[6], uint8_t out[7])
{
    out[0] = 0;
    for (int k = 0; k < 6; k++) {
        out[0] |= (uint8_t)(((a[k] >> 7) & 1u) << k);
        out[1 + k] = a[k] & 0x7F;
    }
}

/* F7 through the cave: the field regression (phone streaming, PC streams too -> silence) gets CLOSE for the phone
 * on the real UART1 path; the ops list / act / set the policy; standby sends nothing */
static void test_bt_sources_cave(void)
{
    uint8_t raw[BT_SRC_RAW];
    static const uint8_t PHONE[6] = {0x62, 0x81, 0x81, 0x81, 0x11, 0xa1};
    boot();
    pwc_tick();                                    /* the power port runs: ON */
    CHECK_EQ(pwc_on(), 1);
    run_ms(400);
    CHECK_EQ(bs.tx_calls, 1);                      /* the first pass learns the links: LIST */
    CHECK_STR(bs.tx[0], "LIST\r\n");
    module_line("LIST");
    module_line("LIST 0");
    /* the opt-in policy "newest wins" (the default is OFF since 2026-11-01 evening) */
    static const uint8_t NEWEST[13] = {6, BTS_MODE_NEWEST, BTS_F_CLOSE_OTHERS | BTS_F_KEEP_PREF | BTS_F_RECOVER, 15, 5, 0};
    CHECK_EQ(rc_of(NEWEST, 13), BT_OK);
    module_line("RING 0 62:81:81:81:11:a1 19 A2DP");
    module_line("RING 1 62:81:81:81:11:a1 19 A2DP");
    module_line("RING 2 62:81:81:81:11:a1 17 AVRCP");
    module_line("A2DP STREAMING START 1");
    run_ms(6000);
    CHECK_EQ(bs.tx_calls, 1);
    CHECK(src_list(raw) != NULL);
    CHECK_EQ(raw[1], BTS_MODE_NEWEST);
    CHECK_EQ(raw[6], 1);                           /* one stream */
    CHECK_EQ(raw[28] & 0x80, 0x80);
    CHECK(memcmp(raw + 29, PHONE, 6) == 0);
    CHECK_EQ(raw[28] & BTS_D_STREAM, BTS_D_STREAM);
    CHECK_EQ(btc_a2dp_links(), 0x03);
    module_line("RING 3 22:82:02:02:92:22 19 A2DP");
    module_line("RING 4 22:82:02:02:92:22 19 A2DP");
    module_line("A2DP STREAMING START 4");
    run_ms(3000);
    CHECK_EQ(bs.tx_calls, 4);
    CHECK(!strcmp(bs.tx[1], "CLOSE 0\r\n") || !strcmp(bs.tx[1], "CLOSE 1\r\n"));
    CHECK_STR(bs.tx[3], "CLOSE 2\r\n");
    CHECK_EQ(bs.max_len, 1);
    for (int i = 0; i < bs.tx_calls && i < 8; i++)
        CHECK(strncmp(bs.tx[i], "SET", 3) != 0);
    module_line("NO CARRIER 0 ERROR 0");
    module_line("NO CARRIER 1 ERROR 0");
    module_line("NO CARRIER 2 ERROR 0");
    run_ms(16);
    CHECK_EQ(btc_a2dp_links(), 0x18);
    /* manual: close the PC (bound controller), policy get / set, refused when not bound */
    uint8_t a[9] = {5, 0};
    static const uint8_t PC[6] = {0x22, 0x82, 0x02, 0x02, 0x92, 0x22};
    pack_addr(PC, a + 2);
    CHECK_EQ(rc_of(a, 9), BT_OK);
    run_ms(400);
    CHECK_EQ(bs.tx_calls, 6);
    pack_addr(PHONE, a + 2);
    CHECK_EQ(rc_of(a, 9), BT_NO_DEVICE);
    stk_binding = 4;
    CHECK_EQ(rc_of(a, 9), BT_NOT_BOUND);
    stk_binding = 3;
    uint8_t g[1] = {6};
    uint32_t n;
    const uint8_t *r = op(2, g, 1, &n);
    CHECK(r && n == 19 && r[1] == BT_OK && r[2] == BTS_MODE_NEWEST && r[4] == 15 && r[5] == 5 && r[6] == 0);
    CHECK_EQ(r[14], 0);                            /* the strategy: always 0 */
    CHECK_EQ(r[17], BTS_LATE_STOP_DEFAULT ? BTS_X_LATE_STOP : 0);   /* the late stop's defaults */
    CHECK_EQ(r[18], BTS_XWAIT_DEFAULT);
    uint8_t sp[14] = {6, BTS_MODE_OFF, BTS_F_CALL_PREF, 20, 9, 1};
    pack_addr(PC, sp + 6);
    r = op(2, sp, 13, &n);                         /* an older app: no strategy byte */
    CHECK(r && r[1] == BT_OK && r[2] == BTS_MODE_OFF && r[3] == BTS_F_CALL_PREF && r[4] == 20 && r[6] == 1);
    CHECK(r && n == 19 && r[14] == 0 && r[15] == BTS_RC_PREF_LAST && r[17] == (BTS_LATE_STOP_DEFAULT ? BTS_X_LATE_STOP : 0));   /* kept */
    sp[13] = 0;                                    /* strategy 0: the one way */
    r = op(2, sp, 14, &n);
    CHECK(r && r[1] == BT_OK && r[14] == 0);
    for (uint8_t st = 1; st <= 5; st++) {          /* the old strategies 1-4 and beyond: refused, nothing
                                                    * changes */
        uint8_t sq[14];
        memcpy(sq, sp, 14);
        sq[13] = st;
        sq[1] = BTS_MODE_SELECT;
        r = op(2, sq, 14, &n);
        CHECK(r && r[1] == BT_BAD_VALUE && r[2] == BTS_MODE_OFF && r[14] == 0);
    }
    {
        uint8_t raw5[BT_SRC_RAW];
        btc_sources(raw5);
        CHECK_EQ((raw5[5] >> 4) & 7, 1);           /* 1D 04 byte 5 bits 4-6: strategy + 1 = 1, for old apps */
    }
    {   /* the experimental options: byte 15, RAM; 0 by default, a shorter write keeps them */
        uint8_t xp[16];
        uint8_t raw6[BT_SRC_RAW];
        memcpy(xp, sp, 14);
        xp[13] = 0;
        xp[14] = BTS_RC_PREF_LAST;
        xp[15] = BTS_X_LATE_STOP;
        r = op(2, xp, 16, &n);
        CHECK(r && n == 19 && r[1] == BT_OK && r[17] == BTS_X_LATE_STOP);
        btc_sources(raw6);
        CHECK_EQ(raw6[116] & 0xC0, 0xC0);          /* bit 6 known, bit 7 late stop on */
        r = op(2, xp, 15, &n);                     /* 14 B args (no options byte): they stay */
        CHECK(r && r[1] == BT_OK && r[17] == BTS_X_LATE_STOP);
        xp[15] = 0x02;                             /* an unknown option: refused, nothing changes */
        r = op(2, xp, 16, &n);
        CHECK(r && r[1] == BT_BAD_VALUE && r[17] == BTS_X_LATE_STOP);
        xp[15] = 0;
        r = op(2, xp, 16, &n);
        CHECK(r && r[1] == BT_OK && r[17] == 0);
        btc_sources(raw6);
        CHECK_EQ(raw6[116] & 0xC0, 0x40);
    }
    {   /* the late stop's wait: byte 16, 100 ms units, RAM; BTS_XWAIT_DEFAULT at power-on (before: 0), 0 = that default, a shorter write keeps it, > BTS_XWAIT_MAX refused; reply byte 18, 1D 04 byte
         * 119 */
        uint8_t xw[17];
        uint8_t raw7[BT_SRC_RAW];
        memcpy(xw, sp, 14);
        xw[13] = 0;
        xw[14] = BTS_RC_PREF_LAST;
        xw[15] = BTS_X_LATE_STOP;
        xw[16] = 5;                                /* 0.5 s */
        btc_sources(raw7);
        CHECK_EQ(raw7[119], BTS_XWAIT_DEFAULT);
        r = op(2, xw, 17, &n);
        CHECK(r && n == 19 && r[1] == BT_OK && r[17] == BTS_X_LATE_STOP && r[18] == 5);
        btc_sources(raw7);
        CHECK_EQ(raw7[119], 5);
        CHECK_EQ(raw7[116] & 0xC0, 0xC0);
        r = op(2, xw, 16, &n);                     /* the old 16 B write (options, no wait): the wait stays */
        CHECK(r && r[1] == BT_OK && r[17] == BTS_X_LATE_STOP && r[18] == 5);
        r = op(2, xw, 13, &n);                     /* the oldest form: everything after the address stays */
        CHECK(r && r[1] == BT_OK && r[17] == BTS_X_LATE_STOP && r[18] == 5);
        xw[16] = BTS_XWAIT_MAX + 1;                /* 10.1 s: refused, nothing changes */
        r = op(2, xw, 17, &n);
        CHECK(r && r[1] == BT_BAD_VALUE && r[18] == 5);
        xw[16] = BTS_XWAIT_MAX;
        r = op(2, xw, 17, &n);
        CHECK(r && r[1] == BT_OK && r[18] == BTS_XWAIT_MAX);
        xw[15] = 0;
        xw[16] = 0;                                /* 0 (= the default wait) */
        r = op(2, xw, 17, &n);
        CHECK(r && r[1] == BT_OK && r[17] == 0 && r[18] == 0);
        btc_sources(raw7);
        CHECK_EQ(raw7[119], 0);
    }
    sp[13] = 0;
    sp[4] = 0;                                     /* hold 0: refused */
    r = op(2, sp, 13, &n);
    CHECK(r && r[1] == BT_BAD_VALUE && r[4] == 20);
    /* standby: no command (the power port not ON) */
    pwc_restart();
    module_line("RING 5 62:81:81:81:11:a1 19 A2DP");
    run_ms(300);                                   /* a read (`SET`) handed out just before still goes */
    int calls = bs.tx_calls;
    run_ms(2000);
    CHECK_EQ(bs.tx_calls, calls);
    CHECK_EQ(rc_of(a, 9), BT_NOT_ON);
}

/* who may change the BT audio (field 2026-11-01: the app bound in the core table, the panel holding the stock binding
 * 4, got rc 2 for 1D 05 01) - the session core's table when it runs, the stock binding without it */
static void test_bt_ctrl_bound(void)
{
    /* the field case: core table 0x0C, stock binding 4 (panel) - the app (port 2) is allowed */
    CHECK_EQ(btc_ctrl_bound(1, 4, 2), 1);
    CHECK_EQ(btc_ctrl_bound(1, 4, 3), 1);
    /* the core says not bound: refused (sxc_ctrl_bound already counts the stock binding as bound) */
    CHECK_EQ(btc_ctrl_bound(0, 3, 2), 0);
    CHECK_EQ(btc_ctrl_bound(0, 4, 0), 0);
    /* no session core (not built / not ready / 1B 01 0): the stock binding alone, as before */
    CHECK_EQ(btc_ctrl_bound(-1, 3, 2), 1);
    CHECK_EQ(btc_ctrl_bound(-1, 4, 2), 0);
    CHECK_EQ(btc_ctrl_bound(-1, 0, 2), 0);
    CHECK_EQ(btc_ctrl_bound(-1, 4, 3), 1);
    /* this host build has no session core: the cave falls back to the stock binding */
    CHECK_EQ(sxc_ctrl_bound(2), -1);
}

/* the record of the 1D 08 reply: count the records whose text is `want` (flags too when want_flags >= 0) */
static int log_has(const uint8_t *r, uint32_t n, const char *want, int want_flags)
{
    int hits = 0;
    for (uint32_t o = 10; o + 8 <= n;) {
        uint32_t len = r[o + 7];
        if (len == strlen(want) && memcmp(r + o + 8, want, len) == 0 && (want_flags < 0 || r[o + 6] == want_flags))
            hits++;
        o += 8 + len;
    }
    return hits;
}

/* the default (2026-11-02 requirement) through the cave: mode SELECT; the PC that streams first
 * is selected; 「이 기기로 듣기」 (1D 05 01) on the phone: AVRCP PAUSE to the PC, no STOP (Windows) -> the sink never
 * stops it (field 2026-11-10): no report 1 s after the PAUSE - registered again; its own STOP
 * -> AVRCP PLAY to the phone, nothing closed (field 2026-11-03: a CLOSE broke the Windows endpoint); 1D 04
 * layout 2 shows the selection and the switch result; 1D 08 reads the module lines and the commands */
static void test_bt_use_cave(void)
{
    static const uint8_t PHONE[6] = {0x62, 0x81, 0x81, 0x81, 0x11, 0xa1};
    uint8_t raw[BT_SRC_RAW];
    uint32_t n;
    boot();
    pwc_tick();
    run_ms(400);
    module_line("LIST");
    module_line("LIST 0");
    const uint8_t g[1] = {6};
    const uint8_t *r = op(2, g, 1, &n);
    CHECK(r && n == 19 && r[1] == BT_OK && r[2] == BTS_MODE_SELECT && r[3] == BTS_F_KEEP_PREF && r[14] == 0);
    CHECK(r && r[15] == BTS_RC_PREF_LAST && r[16] == BT_KV_KEPT);   /* default reconnect; kept on NOR, nothing stored */
    {   /* this replay waits for the PC's own STOP: the late stop off (on by default since 2026-11-13) */
        uint8_t w[16] = {6};
        for (int k = 1; k <= 14; k++)
            w[k] = r[k + 1];
        w[15] = 0;
        r = op(2, w, 16, &n);
        CHECK(r && r[1] == BT_OK && r[17] == 0 && r[18] == BTS_XWAIT_DEFAULT);
    }
    module_line("RING 3 22:82:02:02:92:22 19 A2DP");
    module_line("RING 4 22:82:02:02:92:22 19 A2DP");
    module_line("RING 5 22:82:02:02:92:22 17 AVRCP");
    module_line("A2DP STREAMING START 3");
    module_line("RING 0 62:81:81:81:11:a1 19 A2DP");
    module_line("RING 1 62:81:81:81:11:a1 19 A2DP");
    module_line("RING 2 62:81:81:81:11:a1 17 AVRCP");
    run_ms(900);
    /* the play status of both (RM 6.10.2 lines): the PC plays, the phone is paused */
    CHECK_EQ(bs.tx_calls, 5);
    CHECK_STR(bs.tx[1], "SET 5 SELECT\r\n");
    CHECK_STR(bs.tx[2], "AVRCP PDU 31 1\r\n");
    CHECK_STR(bs.tx[3], "SET 2 SELECT\r\n");
    CHECK_STR(bs.tx[4], "AVRCP PDU 31 1\r\n");
    module_line("AVRCP 5 REGISTER_NOTIFICATION_RSP INTERIM PLAYBACK_STATUS_CHANGED PLAYING");
    module_line("AVRCP 2 REGISTER_NOTIFICATION_RSP INTERIM PLAYBACK_STATUS_CHANGED PAUSED");
    run_ms(5100);
    CHECK_EQ(bs.tx_calls, 6);                      /* LIST, the registrations, then the AUTOCALL read */
    CHECK_STR(bs.tx[5], "SET\r\n");
    CHECK(src_list(raw) != NULL);
    CHECK_EQ(raw[0], BT_SRC_LAYOUT);
    CHECK_EQ(raw[84], 0);                          /* the PC (slot 0) adopted */
    CHECK_EQ(raw[28 + 12] & BTS_D2_SELECTED, BTS_D2_SELECTED);
    CHECK_EQ(raw[28 + 14 + 12] & (BTS_D2_SELECTED | BTS_D2_AVRCP), BTS_D2_AVRCP);
    CHECK_EQ(raw[95], 1);
    uint8_t a[9] = {5, 1};
    pack_addr(PHONE, a + 2);
    CHECK_EQ(rc_of(a, 9), BT_OK);
    CHECK_EQ(raw[112], BTS_PS_PLAYING | BTS_PSB_LIVE);   /* 1D 04 bytes 112..: the play status of slot 0 / 1 */
    CHECK_EQ(raw[113], BTS_PS_PAUSED | BTS_PSB_LIVE);
    run_ms(400);
    CHECK_EQ(bs.tx_calls, 8);
    CHECK_STR(bs.tx[6], "SET 5 SELECT\r\n");
    CHECK_STR(bs.tx[7], "AVRCP PAUSE\r\n");      /* it plays: PAUSE */
    CHECK(src_list(raw) != NULL);
    CHECK_EQ(raw[85], BTS_SW_PAUSE);
    CHECK_EQ(raw[84], 1);                          /* the phone is the selected one at once */
    run_ms(1200);                                  /* Windows keeps streaming after the pause: the sink never stops it
                                                    * (field 2026-11-10) - it waits */
    CHECK(src_list(raw) != NULL);
    CHECK_EQ(raw[85], BTS_SW_PAUSE);
    CHECK_EQ(bs.tx_calls, 10);                     /* no report 1 s after our PAUSE: the PC is asked again */
    CHECK_STR(bs.tx[8], "SET 5 SELECT\r\n");
    CHECK_STR(bs.tx[9], "AVRCP PDU 31 1\r\n");
    module_line("AVRCP 5 REGISTER_NOTIFICATION_RSP INTERIM PLAYBACK_STATUS_CHANGED PAUSED");
    run_ms(3000);
    module_line("A2DP STREAMING STOP 3");          /* it lets go by itself */
    run_ms(400);
    CHECK_EQ(bs.tx_calls, 12);
    CHECK_STR(bs.tx[10], "SET 2 SELECT\r\n");
    CHECK_STR(bs.tx[11], "AVRCP PLAY\r\n");      /* it reported paused: PLAY */
    module_line("A2DP CODEC SBC JOINT_STEREO 44100 BITPOOL 2-53");
    module_line("A2DP STREAMING START 0");
    run_ms(100);
    CHECK(src_list(raw) != NULL);
    CHECK_EQ(raw[85], BTS_SW_IDLE);
    CHECK_EQ(raw[86], BTS_RES_PLAYING);
    CHECK(raw[87] >= 45 && raw[87] <= 55);         /* ~4.9 s, 100 ms units (the PC's own STOP) */
    CHECK_EQ(raw[102], 0);                         /* module health: fine */
    CHECK_EQ(raw[106], BTS_SWT_PLAY);
    CHECK_EQ(raw[112], BTS_PS_PAUSED | BTS_PSB_LIVE | BTS_PSB_OURS);   /* we paused it */
    CHECK_EQ(raw[113], BTS_PS_PAUSED | BTS_PSB_LIVE);
    CHECK_EQ(raw[107], BTS_CODEC_SBC);
    CHECK_EQ(raw[108], 1);                         /* the phone's slot */
    CHECK_EQ(raw[109], 0);                         /* 44.1 kHz */
    CHECK_EQ(raw[28 + 12] & BTS_D2_AVRCP, BTS_D2_AVRCP);   /* the PC keeps its AVRCP */
    CHECK_EQ(bs.max_len, 1);
    CHECK_EQ(btc_a2dp_links(), 0x1B);              /* the phone's A2DP and the PC's - nothing closed */
    /* act out of range; a device without AVRCP gets a switch that ends "ready" */
    a[1] = 7;
    CHECK_EQ(rc_of(a, 9), BT_BAD_VALUE);
    /* 1D 08: from 0 - the first records; from a sequence number on */
    const uint8_t l0[3] = {8, 0, 0};
    r = op(2, l0, 3, &n);
    CHECK(r && r[0] == 8 && r[1] == BT_OK && n > 10);
    CHECK_EQ(log_has(r, n, "module on", 4), 1);
    CHECK_EQ(log_has(r, n, "LIST", 1), 1);         /* the first LIST, sent */
    uint16_t next = (uint16_t)(r[4] << 7 | r[5]);
    CHECK(next >= 15);
    uint8_t lf[3] = {8, (uint8_t)((next - 4) >> 7), (uint8_t)((next - 4) & 0x7F)};
    r = op(2, lf, 3, &n);
    CHECK(r && r[1] == BT_OK && n <= 10 + BT_LOG_CHUNK);
    CHECK_EQ(log_has(r, n, "A2DP CODEC SBC JOINT_STEREO 44100 BITPOOL 2-53", 0), 1);
    CHECK_EQ(log_has(r, n, "AVRCP PLAY", 1), 1);
}

/* 1D 09: AUTOCALL - state without a command; read = `SET`; off refused before the read, from a port that is not
 * bound, in standby; off = `SET CONTROL AUTOCALL` then a read back; restore = the line read before.  Nothing of it
 * without the op (the other tests: no `SET CONTROL` at all). */
static const uint8_t *ac_op(uint8_t o, uint32_t *n)
{
    const uint8_t a[2] = {9, o};
    return op(2, a, 2, n);
}

static void module_listing(bool autocall)
{
    module_line("SET");
    module_line("SET BT BDADDR 02:07:80:aa:bb:cc");
    module_line("SET BT NAME CA79 Audio");
    module_line("SET BT PAIR 62:81:81:81:11:a1 00112233445566778899aabbccddeeff");
    if (autocall)
        module_line("SET CONTROL AUTOCALL 19 5000 A2DP");
    module_line("SET");
}

static void test_bt_autocall_cave(void)
{
    uint32_t n;
    boot();
    pwc_tick();
    run_ms(400);
    module_line("LIST 0");
    const uint8_t *r = ac_op(0, &n);
    CHECK(r && r[1] == BT_OK && r[3] == BTS_AC_UNKNOWN && r[6] == 0);
    CHECK(r && r[7] == 33 && n == 8u + 33u && memcmp(r + 8, "SET CONTROL AUTOCALL 19 5000 A2DP", 33) == 0);
    r = ac_op(2, &n);
    CHECK(r && r[1] == BT_BAD_STATE);              /* not read yet */
    r = ac_op(1, &n);
    CHECK(r && r[1] == BT_OK && r[4] == BTS_AC_READ);
    run_ms(400);
    CHECK_EQ(bs.tx_calls, 2);
    CHECK_STR(bs.tx[1], "SET\r\n");
    module_listing(true);
    run_ms(16);
    r = ac_op(0, &n);
    CHECK(r && r[3] == BTS_AC_ON && r[4] == 0 && r[5] == BTS_OK && r[6] == 1);
    stk_binding = 4;                               /* another port holds the binding */
    r = ac_op(2, &n);
    CHECK(r && r[1] == BT_NOT_BOUND);
    stk_binding = 3;
    r = ac_op(2, &n);
    CHECK(r && r[1] == BT_OK);
    run_ms(400);
    CHECK_EQ(bs.tx_calls, 4);
    CHECK_STR(bs.tx[2], "SET CONTROL AUTOCALL\r\n");
    CHECK_STR(bs.tx[3], "SET\r\n");
    module_listing(false);
    run_ms(16);
    r = ac_op(0, &n);
    CHECK(r && r[3] == BTS_AC_OFF && r[5] == BTS_OK);
    r = ac_op(3, &n);
    CHECK(r && r[1] == BT_OK);
    run_ms(400);
    CHECK_EQ(bs.tx_calls, 6);
    CHECK_STR(bs.tx[4], "SET CONTROL AUTOCALL 19 5000 A2DP\r\n");
    module_listing(true);
    run_ms(16);
    r = ac_op(0, &n);
    CHECK(r && r[3] == BTS_AC_ON && r[5] == BTS_OK);
    r = ac_op(4, &n);
    CHECK(r && r[1] == BT_BAD_VALUE);
    /* the pairing key never reaches the line log */
    const uint8_t l0[3] = {8, 0, 0};
    for (uint16_t from = 0; from < 40; from++) {
        uint8_t lf[3] = {8, (uint8_t)(from >> 7), (uint8_t)(from & 0x7F)};
        r = op(2, from ? lf : l0, 3, &n);
        for (uint32_t i = 0; r && i + 8 <= n; i++)
            CHECK(memcmp(r + i, "00112233", 8) != 0);
    }
    /* standby: refused */
    pwc_restart();
    r = ac_op(1, &n);
    CHECK(r && r[1] == BT_NOT_ON);
}

/* 1D 0A: class of device (field 2026-11-09, an Android TV did not list the class-001f00 sink) - state without a command;
 * read = `SET`; write refused before the read, from a port that is not bound, with a class that is not an audio sink
 * one, while the 1D 09 read-back is pending, in standby; write = `SET BT CLASS 240414` then a read back; restore = the
 * class read before.  Nothing of it without the op (the other tests: no `SET BT CLASS` at all). */
static const uint8_t *co_op(uint8_t o, uint32_t cls, uint32_t *n)
{
    const uint8_t a[6] = {0x0A, o, (uint8_t)((cls >> 21) & 0x7F), (uint8_t)((cls >> 14) & 0x7F), (uint8_t)((cls >> 7) & 0x7F),
                          (uint8_t)(cls & 0x7F)};
    return op(2, a, cls ? 6 : 2, n);
}

static uint32_t co_val(const uint8_t *r) { return (uint32_t)r[0] << 21 | (uint32_t)r[1] << 14 | (uint32_t)r[2] << 7 | r[3]; }

static void class_listing(const char *cls)
{
    module_line("SET");
    module_line("SET BT BDADDR 02:07:80:aa:bb:cc");
    module_line("SET BT NAME CA79 Audio");
    module_line(cls);
    module_line("SET BT PAGEMODE 3 2000 1");
    module_line("SET CONTROL AUTOCALL 19 5000 A2DP");
    module_line("SET");
}

static void test_bt_class_cave(void)
{
    uint32_t n;
    boot();
    pwc_tick();
    run_ms(400);
    module_line("LIST 0");
    const uint8_t *r = co_op(0, 0, &n);
    CHECK(r && n == 19 && r[0] == 0x0A && r[1] == BT_OK && r[3] == BTS_COD_UNKNOWN && r[6] == 0);
    CHECK(r && co_val(r + 11) == 0x001F00u && co_val(r + 15) == 0x240414u);
    r = co_op(2, 0, &n);
    CHECK(r && r[1] == BT_BAD_STATE);              /* not read yet */
    r = co_op(2, 0x001F00u, &n);
    CHECK(r && r[1] == BT_BAD_VALUE);              /* not an audio sink class */
    r = co_op(3, 0x240414u, &n);
    CHECK(r && r[1] == BT_BAD_VALUE);              /* a class only with the write */
    r = co_op(4, 0, &n);
    CHECK(r && r[1] == BT_BAD_VALUE);
    r = co_op(1, 0, &n);
    CHECK(r && r[1] == BT_OK && r[4] == BTS_CO_READ);
    r = ac_op(1, &n);
    CHECK(r && r[1] == BT_BUSY);                   /* one module-setting op at a time */
    run_ms(400);
    CHECK_EQ(bs.tx_calls, 2);
    CHECK_STR(bs.tx[1], "SET\r\n");
    class_listing("SET BT CLASS 001f00");
    run_ms(16);
    r = co_op(0, 0, &n);
    CHECK(r && r[3] == BTS_COD_KNOWN && r[4] == 0 && r[5] == BTS_OK && r[6] == 1 && co_val(r + 7) == 0x001F00u);
    stk_binding = 4;                               /* another port holds the binding */
    r = co_op(2, 0, &n);
    CHECK(r && r[1] == BT_NOT_BOUND);
    stk_binding = 3;
    r = co_op(3, 0, &n);
    CHECK(r && r[1] == BT_BAD_STATE);              /* already the class read before */
    r = co_op(2, 0, &n);
    CHECK(r && r[1] == BT_OK && r[4] == BTS_CO_WRITE);
    run_ms(400);
    CHECK_EQ(bs.tx_calls, 4);
    CHECK_STR(bs.tx[2], "SET BT CLASS 240414\r\n");
    CHECK_STR(bs.tx[3], "SET\r\n");
    class_listing("SET BT CLASS 240414");
    run_ms(16);
    r = co_op(0, 0, &n);
    CHECK(r && r[3] == BTS_COD_KNOWN && r[4] == 0 && r[5] == BTS_OK && co_val(r + 7) == 0x240414u &&
          co_val(r + 11) == 0x001F00u);
    r = co_op(2, 0, &n);
    CHECK(r && r[1] == BT_BAD_STATE);              /* already that class */
    r = co_op(2, 0x240418u, &n);                   /* another audio sink class, given */
    CHECK(r && r[1] == BT_OK && co_val(r + 15) == 0x240418u);
    run_ms(400);
    CHECK_STR(bs.tx[4], "SET BT CLASS 240418\r\n");
    class_listing("SET BT CLASS 240418");
    run_ms(16);
    r = co_op(3, 0, &n);
    CHECK(r && r[1] == BT_OK);
    run_ms(400);
    CHECK_EQ(bs.tx_calls, 8);
    CHECK_STR(bs.tx[6], "SET BT CLASS 001f00\r\n");
    CHECK_STR(bs.tx[7], "SET\r\n");
    class_listing("SET BT CLASS 001f00");
    run_ms(16);
    r = co_op(0, 0, &n);
    CHECK(r && co_val(r + 7) == 0x001F00u && r[5] == BTS_OK);
    /* standby: refused */
    pwc_restart();
    r = co_op(1, 0, &n);
    CHECK(r && r[1] == BT_NOT_ON);
}

/* 1D 0B on any port: unpacked raw, false if no reply */
static bool level(uint8_t port, uint8_t raw[BT_LVL_RAW])
{
    uint32_t n;
    const uint8_t a[1] = {0x0B};
    const uint8_t *r = op(port, a, 1, &n);
    if (!r || n < 2 || r[0] != 0x0B || r[1] != BT_OK)
        return false;
    size_t o = 0;
    for (size_t i = 2; i < n && o < BT_LVL_RAW;) {
        uint8_t hi = r[i++];
        for (size_t k = 0; k < 7 && i < n && o < BT_LVL_RAW; k++, i++)
            raw[o++] = (uint8_t)(r[i] | (((hi >> k) & 1u) << 7));
    }
    return o == BT_LVL_RAW;
}

static unsigned lv16(const uint8_t *raw, int o) { return (unsigned)raw[o] | (unsigned)raw[o + 1] << 8; }

static void set_sample(unsigned i, int16_t v)
{
    uint32_t w = stk_dsp_rec_buf[i / 2];
    w = i & 1 ? (w & 0xFFFFu) | (uint32_t)(uint16_t)v << 16 : (w & 0xFFFF0000u) | (uint16_t)v;
    stk_dsp_rec_buf[i / 2] = w;
}

/* 1D 0B: the DSP record-bus capture - peaks per slot, mean, samples over the floor, the gain, the
 * write position; MOVED only when the DSP's position changed since the previous scan; read only (the buffers and the
 * DSP words unchanged), any port (not only the bound one), answered in standby too */
static void test_bt_level(void)
{
    uint8_t raw[BT_LVL_RAW];
    uint32_t base = (uint32_t)(uintptr_t)stk_dsp_rec_buf;
    boot();
    memset((void *)stk_dsp_rec_buf, 0, sizeof stk_dsp_rec_buf);
    stk_dsp_rec_wp = base + 0x100;
    stk_dsp_rec_n = 0x80;
    stk_dsp_rec_state = 0;
    stk_dsp_rec_gain = 0x2D;
    /* silence: all zero, first scan */
    CHECK(level(1, raw));                       /* port 1 is not the bound one (binding 3 = port 2): still answered */
    CHECK_EQ(raw[0], BT_LVL_LAYOUT);
    CHECK_EQ(raw[1], BT_LVL_F_FIRST);
    CHECK(lv16(raw, 2) == 0 && lv16(raw, 4) == 0 && lv16(raw, 6) == 0 && lv16(raw, 8) == 0);
    CHECK_EQ(lv16(raw, 10), 0x2D);
    CHECK_EQ(lv16(raw, 12), 0x100);
    CHECK_EQ(lv16(raw, 14), 0x80);
    /* the DSP did not move: not MOVED */
    CHECK(level(2, raw));
    CHECK_EQ(raw[1], 0);
    /* noise under the floor (LINE IN ADC [inferred]): peak seen, nothing counted */
    for (unsigned i = 0; i < BT_LVL_SAMPLES; i++)
        set_sample(i, (int16_t)((i % 3) - 1) * 9);   /* -9, 0, 9 */
    stk_dsp_rec_n = 0x100;
    CHECK(level(2, raw));
    CHECK_EQ(raw[1], BT_LVL_F_MOVED);
    CHECK(lv16(raw, 2) == 9 && lv16(raw, 4) == 9 && lv16(raw, 8) == 0);
    /* a signal on slot 0 (L) only, one sample at -32768: peak 0x7FFF, R untouched */
    for (unsigned i = 0; i < BT_LVL_SAMPLES; i++)
        set_sample(i, i & 1 ? 0 : (int16_t)(i % 8 == 0 ? 1000 : i % 8 == 4 ? -1000 : 0));
    set_sample(10, -32768);
    stk_dsp_rec_wp = base + 0x900 + 0x10;       /* buffer 1 */
    CHECK(level(2, raw));
    CHECK_EQ(raw[1], BT_LVL_F_MOVED);
    CHECK_EQ(lv16(raw, 2), 0x7FFF);
    CHECK_EQ(lv16(raw, 4), 0);
    CHECK_EQ(lv16(raw, 8), BT_LVL_SAMPLES / 4u + 1u);   /* every 4th sample (1000 / -1000) + the -32768 one */
    CHECK_EQ(lv16(raw, 6), (unsigned)((576u * 1000u + 32767u) / BT_LVL_SAMPLES));
    CHECK_EQ(lv16(raw, 12), 0x910);
    /* read only: the capture is as it was */
    uint32_t copy[0x480];
    memcpy(copy, (const void *)stk_dsp_rec_buf, sizeof copy);
    CHECK(level(2, raw));
    CHECK(memcmp(copy, (const void *)stk_dsp_rec_buf, sizeof copy) == 0 && stk_dsp_rec_wp == base + 0x910 && stk_dsp_rec_gain == 0x2D);
    /* recording; a write pointer outside the buffers (a DSP not running this code) */
    stk_dsp_rec_state = 1;
    stk_dsp_rec_wp = 0xFFFFFFFFu;
    CHECK(level(2, raw));
    CHECK_EQ(raw[1], BT_LVL_F_MOVED | BT_LVL_F_REC | BT_LVL_F_WP_ODD);
    CHECK_EQ(lv16(raw, 12), 0xFFFF);
    /* standby (the power port off): still answered - nothing is sent anywhere */
    pwc_restart();
    CHECK(level(2, raw));
    /* sent nothing to the module */
    CHECK_EQ(bs.tx_calls, 0);
}

/* 1D 0C: AVRCP controller features (field 2026-11-10, a Windows PC's lone browsing channel rebooted the module) - state
 * without a command; read = `SET`; write refused before the read, from a port that is not bound, with the browsing bit,
 * while the 1D 0A read-back is pending, in standby; write = `SET PROFILE AVRCP CONTROLLER 0f` then a read back; restore
 * = the features read before.  Nothing of it without the op (the other tests: no `SET PROFILE` at all). */
static const uint8_t *av_op(uint8_t o, int f, uint32_t *n)
{
    const uint8_t a[3] = {0x0C, o, (uint8_t)(f & 0x7F)};
    return op(2, a, f >= 0 ? 3 : 2, n);
}

static void avrcp_listing(const char *av)
{
    module_line("SET");
    module_line("SET BT BDADDR 02:07:80:aa:bb:cc");
    module_line("SET BT NAME CA79 Audio");
    module_line("SET BT CLASS 240414");
    module_line("SET BT PAGEMODE 3 2000 1");
    module_line("SET PROFILE A2DP SINK");
    module_line(av);
    module_line("SET");
}

static void test_bt_avrcp_cave(void)
{
    uint32_t n;
    boot();
    pwc_tick();
    run_ms(400);
    module_line("LIST 0");
    const uint8_t *r = av_op(0, -1, &n);
    CHECK(r && n == 13 && r[0] == 0x0C && r[1] == BT_OK && r[3] == BTS_AVF_UNKNOWN && r[6] == 0);
    CHECK(r && (r[9] << 7 | r[10]) == 0x1F && (r[11] << 7 | r[12]) == 0x0F);
    r = av_op(2, -1, &n);
    CHECK(r && r[1] == BT_BAD_STATE);              /* not read yet */
    r = av_op(2, 0x1F, &n);
    CHECK(r && r[1] == BT_BAD_VALUE);              /* the browsing bit */
    r = av_op(2, 0x00, &n);
    CHECK(r && r[1] == BT_BAD_VALUE);              /* no category */
    r = av_op(3, 0x0F, &n);
    CHECK(r && r[1] == BT_BAD_VALUE);              /* features only with the write */
    r = av_op(4, -1, &n);
    CHECK(r && r[1] == BT_BAD_VALUE);
    r = av_op(1, -1, &n);
    CHECK(r && r[1] == BT_OK && r[4] == BTS_AV_READ);
    r = co_op(1, 0, &n);
    CHECK(r && r[1] == BT_BUSY);                   /* one module-setting op at a time */
    run_ms(400);
    CHECK_EQ(bs.tx_calls, 2);
    CHECK_STR(bs.tx[1], "SET\r\n");
    avrcp_listing("SET PROFILE AVRCP CONTROLLER 1f");
    run_ms(16);
    r = av_op(0, -1, &n);
    CHECK(r && r[3] == BTS_AVF_CTRL && r[4] == 0 && r[5] == BTS_OK && r[6] == 1 && (r[7] << 7 | r[8]) == 0x1F);
    stk_binding = 4;                               /* another port holds the binding */
    r = av_op(2, -1, &n);
    CHECK(r && r[1] == BT_NOT_BOUND);
    stk_binding = 3;
    r = av_op(3, -1, &n);
    CHECK(r && r[1] == BT_BAD_STATE);              /* already the features read before */
    r = av_op(2, -1, &n);
    CHECK(r && r[1] == BT_OK && r[4] == BTS_AV_WRITE);
    run_ms(400);
    CHECK_EQ(bs.tx_calls, 4);
    CHECK_STR(bs.tx[2], "SET PROFILE AVRCP CONTROLLER 0f\r\n");
    CHECK_STR(bs.tx[3], "SET\r\n");
    avrcp_listing("SET PROFILE AVRCP CONTROLLER 0f");
    run_ms(16);
    r = av_op(0, -1, &n);
    CHECK(r && r[3] == BTS_AVF_CTRL && r[4] == 0 && r[5] == BTS_OK && (r[7] << 7 | r[8]) == 0x0F &&
          (r[9] << 7 | r[10]) == 0x1F);
    r = av_op(2, -1, &n);
    CHECK(r && r[1] == BT_BAD_STATE);              /* already that */
    r = av_op(3, -1, &n);
    CHECK(r && r[1] == BT_OK);
    run_ms(400);
    CHECK_EQ(bs.tx_calls, 6);
    CHECK_STR(bs.tx[4], "SET PROFILE AVRCP CONTROLLER 1f\r\n");
    CHECK_STR(bs.tx[5], "SET\r\n");
    avrcp_listing("SET PROFILE AVRCP CONTROLLER 1f");
    run_ms(16);
    r = av_op(0, -1, &n);
    CHECK(r && (r[7] << 7 | r[8]) == 0x1F && r[5] == BTS_OK);
    /* a TARGET part: never rewritten */
    r = av_op(1, -1, &n);
    run_ms(400);
    avrcp_listing("SET PROFILE AVRCP CONTROLLER 1f TARGET 3");
    run_ms(16);
    r = av_op(2, -1, &n);
    CHECK(r && r[1] == BT_BAD_STATE && r[3] == BTS_AVF_OTHER);
    /* standby: refused */
    pwc_restart();
    r = av_op(1, -1, &n);
    CHECK(r && r[1] == BT_NOT_ON);
}

/* 1D 0D: audio routing (field 2026-11-11, phone (aptX) -> PC silent) - state without a
 * command; read = `SET`; write refused before the read, from a port that is not bound, with flags other than the two
 * fixed sets, while the 1D 0C read-back is pending, in standby; write = `SET CONTROL AUDIO INTERNAL I2S_SLAVE EVENT`
 * (default) / `… EVENT KEEPALIVE` then a read back; restore = the line read before; the EVENT lines change nothing.
 * Nothing of it without the op (the other tests: no `SET CONTROL AUDIO` at all). */
static const uint8_t *au_op(uint8_t o, int f, uint32_t *n)
{
    const uint8_t a[3] = {0x0D, o, (uint8_t)(f & 0x7F)};
    return op(2, a, f >= 0 ? 3 : 2, n);
}

static void audio_listing(const char *au)
{
    module_line("SET");
    module_line("SET BT BDADDR 02:07:80:aa:bb:cc");
    module_line("SET BT NAME CA79 Audio");
    module_line("SET BT CLASS 240414");
    module_line("SET BT PAGEMODE 3 2000 1");
    module_line(au);
    module_line("SET PROFILE A2DP SINK");
    module_line("SET PROFILE AVRCP CONTROLLER 1f");
    module_line("SET");
}

static void test_bt_audio_cave(void)
{
    uint32_t n;
    boot();
    pwc_tick();
    run_ms(400);
    module_line("LIST 0");
    const uint8_t *r = au_op(0, -1, &n);
    CHECK(r && n == 12 && r[0] == 0x0D && r[1] == BT_OK && r[3] == BTS_AUD_UNKNOWN && r[6] == 0);
    CHECK(r && r[10] == 0 && r[11] == 0x7F);       /* no route event yet */
    CHECK(r && r[8] == BTS_AUD_FACTORY && r[9] == BTS_AUD_EVENT);
    r = au_op(2, -1, &n);
    CHECK(r && r[1] == BT_BAD_STATE);              /* not read yet */
    r = au_op(2, BTS_AUD_KEEP, &n);
    CHECK(r && r[1] == BT_BAD_VALUE);              /* only the two fixed EVENT sets */
    r = au_op(2, 0, &n);
    CHECK(r && r[1] == BT_BAD_VALUE);
    r = au_op(2, 7, &n);
    CHECK(r && r[1] == BT_BAD_VALUE);
    r = au_op(3, BTS_AUD_EVENT, &n);
    CHECK(r && r[1] == BT_BAD_VALUE);              /* flags only with the write */
    r = au_op(4, -1, &n);
    CHECK(r && r[1] == BT_BAD_VALUE);
    r = au_op(1, -1, &n);
    CHECK(r && r[1] == BT_OK && r[4] == BTS_AU_READ);
    r = av_op(1, -1, &n);
    CHECK(r && r[1] == BT_BUSY);                   /* one module-setting op at a time */
    run_ms(400);
    CHECK_EQ(bs.tx_calls, 2);
    CHECK_STR(bs.tx[1], "SET\r\n");
    audio_listing("SET CONTROL AUDIO INTERNAL I2S_SLAVE KEEPALIVE");
    run_ms(16);
    r = au_op(0, -1, &n);
    CHECK(r && r[3] == BTS_AUD_STD && r[4] == 0 && r[5] == BTS_OK && r[6] == 1 && r[7] == BTS_AUD_KEEP &&
          r[8] == BTS_AUD_KEEP);
    stk_binding = 4;                               /* another port holds the binding */
    r = au_op(2, -1, &n);
    CHECK(r && r[1] == BT_NOT_BOUND);
    stk_binding = 3;
    r = au_op(3, -1, &n);
    CHECK(r && r[1] == BT_BAD_STATE);              /* already the line read before */
    r = au_op(2, -1, &n);
    CHECK(r && r[1] == BT_OK && r[4] == BTS_AU_WRITE);
    run_ms(400);
    CHECK_EQ(bs.tx_calls, 4);
    CHECK_STR(bs.tx[2], "SET CONTROL AUDIO INTERNAL I2S_SLAVE EVENT\r\n");
    CHECK_STR(bs.tx[3], "SET\r\n");
    audio_listing("SET CONTROL AUDIO INTERNAL I2S_SLAVE EVENT");
    run_ms(16);
    r = au_op(0, -1, &n);
    CHECK(r && r[3] == BTS_AUD_STD && r[4] == 0 && r[5] == BTS_OK && r[7] == BTS_AUD_EVENT && r[8] == BTS_AUD_KEEP &&
          r[9] == BTS_AUD_EVENT);
    r = au_op(2, -1, &n);
    CHECK(r && r[1] == BT_BAD_STATE);              /* already that */
    r = au_op(2, BTS_AUD_EVENT | BTS_AUD_KEEP, &n);
    CHECK(r && r[1] == BT_OK);
    run_ms(400);
    CHECK_EQ(bs.tx_calls, 6);
    CHECK_STR(bs.tx[4], "SET CONTROL AUDIO INTERNAL I2S_SLAVE EVENT KEEPALIVE\r\n");
    audio_listing("SET CONTROL AUDIO INTERNAL I2S_SLAVE EVENT KEEPALIVE");
    run_ms(16);
    module_line("A2DP CODEC APT-X JOINT_STEREO 44100");   /* EVENT lines with no stream: nothing goes out */
    module_line("AUDIO ROUTE 0 A2DP LEFT RIGHT");
    run_ms(400);
    CHECK_EQ(bs.tx_calls, 6);
    r = au_op(0, -1, &n);
    CHECK(r && r[7] == (BTS_AUD_EVENT | BTS_AUD_KEEP) && r[9] == (BTS_AUD_EVENT | BTS_AUD_KEEP));
    CHECK(r && r[10] == (0x40 | 1) && r[11] == 0x7F); /* link 0, A2DP - no device on it (LIST 0: none) */
    r = au_op(3, -1, &n);
    CHECK(r && r[1] == BT_OK);
    run_ms(400);
    CHECK_EQ(bs.tx_calls, 8);
    CHECK_STR(bs.tx[6], "SET CONTROL AUDIO INTERNAL I2S_SLAVE KEEPALIVE\r\n");
    CHECK_STR(bs.tx[7], "SET\r\n");
    audio_listing("SET CONTROL AUDIO INTERNAL I2S_SLAVE KEEPALIVE");
    run_ms(16);
    r = au_op(0, -1, &n);
    CHECK(r && r[7] == BTS_AUD_KEEP && r[5] == BTS_OK);
    /* another form: never rewritten */
    r = au_op(1, -1, &n);
    run_ms(400);
    audio_listing("SET CONTROL AUDIO INTERNAL I2S_SLAVE EVENT 32");
    run_ms(16);
    r = au_op(2, -1, &n);
    CHECK(r && r[1] == BT_BAD_STATE && r[3] == BTS_AUD_OTHER);
    /* standby: refused */
    pwc_restart();
    r = au_op(1, -1, &n);
    CHECK(r && r[1] == BT_NOT_ON);
}

/* the piano's own reconnect choice (1D 06 byte 14, user decision 2026-11-09) and the policy's NOR part: written once a
 * change is made and no stock writer owns the chip, read back at the next power-on; an older app's 13 / 14 B write
 * keeps it; a value out of range is refused; the same values again write nothing */
static void test_bt_policy_nor(void)
{
    static const uint8_t PC[6] = {0x22, 0x82, 0x02, 0x02, 0x92, 0x22};
    uint8_t raw[BT_SRC_RAW];
    uint32_t n;
    const uint8_t g[1] = {6};
    const uint8_t *r;
    k_keep = 0;
    boot();
    pwc_tick();
    run_ms(100);
    r = op(2, g, 1, &n);
    CHECK(r && n == 19 && r[15] == BTS_RC_PREF_LAST && r[16] == BT_KV_KEPT);
    CHECK(src_list(raw) != NULL);
    CHECK_EQ(raw[116], (BTS_RC_PREF_LAST + 1) | BT_KV_KEPT << 2 | 0x40 | (BTS_LATE_STOP_DEFAULT ? 0x80 : 0));
    CHECK_EQ(k_puts, 0);                           /* nothing written at power-on */
    /* preferred only + the PC preferred, while a stock writer owns the chip: pending */
    uint8_t sp[15] = {6, BTS_MODE_SELECT, BTS_F_KEEP_PREF, 15, 5, 1};
    pack_addr(PC, sp + 6);
    sp[13] = 0;
    sp[14] = BTS_RC_PREF;
    k_idle = 0;
    r = op(2, sp, 15, &n);
    CHECK(r && r[1] == BT_OK && r[15] == BTS_RC_PREF && (r[16] & BT_KV_PENDING));
    run_ms(200);
    CHECK_EQ(k_puts, 0);
    k_idle = 1;
    run_ms(16);
    CHECK(k_puts > 0);
    r = op(2, g, 1, &n);
    CHECK(r && r[16] == BT_KV_KEPT);
    int puts = k_puts;
    r = op(2, sp, 15, &n);                         /* the same again: no write */
    run_ms(50);
    CHECK_EQ(k_puts, puts);
    sp[14] = BTS_RC_MAX + 1;
    r = op(2, sp, 15, &n);
    CHECK(r && r[1] == BT_BAD_VALUE && r[15] == BTS_RC_PREF);
    sp[14] = BTS_RC_PREF;
    r = op(2, sp, 13, &n);                         /* an older app: 13 B - the choice stays */
    CHECK(r && r[1] == BT_OK && r[15] == BTS_RC_PREF);
    /* power cycle on the same store: read back */
    k_keep = 1;
    k_late = 1;                                    /* the store probe has not answered yet: defaults */
    boot();
    k_late = 0;
    run_ms(50);
    r = op(2, g, 1, &n);
    CHECK(r && r[15] == BTS_RC_PREF_LAST && r[6] == 0);
    k_known = 1;
    run_ms(16);
    r = op(2, g, 1, &n);
    CHECK(r && r[15] == BTS_RC_PREF && r[6] == 1 && r[16] == (BT_KV_KEPT | BT_KV_LOADED));
    CHECK(r && r[2] == BTS_MODE_SELECT && r[14] == 0);
    {
        uint8_t back[6] = {(uint8_t)(r[8] | ((r[7] & 1) << 7)), (uint8_t)(r[9] | ((r[7] >> 1 & 1) << 7)),
                           (uint8_t)(r[10] | ((r[7] >> 2 & 1) << 7)), (uint8_t)(r[11] | ((r[7] >> 3 & 1) << 7)),
                           (uint8_t)(r[12] | ((r[7] >> 4 & 1) << 7)), (uint8_t)(r[13] | ((r[7] >> 5 & 1) << 7))};
        CHECK(memcmp(back, PC, 6) == 0);
    }
    /* off: kept too; the preferred device removed */
    sp[5] = 0;
    sp[14] = BTS_RC_OFF;
    r = op(2, sp, 15, &n);
    CHECK(r && r[1] == BT_OK && r[15] == BTS_RC_OFF && r[6] == 0);
    run_ms(16);
    boot();
    run_ms(16);
    r = op(2, g, 1, &n);
    CHECK(r && r[15] == BTS_RC_OFF && r[6] == 0 && (r[16] & BT_KV_LOADED));
    k_keep = 0;
}

/* at a power-on the kept part starts from what the startup settings say - the factory
 * defaults (mode stock), the snapshot's part (current / power-off) or the live value on NOR; nothing before the startup
 * settings are loaded; a changed start value becomes the live value on NOR; a user change (and only a change) starts the
 * power-off snapshot's debounce */
static void test_bt_ng_startup(void)
{
    static const uint8_t PC[6] = {0x22, 0x82, 0x02, 0x02, 0x92, 0x22}, PHONE[6] = {0x62, 0x81, 0x81, 0x81, 0x11, 0xa1};
    uint32_t n;
    const uint8_t g[1] = {6};
    const uint8_t *r;
    uint8_t sp[15] = {6, BTS_MODE_SELECT, BTS_F_KEEP_PREF | BTS_F_FOLLOW, 15, 5, 1};
    int puts;
    /* the live value on NOR: off, the PC preferred, follow on */
    k_keep = 0;
    s_ng = 0;
    boot();
    run_ms(50);
    pack_addr(PC, sp + 6);
    sp[13] = 0;
    sp[14] = BTS_RC_OFF;
    s_ng_changes = 0;
    r = op(2, sp, 15, &n);
    CHECK(r && r[1] == BT_OK && r[15] == BTS_RC_OFF && (r[3] & BTS_F_FOLLOW));
    CHECK_EQ(s_ng_changes, 1);
    r = op(2, sp, 15, &n);                         /* the same again: not a change */
    CHECK_EQ(s_ng_changes, 1);
    run_ms(50);
    CHECK(k_puts > 0);
    /* power cycle, mode stock: the factory defaults, written back as the live value */
    k_keep = 1;
    s_ng = -1;                                     /* the startup settings not loaded yet: the NOR value meanwhile */
    s_ng_applied = 0;
    boot();
    run_ms(100);
    r = op(2, g, 1, &n);
    CHECK(r && r[15] == BTS_RC_OFF && r[6] == 1 && (r[3] & BTS_F_FOLLOW));
    CHECK_EQ(k_puts, 0);
    s_ng = 1;
    run_ms(50);
    r = op(2, g, 1, &n);
    CHECK(r && r[15] == BTS_RC_PREF_LAST && r[6] == 0 && !(r[3] & BTS_F_FOLLOW) && r[2] == BTS_MODE_SELECT);
    CHECK_EQ(s_ng_applied, 0x01);
    CHECK(k_puts > 0);                             /* the live value follows */
    CHECK_EQ(s_ng_changes, 1);                     /* the start value is not a user change */
    /* power cycle, mode current / power-off: the snapshot's part (preferred only, the phone, follow off) */
    s_ng = 2;
    s_ng_applied = 0;
    memset(s_ng_bt, 0, sizeof s_ng_bt);
    s_ng_bt[0] = 1;
    s_ng_bt[1] = BTS_RC_PREF;
    s_ng_bt[2] = 1;
    memcpy(s_ng_bt + 3, PHONE, 6);
    s_ng_bt[9] = 0x01;                             /* follow on */
    boot();
    run_ms(100);
    r = op(2, g, 1, &n);
    CHECK(r && r[15] == BTS_RC_PREF && r[6] == 1 && (r[3] & BTS_F_FOLLOW));
    CHECK_EQ(s_ng_applied, 0x01);
    puts = k_puts;
    CHECK(puts > 0);
    /* the same again at the next power-on: nothing to change, nothing written */
    s_ng_applied = 0;
    boot();
    run_ms(100);
    CHECK_EQ(s_ng_applied, 0);
    CHECK_EQ(k_puts, 0);
    /* keep (a live apply / a snapshot of a build before): the NOR value as it is */
    s_ng = 0;
    boot();
    run_ms(100);
    r = op(2, g, 1, &n);
    CHECK(r && r[15] == BTS_RC_PREF && (r[3] & BTS_F_FOLLOW) && (r[16] & BT_KV_LOADED));
    CHECK_EQ(s_ng_applied, 0);
    /* a bad saved part: the live value stays */
    s_ng = 2;
    s_ng_bt[1] = 9;
    boot();
    run_ms(100);
    r = op(2, g, 1, &n);
    CHECK(r && r[15] == BTS_RC_PREF);
    CHECK_EQ(s_ng_applied, 0);
    {   /* the kept part the startup snapshot takes */
        uint8_t raw[BTS_SAVED_RAW];
        btc_ng_get(raw);
        CHECK(raw[0] == 1 && raw[1] == BTS_RC_PREF && raw[2] == 1 && memcmp(raw + 3, PHONE, 6) == 0 && raw[9] == 1);
    }
    s_ng = 0;
    k_keep = 0;
}

/* the live apply handover through the port: the old payload writes bt_handover when it says
 * idle (ng_quiesce -> btc_handover), the next payload (btc_restart here: a fresh .data, the same RAM) takes and clears it
 * on its first tick - the phone stays selected and nothing but LIST and the play-status registrations goes out; a payload
 * that ticks again after it said idle (the apply did not happen) clears it; a cold boot finds no fresh block */
uint8_t bt_handover[64];

static int sel_slot(const uint8_t raw[BT_SRC_RAW], const uint8_t a[6])
{
    for (int i = 0; i < 4; i++)
        if ((raw[28 + 14 * i] & 0x80) && !memcmp(raw + 29 + 14 * i, a, 6))
            return raw[40 + 14 * i] & BTS_D2_SELECTED;
    return -1;
}

static void ho_lines(void)
{
    module_line("RING 0 22:82:02:02:92:22 19 A2DP");
    module_line("RING 1 22:82:02:02:92:22 19 A2DP");
    module_line("RING 2 22:82:02:02:92:22 17 AVRCP");
    module_line("RING 3 62:81:81:81:11:a1 19 A2DP");
    module_line("RING 4 62:81:81:81:11:a1 19 A2DP");
    module_line("RING 5 62:81:81:81:11:a1 17 AVRCP");
    module_line("A2DP STREAMING START 3");
}

static void test_bt_handover_cave(void)
{
    static const uint8_t PHONE[6] = {0x62, 0x81, 0x81, 0x81, 0x11, 0xa1};
    static const uint8_t PC[6] = {0x22, 0x82, 0x02, 0x02, 0x92, 0x22};
    static const char *const LST[] = {
        "LIST 6",
        "LIST 0 CONNECTED A2DP 1024 0 0 24 0 0 22:82:02:02:92:22 19 INCOMING ACTIVE SLAVE ENCRYPTED 0",
        "LIST 1 CONNECTED A2DP 1024 0 0 24 0 0 22:82:02:02:92:22 19 INCOMING ACTIVE SLAVE ENCRYPTED 0",
        "LIST 2 CONNECTED AVRCP 512 0 0 21 0 0 22:82:02:02:92:22 17 INCOMING ACTIVE SLAVE ENCRYPTED 0",
        "LIST 3 CONNECTED A2DP 1024 0 0 24 0 0 62:81:81:81:11:a1 19 INCOMING ACTIVE SLAVE ENCRYPTED 0",
        "LIST 4 CONNECTED A2DP 1024 0 0 24 0 0 62:81:81:81:11:a1 19 INCOMING ACTIVE SLAVE ENCRYPTED 0",
        "LIST 5 CONNECTED AVRCP 512 0 0 21 0 0 62:81:81:81:11:a1 17 INCOMING ACTIVE SLAVE ENCRYPTED 0",
    };
    uint8_t raw[BT_SRC_RAW];
    memset(bt_handover, 0, sizeof bt_handover);
    boot();
    pwc_tick();
    run_ms(400);
    module_line("LIST 0");
    ho_lines();
    run_ms(3000);                                  /* the guard takes the streaming phone as the selected one */
    CHECK(src_list(raw) != NULL);
    CHECK_EQ(sel_slot(raw, PHONE), BTS_D2_SELECTED);
    CHECK_EQ(sel_slot(raw, PC), 0);
    /* an apply that did not happen: the block goes as soon as this payload ticks again */
    btc_handover();
    CHECK(!memcmp(bt_handover, "C79B", 4) && bt_handover[6] == 2);
    run_ms(8);
    CHECK_EQ(bt_handover[0], 0);
    /* the apply: idle -> block -> the new payload */
    btc_handover();
    CHECK(!memcmp(bt_handover, "C79B", 4));
    now_ms += 16;                                  /* the switch: the old payload is not ticked again */
    stock_host.time_us = now_ms * 1000u;
    btc_restart();
    memset(&bs, 0, sizeof bs);
    run_ms(8);
    CHECK_EQ(bt_handover[0], 0);                   /* taken once */
    run_ms(400);
    CHECK_EQ(bs.tx_calls, 1);
    CHECK_STR(bs.tx[0], "LIST\r\n");
    for (unsigned i = 0; i < sizeof LST / sizeof LST[0]; i++)
        module_line(LST[i]);
    run_ms(15000);
    CHECK(src_list(raw) != NULL);
    CHECK_EQ(sel_slot(raw, PHONE), BTS_D2_SELECTED);
    CHECK_EQ(sel_slot(raw, PC), 0);
    for (int i = 0; i < bs.tx_calls && i < 20; i++)   /* no CALL / CLOSE / module setting - LIST and the registrations */
        CHECK(!strcmp(bs.tx[i], "LIST\r\n") || !strncmp(bs.tx[i], "SET ", 4) || !strcmp(bs.tx[i], "SET\r\n") ||
              !strcmp(bs.tx[i], "AVRCP PDU 31 1\r\n"));
    /* a cold boot (power cycle with the RAM somehow kept): the stamp is of the power-on before - refused */
    btc_handover();
    boot();
    CHECK_EQ(bt_handover[0], 0);
    pwc_tick();
    run_ms(400);
    module_line("LIST 0");
    run_ms(20000);
    CHECK(src_list(raw) != NULL);
    CHECK_EQ(sel_slot(raw, PHONE), -1);
    for (int i = 0; i < bs.tx_calls && i < 20; i++)
        CHECK(strncmp(bs.tx[i], "CALL", 4) != 0);
}

/* the decided module values at power-on through the port: the factory listing -> the three
 * op lines (each + `SET`), then ONE module restart by the stock off / on (as 1D 07), the listing read back; the 1D 0A /
 * 0C / 0D states show it; a decided listing -> nothing but the read */
static void pin_listing(const char *cls, const char *av, const char *au)
{
    module_line("SET");
    module_line("SET BT BDADDR 02:07:80:aa:bb:cc");
    module_line("SET BT NAME CA79 Audio");
    module_line(cls);
    module_line("SET BT PAGEMODE 3 2000 1");
    module_line("SET CONTROL AUTOCALL 19 5000 A2DP");
    module_line("SET PROFILE A2DP SINK");
    module_line(av);
    module_line(au);
    module_line("SET");
}

extern volatile uint8_t stk_bt_audio_cfg;

static void test_bt_pin_cave(void)
{
    uint32_t n;
    const uint8_t *r;
    boot_pin = 1;
    /* the module with the decided values (this piano since 11-11): one `SET`, nothing else */
    boot();
    stk_bt_audio_cfg = 1;
    pwc_tick();
    run_ms(400);
    module_line("LIST 0");
    run_ms(3000);
    CHECK_EQ(bs.tx_calls, 2);
    CHECK_STR(bs.tx[1], "SET\r\n");
    pin_listing("SET BT CLASS 240414", "SET PROFILE AVRCP CONTROLLER 0f", "SET CONTROL AUDIO INTERNAL I2S_SLAVE EVENT");
    run_ms(30000);
    CHECK_EQ(bs.tx_calls, 2);
    CHECK_EQ(stk_bt_power, 1);
    /* the factory module */
    boot();
    stk_bt_audio_cfg = 1;
    pwc_tick();
    run_ms(400);
    module_line("LIST 0");
    run_ms(3000);
    CHECK_STR(bs.tx[1], "SET\r\n");
    pin_listing("SET BT CLASS 001f00", "SET PROFILE AVRCP CONTROLLER 1f", "SET CONTROL AUDIO INTERNAL I2S_SLAVE KEEPALIVE");
    run_ms(400);
    CHECK_EQ(bs.tx_calls, 4);
    CHECK_STR(bs.tx[2], "SET BT CLASS 240414\r\n");
    CHECK_STR(bs.tx[3], "SET\r\n");
    pin_listing("SET BT CLASS 240414", "SET PROFILE AVRCP CONTROLLER 1f", "SET CONTROL AUDIO INTERNAL I2S_SLAVE KEEPALIVE");
    run_ms(400);
    CHECK_EQ(bs.tx_calls, 6);
    CHECK_STR(bs.tx[4], "SET PROFILE AVRCP CONTROLLER 0f\r\n");
    CHECK_STR(bs.tx[5], "SET\r\n");
    pin_listing("SET BT CLASS 240414", "SET PROFILE AVRCP CONTROLLER 0f", "SET CONTROL AUDIO INTERNAL I2S_SLAVE KEEPALIVE");
    run_ms(400);
    CHECK_EQ(bs.tx_calls, 8);
    CHECK_STR(bs.tx[6], "SET CONTROL AUDIO INTERNAL I2S_SLAVE EVENT\r\n");
    CHECK_STR(bs.tx[7], "SET\r\n");
    CHECK_EQ(stk_bt_power, 1);
    pin_listing("SET BT CLASS 240414", "SET PROFILE AVRCP CONTROLLER 0f", "SET CONTROL AUDIO INTERNAL I2S_SLAVE EVENT");
    run_ms(16);
    CHECK_EQ(stk_bt_power, 0);                      /* the stock off (FUN_400AD030), as 1D 07 */
    run_ms(700);
    CHECK_EQ(stk_bt_power, 1);                      /* on again 0.6 s later (FUN_400AD098) */
    run_ms(400);
    CHECK_EQ(bs.tx_calls, 9);                      /* the module is on again: the port learns its links */
    CHECK_STR(bs.tx[8], "LIST\r\n");
    module_line("LIST 0");
    run_ms(2300);
    CHECK_EQ(bs.tx_calls, 9);
    run_ms(700);                                   /* BTS_SET_READ_MS after the module came back: the read back */
    CHECK_EQ(bs.tx_calls, 10);
    CHECK_STR(bs.tx[9], "SET\r\n");
    pin_listing("SET BT CLASS 240414", "SET PROFILE AVRCP CONTROLLER 0f", "SET CONTROL AUDIO INTERNAL I2S_SLAVE EVENT");
    run_ms(16);
    r = av_op(0, -1, &n);
    CHECK(r && r[3] == BTS_AVF_CTRL && r[5] == BTS_OK && (r[7] << 7 | r[8]) == 0x0F && (r[9] << 7 | r[10]) == 0x1F);
    r = au_op(0, -1, &n);
    CHECK(r && r[3] == BTS_AUD_STD && r[5] == BTS_OK && r[7] == BTS_AUD_EVENT && r[8] == BTS_AUD_FACTORY);
    r = co_op(0, 0, &n);
    CHECK(r && r[5] == BTS_OK && co_val(r + 7) == 0x240414u && co_val(r + 11) == 0x001F00u);
    run_ms(60000);
    CHECK_EQ(bs.tx_calls, 10);                     /* once per power-on: no second restart, no more writes */
    CHECK_EQ(stk_bt_power, 1);
    /* the module refuses the AVRCP line (lists 1f again): rc 5 in 1D 0C, the restart for the audio line only, once */
    boot();
    stk_bt_audio_cfg = 1;
    pwc_tick();
    run_ms(400);
    module_line("LIST 0");
    run_ms(3000);
    pin_listing("SET BT CLASS 240414", "SET PROFILE AVRCP CONTROLLER 1f", "SET CONTROL AUDIO INTERNAL I2S_SLAVE KEEPALIVE");
    run_ms(400);
    CHECK_STR(bs.tx[2], "SET PROFILE AVRCP CONTROLLER 0f\r\n");
    pin_listing("SET BT CLASS 240414", "SET PROFILE AVRCP CONTROLLER 1f", "SET CONTROL AUDIO INTERNAL I2S_SLAVE KEEPALIVE");
    run_ms(400);
    CHECK_STR(bs.tx[4], "SET CONTROL AUDIO INTERNAL I2S_SLAVE EVENT\r\n");
    pin_listing("SET BT CLASS 240414", "SET PROFILE AVRCP CONTROLLER 1f", "SET CONTROL AUDIO INTERNAL I2S_SLAVE EVENT");
    run_ms(16);
    r = av_op(0, -1, &n);
    CHECK(r && r[5] == BTS_STATE);
    run_ms(4000);
    CHECK_EQ(bs.tx_calls, 8);                      /* the restart (audio line): LIST, then the read back `SET` */
    CHECK_STR(bs.tx[6], "LIST\r\n");
    CHECK_STR(bs.tx[7], "SET\r\n");
    pin_listing("SET BT CLASS 240414", "SET PROFILE AVRCP CONTROLLER 1f", "SET CONTROL AUDIO INTERNAL I2S_SLAVE EVENT");
    run_ms(60000);
    CHECK_EQ(bs.tx_calls, 8);
    boot_pin = 0;
}

/* a live apply with no handover block taken (from the built-in, from a payload before the handover block, a stale block): the app that applied it holds the binding at the first tick.  The factory listing -> the three lines
 * are written (each + `SET`) but the module is never restarted (「a live apply writes but never restarts」 -
 * before: only a taken block said "live", so this apply restarted the module and dropped every link) */
static void test_bt_pin_live_no_block_cave(void)
{
    memset(bt_handover, 0, sizeof bt_handover);
    boot_pin = 1;
    boot_bound = 1;
    boot();
    boot_bound = 0;
    stk_bt_audio_cfg = 1;
    pwc_tick();
    run_ms(400);
    module_line("LIST 0");
    run_ms(3000);
    CHECK_STR(bs.tx[1], "SET\r\n");
    pin_listing("SET BT CLASS 001f00", "SET PROFILE AVRCP CONTROLLER 1f", "SET CONTROL AUDIO INTERNAL I2S_SLAVE KEEPALIVE");
    run_ms(400);
    CHECK_STR(bs.tx[2], "SET BT CLASS 240414\r\n");
    pin_listing("SET BT CLASS 240414", "SET PROFILE AVRCP CONTROLLER 1f", "SET CONTROL AUDIO INTERNAL I2S_SLAVE KEEPALIVE");
    run_ms(400);
    CHECK_STR(bs.tx[4], "SET PROFILE AVRCP CONTROLLER 0f\r\n");
    pin_listing("SET BT CLASS 240414", "SET PROFILE AVRCP CONTROLLER 0f", "SET CONTROL AUDIO INTERNAL I2S_SLAVE KEEPALIVE");
    run_ms(400);
    CHECK_EQ(bs.tx_calls, 8);
    CHECK_STR(bs.tx[6], "SET CONTROL AUDIO INTERNAL I2S_SLAVE EVENT\r\n");
    pin_listing("SET BT CLASS 240414", "SET PROFILE AVRCP CONTROLLER 0f", "SET CONTROL AUDIO INTERNAL I2S_SLAVE EVENT");
    int powered = 1;
    for (int t = 0; t < 20000; t += 8) {          /* the module stays powered: no stock off, no LIST after a restart */
        run_ms(8);
        if (stk_bt_power != 1)
            powered = 0;
    }
    CHECK_EQ(powered, 1);
    CHECK_EQ(bs.tx_calls, 8);
    boot_pin = 0;
}

/* ---- standby and the BT links (user decision 2026-11-14) ---------------------------------- */
/* the power port's stand-ins (tests/test_power_cave.c, same binary) */
extern volatile uint8_t stk_bt_audio_cfg;
extern volatile uint32_t stk_apo_counter;
extern volatile uint8_t stk_apo_setting;
extern volatile uint32_t stk_apo_limits[4];

/* both ports, one engine-loop pass every 8 ms (the power port's kernel-timer hook only where a test calls it) */
static void run_both(uint32_t ms)
{
    for (uint32_t t = 0; t < ms; t += 8) {
        now_ms += 8;
        stock_host.time_us = now_ms * 1000u;
        btc_tick();
        pwc_tick();
    }
}

/* a power op 18 / 19 on stock port 2: its rc (0xFF: no reply) */
static uint8_t pw_op(uint8_t opc, const uint8_t *args, size_t n)
{
    uint8_t m[24] = {0xF0, 0x7D, 0x43, 0x41, 0x37, 0x39, opc};
    memcpy(m + 7, args, n);
    m[7 + n] = 0xF7;
    int before = stock_host.tx_calls[0] + stock_host.tx_calls[1] + stock_host.tx_calls[2] + stock_host.tx_calls[3];
    pwc_sysex(2, m, (uint32_t)(8 + n));
    run_both(8);
    int after = stock_host.tx_calls[0] + stock_host.tx_calls[1] + stock_host.tx_calls[2] + stock_host.tx_calls[3];
    return after != before && stock_host.last_tx_len >= 10 ? stock_host.last_tx[8] : 0xFF;
}

/* the commands sent since bs was cleared: exactly `want`, or every CALL (want NULL) */
static int sent_calls(const char *want)
{
    int c = 0;
    for (int i = 0; i < bs.tx_calls && i < 20; i++)
        c += want ? !strcmp(bs.tx[i], want) : !strncmp(bs.tx[i], "CALL", 4);
    return c;
}

/* run ms; the times (ms after t0) at which a CALL went out */
static int calls_at(uint32_t ms, uint32_t t0, uint32_t *at, int max)
{
    int n = 0;
    for (uint32_t t = 0; t < ms; t += 8) {
        int before = bs.tx_calls;
        run_both(8);
        for (int i = before; i < bs.tx_calls && i < 20; i++)
            if (!strncmp(bs.tx[i], "CALL", 4) && n < max)
                at[n++] = now_ms - t0;
    }
    return n;
}

enum { SBW_ENTER, SBW_REMOTE_OFF, SBW_APO };

/* power-on with the power policy's flags (0x05: APO -> standby, BT audio off in standby; 0x01: the module kept on - the
 * TV wake), the reconnect choice, a preferred device or none, AUTOCALL as the module's listing says; the phone connects
 * and streams - the guard takes it as the selected one (the "last selected" device) */
static void sbw_setup(uint8_t pflags, uint8_t recon, const uint8_t *pref, bool autocall)
{
    static const uint8_t PHONE[6] = {0x62, 0x81, 0x81, 0x81, 0x11, 0xa1};
    const uint8_t pol[11] = {0x01, 0x10, 0x01, pflags, 0x78, 0x00, 0x60, 0x01, 0x00, 0x00, 0x00};
    const uint8_t g[1] = {6};
    uint8_t sp[15], raw[BT_SRC_RAW];
    uint32_t n;
    const uint8_t *r;
    k_keep = 0;
    boot();
    stk_bt_audio_cfg = 1;
    stk_apo_setting = 1;
    stk_apo_counter = 1;
    pwc_hooks_attached(PW_HK_ALL);
    run_both(16);
    CHECK_EQ(pwc_on(), 1);
    CHECK_EQ(pw_op(0x18, pol, sizeof pol), 0);
    r = op(2, g, 1, &n);
    CHECK(r && n == 19);
    if (!r)
        return;
    memcpy(sp, r, 6);                              /* -, mode, flags, hold, sweep, has_pref */
    sp[0] = 6;
    sp[5] = pref ? 1 : 0;
    pack_addr(pref ? pref : PHONE, sp + 6);
    sp[13] = 0;
    sp[14] = recon;
    r = op(2, sp, 15, &n);
    CHECK(r && r[1] == BT_OK && r[15] == recon);
    run_both(400);
    module_line("LIST 0");
    module_line("RING 0 62:81:81:81:11:a1 19 A2DP");
    module_line("RING 1 62:81:81:81:11:a1 19 A2DP");
    module_line("RING 2 62:81:81:81:11:a1 17 AVRCP");
    module_line("A2DP STREAMING START 0");
    run_both(1000);
    {
        uint8_t a[9] = {5, BTS_ACT_USE};           /* 「이 기기로 듣기」: the phone is the selected (last picked) one */
        pack_addr(PHONE, a + 2);
        CHECK_EQ(rc_of(a, 9), BT_OK);
    }
    run_both(3500);                                /* the AUTOCALL read (`SET`): someone to reconnect */
    module_listing(autocall);
    run_both(500);
    CHECK(src_list(raw) != NULL);
    CHECK_EQ(sel_slot(raw, PHONE), BTS_D2_SELECTED);
    CHECK_EQ(sent_calls(NULL), 0);                 /* connected: nobody called */
}

/* standby by the path, standby_ms in it, then woken (19 02); the time the module came back on (0: it stayed on) */
static uint32_t sbw_cycle(int path, uint32_t standby_ms)
{
    const uint8_t enter[1] = {0x01}, wake[1] = {0x02}, off[1] = {PW_SB_REMOTE_OFF};
    uint8_t d[64];                                 /* PW_DIAG_RAW of the power port's TV wake build (60) */
    uint32_t t_on = 0;
    bool was_off;
    if (path == SBW_ENTER) {
        CHECK_EQ(pw_op(0x19, enter, 1), 0);
    } else if (path == SBW_REMOTE_OFF) {
        CHECK_EQ(pw_op(0x19, off, 1), 0);
    } else {
        module_line("A2DP STREAMING STOP 0");     /* the BT play hold ends (a stream holds the APO) */
        run_both(500);
        pwc_apo_timer();                           /* its release: the menu time counts from here */
        stk_apo_counter = stk_apo_limits[1];       /* the APO menu time is up: the 20 ms hook's next tick */
        pwc_apo_timer();
        run_both(16);
    }
    pwc_diag(d);
    CHECK_EQ(d[2], SB_STANDBY);
    run_both(standby_ms);
    memset(&bs, 0, sizeof bs);
    was_off = stk_bt_power == 0;
    CHECK_EQ(pw_op(0x19, wake, 1), 0);
    for (int i = 0; i < 100 && !t_on; i++) {
        run_both(8);
        if (was_off && stk_bt_power)
            t_on = now_ms;
    }
    run_both(8);
    pwc_diag(d);
    CHECK_EQ(d[2], SB_ON);
    CHECK_EQ(stk_bt_power, 1);
    return was_off ? t_on : 0;
}

/* the user decision of 2026-11-14: a wake after a standby that switched the BT audio module
 * off (power policy 「대기 중 BT 오디오 켜 두기」 off - the links went with it) follows the reconnect choice exactly as a
 * power-on does - the preferred device, else (preferred · last) the device selected before the standby, the same
 * 4 / 8 / 15 / 30 / 60 s; nobody with the choice off or AUTOCALL on; nothing new when the module stayed on (links kept).
 * Every way into standby: 19 01 (「지금 대기」), 19 03 (remote off), the APO */
static void test_bt_standby_wake_reconnect(void)
{
    static const uint8_t PC[6] = {0x22, 0x82, 0x02, 0x02, 0x92, 0x22};
    static const char *const CALL_PHONE = "CALL 62:81:81:81:11:a1 19 A2DP\r\n";
    static const char *const CALL_PC = "CALL 22:82:02:02:92:22 19 A2DP\r\n";
    uint32_t at[8], at_boot[8], t_on, t_boot;
    int n, nb;
    /* preferred · last, nothing preferred: the phone selected before the standby is called after the wake - every path */
    for (int path = SBW_ENTER; path <= SBW_APO; path++) {
        sbw_setup(0x05, BTS_RC_PREF_LAST, NULL, false);
        t_on = sbw_cycle(path, 60000);
        CHECK(t_on != 0);                          /* the module was off and came back */
        run_both(400);
        CHECK_STR(bs.tx[0], "LIST\r\n");          /* it learns the links again (none) */
        module_line("LIST 0");
        n = calls_at(15000, t_on, at, 8);
        CHECK_EQ(n, 2);
        CHECK_EQ(sent_calls(CALL_PHONE), 2);
        CHECK(n == 2 && at[0] >= 4000 && at[0] < 4100 && at[1] - at[0] >= 8000);
    }
    /* the schedule is the power-on's: the PC preferred (preferred only) - the calls after a wake come at the same times
     * after the module start as after a power-on */
    sbw_setup(0x05, BTS_RC_PREF, PC, false);
    t_on = sbw_cycle(SBW_ENTER, 1000);
    module_line("LIST 0");
    n = calls_at(60000, t_on, at, 8);
    CHECK_EQ(sent_calls(CALL_PC), n);
    CHECK_EQ(sent_calls(CALL_PHONE), 0);           /* preferred only: never the last selected */
    k_keep = 1;                                    /* the same NOR: the choice + the PC come back at power-on */
    boot();
    k_keep = 0;
    stk_bt_audio_cfg = 1;
    pwc_hooks_attached(PW_HK_ALL);
    t_boot = now_ms - 8;                           /* boot(): the module on from the first pass */
    run_both(400);
    module_line("LIST 0");
    run_both(3000);
    module_listing(false);
    nb = calls_at(60000 - (now_ms - t_boot), t_boot, at_boot, 8);
    CHECK(n >= 4 && n == nb);
    for (int i = 0; i < n && i < nb; i++)
        CHECK(at[i] / 100 == at_boot[i] / 100);    /* the same 4 / 8 / 15 / 30 s steps (to 0.1 s) */
    /* off: nobody */
    sbw_setup(0x05, BTS_RC_OFF, PC, false);
    sbw_cycle(SBW_APO, 1000);
    module_line("LIST 0");
    run_both(130000);
    CHECK_EQ(sent_calls(NULL), 0);
    /* AUTOCALL on: the module calls by itself - the piano never */
    sbw_setup(0x05, BTS_RC_PREF_LAST, NULL, true);
    sbw_cycle(SBW_REMOTE_OFF, 1000);
    module_line("LIST 0");
    run_both(130000);
    CHECK_EQ(sent_calls(NULL), 0);
    /* the module kept on in standby (the TV wake): the links stayed - no CALL at the wake */
    sbw_setup(0x01, BTS_RC_PREF_LAST, PC, false);
    CHECK_EQ(sbw_cycle(SBW_ENTER, 60000), 0);
    run_both(130000);
    CHECK_EQ(sent_calls(NULL), 0);
    CHECK(btc_a2dp_links() != 0);
    stk_apo_setting = 0;
}

void suite_bt_cave(void)
{
    RUN(test_bt_status_default);
    RUN(test_bt_p2_unlatch);
    RUN(test_bt_p2_unlatch_conditions);
    RUN(test_bt_as_guard);
    RUN(test_bt_switches);
    RUN(test_bt_query);
    RUN(test_bt_query_refused);
    RUN(test_bt_query_ring_reset);
    RUN(test_bt_query_retry);
    RUN(test_bt_p2_ring_check);
    RUN(test_bt_tx_discipline);
    RUN(test_bt_heartbeat);
    RUN(test_bt_sources_cave);
    RUN(test_bt_use_cave);
    RUN(test_bt_autocall_cave);
    RUN(test_bt_class_cave);
    RUN(test_bt_avrcp_cave);
    RUN(test_bt_audio_cave);
    RUN(test_bt_pin_cave);
    RUN(test_bt_policy_nor);
    RUN(test_bt_ng_startup);
    RUN(test_bt_handover_cave);
    RUN(test_bt_pin_live_no_block_cave);
    RUN(test_bt_standby_wake_reconnect);
    RUN(test_bt_ctrl_bound);
    RUN(test_bt_level);
}
