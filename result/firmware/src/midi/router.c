#include "ca79ng/router.h"

#include <string.h>

void midi_cfg_default(midi_cfg_t *c)
{
    memset(c, 0, sizeof *c);
    c->channel = 0;
    c->omni = true;
    c->local = true;
    c->multitimbral = MT_OFF;
    for (int i = 0; i < 16; i++)
        c->channel_play[i] = true;
    c->send_pc = true;
    c->tx_port[MIDI_PORT_DIN] = true;
    c->tx_port[MIDI_PORT_USB] = true;
    c->tx_port[MIDI_PORT_BLE] = true;
    c->tx_port[MIDI_PORT_PANEL] = false; /* the panel gets parameter SysEx, not the note stream */
}

void router_init(router_t *r, const router_io_t *io)
{
    memset(r, 0, sizeof *r);
    midi_cfg_default(&r->cfg);
    r->io = *io;
}

void router_set_channel(router_t *r, uint8_t ch)
{
    r->cfg.channel = ch & 15;
    r->cfg.omni = false;
}

static void tx_all(router_t *r, const uint8_t *b, uint16_t n)
{
    if (!r->io.tx)
        return;
    for (int p = 0; p < MIDI_PORT_INTERNAL; p++)
        if (r->cfg.tx_port[p])
            r->io.tx(r->io.ctx, (midi_port_t)p, b, n);
}

static uint8_t next_ch(uint8_t ch) { return (uint8_t)((ch + 1) & 15); }

/* Which transmit channels a keyboard part uses. */
static int tx_channels(const router_t *r, uint8_t part, uint8_t out[2])
{
    uint8_t ch = r->cfg.channel;
    if (r->cfg.multitimbral == MT_OFF) {
        out[0] = ch;
        return 1;
    }
    switch (part) {
    case PART_LOWER:
    case PART_LAYER:
    case PART_4H_LEFT:
        out[0] = next_ch(ch);
        return 1;
    default:
        out[0] = ch;
        return 1;
    }
}

void router_key_note(router_t *r, uint8_t part, uint8_t note, uint8_t vel)
{
    if (r->cfg.local && r->io.note)
        r->io.note(r->io.ctx, part, note, vel);
    uint8_t chs[2];
    int n = tx_channels(r, part, chs);
    /* In Dual mode with multi-timbral off both parts would send the same note
     * twice on one channel; only the main part transmits in that case. */
    if (r->cfg.multitimbral == MT_OFF && part == PART_LAYER)
        return;
    for (int i = 0; i < n; i++) {
        uint8_t m[3] = {(uint8_t)((vel ? 0x90 : 0x80) | chs[i]), note, vel ? vel : 64};
        tx_all(r, m, 3);
    }
}

void router_key_control(router_t *r, uint8_t part, uint8_t cc, uint8_t value)
{
    if (r->cfg.local && r->io.control)
        r->io.control(r->io.ctx, part, cc, value);
    if (part == PART_LAYER && r->cfg.multitimbral == MT_OFF)
        return;
    uint8_t chs[2];
    int n = tx_channels(r, part, chs);
    for (int i = 0; i < n; i++) {
        uint8_t m[3] = {(uint8_t)(0xB0 | chs[i]), cc, value};
        tx_all(r, m, 3);
    }
}

void router_key_program(router_t *r, uint8_t msb, uint8_t lsb, uint8_t pc)
{
    if (!r->cfg.send_pc)
        return;
    uint8_t ch = r->cfg.channel;
    uint8_t m[8] = {(uint8_t)(0xB0 | ch), 0, msb, (uint8_t)(0xB0 | ch), 32, lsb, (uint8_t)(0xC0 | ch), pc};
    tx_all(r, m, 8);
}

/* Receive-side part for a channel: with multi-timbral off everything plays
 * the keyboard's main part (in omni or on the selected channel). */
static int rx_part(const router_t *r, uint8_t ch)
{
    if (r->cfg.multitimbral == MT_OFF) {
        if (!r->cfg.omni && ch != r->cfg.channel)
            return -1;
        return PART_MAIN1;
    }
    if (!r->cfg.channel_play[ch])
        return -1;
    if (ch == r->cfg.channel)
        return PART_MAIN1;
    return PART_MIDI_BASE + ch;
}

void router_rx(router_t *r, const midi_msg_t *m)
{
    if (m->kind != MIDI_MSG_CHANNEL)
        return;
    uint8_t type = m->status & 0xF0, ch = m->status & 0x0F;
    int part = rx_part(r, ch);
    if (part < 0)
        return;
    switch (type) {
    case 0x80:
        if (r->io.note) r->io.note(r->io.ctx, (uint8_t)part, m->data[0], 0);
        break;
    case 0x90:
        if (r->io.note) r->io.note(r->io.ctx, (uint8_t)part, m->data[0], m->data[1]);
        break;
    case 0xB0:
        switch (m->data[0]) {
        case 0: r->bank_msb[ch] = m->data[1]; break;
        case 32: r->bank_lsb[ch] = m->data[1]; break;
        case 7: case 10: case 11: case 64: case 66: case 67:
            if (r->io.control) r->io.control(r->io.ctx, (uint8_t)part, m->data[0], m->data[1]);
            break;
        case 122: /* Local Control */
            r->cfg.local = m->data[1] >= 64;
            break;
        case 120: case 123: case 124: case 125: case 126: case 127:
            if (r->io.all_notes_off) r->io.all_notes_off(r->io.ctx, (uint8_t)part);
            break;
        default:
            break;
        }
        break;
    case 0xC0:
        if (r->io.program)
            r->io.program(r->io.ctx, (uint8_t)part, r->bank_msb[ch], r->bank_lsb[ch], m->data[0]);
        break;
    default: /* aftertouch / pitch bend: not recognised (implementation chart) */
        break;
    }
}

void router_tx_sysex(router_t *r, midi_port_t port, const uint8_t *msg, uint16_t len)
{
    if (!r->io.tx || len > MIDI_SYSEX_TX_MAX)
        return;
    if (port == MIDI_PORT_COUNT)
        tx_all(r, msg, len);
    else if (port < MIDI_PORT_INTERNAL)
        r->io.tx(r->io.ctx, port, msg, len);
}
