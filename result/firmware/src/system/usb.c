/* OpenGrand — USB memory: attach/detach, the fn 0x24 FileAccess server, SMF
 * play / save / import and the file management remote parameters
 * (USBMemoryFormat 32 51 3C, DeleteFile 32 51 3B, RenameFile 32 51 3A,
 * InternalSongFileWrite 32 52 20). Request/response sequences: REMOTE_PROTOCOL.md §9. */
#include <stdio.h>
#include <string.h>

#include "ca79ng/util.h"
#include "piano_int.h"

static const hal_fs_t *fs(const piano_t *p) { return p->plat.usb; }

bool usb_ready(const piano_t *p) { return fs(p) && fs(p)->mounted && fs(p)->mounted(fs(p)->ctx); }

static void result_all(piano_t *p, uint16_t code)
{
    for (int port = 0; port < MIDI_PORT_COUNT; port++)
        if (p->kp.connected[port])
            kp_send_result(&p->kp, (midi_port_t)port, code);
}

static void unload(piano_t *p)
{
    if (p->rec.cur_song == REC_SLOT_EXT)
        recorder_stop(&p->rec);
    recorder_erase(&p->rec, REC_SLOT_EXT, -1);
    p->rec.dirty &= (uint16_t)~(1u << REC_SLOT_EXT);
    p->usb_selected = -1;
}

void usb_poll(piano_t *p, uint32_t elapsed_ms)
{
    p->usb_poll_ms += elapsed_ms;
    if (p->usb_poll_ms < 100 && !p->booting)
        return;
    p->usb_poll_ms = 0;
    bool m = usb_ready(p);
    if (m == p->usb_mounted)
        return;
    p->usb_mounted = m;
    if (!m)
        unload(p);
    const uint8_t v = m ? 1 : 0;
    kp_local_set(&p->kp, PRM_USBMEMORY, SLOT_SYSTEM, &v, 1); /* USBMemory Attach/Detach notification */
}

bool usb_name_of(piano_t *p, uint32_t id, char *name, size_t cap)
{
    uint32_t size;
    return usb_ready(p) && fs(p)->list(fs(p)->ctx, id, name, cap, &size);
}

bool usb_file_list(void *ctx, uint32_t index, char *name, size_t cap)
{
    piano_t *p = ctx;
    return usb_name_of(p, index, name, cap);
}

/* whole file into the platform SMF buffer; returns length or -code */
static int32_t read_file(piano_t *p, const char *name)
{
    if (!usb_ready(p))
        return -(int32_t)KP_RES_NO_MEDIA;
    if (!p->plat.smf_buf)
        return -(int32_t)KP_RES_MEMORY_OVER;
    int32_t n = fs(p)->read(fs(p)->ctx, name, p->plat.smf_buf, p->plat.smf_cap);
    if (n >= 0 && (uint32_t)n > p->plat.smf_cap)
        return -(int32_t)KP_RES_SIZE_OVER;
    return n;
}

static uint16_t load_into(piano_t *p, uint8_t slot, const uint8_t *data, uint32_t len)
{
    if (p->rec.state == REC_RECORDING)
        return KP_RES_MODE;
    if (!smf_read(&p->rec, slot, data, len))
        return KP_RES_SMF_FORMAT;
    return KP_RES_OK;
}

uint16_t usb_select_name(piano_t *p, const char *name)
{
    int32_t n = read_file(p, name);
    if (n < 0)
        return (uint16_t)-n;
    uint16_t r = load_into(p, REC_SLOT_EXT, p->plat.smf_buf, (uint32_t)n);
    if (r != KP_RES_OK)
        return r;
    music_song_loaded(p, REC_SLOT_EXT);
    p->usb_selected = 0;
    for (uint32_t i = 0;; i++) { /* remember the list id of the file */
        char nm[FS_NAME_MAX];
        if (!usb_name_of(p, i, nm, sizeof nm))
            break;
        if (strcmp(nm, name) == 0) {
            p->usb_selected = (int32_t)i;
            break;
        }
    }
    return KP_RES_OK;
}

