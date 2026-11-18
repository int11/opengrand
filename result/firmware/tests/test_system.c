/* Integration tests: the whole instrument (piano_t) with a fake engine,
 * fake MIDI ports and RAM flash. */
#include <math.h>
#include <stdlib.h>

#include "ca79ng/piano.h"
#include "test.h"

/* ---- fake engine ---- */
static struct {
    int starts, releases;
    float last_freq;
    uint8_t last_vel, last_part, last_msb, last_lsb, last_pc;
    int params;
    uint16_t last_param;
    int32_t last_value;
} eng;

static void e_start(void *c, int v, const engine_note_t *n)
{
    (void)c; (void)v;
    eng.starts++;
    eng.last_freq = n->freq_hz;
    eng.last_vel = n->velocity;
    eng.last_part = n->part;
    eng.last_msb = n->msb; eng.last_lsb = n->lsb; eng.last_pc = n->program;
}
static void e_release(void *c, int v) { (void)c; (void)v; eng.releases++; }
static void e_kill(void *c, int v) { (void)c; (void)v; }
static void e_control(void *c, uint8_t part, uint8_t cc, uint8_t val) { (void)c; (void)part; (void)cc; (void)val; }
static void e_param(void *c, uint16_t id, uint8_t part, int32_t v) { (void)c; (void)part; eng.params++; eng.last_param = id; eng.last_value = v; }
static sound_engine_t ENGINE = {.start = e_start, .release = e_release, .kill = e_kill, .control = e_control, .set_param = e_param};

/* ---- fake ports ---- */
static struct {
    int n;
    midi_port_t port[4096];
    uint8_t msg[4096][24];
    uint16_t len[4096];
    int clicks, accents, power_offs;
} out;

/* The controllers in these tests ACK every value / end the piano sends (fn 0x10 / 0x03) at
 * once, like the apps do: the core sends them stop-and-wait (one in flight per port). */
static struct { midi_port_t port; uint8_t key[3]; } ackq[256];
static int ackq_n;
static void ack_later(midi_port_t port, const uint8_t *m, uint16_t n)
{
    if (n >= 10 && m[0] == 0xF0 && m[1] == 0x40 && (m[3] == 0x10 || m[3] == 0x03) && m[4] == 0x04 && ackq_n < 256) {
        ackq[ackq_n].port = port;
        memcpy(ackq[ackq_n].key, m + 6, 3);
        ackq_n++;
    }
}

static void p_tx(void *c, midi_port_t port, const uint8_t *m, uint16_t n)
{
    (void)c;
    ack_later(port, m, n);
    if (out.n < 4096) {
        out.port[out.n] = port;
        memcpy(out.msg[out.n], m, n < 24 ? n : 24);
        out.len[out.n] = n;
        out.n++;
    }
}
static void p_click(void *c, bool accent, uint8_t vol) { (void)c; (void)vol; out.clicks++; out.accents += accent; }
static void p_off(void *c) { (void)c; out.power_offs++; }

/* ---- RAM flash ---- */
#define FSEC 4096u
static uint8_t flash_mem[2][FSEC];
static bool f_erase(void *c, uint32_t s) { (void)c; memset(flash_mem[s], 0xFF, FSEC); return true; }
static bool f_prog(void *c, uint32_t s, uint32_t o, const void *d, uint32_t n)
{
    (void)c;
    for (uint32_t i = 0; i < n; i++) flash_mem[s][o + i] &= ((const uint8_t *)d)[i];
    return true;
}
static bool f_read(void *c, uint32_t s, uint32_t o, void *d, uint32_t n) { (void)c; memcpy(d, flash_mem[s] + o, n); return true; }
static const hal_flash_t FLASH = {NULL, FSEC, f_erase, f_prog, f_read};

static rec_event_t pool[REC_MAX_EVENTS], stage[REC_MAX_EVENTS];
static piano_t P;
static kvstore_t KV;

