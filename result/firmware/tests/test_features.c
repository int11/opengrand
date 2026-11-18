/* Phase-2 feature tests: the whole instrument (piano_t) with a recording fake
 * engine, fake MIDI ports, RAM flash, a RAM song store (RecorderData) and a
 * RAM USB file system; plus the simulator reference synth for the sound-side
 * behaviour (damper hold, soft depth, drums). */
#include <math.h>
#include <stdlib.h>

#include "../ports/sim/synth.h"
#include "ca79ng/piano.h"
#include "test.h"

/* ---------------- fake engine ---------------- */
static struct {
    int starts, releases, sustains, drums[DRUM_COUNT];
    bool last_hold;
    float last_freq;
    uint8_t last_vel, last_part, last_msb, last_lsb, last_pc;
    int8_t last_voicing;
    int32_t param[PRM_COUNT];
    bool has_param[PRM_COUNT];
    uint8_t master;
} eng;

static void e_start(void *c, int v, const engine_note_t *n)
{
    (void)c; (void)v;
    eng.starts++;
    eng.last_freq = n->freq_hz;
    eng.last_vel = n->velocity;
    eng.last_part = n->part;
    eng.last_msb = n->msb; eng.last_lsb = n->lsb; eng.last_pc = n->program;
    eng.last_voicing = n->voicing;
}
static void e_release(void *c, int v) { (void)c; (void)v; eng.releases++; }
static void e_kill(void *c, int v) { (void)c; (void)v; }
static void e_control(void *c, uint8_t part, uint8_t cc, uint8_t val) { (void)c; if (part == PART_GLOBAL && cc == 7) eng.master = val; }
static void e_param(void *c, uint16_t id, uint8_t part, int32_t v)
{
    (void)c; (void)part;
    if (id < PRM_COUNT) { eng.param[id] = v; eng.has_param[id] = true; }
}
static void e_sustain(void *c, int v, bool hold) { (void)c; (void)v; eng.sustains++; eng.last_hold = hold; }
static void e_drum(void *c, uint8_t inst, uint8_t vel) { (void)c; (void)vel; if (inst < DRUM_COUNT) eng.drums[inst]++; }
static sound_engine_t ENGINE = {.start = e_start, .release = e_release, .kill = e_kill, .control = e_control,
                                .set_param = e_param, .sustain = e_sustain, .drum = e_drum};

/* ---------------- fake ports ---------------- */
#define OUT_MAX 3000
static struct {
    int n;
    midi_port_t port[OUT_MAX];
    uint8_t msg[OUT_MAX][288];
    uint16_t len[OUT_MAX];
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
    if (out.n < OUT_MAX) {
        out.port[out.n] = port;
        memcpy(out.msg[out.n], m, n < 288 ? n : 288);
        out.len[out.n] = n;
        out.n++;
    }
}

/* ---------------- RAM flash ---------------- */
#define FSEC 8192u
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

/* ---------------- RAM RecorderData ---------------- */
static struct {
    uint8_t data[REC_SONGS][16384];
    uint32_t len[REC_SONGS];
    int saves, erases;
} songs;

static void p_save(void *c, uint8_t s, const uint8_t *smf, uint32_t len)
{
    (void)c;
    if (s >= REC_SONGS || len > sizeof songs.data[0]) return;
    if (len) { memcpy(songs.data[s], smf, len); songs.saves++; }
    else songs.erases++;
    songs.len[s] = len;
}
static int32_t p_load(void *c, uint8_t s, uint8_t *buf, uint32_t cap)
{
    (void)c;
    if (s >= REC_SONGS || !songs.len[s] || songs.len[s] > cap) return -1;
    memcpy(buf, songs.data[s], songs.len[s]);
    return (int32_t)songs.len[s];
}

/* ---------------- RAM USB file system ---------------- */
#define RF_FILES 8
static struct {
    bool mounted;
    struct { bool used; char name[FS_NAME_MAX]; uint8_t data[8192]; uint32_t len; } f[RF_FILES];
} rfs;

static int rf_find(const char *name)
{
    for (int i = 0; i < RF_FILES; i++)
        if (rfs.f[i].used && strcmp(rfs.f[i].name, name) == 0) return i;
    return -1;
}
static bool rf_mounted(void *c) { (void)c; return rfs.mounted; }
static bool rf_list(void *c, uint32_t index, char *name, size_t cap, uint32_t *size)
{
    (void)c;
    if (!rfs.mounted) return false;
    /* index-th name in sorted order */
    const char *prev = "";
    for (uint32_t k = 0; k <= index; k++) {
        const char *best = NULL;
        int bi = -1;
        for (int i = 0; i < RF_FILES; i++)
            if (rfs.f[i].used && strcmp(rfs.f[i].name, prev) > 0 && (!best || strcmp(rfs.f[i].name, best) < 0)) {
                best = rfs.f[i].name;
                bi = i;
            }
        if (!best) return false;
        prev = best;
        if (k == index) {
            snprintf(name, cap, "%s", best);
            *size = rfs.f[bi].len;
        }
    }
    return true;
}
static int32_t rf_read(void *c, const char *name, uint8_t *buf, uint32_t cap)
{
    (void)c;
    if (!rfs.mounted) return -(int32_t)KP_RES_NO_MEDIA;
    int i = rf_find(name);
    if (i < 0) return -(int32_t)KP_RES_NOT_FOUND;
    if (rfs.f[i].len > cap) return (int32_t)cap + 1;
    memcpy(buf, rfs.f[i].data, rfs.f[i].len);
    return (int32_t)rfs.f[i].len;
}
static uint16_t rf_write(void *c, const char *name, const uint8_t *d, uint32_t len, bool ow)
{
    (void)c;
    if (!rfs.mounted) return KP_RES_NO_MEDIA;
    int i = rf_find(name);
    if (i >= 0 && !ow) return KP_RES_EXISTS;
    if (i < 0)
        for (int k = 0; k < RF_FILES && i < 0; k++)
            if (!rfs.f[k].used) i = k;
    if (i < 0 || len > sizeof rfs.f[0].data) return KP_RES_FULL;
    rfs.f[i].used = true;
    snprintf(rfs.f[i].name, FS_NAME_MAX, "%s", name);
    memcpy(rfs.f[i].data, d, len);
    rfs.f[i].len = len;
    return KP_RES_OK;
}
static uint16_t rf_remove(void *c, const char *name)
{
    (void)c;
    int i = rf_find(name);
    if (i < 0) return KP_RES_NOT_FOUND;
    rfs.f[i].used = false;
    return KP_RES_OK;
}
static uint16_t rf_rename(void *c, const char *a, const char *b)
{
    (void)c;
    int i = rf_find(a);
    if (i < 0) return KP_RES_NOT_FOUND;
    if (rf_find(b) >= 0) return KP_RES_EXISTS;
    snprintf(rfs.f[i].name, FS_NAME_MAX, "%s", b);
    return KP_RES_OK;
}
static uint16_t rf_format(void *c)
{
    (void)c;
    for (int i = 0; i < RF_FILES; i++) rfs.f[i].used = false;
    return KP_RES_OK;
}
static const hal_fs_t RFS = {NULL, rf_mounted, rf_list, rf_read, rf_write, rf_remove, rf_rename, rf_format};

/* ---------------- instrument ---------------- */
static rec_event_t pool[REC_MAX_EVENTS], stage[REC_MAX_EVENTS];
static uint8_t smf_buf[262144];
static piano_t P;
static kvstore_t KV;
static uint32_t T_US;

static void boot(bool wipe)
{
    memset(&eng, 0, sizeof eng);
    memset(&out, 0, sizeof out);
    if (wipe) {
        memset(flash_mem, 0xFF, sizeof flash_mem);
        memset(&songs, 0, sizeof songs);
        memset(&rfs, 0, sizeof rfs);
    }
    kv_mount(&KV, &FLASH, 0, 1);
    piano_platform_t plat = {.midi_tx = p_tx, .save_song = p_save, .load_song = p_load, .smf_buf = smf_buf,
                             .smf_cap = sizeof smf_buf, .usb = &RFS, .dsp_version = "TEST-DSP 2.0",
                             .fw_checksum = 0x12345678u};
    piano_init(&P, &plat, &ENGINE, &KV, pool, stage);
    piano_tick(&P, 1);
    /* the core answers only bound controller ports (stock): bind the panel and USB */
    const uint8_t sync[] = {0xF0, 0x40, 0x7F, 0x7E, 0x08, 0x02, 0x01, 0x01, 0xF7};
    piano_midi_in(&P, MIDI_PORT_PANEL, sync, sizeof sync);
    piano_midi_in(&P, MIDI_PORT_USB, sync, sizeof sync);
    ackq_n = 0;
    out.n = 0;
    T_US = 1000;
}

static void ack_drain(void)
{
    for (int i = 0; i < ackq_n; i++) {
        const uint8_t a[] = {0xF0, 0x40, 0x7F, 0x07, 0x08, 0x02, ackq[i].key[0], ackq[i].key[1], ackq[i].key[2], 0xF7};
        piano_midi_in(&P, ackq[i].port, a, sizeof a);
    }
    ackq_n = 0;
}

static void ticks(int ms)
{
    for (int i = 0; i < ms; i++) {
        piano_tick(&P, 1);
        ack_drain();
    }
}

static void sx(midi_port_t port, const uint8_t *m, uint16_t n)
{
    piano_midi_in(&P, port, m, n);
    ack_drain();
}

