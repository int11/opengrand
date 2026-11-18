/* Tests for the BT audio source manager (src/bt/btsrc.c): the module's link / stream lines, the policy and the
 * only commands it can form.  The line texts are the field ones of 2026-11-01 (iWRAP 6.2.0 LIST output of the
 * TV link; phone and PC MACs). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ca79ng/btsrc.h"
#include "test.h"

static const uint8_t TV[6] = {0x12, 0x84, 0x04, 0x04, 0x14, 0x24};
static const uint8_t PHONE[6] = {0x62, 0x81, 0x81, 0x81, 0x11, 0xa1};
static const uint8_t PC[6] = {0x22, 0x82, 0x02, 0x02, 0x92, 0x22};

static char sent[48][BTS_OUT_MAX];
static int nsent, nreads;

static int echo_on;      /* run(): the module echoes our `A2DP STREAMING START|STOP` (no link id) - field 2026-11-10 */
static int trace = -1;   /* BTS_TRACE=1: every model line / command with the model time (debugging a scenario) */
static uint32_t trace_now(void);

static void line(bts_t *b, const char *s)
{
    if (trace < 0)
        trace = getenv("BTS_TRACE") != NULL;
    if (trace)
        printf("      %7u <- %s\n", (unsigned)trace_now(), s);
    bts_line(b, s, (uint32_t)strlen(s));
}

/* time passes; every command the manager hands out is recorded - except the configuration read `SET` (a read: the
 * manager reads AUTOCALL once it has someone to reconnect), counted in nreads */
static void run(bts_t *b, uint32_t ms, bool can_act)
{
    for (uint32_t t = 0; t < ms; t += 8) {
        char c[BTS_OUT_MAX];
        bts_tick(b, 8, can_act);
        if (bts_next(b, c, sizeof c)) {
            if (!strcmp(c, "SET"))
                nreads++;
            else if (nsent < 48)
                strcpy(sent[nsent++], c);
            if (trace > 0)
                printf("              -> %s\n", c);
            if (echo_on && !strncmp(c, "A2DP STREAMING ST", 17))
                line(b, c);
        }
    }
}

static int count_sent(const char *prefix)
{
    int n = 0;
    for (int i = 0; i < nsent; i++)
        n += strncmp(sent[i], prefix, strlen(prefix)) == 0;
    return n;
}

/* CALLs by profile: `… 19 A2DP` (a source's sound link) / `… 17 AVRCP` (the sink opens remote control) */
static int calls_of(const char *prof)
{
    int n = 0;
    for (int i = 0; i < nsent; i++)
        n += strncmp(sent[i], "CALL ", 5) == 0 && strstr(sent[i], prof) != NULL;
    return n;
}

static bool no_set_sent(void)
{
    for (int i = 0; i < nsent; i++)
        if (strncmp(sent[i], "SET", 3) == 0)
            return false;
    return true;
}

static int dev_of(const bts_t *b, const uint8_t a[6])
{
    for (int i = 0; i < BTS_DEV_MAX; i++)
        if (b->dev[i].used && b->dev[i].addr_known && memcmp(b->dev[i].addr, a, 6) == 0)
            return i;
    return -1;
}

static void fresh(bts_t *b)
{
    nsent = 0;
    nreads = 0;
    memset(sent, 0, sizeof sent);
    bts_init(b, 0);
    b->pin = BTS_PIN_DONE;   /* tests written before the power-on pin: no pin (its own tests) */
    bts_module(b, true);
    /* the tests written up to c0b89c60 check the switch / guard mechanisms with the sink stopping a source that has
     * AVRCP; the default since 2026-11-10 stops none - those tests: susp_ok 0 */
    b->susp_ok = 1;
}

/* the opt-in "newest wins" policy (the default before the 2026-11-01 evening decision): mode NEWEST, flags 0x0D */
static void newest(bts_policy_t *p)
{
    p->mode = BTS_MODE_NEWEST;
    p->flags = BTS_F_CLOSE_OTHERS | BTS_F_KEEP_PREF | BTS_F_RECOVER;
}

static void fresh_newest(bts_t *b)
{
    bts_policy_t p;
    bts_policy_default(&p);
    newest(&p);
    nsent = 0;
    memset(sent, 0, sizeof sent);
    bts_init(b, &p);
    b->pin = BTS_PIN_DONE;   /* tests written before the power-on pin: no pin (its own tests) */
    bts_module(b, true);
}

/* every command is one of the run-time forms: LIST, CLOSE <l>, CALL <addr> 19 A2DP, SET <l> SELECT, AVRCP PLAY|PAUSE */
static bool only_runtime_sent(void)
{
    for (int i = 0; i < nsent; i++) {
        const char *c = sent[i];
        int l;
        char tail[16];
        if (!strcmp(c, "LIST") || !strcmp(c, "AVRCP PLAY") || !strcmp(c, "AVRCP PAUSE") || !strcmp(c, "A2DP STREAMING STOP") ||
            !strcmp(c, "A2DP STREAMING START") || !strcmp(c, "AVRCP PDU 31 1"))
            continue;
        if (!strncmp(c, "CALL ", 5) && strlen(c) == 31 && !strcmp(c + 22, " 17 AVRCP"))
            continue;
        if (sscanf(c, "CLOSE %d%15s", &l, tail) == 1 && l >= 0 && l < BTS_LINKS)
            continue;
        if (!strncmp(c, "CALL ", 5) && strlen(c) == 30 && !strcmp(c + 22, " 19 A2DP"))
            continue;
        if (sscanf(c, "SET %d %15s", &l, tail) == 2 && l >= 0 && l < BTS_LINKS && !strcmp(tail, "SELECT") &&
            strlen(c) == (size_t)(l >= 10 ? 13 : 12))
            continue;
        return false;
    }
    return true;
}

/* the field LIST of 2026-11-01 (TV linked by AUTOCALL, OUTGOING) builds one device with 4 links, 2 of them A2DP */
static void test_btsrc_list_field(void)
{
    bts_t b;
    fresh(&b);
    line(&b, "LIST");   /* the echo */
    line(&b, "LIST 4");
    line(&b, "LIST 0 CONNECTED A2DP 1024 0 0 24 0 0 12:84:04:04:14:24 19 OUTGOING ACTIVE SLAVE ENCRYPTED 0");
    line(&b, "LIST 1 CONNECTED A2DP 1024 0 0 24 0 0 12:84:04:04:14:24 19 OUTGOING ACTIVE SLAVE ENCRYPTED 0");
    line(&b, "LIST 2 CONNECTED AVRCP 512 0 0 21 0 0 12:84:04:04:14:24 17 INCOMING ACTIVE SLAVE ENCRYPTED 0");
    line(&b, "LIST 3 CONNECTED AVRCP 1008 0 0 21 0 0 12:84:04:04:14:24 1b INCOMING ACTIVE SLAVE ENCRYPTED 0");
    int d = dev_of(&b, TV);
    CHECK(d >= 0);
    CHECK_EQ(b.dev[d].links, 0x0F);
    CHECK_EQ(b.dev[d].a2dp, 0x03);
    CHECK_EQ(b.dev[d].incoming, 0);
    CHECK_EQ(bts_a2dp_links(&b), 0x03);
    CHECK_EQ(b.listing, 0);
    /* a second LIST without link 1-3: they are gone */
    line(&b, "LIST 1");
    line(&b, "LIST 0 CONNECTED A2DP 1024 0 0 24 0 0 12:84:04:04:14:24 19 OUTGOING ACTIVE SLAVE ENCRYPTED 0");
    CHECK_EQ(b.dev[d].links, 0x01);
    line(&b, "LIST 0");
    CHECK_EQ(bts_a2dp_links(&b), 0);
    CHECK(dev_of(&b, TV) < 0);
}

/* the field case: phone streaming, PC connects and starts streaming too -> the module plays nothing.  The newest
 * stream wins after hold_ds; the phone's links are closed (A2DP first), nothing else, never SET. */
static void test_btsrc_two_streams(void)
{
    bts_t b;
    fresh_newest(&b);
    line(&b, "RING 0 62:81:81:81:11:a1 19 A2DP");
    line(&b, "RING 1 62:81:81:81:11:a1 19 A2DP");
    line(&b, "RING 2 62:81:81:81:11:a1 17 AVRCP");
    line(&b, "A2DP STREAMING START 1");
    run(&b, 10000, true);
    CHECK_EQ(nsent, 0);                       /* one source: nothing to do */
    CHECK_EQ(bts_streams(&b), 1);
    line(&b, "RING 3 22:82:02:02:92:22 19 A2DP");
    line(&b, "RING 4 22:82:02:02:92:22 19 A2DP");
    line(&b, "RING 5 22:82:02:02:92:22 17 AVRCP");
    run(&b, 3000, true);
    CHECK_EQ(nsent, 0);                       /* a source that only connects is left alone */
    line(&b, "A2DP STREAMING START 4");
    CHECK_EQ(bts_streams(&b), 2);
    run(&b, 1000, true);
    CHECK_EQ(nsent, 0);                       /* hold 1.5 s */
    run(&b, 1000, true);
    CHECK_EQ(nsent, 3);
    CHECK(!strcmp(sent[0], "CLOSE 0") || !strcmp(sent[0], "CLOSE 1"));
    CHECK_EQ(count_sent("CLOSE 0"), 1);
    CHECK_EQ(count_sent("CLOSE 1"), 1);
    CHECK(!strcmp(sent[2], "CLOSE 2"));       /* AVRCP after the A2DP links */
    CHECK(no_set_sent());
    CHECK_EQ(b.dev[dev_of(&b, PC)].streaming, 1);
    CHECK_EQ(bts_dev_state(&b, dev_of(&b, PC)) & BTS_D_ACTIVE, BTS_D_ACTIVE);
    CHECK_EQ(bts_dev_state(&b, dev_of(&b, PHONE)) & BTS_D_CLOSING, BTS_D_CLOSING);
    line(&b, "NO CARRIER 0 ERROR 0");
    line(&b, "NO CARRIER 1 ERROR 0");
    line(&b, "NO CARRIER 2 ERROR 0");
    CHECK(dev_of(&b, PHONE) < 0);
    CHECK_EQ(bts_streams(&b), 1);
    run(&b, 10000, true);
    CHECK_EQ(nsent, 3);                        /* stable */
    CHECK_EQ(b.n.closes, 1);
}

/* a TV that the policy closed comes back (AUTOCALL / its own) and streams silence: it does not cut off the phone */
static void test_btsrc_no_ping_pong(void)
{
    bts_t b;
    fresh_newest(&b);
    line(&b, "CONNECT 0 A2DP 19");               /* AUTOCALL: OUTGOING, no address yet */
    line(&b, "A2DP STREAMING START 0");
    run(&b, 400, true);
    CHECK_EQ(count_sent("LIST"), 1);             /* learns the address */
    line(&b, "LIST 1");
    line(&b, "LIST 0 CONNECTED A2DP 1024 0 0 24 0 0 12:84:04:04:14:24 19 OUTGOING ACTIVE SLAVE ENCRYPTED 0");
    CHECK(dev_of(&b, TV) >= 0);
    CHECK_EQ(b.dev[dev_of(&b, TV)].streaming, 1); /* the placeholder's stream moved to the TV */
    line(&b, "RING 1 62:81:81:81:11:a1 19 A2DP");
    line(&b, "A2DP STREAMING START 1");
    run(&b, 2000, true);
    CHECK_EQ(count_sent("CLOSE 0"), 1);           /* the phone (newest) wins */
    line(&b, "NO CARRIER 0 ERROR 0");
    run(&b, 5000, true);
    line(&b, "CONNECT 2 A2DP 19");               /* the TV is back ... */
    line(&b, "LIST 2");
    line(&b, "LIST 1 CONNECTED A2DP 1024 0 0 24 0 0 62:81:81:81:11:a1 19 INCOMING ACTIVE MASTER ENCRYPTED 0");
    line(&b, "LIST 2 CONNECTED A2DP 1024 0 0 24 0 0 12:84:04:04:14:24 19 OUTGOING ACTIVE SLAVE ENCRYPTED 0");
    line(&b, "A2DP STREAMING START 2");          /* ... and streams (silence) */
    run(&b, 3000, true);
    CHECK_EQ(count_sent("CLOSE 1"), 0);           /* the phone is not cut off */
    CHECK_EQ(count_sent("CLOSE 2"), 1);           /* the returning TV is closed again (two streams = silence) */
    CHECK(no_set_sent());
}

/* CLOSE_OTHERS: after 5 s of streaming the sources connected at that moment are closed once; KEEP_PREF spares the
 * preferred TV; a source closed once is not closed again within 10 min */
static void test_btsrc_sweep(void)
{
    bts_t b;
    bts_policy_t p;
    bts_policy_default(&p);
    newest(&p);
    p.has_pref = 1;
    memcpy(p.pref, TV, 6);
    nsent = 0;
    bts_init(&b, &p);
    b.pin = BTS_PIN_DONE;   /* tests written before the power-on pin: no pin (its own tests) */
    bts_module(&b, true);
    line(&b, "RING 0 12:84:04:04:14:24 19 A2DP");
    line(&b, "RING 1 22:82:02:02:92:22 19 A2DP");
    line(&b, "RING 2 62:81:81:81:11:a1 19 A2DP");
    line(&b, "A2DP STREAMING START 2");
    run(&b, 4000, true);
    CHECK_EQ(nsent, 0);
    run(&b, 1200, true);
    CHECK_EQ(count_sent("CLOSE 1"), 1);           /* the PC */
    CHECK_EQ(count_sent("CLOSE 0"), 0);           /* the preferred TV is kept */
    CHECK_EQ(bts_dev_state(&b, dev_of(&b, TV)) & BTS_D_PREF, BTS_D_PREF);
    line(&b, "NO CARRIER 1 ERROR 0");
    line(&b, "RING 3 22:82:02:02:92:22 19 A2DP");   /* the PC reconnects */
    line(&b, "A2DP STREAMING STOP 2");
    line(&b, "A2DP STREAMING START 2");
    run(&b, 6000, true);
    CHECK_EQ(count_sent("CLOSE 3"), 0);           /* backoff */
    /* off: nothing at all */
    fresh(&b);
    b.pol.mode = BTS_MODE_OFF;
    line(&b, "RING 0 62:81:81:81:11:a1 19 A2DP");
    line(&b, "RING 1 22:82:02:02:92:22 19 A2DP");
    line(&b, "A2DP STREAMING START 0");
    line(&b, "A2DP STREAMING START 1");
    run(&b, 8000, true);
    CHECK_EQ(nsent, 0);
}

/* standby / module off: no command; a module restart (power edge, READY.) forgets every link */
static void test_btsrc_standby(void)
{
    bts_t b;
    fresh(&b);
    line(&b, "RING 0 62:81:81:81:11:a1 19 A2DP");
    line(&b, "RING 1 22:82:02:02:92:22 19 A2DP");
    line(&b, "A2DP STREAMING START 0");
    line(&b, "A2DP STREAMING START 1");
    run(&b, 5000, false);                          /* standby (can_act 0): nothing goes out */
    CHECK_EQ(nsent, 0);
    bts_module(&b, false);
    CHECK_EQ(bts_a2dp_links(&b), 0);
    run(&b, 1000, true);
    CHECK_EQ(nsent, 0);
    bts_module(&b, true);
    line(&b, "RING 0 62:81:81:81:11:a1 19 A2DP");
    line(&b, "READY.");
    CHECK_EQ(bts_a2dp_links(&b), 0);
}

/* CALL_PREF: after a module start nobody came within 4 s -> CALL the preferred TV, twice at most */
static void test_btsrc_call_pref(void)
{
    bts_t b;
    bts_policy_t p;
    bts_policy_default(&p);
    newest(&p);
    p.flags |= BTS_F_CALL_PREF;
    p.has_pref = 1;
    memcpy(p.pref, TV, 6);
    nsent = 0;
    bts_init(&b, &p);
    b.pin = BTS_PIN_DONE;   /* tests written before the power-on pin: no pin (its own tests) */
    bts_module(&b, true);
    run(&b, 3800, true);
    CHECK_EQ(nsent, 0);
    run(&b, 400, true);
    CHECK_EQ(nsent, 1);
    CHECK(!strcmp(sent[0], "CALL 12:84:04:04:14:24 19 A2DP"));
    run(&b, 60000, true);
    CHECK_EQ(count_sent("CALL"), 2);
    /* somebody connected: no call */
    nsent = 0;
    bts_init(&b, &p);
    b.pin = BTS_PIN_DONE;   /* tests written before the power-on pin: no pin (its own tests) */
    bts_module(&b, true);
    line(&b, "RING 0 62:81:81:81:11:a1 19 A2DP");
    run(&b, 20000, true);
    CHECK_EQ(count_sent("CALL"), 0);
    /* policy check */
    p.hold_ds = 0;
    CHECK(bts_policy_check(&p) != 0);
}

/* RECOVER: two streams that do not end and a module that answers nothing -> one restart request a minute */
static void test_btsrc_recover(void)
{
    bts_t b;
    fresh_newest(&b);
    line(&b, "RING 0 62:81:81:81:11:a1 19 A2DP");
    line(&b, "RING 1 22:82:02:02:92:22 19 A2DP");
    line(&b, "A2DP STREAMING START 0");
    line(&b, "A2DP STREAMING START 1");
    bool req = false;
    for (int i = 0; i < 2000 && !req; i++) {     /* CLOSE 0 goes out (twice), no line comes back */
        run(&b, 8, true);
        req = bts_take_recover(&b);
    }
    CHECK(req);
    CHECK(b.n.unanswered >= 2);
    CHECK_EQ(b.n.recoveries, 1);
    run(&b, 10000, true);
    CHECK(!bts_take_recover(&b));                 /* not again within a minute */
    /* manual actions */
    fresh(&b);
    line(&b, "RING 0 62:81:81:81:11:a1 19 A2DP");
    line(&b, "RING 1 22:82:02:02:92:22 19 A2DP");
    CHECK_EQ(bts_select(&b, PC), BTS_OK);
    run(&b, 400, true);
    CHECK_EQ(count_sent("CLOSE 0"), 1);
    CHECK_EQ(bts_close(&b, TV), BTS_UNKNOWN);
    CHECK_EQ(bts_call(&b, TV), BTS_OK);
    run(&b, 400, true);
    CHECK_EQ(count_sent("CALL 12:84:04:04:14:24"), 1);
    CHECK(no_set_sent());
}

/* ---- a module model from the 2026-11-02 field log -------------------------------------
 * iWRAP 6.2.0 as seen: one A2DP stream at a time (AN986 3.5.1) - a second source's start is silently ignored (no line)
 * while another stream is open; AVRCP PAUSE: the phone stops its stream ~0.3 s later, Windows never does (it keeps
 * the stream started); A2DP STREAMING STOP from the sink: honoured or not (per device); CLOSE of the A2DP signalling
 * link takes the media link with it (AN986 3.6.1); CALL <addr> 19 A2DP -> CALL <l>, CONNECT <l> / <l+1> A2DP 19
 * (AN986 3.5.1); AUTOCALL on: all A2DP gone -> 7 s later the module links the PC again OUTGOING and it streams. */
enum { M_PC = 0, M_PHONE = 1, M_TV = 2, M_N = 3 };
typedef struct {
    const char *mac;
    int a2dp, avrcp;          /* link ids (-1 none); a2dp + 1 is the media link */
    int streaming;
    int stop_on_pause;        /* AVRCP PAUSE -> STREAMING STOP */
    int honor_suspend;        /* A2DP STREAMING STOP from the sink -> STREAMING STOP */
    int restart_after;        /* ms: a suspended / paused source starts again by itself after this (0 never) */
    int stream_on_link;       /* the TV: streams (silence) as soon as its A2DP link is up */
    int no_avrcp;             /* opens no AVRCP link by itself (Windows, field 2026-11-02 #2) */
    int honor_start;          /* A2DP STREAMING START from the sink -> it streams (Windows, [inference]) */
    int browse_ms;            /* opens the AVRCP browsing channel (0x1b) ALONE this long after its A2DP (0 never) - the
                               * module reboots when no control channel is up (field 2026-11-02 #1) */
    int browse;               /* its browsing link id, -1 */
    int ignore_play;          /* AVRCP PLAY does nothing (no player behind it [unconfirmed]) */
    int silent_after_stop;    /* Windows (field 2026-11-02 evening): after a sink STOP every later stream on that A2DP
                               * connection is silent though the module shows STREAMING START - until a new connection */
    int sink_stopped, silent; /* state: the sink stopped it on this connection; the current stream is silent */
    int silent_when_paused;   /* Windows (field 2026-11-02 afternoon): a fresh A2DP link streams at once even while the
                               * player is paused - silence until AVRCP PLAY */
    int paused;               /* state: its player is paused (AVRCP PAUSE; PLAY clears it) */
    int avrcp_drop_on_close;  /* the phone (field 2026-11-02 afternoon): CLOSE of its A2DP signalling link drops its AVRCP */
    int avrcp_back_ms;        /* ... a new A2DP link (sink CALL): it opens AVRCP again by itself this much later (0 never) */
    int link_start_ms;        /* stream_on_link: its stream this long after the sink's CALL (0: 2000 ms); field 2026-11-02
                               * afternoon: CALL -> CONNECT -> START 0.72 s */
    /* play status and the player (2026-11-02 night) */
    int toggle;               /* Windows (field 2026-11-02 night): AVRCP PLAY and PAUSE are both play/pause */
    int ps_rep;               /* answers `AVRCP PDU 31 1` (INTERIM + CHANGED, RM 6.10.2 lines); 0 = no answer */
    int ps_reg;               /* state: registered (one CHANGED left) */
    int retry_free_ms;        /* its player plays but the module ignored its start: it starts this long after the stream
                               * slot frees (field 2026-11-02 night: the PC 0.16 s after the phone's CLOSE; the phone
                               * [inference]) */
    int waiting_free;         /* state */
    int ps_queries;           /* registrations it got */
    int blip_when_paused;     /* the phone (field 2026-11-03 #2): a fresh A2DP link (sink CALL)
                               * while its player is paused: `A2DP STREAMING START` then `STOP` 10 ms later (link_start_ms
                               * after the CALL); playing: it streams */
    int breaks_on_close;      /* Windows + Chrome (field 2026-11-03): a CLOSE of its A2DP removes
                               * the "CA79 Audio" endpoint - Chrome's audio renderer breaks; every later stream silent */
    int broken;               /* state */
    int close_after_ms;       /* the TV the user disconnected in its own settings (field 2026-11-09): it accepts the sink's
                               * CALL (CONNECT l / l+1) and closes both A2DP links by itself this long after (0 never) */
    int pause_stop_ms;        /* stop_on_pause: its STOP this long after AVRCP PAUSE (0: 300 ms; field 2026-11-03 ~3 s) */
    int sink_starts;          /* `A2DP STREAMING START` from the sink on its link */
} mdev_t;
typedef struct {
    uint32_t at;
    int kind;                 /* 0 line, 1 try_start dev, 3 reboot, 4 browsing channel, 6 AVRCP back, 7 blip */
    int dev;
    char text[96];
} mev_t;
static struct {
    bts_t *b;
    mdev_t d[M_N];
    int sel, autocall, next_link, nev, ignored, reboots, silent_starts;
    int direct_switch;        /* the module on a sink `A2DP STREAMING START` for a link while another stream is open: 0
                               * ignores it (as a source's own start - one stream, AN986 3.5.1), 1 stops the open stream
                               * and starts this one [unknown - the docs say nothing] */
    int closes;               /* CLOSE <l> of any A2DP link (field 2026-11-03: never as part of a switch) */
    int plays_to[M_N + 1];    /* AVRCP PLAY per device (M_N: a link of nobody) */
    uint32_t now, all_gone_at;
    mev_t ev[64];
    char ac_line[40];
    unsigned cod;             /* the module's class of device (`SET BT CLASS`, stored in the module) */
    unsigned avf;             /* the module's AVRCP controller features (`SET PROFILE AVRCP CONTROLLER`, stored) */
    int avf_form;             /* how the module lists AVRCP: 0 controller-only, 1 + TARGET, 2 no line (profile off) */
    unsigned aud;             /* the module's `SET CONTROL AUDIO INTERNAL I2S_SLAVE` flags (BTS_AUD_*, stored) */
    int aud_form;             /* how it lists it: 0 the plain form, 1 another (`… I2S_SLAVE EVENT 32`), 2 no line */
    int aud_writes;           /* `SET CONTROL AUDIO …` lines received */
    int refuse;               /* the module does not take a setting write (lists the old value) pin tests */
    int revert;               /* ... takes it but lists the factory values again after a restart */
} M;

static uint32_t trace_now(void) { return M.now; }

static void m_at(uint32_t dt, int kind, int dev, const char *text)
{
    if (M.nev < 64) {
        mev_t *e = &M.ev[M.nev++];
        e->at = M.now + dt;
        e->kind = kind;
        e->dev = dev;
        snprintf(e->text, sizeof e->text, "%s", text ? text : "");
    }
}

static int m_by_link(int l)
{
    for (int i = 0; i < M_N; i++)
        if (l >= 0 && ((M.d[i].a2dp >= 0 && (M.d[i].a2dp == l || M.d[i].a2dp + 1 == l)) || M.d[i].avrcp == l ||
                       M.d[i].browse == l))
            return i;
    return -1;
}

static int m_streams(void)
{
    int n = 0;
    for (int i = 0; i < M_N; i++)
        n += M.d[i].streaming;
    return n;
}

static void m_try_start(int i)
{
    char s[40];
    if (M.d[i].a2dp < 0 || M.d[i].streaming)
        return;
    if (m_streams()) {   /* one stream at a time: silently ignored (field 2026-11-01 G3-G6, 2026-11-02 #2) */
        M.ignored++;
        M.d[i].waiting_free = M.d[i].retry_free_ms > 0;
        return;
    }
    M.d[i].streaming = 1;
    M.d[i].silent = (M.d[i].sink_stopped && M.d[i].silent_after_stop) || (M.d[i].paused && M.d[i].silent_when_paused) ||
                    M.d[i].broken;
    M.silent_starts += M.d[i].silent;
    if (M.aud & BTS_AUD_EVENT) {
        /* RM 6.67 EVENT: the codec loaded into the DSP and the audio routing changed, as the stream starts [inference:
         * order and form from the RM text - the 11-11 field test confirms it] */
        line(M.b, i == M_PHONE ? "A2DP CODEC APT-X JOINT_STEREO 44100" : "A2DP CODEC SBC JOINT_STEREO 44100 BITPOOL 2-53");
        snprintf(s, sizeof s, "AUDIO ROUTE %d A2DP LEFT RIGHT", M.d[i].a2dp);
        line(M.b, s);
    }
    snprintf(s, sizeof s, "A2DP STREAMING START %d", M.d[i].a2dp);
    line(M.b, s);
}

static void m_stop(int i)
{
    char s[40];
    if (!M.d[i].streaming)
        return;
    M.d[i].streaming = 0;
    M.d[i].silent = 0;
    snprintf(s, sizeof s, "A2DP STREAMING STOP %d", M.d[i].a2dp);
    line(M.b, s);
    if (M.d[i].restart_after)
        m_at((uint32_t)M.d[i].restart_after, 1, i, 0);
}

/* its player changes: a registered target sends CHANGED (and is unregistered - AN986 4.3 "triggered only once") */
static void m_player(int i, int paused)
{
    char s[96];
    if (M.d[i].paused == paused)
        return;
    M.d[i].paused = paused;
    if (paused)
        M.d[i].waiting_free = 0;
    if (M.d[i].ps_reg && M.d[i].avrcp >= 0) {
        M.d[i].ps_reg = 0;
        snprintf(s, sizeof s, "AVRCP %d REGISTER_NOTIFICATION_RSP CHANGED PLAYBACK_STATUS_CHANGED %s", M.d[i].avrcp,
                 paused ? "PAUSED" : "PLAYING");
        m_at(40, 0, -1, s);
    }
}

/* the user presses play on the device */
static void m_play(int i)
{
    m_player(i, 0);
    m_try_start(i);
}

/* the module's link lines for a source that connects by itself (RING, INCOMING) */
static void m_connect_in(int i)
{
    char s[64];
    M.d[i].a2dp = M.next_link;
    M.d[i].sink_stopped = 0;   /* a new A2DP connection */
    M.d[i].avrcp = M.d[i].no_avrcp ? -1 : M.next_link + 2;
    M.next_link += 3;
    snprintf(s, sizeof s, "RING %d %s 19 A2DP", M.d[i].a2dp, M.d[i].mac);
    line(M.b, s);
    snprintf(s, sizeof s, "RING %d %s 19 A2DP", M.d[i].a2dp + 1, M.d[i].mac);
    line(M.b, s);
    if (M.d[i].avrcp >= 0) {
        snprintf(s, sizeof s, "RING %d %s 17 AVRCP", M.d[i].avrcp, M.d[i].mac);
        line(M.b, s);
    }
    if (M.d[i].browse_ms)
        m_at((uint32_t)M.d[i].browse_ms, 4, i, 0);
}

/* the iWRAP reboot (field 2026-11-02 #1): every link is gone without NO CARRIER, the banner, READY. */
static void m_reboot(void)
{
    for (int i = 0; i < M_N; i++) {
        M.d[i].a2dp = M.d[i].avrcp = M.d[i].browse = -1;
        M.d[i].streaming = M.d[i].sink_stopped = M.d[i].ps_reg = 0;
    }
    M.next_link = 0;
    M.nev = 0;
    M.reboots++;
    line(M.b, "WRAP THOR AI (6.2.0 build 1135)");
    line(M.b, "Copyright (c) 2003-2017 Silicon Labs Inc.");
    line(M.b, "READY.");
}

/* the source opens the AVRCP browsing channel by itself; no control channel up -> the module reboots ~0.9 s later */
static void m_browse(int i)
{
    char s[64];
    if (M.d[i].a2dp < 0 || M.d[i].browse >= 0)
        return;
    M.d[i].browse = M.next_link++;
    snprintf(s, sizeof s, "RING %d %s 1b AVRCP", M.d[i].browse, M.d[i].mac);
    line(M.b, s);
    if (M.d[i].avrcp < 0) {
        line(M.b, "AVRCP 5103 ERROR_NO_CONTROL_CHANNEL");
        m_at(900, 3, -1, 0);
    }
}

static void m_drop(int i)   /* every link of the source goes (out of range, the user ...) */
{
    char s[48];
    int ls[4] = {M.d[i].a2dp, M.d[i].a2dp + 1, M.d[i].avrcp, M.d[i].browse};
    if (M.d[i].a2dp < 0 && M.d[i].avrcp < 0)
        return;
    M.d[i].streaming = 0;
    for (int k = 0; k < 4; k++)
        if (ls[k] >= 0 && (k < 2 ? M.d[i].a2dp >= 0 : 1)) {
            snprintf(s, sizeof s, "NO CARRIER %d ERROR 0", ls[k]);
            line(M.b, s);
        }
    M.d[i].a2dp = M.d[i].avrcp = M.d[i].browse = -1;
    M.d[i].ps_reg = 0;
}

/* every link of the source is lost (out of range): NO CARRIER with a code other than 0 [the code is the model's - not
 * from a field log] */
static void m_lose(int i)
{
    char s[64];
    int ls[3] = {M.d[i].a2dp, M.d[i].a2dp + 1, M.d[i].avrcp};
    if (M.d[i].a2dp < 0)
        return;
    M.d[i].streaming = 0;
    for (int k = 0; k < 3; k++)
        if (ls[k] >= 0) {
            snprintf(s, sizeof s, "NO CARRIER %d ERROR 108 HCI_ERROR_CONNECTION_TIMEOUT", ls[k]);
            line(M.b, s);
        }
    M.d[i].a2dp = M.d[i].avrcp = M.d[i].browse = -1;
    M.d[i].ps_reg = 0;
}

