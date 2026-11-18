/* OpenGrand — song functions on top of the recorder:
 *   - MusicMode (52 1D): 0 Exit, 2 Internal recorder, 3 USB recorder,
 *     4 Lesson, 5 Concert Magic, 6 Piano Music (USB SMF player);
 *   - Music transport (52 08), SongSelect (52 00), IntSongPart (52 01) as the
 *     part play mask, MusicTempo (52 02), A-B repeat (52 06), song transpose
 *     (TransposeValue 53 02 on part 0x7F);
 *   - recorder persistence through the platform (save_song / load_song);
 *   - Concert Magic (concert.c) and its extension messages 52 0B / 52 0D;
 *   - InternalSongDelete (32 52 21) with ResetCompleteInfo (32 52 22). */
#include <string.h>

#include "ca79ng/util.h"
#include "piano_int.h"

static rec_song_t *cur_song(piano_t *p) { return &p->rec.song[p->rec.cur_song]; }

static void send_all(piano_t *p, uint8_t fn, const uint8_t *body, uint16_t n, midi_port_t except)
{
    for (int port = 0; port < MIDI_PORT_COUNT; port++)
        if (p->kp.connected[port] && port != (int)except)
            kp_send_frame(&p->kp, (midi_port_t)port, fn, body, n);
}

/* ---------------- song output ---------------- */

static void song_all_off(piano_t *p)
{
    for (int i = 0; i < VOICE_MAX; i++) {
        voice_t *v = &p->voices.v[i];
        if (v->active && v->key_down && (v->part >= PART_MIDI_BASE || v->part == PART_MAIN1 || v->part == PART_LAYER ||
                                         v->part == PART_LOWER || v->part == PART_4H_LEFT || v->part == PART_4H_RIGHT))
            voice_note_off(&p->voices, v->part, v->note);
    }
}

/* Recorder playback output. Internal (recorded) songs are physical key
 * events replayed through the current keyboard layout; SMF-loaded songs
 * (ext) play their MIDI channels on the 16 song parts. The song transpose
 * shifts both. */
void music_rec_out(void *ctx, uint8_t part, uint8_t status, uint8_t d1, uint8_t d2)
{
    piano_t *p = ctx;
    (void)part;
    bool ext = cur_song(p)->ext;
    uint8_t type = status & 0xF0, ch = status & 0x0F;
    if (type == 0x90 || type == 0x80) {
        int note = d1 + p->song_transpose;
        if (note < 0 || note > 127)
            return;
        uint8_t vel = type == 0x90 ? d2 : 0;
        if (ext) {
            pn_note(p, (uint8_t)(PART_MIDI_BASE + ch), (uint8_t)note, vel);
            return;
        }
        part_route_t r[2];
        int n = kb_layout_route(&p->layout, note, r);
        for (int i = 0; i < n; i++)
            if (r[i].note >= 0)
                pn_note(p, r[i].part, (uint8_t)r[i].note, vel);
    } else if (type == 0xB0) {
        if (d1 == 123 || d1 == 120) {
            song_all_off(p);
            return;
        }
        uint8_t prt = ext ? (uint8_t)(PART_MIDI_BASE + ch) : PART_MAIN1;
        if (ext && d1 == 0) { p->router.bank_msb[ch] = d2; return; }
        if (ext && d1 == 32) { p->router.bank_lsb[ch] = d2; return; }
        if (!ext || d1 == 7 || d1 == 10 || d1 == 11 || d1 == 64 || d1 == 66 || d1 == 67)
            pn_control(p, prt, d1, d2);
    } else if (type == 0xC0 && ext) {
        /* SMF files are GM/GM2 oriented: GM2 map with bank select */
        uint8_t id[3];
        if (progmap_lookup(MT_ON2, p->router.bank_msb[ch], p->router.bank_lsb[ch], d1, id)) {
            part_state_t *ps = &p->midi_part[ch];
            ps->msb = id[0]; ps->lsb = id[1]; ps->program = id[2];
            if (p->engine && p->engine->set_param)
                p->engine->set_param(p->engine->ctx, PRM_SOUND, (uint8_t)(PART_MIDI_BASE + ch),
                                     (int32_t)(id[0] << 16 | id[1] << 8 | id[2]));
        }
    }
}

/* ---------------- persistence ---------------- */

/* Heuristic for songs restored at boot: anything using channels other than
 * 1/2 or program changes was an imported SMF (play as MIDI channels). */
static bool looks_ext(const recorder_t *r, uint8_t song)
{
    uint32_t n;
    const rec_event_t *e = recorder_events(r, song, &n);
    for (uint32_t i = 0; i < n; i++)
        if ((e[i].status & 0x0F) > 1 || (e[i].status & 0xF0) == 0xC0)
            return true;
    return false;
}

