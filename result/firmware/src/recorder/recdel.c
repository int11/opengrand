/* OpenGrand — internal song delete owned by the core.  See include/ca79ng/recdel.h for the song format. */
#include "ca79ng/recdel.h"

#define H_PARTSET 0x20u   /* per-part settings block offset (0 = none) */
#define H_SETUP 0x24u     /* start settings block offset (0 = none) */
#define H_TRACKS 0x2Cu    /* 16 track offsets (0 = none) */
#define H_END 0x6Cu       /* track data end */
#define H_BARS 0x70u      /* FA count of the longest track */

static uint32_t get32(const uint8_t *p)
{
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

static void put32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)v, p[1] = (uint8_t)(v >> 8), p[2] = (uint8_t)(v >> 16), p[3] = (uint8_t)(v >> 24);
}

uint8_t rd_judge(const rv_view_t *v, uint8_t fmt, uint16_t song, uint8_t part)
{
    if (fmt != 0x04 && fmt != 0x7F)
        return RD_V_STOCK;   /* 0x40160AD4: dropped */
    if (song != RD_ALL_SONGS && song >= RV_SONGS)
        return RD_V_ARG;     /* 0x40160B0C cpyge: song 1 */
    if (song != RD_ALL_SONGS && part != 0 && part != 1 && part != 0x7F)
        return RD_V_STOCK;   /* FUN_4016C800 returns 0 without a write, the stock answers 32 51 7E */
    if ((v->state != RV_ST_STOPPED0 && v->state != RV_ST_STOP) ||
        (v->busy & (RV_BUSY_JOB | RV_BUSY_WORKER | RV_BUSY_FILEOP | RV_BUSY_REC)))
        return RD_V_BUSY;    /* a take / a playing song / a write in flight: the buffer and the table are in use */
    if (song == RD_ALL_SONGS)
        return RD_V_STOCK;
    if (v->store != RV_STORE_OK)
        return RD_V_BUSY;    /* FUN_40168408 would move songs by a broken table; format is the way out */
    return RD_V_OWN;
}

/* FUN_40167EC0's event lengths (the serialiser's copier, REA/objdump 0x40167EC0..0x4016801C) */
static uint32_t ev_len(const uint8_t *e, uint32_t n)
{
    uint8_t b = e[0];
    uint32_t l;
    if (b < 0x80) {   /* note [key, step, b2, b3] (+2 when b2 == FE) (+2 when b3 == FD) */
        if (n < 4)
            return 0;
        return 4u + (e[2] == 0xFE ? 2u : 0u) + (e[3] == 0xFD ? 2u : 0u);
    }
    if (b == 0xF0) {  /* F0, one byte, then bytes < 0x80 up to and with the first >= 0x80 */
        for (l = 2; l < n; l++)
            if (e[l] >= 0x80)
                return l + 1;
        return 0;
    }
    if (b == 0xFA)
        return 2;     /* bar [FA, beats] */
    if (b >= 0xF0)
        return 1;     /* F8 beat, FC end, the rest */
    switch (b & 0xF0) {
    case 0xA0: case 0xB0: case 0xE0: return 4;
    case 0xC0: case 0xD0: return 3;
    default: return 1;   /* 80 / 90: one byte (the copier's default) */
    }
}

uint32_t rd_track_bars(const uint8_t *t, uint32_t n)
{
    uint32_t i = 0, bars = 0;
    if (n < 2 || t[0] != 0xFA)
        return 0;
    while (i < n) {
        uint32_t l;
        if (t[i] == 0xFC)
            return bars;
        if (t[i] == 0xFA)
            bars++;
        l = ev_len(t + i, n - i);
        if (l == 0 || l > n - i)
            return 0;
        i += l;
    }
    return 0;   /* no FC inside the track */
}

static uint8_t empty_song(const uint8_t *empty, uint8_t *out, uint32_t cap, uint32_t *out_len)
{
    if (cap < RD_HDR)
        return RD_E_ROOM;
    for (uint32_t i = 0; i < RD_HDR; i++)
        out[i] = empty[i];
    *out_len = RD_HDR;
    return RD_OK;
}