static void m_command(const char *c)
{
    int l, i;
    char mac[24], s[128];
    if (sscanf(c, "SET %d SELECT", &l) == 1) {
        M.sel = l;
    } else if (!strcmp(c, "AVRCP PDU 31 1")) {
        i = m_by_link(M.sel);
        if (i >= 0 && M.d[i].avrcp == M.sel && M.d[i].ps_rep) {
            M.d[i].ps_reg = 1;
            M.d[i].ps_queries++;
            snprintf(s, sizeof s, "AVRCP %d REGISTER_NOTIFICATION_RSP INTERIM PLAYBACK_STATUS_CHANGED %s", M.sel,
                     M.d[i].paused ? "PAUSED" : "PLAYING");
            m_at(60, 0, -1, s);
        }
    } else if (!strcmp(c, "AVRCP PAUSE") && (i = m_by_link(M.sel)) >= 0 && M.d[i].toggle) {
        m_player(i, !M.d[i].paused);   /* Windows: play/pause - an open stream stays, audible only while playing */
        if (M.d[i].streaming)
            M.d[i].silent = M.d[i].paused || M.d[i].broken || (M.d[i].sink_stopped && M.d[i].silent_after_stop);
    } else if (!strcmp(c, "AVRCP PLAY") && (i = m_by_link(M.sel)) >= 0 && M.d[i].toggle) {
        M.plays_to[i]++;
        m_player(i, !M.d[i].paused);
        if (M.d[i].streaming)
            M.d[i].silent = M.d[i].paused || M.d[i].broken || (M.d[i].sink_stopped && M.d[i].silent_after_stop);
        else if (!M.d[i].paused)
            m_at(400, 1, i, 0);
    } else if (!strcmp(c, "AVRCP PAUSE")) {
        i = m_by_link(M.sel);
        if (i >= 0)
            m_player(i, 1);
        if (i >= 0 && M.d[i].streaming && M.d[i].stop_on_pause)
            m_at(M.d[i].pause_stop_ms ? (uint32_t)M.d[i].pause_stop_ms : 300u, 0, i, "stop");
    } else if (!strcmp(c, "AVRCP PLAY")) {
        i = m_by_link(M.sel);
        M.plays_to[i >= 0 ? i : M_N]++;
        if (i >= 0 && !M.d[i].ignore_play) {
            m_player(i, 0);
            if (M.d[i].streaming)   /* the player resumes on the open stream: heard (unless a sink STOP silenced it) */
                M.d[i].silent = (M.d[i].sink_stopped && M.d[i].silent_after_stop) || M.d[i].broken;
            else
                m_at(400, 1, i, 0);
        }
    } else if (!strcmp(c, "A2DP STREAMING START")) {
        i = m_by_link(M.sel);
        if (i >= 0 && M.d[i].a2dp == M.sel)
            M.d[i].sink_starts++;
        if (i >= 0 && M.d[i].a2dp == M.sel && !M.d[i].streaming && M.d[i].honor_start)
            m_at(100, M.direct_switch ? 8 : 1, i, 0);
    } else if (strstr(c, " 17 AVRCP") && sscanf(c, "CALL %23s 17 AVRCP", mac) == 1) {
        for (i = 0; i < M_N && strcmp(M.d[i].mac, mac); i++)
            ;
        l = M.next_link++;
        snprintf(s, sizeof s, "CALL %d", l);
        line(M.b, s);
        if (i < M_N && M.d[i].a2dp >= 0) {   /* the ACL is up: the target accepts the control channel */
            snprintf(s, sizeof s, "CONNECT %d AVRCP 17", l);
            m_at(300, 0, -1, s);
            M.d[i].avrcp = l;
        } else {
            snprintf(s, sizeof s, "NO CARRIER %d ERROR 406 RFC_CONNECTION_FAILED", l);
            m_at(3000, 0, -1, s);
        }
    } else if (!strcmp(c, "A2DP STREAMING STOP")) {
        i = m_by_link(M.sel);
        if (i >= 0 && M.d[i].a2dp == M.sel && M.d[i].streaming && M.d[i].honor_suspend) {
            m_at(100, 0, i, "stop");
            M.d[i].sink_stopped = 1;
        }
    } else if (sscanf(c, "CLOSE %d", &l) == 1) {
        i = m_by_link(l);
        if (i >= 0 && M.d[i].browse == l) {
            snprintf(s, sizeof s, "NO CARRIER %d ERROR 0", l);
            m_at(150, 0, -1, s);
            M.d[i].browse = -1;
        } else if (i >= 0 && M.d[i].a2dp == l) {   /* the signalling link: both A2DP links go, AVRCP stays */
            M.closes++;
            M.d[i].broken |= M.d[i].breaks_on_close;
            if (M.d[i].streaming) {
                M.d[i].streaming = 0;
                snprintf(s, sizeof s, "A2DP STREAMING STOP %d", l);
                m_at(100, 0, -1, s);
            }
            snprintf(s, sizeof s, "NO CARRIER %d ERROR 0", l);
            m_at(150, 0, -1, s);
            snprintf(s, sizeof s, "NO CARRIER %d ERROR 0", l + 1);
            m_at(160, 0, -1, s);
            M.d[i].a2dp = -1;
            if (M.d[i].avrcp_drop_on_close && M.d[i].avrcp >= 0) {   /* field 2026-11-02 afternoon: NO CARRIER 4 */
                snprintf(s, sizeof s, "NO CARRIER %d ERROR 0", M.d[i].avrcp);
                m_at(170, 0, -1, s);
                M.d[i].avrcp = -1;
                M.d[i].ps_reg = 0;
            }
        } else if (i >= 0 && M.d[i].avrcp == l) {
            snprintf(s, sizeof s, "NO CARRIER %d ERROR 0", l);
            m_at(150, 0, -1, s);
            M.d[i].avrcp = -1;
        }
    } else if (sscanf(c, "CALL %23s 19 A2DP", mac) == 1) {
        for (i = 0; i < M_N && strcmp(M.d[i].mac, mac); i++)
            ;
        l = M.next_link;
        M.next_link += 2;
        snprintf(s, sizeof s, "CALL %d", l);
        line(M.b, s);
        if (i < M_N) {
            snprintf(s, sizeof s, "CONNECT %d A2DP 19", l);
            m_at(1500, 0, -1, s);
            snprintf(s, sizeof s, "CONNECT %d A2DP 19", l + 1);
            m_at(1510, 0, -1, s);
            M.d[i].a2dp = l;
            M.d[i].sink_stopped = 0;   /* a new A2DP connection */
            if (M.d[i].stream_on_link || M.d[i].blip_when_paused)
                m_at(M.d[i].link_start_ms ? (uint32_t)M.d[i].link_start_ms : 2000u, M.d[i].stream_on_link ? 1 : 7, i, 0);
            if (M.d[i].avrcp < 0 && M.d[i].avrcp_back_ms)
                m_at(1510u + (uint32_t)M.d[i].avrcp_back_ms, 6, i, 0);
            if (M.d[i].close_after_ms)
                m_at(1510u + (uint32_t)M.d[i].close_after_ms, 9, i, 0);
        } else {   /* nobody answers the page (RM 6.16 example) */
            snprintf(s, sizeof s, "NO CARRIER %d ERROR 406 RFC_CONNECTION_FAILED", l);
            m_at(5000, 0, -1, s);
        }
    } else if (!strcmp(c, "SET")) {
        line(M.b, "SET");   /* the echo */
        line(M.b, "SET BT BDADDR 02:07:80:aa:bb:cc");
        line(M.b, "SET BT NAME CA79 Audio");
        snprintf(s, sizeof s, "SET BT CLASS %06x", M.cod);
        line(M.b, s);
        line(M.b, "SET BT PAGEMODE 3 2000 1");
        line(M.b, "SET BT PAIR 62:81:81:81:11:a1 0123456789abcdef0123456789abcdef");
        if (M.autocall)
            line(M.b, M.ac_line);
        line(M.b, "SET PROFILE A2DP SINK");
        if (M.avf_form == 0)
            snprintf(s, sizeof s, "SET PROFILE AVRCP CONTROLLER %x", M.avf);   /* field 2026-11-09: `… 1f` */
        else
            snprintf(s, sizeof s, "SET PROFILE AVRCP CONTROLLER %x TARGET 3", M.avf);
        if (M.avf_form != 2)
            line(M.b, s);
        if (M.aud_form == 0) {
            bts_audio_line(s, (uint8_t)M.aud);   /* field: `… INTERNAL I2S_SLAVE KEEPALIVE` [inference: the factory line] */
            line(M.b, s);
        } else if (M.aud_form == 1) {
            line(M.b, "SET CONTROL AUDIO INTERNAL I2S_SLAVE EVENT 32");   /* the stock test menu's other line */
        }
        line(M.b, "SET");
    } else if (!strncmp(c, "SET CONTROL AUDIO INTERNAL I2S_SLAVE", 36)) {
        if (!M.refuse)
            M.aud = (strstr(c + 36, " EVENT") ? BTS_AUD_EVENT : 0u) | (strstr(c + 36, " KEEPALIVE") ? BTS_AUD_KEEP : 0u);
        M.aud_writes++;
    } else if (!strncmp(c, "SET PROFILE AVRCP CONTROLLER ", 29)) {
        if (!M.refuse)
            M.avf = (unsigned)strtoul(c + 29, NULL, 16);
    } else if (!strcmp(c, "SET CONTROL AUTOCALL")) {
        M.autocall = 0;
    } else if (!strncmp(c, "SET CONTROL AUTOCALL ", 21)) {
        M.autocall = 1;
        snprintf(M.ac_line, sizeof M.ac_line, "%s", c);
    } else if (!strncmp(c, "SET BT CLASS ", 13)) {
        if (!M.refuse)
            M.cod = (unsigned)strtoul(c + 13, NULL, 16);
    }
}

static void m_init(bts_t *b)
{
    memset(&M, 0, sizeof M);
    M.b = b;
    /* silent_after_stop 0: the 2026-11-02 evening "silent after a sink STOP" followed a module restart = the
     * endpoint loss [inference]; test_btsrc_windows_silent_after_sink_stop keeps the hypothesis as a risk */
    M.d[M_PC] = (mdev_t){"02:1a:7d:da:13:23", -1, -1, 0, 0, 1, 0, 0, 0, 1, 0, -1, 0, 0, 0, 0};
    M.d[M_PHONE] = (mdev_t){"62:81:81:81:11:a1", -1, -1, 0, 1, 1, 0, 0, 0, 1, 0, -1};
    M.d[M_TV] = (mdev_t){"12:84:04:04:14:24", -1, -1, 0, 1, 1, 0, 1, 0, 1, 0, -1};
    for (int i = 0; i < M_N; i++) {
        M.d[i].ps_rep = 1;     /* Android / Windows report their play status [inference - the D-rows check it] */
        M.d[i].paused = 1;     /* idle until the user plays (m_play) */
    }
    M.d[M_PC].retry_free_ms = 160;
    M.d[M_PHONE].retry_free_ms = 300;
    M.autocall = 1;
    snprintf(M.ac_line, sizeof M.ac_line, "SET CONTROL AUTOCALL 19 5000 A2DP");
    M.cod = 0x001f00;         /* field 2026-11-09 */
    M.avf = 0x1f;             /* field 2026-11-09 `SET PROFILE AVRCP CONTROLLER 1f` (= the stock factory line) */
    M.aud = BTS_AUD_FACTORY;  /* the stock factory line `… INTERNAL I2S_SLAVE KEEPALIVE` (REA 0x4013EE30) */
    M.all_gone_at = 0;
    fresh(b);
    /* the model tests written up to e5bb2372 check the switch / guard mechanisms with a paused Windows source suspended;
     * the default since 2026-11-09 (field) leaves it alone - those tests: test_btsrc_paused_source_kept and the
     * switch runs with sw_susp_paused 0 */
    b->susp_paused = 1;
}

/* the model and the manager run together; returns when ms passed */
static void m_run(uint32_t ms, bool can_act)
{
    for (uint32_t t = 0; t < ms; t += 8) {
        char c[BTS_OUT_MAX];
        M.now += 8;
        for (int k = 0; k < M.nev; k++) {
            mev_t e = M.ev[k];
            if (e.at > M.now)
                continue;
            M.ev[k] = M.ev[--M.nev];
            k--;
            if (e.kind == 1)
                m_try_start(e.dev);
            else if (e.kind == 3) {
                m_reboot();
                break;
            } else if (e.kind == 4)
                m_browse(e.dev);
            else if (e.kind == 8) {   /* direct_switch: the sink's START stops the open stream, starts this one */
                for (int i = 0; i < M_N; i++)
                    if (i != e.dev && M.d[i].streaming) {
                        m_stop(i);
                        M.d[i].waiting_free = !M.d[i].paused && M.d[i].retry_free_ms > 0;
                    }
                m_try_start(e.dev);
            }
            else if (e.kind == 7) {   /* a fresh link of a paused player: it opens the stream and stops it at once */
                m_try_start(e.dev);
                if (M.d[e.dev].paused && M.d[e.dev].streaming)
                    m_at(10, 0, e.dev, "stop");
            }
            else if (e.kind == 6 && M.d[e.dev].a2dp >= 0 && M.d[e.dev].avrcp < 0) {   /* it opens AVRCP again */
                char s[64];
                M.d[e.dev].avrcp = M.next_link++;
                snprintf(s, sizeof s, "RING %d %s 17 AVRCP", M.d[e.dev].avrcp, M.d[e.dev].mac);
                line(M.b, s);
            }
            else if (e.kind == 9 && M.d[e.dev].a2dp >= 0) {   /* it closes its A2DP (field: 1 first, then 0) */
                char s[48];
                int l0 = M.d[e.dev].a2dp;
                M.d[e.dev].a2dp = -1;
                M.d[e.dev].streaming = 0;
                snprintf(s, sizeof s, "NO CARRIER %d ERROR 0", l0 + 1);
                line(M.b, s);
                snprintf(s, sizeof s, "NO CARRIER %d ERROR 0", l0);
                line(M.b, s);
            }
            else if (e.dev >= 0 && !strcmp(e.text, "stop"))
                m_stop(e.dev);
            else
                line(M.b, e.text);
        }
        if (!m_streams())
            for (int i = 0; i < M_N; i++)
                if (M.d[i].waiting_free && M.d[i].a2dp >= 0 && !M.d[i].paused) {
                    M.d[i].waiting_free = 0;
                    m_at((uint32_t)M.d[i].retry_free_ms, 1, i, 0);
                }
        /* AUTOCALL: every A2DP link gone -> 7 s later the module links the PC again and it streams (field #3) */
        int any = 0;
        for (int i = 0; i < M_N; i++)
            any |= M.d[i].a2dp >= 0;
        if (any || !M.autocall)
            M.all_gone_at = 0;
        else if (!M.all_gone_at)
            M.all_gone_at = M.now;
        else if (M.now - M.all_gone_at >= 7000) {
            char s[40];
            M.all_gone_at = 0;
            M.d[M_PC].a2dp = M.next_link;
            M.d[M_PC].sink_stopped = 0;
            M.next_link += 2;
            snprintf(s, sizeof s, "CONNECT %d A2DP 19", M.d[M_PC].a2dp);
            line(M.b, s);
            snprintf(s, sizeof s, "CONNECT %d A2DP 19", M.d[M_PC].a2dp + 1);
            line(M.b, s);
            m_at(500, 1, M_PC, 0);
            if (M.d[M_PC].browse_ms)
                m_at((uint32_t)M.d[M_PC].browse_ms, 4, M_PC, 0);
        }
        bts_tick(M.b, 8, can_act);
        if (bts_next(M.b, c, sizeof c)) {
            if (trace > 0)
                printf("      %7u -> %s\n", (unsigned)M.now, c);
            if (!strcmp(c, "SET"))
                nreads++;
            else if (nsent < 48)
                strcpy(sent[nsent++], c);
            if (!strcmp(c, "LIST")) {   /* the LIST reply from the model's links */
                char s[120];
                int n = 0;
                for (int i = 0; i < M_N; i++)
                    n += (M.d[i].a2dp >= 0) * 2 + (M.d[i].avrcp >= 0) + (M.d[i].browse >= 0);
                snprintf(s, sizeof s, "LIST %d", n);
                line(M.b, s);
                for (int i = 0; i < M_N; i++) {
                    int ls[4] = {M.d[i].a2dp, M.d[i].a2dp >= 0 ? M.d[i].a2dp + 1 : -1, M.d[i].avrcp, M.d[i].browse};
                    for (int k = 0; k < 4; k++)
                        if (ls[k] >= 0) {
                            snprintf(s, sizeof s, "LIST %d CONNECTED %s 1024 0 0 24 0 0 %s %s INCOMING ACTIVE SLAVE "
                                                  "ENCRYPTED 0", ls[k], k < 2 ? "A2DP" : "AVRCP", M.d[i].mac,
                                     k < 2 ? "19" : k == 2 ? "17" : "1b");
                            line(M.b, s);
                        }
                }
            } else {
                m_command(c);
            }
        }
    }
}

static int m_dev(int i)
{
    uint8_t a[6];
    for (int k = 0; k < 6; k++)
        a[k] = (uint8_t)strtol(M.d[i].mac + 3 * k, 0, 16);
    return dev_of(M.b, a);
}

static void m_addr(int i, uint8_t a[6])
{
    for (int k = 0; k < 6; k++)
        a[k] = (uint8_t)strtol(M.d[i].mac + 3 * k, 0, 16);
}

static bool m_selected(int i)
{
    int d = m_dev(i);
    return d >= 0 && (bts_dev_state2(M.b, d) & BTS_D2_SELECTED);
}

/* streaming and heard (not a Windows stream silenced by an earlier sink STOP) */
static bool m_audible(int i) { return M.d[i].streaming && !M.d[i].silent; }

/* the allowed command forms (the manager's run-time set) - AUTOCALL writes are counted apart */
static int ac_writes_sent(void)
{
    return count_sent("SET CONTROL AUTOCALL") + count_sent("SET BT CLASS") + count_sent("SET PROFILE") +
           count_sent("SET CONTROL AUDIO");
}

/* ---- field 2026-11-02: Windows without AVRCP, silent after a sink STOP, the reboot loop - */

/* the time until device i streams (ms, from now), at most `limit` */
static uint32_t m_until_streams(int i, uint32_t limit)
{
    uint32_t t0 = M.now;
    while (!M.d[i].streaming && M.now - t0 < limit)
        m_run(8, true);
    return M.now - t0;
}

static int sent_index(const char *c, int from)
{
    for (int i = from; i < nsent; i++)
        if (!strcmp(sent[i], c))
            return i;
    return -1;
}

/* field #2-#4: PC first (Windows: A2DP only - it never opens AVRCP), then the phone (AVRCP, auto-plays on PLAY).  The
 * opt-out flag keeps the PC without AVRCP, as in the field.  PC -> phone: no AVRCP to pause the PC -> the sink suspends
 * its stream at once (`SET 0 SELECT` + `A2DP STREAMING STOP` - never a CLOSE, field 2026-11-03), phone PLAY.  Phone ->
 * PC: phone PAUSE, its STOP; the PC kept its A2DP link - the sink resumes it (`SET 0 SELECT` + `A2DP STREAMING START`),
 * no CALL */
static void test_btsrc_windows_no_avrcp(void)
{
    bts_t b;
    uint8_t pc[6], phone[6];
    for (int user_pauses = 0; user_pauses < 2; user_pauses++) {
        m_init(&b);
        b.pol.flags |= BTS_F_NO_AVRCP_CALL;
        M.d[M_PC].no_avrcp = 1;
        M.d[M_PC].honor_start = 1;
        m_connect_in(M_PC);
        m_play(M_PC);
        m_run(1000, true);
        m_connect_in(M_PHONE);
        m_run(3000, true);
        CHECK(m_selected(M_PC));
        CHECK(!(bts_dev_state2(&b, m_dev(M_PC)) & BTS_D2_AVRCP));
        m_addr(M_PC, pc);
        m_addr(M_PHONE, phone);
        /* PC -> phone: the PC has no AVRCP - the sink never stops it (field 2026-11-10: a Windows stream the
         * sink stopped stays silent until it reconnects; field #3 of 11-02 stopped it in 1.2 s) - the switch waits */
        int before = nsent;
        CHECK_EQ(bts_use(&b, phone, true), BTS_OK);
        m_run(2000, true);
        CHECK(m_audible(M_PC) && !M.d[M_PHONE].streaming);
        CHECK_EQ(b.sw_phase, BTS_SW_PAUSE);
        CHECK(b.sw_held);
        if (!user_pauses) {
            m_run(BTS_SW_HELD_MS - 1500, true);
            CHECK_EQ(b.sw_phase, BTS_SW_IDLE);
            CHECK_EQ(b.sw_result, BTS_RES_BUSY);    /* the app: pause it on the PC / disconnect it */
            CHECK(m_audible(M_PC));
            CHECK_EQ(count_sent("A2DP STREAMING STOP"), 0);
            CHECK_EQ(count_sent("AVRCP PLAY"), 0);  /* the phone is not asked to play over a stream it cannot replace */
            CHECK_EQ(b.n.held, 1);
            CHECK(only_runtime_sent());
            continue;
        }
        m_player(M_PC, 1);                           /* the user pauses on the PC, Windows closes its stream */
        m_stop(M_PC);
        uint32_t took = m_until_streams(M_PHONE, 4000);
        CHECK(m_audible(M_PHONE) && !M.d[M_PC].streaming);
        CHECK(took < 1500);
        CHECK_EQ(sent_index("A2DP STREAMING STOP", before), -1);
        m_run(200, true);
        CHECK_EQ(b.sw_result, BTS_RES_PLAYING);
        CHECK(M.d[M_PC].a2dp >= 0 && !b.dev[m_dev(M_PC)].susp);   /* linked, never sink-stopped */
        /* phone -> PC: the phone (AVRCP) paused; the PC has no AVRCP - the sink never starts it (field 2026-11-10 night,
         * a Windows stream the sink's START opened stayed silent when the PC played; up to
         * f2056ae8 that START went here): READY at once - the user plays on the PC, which opens its own stream */
        before = nsent;
        CHECK_EQ(bts_use(&b, pc, true), BTS_OK);
        took = m_until_streams(M_PC, 3000);
        CHECK(!M.d[M_PC].streaming && !M.d[M_PHONE].streaming);
        CHECK(!strcmp(sent[before], "SET 5 SELECT") && !strcmp(sent[before + 1], "AVRCP PAUSE"));
        CHECK_EQ(calls_of(" 19 A2DP"), 0);             /* its link was kept: nothing to call */
        CHECK_EQ(sent_index("A2DP STREAMING START", before), -1);
        CHECK_EQ(b.sw_result, BTS_RES_READY);
        CHECK_EQ(b.sw_tried, 0);
        CHECK(b.sw_last_ms < 1500);                    /* pause 0.3 s + settle 0.5 s */
        CHECK(m_selected(M_PC));
        m_play(M_PC);                                  /* the user plays on the PC: its own stream */
        m_run(1200, true);
        CHECK(m_audible(M_PC) && !M.d[M_PHONE].streaming);
        CHECK_EQ(b.sw_result, BTS_RES_READY);          /* (nothing more from the piano) */
        CHECK_EQ(count_sent("A2DP STREAMING STOP"), 0);
        CHECK_EQ(count_sent("CLOSE"), 0);
        CHECK_EQ(M.silent_starts, 0);
        CHECK(only_runtime_sent());
        CHECK_EQ(ac_writes_sent(), 0);
    }
}

/* the default: the sink opens AVRCP to a source without it, 4 s after its A2DP (once - the source had the time to open
 * its own, field 2026-11-09).  A PC that joined while the
 * phone played (its A2DP up, never streamed, the module ignored its play - it reports PLAYING) and does not start again
 * by itself: no PLAY (it plays - PLAY would be play/pause on Windows), 1.2 s, then the sink START.  One that rejects the
 * START too (and has no AVRCP): "ready" (press play on it) after the START - its A2DP is never reopened any more (a
 * CLOSE removed the Windows endpoint, field 2026-11-03) */
static void test_btsrc_windows_avrcp_and_ready(void)
{
    bts_t b;
    uint8_t pc[6];
    m_init(&b);
    M.d[M_PC].no_avrcp = 1;
    M.d[M_PC].honor_start = 1;
    M.d[M_PC].ignore_play = 1;
    M.d[M_PC].retry_free_ms = 0;
    m_connect_in(M_PHONE);
    m_play(M_PHONE);
    m_run(1000, true);
    m_connect_in(M_PC);
    m_run(3900, true);
    CHECK_EQ(calls_of(" 17 AVRCP"), 0);            /* 1.5 s up to ota_main a6fdc5a2 */
    m_run(200, true);
    CHECK_EQ(count_sent("CALL 02:1a:7d:da:13:23 17 AVRCP"), 1);
    m_run(1000, true);
    CHECK(bts_dev_state2(&b, m_dev(M_PC)) & BTS_D2_AVRCP);
    CHECK(!(bts_dev_state2(&b, m_dev(M_PC)) & BTS_D2_CALLING));   /* an AVRCP call is no "connecting…" */
    m_play(M_PC);                             /* ignored: the phone streams */
    m_run(5000, true);
    CHECK_EQ(calls_of(" 17 AVRCP"), 1);           /* the phone opened its own: nothing for it */
    m_addr(M_PC, pc);
    int before = nsent;
    CHECK_EQ(bts_use(&b, pc, true), BTS_OK);
    uint32_t took = m_until_streams(M_PC, 8000);
    CHECK(m_audible(M_PC));
    CHECK(took < 2800);                            /* pause 0.3 s + settle 0.5 s + its own start 1.2 s + START 0.1 s */
    CHECK_EQ(sent_index("AVRCP PLAY", before), -1);
    CHECK(sent_index("A2DP STREAMING START", before) > 0);
    CHECK_EQ(M.plays_to[M_PC], 0);
    m_run(200, true);
    CHECK_EQ(b.sw_result, BTS_RES_PLAYING);
    CHECK_EQ(b.sw_tried, BTS_SWT_KEPT | BTS_SWT_START);
    CHECK_EQ(bts_dev_ps(&b, m_dev(M_PC)) & 7, BTS_PS_PLAYING);
    /* no AVRCP: "ready" at once, never the sink's START (up to f2056ae8 the START, 1 s, then
     * "ready") - the link stays, nothing closed or called */
    m_init(&b);
    b.pol.flags |= BTS_F_NO_AVRCP_CALL;
    M.d[M_PC].no_avrcp = 1;
    M.d[M_PC].honor_start = 0;
    m_connect_in(M_PHONE);
    m_play(M_PHONE);
    m_connect_in(M_PC);
    m_run(2000, true);
    CHECK(m_selected(M_PHONE));
    CHECK_EQ(bts_use(&b, pc, true), BTS_OK);
    m_run(2500, true);
    CHECK_EQ(b.sw_phase, BTS_SW_IDLE);
    CHECK_EQ(b.sw_result, BTS_RES_READY);
    CHECK_EQ(b.sw_tried, 0);
    CHECK_EQ(count_sent("A2DP STREAMING START"), 0);
    CHECK_EQ(count_sent("CLOSE"), 0);
    CHECK_EQ(calls_of(" 19 A2DP"), 0);
    CHECK(M.d[M_PC].a2dp >= 0);
    CHECK(only_runtime_sent());
}

/* field #1: the phone plays; the PC connects A2DP and opens the AVRCP browsing channel (0x1b) alone 1 s later - before
 * the sink's AVRCP call - and the module reboots: the PC is the suspect.  Up to ota_main a6fdc5a2 the sink then called
 * its control channel 0.3 s after its A2DP; field 2026-11-09: that early CALL stayed
 * unanswered and the PC's lone browsing channel followed 11 s later -> reboot.  Now the sink never opens AVRCP to the
 * suspect first: AUTOCALL links the PC again (here it no longer opens the browsing channel alone - re-paired, as in the
 * field) and it gets no `CALL … 17 AVRCP` at all, keeps playing, no second reboot */
static void test_btsrc_reboot_suspect(void)
{
    bts_t b;
    m_init(&b);
    M.d[M_PC].no_avrcp = 1;
    M.d[M_PC].browse_ms = 1000;
    m_connect_in(M_PHONE);
    m_play(M_PHONE);
    m_run(8000, true);                             /* past the boot grace */
    CHECK_EQ(b.n.reboots, 0);
    m_connect_in(M_PC);
    m_run(2500, true);
    CHECK_EQ(M.reboots, 1);
    CHECK_EQ(b.n.reboots, 1);
    CHECK_EQ(b.n.orphans, 1);
    CHECK_EQ(bts_health(&b), BTS_MH_SUSPECT);     /* one reboot: no loop */
    CHECK_EQ(bts_a2dp_links(&b), 0);
    CHECK_EQ(calls_of(" 17 AVRCP"), 0);            /* 1 s: before the sink's 4 s */
    M.d[M_PC].browse_ms = 0;
    m_run(9000, true);                             /* AUTOCALL: the PC again (OUTGOING) */
    CHECK(M.d[M_PC].a2dp >= 0);
    m_run(60000, true);
    CHECK_EQ(calls_of(" 17 AVRCP"), 0);            /* the suspect: never from the sink */
    CHECK(M.d[M_PC].avrcp < 0);
    CHECK_EQ(M.reboots, 1);
    CHECK_EQ(b.n.reboots, 1);
    CHECK(!b.loop);
    CHECK(m_audible(M_PC));
    CHECK(only_runtime_sent());
    CHECK_EQ(ac_writes_sent(), 0);
}

/* a PC that opens the browsing channel alone every time (0.2 s after its A2DP): reboot, AUTOCALL, reboot -> the loop guard:
 * health LOOP, no CALL of the manager while it holds (AVRCP, reconnect), no module setting written; it clears 5 min after
 * the last reboot.  A restart after a power edge is not counted. */
static void test_btsrc_reboot_loop(void)
{
    bts_t b;
    m_init(&b);
    M.d[M_PC].no_avrcp = 1;
    M.d[M_PC].browse_ms = 200;
    m_run(8000, true);
    m_connect_in(M_PC);
    m_run(1500, true);
    CHECK_EQ(b.n.reboots, 1);
    CHECK(!b.loop);
    m_run(9000, true);                             /* AUTOCALL 7 s -> the PC -> browsing 0.2 s -> reboot 2 */
    CHECK_EQ(M.reboots, 2);
    CHECK_EQ(b.n.reboots, 2);
    CHECK(b.loop);
    CHECK(bts_health(&b) & BTS_MH_LOOP);
    int avrcp_before = calls_of(" 17 AVRCP");
    m_run(60000, true);                            /* the module keeps looping (AUTOCALL is its own) */
    CHECK(M.reboots >= 5);
    CHECK(b.loop);
    CHECK_EQ(calls_of(" 17 AVRCP"), avrcp_before); /* held */
    CHECK_EQ(calls_of(" 19 A2DP"), 0);
    CHECK_EQ(ac_writes_sent(), 0);                 /* AUTOCALL stays the user's op */
    /* the user turns AUTOCALL off / re-pairs: quiet - the guard ends 5 min after the last reboot */
    M.autocall = 0;
    m_run(240000, true);
    CHECK(b.loop);                                 /* < 5 min since the last reboot */
    m_run(70000, true);
    CHECK(!b.loop);
    /* the module switched off and on (standby / 1D 07): its banner is no reboot */
    uint16_t n = b.n.reboots;
    bts_module(&b, false);
    m_run(600, true);
    bts_module(&b, true);
    m_run(800, true);
    line(&b, "WRAP THOR AI (6.2.0 build 1135)");
    line(&b, "READY.");
    CHECK_EQ(b.n.reboots, n);
}

/* `A2DP CODEC …` (RM 6.67 events): parsed and kept with the stream it came with */
static void test_btsrc_codec(void)
{
    bts_t b;
    m_init(&b);
    m_connect_in(M_PC);
    line(&b, "A2DP CODEC AAC JOINT_STEREO 44100 BITPOOL 0-0");
    m_play(M_PC);
    m_run(100, true);
    int d = m_dev(M_PC);
    CHECK(d >= 0 && b.dev[d].codec == BTS_CODEC_AAC);
    CHECK_EQ(b.codec_dev, d);
    line(&b, "A2DP CODEC SBC JOINT_STEREO 48000 BITPOOL 2-53");   /* reloaded while it plays */
    CHECK_EQ(b.dev[d].codec, BTS_CODEC_SBC);
    CHECK_EQ(b.codec_rate, 1);
}

/* field 2026-11-02: PC (Windows) plays; the phone connects; 「이 기기로 듣기」 on the phone.  Windows keeps its stream
 * after AVRCP PAUSE: 0.7 s later the sink suspends it (`SET 0 SELECT` + `A2DP STREAMING STOP` - never a CLOSE: field
 * 2026-11-03, the Windows endpoint went with it) -> the phone starts by itself (it reports PLAYING: no PLAY); and
 * back: the PC kept every link - the sink resumes its stream (`A2DP STREAMING START`), its player paused by us: silent
 * until the PLAY that follows (it reports PAUSED) */
static void test_btsrc_switch_windows_pause_suspend(void)
{
    bts_t b;
    uint8_t phone[6];
    m_init(&b);
    b.susp_paused = 1;   /* the suspend path of a paused Windows stream (up to e5bb2372; field 2026-11-09: silent on
                          * return - the default leaves such a source alone, test_btsrc_paused_source_kept) */
    M.d[M_PC].silent_when_paused = 1;
    m_connect_in(M_PC);
    m_run(500, true);
    m_play(M_PC);
    m_run(2000, true);
    CHECK(m_selected(M_PC));                       /* adopted: the first source that streams */
    m_connect_in(M_PHONE);
    m_run(2000, true);
    m_play(M_PHONE);                          /* the user presses play on the phone: ignored by the module */
    m_run(1000, true);
    CHECK_EQ(M.ignored, 1);
    CHECK(M.d[M_PC].streaming && !M.d[M_PHONE].streaming);
    int before = nsent;
    m_addr(M_PHONE, phone);
    CHECK_EQ(bts_use(&b, phone, true), BTS_OK);
    CHECK(m_selected(M_PHONE));
    uint32_t took = m_until_streams(M_PHONE, 10000);
    CHECK(m_audible(M_PHONE) && !M.d[M_PC].streaming);
    CHECK(took < 2400);                            /* pause 0.7 s + suspend 0.1 s + its own start 0.3 s */
    m_run(200, true);
    CHECK_EQ(b.sw_phase, BTS_SW_IDLE);
    CHECK_EQ(b.sw_result, BTS_RES_PLAYING);
    CHECK(!strcmp(sent[before], "SET 2 SELECT") && !strcmp(sent[before + 1], "AVRCP PAUSE"));
    {   /* the PC's A2DP signalling link suspended (its re-registration after its CHANGED may come first); the phone
         * reports PLAYING and starts by itself: no PLAY to it */
        int c = sent_index("A2DP STREAMING STOP", before);
        CHECK(c > before + 1 && !strcmp(sent[c - 1], "SET 0 SELECT") && count_sent("A2DP STREAMING STOP") == 1);
        CHECK_EQ(sent_index("AVRCP PLAY", before), -1);
        CHECK_EQ(M.plays_to[M_PHONE], 0);
        CHECK_EQ(b.sw_tried, BTS_SWT_KEPT);
    }
    CHECK(M.d[M_PC].a2dp >= 0 && M.d[M_PC].avrcp >= 0 && M.d[M_PC].paused);   /* every link kept */
    CHECK(!(bts_dev_state2(&b, m_dev(M_PC)) & BTS_D2_PARKED));
    /* and back: the phone stops on pause; the PC (suspended by the sink, reports PAUSED, paused by us) gets PLAY first
     * and restarts its own stream - no sink START (field 2026-11-09: Windows put no sound on a stream the sink
     * resumed; before 11-09 evening the sink resumed it first, then PLAY) */
    uint8_t pc[6];
    m_addr(M_PC, pc);
    before = nsent;
    CHECK_EQ(bts_use(&b, pc, true), BTS_OK);
    took = m_until_streams(M_PC, 10000);
    m_run(600, true);
    CHECK(m_audible(M_PC) && !M.d[M_PHONE].streaming);
    CHECK(took < 2000);                            /* pause 0.3 s + settle 0.5 s + its own start */
    CHECK_EQ(calls_of(" 19 A2DP"), 0);
    {
        int pl = sent_index("AVRCP PLAY", before);
        CHECK(pl > 0 && !strncmp(sent[pl - 1], "SET ", 4));   /* its AVRCP link selected */
        CHECK_EQ(sent_index("A2DP STREAMING START", before), -1);
    }
    CHECK_EQ(M.plays_to[M_PC], 1);
    CHECK_EQ(b.sw_result, BTS_RES_PLAYING);
    CHECK_EQ(b.sw_tried, BTS_SWT_PLAY);
    CHECK_EQ(count_sent("CLOSE"), 0);
    CHECK_EQ(M.closes, 0);
    CHECK_EQ(M.silent_starts, 0);                  /* its own start after PLAY: heard */
    CHECK(only_runtime_sent());
    CHECK_EQ(ac_writes_sent(), 0);
}

/* the CALL path (field 2026-11-02 afternoon, ota_main 6f181a70): a source with no A2DP link - here the user's own
 * 「소리 연결만 끊기」 (act 6, the only CLOSE left besides 「끊기」) - is CALLed when picked.  PC (Windows, AVRCP) -> phone
 * (the PC suspended, not closed) -> act 6 on the PC -> back to the PC: CALL, Windows opens the stream of the fresh link
 * at once - silence while paused - PLAY once its stream is up.  Act 6 on the phone drops its AVRCP too (Android, field
 * 2026-11-02 afternoon) -> back to it: CALL, PLAY once its AVRCP is back (no 1.5 s self-start wait).  Variant 1: its
 * AVRCP does not come back by itself - the sink opens it 0.7 s after the link (field 2026-11-03), then PLAY.
 * Variant 2: that also opted out (flag 0x20) - no AVRCP: the sink starts it after 1.5 s.  Only the two act-6 CLOSEs. */
