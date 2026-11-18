/* Phase 3-4 settings state S1 (src/kproto/kpstate.c) against a fake stock: the dump walk, frame parsing
 * into the store, the sub-indexed side table, the compare pass, the task-19 budget
 * (the emulator round trip is result/stockfw/tools/roundtrip_state.py). */
#include <string.h>

#include "ca79ng/kpstate.h"
#include "test.h"

#define NF 16
/* the fake stock: per descriptor i, up to NF frames (key + data) answered for j = 0.., a "0" gap allowed */
static struct {
    uint8_t n[KST_DESC_N];             /* frames of descriptor i (0 = descriptor answers -1 at once)     */
    uint8_t gap[KST_DESC_N];           /* j that answers 0 (no frame for this j), 0xFF none               */
    uint8_t key[KST_DESC_N][NF][3];
    uint8_t data[KST_DESC_N][NF][8];
    uint8_t dlen[KST_DESC_N][NF];
    bool match[KST_DESC_N];
    uint32_t value_calls[KST_DESC_N];
    bool raw_bad[KST_DESC_N];          /* frame without its F7 */
} fk;

static void fk_reset(void)
{
    memset(&fk, 0, sizeof fk);
    memset(fk.gap, 0xFF, sizeof fk.gap);
}

static void fk_add(uint8_t i, uint8_t v1, uint8_t v2, uint8_t part, const uint8_t *d, uint8_t n)
{
    uint8_t k = fk.n[i]++;
    fk.match[i] = true;
    fk.key[i][k][0] = v1;
    fk.key[i][k][1] = v2;
    fk.key[i][k][2] = part;
    memcpy(fk.data[i][k], d, n);
    fk.dlen[i][k] = n;
}

static bool f_match(void *c, uint8_t i, uint8_t v1, uint8_t v2)
{
    (void)c;
    CHECK(v1 == 0x5F && v2 == 0x7F);
    return fk.match[i];
}

static int f_value(void *c, uint8_t port, uint8_t cc, uint8_t i, uint8_t part, uint8_t j, uint8_t out[KST_FRAME_MAX])
{
    uint8_t k, o = 0;
    (void)c;
    (void)port;
    CHECK(cc == 0x7F && part == 0x7F);
    fk.value_calls[i]++;
    if (j == fk.gap[i])
        return 0;
    k = (uint8_t)(j > fk.gap[i] ? j - 1 : j);
    if (k >= fk.n[i])
        return -1;
    out[o++] = 0xF0; out[o++] = 0x40; out[o++] = cc; out[o++] = 0x10; out[o++] = 0x04; out[o++] = 0x1E;
    memcpy(out + o, fk.key[i][k], 3);
    o += 3;
    memcpy(out + o, fk.data[i][k], fk.dlen[i][k]);
    o += fk.dlen[i][k];
    if (!fk.raw_bad[i])
        out[o++] = 0xF7;
    return o;
}

static const kst_io_t IO = {.ctx = 0, .match = f_match, .value = f_value};

static param_store_t store;
static kst_t st;

/* a small stock: the System values the app reads back + per-part Sound + a few odd frames */
static void fk_stock(void)
{
    fk_reset();
    fk_add(0x07, 0x55, 0x00, 0x00, (const uint8_t[]){0x00, 0x00, 0x00}, 3);   /* Sound, 3 parts (0x80 style) */
    fk_add(0x07, 0x55, 0x00, 0x08, (const uint8_t[]){0x00, 0x01, 0x00}, 3);
    fk_add(0x07, 0x55, 0x00, 0x09, (const uint8_t[]){0x00, 0x02, 0x00}, 3);
    fk.gap[0x07] = 1;                                                           /* slot without a value */
    fk_add(0x51, 0x51, 0x0B, 0x7F, (const uint8_t[]){0x02}, 1);                /* SpeakerVolume */
    fk_add(0x71, 0x56, 0x01, 0x7F, (const uint8_t[]){0x00, 0x78}, 2);          /* MetronomeTempo 120 */
    fk_add(0x72, 0x56, 0x02, 0x7F, (const uint8_t[]){0x04, 0x02}, 2);          /* BeatSelect 4/4 */
    fk_add(0x73, 0x56, 0x09, 0x7F, (const uint8_t[]){0x00}, 1);                /* RhythmSelect */
    fk_add(0x74, 0x56, 0x03, 0x7F, (const uint8_t[]){0x4A}, 1);                /* MetronomeVolume 74 */
    fk_add(0x75, 0x56, 0x0A, 0x7F, (const uint8_t[]){0x00}, 1);                /* MetronomeMode */
    fk_add(0x8B, 0x52, 0x01, 0x7F, (const uint8_t[]){0x7F}, 1);                /* IntSongPart: both */
    fk_add(0x92, 0x52, 0x02, 0x7F, (const uint8_t[]){0x00, 0x00}, 2);          /* MusicTempo: no song */
    fk_add(0x78, 0x51, 0x1A, 0x7F, (const uint8_t[]){0x00, 0x40, 0x00}, 3);    /* UserEqGain, 4 bands */
    fk_add(0x78, 0x51, 0x1A, 0x7F, (const uint8_t[]){0x01, 0x41, 0x00}, 3);
    fk_add(0x78, 0x51, 0x1A, 0x7F, (const uint8_t[]){0x02, 0x42, 0x00}, 3);
    fk_add(0x78, 0x51, 0x1A, 0x7F, (const uint8_t[]){0x03, 0x43, 0x00}, 3);
    fk_add(0x90, 0x55, 0x01, 0x1F, (const uint8_t[]){0x64}, 1);                /* Volume, part 1F: no core slot */
    fk_add(0x30, 0x5A, 0x02, 0x7F, (const uint8_t[]){0x01, 0x02, 0x03, 0x04, 0x05}, 5); /* variable value */
    fk_add(0x31, 0x56, 0x01, 0x7F, (const uint8_t[]){0x00, 0x78}, 2);
    fk.raw_bad[0x31] = true;                                                    /* malformed: no F7 */
    fk.match[0x69] = fk.match[0x76] = fk.match[0x8E] = true;                    /* skipped regardless */
    fk_add(0x69, 0x51, 0x0B, 0x7F, (const uint8_t[]){0x55}, 1);
}

