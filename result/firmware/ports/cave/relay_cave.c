/* OpenGrand cave port - panel-UI relay (0x7D ops 0x30-0x3F <-> port 3).  See relay_cave.h. */
#include "relay_cave.h"

#include <stdbool.h>
#include <string.h>

#include "hal_cave.h"
#include "relay_stock.h"

#define PANEL 3u

enum {
    RL_WHY_NONE = 0, RL_WHY_LEN = 1, RL_WHY_DIR = 2, RL_WHY_NOSRC = 3, RL_WHY_FULL = 4, RL_WHY_CTX = 5,
};

/* payload .data: copied fresh with the payload, written by task 19 only */
static struct {
    uint8_t src;            /* reply port + 1 (R1), 0 none */
    uint8_t last_why;       /* RL_WHY_* of the last drop */
    uint16_t peak3;         /* highest port 3 ring count seen before a relayed frame */
    uint32_t to_panel;      /* frames relayed to port 3 */
    uint32_t to_panel_b;    /* their bytes */
    uint32_t from_panel;    /* replies relayed back */
    uint32_t drop_len;      /* longer than RL_FRAME_MAX (or shorter than a frame) */
    uint32_t drop_dir;      /* request from port 3 / reply from a controller port (R3) */
    uint32_t drop_nosrc;    /* reply with no request seen (R2) */
    uint32_t drop_full;     /* target ring without room for the whole frame + reserve (R4) */
    uint32_t drop_ctx;      /* not a task that may block (never expected: task 19) */
} rl = {0};

int rlc_owns(uint8_t op)
{
    return (op >= 0x30 && op <= 0x3F) || (op >= 0x70 && op <= 0x7F);
}

static uint16_t ring_count(uint8_t p)
{
    switch (p) {
    case 0: return stk_txcnt0;
    case 1: return stk_txcnt1;
    case 2: return stk_txcnt2;
    default: return stk_txcnt3;
    }
}

static void drop(uint32_t *ctr, uint8_t why)
{
    (*ctr)++;
    rl.last_why = why;
}

/* R4: the whole frame, in one writer call, only when it fits below the reserve - the writer then never
 * spins on a full ring for our sake, and the stock keeps RL_RESERVE bytes for its own frames */
static bool put(uint8_t p, const uint8_t *buf, uint32_t len)
{
    uint32_t cnt = ring_count(p);
    if (p == PANEL && cnt > rl.peak3)
        rl.peak3 = (uint16_t)cnt;
    if (cnt + len + RL_RESERVE > STK_TXRING_LIMIT) {
        drop(&rl.drop_full, RL_WHY_FULL);
        return false;
    }
    if (!cave_may_block()) {
        drop(&rl.drop_ctx, RL_WHY_CTX);
        return false;
    }
    hal_midi_tx(cave_port_from_stock(p), buf, (uint16_t)len);
    return true;
}

void rlc_sysex(uint8_t port, const uint8_t *buf, uint32_t len)
{
    uint8_t op;
    /* the cave checked F0 7D C A 7 9 <op> .. F7 (len >= 8); the length first (R3) */
    if (len < 8 || len > RL_FRAME_MAX || port > PANEL) {
        drop(&rl.drop_len, RL_WHY_LEN);
        return;
    }
    op = buf[6];
    if (op >= 0x30 && op <= 0x3F) {
        if (port == PANEL) {
            drop(&rl.drop_dir, RL_WHY_DIR);
            return;
        }
        rl.src = (uint8_t)(port + 1);   /* the asking port, even when the ring is full: its retry comes back here */
        if (put(PANEL, buf, len)) {
            rl.to_panel++;
            rl.to_panel_b += len;
        }
        return;
    }
    if (op < 0x70 || op > 0x7F)
        return;                          /* not ours (rlc_owns) */
    if (port != PANEL) {
        drop(&rl.drop_dir, RL_WHY_DIR);
        return;
    }
    if (!rl.src) {
        drop(&rl.drop_nosrc, RL_WHY_NOSRC);
        return;
    }
    if (put((uint8_t)(rl.src - 1), buf, len))
        rl.from_panel++;
}

static void put32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24);
}

/* layout 1 (LE): 0 layout · 1 reply port + 1 · 2 last drop reason · 3 reserve / 16 · 4..5 port 3 ring count now ·
 * 6..7 peak · 8 to_panel · 12 bytes · 16 from_panel · 20 drop len · 24 dir · 28 no source · 32 full · 36 context */
void rlc_stats(uint8_t raw[RL_DIAG_RAW])
{
    memset(raw, 0, RL_DIAG_RAW);
    raw[0] = 1;
    raw[1] = rl.src;
    raw[2] = rl.last_why;
    raw[3] = (uint8_t)(RL_RESERVE / 16u);
    raw[4] = (uint8_t)stk_txcnt3;
    raw[5] = (uint8_t)(stk_txcnt3 >> 8);
    raw[6] = (uint8_t)rl.peak3;
    raw[7] = (uint8_t)(rl.peak3 >> 8);
    put32(raw + 8, rl.to_panel);
    put32(raw + 12, rl.to_panel_b);
    put32(raw + 16, rl.from_panel);
    put32(raw + 20, rl.drop_len);
    put32(raw + 24, rl.drop_dir);
    put32(raw + 28, rl.drop_nosrc);
    put32(raw + 32, rl.drop_full);
    put32(raw + 36, rl.drop_ctx);
}

void rlc_diag(uint8_t port)
{
    uint8_t raw[RL_DIAG_RAW], m[10 + RL_DIAG_RAW + (RL_DIAG_RAW + 6) / 7];
    uint32_t o = 0;
    m[o++] = 0xF0; m[o++] = 0x7D; m[o++] = 0x43; m[o++] = 0x41; m[o++] = 0x37; m[o++] = 0x39;
    m[o++] = 0x1B | 0x40;
    m[o++] = RL_DIAG_SUB;
    m[o++] = 0;   /* rc */
    rlc_stats(raw);
    for (uint32_t i = 0; i < RL_DIAG_RAW; i += 7) {
        uint32_t g = RL_DIAG_RAW - i < 7 ? RL_DIAG_RAW - i : 7;
        uint8_t hi = 0;
        for (uint32_t k = 0; k < g; k++)
            hi |= (uint8_t)(((raw[i + k] >> 7) & 1u) << k);
        m[o++] = hi;
        for (uint32_t k = 0; k < g; k++)
            m[o++] = raw[i + k] & 0x7F;
    }
    m[o++] = 0xF7;
    hal_midi_tx(cave_port_from_stock(port & 3u), m, (uint16_t)o);
}

void rlc_restart(void)
{
    memset(&rl, 0, sizeof rl);
}
