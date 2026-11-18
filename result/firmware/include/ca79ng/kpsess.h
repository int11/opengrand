/* OpenGrand - Phase 3-2 session core (stage 1): controller bindings, CONNECT / DISCONNECT, request
 * answers (fn 0x02) with a per-port non-blocking stop-and-wait sender on 32-bit time, and the
 * ACK / NAK / Wait / Cancel assignment between that sender and the stock one.
 *
 * The rules chosen for stage 1:
 *
 *   - Bindings: one bit per stock port 0..3 (3 = the panel, home).  The stock keeps ONE binding
 *     (*0x413CA3DE, "rep" here = stock port + 1); stage 1 keeps it on the controller that
 *     CONNECTed last, so that controller has every stock path (VT edit, files, recorder,
 *     notifications) exactly as on the stock.  A CONNECT never displaces anybody: the other
 *     controller stays bound in the core (its sets go through the front, its requests through the
 *     core, its ACKs to the core sender).  The table follows the stock binding too (a binding the
 *     stock makes is added, one the stock drops is removed).
 *   - CONNECT 7E 01 01 from p: stock unbound or already p -> passed (stock answers, binds).
 *     Otherwise the core answers 7E 02 mode sub itself (the stock's other-port bytes), makes p the
 *     stock binding through io.take_rep (stock ctx copy + bind, no 7E 01 00 to the old one).
 *   - DISCONNECT 7E 01 00 from the stock-bound port -> passed; from a port only the core binds ->
 *     unbound by the core, no frame.
 *   - Request 02 v1 v2 part (n 10) from a bound port: the core answers (the stock request task
 *     FUN_40152A34 / FUN_401526DC order: descriptors 0..0xAF, value frames from io.value, End
 *     03 v1 v2 7F, Cancel checked after each descriptor).  Packed 02 5F 00 xx: stock, rep port only.
 *     One frame in flight per port, 500 ms per try, 16 tries, time in 32-bit ms (no 16-bit wrap).
 *     16 failures: the request ends without End, the port is unbound unless it is the panel.
 *   - fn 07 / 06 / 04 (ACK / NAK / Wait): the core's in-flight key on that port first; else to the
 *     stock only if the port is the stock binding and the function is not Wait (04 is never passed:
 *     the stock wait loop never re-reads its flags).  fn 05 Cancel: ends the core request at the
 *     next descriptor; passed too when from the stock binding.  fn 0x24 7C (file Wait): dropped.
 *
 * Stage 2 - change fan-out: every value change reaches every
 * core-bound controller, as the stock's own notification frame  F0 40 7F 10 04 1E v1 v2 part data F7
 * (FUN_4014C048 -> FUN_4014C010 -> builder FUN_4014B0DC, cc 0x7F), each port on its own reliable sender
 * (the same in-flight slot as the request answers: one frame at a time, 500 ms x 16, 32-bit time).
 *   - a set the core front applied (kps_set_begin / kps_set_end around the stock apply): the value frames of
 *     every key in the set are read before and after (io.key_value = stock getter + builder); the ones that
 *     changed go to every bound port except the one that sent the set (the stock never echoes a set: it
 *     only ACKs it, FUN_4014A2C8 fn 0x10).  No change, no frame (the panel's cache push at boot is silent).
 *   - a notification the stock sent its binding (headphone 51 29, USB memory 51 2E, MaxTime 52 18, MIDI
 *     mode 58 10 - the notify paths FUN_4014C048 / FUN_40151A38): the platform hands the bytes the stock
 *     wrote into the binding's TX ring to kps_tap(); a complete fn 0x10 04 1E frame (not the packed
 *     dump 5F 00, not VT edit) goes to every other bound port.  fn 0x32 04 1E frames (song progress
 *     32 52 03 - the stock sends them without an ACK wait) are relayed at once, unreliably, the same way -
 *   except BeatCounter 32 56 0B, which the metronome module makes itself (kpmetro.h, kps_relay).
 *   Per port a FIFO of KPS_FANQ frames; a pending frame of the same key is replaced in place (the newest
 *   value, no storm).  A fan frame interleaves with a request answer at frame granularity, as the stock's
 *   notifications (task 24 / 28) interleave with its sender.  A fan frame's ACK from the stock binding's
 *   port is also passed to the stock (same-key notifications in flight there are not starved).
 *
 * File access from any bound controller (field 2026-11-09): the stock runs fn 0x24
 * for its binding only (FUN_4014A2C8 0x4014A894: FUN_40151BCC(port) == 0 -> return, no ACK) and every reply of a file job -
 * the request ACK 24 7F, FileName 24 03 + 24 1F (file task, stop-and-wait FUN_4014B638), TopOfData / data / 7B, the results
 * 32 51 7E and 7E 03 - goes out through FUN_4014AD04, which reads the binding *0x413CA3DE at each send.  There is no reply
 * port of its own.  So a request (24 04..0B, 11, 12) from a bound controller that is not the stock binding takes a file
 * lease: the core makes it the stock binding (io.file_bind, the stock's own FUN_40151B5C) for that one job, the stock runs
 * the job as for its binding (the requester's ACK / NAK reach the stock flags in order, its 24 7C Wait is still dropped),
 * and when the job is over (io.file_state idle, KPS_FILE_QUIET_MS without a file frame either way) the binding goes back
 * to the controller that held it.  One job at a time: while a lease or the holder's own job runs, another controller's
 * request is answered 24 7F + Busy 7E 03 01 00 00 by the core (the stock's bytes, FUN_4014B8A0 + FUN_4014BEF0(0)), its
 * other fn 0x24 frames are dropped (as the stock drops an unbound port's).
 * Pure logic: the platform binds kps_io_t (ports/cave/sysex_cave.c on the piano, tests on the host). */
