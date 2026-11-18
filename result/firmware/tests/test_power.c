/* BT audio activity model, the APO hold rule (whole instrument) and the partial
 * standby state machine over the hal.h host stubs. */
#include "ca79ng/piano.h"
#include "ca79ng/power.h"
#include "ca79ng/standby.h"
#include "hal.h"
#include "hal_power_sim.h"
#include "test.h"

#define MIN_MS (60u * 1000u)

/* ---- instrument with a silent engine ---- */
static void e_start(void *c, int v, const engine_note_t *n) { (void)c; (void)v; (void)n; }
static void e_release(void *c, int v) { (void)c; (void)v; }
static void e_kill(void *c, int v) { (void)c; (void)v; }
static void e_control(void *c, uint8_t part, uint8_t cc, uint8_t val) { (void)c; (void)part; (void)cc; (void)val; }
static void e_param(void *c, uint16_t id, uint8_t part, int32_t v) { (void)c; (void)id; (void)part; (void)v; }
static sound_engine_t ENGINE = {.start = e_start, .release = e_release, .kill = e_kill, .control = e_control,
                                .set_param = e_param};

static struct {
    int power_offs;
    int tx_7e_00_00; /* 7E 00 00 frames sent anywhere */
    /* 0x7D frames, last NG_LOG */
    int ng_n;
    uint8_t ng[16][48];
    uint16_t ng_len[16];
    midi_port_t ng_port[16];
    /* scripted controller: ACKs the piano's fn 0x10 / 0x03 so a bound port stays bound */
    bool auto_ack;
    int ackq_n;
    uint8_t ackq[32][4];
} o;

static void ack_drain(void);

static void p_tx(void *c, midi_port_t port, const uint8_t *m, uint16_t n)
{
    (void)c;
    if (o.auto_ack && n >= 10 && m[0] == 0xF0 && m[1] == 0x40 && (m[3] == 0x10 || m[3] == 0x03) && o.ackq_n < 32) {
        o.ackq[o.ackq_n][0] = (uint8_t)port;
        memcpy(&o.ackq[o.ackq_n][1], m + 6, 3);
        o.ackq_n++;
    }
    if (n == 9 && m[0] == 0xF0 && m[1] == 0x40 && m[3] == 0x7E && m[6] == 0x00 && m[7] == 0x00)
        o.tx_7e_00_00++;
    if (n >= 8 && m[0] == 0xF0 && m[1] == 0x7D && n <= 48) {
        int i = o.ng_n++ % 16;
        memcpy(o.ng[i], m, n);
        o.ng_len[i] = n;
        o.ng_port[i] = port;
    }
}
static void p_off(void *c) { (void)c; o.power_offs++; }

static rec_event_t pool[REC_MAX_EVENTS], stage[REC_MAX_EVENTS];
static piano_t P;

/* boot with APO 15 min and the given hold policy (NULL = default) */
static void boot15(const apo_policy_t *pol)
{
    memset(&o, 0, sizeof o);
    piano_platform_t plat = {.midi_tx = p_tx, .power_off = p_off};
    piano_init(&P, &plat, &ENGINE, NULL, pool, stage);
    /* bind the panel: the core takes sets only from a bound controller port (stock) */
    const uint8_t sync[] = {0xF0, 0x40, 0x7F, 0x7E, 0x08, 0x02, 0x01, 0x01, 0xF7};
    piano_midi_in(&P, MIDI_PORT_PANEL, sync, sizeof sync);
    /* the hold-rule tests observe the APO expiry as the real off (policy flag clear);
     * the standby action has its own tests below */
    power_policy_t pp;
    power_policy_default(&pp);
    pp.apo_standby = false;
    if (pol)
        pp.hold = *pol;
    piano_set_power_policy(&P, &pp, false);
    const uint8_t apo[] = {0xF0, 0x40, 0x00, 0x10, 0x08, 0x02, 0x51, 0x00, 0x7F, 0x01, 0xF7};
    piano_midi_in(&P, MIDI_PORT_PANEL, apo, sizeof apo);
}

static void bt(const char *line) { piano_bt_line(&P, line, (uint16_t)strlen(line)); }

static void ack_drain(void)
{
    while (o.ackq_n) {
        uint8_t k[4];
        memcpy(k, o.ackq[0], 4);
        memmove(o.ackq, o.ackq + 1, (size_t)(o.ackq_n - 1) * sizeof o.ackq[0]);
        o.ackq_n--;
        const uint8_t a[] = {0xF0, 0x40, 0x7F, 0x07, 0x08, 0x02, k[1], k[2], k[3], 0xF7};
        piano_midi_in(&P, (midi_port_t)k[0], a, sizeof a);
    }
}

/* advance whole minutes, 1 s at a time */
static void minutes(uint32_t m)
{
    for (uint32_t i = 0; i < m * 60u; i++) {
        piano_tick(&P, 1000);
        ack_drain();
    }
}

static void connect_and_play(void)
{
    piano_bt_link(&P, true);
    bt("RING 1 a6:c1:38:00:11:22 17 AVRCP");
    bt("CONNECT 0 A2DP 19");
    bt("A2DP CODEC SBC JOINT_STEREO 44100 BITPOOL 2-53");
    bt("A2DP STREAMING START 0");
}

/* ---------------- BT audio model ---------------- */

TEST(test_bt_audio_lines)
{
    bt_audio_t b;
    bt_audio_init(&b);
    CHECK_EQ(bt_audio_state(&b), BT_AUDIO_IDLE);
    CHECK(!bt_audio_line(&b, "WRAP THOR AI 6.2.0 build 1135", 29));
    CHECK(!bt_audio_line(&b, "AVRCP 1 EVENT VOLUME_CHANGED 64", 31));
    CHECK(!bt_audio_line(&b, "RING 1 a6:c1:38:00:11:22 17 AVRCP", 33)); /* AVRCP is not audio */
    CHECK_EQ(bt_audio_state(&b), BT_AUDIO_IDLE);
    CHECK(bt_audio_line(&b, "CONNECT 0 A2DP 19", 17));
    CHECK_EQ(bt_audio_state(&b), BT_AUDIO_CONNECTED);
    CHECK(bt_audio_line(&b, "A2DP STREAMING START 0", 22));
    CHECK_EQ(bt_audio_state(&b), BT_AUDIO_STREAMING);
    CHECK_EQ(b.starts, 1);
    CHECK(bt_audio_line(&b, "A2DP STREAMING STOP 0", 21));
    CHECK_EQ(bt_audio_state(&b), BT_AUDIO_CONNECTED);
    /* a NO CARRIER of another link (AVRCP) keeps A2DP */
    bt_audio_line(&b, "A2DP STREAMING START 0", 22);
    CHECK(!bt_audio_line(&b, "NO CARRIER 1 ERROR 0", 20));
    CHECK_EQ(bt_audio_state(&b), BT_AUDIO_STREAMING);
    CHECK(bt_audio_line(&b, "NO CARRIER 0 ERROR 0", 20));
    CHECK_EQ(bt_audio_state(&b), BT_AUDIO_IDLE);
    /* the module's reconnect attempts after the TV went away */
    CHECK(!bt_audio_line(&b, "NO CARRIER 0 ERROR 9802 L2CAP_CONNECTION_KEY_MISSING", 52));
    CHECK_EQ(bt_audio_state(&b), BT_AUDIO_IDLE);
    /* incoming A2DP via RING, then a module reboot drops everything */
    CHECK(bt_audio_line(&b, "RING 2 a6:c1:38:00:11:22 19 A2DP", 32));
    bt_audio_line(&b, "A2DP STREAMING START 3", 22);
    CHECK_EQ(bt_audio_state(&b), BT_AUDIO_STREAMING);
    CHECK(bt_audio_line(&b, "READY.", 6));
    CHECK_EQ(bt_audio_state(&b), BT_AUDIO_IDLE);
    /* garbage */
    CHECK(!bt_audio_line(&b, "NO CARRIER x ERROR", 18));
    CHECK(!bt_audio_line(&b, "A2DP STREAMING PAUSE 0", 22));
    CHECK(!bt_audio_line(&b, "", 0));
}

