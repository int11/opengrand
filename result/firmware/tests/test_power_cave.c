/* Tests for the cave power port (ports/cave/power_cave.c): the power policy ops, the APO rule on
 * the stock counter, partial standby bound to the stock routines, the re-light hooks, the panel
 * touch / key / power-switch wake and the policy kvstore over NOR unused_3.
 * Every stock routine and global of power_stock.h is a recording stand-in here, with a small model
 * of the stock 20 ms APO body (FUN_400ABFE8's counter block) and of the NOR chip.
 * TV wake is always built in (since ota_power2): standby keeps the BT audio module on by default. */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#include "../ports/cave/bt_cave.h"
#include "../ports/cave/hal_cave.h"
#include "../ports/cave/power_cave.h"
#include "../ports/cave/stock_host.h"
#include "ca79ng/power.h"
#include "ca79ng/standby.h"
#include "test.h"

#define MIN_MS (60u * 1000u)
#define KV_LO 0x604C0000u
#define NOR_SIZE 0x40000u

/* ---- stock stand-ins (power_stock.h) ---- */
volatile uint32_t stk_apo_counter;
volatile uint8_t stk_power_req;
volatile uint8_t stk_gp3_event;
volatile uint8_t stk_gp3_level;
volatile uint8_t stk_apo_setting;
volatile uint32_t stk_power_press;
volatile uint32_t stk_bt_power;
volatile uint8_t stk_a2dp_flag;
volatile uint8_t stk_bt_audio_cfg;
volatile uint8_t stk_special_mode;
volatile uint8_t stk_binding;
volatile uint8_t stk_bt_rx_ring[0x800];
volatile uint16_t stk_bt_rx_wr;
volatile uint16_t stk_bt_rx_rd;
volatile uint8_t stk_bt_rx_flag;
volatile uint8_t stk_key_state[88];
volatile uint8_t stk_key_scan_on;
volatile uint32_t stk_apo_limits[4] = {0, 45000, 180000, 360000};
volatile uint32_t stk_gpio_out67;
volatile uint8_t stk_sx_state[4];
volatile uint16_t stk_sx_len[4];

static struct {
    char log[64][16];      /* routine calls in order */
    int n;
    uint8_t hp;            /* GP5[14] input: headphones plugged */
    uint8_t hp_var;        /* 0x4139E7FA */
    int busy;              /* stock writer owns the chip */
    int jobs;              /* deferred writer job */
    int dsp;               /* dis_dsp depth */
    int busy_set;          /* stock busy flag we raised */
    int nor_bad_ctx;       /* NOR writer called without dis_dsp + busy flag */
    int nor_out_of_range;
    int erases, programs;
    int dly;
    int rx_resets;         /* stk_bt_rx_reset calls */
    uint8_t nor[NOR_SIZE]; /* unused_3 */
} fs;

static void logc(const char *s)
{
    if (fs.n < 64) {
        strncpy(fs.log[fs.n], s, 15);
        fs.log[fs.n][15] = 0;
    }
    fs.n++;
}

void stk_amp_off(void) { stk_gpio_out67 &= ~0x8000u; logc("amp_off"); }
void stk_amp_on(void) { stk_gpio_out67 |= 0x8000u; logc("amp_on"); }
void stk_mute(void) { stk_gpio_out67 &= ~0x0800u; logc("mute"); }
void stk_unmute(void) { stk_gpio_out67 |= 0x0800u; logc("unmute"); }
void stk_cleanup(void) { logc("cleanup"); }
void stk_hp_apply(void)
{
    fs.hp_var = fs.hp;
    if (fs.hp)
        stk_gpio_out67 &= ~0x8000u;   /* FUN_4013BD94 switches the amp off */
    logc("hp_apply");
}
int stk_hp_present(void) { return fs.hp_var; }
void stk_bt_audio_off(void) { stk_bt_power = 0; logc("bt_off"); }
void stk_bt_audio_on(void) { stk_bt_power = 1; logc("bt_on"); }
void stk_bt_flags_clear(void) { stk_a2dp_flag = 0; logc("bt_clear"); }
/* FUN_40145C00, the stock store order */
void stk_bt_rx_reset(void)
{
    stk_bt_rx_wr = 0;
    stk_bt_rx_flag = 0;
    stk_bt_rx_rd = 0;
    fs.rx_resets++;
}
int stk_flash_busy(void) { return fs.busy || fs.busy_set; }
int stk_flash_jobs(void) { return fs.jobs; }
void stk_dis_dsp(void) { fs.dsp++; }
void stk_ena_dsp(void) { fs.dsp--; }
void stk_dly_tsk(uint32_t ms) { (void)ms; fs.dly++; }
static void stk_flash_busy_set(int on) { fs.busy_set = on; }

static uint8_t *nor_at(uint32_t a, uint32_t n)
{
    if (a < KV_LO || a + n > KV_LO + NOR_SIZE) {
        fs.nor_out_of_range++;
        return NULL;
    }
    return fs.nor + (a - KV_LO);
}

void stk_flash_copy(uint32_t src, void *dst, uint32_t len)
{
    uint8_t *p = nor_at(src, len);
    if (p)
        memcpy(dst, p, len);
    else
        memset(dst, 0xFF, len);
}

static int stk_nor_erase(uint32_t a)
{
    uint8_t *p = nor_at(a, 0x20000);
    if (!fs.dsp || !fs.busy_set)
        fs.nor_bad_ctx++;
    fs.erases++;
    if (p)
        memset(p, 0xFF, 0x20000);
    return 0;
}

static int stk_nor_prog_buf(uint32_t a, const void *src, uint32_t len)
{
    uint8_t *p = nor_at(a, len);
    const uint8_t *s = src;
    if (!fs.dsp || !fs.busy_set || len > 512 || (len & 1) || (a & 0x1FF) + len > 512)
        fs.nor_bad_ctx++;
    fs.programs++;
    if (p)
        for (uint32_t i = 0; i < len; i++)
            p[i] &= s[i];   /* NOR: program only clears bits */
    return 0;
}

/* the loader's NOR write service (ota_loader_v4 stage-0 struct s0_svc): the same owner-of-the-chip sequence over the
 * stand-ins above - dispatch off, idle check (1 = busy, nothing done), busy flag around the writer */
static int svc_erase(unsigned int a)
{
    int rc;
    stk_dis_dsp();
    if (stk_flash_busy() || stk_flash_jobs()) {
        stk_ena_dsp();
        return 1;
    }
    stk_flash_busy_set(1);
    rc = stk_nor_erase(a);
    stk_flash_busy_set(0);
    stk_ena_dsp();
    return rc ? 2 : 0;
}

static int svc_prog(unsigned int a, const void *src, unsigned int n)
{
    int rc;
    stk_dis_dsp();
    if (stk_flash_busy() || stk_flash_jobs()) {
        stk_ena_dsp();
        return 1;
    }
    stk_flash_busy_set(1);
    rc = stk_nor_prog_buf(a, src, n);
    stk_flash_busy_set(0);
    stk_ena_dsp();
    return rc ? 2 : 0;
}

/* ---- remote off: stock RAM the recorder view reads, the session core's answer ---- */
static struct {
    uint8_t rec_state;     /* 0x4191EF79 */
    uint8_t fileop;        /* 0x413CA3EA */
    uint8_t job1;          /* 0x4139DAB4 */
    int core;              /* sxc_ctrl_bound: -1 no session core, 0 / 1 */
    uint8_t core_mask;     /* bit p: the core has port p bound */
} rs;

void pwc_host_read(uint32_t addr, void *dst, uint32_t n)
{
    uint8_t *d = dst;
    for (uint32_t i = 0; i < n; i++) {
        uint32_t a = addr + i;
        d[i] = a == 0x4191EF79u ? rs.rec_state : a == 0x413CA3EAu ? rs.fileop : a == 0x4139DAB4u ? rs.job1
               : a == 0x4191EF76u ? 0xFF : 0;
    }
}

int pwc_host_core_bound(uint8_t port)
{
    if (rs.core < 0)
        return -1;
    return ((rs.core_mask >> port) & 1u) || stk_binding == (uint8_t)(port + 1) ? 1 : 0;
}

/* ---- the stock 20 ms body around the hook, and the clock ---- */
static int expired_by_stock;   /* stock state byte became 2 through its own APO test */
/* the power switch GP3[1] (FUN_400ABD74 0x400ABE94-0x400ABFB8): the edge block first, the
 * counter block last */
static struct {
    int pressed, prev;    /* the switch now / at the last tick (the stock's 0x4139E809) */
    int offs;             /* releases with state byte 1: the stock real off (state byte 2, then the executor) */
    uint32_t max_press;   /* highest counter the body left */
} sw;

static void stock_20ms(void)
{
    pwc_apo_timer();   /* the hook stub runs first, then FUN_400ABFE8 */
    if (sw.pressed != sw.prev) {
        sw.prev = sw.pressed;
        if (sw.pressed) {            /* 0x400ABF14: counter 1, state byte 0, APO counter 0 */
            stk_power_press = 1;
            stk_power_req = 0;
            stk_apo_counter = 0;
        } else {                     /* 0x400ABE9C: APO counter + 1, counter 0, state 1 + boot done + flash idle -> 2 */
            stk_apo_counter++;
            stk_power_press = 0;
            if (stk_power_req == 1 && !fs.busy && !fs.busy_set) {
                stk_power_req = 2;
                sw.offs++;
            }
        }
    }
    uint8_t s = stk_apo_setting;
    if (stk_special_mode != 1 && s && s < 4) {
        uint32_t r0 = stk_apo_counter + 1;
        if (stk_apo_limits[s] < r0) {
            stk_power_req = 2;
            expired_by_stock++;
        }
        stk_apo_counter = r0;
    }
    if (stk_power_press) {           /* 0x400ABF94: + 1, > 15 -> state byte 1 */
        stk_power_press++;
        if (stk_power_press > sw.max_press)
            sw.max_press = stk_power_press;
        if (stk_power_press > 15)
            stk_power_req = 1;
    }
}

/* the stock activity reset FUN_400ACB84 (keys, MIDI bytes, ...) */
static void stock_activity(void)
{
    if (stk_apo_counter >= 2)
        stk_apo_counter = 1;
    stk_power_req = 0;
}

static uint32_t now_ms;

/* the engine loop every 20 ms (the real one runs every 8 ms; the rule only needs ms) */
static void run_ms(uint32_t ms)
{
    for (uint32_t t = 0; t < ms; t += 20) {
        now_ms += 20;
        stock_host.time_us = now_ms * 1000u;
        stock_20ms();
        pwc_tick();
    }
}

/* ---- 0x7D traffic ---- */
static const uint8_t HEAD[7] = {0xF0, 0x7D, 0x43, 0x41, 0x37, 0x39, 0};

/* request on a stock port, one engine-loop pass, the reply (NULL if none) */
static const uint8_t *op(uint8_t port, uint8_t opc, const uint8_t *args, size_t n, uint32_t *len)
{
    uint8_t m[40];
    memcpy(m, HEAD, 6);
    m[6] = opc;
    memcpy(m + 7, args, n);
    m[7 + n] = 0xF7;
    int before = stock_host.tx_calls[0] + stock_host.tx_calls[1] + stock_host.tx_calls[2] + stock_host.tx_calls[3];
    pwc_sysex(port, m, (uint32_t)(8 + n));
    run_ms(20);
    int after = stock_host.tx_calls[0] + stock_host.tx_calls[1] + stock_host.tx_calls[2] + stock_host.tx_calls[3];
    if (after == before)
        return NULL;
    *len = stock_host.last_tx_len;
    return stock_host.last_tx;
}

static bool reply_is(const uint8_t *r, uint32_t n, const uint8_t *want, uint32_t wn)
{
    return r && n == wn && memcmp(r, want, wn) == 0;
}

static void bt_line(const char *s)
{
    while (*s) {
        stk_bt_rx_ring[stk_bt_rx_wr & 0x7FF] = (uint8_t)*s++;
        stk_bt_rx_wr = (uint16_t)((stk_bt_rx_wr + 1) & 0x7FF);
    }
    stk_bt_rx_ring[stk_bt_rx_wr & 0x7FF] = '\r';
    stk_bt_rx_wr = (uint16_t)((stk_bt_rx_wr + 1) & 0x7FF);
    stk_bt_rx_ring[stk_bt_rx_wr & 0x7FF] = '\n';
    stk_bt_rx_wr = (uint16_t)((stk_bt_rx_wr + 1) & 0x7FF);
    stk_bt_rx_rd = stk_bt_rx_wr;   /* the stock assembler (busy loop) has taken it */
}

