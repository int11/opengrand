/* OpenGrand - Phase 3-2 session core, stage 1.  See kpsess.h. */
#include "ca79ng/kpsess.h"

#include <string.h>

#define FN_REQ 0x02
#define FN_WAIT 0x04
#define FN_CANCEL 0x05
#define FN_NAK 0x06
#define FN_ACK 0x07
#define FN_FILE 0x24
#define FN_SYS 0x7E

static bool due(uint32_t now, uint32_t t) { return (int32_t)(now - t) >= 0; }

/* FUN_4014C080: F0 40 7F 7E 04 1E 01 01 F7 to port 3 only (FUN_40144CD0) - "CONNECT again" */
static const uint8_t invite[] = {0xF0, 0x40, 0x7F, 0x7E, 0x04, 0x1E, 0x01, 0x01, 0xF7};

uint32_t kps_clock(kps_clock_t *c, uint16_t tick2ms)
{
    if (!c->init) {
        c->init = true;
        c->last = tick2ms;
        c->ms = (uint32_t)tick2ms * 2u;
        return c->ms;
    }
    c->ms += (uint32_t)(uint16_t)(tick2ms - c->last) * 2u;   /* modulo 2^16: wrap-safe */
    c->last = tick2ms;
    return c->ms;
}

void kps_init(kps_t *s, const kps_io_t *io, uint32_t now_ms)
{
    memset(s, 0, sizeof *s);
    s->io = *io;
    s->enabled = true;
    s->now = now_ms;
    s->last_binding = io->binding(io->ctx);
    if (s->last_binding >= 1 && s->last_binding <= KPS_PORTS)
        s->bound = (uint8_t)(1u << (s->last_binding - 1));
    for (uint8_t p = 0; p < KPS_PORTS; p++)
        if (!io->link_up || io->link_up(io->ctx, p))
            s->link_prev |= (uint8_t)(1u << p);
}

bool kps_resume(kps_t *s)
{
    uint8_t b = s->io.binding(s->io.ctx);
    if (!s->enabled || b < 1 || b > KPS_PORTS || kps_bound(s, KPS_HOME))
        return false;
    s->io.tx(s->io.ctx, KPS_HOME, invite, sizeof invite);
    if (s->st.resume_invites != 0xFFFFu)
        s->st.resume_invites++;
    return true;
}

bool kps_bound(const kps_t *s, uint8_t port)
{
    return port < KPS_PORTS && (s->bound & (1u << port)) != 0;
}

static void abort_port(kps_t *s, uint8_t p)
{
    memset(&s->port[p], 0, sizeof s->port[p]);
}

/* the port is gone: its request, its frame in flight and its fan-out queue */
static void forget(kps_t *s, uint8_t p)
{
    abort_port(s, p);
    s->fan[p].cnt = 0;
}

static void drop(kps_t *s, uint8_t p)
{
    s->bound &= (uint8_t)~(1u << p);
    forget(s, p);
}

/* unbound without being asked (16 failures, link down, the stock's own unbind) */
static void unbind(kps_t *s, uint8_t p)
{
    if (kps_bound(s, p))
        s->st.unbinds++;
    s->bound &= (uint8_t)~(1u << p);
    forget(s, p);
}

static void lease_end(kps_t *s, bool give_back);

void kps_enable(kps_t *s, bool on)
{
    if (!on) {
        if (s->lease)
            lease_end(s, s->io.binding(s->io.ctx) == s->lease);
        for (uint8_t p = 0; p < KPS_PORTS; p++)
            forget(s, p);
        s->set_on = false;
        kps_tap_reset(s);
    }
    s->enabled = on;
}

/* the table follows what the stock does to its own binding (CONNECT it answered, DISCONNECT,
 * link down, its 16-failure unbind, the factory frame) */
static void sync(kps_t *s)
{
    uint8_t b = s->io.binding(s->io.ctx);
    if (b == s->last_binding)
        return;
    if (b >= 1 && b <= KPS_PORTS)
        s->bound |= (uint8_t)(1u << (b - 1));
    else if (s->last_binding >= 1 && s->last_binding <= KPS_PORTS)
        unbind(s, (uint8_t)(s->last_binding - 1));
    s->last_binding = b;
}

