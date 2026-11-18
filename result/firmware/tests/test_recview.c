/* Phase 3-6 R0: the read-only recorder view (src/recorder/recview.c) over a fake stock memory. */
#include <stdlib.h>

#include "ca79ng/recview.h"
#include "test.h"

/* ---- fake stock memory: the regions recview may read, everything else is a test failure ---- */
static struct region {
    uint32_t base, size;
    uint8_t *mem;
} R[] = {
    {0x40500000u, 0x100000u, 0},  /* RecorderData mirror */
    {0x4191E000u, 0x1100u, 0},    /* recorder / player state */
    {0x41915600u, 0x120u, 0},     /* MusicMode, tempo */
    {0x4139DA00u, 0x100u, 0},     /* NOR job table, worker flag */
    {0x413CA300u, 0x100u, 0},     /* file executor */
};
#define NR (sizeof R / sizeof R[0])
static int stray_reads;

static uint8_t *at(uint32_t a, uint32_t n)
{
    for (unsigned i = 0; i < NR; i++)
        if (a >= R[i].base && a - R[i].base + n <= R[i].size)
            return R[i].mem + (a - R[i].base);
    return 0;
}

static void fake_read(void *ctx, uint32_t a, void *dst, uint32_t n)
{
    (void)ctx;
    uint8_t *p = at(a, n);
    if (!p) {
        stray_reads++;
        memset(dst, 0xEE, n);
        return;
    }
    memcpy(dst, p, n);
}

static const rv_mem_t MEM = {fake_read, 0};

static void w8(uint32_t a, uint8_t v) { *at(a, 1) = v; }
static void w16(uint32_t a, uint16_t v) { uint8_t *p = at(a, 2); p[0] = (uint8_t)v, p[1] = (uint8_t)(v >> 8); }
static void w32(uint32_t a, uint32_t v) { w16(a, (uint16_t)v), w16(a + 2, (uint16_t)(v >> 16)); }
static uint32_t r32(uint32_t a) { uint8_t *p = at(a, 4); return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24; }

static void mem_reset(void)
{
    for (unsigned i = 0; i < NR; i++) {
        free(R[i].mem);
        R[i].mem = calloc(1, R[i].size);
    }
    stray_reads = 0;
}

/* ---- a RecorderData image in the stock format (same builder as
 * result/stockfw/tools/stock_songs.py: per bar FA 23 + 4 quarter notes, FC) ---- */
static uint32_t song_bytes(uint8_t *h, unsigned i, unsigned p1, unsigned p2, bool soundmode, uint32_t pad_to)
{
    memset(h, 0, 0x74);
    char name[33];
    snprintf(name, sizeof name, "SONG %02u%-25s", i + 1, soundmode ? " - SoundMode" : "");
    memcpy(h, name, 32);
    h[0x28] = 0x10;
    if (!p1 && !p2)
        return 0x74;
    uint32_t n = 0x74;
    static const uint8_t blk[] = {0x00, 0x00, 0x00, 0x78, 0xFC, 0, 0, 0}; /* the dump's start block shape */
    memcpy(h + n, blk, sizeof blk);
    h[0x24] = 0x74;
    n += sizeof blk;
    unsigned parts[2] = {p1, p2};
    for (unsigned k = 0; k < 2; k++) {
        if (!parts[k])
            continue;
        h[0x2C + 4 * k] = (uint8_t)n, h[0x2D + 4 * k] = (uint8_t)(n >> 8), h[0x2E + 4 * k] = (uint8_t)(n >> 16);
        for (unsigned b = 0; b < parts[k]; b++) {
            h[n++] = 0xFA, h[n++] = 0x23;
            for (unsigned q = 0; q < 4; q++)
                h[n++] = (uint8_t)(60 + q), h[n++] = 0, h[n++] = 0x40, h[n++] = 0x30, h[n++] = 0xF8;
        }
        h[n++] = 0xFC;
    }
    while (n < pad_to)
        h[n++] = 0;
    while (n & 3)
        h[n++] = 0;
    h[0x6C] = (uint8_t)n, h[0x6D] = (uint8_t)(n >> 8), h[0x6E] = (uint8_t)(n >> 16);
    unsigned mb = p1 > p2 ? p1 : p2;
    h[0x70] = (uint8_t)mb, h[0x71] = (uint8_t)(mb >> 8);
    return n;
}