#ifndef CA79NG_KPSESS_H
#define CA79NG_KPSESS_H

#include <stdbool.h>
#include <stdint.h>

#define KPS_PORTS 4u
#define KPS_HOME 3u          /* the built-in panel's stock port */
#define KPS_FRAME_MAX 40u
#define KPS_TRIES 16u        /* FUN_4014BD1C: 16 sends                      */
#define KPS_WAIT_MS 500u     /* FUN_4014BCAC(250) on the 2 ms counter        */
#define KPS_NAK_MS 5u        /* dly_tsk(5) before the stock's next send      */
#define KPS_WAITS_MAX 16u    /* Wait (fn 04) extends a try at most this often */
#define KPS_DESC_N 0xB0u     /* request loop 0..0xAF                         */
#define KPS_FANQ 16u         /* fan-out frames waiting per port               */
#define KPS_SET_KEYS 52u     /* keys of one set (packed body <= 0xF5 B, 4+ B each) */
#define KPS_SNAP 64u         /* value frames remembered across one set        */
#define KPS_KEY_FRAMES 4u    /* frames one key can have (sub-indexed 51 1A 7F x4) */
#define KPS_FILE_QUIET_MS 600u   /* a lease ends after this long without a file frame (> one 500 ms stock try) */
#define KPS_FILE_SOFT_MS 8000u   /* a pending job / upload state counts as busy this long after the last file frame */
/* io.file_state bits (the stock's fn 0x24 job state) */
#define KPS_FS_HARD 0x01u    /* list running (0x413CA3EC) or a name complete, executor due (0x413CA3EB)       */
#define KPS_FS_SOFT 0x02u    /* job pending (0x413CA3EA), write waiting (0x413CA3ED), upload data (0x413CA3E5) */

