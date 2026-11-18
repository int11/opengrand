/* OpenGrand runtime-hook demo payload (image ota_hookdemo).
 *
 * The smallest payload that uses the ota_loader_v2 hook service (include/ota.h struct ota_svc):
 * in attach() it rewrites the Kawai SysEx call 0x40147E94 (`bl FUN_4014A2C8`, the Phase 3-1
 * interception point) to a pass-through counter, and reports the count with 0x7D op 0x0B.
 * Every other SysEx keeps its stock path - the stub tail-calls FUN_4014A2C8 with r0-r3, lr and
 * sp exactly as the stock `bl` left them (lr = 0x40147E98), so no payload frame is ever live
 * while the stock handler runs (it may block in a TX semaphore - the task-19 contract).
 *
 * The other four allow-listed sites get the same kind of stub (not registered by attach();
 * tools/roundtrip_ota_hooks.py registers them to run each stock path through a patched site).
 * On ota_loader (v1) attach() is never called: identify answers, op 0x0B reports attach rc 0xFF.
 * No core, no stock writes, no flash.
 *
 * -DHOOKDEMO_V3=1 (payload ota_hookdemo3): the same with one
 * stub per site of the ota_loader_v3 allow-list (17, linked with hook_demo3.ld) and op 0x0B layout 2
 * (17 counters).  Without the macro this file compiles byte for byte as ota_hookdemo.            */
#include "stock.h"
#include "ota.h"

#define TAG0 0x43
#define TAG1 0x41
#define TAG2 0x37
#define TAG3 0x39
#define OP_IDENTIFY 0x01
#define OP_HOOKDEMO 0x0B

#define SITE_KAWAI 0x40147E94u
#define ORIG_KAWAI 0xEB00090Bu

/* allow-list order: kawai, midi tail, headphone poll, executor, APO (20 ms)
 * (+ v3: program change, control change, delayed timers, task-20 pass, task-20 tail, Music queue,
 *  metronome, remote mode, media result, file op, transport, mode-3 notify) */
#ifdef HOOKDEMO_V3
#define NSTUB 17
volatile u32 hk_count[NSTUB] = { 0 };
#else
#define NSTUB 5
volatile u32 hk_count[5] = { 0, 0, 0, 0, 0 };
#endif
static const struct ota_svc *svc = 0;
static u8 attach_rc = 0xFF;

/* pass-through stubs: count, then tail-call the stock target.  Only ip and 4 bytes of stack
 * below sp are touched (sp is back before the branch); flags are not changed.  No call that
 * can block: valid at a kernel-timer site too.                                              */
#define STUB(name, idx, target)                                   \
    "    .global " name "\n"                                       \
    "    .type   " name ", %function\n"                            \
    name ":\n"                                                     \
    "    push    {r0}\n"                                           \
    "    ldr     ip, =hk_count + " #idx " * 4\n"                   \
    "    ldr     r0, [ip]\n"                                       \
    "    add     r0, r0, #1\n"                                     \
    "    str     r0, [ip]\n"                                       \
    "    pop     {r0}\n"                                           \
    "    b       " target "\n"                                     \
    "    .ltorg\n"                                                 \
    "    .size   " name ", . - " name "\n"
void hk_kawai(void);
__asm__(
    "    .pushsection .text.hk_stubs, \"ax\", %progbits\n"
    "    .arm\n"
    STUB("hk_kawai", 0, "stk_hk_kawai")
    STUB("hk_midi", 1, "stk_hk_midi")
    STUB("hk_hp", 2, "stk_hk_hp")
    STUB("hk_exec", 3, "stk_hk_exec")
    STUB("hk_apo", 4, "stk_hk_apo")
#ifdef HOOKDEMO_V3
    STUB("hk_pc", 5, "stk_hk_pc")
    STUB("hk_cc", 6, "stk_hk_cc")
    STUB("hk_dly", 7, "stk_hk_dly")
    STUB("hk_t20", 8, "stk_hk_t20")
    STUB("hk_t20tail", 9, "stk_hk_t20tail")
    STUB("hk_mq", 10, "stk_hk_mq")
    STUB("hk_metro", 11, "stk_hk_metro")
    STUB("hk_rmode", 12, "stk_hk_rmode")
    STUB("hk_media", 13, "stk_hk_media")
    STUB("hk_file", 14, "stk_hk_file")
    STUB("hk_xport", 15, "stk_hk_xport")
    STUB("hk_notify", 16, "stk_hk_notify")
#endif
    "    .popsection\n");

