/* Phase 3-5 metronome stage 1 (src/kproto/kpmetro.c): the stock apply's rules for metronome sets, the store
 * bytes the stock getter answers, the BeatCounter frames from sets and from the 2 ms poll of the stock run
 * state (the emulator round trip that runs the stock paths is
 * result/stockfw/tools/roundtrip_metro.py). */
#include <string.h>

#include "ca79ng/kpmetro.h"
#include "test.h"

static const kpm_cfg_t DEFAULT_CFG = {0, 0, 0, 3, 120, 74};   /* this piano's NOR bytes */

static kpm_result_t judge2(const kpm_cfg_t *c, uint8_t v2, uint8_t a, uint8_t b)
{
    const uint8_t d[2] = {a, b};
    return kpm_judge(c, v2, d, 2);
}

static kpm_result_t judge1(const kpm_cfg_t *c, uint8_t v2, uint8_t a)
{
    return kpm_judge(c, v2, &a, 1);
}

TEST(kpm_judge_follows_the_stock_apply)
{
    kpm_cfg_t c = DEFAULT_CFG;
    kpm_result_t r;
    CHECK(kpm_is_key(0x56, 0x08, 0x7F) && kpm_is_key(0x56, 0x0A, 0x7F));
    CHECK(!kpm_is_key(0x56, 0x0B, 0x7F) && !kpm_is_key(0x56, 0x01, 0x00) && !kpm_is_key(0x52, 0x02, 0x7F));
    /* on: raw byte, start in click mode sends BeatCounter 1 */
    r = judge1(&c, 0x08, 5);
    CHECK_EQ(r.verdict, KPM_APPLIED);
    CHECK_EQ(r.cfg.on, 5);
    CHECK_EQ(r.fx & (KPM_FX_START | KPM_FX_RHYTHM | KPM_FX_BEAT1 | KPM_FX_CHANGED), KPM_FX_START | KPM_FX_BEAT1 | KPM_FX_CHANGED);
    c.on = 1;
    r = judge1(&c, 0x08, 0);
    CHECK_EQ(r.fx, KPM_FX_STOP | KPM_FX_CHANGED);
    c.mode = 1;
    r = judge1(&c, 0x08, 1);   /* already on, rhythm mode: rhythm start, nothing changed in the block */
    CHECK_EQ(r.fx & (KPM_FX_RHYTHM | KPM_FX_BEAT1 | KPM_FX_CHANGED), KPM_FX_RHYTHM | KPM_FX_BEAT1);
    c = DEFAULT_CFG;
    /* tempo: raw, accumulator clamps */
    r = judge2(&c, 0x01, 0x00, 0x05);
    CHECK_EQ(r.cfg.tempo, 5);
    CHECK_EQ(r.bpm, 120);
    r = judge2(&c, 0x01, 0x7F, 0x7F);
    CHECK_EQ(r.cfg.tempo, 16383);
    CHECK_EQ(r.bpm, 120);
    r = judge2(&c, 0x01, 0x01, 0x7F);
    CHECK_EQ(r.bpm, 255);
    CHECK_EQ(r.fx, KPM_FX_TEMPO | KPM_FX_CHANGED);
    /* beat: refused value keeps the index but restarts anyway */
    r = judge2(&c, 0x02, 0x06, 0x03);
    CHECK_EQ(r.cfg.beat, 6);
    CHECK(r.fx & KPM_FX_START);
    r = judge2(&c, 0x02, 0x07, 0x02);
    CHECK_EQ(r.verdict, KPM_APPLIED);
    CHECK_EQ(r.cfg.beat, 3);
    CHECK(r.fx & KPM_FX_START);
    CHECK(!(r.fx & KPM_FX_CHANGED));
    CHECK(!(r.fx & KPM_FX_BEAT1));   /* metronome off: the restart sends nothing */
    /* rhythm < 100 only */
    r = judge1(&c, 0x09, 99);
    CHECK_EQ(r.cfg.rhythm, 99);
    CHECK_EQ(r.fx, KPM_FX_RELOAD | KPM_FX_CHANGED);
    r = judge1(&c, 0x09, 100);
    CHECK_EQ(r.verdict, KPM_APPLIED);
    CHECK_EQ(r.fx, 0);
    /* volume, mode */
    r = judge1(&c, 0x03, 0x7F);
    CHECK_EQ(r.cfg.volume, 0x7F);
    CHECK(r.fx & KPM_FX_VOLUME);
    r = judge1(&c, 0x0A, 2);
    CHECK_EQ(r.cfg.mode, 2);
    CHECK(r.fx & KPM_FX_RHYTHM);
    r = judge1(&c, 0x0A, 0);
    CHECK(r.fx & KPM_FX_START);
    /* a length other than the descriptor's is not applied */
    {
        const uint8_t d[2] = {1, 2};
        CHECK_EQ(kpm_judge(&c, 0x08, d, 2).verdict, KPM_IGNORED);
        CHECK_EQ(kpm_judge(&c, 0x01, d, 1).verdict, KPM_IGNORED);
        CHECK_EQ(kpm_judge(&c, 0x0B, d, 2).verdict, KPM_NOT_MINE);
    }
}