void music_init(piano_t *p)
{
    p->cm.mode = CM_MODE_ENSEMBLE;
    if (p->plat.load_song && p->plat.smf_buf) {
        for (uint8_t s = 0; s < REC_SONGS; s++) {
            int32_t n = p->plat.load_song(p->plat.ctx, s, p->plat.smf_buf, p->plat.smf_cap);
            if (n > 0 && (uint32_t)n <= p->plat.smf_cap && smf_read(&p->rec, s, p->plat.smf_buf, (size_t)n))
                p->rec.song[s].ext = looks_ext(&p->rec, s);
        }
    }
    p->rec.dirty = 0;
    recorder_select(&p->rec, 0);
}

static void persist(piano_t *p)
{
    if (!p->rec.dirty || p->rec.state == REC_RECORDING || kp_file_busy(&p->kp))
        return;
    for (uint8_t s = 0; s < REC_SLOTS; s++) {
        uint16_t bit = (uint16_t)(1u << s);
        if (!(p->rec.dirty & bit))
            continue;
        p->rec.dirty &= (uint16_t)~bit;
        if (s == REC_SLOT_EXT) {
            /* USB recorder: a finished recording is written to the stick */
            if (p->music_mode == MM_USB_REC && p->rec.song[s].count)
                (void)usb_save_song(p, s, NULL);
            continue;
        }
        if (!p->plat.save_song)
            continue;
        size_t n = 0;
        if (p->rec.song[s].count && p->plat.smf_buf)
            n = smf_write(&p->rec, s, p->plat.smf_buf, p->plat.smf_cap);
        p->plat.save_song(p->plat.ctx, s, p->plat.smf_buf, (uint32_t)n);
    }
}

void music_song_loaded(piano_t *p, uint8_t slot)
{
    p->rec.dirty &= (uint16_t)~(1u << slot); /* just loaded: nothing to save */
    p->rec.song[slot].ext = true;
    recorder_select(&p->rec, slot);
    p->music_bpm = 0;
    recorder_set_tempo(&p->rec, 0);
    uint8_t t[2];
    bpm_encode(p->rec.song[slot].bpm, t);
    kp_local_set(&p->kp, PRM_MUSIC_TEMPO, SLOT_SYSTEM, t, 2);
}

void music_erase_all(piano_t *p)
{
    recorder_stop(&p->rec);
    cm_stop(&p->cm);
    for (uint8_t s = 0; s < REC_SONGS; s++)
        recorder_erase(&p->rec, s, -1);
}

/* ---------------- Concert Magic glue ---------------- */

static void cm_emit(void *ctx, const rec_event_t *e, uint8_t vel)
{
    music_rec_out(ctx, e->part, (uint8_t)((vel ? 0x90 : 0x80) | (e->status & 0x0F)), e->d1, vel);
}

/* Ensemble: the accompaniment runs on the song clock at the tapped tempo
 * and waits just before the next trigger group. */
static void cm_clock(piano_t *p, bool catch_up)
{
    recorder_t *r = &p->rec;
    if (p->cm.mode != CM_MODE_ENSEMBLE)
        return;
    if (catch_up && r->state == REC_PLAYING && p->cm.group_ms > r->pos_ms) {
        uint32_t keep = r->tempo_q16;
        r->tempo_q16 = 65536;
        r->limit_ms = p->cm.group_ms;
        recorder_tick(r, p->cm.group_ms - r->pos_ms);
        r->tempo_q16 = keep;
    }
    r->limit_ms = p->cm.next_ms == UINT32_MAX ? UINT32_MAX : (p->cm.next_ms ? p->cm.next_ms - 1 : 0);
    recorder_set_tempo(r, p->music_bpm);
    r->tempo_q16 = (uint32_t)(((uint64_t)r->tempo_q16 * p->cm.ratio_q16) >> 16);
}

static void cm_begin(piano_t *p)
{
    recorder_t *r = &p->rec;
    recorder_stop(r);
    uint32_t n;
    const rec_event_t *ev = recorder_events(r, r->cur_song, &n);
    bool ext = cur_song(p)->ext;
    cm_start(&p->cm, ev, n, ext);
    if (p->cm.mode == CM_MODE_ENSEMBLE) {
        if (ext) {
            r->ch_mask = (uint16_t)~(1u << p->cm.trigger);
            recorder_play(r, 3);
        } else {
            r->ch_mask = 0xFFFF;
            recorder_play(r, p->cm.trigger ? 1 : 2);
        }
        cm_clock(p, false);
    }
}

static void cm_end(piano_t *p)
{
    cm_stop(&p->cm);
    p->rec.limit_ms = UINT32_MAX;
    p->rec.ch_mask = 0xFFFF;
    recorder_stop(&p->rec);
}