TEST(test_bt_audio_rx_and_link_flag)
{
    bt_audio_t b;
    bt_audio_init(&b);
    const char *raw = "READY.\r\nCONNECT 0 A2DP 19\r\nA2DP STREAM";
    bt_audio_rx(&b, (const uint8_t *)raw, (uint16_t)strlen(raw));
    CHECK_EQ(bt_audio_state(&b), BT_AUDIO_CONNECTED);
    const char *rest = "ING START 0\r\n\n";
    bt_audio_rx(&b, (const uint8_t *)rest, (uint16_t)strlen(rest));
    CHECK_EQ(bt_audio_state(&b), BT_AUDIO_STREAMING);
    /* an overlong line is dropped, the next one still parses */
    uint8_t junk[BT_LINE_MAX + 20];
    memset(junk, 'A', sizeof junk);
    bt_audio_rx(&b, junk, sizeof junk);
    const char *stop = "\r\nA2DP STREAMING STOP 0\r\n";
    bt_audio_rx(&b, (const uint8_t *)stop, (uint16_t)strlen(stop));
    CHECK_EQ(bt_audio_state(&b), BT_AUDIO_CONNECTED);

    /* stock link flag: a constant 0 does nothing, a 1 -> 0 edge is link loss */
    bt_audio_init(&b);
    bt_audio_link(&b, false);
    bt_audio_line(&b, "A2DP STREAMING START 0", 22); /* START with the flag not seen yet */
    CHECK_EQ(bt_audio_state(&b), BT_AUDIO_STREAMING);
    bt_audio_link(&b, true);
    CHECK_EQ(bt_audio_state(&b), BT_AUDIO_STREAMING);
    bt_audio_link(&b, false); /* TV Bluetooth off / BT audio setting off */
    CHECK_EQ(bt_audio_state(&b), BT_AUDIO_IDLE);
    bt_audio_link(&b, true);  /* flag alone = connected (it stays stale after a module reset) */
    CHECK_EQ(bt_audio_state(&b), BT_AUDIO_CONNECTED);
}

/* ---------------- APO hold (whole instrument) ---------------- */

TEST(test_apo_streaming_keeps_piano_on)
{
    boot15(NULL);
    connect_and_play();
    minutes(90); /* far past the 15 min APO, below the 5 h cap */
    CHECK_EQ(o.power_offs, 0);
    CHECK(P.apo_hold.holding);
    /* STREAMING STOP: the full 15 min count again from here */
    bt("A2DP STREAMING STOP 0");
    minutes(14);
    CHECK_EQ(o.power_offs, 0);
    minutes(1);
    CHECK_EQ(o.power_offs, 1);
    CHECK_EQ(o.tx_7e_00_00, 1); /* the real power-off tells the bound panel 7E 00 00 (stock FUN_4014B0A8) */
}

/* The rule (owner decision 2026-11-02): while the stream is open no auto-off up to the max hold;
 * at the max hold the APO acts right away for an idle piano, a used one counts on from its last activity. */
TEST(test_apo_tv_pause_falls_back_to_max_hold)
{
    boot15(NULL); /* default cap 5 h */
    connect_and_play();
    /* TV paused: the stream stays open, no STOP ever comes */
    minutes(5 * 60 - 1);
    CHECK_EQ(o.power_offs, 0);
    CHECK(P.apo_hold.holding && !P.apo_hold.capped);
    CHECK_EQ(P.power.idle_ms, 15u * MIN_MS); /* the clock ran, the hold only kept it from expiring */
    minutes(1);
    CHECK(P.apo_hold.capped);
    CHECK_EQ(o.power_offs, 1); /* the cap: off right away, not cap + 15 min */

    /* activity right before the cap: the normal APO counts on from it */
    boot15(NULL);
    connect_and_play();
    minutes(5 * 60 - 5);
    const uint8_t reset[] = {0xF0, 0x40, 0x7F, 0x32, 0x08, 0x02, 0x51, 0x39, 0x7F, 0x7F, 0xF7};
    piano_midi_in(&P, MIDI_PORT_PANEL, reset, sizeof reset); /* PowerManagementTimerReset (a panel touch) */
    minutes(5);                                              /* the cap: 5 of 15 min since the touch */
    CHECK(P.apo_hold.capped);
    CHECK_EQ(o.power_offs, 0);
    minutes(9);
    CHECK_EQ(o.power_offs, 0);
    minutes(1);
    CHECK_EQ(o.power_offs, 1);

    /* a new STREAMING START (play after a source switch) re-arms the cap */
    apo_policy_t pol = {.max_hold_ms = 30u * MIN_MS, .hold_connected = false};
    boot15(&pol);
    connect_and_play();
    minutes(20);
    piano_midi_in(&P, MIDI_PORT_PANEL, reset, sizeof reset);
    minutes(12); /* capped at 30, 12 of 15 min counted since the touch at 20 */
    CHECK(P.apo_hold.capped);
    CHECK_EQ(o.power_offs, 0);
    bt("A2DP STREAMING START 0");
    minutes(29);
    CHECK_EQ(o.power_offs, 0);
    CHECK(P.apo_hold.holding);
    minutes(1); /* the new window used up, idle since 20: off right away */
    CHECK_EQ(o.power_offs, 1);
}

/* (owner decision 2026-11-03): a source that reports PAUSED / STOPPED via AVRCP does not hold although
 * its stream stays open (Windows keeps it open for minutes after a pause); PLAYING again opens a new window; no status
 * known = the stream alone holds (the old rule) */
TEST(test_apo_hold_follows_the_play_status)
{
    boot15(NULL); /* 5 h */
    connect_and_play();
    bt_audio_play(&P.bt, 1);
    minutes(60);
    CHECK(P.apo_hold.holding);
    CHECK_EQ(P.power.idle_ms, 15u * MIN_MS);   /* resting at the limit */
    bt_audio_play(&P.bt, 0);                    /* CHANGED PAUSED, the stream stays open */
    minutes(1);
    CHECK(!P.apo_hold.holding);
    CHECK_EQ(bt_audio_state(&P.bt), BT_AUDIO_STREAMING);
    CHECK_EQ(o.power_offs, 0);                  /* released like a STOP: the full 15 min from the pause */
    minutes(13);
    CHECK_EQ(o.power_offs, 0);
    minutes(2);
    CHECK_EQ(o.power_offs, 1);

    /* PLAYING again with the stream still open: a new hold (a new 30 min window), as a new START */
    apo_policy_t pol = {.max_hold_ms = 30u * MIN_MS, .hold_connected = false};
    boot15(&pol);
    connect_and_play();
    bt_audio_play(&P.bt, 1);
    minutes(20);
    bt_audio_play(&P.bt, 0);                    /* paused at 20 min */
    minutes(10);
    CHECK_EQ(o.power_offs, 0);
    bt_audio_play(&P.bt, 1);                    /* played again at 30 min (no new START) */
    minutes(29);
    CHECK(P.apo_hold.holding && !P.apo_hold.capped);
    CHECK_EQ(o.power_offs, 0);
    minutes(1);                                 /* the new window used up, idle: off right away */
    CHECK(P.apo_hold.capped);
    CHECK_EQ(o.power_offs, 1);

    /* paused after the cap: nothing to release (the normal APO already runs), PLAYING opens a new window */
    boot15(&pol);
    connect_and_play();
    bt_audio_play(&P.bt, 1);
    const uint8_t reset[] = {0xF0, 0x40, 0x7F, 0x32, 0x08, 0x02, 0x51, 0x39, 0x7F, 0x7F, 0xF7};
    minutes(25);
    piano_midi_in(&P, MIDI_PORT_PANEL, reset, sizeof reset);   /* a touch at 25 min */
    minutes(6);                                                 /* capped at 30, 6 of 15 min since the touch */
    CHECK(P.apo_hold.capped);
    bt_audio_play(&P.bt, 0);
    minutes(1);
    CHECK(!P.apo_hold.released && !P.apo_hold.capped);
    bt_audio_play(&P.bt, 1);
    minutes(29);
    CHECK_EQ(o.power_offs, 0);
    CHECK(P.apo_hold.holding);

    /* stopped (and any other non-playing report) = paused; not known = the old rule (the stream holds up to the cap) */
    boot15(&pol);
    connect_and_play();
    bt_audio_play(&P.bt, -1);
    minutes(29);
    CHECK(P.apo_hold.holding);
    CHECK_EQ(o.power_offs, 0);
    minutes(1);
    CHECK_EQ(o.power_offs, 1);

    /* "hold while only connected" (opt-in) holds on the link, paused or not */
    pol.hold_connected = true;
    boot15(&pol);
    connect_and_play();
    bt_audio_play(&P.bt, 0);
    minutes(20);
    CHECK(P.apo_hold.holding);
    CHECK_EQ(o.power_offs, 0);
}

