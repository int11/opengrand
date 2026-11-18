#include "ca79ng/piano.h"

#include <string.h>

#include "ca79ng/util.h"
#include "piano_int.h"

/* ---------------- helpers ---------------- */

static part_state_t *part_of_slot(piano_t *p, int slot)
{
    switch (slot) {
    case SLOT_MAIN1: return &p->layout.main;
    case SLOT_LAYER: return &p->layout.layer;
    case SLOT_LOWER: return &p->layout.lower;
    case SLOT_FOUR_HANDS_RIGHT: return &p->layout.right4h;
    case SLOT_FOUR_HANDS_LEFT: return &p->layout.left4h;
    default: return NULL;
    }
}

part_state_t *pn_part(piano_t *p, uint8_t part)
{
    if (part >= PART_MIDI_BASE && part < PART_MIDI_BASE + 16)
        return &p->midi_part[part - PART_MIDI_BASE];
    switch (part) {
    case PART_LAYER: return &p->layout.layer;
    case PART_LOWER: return &p->layout.lower;
    case PART_4H_RIGHT: return &p->layout.right4h;
    case PART_4H_LEFT: return &p->layout.left4h;
    default: return &p->layout.main;
    }
}

static void engine_param(piano_t *p, param_id_t id, int slot, int32_t v)
{
    if (p->engine && p->engine->set_param)
        p->engine->set_param(p->engine->ctx, (uint16_t)id, param_part_from_slot(slot), v);
}

/* Factory defaults with the OpenGrand firmware overrides applied. */
static void store_defaults(piano_t *p)
{
    param_store_defaults(&p->store);
    /* Bluetooth MIDI is Off by default (original panel v3.4.12 behaviour,
     * bt.h); the app's initdata says On, the firmware is the source of truth. */
    const uint8_t off = 0;
    param_put(&p->store, PRM_BLUETOOTH_MIDI, SLOT_SYSTEM, &off, 1);
}

/* The System-part values the metronome and song modules hold: the store answers dump / get, so it must have them from the start - the stock
 * holds the same values in its settings block / recorder state and its getter FUN_4015E960 answers them.
 * Only where the store has nothing yet (settings loaded from the kvstore stay); same bytes as the stock's
 * value frames (tempo hi7 lo7, beat [beats, denominator code], song tempo 00 00 = no song, parts 7F = both). */
static void put_missing(piano_t *p, param_id_t id, const uint8_t *d, uint8_t n)
{
    if (!(p->store.valid[id] & (1u << SLOT_SYSTEM)))
        param_put(&p->store, id, SLOT_SYSTEM, d, n);
}

void pn_store_music_tempo(piano_t *p)
{
    uint8_t t[2] = {0, 0};
    if (p->music_bpm)
        bpm_encode(p->music_bpm, t);
    param_put(&p->store, PRM_MUSIC_TEMPO, SLOT_SYSTEM, t, 2);
}

static void store_system_values(piano_t *p)
{
    uint8_t t[2], v;
    bpm_encode(p->metro.bpm, t);
    put_missing(p, PRM_METRONOME_TEMPO, t, 2);
    if (beat_encode(p->metro.numerator, p->metro.denominator, t))
        put_missing(p, PRM_BEAT_SELECT, t, 2);
    put_missing(p, PRM_RHYTHM_SELECT, &p->metro.rhythm, 1);
    put_missing(p, PRM_METRONOME_VOLUME, &p->metro.volume, 1);
    v = p->metro.mode == METRO_RHYTHM;
    put_missing(p, PRM_METRONOME_MODE, &v, 1);
    if (!(p->store.valid[PRM_MUSIC_TEMPO] & (1u << SLOT_SYSTEM)))
        pn_store_music_tempo(p);
    v = p->rec.play_mask == 3 ? 0x7F : (p->rec.play_mask & 2 ? 1 : 0);
    put_missing(p, PRM_INT_SONG_PART, &v, 1);
}

/* ---------------- sound side ---------------- */

void pn_note(piano_t *p, uint8_t part, uint8_t note, uint8_t vel)
{
    if (!vel) {
        voice_note_off(&p->voices, part, note);
        return;
    }
    int v = voice_note_on(&p->voices, part, note, vel);
    if (p->engine && p->engine->start) {
        const part_state_t *ps = pn_part(p, part);
        engine_note_t n = {.freq_hz = tuning_freq(&p->tuning, note), .velocity = vel, .part = part,
                           .msb = ps->msb, .lsb = ps->lsb, .program = ps->program,
                           .voicing = p->user_voicing ? p->vt.voicing[note & 127] : 0};
        p->engine->start(p->engine->ctx, v, &n);
    }
}

void pn_control(piano_t *p, uint8_t part, uint8_t cc, uint8_t value)
{
    if (cc == 64 || cc == 66 || cc == 67)
        voice_pedal(&p->voices, part, cc, value);
    else if (p->engine && p->engine->control)
        p->engine->control(p->engine->ctx, part, cc, value);
}

