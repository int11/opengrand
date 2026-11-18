/* Minimal test harness: each test file defines TEST(name) functions and
 * registers them with RUN(name) inside a suite function. */
#ifndef CA79NG_TEST_H
#define CA79NG_TEST_H

#include <stdio.h>
#include <string.h>

extern int t_failures;
extern int t_checks;

#define CHECK(cond)                                                                   \
    do {                                                                              \
        t_checks++;                                                                   \
        if (!(cond)) {                                                                \
            t_failures++;                                                             \
            fprintf(stderr, "  FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);         \
        }                                                                             \
    } while (0)

#define CHECK_EQ(a, b)                                                                \
    do {                                                                              \
        long long _a = (long long)(a), _b = (long long)(b);                           \
        t_checks++;                                                                   \
        if (_a != _b) {                                                               \
            t_failures++;                                                             \
            fprintf(stderr, "  FAIL %s:%d: %s == %s (%lld vs %lld)\n", __FILE__,      \
                    __LINE__, #a, #b, _a, _b);                                        \
        }                                                                             \
    } while (0)

#define CHECK_MEM(a, b, n) CHECK(memcmp((a), (b), (n)) == 0)

#define TEST(name) static void name(void)
#define RUN(name)                                                                     \
    do {                                                                              \
        int _f = t_failures;                                                          \
        name();                                                                       \
        printf("  %-44s %s\n", #name, t_failures == _f ? "ok" : "FAILED");            \
    } while (0)

#endif
