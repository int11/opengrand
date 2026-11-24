#include "ca79ng/kproto.h"

#include <string.h>

#include "ca79ng/util.h"

#define FN_REQUEST 0x02
#define FN_EOD 0x03
#define FN_WAIT 0x04
#define FN_CANCEL 0x05
#define FN_NACK 0x06
#define FN_ACK 0x07
#define FN_STAGE 0x09
#define FN_PARAM 0x10
#define FN_FILE 0x24
#define FN_MODE 0x31
#define FN_REMOTE 0x32
#define FN_IDENTITY 0x60
#define FN_SYSTEM 0x7E

#define NO_PORT ((uint8_t)MIDI_PORT_COUNT)

enum { KP_Q_PARAM = 1, KP_Q_END, KP_Q_PACKED };

/* Result 0xF0 of the stock VT-edit interception (FUN_4014DC0C): 7E 03 00 01 70 */
#define KP_RES_VT_MODE 0x00F0

_Static_assert(PRM_COUNT <= 256, "queue items keep the parameter id in one byte");

void kp_init(kproto_t *kp, param_store_t *store, const kp_identity_t *id, const kp_io_t *io)
{
    memset(kp, 0, sizeof *kp);
    kp->store = store;
    kp->id = *id;
    kp->io = *io;
    kp->dev_id = KP_DEV;
    for (int k = 0; k < KP_EX_COUNT; k++)
        kp->excl_owner[k] = NO_PORT;
}

static void tx(kproto_t *kp, midi_port_t port, const uint8_t *m, uint16_t n)
{
    if (kp->io.tx)
        kp->io.tx(kp->io.ctx, port, m, n);
}

/* piano -> controller frame F0 40 7F fn 04 1E body.. F7 into out; returns its length (0 = too long) */
static uint16_t build_frame(const kproto_t *kp, uint8_t *out, uint16_t cap, uint8_t fn, const uint8_t *body, uint16_t n)
{
    if (n + 7u > cap)
        return 0;
    out[0] = 0xF0; out[1] = 0x40; out[2] = kp->dev_id; out[3] = fn; out[4] = kp->id.group; out[5] = kp->id.machine;
    memcpy(out + 6, body, n);
    out[6 + n] = 0xF7;
    return (uint16_t)(n + 7);
}

static void send_frame(kproto_t *kp, midi_port_t port, uint8_t fn, const uint8_t *body, uint16_t n)
{
    uint8_t m[MIDI_SYSEX_TX_MAX];
    uint16_t l = build_frame(kp, m, sizeof m, fn, body, n);
    if (l)
        tx(kp, port, m, l);
}

void kp_send_frame(kproto_t *kp, midi_port_t port, uint8_t fn, const uint8_t *body, uint16_t n)
{
    send_frame(kp, port, fn, body, n);
}

void kp_send_result(kproto_t *kp, midi_port_t port, uint16_t code)
{
    const uint8_t body[4] = {0x03, (uint8_t)((code >> 14) & 0x7F), (uint8_t)((code >> 7) & 0x7F), (uint8_t)(code & 0x7F)};
    send_frame(kp, port, FN_SYSTEM, body, 4);
}

void kp_send_status(kproto_t *kp, uint8_t fn, uint8_t v1, uint8_t v2, uint8_t part, const uint8_t *data, uint8_t len)
{
    uint8_t body[3 + 16] = {v1, v2, part};
    if (len > 16)
        return;
    memcpy(body + 3, data, len);
    for (int p = 0; p < MIDI_PORT_COUNT; p++)
        if (kp->connected[p])
            send_frame(kp, (midi_port_t)p, fn, body, (uint16_t)(3 + len));
}

/* ---------------- per-port stop-and-wait transmitter ----------------
 * Every fn 0x10 value and fn 0x03 end the core originates goes through here: one frame in
 * flight per port until the controller echoes it with 07 (06 = resend at once), resent
 * every 250 ms, 16 sends at most (stock FUN_4014BD1C / FUN_4014BCAC). A non-home port that
 * never answers is unbound; the home port (panel) only loses that frame. */

static void port_leave(kproto_t *kp, midi_port_t port, bool invite_home);

static bool q_push(kproto_t *kp, midi_port_t port, uint8_t kind, uint8_t a, uint8_t b, uint8_t c)
{
    kp_port_t *s = &kp->port[port];
    /* a value already waiting is sent with the newest store contents: no second copy
     * (c = band + 1 of a banded key: each band is a value of its own) */
    for (unsigned i = 0; kind == KP_Q_PARAM && i < s->qn; i++) {
        const kp_qitem_t *q = &s->q[(s->qhead + i) % KP_TXQ_LEN];
        if (q->kind == KP_Q_PARAM && q->a == a && q->b == b && q->c == c)
            return true;
    }
    if (s->qn >= KP_TXQ_LEN) {
        s->dropped++;
        return false;
    }
    kp_qitem_t *q = &s->q[(s->qhead + s->qn) % KP_TXQ_LEN];
    q->kind = kind; q->a = a; q->b = b; q->c = c;
    s->qn++;
    return true;
}

/* stock order of the packed answer to 02 5F 00 (FUN_401525D4, emulator) */
static const uint8_t PACKED_KEYS[][2] = {
    {0x50, 0x29}, {0x50, 0x26}, {0x50, 0x2A}, {0x55, 0x60}, {0x55, 0x61}, {0x55, 0x00}, {0x53, 0x02},
    {0x55, 0x07}, {0x55, 0x08}, {0x55, 0x03}, {0x55, 0x09}, {0x55, 0x05}, {0x55, 0x11}, {0x55, 0x12},
    {0x55, 0x13}, {0x50, 0x00}, {0x50, 0x01}, {0x50, 0x03}, {0x50, 0x06}, {0x50, 0x28}, {0x50, 0x07},
    {0x50, 0x0A}, {0x50, 0x0F}, {0x50, 0x11}, {0x50, 0x12}, {0x50, 0x02}, {0x50, 0x04}, {0x50, 0x15},
    {0x50, 0x16}, {0x50, 0x05}, {0x50, 0x08}, {0x50, 0x09}, {0x55, 0x45}, {0x50, 0x0B}, {0x50, 0x0D},
};

