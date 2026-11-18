/* OpenGrand — user Virtual Technician data: user touch curve and the per-note
 * voicing / key volume / tuning / temperament editors.
 *
 * Wire layouts (protocol_spec_CA79.json, sequences "PerNote*" and
 * "UserTouchCurve"; REMOTE_PROTOCOL.md §9):
 *   PerNote<X>Data  fn10 50 v2 7F [memoryNo, octaveNo, ofs+64 x N]
 *                   v2 17 Voicing (N 128, -5..+5), 10 KeyVolume (128, -50..+50),
 *                   0C Tuning (128, cents -50..+50), 0E Temperament (12, -50..+50)
 *   PerNote<X>Save  fn32 50 v2 7F [0]   -> working copy saved to kvstore
 *   PerNote<X>EditExit fn31 50 v2 7F [0] -> unsaved edits dropped (reload),
 *                   forwarded to the other connected controllers
 *   request 02 50 v2 7F -> PerNote<X>Data [127, 127, ...] then 03 50 v2 7F
 *   TouchCurveBulkOperationPoint fn10 5A 0D 7F [mem, (exist, vel) x 128] (RX)
 *   TouchCurveBulkName fn10 5A 0C 7F [mem, 3 x 7 bit per code point]
 *   TouchCurveOperationPointHeader/Data/End 5A 04/05/06 (chunked, OpenGrand layout, RX only)
 *   TouchCurveOneOperationPointChange 5A 07 [old, new, exist, vel]
 *   TouchCurveAllOperationPointsClear 5A 08, TouchCurveLearn 32 5A 0B [mem, 1|2]
 *   TouchCurveSave 32 5A 0A, TouchCurveEditExit 31 5A 09
 *   request 02 5A 7F 7F -> BulkName, BulkOperationPoint (5A 0D), 03 5A 7F 7F */
#include <string.h>

#include "ca79ng/util.h"
#include "piano_int.h"


typedef struct {
    uint8_t v2;
    uint16_t key;
    uint8_t size;
    int8_t lim;
} pernote_kind_t;

static const pernote_kind_t KINDS[4] = {
    {0x17, KV_KEY_PERNOTE_VOICING, 128, 5},
    {0x10, KV_KEY_PERNOTE_VOLUME, 128, 50},
    {0x0C, KV_KEY_PERNOTE_TUNING, 128, 50},
    {0x0E, KV_KEY_PERNOTE_TEMPERAMENT, 12, 50},
};

static int8_t *pernote_array(piano_t *p, int k)
{
    switch (k) {
    case 0: return p->vt.voicing;
    case 1: return p->vt.key_volume;
    case 2: return p->vt.tuning;
    default: return p->vt.temperament;
    }
}

static int kind_of_v2(uint8_t v2)
{
    for (int k = 0; k < 4; k++)
        if (KINDS[k].v2 == v2)
            return k;
    return -1;
}

static void curve_identity(vt_user_t *vt)
{
    for (int i = 0; i < 128; i++)
        vt->curve[i] = (uint8_t)i;
    memset(vt->curve_exist, 0, sizeof vt->curve_exist);
}

static bool exist(const vt_user_t *vt, int i) { return (vt->curve_exist[i >> 3] >> (i & 7)) & 1; }

static void set_exist(vt_user_t *vt, int i, bool on)
{
    if (on) vt->curve_exist[i >> 3] |= (uint8_t)(1u << (i & 7));
    else vt->curve_exist[i >> 3] &= (uint8_t)~(1u << (i & 7));
}

/* Rebuild the curve by linear interpolation between operation points, with
 * the fixed ends (0 -> 0, 127 -> 127) when they are not points themselves. */
static void curve_from_points(vt_user_t *vt)
{
    int prev = 0;
    uint8_t prev_out = exist(vt, 0) ? vt->curve[0] : 0;
    for (int i = 1; i < 128; i++) {
        bool last = i == 127;
        if (!exist(vt, i) && !last)
            continue;
        uint8_t out = exist(vt, i) ? vt->curve[i] : 127;
        for (int k = prev + 1; k < i; k++)
            vt->curve[k] = (uint8_t)(prev_out + (out - prev_out) * (k - prev) / (i - prev));
        vt->curve[i] = out;
        prev = i;
        prev_out = out;
    }
    vt->curve[0] = exist(vt, 0) ? vt->curve[0] : 0;
}