TEST(kst_fill_walks_like_the_stock_dump)
{
    uint8_t v[4];
    fk_stock();
    memset(&store, 0, sizeof store);
    kst_init(&st, &store, &IO);
    CHECK(!st.synced);
    kst_run(&st, KST_FILL);
    CHECK(st.synced);
    CHECK_EQ(st.mode, KST_IDLE);
    /* 0x69 / 0x76 / 0x8E are never asked (FUN_40152A34), unmatched descriptors neither */
    CHECK_EQ(fk.value_calls[0x69], 0);
    CHECK_EQ(fk.value_calls[0x76], 0);
    CHECK_EQ(fk.value_calls[0x8E], 0);
    CHECK_EQ(fk.value_calls[0x00], 0);
    CHECK_EQ(fk.value_calls[0x07], 5);   /* j 0, gap 1, 2, 3, done 4 */
    CHECK_EQ(st.st.frames, 3 + 1 + 7 + 4 + 1 + 1 + 1);
    CHECK_EQ(st.st.unknown, 1);
    CHECK_MEM(st.last_unknown, ((const uint8_t[]){0x55, 0x01, 0x1F}), 3);
    CHECK_EQ(st.st.longval, 1);
    CHECK_EQ(st.st.bad, 1);
    CHECK_EQ(st.st.indexed, 4);
    CHECK_EQ(st.st.stored, 3 + 1 + 7 + 4);
    CHECK_EQ(st.st.diffs, 0);
    CHECK_EQ(st.st.passes, 1);
    /* the System values the app could not read are now in the store */
    CHECK(param_get(&store, PRM_METRONOME_TEMPO, SLOT_SYSTEM, v) && v[0] == 0x00 && v[1] == 0x78);
    CHECK(param_get(&store, PRM_BEAT_SELECT, SLOT_SYSTEM, v) && v[0] == 0x04 && v[1] == 0x02);
    CHECK(param_get(&store, PRM_RHYTHM_SELECT, SLOT_SYSTEM, v) && v[0] == 0x00);
    CHECK(param_get(&store, PRM_METRONOME_VOLUME, SLOT_SYSTEM, v) && v[0] == 0x4A);
    CHECK(param_get(&store, PRM_METRONOME_MODE, SLOT_SYSTEM, v) && v[0] == 0x00);
    CHECK(param_get(&store, PRM_INT_SONG_PART, SLOT_SYSTEM, v) && v[0] == 0x7F);
    CHECK(param_get(&store, PRM_MUSIC_TEMPO, SLOT_SYSTEM, v) && v[0] == 0x00 && v[1] == 0x00);
    CHECK(param_get(&store, PRM_SPEAKER_VOLUME, SLOT_SYSTEM, v) && v[0] == 0x02);
    /* per-part values land in their slots */
    CHECK(param_get(&store, PRM_SOUND, SLOT_MAIN1, v) && v[1] == 0x00);
    CHECK(param_get(&store, PRM_SOUND, SLOT_LAYER, v) && v[1] == 0x01);
    CHECK(param_get(&store, PRM_SOUND, SLOT_LOWER, v) && v[1] == 0x02);
    /* the 4 NG extension values are not the stock's: untouched */
    CHECK(!param_get(&store, PRM_SOUND_PREVIEW_VOLUME, SLOT_SYSTEM, v));
    CHECK(!param_get(&store, PRM_MUSIC_REPEAT, SLOT_SYSTEM, v));
    CHECK(!param_get(&store, PRM_PLAY_MODE, SLOT_SYSTEM, v));
    /* sub-indexed: all 4 bands kept in the side table, not one in the store */
    for (uint8_t b = 0; b < 4; b++) {
        CHECK_EQ(kst_indexed_get(&st, 0x51, 0x1A, 0x7F, b, v), 3);
        CHECK_EQ(v[1], 0x40 + b);
    }
    CHECK_EQ(kst_indexed_get(&st, 0x51, 0x1A, 0x7F, 4, v), 0);
    CHECK(!param_get(&store, PRM_USER_EQ_GAIN, SLOT_SYSTEM, v));
    CHECK(kst_is_indexed(0x54, 0x05, 0x0F) && kst_is_indexed(0x51, 0x1B, 0x7F) && !kst_is_indexed(0x51, 0x0B, 0x7F));
}

TEST(kst_budget_is_resumable)
{
    param_store_t a;
    uint32_t calls = 0;
    fk_stock();
    memset(&a, 0, sizeof a);
    kst_init(&st, &a, &IO);
    kst_run(&st, KST_FILL);
    memset(&store, 0, sizeof store);
    kst_init(&st, &store, &IO);
    kst_start(&st, KST_FILL);
    while (!kst_step(&st, 4)) {
        calls++;
        CHECK(!st.synced);
        CHECK(calls < 100);
    }
    CHECK(st.synced);
    /* every one of the 0xB0 descriptors is matched or skipped; only answered ones count against the budget */
    CHECK(calls >= 3);
    CHECK_MEM(&a, &store, sizeof store);
    CHECK_EQ(st.st.frames, 18);
    CHECK(kst_step(&st, 4));   /* idle: nothing to do */
    CHECK(kst_step_calls(&st, 4));
    /* the cave's bound: at most `calls` value reads per step, same store at the end */
    memset(&store, 0, sizeof store);
    kst_init(&st, &store, &IO);
    kst_start(&st, KST_FILL);
    calls = 0;
    {
        uint32_t frames = 0;
        while (!kst_step_calls(&st, 3)) {
            CHECK(st.st.frames - frames <= 3);
            frames = st.st.frames;
            calls++;
            CHECK(calls < 1000);
        }
    }
    CHECK(st.synced);
    CHECK(calls >= 6);   /* 18 frames, 3 reads per step */
    CHECK_MEM(&a, &store, sizeof store);
    CHECK_EQ(st.st.frames, 18);
}

