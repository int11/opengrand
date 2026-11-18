/* OpenGrand cave port - Phase 3-6 R0: the recorder view (src/recorder/recview.c) over plain loads
 * from stock RAM.  Read only: no stock routine is called and no stock address is written.
 *
 * Not linked into any image yet (R0 adds no slot payload).  result/stockfw/tools/roundtrip_rec.py
 * builds this file + recview.c with the pinned ARM toolchain and runs rv_cave_snapshot() inside
 * the stock-image emulator, against stock getters. */
#include "ca79ng/recview.h"

static void cave_read(void *ctx, uint32_t addr, void *dst, uint32_t n)
{
    (void)ctx;
    const volatile uint8_t *s = (const volatile uint8_t *)(uintptr_t)addr;
    uint8_t *d = dst;
    for (uint32_t i = 0; i < n; i++)
        d[i] = s[i];
}

static const rv_mem_t CAVE_MEM = {cave_read, 0};

/* one full view, encoded (RV_ENC_LEN bytes) into out; the view itself lives in *scratch */
uint32_t rv_cave_snapshot(rv_view_t *scratch, uint8_t *out)
{
    rv_read(&CAVE_MEM, scratch);
    return (uint32_t)rv_encode(scratch, out, RV_ENC_LEN);
}

/* the 2 ms poll's shape: live fields every call, the song table only after a commit point;
 * returns rv_diff(prev, cur) (the caller then copies cur to prev) */
uint32_t rv_cave_poll(rv_view_t *prev, rv_view_t *cur)
{
    rv_read_live(&CAVE_MEM, cur);
    /* COMMIT depends on the live fields only, so cur's stale table does not matter here */
    if (rv_diff(prev, cur) & RV_CH_COMMIT)
        rv_read_store(&CAVE_MEM, cur);
    else {
        cur->store = prev->store, cur->store_bad = prev->store_bad, cur->store_end = prev->store_end;
        for (unsigned i = 0; i < RV_SONGS; i++)
            cur->songs[i] = prev->songs[i];
    }
    return rv_diff(prev, cur);
}
