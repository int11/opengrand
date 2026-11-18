/* OpenGrand cave port - the few C runtime routines the core needs inside the cave.
 *
 * The cave links no C library (-ffreestanding, -fno-builtin, no libgcc): the core only
 * needs these five string routines and unsigned 32-bit division, which ARM926EJ-S lacks
 * in hardware.  Kept deliberately simple - byte loops and shift/subtract division.
 *
 * On the host (CAVE_RT_HOST) every name gets a cave_rt_ prefix so the unit tests can
 * compare them against the host C library without clashing with it. */
#include <stddef.h>
#include <stdint.h>

#ifdef CAVE_RT_HOST
#define RT(name) cave_rt_##name
#else
#define RT(name) name
#endif

void *RT(memcpy)(void *dst, const void *src, size_t n);
void *RT(memset)(void *dst, int c, size_t n);
int RT(memcmp)(const void *a, const void *b, size_t n);
size_t RT(strlen)(const char *s);
char *RT(strncpy)(char *dst, const char *src, size_t n);
unsigned RT(__aeabi_uidiv)(unsigned num, unsigned den);
unsigned long long RT(__aeabi_uidivmod)(unsigned num, unsigned den);

void *RT(memcpy)(void *dst, const void *src, size_t n)
{
    uint8_t *d = dst;
    const uint8_t *s = src;
    while (n--)
        *d++ = *s++;
    return dst;
}

void *RT(memset)(void *dst, int c, size_t n)
{
    uint8_t *d = dst;
    while (n--)
        *d++ = (uint8_t)c;
    return dst;
}

int RT(memcmp)(const void *a, const void *b, size_t n)
{
    const uint8_t *p = a, *q = b;
    for (; n--; p++, q++)
        if (*p != *q)
            return *p < *q ? -1 : 1;
    return 0;
}

size_t RT(strlen)(const char *s)
{
    size_t n = 0;
    while (s[n])
        n++;
    return n;
}

char *RT(strncpy)(char *dst, const char *src, size_t n)
{
    size_t i = 0;
    for (; i < n && src[i]; i++)
        dst[i] = src[i];
    for (; i < n; i++)
        dst[i] = 0;
    return dst;
}

/* quotient and remainder by shift/subtract; division by zero returns 0 / num */
static unsigned udiv(unsigned num, unsigned den, unsigned *rem)
{
    unsigned q = 0, r = 0;
    int bit;
    if (den == 0) {
        *rem = num;
        return 0;
    }
    for (bit = 31; bit >= 0; bit--) {
        unsigned carry = r >> 31;   /* r < den may be >= 2^31: keep the shifted-out bit */
        r = (r << 1) | ((num >> bit) & 1u);
        if (carry || r >= den) {
            r -= den;
            q |= 1u << bit;
        }
    }
    *rem = r;
    return q;
}

unsigned RT(__aeabi_uidiv)(unsigned num, unsigned den)
{
    unsigned r;
    return udiv(num, den, &r);
}

/* AEABI: quotient in r0, remainder in r1 = a 64-bit return value {r0, r1} */
unsigned long long RT(__aeabi_uidivmod)(unsigned num, unsigned den)
{
    unsigned r, q = udiv(num, den, &r);
    return (unsigned long long)r << 32 | q;
}