/* 10 5F 00 7F <len v1 v2 part data..>.. (len counts itself) */
static uint16_t build_packed(const kproto_t *kp, uint8_t *out, uint16_t cap, uint8_t part)
{
    uint8_t body[3 + KP_PACKED_MAX] = {0x5F, 0x00, 0x7F};
    uint16_t o = 3;
    if (part == 0x7F) /* the stock answers 02 5F 00 7F with Main1 (part 00) entries */
        part = 0x00;
    int slot = param_slot_from_part(part);
    for (unsigned i = 0; slot >= 0 && i < sizeof PACKED_KEYS / sizeof PACKED_KEYS[0]; i++) {
        int id = param_find(FN_PARAM, PACKED_KEYS[i][0], PACKED_KEYS[i][1], part);
        uint8_t v[4];
        if (id < 0 || !param_get(kp->store, (param_id_t)id, slot, v))
            continue;
        uint8_t l = PARAM_DESC[id].len;
        if (o + 4u + l > sizeof body)
            break;
        body[o++] = (uint8_t)(4 + l);
        body[o++] = PACKED_KEYS[i][0];
        body[o++] = PACKED_KEYS[i][1];
        body[o++] = part;
        memcpy(body + o, v, l);
        o = (uint16_t)(o + l);
    }
    return build_frame(kp, out, cap, FN_PARAM, body, o);
}

/* Which descriptors a request 02 v1 v2 part returns (stock FUN_40152A34 / FUN_4015205C):
 * fn 0x10 values only, v1 5F = every group, v2 7F or 00 = the whole group; 51 02 never,
 * 56 0B (BeatCounter) not in the full dump. */
static bool req_match(const kp_port_t *s, const param_desc_t *d)
{
    if (d->fn != FN_PARAM || d->var_len)
        return false;
    if (d->v1 == 0x51 && d->v2 == 0x02)
        return false;
    if (s->req_v1 == 0x5F) {
        if (d->v1 == 0x56 && d->v2 == 0x0B)
            return false;
    } else if (d->v1 != s->req_v1) {
        return false;
    }
    return s->req_v2 == 0x7F || s->req_v2 == 0x00 || s->req_v2 == d->v2;
}

/* next (id, slot, band) of the request in progress; part 7F = every stored part. A banded key (User EQ)
 * answers one frame per stored band, lowest first, like the stock walk (band -1 = not banded). */
static bool req_next(kproto_t *kp, kp_port_t *s, int *id_out, int *slot_out, int *band_out)
{
    int want = s->req_part == 0x7F ? -1 : param_slot_from_part(s->req_part);
    if (s->req_part != 0x7F && want < 0)
        return false;
    for (; s->req_id < PRM_COUNT; s->req_id++, s->req_slot = 0) {
        const param_desc_t *d = &PARAM_DESC[s->req_id];
        if (!req_match(s, d))
            continue;
        for (; s->req_slot < PARAM_SLOT_COUNT; s->req_slot++) {
            if (want >= 0 && s->req_slot != want)
                continue;
            /* several descriptors share (v1, v2) and differ by part rule: answer for a part
             * only from the one a set to that part lands in (param_find), once */
            if ((kp->store->valid[s->req_id] & (1u << s->req_slot)) &&
                param_find(d->fn, d->v1, d->v2, param_part_from_slot(s->req_slot)) == (int)s->req_id) {
                int k = param_banded((param_id_t)s->req_id, s->req_slot);
                *id_out = s->req_id;
                *slot_out = s->req_slot;
                *band_out = -1;
                if (k >= 0 && kp->store->band_valid[k]) {
                    while (s->req_band < PARAM_BANDS && !(kp->store->band_valid[k] & (1u << s->req_band)))
                        s->req_band++;
                    if (s->req_band < PARAM_BANDS) {
                        *band_out = s->req_band++;
                        return true;
                    }
                    s->req_band = 0;
                    continue;
                }
                s->req_slot++;
                return true;
            }
        }
    }
    return false;
}

static void q_transmit(kproto_t *kp, midi_port_t port)
{
    kp_port_t *s = &kp->port[port];
    s->wait_ms = 0;
    tx(kp, port, s->frame, s->len);
}

/* load the next frame into the in-flight slot (false = nothing left) */
static bool q_load(kproto_t *kp, midi_port_t port)
{
    kp_port_t *s = &kp->port[port];
    while (s->qn || s->req_active) {
        uint8_t body[3 + 4];
        uint16_t l = 0;
        if (s->qn) {
            kp_qitem_t it = s->q[s->qhead];
            s->qhead = (uint8_t)((s->qhead + 1) % KP_TXQ_LEN);
            s->qn--;
            if (it.kind == KP_Q_PARAM) {
                uint8_t v[4];
                if (!(it.c ? param_get_band(kp->store, (param_id_t)it.a, it.b, (uint8_t)(it.c - 1), v)
                           : param_get(kp->store, (param_id_t)it.a, it.b, v)))
                    continue;
                const param_desc_t *d = &PARAM_DESC[it.a];
                if (d->len > 4)
                    continue;
                body[0] = d->v1; body[1] = d->v2; body[2] = param_part_from_slot(it.b);
                memcpy(body + 3, v, d->len);
                l = build_frame(kp, s->frame, sizeof s->frame, FN_PARAM, body, (uint16_t)(3 + d->len));
            } else if (it.kind == KP_Q_END) {
                body[0] = it.a; body[1] = it.b; body[2] = it.c;
                l = build_frame(kp, s->frame, sizeof s->frame, FN_EOD, body, 3);
            } else if (it.kind == KP_Q_PACKED) {
                l = build_packed(kp, s->frame, sizeof s->frame, it.a);
            }
        } else {
            int id, slot, band;
            if (req_next(kp, s, &id, &slot, &band)) {
                uint8_t v[4];
                if (band >= 0)
                    param_get_band(kp->store, (param_id_t)id, slot, (uint8_t)band, v);
                else
                    param_get(kp->store, (param_id_t)id, slot, v);
                const param_desc_t *d = &PARAM_DESC[id];
                body[0] = d->v1; body[1] = d->v2; body[2] = param_part_from_slot(slot);
                memcpy(body + 3, v, d->len);
                l = build_frame(kp, s->frame, sizeof s->frame, FN_PARAM, body, (uint16_t)(3 + d->len));
            } else { /* end of the answer: 03 v1 v2 7F (stock FUN_4014BDC8) */
                s->req_active = 0;
                body[0] = s->req_v1; body[1] = s->req_v2; body[2] = 0x7F;
                l = build_frame(kp, s->frame, sizeof s->frame, FN_EOD, body, 3);
            }
        }
        if (!l)
            continue;
        s->len = l;
        memcpy(s->key, s->frame + 6, 3);
        s->tries = 0;
        return true;
    }
    return false;
}

