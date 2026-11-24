/* Tests for the parameter store and the Kawai SysEx protocol handler. */
#include <stdlib.h>

#include "ca79ng/bt.h"
#include "ca79ng/kproto.h"
#include "test.h"

typedef struct {
    int n;
    midi_port_t port[2048];
    uint8_t msg[2048][32];
    uint16_t len[2048];
    int params;
    param_id_t last_id;
    int last_slot;
    uint8_t last_fn;
} sink_t;

static sink_t sink;

static void s_tx(void *c, midi_port_t port, const uint8_t *m, uint16_t n)
{
    (void)c;
    if (sink.n < 2048) {
        sink.port[sink.n] = port;
        memcpy(sink.msg[sink.n], m, n < 32 ? n : 32);
        sink.len[sink.n] = n;
        sink.n++;
    }
}

static void s_param(void *c, param_id_t id, int slot, uint8_t fn, const uint8_t *d, uint8_t l, midi_port_t o)
{
    (void)c; (void)d; (void)l; (void)o;
    sink.params++;
    sink.last_id = id;
    sink.last_slot = slot;
    sink.last_fn = fn;
}

static param_store_t store;
static kproto_t kp;

static void setup(void)
{
    memset(&sink, 0, sizeof sink);
    param_store_defaults(&store);
    kp_identity_t id = {0x04, 0x1E, 0x00, 0x02, 0x00, 0x00, "1.0.0"};
    kp_io_t io = {.tx = s_tx, .param = s_param};
    kp_init(&kp, &store, &id, &io);
}

static void feed(midi_port_t port, const uint8_t *m, uint16_t n) { kp_handle_sysex(&kp, port, m, n); }

TEST(test_store_defaults_from_initial_data)
{
    setup();
    uint8_t v[4];
    CHECK(param_get(&store, PRM_SOUND, SLOT_MAIN1, v));
    CHECK_EQ(v[0], 95); /* SK-EX Rendering: MSB 95 LSB 112 PC 0 */
    CHECK_EQ(v[1], 112);
    CHECK_EQ(v[2], 0);
    CHECK_EQ(param_get1(&store, PRM_REVERB_TYPE, SLOT_MAIN1, 0xFF), 2); /* Small Hall */
    CHECK_EQ(param_get1(&store, PRM_RESONANCE_DEPTH, SLOT_MAIN1, 0), 75);
}

TEST(test_range_codec_matches_js)
{
    /* ResonanceDepth 1..10 <-> 12,24,37,50,63,75,88,101,114,127 */
    static const uint8_t exp[10] = {12, 24, 37, 50, 63, 75, 88, 101, 114, 127};
    for (int ui = 1; ui <= 10; ui++) {
        CHECK_EQ(param_range_encode(PRM_RESONANCE_DEPTH, ui), exp[ui - 1]);
        CHECK_EQ(param_range_decode(PRM_RESONANCE_DEPTH, exp[ui - 1]), ui);
    }
}

static const uint8_t SYNC[] = {0xF0, 0x40, 0x7F, 0x7E, 0x08, 0x02, 0x01, 0x01, 0xF7};

static void bind(midi_port_t port) { feed(port, SYNC, sizeof SYNC); }

/* controller -> piano frame F0 40 7F fn 08 02 body.. F7 */
static void cx(midi_port_t port, uint8_t fn, const uint8_t *body, int n)
{
    uint8_t m[300] = {0xF0, 0x40, 0x7F, fn, 0x08, 0x02};
    memcpy(m + 6, body, (size_t)n);
    m[6 + n] = 0xF7;
    feed(port, m, (uint16_t)(7 + n));
}
#define CX(port, fn, ...) do { const uint8_t _b[] = {__VA_ARGS__}; cx(port, fn, _b, (int)sizeof _b); } while (0)

/* is frame i = F0 40 7F fn 04 1E body.. F7 to port? */
static bool is_frame(int i, midi_port_t port, uint8_t fn, const uint8_t *body, int n)
{
    if (i < 0 || i >= sink.n || sink.port[i] != port || sink.len[i] != 7 + n)
        return false;
    const uint8_t *m = sink.msg[i];
    return m[0] == 0xF0 && m[1] == 0x40 && m[2] == 0x7F && m[3] == fn && m[4] == 0x04 && m[5] == 0x1E &&
           memcmp(m + 6, body, (size_t)n) == 0 && m[6 + n] == 0xF7;
}
#define IS(i, port, fn, ...) ({ const uint8_t _b[] = {__VA_ARGS__}; is_frame(i, port, fn, _b, (int)sizeof _b); })

/* ACK the frame the piano sent last to port (07 v1 v2 part) */
static void ack_last(midi_port_t port)
{
    for (int i = sink.n - 1; i >= 0; i--)
        if (sink.port[i] == port) {
            CX(port, 0x07, sink.msg[i][6], sink.msg[i][7], sink.msg[i][8]);
            return;
        }
}

