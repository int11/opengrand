/* OpenGrand - Phase 3-4 settings state, stage S1 (store first fill) and the compare pass S2 builds on.
 *
 * While the stock RAM still owns the settings (until S4), the core's parameter store is filled from what
 * the stock would answer to a full dump 02 5F 7F 7F: the same descriptor walk as the stock request task
 * (FUN_40152A34: descriptors 0..0xAF, 0x69 and - for v1 0x5F - 0x76 / 0x8E skipped, FUN_4015205C match)
 * and the same value frames (FUN_401526DC: getter FUN_4015E960 + frame builder FUN_4014B0DC, part 0x7F).
 * Each value frame  F0 40 cc 10 04 1E v1 v2 part data.. F7  is parsed and put into the store under the
 * core's own key (param_find / param_slot_from_part) - so the store holds the bytes a dump would send.
 *
 * The io is the session core's (kps_io_t match / value, same function types): on the piano the cave
 * passes the functions that already build the session core's dump frames from the stock routines
 * (ports/cave/sysex_cave.c ss_match / ss_value); on the host a test fake or the emulator round trip
 * (result/stockfw/tools/roundtrip_state.py) does.
 *
 *   fill     put every value, count what the core cannot hold (unknown key, long / variable value); the
 *            two sub-indexed keys (several frames per key) go to a side table, not the store
 *   compare  the same walk, but a value that differs from the store (or is missing in it) is counted as
 *            a difference, remembered (last key) and written - S2 runs this a few descriptors per turn
 * Both are resumable: kst_step() walks at most `budget` descriptors per call, kst_step_calls() at most `calls`
 * value reads (the task-19 pump-tail bound the cave uses).
 * Pure logic, no stock knowledge beyond the frame format. */
#ifndef CA79NG_KPSTATE_H
#define CA79NG_KPSTATE_H

#include <stdbool.h>
#include <stdint.h>

#include "ca79ng/kproto.h"

#define KST_DESC_N 0xB0u      /* stock descriptors 0..0xAF (FUN_40152A34)       */
#define KST_FRAME_MAX 40u     /* == KPS_FRAME_MAX                               */
#define KST_J_MAX 16u         /* most value frames one descriptor gives (0x81/0x82 x16) */
#define KST_IDX_MAX 12u       /* side table for sub-indexed keys (54 05 0F x2, 51 1A 7F x4, 51 1B 7F x2) */

typedef struct {
    void *ctx;
    /* FUN_4015205C(i, v1, v2) != -1 (called with v1 0x5F, v2 0x7F) */
    bool (*match)(void *ctx, uint8_t i, uint8_t v1, uint8_t v2);
    /* value frame j of descriptor i, as the session core's io.value (port, device byte cc, part 0x7F):
     * > 0 frame length, 0 = no frame for this j (go on), < 0 = descriptor done */
    int (*value)(void *ctx, uint8_t port, uint8_t cc, uint8_t i, uint8_t part, uint8_t j, uint8_t out[KST_FRAME_MAX]);
    /* S2 (optional): a compare difference nobody explains (kst_why_t KST_WHY_NONE) - the value frame f (cc 0x7F, the
     * stock's notification bytes) after the store was fixed; the platform fans it out to the bound controllers */
    void (*fan)(void *ctx, const uint8_t *f, uint32_t n);
} kst_io_t;