static void q_pump(kproto_t *kp, midi_port_t port)
{
    kp_port_t *s = &kp->port[port];
    if (s->len || !kp->connected[port])
        return;
    if (q_load(kp, port)) {
        s->tries = 1;
        q_transmit(kp, port);
    }
}

static void q_clear(kproto_t *kp, midi_port_t port)
{
    kp_port_t *s = &kp->port[port];
    s->len = 0;
    s->qn = 0;
    s->qhead = 0;
    s->req_active = 0;
}

/* a resend is due (time-out or NAK): 16 sends, then give up */
static void q_retry(kproto_t *kp, midi_port_t port)
{
    kp_port_t *s = &kp->port[port];
    if (s->tries >= KP_ACK_TRIES) {
        s->failed++;
        if (port == KP_HOME_PORT) { /* the panel is never unbound (stock exception for port 3) */
            s->len = 0;
            q_pump(kp, port);
        } else {
            port_leave(kp, port, true);
        }
        return;
    }
    s->tries++;
    q_transmit(kp, port);
}

static bool key_is(const kp_port_t *s, const uint8_t *m) { return s->len && memcmp(s->key, m + 6, 3) == 0; }

/* c = band + 1 of a banded key's value (0 = the store's value as param_get reads it) */
static void send_param(kproto_t *kp, midi_port_t port, param_id_t id, int slot, uint8_t c)
{
    if (port >= MIDI_PORT_COUNT || !kp->connected[port] || (unsigned)id >= PRM_COUNT || slot < 0 || slot >= PARAM_SLOT_COUNT)
        return;
    if (q_push(kp, port, KP_Q_PARAM, (uint8_t)id, (uint8_t)slot, c))
        q_pump(kp, port);
}

static void notify(kproto_t *kp, param_id_t id, int slot, uint8_t c, midi_port_t except)
{
    for (int p = 0; p < MIDI_PORT_COUNT; p++)
        if (kp->connected[p] && p != (int)except)
            send_param(kp, (midi_port_t)p, id, slot, c);
}

/* the queue tag of a value just stored: its band for a banded key */
static uint8_t band_tag(param_id_t id, int slot, const uint8_t *data, uint8_t len)
{
    return param_banded(id, slot) >= 0 && len && data[0] < PARAM_BANDS ? (uint8_t)(data[0] + 1) : 0;
}

void kp_send_param(kproto_t *kp, midi_port_t port, param_id_t id, int slot) { send_param(kp, port, id, slot, 0); }

void kp_notify(kproto_t *kp, param_id_t id, int slot, midi_port_t except) { notify(kp, id, slot, 0, except); }

void kp_local_set(kproto_t *kp, param_id_t id, int slot, const uint8_t *data, uint8_t len)
{
    if (param_put(kp->store, id, slot, data, len))
        notify(kp, id, slot, band_tag(id, slot, data, len), MIDI_PORT_COUNT);
}

static void start_request(kproto_t *kp, midi_port_t port, uint8_t v1, uint8_t v2, uint8_t part)
{
    kp_port_t *s = &kp->port[port];
    s->req_active = 1;
    s->req_v1 = v1; s->req_v2 = v2; s->req_part = part;
    s->req_id = 0;
    s->req_slot = 0;
    s->req_band = 0;
    q_pump(kp, port);
}

void kp_dump_all(kproto_t *kp, midi_port_t port)
{
    if (port >= MIDI_PORT_COUNT || !kp->connected[port])
        return;
    start_request(kp, port, 0x5F, 0x7F, 0x7F);
}

bool kp_tx_idle(const kproto_t *kp, midi_port_t port)
{
    const kp_port_t *s = &kp->port[port];
    return !s->len && !s->qn && !s->req_active;
}

/* ---------------- session ---------------- */

static bool file_active(const kproto_t *kp)
{
    return kp->file.state == 1 || kp->fa.pending || kp->fa.write_ready || kp->fa.tx_kind;
}

static void file_reset(kproto_t *kp)
{
    kp->file.state = 0;
    kp->fa.pending = 0;
    kp->fa.write_ready = false;
    kp->fa.tx_kind = 0;
}

/* `port` stops being a controller (DISCONNECT, link down, ACK time-out). Like the stock
 * FUN_4014C080 the panel (home port) is told 7E 01 01 so it takes control back. */
static void port_leave(kproto_t *kp, midi_port_t port, bool invite_home)
{
    if (!kp->connected[port])
        return;
    kp->connected[port] = false;
    q_clear(kp, port);
    for (int k = 0; k < KP_EX_COUNT; k++)
        if (kp->excl_owner[k] == port) {
            if (k == KP_EX_FILE)
                file_reset(kp);
            kp->excl_owner[k] = NO_PORT;
        }
    if (kp->fa.tx_kind && kp->fa.tx_port == port)
        kp->fa.tx_kind = 0;
    if (kp->io.session)
        kp->io.session(kp->io.ctx, port, false);
    if (invite_home) {
        const uint8_t body[2] = {0x01, 0x01};
        send_frame(kp, KP_HOME_PORT, FN_SYSTEM, body, 2);
    }
}

void kp_link_down(kproto_t *kp, midi_port_t port)
{
    if (port < MIDI_PORT_COUNT)
        port_leave(kp, port, true);
}

void kp_shutdown_notify(kproto_t *kp, midi_port_t except)
{
    const uint8_t body[2] = {0x00, 0x00};
    for (int p = 0; p < MIDI_PORT_COUNT; p++)
        if (kp->connected[p] && p != (int)except)
            send_frame(kp, (midi_port_t)p, FN_SYSTEM, body, 2);
}

static void connect_port(kproto_t *kp, midi_port_t port)
{
    kp_port_t *s = &kp->port[port];
    /* a (re)connecting controller starts a fresh session: nothing of the old one is resent */
    q_clear(kp, port);
    kp->connected[port] = true;
    /* 7E 02 a b: b carries 0x40 when this port has not changed the Sound since
     * (stock 0x4014A7B0..0x4014A7D4, flag 0x413CA3B0[port] read and cleared) */
    const uint8_t body[3] = {0x02, 0x00, (uint8_t)(s->remote_changed ? 0x00 : 0x40)};
    s->remote_changed = false;
    send_frame(kp, port, FN_SYSTEM, body, 3);
    if (kp->io.session)
        kp->io.session(kp->io.ctx, port, true);
}