TEST(test_apo_no_carrier_resumes_countdown)
{
    boot15(NULL);
    connect_and_play();
    minutes(30);
    bt("NO CARRIER 0 ERROR 0");
    bt("NO CARRIER 1 ERROR 0");
    /* the stock flag is still 1 until its own parser runs: connected, not streaming */
    CHECK_EQ(bt_audio_state(&P.bt), BT_AUDIO_CONNECTED);
    CHECK_EQ(P.bt.a2dp_links, 0);
    minutes(14);
    CHECK_EQ(o.power_offs, 0);
    minutes(1);
    CHECK_EQ(o.power_offs, 1);

    /* link loss seen only through the stock flag (no line reached us) */
    boot15(NULL);
    connect_and_play();
    minutes(30);
    piano_bt_link(&P, false);
    minutes(15);
    CHECK_EQ(o.power_offs, 1);
}

TEST(test_apo_policy_options)
{
    /* connected but not streaming: not held by default (TV link stays up while silent) */
    boot15(NULL);
    piano_bt_link(&P, true);
    bt("CONNECT 0 A2DP 19");
    minutes(15);
    CHECK_EQ(o.power_offs, 1);

    /* owner option: connected counts too, with the same cap */
    apo_policy_t pol;
    apo_policy_default(&pol);
    pol.hold_connected = true;
    boot15(&pol);
    piano_bt_link(&P, true);
    bt("CONNECT 0 A2DP 19");
    minutes(5 * 60 - 1);
    CHECK_EQ(o.power_offs, 0);
    minutes(1);
    CHECK_EQ(o.power_offs, 1); /* the cap, idle: right away */

    /* max hold 0 = stock behaviour: streaming does not hold */
    pol = (apo_policy_t){.max_hold_ms = 0};
    boot15(&pol);
    connect_and_play();
    minutes(15);
    CHECK_EQ(o.power_offs, 1);

    /* APO_HOLD_FOREVER: a paused TV never lets go */
    pol = (apo_policy_t){.max_hold_ms = APO_HOLD_FOREVER};
    boot15(&pol);
    connect_and_play();
    minutes(10 * 60);
    CHECK_EQ(o.power_offs, 0);

    /* APO Never: nothing to hold, never off */
    boot15(NULL);
    const uint8_t never[] = {0xF0, 0x40, 0x00, 0x10, 0x08, 0x02, 0x51, 0x00, 0x7F, 0x00, 0xF7};
    piano_midi_in(&P, MIDI_PORT_PANEL, never, sizeof never);
    connect_and_play();
    minutes(200);
    CHECK_EQ(o.power_offs, 0);
    CHECK(!P.apo_hold.holding);
}

/* ---------------- partial standby ---------------- */

static standby_t SB;

static void sb_boot(uint32_t cap_ms, bool deep)
{
    hal_power_sim_reset();
    standby_ops_t ops;
    hal_standby_ops(&ops);
    standby_init(&SB, &ops, cap_ms, deep);
}

static bool log_is(const char *const *want, int n)
{
    if (hal_power_sim.n != n)
        return false;
    for (int i = 0; i < n; i++)
        if (strcmp(hal_power_sim.log[i], want[i]) != 0)
            return false;
    return true;
}

TEST(test_standby_enter_wake_order)
{
    sb_boot(STANDBY_CAP_DEFAULT_MS, false);
    CHECK(standby_enter(&SB));
    static const char *const ENTER[] = {"cleanup", "amp_off", "mute", "bt_off", "note_standby"};
    CHECK(log_is(ENTER, 5));
    CHECK_EQ(SB.state, SB_STANDBY);
    CHECK(!hal_power_sim.amp && hal_power_sim.mute && !hal_power_sim.bt_audio && hal_power_sim.codec);
    CHECK(!standby_enter(&SB));

    hal_power_sim.n = 0;
    CHECK(standby_wake(&SB, SB_WAKE_KEY));
    static const char *const WAKE1[] = {"headphones", "unmute"};
    CHECK(log_is(WAKE1, 2));
    CHECK_EQ(SB.state, SB_WAKING);
    standby_tick(&SB, STANDBY_WAKE_DELAY_MS - 1);
    CHECK(!hal_power_sim.amp); /* amp waits for the delay after the unmute */
    standby_tick(&SB, 1);
    static const char *const WAKE[] = {"headphones", "unmute", "headphones", "amp_on", "bt_on", "note_awake"};
    CHECK(log_is(WAKE, 6));
    CHECK_EQ(SB.state, SB_ON);
    CHECK(hal_power_sim.amp && !hal_power_sim.mute && hal_power_sim.bt_audio);
    CHECK_EQ(SB.last_wake, SB_WAKE_KEY);

    /* stage 2 (codec) with headphones plugged and BT audio set Off */
    sb_boot(STANDBY_CAP_DEFAULT_MS, true);
    hal_power_sim.bt_audio_enabled = false;
    hal_power_sim.headphones = true;
    standby_enter(&SB);
    static const char *const ENTER2[] = {"cleanup", "amp_off", "mute", "codec_off", "note_standby"};
    CHECK(log_is(ENTER2, 5));
    hal_power_sim.n = 0;
    standby_wake(&SB, SB_WAKE_POWER_BUTTON);
    standby_tick(&SB, STANDBY_WAKE_DELAY_MS);
    CHECK_EQ(SB.state, SB_WAKING); /* 500 ms after a codec re-init */
    standby_tick(&SB, STANDBY_WAKE_DELAY_CODEC_MS - STANDBY_WAKE_DELAY_MS);
    static const char *const WAKE2[] = {"codec_on", "headphones", "unmute", "headphones", "note_awake"};
    CHECK(log_is(WAKE2, 5));
    CHECK(!hal_power_sim.amp); /* headphones: speakers stay off */

    /* GP3[8] alarm at wake: amp and mute are left to the stock event */
    sb_boot(STANDBY_CAP_DEFAULT_MS, false);
    standby_enter(&SB);
    hal_power_sim.amp_alarm = true;
    hal_power_sim.n = 0;
    standby_wake(&SB, SB_WAKE_BLE_OP);
    standby_tick(&SB, STANDBY_WAKE_DELAY_MS);
    static const char *const WAKE3[] = {"headphones", "bt_on", "note_awake"};
    CHECK(log_is(WAKE3, 3));
    CHECK(hal_power_sim.mute && !hal_power_sim.amp);
}

TEST(test_standby_wake_sources)
{
    sb_boot(STANDBY_CAP_DEFAULT_MS, false);
    standby_enter(&SB);
    const uint8_t touch[] = {0xF0, 0x40, 0x7F, 0x32, 0x08, 0x02, 0x51, 0x39, 0x7F, 0x7F, 0xF7};
    const uint8_t vol[] = {0xF0, 0x40, 0x7F, 0x10, 0x08, 0x02, 0x55, 0x01, 0x7F, 0x40, 0xF7};
    CHECK(!standby_midi_wake(&SB, MIDI_PORT_BLE, touch, sizeof touch));   /* only the panel port */
    CHECK(!standby_midi_wake(&SB, MIDI_PORT_PANEL, vol, sizeof vol));     /* other panel traffic */
    CHECK(!standby_midi_wake(&SB, MIDI_PORT_PANEL, touch, 5));
    CHECK_EQ(SB.state, SB_STANDBY);
    CHECK(standby_midi_wake(&SB, MIDI_PORT_PANEL, touch, sizeof touch));
    CHECK_EQ(SB.last_wake, SB_WAKE_PANEL_TOUCH);
    CHECK(!standby_wake(&SB, SB_WAKE_KEY)); /* already waking */
    standby_tick(&SB, STANDBY_WAKE_DELAY_MS);
    CHECK_EQ(SB.state, SB_ON);
    CHECK(!standby_midi_wake(&SB, MIDI_PORT_PANEL, touch, sizeof touch)); /* not in standby */
    CHECK(!standby_wake(&SB, SB_WAKE_KEY));
}