TEST(kpm_beat_tables)
{
    uint16_t t;
    uint8_t b;
    CHECK_EQ(kpm_beat_index(4, 2), 3);
    CHECK_EQ(kpm_beat_index(1, 2), 0);
    CHECK_EQ(kpm_beat_index(5, 2), 4);
    CHECK_EQ(kpm_beat_index(3, 3), 5);
    CHECK_EQ(kpm_beat_index(12, 3), 9);
    CHECK_EQ(kpm_beat_index(6, 2), -1);
    CHECK_EQ(kpm_beat_index(4, 3), -1);
    CHECK_EQ(kpm_beat_index(13, 3), -1);
    CHECK_EQ(kpm_beat_index(4, 5), -1);
    kpm_beat_entry(6, &t, &b);
    CHECK(t == 0x30 && b == 6);
    kpm_beat_entry(12, &t, &b);   /* > 9 -> 3 = 4/4 */
    CHECK(t == 0x60 && b == 4);
}

TEST(kpm_values_and_store_are_the_getters)
{
    kpm_cfg_t c = {1, 0, 7, 6, 300, 0x40};
    param_store_t s;
    uint8_t v[4];
    int slot = param_slot_from_part(0x7F);
    CHECK_EQ(kpm_value(&c, 0x01, v), 2);
    CHECK(v[0] == 0x02 && v[1] == 0x2C);   /* 300 = 2 * 128 + 44 */
    CHECK_EQ(kpm_value(&c, 0x02, v), 2);
    CHECK(v[0] == 6 && v[1] == 3);
    c.beat = 11;
    kpm_value(&c, 0x02, v);
    CHECK(v[0] == 4 && v[1] == 2);
    CHECK_EQ(kpm_value(&c, 0x0B, v), 0);
    memset(&s, 0, sizeof s);
    CHECK_EQ(kpm_store(&c, &s), 6);
    CHECK_EQ(kpm_store(&c, &s), 0);
    CHECK(param_get(&s, PRM_RHYTHM_SELECT, slot, v) && v[0] == 7);
    CHECK(param_get(&s, PRM_METRONOME, slot, v) && v[0] == 1);
    CHECK(param_get(&s, PRM_BEAT_SELECT, slot, v) && v[0] == 4 && v[1] == 2);
}

TEST(kpm_accum_and_ports)
{
    uint32_t q, r;
    CHECK_EQ(kpm_accum(120, &q, &r), 120);
    CHECK(q == 0 && r == 23040);
    CHECK_EQ(kpm_accum(400, &q, &r), 400);
    CHECK(q == 1 && r == 16800);
    CHECK_EQ(kpm_accum(9, &q, &r), 120);
    CHECK_EQ(kpm_accum(401, &q, &r), 120);
    CHECK_EQ(kpm_stock_ports(0, true), 0);
    CHECK_EQ(kpm_stock_ports(2, true), 0x02);
    CHECK_EQ(kpm_stock_ports(4, false), 0x08);
    CHECK_EQ(kpm_stock_ports(4, true), 0x0F);
}

static kpm_run_t run_of(int pos, int len, int beat, int bpb)
{
    kpm_run_t r = {(int16_t)pos, (int16_t)len, (int16_t)(len / 2), (int8_t)beat, (uint8_t)bpb, 1, 0, 2};
    return r;
}