/* F0 40 00 fn 08 02 v1 v2 part data.. F7 */
static void kx(midi_port_t port, uint8_t fn, uint8_t v1, uint8_t v2, uint8_t part, const uint8_t *d, int n)
{
    uint8_t m[300] = {0xF0, 0x40, 0x00, fn, 0x08, 0x02, v1, v2, part};
    memcpy(m + 9, d, (size_t)n);
    m[9 + n] = 0xF7;
    sx(port, m, (uint16_t)(10 + n));
}
#define SET(port, v1, v2, part, ...) do { const uint8_t _d[] = {__VA_ARGS__}; kx(port, 0x10, v1, v2, part, _d, (int)sizeof _d); } while (0)
#define MODE(port, v1, v2, part, ...) do { const uint8_t _d[] = {__VA_ARGS__}; kx(port, 0x31, v1, v2, part, _d, (int)sizeof _d); } while (0)
#define CMD(port, v1, v2, part, ...) do { const uint8_t _d[] = {__VA_ARGS__}; kx(port, 0x32, v1, v2, part, _d, (int)sizeof _d); } while (0)

static void connect(midi_port_t port)
{
    const uint8_t sync[] = {0xF0, 0x40, 0x7F, 0x7E, 0x08, 0x02, 0x01, 0x01, 0xF7};
    sx(port, sync, sizeof sync);
    out.n = 0;
}

static void press_key(uint8_t note, uint32_t travel_us)
{
    uint8_t k = (uint8_t)(note - KB_LOWEST_NOTE);
    piano_key_edge(&P, k, 1, true, T_US);
    T_US += travel_us;
    piano_key_edge(&P, k, 2, true, T_US);
    T_US += 1000;
}
static void release_key(uint8_t note)
{
    uint8_t k = (uint8_t)(note - KB_LOWEST_NOTE);
    piano_key_edge(&P, k, 2, false, T_US);
    T_US += 1000;
    piano_key_edge(&P, k, 1, false, T_US);
    T_US += 1000;
}

static int find_msg(midi_port_t port, uint8_t fn, uint8_t v1, uint8_t v2)
{
    for (int i = 0; i < out.n; i++)
        if (out.port[i] == port && out.msg[i][1] == 0x40 && out.msg[i][3] == fn && out.msg[i][6] == v1 &&
            (v2 == 0xFF || out.msg[i][7] == v2))
            return i;
    return -1;
}
static int count_msg(midi_port_t port, uint8_t fn, uint8_t v1, uint8_t v2)
{
    int c = 0;
    for (int i = 0; i < out.n; i++)
        if (out.port[i] == port && out.msg[i][1] == 0x40 && out.msg[i][3] == fn && out.msg[i][6] == v1 &&
            (v2 == 0xFF || out.msg[i][7] == v2))
            c++;
    return c;
}
/* last Result (7E 03 ..) to port as a KP_RES value, -1 if none */
static int last_result(midi_port_t port)
{
    for (int i = out.n - 1; i >= 0; i--)
        if (out.port[i] == port && out.msg[i][3] == 0x7E && out.msg[i][6] == 0x03)
            return out.msg[i][7] << 14 | out.msg[i][8] << 7 | out.msg[i][9];
    return -1;
}
static void all_tx_within_limit(void)
{
    for (int i = 0; i < out.n; i++)
        CHECK(out.len[i] <= MIDI_SYSEX_TX_MAX);
}

/* SMF (format 0, division 500, 120 BPM -> 1 tick = 1 ms) from (ms, status, d1, d2) */
typedef struct { uint32_t ms; uint8_t st, d1, d2; } ev_t;
static size_t make_smf(const ev_t *e, int n, uint8_t *o)
{
    size_t k = 0;
    memcpy(o, "MThd\0\0\0\6\0\0\0\1\x01\xF4", 14);
    k = 14;
    memcpy(o + k, "MTrk", 4);
    size_t lenpos = k + 4;
    k += 8;
    const uint8_t tempo[] = {0x00, 0xFF, 0x51, 0x03, 0x07, 0xA1, 0x20};
    memcpy(o + k, tempo, sizeof tempo);
    k += sizeof tempo;
    uint32_t last = 0;
    for (int i = 0; i < n; i++) {
        uint32_t dt = e[i].ms - last;
        last = e[i].ms;
        uint8_t tmp[5];
        int q = 0;
        tmp[q++] = dt & 0x7F;
        while ((dt >>= 7)) tmp[q++] = (uint8_t)(0x80 | (dt & 0x7F));
        while (q) o[k++] = tmp[--q];
        o[k++] = e[i].st;
        o[k++] = e[i].d1;
        if ((e[i].st & 0xE0) != 0xC0) o[k++] = e[i].d2;
    }
    const uint8_t eot[] = {0x00, 0xFF, 0x2F, 0x00};
    memcpy(o + k, eot, 4);
    k += 4;
    uint32_t tl = (uint32_t)(k - lenpos - 4);
    o[lenpos] = (uint8_t)(tl >> 24); o[lenpos + 1] = (uint8_t)(tl >> 16); o[lenpos + 2] = (uint8_t)(tl >> 8); o[lenpos + 3] = (uint8_t)tl;
    return k;
}

/* ---- fn 0x24 helpers (controller side) ---- */
static void fa_simple(midi_port_t port, uint8_t v1)
{
    const uint8_t m[] = {0xF0, 0x40, 0x00, 0x24, 0x08, 0x02, v1, 0xF7};
    sx(port, m, sizeof m);
}
static void fa_packet(midi_port_t port, uint8_t v1, uint8_t counter, const uint8_t *raw, size_t n)
{
    uint8_t m[64] = {0xF0, 0x40, 0x00, 0x24, 0x08, 0x02, v1, counter};
    size_t o = 8 + kp_pack87(raw, n, m + 8);
    m[o] = kp_xor(m + 1, o - 1);
    m[o + 1] = 0xF7;
    sx(port, m, (uint16_t)(o + 2));
}
static void fa_name(midi_port_t port, const char *name)
{
    uint8_t raw[KP_NAME_MAX];
    size_t n = 0;
    raw[n++] = '\\';
    for (const char *c = name; *c; c++) raw[n++] = (uint8_t)*c;
    raw[n++] = 0;
    for (size_t off = 0, cnt = 0; off < n; off += 21, cnt++)
        fa_packet(port, 0x03, (uint8_t)cnt, raw + off, n - off < 21 ? n - off : 21);
}
static void fa_data(midi_port_t port, const uint8_t *d, size_t n)
{
    uint32_t len = (uint32_t)n; /* TopOfDataLength = byte count */
    const uint8_t top[] = {0xF0, 0x40, 0x00, 0x24, 0x08, 0x02, 0x01, (uint8_t)(len >> 21 & 0x7F),
                           (uint8_t)(len >> 14 & 0x7F), (uint8_t)(len >> 7 & 0x7F), (uint8_t)(len & 0x7F), 0xF7};
    sx(port, top, sizeof top);
    for (size_t off = 0, cnt = 0; off < n; off += 21, cnt++)
        fa_packet(port, 0x02, (uint8_t)cnt, d + off, n - off < 21 ? n - off : 21);
    fa_simple(port, 0x7B);
}
/* payload of a piano 02/03 packet at out index i */
static size_t fa_payload(int i, uint8_t *dst)
{
    return kp_unpack78(out.msg[i] + 8, (size_t)(out.len[i] - 10), dst);
}

/* ================= tests ================= */

TEST(test_damper_hold_engine_and_synth)
{
    boot(true);
    press_key(60, 3000);
    piano_pedal(&P, PEDAL_DAMPER, P.pedals.adc_max[PEDAL_DAMPER]);
    release_key(60);
    CHECK_EQ(eng.sustains, 1);
    CHECK(!eng.last_hold); /* Damper Hold Off by default */
    CHECK_EQ(eng.releases, 0);
    SET(MIDI_PORT_PANEL, 0x50, 0x20, 0x0F, 0x01); /* DamperHold On */
    CHECK_EQ(eng.sustains, 2);
    CHECK(eng.last_hold);
    CHECK(P.voices.damper_hold);

    /* reference synth: an organ held by the damper decays with Hold Off */
    static synth_t s;
    sound_engine_t e;
    synth_init(&s, 8000.0f);
    synth_bind(&s, &e);
    engine_note_t organ = {.freq_hz = 220.0f, .velocity = 100, .part = 0, .msb = 121, .lsb = 0, .program = 17};
    e.start(e.ctx, 0, &organ);
    e.start(e.ctx, 1, &organ);
    static float buf[2 * 8000];
    synth_render(&s, buf, 800, NULL, 0);
    float start_env = s.v[0].env;
    e.sustain(e.ctx, 0, false);
    e.sustain(e.ctx, 1, true);
    for (int k = 0; k < 6; k++)
        synth_render(&s, buf, 8000, NULL, 0); /* 6 s */
    CHECK(start_env > 0.9f);
    CHECK(!s.v[0].on || s.v[0].env < 0.1f); /* decayed naturally */
    CHECK(s.v[1].on && s.v[1].env > 0.9f);  /* held */
}

