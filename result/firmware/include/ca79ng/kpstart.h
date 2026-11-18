/* OpenGrand - startup settings owned by the core.
 *
 * The piano keeps its startup settings itself: a mode and a snapshot in the core's kvstore (NOR unused_3), and
 * every controller (app, panel, phone) only reads and writes the piano's values.
 *   mode     KSU_STOCK     nothing from the core: the stock boots from its UserMemory (the default)
 *            KSU_CURRENT   restore snapshot C, taken only when asked ("save current settings as startup")
 *            KSU_POWEROFF  restore snapshot P, taken 3 s after the last change (min. 10 s apart) and on standby / APO
 *   snapshot a serialization of the settings store that the Phase 3-4 S1 walk keeps equal to the stock
 *            (kpstate.h): every value the dump has (kst_seen) and the side table, minus the keys that are not
 *            startup settings (run state, notifications, BT MIDI - ksu_excluded).  AutoPowerOff / LockMode are in it
 *            too (the stock keeps them in settings block A; a core set at boot changes them like the
 *            app's set, the stock's own shutdown write keeps them).
 *            Bytes: 'K' 'S' 1 0 | count u16 | 0 0 | count x (v1 v2 part len data[len]).
 *   NG part  (user decision 2026-11-13 "A": the modes govern every piano setting) our own settings that live
 *            in the cave ports: the BT policy's kept part (bts_saved_*: reconnect choice, preferred device, "follow")
 *            and the core power policy (power_policy_t wire form).  Saved with each snapshot as its own kvstore record
 *            per bank (base + 1 + 2 * KSU_CHUNKS + bank): 'N' 'G' 1 bank | gen u32 | payload | crc32, payload =
 *            1 | bt_len bt[] | pw_len pw[].  It belongs to the snapshot whose header names that bank with that gen -
 *            a snapshot written by a build before (no NG record, or an old one of another gen) has no NG part, and
 *            builds before simply ignore the record (a rollback still restores the stock part).
 *   at boot  (never after a live apply) ksu_ng_decide: stock -> the factory defaults of the NG part + AutoPowerOff /
 *            LockMode factory values (ksu_snap_factory) when the mode was chosen by this kind of build
 *            (KV_KEY_STARTUP_ALL); current / power-off -> the snapshot's NG part when it has one; otherwise (a mode or
 *            snapshot from a build before) the live values on NOR stay (keys 0x0040 / 0x0041, as before).
 *   restore  each record whose value differs from the store becomes a "core-originated set" through the stock
 *            apply path (the platform: kps_set_begin(KPS_FROM_CORE) + FUN_4014EBFC(ctx 0) + kps_set_end), so the
 *            fan-out tells every bound controller; then a compare walk and one more round for what a set changed
 *            in passing (Sound -> reverb ...).
 *   kvstore  KV_KEY_STARTUP_MODE [1, mode]; per snapshot (C = KV_KEY_STARTUP_C, P = KV_KEY_STARTUP_P) a 16-byte
 *            header record at the base key and two banks of up to KSU_CHUNKS chunks (base + 1 + bank * KSU_CHUNKS
 *            + k).  A save writes the chunks into the bank the header does not name, then the header: a write cut
 *            anywhere leaves the old snapshot whole (the header still names the old bank, or the new header's CRC
 *            covers only finished chunks).
 * Pure logic over the store / kpstate / kvstore types: the cave port is ports/cave/startup_cave.c. */
#ifndef CA79NG_KPSTART_H
#define CA79NG_KPSTART_H

#include <stdbool.h>
#include <stdint.h>

#include "ca79ng/kproto.h"
#include "ca79ng/kpstate.h"
#include "ca79ng/storage.h"

typedef enum { KSU_STOCK = 0, KSU_CURRENT = 1, KSU_POWEROFF = 2, KSU_MODES = 3 } ksu_mode_t;
typedef enum { KSU_C = 0, KSU_P = 1 } ksu_slot_t;

#define KSU_CHUNK 512u                        /* == KV_MAX_VALUE                      */
#define KSU_CHUNKS 4u
#define KSU_SNAP_MAX (KSU_CHUNK * KSU_CHUNKS)  /* 2 KB; the whole store is ~1.3 KB     */
#define KSU_SNAP_HEAD 8u
#define KSU_KV_HDR 16u
#define KSU_FRAME_MAX 40u
#define KSU_QUIET_MS 3000u                    /* save 3 s after the last change ...    */
#define KSU_GAP_MS 10000u                     /* ... and at least 10 s after the last  */

/* not a startup setting (any part): run state, sensors, notifications, settings the stock keeps outside
 * UserMemory (APO / Lock - settings block A), the BLE MIDI switch (a restore must never lock the app out) */
bool ksu_excluded(uint8_t v1, uint8_t v2);

/* serialize the store values a walk has read + the side table, minus the excluded keys.  Bytes, -1 = > cap. */
int ksu_snap_build(const param_store_t *store, const kst_t *st, uint8_t *out, uint32_t cap);
/* well formed (head, records inside n, count matches)?  count may be NULL */
bool ksu_snap_check(const uint8_t *snap, uint32_t n, uint16_t *count);
/* record at byte `pos` (start: KSU_SNAP_HEAD): its length (>= 4) and *rec, 0 at the end, -1 malformed */
int ksu_rec(const uint8_t *snap, uint32_t n, uint32_t pos, const uint8_t **rec);
/* does record rec (v1 v2 part len data) differ from the store / side table?  1 yes, 0 no, -1 the core cannot
 * hold that key (unknown, or a side-table key with no entry) - the platform applies it anyway */