static void snd_note(void *ctx, uint8_t part, uint8_t note, uint8_t vel) { pn_note(ctx, part, note, vel); }
static void snd_control(void *ctx, uint8_t part, uint8_t cc, uint8_t value) { pn_control(ctx, part, cc, value); }

/* Received program change: the program map depends on Multi-timbral Off/On1
 * (CA79 numbering) or On2 (GM2 numbering + bank), router.h. */
static void snd_program(void *ctx, uint8_t part, uint8_t msb, uint8_t lsb, uint8_t pc)
{
    piano_t *p = ctx;
    uint8_t id[3];
    if (!progmap_lookup(p->router.cfg.multitimbral, msb, lsb, pc, id))
        return;
    if (part == PART_MAIN1) {
        kp_local_set(&p->kp, PRM_SOUND, SLOT_MAIN1, id, 3);
        piano_apply(p, PRM_SOUND, SLOT_MAIN1);
        return;
    }
    part_state_t *ps = pn_part(p, part);
    ps->msb = id[0]; ps->lsb = id[1]; ps->program = id[2];
    if (p->engine && p->engine->set_param)
        p->engine->set_param(p->engine->ctx, PRM_SOUND, part, (int32_t)(id[0] << 16 | id[1] << 8 | id[2]));
}

static void snd_all_off(void *ctx, uint8_t part)
{
    piano_t *p = ctx;
    for (int i = 0; i < VOICE_MAX; i++)
        if (p->voices.v[i].active && p->voices.v[i].part == part && p->voices.v[i].key_down)
            voice_note_off(&p->voices, part, p->voices.v[i].note);
}

static void midi_tx(void *ctx, midi_port_t port, const uint8_t *b, uint16_t n)
{
    piano_t *p = ctx;
    if (p->plat.midi_tx)
        p->plat.midi_tx(p->plat.ctx, port, b, n);
}

static void ble_uart_tx(void *ctx, const uint8_t *b, uint16_t n) { midi_tx(ctx, MIDI_PORT_BLE, b, n); }

/* MIDI parts 1..16: sound, controllers and pedals back to power-on state. */
void pn_reset_midi_parts(piano_t *p)
{
    for (int ch = 0; ch < 16; ch++) {
        uint8_t part = (uint8_t)(PART_MIDI_BASE + ch);
        snd_all_off(p, part);
        p->midi_part[ch] = (part_state_t){.msb = SOUND_TABLE[0].msb, .lsb = SOUND_TABLE[0].lsb,
                                          .program = SOUND_TABLE[0].pc, .volume = 100, .enabled = true};
        voice_pedal(&p->voices, part, 64, 0);
        voice_pedal(&p->voices, part, 66, 0);
        voice_pedal(&p->voices, part, 67, 0);
        if (p->engine && p->engine->control) {
            p->engine->control(p->engine->ctx, part, 7, 100);
            p->engine->control(p->engine->ctx, part, 10, 64);
            p->engine->control(p->engine->ctx, part, 11, 127);
        }
    }
    router_reset_banks(&p->router);
}

/* ---------------- kproto callbacks ---------------- */

static void apply_value(piano_t *p, param_id_t id, int slot, uint8_t fn, const uint8_t *d, uint8_t len, midi_port_t origin);
static void on_midi(void *ctx, const midi_msg_t *m);

static void kp_param(void *ctx, param_id_t id, int slot, uint8_t fn, const uint8_t *d, uint8_t len, midi_port_t origin)
{
    piano_t *p = ctx;
    power_timer_activity(&p->power);
    apply_value(p, id, slot, fn, d, len, origin);
}

static bool is_volume_param(param_id_t id) { return id == PRM_VOLUME || id == PRM_VOLUME_2; }

/* Lock mode (OpenGrand semantics, REMOTE_PROTOCOL.md §9):
 *   Panel  - every parameter set/command arriving from the built-in panel
 *            port is rejected (NACK), except LockMode itself (unlock);
 *   Volume - master volume (slider, universal SysEx) and Volume (55 01)
 *            changes are ignored from every source. */
static bool kp_accept(void *ctx, param_id_t id, int slot, uint8_t fn, const uint8_t *d, uint8_t len, midi_port_t origin)
{
    piano_t *p = ctx;
    (void)slot; (void)fn; (void)d; (void)len;
    if ((p->lock & LOCK_PANEL) && origin == MIDI_PORT_PANEL && id != PRM_LOCK_MODE)
        return false;
    if ((p->lock & LOCK_VOLUME) && is_volume_param(id))
        return false;
    return true;
}

