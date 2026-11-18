/* Tests for the cave panel-UI relay (ports/cave/relay_cave.c, slot payload ota_main feat2 0x04): rules R1-R6 of
 * The TX writers are stock_host.c's recorders; the
 * four TX ring byte counts (relay_stock.h) are stand-ins here. */
#include <stdbool.h>
#include <stdint.h>

#include "../ports/cave/relay_cave.h"
#include "../ports/cave/stock_host.h"
#include "test.h"

/* ---- stock stand-ins (relay_stock.h) ---- */
volatile uint16_t stk_txcnt0, stk_txcnt1, stk_txcnt2, stk_txcnt3;

#define LIMIT 0x7FEu

static void fresh(void)
{
    rlc_restart();
    stock_host_reset();
    stk_txcnt0 = stk_txcnt1 = stk_txcnt2 = stk_txcnt3 = 0;
}

/* F0 7D 43 41 37 39 <op> <n bytes of body> F7 */
static uint32_t frame(uint8_t *m, uint8_t op, uint32_t n, uint8_t seed)
{
    uint32_t o = 0;
    m[o++] = 0xF0; m[o++] = 0x7D; m[o++] = 0x43; m[o++] = 0x41; m[o++] = 0x37; m[o++] = 0x39;
    m[o++] = op;
    for (uint32_t i = 0; i < n; i++)
        m[o++] = (uint8_t)((seed + i * 7u) & 0x7F);
    m[o++] = 0xF7;
    return o;
}

static uint32_t stat32(const uint8_t *raw, int o)
{
    return raw[o] | (uint32_t)raw[o + 1] << 8 | (uint32_t)raw[o + 2] << 16 | (uint32_t)raw[o + 3] << 24;
}

TEST(relay_owns_only_its_ops)
{
    CHECK(!rlc_owns(0x2F));
    CHECK(rlc_owns(0x30));
    CHECK(rlc_owns(0x3F));
    CHECK(!rlc_owns(0x40));
    CHECK(!rlc_owns(0x5B));   /* op 0x1B's reply: never ours */
    CHECK(!rlc_owns(0x6F));
    CHECK(rlc_owns(0x70));
    CHECK(rlc_owns(0x7F));
    CHECK(!rlc_owns(0x01) && !rlc_owns(0x1B) && !rlc_owns(0x1D) && !rlc_owns(0x12));
}

TEST(relay_request_verbatim_to_panel_reply_back)
{
    uint8_t m[300], r[300], raw[RL_DIAG_RAW];
    uint32_t n, k;
    fresh();
    n = frame(m, 0x32, 232, 5);   /* 240 B: the largest Data frame */
    rlc_sysex(2, m, n);
    CHECK_EQ(stock_host.tx_calls[3], 1);
    CHECK_EQ(stock_host.last_tx_port, 3);
    CHECK_EQ(stock_host.last_tx_len, n);
    CHECK_MEM(stock_host.last_tx, m, n);   /* F0 .. F7 as it came */
    k = frame(r, 0x72, 7, 9);
    rlc_sysex(3, r, k);
    CHECK_EQ(stock_host.tx_calls[2], 1);
    CHECK_EQ(stock_host.last_tx_port, 2);
    CHECK_MEM(stock_host.last_tx, r, k);
    CHECK_EQ(stock_host.log_n, 2);   /* nothing else went anywhere */
    rlc_stats(raw);
    CHECK_EQ(raw[0], 1);
    CHECK_EQ(raw[1], 3);              /* reply port 2 (+1) */
    CHECK_EQ(stat32(raw, 8), 1);
    CHECK_EQ(stat32(raw, 12), n);
    CHECK_EQ(stat32(raw, 16), 1);
    CHECK_EQ(stat32(raw, 20) + stat32(raw, 24) + stat32(raw, 28) + stat32(raw, 32) + stat32(raw, 36), 0);
}

