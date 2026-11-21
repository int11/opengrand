/* OpenGrand — RAM canary.  See ramcanary.h. */
#include "ca79ng/ramcanary.h"

#include <string.h>

static void put16(uint8_t *p, uint32_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }
static void put32(uint8_t *p, uint32_t v) { put16(p, v); put16(p + 2, v >> 16); }

static uint32_t words_of(const rcn_region_t *g) { return (g->hi - g->lo) / 4u; }

void rcn_init(rcn_t *c, const rcn_region_t *tab, uint8_t n)
{
    memset(c, 0, sizeof *c);
    c->tab = tab;
    c->n = n > RCN_REGIONS_MAX ? (uint8_t)RCN_REGIONS_MAX : n;
}

uint32_t rcn_words(const rcn_t *c, uint16_t mask)
{
    uint32_t w = 0;
    for (unsigned i = 0; i < c->n; i++)
        if (mask & (1u << i))
            w += words_of(&c->tab[i]);
    return w;
}

int rcn_arm(rcn_t *c, uint16_t mask)
{
    if (!mask || (mask >> c->n))
        return 1;
    c->armed = mask;
    c->filled = c->dirty = 0;
    c->phase = RCN_PH_FILL;
    c->vi = 0;
    c->passes = c->filled_words = c->verified_words = 0;
    c->arms++;
    for (unsigned i = 0; i < RCN_REGIONS_MAX; i++) {
        memset(&c->r[i], 0, sizeof c->r[i]);
        if (mask & (1u << i))
            c->r[i].state = RCN_FILL;
    }
    return 0;
}

void rcn_disarm(rcn_t *c)
{
    c->armed = 0;
    c->phase = RCN_PH_OFF;
    for (unsigned i = 0; i < RCN_REGIONS_MAX; i++)
        c->r[i].state = RCN_OFF;
}

/* the hot loops are their own functions: few live values, so the compiler keeps them in registers (Thumb-1) */
static __attribute__((noinline)) void rcn_fill_words(volatile uint32_t *p, volatile uint32_t *end, uint32_t a)
{
    const uint32_t x = RCN_XOR;
    while (p != end)
        *p++ = a ^ x, a += 4u;
}

static uint32_t fill(rcn_t *c, uint32_t budget)
{
    uint32_t done = 0;
    while (done < budget && c->filled != c->armed) {
        unsigned i = 0;
        while (!((c->armed & ~c->filled) & (1u << i)))
            i++;
        const rcn_region_t *g = &c->tab[i];
        rcn_reg_t *r = &c->r[i];
        uint32_t words = words_of(g), n = words - r->cur;
        if (n > budget - done)
            n = budget - done;
        volatile uint32_t *p = (volatile uint32_t *)(g->base + 4u * r->cur);
        rcn_fill_words(p, p + n, g->lo + 4u * r->cur);
        r->cur += n;
        done += n;
        if (r->cur >= words) {
            r->cur = 0;
            r->state = RCN_VERIFY;
            c->filled |= (uint16_t)(1u << i);
        }
    }
    c->filled_words += done;
    return done;
}

static unsigned next_armed(const rcn_t *c, unsigned i)
{
    for (unsigned k = 1; k <= c->n; k++) {
        unsigned j = (i + k) % c->n;
        if (c->armed & (1u << j))
            return j;
    }
    return i;
}

/* the hot loop: the first word in [p, end) that does not hold the pattern (end = none).  v ^ a against the constant
 * keeps it to a load, an eor, a compare and the pointer / address steps per word. */
static __attribute__((noinline)) const volatile uint32_t *rcn_scan(const volatile uint32_t *p, const volatile uint32_t *end, uint32_t a)
{
    const uint32_t x = RCN_XOR;
    while (p != end && (*p ^ a) == x)
        p++, a += 4u;
    return p;
}

static void mismatch(rcn_t *c, unsigned i, uint32_t a, uint32_t v)
{
    rcn_reg_t *r = &c->r[i];
    r->cnt++;
    if (!r->first_sweep) {
        r->first_addr = a, r->first_val = v, r->first_sweep = r->sweeps + 1u;
        r->min_addr = r->max_addr = a;
        c->dirty |= (uint16_t)(1u << i);
    } else {
        if (a < r->min_addr)
            r->min_addr = a;
        if (a > r->max_addr)
            r->max_addr = a;
    }
}

static uint32_t verify(rcn_t *c, uint32_t budget)
{
    uint32_t done = 0;
    if (!(c->armed & (1u << c->vi)))
        c->vi = (uint8_t)next_armed(c, c->vi);
    while (done < budget) {
        unsigned i = c->vi;
        const rcn_region_t *g = &c->tab[i];
        rcn_reg_t *r = &c->r[i];
        uint32_t words = words_of(g), n = words - r->cur;
        if (n > budget - done)
            n = budget - done;
        const volatile uint32_t *p = (const volatile uint32_t *)(g->base + 4u * r->cur), *end = p + n;
        uint32_t a = g->lo + 4u * r->cur;
        for (;;) {
            const volatile uint32_t *q = rcn_scan(p, end, a);
            if (q == end)
                break;
            a += (uint32_t)((const volatile uint8_t *)q - (const volatile uint8_t *)p);
            mismatch(c, i, a, *q);
            p = q + 1, a += 4u;
        }
        r->cur += n;
        done += n;
        if (r->cur >= words) {
            r->cur = 0;
            r->sweeps++;
            r->changed_last = r->cnt;
            if (r->cnt > r->changed_max)
                r->changed_max = r->cnt;
            r->cnt = 0;
            c->vi = (uint8_t)next_armed(c, i);
        }
    }
    c->verified_words += done;
    return done;
}

uint32_t rcn_step(rcn_t *c, uint32_t budget)
{
    uint32_t done;
    if (!c->armed || !budget)
        return 0;
    c->passes++;
    done = fill(c, budget);
    if (c->filled != c->armed)
        return done;
    c->phase = RCN_PH_VERIFY;
    return done + verify(c, budget - done);
}

void rcn_encode_status(const rcn_t *c, uint32_t budget, uint32_t last_us, uint32_t max_us, uint8_t raw[RCN_STATUS])
{
    memset(raw, 0, RCN_STATUS);
    raw[0] = 1;
    raw[1] = c->n;
    raw[2] = c->phase;
    put16(raw + 4, c->armed);
    put16(raw + 6, c->filled);
    put16(raw + 8, c->dirty);
    put16(raw + 10, budget);
    put32(raw + 12, c->passes);
    put32(raw + 16, c->filled_words);
    put32(raw + 20, c->verified_words);
    put32(raw + 24, last_us);
    put32(raw + 28, max_us);
}

void rcn_encode_region(const rcn_t *c, unsigned i, uint8_t raw[RCN_REGION])
{
    memset(raw, 0, RCN_REGION);
    if (i >= c->n)
        return;
    const rcn_reg_t *r = &c->r[i];
    raw[0] = r->state;
    raw[1] = (c->armed >> i) & 1u;
    put32(raw + 4, c->tab[i].lo);
    put32(raw + 8, c->tab[i].hi);
    put32(raw + 12, r->cur);
    put32(raw + 16, r->sweeps);
    put32(raw + 20, r->changed_last);
    put32(raw + 24, r->changed_max);
    put32(raw + 28, r->min_addr);
    put32(raw + 32, r->max_addr);
    put32(raw + 36, r->first_addr);
    put32(raw + 40, r->first_val);
    put32(raw + 44, r->first_sweep);
}