static int logged(const char *s)
{
    int c = 0;
    for (int i = 0; i < fs.n && i < 64; i++)
        c += strcmp(fs.log[i], s) == 0;
    return c;
}

/* fresh "power-on": stock RAM as the stock boot leaves it, the NOR kept unless wipe */
static void boot(bool wipe)
{
    stock_host_reset();
    pwc_restart();
    pwc_nor_bind(svc_erase, svc_prog);
    stk_apo_counter = 1;
    stk_power_req = 0;
    stk_gp3_event = 0;
    stk_gp3_level = 0;
    stk_apo_setting = 1;          /* 15 min */
    stk_power_press = 0;
    stk_bt_power = 1;
    stk_a2dp_flag = 0;
    stk_bt_audio_cfg = 1;
    stk_special_mode = 0;
    stk_binding = 3;              /* the app on BLE (stock port 2) */
    memset(&rs, 0, sizeof rs);
    rs.rec_state = 2;             /* recorder stopped, song loaded */
    rs.core = -1;
    memset((void *)stk_bt_rx_ring, 0, sizeof stk_bt_rx_ring);
    stk_bt_rx_wr = 0;
    stk_bt_rx_rd = 0;
    stk_bt_rx_flag = 0;
    memset((void *)stk_key_state, 0, sizeof stk_key_state);
    memset((void *)stk_sx_state, 0, sizeof stk_sx_state);
    memset((void *)stk_sx_len, 0, sizeof stk_sx_len);
    stk_key_scan_on = 1;
    stk_gpio_out67 = 0x8800u;     /* amp on, unmuted */
    if (wipe)
        memset(fs.nor, 0xFF, sizeof fs.nor);
    uint8_t nor[NOR_SIZE];
    memcpy(nor, fs.nor, sizeof nor);
    memset(&fs, 0, sizeof fs);
    memcpy(fs.nor, nor, sizeof nor);
    expired_by_stock = 0;
    memset(&sw, 0, sizeof sw);
    now_ms = 0;
    pwc_hooks_attached(PW_HK_ALL);
    run_ms(20);
    fs.n = 0;
}

static uint8_t diag_state(void)
{
    uint8_t d[PW_DIAG_RAW];
    pwc_diag(d);
    return d[2];
}

/* ---------------------------------------------------------------------------------------- */

TEST(pwc_policy_ops_match_the_host_core)
{
    static const uint8_t GET[] = {0x00};
    /* default policy 01 01 2C 01 E0 01 00 00 packed (ngext.c / app ngext.ts; max hold 5 h since 2026-11-02) */
    static const uint8_t R_GET[] = {0xF0, 0x7D, 0x43, 0x41, 0x37, 0x39, 0x58, 0x00, 0x00,
                                    0x10, 0x01, 0x01, 0x2C, 0x01, 0x60, 0x01, 0x00, 0x00, 0x00, 0xF7};
    /* the firmware Makefile's simulator check: set -> reply, then get after a restart */
    static const uint8_t SET[] = {0x01, 0x10, 0x01, 0x02, 0x3C, 0x00, 0x70, 0x00, 0x00, 0x00, 0x00};
    static const uint8_t R_SET[] = {0xF0, 0x7D, 0x43, 0x41, 0x37, 0x39, 0x58, 0x01, 0x00,
                                    0x10, 0x01, 0x02, 0x3C, 0x00, 0x70, 0x00, 0x00, 0x00, 0x00, 0xF7};
    static const uint8_t R_GET2[] = {0xF0, 0x7D, 0x43, 0x41, 0x37, 0x39, 0x58, 0x00, 0x00,
                                     0x10, 0x01, 0x02, 0x3C, 0x00, 0x70, 0x00, 0x00, 0x00, 0x00, 0xF7};
    uint32_t n = 0;
    const uint8_t *r;
    boot(true);
    r = op(2, 0x18, GET, sizeof GET, &n);
    CHECK(reply_is(r, n, R_GET, sizeof R_GET));
    CHECK_EQ(stock_host.last_tx_port, 2);                   /* back to the port it came from */
    r = op(0, 0x18, GET, sizeof GET, &n);                    /* get: any port */
    CHECK(r && n == sizeof R_GET && stock_host.last_tx_port == 0);
    r = op(0, 0x18, SET, sizeof SET, &n);                    /* set: bound port only */
    CHECK(r && n == 20 && r[7] == 0x01 && r[8] == 2);
    CHECK_EQ(fs.programs, 0);
    r = op(2, 0x18, SET, sizeof SET, &n);
    CHECK(reply_is(r, n, R_SET, sizeof R_SET));
    CHECK_EQ(fs.erases, 0);                                  /* blank unused_3: no erase cycle */
    CHECK(fs.programs > 0);
    CHECK_EQ(fs.nor_bad_ctx, 0);
    CHECK_EQ(fs.nor_out_of_range, 0);
    CHECK_EQ(fs.dsp, 0);
    CHECK_EQ(fs.busy_set, 0);
    /* restart on the same NOR: the policy is back */
    boot(false);
    r = op(2, 0x18, GET, sizeof GET, &n);
    CHECK(reply_is(r, n, R_GET2, sizeof R_GET2));
    /* bad value (cap 29 min) rc 1, policy unchanged; unknown sub-op / short / bad tag silent */
    static const uint8_t BAD[] = {0x01, 0x00, 0x01, 0x01, 0x78, 0x00, 0x1D, 0x00, 0x00, 0x00, 0x00};
    r = op(2, 0x18, BAD, sizeof BAD, &n);
    CHECK(r && r[7] == 0x01 && r[8] == 1 && r[11] == 0x02);
    static const uint8_t UNK[] = {0x05};
    CHECK(op(2, 0x18, UNK, sizeof UNK, &n) == NULL);
    CHECK(op(2, 0x18, NULL, 0, &n) == NULL);
    /* defaults + save */
    static const uint8_t DEF[] = {0x02};
    r = op(2, 0x18, DEF, sizeof DEF, &n);
    CHECK(r && n == 20 && r[7] == 0x02 && r[8] == 0 && memcmp(r + 9, R_GET + 9, 10) == 0);
    boot(false);
    r = op(2, 0x18, GET, sizeof GET, &n);
    CHECK(reply_is(r, n, R_GET, sizeof R_GET));
}

TEST(pwc_kvstore_erases_only_unused_3_and_only_when_needed)
{
    static const uint8_t SET[] = {0x01, 0x10, 0x01, 0x02, 0x3C, 0x00, 0x70, 0x00, 0x00, 0x00, 0x00};
    uint32_t n = 0;
    boot(true);
    fs.nor[0x30000] = 0x00;   /* sector B not blank (foreign byte): formatting must erase it */
    run_ms(20);
    CHECK(op(2, 0x18, SET, sizeof SET, &n) != NULL);
    CHECK_EQ(fs.erases, 1);
    CHECK_EQ(fs.nor[0x30000], 0xFF);
    CHECK_EQ(fs.nor_out_of_range, 0);
    CHECK_EQ(fs.nor_bad_ctx, 0);
    /* 400 more saves: appends only (no compaction yet), every NOR call in range */
    int e0 = fs.erases;
    for (int i = 0; i < 400; i++)
        op(2, 0x18, SET, sizeof SET, &n);
    CHECK_EQ(fs.erases, e0);
    CHECK_EQ(fs.nor_out_of_range, 0);
    /* a stock writer owns the chip the whole time: save fails (rc 4), nothing programmed */
    int p0 = fs.programs;
    fs.jobs = 1;
    const uint8_t *r = op(2, 0x18, SET, sizeof SET, &n);
    CHECK(r && r[8] == 4);
    CHECK_EQ(fs.programs, p0);
    CHECK(fs.dly >= 200);
    fs.jobs = 0;
}

TEST(pwc_busy_flash_at_boot_never_formats_the_store)
{
    static const uint8_t SET[] = {0x01, 0x10, 0x01, 0x02, 0x3C, 0x00, 0x70, 0x00, 0x00, 0x00, 0x00};
    static const uint8_t GET[] = {0x00};
    uint32_t n = 0;
    boot(true);
    CHECK(op(2, 0x18, SET, sizeof SET, &n) != NULL);
    /* power-on while a stock writer holds the chip: defaults, no waiting, nothing written */
    stock_host_reset();
    pwc_restart();
    fs.jobs = 1;
    int p0 = fs.programs, e0 = fs.erases, d0 = fs.dly;
    run_ms(200);
    uint8_t d[PW_DIAG_RAW];
    pwc_diag(d);
    CHECK_EQ(d[5], 0);                 /* store state unknown, asked again every pass */
    CHECK_EQ(fs.dly, d0);              /* the probe never waits */
    CHECK_EQ(fs.programs, p0);
    CHECK_EQ(fs.erases, e0);
    /* the writer finishes: the saved policy is loaded */
    fs.jobs = 0;
    run_ms(20);
    const uint8_t *r = op(2, 0x18, GET, sizeof GET, &n);
    CHECK(r && n == 20 && r[10] == 0x01 && r[11] == 0x02 && r[12] == 0x3C);
    pwc_diag(d);
    CHECK_EQ(d[5], 2);
}

TEST(pwc_apo_expiry_enters_standby_without_the_stock_off)
{
    uint32_t n = 0;
    uint8_t q[] = {0x00};
    boot(true);
    run_ms(15 * MIN_MS - 100);
    CHECK_EQ(diag_state(), SB_ON);
    run_ms(200);
    CHECK_EQ(diag_state(), SB_STANDBY);
    CHECK_EQ(expired_by_stock, 0);
    CHECK_EQ(stk_power_req, 0);
    /* the stock standby order on the stock routines */
    CHECK(fs.n >= 3 && !strcmp(fs.log[0], "cleanup") && !strcmp(fs.log[1], "amp_off") &&
          !strcmp(fs.log[2], "mute"));
    CHECK_EQ(logged("bt_off"), 0);                           /* TV wake (default): the module stays on */
    CHECK_EQ(stk_bt_power, 1);
    CHECK_EQ(stk_gpio_out67 & 0x8800u, 0);
    /* notice 59 7F 00 01 to the bound app port (2), not to the panel */
    static const uint8_t NOTE[] = {0xF0, 0x7D, 0x43, 0x41, 0x37, 0x39, 0x59, 0x7F, 0x00, 0x01, 0xF7};
    CHECK(stock_host.last_tx_port == 2 && stock_host.last_tx_len == sizeof NOTE &&
          memcmp(stock_host.last_tx, NOTE, sizeof NOTE) == 0);
    /* in standby the stock counter is held: 9 h later no stock expiry; the 8 h cap decides */
    const uint8_t *r = op(2, 0x19, q, 1, &n);
    CHECK(r && r[7] == 0 && r[8] == 0 && r[9] == SB_STANDBY);
    run_ms(8u * 60u * MIN_MS - 2 * MIN_MS);
    CHECK_EQ(diag_state(), SB_STANDBY);
    CHECK_EQ(expired_by_stock, 0);
    CHECK(stk_apo_counter <= 2);
    run_ms(3 * MIN_MS);
    CHECK_EQ(diag_state(), SB_OFF);
    CHECK_EQ(stk_power_req, 2);   /* the stock executor's request: 7E 00 00, cleanup, final cut */
    stk_power_req = 0;            /* an activity reset clears it ... */
    run_ms(20);
    CHECK_EQ(stk_power_req, 2);   /* ... the request is re-asserted until the executor takes it */
}

TEST(pwc_real_off_policy_waits_for_the_flash)
{
    static const uint8_t SET_OFF[] = {0x01, 0x10, 0x01, 0x00, 0x78, 0x00, 0x60, 0x01, 0x00, 0x00, 0x00};
    uint32_t n = 0;
    boot(true);
    const uint8_t *r = op(2, 0x18, SET_OFF, sizeof SET_OFF, &n);   /* flags 0: APO -> real off */
    CHECK(r && r[8] == 0);
    fs.jobs = 1;
    run_ms(15 * MIN_MS + 200);
    CHECK_EQ(diag_state(), SB_OFF_PENDING);
    CHECK_EQ(stk_power_req, 0);
    CHECK_EQ(expired_by_stock, 0);
    run_ms(10 * MIN_MS);
    CHECK_EQ(stk_power_req, 0);
    fs.jobs = 0;
    run_ms(40);
    CHECK_EQ(diag_state(), SB_OFF);
    CHECK_EQ(stk_power_req, 2);
    CHECK_EQ(logged("amp_off"), 0);   /* a real off from ON: the executor silences, not us */
}

