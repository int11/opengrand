/* OpenGrand — small shared utilities (ring buffer, CRC32, SHA-256, clamp helpers). */
#ifndef CA79NG_UTIL_H
#define CA79NG_UTIL_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define CA_ARRAY_LEN(a) (sizeof(a) / sizeof((a)[0]))
#define CA_MIN(a, b) ((a) < (b) ? (a) : (b))
#define CA_MAX(a, b) ((a) > (b) ? (a) : (b))

static inline int ca_clampi(int v, int lo, int hi) { return v < lo ? lo : (v > hi ? hi : v); }

/* ---- Single-producer / single-consumer byte ring (ISR -> event loop). ----
 * Capacity must be a power of two. head is written only by the producer,
 * tail only by the consumer, so no lock is needed on a single core. */
typedef struct {
    uint8_t *buf;
    uint32_t mask;
    volatile uint32_t head;
    volatile uint32_t tail;
    uint32_t dropped; /* bytes rejected because the ring was full */
} ca_ring_t;

void ca_ring_init(ca_ring_t *r, uint8_t *storage, uint32_t capacity_pow2);
bool ca_ring_put(ca_ring_t *r, uint8_t b);
bool ca_ring_get(ca_ring_t *r, uint8_t *out);
uint32_t ca_ring_count(const ca_ring_t *r);

/* ---- Checksums ---- */
uint32_t ca_crc32(uint32_t crc, const void *data, size_t len); /* start with crc = 0 */

typedef struct {
    uint32_t state[8];
    uint64_t bitlen;
    uint8_t data[64];
    uint32_t datalen;
} ca_sha256_t;

void ca_sha256_init(ca_sha256_t *c);
void ca_sha256_update(ca_sha256_t *c, const void *data, size_t len);
void ca_sha256_final(ca_sha256_t *c, uint8_t out[32]);

#endif
