/* OpenGrand cave port - the core instance that lives in the stock image.
 *
 * Dormant mode (image core_probe): the core is initialised and ticked from the engine-loop tail (task 24), but the stock
 * firmware keeps owning SysEx, parameters and power.  Its only output is the reply of the
 * self-test op (0x7D op 0x0A), which proves on the piano that the core links, runs and
 * reaches the stock routines it is bound to. */
#ifndef CA79NG_CORE_CAVE_H
#define CA79NG_CORE_CAVE_H

#include <stdbool.h>
#include <stdint.h>

#define CORE_OP_SELFTEST 0x0A       /* 0x7D request op; reply op 0x4A */
#define CORE_SELFTEST_RAW 32        /* raw reply bytes before 7-in-8 packing */
#define CORE_SELFTEST_FIXED 19      /* leading raw bytes that are the same on every run and
                                     * on every build of this code (host == piano)        */
#define CORE_SELFTEST_FRAME (7 + CORE_SELFTEST_RAW + (CORE_SELFTEST_RAW + 6) / 7 + 1)

/* self-test checks, bit i of the pass mask */
enum {
    CORE_CHK_GUARDS = 0,    /* R1 RW region guard words intact                          */
    CORE_CHK_CONNECT,       /* 7E 01 01 -> 7E 02 00 40, session callback, connected      */
    CORE_CHK_PARAM_SET,     /* fn 10 AutoPowerOff = 2 -> ACK, param callback (dormant)   */
    CORE_CHK_STORE,         /* the store holds 2 and its CRC moved                       */
    CORE_CHK_REQUEST,       /* fn 02 51 03 -> fn 10 value; after its ACK, EOD 03 51 03 7F */
    CORE_CHK_VERSION,       /* 7E 04 41 -> 7E 04 01 "1.0.0"                              */
    CORE_CHK_NACK,          /* unknown (v1, v2) -> ACK as stock, not applied, rx_errors++ */
    CORE_CHK_DISCONNECT,    /* 7E 01 00 -> 7E 01 01 to the panel, session cb, unbound    */
    CORE_CHK_PACK87,        /* kp_pack87 / kp_unpack78 round trip of 0..255             */
    CORE_CHK_COUNT
};

/* Engine-loop tail (task 24): initialise on the first call, then account time and serve
 * a pending self-test request.  Returns quickly when nothing is pending. */
void core_tick(void);

/* SysEx hook (task 19): remember a self-test request from stock port 0..3.  The test runs
 * on the next core_tick, so all core state is touched by one task only. */
void core_request(uint8_t stock_port);

/* Run the self-test script now (resets the core instance first) and fill raw[32]:
 *   0  layout version (1)       1  check count       2..3   pass mask u16
 *   4..7   CRC-32 of the default store             8..11  CRC-32 of the store after
 *   12..15 CRC-32 of every frame kproto emitted    16..17 bytes emitted u16
 *   18 frames emitted           19 flags: bit0 initialised, bit1 guards intact,
 *                                         bit2 APO timer due (dormant: never acted on)
 *   20..23 core ticks u32       24..27 core milliseconds (hal_time_us) u32
 *   28..31 hal_time_us() now
 * Bytes 0..18 (CORE_SELFTEST_FIXED) do not depend on time or on the platform. */
void core_selftest(uint8_t raw[CORE_SELFTEST_RAW]);

/* F0 7D 43 41 37 39 4A <pack7(raw)> F7; returns the frame length. */
uint16_t core_selftest_frame(const uint8_t raw[CORE_SELFTEST_RAW], uint8_t *out);

bool core_ready(void);

/* OTA "apply now" (ota_loader_v2): true when no self-test request is pending; core_restart()
 * forgets everything in the cave .data so the next core_tick re-initialises the RW region. */
bool core_idle(void);
void core_restart(void);

#endif
