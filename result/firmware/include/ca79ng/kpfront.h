/* OpenGrand - Phase 3-1 SysEx front: the core judges the Kawai parameter set (fn 0x10) of the stock
 * firmware's controller link, the stock firmware only applies it.
 *
 * Cut line:
 *   - the core owns  F0 40 <dev> 10 08 02 v1 v2 part [data..] F7  (n >= 10) except the VT-edit
 *     prefixes (d0 0x5A, d0 0x50 + d1 0C/0E/10/17), which the stock judges before the function code;
 *   - for an owned frame it decides, in the stock order (FUN_4014A2C8 0x4014A304..0x4014AAA0):
 *     device byte (== the receive device number or 0x7F) -> bound port (binding == port + 1) ->
 *     ACK F0 40 7F 07 04 1E v1 v2 part F7 unconditionally (v1 0x70: v1 v2 only, FUN_4014BB54) to the
 *     bound port, to ports 0..2 first when the panel (port 3) is bound and port-3 forwarding is on
 *     (FUN_4014AD04) -> mirror into the core's parameter store -> apply through the stock
 *     FUN_4014EBFC(port + 1, 0x10, msg + 6, n) (lookup, length, part, staging gate, apply switch);
 *   - everything else is passed to the stock handler unchanged.
 * Not bound / wrong device byte: silent, like the stock.  The mirror has no authority until
 * Phase 3-4 (the stock apply switch stays the owner of the settings); unknown keys are ACKed
 * and counted, staged sets (fn 0x09 .. 0x03) are mirrored and counted.
 * Pure logic: the platform binds kpf_io_t (ports/cave/sysex_cave.c on the piano, tests on the host). */
#ifndef CA79NG_KPFRONT_H
#define CA79NG_KPFRONT_H

#include <stdbool.h>
#include <stdint.h>

#include "ca79ng/kproto.h"

typedef struct {
    void *ctx;
    uint8_t (*binding)(void *ctx);   /* bound controller: stock port + 1, 0 = none (stock *0x413CA3DE)  */
    uint32_t (*device)(void *ctx);   /* receive device number (stock *0x413B5404)                       */
    bool (*forward)(void *ctx);      /* port 3 -> 0..2 forwarding on (stock *0x413B5408 != 0)            */
    bool (*staging)(void *ctx);      /* fn 0x09 staging on (stock *0x419156A9 != 0)                      */
    void (*tx)(void *ctx, uint8_t stock_port, const uint8_t *msg, uint16_t len);
    /* the stock apply path FUN_4014EBFC(ctx_id, 0x10, msg + 6, n): ctx_id = stock port + 1 */
    void (*apply)(void *ctx, uint8_t ctx_id, const uint8_t *body, uint16_t n);
} kpf_io_t;

typedef enum { KPF_PASS = 0, KPF_SILENT = 1, KPF_OWN = 2 } kpf_verdict_t;

typedef struct {
    uint32_t seen;      /* frames offered to the front                                       */
    uint32_t passed;    /* left to the stock handler (not fn 0x10 set, VT edit, short, off)    */
    uint32_t owned;     /* ACKed and applied                                                  */
    uint32_t silent;    /* owned and dropped: not bound / other device number                 */
    uint32_t mirrored;  /* values written into the mirror store                               */
    uint32_t unknown;   /* values the core does not know (or invalid): ACKed, not mirrored     */
    uint32_t staged;    /* values mirrored while the stock staging flag was on                 */
    uint32_t packed;    /* owned frames in the packed form 10 5F 00 ..                         */
    uint32_t acks;      /* ACK frames sent (one per destination port)                          */
} kpf_stats_t;

typedef struct {
    kpf_io_t io;
    param_store_t *mirror;   /* NULL = no mirror */
    bool enabled;            /* false = pass everything to the stock handler */
    kpf_stats_t st;
    uint8_t last_key[3];     /* v1 v2 part of the last owned frame */
} kpf_t;

#define KPF_ACK_MAX 10u

void kpf_init(kpf_t *f, const kpf_io_t *io, param_store_t *mirror);

/* What the front would do with this frame from stock port 0..3 (reads io.binding / io.device only). */
kpf_verdict_t kpf_judge(const kpf_t *f, uint8_t stock_port, const uint8_t *m, uint32_t n);

/* The ACK frame the stock FUN_4014BE98 builds for this set; returns its length (9 or 10). */
uint16_t kpf_ack_frame(const uint8_t *m, uint8_t out[KPF_ACK_MAX]);

/* Handle one received Kawai frame: true = the core owned it (the stock handler must not run). */
bool kpf_handle(kpf_t *f, uint8_t stock_port, const uint8_t *m, uint32_t n);

#endif
