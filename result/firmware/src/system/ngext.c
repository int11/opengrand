/* OpenGrand — extension SysEx (manufacturer ID 0x7D) answered by the core itself.
 *
 *   request  F0 7D 43 41 37 39 <op> [args] F7
 *   reply    F0 7D 43 41 37 39 <op|0x40> [data] F7      to the port the request came from
 *
 * REMOTE_PROTOCOL.md §10 (family) and §10.4 (ops 0x18 power policy, 0x19 standby).
 * On the instrument the cave answers the other ops (identify, probes, OTA); when the
 * core runs there, the cave hands 0x18 / 0x19 to ng_sysex and ORs NG_FEAT1_POWER into
 * its own Identify. On the host (simulator, tests) the core also answers Identify, so
 * the app can find the feature without a cave. Unknown ops stay silent, like the cave. */
#include <string.h>

#include "piano_int.h"

#define NG_ID 0x7D
#define NG_VERSION 1
#define OP_IDENTIFY 0x01
#define OP_POWER 0x18
#define OP_STANDBY 0x19

/* result codes of 0x18 / 0x19 */
enum { NG_OK = 0, NG_BAD_VALUE = 1, NG_NOT_BOUND = 2, NG_BAD_STATE = 3, NG_SAVE_FAILED = 4, NG_BUSY = 6, NG_APO_OFF = 7 };

static const uint8_t TAG[4] = {0x43, 0x41, 0x37, 0x39}; /* "CA79" */

/* 7-in-8 packing of the cave (prefix byte bit k = bit 7 of the k-th byte that follows),
 * app ngext.ts unpack7 — not kproto's kp_pack87, whose prefix bit order is reversed. */
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

static void reply(piano_t *p, midi_port_t port, uint8_t op, const uint8_t *d, size_t n)
{
    uint8_t m[48];
    if (n > sizeof m - 8)
        return;
    m[0] = 0xF0;
    m[1] = NG_ID;
    memcpy(m + 2, TAG, 4);
    m[6] = (uint8_t)(op | 0x40);
    for (size_t i = 0; i < n; i++)
        m[7 + i] = d[i] & 0x7F;
    m[7 + n] = 0xF7;
    if (p->plat.midi_tx)
        p->plat.midi_tx(p->plat.ctx, port, m, (uint16_t)(8 + n));
}

/* 58 <sub> <rc> <pack7 raw policy> */
static void policy_reply(piano_t *p, midi_port_t port, uint8_t sub, uint8_t rc)
{
    uint8_t raw[POWER_POLICY_RAW], d[3 + POWER_POLICY_RAW + 2];
    power_policy_encode(&p->policy, raw);
    d[0] = sub;
    d[1] = rc;
    size_t n = 2 + pack7(raw, sizeof raw, d + 2);
    reply(p, port, OP_POWER, d, n);
}

static uint8_t sb_state_code(standby_state_t s) { return (uint8_t)s; /* 0 on .. 4 off, standby.h order */ }

/* 59 <sub> <rc> <state> */
static void standby_reply(piano_t *p, midi_port_t port, uint8_t sub, uint8_t rc)
{
    const uint8_t d[3] = {sub, rc, sb_state_code(p->sb.state)};
    reply(p, port, OP_STANDBY, d, sizeof d);
}

static bool bound(const piano_t *p, midi_port_t port) { return port < MIDI_PORT_COUNT && p->kp.connected[port]; }

static void op_power(piano_t *p, midi_port_t port, const uint8_t *a, size_t n)
{
    if (n < 1)
        return;
    uint8_t sub = a[0];
    switch (sub) {
    case 0x00: /* get */
        policy_reply(p, port, sub, NG_OK);
        return;
    case 0x01: { /* set + save */
        if (!bound(p, port)) {
            policy_reply(p, port, sub, NG_NOT_BOUND);
            return;
        }
        uint8_t raw[POWER_POLICY_RAW + 7];
        size_t got = unpack7(a + 1, n - 1, raw, sizeof raw);
        power_policy_t pol = p->policy;
        if (!power_policy_decode(&pol, raw, (uint16_t)got)) {
            policy_reply(p, port, sub, NG_BAD_VALUE);
            return;
        }
        power_timer_activity(&p->power); /* a user setting is activity */
        bool saved = piano_set_power_policy(p, &pol, true);
        policy_reply(p, port, sub, saved ? NG_OK : NG_SAVE_FAILED);
        return;
    }
    case 0x02: { /* owner defaults + save */
        if (!bound(p, port)) {
            policy_reply(p, port, sub, NG_NOT_BOUND);
            return;
        }
        power_policy_t pol;
        power_policy_default(&pol);
        power_timer_activity(&p->power);
        bool saved = piano_set_power_policy(p, &pol, true);
        policy_reply(p, port, sub, saved ? NG_OK : NG_SAVE_FAILED);
        return;
    }
    default:
        return; /* unknown sub-op: silent */
    }
}