static void test_btsrc_windows_paused_fresh(void)
{
    bts_t b;
    uint8_t pc[6], phone[6];
    for (int variant = 0; variant < 3; variant++) {
        m_init(&b);
        b.susp_paused = 1;   /* the CALL path after a suspended Windows source (the suspend itself: up to e5bb2372) */
        if (variant == 2)
            b.pol.flags |= BTS_F_NO_AVRCP_CALL;
        M.d[M_PC].silent_when_paused = 1;
        M.d[M_PHONE].stop_on_pause = 0;            /* field: no STOP within 0.85 s of its PAUSE */
        M.d[M_PHONE].avrcp_drop_on_close = 1;
        M.d[M_PHONE].avrcp_back_ms = variant ? 0 : 600;
        M.d[M_PHONE].honor_start = 1;
        m_addr(M_PC, pc);
        m_addr(M_PHONE, phone);
        m_connect_in(M_PC);
        m_play(M_PC);
        m_run(1500, true);
        m_connect_in(M_PHONE);
        m_run(2500, true);
        CHECK(m_selected(M_PC) && m_audible(M_PC));
        /* PC -> phone */
        CHECK_EQ(bts_use(&b, phone, true), BTS_OK);
        uint32_t took = m_until_streams(M_PHONE, 10000);
        m_run(300, true);
        CHECK(m_audible(M_PHONE) && !M.d[M_PC].streaming);
        CHECK(took < 2400);
        CHECK(M.d[M_PC].a2dp >= 0 && M.d[M_PC].avrcp >= 0 && M.d[M_PC].paused);   /* suspended, every link kept */
        CHECK_EQ(b.sw_result, BTS_RES_PLAYING);
        /* the user's act 6 on the PC: its A2DP closed - parked, AVRCP kept */
        CHECK_EQ(bts_action(&b, BTS_ACT_MUTE, pc), BTS_OK);
        m_run(500, true);
        CHECK(M.d[M_PC].a2dp < 0 && M.d[M_PC].avrcp >= 0);
        CHECK(bts_dev_state2(&b, m_dev(M_PC)) & BTS_D2_PARKED);
        /* phone -> PC: the CALL path */
        M.d[M_PC].stream_on_link = 1;
        M.d[M_PC].link_start_ms = 1700;            /* the stream of the fresh link before the 0.5 s settle */
        int plays = M.plays_to[M_PC], before = nsent;
        CHECK_EQ(bts_use(&b, pc, true), BTS_OK);
        uint32_t took_pc = m_until_streams(M_PC, 10000);
        CHECK(M.d[M_PC].streaming && !m_audible(M_PC));   /* Windows: the stream is up, silent - its player is paused */
        m_run(600, true);
        CHECK(m_audible(M_PC));                    /* PLAY reached it on its AVRCP control link */
        CHECK_EQ(M.plays_to[M_PC], plays + 1);
        CHECK_EQ(M.plays_to[M_N], 0);
        CHECK_EQ(b.sw_result, BTS_RES_PLAYING);
        CHECK_EQ(b.sw_tried, BTS_SWT_FRESH | BTS_SWT_PLAY | BTS_SWT_PLAY_UP);
        uint32_t sw_pc = b.sw_last_ms;
        CHECK(sw_pc < 3500);                       /* pause 0.3 + 0.7 s, suspend, CALL 1.5 s, START 0.2 s, PLAY 0.3 s */
        CHECK(took_pc < 3300);
        CHECK(sent_index("AVRCP PLAY", before) > 0);
        CHECK(M.d[M_PHONE].a2dp >= 0 && M.d[M_PHONE].avrcp >= 0);   /* the phone suspended, links kept */
        /* the user's act 6 on the phone: its AVRCP goes with its A2DP (Android) */
        CHECK_EQ(bts_action(&b, BTS_ACT_MUTE, phone), BTS_OK);
        m_run(500, true);
        CHECK(M.d[M_PHONE].a2dp < 0 && M.d[M_PHONE].avrcp < 0);
        CHECK(dev_of(&b, phone) >= 0 && (bts_dev_state2(&b, dev_of(&b, phone)) & BTS_D2_PARKED));
        /* PC -> phone again: CALL, then PLAY once its AVRCP is back (no self-start wait) / without it: READY - the sink
         * never starts a source that has no AVRCP (up to f2056ae8 the sink's START after 1.5 s) */
        plays = M.plays_to[M_PHONE];
        before = nsent;
        CHECK_EQ(bts_use(&b, phone, true), BTS_OK);
        took = m_until_streams(M_PHONE, variant == 2 ? 6000 : 12000);
        m_run(300, true);
        CHECK(!M.d[M_PC].streaming);
        CHECK_EQ(b.sw_result, variant == 2 ? BTS_RES_READY : BTS_RES_PLAYING);
        CHECK_EQ(count_sent("CALL 62:81:81:81:11:a1 19 A2DP"), 1);
        if (variant < 2) {
            CHECK(m_audible(M_PHONE));
            CHECK_EQ(M.plays_to[M_PHONE], plays + 1);
            CHECK_EQ(sent_index("A2DP STREAMING START", before), -1);
            CHECK_EQ(b.sw_tried, BTS_SWT_FRESH | BTS_SWT_PLAY);
            CHECK_EQ(calls_of(" 17 AVRCP"), variant);   /* variant 1: the sink opened its AVRCP */
            CHECK(took < (variant ? 5200u : 4800u)); /* 1.0 s pause + suspend 0.3 s + CALL 1.5 s + AVRCP 0.6 s (sink: 0.7 + 0.3 s) + its play status 0.3 s + PLAY 0.7 s */
        } else {
            CHECK_EQ(M.plays_to[M_PHONE], plays);
            CHECK(!M.d[M_PHONE].streaming);        /* its player stays paused: the app says "press play on it" */
            CHECK_EQ(sent_index("A2DP STREAMING START", before), -1);
            CHECK_EQ(b.sw_tried, BTS_SWT_FRESH);
            CHECK(b.sw_last_ms < 4800);            /* ... CALL 1.5 s + 1.5 s for AVRCP / its own start */
        }
        if (getenv("BTS_TIMES"))
            printf("    variant %d: phone to PC stream %u ms, switch %u ms; PC to phone %u ms, switch %u ms\n", variant,
                   (unsigned)took_pc, (unsigned)sw_pc, (unsigned)took, (unsigned)b.sw_last_ms);
        CHECK_EQ(M.closes, 2);                     /* the two act-6 CLOSEs only */
        CHECK(only_runtime_sent());
        CHECK_EQ(ac_writes_sent(), 0);
    }
}

/* field 2026-11-02 night (ota_main 9e6c8c32): the phone selected and streaming; the PC's YouTube
 * plays too (its start ignored by the module - one stream).  「이 기기로 듣기」 on the PC: phone PAUSE, its stream
 * suspended (the field build closed its A2DP), the PC's stream starts at once on the link it had - and the old build
 * then sent AVRCP PLAY, which Windows takes as
 * play/pause: the PC paused.  Now: the PC reports PLAYING -> no PLAY (KEPT); without a status (opt-out flag = the
 * module / target does not answer) the "paused by us" rule: we never paused it -> no PLAY (NOT_OURS).  Then PC ->
 * phone (PAUSE to the playing PC, suspend; the phone - paused by us, reports PAUSED - resumed by the sink, then PLAY)
 * and phone -> PC (the PC paused by us, resumed: silence -> PAUSED -> exactly one PLAY = play).  The phone takes PLAY
 * / PAUSE as discrete commands, the PC as a toggle.  Nothing is closed: both keep A2DP and AVRCP. */
static void test_btsrc_play_status_toggle(void)
{
    bts_t b;
    uint8_t pc[6], phone[6];
    for (int variant = 0; variant < 2; variant++) {
        m_init(&b);
        if (variant)
            b.pol.flags |= BTS_F_NO_PS;           /* no play status: the fallback rule */
        M.d[M_PC].toggle = 1;
        M.d[M_PHONE].stop_on_pause = 0;           /* field: no STOP within 0.85 s of its PAUSE */
        M.d[M_PHONE].avrcp_drop_on_close = 1;     /* (nothing closes it now) */
        M.d[M_PHONE].avrcp_back_ms = 300;
        m_addr(M_PC, pc);
        m_addr(M_PHONE, phone);
        m_connect_in(M_PHONE);
        m_play(M_PHONE);
        m_run(1000, true);
        m_connect_in(M_PC);
        m_run(2000, true);
        m_play(M_PC);                             /* YouTube plays on the PC: ignored by the module */
        m_run(1000, true);
        CHECK(m_selected(M_PHONE) && m_audible(M_PHONE) && !M.d[M_PC].streaming && !M.d[M_PC].paused);
        CHECK_EQ(M.ignored, 1);
        if (!variant) {
            CHECK_EQ(bts_dev_ps(&b, m_dev(M_PC)), BTS_PS_PLAYING | BTS_PSB_LIVE);
            CHECK_EQ(bts_dev_ps(&b, m_dev(M_PHONE)), BTS_PS_PLAYING | BTS_PSB_LIVE);
        } else {
            CHECK_EQ(bts_dev_ps(&b, m_dev(M_PC)), 0);
            CHECK_EQ(count_sent("AVRCP PDU"), 0);
        }
        /* phone -> PC: the field case */
        int before = nsent;
        CHECK_EQ(bts_use(&b, pc, true), BTS_OK);
        uint32_t took = m_until_streams(M_PC, 8000);
        m_run(1500, true);
        CHECK(m_audible(M_PC) && !M.d[M_PC].paused);   /* still playing - not toggled off */
        CHECK_EQ(M.plays_to[M_PC], 0);
        CHECK_EQ(sent_index("AVRCP PLAY", before), -1);
        CHECK(sent_index("AVRCP PAUSE", before) > 0); /* the phone (it played) */
        CHECK_EQ(b.sw_result, BTS_RES_PLAYING);
        CHECK_EQ(b.sw_tried, variant ? BTS_SWT_NOT_OURS : BTS_SWT_KEPT);
        CHECK(b.n.play_skips == 1);
        CHECK(took < 2000);                       /* PAUSE 0.3 s + 0.7 s + suspend + its own start 0.16 s */
        /* PC -> phone: PAUSE to the PC (it plays: a toggle -> paused; Windows keeps the stream) -> suspend; the phone
         * (paused by us, suspended by us) is resumed, then PLAY */
        before = nsent;
        CHECK_EQ(bts_use(&b, phone, true), BTS_OK);
        took = m_until_streams(M_PHONE, 10000);
        m_run(600, true);
        CHECK(m_audible(M_PHONE) && !M.d[M_PC].streaming && M.d[M_PC].paused);
        CHECK_EQ(M.plays_to[M_PHONE], 1);
        CHECK_EQ(b.sw_result, BTS_RES_PLAYING);
        CHECK(took < 5000);
        if (!variant)
            CHECK_EQ(bts_dev_ps(&b, m_dev(M_PC)) & 7, BTS_PS_PAUSED);
        CHECK(bts_dev_ps(&b, m_dev(M_PC)) & BTS_PSB_OURS);
        /* phone -> PC again: the PC was paused by us; its fresh link streams silence -> one PLAY = it plays */
        M.d[M_PC].stream_on_link = 1;
        M.d[M_PC].silent_when_paused = 1;
        M.d[M_PC].link_start_ms = 700;
        int plays = M.plays_to[M_PC];
        CHECK_EQ(bts_use(&b, pc, true), BTS_OK);
        took = m_until_streams(M_PC, 10000);
        m_run(1200, true);
        CHECK(m_audible(M_PC) && !M.d[M_PC].paused);
        CHECK_EQ(M.plays_to[M_PC], plays + 1);
        CHECK(b.sw_tried & BTS_SWT_PLAY);
        CHECK_EQ(b.sw_result, BTS_RES_PLAYING);
        CHECK_EQ(count_sent("CLOSE"), 0);
        CHECK_EQ(calls_of(" 19 A2DP"), 0);
        CHECK(M.d[M_PHONE].avrcp >= 0 && M.d[M_PC].avrcp >= 0 && M.d[M_PHONE].a2dp >= 0);
        CHECK(only_runtime_sent());
        CHECK_EQ(ac_writes_sent(), 0);
    }
}

/* field 2026-11-03 (ota_main 1afe1978, test 2-가): #1 the phone streaming (selected), the
 * Windows PC's YouTube playing but ignored -> PC 「이 기기로 듣기」: phone PAUSE (CHANGED PAUSED), 0.7 s, its stream
 * suspended (the field build CLOSEd its A2DP and its AVRCP went too - here the user's act 6 does that, the only way
 * left), the PC starts by itself -> no PLAY (it plays).  #2 PC -> phone: PC PAUSE (toggle -> paused, Windows keeps the
 * stream), the PC suspended, `CALL <phone> 19 A2DP` -> CONNECT -> the phone (paused by the piano in #1) opens its
 * stream and stops it 10 ms later; it opens no AVRCP by itself.  1afe1978 took that START for
 * success and ended ("sound link open"); its AVRCP came 1.1 s later (the sink's CALL) and reported PAUSED - no PLAY:
 * silence.  Now: the switch stays until its AVRCP is up; status PAUSED + paused by the piano -> exactly one PLAY.
 * variant 1: the opt-out flag 0x20 (the sink never opens AVRCP) - no AVRCP ever: the sink starts the stream. */
static void test_btsrc_field_1103_blip(void)
{
    bts_t b;
    uint8_t pc[6], phone[6];
    for (int variant = 0; variant < 2; variant++) {
        m_init(&b);
        if (variant)
            b.pol.flags |= BTS_F_NO_AVRCP_CALL;
        M.d[M_PC].toggle = 1;
        M.d[M_PHONE].stop_on_pause = 0;           /* field #1: its A2DP CLOSEd 0.7 s after the PAUSE, no STOP before */
        M.d[M_PHONE].avrcp_drop_on_close = 1;     /* field #1: NO CARRIER 2 with its A2DP CLOSE */
        M.d[M_PHONE].blip_when_paused = 1;
        M.d[M_PHONE].link_start_ms = 1900;        /* CONNECT at 1.5 s, its START 0.4 s later */
        M.d[M_PHONE].honor_start = 1;
        m_addr(M_PC, pc);
        m_addr(M_PHONE, phone);
        m_connect_in(M_PHONE);
        m_play(M_PHONE);
        m_run(1000, true);
        m_connect_in(M_PC);
        m_run(2000, true);
        m_play(M_PC);
        m_run(1000, true);
        CHECK(m_selected(M_PHONE) && m_audible(M_PHONE) && !M.d[M_PC].streaming);
        /* #1 phone -> PC */
        CHECK_EQ(bts_use(&b, pc, true), BTS_OK);
        m_until_streams(M_PC, 8000);
        m_run(1500, true);
        CHECK(m_audible(M_PC) && !M.d[M_PC].paused);
        CHECK_EQ(M.plays_to[M_PC], 0);
        CHECK(M.d[M_PHONE].a2dp >= 0 && M.d[M_PHONE].avrcp >= 0);   /* suspended, every link kept */
        CHECK_EQ(bts_action(&b, BTS_ACT_MUTE, phone), BTS_OK);     /* the user's act 6: the 11-03 state */
        m_run(500, true);
        CHECK(M.d[M_PHONE].a2dp < 0 && M.d[M_PHONE].avrcp < 0 && M.d[M_PHONE].paused);
        CHECK(m_dev(M_PHONE) >= 0);
        CHECK(m_dev(M_PHONE) >= 0 && (bts_dev_ps(&b, m_dev(M_PHONE)) & BTS_PSB_OURS));   /* by address, its links gone */
        CHECK(m_dev(M_PHONE) >= 0 && (bts_dev_state2(&b, m_dev(M_PHONE)) & BTS_D2_PARKED));
        /* #2 PC -> phone: the field failure */
        int before = nsent, starts = (int)b.n.starts;
        CHECK_EQ(bts_use(&b, phone, true), BTS_OK);
        m_run(8000, true);
        CHECK((int)b.n.starts >= starts + 1);                  /* the blip went by */
        CHECK(M.d[M_PHONE].streaming == !variant);
        CHECK(variant || !M.d[M_PHONE].paused);                /* the phone plays (variant 1: nothing can unpause it) */
        CHECK(!M.d[M_PC].streaming && M.d[M_PC].paused);
        CHECK_EQ(M.plays_to[M_PC], 0);
        CHECK_EQ(b.sw_result, variant ? BTS_RES_READY : BTS_RES_PLAYING);   /* variant 1: no AVRCP - never the sink's
                                                                              * START: press play on it */
        CHECK_EQ(count_sent("CALL 62:81:81:81:11:a1 19 A2DP"), 1);
        if (!variant) {
            CHECK_EQ(M.plays_to[M_PHONE], 1);                  /* exactly one PLAY */
            CHECK_EQ(calls_of(" 17 AVRCP"), 1);
            CHECK_EQ(sent_index("A2DP STREAMING START", before), -1);
            CHECK_EQ(b.sw_tried & (BTS_SWT_FRESH | BTS_SWT_PLAY), BTS_SWT_FRESH | BTS_SWT_PLAY);
            CHECK(b.sw_last_ms < 5000);
        } else {
            CHECK_EQ(M.plays_to[M_PHONE], 0);
            CHECK_EQ(calls_of(" 17 AVRCP"), 0);
            CHECK_EQ(sent_index("A2DP STREAMING START", before), -1);
            CHECK_EQ(b.sw_tried, BTS_SWT_FRESH);
        }
        if (getenv("BTS_TIMES"))
            printf("    field 11-03 variant %d: PC to phone switch %u ms, tried 0x%02x\n", variant, (unsigned)b.sw_last_ms,
                   (unsigned)b.sw_tried);
        CHECK_EQ(M.closes, 1);                                 /* the act 6 only */
        CHECK(M.d[M_PC].a2dp >= 0 && M.d[M_PC].avrcp >= 0);
        CHECK(only_runtime_sent());
        CHECK_EQ(ac_writes_sent(), 0);
    }
}

/* the safety net after a switch that ended with no PLAY decision: the 11-03 phone, the sink opts out of opening AVRCP
 * (flag 0x20) so the switch ends READY - no AVRCP: never the sink's START (up to f2056ae8 the
 * switch ended on the sink START's stream); the phone opens AVRCP by itself 3.5 s after its link.  a: it reports PAUSED, the piano had paused it -> exactly one late PLAY (tries PLAY | LATE).  b: the user pressed
 * play on the phone first - it reports PLAYING -> no PLAY (Windows-style toggle safety).  c: the user picked the PC
 * before the phone's AVRCP came -> nothing for the phone. */
static void test_btsrc_late_play(void)
{
    bts_t b;
    uint8_t pc[6], phone[6];
    for (int variant = 0; variant < 3; variant++) {
        m_init(&b);
        b.pol.flags |= BTS_F_NO_AVRCP_CALL;
        M.d[M_PC].toggle = 1;
        M.d[M_PHONE].stop_on_pause = 0;
        M.d[M_PHONE].avrcp_drop_on_close = 1;
        M.d[M_PHONE].blip_when_paused = 1;
        M.d[M_PHONE].link_start_ms = 1900;
        M.d[M_PHONE].honor_start = 1;
        M.d[M_PHONE].avrcp_back_ms = 3500;
        m_addr(M_PC, pc);
        m_addr(M_PHONE, phone);
        m_connect_in(M_PHONE);
        m_play(M_PHONE);
        m_run(1000, true);
        m_connect_in(M_PC);
        m_run(2000, true);
        m_play(M_PC);
        m_run(1000, true);
        CHECK_EQ(bts_use(&b, pc, true), BTS_OK);
        m_until_streams(M_PC, 8000);
        m_run(1500, true);
        CHECK_EQ(bts_action(&b, BTS_ACT_MUTE, phone), BTS_OK);   /* the user's act 6: the phone loses A2DP + AVRCP */
        m_run(500, true);
        CHECK(m_audible(M_PC) && M.d[M_PHONE].avrcp < 0);
        CHECK_EQ(bts_use(&b, phone, true), BTS_OK);
        uint32_t t0 = M.now;
        while (b.sw_phase != BTS_SW_IDLE && M.now - t0 < 10000)
            m_run(8, true);
        CHECK_EQ(b.sw_result, BTS_RES_READY);
        CHECK_EQ(b.sw_tried, BTS_SWT_FRESH);
        CHECK(!M.d[M_PHONE].streaming && M.d[M_PHONE].avrcp < 0 && M.d[M_PHONE].paused);
        CHECK_EQ(count_sent("A2DP STREAMING START"), 0);
        CHECK(b.sw_late);
        if (variant == 1)
            m_player(M_PHONE, 0);              /* the user presses play on the phone */
        if (variant == 2)
            CHECK_EQ(bts_use(&b, pc, true), BTS_OK);
        m_run(4000, true);
        CHECK(M.d[M_PHONE].avrcp >= 0 || variant == 2);
        CHECK(!b.sw_late);
        CHECK_EQ(M.plays_to[M_PHONE], variant ? 0 : 1);
        if (!variant) {
            CHECK(!M.d[M_PHONE].paused && M.d[M_PHONE].streaming);   /* the late PLAY: its own stream */
            CHECK_EQ(b.sw_tried, BTS_SWT_FRESH | BTS_SWT_PLAY | BTS_SWT_LATE);
        } else if (variant == 1) {
            CHECK(!M.d[M_PHONE].paused);
            CHECK_EQ(b.sw_tried, BTS_SWT_FRESH);
        }
        CHECK_EQ(count_sent("A2DP STREAMING START"), 0);
        CHECK(only_runtime_sent());
        CHECK_EQ(ac_writes_sent(), 0);
    }
}

/* the PC (Windows) selected and streaming; the user pauses it on the PC - Windows keeps the stream open (field
 * 2026-11-02 #1: STOP 9 s later / never).  「이 기기로 듣기」 on the phone: the PC reports PAUSED -> no PAUSE to it (a PAUSE
 * would be play/pause: it would start again); 0.7 s later the sink suspends its stream (never a CLOSE).  Its player
 * stays paused, its links stay. */
static void test_btsrc_play_status_paused_stream(void)
{
    bts_t b;
    uint8_t phone[6];
    m_init(&b);
    M.d[M_PC].toggle = 1;
    m_addr(M_PHONE, phone);
    m_connect_in(M_PC);
    m_play(M_PC);
    m_run(1000, true);
    m_connect_in(M_PHONE);
    m_run(2000, true);
    m_player(M_PC, 1);                            /* paused on the PC: CHANGED PAUSED, the stream stays */
    M.d[M_PC].silent = 1;
    m_run(1000, true);
    CHECK(M.d[M_PC].streaming && m_selected(M_PC));
    CHECK_EQ(bts_dev_ps(&b, m_dev(M_PC)) & 7, BTS_PS_PAUSED);
    int before = nsent;
    CHECK_EQ(bts_use(&b, phone, true), BTS_OK);
    uint32_t took = m_until_streams(M_PHONE, 8000);
    m_run(300, true);
    CHECK(m_audible(M_PHONE) && !M.d[M_PC].streaming);
    CHECK(M.d[M_PC].paused);                      /* not toggled back on */
    CHECK_EQ(sent_index("AVRCP PAUSE", before), -1);
    CHECK_EQ(b.n.pause_skips, 1);
    CHECK(sent_index("A2DP STREAMING STOP", before) > before);
    CHECK_EQ(count_sent("CLOSE"), 0);
    CHECK(M.d[M_PC].a2dp >= 0 && M.d[M_PC].avrcp >= 0);
    CHECK_EQ(M.plays_to[M_PHONE], 1);             /* idle phone, reported paused: PLAY */
    CHECK(took < 2000);
    CHECK(only_runtime_sent());
}

/* the reply lines: RM 6.10.2 (`AVRCP 3 REGISTER_NOTIFICATION_RSP INTERIM PLAYBACK_STATUS_CHANGED PLAYING`), the
 * AN986 4.3 form with `PDU_`, CHANGED (then registered again), REJ (not again on this link), no answer (1.5 s: not
 * again), the unparsed `RSP PDU_ID` form and other AVRCP lines (nothing); a new AVRCP link starts over */
static void test_btsrc_play_status_lines(void)
{
    bts_t b;
    fresh(&b);
    line(&b, "RING 0 62:81:81:81:11:a1 19 A2DP");
    line(&b, "RING 1 62:81:81:81:11:a1 19 A2DP");
    line(&b, "RING 2 62:81:81:81:11:a1 17 AVRCP");
    run(&b, 200, true);
    CHECK_EQ(count_sent("AVRCP PDU"), 0);         /* BTS_PS_DELAY_MS */
    run(&b, 400, true);
    CHECK(sent_index("SET 2 SELECT", 0) >= 0 && sent_index("AVRCP PDU 31 1", 0) == sent_index("SET 2 SELECT", 0) + 1);
    int d = dev_of(&b, PHONE);
    CHECK(b.dev[d].ps_pend && bts_dev_ps(&b, d) == 0);
    line(&b, "AVRCP PDU 31 1");                   /* the echo: nothing */
    line(&b, "AVRCP 2 PDU_REGISTER_NOTIFICATION_RSP INTERIM PLAYBACK_STATUS_CHANGED PAUSED");
    CHECK_EQ(bts_dev_ps(&b, d), BTS_PS_PAUSED | BTS_PSB_LIVE);
    line(&b, "AVRCP 2 RSP PDU_ID 31, data: 01 01");   /* unparsed form: nothing */
    line(&b, "AVRCP 5103 ERROR_NO_CONTROL_CHANNEL");
    CHECK_EQ(bts_dev_ps(&b, d), BTS_PS_PAUSED | BTS_PSB_LIVE);
    run(&b, 2000, true);
    CHECK_EQ(count_sent("AVRCP PDU 31 1"), 1);    /* registered: nothing more */
    line(&b, "AVRCP 2 REGISTER_NOTIFICATION_RSP CHANGED PLAYBACK_STATUS_CHANGED PLAYING");
    CHECK_EQ(bts_dev_ps(&b, d), BTS_PS_PLAYING);
    run(&b, 400, true);
    CHECK_EQ(count_sent("AVRCP PDU 31 1"), 2);    /* again (a notification fires once) */
    line(&b, "AVRCP 2 REGISTER_NOTIFICATION_RSP INTERIM PLAYBACK_STATUS_CHANGED 2");   /* the number form */
    CHECK_EQ(bts_dev_ps(&b, d), BTS_PS_PAUSED | BTS_PSB_LIVE);
    line(&b, "AVRCP 2 REGISTER_NOTIFICATION_RSP CHANGED PLAYBACK_STATUS_CHANGED STOPPED");
    CHECK_EQ(bts_dev_ps(&b, d) & 7, BTS_PS_STOPPED);
    run(&b, 400, true);
    line(&b, "AVRCP 2 REGISTER_NOTIFICATION_RSP REJ");
    CHECK(bts_dev_ps(&b, d) & BTS_PSB_NONE);
    int n = count_sent("AVRCP PDU 31 1");
    run(&b, 3000, true);
    CHECK_EQ(count_sent("AVRCP PDU 31 1"), n);    /* not again on this link */
    /* the AVRCP link goes and comes: a new start; no answer within 1.5 s -> not again */
    line(&b, "NO CARRIER 2 ERROR 0");
    CHECK_EQ(bts_dev_ps(&b, d), 0);
    line(&b, "RING 3 62:81:81:81:11:a1 17 AVRCP");
    run(&b, 600, true);
    CHECK_EQ(count_sent("AVRCP PDU 31 1"), n + 1);
    run(&b, 1600, true);
    CHECK(bts_dev_ps(&b, d) & BTS_PSB_NONE);
    run(&b, 3000, true);
    CHECK_EQ(count_sent("AVRCP PDU 31 1"), n + 1);
    CHECK(only_runtime_sent());
    /* mode NEWEST: no registration */
    fresh_newest(&b);
    line(&b, "RING 2 62:81:81:81:11:a1 17 AVRCP");
    run(&b, 2000, true);
    CHECK_EQ(count_sent("AVRCP PDU"), 0);
}

/* the power port's BT hold: bts_stream_playing - the play status of the source that streams on a link:
 * 1 PLAYING, 0 PAUSED / STOPPED / SEEKING / ERROR, -1 not known (no device on the link, no AVRCP, not answered yet, REJ,
 * an unknown word); a START without a link id: the one device that streams */
static void test_btsrc_stream_playing(void)
{
    bts_t b;
    fresh(&b);
    CHECK_EQ(bts_stream_playing(&b, 3), -1);       /* no device */
    line(&b, "RING 3 22:82:02:02:92:22 19 A2DP");
    line(&b, "RING 5 22:82:02:02:92:22 19 A2DP");
    line(&b, "A2DP STREAMING START 3");
    CHECK_EQ(bts_stream_playing(&b, 3), -1);       /* no AVRCP */
    line(&b, "RING 4 22:82:02:02:92:22 17 AVRCP");
    run(&b, 600, true);
    CHECK_EQ(bts_stream_playing(&b, 3), -1);       /* registered, no answer yet */
    line(&b, "AVRCP 4 REGISTER_NOTIFICATION_RSP INTERIM PLAYBACK_STATUS_CHANGED PLAYING");
    CHECK_EQ(bts_stream_playing(&b, 3), 1);
    CHECK_EQ(bts_stream_playing(&b, -1), 1);       /* no link id: the one that streams */
    line(&b, "AVRCP 4 REGISTER_NOTIFICATION_RSP CHANGED PLAYBACK_STATUS_CHANGED PAUSED");
    CHECK_EQ(bts_stream_playing(&b, 3), 0);        /* Windows: paused, the stream stays open */
    run(&b, 400, true);                            /* registered again: still paused */
    line(&b, "AVRCP 4 REGISTER_NOTIFICATION_RSP INTERIM PLAYBACK_STATUS_CHANGED PAUSED");
    CHECK_EQ(bts_stream_playing(&b, 3), 0);
    line(&b, "AVRCP 4 REGISTER_NOTIFICATION_RSP CHANGED PLAYBACK_STATUS_CHANGED STOPPED");
    CHECK_EQ(bts_stream_playing(&b, 3), 0);
    run(&b, 400, true);
    line(&b, "AVRCP 4 REGISTER_NOTIFICATION_RSP INTERIM PLAYBACK_STATUS_CHANGED ERROR");
    CHECK_EQ(bts_dev_ps(&b, dev_of(&b, PC)) & 7, BTS_PS_ERROR);
    CHECK_EQ(bts_stream_playing(&b, 3), 0);
    line(&b, "AVRCP 4 REGISTER_NOTIFICATION_RSP CHANGED PLAYBACK_STATUS_CHANGED FWD_SEEK");
    CHECK_EQ(bts_stream_playing(&b, 3), 0);
    run(&b, 400, true);
    line(&b, "AVRCP 4 REGISTER_NOTIFICATION_RSP INTERIM PLAYBACK_STATUS_CHANGED BOGUS");
    CHECK_EQ(bts_stream_playing(&b, 3), -1);       /* a word not known: not known */
    line(&b, "AVRCP 4 REGISTER_NOTIFICATION_RSP CHANGED PLAYBACK_STATUS_CHANGED PLAYING");
    CHECK_EQ(bts_stream_playing(&b, 3), 1);
    run(&b, 400, true);
    line(&b, "AVRCP 4 REGISTER_NOTIFICATION_RSP REJ");   /* the re-registration rejected: not known any more */
    CHECK_EQ(bts_stream_playing(&b, 3), -1);
    /* two streams, no link id: not known */
    line(&b, "RING 0 62:81:81:81:11:a1 19 A2DP");
    line(&b, "A2DP STREAMING START 0");
    CHECK_EQ(bts_stream_playing(&b, -1), -1);
    /* the AVRCP link goes: not known */
    fresh(&b);
    line(&b, "RING 3 22:82:02:02:92:22 19 A2DP");
    line(&b, "RING 4 22:82:02:02:92:22 17 AVRCP");
    line(&b, "A2DP STREAMING START 3");
    line(&b, "AVRCP 4 REGISTER_NOTIFICATION_RSP INTERIM PLAYBACK_STATUS_CHANGED PAUSED");
    CHECK_EQ(bts_stream_playing(&b, 3), 0);
    line(&b, "NO CARRIER 4 ERROR 0");
    CHECK_EQ(bts_stream_playing(&b, 3), -1);
}

/* the model's Windows fact (field 2026-11-02 evening): once the sink stopped its stream, every later stream on that A2DP
 * connection is silent (the module shows STREAMING START) until the connection is new.  The manager never sends that
 * STOP: across a switch round trip and a guard episode it stays at 0, and every stream is audible. */
static void test_btsrc_windows_silent_after_sink_stop(void)
{
    bts_t b;
    m_init(&b);
    M.d[M_PC].silent_after_stop = 1;
    m_connect_in(M_PC);
    m_play(M_PC);
    m_run(300, true);
    M.sel = M.d[M_PC].a2dp;                        /* what an old firmware did: SET 0 SELECT + A2DP STREAMING STOP */
    m_command("A2DP STREAMING STOP");
    m_run(300, true);
    CHECK(!M.d[M_PC].streaming);
    m_play(M_PC);                             /* the user presses play / toggles the output */
    CHECK(M.d[M_PC].streaming && !m_audible(M_PC));
    CHECK_EQ(M.silent_starts, 1);
    m_drop(M_PC);                                  /* a new connection (module restart / re-pair) */
    m_connect_in(M_PC);
    m_play(M_PC);
    CHECK(m_audible(M_PC));
}

/* GUARD: the phone is selected and paused; the PC (Windows) starts by itself -> AVRCP PAUSE; it keeps streaming ->
 * 0.7 s later the sink suspends it (never a CLOSE - every link stays); FOLLOW: the PC becomes the selected one */
static void test_btsrc_guard(void)
{
    bts_t b;
    m_init(&b);
    m_connect_in(M_PHONE);
    m_play(M_PHONE);
    m_connect_in(M_PC);
    m_run(1000, true);
    CHECK(m_selected(M_PHONE));
    M.d[M_PHONE].streaming = 0;
    line(&b, "A2DP STREAMING STOP 0");             /* the user paused the phone */
    m_play(M_PC);
    m_run(600, true);
    CHECK_EQ(b.n.refusals, 1);
    CHECK_EQ(count_sent("AVRCP PAUSE"), 1);
    CHECK(bts_dev_state2(&b, m_dev(M_PC)) & BTS_D2_REFUSED);
    m_run(1000, true);
    {   /* the PC's A2DP signalling link (links 3/4, AVRCP 5) suspended */
        int c = sent_index("A2DP STREAMING STOP", 0);
        CHECK(c > 0 && !strcmp(sent[c - 1], "SET 3 SELECT"));
    }
    CHECK_EQ(count_sent("CLOSE"), 0);
    CHECK(!M.d[M_PC].streaming && M.d[M_PC].a2dp >= 0 && M.d[M_PC].avrcp >= 0);
    CHECK(m_selected(M_PHONE));
    m_play(M_PHONE);                          /* the phone can play: nothing holds the stream */
    m_run(500, true);
    CHECK(m_audible(M_PHONE));
    CHECK(only_runtime_sent());
    /* a phone (stops on pause) that is not selected: paused only, nothing closed */
    m_init(&b);
    m_connect_in(M_PC);
    m_play(M_PC);
    m_connect_in(M_PHONE);
    m_run(1000, true);
    m_stop(M_PC);
    m_play(M_PHONE);
    m_run(2000, true);
    CHECK_EQ(b.n.refusals, 1);
    CHECK(!M.d[M_PHONE].streaming);
    CHECK_EQ(count_sent("CLOSE"), 0);
    CHECK(m_selected(M_PC));
    /* FOLLOW */
    m_init(&b);
    b.pol.flags |= BTS_F_FOLLOW;
    m_connect_in(M_PHONE);
    m_play(M_PHONE);
    m_connect_in(M_PC);
    m_run(1000, true);
    m_stop(M_PHONE);
    m_play(M_PC);
    m_run(1000, true);
    CHECK(m_selected(M_PC));
    CHECK_EQ(b.n.refusals, 0);
    CHECK(M.d[M_PC].streaming);
    /* the selected one is gone: the next source that streams is adopted, nothing refused */
    m_init(&b);
    m_connect_in(M_PHONE);
    m_play(M_PHONE);
    m_connect_in(M_PC);
    m_run(1000, true);
    m_drop(M_PHONE);
    m_play(M_PC);
    m_run(1000, true);
    CHECK(m_selected(M_PC));
    CHECK_EQ(b.n.refusals, 0);
    /* mode OFF: table only, nothing automatic */
    m_init(&b);
    b.pol.mode = BTS_MODE_OFF;
    m_connect_in(M_PHONE);
    m_connect_in(M_PC);
    line(&b, "A2DP STREAMING START 0");
    line(&b, "A2DP STREAMING START 3");
    m_run(20000, true);
    CHECK_EQ(b.n.refusals, 0);                     /* two streams, nothing refused or closed */
    CHECK_EQ(count_sent("LIST"), nsent);
}