TEST(pwc_bt_streaming_holds_apo_up_to_the_max_hold)
{
    boot(true);
    bt_line("CONNECT 0 A2DP 19");
    bt_line("A2DP STREAMING START 0");
    run_ms(90 * MIN_MS);
    CHECK_EQ(diag_state(), SB_ON);
    /* the stock counter ran (no activity): it rests at the 15 min limit, the hold only stopped its expiry */
    CHECK(stk_apo_counter >= 45000u - 1u && stk_apo_counter <= 45000u);
    CHECK_EQ(stk_power_req, 0);
    bt_line("A2DP STREAMING STOP 0");
    run_ms(14 * MIN_MS);          /* STOP reset the counter: the full 15 min from here */
    CHECK_EQ(diag_state(), SB_ON);
    run_ms(MIN_MS + 200);
    CHECK_EQ(diag_state(), SB_STANDBY);
    /* paused TV: START without STOP -> held up to the 5 h default, then right away */
    boot(true);
    bt_line("A2DP STREAMING START 0");
    run_ms(5 * 60 * MIN_MS - MIN_MS);
    CHECK_EQ(diag_state(), SB_ON);
    run_ms(MIN_MS + 200);
    CHECK_EQ(diag_state(), SB_STANDBY);
    /* activity 5 min before the cap: from the cap the normal APO counts on from that activity (10 min more) */
    boot(true);
    bt_line("A2DP STREAMING START 0");
    run_ms(5 * 60 * MIN_MS - 5 * MIN_MS);
    stock_activity();
    run_ms(5 * MIN_MS + 200);
    CHECK_EQ(diag_state(), SB_ON);
    {
        uint8_t dd[PW_DIAG_RAW];
        pwc_diag(dd);
        CHECK(dd[3] & 0x10);      /* max hold used up */
    }
    run_ms(9 * MIN_MS);
    CHECK_EQ(diag_state(), SB_ON);
    run_ms(MIN_MS + 200);
    CHECK_EQ(diag_state(), SB_STANDBY);
    /* activity still resets the counter as in stock */
    boot(true);
    run_ms(14 * MIN_MS);
    stock_activity();
    run_ms(14 * MIN_MS);
    CHECK_EQ(diag_state(), SB_ON);
    /* a ring that already held the START when the payload came up (applied mid-stream) */
    stock_host_reset();
    pwc_restart();
    memset((void *)stk_bt_rx_ring, 0, sizeof stk_bt_rx_ring);
    stk_bt_rx_wr = 0x7F0;
    bt_line("A2DP STREAMING START 1");   /* wraps around the ring end */
    stk_apo_counter = 1;
    run_ms(60 * MIN_MS);
    CHECK_EQ(diag_state(), SB_ON);
    uint8_t d[PW_DIAG_RAW];
    pwc_diag(d);
    CHECK_EQ(d[4], BT_AUDIO_STREAMING);
    CHECK(d[3] & 0x08);
}

TEST(pwc_standby_blocks_relight_and_wakes)
{
    uint32_t n = 0;
    uint8_t enter[] = {0x01}, wake[] = {0x02};
    boot(true);
    /* enter over the op (bound app) */
    const uint8_t *r = op(2, 0x19, enter, 1, &n);
    CHECK(r && r[7] == 0x01 && r[8] == 0 && r[9] == SB_STANDBY);
    r = op(2, 0x19, enter, 1, &n);
    CHECK(r && r[8] == 3);                         /* already in standby */
    /* hooks: the headphone poll is skipped, a GP3[8] event is dropped before the executor */
    CHECK_EQ(pwc_hp_pass(), 0);
    stk_gp3_event = 1;
    pwc_exec_pre();
    CHECK_EQ(stk_gp3_event, 0);
    /* safety net: something switched the amp on -> off again, counted */
    stk_gpio_out67 |= 0x8000u;
    pwc_exec_pre();
    CHECK_EQ(stk_gpio_out67 & 0x8800u, 0);
    uint8_t d[PW_DIAG_RAW];
    pwc_diag(d);
    CHECK_EQ(d[47], 1);
    CHECK_EQ(d[38], 1);
    /* periodic Kawai traffic, panel touches on other ports: no wake */
    static const uint8_t TOUCH[] = {0xF0, 0x40, 0x00, 0x32, 0x08, 0x02, 0x51, 0x39, 0x7F, 0x00, 0xF7};
    static const uint8_t OTHER[] = {0xF0, 0x40, 0x00, 0x10, 0x08, 0x02, 0x51, 0x00, 0x7F, 0x01, 0xF7};
    pwc_kawai_peek(3, OTHER, sizeof OTHER);
    pwc_kawai_peek(2, TOUCH, sizeof TOUCH);
    pwc_kawai_peek(3, TOUCH, 8);
    run_ms(200);
    CHECK_EQ(diag_state(), SB_STANDBY);
    /* a key already down when standby began does not wake; a new press does */
    boot(true);
    stk_key_state[39] = 0xE0;
    op(2, 0x19, enter, 1, &n);
    run_ms(200);
    CHECK_EQ(diag_state(), SB_STANDBY);
    stk_key_state[39] = 0;
    run_ms(20);
    stk_key_state[40] = 0xE0;
    run_ms(20);
    CHECK_EQ(diag_state(), SB_WAKING);
    CHECK_EQ(pwc_hp_pass(), 1);                     /* the stock's again from WAKING */
    run_ms(120);
    CHECK_EQ(diag_state(), SB_ON);
    CHECK_EQ(stk_gpio_out67 & 0x8800u, 0x8800u);
    CHECK_EQ(stk_bt_power, 1);
    pwc_diag(d);
    CHECK_EQ(d[7], SB_WAKE_KEY);
    /* wake order: headphones re-read, unmute, 100 ms, amp, BT */
    int i0 = -1;
    for (int i = 0; i < fs.n && i < 64; i++)
        if (!strcmp(fs.log[i], "hp_apply")) { i0 = i; break; }
    CHECK(i0 >= 0 && i0 + 2 < fs.n && !strcmp(fs.log[i0 + 1], "unmute") && !strcmp(fs.log[i0 + 2], "amp_on"));
    CHECK_EQ(logged("bt_on"), 0);                            /* never switched off (TV wake) */
    CHECK_EQ(logged("hp_apply"), 1);
    /* panel touch (stock port 3) wakes */
    stk_key_state[40] = 0;
    op(2, 0x19, enter, 1, &n);
    pwc_kawai_peek(3, TOUCH, sizeof TOUCH);
    run_ms(20);
    pwc_diag(d);
    CHECK_EQ(d[7], SB_WAKE_PANEL_TOUCH);
    run_ms(200);
    /* power switch press wakes */
    op(2, 0x19, enter, 1, &n);
    stk_power_press = 1;
    run_ms(20);
    stk_power_press = 0;
    pwc_diag(d);
    CHECK_EQ(d[7], SB_WAKE_POWER_BUTTON);
    run_ms(200);
    /* op wake from any port, even unbound; then the APO counts again from the wake */
    op(2, 0x19, enter, 1, &n);
    stk_binding = 0;
    r = op(0, 0x19, wake, 1, &n);
    CHECK(r && r[7] == 0x02 && r[8] == 0 && r[9] == SB_WAKING);
    run_ms(200);
    CHECK_EQ(diag_state(), SB_ON);
    run_ms(14 * MIN_MS);
    CHECK_EQ(diag_state(), SB_ON);
    run_ms(MIN_MS + 200);
    CHECK_EQ(diag_state(), SB_STANDBY);
    /* enter needs a bound controller */
    boot(true);
    stk_binding = 0;
    r = op(2, 0x19, enter, 1, &n);
    CHECK(r && r[8] == 2 && r[9] == SB_ON);
}

TEST(pwc_wake_with_headphones_or_alarm)
{
    uint32_t n = 0;
    uint8_t enter[] = {0x01}, wake[] = {0x02};
    boot(true);
    op(2, 0x19, enter, 1, &n);
    fs.hp = 1;                       /* headphones plugged during standby */
    fs.n = 0;
    op(2, 0x19, wake, 1, &n);
    run_ms(200);
    CHECK_EQ(logged("amp_on"), 0);
    CHECK_EQ(stk_gpio_out67 & 0x8800u, 0x0800u);
    CHECK_EQ(fs.hp_var, 1);
    /* GP3[8] alarm at wake: amp and mute left to the stock event 1 */
    fs.hp = 0;
    op(2, 0x19, enter, 1, &n);
    stk_gp3_level = 1;
    fs.n = 0;
    op(2, 0x19, wake, 1, &n);
    run_ms(200);
    CHECK_EQ(logged("amp_on"), 0);
    CHECK_EQ(logged("unmute"), 0);
    CHECK_EQ(logged("bt_on"), 0);
    CHECK_EQ(stk_bt_power, 1);
    CHECK_EQ(diag_state(), SB_ON);
}

TEST(pwc_needs_the_hooks_and_quiesces_awake)
{
    uint32_t n = 0;
    uint8_t enter[] = {0x01};
    boot(true);
    pwc_hooks_attached(PW_HK_APO | PW_HK_KAWAI);   /* old loader / partial attach */
    const uint8_t *r = op(2, 0x19, enter, 1, &n);
    CHECK(r && r[8] == PW_NO_HOOKS && r[9] == SB_ON);
    run_ms(15 * MIN_MS + 200);                     /* APO -> no standby possible -> real off */
    CHECK_EQ(diag_state(), SB_OFF);
    CHECK_EQ(stk_power_req, 2);
    /* apply-now while in standby: woken first, idle once ON */
    boot(true);
    op(2, 0x19, enter, 1, &n);
    CHECK_EQ(pwc_quiesce(), 1);
    CHECK_EQ(diag_state(), SB_WAKING);
    run_ms(200);
    CHECK_EQ(pwc_quiesce(), 0);
    CHECK_EQ(stk_gpio_out67 & 0x8800u, 0x8800u);
    /* a queued request keeps it busy */
    static const uint8_t m[] = {0xF0, 0x7D, 0x43, 0x41, 0x37, 0x39, 0x19, 0x00, 0xF7};
    pwc_sysex(2, m, sizeof m);
    CHECK_EQ(pwc_quiesce(), 1);
    run_ms(20);
    CHECK_EQ(pwc_quiesce(), 0);
}

TEST(pwc_diag_op)
{
    uint32_t n = 0;
    boot(true);
    run_ms(1000);
    const uint8_t *r = op(1, 0x1A, NULL, 0, &n);
    CHECK(r && n == 8 + PW_DIAG_RAW + (PW_DIAG_RAW + 6) / 7 && r[6] == 0x5A);
    uint8_t d[PW_DIAG_RAW];
    pwc_diag(d);
    CHECK_EQ(d[0], PW_DIAG_LAYOUT);
    CHECK_EQ(d[0], 2);
    CHECK_EQ(d[1], PW_HK_ALL);
    CHECK(d[3] & 0x80);                 /* guards intact */
    CHECK_EQ(d[5], 1);                  /* blank store */
    CHECK_EQ(d[6], 1);
    CHECK(d[12] | d[13]);               /* timer ticks */
    CHECK_EQ(d[48], 0x0B);              /* layout 2: BT module on, TV wake armed, not latched, F-A1 armed */
    CHECK_EQ(d[49], 0);
    CHECK_EQ(d[50], 0);
    CHECK_EQ(d[51], 0);
}

/* ---- F-A1: the POWER button goes to standby ---- */

static void press_ms(uint32_t ms)
{
    sw.max_press = 0;
    sw.pressed = 1;
    run_ms(ms);
    sw.pressed = 0;
    run_ms(40);
}