static void boot(bool wipe_flash)
{
    memset(&eng, 0, sizeof eng);
    memset(&out, 0, sizeof out);
    if (wipe_flash)
        memset(flash_mem, 0xFF, sizeof flash_mem);
    kv_mount(&KV, &FLASH, 0, 1);
    piano_platform_t plat = {.midi_tx = p_tx, .metronome_click = p_click, .power_off = p_off};
    piano_init(&P, &plat, &ENGINE, &KV, pool, stage);
    piano_tick(&P, 1);
    /* the core answers only bound controller ports (stock): bind the panel and USB */
    const uint8_t sync[] = {0xF0, 0x40, 0x7F, 0x7E, 0x08, 0x02, 0x01, 0x01, 0xF7};
    piano_midi_in(&P, MIDI_PORT_PANEL, sync, sizeof sync);
    piano_midi_in(&P, MIDI_PORT_USB, sync, sizeof sync);
    ackq_n = 0;
    out.n = 0;
}

static void ack_drain(void)
{
    for (int i = 0; i < ackq_n; i++) {
        const uint8_t a[] = {0xF0, 0x40, 0x7F, 0x07, 0x08, 0x02, ackq[i].key[0], ackq[i].key[1], ackq[i].key[2], 0xF7};
        piano_midi_in(&P, ackq[i].port, a, sizeof a);
    }
    ackq_n = 0;
}

static void sx(midi_port_t port, const uint8_t *m, uint16_t n)
{
    piano_midi_in(&P, port, m, n);
    ack_drain();
}

/* press key index k with a given S1->S2 travel time */
static void press(uint8_t k, uint32_t travel_us, uint32_t *t)
{
    piano_key_edge(&P, k, 1, true, *t);
    *t += travel_us;
    piano_key_edge(&P, k, 2, true, *t);
    *t += 1000;
}
static void release(uint8_t k, uint32_t *t)
{
    piano_key_edge(&P, k, 2, false, *t);
    *t += 1000;
    piano_key_edge(&P, k, 1, false, *t);
    *t += 1000;
}

static int find_tx(midi_port_t port, uint8_t b0, uint8_t b1)
{
    for (int i = 0; i < out.n; i++)
        if (out.port[i] == port && out.msg[i][0] == b0 && (b1 == 0xFF || out.msg[i][1] == b1))
            return i;
    return -1;
}

TEST(test_key_plays_and_transmits)
{
    boot(true);
    uint32_t t = 1000;
    press(48, 4000, &t); /* key 48 = A4 (note 69) */
    CHECK_EQ(eng.starts, 1);
    CHECK(fabsf(eng.last_freq - 440.0f) < 1.0f); /* stretch tuning shifts A4 only slightly */
    CHECK_EQ(eng.last_msb, 95); /* SK-EX Rendering by default */
    CHECK_EQ(eng.last_lsb, 112);
    int i = find_tx(MIDI_PORT_DIN, 0x90, 69);
    CHECK(i >= 0);
    CHECK(find_tx(MIDI_PORT_USB, 0x90, 69) >= 0);
    CHECK_EQ(find_tx(MIDI_PORT_PANEL, 0x90, 69), -1); /* panel gets no note stream */
    release(48, &t);
    CHECK_EQ(eng.releases, 1);
    CHECK(find_tx(MIDI_PORT_DIN, 0x80, 69) >= 0);
}

