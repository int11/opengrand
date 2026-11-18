/* Tests for the cave playback-control port (ports/cave/recctl_cave.c, slot payload ota_main feat2 0x10).  The stock RAM, the stock routines (rec_stock.h) and the session
 * core's TX / bound mask are stand-ins here; the stock-image emulator run is result/stockfw/tools/roundtrip_recplay.py. */
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "../ports/cave/recctl_cave.h"
#include "ca79ng/recctl.h"
#include "ca79ng/recsave.h"
#include "ca79ng/recview.h"
#include "test.h"

/* ---- stock RAM stand-in: three windows, the rest reads 0 ---- */
static uint8_t ram19[0x10000];    /* 0x41910000: recorder / player / Music queue / tempo */
static uint8_t ram13[0x40000];    /* 0x41390000: NOR job table, file executor            */
static uint8_t store[0x100000];   /* 0x40500000: RecorderData mirror                      */
static uint8_t tpl[0x74 * 10];    /* 0x400A7574: ROM empty song headers                    */
static uint8_t sbuf[0x100000];    /* 0x40A00000: the stock serialisation buffer            */

static uint8_t *at(uint32_t a)
{
    if (a >= 0x41910000u && a < 0x41920000u)
        return &ram19[a - 0x41910000u];
    if (a >= 0x41390000u && a < 0x413D0000u)
        return &ram13[a - 0x41390000u];
    if (a >= 0x40500000u && a < 0x40600000u)
        return &store[a - 0x40500000u];
    if (a >= 0x400A7574u && a < 0x400A7574u + sizeof tpl)
        return &tpl[a - 0x400A7574u];
    if (a >= 0x40A00000u && a < 0x40A00000u + sizeof sbuf)
        return &sbuf[a - 0x40A00000u];
    return 0;
}

uint8_t *rcc_host_ptr(uint32_t addr) { return at(addr); }

void rcc_host_read(uint32_t addr, void *dst, uint32_t n)
{
    uint8_t *d = dst;
    for (uint32_t i = 0; i < n; i++) {
        uint8_t *p = at(addr + i);
        d[i] = p ? *p : 0;
    }
}

static void poke8(uint32_t a, uint8_t v) { *at(a) = v; }
static void poke16(uint32_t a, uint16_t v) { poke8(a, (uint8_t)v), poke8(a + 1, (uint8_t)(v >> 8)); }
static void poke32(uint32_t a, uint32_t v) { poke16(a, (uint16_t)v), poke16(a + 2, (uint16_t)(v >> 16)); }

static uint32_t peek32(uint32_t a) { return at(a)[0] | at(a)[1] << 8 | at(a)[2] << 16 | (uint32_t)at(a)[3] << 24; }

/* 10 songs of 0x80 B from 0x30; song 1 (index 0) has part 1 (track at +0x74: FA 04, a note, FC) and 3 bars, the
 * others are empty (0x80 B: the 0x74 B header + padding) */
static void store_build(void)
{
    uint32_t off = 0x30;
    static const uint8_t trk[8] = {0xFA, 0x04, 0x3C, 0x00, 0x64, 0x10, 0xFC, 0x00};
    memset(store, 0, sizeof store);
    for (int i = 0; i < RV_SONGS; i++, off += 0x80)
        poke32(0x40500000u + 4u * (uint32_t)i, off);
    poke32(0x40500000u + 40u, off);              /* off[10] */
    poke32(0x40500000u + 44u, RV_STORE_LIMIT);   /* off[11] */
    poke16(0x40500000u + off, 0xA55A);
    poke16(0x40500000u + RV_STORE_LIMIT, 0xA55A);
    poke32(0x40500000u + 0x30 + 0x2C, 0x74);     /* song 1, track 0 */
    memcpy(&store[0x30 + 0x74], trk, sizeof trk);
    poke32(0x40500000u + 0x30 + 0x6C, 0x7C);
    poke32(0x40500000u + 0x30 + 0x70, 3);
    for (int i = 0; i < 10; i++) {
        memset(&tpl[0x74 * i], 0, 0x74);
        memcpy(&tpl[0x74 * i], "SONG 0", 6);
        tpl[0x74 * i + 6] = (uint8_t)('1' + i);
        tpl[0x74 * i + 0x28] = 16;
    }
}

/* ---- stock routines (rec_stock.h) ---- */
static int posts, selects, parts, tempos, seeks, results;
static uint32_t result_rc;
static uint8_t post_kind[8], post_val[8];
static uint32_t last_arg;