TEST(pwc_power_button_standby_and_long_press_off)
{
    uint32_t n = 0;
    uint8_t d[PW_DIAG_RAW];
    boot(true);
    run_ms(100);
    pwc_diag(d);
    CHECK(d[48] & 0x08);                              /* armed: hooks + policy known, bit3 0 */
    /* from ON: 0.2 s -> nothing (the stock ignores it too) */
    press_ms(200);
    CHECK_EQ(diag_state(), SB_ON);
    CHECK_EQ(stk_power_req, 0);
    CHECK_EQ(logged("cleanup"), 0);
    /* from ON: 0.3 s, 1.5 s, 3.9 s -> standby, the counter never above 15, no stock state byte */
    static const uint32_t SHORT[] = {320, 1500, 3900};
    for (int i = 0; i < 3; i++) {
        uint32_t c0 = (uint32_t)logged("cleanup");
        press_ms(SHORT[i]);
        CHECK(sw.max_press <= 15);
        CHECK_EQ(sw.offs, 0);
        CHECK_EQ(stk_power_req, 0);
        CHECK_EQ(diag_state(), SB_STANDBY);
        CHECK_EQ((uint32_t)logged("cleanup"), c0 + 1);
        CHECK_EQ(stk_gpio_out67 & 0x8800u, 0);
        /* in standby: a tap wakes (as before F-A1) */
        press_ms(100);
        pwc_diag(d);
        CHECK_EQ(d[7], SB_WAKE_POWER_BUTTON);
        run_ms(200);
        CHECK_EQ(diag_state(), SB_ON);
    }
    /* in standby: 1.5 s -> wakes at the press, the release does nothing (no standby again, no stock off) */
    op(2, 0x19, (const uint8_t[]){0x01}, 1, &n);
    CHECK_EQ(diag_state(), SB_STANDBY);
    press_ms(1500);
    run_ms(200);
    CHECK_EQ(diag_state(), SB_ON);
    CHECK_EQ(sw.offs, 0);
    CHECK_EQ(stk_power_req, 0);
    /* from ON: 5 s -> the clamp lets go at 4 s, the stock's state byte 1, released = the stock real off */
    press_ms(5000);
    CHECK(sw.max_press > 15);
    CHECK_EQ(sw.offs, 1);
    CHECK_EQ(stk_power_req, 2);
    CHECK_EQ(diag_state(), SB_ON);                    /* the core leaves the stock off alone */
    /* from standby: 5 s -> woken at the press, then the stock real off */
    boot(true);
    run_ms(100);
    op(2, 0x19, (const uint8_t[]){0x01}, 1, &n);
    press_ms(5000);
    CHECK_EQ(sw.offs, 1);
    CHECK_EQ(stk_power_req, 2);
    /* the APO policy "power off" does not matter: the button still goes to standby */
    static const uint8_t APO_OFF[] = {0x01, 0x10, 0x01, 0x00, 0x2C, 0x01, 0x60, 0x01, 0x00, 0x00, 0x00};
    boot(true);
    run_ms(100);
    const uint8_t *r = op(2, 0x18, APO_OFF, sizeof APO_OFF, &n);
    CHECK(r && r[8] == 0);
    press_ms(1000);
    CHECK_EQ(diag_state(), SB_STANDBY);
    CHECK_EQ(sw.offs, 0);
    /* policy bit3 = the stock button: 1 s from ON -> the stock real off, as without OpenGrand */
    static const uint8_t STOCK_BTN[] = {0x01, 0x10, 0x01, 0x09, 0x2C, 0x01, 0x60, 0x01, 0x00, 0x00, 0x00};
    boot(true);
    run_ms(100);
    r = op(2, 0x18, STOCK_BTN, sizeof STOCK_BTN, &n);
    CHECK(r && r[8] == 0 && r[11] == 0x09);
    pwc_diag(d);
    CHECK_EQ(d[48] & 0x08, 0);
    press_ms(1000);
    CHECK_EQ(sw.offs, 1);
    CHECK_EQ(stk_power_req, 2);
    CHECK_EQ(logged("cleanup"), 0);
    /* ... and it survives a power cycle (kvstore) */
    boot(false);
    run_ms(100);
    press_ms(1000);
    CHECK_EQ(sw.offs, 1);
    /* no standby hooks (old loader / partial attach): no clamp, the stock off */
    boot(true);
    pwc_hooks_attached(PW_HK_APO | PW_HK_KAWAI);
    run_ms(100);
    press_ms(1000);
    CHECK_EQ(sw.offs, 1);
    CHECK_EQ(diag_state(), SB_ON);
    /* the timer hook not running (payload restarted, not armed yet): the stock unchanged */
    boot(true);
    pwc_restart();
    sw.pressed = 1;
    for (int t = 0; t < 50; t++) {
        now_ms += 20;
        stock_20ms();
    }
    sw.pressed = 0;
    stock_20ms();
    CHECK_EQ(sw.offs, 1);
    /* a press found above the clamp when the hook arms is the stock's: no clamp, no standby */
    boot(true);
    pwc_restart();
    sw.pressed = 1;
    for (int t = 0; t < 20; t++) {
        now_ms += 20;
        stock_20ms();
    }
    CHECK_EQ(stk_power_req, 1);
    run_ms(500);                                       /* the engine loop arms the hook mid-press */
    sw.pressed = 0;
    run_ms(40);
    CHECK_EQ(sw.offs, 1);
    CHECK_EQ(diag_state(), SB_ON);
    /* release exactly on the 4 s edge: either the standby or the stock off, never neither */
    for (uint32_t ms = 3980; ms <= 4080; ms += 20) {
        boot(true);
        run_ms(100);
        press_ms(ms);
        CHECK((diag_state() == SB_STANDBY) != (sw.offs == 1));
    }
}

/* ---- TV wake ---- */

TEST(pwc_tv_wake_stream_start_wakes_connect_does_not)
{
    uint32_t n = 0;
    uint8_t enter[] = {0x01};
    uint8_t d[PW_DIAG_RAW];
    boot(true);
    /* the TV links up while ON and stays linked (no stream): no hold, APO -> standby, module on */
    bt_line("RING 0 00:11:22:33:44:55 19 A2DP");
    bt_line("CONNECT 0 A2DP 19");
    stk_a2dp_flag = 1;
    run_ms(15 * MIN_MS + 200);
    CHECK_EQ(diag_state(), SB_STANDBY);
    CHECK_EQ(logged("bt_off"), 0);
    CHECK_EQ(stk_bt_power, 1);
    pwc_diag(d);
    CHECK_EQ(d[4], BT_AUDIO_CONNECTED);
    /* a link alone never wakes: reconnects, codec lines */
    bt_line("NO CARRIER 0 ERROR 0");
    bt_line("CONNECT 1 A2DP 19");
    bt_line("A2DP CODEC SBC JOINT_STEREO 44100 BITPOOL 2-53");
    run_ms(10 * MIN_MS);
    CHECK_EQ(diag_state(), SB_STANDBY);
    CHECK_EQ(stk_gpio_out67 & 0x8800u, 0);
    /* the TV plays: STREAMING START -> wake (cause 5), amp back 100 ms later */
    fs.n = 0;
    bt_line("A2DP STREAMING START 1");
    run_ms(20);
    pwc_diag(d);
    CHECK_EQ(d[2], SB_WAKING);
    CHECK_EQ(d[7], SB_WAKE_BT_STREAM);
    CHECK_EQ(pwc_hp_pass(), 1);                   /* the stock's again */
    run_ms(120);
    CHECK_EQ(diag_state(), SB_ON);
    CHECK_EQ(stk_gpio_out67 & 0x8800u, 0x8800u);
    CHECK_EQ(logged("bt_on"), 0);
    static const uint8_t AWAKE[] = {0xF0, 0x7D, 0x43, 0x41, 0x37, 0x39, 0x59, 0x7F, 0x00, 0x00, 0xF7};
    CHECK(stock_host.last_tx_len == sizeof AWAKE && memcmp(stock_host.last_tx, AWAKE, sizeof AWAKE) == 0);
    /* after the wake the streaming hold owns the APO: 90 min on, STOP -> 15 min -> standby again */
    run_ms(90 * MIN_MS);
    CHECK_EQ(diag_state(), SB_ON);
    CHECK_EQ(expired_by_stock, 0);
    bt_line("A2DP STREAMING STOP 1");
    run_ms(14 * MIN_MS);
    CHECK_EQ(diag_state(), SB_ON);
    run_ms(MIN_MS + 200);
    CHECK_EQ(diag_state(), SB_STANDBY);
    CHECK_EQ(stk_bt_power, 1);
    /* a START already seen before the standby began does not wake; the next one does */
    boot(true);
    bt_line("A2DP STREAMING START 0");
    run_ms(20);
    const uint8_t *r = op(2, 0x19, enter, 1, &n);
    CHECK(r && r[8] == 0 && r[9] == SB_STANDBY);
    run_ms(MIN_MS);
    CHECK_EQ(diag_state(), SB_STANDBY);
    bt_line("A2DP STREAMING STOP 0");
    run_ms(MIN_MS);
    CHECK_EQ(diag_state(), SB_STANDBY);
    bt_line("A2DP STREAMING START 0");
    run_ms(20);
    CHECK_EQ(diag_state(), SB_WAKING);
}

TEST(pwc_tv_wake_off_by_policy_switches_the_module_off)
{
    /* flags 0x05: APO -> standby, standby switches BT audio off (ota_power behaviour) */
    static const uint8_t SET_BTOFF[] = {0x01, 0x10, 0x01, 0x05, 0x78, 0x00, 0x60, 0x01, 0x00, 0x00, 0x00};
    static const uint8_t GET[] = {0x00};
    uint32_t n = 0;
    uint8_t enter[] = {0x01}, wake[] = {0x02};
    uint8_t d[PW_DIAG_RAW];
    boot(true);
    const uint8_t *r = op(2, 0x18, SET_BTOFF, sizeof SET_BTOFF, &n);
    CHECK(r && r[8] == 0 && r[11] == 0x05);
    pwc_diag(d);
    CHECK_EQ(d[48], 0x09);                        /* module on, TV wake not armed, F-A1 armed */
    fs.n = 0;
    r = op(2, 0x19, enter, 1, &n);
    CHECK(r && r[8] == 0 && r[9] == SB_STANDBY);
    CHECK(fs.n >= 4 && !strcmp(fs.log[3], "bt_off"));
    CHECK_EQ(stk_bt_power, 0);
    bt_line("A2DP STREAMING START 0");            /* (cannot come with the module off; still no wake) */
    run_ms(200);
    CHECK_EQ(diag_state(), SB_STANDBY);
    op(2, 0x19, wake, 1, &n);
    run_ms(200);
    CHECK_EQ(diag_state(), SB_ON);
    CHECK_EQ(stk_bt_power, 1);
    CHECK_EQ(logged("bt_on"), 1);
    /* kept across a restart */
    boot(false);
    r = op(2, 0x18, GET, sizeof GET, &n);
    CHECK(r && r[11] == 0x05);
}

/* fill the ring with ignored lines until the write index reaches `to` (one line per pass) */
static void bt_fill_to(uint16_t to)
{
    while (stk_bt_rx_wr != to) {
        uint16_t room = (uint16_t)((to - stk_bt_rx_wr) & 0x7FF);
        if (room >= 34) {
            bt_line("AVRCP 0 PLAYBACK STATUS CHANGED");   /* 31 + CR LF = 33 B, ignored */
        } else {
            stk_bt_rx_ring[stk_bt_rx_wr] = '\n';
            stk_bt_rx_wr = (uint16_t)((stk_bt_rx_wr + 1) & 0x7FF);
            stk_bt_rx_rd = stk_bt_rx_wr;
        }
        run_ms(20);
    }
}

TEST(pwc_ring_reset_never_rereads_old_lines)
{
    uint32_t n = 0;
    uint8_t enter[] = {0x01};
    uint8_t d[PW_DIAG_RAW];
    /* (b) a stock reset seen only by the write index: lap 1 leaves START / STOP at 0x600, lap 2
     * stops at 0x100, then the assembler resets the ring (overflow) and the module writes on from 0.
     * Reading on from 0x100 would run over 0x600 again: a stale START, a false TV wake. */
    boot(true);
    bt_fill_to(0x600);
    bt_line("A2DP STREAMING START 0");
    run_ms(20);
    bt_line("A2DP STREAMING STOP 0");
    run_ms(20);
    bt_fill_to(0x100);
    const uint8_t *r = op(2, 0x19, enter, 1, &n);
    CHECK(r && r[8] == 0 && r[9] == SB_STANDBY);
    pwc_diag(d);
    uint8_t starts = d[46];
    CHECK_EQ(starts, 1);
    stk_bt_rx_wr = 0;                             /* FUN_40145C00 by the assembler */
    stk_bt_rx_rd = 0;
    bt_line("AVRCP 0 PLAYBACK STATUS CHANGED");
    run_ms(20);
    run_ms(MIN_MS);
    pwc_diag(d);
    CHECK_EQ(d[2], SB_STANDBY);
    CHECK_EQ(d[46], starts);
    CHECK_EQ(d[51], 1);                           /* one ring reset taken */
    /* lines after the reset are read: a real START wakes */
    bt_line("A2DP STREAMING START 0");
    run_ms(20);
    CHECK_EQ(diag_state(), SB_WAKING);
    run_ms(200);
    /* (a) the flag shows the reset: overflow bit, the assembler resets on its next turn */
    boot(true);
    bt_fill_to(0x700);
    bt_line("A2DP STREAMING START 0");
    run_ms(20);
    bt_line("A2DP STREAMING STOP 0");
    run_ms(20);
    bt_fill_to(0x780);
    op(2, 0x19, enter, 1, &n);
    stk_bt_rx_flag = 0x01;                        /* ISR: ring full */
    run_ms(20);
    stk_bt_rx_wr = 0;                             /* assembler: reset, flag 0 */
    stk_bt_rx_rd = 0;
    stk_bt_rx_flag = 0;
    bt_line("AVRCP 0 PLAYBACK STATUS CHANGED");
    run_ms(MIN_MS);
    pwc_diag(d);
    CHECK_EQ(d[2], SB_STANDBY);
    CHECK_EQ(d[46], 1);
    CHECK_EQ(d[51], 1);
    /* an ordinary wrap is not a reset */
    boot(true);
    bt_fill_to(0x7F0);
    bt_line("A2DP STREAMING START 0");            /* crosses the ring end */
    run_ms(20);
    pwc_diag(d);
    CHECK_EQ(d[4], BT_AUDIO_STREAMING);
    CHECK_EQ(d[51], 0);
}