/* field #3 with AUTOCALL on: the phone drops, the module itself re-links the PC (OUTGOING CONNECT, no address) and
 * it streams -> LIST learns the address, the PC is adopted (the selected phone is gone); nothing is CALLed by us */
static void test_btsrc_autocall_on(void)
{
    bts_t b;
    m_init(&b);
    m_connect_in(M_PHONE);
    m_play(M_PHONE);
    m_run(4000, true);
    CHECK_EQ(nreads, 1);                           /* someone to reconnect: AUTOCALL read once */
    CHECK_EQ(b.ac_state, BTS_AC_ON);
    CHECK(!strcmp(bts_autocall_line(&b), "SET CONTROL AUTOCALL 19 5000 A2DP"));
    m_drop(M_PHONE);
    m_run(12000, true);                            /* AUTOCALL 7 s, then the sink's AVRCP 4 s after the PC's A2DP */
    CHECK(M.d[M_PC].streaming);
    CHECK(m_selected(M_PC));
    CHECK_EQ(calls_of(" 19 A2DP"), 0);            /* AUTOCALL linked it, not us */
    CHECK_EQ(calls_of(" 17 AVRCP"), 1);           /* its AUTOCALL link has no AVRCP: the sink opens it, once */
    CHECK(!strcmp(sent[nsent - 1], "CALL 02:1a:7d:da:13:23 17 AVRCP") || count_sent("CALL 02:1a:7d:da:13:23 17 AVRCP") == 1);
    CHECK(M.d[M_PC].avrcp >= 0);
    CHECK(bts_dev_state2(&b, m_dev(M_PC)) & BTS_D2_AVRCP);
    CHECK_EQ(ac_writes_sent(), 0);
    /* the pairing key of the listing never reaches the line log */
    uint8_t out[1600];
    uint16_t first, next;
    uint32_t n = bts_log_read(&b, 0, out, sizeof out, &first, &next);
    bool key = false;
    for (uint32_t i = 0; i + 8 <= n; i++)
        key |= memcmp(out + i, "01234567", 8) == 0;
    CHECK(!key);
}

/* the explicit AUTOCALL op: refused before a read; off -> `SET CONTROL AUTOCALL` + read back; then our manager owns
 * reconnecting (the TV, preferred, sink-initiated after it dropped - backoff, never while a source has A2DP, never in
 * standby); restore writes the exact line read before; nothing of it without the op */
static void test_btsrc_autocall_op(void)
{
    bts_t b;
    m_init(&b);
    CHECK_EQ(bts_autocall(&b, BTS_AC_SET_OFF), BTS_STATE);   /* not read yet */
    CHECK_EQ(bts_autocall(&b, BTS_AC_RESTORE), BTS_STATE);
    CHECK_EQ(bts_autocall(&b, 9), BTS_BAD);
    CHECK_EQ(bts_autocall(&b, BTS_AC_READ), BTS_OK);
    m_run(500, true);
    CHECK_EQ(b.ac_state, BTS_AC_ON);
    CHECK_EQ(b.ac_op, 0);
    CHECK_EQ(ac_writes_sent(), 0);
    snprintf(M.ac_line, sizeof M.ac_line, "SET CONTROL AUTOCALL 19 7000 A2DP");   /* a value other than the factory one */
    CHECK_EQ(bts_autocall(&b, BTS_AC_READ), BTS_OK);
    m_run(500, true);
    CHECK(!strcmp(bts_autocall_line(&b), "SET CONTROL AUTOCALL 19 5000 A2DP"));   /* the first value read is kept */
    m_init(&b);
    snprintf(M.ac_line, sizeof M.ac_line, "SET CONTROL AUTOCALL 19 7000 A2DP");
    CHECK_EQ(bts_autocall(&b, BTS_AC_READ), BTS_OK);
    m_run(500, true);
    CHECK_EQ(bts_autocall(&b, BTS_AC_SET_OFF), BTS_OK);
    CHECK_EQ(bts_autocall(&b, BTS_AC_SET_OFF), BTS_FULL);    /* one op at a time */
    m_run(600, true);
    CHECK_EQ(count_sent("SET CONTROL AUTOCALL"), 1);
    CHECK(!strcmp(sent[nsent - 1], "SET CONTROL AUTOCALL"));
    CHECK_EQ(M.autocall, 0);
    CHECK_EQ(b.ac_state, BTS_AC_OFF);
    CHECK_EQ(b.ac_rc, BTS_OK);
    /* the TV is preferred; it is called (it cannot call the piano), streams; then it drops: called again after 4 s */
    b.pol.has_pref = 1;
    m_addr(M_TV, b.pol.pref);
    m_run(4500, true);
    CHECK_EQ(count_sent("CALL 12:84:04:04:14:24 19 A2DP"), 1);
    m_run(3000, true);
    CHECK(M.d[M_TV].streaming);
    CHECK(m_selected(M_TV));
    m_drop(M_TV);
    m_run(3000, true);
    CHECK_EQ(count_sent("CALL 12:84:04:04:14:24 19 A2DP"), 1);
    m_run(1500, true);
    CHECK_EQ(count_sent("CALL 12:84:04:04:14:24 19 A2DP"), 2);
    m_run(3000, true);
    CHECK(M.d[M_TV].streaming);
    /* never while another source has A2DP; never in standby */
    m_drop(M_TV);
    m_connect_in(M_PHONE);
    m_run(20000, true);
    CHECK_EQ(calls_of(" 19 A2DP"), 2);
    m_drop(M_PHONE);
    m_run(20000, false);
    CHECK_EQ(calls_of(" 19 A2DP"), 2);
    /* a TV that never answers: backoff 4 / 8 / 15 / 30 / 60 s, 10 calls at most */
    M.d[M_TV].mac = "12:84:04:04:14:25";           /* the model does not know it: CALL fails silently */
    nsent = 0;
    m_run(60000, true);
    CHECK_EQ(calls_of(" 19 A2DP"), 4);               /* t = 4, 12, 27, 57 s */
    m_run(600000, true);
    CHECK_EQ(calls_of(" 19 A2DP"), BTS_RC_TRIES);
    /* restore: exactly the line read before */
    nsent = 0;
    CHECK_EQ(bts_autocall(&b, BTS_AC_RESTORE), BTS_OK);
    m_run(600, true);
    CHECK_EQ(count_sent("SET CONTROL AUTOCALL"), 1);
    CHECK(!strcmp(sent[0], "SET CONTROL AUTOCALL 19 7000 A2DP"));
    CHECK_EQ(b.ac_state, BTS_AC_ON);
    CHECK_EQ(b.ac_rc, BTS_OK);
    CHECK_EQ(bts_autocall(&b, BTS_AC_RESTORE), BTS_STATE);
    /* after a power cycle the saved line is gone: restore writes the stock factory line */
    m_init(&b);
    M.autocall = 0;
    CHECK_EQ(bts_autocall(&b, BTS_AC_READ), BTS_OK);
    m_run(500, true);
    CHECK_EQ(b.ac_state, BTS_AC_OFF);
    CHECK_EQ(bts_autocall(&b, BTS_AC_RESTORE), BTS_OK);
    m_run(600, true);
    CHECK_EQ(count_sent("SET CONTROL AUTOCALL 19 5000 A2DP"), 1);
    /* standby drops a pending op */
    CHECK_EQ(bts_autocall(&b, BTS_AC_READ), BTS_OK);
    m_run(100, false);
    CHECK_EQ(b.ac_op, 0);
}

/* the explicit class-of-device op (field 2026-11-09: an Android TV's pairing list dropped the class-001f00 sink):
 * refused before a read; write -> exactly `SET BT CLASS 240414` + `SET`, read back; one op at a time with the AUTOCALL
 * op; restore writes the class read before; after a power cycle with the audio class kept by the module, restore falls
 * back to 001f00; a class that is not an audio sink one is refused; standby drops a pending op; the manager never
 * writes it by itself */
static void test_btsrc_class_op(void)
{
    bts_t b;
    m_init(&b);
    CHECK(bts_cod_ok(BTS_COD_AUDIO_SINK));
    CHECK(bts_cod_ok(0x240418u));                 /* headphones */
    CHECK(!bts_cod_ok(BTS_COD_IWRAP));
    CHECK(!bts_cod_ok(0x200414u));                /* no Rendering bit */
    CHECK(!bts_cod_ok(0x240514u));                /* major Peripheral */
    CHECK(!bts_cod_ok(0x240415u));                /* format bits */
    CHECK(!bts_cod_ok(0x1240414u));
    CHECK_EQ(bts_class(&b, BTS_CO_WRITE, 0), BTS_STATE);      /* not read yet */
    CHECK_EQ(bts_class(&b, BTS_CO_RESTORE, 0), BTS_STATE);
    CHECK_EQ(bts_class(&b, 9, 0), BTS_BAD);
    CHECK_EQ(bts_class(&b, BTS_CO_WRITE, 0x001f00u), BTS_BAD);
    CHECK_EQ(bts_class(&b, BTS_CO_READ, 0), BTS_OK);
    CHECK_EQ(bts_autocall(&b, BTS_AC_READ), BTS_FULL);       /* one module-setting op at a time */
    CHECK_EQ(bts_class(&b, BTS_CO_READ, 0), BTS_FULL);
    m_run(500, true);
    CHECK_EQ(b.cod_state, BTS_COD_KNOWN);
    CHECK_EQ(b.cod, 0x001f00u);
    CHECK_EQ(b.cod_saved, 1);
    CHECK_EQ(b.cod_op, 0);
    CHECK_EQ(b.cod_rc, BTS_OK);
    CHECK_EQ(ac_writes_sent(), 0);
    CHECK_EQ(b.ac_state, BTS_AC_ON);              /* the same listing also told AUTOCALL */
    CHECK_EQ(bts_class(&b, BTS_CO_RESTORE, 0), BTS_STATE);   /* already the value read before */
    nsent = 0;
    nreads = 0;
    CHECK_EQ(bts_class(&b, BTS_CO_WRITE, 0), BTS_OK);
    CHECK_EQ(bts_autocall(&b, BTS_AC_SET_OFF), BTS_FULL);    /* the class read-back is pending */
    CHECK_EQ(bts_class(&b, BTS_CO_WRITE, 0), BTS_FULL);
    m_run(600, true);
    CHECK_EQ(nsent, 1);                           /* the write; its read back `SET` counts in nreads */
    CHECK(!strcmp(sent[0], "SET BT CLASS 240414"));
    CHECK_EQ(nreads, 1);
    CHECK_EQ(M.cod, 0x240414u);
    CHECK_EQ(b.cod, 0x240414u);
    CHECK_EQ(b.cod_rc, BTS_OK);
    CHECK_EQ(b.cod_want, 0x240414u);
    CHECK_EQ(b.n.cod_writes, 1);
    CHECK_EQ(bts_class_restore(&b), 0x001f00u);  /* the class read first is kept */
    CHECK_EQ(bts_class(&b, BTS_CO_WRITE, 0), BTS_STATE);     /* already that class */
    /* another audio sink class (headphones) - the op's own value */
    CHECK_EQ(bts_class(&b, BTS_CO_WRITE, 0x240418u), BTS_OK);
    m_run(600, true);
    CHECK_EQ(count_sent("SET BT CLASS 240418"), 1);
    CHECK_EQ(b.cod, 0x240418u);
    CHECK_EQ(b.cod_rc, BTS_OK);
    /* restore: exactly the class read before */
    nsent = 0;
    CHECK_EQ(bts_class(&b, BTS_CO_RESTORE, 0), BTS_OK);
    m_run(600, true);
    CHECK_EQ(nsent, 1);
    CHECK(!strcmp(sent[0], "SET BT CLASS 001f00"));
    CHECK_EQ(M.cod, 0x001f00u);
    CHECK_EQ(b.cod_rc, BTS_OK);
    /* after a power cycle the module keeps the audio class: nothing saved, restore falls back to 001f00 */
    m_init(&b);
    M.cod = 0x240414u;
    CHECK_EQ(bts_class(&b, BTS_CO_READ, 0), BTS_OK);
    m_run(500, true);
    CHECK_EQ(b.cod_saved, 0);
    CHECK_EQ(bts_class_restore(&b), BTS_COD_IWRAP);
    CHECK_EQ(bts_class(&b, BTS_CO_RESTORE, 0), BTS_OK);
    m_run(600, true);
    CHECK_EQ(count_sent("SET BT CLASS 001f00"), 1);
    CHECK_EQ(b.cod, 0x001f00u);
    CHECK_EQ(b.cod_saved, 1);                     /* the listing after it: 001f00 is now the class read before */
    /* the module ignoring the write: rc STATE from the read-back */
    m_init(&b);
    CHECK_EQ(bts_class(&b, BTS_CO_READ, 0), BTS_OK);
    m_run(500, true);
    CHECK_EQ(bts_class(&b, BTS_CO_WRITE, 0), BTS_OK);
    for (int i = 0; i < 80 && b.cod_op; i++) {
        m_run(8, true);
        M.cod = 0x001f00u;                        /* the module keeps its class */
    }
    CHECK_EQ(b.cod_op, 0);
    CHECK_EQ(b.cod_rc, BTS_STATE);
    /* standby drops a pending op */
    CHECK_EQ(bts_class(&b, BTS_CO_READ, 0), BTS_OK);
    m_run(100, false);
    CHECK_EQ(b.cod_op, 0);
    CHECK_EQ(b.cod_rc, BTS_FULL);
}

/* the explicit AVRCP features op (field 2026-11-10: a Windows PC's lone browsing channel 0x1b
 * rebooted the module): refused before a read; write -> exactly `SET PROFILE AVRCP CONTROLLER 0f` + `SET`, read back;
 * one op at a time with the AUTOCALL / class ops; restore writes the features read before (1f); after a power cycle
 * with 0f kept, restore falls back to the factory 1f; features with the browsing bit / no category are refused; a
 * listing with a TARGET part or without AVRCP is never rewritten; standby drops a pending op; never by itself */
static void test_btsrc_avrcp_features_op(void)
{
    bts_t b;
    m_init(&b);
    CHECK(bts_avf_ok(BTS_AVF_NO_BROWSE));
    CHECK(bts_avf_ok(0x01));
    CHECK(!bts_avf_ok(0x00));                     /* AN986 3.2: at least one category */
    CHECK(!bts_avf_ok(BTS_AVF_FACTORY));          /* the browsing bit */
    CHECK(!bts_avf_ok(0x10));
    CHECK_EQ(bts_avf_restore(&b), BTS_AVF_FACTORY);
    CHECK_EQ(bts_avrcp_features(&b, BTS_AV_WRITE, 0), BTS_STATE);     /* not read yet */
    CHECK_EQ(bts_avrcp_features(&b, BTS_AV_RESTORE, 0), BTS_STATE);
    CHECK_EQ(bts_avrcp_features(&b, 9, 0), BTS_BAD);
    CHECK_EQ(bts_avrcp_features(&b, BTS_AV_WRITE, 0x1f), BTS_BAD);
    CHECK_EQ(bts_avrcp_features(&b, BTS_AV_READ, 0), BTS_OK);
    CHECK_EQ(bts_class(&b, BTS_CO_READ, 0), BTS_FULL);              /* one module-setting op at a time */
    CHECK_EQ(bts_autocall(&b, BTS_AC_READ), BTS_FULL);
    CHECK_EQ(bts_avrcp_features(&b, BTS_AV_READ, 0), BTS_FULL);
    m_run(500, true);
    CHECK_EQ(b.avf_state, BTS_AVF_CTRL);
    CHECK_EQ(b.avf, 0x1f);
    CHECK_EQ(b.avf_saved, 1);
    CHECK_EQ(b.avf_op, 0);
    CHECK_EQ(b.avf_rc, BTS_OK);
    CHECK_EQ(ac_writes_sent(), 0);
    CHECK_EQ(b.cod_state, BTS_COD_KNOWN);         /* the same listing also told the class */
    CHECK_EQ(bts_avrcp_features(&b, BTS_AV_RESTORE, 0), BTS_STATE);  /* already the features read before */
    nsent = 0;
    nreads = 0;
    CHECK_EQ(bts_avrcp_features(&b, BTS_AV_WRITE, 0), BTS_OK);
    CHECK_EQ(bts_class(&b, BTS_CO_READ, 0), BTS_FULL);              /* the AVRCP read-back is pending */
    m_run(600, true);
    CHECK_EQ(nsent, 1);
    CHECK(!strcmp(sent[0], "SET PROFILE AVRCP CONTROLLER 0f"));
    CHECK_EQ(nreads, 1);
    CHECK_EQ(M.avf, 0x0fu);
    CHECK_EQ(b.avf, 0x0f);
    CHECK_EQ(b.avf_rc, BTS_OK);
    CHECK_EQ(b.n.avf_writes, 1);
    CHECK_EQ(b.n.cod_writes, 0);
    CHECK_EQ(bts_avf_restore(&b), 0x1f);          /* the features read first are kept */
    CHECK_EQ(bts_avrcp_features(&b, BTS_AV_WRITE, 0), BTS_STATE);   /* already that */
    CHECK_EQ(bts_avrcp_features(&b, BTS_AV_WRITE, 0x01), BTS_OK);   /* category 1 only - the op's own value */
    m_run(600, true);
    CHECK_EQ(count_sent("SET PROFILE AVRCP CONTROLLER 01"), 1);
    CHECK_EQ(b.avf, 0x01);
    /* restore: exactly the features read before */
    nsent = 0;
    CHECK_EQ(bts_avrcp_features(&b, BTS_AV_RESTORE, 0), BTS_OK);
    m_run(600, true);
    CHECK_EQ(nsent, 1);
    CHECK(!strcmp(sent[0], "SET PROFILE AVRCP CONTROLLER 1f"));
    CHECK_EQ(M.avf, 0x1fu);
    CHECK_EQ(b.avf_rc, BTS_OK);
    /* after a power cycle the module keeps 0f: nothing saved, restore falls back to the factory 1f */
    m_init(&b);
    M.avf = 0x0f;
    CHECK_EQ(bts_avrcp_features(&b, BTS_AV_READ, 0), BTS_OK);
    m_run(500, true);
    CHECK_EQ(b.avf_saved, 0);
    CHECK_EQ(bts_avf_restore(&b), BTS_AVF_FACTORY);
    CHECK_EQ(bts_avrcp_features(&b, BTS_AV_RESTORE, 0), BTS_OK);
    m_run(600, true);
    CHECK_EQ(count_sent("SET PROFILE AVRCP CONTROLLER 1f"), 1);
    CHECK_EQ(b.avf, 0x1f);
    CHECK_EQ(b.avf_saved, 1);
    /* a TARGET part / no AVRCP line: read, never rewritten */
    for (int form = 1; form <= 2; form++) {
        m_init(&b);
        M.avf_form = form;
        CHECK_EQ(bts_avrcp_features(&b, BTS_AV_READ, 0), BTS_OK);
        m_run(500, true);
        CHECK_EQ(b.avf_state, BTS_AVF_OTHER);
        CHECK_EQ(b.avf_rc, BTS_OK);
        CHECK_EQ(bts_avrcp_features(&b, BTS_AV_WRITE, 0), BTS_STATE);
        CHECK_EQ(bts_avrcp_features(&b, BTS_AV_RESTORE, 0), BTS_STATE);
        CHECK_EQ(ac_writes_sent(), 0);
    }
    /* the module ignoring the write: rc STATE from the read-back */
    m_init(&b);
    CHECK_EQ(bts_avrcp_features(&b, BTS_AV_READ, 0), BTS_OK);
    m_run(500, true);
    CHECK_EQ(bts_avrcp_features(&b, BTS_AV_WRITE, 0), BTS_OK);
    for (int i = 0; i < 80 && b.avf_op; i++) {
        m_run(8, true);
        M.avf = 0x1f;                             /* the module keeps its features */
    }
    CHECK_EQ(b.avf_op, 0);
    CHECK_EQ(b.avf_rc, BTS_STATE);
    /* standby drops a pending op */
    CHECK_EQ(bts_avrcp_features(&b, BTS_AV_READ, 0), BTS_OK);
    m_run(100, false);
    CHECK_EQ(b.avf_op, 0);
    CHECK_EQ(b.avf_rc, BTS_FULL);
}

/* the explicit audio routing op (field 2026-11-11): the line formed for
 * each flag set (RM 6.67 order); refused before a read; write -> exactly `SET CONTROL AUDIO INTERNAL I2S_SLAVE EVENT`
 * (default) or `… EVENT KEEPALIVE` + `SET`, read back; one op at a time with the other module-setting ops; restore
 * writes the line read before (`… KEEPALIVE`); after a power cycle with EVENT kept, restore falls back to the factory
 * line; any other flag set refused; another form / no line never rewritten; standby drops a pending op */
static void test_btsrc_audio_op(void)
{
    bts_t b;
    char w[BTS_OUT_MAX];
    bts_audio_line(w, BTS_AUD_FACTORY);
    CHECK(!strcmp(w, "SET CONTROL AUDIO INTERNAL I2S_SLAVE KEEPALIVE"));   /* = REA 0x4013EE30 without CR LF */
    bts_audio_line(w, BTS_AUD_EVENT);
    CHECK(!strcmp(w, "SET CONTROL AUDIO INTERNAL I2S_SLAVE EVENT"));
    bts_audio_line(w, BTS_AUD_EVENT | BTS_AUD_KEEP);
    CHECK(!strcmp(w, "SET CONTROL AUDIO INTERNAL I2S_SLAVE EVENT KEEPALIVE"));
    CHECK(strlen(w) < BTS_OUT_MAX);
    bts_audio_line(w, 0);
    CHECK(!strcmp(w, "SET CONTROL AUDIO INTERNAL I2S_SLAVE"));
    CHECK(bts_aud_ok(BTS_AUD_EVENT));
    CHECK(bts_aud_ok(BTS_AUD_EVENT | BTS_AUD_KEEP));
    CHECK(!bts_aud_ok(0));
    CHECK(!bts_aud_ok(BTS_AUD_KEEP));             /* the factory flags: only restore writes them */
    CHECK(!bts_aud_ok(4));
    m_init(&b);
    CHECK_EQ(bts_aud_restore(&b), BTS_AUD_FACTORY);
    CHECK_EQ(bts_audio(&b, BTS_AU_WRITE, 0), BTS_STATE);      /* not read yet */
    CHECK_EQ(bts_audio(&b, BTS_AU_RESTORE, 0), BTS_STATE);
    CHECK_EQ(bts_audio(&b, 4, 0), BTS_BAD);
    CHECK_EQ(bts_audio(&b, 0, 0), BTS_BAD);
    CHECK_EQ(bts_audio(&b, BTS_AU_WRITE, BTS_AUD_KEEP), BTS_BAD);
    CHECK_EQ(bts_audio(&b, BTS_AU_READ, 0), BTS_OK);
    CHECK_EQ(bts_avrcp_features(&b, BTS_AV_READ, 0), BTS_FULL);   /* one module-setting op at a time */
    CHECK_EQ(bts_class(&b, BTS_CO_READ, 0), BTS_FULL);
    CHECK_EQ(bts_audio(&b, BTS_AU_READ, 0), BTS_FULL);
    m_run(500, true);
    CHECK_EQ(b.aud_state, BTS_AUD_STD);
    CHECK_EQ(b.aud, BTS_AUD_KEEP);
    CHECK_EQ(b.aud_saved, 1);
    CHECK_EQ(b.aud_op, 0);
    CHECK_EQ(b.aud_rc, BTS_OK);
    CHECK_EQ(b.avf_state, BTS_AVF_CTRL);          /* the same listing told the others */
    CHECK_EQ(b.avf, 0x1f);
    CHECK_EQ(ac_writes_sent(), 0);
    CHECK_EQ(bts_audio(&b, BTS_AU_RESTORE, 0), BTS_STATE);    /* already the line read before */
    nsent = 0;
    nreads = 0;
    CHECK_EQ(bts_audio(&b, BTS_AU_WRITE, 0), BTS_OK);        /* variant (1): KEEPALIVE off, EVENT on */
    CHECK_EQ(bts_avrcp_features(&b, BTS_AV_READ, 0), BTS_FULL);
    m_run(600, true);
    CHECK_EQ(nsent, 1);
    CHECK(!strcmp(sent[0], "SET CONTROL AUDIO INTERNAL I2S_SLAVE EVENT"));
    CHECK_EQ(nreads, 1);
    CHECK_EQ(M.aud, BTS_AUD_EVENT);
    CHECK_EQ(b.aud, BTS_AUD_EVENT);
    CHECK_EQ(b.aud_rc, BTS_OK);
    CHECK_EQ(bts_aud_restore(&b), BTS_AUD_KEEP);  /* the flags read first are kept */
    CHECK_EQ(bts_audio(&b, BTS_AU_WRITE, BTS_AUD_EVENT), BTS_STATE);   /* already that */
    CHECK_EQ(bts_audio(&b, BTS_AU_WRITE, BTS_AUD_EVENT | BTS_AUD_KEEP), BTS_OK);   /* variant (2): events only */
    m_run(600, true);
    CHECK_EQ(count_sent("SET CONTROL AUDIO INTERNAL I2S_SLAVE EVENT KEEPALIVE"), 1);
    CHECK_EQ(b.aud, BTS_AUD_EVENT | BTS_AUD_KEEP);
    CHECK_EQ(bts_aud_restore(&b), BTS_AUD_KEEP);
    /* restore: exactly the line read before */
    nsent = 0;
    CHECK_EQ(bts_audio(&b, BTS_AU_RESTORE, 0), BTS_OK);
    m_run(600, true);
    CHECK_EQ(nsent, 1);
    CHECK(!strcmp(sent[0], "SET CONTROL AUDIO INTERNAL I2S_SLAVE KEEPALIVE"));
    CHECK_EQ(M.aud, BTS_AUD_KEEP);
    CHECK_EQ(b.aud_rc, BTS_OK);
    CHECK_EQ(M.aud_writes, 3);
    CHECK_EQ(count_sent("SET PROFILE"), 0);
    /* a module whose own line had no flags: restore writes that back (not the factory line) */
    m_init(&b);
    M.aud = 0;
    CHECK_EQ(bts_audio(&b, BTS_AU_READ, 0), BTS_OK);
    m_run(500, true);
    CHECK_EQ(b.aud_saved, 1);
    CHECK_EQ(bts_aud_restore(&b), 0);
    CHECK_EQ(bts_audio(&b, BTS_AU_WRITE, 0), BTS_OK);
    m_run(600, true);
    CHECK_EQ(bts_audio(&b, BTS_AU_RESTORE, 0), BTS_OK);
    m_run(600, true);
    CHECK(nsent > 0 && !strcmp(sent[nsent - 1], "SET CONTROL AUDIO INTERNAL I2S_SLAVE"));
    CHECK_EQ(M.aud, 0u);
    /* after a power cycle the module keeps EVENT: nothing saved, restore falls back to the factory line */
    m_init(&b);
    M.aud = BTS_AUD_EVENT;
    CHECK_EQ(bts_audio(&b, BTS_AU_READ, 0), BTS_OK);
    m_run(500, true);
    CHECK_EQ(b.aud_saved, 0);
    CHECK_EQ(bts_aud_restore(&b), BTS_AUD_FACTORY);
    CHECK_EQ(bts_audio(&b, BTS_AU_RESTORE, 0), BTS_OK);
    m_run(600, true);
    CHECK_EQ(count_sent("SET CONTROL AUDIO INTERNAL I2S_SLAVE KEEPALIVE"), 1);
    CHECK_EQ(b.aud, BTS_AUD_KEEP);
    CHECK_EQ(b.aud_saved, 1);
    /* another form / no line: read, never rewritten */
    for (int form = 1; form <= 2; form++) {
        m_init(&b);
        M.aud_form = form;
        CHECK_EQ(bts_audio(&b, BTS_AU_READ, 0), BTS_OK);
        m_run(500, true);
        CHECK_EQ(b.aud_state, BTS_AUD_OTHER);
        CHECK_EQ(b.aud_rc, BTS_OK);
        CHECK_EQ(bts_audio(&b, BTS_AU_WRITE, 0), BTS_STATE);
        CHECK_EQ(bts_audio(&b, BTS_AU_RESTORE, 0), BTS_STATE);
        CHECK_EQ(ac_writes_sent(), 0);
    }
    /* listed forms that are not the plain one (flags out of the RM order, AAx, a duplicate, other routing) */
    {
        static const char *const other[] = {"SET CONTROL AUDIO INTERNAL I2S_SLAVE KEEPALIVE EVENT",
                                            "SET CONTROL AUDIO INTERNAL I2S_SLAVE KEEPALIVE AA3",
                                            "SET CONTROL AUDIO INTERNAL I2S_SLAVE EVENT EVENT",
                                            "SET CONTROL AUDIO INTERNAL I2S KEEPALIVE",
                                            "SET CONTROL AUDIO INTERNAL / I2S_SLAVE KEEPALIVE"};
        for (int k = 0; k < 5; k++) {
            m_init(&b);
            line(&b, "SET BT BDADDR 02:07:80:aa:bb:cc");
            line(&b, other[k]);
            line(&b, "SET");
            CHECK_EQ(b.aud_state, BTS_AUD_OTHER);
        }
    }
    /* the module ignoring the write: rc STATE from the read-back */
    m_init(&b);
    CHECK_EQ(bts_audio(&b, BTS_AU_READ, 0), BTS_OK);
    m_run(500, true);
    CHECK_EQ(bts_audio(&b, BTS_AU_WRITE, 0), BTS_OK);
    for (int i = 0; i < 80 && b.aud_op; i++) {
        m_run(8, true);
        M.aud = BTS_AUD_KEEP;                     /* the module keeps its line */
    }
    CHECK_EQ(b.aud_op, 0);
    CHECK_EQ(b.aud_rc, BTS_STATE);
    /* standby drops a pending op */
    CHECK_EQ(bts_audio(&b, BTS_AU_READ, 0), BTS_OK);
    m_run(100, false);
    CHECK_EQ(b.aud_op, 0);
    CHECK_EQ(b.aud_rc, BTS_FULL);
}

/* the EVENT lines (RM 6.67 `AUDIO ROUTE {link} {type} {channels}`, `A2DP CODEC {codec} {mode} {rate} …`) are no stream
 * events: replayed around the stream lines they change nothing but the codec; the line log keeps them; a whole
 * switch PC -> phone -> PC with the module's EVENT flag on sends exactly the commands it sends without */
static void test_btsrc_audio_event_lines(void)
{
    static const char *const ev[] = {"AUDIO ROUTE 0 A2DP LEFT RIGHT", "AUDIO ROUTE 4 A2DP LEFT RIGHT",
                                     "AUDIO ROUTE 1 SCO LEFT", "AUDIO ROUTE 0 TUNE LEFT RIGHT", "AUDIO ROUTE",
                                     "AUDIO ROUTE 99 A2DP", "A2DP CODEC APT-X JOINT_STEREO 44100",
                                     "A2DP CODEC SBC JOINT_STEREO 44100 BITPOOL 2-53"};
    static char without[48][BTS_OUT_MAX];
    bts_t b;
    char s[48];
    m_init(&b);
    m_connect_in(M_PC);
    m_connect_in(M_PHONE);
    m_run(1500, true);
    int pc = m_dev(M_PC), ph = m_dev(M_PHONE);
    CHECK(pc >= 0 && ph >= 0);
    int lists = b.n.lists, starts = b.n.starts, before = nsent;
    for (int k = 0; k < 8; k++)
        line(&b, ev[k]);                          /* no stream open: no stream appears */
    CHECK_EQ(bts_streams(&b), 0);
    CHECK_EQ(b.n.starts, starts);
    CHECK_EQ(b.route, 1);                         /* `AUDIO ROUTE 0 TUNE …`: link 0 kept, the bad ones skipped */
    CHECK_EQ(b.route_a2dp, 0);
    CHECK_EQ(b.dev[pc].codec, 0);                 /* a codec line with no stream is attached to nobody */
    CHECK_EQ(b.dev[ph].codec, 0);
    m_run(1000, true);
    CHECK_EQ(nsent, before);                      /* nothing asked (no LIST: nothing unknown) */
    CHECK_EQ(b.n.lists, lists);
    /* the phone (aptX) starts: its codec line and route come first */
    line(&b, "A2DP CODEC APT-X JOINT_STEREO 44100");
    snprintf(s, sizeof s, "AUDIO ROUTE %d A2DP LEFT RIGHT", M.d[M_PHONE].a2dp);
    line(&b, s);
    CHECK_EQ(bts_streams(&b), 0);
    M.d[M_PHONE].streaming = 1;
    snprintf(s, sizeof s, "A2DP STREAMING START %d", M.d[M_PHONE].a2dp);
    line(&b, s);
    CHECK_EQ(bts_streams(&b), 1);
    CHECK(b.dev[ph].streaming && !b.dev[pc].streaming);
    CHECK_EQ(b.dev[ph].codec, BTS_CODEC_APTX);
    CHECK_EQ(b.route, M.d[M_PHONE].a2dp + 1);
    CHECK_EQ(b.route_a2dp, 1);
    CHECK_EQ(b.link_dev[b.route - 1], ph);
    CHECK_EQ(b.active, ph);
    snprintf(s, sizeof s, "AUDIO ROUTE %d A2DP LEFT RIGHT\r\n", M.d[M_PC].a2dp);
    bts_rx(&b, (const uint8_t *)s, (uint32_t)strlen(s));   /* from the UART (logged): a route event naming the PC's
                                                             * link - still the phone's stream */
    s[strlen(s) - 2] = 0;
    CHECK(b.dev[ph].streaming && !b.dev[pc].streaming);
    CHECK_EQ(b.link_dev[b.route - 1], pc);        /* H1 is read here: the module routes the PC's link */
    CHECK_EQ(b.active, ph);
    /* the line log keeps the event lines as they came */
    {
        uint8_t out[2048];
        uint16_t first, next;
        uint32_t n = bts_log_read(&b, 0, out, sizeof out, &first, &next), hits = 0, k = (uint32_t)strlen(s);
        for (uint32_t p = 0; p + k <= n; p++)
            hits += memcmp(out + p, s, k) == 0;   /* the newest route line, whole */
        CHECK(hits >= 1);
        CHECK(n > k && memcmp(out + n - k, s, k) == 0);
    }
    /* a whole switch with and without the EVENT flag: the same commands */
    {
        int nwithout = 0;
        uint8_t a[6];
        for (int run = 0; run < 2; run++) {
            m_init(&b);
            M.aud = run ? BTS_AUD_EVENT : BTS_AUD_KEEP;
            m_connect_in(M_PC);
            m_run(500, true);
            m_play(M_PC);
            m_run(2000, true);
            m_connect_in(M_PHONE);
            m_run(2000, true);
            m_addr(M_PHONE, a);
            CHECK_EQ(bts_use(&b, a, true), BTS_OK);
            m_run(8000, true);
            CHECK(M.d[M_PHONE].streaming);
            m_addr(M_PC, a);
            CHECK_EQ(bts_use(&b, a, true), BTS_OK);
            m_run(8000, true);
            CHECK(M.d[M_PC].streaming);
            CHECK_EQ(b.dev[m_dev(M_PC)].codec, run ? BTS_CODEC_SBC : 0);
            if (!run) {
                nwithout = nsent;
                memcpy(without, sent, sizeof without);
            } else {
                CHECK_EQ(nsent, nwithout);
                for (int i = 0; i < nsent && i < nwithout; i++)
                    CHECK(!strcmp(sent[i], without[i]));
            }
            CHECK_EQ(ac_writes_sent(), 0);
        }
    }
}

