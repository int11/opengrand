/* OpenGrand - Bluetooth audio source manager (see ca79ng/btsrc.h). */
#include "ca79ng/btsrc.h"

#include <string.h>

#include "ca79ng/util.h"

/* ---- helpers ------------------------------------------------------------------------------------ */
#define MAX_TOK 12
typedef struct {
    const char *s[MAX_TOK];
    uint8_t n[MAX_TOK];
    int count;
} toks_t;

static void tokenize(const char *line, uint32_t len, toks_t *t)
{
    uint32_t i = 0;
    t->count = 0;
    while (i < len && t->count < MAX_TOK) {
        uint32_t s;
        while (i < len && line[i] == ' ')
            i++;
        if (i >= len)
            break;
        s = i;
        while (i < len && line[i] != ' ')
            i++;
        t->s[t->count] = line + s;
        t->n[t->count] = (uint8_t)(i - s < 255 ? i - s : 255);
        t->count++;
    }
}

static bool tok_is(const toks_t *t, int i, const char *w)
{
    size_t n = strlen(w);
    return i < t->count && t->n[i] == n && memcmp(t->s[i], w, n) == 0;
}

static int tok_num(const toks_t *t, int i, int max)
{
    int v = 0;
    if (i >= t->count || t->n[i] == 0 || t->n[i] > 3)
        return -1;
    for (int k = 0; k < t->n[i]; k++) {
        char c = t->s[i][k];
        if (c < '0' || c > '9')
            return -1;
        v = v * 10 + (c - '0');
    }
    return v <= max ? v : -1;
}

static int hexv(char c)
{
    if (c >= '0' && c <= '9')
        return c - '0';
    if (c >= 'a' && c <= 'f')
        return c - 'a' + 10;
    if (c >= 'A' && c <= 'F')
        return c - 'A' + 10;
    return -1;
}

/* xx:xx:xx:xx:xx:xx */
static bool tok_addr(const toks_t *t, int i, uint8_t a[6])
{
    const char *s;
    if (i >= t->count || t->n[i] != 17)
        return false;
    s = t->s[i];
    for (int k = 0; k < 6; k++) {
        int h = hexv(s[k * 3]), l = hexv(s[k * 3 + 1]);
        if (h < 0 || l < 0 || (k < 5 && s[k * 3 + 2] != ':'))
            return false;
        a[k] = (uint8_t)(h << 4 | l);
    }
    return true;
}

/* a PSM token as printed by the module (hex, "17" / "19" / "1b"); 0 = none */
static uint8_t tok_hex(const toks_t *t, int i)
{
    int v = 0;
    if (i >= t->count || t->n[i] == 0 || t->n[i] > 2)
        return 0;
    for (int k = 0; k < t->n[i]; k++) {
        int h = hexv(t->s[i][k]);
        if (h < 0)
            return 0;
        v = v * 16 + h;
    }
    return (uint8_t)v;
}

static uint8_t prof_of(const toks_t *t, int i)
{
    if (tok_is(t, i, "A2DP"))
        return BTS_P_A2DP;
    if (tok_is(t, i, "AVRCP"))
        return BTS_P_AVRCP;
    return i < t->count ? BTS_P_OTHER : BTS_P_NONE;
}

static void put_hex(char *o, uint8_t v)
{
    static const char H[] = "0123456789abcdef";
    o[0] = H[v >> 4];
    o[1] = H[v & 15];
}

/* ---- policy ------------------------------------------------------------------------------------- */
void bts_policy_default(bts_policy_t *p)
{
    memset(p, 0, sizeof *p);
    /* user requirement 2026-11-02: every source stays connected, the one the user selects plays; no link is closed by the policy except the A2DP link of a source that keeps forcing its stream */
    p->mode = BTS_MODE_SELECT;
    p->flags = BTS_F_KEEP_PREF;
    p->hold_ds = 15;   /* 1.5 s: a second stream silences the module, so short */
    p->sweep_s = 5;    /* a notification sound does not sweep the other sources away */
    p->recon = BTS_RC_PREF_LAST; /* AUTOCALL off: the preferred device, else the last selected one (user 2026-11-09) */
    p->xopt = BTS_LATE_STOP_DEFAULT ? BTS_X_LATE_STOP : 0;   /* the late stop */
    p->xwait = BTS_XWAIT_DEFAULT;                            /* ... its wait */
}

/* the legacy mode NEWEST (closes the sources that lose - against the no-CLOSE rule;
 * the app never offers it) is compiled out of the cave (-DBTS_NO_NEWEST: the slot payload needs the room):
 * there the policy write refuses it.  The host model keeps it for its tests. */
#ifdef BTS_NO_NEWEST
#define NEWEST_OK 0
#else
#define NEWEST_OK 1
#endif

int bts_policy_check(const bts_policy_t *p)
{
    if (p->mode > BTS_MODE_SELECT || (!NEWEST_OK && p->mode == BTS_MODE_NEWEST) || (p->flags & ~BTS_F_ALL) || p->hold_ds == 0 || p->hold_ds > 250 ||
        p->sweep_s == 0 || p->sweep_s > 250 || p->has_pref > 1 || p->recon > BTS_RC_MAX ||
        (p->xopt & ~BTS_X_ALL) || p->xwait > BTS_XWAIT_MAX)
        return -1;
    return 0;
}

void bts_saved_encode(const bts_policy_t *p, uint8_t out[BTS_SAVED_RAW])
{
    memset(out, 0, BTS_SAVED_RAW);
    out[0] = 1;   /* version */
    out[1] = p->recon;
    out[2] = p->has_pref;
    if (p->has_pref)
        memcpy(out + 3, p->pref, 6);
    out[9] = (p->flags & BTS_F_FOLLOW) ? 0x01 : 0;   /* kept flags */
}

bool bts_saved_apply(bts_policy_t *p, const uint8_t *raw, uint32_t n)
{
    if (n < 9 || raw[0] != 1 || raw[1] > BTS_RC_MAX || raw[2] > 1)
        return false;
    if (n >= 10 && (raw[9] & 0x01))
        p->flags |= BTS_F_FOLLOW;
    else
        p->flags &= (uint8_t)~BTS_F_FOLLOW;
    p->recon = raw[1];
    p->has_pref = raw[2];
    if (raw[2])
        memcpy(p->pref, raw + 3, 6);
    else
        memset(p->pref, 0, 6);
    return true;
}

/* ---- table -------------------------------------------------------------------------------------- */
static void forget_links(bts_t *b)
{
    memset(b->dev, 0, sizeof b->dev);
    for (int i = 0; i < BTS_LINKS; i++) {
        b->link_dev[i] = -1;
        b->link_prof[i] = BTS_P_NONE;
        b->link_ch[i] = 0;
    }
    b->active = -1;
    b->listing = 0;
    b->list_want = 0;
    b->dual_ms = 0;
    b->call_on = 0;
    b->av_lid = -1;
    b->set_listing = 0;
}

static void sw_end(bts_t *b, uint8_t result)
{
    if (b->sw_phase == BTS_SW_IDLE)
        return;
    b->sw_phase = BTS_SW_IDLE;
    b->sw_result = result;
    b->sw_last_ms = b->sw_total;
    b->sw_ago = 0;
    /* ended without a PLAY decision on the target (no AVRCP then): its AVRCP may still come (sw_late_play) */
    b->sw_late = b->sw_play && !b->sw_pl && (result == BTS_RES_PLAYING || result == BTS_RES_READY);
    b->sw_late_ms = 0;
}

void bts_init(bts_t *b, const bts_policy_t *pol)
{
    memset(b, 0, sizeof *b);
    if (pol && bts_policy_check(pol) == 0)
        b->pol = *pol;
    else
        bts_policy_default(&b->pol);
    forget_links(b);
    b->gap_ms = BTS_CMD_GAP_MS;
    b->recover_ago = BTS_RECOVER_GAP_MS;
    b->pref_wait = BTS_PREF_RETRY_MS;
}

static void module_restarted(bts_t *b)
{
    sw_end(b, BTS_RES_CANCELLED);
    b->sw_late = 0;
    forget_links(b);
    b->route = 0;   /* the routing events start again with the module */
    b->rc_tries = 0;
    b->rc_wait = 0;
    b->rc_up_ms = 0;
    b->q_n = 0;
    b->waiting = 0;
    b->no_answer = 0;
    b->ready_ms = 0;
    b->pref_tries = 0;
    b->pref_wait = BTS_PREF_RETRY_MS;
}

/* a reboot the module did by itself: count it; the second within BTS_LOOP_WIN_MS sets the loop guard; a lone browsing
 * channel just before it marks that source as the suspect (the sink never opens its AVRCP control link first any more -
 * field 2026-11-09: our early CALL to such a PC stayed unanswered and preceded its lone browsing channel) */
static void module_rebooted(bts_t *b)
{
    if (b->n.reboots < 0xFFFFu)
        b->n.reboots++;
    if (b->reboot_seen && b->reboot_ago < BTS_LOOP_WIN_MS && !b->loop) {
        b->loop = 1;
        bts_log(b, BTS_L_EVENT, "module reboot loop", 18);
    } else {
        bts_log(b, BTS_L_EVENT, "module rebooted", 15);
    }
    b->reboot_seen = 1;
    b->reboot_ago = 0;
    if (b->has_orphan && b->orphan_ago < BTS_ORPHAN_REBOOT_MS) {
        memcpy(b->suspect, b->orphan_addr, 6);
        b->has_suspect = 1;
    }
    b->has_orphan = 0;
}

uint8_t bts_health(const bts_t *b)
{
    return (uint8_t)((b->loop ? BTS_MH_LOOP : 0) | (b->has_suspect ? BTS_MH_SUSPECT : 0) | (b->pin_bad ? BTS_MH_PIN : 0));
}

void bts_module(bts_t *b, bool on)
{
    if ((b->module_on != 0) != on) {
        module_restarted(b);
        bts_log(b, BTS_L_EVENT, on ? "module on" : "module off", on ? 9u : 10u);
        if (!on && b->pin == BTS_PIN_RESTART)
            b->pin = BTS_PIN_BACK;   /* the pin's restart (or any power edge meanwhile): read the listing once it is back */
    }
    b->module_on = on;
}

static int dev_by_addr(const bts_t *b, const uint8_t a[6])
{
    for (int i = 0; i < BTS_DEV_MAX; i++)
        if (b->dev[i].used && b->dev[i].addr_known && memcmp(b->dev[i].addr, a, 6) == 0)
            return i;
    return -1;
}

static int dev_new(bts_t *b)
{
    for (int pass = 0; pass < 2; pass++)   /* a free slot, else one parked with no link (the oldest kept is fine) */
        for (int i = 0; i < BTS_DEV_MAX; i++)
            if (!b->dev[i].used || (pass && b->dev[i].parked && !b->dev[i].links)) {
                if (b->active == i)
                    b->active = -1;
                memset(&b->dev[i], 0, sizeof b->dev[i]);
                b->dev[i].used = 1;
                return i;
            }
    return -1;
}

static void dev_free_if_empty(bts_t *b, int d)
{
    if (d < 0 || !b->dev[d].used || b->dev[d].links || b->dev[d].parked)
        return;
    memset(&b->dev[d], 0, sizeof b->dev[d]);
    if (b->active == d)
        b->active = -1;
}

static void link_drop(bts_t *b, int l)
{
    int d = b->link_dev[l];
    if (d >= 0) {
        bts_dev_t *v = &b->dev[d];
        v->links = (uint16_t)(v->links & ~(1u << l));
        v->a2dp = (uint16_t)(v->a2dp & ~(1u << l));
        if (!v->a2dp) {
            v->streaming = 0;
            v->stream_ms = 0;
            v->avrcp_called = 0;   /* a new A2DP episode may open AVRCP again */
            v->noavrcp_ms = 0;
            v->a2dp_closing = 0;
            v->fresh = 0;
            v->susp = 0;
            v->a2dp_ms = 0;
        }
        if (v->ps_link == l + 1) {   /* its registration went with the AVRCP link: the status is not known any more */
            v->ps_link = 0;
            v->ps = BTS_PS_UNKNOWN;
            v->ps_pend = v->ps_live = v->ps_none = 0;
            v->ps_wait = 0;
        }
        dev_free_if_empty(b, d);
    }
    b->link_dev[l] = -1;
    b->link_prof[l] = BTS_P_NONE;
    b->link_ch[l] = 0;
}

/* link l belongs to the device with this address (NULL: not known yet - a device of its own) */
static void link_set(bts_t *b, int l, const uint8_t *addr, uint8_t prof, int incoming)
{
    int d = addr ? dev_by_addr(b, addr) : -1;
    int cur = b->link_dev[l];
    if (cur >= 0 && addr && !b->dev[cur].addr_known && d < 0) {
        /* the placeholder learns its address */
        memcpy(b->dev[cur].addr, addr, 6);
        b->dev[cur].addr_known = 1;
        d = cur;
    }
    if (cur >= 0 && d >= 0 && cur != d) {
        /* move the link (and a placeholder's state) to the known device */
        bts_dev_t *o = &b->dev[cur], *n = &b->dev[d];
        if (o->streaming && (!n->streaming || o->seq > n->seq)) {
            n->streaming = 1;
            n->seq = o->seq;
            n->stream_ms = o->stream_ms;
        }
        if (b->active == cur)
            b->active = (int8_t)d;
        o->links = (uint16_t)(o->links & ~(1u << l));
        o->a2dp = (uint16_t)(o->a2dp & ~(1u << l));
        dev_free_if_empty(b, cur);
    }
    if (d < 0)
        d = cur >= 0 ? cur : dev_new(b);
    if (d < 0)
        return;   /* table full: the link stays unknown */
    if (addr && !b->dev[d].addr_known) {
        memcpy(b->dev[d].addr, addr, 6);
        b->dev[d].addr_known = 1;
    }
    b->link_dev[l] = (int8_t)d;
    if (prof != BTS_P_NONE)
        b->link_prof[l] = prof;
    b->dev[d].links = (uint16_t)(b->dev[d].links | (1u << l));
    if (b->link_prof[l] == BTS_P_A2DP) {
        bts_dev_t *v = &b->dev[d];
        if (!v->a2dp) {   /* a new A2DP episode: ours if our CALL to this address brought it */
            v->fresh = !v->streaming && b->call_on && !b->call_avrcp && v->addr_known &&
                       memcmp(v->addr, b->call_addr, 6) == 0;
            v->a2dp_ms = 0;
        }
        v->a2dp = (uint16_t)(v->a2dp | (1u << l));
        v->parked = 0;   /* a sound link again */
    }
    if (incoming >= 0 && b->link_prof[l] == BTS_P_A2DP)
        b->dev[d].incoming = (uint8_t)incoming;
}

static int closed_find(const bts_t *b, const uint8_t a[6])
{
    for (int i = 0; i < BTS_DEV_MAX; i++)
        if (b->closed[i].used && memcmp(b->closed[i].addr, a, 6) == 0)
            return i;
    return -1;
}

static bool closed_within(const bts_t *b, const bts_dev_t *v, uint32_t ms)
{
    int c;
    if (!v->addr_known)
        return false;
    c = closed_find(b, v->addr);
    return c >= 0 && b->closed[c].ago_ms < ms;
}

static void closed_note(bts_t *b, const bts_dev_t *v)
{
    int c, oldest = 0;
    if (!v->addr_known)
        return;
    c = closed_find(b, v->addr);
    if (c < 0) {
        for (int i = 0; i < BTS_DEV_MAX; i++) {
            if (!b->closed[i].used) {
                oldest = i;
                break;
            }
            if (b->closed[i].ago_ms > b->closed[oldest].ago_ms)
                oldest = i;
        }
        c = oldest;
    }
    b->closed[c].used = 1;
    memcpy(b->closed[c].addr, v->addr, 6);
    b->closed[c].ago_ms = 0;
}

/* ---- command queue ------------------------------------------------------------------------------ */
static bool q_push(bts_t *b, const char *s)
{
    size_t n = strlen(s);
    if (b->q_n >= BTS_Q || n >= BTS_CMD_MAX)
        return false;
    for (int i = 0; i < b->q_n; i++)
        if (strlen(b->q[i]) == n && memcmp(b->q[i], s, n) == 0)
            return true;   /* already queued */
    memcpy(b->q[b->q_n], s, n + 1);
    b->q_n++;
    return true;
}

/* one command ahead of everything queued (it goes out after the gap) */
static bool q_push_front(bts_t *b, const char *s)
{
    size_t n = strlen(s);
    if (b->q_n >= BTS_Q || n >= BTS_CMD_MAX)
        return false;
    for (int i = b->q_n; i > 0; i--)
        memcpy(b->q[i], b->q[i - 1], BTS_CMD_MAX);
    memcpy(b->q[0], s, n + 1);
    b->q_n++;
    return true;
}

/* two commands that belong together (link selection + the command for it): both or none, no de-duplication */
static bool q_push2(bts_t *b, const char *s1, const char *s2)
{
    size_t n1 = strlen(s1), n2 = strlen(s2);
    if (b->q_n + 2 > BTS_Q || n1 >= BTS_CMD_MAX || n2 >= BTS_CMD_MAX)
        return false;
    memcpy(b->q[b->q_n++], s1, n1 + 1);
    memcpy(b->q[b->q_n++], s2, n2 + 1);
    return true;
}

/* a link id (0..15) in decimal, no division (the payload links no signed divide helper) */
static int put_num(char *o, unsigned v)
{
    int k = 0;
    if (v >= 10) {
        o[k++] = '1';
        v -= 10;
    }
    o[k++] = (char)('0' + v);
    return k;
}

