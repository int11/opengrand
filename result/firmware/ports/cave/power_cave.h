/* OpenGrand cave port - power policy, partial standby and the BT audio APO rule on the stock
 * CA79 V2.19 firmware (slot payload ota_power, feat1 0x10).
 *
 * The core modules run unchanged: standby.c (state machine), power.c (BT audio model, APO
 * hold rule, policy codec) and kvstore.c (policy record).  This port binds them:
 *   - standby_ops_t -> stock routines (power_stock.h), task 24 only;
 *   - the stock APO counter stays the clock: the 20 ms kernel-timer hook (site 0x400AA874,
 *     before FUN_400ABFE8) holds it at 1 while not ON, keeps it from expiring while the BT stream
 *     hold holds (it still counts and activity still resets it), resets it when
 *     the stream ends before the max hold, and turns an expiry
 *     into a request for the engine loop (standby or the delayed real off) - the stock state
 *     byte is written (= 2) only for the real off, after the flash is idle;
 *   - two re-light paths blocked while silenced: the headphone poll (site 0x40143AB4) is
 *     skipped, the GP3[8] event is cleared before the executor (site 0x40143AB8);
 *   - the panel touch notice (Kawai SysEx 32 51 39 on stock port 3) is seen at the Kawai SysEx
 *     call (site 0x40147E94) and wakes; keys and the power switch wake from the engine loop;
 *   - BT module lines are read from the stock UART1 RX ring (read only, own read index);
 *   - the policy lives in a kvstore over NOR unused_3 (0x604C0000, two 128 KB sectors), written
 *     only through the guarded wrappers below (range = unused_3, chip owned under dis_dsp).
 * 0x7D ops 0x18 / 0x19 answer byte-for-byte like the host core (src/system/ngext.c), plus the
 * read-only diagnostics op 0x1A.  Contract: REMOTE_PROTOCOL.md section 10.4.
 *
 * TV wake (first in slot payload ota_power2, feat1 0x40):
 * standby keeps the BT audio module on unless the policy flag bit 2 says otherwise, and an A2DP
 * STREAMING START line wakes (cause 5); a UART1 RX 0x80 latch is cleared from the engine loop with
 * the stock ring reset FUN_40145C00; op 0x1A answers layout 2 (60 B with the remote-off tail).
 * Every build: the BT line reader follows a stock ring reset instead of re-reading old bytes (F4). */
#ifndef CA79NG_POWER_CAVE_H
#define CA79NG_POWER_CAVE_H

#include <stdbool.h>
#include <stdint.h>

#define PW_OP_POLICY 0x18
#define PW_OP_STANDBY 0x19
#define PW_OP_DIAG 0x1A
/* TV wake (ota_power2), the BT port's pwc_on / link state (ota_bt), remote off with the core's binding table and quiet
 * polls (ota_main feat2 0x20) are no longer build switches: every build of
 * this port (ota_main, the host tests) has them (field-tested,
 * then folded).  The old PWC_TVWAKE / PWC_BTSRC / PWC_REMOTE_OFF / PWC_QUIET-off
 * builds are the frozen blobs ota_power / ota_power2 / ota_power3, rebuilt from their own commits.
 *
 * Remote off: op 19 03, and every "bound controller" test of this port (18 01 / 18 02 / 19 01 / 19 03) follows the
 * session core's binding table (btc_ctrl_bound(sxc_ctrl_bound(port), stock binding, port)).
 * Quiet polls: polls and reads do not keep the piano on.
 * The stock byte parser resets the APO counter on every MIDI byte < 0xF8 (FUN_40148B24 -> FUN_400ACB84); the frames are
 * classified where they end (Kawai: the 0x40147E94 stub, 0x7D: the payload's ng_sysex) with power_frame_quiet(), and the
 * MIDI pump tail (0x401539AC, task 19, after the four port pumps) puts the counter back where it would be when that pump
 * pass carried only polls / reads (or SysEx bytes of a frame still open). */
/* 1 = the piano is ON (not standby / waking / switching off): the BT port may send module commands */
int pwc_on(void);
#ifndef PWC_STARTUP
#define PWC_STARTUP 0 /* 1 = linked with the startup-settings port (ota_main; the host tests build without it): suc_tick / suc_flush */
#endif
#define PW_DIAG_LAYOUT 2
#define PW_DIAG_RAW 60   /* layout 2 + 52 remote offs done, 53 refused (busy), 54..55 APO resets undone (polls only),
                          * 56..57 quiet frames, 58..59 activity frames (u16 LE, saturating) - readers of layout 2 ignore
                          * the tail */