/* ---- S2: why did a compare pass find a difference? ----------------------------------------
 * Every difference is fixed (the store takes the stock's value).  It is "explained" when the stock changed the value
 * through a path the core knows of, inside the window since the start of the walk before this one (the previous
 * visit of the key - events older than that would have shown up there):
 *   SET      a set / command / recctl command the core applied, side effects included (FUN_4016053C changes more
 *            than the set's key: Sound -> its effects, the deferred apply FUN_401638E0 of descriptor 0x46)
 *   STOCK    a controller frame the core passed to the stock handler (CONNECT -> FUN_40151BA0 -> stop-all
 *            FUN_400B0EA8, VT edit, staging, FUN_4014A1B4 ...)
 *   NOTIFY   the stock notified exactly this key on its binding's TX ring (FUN_4014C048, kpsess tap)
 *   KEY      a key the stock changes by itself, whoever is bound (kst_key_stock: headphone / USB / GM reset /
 *            song load notices, metronome run state, song tempo / part, MIDI channel Program Change / Local Control)
 *   RESTORE  the core's own startup restore (startup_cave.c)
 *   POWER    standby / wake (stop-all FUN_400B0EA8, headphone re-read FUN_400ACC7C, BT module on / off)
 *   REC      the recorder / song state changed (song start loads settings, FUN_40169EDC / FUN_40169F00 stop the
 *            metronome)
 * Anything else is unexplained (NONE): the core missed a change - counted, remembered in a small ring and fanned
 * out through io.fan so every controller's screen follows the piano. */
typedef enum {
    KST_WHY_NONE = 0, KST_WHY_SET, KST_WHY_STOCK, KST_WHY_NOTIFY, KST_WHY_KEY, KST_WHY_RESTORE, KST_WHY_POWER,
    KST_WHY_REC, KST_WHY_N
} kst_why_t;

#define KST_NOTES 8u   /* NOTIFY keys remembered (older live ones overflow into a global NOTIFY event) */
#define KST_RING 8u    /* unexplained differences remembered                                         */

typedef struct {
    uint8_t key[3];       /* v1 v2 part                           */
    uint8_t lens;         /* old length << 4 | new length (0 none) */
    uint8_t old[4];
    uint8_t now[4];
} kst_unexp_t;

typedef struct {
    uint8_t key[3];
    uint8_t live;
    uint32_t ev;          /* event number of the notice            */
} kst_note_t;

typedef enum { KST_IDLE = 0, KST_FILL = 1, KST_COMPARE = 2 } kst_mode_t;

typedef struct {
    uint32_t frames;    /* value frames read                                                   */
    uint32_t stored;    /* put into the store (fill) / found equal or fixed (compare)          */
    uint32_t unknown;   /* key the core's table does not have (v1 v2 part)                     */
    uint32_t longval;   /* known key, but a variable / > 4 byte value the store cannot hold    */
    uint32_t bad;       /* malformed frame, or a value param_validate refuses                  */
    uint32_t diffs;     /* compare: value differed from the store (or was missing) - fixed     */
    uint32_t indexed;   /* of stored: sub-indexed values kept in the side table                */
    uint32_t passes;    /* walks finished                                                      */
} kst_stats_t;

/* one sub-indexed value: key v1 v2 part, data[0] = the sub-index; i / j = where the walk read it (descriptor, frame
 * index - S3 answers a request's frame j of descriptor i from here) */
typedef struct {
    uint8_t key[3];
    uint8_t len;          /* 0 = free */
    uint8_t data[4];
    uint8_t i, j;
} kst_idx_t;

/* S3 read owner: what kst_s3_frame did with the request values it was asked for (since init) */
typedef struct {
    uint32_t served;      /* answered from the store / side table                                             */
    uint32_t stock;       /* not the store's: run-state descriptor (kst_s3_owned) or a key the stock changes itself */
    uint32_t untrusted;   /* the store's, but a change since the last clean walk may not be in it yet           */
    uint32_t miss;        /* the store's, trusted, but the store has no such value (unknown key / not seen)     */
    uint32_t stale;       /* unexplained compare differences found while the store was trusted (served stale)  */
    uint32_t fanned_key;  /* KEY differences of silent keys fanned out (kst_key_silent)                          */
} kst_s3_stats_t;

/* Phase 4 (kpchan): values the core read back right after a stock path it
 * ran itself (MIDI channel Program Change / Control Change) */