/* the sink may stop a source's stream: never since 2026-11-10; the old behaviour (susp_ok,
 * host model tests only) is compiled out of the cave (-DBTS_NEVER_SUSPEND: the slot payload has no room for dead code) */
#ifdef BTS_NEVER_SUSPEND
#define SUSP_OK(b) ((void)(b), 0)
#else
#define SUSP_OK(b) ((b)->susp_ok != 0)
#endif

/* counters only the host tests read (no wire byte, no port reads them): compiled out of the cave (-DBTS_NO_TEST_STATS,
 * the slot payload has no room for numbers nobody sees) */
#ifdef BTS_NO_TEST_STATS
#define TSTAT(f) ((void)0)
#else
#define TSTAT(f) do { if (b->n.f < 0xFFFFu) b->n.f++; } while (0)
#endif

/* the AVRCP control link of a device: PSM 0x17, else the lowest AVRCP link whose PSM is not known; never the browsing
 * channel (0x1b - field 2026-11-02: a PC opened it alone, `AVRCP … ERROR_NO_CONTROL_CHANNEL`); -1 none */
static int avrcp_link(const bts_t *b, int d)
{
    int any = -1;
    for (int l = 0; l < BTS_LINKS; l++)
        if ((b->dev[d].links & (1u << l)) && b->link_prof[l] == BTS_P_AVRCP) {
            if (b->link_ch[l] == 0x17)
                return l;
            if (any < 0 && b->link_ch[l] != 0x1b)
                any = l;
        }
    return any;
}

static void q_close_link(bts_t *b, unsigned l);

/* the A2DP signalling link of a device (its lowest A2DP link id - the one STREAMING lines name, AN986 3.5); -1 */
static int a2dp_link(const bts_t *b, int d)
{
    for (int l = 0; l < BTS_LINKS; l++)
        if (b->dev[d].a2dp & (1u << l))
            return l;
    return -1;
}

/* `SET <l> SELECT` + cmd: the command goes to link l (RM 6.98 - a run-time selection, nothing stored) */
static bool q_on_link(bts_t *b, int l, const char *cmd)
{
    char sel[16] = "SET ";
    int k = 4;
    k += put_num(sel + k, (unsigned)l);
    memcpy(sel + k, " SELECT", 8);
    return q_push2(b, sel, cmd);
}

/* the sink suspends device d's stream: `SET <its A2DP signalling link> SELECT` + `A2DP STREAMING STOP` (AN986 6.1 table
 * 1; RN 6.5.2 routes A2DP commands to the selected link; field 2026-11-02 #3: Windows answered `STREAMING STOP 0` in
 * 0.1 s).  The links stay - field 2026-11-03: closing a source's A2DP dropped the phone's
 * AVRCP and removed the Windows endpoint (Chrome's audio broke).  Once until it starts again.  The 2026-11-02 evening
 * belief "Windows is silent after a sink STOP" is confounded - the silent case followed a module restart,
 * i.e. the endpoint loss [inference]; the field matrix decides. */
static bool q_stop_stream(bts_t *b, int d);
static bool q_suspend(bts_t *b, int d)
{
    if (a2dp_link(b, d) < 0 || b->dev[d].susp || !b->dev[d].streaming)
        return false;
    if (!SUSP_OK(b) || avrcp_link(b, d) < 0) {
        /* never (field 2026-11-09 / 2026-11-10): after a sink-initiated suspend the Windows
         * Bluetooth audio device stayed silent through the sink's START, pause / play on the PC, a Windows reconnect, a
         * module restart - only restarting its PnP device brought the sound back.  The module lines cannot tell Windows
         * from a phone before the harm, so no source is stopped: it is paused (AVRCP) and the switch waits for its
         * own STOP (BTS_SW_HELD_MS); without AVRCP the user pauses it there / 「끊기」 */
        if (!b->dev[d].held) {
            b->dev[d].held = 1;
            TSTAT(held);
            bts_log(b, BTS_L_EVENT, "not stopped", 11);
        }
        return false;
    }
    return q_stop_stream(b, d);
}

/* the sink's stop itself (q_suspend - host model only - and the opt-in late stop of a switch, BTS_X_LATE_STOP, which
 * the cave keeps): once until the stream starts again (susp, cleared by its START / A2DP link gone) */
static bool q_stop_stream(bts_t *b, int d)
{
    int l = a2dp_link(b, d);
    if (l < 0 || b->dev[d].susp || !b->dev[d].streaming || !q_on_link(b, l, "A2DP STREAMING STOP"))
        return false;
    b->dev[d].susp = 1;
    if (b->n.suspends < 0xFFFFu)
        b->n.suspends++;
    return true;
}

/* close device d's A2DP only (the user's act 6 - never a switch / the guard): CLOSE its signalling link - "Terminating the control channel makes iWRAP also terminate
 * the data channel" (AN986 3.6.1); the AVRCP link and the ACL stay.  Once per link (a second CLOSE could hit a reused
 * link id). */
static int q_close_a2dp(bts_t *b, int d)
{
    int l = a2dp_link(b, d);
    if (l < 0)
        return BTS_UNKNOWN;
    if (b->dev[d].a2dp_closing)
        return BTS_OK;
    b->dev[d].a2dp_closing = 1;
    b->dev[d].parked = b->dev[d].addr_known;   /* stays listed (no AVRCP: no link left) - picking it CALLs it */
    b->dev[d].a2dp_close_ms = 0;
    q_close_link(b, (unsigned)l);
    if (b->n.closes < 0xFFFFu)
        b->n.closes++;
    return BTS_OK;
}

/* `SET <l> SELECT` + `AVRCP PAUSE|PLAY` for device d */
static int q_avrcp(bts_t *b, int d, bool play)
{
    int l = avrcp_link(b, d);
    if (l < 0)
        return BTS_NO_AVRCP;
    if (!q_on_link(b, l, play ? "AVRCP PLAY" : "AVRCP PAUSE"))
        return BTS_FULL;
    b->dev[d].ps_ours = !play;   /* PAUSE: we paused it (a later PLAY is ours to send); PLAY: no longer */
    b->dev[d].pz_wait = !play;   /* PAUSE: its report should follow - else asked again (BTS_PZ_RECHECK_MS) */
    b->dev[d].pz_ms = 0;
    return BTS_OK;
}

static void q_close_link(bts_t *b, unsigned l)
{
    char c[10] = "CLOSE ";
    int k = 6;
    k += put_num(c + k, l & 15u);   /* no division: the payload links no divide helper */
    c[k] = 0;
    q_push(b, c);
}

static int close_dev(bts_t *b, int d)
{
    bts_dev_t *v = &b->dev[d];
    int n = 0;
    if (!v->links)
        return 0;
    /* A2DP first (the sound), then the rest of the device's links */
    for (int pass = 0; pass < 2; pass++)
        for (int l = 0; l < BTS_LINKS; l++)
            if ((v->links & (1u << l)) && ((b->link_prof[l] == BTS_P_A2DP) == (pass == 0))) {
                q_close_link(b, (unsigned)l);
                n++;
            }
    v->closing = 1;
    v->closing_ms = 0;
    closed_note(b, v);
    if (b->n.closes < 0xFFFFu)
        b->n.closes++;
    return n;
}

/* `CALL <addr> 19 A2DP` (RM 6.16, AN986 3.5.1) or `CALL <addr> 17 AVRCP` (RM 6.16 example, AN986 3.5.2) */
static bool q_call_p(bts_t *b, const uint8_t a[6], bool avrcp)
{
    char c[BTS_CMD_MAX] = "CALL ";
    int k = 5;
    for (int i = 0; i < 6; i++) {
        put_hex(c + k, a[i]);
        k += 2;
        if (i < 5)
            c[k++] = ':';
    }
    memcpy(c + k, avrcp ? " 17 AVRCP" : " 19 A2DP", avrcp ? 10 : 9);
    if (!q_push(b, c))
        return false;
    if (b->n.calls < 0xFFFFu)
        b->n.calls++;
    b->call_on = 1;   /* CALL <l> / address-less CONNECT lines from now on belong to this address */
    b->call_avrcp = avrcp;
    memcpy(b->call_addr, a, 6);
    b->call_ms = 0;
    return true;
}

static void q_call(bts_t *b, const uint8_t a[6]) { q_call_p(b, a, false); }

uint32_t bts_next(bts_t *b, char *out, uint32_t cap)
{
    size_t n;
    /* nothing while the module prints a `SET` listing: an echoed bare `SET` would end it early */
    if (!b->q_n || b->gap_ms < BTS_CMD_GAP_MS || !b->module_on || b->set_listing)
        return 0;
    if (b->q[0][0] == BTS_Q_AUDIO || b->q[0][0] == BTS_Q_AVRCP) {
        /* a module-setting op's token: the line is formed here (the audio one is longer than a queue slot) */
        uint8_t v = (uint8_t)((b->q[0][1] & 15) << 4 | (b->q[0][2] & 15));
        if (cap < BTS_OUT_MAX)
            return 0;
        if (b->q[0][0] == BTS_Q_AUDIO) {
            bts_audio_line(out, v);
        } else {
            /* `SET PROFILE AVRCP CONTROLLER <2 lower-case hex digits>` - the form the module lists */
            static const char hx[] = "0123456789abcdef";
            memcpy(out, "SET PROFILE AVRCP CONTROLLER ", 29);
            out[29] = hx[v >> 4];
            out[30] = hx[v & 15];
            out[31] = 0;
        }
        n = strlen(out);
    } else {
        n = strlen(b->q[0]);
        if (n + 1 > cap)
            return 0;
        memcpy(out, b->q[0], n + 1);
    }
    for (int i = 1; i < b->q_n; i++)
        memcpy(b->q[i - 1], b->q[i], BTS_CMD_MAX);
    b->q_n--;
    b->gap_ms = 0;
    b->waiting = 1;
    b->wait_ms = 0;
    /* `SET <l> SELECT` (q_on_link - always the first of a pair): its echo lets the paired command go at once instead of
     * BTS_CMD_GAP_MS later (the switch's PAUSE / STOP / PLAY each waited the full gap) */
    b->sel_out = n >= 12 && n <= 13 && memcmp(out, "SET ", 4) == 0 && memcmp(out + n - 7, " SELECT", 7) == 0;
    if (b->sel_out)
        b->sel_lnk = (int8_t)(n == 12 ? out[4] - '0' : (out[4] - '0') * 10 + (out[5] - '0'));
    bts_log(b, BTS_L_TX, out, (uint32_t)n);
    if ((n == 20 || n == 19) && memcmp(out, "A2DP STREAMING ST", 17) == 0) {
        /* the module echoes it (no link id): that echo is no stream event (field 2026-11-10 -
         * the echo of the PC's sink START was taken for the phone's own start, the piano then skipped its PLAY) */
        b->echo_ss |= out[17] == 'A' ? 1u : 2u;
        b->echo_ms = 0;
    }
#ifndef BTS_NO_TEST_STATS
    if (n == 4 && memcmp(out, "LIST", 4) == 0)
        TSTAT(lists);
    if (n >= 12 && memcmp(out, "SET BT CLASS", 12) == 0)
        TSTAT(cod_writes);
    if (n >= 17 && memcmp(out, "SET PROFILE AVRCP", 17) == 0)
        TSTAT(avf_writes);
#endif
    return (uint32_t)n;
}

/* ---- lines -------------------------------------------------------------------------------------- */
static void list_finish(bts_t *b)
{
    for (int l = 0; l < BTS_LINKS; l++)
        if (b->link_dev[l] >= 0 && !(b->list_seen & (1u << l)))
            link_drop(b, l);
    b->listing = 0;
    b->ho.listed = 1;   /* live apply handover: the table is the module's now */
}

static int newest_a2dp_dev(const bts_t *b)
{
    int best = -1;
    for (int i = 0; i < BTS_DEV_MAX; i++)
        if (b->dev[i].used && b->dev[i].a2dp && (best < 0 || b->dev[i].seq > b->dev[best].seq))
            best = i;
    return best;
}