TEST(test_connect_identity_version)
{
    setup();
    bind(MIDI_PORT_PANEL);
    CHECK(kp.connected[MIDI_PORT_PANEL]);
    CHECK_EQ(sink.n, 1);
    /* device byte 0x7F and b = 0x40 like the stock (0x4014A7B0) */
    CHECK(IS(0, MIDI_PORT_PANEL, 0x7E, 0x02, 0x00, 0x40));
    CX(MIDI_PORT_PANEL, 0x7E, 0x05, 0x40);
    CHECK(IS(1, MIDI_PORT_PANEL, 0x7E, 0x05, 0x00, 0x00, 0x02));
    CX(MIDI_PORT_PANEL, 0x7E, 0x05, 0x41); /* destination: two bytes, as the stock */
    CHECK(IS(2, MIDI_PORT_PANEL, 0x7E, 0x05, 0x01, 0x00, 0x00));
    CX(MIDI_PORT_PANEL, 0x7E, 0x04, 0x41);
    CHECK_EQ(sink.msg[3][6], 0x04);
    CHECK_EQ(sink.msg[3][7], 0x01);
    CHECK_MEM(sink.msg[3] + 8, "1.0.0", 5);
    /* the identity request fn 0x60 is answered on any port, bound or not */
    sink.n = 0;
    const uint8_t idr[] = {0xF0, 0x40, 0x7F, 0x60, 0x08, 0x02, 0xF7};
    feed(MIDI_PORT_USB, idr, sizeof idr);
    const uint8_t idw[] = {0xF0, 0x40, 0x7F, 0x61, 0x04, 0x02, 0xF7};
    CHECK_EQ(sink.n, 1);
    CHECK_MEM(sink.msg[0], idw, sizeof idw);
}

TEST(test_unbound_port_is_silent)
{
    setup();
    /* nothing but CONNECT (and fn 0x60) is answered on a port that is not bound */
    CX(MIDI_PORT_USB, 0x10, 0x55, 0x08, 0x00, 0x05);
    CX(MIDI_PORT_USB, 0x02, 0x55, 0x08, 0x00);
    CX(MIDI_PORT_USB, 0x7E, 0x05, 0x40);
    CX(MIDI_PORT_USB, 0x7E, 0x01, 0x00);
    CX(MIDI_PORT_USB, 0x24, 0x11);
    CHECK_EQ(sink.n, 0);
    CHECK_EQ(sink.params, 0);
    CHECK_EQ(param_get1(&store, PRM_REVERB_TYPE, SLOT_MAIN1, 0), 2);
}

TEST(test_param_set_ack_and_notify_other_controller)
{
    setup();
    bind(MIDI_PORT_PANEL);
    bind(MIDI_PORT_BLE);
    sink.n = 0;
    /* phone (BLE) sets ReverbType = Cathedral on Main1 */
    CX(MIDI_PORT_BLE, 0x10, 0x55, 0x08, 0x00, 0x05);
    CHECK_EQ(param_get1(&store, PRM_REVERB_TYPE, SLOT_MAIN1, 0), 5);
    CHECK_EQ(sink.params, 1);
    CHECK_EQ(sink.last_id, PRM_REVERB_TYPE);
    CHECK_EQ(sink.n, 2);
    CHECK(IS(0, MIDI_PORT_BLE, 0x07, 0x55, 0x08, 0x00));         /* ACK to the phone */
    CHECK(IS(1, MIDI_PORT_PANEL, 0x10, 0x55, 0x08, 0x00, 0x05)); /* the panel is told */

    /* same value again: ACK but no notification */
    sink.n = 0;
    CX(MIDI_PORT_BLE, 0x10, 0x55, 0x08, 0x00, 0x05);
    CHECK_EQ(sink.n, 1);

    /* the panel has not ACKed yet: the next change waits in its queue, value read when sent */
    sink.n = 0;
    CX(MIDI_PORT_BLE, 0x10, 0x55, 0x08, 0x00, 0x03);
    CX(MIDI_PORT_BLE, 0x10, 0x55, 0x08, 0x00, 0x04);
    CHECK_EQ(sink.n, 2); /* two ACKs to the phone only */
    CHECK(!kp_tx_idle(&kp, MIDI_PORT_PANEL));
    CX(MIDI_PORT_PANEL, 0x07, 0x55, 0x08, 0x00); /* panel ACKs the first notification */
    CHECK_EQ(sink.n, 3);
    CHECK(IS(2, MIDI_PORT_PANEL, 0x10, 0x55, 0x08, 0x00, 0x04)); /* one copy, newest value */
    ack_last(MIDI_PORT_PANEL);
    CHECK(kp_tx_idle(&kp, MIDI_PORT_PANEL));

    /* the panel changes it: the phone is told */
    sink.n = 0;
    CX(MIDI_PORT_PANEL, 0x10, 0x55, 0x08, 0x00, 0x01);
    CHECK(IS(0, MIDI_PORT_PANEL, 0x07, 0x55, 0x08, 0x00));
    CHECK(IS(1, MIDI_PORT_BLE, 0x10, 0x55, 0x08, 0x00, 0x01));
}