TEST(test_soft_pedal_depth)
{
    boot(true);
    SET(MIDI_PORT_PANEL, 0x50, 0x12, 0x00, 127); /* depth 10 */
    CHECK_EQ(eng.param[PRM_SOFT_PEDAL_DEPTH], 10);
    CHECK_EQ(P.pedals.soft_depth, 10);
    SET(MIDI_PORT_PANEL, 0x50, 0x12, 0x00, 12); /* depth 1 */
    CHECK_EQ(eng.param[PRM_SOFT_PEDAL_DEPTH], 1);

    static synth_t s;
    sound_engine_t e;
    synth_init(&s, 8000.0f);
    synth_bind(&s, &e);
    e.control(e.ctx, 0, 67, 127);
    e.set_param(e.ctx, PRM_SOFT_PEDAL_DEPTH, 0, 1);
    float g1 = synth_soft_gain(&s);
    e.set_param(e.ctx, PRM_SOFT_PEDAL_DEPTH, 0, 10);
    float g10 = synth_soft_gain(&s);
    CHECK(g1 > 0.9f);
    CHECK(g10 < 0.5f);
    e.control(e.ctx, 0, 67, 0);
    CHECK(synth_soft_gain(&s) == 1.0f);
}

TEST(test_hammer_delay)
{
    boot(true);
    SET(MIDI_PORT_PANEL, 0x50, 0x07, 0x00, 127); /* Hammer Delay 10 */
    CHECK_EQ(P.hammer_delay, 10);
    press_key(60, 100000); /* very soft */
    CHECK_EQ(eng.starts, 0);
    ticks(10);
    CHECK_EQ(eng.starts, 0);
    ticks(25);
    CHECK_EQ(eng.starts, 1);
    release_key(60);
    /* fortissimo is not delayed */
    press_key(62, 1000);
    CHECK_EQ(eng.starts, 2);
    release_key(62);
    /* released before the delayed strike: it still sounds, then stops */
    int rel = eng.releases;
    press_key(64, 100000);
    release_key(64);
    CHECK_EQ(eng.starts, 3);
    CHECK(eng.releases > rel);
    /* Off: immediate */
    SET(MIDI_PORT_PANEL, 0x50, 0x07, 0x00, 0);
    press_key(65, 100000);
    CHECK_EQ(eng.starts, 4);
}

TEST(test_user_touch_curve)
{
    boot(true);
    connect(MIDI_PORT_PANEL);
    SET(MIDI_PORT_PANEL, 0x50, 0x00, 0x00, 0x40); /* TouchCurve = User */
    /* original bulk form: constant output 100 */
    uint8_t bulk[257];
    bulk[0] = 0x7F;
    for (int i = 0; i < 128; i++) { bulk[1 + 2 * i] = 0; bulk[2 + 2 * i] = 100; }
    out.n = 0;
    kx(MIDI_PORT_PANEL, 0x10, 0x5A, 0x0D, 0x7F, bulk, 257);
    CHECK_EQ(out.msg[0][3], 0x07); /* ACKed */
    press_key(60, 3000);
    CHECK_EQ(eng.last_vel, 100);
    press_key(62, 60000);
    CHECK_EQ(eng.last_vel, 100);
    /* request-all: name + the original single BulkOperationPoint (267 B, within the
     * original app's 511-byte limit) + end of data */
    out.n = 0;
    const uint8_t rq[] = {0xF0, 0x40, 0x00, 0x02, 0x08, 0x02, 0x5A, 0x7F, 0x7F, 0xF7};
    sx(MIDI_PORT_PANEL, rq, sizeof rq);
    CHECK(find_msg(MIDI_PORT_PANEL, 0x10, 0x5A, 0x0C) >= 0);
    int d = find_msg(MIDI_PORT_PANEL, 0x10, 0x5A, 0x0D);
    CHECK(d >= 0);
    if (d >= 0) {
        CHECK_EQ(out.len[d], 7 + 4 + 2 * 128);
        CHECK_EQ(out.msg[d][10 + 1], 100); /* point 0: (exist, vel) -> vel 100 */
    }
    CHECK_EQ(count_msg(MIDI_PORT_PANEL, 0x10, 0x5A, 0x05), 0); /* chunked form is receive-only */
    int eod = find_msg(MIDI_PORT_PANEL, 0x03, 0x5A, 0x7F);
    CHECK(eod == out.n - 1);
    all_tx_within_limit();
    /* save, then an unsaved one-point edit is dropped by EditExit */
    CMD(MIDI_PORT_PANEL, 0x5A, 0x0A, 0x7F);
    SET(MIDI_PORT_PANEL, 0x5A, 0x07, 0x7F, 0, 64, 1, 20); /* point at 64 -> 20 */
    CHECK_EQ(P.touch.user_table[64], 20);
    CHECK(P.touch.user_table[32] < 100); /* interpolated 0..20 */
    MODE(MIDI_PORT_PANEL, 0x5A, 0x09, 0x7F);
    CHECK_EQ(P.touch.user_table[64], 100);
    /* persisted across power cycles */
    boot(false);
    /* the curve data is kept; the TouchCurve selection itself is a startup setting */
    CHECK_EQ(P.vt.curve[60], 100);
    SET(MIDI_PORT_PANEL, 0x50, 0x00, 0x00, 0x40);
    press_key(60, 3000);
    CHECK_EQ(eng.last_vel, 100);
    /* chunked OpenGrand form from a controller */
    uint8_t chunk[2 + 64];
    chunk[0] = 0x7F;
    for (int off = 0; off < 128; off += 32) {
        chunk[1] = (uint8_t)off;
        for (int i = 0; i < 32; i++) { chunk[2 + 2 * i] = 0; chunk[3 + 2 * i] = (uint8_t)(off + i < 64 ? 50 : 120); }
        kx(MIDI_PORT_PANEL, 0x10, 0x5A, 0x05, 0x7F, chunk, sizeof chunk);
    }
    SET(MIDI_PORT_PANEL, 0x5A, 0x06, 0x7F);
    CHECK_EQ(P.touch.user_table[10], 50);
    CHECK_EQ(P.touch.user_table[100], 120);
    /* learn: soft..strong strokes spread over the whole range */
    connect(MIDI_PORT_PANEL);
    CMD(MIDI_PORT_PANEL, 0x5A, 0x0B, 0x7F, 0x7F, 1);
    for (int i = 0; i < 6; i++) { press_key((uint8_t)(48 + i), (uint32_t)(4000 + i * 6000)); release_key((uint8_t)(48 + i)); }
    CMD(MIDI_PORT_PANEL, 0x5A, 0x0B, 0x7F, 0x7F, 2);
    CHECK(!P.vt.learning);
    CHECK_EQ(P.touch.user_table[P.vt.learn_max], 127);
    CHECK_EQ(P.touch.user_table[P.vt.learn_min], 1);
    CHECK(find_msg(MIDI_PORT_PANEL, 0x10, 0x5A, 0x0D) >= 0); /* new curve pushed */
}

TEST(test_pernote_vt)
{
    boot(true);
    connect(MIDI_PORT_PANEL);
    connect(MIDI_PORT_USB);
    /* tuning: User stretch, +40 cents on A4 */
    SET(MIDI_PORT_PANEL, 0x50, 0x0B, 0x00, 0x40);
    uint8_t d[130];
    d[0] = 0x7F; d[1] = 0x7F;
    for (int i = 0; i < 128; i++) d[2 + i] = 64;
    d[2 + 69] = 64 + 40;
    kx(MIDI_PORT_PANEL, 0x10, 0x50, 0x0C, 0x7F, d, 130);
    press_key(69, 3000);
    CHECK(fabsf(eng.last_freq - 440.0f * powf(2.0f, 40.0f / 1200.0f)) < 0.5f);
    release_key(69);
    CMD(MIDI_PORT_PANEL, 0x50, 0x0C, 0x7F, 0x00); /* save */
    d[2 + 69] = 64 - 30;                           /* unsaved edit */
    kx(MIDI_PORT_PANEL, 0x10, 0x50, 0x0C, 0x7F, d, 130);
    CHECK_EQ(P.tuning.per_note_cents[69], -30);
    out.n = 0;
    MODE(MIDI_PORT_PANEL, 0x50, 0x0C, 0x7F, 0x00); /* exit without saving */
    CHECK_EQ(P.tuning.per_note_cents[69], 40);
    CHECK(find_msg(MIDI_PORT_USB, 0x31, 0x50, 0x0C) >= 0);   /* other controller closes its editor */
    CHECK_EQ(find_msg(MIDI_PORT_PANEL, 0x31, 0x50, 0x0C), -1);
    /* request returns the data then 03 50 0C 7F */
    out.n = 0;
    const uint8_t rq[] = {0xF0, 0x40, 0x00, 0x02, 0x08, 0x02, 0x50, 0x0C, 0x7F, 0xF7};
    sx(MIDI_PORT_PANEL, rq, sizeof rq);
    int i = find_msg(MIDI_PORT_PANEL, 0x10, 0x50, 0x0C);
    CHECK(i >= 0);
    if (i >= 0) {
        CHECK_EQ(out.len[i], 3 + 6 + 2 + 128 + 1);
        CHECK_EQ(out.msg[i][9 + 2 + 69], 104);
    }
    CHECK(find_msg(MIDI_PORT_PANEL, 0x03, 0x50, 0x0C) >= 0);
    /* key volume: User, -50 on C4 */
    SET(MIDI_PORT_PANEL, 0x50, 0x0F, 0x00, 0x40);
    for (int k = 0; k < 128; k++) d[2 + k] = 64;
    press_key(60, 3000);
    uint8_t v_ref = eng.last_vel;
    release_key(60);
    d[2 + 60] = 64 - 50;
    kx(MIDI_PORT_PANEL, 0x10, 0x50, 0x10, 0x7F, d, 130);
    press_key(60, 3000);
    CHECK(eng.last_vel < v_ref * 0.4f);
    release_key(60);
    /* temperament: User, +20 on E (pitch class 4), key C */
    SET(MIDI_PORT_PANEL, 0x50, 0x0D, 0x00, 0x40, 0x00);
    uint8_t t[14] = {0x7F, 0x7F, 64, 64, 64, 64, 84, 64, 64, 64, 64, 64, 64, 64};
    kx(MIDI_PORT_PANEL, 0x10, 0x50, 0x0E, 0x7F, t, 14);
    CHECK_EQ(P.tuning.user_temperament[4], 20);
    /* voicing: User, +3 on G4 goes to the engine */
    SET(MIDI_PORT_PANEL, 0x50, 0x01, 0x00, 0x40);
    for (int k = 0; k < 128; k++) d[2 + k] = 64;
    d[2 + 67] = 67;
    kx(MIDI_PORT_PANEL, 0x10, 0x50, 0x17, 0x7F, d, 130);
    press_key(67, 3000);
    CHECK_EQ(eng.last_voicing, 3);
    CMD(MIDI_PORT_PANEL, 0x50, 0x17, 0x7F, 0x00);
    boot(false);
    CHECK_EQ(P.tuning.per_note_cents[69], 40); /* saved tuning survives */
    CHECK_EQ(P.vt.voicing[67], 3);
    CHECK_EQ(P.vt.key_volume[60], 0);          /* key volume was never saved */
}