TEST(test_remote_changes_sound_and_local_off)
{
    boot(true);
    const uint8_t sync[] = {0xF0, 0x40, 0x7F, 0x7E, 0x08, 0x02, 0x01, 0x01, 0xF7};
    sx(MIDI_PORT_PANEL, sync, sizeof sync);
    /* select Jazz Organ (bank 121/0, PC 17) on Main1 */
    const uint8_t snd[] = {0xF0, 0x40, 0x00, 0x10, 0x08, 0x02, 0x55, 0x00, 0x00, 121, 0, 17, 0xF7};
    sx(MIDI_PORT_PANEL, snd, sizeof snd);
    uint32_t t = 1000;
    press(39, 3000, &t);
    CHECK_EQ(eng.last_msb, 121);
    CHECK_EQ(eng.last_pc, 17);
    /* the program change was transmitted (Transmit PGM on by default) */
    /* bank select MSB/LSB + program change go out as one 8-byte burst */
    int pc = find_tx(MIDI_PORT_DIN, 0xB0, 0x00);
    CHECK(pc >= 0);
    if (pc >= 0) {
        CHECK_EQ(out.msg[pc][2], 121);
        CHECK_EQ(out.msg[pc][6], 0xC0);
        CHECK_EQ(out.msg[pc][7], 17);
    }
    /* Local Control off: keys still transmit, nothing sounds */
    const uint8_t loc[] = {0xF0, 0x40, 0x00, 0x10, 0x08, 0x02, 0x58, 0x01, 0x7F, 0x00, 0xF7};
    sx(MIDI_PORT_PANEL, loc, sizeof loc);
    int starts = eng.starts;
    out.n = 0;
    press(40, 3000, &t);
    CHECK_EQ(eng.starts, starts);
    CHECK(find_tx(MIDI_PORT_DIN, 0x90, 61) >= 0);
}

TEST(test_dual_split_fourhands)
{
    boot(true);
    const uint8_t dual[] = {0xF0, 0x40, 0x00, 0x10, 0x08, 0x02, 0x53, 0x00, 0x0F, 0x01, 0xF7};
    sx(MIDI_PORT_PANEL, dual, sizeof dual);
    CHECK_EQ(P.layout.mode, KBMODE_DUAL);
    uint32_t t = 0;
    press(39, 3000, &t);
    CHECK_EQ(eng.starts, 2); /* main + layer */
    CHECK_EQ(eng.last_part, PART_LAYER);
    CHECK_EQ(eng.last_msb, 95); /* layer default: Slow Strings 95/1/44 */
    CHECK_EQ(eng.last_lsb, 1);
    release(39, &t);

    const uint8_t four[] = {0xF0, 0x40, 0x00, 0x10, 0x08, 0x02, 0x53, 0x00, 0x0F, 0x03, 0xF7};
    sx(MIDI_PORT_PANEL, four, sizeof four);
    eng.starts = 0;
    press(10, 3000, &t); /* left half */
    CHECK_EQ(eng.last_part, PART_4H_LEFT);
    press(70, 3000, &t); /* right half */
    CHECK_EQ(eng.last_part, PART_4H_RIGHT);
}

TEST(test_midi_in_plays_and_multitimbral)
{
    boot(true);
    const uint8_t on[] = {0x90, 60, 100};
    piano_midi_in(&P, MIDI_PORT_DIN, on, 3);
    CHECK_EQ(eng.starts, 1);
    CHECK_EQ(eng.last_part, PART_MAIN1); /* omni */
    const uint8_t mt[] = {0xF0, 0x40, 0x00, 0x10, 0x08, 0x02, 0x58, 0x10, 0x7F, 0x01, 0xF7};
    sx(MIDI_PORT_USB, mt, sizeof mt);
    const uint8_t on5[] = {0x94, 64, 90};
    piano_midi_in(&P, MIDI_PORT_USB, on5, 3);
    CHECK_EQ(eng.last_part, PART_MIDI_BASE + 4);
}