TEST(pwc_uart1_latch_is_cleared)
{
    uint32_t n = 0;
    uint8_t enter[] = {0x01};
    uint8_t d[PW_DIAG_RAW];
    boot(true);
    bt_line("CONNECT 0 A2DP 19");
    op(2, 0x19, enter, 1, &n);
    /* UART error: the assembler reset the ring and latched the flag; the ISR drops every byte */
    stk_bt_rx_wr = 0;
    stk_bt_rx_rd = 0;
    stk_bt_rx_flag = 0x80;
    run_ms(400);
    CHECK_EQ(fs.rx_resets, 0);                    /* not before BT_UNLATCH_MS */
    pwc_diag(d);
    CHECK_EQ(d[48] & 0x04, 0x04);
    CHECK_EQ(d[49], 0x80);
    run_ms(200);
    CHECK_EQ(fs.rx_resets, 1);                    /* cleared with the stock reset */
    CHECK_EQ(stk_bt_rx_flag, 0);
    pwc_diag(d);
    CHECK_EQ(d[50], 1);
    run_ms(MIN_MS);
    CHECK_EQ(fs.rx_resets, 1);                    /* once */
    /* the lines flow again: the TV's START wakes */
    bt_line("A2DP STREAMING START 0");
    run_ms(20);
    CHECK_EQ(diag_state(), SB_WAKING);
    /* never while bytes are in the ring (the assembler clears it itself) or the module is off */
    boot(true);
    stk_bt_rx_flag = 0x80;
    stk_bt_rx_wr = 5;
    stk_bt_rx_rd = 0;
    run_ms(2000);
    CHECK_EQ(fs.rx_resets, 0);
    stk_bt_rx_rd = 5;
    stk_bt_power = 0;
    run_ms(2000);
    CHECK_EQ(fs.rx_resets, 0);
    stk_bt_power = 1;
    run_ms(600);
    CHECK_EQ(fs.rx_resets, 1);
}

/* ota_power3 "no cap": policy cap 0xFFFF. Long spans run the
 * stock 20 ms body and the engine loop once per second (the µs clock wraps every 71.6 min - many
 * times here - as on the piano). */
static void run_coarse(uint32_t ms)
{
    for (uint32_t t = 0; t < ms; t += 1000) {
        now_ms += 1000;
        stock_host.time_us = now_ms * 1000u;
        stock_20ms();
        pwc_tick();
    }
}

TEST(pwc_no_cap_keeps_standby_for_days)
{
    /* 01 01 78 00 FF FF 00 00 packed: hi bits 4 and 5 of the first group */
    static const uint8_t SET[] = {0x01, 0x30, 0x01, 0x01, 0x78, 0x00, 0x7F, 0x7F, 0x00, 0x00, 0x00};
    static const uint8_t GET[] = {0x00}, WAKE[] = {0x02};
    uint32_t n = 0;
    boot(true);
    const uint8_t *r = op(2, 0x18, SET, sizeof SET, &n);
    CHECK(r && n == 20 && r[7] == 0x01 && r[8] == 0 && memcmp(r + 9, SET + 1, 10) == 0);
    boot(false);   /* power cycle: the saved 0xFFFF is back */
    r = op(2, 0x18, GET, sizeof GET, &n);
    CHECK(r && n == 20 && r[8] == 0 && memcmp(r + 9, SET + 1, 10) == 0);
    run_ms(15 * MIN_MS + 100);
    CHECK_EQ(diag_state(), SB_STANDBY);
    run_coarse(7u * 24u * 60u * MIN_MS);   /* 7 days */
    CHECK_EQ(diag_state(), SB_STANDBY);
    CHECK_EQ(stk_power_req, 0);
    CHECK_EQ(expired_by_stock, 0);
    CHECK(stk_apo_counter <= 2);           /* held by the 20 ms hook the whole time */
    uint8_t d[PW_DIAG_RAW];
    pwc_diag(d);
    uint32_t sb_ms = (uint32_t)d[20] | (uint32_t)d[21] << 8 | (uint32_t)d[22] << 16 | (uint32_t)d[23] << 24;
    CHECK(sb_ms >= 7u * 24u * 60u * MIN_MS && sb_ms < 7u * 24u * 60u * MIN_MS + 2u * MIN_MS);
    /* the app wakes it */
    r = op(2, 0x19, WAKE, sizeof WAKE, &n);
    CHECK(r && r[8] == 0);
    run_ms(200);
    CHECK_EQ(diag_state(), SB_ON);
    CHECK_EQ(stk_gpio_out67 & 0x8800u, 0x8800u);
    /* 0xFFFE is still a bad value: rc 1, nothing changes */
    static const uint8_t BAD[] = {0x01, 0x30, 0x01, 0x01, 0x78, 0x00, 0x7E, 0x7F, 0x00, 0x00, 0x00};
    r = op(2, 0x18, BAD, sizeof BAD, &n);
    CHECK(r && r[8] == 1 && memcmp(r + 9, SET + 1, 10) == 0);
}

/* ---- feature 1: the hold with several sources, the cap as a setting ----------------------------- */
TEST(pwc_bt_hold_ignores_another_sources_stop_and_caps)
{
    uint32_t n = 0;
    boot(true);
    bt_line("CONNECT 0 A2DP 19");
    bt_line("CONNECT 2 A2DP 19");
    bt_line("A2DP STREAMING START 0");
    run_ms(10 * MIN_MS);
    bt_line("A2DP STREAMING STOP 2");   /* a refused second source: the playing one still holds */
    run_ms(40 * MIN_MS);
    CHECK_EQ(diag_state(), SB_ON);
    uint8_t d[PW_DIAG_RAW];
    pwc_diag(d);
    CHECK_EQ(d[4], BT_AUDIO_STREAMING);
    bt_line("A2DP STREAMING STOP 0");
    run_ms(15 * MIN_MS + 200);
    CHECK_EQ(diag_state(), SB_STANDBY);

    /* Windows / TV paused, stream open: hold 30 min (op 18 01, bytes 2..3 = 30), then the APO right away */
    static const uint8_t SET30[] = {0x01, 0x10, 0x01, 0x01, 0x1E, 0x00, 0x60, 0x01, 0x00, 0x00, 0x00};
    boot(true);
    const uint8_t *r = op(2, 0x18, SET30, sizeof SET30, &n);
    CHECK(r && r[8] == 0 && r[12] == 0x1E);
    bt_line("CONNECT 0 A2DP 19");
    bt_line("A2DP STREAMING START 0");
    run_ms(29 * MIN_MS);
    CHECK_EQ(diag_state(), SB_ON);
    pwc_diag(d);
    CHECK((d[3] & 0x18) == 0x08);       /* holding, not used up */
    run_ms(MIN_MS + 200);
    CHECK_EQ(diag_state(), SB_STANDBY);
    pwc_diag(d);
    CHECK(d[3] & 0x10);                 /* max hold used up */
    /* hold 0 = rule off (stock): the open stream does not hold at all */
    static const uint8_t SET0[] = {0x01, 0x10, 0x01, 0x01, 0x00, 0x00, 0x60, 0x01, 0x00, 0x00, 0x00};
    boot(true);
    r = op(2, 0x18, SET0, sizeof SET0, &n);
    CHECK(r && r[8] == 0);
    bt_line("A2DP STREAMING START 0");
    run_ms(15 * MIN_MS + 200);
    CHECK_EQ(diag_state(), SB_STANDBY);
}

/* ---- (owner decision 2026-11-03): the hold follows the streaming source's AVRCP play status ---------
 * The BT port's source manager runs too (btc_tick after pwc_tick, as the engine loop does) and reads the same ring. */
static void run_bt_ms(uint32_t ms)
{
    for (uint32_t t = 0; t < ms; t += 20) {
        now_ms += 20;
        stock_host.time_us = now_ms * 1000u;
        stock_20ms();
        pwc_tick();
        btc_tick();
    }
}

static void boot_bt(void)
{
    boot(true);
    btc_restart();
    run_bt_ms(20);   /* the source manager starts reading the ring here */
    bt_line("RING 3 22:82:02:02:92:22 19 A2DP");   /* a Windows PC: A2DP 3 / 5, AVRCP 4 */
    bt_line("RING 5 22:82:02:02:92:22 19 A2DP");
    bt_line("RING 4 22:82:02:02:92:22 17 AVRCP");
    bt_line("A2DP STREAMING START 3");
    run_bt_ms(1000);
}

/* a CHANGED notification, then the INTERIM answer to the registration the manager sends again (AN986 4.3) */
static void ps_changed(const char *word)
{
    char l[96];
    snprintf(l, sizeof l, "AVRCP 4 REGISTER_NOTIFICATION_RSP CHANGED PLAYBACK_STATUS_CHANGED %s", word);
    bt_line(l);
    run_bt_ms(500);
    snprintf(l, sizeof l, "AVRCP 4 REGISTER_NOTIFICATION_RSP INTERIM PLAYBACK_STATUS_CHANGED %s", word);
    bt_line(l);
}

static uint8_t diag_flags(void)
{
    uint8_t d[PW_DIAG_RAW];
    pwc_diag(d);
    return d[3];
}

TEST(pwc_bt_hold_follows_the_play_status)
{
    uint32_t n = 0;
    /* PLAYING holds; CHANGED PAUSED with the stream left open (Windows) ends the hold like a STOP: 15 min from the pause */
    boot_bt();
    bt_line("AVRCP 4 REGISTER_NOTIFICATION_RSP INTERIM PLAYBACK_STATUS_CHANGED PLAYING");
    run_bt_ms(60 * MIN_MS);
    CHECK_EQ(diag_state(), SB_ON);
    CHECK(diag_flags() & 0x08);
    CHECK(stk_apo_counter >= 45000u - 1u && stk_apo_counter <= 45000u);
    ps_changed("PAUSED");
    run_bt_ms(14 * MIN_MS);
    CHECK_EQ(diag_state(), SB_ON);
    CHECK(!(diag_flags() & 0x08));
    {
        uint8_t d[PW_DIAG_RAW];
        pwc_diag(d);
        CHECK_EQ(d[4], BT_AUDIO_STREAMING);   /* the stream is still open */
    }
    run_bt_ms(MIN_MS + 200);
    CHECK_EQ(diag_state(), SB_STANDBY);

    /* PLAYING again with the stream still open: a new 30 min window (op 18 01 hold 30) */
    static const uint8_t SET30[] = {0x01, 0x10, 0x01, 0x01, 0x1E, 0x00, 0x60, 0x01, 0x00, 0x00, 0x00};
    boot_bt();
    const uint8_t *r = op(2, 0x18, SET30, sizeof SET30, &n);
    CHECK(r && r[8] == 0);
    bt_line("AVRCP 4 REGISTER_NOTIFICATION_RSP INTERIM PLAYBACK_STATUS_CHANGED PLAYING");
    run_bt_ms(20 * MIN_MS);
    ps_changed("PAUSED");
    run_bt_ms(10 * MIN_MS);
    CHECK_EQ(diag_state(), SB_ON);
    CHECK(!(diag_flags() & 0x08));
    ps_changed("PLAYING");                    /* played again at 30 min, no new START */
    run_bt_ms(29 * MIN_MS);
    CHECK_EQ(diag_state(), SB_ON);
    CHECK((diag_flags() & 0x18) == 0x08);
    run_bt_ms(MIN_MS + 200);
    CHECK_EQ(diag_state(), SB_STANDBY);       /* the new window used up, idle: right away */

    /* STOPPED counts as paused */
    boot_bt();
    bt_line("AVRCP 4 REGISTER_NOTIFICATION_RSP INTERIM PLAYBACK_STATUS_CHANGED STOPPED");
    run_bt_ms(15 * MIN_MS + 200);
    CHECK_EQ(diag_state(), SB_STANDBY);

    /* no play status (no answer, REJ): the stream holds as before - up to the 5 h default */
    boot_bt();
    bt_line("AVRCP 4 REGISTER_NOTIFICATION_RSP REJ");
    run_bt_ms(4 * 60 * MIN_MS);
    CHECK_EQ(diag_state(), SB_ON);
    CHECK(diag_flags() & 0x08);
}

