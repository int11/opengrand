/* OpenGrand - Phase 3-1 SysEx front.  See kpfront.h. */
#include "ca79ng/kpfront.h"

#include <string.h>

#include "ca79ng/util.h"

#define FN_SET 0x10

void kpf_init(kpf_t *f, const kpf_io_t *io, param_store_t *mirror)
{
    memset(f, 0, sizeof *f);
    f->io = *io;
    f->mirror = mirror;
    f->enabled = true;
}

/* d0 0x5A and d0 0x50 + d1 0C/0E/10/17: the stock judges these before the function code
 * (0x4014A370..0x4014A3B4) - VT edit, left to it */
static bool vt_edit(const uint8_t *m)
{
    return m[6] == 0x5A || (m[6] == 0x50 && (m[7] == 0x0C || m[7] == 0x0E || m[7] == 0x10 || m[7] == 0x17));
}

kpf_verdict_t kpf_judge(const kpf_t *f, uint8_t stock_port, const uint8_t *m, uint32_t n)
{
    uint32_t dev;
    if (!f->enabled || stock_port > 3 || n < 10 || m[0] != 0xF0 || m[1] != 0x40 || m[3] != FN_SET)
        return KPF_PASS;
    if (m[4] != 0x08 || m[5] != 0x02 || vt_edit(m))
        return KPF_PASS;
    dev = f->io.device(f->io.ctx);   /* 0x4014A304: FUN_40147D60() == cc, or cc == 0x7F */
    if (dev != m[2] && m[2] != 0x7F)
        return KPF_SILENT;
    if (f->io.binding(f->io.ctx) != (uint8_t)(stock_port + 1))   /* FUN_40151BCC */
        return KPF_SILENT;
    return KPF_OWN;
}

uint16_t kpf_ack_frame(const uint8_t *m, uint8_t out[KPF_ACK_MAX])
{
    /* FUN_4014ACD8 header, FUN_4014BB54 key length (2 for v1 0x70, else 3), F7 */
    uint16_t k = m[6] == 0x70 ? 2 : 3, o = 6;
    out[0] = 0xF0;
    out[1] = 0x40;
    out[2] = 0x7F;
    out[3] = 0x07;
    out[4] = 0x04;
    out[5] = 0x1E;
    for (uint16_t i = 0; i < k; i++)
        out[o++] = m[6 + i];
    out[o++] = 0xF7;
    return o;
}

/* FUN_4014AD04: to the bound port; with the panel bound and port 3 forwarding on, to 0, 1, 2 first */
static void send_ack(kpf_t *f, uint8_t binding, const uint8_t *m)
{
    uint8_t a[KPF_ACK_MAX];
    uint16_t n = kpf_ack_frame(m, a);
    if (binding == 4 && f->io.forward(f->io.ctx)) {
        for (uint8_t p = 0; p < 3; p++) {
            f->io.tx(f->io.ctx, p, a, n);
            f->st.acks++;
        }
    }
    f->io.tx(f->io.ctx, (uint8_t)(binding - 1), a, n);
    f->st.acks++;
}

static void mirror_one(kpf_t *f, bool staged, uint8_t v1, uint8_t v2, uint8_t part, const uint8_t *d, uint32_t dlen)
{
    int id = param_find(FN_SET, v1, v2, part);
    int slot = param_slot_from_part(part);
    uint8_t len = (uint8_t)CA_MIN(dlen, 255u);
    if (id < 0 || slot < 0 || !param_validate((param_id_t)id, d, len)) {
        f->st.unknown++;
        return;
    }
    param_put(f->mirror, (param_id_t)id, slot, d, len);
    f->st.mirrored++;
    if (staged)
        f->st.staged++;
}

/* same parse as kproto handle_param: single, or packed 10 5F 00 part <len v1 v2 part data..>..
 * (len counts itself, body <= 0xF5) */
static void mirror(kpf_t *f, const uint8_t *m, uint32_t n)
{
    bool staged;
    if (!f->mirror)
        return;
    staged = f->io.staging(f->io.ctx);
    if (m[6] == 0x5F && m[7] == 0x00) {
        uint32_t end = n - 1, i = 9;
        f->st.packed++;
        if (end - i > KP_PACKED_MAX)
            return;
        while (i < end) {
            uint8_t l = m[i];
            if (l < 4 || i + l > end)
                break;
            mirror_one(f, staged, m[i + 1], m[i + 2], m[i + 3], m + i + 4, (uint32_t)l - 4);
            i += l;
        }
        return;
    }
    mirror_one(f, staged, m[6], m[7], m[8], m + 9, n - 10);
}

bool kpf_handle(kpf_t *f, uint8_t stock_port, const uint8_t *m, uint32_t n)
{
    kpf_verdict_t v = kpf_judge(f, stock_port, m, n);
    f->st.seen++;
    if (v == KPF_PASS) {
        f->st.passed++;
        return false;
    }
    if (v == KPF_SILENT) {
        f->st.silent++;
        return true;
    }
    f->st.owned++;
    memcpy(f->last_key, m + 6, 3);
    send_ack(f, (uint8_t)(stock_port + 1), m);   /* before the lookup: unknown keys are ACKed too */
    mirror(f, m, n);
    f->io.apply(f->io.ctx, (uint8_t)(stock_port + 1), m + 6, (uint16_t)n);
    return true;
}