/* FUN_4014AD04 routing for one destination port: the panel with port 3 forwarding on gets 0, 1, 2 first */
static void out(kps_t *s, uint8_t p, const uint8_t *m, uint16_t n)
{
    if (p == KPS_HOME && s->io.forward && s->io.forward(s->io.ctx))
        for (uint8_t q = 0; q < 3; q++)
            s->io.tx(s->io.ctx, q, m, n);
    s->io.tx(s->io.ctx, p, m, n);
}

static void send(kps_t *s, uint8_t p)
{
    kps_port_t *pp = &s->port[p];
    if (pp->fan)   /* a notification goes to its own port only (no port 3 forwarding copies) */
        s->io.tx(s->io.ctx, p, pp->frame, pp->n);
    else
        out(s, p, pp->frame, pp->n);
    pp->tries++;
    pp->nakd = false;
    pp->due = s->now + KPS_WAIT_MS;
}

static void start_frame(kps_t *s, uint8_t p, const uint8_t *m, uint16_t n, bool fan)
{
    kps_port_t *pp = &s->port[p];
    if (n > KPS_FRAME_MAX)
        n = KPS_FRAME_MAX;
    memcpy(pp->frame, m, n);
    pp->n = n;
    pp->busy = true;
    pp->fan = fan;
    pp->tries = 0;
    pp->waits = 0;
    send(s, p);
}

/* FUN_40152A34: descriptor i is skipped without a call (0x69; 0x76 / 0x8E for v1 5F) or answered */
static bool skip(const kps_t *s, const kps_port_t *pp, uint8_t i)
{
    if (i == 0x69 || (pp->v1 == 0x5F && (i == 0x76 || i == 0x8E)))
        return true;
    return !s->io.match(s->io.ctx, i, pp->v1, pp->v2);
}

/* the next frame of the request on port p: a value, else End; nothing when it was cancelled */
static void advance(kps_t *s, uint8_t p)
{
    kps_port_t *pp = &s->port[p];
    uint8_t f[KPS_FRAME_MAX];
    while (pp->i < KPS_DESC_N) {
        int r;
        if (pp->j == 0 && skip(s, pp, pp->i)) {
            pp->i++;
            continue;
        }
        r = pp->j < 16 ? s->io.value(s->io.ctx, p, pp->cc, pp->i, pp->part, pp->j, f) : -1;
        if (r > 0) {
            pp->j++;
            s->st.values++;
            start_frame(s, p, f, (uint16_t)r, false);
            return;
        }
        if (r == 0) {
            pp->j++;
            continue;
        }
        /* descriptor done: FUN_40151D08 after each FUN_401526DC */
        if (pp->cancel) {
            abort_port(s, p);
            return;
        }
        pp->i++;
        pp->j = 0;
    }
    /* FUN_4014BDC8: F0 40 cc 03 04 1E v1 v2 7F F7 */
    f[0] = 0xF0;
    f[1] = 0x40;
    f[2] = pp->cc;
    f[3] = 0x03;
    f[4] = 0x04;
    f[5] = 0x1E;
    f[6] = pp->v1;
    f[7] = pp->v2;
    f[8] = 0x7F;
    f[9] = 0xF7;
    pp->end = true;
    s->st.ends++;
    start_frame(s, p, f, 10, false);
}

/* drop q[0] (the cave payload has memcpy / memset but no memmove) */
static void pop_front(kps_fanq_t *q)
{
    q->cnt--;
    for (uint8_t i = 0; i < q->cnt; i++)
        q->q[i] = q->q[i + 1];
}

/* the port's sender is free: a waiting fan-out frame first, else the request's next frame */
static void next(kps_t *s, uint8_t p)
{
    kps_port_t *pp = &s->port[p];
    kps_fanq_t *q = &s->fan[p];
    if (pp->busy || !s->enabled)
        return;
    if (q->cnt && kps_bound(s, p)) {
        kps_fan_t f = q->q[0];
        pop_front(q);
        s->st.fan_sent++;
        start_frame(s, p, f.frame, f.n, true);
        return;
    }
    if (pp->active)
        advance(s, p);
}