TEST(test_standby_hard_cap)
{
    sb_boot(STANDBY_CAP_DEFAULT_MS, false);
    standby_enter(&SB);
    for (uint32_t m = 0; m < 8u * 60u - 1u; m++)
        standby_tick(&SB, MIN_MS);
    CHECK_EQ(hal_power_sim.power_offs, 0);
    CHECK(standby_active(&SB));
    standby_tick(&SB, MIN_MS);
    CHECK_EQ(hal_power_sim.power_offs, 1);
    CHECK_EQ(SB.state, SB_OFF);
    CHECK(!standby_wake(&SB, SB_WAKE_POWER_BUTTON)); /* off is final */
    standby_tick(&SB, MIN_MS);
    CHECK_EQ(hal_power_sim.power_offs, 1);

    /* hard off straight from ON (power button held / remote off) */
    sb_boot(0, false);
    standby_hard_off(&SB);
    CHECK_EQ(hal_power_sim.power_offs, 1);
    /* cap 0 = no cap */
    sb_boot(0, false);
    standby_enter(&SB);
    standby_tick(&SB, 0xFFFFFFF0u);
    standby_tick(&SB, 0xFFFFFFF0u);
    CHECK_EQ(SB.state, SB_STANDBY);
}

TEST(test_standby_flash_busy_defers_power_off)
{
    sb_boot(60u * MIN_MS, false);
    standby_enter(&SB); /* cleanup closed a recording: the RecorderData job is queued */
    hal_power_sim.flash_busy = true;
    standby_tick(&SB, 60u * MIN_MS);
    CHECK_EQ(SB.state, SB_OFF_PENDING);
    CHECK_EQ(hal_power_sim.power_offs, 0);
    for (int i = 0; i < 100; i++)
        standby_tick(&SB, 1000);
    CHECK_EQ(hal_power_sim.power_offs, 0);
    CHECK_EQ(SB.busy_wait_ms, 100000);
    CHECK(!standby_wake(&SB, SB_WAKE_KEY)); /* the off decision stands */
    hal_power_sim.flash_busy = false;
    standby_tick(&SB, 20);
    CHECK_EQ(hal_power_sim.power_offs, 1);
    CHECK_EQ(SB.state, SB_OFF);

    /* hard off from ON while busy */
    sb_boot(0, false);
    hal_power_sim.flash_busy = true;
    standby_hard_off(&SB);
    standby_hard_off(&SB);
    CHECK_EQ(hal_power_sim.power_offs, 0);
    hal_power_sim.flash_busy = false;
    standby_tick(&SB, 1);
    CHECK_EQ(hal_power_sim.power_offs, 1);
}

TEST(test_hal_power_stubs)
{
    hal_power_sim_reset();
    hal_power_sim.a2dp_link = true;
    CHECK(hal_bt_a2dp_link());
    CHECK(hal_bt_audio_enabled());
    CHECK(!hal_flash_busy());
    CHECK(!hal_amp_alarm());
    hal_amp_enable(false);
    hal_output_mute(true);
    hal_bt_audio_power(false);
    hal_codec_power(false);
    hal_standby_cleanup();
    CHECK(!hal_headphones_present());
    CHECK(!hal_power_sim.amp && hal_power_sim.mute && !hal_power_sim.bt_audio && !hal_power_sim.codec);
    CHECK_EQ(hal_power_sim.n, 6);
}

/* ---------------- power policy: APO -> standby, 0x7D ops, persistence ---------------- */

#define FSEC 4096u
static uint8_t FMEM[2][FSEC];
static bool f_erase(void *c, uint32_t sct) { (void)c; memset(FMEM[sct], 0xFF, FSEC); return true; }
static bool f_prog(void *c, uint32_t sct, uint32_t off, const void *d, uint32_t n)
{
    (void)c;
    for (uint32_t i = 0; i < n; i++) FMEM[sct][off + i] &= ((const uint8_t *)d)[i];
    return true;
}
static bool f_read(void *c, uint32_t sct, uint32_t off, void *d, uint32_t n) { (void)c; memcpy(d, FMEM[sct] + off, n); return true; }
static const hal_flash_t FLASH = {NULL, FSEC, f_erase, f_prog, f_read};
static kvstore_t KV;
static standby_ops_t HAL_OPS;

static const uint8_t SYNC[] = {0xF0, 0x40, 0x7F, 0x7E, 0x08, 0x02, 0x01, 0x01, 0xF7};
static const uint8_t APO15[] = {0xF0, 0x40, 0x00, 0x10, 0x08, 0x02, 0x51, 0x00, 0x7F, 0x01, 0xF7};

/* power-on with the settings flash (wipe = new instrument), hal stubs, panel + USB bound */
static void boot_flash(bool wipe)
{
    memset(&o, 0, sizeof o);
    if (wipe)
        memset(FMEM, 0xFF, sizeof FMEM);
    kv_mount(&KV, &FLASH, 0, 1);
    hal_power_sim_reset();
    hal_standby_ops(&HAL_OPS);
    piano_platform_t plat = {.midi_tx = p_tx, .power_off = p_off, .standby = &HAL_OPS};
    piano_init(&P, &plat, &ENGINE, &KV, pool, stage);
    piano_midi_in(&P, MIDI_PORT_PANEL, SYNC, sizeof SYNC);
    piano_midi_in(&P, MIDI_PORT_USB, SYNC, sizeof SYNC);
    o.auto_ack = true;
    piano_midi_in(&P, MIDI_PORT_PANEL, APO15, sizeof APO15);
    ack_drain();
    o.ng_n = 0;
}

/* F0 7D "CA79" op args F7 into a port; returns the last 0x7D reply (or NULL) */
static const uint8_t *ng(midi_port_t port, const uint8_t *args, int n, uint16_t *len)
{
    uint8_t m[40] = {0xF0, 0x7D, 0x43, 0x41, 0x37, 0x39};
    memcpy(m + 6, args, (size_t)n);
    m[6 + n] = 0xF7;
    int before = o.ng_n;
    piano_midi_in(&P, port, m, (uint16_t)(7 + n));
    ack_drain();
    if (o.ng_n == before)
        return NULL;
    int i = (o.ng_n - 1) % 16;
    if (len) *len = o.ng_len[i];
    return o.ng[i];
}

/* 18 01 <pack7 raw>: a policy as the app sends it */
static const uint8_t *ng_set(midi_port_t port, const uint8_t raw[POWER_POLICY_RAW], uint16_t *len)
{
    uint8_t a[2 + 12] = {0x18, 0x01};
    int k = 2;
    for (int i = 0; i < POWER_POLICY_RAW; i += 7) {
        int g = POWER_POLICY_RAW - i < 7 ? POWER_POLICY_RAW - i : 7;
        uint8_t hi = 0;
        for (int j = 0; j < g; j++) hi |= (uint8_t)(((raw[i + j] >> 7) & 1) << j);
        a[k++] = hi;
        for (int j = 0; j < g; j++) a[k++] = raw[i + j] & 0x7F;
    }
    return ng(port, a, k, len);
}