TEST(test_param_ack_like_stock)
{
    setup();
    bind(MIDI_PORT_USB);
    sink.n = 0;
    /* out of range (MIDIChannel 0..15): ACKed like the stock, not applied */
    const param_desc_t *d = &PARAM_DESC[PRM_MIDICHANNEL];
    CHECK_EQ(d->v1, 0x58);
    CX(MIDI_PORT_USB, 0x10, 0x58, 0x00, 0x7F, 0x7F);
    CHECK(IS(0, MIDI_PORT_USB, 0x07, 0x58, 0x00, 0x7F));
    if (d->data_max[0] < 0x7F)
        CHECK_EQ(sink.params, 0);
    sink.params = 0;
    /* unknown parameter: ACK (stock FUN_4014BE98 ACKs before the lookup), ignored */
    CX(MIDI_PORT_USB, 0x10, 0x5E, 0x5E, 0x00, 0x01);
    CHECK(IS(1, MIDI_PORT_USB, 0x07, 0x5E, 0x5E, 0x00));
    CHECK_EQ(sink.params, 0);
    /* frames not from a controller (wrong 08 02) are ignored */
    sink.n = 0;
    const uint8_t foreign[] = {0xF0, 0x40, 0x7F, 0x10, 0x04, 0x1E, 0x55, 0x08, 0x00, 0x01, 0xF7};
    feed(MIDI_PORT_USB, foreign, sizeof foreign);
    CHECK_EQ(sink.n, 0);
    /* packed set 10 5F 00 7F <len v1 v2 part data>..: one ACK, each bundle applied */
    CX(MIDI_PORT_USB, 0x10, 0x5F, 0x00, 0x7F, 0x05, 0x55, 0x08, 0x00, 0x04, 0x05, 0x51, 0x00, 0x7F, 0x02);
    CHECK_EQ(sink.n, 1);
    CHECK(IS(0, MIDI_PORT_USB, 0x07, 0x5F, 0x00, 0x7F));
    CHECK_EQ(param_get1(&store, PRM_REVERB_TYPE, SLOT_MAIN1, 0), 4);
    CHECK_EQ(param_get1(&store, PRM_AUTO_POWER_OFF, SLOT_SYSTEM, 0), 2);
    CHECK_EQ(sink.params, 2);
}

TEST(test_remote_command_no_ack)
{
    setup();
    bind(MIDI_PORT_PANEL);
    sink.n = 0;
    /* Metronome run: fn 0x32 56 08 part 7F data 01 */
    CX(MIDI_PORT_PANEL, 0x32, 0x56, 0x08, 0x7F, 0x01);
    CHECK_EQ(sink.params, 1);
    CHECK_EQ(sink.last_fn, 0x32);
    CHECK_EQ(sink.n, 0);
}

TEST(test_dump_all)
{
    setup();
    bind(MIDI_PORT_PANEL);
    sink.n = 0;
    CX(MIDI_PORT_PANEL, 0x02, 0x5F, 0x7F, 0x7F);
    CHECK_EQ(sink.n, 1); /* stop-and-wait: one value until it is ACKed */
    CHECK_EQ(sink.msg[0][3], 0x10);
    /* a NAK resends the same frame */
    CX(MIDI_PORT_PANEL, 0x06, sink.msg[0][6], sink.msg[0][7], sink.msg[0][8]);
    CHECK_EQ(sink.n, 2);
    CHECK_MEM(sink.msg[1], sink.msg[0], 11);
    /* a second request while the first runs is dropped (stock request task busy) */
    CX(MIDI_PORT_PANEL, 0x02, 0x51, 0x03, 0x7F);
    CHECK_EQ(sink.n, 2);
    for (int guard = 0; guard < 1000 && sink.msg[sink.n - 1][3] != 0x03; guard++)
        ack_last(MIDI_PORT_PANEL);
    CHECK(sink.n > 100);
    CHECK(IS(sink.n - 1, MIDI_PORT_PANEL, 0x03, 0x5F, 0x7F, 0x7F));
    for (int i = 0; i < sink.n; i++)
        CHECK(sink.len[i] <= MIDI_SYSEX_TX_MAX);
    int n = sink.n;
    ack_last(MIDI_PORT_PANEL);
    CHECK_EQ(sink.n, n);
    CHECK(kp_tx_idle(&kp, MIDI_PORT_PANEL));
}