static void handle_system(kproto_t *kp, midi_port_t port, const uint8_t *m, uint16_t n)
{
    if (n < 9)
        return;
    uint8_t a = m[6], b = m[7];
    if (a == 0x01 && b == 0x00) { /* DISCONNECT (stock 0x4014A800: unbind, port 3 gets 7E 01 01) */
        port_leave(kp, port, true);
    } else if (a == 0x00 && b == 0x00) {
        /* stock: no-op. OpenGrand remote power off (bound port only); the others are told */
        kp_shutdown_notify(kp, port);
        if (kp->io.power_off)
            kp->io.power_off(kp->io.ctx);
    } else if (a == 0x05 && b == 0x40) {
        const uint8_t body[4] = {0x05, 0x00, kp->id.sub1, kp->id.sub2};
        send_frame(kp, port, FN_SYSTEM, body, 4);
    } else if (a == 0x05 && b == 0x41) { /* 05 01 00 <dest>, two bytes as the stock (trace) */
        const uint8_t body[4] = {0x05, 0x01, 0x00, kp->id.destination};
        send_frame(kp, port, FN_SYSTEM, body, 4);
    } else if (a == 0x05 && b == 0x42) {
        const uint8_t body[3] = {0x05, 0x02, kp->id.language};
        send_frame(kp, port, FN_SYSTEM, body, 3);
    } else if (a == 0x04 && b == 0x41 && n == 9) {
        uint8_t body[2 + 24] = {0x04, 0x01};
        size_t l = kp->id.version ? strlen(kp->id.version) : 0;
        if (l > 24) l = 24;
        for (size_t i = 0; i < l; i++)
            body[2 + i] = (uint8_t)(kp->id.version[i] & 0x7F);
        send_frame(kp, port, FN_SYSTEM, body, (uint16_t)(2 + l));
    } else if (a == 0x04 && b >= 0x40 && b <= 0x43 && kp->io.info) {
        /* RomName / Version / Checksum / MemoryAddress (system_control.json
         * 04 00..03; request = code | 0x40). OpenGrand: an optional byte before
         * F7 selects the component (0 main, 1 DSP, 2 BLE module); the reply
         * then echoes it: 04 0k <component> <data>. */
        uint8_t comp = n >= 10 ? m[8] : 0;
        uint8_t body[3 + 48] = {0x04, (uint8_t)(b & 0x0F)};
        size_t o = 2;
        if (n >= 10)
            body[o++] = comp;
        int l = kp->io.info(kp->io.ctx, (uint8_t)(b & 0x0F), comp, body + o, sizeof body - o);
        if (l >= 0) {
            for (int i = 0; i < l; i++)
                body[o + (size_t)i] &= 0x7F;
            send_frame(kp, port, FN_SYSTEM, body, (uint16_t)(o + (size_t)l));
        } else {
            kp_send_result(kp, port, KP_RES_NO_DATA);
        }
    }
    /* 00 40 probe / 02 03 special-mode values: accepted silently */
}

/* ---------------- parameters ---------------- */

static void apply_staged(kproto_t *kp, midi_port_t port)
{
    for (int id = 0; id < PRM_COUNT; id++) {
        uint32_t bits = kp->staged[id];
        kp->staged[id] = 0;
        for (int slot = 0; bits && slot < PARAM_SLOT_COUNT; slot++) {
            uint8_t v[4];
            if (!(bits & (1u << slot)) || !param_get(kp->store, (param_id_t)id, slot, v))
                continue;
            bits &= ~(1u << slot);
            if (kp->io.param)
                kp->io.param(kp->io.ctx, (param_id_t)id, slot, FN_PARAM, v, PARAM_DESC[id].len, port);
        }
    }
}

/* Decode, check and apply one fn 0x10/0x31/0x32 parameter (the ACK is the caller's).
 * Returns false only when the Lock hook refused it. Unknown / invalid values are ignored
 * silently, like the stock lookup FUN_4014E8AC / length check FUN_4014EBA0. */
static bool apply_param(kproto_t *kp, midi_port_t port, uint8_t fn, uint8_t v1, uint8_t v2, uint8_t part,
                        const uint8_t *data, uint16_t dlen)
{
    uint8_t len = (uint8_t)CA_MIN(dlen, 255);
    int id = param_find(fn, v1, v2, part);
    int slot = param_slot_from_part(part);
    if (id < 0 && kp->io.ext && kp->io.ext(kp->io.ctx, port, fn, v1, v2, part, data, dlen))
        return true;
    if (id < 0 || slot < 0 || !param_validate((param_id_t)id, data, len)) {
        kp->rx_errors++;
        return true;
    }
    if (kp->io.accept && !kp->io.accept(kp->io.ctx, (param_id_t)id, slot, fn, data, len, port))
        return false;
    if (fn == FN_PARAM) {
        if (id == PRM_SOUND)
            kp->port[port].remote_changed = true;
        bool changed = param_put(kp->store, (param_id_t)id, slot, data, len);
        if (changed)
            notify(kp, (param_id_t)id, slot, band_tag((param_id_t)id, slot, data, len), port);
        if (kp->staging) { /* stock FUN_4015C410: kept in the settings block, applied at fn 0x03 */
            kp->staged[id] |= 1u << slot;
            return true;
        }
    }
    if (kp->io.param)
        kp->io.param(kp->io.ctx, (param_id_t)id, slot, fn, data, len, port);
    return true;
}

static void reply_ack(kproto_t *kp, midi_port_t port, uint8_t fn, const uint8_t *msg)
{
    uint8_t body[3] = {msg[6], msg[7], msg[8]};
    send_frame(kp, port, fn, body, 3);
}

/* would the Lock hook refuse this set? (decided before the ACK so a refusal can NAK) */
static bool lock_refuses(kproto_t *kp, midi_port_t port, uint8_t fn, const uint8_t *m, uint16_t n)
{
    if (!kp->io.accept || n < 10)
        return false;
    int id = param_find(fn, m[6], m[7], m[8]);
    int slot = param_slot_from_part(m[8]);
    uint8_t len = (uint8_t)CA_MIN(n - 10, 255);
    return id >= 0 && slot >= 0 && param_validate((param_id_t)id, m + 9, len) &&
           !kp->io.accept(kp->io.ctx, (param_id_t)id, slot, fn, m + 9, len, port);
}