TEST(test_recorder_persistence_and_reset)
{
    boot(true);
    connect(MIDI_PORT_PANEL);
    CMD(MIDI_PORT_PANEL, 0x52, 0x08, 0x7F, 0x03); /* RecStandby */
    press_key(60, 3000);
    ticks(300);
    release_key(60);
    ticks(100);
    CMD(MIDI_PORT_PANEL, 0x52, 0x08, 0x7F, 0x02); /* stop: commit */
    ticks(1);
    CHECK_EQ(songs.saves, 1);
    CHECK(songs.len[0] > 14);
    CHECK_MEM(songs.data[0], "MThd", 4);
    boot(false);
    CHECK_EQ(P.rec.song[0].count, 2);
    CHECK(!P.rec.song[0].ext);
    /* playback after power cycle */
    int s0 = eng.starts;
    CMD(MIDI_PORT_PANEL, 0x52, 0x08, 0x7F, 0x01);
    ticks(500);
    CHECK_EQ(eng.starts - s0, 1);
    /* InternalSongDelete song 1 (index 0), both parts */
    connect(MIDI_PORT_PANEL);
    CMD(MIDI_PORT_PANEL, 0x52, 0x21, 0x7F, 0x04, 0x00, 0x00, 0x7F);
    ticks(1);
    CHECK_EQ(P.rec.song[0].count, 0);
    CHECK_EQ(songs.len[0], 0);
    CHECK_EQ(songs.erases, 1);
    int i = find_msg(MIDI_PORT_PANEL, 0x32, 0x52, 0x22);
    CHECK(i >= 0);
    if (i >= 0) CHECK_EQ(out.msg[i][9], 0x00);
    /* delete all (7F 7F 7F 7F accepted despite the descriptor mask) */
    recorder_select(&P.rec, 3);
    recorder_standby(&P.rec, 0, 120, 4, 4);
    recorder_start(&P.rec);
    recorder_input(&P.rec, 0x90, 64, 80);
    recorder_stop(&P.rec);
    ticks(1);
    CHECK(songs.len[3] > 0);
    out.n = 0;
    CMD(MIDI_PORT_PANEL, 0x52, 0x21, 0x7F, 0x7F, 0x7F, 0x7F, 0x7F);
    ticks(1);
    CHECK_EQ(P.rec.song[3].count, 0);
    CHECK_EQ(songs.len[3], 0);
    i = find_msg(MIDI_PORT_PANEL, 0x32, 0x52, 0x22);
    CHECK(i >= 0);
    if (i >= 0) CHECK_EQ(out.msg[i][10], 0x7F);
}

TEST(test_rhythm_patterns_and_drums)
{
    static drum_pattern_t pt[RHYTHM_COUNT];
    int unique = 0;
    for (unsigned r = 0; r < RHYTHM_COUNT; r++) {
        rhythm_pattern(r, &pt[r]);
        CHECK_EQ(pt[r].beats, RHYTHM_TABLE[r].numerator);
        CHECK(pt[r].steps_per_beat == 2 || pt[r].steps_per_beat == 3 || pt[r].steps_per_beat == 4 || pt[r].steps_per_beat == 6);
        CHECK(pt[r].steps_per_beat * pt[r].beats <= 32);
        uint32_t any = 0;
        for (int k = 0; k < DRUM_PATTERN_INSTS; k++) any |= pt[r].hit[k];
        CHECK(any & 1u); /* something on the downbeat */
        bool dup = false;
        for (unsigned q = 0; q < r && !dup; q++)
            dup = memcmp(&pt[q], &pt[r], sizeof pt[r]) == 0;
        unique += !dup;
    }
    CHECK(unique >= 80);
    boot(true);
    MODE(MIDI_PORT_PANEL, 0x56, 0x0A, 0x7F, 0x01); /* Rhythm mode */
    SET(MIDI_PORT_PANEL, 0x56, 0x09, 0x7F, 0);      /* 8 Beat 1 */
    CMD(MIDI_PORT_PANEL, 0x56, 0x08, 0x7F, 0x01);   /* start, 120 BPM */
    ticks(4000);                                    /* two bars + downbeat */
    CHECK(eng.drums[DRUM_KICK] >= 4);
    CHECK(eng.drums[DRUM_SNARE] >= 2);
    CHECK(eng.drums[DRUM_HAT] + eng.drums[DRUM_RIDE] >= 8);
    /* a waltz rhythm switches the bar to 3/4 */
    int waltz = -1;
    for (unsigned r = 0; r < RHYTHM_COUNT; r++)
        if (RHYTHM_TABLE[r].numerator == 3) { waltz = (int)r; break; }
    CHECK(waltz >= 0);
    SET(MIDI_PORT_PANEL, 0x56, 0x09, 0x7F, (uint8_t)waltz);
    CHECK_EQ(P.metro.numerator, 3);
    /* the reference synth renders a drum hit */
    static synth_t s;
    synth_init(&s, 8000.0f);
    synth_drum(&s, DRUM_KICK, 120);
    synth_drum(&s, DRUM_HAT, 100);
    static float buf[2 * 800];
    synth_render(&s, buf, 800, NULL, 0);
    float e = 0;
    for (int k = 0; k < 1600; k++) e += buf[k] * buf[k];
    CHECK(e > 0.01f);
}

static void upload_song(const ev_t *e, int n)
{
    static uint8_t smf[4096];
    size_t len = make_smf(e, n, smf);
    fa_data(MIDI_PORT_PANEL, smf, len);
}

TEST(test_song_tempo_transpose_parts)
{
    boot(true);
    connect(MIDI_PORT_PANEL);
    const ev_t song[] = {{0, 0x90, 60, 100}, {0, 0x91, 48, 90}, {400, 0x80, 60, 0}, {400, 0x81, 48, 0},
                         {1000, 0x90, 62, 100}, {1200, 0x80, 62, 0}};
    upload_song(song, 6); /* anonymous upload = play from the USB slot */
    CHECK_EQ(last_result(MIDI_PORT_PANEL), KP_RES_OK);
    CHECK_EQ(P.rec.cur_song, REC_SLOT_EXT);
    CHECK(P.rec.song[REC_SLOT_EXT].ext);
    CHECK_EQ(P.rec.song[REC_SLOT_EXT].bpm, 120);
    CHECK(find_msg(MIDI_PORT_PANEL, 0x10, 0x52, 0x02) >= 0); /* MusicTempo of the song */
    /* double tempo: the note at 1000 ms sounds after ~500 ms */
    SET(MIDI_PORT_PANEL, 0x52, 0x02, 0x7F, 240 >> 7, 240 & 0x7F);
    CMD(MIDI_PORT_PANEL, 0x52, 0x08, 0x7F, 0x01);
    ticks(1);
    CHECK_EQ(eng.starts, 2);
    CHECK_EQ(eng.last_part, PART_MIDI_BASE + 1);
    ticks(510);
    CHECK_EQ(eng.starts, 3);
    CHECK(fabsf(eng.last_freq - tuning_freq(&P.tuning, 62)) < 0.01f);
    CMD(MIDI_PORT_PANEL, 0x52, 0x08, 0x7F, 0x02);
    /* song transpose +2 (TransposeValue on part 0x7F), key transpose untouched */
    SET(MIDI_PORT_PANEL, 0x53, 0x02, 0x7F, 66);
    CHECK_EQ(P.song_transpose, 2);
    CHECK_EQ(P.tuning.transpose, 0);
    /* Part2 only (left hand = channel 2) */
    SET(MIDI_PORT_PANEL, 0x52, 0x01, 0x7F, 0x01);
    eng.starts = 0;
    CMD(MIDI_PORT_PANEL, 0x52, 0x08, 0x7F, 0x01);
    ticks(1);
    CHECK_EQ(eng.starts, 1);
    CHECK_EQ(eng.last_part, PART_MIDI_BASE + 1);
    CHECK(fabsf(eng.last_freq - tuning_freq(&P.tuning, 50)) < 0.01f); /* 48 + 2 */
    ticks(1000);
    CHECK_EQ(eng.starts, 1); /* Part1 notes muted */
}