TEST(test_request_forms)
{
    setup();
    bind(MIDI_PORT_USB);
    sink.n = 0;
    /* single value: the end echoes the key with part 7F (stock FUN_4014BDC8) */
    CX(MIDI_PORT_USB, 0x02, 0x55, 0x08, 0x00);
    CHECK(IS(0, MIDI_PORT_USB, 0x10, 0x55, 0x08, 0x00, 0x02));
    ack_last(MIDI_PORT_USB);
    CHECK(IS(1, MIDI_PORT_USB, 0x03, 0x55, 0x08, 0x7F));
    ack_last(MIDI_PORT_USB);
    /* v2 0x00 = the whole group at that part (stock FUN_4015205C) */
    sink.n = 0;
    CX(MIDI_PORT_USB, 0x02, 0x51, 0x00, 0x7F);
    for (int guard = 0; guard < 200 && sink.msg[sink.n - 1][3] != 0x03; guard++)
        ack_last(MIDI_PORT_USB);
    CHECK(IS(sink.n - 1, MIDI_PORT_USB, 0x03, 0x51, 0x00, 0x7F));
    CHECK(sink.n > 10);
    for (int i = 0; i < sink.n - 1; i++)
        CHECK(sink.msg[i][3] == 0x10 && sink.msg[i][6] == 0x51 && sink.msg[i][8] == 0x7F);
    CHECK(IS(0, MIDI_PORT_USB, 0x10, 0x51, 0x03, 0x7F, 0x40)); /* Tuning first, as the stock */
    ack_last(MIDI_PORT_USB);
    /* packed answer 02 5F 00: one 10 5F 00 7F <len v1 v2 part data>.. frame, then 03 5F 00 7F */
    sink.n = 0;
    CX(MIDI_PORT_USB, 0x02, 0x5F, 0x00, 0x7F);
    CHECK_EQ(sink.n, 1);
    CHECK_EQ(sink.msg[0][3], 0x10);
    CHECK_MEM(sink.msg[0] + 6, "\x5F\x00\x7F", 3);
    CHECK(sink.msg[0][9] >= 5 && sink.msg[0][12] == 0x00); /* <len v1 v2 part=00 ..> */
    CHECK(sink.len[0] <= 10 + KP_PACKED_MAX);
    ack_last(MIDI_PORT_USB);
    CHECK(IS(1, MIDI_PORT_USB, 0x03, 0x5F, 0x00, 0x7F));
}

TEST(test_ack_timeout_unbinds_but_not_the_panel)
{
    setup();
    bind(MIDI_PORT_PANEL);
    bind(MIDI_PORT_USB);
    sink.n = 0;
    CX(MIDI_PORT_USB, 0x02, 0x55, 0x08, 0x00);
    CHECK_EQ(sink.n, 1);
    for (int i = 0; i < 15; i++)
        kp_tick(&kp, KP_ACK_TIMEOUT_MS);
    CHECK_EQ(sink.n, 16); /* 16 sends of the same value */
    CHECK(kp.connected[MIDI_PORT_USB]);
    kp_tick(&kp, KP_ACK_TIMEOUT_MS);
    CHECK(!kp.connected[MIDI_PORT_USB]); /* given up: unbound, the panel is invited back */
    CHECK(IS(16, MIDI_PORT_PANEL, 0x7E, 0x01, 0x01));
    /* the panel (home port) loses the frame but stays bound */
    sink.n = 0;
    CX(MIDI_PORT_PANEL, 0x02, 0x55, 0x08, 0x00);
    for (int i = 0; i < 16; i++)
        kp_tick(&kp, KP_ACK_TIMEOUT_MS);
    CHECK(kp.connected[MIDI_PORT_PANEL]);
    CHECK(IS(sink.n - 1, MIDI_PORT_PANEL, 0x03, 0x55, 0x08, 0x7F)); /* went on with the end */
}

TEST(test_disconnect_and_link_down_invite_the_panel)
{
    setup();
    bind(MIDI_PORT_PANEL);
    bind(MIDI_PORT_BLE);
    bind(MIDI_PORT_USB);
    sink.n = 0;
    CX(MIDI_PORT_USB, 0x7E, 0x01, 0x00);
    CHECK(!kp.connected[MIDI_PORT_USB]);
    CHECK(kp.connected[MIDI_PORT_PANEL]); /* another CONNECT / DISCONNECT never displaces the panel */
    CHECK_EQ(sink.n, 1);
    CHECK(IS(0, MIDI_PORT_PANEL, 0x7E, 0x01, 0x01));
    kp_link_down(&kp, MIDI_PORT_BLE);
    CHECK(!kp.connected[MIDI_PORT_BLE]);
    CHECK(IS(1, MIDI_PORT_PANEL, 0x7E, 0x01, 0x01));
    kp_link_down(&kp, MIDI_PORT_BLE); /* already gone: nothing */
    CHECK_EQ(sink.n, 2);
    /* power off: 7E 00 00 to every bound port */
    bind(MIDI_PORT_USB);
    sink.n = 0;
    kp_shutdown_notify(&kp, MIDI_PORT_COUNT);
    CHECK_EQ(sink.n, 2);
    CHECK(IS(0, MIDI_PORT_USB, 0x7E, 0x00, 0x00) || IS(1, MIDI_PORT_USB, 0x7E, 0x00, 0x00));
    /* a port that changed the Sound reconnects with b = 0x00 */
    CX(MIDI_PORT_USB, 0x10, 0x55, 0x00, 0x00, 121, 0, 17);
    sink.n = 0;
    bind(MIDI_PORT_USB);
    CHECK(IS(0, MIDI_PORT_USB, 0x7E, 0x02, 0x00, 0x00));
}