typedef struct {
    void *ctx;
    uint8_t (*binding)(void *ctx);   /* stock binding = stock port + 1, 0 none (*0x413CA3DE) */
    uint32_t (*device)(void *ctx);   /* receive device number (*0x413B5404)                    */
    bool (*forward)(void *ctx);      /* port 3 -> 0..2 forwarding (*0x413B5408 != 0)          */
    void (*tx)(void *ctx, uint8_t port, const uint8_t *m, uint16_t n);
    /* FUN_4015205C(i, v1, v2) != -1: descriptor i answers a request for v1 v2 */
    bool (*match)(void *ctx, uint8_t i, uint8_t v1, uint8_t v2);
    /* value frame j of descriptor i (request device byte cc, part byte) for a request from port:
     * > 0 frame length in out, 0 = nothing for this j, < 0 = no frame for j or any later j */
    int (*value)(void *ctx, uint8_t port, uint8_t cc, uint8_t i, uint8_t part, uint8_t j,
                 uint8_t out[KPS_FRAME_MAX]);
    /* CONNECT from port while another port is the stock binding: the stock's reply bytes
     * (reads and clears port's remote-change flag); then take_rep makes port the stock binding
     * without displacing, and connect_post runs the stock's after-CONNECT mode step */
    void (*connect_info)(void *ctx, uint8_t port, uint8_t *mode, uint8_t *sub);
    void (*take_rep)(void *ctx, uint8_t port);
    void (*connect_post)(void *ctx);
    void (*stock_unbind)(void *ctx);            /* 16 failures on the stock-bound port       */
    void (*remote_clear)(void *ctx, uint8_t port);   /* a request answered in full            */
    bool (*link_up)(void *ctx, uint8_t port);   /* BT link of ports 1 / 2 (others: true)     */
    /* stage 2: the k-th current value frame (cc 0x7F, the stock notification bytes) whose key is
     * v1 v2 part (v1 0x70: v1 v2); > 0 length, <= 0 no such frame.  NULL = no fan-out of sets. */
    int (*key_value)(void *ctx, uint8_t v1, uint8_t v2, uint8_t part, uint8_t k, uint8_t out[KPS_FRAME_MAX]);
    /* stage 2 (optional): a stock parameter notification seen on the binding's TX ring (the frames counted in
     * tap_frames) - Phase 3-4 S2 explains a store difference of that key with it (kpstate.h KST_WHY_NOTIFY) */
    void (*tapped)(void *ctx, const uint8_t *f, uint8_t n);
    /* file lease (optional, both or neither): make stock port `port` the stock binding (FUN_40151B5C, nothing else), and
     * the stock's fn 0x24 job state (KPS_FS_* bits).  NULL = fn 0x24 stays the stock's binding-only path. */
    void (*file_bind)(void *ctx, uint8_t port);
    uint8_t (*file_state)(void *ctx);
} kps_io_t;

typedef enum { KPS_PASS = 0, KPS_DONE = 1 } kps_verdict_t;