/* an offset field is 0 or inside the song after the header */
static int off_ok(uint32_t o, uint32_t len) { return o == 0 || (o >= RD_HDR && o < len); }

uint8_t rd_build(const uint8_t *song, uint32_t len, uint8_t part, const uint8_t *empty, uint8_t *out, uint32_t cap,
                 uint32_t *out_len)
{
    uint32_t offs[RV_TRACKS], start, end, cut, pad, data_end, bars = 0, left = 0;
    *out_len = 0;
    if (len < RD_HDR || (len & 3u))
        return RD_E_SONG;
    if (part != 0 && part != 1 && part != 0x7F)
        return RD_NOTHING;
    data_end = get32(song + H_END);
    if (!off_ok(get32(song + H_PARTSET), len) || !off_ok(get32(song + H_SETUP), len) ||
        (data_end != 0 && (data_end < RD_HDR || data_end > len)))
        return RD_E_SONG;
    for (uint32_t t = 0; t < RV_TRACKS; t++) {
        offs[t] = get32(song + H_TRACKS + 4u * t);
        if (!off_ok(offs[t], len) || (offs[t] && song[offs[t]] != 0xFA))
            return RD_E_SONG;
        if (offs[t] && !(part == 0x7F ? t < 2 : t == part))
            left++;
    }
    if (part == 0x7F) {
        if (!offs[0] && !offs[1] && len == RD_HDR)
            return RD_NOTHING;   /* already the empty song */
        /* KSO-imported slots 2-15 go with it: "Delete" is the whole song */
        return empty_song(empty, out, cap, out_len);
    }
    start = offs[part];
    if (!start)
        return RD_NOTHING;
    if (!left)
        return empty_song(empty, out, cap, out_len);   /* the last track: the song is empty */
    /* the track runs to the next block / track / data end after it (each track is padded to 4, FUN_40167E98) */
    end = len;
    {
        uint32_t cand[3] = {get32(song + H_PARTSET), get32(song + H_SETUP), data_end};
        for (uint32_t k = 0; k < 3; k++)
            if (cand[k] > start && cand[k] < end)
                end = cand[k];
        for (uint32_t t = 0; t < RV_TRACKS; t++)
            if (offs[t] > start && offs[t] < end)
                end = offs[t];
    }
    cut = end - start;
    /* the song length must stay 4-aligned (the boot check's song starts): zero padding at the end when the cut is
     * not (a track the serialiser wrote is padded, FUN_40167E98; one from a KSO need not be - the stock reads bytes) */
    pad = (0u - (len - cut)) & 3u;
    if (len - cut + pad > cap)
        return RD_E_ROOM;
    for (uint32_t i = 0; i < start; i++)
        out[i] = song[i];
    for (uint32_t i = end; i < len; i++)
        out[i - cut] = song[i];
    /* offsets after the cut move down; the removed slot becomes 0 */
    {
        static const uint32_t fields[3] = {H_PARTSET, H_SETUP, H_END};
        for (uint32_t k = 0; k < 3; k++) {
            uint32_t o = get32(song + fields[k]);
            if (o > start)
                put32(out + fields[k], o - cut);
        }
    }
    for (uint32_t t = 0; t < RV_TRACKS; t++) {
        uint32_t o = t == part ? 0 : offs[t], b;
        if (o > start)
            o -= cut;
        put32(out + H_TRACKS + 4u * t, o);
        if (!o)
            continue;
        /* bars of a track left (the walk stops at its FC; it may not run past the song) */
        b = rd_track_bars(out + o, len - cut - o);
        if (b == 0)
            return RD_E_SONG;
        if (b > bars)
            bars = b;
    }
    put32(out + H_BARS, bars);
    for (uint32_t i = 0; i < pad; i++)
        out[len - cut + i] = 0;
    *out_len = len - cut + pad;
    return RD_OK;
}
