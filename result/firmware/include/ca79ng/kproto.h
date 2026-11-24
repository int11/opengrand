/* OpenGrand — parameter store and Kawai remote SysEx protocol (piano side).
 *
 * Wire behaviour reproduced from the analysis (both sides agree):
 *   RX  F0 40 cc FN 08 02 v1 v2 part data.. F7     controller -> piano
 *   TX  F0 40 7F FN 04 1E v1 v2 part data.. F7     piano -> controller (device byte 0x7F, as stock)
 *   FN 0x7E system: 01 01 connect, 01 00 disconnect, 05 40/41/42 model/dest/lang,
 *                   04 41 version, 00 00 power off
 *   FN 0x10 parameter set  -> ACK (fn 07 echoing v1 v2 part) unconditionally, like the
 *                             stock (unknown / invalid values are ACKed and ignored);
 *                             packed form 10 5F 00 part <len v1 v2 part data>..
 *   FN 0x31 mode change / 0x32 remote command -> action, no ACK
 *   FN 0x02 request (5F 7F = all, v2 0x00 = whole group, 5F 00 = packed) -> 0x10 values
 *                   one at a time (stop-and-wait on the controller's 07), then 03 v1 v2 7F
 *   FN 0x09 / 0x03 staging: 09 -> the piano asks 02 5F 7F 7F, sets are stored but not
 *                   applied until the controller's 03
 *   FN 0x24 file transfer (see filexfer)
 *   FN 0x60 identity request -> F0 40 7F 61 04 02 F7 (as observed)
 * Stock behaviour kept: only bound ports are answered
 * (CONNECT and fn 0x60 excepted); d0 0x5A and d0 0x50 + d1 {0C,0E,10,17} (VT edit) are
 * judged before the function code and answered 7E 03 00 01 70 outside the edit mode.
 * Unlike the original firmware (one bound controller port, route var 0x413ca3de),
 * several controllers — the built-in panel (home port, stock port 3) and a phone — may be
 * bound at once and are kept in sync: changes go to
 * every bound port but the sender through per-port stop-and-wait queues, exclusive
 * operations (file transfer, recorder, touch-curve edit) belong to one port and others get
 * Result Busy, and a non-home controller leaving sends 7E 01 01 to the panel. */
#ifndef CA79NG_KPROTO_H
#define CA79NG_KPROTO_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "ca79ng/midi.h"
#include "ca79ng/params_gen.h"

/* ---------------- parameter store ---------------- */
/* User EQ (UserEqGain 51 1A 7F, UserEqFreq 51 1B 7F): the first data byte is a band and the stock keeps a
 * value per band under the one address (dump: 51 1A 7F x4 bands 0-3, 51 1B 7F x2 bands 1-2). v / valid keep
 * the last band written, as for every key; band / band_valid keep each band of the System slot. */
#define PARAM_BANDED 2  /* UserEqGain, UserEqFreq */
#define PARAM_BANDS 4
typedef struct {
    uint8_t v[PRM_COUNT][PARAM_SLOT_COUNT][4];
    uint32_t valid[PRM_COUNT]; /* bit per slot */
    uint8_t band[PARAM_BANDED][PARAM_BANDS][4];
    uint8_t band_valid[PARAM_BANDED]; /* bit per band */
} param_store_t;

void param_store_defaults(param_store_t *s);
int param_slot_from_part(uint8_t part);          /* -1 if unknown */
uint8_t param_part_from_slot(int slot);
bool param_get(const param_store_t *s, param_id_t id, int slot, uint8_t out[4]);
uint8_t param_get1(const param_store_t *s, param_id_t id, int slot, uint8_t fallback);
/* returns true if the value changed */
bool param_put(param_store_t *s, param_id_t id, int slot, const uint8_t *data, uint8_t len);
/* banded keys (User EQ, System slot): one value per band - param_banded is -1 for every other (id, slot) */
int param_banded(param_id_t id, int slot);
bool param_get_band(const param_store_t *s, param_id_t id, int slot, uint8_t band, uint8_t out[4]);
int param_find(uint8_t fn, uint8_t v1, uint8_t v2, uint8_t part); /* -1 if unknown */
bool param_validate(param_id_t id, const uint8_t *data, uint8_t len);

/* Range codec used by the controller UI ("min" types): matches the JS encoder. */
uint8_t param_range_encode(param_id_t id, int ui);
int param_range_decode(param_id_t id, uint8_t raw);