void vt_apply(piano_t *p)
{
    for (int k = 0; k < KB_KEYS; k++)
        p->touch.per_note_volume[k] = (int8_t)ca_clampi(p->vt.key_volume[KB_LOWEST_NOTE + k] * 2 / 5, -20, 20);
    for (int n = 0; n < 128; n++)
        p->tuning.per_note_cents[n] = p->vt.tuning[n];
    for (int i = 0; i < 12; i++)
        p->tuning.user_temperament[i] = p->vt.temperament[i];
    memcpy(p->touch.user_table, p->vt.curve, 128);
    p->touch.user_table_valid = true;
}

static void load_pernote(piano_t *p, int k)
{
    int8_t *a = pernote_array(p, k);
    memset(a, 0, KINDS[k].size);
    if (p->kv)
        (void)kv_get(p->kv, KINDS[k].key, a, KINDS[k].size);
    for (int i = 0; i < KINDS[k].size; i++)
        a[i] = (int8_t)ca_clampi(a[i], -KINDS[k].lim, KINDS[k].lim);
}

/* kvstore record: curve[128] exist[16] name[16] */
static void load_curve(piano_t *p)
{
    uint8_t buf[160];
    curve_identity(&p->vt);
    p->vt.curve_name[0] = 0;
    int n = p->kv ? kv_get(p->kv, KV_KEY_USER_TOUCHCURVE, buf, sizeof buf) : -1;
    if (n >= 144) {
        memcpy(p->vt.curve, buf, 128);
        memcpy(p->vt.curve_exist, buf + 128, 16);
        size_t l = (size_t)n > 144 ? CA_MIN((size_t)n - 144, (size_t)16) : 0;
        memcpy(p->vt.curve_name, buf + 144, l);
        p->vt.curve_name[l] = 0;
    }
}

static void save_curve(piano_t *p)
{
    if (!p->kv)
        return;
    uint8_t buf[160];
    memcpy(buf, p->vt.curve, 128);
    memcpy(buf + 128, p->vt.curve_exist, 16);
    size_t l = strlen(p->vt.curve_name);
    memcpy(buf + 144, p->vt.curve_name, l);
    kv_put(p->kv, KV_KEY_USER_TOUCHCURVE, buf, (uint16_t)(144 + l));
}

void vt_init(piano_t *p)
{
    for (int k = 0; k < 4; k++)
        load_pernote(p, k);
    load_curve(p);
    vt_apply(p);
}

void vt_reset(piano_t *p)
{
    if (p->kv) {
        for (int k = 0; k < 4; k++)
            kv_delete(p->kv, KINDS[k].key);
        kv_delete(p->kv, KV_KEY_USER_TOUCHCURVE);
    }
    vt_init(p);
}

/* ---- piano -> controller ---- */

static void send_pernote(piano_t *p, midi_port_t port, int k)
{
    uint8_t body[3 + 2 + 128];
    const int8_t *a = pernote_array(p, k);
    body[0] = 0x50; body[1] = KINDS[k].v2; body[2] = 0x7F;
    body[3] = 0x7F; body[4] = 0x7F; /* Temporary memory, all octaves */
    for (int i = 0; i < KINDS[k].size; i++)
        body[5 + i] = (uint8_t)(a[i] + 64);
    kp_send_frame(&p->kp, port, 0x10, body, (uint16_t)(5 + KINDS[k].size));
}

static void send_curve(piano_t *p, midi_port_t port)
{
    /* name: TouchCurveBulkName [127, code point as 3 x 7 bit LE ...] */
    uint8_t body[3 + 1 + 3 * 16];
    size_t o = 0;
    body[o++] = 0x5A; body[o++] = 0x0C; body[o++] = 0x7F; body[o++] = 0x7F;
    for (size_t i = 0; p->vt.curve_name[i] && i < 16; i++) {
        body[o++] = (uint8_t)(p->vt.curve_name[i] & 0x7F);
        body[o++] = 0;
        body[o++] = 0;
    }
    kp_send_frame(&p->kp, port, 0x10, body, (uint16_t)o);
    /* points: the original single TouchCurveBulkOperationPoint
     * [127, (exist, vel) x 128] — a 267-byte frame, within the original app's
     * 511-byte receive limit, so the original PianoRemote editor can load it.
     * (The chunked 04/05/06 form is still accepted on receive.) */
    uint8_t d[4 + 2 * 128] = {0x5A, 0x0D, 0x7F, 0x7F};
    for (int i = 0; i < 128; i++) {
        d[4 + 2 * i] = exist(&p->vt, i) ? 1 : 0;
        d[5 + 2 * i] = p->vt.curve[i] & 0x7F;
    }
    kp_send_frame(&p->kp, port, 0x10, d, sizeof d);
}

