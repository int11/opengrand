/* OpenGrand cave port - RAM canary.  See canary_cave.h.
 *
 * The regions (hardware baseline study §2.2, every edge moved in by a 4 KB guard; R1 - our own RW / staging - and
 * every block the study marks as used are not in the table):
 *   0 T1-A  0x40600000-0x409FFFFF  4 MB    power-on garbage.  The only stock literals are the exclusive end of the
 *           RecorderData mirror, 0x40600000 (FUN_40165560 bounds song pointers below it; FUN_4016C84C pads the mirror
 *           with zeros up to it and writes 5A A5 at 0x405FFFFE / 0x405FFFFF)
 *   1 T1-B  0x41943000-0x41CFFFFF  3.7 MB  zeroed by the stock main at every boot (memset 0x41000000, 0xD00000); the
 *           declared data ends at 0x4194285A
 *   2 T1-C  0x403C0000-0x403FFFFF  256 KB  DSP image mirror slot, never copied
 *   3 T1-D  0x40001000-0x4005FFFF  380 KB  the BootProgram updater's area (update mode only); 0x40000000 (key code)
 *           and its page are kept out.  The 0x400xxxxx constants of FUN_400AA160 are code values it compares, not
 *           addresses
 *   4 T1-E  0x400B3AD8-0x400BFFFF  49 KB   FF tail of the BootProgram copy
 *   5 T2-a  0x40C00000-0x40DFFFFF  2 MB    declared by the dead linker scatter table only (the kernel heap starts at
 *           0x40E00000)
 *   6 T2-b  0x41D00000-0x41EF7FFF  2 MB    declared by the dead scatter table only (MMU tables from 0x41EF8000)
 *   7 T2-c  0x41EFDC00-0x41FFFFFF  1 MB    above the MMU L1 / L2 tables, no reference */
#include "canary_cave.h"

#include "ca79ng/ramcanary.h"
#include "hal_cave.h"      /* hal_time_us, hal_midi_tx, cave_port_from_stock */
#include "sysex_cave.h"    /* sxc_ctrl_bound */
#include "bt_cave.h"       /* btc_ctrl_bound */
#include "power_stock.h"   /* stk_binding */

#define G 0x1000u   /* guard at each edge */
static const rcn_region_t CNC_TAB[CNC_REGIONS] = {
    {0x40600000u + G, 0x40A00000u - G, 0x40600000u + G},
    {0x41943000u + G, 0x41D00000u - G, 0x41943000u + G},
    {0x403C0000u + G, 0x40400000u - G, 0x403C0000u + G},
    {0x40001000u + G, 0x40060000u - G, 0x40001000u + G},
    {0x400B4000u + G, 0x400C0000u - G, 0x400B4000u + G},
    {0x40C00000u + G, 0x40E00000u - G, 0x40C00000u + G},
    {0x41D00000u + G, 0x41EF8000u - G, 0x41D00000u + G},
    {0x41EFE000u + G, 0x42000000u - G, 0x41EFE000u + G},
};

/* payload .data (explicit initialisers: nothing zeroes .bss) - fresh with every payload copy, off at every boot */
static rcn_t cn = {0};
static uint8_t inited = 0;
static uint32_t last_us = 0, max_us = 0;
/* the one queued request: task 19 fills it and raises `req`, task 24 answers and lowers it */
static volatile uint8_t req = 0;
static volatile uint8_t req_port = 0, req_sub = 0, req_a = 0, req_b = 0;

static int bound(uint8_t port) { return btc_ctrl_bound(sxc_ctrl_bound(port), stk_binding, port) != 0; }

static uint16_t pack7(const uint8_t *in, uint16_t n, uint8_t *out)
{
    uint16_t o = 0;
    for (uint16_t i = 0; i < n; i += 7) {
        uint16_t g = (uint16_t)(n - i < 7 ? n - i : 7);
        uint8_t hi = 0;
        for (uint16_t k = 0; k < g; k++)
            hi |= (uint8_t)(((in[i + k] >> 7) & 1u) << k);
        out[o++] = hi;
        for (uint16_t k = 0; k < g; k++)
            out[o++] = in[i + k] & 0x7F;
    }
    return o;
}

/* F0 7D 'C' 'A' '7' '9' 60 <sub> [head bytes] <pack7 raw> F7 */
static void send(uint8_t port, uint8_t sub, const uint8_t *head, uint8_t nh, const uint8_t *raw, uint16_t n)
{
    uint8_t m[9 + 3 + RCN_REGION + (RCN_REGION + 6) / 7];
    uint16_t o = 0;
    m[o++] = 0xF0, m[o++] = 0x7D, m[o++] = 0x43, m[o++] = 0x41, m[o++] = 0x37, m[o++] = 0x39;
    m[o++] = CNC_OP | 0x40;
    m[o++] = sub;
    for (uint8_t i = 0; i < nh; i++)
        m[o++] = head[i] & 0x7F;
    if (n)
        o = (uint16_t)(o + pack7(raw, n, m + o));
    m[o++] = 0xF7;
    hal_midi_tx(cave_port_from_stock(port), m, o);
}

static void answer(void)
{
    uint8_t port = req_port, h[3];
    if (req_sub == 0x00) {
        uint8_t raw[RCN_STATUS];
        rcn_encode_status(&cn, RCN_PASS_WORDS, last_us, max_us, raw);
        send(port, 0x00, 0, 0, raw, RCN_STATUS);
    } else if (req_sub == 0x02) {
        uint8_t raw[RCN_REGION];
        h[0] = req_a;
        rcn_encode_region(&cn, req_a, raw);
        send(port, 0x02, h, 1, raw, RCN_REGION);
    } else {
        if (req_sub == 0x01) {
            uint16_t mask = (uint16_t)(req_a | (uint16_t)req_b << 7);
            if (!bound(port)) {
                h[0] = CNC_RC_NOT_BOUND;
            } else if (rcn_arm(&cn, mask)) {
                h[0] = CNC_RC_ARG;
            } else {
                h[0] = CNC_RC_OK;
                last_us = max_us = 0;
            }
        } else {
            rcn_disarm(&cn);
            h[0] = CNC_RC_OK;
        }
        h[1] = (uint8_t)(cn.armed & 0x7F);
        h[2] = (uint8_t)(cn.armed >> 7);
        send(port, req_sub, h, 3, 0, 0);
    }
}

void cnc_tick(void)
{
    if (!inited) {
        rcn_init(&cn, CNC_TAB, CNC_REGIONS);
        inited = 1;
    }
    if (req) {
        answer();
        req = 0;
    }
    if (cn.armed) {
        uint32_t t0 = hal_time_us();
        rcn_step(&cn, RCN_PASS_WORDS);
        last_us = hal_time_us() - t0;
        if (last_us > max_us)
            max_us = last_us;
    }
}

void cnc_sysex(uint8_t port, const uint8_t *buf, uint32_t len)
{
    uint8_t sub = len >= 9 ? buf[7] : 0;
    if (req)
        return;   /* busy: dropped (silent) */
    if (!(len == 8 || (len == 9 && (sub == 0x00 || sub == 0x03)) || (len == 10 && sub == 0x02 && buf[8] < CNC_REGIONS) ||
          (len == 11 && sub == 0x01)))
        return;   /* unknown form: silent, like an unknown op */
    req_port = port & 3u;
    req_sub = sub;
    req_a = len >= 10 ? buf[8] & 0x7F : 0;
    req_b = len == 11 ? buf[9] & 0x7F : 0;
    req = 1;
}