TEST(test_metronome_beat_counter_to_remote)
{
    boot(true);
    const uint8_t sync[] = {0xF0, 0x40, 0x7F, 0x7E, 0x08, 0x02, 0x01, 0x01, 0xF7};
    sx(MIDI_PORT_PANEL, sync, sizeof sync);
    /* tempo 150 (14-bit 01 16), then run via remote command fn 0x32 */
    const uint8_t tempo[] = {0xF0, 0x40, 0x00, 0x10, 0x08, 0x02, 0x56, 0x01, 0x7F, 0x01, 0x16, 0xF7};
    sx(MIDI_PORT_PANEL, tempo, sizeof tempo);
    CHECK_EQ(P.metro.bpm, 150);
    const uint8_t run[] = {0xF0, 0x40, 0x00, 0x32, 0x08, 0x02, 0x56, 0x08, 0x7F, 0x01, 0xF7};
    sx(MIDI_PORT_PANEL, run, sizeof run);
    out.n = 0;
    for (int i = 0; i < 2000; i++)
        piano_tick(&P, 1); /* 2 s at 150 BPM = 5 beats + start downbeat */
    CHECK_EQ(out.clicks, 6);
    CHECK_EQ(out.accents, 2);
    int beat_msgs = 0;
    for (int i = 0; i < out.n; i++)
        if (out.port[i] == MIDI_PORT_PANEL && out.msg[i][3] == 0x10 && out.msg[i][6] == 0x56 && out.msg[i][7] == 0x0B)
            beat_msgs++;
    CHECK_EQ(beat_msgs, 6);
}

/* the System values the metronome and song modules hold answer a
 * single get (they were missing: only the end mark came) - the stock's value bytes */
TEST(test_system_values_answer_get)
{
    static const struct { uint8_t v1, v2, n, d[2]; } want[] = {
        {0x56, 0x01, 2, {0x00, 0x78}},   /* MetronomeTempo 120 */
        {0x56, 0x02, 2, {0x04, 0x02}},   /* BeatSelect 4/4 (stock beat table 0x401235B8: 4 beats, code 2) */
        {0x56, 0x09, 1, {0x00}},         /* RhythmSelect */
        {0x56, 0x0A, 1, {0x00}},         /* MetronomeMode click */
        {0x56, 0x03, 1, {80}},           /* MetronomeVolume (core default) */
        {0x52, 0x02, 2, {0x00, 0x00}},   /* MusicTempo: no song */
        {0x52, 0x01, 1, {0x7F}},         /* IntSongPart: both */
    };
    boot(true);
    for (unsigned k = 0; k < sizeof want / sizeof want[0]; k++) {
        const uint8_t get[] = {0xF0, 0x40, 0x7F, 0x02, 0x08, 0x02, want[k].v1, want[k].v2, 0x7F, 0xF7};
        int found = 0;
        out.n = 0;
        sx(MIDI_PORT_USB, get, sizeof get);
        for (int i = 0; i < out.n; i++)
            if (out.port[i] == MIDI_PORT_USB && out.msg[i][3] == 0x10 && out.msg[i][6] == want[k].v1 &&
                out.msg[i][7] == want[k].v2 && out.msg[i][8] == 0x7F && out.len[i] == 10u + want[k].n &&
                memcmp(out.msg[i] + 9, want[k].d, want[k].n) == 0)
                found++;
        CHECK_EQ(found, 1);
    }
    /* a song tempo set, then a song select: back to 00 00 */
    const uint8_t mt[] = {0xF0, 0x40, 0x7F, 0x10, 0x08, 0x02, 0x52, 0x02, 0x7F, 0x00, 0x64, 0xF7};
    sx(MIDI_PORT_USB, mt, sizeof mt);
    uint8_t v[4];
    CHECK(param_get(&P.store, PRM_MUSIC_TEMPO, SLOT_SYSTEM, v) && v[1] == 0x64);
    const uint8_t sel[] = {0xF0, 0x40, 0x7F, 0x10, 0x08, 0x02, 0x52, 0x00, 0x7F, 0x00, 0x00, 0x01, 0xF7};
    sx(MIDI_PORT_USB, sel, sizeof sel);
    CHECK(param_get(&P.store, PRM_MUSIC_TEMPO, SLOT_SYSTEM, v) && v[0] == 0 && v[1] == 0);
}