bool music_key(piano_t *p, uint8_t note, uint8_t vel, bool on)
{
    if (p->music_mode != MM_CONCERT || !p->cm.active)
        return false;
    uint32_t n;
    const rec_event_t *ev = recorder_events(&p->rec, p->rec.cur_song, &n);
    if (!on) {
        cm_key_off(&p->cm, note, cm_emit, p);
        return true;
    }
    cm_result_t res = cm_key_on(&p->cm, ev, n, note, vel, p->now_ms, cm_emit, p);
    if (res != CM_NG)
        cm_clock(p, true);
    const uint8_t body[3] = {(uint8_t)res, (uint8_t)((p->cm.groups >> 7) & 0x7F), (uint8_t)(p->cm.groups & 0x7F)};
    kp_send_status(&p->kp, 0x10, EXT_V1_MUSIC, EXT_CM_RESULT, 0x7F, body, 3);
    return true;
}

/* ---------------- transport & modes ---------------- */

static void music_command(piano_t *p, uint8_t cmd)
{
    recorder_t *r = &p->rec;
    switch (cmd) {
    case 0:
        recorder_pause(r);
        break;
    case 1:
        if (p->music_mode == MM_CONCERT) {
            cm_begin(p);
        } else {
            recorder_set_tempo(r, p->music_bpm);
            recorder_play(r, r->play_mask);
        }
        break;
    case 2:
        if (p->cm.active)
            cm_end(p);
        else
            recorder_stop(r);
        break;
    case 3:
    case 4:
        if (p->music_mode == MM_USB_REC && r->cur_song != REC_SLOT_EXT) {
            recorder_select(r, REC_SLOT_EXT);
            recorder_erase(r, REC_SLOT_EXT, -1);
            r->song[REC_SLOT_EXT].ext = false;
        }
        if (cmd == 3 || r->state != REC_STANDBY)
            recorder_standby(r, r->rec_part, p->metro.bpm, p->metro.numerator, p->metro.denominator);
        if (cmd == 4)
            recorder_start(r);
        break;
    case 7: recorder_seek(r, r->pos_ms + 4000); break;
    case 8: recorder_seek(r, r->pos_ms > 4000 ? r->pos_ms - 4000 : 0); break;
    default: break;
    }
}

static void set_mode(piano_t *p, uint8_t m)
{
    if (m > MM_PIANO_MUSIC)
        return;
    if (p->cm.active)
        cm_end(p);
    recorder_stop(&p->rec);
    p->music_mode = m;
    switch (m) {
    case MM_INTERNAL:
        if (p->rec.cur_song >= REC_SONGS)
            recorder_select(&p->rec, 0);
        break;
    case MM_USB_REC:
    case MM_PIANO_MUSIC:
        recorder_select(&p->rec, REC_SLOT_EXT);
        break;
    default: /* Lesson / Concert Magic keep the selected song (internal or USB) */
        break;
    }
}

void music_set_transpose(piano_t *p, int t)
{
    song_all_off(p);
    p->song_transpose = (int8_t)ca_clampi(t, -12, 12);
}

/* What the stock tells a controller about the selected internal song (SongSelect / IntSongPart): Result 0xEC when the song (or the chosen part) has no data,
 * then MaxTime 10 52 18 7F [0, bars hi7, bars lo7, 0, 0, 0] = bars of the longest part (0 if none). */
static void song_report(piano_t *p, int part)
{
    if (p->booting)
        return;
    recorder_t *r = &p->rec;
    if (r->cur_song >= REC_SONGS)
        return;
    const rec_song_t *s = &r->song[r->cur_song];
    uint32_t last = 0, n = 0;
    for (uint32_t i = s->first; i < s->first + s->count; i++)
        if (part < 0 || r->pool[i].part == part) {
            n++;
            if (r->pool[i].ms > last)
                last = r->pool[i].ms;
        }
    uint32_t bars = 0;
    if (n) {
        uint32_t bpm = s->bpm ? s->bpm : 120, num = s->numerator ? s->numerator : 4;
        uint32_t bar_ms = 60000u * num / bpm;
        bars = last / bar_ms + 1;
        if (bars > 0x3FFF)
            bars = 0x3FFF;
    } else {
        for (int port = 0; port < MIDI_PORT_COUNT; port++)
            if (p->kp.connected[port])
                kp_send_result(&p->kp, (midi_port_t)port, KP_RES_NO_DATA);
    }
    const uint8_t mt[6] = {0, (uint8_t)(bars >> 7 & 0x7F), (uint8_t)(bars & 0x7F), 0, 0, 0};
    kp_send_status(&p->kp, 0x10, 0x52, 0x18, 0x7F, mt, 6);
}

