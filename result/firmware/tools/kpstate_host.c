/* Host shim for result/stockfw/tools/roundtrip_state.py: the core's settings-state module (kpstate.c) and
 * parameter store as a shared library (build/libkpstate.so, ctypes), its io.match / io.value bound to
 * callbacks the round trip answers from the stock image under the emulator.  Test tool only. */
#include <string.h>

#include "ca79ng/kpstate.h"

typedef int (*kh_match_fn)(int i);
typedef int (*kh_value_fn)(int i, int j, uint8_t *out);

static param_store_t store;
static kst_t st;
static kh_match_fn cb_match;
static kh_value_fn cb_value;

static bool io_match(void *c, uint8_t i, uint8_t v1, uint8_t v2)
{
    (void)c;
    return v1 == 0x5F && v2 == 0x7F && cb_match(i) != 0;
}

static int io_value(void *c, uint8_t port, uint8_t cc, uint8_t i, uint8_t part, uint8_t j, uint8_t out[KST_FRAME_MAX])
{
    (void)c;
    (void)port;
    if (cc != 0x7F || part != 0x7F)
        return -1;
    return cb_value(i, j, out);
}

/* empty store (S1 fills it), or the core's generated defaults (defaults = 1) */
void kh_init(kh_match_fn m, kh_value_fn v, int defaults)
{
    kst_io_t io = {.ctx = 0, .match = io_match, .value = io_value};
    cb_match = m;
    cb_value = v;
    if (defaults)
        param_store_defaults(&store);
    else
        memset(&store, 0, sizeof store);
    kst_init(&st, &store, &io);
}

/* mode 1 fill, 2 compare; budget per kst_step (0 = all at once); returns the kst_step calls made */
int kh_run(int mode, int budget)
{
    int calls = 0;
    kst_start(&st, (kst_mode_t)mode);
    for (;;) {
        calls++;
        if (kst_step(&st, budget > 0 ? (uint32_t)budget : KST_DESC_N))
            return calls;
    }
}

/* frames stored unknown longval bad diffs passes synced diffs_total last_diff(3) last_unknown(3) indexed */
void kh_stats(uint32_t out[16])
{
    out[0] = st.st.frames;
    out[1] = st.st.stored;
    out[2] = st.st.unknown;
    out[3] = st.st.longval;
    out[4] = st.st.bad;
    out[5] = st.st.diffs;
    out[6] = st.st.passes;
    out[7] = st.synced;
    out[8] = st.diffs_total;
    for (int k = 0; k < 3; k++) {
        out[9 + k] = st.last_diff[k];
        out[12 + k] = st.last_unknown[k];
    }
    out[15] = st.st.indexed;
}

/* a side-table (sub-indexed) value: its length, 0 none, -1 not an indexed key */
int kh_idx_get(int v1, int v2, int part, int sub, uint8_t out[4])
{
    if (!kst_is_indexed((uint8_t)v1, (uint8_t)v2, (uint8_t)part))
        return -1;
    return kst_indexed_get(&st, (uint8_t)v1, (uint8_t)v2, (uint8_t)part, (uint8_t)sub, out);
}

/* the store's value for a (v1 v2 part) key: its length (descriptor len), -1 not a core key, 0 not valid */
int kh_get(int v1, int v2, int part, uint8_t out[4])
{
    int id = param_find(0x10, (uint8_t)v1, (uint8_t)v2, (uint8_t)part);
    int slot = param_slot_from_part((uint8_t)part);
    if (id < 0 || slot < 0)
        return -1;
    if (!param_get(&store, (param_id_t)id, slot, out))
        return 0;
    return PARAM_DESC[id].len;
}

/* a frame through kst_put_frame on the shim's store (result code, *changed) */
int kh_put_frame(const uint8_t *f, int n, int *changed)
{
    bool c;
    int r = (int)kst_put_frame(&store, f, (uint32_t)n, &c);
    *changed = c;
    return r;
}

/* number of valid (id, slot) entries in the store */
int kh_count(void)
{
    int n = 0;
    for (int id = 0; id < PRM_COUNT; id++)
        n += __builtin_popcount(store.valid[id]);
    return n;
}

/* S3: the read owner on / off; the frame the store answers for frame j of descriptor i with key
 * v1 v2 part and device byte cc (> 0 length, 0 = not the store's); its counters */
void kh_s3_enable(int on)
{
    kst_s3_enable(&st, on != 0);
}

int kh_s3_frame(int i, int j, int cc, int v1, int v2, int part, uint8_t out[KST_FRAME_MAX])
{
    const uint8_t key[3] = {(uint8_t)v1, (uint8_t)v2, (uint8_t)part};
    return kst_s3_frame(&st, (uint8_t)i, (uint8_t)j, (uint8_t)cc, key, out);
}

/* served stock untrusted miss stale fanned_key trusted */
void kh_s3_stats(uint32_t out[7])
{
    out[0] = st.s3.served;
    out[1] = st.s3.stock;
    out[2] = st.s3.untrusted;
    out[3] = st.s3.miss;
    out[4] = st.s3.stale;
    out[5] = st.s3.fanned_key;
    out[6] = kst_s3_trusted(&st);
}

int kh_s3_owned(int i)
{
    return kst_s3_owned((uint8_t)i);
}

/* S4 (roundtrip_state.py 8): is key v1 v2 part in the pure table; the descriptor value mask / length the gate uses */
int kh_s4_pure(int v1, int v2, int part)
{
    if (v1 < 0)
        return (int)kst_s4_pure_count();
    return kst_s4_pure((uint8_t)v1, (uint8_t)v2, (uint8_t)part) ? 1 : 0;
}

/* S4 judge of one set frame (F0 .. F7): the frame to apply into out (>= n B), its length (0 = nothing);
 * counts[4] = own, refused, passed, listed keys */
int kh_s4_judge(const uint8_t *m, int n, uint8_t *out, uint32_t counts[4])
{
    static kst_s4_set_t r;
    int len = (int)kst_s4_judge(&st, m, (uint32_t)n, out, &r);
    counts[0] = r.nown;
    counts[1] = r.refused;
    counts[2] = r.passed;
    counts[3] = r.n;
    return len;
}

/* a key the stock changes by itself (S2 KEY) */
int kh_key_stock(int v1, int v2, int part)
{
    return kst_key_stock((uint8_t)v1, (uint8_t)v2, (uint8_t)part) ? 1 : 0;
}