typedef struct {
    unsigned p1, p2;
    bool sm;
    uint32_t pad;
} lay_t;

static uint32_t OFF[12];

static void store_build(const lay_t *lay, unsigned n)
{
    static uint8_t buf[0x10000];
    uint32_t cur = 0x30;
    for (unsigned i = 0; i < RV_SONGS; i++) {
        lay_t l = i < n ? lay[i] : (lay_t){0, 0, false, 0};
        uint32_t len = song_bytes(buf, i, l.p1, l.p2, l.sm, l.pad);
        memcpy(at(RV_A_STORE + cur, len), buf, len);
        OFF[i] = cur;
        cur += len;
    }
    OFF[10] = cur, OFF[11] = RV_STORE_LIMIT;
    for (unsigned i = 0; i < 12; i++)
        w32(RV_A_STORE + 4 * i, OFF[i]);
    w16(RV_A_STORE + cur, 0xA55A);
    w16(RV_A_STORE + RV_STORE_LIMIT, 0xA55A);
}

static const lay_t LAYOUT[] = {{3, 0, true, 0}, {2, 5, false, 0}, {0, 4, false, 0}, {200, 0, false, 0}};

TEST(recview_transport_map)
{
    static const uint8_t want[9] = {RV_T_IDLE, RV_T_PLAYING, RV_T_IDLE, RV_T_REC_ARMED, RV_T_RECORDING,
                                    RV_T_REC_ARMED, RV_T_COUNT_IN, RV_T_UNKNOWN, RV_T_UNKNOWN};
    for (unsigned s = 0; s < 9; s++)
        CHECK_EQ(rv_transport((uint8_t)s), want[s]);
    CHECK_EQ(rv_transport(0xF0), RV_T_UNKNOWN);
}

TEST(recview_live_fields)
{
    mem_reset();
    store_build(LAYOUT, 4);
    w8(RV_A_STATE, 1), w8(RV_A_SONG, 3), w8(RV_A_REC_PART, 1), w8(RV_A_PLAY_SEL, 2);
    w8(RV_A_MUSIC_MODE, 2), w16(RV_A_TEMPO, 0x1234), w16(RV_A_TEMPO_ORIG, 120);
    w32(RV_A_POS, 0x00012345), w16(RV_A_MAX_BARS, 200), w8(RV_A_MAX_SLOT, 3);
    w16(RV_A_TRACKS + 3 * RV_TRACK_SIZE + RV_TRACK_BAR, 17), w16(RV_A_TRACKS + RV_TRACK_BAR, 99);
    w16(RV_A_RING_COUNT, 0x0FFF);
    w16(RV_A_BAR, 21), w8(RV_A_MQ_COUNT, 5), w8(RV_A_MQ_HOLD, 0x82);
    rv_view_t v;
    rv_read(&MEM, &v);
    CHECK_EQ(stray_reads, 0);
    CHECK_EQ(v.state, 1);
    CHECK_EQ(v.transport, RV_T_PLAYING);
    CHECK_EQ(v.song, 3);
    CHECK_EQ(v.rec_part, 1);
    CHECK_EQ(v.play_sel, 2);
    CHECK_EQ(v.music_mode, 2);
    CHECK_EQ(v.tempo, 0x1234);
    CHECK_EQ(v.tempo_orig, 120);
    CHECK_EQ(v.pos, 0x12345);
    CHECK_EQ(v.max_bars, 200);
    CHECK_EQ(v.max_slot, 3);
    CHECK_EQ(v.cur_bar, 17); /* the max slot's track, not slot 0 */
    CHECK_EQ(v.ring, 0x0FFF);
    CHECK_EQ(v.bar, 21); /* R1: progress bar, queue count, deferred mode */
    CHECK_EQ(v.mq_count, 5);
    CHECK_EQ(v.mq_hold, 0x82);
    CHECK_EQ(v.busy, 0);
    /* no track loaded: max slot 0xFF -> bar 0, and no read past the 16 track states */
    w8(RV_A_MAX_SLOT, 0xFF);
    rv_read_live(&MEM, &v);
    CHECK_EQ(v.cur_bar, 0);
    CHECK_EQ(stray_reads, 0);
}

