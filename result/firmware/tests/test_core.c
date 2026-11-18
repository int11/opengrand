/* Tests for util, MIDI parsing, keyboard, tuning, voices, metronome. */
#include <math.h>
#include <stdlib.h>

#include "ca79ng/keyboard.h"
#include "ca79ng/midi.h"
#include "ca79ng/rhythm.h"
#include "ca79ng/sound.h"
#include "ca79ng/util.h"
#include "test.h"

/* ---------------- util ---------------- */
TEST(test_ring)
{
    uint8_t mem[8], b;
    ca_ring_t r;
    ca_ring_init(&r, mem, 8);
    for (int i = 0; i < 8; i++)
        CHECK(ca_ring_put(&r, (uint8_t)i));
    CHECK(!ca_ring_put(&r, 99));
    CHECK_EQ(r.dropped, 1);
    for (int i = 0; i < 8; i++) {
        CHECK(ca_ring_get(&r, &b));
        CHECK_EQ(b, i);
    }
    CHECK(!ca_ring_get(&r, &b));
}

TEST(test_crc_sha)
{
    CHECK_EQ(ca_crc32(0, "123456789", 9), 0xCBF43926u);
    uint8_t d[32];
    ca_sha256_t c;
    ca_sha256_init(&c);
    ca_sha256_update(&c, "abc", 3);
    ca_sha256_final(&c, d);
    static const uint8_t exp[4] = {0xba, 0x78, 0x16, 0xbf};
    CHECK_MEM(d, exp, 4);
    CHECK_EQ(d[31], 0xad);
}

/* ---------------- MIDI parser ---------------- */
static midi_msg_t got[16];
static uint8_t got_sysex[16][64];
static int ngot;

static void collect(void *ctx, const midi_msg_t *m)
{
    (void)ctx;
    if (ngot < 16) {
        got[ngot] = *m;
        if (m->kind == MIDI_MSG_SYSEX)
            memcpy(got_sysex[ngot], m->sysex, m->sysex_len < 64 ? m->sysex_len : 64);
        ngot++;
    }
}

TEST(test_midi_running_status_and_realtime)
{
    midi_parser_t p;
    midi_parser_init(&p, MIDI_PORT_DIN, collect, NULL);
    ngot = 0;
    /* note on, running status note on, realtime clock in the middle of a message */
    const uint8_t in[] = {0x90, 60, 100, 62, 0xF8, 90, 0xC0, 5};
    midi_parser_feed_buf(&p, in, sizeof in);
    CHECK_EQ(ngot, 4);
    CHECK_EQ(got[0].status, 0x90);
    CHECK_EQ(got[0].data[0], 60);
    CHECK_EQ(got[1].kind, MIDI_MSG_REALTIME);
    CHECK_EQ(got[2].data[0], 62);
    CHECK_EQ(got[2].data[1], 90);
    CHECK_EQ(got[3].status, 0xC0);
    CHECK_EQ(got[3].data[0], 5);
}

TEST(test_midi_sysex)
{
    midi_parser_t p;
    midi_parser_init(&p, MIDI_PORT_PANEL, collect, NULL);
    ngot = 0;
    const uint8_t in[] = {0xF0, 0x40, 0x7F, 0x7E, 0x08, 0x02, 0x01, 0x01, 0xF7, 0x80, 60, 0};
    midi_parser_feed_buf(&p, in, sizeof in);
    CHECK_EQ(ngot, 2);
    CHECK_EQ(got[0].kind, MIDI_MSG_SYSEX);
    CHECK_EQ(got[0].sysex_len, 9);
    CHECK_MEM(got_sysex[0], in, 9);
    CHECK_EQ(got[0].port, MIDI_PORT_PANEL);
    CHECK_EQ(got[1].status, 0x80);
}

TEST(test_midi_sysex_overflow_dropped)
{
    midi_parser_t p;
    midi_parser_init(&p, MIDI_PORT_USB, collect, NULL);
    ngot = 0;
    midi_parser_feed(&p, 0xF0);
    for (int i = 0; i < 700; i++)
        midi_parser_feed(&p, 0x11);
    midi_parser_feed(&p, 0xF7);
    CHECK_EQ(ngot, 0);
    CHECK_EQ(p.errors, 1);
    /* parser recovers */
    const uint8_t in[] = {0x90, 64, 1};
    midi_parser_feed_buf(&p, in, 3);
    CHECK_EQ(ngot, 1);
}