/* the switch to a device that has no A2DP (the TV after it was closed): CALL, settle, PLAY; a CALL that fails ends
 * the switch "no link"; standby cancels a switch */
static void test_btsrc_switch_call(void)
{
    bts_t b;
    uint8_t tv[6];
    m_init(&b);
    m_connect_in(M_PHONE);
    m_play(M_PHONE);
    m_run(1000, true);
    m_addr(M_TV, tv);
    CHECK_EQ(bts_use(&b, tv, true), BTS_UNKNOWN);  /* never seen, not preferred */
    b.pol.has_pref = 1;
    memcpy(b.pol.pref, tv, 6);
    CHECK_EQ(bts_use(&b, tv, true), BTS_OK);
    uint32_t t0 = M.now;
    while (!M.d[M_TV].streaming && M.now - t0 < 10000)
        m_run(8, true);
    CHECK(M.d[M_TV].streaming && !M.d[M_PHONE].streaming);
    CHECK(M.now - t0 < 3000);
    CHECK_EQ(count_sent("CALL 12:84:04:04:14:24 19 A2DP"), 1);
    int tvd = m_dev(M_TV);
    CHECK(tvd >= 0 && b.dev[tvd].a2dp == 0x18);    /* CALL <l> + the second CONNECT: both A2DP links the TV's */
    m_run(3300, true);                             /* the TV: its AVRCP first (the sink opens it), up to 3 s */
    CHECK_EQ(b.sw_result, BTS_RES_PLAYING);
    /* a CALL nobody answers */
    m_init(&b);
    m_connect_in(M_PHONE);
    m_run(500, true);
    static const uint8_t GONE[6] = {0x11, 0x22, 0x33, 0x44, 0x55, 0x66};
    b.pol.has_pref = 1;
    memcpy(b.pol.pref, GONE, 6);
    CHECK_EQ(bts_use(&b, GONE, true), BTS_OK);
    m_run(9000, true);
    CHECK_EQ(b.sw_phase, BTS_SW_IDLE);
    CHECK_EQ(b.sw_result, BTS_RES_NO_LINK);
    /* standby cancels */
    uint8_t ph[6];
    m_addr(M_PHONE, ph);
    CHECK_EQ(bts_use(&b, ph, true), BTS_OK);
    m_run(16, false);
    CHECK_EQ(b.sw_result, BTS_RES_CANCELLED);
    /* a device without AVRCP whose source answers nothing at all: "ready" (press play on it once) - no sink START to a
     * source without AVRCP, its A2DP is never reopened (field 2026-11-03: a CLOSE broke the
     * Windows endpoint) */
    m_init(&b);
    b.pol.flags |= BTS_F_NO_AVRCP_CALL;
    line(&b, "RING 0 62:81:81:81:11:a1 19 A2DP");
    line(&b, "RING 1 62:81:81:81:11:a1 19 A2DP");
    CHECK_EQ(bts_use(&b, PHONE, true), BTS_OK);
    m_run(1500, true);
    CHECK_EQ(b.sw_phase, BTS_SW_IDLE);
    CHECK_EQ(count_sent("A2DP STREAMING START"), 0);
    CHECK_EQ(count_sent("CLOSE"), 0);
    CHECK_EQ(b.sw_result, BTS_RES_READY);
    CHECK_EQ(b.sw_tried, 0);
    CHECK_EQ(calls_of(" 17 AVRCP"), 0);           /* the opt-out flag */
    /* act 6 (MUTE): the A2DP signalling link only */
    nsent = 0;
    CHECK_EQ(bts_action(&b, BTS_ACT_MUTE, PHONE), BTS_OK);
    CHECK_EQ(bts_action(&b, 7, PHONE), BTS_BAD);
    m_run(300, true);
    CHECK_EQ(nsent, 1);
    CHECK(!strcmp(sent[0], "CLOSE 0"));
}

/* ---- field 2026-11-03 (a12c870a): the switch, no CLOSE ever ------------------------------
 * The model as the field showed it: Windows + Chrome (toggle PLAY / PAUSE, keeps a paused stream open, honours the
 * sink's STOP / START, its fresh / resumed stream silent while paused, a CLOSE of its A2DP breaks Chrome's audio for
 * good); Android (AVRCP PLAY / PAUSE discrete, its stream stops ~3 s after a pause, a CLOSE of its A2DP drops its
 * AVRCP).  D27: the PC (YouTube) plays and is selected, the phone connected and paused -> 「이 기기로 듣기」
 * on the phone.  D25: right after, back to the PC.  What the user would hear, what the piano sent.  (The strategies
 * WAIT / HOLD / DIRECT / SELECT were removed - one way to switch.) */
typedef struct {
    int phone_audible, pc_audible_back, pc_paused_after_d27, pc_playing_back, d27_result, d25_result;
    uint32_t d27_ms, d25_ms;
} sw_run_t;

static int sw_susp_paused;   /* 1: a source reporting paused is suspended too (the behaviour up to e5bb2372) */

static sw_run_t sw_run(bts_t *b, int direct_switch, int silent_after_stop)
{
    sw_run_t r;
    uint8_t pc[6], phone[6];
    memset(&r, 0, sizeof r);
    m_init(b);
    b->susp_paused = (uint8_t)sw_susp_paused;
    M.direct_switch = direct_switch;
    M.d[M_PC].toggle = 1;
    M.d[M_PC].silent_when_paused = 1;
    M.d[M_PC].breaks_on_close = 1;
    M.d[M_PC].silent_after_stop = silent_after_stop;
    M.d[M_PHONE].pause_stop_ms = 3000;            /* field 2026-11-03: Android closes its stream ~3 s after a pause */
    M.d[M_PHONE].avrcp_drop_on_close = 1;
    m_addr(M_PC, pc);
    m_addr(M_PHONE, phone);
    m_connect_in(M_PC);
    m_play(M_PC);
    m_run(1500, true);
    m_connect_in(M_PHONE);
    m_run(2500, true);
    CHECK(m_selected(M_PC) && m_audible(M_PC));
    /* D27: PC -> phone */
    CHECK_EQ(bts_use(b, phone, true), BTS_OK);
    uint32_t t0 = M.now;
    while (b->sw_phase != BTS_SW_IDLE && M.now - t0 < 12000)
        m_run(8, true);
    m_run(1500, true);
    r.d27_result = b->sw_result;
    r.d27_ms = b->sw_last_ms;
    r.phone_audible = m_audible(M_PHONE);
    r.pc_paused_after_d27 = M.d[M_PC].paused;
    /* D25: phone -> PC */
    CHECK_EQ(bts_use(b, pc, true), BTS_OK);
    t0 = M.now;
    while (b->sw_phase != BTS_SW_IDLE && M.now - t0 < 12000)
        m_run(8, true);
    m_run(1500, true);
    r.d25_result = b->sw_result;
    r.d25_ms = b->sw_last_ms;
    r.pc_audible_back = m_audible(M_PC);
    r.pc_playing_back = !M.d[M_PC].paused;
    /* no CLOSE, every link kept (the phone stays connected, the Windows endpoint stays) */
    CHECK_EQ(count_sent("CLOSE"), 0);
    CHECK_EQ(M.closes, 0);
    CHECK(!M.d[M_PC].broken);
    CHECK(M.d[M_PC].a2dp >= 0 && M.d[M_PC].avrcp >= 0 && M.d[M_PHONE].a2dp >= 0 && M.d[M_PHONE].avrcp >= 0);
    CHECK_EQ(calls_of(" 19 A2DP"), 0);
    CHECK(only_runtime_sent());
    CHECK_EQ(ac_writes_sent(), 0);
    if (getenv("BTS_TIMES"))
        printf("    switch%s: D27 result %d %u ms phone %s, PC %s | D25 result %d %u ms PC %s\n",
               direct_switch ? " (module switches)" : "", r.d27_result, (unsigned)r.d27_ms,
               r.phone_audible ? "heard" : "silent", r.pc_paused_after_d27 ? "paused" : "playing", r.d25_result,
               (unsigned)r.d25_ms, r.pc_audible_back ? "heard" : "silent");
    return r;
}

static void test_btsrc_switch_field_1103(void)
{
    bts_t b;
    sw_run_t r;
    /* as up to e5bb2372 (a source reporting paused suspended too): PC PAUSE (it reports PLAYING - a toggle: paused),
     * 0.7 s, suspended; the phone (paused by the user, reports PAUSED) PLAY.  Back: the phone PAUSE, suspended at 0.7 s;
     * the PC resumed by the sink, PLAY = play */
    sw_susp_paused = 1;
    r = sw_run(&b, 0, 0);
    CHECK(r.phone_audible && r.pc_paused_after_d27 && r.d27_result == BTS_RES_PLAYING);
    CHECK(r.pc_audible_back && r.pc_playing_back && r.d25_result == BTS_RES_PLAYING);
    CHECK(r.d27_ms < 2500 && r.d25_ms < 2500);
    CHECK_EQ(count_sent("A2DP STREAMING STOP"), 2);
    CHECK_EQ(M.plays_to[M_PC], 1);
    /* the risk the field had to rule out: if Windows were silent after a sink STOP (the earlier reading), the PC
     * would come back silent - the module shows nothing (= field 2026-11-09) */
    r = sw_run(&b, 0, 1);
    CHECK(r.phone_audible && !r.pc_audible_back && r.d25_result == BTS_RES_PLAYING);
    sw_susp_paused = 0;
    /* field 2026-11-09 evening: the paused PC's open stream is suspended again before the phone starts
     * (7a206fac-8f98d335 left it open - the module kept it, the phone silent - the field firmware's way is no longer in
     * the code).  The phone heard either module.  Back to the PC: PLAY first, its own start (no sink resume) - silent
     * only if Windows mutes every stream after a sink STOP (silent_after_stop, the model's pessimistic reading of the 11-09 field test);
     * heard if only the sink's resume was silent (the field decides) */
    for (int sw = 0; sw < 2; sw++) {
        r = sw_run(&b, sw, 1);
        CHECK(r.phone_audible && r.d27_result == BTS_RES_PLAYING && r.d27_ms < 2500);
        CHECK(!r.pc_audible_back && r.d25_result == BTS_RES_PLAYING);   /* the risk the 11-09 field test left */
        CHECK_EQ(count_sent("A2DP STREAMING STOP"), 2);   /* the PC at D27, the paused phone (still streaming) at D25 */
        r = sw_run(&b, sw, 0);
        CHECK(r.phone_audible && r.d27_result == BTS_RES_PLAYING && r.pc_audible_back && r.d25_result == BTS_RES_PLAYING);
    }
}

/* ---- 2026-11-09 ------------------------------------------------------------------------- */
static const uint8_t F9_PC[6] = {0xa2, 0x83, 0x83, 0x83, 0x13, 0x23};
static const uint8_t F9_PHONE[6] = {0x62, 0x81, 0x81, 0x81, 0x11, 0xa1};

static void rx(bts_t *b, const char *s) { bts_rx(b, (const uint8_t *)s, (uint32_t)strlen(s)); }

static int count_exact(const char *c)
{
    int n = 0;
    for (int i = 0; i < nsent; i++)
        n += !strcmp(sent[i], c);
    return n;
}

/* the piano's own reconnect while AUTOCALL is off (user decision 2026-11-09): preferred, else last picked (default) /
 * preferred only / off; the kept part of the policy (recon + preferred) encodes and applies back, bad records change
 * nothing */
static void test_btsrc_reconnect_choice(void)
{
    bts_t b;
    bts_policy_t p;
    bts_policy_default(&p);
    CHECK_EQ(p.recon, BTS_RC_PREF_LAST);
    p.recon = BTS_RC_MAX + 1;
    CHECK_EQ(bts_policy_check(&p), -1);
    for (int c = 0; c < 6; c++) {
        /* c: 0 default + pref, 1 default no pref, 2 pref only + pref, 3 pref only no pref, 4 off + pref, 5 off no pref */
        uint8_t recon = c < 2 ? BTS_RC_PREF_LAST : c < 4 ? BTS_RC_PREF : BTS_RC_OFF;
        bool pref = !(c & 1);
        fresh(&b);
        b.pol.recon = recon;
        b.ac_state = BTS_AC_OFF;
        b.has_sel = 1;
        memcpy(b.sel_addr, F9_PHONE, 6);
        b.pol.has_pref = pref;
        if (pref)
            memcpy(b.pol.pref, F9_PC, 6);
        run(&b, 5000, true);
        int pc = count_sent("CALL a2:83:83:83:13:23 19 A2DP"), ph = count_sent("CALL 62:81:81:81:11:a1 19 A2DP");
        CHECK_EQ(pc, recon != BTS_RC_OFF && pref ? 1 : 0);
        CHECK_EQ(ph, recon == BTS_RC_PREF_LAST && !pref ? 1 : 0);
        CHECK(only_runtime_sent());
    }
    /* the NOR record */
    uint8_t raw[BTS_SAVED_RAW];
    bts_policy_default(&p);
    p.recon = BTS_RC_PREF;
    p.has_pref = 1;
    memcpy(p.pref, F9_PC, 6);
    bts_saved_encode(&p, raw);
    CHECK(raw[0] == 1 && raw[1] == BTS_RC_PREF && raw[2] == 1 && !memcmp(raw + 3, F9_PC, 6) && raw[9] == 0);
    bts_policy_t q;
    bts_policy_default(&q);
    CHECK(bts_saved_apply(&q, raw, sizeof raw));
    CHECK(q.recon == BTS_RC_PREF && q.has_pref == 1 && !memcmp(q.pref, F9_PC, 6));
    raw[1] = 3;
    bts_policy_default(&q);
    CHECK(!bts_saved_apply(&q, raw, sizeof raw) && q.recon == BTS_RC_PREF_LAST && !q.has_pref);
    raw[1] = 0;
    raw[0] = 2;
    CHECK(!bts_saved_apply(&q, raw, sizeof raw));
    raw[0] = 1;
    CHECK(!bts_saved_apply(&q, raw, 8));
}


/* field 2026-11-09 (e5bb2372): the PC (a2:..:13:23, A2DP 0, AVRCP 1) plays and is selected, the phone
 * (A2DP 4, AVRCP 6) paused.  The PC pauses -> registered again (`SET 1 SELECT`) - the module prints its echo into the
 * phone's CHANGED: `SAVRCP 6 … PLAYING` / `ET 1 SELECT`.  The phone's PLAYING is read (the field firmware missed it,
 * took the phone for paused and skipped the PAUSE) -> STOP 0, START 4 -> the guard PAUSEs the phone. */
static void test_btsrc_interleaved_field(void)
{
    bts_t b;
    fresh(&b);
    b.ac_state = BTS_AC_ON;
    line(&b, "RING 0 a2:83:83:83:13:23 19 A2DP");
    line(&b, "RING 1 a2:83:83:83:13:23 17 AVRCP");
    line(&b, "A2DP STREAMING START 0");
    line(&b, "RING 4 62:81:81:81:11:a1 19 A2DP");
    line(&b, "RING 6 62:81:81:81:11:a1 17 AVRCP");
    run(&b, 900, true);
    int ph = dev_of(&b, F9_PHONE), pc = dev_of(&b, F9_PC);
    CHECK(ph >= 0 && pc >= 0 && bts_sel_dev(&b) == pc);
    line(&b, "AVRCP 1 REGISTER_NOTIFICATION_RSP INTERIM PLAYBACK_STATUS_CHANGED PLAYING");
    line(&b, "AVRCP 6 REGISTER_NOTIFICATION_RSP INTERIM PLAYBACK_STATUS_CHANGED PAUSED");
    line(&b, "AVRCP 1 REGISTER_NOTIFICATION_RSP CHANGED PLAYBACK_STATUS_CHANGED PAUSED");
    run(&b, 400, true);
    CHECK(sent_index("SET 1 SELECT", 0) >= 0);
    rx(&b, "S");                                  /* the echo's first byte, then the phone's line, then the rest */
    rx(&b, "AVRCP 6 REGISTER_NOTIFICATION_RSP CHANGED PLAYBACK_STATUS_CHANGED PLAYING\r\n");
    rx(&b, "ET 1 SELECT\r\n");
    CHECK_EQ(bts_dev_ps(&b, ph) & 7, BTS_PS_PLAYING);
    CHECK_EQ(b.n.recovered, 1);
    line(&b, "AVRCP 1 REGISTER_NOTIFICATION_RSP INTERIM PLAYBACK_STATUS_CHANGED PAUSED");
    int before = nsent;
    line(&b, "A2DP STREAMING STOP 0");
    line(&b, "A2DP STREAMING START 4");
    run(&b, 1000, true);
    int pz = sent_index("AVRCP PAUSE", before);
    CHECK(pz > 0 && !strcmp(sent[pz - 1], "SET 6 SELECT"));   /* the phone paused (field: `no PAUSE: it is paused`) */
    CHECK_EQ(b.n.pause_skips, 0);
    /* field 11-09: `SETA2DP STREAMING STOP 4` / ` 0 SELECT` - the phone's STOP read */
    CHECK(b.dev[ph].streaming);
    rx(&b, "SETA2DP STREAMING STOP 4\r\n 0 SELECT\r\n");
    CHECK(!b.dev[ph].streaming);
    CHECK_EQ(b.n.recovered, 2);
    /* other lines after a garbage prefix: NO CARRIER / RING / START */
    rx(&b, "ETNO CARRIER 6 ERROR 0\r\n");
    CHECK_EQ(bts_dev_state2(&b, ph) & BTS_D2_AVRCP, 0);
    rx(&b, "AVRCP PDU 3RING 7 62:81:81:81:11:a1 17 AVRCP\r\n");
    CHECK_EQ(b.link_dev[7], ph);
    rx(&b, "SET 4 SA2DP STREAMING START 4\r\n");
    CHECK(b.dev[ph].streaming);
    CHECK_EQ(b.n.recovered, 5);
    CHECK(only_runtime_sent());
}

/* a status line that cannot be read asks that link again (AVRCP PDU 31 1) - its status unknown meanwhile; a fragment
 * with no link: every registered device; at most BTS_PS_FIX_MAX times until a clean line; echoes never */
static void test_btsrc_garbled_status(void)
{
    bts_t b;
    fresh(&b);
    line(&b, "RING 0 a2:83:83:83:13:23 19 A2DP");
    line(&b, "RING 1 a2:83:83:83:13:23 17 AVRCP");
    line(&b, "RING 4 62:81:81:81:11:a1 19 A2DP");
    line(&b, "RING 6 62:81:81:81:11:a1 17 AVRCP");
    run(&b, 900, true);
    int ph = dev_of(&b, F9_PHONE);
    line(&b, "AVRCP 1 REGISTER_NOTIFICATION_RSP INTERIM PLAYBACK_STATUS_CHANGED PLAYING");
    line(&b, "AVRCP 6 REGISTER_NOTIFICATION_RSP INTERIM PLAYBACK_STATUS_CHANGED PAUSED");
    run(&b, 500, true);
    int regs = count_exact("AVRCP PDU 31 1");
    CHECK_EQ(regs, 2);
    /* echoes and our own commands' fragments: nothing */
    line(&b, "AVRCP PDU 31 1");
    line(&b, "ET 6 SELECT");
    line(&b, "AVRCP PAUSE");
    line(&b, "SET 6 SELECT");
    line(&b, "AVRCP 6 RSP PDU_ID 31, data: 01 01");
    run(&b, 500, true);
    CHECK_EQ(count_exact("AVRCP PDU 31 1"), regs);
    CHECK_EQ(b.n.garbled, 0);
    /* a run-on word: unknown now, asked again on link 6 */
    line(&b, "AVRCP 6 REGISTER_NOTIFICATION_RSP CHANGED PLAYBACK_STATUS_CHANGED PLAYINGET 1 SELECT");
    CHECK_EQ(bts_dev_ps(&b, ph) & 7, BTS_PS_UNKNOWN);
    run(&b, 500, true);
    CHECK_EQ(count_exact("AVRCP PDU 31 1"), regs + 1);
    CHECK(!strcmp(sent[nsent - 2], "SET 6 SELECT"));
    line(&b, "AVRCP 6 REGISTER_NOTIFICATION_RSP INTERIM PLAYBACK_STATUS_CHANGED PLAYING");
    CHECK_EQ(bts_dev_ps(&b, ph), BTS_PS_PLAYING | BTS_PSB_LIVE);
    /* a cut word (`PLA`) and a known word with bytes after it (read, asked again) */
    line(&b, "AVRCP 6 REGISTER_NOTIFICATION_RSP CHANGED PLAYBACK_STATUS_CHANGED PAU");
    run(&b, 500, true);
    CHECK_EQ(count_exact("AVRCP PDU 31 1"), regs + 2);
    line(&b, "AVRCP 6 REGISTER_NOTIFICATION_RSP INTERIM PLAYBACK_STATUS_CHANGED PAUSED ET 6 SELECT");
    CHECK_EQ(bts_dev_ps(&b, ph) & 7, BTS_PS_PAUSED);
    run(&b, 500, true);
    CHECK_EQ(count_exact("AVRCP PDU 31 1"), regs + 3);
    line(&b, "AVRCP 6 REGISTER_NOTIFICATION_RSP INTERIM PLAYBACK_STATUS_CHANGED PAUSED");
    /* a line cut in two: link 6 from the first part; the second part (no link): every registered device */
    line(&b, "AVRCP 6 REGISTER_NOTIF");
    run(&b, 500, true);
    CHECK_EQ(count_exact("AVRCP PDU 31 1"), regs + 4);
    line(&b, "AVRCP 6 REGISTER_NOTIFICATION_RSP INTERIM PLAYBACK_STATUS_CHANGED PAUSED");
    line(&b, "ICATION_RSP CHANGED PLAYBACK_STATUS_CHANGED PLAYING");
    run(&b, 1200, true);
    CHECK_EQ(count_exact("AVRCP PDU 31 1"), regs + 6);   /* the PC and the phone */
    CHECK(bts_dev_ps(&b, ph) & 7);                 /* a fragment with no link leaves the status as it was */
    line(&b, "AVRCP 1 REGISTER_NOTIFICATION_RSP INTERIM PLAYBACK_STATUS_CHANGED PLAYING");
    line(&b, "AVRCP 6 REGISTER_NOTIFICATION_RSP INTERIM PLAYBACK_STATUS_CHANGED PAUSED");
    /* garbage again and again: at most BTS_PS_FIX_MAX re-registrations, then a clean line resets the count */
    int n0 = count_exact("AVRCP PDU 31 1");
    for (int i = 0; i < 6; i++) {
        line(&b, "AVRCP 6 REGISTER_NOTIFICATION_RSP CHANGED PLAYBACK_STATUS_CHANGED PAUSEDET");
        line(&b, "AVRCP 6 REGISTER_NOTIFICATION_RSP CHANGED PLAYBACK_STATUS_CHANGED PL");
        run(&b, 500, true);
    }
    CHECK(count_exact("AVRCP PDU 31 1") > n0 && count_exact("AVRCP PDU 31 1") <= n0 + (int)BTS_PS_FIX_MAX);
    CHECK_EQ(b.dev[ph].ps_fix, BTS_PS_FIX_MAX);
    n0 = count_exact("AVRCP PDU 31 1");
    line(&b, "AVRCP 6 REGISTER_NOTIFICATION_RSP CHANGED PLAYBACK_STATUS_CHANGED PL");
    run(&b, 500, true);
    CHECK_EQ(count_exact("AVRCP PDU 31 1"), n0);   /* the limit holds */
    line(&b, "AVRCP 6 REGISTER_NOTIFICATION_RSP INTERIM PLAYBACK_STATUS_CHANGED PAUSED");
    CHECK_EQ(b.dev[ph].ps_fix, 0);
    /* a word the target chose (not a cut one): unknown as before, not asked again */
    n0 = count_exact("AVRCP PDU 31 1");
    line(&b, "AVRCP 6 REGISTER_NOTIFICATION_RSP INTERIM PLAYBACK_STATUS_CHANGED BOGUS");
    run(&b, 500, true);
    CHECK_EQ(count_exact("AVRCP PDU 31 1"), n0);
    CHECK(only_runtime_sent());
}

/* field 2026-11-09: a source that reports paused and keeps its stream (Windows) is not suspended by the guard
 * (the switch suspends it since 11-09 evening - test_btsrc_field_1109_evening_*); one that does not report paused is */
static void test_btsrc_paused_source_kept(void)
{
    bts_t b;
    for (int old = 0; old < 2; old++) {
        fresh(&b);
        b.susp_paused = (uint8_t)old;
        b.has_sel = 1;
        memcpy(b.sel_addr, F9_PHONE, 6);
        line(&b, "RING 4 62:81:81:81:11:a1 19 A2DP");
        line(&b, "RING 6 62:81:81:81:11:a1 17 AVRCP");
        line(&b, "RING 0 a2:83:83:83:13:23 19 A2DP");
        line(&b, "RING 1 a2:83:83:83:13:23 17 AVRCP");
        run(&b, 900, true);
        line(&b, "AVRCP 1 REGISTER_NOTIFICATION_RSP INTERIM PLAYBACK_STATUS_CHANGED PLAYING");
        line(&b, "AVRCP 6 REGISTER_NOTIFICATION_RSP INTERIM PLAYBACK_STATUS_CHANGED PAUSED");
        int before = nsent;
        line(&b, "A2DP STREAMING START 0");          /* the PC starts while the phone is selected: refused */
        run(&b, 450, true);
        CHECK(sent_index("AVRCP PAUSE", before) > 0);
        line(&b, "AVRCP 1 REGISTER_NOTIFICATION_RSP CHANGED PLAYBACK_STATUS_CHANGED PAUSED");   /* stream kept open */
        run(&b, 2000, true);
        CHECK_EQ(count_exact("A2DP STREAMING STOP"), old ? 1 : 0);
    }
    /* no AVRCP control link: never suspended (field 2026-11-10 - Windows stays silent on a stream the sink
     * stopped until it reconnects); logged once */
    fresh(&b);
    b.has_sel = 1;
    memcpy(b.sel_addr, F9_PHONE, 6);
    line(&b, "RING 4 62:81:81:81:11:a1 19 A2DP");
    line(&b, "RING 0 a2:83:83:83:13:23 19 A2DP");
    line(&b, "A2DP STREAMING START 0");
    run(&b, 8000, true);
    CHECK_EQ(count_exact("A2DP STREAMING STOP"), 0);
    CHECK_EQ(b.n.held, 1);
}

/* field 2026-11-09: a PC that opens the AVRCP browsing channel (1b) with no control channel - the module rebooted each
 * time: `CLOSE <that link>` goes out first, ahead of the queue; not with the opt-out flag 0x20 / in mode NEWEST */
static void test_btsrc_lone_browsing_closed(void)
{
    bts_t b;
    for (int optout = 0; optout < 2; optout++) {
        fresh(&b);
        if (optout)
            b.pol.flags |= BTS_F_NO_AVRCP_CALL;
        line(&b, "RING 0 a2:83:83:83:13:23 19 A2DP");
        line(&b, "RING 2 a2:83:83:83:13:23 19 A2DP");
        run(&b, 400, true);
        int before = nsent;
        line(&b, "RING 3 a2:83:83:83:13:23 1b AVRCP");
        run(&b, 200, true);
        if (!optout) {
            CHECK(nsent > before && !strcmp(sent[before], "CLOSE 3"));
            CHECK_EQ(b.n.orphan_closes, 1);
        } else {
            CHECK_EQ(count_sent("CLOSE"), 0);
        }
        /* with the control channel up first, the browsing channel is fine: nothing */
        line(&b, "NO CARRIER 3 ERROR 0");
        line(&b, "RING 5 a2:83:83:83:13:23 17 AVRCP");
        int c0 = count_sent("CLOSE");
        line(&b, "RING 6 a2:83:83:83:13:23 1b AVRCP");
        run(&b, 400, true);
        CHECK_EQ(count_sent("CLOSE"), c0);
        CHECK(only_runtime_sent());
    }
}

static uint32_t log_cap = 400;   /* the cave's reply holds 128 B of records */

static int log_records(const bts_t *b, uint16_t from, char text[][BTS_LINE_MAX + 1], uint8_t *flags, int max,
                       uint16_t *next)
{
    uint8_t out[400];
    uint16_t first;
    int k = 0;
    uint32_t n = bts_log_read(b, from, out, log_cap, &first, next), o = 0;
    while (o + 8 <= n && k < max) {
        uint32_t len = out[o + 7];
        flags[k] = out[o + 6];
        memcpy(text[k], out + o + 8, len);
        text[k][len] = 0;
        k++;
        o += 8 + len;
    }
    return k;
}

/* the line log keeps every module line and every command (TX), in order, read by sequence number; a full ring
 * drops the oldest; one read returns whole records that fit the reply */
static void test_btsrc_line_log(void)
{
    bts_t b;
    char text[16][BTS_LINE_MAX + 1];
    uint8_t flags[16];
    uint16_t next, first;
    static const uint8_t RX[] = "A2DP CODEC SBC JOINT_STEREO 44100 BITPOOL 2-53\r\nAVRCP PLAY PRESS\r\n\r\nx\r\n";
    fresh(&b);   /* "module on" event */
    b.pol.flags |= BTS_F_NO_PS;   /* no play-status registration in this log (test_btsrc_play_status has it) */
    run(&b, 1230, true);
    bts_rx(&b, RX, sizeof RX - 1);
    line(&b, "RING 0 62:81:81:81:11:a1 19 A2DP");   /* bts_line itself does not log: only the byte assembler */
    CHECK_EQ(bts_use(&b, PC, true), BTS_UNKNOWN);
    line(&b, "RING 1 62:81:81:81:11:a1 17 AVRCP");
    CHECK_EQ(bts_avrcp(&b, PHONE, false), BTS_OK);
    run(&b, 400, true);
    int k = log_records(&b, 0, text, flags, 16, &next);
    CHECK_EQ(k, 6);
    CHECK(!strcmp(text[0], "module on") && flags[0] == BTS_L_EVENT);
    CHECK(!strcmp(text[1], "A2DP CODEC SBC JOINT_STEREO 44100 BITPOOL 2-53") && flags[1] == 0);
    CHECK(!strcmp(text[2], "AVRCP PLAY PRESS"));
    CHECK(!strcmp(text[3], "x"));                     /* one-character lines are kept too */
    CHECK(!strcmp(text[4], "SET 1 SELECT") && flags[4] == BTS_L_TX);
    CHECK(!strcmp(text[5], "AVRCP PAUSE") && flags[5] == BTS_L_TX);
    CHECK_EQ(next, 6);
    k = log_records(&b, 4, text, flags, 16, &next);  /* from a sequence number */
    CHECK_EQ(k, 2);
    CHECK(!strcmp(text[0], "SET 1 SELECT"));
    CHECK_EQ(log_records(&b, 6, text, flags, 16, &next), 0);
    /* the time stamp: 10 ms units, 4 x 7 bits */
    uint8_t out[140];
    bts_log_read(&b, 1, out, sizeof out, &first, &next);
    uint32_t t = (uint32_t)out[2] << 21 | (uint32_t)out[3] << 14 | (uint32_t)out[4] << 7 | out[5];
    CHECK_EQ(t, 123);
    /* overflow: 200 long lines - the oldest are dropped, the newest kept, sequence numbers keep counting */
    char ln[BTS_LINE_MAX + 20];
    for (int i = 0; i < 200; i++) {
        int m = snprintf(ln, sizeof ln, "LIST %d CONNECTED A2DP 1024 0 0 24 0 0 12:84:04:04:14:24 19 OUTGOING ACTIVE "
                                        "SLAVE ENCRYPTED 0 %03d\r\n", i % 16, i);
        bts_rx(&b, (const uint8_t *)ln, (uint32_t)m);
    }
    bts_log_read(&b, 0, out, sizeof out, &first, &next);
    CHECK_EQ(next, 206);
    CHECK(first > 6 && b.log.used <= BTS_LOG_CAP);
    k = log_records(&b, (uint16_t)(next - 1), text, flags, 16, &next);
    CHECK_EQ(k, 1);
    CHECK(strstr(text[0], "ENCRYPTED 0 199") != NULL);
    log_cap = 128;
    k = log_records(&b, 0, text, flags, 16, &next);  /* older than kept: from the oldest, whole records only */
    CHECK_EQ(k, 1);
    /* a line longer than the assembler takes: cut, flagged */
    for (int i = 0; i < 130; i++)
        bts_rx(&b, (const uint8_t *)"y", 1);
    bts_rx(&b, (const uint8_t *)"\n", 1);
    k = log_records(&b, next, text, flags, 16, &next);
    CHECK_EQ(k, 1);
    CHECK_EQ(strlen(text[0]), BTS_LINE_MAX);
    CHECK_EQ(flags[0], BTS_L_TRUNC);
    log_cap = 400;
}

/* until the manager has device i's A2DP link (its CONNECT came), at most `limit` ms */
static void m_until_a2dp(int i, uint32_t limit)
{
    uint32_t t0 = M.now;
    while ((m_dev(i) < 0 || !M.b->dev[m_dev(i)].a2dp) && M.now - t0 < limit)
        m_run(8, true);
}

/* the manager's reconnect CALLs (b->n.reconnects) over ms: their model times into at[] (at most max) */
static int m_calls_during(bts_t *b, uint32_t ms, uint32_t *at, int max)
{
    int n = 0;
    uint16_t before = b->n.reconnects;
    for (uint32_t t = 0; t < ms; t += 8) {
        uint16_t r = b->n.reconnects;
        m_run(8, true);
        if (b->n.reconnects != r && n < max)
            at[n++] = M.now;
    }
    (void)before;
    return n;
}


