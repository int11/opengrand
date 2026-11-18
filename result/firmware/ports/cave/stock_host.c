/* Host stand-in for the stock routines and globals in cave_stock.h, so the cave port
 * (hal_cave.c, core_cave.c) builds and runs on the host: unit tests (tests/test_cave.c)
 * and tools/cave_selftest.c.  Records every call; never part of the piano build. */
#include "stock_host.h"

#include <string.h>

stock_host_t stock_host;

/* Read-only (const volatile) for the port, writable here: this file deliberately does not
 * include cave_stock.h, the same way the piano's stock RAM is "const" only to the cave. */
volatile uint8_t stk_int_nest;
volatile uint8_t stk_dsp_disabled;
volatile uint8_t stk_cur_task;

void stock_host_reset(void)
{
    memset(&stock_host, 0, sizeof stock_host);
    stock_host_context(0, 0, 5);
}

void stock_host_context(uint8_t nest, uint8_t dsp_disabled, uint8_t task)
{
    stk_int_nest = nest;
    stk_dsp_disabled = dsp_disabled;
    stk_cur_task = task;
}

uint32_t stk_time_us(void) { return stock_host.time_us; }

static void tx(int port, const uint8_t *buf, uint32_t len)
{
    stock_host.tx_calls[port]++;
    stock_host.last_tx_port = port;
    stock_host.last_tx_len = len;
    memcpy(stock_host.last_tx, buf, len < sizeof stock_host.last_tx ? len : sizeof stock_host.last_tx);
    if (stock_host.log_n < STOCK_HOST_LOG) {
        int i = stock_host.log_n;
        stock_host.log_port[i] = (uint8_t)port;
        stock_host.log_len[i] = (uint16_t)len;
        memcpy(stock_host.log[i], buf, len < sizeof stock_host.log[i] ? len : sizeof stock_host.log[i]);
    }
    stock_host.log_n++;
}

void stk_tx_port0(const uint8_t *buf, uint32_t len) { tx(0, buf, len); }
void stk_tx_port1(const uint8_t *buf, uint32_t len) { tx(1, buf, len); }
void stk_tx_port2(const uint8_t *buf, uint32_t len) { tx(2, buf, len); }
void stk_tx_port3(const uint8_t *buf, uint32_t len) { tx(3, buf, len); }

int stk_param_apply(uint32_t ctx, uint32_t part, uint32_t id, const uint8_t *data)
{
    stock_host.apply_calls++;
    stock_host.apply_args[0] = ctx;
    stock_host.apply_args[1] = part;
    stock_host.apply_args[2] = id;
    stock_host.apply_v0 = data ? data[0] : 0xFF;
    return stock_host.apply_ret;
}
