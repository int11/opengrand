/* Phase 3-1 SysEx front: the core module (src/kproto/kpfront.c) against the stock behaviour read off
 * FUN_4014A2C8 / FUN_4014BE98 / FUN_4014AD04, and its cave port (ports/cave/sysex_cave.c) with
 * recording stand-ins for the stock routine and globals. */
#include <string.h>

#include "ca79ng/kpfront.h"
#include "stock_host.h"
#include "sysex_cave.h"
#include "test.h"

/* ---- stand-ins (sysex_stock.h); stk_binding is test_power_cave.c's ---------------------------- */
extern volatile uint8_t stk_binding;
volatile uint32_t stk_dev_id;
volatile uint8_t stk_stage_flag;
volatile uint32_t stk_port3_fwd;
volatile uint8_t stk_sess_base[0x42];
volatile uint16_t stk_tick2ms;
static volatile unsigned char fake_inflight;

static struct {
    int calls;
    uint32_t ctx, fn, len;
    uint8_t body[64];
    int inflight_seen;   /* the resident's flag while the stock apply path runs */
} ps;

void stk_param_set(uint32_t ctx, uint32_t fn, const uint8_t *body, uint32_t len)
{
    ps.calls++;
    ps.ctx = ctx;
    ps.fn = fn;
    ps.len = len;
    memcpy(ps.body, body, len - 6 < sizeof ps.body ? len - 6 : sizeof ps.body);
    ps.inflight_seen = fake_inflight;
}

/* ---- the core module with a fake io ------------------------------------------------------------ */
static struct {
    uint8_t binding;
    uint32_t device;
    bool fwd, staging;
    int ntx;
    uint8_t txp[8];
    uint8_t tx[8][16];
    uint16_t txn[8];
    int napply;
    uint8_t actx;
    const uint8_t *abody;
    uint16_t an;
} fk;

static uint8_t f_binding(void *c) { (void)c; return fk.binding; }
static uint32_t f_device(void *c) { (void)c; return fk.device; }
static bool f_forward(void *c) { (void)c; return fk.fwd; }
static bool f_staging(void *c) { (void)c; return fk.staging; }
static void f_tx(void *c, uint8_t p, const uint8_t *m, uint16_t n)
{
    (void)c;
    if (fk.ntx < 8) {
        fk.txp[fk.ntx] = p;
        fk.txn[fk.ntx] = n;
        memcpy(fk.tx[fk.ntx], m, n < 16 ? n : 16);
    }
    fk.ntx++;
}
static void f_apply(void *c, uint8_t ctx, const uint8_t *b, uint16_t n)
{
    (void)c;
    fk.napply++;
    fk.actx = ctx;
    fk.abody = b;
    fk.an = n;
}

static const kpf_io_t FIO = {0, f_binding, f_device, f_forward, f_staging, f_tx, f_apply};
static param_store_t store;
static kpf_t fr;

static void fresh(uint8_t binding)
{
    memset(&fk, 0, sizeof fk);
    fk.binding = binding;
    param_store_defaults(&store);
    kpf_init(&fr, &FIO, &store);
}

static const uint8_t SET_APO[] = {0xF0, 0x40, 0x00, 0x10, 0x08, 0x02, 0x51, 0x00, 0x7F, 0x02, 0xF7};
static const uint8_t ACK_APO[] = {0xF0, 0x40, 0x7F, 0x07, 0x04, 0x1E, 0x51, 0x00, 0x7F, 0xF7};