/* ---------------- keyboard ---------------- */
TEST(test_scanner_velocity_and_repetition)
{
    key_scanner_t s;
    key_scanner_init(&s);
    CHECK_EQ(key_scanner_velocity(&s, 1000), 127);
    CHECK_EQ(key_scanner_velocity(&s, 200000), 1);
    uint8_t mid = key_scanner_velocity(&s, 13000);
    CHECK(mid > 40 && mid < 90);

    key_event_t e = key_scanner_edge(&s, 39, 1, true, 1000);
    CHECK_EQ(e.kind, KEY_EV_NONE);
    e = key_scanner_edge(&s, 39, 2, true, 1000 + 3000);
    CHECK_EQ(e.kind, KEY_EV_ON);
    CHECK(e.velocity > 100);
    /* half release to S2-open and re-strike without returning to S1-open */
    e = key_scanner_edge(&s, 39, 2, false, 50000);
    CHECK_EQ(e.kind, KEY_EV_NONE);
    e = key_scanner_edge(&s, 39, 2, true, 50000 + 20000);
    CHECK_EQ(e.kind, KEY_EV_ON);
    e = key_scanner_edge(&s, 39, 1, false, 90000);
    CHECK_EQ(e.kind, KEY_EV_OFF);
}

TEST(test_touch_curves)
{
    touch_config_t c;
    touch_config_default(&c);
    uint8_t normal = touch_apply(&c, 40, 64);
    c.curve = TC_LIGHT4;
    uint8_t light = touch_apply(&c, 40, 64);
    c.curve = TC_HEAVY4;
    uint8_t heavy = touch_apply(&c, 40, 64);
    CHECK(light > normal && normal > heavy);
    c.curve = TC_OFF;
    CHECK_EQ(touch_apply(&c, 40, 5), 100);
    CHECK_EQ(touch_apply(&c, 40, 127), 100);
    c.curve = TC_NORMAL;
    c.minimum_touch = 20;
    CHECK_EQ(touch_apply(&c, 40, 30), 0);
    CHECK(touch_apply(&c, 40, 60) > 0);
    c.minimum_touch = 1;
    c.key_volume = KV_HIGH_DAMP;
    CHECK(touch_apply(&c, 87, 100) < touch_apply(&c, 10, 100));
    c.key_volume = KV_NORMAL;
    c.curve = TC_USER;
    c.user_out[4] = 100; /* raise the middle of the curve */
    CHECK(touch_apply(&c, 40, 64) > 64);
}

TEST(test_pedals)
{
    pedal_config_t p;
    pedal_config_default(&p);
    CHECK_EQ(pedal_value(&p, PEDAL_DAMPER, 100), 0);
    CHECK_EQ(pedal_value(&p, PEDAL_DAMPER, 4000), 127);
    CHECK_EQ(pedal_value(&p, PEDAL_SOSTENUTO, 3000), 127);
    CHECK_EQ(pedal_value(&p, PEDAL_SOSTENUTO, 500), 0);
    p.half_pedal_adjust = 1;
    uint8_t early = pedal_value(&p, PEDAL_DAMPER, 1000);
    p.half_pedal_adjust = 7;
    uint8_t late = pedal_value(&p, PEDAL_DAMPER, 1000);
    CHECK(early > late);
}

/* ---------------- tuning ---------------- */
TEST(test_tuning)
{
    tuning_t t;
    tuning_default(&t);
    t.stretch = STRETCH_OFF;
    CHECK(fabsf(tuning_freq(&t, 69) - 440.0f) < 0.01f);
    CHECK(fabsf(tuning_freq(&t, 81) - 880.0f) < 0.05f);
    t.master_hz = 442.0f;
    CHECK(fabsf(tuning_freq(&t, 69) - 442.0f) < 0.01f);
    CHECK_EQ(tuning_hz_to_raw(440.0f), 64);
    CHECK_EQ(tuning_hz_to_raw(427.0f), 38);
    CHECK_EQ(tuning_hz_to_raw(453.0f), 90);
    CHECK(fabsf(tuning_raw_to_hz(65) - 440.5f) < 0.001f);
    /* stretch: bass flat, treble sharp */
    t.master_hz = 440.0f;
    t.stretch = STRETCH_NORMAL;
    CHECK(tuning_cents(&t, 21) < -5.0f);
    CHECK(tuning_cents(&t, 108) > 5.0f);
    /* temperament key keeps A unchanged */
    t.stretch = STRETCH_OFF;
    t.temperament = TEMP_MEANTONE;
    t.temperament_key = 0;
    CHECK(fabsf(tuning_cents(&t, 69)) < 0.001f);
    CHECK(fabsf(tuning_cents(&t, 64)) > 1.0f); /* E differs in meantone */
}

/* ---------------- voices ---------------- */
static int eng_started, eng_released, eng_killed;
static void e_start(void *c, int v, const engine_note_t *n) { (void)c; (void)v; (void)n; eng_started++; }
static void e_release(void *c, int v) { (void)c; (void)v; eng_released++; }
static void e_kill(void *c, int v) { (void)c; (void)v; eng_killed++; }