uint16_t usb_select_id(piano_t *p, uint32_t id)
{
    char name[FS_NAME_MAX];
    if (!usb_ready(p))
        return KP_RES_NO_MEDIA;
    if (!usb_name_of(p, id, name, sizeof name))
        return KP_RES_NOT_FOUND;
    return usb_select_name(p, name);
}

/* Import a file into internal song `song` (0xFF: the current one; persisted like a recording). */
static uint16_t load_internal(piano_t *p, const char *name, uint8_t song)
{
    int32_t n = read_file(p, name);
    if (n < 0)
        return (uint16_t)-n;
    uint8_t slot = song < REC_SONGS ? song : p->rec.cur_song < REC_SONGS ? p->rec.cur_song : 0;
    uint16_t r = load_into(p, slot, p->plat.smf_buf, (uint32_t)n);
    if (r == KP_RES_OK) {
        p->rec.song[slot].ext = true;
        recorder_select(&p->rec, slot);
    }
    return r;
}

uint16_t usb_save_song(piano_t *p, uint8_t slot, const char *name)
{
    if (!usb_ready(p))
        return KP_RES_NO_MEDIA;
    if (slot >= REC_SLOTS || p->rec.song[slot].count == 0)
        return KP_RES_NO_DATA;
    if (!p->plat.smf_buf || kp_file_busy(&p->kp))
        return KP_RES_BUSY;
    size_t n = smf_write(&p->rec, slot, p->plat.smf_buf, p->plat.smf_cap);
    if (!n)
        return KP_RES_MEMORY_OVER;
    if (name) /* 24 0A: written over like the stock (FUN_400A92C0 without the exists check) */
        return fs(p)->write(fs(p)->ctx, name, p->plat.smf_buf, (uint32_t)n, true);
    /* automatic name: SONG_nn.MID (internal song nn+1) / USBREC_nnn.MID, first free */
    char auto_name[32];
    for (int k = 0; k < 1000; k++) {
        if (slot < REC_SONGS)
            snprintf(auto_name, sizeof auto_name, k ? "SONG_%02u_%d.MID" : "SONG_%02u.MID", slot + 1u, k);
        else
            snprintf(auto_name, sizeof auto_name, "USBREC_%03d.MID", k + 1);
        uint16_t r = fs(p)->write(fs(p)->ctx, auto_name, p->plat.smf_buf, (uint32_t)n, false);
        if (r != KP_RES_EXISTS)
            return r;
    }
    return KP_RES_FULL;
}

uint16_t usb_delete_id(piano_t *p, uint32_t id)
{
    char name[FS_NAME_MAX];
    if (!usb_ready(p))
        return KP_RES_NO_MEDIA;
    if (!usb_name_of(p, id, name, sizeof name))
        return KP_RES_NOT_FOUND;
    if (p->usb_selected == (int32_t)id)
        unload(p);
    return fs(p)->remove(fs(p)->ctx, name);
}

uint16_t usb_format(piano_t *p)
{
    if (!usb_ready(p))
        return KP_RES_NO_MEDIA;
    unload(p);
    return fs(p)->format(fs(p)->ctx);
}

/* ---- fn 0x24 FileAccess ---- */