static void fail(kps_t *s, uint8_t p)
{
    kps_port_t *pp = &s->port[p];
    if (pp->fan)
        s->st.fan_fail++;
    else
        s->st.failed++;
    if (p == KPS_HOME) {   /* FUN_4014BD1C: binding 4 is never released */
        if (pp->fan) {     /* that notification is lost; a request in progress goes on */
            pp->busy = false;
            pp->fan = false;
        } else {
            abort_port(s, p);
        }
        next(s, p);
        return;
    }
    if (s->io.binding(s->io.ctx) == (uint8_t)(p + 1)) {
        s->io.stock_unbind(s->io.ctx);
        s->last_binding = s->io.binding(s->io.ctx);
        unbind(s, p);
        if (!kps_bound(s, KPS_HOME))   /* nobody holds the stock binding: the panel may take it */
            s->io.tx(s->io.ctx, KPS_HOME, invite, sizeof invite);
        return;
    }
    unbind(s, p);
}

/* key of an ACK-type frame: FUN_4014C148 / FUN_4014BB54 - 2 bytes for v1 0x70, else 3, then F7 */
static bool key_match(const kps_port_t *pp, const uint8_t *m, uint32_t n)
{
    uint32_t k = m[6] == 0x70 ? 2 : 3;
    if (!pp->busy || n < 7 + k || m[6 + k] != 0xF7)
        return false;
    return memcmp(m + 6, pp->frame + 6, k) == 0;
}

static kps_verdict_t on_ack(kps_t *s, uint8_t p, const uint8_t *m, uint32_t n, bool rep)
{
    kps_port_t *pp = &s->port[p];
    uint8_t fn = m[3];
    if (key_match(pp, m, n)) {
        bool fan = pp->fan;
        if (fn == FN_ACK) {
            if (fan)
                s->st.fan_acks++;
            else
                s->st.acks++;
            pp->busy = false;
            pp->fan = false;
            if (!fan && pp->end) {
                s->io.remote_clear(s->io.ctx, p);   /* 0x40152B5C: answered in full */
                abort_port(s, p);
            }
            next(s, p);
        } else if (fn == FN_NAK) {
            s->st.naks++;
            pp->nakd = true;
            pp->due = s->now + KPS_NAK_MS;
        } else {
            s->st.waits++;
            if (pp->waits < KPS_WAITS_MAX) {
                pp->waits++;
                pp->due = s->now + KPS_WAIT_MS;
            }
        }
        if (fan && rep && fn != FN_WAIT) {   /* the stock may wait for the same key on its binding's port */
            s->st.to_stock++;
            return KPS_PASS;
        }
        return KPS_DONE;
    }
    if (fn == FN_WAIT) {   /* never to the stock: FUN_4014BCAC would spin for ever */
        s->st.wait_drop++;
        return KPS_DONE;
    }
    if (rep) {
        s->st.to_stock++;
        return KPS_PASS;
    }
    s->st.ack_drop++;
    return KPS_DONE;
}

static kps_verdict_t on_request(kps_t *s, uint8_t p, const uint8_t *m, uint32_t n, bool rep)
{
    kps_port_t *pp = &s->port[p];
    if (n != 10)
        return KPS_PASS;   /* the stock ignores it as well */
    if (m[6] == 0x5F && m[7] == 0x00) {   /* packed dump FUN_401525D4: left to the stock */
        if (rep)
            return KPS_PASS;
        s->st.dropped++;
        return KPS_DONE;
    }
    if (!kps_bound(s, p) || pp->active) {   /* FUN_4014FD94: not bound / busy -> dropped */
        s->st.dropped++;
        return KPS_DONE;
    }
    if (!pp->busy)   /* else a fan-out frame is in flight: kept, the answer starts after its ACK */
        memset(pp, 0, sizeof *pp);
    pp->active = true;
    pp->end = false;
    pp->cancel = false;
    pp->i = 0;
    pp->j = 0;
    pp->cc = m[2];
    pp->v1 = m[6];
    pp->v2 = m[7];
    pp->part = m[8];
    s->st.requests++;
    next(s, p);
    return KPS_DONE;
}

