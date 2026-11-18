/* OpenGrand cave port - startup settings owned by the core (slot payload ota_main, Identify feat2 0x08).
 *
 * The core module src/kproto/kpstart.c (mode, snapshot, kvstore records) bound to the stock firmware:
 *   task 24 (engine loop, through power_cave.c - the owner of the kvstore over NOR unused_3 and its guarded
 *           writers): suc_tick() loads mode + snapshot once the store is probed, writes the snapshots / mode the
 *           task-19 side hands it, and suc_flush() / suc_busy() let the power port save before standby / APO off;
 *   task 19 (the MIDI pump tail 0x401539AC, through sysex_cave.c): suc_pump() restores once at boot (after the
 *           Phase 3-4 S1 fill) by core-originated sets (sxc_core_set: the stock apply FUN_4014EBFC with ctx 0 + the
 *           stage-2 fan-out to every bound controller), then takes snapshots: P after 3 s quiet (>= 10 s apart) in
 *           mode power-off, C when asked; each one after a compare walk so the store equals the stock.
 * 0x7D op 0x1C (REMOTE_PROTOCOL.md section 10.8): 1C [00] status (any port), 1C 01 m set mode, 1C 02 save the current
 * settings as startup (C, mode current), 1C 03 clear (stock startup) - the bound controller only.
 * Since the modes govern every setting: each snapshot also carries the NG part (the BT port's kept policy +
 * the power policy, written by task 24 from the ports' RAM) and AutoPowerOff / LockMode; at a power-on (not a live apply)
 * the ports take suc_ng(); mode stock chosen by this kind of build restores the factory values (NG defaults + the
 * block A settings through the same core-originated sets). */
#ifndef CA79NG_STARTUP_CAVE_H
#define CA79NG_STARTUP_CAVE_H

#include <stdint.h>

#include "ca79ng/kproto.h"
#include "ca79ng/kpstate.h"
#include "ca79ng/storage.h"

#define SU_OP 0x1C
#define SU_RAW 40   /* 36 before the NG part (bytes 36..39) */

/* rc of op 0x1C */
enum { SU_OK = 0, SU_BAD_VALUE = 1, SU_NOT_BOUND = 2, SU_BUSY = 3, SU_SAVE_FAILED = 4 };

/* task 19, cave ng_sysex: op 0x1C (status answered at once, the rest by the pump tail / engine loop) */
void suc_sysex(uint8_t stock_port, const uint8_t *buf, uint32_t len);
/* task 19, pump tail (sysex_cave.c ssc_pump): `changes` = a counter that moves with every value change the
 * controllers / the stock make (front sets, commands, stock notifications); now = 32-bit ms */
void suc_pump(param_store_t *store, kst_t *st, uint32_t changes, uint32_t now_ms);
/* task 19: a fn 0x31 / 0x32 command the core ran (sx_cmd): 32 51 34 07 (stock save) / 32 51 36 07|04 (factory) */
void suc_cmd(const uint8_t *m, uint32_t n);

/* task 24 (power_cave.c pwc_tick): load / write; allow_compact = the piano is not ON (standby) */
void suc_tick(int allow_compact);
/* task 24: standby / APO off ahead - save P now (mode power-off, something changed); suc_busy() until written */
void suc_flush(void);
int suc_busy(void);
/* OTA apply-now: 0 = idle */
int suc_quiesce(void);
/* 1 while the boot restore, its resync walks or a snapshot capture run, or an op 0x1C is queued (task 19) */
int suc_settling(void);

/* the raw op 0x1C status record */
void suc_status(uint8_t raw[SU_RAW]);

/* ---- the NG part (the modes govern our own settings too) ---- */
/* what our settings start from at this power-on (task 24, the BT and power ports, once each): SUC_NG_WAIT until the
 * startup settings are loaded, then KSU_NG_KEEP (the live values on NOR stay) / KSU_NG_DEFAULTS (the factory defaults)
 * / KSU_NG_SAVED (*bt / *pw: the snapshot's parts, a part of length 0 = not carried: keep) */
#define SUC_NG_WAIT (-1)
int suc_ng(const uint8_t **bt, uint8_t *btn, const uint8_t **pw, uint8_t *pwn);
/* a port took suc_ng's answer and changed its values (bit: SUC_NG_BT / SUC_NG_PW) - status byte 38 */
enum { SUC_NG_BT = 0x01, SUC_NG_PW = 0x02 };
void suc_ng_applied(uint8_t bit);
/* a user change of an NG setting (task 24): starts the power-off snapshot's debounce like a stock change */
void suc_ng_changed(void);
/* the ports' NG parts now (task 24): bt_cave.c BTS_SAVED_RAW bytes, power_cave.c POWER_POLICY_RAW bytes */
void btc_ng_get(uint8_t *out);
void pwc_ng_get(uint8_t *out);

/* power_cave.c: the kvstore, mounted (mount = 1: a blank store is created first); NULL = not available.
 * pwc_kv_known(): 0 while the first probe has not answered (a stock writer owned the chip) */
kvstore_t *pwc_kv(int mount);
int pwc_kv_known(void);
int pwc_flash_idle(void);
/* sysex_cave.c: a set the core makes itself (ctx 0, fan-out to every bound controller) */
void sxc_core_set(const uint8_t *f, uint32_t n);

#endif