/* ---------------- protocol ---------------- */
typedef struct {
    uint8_t group, machine, sub1, sub2;
    uint8_t destination, language;
    const char *version; /* ASCII, e.g. "1.0.0" */
} kp_identity_t;

typedef struct kproto kproto_t;

typedef struct {
    void *ctx;
    void (*tx)(void *ctx, midi_port_t port, const uint8_t *msg, uint16_t len);
    /* parameter accepted (fn 0x10) or action (0x31/0x32); origin port for UI echo */
    void (*param)(void *ctx, param_id_t id, int slot, uint8_t fn, const uint8_t *data, uint8_t len, midi_port_t origin);
    void (*power_off)(void *ctx);
    void (*session)(void *ctx, midi_port_t port, bool connected);
    /* universal SysEx (GM on, master volume / tuning, GS reset) */
    void (*universal)(void *ctx, const uint8_t *msg, uint16_t len);
    /* completed fn 0x24 transfer. kind KP_FILE_ANON (0): upload without a
     * FileWriteRequest; KP_FILE_WRITE (1): data of a FileWriteRequest, the
     * target name is kp->fa.name. */
    void (*file)(void *ctx, midi_port_t port, uint8_t kind, const uint8_t *data, uint32_t len);
    /* ---- optional OpenGrand hooks (all may be NULL) ---- */
    /* Called before a decoded parameter is stored. Returning false rejects it
     * (fn 0x10 is NACKed, 0x31/0x32 ignored). Used by Lock mode. */
    bool (*accept)(void *ctx, param_id_t id, int slot, uint8_t fn, const uint8_t *data, uint8_t len, midi_port_t origin);
    /* A fn 0x10/0x31/0x32 frame without a descriptor (OpenGrand extension
     * parameters, TouchCurveBulk*). Return true if handled; fn 0x10 is ACKed. */
    bool (*ext)(void *ctx, midi_port_t port, uint8_t fn, uint8_t v1, uint8_t v2, uint8_t part,
                const uint8_t *data, uint16_t len);
    /* A request (fn 0x02) the store cannot answer itself (variable-length
     * data). Return true if handled: kproto then ends it with 03 v1 v2 part. */
    bool (*request)(void *ctx, midi_port_t port, uint8_t v1, uint8_t v2, uint8_t part);
    /* fn 0x24 FileAccess operation with complete file name(s). Returns a
     * result code (KP_RES_*); KP_RES_PENDING when the handler already
     * answered (e.g. started kp_file_send for a read). */
    uint16_t (*file_op)(void *ctx, midi_port_t port, uint8_t op, const char *name, const char *name2);
    /* fn 0x24 FileListRequest: name of entry `index`, false past the end. */
    bool (*file_list)(void *ctx, uint32_t index, char *name, size_t cap);
    /* system query 04 4x with an explicit component byte (OpenGrand: 0 main,
     * 1 DSP, 2 BLE module). Writes ASCII / bytes; returns length or -1. */
    int (*info)(void *ctx, uint8_t kind, uint8_t component, uint8_t *out, size_t cap);
    /* Is the exclusive activity `kind` (KP_EX_RECORDER) still running? The owning port
     * keeps it while this returns true; NULL = never exclusive. */
    bool (*excl_active)(void *ctx, uint8_t kind);
} kp_io_t;

#define KP_FILE_MAX (64u * 1024u)
#define KP_NAME_MAX 256u

enum { KP_FILE_ANON = 0, KP_FILE_WRITE = 1 };

/* fn 0x24 v1 request codes (protocol_spec_CA79.json "file_dump_fn_0x24") */
enum {
    KP_FA_TOP = 0x01, KP_FA_DATA = 0x02, KP_FA_NAME = 0x03, KP_FA_READ = 0x04, KP_FA_WRITE = 0x05,
    KP_FA_DELETE = 0x06, KP_FA_RENAME = 0x07, KP_FA_SELECT = 0x08, KP_FA_LOAD = 0x09, KP_FA_SAVE = 0x0A,
    KP_FA_LIST = 0x11, KP_FA_END_LIST = 0x1F, KP_FA_EOD = 0x7B, KP_FA_WAIT = 0x7C,
    KP_FA_CANCEL = 0x7D, KP_FA_NACK = 0x7E, KP_FA_ACK = 0x7F,
};

/* Result codes sent as 7E 03 <v2> <v3> <v4> (data/app/tables/result_codes.json);
 * value = v2<<14 | v3<<7 | v4. */