TEST(test_recorder_record_play_and_smf)
{
    boot(true);
    const uint8_t rec[] = {0xF0, 0x40, 0x00, 0x32, 0x08, 0x02, 0x52, 0x08, 0x7F, 0x03, 0xF7}; /* RecStandby */
    sx(MIDI_PORT_PANEL, rec, sizeof rec);
    CHECK_EQ(P.rec.state, REC_STANDBY);
    uint32_t t = 0;
    press(39, 3000, &t); /* first key starts recording */
    CHECK_EQ(P.rec.state, REC_RECORDING);
    for (int i = 0; i < 500; i++) piano_tick(&P, 1);
    release(39, &t);
    for (int i = 0; i < 200; i++) piano_tick(&P, 1);
    press(43, 3000, &t);
    for (int i = 0; i < 300; i++) piano_tick(&P, 1);
    release(43, &t);
    const uint8_t stop[] = {0xF0, 0x40, 0x00, 0x32, 0x08, 0x02, 0x52, 0x08, 0x7F, 0x02, 0xF7}; /* Reset */
    sx(MIDI_PORT_PANEL, stop, sizeof stop);
    CHECK_EQ(P.rec.song[0].count, 4);

    int starts = eng.starts;
    const uint8_t play[] = {0xF0, 0x40, 0x00, 0x32, 0x08, 0x02, 0x52, 0x08, 0x7F, 0x01, 0xF7};
    sx(MIDI_PORT_PANEL, play, sizeof play);
    for (int i = 0; i < 1200; i++) piano_tick(&P, 1);
    CHECK_EQ(eng.starts - starts, 2);
    CHECK_EQ(P.rec.state, REC_IDLE); /* finished */

    static uint8_t smf[4096];
    size_t n = smf_write(&P.rec, 0, smf, sizeof smf);
    CHECK(n > 40);
    CHECK_MEM(smf, "MThd", 4);
    CHECK(smf_read(&P.rec, 1, smf, n));
    CHECK_EQ(P.rec.song[1].count, 4);
    CHECK(abs((int)P.rec.song[1].length_ms - (int)P.rec.song[0].length_ms) <= 2);
}

TEST(test_overdub_second_part)
{
    boot(true);
    recorder_t *r = &P.rec;
    recorder_standby(r, 0, 120, 4, 4);
    recorder_start(r);
    recorder_input(r, 0x90, 60, 100);
    recorder_tick(r, 100);
    recorder_input(r, 0x80, 60, 0);
    recorder_stop(r);
    /* part 2 while part 1 plays back */
    int starts = eng.starts;
    recorder_standby(r, 1, 120, 4, 4);
    recorder_start(r);
    recorder_tick(r, 10); /* part 1 note-on plays */
    CHECK_EQ(eng.starts - starts, 1);
    recorder_input(r, 0x90, 64, 90);
    recorder_tick(r, 200);
    recorder_input(r, 0x80, 64, 0);
    recorder_stop(r);
    CHECK_EQ(r->song[0].count, 4);
    /* events stay time-ordered across parts */
    for (uint32_t i = 1; i < r->song[0].count; i++)
        CHECK(r->pool[r->song[0].first + i - 1].ms <= r->pool[r->song[0].first + i].ms);
}

