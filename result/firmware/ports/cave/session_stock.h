/* OpenGrand cave port - the stock CA79 V2.19 routines and globals the Phase 3-2 session port binds to
 * (sysex_cave.c built with CA79NG_SESSION, slot payload ota_session).
 *
 * On the piano the addresses come from the linker (result/stockfw/cave/stock_session.ld).  Evidence:
 * from REA and a disassembly re-read (request task FUN_40152A34 / FUN_401526DC, CONNECT
 * 0x4014A6F8..0x4014A7FC, frame builder FUN_4014B0DC).
 *
 * Context: task 19 - the hook at 0x40147E94 (where the stock calls all of these from FUN_4014A2C8)
 * and the MIDI pump tail 0x401539AC (where the stock request task's work now runs, same task). */
#ifndef CA79NG_SESSION_STOCK_H
#define CA79NG_SESSION_STOCK_H

#include <stdint.h>

/* request answers (FUN_401526DC) */
const uint8_t *stk_desc(uint32_t i);                        /* FUN_4014C034: 10-byte descriptor i  */
int32_t stk_req_match(uint32_t i, uint32_t v1, uint32_t v2); /* FUN_4015205C: -1 = not this request */
uint32_t stk_part_x(uint32_t part);                         /* FUN_4014EAF4                        */
uint32_t stk_desc_has(uint32_t i, uint32_t x);              /* FUN_401520B4                        */
uint32_t stk_ctx_of(uint32_t binding);                      /* FUN_4015B470                        */
uint32_t stk_slot16(uint32_t j);                            /* FUN_4015B188                        */
uint32_t stk_slot5(uint32_t j);                             /* FUN_4015B00C                        */
int32_t stk_getter(uint32_t ctx, uint32_t x, uint32_t i, uint8_t *buf);   /* FUN_4015E960: 0 = ok */
/* FUN_4014B0DC(cc, {desc, key, buf}, out, desc[8]): builds one value frame, returns its length */
typedef struct {
    const uint8_t *desc;
    uint8_t key;
    uint8_t pad[3];
    const uint8_t *buf;
} stk_valsrc_t;
uint32_t stk_frame_build(uint32_t cc, const stk_valsrc_t *src, uint8_t *out, uint32_t n);
extern const uint8_t stk_key80[];   /* 0x400FD8CB: key byte per FUN_4015B00C slot (desc[3] 0x80)  */
extern const uint8_t stk_key81[];   /* 0x400FD8D0: (desc[3] 0x81, slot 0 -> 0x0F)                  */
extern const uint8_t stk_key82[];   /* 0x400FD8E0: (desc[3] 0x82)                                  */

/* CONNECT (0x4014A75C..0x4014A7FC) and the stock binding */
uint32_t stk_remote_get(uint32_t port);                     /* FUN_40151BB8 */
void stk_remote_set(uint32_t port, uint32_t v);             /* FUN_40151B90 */
uint32_t stk_conn_mode(void);                               /* FUN_400B1414 */
uint32_t stk_conn_sub(void);                                /* FUN_400B1428 */
uint32_t stk_conn_kind(void);                               /* FUN_400B143C */
void stk_conn_post_a(void);                                 /* FUN_400B0FBC (kind 1..3) */
void stk_conn_post_b(void);                                 /* FUN_400B13AC (kind 5)    */
void stk_ctx_copy(uint32_t port);                           /* FUN_4015DE98 */
void stk_ctx_clean(void);                                   /* FUN_4015C33C */
void stk_bind(uint32_t port);                               /* FUN_40151B5C */
/* the reliable sender's 16-failure release (0x4014BDA8..): unbind, then the three state resets */
void stk_unbind(void);                                      /* FUN_4014B55C */
void stk_reset_a(void);                                     /* FUN_4014B5A0 */
void stk_reset_b(void);                                     /* FUN_4014B5D0 */
void stk_reset_c(void);                                     /* FUN_4014B610 */

extern const volatile uint8_t stk_link_state;   /* 0x4139E7F8: 2 = port 2 (GP4[6]), 1 = port 1, 0 none */

/* file lease: the stock's fn 0x24 job state, read only.  stk_file = 0x413CA3E5:
 *   +0 upload data state (FUN_4014F96C 1 after TopOfData, FUN_4014FB94 2, FUN_4014FABC 3 after 24 7B, FUN_4014B610 0)
 *   +5 pending job d0 (0x413CA3EA: FUN_4014F8CC / F870 / F7EC set it, the executor FUN_4014F364 clears it)
 *   +6 name complete, executor due (0x413CA3EB: FUN_4014EED8 / F7EC set, FUN_4014F364 clears on entry)
 *   +7 list running (0x413CA3EC: FUN_4014F018 sets, the file task 0x40151EF0 clears after 24 1F)
 *   +8 write waiting for data (0x413CA3ED: executor job 5 sets, the write task / FUN_4014B5D0 clear) */
#define STK_FILE_DATA 0u
#define STK_FILE_JOB 5u
#define STK_FILE_READY 6u
#define STK_FILE_LIST 7u
#define STK_FILE_WRITE 8u
extern const volatile uint8_t stk_file[9];

/* Phase 4 A intake gate (sysex_cave.c): the stock's own data
 * behind FUN_4014A2C8's tests and info answers, read only */
extern const volatile uint8_t stk_gate_media;       /* 0x41359E45 (& 0x7F: USB memory mounted, FUN_4012795C)   */
extern const volatile uint8_t stk_gate_model;       /* 0x419156A7 model class (FUN_4015BA64: test mode allowed) */
extern const volatile uint8_t stk_gate_info_d;      /* 0x418CE0EC (7E 05 02, literal 0x4014ACD4)                 */
uint32_t stk_gate_model_idx(void);                  /* FUN_400B39B8: *0x413A77A0                                 */
const uint8_t *stk_gate_model_entry(uint32_t idx);  /* FUN_400B389C: 0x400A6050 + 0x13 idx                       */
const uint8_t *stk_gate_version(void);              /* FUN_400B3894: 0x400A6147 ("V2.19")                        */
uint32_t stk_gate_info_b(void);                     /* FUN_400B39F0: *0x413A77B0                                 */
uint32_t stk_gate_info_c(void);                     /* FUN_400B39D8: *0x413A77A4                                 */

/* stage 2: the SysEx TX ring of each stock port (2 KB, write index & 0x7FF), filled by the writers
 * FUN_4014492C / FUN_401445AC / FUN_40145284 / FUN_40144CD0 under a per-port semaphore - read only */
#define STK_TXRING_MASK 0x7FFu
extern const volatile uint8_t stk_txring0[], stk_txring1[], stk_txring2[], stk_txring3[];
extern const volatile uint16_t stk_txwr0, stk_txwr1, stk_txwr2, stk_txwr3;

#endif