void stk_rec_post(uint32_t kind, uint32_t value)
{
    if (posts < 8)
        post_kind[posts] = (uint8_t)kind, post_val[posts] = (uint8_t)value;
    posts++;
}
uint32_t stk_rec_block(uint32_t zero) { (void)zero; return 0x418F19E4u; }
uint32_t stk_rec_select(uint32_t block, uint32_t song) { (void)block; selects++; last_arg = song; return 0; }
uint32_t stk_rec_part(uint32_t v) { parts++; last_arg = v; return 0; }
void stk_rec_maxtime(uint32_t rc) { (void)rc; }
void stk_rec_result(uint32_t rc) { results++, result_rc = rc; }
void stk_rec_tempo(uint32_t bpm) { tempos++; last_arg = bpm; }
void stk_rec_seek(uint32_t bar, uint32_t b, uint32_t block) { (void)b; (void)block; seeks++; last_arg = bar; }

/* the song writer FUN_40168408 as the stock does it: song := src, the songs after
 * it move, the offset table and off[10] follow; the NOR job = the slot-1 pending byte */
static int writes, loads, edit_oks;
static uint32_t write_song, write_len, load_song;
uint32_t stk_rec_store_write(uint32_t song, uint32_t src, uint32_t len)
{
    uint32_t o0 = peek32(0x40500000u + 4 * song), o1 = peek32(0x40500000u + 4 * (song + 1)),
             end = peek32(0x40500000u + 40u), d = len - (o1 - o0);
    writes++, write_song = song, write_len = len;
    memmove(&store[o1 + d], &store[o1], end + 2 - o1);
    memcpy(&store[o0], at(src), len);
    for (uint32_t i = song + 1; i <= 10; i++)
        poke32(0x40500000u + 4 * i, peek32(0x40500000u + 4 * i) + d);
    poke8(RV_A_JOB1, 1);
    return 0;
}
void stk_rec_store_changed(void) {}
uint32_t stk_rec_load(uint32_t song) { loads++, load_song = song; return 0; }
void stk_rec_load_done(void) {}
void stk_rec_edit_ok(uint32_t rc) { (void)rc; edit_oks++; }

/* ---- session core (sysex_cave.c) ---- */
static uint8_t bound = 0x0C;   /* app (BLE, port 2) + panel (port 3) */
static int ntx;
static uint8_t tx_port[32];
static uint8_t tx_f[32][96];
static uint16_t tx_n[32];

uint8_t sxc_bound_mask(void) { return bound; }
void sxc_tx(uint8_t port, const uint8_t *f, uint16_t n)
{
    if (ntx < 32) {
        tx_port[ntx] = port;
        tx_n[ntx] = n;
        memcpy(tx_f[ntx], f, n < 96 ? n : 96);
    }
    ntx++;
}

static const uint8_t BUSY[] = {0xF0, 0x40, 0x7F, 0x7E, 0x04, 0x1E, 0x03, 0x01, 0x00, 0x00, 0xF7};
static const uint8_t NODATA[] = {0xF0, 0x40, 0x7F, 0x7E, 0x04, 0x1E, 0x03, 0x00, 0x01, 0x6C, 0xF7};

static uint32_t now;

static void fresh(void)
{
    memset(ram19, 0, sizeof ram19);
    memset(ram13, 0, sizeof ram13);
    store_build();
    poke8(RV_A_STATE, RV_ST_STOP);
    poke8(RV_A_MUSIC_MODE, 2);
    poke16(RV_A_TEMPO, 120), poke16(RV_A_TEMPO_ORIG, 120);
    poke16(RV_A_MAX_BARS, 3);
    poke8(RV_A_MAX_SLOT, 0);
    posts = selects = parts = tempos = seeks = 0;
    ntx = 0;
    bound = 0x0C;
    now = 1000;
    rcc_restart();
    rcc_pump(now);   /* first pass: the full view */
}

static void pump(uint32_t ms)
{
    for (uint32_t t = 0; t < ms; t += 2)
        rcc_pump(now += 2);
}

static uint32_t kawai(uint8_t *m, uint8_t fn, uint8_t v2, const uint8_t *d, uint32_t nd)
{
    uint32_t o = 0;
    m[o++] = 0xF0, m[o++] = 0x40, m[o++] = 0x7F, m[o++] = fn, m[o++] = 0x08, m[o++] = 0x02;
    m[o++] = 0x52, m[o++] = v2, m[o++] = 0x7F;
    for (uint32_t i = 0; i < nd; i++)
        m[o++] = d[i];
    m[o++] = 0xF7;
    return o;
}