TEST(test_startup_settings_and_factory_reset)
{
    boot(true);
    const uint8_t rt[] = {0xF0, 0x40, 0x00, 0x10, 0x08, 0x02, 0x55, 0x08, 0x00, 0x05, 0xF7}; /* Cathedral */
    sx(MIDI_PORT_PANEL, rt, sizeof rt);
    /* 00 00 00 00 (the app's old bytes): a failed lookup on the stock - nothing saved */
    const uint8_t nosave[] = {0xF0, 0x40, 0x00, 0x32, 0x08, 0x02, 0x51, 0x34, 0x7F, 0x00, 0x00, 0x00, 0x00, 0xF7};
    sx(MIDI_PORT_PANEL, nosave, sizeof nosave);
    boot(false);
    CHECK_EQ(param_get1(&P.store, PRM_REVERB_TYPE, SLOT_MAIN1, 0), 2);
    sx(MIDI_PORT_PANEL, rt, sizeof rt);
    const uint8_t save[] = {0xF0, 0x40, 0x00, 0x32, 0x08, 0x02, 0x51, 0x34, 0x7F, 0x07, 0x00, 0x7F, 0x7F, 0xF7};
    sx(MIDI_PORT_PANEL, save, sizeof save);
    boot(false); /* power cycle, flash kept */
    CHECK_EQ(param_get1(&P.store, PRM_REVERB_TYPE, SLOT_MAIN1, 0), 5);
    /* 36 07: only the saved startup settings back to factory - the live value stays until the next boot */
    const uint8_t reset7[] = {0xF0, 0x40, 0x00, 0x32, 0x08, 0x02, 0x51, 0x36, 0x7F, 0x07, 0x7F, 0x7F, 0x7F, 0xF7};
    sx(MIDI_PORT_PANEL, reset7, sizeof reset7);
    CHECK_EQ(param_get1(&P.store, PRM_REVERB_TYPE, SLOT_MAIN1, 0), 5);
    boot(false);
    CHECK_EQ(param_get1(&P.store, PRM_REVERB_TYPE, SLOT_MAIN1, 0), 2);
    sx(MIDI_PORT_PANEL, rt, sizeof rt);
    sx(MIDI_PORT_PANEL, save, sizeof save);
    const uint8_t curve[4] = {1, 2, 3, 4};
    kv_put(&KV, KV_KEY_USER_TOUCHCURVE, curve, sizeof curve); /* saved user VT data */
    piano_factory_reset(&P);
    CHECK_EQ(param_get1(&P.store, PRM_REVERB_TYPE, SLOT_MAIN1, 0), 2);
    uint8_t tmp[8];
    CHECK_EQ(kv_get(&KV, KV_KEY_USER_TOUCHCURVE, tmp, sizeof tmp), -1); /* user VT data cleared */
    boot(false);
    CHECK_EQ(param_get1(&P.store, PRM_REVERB_TYPE, SLOT_MAIN1, 0), 2);
}

TEST(test_auto_power_off)
{
    boot(true);
    const uint8_t apo[] = {0xF0, 0x40, 0x00, 0x10, 0x08, 0x02, 0x51, 0x00, 0x7F, 0x01, 0xF7}; /* 15 min */
    sx(MIDI_PORT_PANEL, apo, sizeof apo);
    for (int m = 0; m < 14; m++) piano_tick(&P, 60000);
    CHECK_EQ(out.power_offs, 0);
    uint32_t t = 0;
    press(30, 3000, &t); /* activity resets the timer */
    for (int m = 0; m < 14; m++) piano_tick(&P, 60000);
    CHECK_EQ(out.power_offs, 0);
    piano_tick(&P, 60000);
    /* owner decision 2026-10-31: APO expiry enters partial standby, the real off follows the cap */
    CHECK_EQ(out.power_offs, 0);
    CHECK_EQ(P.sb.state, SB_STANDBY);
    for (int m = 0; m < 8 * 60; m++) piano_tick(&P, 60000);
    CHECK_EQ(out.power_offs, 1);
}

void suite_system(void)
{
    puts("system:");
    RUN(test_key_plays_and_transmits);
    RUN(test_system_values_answer_get);
    RUN(test_remote_changes_sound_and_local_off);
    RUN(test_dual_split_fourhands);
    RUN(test_midi_in_plays_and_multitimbral);
    RUN(test_metronome_beat_counter_to_remote);
    RUN(test_recorder_record_play_and_smf);
    RUN(test_overdub_second_part);
    RUN(test_startup_settings_and_factory_reset);
    RUN(test_auto_power_off);
}