bool music_apply_value(piano_t *p, param_id_t id, const uint8_t *d, uint8_t len)
{
    uint8_t v0 = len ? d[0] : 0;
    recorder_t *r = &p->rec;
    switch (id) {
    case PRM_MUSIC_MODE:
    case PRM_MUSIC_MODE_MODE:
        set_mode(p, v0);
        return true;
    case PRM_MUSIC:
    case PRM_MUSIC_REMOTE:
        music_command(p, v0);
        return true;
    case PRM_SONG_SELECT: { /* [numType 0 song / 1 file list id, num hi7, lo7] */
        if (len < 3)
            return true;
        uint32_t num = (uint32_t)d[1] << 7 | d[2];
        if (d[0] == 0 && num < REC_SONGS) {
            if (p->cm.active)
                cm_end(p);
            recorder_select(r, (uint8_t)num);
            p->music_bpm = 0;
            recorder_set_tempo(r, 0);
            pn_store_music_tempo(p);   /* the store answers get / dump: no song tempo now */
            song_report(p, -1);
        } else if (d[0] == 1 && !p->booting) {
            uint16_t res = usb_select_id(p, num);
            if (res != KP_RES_OK)
                for (int port = 0; port < MIDI_PORT_COUNT; port++)
                    if (p->kp.connected[port])
                        kp_send_result(&p->kp, (midi_port_t)port, res);
        }
        return true;
    }
    case PRM_INT_SONG_PART:
        r->rec_part = v0 == 1 ? 1 : 0;
        r->play_mask = v0 == 0x7F ? 3 : (uint8_t)(1u << (v0 & 1));
        song_report(p, v0 == 0x7F ? -1 : (v0 & 1));
        return true;
    case PRM_MUSIC_TEMPO:
        if (len >= 2) {
            int bpm = bpm_decode(d);
            p->music_bpm = (uint16_t)(bpm >= 10 && bpm <= 500 ? bpm : 0);
            if (p->cm.active)
                cm_clock(p, false);
            else
                recorder_set_tempo(r, p->music_bpm);
        }
        return true;
    case PRM_ABREPEAT:
    case PRM_ABREPEAT_REMOTE:
        if (v0 == 0) recorder_set_ab(r, false, 0, 0);
        else if (v0 == 1) r->a_ms = r->pos_ms;
        else if (v0 == 2) { recorder_set_ab(r, true, r->a_ms, r->pos_ms); recorder_seek(r, r->a_ms); }
        return true;
    case PRM_INTERNAL_SONG_DELETE_REMOTE: { /* [format, song hi7, lo7, part]; 7F 7F 7F 7F = all */
        if (len < 4)
            return true;
        /* stock apply 0x40160AD4: d[0] must be 7F or 04; hi:lo 7F:7F = all songs (format) */
        if (d[0] != 0x7F && d[0] != 0x04)
            return true;
        uint8_t info[3];
        if (d[1] == 0x7F && d[2] == 0x7F) {
            music_erase_all(p);
            info[0] = 0; info[1] = 0x7F; info[2] = 0;
        } else {
            uint32_t s = (uint32_t)d[1] << 7 | d[2];
            if (s >= REC_SONGS)
                return true;
            recorder_erase(r, (uint8_t)s, d[3] == 0x7F ? -1 : (d[3] & 1));
            info[0] = 0; info[1] = d[1]; info[2] = d[2];
        }
        kp_send_status(&p->kp, 0x32, 0x52, EXT_RESET_COMPLETE, 0x7F, info, 3); /* ResetCompleteInfo */
        return true;
    }
    default:
        return false;
    }
}

/* ---------------- extension messages ---------------- */

static void send_cm_setting(piano_t *p, midi_port_t port, bool all)
{
    const uint8_t body[6] = {EXT_V1_MUSIC, EXT_CM_SETTING, 0x7F, p->cm.mode, p->cm.smooth, p->cm.trigger};
    if (all) send_all(p, 0x10, body, 6, port);
    else kp_send_frame(&p->kp, port, 0x10, body, 6);
}

bool music_ext(piano_t *p, midi_port_t port, uint8_t fn, uint8_t v1, uint8_t v2, const uint8_t *d, uint16_t len)
{
    if (fn != 0x10 || v1 != EXT_V1_MUSIC)
        return false;
    if (v2 == EXT_CM_SETTING && len >= 3) {
        if (d[0] > 1 || d[1] > 1 || d[2] > 1)
            return false;
        p->cm.mode = d[0];
        p->cm.smooth = d[1] != 0;
        p->cm.trigger = d[2];
        send_cm_setting(p, port, true);
        return true;
    }
    return false;
}

bool music_request(piano_t *p, midi_port_t port, uint8_t v1, uint8_t v2)
{
    if (v1 != EXT_V1_MUSIC)
        return false;
    if (v2 == EXT_CM_SETTING) { send_cm_setting(p, port, false); return true; }
    return false;
}

void music_tick(piano_t *p, uint32_t elapsed_ms)
{
    (void)elapsed_ms;
    persist(p);
}