static int music(uint8_t port, uint8_t fn, uint8_t v)
{
    uint8_t m[16];
    uint32_t n = kawai(m, fn, 0x08, &v, 1);
    return rcc_kawai(port, m, n);
}

static int song_select(uint8_t port, uint8_t song)
{
    uint8_t m[16], d[3] = {0, 0, song};
    uint32_t n = kawai(m, 0x10, 0x00, d, 3);
    return rcc_kawai(port, m, n);
}

static void op(uint8_t port, const uint8_t *args, uint32_t na)
{
    uint8_t m[16] = {0xF0, 0x7D, 0x43, 0x41, 0x37, 0x39, RCC_OP};
    uint32_t n = 7;
    for (uint32_t i = 0; i < na; i++)
        m[n++] = args[i];
    m[n++] = 0xF7;
    rcc_sysex(port, m, n);
}

static void op_cmd(uint8_t port, uint8_t cmd, uint16_t arg)
{
    uint8_t a[4] = {0x02, cmd, (uint8_t)((arg >> 7) & 0x7F), (uint8_t)(arg & 0x7F)};
    op(port, a, 4);
}

static bool sent(int i, uint8_t port, const uint8_t *f, uint16_t n)
{
    return i < ntx && tx_port[i] == port && tx_n[i] == n && memcmp(tx_f[i], f, n) == 0;
}

/* F0 7D 43 41 37 39 5E sub a b F7 */
static bool short_reply(int i, uint8_t port, uint8_t sub, uint8_t a, uint8_t b)
{
    const uint8_t f[11] = {0xF0, 0x7D, 0x43, 0x41, 0x37, 0x39, 0x5E, sub, a, b, 0xF7};
    return sent(i, port, f, 11);
}

TEST(recplay_waits_for_its_first_view)
{
    fresh();
    rcc_restart();
    CHECK_EQ(music(3, 0x32, 1), 0);   /* before the first pump pass: the stock's */
    CHECK_EQ(posts, 0);
}

TEST(recplay_kawai_play_runs_through_the_queue)
{
    fresh();
    CHECK_EQ(music(2, 0x32, 1), 1);   /* the app (not the stock binding) */
    CHECK_EQ(posts, 1);
    CHECK_EQ(post_kind[0], RC_Q_MUSIC);
    CHECK_EQ(post_val[0], RC_MUSIC_PLAY);
    CHECK_EQ(ntx, 0);                 /* accepted: no refusal frame, no 0x7D (nobody subscribed) */
    poke8(RV_A_STATE, RV_ST_PLAY);    /* task 20 ran it */
    pump(100);
    CHECK_EQ(ntx, 0);
    CHECK_EQ(music(3, 0x10, 1), 1);   /* the panel's Play while playing: NOP, owned, nothing sent */
    CHECK_EQ(posts, 1);
    CHECK_EQ(ntx, 0);
    /* MusicMode 0: the core enters MusicMode 2 first */
    fresh();
    poke8(RV_A_MUSIC_MODE, 0);
    CHECK_EQ(music(3, 0x32, 1), 1);
    CHECK_EQ(posts, 2);
    CHECK(post_kind[0] == RC_Q_MODE && post_val[0] == 2 && post_kind[1] == RC_Q_MUSIC && post_val[1] == 1);
}