static bool kp_ext(void *ctx, midi_port_t port, uint8_t fn, uint8_t v1, uint8_t v2, uint8_t part, const uint8_t *d, uint16_t len)
{
    piano_t *p = ctx;
    if (part != 0x7F)
        return false;
    if ((p->lock & LOCK_PANEL) && port == MIDI_PORT_PANEL)
        return false;
    power_timer_activity(&p->power);
    return vt_ext(p, port, fn, v1, v2, d, len) || music_ext(p, port, fn, v1, v2, d, len);
}

static void send_ble_name(piano_t *p, midi_port_t port)
{
    uint8_t body[3 + 23] = {0x51, 0x3E, 0x7F};
    for (int i = 0; i < 23 && p->ble.name[i]; i++)
        body[3 + i] = (uint8_t)(p->ble.name[i] & 0x7F);
    kp_send_frame(&p->kp, port, 0x10, body, sizeof body);
}

static bool kp_request(void *ctx, midi_port_t port, uint8_t v1, uint8_t v2, uint8_t part)
{
    piano_t *p = ctx;
    if (part != 0x7F)
        return false;
    if (v1 == 0x51 && v2 == 0x3E) { /* BluetoothMIDIName */
        send_ble_name(p, port);
        return true;
    }
    return vt_request(p, port, v1, v2) || music_request(p, port, v1, v2);
}

static void kp_file(void *ctx, midi_port_t port, uint8_t kind, const uint8_t *d, uint32_t len)
{
    usb_file_upload(ctx, port, kind, d, len);
}

static size_t put_ascii(uint8_t *out, size_t cap, const char *s)
{
    size_t l = s ? strlen(s) : 0;
    if (l > cap) l = cap;
    memcpy(out, s, l);
    return l;
}

/* Information: RomName / Version / Checksum per component (0 main, 1 DSP, 2 BLE). */
static int kp_info(void *ctx, uint8_t kind, uint8_t comp, uint8_t *out, size_t cap)
{
    piano_t *p = ctx;
    switch (kind) {
    case 0: /* RomName */
        if (comp == 0) return (int)put_ascii(out, cap, "OpenGrand MAIN");
        if (comp == 1) return (int)put_ascii(out, cap, "OpenGrand DSP");
        if (comp == 2) return (int)put_ascii(out, cap, p->ble.name);
        return -1;
    case 1: /* Version */
        if (comp == 0) return (int)put_ascii(out, cap, CA79NG_VERSION);
        if (comp == 1 && p->plat.dsp_version) return (int)put_ascii(out, cap, p->plat.dsp_version);
        if (comp == 2 && p->ble.fw_version[0]) return (int)put_ascii(out, cap, p->ble.fw_version);
        return -1;
    case 2: /* Checksum: 32 bit as 8 nibbles, MSB first */
        if (comp != 0 || cap < 8) return -1;
        for (int i = 0; i < 8; i++)
            out[i] = (uint8_t)((p->plat.fw_checksum >> (28 - 4 * i)) & 0x0F);
        return 8;
    default:
        return -1;
    }
}

/* recorder ownership (REMOTE_PROTOCOL §4): held while the recorder / player is busy */
static bool kp_excl_active(void *ctx, uint8_t kind)
{
    piano_t *p = ctx;
    return kind == KP_EX_RECORDER && (p->rec.state != REC_IDLE || p->cm.active);
}

void pn_power_off(piano_t *p)
{
    p->powered = false;
    if (p->plat.power_off)
        p->plat.power_off(p->plat.ctx);
}

static void set_master_volume(piano_t *p, uint8_t v)
{
    p->master_volume = v & 0x7F;
    if (p->engine && p->engine->control)
        p->engine->control(p->engine->ctx, PART_GLOBAL, 7, p->master_volume);
}

void piano_master_volume(piano_t *p, uint8_t value)
{
    if (p->lock & LOCK_VOLUME)
        return;
    set_master_volume(p, value);
}

static void kp_universal(void *ctx, const uint8_t *m, uint16_t n)
{
    piano_t *p = ctx;
    if (n >= 6 && m[1] == 0x7E && m[3] == 0x09) {
        /* GM System On (01) / Off (02) / GM2 On (03): reset the MIDI parts,
         * their controllers and the program map */
        voice_all_off(&p->voices, false);
        pn_reset_midi_parts(p);
    } else if (n >= 11 && m[1] == 0x41 && m[3] == 0x42 && m[4] == 0x12 && m[5] == 0x40 && m[6] == 0x00 &&
               m[7] == 0x7F && m[8] == 0x00) {
        /* Roland GS Reset F0 41 dd 42 12 40 00 7F 00 41 F7 (checksum 41) */
        if (((0x40 + 0x00 + 0x7F + 0x00 + m[9]) & 0x7F) == 0) {
            voice_all_off(&p->voices, false);
            pn_reset_midi_parts(p);
        }
    } else if (n >= 8 && m[1] == 0x7F && m[3] == 0x04 && m[4] == 0x01) { /* master volume */
        piano_master_volume(p, m[6]);
    } else if (n >= 8 && m[1] == 0x7F && m[3] == 0x04 && m[4] == 0x03) { /* master fine tuning (14 bit, 8192 = 0) */
        int v = (m[6] << 7 | m[5]) - 8192;
        p->tuning.master_hz = 440.0f * (1.0f + (float)v / 8192.0f * 0.0578f); /* +-100 cents */
    }
}

