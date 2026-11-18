/* OpenGrand slot payload ota_main - the runtime hooks of the power port (written for ota_power)
 * (result/firmware/ports/cave/power_cave.c).
 *
 * attach() (ota_loader_v2, struct ota_svc) registers the four power sites of the five allow-listed sites
 * Each stub saves what the site's contract needs,
 * calls its C body, and ends in the stock target:
 *
 *   0x40147E94  Kawai SysEx -> FUN_4014A2C8   task 19   pwc_kawai_peek(r0 port, r1 buf, r2 len):
 *               loads + one store, no call; then `b` to the stock target with r0-r3, ip, lr as
 *               the stock `bl` left them (no payload frame survives the tail call, nothing blocks
 *               before it: contract (a) of the hook service)
 *   0x40143AB4  headphone poll -> FUN_400ACFA0  task 24  pwc_hp_pass() == 0 -> return to site + 4
 *               without the poll (its result is unused, the next instruction is a bl)
 *   0x40143AB8  executor -> FUN_400ACD28        task 24  pwc_exec_pre(), then the executor
 *   0x400AA874  20 ms kernel timer -> FUN_400ABFE8   pwc_apo_timer(): loads / stores only, no call,
 *               no wait (OTA_HKF_NOBLOCK); the stock target always runs (power switch tick)
 *
 * The fifth, 0x401539AC (MIDI pump tail), is the session core's (below).  On ota_loader (v1) attach() is never called:
 * no hooks, pwc_hooks_attached() never says otherwise, and the port refuses standby (rc 5). */
#include "ota.h"
#include "s0.h"
#include "power_cave.h"
/* Built only into ota_main (every feature; the power-only payloads ota_power / ota_power2 / ota_power3 are frozen
 * blobs and the per-feature ota_sysex / ota_session were discarded), so
 * the SysEx front and the session core are always here.
 *
 * The 0x40147E94 stub is hk_sx_kawai - the panel touch peek as above,
 * then the Phase 3-1 SysEx front (ports/cave/sysex_cave.c).  It owns the fn 0x10 set of the bound controller
 * (returns to the site + 4 without the stock handler) or returns 0 and the stub tail-calls FUN_4014A2C8 with
 * r0-r3, ip, lr as the stock `bl` left them.  The front may block (TX writer, the stock apply path) and holds
 * svc->inflight meanwhile - contract (b) of the hook service; the stub itself never blocks after
 * the flag is back at 0.  hk_pw_kawai (the peek alone, ota_power's stub) stays assembled: it shares the section
 * with the others, and ota_main's bytes include it.
 *
 * The session core adds a fifth hook at the MIDI pump tail
 * 0x401539AC (task 19, every 2 ms) - hk_ss_midi calls ssc_pump() (the session core's sender timer; it holds
 * svc->inflight while it may block, contract (b)) and then `b` to the stock target with r0-r3, ip, lr as the
 * `bl` left them; r4 / r5 (the pump loop's) are callee-saved through the C call.  Before ssc_pump the same stub runs
 * pwc_midi_tail() (a leaf: the APO reset of a pump pass that carried only polls / reads is undone, PWC_QUIET). */
#if !CA79NG_SESSION
#error "power_hooks.c is the hook set of ota_main: build it with the session core (FEAT2 0x02, -DCA79NG_SESSION=1)"
#endif
#include "sysex_cave.h"
void hk_ss_midi(void);

void hk_pw_kawai(void);
void hk_pw_hp(void);
void hk_pw_exec(void);
void hk_pw_apo(void);
void hk_sx_kawai(void);
#define HK_KAWAI hk_sx_kawai