TEST(test_power_policy_codec)
{
    power_policy_t d, x;
    power_policy_default(&d);
    CHECK(d.apo_standby);
    CHECK(!d.hold.hold_connected);
    CHECK_EQ(d.hold.max_hold_ms, 5u * 60u * MIN_MS);
    CHECK_EQ(d.standby_cap_ms, STANDBY_CAP_DEFAULT_MS);
    uint8_t raw[POWER_POLICY_RAW];
    power_policy_encode(&d, raw);
    const uint8_t want[POWER_POLICY_RAW] = {1, 0x01, 0x2C, 0x01, 0xE0, 0x01, 0, 0}; /* 300 min, 480 min */
    CHECK_MEM(raw, want, sizeof want);
    memset(&x, 0, sizeof x);
    CHECK(power_policy_decode(&x, raw, sizeof raw));
    CHECK(x.apo_standby && x.hold.max_hold_ms == d.hold.max_hold_ms && x.standby_cap_ms == d.standby_cap_ms);

    /* unlimited hold, rule off, hold connected */
    x.hold.max_hold_ms = APO_HOLD_FOREVER;
    x.hold.hold_connected = true;
    x.apo_standby = false;
    power_policy_encode(&x, raw);
    CHECK_EQ(raw[1], POWER_FLAG_HOLD_CONNECTED);
    CHECK_EQ(raw[2] | raw[3] << 8, POWER_HOLD_UNLIMITED);
    power_policy_t y = d;
    CHECK(power_policy_decode(&y, raw, sizeof raw));
    CHECK_EQ(y.hold.max_hold_ms, APO_HOLD_FOREVER);
    CHECK(y.hold.hold_connected && !y.apo_standby);
    raw[2] = raw[3] = 0;
    CHECK(power_policy_decode(&y, raw, sizeof raw));
    CHECK_EQ(y.hold.max_hold_ms, 0);

    /* rejected: version, short, hold > 24 h, cap < 30 min / > 24 h (no "never") */
    power_policy_encode(&d, raw);
    y = d;
    raw[0] = 2;
    CHECK(!power_policy_decode(&y, raw, sizeof raw));
    raw[0] = 1;
    CHECK(!power_policy_decode(&y, raw, 5));
    raw[2] = 0xA1; raw[3] = 0x05; /* 1441 min */
    CHECK(!power_policy_decode(&y, raw, sizeof raw));
    raw[2] = 120; raw[3] = 0;
    raw[4] = 29; raw[5] = 0;
    CHECK(!power_policy_decode(&y, raw, sizeof raw));
    raw[4] = 0; raw[5] = 0;
    CHECK(!power_policy_decode(&y, raw, sizeof raw));
    raw[4] = 0xA1; raw[5] = 0x05;
    CHECK(!power_policy_decode(&y, raw, sizeof raw));
    CHECK(y.apo_standby && y.standby_cap_ms == d.standby_cap_ms); /* untouched on failure */
}

/* the standby notice reaches every connected controller but the panel, also when the panel connected last (the
 * stock binding's place in the cave) */
TEST(test_standby_notice_to_every_controller_but_the_panel)
{
    boot_flash(true);
    const uint8_t ble_on[] = {0xF0, 0x40, 0x00, 0x10, 0x08, 0x02, 0x51, 0x3D, 0x7F, 0x01, 0xF7};
    piano_midi_in(&P, MIDI_PORT_PANEL, ble_on, sizeof ble_on);   /* BLE MIDI on (51 3D), then the phone connects */
    ack_drain();
    piano_midi_in(&P, MIDI_PORT_BLE, SYNC, sizeof SYNC);
    piano_midi_in(&P, MIDI_PORT_PANEL, SYNC, sizeof SYNC);   /* the panel connects last */
    ack_drain();
    CHECK(P.kp.connected[MIDI_PORT_BLE] && P.kp.connected[MIDI_PORT_USB]);
    o.ng_n = 0;
    minutes(15);
    CHECK_EQ(P.sb.state, SB_STANDBY);
    int usb = 0, ble = 0, panel = 0;
    for (int i = 0; i < o.ng_n && i < 16; i++) {
        if (o.ng_len[i] != 11 || o.ng[i][6] != 0x59 || o.ng[i][7] != 0x7F || o.ng[i][9] != 0x01)
            continue;
        usb += o.ng_port[i] == MIDI_PORT_USB;
        ble += o.ng_port[i] == MIDI_PORT_BLE;
        panel += o.ng_port[i] == MIDI_PORT_PANEL;
    }
    CHECK_EQ(usb, 1);
    CHECK_EQ(ble, 1);
    CHECK_EQ(panel, 0);
}

TEST(test_apo_expiry_enters_standby)
{
    boot_flash(true);
    minutes(14);
    CHECK_EQ(P.sb.state, SB_ON);
    o.ng_n = 0;
    minutes(1);
    /* default policy: partial standby, not the real off; no 7E 00 00 to anyone */
    CHECK_EQ(P.sb.state, SB_STANDBY);
    CHECK_EQ(o.power_offs, 0);
    CHECK_EQ(o.tx_7e_00_00, 0);
    CHECK(P.powered);
    /* default policy keeps the BT audio module on (TV wake) */
    CHECK(!hal_power_sim.amp && hal_power_sim.mute && hal_power_sim.bt_audio);
    /* notice 59 7F 00 01 to the bound USB controller, not to the panel */
    CHECK_EQ(o.ng_n, 1);
    const uint8_t note[] = {0xF0, 0x7D, 0x43, 0x41, 0x37, 0x39, 0x59, 0x7F, 0x00, 0x01, 0xF7};
    CHECK_EQ(o.ng_len[0], sizeof note);
    CHECK_MEM(o.ng[0], note, sizeof note);
    CHECK_EQ(o.ng_port[0], MIDI_PORT_USB);

    /* parameter traffic does not wake; a key does, and the APO counts from the wake-up */
    const uint8_t vol[] = {0xF0, 0x40, 0x00, 0x10, 0x08, 0x02, 0x55, 0x01, 0x00, 0x40, 0xF7};
    piano_midi_in(&P, MIDI_PORT_USB, vol, sizeof vol);
    minutes(60);
    CHECK_EQ(P.sb.state, SB_STANDBY);
    piano_key_edge(&P, 40, 1, true, 0);
    piano_key_edge(&P, 40, 2, true, 3000);
    CHECK_EQ(P.sb.state, SB_WAKING);
    piano_tick(&P, STANDBY_WAKE_DELAY_MS);
    CHECK_EQ(P.sb.state, SB_ON);
    CHECK(hal_power_sim.amp && !hal_power_sim.mute && hal_power_sim.bt_audio);
    CHECK_EQ(o.ng[(o.ng_n - 1) % 16][9], 0x00); /* awake notice: state 0 */
    minutes(14);
    CHECK_EQ(P.sb.state, SB_ON);
    minutes(1);
    CHECK_EQ(P.sb.state, SB_STANDBY);

    /* standby cap 8 h, then the real off with 7E 00 00 */
    for (int m = 0; m < 8 * 60 - 1; m++) { piano_tick(&P, MIN_MS); ack_drain(); }
    CHECK_EQ(o.power_offs, 0);
    piano_tick(&P, MIN_MS);
    CHECK_EQ(o.power_offs, 1);
    CHECK_EQ(o.tx_7e_00_00, 2); /* panel + USB */
    CHECK(!P.powered);

    /* a pending flash job holds the off back */
    boot_flash(true);
    minutes(15);
    hal_power_sim.flash_busy = true;
    for (int m = 0; m < 8 * 60 + 30; m++) { piano_tick(&P, MIN_MS); ack_drain(); }
    CHECK_EQ(P.sb.state, SB_OFF_PENDING);
    CHECK_EQ(o.power_offs, 0);
    hal_power_sim.flash_busy = false;
    piano_tick(&P, 20);
    CHECK_EQ(o.power_offs, 1);

    /* streaming BT audio still holds the APO, so no standby either */
    boot_flash(true);
    connect_and_play();
    minutes(90);
    CHECK_EQ(P.sb.state, SB_ON);

    /* remote power-off (7E 00 00 from a bound controller) goes through the same deferral */
    boot_flash(true);
    hal_power_sim.flash_busy = true;
    const uint8_t off[] = {0xF0, 0x40, 0x7F, 0x7E, 0x08, 0x02, 0x00, 0x00, 0xF7};
    piano_midi_in(&P, MIDI_PORT_USB, off, sizeof off);
    CHECK_EQ(o.power_offs, 0);
    hal_power_sim.flash_busy = false;
    piano_tick(&P, 1);
    CHECK_EQ(o.power_offs, 1);
}