static void send_edit_exit(piano_t *p, uint8_t v1, uint8_t v2, bool with_data, midi_port_t except)
{
    const uint8_t body[4] = {v1, v2, 0x7F, 0x00};
    for (int port = 0; port < MIDI_PORT_COUNT; port++)
        if (p->kp.connected[port] && port != (int)except)
            kp_send_frame(&p->kp, (midi_port_t)port, 0x31, body, with_data ? 4 : 3);
}

bool vt_request(piano_t *p, midi_port_t port, uint8_t v1, uint8_t v2)
{
    if (v1 == 0x50) {
        int k = kind_of_v2(v2);
        if (k < 0)
            return false;
        send_pernote(p, port, k);
        return true;
    }
    if (v1 == 0x5A && v2 == 0x7F) {
        send_curve(p, port);
        return true;
    }
    return false;
}

/* ---- controller -> piano ---- */

static void pernote_data(piano_t *p, int k, const uint8_t *d, uint8_t len)
{
    if (len < 3)
        return;
    int8_t *a = pernote_array(p, k);
    uint8_t oct = d[1];
    int n = len - 2;
    int start = (oct == 0x7F || KINDS[k].size == 12) ? 0 : oct * 12;
    for (int i = 0; i < n && start + i < KINDS[k].size; i++)
        a[start + i] = (int8_t)ca_clampi((int)d[2 + i] - 64, -KINDS[k].lim, KINDS[k].lim);
    vt_apply(p);
}

static void touch_curve_user(piano_t *p)
{
    const uint8_t user = TC_USER;
    kp_local_set(&p->kp, PRM_TOUCH_CURVE, SLOT_MAIN1, &user, 1);
    p->touch.curve = TC_USER;
}

void vt_learn_input(piano_t *p, uint8_t raw)
{
    vt_user_t *vt = &p->vt;
    if (vt->learn_n == 0 || raw < vt->learn_min) vt->learn_min = raw;
    if (vt->learn_n == 0 || raw > vt->learn_max) vt->learn_max = raw;
    vt->learn_sum += raw;
    vt->learn_n++;
}

/* Learned curve: the player's own softest..strongest strokes are spread
 * over the full velocity range (OpenGrand method, REMOTE_PROTOCOL.md §9). */
static void learn_finish(piano_t *p)
{
    vt_user_t *vt = &p->vt;
    vt->learning = false;
    if (vt->learn_n < 4 || vt->learn_max <= vt->learn_min)
        return;
    int lo = vt->learn_min, hi = vt->learn_max;
    for (int i = 0; i < 128; i++)
        vt->curve[i] = (uint8_t)ca_clampi(1 + (i - lo) * 126 / (hi - lo), 1, 127);
    vt->curve[0] = 0;
    memset(vt->curve_exist, 0, sizeof vt->curve_exist);
    vt_apply(p);
    touch_curve_user(p);
    for (int port = 0; port < MIDI_PORT_COUNT; port++)
        if (p->kp.connected[port])
            send_curve(p, (midi_port_t)port);
}