TEST(recview_busy_bits)
{
    mem_reset();
    store_build(LAYOUT, 4);
    rv_view_t v;
    w8(RV_A_STATE, 2);
    rv_read_live(&MEM, &v);
    CHECK_EQ(v.busy, 0);
    w8(RV_A_JOB0 + 8, 1); /* slot 1 = RecorderData job pending (FUN_401342AC reads both) */
    rv_read_live(&MEM, &v);
    CHECK_EQ(v.busy, RV_BUSY_JOB);
    w8(RV_A_JOB1, 0), w8(RV_A_JOB0, 1), w8(RV_A_WORKER, 1), w8(RV_A_FILEOP, 9);
    rv_read_live(&MEM, &v);
    CHECK_EQ(v.busy, RV_BUSY_JOB | RV_BUSY_WORKER | RV_BUSY_FILEOP);
    mem_reset();
    store_build(LAYOUT, 4);
    for (uint8_t s = 3; s <= 6; s++) {
        w8(RV_A_STATE, s);
        rv_read_live(&MEM, &v);
        CHECK_EQ(v.busy, RV_BUSY_REC);
    }
}

TEST(recview_song_table)
{
    mem_reset();
    store_build(LAYOUT, 4);
    rv_view_t v;
    rv_read_store(&MEM, &v);
    CHECK_EQ(stray_reads, 0);
    CHECK_EQ(v.store, RV_STORE_OK);
    CHECK_EQ(v.store_end, OFF[10]);
    CHECK_EQ(v.songs[0].bars, 3);
    CHECK_EQ(v.songs[0].flags, RV_SONG_PART1 | RV_SONG_SETTINGS | RV_SONG_SOUNDMODE);
    CHECK_EQ(v.songs[1].bars, 5);
    CHECK_EQ(v.songs[1].flags, RV_SONG_PART1 | RV_SONG_PART2 | RV_SONG_SETTINGS);
    CHECK_EQ(v.songs[2].flags, RV_SONG_PART2 | RV_SONG_SETTINGS);
    CHECK_EQ(v.songs[3].bars, 200);
    for (unsigned i = 4; i < RV_SONGS; i++) {
        CHECK_EQ(v.songs[i].len, 0x74);
        CHECK_EQ(v.songs[i].bars, 0);
        CHECK_EQ(v.songs[i].flags, 0);
    }
    for (unsigned i = 0; i < RV_SONGS; i++)
        CHECK_EQ(v.songs[i].len, OFF[i + 1] - OFF[i]);
    /* a track in slot 5 (only KSO import fills 2-15) */
    uint32_t s = RV_A_STORE + OFF[1];
    w32(s + 0x2C + 4 * 5, r32(s + 0x2C));
    rv_read_store(&MEM, &v);
    CHECK_EQ(v.store, RV_STORE_OK);
    CHECK(v.songs[1].flags & RV_SONG_EXTRA);
}

/* the device's own store has this shape (NOR dump 0x60500000, read 2026-11-01): song 1 0x7FF0 B /
 * 199 bars / part 1 at +0x7C / start block at +0x74 / "- SoundMode", song 2 0xFA8 B / 13 bars,
 * songs 3-10 empty 0x74 B, end 0x9368.  Synthetic content, same structure. */