/* ---- feature 2: remote off 19 03 --------------------------------------------------------------------------------- */
static const uint8_t OFF[] = {0x03};

TEST(pwc_remote_off_from_any_bound_controller)
{
    uint32_t n = 0;
    const uint8_t *r;
    boot(true);
    /* the panel holds the stock binding; no session core: the app is not bound -> rc 2, nothing happens */
    stk_binding = 4;
    r = op(2, 0x19, OFF, 1, &n);
    CHECK(r && n == 12 && r[6] == 0x59 && r[7] == 0x03 && r[8] == 2 && r[9] == SB_ON && r[10] == 0);
    CHECK_EQ(logged("cleanup"), 0);
    /* the session core has the app bound too (not the stock binding): accepted -> standby like an APO expiry */
    rs.core = 0;
    rs.core_mask = 0x04;
    r = op(2, 0x19, OFF, 1, &n);
    CHECK(r && n == 12 && r[7] == 0x03 && r[8] == 0 && r[9] == SB_STANDBY && r[10] == 0);
    CHECK_EQ(stock_host.last_tx_port, 2);
    CHECK_EQ(diag_state(), SB_STANDBY);
    CHECK_EQ(logged("cleanup"), 1);
    CHECK_EQ(stk_gpio_out67 & 0x8800u, 0);
    CHECK_EQ(stk_power_req, 0);
    /* again in standby: rc 3 */
    r = op(2, 0x19, OFF, 1, &n);
    CHECK(r && r[8] == 3 && r[9] == SB_STANDBY);
    /* the same core rule for 19 01 / 18 01 */
    static const uint8_t WAKE[] = {0x02};
    r = op(2, 0x19, WAKE, 1, &n);
    run_ms(200);
    CHECK_EQ(diag_state(), SB_ON);
    static const uint8_t ENTER[] = {0x01};
    r = op(2, 0x19, ENTER, 1, &n);
    CHECK(r && r[8] == 0);
    /* the core says not bound (port 0): refused although a session core runs */
    r = op(0, 0x19, OFF, 1, &n);
    CHECK(r && r[8] == 2);
    uint8_t d[PW_DIAG_RAW];
    pwc_diag(d);
    CHECK_EQ(PW_DIAG_RAW, 60);   /* + the poll counters 54..59 (PWC_QUIET) */
    CHECK_EQ(d[52], 1);
    CHECK_EQ(d[53], 0);

    /* policy "power off" (flags 0): the real off through the stock executor */
    static const uint8_t SET_OFF[] = {0x01, 0x10, 0x01, 0x00, 0x78, 0x00, 0x60, 0x01, 0x00, 0x00, 0x00};
    boot(true);
    r = op(2, 0x18, SET_OFF, sizeof SET_OFF, &n);
    CHECK(r && r[8] == 0);
    r = op(2, 0x19, OFF, 1, &n);
    CHECK(r && r[8] == 0 && r[9] == SB_OFF);
    CHECK_EQ(stk_power_req, 2);
    CHECK_EQ(logged("amp_off"), 0);
}

/* the standby notice 59 7F 00 <state> goes to every controller bound() counts but the panel (stock port 3) - the app
 * hears it while the panel holds the stock binding */
static int notices_to(int port, uint8_t state)
{
    const uint8_t want[] = {0xF0, 0x7D, 0x43, 0x41, 0x37, 0x39, 0x59, 0x7F, 0x00, state, 0xF7};
    int c = 0;
    for (int i = 0; i < stock_host.log_n && i < STOCK_HOST_LOG; i++)
        c += stock_host.log_port[i] == port && stock_host.log_len[i] == sizeof want &&
             memcmp(stock_host.log[i], want, sizeof want) == 0;
    return c;
}

TEST(pwc_standby_notice_reaches_core_bound_controllers)
{
    uint32_t n = 0;
    static const uint8_t WAKE[] = {0x02};
    /* panel holds the stock binding, the session core has the app (BLE, 2), USB (1) and the panel bound */
    boot(true);
    stk_binding = 4;
    rs.core = 0;
    rs.core_mask = 0x0E;
    stock_host.log_n = 0;
    run_ms(15 * MIN_MS + 100);
    CHECK_EQ(diag_state(), SB_STANDBY);
    CHECK_EQ(notices_to(1, SB_STANDBY), 1);
    CHECK_EQ(notices_to(2, SB_STANDBY), 1);
    CHECK_EQ(notices_to(0, SB_STANDBY), 0);
    CHECK_EQ(notices_to(3, SB_STANDBY), 0);   /* never the panel */
    stock_host.log_n = 0;
    CHECK(op(1, 0x19, WAKE, 1, &n) != NULL);
    run_ms(200);
    CHECK_EQ(diag_state(), SB_ON);
    CHECK_EQ(notices_to(1, SB_ON), 1);
    CHECK_EQ(notices_to(2, SB_ON), 1);
    CHECK_EQ(notices_to(3, SB_ON), 0);

    /* no session core: the stock binding alone decides - the panel's binding reaches nobody (the old rule) */
    boot(true);
    stk_binding = 4;
    stock_host.log_n = 0;
    run_ms(15 * MIN_MS + 100);
    CHECK_EQ(diag_state(), SB_STANDBY);
    CHECK_EQ(notices_to(0, SB_STANDBY) + notices_to(1, SB_STANDBY) + notices_to(2, SB_STANDBY) +
             notices_to(3, SB_STANDBY), 0);
    /* ... and the app's binding reaches the app only */
    boot(true);
    stock_host.log_n = 0;
    run_ms(15 * MIN_MS + 100);
    CHECK_EQ(notices_to(2, SB_STANDBY), 1);
    CHECK_EQ(notices_to(1, SB_STANDBY), 0);
    CHECK_EQ(notices_to(3, SB_STANDBY), 0);
}

TEST(pwc_remote_off_refused_while_recording_file_op_or_flash)
{
    uint32_t n = 0;
    const uint8_t *r;
    boot(true);
    for (uint8_t stt = 3; stt <= 6; stt++) {   /* armed, recording, armed (no metronome), count-in */
        rs.rec_state = stt;
        r = op(2, 0x19, OFF, 1, &n);
        CHECK(r && r[8] == 6 && r[9] == SB_ON && r[10] == POWER_OFF_RECORDING);
    }
    CHECK_EQ(logged("cleanup"), 0);
    rs.rec_state = 2;
    rs.fileop = 3;
    r = op(2, 0x19, OFF, 1, &n);
    CHECK(r && r[8] == 6 && r[10] == POWER_OFF_FILE);
    rs.fileop = 0;
    rs.job1 = 1;                               /* RecorderData job queued */
    r = op(2, 0x19, OFF, 1, &n);
    CHECK(r && r[8] == 6 && r[10] == POWER_OFF_FLASH);
    rs.job1 = 0;
    fs.busy = 1;                               /* a stock writer owns the chip */
    r = op(2, 0x19, OFF, 1, &n);
    CHECK(r && r[8] == 6 && r[10] == POWER_OFF_FLASH);
    fs.busy = 0;
    CHECK_EQ(diag_state(), SB_ON);
    CHECK_EQ(stk_power_req, 0);
    uint8_t d[PW_DIAG_RAW];
    pwc_diag(d);
    CHECK_EQ(d[53], 7);
    /* playing a song is no reason: stopped by the standby cleanup */
    rs.rec_state = 1;
    r = op(2, 0x19, OFF, 1, &n);
    CHECK(r && r[8] == 0 && r[9] == SB_STANDBY);
}

/* ---- polls are not activity ---- */

/* the stock byte parser FUN_40148B24 for the bytes [from, upto) of one frame on a stock port: every byte < 0xF8 resets
 * the counter (FUN_400ACB84); F0 opens the assembly, F7 ends it and the end handler hands the frame on (Kawai: the
 * 0x40147E94 stub's peek, 0x7D: the payload's ng_sysex) */
static void parse_bytes(uint8_t port, const uint8_t *m, uint32_t from, uint32_t upto)
{
    for (uint32_t i = from; i < upto; i++) {
        stock_activity();
        if (m[i] == 0xF0) {
            stk_sx_state[port] = 0xF0;
            stk_sx_len[port] = 1;
        } else if (m[i] == 0xF7) {
            stk_sx_len[port]++;
            stk_sx_state[port] = 0xF7;
            if (m[1] == 0x40)
                pwc_kawai_peek(port, m, i + 1);
            else if (m[1] == 0x7D)
                pwc_frame_7d(m, i + 1);
            stk_sx_state[port] = 0;
        } else {
            stk_sx_len[port]++;
        }
    }
}

/* one MIDI pump pass with this frame, then the pump tail */
static void pump_pass(uint8_t port, const uint8_t *m, uint32_t n)
{
    parse_bytes(port, m, 0, n);
    pwc_midi_tail();
}

static const uint8_t F_1B03[] = {0xF0, 0x7D, 0x43, 0x41, 0x37, 0x39, 0x1B, 0x03, 0xF7};
static const uint8_t F_1D04[] = {0xF0, 0x7D, 0x43, 0x41, 0x37, 0x39, 0x1D, 0x04, 0xF7};
static const uint8_t F_1A[] = {0xF0, 0x7D, 0x43, 0x41, 0x37, 0x39, 0x1A, 0xF7};
static const uint8_t F_GET[] = {0xF0, 0x40, 0x7F, 0x02, 0x08, 0x02, 0x51, 0x00, 0x7F, 0xF7};
static const uint8_t F_ACK[] = {0xF0, 0x40, 0x7F, 0x07, 0x08, 0x02, 0x51, 0x00, 0x7F, 0xF7};
static const uint8_t F_SET[] = {0xF0, 0x40, 0x7F, 0x10, 0x08, 0x02, 0x55, 0x01, 0x00, 0x40, 0xF7};
static const uint8_t F_WAKE[] = {0xF0, 0x7D, 0x43, 0x41, 0x37, 0x39, 0x19, 0x02, 0xF7};

