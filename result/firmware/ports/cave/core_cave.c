/* OpenGrand cave port - dormant core instance and its self-test.  See core_cave.h. */
#include "core_cave.h"

#include <string.h>

#include "ca79ng/kproto.h"
#include "ca79ng/piano.h"   /* CA79NG_VERSION, the identity the core reports */
#include "ca79ng/system.h"
#include "ca79ng/util.h"
#include "hal_cave.h"

#define GUARD_LO 0xC0DE5A17u
#define GUARD_HI 0x5A17C0DEu
#define CAP_FRAMES 8
#define CAP_BYTES 24

/* ---- RW state in R1 (zeroed by core_init, never loaded from flash) ---------------- */
typedef struct {
    uint32_t guard_lo;
    param_store_t store;
    kproto_t kp;
    /* self-test scratch: captured kproto output and the pack87 buffers */
    uint8_t cap[CAP_FRAMES][CAP_BYTES];
    uint16_t cap_len[CAP_FRAMES];
    uint8_t raw[256], packed[256 + 37], back[256 + 8];
    uint32_t guard_hi;
} core_rw_t;

CAVE_RW static core_rw_t rw;

/* ---- small state in the cave's own .data (zero at boot: loaded from flash) ---------- */
static struct {
    uint8_t inited;
    uint8_t apo_due;
    uint32_t ticks;
    uint32_t last_us, acc_us, ms;
    power_timer_t apo;
    /* what the kproto callbacks saw during the current self-test */
    uint32_t tx_crc;
    uint16_t tx_bytes;
    uint8_t tx_frames;
    uint8_t tx_port_ok;
    uint8_t params;
    uint8_t param_id, param_slot, param_v0;
    int8_t session_port;
    int8_t session_up;
} st = {0};

static volatile uint8_t req_pending = 0;
static volatile uint8_t req_port = 0;

static const kp_identity_t IDENTITY = {
    .group = 0x04, .machine = 0x1E, .sub1 = 0x00, .sub2 = 0x02,
    .destination = 0x01, .language = 0x00, .version = CA79NG_VERSION,
};

/* ---- kproto callbacks: dormant, nothing reaches the stock firmware ---------------- */

static void io_tx(void *ctx, midi_port_t port, const uint8_t *msg, uint16_t len)
{
    (void)ctx;
    if (st.tx_frames < CAP_FRAMES) {
        uint16_t n = len < CAP_BYTES ? len : CAP_BYTES;
        memcpy(rw.cap[st.tx_frames], msg, n);
        rw.cap_len[st.tx_frames] = len;
    }
    if (port != MIDI_PORT_PANEL)
        st.tx_port_ok = 0;
    st.tx_crc = ca_crc32(st.tx_crc, msg, len);
    st.tx_bytes = (uint16_t)(st.tx_bytes + len);
    if (st.tx_frames != 0xFF)
        st.tx_frames++;
}

static void io_param(void *ctx, param_id_t id, int slot, uint8_t fn, const uint8_t *data, uint8_t len,
                     midi_port_t origin)
{
    (void)ctx; (void)fn; (void)origin;
    /* dormant: the stock firmware applies parameters itself; Phase 3-1 maps id/slot to
     * the stock internal id and calls cave_param_apply() here. */
    st.params++;
    st.param_id = (uint8_t)id;
    st.param_slot = (uint8_t)slot;
    st.param_v0 = len ? data[0] : 0xFF;
}

static void io_session(void *ctx, midi_port_t port, bool connected)
{
    (void)ctx;
    st.session_port = (int8_t)port;
    st.session_up = connected ? 1 : 0;
}

static void io_power_off(void *ctx)
{
    (void)ctx;
    hal_power_off();   /* not bound yet: logged and ignored */
}

static void core_reset(void)
{
    const kp_io_t io = {.tx = io_tx, .param = io_param, .session = io_session, .power_off = io_power_off};
    param_store_defaults(&rw.store);
    kp_init(&rw.kp, &rw.store, &IDENTITY, &io);
    power_timer_init(&st.apo, param_get1(&rw.store, PRM_AUTO_POWER_OFF, SLOT_SYSTEM, 0));
    st.apo_due = 0;
}

static void core_init(uint32_t now)
{
    memset(&rw, 0, sizeof rw);
    rw.guard_lo = GUARD_LO;
    rw.guard_hi = GUARD_HI;
    core_reset();
    st.last_us = now;
    st.acc_us = 0;
    st.ms = 0;
    st.inited = 1;
}

bool core_ready(void) { return st.inited != 0; }

bool core_idle(void) { return req_pending == 0; }

void core_restart(void)
{
    req_pending = 0;
    memset(&st, 0, sizeof st);
}

void core_request(uint8_t stock_port)
{
    req_port = stock_port & 3;
    req_pending = 1;
}