TEST(recview_dump_shape)
{
    mem_reset();
    static const lay_t dump[] = {{199, 0, true, 0x7FF0}, {13, 0, true, 0xFA8}};
    store_build(dump, 2);
    CHECK_EQ(OFF[1], 0x8020);
    CHECK_EQ(OFF[2], 0x8FC8);
    CHECK_EQ(OFF[10], 0x9368);
    CHECK_EQ(r32(RV_A_STORE + 0x30 + 0x2C), 0x7C);
    rv_view_t v;
    rv_read_store(&MEM, &v);
    CHECK_EQ(v.store, RV_STORE_OK);
    CHECK_EQ(v.songs[0].len, 0x7FF0);
    CHECK_EQ(v.songs[0].bars, 199);
    CHECK_EQ(v.songs[1].len, 0xFA8);
    CHECK_EQ(v.songs[1].bars, 13);
    CHECK_EQ(v.songs[0].flags, RV_SONG_PART1 | RV_SONG_SETTINGS | RV_SONG_SOUNDMODE);
    CHECK_EQ(v.songs[2].flags, 0);
}

TEST(recview_store_checks)
{
    rv_view_t v;
    /* blank */
    mem_reset();
    store_build(LAYOUT, 4);
    w32(RV_A_STORE, 0xFFFFFFFFu);
    rv_read_store(&MEM, &v);
    CHECK_EQ(v.store, RV_STORE_BLANK);
    /* song start below 0x30 / not aligned */
    mem_reset();
    store_build(LAYOUT, 4);
    w32(RV_A_STORE + 4 * 3, 0x10);
    rv_read_store(&MEM, &v);
    CHECK_EQ(v.store, RV_STORE_SONG_RANGE);
    CHECK_EQ(v.store_bad, 3);
    mem_reset();
    store_build(LAYOUT, 4);
    w32(RV_A_STORE + 4 * 5, OFF[5] + 2);
    rv_read_store(&MEM, &v);
    CHECK_EQ(v.store, RV_STORE_SONG_ALIGN);
    CHECK_EQ(v.store_bad, 5);
    /* track outside / not FA */
    mem_reset();
    store_build(LAYOUT, 4);
    w32(RV_A_STORE + OFF[1] + 0x2C + 4, 0x00200000u);
    rv_read_store(&MEM, &v);
    CHECK_EQ(v.store, RV_STORE_TRACK_RANGE);
    CHECK_EQ(v.store_bad, 1 * 16 + 1);
    mem_reset();
    store_build(LAYOUT, 4);
    w8(RV_A_STORE + OFF[0] + r32(RV_A_STORE + OFF[0] + 0x2C), 0xF8);
    rv_read_store(&MEM, &v);
    CHECK_EQ(v.store, RV_STORE_TRACK_FA);
    CHECK_EQ(v.store_bad, 0);
    /* passes the boot check but not ours: descending starts, end word, markers */
    mem_reset();
    store_build(LAYOUT, 4);
    w32(RV_A_STORE + 4 * 7, OFF[9]);
    rv_read_store(&MEM, &v);
    CHECK_EQ(v.store, RV_STORE_ORDER);
    mem_reset();
    store_build(LAYOUT, 4);
    w32(RV_A_STORE + 0x2C, 0xFFFFC);
    rv_read_store(&MEM, &v);
    CHECK_EQ(v.store, RV_STORE_ORDER);
    CHECK_EQ(v.store_bad, RV_SONGS);
    mem_reset();
    store_build(LAYOUT, 4);
    w16(RV_A_STORE + OFF[10], 0);
    rv_read_store(&MEM, &v);
    CHECK_EQ(v.store, RV_STORE_MARKER);
    mem_reset();
    store_build(LAYOUT, 4);
    w16(RV_A_STORE + RV_STORE_LIMIT, 0xFFFF);
    rv_read_store(&MEM, &v);
    CHECK_EQ(v.store, RV_STORE_MARKER);
    /* a failed check reports no songs (never a half-read table) */
    for (unsigned i = 0; i < RV_SONGS; i++)
        CHECK_EQ(v.songs[i].len, 0);
    CHECK_EQ(stray_reads, 0);
}