TEST(test_vt_edit_interception)
{
    setup();
    bind(MIDI_PORT_USB);
    sink.n = 0;
    /* touch curve not on User: any fn with d0 5A gets 7E 03 00 01 70 (stock FUN_4014DC0C) */
    CX(MIDI_PORT_USB, 0x10, 0x5A, 0x08, 0x7F);
    CX(MIDI_PORT_USB, 0x02, 0x5A, 0x7F, 0x7F);
    CX(MIDI_PORT_USB, 0x10, 0x50, 0x17, 0x7F, 0x7F, 0x7F, 0x40); /* per-note voicing, Voicing not User */
    CHECK_EQ(sink.n, 3);
    for (int i = 0; i < 3; i++)
        CHECK(IS(i, MIDI_PORT_USB, 0x7E, 0x03, 0x00, 0x01, 0x70));
    CHECK_EQ(sink.params, 0);
    /* 50 xx outside 0C/0E/10/17 is an ordinary parameter */
    sink.n = 0;
    CX(MIDI_PORT_USB, 0x10, 0x50, 0x00, 0x00, 0x40); /* TouchCurve = User */
    CHECK(IS(0, MIDI_PORT_USB, 0x07, 0x50, 0x00, 0x00));
    CX(MIDI_PORT_USB, 0x10, 0x5A, 0x08, 0x7F); /* now in the edit mode: an ordinary set (ACK) */
    CHECK(IS(1, MIDI_PORT_USB, 0x07, 0x5A, 0x08, 0x7F));
}

TEST(test_staging_fn09_fn03)
{
    setup();
    bind(MIDI_PORT_PANEL);
    sink.n = 0;
    CX(MIDI_PORT_PANEL, 0x09, 0x5F, 0x7F, 0x7F);
    CHECK(IS(0, MIDI_PORT_PANEL, 0x02, 0x5F, 0x7F, 0x7F)); /* the piano asks for the controller's state */
    CX(MIDI_PORT_PANEL, 0x10, 0x55, 0x08, 0x00, 0x05);
    CHECK(IS(1, MIDI_PORT_PANEL, 0x07, 0x55, 0x08, 0x00));
    CHECK_EQ(param_get1(&store, PRM_REVERB_TYPE, SLOT_MAIN1, 0), 5); /* stored ... */
    CHECK_EQ(sink.params, 0);                                         /* ... not applied */
    CX(MIDI_PORT_PANEL, 0x03, 0x5F, 0x7F, 0x7F);
    CHECK_EQ(sink.params, 1); /* applied at the controller's end */
    CHECK_EQ(sink.last_id, PRM_REVERB_TYPE);
    CHECK_EQ(sink.n, 2);      /* fn 0x03 itself is not answered (stock) */
}

static bool rec_busy;
static int find_result(midi_port_t port)
{
    for (int i = 0; i < sink.n; i++)
        if (sink.port[i] == port && sink.msg[i][3] == 0x7E && sink.msg[i][6] == 0x03)
            return i;
    return -1;
}
static bool s_excl(void *c, uint8_t kind) { (void)c; return kind == KP_EX_RECORDER && rec_busy; }