TEST(test_lesson_ab_repeat)
{
    boot(true);
    connect(MIDI_PORT_PANEL);
    const ev_t song[] = {{100, 0x90, 60, 100}, {200, 0x80, 60, 0}, {500, 0x90, 64, 100}, {600, 0x80, 64, 0},
                         {900, 0x90, 67, 100}, {1000, 0x80, 67, 0}};
    upload_song(song, 6);
    MODE(MIDI_PORT_PANEL, 0x52, 0x1D, 0x7F, 0x04); /* Lesson */
    CHECK_EQ(P.music_mode, MM_LESSON);
    CHECK_EQ(P.rec.cur_song, REC_SLOT_EXT);
    CMD(MIDI_PORT_PANEL, 0x52, 0x08, 0x7F, 0x01);
    ticks(300);
    CMD(MIDI_PORT_PANEL, 0x52, 0x06, 0x7F, 0x01); /* A at 300 */
    ticks(400);
    CMD(MIDI_PORT_PANEL, 0x52, 0x06, 0x7F, 0x02); /* B at 700, loop */
    int s = eng.starts;
    ticks(2000); /* 5 loops of 400 ms, each with the note at 500 */
    CHECK(eng.starts - s >= 4);
    CHECK_EQ(P.rec.state, REC_PLAYING);
    /* half tempo slows the loop down */
    SET(MIDI_PORT_PANEL, 0x52, 0x02, 0x7F, 0, 60);
    s = eng.starts;
    ticks(2000);
    CHECK(eng.starts - s <= 3);
}

TEST(test_concert_magic)
{
    boot(true);
    connect(MIDI_PORT_PANEL);
    /* melody on channel 1, accompaniment on channel 2 */
    const ev_t song[] = {{0, 0x90, 60, 100}, {250, 0x91, 36, 80}, {400, 0x80, 60, 0}, {500, 0x90, 62, 100},
                         {900, 0x80, 62, 0}, {1000, 0x90, 64, 100}, {1010, 0x90, 67, 50}, {1400, 0x80, 64, 0},
                         {1400, 0x80, 67, 0}, {1500, 0x81, 36, 0}};
    upload_song(song, 10);
    MODE(MIDI_PORT_PANEL, 0x52, 0x1D, 0x7F, 0x05);
    CMD(MIDI_PORT_PANEL, 0x52, 0x08, 0x7F, 0x01); /* arm */
    CHECK(P.cm.active);
    out.n = 0;
    press_key(72, 3000); /* any key plays the first melody note */
    CHECK_EQ(eng.starts, 1);
    CHECK_EQ(eng.last_part, PART_MIDI_BASE);
    CHECK(fabsf(eng.last_freq - tuning_freq(&P.tuning, 60)) < 0.01f);
    uint8_t vel1 = eng.last_vel;
    int i = find_msg(MIDI_PORT_PANEL, 0x10, 0x52, 0x0B);
    CHECK(i >= 0);
    if (i >= 0) { CHECK_EQ(out.msg[i][9], CM_OK); CHECK_EQ(out.msg[i][11], 1); }
    release_key(72);
    CHECK(eng.releases >= 1);
    /* Ensemble: the accompaniment (ch 2, at 250 ms) follows on the clock */
    ticks(300);
    CHECK_EQ(eng.starts, 2);
    CHECK_EQ(eng.last_part, PART_MIDI_BASE + 1);
    ticks(200);           /* ~500 ms after the first tap = song tempo */
    out.n = 0;
    press_key(74, 3000);
    CHECK_EQ(eng.starts, 3);
    CHECK_EQ(P.cm.last, CM_OK);
    release_key(74);
    /* tapping far too early -> TOOFAST, the chord is still played */
    ticks(100);
    press_key(76, 1500); /* strong */
    CHECK_EQ(P.cm.last, CM_TOOFAST);
    CHECK_EQ(eng.starts, 5);
    CHECK(eng.last_vel < 127 / 2 + 1); /* second chord note keeps its 50/100 balance */
    CHECK(P.cm.ratio_q16 > 65536);          /* tempo estimate moved up, clamped */
    CHECK(P.cm.ratio_q16 <= 98304);
    release_key(76);
    /* end of song -> NG */
    press_key(77, 3000);
    CHECK_EQ(P.cm.last, CM_NG);
    CHECK_EQ(eng.starts, 5);
    (void)vel1;
    /* Melody mode via the extension setting: no accompaniment clock */
    SET(MIDI_PORT_PANEL, 0x52, 0x0D, 0x7F, 0, 0, 0);
    CHECK_EQ(P.cm.mode, CM_MODE_MELODY);
    CMD(MIDI_PORT_PANEL, 0x52, 0x08, 0x7F, 0x01);
    CHECK(P.rec.state != REC_PLAYING);
    int s = eng.starts;
    press_key(60, 3000);
    ticks(600);
    CHECK_EQ(eng.starts - s, 1);
    /* keys play normally again after leaving the mode */
    MODE(MIDI_PORT_PANEL, 0x52, 0x1D, 0x7F, 0x00);
    s = eng.starts;
    press_key(65, 3000);
    CHECK_EQ(eng.starts - s, 1);
    CHECK_EQ(eng.last_part, PART_MAIN1);
}

static int emit_n;
static void emit(void *ctx, const rec_event_t *e, uint8_t v)
{
    (void)ctx; (void)e; (void)v;
    emit_n++;
}

TEST(test_concert_magic_tempo_logic)
{
    /* pure module: slow taps -> SLOW, chord groups -> CHORD */
    static rec_event_t ev[6] = {{0, 0x90, 60, 100, 0}, {500, 0x90, 62, 100, 0}, {1000, 0x90, 64, 100, 0},
                                {1005, 0x90, 67, 60, 0}, {1500, 0x90, 65, 90, 0}, {2000, 0x90, 60, 90, 1}};
    static concert_t c;
    memset(&c, 0, sizeof c);
    c.mode = CM_MODE_MELODY;
    cm_start(&c, ev, 6, false);
    emit_n = 0;
    CHECK_EQ(cm_key_on(&c, ev, 6, 60, 90, 0, emit, NULL), CM_OK);
    CHECK_EQ(cm_key_on(&c, ev, 6, 61, 90, 1000, emit, NULL), CM_SLOW);  /* 500 song ms in 1000 ms */
    CHECK(c.ratio_q16 == 32768);                                          /* clamped to 0.5 */
    CHECK_EQ(cm_key_on(&c, ev, 6, 62, 90, 1500, emit, NULL), CM_CHORD); /* 500 in 500 + 2 notes */
    CHECK_EQ(cm_key_on(&c, ev, 6, 63, 90, 1800, emit, NULL), CM_FAST);  /* 500 in 300 */
    CHECK_EQ(cm_key_on(&c, ev, 6, 64, 90, 1900, emit, NULL), CM_NG);    /* part 1 note is not a trigger */
    CHECK_EQ(emit_n, 5); /* 5 note-ons (chord = 2); different keys, so no key-offs */
}