TEST(kpm_set_and_poll_make_the_stock_beatcounters)
{
    kpm_t m;
    kpm_result_t r;
    uint8_t f[KPM_FRAME_LEN], o[2][KPM_FRAME_LEN];
    const uint8_t want1[KPM_FRAME_LEN] = {0xF0, 0x40, 0x7F, 0x32, 0x04, 0x1E, 0x56, 0x0B, 0x7F, 0x01, 0x7F, 0xF7};
    kpm_run_t s = run_of(10, 96, 2, 4);
    const uint8_t on = 1;
    kpm_init(&m, &DEFAULT_CFG);
    s.on = 0;
    CHECK_EQ(kpm_poll(&m, &s, o), 0);   /* first poll: takes the state */
    /* on: the apply's BeatCounter 1, counters armed (beat = beats per bar, position = length) */
    CHECK_EQ(kpm_set(&m, 0x08, &on, 1, &r, f), KPM_FRAME_LEN);
    CHECK_MEM(f, want1, KPM_FRAME_LEN);
    s = run_of(96, 96, 4, 4);
    CHECK_EQ(kpm_poll(&m, &s, o), 0);   /* same pass: armed, nothing yet */
    s = run_of(0, 96, 1, 4);
    CHECK_EQ(kpm_poll(&m, &s, o), 1);   /* task 20 wrapped: beat 1 */
    CHECK_MEM(o[0], want1, KPM_FRAME_LEN);
    for (int p = 1; p < 96; p++) {
        s = run_of(p, 96, 1, 4);
        CHECK_EQ(kpm_poll(&m, &s, o), 0);
    }
    s = run_of(0, 96, 2, 4);
    CHECK_EQ(kpm_poll(&m, &s, o), 1);
    CHECK_EQ(o[0][9], 2);
    /* a 1-beat bar: the beat stays 1, the position goes back */
    kpm_init(&m, &DEFAULT_CFG);
    s = run_of(95, 96, 1, 1);
    kpm_poll(&m, &s, o);
    s = run_of(0, 96, 1, 1);
    CHECK_EQ(kpm_poll(&m, &s, o), 1);
    CHECK_EQ(o[0][9], 1);
    /* half-beat position 0: FUN_4016EA18 marks the wrap as a half beat, nothing sent */
    s = run_of(95, 96, 1, 4);
    s.half = 0;
    kpm_poll(&m, &s, o);
    s = run_of(0, 96, 2, 4);
    s.half = 0;
    CHECK_EQ(kpm_poll(&m, &s, o), 0);
    /* off: nothing */
    s = run_of(0, 96, 3, 4);
    s.on = 0;
    CHECK_EQ(kpm_poll(&m, &s, o), 0);
    CHECK_EQ(m.st.beats, 1);   /* since the second kpm_init: the 1-beat bar wrap */
}

TEST(kpm_poll_reload_recorder_and_race)
{
    kpm_t m;
    kpm_result_t r;
    uint8_t f[KPM_FRAME_LEN], o[2][KPM_FRAME_LEN];
    kpm_run_t s = run_of(40, 96, 2, 4);
    const uint8_t d[2] = {6, 3};
    kpm_init(&m, &DEFAULT_CFG);
    s.reload = 1;
    kpm_poll(&m, &s, o);
    /* task 20 ran FUN_4016EF68 (flag fell): 1, and in the same pass the first wrap: 1 again */
    s = run_of(0, 96, 1, 4);
    CHECK_EQ(kpm_poll(&m, &s, o), 2);
    CHECK(o[0][9] == 1 && o[1][9] == 1);
    CHECK_EQ(m.st.reloads, 1);
    /* only the flag fell, counters armed */
    s.reload = 1;
    kpm_poll(&m, &s, o);
    s = run_of(96, 96, 4, 4);
    CHECK_EQ(kpm_poll(&m, &s, o), 1);
    /* count-in (state 6) and recorder state changes: no frames */
    s = run_of(0, 96, 1, 4);
    s.rec_state = 6;
    CHECK_EQ(kpm_poll(&m, &s, o), 0);
    s = run_of(0, 96, 2, 4);
    s.rec_state = 4;
    CHECK_EQ(kpm_poll(&m, &s, o), 0);
    CHECK_EQ(m.st.resyncs, 2);
    /* race: a beat set preempting FUN_4016EA18 - the poll sees it armed, then the stale position comes back */
    m.cfg.on = 1;
    s = run_of(16, 96, 2, 4);
    kpm_poll(&m, &s, o);
    CHECK_EQ(kpm_set(&m, 0x02, d, 2, &r, f), KPM_FRAME_LEN);
    s = run_of(48, 48, 6, 6);
    CHECK_EQ(kpm_poll(&m, &s, o), 0);
    s = run_of(17, 48, 6, 6);   /* position written back: back, but the beat did not move on - no frame */
    CHECK_EQ(kpm_poll(&m, &s, o), 0);
    s = run_of(0, 48, 1, 6);
    CHECK_EQ(kpm_poll(&m, &s, o), 1);
    CHECK_EQ(o[0][9], 1);
    CHECK_EQ(m.st.starts, 1);
}

void suite_kpmetro(void);
void suite_kpmetro(void)
{
    printf("metronome stage 1 (Phase 3-5)\n");
    RUN(kpm_judge_follows_the_stock_apply);
    RUN(kpm_beat_tables);
    RUN(kpm_values_and_store_are_the_getters);
    RUN(kpm_accum_and_ports);
    RUN(kpm_set_and_poll_make_the_stock_beatcounters);
    RUN(kpm_poll_reload_recorder_and_race);
}