static kps_verdict_t on_connect(kps_t *s, uint8_t p)
{
    uint8_t b = s->io.binding(s->io.ctx), mode = 0, sub = 0;
    uint8_t r[10] = {0xF0, 0x40, 0x7F, 0x7E, 0x04, 0x1E, 0x02, 0, 0, 0xF7};
    s->bound |= (uint8_t)(1u << p);
    if (b == 0 || b == (uint8_t)(p + 1)) {
        s->st.conn_pass++;
        return KPS_PASS;
    }
    /* 0x4014A75C..0x4014A7D4 without FUN_40151BA0 / FUN_4014B9E8 (no 7E 01 00 to the old one) */
    s->io.connect_info(s->io.ctx, p, &mode, &sub);
    s->io.take_rep(s->io.ctx, p);
    s->last_binding = s->io.binding(s->io.ctx);
    r[7] = mode;
    r[8] = sub;
    out(s, p, r, sizeof r);
    s->io.connect_post(s->io.ctx);
    s->st.conn_own++;
    return KPS_DONE;
}

static kps_verdict_t on_disconnect(kps_t *s, uint8_t p, bool rep)
{
    if (rep) {   /* the stock unbinds and invites the panel (0x4014A80C..0x4014A82C) */
        drop(s, p);
        s->last_binding = 0;
        return KPS_PASS;
    }
    if (!kps_bound(s, p))
        return KPS_PASS;
    drop(s, p);
    s->st.disc_own++;
    return KPS_DONE;
}

static bool vt_edit(const uint8_t *m)
{
    return m[6] == 0x5A || (m[6] == 0x50 && (m[7] == 0x0C || m[7] == 0x0E || m[7] == 0x10 || m[7] == 0x17));
}

/* ---- file lease (kpsess.h) -------------------------------------------------- */

/* the requests that start a job (FUN_4014A2C8 fn 0x24: 04..0B -> FUN_4014F8CC / FUN_4014F870, 11 -> FUN_4014F018, 12 ->
 * FUN_4014F7EC); everything else (01 / 02 / 03 data, 7B, 1F, 7C..7F) belongs to a job already running */
static bool file_request(uint8_t d0)
{
    return (d0 >= 0x04 && d0 <= 0x0B) || d0 == 0x11 || d0 == 0x12;
}

/* the stock answers a second job with its request ACK and Busy (0x4014A9B0 / A98C / A96C: FUN_4014B8A0(0x7F) first, then
 * FUN_4014BEF0(0) = FUN_4014AEE0(0x7F, 3, 1, {0, 0})) - the same bytes, to the asking port only */
static void file_busy(kps_t *s, uint8_t p)
{
    static const uint8_t ack[] = {0xF0, 0x40, 0x7F, 0x24, 0x04, 0x1E, 0x7F, 0xF7};
    static const uint8_t busy[] = {0xF0, 0x40, 0x7F, 0x7E, 0x04, 0x1E, 0x03, 0x01, 0x00, 0x00, 0xF7};
    out(s, p, ack, sizeof ack);
    out(s, p, busy, sizeof busy);
    if (s->st.file_busy != 0xFFFFu)
        s->st.file_busy++;
}

static void inc16(uint16_t *c)
{
    if (*c != 0xFFFFu)
        (*c)++;
}

/* is the stock's file machinery in use?  `quiet` = ms since the last file frame either way */
static bool file_in_use(const kps_t *s, uint32_t quiet)
{
    uint8_t fs = s->io.file_state(s->io.ctx);
    return (fs & KPS_FS_HARD) || ((fs & KPS_FS_SOFT) && quiet < KPS_FILE_SOFT_MS);
}