TEST(kpf_judge_cut_line)
{
    fresh(3);   /* bound on stock port 2 (BLE) */
    CHECK_EQ(kpf_judge(&fr, 2, SET_APO, sizeof SET_APO), KPF_OWN);
    {   /* device byte: == receive number, or 0x7F; otherwise silent like the stock */
        uint8_t m[sizeof SET_APO];
        memcpy(m, SET_APO, sizeof m);
        m[2] = 0x7F;
        CHECK_EQ(kpf_judge(&fr, 2, m, sizeof m), KPF_OWN);
        m[2] = 0x05;
        CHECK_EQ(kpf_judge(&fr, 2, m, sizeof m), KPF_SILENT);
        fk.device = 5;
        CHECK_EQ(kpf_judge(&fr, 2, m, sizeof m), KPF_OWN);
        fk.device = 0;
        m[2] = 0x00;
        /* not the bound port: silent; nothing bound: silent */
        CHECK_EQ(kpf_judge(&fr, 1, m, sizeof m), KPF_SILENT);
        fk.binding = 0;
        CHECK_EQ(kpf_judge(&fr, 2, m, sizeof m), KPF_SILENT);
        fk.binding = 3;
        /* the stock's: other functions, other headers (71 / not 08 02), VT edit prefixes, short frames */
        m[3] = 0x31;
        CHECK_EQ(kpf_judge(&fr, 2, m, sizeof m), KPF_PASS);
        m[3] = 0x10;
        m[4] = 0x71;
        CHECK_EQ(kpf_judge(&fr, 2, m, sizeof m), KPF_PASS);
        m[4] = 0x08;
        m[5] = 0x03;
        CHECK_EQ(kpf_judge(&fr, 2, m, sizeof m), KPF_PASS);
        m[5] = 0x02;
        m[6] = 0x5A;
        CHECK_EQ(kpf_judge(&fr, 2, m, sizeof m), KPF_PASS);
        m[6] = 0x50;
        m[7] = 0x0C;
        CHECK_EQ(kpf_judge(&fr, 2, m, sizeof m), KPF_PASS);
        m[7] = 0x17;
        CHECK_EQ(kpf_judge(&fr, 2, m, sizeof m), KPF_PASS);
        m[7] = 0x0D;   /* 50 0D: an ordinary set */
        CHECK_EQ(kpf_judge(&fr, 2, m, sizeof m), KPF_OWN);
        CHECK_EQ(kpf_judge(&fr, 2, m, 9), KPF_PASS);
        CHECK_EQ(kpf_judge(&fr, 4, m, sizeof m), KPF_PASS);
        fr.enabled = false;
        CHECK_EQ(kpf_judge(&fr, 2, SET_APO, sizeof SET_APO), KPF_PASS);
    }
}

TEST(kpf_ack_bytes_and_ports)
{
    uint8_t a[KPF_ACK_MAX];
    const uint8_t m70[] = {0xF0, 0x40, 0x00, 0x10, 0x08, 0x02, 0x70, 0x01, 0x7F, 0x00, 0xF7};
    const uint8_t ack70[] = {0xF0, 0x40, 0x7F, 0x07, 0x04, 0x1E, 0x70, 0x01, 0xF7};
    CHECK_EQ(kpf_ack_frame(SET_APO, a), 10);
    CHECK_MEM(a, ACK_APO, 10);
    CHECK_EQ(kpf_ack_frame(m70, a), 9);   /* FUN_4014BB54: v1 0x70 echoes two bytes */
    CHECK_MEM(a, ack70, 9);

    fresh(3);
    CHECK(kpf_handle(&fr, 2, SET_APO, sizeof SET_APO));
    CHECK_EQ(fk.ntx, 1);
    CHECK_EQ(fk.txp[0], 2);
    CHECK_EQ(fk.txn[0], 10);
    CHECK_MEM(fk.tx[0], ACK_APO, 10);
    /* panel bound, port 3 forwarding on: 0, 1, 2 first, then 3 (FUN_4014AD04) */
    fresh(4);
    fk.fwd = true;
    CHECK(kpf_handle(&fr, 3, SET_APO, sizeof SET_APO));
    CHECK_EQ(fk.ntx, 4);
    CHECK(fk.txp[0] == 0 && fk.txp[1] == 1 && fk.txp[2] == 2 && fk.txp[3] == 3);
    CHECK_MEM(fk.tx[3], ACK_APO, 10);
    CHECK_EQ(fr.st.acks, 4);
    fresh(4);
    CHECK(kpf_handle(&fr, 3, SET_APO, sizeof SET_APO));
    CHECK(fk.ntx == 1 && fk.txp[0] == 3);
}