TEST(kst_compare_counts_and_fixes)
{
    uint8_t v[4];
    fk_stock();
    memset(&store, 0, sizeof store);
    kst_init(&st, &store, &IO);
    kst_run(&st, KST_FILL);
    kst_run(&st, KST_COMPARE);
    CHECK_EQ(st.st.diffs, 0);
    CHECK_EQ(st.diffs_total, 0);
    /* the stock changed two values on its own (panel, StartUp, ...): tempo 100, EQ band 2 */
    fk.data[0x71][0][1] = 0x64;
    fk.data[0x78][2][1] = 0x50;
    kst_start(&st, KST_COMPARE);
    while (!kst_step(&st, 2))
        ;
    CHECK_EQ(st.st.diffs, 2);
    CHECK_EQ(st.diffs_total, 2);
    CHECK_MEM(st.last_diff, ((const uint8_t[]){0x51, 0x1A, 0x7F}), 3);   /* 0x78 comes after 0x71 */
    CHECK(param_get(&store, PRM_METRONOME_TEMPO, SLOT_SYSTEM, v) && v[1] == 0x64);
    CHECK(kst_indexed_get(&st, 0x51, 0x1A, 0x7F, 2, v) == 3 && v[1] == 0x50);
    kst_run(&st, KST_COMPARE);
    CHECK_EQ(st.st.diffs, 0);
    CHECK_EQ(st.diffs_total, 2);
    /* a value missing in the store counts as a difference too (e.g. a core default overwritten later) */
    store.valid[PRM_RHYTHM_SELECT] = 0;
    kst_run(&st, KST_COMPARE);
    CHECK_EQ(st.st.diffs, 1);
    CHECK_MEM(st.last_diff, ((const uint8_t[]){0x56, 0x09, 0x7F}), 3);
    CHECK(st.synced);   /* compare does not clear it */
}

TEST(kst_put_frame_classes)
{
    bool ch;
    const uint8_t ok[] = {0xF0, 0x40, 0x7F, 0x10, 0x04, 0x1E, 0x56, 0x03, 0x7F, 0x30, 0xF7};
    const uint8_t shortv[] = {0xF0, 0x40, 0x7F, 0x10, 0x04, 0x1E, 0x56, 0x01, 0x7F, 0x00, 0xF7};   /* len 2 wanted */
    const uint8_t over[] = {0xF0, 0x40, 0x7F, 0x10, 0x04, 0x1E, 0x56, 0x03, 0x7F, 0x30, 0x01, 0xF7};
    const uint8_t range[] = {0xF0, 0x40, 0x7F, 0x10, 0x04, 0x1E, 0x56, 0x03, 0x7F, 0x80, 0xF7};
    const uint8_t fn03[] = {0xF0, 0x40, 0x7F, 0x03, 0x04, 0x1E, 0x56, 0x03, 0x7F, 0xF7};
    const uint8_t unk[] = {0xF0, 0x40, 0x7F, 0x10, 0x04, 0x1E, 0x7E, 0x7E, 0x7F, 0x00, 0xF7};
    memset(&store, 0, sizeof store);
    CHECK_EQ(kst_put_frame(&store, ok, sizeof ok, &ch), KST_PUT);
    CHECK(ch);
    CHECK_EQ(kst_put_frame(&store, ok, sizeof ok, &ch), KST_SAME);
    CHECK(!ch);
    CHECK_EQ(kst_put_frame(&store, shortv, sizeof shortv, &ch), KST_BAD);
    CHECK_EQ(kst_put_frame(&store, over, sizeof over, &ch), KST_BAD);
    CHECK_EQ(kst_put_frame(&store, range, sizeof range, &ch), KST_BAD);
    CHECK_EQ(kst_put_frame(&store, fn03, sizeof fn03, &ch), KST_BAD);
    CHECK_EQ(kst_put_frame(&store, unk, sizeof unk, &ch), KST_UNKNOWN);
    CHECK_EQ(kst_put_frame(&store, ok, 9, &ch), KST_BAD);
}

/* ---- S2: explained / unexplained differences ---------------------------------------------- */
static struct {
    uint32_t n;
    uint8_t f[KST_FRAME_MAX];
    uint32_t len;
} fan;

static void f_fan(void *c, const uint8_t *f, uint32_t n)
{
    (void)c;
    fan.n++;
    memcpy(fan.f, f, n);
    fan.len = n;
}

static const kst_io_t IO2 = {.ctx = 0, .match = f_match, .value = f_value, .fan = f_fan};

static void s2_ready(void)
{
    fk_stock();
    memset(&store, 0, sizeof store);
    memset(&fan, 0, sizeof fan);
    kst_init(&st, &store, &IO2);
    kst_run(&st, KST_FILL);
    kst_run(&st, KST_COMPARE);
}