TEST(recplay_refusals_are_the_same_for_every_controller)
{
    /* NOR job busy */
    fresh();
    poke8(RV_A_JOB1, 1);
    CHECK_EQ(music(3, 0x32, 1), 1);
    CHECK_EQ(music(2, 0x32, 1), 1);
    CHECK_EQ(song_select(2, 0), 1);
    op_cmd(2, RC_PLAY, 0);
    pump(2);
    CHECK_EQ(posts + selects, 0);
    CHECK(sent(0, 3, BUSY, sizeof BUSY));
    CHECK(sent(1, 2, BUSY, sizeof BUSY));
    CHECK(sent(2, 2, BUSY, sizeof BUSY));
    CHECK(short_reply(3, 2, 0x02, RC_E_BUSY, RC_PLAY));
    /* recording (state 4): Play / SongSelect refused (SongSelect would commit), Stop is R2's - the stock's */
    fresh();
    poke8(RV_A_STATE, RV_ST_RECORDING);
    CHECK_EQ(song_select(3, 1), 1);
    CHECK_EQ(music(2, 0x32, 1), 1);
    CHECK_EQ(music(3, 0x32, 2), 0);
    CHECK_EQ(music(2, 0x32, 3), 0);   /* record transports: always the stock's */
    op_cmd(2, RC_SELECT, 1);
    pump(2);
    CHECK_EQ(posts + selects, 0);
    CHECK(sent(0, 3, BUSY, sizeof BUSY) && sent(1, 2, BUSY, sizeof BUSY));
    CHECK(short_reply(2, 2, 0x02, RC_E_REC, RC_SELECT));
    /* MusicMode pending (deferred mode 0x419156A4): the stock would drop the Music; the core refuses it */
    fresh();
    poke8(RV_A_MQ_HOLD, 0x82);
    CHECK_EQ(music(3, 0x10, 1), 1);
    CHECK_EQ(music(2, 0x32, 1), 1);
    op_cmd(2, RC_PLAY, 0);
    pump(2);
    CHECK_EQ(posts, 0);
    CHECK(sent(0, 3, BUSY, sizeof BUSY) && sent(1, 2, BUSY, sizeof BUSY));
    CHECK(short_reply(2, 2, 0x02, RC_E_BUSY, RC_PLAY));
    /* empty song: the stock's own Result 0xEC bytes */
    fresh();
    poke8(RV_A_SONG, 4);
    CHECK_EQ(music(3, 0x32, 1), 1);
    CHECK(sent(0, 3, NODATA, sizeof NODATA));
    /* command pending: the next one waits (Stop may replace it) */
    fresh();
    CHECK_EQ(music(2, 0x32, 1), 1);
    CHECK_EQ(song_select(3, 1), 1);
    CHECK(sent(0, 3, BUSY, sizeof BUSY));
    CHECK_EQ(selects, 0);
}

TEST(recplay_other_players_stay_the_stocks)
{
    fresh();
    poke8(RV_A_MUSIC_MODE, 3);        /* USB player */
    CHECK_EQ(music(3, 0x32, 1), 0);
    CHECK_EQ(song_select(2, 1), 0);
    fresh();
    poke8(RV_A_MUSIC_MODE, 0);        /* MusicProgress in mode 0: the stock ignores it - left to it */
    {
        uint8_t m[20], d[6] = {0, 0, 2, 0, 0, 0};
        uint32_t n = kawai(m, 0x32, 0x03, d, 6);
        CHECK_EQ(rcc_kawai(2, m, n), 0);
        poke8(RV_A_MUSIC_MODE, 2);
        CHECK_EQ(rcc_kawai(2, m, n), 1);
        CHECK_EQ(seeks, 1);
        CHECK_EQ(last_arg, 2);
        d[0] = 1;                     /* not the bar form */
        n = kawai(m, 0x32, 0x03, d, 6);
        CHECK_EQ(rcc_kawai(2, m, n), 0);
    }
    {
        uint8_t m[16], d[3] = {1, 0, 0};   /* SongSelect numType 1 (USB file) */
        uint32_t n = kawai(m, 0x10, 0x00, d, 3);
        CHECK_EQ(rcc_kawai(2, m, n), 0);
    }
    CHECK_EQ(music(2, 0x32, 7), 0);   /* fast forward */
    CHECK_EQ(ntx, 0);
}

TEST(recplay_op_status_list_and_subscription)
{
    uint8_t a0[1] = {0x00}, a1[1] = {0x01};
    fresh();
    op(2, a0, 1);
    CHECK_EQ(ntx, 1);
    CHECK(tx_port[0] == 2 && tx_f[0][6] == 0x5E && tx_f[0][7] == 0 && tx_f[0][8] == 0);
    CHECK_EQ(tx_n[0], 9 + 55 + 1);    /* pack7(48 B) = 55 */
    CHECK_EQ(tx_f[0][9 + 1], RV_ENC_VERSION);   /* raw[0] after the first pack7 prefix */
    CHECK_EQ(tx_f[0][9 + 2], RV_ST_STOP);
    op(2, a1, 1);
    CHECK_EQ(ntx, 2);
    CHECK(tx_f[1][7] == 1 && tx_n[1] == 9 + 80 + 1);
    /* a state change: 5E 00 to the subscribed app only (the panel never asked: no 0x7D frame for it) */
    ntx = 0;
    pump(60);
    CHECK_EQ(ntx, 0);
    poke8(RV_A_STATE, RV_ST_PLAY);
    pump(60);
    CHECK_EQ(ntx, 1);
    CHECK(tx_port[0] == 2 && tx_f[0][6] == 0x5E && tx_f[0][7] == 0);
    /* the bar alone: at most every 250 ms */
    ntx = 0;
    for (int b = 1; b <= 10; b++) {
        poke16(RV_A_BAR, (uint16_t)b);
        pump(50);
    }
    CHECK(ntx >= 1 && ntx <= 3);
    /* the song table changed (a commit): 5E 00 + 5E 01 */
    ntx = 0;
    poke8(RV_A_JOB1, 1);
    pump(60);
    poke32(0x40500000u + 0x30 + 0x80 + 0x2C, 0x74);   /* song 2 gains part 1 */
    poke8(0x40500000u + 0x30 + 0x80 + 0x74, 0xFA);
    poke8(RV_A_JOB1, 0);
    pump(60);
    CHECK(ntx >= 2 && tx_f[ntx - 1][7] == 1);
    /* the app leaves: forgotten */
    bound = 0x08;
    ntx = 0;
    poke8(RV_A_STATE, RV_ST_STOP);
    pump(100);
    CHECK_EQ(ntx, 0);
}