TEST(kpf_apply_delegated_and_mirror)
{
    fresh(3);
    CHECK(kpf_handle(&fr, 2, SET_APO, sizeof SET_APO));
    /* the stock FUN_4014EBFC(port + 1, 0x10, msg + 6, n) */
    CHECK_EQ(fk.napply, 1);
    CHECK_EQ(fk.actx, 3);
    CHECK(fk.abody == SET_APO + 6);
    CHECK_EQ(fk.an, sizeof SET_APO);
    CHECK_EQ(param_get1(&store, PRM_AUTO_POWER_OFF, SLOT_SYSTEM, 0xFF), 2);
    CHECK(fr.st.owned == 1 && fr.st.mirrored == 1 && fr.st.unknown == 0 && fr.st.staged == 0);
    CHECK(fr.last_key[0] == 0x51 && fr.last_key[1] == 0x00 && fr.last_key[2] == 0x7F);

    /* unknown key: ACKed and handed to the stock all the same, not mirrored */
    {
        const uint8_t u[] = {0xF0, 0x40, 0x00, 0x10, 0x08, 0x02, 0x51, 0x7E, 0x7F, 0x00, 0xF7};
        CHECK(kpf_handle(&fr, 2, u, sizeof u));
        CHECK_EQ(fk.ntx, 2);
        CHECK_EQ(fk.napply, 2);
        CHECK_EQ(fr.st.unknown, 1);
    }
    /* staging (fn 0x09 .. 0x03): mirrored and counted, the stock gate decides */
    fk.staging = true;
    {
        uint8_t m[sizeof SET_APO];
        memcpy(m, SET_APO, sizeof m);
        m[9] = 1;
        CHECK(kpf_handle(&fr, 2, m, sizeof m));
        CHECK_EQ(fr.st.staged, 1);
        CHECK_EQ(param_get1(&store, PRM_AUTO_POWER_OFF, SLOT_SYSTEM, 0xFF), 1);
    }
    fk.staging = false;
    /* packed 10 5F 00 7F <05 51 00 7F 03> <05 51 00 7F 00>: one ACK, one stock call, both values */
    {
        const uint8_t p[] = {0xF0, 0x40, 0x00, 0x10, 0x08, 0x02, 0x5F, 0x00, 0x7F,
                             0x05, 0x51, 0x00, 0x7F, 0x03, 0x05, 0x51, 0x00, 0x7F, 0x00, 0xF7};
        const uint8_t pack_ack[] = {0xF0, 0x40, 0x7F, 0x07, 0x04, 0x1E, 0x5F, 0x00, 0x7F, 0xF7};
        int n0 = fk.ntx;
        CHECK(kpf_handle(&fr, 2, p, sizeof p));
        CHECK_EQ(fk.ntx, n0 + 1);
        CHECK_MEM(fk.tx[n0], pack_ack, sizeof pack_ack);
        CHECK_EQ(fk.napply, 4);
        CHECK_EQ(fr.st.packed, 1);
        CHECK_EQ(fr.st.mirrored, 4);
        CHECK_EQ(param_get1(&store, PRM_AUTO_POWER_OFF, SLOT_SYSTEM, 0xFF), 0);
    }
    /* silent: no ACK, no stock call, but owned (the stock handler must not answer either) */
    {
        int n0 = fk.ntx, a0 = fk.napply;
        CHECK(kpf_handle(&fr, 1, SET_APO, sizeof SET_APO));
        CHECK(fk.ntx == n0 && fk.napply == a0 && fr.st.silent == 1);
    }
    /* pass */
    {
        const uint8_t c[] = {0xF0, 0x40, 0x00, 0x7E, 0x08, 0x02, 0x01, 0x01, 0xF7};
        CHECK(!kpf_handle(&fr, 2, c, sizeof c));
        CHECK_EQ(fr.st.passed, 1);
        CHECK_EQ(fr.st.seen, 6);
    }
}

/* ---- the cave port ------------------------------------------------------------------------------ */
static void cave_fresh(void)
{
    stock_host_reset();
    memset(&ps, 0, sizeof ps);
    stk_binding = 3;
    stk_dev_id = 0;
    stk_stage_flag = 0;
    stk_port3_fwd = 0;
    fake_inflight = 0;
    sxc_restart();
}

static int unpack7(const uint8_t *in, int n, uint8_t *out)
{
    int o = 0;
    for (int i = 0; i < n;) {
        uint8_t hi = in[i++];
        for (int k = 0; k < 7 && i < n; k++, i++)
            out[o++] = (uint8_t)(in[i] | (((hi >> k) & 1u) << 7));
    }
    return o;
}

