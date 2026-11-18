/* Host stand-in for the stock CA79 routines the cave port binds to (see stock_host.c). */
#ifndef CA79NG_STOCK_HOST_H
#define CA79NG_STOCK_HOST_H

#include <stdint.h>


/* stand-ins for the routines in cave_stock.h (declared there for the port) */
uint32_t stk_time_us(void);
void stk_tx_port0(const uint8_t *buf, uint32_t len);
void stk_tx_port1(const uint8_t *buf, uint32_t len);
void stk_tx_port2(const uint8_t *buf, uint32_t len);
void stk_tx_port3(const uint8_t *buf, uint32_t len);
int stk_param_apply(uint32_t ctx, uint32_t part, uint32_t id, const uint8_t *data);

#define STOCK_HOST_LOG 16

typedef struct {
    uint32_t time_us;          /* what stk_time_us() returns */
    int tx_calls[4];
    int last_tx_port;
    uint32_t last_tx_len;
    uint8_t last_tx[256];
    int log_n;                 /* every frame in order (first STOCK_HOST_LOG kept) */
    uint8_t log_port[STOCK_HOST_LOG];
    uint16_t log_len[STOCK_HOST_LOG];
    uint8_t log[STOCK_HOST_LOG][80];
    int apply_calls;
    uint32_t apply_args[3];    /* ctx, part, id */
    uint8_t apply_v0;
    int apply_ret;
} stock_host_t;

extern stock_host_t stock_host;

void stock_host_reset(void);   /* zero the record; task context (nest 0, dispatch on, task 5) */
void stock_host_context(uint8_t nest, uint8_t dsp_disabled, uint8_t task);

#endif