TEST(test_usb_file_access)
{
    boot(true);
    rfs.mounted = true;
    connect(MIDI_PORT_PANEL);
    ticks(150);
    int i = find_msg(MIDI_PORT_PANEL, 0x10, 0x51, 0x2E); /* USBMemory Attach */
    CHECK(i >= 0);
    if (i >= 0) CHECK_EQ(out.msg[i][9], 1);
    const ev_t song[] = {{0, 0x90, 60, 100}, {300, 0x80, 60, 0}};
    uint8_t smf[512];
    size_t sl = make_smf(song, 2, smf);
    rf_write(NULL, "B.MID", smf, (uint32_t)sl, true);
    rf_write(NULL, "A.TXT", (const uint8_t *)"hello", 5, true);

    /* list: the request is acked first (as the stock does), then one name per Ack, then End of list */
    out.n = 0;
    fa_simple(MIDI_PORT_PANEL, 0x11);
    CHECK_EQ(out.n, 2);
    CHECK_EQ(out.msg[0][6], 0x7F);
    uint8_t raw[64];
    size_t n = fa_payload(1, raw);
    CHECK_EQ(out.msg[1][6], 0x03);
    CHECK(n == 7 && memcmp(raw, "\\A.TXT", 7) == 0);
    fa_simple(MIDI_PORT_PANEL, 0x7E); /* NAK -> same packet again */
    CHECK_EQ(out.n, 3);
    CHECK_MEM(out.msg[2], out.msg[1], out.len[1]);
    fa_simple(MIDI_PORT_PANEL, 0x7F);
    n = fa_payload(3, raw);
    CHECK(n == 7 && memcmp(raw, "\\B.MID", 7) == 0);
    fa_simple(MIDI_PORT_PANEL, 0x7F);
    CHECK_EQ(out.msg[4][6], 0x1F);
    fa_simple(MIDI_PORT_PANEL, 0x7F); /* the controller acks End of list too (stock waits for it) */
    CHECK_EQ(out.n, 5);

    /* read: request acked, name acked, TopOfData waits for its Ack, then the data, then EndOfData */
    out.n = 0;
    fa_simple(MIDI_PORT_PANEL, 0x04);
    CHECK_EQ(out.msg[0][6], 0x7F); /* request acked */
    fa_name(MIDI_PORT_PANEL, "A.TXT");
    CHECK_EQ(out.msg[1][6], 0x7F); /* name packet acked */
    CHECK_EQ(out.msg[2][6], 0x01); /* TopOfDataLength: 5 bytes */
    CHECK_EQ(out.msg[2][10], 5);
    CHECK_EQ(out.n, 3);            /* nothing more until TopOfData is acked */
    fa_simple(MIDI_PORT_PANEL, 0x7E); /* NAK -> TopOfData again */
    CHECK_EQ(out.n, 4);
    CHECK_MEM(out.msg[3], out.msg[2], out.len[2]);
    fa_simple(MIDI_PORT_PANEL, 0x7F);
    CHECK_EQ(out.msg[4][6], 0x02);
    CHECK_EQ(out.msg[4][out.len[4] - 2], kp_xor(out.msg[4] + 1, out.len[4] - 3));
    n = fa_payload(4, raw);
    CHECK(n == 5 && memcmp(raw, "hello", 5) == 0);
    fa_simple(MIDI_PORT_PANEL, 0x7F);
    CHECK_EQ(out.msg[5][6], 0x7B);
    fa_simple(MIDI_PORT_PANEL, 0x7F); /* ack of EndOfData: stream already closed, ignored */
    CHECK_EQ(out.n, 6);

    /* write */
    out.n = 0;
    const uint8_t wr[] = {0xF0, 0x40, 0x00, 0x24, 0x08, 0x02, 0x05, 0x00, 0x00, 0xF7};
    sx(MIDI_PORT_PANEL, wr, sizeof wr);
    fa_name(MIDI_PORT_PANEL, "C.TXT");
    fa_data(MIDI_PORT_PANEL, (const uint8_t *)"0123456789abcdefghijklmnopq", 27);
    CHECK_EQ(last_result(MIDI_PORT_PANEL), KP_RES_OK);
    i = rf_find("C.TXT");
    CHECK(i >= 0);
    if (i >= 0) CHECK(rfs.f[i].len == 27 && memcmp(rfs.f[i].data, "0123456789abcdefghijklmnopq", 27) == 0);
    /* same name again -> exists */
    sx(MIDI_PORT_PANEL, wr, sizeof wr);
    fa_name(MIDI_PORT_PANEL, "C.TXT");
    fa_data(MIDI_PORT_PANEL, (const uint8_t *)"x", 1);
    CHECK_EQ(last_result(MIDI_PORT_PANEL), KP_RES_EXISTS);

    /* rename + delete */
    fa_simple(MIDI_PORT_PANEL, 0x07);
    fa_name(MIDI_PORT_PANEL, "C.TXT");
    fa_name(MIDI_PORT_PANEL, "D.TXT");
    CHECK_EQ(last_result(MIDI_PORT_PANEL), KP_RES_OK);
    CHECK(rf_find("D.TXT") >= 0 && rf_find("C.TXT") < 0);
    fa_simple(MIDI_PORT_PANEL, 0x06);
    fa_name(MIDI_PORT_PANEL, "D.TXT");
    CHECK_EQ(last_result(MIDI_PORT_PANEL), KP_RES_OK);
    fa_simple(MIDI_PORT_PANEL, 0x06);
    fa_name(MIDI_PORT_PANEL, "D.TXT");
    CHECK_EQ(last_result(MIDI_PORT_PANEL), KP_RES_NOT_FOUND);

    /* select + Piano Music playback */
    fa_simple(MIDI_PORT_PANEL, 0x08);
    fa_name(MIDI_PORT_PANEL, "B.MID");
    CHECK_EQ(last_result(MIDI_PORT_PANEL), KP_RES_OK);
    CHECK_EQ(P.usb_selected, 1);
    MODE(MIDI_PORT_PANEL, 0x52, 0x1D, 0x7F, 0x06);
    CMD(MIDI_PORT_PANEL, 0x52, 0x08, 0x7F, 0x01);
    ticks(2);
    CHECK_EQ(eng.starts, 1);
    CHECK_EQ(eng.last_part, PART_MIDI_BASE);
    CMD(MIDI_PORT_PANEL, 0x52, 0x08, 0x7F, 0x02);
    /* SongSelect by file list id (numType 1) */
    SET(MIDI_PORT_PANEL, 0x52, 0x00, 0x7F, 0x01, 0x00, 0x00); /* A.TXT: not an SMF */
    CHECK_EQ(last_result(MIDI_PORT_PANEL), KP_RES_SMF_FORMAT);
    SET(MIDI_PORT_PANEL, 0x52, 0x00, 0x7F, 0x01, 0x00, 0x01);
    CHECK_EQ(P.usb_selected, 1);

    /* save an internal song to USB, then load it back into song 3 */
    recorder_select(&P.rec, 1);
    recorder_standby(&P.rec, 0, 120, 4, 4);
    recorder_start(&P.rec);
    recorder_input(&P.rec, 0x90, 64, 80);
    recorder_tick(&P.rec, 100);
    recorder_input(&P.rec, 0x80, 64, 0);
    recorder_stop(&P.rec);
    CMD(MIDI_PORT_PANEL, 0x52, 0x20, 0x7F, 0x01, 0x00, 0x01, 0x7F);
    CHECK_EQ(last_result(MIDI_PORT_PANEL), KP_RES_OK);
    CHECK(rf_find("SONG_02.MID") >= 0);
    CMD(MIDI_PORT_PANEL, 0x52, 0x20, 0x7F, 0x01, 0x00, 0x01, 0x7F);
    CHECK(rf_find("SONG_02_1.MID") >= 0);
    SET(MIDI_PORT_PANEL, 0x52, 0x00, 0x7F, 0x00, 0x00, 0x03);
    CHECK_EQ(P.rec.cur_song, 3);
    fa_simple(MIDI_PORT_PANEL, 0x09);
    fa_name(MIDI_PORT_PANEL, "SONG_02.MID");
    CHECK_EQ(last_result(MIDI_PORT_PANEL), KP_RES_OK);
    CHECK_EQ(P.rec.song[3].count, 2);
    ticks(1);
    CHECK(songs.len[3] > 0); /* imported song persisted */

    /* the stock request forms: 24 0A <song> <type 02 = MID> + name saves an
     * internal song (written over), 24 09 <song> <type> + name imports into that song; KSO (03) is refused */
    const uint8_t sv[] = {0xF0, 0x40, 0x00, 0x24, 0x08, 0x02, 0x0A, 0x01, 0x02, 0xF7};
    sx(MIDI_PORT_PANEL, sv, sizeof sv);
    fa_name(MIDI_PORT_PANEL, "S2.MID");
    CHECK_EQ(last_result(MIDI_PORT_PANEL), KP_RES_OK);
    CHECK(rf_find("S2.MID") >= 0);
    sx(MIDI_PORT_PANEL, sv, sizeof sv);
    fa_name(MIDI_PORT_PANEL, "S2.MID");
    CHECK_EQ(last_result(MIDI_PORT_PANEL), KP_RES_OK); /* same name again: written over like the stock */
    const uint8_t ld[] = {0xF0, 0x40, 0x00, 0x24, 0x08, 0x02, 0x09, 0x05, 0x02, 0xF7};
    sx(MIDI_PORT_PANEL, ld, sizeof ld);
    fa_name(MIDI_PORT_PANEL, "S2.MID");
    CHECK_EQ(last_result(MIDI_PORT_PANEL), KP_RES_OK);
    CHECK_EQ(P.rec.song[5].count, 2); /* into song 6 (d1 = 5), not the current song */
    const uint8_t ldk[] = {0xF0, 0x40, 0x00, 0x24, 0x08, 0x02, 0x09, 0x05, 0x03, 0xF7};
    sx(MIDI_PORT_PANEL, ldk, sizeof ldk);
    fa_name(MIDI_PORT_PANEL, "S2.MID");
    CHECK_EQ(last_result(MIDI_PORT_PANEL), KP_RES_SMF_FORMAT);
    const uint8_t svk[] = {0xF0, 0x40, 0x00, 0x24, 0x08, 0x02, 0x0A, 0x01, 0x03, 0xF7};
    sx(MIDI_PORT_PANEL, svk, sizeof svk);
    fa_name(MIDI_PORT_PANEL, "S2.KSO");
    CHECK_EQ(last_result(MIDI_PORT_PANEL), KP_RES_PARAM);

    /* write in the stock form 24 05 01 00, 21 bytes (one full packet) + the empty packet the app adds
     * for the stock receiver; TopOfData is the byte count */
    const uint8_t wr1[] = {0xF0, 0x40, 0x00, 0x24, 0x08, 0x02, 0x05, 0x01, 0x00, 0xF7};
    sx(MIDI_PORT_PANEL, wr1, sizeof wr1);
    fa_name(MIDI_PORT_PANEL, "W21.TXT");
    {
        const uint8_t top21[] = {0xF0, 0x40, 0x00, 0x24, 0x08, 0x02, 0x01, 0, 0, 0, 21, 0xF7};
        sx(MIDI_PORT_PANEL, top21, sizeof top21);
        fa_packet(MIDI_PORT_PANEL, 0x02, 0, (const uint8_t *)"abcdefghijklmnopqrstu", 21);
        fa_packet(MIDI_PORT_PANEL, 0x02, 1, (const uint8_t *)"", 0);
        CHECK_EQ(out.msg[out.n - 1][6], 0x7F); /* the empty packet is acked */
        fa_simple(MIDI_PORT_PANEL, 0x7B);
    }
    CHECK_EQ(last_result(MIDI_PORT_PANEL), KP_RES_OK);
    i = rf_find("W21.TXT");
    CHECK(i >= 0);
    if (i >= 0) CHECK(rfs.f[i].len == 21 && memcmp(rfs.f[i].data, "abcdefghijklmnopqrstu", 21) == 0);

    /* play in the stock sequence: MusicMode 3 (USB memory) + select + Play */
    CMD(MIDI_PORT_PANEL, 0x52, 0x08, 0x7F, 0x02);
    MODE(MIDI_PORT_PANEL, 0x52, 0x1D, 0x7F, 0x03);
    fa_simple(MIDI_PORT_PANEL, 0x08);
    fa_name(MIDI_PORT_PANEL, "B.MID");
    CHECK_EQ(last_result(MIDI_PORT_PANEL), KP_RES_OK);
    {
        int before = eng.starts;
        CMD(MIDI_PORT_PANEL, 0x52, 0x08, 0x7F, 0x01);
        ticks(2);
        CHECK(eng.starts > before);
    }
    CMD(MIDI_PORT_PANEL, 0x52, 0x08, 0x7F, 0x02);
    MODE(MIDI_PORT_PANEL, 0x52, 0x1D, 0x7F, 0x00);

    /* DeleteFile / RenameFile remote parameters by file list id */
    char nm[FS_NAME_MAX];
    uint32_t sz;
    CHECK(rf_list(NULL, 0, nm, sizeof nm, &sz) && strcmp(nm, "A.TXT") == 0);
    CMD(MIDI_PORT_PANEL, 0x51, 0x3B, 0x7F, 0x01, 0x00, 0x00, 0x00);
    CHECK_EQ(last_result(MIDI_PORT_PANEL), KP_RES_OK);
    CHECK(rf_find("A.TXT") < 0);
    CMD(MIDI_PORT_PANEL, 0x51, 0x3A, 0x7F, 0x01, 0x00, 0x00, 0x00); /* B.MID */
    fa_name(MIDI_PORT_PANEL, "E.MID");
    CHECK_EQ(last_result(MIDI_PORT_PANEL), KP_RES_OK);
    CHECK(rf_find("E.MID") >= 0);
    all_tx_within_limit();

    /* format */
    CMD(MIDI_PORT_PANEL, 0x51, 0x3C, 0x7F, 0x00);
    CHECK_EQ(last_result(MIDI_PORT_PANEL), KP_RES_OK);
    CHECK(!rf_list(NULL, 0, nm, sizeof nm, &sz));

    /* detach: notification, the USB song is unloaded, requests report no media */
    rfs.mounted = false;
    out.n = 0;
    ticks(150);
    i = find_msg(MIDI_PORT_PANEL, 0x10, 0x51, 0x2E);
    CHECK(i >= 0);
    if (i >= 0) CHECK_EQ(out.msg[i][9], 0);
    CHECK_EQ(P.rec.song[REC_SLOT_EXT].count, 0);
    fa_simple(MIDI_PORT_PANEL, 0x11);
    CHECK_EQ(last_result(MIDI_PORT_PANEL), KP_RES_NO_MEDIA);
}