TEST(kst_s2_unexplained_is_fixed_ringed_and_fanned)
{
    kst_unexp_t u;
    uint8_t v[4];
    s2_ready();
    CHECK_EQ(st.why[KST_WHY_NONE], 0);
    CHECK_EQ(kst_ring_count(&st), 0);
    CHECK(!kst_ring_get(&st, 0, &u));
    fk.data[0x71][0][1] = 0x64;   /* tempo 120 -> 100, nobody told the core */
    kst_run(&st, KST_COMPARE);
    CHECK_EQ(st.st.diffs, 1);
    CHECK_EQ(st.why[KST_WHY_NONE], 1);
    CHECK_EQ(st.last_why, KST_WHY_NONE);
    CHECK(param_get(&store, PRM_METRONOME_TEMPO, SLOT_SYSTEM, v) && v[1] == 0x64);   /* fixed */
    CHECK_EQ(fan.n, 1);
    CHECK_EQ(st.fanned, 1);
    CHECK_MEM(fan.f, ((const uint8_t[]){0xF0, 0x40, 0x7F, 0x10, 0x04, 0x1E, 0x56, 0x01, 0x7F, 0x00, 0x64, 0xF7}), 12);
    CHECK_EQ(kst_ring_count(&st), 1);
    CHECK(kst_ring_get(&st, 0, &u));
    CHECK_MEM(u.key, ((const uint8_t[]){0x56, 0x01, 0x7F}), 3);
    CHECK_EQ(u.lens, 0x22);
    CHECK_MEM(u.old, ((const uint8_t[]){0x00, 0x78}), 2);
    CHECK_MEM(u.now, ((const uint8_t[]){0x00, 0x64}), 2);
    kst_run(&st, KST_COMPARE);   /* fixed: nothing the next time */
    CHECK_EQ(st.st.diffs, 0);
    CHECK_EQ(fan.n, 1);
    /* a side-table value: old / new with its sub-index byte */
    fk.data[0x78][1][1] = 0x55;
    kst_run(&st, KST_COMPARE);
    CHECK_EQ(st.why[KST_WHY_NONE], 2);
    CHECK(kst_ring_get(&st, 0, &u) && u.key[1] == 0x1A && u.lens == 0x33 && u.old[0] == 1 && u.old[1] == 0x41 &&
          u.now[1] == 0x55);
    CHECK(kst_ring_get(&st, 1, &u) && u.key[1] == 0x01);   /* the older one */
    /* the ring keeps the last KST_RING */
    for (uint8_t t = 0; t < 10; t++) {
        fk.data[0x74][0][0] = (uint8_t)(0x10 + t);
        kst_run(&st, KST_COMPARE);
    }
    CHECK_EQ(st.why[KST_WHY_NONE], 12);
    CHECK_EQ(kst_ring_count(&st), KST_RING);
    CHECK(kst_ring_get(&st, 0, &u) && u.key[1] == 0x03 && u.now[0] == 0x19 && u.old[0] == 0x18);
    CHECK(kst_ring_get(&st, KST_RING - 1, &u) && u.now[0] == 0x12);
    CHECK(!kst_ring_get(&st, KST_RING, &u));
}

TEST(kst_s2_window_and_reasons)
{
    s2_ready();
    /* a set the core applied (side effects): explained, not fanned */
    kst_event(&st, KST_WHY_SET);
    fk.data[0x71][0][1] = 0x64;
    kst_run(&st, KST_COMPARE);
    CHECK_EQ(st.why[KST_WHY_SET], 1);
    CHECK_EQ(st.why[KST_WHY_NONE], 0);
    CHECK_EQ(fan.n, 0);
    /* the next walk: that set is older than the previous walk's start - no longer an explanation */
    fk.data[0x71][0][1] = 0x65;
    kst_run(&st, KST_COMPARE);
    CHECK_EQ(st.why[KST_WHY_NONE], 1);
    CHECK_EQ(fan.n, 1);
    /* an event during the previous walk (after it passed the key) still explains */
    kst_start(&st, KST_COMPARE);
    while (!kst_step_calls(&st, 1))
        if (st.i > 0x71 && !st.last_ev[KST_WHY_POWER])
            kst_event(&st, KST_WHY_POWER);   /* once the walk is past 0x71 */
    fk.data[0x71][0][1] = 0x66;
    kst_run(&st, KST_COMPARE);
    CHECK_EQ(st.why[KST_WHY_POWER], 1);
    /* a notice explains its own key only */
    {
        const uint8_t k[3] = {0x56, 0x03, 0x7F};
        kst_note(&st, k);
    }
    fk.data[0x74][0][0] = 0x20;   /* 56 03 7F: noticed */
    fk.data[0x73][0][0] = 0x01;   /* 56 09 7F: not */
    kst_run(&st, KST_COMPARE);
    CHECK_EQ(st.why[KST_WHY_NOTIFY], 1);
    CHECK_EQ(st.why[KST_WHY_NONE], 2);
    CHECK_MEM(fan.f + 6, ((const uint8_t[]){0x56, 0x09, 0x7F, 0x01}), 4);
    /* keys the stock changes itself (REA): song tempo, song part */
    CHECK(kst_key_stock(0x52, 0x02, 0x7F) && kst_key_stock(0x56, 0x08, 0x7F) && kst_key_stock(0x55, 0x00, 0x09));
    CHECK(!kst_key_stock(0x56, 0x01, 0x7F) && !kst_key_stock(0x55, 0x01, 0x00));
    fk.data[0x92][0][1] = 0x50;
    fk.data[0x8B][0][0] = 0x00;
    kst_run(&st, KST_COMPARE);
    CHECK_EQ(st.why[KST_WHY_KEY], 2);
    CHECK_EQ(st.why[KST_WHY_NONE], 2);
    /* a stock-handled frame / restore / recorder */
    kst_event(&st, KST_WHY_RESTORE);
    fk.data[0x75][0][0] = 0x01;
    kst_run(&st, KST_COMPARE);
    CHECK_EQ(st.why[KST_WHY_RESTORE], 1);
    CHECK_EQ(st.why[KST_WHY_NONE], 2);
    CHECK_EQ(fan.n, 2);
    kst_event(&st, KST_WHY_NONE);   /* not an event */
    kst_event(&st, KST_WHY_N);
    fk.data[0x75][0][0] = 0x00;
    kst_run(&st, KST_COMPARE);
    CHECK_EQ(st.why[KST_WHY_NONE], 3);
}