/* field 2026-11-09: a Windows PC connects - `RING 0 <PC> 19 A2DP` · 0.3 s later
 * `RING 1 … 19 A2DP`.  Up to ota_main a6fdc5a2 the sink called `CALL <PC> 17 AVRCP` right then (0.3 s, the suspect; 1.5 s
 * otherwise) -> `CALL 2`, no CONNECT for 11 s -> `RING 3 <PC> 1b AVRCP` -> CLOSE 3 -> ERROR_NO_CONTROL_CHANNEL -> reboot.
 * Left alone (flag 0x20) the PC opened `17` itself 0.5 s after its A2DP.  Now: (a) the PC's own 17 at 0.5 s - the sink
 * never calls; (b) a source that opens none - the sink's CALL only 4 s after its A2DP, unanswered -> `CLOSE 2` 1.5 s
 * after `CALL 2`, so nothing of ours is pending when the 1b comes 11 s after the CALL; the A2DP links stay; the module
 * then reboots (as in the field) -> the PC is the suspect: no CALL to it any more; (c) a phone answers within 0.5 s:
 * nothing closed, its play status registered */
static void test_btsrc_field_1109_pc_avrcp(void)
{
    bts_t b;
    int d, before;
    /* (a) */
    fresh(&b);
    run(&b, 6000, true);                           /* past the boot grace */
    line(&b, "RING 0 a2:83:83:83:13:23 19 A2DP");
    run(&b, 504, true);
    line(&b, "RING 1 a2:83:83:83:13:23 17 AVRCP");
    run(&b, 800, true);
    CHECK(sent_index("AVRCP PDU 31 1", 0) >= 1 && !strcmp(sent[sent_index("AVRCP PDU 31 1", 0) - 1], "SET 1 SELECT"));
    line(&b, "AVRCP 1 REGISTER_NOTIFICATION_RSP INTERIM PLAYBACK_STATUS_CHANGED STOPPED");
    run(&b, 20000, true);
    CHECK_EQ(calls_of(" 17 AVRCP"), 0);
    CHECK_EQ(count_sent("CLOSE"), 0);
    d = dev_of(&b, (const uint8_t[6]){0xa2, 0x83, 0x83, 0x83, 0x13, 0x23});
    CHECK(d >= 0 && (bts_dev_ps(&b, d) & 7) == BTS_PS_STOPPED);
    CHECK(only_runtime_sent());
    /* (b) */
    fresh(&b);
    run(&b, 6000, true);
    line(&b, "RING 0 a2:83:83:83:13:23 19 A2DP");
    run(&b, 296, true);
    line(&b, "RING 1 a2:83:83:83:13:23 19 A2DP");
    run(&b, 3400, true);                           /* 3.7 s after the A2DP */
    CHECK_EQ(calls_of(" 17 AVRCP"), 0);            /* up to a6fdc5a2: at 0.3 s (suspect) / 1.5 s */
    run(&b, 400, true);
    CHECK_EQ(count_sent("CALL a2:83:83:83:13:23 17 AVRCP"), 1);
    line(&b, "CALL 2");
    CHECK_EQ(b.av_lid, 2);
    CHECK(b.call_on && b.call_avrcp);
    run(&b, 1400, true);
    CHECK_EQ(count_sent("CLOSE"), 0);
    CHECK(b.call_on);
    run(&b, 200, true);
    CHECK_EQ(count_sent("CLOSE 2"), 1);            /* our unanswered CALL: closed by its link id */
    CHECK(!b.call_on);
    CHECK_EQ(b.av_lid, -1);
    CHECK_EQ(b.n.avrcp_giveups, 1);
    d = dev_of(&b, (const uint8_t[6]){0xa2, 0x83, 0x83, 0x83, 0x13, 0x23});
    CHECK(d >= 0 && b.dev[d].links == 0x03 && b.dev[d].a2dp == 0x03);   /* only its A2DP links */
    line(&b, "NO CARRIER 2 ERROR 0");
    run(&b, 9400, true);                           /* 11 s after the CALL */
    before = nsent;
    line(&b, "RING 3 a2:83:83:83:13:23 1b AVRCP");
    CHECK(!b.call_on && b.av_lid < 0);             /* nothing of ours pending when the browsing channel comes */
    for (int l = 0; l < BTS_LINKS; l++)
        CHECK(!(b.link_dev[l] == d && b.link_prof[l] == BTS_P_NONE));
    run(&b, 200, true);
    CHECK(nsent > before && !strcmp(sent[before], "CLOSE 3"));   /* the lone 1b: closed as before */
    CHECK_EQ(calls_of(" 17 AVRCP"), 1);            /* once per A2DP episode */
    CHECK_EQ(count_sent("CLOSE 0") + count_sent("CLOSE 1"), 0);
    line(&b, "AVRCP 5103 ERROR_NO_CONTROL_CHANNEL");
    line(&b, "WRAP THOR AI (6.2.0 build 1135)");
    line(&b, "READY.");
    CHECK_EQ(b.n.reboots, 1);
    CHECK_EQ(bts_health(&b), BTS_MH_SUSPECT);
    line(&b, "RING 0 a2:83:83:83:13:23 19 A2DP");
    line(&b, "RING 1 a2:83:83:83:13:23 19 A2DP");
    run(&b, 30000, true);
    CHECK_EQ(calls_of(" 17 AVRCP"), 1);            /* the suspect: never again from the sink */
    CHECK(only_runtime_sent());
    /* (c) */
    fresh(&b);
    line(&b, "RING 0 62:81:81:81:11:a1 19 A2DP");
    line(&b, "RING 1 62:81:81:81:11:a1 19 A2DP");
    run(&b, 4100, true);
    CHECK_EQ(count_sent("CALL 62:81:81:81:11:a1 17 AVRCP"), 1);
    line(&b, "CALL 2");
    run(&b, 400, true);
    line(&b, "CONNECT 2 AVRCP 17");
    run(&b, 3000, true);
    CHECK_EQ(count_sent("CLOSE"), 0);
    CHECK_EQ(b.n.avrcp_giveups, 0);
    CHECK_EQ(b.av_lid, -1);
    CHECK(sent_index("AVRCP PDU 31 1", 0) >= 1 && !strcmp(sent[sent_index("AVRCP PDU 31 1", 0) - 1], "SET 2 SELECT"));
    d = dev_of(&b, PHONE);
    CHECK(d >= 0 && (bts_dev_state2(&b, d) & BTS_D2_AVRCP));
    CHECK(only_runtime_sent());
}
/* field 2026-11-09 evening (ota_main a6fdc5a2, flags 0x24, strategy 0): the phone (A2DP 0, AVRCP 2), the PC
 * (A2DP 3, AVRCP 4), both registered and reporting PAUSED. */
static void f9e_setup(bts_t *b, bool sel_phone)
{
    fresh(b);
    b->ac_state = BTS_AC_ON;
    b->has_sel = 1;
    memcpy(b->sel_addr, sel_phone ? F9_PHONE : F9_PC, 6);
    line(b, "RING 0 62:81:81:81:11:a1 19 A2DP");
    line(b, "RING 2 62:81:81:81:11:a1 17 AVRCP");
    line(b, "RING 3 a2:83:83:83:13:23 19 A2DP");
    line(b, "RING 4 a2:83:83:83:13:23 17 AVRCP");
    run(b, 900, true);
    line(b, "AVRCP 2 REGISTER_NOTIFICATION_RSP INTERIM PLAYBACK_STATUS_CHANGED PAUSED");
    line(b, "AVRCP 4 REGISTER_NOTIFICATION_RSP INTERIM PLAYBACK_STATUS_CHANGED PAUSED");
    run(b, 600, true);
}

/* (1) `1323.6 AVRCP  REGISTER_NOTIFICATION_RSP CHANGED PLAYBACK_STATUS_CHANGED PLAYING` - no link id: the phone's
 * CHANGED, unattributed; the field firmware ignored the line (no `AVRCP <n>`), the phone's one-shot registration was
 * used up and never renewed.  Now every registered device is asked again.  (2) the phone then paused / played by itself
 * (`STOP 0` / `START 0` …, no AVRCP lines), its status still the old PAUSED: 「이 기기로 듣기」 on the PC sent no PAUSE
 * (`no PAUSE: it is paused`) and kept the phone's stream (`paused source keeps its stream`) - the phone played on.  A
 * stream it started after its last report is taken for playing: PAUSE, and suspended if it keeps streaming. */
static void test_btsrc_field_1109_evening_stale(void)
{
    bts_t b;
    f9e_setup(&b, true);
    int ph = dev_of(&b, F9_PHONE), pc = dev_of(&b, F9_PC);
    CHECK(ph >= 0 && pc >= 0);
    CHECK_EQ(bts_dev_ps(&b, ph) & 7, BTS_PS_PAUSED);
    /* (1) */
    int before = nsent;
    line(&b, "AVRCP  REGISTER_NOTIFICATION_RSP CHANGED PLAYBACK_STATUS_CHANGED PLAYING");
    CHECK_EQ(b.n.garbled, 1);
    run(&b, 1500, true);
    {
        int r1 = sent_index("AVRCP PDU 31 1", before), r2 = r1 >= 0 ? sent_index("AVRCP PDU 31 1", r1 + 1) : -1;
        CHECK(r1 > before && r2 > r1);          /* both registered devices asked again, one at a time */
        CHECK((r1 > 0 && !strcmp(sent[r1 - 1], "SET 2 SELECT")) || (r2 > 0 && !strcmp(sent[r2 - 1], "SET 2 SELECT")));
    }
    line(&b, "AVRCP 2 REGISTER_NOTIFICATION_RSP INTERIM PLAYBACK_STATUS_CHANGED PLAYING");
    line(&b, "AVRCP 4 REGISTER_NOTIFICATION_RSP INTERIM PLAYBACK_STATUS_CHANGED PAUSED");
    CHECK_EQ(bts_dev_ps(&b, ph) & 7, BTS_PS_PLAYING);
    /* an error line without a link is no status line: nothing asked */
    line(&b, "AVRCP 5103 ERROR_NO_CONTROL_CHANNEL");
    CHECK_EQ(b.n.garbled, 1);

    /* (2) the field case: the CHANGED lost altogether - the phone's last report PAUSED, then it streams by itself */
    f9e_setup(&b, true);
    ph = dev_of(&b, F9_PHONE);
    line(&b, "A2DP STREAMING START 0");
    run(&b, 300, true);
    line(&b, "A2DP STREAMING STOP 0");
    run(&b, 300, true);
    line(&b, "A2DP STREAMING START 0");
    run(&b, 2000, true);
    CHECK(b.dev[ph].streaming && (bts_dev_ps(&b, ph) & 7) == BTS_PS_PAUSED);
    before = nsent;
    CHECK_EQ(bts_use(&b, F9_PC, true), BTS_OK);
    run(&b, 500, true);
    {
        int pz = sent_index("AVRCP PAUSE", before);
        CHECK(pz > before && !strcmp(sent[pz - 1], "SET 2 SELECT"));   /* field: `no PAUSE: it is paused` */
        CHECK_EQ(b.n.pause_skips, 0);
    }
    run(&b, 800, true);                       /* its CHANGED lost again: it still streams at 0.7 s - suspended */
    {
        int st = sent_index("A2DP STREAMING STOP", before);
        CHECK(st > before && !strcmp(sent[st - 1], "SET 0 SELECT"));
    }
    /* a report newer than the stream (the phone reports PAUSED after its start): no PAUSE (a toggle on Windows), but
     * still streaming at 0.7 s it is suspended before the PC starts (no longer left open) */
    f9e_setup(&b, true);
    line(&b, "A2DP STREAMING START 0");
    run(&b, 2000, true);
    line(&b, "AVRCP 2 REGISTER_NOTIFICATION_RSP INTERIM PLAYBACK_STATUS_CHANGED PAUSED");   /* (as on a re-register) */
    before = nsent;
    CHECK_EQ(bts_use(&b, F9_PC, true), BTS_OK);
    run(&b, 1300, true);
    CHECK_EQ(sent_index("AVRCP PAUSE", before), -1);
    CHECK(b.n.pause_skips >= 1);
    {
        int st = sent_index("A2DP STREAMING STOP", before);
        CHECK(st > before && !strcmp(sent[st - 1], "SET 0 SELECT"));
        CHECK_EQ(sent_index("A2DP STREAMING START", before), -1);   /* nothing to the PC before the phone's STOP */
    }
    CHECK(only_runtime_sent());
}

/* run 2 (1524.9): PC selected and playing (A2DP 3 streams), the phone plays too (its stream open, status stale
 * PAUSED).  「이 기기로 듣기」 on the phone: PAUSE to the PC -> CHANGED PAUSED, its stream kept open (Windows).  The
 * field firmware kept it (`paused source keeps its stream: no suspend`), sent the sink's START to the phone - only
 * echoed - and PLAY to the playing phone: the module stayed on the paused PC, silence.  Now: the PC suspended at 0.7 s,
 * then the sink's START on the phone (its stream predates the pick: points the output at it), no PLAY to it (it
 * streams after its last report). */
static void test_btsrc_field_1109_evening_kept(void)
{
    bts_t b;
    f9e_setup(&b, false);
    line(&b, "A2DP STREAMING START 3");
    line(&b, "AVRCP 4 REGISTER_NOTIFICATION_RSP CHANGED PLAYBACK_STATUS_CHANGED PLAYING");
    run(&b, 600, true);
    line(&b, "AVRCP 4 REGISTER_NOTIFICATION_RSP INTERIM PLAYBACK_STATUS_CHANGED PLAYING");
    run(&b, 300, true);
    line(&b, "A2DP STREAMING START 0");       /* the user plays on the phone: no AVRCP line (the field) */
    run(&b, BTS_GUARD_MS - 50, true);         /* (picked before the guard refuses it, BTS_GUARD_MS) */
    int ph = dev_of(&b, F9_PHONE);
    CHECK(b.dev[ph].streaming && (bts_dev_ps(&b, ph) & 7) == BTS_PS_PAUSED);
    int before = nsent;
    CHECK_EQ(bts_use(&b, F9_PHONE, true), BTS_OK);
    run(&b, 300, true);
    {
        int pz = sent_index("AVRCP PAUSE", before);
        CHECK(pz > before && !strcmp(sent[pz - 1], "SET 4 SELECT"));
    }
    line(&b, "AVRCP 4 REGISTER_NOTIFICATION_RSP CHANGED PLAYBACK_STATUS_CHANGED PAUSED");
    run(&b, 300, true);
    line(&b, "AVRCP 4 REGISTER_NOTIFICATION_RSP INTERIM PLAYBACK_STATUS_CHANGED PAUSED");
    run(&b, 700, true);
    int st = sent_index("A2DP STREAMING STOP", before);
    CHECK(st > before && !strcmp(sent[st - 1], "SET 3 SELECT"));   /* field: kept, silence */
    CHECK_EQ(sent_index("A2DP STREAMING START", before), -1);      /* not before the PC's STOP */
    line(&b, "A2DP STREAMING STOP 3");
    run(&b, 2000, true);
    {
        int ss = sent_index("A2DP STREAMING START", st);
        CHECK(ss > st && !strcmp(sent[ss - 1], "SET 0 SELECT"));   /* re-assert: the phone's open stream */
        CHECK_EQ(count_exact("A2DP STREAMING START"), 1);
    }
    CHECK_EQ(sent_index("AVRCP PLAY", before), -1);                /* field: PLAY to the playing phone */
    CHECK_EQ(b.sw_phase, BTS_SW_IDLE);
    CHECK_EQ(b.sw_result, BTS_RES_PLAYING);
    CHECK(b.dev[ph].streaming);
    CHECK(only_runtime_sent());
}

/* run 3 (1862.7): the phone selected, playing, fresh status; the PC paused, no stream.  「이 기기로 듣기」 on the PC:
 * PAUSE to the phone -> CHANGED PAUSED; the field firmware kept the paused phone's stream, the sink's START on the PC ->
 * `A2DP STREAMING START 3`, PLAY to the PC -> PC playing - then the phone's own STOP 3 s after its pause and the piano was
 * silent [inference: the output follows the last stream event].  Now: the phone suspended at 0.7 s (its STOP before the
 * PC starts), then PLAY to the PC; and a later STOP of another source within BTS_SW_REASSERT_MS re-sends the sink's START
 * on the PC once.  Then the follow-up: the user re-picks the selected, streaming PC - the sink's START on it (the field
 * firmware sent nothing: `no PLAY: not paused by us`, 0.0 s). */
static void test_btsrc_field_1109_evening_late_stop(void)
{
    bts_t b;
    f9e_setup(&b, true);
    line(&b, "A2DP STREAMING START 0");
    line(&b, "AVRCP 2 REGISTER_NOTIFICATION_RSP CHANGED PLAYBACK_STATUS_CHANGED PLAYING");
    run(&b, 600, true);
    line(&b, "AVRCP 2 REGISTER_NOTIFICATION_RSP INTERIM PLAYBACK_STATUS_CHANGED PLAYING");
    run(&b, 300, true);
    int pc = dev_of(&b, F9_PC);
    int before = nsent;
    CHECK_EQ(bts_use(&b, F9_PC, true), BTS_OK);
    run(&b, 300, true);
    {
        int pz = sent_index("AVRCP PAUSE", before);
        CHECK(pz > before && !strcmp(sent[pz - 1], "SET 2 SELECT"));
    }
    line(&b, "AVRCP 2 REGISTER_NOTIFICATION_RSP CHANGED PLAYBACK_STATUS_CHANGED PAUSED");
    run(&b, 300, true);
    line(&b, "AVRCP 2 REGISTER_NOTIFICATION_RSP INTERIM PLAYBACK_STATUS_CHANGED PAUSED");
    run(&b, 700, true);
    int st = sent_index("A2DP STREAMING STOP", before);
    CHECK(st > before && !strcmp(sent[st - 1], "SET 0 SELECT"));   /* field: `paused source keeps its stream` */
    CHECK_EQ(sent_index("A2DP STREAMING START", before), -1);
    CHECK_EQ(sent_index("AVRCP PLAY", before), -1);
    line(&b, "A2DP STREAMING STOP 0");
    run(&b, 1000, true);
    {
        int pl = sent_index("AVRCP PLAY", st);
        CHECK(pl > st && !strcmp(sent[pl - 1], "SET 4 SELECT"));   /* the PC reports PAUSED: PLAY */
    }
    line(&b, "A2DP STREAMING START 3");
    run(&b, 300, true);
    CHECK_EQ(b.sw_phase, BTS_SW_IDLE);
    CHECK_EQ(b.sw_result, BTS_RES_PLAYING);
    CHECK_EQ(count_exact("A2DP STREAMING START"), 0);
    /* a late STOP of the phone (as the field's, 3 s after its pause - here its own short start refused by nothing yet) */
    line(&b, "A2DP STREAMING START 0");
    run(&b, 200, true);                       /* (shorter than BTS_GUARD_MS: the guard sends nothing) */
    int mark = nsent;
    line(&b, "A2DP STREAMING STOP 0");
    run(&b, 400, true);
    {
        int ss = sent_index("A2DP STREAMING START", mark);
        CHECK(ss > mark && !strcmp(sent[ss - 1], "SET 3 SELECT"));   /* the output pointed at the PC again */
    }
    line(&b, "A2DP STREAMING START 0");
    run(&b, 200, true);
    line(&b, "A2DP STREAMING STOP 0");
    run(&b, 400, true);
    CHECK_EQ(count_exact("A2DP STREAMING START"), 1);                /* once per switch */
    CHECK(b.dev[pc].streaming);
    /* the follow-up: the PC selected and streaming, the user picks it again - the sink's START on it, no PLAY (its
     * player not paused by us) */
    line(&b, "AVRCP 4 REGISTER_NOTIFICATION_RSP CHANGED PLAYBACK_STATUS_CHANGED PLAYING");
    run(&b, 600, true);
    line(&b, "AVRCP 4 REGISTER_NOTIFICATION_RSP INTERIM PLAYBACK_STATUS_CHANGED PLAYING");
    run(&b, 300, true);
    mark = nsent;
    CHECK_EQ(bts_use(&b, F9_PC, true), BTS_OK);
    run(&b, 600, true);
    {
        int ss = sent_index("A2DP STREAMING START", mark);
        CHECK(ss > mark && !strcmp(sent[ss - 1], "SET 3 SELECT"));
    }
    CHECK_EQ(sent_index("AVRCP PLAY", mark), -1);
    CHECK_EQ(sent_index("AVRCP PAUSE", mark), -1);
    CHECK_EQ(b.sw_result, BTS_RES_PLAYING);
    CHECK(only_runtime_sent());
}

/* field 2026-11-09 (ota_main 048ec415): AUTOCALL off, reconnect 2, nothing preferred, the
 * last selected device an Android TV the user had disconnected in the TV's settings.  After a module reboot the TV is
 * called; it accepts (`CALL 0` · `CONNECT 0 / 1 A2DP 19`) and closes both A2DP links 0.3 s later (`NO CARRIER 1 / 0
 * ERROR 0`).  The field firmware took the CONNECT for success, restarted the backoff and called every ~5 s (771.3,
 * 777.2, 782.8, 787.8, 793.0 …).  Now: such a short connection is a failed try (4 / 8 / 15 / 30 / 60 s, 10 at most);
 * a source that closes its own A2DP after a normal connection (the user, there) is not called as "the last selected"
 * any more - until it is picked again or connects by itself and stays; the preferred device is still called, backed
 * off; a link loss (another code) and our own CLOSE change nothing */
static void test_btsrc_field_1109_tv_recall(void)
{
    bts_t b;
    uint8_t tv[6];
    uint32_t at[32];
    int n;
    m_init(&b);
    m_addr(M_TV, tv);
    M.autocall = 0;
    CHECK_EQ(bts_autocall(&b, BTS_AC_READ), BTS_OK);
    m_run(500, true);
    CHECK_EQ(b.ac_state, BTS_AC_OFF);
    CHECK_EQ(b.pol.recon, BTS_RC_PREF_LAST);
    /* the TV was selected (its disconnect came before this power-on's lines - e.g. while the module rebooted) */
    b.has_sel = 1;
    memcpy(b.sel_addr, tv, 6);
    M.d[M_TV].close_after_ms = 300;
    m_reboot();
    n = m_calls_during(&b, 120000, at, 32);
    /* field: a CALL every ~5 s (~22 in 2 min); now 4 s after READY., then 8 / 15 / 30 s after each close */
    CHECK_EQ(n, 4);
    CHECK(n >= 2 && at[1] - at[0] > 8000u);
    CHECK(n >= 4 && at[3] - at[2] > 30000u);
    m_run(900000, true);
    CHECK_EQ(b.n.reconnects, BTS_RC_TRIES);           /* the limit holds: the TV stops being paged */
    CHECK_EQ(b.sel_norc, 0);                          /* short connections never count as "the user closed it" */
    CHECK(only_runtime_sent());

    /* the user disconnects the selected TV on the TV after a normal connection: not called again this power-on - not
     * after a module reboot either */
    m_init(&b);
    M.autocall = 0;
    CHECK_EQ(bts_autocall(&b, BTS_AC_READ), BTS_OK);
    m_run(500, true);
    m_connect_in(M_TV);
    m_play(M_TV);
    m_run(5000, true);
    CHECK(M.d[M_TV].streaming);
    CHECK(m_selected(M_TV));
    m_drop(M_TV);                                     /* NO CARRIER … ERROR 0 for each link */
    CHECK_EQ(b.sel_norc, 1);
    CHECK_EQ(m_calls_during(&b, 120000, at, 32), 0);
    m_reboot();
    CHECK_EQ(m_calls_during(&b, 60000, at, 32), 0);
    CHECK(b.has_sel && !memcmp(b.sel_addr, tv, 6));   /* still the selected one (the app shows it) */
    /* it connects by itself and stays: a target again; lost out of range -> called (4 s) */
    m_connect_in(M_TV);
    m_run(2000, true);
    CHECK_EQ(b.sel_norc, 1);                          /* not yet: 3 s */
    m_run(2000, true);
    CHECK_EQ(b.sel_norc, 0);
    m_lose(M_TV);
    CHECK_EQ(b.sel_norc, 0);                          /* a link loss is not the user */
    n = m_calls_during(&b, 5000, at, 32);
    CHECK_EQ(n, 1);
    m_until_a2dp(M_TV, 5000);
    CHECK(M.d[M_TV].a2dp >= 0);                       /* called back */
    /* the user closes it there right after it connected (< 3 s): only a failed try, still a target - called again 8 s
     * later (the try was not reset) */
    m_run(1000, true);
    m_drop(M_TV);
    CHECK_EQ(b.sel_norc, 0);
    CHECK_EQ(m_calls_during(&b, 7500, at, 32), 0);
    CHECK_EQ(m_calls_during(&b, 1000, at, 32), 1);
    m_until_a2dp(M_TV, 5000);
    CHECK(M.d[M_TV].a2dp >= 0);
    m_run(4000, true);
    /* our own CLOSE (act 6 「소리 연결만 끊기」) is not the device's: the TV stays the target */
    CHECK_EQ(bts_action(&b, BTS_ACT_MUTE, tv), BTS_OK);
    m_run(1000, true);
    CHECK_EQ(M.d[M_TV].a2dp, -1);
    CHECK_EQ(b.sel_norc, 0);
    /* the user disconnects it there; then picks it in the app: called again */
    m_until_a2dp(M_TV, 8000);                         /* called back after the mute (as before) */
    CHECK(M.d[M_TV].a2dp >= 0);
    m_run(4000, true);
    m_drop(M_TV);
    CHECK_EQ(b.sel_norc, 1);
    CHECK_EQ(m_calls_during(&b, 30000, at, 32), 0);
    nsent = 0;
    CHECK_EQ(bts_use(&b, tv, true), BTS_OK);
    CHECK_EQ(b.sel_norc, 0);
    m_run(3000, true);
    CHECK_EQ(count_sent("CALL 12:84:04:04:14:24 19 A2DP"), 1);

    /* the preferred TV closed by the user is still called (preferred rules unchanged), backed off when it closes at once */
    m_init(&b);
    M.autocall = 0;
    CHECK_EQ(bts_autocall(&b, BTS_AC_READ), BTS_OK);
    m_run(500, true);
    b.pol.has_pref = 1;
    memcpy(b.pol.pref, tv, 6);
    m_connect_in(M_TV);
    m_play(M_TV);
    m_run(5000, true);
    CHECK(m_selected(M_TV));
    M.d[M_TV].close_after_ms = 300;
    m_drop(M_TV);
    n = m_calls_during(&b, 120000, at, 32);
    CHECK_EQ(n, 4);                                   /* 4 s after the close, then 8 / 15 / 30 s after each short one */
}

/* ---- field 2026-11-10 (ota_main c0b89c60, strategy 0, mode 2, flags 0x04; the PC's AVRCP
 * service disabled on Windows - A2DP only).  Phone 62:81…11:a1 (A2DP 0/1, AVRCP 2), PC a2:83…13:23 (A2DP 4/5). */
static void f10_setup(bts_t *b)
{
    fresh(b);
    b->pol.flags |= BTS_F_NO_AVRCP_CALL;   /* (the field's flags 0x04 CALLed the PC's 17 and gave up - not the point here) */
    b->ac_state = BTS_AC_ON;
    b->has_sel = 1;
    memcpy(b->sel_addr, F9_PHONE, 6);
    line(b, "RING 0 62:81:81:81:11:a1 19 A2DP");
    line(b, "RING 1 62:81:81:81:11:a1 19 A2DP");
    line(b, "RING 2 62:81:81:81:11:a1 17 AVRCP");
    line(b, "RING 4 a2:83:83:83:13:23 19 A2DP");
    line(b, "RING 5 a2:83:83:83:13:23 19 A2DP");
    run(b, 900, true);
    line(b, "AVRCP 2 REGISTER_NOTIFICATION_RSP INTERIM PLAYBACK_STATUS_CHANGED PLAYING");
    line(b, "A2DP STREAMING START 0");          /* step 1: the phone plays (-11 dBFS) */
    run(b, 1500, true);
}

/* step 2 as the field firmware ran it (susp_ok 1 = the sink stopped the phone 0.7 s after the PAUSE): `880.4 SET 2 SELECT
 * / AVRCP PAUSE` - no CHANGED from the phone - `881.1 SET 0 SELECT / A2DP STREAMING STOP`, then the PC (no AVRCP): the
 * sink's `SET 4 SELECT / A2DP STREAMING START`.  The module echoes both commands without a link id; c0b89c60 took the
 * echoed START for the newest A2DP source's own start - the phone: "streaming", its "paused by us" cleared, its report
 * older than that start.  Step 3 then re-sent START to the phone and skipped the PLAY (`no PLAY: it plays`).  Now the echo
 * is no event; 1 s after the PAUSE with no report the phone is registered again.  Now the PC (no AVRCP) gets
 * no sink START at all (READY - its own play opens its stream); the START echo is checked on a re-pick's START. */
static void test_btsrc_field_1110_echo(void)
{
    bts_t b;
    f10_setup(&b);
    int ph = dev_of(&b, F9_PHONE), pc = dev_of(&b, F9_PC);
    CHECK(ph >= 0 && pc >= 0 && b.dev[ph].streaming && !b.dev[pc].streaming);
    echo_on = 1;
    int before = nsent;
    CHECK_EQ(bts_use(&b, F9_PC, true), BTS_OK);
    run(&b, 300, true);
    CHECK(sent_index("AVRCP PAUSE", before) == before + 1 && !strcmp(sent[before], "SET 2 SELECT"));
    run(&b, 900, true);                          /* 0.7 s: the field firmware's sink STOP (susp_ok 1 here) */
    int st = sent_index("A2DP STREAMING STOP", before);
    CHECK(st > before && !strcmp(sent[st - 1], "SET 0 SELECT"));
    CHECK(b.dev[ph].streaming);                  /* the echo (no link id) is no STOP */
    line(&b, "A2DP STREAMING STOP 0");           /* the module's own line */
    CHECK(!b.dev[ph].streaming);
    run(&b, 2400, true);                         /* 1 s after the PAUSE with no report (counted while nothing is queued) the
                                                  * phone is asked again; the PC (no AVRCP): no sink START - READY */
    CHECK_EQ(count_exact("AVRCP PDU 31 1") - 1, 1);   /* (the first: at its AVRCP link) */
    CHECK_EQ(sent_index("A2DP STREAMING START", before), -1);   /* f2056ae8: `SET 4 SELECT` + `A2DP STREAMING START` */
    CHECK_EQ(b.sw_result, BTS_RES_READY);
    CHECK(!b.dev[ph].streaming);
    CHECK(b.dev[ph].ps_ours);
    line(&b, "A2DP STREAMING START 4");          /* the user plays on the PC: its own stream */
    CHECK(b.dev[pc].streaming && !b.dev[ph].streaming);
    /* the START echo: the phone plays by itself, the PC's stream stops (the PC = the newest stream, the echo's old
     * owner), 「이 기기로 듣기」 on the phone that streams - the sink's START once (`re-assert START`, AVRCP), echoed */
    line(&b, "A2DP STREAMING START 0");
    line(&b, "A2DP STREAMING STOP 4");
    CHECK(b.dev[ph].streaming && !b.dev[pc].streaming && !b.dev[ph].ps_ours);
    before = nsent;
    CHECK_EQ(bts_use(&b, F9_PHONE, true), BTS_OK);
    run(&b, 600, true);
    int sa = sent_index("A2DP STREAMING START", before);
    CHECK(sa > before && !strcmp(sent[sa - 1], "SET 0 SELECT"));
    CHECK(!b.dev[pc].streaming);                 /* c0b89c60: the echo = the newest source's own START */
    /* an address-less STREAMING line with no command of ours before it: still the module's (as before) */
    run(&b, 2000, true);
    CHECK_EQ(b.echo_ss, 0);
    echo_on = 0;
}

/* step 3: 「이 기기로 듣기」 on the phone while the PC plays.  c0b89c60 (its state after step 2 as above: the phone
 * "streaming", "paused by us" cleared, the last report PLAYING): `905.4 # re-assert START` / `SET 0 SELECT` / `A2DP
 * STREAMING START` / `# no PLAY: it plays` -> the phone stayed paused, -52 dBFS.  Now: (a) the phone the piano paused
 * gets AVRCP PLAY whatever its last report (stale PLAYING here), no sink START before it; (b) the PC (no AVRCP) is never
 * stopped by the sink - the switch waits for its own stop (BUSY after BTS_SW_HELD_MS) */
static void test_btsrc_field_1110_paused_by_us(void)
{
    bts_t b;
    /* (a) the decision, from the state the field firmware had - the phone's stream flag stale (1), report PLAYING, paused
     * by us; nothing else streams */
    f10_setup(&b);
    b.susp_ok = 0;                               /* (the phone's stream flag stays as the field firmware had it) */
    int ph = dev_of(&b, F9_PHONE);
    int before = nsent;
    CHECK_EQ(bts_use(&b, F9_PC, true), BTS_OK);
    run(&b, 300, true);
    CHECK(sent_index("AVRCP PAUSE", before) > 0 && b.dev[ph].ps_ours);
    CHECK_EQ(b.dev[ph].ps, BTS_PS_PLAYING);      /* no CHANGED */
    bts_close(&b, F9_PC);                        /* (out of the picture: the PC) */
    run(&b, 200, true);
    line(&b, "NO CARRIER 4 ERROR 0");
    line(&b, "NO CARRIER 5 ERROR 0");
    b.pol.flags |= BTS_F_NO_PS;                  /* (no re-registration: the stale report stays - the rule alone) */
    run(&b, 3000, true);
    CHECK(b.dev[ph].streaming && b.dev[ph].ps == BTS_PS_PLAYING && b.dev[ph].ps_ours);
    b.pol.flags &= (uint8_t)~BTS_F_NO_PS;
    before = nsent;
    CHECK_EQ(bts_use(&b, F9_PHONE, true), BTS_OK);
    run(&b, 600, true);
    CHECK_EQ(sent_index("A2DP STREAMING START", before), -1);   /* c0b89c60: `re-assert START` first */
    int p = sent_index("AVRCP PLAY", before);
    CHECK(p > before && !strcmp(sent[p - 1], "SET 2 SELECT"));  /* c0b89c60: `no PLAY: it plays` */
    CHECK_EQ(b.sw_tried & (BTS_SWT_KEPT | BTS_SWT_START), 0);

    /* (b) the default (susp_ok 0): the phone plays, PC (no AVRCP) picked - PAUSE to the phone, never a sink STOP; the
     * phone stops itself ~3 s later (Android); the PC: no sink START - READY, the user plays on the PC */
    f10_setup(&b);
    b.susp_ok = 0;
    ph = dev_of(&b, F9_PHONE);
    int pc = dev_of(&b, F9_PC);
    echo_on = 1;
    before = nsent;
    CHECK_EQ(bts_use(&b, F9_PC, true), BTS_OK);
    run(&b, 2900, true);
    CHECK_EQ(count_exact("A2DP STREAMING STOP"), 0);
    CHECK_EQ(b.sw_phase, BTS_SW_PAUSE);
    CHECK(b.sw_held);
    line(&b, "AVRCP 2 REGISTER_NOTIFICATION_RSP INTERIM PLAYBACK_STATUS_CHANGED PAUSED");   /* the re-registration */
    line(&b, "A2DP STREAMING STOP 0");           /* its own STOP */
    run(&b, 700, true);
    CHECK_EQ(sent_index("A2DP STREAMING START", before), -1);
    CHECK_EQ(b.sw_result, BTS_RES_READY);
    line(&b, "A2DP STREAMING START 4");          /* the user plays on the PC */
    run(&b, 1200, true);
    CHECK_EQ(b.sw_result, BTS_RES_READY);
    CHECK(b.dev[pc].streaming && !b.dev[ph].streaming && b.dev[ph].ps_ours);
    /* step 3: the PC plays, 「이 기기로 듣기」 on the phone: nothing stops the PC; the phone is not asked to play over it */
    before = nsent;
    CHECK_EQ(bts_use(&b, F9_PHONE, true), BTS_OK);
    run(&b, BTS_SW_HELD_MS - 500, true);
    CHECK_EQ(b.sw_phase, BTS_SW_PAUSE);
    run(&b, 1000, true);
    CHECK_EQ(b.sw_result, BTS_RES_BUSY);
    CHECK_EQ(count_exact("A2DP STREAMING STOP"), 0);
    CHECK_EQ(sent_index("AVRCP PLAY", before), -1);
    CHECK_EQ(sent_index("A2DP STREAMING START", before), -1);
    CHECK(b.dev[pc].streaming && b.dev[pc].held);
    /* the user pauses on the PC; Windows lets the stream go after the BUSY: the switch goes on by itself -
     * PLAY (paused by us), no START */
    line(&b, "A2DP STREAMING STOP 4");
    run(&b, 800, true);
    int p2 = sent_index("AVRCP PLAY", before);
    CHECK(p2 > before && !strcmp(sent[p2 - 1], "SET 2 SELECT"));
    line(&b, "A2DP STREAMING START 0");
    run(&b, 300, true);
    CHECK_EQ(b.sw_result, BTS_RES_PLAYING);
    CHECK_EQ(sent_index("A2DP STREAMING START", before), -1);
    CHECK(only_runtime_sent());
    echo_on = 0;
}