static void handle_param(kproto_t *kp, midi_port_t port, const uint8_t *m, uint16_t n, uint8_t fn)
{
    if (n < 10) {
        kp->rx_errors++;
        return;
    }
    if (fn != FN_PARAM) {
        apply_param(kp, port, fn, m[6], m[7], m[8], m + 9, (uint16_t)(n - 10));
        return;
    }
    if (m[6] == 0x5F && m[7] == 0x00) {
        /* packed: 10 5F 00 part <len v1 v2 part data..>.., len counts itself, total <= 0xF5
         * (stock FUN_4014EBFC); one ACK for the frame */
        reply_ack(kp, port, FN_ACK, m);
        uint16_t end = (uint16_t)(n - 1), i = 9;
        if ((unsigned)(end - i) > KP_PACKED_MAX)
            return;
        while (i < end) {
            uint8_t l = m[i];
            if (l < 4 || i + l > end)
                break;
            apply_param(kp, port, FN_PARAM, m[i + 1], m[i + 2], m[i + 3], m + i + 4, (uint16_t)(l - 4));
            i = (uint16_t)(i + l);
        }
        return;
    }
    /* the stock ACKs every set from the bound port before looking it up (FUN_4014BE98);
     * OpenGrand Lock mode (REMOTE_PROTOCOL §9.9) still answers a refused set with NAK */
    if (lock_refuses(kp, port, fn, m, n)) {
        reply_ack(kp, port, FN_NACK, m);
        return;
    }
    reply_ack(kp, port, FN_ACK, m);
    apply_param(kp, port, fn, m[6], m[7], m[8], m + 9, (uint16_t)(n - 10));
}

static void handle_request(kproto_t *kp, midi_port_t port, const uint8_t *m, uint16_t n)
{
    if (n < 10)
        return;
    uint8_t v1 = m[6], v2 = m[7], part = m[8];
    kp_port_t *s = &kp->port[port];
    if (s->req_active) /* stock FUN_4014FD94: the request task is busy, the request is dropped */
        return;
    s->remote_changed = false; /* stock FUN_40151B90(bind-1, 0) after a request */
    if (v1 == 0x5F && v2 == 0x00) { /* packed answer, then 03 5F 00 7F */
        if (q_push(kp, port, KP_Q_PACKED, part, 0, 0))
            q_push(kp, port, KP_Q_END, 0x5F, 0x00, 0x7F);
        q_pump(kp, port);
        return;
    }
    if (!(v1 == 0x5F && v2 == 0x7F) && kp->io.request && kp->io.request(kp->io.ctx, port, v1, v2, part)) {
        q_push(kp, port, KP_Q_END, v1, v2, 0x7F);
        q_pump(kp, port);
        return;
    }
    start_request(kp, port, v1, v2, part);
}

/* ---------------- fn 0x24 file transfer ---------------- */

uint8_t kp_xor(const uint8_t *p, size_t n)
{
    uint8_t x = 0;
    while (n--)
        x ^= *p++;
    return x & 0x7F;
}

size_t kp_pack87(const uint8_t *in, size_t n, uint8_t *out)
{
    size_t o = 0;
    for (size_t i = 0; i < n; i += 7) {
        size_t g = CA_MIN((size_t)7, n - i);
        uint8_t hdr = 0;
        for (size_t k = 0; k < g; k++)
            hdr |= (uint8_t)((in[i + k] & 0x80) >> (k + 1));
        out[o++] = hdr;
        for (size_t k = 0; k < g; k++)
            out[o++] = in[i + k] & 0x7F;
    }
    return o;
}

size_t kp_unpack78(const uint8_t *in, size_t n, uint8_t *out)
{
    size_t o = 0;
    for (size_t i = 0; i < n;) {
        uint8_t hdr = in[i++];
        for (size_t k = 0; k < 7 && i < n; k++, i++)
            out[o++] = (uint8_t)(in[i] | (((hdr << (k + 1)) & 0x80)));
    }
    return o;
}

static void file_reply(kproto_t *kp, midi_port_t port, uint8_t code)
{
    send_frame(kp, port, FN_FILE, &code, 1);
}

/* ---- FileAccess transmit stream (piano -> controller), stop-and-wait ---- */

/* Builds a 02/03 packet: v1 counter <8to7 groups of up to 21 bytes> chk. */
static void fa_send_packet(kproto_t *kp, uint8_t v1, uint32_t counter, const uint8_t *raw, size_t n)
{
    kp_fa_t *a = &kp->fa;
    uint8_t *m = a->last;
    m[0] = 0xF0; m[1] = 0x40; m[2] = kp->dev_id; m[3] = FN_FILE; m[4] = kp->id.group; m[5] = kp->id.machine;
    m[6] = v1;
    m[7] = (uint8_t)(counter & 0x7F); /* the original encoder does not wrap; we keep 7 bits on the wire */
    size_t o = 8 + kp_pack87(raw, n, m + 8);
    m[o] = kp_xor(m + 1, o - 1);
    m[o + 1] = 0xF7;
    a->last_len = (uint16_t)(o + 2);
    kp->fa_wait_ms = 0;
    kp->fa_tries = 1;
    tx(kp, a->tx_port, m, a->last_len);
}

static void fa_send_simple(kproto_t *kp, midi_port_t port, uint8_t v1)
{
    file_reply(kp, port, v1);
}

/* Load the next list name into tx_name (with leading '\' and NUL). */
static bool fa_next_name(kproto_t *kp)
{
    kp_fa_t *a = &kp->fa;
    char name[KP_NAME_MAX];
    if (!kp->io.file_list || !kp->io.file_list(kp->io.ctx, a->tx_index, name, sizeof name))
        return false;
    a->tx_index++;
    size_t l = strlen(name);
    if (l > KP_NAME_MAX - 3)
        l = KP_NAME_MAX - 3;
    a->tx_name[0] = '\\';
    memcpy(a->tx_name + 1, name, l);
    a->tx_name[1 + l] = 0;
    a->tx_name_len = (uint16_t)(l + 2);
    a->tx_name_pos = 0;
    a->tx_counter = 0;
    return true;
}