static void op_standby(piano_t *p, midi_port_t port, const uint8_t *a, size_t n)
{
    if (n < 1)
        return;
    uint8_t sub = a[0];
    switch (sub) {
    case 0x00: /* query */
        standby_reply(p, port, sub, NG_OK);
        return;
    case 0x01: /* enter: a bound controller only, like the remote power-off */
        if (!bound(p, port)) {
            standby_reply(p, port, sub, NG_NOT_BOUND);
            return;
        }
        standby_reply(p, port, sub, piano_standby_enter(p) ? NG_OK : NG_BAD_STATE);
        return;
    case 0x02: /* wake: any port (the app may not be bound yet after its own reconnect) */
        standby_reply(p, port, sub, piano_standby_wake(p, SB_WAKE_BLE_OP) ? NG_OK : NG_BAD_STATE);
        return;
    case 0x03: { /* remote off (feature 2): a bound controller; 59 03 <rc> <state> <why> */
        uint8_t rc, why = POWER_OFF_OK;
        bool bad = false;
        if (!bound(p, port)) {
            rc = NG_NOT_BOUND;
        } else {
            why = piano_remote_off(p, &bad);
            rc = why != POWER_OFF_OK ? NG_BUSY : bad ? NG_BAD_STATE : NG_OK;
        }
        const uint8_t d[4] = {sub, rc, sb_state_code(p->sb.state), why};
        reply(p, port, OP_STANDBY, d, sizeof d);
        return;
    }
    case 0x04: { /* APO test shortcut: the APO expires in s s (10..600, 0 = the full menu time again), RAM
                  * only; 59 04 <rc> <state> <s_hi7> <s_lo7> <menu setting> <flags: 1 = BT hold holds now> - as the cave */
        uint32_t secs = n >= 3 ? ((uint32_t)(a[1] & 0x7Fu) << 7) | (a[2] & 0x7Fu) : n == 2 ? (uint32_t)(a[1] & 0x7Fu) : 0xFFFFu;
        uint32_t lim = power_timer_limit_ms(p->power.setting);
        uint8_t rc;
        if (!bound(p, port))
            rc = NG_NOT_BOUND;
        else if (!lim)
            rc = NG_APO_OFF;
        else if (!power_apo_test_secs_ok(secs))
            rc = NG_BAD_VALUE;
        else if (!p->powered || p->sb.state != SB_ON)
            rc = NG_BAD_STATE;
        else {
            p->power.idle_ms = secs ? lim - secs * 1000u : 0; /* the menu time minus what is left (the hold still holds) */
            rc = NG_OK;
        }
        const uint8_t d[7] = {sub, rc, sb_state_code(p->sb.state), (uint8_t)((secs >> 7) & 0x7Fu), (uint8_t)(secs & 0x7Fu),
                              p->power.setting, (uint8_t)(p->apo_hold.holding ? 1 : 0)};
        reply(p, port, OP_STANDBY, d, sizeof d);
        return;
    }
    default:
        return;
    }
}

bool ng_sysex(piano_t *p, midi_port_t port, const uint8_t *m, uint16_t n)
{
    if (n < 2 || m[0] != 0xF0 || m[1] != NG_ID)
        return false;
    /* tag and length first: the stock receiver appends F7 to truncated messages */
    if (n < 8 || memcmp(m + 2, TAG, 4) != 0 || m[n - 1] != 0xF7)
        return true;
    uint8_t op = m[6];
    const uint8_t *a = m + 7;
    size_t na = (size_t)n - 8;
    switch (op) {
    case OP_IDENTIFY: {
        /* 41 <ngver> <base_maj> <base_min> <feat0> <feat1>: the host core has no cave, so it
         * reports only what it answers itself.  No feat2 byte: the third feature byte (BT port 0x01,
         * session core 0x02) is cave-only, and an absent feat2 reads as 0 (REMOTE_PROTOCOL section 10) */
        const uint8_t d[5] = {NG_VERSION, 2, 19, NG_FEAT0_CORE, NG_FEAT1_POWER | NG_FEAT1_TVWAKE};
        reply(p, port, op, d, sizeof d);
        return true;
    }
    case OP_POWER:
        op_power(p, port, a, na);
        return true;
    case OP_STANDBY:
        op_standby(p, port, a, na);
        return true;
    default:
        return true; /* silent, like the cave */
    }
}

/* 59 7F 00 <state>: standby entered / left, to every bound controller except the panel
 * (the stock panel app's reaction to a 0x7D frame is [unverified]; its own wake path is
 * the touch notice 32 51 39). Never 7E 00 00 — that would block the panel's settings save. */
void ng_standby_notice(piano_t *p, standby_state_t st)
{
    const uint8_t d[3] = {0x7F, NG_OK, sb_state_code(st)};
    for (int port = 0; port < MIDI_PORT_COUNT; port++)
        if (port != MIDI_PORT_PANEL && p->kp.connected[port])
            reply(p, (midi_port_t)port, OP_STANDBY, d, sizeof d);
}