bool vt_apply_value(piano_t *p, param_id_t id, uint8_t fn, const uint8_t *d, uint8_t len, midi_port_t origin)
{
    (void)fn;
    vt_user_t *vt = &p->vt;
    switch (id) {
    case PRM_PER_NOTE_VOICING_DATA: pernote_data(p, 0, d, len); return true;
    case PRM_PER_NOTE_KEY_VOLUME_DATA: pernote_data(p, 1, d, len); return true;
    case PRM_PER_NOTE_TUNING_DATA: pernote_data(p, 2, d, len); return true;
    case PRM_PER_NOTE_TEMPERAMENT_DATA: pernote_data(p, 3, d, len); return true;
    case PRM_PER_NOTE_VOICING_SAVE_REMOTE:
    case PRM_PER_NOTE_KEY_VOLUME_SAVE_REMOTE:
    case PRM_PER_NOTE_TUNING_SAVE_REMOTE:
    case PRM_PER_NOTE_TEMPERAMENT_SAVE_REMOTE: {
        int k = kind_of_v2(PARAM_DESC[id].v2);
        if (k >= 0 && p->kv)
            kv_put(p->kv, KINDS[k].key, pernote_array(p, k), KINDS[k].size);
        return true;
    }
    case PRM_PER_NOTE_VOICING_EDIT_EXIT_MODE:
    case PRM_PER_NOTE_KEY_VOLUME_EDIT_EXIT_MODE:
    case PRM_PER_NOTE_TUNING_EDIT_EXIT_MODE:
    case PRM_PER_NOTE_TEMPERAMENT_EDIT_EXIT_MODE: {
        int k = kind_of_v2(PARAM_DESC[id].v2);
        if (k < 0)
            return true;
        load_pernote(p, k);
        vt_apply(p);
        send_edit_exit(p, 0x50, KINDS[k].v2, k != 3, origin);
        return true;
    }
    case PRM_TOUCH_CURVE_ONE_OPERATION_POINT_CHANGE:
        if (len >= 4) {
            set_exist(vt, d[0] & 127, false);
            set_exist(vt, d[1] & 127, d[2] != 0);
            if (d[2])
                vt->curve[d[1] & 127] = d[3] & 127;
            curve_from_points(vt);
            vt_apply(p);
        }
        return true;
    case PRM_TOUCH_CURVE_ALL_OPERATION_POINTS_CLEAR:
        curve_identity(vt);
        vt_apply(p);
        return true;
    case PRM_TOUCH_CURVE_OPERATION_POINT_HEADER:
    case PRM_TOUCH_CURVE_NAME_HEADER:
        if (id == PRM_TOUCH_CURVE_NAME_HEADER)
            vt->curve_name[0] = 0;
        return true;
    case PRM_TOUCH_CURVE_OPERATION_POINT_DATA: /* [mem, offset, (exist, vel) ...] */
        for (int i = 0; len >= 2 && 2 + 2 * i + 1 < len; i++) {
            int x = d[1] + i;
            if (x > 127)
                break;
            set_exist(vt, x, d[2 + 2 * i] != 0);
            vt->curve[x] = d[3 + 2 * i] & 127;
        }
        return true;
    case PRM_TOUCH_CURVE_OPERATION_POINT_END:
        vt_apply(p);
        return true;
    case PRM_TOUCH_CURVE_NAME_DATA: { /* [mem, offset(chars), 3 bytes per code point ...] */
        if (len < 2)
            return true;
        size_t at = d[1];
        for (int i = 2; i + 2 < len + 0 && at < 16; i += 3)
            vt->curve_name[at++] = (char)((d[i] >= 0x20 && d[i] < 0x7F && !d[i + 1] && !d[i + 2]) ? d[i] : '?');
        if (at <= 16)
            vt->curve_name[at] = 0;
        return true;
    }
    case PRM_TOUCH_CURVE_NAME_END: return true;
    case PRM_TOUCH_CURVE_LEARN_REMOTE:
        if (len >= 2 && d[1] == 1) {
            vt->learning = true;
            vt->learn_n = 0;
            vt->learn_sum = 0;
        } else if (len >= 2 && vt->learning) {
            learn_finish(p);
        }
        return true;
    case PRM_TOUCH_CURVE_SAVE_REMOTE:
        save_curve(p);
        return true;
    case PRM_TOUCH_CURVE_EDIT_EXIT_MODE:
        load_curve(p);
        vt_apply(p);
        send_edit_exit(p, 0x5A, 0x09, false, origin);
        return true;
    default:
        return false;
    }
}

/* Frames without a descriptor: TouchCurveBulkOperationPoint (257 data bytes),
 * TouchCurveBulkName, TouchCurveMemoryNumber. */
bool vt_ext(piano_t *p, midi_port_t port, uint8_t fn, uint8_t v1, uint8_t v2, const uint8_t *d, uint16_t len)
{
    (void)port;
    if (fn != 0x10 || v1 != 0x5A)
        return false;
    vt_user_t *vt = &p->vt;
    switch (v2) {
    case 0x00: /* TouchCurveMemoryNumber [127 = Temporary] */
        return true;
    case 0x0C: { /* TouchCurveBulkName [mem, 3 x 7 bit per code point] */
        size_t at = 0;
        for (uint16_t i = 1; i + 2 < len + 0 && at < 16; i += 3)
            vt->curve_name[at++] = (char)((d[i] >= 0x20 && d[i] < 0x7F && !d[i + 1] && !d[i + 2]) ? d[i] : '?');
        vt->curve_name[at] = 0;
        return true;
    }
    case 0x0D: /* TouchCurveBulkOperationPoint [mem, (exist, vel) x 128] */
        for (int i = 0; i < 128 && 2 + 2 * i < len; i++) {
            set_exist(vt, i, d[1 + 2 * i] != 0);
            vt->curve[i] = d[2 + 2 * i] & 127;
        }
        vt_apply(p);
        return true;
    default:
        return false;
    }
}