TEST(recplay_op_commands_answer_and_confirm)
{
    fresh();
    op_cmd(0, RC_PLAY, 0);            /* port 0: nobody bound there */
    CHECK(short_reply(0, 0, 0x02, RCC_NOT_BOUND, RC_PLAY));
    ntx = 0;
    op_cmd(2, RC_TEMPO, 144);
    CHECK_EQ(rcc_quiesce(), 1);
    op_cmd(2, RC_TEMPO, 150);         /* mailbox full */
    CHECK(short_reply(0, 2, 0x02, RC_E_PENDING, RC_TEMPO));
    pump(2);
    CHECK_EQ(rcc_quiesce(), 0);
    CHECK_EQ(tempos, 1);
    CHECK_EQ(last_arg, 144);
    CHECK(short_reply(1, 2, 0x02, RC_OK, RC_TEMPO));
    poke16(RV_A_TEMPO, 144);          /* the stock routine took it */
    ntx = 0;
    pump(60);
    CHECK(short_reply(0, 2, 0x03, RC_EV_DONE, RC_TEMPO));
    /* seek in MusicMode 0 (the stock seeks only in mode 2) */
    poke8(RV_A_MUSIC_MODE, 0);
    ntx = 0;
    op_cmd(2, RC_SEEK, 2);
    pump(2);
    CHECK(short_reply(0, 2, 0x02, RC_E_MODE, RC_SEEK));
    /* a Kawai Stop replaces the app's pending Play: the app hears so */
    poke8(RV_A_MUSIC_MODE, 2);
    ntx = 0;
    op_cmd(2, RC_PLAY, 0);
    pump(2);
    CHECK(short_reply(0, 2, 0x02, RC_OK, RC_PLAY));
    CHECK_EQ(music(3, 0x32, 2), 1);
    CHECK(short_reply(1, 2, 0x03, RCC_EV_REPLACED, RC_PLAY));
    /* bad sub / bad command */
    ntx = 0;
    op_cmd(2, 9, 0);
    pump(2);
    CHECK(short_reply(0, 2, 0x02, RC_E_ARG, 9));
}

/* ---- R2 (RCC_REC2, feat2 0x40 build) ---- */

TEST(recrec_off_leaves_record_transports_to_the_stock)
{
    rcc_host_rec2(0);
    fresh();
    poke8(RV_A_SONG, 4);
    CHECK_EQ(music(3, 0x32, 3), 0);
    poke8(RV_A_STATE, RV_ST_RECORDING);
    CHECK_EQ(music(3, 0x32, 2), 0);
    op_cmd(2, RC_REC, RC_REC_KEEP);
    pump(2);
    CHECK(short_reply(0, 2, 0x02, RC_E_ARG, RC_REC));
    CHECK_EQ(posts, 0);
}