typedef struct {
    uint32_t frames;      /* frames given to kst_absorb                                                         */
    uint32_t changed;     /* of them, the store's bytes were different / missing - now fixed                   */
    uint32_t fanned;      /* of the changed ones, handed to io.fan (the rule a compare difference would follow)  */
    uint32_t bad;         /* not a value the store can hold (unknown key, long, malformed)                       */
} kst_abs_stats_t;
/* S4 write owner: what kst_s4_judge / kst_s4_follow / kst_s4_done did (since init) */
typedef struct {
    uint32_t sets;        /* sets judged (S4 on, staging off)                                                   */
    uint32_t values;      /* values in them                                                                       */
    uint32_t own;         /* key known, value valid (length + the stock descriptor's masks): applied, store written */
    uint32_t refused;     /* key known, value invalid: not applied - the stock would have applied it unchecked    */
    uint32_t passed;      /* key the core does not know: applied as the stock would, the store learns by compare  */
    uint32_t followed;    /* own values whose stock value read back after the apply == the store                 */
    uint32_t wmiss;       /* own values the stock did not follow (the store took the stock's value)               */
    uint32_t kept;        /* sets after which the store stayed trusted (every own key pure, all followed)         */
    uint32_t side;        /* compare differences of a kept set found while trusted: the pure table was wrong     */
    uint32_t dropped;     /* sets with nothing left to apply (every value refused)                                */
} kst_s4_stats_t;

#define KST_S4_KEYS 52u   /* keys of one set (== KPS_SET_KEYS: packed body <= 0xF5 B, 4+ B each) */

/* one set as S4 judged it: the keys to read back after the apply */
typedef struct {
    uint8_t key[KST_S4_KEYS][3];
    uint8_t own[KST_S4_KEYS];   /* 1 own, 0 refused (read back too: the front's mirror may hold it)          */
    uint8_t n;                  /* keys listed (known, single-valued, <= 4 B)                                */
    uint8_t nown, refused, passed, unlisted;   /* unlisted: own values with no read-back (indexed, long)      */
    uint8_t miss;               /* own read-backs that differed                                              */
    bool pure;                  /* every own key in the pure table                                           */
} kst_s4_set_t;

typedef struct {
    kst_io_t io;
    param_store_t *store;
    kst_mode_t mode;
    uint8_t i, j;         /* walk cursor                                  */
    bool synced;          /* a fill pass finished (S1 end condition)      */
    kst_stats_t st;       /* counters of the current / last walk          */
    uint32_t diffs_total; /* compare differences since init               */
    uint8_t last_diff[3]; /* v1 v2 part of the last difference            */
    uint8_t last_unknown[3];
    kst_idx_t idx[KST_IDX_MAX];
    /* slot bits of the store values a walk has read (the keys a dump has: the startup snapshot's key set,
     * kpstart.h) - set by fill and compare, never cleared */
    uint32_t seen[PRM_COUNT];
    /* S2: the explanation window and what the compare passes found */
    uint32_t ev;                   /* known stock-change events so far (kst_event / kst_note)              */
    uint32_t last_ev[KST_WHY_N];   /* the last global event per reason (0 = none)                          */
    uint32_t mark_prev, mark_cur;  /* ev at the start of the previous / current walk                       */
    uint32_t mark_save;            /* mark_prev before kst_start (kst_abort puts both back)                */
    kst_note_t notes[KST_NOTES];
    uint8_t note_next;
    uint8_t last_why;              /* reason of the last difference                                         */
    uint8_t ring_head;             /* next ring slot                                                       */
    uint32_t why[KST_WHY_N];       /* compare differences per reason since init; why[0] = unexplained       */
    uint32_t fanned;               /* unexplained differences handed to io.fan                              */
    kst_unexp_t ring[KST_RING];
    /* S3: the store answers requests while it is trusted */
    bool s3_on;                    /* kst_s3_enable                                                         */
    bool clean;                    /* a walk finished with no store-relevant event while it ran              */
    uint32_t gen;                  /* store-relevant events (kst_event, kst_note of a key not kst_key_stock)  */
    uint32_t gen_start;            /* gen when the current walk started                                     */
    uint32_t clean_gen;            /* gen at the end of the last clean walk                                 */
    kst_s3_stats_t s3;
    kst_abs_stats_t abs;
    /* S4: the core judges and writes every set first */
    bool s4_on;                    /* kst_s4_enable                                                          */
    kst_s4_stats_t s4;
    uint8_t s4_last_refused[3], s4_last_miss[3], s4_last_side[3];
} kst_t;