TEST(kst_s2_abort_keeps_the_window)
{
    s2_ready();
    kst_event(&st, KST_WHY_STOCK);
    fk.data[0x71][0][1] = 0x64;
    kst_start(&st, KST_COMPARE);
    CHECK(!kst_step_calls(&st, 2));   /* stopped early (a pause), before 0x71 */
    kst_abort(&st);
    CHECK_EQ(st.mode, KST_IDLE);
    CHECK_EQ(st.st.passes, 2);     /* not a pass (fill + compare before) */
    kst_abort(&st);                /* idle: nothing */
    kst_run(&st, KST_COMPARE);
    CHECK_EQ(st.why[KST_WHY_STOCK], 1);
    CHECK_EQ(st.why[KST_WHY_NONE], 0);
    /* without the abort the same order loses the explanation (what the window guards) */
    kst_event(&st, KST_WHY_STOCK);
    fk.data[0x71][0][1] = 0x65;
    kst_start(&st, KST_COMPARE);
    CHECK(!kst_step_calls(&st, 2));
    st.mode = KST_IDLE;            /* walk dropped without kst_abort */
    kst_run(&st, KST_COMPARE);
    CHECK_EQ(st.why[KST_WHY_NONE], 1);
}

TEST(kst_s2_notes_overflow_explains_all)
{
    uint8_t k[3] = {0x60, 0x00, 0x7F};
    s2_ready();
    for (uint8_t t = 0; t < KST_NOTES; t++) {
        k[1] = t;
        kst_note(&st, k);
    }
    k[1] = 0;
    kst_note(&st, k);   /* the same key again: refreshed in place, nothing pushed out */
    fk.data[0x73][0][0] = 0x02;
    kst_run(&st, KST_COMPARE);
    CHECK_EQ(st.why[KST_WHY_NONE], 1);
    for (uint8_t t = 0; t <= KST_NOTES; t++) {   /* one more than the table: a live notice is pushed out */
        k[1] = (uint8_t)(0x20 + t);
        kst_note(&st, k);
    }
    fk.data[0x73][0][0] = 0x03;
    kst_run(&st, KST_COMPARE);
    CHECK_EQ(st.why[KST_WHY_NOTIFY], 1);
    CHECK_EQ(st.why[KST_WHY_NONE], 1);
}

/* ---- S3: read owner ----------------------------------------------------------------------------- */
static void s3_ready(void)
{
    fk_stock();
    fk_add(0x5C, 0x58, 0x01, 0x7F, (const uint8_t[]){0x01}, 1);   /* Local Control: a settings byte, but a KEY key */
    memset(&store, 0, sizeof store);
    memset(&fan, 0, sizeof fan);
    kst_init(&st, &store, &IO2);
}

static void same_as_stock(uint8_t i, uint8_t j, const uint8_t key[3])
{
    uint8_t f[KST_FRAME_MAX], want[KST_FRAME_MAX];
    int n = kst_s3_frame(&st, i, j, 0x7F, key, f), w = f_value(0, 3, 0x7F, i, 0x7F, j, want);
    CHECK(n > 0);
    CHECK_EQ(n, w);
    if (n > 0 && n == w)
        CHECK_MEM(f, want, (size_t)n);
}

TEST(kst_s3_answers_from_the_store_when_trusted)
{
    static const uint8_t tempo[3] = {0x56, 0x01, 0x7F}, spk[3] = {0x51, 0x0B, 0x7F}, eq[3] = {0x51, 0x1A, 0x7F},
                         snd[3] = {0x55, 0x00, 0x08}, song[3] = {0x52, 0x02, 0x7F}, local[3] = {0x58, 0x01, 0x7F},
                         vol1f[3] = {0x55, 0x01, 0x1F}, vol[3] = {0x56, 0x03, 0x7F}, hp[3] = {0x51, 0x29, 0x7F};
    uint8_t f[KST_FRAME_MAX];
    uint32_t ev;
    s3_ready();
    CHECK_EQ(kst_s3_frame(&st, 0x71, 0, 0x7F, tempo, f), 0);   /* off: nothing counted */
    CHECK_EQ(st.s3.untrusted + st.s3.served + st.s3.stock, 0);
    kst_s3_enable(&st, true);
    CHECK_EQ(kst_s3_frame(&st, 0x71, 0, 0x7F, tempo, f), 0);   /* not filled yet */
    CHECK_EQ(st.s3.untrusted, 1);
    kst_run(&st, KST_FILL);
    CHECK(kst_s3_trusted(&st));
    same_as_stock(0x71, 0, tempo);
    same_as_stock(0x51, 0, spk);
    for (uint8_t j = 0; j < 4; j++)   /* side table: by where the walk read it */
        same_as_stock(0x78, j, eq);
    CHECK_EQ(st.s3.served, 6);
    /* the request's device byte goes into the frame */
    CHECK_EQ(kst_s3_frame(&st, 0x71, 0, 0x10, tempo, f), 12);
    CHECK_MEM(f, ((const uint8_t[]){0xF0, 0x40, 0x10, 0x10, 0x04, 0x1E, 0x56, 0x01, 0x7F, 0x00, 0x78, 0xF7}), 12);
    /* not the store's: run-state descriptors, keys the stock changes itself */
    CHECK_EQ(kst_s3_frame(&st, 0x07, 1, 0x7F, snd, f), 0);
    CHECK_EQ(kst_s3_frame(&st, 0x92, 0, 0x7F, song, f), 0);
    CHECK_EQ(kst_s3_frame(&st, 0x5C, 0, 0x7F, local, f), 0);
    CHECK_EQ(st.s3.stock, 3);
    /* the store's, but no such value: unknown key, a j the walk never read, a key not read for this descriptor */
    CHECK_EQ(kst_s3_frame(&st, 0x90, 0, 0x7F, vol1f, f), 0);
    CHECK_EQ(kst_s3_frame(&st, 0x78, 4, 0x7F, eq, f), 0);
    CHECK_EQ(kst_s3_frame(&st, 0x72, 0, 0x7F, (const uint8_t[]){0x56, 0x0B, 0x7F}, f), 0);
    CHECK_EQ(st.s3.miss, 3);
    /* a store-relevant event: the stock answers until a whole walk ran after it */
    kst_event(&st, KST_WHY_SET);
    CHECK(!kst_s3_trusted(&st));
    CHECK_EQ(kst_s3_frame(&st, 0x71, 0, 0x7F, tempo, f), 0);
    CHECK_EQ(st.s3.untrusted, 2);
    kst_run(&st, KST_COMPARE);
    CHECK(kst_s3_trusted(&st));
    /* run-state-only events and notices of the stock's own keys keep it */
    ev = st.ev;
    kst_event_run(&st, KST_WHY_STOCK);
    CHECK_EQ(st.ev, ev + 1);
    CHECK_EQ(st.last_ev[KST_WHY_STOCK], ev + 1);   /* still an S2 explanation */
    kst_note(&st, hp);
    CHECK(kst_s3_trusted(&st));
    kst_note(&st, vol);                             /* a notice of a settings key */
    CHECK(!kst_s3_trusted(&st));
    kst_run(&st, KST_COMPARE);
    CHECK(kst_s3_trusted(&st));
    /* an event during a walk: that walk is not clean, the next one is */
    kst_start(&st, KST_COMPARE);
    CHECK(!kst_step_calls(&st, 2));
    kst_event(&st, KST_WHY_POWER);
    while (!kst_step_calls(&st, 4))
        ;
    CHECK(!kst_s3_trusted(&st));
    /* an aborted walk is not clean either */
    kst_start(&st, KST_COMPARE);
    CHECK(!kst_step_calls(&st, 2));
    kst_abort(&st);
    CHECK(!kst_s3_trusted(&st));
    kst_run(&st, KST_COMPARE);
    CHECK(kst_s3_trusted(&st));
    same_as_stock(0x74, 0, vol);
    /* the ownership table (REA FUN_4015E960): settings block / global reads in, run-state calls out */
    CHECK(kst_s3_owned(0x01) && kst_s3_owned(0x15) && kst_s3_owned(0x37) && kst_s3_owned(0x57) && kst_s3_owned(0x72));
    CHECK(kst_s3_owned(0x78) && kst_s3_owned(0x90) && kst_s3_owned(0xAF));
    CHECK(!kst_s3_owned(0x00) && !kst_s3_owned(0x07) && !kst_s3_owned(0x58) && !kst_s3_owned(0x70));
    CHECK(!kst_s3_owned(0x79) && !kst_s3_owned(0x8B) && !kst_s3_owned(0x92) && !kst_s3_owned(0xA6));
    kst_s3_enable(&st, false);
    CHECK_EQ(kst_s3_frame(&st, 0x71, 0, 0x7F, tempo, f), 0);
}