int ksu_rec_differs(const uint8_t *rec, const param_store_t *store, const kst_t *st);
/* the set F0 40 7F 10 08 02 v1 v2 part data F7 of a record (length), and the value frame the stock would
 * notify for it (F0 40 7F 10 04 1E ..) - what the store takes after the set */
uint32_t ksu_set_frame(const uint8_t *rec, uint8_t out[KSU_FRAME_MAX]);
uint32_t ksu_value_frame(const uint8_t *rec, uint8_t out[KSU_FRAME_MAX]);

/* save debouncer (ms clock): dirty after a change, due KSU_QUIET_MS after the last one and KSU_GAP_MS after the
 * last save */
typedef struct {
    uint32_t quiet_ms, gap_ms;
    uint32_t last_change, last_save;
    bool dirty, saved;
} ksu_deb_t;
void ksu_deb_init(ksu_deb_t *d, uint32_t quiet_ms, uint32_t gap_ms);
void ksu_deb_change(ksu_deb_t *d, uint32_t now);
bool ksu_deb_due(const ksu_deb_t *d, uint32_t now);
void ksu_deb_saved(ksu_deb_t *d, uint32_t now);

/* kvstore */
enum { KSU_KV_OK = 0, KSU_KV_NOROOM = 1, KSU_KV_FAIL = 2 };
ksu_mode_t ksu_kv_mode(kvstore_t *kv);                       /* KSU_STOCK when absent / bad */
/* writes the mode record and then KV_KEY_STARTUP_ALL with the same mode */
bool ksu_kv_set_mode(kvstore_t *kv, ksu_mode_t mode);
/* the mode was chosen by a build whose modes cover every setting (KV_KEY_STARTUP_ALL names the mode on NOR) */
bool ksu_kv_mode_all(kvstore_t *kv);
/* bytes a save of an n-byte snapshot with an ngn-byte NG payload appends (chunks + NG record + header) */
uint32_t ksu_kv_need(uint32_t n, uint32_t ngn);
/* save snapshot `slot` (+ its NG payload, ngn 0 = none); without allow_compact a save that does not fit the active
 * sector is refused (KSU_KV_NOROOM: a compaction erases a sector with dispatch off - only in standby / before the off) */
int ksu_kv_save(kvstore_t *kv, ksu_slot_t slot, const uint8_t *snap, uint32_t n, const uint8_t *ng, uint32_t ngn,
                bool allow_compact);
/* the NG payload of snapshot `slot` (the one the header names): its length, -1 = none (a snapshot of a build before) */
int ksu_kv_load_ng(kvstore_t *kv, ksu_slot_t slot, uint8_t *out, uint32_t cap);
/* load snapshot `slot`: its length, -1 = none / torn / bad; gen (may be NULL) = its save count */
int ksu_kv_load(kvstore_t *kv, ksu_slot_t slot, uint8_t *out, uint32_t cap, uint32_t *gen);
/* both snapshots gone, mode stock (factory reset, op 1C 03) */
bool ksu_kv_clear(kvstore_t *kv);

/* ---- the NG part ---- */
#define KSU_NG_BT 10u    /* == BTS_SAVED_RAW (btsrc.h)                  */
#define KSU_NG_PW 8u     /* == POWER_POLICY_RAW (power.h)               */
#define KSU_NG_MAX 32u   /* payload cap                                 */
#define KSU_NG_REC (12u + KSU_NG_MAX)
/* payload 1 | btn bt[btn] | pwn pw[pwn] (a part of length 0 = not carried): its length, 0 = does not fit */
uint32_t ksu_ng_build(const uint8_t *bt, uint8_t btn, const uint8_t *pw, uint8_t pwn, uint8_t out[KSU_NG_MAX]);
/* the parts of a payload (false = malformed); a part not carried comes back with length 0 */
bool ksu_ng_parts(const uint8_t *ng, uint32_t n, const uint8_t **bt, uint8_t *btn, const uint8_t **pw, uint8_t *pwn);
/* what the NG part starts from at a power-on */
typedef enum {
    KSU_NG_KEEP = 0,      /* the live values on NOR (a live apply, or a mode / snapshot of a build before) */
    KSU_NG_DEFAULTS = 1,  /* factory defaults (mode stock chosen by a build with the NG part, or a factory reset) */
    KSU_NG_SAVED = 2,     /* the snapshot's NG part */
} ksu_ng_t;
ksu_ng_t ksu_ng_decide(ksu_mode_t mode, bool all, bool at_boot, bool have_ng);
/* the stock's factory AutoPowerOff index for the settings block A words 0 (model code) and 3 (destination) - the rule of
 * FUN_400B0B9C (REA): destination 0xA5A5AA01 -> 1 (15 min); 0xAAA55504 /
 * 0x555AA503 / 0x55AA5A02 -> 0; otherwise model code 0xAA5AA502 -> 1, else 0 */
uint8_t ksu_apo_factory(uint32_t model_code, uint32_t dest);
/* a snapshot of the two block A settings at their factory values: 51 00 7F apo, 51 22 7F 00 00 (bytes, -1 = > cap) */
int ksu_snap_factory(uint8_t apo, uint8_t *out, uint32_t cap);

#endif