TEST(recrec_kawai_take)
{
    uint8_t raw[RCC_STATUS_RAW];
    rcc_host_rec2(1);
    fresh();
    poke8(RV_A_SONG, 4);   /* empty song 5 */
    pump(2);
    /* the panel's Rec (Music 3): the core arms through the queue */
    CHECK_EQ(music(3, 0x32, 3), 1);
    CHECK(posts == 1 && post_kind[0] == RC_Q_MUSIC && post_val[0] == RC_MUSIC_ARM);
    CHECK_EQ(ntx, 0);
    poke8(RV_A_STATE, RV_ST_REC_STANDBY);
    pump(4);
    /* the app's Play while armed: refused with the stock Busy bytes */
    CHECK_EQ(music(2, 0x32, 1), 1);
    CHECK(sent(0, 2, BUSY, sizeof BUSY));
    /* start (Music 4) */
    CHECK_EQ(music(3, 0x32, 4), 1);
    CHECK(posts == 2 && post_val[1] == RC_MUSIC_START);
    poke8(RV_A_STATE, RV_ST_RECORDING);
    pump(4);
    /* finish from the panel, then the app's stop arrives before task 20 ran: one FC only */
    CHECK_EQ(music(3, 0x32, 2), 1);
    CHECK(posts == 3 && post_val[2] == RC_MUSIC_STOP);
    CHECK_EQ(music(2, 0x32, 2), 1);
    CHECK_EQ(music(2, 0x32, 0), 1);
    CHECK_EQ(posts, 3);
    rcc_status(raw);
    CHECK_EQ(raw[35] & 0x30, 0x30);   /* rec offered, fc_out */
    poke8(RV_A_STATE, RV_ST_STOP);
    pump(4);
    rcc_status(raw);
    CHECK_EQ(raw[35] & 0x30, 0x10);
    rcc_host_rec2(0);
}

TEST(recrec_finish_deferred_and_op_commands)
{
    rcc_host_rec2(1);
    fresh();
    poke8(RV_A_SONG, 4);
    pump(2);
    ntx = 0;
    op_cmd(2, RC_COUNT_IN, 0);   /* part 1 of the empty song */
    pump(2);
    CHECK(short_reply(0, 2, 0x02, RC_OK, RC_COUNT_IN));
    CHECK(posts == 1 && post_val[0] == RC_MUSIC_COUNT_IN);
    poke8(RV_A_STATE, RV_ST_COUNT_IN);
    pump(2);
    CHECK(short_reply(1, 2, 0x03, RC_EV_DONE, RC_COUNT_IN));
    /* a stop during the count-in: no stock command leaves it */
    ntx = 0;
    op_cmd(2, RC_STOP, 0);
    pump(2);
    CHECK(short_reply(0, 2, 0x02, RC_E_WRONG, RC_STOP));
    CHECK_EQ(music(3, 0x32, 2), 1);   /* the panel's stop: Busy */
    CHECK(sent(1, 3, BUSY, sizeof BUSY));
    op_cmd(2, RC_REC_START, 0);
    pump(2);
    poke8(RV_A_STATE, RV_ST_RECORDING);
    pump(2);
    /* a NOR job appears: the finish is accepted and waits */
    poke8(RV_A_JOB1, 1);
    posts = 0, ntx = 0;
    op_cmd(2, RC_STOP, 0);
    pump(20);
    CHECK(short_reply(0, 2, 0x02, RC_OK, RC_STOP));
    CHECK_EQ(posts, 0);
    poke8(RV_A_JOB1, 0);
    pump(2);
    CHECK(posts == 1 && post_val[0] == RC_MUSIC_STOP);
    poke8(RV_A_STATE, RV_ST_STOP);
    pump(4);
    CHECK(short_reply(1, 2, 0x03, RC_EV_DONE, RC_STOP));
    /* song 1 holds part 1: the op asks for the overwrite flag, the Kawai Rec does not */
    fresh();
    ntx = 0;
    op_cmd(2, RC_REC, 0);
    pump(2);
    CHECK(short_reply(0, 2, 0x02, RC_E_EXISTS, RC_REC));
    op_cmd(2, RC_REC, RC_REC_OVERWRITE);   /* hi7 = 2 */
    pump(2);
    CHECK(short_reply(1, 2, 0x02, RC_OK, RC_REC));
    rcc_host_rec2(0);
}

/* ---- R3 (RCC_REC3, feat3 0x04): the save decision in the port ---- */
static int del(uint8_t port, uint8_t fmt, uint16_t song, uint8_t part)
{
    uint8_t m[20], d[4] = {fmt, (uint8_t)((song >> 7) & 0x7F), (uint8_t)(song & 0x7F), part};
    uint32_t n = kawai(m, 0x32, 0x21, d, 4);
    return rcc_kawai(port, m, n);
}