TEST(test_exclusive_operations)
{
    setup();
    kp.io.excl_active = s_excl;
    bind(MIDI_PORT_PANEL);
    bind(MIDI_PORT_BLE);
    sink.n = 0;
    /* file: the phone starts a read (waits for the name); the panel's list is Busy */
    CX(MIDI_PORT_BLE, 0x24, 0x04);
    CHECK(IS(0, MIDI_PORT_BLE, 0x24, 0x7F));
    CX(MIDI_PORT_PANEL, 0x24, 0x11);
    CHECK(IS(1, MIDI_PORT_PANEL, 0x24, 0x7F));
    CHECK(IS(2, MIDI_PORT_PANEL, 0x7E, 0x03, 0x01, 0x00, 0x00));
    CX(MIDI_PORT_PANEL, 0x24, 0x7F); /* somebody else's ACK: ignored */
    CHECK_EQ(sink.n, 3);
    CX(MIDI_PORT_BLE, 0x24, 0x7D); /* the owner cancels: free again */
    sink.n = 0;
    CX(MIDI_PORT_PANEL, 0x24, 0x04);
    CHECK_EQ(sink.n, 1);
    CHECK(IS(0, MIDI_PORT_PANEL, 0x24, 0x7F));
    CX(MIDI_PORT_PANEL, 0x24, 0x7D);

    /* touch-curve edit: the panel edits, the phone's 5A set is ACKed but Busy */
    CX(MIDI_PORT_PANEL, 0x10, 0x50, 0x00, 0x00, 0x40);
    ack_last(MIDI_PORT_BLE); /* the phone ACKs the notification */
    sink.n = 0;
    CX(MIDI_PORT_PANEL, 0x10, 0x5A, 0x08, 0x7F); /* 5A 08 clear: ACK, the phone is told */
    CHECK(IS(0, MIDI_PORT_PANEL, 0x07, 0x5A, 0x08, 0x7F));
    CHECK(IS(1, MIDI_PORT_BLE, 0x10, 0x5A, 0x08, 0x7F));
    ack_last(MIDI_PORT_BLE);
    CX(MIDI_PORT_BLE, 0x10, 0x5A, 0x08, 0x7F);
    CHECK(IS(2, MIDI_PORT_BLE, 0x07, 0x5A, 0x08, 0x7F));
    CHECK(IS(3, MIDI_PORT_BLE, 0x7E, 0x03, 0x01, 0x00, 0x00));
    CHECK_EQ(sink.n, 4);
    CX(MIDI_PORT_PANEL, 0x31, 0x5A, 0x09, 0x7F); /* EditExit releases it */
    sink.n = 0;
    CX(MIDI_PORT_BLE, 0x10, 0x5A, 0x08, 0x7F);
    CHECK(IS(0, MIDI_PORT_BLE, 0x07, 0x5A, 0x08, 0x7F));
    CHECK_EQ(find_result(MIDI_PORT_BLE), -1);
    /* an idle owner (now the phone) loses it after a minute */
    kp_tick(&kp, KP_EXCL_IDLE_MS);
    sink.n = 0;
    CX(MIDI_PORT_PANEL, 0x10, 0x5A, 0x08, 0x7F);
    CHECK_EQ(find_result(MIDI_PORT_PANEL), -1);

    /* recorder: owned while the recorder runs */
    sink.n = 0;
    CX(MIDI_PORT_BLE, 0x32, 0x52, 0x08, 0x7F, 0x01);
    rec_busy = true;
    CX(MIDI_PORT_PANEL, 0x32, 0x52, 0x08, 0x7F, 0x02);
    CHECK(IS(0, MIDI_PORT_PANEL, 0x7E, 0x03, 0x01, 0x00, 0x00));
    CX(MIDI_PORT_BLE, 0x32, 0x52, 0x08, 0x7F, 0x02); /* the owner may */
    CHECK_EQ(sink.n, 1);
    rec_busy = false;
    CX(MIDI_PORT_PANEL, 0x32, 0x52, 0x08, 0x7F, 0x01);
    CHECK_EQ(sink.n, 1);
    /* the owner leaving frees everything */
    rec_busy = true;
    kp_link_down(&kp, MIDI_PORT_PANEL);
    bind(MIDI_PORT_PANEL);
    sink.n = 0;
    CX(MIDI_PORT_BLE, 0x32, 0x52, 0x08, 0x7F, 0x02);
    CHECK_EQ(sink.n, 0);
    rec_busy = false;
}

TEST(test_pack87_roundtrip_and_file_transfer)
{
    uint8_t in[20], packed[32], out[32];
    for (int i = 0; i < 20; i++)
        in[i] = (uint8_t)(i * 37 + 0x80 * (i & 1));
    size_t pn = kp_pack87(in, 20, packed);
    CHECK_EQ(pn, 20 + 3);
    for (size_t i = 0; i < pn; i++)
        CHECK(packed[i] < 0x80);
    CHECK_EQ(kp_unpack78(packed, pn, out), 20);
    CHECK_MEM(in, out, 20);

    setup();
    static uint8_t got[64];
    static uint32_t got_len;
    got_len = 0;
    void file_cb(void *c, midi_port_t p, uint8_t k, const uint8_t *d, uint32_t l);
    kp.io.file = file_cb;
    bind(MIDI_PORT_USB);
    sink.n = 0;
    const uint8_t start[] = {0xF0, 0x40, 0x00, 0x24, 0x08, 0x02, 0x01, 0, 0, 0, 12, 0xF7}; /* 12 bytes */
    feed(MIDI_PORT_USB, start, sizeof start);
    CHECK_EQ(sink.msg[0][6], 0x7F);
    /* one data packet carrying "Hello, CA79!" */
    const char *txt = "Hello, CA79!";
    uint8_t pk[64] = {0xF0, 0x40, 0x00, 0x24, 0x08, 0x02, 0x02, 0x00};
    size_t g = kp_pack87((const uint8_t *)txt, 12, pk + 8);
    size_t n = 8 + g;
    pk[n] = kp_xor(pk + 1, n - 1);
    pk[n + 1] = 0xF7;
    feed(MIDI_PORT_USB, pk, (uint16_t)(n + 2));
    CHECK_EQ(sink.msg[1][6], 0x7F);
    const uint8_t end[] = {0xF0, 0x40, 0x00, 0x24, 0x08, 0x02, 0x7B, 0xF7};
    feed(MIDI_PORT_USB, end, sizeof end);
    CHECK_EQ(sink.msg[2][6], 0x7F);
    extern uint8_t g_file_buf[64];
    extern uint32_t g_file_len;
    CHECK_EQ(g_file_len, 12);
    CHECK_MEM(g_file_buf, txt, 12);
    (void)got; (void)got_len;

    /* corrupted checksum -> NAK */
    feed(MIDI_PORT_USB, start, sizeof start);
    pk[n] ^= 1;
    feed(MIDI_PORT_USB, pk, (uint16_t)(n + 2));
    CHECK_EQ(sink.msg[sink.n - 1][6], 0x7E);
}

