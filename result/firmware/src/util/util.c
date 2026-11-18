#include "ca79ng/util.h"

#include <string.h>

void ca_ring_init(ca_ring_t *r, uint8_t *storage, uint32_t capacity_pow2)
{
    r->buf = storage;
    r->mask = capacity_pow2 - 1u;
    r->head = r->tail = 0;
    r->dropped = 0;
}

bool ca_ring_put(ca_ring_t *r, uint8_t b)
{
    uint32_t h = r->head;
    if (h - r->tail > r->mask) {
        r->dropped++;
        return false;
    }
    r->buf[h & r->mask] = b;
    r->head = h + 1u;
    return true;
}

bool ca_ring_get(ca_ring_t *r, uint8_t *out)
{
    uint32_t t = r->tail;
    if (t == r->head)
        return false;
    *out = r->buf[t & r->mask];
    r->tail = t + 1u;
    return true;
}

uint32_t ca_ring_count(const ca_ring_t *r) { return r->head - r->tail; }

uint32_t ca_crc32(uint32_t crc, const void *data, size_t len)
{
    const uint8_t *p = data;
    crc = ~crc;
    while (len--) {
        crc ^= *p++;
        for (int k = 0; k < 8; k++)
            crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1u)));
    }
    return ~crc;
}

/* ---- SHA-256 (FIPS 180-4) ---- */
static const uint32_t K256[64] = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
    0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
    0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
    0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
    0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
    0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
    0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
    0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2,
};

#define ROTR(x, n) (((x) >> (n)) | ((x) << (32 - (n))))

static void sha256_block(ca_sha256_t *c, const uint8_t *d)
{
    uint32_t m[64], a, b, cc, dd, e, f, g, h, t1, t2;
    for (int i = 0; i < 16; i++)
        m[i] = (uint32_t)d[i * 4] << 24 | (uint32_t)d[i * 4 + 1] << 16 | (uint32_t)d[i * 4 + 2] << 8 | d[i * 4 + 3];
    for (int i = 16; i < 64; i++) {
        uint32_t s0 = ROTR(m[i - 15], 7) ^ ROTR(m[i - 15], 18) ^ (m[i - 15] >> 3);
        uint32_t s1 = ROTR(m[i - 2], 17) ^ ROTR(m[i - 2], 19) ^ (m[i - 2] >> 10);
        m[i] = m[i - 16] + s0 + m[i - 7] + s1;
    }
    a = c->state[0]; b = c->state[1]; cc = c->state[2]; dd = c->state[3];
    e = c->state[4]; f = c->state[5]; g = c->state[6]; h = c->state[7];
    for (int i = 0; i < 64; i++) {
        t1 = h + (ROTR(e, 6) ^ ROTR(e, 11) ^ ROTR(e, 25)) + ((e & f) ^ (~e & g)) + K256[i] + m[i];
        t2 = (ROTR(a, 2) ^ ROTR(a, 13) ^ ROTR(a, 22)) + ((a & b) ^ (a & cc) ^ (b & cc));
        h = g; g = f; f = e; e = dd + t1; dd = cc; cc = b; b = a; a = t1 + t2;
    }
    c->state[0] += a; c->state[1] += b; c->state[2] += cc; c->state[3] += dd;
    c->state[4] += e; c->state[5] += f; c->state[6] += g; c->state[7] += h;
}

void ca_sha256_init(ca_sha256_t *c)
{
    static const uint32_t iv[8] = {0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
                                   0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
    memcpy(c->state, iv, sizeof iv);
    c->bitlen = 0;
    c->datalen = 0;
}

void ca_sha256_update(ca_sha256_t *c, const void *data, size_t len)
{
    const uint8_t *p = data;
    while (len--) {
        c->data[c->datalen++] = *p++;
        if (c->datalen == 64) {
            sha256_block(c, c->data);
            c->bitlen += 512;
            c->datalen = 0;
        }
    }
}

void ca_sha256_final(ca_sha256_t *c, uint8_t out[32])
{
    uint32_t i = c->datalen;
    c->bitlen += (uint64_t)c->datalen * 8u;
    c->data[i++] = 0x80;
    if (i > 56) {
        while (i < 64) c->data[i++] = 0;
        sha256_block(c, c->data);
        i = 0;
    }
    while (i < 56) c->data[i++] = 0;
    for (int k = 0; k < 8; k++)
        c->data[63 - k] = (uint8_t)(c->bitlen >> (8 * k));
    sha256_block(c, c->data);
    for (int k = 0; k < 8; k++) {
        out[k * 4] = (uint8_t)(c->state[k] >> 24);
        out[k * 4 + 1] = (uint8_t)(c->state[k] >> 16);
        out[k * 4 + 2] = (uint8_t)(c->state[k] >> 8);
        out[k * 4 + 3] = (uint8_t)c->state[k];
    }
}
