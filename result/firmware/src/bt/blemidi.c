#include <string.h>

#include "ca79ng/bt.h"

static const uint8_t HDR[6] = {0xF0, 0x00, 0x02, 0x08, 0x10, 0x55};

size_t blemidi_cmd(uint8_t *out, uint8_t cmd, uint8_t sub, const uint8_t *data, size_t n)
{
    size_t o = 0;
    memcpy(out, HDR, sizeof HDR);
    o = sizeof HDR;
    out[o++] = cmd;
    out[o++] = sub;
    for (size_t i = 0; i < n && o < 30; i++)
        out[o++] = data[i] & 0x7F;
    out[o++] = 0xF7;
    return o;
}

static void send(blemidi_t *b, uint8_t cmd, uint8_t sub, const uint8_t *d, size_t n)
{
    uint8_t m[32];
    size_t l = blemidi_cmd(m, cmd, sub, d, n);
    if (b->io.uart_tx)
        b->io.uart_tx(b->io.ctx, m, (uint16_t)l);
}

void blemidi_init(blemidi_t *b, const blemidi_io_t *io, const char *name)
{
    memset(b, 0, sizeof *b);
    b->io = *io;
    b->enabled = false; /* default Off since panel v3.4.12 ("in accordance with regulations") */
    const uint8_t ts_off[2] = {0x00, 0x00};
    send(b, 0x00, 0x02, ts_off, 2);
    blemidi_set_name(b, name);
}

void blemidi_set_name(blemidi_t *b, const char *name)
{
    uint8_t d[16] = {0};
    size_t n = strlen(name);
    if (n > 16)
        n = 16;
    memcpy(d, name, n);
    memcpy(b->name, name, n);
    b->name[n] = 0;
    send(b, 0x00, 0x01, d, 16);
}

void blemidi_set_enabled(blemidi_t *b, bool on)
{
    if (b->enabled && !on) {
        const uint8_t d = 0x01;
        send(b, 0x7F, 0x01, &d, 1); /* drop the current central */
    }
    b->enabled = on;
}

void blemidi_request_firmware(blemidi_t *b)
{
    const uint8_t d = 0x00;
    send(b, 0x10, 0x04, &d, 1);
}

void blemidi_enter_dfu(blemidi_t *b)
{
    const uint8_t d = 0x01;
    send(b, 0x7F, 0x00, &d, 1);
}

bool blemidi_handle_sysex(blemidi_t *b, const uint8_t *m, uint16_t n)
{
    if (n < 9 || memcmp(m, HDR, sizeof HDR) != 0)
        return false;
    if (m[6] == 0x20 && m[7] == 0x04) {
        size_t k = 0;
        for (uint16_t i = 8; i < n - 1 && k < sizeof b->fw_version - 1; i++)
            if (m[i] >= 0x20)
                b->fw_version[k++] = (char)m[i];
        b->fw_version[k] = 0;
    }
    return true;
}