uint8_t g_file_buf[64];
uint32_t g_file_len;
void file_cb(void *c, midi_port_t p, uint8_t k, const uint8_t *d, uint32_t l)
{
    (void)c; (void)p; (void)k;
    g_file_len = l;
    memcpy(g_file_buf, d, l < 64 ? l : 64);
}

static uint8_t ble_out[8][32];
static int ble_n;
static void ble_tx(void *c, const uint8_t *b, uint16_t n)
{
    (void)c;
    if (ble_n < 8) memcpy(ble_out[ble_n++], b, n < 32 ? n : 32);
}

TEST(test_blemidi_module_commands)
{
    ble_n = 0;
    blemidi_t b;
    blemidi_io_t io = {NULL, ble_tx};
    blemidi_init(&b, &io, "OpenGrand");
    /* timestamp off first (module bug workaround), then the name */
    const uint8_t ts[] = {0xF0, 0x00, 0x02, 0x08, 0x10, 0x55, 0x00, 0x02, 0x00, 0x00, 0xF7};
    CHECK_MEM(ble_out[0], ts, sizeof ts);
    const uint8_t nm[] = {0xF0, 0x00, 0x02, 0x08, 0x10, 0x55, 0x00, 0x01, 'O', 'p', 'e', 'n'};
    CHECK_MEM(ble_out[1], nm, sizeof nm);
    CHECK_EQ(ble_out[1][8 + 16], 0xF7);
    blemidi_enter_dfu(&b);
    const uint8_t dfu[] = {0xF0, 0x00, 0x02, 0x08, 0x10, 0x55, 0x7F, 0x00, 0x01, 0xF7};
    CHECK_MEM(ble_out[2], dfu, sizeof dfu);
    const uint8_t reply[] = {0xF0, 0x00, 0x02, 0x08, 0x10, 0x55, 0x20, 0x04, '2', '.', '1', 0xF7};
    CHECK(blemidi_handle_sysex(&b, reply, sizeof reply));
    CHECK(strcmp(b.fw_version, "2.1") == 0);
}

/* User EQ: the first data byte is a band, one value per band under one address (stock dump 51 1A 7F x4
 * bands 0-3, 51 1B 7F x2 bands 1-2, captures 261031) */