/* the save record 5E 04: unpack the 24 raw bytes of the last such frame to `port` */
static bool save_record(uint8_t port, uint8_t raw[24])
{
    for (int i = (ntx < 32 ? ntx : 32) - 1; i >= 0; i--) {
        if (tx_port[i] != port || tx_n[i] < 12 || tx_f[i][6] != 0x5E || tx_f[i][7] != 0x04)
            continue;
        const uint8_t *p = tx_f[i] + 9;
        int o = 0;
        while (o < 24 && *p != 0xF7) {
            uint8_t hi = *p++;
            for (int k = 0; k < 7 && o < 24; k++)
                raw[o++] = (uint8_t)(*p++ | (((hi >> k) & 1u) << 7));
        }
        return o == 24;
    }
    return false;
}

/* the delete is the core's in every build (recdel.h): the stock's own re-serialises the last take */
TEST(recdel_port_deletes_with_the_stock_writer)
{
    uint8_t raw[RCC_LIST_RAW];
    fresh();
    rcc_status(raw);
    CHECK_EQ(raw[31] & 0x01, 0x01);          /* the app's cue: this firmware deletes */
    writes = loads = edit_oks = results = 0;
    /* song 1 (part 1 only), part 2: nothing to remove - the stock's OK answer, no write */
    CHECK_EQ(del(3, 0x04, 0, 1), 1);
    CHECK_EQ(writes, 0);
    CHECK_EQ(edit_oks, 1);
    /* part 1 = its only track: the song becomes the ROM template, the songs after it move down */
    CHECK_EQ(del(2, 0x04, 0, 0), 1);
    CHECK_EQ(writes, 1);
    CHECK_EQ(write_song, 0);
    CHECK_EQ(write_len, 0x74);
    CHECK_EQ(loads, 1);
    CHECK_EQ(load_song, 0);
    CHECK_EQ(edit_oks, 2);
    CHECK_EQ(peek32(0x40500000u + 4), 0x30 + 0x74);
    CHECK_MEM(&store[0x30], tpl, 0x74);
    /* 1E 01 right away lists song 1 empty (the table read again after the write, before the job) */
    rcc_list(raw);
    CHECK_EQ(raw[6] & 0x03, 0);
    CHECK_EQ(raw[0] | raw[1] << 8, 0x74);
    /* the job is pending now: Busy, nothing written */
    ntx = 0;
    CHECK_EQ(del(2, 0x04, 1, 0x7F), 1);
    CHECK(sent(0, 2, BUSY, sizeof BUSY));
    CHECK_EQ(writes, 1);
    poke8(RV_A_JOB1, 0);
    /* playing: Busy; song 12: Busy (the stock would take song 1); fmt 00 / part 5 / every song: the stock's */
    poke8(RV_A_STATE, RV_ST_PLAY);
    ntx = 0;
    CHECK_EQ(del(3, 0x04, 1, 0x7F), 1);
    CHECK(sent(0, 3, BUSY, sizeof BUSY));
    poke8(RV_A_STATE, RV_ST_STOP);
    CHECK_EQ(del(3, 0x04, 11, 0x7F), 1);
    CHECK_EQ(del(3, 0x00, 1, 0x7F), 0);
    CHECK_EQ(del(3, 0x04, 1, 5), 0);
    CHECK_EQ(del(3, 0x7F, 0x3FFF, 0x7F), 0);
    CHECK_EQ(writes, 1);
    /* a song the core cannot read (a track without FC behind the cut): Busy, nothing written */
    poke32(0x40500000u + peek32(0x40500000u + 4) + 0x2C, 0x74);   /* song 2: two tracks */
    poke32(0x40500000u + peek32(0x40500000u + 4) + 0x30, 0x78);
    poke8(0x40500000u + peek32(0x40500000u + 4) + 0x74, 0xFA);
    poke8(0x40500000u + peek32(0x40500000u + 4) + 0x78, 0xFA);
    pump(2);
    ntx = 0;
    CHECK_EQ(del(3, 0x04, 1, 0), 1);
    CHECK(sent(0, 3, BUSY, sizeof BUSY));
    CHECK_EQ(writes, 1);
}

TEST(recsave_off_leaves_deletes_and_op_04)
{
    uint8_t raw[RCC_STATUS_RAW];
    rcc_host_rec2(1), rcc_host_rec3(0);
    fresh();
    CHECK_EQ(del(3, 0x04, 0, 0x7F), 1);   /* the core's (recdel), no window */
    poke8(RV_A_JOB1, 0);
    poke8(RV_A_STATE, RV_ST_RECORDING);
    CHECK_EQ(del(3, 0x04, 0, 0x7F), 1);   /* refused under a take */
    ntx = 0;
    const uint8_t a[2] = {0x04, 0x00};
    op(2, a, 1);
    CHECK(short_reply(0, 2, 0x04, RC_E_ARG, 0));
    rcc_status(raw);
    CHECK_EQ(raw[35] & 0x80, 0);
    rcc_host_rec2(0);
}