TEST(relay_reply_goes_to_the_latest_asker)
{
    uint8_t m[32], r[32];
    uint32_t n = frame(m, 0x30, 0, 0), k = frame(r, 0x70, 3, 1);
    fresh();
    rlc_sysex(0, m, n);
    rlc_sysex(3, r, k);
    CHECK_EQ(stock_host.last_tx_port, 0);
    rlc_sysex(1, m, n);
    rlc_sysex(3, r, k);
    CHECK_EQ(stock_host.last_tx_port, 1);
    rlc_sysex(2, m, n);
    rlc_sysex(3, r, k);
    CHECK_EQ(stock_host.last_tx_port, 2);
    CHECK_EQ(stock_host.tx_calls[3], 3);
    CHECK_EQ(stock_host.tx_calls[0] + stock_host.tx_calls[1] + stock_host.tx_calls[2], 3);
}

TEST(relay_drops_wrong_direction_and_orphans)
{
    uint8_t m[32], raw[RL_DIAG_RAW];
    uint32_t n;
    fresh();
    n = frame(m, 0x71, 5, 0);
    rlc_sysex(3, m, n);               /* R2: a reply before any request: no reply port */
    n = frame(m, 0x31, 5, 0);
    rlc_sysex(3, m, n);               /* R3: a request from the panel itself */
    n = frame(m, 0x7F, 0, 0);
    rlc_sysex(2, m, n);               /* R3: a reply from a controller port */
    CHECK_EQ(stock_host.log_n, 0);
    rlc_stats(raw);
    CHECK_EQ(raw[1], 0);              /* no reply port learnt */
    CHECK_EQ(stat32(raw, 24), 2);     /* direction */
    CHECK_EQ(stat32(raw, 28), 1);     /* no source */
    CHECK_EQ(raw[2], 2);              /* last reason: direction */
}

TEST(relay_length_first)
{
    uint8_t m[300], raw[RL_DIAG_RAW];
    uint32_t n;
    fresh();
    n = frame(m, 0x32, RL_FRAME_MAX - 8, 1);   /* exactly 256 B: passes */
    CHECK_EQ(n, RL_FRAME_MAX);
    rlc_sysex(2, m, n);
    CHECK_EQ(stock_host.tx_calls[3], 1);
    n = frame(m, 0x32, RL_FRAME_MAX - 7, 1);   /* 257 B: dropped whole */
    rlc_sysex(2, m, n);
    CHECK_EQ(stock_host.tx_calls[3], 1);
    rlc_sysex(2, m, 7);                         /* shorter than a frame */
    rlc_sysex(4, m, 9);                         /* no such stock port */
    CHECK_EQ(stock_host.tx_calls[3], 1);
    rlc_stats(raw);
    CHECK_EQ(stat32(raw, 20), 3);
    CHECK_EQ(raw[2], 1);
}

TEST(relay_whole_or_nothing_on_a_busy_ring)
{
    uint8_t m[300], r[300], raw[RL_DIAG_RAW];
    uint32_t n = frame(m, 0x32, 232, 3), k = frame(r, 0x70, 108, 2);   /* 240 B request, 116 B Status reply */
    fresh();
    stk_txcnt3 = (uint16_t)(LIMIT - RL_RESERVE - n + 1);   /* one byte short (the stock's reserve kept) */
    rlc_sysex(2, m, n);
    CHECK_EQ(stock_host.tx_calls[3], 0);
    stk_txcnt3 = (uint16_t)(LIMIT - RL_RESERVE - n);       /* exactly fits */
    rlc_sysex(2, m, n);
    CHECK_EQ(stock_host.tx_calls[3], 1);
    CHECK_EQ(stock_host.last_tx_len, n);
    stk_txcnt3 = 0x7FF;                                     /* the writer would spin: never called */
    rlc_sysex(2, m, n);
    CHECK_EQ(stock_host.tx_calls[3], 1);
    /* the reply ring (BLE port 2) in the middle of a stock dump: dropped, the panel's next reply passes */
    stk_txcnt2 = (uint16_t)(LIMIT - RL_RESERVE - k + 1);
    rlc_sysex(3, r, k);
    CHECK_EQ(stock_host.tx_calls[2], 0);
    stk_txcnt2 = 100;
    rlc_sysex(3, r, k);
    CHECK_EQ(stock_host.tx_calls[2], 1);
    rlc_stats(raw);
    CHECK_EQ(stat32(raw, 32), 3);                /* full */
    CHECK_EQ(stat32(raw, 8), 1);
    CHECK_EQ(stat32(raw, 16), 1);
    CHECK_EQ(raw[6] | raw[7] << 8, 0x7FF);      /* peak port 3 count seen */
    CHECK_EQ(raw[1], 3);                         /* the asker stays the reply port even when its frame was dropped */
}