__asm__(
    "    .pushsection .text.hk_pw, \"ax\", %progbits\n"
    "    .arm\n"
    "    .global hk_pw_kawai\n"
    "    .type   hk_pw_kawai, %function\n"
    "hk_pw_kawai:\n"
    "    push    {r0-r3, ip, lr}\n"
    "    bl      pwc_kawai_peek\n"
    "    pop     {r0-r3, ip, lr}\n"
    "    b       stk_hk_kawai\n"
    "    .size   hk_pw_kawai, . - hk_pw_kawai\n"
    "    .global hk_sx_kawai\n"
    "    .type   hk_sx_kawai, %function\n"
    "hk_sx_kawai:\n"
    "    push    {r0-r3, ip, lr}\n"
    "    bl      pwc_kawai_peek\n"
    "    ldm     sp, {r0-r2}\n"
    "    bl      sxc_kawai\n"
    "    cmp     r0, #0\n"
    "    pop     {r0-r3, ip, lr}\n"
    "    bxne    lr\n"
    "    b       stk_hk_kawai\n"
    "    .size   hk_sx_kawai, . - hk_sx_kawai\n"
    "    .global hk_ss_midi\n"
    "    .type   hk_ss_midi, %function\n"
    "hk_ss_midi:\n"
    "    push    {r0-r3, ip, lr}\n"
    "    bl      pwc_midi_tail\n"
    "    bl      ssc_pump\n"
    "    pop     {r0-r3, ip, lr}\n"
    "    b       stk_hk_midi\n"
    "    .size   hk_ss_midi, . - hk_ss_midi\n"
    "    .global hk_pw_hp\n"
    "    .type   hk_pw_hp, %function\n"
    "hk_pw_hp:\n"
    "    push    {r4, lr}\n"
    "    bl      pwc_hp_pass\n"
    "    cmp     r0, #0\n"
    "    pop     {r4, lr}\n"
    "    bxeq    lr\n"
    "    b       stk_hk_hp\n"
    "    .size   hk_pw_hp, . - hk_pw_hp\n"
    "    .global hk_pw_exec\n"
    "    .type   hk_pw_exec, %function\n"
    "hk_pw_exec:\n"
    "    push    {r4, lr}\n"
    "    bl      pwc_exec_pre\n"
    "    pop     {r4, lr}\n"
    "    b       stk_hk_exec\n"
    "    .size   hk_pw_exec, . - hk_pw_exec\n"
    "    .global hk_pw_apo\n"
    "    .type   hk_pw_apo, %function\n"
    "hk_pw_apo:\n"
    "    push    {r4, lr}\n"
    "    bl      pwc_apo_timer\n"
    "    pop     {r4, lr}\n"
    "    b       stk_hk_apo\n"
    "    .size   hk_pw_apo, . - hk_pw_apo\n"
    "    .popsection\n");

/* Phase 4 (feat3 0x10): three ota_loader_v3 sites in task 19.  Off
 * (mcc_on 0): `b` to the stock target, every register as the parser's / the task's `bl` left it.  On, PC / CC: the core's
 * judgement first (a call that only reads) - 0 drop: return to the parser; 1 stock: tail call as when off; 2 setting:
 * `b` into mcc_*_run, which runs the stock routine under svc->inflight and returns to the parser itself.  The delayed
 * apply (r0 = elapsed) goes to mcc_defer, the core's walk of the same tables. */