TEST(test_voice_damper_sostenuto)
{
    sound_engine_t eng = {.start = e_start, .release = e_release, .kill = e_kill};
    static voice_pool_t p;
    voice_pool_init(&p, &eng);
    eng_released = eng_killed = 0;

    voice_note_on(&p, 0, 60, 100);
    voice_pedal(&p, 0, 64, 127);
    voice_note_off(&p, 0, 60);
    CHECK_EQ(eng_released, 0); /* held by damper */
    voice_pedal(&p, 0, 64, 0);
    CHECK_EQ(eng_released, 1);

    /* sostenuto captures only the key held when it is pressed */
    eng_released = 0;
    voice_note_on(&p, 0, 48, 100);
    voice_pedal(&p, 0, 66, 127);
    voice_note_on(&p, 0, 72, 100);
    voice_note_off(&p, 0, 48);
    voice_note_off(&p, 0, 72);
    CHECK_EQ(eng_released, 1); /* 72 released, 48 sustained */
    voice_pedal(&p, 0, 66, 0);
    CHECK_EQ(eng_released, 2);
}

TEST(test_voice_stealing)
{
    sound_engine_t eng = {.start = e_start, .release = e_release, .kill = e_kill};
    static voice_pool_t p;
    voice_pool_init(&p, &eng);
    eng_killed = 0;
    for (int i = 0; i < VOICE_MAX; i++)
        voice_note_on(&p, 0, (uint8_t)(i % 128), 80);
    CHECK_EQ(voice_active_count(&p), VOICE_MAX);
    voice_note_on(&p, 1, 60, 80);
    CHECK_EQ(p.stolen, 1);
    CHECK_EQ(eng_killed, 1);
}

TEST(test_layout_modes)
{
    kb_layout_t l;
    kb_layout_default(&l);
    part_route_t r[2];
    CHECK_EQ(kb_layout_route(&l, 60, r), 1);
    CHECK_EQ(r[0].part, PART_MAIN1);
    l.mode = KBMODE_DUAL;
    CHECK_EQ(kb_layout_route(&l, 60, r), 2);
    CHECK_EQ(r[1].part, PART_LAYER);
    l.mode = KBMODE_SPLIT;
    kb_layout_route(&l, 40, r);
    CHECK_EQ(r[0].part, PART_LOWER);
    l.mode = KBMODE_4HANDS;
    l.split_point = 64;
    kb_layout_route(&l, 40, r);
    CHECK_EQ(r[0].part, PART_4H_LEFT);
    CHECK_EQ(r[0].note, 40 + 24);
    kb_layout_route(&l, 80, r);
    CHECK_EQ(r[0].part, PART_4H_RIGHT);
    CHECK_EQ(r[0].note, 80 - 24);
}

/* ---------------- metronome ---------------- */
TEST(test_metronome)
{
    metronome_t m;
    metro_event_t ev[8];
    metronome_init(&m);
    metronome_set_bpm(&m, 120);
    CHECK(metronome_set_signature(&m, 3, 4));
    CHECK(!metronome_set_signature(&m, 11, 8));
    metronome_start(&m);
    int n = metronome_advance(&m, 0, ev, 8);
    CHECK_EQ(n, 1);
    CHECK(ev[0].accent);
    int beats = 0, accents = 0;
    for (int i = 0; i < 3000; i++) { /* 3 s in 1 ms steps at 120 BPM = 6 beats */
        n = metronome_advance(&m, 1000, ev, 8);
        for (int k = 0; k < n; k++) {
            beats++;
            accents += ev[k].accent;
        }
    }
    CHECK_EQ(beats, 6);
    CHECK_EQ(accents, 2);
    uint8_t b[2];
    bpm_encode(300, b);
    CHECK_EQ(b[0], 2);
    CHECK_EQ(b[1], 44);
    CHECK_EQ(bpm_decode(b), 300);
    int num, den;
    CHECK(beat_encode(6, 8, b));
    CHECK_EQ(b[1], 3);
    CHECK(beat_decode(b, &num, &den));
    CHECK_EQ(den, 8);
}

void suite_core(void)
{
    puts("core:");
    RUN(test_ring);
    RUN(test_crc_sha);
    RUN(test_midi_running_status_and_realtime);
    RUN(test_midi_sysex);
    RUN(test_midi_sysex_overflow_dropped);
    RUN(test_scanner_velocity_and_repetition);
    RUN(test_touch_curves);
    RUN(test_pedals);
    RUN(test_tuning);
    RUN(test_voice_damper_sostenuto);
    RUN(test_voice_stealing);
    RUN(test_layout_modes);
    RUN(test_metronome);
}