TEST(kst_s3_silent_keys_fanned_and_stale_counted)
{
    s3_ready();
    kst_run(&st, KST_FILL);
    /* MIDI Program Change (KEY 55 00 <part>): S3 off -> explained, not fanned (S2) */
    fk.data[0x07][1][1] = 0x05;
    kst_run(&st, KST_COMPARE);
    CHECK_EQ(st.why[KST_WHY_KEY], 1);
    CHECK_EQ(fan.n, 0);
    /* S3 on: fanned to every controller, still explained */
    kst_s3_enable(&st, true);
    fk.data[0x07][1][1] = 0x06;
    kst_run(&st, KST_COMPARE);
    CHECK_EQ(st.why[KST_WHY_KEY], 2);
    CHECK_EQ(st.why[KST_WHY_NONE], 0);
    CHECK_EQ(fan.n, 1);
    CHECK_EQ(st.s3.fanned_key, 1);
    CHECK_MEM(fan.f + 6, ((const uint8_t[]){0x55, 0x00, 0x08, 0x00, 0x06, 0x00, 0xF7}), 7);
    fk.data[0x5C][0][0] = 0x00;   /* CC 0x7A Local Control off: silent too */
    kst_run(&st, KST_COMPARE);
    CHECK_EQ(fan.n, 2);
    CHECK_MEM(fan.f + 6, ((const uint8_t[]){0x58, 0x01, 0x7F, 0x00}), 4);
    /* a KEY the stock notifies or another module tells (song tempo): not fanned */
    fk.data[0x92][0][1] = 0x50;
    kst_run(&st, KST_COMPARE);
    CHECK_EQ(st.why[KST_WHY_KEY], 4);
    CHECK_EQ(fan.n, 2);
    CHECK(kst_key_silent(0x55, 0x00, 0x0B) && kst_key_silent(0x58, 0x01, 0x7F));
    CHECK(!kst_key_silent(0x52, 0x02, 0x7F) && !kst_key_silent(0x56, 0x08, 0x7F) && !kst_key_silent(0x51, 0x29, 0x7F));
    /* a hidden change of a settings value while trusted: unexplained and counted stale (S3 may have served it) */
    CHECK(kst_s3_trusted(&st));
    fk.data[0x74][0][0] = 0x11;
    kst_run(&st, KST_COMPARE);
    CHECK_EQ(st.why[KST_WHY_NONE], 1);
    CHECK_EQ(st.s3.stale, 1);
    /* after an event it is explained, not stale */
    kst_event(&st, KST_WHY_SET);
    fk.data[0x74][0][0] = 0x12;
    kst_run(&st, KST_COMPARE);
    CHECK_EQ(st.why[KST_WHY_SET], 1);
    CHECK_EQ(st.s3.stale, 1);
}

/* ---- S4: write owner ---------------------------------------------------- */
static uint32_t frame(const uint8_t *body, uint32_t n, uint8_t out[300])
{
    out[0] = 0xF0; out[1] = 0x40; out[2] = 0x00; out[3] = 0x10; out[4] = 0x08; out[5] = 0x02;
    memcpy(out + 6, body, n);
    out[6 + n] = 0xF7;
    return n + 7;
}