/* Send the next frame of the active stream (called at start and per Ack). */
static void fa_advance(kproto_t *kp)
{
    kp_fa_t *a = &kp->fa;
    if (a->tx_kind == 1) {
        if (a->tx_pos >= a->tx_len) {
            fa_send_simple(kp, a->tx_port, KP_FA_EOD);
            a->tx_kind = 0;
            return;
        }
        size_t g = CA_MIN((size_t)21, (size_t)(a->tx_len - a->tx_pos));
        fa_send_packet(kp, KP_FA_DATA, a->tx_counter++, a->tx_data + a->tx_pos, g);
        a->tx_pos += (uint32_t)g;
    } else if (a->tx_kind == 2) {
        if (a->tx_name_pos >= a->tx_name_len && !fa_next_name(kp)) {
            fa_send_simple(kp, a->tx_port, KP_FA_END_LIST);
            a->tx_kind = 0;
            return;
        }
        size_t g = CA_MIN((size_t)21, (size_t)(a->tx_name_len - a->tx_name_pos));
        fa_send_packet(kp, KP_FA_NAME, a->tx_counter++, a->tx_name + a->tx_name_pos, g);
        a->tx_name_pos = (uint16_t)(a->tx_name_pos + g);
    }
}

void kp_file_send(kproto_t *kp, midi_port_t port, const uint8_t *data, uint32_t len)
{
    kp_fa_t *a = &kp->fa;
    a->tx_kind = 1;
    a->tx_port = port;
    a->tx_data = data;
    a->tx_len = len;
    a->tx_pos = 0;
    a->tx_counter = 0;
    /* TopOfData is stop-and-wait like every other packet: the first DataPacket goes out on its Ack and
     * a Nak resends it (stock FUN_4014B6E0 sends it through the Ack-waiting sender FUN_4014B638). */
    uint8_t *m = a->last;
    m[0] = 0xF0; m[1] = 0x40; m[2] = kp->dev_id; m[3] = FN_FILE; m[4] = kp->id.group; m[5] = kp->id.machine;
    m[6] = KP_FA_TOP; /* the byte count, like the stock (FUN_4014B6E0 is given the file size) */
    m[7] = (uint8_t)((len >> 21) & 0x7F);
    m[8] = (uint8_t)((len >> 14) & 0x7F);
    m[9] = (uint8_t)((len >> 7) & 0x7F);
    m[10] = (uint8_t)(len & 0x7F);
    m[11] = 0xF7;
    a->last_len = 12;
    kp->fa_wait_ms = 0;
    kp->fa_tries = 1;
    tx(kp, port, m, a->last_len);
}

bool kp_file_busy(const kproto_t *kp) { return kp->fa.tx_kind != 0; }

void kp_file_expect_name(kproto_t *kp, uint8_t op, const char *first)
{
    kp_fa_t *a = &kp->fa;
    a->pending = op;
    a->nlen = 0;
    a->name[0] = a->name2[0] = 0;
    a->names_needed = 1;
    if (op == KP_FA_RENAME && first) {
        strncpy(a->name, first, KP_NAME_MAX - 1);
        a->name[KP_NAME_MAX - 1] = 0;
    } else if (op == KP_FA_RENAME) {
        a->names_needed = 2;
    }
}

/* Strip the leading '\' and convert '\' separators to '/'. */
static void fa_copy_name(char *dst, const uint8_t *src, size_t n)
{
    size_t o = 0, i = 0;
    while (i < n && src[i] == '\\')
        i++;
    for (; i < n && o < KP_NAME_MAX - 1 && src[i]; i++)
        dst[o++] = src[i] == '\\' ? '/' : (char)src[i];
    dst[o] = 0;
}

static void fa_name_complete(kproto_t *kp, midi_port_t port)
{
    kp_fa_t *a = &kp->fa;
    char nm[KP_NAME_MAX];
    fa_copy_name(nm, a->nbuf, a->nlen);
    a->nlen = 0;
    if (a->pending == KP_FA_RENAME && a->name[0] == 0 && a->names_needed == 2) {
        memcpy(a->name, nm, sizeof nm);
        return; /* wait for the new name */
    }
    if (a->pending == KP_FA_RENAME)
        memcpy(a->name2, nm, sizeof nm);
    else
        memcpy(a->name, nm, sizeof nm);
    uint8_t op = a->pending;
    a->pending = 0;
    if (op == KP_FA_WRITE) {
        a->write_ready = true; /* data follows as 01 / 02.. / 7B */
        return;
    }
    if (!op)
        return;
    uint16_t r = kp->io.file_op ? kp->io.file_op(kp->io.ctx, port, op, a->name, a->name2) : KP_RES_ERROR;
    if (r != KP_RES_PENDING)
        kp_send_result(kp, port, r);
}

