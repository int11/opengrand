#include <string.h>

#include "ca79ng/params_gen.h"
#include "ca79ng/router.h"

static void sound_id(int idx, uint8_t out[3])
{
    out[0] = SOUND_TABLE[idx].msb;
    out[1] = SOUND_TABLE[idx].lsb;
    out[2] = SOUND_TABLE[idx].pc;
}

bool progmap_lookup(multitimbral_t mode, uint8_t msb, uint8_t lsb, uint8_t pc, uint8_t out[3])
{
    pc &= 0x7F;
    if (mode != MT_ON2) {
        for (unsigned i = 0; i < SOUND_COUNT; i++)
            if (SOUND_TABLE[i].prog == pc) { /* PC = position in the program change list */
                sound_id((int)i, out);
                return true;
            }
        return false;
    }
    const progmap_entry_t *exact = NULL, *capital = NULL, *any = NULL;
    for (unsigned i = 0; i < PROGMAP_ON2_COUNT; i++) {
        const progmap_entry_t *e = &PROGMAP_ON2[i];
        if (e->on2_pc != pc + 1)
            continue;
        if (!any) any = e;
        if (!capital && e->msb == 121 && e->lsb == 0) capital = e;
        if (e->msb == msb && e->lsb == lsb) { exact = e; break; }
    }
    const progmap_entry_t *e = exact ? exact : (capital ? capital : any);
    if (!e)
        return false;
    if (e->sound >= 0) {
        sound_id(e->sound, out);
    } else {
        out[0] = e->msb;
        out[1] = e->lsb;
        out[2] = (uint8_t)(pc | 0x80);
    }
    return true;
}

void router_reset_banks(router_t *r)
{
    memset(r->bank_msb, 0, sizeof r->bank_msb);
    memset(r->bank_lsb, 0, sizeof r->bank_lsb);
}