/* ---------------- init ---------------- */

void piano_init(piano_t *p, const piano_platform_t *plat, sound_engine_t *engine, kvstore_t *kv,
                rec_event_t *rec_pool, rec_event_t *rec_stage)
{
    memset(p, 0, sizeof *p);
    p->plat = *plat;
    p->engine = engine;
    p->kv = kv;

    store_defaults(p);
    if (kv)
        settings_load(&p->store, kv);

    kp_identity_t id = {.group = 0x04, .machine = 0x1E, .sub1 = 0x00, .sub2 = 0x02,
                        .destination = 0x01, .language = 0x00, .version = CA79NG_VERSION}; /* 05 01 00 01 as this unit (trace) */
    kp_io_t kio = {.ctx = p, .tx = midi_tx, .param = kp_param, .power_off = pw_remote_off, .universal = kp_universal,
                   .file = kp_file, .accept = kp_accept, .ext = kp_ext, .request = kp_request,
                   .file_op = usb_file_op, .file_list = usb_file_list, .info = kp_info,
                   .excl_active = kp_excl_active};
    kp_init(&p->kp, &p->store, &id, &kio);

    router_io_t rio = {.ctx = p, .note = snd_note, .control = snd_control, .program = snd_program,
                       .all_notes_off = snd_all_off, .tx = midi_tx};
    router_init(&p->router, &rio);
    for (int i = 0; i < MIDI_PORT_COUNT; i++)
        midi_parser_init(&p->parser[i], (midi_port_t)i, on_midi, p);

    key_scanner_init(&p->scanner);
    touch_config_default(&p->touch);
    pedal_config_default(&p->pedals);
    kb_layout_default(&p->layout);
    tuning_default(&p->tuning);
    voice_pool_init(&p->voices, engine);
    metronome_init(&p->metro);
    rhythm_pattern(0, &p->pattern);
    recorder_init(&p->rec, rec_pool, rec_stage, music_rec_out, p);
    power_timer_init(&p->power, 0);
    bt_audio_init(&p->bt);
    apo_hold_init(&p->apo_hold, NULL);
    pw_init(p); /* power policy from the kvstore, standby state machine */
    memset(p->key_sounding, -1, sizeof p->key_sounding);
    p->master_volume = 127;
    p->usb_selected = -1;
    p->preview.volume = 100;
    pn_reset_midi_parts(p);
    store_system_values(p);

    /* BLE-MIDI module: timestamps off, advertised name (kvstore or default),
     * firmware version query (Information) */
    char name[24] = "OpenGrand";
    if (kv) {
        int l = kv_get(kv, KV_KEY_BLE_NAME, name, sizeof name - 1);
        name[l > 0 ? l : 0] = 0;
        if (l <= 0)
            strcpy(name, "OpenGrand");
    }
    blemidi_io_t bio = {.ctx = p, .uart_tx = ble_uart_tx};
    blemidi_init(&p->ble, &bio, name);
    blemidi_request_firmware(&p->ble);
    p->router.cfg.tx_port[MIDI_PORT_BLE] = false;

    vt_init(p);
    p->powered = true;
    p->booting = true;
    piano_apply_all(p);
    music_init(p);
    p->booting = false;
}

/* ---------------- parameter application ---------------- */

void piano_apply(piano_t *p, param_id_t id, int slot)
{
    uint8_t v[4];
    if (param_get(&p->store, id, slot, v))
        apply_value(p, id, slot, PARAM_DESC[id].fn, v, PARAM_DESC[id].len, MIDI_PORT_INTERNAL);
}

void piano_apply_all(piano_t *p)
{
    for (int id = 0; id < PRM_COUNT; id++) {
        if (PARAM_DESC[id].fn != 0x10 || PARAM_DESC[id].var_len)
            continue;
        if (id == PRM_METRONOME || id == PRM_MUSIC || id == PRM_MUSIC_MODE || id == PRM_SOUND_PREVIEW_START ||
            id == PRM_SOUND_PREVIEW_STOP || id == PRM_USBMEMORY)
            continue; /* transport / one-shot state is never restored at boot */
        for (int s = 0; s < PARAM_SLOT_COUNT; s++)
            if (p->store.valid[id] & (1u << s))
                piano_apply(p, (param_id_t)id, s);
    }
}