TEST(recsave_port_windows_and_refusals)
{
    uint8_t raw[RCC_STATUS_RAW], sv[24];
    rcc_host_rec2(1), rcc_host_rec3(1);
    fresh();
    rcc_status(raw);
    CHECK_EQ(raw[35] & 0x80, 0x80);
    /* the app subscribes and reads the save record */
    ntx = 0;
    const uint8_t a[2] = {0x04, 0x00};
    op(2, a, 1);
    CHECK(save_record(2, sv) && sv[0] == 1 && sv[1] == 1 && sv[2] == RS_P_IDLE);
    pump(2);   /* a settled pass: the table every later write is compared with */
    /* delete while recording: Busy from the core, the stock apply never sees it */
    poke8(RV_A_STATE, RV_ST_RECORDING);
    pump(2);
    ntx = 0;
    CHECK_EQ(del(3, 0x04, 0, 0x7F), 1);
    CHECK(sent(0, 3, BUSY, sizeof BUSY));
    CHECK_EQ(rcc_file_veto(0x09, 2), 1);   /* import under a take */
    CHECK_EQ(rcc_file_veto(0x08, 0), 1);   /* USB load under a take */
    CHECK_EQ(rcc_file_veto(0x0A, 0), 0);
    /* the commit: job queued, state 2 - a window opens; loads are refused until the job is done */
    poke8(RV_A_JOB1, 1);
    poke8(RV_A_STATE, RV_ST_STOP);
    pump(2);
    ntx = 0;
    CHECK_EQ(song_select(2, 1), 1);
    CHECK(sent(0, 2, BUSY, sizeof BUSY));
    CHECK_EQ(del(3, 0x04, 1, 0x7F), 1);
    CHECK_EQ(rcc_file_veto(0x09, 2), 1);
    op_cmd(2, RC_PLAY, 0);
    pump(2);
    CHECK(short_reply(2, 2, 0x02, RC_E_BUSY, RC_PLAY));
    /* the job ends; song 1 got a part 2 (the take) - the window closes OK and 5E 04 goes to the subscriber */
    poke32(0x40500000u + 0x30 + 0x30, 0x78);   /* song 1, track 1 */
    poke8(0x40500000u + 0x30 + 0x78, 0xFA);
    poke8(RV_A_JOB1, 0);
    ntx = 0;
    pump(2);
    CHECK(save_record(2, sv));
    CHECK_EQ(sv[6], RS_R_OK);
    CHECK_EQ(sv[7], RS_K_COMMIT);
    CHECK_EQ(sv[22] | sv[23] << 8, 1);
    /* an accepted delete: the core writes it (rcc_kawai 1, recdel), the window waits for its job */
    CHECK_EQ(del(3, 0x04, 0, 0x7F), 1);
    CHECK_EQ(song_select(2, 1), 1);        /* refused: the delete's window */
    CHECK_EQ(del(3, 0x04, 11, 0x7F), 1);   /* song 12: refused (the stock would delete song 1) */
    poke8(RV_A_JOB1, 1);
    pump(2);
    poke8(RV_A_JOB1, 0);
    ntx = 0;
    pump(2);
    CHECK(save_record(2, sv) && sv[6] == RS_R_OK && sv[7] == RS_K_DELETE && sv[18] == 1);
    selects = 0;
    CHECK_EQ(song_select(2, 1), 1);        /* the core's again: runs the stock select */
    CHECK_EQ(selects, 1);
    rcc_host_rec2(0), rcc_host_rec3(0);
}

void suite_recctl_cave(void)
{
    printf("playback control port (feat2 0x10)\n");
    RUN(recplay_waits_for_its_first_view);
    RUN(recplay_kawai_play_runs_through_the_queue);
    RUN(recplay_refusals_are_the_same_for_every_controller);
    RUN(recplay_other_players_stay_the_stocks);
    RUN(recplay_op_status_list_and_subscription);
    RUN(recplay_op_commands_answer_and_confirm);
    RUN(recrec_off_leaves_record_transports_to_the_stock);
    RUN(recrec_kawai_take);
    RUN(recrec_finish_deferred_and_op_commands);
    RUN(recdel_port_deletes_with_the_stock_writer);
    RUN(recsave_off_leaves_deletes_and_op_04);
    RUN(recsave_port_windows_and_refusals);
}