TEST(power_frame_quiet_tells_polls_from_activity)
{
    static const uint8_t connect[] = {0xF0, 0x40, 0x7F, 0x7E, 0x08, 0x02, 0x01, 0x01, 0xF7};
    static const uint8_t ver[] = {0xF0, 0x40, 0x7F, 0x7E, 0x08, 0x02, 0x04, 0x41, 0xF7};
    static const uint8_t touch[] = {0xF0, 0x40, 0x7F, 0x32, 0x08, 0x02, 0x51, 0x39, 0x00, 0x7F, 0xF7};
    static const uint8_t cmd31[] = {0xF0, 0x40, 0x7F, 0x31, 0x08, 0x02, 0x52, 0x00, 0x00, 0x01, 0xF7};
    static const uint8_t ident[] = {0xF0, 0x7D, 0x43, 0x41, 0x37, 0x39, 0x01, 0xF7};
    static const uint8_t pol_get[] = {0xF0, 0x7D, 0x43, 0x41, 0x37, 0x39, 0x18, 0x00, 0xF7};
    static const uint8_t pol_set[] = {0xF0, 0x7D, 0x43, 0x41, 0x37, 0x39, 0x18, 0x02, 0xF7};
    static const uint8_t sb_get[] = {0xF0, 0x7D, 0x43, 0x41, 0x37, 0x39, 0x19, 0x00, 0xF7};
    static const uint8_t off[] = {0xF0, 0x7D, 0x43, 0x41, 0x37, 0x39, 0x19, 0x03, 0xF7};
    static const uint8_t fr_on[] = {0xF0, 0x7D, 0x43, 0x41, 0x37, 0x39, 0x1B, 0x01, 0x01, 0xF7};
    static const uint8_t beat[] = {0xF0, 0x7D, 0x43, 0x41, 0x37, 0x39, 0x1D, 0x0F, 0xF7};
    static const uint8_t log08[] = {0xF0, 0x7D, 0x43, 0x41, 0x37, 0x39, 0x1D, 0x08, 0x00, 0x10, 0xF7};
    static const uint8_t pol6r[] = {0xF0, 0x7D, 0x43, 0x41, 0x37, 0x39, 0x1D, 0x06, 0xF7};
    static const uint8_t pol6w[] = {0xF0, 0x7D, 0x43, 0x41, 0x37, 0x39, 0x1D, 0x06, 2, 0, 3, 30, 0, 0, 0, 0, 0, 0, 0, 0, 0xF7};
    static const uint8_t src5[] = {0xF0, 0x7D, 0x43, 0x41, 0x37, 0x39, 0x1D, 0x05, 0x01, 0, 1, 2, 3, 4, 5, 6, 7, 0xF7};
    static const uint8_t ac0[] = {0xF0, 0x7D, 0x43, 0x41, 0x37, 0x39, 0x1D, 0x09, 0x00, 0xF7};
    static const uint8_t ac2[] = {0xF0, 0x7D, 0x43, 0x41, 0x37, 0x39, 0x1D, 0x09, 0x02, 0xF7};
    static const uint8_t co1[] = {0xF0, 0x7D, 0x43, 0x41, 0x37, 0x39, 0x1D, 0x0A, 0x01, 0xF7};
    static const uint8_t co2[] = {0xF0, 0x7D, 0x43, 0x41, 0x37, 0x39, 0x1D, 0x0A, 0x02, 0xF7};
    static const uint8_t av0[] = {0xF0, 0x7D, 0x43, 0x41, 0x37, 0x39, 0x1D, 0x0C, 0xF7};
    static const uint8_t av1[] = {0xF0, 0x7D, 0x43, 0x41, 0x37, 0x39, 0x1D, 0x0C, 0x01, 0xF7};
    static const uint8_t av2[] = {0xF0, 0x7D, 0x43, 0x41, 0x37, 0x39, 0x1D, 0x0C, 0x02, 0xF7};
    static const uint8_t av3[] = {0xF0, 0x7D, 0x43, 0x41, 0x37, 0x39, 0x1D, 0x0C, 0x03, 0xF7};
    static const uint8_t au0[] = {0xF0, 0x7D, 0x43, 0x41, 0x37, 0x39, 0x1D, 0x0D, 0xF7};
    static const uint8_t au1[] = {0xF0, 0x7D, 0x43, 0x41, 0x37, 0x39, 0x1D, 0x0D, 0x01, 0xF7};
    static const uint8_t au2[] = {0xF0, 0x7D, 0x43, 0x41, 0x37, 0x39, 0x1D, 0x0D, 0x02, 0x01, 0xF7};
    static const uint8_t au3[] = {0xF0, 0x7D, 0x43, 0x41, 0x37, 0x39, 0x1D, 0x0D, 0x03, 0xF7};
    static const uint8_t lvl[] = {0xF0, 0x7D, 0x43, 0x41, 0x37, 0x39, 0x1D, 0x0B, 0xF7};   /* 1D 0B level: a read */
    static const uint8_t rec_st[] = {0xF0, 0x7D, 0x43, 0x41, 0x37, 0x39, 0x1E, 0x01, 0xF7};
    static const uint8_t rec_cmd[] = {0xF0, 0x7D, 0x43, 0x41, 0x37, 0x39, 0x1E, 0x02, 0x01, 0x00, 0x00, 0xF7};
    static const uint8_t su_get[] = {0xF0, 0x7D, 0x43, 0x41, 0x37, 0x39, 0x1C, 0xF7};
    static const uint8_t su_set[] = {0xF0, 0x7D, 0x43, 0x41, 0x37, 0x39, 0x1C, 0x01, 0x01, 0xF7};
    static const uint8_t relay[] = {0xF0, 0x7D, 0x43, 0x41, 0x37, 0x39, 0x31, 0x00, 0xF7};
    static const uint8_t relay_r[] = {0xF0, 0x7D, 0x43, 0x41, 0x37, 0x39, 0x71, 0x00, 0xF7};
    static const uint8_t universal[] = {0xF0, 0x7E, 0x7F, 0x06, 0x01, 0xF7};
    static const uint8_t fa_list[] = {0xF0, 0x40, 0x7F, 0x24, 0x08, 0x02, 0x11, 0x00, 0xF7};
    static const uint8_t fa_ack[] = {0xF0, 0x40, 0x7F, 0x24, 0x08, 0x02, 0x7F, 0x00, 0xF7};
    static const uint8_t fa_save[] = {0xF0, 0x40, 0x7F, 0x24, 0x08, 0x02, 0x0A, 0x00, 0x00, 0xF7};
    /* the GPIO diagnostic 0x17: every form is a read, notices on / off included */
    static const uint8_t gp_snap[] = {0xF0, 0x7D, 0x43, 0x41, 0x37, 0x39, 0x17, 0xF7};
    static const uint8_t gp_log[] = {0xF0, 0x7D, 0x43, 0x41, 0x37, 0x39, 0x17, 0x01, 0xF7};
    static const uint8_t gp_on[] = {0xF0, 0x7D, 0x43, 0x41, 0x37, 0x39, 0x17, 0x02, 0x01, 0xF7};
    CHECK(power_frame_quiet(gp_snap, sizeof gp_snap));
    CHECK(power_frame_quiet(gp_log, sizeof gp_log));
    CHECK(power_frame_quiet(gp_on, sizeof gp_on));
    CHECK(power_frame_quiet(lvl, sizeof lvl));
    CHECK(power_frame_quiet(fa_list, sizeof fa_list));
    CHECK(power_frame_quiet(fa_ack, sizeof fa_ack));
    CHECK(!power_frame_quiet(fa_save, sizeof fa_save));
    CHECK(power_frame_quiet(F_GET, sizeof F_GET));
    CHECK(power_frame_quiet(F_ACK, sizeof F_ACK));
    CHECK(power_frame_quiet(connect, sizeof connect));
    CHECK(power_frame_quiet(ver, sizeof ver));
    CHECK(!power_frame_quiet(F_SET, sizeof F_SET));
    CHECK(!power_frame_quiet(touch, sizeof touch));
    CHECK(!power_frame_quiet(cmd31, sizeof cmd31));
    CHECK(power_frame_quiet(ident, sizeof ident));
    CHECK(power_frame_quiet(F_1A, sizeof F_1A));
    CHECK(power_frame_quiet(F_1B03, sizeof F_1B03));
    CHECK(power_frame_quiet(F_1D04, sizeof F_1D04));
    CHECK(power_frame_quiet(pol_get, sizeof pol_get));
    CHECK(!power_frame_quiet(pol_set, sizeof pol_set));
    CHECK(power_frame_quiet(sb_get, sizeof sb_get));
    CHECK(!power_frame_quiet(F_WAKE, sizeof F_WAKE));
    CHECK(!power_frame_quiet(off, sizeof off));
    CHECK(!power_frame_quiet(fr_on, sizeof fr_on));
    CHECK(power_frame_quiet(beat, sizeof beat));
    CHECK(power_frame_quiet(log08, sizeof log08));
    CHECK(power_frame_quiet(pol6r, sizeof pol6r));
    CHECK(!power_frame_quiet(pol6w, sizeof pol6w));
    CHECK(!power_frame_quiet(src5, sizeof src5));
    CHECK(power_frame_quiet(ac0, sizeof ac0));
    CHECK(!power_frame_quiet(ac2, sizeof ac2));
    CHECK(power_frame_quiet(co1, sizeof co1));     /* class of device: state / read are not activity, a write is */
    CHECK(!power_frame_quiet(co2, sizeof co2));
    CHECK(power_frame_quiet(av0, sizeof av0));     /* AVRCP features: state / read are not activity, write / restore are */
    CHECK(power_frame_quiet(av1, sizeof av1));
    CHECK(!power_frame_quiet(av2, sizeof av2));
    CHECK(!power_frame_quiet(av3, sizeof av3));
    CHECK(power_frame_quiet(au0, sizeof au0));     /* audio routing: state / read are not activity, write / restore are */
    CHECK(power_frame_quiet(au1, sizeof au1));
    CHECK(!power_frame_quiet(au2, sizeof au2));
    CHECK(!power_frame_quiet(au3, sizeof au3));
    CHECK(power_frame_quiet(rec_st, sizeof rec_st));
    CHECK(!power_frame_quiet(rec_cmd, sizeof rec_cmd));
    CHECK(power_frame_quiet(su_get, sizeof su_get));
    CHECK(!power_frame_quiet(su_set, sizeof su_set));
    CHECK(!power_frame_quiet(relay, sizeof relay));
    CHECK(power_frame_quiet(relay_r, sizeof relay_r));
    CHECK(!power_frame_quiet(universal, sizeof universal));
    CHECK(!power_frame_quiet(F_GET, 3));
}

/* field 2026-11-02: an app left connected polls 1B 03 / 1D 04 / 1A / Kawai gets, the panel ACKs - the APO still runs
 * out at the menu time; a real set resets it; a frame split over two pump passes; a key in the same pass counts */
TEST(pwc_polls_do_not_keep_the_piano_on)
{
    uint8_t d[PW_DIAG_RAW];
    uint32_t n = 0;
    static const uint8_t wake[] = {0x02};
    boot(true);
    uint32_t t = 0, entered = 0;
    while (t < 20u * MIN_MS && !entered) {
        pump_pass(2, F_1D04, sizeof F_1D04);
        if (t % 10000u == 0)
            pump_pass(2, F_1B03, sizeof F_1B03);
        if (t % 4000u == 0) {
            pump_pass(2, F_1A, sizeof F_1A);
            pump_pass(2, F_GET, sizeof F_GET);
            pump_pass(3, F_ACK, sizeof F_ACK);   /* the panel ACKs a notice */
        }
        run_ms(2000);
        t += 2000;
        if (diag_state() == SB_STANDBY)
            entered = t;
    }
    CHECK(entered >= 15u * MIN_MS - 4000u && entered <= 15u * MIN_MS + 4000u);
    pwc_diag(d);
    CHECK(d[54] | d[55]);                      /* resets undone */
    CHECK((d[56] | (d[57] << 8)) > 400);       /* quiet frames */
    CHECK_EQ(d[58] | (d[59] << 8), 0);         /* no activity frame */
    /* wake (19 02 is activity), then a real set resets the counter and is not undone */
    pump_pass(2, F_WAKE, sizeof F_WAKE);
    op(2, 0x19, wake, 1, &n);
    run_ms(2000);
    CHECK_EQ(diag_state(), SB_ON);
    run_ms(5u * MIN_MS);
    CHECK(stk_apo_counter > 14000u);
    pump_pass(2, F_SET, sizeof F_SET);
    CHECK_EQ(stk_apo_counter, 1);
    pwc_diag(d);
    CHECK_EQ(d[58] | (d[59] << 8), 2);         /* 19 02 + the set */
    /* a poll split over two pump passes: both undone */
    run_ms(MIN_MS);
    uint32_t c0 = stk_apo_counter;
    parse_bytes(2, F_1D04, 0, 4);
    pwc_midi_tail();
    CHECK(stk_apo_counter >= c0);
    run_ms(20);
    parse_bytes(2, F_1D04, 4, sizeof F_1D04);
    pwc_midi_tail();
    CHECK(stk_apo_counter >= c0 + 1);
    /* a set split over two passes: the first half undone, its end (F7) resets for good */
    parse_bytes(2, F_SET, 0, 5);
    pwc_midi_tail();
    CHECK(stk_apo_counter >= c0);
    parse_bytes(2, F_SET, 5, sizeof F_SET);
    pwc_midi_tail();
    CHECK_EQ(stk_apo_counter, 1);
    /* a key down at the tail: the reset of that pass stays (the key scan runs in another task) */
    run_ms(MIN_MS);
    stk_key_state[40] = 0xE0;
    pump_pass(2, F_1D04, sizeof F_1D04);
    CHECK_EQ(stk_apo_counter, 1);
    stk_key_state[40] = 0;
    run_ms(MIN_MS);
    pump_pass(2, F_1D04, sizeof F_1D04);   /* released since the last tail: still counted */
    CHECK_EQ(stk_apo_counter, 1);
    /* a pass with no classified frame (channel bytes, another maker's SysEx) is activity */
    run_ms(MIN_MS);
    stock_activity();
    pwc_midi_tail();
    CHECK_EQ(stk_apo_counter, 1);
    /* a stale open frame (no new bytes) does not hide a reset */
    run_ms(MIN_MS);
    stk_sx_state[1] = 0xF0;
    stk_sx_len[1] = 3;
    pwc_midi_tail();
    run_ms(MIN_MS);
    stock_activity();
    pwc_midi_tail();
    CHECK_EQ(stk_apo_counter, 1);
    stk_sx_state[1] = 0;
}