/* the guard (default susp_ok 0): a source that is not the selected one starts - PAUSE (AVRCP), never a sink STOP, also
 * the third time; without AVRCP nothing at all (logged once) */
static void test_btsrc_field_1110_guard(void)
{
    bts_t b;
    fresh(&b);
    b.susp_ok = 0;
    b.has_sel = 1;
    memcpy(b.sel_addr, F9_PHONE, 6);
    line(&b, "RING 0 62:81:81:81:11:a1 19 A2DP");
    line(&b, "RING 2 62:81:81:81:11:a1 17 AVRCP");
    line(&b, "RING 3 a2:83:83:83:13:23 19 A2DP");
    line(&b, "RING 4 a2:83:83:83:13:23 17 AVRCP");
    run(&b, 900, true);
    line(&b, "AVRCP 4 REGISTER_NOTIFICATION_RSP INTERIM PLAYBACK_STATUS_CHANGED PLAYING");
    for (int k = 0; k < 4; k++) {
        line(&b, "A2DP STREAMING START 3");
        run(&b, 4000, true);
        line(&b, "A2DP STREAMING STOP 3");
        run(&b, 200, true);
    }
    CHECK(count_exact("AVRCP PAUSE") >= 1);
    CHECK_EQ(count_exact("A2DP STREAMING STOP"), 0);
    CHECK_EQ(b.n.held, 0);                       /* (AVRCP: PAUSE only - q_suspend never reached) */
}

/* field 2026-11-10 night (ota_main f2056ae8): the PC = …13:23 with its Windows AVRCP
 * service off (A2DP 0/1 only), the phone = …11:a1 (A2DP 2/3, AVRCP 4).  The default policy (susp_ok 0); the sink does
 * not open the PC's AVRCP here (flag 0x20 - the field's flags 0x04 CALLed its 17 and gave up, not the point). */
static void f11_setup(bts_t *b)
{
    fresh(b);
    b->susp_ok = 0;
    b->pol.flags |= BTS_F_NO_AVRCP_CALL;
    b->pol.xopt = 0;                            /* the 11-10 night replay had no late stop (on by default since 2026-11-13) */
    b->ac_state = BTS_AC_ON;
    b->has_sel = 1;
    memcpy(b->sel_addr, F9_PHONE, 6);
    line(b, "RING 0 a2:83:83:83:13:23 19 A2DP");
    line(b, "RING 1 a2:83:83:83:13:23 19 A2DP");
    line(b, "RING 2 62:81:81:81:11:a1 19 A2DP");
    line(b, "RING 3 62:81:81:81:11:a1 19 A2DP");
    line(b, "RING 4 62:81:81:81:11:a1 17 AVRCP");
    run(b, 900, true);
    line(b, "AVRCP 4 REGISTER_NOTIFICATION_RSP INTERIM PLAYBACK_STATUS_CHANGED PLAYING");
    line(b, "A2DP STREAMING START 2");          /* step 1: the phone plays (-21...-34 dBFS) */
    run(b, 1500, true);
}

/* step 2: 「이 기기로 듣기」 on the PC (paused, no AVRCP) while the phone plays.  f2056ae8: `159.1 AVRCP PAUSE` (phone) ->
 * `162.3 STREAMING STOP 2` (its own, 3.2 s) -> `162.5 SET 0 SELECT` / `A2DP STREAMING START` -> `162.7 STREAMING START 0`
 * (the PC) - and when the PC played later the piano got nothing for minutes; only a Windows PnP restart helped, after
 * which the stream the PC opened itself had sound.  Now: no sink START to a source without AVRCP - READY ("press play
 * on that device"), the PC's own start opens its stream; nothing more is sent. */
/* `SET <l> SELECT` goes out, its echo lets the paired command go at once (not BTS_CMD_GAP_MS later);
 * without the echo the gap stays.  The echo with its head glued to the line before (field 2026-11-13: `< T 3 SELECT`
 * after `> SET 3 SELECT` - the paired STOP then waited the full gap) or other bytes in front counts too; another link's
 * echo does not */
static void test_btsrc_pair_gap(void)
{
    static const char *const ECHO[] = {NULL, "SET 4 SELECT", "T 4 SELECT", "ET 4 SELECT", "4 SELECT", "  T 4 SELECT",
                                       "PAUSEDSET 4 SELECT", "T 3 SELECT", "SET 44 SELECT", "T 4 SELECTED", "4 SELECT X"};
    static const bool QUICK[] = {false, true, true, true, true, true, true, false, false, false, false};
    for (unsigned e = 0; e < sizeof ECHO / sizeof ECHO[0]; e++) {
        bts_t b;
        f11_setup(&b);
        CHECK_EQ(bts_use(&b, F9_PC, true), BTS_OK);
        char c[BTS_OUT_MAX];
        uint32_t t = 0, t_sel = 0, t_cmd = 0;
        bool got_sel = false;
        for (; t < 2000 && !t_cmd; t += 2) {
            bts_tick(&b, 2, true);
            if (bts_next(&b, c, sizeof c)) {
                if (!got_sel && !strncmp(c, "SET 4 SELECT", 12)) {
                    got_sel = true;
                    t_sel = t;
                    if (ECHO[e])
                        line(&b, ECHO[e]);
                } else if (got_sel) {
                    CHECK(!strcmp(c, "AVRCP PAUSE"));
                    t_cmd = t;
                }
            }
        }
        CHECK(got_sel && t_cmd);
        if (QUICK[e])
            CHECK(t_cmd - t_sel <= 4);
        else
            CHECK(t_cmd - t_sel >= BTS_CMD_GAP_MS);
        if (QUICK[e] != (t_cmd - t_sel <= 4))
            printf("  pair gap: echo '%s' -> %u ms\n", ECHO[e] ? ECHO[e] : "(none)", (unsigned)(t_cmd - t_sel));
    }
}

static void test_btsrc_field_1110_night_no_start(void)
{
    bts_t b;
    f11_setup(&b);
    echo_on = 1;
    int ph = dev_of(&b, F9_PHONE), pc = dev_of(&b, F9_PC);
    CHECK(ph >= 0 && pc >= 0 && b.dev[ph].streaming);
    int before = nsent;
    CHECK_EQ(bts_use(&b, F9_PC, true), BTS_OK);
    run(&b, 300, true);
    CHECK(sent_index("AVRCP PAUSE", before) == before + 1 && !strcmp(sent[before], "SET 4 SELECT"));
    line(&b, "AVRCP 4 REGISTER_NOTIFICATION_RSP CHANGED PLAYBACK_STATUS_CHANGED PAUSED");
    run(&b, 300, true);
    line(&b, "AVRCP 4 REGISTER_NOTIFICATION_RSP INTERIM PLAYBACK_STATUS_CHANGED PAUSED");
    run(&b, 2600, true);
    CHECK_EQ(b.sw_phase, BTS_SW_PAUSE);         /* waits for the phone's own STOP */
    line(&b, "A2DP STREAMING STOP 2");
    run(&b, 1500, true);
    CHECK_EQ(count_exact("A2DP STREAMING START"), 0);   /* f2056ae8: `SET 0 SELECT` + `A2DP STREAMING START` */
    CHECK_EQ(b.sw_phase, BTS_SW_IDLE);
    CHECK_EQ(b.sw_result, BTS_RES_READY);       /* the app: press play on the PC */
    CHECK_EQ(b.sw_tried, 0);
    CHECK_EQ(bts_sel_dev(&b), pc);
    CHECK(!b.dev[pc].streaming && b.dev[ph].ps_ours);
    /* the PC plays: its own stream (with sound in the field); the piano sends nothing (selected, no AVRCP) */
    before = nsent;
    line(&b, "A2DP STREAMING START 0");
    run(&b, 5000, true);
    CHECK(b.dev[pc].streaming);
    CHECK_EQ(nsent, before);
    /* 「이 기기로 듣기」 on the PC again while it streams: no AVRCP - not even the re-pick START (an open stream) */
    CHECK_EQ(bts_use(&b, F9_PC, true), BTS_OK);
    run(&b, 1500, true);
    CHECK_EQ(nsent, before);
    CHECK_EQ(b.sw_result, BTS_RES_PLAYING);
    CHECK_EQ(count_exact("A2DP STREAMING STOP"), 0);
    CHECK(only_runtime_sent());
    echo_on = 0;
}

/* steps 3 / 4: the PC (no AVRCP) plays, 「이 기기로 듣기」 on the phone.  f2056ae8: the app 「…그 기기에서 일시정지하세요」,
 * the user paused the PC ~10 s later, `1281.1` BUSY 12 s after the pick, the PC let its stream go by itself at `1300.1`
 * (`STREAMING STOP 0`, ~20 s after the pause, 31 s after the pick) - the piano sent nothing more and the app kept 「다른
 * 기기가 소리를 놓지 않습니다」.  Now: the wait is BTS_SW_HELD_MS (30 s); a STOP after the BUSY (within
 * BTS_SW_RESUME_MS) goes on with the switch - the phone (paused by the piano in step 2) gets its PLAY; later than that
 * the BUSY is only cleared.  (b) = step 4 as the field had it: the phone started by itself (adb) at `1314.4` - the
 * piano sends nothing to the selected phone's own stream (its silence there is (c), not a command of ours). */
static void test_btsrc_field_1110_night_late_stop(void)
{
    bts_t b;
    for (int late = 0; late < 2; late++) {
        f11_setup(&b);
        echo_on = 1;
        int ph = dev_of(&b, F9_PHONE), pc = dev_of(&b, F9_PC);
        /* step 2 as above, the PC then plays */
        CHECK_EQ(bts_use(&b, F9_PC, true), BTS_OK);
        run(&b, 300, true);
        line(&b, "AVRCP 4 REGISTER_NOTIFICATION_RSP CHANGED PLAYBACK_STATUS_CHANGED PAUSED");
        run(&b, 300, true);
        line(&b, "AVRCP 4 REGISTER_NOTIFICATION_RSP INTERIM PLAYBACK_STATUS_CHANGED PAUSED");
        run(&b, 2600, true);
        line(&b, "A2DP STREAMING STOP 2");
        run(&b, 1500, true);
        line(&b, "A2DP STREAMING START 0");
        run(&b, 3000, true);
        CHECK(b.dev[pc].streaming && b.dev[ph].ps_ours);
        /* step 3 */
        int before = nsent;
        CHECK_EQ(bts_use(&b, F9_PHONE, true), BTS_OK);
        run(&b, 12500, true);
        CHECK_EQ(b.sw_phase, BTS_SW_PAUSE);     /* f2056ae8: BUSY at 12 s */
        CHECK(b.sw_held);
        CHECK_EQ(nsent, before);                /* nothing to the PC (no AVRCP), nothing to the phone over its stream */
        if (!late) {
            run(&b, 18500, true);               /* 31 s after the pick: the PC's own STOP (field 1300.1) */
            CHECK_EQ(b.sw_result, BTS_RES_BUSY);   /* (30 s: BUSY a moment before) */
            line(&b, "A2DP STREAMING STOP 0");
            run(&b, 900, true);
            int p = sent_index("AVRCP PLAY", before);
            CHECK(p > before && !strcmp(sent[p - 1], "SET 4 SELECT"));   /* f2056ae8: nothing */
            CHECK(b.sw_phase == BTS_SW_PLAY);
            line(&b, "AVRCP 4 REGISTER_NOTIFICATION_RSP CHANGED PLAYBACK_STATUS_CHANGED PLAYING");
            line(&b, "A2DP STREAMING START 2");
            run(&b, 600, true);
            CHECK_EQ(b.sw_result, BTS_RES_PLAYING);
        } else {
            /* the PC holds its stream past the resume window: the BUSY stays while it streams, then is cleared */
            run(&b, 17500 + BTS_SW_RESUME_MS + 1000, true);
            CHECK_EQ(b.sw_result, BTS_RES_BUSY);
            line(&b, "A2DP STREAMING STOP 0");
            run(&b, 300, true);
            CHECK_EQ(b.sw_result, BTS_RES_NONE);   /* f2056ae8: BUSY - the app's stale 「다른 기기가 소리를 놓지 않습니다」 */
            CHECK_EQ(b.sw_phase, BTS_SW_IDLE);
            CHECK_EQ(sent_index("AVRCP PLAY", before), -1);
            /* step 4 (field 1314.4): the user plays the phone itself - its own stream, nothing from the piano */
            int n4 = nsent;
            line(&b, "AVRCP 4 REGISTER_NOTIFICATION_RSP CHANGED PLAYBACK_STATUS_CHANGED PLAYING");
            line(&b, "A2DP STREAMING START 2");
            run(&b, 3000, true);
            CHECK(b.dev[ph].streaming && !b.dev[pc].streaming && !b.dev[ph].ps_ours);
            for (int i = n4; i < nsent; i++)
                CHECK(!strcmp(sent[i], "AVRCP PDU 31 1") || !strcmp(sent[i], "SET 4 SELECT"));   /* re-registration only */
        }
        CHECK_EQ(count_exact("A2DP STREAMING START"), 0);
        CHECK_EQ(count_exact("A2DP STREAMING STOP"), 0);
        CHECK_EQ(b.n.held, 1);                  /* `# not stopped` once for the PC's stream */
        CHECK(only_runtime_sent());
        echo_on = 0;
    }
}

/* ---- the opt-in late stop (BTS_X_LATE_STOP) ---------------------------------------- */
/* field 2026-11-11 05:00-05:30 (ota_main 87579f5c, module `SET CONTROL AUDIO INTERNAL I2S_SLAVE EVENT` + `SET PROFILE
 * AVRCP CONTROLLER 0f`, the PC re-paired - its AVRCP back): the PC = …13:23 (Windows, A2DP 0/1, AVRCP 2), the phone =
 * …11:a1 (A2DP 3/4, AVRCP 5).  PC -> phone with the default strategy: `AVRCP PAUSE` to the PC at ~0.5 s -> `CHANGED
 * PAUSED` -> Windows keeps its stream, its own `A2DP STREAMING STOP 0` at ~11.7 s -> `AVRCP PLAY` to the phone -> sound
 * at 14.2 s.  Here: the PC selected and playing, the phone paused (stream closed). */
static void f12_setup(bts_t *b, uint8_t xopt)
{
    fresh(b);
    b->susp_ok = 0;                              /* the cave (-DBTS_NEVER_SUSPEND) */
    b->pol.xopt = xopt;
    b->ac_state = BTS_AC_ON;
    b->has_sel = 1;
    memcpy(b->sel_addr, F9_PC, 6);
    line(b, "RING 0 a2:83:83:83:13:23 19 A2DP");
    line(b, "RING 1 a2:83:83:83:13:23 19 A2DP");
    line(b, "RING 2 a2:83:83:83:13:23 17 AVRCP");
    line(b, "RING 3 62:81:81:81:11:a1 19 A2DP");
    line(b, "RING 4 62:81:81:81:11:a1 19 A2DP");
    line(b, "RING 5 62:81:81:81:11:a1 17 AVRCP");
    run(b, 900, true);
    line(b, "AVRCP 2 REGISTER_NOTIFICATION_RSP INTERIM PLAYBACK_STATUS_CHANGED PLAYING");
    run(b, 400, true);
    line(b, "AVRCP 5 REGISTER_NOTIFICATION_RSP INTERIM PLAYBACK_STATUS_CHANGED PAUSED");
    line(b, "A2DP STREAMING START 0");          /* the PC plays (`AUDIO ROUTE 0 A2DP …`, `A2DP CODEC AAC STEREO 48000`) */
    run(b, 1500, true);
    nsent = 0;
    memset(sent, 0, sizeof sent);
}

static int log_hits(const bts_t *b, const char *s)
{
    uint8_t out[2048];
    uint16_t first, next;
    uint32_t n = bts_log_read(b, 0, out, sizeof out, &first, &next), hits = 0, k = (uint32_t)strlen(s);
    for (uint32_t p = 0; p + k <= n; p++)
        hits += memcmp(out + p, s, k) == 0;
    return (int)hits;
}

/* a source answers our PAUSE with CHANGED PAUSED (the one-shot registration is then renewed: INTERIM PAUSED) */
static void src_paused(bts_t *b, const char *link)
{
    char c[96];
    snprintf(c, sizeof c, "AVRCP %s REGISTER_NOTIFICATION_RSP CHANGED PLAYBACK_STATUS_CHANGED PAUSED", link);
    line(b, c);
    run(b, 300, true);
    snprintf(c, sizeof c, "AVRCP %s REGISTER_NOTIFICATION_RSP INTERIM PLAYBACK_STATUS_CHANGED PAUSED", link);
    line(b, c);
}

/* PC -> phone, option on with a 4.5 s wait (xwait 45 = 4.5 s, counted from the PAUSED report): PAUSE
 * -> PAUSED -> the PC holds its stream -> 4.5 s after the report `SET 0 SELECT` +
 * `A2DP STREAMING STOP` (event `late STOP`; the module's echo without a link id is no event) -> the PC's `STOP 0` -> PLAY
 * to the phone -> its own `START 3`.  Then back to the PC: the phone stops by itself at 3.2 s (before the grace - no
 * STOP to it), the PC the sink stopped gets PLAY (paused by us), no sink START; its own `START 0`.  Three rounds (the
 * field steps); in round 2 Windows opens no stream within BTS_SW_READY_MS: READY (「그 기기에서 재생」), its
 * START right after turns the result into PLAYING. */
static void test_btsrc_late_stop_pc_to_phone(void)
{
    bts_t b;
    f12_setup(&b, BTS_X_LATE_STOP);
    b.pol.xwait = 45;
    for (int round = 0; round < 3; round++) {
        int ph, pc, before, st, pl;
        nsent = 0;
        memset(sent, 0, sizeof sent);
        echo_on = 1;
        ph = dev_of(&b, F9_PHONE);
        pc = dev_of(&b, F9_PC);
        CHECK(b.dev[pc].streaming && !b.dev[ph].streaming);
        before = nsent;
        CHECK_EQ(bts_use(&b, F9_PHONE, true), BTS_OK);
        run(&b, 400, true);
        CHECK(sent_index("AVRCP PAUSE", before) == before + 1 && !strcmp(sent[before], "SET 2 SELECT"));
        src_paused(&b, "2");
        run(&b, 3500, true);                         /* ~3.9 s after the PAUSE went out: Windows still holds it - nothing yet */
        CHECK_EQ(sent_index("A2DP STREAMING STOP", before), -1);
        CHECK(b.dev[pc].streaming && b.sw_phase == BTS_SW_PAUSE);
        run(&b, 1000, true);                         /* the grace (BTS_SW_LATE_STOP_MS, counted once the PAUSE is out) is over */
        st = sent_index("A2DP STREAMING STOP", before);
        CHECK(st > before && !strcmp(sent[st - 1], "SET 0 SELECT"));
        CHECK(b.dev[pc].streaming);                  /* the echo (no link id) is no STOP */
        CHECK_EQ(log_hits(&b, "late STOP"), round + 1);
        line(&b, "A2DP STREAMING STOP 0");           /* the module's own line (Windows answered in 0.1 s, field 11-02) */
        run(&b, 900, true);
        pl = sent_index("AVRCP PLAY", before);
        CHECK(pl > st && !strcmp(sent[pl - 1], "SET 5 SELECT"));
        CHECK_EQ(b.sw_phase, BTS_SW_PLAY);
        CHECK(b.sw_total < 6500u);                   /* field without the option: 14.2 s */
        line(&b, "AVRCP 5 REGISTER_NOTIFICATION_RSP CHANGED PLAYBACK_STATUS_CHANGED PLAYING");
        line(&b, "A2DP STREAMING START 3");
        run(&b, 400, true);
        CHECK_EQ(b.sw_result, BTS_RES_PLAYING);
        CHECK(b.dev[pc].susp && b.dev[pc].ps_ours && !b.dev[pc].streaming);
        line(&b, "AVRCP 5 REGISTER_NOTIFICATION_RSP INTERIM PLAYBACK_STATUS_CHANGED PLAYING");
        run(&b, 2000, true);
        /* back to the PC */
        before = nsent;
        CHECK_EQ(bts_use(&b, F9_PC, true), BTS_OK);
        run(&b, 400, true);
        CHECK(sent_index("AVRCP PAUSE", before) == before + 1 && !strcmp(sent[before], "SET 5 SELECT"));
        src_paused(&b, "5");
        run(&b, 2500, true);
        line(&b, "A2DP STREAMING STOP 3");           /* the phone's own STOP, 3.2 s after the PAUSE */
        run(&b, 900, true);
        CHECK_EQ(sent_index("A2DP STREAMING STOP", before), -1);   /* nothing to the phone - it let go in time */
        pl = sent_index("AVRCP PLAY", before);
        CHECK(pl > before && !strcmp(sent[pl - 1], "SET 2 SELECT"));   /* the PC the sink stopped: PLAY */
        if (round == 1) {
            run(&b, BTS_SW_READY_MS + 200, true);
            CHECK_EQ(b.sw_result, BTS_RES_READY);
            CHECK_EQ(b.sw_tried & (BTS_SWT_PLAY | BTS_SWT_START), BTS_SWT_PLAY);
        }
        line(&b, "AVRCP 2 REGISTER_NOTIFICATION_RSP CHANGED PLAYBACK_STATUS_CHANGED PLAYING");
        line(&b, "A2DP STREAMING START 0");
        run(&b, 400, true);
        CHECK_EQ(b.sw_result, BTS_RES_PLAYING);   /* round 1: the late START turned READY into PLAYING */
        CHECK_EQ(log_hits(&b, "late START: playing"), round >= 1 ? 1 : 0);
        CHECK(b.dev[pc].streaming && !b.dev[pc].susp && !b.dev[ph].streaming && !b.dev[ph].susp);
        CHECK_EQ(count_exact("A2DP STREAMING START"), 0);
        CHECK(only_runtime_sent());
        line(&b, "AVRCP 2 REGISTER_NOTIFICATION_RSP INTERIM PLAYBACK_STATUS_CHANGED PLAYING");
        run(&b, 2000, true);
        echo_on = 0;
    }
    CHECK_EQ(b.n.suspends, 3);
}

/* where the option does nothing: (a) off (the default) - the field's 14.2 s: the PC's own STOP at ~11.7 s; (b) the PC
 * still reports PLAYING (its PAUSE lost) - never stopped, BUSY at 30 s; (c) the PC without AVRCP (Windows service off,
 * field 11-10/11) - never stopped; (e) the guard never uses it ((d) strategy 「선택만」 - since removed) */
static void test_btsrc_late_stop_not(void)
{
    bts_t b;
    int pl;
    bts_policy_t p;
    bts_policy_default(&p);
    CHECK_EQ(p.xopt, BTS_LATE_STOP_DEFAULT ? BTS_X_LATE_STOP : 0);
    p.xopt = 0x02;
    CHECK_EQ(bts_policy_check(&p), -1);
    p.xopt = BTS_X_LATE_STOP;
    CHECK_EQ(bts_policy_check(&p), 0);
    /* (a) */
    f12_setup(&b, 0);
    echo_on = 1;
    CHECK_EQ(bts_use(&b, F9_PHONE, true), BTS_OK);
    run(&b, 400, true);
    src_paused(&b, "2");
    run(&b, 10900, true);                            /* 11.7 s after the pick */
    CHECK_EQ(count_exact("A2DP STREAMING STOP"), 0);
    CHECK_EQ(b.sw_phase, BTS_SW_PAUSE);
    line(&b, "A2DP STREAMING STOP 0");
    run(&b, 900, true);
    pl = sent_index("AVRCP PLAY", 0);
    CHECK(pl > 0 && !strcmp(sent[pl - 1], "SET 5 SELECT"));
    CHECK_EQ(log_hits(&b, "late STOP"), 0);
    echo_on = 0;
    /* (b) */
    f12_setup(&b, BTS_X_LATE_STOP);
    CHECK_EQ(bts_use(&b, F9_PHONE, true), BTS_OK);
    run(&b, 1600, true);                             /* no CHANGED: asked again after 1 s - the PC says PLAYING */
    line(&b, "AVRCP 2 REGISTER_NOTIFICATION_RSP INTERIM PLAYBACK_STATUS_CHANGED PLAYING");
    run(&b, BTS_SW_HELD_MS, true);
    CHECK_EQ(b.sw_result, BTS_RES_BUSY);
    CHECK_EQ(count_exact("A2DP STREAMING STOP"), 0);
    CHECK_EQ(sent_index("AVRCP PLAY", 0), -1);
    /* (c) */
    f12_setup(&b, BTS_X_LATE_STOP);
    b.pol.flags |= BTS_F_NO_AVRCP_CALL;
    line(&b, "NO CARRIER 2 ERROR 0");                /* its AVRCP gone (the Windows AVRCP service off) */
    run(&b, 500, true);
    nsent = 0;
    CHECK_EQ(bts_use(&b, F9_PHONE, true), BTS_OK);
    run(&b, 12000, true);
    CHECK_EQ(count_exact("A2DP STREAMING STOP"), 0);
    CHECK_EQ(b.sw_phase, BTS_SW_PAUSE);
    CHECK_EQ(log_hits(&b, "late STOP"), 0);
    /* (e) the phone (not selected) starts while the PC is selected and paused: PAUSE (the guard), never a sink STOP */
    f12_setup(&b, BTS_X_LATE_STOP);
    line(&b, "AVRCP 2 REGISTER_NOTIFICATION_RSP CHANGED PLAYBACK_STATUS_CHANGED PAUSED");
    line(&b, "A2DP STREAMING STOP 0");
    line(&b, "AVRCP 5 REGISTER_NOTIFICATION_RSP CHANGED PLAYBACK_STATUS_CHANGED PLAYING");
    line(&b, "A2DP STREAMING START 3");
    run(&b, 1000, true);
    CHECK(sent_index("AVRCP PAUSE", 0) >= 0);    /* refused: PAUSE */
    line(&b, "AVRCP 5 REGISTER_NOTIFICATION_RSP CHANGED PLAYBACK_STATUS_CHANGED PAUSED");
    run(&b, 8000, true);                         /* paused, and it keeps its stream: left alone (no switch) */
    CHECK_EQ(count_exact("A2DP STREAMING STOP"), 0);
}

/* the short wait (policy xwait, from the PAUSED report).  (a) PC -> phone, default wait (xwait 0 = 1.0 s):
 * PAUSE -> PAUSED 0.2 s later -> at 0.9 s after the report nothing, at 1.0 s `SET 0 SELECT` + `A2DP STREAMING STOP` ->
 * the PC's STOP 0 -> PLAY to the phone -> its START: under ~2 s plus the phone's own start.  (b) phone -> PC with 0.5 s:
 * the phone (Android) reports PAUSED and holds its stream ~3 s - the piano stops it 0.5 s after the report (intended,
 * field-tested 2026-11-13) -> its STOP 3 -> PLAY to the PC.  (c) a renewed INTERIM PAUSED does not restart the wait; a source
 * that reported PAUSED before the pick (no PAUSE: it is paused) and still holds is stopped once the wait from that
 * report is over.  (d) xwait range: 0..BTS_XWAIT_MAX, above refused. */
static void test_btsrc_late_stop_short_wait(void)
{
    bts_t b;
    bts_policy_t p;
    int ph, pc, st, pl, before;
    /* (a) */
    f12_setup(&b, BTS_X_LATE_STOP);
    echo_on = 1;
    CHECK_EQ(b.pol.xwait, BTS_XWAIT_DEFAULT);        /* the default (before: 0, the same 1.0 s) */
    b.pol.xwait = 10;                                /* 1.0 s (the timing below - independent of the default) */
    CHECK_EQ(bts_use(&b, F9_PHONE, true), BTS_OK);
    run(&b, 200, true);
    CHECK(sent_index("AVRCP PAUSE", 0) == 1 && !strcmp(sent[0], "SET 2 SELECT"));
    run(&b, 200, true);                              /* PAUSED 0.2 s after the PAUSE (field 0.1-0.3 s) */
    line(&b, "AVRCP 2 REGISTER_NOTIFICATION_RSP CHANGED PLAYBACK_STATUS_CHANGED PAUSED");
    run(&b, 300, true);
    line(&b, "AVRCP 2 REGISTER_NOTIFICATION_RSP INTERIM PLAYBACK_STATUS_CHANGED PAUSED");   /* no restart */
    run(&b, 500, true);                              /* 0.8 s after the report */
    CHECK_EQ(sent_index("A2DP STREAMING STOP", 0), -1);
    run(&b, 500, true);                              /* 1.3 s (the wait at 1.0 s, then SET + STOP go out 150 ms apart) */
    st = sent_index("A2DP STREAMING STOP", 0);
    CHECK(st > 0 && !strcmp(sent[st - 1], "SET 0 SELECT"));
    CHECK_EQ(log_hits(&b, "late STOP"), 1);
    line(&b, "A2DP STREAMING STOP 0");
    for (int k = 0; k < 40 && sent_index("AVRCP PLAY", 0) < 0; k++)
        run(&b, 50, true);                          /* until the PLAY is out */
    pl = sent_index("AVRCP PLAY", 0);
    CHECK(pl > st && !strcmp(sent[pl - 1], "SET 5 SELECT"));
    CHECK(b.sw_total < 2000u);                       /* pick -> PLAY out; field with 4.5 s from the PAUSE: 6.2-6.9 s to sound */
    line(&b, "AVRCP 5 REGISTER_NOTIFICATION_RSP CHANGED PLAYBACK_STATUS_CHANGED PLAYING");
    line(&b, "A2DP STREAMING START 3");
    run(&b, 400, true);
    CHECK_EQ(b.sw_result, BTS_RES_PLAYING);
    line(&b, "AVRCP 5 REGISTER_NOTIFICATION_RSP INTERIM PLAYBACK_STATUS_CHANGED PLAYING");
    run(&b, 2000, true);
    /* (b) back to the PC with 0.5 s: the phone is stopped by the piano */
    b.pol.xwait = 5;
    before = nsent;
    ph = dev_of(&b, F9_PHONE);
    pc = dev_of(&b, F9_PC);
    CHECK_EQ(bts_use(&b, F9_PC, true), BTS_OK);
    run(&b, 300, true);
    CHECK(sent_index("AVRCP PAUSE", before) == before + 1 && !strcmp(sent[before], "SET 5 SELECT"));
    line(&b, "AVRCP 5 REGISTER_NOTIFICATION_RSP CHANGED PLAYBACK_STATUS_CHANGED PAUSED");
    run(&b, 400, true);
    CHECK_EQ(sent_index("A2DP STREAMING STOP", before), -1);
    run(&b, 400, true);                              /* 0.8 s after the report: the phone still holds (Android ~3 s) */
    st = sent_index("A2DP STREAMING STOP", before);
    CHECK(st > before && !strcmp(sent[st - 1], "SET 3 SELECT"));
    CHECK(b.dev[ph].susp);
    line(&b, "A2DP STREAMING STOP 3");
    for (int k = 0; k < 40 && sent_index("AVRCP PLAY", before) < 0; k++)
        run(&b, 50, true);                          /* until the PLAY is out */
    pl = sent_index("AVRCP PLAY", before);
    CHECK(pl > st && !strcmp(sent[pl - 1], "SET 2 SELECT"));
    CHECK(b.sw_total < 1800u);                       /* pick -> PLAY out; field (the phone let go by itself): 3.4-3.8 s */
    line(&b, "AVRCP 2 REGISTER_NOTIFICATION_RSP CHANGED PLAYBACK_STATUS_CHANGED PLAYING");
    line(&b, "A2DP STREAMING START 0");
    run(&b, 400, true);
    CHECK_EQ(b.sw_result, BTS_RES_PLAYING);
    CHECK(b.dev[pc].streaming && !b.dev[ph].streaming);
    CHECK_EQ(count_exact("A2DP STREAMING START"), 0);
    CHECK(only_runtime_sent());
    line(&b, "AVRCP 2 REGISTER_NOTIFICATION_RSP INTERIM PLAYBACK_STATUS_CHANGED PLAYING");
    run(&b, 2000, true);
    /* (b2) and to the phone again: the phone the piano stopped gets PLAY, no sink START */
    before = nsent;
    CHECK_EQ(bts_use(&b, F9_PHONE, true), BTS_OK);
    run(&b, 300, true);
    line(&b, "AVRCP 2 REGISTER_NOTIFICATION_RSP CHANGED PLAYBACK_STATUS_CHANGED PAUSED");
    run(&b, 700, true);
    st = sent_index("A2DP STREAMING STOP", before);
    CHECK(st > before && !strcmp(sent[st - 1], "SET 0 SELECT"));
    line(&b, "A2DP STREAMING STOP 0");
    run(&b, 900, true);
    pl = sent_index("AVRCP PLAY", before);
    CHECK(pl > st && !strcmp(sent[pl - 1], "SET 5 SELECT"));
    line(&b, "A2DP STREAMING START 3");
    run(&b, 400, true);
    CHECK_EQ(b.sw_result, BTS_RES_PLAYING);
    CHECK_EQ(count_exact("A2DP STREAMING START"), 0);
    CHECK_EQ(b.n.suspends, 3);
    echo_on = 0;
    /* (c) the PC paused by the user 2 s before the pick, still holding: no PAUSE, stopped at once (wait over) */
    f12_setup(&b, BTS_X_LATE_STOP);
    line(&b, "AVRCP 2 REGISTER_NOTIFICATION_RSP CHANGED PLAYBACK_STATUS_CHANGED PAUSED");
    run(&b, 2000, true);
    nsent = 0;
    memset(sent, 0, sizeof sent);
    CHECK_EQ(bts_use(&b, F9_PHONE, true), BTS_OK);
    run(&b, 400, true);
    CHECK_EQ(sent_index("AVRCP PAUSE", 0), -1);
    st = sent_index("A2DP STREAMING STOP", 0);
    CHECK(st > 0 && !strcmp(sent[st - 1], "SET 0 SELECT"));
    /* (c2) no PAUSED report ever (its status says PLAYING): never stopped, BUSY at 30 s, even with the shortest wait */
    f12_setup(&b, BTS_X_LATE_STOP);
    b.pol.xwait = 1;
    CHECK_EQ(bts_use(&b, F9_PHONE, true), BTS_OK);
    run(&b, BTS_SW_HELD_MS + 2000, true);
    CHECK_EQ(b.sw_result, BTS_RES_BUSY);
    CHECK_EQ(count_exact("A2DP STREAMING STOP"), 0);
    /* (d) */
    bts_policy_default(&p);
    CHECK_EQ(p.xwait, BTS_XWAIT_DEFAULT);
    CHECK_EQ(BTS_SW_LATE_STOP_MS, BTS_XWAIT_DEFAULT * 100u);   /* xwait 0 (an app's write) = the default wait */
    p.xwait = 0;
    CHECK_EQ(bts_policy_check(&p), 0);
    p.xwait = BTS_XWAIT_MAX;
    CHECK_EQ(bts_policy_check(&p), 0);
    p.xwait = BTS_XWAIT_MAX + 1;
    CHECK_EQ(bts_policy_check(&p), -1);
}