TEST(kst_s4_gate_and_frame)
{
    static const uint8_t spk[] = {0x51, 0x0B, 0x7F, 0x10}, spk_long[] = {0x51, 0x0B, 0x7F, 0x10, 0x10},
                         unk[] = {0x51, 0x7E, 0x7F, 0x00}, eq[] = {0x51, 0x1A, 0x7F, 0x01, 0x41, 0x00},
                         high[] = {0x51, 0x0B, 0x7F, 0x80},
                         packed[] = {0x5F, 0x00, 0x7F, 0x05, 0x51, 0x0B, 0x7F, 0x10,   /* speaker volume: own        */
                                     0x05, 0x56, 0x01, 0x7F, 0x00,                       /* tempo one byte short       */
                                     0x05, 0x51, 0x7E, 0x7F, 0x00,                       /* unknown key: passed        */
                                     0x06, 0x56, 0x01, 0x7F, 0x00, 0x64},                /* tempo 100: own             */
                         packed_ok[] = {0x5F, 0x00, 0x7F, 0x05, 0x51, 0x0B, 0x7F, 0x10, 0x06, 0x56, 0x01, 0x7F, 0x00, 0x64},
                         packed_bad[] = {0x5F, 0x00, 0x7F, 0x04, 0x51, 0x0B, 0x7F, 0x05, 0x56, 0x01, 0x7F, 0x00};
    uint8_t m[300], out[300];
    uint32_t n;
    kst_s4_set_t r;
    memset(&store, 0, sizeof store);
    kst_init(&st, &store, &IO);
    /* a valid single set: the set itself, one key to read back */
    n = frame(spk, sizeof spk, m);
    CHECK_EQ(kst_s4_judge(&st, m, n, out, &r), n);
    CHECK_MEM(out, m, n);
    CHECK_EQ(r.nown, 1);
    CHECK_EQ(r.n, 1);
    CHECK(r.own[0] && r.pure);
    CHECK_MEM(r.key[0], spk, 3);
    /* wrong length, a byte above 7F: refused, nothing to apply - still read back (the mirror may hold it) */
    n = frame(spk_long, sizeof spk_long, m);
    CHECK_EQ(kst_s4_judge(&st, m, n, out, &r), 0);
    CHECK_EQ(r.refused, 1);
    CHECK_EQ(r.n, 1);
    CHECK(!r.own[0]);
    n = frame(high, sizeof high, m);
    CHECK_EQ(kst_s4_judge(&st, m, n, out, &r), 0);
    CHECK_EQ(st.s4.dropped, 2);
    CHECK_MEM(st.s4_last_refused, spk, 3);
    /* a key the core does not know: the stock's, applied as it came */
    n = frame(unk, sizeof unk, m);
    CHECK_EQ(kst_s4_judge(&st, m, n, out, &r), n);
    CHECK_EQ(r.passed, 1);
    CHECK_EQ(r.n, 0);
    /* a sub-indexed key: own, but no single value to read back - not pure */
    n = frame(eq, sizeof eq, m);
    CHECK_EQ(kst_s4_judge(&st, m, n, out, &r), n);
    CHECK_EQ(r.nown, 1);
    CHECK_EQ(r.unlisted, 1);
    CHECK(!r.pure);
    /* packed: the refused entry cut out, the rest in order, F7 at the end */
    n = frame(packed, sizeof packed, m);
    {
        uint8_t want[300];
        uint32_t wn = frame(packed_ok, sizeof packed_ok, want);
        uint32_t got = kst_s4_judge(&st, m, n, out, &r);
        uint8_t kept[300];
        memcpy(kept, want, wn);
        /* + the passed unknown entry between them */
        memcpy(kept + 14, (const uint8_t[]){0x05, 0x51, 0x7E, 0x7F, 0x00, 0x06, 0x56, 0x01, 0x7F, 0x00, 0x64, 0xF7}, 12);
        CHECK_EQ(got, wn + 5);
        CHECK_MEM(out, kept, wn + 5);
    }
    CHECK_EQ(r.nown, 2);
    CHECK_EQ(r.refused, 1);
    CHECK_EQ(r.passed, 1);
    CHECK_EQ(r.n, 2);   /* speaker volume, tempo (listed once, own) */
    CHECK(r.own[0] && r.own[1]);
    /* packed, nothing refused: byte for byte the set */
    n = frame(packed_ok, sizeof packed_ok, m);
    CHECK_EQ(kst_s4_judge(&st, m, n, out, &r), n);
    CHECK_MEM(out, m, n);
    /* packed, every value refused: nothing */
    n = frame(packed_bad, sizeof packed_bad, m);
    CHECK_EQ(kst_s4_judge(&st, m, n, out, &r), 0);
    CHECK_EQ(r.refused, 2);
    CHECK_EQ(st.s4.sets, 8);
    /* the pure table (emulator-measured): speaker volume, tempo in; TouchCurve (part 00 -> 08..0B) and songs out */
    CHECK(kst_s4_pure(0x51, 0x0B, 0x7F) && kst_s4_pure(0x56, 0x01, 0x7F) && kst_s4_pure(0x50, 0x01, 0x00));
    CHECK(!kst_s4_pure(0x50, 0x00, 0x00) && !kst_s4_pure(0x52, 0x1E, 0x7F) && !kst_s4_pure(0x55, 0x00, 0x00));
    CHECK(!kst_s4_pure(0x50, 0x01, 0x7F) && kst_s4_pure_count() > 100);
}

/* one set the way the cave runs it: judge, the front's mirror, the fake stock applies (stock_val, or keeps its value when
 * refuses), read-back, done */