typedef struct {
    uint32_t requests;    /* fn 0x02 taken                         */
    uint32_t dropped;     /* fn 0x02 dropped: busy / not bound      */
    uint32_t values;      /* value frames (first sends)             */
    uint32_t ends;        /* End frames (first sends)               */
    uint32_t resends;     /* sends after a timeout / NAK            */
    uint32_t timeouts;
    uint32_t naks;
    uint32_t waits;       /* Wait matched by the core               */
    uint32_t acks;        /* ACK matched by the core                */
    uint32_t to_stock;    /* ACK / NAK / Cancel / fn 24 7D-7F passed (stock binding)  */
    uint32_t ack_drop;    /* ACK / NAK dropped (neither core key nor stock binding)    */
    uint32_t wait_drop;   /* Wait (04, 24 7C) not given to the stock                   */
    uint32_t cancels;
    uint32_t failed;      /* requests ended by 16 failures          */
    uint16_t conn_own;    /* CONNECT answered by the core           */
    uint16_t conn_pass;   /* CONNECT passed to the stock            */
    uint16_t disc_own;    /* DISCONNECT handled by the core         */
    uint16_t unbinds;     /* ports unbound by 16 failures / link down / stock */
    /* stage 2 fan-out */
    uint32_t fan_sets;    /* sets that changed at least one value (fanned)          */
    uint32_t fan_same;    /* sets fanned out to nobody: no value changed            */
    uint32_t fan_queued;  /* frames put in a port's queue                            */
    uint32_t fan_merged;  /* of those: replaced a pending frame of the same key      */
    uint32_t fan_sent;    /* fan frames sent (first sends)                           */
    uint32_t fan_acks;    /* fan frames ACKed                                        */
    uint32_t fan_drop;    /* queue full: the oldest pending frame dropped            */
    uint32_t fan_fail;    /* fan frames given up after 16 sends                      */
    uint32_t tap_frames;  /* stock notifications seen on the binding's TX ring       */
    uint32_t tap_fanned;  /* of those: fanned out                                    */
    uint32_t tap_long;    /* frames longer than KPS_FRAME_MAX (not fanned)            */
    uint32_t snap_over;   /* set keys / frames beyond the snapshot: fanned unchecked */
    uint32_t tap_relayed; /* fn 0x32 frames (no ACK) relayed at once, per port         */
    uint16_t resume_invites; /* kps_resume: the panel invited after a live payload swap */
    /* file lease */
    uint16_t file_leases;  /* fn 0x24 jobs run for a controller that was not the stock binding        */
    uint16_t file_back;    /* leases ended by the core: the binding given back to its holder          */
    uint16_t file_lost;    /* leases ended by somebody else (CONNECT, DISCONNECT, the stock's unbind)  */
    uint16_t file_busy;    /* requests answered Busy by the core (another job running)                */
} kps_stats_t;

typedef struct {
    bool active;          /* a request is being answered            */
    bool end;             /* the End frame is (to be) in flight      */
    bool cancel;
    uint8_t cc, v1, v2, part;
    uint8_t i, j;         /* next descriptor / sub index             */
    /* the frame in flight */
    bool busy;
    bool fan;             /* the frame in flight is a fan-out frame  */
    bool nakd;            /* the last try ended by NAK              */
    uint8_t frame[KPS_FRAME_MAX];
    uint16_t n;
    uint8_t tries, waits;
    uint32_t due;         /* resend at this time (ms)                */
} kps_port_t;

typedef struct {
    uint8_t n;            /* frame length                            */
    uint8_t frame[KPS_FRAME_MAX];
} kps_fan_t;

typedef struct {
    kps_fan_t q[KPS_FANQ];   /* FIFO, q[0] first                     */
    uint8_t cnt;
} kps_fanq_t;

typedef struct {
    uint8_t key[3];
    uint8_t k;            /* frame index within the key             */
    uint32_t h;           /* hash of the frame bytes                */
} kps_snap_t;

typedef struct {
    kps_io_t io;
    bool enabled;
    uint8_t bound;        /* bit p = stock port p bound             */
    uint8_t last_binding; /* the stock binding seen last            */
    uint8_t link_prev;    /* bit p = link was up                    */
    uint32_t now;         /* ms                                      */
    kps_port_t port[KPS_PORTS];
    kps_stats_t st;
    /* stage 2 */
    kps_fanq_t fan[KPS_PORTS];
    bool set_on;          /* between kps_set_begin and kps_set_end   */
    uint8_t set_from;
    uint8_t nkeys, nsnap;
    uint8_t keys[KPS_SET_KEYS][3];
    kps_snap_t snap[KPS_SNAP];
    uint8_t tap_buf[KPS_FRAME_MAX];
    uint8_t tap_n;        /* bytes of the frame being assembled      */
    bool tap_in;          /* inside F0 .. F7                         */
    bool tap_over;        /* that frame is longer than tap_buf       */
    /* file lease */
    uint8_t lease;        /* stock binding the lease made (owner port + 1), 0 none */
    uint8_t lease_rep;    /* the stock binding before it (given back at the end)   */
    uint32_t file_last;   /* ms of the last fn 0x24 frame in (any port) or stock byte to the owner */
} kps_t;