void kst_init(kst_t *s, param_store_t *store, const kst_io_t *io);

/* start a walk (resets the walk counters, keeps diffs_total / synced) */
void kst_start(kst_t *s, kst_mode_t mode);

/* walk at most `budget` descriptors; true when the walk is finished (mode back to KST_IDLE) */
bool kst_step(kst_t *s, uint32_t budget);

/* walk at most `calls` io.value calls (>= 1) - the cave's per-pass bound: one value frame costs the stock getter +
 * builder (~1.7-2.5 k instructions in the emulator), while one descriptor can give 16 of them (0x81 / 0x82 slots) */
bool kst_step_calls(kst_t *s, uint32_t calls);

/* the whole walk at once (host tools, tests) */
void kst_run(kst_t *s, kst_mode_t mode);

typedef enum { KST_PUT = 0, KST_SAME, KST_UNKNOWN, KST_LONG, KST_BAD } kst_frame_t;

/* keys whose first data byte is a sub-index (several frames per key in one dump) - side table */
bool kst_is_indexed(uint8_t v1, uint8_t v2, uint8_t part);
/* did a walk read store value (id, slot)? */
bool kst_seen(const kst_t *s, int id, int slot);
/* a side-table value: its length, 0 = none */
int kst_indexed_get(const kst_t *s, uint8_t v1, uint8_t v2, uint8_t part, uint8_t sub, uint8_t out[4]);

/* one value frame into the store: what happened to it (fill and compare share it; `changed` set when the
 * store's bytes were different or missing) */
kst_frame_t kst_put_frame(param_store_t *store, const uint8_t *f, uint32_t n, bool *changed);

/* ---- S2 ---- */
/* a global event of reason why (SET, STOCK, RESTORE, POWER, REC; NOTIFY = a notice whose key was not kept) */
void kst_event(kst_t *s, kst_why_t why);
/* the stock notified key v1 v2 part (NOTIFY) */
void kst_note(kst_t *s, const uint8_t key[3]);
/* REA-proven keys the stock changes by itself whoever is bound (KEY) */
bool kst_key_stock(uint8_t v1, uint8_t v2, uint8_t part);
/* why would a difference of this key be explained now (KST_WHY_NONE = unexplained) */
kst_why_t kst_explain(const kst_t *s, const uint8_t key[3]);
/* stop the current walk without finishing it (not a pass; the explanation window stays where it was) */
void kst_abort(kst_t *s);
/* unexplained differences held in the ring (<= KST_RING); k-th newest (0 = the last one), false if none */
uint32_t kst_ring_count(const kst_t *s);
bool kst_ring_get(const kst_t *s, uint32_t k, kst_unexp_t *out);

/* ---- S3: read owner ------------------------------------------------
 * Requests (fn 0x02) are answered from the store instead of the stock getter, value by value, when all of these hold:
 *   - S3 is on (kst_s3_enable; the cave: feature bit + op 1B 05 04)
 *   - the descriptor is a settings descriptor (kst_s3_owned): REA FUN_4015E960 reads its value straight from the
 *     settings block (FUN_4015B4D8, ctx 0..5 = one block) / the live global (FUN_4015B4D0) or through a ROM table, and
 *     whether it has a value depends on x only - not a run-state call (recorder, BT, song, VT, MusicMode, Sound table)
 *   - the key is not one the stock changes by itself (kst_key_stock: no event tells the core)
 *   - the store is trusted: synced, and a whole walk has run since the last store-relevant event with none during it
 *     (kst_event, a notice of a non-stock key; kst_event_run does not count - its paths change run state only)
 *   - the store holds the value (key known, read by a walk) - else the stock answers (miss)
 * The frame is the stock builder's (FUN_4014B0DC): F0 40 cc 10 04 1E v1 v2 part data.. F7, data = the stored bytes. */