TEST(relay_never_sends_outside_a_task)
{
    uint8_t m[32], raw[RL_DIAG_RAW];
    uint32_t n = frame(m, 0x30, 0, 0);
    fresh();
    stock_host_context(1, 0, 5);   /* interrupt nesting: the writer's semaphore must not be taken */
    rlc_sysex(2, m, n);
    stock_host_context(0, 1, 5);   /* dispatch disabled */
    rlc_sysex(2, m, n);
    CHECK_EQ(stock_host.log_n, 0);
    rlc_stats(raw);
    CHECK_EQ(stat32(raw, 36), 2);
    CHECK_EQ(raw[2], 5);
}

TEST(relay_keeps_order_per_direction)
{
    uint8_t m[3][40];
    uint32_t n[3];
    fresh();
    for (int i = 0; i < 3; i++)
        n[i] = frame(m[i], (uint8_t)(0x32 + (i == 2)), (uint32_t)(10 + i), (uint8_t)i);
    for (int i = 0; i < 3; i++)
        rlc_sysex(2, m[i], n[i]);
    CHECK_EQ(stock_host.log_n, 3);
    for (int i = 0; i < 3; i++) {
        CHECK_EQ(stock_host.log_port[i], 3);
        CHECK_EQ(stock_host.log_len[i], n[i]);
        CHECK_MEM(stock_host.log[i], m[i], n[i]);
    }
}

TEST(relay_diag_frame)
{
    uint8_t m[32], r[32];
    uint32_t n = frame(m, 0x30, 0, 0), k = frame(r, 0x70, 4, 0);
    fresh();
    rlc_sysex(2, m, n);
    rlc_sysex(3, r, k);
    stk_txcnt3 = 0x123;
    stock_host.log_n = 0;
    rlc_diag(2);
    CHECK_EQ(stock_host.last_tx_port, 2);
    CHECK_EQ(stock_host.last_tx_len, 56);   /* 9 + pack7(40) 46 + F7 */
    {
        const uint8_t *f = stock_host.last_tx;
        uint8_t raw[RL_DIAG_RAW];
        uint32_t o = 9, i = 0;
        CHECK(f[0] == 0xF0 && f[1] == 0x7D && f[6] == 0x5B && f[7] == RL_DIAG_SUB && f[8] == 0 && f[55] == 0xF7);
        while (i < RL_DIAG_RAW) {
            uint8_t hi = f[o++];
            for (uint32_t b = 0; b < 7 && i < RL_DIAG_RAW; b++, i++)
                raw[i] = (uint8_t)(f[o++] | (((hi >> b) & 1u) << 7));
        }
        CHECK_EQ(raw[0], 1);
        CHECK_EQ(raw[1], 3);
        CHECK_EQ(raw[3], RL_RESERVE / 16);
        CHECK_EQ(raw[4] | raw[5] << 8, 0x123);
        CHECK_EQ(stat32(raw, 8), 1);
        CHECK_EQ(stat32(raw, 12), n);
        CHECK_EQ(stat32(raw, 16), 1);
        for (uint32_t j = 0; j < 55; j++)
            CHECK(j == 0 || f[j] < 0x80);
    }
}

void suite_relay_cave(void)
{
    printf("panel-UI relay (feat2 0x04)\n");
    RUN(relay_owns_only_its_ops);
    RUN(relay_request_verbatim_to_panel_reply_back);
    RUN(relay_reply_goes_to_the_latest_asker);
    RUN(relay_drops_wrong_direction_and_orphans);
    RUN(relay_length_first);
    RUN(relay_whole_or_nothing_on_a_busy_ring);
    RUN(relay_never_sends_outside_a_task);
    RUN(relay_keeps_order_per_direction);
    RUN(relay_diag_frame);
}
