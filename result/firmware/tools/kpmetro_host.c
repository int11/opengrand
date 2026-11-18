/* Host shim for result/stockfw/tools/roundtrip_metro.py: the core's metronome module (kpmetro.c) and a
 * parameter store as a shared library (build/libkpmetro.so, ctypes) with flat int arguments.  Test tool only. */
#include <string.h>

#include "ca79ng/kpmetro.h"

static kpm_t m;
static param_store_t store;

static void cfg_out(const kpm_cfg_t *c, uint8_t out[8])
{
    /* the stock block bytes +0x49..+0x50 (pad +0x4D = 0, tempo little-endian) */
    out[0] = c->on;
    out[1] = c->mode;
    out[2] = c->rhythm;
    out[3] = c->beat;
    out[4] = 0;
    out[5] = (uint8_t)(c->tempo & 0xFF);
    out[6] = (uint8_t)(c->tempo >> 8);
    out[7] = c->volume;
}

/* cfg = block bytes +0x49..+0x50 */
void kmh_init(const uint8_t cfg[8])
{
    kpm_cfg_t c = {cfg[0], cfg[1], cfg[2], cfg[3], (uint16_t)(cfg[5] | cfg[6] << 8), cfg[7]};
    kpm_init(&m, &c);
    memset(&store, 0, sizeof store);
}

/* a set: info = {verdict, fx, bpm}; cfg = block after; frame = BeatCounter 1 if any; returns the frame length */
int kmh_set(int v2, const uint8_t *d, int n, uint8_t cfg[8], uint8_t frame[KPM_FRAME_LEN], uint32_t info[3])
{
    kpm_result_t r;
    uint32_t k = kpm_set(&m, (uint8_t)v2, d, (uint32_t)n, &r, frame);
    info[0] = r.verdict;
    info[1] = r.fx;
    info[2] = r.bpm;
    cfg_out(&m.cfg, cfg);
    return (int)k;
}

/* the 2 ms poll: frames into out (2 x 12), their count */
int kmh_poll(int pos, int len, int half, int beat, int bpb, int on, int reload, int rec, uint8_t out[2 * KPM_FRAME_LEN])
{
    kpm_run_t r = {(int16_t)pos, (int16_t)len, (int16_t)half, (int8_t)beat, (uint8_t)bpb, (uint8_t)on, (uint8_t)reload, (uint8_t)rec};
    uint8_t f[2][KPM_FRAME_LEN];
    uint32_t k = kpm_poll(&m, &r, f);
    memcpy(out, f, sizeof f);
    return (int)k;
}

/* the module's values into the store, then the store's bytes for 56 v2 7F: length, -1 not a core key, 0 none */
int kmh_store_get(int v2, uint8_t out[4])
{
    int id = param_find(0x10, 0x56, (uint8_t)v2, 0x7F), slot = param_slot_from_part(0x7F);
    kpm_store(&m.cfg, &store);
    if (id < 0 || slot < 0)
        return -1;
    if (!param_get(&store, (param_id_t)id, slot, out))
        return 0;
    return PARAM_DESC[id].len;
}

int kmh_beat_index(int num, int den) { return kpm_beat_index((uint8_t)num, (uint8_t)den); }

int kmh_accum(int tempo, uint32_t qr[2]) { return kpm_accum((uint16_t)tempo, &qr[0], &qr[1]); }

int kmh_stock_ports(int binding, int forward) { return kpm_stock_ports((uint8_t)binding, forward != 0); }

void kmh_beat_entry(int index, uint32_t out[2])
{
    uint16_t t;
    uint8_t b;
    kpm_beat_entry((uint8_t)index, &t, &b);
    out[0] = t;
    out[1] = b;
}

/* sets ignored refused beats starts reloads resyncs */
void kmh_stats(uint32_t out[7])
{
    out[0] = m.st.sets;
    out[1] = m.st.ignored;
    out[2] = m.st.refused;
    out[3] = m.st.beats;
    out[4] = m.st.starts;
    out[5] = m.st.reloads;
    out[6] = m.st.resyncs;
}