bool kst_s3_owned(uint8_t i);
/* KEY keys the stock changes without telling any controller (MIDI Program Change -> 55 00 <part>, CC 0x7A -> 58 01 7F):
 * with S3 on, a compare difference of one is fanned out like an unexplained one */
bool kst_key_silent(uint8_t v1, uint8_t v2, uint8_t part);
void kst_s3_enable(kst_t *s, bool on);
bool kst_s3_trusted(const kst_t *s);
/* an event whose stock paths change run-state keys only (REA: CONNECT / DISCONNECT -> FUN_40151BA0 stop-all): it explains
 * S2 differences like kst_event but leaves the store trusted */
void kst_event_run(kst_t *s, kst_why_t why);
/* frame j of descriptor i whose key is v1 v2 part (the stock's own derivation: descriptor bytes 1-2 + key byte), for a
 * request with device byte cc: > 0 the frame from the store, 0 = not the store's - the caller asks the stock */
int kst_s3_frame(kst_t *s, uint8_t i, uint8_t j, uint8_t cc, const uint8_t key[3], uint8_t out[KST_FRAME_MAX]);

/* ---- Phase 4: same-tick store update -------------------------------
 * The value frame f (cc 0x7F, the stock builder's bytes) of a key a stock path the core just ran may have changed: into the
 * store now, so the next compare pass finds nothing.  When the store changed, io.fan gets the frame under the rule the
 * compare pass would have applied one walk later: a silent stock key (kst_key_silent) only while S3 is on, any other key
 * that is not the stock's own (kst_key_stock) always (it would have been an unexplained difference).  Not a store-relevant
 * event (gen unchanged): the store holds the new value.  Returns true when the store changed. */
bool kst_absorb(kst_t *s, const uint8_t *f, uint32_t n);

/* ---- S4: write owner -------------------------------------------------
 * A fn 0x10 set of a bound controller is judged value by value before the stock applies it:
 *   own      key known (param_find), data length == the descriptor's, every byte within the stock descriptor's value
 *            mask (param_validate) - the store takes it (the front's mirror) and the stock applies it
 *   refused  key known, value not valid - not applied (the stock's FUN_4014EBFC has no range check: REA)
 *   passed   key unknown to the core - applied as the stock would; the compare walk teaches the store
 * The frame the stock applies is the set itself, or - when values were refused - the set without them (packed) / nothing.
 * After the apply the platform reads every listed key back from the stock (getter) into kst_s4_follow: equal = followed,
 * else a write miss (the store takes the stock's value).  kst_s4_done then tells the S2/S3 bookkeeping: when every own
 * key is in the pure table (its apply changes no other dump value - emulator-measured, roundtrip_state 8), nothing was
 * passed and nothing missed, the store stays trusted (kst_event_run); else the S3 rule (kst_event: untrusted until a
 * clean walk).  A compare difference found while trusted that only a kept set explains counts as side (pure table wrong)
 * and drops the trust.  With S4 off nothing here runs: exactly S3. */
void kst_s4_enable(kst_t *s, bool on);
bool kst_s4_pure(uint8_t v1, uint8_t v2, uint8_t part);
uint32_t kst_s4_pure_count(void);   /* entries of the pure table */
/* m: the set F0 40 cc 10 08 02 .. F7 (n bytes, n >= 10).  out (>= n bytes): the frame to apply; returns its length, 0 =
 * nothing to apply.  r: filled for the read-back */
uint32_t kst_s4_judge(kst_t *s, const uint8_t *m, uint32_t n, uint8_t *out, kst_s4_set_t *r);
/* the stock's value frame (cc 0x7F, as io key_value gives) of listed key k after the apply */
void kst_s4_follow(kst_t *s, kst_s4_set_t *r, uint32_t k, const uint8_t *f, uint32_t fn);
/* after the read-back: the S2 event and the S3 trust (no event at all when nothing was applied) */
void kst_s4_done(kst_t *s, const kst_s4_set_t *r);

#endif