/* end the lease; give_back: the owner still holds the stock binding (the job is over) - the holder before gets it back */
static void lease_end(kps_t *s, bool give_back)
{
    uint8_t b = s->io.binding(s->io.ctx), rep = s->lease_rep;
    bool holder = rep >= 1 && rep <= KPS_PORTS && kps_bound(s, (uint8_t)(rep - 1));
    if (give_back || b == 0) {
        /* b == 0: the stock dropped the owner mid-job (16 failures FUN_4014B638, its DISCONNECT) - the holder had done
         * nothing to lose the binding, it gets it back as well (a stock invitation to the panel is harmless: same-port
         * CONNECT is the stock's) */
        if (holder)
            s->io.file_bind(s->io.ctx, (uint8_t)(rep - 1));
        if (give_back)
            inc16(&s->st.file_back);
        else
            inc16(&s->st.file_lost);
    } else {
        inc16(&s->st.file_lost);   /* somebody CONNECTed: the last CONNECT keeps the binding, as always */
    }
    s->last_binding = s->io.binding(s->io.ctx);
    s->lease = 0;
    s->lease_rep = 0;
}

static kps_verdict_t on_file(kps_t *s, uint8_t p, const uint8_t *m, uint32_t n, bool rep)
{
    uint32_t quiet = s->now - s->file_last;
    uint8_t d0 = m[6];
    (void)n;
    s->file_last = s->now;
    if (d0 == 0x7C) {   /* file Wait: FUN_4014B4E0 would spin for ever on it (it never re-reads the flag) */
        s->st.wait_drop++;
        return KPS_DONE;
    }
    if (!s->io.file_bind || !s->io.file_state)
        return KPS_PASS;
    if (s->lease) {
        if (s->lease == (uint8_t)(p + 1))
            return KPS_PASS;   /* the owner: the stock binding now - the stock runs it */
        if (file_request(d0) && kps_bound(s, p))
            file_busy(s, p);
        return KPS_DONE;       /* not to the stock: it would take it as the owner's (or drop it) */
    }
    if (rep || !kps_bound(s, p) || !file_request(d0))
        return KPS_PASS;       /* the binding's own job / an unbound port / no job of its own: the stock as ever */
    /* the holder's job (or a stuck one) - the stock would say Busy to its binding.  Quiet too: the executor clears the job
     * byte just before it sends the Result (FUN_4014F364 -> FUN_4014F294), so a just-finished job looks idle a moment early */
    if (quiet < KPS_FILE_QUIET_MS || file_in_use(s, quiet)) {
        file_busy(s, p);
        return KPS_DONE;
    }
    s->lease_rep = s->io.binding(s->io.ctx);
    s->io.file_bind(s->io.ctx, p);
    s->last_binding = s->io.binding(s->io.ctx);
    s->lease = (uint8_t)(p + 1);
    inc16(&s->st.file_leases);
    return KPS_PASS;   /* the stock handler now passes its gate: ACK, the job, every reply to p */
}

/* kps_tick: has the leased job ended? */
static void lease_tick(kps_t *s)
{
    uint8_t b = s->io.binding(s->io.ctx);
    uint32_t quiet = s->now - s->file_last;
    if (b != s->lease) {
        lease_end(s, false);
        return;
    }
    if (quiet < KPS_FILE_QUIET_MS || file_in_use(s, quiet))
        return;
    lease_end(s, true);
}

bool kps_file_lease(const kps_t *s, uint8_t *owner, uint8_t *holder)
{
    if (owner)
        *owner = s->lease;
    if (holder)
        *holder = s->lease_rep;
    return s->lease != 0;
}