TEST(test_ng_power_policy_op)
{
    boot_flash(true);
    uint16_t n = 0;
    /* Identify: the host core reports feat1 0x10 (power policy) | 0x40 (TV wake) */
    const uint8_t id[] = {0x01};
    const uint8_t *r = ng(MIDI_PORT_BLE, id, 1, &n);
    CHECK(r == NULL); /* BLE port: Bluetooth MIDI Off, nothing from the radio is accepted */
    r = ng(MIDI_PORT_DIN, id, 1, &n);
    const uint8_t idr[] = {0xF0, 0x7D, 0x43, 0x41, 0x37, 0x39, 0x41, 0x01, 0x02, 0x13, 0x01, 0x50, 0xF7};
    CHECK(r && n == sizeof idr && memcmp(r, idr, n) == 0);

    /* get on an unbound port: 58 00 00 <pack7 defaults> */
    const uint8_t get[] = {0x18, 0x00};
    r = ng(MIDI_PORT_DIN, get, 2, &n);
    const uint8_t getr[] = {0xF0, 0x7D, 0x43, 0x41, 0x37, 0x39, 0x58, 0x00, 0x00,
                            0x10, 0x01, 0x01, 0x2C, 0x01, 0x60, 0x01, 0x00, 0x00, 0x00, 0xF7};
    CHECK(r && n == sizeof getr && memcmp(r, getr, n) == 0);

    /* set from an unbound port: rc 2, unchanged */
    const uint8_t mine[POWER_POLICY_RAW] = {1, POWER_FLAG_HOLD_CONNECTED, 60, 0, 240, 0, 0, 0}; /* off at APO, 1 h, 4 h */
    r = ng_set(MIDI_PORT_DIN, mine, &n);
    CHECK(r && r[6] == 0x58 && r[7] == 0x01 && r[8] == 2);
    CHECK(P.policy.apo_standby);

    /* set from the bound USB controller: rc 0, applied, saved, echoed */
    r = ng_set(MIDI_PORT_USB, mine, &n);
    CHECK(r && r[7] == 0x01 && r[8] == 0);
    CHECK(!P.policy.apo_standby && P.policy.hold.hold_connected);
    CHECK_EQ(P.policy.hold.max_hold_ms, 60u * MIN_MS);
    CHECK_EQ(P.apo_hold.pol.max_hold_ms, 60u * MIN_MS);
    CHECK_EQ(P.sb.cap_ms, 240u * MIN_MS);
    uint8_t saved[POWER_POLICY_RAW];
    CHECK_EQ(kv_get(&KV, KV_KEY_POWER_POLICY, saved, sizeof saved), POWER_POLICY_RAW);
    CHECK_MEM(saved, mine, sizeof mine);

    /* a bad value: rc 1, nothing changes */
    const uint8_t bad[POWER_POLICY_RAW] = {1, 0x01, 120, 0, 10, 0, 0, 0}; /* cap 10 min */
    r = ng_set(MIDI_PORT_USB, bad, &n);
    CHECK(r && r[8] == 1);
    CHECK(!P.policy.apo_standby);
    CHECK_EQ(P.sb.cap_ms, 240u * MIN_MS);

    /* the new policy acts: connected only now holds (1 h cap - then right away), APO expiry is the real off */
    piano_bt_link(&P, true);
    bt("CONNECT 0 A2DP 19");
    minutes(59);
    CHECK_EQ(o.power_offs, 0);
    minutes(1);
    CHECK_EQ(o.power_offs, 1);
    CHECK_EQ(P.sb.state, SB_OFF);

    /* defaults (18 02) from a bound port */
    boot_flash(false);
    CHECK(!P.policy.apo_standby); /* still the saved one */
    const uint8_t def[] = {0x18, 0x02};
    r = ng(MIDI_PORT_USB, def, 2, &n);
    CHECK(r && r[7] == 0x02 && r[8] == 0);
    CHECK(P.policy.apo_standby);

    /* unknown op / sub-op and a wrong tag stay silent */
    const uint8_t unk[] = {0x3E};
    CHECK(ng(MIDI_PORT_USB, unk, 1, &n) == NULL);
    const uint8_t unks[] = {0x18, 0x55};
    CHECK(ng(MIDI_PORT_USB, unks, 2, &n) == NULL);
    const uint8_t badtag[] = {0xF0, 0x7D, 0x43, 0x41, 0x37, 0x38, 0x01, 0xF7};
    int before = o.ng_n;
    piano_midi_in(&P, MIDI_PORT_USB, badtag, sizeof badtag);
    CHECK_EQ(o.ng_n, before);
}

TEST(test_power_policy_persists_across_reboot)
{
    boot_flash(true);
    const uint8_t mine[POWER_POLICY_RAW] = {1, 0x00, 0xFF, 0xFF, 30, 0, 0, 0}; /* off, unlimited hold, cap 30 min */
    uint16_t n;
    const uint8_t *r = ng_set(MIDI_PORT_USB, mine, &n);
    CHECK(r && r[8] == 0);

    /* power cycle: same flash, new instance */
    boot_flash(false);
    CHECK(!P.policy.apo_standby);
    CHECK_EQ(P.policy.hold.max_hold_ms, APO_HOLD_FOREVER);
    CHECK_EQ(P.apo_hold.pol.max_hold_ms, APO_HOLD_FOREVER);
    CHECK_EQ(P.sb.cap_ms, 30u * MIN_MS);
    const uint8_t get[] = {0x18, 0x00};
    r = ng(MIDI_PORT_DIN, get, 2, &n);
    CHECK(r && n == 20);
    uint8_t enc[POWER_POLICY_RAW];
    power_policy_encode(&P.policy, enc);
    CHECK_MEM(enc, mine, sizeof mine);
    /* it acts after the reboot too: a paused TV holds forever, APO expiry would be the real off */
    connect_and_play();
    minutes(10 * 60);
    CHECK_EQ(o.power_offs, 0);

    /* "Save startup settings" does not touch it; the factory reset brings the defaults back */
    boot_flash(false);
    const uint8_t save[] = {0xF0, 0x40, 0x00, 0x32, 0x08, 0x02, 0x51, 0x35, 0x7F, 0x00, 0x00, 0x00, 0x00, 0xF7};
    piano_midi_in(&P, MIDI_PORT_PANEL, save, sizeof save);
    CHECK(!P.policy.apo_standby);
    piano_factory_reset(&P);
    CHECK(P.policy.apo_standby);
    CHECK_EQ(kv_get(&KV, KV_KEY_POWER_POLICY, enc, sizeof enc), -1);
    boot_flash(false);
    CHECK(P.policy.apo_standby);
    CHECK_EQ(P.sb.cap_ms, STANDBY_CAP_DEFAULT_MS);

    /* a damaged record falls back to the defaults */
    const uint8_t junk[POWER_POLICY_RAW] = {9, 0, 0, 0, 0, 0, 0, 0};
    kv_put(&KV, KV_KEY_POWER_POLICY, junk, sizeof junk);
    boot_flash(false);
    CHECK(P.policy.apo_standby);
    CHECK_EQ(P.policy.hold.max_hold_ms, APO_MAX_HOLD_DEFAULT_MS);
}