static void handle_file(kproto_t *kp, midi_port_t port, const uint8_t *m, uint16_t n)
{
    kp_file_rx_t *f = &kp->file;
    kp_fa_t *a = &kp->fa;
    if (n < 8)
        return;
    uint8_t v1 = m[6];
    switch (v1) {
    case KP_FA_TOP: /* TopOfDataLength: 4 x 7-bit, number of bytes */
        if (n < 12) break;
        f->expected_len = (uint32_t)m[7] << 21 | (uint32_t)m[8] << 14 | (uint32_t)m[9] << 7 | m[10];
        f->packets = 0;
        f->len = 0;
        f->state = 1;
        f->kind = a->write_ready ? KP_FILE_WRITE : KP_FILE_ANON;
        file_reply(kp, port, 0x7F);
        return;
    case KP_FA_DATA: { /* DataPacket: counter, up to 3 packed groups, xor */
        if (f->state != 1 || n < 10) {
            file_reply(kp, port, 0x7E);
            return;
        }
        uint8_t chk = m[n - 2];
        if (kp_xor(m + 1, (size_t)(n - 3)) != chk) {
            file_reply(kp, port, 0x7E);
            return;
        }
        uint8_t tmp[32];
        size_t got = kp_unpack78(m + 8, (size_t)(n - 10), tmp);
        if (f->len + got > KP_FILE_MAX) {
            f->state = 0;
            file_reply(kp, port, 0x7D);
            return;
        }
        memcpy(f->buf + f->len, tmp, got);
        f->len += (uint32_t)got;
        f->packets++;
        file_reply(kp, port, 0x7F);
        return;
    }
    case KP_FA_NAME: { /* FileName packets: counter, groups, xor; NUL ends a name */
        if (n < 10 || kp_xor(m + 1, (size_t)(n - 3)) != m[n - 2]) {
            file_reply(kp, port, 0x7E);
            return;
        }
        uint8_t tmp[32];
        size_t got = kp_unpack78(m + 8, (size_t)(n - 10), tmp);
        file_reply(kp, port, 0x7F);
        for (size_t i = 0; i < got; i++) {
            if (a->nlen < KP_NAME_MAX)
                a->nbuf[a->nlen++] = tmp[i];
            if (tmp[i] == 0)
                fa_name_complete(kp, port);
        }
        return;
    }
    /* The stock acknowledges every request (04-0B, 11) with 24 7F before acting on it
     * (0x4014A96C / 0x4014A98C / 0x4014A9B0), so the controller can wait for it. */
    case KP_FA_READ: case KP_FA_DELETE: case KP_FA_SELECT:
        file_reply(kp, port, 0x7F);
        kp_file_expect_name(kp, v1, NULL);
        return;
    case KP_FA_LOAD: /* 09 <song> <type>, then the name (stock FUN_4014F870 keeps d1/d2) */
    case KP_FA_SAVE: /* 0A <song> <type>, then the name */
    case KP_FA_WRITE: /* 05 <01> <00>, then the name */
        file_reply(kp, port, 0x7F);
        kp_file_expect_name(kp, v1, NULL);
        a->d1 = n > 8 ? m[7] : 0xFF;
        a->d2 = n > 9 ? m[8] : 0xFF;
        if (v1 == KP_FA_WRITE)
            a->write_ready = false;
        return;
    case KP_FA_RENAME:
        file_reply(kp, port, 0x7F);
        kp_file_expect_name(kp, v1, NULL);
        return;
    case KP_FA_LIST:
        file_reply(kp, port, 0x7F);
        a->tx_kind = 2;
        a->tx_port = port;
        a->tx_index = 0;
        a->tx_name_len = a->tx_name_pos = 0;
        if (kp->io.file_op) {
            uint16_t r = kp->io.file_op(kp->io.ctx, port, KP_FA_LIST, "", "");
            if (r != KP_RES_OK && r != KP_RES_PENDING) {
                a->tx_kind = 0;
                kp_send_result(kp, port, r);
                return;
            }
        }
        fa_advance(kp);
        return;
    case KP_FA_ACK: /* controller acknowledged our last packet */
        if (a->tx_kind && port == a->tx_port)
            fa_advance(kp);
        return;
    case KP_FA_NACK: /* resend */
        if (a->tx_kind && port == a->tx_port && a->last_len) {
            kp->fa_wait_ms = 0;
            kp->fa_tries++;
            tx(kp, port, a->last, a->last_len);
        }
        return;
    case KP_FA_WAIT:
        return;
    case KP_FA_EOD: /* EndOfData */
        if (f->state == 1 && f->len == f->expected_len) {
            file_reply(kp, port, 0x7F);
            uint8_t kind = f->kind;
            a->write_ready = false;
            if (kp->io.file)
                kp->io.file(kp->io.ctx, port, kind, f->buf, f->len);
        } else {
            file_reply(kp, port, 0x7E);
        }
        f->state = 0;
        return;
    case KP_FA_CANCEL: /* Cancel: aborts reception, pending names and our stream */
        f->state = 0;
        a->pending = 0;
        a->write_ready = false;
        a->tx_kind = 0;
        return;
    default:
        break;
    }
}

/* ---------------- exclusive operations ---------------- */

/* Is the activity `k` owned by a bound port other than `port`? */
static bool excl_other(kproto_t *kp, uint8_t k, midi_port_t port)
{
    uint8_t o = kp->excl_owner[k];
    if (o == NO_PORT || o == port || !kp->connected[o])
        return false;
    switch (k) {
    case KP_EX_FILE:
        return file_active(kp);
    case KP_EX_RECORDER:
        return kp->io.excl_active && kp->io.excl_active(kp->io.ctx, KP_EX_RECORDER);
    default:
        return kp->excl_idle_ms[k] < KP_EXCL_IDLE_MS;
    }
}

static void excl_take(kproto_t *kp, uint8_t k, midi_port_t port)
{
    kp->excl_owner[k] = port;
    kp->excl_idle_ms[k] = 0;
}

/* The exclusive activity a frame belongs to, or KP_EX_COUNT. */
static uint8_t excl_kind(uint8_t fn, const uint8_t *m, uint16_t n)
{
    if (fn == FN_FILE)
        return KP_EX_FILE;
    if (n >= 9 && m[6] == 0x5A && (fn == FN_PARAM || fn == FN_MODE || fn == FN_REMOTE || fn == FN_REQUEST))
        return KP_EX_TOUCHCURVE;
    if (n >= 9 && m[6] == 0x52 && (fn == FN_PARAM || fn == FN_MODE || fn == FN_REMOTE))
        return KP_EX_RECORDER;
    return KP_EX_COUNT;
}

/* Another port owns it: answer like the stock answers a busy worker — fn 0x10 is still
 * ACKed (the controller's handshake would otherwise give up the session), file requests
 * get 24 7F first (0x4014A96C), then Result Busy 7E 03 01 00 00. */
static void reply_busy(kproto_t *kp, midi_port_t port, uint8_t fn, const uint8_t *m, uint16_t n)
{
    if (fn == FN_FILE) {
        uint8_t v1 = n > 7 ? m[6] : 0;
        if (v1 > KP_FA_EOD)
            return; /* 7C..7F: wait / cancel / acknowledgements of somebody else's stream */
        if ((v1 >= KP_FA_READ && v1 <= KP_FA_SAVE) || v1 == KP_FA_LIST)
            file_reply(kp, port, KP_FA_ACK);
    } else if (fn == FN_PARAM && n >= 10) {
        reply_ack(kp, port, FN_ACK, m);
    }
    kp_send_result(kp, port, KP_RES_BUSY);
}

/* ---------------- VT edit interception ---------------- */

/* d0 0x5A (user touch curve) and d0 0x50 + d1 0C/0E/10/17 (per-note VT) are judged before
 * the function code (stock 0x4014A370..0x4014A3B0, 0x4014AC0C): outside the edit mode —
 * the base parameter not on User — the answer is 7E 03 00 01 70. */
static bool vt_intercept(const uint8_t *m, uint16_t n)
{
    return n >= 9 && (m[6] == 0x5A || (m[6] == 0x50 && (m[7] == 0x0C || m[7] == 0x0E || m[7] == 0x10 || m[7] == 0x17)));
}