static void set_rhythm(piano_t *p, uint8_t idx)
{
    p->metro.rhythm = (uint8_t)(idx % RHYTHM_COUNT);
    rhythm_pattern(p->metro.rhythm, &p->pattern);
    if (p->metro.mode == METRO_RHYTHM) {
        metronome_set_signature(&p->metro, p->pattern.beats, p->pattern.denominator);
        p->metro.steps_per_beat = p->pattern.steps_per_beat;
    }
}

static void set_ble(piano_t *p, bool on)
{
    blemidi_set_enabled(&p->ble, on);
    p->router.cfg.tx_port[MIDI_PORT_BLE] = on;
    if (!on)
        kp_link_down(&p->kp, MIDI_PORT_BLE); /* the phone's session ends with the link; the panel is told */
}

static void set_ble_name(piano_t *p, const uint8_t *d, uint8_t len)
{
    char name[24];
    size_t n = 0;
    for (uint8_t i = 0; i < len && i < 23; i++) {
        if (d[i] == 0)
            break;
        name[n++] = (d[i] >= 0x20 && d[i] < 0x7F) ? (char)d[i] : '?';
    }
    name[n] = 0;
    if (n == 0)
        return;
    blemidi_set_name(&p->ble, name);
    if (p->kv)
        kv_put(p->kv, KV_KEY_BLE_NAME, name, (uint16_t)n);
    for (int port = 0; port < MIDI_PORT_COUNT; port++)
        if (p->kp.connected[port])
            send_ble_name(p, (midi_port_t)port);
}

