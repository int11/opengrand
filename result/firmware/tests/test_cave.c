/* Tests for the cave port (ports/cave): hal.h bound to the stock firmware, the dormant
 * core instance and its self-test, and the cave's C runtime.  The stock routines are the
 * host stand-ins of ports/cave/stock_host.c. */
#include <stdint.h>
#include <stdlib.h>

#include "../ports/cave/core_cave.h"
#include "../ports/cave/hal_cave.h"
#include "../ports/cave/stock_host.h"
#include "test.h"

/* cave_rt.c built with CAVE_RT_HOST */
void *cave_rt_memcpy(void *dst, const void *src, size_t n);
void *cave_rt_memset(void *dst, int c, size_t n);
int cave_rt_memcmp(const void *a, const void *b, size_t n);
size_t cave_rt_strlen(const char *s);
char *cave_rt_strncpy(char *dst, const char *src, size_t n);
unsigned cave_rt___aeabi_uidiv(unsigned num, unsigned den);
unsigned long long cave_rt___aeabi_uidivmod(unsigned num, unsigned den);

static void unpack7(const uint8_t *in, size_t n, uint8_t *out)
{
    size_t i = 0, o = 0;
    while (i < n) {
        uint8_t hi = in[i++];
        for (int k = 0; k < 7 && i < n; k++)
            out[o++] = (uint8_t)(in[i++] | ((hi >> k & 1) ? 0x80 : 0));
    }
}

static uint32_t le32(const uint8_t *p) { return (uint32_t)p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24; }

TEST(cave_port_map_is_a_bijection)
{
    int seen[4] = {0};
    for (int p = 0; p < MIDI_PORT_INTERNAL; p++) {
        int s = cave_stock_port((midi_port_t)p);
        CHECK(s >= 0 && s < 4);
        if (s >= 0 && s < 4)
            seen[s]++;
        CHECK_EQ(cave_port_from_stock((uint8_t)s), p);
    }
    for (int s = 0; s < 4; s++)
        CHECK_EQ(seen[s], 1);
    CHECK_EQ(cave_stock_port(MIDI_PORT_BLE), 2);    /* observed: module name SysEx -> port 2 */
    CHECK_EQ(cave_stock_port(MIDI_PORT_PANEL), 3);  /* inferred: DISCONNECT notice -> port 3 */
    CHECK_EQ(cave_stock_port(MIDI_PORT_INTERNAL), -1);
    CHECK_EQ(cave_stock_port((midi_port_t)99), -1);
    CHECK_EQ(cave_port_from_stock(4), MIDI_PORT_INTERNAL);
}

TEST(cave_midi_tx_goes_to_the_stock_writer_in_task_context)
{
    static const uint8_t m[] = {0xF0, 0x7D, 0x01, 0xF7};
    stock_host_reset();
    for (int p = 0; p < MIDI_PORT_INTERNAL; p++) {
        hal_midi_tx((midi_port_t)p, m, sizeof m);
        int s = cave_stock_port((midi_port_t)p);
        CHECK_EQ(stock_host.last_tx_port, s);
        CHECK_EQ(stock_host.last_tx_len, sizeof m);
        CHECK_MEM(stock_host.last_tx, m, sizeof m);
    }
    for (int s = 0; s < 4; s++)
        CHECK_EQ(stock_host.tx_calls[s], 1);
}

TEST(cave_blocking_calls_are_refused_outside_a_task)
{
    static const uint8_t m[] = {0xF0, 0xF7};
    static const uint8_t v[] = {1};
    /* ISR (nesting), dispatch disabled, kernel pseudo task 0x21 = cyclic/timer handlers */
    static const uint8_t ctx[3][3] = {{1, 0, 5}, {0, 1, 5}, {0, 0, 0x21}};
    for (int c = 0; c < 3; c++) {
        stock_host_reset();
        stock_host_context(ctx[c][0], ctx[c][1], ctx[c][2]);
        uint16_t before = cave_log_state()->count;
        CHECK(!cave_may_block());
        hal_midi_tx(MIDI_PORT_BLE, m, sizeof m);
        CHECK_EQ(stock_host.tx_calls[2], 0);
        CHECK_EQ(cave_log_state()->last_code, CAVE_LOG_TX_CONTEXT);
        CHECK_EQ(cave_param_apply(0, 0x7F, 0x68, v), -1);
        CHECK_EQ(stock_host.apply_calls, 0);
        CHECK_EQ(cave_log_state()->last_code, CAVE_LOG_APPLY_CONTEXT);
        CHECK_EQ(cave_log_state()->count, before + 2);
    }
    stock_host_reset();
    CHECK(cave_may_block());
    hal_midi_tx(MIDI_PORT_INTERNAL, m, sizeof m);
    CHECK_EQ(cave_log_state()->last_code, CAVE_LOG_TX_PORT);
}