TEST(recview_diff)
{
    mem_reset();
    store_build(LAYOUT, 4);
    rv_view_t a, b;
    w8(RV_A_STATE, 2);
    rv_read(&MEM, &a);
    b = a;
    CHECK_EQ(rv_diff(&a, &b), 0);
    b.state = 1, b.transport = RV_T_PLAYING;
    CHECK_EQ(rv_diff(&a, &b), RV_CH_TRANSPORT);
    b = a, b.cur_bar = 2;
    CHECK_EQ(rv_diff(&a, &b), RV_CH_BAR);
    b = a, b.play_sel = 1;
    CHECK_EQ(rv_diff(&a, &b), RV_CH_PART);
    b = a, b.songs[4].bars = 1;
    CHECK_EQ(rv_diff(&a, &b), RV_CH_STORE);
    /* commit points */
    a.state = 4, a.transport = RV_T_RECORDING, a.busy = RV_BUSY_REC;
    b = a, b.state = 2, b.transport = RV_T_IDLE, b.busy = RV_BUSY_JOB;
    CHECK(rv_diff(&a, &b) & RV_CH_COMMIT);
    a = b, b.busy = 0;
    CHECK_EQ(rv_diff(&a, &b), RV_CH_BUSY | RV_CH_COMMIT);
    a.busy = RV_BUSY_FILEOP, b.busy = 0;
    CHECK(rv_diff(&a, &b) & RV_CH_COMMIT);
    a.busy = 0, b.busy = RV_BUSY_JOB; /* a job starting is not a commit point */
    CHECK_EQ(rv_diff(&a, &b), RV_CH_BUSY);
}

TEST(recview_encode)
{
    mem_reset();
    store_build(LAYOUT, 4);
    w8(RV_A_STATE, 3), w8(RV_A_SONG, 1), w16(RV_A_TEMPO, 0x0102), w32(RV_A_POS, 0x0A0B0C0D);
    w8(RV_A_MAX_SLOT, 0xFF), w16(RV_A_BAR, 0x0123);
    rv_view_t v;
    rv_read(&MEM, &v);
    uint8_t o[RV_ENC_LEN + 4];
    CHECK_EQ(rv_encode(&v, o, RV_ENC_LEN - 1), 0);
    CHECK_EQ(rv_encode(&v, o, sizeof o), RV_ENC_LEN);
    CHECK_EQ(RV_ENC_LEN, 102);
    CHECK_EQ(o[0], RV_ENC_VERSION);
    CHECK_EQ(o[1], 3);
    CHECK_EQ(o[2], RV_T_REC_ARMED);
    CHECK_EQ(o[3], 1);
    CHECK_EQ(o[7], RV_BUSY_REC);
    CHECK_EQ(o[8] | o[9] << 8, 0x0102);
    CHECK_EQ(o[12], 0x0D);
    CHECK_EQ(o[15], 0x0A);
    CHECK_EQ(o[18], 0xFF);
    CHECK_EQ(o[19], RV_STORE_OK);
    CHECK_EQ(o[24] | o[25] << 8 | o[26] << 16, OFF[10]);
    CHECK_EQ(o[29] | o[30] << 8, 0x0123); /* R1: bar in the old padding */
    CHECK_EQ(o[31], 0);
    CHECK_EQ(o[32 + 7 * 3 + 4], 200);
    CHECK_EQ(o[32 + 7 * 0 + 6], RV_SONG_PART1 | RV_SONG_SETTINGS | RV_SONG_SOUNDMODE);
    for (unsigned i = 0; i < NR; i++)
        free(R[i].mem), R[i].mem = 0;
}

void suite_recview(void)
{
    printf("recview (Phase 3-6 R0)\n");
    RUN(recview_transport_map);
    RUN(recview_live_fields);
    RUN(recview_busy_bits);
    RUN(recview_song_table);
    RUN(recview_dump_shape);
    RUN(recview_store_checks);
    RUN(recview_diff);
    RUN(recview_encode);
}