void core_tick(void)
{
    uint32_t now = hal_time_us();
    if (!st.inited) {
        core_init(now);
    } else {
        uint32_t whole;
        st.acc_us += now - st.last_us;
        st.last_us = now;
        whole = st.acc_us / 1000u;
        st.acc_us -= whole * 1000u;
        st.ms += whole;
        if (power_timer_tick(&st.apo, whole))
            st.apo_due = 1;   /* dormant: the stock APO stays in charge */
    }
    st.ticks++;

    if (req_pending) {
        uint8_t raw[CORE_SELFTEST_RAW];
        uint8_t frame[CORE_SELFTEST_FRAME];
        uint8_t port = req_port;
        req_pending = 0;
        core_selftest(raw);
        hal_midi_tx(cave_port_from_stock(port), frame, core_selftest_frame(raw, frame));
    }
}

/* ---- self-test ---------------------------------------------------------------------- */

static bool sent(int i, const uint8_t *want, uint16_t n)
{
    return i < CAP_FRAMES && i < st.tx_frames && rw.cap_len[i] == n && n <= CAP_BYTES &&
           memcmp(rw.cap[i], want, n) == 0;
}

static void feed(const uint8_t *msg, uint16_t n) { kp_handle_sysex(&rw.kp, MIDI_PORT_PANEL, msg, n); }

static void put32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24);
}

static bool guards_ok(void) { return rw.guard_lo == GUARD_LO && rw.guard_hi == GUARD_HI; }