/* hook sites (allow-list of ota_loader_v2) and their bit in the attach mask */
#define PW_SITE_KAWAI 0x40147E94u /* bl FUN_4014A2C8, task 19 */
#define PW_SITE_HP 0x40143AB4u    /* bl FUN_400ACFA0, task 24 */
#define PW_SITE_EXEC 0x40143AB8u  /* bl FUN_400ACD28, task 24 */
#define PW_SITE_APO 0x400AA874u   /* bl FUN_400ABFE8, 20 ms kernel timer */
enum { PW_HK_KAWAI = 0x01, PW_HK_HP = 0x02, PW_HK_EXEC = 0x04, PW_HK_APO = 0x08, PW_HK_ALL = 0x0F };

/* NOR range of the policy kvstore: unused_3, no stock reader / writer (REA) */
#define PW_KV_LO 0x604C0000u
#define PW_KV_HI 0x60500000u
#define PW_KV_SECTOR 0x20000u

/* result codes of 0x18 / 0x19 (ngext.c) + 5: standby needs the runtime hooks, 6: remote off refused now (19 03: the
 * reason byte follows, POWER_OFF_* of ca79ng/power.h - recording, file operation, flash job) */
enum { PW_OK = 0, PW_BAD_VALUE = 1, PW_NOT_BOUND = 2, PW_BAD_STATE = 3, PW_SAVE_FAILED = 4, PW_NO_HOOKS = 5, PW_BUSY = 6,
       PW_APO_OFF = 7 };
#define PW_SB_REMOTE_OFF 0x03
/* 19 04 <s_hi7> <s_lo7>: APO test shortcut - the stock counter jumps so that the menu limit minus
 * the counter = s seconds (10..600; 0 = back to the full menu time), RAM only.  rc 7 = the APO does not count (menu Off or
 * a special mode). */
#define PW_SB_APO_TEST 0x04

/* task 19 (cave ng_sysex): queue a 0x18 / 0x19 / 0x1A request; answered by the next pwc_tick */
void pwc_sysex(uint8_t stock_port, const uint8_t *buf, uint32_t len);
/* task 24 (engine loop, every ~8 ms): everything else */
void pwc_tick(void);
/* OTA apply-now: 0 = idle.  Not idle while a request is queued, and while the piano is not ON
 * (a standby is woken first, so the next cave never inherits a silenced piano). */
int pwc_quiesce(void);
/* attach(): which hooks the resident accepted (PW_HK_* mask) */
void pwc_hooks_attached(uint8_t mask);
/* The NOR write service (ota_loader_v4 stage-0 struct s0_svc nor_erase / nor_prog, handed to the main in attach():
 * struct ota_svc version >= 3).  rc: 0 done, 1 a stock writer owns the chip (nothing done), 2 failed, 3 refused by
 * the guard.  Not bound (an older loader): the kvstore is read-only - every write fails (nor_errors). */
typedef int (*pwc_nor_erase_fn)(unsigned int addr);
typedef int (*pwc_nor_prog_fn)(unsigned int addr, const void *src, unsigned int n);
void pwc_nor_bind(pwc_nor_erase_fn erase, pwc_nor_prog_fn prog);

/* hook bodies, called from the payload's stubs before the stock target */
void pwc_apo_timer(void);                                         /* kernel timer: never blocks  */
int pwc_hp_pass(void);                                            /* 0 = skip FUN_400ACFA0        */
void pwc_exec_pre(void);                                          /* before FUN_400ACD28          */
void pwc_kawai_peek(uint32_t port, const uint8_t *buf, uint32_t len); /* task 19, never blocks  */
/* task 19: a 0x7D frame reached the payload's ng_sysex (classified only) */
void pwc_frame_7d(const uint8_t *buf, uint32_t len);
/* task 19, MIDI pump tail 0x401539AC (after the four port pumps, before the session core's ssc_pump): undo the APO reset
 * of a pump pass that carried only polls / reads.   Loads / stores only. */
void pwc_midi_tail(void);

/* host tests: forget everything (the RW region is re-initialised on the next pwc_tick) */
void pwc_restart(void);
#ifdef PWC_HOST
/* host tests: the stock RAM reads of the recorder view (recview.h addresses) and the session core's binding answer
 * (sxc_ctrl_bound) - the test's stand-ins */
void pwc_host_read(uint32_t addr, void *dst, uint32_t n);
int pwc_host_core_bound(uint8_t port);
#endif
/* host tests / diagnostics: the raw op 0x1A record */
void pwc_diag(uint8_t raw[PW_DIAG_RAW]);

#endif