TEST(test_usb_recorder_saves_to_stick)
{
    boot(true);
    rfs.mounted = true;
    connect(MIDI_PORT_PANEL);
    ticks(150);
    MODE(MIDI_PORT_PANEL, 0x52, 0x1D, 0x7F, 0x03); /* USB recorder */
    CMD(MIDI_PORT_PANEL, 0x52, 0x08, 0x7F, 0x03);
    press_key(60, 3000);
    ticks(200);
    release_key(60);
    CMD(MIDI_PORT_PANEL, 0x52, 0x08, 0x7F, 0x02);
    ticks(1);
    CHECK(rf_find("USBREC_001.MID") >= 0);
    CHECK_EQ(songs.saves, 0); /* internal memory untouched */
}

TEST(test_on2_program_map_and_gs_reset)
{
    /* generated table agrees with the sound table */
    for (unsigned k = 0; k < PROGMAP_ON2_COUNT; k++)
        if (PROGMAP_ON2[k].sound >= 0) {
            const char *nm = SOUND_TABLE[PROGMAP_ON2[k].sound].name;
            /* the manual prints other banks for these two than the stock app sends */
            if (!strcmp(nm, "Marimba") || !strcmp(nm, "Celesta"))
                continue;
            CHECK_EQ(SOUND_TABLE[PROGMAP_ON2[k].sound].msb, PROGMAP_ON2[k].msb);
            CHECK_EQ(SOUND_TABLE[PROGMAP_ON2[k].sound].lsb, PROGMAP_ON2[k].lsb);
        }
    uint8_t a[3], b[3];
    CHECK(progmap_lookup(MT_ON1, 0, 0, 9, a));
    CHECK(progmap_lookup(MT_ON2, 121, 0, 9, b));
    CHECK(memcmp(a, b, 3) != 0); /* distinct maps */
    CHECK_EQ(a[0], 121); CHECK_EQ(a[2], 1); /* On1 PC 10 = EX | Modern = 121/0/1 */
    CHECK(b[2] & 0x80);                    /* GM2-only voice */
    CHECK(progmap_lookup(MT_ON2, 121, 0, 0, b));
    CHECK(b[0] == 121 && b[1] == 0 && b[2] == 0); /* GM2 piano 1 = SK-EX | Concert = 121/0/0 */
    CHECK(progmap_lookup(MT_ON2, 95, 112, 0, b));
    CHECK(b[0] == 95 && b[1] == 112 && b[2] == 0);
    CHECK(progmap_lookup(MT_ON2, 7, 7, 0, b)); /* unknown bank -> capital voice */
    CHECK(b[0] == 121 && b[2] == 0);

    boot(true);
    SET(MIDI_PORT_USB, 0x58, 0x10, 0x7F, 0x02); /* On2 */
    const uint8_t pc[] = {0xB4, 0x00, 121, 0xB4, 0x20, 0x00, 0xC4, 0x00, 0xB4, 0x07, 50};
    piano_midi_in(&P, MIDI_PORT_USB, pc, sizeof pc);
    CHECK(P.midi_part[4].msb == 121 && P.midi_part[4].program == 0); /* SK-EX | Concert = 121/0/0 */
    const uint8_t on[] = {0x94, 60, 100};
    piano_midi_in(&P, MIDI_PORT_USB, on, 3);
    CHECK_EQ(eng.last_part, PART_MIDI_BASE + 4);
    CHECK_EQ(eng.last_pc, 0);
    piano_pedal(&P, PEDAL_DAMPER, 0);
    const uint8_t dmp[] = {0xB4, 64, 127};
    piano_midi_in(&P, MIDI_PORT_USB, dmp, 3);
    CHECK_EQ(P.voices.damper[(PART_MIDI_BASE + 4) & 15], 127);
    /* GS reset: program, bank registers and controllers back to defaults */
    int rel = eng.releases;
    const uint8_t gs[] = {0xF0, 0x41, 0x10, 0x42, 0x12, 0x40, 0x00, 0x7F, 0x00, 0x41, 0xF7};
    piano_midi_in(&P, MIDI_PORT_USB, gs, sizeof gs);
    CHECK(eng.releases > rel);
    CHECK_EQ(P.midi_part[4].program, SOUND_TABLE[0].pc);
    CHECK_EQ(P.midi_part[4].msb, SOUND_TABLE[0].msb);
    CHECK_EQ(P.router.bank_msb[4], 0);
    CHECK_EQ(P.voices.damper[(PART_MIDI_BASE + 4) & 15], 0);
    /* a GS reset with a bad checksum is ignored */
    piano_midi_in(&P, MIDI_PORT_USB, pc, sizeof pc);
    const uint8_t bad[] = {0xF0, 0x41, 0x10, 0x42, 0x12, 0x40, 0x00, 0x7F, 0x00, 0x42, 0xF7};
    piano_midi_in(&P, MIDI_PORT_USB, bad, sizeof bad);
    CHECK_EQ(P.midi_part[4].msb, 121); /* still SK-EX | Concert (reset would give SOUND_TABLE[0]) */
    /* GM2 System On */
    const uint8_t gm2[] = {0xF0, 0x7E, 0x7F, 0x09, 0x03, 0xF7};
    piano_midi_in(&P, MIDI_PORT_USB, gm2, sizeof gm2);
    CHECK_EQ(P.midi_part[4].program, SOUND_TABLE[0].pc);
}

