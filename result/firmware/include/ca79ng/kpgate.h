/* OpenGrand - Phase 4 (A) intake gate: the core decides which of the remaining Kawai frames the stock handler
 * FUN_4014A2C8 runs, the stock only executes.
 *
 * Called from the existing SysEx front-end hook 0x40147E94 (no new hook site), after the session core (kpsess) and
 * before the commands / the front: the frames those already judge (CONNECT / DISCONNECT, requests fn 0x02, ACK / NAK /
 * Wait / Cancel fn 0x04-0x07, the file lease of fn 0x24, commands fn 0x31 / 0x32, sets fn 0x10) pass through
 * unchanged.  For the rest the gate re-states the stock's own tests (REA, FUN_4014A2C8 0x4014A2C8..0x4014AC94) as the
 * core's decision:
 *   - fn 0x70 (flash tools FUN_4014A1B4) and fn 0x60 (identity FUN_40149D30, answered on every port): the stock, as is
 *   - device byte not ours and not 0x7F, m[4] / m[5] not 08 02 (0x71 02 from port 0 excepted: the stock's own path),
 *     fn 0x00 / 0x01 / 0x08 / 0x0A-0x0F / 0x11-0x23 / 0x25-0x30 / 0x33-0x7D / 0x7F: the stock does nothing - dropped
 *   - fn 0x03 (n 10) / 0x09, 7E 02 03 (test mode, model class *0x419156A7 != 0), 7E 04 / 05 (info, m[7] 0x40-0x42):
 *     the stock binding only, as the stock; 7E 00 / 03 / other subs: nothing - dropped
 *   - fn 0x24 of the stock binding: the request intake.  List 24 11: mode 5 -> the ACK only, no USB memory -> ACK +
 *     error 7E 03 00 00 21, a list running -> ACK + Busy; a job 24 04-0B / 12 while one is pending -> ACK + Busy - the
 *     stock's bytes, sent by the core; else the stock queues it.  Data / ACK frames of a running job (01 02 03 1F 7B
 *     7D 7E 7F) go to the stock; unknown d0: dropped.
 * Deliberate difference (see project notes): 7E 04 00 / 04 01 / 05 00 / 05 01 / 05 02 from a bound controller that is
 * not the stock binding are answered by the core to that controller - the stock's frame (FUN_4014AEE0 bytes) built from
 * the stock's own data (io.info).  The stock drops them (gate FUN_40151BCC).  7E 04 02 (a 4 MB flash checksum in task
 * 19) stays the binding's.
 * Pure logic: the platform binds kpg_io_t (ports/cave/sysex_cave.c in ota_main; tests on the host). */
#ifndef CA79NG_KPGATE_H
#define CA79NG_KPGATE_H

#include <stdbool.h>
#include <stdint.h>

#define KPG_PORTS 4u
#define KPG_HOME 3u
#define KPG_INFO_MAX 0x1Cu   /* FUN_4014AEE0 copies at most 0x1C data bytes (up to the first 0xFF) */
#define KPG_FRAME_MAX (KPG_INFO_MAX + 9u)

/* io.file(which) */
#define KPG_FILE_JOB 0u      /* 0x413CA3EA pending job d0 (FUN_4014F8CC / F870 / F7EC: != 0 -> Busy) */
#define KPG_FILE_LIST 1u     /* 0x413CA3EC list running (FUN_4014F018: != 0 -> Busy)                  */

typedef struct {
    void *ctx;
    uint8_t (*binding)(void *ctx);   /* stock binding = stock port + 1, 0 none (*0x413CA3DE)   */
    uint32_t (*device)(void *ctx);   /* receive device number (FUN_40147D60, *0x413B5404)       */
    bool (*forward)(void *ctx);      /* port 3 -> 0..2 forwarding (*0x413B5408 != 0)          */
    bool (*bound)(void *ctx, uint8_t port);   /* the session core's table                      */
    void (*tx)(void *ctx, uint8_t port, const uint8_t *m, uint16_t n);
    uint8_t (*file)(void *ctx, uint8_t which);   /* KPG_FILE_*                                  */
    bool (*media)(void *ctx);        /* USB memory mounted (FUN_4012795C != 0)                  */
    uint8_t (*mode)(void *ctx);      /* FUN_400B143C (*0x4139F4C6): 5 = no list               */
    bool (*test_ok)(void *ctx);      /* FUN_4015BA64 (*0x419156A7) != 0: test mode allowed     */
    /* the data bytes the stock's info answer for 7E sub sel carries, as the stock fills its local buffer (0xFF ends
     * them, FUN_4014AEE0): > 0 bytes written to out, <= 0 none.  NULL = no core info answers. */
    int (*info)(void *ctx, uint8_t sub, uint8_t sel, uint8_t out[KPG_INFO_MAX + 1u]);
} kpg_io_t;

typedef enum {
    KPG_PASS = 0,   /* the stock handler runs it */
    KPG_DROP = 1,   /* the core refused it: nothing sent, the stock handler not called */
    KPG_DONE = 2,   /* the core answered it itself: the stock handler not called */
} kpg_verdict_t;

typedef struct {
    uint32_t passed;     /* frames the gate judged and gave to the stock                     */
    uint32_t dropped;    /* frames the gate refused (the stock would have done nothing)       */
    uint16_t info_own;   /* info answers the core sent to a non-binding bound controller      */
    uint16_t file_busy;  /* the binding's file requests answered Busy by the core             */
    uint16_t file_err;   /* the binding's list answered "no USB memory" (7E 03 00 00 21)      */
    uint16_t file_ack;   /* the binding's list answered ACK only (mode 5)                     */
    uint16_t test_drop;  /* 7E 02 refused (not the binding / not 03 / model class 0)          */
    uint16_t other_drop; /* binding-only fn 03 / 09 / 7E 04 / 05 from another port            */
} kpg_stats_t;

typedef struct {
    kpg_io_t io;
    bool enabled;
    kpg_stats_t st;
} kpg_t;

void kpg_init(kpg_t *g, const kpg_io_t *io);
void kpg_enable(kpg_t *g, bool on);

/* One received Kawai frame (n = wire length with F7) from stock port 0..3 that the session core passed. */
kpg_verdict_t kpg_rx(kpg_t *g, uint8_t port, const uint8_t *m, uint32_t n);

/* FUN_4014AEE0(0x7F, sub, sel, data): F0 40 7F 7E 04 1E sub sel <data up to 0xFF, at most 0x1C> F7 -> length */
uint16_t kpg_sys_frame(uint8_t sub, uint8_t sel, const uint8_t *data, uint8_t out[KPG_FRAME_MAX]);

#endif