enum {
    KP_RES_OK = 0x0000, KP_RES_ERROR = 0x0001, KP_RES_NO_MEDIA = 0x0021, KP_RES_MOUNT = 0x0022,
    KP_RES_ACCESS = 0x0031, KP_RES_FULL = 0x0032, KP_RES_DENIED = 0x0033, KP_RES_PARAM = 0x0041,
    KP_RES_PATH = 0x0042, KP_RES_EXISTS = 0x0055, KP_RES_NOT_FOUND = 0x0056,
    KP_RES_SIZE_OVER = 0x00E8, KP_RES_SMF_FORMAT = 0x00E9, KP_RES_MEMORY_OVER = 0x00EA,
    KP_RES_NO_DATA = 0x00EC, KP_RES_MODE = 0x00ED, KP_RES_SAME_NAME = 0x00F8,
    KP_RES_BUSY = 0x4000, KP_RES_PENDING = 0xFFFF,
};

typedef struct {
    uint8_t state; /* 0 idle, 1 receiving */
    uint8_t kind;
    uint32_t expected_len, packets; /* TopOfDataLength is the byte count (stock FUN_4014F96C) */
    uint32_t len;
    uint8_t buf[KP_FILE_MAX];
} kp_file_rx_t;

/* FileAccess (fn 0x24 v1 03..11) state: name reception and the
 * stop-and-wait transmit stream (one packet per controller Ack). */
typedef struct {
    uint8_t pending;              /* request waiting for its name(s), 0 = none */
    uint8_t names_needed;         /* 1, or 2 for rename */
    char name[KP_NAME_MAX];       /* first / only name */
    char name2[KP_NAME_MAX];      /* rename target */
    uint8_t nbuf[KP_NAME_MAX + 8];
    uint16_t nlen;
    bool write_ready;             /* FileWriteRequest name complete: next upload is that file */
    uint8_t d1, d2;               /* request arguments of 05 / 09 / 0A (0xFF = absent): song, file type index */
    /* transmit stream */
    uint8_t tx_kind;              /* 0 none, 1 data, 2 list */
    midi_port_t tx_port;
    const uint8_t *tx_data;
    uint32_t tx_len, tx_pos, tx_counter, tx_index;
    uint8_t tx_name[KP_NAME_MAX + 8];
    uint16_t tx_name_len, tx_name_pos;
    uint8_t last[48];
    uint16_t last_len;
} kp_fa_t;

/* ---- multi-controller session ---- */
#define KP_DEV 0x7F                  /* device byte of every piano frame (stock 0x7F) */
#define KP_HOME_PORT MIDI_PORT_PANEL /* stock port 3: never unbound by ACK time-outs */
#define KP_ACK_TIMEOUT_MS 250u       /* stock FUN_4014BCAC(250) */
#define KP_ACK_TRIES 16u             /* stock FUN_4014BD1C: 16 sends, then unbind */
#define KP_TXQ_LEN 64u
#define KP_TX_FRAME_MAX 272u
#define KP_PACKED_MAX 0xF5u          /* packed body limit (stock FUN_4014EBFC) */
#define KP_EXCL_IDLE_MS 60000u       /* file / touch-curve ownership lapses after this idle time */

enum { KP_EX_FILE = 0, KP_EX_RECORDER, KP_EX_TOUCHCURVE, KP_EX_COUNT };

typedef struct {
    uint8_t kind; /* KP_Q_* */
    uint8_t a, b, c;
} kp_qitem_t;

/* Per-port stop-and-wait transmitter: one fn 0x10 / 0x03 frame in flight, waiting for the
 * controller's 07 (or 06 = resend); a FIFO of values / ends and a request cursor behind it. */
typedef struct {
    uint8_t frame[KP_TX_FRAME_MAX];
    uint16_t len;                 /* in-flight frame length, 0 = idle */
    uint8_t key[3];               /* v1 v2 part the ACK must echo */
    uint8_t tries;
    uint16_t wait_ms;
    kp_qitem_t q[KP_TXQ_LEN];
    uint8_t qhead, qn;
    /* fn 0x02 answer in progress (stock request task: one at a time, a second request
     * while it runs is dropped) */
    uint8_t req_active, req_v1, req_v2, req_part;
    uint16_t req_id;
    uint8_t req_slot;
    uint8_t req_band;             /* banded key (User EQ): next band of req_id / req_slot */
    bool remote_changed;          /* stock 0x413CA3B0[port]: Sound changed from this port */
    uint32_t dropped, failed;
} kp_port_t;