TEST(cave_param_apply_passes_the_stock_contract)
{
    static const uint8_t v[] = {2};
    stock_host_reset();
    stock_host.apply_ret = 0xF0;
    CHECK_EQ(cave_param_apply(1, 0x7F, 0x68, v), 0xF0);
    CHECK_EQ(stock_host.apply_calls, 1);
    CHECK_EQ(stock_host.apply_args[0], 1);
    CHECK_EQ(stock_host.apply_args[1], 0x7F);
    CHECK_EQ(stock_host.apply_args[2], 0x68);
    CHECK_EQ(stock_host.apply_v0, 2);
}

TEST(cave_hal_stubs)
{
    uint8_t b = 0;
    uint32_t a = 0, c = 0;
    stock_host_reset();
    stock_host.time_us = 123456789u;
    CHECK_EQ(hal_time_us(), 123456789u);
    CHECK(!hal_midi_rx(MIDI_PORT_PANEL, &b));
    CHECK(hal_settings_flash() == NULL);
    hal_settings_sectors(&a, &c);
    CHECK_EQ(a, 0x60400000u);
    CHECK_EQ(c, 0x60420000u);
    CHECK(hal_usb_fs() == NULL);
    hal_power_off();
    CHECK_EQ(cave_log_state()->last_code, CAVE_LOG_POWER_OFF);
    CHECK(!hal_update_keys_held());
}

TEST(cave_core_selftest_passes_and_is_deterministic)
{
    uint8_t r1[CORE_SELFTEST_RAW], r2[CORE_SELFTEST_RAW];
    stock_host_reset();
    core_selftest(r1);
    CHECK_EQ(r1[0], 1);
    CHECK_EQ(r1[1], CORE_CHK_COUNT);
    CHECK_EQ(r1[2] | r1[3] << 8, (1u << CORE_CHK_COUNT) - 1);
    CHECK(le32(r1 + 4) != le32(r1 + 8));       /* the script moved the store */
    CHECK_EQ(r1[18], 7);                        /* seven kproto frames */
    CHECK_EQ(r1[19] & 3, 3);                    /* initialised, guards intact */
    core_selftest(r2);
    CHECK_MEM(r1, r2, CORE_SELFTEST_FIXED);     /* the core is reset before every run */
    CHECK_EQ(stock_host.tx_calls[0] + stock_host.tx_calls[1] + stock_host.tx_calls[2] + stock_host.tx_calls[3], 0);
    CHECK_EQ(stock_host.apply_calls, 0);        /* dormant: never applies to the stock */
}

TEST(cave_core_tick_serves_a_request_on_the_port_it_came_from)
{
    stock_host_reset();
    core_tick();   /* first tick initialises (already done by the previous test on the host) */
    CHECK(core_ready());
    for (int s = 0; s < 4; s++) {
        stock_host_reset();
        core_request((uint8_t)s);
        CHECK_EQ(stock_host.tx_calls[s], 0);    /* nothing happens until the engine loop */
        core_tick();
        CHECK_EQ(stock_host.tx_calls[s], 1);
        CHECK_EQ(stock_host.last_tx_port, s);
        CHECK_EQ(stock_host.last_tx_len, CORE_SELFTEST_FRAME);
        static const uint8_t head[] = {0xF0, 0x7D, 0x43, 0x41, 0x37, 0x39, 0x4A};
        CHECK_MEM(stock_host.last_tx, head, sizeof head);
        CHECK_EQ(stock_host.last_tx[CORE_SELFTEST_FRAME - 1], 0xF7);
        bool seven = true;
        for (unsigned i = 1; i < CORE_SELFTEST_FRAME - 1; i++)
            seven &= stock_host.last_tx[i] < 0x80;
        CHECK(seven);
        uint8_t raw[CORE_SELFTEST_RAW + 8];
        unpack7(stock_host.last_tx + 7, CORE_SELFTEST_FRAME - 8, raw);
        CHECK_EQ(raw[2] | raw[3] << 8, (1u << CORE_CHK_COUNT) - 1);
        core_tick();   /* served once */
        CHECK_EQ(stock_host.tx_calls[s], 1);
    }
}