static bool front_reply(uint8_t port, const uint8_t *req, uint32_t n, uint8_t *sub, uint8_t *rc, uint8_t raw[SX_DIAG_RAW])
{
    int l0 = stock_host.log_n, len;
    sxc_sysex(port, req, n);
    if (stock_host.log_n != l0 + 1 || stock_host.log_port[l0] != port)
        return false;
    len = stock_host.log_len[l0];
    const uint8_t *m = stock_host.log[l0];
    if (len < 10 || m[0] != 0xF0 || m[1] != 0x7D || m[6] != 0x5B || m[len - 1] != 0xF7)
        return false;
    *sub = m[7];
    *rc = m[8];
    return unpack7(m + 9, len - 10, raw) == SX_DIAG_RAW;
}

TEST(sysex_cave_front)
{
    uint8_t raw[SX_DIAG_RAW], sub, rc;
    cave_fresh();
    sxc_attached(1, &fake_inflight);
    /* before the first engine-loop pass: everything goes to the stock handler */
    CHECK_EQ(sxc_kawai(2, SET_APO, sizeof SET_APO), 0);
    CHECK_EQ(stock_host.log_n, 0);
    sxc_tick();
    CHECK_EQ(sxc_kawai(2, SET_APO, sizeof SET_APO), 1);
    CHECK_EQ(stock_host.log_n, 1);
    CHECK_EQ(stock_host.log_port[0], 2);
    CHECK_MEM(stock_host.log[0], ACK_APO, sizeof ACK_APO);
    CHECK(ps.calls == 1 && ps.ctx == 3 && ps.fn == 0x10 && ps.len == sizeof SET_APO && ps.body[0] == 0x51);
    CHECK_EQ(ps.inflight_seen, 1);   /* held across the blocking stock call */
    CHECK_EQ(fake_inflight, 0);      /* and released before the stub returns */
    /* a non-0x10 frame: tail call, no flag left behind, nothing sent */
    {
        const uint8_t touch[] = {0xF0, 0x40, 0x00, 0x32, 0x08, 0x02, 0x51, 0x39, 0x7F, 0x00, 0xF7};
        CHECK_EQ(sxc_kawai(3, touch, sizeof touch), 0);
        CHECK(stock_host.log_n == 1 && ps.calls == 1 && fake_inflight == 0);
    }
    /* port 3 forwarding: the ACK copies go to 0, 1, 2 first */
    stk_binding = 4;
    stk_port3_fwd = 1;
    CHECK_EQ(sxc_kawai(3, SET_APO, sizeof SET_APO), 1);
    CHECK_EQ(stock_host.log_n, 5);
    CHECK(stock_host.log_port[1] == 0 && stock_host.log_port[2] == 1 && stock_host.log_port[3] == 2 &&
          stock_host.log_port[4] == 3);
    CHECK_EQ(ps.ctx, 4);
    stk_port3_fwd = 0;
    stk_binding = 3;

    /* op 0x1B read */
    {
        const uint8_t q[] = {0xF0, 0x7D, 0x43, 0x41, 0x37, 0x39, 0x1B, 0x00, 0xF7};
        CHECK(front_reply(1, q, sizeof q, &sub, &rc, raw));
        CHECK(sub == 0 && rc == SX_OK);
        CHECK_EQ(raw[0], 1);
        CHECK_EQ(raw[1], 0x0F);   /* enabled, hooked, ready, guards */
        CHECK(raw[2] == 0x51 && raw[3] == 0x00 && raw[4] == 0x7F);
        CHECK_EQ(raw[6], 3);      /* seen */
        CHECK_EQ(raw[10], 1);     /* passed */
        CHECK_EQ(raw[14], 2);     /* owned */
        CHECK_EQ(raw[22], 2);     /* mirrored */
        CHECK_EQ(raw[38], 5);     /* ACK frames */
    }
    /* op 0x1B 01 00: only from the bound port; then the front passes everything */
    {
        const uint8_t off[] = {0xF0, 0x7D, 0x43, 0x41, 0x37, 0x39, 0x1B, 0x01, 0x00, 0xF7};
        const uint8_t on[] = {0xF0, 0x7D, 0x43, 0x41, 0x37, 0x39, 0x1B, 0x01, 0x01, 0xF7};
        const uint8_t bad[] = {0xF0, 0x7D, 0x43, 0x41, 0x37, 0x39, 0x1B, 0x01, 0x02, 0xF7};
        CHECK(front_reply(1, off, sizeof off, &sub, &rc, raw));
        CHECK(sub == 1 && rc == SX_NOT_BOUND && (raw[1] & 1));
        CHECK(front_reply(2, bad, sizeof bad, &sub, &rc, raw));
        CHECK(rc == SX_BAD_VALUE && (raw[1] & 1));
        CHECK(front_reply(2, off, sizeof off, &sub, &rc, raw));
        CHECK(rc == SX_OK && !(raw[1] & 1));
        CHECK_EQ(sxc_kawai(2, SET_APO, sizeof SET_APO), 0);
        CHECK(front_reply(2, on, sizeof on, &sub, &rc, raw));
        CHECK(rc == SX_OK && (raw[1] & 1));
        CHECK_EQ(sxc_kawai(2, SET_APO, sizeof SET_APO), 1);
    }
    /* op 0x1B 02: stock session globals + per-port frame / CONNECT counts (second field failure) */
    {
        const uint8_t q2[] = {0xF0, 0x7D, 0x43, 0x41, 0x37, 0x39, 0x1B, 0x02, 0xF7};
        const uint8_t con[] = {0xF0, 0x40, 0x7F, 0x7E, 0x08, 0x02, 0x01, 0x01, 0xF7};
        const uint8_t dis[] = {0xF0, 0x40, 0x7F, 0x7E, 0x08, 0x02, 0x01, 0x00, 0xF7};
        stk_binding = 3;   /* the app holds it; the panel CONNECTs */
        stk_sess_base[0x2F] = 3;
        stk_sess_base[0x30] = 1;
        stk_sess_base[2] = 1;
        stk_sess_base[0x41] = 1;
        stk_tick2ms = 0xFF06;
        CHECK_EQ(sxc_kawai(3, con, sizeof con), 0);   /* counted, then the stock's */
        CHECK_EQ(sxc_kawai(3, dis, sizeof dis), 0);
        CHECK(front_reply(2, q2, sizeof q2, &sub, &rc, raw));
        CHECK(sub == 2 && rc == SX_OK && raw[0] == 2);
        CHECK(raw[1] == 3 && raw[2] == 3 && raw[3] == 1 && raw[6] == 1 && raw[11] == 1);
        CHECK(raw[12] == 0x06 && raw[13] == 0xFF);
        CHECK_EQ(raw[16 + 12], 4);              /* port 3: touch, forwarded set, CONNECT, DISCONNECT */
        CHECK(raw[16 + 8] >= 1);                /* port 2: the sets above */
        CHECK(raw[38] == 1 && raw[43] == 1);    /* port 3: one CONNECT, one DISCONNECT */
        CHECK(raw[44] == 4 && raw[45] == 3);    /* last CONNECT from stock port 3, the app was bound */
        memset((void *)stk_sess_base, 0, sizeof stk_sess_base);
        stk_tick2ms = 0;
    }
    /* no attach (ota_loader v1): the hook is never called; the op still answers, hooked bit clear */
    cave_fresh();
    sxc_tick();
    {
        const uint8_t q[] = {0xF0, 0x7D, 0x43, 0x41, 0x37, 0x39, 0x1B, 0xF7};
        CHECK(front_reply(0, q, sizeof q, &sub, &rc, raw));
        CHECK(rc == SX_OK && raw[1] == 0x0D);
    }
    /* without the resident's flag the body still works (and touches no flag) */
    CHECK_EQ(sxc_kawai(2, SET_APO, sizeof SET_APO), 1);
    CHECK_EQ(ps.inflight_seen, 0);
}

void suite_sysex_cave(void)
{
    printf("sysex front (Phase 3-1)\n");
    RUN(kpf_judge_cut_line);
    RUN(kpf_ack_bytes_and_ports);
    RUN(kpf_apply_delegated_and_mirror);
    RUN(sysex_cave_front);
}