TEST(test_ng_standby_op)
{
    boot_flash(true);
    uint16_t n;
    const uint8_t q[] = {0x19, 0x00}, enter[] = {0x19, 0x01}, wake[] = {0x19, 0x02};
    const uint8_t *r = ng(MIDI_PORT_DIN, q, 2, &n);
    CHECK(r && n == 11 && r[6] == 0x59 && r[7] == 0x00 && r[8] == 0 && r[9] == 0);
    r = ng(MIDI_PORT_DIN, enter, 2, &n); /* unbound */
    CHECK(r && r[8] == 2 && r[9] == 0);
    r = ng(MIDI_PORT_USB, enter, 2, &n);
    CHECK(r && r[7] == 0x01 && r[8] == 0 && r[9] == 1);
    CHECK_EQ(P.sb.state, SB_STANDBY);
    r = ng(MIDI_PORT_USB, enter, 2, &n);
    CHECK(r && r[8] == 3); /* already in standby */
    /* panel touch (32 51 39 on the panel port) wakes */
    const uint8_t touch[] = {0xF0, 0x40, 0x7F, 0x32, 0x08, 0x02, 0x51, 0x39, 0x7F, 0x7F, 0xF7};
    piano_midi_in(&P, MIDI_PORT_PANEL, touch, sizeof touch);
    CHECK_EQ(P.sb.state, SB_WAKING);
    CHECK_EQ(P.sb.last_wake, SB_WAKE_PANEL_TOUCH);
    piano_tick(&P, STANDBY_WAKE_DELAY_MS);
    CHECK_EQ(P.sb.state, SB_ON);
    /* wake op from an unbound port */
    ng(MIDI_PORT_USB, enter, 2, &n);
    r = ng(MIDI_PORT_DIN, wake, 2, &n);
    CHECK(r && r[7] == 0x02 && r[8] == 0 && r[9] == 2);
    CHECK_EQ(P.sb.last_wake, SB_WAKE_BLE_OP);
    r = ng(MIDI_PORT_DIN, wake, 2, &n);
    CHECK(r && r[8] == 3);
}

/* remote off 19 03 (feature 2): bound only, refused while recording, then where the APO goes */
TEST(test_ng_remote_off_op)
{
    boot_flash(true);
    uint16_t n;
    const uint8_t off[] = {0x19, 0x03};
    const uint8_t *r = ng(MIDI_PORT_DIN, off, 2, &n); /* unbound */
    CHECK(r && n == 12 && r[6] == 0x59 && r[7] == 0x03 && r[8] == 2 && r[9] == 0 && r[10] == 0);
    recorder_standby(&P.rec, 0, 120, 4, 4);
    r = ng(MIDI_PORT_USB, off, 2, &n);
    CHECK(r && r[8] == 6 && r[9] == 0 && r[10] == POWER_OFF_RECORDING);
    recorder_start(&P.rec);
    r = ng(MIDI_PORT_USB, off, 2, &n);
    CHECK(r && r[8] == 6 && r[10] == POWER_OFF_RECORDING);
    CHECK_EQ(P.sb.state, SB_ON);
    recorder_stop(&P.rec);
    piano_tick(&P, 20);
    r = ng(MIDI_PORT_USB, off, 2, &n);
    CHECK(r && r[8] == 0 && r[9] == SB_STANDBY && r[10] == 0);
    CHECK_EQ(P.sb.state, SB_STANDBY);
    r = ng(MIDI_PORT_USB, off, 2, &n);
    CHECK(r && r[8] == 3);
    CHECK_EQ(power_remote_off_check(false, true, true), POWER_OFF_FILE);
    CHECK_EQ(power_remote_off_check(false, false, true), POWER_OFF_FLASH);
    CHECK_EQ(power_remote_off_check(true, true, true), POWER_OFF_RECORDING);
    CHECK_EQ(power_remote_off_check(false, false, false), POWER_OFF_OK);
}

/* APO test shortcut 19 04: the host core's bytes are the cave's; the APO goes where the policy says */
TEST(test_ng_apo_test_op)
{
    boot_flash(true);
    uint16_t n;
    const uint8_t t60[] = {0x19, 0x04, 0x00, 0x3C}, t5[] = {0x19, 0x04, 0x00, 0x05}, cancel[] = {0x19, 0x04, 0x00};
    const uint8_t *r = ng(MIDI_PORT_DIN, t60, sizeof t60, &n); /* unbound */
    CHECK(r && n == 15 && r[6] == 0x59 && r[7] == 0x04 && r[8] == 2);
    r = ng(MIDI_PORT_USB, t5, sizeof t5, &n);
    CHECK(r && r[8] == 1);
    r = ng(MIDI_PORT_USB, t60, sizeof t60, &n);
    const uint8_t want[] = {0x04, 0x00, 0x00, 0x00, 0x3C, 0x01, 0x00};
    CHECK(r && memcmp(r + 7, want, sizeof want) == 0);
    piano_tick(&P, 59000);
    CHECK_EQ(P.sb.state, SB_ON);
    piano_tick(&P, 1000);
    CHECK_EQ(P.sb.state, SB_STANDBY);
    piano_standby_wake(&P, SB_WAKE_BLE_OP);
    piano_tick(&P, STANDBY_WAKE_DELAY_MS);
    CHECK_EQ(P.sb.state, SB_ON);
    /* 19 04 00: the full menu time again */
    ng(MIDI_PORT_USB, t60, sizeof t60, &n);
    piano_tick(&P, 30000);
    r = ng(MIDI_PORT_USB, cancel, sizeof cancel, &n);
    CHECK(r && r[8] == 0 && r[10] == 0 && r[11] == 0);
    piano_tick(&P, 60000);
    CHECK_EQ(P.sb.state, SB_ON);
    /* APO Never: rc 7 */
    P.power.setting = 0;
    r = ng(MIDI_PORT_USB, t60, sizeof t60, &n);
    CHECK(r && r[8] == 7 && r[12] == 0);
}

/* TV wake: standby keeps the BT audio module on, only STREAMING START wakes */
TEST(test_standby_tv_wake)
{
    /* core: keep_bt skips the module, a START counted after the entry wakes once */
    sb_boot(STANDBY_CAP_DEFAULT_MS, false);
    SB.keep_bt = true;
    CHECK(!standby_bt_stream(&SB, 3)); /* ON: only records the count */
    CHECK(standby_enter(&SB));
    static const char *const ENTER[] = {"cleanup", "amp_off", "mute", "note_standby"};
    CHECK(log_is(ENTER, 4));
    CHECK(hal_power_sim.bt_audio);
    CHECK(!standby_bt_stream(&SB, 3)); /* the START seen before the standby */
    CHECK_EQ(SB.state, SB_STANDBY);
    CHECK(standby_bt_stream(&SB, 4));
    CHECK_EQ(SB.state, SB_WAKING);
    CHECK_EQ(SB.last_wake, SB_WAKE_BT_STREAM);
    hal_power_sim.n = 0;
    standby_tick(&SB, STANDBY_WAKE_DELAY_MS);
    static const char *const WAKE[] = {"headphones", "amp_on", "note_awake"};
    CHECK(log_is(WAKE, 3));
    CHECK(!standby_bt_stream(&SB, 5)); /* not in standby */
    /* without keep_bt (module switched off) a START never wakes */
    sb_boot(STANDBY_CAP_DEFAULT_MS, false);
    standby_enter(&SB);
    CHECK(!hal_power_sim.bt_audio);
    CHECK(!standby_bt_stream(&SB, 1));
    CHECK_EQ(SB.state, SB_STANDBY);

    /* piano: APO -> standby with the TV linked but silent; CONNECT / codec lines do not wake */
    boot_flash(true);
    piano_bt_link(&P, true);
    bt("CONNECT 0 A2DP 19");
    minutes(15);
    CHECK_EQ(P.sb.state, SB_STANDBY);
    CHECK(hal_power_sim.bt_audio && !hal_power_sim.amp);
    bt("NO CARRIER 0 ERROR 0");
    bt("CONNECT 1 A2DP 19");
    bt("A2DP CODEC SBC JOINT_STEREO 44100 BITPOOL 2-53");
    minutes(30);
    CHECK_EQ(P.sb.state, SB_STANDBY);
    /* the TV plays: wake, amp on, and the streaming hold keeps it on */
    bt("A2DP STREAMING START 1");
    piano_tick(&P, 20);
    CHECK_EQ(P.sb.state, SB_WAKING);
    CHECK_EQ(P.sb.last_wake, SB_WAKE_BT_STREAM);
    piano_tick(&P, STANDBY_WAKE_DELAY_MS);
    CHECK_EQ(P.sb.state, SB_ON);
    CHECK(hal_power_sim.amp && !hal_power_sim.mute && hal_power_sim.bt_audio);
    minutes(90);
    CHECK_EQ(P.sb.state, SB_ON);
    bt("A2DP STREAMING STOP 1");
    minutes(15);
    CHECK_EQ(P.sb.state, SB_STANDBY);

    /* policy flag bit 2: standby switches the module off, no stream wake */
    boot_flash(true);
    power_policy_t pol;
    power_policy_default(&pol);
    pol.standby_bt_off = true;
    uint8_t raw[POWER_POLICY_RAW];
    power_policy_encode(&pol, raw);
    CHECK_EQ(raw[1], POWER_FLAG_APO_STANDBY | POWER_FLAG_STANDBY_BT_OFF);
    uint16_t n;
    const uint8_t *r = ng_set(MIDI_PORT_USB, raw, &n);
    CHECK(r && r[8] == 0);
    minutes(15);
    CHECK_EQ(P.sb.state, SB_STANDBY);
    CHECK(!hal_power_sim.bt_audio);
    bt("A2DP STREAMING START 0");
    piano_tick(&P, 20);
    CHECK_EQ(P.sb.state, SB_STANDBY);
    /* the default record bytes stay those of ota_power (bit 2 clear = TV wake on) */
    power_policy_default(&pol);
    power_policy_encode(&pol, raw);
    CHECK_EQ(raw[1], POWER_FLAG_APO_STANDBY);
    raw[1] = POWER_FLAG_APO_STANDBY | POWER_FLAG_STANDBY_BT_OFF;
    CHECK(power_policy_decode(&pol, raw, sizeof raw) && pol.standby_bt_off);
    /* bit 3 (F-A1): the stock POWER button; default 0 = the button goes to standby */
    CHECK(!pol.button_stock);
    raw[1] = POWER_FLAG_APO_STANDBY | POWER_FLAG_BUTTON_STOCK;
    CHECK(power_policy_decode(&pol, raw, sizeof raw) && pol.button_stock && !pol.standby_bt_off);
    power_policy_encode(&pol, raw);
    CHECK_EQ(raw[1], POWER_FLAG_APO_STANDBY | POWER_FLAG_BUTTON_STOCK);
}

