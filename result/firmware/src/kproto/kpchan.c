/* OpenGrand - Phase 4: MIDI channel messages -> settings, delayed parameter apply.  See kpchan.h.
 * Every rule below is one branch of the stock routines (REA). */
#include "ca79ng/kpchan.h"

/* FUN_40146D04 */
uint8_t kch_path(const kch_in_t *in, uint8_t ch)
{
    if (in->bound)
        return ch == 0 ? 0 : 2;
    if (in->run)
        return 0;
    if (in->sub3)
        return 2;
    return in->mode;
}

/* FUN_40146D70 (the omni byte is passed in by both callers) */
bool kch_accept(const kch_in_t *in, uint8_t ch)
{
    if (in->bound)
        return ch == 0;
    return in->omni != 0 || in->rx_ch == ch;
}

static kch_dec_t dec(uint8_t act, uint8_t path)
{
    kch_dec_t d = {.act = act, .key = {0, 0, 0}, .side = 0, .path = path};
    return d;
}

static kch_dec_t setting(uint8_t path, uint8_t v1, uint8_t v2, uint8_t part, uint8_t side)
{
    kch_dec_t d = {.act = KCH_SETTING, .key = {v1, v2, part}, .side = side, .path = path};
    return d;
}

/* FUN_401475CC: path 0 + accepted -> FUN_4016053C(ctx, 0x7F, 0xB0, {0}) then FUN_4016053C(ctx, 0, 7, {msb, lsb, pc}) =
 * Sound of part slot 0 (key 55 00 00; the Sound apply also sets the sound's effects - side); path 0 not accepted ->
 * nothing; any other path -> the part voice FUN_401639C4 (run state) */
kch_dec_t kch_pc(const kch_in_t *in, uint8_t ch)
{
    uint8_t p = kch_path(in, ch);
    if (p != 0)
        return dec(KCH_STOCK, p);
    if (!kch_accept(in, ch))
        return dec(KCH_DROP, p);
    return setting(p, 0x55, 0x00, 0x00, 1);
}

/* Local Control 0x7A (both paths): only on the receive channel, only 0 / 0x7F -> FUN_4016053C(ctx, 0x7F, 0x5C) = 58 01 7F */
static kch_dec_t local(uint8_t p, const kch_in_t *in, uint8_t ch, uint8_t val)
{
    if (in->rx_ch != ch || (val != 0 && val != 0x7F))
        return dec(KCH_DROP, p);
    return setting(p, 0x58, 0x01, 0x7F, 0);
}

/* FUN_40148218 */
kch_dec_t kch_cc(const kch_in_t *in, uint8_t ch, uint8_t cc, uint8_t val)
{
    uint8_t p = kch_path(in, ch);
    if (p != 0) {
        /* the part path: every CC is part run state (bank / RPN / NRPN tables, the per-part table *0x40148B1C, notes
         * off, omni byte) except Local Control */
        if (cc == 0x7A)
            return local(p, in, ch, val);
        return dec(cc < 0x80 ? KCH_STOCK : KCH_DROP, p);
    }
    if (!kch_accept(in, ch))
        return dec(KCH_DROP, p);
    switch (cc) {
    case 0x00:            /* bank MSB table                                        */
    case 0x20:            /* bank LSB table *0x40148B20                             */
    case 0x07:            /* volume  FUN_4015ABE4                                   */
    case 0x0A:            /* pan     FUN_4015ACC0                                   */
    case 0x0B:            /* expression FUN_4015AD08                                */
    case 0x40:            /* damper  FUN_4015AD50                                   */
    case 0x42:            /* sostenuto FUN_4015AC2C                                 */
    case 0x43:            /* soft    FUN_4015AC78                                   */
    case 0x78:            /* all sound off FUN_40154014                             */
    case 0x7B:            /* all notes off + the note tables                       */
    case 0x7C:            /* omni off: notes off, block +0x32 = 0 on the rx channel (no key reads it) */
    case 0x7D:            /* omni on:  notes off, block +0x32 = 1 on the rx channel */
    case 0x7E:
    case 0x7F:
        return dec(KCH_STOCK, p);
    case 0x50:            /* FUN_40154098 / FUN_40154030 / FUN_40153DA4 / FUN_40153E0C: only while FUN_40135980() != 0 */
    case 0x51:
    case 0x52:
    case 0x53:
        return dec(in->run ? KCH_STOCK : KCH_DROP, p);
    case 0x5C:            /* FUN_4015A0AC (FUN_40135980() == 0, the layer's effect type 0x12..0x17) -> FUN_40159EFC:
                           * block + layer * 0x34 + 0x5D = 0 / 0x7F = 55 14 <layer part> */
        return in->run ? dec(KCH_DROP, p) : setting(p, 0x55, 0x14, KCH_PART_ALL, 0);
    case 0x7A:
        return local(p, in, ch, val);
    default:
        return dec(KCH_DROP, p);
    }
}

/* FUN_401638E0, instruction order kept (0x401638E0-0x401639B0): the slot is cleared before the apply, the value byte set
 * to 0xFF after it, the armed word rewritten at the end (an apply that re-arms is overwritten, as in the stock) */
uint32_t kch_defer(volatile int32_t *tm, volatile uint8_t *val, volatile uint32_t *any, int32_t elapsed,
                   void (*apply)(void *ctx, uint8_t c, uint8_t x, uint8_t value), void *ctx)
{
    uint32_t still = 0, n = 0;
    if (*any == 0)
        return 0;
    for (uint8_t c = 0; c < KCH_DEF_N; c++) {
        for (uint8_t x = 0; x < KCH_DEF_X; x++) {
            uint32_t i = (uint32_t)c * KCH_DEF_X + x;
            int32_t t = tm[i];
            if (t <= 0)
                continue;
            if (t > elapsed) {   /* subs / bgt: the signed comparison, no wrap */
                tm[i] = t - elapsed;
                still = 1;
                continue;
            }
            tm[i] = 0;
            if (val[i] < 0x80) {
                apply(ctx, c, x, val[i]);
                val[i] = 0xFF;
                n++;
            }
        }
    }
    *any = still;
    return n;
}