void hk_mc_pc(void);
void hk_mc_cc(void);
void hk_mc_defer(void);
__asm__(
    "    .pushsection .text.hk_mc, \"ax\", %progbits\n"
    "    .arm\n"
    "    .global hk_mc_pc\n"
    "    .type   hk_mc_pc, %function\n"
    "hk_mc_pc:\n"
    "    ldr     ip, .Lmc_on\n"
    "    ldrb    ip, [ip]\n"
    "    cmp     ip, #0\n"
    "    beq     stk_mc_pc\n"
    "    push    {r0-r3, ip, lr}\n"
    "    bl      mcc_pc_judge\n"
    "    cmp     r0, #1\n"
    "    pop     {r0-r3, ip, lr}\n"
    "    beq     stk_mc_pc\n"
    "    bhi     mcc_pc_run\n"
    "    bx      lr\n"
    "    .size   hk_mc_pc, . - hk_mc_pc\n"
    "    .global hk_mc_cc\n"
    "    .type   hk_mc_cc, %function\n"
    "hk_mc_cc:\n"
    "    ldr     ip, .Lmc_on\n"
    "    ldrb    ip, [ip]\n"
    "    cmp     ip, #0\n"
    "    beq     stk_mc_cc\n"
    "    push    {r0-r3, ip, lr}\n"
    "    bl      mcc_cc_judge\n"
    "    cmp     r0, #1\n"
    "    pop     {r0-r3, ip, lr}\n"
    "    beq     stk_mc_cc\n"
    "    bhi     mcc_cc_run\n"
    "    bx      lr\n"
    "    .size   hk_mc_cc, . - hk_mc_cc\n"
    "    .global hk_mc_defer\n"
    "    .type   hk_mc_defer, %function\n"
    "hk_mc_defer:\n"
    "    ldr     ip, .Lmc_on\n"
    "    ldrb    ip, [ip]\n"
    "    cmp     ip, #0\n"
    "    beq     stk_mc_defer\n"
    "    b       mcc_defer\n"
    "    .size   hk_mc_defer, . - hk_mc_defer\n"
    "    .align  2\n"
    ".Lmc_on:\n"
    "    .word   mcc_on\n"
    "    .popsection\n");

/* ota_loader_v3 allow-list words */
static const struct {
    unsigned int site, orig;
    void (*stub)(void);
} MC_SITES[] = {
    {MC_SITE_PC, 0xEBFFFA5Eu, hk_mc_pc},
    {MC_SITE_CC, 0xEBFFFD6Cu, hk_mc_cc},
    {MC_SITE_DEFER, 0xEB003FD0u, hk_mc_defer},
};

/* only a v3 resident (svc version 2) has these sites; on ota_loader_v2 nothing is asked and the port stays off */
static unsigned char mc_attach(const struct ota_svc *svc)
{
    unsigned char m = 0;
    if (svc->version < 2)
        return 0;
    for (unsigned int i = 0; i < sizeof MC_SITES / sizeof MC_SITES[0]; i++)
        if (svc->hook_set(MC_SITES[i].site, MC_SITES[i].orig, (unsigned int)MC_SITES[i].stub, 0) == OTA_HK_OK)
            m |= (unsigned char)(1u << i);
    return m;
}

/* Phase 4 executor gate (feat3 0x08): two engine-loop sites of the
 * ota_loader_v3 allow-list, hooked only while the gate is on (xgate_cave.c xgc_tick, never in attach()).  Task 24 =
 * the resident's task: the body runs and returns; 0 -> back to the site + 4 without the stock executor (its pending
 * byte stays), else `b` to it - both executors take no argument. */
#include "xgate_cave.h"
void hk_xg_media(void);
void hk_xg_xport(void);
__asm__(
    "    .pushsection .text.hk_xg, \"ax\", %progbits\n"
    "    .arm\n"
    "    .global hk_xg_media\n"
    "    .type   hk_xg_media, %function\n"
    "hk_xg_media:\n"
    "    push    {r4, lr}\n"
    "    bl      xgc_media_pass\n"
    "    cmp     r0, #0\n"
    "    pop     {r4, lr}\n"
    "    bxeq    lr\n"
    "    b       stk_hk_media\n"
    "    .size   hk_xg_media, . - hk_xg_media\n"
    "    .global hk_xg_xport\n"
    "    .type   hk_xg_xport, %function\n"
    "hk_xg_xport:\n"
    "    push    {r4, lr}\n"
    "    bl      xgc_xport_pass\n"
    "    cmp     r0, #0\n"
    "    pop     {r4, lr}\n"
    "    bxeq    lr\n"
    "    b       stk_hk_xport\n"
    "    .size   hk_xg_xport, . - hk_xg_xport\n"
    "    .popsection\n");

static const struct ota_svc *xg_svc = 0;   /* attach()'s service; 0 on ota_loader (v1): attach() is never called */

int xgh_have_svc(void)
{
    return xg_svc != 0;
}