/* `SET CONTROL AUTOCALL <target hex> <timeout dec> <profile>` (RM 6.68) - a line restore may write back */
static bool autocall_line_ok(const toks_t *t, uint32_t len)
{
    if (t->count != 6 || len >= BTS_CMD_MAX || !tok_is(t, 1, "CONTROL") || !tok_is(t, 2, "AUTOCALL"))
        return false;
    for (int i = 3; i < 6; i++) {
        if (t->n[i] == 0 || t->n[i] > (i == 3 ? 32 : i == 4 ? 5 : 8))
            return false;
        for (int k = 0; k < t->n[i]; k++) {
            char c = t->s[i][k];
            bool ok = i == 3   ? hexv(c) >= 0
                      : i == 4 ? (c >= '0' && c <= '9')
                               : ((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-');
            if (!ok)
                return false;
        }
    }
    return true;
}

/* `SET BT CLASS <hex, 1..6 digits>` as the module lists it (field 2026-11-09: `SET BT CLASS 001f00`); -1 not that line */
static int32_t class_line(const toks_t *t)
{
    uint32_t v = 0;
    if (t->count != 4 || !tok_is(t, 1, "BT") || !tok_is(t, 2, "CLASS") || t->n[3] == 0 || t->n[3] > 6)
        return -1;
    for (int k = 0; k < t->n[3]; k++) {
        int h = hexv(t->s[3][k]);
        if (h < 0)
            return -1;
        v = v << 4 | (uint32_t)h;
    }
    return (int32_t)v;
}

/* `SET PROFILE AVRCP …` as the module lists it (field 2026-11-09: `SET PROFILE AVRCP CONTROLLER 1f`; syntax AN986 3.2
 * `SET PROFILE AVRCP {CONTROLLER | TARGET} {features} [{CONTROLLER | TARGET} {features}]`): -1 not that line, else
 * BTS_AVF_CTRL with *f = the controller features (the controller-only form), or BTS_AVF_OTHER (any other form) */
static int avrcp_line(const toks_t *t, uint8_t *f)
{
    uint32_t v = 0;
    if (t->count < 3 || !tok_is(t, 1, "PROFILE") || !tok_is(t, 2, "AVRCP"))
        return -1;
    if (t->count != 5 || !tok_is(t, 3, "CONTROLLER") || t->n[4] == 0 || t->n[4] > 2)
        return BTS_AVF_OTHER;
    for (int k = 0; k < t->n[4]; k++) {
        int h = hexv(t->s[4][k]);
        if (h < 0)
            return BTS_AVF_OTHER;
        v = v << 4 | (uint32_t)h;
    }
    *f = (uint8_t)v;
    return BTS_AVF_CTRL;
}

/* the end of a complete listing for an 8-bit setting: the state and value read; the first value read with
 * (v & mask) == keep is the module's own (restore writes it back); a pending op's result (read: OK; write / restore:
 * the value read back is the one wanted) */
static void set8_end(bts_set8_t *s, uint8_t mask, uint8_t keep, uint8_t restore)
{
    s->state = s->set_state;
    if (s->state == 1) {
        s->now = s->set_v;
        if (!s->saved && (s->now & mask) == keep) {
            s->orig = s->now;
            s->saved = 1;
        }
    }
    if (s->op) {
        uint8_t want = s->op == 2 ? s->want : restore;
        s->rc = s->op == 1 || (s->state == 1 && s->now == want) ? BTS_OK : BTS_STATE;
        s->op = 0;
    }
}

/* the `SET` listing (RM 3.5 / 6.68): `SET BT BDADDR …` first, a bare `SET` last; `SET CONTROL AUTOCALL …` is listed
 * only while AUTOCALL is enabled.  A complete listing decides the AUTOCALL state and the class of device. */
static void set_line(bts_t *b, const toks_t *t, const char *line, uint32_t len)
{
    if (t->count >= 3 && tok_is(t, 1, "BT") && tok_is(t, 2, "BDADDR")) {
        b->set_listing = 1;
        b->set_seen_ac = 0;
        b->set_seen_cod = 0;
        b->set_seen_avf = 0;
        b->set_avf_state = BTS_AVF_OTHER;   /* no AVRCP line: the profile is off */
        b->set_aud_state = BTS_AUD_OTHER;   /* no `SET CONTROL AUDIO` line: not the plain form */
        b->set_ms = 0;
        return;
    }
    if (!b->set_listing)
        return;   /* an echo (`SET`, `SET 5 SELECT`, `SET CONTROL AUTOCALL`) or a line outside a listing */
    b->set_ms = 0;
    if (class_line(t) >= 0) {
        b->set_seen_cod = 1;
        b->set_cod = (uint32_t)class_line(t);
    } else if (t->count >= 3 && tok_is(t, 1, "PROFILE") && tok_is(t, 2, "AVRCP")) {
        uint8_t f = 0;
        int k = avrcp_line(t, &f);
        if (!b->set_seen_avf || k == BTS_AVF_OTHER) {   /* a second AVRCP line: not the plain form */
            b->set_avf_state = b->set_seen_avf ? BTS_AVF_OTHER : (uint8_t)k;
            b->set_avf = f;
        }
        b->set_seen_avf = 1;
    } else if (len >= 17 && memcmp(line, "SET CONTROL AUDIO", 17) == 0) {
        /* RM 6.67 list format `SET CONTROL AUDIO {sco} {a2dp} [EVENT] [KEEPALIVE] [AAx] [i2s_bits]`: the plain form is
         * exactly one of the four lines bts_audio_line forms (routing `INTERNAL I2S_SLAVE`, EVENT / KEEPALIVE in the
         * RM order) - anything else OTHER */
        char w[BTS_OUT_MAX];
        uint8_t k = BTS_AUD_OTHER;
        for (uint8_t f = 0; f < 4; f++) {
            bts_audio_line(w, f);
            if (strlen(w) == len && memcmp(w, line, len) == 0) {
                k = BTS_AUD_STD;
                b->set_aud = f;
            }
        }
        b->set_aud_state = k;
    } else if (autocall_line_ok(t, len)) {
        b->set_seen_ac = 1;
        memcpy(b->set_ac, line, len);
        b->set_ac[len] = 0;
    } else if (t->count == 1) {   /* the bare `SET` that ends the listing */
        b->set_listing = 0;
        b->set_done = 1;
        b->ac_state = b->set_seen_ac ? BTS_AC_ON : BTS_AC_OFF;
        if (b->set_seen_ac && !b->ac_saved) {
            memcpy(b->ac_line, b->set_ac, BTS_CMD_MAX);   /* the original - restore writes exactly this back */
            b->ac_saved = 1;
        }
        if (b->ac_op) {
            b->ac_rc = (b->ac_op == BTS_AC_SET_OFF) == (b->ac_state == BTS_AC_OFF) ? BTS_OK : BTS_STATE;
            if (b->ac_op == BTS_AC_READ)
                b->ac_rc = BTS_OK;
            b->ac_op = 0;
        }
        b->cod_state = b->set_seen_cod ? BTS_COD_KNOWN : BTS_COD_UNKNOWN;
        if (b->set_seen_cod) {
            b->cod = b->set_cod;
            if (!b->cod_saved && !bts_cod_ok(b->cod)) {
                b->cod_orig = b->cod;   /* the module's own class - restore writes exactly this back */
                b->cod_saved = 1;
            }
        }
        if (b->cod_op) {
            uint32_t want = b->cod_op == BTS_CO_WRITE ? b->cod_want : bts_class_restore(b);
            b->cod_rc = b->cod_op == BTS_CO_READ ? (b->set_seen_cod ? BTS_OK : BTS_STATE)
                      : b->set_seen_cod && b->cod == want ? BTS_OK : BTS_STATE;
            b->cod_op = 0;
        }
        /* AVRCP features: no AVRCP line at all = the profile is off (BTS_AVF_OTHER - nothing to write); the module's
         * own value (the browsing bit on) is what restore writes back */
        set8_end(&b->avs, BTS_AVF_BROWSE, BTS_AVF_BROWSE, bts_avf_restore(b));
        /* audio routing (no line: OTHER); the module's own flags (without EVENT - the op always writes it) */
        set8_end(&b->aus, BTS_AUD_EVENT, 0, bts_aud_restore(b));
    }
}

/* the status word after PLAYBACK_STATUS_CHANGED: RM 6.10.2 / AN986 6.3.4 print PLAYING / PAUSED, AN986 5.2 (browsing)
 * STOPPED; the AVRCP 1.4 names of the other values and the numbers of AN986 6.4.1 (0 stopped, 1 playing, 2 paused,
 * 3 fast forward, 4 rewind) are accepted too [inference: the docs show no other form]; anything else: unknown */
static uint8_t ps_word(const toks_t *t, int i)
{
    if (tok_is(t, i, "PLAYING") || tok_is(t, i, "1"))
        return BTS_PS_PLAYING;
    if (tok_is(t, i, "PAUSED") || tok_is(t, i, "2"))
        return BTS_PS_PAUSED;
    if (tok_is(t, i, "STOPPED") || tok_is(t, i, "0"))
        return BTS_PS_STOPPED;
    if (tok_is(t, i, "FWD_SEEK") || tok_is(t, i, "REV_SEEK") || tok_is(t, i, "3") || tok_is(t, i, "4"))
        return BTS_PS_SEEKING;
    if (tok_is(t, i, "ERROR") || tok_is(t, i, "255"))
        return BTS_PS_ERROR;   /* AVRCP 1.4 0xFF; a word not known at all stays unknown (the BT hold keeps holding) */
    return BTS_PS_UNKNOWN;
}

static bool ps_paused(const bts_dev_t *v);

static void ps_set(bts_t *b, int d, uint8_t ps)
{
    bts_dev_t *v = &b->dev[d];
    bool was = ps_paused(v);
    v->ps = ps;
    v->ps_old = 0;   /* a report newer than its stream */
    v->pz_wait = 0;  /* a report after our PAUSE */
    if (ps == BTS_PS_PLAYING || ps == BTS_PS_SEEKING)
        v->ps_ours = 0;   /* it plays: whatever paused it, it is not paused now */
    if (!was && ps_paused(v))
        v->pzr_ms = 0;    /* it began to report paused / stopped: the late stop's wait counts from here (a repeated report - the renewed registration's INTERIM - does not restart it) */
}

/* a status word cut or run into other bytes (field 2026-11-09: lines interleaved): it is a prefix of a known word, or a
 * known word is a prefix of it - not a word the target chose (`BOGUS` stays "unknown", as before) */
static bool ps_word_cut(const toks_t *t, int i)
{
    static const char *const W[6] = {"PLAYING", "PAUSED", "STOPPED", "FWD_SEEK", "REV_SEEK", "ERROR"};
    if (i >= t->count)
        return true;   /* no word at all after PLAYBACK_STATUS_CHANGED */
    for (int k = 0; k < 6; k++) {
        size_t n = strlen(W[k]), m = t->n[i] < n ? t->n[i] : n;
        if (t->n[i] != n && memcmp(t->s[i], W[k], m) == 0)
            return true;
    }
    return false;
}

/* what avrcp_rsp made of a line */
enum { PSL_OTHER = 0 /* not a status line */, PSL_OK = 1 /* a clean status / REJ */,
       PSL_FIXED = 2 /* the status word read, other bytes in the line: ask again to be sure */,
       PSL_BAD = 3 /* a status line that cannot be read: ask again */ };

/* `AVRCP {link_id} {PDU_ID name}_RSP [parsed data]` (RM 6.10.1 events; AN986 4.3 prints the controller's names with a
 * `PDU_` prefix): the reply to our `AVRCP PDU 31 1` (REGISTER_NOTIFICATION, event 1 PLAYBACK_STATUS_CHANGED) - INTERIM
 * right after the request with the current status, CHANGED once when it changes (then registered again: "all
 * notifications are triggered only once", AN986 4.3), `… _RSP REJ` = not supported.  Other AVRCP lines (passthrough
 * answers, `AVRCP 5103 ERROR_…`, the unparsed `RSP PDU_ID` form) change nothing.  Interleaved bytes (field 2026-11-09):
 * a cut / run-on word or bytes after it -> PSL_BAD / PSL_FIXED (the caller asks again). */
static int avrcp_rsp(bts_t *b, const toks_t *t, int l)
{
    int d = b->link_dev[l];
    const char *n = t->s[2];
    uint8_t nn = t->n[2];
    bts_dev_t *v;
    if (nn > 4 && !memcmp(n, "PDU_", 4)) {
        n += 4;
        nn = (uint8_t)(nn - 4);
    }
    if (d < 0 || nn != 25 || memcmp(n, "REGISTER_NOTIFICATION_RSP", 25) != 0)
        return PSL_OTHER;
    v = &b->dev[d];
    if (tok_is(t, 3, "REJ")) {
        if (v->ps_link == l + 1) {
            v->ps_pend = v->ps_live = 0;
            v->ps_none = 1;
        }
        return PSL_OK;
    }
    for (int i = 3; i < t->count; i++)
        if (tok_is(t, i, "PLAYBACK_STATUS_CHANGED")) {
            bool changed = tok_is(t, 3, "CHANGED");
            uint8_t ps = ps_word(t, i + 1);
            bool junk = t->count > i + 2 || (!changed && !tok_is(t, 3, "INTERIM")) ||
                        (ps == BTS_PS_UNKNOWN && ps_word_cut(t, i + 1));
            if (junk && ps == BTS_PS_UNKNOWN)
                return PSL_BAD;
            ps_set(b, d, ps);
            {
                int c = avrcp_link(b, d);
                v->ps_link = (uint8_t)((c >= 0 ? c : l) + 1);
            }
            v->ps_pend = 0;
            v->ps_none = 0;
            v->ps_live = !changed;   /* a CHANGED used the registration up: register again (ps_tick) */
            v->ps_wait = changed ? BTS_PS_DELAY_MS : 0;
            if (junk)
                return PSL_FIXED;
            v->ps_fix = 0;
            return PSL_OK;
        }
    return PSL_BAD;   /* REGISTER_NOTIFICATION_RSP without its event: a fragment */
}

/* ---- interleaved lines (field 2026-11-09) ---------------------------------------------- */
/* `<n>` (1..2 digits, < BTS_LINKS) at s, followed by the end or a space: its length, else 0 */
static uint32_t link_at(const char *s, uint32_t n)
{
    uint32_t k = 0, v = 0;
    while (k < n && k < 3 && s[k] >= '0' && s[k] <= '9')
        v = v * 10u + (uint32_t)(s[k++] - '0');
    if (k < 1 || k > 2 || (k < n && s[k] != ' ') || v >= BTS_LINKS)
        return 0;
    return k;
}

static bool at(const char *s, uint32_t n, const char *w)
{
    uint32_t m = (uint32_t)strlen(w);
    return n >= m && memcmp(s, w, m) == 0;
}

/* where a module line starts inside `line` after some other bytes: `AVRCP <n> <…_RSP…>`, `A2DP STREAMING START|STOP
 * <n>`, `RING <n> <addr>`, `NO CARRIER <n>` at an offset > 0 (our commands' echoes never have these forms: no link id
 * after STREAMING / AVRCP), else 0 */
static uint32_t embedded_start(const char *line, uint32_t len)
{
    for (uint32_t p = 1; p + 7 < len; p++) {
        const char *s = line + p;
        uint32_t n = len - p, k;
        if (at(s, n, "AVRCP ") && (k = link_at(s + 6, n - 6)) != 0 && 7 + k < n) {
            uint32_t e = 7 + k;
            while (e < n && s[e] != ' ')
                e++;
            for (uint32_t q = 7 + k; q + 4 <= e; q++)
                if (!memcmp(s + q, "_RSP", 4))
                    return p;
        }
        if (at(s, n, "A2DP STREAMING START ") && link_at(s + 21, n - 21))
            return p;
        if (at(s, n, "A2DP STREAMING STOP ") && link_at(s + 20, n - 20))
            return p;
        if (at(s, n, "RING ") && (k = link_at(s + 5, n - 5)) != 0 && 6 + k + 17 <= n && s[6 + k + 2] == ':')
            return p;
        if (at(s, n, "NO CARRIER ") && link_at(s + 11, n - 11))
            return p;
    }
    return 0;
}

/* a fragment of the play-status reply (its words cut apart by other bytes) - none of our commands' echoes has these */
static bool ps_fragment(const char *line, uint32_t len)
{
    static const char *const F[5] = {"REGISTER_N", "NOTIFICATION", "_RSP", "PLAYBACK_S", "STATUS_CHANGED"};
    for (int k = 0; k < 5; k++)
        for (uint32_t p = 0; p < len; p++)
            if (at(line + p, len - p, F[k]))
                return true;
    return false;
}

/* the play-status words only (not `_RSP`: other AVRCP answers have it) */
static bool ps_words(const char *line, uint32_t len)
{
    static const char *const F[4] = {"REGISTER_N", "NOTIFICATION", "PLAYBACK_S", "STATUS_CHANGED"};
    for (int k = 0; k < 4; k++)
        for (uint32_t p = 0; p < len; p++)
            if (at(line + p, len - p, F[k]))
                return true;
    return false;
}

/* the first `AVRCP <n>` in the line: the link, -1 */
static int avrcp_link_in(const char *line, uint32_t len)
{
    for (uint32_t p = 0; p + 7 <= len; p++) {
        uint32_t k;
        if (at(line + p, len - p, "AVRCP ") && (k = link_at(line + p + 6, len - p - 6)) != 0)
            return k == 1 ? line[p + 6] - '0' : (line[p + 6] - '0') * 10 + (line[p + 7] - '0');
    }
    return -1;
}

static bool ps_on(const bts_t *b);
static void ev(bts_t *b, const char *text);

/* a play-status line that could not be read (or was read with other bytes in it): ask that link's device again - no
 * link readable: every device whose registration is live or waiting for its answer.  lost = nothing was read: that
 * device's status is unknown until the answer (a PAUSE is not skipped on a stale "paused" - the field bug).  At most
 * BTS_PS_FIX_MAX times per device until a clean status line. */
static void ps_garbled(bts_t *b, int l, bool lost)
{
    if (!ps_on(b))
        return;
    if (b->n.garbled < 0xFFFFu)
        b->n.garbled++;
    for (int i = 0; i < BTS_DEV_MAX; i++) {
        bts_dev_t *v = &b->dev[i];
        if (!v->used || avrcp_link(b, i) < 0)
            continue;
        if (l >= 0 ? b->link_dev[l] != i : !(v->ps_live || v->ps_pend))
            continue;
        if (v->ps_fix >= BTS_PS_FIX_MAX)
            continue;
        v->ps_fix++;
        if (lost && l >= 0)
            v->ps = BTS_PS_UNKNOWN;
        v->ps_live = v->ps_pend = v->ps_none = 0;
        v->ps_wait = BTS_PS_DELAY_MS;   /* ps_tick registers again at the next empty queue */
        ev(b, "garbled: ask again");   /* (before `garbled status: ask again`) */
    }
}

/* `NO CARRIER <l> ERROR 0` on an A2DP link of device d: closed by the source itself (not our CLOSE - those set
 * closing / a2dp_closing / parked; a link loss has another code) after a normal connection (BTS_RC_STABLE_MS) - the
 * user disconnected it there (field 2026-11-09: an Android TV disconnected in its own settings, then called by the piano
 * every ~5 s).  The selected one stops being the "last selected" reconnect target */
static void a2dp_closed_by_source(bts_t *b, int d, const toks_t *t)
{
    const bts_dev_t *v = &b->dev[d];
    if (!v->addr_known || v->a2dp_ms < BTS_RC_STABLE_MS || v->closing || v->a2dp_closing || v->parked ||
        !tok_is(t, 3, "ERROR") || !tok_is(t, 4, "0") || !b->has_sel || b->sel_norc || memcmp(v->addr, b->sel_addr, 6))
        return;
    b->sel_norc = 1;
    bts_log(b, BTS_L_EVENT, "sel left: no recon", 18);   /* (before `sel closed by it: no recon`) */
}

/* the echo of our `SET <l> SELECT`: whole, or with its head glued to the line before it (field
 * 2026-11-13: `> SET 3 SELECT` -> `< T 3 SELECT`, the `SE` went with the line before) or other bytes in front - the
 * last two words `<l> SELECT` with our l, before them nothing, a tail of `SET` or a word ending in `SET`.  No module line
 * but this echo ends in `<l> SELECT` */
static bool sel_echo(const bts_t *b, const toks_t *t)
{
    int c = t->count;
    uint8_t k;
    if (c < 2 || !tok_is(t, c - 1, "SELECT") || tok_num(t, c - 2, BTS_LINKS - 1) != b->sel_lnk)
        return false;
    if (c == 2)
        return true;
    k = t->n[c - 3];
    if (c == 3 && k <= 3 && memcmp(t->s[0], "SET" + 3 - k, k) == 0)
        return true;
    return k >= 3 && memcmp(t->s[c - 3] + k - 3, "SET", 3) == 0;
}

void bts_line(bts_t *b, const char *line, uint32_t len)
{
    toks_t t;
    int l;
    uint8_t a[6];
    uint32_t emb;
    tokenize(line, len, &t);
    if (!t.count)
        return;
    if (b->sel_out && sel_echo(b, &t)) {
        b->sel_out = 0;                /* the module took our `SET <l> SELECT`: its paired command may go now */
        b->gap_ms = BTS_CMD_GAP_MS;
    }
    if (!b->set_listing && !(tok_is(&t, 0, "SET") && tok_is(&t, 1, "BT")) && (emb = embedded_start(line, len)) != 0) {
        /* a module line after other bytes (field 2026-11-09: `SAVRCP 6 … PLAYING` = our echo's `S` + the phone's
         * CHANGED; `SETA2DP STREAMING STOP 4`): read it from there */
        if (b->n.recovered < 0xFFFFu)
            b->n.recovered++;
        bts_line(b, line + emb, len - emb);
        return;
    }
    if (b->n.lines < 0xFFFFu)
        b->n.lines++;
    b->waiting = 0;   /* the module is alive (an echo is enough) */
    b->no_answer = 0;
    if (b->listing && !tok_is(&t, 0, "LIST"))
        list_finish(b);   /* the reply ended early */
    if (!b->set_listing && !tok_is(&t, 0, "AVRCP") && ps_fragment(line, len)) {
        /* a piece of a play-status reply cut apart by other bytes (`SET 1 SAVRCP 6 REGISTER_NOTIF…`, `…ICATION_RSP
         * CHANGED …`): nothing can be read from it - ask again */
        ps_garbled(b, avrcp_link_in(line, len), true);
        return;
    }

    if ((t.count == 1 && tok_is(&t, 0, "READY.")) || (t.count >= 2 && tok_is(&t, 0, "WRAP"))) {
        /* the iWRAP start banner (`WRAP THOR AI (6.2.0 build 1135)`) / READY. while the module stayed powered: it
         * rebooted by itself (field 2026-11-02) - not the start after power-on (ready_ms counts from that edge) */
        if (b->module_on && b->ready_ms >= BTS_BOOT_GRACE_MS)
            module_rebooted(b);
        module_restarted(b);
        return;
    }
    if (tok_is(&t, 0, "SET")) {
        set_line(b, &t, line, len);
        return;
    }
    if (t.count == 2 && tok_is(&t, 0, "CALL") && (l = tok_num(&t, 1, BTS_LINKS - 1)) >= 0) {
        /* RM 6.16: `CALL {link_id}` answers our CALL - the link is that address's (the echo has 4 tokens) */
        if (b->call_on) {
            if (b->link_dev[l] >= 0)
                link_drop(b, l);
            link_set(b, l, b->call_addr, BTS_P_NONE, 0);
            if (b->call_avrcp) {
                b->link_ch[l] = 0x17;   /* the control channel we asked for (its CONNECT confirms the profile) */
                b->av_lid = (int8_t)l;  /* no CONNECT on it within BTS_AVRCP_GIVEUP_MS: closed (avrcp_giveup) */
                b->av_ms = 0;
            }
        }
        return;
    }
    if (tok_is(&t, 0, "LIST")) {
        if (t.count == 2 && (l = tok_num(&t, 1, 99)) >= 0) {   /* LIST <n> */
            b->listing = 1;
            b->list_left = (uint8_t)l;
            b->list_seen = 0;
            if (!l)
                list_finish(b);
        } else if (t.count >= 4 && tok_is(&t, 2, "CONNECTED") && (l = tok_num(&t, 1, BTS_LINKS - 1)) >= 0) {
            int inc = -1;
            bool have = false;
            uint8_t ch = 0;
            for (int i = 4; i < t.count; i++) {
                if (!have && tok_addr(&t, i, a)) {
                    have = true;
                    ch = tok_hex(&t, i + 1);
                }
                if (tok_is(&t, i, "INCOMING"))
                    inc = 1;
                else if (tok_is(&t, i, "OUTGOING"))
                    inc = 0;
            }
            if (b->link_dev[l] >= 0 && have && b->dev[b->link_dev[l]].addr_known &&
                memcmp(b->dev[b->link_dev[l]].addr, a, 6) != 0)
                link_drop(b, l);   /* the id was reused by another device */
            link_set(b, l, have ? a : 0, prof_of(&t, 3), inc);
            if (ch && b->link_dev[l] >= 0)
                b->link_ch[l] = ch;
            b->list_seen = (uint16_t)(b->list_seen | (1u << l));
            if (b->listing && b->list_left && --b->list_left == 0)
                list_finish(b);
        }
        return;
    }
    if (tok_is(&t, 0, "RING") && (l = tok_num(&t, 1, BTS_LINKS - 1)) >= 0) {
        bool have = tok_addr(&t, 2, a);
        uint8_t p = BTS_P_OTHER;
        for (int i = 3; i < t.count; i++)
            if (prof_of(&t, i) == BTS_P_A2DP || prof_of(&t, i) == BTS_P_AVRCP)
                p = prof_of(&t, i);
        if (b->link_dev[l] >= 0)
            link_drop(b, l);
        link_set(b, l, have ? a : 0, p, 1);
        if (b->link_dev[l] >= 0) {
            int d = b->link_dev[l];
            b->link_ch[l] = tok_hex(&t, 3);
            if (p == BTS_P_AVRCP && b->link_ch[l] == 0x1b && have && avrcp_link(b, d) < 0) {
                /* AVRCP browsing without the control channel (field 2026-11-02: the module rebooted ~1 s later) */
                memcpy(b->orphan_addr, a, 6);
                b->has_orphan = 1;
                b->orphan_ago = 0;
                TSTAT(orphans);
                if (b->pol.mode == BTS_MODE_SELECT && !(b->pol.flags & BTS_F_NO_AVRCP_CALL)) {
                    /* field 2026-11-09: a freshly paired Windows PC did this on every connect, `AVRCP 5103
                     * ERROR_NO_CONTROL_CHANNEL` and the module reboot followed; our `CALL … 17 AVRCP` got no CONNECT in
                     * time.  The browsing channel alone "can be ignored or closed" (AN986 3.5.2): close that
                     * one link at once, ahead of the queue - a run-time CLOSE of the browsing link only (its A2DP and
                     * any control link stay); whether it beats the reboot is for the field (D41) */
                    char c[10] = "CLOSE ";
                    c[6 + put_num(c + 6, (unsigned)l & 15u)] = 0;
                    if (q_push_front(b, c)) {
                        TSTAT(orphan_closes);
                        bts_log(b, BTS_L_EVENT, "lone 1b: CLOSE", 14);
                    }
                }
            }
        }
        if (!have)
            b->list_want = 1;
        return;
    }
    if (tok_is(&t, 0, "CONNECT") && (l = tok_num(&t, 1, BTS_LINKS - 1)) >= 0) {
        int d = b->link_dev[l];
        uint8_t p = prof_of(&t, 2);
        if (d < 0 || !b->dev[d].addr_known) {
            b->list_want = 1;   /* LIST confirms (an AUTOCALL link may come at the same time) */
            if (b->call_on && p == BTS_P_A2DP && d < 0) {
                link_set(b, l, b->call_addr, p, 0);   /* the second A2DP channel of our CALL (AN986 3.5.1) */
                d = b->link_dev[l];
            }
        }
        link_set(b, l, 0, p, -1);
        if (b->link_dev[l] >= 0)
            b->link_ch[l] = tok_hex(&t, 3);
        return;
    }
    if (tok_is(&t, 0, "NO") && tok_is(&t, 1, "CARRIER") && (l = tok_num(&t, 2, BTS_LINKS - 1)) >= 0) {
        int d = b->link_dev[l];
        if (b->call_on && d >= 0 && b->link_prof[l] == BTS_P_NONE && b->dev[d].addr_known &&
            memcmp(b->dev[d].addr, b->call_addr, 6) == 0)
            b->call_on = 0;   /* our CALL failed */
        if (d >= 0 && b->link_prof[l] == BTS_P_A2DP)
            a2dp_closed_by_source(b, d, &t);
        link_drop(b, l);
        return;
    }
    if (tok_is(&t, 0, "AVRCP") && t.count >= 3 && (l = tok_num(&t, 1, BTS_LINKS - 1)) >= 0) {
        int r = avrcp_rsp(b, &t, l);
        if (r >= PSL_FIXED || (r == PSL_OTHER && ps_fragment(line, len)))
            ps_garbled(b, l, r != PSL_FIXED);
        return;
    }
    if (tok_is(&t, 0, "AVRCP") && ps_words(line, len)) {
        /* a play-status line without its link id (field 2026-11-09 evening: `AVRCP  REGISTER_NOTIFICATION_RSP CHANGED
         * PLAYBACK_STATUS_CHANGED PLAYING` - the id lost between our echoes): whose it is cannot be told, and that
         * device's one-shot registration is used up - every registered device is asked again */
        ps_garbled(b, -1, true);
        return;
    }
    if (tok_is(&t, 0, "AUDIO") && tok_is(&t, 1, "ROUTE")) {
        /* RM 6.67 `AUDIO ROUTE {link_id} {type} {channels}` (only with the module's EVENT flag) "when the audio routings
         * are changed; e.g. when an A2DP or SCO connection is started or closed": the link the module routes - no
         * stream event (the stream lines tell those); a line without a link id is kept in the log only */
        if ((l = tok_num(&t, 2, BTS_LINKS - 1)) >= 0) {
            b->route = (uint8_t)(l + 1);
            b->route_a2dp = tok_is(&t, 3, "A2DP");
        }
        return;
    }
    if (tok_is(&t, 0, "A2DP") && tok_is(&t, 1, "CODEC") && t.count >= 3) {
        /* RM 6.67: `A2DP CODEC {codec} {channel_mode} {rate} BITPOOL 2-250` when a codec is loaded into the DSP, e.g.
         * when an A2DP stream starts - no link id: it belongs to the stream that starts with it / plays now */
        int d = b->active;
        b->codec = tok_is(&t, 2, "SBC") ? BTS_CODEC_SBC : tok_is(&t, 2, "AAC") ? BTS_CODEC_AAC
                 : tok_is(&t, 2, "APT-X") ? BTS_CODEC_APTX : tok_is(&t, 2, "APT-X_LL") ? BTS_CODEC_APTX_LL : BTS_CODEC_OTHER;
        b->codec_rate = tok_is(&t, 4, "44100") ? 0 : tok_is(&t, 4, "48000") ? 1 : 2;
        b->codec_ago = 0;
        b->codec_dev = 0xFF;
        if (d >= 0 && b->dev[d].used && b->dev[d].streaming) {
            b->dev[d].codec = b->codec;
            b->codec_dev = (uint8_t)d;
        }
        return;
    }
    if (tok_is(&t, 0, "A2DP") && tok_is(&t, 1, "STREAMING")) {
        int d;
        l = tok_num(&t, 3, BTS_LINKS - 1);
        if (l < 0 && t.count == 3) {
            uint8_t e = tok_is(&t, 2, "START") ? 1u : tok_is(&t, 2, "STOP") ? 2u : 0u;
            if (e & b->echo_ss) {        /* our own command echoed (no link id): no stream event */
                b->echo_ss &= (uint8_t)~e;
                return;
            }
        }
        d = l >= 0 ? b->link_dev[l] : -1;
        b->ho.ev = 1;                    /* a stream event after a live apply: newer than the handed-over state */
        if (l >= 0 && d < 0) {           /* a link we have not heard of: learn it */
            link_set(b, l, 0, BTS_P_A2DP, -1);
            d = b->link_dev[l];
            b->list_want = 1;
        }
        if (tok_is(&t, 2, "START")) {
            if (d < 0) {                 /* no link id: the source that connected last */
                d = newest_a2dp_dev(b);
                b->list_want = 1;
            }
            if (d >= 0) {
                if (b->codec && b->codec_ago < 1500u) {   /* the codec line came with this start */
                    b->dev[d].codec = b->codec;
                    b->codec_dev = (uint8_t)d;
                }
                if (b->dev[d].fresh || (b->call_on && !b->call_avrcp && b->dev[d].addr_known &&
                                        memcmp(b->dev[d].addr, b->call_addr, 6) == 0))
                    b->dev[d].fresh = 1;     /* a stream of a link we CALLed (Windows opens it while paused) or one the
                                              * sink started: says nothing of its player until it lasted
                                              * BTS_SW_STEADY_MS (field 2026-11-03: the phone's START, STOP 10 ms later) */
                else {
                    if (!b->dev[d].streaming)
                        b->dev[d].ps_ours = 0;   /* it started by itself on a link it had: its player plays */
                    b->dev[d].ps_old = 1;        /* its last play-status report is older than this start */
                }
                b->dev[d].streaming = 1;
                b->dev[d].held = 0;
                b->dev[d].susp = 0;
                b->dev[d].stream_ms = 0;
                b->dev[d].seq = ++b->seq;
                b->dev[d].swept = 0;
                b->active = (int8_t)d;
            }
            TSTAT(starts);
            if (d >= 0 && b->sw_phase == BTS_SW_IDLE && b->sw_result == BTS_RES_READY && (b->sw_tried & BTS_SWT_PLAY) &&
                b->sw_ago < BTS_SW_LATE_MS && b->has_sel && memcmp(b->sel_addr, b->sw_addr, 6) == 0 &&
                dev_by_addr(b, b->sw_addr) == d) {
                /* the target we sent PLAY opened its stream after the switch said "ready" (field 2026-11-11:
                 * the phone 1.4 s after PLAY): it plays after all - the app's 「시작하지 못했습니다」 goes */
                b->sw_result = BTS_RES_PLAYING;
                b->sw_last_ms += b->sw_ago;
                b->sw_ago = 0;
                ev(b, "late START: playing");
            }
        } else if (tok_is(&t, 2, "STOP")) {
            if (d < 0) {
                for (int i = 0; i < BTS_DEV_MAX; i++)
                    if (b->dev[i].used && b->dev[i].streaming && (d < 0 || b->dev[i].seq > b->dev[d].seq))
                        d = i;
            }
            if (d >= 0) {
                int s2 = dev_by_addr(b, b->sw_addr), l2;
                bool was = b->dev[d].streaming != 0;   /* (a second STOP of a stopped stream says nothing) */
                b->dev[d].streaming = 0;
                b->dev[d].held = 0;
                b->dev[d].stream_ms = 0;
                b->dev[d].refusing = 0;
                if (was && b->sw_phase == BTS_SW_IDLE && b->sw_result == BTS_RES_PLAYING &&
                    !(b->sw_reassert & BTS_RA_LATE) && b->sw_ago < BTS_SW_REASSERT_MS && s2 >= 0 && s2 != d && b->dev[s2].streaming &&
                    avrcp_link(b, s2) >= 0 &&
                    b->pol.mode == BTS_MODE_SELECT && (l2 = a2dp_link(b, s2)) >= 0 &&
                    q_on_link(b, l2, "A2DP STREAMING START")) {
                    /* another source's STOP after the switch put the target on (field 2026-11-09 evening: the left
                     * phone's own STOP 3 s after the PC's START - the piano silent): the output follows the last stream
                     * event [inference] - point it at the target again, once */
                    b->sw_reassert |= BTS_RA_LATE;
                    bts_log(b, BTS_L_EVENT, "late STOP: re-assert START", 26);
                }
            }
        }
        return;
    }
}

/* the summary event of a `SET` listing kept out of the line log: `SET list: <n> lines` (put together from "SET" and
 * " list: " - no `SET <word>` literal in the payload, see bts_rx) */
static void set_list_logged(bts_t *b, const char *tail)
{
    char e[40];
    uint32_t k = 0, n = b->set_lines;
    const char *h;
    for (h = "SET"; *h;)
        e[k++] = *h++;
    for (h = " list: "; *h;)
        e[k++] = *h++;
    if (n >= 100)
        e[k++] = (char)('0' + n / 100);
    if (n >= 10)
        e[k++] = (char)('0' + n / 10 % 10);
    e[k++] = (char)('0' + n % 10);
    for (h = n == 1 ? " line" : " lines"; *h;)
        e[k++] = *h++;
    while (*tail && k < sizeof e)
        e[k++] = *tail++;
    b->set_lines = 0;
    bts_log(b, BTS_L_EVENT, e, k);
}

void bts_rx(bts_t *b, const uint8_t *bytes, uint32_t n)
{
    for (uint32_t i = 0; i < n; i++) {
        uint8_t c = bytes[i];
        if (c == '\r' || c == '\n') {
            /* every line, before it is acted on - a pairing's link key (`SET BT PAIR <addr> <key>`) is cut off.  The
             * lines of a `SET` listing (`SET BT BDADDR …` .. the bare `SET`) are parsed as before but not logged: one
             * summary event when it ends (the power-on read, pushed the first lines of a
             * live apply out of the ring) */
            /* (no `SET <word>` literal in the payload: roundtrip_bt.py 1 reads every such string as a write form) */
            uint8_t in_list = b->len >= 3 && memcmp(b->line, "SET", 3) == 0 &&
                              (b->set_listing || (b->len >= 13 && b->line[3] == ' ' && memcmp(b->line + 4, "BT BDADDR", 9) == 0));
            if (in_list) {
                if (!b->set_listing)
                    b->set_lines = 0;
                if (b->set_lines < 255)
                    b->set_lines++;
            } else if (b->len >= 29 && memcmp(b->line, "SET BT PAIR ", 12) == 0)
                bts_log(b, BTS_L_TRUNC, b->line, 29);
            else if (b->len)
                bts_log(b, b->ovf ? BTS_L_TRUNC : 0, b->line, b->len);
            if (!b->ovf && b->len >= 2)
                bts_line(b, b->line, b->len);
            if (in_list && !b->set_listing)
                set_list_logged(b, "");
            b->len = 0;
            b->ovf = 0;
        } else if (b->len < BTS_LINE_MAX) {
            b->line[b->len++] = (char)c;
        } else {
            b->ovf = 1;
        }
    }
}

/* ---- policy tick -------------------------------------------------------------------------------- */
int bts_streams(const bts_t *b)
{
    int n = 0;
    for (int i = 0; i < BTS_DEV_MAX; i++)
        n += b->dev[i].used && b->dev[i].streaming;
    return n;
}

static bool is_pref(const bts_t *b, const bts_dev_t *v)
{
    return b->pol.has_pref && v->addr_known && memcmp(v->addr, b->pol.pref, 6) == 0;
}

/* CALL_PREF (AUTOCALL on or not known): the preferred device after the module (re)starts, at most twice */
static void pref_call(bts_t *b)
{
    if ((b->pol.flags & BTS_F_CALL_PREF) && b->pol.has_pref && b->ac_state != BTS_AC_OFF && !bts_a2dp_links(b) && !b->loop &&
        b->ready_ms >= BTS_PREF_WAIT_MS && b->pref_tries < 2 && b->pref_wait >= BTS_PREF_RETRY_MS) {
        b->pref_tries++;
        b->pref_wait = 0;
        q_call(b, b->pol.pref);
    }
}

#ifndef BTS_NO_NEWEST
static void policy(bts_t *b)
{
    int win = -1, nstream = bts_streams(b);
    if (nstream >= 2) {
        /* the newest stream, unless it is a source closed a moment ago that came back (no ping-pong) */
        for (int pass = 0; pass < 2 && win < 0; pass++)
            for (int i = 0; i < BTS_DEV_MAX; i++) {
                bts_dev_t *v = &b->dev[i];
                if (!v->used || !v->streaming || v->closing)
                    continue;
                if (pass == 0 && closed_within(b, v, BTS_REJOIN_MS))
                    continue;
                if (win < 0 || v->seq > b->dev[win].seq)
                    win = i;
            }
        if (win >= 0 && b->dev[win].stream_ms >= (uint32_t)b->pol.hold_ds * 100u) {
            for (int i = 0; i < BTS_DEV_MAX; i++)
                if (i != win && b->dev[i].used && b->dev[i].streaming && !b->dev[i].closing)
                    close_dev(b, i);
            b->active = (int8_t)win;
            b->dev[win].swept = 1;   /* the others that were streaming are being closed */
        }
        return;
    }
    if (nstream == 1 && (b->pol.flags & BTS_F_CLOSE_OTHERS)) {
        for (int i = 0; i < BTS_DEV_MAX; i++)
            if (b->dev[i].used && b->dev[i].streaming)
                win = i;
        if (win >= 0 && !b->dev[win].swept && b->dev[win].stream_ms >= (uint32_t)b->pol.sweep_s * 1000u) {
            b->dev[win].swept = 1;
            for (int i = 0; i < BTS_DEV_MAX; i++) {
                bts_dev_t *v = &b->dev[i];
                if (i == win || !v->used || !v->links || v->closing)
                    continue;
                if ((b->pol.flags & BTS_F_KEEP_PREF) && is_pref(b, v))
                    continue;
                if (closed_within(b, v, BTS_BACKOFF_MS))
                    continue;   /* closed once already: do not fight a source that keeps coming back */
                close_dev(b, i);
            }
        }
    }
    pref_call(b);
}
#endif

int bts_sel_dev(const bts_t *b)
{
    return b->has_sel ? dev_by_addr(b, b->sel_addr) : -1;
}

static void select_dev(bts_t *b, int d)
{
    memcpy(b->sel_addr, b->dev[d].addr, 6);
    b->has_sel = 1;
    b->sel_norc = 0;
    b->active = (int8_t)d;
    b->dev[d].refusing = 0;
    b->dev[d].refuse_n = 0;
}

/* every source other than the switch target that streams has no AVRCP control link (q_suspend leaves it alone) */
static bool held_only(const bts_t *b, int d)
{
    for (int i = 0; i < BTS_DEV_MAX; i++)
        if (i != d && b->dev[i].used && b->dev[i].streaming && avrcp_link(b, i) >= 0)
            return false;
    return true;
}

/* the sources other than the switch target that stream */
static int others_streaming(const bts_t *b, int d)
{
    int n = 0;
    for (int i = 0; i < BTS_DEV_MAX; i++)
        n += i != d && b->dev[i].used && b->dev[i].streaming;
    return n;
}

static void ev(bts_t *b, const char *text) { bts_log(b, BTS_L_EVENT, text, (uint32_t)strlen(text)); }

static bool ps_on(const bts_t *b) { return b->pol.mode == BTS_MODE_SELECT && !(b->pol.flags & BTS_F_NO_PS); }

/* the play status: `SET <l> SELECT` + `AVRCP PDU 31 1` (RM 6.10 / AN986 6.3: PDU 0x31 RegisterNotification, event 0x01
 * playback status changed) on every AVRCP control link, BTS_PS_DELAY_MS after it came; the INTERIM answer carries the
 * status now, a CHANGED the new one (then again).  No answer within BTS_PS_ANSWER_MS / REJ: not this link again - the
 * "paused by us" rule decides.  Mode SELECT only, never with the opt-out flag; nothing is stored in the module. */
static void ps_tick(bts_t *b, uint32_t ms)
{
    if (!ps_on(b))
        return;
    for (int i = 0; i < BTS_DEV_MAX; i++) {
        bts_dev_t *v = &b->dev[i];
        int l;
        if (!v->used || (l = avrcp_link(b, i)) < 0)
            continue;
        if (v->ps_link != l + 1) {   /* a new control link: nothing known on it yet */
            v->ps_link = (uint8_t)(l + 1);
            v->ps = BTS_PS_UNKNOWN;
            v->ps_pend = v->ps_live = v->ps_none = 0;
            v->ps_wait = 0;
            v->ps_fix = 0;
        }
        if (v->ps_none || v->ps_live)
            continue;
        if (v->ps_pend) {
            if (!b->q_n && (v->ps_wait += ms) >= BTS_PS_ANSWER_MS) {   /* counted once the commands are out */
                v->ps_pend = 0;
                v->ps_none = 1;
                ev(b, "no play status");
            }
            continue;
        }
        if (v->ps_wait < BTS_PS_DELAY_MS && !(b->sw_phase != BTS_SW_IDLE && v->addr_known &&
                                               memcmp(v->addr, b->sw_addr, 6) == 0)) {
            v->ps_wait += ms;   /* (the switch target: at once - its PLAY waits for this) */
            continue;
        }
        if (b->q_n)
            return;   /* only into an empty queue: other commands first */
        if (q_on_link(b, l, "AVRCP PDU 31 1")) {
            v->ps_pend = 1;
            v->ps_wait = 0;
        }
        return;   /* one registration at a time */
    }
}

/* our AVRCP PAUSE and no play-status report within BTS_PZ_RECHECK_MS (counted once the commands are out): its one-shot
 * registration may have been used up / its CHANGED lost (field 2026-11-10: the phone paused, no CHANGED PAUSED) - register
 * again, once per PAUSE; the INTERIM tells its status now */
static void pz_recheck(bts_t *b, uint32_t ms)
{
    if (!ps_on(b))
        return;
    for (int i = 0; i < BTS_DEV_MAX; i++) {
        bts_dev_t *v = &b->dev[i];
        int l;
        if (!v->used || !v->pz_wait)
            continue;
        if ((l = avrcp_link(b, i)) < 0 || v->ps_none || v->ps_link != l + 1) {
            v->pz_wait = 0;   /* no link / it does not report / a new link (ps_tick registers that one) */
            continue;
        }
        if (b->q_n || (v->pz_ms += ms) < BTS_PZ_RECHECK_MS)
            continue;
        if (v->ps_pend) {
            v->pz_wait = 0;   /* a registration is out already */
            continue;
        }
        if (q_on_link(b, l, "AVRCP PDU 31 1")) {
            v->pz_wait = 0;
            v->ps_pend = 1;
            v->ps_live = 0;
            v->ps_wait = 0;
            ev(b, "PAUSE unreported: ask");
        }
        return;   /* one at a time */
    }
}

/* it reports paused / stopped - and that report is newer than its stream (a stream it started by itself after the
 * report: its player plays, whatever the report said - field 2026-11-09 evening, the CHANGED PLAYING lost) */
static bool ps_paused(const bts_dev_t *v)
{
    return (v->ps == BTS_PS_PAUSED || v->ps == BTS_PS_STOPPED) && !(v->streaming && v->ps_old);
}

/* AVRCP PLAY to device d?  1 yes, 0 no (*why = BTS_SWT_KEPT / BTS_SWT_NOT_OURS), -1 wait (its status comes right after
 * the registration - bounded by BTS_PS_DELAY_MS + BTS_PS_ANSWER_MS).  Windows takes the PLAY passthrough as play/pause
 * (field 2026-11-02 night: PLAY to a PC that played paused it), so: PLAY to one the piano paused itself, nothing since
 * showing it playing (first - field 2026-11-10); else only to a source that reports paused / stopped. */
static int play_wanted(bts_t *b, int d, uint8_t *why)
{
    bts_dev_t *v = &b->dev[d];
    if (v->ps_ours && v->ps != BTS_PS_UNKNOWN)
        return 1;   /* the piano paused it and nothing since showed it playing (a PLAYING report or its own start clears
                     * this): PLAY, whatever its last report said - field 2026-11-10: the phone the piano had
                     * paused sent no CHANGED, its stale PLAYING skipped the PLAY - silence.  A new link (status not
                     * known yet): its INTERIM first, as before */
    if (v->ps == BTS_PS_PLAYING || v->ps == BTS_PS_SEEKING ||
        ((v->ps == BTS_PS_PAUSED || v->ps == BTS_PS_STOPPED) && !ps_paused(v))) {   /* its stream newer than the report */
        *why = BTS_SWT_KEPT;
        return 0;
    }
    if (ps_paused(v))
        return 1;
    if (ps_on(b) && avrcp_link(b, d) >= 0 && !v->ps_none && !v->ps_live)
        return -1;
    if (v->ps_ours)
        return 1;
    *why = BTS_SWT_NOT_OURS;
    return 0;
}

static void play_skipped(bts_t *b, uint8_t why)
{
    TSTAT(play_skips);
    ev(b, why == BTS_SWT_KEPT ? "no PLAY: it plays" : "no PLAY: not paused by us");
}

/* AVRCP PAUSE to a source that streams?  Not when it reports paused / stopped (a Windows stream stays open ~10 s after a
 * pause - PAUSE would be play/pause and start it again); unknown: yes, it streams. */
static bool pause_wanted(bts_t *b, int d)
{
    if (!ps_paused(&b->dev[d]))
        return true;   /* playing / unknown / reported paused before its stream started (a PAUSE to a paused one: harmless) */
    TSTAT(pause_skips);
    ev(b, "no PAUSE: it is paused");
    return false;
}

/* the target is linked but silent: the sink starts its stream itself - `SET <A2DP signalling link> SELECT` + `A2DP
 * STREAMING START` (AN986 6.1 table 1; RN 6.5.2 routes A2DP commands to the selected A2DP link).  Never to a source
 * without an AVRCP control link (field 2026-11-10 night: the paused Windows PC (AVRCP off)
 * answered the sink's START with `STREAMING START 0`, and when it played later the piano got nothing for minutes - only a
 * Windows PnP restart helped; the stream it opened itself when it played had sound): the switch ends READY - the app
 * says "press play on that device", whose own start opens its stream. */
static void sw_sink_start(bts_t *b, int d)
{
    int l = a2dp_link(b, d);
    if (avrcp_link(b, d) < 0) {
        sw_end(b, BTS_RES_READY);
        return;
    }
    if (l < 0 || !q_on_link(b, l, "A2DP STREAMING START")) {
        sw_end(b, BTS_RES_NO_LINK);
        return;
    }
    b->dev[d].fresh = 1;   /* the stream that follows is the sink's doing, not its player's */
    b->sw_tried |= BTS_SWT_START;
    b->sw_phase = BTS_SW_START;
    b->sw_ms = 0;
}

/* the source whose lone browsing channel preceded a module reboot: the sink never opens its AVRCP first */
static bool is_suspect(const bts_t *b, const bts_dev_t *v)
{
    return b->has_suspect && v->addr_known && memcmp(v->addr, b->suspect, 6) == 0;
}

/* device d's AVRCP control link may still come: our `CALL … 17 AVRCP` to it is out, or the sink may open one
 * (avrcp_open: mode SELECT, not opted out, no reboot loop, not the suspect) and has not in this A2DP episode */
static bool avrcp_coming(const bts_t *b, int d)
{
    const bts_dev_t *v = &b->dev[d];
    if (!v->addr_known)
        return false;
    if (b->call_on && b->call_avrcp && memcmp(v->addr, b->call_addr, 6) == 0)
        return true;
    return !v->avrcp_called && b->pol.mode == BTS_MODE_SELECT && !(b->pol.flags & BTS_F_NO_AVRCP_CALL) && !b->loop &&
           !is_suspect(b, v);
}

/* it reports paused / stopped and still streams: Windows keeps a paused stream open (minutes) and puts no sound on it
 * after the sink suspended and resumed it (field 2026-11-09) - never suspended */
static bool paused_open(const bts_t *b, const bts_dev_t *v)
{
    return !b->susp_paused && v->streaming && ps_paused(v);
}

/* making the source that plays (device i, not the target) let go - never a CLOSE (field 2026-11-03): AVRCP PAUSE by the play-status rules (the switch / the guard); no AVRCP to pause it: q_suspend
 * (host model only - the cave never stops it) */
static void leave(bts_t *b, int i)
{
    if (pause_wanted(b, i) && q_avrcp(b, i, false) == BTS_NO_AVRCP)
        q_suspend(b, i);
}

/* the PLAY decision on the target (its A2DP link up, no stream yet): 1 PLAY sent / not needed (phase PLAY follows), 0
 * nothing to decide on (no AVRCP, or decided already), -1 wait (its play status comes right after the registration) */
static int sw_decide_play(bts_t *b, int d)
{
    uint8_t why = 0;
    int w;
    if (avrcp_link(b, d) < 0 || b->sw_pl)
        return 0;
    w = play_wanted(b, d, &why);
    if (w < 0)
        return -1;
    if (w > 0 && q_avrcp(b, d, true) != BTS_OK)
        return 0;
    /* PLAY sent - or not needed / not ours to send (it plays already, or we did not pause it): either way its own start
     * gets BTS_SW_PLAY_MS before the sink starts the stream (that START is no toggle) */
    b->sw_pl = 1;
    b->sw_tried |= w ? BTS_SWT_PLAY : why;
    if (!w)
        play_skipped(b, why);
    b->sw_phase = BTS_SW_PLAY;
    b->sw_ms = 0;
    return 1;
}

/* the switch (USE): one step per tick; phase time counts only while no command is queued (the commands are out) */
static void sw_step(bts_t *b, uint32_t ms)
{
    int d = dev_by_addr(b, b->sw_addr), o = others_streaming(b, d);
    if (b->sw_phase == BTS_SW_IDLE) {
        /* a switch that ended BUSY waiting for the others' own STOP: once none streams, it goes on (within
         * BTS_SW_RESUME_MS, its target still the selected one and known), else the stale BUSY is cleared (field 2026-11-10
         * night: the PC let go 20 s after its pause, after the BUSY; the app kept 「다른 기기가 소리를 놓지
         * 않습니다」) */
        if (b->sw_result == BTS_RES_BUSY && b->sw_held && !o) {
            b->sw_held = 0;
            b->sw_result = BTS_RES_NONE;
            if (d >= 0 && b->sw_ago < BTS_SW_RESUME_MS && b->has_sel && memcmp(b->sel_addr, b->sw_addr, 6) == 0) {
                b->sw_phase = BTS_SW_LINK;
                b->sw_ms = 0;
                ev(b, "let go: on");
            }
        }
        return;
    }
    if (b->sw_total < 0x80000000u)
        b->sw_total += ms;
    if (d >= 0 && b->dev[d].streaming && b->sw_phase >= BTS_SW_LINK) {
        /* its stream is up - that alone is no sound: Windows opens the stream of a fresh A2DP link while its player
         * stays paused (field 2026-11-02 afternoon), a resumed stream of a player we paused is silent too.  So the PLAY
         * decision once on this link (no AVRCP: nothing more the piano can send).  The result is "stream up (+ PLAY
         * sent)", not "it sounds" - the module cannot tell. */
        bool av = avrcp_link(b, d) >= 0;
        if ((b->sw_reassert & BTS_RA_PRE) && !(b->sw_tried & BTS_SWT_START) && !b->sw_pl && av && !b->dev[d].ps_ours) {
            /* its stream predates this pick: the module may be playing another (or nothing) - the sink's START on its
             * link points the output at it (an open stream: only echoed, field 2026-11-09 evening - harmless).  Not to
             * one the piano paused that has AVRCP: PLAY opens its stream (field 2026-11-10: this START made the
             * paused phone's stream start and stop, the PLAY skipped); never one without AVRCP */
            int l = a2dp_link(b, d);
            b->sw_tried |= BTS_SWT_START;
            if (l >= 0 && q_on_link(b, l, "A2DP STREAMING START")) {
                ev(b, "re-assert START");
            }
            return;
        }
        if (b->sw_play && !b->sw_pl && !av &&
            b->dev[d].stream_ms < (avrcp_coming(b, d) ? BTS_SW_AVRCP_WAIT_MS : BTS_SW_STEADY_MS))
            return;   /* no AVRCP to decide PLAY on: not "playing" yet - field 2026-11-03: the phone (paused by the
                       * piano) opened its fresh link's stream and stopped it 10 ms later; its AVRCP came 1.1 s after */
        if (b->sw_play && !b->sw_pl && av) {
            uint8_t why = 0;
            int w = play_wanted(b, d, &why);
            if (w < 0)
                return;                    /* its play status first (field 2026-11-02 night: PLAY toggled a playing PC) */
            if (w > 0 && q_avrcp(b, d, true) == BTS_OK) {
                b->sw_pl = 1;
                b->sw_tried |= BTS_SWT_PLAY | BTS_SWT_PLAY_UP;
            } else if (!w) {
                b->sw_pl = 1;
                b->sw_tried |= why;
                play_skipped(b, why);
            }
        }
        if (!b->q_n)
            sw_end(b, BTS_RES_PLAYING);   /* once the PLAY is out (the time includes it) */
        return;
    }
    if (b->q_n)
        return;
    b->sw_ms += ms;
    switch (b->sw_phase) {
    case BTS_SW_PAUSE:      /* AVRCP PAUSE sent: a phone stops its stream ~3 s later (field 2026-11-03) */
    case BTS_SW_SUSPEND:    /* the sink suspended the others' streams */
    case BTS_SW_CLOSE:      /* (no longer entered - firmware up to a12c870a closed their A2DP) */
        if (!o) {
            b->sw_phase = BTS_SW_LINK;
            b->sw_ms = 0;
        } else if (!SUSP_OK(b) || b->sw_held || held_only(b, d)) {
            b->sw_held = 1;  /* the sink stops none of them (q_suspend): wait for their own STOP - after our PAUSE, or
                              * the user's pause there (no AVRCP: the app says so; 1D 04 phase PAUSE - no log line: the
                              * slot payload has no room) */
            if (b->sw_ms >= BTS_SW_HELD_MS)
                sw_end(b, BTS_RES_BUSY);   /* field 2026-11-10: sink-stopping it would leave Windows silent for good */
            else if ((b->pol.xopt & BTS_X_LATE_STOP) && b->sw_phase == BTS_SW_PAUSE) {
                /* opt-in experiment: a source that says it is paused / stopped and still keeps its
                 * stream the policy wait after that report (Windows ~11 s, phones ~3 s) - the sink stops it; never one
                 * without AVRCP or with no / a playing report (that one: BUSY at BTS_SW_HELD_MS).  Its STOP line ends
                 * the wait (o 0), the target gets its PLAY as usual */
                uint32_t w = b->pol.xwait ? b->pol.xwait * 100u : BTS_SW_LATE_STOP_MS;
                for (int i = 0; i < BTS_DEV_MAX; i++)
                    if (i != d && b->dev[i].used && b->dev[i].streaming && avrcp_link(b, i) >= 0 &&
                        ps_paused(&b->dev[i]) && b->dev[i].pzr_ms >= w && q_stop_stream(b, i))
                        ev(b, "late STOP");
            }
        } else if (b->sw_phase == BTS_SW_PAUSE && b->sw_ms >= BTS_SW_PAUSE_MS) {
            /* still streaming after the pause: the sink suspends it - its links stay - BEFORE the target starts, also a
             * source that reports paused (field 2026-11-09 evening: leaving it open (7a206fac-8f98d335, the
             * 11-09 rule) failed both ways - the module kept the paused PC's stream (the phone's START only echoed), and
             * the paused phone's own STOP 3 s after the PC's START muted the piano).  The 11-09 finding (Windows silent on a stream
             * the sink resumed) is met on the way back instead: PLAY first (SETTLE) */
            for (int i = 0; i < BTS_DEV_MAX; i++)
                if (i != d && b->dev[i].used && b->dev[i].streaming)
                    q_suspend(b, i);
            b->sw_phase = BTS_SW_SUSPEND;
            b->sw_ms = 0;
        } else if (b->sw_phase != BTS_SW_PAUSE && b->sw_ms >= BTS_SW_SUSPEND_MS) {
            sw_end(b, BTS_RES_BUSY);       /* the other stream would not go */
        }
        return;
    case BTS_SW_LINK:
        if (b->call_on && b->call_avrcp)
            return;                        /* our AVRCP CALL is out: its CALL <l> line must not be taken for the A2DP one */
        if (d >= 0 && b->dev[d].a2dp) {
            b->sw_phase = BTS_SW_SETTLE;
            b->sw_ms = BTS_SW_SETTLE_MS;   /* an A2DP link that was there: no settling */
        } else {
            q_call(b, b->sw_addr);         /* sink-initiated (the TV / a source whose links went) */
            b->sw_phase = BTS_SW_CALL;
            b->sw_ms = 0;
        }
        return;
    case BTS_SW_CALL:
        if (d >= 0 && b->dev[d].a2dp) {
            b->sw_phase = BTS_SW_SETTLE;
            b->sw_ms = 0;
            b->sw_pl = 0;                  /* a new A2DP link: PLAY again on it */
            b->sw_tried |= BTS_SWT_FRESH;
        } else if (b->sw_ms >= BTS_SW_CALL_MS || !b->call_on) {
            sw_end(b, BTS_RES_NO_LINK);
        }
        return;
    case BTS_SW_SETTLE:
        /* AVRCP up: PLAY after BTS_SW_SETTLE_MS - also on a link we just CALLed (no waiting for a self-start).  A fresh
         * link without AVRCP: up to BTS_SW_FRESH_MS for its AVRCP to come back or its own start; then the sink starts
         * the stream.  A target the sink suspended (host model): resumed by the sink's START first, the PLAY decision
         * once its stream is up. */
        if (d < 0 || !b->dev[d].a2dp) {
            sw_end(b, BTS_RES_NO_LINK);
        } else if (b->sw_ms >= BTS_SW_SETTLE_MS) {
            bool av = avrcp_link(b, d) >= 0;
            if (!b->sw_play) {
                sw_end(b, BTS_RES_READY);
            } else if (SUSP_OK(b) && b->dev[d].susp && !av && !(b->sw_tried & BTS_SWT_START)) {
                sw_sink_start(b, d);       /* resume what the sink suspended - no AVRCP to ask its player (with AVRCP:
                                            * the PLAY decision first, the sink's START after BTS_SW_PLAY_MS - field
                                            * 2026-11-09: Windows put no sound on a stream the sink resumed) */
            } else if (av && !b->sw_pl) {
                sw_decide_play(b, d);      /* -1: its play status first */
            } else if (!av && (b->sw_tried & BTS_SWT_FRESH) && avrcp_coming(b, d) && b->sw_ms < BTS_SW_AVRCP_WAIT_MS) {
                return;                    /* the sink opens its AVRCP (avrcp_open, BTS_SW_AVRCP_MS): PLAY on that */
            } else if (av || !(b->sw_tried & BTS_SWT_FRESH) || b->sw_ms >= BTS_SW_FRESH_MS) {
                sw_sink_start(b, d);       /* no AVRCP (Windows / a phone whose AVRCP did not come back) */
            }
        }
        return;
    case BTS_SW_PLAY:
        if (b->sw_ms >= BTS_SW_PLAY_MS) {
            if (d < 0 || !b->dev[d].a2dp)
                sw_end(b, BTS_RES_NO_LINK);
            else if ((b->sw_tried & (BTS_SWT_START | BTS_SWT_KEPT)) != BTS_SWT_KEPT) {
                if (b->sw_ms >= BTS_SW_READY_MS)
                    sw_end(b, BTS_RES_READY);  /* our PLAY (or an unknown status) brought no stream: its player does not
                                            * play - a stream the sink opened now would be the silent one of field
                                            * 2026-11-10 night: press play on the device */
            } else
                sw_sink_start(b, d);       /* it reports PLAYING and has no stream (the module took another one): its
                                            * player plays: the sink's START (field 2026-11-09 evening #1: the sink's
                                            * START on a paused PC, its PLAY 0.3 s later - sound) */
        }
        return;
    case BTS_SW_START:
        if (b->sw_ms >= BTS_SW_START_MS) {
            if (d < 0 || !b->dev[d].a2dp)
                sw_end(b, BTS_RES_NO_LINK);
            else if (!b->sw_play || sw_decide_play(b, d) == 0)
                sw_end(b, BTS_RES_READY);  /* everything tried: press play on the device once - its link is kept (no
                                            * reopen any more: CLOSE broke Windows, field 2026-11-03) */
            /* else: the resumed target did not stream - its PLAY decision went now (or waits for its status) */
        }
        return;
    default:
        sw_end(b, BTS_RES_CANCELLED);
        return;
    }
}

/* refuse a stream of a source that is not the selected one (leave); host model (susp_ok): the third time within
 * BTS_GUARD_WIN_MS it is suspended at once (no PAUSE - a toggle on Windows).  Never closed. */
static void refuse(bts_t *b, int i)
{
    bts_dev_t *v = &b->dev[i];
    if (v->refuse_ms >= BTS_GUARD_WIN_MS)
        v->refuse_n = 0;
    if (v->refuse_n < 255)
        v->refuse_n++;
    v->refusing = 1;
    v->refuse_ms = 0;
    if (b->n.refusals < 0xFFFFu)
        b->n.refusals++;
    if (SUSP_OK(b) && v->refuse_n >= BTS_GUARD_CLOSE_N && !paused_open(b, v))
        q_suspend(b, i);
    else
        leave(b, i);
}

/* mode SELECT: only the selected source may stream */
static void guard(bts_t *b)
{
    int s = bts_sel_dev(b);
    bool sel_here = s >= 0 && b->dev[s].links;
    for (int i = 0; i < BTS_DEV_MAX; i++) {
        bts_dev_t *v = &b->dev[i];
        if (i == s || !v->used || !v->streaming || !v->addr_known)
            continue;
        if (!b->has_sel || !sel_here || (b->pol.flags & BTS_F_FOLLOW)) {
            /* nothing selected / the selected one is gone / follow: this one is the source now */
            if (s >= 0 && b->dev[s].streaming)
                refuse(b, s);
            select_dev(b, i);
            s = i;
            sel_here = true;
            continue;
        }
        if (v->stream_ms >= BTS_GUARD_MS && !v->refusing)
            refuse(b, i);
        else if (SUSP_OK(b) && v->refusing && v->refuse_ms >= BTS_SW_PAUSE_MS && !paused_open(b, v))
            q_suspend(b, i);   /* still streaming after the pause, status not paused: the sink suspends it - never
                                * closed; one that reports paused keeps its stream (Windows - field 2026-11-09) */
    }
}

/* AUTOCALL off: we reconnect, by the policy's choice (recon) - the preferred device, else (BTS_RC_PREF_LAST) the last
 * selected one; BTS_RC_OFF none; only while no source has A2DP */
static const uint32_t RC_BACKOFF[5] = {4000u, 8000u, 15000u, 30000u, 60000u};

static void reconnect(bts_t *b)
{
    const uint8_t *target = b->pol.recon == BTS_RC_OFF ? 0 : b->pol.has_pref ? b->pol.pref
                          : b->pol.recon == BTS_RC_PREF_LAST && b->has_sel && !b->sel_norc ? b->sel_addr : 0;
    if (bts_a2dp_links(b)) {
        if (b->rc_up_ms >= BTS_RC_STABLE_MS)
            b->rc_tries = 0;   /* a connection that stays ends the episode; one closed sooner was a failed try (field
                                * 2026-11-09: CALL -> CONNECT 0 / 1 -> NO CARRIER 1 / 0 in 0.3 s, a CALL every ~5 s) */
        b->rc_wait = 0;
        return;
    }
    if (b->ac_state != BTS_AC_OFF || !target || b->call_on || b->sw_phase != BTS_SW_IDLE || b->loop ||
        b->rc_tries >= BTS_RC_TRIES || b->rc_wait < RC_BACKOFF[b->rc_tries < 4 ? b->rc_tries : 4])
        return;
    b->rc_tries++;
    b->rc_wait = 0;
    if (b->n.reconnects < 0xFFFFu)
        b->n.reconnects++;
    q_call(b, target);
}

/* a source with A2DP and no AVRCP control link: the sink opens it (`CALL <addr> 17 AVRCP`, RM 6.16 / AN986 3.5.2), once
 * per A2DP episode, BTS_AVRCP_CALL_MS after - the source opens its own first.  Field 2026-11-09: a Windows PC answered our CALL 0.3 s after its A2DP with nothing for 11 s, then opened the browsing
 * channel (0x1b) alone and the module rebooted; with the sink not calling it opened its control channel 0.5 s after its
 * A2DP and nothing went wrong; phones answer our CALL within 0.5 s.  The switch target on an A2DP link we CALLed: sooner
 * (BTS_SW_AVRCP_MS - the switch waits for this AVRCP to decide PLAY; field 2026-11-03: the phone opened none by itself
 * on our link).  The suspect: never.  It also gives 「이 기기로 듣기」 PLAY / PAUSE for that source.  Mode SELECT only (not
 * the legacy modes), never with the opt-out flag, in the reboot loop, during a switch or while another CALL of ours is out;
 * unanswered: avrcp_giveup. */
static void avrcp_open(bts_t *b, uint32_t ms)
{
    for (int i = 0; i < BTS_DEV_MAX; i++) {
        bts_dev_t *v = &b->dev[i];
        uint32_t wait;
        bool tgt;
        if (!v->used)
            continue;
        if (!v->a2dp || avrcp_link(b, i) >= 0) {
            v->noavrcp_ms = 0;
            continue;
        }
        if (v->noavrcp_ms < 0x80000000u)
            v->noavrcp_ms += ms;
        /* the switch target once its A2DP link is up (our A2DP CALL answered): sooner - the switch waits for this
         * AVRCP to decide PLAY (field 2026-11-03: the phone opened none by itself) */
        tgt = b->sw_phase >= BTS_SW_CALL && (b->sw_tried & BTS_SWT_FRESH) && v->addr_known &&
              memcmp(v->addr, b->sw_addr, 6) == 0;
        wait = tgt ? BTS_SW_AVRCP_MS : BTS_AVRCP_CALL_MS;
        if (v->avrcp_called || !v->addr_known || is_suspect(b, v) || v->noavrcp_ms < wait || b->pol.mode != BTS_MODE_SELECT ||
            (b->pol.flags & BTS_F_NO_AVRCP_CALL) || b->loop || b->call_on || (b->sw_phase != BTS_SW_IDLE && !tgt))
            continue;
        if (q_call_p(b, v->addr, true)) {
            v->avrcp_called = 1;
            if (b->n.avrcp_calls < 0xFFFFu)
                b->n.avrcp_calls++;
        }
        return;   /* one CALL at a time */
    }
}

/* our `CALL … 17 AVRCP` answered `CALL <l>` and no CONNECT on l within BTS_AVRCP_GIVEUP_MS: `CLOSE <l>` (RM 6.18 - the
 * link id "from a previously used command CALL"), ahead of the queue; the CALL is over (no other CALL waits for it, the
 * switch goes on without that AVRCP) and not made again in this A2DP episode.  Field 2026-11-09: the PC that left our
 * CALL unanswered opened its browsing channel alone 11 s later and the module rebooted though we closed that channel -
 * our CALL must not be pending then.  Whether the module ends a page in progress on CLOSE is not in the docs [unknown]:
 * the link is forgotten here at once, a late CONNECT / NO CARRIER of it is a link of nobody (LIST sorts it out). */
static void avrcp_giveup(bts_t *b, uint32_t ms)
{
    int l = b->av_lid;
    char c[10] = "CLOSE ";
    if (l < 0)
        return;
    if (b->link_dev[l] < 0 || b->link_prof[l] != BTS_P_NONE) {
        b->av_lid = -1;   /* CONNECT came (the link is AVRCP) / NO CARRIER (the call failed) */
        return;
    }
    if ((b->av_ms += ms) < BTS_AVRCP_GIVEUP_MS)
        return;
    c[6 + put_num(c + 6, (unsigned)l & 15u)] = 0;
    if (!q_push_front(b, c))
        return;           /* the queue is full: next tick */
    if (b->call_on && b->call_avrcp && b->dev[b->link_dev[l]].addr_known &&
        memcmp(b->dev[b->link_dev[l]].addr, b->call_addr, 6) == 0)
        b->call_on = 0;
    link_drop(b, l);
    b->av_lid = -1;
    b->list_want = 1;
    TSTAT(avrcp_giveups);
    ev(b, "AVRCP CALL: CLOSE");   /* (before `AVRCP CALL unanswered: CLOSE`) */
}

/* safety net after a switch that ended with no PLAY decision (no AVRCP on the target then): its AVRCP comes within
 * BTS_SW_LATE_MS, the piano had paused it, nothing since showed it playing, and it is still the selected one -> AVRCP
 * PLAY once (whatever its last report - a report older than our PAUSE is stale, field 2026-11-10). */
static void sw_late_play(bts_t *b, uint32_t ms)
{
    int d;
    bts_dev_t *v;
    if (!b->sw_late || b->sw_phase != BTS_SW_IDLE)
        return;
    if ((b->sw_late_ms += ms) >= BTS_SW_LATE_MS || !b->has_sel || memcmp(b->sel_addr, b->sw_addr, 6) != 0) {
        b->sw_late = 0;
        return;
    }
    d = dev_by_addr(b, b->sw_addr);
    if (d < 0 || avrcp_link(b, d) < 0 || b->q_n)
        return;
    v = &b->dev[d];
    if (!v->ps_ours) {
        b->sw_late = 0;   /* it plays (a PLAYING report clears ps_ours) / it was not the piano that paused it */
        return;
    }
    if (v->ps == BTS_PS_UNKNOWN && ps_on(b) && !v->ps_none)
        return;           /* a new link: its play status first (the user may have pressed play meanwhile) */
    if (q_avrcp(b, d, true) == BTS_OK) {
        b->sw_late = 0;
        b->sw_tried |= BTS_SWT_PLAY | BTS_SWT_LATE;
        ev(b, "late PLAY");
    }
}

static void ho_tick(bts_t *b, uint32_t ms);

/* ---- the decided module values (btsrc.h BTS_PIN_*) ----------------------------------------- */
static uint8_t pin_differs(const bts_t *b)
{
    /* a line not listed (class) / not the plain form (AVRCP controller-only, the audio routing's four lines) is never
     * written - the same rule as the ops */
    return (uint8_t)((b->cod_state == BTS_COD_KNOWN && b->cod != BTS_PIN_COD ? BTS_PB_COD : 0) |
                     (b->avs.state == 1 && b->avs.now != BTS_PIN_AVF ? BTS_PB_AVF : 0) |
                     (b->aus.state == 1 && b->aus.now != BTS_PIN_AUD ? BTS_PB_AUD : 0));
}

static void pin_end(bts_t *b, const char *text)
{
    b->pin = BTS_PIN_DONE;
    ev(b, text);
}

static bool setting_busy(const bts_t *b);

/* the decided settings written and read back that the module does not show now (or no line / not the plain form) */
static uint8_t pin_missing(const bts_t *b, uint8_t w)
{
    return (uint8_t)(w & ((b->cod_state == BTS_COD_KNOWN && b->cod == BTS_PIN_COD ? 0 : BTS_PB_COD) |
                          (b->avs.state == 1 && b->avs.now == BTS_PIN_AVF ? 0 : BTS_PB_AVF) |
                          (b->aus.state == 1 && b->aus.now == BTS_PIN_AUD ? 0 : BTS_PB_AUD)));
}

/* after the `!can_act` return (module on, piano on, no module query on the wire).  One module-setting op at a time
 * (the app's own op first), never while a switch runs; every step once per power-on - a refusal is not tried again */
static void pin_tick(bts_t *b, uint32_t ms)
{
    uint8_t k;
    if (b->pin == BTS_PIN_DONE)
        return;
    if (b->pin_ms < 0x80000000u)
        b->pin_ms += ms;
    if (b->pin == BTS_PIN_RESTART) {
        if (b->pin_ms >= BTS_PIN_RESTART_MS) {   /* the port did not restart it (busy / Bluetooth Audio off) */
            b->pin_bad |= (uint8_t)(b->pin_w & (BTS_PB_AVF | BTS_PB_AUD) & ~b->pin_bad);
            pin_end(b, "pin: no restart");
        }
        return;
    }
    if (setting_busy(b) || b->sw_phase != BTS_SW_IDLE)
        return;
    if (b->pin == BTS_PIN_WAIT) {
        if (!b->set_done || b->loop)
            return;   /* the first complete listing of this power-on; nothing during the reboot-loop guard */
        b->pin_todo = b->pin_w = pin_differs(b);
        if (!b->pin_w) {
            pin_end(b, "pin: kept");
            return;
        }
        b->pin = BTS_PIN_WRITE;
    }
    if (b->pin == BTS_PIN_WRITE) {
        if (b->pin_todo) {
            int rc;
            k = (uint8_t)(b->pin_todo & -b->pin_todo);   /* class, then AVRCP, then audio - each its op's write + read */
            rc = k == BTS_PB_COD ? bts_class(b, BTS_CO_WRITE, BTS_PIN_COD)
               : k == BTS_PB_AVF ? bts_avrcp_features(b, BTS_AV_WRITE, BTS_PIN_AVF)
                                 : bts_audio(b, BTS_AU_WRITE, BTS_PIN_AUD);
            if (rc == BTS_FULL)
                return;   /* the queue is full: the next tick */
            b->pin_todo &= (uint8_t)~k;
            if (rc != BTS_OK)
                b->pin_w &= (uint8_t)~k;   /* a listing since changed it (now the value / another form): not written */
            return;
        }
        /* all read back: a write the module did not take stays so for this power-on (the op's rc BTS_STATE) */
        b->pin_bad |= (uint8_t)(b->pin_w & ((b->cod_rc != BTS_OK ? BTS_PB_COD : 0) |
                                            (b->avs.rc != BTS_OK ? BTS_PB_AVF : 0) |
                                            (b->aus.rc != BTS_OK ? BTS_PB_AUD : 0)));
        if (!(b->pin_w & ~b->pin_bad & (BTS_PB_AVF | BTS_PB_AUD))) {
            pin_end(b, b->pin_bad ? "pin: not taken" : "pin: written");   /* the class needs no restart (field 11-09) */
        } else if (b->live || b->loop) {
            /* a live apply keeps the links; the reboot-loop guard: the stored lines apply at the next
             * module start (the next power-on) */
            pin_end(b, "pin: restart at power-on");
        } else {
            b->recover_req = 1;   /* the port restarts the module as op 1D 07 does (stock off, 0.6 s, on) */
            b->pin = BTS_PIN_RESTART;
            b->pin_ms = 0;
            ev(b, "pin: module restart");
        }
        return;
    }
    if (b->pin == BTS_PIN_BACK) {
        if (b->ready_ms < BTS_SET_READ_MS || !q_push(b, "SET"))
            return;
        b->set_done = 0;
        b->pin = BTS_PIN_CHECK;
        b->pin_ms = 0;
        return;
    }
    /* BTS_PIN_CHECK: the listing after the restart */
    if (!b->set_done) {
        if (b->pin_ms >= BTS_PIN_CHECK_MS) {
            b->pin_bad |= (uint8_t)(b->pin_w & ~b->pin_bad);
            pin_end(b, "pin: no read back");
        }
        return;
    }
    k = pin_missing(b, (uint8_t)(b->pin_w & ~b->pin_bad));
    if (k & BTS_PB_COD)
        b->cod_rc = BTS_STATE;
    if (k & BTS_PB_AVF)
        b->avs.rc = BTS_STATE;
    if (k & BTS_PB_AUD)
        b->aus.rc = BTS_STATE;
    b->pin_bad |= k;
    pin_end(b, k ? "pin: not kept" : "pin: done");
}

void bts_tick(bts_t *b, uint32_t ms, bool can_act)
{
    int nstream;
    if (b->now_ms < 0xFFFFFFFFu - ms)
        b->now_ms += ms;
    if (b->gap_ms < 0x80000000u)
        b->gap_ms += ms;
    if (b->recover_ago < 0x80000000u)
        b->recover_ago += ms;
    if (b->sw_ago < 0x80000000u)
        b->sw_ago += ms;
    if (b->pref_wait < 0x80000000u)
        b->pref_wait += ms;
    if (b->reboot_ago < 0x80000000u)
        b->reboot_ago += ms;
    if (b->orphan_ago < 0x80000000u)
        b->orphan_ago += ms;
    if (b->codec_ago < 0x80000000u)
        b->codec_ago += ms;
    if (b->echo_ss && (b->echo_ms += ms) >= BTS_ECHO_MS)
        b->echo_ss = 0;   /* no echo came: a later address-less line is the module's */
    if (b->loop && b->reboot_ago >= BTS_LOOP_HOLD_MS) {
        b->loop = 0;   /* quiet long enough: the manager may call again */
        bts_log(b, BTS_L_EVENT, "reboot loop over", 16);
    }
    for (int i = 0; i < BTS_DEV_MAX; i++) {
        if (b->closed[i].used && b->closed[i].ago_ms < 0x80000000u)
            b->closed[i].ago_ms += ms;
        if (!b->dev[i].used)
            continue;
        if (b->dev[i].streaming && b->dev[i].stream_ms < 0x80000000u)
            b->dev[i].stream_ms += ms;
        if (b->dev[i].pzr_ms < 0x80000000u)
            b->dev[i].pzr_ms += ms;
        if (b->dev[i].streaming && b->dev[i].stream_ms >= BTS_SW_STEADY_MS)
            b->dev[i].fresh = 0;   /* its stream lasted: a later start on this link is its own */
        if (b->dev[i].a2dp && b->dev[i].a2dp_ms < 0x80000000u) {
            b->dev[i].a2dp_ms += ms;
            if (b->sel_norc && b->dev[i].a2dp_ms >= BTS_RC_STABLE_MS && b->has_sel && b->dev[i].addr_known &&
                memcmp(b->dev[i].addr, b->sel_addr, 6) == 0)
                b->sel_norc = 0;   /* it connected again and stays: a reconnect target again */
        }
        if (b->dev[i].refuse_ms < 0x80000000u)
            b->dev[i].refuse_ms += ms;
        if (b->dev[i].a2dp_closing && (b->dev[i].a2dp_close_ms += ms) >= 3000u)
            b->dev[i].a2dp_closing = 0;   /* the CLOSE did not take: may be tried again */
        if (b->dev[i].closing) {
            b->dev[i].closing_ms += ms;
            if (b->dev[i].closing_ms > 5000u)
                b->dev[i].closing = 0;   /* the close did not take: may be tried again */
        }
    }
    if (!b->module_on) {
        b->q_n = 0;
        b->waiting = 0;
        b->dual_ms = 0;
        b->ho.armed = 0;   /* the module went off (standby): the links went with it, nothing to call back */
        return;
    }
    if (b->ready_ms < 0x80000000u)
        b->ready_ms += ms;
    if (b->rc_wait < 0x80000000u)
        b->rc_wait += ms;
    if (!bts_a2dp_links(b))
        b->rc_up_ms = 0;
    else if (b->rc_up_ms < 0x80000000u)
        b->rc_up_ms += ms;
    avrcp_giveup(b, ms);
    if (b->call_on) {
        int c = dev_by_addr(b, b->call_addr);
        uint16_t a = c >= 0 ? b->dev[c].a2dp : 0;
        b->call_ms += ms;
        if (b->call_ms >= BTS_CALL_MS || (!b->call_avrcp && (a & (a - 1))) ||
            (b->call_avrcp && c >= 0 && b->call_ms >= 200u && avrcp_link(b, c) >= 0))
            b->call_on = 0;   /* timed out / both A2DP channels / the AVRCP link of the call are up */
    }
    if (b->set_listing) {
        b->set_ms += ms;
        if (b->set_ms >= BTS_SET_LIST_MS) {
            b->set_listing = 0;   /* no closing `SET`: no decision */
            set_list_logged(b, " (no end)");
        }
    }
    if (b->waiting) {
        b->wait_ms += ms;
        if (b->wait_ms >= BTS_ANSWER_MS) {
            b->waiting = 0;
            if (b->no_answer < 255)
                b->no_answer++;
            if (b->n.unanswered < 0xFFFFu)
                b->n.unanswered++;
        }
    }
    nstream = bts_streams(b);
    if (nstream >= 2) {
        if (!b->dual_ms && b->n.dual < 0xFFFFu)
            b->n.dual++;
        b->dual_ms += ms;
    } else {
        b->dual_ms = 0;
    }
    if (b->list_want) {
        b->list_wait += ms;
        if (b->list_wait >= BTS_LIST_DELAY_MS && can_act && q_push(b, "LIST")) {
            b->list_want = 0;
            b->list_wait = 0;
        }
    } else {
        b->list_wait = 0;
    }
    if (!can_act) {
        b->q_n = 0;   /* standby / test mode / a module query on the wire: nothing goes out */
        sw_end(b, BTS_RES_CANCELLED);
        if (b->ac_op) {
            b->ac_op = 0;
            b->ac_rc = BTS_FULL;   /* the op's commands were dropped */
        }
        if (b->cod_op) {
            b->cod_op = 0;
            b->cod_rc = BTS_FULL;
        }
        if (b->avf_op) {
            b->avf_op = 0;
            b->avf_rc = BTS_FULL;
        }
        if (b->aud_op) {
            b->aud_op = 0;
            b->aud_rc = BTS_FULL;
        }
        return;
    }
    /* AUTOCALL state unknown and there is someone to reconnect: read the configuration once per power-on (`SET` is a
     * read; the module keeps AUTOCALL across its own restarts, so does ac_state) */
    if (!b->set_read && ((b->ac_state == BTS_AC_UNKNOWN && (b->has_sel || b->pol.has_pref)) ||
                         (b->pin == BTS_PIN_WAIT && !b->set_done)) &&
        b->ready_ms >= BTS_SET_READ_MS && q_push(b, "SET"))
        b->set_read = 1;   /* the configuration, read only: is AUTOCALL on? the decided module values? */
    pin_tick(b, ms);
    sw_step(b, ms);
    ps_tick(b, ms);   /* after the switch: a registration never holds up the switch's own commands */
    pz_recheck(b, ms);
    sw_late_play(b, ms);
    avrcp_open(b, ms);
#ifndef BTS_NO_NEWEST
    if (b->pol.mode == BTS_MODE_NEWEST)
        policy(b);
    else
#endif
    if (b->pol.mode == BTS_MODE_SELECT && b->sw_phase == BTS_SW_IDLE && !b->q_n)
        guard(b);
    if (b->pol.mode != BTS_MODE_NEWEST)
        pref_call(b);
    reconnect(b);
    ho_tick(b, ms);
    if ((b->pol.flags & BTS_F_RECOVER) && b->dual_ms >= (uint32_t)b->pol.hold_ds * 100u + BTS_RECOVER_EXTRA_MS &&
        b->no_answer >= 2 && b->recover_ago >= BTS_RECOVER_GAP_MS) {
        b->recover_req = 1;
        b->recover_ago = 0;
        if (b->n.recoveries < 0xFFFFu)
            b->n.recoveries++;
    }
}

bool bts_take_recover(bts_t *b)
{
    bool r = b->recover_req != 0;
    b->recover_req = 0;
    return r;
}

/* ---- manual actions ----------------------------------------------------------------------------- */
int bts_close(bts_t *b, const uint8_t addr[6])
{
    int d = dev_by_addr(b, addr);
    if (d < 0)
        return BTS_UNKNOWN;
    if (b->dev[d].parked && !b->dev[d].links) {   /* 「끊기」 on a parked device: forget it */
        b->dev[d].parked = 0;
        dev_free_if_empty(b, d);
        return BTS_OK;
    }
    b->dev[d].parked = 0;
    return close_dev(b, d) ? BTS_OK : BTS_UNKNOWN;
}

int bts_select(bts_t *b, const uint8_t addr[6])
{
    int d = dev_by_addr(b, addr);
    if (d < 0)
        return BTS_UNKNOWN;
    for (int i = 0; i < BTS_DEV_MAX; i++)
        if (i != d && b->dev[i].used && b->dev[i].links)
            close_dev(b, i);
    b->active = (int8_t)d;
    return BTS_OK;
}

int bts_call(bts_t *b, const uint8_t addr[6])
{
    uint8_t before = b->q_n;
    q_call(b, addr);
    return b->q_n > before ? BTS_OK : BTS_FULL;
}

int bts_use(bts_t *b, const uint8_t addr[6], bool play)
{
    int d = dev_by_addr(b, addr);
    if (d < 0 && !(b->pol.has_pref && memcmp(addr, b->pol.pref, 6) == 0) &&
        !(b->has_sel && memcmp(addr, b->sel_addr, 6) == 0))
        return BTS_UNKNOWN;   /* a device that is not here can be switched to only if it is known (pref / selected) */
    memcpy(b->sel_addr, addr, 6);
    b->has_sel = 1;
    b->sel_norc = 0;   /* picked again: a reconnect target again */
    if (d >= 0)
        select_dev(b, d);
    memcpy(b->sw_addr, addr, 6);
    b->sw_play = play;
    b->sw_ms = 0;
    b->sw_total = 0;
    b->sw_result = BTS_RES_NONE;
    b->sw_tried = 0;
    b->sw_pl = 0;
    b->sw_late = 0;
    b->sw_held = 0;
    /* its stream was open before this pick (the user's 「이 기기로 듣기」 on the device already heard - or not heard): the
     * sink's START goes to it once even so (field 2026-11-09 evening: the piano silent, re-picking the selected PC sent
     * nothing - `no PLAY: not paused by us`) */
    b->sw_reassert = d >= 0 && b->dev[d].streaming ? BTS_RA_PRE : 0;
    b->sw_phase = BTS_SW_PAUSE;   /* the others are paused now */
    for (int i = 0; i < BTS_DEV_MAX; i++)
        if (i != d)
            b->dev[i].ps_ours = 0;   /* the user picked another device: a pause the piano made before is not ours to undo
                                      * (the one that plays now is paused below - that one is ours again) */
    for (int i = 0; i < BTS_DEV_MAX; i++)
        if (i != d && b->dev[i].used && b->dev[i].streaming)
            leave(b, i);   /* never a CLOSE (field 2026-11-03) */
    return BTS_OK;
}

/* a module-setting op's read-back is pending (the four ops run one at a time) */
static bool setting_busy(const bts_t *b) { return b->ac_op || b->cod_op || b->avf_op || b->aud_op; }

int bts_autocall(bts_t *b, uint8_t op)
{
    if (setting_busy(b))
        return BTS_FULL;   /* the previous op (this one's or another module-setting op's) has not been read back yet */
    if (op == BTS_AC_SET_OFF) {
        if (b->ac_state != BTS_AC_ON || !b->ac_saved)
            return BTS_STATE;   /* the original line must be read first (it is what restore writes back) */
        if (!q_push(b, "SET CONTROL AUTOCALL"))   /* RM 6.68 "Disable: SET CONTROL AUTOCALL" */
            return BTS_FULL;
    } else if (op == BTS_AC_RESTORE) {
        if (b->ac_state != BTS_AC_OFF)
            return BTS_STATE;
        if (!q_push(b, bts_autocall_line(b)))
            return BTS_FULL;
    } else if (op != BTS_AC_READ) {
        return BTS_BAD;
    }
    q_push(b, "SET");   /* read back: the listing decides the result */
    b->ac_op = op;
    b->ac_rc = BTS_OK;
    return BTS_OK;
}

const char *bts_autocall_line(const bts_t *b)
{
    /* the stock factory line (FUN_4013E740 sends it from 0x4013EE0C - REA; field M6 read the same value) */
    return b->ac_saved ? b->ac_line : "SET CONTROL AUTOCALL 19 5000 A2DP";
}

bool bts_cod_ok(uint32_t cod)
{
    /* Bluetooth assigned numbers (Baseband): bits 0-1 format type 0, bits 8-12 major class (0x04 Audio/Video), bit 18
     * Rendering service - what Android's BluetoothClass.doesClassMatch(PROFILE_A2DP) accepts (field 2026-11-09) */
    return cod <= 0xFFFFFFu && (cod & 3u) == 0 && ((cod >> 8) & 0x1Fu) == 0x04u && (cod & 0x040000u) != 0;
}

uint32_t bts_class_restore(const bts_t *b) { return b->cod_saved ? b->cod_orig : BTS_COD_IWRAP; }

/* `SET BT CLASS <6 lower-case hex digits>` - the form the module lists */
static void class_cmd(char out[20], uint32_t cod)
{
    static const char hx[] = "0123456789abcdef";
    memcpy(out, "SET BT CLASS ", 13);
    for (int k = 0; k < 6; k++)
        out[13 + k] = hx[(cod >> (20 - 4 * k)) & 0xFu];
    out[19] = 0;
}

int bts_class(bts_t *b, uint8_t op, uint32_t cod)
{
    char cmd[20];
    if (op != BTS_CO_READ && op != BTS_CO_WRITE && op != BTS_CO_RESTORE)
        return BTS_BAD;
    if (op == BTS_CO_WRITE) {
        if (!cod)
            cod = BTS_COD_AUDIO_SINK;
        if (!bts_cod_ok(cod))
            return BTS_BAD;
    }
    if (setting_busy(b))
        return BTS_FULL;   /* the previous op (this one's or another module-setting op's) has not been read back yet */
    if (op != BTS_CO_READ) {
        uint32_t want = op == BTS_CO_WRITE ? cod : bts_class_restore(b);
        if (b->cod_state != BTS_COD_KNOWN || b->cod == want)
            return BTS_STATE;   /* read first (restore needs the value read before); nothing to change */
        class_cmd(cmd, want);
        if (!q_push(b, cmd))
            return BTS_FULL;
        b->cod_want = want;
    }
    q_push(b, "SET");   /* read back: the listing decides the result */
    b->cod_op = op;
    b->cod_rc = BTS_OK;
    return BTS_OK;
}

bool bts_avf_ok(uint8_t f) { return f >= 1 && f <= 0x0F; }

uint8_t bts_avf_restore(const bts_t *b) { return b->avf_saved ? b->avf_orig : (uint8_t)BTS_AVF_FACTORY; }

/* the explicit op of an 8-bit setting (state 1 = the plain form): write `wr` / restore `rs` as one queued token line
 * (BTS_Q_*) + a `SET` read back */
static int set8_op(bts_t *b, bts_set8_t *s, char type, uint8_t op, uint8_t wr, uint8_t rs)
{
    if (setting_busy(b))
        return BTS_FULL;   /* another module-setting op's read-back is pending */
    if (op != 1) {
        uint8_t want = op == 2 ? wr : rs;
        char cmd[4] = {type, (char)(0x40 | want >> 4), (char)(0x40 | (want & 15)), 0};
        if (s->state != 1 || s->now == want)
            return BTS_STATE;   /* read first (the plain form - restore needs it); nothing to change */
        if (!q_push(b, cmd))
            return BTS_FULL;
        s->want = want;
    }
    q_push(b, "SET");   /* read back: the listing decides the result */
    s->op = op;
    s->rc = BTS_OK;
    return BTS_OK;
}

int bts_avrcp_features(bts_t *b, uint8_t op, uint8_t f)
{
    if (op != BTS_AV_READ && op != BTS_AV_WRITE && op != BTS_AV_RESTORE)
        return BTS_BAD;
    if (op == BTS_AV_WRITE) {
        if (!f)
            f = BTS_AVF_NO_BROWSE;
        if (!bts_avf_ok(f))
            return BTS_BAD;
    }
    return set8_op(b, &b->avs, BTS_Q_AVRCP, op, f, bts_avf_restore(b));
}

bool bts_aud_ok(uint8_t f) { return f == BTS_AUD_EVENT || f == (BTS_AUD_EVENT | BTS_AUD_KEEP); }

uint8_t bts_aud_restore(const bts_t *b) { return b->aud_saved ? b->aud_orig : (uint8_t)BTS_AUD_FACTORY; }

void bts_audio_line(char *out, uint8_t f)
{
    /* RM 6.67 flag order: [EVENT] [KEEPALIVE] - the factory line (REA 0x4013EE30) without its CR LF when f = KEEP */
    uint32_t n = 36;
    memcpy(out, "SET CONTROL AUDIO INTERNAL I2S_SLAVE", 36);
    if (f & BTS_AUD_EVENT) {
        memcpy(out + n, " EVENT", 6);
        n += 6;
    }
    if (f & BTS_AUD_KEEP) {
        memcpy(out + n, " KEEPALIVE", 10);
        n += 10;
    }
    out[n] = 0;
}

int bts_audio(bts_t *b, uint8_t op, uint8_t f)
{
    if (op < BTS_AU_READ || op > BTS_AU_RESTORE)
        return BTS_BAD;
    if (op == BTS_AU_WRITE) {
        if (!f)
            f = BTS_AUD_EVENT;
        if (!bts_aud_ok(f))
            return BTS_BAD;
    }
    return set8_op(b, &b->aus, BTS_Q_AUDIO, op, f, bts_aud_restore(b));
}

uint8_t bts_dev_state2(const bts_t *b, int i)
{
    const bts_dev_t *v = &b->dev[i];
    uint8_t s = 0;
    if (!v->used)
        return 0;
    if (b->has_sel && v->addr_known && memcmp(v->addr, b->sel_addr, 6) == 0)
        s |= BTS_D2_SELECTED;
    if (v->refuse_n && v->refuse_ms < BTS_GUARD_WIN_MS)
        s |= BTS_D2_REFUSED;
    if (avrcp_link(b, i) >= 0)
        s |= BTS_D2_AVRCP;
    if (b->sw_phase != BTS_SW_IDLE && v->addr_known && memcmp(v->addr, b->sw_addr, 6) == 0)
        s |= BTS_D2_TARGET;
    if (b->call_on && !b->call_avrcp && v->addr_known && memcmp(v->addr, b->call_addr, 6) == 0)
        s |= BTS_D2_CALLING;
    if (v->parked && !v->a2dp)
        s |= BTS_D2_PARKED;
    return s;
}

uint8_t bts_dev_ps(const bts_t *b, int i)
{
    const bts_dev_t *v = &b->dev[i];
    if (!v->used)
        return 0;
    return (uint8_t)((v->ps & 7u) | (v->ps_live ? BTS_PSB_LIVE : 0) | (v->ps_none ? BTS_PSB_NONE : 0) |
                     (v->ps_ours ? BTS_PSB_OURS : 0));
}

static int bts_mute(bts_t *b, const uint8_t addr[6])
{
    int d = dev_by_addr(b, addr);
    if (d < 0)
        return BTS_UNKNOWN;
    return q_close_a2dp(b, d);
}

int bts_avrcp(bts_t *b, const uint8_t addr[6], bool play)
{
    int d = dev_by_addr(b, addr);
    if (d < 0)
        return BTS_UNKNOWN;
    return q_avrcp(b, d, play);
}

int bts_action(bts_t *b, uint8_t act, const uint8_t addr[6])
{
    switch (act) {
    case BTS_ACT_CLOSE: return bts_close(b, addr);
    case BTS_ACT_USE: return bts_use(b, addr, true);
    case BTS_ACT_CALL: return bts_call(b, addr);
    case BTS_ACT_ONLY: return bts_select(b, addr);
    case BTS_ACT_PAUSE: return bts_avrcp(b, addr, false);
    case BTS_ACT_PLAY: return bts_avrcp(b, addr, true);
    case BTS_ACT_MUTE: return bts_mute(b, addr);
    default: return BTS_BAD;
    }
}

/* ---- line log ----------------------------------------------------------------------------------- */
static uint8_t log_at(const bts_log_t *g, uint32_t off) { return g->buf[off % BTS_LOG_CAP]; }

static void log_drop_oldest(bts_log_t *g)
{
    uint32_t sz = BTS_LOG_HDR + log_at(g, g->head);
    g->head = (uint16_t)((g->head + sz) % BTS_LOG_CAP);
    g->used = (uint16_t)(g->used - sz);
    g->first = (uint16_t)((g->first + 1) & BTS_SEQ_MASK);
}

void bts_log(bts_t *b, uint8_t flags, const char *text, uint32_t n)
{
    bts_log_t *g = &b->log;
    uint32_t sz, w;
    uint8_t hdr[BTS_LOG_HDR];
    if (n > BTS_LINE_MAX) {
        n = BTS_LINE_MAX;
        flags |= BTS_L_TRUNC;
    }
    sz = BTS_LOG_HDR + n;
    while (g->used && BTS_LOG_CAP - g->used < sz)
        log_drop_oldest(g);
    hdr[0] = (uint8_t)n;
    hdr[1] = flags;
    hdr[2] = (uint8_t)b->now_ms;
    hdr[3] = (uint8_t)(b->now_ms >> 8);
    hdr[4] = (uint8_t)(b->now_ms >> 16);
    hdr[5] = (uint8_t)(b->now_ms >> 24);
    w = (uint32_t)g->head + g->used;
    for (uint32_t i = 0; i < sz; i++)
        g->buf[(w + i) % BTS_LOG_CAP] = i < BTS_LOG_HDR ? hdr[i] : (uint8_t)text[i - BTS_LOG_HDR];
    g->used = (uint16_t)(g->used + sz);
    g->next = (uint16_t)((g->next + 1) & BTS_SEQ_MASK);
}

uint32_t bts_log_read(const bts_t *b, uint16_t from, uint8_t *out, uint32_t cap, uint16_t *first, uint16_t *next)
{
    const bts_log_t *g = &b->log;
    uint32_t off = g->head, left = g->used, o = 0;
    uint16_t seq = g->first;
    *first = g->first;
    *next = g->next;
    if (((uint32_t)(from - g->first) & BTS_SEQ_MASK) > ((uint32_t)(g->next - g->first) & BTS_SEQ_MASK))
        from = g->first;   /* older than the oldest kept: from the oldest */
    while (left) {
        uint32_t n = log_at(g, off), rec = BTS_LOG_HDR + n, t;
        if (((uint32_t)(seq - from) & BTS_SEQ_MASK) < 0x2000u) {
            uint32_t room;
            if (o + 8 >= cap)
                break;
            room = cap - o - 8;
            if (n > room) {
                if (o)
                    break;   /* whole records only, except the first */
                n = room;
            }
            t = ((uint32_t)log_at(g, off + 2) | (uint32_t)log_at(g, off + 3) << 8 | (uint32_t)log_at(g, off + 4) << 16 |
                 (uint32_t)log_at(g, off + 5) << 24) / 10u;
            out[o++] = (uint8_t)((seq >> 7) & 0x7F);
            out[o++] = (uint8_t)(seq & 0x7F);
            out[o++] = (uint8_t)((t >> 21) & 0x7F);
            out[o++] = (uint8_t)((t >> 14) & 0x7F);
            out[o++] = (uint8_t)((t >> 7) & 0x7F);
            out[o++] = (uint8_t)(t & 0x7F);
            out[o++] = (uint8_t)(log_at(g, off + 1) & 0x7F);
            out[o++] = (uint8_t)n;
            for (uint32_t i = 0; i < n; i++) {
                uint8_t c = log_at(g, off + BTS_LOG_HDR + i);
                out[o++] = c >= 0x80 ? (uint8_t)'?' : c;
            }
        }
        off += rec;
        left -= rec;
        seq = (uint16_t)((seq + 1) & BTS_SEQ_MASK);
    }
    return o;
}

int bts_stream_playing(const bts_t *b, int l)
{
    int d = l >= 0 && l < BTS_LINKS ? b->link_dev[l] : -1;
    if (d < 0)   /* START without a link id, or its link not in the table: the one device that streams */
        for (int i = 0; i < BTS_DEV_MAX; i++)
            if (b->dev[i].used && b->dev[i].streaming) {
                if (d >= 0)
                    return -1;
                d = i;
            }
    if (d < 0 || !b->dev[d].used || b->dev[d].ps_none || b->dev[d].ps == BTS_PS_UNKNOWN)
        return -1;
    return b->dev[d].ps == BTS_PS_PLAYING ? 1 : 0;
}

uint16_t bts_a2dp_links(const bts_t *b)
{
    uint16_t m = 0;
    for (int i = 0; i < BTS_DEV_MAX; i++)
        if (b->dev[i].used)
            m = (uint16_t)(m | b->dev[i].a2dp);
    return m;
}

uint8_t bts_dev_state(const bts_t *b, int i)
{
    const bts_dev_t *v = &b->dev[i];
    uint8_t s = 0;
    if (!v->used)
        return 0;
    if (v->streaming)
        s |= BTS_D_STREAM;
    if (v->incoming)
        s |= BTS_D_INCOMING;
    if (closed_within(b, v, BTS_REJOIN_MS))
        s |= BTS_D_CLOSED;
    if (is_pref(b, v))
        s |= BTS_D_PREF;
    if (b->active == i)
        s |= BTS_D_ACTIVE;
    if (v->addr_known)
        s |= BTS_D_ADDR;
    if (v->closing)
        s |= BTS_D_CLOSING;
    return s;
}

/* ---- live apply handover (btsrc.h) ---------------------------------------------------------- */
static uint32_t ho_get32(const uint8_t *p)
{
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

static void ho_put32(uint8_t *p, uint32_t v)
{
    for (int i = 0; i < 4; i++)
        p[i] = (uint8_t)(v >> (8 * i));
}

uint32_t bts_handover_save(const bts_t *b, uint32_t stamp_us, uint8_t out[BTS_HO_RAW])
{
    int s = bts_sel_dev(b);
    uint8_t n = 0;
    memset(out, 0, BTS_HO_RAW);
    memcpy(out, "C79B", 4);
    out[4] = BTS_HO_VER;
    if (b->has_sel) {
        out[5] |= BTS_HO_F_SEL;
        memcpy(out + 8, b->sel_addr, 6);
        if (s >= 0 && b->dev[s].streaming)
            out[5] |= BTS_HO_F_STREAM;
    }
    for (int i = 0; i < BTS_DEV_MAX; i++)
        if (b->dev[i].used && b->dev[i].addr_known && b->dev[i].a2dp)
            memcpy(out + 16 + 6 * n++, b->dev[i].addr, 6);
    out[6] = n;
    ho_put32(out + 44, stamp_us);
    ho_put32(out + 48, ca_crc32(0, out, 48));
    return BTS_HO_RAW;
}

int bts_handover_load(bts_t *b, const uint8_t *raw, uint32_t n, uint32_t now_us)
{
    bts_ho_t *h = &b->ho;
    uint8_t k = 0;
    if (n < BTS_HO_RAW || memcmp(raw, "C79B", 4) != 0 || raw[4] != BTS_HO_VER || raw[6] > BTS_DEV_MAX ||
        ho_get32(raw + 48) != ca_crc32(0, raw, 48) || now_us - ho_get32(raw + 44) >= BTS_HO_FRESH_US)
        return -1;
    memset(h, 0, sizeof *h);
    if (raw[5] & BTS_HO_F_SEL) {
        memcpy(b->sel_addr, raw + 8, 6);
        b->has_sel = 1;
        h->sel_stream = (raw[5] & BTS_HO_F_STREAM) != 0;
    }
    for (int pass = 0; pass < 2; pass++)   /* the selected device first: it is called back first */
        for (int i = 0; i < raw[6]; i++) {
            const uint8_t *a = raw + 16 + 6 * i;
            bool sel = b->has_sel && memcmp(a, b->sel_addr, 6) == 0;
            if (sel == (pass == 0))
                memcpy(h->addr[k++], a, 6);
        }
    h->n = k;
    h->armed = 1;
    b->live = 1;   /* the decided module values: written if needed, the module never restarted */
    h->wait = BTS_HO_LIST_MS;   /* the first tick asks the module's table (it keeps its links over the apply) unless the
                                 * port already does */
    ev(b, "live apply: handover");
    return k;
}

/* after the first LIST: a handed-over device with A2DP stays (the selected one's stream state back, unless a stream line
 * came since); one without is called back - one CALL at a time, in order, never while another CALL, a switch, the reboot
 * loop or the module's own AUTOCALL is on */
static void ho_tick(bts_t *b, uint32_t ms)
{
    bts_ho_t *h = &b->ho;
    bool left = false;
    if (!h->armed)
        return;
    h->ms += ms;
    h->wait += ms;
    if (!h->listed) {
        if (h->ms >= BTS_HO_MS)
            h->armed = 0;
        else if (h->wait >= BTS_HO_LIST_MS && !b->list_want && !b->listing) {
            b->list_want = 1;   /* no reply: ask again */
            h->wait = 0;
        }
        return;
    }
    for (int i = 0; i < h->n; i++) {
        int d;
        if (h->done[i])
            continue;
        d = dev_by_addr(b, h->addr[i]);
        if (d >= 0 && b->dev[d].a2dp) {
            h->done[i] = 1;
            if (i == 0 && h->sel_stream && !h->ev && !b->dev[d].streaming && b->has_sel &&
                memcmp(h->addr[0], b->sel_addr, 6) == 0) {
                bts_dev_t *v = &b->dev[d];
                v->streaming = 1;   /* its stream went on over the apply (LIST does not show streams) */
                v->seq = ++b->seq;
                v->stream_ms = BTS_SW_STEADY_MS;
                v->fresh = 0;
                b->active = (int8_t)d;
            }
            ev(b, h->tries[i] ? "handover: back" : "handover: kept");
            continue;
        }
        left = true;
        if (h->tries[i] >= BTS_HO_TRIES) {
            if (h->wait >= BTS_HO_GAP_MS) {
                h->done[i] = 1;
                left = false;
                ev(b, "handover: not back");
                continue;
            }
            break;
        }
        if (b->call_on || b->sw_phase != BTS_SW_IDLE || b->loop || b->ac_state == BTS_AC_ON || b->q_n ||
            (h->tries[i] && h->wait < BTS_HO_GAP_MS))
            break;   /* one at a time, in order */
        q_call(b, h->addr[i]);
        if (b->call_on) {
            h->tries[i]++;
            h->wait = 0;
            ev(b, "handover: CALL");
        }
        break;
    }
    for (int i = 0; i < h->n; i++)
        left = left || !h->done[i];
    if (!left || h->ms >= BTS_HO_MS)
        h->armed = 0;
}