void core_selftest(uint8_t raw[CORE_SELFTEST_RAW])
{
    /* controller -> piano frames (F0 40 <dev> <fn> 08 02 ...) */
    static const uint8_t CONNECT[] = {0xF0, 0x40, 0x00, 0x7E, 0x08, 0x02, 0x01, 0x01, 0xF7};
    static const uint8_t SET_APO[] = {0xF0, 0x40, 0x00, 0x10, 0x08, 0x02, 0x51, 0x00, 0x7F, 0x02, 0xF7};
    /* 51 03 (Tuning): v2 0x00 would ask for the whole group 51 (stock wildcard) */
    static const uint8_t REQ_TUNE[] = {0xF0, 0x40, 0x00, 0x02, 0x08, 0x02, 0x51, 0x03, 0x7F, 0xF7};
    static const uint8_t ACK_TUNE[] = {0xF0, 0x40, 0x7F, 0x07, 0x08, 0x02, 0x51, 0x03, 0x7F, 0xF7};
    static const uint8_t VERSION[] = {0xF0, 0x40, 0x00, 0x7E, 0x08, 0x02, 0x04, 0x41, 0xF7};
    static const uint8_t UNKNOWN[] = {0xF0, 0x40, 0x00, 0x10, 0x08, 0x02, 0x51, 0x7E, 0x7F, 0x00, 0xF7};
    static const uint8_t DISCONNECT[] = {0xF0, 0x40, 0x00, 0x7E, 0x08, 0x02, 0x01, 0x00, 0xF7};
    /* piano -> controller replies the core must produce (REMOTE_PROTOCOL sections 3-5) */
    static const uint8_t R_MODE[] = {0xF0, 0x40, 0x7F, 0x7E, 0x04, 0x1E, 0x02, 0x00, 0x40, 0xF7};
    static const uint8_t R_ACK[] = {0xF0, 0x40, 0x7F, 0x07, 0x04, 0x1E, 0x51, 0x00, 0x7F, 0xF7};
    static const uint8_t R_EOD[] = {0xF0, 0x40, 0x7F, 0x03, 0x04, 0x1E, 0x51, 0x03, 0x7F, 0xF7};
    static const uint8_t R_VERSION[] = {0xF0, 0x40, 0x7F, 0x7E, 0x04, 0x1E, 0x04, 0x01,
                                        '1', '.', '0', '.', '0', 0xF7};
    static const uint8_t R_UNKNOWN_ACK[] = {0xF0, 0x40, 0x7F, 0x07, 0x04, 0x1E, 0x51, 0x7E, 0x7F, 0xF7};
    static const uint8_t R_HOME[] = {0xF0, 0x40, 0x7F, 0x7E, 0x04, 0x1E, 0x01, 0x01, 0xF7};
    uint8_t r_value[] = {0xF0, 0x40, 0x7F, 0x10, 0x04, 0x1E, 0x51, 0x03, 0x7F, 0x00, 0xF7};
    uint16_t mask = 0;
    uint32_t crc_def, crc_after, errors;
    bool guards;
    size_t np, nb;
    int i;

    if (!st.inited)
        core_init(hal_time_us());
    guards = guards_ok();
    if (guards)
        mask |= 1u << CORE_CHK_GUARDS;

    core_reset();
    st.tx_crc = 0;
    st.tx_bytes = 0;
    st.tx_frames = 0;
    st.tx_port_ok = 1;
    st.params = 0;
    st.session_port = -1;
    st.session_up = -1;
    crc_def = ca_crc32(0, &rw.store, sizeof rw.store);

    feed(CONNECT, sizeof CONNECT);
    if (st.tx_frames == 1 && sent(0, R_MODE, sizeof R_MODE) && st.session_port == MIDI_PORT_PANEL &&
        st.session_up == 1 && rw.kp.connected[MIDI_PORT_PANEL])
        mask |= 1u << CORE_CHK_CONNECT;

    feed(SET_APO, sizeof SET_APO);
    if (st.tx_frames == 2 && sent(1, R_ACK, sizeof R_ACK) && st.params == 1 &&
        st.param_id == PRM_AUTO_POWER_OFF && st.param_slot == SLOT_SYSTEM && st.param_v0 == 2)
        mask |= 1u << CORE_CHK_PARAM_SET;
    crc_after = ca_crc32(0, &rw.store, sizeof rw.store);
    if (param_get1(&rw.store, PRM_AUTO_POWER_OFF, SLOT_SYSTEM, 0xFF) == 2 && crc_after != crc_def)
        mask |= 1u << CORE_CHK_STORE;

    /* stop-and-wait: the value, then the end only after the controller's ACK */
    r_value[9] = param_get1(&rw.store, PRM_TUNING, SLOT_SYSTEM, 0x7F);
    feed(REQ_TUNE, sizeof REQ_TUNE);
    i = st.tx_frames;
    feed(ACK_TUNE, sizeof ACK_TUNE);
    if (i == 3 && st.tx_frames == 4 && sent(2, r_value, sizeof r_value) && sent(3, R_EOD, sizeof R_EOD))
        mask |= 1u << CORE_CHK_REQUEST;
    feed(ACK_TUNE, sizeof ACK_TUNE); /* the end carries the same key */

    feed(VERSION, sizeof VERSION);
    if (st.tx_frames == 5 && sent(4, R_VERSION, sizeof R_VERSION))
        mask |= 1u << CORE_CHK_VERSION;

    errors = rw.kp.rx_errors;
    feed(UNKNOWN, sizeof UNKNOWN);
    if (st.tx_frames == 6 && sent(5, R_UNKNOWN_ACK, sizeof R_UNKNOWN_ACK) && rw.kp.rx_errors == errors + 1 &&
        st.params == 1)
        mask |= 1u << CORE_CHK_NACK;

    feed(DISCONNECT, sizeof DISCONNECT);
    if (st.tx_frames == 7 && sent(6, R_HOME, sizeof R_HOME) && st.session_up == 0 &&
        !rw.kp.connected[MIDI_PORT_PANEL] && st.tx_port_ok)
        mask |= 1u << CORE_CHK_DISCONNECT;

    for (i = 0; i < 256; i++)
        rw.raw[i] = (uint8_t)i;
    np = kp_pack87(rw.raw, 256, rw.packed);
    nb = kp_unpack78(rw.packed, np, rw.back);
    {
        bool seven = true;
        for (i = 0; i < (int)np; i++)
            seven &= rw.packed[i] < 0x80;
        if (np == 256 + 37 && nb == 256 && seven && memcmp(rw.raw, rw.back, 256) == 0)
            mask |= 1u << CORE_CHK_PACK87;
    }

    memset(raw, 0, CORE_SELFTEST_RAW);
    raw[0] = 1;
    raw[1] = CORE_CHK_COUNT;
    raw[2] = (uint8_t)mask;
    raw[3] = (uint8_t)(mask >> 8);
    put32(raw + 4, crc_def);
    put32(raw + 8, crc_after);
    put32(raw + 12, st.tx_crc);
    raw[16] = (uint8_t)st.tx_bytes;
    raw[17] = (uint8_t)(st.tx_bytes >> 8);
    raw[18] = st.tx_frames;
    raw[19] = (uint8_t)((st.inited ? 1 : 0) | (guards ? 2 : 0) | (st.apo_due ? 4 : 0));
    put32(raw + 20, st.ticks);
    put32(raw + 24, st.ms);
    put32(raw + 28, hal_time_us());
}

/* 8-bit -> 7-bit, the 0x7D family's packing: a prefix byte whose bit i is bit 7 of the
 * i-th of up to 7 following bytes (REMOTE_PROTOCOL section 10.1, cave pack7()). */
uint16_t core_selftest_frame(const uint8_t raw[CORE_SELFTEST_RAW], uint8_t *out)
{
    static const uint8_t HEAD[7] = {0xF0, 0x7D, 0x43, 0x41, 0x37, 0x39, CORE_OP_SELFTEST | 0x40};
    uint16_t o = 7;
    uint32_t n = CORE_SELFTEST_RAW;
    memcpy(out, HEAD, sizeof HEAD);
    while (n) {
        uint32_t g = n < 7 ? n : 7, i;
        uint8_t hi = 0;
        for (i = 0; i < g; i++)
            if (raw[i] & 0x80)
                hi |= (uint8_t)(1u << i);
        out[o++] = hi;
        for (i = 0; i < g; i++)
            out[o++] = raw[i] & 0x7F;
        raw += g;
        n -= g;
    }
    out[o++] = 0xF7;
    return o;
}