/* AVRCP PLAY to the target, its START 1.4 s later (the old 1.2 s limit said READY - the app's
 * 「시작하지 못했습니다」 though it played): PLAYING now.  With no START at all READY comes at BTS_SW_READY_MS, and a
 * START within BTS_SW_LATE_MS after that READY turns it into PLAYING (the app clears its warning); a START of another
 * device does not, nor one after BTS_SW_LATE_MS. */
static void test_btsrc_late_start_result(void)
{
    bts_t b;
    int pl;
    /* the phone paused by the piano, the PC selected and playing -> pick the phone */
    for (int v = 0; v < 3; v++) {
        f12_setup(&b, BTS_X_LATE_STOP);
        CHECK_EQ(bts_use(&b, F9_PHONE, true), BTS_OK);
        run(&b, 400, true);
        line(&b, "AVRCP 2 REGISTER_NOTIFICATION_RSP CHANGED PLAYBACK_STATUS_CHANGED PAUSED");
        run(&b, 1100, true);
        line(&b, "A2DP STREAMING STOP 0");
        run(&b, 900, true);
        pl = sent_index("AVRCP PLAY", 0);
        CHECK(pl > 0 && !strcmp(sent[pl - 1], "SET 5 SELECT"));
        CHECK_EQ(b.sw_phase, BTS_SW_PLAY);
        if (v == 0) {
            run(&b, 1400, true);                     /* the field's 1.4 s */
            CHECK_EQ(b.sw_phase, BTS_SW_PLAY);       /* not READY yet */
            line(&b, "A2DP STREAMING START 3");
            run(&b, 400, true);
            CHECK_EQ(b.sw_result, BTS_RES_PLAYING);
            CHECK_EQ(log_hits(&b, "late START: playing"), 0);
        } else if (v == 1) {
            run(&b, BTS_SW_READY_MS + 200, true);
            CHECK_EQ(b.sw_result, BTS_RES_READY);
            run(&b, 1500, true);
            line(&b, "A2DP STREAMING START 3");      /* the phone played after all */
            run(&b, 100, true);
            CHECK_EQ(b.sw_result, BTS_RES_PLAYING);
            CHECK_EQ(log_hits(&b, "late START: playing"), 1);
            CHECK(b.sw_last_ms >= BTS_SW_READY_MS + 1500u);   /* the reported time is when it started */
            CHECK_EQ(count_exact("A2DP STREAMING START"), 0);
        } else {
            run(&b, BTS_SW_READY_MS + 200, true);
            CHECK_EQ(b.sw_result, BTS_RES_READY);
            line(&b, "A2DP STREAMING START 0");      /* the PC (not the target) starts: stays READY */
            run(&b, 100, true);
            CHECK_EQ(b.sw_result, BTS_RES_READY);
            line(&b, "A2DP STREAMING STOP 0");
            run(&b, BTS_SW_LATE_MS + 200, true);
            line(&b, "A2DP STREAMING START 3");      /* too late to be this switch's answer */
            run(&b, 100, true);
            CHECK_EQ(b.sw_result, BTS_RES_READY);
        }
    }
}

/* ---- live apply handover ---------------------------------------------------------------- */
/* the field case of 2026-11-11: the PC (A2DP 0 / 1, AVRCP 2) and the phone (A2DP 3 / 4, AVRCP 5) connected, the phone
 * selected and streaming; recon off (the user's setting), AUTOCALL off */
static void ho_scene(bts_t *b)
{
    fresh(b);
    b->susp_ok = 0;
    b->pol.recon = BTS_RC_OFF;
    b->ac_state = BTS_AC_OFF;
    line(b, "RING 0 22:82:02:02:92:22 19 A2DP");
    line(b, "RING 1 22:82:02:02:92:22 19 A2DP");
    line(b, "RING 2 22:82:02:02:92:22 17 AVRCP");
    line(b, "RING 3 62:81:81:81:11:a1 19 A2DP");
    line(b, "RING 4 62:81:81:81:11:a1 19 A2DP");
    line(b, "RING 5 62:81:81:81:11:a1 17 AVRCP");
    line(b, "A2DP STREAMING START 3");
    run(b, 200, true);
    b->has_sel = 1;
    memcpy(b->sel_addr, PHONE, 6);
}

static const char *const HO_LIST[] = {
    "LIST 6",
    "LIST 0 CONNECTED A2DP 1024 0 0 24 0 0 22:82:02:02:92:22 19 INCOMING ACTIVE SLAVE ENCRYPTED 0",
    "LIST 1 CONNECTED A2DP 1024 0 0 24 0 0 22:82:02:02:92:22 19 INCOMING ACTIVE SLAVE ENCRYPTED 0",
    "LIST 2 CONNECTED AVRCP 512 0 0 21 0 0 22:82:02:02:92:22 17 INCOMING ACTIVE SLAVE ENCRYPTED 0",
    "LIST 3 CONNECTED A2DP 1024 0 0 24 0 0 62:81:81:81:11:a1 19 INCOMING ACTIVE SLAVE ENCRYPTED 0",
    "LIST 4 CONNECTED A2DP 1024 0 0 24 0 0 62:81:81:81:11:a1 19 INCOMING ACTIVE SLAVE ENCRYPTED 0",
    "LIST 5 CONNECTED AVRCP 512 0 0 21 0 0 62:81:81:81:11:a1 17 INCOMING ACTIVE SLAVE ENCRYPTED 0",
};

/* the new payload: bts_init + the block (as bt_cave.c does on its first tick), module on */
static int ho_attach(bts_t *b, const uint8_t *raw, uint32_t now_us)
{
    int k;
    nsent = 0;
    nreads = 0;
    memset(sent, 0, sizeof sent);
    bts_init(b, 0);
    b->pin = BTS_PIN_DONE;   /* tests written before the power-on pin: no pin (its own tests) */
    b->pol.recon = BTS_RC_OFF;   /* kv_tick reads the user's setting back from NOR */
    k = bts_handover_load(b, raw, BTS_HO_RAW, now_us);
    bts_module(b, true);
    return k;
}

static void test_btsrc_handover_block(void)
{
    static bts_t b;
    uint8_t raw[BTS_HO_RAW], bad[BTS_HO_RAW];
    ho_scene(&b);
    CHECK_EQ(bts_handover_save(&b, 1000000u, raw), BTS_HO_RAW);
    CHECK(!memcmp(raw, "C79B", 4) && raw[4] == BTS_HO_VER && raw[5] == (BTS_HO_F_SEL | BTS_HO_F_STREAM) && raw[6] == 2);
    CHECK(!memcmp(raw + 8, PHONE, 6));
    CHECK((!memcmp(raw + 16, PC, 6) && !memcmp(raw + 22, PHONE, 6)) || (!memcmp(raw + 16, PHONE, 6) && !memcmp(raw + 22, PC, 6)));
    /* a live apply (~0.1 s later): taken, the selected one first, the selection back at once */
    CHECK_EQ(ho_attach(&b, raw, 1100000u), 2);
    CHECK(b.has_sel && !memcmp(b.sel_addr, PHONE, 6) && b.ho.armed && !memcmp(b.ho.addr[0], PHONE, 6) &&
          !memcmp(b.ho.addr[1], PC, 6));
    CHECK_EQ(log_hits(&b, "live apply: handover"), 1);
    /* a cold boot: nothing (stale stamp - also across the u32 wrap - / CRC / magic / version / count / all zero) */
    CHECK_EQ(ho_attach(&b, raw, 1000000u + BTS_HO_FRESH_US), -1);
    CHECK(!b.has_sel && !b.ho.armed && !b.list_want);
    CHECK_EQ(ho_attach(&b, raw, 999999u), -1);                  /* written "after" now: an older power-on */
    CHECK_EQ(ho_attach(&b, raw, 3000000u), 2);
    for (int i = 0; i < 52; i += 13) {
        memcpy(bad, raw, sizeof bad);
        bad[i] ^= 0x01;
        CHECK_EQ(ho_attach(&b, bad, 1100000u), -1);
        CHECK(!b.has_sel && !b.ho.armed);
    }
    memset(bad, 0, sizeof bad);
    CHECK_EQ(ho_attach(&b, bad, 1100000u), -1);
    CHECK_EQ(ho_attach(&b, raw, 1100000u), 2);
    CHECK_EQ(bts_handover_load(&b, raw, BTS_HO_RAW - 1, 1100000u), -1);
    /* nothing selected, nothing connected: an empty block is still a live apply - no device, no CALL */
    fresh(&b);
    bts_handover_save(&b, 7u, raw);
    CHECK(raw[5] == 0 && raw[6] == 0);
    CHECK_EQ(ho_attach(&b, raw, 8u), 0);
    line(&b, "LIST 0");
    run(&b, 20000, true);
    CHECK_EQ(calls_of("19 A2DP"), 0);
    CHECK(!b.ho.armed && !b.has_sel);
}

/* the links stayed (the module keeps them - what the emulator shows of the apply): nothing sent but LIST and the
 * play-status registrations, the selected phone streams and plays on, a switch to the PC needs no CALL */
static void test_btsrc_handover_kept(void)
{
    static bts_t b;
    uint8_t raw[BTS_HO_RAW];
    ho_scene(&b);
    bts_handover_save(&b, 50000u, raw);
    CHECK_EQ(ho_attach(&b, raw, 60000u), 2);
    run(&b, 400, true);
    CHECK_EQ(nsent, 1);
    CHECK(!strcmp(sent[0], "LIST"));
    for (unsigned i = 0; i < sizeof HO_LIST / sizeof HO_LIST[0]; i++)
        line(&b, HO_LIST[i]);
    run(&b, 20000, true);
    int ph = dev_of(&b, PHONE), pc = dev_of(&b, PC);
    CHECK(ph >= 0 && pc >= 0 && b.dev[ph].a2dp == 0x18 && b.dev[pc].a2dp == 0x03);
    CHECK_EQ(bts_sel_dev(&b), ph);
    CHECK(b.dev[ph].streaming && !b.dev[pc].streaming && b.active == ph);
    CHECK_EQ(calls_of("19 A2DP"), 0);
    CHECK_EQ(count_sent("CLOSE"), 0);
    CHECK_EQ(count_sent("A2DP STREAMING"), 0);
    CHECK_EQ(count_sent("AVRCP PAUSE") + count_sent("AVRCP PLAY"), 0);   /* the phone plays on, the PC is left alone */
    CHECK(only_runtime_sent());
    CHECK_EQ(log_hits(&b, "handover: kept"), 2);
    CHECK(!b.ho.armed);
    /* a stream line before the LIST reply: the live state wins (the phone stopped meanwhile) */
    ho_scene(&b);
    bts_handover_save(&b, 50000u, raw);
    ho_attach(&b, raw, 60000u);
    run(&b, 400, true);
    line(&b, "A2DP STREAMING STOP 3");
    for (unsigned i = 0; i < sizeof HO_LIST / sizeof HO_LIST[0]; i++)
        line(&b, HO_LIST[i]);
    run(&b, 2000, true);
    ph = dev_of(&b, PHONE);
    CHECK(ph >= 0 && !b.dev[ph].streaming && bts_sel_dev(&b) == ph);
    /* the old payload's own path: without a block (a cold boot) the same LIST leaves nothing selected */
    ho_scene(&b);
    {
        uint8_t none[BTS_HO_RAW] = {0};
        ho_attach(&b, none, 60000u);
    }
    for (unsigned i = 0; i < sizeof HO_LIST / sizeof HO_LIST[0]; i++)
        line(&b, HO_LIST[i]);
    run(&b, 2000, true);
    CHECK(bts_sel_dev(&b) < 0 && !b.ho.armed);
}

/* the links dropped over the apply (the field 2026-11-11 symptom): LIST shows none -> the selected phone is called back
 * first, then the PC, with the reconnect setting off; each comes back -> both connected, the phone still selected */
static void test_btsrc_handover_dropped(void)
{
    static bts_t b;
    uint8_t raw[BTS_HO_RAW];
    ho_scene(&b);
    bts_handover_save(&b, 50000u, raw);
    CHECK_EQ(ho_attach(&b, raw, 60000u), 2);
    run(&b, 400, true);
    line(&b, "LIST 0");
    run(&b, 200, true);
    CHECK_EQ(calls_of("19 A2DP"), 1);
    CHECK(!strcmp(sent[nsent - 1], "CALL 62:81:81:81:11:a1 19 A2DP"));
    line(&b, "CALL 0");
    line(&b, "CONNECT 0 A2DP 19");
    line(&b, "CONNECT 1 A2DP 19");
    run(&b, 300, true);
    line(&b, "RING 2 62:81:81:81:11:a1 17 AVRCP");
    run(&b, 500, true);
    CHECK_EQ(count_sent("CALL 22:82:02:02:92:22 19 A2DP"), 1);    /* the PC next, once the phone's call is over */
    line(&b, "CALL 3");
    line(&b, "CONNECT 3 A2DP 19");
    line(&b, "CONNECT 4 A2DP 19");
    run(&b, 300, true);
    line(&b, "RING 5 22:82:02:02:92:22 17 AVRCP");
    run(&b, 30000, true);
    int ph = dev_of(&b, PHONE), pc = dev_of(&b, PC);
    CHECK(ph >= 0 && pc >= 0 && b.dev[ph].a2dp && b.dev[pc].a2dp);
    CHECK_EQ(bts_sel_dev(&b), ph);
    CHECK_EQ(calls_of("19 A2DP"), 2);
    CHECK_EQ(log_hits(&b, "handover: back"), 2);
    CHECK_EQ(count_sent("CLOSE"), 0);
    CHECK(only_runtime_sent());
    CHECK(!b.ho.armed);
    /* nobody answers: BTS_HO_TRIES CALLs per device, BTS_HO_GAP_MS apart, the phone's first; then nothing more ever */
    ho_scene(&b);
    bts_handover_save(&b, 50000u, raw);
    ho_attach(&b, raw, 60000u);
    run(&b, 400, true);
    line(&b, "LIST 0");
    run(&b, 4000, true);
    CHECK_EQ(count_sent("CALL 62:81:81:81:11:a1 19 A2DP"), 1);
    run(&b, 2000, true);
    CHECK_EQ(count_sent("CALL 62:81:81:81:11:a1 19 A2DP"), 1);    /* < BTS_HO_GAP_MS + the call's own 10 s */
    run(&b, 60000, true);
    CHECK_EQ(count_sent("CALL 62:81:81:81:11:a1 19 A2DP"), (int)BTS_HO_TRIES);
    CHECK(count_sent("CALL 22:82:02:02:92:22 19 A2DP") >= 1);
    CHECK(!b.ho.armed);
    int total = calls_of("19 A2DP");
    run(&b, 120000, true);
    CHECK_EQ(calls_of("19 A2DP"), total);
    CHECK_EQ(calls_of("19 A2DP"), total);
    CHECK(total <= 2 * (int)BTS_HO_TRIES);
    /* the module's own AUTOCALL is on: the module calls, the piano does not */
    ho_scene(&b);
    bts_handover_save(&b, 50000u, raw);
    ho_attach(&b, raw, 60000u);
    b.ac_state = BTS_AC_ON;
    run(&b, 400, true);
    line(&b, "LIST 0");
    run(&b, 20000, true);
    CHECK_EQ(calls_of("19 A2DP"), 0);
    /* standby right after the apply: the module goes off, the handover with it */
    ho_scene(&b);
    bts_handover_save(&b, 50000u, raw);
    ho_attach(&b, raw, 60000u);
    bts_module(&b, false);
    run(&b, 100, false);
    bts_module(&b, true);
    line(&b, "LIST 0");
    run(&b, 20000, true);
    CHECK_EQ(calls_of("19 A2DP"), 0);
}

/* a cold boot with the reconnect setting off never calls; with "preferred · last" only its own target (unchanged) */
static void test_btsrc_handover_cold_boot(void)
{
    static bts_t b;
    uint8_t zero[BTS_HO_RAW] = {0};
    CHECK_EQ(ho_attach(&b, zero, 1000u), -1);
    b.ac_state = BTS_AC_OFF;
    run(&b, 400, true);
    line(&b, "LIST 0");
    run(&b, 60000, true);
    CHECK_EQ(calls_of("19 A2DP"), 0);
    CHECK(!b.has_sel && !b.ho.armed);
    CHECK_EQ(log_hits(&b, "handover"), 0);
}

/* ---- the decided module values kept at power-on ----------------------------------------- */
static int pin_restarts;

/* the model + the port's part of a restart: bts_take_recover -> the stock off (every link gone), 0.6 s, on
 * (bt_cave.c src_restart_begin / src_tick) */
static void pin_run(uint32_t ms)
{
    for (uint32_t t = 0; t < ms; t += 8) {
        m_run(8, true);
        if (bts_take_recover(M.b)) {
            pin_restarts++;
            bts_module(M.b, false);
            for (int i = 0; i < M_N; i++) {
                M.d[i].a2dp = M.d[i].avrcp = M.d[i].browse = -1;
                M.d[i].streaming = M.d[i].ps_reg = 0;
            }
            if (M.revert) {
                M.cod = 0x001f00;
                M.avf = 0x1f;
                M.aud = BTS_AUD_FACTORY;
            }
            m_run(600, false);
            bts_module(M.b, true);
        }
    }
}

/* the model with the pin on (m_init turns it off for the tests written before it); the module's values given */
static void pin_init(bts_t *b, unsigned cod, unsigned avf, unsigned aud)
{
    m_init(b);
    b->pin = BTS_PIN_WAIT;
    M.cod = cod;
    M.avf = avf;
    M.aud = aud;
    pin_restarts = 0;
}

/* all three already decided (this piano's module since 11-11): one `SET` read, nothing written, no restart */
static void test_btsrc_pin_decided(void)
{
    bts_t b;
    pin_init(&b, BTS_PIN_COD, BTS_PIN_AVF, BTS_PIN_AUD);
    pin_run(2900);
    CHECK_EQ(nreads, 0);
    CHECK_EQ(b.pin, BTS_PIN_WAIT);
    pin_run(600);                                  /* BTS_SET_READ_MS after module on: one `SET`, read only */
    CHECK_EQ(nreads, 1);
    CHECK_EQ(b.pin, BTS_PIN_DONE);
    CHECK_EQ(log_hits(&b, "pin: kept"), 1);
    pin_run(30000);
    CHECK_EQ(nreads, 1);
    CHECK_EQ(ac_writes_sent(), 0);                 /* no write, no restart */
    CHECK_EQ(pin_restarts, 0);
    CHECK_EQ(b.pin_w, 0);
    CHECK_EQ(bts_health(&b) & BTS_MH_PIN, 0);
    CHECK_EQ(b.cod_op | b.avf_op | b.aud_op, 0);
}

/* the lines of a `SET` listing from the UART are parsed as
 * before but kept out of the line log - one summary event; the echo and the other lines are logged as before; a listing
 * without its closing `SET` is summed up when it times out */
static void test_btsrc_set_list_log(void)
{
    bts_t b;
    static const char L[] = "SET\r\nSET BT BDADDR 02:07:80:aa:bb:cc\r\nSET BT NAME CA79 Audio\r\nSET BT CLASS 240414\r\n"
                            "SET BT PAIR 22:82:02:02:92:22 0123456789abcdef0123456789abcdef\r\nSET\r\nRING 0 22:82:02:02:92:22 19 A2DP\r\n";
    m_init(&b);
    bts_rx(&b, (const uint8_t *)L, (uint32_t)strlen(L));
    CHECK_EQ(b.set_cod, 0x240414u);                /* parsed */
    CHECK_EQ(b.set_done, 1);
    CHECK_EQ(log_hits(&b, "SET BT"), 0);           /* not logged: no BDADDR / NAME / CLASS / PAIR line */
    CHECK_EQ(log_hits(&b, "SET list: 5 lines"), 1);
    CHECK_EQ(log_hits(&b, "RING 0 22:82:02:02:92:22 19 A2DP"), 1);
    CHECK_EQ(log_hits(&b, "SET"), 2);             /* the echo + the summary */
    {
        static const char T[] = "SET BT BDADDR 02:07:80:aa:bb:cc\r\nSET BT NAME X\r\n";
        bts_rx(&b, (const uint8_t *)T, (uint32_t)strlen(T));
    }
    run(&b, BTS_SET_LIST_MS + 100, false);
    CHECK_EQ(log_hits(&b, "SET list: 2 lines (no end)"), 1);
    CHECK_EQ(log_hits(&b, "SET BT"), 0);
}

/* the class only differs: one `SET BT CLASS 240414` + its read back, no restart */
static void test_btsrc_pin_class_only(void)
{
    bts_t b;
    pin_init(&b, 0x001f00, BTS_PIN_AVF, BTS_PIN_AUD);
    pin_run(5000);
    CHECK_EQ(ac_writes_sent(), 1);
    CHECK(!strcmp(sent[0], "SET BT CLASS 240414"));
    CHECK_EQ(nreads, 2);                           /* the power-on read + the op's read back */
    CHECK_EQ(M.cod, 0x240414u);
    CHECK_EQ(b.cod, 0x240414u);
    CHECK_EQ(b.cod_rc, BTS_OK);
    CHECK_EQ(bts_class_restore(&b), 0x001f00u);    /* op 1D 0A restore still knows the module's own value */
    CHECK_EQ(b.pin, BTS_PIN_DONE);
    CHECK_EQ(b.pin_w, BTS_PB_COD);
    CHECK_EQ(b.pin_bad, 0);
    CHECK_EQ(log_hits(&b, "pin: written"), 1);
    pin_run(30000);
    CHECK_EQ(pin_restarts, 0);                     /* the class needs no restart (field 11-09) */
    CHECK_EQ(ac_writes_sent(), 1);
}

/* the factory listing: three writes (class, AVRCP, audio - each its op's line + read back), exactly one restart, the
 * listing read back after it; the ops still restore for diagnostics and the pin does not undo that this power-on */
static void test_btsrc_pin_factory(void)
{
    bts_t b;
    pin_init(&b, 0x001f00, 0x1f, BTS_AUD_FACTORY);
    pin_run(6000);
    CHECK_EQ(ac_writes_sent(), 3);
    CHECK(!strcmp(sent[0], "SET BT CLASS 240414"));
    CHECK(!strcmp(sent[1], "SET PROFILE AVRCP CONTROLLER 0f"));
    CHECK(!strcmp(sent[2], "SET CONTROL AUDIO INTERNAL I2S_SLAVE EVENT"));
    CHECK_EQ(pin_restarts, 1);
    CHECK_EQ(M.cod, 0x240414u);
    CHECK_EQ(M.avf, 0x0fu);
    CHECK_EQ(M.aud, BTS_AUD_EVENT);
    pin_run(5000);                                 /* back: the listing read once more */
    CHECK_EQ(nreads, 5);
    CHECK_EQ(b.pin, BTS_PIN_DONE);
    CHECK_EQ(b.pin_bad, 0);
    CHECK_EQ(b.pin_w, BTS_PB_COD | BTS_PB_AVF | BTS_PB_AUD);
    CHECK_EQ(log_hits(&b, "pin: done"), 1);
    CHECK_EQ(bts_avf_restore(&b), 0x1fu);           /* the ops' restore values: the module's own lines */
    CHECK_EQ(bts_aud_restore(&b), BTS_AUD_FACTORY);
    pin_run(60000);
    CHECK_EQ(pin_restarts, 1);                     /* exactly one */
    CHECK_EQ(ac_writes_sent(), 3);
    CHECK_EQ(nreads, 5);
    CHECK_EQ(bts_health(&b) & BTS_MH_PIN, 0);
    nsent = 0;
    CHECK_EQ(bts_avrcp_features(&b, BTS_AV_RESTORE, 0), BTS_OK);
    pin_run(600);
    CHECK_EQ(ac_writes_sent(), 1);
    CHECK(!strcmp(sent[0], "SET PROFILE AVRCP CONTROLLER 1f"));
    CHECK_EQ(b.avf_rc, BTS_OK);
    pin_run(30000);
    CHECK_EQ(ac_writes_sent(), 1);
    CHECK_EQ(pin_restarts, 1);
}

/* the audio line only differs: its write + one restart + the read back */
static void test_btsrc_pin_audio_only(void)
{
    bts_t b;
    pin_init(&b, BTS_PIN_COD, BTS_PIN_AVF, BTS_AUD_FACTORY);
    pin_run(12000);
    CHECK_EQ(ac_writes_sent(), 1);
    CHECK(!strcmp(sent[0], "SET CONTROL AUDIO INTERNAL I2S_SLAVE EVENT"));
    CHECK_EQ(pin_restarts, 1);
    CHECK_EQ(nreads, 3);
    CHECK_EQ(b.pin, BTS_PIN_DONE);
    CHECK_EQ(b.pin_bad, 0);
}

/* the module refuses: no second try in that power-on (rc BTS_STATE, health BTS_MH_PIN) */
static void test_btsrc_pin_refused(void)
{
    bts_t b;
    /* no write taken: each op's read back says so, no restart */
    pin_init(&b, 0x001f00, 0x1f, BTS_AUD_FACTORY);
    M.refuse = 1;
    pin_run(6000);
    CHECK_EQ(ac_writes_sent(), 3);
    CHECK_EQ(pin_restarts, 0);
    CHECK_EQ(b.pin, BTS_PIN_DONE);
    CHECK_EQ(b.pin_bad, BTS_PB_COD | BTS_PB_AVF | BTS_PB_AUD);
    CHECK_EQ(b.cod_rc, BTS_STATE);
    CHECK_EQ(b.avf_rc, BTS_STATE);
    CHECK_EQ(b.aud_rc, BTS_STATE);
    CHECK(bts_health(&b) & BTS_MH_PIN);
    CHECK_EQ(log_hits(&b, "pin: not taken"), 1);
    CHECK_EQ(bts_class(&b, BTS_CO_READ, 0), BTS_OK);   /* another listing (the app's read): no second try */
    pin_run(60000);
    CHECK_EQ(ac_writes_sent(), 3);
    CHECK_EQ(pin_restarts, 0);
    /* taken, but the factory values listed again after the restart: given up, one restart only */
    pin_init(&b, BTS_PIN_COD, 0x1f, BTS_AUD_FACTORY);
    M.revert = 1;
    pin_run(12000);
    CHECK_EQ(ac_writes_sent(), 2);
    CHECK_EQ(pin_restarts, 1);
    CHECK_EQ(b.pin, BTS_PIN_DONE);
    CHECK_EQ(b.pin_bad, BTS_PB_AVF | BTS_PB_AUD);   /* the class was not written - not checked */
    CHECK_EQ(b.avf_rc, BTS_STATE);
    CHECK_EQ(b.aud_rc, BTS_STATE);
    CHECK(bts_health(&b) & BTS_MH_PIN);
    pin_run(120000);
    CHECK_EQ(ac_writes_sent(), 2);
    CHECK_EQ(pin_restarts, 1);
    /* the port did not restart it (busy / Bluetooth Audio off): given up after BTS_PIN_RESTART_MS */
    pin_init(&b, BTS_PIN_COD, 0x1f, BTS_PIN_AUD);
    for (int t = 0; t < 8000; t += 8) {
        m_run(8, true);
        (void)bts_take_recover(&b);                /* taken, nothing done */
    }
    CHECK_EQ(b.pin, BTS_PIN_DONE);
    CHECK_EQ(b.pin_bad, BTS_PB_AVF);
    CHECK_EQ(log_hits(&b, "pin: no restart"), 1);
    pin_run(60000);
    CHECK_EQ(pin_restarts, 0);
    CHECK_EQ(ac_writes_sent(), 1);
}

/* not during a switch, not in standby / with the module off, not during the reboot-loop guard, a line not in the plain
 * form never written, a live apply writes but never restarts */
static void test_btsrc_pin_guards(void)
{
    bts_t b;
    uint8_t tv[6];
    /* the user picked the TV and it does not answer the CALL: nothing until the switch ends */
    pin_init(&b, 0x001f00, 0x1f, BTS_AUD_FACTORY);
    m_addr(M_TV, tv);
    b.pol.has_pref = 1;
    memcpy(b.pol.pref, tv, 6);
    M.d[M_TV].mac = "12:84:04:04:14:25";           /* the model does not know it: the CALL fails 5 s later */
    pin_run(2500);
    CHECK_EQ(bts_use(&b, tv, true), BTS_OK);
    pin_run(3000);
    CHECK(b.sw_phase != BTS_SW_IDLE);
    CHECK_EQ(nreads, 1);                           /* the listing came */
    CHECK_EQ(ac_writes_sent(), 0);
    CHECK_EQ(b.pin, BTS_PIN_WAIT);
    while (b.sw_phase != BTS_SW_IDLE && M.now < 30000)
        pin_run(8);
    CHECK_EQ(ac_writes_sent(), 0);
    pin_run(8000);
    CHECK_EQ(ac_writes_sent(), 3);
    CHECK_EQ(pin_restarts, 1);
    /* standby: nothing; the module off (no listing): nothing */
    pin_init(&b, 0x001f00, 0x1f, BTS_AUD_FACTORY);
    m_run(20000, false);
    CHECK_EQ(nreads + nsent, 0);
    bts_module(&b, false);
    pin_run(20000);
    CHECK_EQ(nreads + nsent, 0);
    CHECK_EQ(b.pin, BTS_PIN_WAIT);
    /* the reboot-loop guard: no write while it holds */
    pin_init(&b, 0x001f00, 0x1f, BTS_AUD_FACTORY);
    b.loop = 1;
    b.reboot_seen = 1;
    pin_run(6000);
    CHECK_EQ(nreads, 1);
    CHECK_EQ(ac_writes_sent(), 0);
    CHECK_EQ(b.pin, BTS_PIN_WAIT);
    /* no AVRCP / audio line in the plain form: those never written */
    pin_init(&b, 0x001f00, 0x1f, BTS_AUD_FACTORY);
    M.avf_form = 1;                                /* `… TARGET 3` */
    M.aud_form = 1;                                /* `… I2S_SLAVE EVENT 32` */
    pin_run(8000);
    CHECK_EQ(ac_writes_sent(), 1);                            /* the class only */
    CHECK_EQ(pin_restarts, 0);
    CHECK_EQ(b.pin_bad, 0);
    /* a live apply (handover block taken): written, the module never restarted */
    pin_init(&b, BTS_PIN_COD, BTS_PIN_AVF, BTS_AUD_FACTORY);
    b.live = 1;
    pin_run(8000);
    CHECK_EQ(ac_writes_sent(), 1);
    CHECK_EQ(pin_restarts, 0);
    CHECK_EQ(b.pin, BTS_PIN_DONE);
    CHECK_EQ(b.pin_bad, 0);
    CHECK_EQ(log_hits(&b, "pin: restart at power-on"), 1);
}

void suite_btsrc(void)
{
    RUN(test_btsrc_pin_decided);
    RUN(test_btsrc_set_list_log);
    RUN(test_btsrc_pin_class_only);
    RUN(test_btsrc_pin_factory);
    RUN(test_btsrc_pin_audio_only);
    RUN(test_btsrc_pin_refused);
    RUN(test_btsrc_pin_guards);
    RUN(test_btsrc_handover_block);
    RUN(test_btsrc_handover_kept);
    RUN(test_btsrc_handover_dropped);
    RUN(test_btsrc_handover_cold_boot);
    RUN(test_btsrc_late_stop_pc_to_phone);
    RUN(test_btsrc_late_stop_short_wait);
    RUN(test_btsrc_late_start_result);
    RUN(test_btsrc_late_stop_not);
    RUN(test_btsrc_field_1110_night_no_start);
    RUN(test_btsrc_field_1110_night_late_stop);
    RUN(test_btsrc_pair_gap);
    RUN(test_btsrc_field_1110_echo);
    RUN(test_btsrc_field_1110_paused_by_us);
    RUN(test_btsrc_field_1110_guard);
    RUN(test_btsrc_field_1109_tv_recall);
    RUN(test_btsrc_field_1109_evening_stale);
    RUN(test_btsrc_field_1109_evening_kept);
    RUN(test_btsrc_field_1109_evening_late_stop);
    RUN(test_btsrc_field_1109_pc_avrcp);
    RUN(test_btsrc_switch_field_1103);
    RUN(test_btsrc_reconnect_choice);
    RUN(test_btsrc_interleaved_field);
    RUN(test_btsrc_garbled_status);
    RUN(test_btsrc_paused_source_kept);
    RUN(test_btsrc_lone_browsing_closed);
    RUN(test_btsrc_switch_windows_pause_suspend);
    RUN(test_btsrc_windows_silent_after_sink_stop);
    RUN(test_btsrc_windows_paused_fresh);
    RUN(test_btsrc_play_status_toggle);
    RUN(test_btsrc_field_1103_blip);
    RUN(test_btsrc_late_play);
    RUN(test_btsrc_play_status_paused_stream);
    RUN(test_btsrc_play_status_lines);
    RUN(test_btsrc_stream_playing);
    RUN(test_btsrc_windows_no_avrcp);
    RUN(test_btsrc_windows_avrcp_and_ready);
    RUN(test_btsrc_reboot_suspect);
    RUN(test_btsrc_reboot_loop);
    RUN(test_btsrc_codec);
    RUN(test_btsrc_guard);
    RUN(test_btsrc_autocall_on);
    RUN(test_btsrc_autocall_op);
    RUN(test_btsrc_class_op);
    RUN(test_btsrc_avrcp_features_op);
    RUN(test_btsrc_audio_op);
    RUN(test_btsrc_audio_event_lines);
    RUN(test_btsrc_switch_call);
    RUN(test_btsrc_line_log);
    RUN(test_btsrc_list_field);
    RUN(test_btsrc_two_streams);
    RUN(test_btsrc_no_ping_pong);
    RUN(test_btsrc_sweep);
    RUN(test_btsrc_standby);
    RUN(test_btsrc_call_pref);
    RUN(test_btsrc_recover);
}