static bool vt_edit_mode(const kproto_t *kp, const uint8_t *m)
{
    param_id_t base;
    if (m[6] == 0x5A)
        base = PRM_TOUCH_CURVE;
    else if (m[7] == 0x0C)
        base = PRM_STRETCH_TUNING;
    else if (m[7] == 0x0E)
        base = PRM_TEMPERAMENT;
    else if (m[7] == 0x10)
        base = PRM_KEY_VOLUME;
    else
        base = PRM_VOICING;
    return param_get1(kp->store, base, SLOT_MAIN1, 0) == 0x40; /* User */
}

/* ---------------- time ---------------- */

void kp_tick(kproto_t *kp, uint32_t ms)
{
    for (int p = 0; p < MIDI_PORT_COUNT; p++) {
        kp_port_t *s = &kp->port[p];
        if (!s->len)
            continue;
        uint32_t w = s->wait_ms + ms;
        s->wait_ms = (uint16_t)CA_MIN(w, 0xFFFFu);
        if (w >= KP_ACK_TIMEOUT_MS)
            q_retry(kp, (midi_port_t)p);
    }
    kp_fa_t *a = &kp->fa;
    if (a->tx_kind && a->last_len) {
        uint32_t w = kp->fa_wait_ms + ms;
        kp->fa_wait_ms = (uint16_t)CA_MIN(w, 0xFFFFu);
        if (w >= KP_ACK_TIMEOUT_MS) {
            kp->fa_wait_ms = 0;
            if (kp->fa_tries >= KP_ACK_TRIES) { /* stock FUN_4014B638: 16 sends, then unbind */
                midi_port_t port = a->tx_port;
                a->tx_kind = 0;
                if (port != KP_HOME_PORT)
                    port_leave(kp, port, true);
            } else {
                kp->fa_tries++;
                tx(kp, a->tx_port, a->last, a->last_len);
            }
        }
    }
    for (int k = 0; k < KP_EX_COUNT; k++)
        if (kp->excl_owner[k] != NO_PORT && kp->excl_idle_ms[k] < KP_EXCL_IDLE_MS)
            kp->excl_idle_ms[k] += ms;
    /* an abandoned file operation (no frame from its owner for a minute) frees the worker */
    if (kp->excl_owner[KP_EX_FILE] != NO_PORT && kp->excl_idle_ms[KP_EX_FILE] >= KP_EXCL_IDLE_MS && !kp->fa.tx_kind) {
        file_reset(kp);
        kp->excl_owner[KP_EX_FILE] = NO_PORT;
    }
}

/* ---------------- dispatch ---------------- */

static void handle_ack(kproto_t *kp, midi_port_t port, uint8_t fn, const uint8_t *m, uint16_t n)
{
    kp_port_t *s = &kp->port[port];
    if (n < 10 || !key_is(s, m))
        return;
    if (fn == FN_ACK) {
        s->len = 0;
        q_pump(kp, port);
    } else if (fn == FN_NACK) {
        q_retry(kp, port);
    } else if (fn == FN_WAIT) { /* stock FUN_4014C1DC: the controller needs longer */
        s->wait_ms = 0;
    }
}

bool kp_handle_sysex(kproto_t *kp, midi_port_t port, const uint8_t *m, uint16_t n)
{
    if (n < 4 || m[0] != 0xF0 || m[n - 1] != 0xF7 || port >= MIDI_PORT_COUNT)
        return false;
    if (m[1] == 0x7E || m[1] == 0x7F || m[1] == 0x41) {
        if (kp->io.universal)
            kp->io.universal(kp->io.ctx, m, n);
        return true;
    }
    if (m[1] != 0x40 || n < 7)
        return false;
    uint8_t fn = m[3];
    if (fn == FN_IDENTITY) { /* reply bytes as observed in the original handler (0x40149d30) */
        const uint8_t r[7] = {0xF0, 0x40, kp->dev_id, 0x61, 0x04, 0x02, 0xF7};
        tx(kp, port, r, sizeof r);
        return true;
    }
    /* only frames from a controller (08 02) are accepted */
    if (m[4] != 0x08 || m[5] != 0x02) {
        kp->rx_errors++;
        return true;
    }
    kp->rx_frames++;
    if (fn == FN_SYSTEM && n >= 9 && m[6] == 0x01 && m[7] == 0x01) { /* CONNECT: any port */
        connect_port(kp, port);
        return true;
    }
    if (!kp->connected[port]) /* the stock answers nothing else from an unbound port */
        return true;
    if (fn != FN_ACK && fn != FN_NACK && vt_intercept(m, n) && !vt_edit_mode(kp, m)) {
        kp_send_result(kp, port, KP_RES_VT_MODE);
        return true;
    }
    uint8_t ek = excl_kind(fn, m, n);
    if (ek < KP_EX_COUNT) {
        if (excl_other(kp, ek, port)) {
            reply_busy(kp, port, fn, m, n);
            return true;
        }
        excl_take(kp, ek, port);
        if (ek == KP_EX_TOUCHCURVE && fn == FN_MODE && m[7] == 0x09) /* 31 5A 09 EditExit */
            kp->excl_owner[ek] = NO_PORT;
    }
    switch (fn) {
    case FN_SYSTEM: handle_system(kp, port, m, n); break;
    case FN_PARAM:
    case FN_MODE:
    case FN_REMOTE: handle_param(kp, port, m, n, fn); break;
    case FN_REQUEST: handle_request(kp, port, m, n); break;
    case FN_FILE: handle_file(kp, port, m, n); break;
    case FN_ACK:
    case FN_NACK:
    case FN_WAIT: handle_ack(kp, port, fn, m, n); break;
    case FN_CANCEL: /* stock 0x413CA3EF: the piano stops what it is sending this port */
        kp->port[port].req_active = 0;
        kp->port[port].len = 0;
        q_pump(kp, port);
        break;
    case FN_STAGE: { /* stock FUN_4015D5A8: staging on, ask the controller for its state */
        kp->staging = true;
        const uint8_t body[3] = {0x5F, 0x7F, 0x7F};
        send_frame(kp, port, FN_REQUEST, body, 3);
        break;
    }
    case FN_EOD: /* stock FUN_4015D5D4: staging off; OpenGrand then applies what was staged */
        if (kp->staging) {
            kp->staging = false;
            apply_staged(kp, port);
        }
        break;
    default: break;
    }
    return true;
}