uint16_t usb_file_op(void *ctx, midi_port_t port, uint8_t op, const char *name, const char *name2)
{
    piano_t *p = ctx;
    if (!usb_ready(p))
        return KP_RES_NO_MEDIA;
    switch (op) {
    case KP_FA_LIST:
        return KP_RES_OK;
    case KP_FA_READ: {
        if (kp_file_busy(&p->kp))
            return KP_RES_BUSY;
        int32_t n = read_file(p, name);
        if (n < 0)
            return (uint16_t)-n;
        kp_file_send(&p->kp, port, p->plat.smf_buf, (uint32_t)n);
        return KP_RES_PENDING; /* the data stream is the answer */
    }
    case KP_FA_DELETE: {
        if (p->usb_selected >= 0) {
            char cur[FS_NAME_MAX];
            if (usb_name_of(p, (uint32_t)p->usb_selected, cur, sizeof cur) && strcmp(cur, name) == 0)
                unload(p);
        }
        return fs(p)->remove(fs(p)->ctx, name);
    }
    case KP_FA_RENAME:
        if (!name2[0])
            return KP_RES_PATH;
        return fs(p)->rename(fs(p)->ctx, name, name2);
    case KP_FA_SELECT:
        return usb_select_name(p, name);
    case KP_FA_LOAD: /* 24 09 <song> <type>: SMF only (type index 2 or absent); KSO is a stock format */
        if (p->kp.fa.d2 != 0xFF && p->kp.fa.d2 != 2)
            return KP_RES_SMF_FORMAT;
        return load_internal(p, name, p->kp.fa.d1);
    case KP_FA_SAVE: /* 24 0A <song> <type>: internal song as SMF under the given name */
        if (p->kp.fa.d1 >= REC_SONGS || p->kp.fa.d2 != 2)
            return KP_RES_PARAM;
        return usb_save_song(p, p->kp.fa.d1, name);
    default:
        return KP_RES_PARAM;
    }
}

void usb_file_upload(piano_t *p, midi_port_t port, uint8_t kind, const uint8_t *data, uint32_t len)
{
    uint16_t r;
    if (kind == KP_FILE_WRITE) {
        /* FileWriteRequest: store the file on the stick under the given name */
        r = usb_ready(p) ? fs(p)->write(fs(p)->ctx, p->kp.fa.name, data, len, false) : KP_RES_NO_MEDIA;
    } else {
        /* anonymous upload (no FileWriteRequest): an SMF to play right away
         * from the USB/scratch slot, without USB memory */
        r = load_into(p, REC_SLOT_EXT, data, len);
        if (r == KP_RES_OK) {
            music_song_loaded(p, REC_SLOT_EXT);
            p->usb_selected = -1;
        }
    }
    kp_send_result(&p->kp, port, r);
}

/* ---- remote parameters ---- */

bool usb_apply_value(piano_t *p, param_id_t id, const uint8_t *d, uint8_t len)
{
    switch (id) {
    case PRM_USBMEMORY: { /* status owned by the piano: answer with the real state */
        const uint8_t v = p->usb_mounted ? 1 : 0;
        if (param_put(&p->store, PRM_USBMEMORY, SLOT_SYSTEM, &v, 1))
            kp_notify(&p->kp, PRM_USBMEMORY, SLOT_SYSTEM, MIDI_PORT_COUNT);
        return true;
    }
    case PRM_USBMEMORY_FORMAT_REMOTE:
        result_all(p, usb_format(p));
        return true;
    case PRM_DELETE_FILE_REMOTE: /* [1 = file list id, id hi7, lo7, 0] */
        if (len >= 3)
            result_all(p, d[0] == 1 ? usb_delete_id(p, (uint32_t)d[1] << 7 | d[2]) : KP_RES_PARAM);
        return true;
    case PRM_RENAME_FILE_REMOTE: { /* [1, id hi7, lo7, 0], then FileName packets with the new name */
        char name[FS_NAME_MAX];
        if (len < 3 || d[0] != 1) {
            result_all(p, KP_RES_PARAM);
        } else if (!usb_ready(p)) {
            result_all(p, KP_RES_NO_MEDIA);
        } else if (!usb_name_of(p, (uint32_t)d[1] << 7 | d[2], name, sizeof name)) {
            result_all(p, KP_RES_NOT_FOUND);
        } else {
            kp_file_expect_name(&p->kp, KP_FA_RENAME, name);
        }
        return true;
    }
    case PRM_INTERNAL_SONG_FILE_WRITE_REMOTE: { /* [format 1 = SMF, song hi7, lo7, part] */
        if (len < 3)
            return true;
        uint32_t s = (uint32_t)d[1] << 7 | d[2];
        result_all(p, s < REC_SONGS ? usb_save_song(p, (uint8_t)s, NULL) : KP_RES_PARAM);
        return true;
    }
    default:
        return false;
    }
}