static void apply_value(piano_t *p, param_id_t id, int slot, uint8_t fn, const uint8_t *d, uint8_t len, midi_port_t origin)
{
    part_state_t *ps = part_of_slot(p, slot);
    uint8_t v0 = len ? d[0] : 0;
    if (vt_apply_value(p, id, fn, d, len, origin) || music_apply_value(p, id, d, len) || usb_apply_value(p, id, d, len))
        return;
    switch (id) {
    /* --- sound & keyboard modes --- */
    case PRM_SOUND:
        if (ps && len >= 3) {
            ps->msb = d[0]; ps->lsb = d[1]; ps->program = d[2];
            if (slot == SLOT_MAIN1 && !p->booting) /* no PC burst at power-on */
                router_key_program(&p->router, d[0], d[1], d[2]);
        }
        engine_param(p, id, slot, len >= 3 ? (d[0] << 16 | d[1] << 8 | d[2]) : 0);
        return;
    case PRM_KEYBOARD_MODE:
    case PRM_KEYBOARD_MODE_MODE:
        if (v0 <= KBMODE_4HANDS) {
            voice_all_off(&p->voices, false);
            p->layout.mode = (kb_mode_t)v0;
            p->router.kb_mode = (kb_mode_t)v0;
        }
        return;
    case PRM_TRANSPOSE_VALUE:
        /* part 0x0F (Global) = key transpose; part 0x7F (System) = song
         * transpose (app contract, REMOTE_PROTOCOL.md §9) */
        if (slot == SLOT_SYSTEM)
            music_set_transpose(p, v0 - 64);
        else
            p->tuning.transpose = (int8_t)ca_clampi(v0 - 64, -12, 12);
        return;
    case PRM_TRANSPOSE_VALUE_2: if (ps) ps->transpose = (int8_t)ca_clampi(v0 - 64, -24, 24); return;
    case PRM_OCTAVE_SHIFT: if (ps) ps->octave_shift = (int8_t)ca_clampi(v0 - 64, -3, 3); return;
    case PRM_BALANCE: p->layout.balance = v0; return;
    case PRM_SPLIT_POINT: p->layout.split_point = v0; return;
    case PRM_LOWER_PEDAL: p->layout.lower_pedal = v0 != 0; return;
    case PRM_DYNAMICS: p->touch.dynamics = v0; return;
    /* --- touch / pedals / tuning (Virtual Technician) --- */
    case PRM_TOUCH_CURVE: p->touch.curve = (touch_curve_t)v0; break;
    case PRM_MINIMUM_TOUCH: p->touch.minimum_touch = (uint8_t)ca_clampi(v0, 1, 20); break;
    case PRM_KEY_VOLUME: p->touch.key_volume = (key_volume_t)v0; break;
    case PRM_VOICING:
        if (slot == SLOT_MAIN1)
            p->user_voicing = v0 == 0x40;
        break;
    case PRM_HALF_PEDAL_ADJUST: p->pedals.half_pedal_adjust = (uint8_t)param_range_decode(id, v0); break;
    case PRM_SOFT_PEDAL_DEPTH:
        /* the engine gets the decoded depth 1..10 (soft amount scale) */
        p->pedals.soft_depth = (uint8_t)param_range_decode(id, v0);
        engine_param(p, id, slot, p->pedals.soft_depth);
        return;
    case PRM_HAMMER_DELAY:
        if (slot == SLOT_MAIN1)
            p->hammer_delay = (uint8_t)param_range_decode(id, v0);
        break;
    case PRM_TUNING: p->tuning.master_hz = tuning_raw_to_hz(v0); break;
    case PRM_TEMPERAMENT:
        if (len >= 2) {
            p->tuning.temperament = v0 < TEMP_USER ? (temperament_t)v0 : TEMP_USER;
            p->tuning.temperament_key = (uint8_t)(d[1] % 12);
        }
        break;
    case PRM_STRETCH_TUNING: p->tuning.stretch = (stretch_t)v0; break;
    case PRM_DAMPER_HOLD: voice_set_damper_hold(&p->voices, v0 != 0); break;
    /* --- MIDI --- */
    case PRM_MIDICHANNEL: router_set_channel(&p->router, (uint8_t)(v0 & 15)); return;
    case PRM_LOCAL_CONTROL: p->router.cfg.local = v0 != 0; return;
    case PRM_MULTI_TIMBRAL_MODE: p->router.cfg.multitimbral = (multitimbral_t)ca_clampi(v0, 0, 2); return;
    case PRM_TRANSMIT_PROGRAM_CHANGE: p->router.cfg.send_pc = len >= 2 ? d[1] != 0 : v0 != 0; return;
    case PRM_CHANNEL_MUTE:
    case PRM_CHANNEL_MUTE_2:
        if (slot >= SLOT_MIDI1CH)
            p->router.cfg.channel_play[slot - SLOT_MIDI1CH] = v0 != 0;
        return;
    /* --- metronome / rhythm --- */
    case PRM_METRONOME:
    case PRM_METRONOME_REMOTE:
        if (v0) metronome_start(&p->metro); else metronome_stop(&p->metro);
        return;
    case PRM_METRONOME_TEMPO: if (len >= 2) metronome_set_bpm(&p->metro, bpm_decode(d)); return;
    case PRM_BEAT_SELECT: {
        int num, den;
        if (len >= 2 && beat_decode(d, &num, &den))
            metronome_set_signature(&p->metro, num, den);
        return;
    }
    case PRM_RHYTHM_SELECT: set_rhythm(p, v0); break;
    case PRM_METRONOME_VOLUME: p->metro.volume = v0; return;
    case PRM_METRONOME_MODE:
    case PRM_METRONOME_MODE_MODE:
        p->metro.mode = v0 ? METRO_RHYTHM : METRO_CLICK;
        p->metro.steps_per_beat = 0;
        if (v0)
            set_rhythm(p, p->metro.rhythm);
        return;
    /* --- system --- */
    case PRM_AUTO_POWER_OFF: power_timer_init(&p->power, v0); return;
    case PRM_POWER_MANAGEMENT_TIMER_RESET_REMOTE: power_timer_activity(&p->power); return;
    /* the stock looks the command up by its first data byte (FUN_4014E8AC): 07 = the startup-settings descriptor,
     * 04 = the full factory reset; any other byte is a failed lookup = no action */
    case PRM_START_UP_SETTING_SAVE_REMOTE:
        if (v0 == 0x07 && p->kv) settings_save(&p->store, p->kv);
        return;
    case PRM_START_UP_SETTING_RESET_START_REMOTE:
        if (v0 == 0x04)
            piano_factory_reset(p);                       /* everything back to factory, now */
        else if (v0 == 0x07 && p->kv)
            settings_clear(p->kv);                        /* only the saved startup settings: factory from the next boot */
        return;
    case PRM_VOLUME:
    case PRM_VOLUME_2:
        if (p->engine && p->engine->control)
            p->engine->control(p->engine->ctx, param_part_from_slot(slot), 7, v0);
        return;
    case PRM_LOCK_MODE: p->lock = v0 & 3; return;
    case PRM_BLUETOOTH_MIDI: set_ble(p, v0 != 0); return;
    case PRM_BLUETOOTH_MIDINAME: set_ble_name(p, d, len); return;
    case PRM_SOUND_PREVIEW_START: preview_start(p, v0); return;
    case PRM_SOUND_PREVIEW_STOP: preview_stop(p); return;
    case PRM_SOUND_PREVIEW_VOLUME: p->preview.volume = v0; return;
    default:
        break;
    }
    /* everything else is a sound-engine parameter (VT, reverb, effects, EQ...) */
    engine_param(p, id, slot, v0);
}

void piano_factory_reset(piano_t *p)
{
    voice_all_off(&p->voices, true);
    metronome_stop(&p->metro);
    recorder_stop(&p->rec);
    preview_stop(p);
    if (p->kv)
        settings_clear(p->kv);
    vt_reset(p); /* user touch curve + per-note VT data back to factory (recorder songs are kept) */
    pw_factory_reset(p); /* power policy back to the owner defaults */
    store_defaults(p);
    piano_apply_all(p);
    for (int port = 0; port < MIDI_PORT_COUNT; port++)
        if (p->kp.connected[port])
            kp_dump_all(&p->kp, (midi_port_t)port);
    const uint8_t done[1] = {0x00};
    kp_send_status(&p->kp, 0x32, 0x51, 0x37, 0x7F, done, 1); /* StartUpSettingResetComplete */
}

