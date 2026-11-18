/* OpenGrand — Bluetooth audio activity model and the APO hold rule (see ca79ng/power.h). */
#include "ca79ng/power.h"

#include <string.h>

void bt_audio_init(bt_audio_t *b)
{
    memset(b, 0, sizeof *b);
    b->stream_link = -1;
    b->play = -1;
}

void bt_audio_play(bt_audio_t *b, int playing)
{
    b->play = (int8_t)(playing < 0 ? -1 : playing ? 1 : 0);
}

/* ---- tokenizer: space separated words of one line ---- */
#define MAX_TOK 8
typedef struct {
    const char *s[MAX_TOK];
    uint8_t n[MAX_TOK];
    int count;
} toks_t;

static void tokenize(const char *line, uint16_t len, toks_t *t)
{
    t->count = 0;
    uint16_t i = 0;
    while (i < len && t->count < MAX_TOK) {
        while (i < len && line[i] == ' ')
            i++;
        if (i >= len)
            break;
        uint16_t s = i;
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

/* iWRAP link ids are small decimals; -1 if the token is not one */
static int tok_link(const toks_t *t, int i)
{
    if (i >= t->count || t->n[i] == 0 || t->n[i] > 2)
        return -1;
    int v = 0;
    for (int k = 0; k < t->n[i]; k++) {
        char c = t->s[i][k];
        if (c < '0' || c > '9')
            return -1;
        v = v * 10 + (c - '0');
    }
    return v < 16 ? v : -1;
}

static void drop_stream(bt_audio_t *b)
{
    b->streaming = false;
    b->stream_link = -1;
}

bool bt_audio_line(bt_audio_t *b, const char *line, uint16_t len)
{
    toks_t t;
    tokenize(line, len, &t);
    b->lines++;
    int link;

    /* READY. — the module (re)booted: every link it had is gone. The stock parser only
     * sets 0x413AE5C1 and leaves the link flag stale. */
    if (t.count == 1 && tok_is(&t, 0, "READY.")) {
        bool changed = b->a2dp_links || b->streaming;
        b->a2dp_links = 0;
        drop_stream(b);
        return changed;
    }
    /* CONNECT <link> A2DP <psm> */
    if (tok_is(&t, 0, "CONNECT") && tok_is(&t, 2, "A2DP") && (link = tok_link(&t, 1)) >= 0) {
        uint16_t m = (uint16_t)(b->a2dp_links | (1u << link));
        bool changed = m != b->a2dp_links;
        b->a2dp_links = m;
        return changed;
    }
    /* RING <link> <addr> <psm> A2DP (incoming); AVRCP rings are not audio */
    if (tok_is(&t, 0, "RING") && (link = tok_link(&t, 1)) >= 0) {
        for (int i = 2; i < t.count; i++) {
            if (tok_is(&t, i, "A2DP")) {
                uint16_t m = (uint16_t)(b->a2dp_links | (1u << link));
                bool changed = m != b->a2dp_links;
                b->a2dp_links = m;
                return changed;
            }
        }
        b->ignored++;
        return false;
    }
    /* A2DP STREAMING START|STOP [<link>] */
    if (tok_is(&t, 0, "A2DP") && tok_is(&t, 1, "STREAMING")) {
        if (tok_is(&t, 2, "START")) {
            b->streaming = true;
            b->stream_link = (int8_t)tok_link(&t, 3);
            b->starts++;
            return true;
        }
        if (tok_is(&t, 2, "STOP")) {
            int sl = tok_link(&t, 3);
            bool changed = b->streaming;
            if (sl >= 0 && b->stream_link >= 0 && sl != b->stream_link) {
                b->ignored++;   /* another source's stream ended: the playing one still holds */
                return false;
            }
            drop_stream(b);
            return changed;
        }
        b->ignored++;
        return false;
    }
    /* NO CARRIER <link> [ERROR <code> [text]] — the stock parser needs ERROR; every
     * observed line has it, but the link id is all this model needs. */
    if (tok_is(&t, 0, "NO") && tok_is(&t, 1, "CARRIER") && (link = tok_link(&t, 2)) >= 0) {
        bool changed = false;
        if (b->a2dp_links & (1u << link)) {
            b->a2dp_links = (uint16_t)(b->a2dp_links & ~(1u << link));
            changed = true;
        }
        /* the stream ends with its own link, or when no A2DP link is left */
        if (b->streaming && (b->stream_link == link || (changed && !b->a2dp_links))) {
            drop_stream(b);
            changed = true;
        }
        return changed;
    }
    b->ignored++;
    return false;
}

void bt_audio_rx(bt_audio_t *b, const uint8_t *bytes, uint16_t n)
{
    for (uint16_t i = 0; i < n; i++) {
        uint8_t c = bytes[i];
        if (c == '\r' || c == '\n') {
            if (!b->overflow && b->len >= 2)
                bt_audio_line(b, b->buf, b->len);
            b->len = 0;
            b->overflow = false;
        } else if (b->len < BT_LINE_MAX) {
            b->buf[b->len++] = (char)c;
        } else {
            b->overflow = true;
        }
    }
}

void bt_audio_link(bt_audio_t *b, bool up)
{
    if (b->link_flag && !up) {
        b->a2dp_links = 0;
        drop_stream(b);
    }
    b->link_flag = up;
}

bt_audio_state_t bt_audio_state(const bt_audio_t *b)
{
    if (b->streaming)
        return BT_AUDIO_STREAMING;
    if (b->a2dp_links || b->link_flag)
        return BT_AUDIO_CONNECTED;
    return BT_AUDIO_IDLE;
}

const char *bt_audio_state_name(bt_audio_state_t s)
{
    switch (s) {
    case BT_AUDIO_STREAMING: return "streaming";
    case BT_AUDIO_CONNECTED: return "connected";
    default: return "idle";
    }
}

/* ---------------- APO hold ---------------- */

void apo_policy_default(apo_policy_t *pol)
{
    pol->max_hold_ms = APO_MAX_HOLD_DEFAULT_MS;
    pol->hold_connected = false;
}

void apo_hold_init(apo_hold_t *h, const apo_policy_t *pol)
{
    memset(h, 0, sizeof *h);
    if (pol)
        h->pol = *pol;
    else
        apo_policy_default(&h->pol);
}

bool apo_tick(apo_hold_t *h, power_timer_t *t, const bt_audio_t *bt, uint32_t elapsed_ms)
{
    bt_audio_state_t s = bt ? bt_audio_state(bt) : BT_AUDIO_IDLE;
    uint32_t lim = power_timer_limit_ms(t->setting);
    /* a stream holds unless its source says it does not play; "hold while only connected"
     * (opt-in) holds on any link, playing or not */
    bool want = h->pol.max_hold_ms != 0 && lim != 0 &&
                ((s == BT_AUDIO_STREAMING && bt->play != 0) || (h->pol.hold_connected && s != BT_AUDIO_IDLE));
    bool was = h->holding;
    h->holding = false;
    h->released = false;
    if (!want) {
        /* no stream (or its source reports it is not playing): the next one (or PLAYING again) starts a fresh
         * hold; a hold that ends here (not at its cap) gives the full menu time from now */
        if (was) {
            power_timer_activity(t);
            h->released = true;
        }
        h->capped = false;
        h->held_ms = 0;
        h->seen_starts = bt ? bt->starts : 0;
        return power_timer_tick(t, elapsed_ms);
    }
    if (bt->starts != h->seen_starts) {
        /* a new STREAMING START (play after a source switch, ...) re-arms the cap */
        h->seen_starts = bt->starts;
        h->capped = false;
        h->held_ms = 0;
    }
    if (h->capped)
        return power_timer_tick(t, elapsed_ms);
    h->held_ms = (h->held_ms > UINT32_MAX - elapsed_ms) ? UINT32_MAX : h->held_ms + elapsed_ms;
    if (h->pol.max_hold_ms != APO_HOLD_FOREVER && h->held_ms >= h->pol.max_hold_ms) {
        /* the cap: the normal APO from here, as if there were no stream - an idle piano expires now,
         * one used within the menu time counts on from its last activity */
        h->capped = true;
        return power_timer_tick(t, elapsed_ms);
    }
    h->holding = true;
    (void)power_timer_tick(t, elapsed_ms);   /* the clock runs; the hold only stops the expiry */
    if (t->idle_ms > lim)
        t->idle_ms = lim;
    return false;
}

/* ---------------- power policy (user setting) ---------------- */

#define MIN_MS 60000u

void power_policy_default(power_policy_t *pol)
{
    apo_policy_default(&pol->hold);
    pol->apo_standby = true;
    pol->standby_cap_ms = POWER_CAP_DEFAULT_MS;
    pol->standby_bt_off = false; /* TV wake on (owner decision 2026-10-31) */
    pol->button_stock = false;   /* POWER button -> standby (F-A1) */
}

static void put16(uint8_t *b, uint32_t v)
{
    b[0] = (uint8_t)(v & 0xFF);
    b[1] = (uint8_t)((v >> 8) & 0xFF);
}

void power_policy_encode(const power_policy_t *pol, uint8_t raw[POWER_POLICY_RAW])
{
    memset(raw, 0, POWER_POLICY_RAW);
    raw[0] = POWER_POLICY_VERSION;
    raw[1] = (uint8_t)((pol->apo_standby ? POWER_FLAG_APO_STANDBY : 0) |
                       (pol->hold.hold_connected ? POWER_FLAG_HOLD_CONNECTED : 0) |
                       (pol->standby_bt_off ? POWER_FLAG_STANDBY_BT_OFF : 0) |
                       (pol->button_stock ? POWER_FLAG_BUTTON_STOCK : 0));
    uint32_t hold = pol->hold.max_hold_ms;
    uint32_t hold_min = hold == APO_HOLD_FOREVER ? POWER_HOLD_UNLIMITED
                        : hold == 0              ? 0
                                                 : (hold / MIN_MS ? hold / MIN_MS : 1);
    if (hold_min != POWER_HOLD_UNLIMITED && hold_min > POWER_HOLD_MAX_MIN)
        hold_min = POWER_HOLD_MAX_MIN;
    put16(raw + 2, hold_min);
    put16(raw + 4, pol->standby_cap_ms ? pol->standby_cap_ms / MIN_MS : POWER_CAP_NONE);
}

bool power_policy_decode(power_policy_t *pol, const uint8_t *raw, uint16_t n)
{
    if (n < 6 || raw[0] != POWER_POLICY_VERSION)
        return false;
    uint32_t hold_min = (uint32_t)raw[2] | (uint32_t)raw[3] << 8;
    uint32_t cap_min = (uint32_t)raw[4] | (uint32_t)raw[5] << 8;
    if (hold_min > POWER_HOLD_MAX_MIN && hold_min != POWER_HOLD_UNLIMITED)
        return false;
    if ((cap_min < POWER_CAP_MIN_MIN || cap_min > POWER_CAP_MAX_MIN) && cap_min != POWER_CAP_NONE)
        return false;
    pol->apo_standby = (raw[1] & POWER_FLAG_APO_STANDBY) != 0;
    pol->hold.hold_connected = (raw[1] & POWER_FLAG_HOLD_CONNECTED) != 0;
    pol->standby_bt_off = (raw[1] & POWER_FLAG_STANDBY_BT_OFF) != 0;
    pol->button_stock = (raw[1] & POWER_FLAG_BUTTON_STOCK) != 0;
    pol->hold.max_hold_ms = hold_min == POWER_HOLD_UNLIMITED ? APO_HOLD_FOREVER : hold_min * MIN_MS;
    pol->standby_cap_ms = cap_min == POWER_CAP_NONE ? 0 : cap_min * MIN_MS;   /* 0 = standby_t no cap */
    return true;
}

/* ---------------- remote off (feature 2) ---------------- */

uint8_t power_remote_off_check(bool recording, bool file_op, bool flash_busy)
{
    if (recording)
        return POWER_OFF_RECORDING;
    if (file_op)
        return POWER_OFF_FILE;
    if (flash_busy)
        return POWER_OFF_FLASH;
    return POWER_OFF_OK;
}