kps_verdict_t kps_rx(kps_t *s, uint8_t port, const uint8_t *m, uint32_t n, uint32_t now_ms)
{
    uint8_t fn;
    uint32_t dev;
    bool rep;
    if (!s->enabled || port >= KPS_PORTS)
        return KPS_PASS;
    s->now = now_ms;
    sync(s);
    if (n < 7 || m[0] != 0xF0 || m[1] != 0x40)
        return KPS_PASS;
    fn = m[3];
    if (fn == 0x70 || fn == 0x60 || m[4] != 0x08 || m[5] != 0x02 || vt_edit(m))
        return KPS_PASS;   /* judged by the stock before the function code (0x4014A2DC..0x4014A3B4) */
    dev = s->io.device(s->io.ctx);
    if (dev != m[2] && m[2] != 0x7F)
        return KPS_PASS;   /* the stock ignores it */
    rep = s->io.binding(s->io.ctx) == (uint8_t)(port + 1);
    switch (fn) {
    case FN_SYS:
        if (n == 9 && m[6] == 0x01 && m[8] == 0xF7) {
            if (m[7] == 0x01) {
                if (s->lease == (uint8_t)(port + 1)) {   /* the owner CONNECTs: it keeps the binding (last CONNECT) */
                    s->lease = 0;
                    s->lease_rep = 0;
                    inc16(&s->st.file_lost);
                }
                return on_connect(s, port);
            }
            if (m[7] == 0x00)
                return on_disconnect(s, port, rep);
        }
        return KPS_PASS;
    case FN_REQ:
        return on_request(s, port, m, n, rep);
    case FN_ACK:
    case FN_NAK:
    case FN_WAIT:
        return on_ack(s, port, m, n, rep);
    case FN_CANCEL:
        if (s->port[port].active) {
            s->port[port].cancel = true;
            s->st.cancels++;
        }
        if (rep) {
            s->st.to_stock++;
            return KPS_PASS;
        }
        return KPS_DONE;
    case FN_FILE:
        return on_file(s, port, m, n, rep);
    default:
        return KPS_PASS;
    }
}

void kps_tick(kps_t *s, uint32_t now_ms)
{
    if (!s->enabled)
        return;
    s->now = now_ms;
    sync(s);
    if (s->lease)
        lease_tick(s);
    for (uint8_t p = 0; p < KPS_PORTS; p++) {
        kps_port_t *pp = &s->port[p];
        bool up = !s->io.link_up || s->io.link_up(s->io.ctx, p);
        bool was = (s->link_prev & (1u << p)) != 0;
        if (up)
            s->link_prev |= (uint8_t)(1u << p);
        else
            s->link_prev &= (uint8_t)~(1u << p);
        /* link down of a port only the core binds (the stock's FUN_4015B494 handles its own binding) */
        if (was && !up && kps_bound(s, p) && s->io.binding(s->io.ctx) != (uint8_t)(p + 1)) {
            unbind(s, p);
            continue;
        }
        if (!pp->busy) {
            if (s->fan[p].cnt)
                next(s, p);
            continue;
        }
        if (!due(now_ms, pp->due))
            continue;
        if (!pp->nakd)
            s->st.timeouts++;
        if (pp->tries >= KPS_TRIES) {
            fail(s, p);
            continue;
        }
        s->st.resends++;
        send(s, p);
    }
}

/* ---- stage 2: change fan-out ------------------------------------------------------------------- */

/* keys whose first data byte is a sub-index (one dump carries several frames of the key: kpstate.c) */
static bool indexed(const uint8_t *f)
{
    return (f[6] == 0x54 && f[7] == 0x05 && f[8] == 0x0F) || (f[6] == 0x51 && f[7] == 0x1A && f[8] == 0x7F) ||
           (f[6] == 0x51 && f[7] == 0x1B && f[8] == 0x7F);
}

/* two notification frames that carry the same value slot (the newer replaces the older) */
static bool same_slot(const uint8_t *a, uint8_t na, const uint8_t *b, uint8_t nb)
{
    if (na < 10 || nb < 10 || a[6] != b[6] || a[7] != b[7])
        return false;
    if (a[6] == 0x70)   /* 2-byte key (FUN_4014BB54) */
        return true;
    if (a[8] != b[8])
        return false;
    return !indexed(a) || (na > 10 && nb > 10 && a[9] == b[9]);
}

static void fan_put(kps_t *s, uint8_t p, const uint8_t *f, uint16_t n)
{
    kps_fanq_t *q = &s->fan[p];
    s->st.fan_queued++;
    for (uint8_t i = 0; i < q->cnt; i++)
        if (same_slot(q->q[i].frame, q->q[i].n, f, (uint8_t)n)) {
            memcpy(q->q[i].frame, f, n);
            q->q[i].n = (uint8_t)n;
            s->st.fan_merged++;
            return;
        }
    if (q->cnt == KPS_FANQ) {   /* the oldest waiting frame makes room */
        pop_front(q);
        s->st.fan_drop++;
    }
    memcpy(q->q[q->cnt].frame, f, n);
    q->q[q->cnt].n = (uint8_t)n;
    q->cnt++;
}