static void tx(u32 port, const u8 *buf, u32 len)
{
    switch (port & 3) {
    case 0: stk_tx_port0(buf, len); break;
    case 1: stk_tx_port1(buf, len); break;
    case 2: stk_tx_port2(buf, len); break;
    default: stk_tx_port3(buf, len); break;
    }
}

static void reply(u32 port, u8 op, const u8 *raw, u32 n, int packed)
{
    u8 msg[NSTUB == 5 ? 48 : 8 + 96];   /* v3: 72 raw -> 83 packed + 8 */
    u32 i = 0;
    msg[i++] = 0xF0; msg[i++] = 0x7D;
    msg[i++] = TAG0; msg[i++] = TAG1; msg[i++] = TAG2; msg[i++] = TAG3;
    msg[i++] = (u8)(op | 0x40);
    if (!packed) {
        while (n--)
            msg[i++] = *raw++ & 0x7F;
    } else {
        while (n) {                       /* pack7: a high-bit byte, then up to 7 low-7 bytes */
            u32 g = n < 7 ? n : 7, k;
            u8 hi = 0;
            for (k = 0; k < g; k++)
                if (raw[k] & 0x80)
                    hi |= (u8)(1u << k);
            msg[i++] = hi;
            for (k = 0; k < g; k++)
                msg[i++] = raw[k] & 0x7F;
            raw += g;
            n -= g;
        }
    }
    msg[i++] = 0xF7;
    tx(port, msg, i);
}

static void demo_attach(const struct ota_svc *s)
{
    if (s->magic != OTA_SVC_MAGIC || s->version < 1) {
        attach_rc = 0xFE;
        return;
    }
    svc = s;
    attach_rc = (u8)s->hook_set(SITE_KAWAI, ORIG_KAWAI, (u32)hk_kawai, 0);
}

static void demo_sysex(u32 port, const u8 *buf, u32 len)
{
    if (len < 8 || buf[0] != 0xF0 || buf[1] != 0x7D || buf[len - 1] != 0xF7 ||
        buf[2] != TAG0 || buf[3] != TAG1 || buf[4] != TAG2 || buf[5] != TAG3)
        return;
    if (buf[6] == OP_IDENTIFY) {
        static const u8 d[5] = { 1, 2, 19, CA79NG_FEAT0, CA79NG_FEAT1 };
        reply(port, OP_IDENTIFY, d, 5, 0);
    } else if (buf[6] == OP_HOOKDEMO) {
        u8 r[4 + 4 * NSTUB];
        u32 i;
        r[0] = NSTUB == 5 ? 1 : 2;        /* layout */
        r[1] = attach_rc;
        r[2] = svc ? (u8)svc->version : 0;
        r[3] = 0;
        for (i = 0; i < NSTUB; i++) {
            u32 v = hk_count[i];
            r[4 + 4 * i] = (u8)v; r[5 + 4 * i] = (u8)(v >> 8);
            r[6 + 4 * i] = (u8)(v >> 16); r[7 + 4 * i] = (u8)(v >> 24);
        }
        reply(port, OP_HOOKDEMO, r, sizeof r, 1);
    }
}

static void demo_tick(void)
{
}

/* nothing pending, no stock resource held, and the stubs never leave a payload frame */
static int demo_quiesce(void)
{
    return 0;
}

__attribute__((section(".ota_entry"), used))
const struct ota_entry_x ota_entry = {
    { OTA_ENTRY_MAGIC, OTA_ABI, demo_sysex, demo_tick,
      (CA79NG_FEAT0) | ((CA79NG_FEAT1) << 8) | OTA_EF_ATTACH, demo_quiesce },
    demo_attach
};