TEST(test_bluetooth_midi)
{
    boot(true);
    CHECK(!P.ble.enabled); /* default Off */
    CHECK_EQ(param_get1(&P.store, PRM_BLUETOOTH_MIDI, SLOT_SYSTEM, 9), 0);
    const uint8_t on[] = {0x90, 60, 100};
    piano_midi_in(&P, MIDI_PORT_BLE, on, 3);
    CHECK_EQ(eng.starts, 0);
    const uint8_t sync[] = {0xF0, 0x40, 0x7F, 0x7E, 0x08, 0x02, 0x01, 0x01, 0xF7};
    piano_midi_in(&P, MIDI_PORT_BLE, sync, sizeof sync);
    CHECK(!P.kp.connected[MIDI_PORT_BLE]);
    /* module reply is consumed even when Off */
    const uint8_t fw[] = {0xF0, 0x00, 0x02, 0x08, 0x10, 0x55, 0x20, 0x04, '3', '.', '1', 0xF7};
    piano_midi_in(&P, MIDI_PORT_BLE, fw, sizeof fw);
    CHECK(strcmp(P.ble.fw_version, "3.1") == 0);
    connect(MIDI_PORT_PANEL);
    SET(MIDI_PORT_PANEL, 0x51, 0x3D, 0x7F, 0x01);
    CHECK(P.ble.enabled);
    piano_midi_in(&P, MIDI_PORT_BLE, on, 3);
    CHECK_EQ(eng.starts, 1);
    out.n = 0;
    press_key(64, 3000);
    bool tx_ble = false;
    for (int i = 0; i < out.n; i++)
        tx_ble |= out.port[i] == MIDI_PORT_BLE && out.msg[i][0] == 0x90;
    CHECK(tx_ble);
    /* name: 23 bytes, module gets the first 16 */
    uint8_t name[23] = {'M', 'y', ' ', 'P', 'i', 'a', 'n', 'o'};
    out.n = 0;
    kx(MIDI_PORT_PANEL, 0x10, 0x51, 0x3E, 0x7F, name, 23);
    CHECK(strcmp(P.ble.name, "My Piano") == 0);
    bool mod = false;
    for (int i = 0; i < out.n; i++)
        mod |= out.port[i] == MIDI_PORT_BLE && out.msg[i][1] == 0x00 && out.msg[i][7] == 0x01 && out.msg[i][8] == 'M';
    CHECK(mod);
    out.n = 0;
    const uint8_t rq[] = {0xF0, 0x40, 0x00, 0x02, 0x08, 0x02, 0x51, 0x3E, 0x7F, 0xF7};
    sx(MIDI_PORT_PANEL, rq, sizeof rq);
    int i = find_msg(MIDI_PORT_PANEL, 0x10, 0x51, 0x3E);
    CHECK(i >= 0);
    if (i >= 0) { CHECK_EQ(out.len[i], 10 + 23); CHECK_EQ(out.msg[i][9], 'M'); }
    /* Off again: the BLE controller session ends */
    piano_midi_in(&P, MIDI_PORT_BLE, sync, sizeof sync);
    CHECK(P.kp.connected[MIDI_PORT_BLE]);
    SET(MIDI_PORT_PANEL, 0x51, 0x3D, 0x7F, 0x00);
    CHECK(!P.kp.connected[MIDI_PORT_BLE]);
    boot(false);
    CHECK(strcmp(P.ble.name, "My Piano") == 0);
}

TEST(test_information_queries)
{
    boot(true);
    const uint8_t ver[] = {0xF0, 0x40, 0x00, 0x7E, 0x08, 0x02, 0x04, 0x41, 0xF7};
    sx(MIDI_PORT_PANEL, ver, sizeof ver);
    CHECK(out.n == 1 && out.msg[0][6] == 0x04 && out.msg[0][7] == 0x01 && memcmp(out.msg[0] + 8, CA79NG_VERSION, 5) == 0);
    out.n = 0;
    const uint8_t dsp[] = {0xF0, 0x40, 0x00, 0x7E, 0x08, 0x02, 0x04, 0x41, 0x01, 0xF7};
    sx(MIDI_PORT_PANEL, dsp, sizeof dsp);
    CHECK(out.n == 1 && out.msg[0][8] == 0x01 && memcmp(out.msg[0] + 9, "TEST-DSP 2.0", 12) == 0);
    out.n = 0;
    const uint8_t blev[] = {0xF0, 0x40, 0x00, 0x7E, 0x08, 0x02, 0x04, 0x41, 0x02, 0xF7};
    sx(MIDI_PORT_PANEL, blev, sizeof blev);
    CHECK_EQ(last_result(MIDI_PORT_PANEL), KP_RES_NO_DATA); /* module has not answered yet */
    const uint8_t fw[] = {0xF0, 0x00, 0x02, 0x08, 0x10, 0x55, 0x20, 0x04, '1', '.', '7', 0xF7};
    piano_midi_in(&P, MIDI_PORT_BLE, fw, sizeof fw);
    out.n = 0;
    sx(MIDI_PORT_PANEL, blev, sizeof blev);
    CHECK(out.n == 1 && memcmp(out.msg[0] + 9, "1.7", 3) == 0);
    out.n = 0;
    const uint8_t rom[] = {0xF0, 0x40, 0x00, 0x7E, 0x08, 0x02, 0x04, 0x40, 0xF7};
    sx(MIDI_PORT_PANEL, rom, sizeof rom);
    CHECK(out.n == 1 && out.msg[0][7] == 0x00 && memcmp(out.msg[0] + 8, "OpenGrand", 9) == 0);
    out.n = 0;
    const uint8_t sum[] = {0xF0, 0x40, 0x00, 0x7E, 0x08, 0x02, 0x04, 0x42, 0xF7};
    sx(MIDI_PORT_PANEL, sum, sizeof sum);
    CHECK(out.n == 1 && out.len[0] == 9 + 8);
    const uint8_t want[8] = {1, 2, 3, 4, 5, 6, 7, 8};
    CHECK_MEM(out.msg[0] + 8, want, 8);
}

TEST(test_lock_mode)
{
    boot(true);
    SET(MIDI_PORT_PANEL, 0x51, 0x22, 0x7F, 0x01, 0x00); /* Panel lock */
    CHECK_EQ(P.lock, LOCK_PANEL);
    out.n = 0;
    SET(MIDI_PORT_PANEL, 0x55, 0x08, 0x00, 0x05);
    CHECK_EQ(out.msg[0][3], 0x06); /* NACK */
    CHECK_EQ(param_get1(&P.store, PRM_REVERB_TYPE, SLOT_MAIN1, 0), 2);
    SET(MIDI_PORT_PANEL, 0x52, 0x0D, 0x7F, 0, 0, 0); /* extensions are ignored (ACKed, as the stock ACKs any set) */
    CHECK_EQ(out.msg[1][3], 0x07);
    CHECK_EQ(P.cm.mode, 1); /* Concert Magic settings untouched (default Ensemble) */
    out.n = 0;
    SET(MIDI_PORT_USB, 0x55, 0x08, 0x00, 0x05); /* other controllers still work */
    CHECK_EQ(out.msg[0][3], 0x07);
    CHECK_EQ(param_get1(&P.store, PRM_REVERB_TYPE, SLOT_MAIN1, 0), 5);
    SET(MIDI_PORT_PANEL, 0x51, 0x22, 0x7F, 0x00, 0x00); /* unlock from the panel */
    CHECK_EQ(P.lock, 0);
    SET(MIDI_PORT_PANEL, 0x51, 0x22, 0x7F, 0x02, 0x00); /* Volume lock */
    piano_master_volume(&P, 90);
    CHECK_EQ(P.master_volume, 127);
    const uint8_t mv[] = {0xF0, 0x7F, 0x7F, 0x04, 0x01, 0x00, 0x20, 0xF7};
    piano_midi_in(&P, MIDI_PORT_USB, mv, sizeof mv);
    CHECK_EQ(P.master_volume, 127);
    out.n = 0;
    SET(MIDI_PORT_USB, 0x55, 0x01, 0x00, 40);
    CHECK_EQ(out.msg[0][3], 0x06);
    SET(MIDI_PORT_PANEL, 0x55, 0x08, 0x00, 0x01); /* panel is free with Volume lock */
    CHECK_EQ(param_get1(&P.store, PRM_REVERB_TYPE, SLOT_MAIN1, 0), 1);
    SET(MIDI_PORT_PANEL, 0x51, 0x22, 0x7F, 0x00, 0x00);
    piano_master_volume(&P, 90);
    CHECK_EQ(eng.master, 90);
}

TEST(test_sound_preview)
{
    boot(true);
    connect(MIDI_PORT_PANEL);
    SET(MIDI_PORT_PANEL, 0x50, 0x1F, 0x7F, 64);   /* preview volume */
    SET(MIDI_PORT_PANEL, 0x50, 0x1D, 0x7F, 0x00); /* Touch Curve: soft -> strong */
    CHECK_EQ(eng.starts, 1);
    CHECK_EQ(eng.last_vel, 20 * 64 / 127);
    ticks(2700);
    CHECK_EQ(eng.starts, 5);
    CHECK(!P.preview.active);
    CHECK(find_msg(MIDI_PORT_PANEL, 0x10, 0x50, 0x1E) >= 0); /* finished on its own */
    for (int i = 0; i < out.n; i++)
        CHECK(!(out.port[i] == MIDI_PORT_DIN && (out.msg[i][0] & 0xF0) == 0x90)); /* no MIDI out */
    /* Damper resonance: pedal down, then Stop releases everything */
    SET(MIDI_PORT_PANEL, 0x50, 0x1D, 0x7F, 0x02);
    ticks(200);
    CHECK(P.voices.damper[PART_MAIN1] >= 64);
    SET(MIDI_PORT_PANEL, 0x50, 0x1E, 0x7F, 0x02);
    CHECK(!P.preview.active);
    CHECK_EQ(P.voices.damper[PART_MAIN1], 0);
}

void suite_features(void)
{
    puts("features:");
    RUN(test_damper_hold_engine_and_synth);
    RUN(test_soft_pedal_depth);
    RUN(test_hammer_delay);
    RUN(test_user_touch_curve);
    RUN(test_pernote_vt);
    RUN(test_recorder_persistence_and_reset);
    RUN(test_rhythm_patterns_and_drums);
    RUN(test_song_tempo_transpose_parts);
    RUN(test_lesson_ab_repeat);
    RUN(test_concert_magic);
    RUN(test_concert_magic_tempo_logic);
    RUN(test_usb_file_access);
    RUN(test_usb_recorder_saves_to_stick);
    RUN(test_on2_program_map_and_gs_reset);
    RUN(test_bluetooth_midi);
    RUN(test_information_queries);
    RUN(test_lock_mode);
    RUN(test_sound_preview);
}