void kps_init(kps_t *s, const kps_io_t *io, uint32_t now_ms);

/* One received Kawai frame from stock port 0..3 (n = wire length with F7). */
kps_verdict_t kps_rx(kps_t *s, uint8_t port, const uint8_t *m, uint32_t n, uint32_t now_ms);

/* Periodic (task 19 pump tail): resends due frames, link-down check, binding sync. */
void kps_tick(kps_t *s, uint32_t now_ms);

/* A payload that replaced a running one (ota_loader_v2 "apply now", op 0x15) starts with an empty
 * table: kps_init only knows the stock binding.  The panel never drops its USB link, so it would not
 * CONNECT again by itself and stays out of the table (field 2026-11-01).  Call once, on the first
 * task-19 pass after kps_init: when a controller is stock-bound (the piano has been in use - at the
 * boot load nobody is yet) and the panel (KPS_HOME) is not in the table, the panel gets the stock's
 * own invitation F0 40 7F 7E 04 1E 01 01 F7 (FUN_4014C080: port 3 only, no forwarding copies) and
 * CONNECTs again - the core answers it like any other-port CONNECT, nobody is displaced.  Returns
 * true when the invitation went out. */
bool kps_resume(kps_t *s);

/* Is stock port p bound (core table)? */
bool kps_bound(const kps_t *s, uint8_t port);

/* Turn the session core off (everything passes to the stock, requests in progress end) / on. */
void kps_enable(kps_t *s, bool on);

/* Stage 2.  A set frame m (n bytes, single or packed 10 5F 00) from port `from` that the front is about
 * to apply: remember the current value frames of its keys.  kps_set_end after the stock apply fans out
 * the frames that changed to every bound port except `from`.  Nothing happens unless another port is
 * bound (and io.key_value is set). */
void kps_set_begin(kps_t *s, uint8_t from, const uint8_t *m, uint32_t n);
/* `from` of a set the core itself makes (startup restore, kpstart.h): no controller sent it, so its changes go to
 * every bound port - the stock binding's too (a set has no stock echo, so nobody gets it twice) */
#define KPS_FROM_CORE 0xFFu
void kps_set_end(kps_t *s);

/* Stage 2.  Bytes the stock wrote into stock port `port`'s TX ring (the stock binding's port; the
 * platform leaves out the bytes the core itself wrote).  Complete notifications are fanned out to the
 * other bound ports.  kps_tap_reset: forget a frame half seen (the binding moved). */
void kps_tap(kps_t *s, uint8_t port, const uint8_t *b, uint32_t n);
void kps_tap_reset(kps_t *s);

/* Stage 2.  Put a notification frame in the queue of every bound port not in `exclude` (bit p). */
void kps_fanout(kps_t *s, uint8_t exclude, const uint8_t *f, uint16_t n);

/* A frame the controllers never ACK (fn 0x32: song progress, BeatCounter) sent at once, unreliably, to every
 * bound port not in `exclude` (bit p); nothing when the core is off.  Returns the number of ports sent to. */
uint8_t kps_relay(kps_t *s, uint8_t exclude, const uint8_t *f, uint16_t n);

/* File lease: true while a fn 0x24 job runs for a controller that was not the stock binding; owner = its port + 1,
 * holder = the stock binding it gets back (port + 1, 0 none). */
bool kps_file_lease(const kps_t *s, uint8_t *owner, uint8_t *holder);

/* frames waiting in port p's fan-out queue */
uint8_t kps_fan_pending(const kps_t *s, uint8_t port);

/* 32-bit ms from the stock's u16 2 ms counter: call with every new reading. */
typedef struct {
    uint16_t last;
    uint32_t ms;
    bool init;
} kps_clock_t;
uint32_t kps_clock(kps_clock_t *c, uint16_t tick2ms);

#endif
