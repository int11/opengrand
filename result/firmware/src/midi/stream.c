#include "ca79ng/midi.h"

#include <string.h>

int midi_data_len(uint8_t status)
{
    if (status < 0x80)
        return 0;
    if (status < 0xF0) {
        switch (status & 0xF0) {
        case 0xC0:
        case 0xD0:
            return 1;
        default:
            return 2;
        }
    }
    switch (status) {
    case 0xF0: return -1;
    case 0xF1: /* MTC quarter frame */
    case 0xF3: /* song select */
        return 1;
    case 0xF2: return 2; /* song position */
    default: return 0;   /* F6 tune request, F7, realtime */
    }
}

void midi_parser_init(midi_parser_t *p, midi_port_t port, midi_msg_cb cb, void *ctx)
{
    memset(p, 0, sizeof *p);
    p->port = port;
    p->cb = cb;
    p->ctx = ctx;
}

static void emit(midi_parser_t *p, midi_msg_kind_t kind, uint8_t status)
{
    midi_msg_t m = {.kind = kind, .port = p->port, .status = status};
    m.data[0] = p->data[0];
    m.data[1] = p->data[1];
    if (p->cb)
        p->cb(p->ctx, &m);
}

static void end_sysex(midi_parser_t *p, bool complete)
{
    if (complete && !p->sysex_overflow && p->sysex_len < MIDI_SYSEX_MAX) {
        p->sysex[p->sysex_len++] = 0xF7;
        midi_msg_t m = {.kind = MIDI_MSG_SYSEX, .port = p->port, .status = 0xF0,
                        .sysex = p->sysex, .sysex_len = p->sysex_len};
        if (p->cb)
            p->cb(p->ctx, &m);
    } else {
        p->errors++;
    }
    p->in_sysex = false;
    p->sysex_overflow = false;
    p->sysex_len = 0;
}

void midi_parser_feed(midi_parser_t *p, uint8_t b)
{
    if (b >= 0xF8) { /* realtime: may appear anywhere, even inside SysEx */
        midi_msg_t m = {.kind = MIDI_MSG_REALTIME, .port = p->port, .status = b};
        if (p->cb)
            p->cb(p->ctx, &m);
        return;
    }
    if (p->in_sysex) {
        if (b == 0xF7) {
            end_sysex(p, true);
            return;
        }
        if (b < 0x80) {
            if (p->sysex_len < MIDI_SYSEX_MAX - 1)
                p->sysex[p->sysex_len++] = b;
            else
                p->sysex_overflow = true;
            return;
        }
        end_sysex(p, false); /* any other status aborts the SysEx; fall through */
    }
    if (b & 0x80) {
        if (b == 0xF0) {
            p->in_sysex = true;
            p->sysex_len = 0;
            p->sysex[p->sysex_len++] = 0xF0;
            p->running = 0;
            return;
        }
        if (b == 0xF7) { /* stray end-of-exclusive */
            p->errors++;
            return;
        }
        int n = midi_data_len(b);
        p->have = 0;
        p->need = (uint8_t)n;
        if (b >= 0xF0) { /* system common clears running status */
            p->running = 0;
            if (n == 0) {
                p->data[0] = p->data[1] = 0;
                emit(p, MIDI_MSG_COMMON, b);
            } else {
                p->running = b; /* only until its data arrives */
            }
            return;
        }
        p->running = b;
        return;
    }
    /* data byte */
    if (!p->running) {
        p->errors++;
        return;
    }
    p->data[p->have++] = b;
    if (p->have >= p->need) {
        uint8_t st = p->running;
        if (p->need < 2)
            p->data[1] = 0;
        p->have = 0;
        if (st >= 0xF0) {
            emit(p, MIDI_MSG_COMMON, st);
            p->running = 0;
        } else {
            emit(p, MIDI_MSG_CHANNEL, st);
        }
    }
}

void midi_parser_feed_buf(midi_parser_t *p, const uint8_t *buf, size_t len)
{
    for (size_t i = 0; i < len; i++)
        midi_parser_feed(p, buf[i]);
}