int xgh_set(int which)
{
    if (!xg_svc)
        return 0xFF;
    return which ? xg_svc->hook_set(XGC_SITE_XPORT, XGC_WORD_XPORT, (unsigned int)hk_xg_xport, 0)
                 : xg_svc->hook_set(XGC_SITE_MEDIA, XGC_WORD_MEDIA, (unsigned int)hk_xg_media, 0);
}

int xgh_clear(int which)
{
    if (!xg_svc)
        return 0xFF;
    return xg_svc->hook_clear(which ? XGC_SITE_XPORT : XGC_SITE_MEDIA);
}

/* fn 0x24 Read job with 7-bit DataPacket counters (ports/cave/fread_cave.c):
 * the ota_loader_v3 engine-loop site #15 0x40143ACC (task 24, the resident's task: the body runs - possibly a whole
 * stop-and-wait transfer, as the stock job does - and returns).  0 -> back to the site + 4 (the Read was done here),
 * else `b` to the stock executor FUN_4014F364 (no argument).  Hooked in attach() on a v3 resident only. */
#include "fread_cave.h"
void hk_fr_file(void);
__asm__(
    "    .pushsection .text.hk_fr, \"ax\", %progbits\n"
    "    .arm\n"
    "    .global hk_fr_file\n"
    "    .type   hk_fr_file, %function\n"
    "hk_fr_file:\n"
    "    push    {r4, lr}\n"
    "    bl      frc_pass\n"
    "    cmp     r0, #0\n"
    "    pop     {r4, lr}\n"
    "    bxeq    lr\n"
    "    b       stk_hk_file\n"
    "    .size   hk_fr_file, . - hk_fr_file\n"
    "    .popsection\n");

/* allow-list words (ota_loader_v2 hk_sites, = the stock image's `bl` at each site) */
static const struct {
    unsigned int site, orig;
    void (*stub)(void);
    unsigned char bit, flags;
} SITES[] = {
    {PW_SITE_KAWAI, 0xEB00090Bu, HK_KAWAI, PW_HK_KAWAI, 0},
    {PW_SITE_HP, 0xEBFDA539u, hk_pw_hp, PW_HK_HP, 0},
    {PW_SITE_EXEC, 0xEBFDA49Au, hk_pw_exec, PW_HK_EXEC, 0},
    {PW_SITE_APO, 0xEB0005DBu, hk_pw_apo, PW_HK_APO, OTA_HKF_NOBLOCK},
};

volatile unsigned char pw_attach_rc[4] = {0xFF, 0xFF, 0xFF, 0xFF};

void pw_attach(const struct ota_svc *svc)
{
    unsigned char mask = 0;
    unsigned int i;
    if (svc->magic != OTA_SVC_MAGIC || svc->version < 1)
        return;
    for (i = 0; i < sizeof SITES / sizeof SITES[0]; i++) {
        int rc = svc->hook_set(SITES[i].site, SITES[i].orig, (unsigned int)SITES[i].stub, SITES[i].flags);
        pw_attach_rc[i] = (unsigned char)rc;
        if (rc == OTA_HK_OK)
            mask |= SITES[i].bit;
    }
    pwc_hooks_attached(mask);
    sxc_attached((mask & PW_HK_KAWAI) != 0, svc->inflight);
    ssc_attached(svc->hook_set(SS_SITE_MIDI, 0xEBFF5B88u, (unsigned int)hk_ss_midi, 0) == OTA_HK_OK);
    mcc_attached(mc_attach(svc));
    frc_attached(svc->version >= 2 ? svc->hook_set(FRC_SITE, FRC_WORD, (unsigned int)hk_fr_file, 0) : 0xFF);
    xg_svc = svc;   /* the gate hooks its two sites later, from the engine loop, only when switched on */
    /* ota_loader_v4 (svc version 3): the kvstore writes go through stage-0's guarded NOR service - the only NOR
     * writer this payload has (no stock writer is linked); below 3 the kvstore stays read-only */
    if (svc->version >= 3 && svc->nor && svc->nor->magic == S0_SVC_MAGIC && svc->nor->version >= 1)
        pwc_nor_bind(svc->nor->nor_erase, svc->nor->nor_prog);
}
