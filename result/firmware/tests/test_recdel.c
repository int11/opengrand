/* Tests for the core's internal song delete (src/recorder/recdel.c).
 * The stock-image emulator run (the core's delete inside ota_main against the stock writer) is
 * result/stockfw/tools/roundtrip_recdel.py. */
#include <stdint.h>
#include <string.h>

#include "ca79ng/recdel.h"
#include "test.h"

static void put32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)v, p[1] = (uint8_t)(v >> 8), p[2] = (uint8_t)(v >> 16), p[3] = (uint8_t)(v >> 24);
}

static uint32_t get32(const uint8_t *p)
{
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

/* a track of `bars` bars, one note per bar, FC, padded to 4 */
static uint32_t track(uint8_t *t, uint32_t bars, uint8_t key)
{
    uint32_t n = 0;
    for (uint32_t b = 0; b < bars; b++) {
        t[n++] = 0xFA, t[n++] = 0x04;
        t[n++] = key, t[n++] = 0x10, t[n++] = 0x64, t[n++] = 0x30;    /* note, 4 B */
        t[n++] = 0xF8;                                                  /* a beat */
        t[n++] = 0xB0, t[n++] = 0x00, t[n++] = 0x40, t[n++] = 0x7F;    /* pedal CC, 4 B */
    }
    t[n++] = key, t[n++] = 0x20, t[n++] = 0xFE, t[n++] = 0x05, t[n++] = 0x01, t[n++] = 0x02;   /* 6 B note */
    t[n++] = 0xFC;
    while (n & 3u)
        t[n++] = 0;
    return n;
}

static const char NAME[] = "SONG 03                         ";

/* name, a start-settings block at +0x74 (FC-terminated, 8 B), tracks in slot order given by bars[16] (0 = none) */
static uint32_t song_make(uint8_t *s, const uint32_t bars[16])
{
    uint32_t o = RD_HDR, max = 0;
    memset(s, 0, 0x2000);
    memcpy(s, NAME, 32);
    put32(s + 0x24, o);
    memcpy(s + o, "\x01\x02\x03\x04\x05\x06\x07\xFC", 8);
    o += 8;
    put32(s + 0x28, 16);
    for (int t = 0; t < 16; t++) {
        if (!bars[t])
            continue;
        put32(s + 0x2C + 4 * t, o);
        o += track(s + o, bars[t], (uint8_t)(60 + t));
        if (bars[t] > max)
            max = bars[t];
    }
    put32(s + 0x6C, o);
    put32(s + 0x70, max);
    return o;
}

static uint8_t empty_hdr[RD_HDR];

static void empty_make(void)
{
    memset(empty_hdr, 0, sizeof empty_hdr);
    memcpy(empty_hdr, "SONG 03 ", 8);
    put32(empty_hdr + 0x28, 16);
}

TEST(recdel_track_bars_follow_the_stock_copier)
{
    uint8_t t[256];
    uint32_t n = track(t, 3, 60);
    CHECK_EQ(rd_track_bars(t, n), 3);
    CHECK_EQ(rd_track_bars(t, 5), 0);            /* runs past the end without FC */
    t[0] = 0x3C;
    CHECK_EQ(rd_track_bars(t, n), 0);            /* not starting with FA */
    /* F0 sysex event: F0, one byte, data < 0x80, terminator >= 0x80; C0 3 B, 90 1 B */
    {
        const uint8_t u[] = {0xFA, 0x04, 0xF0, 0x00, 0x11, 0x22, 0xF7, 0xC0, 0x01, 0x05, 0x90, 0xFA, 0x03, 0xFC};
        CHECK_EQ(rd_track_bars(u, sizeof u), 2);
    }
    {
        const uint8_t u[] = {0xFA, 0x04, 0x3C, 0x01, 0x40, 0xFD, 0xFA, 0xFA, 0xFA, 0x04, 0xFC};   /* b3 FD: +2 hides FA FA */
        CHECK_EQ(rd_track_bars(u, sizeof u), 2);
    }
}

TEST(recdel_part2_of_a_two_part_song)
{
    static uint8_t s[0x2000], o[0x2000];
    uint32_t bars[16] = {4, 7}, len = song_make(s, bars), n = 0, t1 = get32(s + 0x30), cut = get32(s + 0x6C) - t1;
    CHECK_EQ(rd_build(s, len, 1, empty_hdr, o, sizeof o, &n), RD_OK);
    CHECK_EQ(n, len - cut);
    CHECK_EQ(get32(o + 0x2C), RD_HDR + 8);       /* part 1 where it was */
    CHECK_EQ(get32(o + 0x30), 0);                /* part 2 gone */
    CHECK_EQ(get32(o + 0x6C), t1);               /* data end at the cut */
    CHECK_EQ(get32(o + 0x70), 4);                /* bars of what is left */
    CHECK_MEM(o, s, 0x2C);                       /* name, blocks, slot count untouched */
    CHECK_MEM(o + RD_HDR, s + RD_HDR, t1 - RD_HDR);
    CHECK_EQ(n & 3u, 0);
}

TEST(recdel_part1_moves_part2_down)
{
    static uint8_t s[0x2000], o[0x2000];
    uint32_t bars[16] = {4, 7}, len = song_make(s, bars), n = 0, t0 = get32(s + 0x2C), t1 = get32(s + 0x30);
    CHECK_EQ(rd_build(s, len, 0, empty_hdr, o, sizeof o, &n), RD_OK);
    CHECK_EQ(n, len - (t1 - t0));
    CHECK_EQ(get32(o + 0x2C), 0);
    CHECK_EQ(get32(o + 0x30), t0);               /* part 2 now where part 1 began */
    CHECK_EQ(o[t0], 0xFA);
    CHECK_MEM(o + t0, s + t1, len - t1);
    CHECK_EQ(get32(o + 0x6C), get32(s + 0x6C) - (t1 - t0));
    CHECK_EQ(get32(o + 0x70), 7);
    CHECK_EQ(get32(o + 0x24), RD_HDR);           /* the settings block before the cut: same offset */
}

TEST(recdel_whole_song_and_last_part_give_the_template)
{
    static uint8_t s[0x2000], o[0x2000];
    uint32_t bars[16] = {4, 7}, len = song_make(s, bars), n = 0;
    CHECK_EQ(rd_build(s, len, 0x7F, empty_hdr, o, sizeof o, &n), RD_OK);
    CHECK_EQ(n, RD_HDR);
    CHECK_MEM(o, empty_hdr, RD_HDR);
    uint32_t one[16] = {0, 5};
    len = song_make(s, one);
    CHECK_EQ(rd_build(s, len, 1, empty_hdr, o, sizeof o, &n), RD_OK);   /* the only track */
    CHECK_EQ(n, RD_HDR);
    CHECK_EQ(rd_build(s, len, 0, empty_hdr, o, sizeof o, &n), RD_NOTHING);   /* no part 1 */
    CHECK_EQ(n, 0);
    CHECK_EQ(rd_build(empty_hdr, RD_HDR, 0x7F, empty_hdr, o, sizeof o, &n), RD_NOTHING);   /* already empty */
    CHECK_EQ(rd_build(s, len, 2, empty_hdr, o, sizeof o, &n), RD_NOTHING);
}

/* tracks not padded to 4 (a KSO need not be; tools/stock_songs.py's synthetic songs are not): the song is padded back */
TEST(recdel_unpadded_tracks)
{
    static uint8_t s[0x200], o[0x200];
    static const uint8_t t0[] = {0xFA, 0x23, 0x3C, 0x00, 0x40, 0x30, 0xF8, 0xFC};                 /* 8 B */
    static const uint8_t t1[] = {0xFA, 0x23, 0x3D, 0x00, 0x40, 0x30, 0xF8, 0xFA, 0x23, 0xFC};    /* 10 B, 2 bars */
    uint32_t n = 0, len;
    memset(s, 0, sizeof s);
    put32(s + 0x24, 0x74);
    memcpy(s + 0x74, "\x07\xA1\x20\x00\xFC", 5);
    put32(s + 0x2C, 0x79);
    memcpy(s + 0x79, t0, sizeof t0);
    put32(s + 0x30, 0x81);
    memcpy(s + 0x81, t1, sizeof t1);
    put32(s + 0x6C, 0x8B);
    put32(s + 0x70, 2);
    len = 0x8C;
    CHECK_EQ(rd_build(s, len, 0, empty_hdr, o, sizeof o, &n), RD_OK);   /* cut 8: stays aligned */
    CHECK_EQ(n, 0x84);
    CHECK_EQ(get32(o + 0x30), 0x79);
    CHECK_EQ(get32(o + 0x70), 2);
    CHECK_EQ(rd_build(s, len, 1, empty_hdr, o, sizeof o, &n), RD_OK);   /* cut 10 -> 0x82, padded to 0x84 */
    CHECK_EQ(n, 0x84);
    CHECK_EQ(get32(o + 0x6C), 0x81);
    CHECK_EQ(get32(o + 0x70), 1);
    CHECK_EQ(o[0x81] | o[0x82] | o[0x83], 0);
}

TEST(recdel_kso_slots_stay_with_a_part_delete)
{
    static uint8_t s[0x2000], o[0x2000];
    uint32_t bars[16] = {3, 0, 2, 0, 0, 9}, len = song_make(s, bars), n = 0, t0 = get32(s + 0x2C), t2 = get32(s + 0x34);
    CHECK_EQ(rd_build(s, len, 0, empty_hdr, o, sizeof o, &n), RD_OK);
    CHECK_EQ(get32(o + 0x34), t0);
    CHECK_EQ(get32(o + 0x40), get32(s + 0x40) - (t2 - t0));
    CHECK_EQ(get32(o + 0x70), 9);
}

TEST(recdel_refuses_what_it_cannot_read)
{
    static uint8_t s[0x2000], o[0x2000];
    uint32_t bars[16] = {4, 7}, len = song_make(s, bars), n = 0;
    CHECK_EQ(rd_build(s, len - 2, 1, empty_hdr, o, sizeof o, &n), RD_E_SONG);   /* a song length not 4-aligned */
    CHECK_EQ(rd_build(s, 0x40, 1, empty_hdr, o, sizeof o, &n), RD_E_SONG);      /* shorter than a header */
    uint32_t keep = get32(s + 0x30);
    put32(s + 0x30, len + 4);                                                   /* a track outside the song */
    CHECK_EQ(rd_build(s, len, 0, empty_hdr, o, sizeof o, &n), RD_E_SONG);
    put32(s + 0x30, keep);
    s[keep] = 0x3C;                                                             /* a track not starting with FA */
    CHECK_EQ(rd_build(s, len, 0, empty_hdr, o, sizeof o, &n), RD_E_SONG);
    s[keep] = 0xFA;
    for (uint32_t i = keep; i < len; i++)                                       /* part 2 loses its FC */
        if (s[i] == 0xFC)
            s[i] = 0xF8;
    CHECK_EQ(rd_build(s, len, 0, empty_hdr, o, sizeof o, &n), RD_E_SONG);      /* the track left cannot be walked */
    len = song_make(s, bars);
    CHECK_EQ(rd_build(s, len, 1, empty_hdr, o, 0x80, &n), RD_E_ROOM);
}

TEST(recdel_judge)
{
    rv_view_t v;
    memset(&v, 0, sizeof v);
    v.state = RV_ST_STOP;
    CHECK_EQ(rd_judge(&v, 0x04, 3, 0x7F), RD_V_OWN);
    CHECK_EQ(rd_judge(&v, 0x7F, 3, 0), RD_V_OWN);
    CHECK_EQ(rd_judge(&v, 0x00, 3, 0x7F), RD_V_STOCK);         /* fmt: dropped by the stock */
    CHECK_EQ(rd_judge(&v, 0x04, 3, 5), RD_V_STOCK);            /* part: the stock writes nothing */
    CHECK_EQ(rd_judge(&v, 0x7F, RD_ALL_SONGS, 0x7F), RD_V_STOCK);   /* format: the stock's real one */
    CHECK_EQ(rd_judge(&v, 0x04, 11, 0x7F), RD_V_ARG);
    v.state = RV_ST_STOPPED0;
    CHECK_EQ(rd_judge(&v, 0x04, 3, 0x7F), RD_V_OWN);
    for (uint8_t st = RV_ST_PLAY; st <= RV_ST_COUNT_IN; st++) {
        if (st == RV_ST_STOP)
            continue;
        v.state = st;
        CHECK_EQ(rd_judge(&v, 0x04, 3, 0x7F), RD_V_BUSY);
        CHECK_EQ(rd_judge(&v, 0x7F, RD_ALL_SONGS, 0x7F), RD_V_BUSY);
    }
    v.state = RV_ST_STOP;
    v.busy = RV_BUSY_JOB;
    CHECK_EQ(rd_judge(&v, 0x04, 3, 0x7F), RD_V_BUSY);
    v.busy = RV_BUSY_FILEOP;
    CHECK_EQ(rd_judge(&v, 0x04, 3, 0x7F), RD_V_BUSY);
    v.busy = 0;
    v.store = RV_STORE_TRACK_FA;
    CHECK_EQ(rd_judge(&v, 0x04, 3, 0x7F), RD_V_BUSY);
    CHECK_EQ(rd_judge(&v, 0x7F, RD_ALL_SONGS, 0x7F), RD_V_STOCK);   /* a broken table can still be formatted */
}


/* R3 repair: the song the stock commit writes for a take of part 2 with no events on an empty song (field 2026-11-24,
 * emulator tools/roundtrip_recfix.py): name "- SoundMode", +0x24 0, slot 0 = 0x74 = the song length, +0x6C 0x74 */
TEST(recdel_repair_broken_empty_take)
{
    static uint8_t s[0x100], out[0x100];
    uint32_t len = 0;
    memset(s, 0, sizeof s);
    memcpy(s, "SONG 06 - SoundMode             ", 32);
    put32(s + 0x28, 16), put32(s + 0x2C, RD_HDR), put32(s + 0x6C, RD_HDR);
    CHECK_EQ(rd_repair(s, RD_HDR, empty_hdr, out, sizeof out, &len), RD_OK);
    CHECK_EQ(len, RD_HDR);
    CHECK(memcmp(out, empty_hdr, RD_HDR) == 0);   /* the boot template: what the core's delete writes for an empty song */
    /* part 2 only, the same take: slot 0 = the length again (124 B in the emulator) */
    put32(s + 0x24, RD_HDR), memcpy(s + RD_HDR, "\x00\x00\x00\x78\xFC\0\0\0", 8), put32(s + 0x2C, RD_HDR + 8);
    put32(s + 0x6C, RD_HDR + 8);
    CHECK_EQ(rd_repair(s, RD_HDR + 8, empty_hdr, out, sizeof out, &len), RD_OK);
    CHECK_EQ(len, RD_HDR);
}

TEST(recdel_repair_keeps_good_tracks)
{
    static uint8_t s[0x2000], out[0x2000];
    uint32_t bars[16] = {3, 0}, len = 0, n = song_make(s, bars);
    /* fine as it is: nothing to mend */
    CHECK_EQ(rd_repair(s, n, empty_hdr, out, sizeof out, &len), RD_NOTHING);
    /* slot 1 points at the end (outside), slot 2 at a non-FA byte: both dropped, part 1 kept, same length */
    put32(s + 0x30, n), put32(s + 0x34, get32(s + 0x2C) + 2);
    put32(s + 0x70, 9);
    CHECK_EQ(rd_repair(s, n, empty_hdr, out, sizeof out, &len), RD_OK);
    CHECK_EQ(len, n);
    CHECK_EQ(get32(out + 0x2C), get32(s + 0x2C));
    CHECK_EQ(get32(out + 0x30), 0);
    CHECK_EQ(get32(out + 0x34), 0);
    CHECK_EQ(get32(out + 0x70), 3);   /* bars counted again over what is left */
    CHECK(memcmp(out + RD_HDR, s + RD_HDR, n - RD_HDR) == 0);
}

TEST(recdel_repair_refuses_what_it_cannot_read)
{
    static uint8_t s[0x2000], out[0x2000];
    uint32_t bars[16] = {3, 0}, len = 0, n = song_make(s, bars);
    put32(s + 0x30, n);                 /* one slot to drop */
    CHECK_EQ(rd_repair(s, n - 1, empty_hdr, out, sizeof out, &len), RD_E_SONG);   /* length not 4-aligned */
    put32(s + 0x24, n + 4);
    CHECK_EQ(rd_repair(s, n, empty_hdr, out, sizeof out, &len), RD_E_SONG);       /* settings block outside */
    put32(s + 0x24, RD_HDR);
    s[get32(s + 0x2C) + n - get32(s + 0x2C) - 4] = 0;   /* keep the FA, break the FC: the kept track cannot be walked */
    memset(s + get32(s + 0x2C) + 2, 0xF8, n - get32(s + 0x2C) - 2);
    CHECK_EQ(rd_repair(s, n, empty_hdr, out, sizeof out, &len), RD_E_SONG);
    CHECK_EQ(rd_repair(s, n, empty_hdr, out, 0x10, &len) != RD_OK, 1);
}

void suite_recdel(void)
{
    printf("internal song delete (recdel)\n");
    empty_make();
    RUN(recdel_track_bars_follow_the_stock_copier);
    RUN(recdel_part2_of_a_two_part_song);
    RUN(recdel_part1_moves_part2_down);
    RUN(recdel_whole_song_and_last_part_give_the_template);
    RUN(recdel_unpadded_tracks);
    RUN(recdel_kso_slots_stay_with_a_part_delete);
    RUN(recdel_refuses_what_it_cannot_read);
    RUN(recdel_judge);
    RUN(recdel_repair_broken_empty_take);
    RUN(recdel_repair_keeps_good_tracks);
    RUN(recdel_repair_refuses_what_it_cannot_read);
}