/* the BT streaming hold stops only the expiry: the stock counter keeps counting (polls are undone at the pump tail as
 * outside the hold), it rests at the limit, and after STOP the full APO time runs */
TEST(pwc_polls_leave_the_stream_hold_alone)
{
    boot(true);
    stk_a2dp_flag = 1;
    bt_line("CONNECT 0 A2DP 19");
    bt_line("A2DP STREAMING START 0");
    for (int i = 0; i < 60 * 10; i++) {   /* 20 virtual minutes, a poll every 2 s */
        pump_pass(2, F_1D04, sizeof F_1D04);
        run_ms(2000);
    }
    CHECK_EQ(diag_state(), SB_ON);
    CHECK(stk_apo_counter >= 45000u - 1u && stk_apo_counter <= 45000u);
    bt_line("A2DP STREAMING STOP 0");
    for (int i = 0; i < 60 * 9 && diag_state() == SB_ON; i++) {
        pump_pass(2, F_1D04, sizeof F_1D04);
        run_ms(2000);
    }
    CHECK_EQ(diag_state(), SB_STANDBY);
}

/* ---- APO test shortcut 19 04 ---- */
static const uint8_t F_APO60[] = {0xF0, 0x7D, 0x43, 0x41, 0x37, 0x39, 0x19, 0x04, 0x00, 0x3C, 0xF7};

/* 19 04 <secs>: as the app sends it - through the byte parser (it is activity) and to the engine loop */
static const uint8_t *apo_test(uint8_t port, uint32_t secs, uint32_t *n)
{
    const uint8_t a[3] = {0x04, (uint8_t)((secs >> 7) & 0x7F), (uint8_t)(secs & 0x7F)};
    uint8_t m[11];
    memcpy(m, HEAD, 6);
    m[6] = 0x19;
    memcpy(m + 7, a, 3);
    m[10] = 0xF7;
    parse_bytes(port, m, 0, sizeof m);
    pwc_midi_tail();
    return op(port, 0x19, a, 3, n);
}

/* run with an app poll every 2 s until standby (or the limit): ms from now, 0 = never */
static uint32_t polled_until_standby(uint32_t limit_ms)
{
    for (uint32_t t = 0; t < limit_ms; t += 2000) {
        pump_pass(2, F_1D04, sizeof F_1D04);
        run_ms(2000);
        if (diag_state() == SB_STANDBY)
            return t + 2000;
    }
    return 0;
}

TEST(pwc_apo_test_shortcut)
{
    uint32_t n = 0, t;
    const uint8_t *r;
    static const uint8_t wake[] = {0x02};
    boot(true);
    CHECK(!power_frame_quiet(F_APO60, sizeof F_APO60));   /* a command: activity */
    /* 60 s: reply 59 04 00 <ON> 00 3C <menu 1> <not held>; the counter jumps to limit - 3000 on the next 20 ms tick */
    r = apo_test(2, 60, &n);
    static const uint8_t want[] = {0x04, 0x00, SB_ON, 0x00, 0x3C, 0x01, 0x00};
    CHECK(r && n == 15 && r[6] == 0x59 && memcmp(r + 7, want, sizeof want) == 0);
    run_ms(20);
    CHECK_EQ(stk_apo_limits[1] - stk_apo_counter, 3000u - 1u);
    /* the app polls every 2 s: the countdown is not reset, standby at 60 s (the policy's APO action) */
    t = polled_until_standby(5u * MIN_MS);
    CHECK(t >= 58000u && t <= 62000u);
    CHECK_EQ(stk_power_req, 0);
    CHECK_EQ(logged("cleanup"), 1);
    /* nothing saved: no NOR erase / program (RAM only) */
    CHECK_EQ(fs.erases + fs.programs, 0);
    /* in standby: rc 3, nothing moves */
    r = apo_test(2, 60, &n);
    CHECK(r && r[8] == PW_BAD_STATE && r[9] == SB_STANDBY);
    op(2, 0x19, wake, 1, &n);
    run_ms(2000);
    CHECK_EQ(diag_state(), SB_ON);

    /* activity during the countdown (a fn 10 set at 30 s) gives the full menu time back */
    r = apo_test(2, 60, &n);
    CHECK(r && r[8] == PW_OK);
    t = polled_until_standby(30000);
    CHECK_EQ(t, 0);
    pump_pass(2, F_SET, sizeof F_SET);
    CHECK_EQ(stk_apo_counter, 1);
    t = polled_until_standby(14u * MIN_MS);
    CHECK_EQ(t, 0);
    CHECK_EQ(diag_state(), SB_ON);
    /* 19 04 00 (s = 0) puts the full menu time back too */
    r = apo_test(2, 30, &n);
    CHECK(r && r[8] == PW_OK);
    run_ms(10000);
    {
        const uint8_t a[2] = {0x04, 0x00};
        r = op(2, 0x19, a, 2, &n);
        CHECK(r && r[8] == PW_OK && r[10] == 0 && r[11] == 0);
    }
    run_ms(20);
    CHECK(stk_apo_counter <= 3);
    t = polled_until_standby(MIN_MS);
    CHECK_EQ(t, 0);

    /* the BT stream hold holds the countdown; after STOP the full menu time */
    r = apo_test(2, 60, &n);
    CHECK(r && r[8] == PW_OK);
    run_ms(10000);
    stk_a2dp_flag = 1;
    bt_line("CONNECT 0 A2DP 19");
    bt_line("A2DP STREAMING START 0");
    t = polled_until_standby(3u * MIN_MS);
    CHECK_EQ(t, 0);
    CHECK(stk_apo_counter >= 45000u - 1u && stk_apo_counter <= 45000u);   /* expired under the hold: resting at the limit */
    /* asked while held: accepted (flags bit 0), the counter moves but the hold still stops the expiry */
    r = apo_test(2, 20, &n);
    CHECK(r && r[8] == PW_OK && r[13] == 0x01);
    run_ms(MIN_MS);
    CHECK_EQ(diag_state(), SB_ON);
    bt_line("A2DP STREAMING STOP 0");
    t = polled_until_standby(16u * MIN_MS);
    CHECK(t >= 15u * MIN_MS - 4000u && t <= 15u * MIN_MS + 4000u);

    /* refusals: range (5 s, 601 s), unbound port, APO Off / special mode (rc 7) - the counter untouched */
    boot(true);
    run_ms(MIN_MS);
    uint32_t c0 = stk_apo_counter;
    r = apo_test(2, 5, &n);
    CHECK(r && r[8] == PW_BAD_VALUE);
    r = apo_test(2, 601, &n);
    CHECK(r && r[8] == PW_BAD_VALUE && r[10] == 0x04 && r[11] == 0x59);
    stk_binding = 4;
    r = apo_test(2, 60, &n);
    CHECK(r && r[8] == PW_NOT_BOUND);
    stk_binding = 3;
    stk_apo_setting = 0;
    r = apo_test(2, 60, &n);
    CHECK(r && r[8] == PW_APO_OFF && r[12] == 0);
    stk_apo_setting = 1;
    stk_special_mode = 1;
    r = apo_test(2, 60, &n);
    CHECK(r && r[8] == PW_APO_OFF);
    stk_special_mode = 0;
    run_ms(100);
    CHECK(stk_apo_counter < 100u);   /* only the frames' own activity resets, no jump */
    (void)c0;
    /* 600 s on the 60 min menu: limit - counter = 30000 */
    stk_apo_setting = 2;
    r = apo_test(2, 600, &n);
    CHECK(r && r[8] == PW_OK && r[12] == 2);
    run_ms(20);
    CHECK_EQ(stk_apo_limits[2] - stk_apo_counter, 30000u - 1u);
    CHECK_EQ(diag_state(), SB_ON);
}

/* field 2026-11-15: the engine loop answers 19 04 and the timer moves the counter while task 19
 * is still in the pump pass that carried the 19 04 frame; more bytes of that pass (a poll) reset the counter after the
 * move.  The tail must keep the move (the pass's only activity was the request itself), not take the reset as activity. */
TEST(pwc_apo_test_survives_a_reset_in_its_own_pump_pass)
{
    uint32_t n = 0;
    const uint8_t a[3] = {0x04, 0x00, 0x3C};
    uint8_t m[11];
    boot(true);
    for (int i = 0; i < 25; i++) {   /* 50 s connected, the app polling: the counter counts from the last activity */
        pump_pass(2, F_1A, sizeof F_1A);
        run_ms(2000);
    }
    CHECK(stk_apo_counter > 2000u);
    memcpy(m, HEAD, 6);
    m[6] = 0x19;
    memcpy(m + 7, a, 3);
    m[10] = 0xF7;
    parse_bytes(2, m, 0, sizeof m);           /* the 19 04 frame ends ... */
    const uint8_t *r = op(2, 0x19, a, 3, &n); /* ... task 24 answers before task 19 reaches the tail */
    CHECK(r && r[8] == PW_OK);
    run_ms(20);                               /* the 20 ms hook moves the counter */
    CHECK_EQ(stk_apo_limits[1] - stk_apo_counter, 3000u - 1u);
    parse_bytes(2, F_1A, 0, sizeof F_1A);     /* a poll in the same pass resets it */
    CHECK_EQ(stk_apo_counter, 1);
    pwc_midi_tail();
    CHECK(stk_apo_limits[1] - stk_apo_counter <= 3000u);   /* the move stands */
    CHECK_EQ(polled_until_standby(5u * MIN_MS) / 2000u, 30u); /* standby 60 s after the request (+- one 2 s step) */
    /* an activity frame (a real set) after the move in the same pass still wins: the full menu time */
    pump_pass(2, F_WAKE, sizeof F_WAKE);
    {
        const uint8_t w[1] = {0x02};
        op(2, 0x19, w, 1, &n);
    }
    run_ms(2000);
    CHECK_EQ(diag_state(), SB_ON);
    parse_bytes(2, m, 0, sizeof m);
    r = op(2, 0x19, a, 3, &n);
    CHECK(r && r[8] == PW_OK);
    run_ms(20);
    CHECK(stk_apo_limits[1] - stk_apo_counter < 3000u);
    parse_bytes(2, F_SET, 0, sizeof F_SET);
    pwc_midi_tail();
    CHECK_EQ(stk_apo_counter, 1);
    CHECK_EQ(polled_until_standby(2u * MIN_MS), 0u);
}

void suite_power_cave(void)
{
    printf("power cave port\n");
    RUN(pwc_policy_ops_match_the_host_core);
    RUN(pwc_kvstore_erases_only_unused_3_and_only_when_needed);
    RUN(pwc_busy_flash_at_boot_never_formats_the_store);
    RUN(pwc_apo_expiry_enters_standby_without_the_stock_off);
    RUN(pwc_real_off_policy_waits_for_the_flash);
    RUN(pwc_bt_streaming_holds_apo_up_to_the_max_hold);
    RUN(pwc_standby_blocks_relight_and_wakes);
    RUN(pwc_wake_with_headphones_or_alarm);
    RUN(pwc_needs_the_hooks_and_quiesces_awake);
    RUN(pwc_diag_op);
    RUN(pwc_tv_wake_stream_start_wakes_connect_does_not);
    RUN(pwc_tv_wake_off_by_policy_switches_the_module_off);
    RUN(pwc_ring_reset_never_rereads_old_lines);
    RUN(pwc_uart1_latch_is_cleared);
    RUN(pwc_no_cap_keeps_standby_for_days);
    RUN(pwc_bt_hold_ignores_another_sources_stop_and_caps);
    RUN(pwc_bt_hold_follows_the_play_status);
    RUN(pwc_remote_off_from_any_bound_controller);
    RUN(pwc_standby_notice_reaches_core_bound_controllers);
    RUN(pwc_remote_off_refused_while_recording_file_op_or_flash);
    RUN(power_frame_quiet_tells_polls_from_activity);
    RUN(pwc_polls_do_not_keep_the_piano_on);
    RUN(pwc_polls_leave_the_stream_hold_alone);
    RUN(pwc_apo_test_shortcut);
    RUN(pwc_apo_test_survives_a_reset_in_its_own_pump_pass);
    RUN(pwc_power_button_standby_and_long_press_off);
}