/* ---------------- inputs ---------------- */

/* Note-on of a key through layout and router (hammer delay already applied). */
static void key_sound_on(piano_t *p, uint8_t key, uint8_t vel)
{
    for (int i = 0; i < 2; i++)
        if (p->key_sounding[key][i] >= 0)
            router_key_note(&p->router, p->key_part[key][i], (uint8_t)p->key_sounding[key][i], vel);
}

static void key_sound_off(piano_t *p, uint8_t key)
{
    if (p->hammer_wait[key]) { /* released before its delayed strike: strike now, then damp */
        p->hammer_wait[key] = 0;
        key_sound_on(p, key, p->hammer_vel[key]);
    }
    for (int i = 0; i < 2; i++) {
        if (p->key_sounding[key][i] >= 0)
            router_key_note(&p->router, p->key_part[key][i], (uint8_t)p->key_sounding[key][i], 0);
        p->key_sounding[key][i] = -1;
    }
}

/* Hammer Delay 0..10: soft strokes sound later, up to 30 ms at velocity 1
 * and setting 10; fortissimo (127) is never delayed. */
static uint8_t hammer_delay_ms(const piano_t *p, uint8_t vel)
{
    if (!p->hammer_delay)
        return 0;
    return (uint8_t)((uint32_t)p->hammer_delay * 3u * (127u - vel) / 126u);
}

void piano_key_edge(piano_t *p, uint8_t key, uint8_t sensor, bool closed, uint32_t t_us)
{
    if (!p->powered || key >= KB_KEYS)
        return;
    key_event_t ev = key_scanner_edge(&p->scanner, key, sensor, closed, t_us);
    if (ev.kind == KEY_EV_NONE)
        return;
    if (ev.kind == KEY_EV_ON && p->sb.state == SB_STANDBY)
        standby_wake(&p->sb, SB_WAKE_KEY); /* the note itself is still muted */
    power_timer_activity(&p->power);
    uint8_t note = (uint8_t)(KB_LOWEST_NOTE + key);
    if (ev.kind == KEY_EV_ON) {
        if (p->vt.learning)
            vt_learn_input(p, ev.velocity);
        uint8_t vel = touch_apply(&p->touch, key, ev.velocity);
        if (!vel)
            return;
        if (music_key(p, note, vel, true))
            return;
        /* release a still-sounding repetition first */
        key_sound_off(p, key);
        part_route_t r[2];
        int n = kb_layout_route(&p->layout, note, r);
        for (int i = 0; i < 2; i++) {
            p->key_sounding[key][i] = (i < n) ? r[i].note : -1;
            p->key_part[key][i] = (i < n) ? r[i].part : 0;
        }
        uint8_t delay = hammer_delay_ms(p, vel);
        if (delay) {
            /* scheduled in piano_tick, never from the key ISR */
            p->hammer_wait[key] = delay;
            p->hammer_vel[key] = vel;
        } else {
            key_sound_on(p, key, vel);
        }
        recorder_input(&p->rec, 0x90, note, vel);
    } else {
        if (music_key(p, note, 0, false))
            return;
        key_sound_off(p, key);
        recorder_input(&p->rec, 0x80, note, 0);
    }
}

void piano_pedal(piano_t *p, pedal_id_t id, uint16_t adc)
{
    if (!p->powered)
        return;
    uint8_t v = pedal_value(&p->pedals, id, adc);
    if (v == p->pedal_cc[id])
        return;
    p->pedal_cc[id] = v;
    power_timer_activity(&p->power);
    static const uint8_t CC[PEDAL_COUNT] = {64, 66, 67};
    uint8_t cc = CC[id];
    switch (p->layout.mode) {
    case KBMODE_4HANDS:
        /* right pedal = damper of the right half, left pedal = damper of the left half */
        if (id == PEDAL_DAMPER) router_key_control(&p->router, PART_4H_RIGHT, 64, v);
        else if (id == PEDAL_SOFT) router_key_control(&p->router, PART_4H_LEFT, 64, v);
        else router_key_control(&p->router, PART_4H_RIGHT, cc, v);
        break;
    case KBMODE_DUAL:
        router_key_control(&p->router, PART_MAIN1, cc, v);
        router_key_control(&p->router, PART_LAYER, cc, v);
        break;
    case KBMODE_SPLIT:
        router_key_control(&p->router, PART_MAIN1, cc, v);
        if (p->layout.lower_pedal || id != PEDAL_DAMPER)
            router_key_control(&p->router, PART_LOWER, cc, v);
        break;
    default:
        router_key_control(&p->router, PART_MAIN1, cc, v);
        break;
    }
    recorder_input(&p->rec, 0xB0, cc, v);
}

