/* cave_selftest - run the cave port's core self-test on the host and print the raw reply.
 *
 *   build/cave_selftest        -> "<fixed> <frame>"  (hex, one line)
 *
 * <fixed> is the first CORE_SELFTEST_FIXED raw bytes, which do not depend on time or on
 * the platform; result/stockfw/tools/roundtrip_ota.py (check 4) compares them with what the
 * same code answers when it runs as ARM code in a slot payload (Unicorn), so the
 * host build and the piano build are checked against each other. */
#include <stdio.h>

#include "../ports/cave/core_cave.h"
#include "../ports/cave/stock_host.h"

int main(void)
{
    uint8_t raw[CORE_SELFTEST_RAW], frame[CORE_SELFTEST_FRAME];
    stock_host_reset();
    core_tick();
    core_selftest(raw);
    uint16_t n = core_selftest_frame(raw, frame);
    for (int i = 0; i < CORE_SELFTEST_FIXED; i++)
        printf("%02x", raw[i]);
    printf(" ");
    for (int i = 0; i < n; i++)
        printf("%02x", frame[i]);
    printf("\n");
    return (raw[2] | raw[3] << 8) == (1u << CORE_CHK_COUNT) - 1 ? 0 : 1;
}