static uint8_t fanout(kps_t *s, uint8_t exclude, const uint8_t *f, uint16_t n)
{
    uint8_t k = 0;
    if (!s->enabled || n < 10 || n > KPS_FRAME_MAX)
        return 0;
    for (uint8_t p = 0; p < KPS_PORTS; p++) {
        if (!kps_bound(s, p) || (exclude & (1u << p)))
            continue;
        fan_put(s, p, f, n);
        next(s, p);
        k++;
    }
    return k;
}

void kps_fanout(kps_t *s, uint8_t exclude, const uint8_t *f, uint16_t n)
{
    (void)fanout(s, exclude, f, n);
}

uint8_t kps_relay(kps_t *s, uint8_t exclude, const uint8_t *f, uint16_t n)
{
    uint8_t k = 0;
    if (!s->enabled)
        return 0;
    for (uint8_t p = 0; p < KPS_PORTS; p++)
        if (kps_bound(s, p) && !(exclude & (1u << p))) {
            s->io.tx(s->io.ctx, p, f, n);
            k++;
        }
    return k;
}

uint8_t kps_fan_pending(const kps_t *s, uint8_t port)
{
    return port < KPS_PORTS ? s->fan[port].cnt : 0;
}

static uint32_t hash(const uint8_t *b, int n)
{
    uint32_t h = 2166136261u;   /* FNV-1a */
    for (int i = 0; i < n; i++)
        h = (h ^ b[i]) * 16777619u;
    return h;
}

static void add_key(kps_t *s, uint8_t v1, uint8_t v2, uint8_t part)
{
    for (uint8_t i = 0; i < s->nkeys; i++)
        if (s->keys[i][0] == v1 && s->keys[i][1] == v2 && s->keys[i][2] == part)
            return;
    if (s->nkeys == KPS_SET_KEYS) {
        s->st.snap_over++;
        return;
    }
    s->keys[s->nkeys][0] = v1;
    s->keys[s->nkeys][1] = v2;
    s->keys[s->nkeys][2] = part;
    s->nkeys++;
}

void kps_set_begin(kps_t *s, uint8_t from, const uint8_t *m, uint32_t n)
{
    uint8_t f[KPS_FRAME_MAX], others;
    s->set_on = false;
    s->nkeys = 0;
    s->nsnap = 0;
    if (!s->enabled || !s->io.key_value || (from >= KPS_PORTS && from != KPS_FROM_CORE) || n < 10)
        return;
    others = from == KPS_FROM_CORE ? s->bound : (uint8_t)(s->bound & ~(1u << from));
    if (others == 0)
        return;   /* nobody else to tell */
    if (m[6] == 0x5F && m[7] == 0x00) {   /* packed 10 5F 00 part <len v1 v2 part data..>.. (kpfront.c) */
        uint32_t end = n - 1, i = 9;
        while (i < end) {
            uint8_t l = m[i];
            if (l < 4 || i + l > end)
                break;
            add_key(s, m[i + 1], m[i + 2], m[i + 3]);
            i += l;
        }
    } else {
        add_key(s, m[6], m[7], m[8]);
    }
    for (uint8_t i = 0; i < s->nkeys; i++)
        for (uint8_t k = 0; k < KPS_KEY_FRAMES; k++) {
            int r = s->io.key_value(s->io.ctx, s->keys[i][0], s->keys[i][1], s->keys[i][2], k, f);
            if (r <= 0)
                break;
            if (s->nsnap == KPS_SNAP) {
                s->st.snap_over++;
                break;
            }
            memcpy(s->snap[s->nsnap].key, s->keys[i], 3);
            s->snap[s->nsnap].k = k;
            s->snap[s->nsnap].h = hash(f, r);
            s->nsnap++;
        }
    s->set_from = from;
    s->set_on = true;
}