void piano_midi_in(piano_t *p, midi_port_t port, const uint8_t *bytes, uint16_t len)
{
    if (port < MIDI_PORT_COUNT)
        midi_parser_feed_buf(&p->parser[port], bytes, len);
}

static void on_midi(void *ctx, const midi_msg_t *m)
{
    piano_t *p = ctx;
    if (m->port == MIDI_PORT_BLE) {
        /* replies of the BLE module itself never reach kproto */
        if (m->kind == MIDI_MSG_SYSEX && blemidi_handle_sysex(&p->ble, m->sysex, m->sysex_len))
            return;
        if (!p->ble.enabled)
            return; /* Bluetooth MIDI Off: nothing from the radio is accepted */
    }
    if (m->kind == MIDI_MSG_SYSEX) {
        if (ng_sysex(p, m->port, m->sysex, m->sysex_len))
            return; /* OpenGrand extension (0x7D): never reaches kproto */
        pw_midi_wake(p, m->port, m->sysex, m->sysex_len);
        kp_handle_sysex(&p->kp, m->port, m->sysex, m->sysex_len);
    } else if (m->kind == MIDI_MSG_CHANNEL) {
        power_timer_activity(&p->power);
        router_rx(&p->router, m);
    }
    /* realtime (active sensing / clock) needs no action */
}

static void rhythm_step(piano_t *p, uint8_t step)
{
    if (!p->engine || !p->engine->drum)
        return;
    for (unsigned inst = 0; inst < DRUM_PATTERN_INSTS; inst++) {
        uint8_t v = rhythm_hit(&p->pattern, inst, step);
        if (v)
            p->engine->drum(p->engine->ctx, (uint8_t)inst, (uint8_t)ca_clampi(v * p->metro.volume / 127, 1, 127));
    }
}

void piano_tick(piano_t *p, uint32_t elapsed_ms)
{
    if (!p->powered)
        return;
    p->now_ms += elapsed_ms;
    /* delayed hammer strikes */
    for (int k = 0; k < KB_KEYS; k++) {
        if (!p->hammer_wait[k])
            continue;
        if (p->hammer_wait[k] <= elapsed_ms) {
            p->hammer_wait[k] = 0;
            key_sound_on(p, (uint8_t)k, p->hammer_vel[k]);
        } else {
            p->hammer_wait[k] = (uint8_t)(p->hammer_wait[k] - elapsed_ms);
        }
    }
    metro_event_t ev[16];
    int n = metronome_advance(&p->metro, elapsed_ms * 1000u, ev, 16);
    for (int i = 0; i < n; i++) {
        if (ev[i].kind == METRO_EV_STEP) {
            rhythm_step(p, ev[i].beat);
            continue;
        }
        if (p->metro.mode == METRO_CLICK && p->plat.metronome_click)
            p->plat.metronome_click(p->plat.ctx, ev[i].accent, p->metro.volume);
        const uint8_t bc[2] = {(uint8_t)(ev[i].beat + 1), 0x01};
        kp_send_status(&p->kp, 0x10, 0x56, 0x0B, 0x7F, bc, 2); /* BeatCounter */
    }
    recorder_tick(&p->rec, elapsed_ms);
    if (p->rec.state == REC_PLAYING || p->rec.state == REC_RECORDING) {
        p->progress_ms += elapsed_ms;
        if (p->progress_ms >= 250) {
            p->progress_ms = 0;
            uint32_t bar, beat;
            recorder_progress(&p->rec, &bar, &beat);
            const uint8_t pr[6] = {0, (uint8_t)((bar >> 7) & 0x7F), (uint8_t)(bar & 0x7F), (uint8_t)beat, 0, 0};
            kp_send_status(&p->kp, 0x10, 0x52, 0x03, 0x7F, pr, 6); /* MusicProgress */
        }
    }
    music_tick(p, elapsed_ms);
    preview_tick(p, elapsed_ms);
    usb_poll(p, elapsed_ms);
    kp_tick(&p->kp, elapsed_ms);
    pw_tick(p, elapsed_ms); /* APO (hold rule, -> standby or off) and the standby state machine */
}

/* ---------------- BT audio / APO hold ---------------- */

void piano_bt_line(piano_t *p, const char *line, uint16_t len)
{
    /* module lines are not user activity (stock: FUN_4014571C never resets the APO
     * counter); they only change what apo_tick holds */
    bt_audio_line(&p->bt, line, len);
}

void piano_bt_link(piano_t *p, bool up) { bt_audio_link(&p->bt, up); }

void piano_set_apo_policy(piano_t *p, const apo_policy_t *pol)
{
    if (pol)
        p->policy.hold = *pol;
    else
        apo_policy_default(&p->policy.hold);
    apo_hold_init(&p->apo_hold, &p->policy.hold);
}