TEST(cave_core_tick_counts_time_from_hal_time_us)
{
    uint8_t a[CORE_SELFTEST_RAW], b[CORE_SELFTEST_RAW];
    stock_host_reset();
    stock_host.time_us = 0xFFFFF000u;   /* wraps during the run */
    core_tick();
    core_selftest(a);
    for (int i = 0; i < 10; i++) {
        stock_host.time_us += 8300;
        core_tick();
    }
    core_selftest(b);
    CHECK_EQ(le32(b + 20) - le32(a + 20), 10);   /* ticks */
    CHECK_EQ(le32(b + 24) - le32(a + 24), 83);   /* ms */
    CHECK_EQ(le32(b + 28), stock_host.time_us);
    CHECK_EQ(b[19] & 4, 0);                      /* AutoPowerOff = Never: never due */
}

TEST(cave_core_hardware_reply_refused_in_timer_context)
{
    stock_host_reset();
    stock_host_context(0, 0, 0x21);
    core_request(2);
    core_tick();
    CHECK_EQ(stock_host.tx_calls[2], 0);
    CHECK_EQ(cave_log_state()->last_code, CAVE_LOG_TX_CONTEXT);
    stock_host_reset();
}

TEST(cave_rt_matches_libc)
{
    uint8_t a[64], b[64];
    char s1[16], s2[16];
    for (int i = 0; i < 64; i++)
        a[i] = (uint8_t)(i * 37 + 1);
    CHECK(cave_rt_memcpy(b, a, 64) == b);
    CHECK_MEM(a, b, 64);
    cave_rt_memset(b + 3, 0xAB, 10);
    for (int i = 0; i < 64; i++)
        CHECK_EQ(b[i], (i >= 3 && i < 13) ? 0xAB : a[i]);
    CHECK_EQ(cave_rt_memcmp(a, b, 3), 0);
    CHECK((cave_rt_memcmp(a, b, 64) < 0) == (memcmp(a, b, 64) < 0));
    CHECK((cave_rt_memcmp(b, a, 64) < 0) == (memcmp(b, a, 64) < 0));
    CHECK_EQ(cave_rt_memcmp(a, b, 0), 0);
    CHECK_EQ(cave_rt_strlen(""), 0);
    CHECK_EQ(cave_rt_strlen("1.0.0"), 5);
    memset(s1, 0x55, sizeof s1);
    memset(s2, 0x55, sizeof s2);
    cave_rt_strncpy(s1, "abc", 8);
    strncpy(s2, "abc", 8);
    CHECK_MEM(s1, s2, sizeof s1);
    cave_rt_strncpy(s1, "abcdefghij", 4);   /* truncates: no NUL, the rest untouched */
    memcpy(s2, "abcd", 4);
    CHECK_MEM(s1, s2, sizeof s1);
}

TEST(cave_rt_unsigned_division)
{
    static const unsigned edge[] = {0, 1, 2, 3, 7, 10, 21, 1000, 20000, 0x7FFFFFFFu, 0x80000000u,
                                    0x80000001u, 0xFFFFFFFEu, 0xFFFFFFFFu};
    int bad = 0;
    uint32_t x = 0x12345678u;
    for (unsigned i = 0; i < sizeof edge / sizeof edge[0]; i++)
        for (unsigned j = 1; j < sizeof edge / sizeof edge[0]; j++) {
            unsigned n = edge[i], d = edge[j];
            unsigned long long qr = cave_rt___aeabi_uidivmod(n, d);
            bad += cave_rt___aeabi_uidiv(n, d) != n / d || (unsigned)qr != n / d || (unsigned)(qr >> 32) != n % d;
        }
    for (int i = 0; i < 20000; i++) {
        x ^= x << 13; x ^= x >> 17; x ^= x << 5;
        unsigned n = x;
        x ^= x << 13; x ^= x >> 17; x ^= x << 5;
        unsigned d = (i & 1) ? x : (x >> (x & 31));
        if (!d)
            continue;
        unsigned long long qr = cave_rt___aeabi_uidivmod(n, d);
        bad += cave_rt___aeabi_uidiv(n, d) != n / d || (unsigned)qr != n / d || (unsigned)(qr >> 32) != n % d;
    }
    CHECK_EQ(bad, 0);
    CHECK_EQ(cave_rt___aeabi_uidiv(5, 0), 0);
}

void suite_cave(void)
{
    puts("cave port:");
    RUN(cave_port_map_is_a_bijection);
    RUN(cave_midi_tx_goes_to_the_stock_writer_in_task_context);
    RUN(cave_blocking_calls_are_refused_outside_a_task);
    RUN(cave_param_apply_passes_the_stock_contract);
    RUN(cave_hal_stubs);
    RUN(cave_core_selftest_passes_and_is_deterministic);
    RUN(cave_core_tick_serves_a_request_on_the_port_it_came_from);
    RUN(cave_core_tick_counts_time_from_hal_time_us);
    RUN(cave_core_hardware_reply_refused_in_timer_context);
    RUN(cave_rt_matches_libc);
    RUN(cave_rt_unsigned_division);
}
