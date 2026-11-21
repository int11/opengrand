/* OpenGrand cave port - Phase 3-1: the core judges the Kawai parameter set (fn 0x10) on the stock
 * CA79 V2.19 firmware (slot payload ota_sysex, feat1 0x20; with the power port of ota_power).
 *
 * The runtime hook at 0x40147E94 (the only call of the stock Kawai handler FUN_4014A2C8, task 19)
 * hands every Kawai frame to sxc_kawai().  The core front (src/kproto/kpfront.c) owns the fn 0x10 set
 * of the bound controller: it decides silence / ACK bytes / ACK ports itself, mirrors the value into
 * its parameter store, and delegates the application to the stock FUN_4014EBFC(port + 1, ...).
 * Everything else returns 0 and the stub tail-calls the stock handler with its registers untouched.
 * While the front owns a frame it holds the resident's inflight flag (it may block in the TX writer
 * or inside the stock apply path - contract (b) of the runtime-hook service).
 *
 * 0x7D op 0x1B: front diagnostics, the enable switch and (sub 02) the stock session state with per-port
 * frame / CONNECT counts (REMOTE_PROTOCOL.md section 10.5). */
#ifndef CA79NG_SYSEX_CAVE_H
#define CA79NG_SYSEX_CAVE_H

#include <stdint.h>

#define SX_OP_FRONT 0x1B
#define SX_DIAG_RAW 48
#define SX_SITE_KAWAI 0x40147E94u

/* rc of op 0x1B (the power ops' numbering) */
enum { SX_OK = 0, SX_BAD_VALUE = 1, SX_NOT_BOUND = 2 };

/* task 24 (engine loop): initialise the mirror store and the front on the first call */
void sxc_tick(void);
/* task 19, hook body at 0x40147E94 (r0 port, r1 frame, r2 length): 1 = handled, do not run the stock
 * handler; 0 = tail-call FUN_4014A2C8 */
int sxc_kawai(uint32_t port, const uint8_t *buf, uint32_t len);
/* task 19 (cave ng_sysex): 0x7D op 0x1B, answered at once on the same port */
void sxc_sysex(uint8_t stock_port, const uint8_t *buf, uint32_t len);
/* attach(): whether the 0x40147E94 hook is in place, and the resident's inflight flag */
void sxc_attached(int hooked, volatile unsigned char *inflight);

/* host tests: forget everything (re-initialised on the next sxc_tick) */
void sxc_restart(void);
/* the raw op 0x1B record */
void sxc_diag(uint8_t raw[SX_DIAG_RAW]);
/* the raw op 0x1B 02 record: stock session globals (read only) and the per-port frame / CONNECT counts */
void sxc_session(uint8_t raw[SX_DIAG_RAW]);
/* is the controller on stock port `port` bound?  1 / 0 = the session core's binding table + the stock binding (it runs and is on);
 * -1 = no session core here (not built, not ready, or switched off by 1B 01 0): the caller uses the stock binding.
 * A byte read - any task (bt_cave.c gates the 1D BT-audio changes with it) */
int sxc_ctrl_bound(uint8_t port);

#if CA79NG_SESSION
/* ota_session (Phase 3-2 stage 1): the session core
 * (src/kproto/kpsess.c) sees every Kawai frame first in sxc_kawai(); the MIDI pump tail 0x401539AC
 * (task 19, every 2 ms) runs its sender timer through ssc_pump().  op 0x1B 03: its record. */
#define SS_SITE_MIDI 0x401539ACu
void ssc_pump(void);
void ssc_attached(int hooked);
void sxc_core(uint8_t raw[SX_DIAG_RAW], uint8_t req_port);
/* stage 2: sets the front applies and the stock's
 * notifications on its binding's TX ring fan out to the other bound controllers.  op 0x1B 04: record. */
void sxc_fan(uint8_t raw[SX_DIAG_RAW], uint8_t req_port);
/* op 0x1B 05 (Phase 3-4 S1 + Phase 3-5 stage 1): settings-store fill / compare walk and the metronome module */
void sxc_state(uint8_t raw[SX_DIAG_RAW]);
/* op 0x1B 05 02 / 03 (Phase 3-4 S2): the periodic compare and its explained / unexplained
 * differences, the ring of the last unexplained ones (three from the k-th newest) */
void sxc_state_s2(uint8_t raw[SX_DIAG_RAW], uint32_t now_ms);
void sxc_state_ring(uint8_t raw[SX_DIAG_RAW], uint8_t k);
/* op 0x1B 05 04 / 05 (Phase 3-4 S3, always on from fold step 3; a set byte gets rc 1): the read owner's
 * record (layout 8) - flags, what the store answered, the trust generation. */
void sxc_state_s3(uint8_t raw[SX_DIAG_RAW]);
/* op 0x1B 05 06 / 07 (Phase 3-4 S4, always on from fold step 3; a set byte gets rc 1): the write owner's
 * record (layout 9) - flags, what the gate / read-back / trust did. */
void sxc_state_s4(uint8_t raw[SX_DIAG_RAW]);
#endif

/* ota_main feat3 0x10 (Phase 4): the MIDI channel messages that change
 * settings and the delayed parameter apply, judged by the core at the ota_loader_v3 sites below (task 19).  The stubs
 * live in src/power_hooks.c; they read mcc_on (set by the front's init when attached, never cleared - fold step 3). */
#define MC_SITE_PC 0x40148C4Cu
#define MC_SITE_CC 0x40148C60u
#define MC_SITE_DEFER 0x40153998u
extern volatile uint8_t mcc_on;
uint32_t mcc_pc_judge(uint32_t port, uint32_t st, uint32_t prog);
void mcc_pc_run(uint32_t port, uint32_t st, uint32_t prog);
uint32_t mcc_cc_judge(uint32_t port, uint32_t st, uint32_t cc, uint32_t val);
void mcc_cc_run(uint32_t port, uint32_t st, uint32_t cc, uint32_t val);
void mcc_defer(int32_t elapsed);
/* attach(): the sites in place (bit 0 PC, 1 CC, 2 delayed apply) */
void mcc_attached(uint8_t mask);
/* op 0x1B 07: the record (layout 9) */
void sxc_chan(uint8_t raw[SX_DIAG_RAW]);

#endif