static void s4_set(const uint8_t *body, uint32_t blen, uint8_t i, uint8_t stock_val)
{
    uint8_t m[300], out[300], f[KST_FRAME_MAX];
    kst_s4_set_t r;
    uint32_t n = frame(body, blen, m);
    if (kst_s4_judge(&st, m, n, out, &r)) {
        int id = param_find(0x10, body[0], body[1], body[2]);
        param_put(&store, (param_id_t)id, param_slot_from_part(body[2]), body + 3, (uint8_t)(blen - 3));   /* mirror */
        fk.data[i][0][0] = stock_val;
    }
    for (uint32_t k = 0; k < r.n; k++) {
        int fn = f_value(0, 3, 0x7F, i, 0x7F, 0, f);
        kst_s4_follow(&st, &r, k, f, (uint32_t)fn);
    }
    kst_s4_done(&st, &r);
}

TEST(kst_s4_read_back_and_trust)
{
    static const uint8_t spk10[] = {0x51, 0x0B, 0x7F, 0x10}, spk20[] = {0x51, 0x0B, 0x7F, 0x20},
                         tc[] = {0x56, 0x03, 0x7F, 0x30};
    uint8_t v[4];
    uint32_t gen;
    s3_ready();
    kst_run(&st, KST_FILL);
    kst_s3_enable(&st, true);
    gen = st.gen;
    kst_s4_enable(&st, true);
    CHECK_EQ(st.gen, gen + 1);   /* switching drops the trust: a clean walk first */
    CHECK(!kst_s3_trusted(&st));
    kst_run(&st, KST_COMPARE);
    CHECK(kst_s3_trusted(&st));
    /* a pure key, followed: the store keeps the trust (S3 answers at once), S2 still has the SET event */
    gen = st.gen;
    s4_set(spk10, sizeof spk10, 0x51, 0x10);
    CHECK_EQ(st.s4.followed, 1);
    CHECK_EQ(st.s4.kept, 1);
    CHECK_EQ(st.gen, gen);
    CHECK(kst_s3_trusted(&st));
    CHECK(st.last_ev[KST_WHY_SET] == st.ev);
    kst_run(&st, KST_COMPARE);
    CHECK_EQ(st.st.diffs, 0);
    /* the stock did not take the value (its case refused it): write miss, the store takes the stock's, S3 rule */
    s4_set(spk20, sizeof spk20, 0x51, 0x10);
    CHECK_EQ(st.s4.wmiss, 1);
    CHECK_MEM(st.s4_last_miss, spk20, 3);
    CHECK(param_get(&store, (param_id_t)param_find(0x10, 0x51, 0x0B, 0x7F), param_slot_from_part(0x7F), v));
    CHECK_EQ(v[0], 0x10);
    CHECK(!kst_s3_trusted(&st));
    kst_run(&st, KST_COMPARE);
    CHECK(kst_s3_trusted(&st));
    /* a value the gate refuses (one byte long) that the front's mirror took anyway: the read-back gives the store the
     * stock's value back; nothing applied, so no event - still trusted */
    {
        static const uint8_t spk_long[] = {0x51, 0x0B, 0x7F, 0x30, 0x30};
        uint8_t m[300], out[300], f[KST_FRAME_MAX];
        kst_s4_set_t r;
        uint32_t n = frame(spk_long, sizeof spk_long, m), ev = st.ev;
        int fn;
        CHECK_EQ(kst_s4_judge(&st, m, n, out, &r), 0);
        param_put(&store, (param_id_t)param_find(0x10, 0x51, 0x0B, 0x7F), param_slot_from_part(0x7F), spk_long + 3, 2);
        fn = f_value(0, 3, 0x7F, 0x51, 0x7F, 0, f);
        kst_s4_follow(&st, &r, 0, f, (uint32_t)fn);
        kst_s4_done(&st, &r);
        CHECK(param_get(&store, (param_id_t)param_find(0x10, 0x51, 0x0B, 0x7F), param_slot_from_part(0x7F), v));
        CHECK_EQ(v[0], 0x10);
        CHECK_EQ(st.ev, ev);
        CHECK(kst_s3_trusted(&st));
        CHECK_EQ(st.s4.wmiss, 1);
    }
    /* a kept set whose apply changed another settings value after all: the compare finds it - side, trust dropped */
    s4_set(tc, sizeof tc, 0x74, 0x30);
    CHECK_EQ(st.s4.kept, 2);
    fk.data[0x71][0][1] = 0x50;   /* tempo moved by the metronome volume apply */
    kst_run(&st, KST_COMPARE);
    CHECK_EQ(st.s4.side, 1);
    CHECK_MEM(st.s4_last_side, ((const uint8_t[]){0x56, 0x01, 0x7F}), 3);
    CHECK(!kst_s3_trusted(&st));
    CHECK_EQ(st.s3.stale, 0);   /* explained (SET), so not the S3 stale count */
    kst_run(&st, KST_COMPARE);
    CHECK(kst_s3_trusted(&st));
    /* S4 off: a set is the S3 rule again; side is not counted */
    kst_s4_enable(&st, false);
    kst_run(&st, KST_COMPARE);
    kst_event(&st, KST_WHY_SET);
    fk.data[0x71][0][1] = 0x51;
    kst_run(&st, KST_COMPARE);
    CHECK_EQ(st.s4.side, 1);
}

void suite_kpstate(void);
void suite_kpstate(void)
{
    printf("settings state S1 (Phase 3-4)\n");
    RUN(kst_fill_walks_like_the_stock_dump);
    RUN(kst_budget_is_resumable);
    RUN(kst_compare_counts_and_fixes);
    RUN(kst_put_frame_classes);
    printf("settings state S2 (Phase 3-4)\n");
    RUN(kst_s2_unexplained_is_fixed_ringed_and_fanned);
    RUN(kst_s2_window_and_reasons);
    RUN(kst_s2_abort_keeps_the_window);
    RUN(kst_s2_notes_overflow_explains_all);
    printf("settings state S3 (Phase 3-4)\n");
    RUN(kst_s3_answers_from_the_store_when_trusted);
    RUN(kst_s3_silent_keys_fanned_and_stale_counted);
    printf("settings state S4 (Phase 3-4)\n");
    RUN(kst_s4_gate_and_frame);
    RUN(kst_s4_read_back_and_trust);
}