void kps_set_end(kps_t *s)
{
    uint8_t f[KPS_FRAME_MAX];
    bool changed = false;
    if (!s->set_on)
        return;
    s->set_on = false;
    for (uint8_t i = 0; i < s->nkeys; i++)
        for (uint8_t k = 0; k < KPS_KEY_FRAMES; k++) {
            int r = s->io.key_value(s->io.ctx, s->keys[i][0], s->keys[i][1], s->keys[i][2], k, f);
            const kps_snap_t *was = 0;
            if (r <= 0)
                break;
            for (uint8_t j = 0; j < s->nsnap; j++)
                if (memcmp(s->snap[j].key, s->keys[i], 3) == 0 && s->snap[j].k == k) {
                    was = &s->snap[j];
                    break;
                }
            if (was && was->h == hash(f, r))
                continue;
            fanout(s, s->set_from == KPS_FROM_CORE ? 0u : (uint8_t)(1u << s->set_from), f, (uint16_t)r);
            changed = true;
        }
    if (changed)
        s->st.fan_sets++;
    else
        s->st.fan_same++;
}

void kps_tap_reset(kps_t *s)
{
    s->tap_in = false;
    s->tap_over = false;
    s->tap_n = 0;
}

/* one complete frame the stock wrote to its binding's port */
static void tap_frame(kps_t *s, uint8_t port)
{
    const uint8_t *f = s->tap_buf;
    uint8_t n = s->tap_n, ex = (uint8_t)(1u << port);
    if (port == KPS_HOME && s->io.forward && s->io.forward(s->io.ctx))
        ex |= 0x07;   /* FUN_4014AD04 already copied it to ports 0..2 */
    if (n >= 10 && f[1] == 0x40 && f[3] == 0x32 && f[4] == 0x04 && f[5] == 0x1E) {
        /* fn 0x32 (song progress 32 52 03, ...): the stock sends these without waiting for an ACK and the
         * controllers do not ACK them - relayed the same way, at once, never through the reliable queue.
         * BeatCounter 32 56 0B is not: the metronome module (kpmetro) makes those frames
         * for the other controllers itself - relaying it here too would send every beat twice. */
        if (f[6] == 0x56 && f[7] == 0x0B)
            return;
        /* 32 51 7E 7F hi lo: a file job's Result (FUN_4014F294 -> FUN_4014AD04) - the answer to the binding's own job,
         * not a notice (file lease: the owner's job result must not reach the holder) */
        if (f[6] == 0x51 && f[7] == 0x7E)
            return;
        s->st.tap_relayed += kps_relay(s, ex, f, n);
        return;
    }
    if (n < 10 || f[1] != 0x40 || f[3] != 0x10 || f[4] != 0x04 || f[5] != 0x1E)
        return;   /* not a parameter notification (ACK, CONNECT reply, 7E 03 result, 0x7D, ...) */
    if ((f[6] == 0x5F && f[7] == 0x00) || vt_edit(f))
        return;   /* the packed dump answer and VT edit: answers to the binding's own request */
    s->st.tap_frames++;
    if (s->io.tapped)
        s->io.tapped(s->io.ctx, f, n);
    if (fanout(s, ex, f, n))
        s->st.tap_fanned++;
}

void kps_tap(kps_t *s, uint8_t port, const uint8_t *b, uint32_t n)
{
    if (port >= KPS_PORTS)
        return;
    if (s->lease == (uint8_t)(port + 1) && n)
        s->file_last = s->now;   /* the stock sends the owner's job: not over yet */
    for (uint32_t i = 0; i < n; i++) {
        uint8_t c = b[i];
        if (c == 0xF0) {
            s->tap_in = true;
            s->tap_over = false;
            s->tap_buf[0] = c;
            s->tap_n = 1;
            continue;
        }
        if (!s->tap_in || c >= 0xF8)
            continue;   /* outside a SysEx; real-time bytes may sit anywhere */
        if (c >= 0x80 && c != 0xF7) {
            s->tap_in = false;   /* a status byte ends the SysEx */
            continue;
        }
        if (s->tap_n < KPS_FRAME_MAX)
            s->tap_buf[s->tap_n++] = c;
        else
            s->tap_over = true;
        if (c == 0xF7) {
            s->tap_in = false;
            if (s->tap_over) {
                if (s->tap_buf[3] == 0x10)
                    s->st.tap_long++;
            } else if (s->enabled) {
                tap_frame(s, port);
            }
        }
    }
}
