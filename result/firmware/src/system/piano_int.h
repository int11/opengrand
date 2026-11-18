/* OpenGrand — private interface between the piano_t modules
 * (piano.c core, vtedit.c, music.c, usb.c, preview.c). */
#ifndef CA79NG_PIANO_INT_H
#define CA79NG_PIANO_INT_H

#include "ca79ng/piano.h"

/* OpenGrand extension parameters (not in the original descriptor table;
 * REMOTE_PROTOCOL.md §9). fn 0x10, part 0x7F. */
#define EXT_V1_MUSIC 0x52
#define EXT_CM_RESULT 0x0B       /* piano -> controller [code, groups_hi, groups_lo] */
#define EXT_CM_SETTING 0x0D      /* [mode 0 Melody / 1 Ensemble, smooth 0/1, trigger 0 Part1 / 1 Part2] */
#define EXT_V1_SYSTEM 0x51
#define EXT_RESET_COMPLETE 0x22  /* (original ResetCompleteInfo, fn 0x32 52 22) */

/* piano.c */
void pn_note(piano_t *p, uint8_t part, uint8_t note, uint8_t vel);
void pn_control(piano_t *p, uint8_t part, uint8_t cc, uint8_t value);
void pn_reset_midi_parts(piano_t *p);
void pn_store_music_tempo(piano_t *p);
part_state_t *pn_part(piano_t *p, uint8_t part);

/* vtedit.c — user touch curve and per-note VT data */
void vt_init(piano_t *p);
void vt_apply(piano_t *p);
bool vt_apply_value(piano_t *p, param_id_t id, uint8_t fn, const uint8_t *d, uint8_t len, midi_port_t origin);
bool vt_ext(piano_t *p, midi_port_t port, uint8_t fn, uint8_t v1, uint8_t v2, const uint8_t *d, uint16_t len);
bool vt_request(piano_t *p, midi_port_t port, uint8_t v1, uint8_t v2);
void vt_learn_input(piano_t *p, uint8_t raw);
void vt_reset(piano_t *p);

/* music.c — song modes, persistence, Lesson, Concert Magic */
void music_init(piano_t *p);
void music_tick(piano_t *p, uint32_t elapsed_ms);
bool music_apply_value(piano_t *p, param_id_t id, const uint8_t *d, uint8_t len);
bool music_ext(piano_t *p, midi_port_t port, uint8_t fn, uint8_t v1, uint8_t v2, const uint8_t *d, uint16_t len);
bool music_request(piano_t *p, midi_port_t port, uint8_t v1, uint8_t v2);
/* key event while a song mode may own the keyboard; true = consumed */
bool music_key(piano_t *p, uint8_t note, uint8_t vel, bool on);
void music_rec_out(void *ctx, uint8_t part, uint8_t status, uint8_t d1, uint8_t d2);
void music_song_loaded(piano_t *p, uint8_t slot);
void music_erase_all(piano_t *p);
void music_set_transpose(piano_t *p, int t);
bool usb_apply_value(piano_t *p, param_id_t id, const uint8_t *d, uint8_t len);

/* usb.c — USB memory and fn 0x24 FileAccess server */
void usb_poll(piano_t *p, uint32_t elapsed_ms);
bool usb_ready(const piano_t *p);
uint16_t usb_file_op(void *ctx, midi_port_t port, uint8_t op, const char *name, const char *name2);
bool usb_file_list(void *ctx, uint32_t index, char *name, size_t cap);
void usb_file_upload(piano_t *p, midi_port_t port, uint8_t kind, const uint8_t *data, uint32_t len);
uint16_t usb_select_id(piano_t *p, uint32_t id);
uint16_t usb_select_name(piano_t *p, const char *name);
uint16_t usb_save_song(piano_t *p, uint8_t slot, const char *name);
uint16_t usb_delete_id(piano_t *p, uint32_t id);
uint16_t usb_format(piano_t *p);
bool usb_name_of(piano_t *p, uint32_t id, char *name, size_t cap);

/* powerctl.c — power policy, APO action, partial standby glue */
void pw_init(piano_t *p);
void pw_tick(piano_t *p, uint32_t elapsed_ms);
void pw_factory_reset(piano_t *p);
void pw_remote_off(void *ctx);   /* kproto io.power_off: 7E 00 00 already sent by kproto */
void pw_midi_wake(piano_t *p, midi_port_t port, const uint8_t *m, uint16_t n);
void pn_power_off(piano_t *p);   /* piano.c: the real power-off (platform) */

/* ngext.c — OpenGrand extension SysEx (manufacturer 0x7D) handled by the core */
#define NG_FEAT0_CORE 0x01         /* Identify */
#define NG_FEAT1_POWER 0x10        /* ops 0x18 power policy, 0x19 standby (REMOTE_PROTOCOL §10.4) */
#define NG_FEAT1_TVWAKE 0x40       /* standby keeps BT audio on, STREAMING START wakes (policy flag bit 2) */
bool ng_sysex(piano_t *p, midi_port_t port, const uint8_t *m, uint16_t n);
void ng_standby_notice(piano_t *p, standby_state_t st);

/* preview.c — Sound Preview phrases */
void preview_start(piano_t *p, uint8_t target);
void preview_stop(piano_t *p);
void preview_tick(piano_t *p, uint32_t elapsed_ms);

#endif