struct kproto {
    param_store_t *store;
    kp_identity_t id;
    kp_io_t io;
    uint8_t dev_id; /* device id byte used in replies (KP_DEV) */
    bool connected[MIDI_PORT_COUNT]; /* bound (synced) controller ports */
    kp_port_t port[MIDI_PORT_COUNT];
    uint8_t excl_owner[KP_EX_COUNT]; /* MIDI_PORT_COUNT = free */
    uint32_t excl_idle_ms[KP_EX_COUNT];
    bool staging;                    /* fn 0x09 .. 0x03: sets stored, not applied */
    uint32_t staged[PRM_COUNT];      /* slot bits awaiting apply */
    uint16_t fa_wait_ms;             /* fn 0x24 transmit stream: ACK wait */
    uint8_t fa_tries;
    kp_file_rx_t file;
    kp_fa_t fa;
    uint32_t rx_frames, rx_errors;
};

void kp_init(kproto_t *kp, param_store_t *store, const kp_identity_t *id, const kp_io_t *io);
/* Returns true if the SysEx was a Kawai/universal message handled here. */
bool kp_handle_sysex(kproto_t *kp, midi_port_t port, const uint8_t *msg, uint16_t len);
/* Local change (panel keys, firmware logic): store + notify all controllers. */
void kp_local_set(kproto_t *kp, param_id_t id, int slot, const uint8_t *data, uint8_t len);
/* Queue one stored value (fn 0x10) to a bound port (stop-and-wait, value read when sent). */
void kp_send_param(kproto_t *kp, midi_port_t port, param_id_t id, int slot);
/* Queue a value to every bound port except `except` (MIDI_PORT_COUNT = all). */
void kp_notify(kproto_t *kp, param_id_t id, int slot, midi_port_t except);
/* Full parameter dump to one port (as the answer to 02 5F 7F 7F): queued, stop-and-wait,
 * ends with 03 5F 7F 7F. */
void kp_dump_all(kproto_t *kp, midi_port_t port);
/* Advance time: ACK time-outs / resends (250 ms x 16, then unbind a non-home port),
 * ownership time-outs. */
void kp_tick(kproto_t *kp, uint32_t elapsed_ms);
/* A controller's link went down (BLE disconnect, Bluetooth MIDI off): unbind it and,
 * like the stock link-down path FUN_4015B494, invite the panel back with 7E 01 01. */
void kp_link_down(kproto_t *kp, midi_port_t port);
/* The piano is about to power off: 7E 00 00 to every bound port except `except`
 * (stock FUN_4014B0A8 sends it to the bound controller). */
void kp_shutdown_notify(kproto_t *kp, midi_port_t except);
/* Nothing queued or in flight towards `port`. */
bool kp_tx_idle(const kproto_t *kp, midi_port_t port);
/* Send an unsolicited remote/status message (e.g. BeatCounter, MusicProgress). */
void kp_send_status(kproto_t *kp, uint8_t fn, uint8_t v1, uint8_t v2, uint8_t part, const uint8_t *data, uint8_t len);

/* Result message 7E 03 v2 v3 v4 to one port (code = KP_RES_*). */
void kp_send_result(kproto_t *kp, midi_port_t port, uint16_t code);
/* Raw piano->controller frame F0 40 dev fn 04 1E body.. F7 (≤ MIDI_SYSEX_TX_MAX = 511 bytes). */
void kp_send_frame(kproto_t *kp, midi_port_t port, uint8_t fn, const uint8_t *body, uint16_t n);
/* Start a fn 0x24 data stream (TopOfDataLength, DataPackets paced by the
 * controller's Acks, EndOfData). `data` must stay valid until it ends. */
void kp_file_send(kproto_t *kp, midi_port_t port, const uint8_t *data, uint32_t len);
/* Expect FileName packets for `op` without a preceding 0x24 request (used by
 * the RenameFile remote parameter: first name already known). */
void kp_file_expect_name(kproto_t *kp, uint8_t op, const char *first);
bool kp_file_busy(const kproto_t *kp);

/* 8-to-7 bit packing used by fn 0x24 (hdr byte carries the MSBs, bit 6-k for byte k). */
size_t kp_pack87(const uint8_t *in, size_t n, uint8_t *out);
size_t kp_unpack78(const uint8_t *in, size_t n, uint8_t *out);
uint8_t kp_xor(const uint8_t *p, size_t n);

#endif