TEST(test_user_eq_bands)
{
    setup();
    uint8_t v[4];
    CHECK(param_get_band(&store, PRM_USER_EQ_GAIN, SLOT_SYSTEM, 0, v) && v[0] == 0 && v[1] == 0x40);
    CHECK(param_get_band(&store, PRM_USER_EQ_FREQ, SLOT_SYSTEM, 1, v) && v[1] == 0x3D && v[2] == 0x56);
    CHECK(!param_get_band(&store, PRM_USER_EQ_FREQ, SLOT_SYSTEM, 0, v)); /* only the mid bands have a frequency */
    CHECK(!param_get_band(&store, PRM_USER_EQ_GAIN, SLOT_MAIN1, 0, v));
    CHECK(!param_get_band(&store, PRM_REVERB_TYPE, SLOT_SYSTEM, 0, v));
    CHECK(param_get(&store, PRM_USER_EQ_GAIN, SLOT_SYSTEM, v) && v[0] == 3); /* param_get: the last band, as before */
    bind(MIDI_PORT_PANEL);
    bind(MIDI_PORT_BLE);
    /* the phone sets bands 0 and 2 while the panel has not ACKed: each band reaches the panel */
    sink.n = 0;
    CX(MIDI_PORT_BLE, 0x10, 0x51, 0x1A, 0x7F, 0x00, 0x46, 0x00);
    CX(MIDI_PORT_BLE, 0x10, 0x51, 0x1A, 0x7F, 0x02, 0x3A, 0x00);
    CX(MIDI_PORT_BLE, 0x10, 0x51, 0x1A, 0x7F, 0x00, 0x47, 0x00); /* band 0 again: one copy, newest value */
    CHECK(IS(1, MIDI_PORT_PANEL, 0x10, 0x51, 0x1A, 0x7F, 0x00, 0x46, 0x00));
    ack_last(MIDI_PORT_PANEL);
    CHECK(IS(sink.n - 1, MIDI_PORT_PANEL, 0x10, 0x51, 0x1A, 0x7F, 0x02, 0x3A, 0x00));
    ack_last(MIDI_PORT_PANEL);
    CHECK(IS(sink.n - 1, MIDI_PORT_PANEL, 0x10, 0x51, 0x1A, 0x7F, 0x00, 0x47, 0x00));
    ack_last(MIDI_PORT_PANEL);
    CHECK(kp_tx_idle(&kp, MIDI_PORT_PANEL));
    /* the same band value again: not a change (another band in between does not make it one) */
    int n = sink.n;
    CX(MIDI_PORT_BLE, 0x10, 0x51, 0x1A, 0x7F, 0x02, 0x3A, 0x00);
    CHECK_EQ(sink.n, n + 1); /* the ACK only */
    CHECK(kp_tx_idle(&kp, MIDI_PORT_PANEL));
    /* a get answers every band, lowest first, then the end */
    sink.n = 0;
    CX(MIDI_PORT_BLE, 0x02, 0x51, 0x1A, 0x7F);
    for (int guard = 0; guard < 20 && sink.msg[sink.n - 1][3] != 0x03; guard++)
        ack_last(MIDI_PORT_BLE);
    CHECK_EQ(sink.n, 5);
    CHECK(IS(0, MIDI_PORT_BLE, 0x10, 0x51, 0x1A, 0x7F, 0x00, 0x47, 0x00));
    CHECK(IS(1, MIDI_PORT_BLE, 0x10, 0x51, 0x1A, 0x7F, 0x01, 0x40, 0x00));
    CHECK(IS(2, MIDI_PORT_BLE, 0x10, 0x51, 0x1A, 0x7F, 0x02, 0x3A, 0x00));
    CHECK(IS(3, MIDI_PORT_BLE, 0x10, 0x51, 0x1A, 0x7F, 0x03, 0x40, 0x00));
    CHECK(IS(4, MIDI_PORT_BLE, 0x03, 0x51, 0x1A, 0x7F));
    ack_last(MIDI_PORT_BLE);
    /* the full dump: 51 1A 7F x4 (bands 0-3), 51 1B 7F x2 (bands 1-2) */
    sink.n = 0;
    CX(MIDI_PORT_BLE, 0x02, 0x5F, 0x7F, 0x7F);
    for (int guard = 0; guard < 1000 && sink.msg[sink.n - 1][3] != 0x03; guard++)
        ack_last(MIDI_PORT_BLE);
    int gain = 0, freq = 0;
    uint8_t gain_bands = 0, freq_bands = 0;
    for (int i = 0; i < sink.n; i++) {
        const uint8_t *m = sink.msg[i];
        if (m[3] != 0x10 || m[6] != 0x51 || m[8] != 0x7F)
            continue;
        if (m[7] == 0x1A) { gain++; gain_bands |= (uint8_t)(1u << m[9]); }
        if (m[7] == 0x1B) { freq++; freq_bands |= (uint8_t)(1u << m[9]); }
    }
    CHECK_EQ(gain, 4);
    CHECK_EQ(gain_bands, 0x0F);
    CHECK_EQ(freq, 2);
    CHECK_EQ(freq_bands, 0x06);
    /* a band byte past the four bands: kept as the key's value only, no band */
    CHECK(param_put(&store, PRM_USER_EQ_GAIN, SLOT_SYSTEM, (const uint8_t[]){0x05, 0x40, 0x00}, 3));
    CHECK(param_get(&store, PRM_USER_EQ_GAIN, SLOT_SYSTEM, v) && v[0] == 5);
    CHECK_EQ(store.band_valid[0], 0x0F);
}

void suite_kproto(void)
{
    puts("kproto:");
    RUN(test_blemidi_module_commands);
    RUN(test_store_defaults_from_initial_data);
    RUN(test_range_codec_matches_js);
    RUN(test_connect_identity_version);
    RUN(test_unbound_port_is_silent);
    RUN(test_param_set_ack_and_notify_other_controller);
    RUN(test_param_ack_like_stock);
    RUN(test_remote_command_no_ack);
    RUN(test_dump_all);
    RUN(test_request_forms);
    RUN(test_ack_timeout_unbinds_but_not_the_panel);
    RUN(test_disconnect_and_link_down_invite_the_panel);
    RUN(test_vt_edit_interception);
    RUN(test_staging_fn09_fn03);
    RUN(test_exclusive_operations);
    RUN(test_pack87_roundtrip_and_file_transfer);
    RUN(test_user_eq_bands);
}