/* "No cap" (owner decision 2026-11-01): cap u16 0xFFFF = the
 * standby lasts until something wakes the piano - never the real off. */
TEST(test_standby_no_cap)
{
    /* codec: 0xFFFF <-> standby_cap_ms 0 (standby_t's "no cap"); 0 itself stays a bad value */
    power_policy_t pol, y;
    power_policy_default(&pol);
    uint8_t raw[POWER_POLICY_RAW];
    pol.standby_cap_ms = 0;
    power_policy_encode(&pol, raw);
    const uint8_t want[POWER_POLICY_RAW] = {1, 0x01, 0x2C, 0x01, 0xFF, 0xFF, 0, 0};
    CHECK_MEM(raw, want, sizeof want);
    power_policy_default(&y);
    CHECK(power_policy_decode(&y, raw, sizeof raw));
    CHECK_EQ(y.standby_cap_ms, 0);
    raw[4] = 0xFE; /* 65534 min: neither a range value nor 0xFFFF */
    y.standby_cap_ms = 7;
    CHECK(!power_policy_decode(&y, raw, sizeof raw));
    CHECK_EQ(y.standby_cap_ms, 7);

    /* set over op 18 01 from the bound controller: rc 0, applied, saved as 0xFFFF */
    boot_flash(true);
    uint16_t n;
    const uint8_t *r = ng_set(MIDI_PORT_USB, want, &n);
    CHECK(r && r[7] == 0x01 && r[8] == 0);
    CHECK_EQ(P.policy.standby_cap_ms, 0);
    CHECK_EQ(P.sb.cap_ms, 0);
    uint8_t saved[POWER_POLICY_RAW];
    CHECK_EQ(kv_get(&KV, KV_KEY_POWER_POLICY, saved, sizeof saved), POWER_POLICY_RAW);
    CHECK_MEM(saved, want, sizeof want);

    /* power cycle: still no cap, 18 00 answers 0xFFFF */
    boot_flash(false);
    CHECK_EQ(P.sb.cap_ms, 0);
    const uint8_t get[] = {0x18, 0x00};
    r = ng(MIDI_PORT_DIN, get, 2, &n);
    CHECK(r && n == 20 && r[8] == 0);
    uint8_t enc[POWER_POLICY_RAW];
    power_policy_encode(&P.policy, enc);
    CHECK_MEM(enc, want, sizeof want);

    /* APO -> standby, then 7 days (virtual) of standby: never off, never 7E 00 00 */
    minutes(15);
    CHECK_EQ(P.sb.state, SB_STANDBY);
    for (int m = 0; m < 7 * 24 * 60; m++) { piano_tick(&P, MIN_MS); ack_drain(); }
    CHECK_EQ(P.sb.state, SB_STANDBY);
    CHECK_EQ(o.power_offs, 0);
    CHECK_EQ(o.tx_7e_00_00, 0);
    CHECK(P.powered);
    CHECK_EQ(P.sb.t_ms, 7u * 24u * 60u * MIN_MS);
    /* past the u32 ms range (49.7 days): the state timer saturates, still standby */
    for (int h = 0; h < 60 * 24; h++) { piano_tick(&P, 60u * MIN_MS); ack_drain(); }
    CHECK_EQ(P.sb.state, SB_STANDBY);
    CHECK_EQ(P.sb.t_ms, UINT32_MAX);
    CHECK_EQ(o.power_offs, 0);
    /* the app wakes it (19 02 answers on any port) */
    const uint8_t wake[] = {0x19, 0x02};
    r = ng(MIDI_PORT_DIN, wake, 2, &n);
    CHECK(r && r[8] == 0);
    piano_tick(&P, STANDBY_WAKE_DELAY_MS);
    CHECK_EQ(P.sb.state, SB_ON);
    CHECK(hal_power_sim.amp && !hal_power_sim.mute);

    /* back to a cap: the old behaviour (60 min standby, then the real off) */
    const uint8_t hour[POWER_POLICY_RAW] = {1, 0x01, 120, 0, 60, 0, 0, 0};
    r = ng_set(MIDI_PORT_USB, hour, &n);
    CHECK(r && r[8] == 0);
    CHECK_EQ(P.sb.cap_ms, 60u * MIN_MS);
    minutes(15);
    CHECK_EQ(P.sb.state, SB_STANDBY);
    for (int m = 0; m < 59; m++) { piano_tick(&P, MIN_MS); ack_drain(); }
    CHECK_EQ(o.power_offs, 0);
    piano_tick(&P, MIN_MS);
    CHECK_EQ(o.power_offs, 1);
}

void suite_power(void)
{
    puts("power:");
    RUN(test_bt_audio_lines);
    RUN(test_bt_audio_rx_and_link_flag);
    RUN(test_apo_streaming_keeps_piano_on);
    RUN(test_apo_tv_pause_falls_back_to_max_hold);
    RUN(test_apo_hold_follows_the_play_status);
    RUN(test_apo_no_carrier_resumes_countdown);
    RUN(test_apo_policy_options);
    RUN(test_standby_enter_wake_order);
    RUN(test_standby_wake_sources);
    RUN(test_standby_hard_cap);
    RUN(test_standby_flash_busy_defers_power_off);
    RUN(test_hal_power_stubs);
    RUN(test_power_policy_codec);
    RUN(test_apo_expiry_enters_standby);
    RUN(test_standby_notice_to_every_controller_but_the_panel);
    RUN(test_ng_power_policy_op);
    RUN(test_power_policy_persists_across_reboot);
    RUN(test_ng_standby_op);
    RUN(test_ng_remote_off_op);
    RUN(test_ng_apo_test_op);
    RUN(test_standby_tv_wake);
    RUN(test_standby_no_cap);
}
