/* OpenGrand — executor gate (Phase 4 A, loader v3 sites 13 / 16): the core decides WHEN two stock
 * engine-loop executors run; the stock still does the work.
 *
 * Both executors are polled by the stock
 * engine loop (task 24, every 8 ms) and act on a "pending" byte another task set.  Evidence: REA
 * (Ghidra 12.1.4) on CAx9_P06.SYS:
 *
 *   media executor FUN_40127594 (site 0x40143AC4) - USB memory mount / unmount event
 *     0x41359E45 u8  bit 7 = event pending, bits 0-6 = 1 mounted / 0 removed.  Written only by
 *                    FUN_401276D4(v) = v | 0x80 (callers: unmount FUN_400A95B8 -> 0, mount
 *                    FUN_400A95F0 -> 1); the executor clears bit 7.
 *     removal: stop the player of the selected file type (FUN_401273B8), clear the audio name
 *     table (FUN_401241CC), then FORCE that player's state byte to 2 - for type 3 (KSO) the
 *     recorder state 0x4191EF79, without looking at it - and send the 0x86 notice.
 *     0x413CA3F5 u8  type of the file selected by fn 0x24 08 (FUN_4014EDFC): 3 KSO, 4 MID, 5 MP3, 6 WAV
 *   audio transport executor FUN_40123E34 (site 0x40143AD8)
 *     0x41058ACE u8  bit 7 = command pending, bits 0-6 = op (0 stop, 1 play, 2, 3 / 4 record
 *                    prepare, 5, 6).  Written by FUN_40123848(op) (op | 0x80, under semaphore
 *                    0x41058AC4) and cleared by the executor.
 *     0x413CA3EA u8  file-executor operation pending: the executor answers Busy (FUN_4014BEF0)
 *                    and DROPS the command.
 *
 * Rules (each one only ever DEFERS: the hook returns to the engine loop without calling the stock
 * executor, the pending byte stays as it is, and the stock runs it on a later 8 ms pass):
 *   media  XG_WHY_REC     removal while the selected type is 3 (KSO) and the recorder state is 3..6
 *                         (record standby / recording / count-in): the stock would force the state to
 *                         2 in the middle of the take.  Held until the state leaves 3..6.
 *   xport  XG_WHY_FILEOP  a pending STOP (op 0) while a file operation runs: the stock would drop it
 *                         with Busy and the audio would keep playing.  Held until the file operation
 *                         ends, at most XG_XPORT_CAP passes - then the stock runs it (its Busy).
 * Everything else, and everything while the gate is off, runs the stock executor unchanged. */
#ifndef CA79NG_XGATE_H
#define CA79NG_XGATE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* stock addresses (RAM, read only) */
#define XG_A_MEDIA     0x41359E45u
#define XG_A_SEL_TYPE  0x413CA3F5u
#define XG_A_REC_STATE 0x4191EF79u
#define XG_A_XCMD      0x41058ACEu
#define XG_A_FILEOP    0x413CA3EAu
#define XG_A_AUDIO     0x41058AD1u   /* audio player state (FUN_40123B40), reported only */

#define XG_PENDING     0x80u
#define XG_TYPE_KSO    3u
#define XG_OP_STOP     0u
/* a deferred stop runs anyway after this many engine-loop passes (8 ms each: 5 s) */
#define XG_XPORT_CAP   625u

#define XG_RUN   1   /* call the stock executor */
#define XG_DEFER 0   /* return to the engine loop without it; the pending byte is left as it is */

enum { XG_WHY_NONE = 0, XG_WHY_REC = 1, XG_WHY_FILEOP = 2, XG_WHY_CAP = 3 };
enum { XG_HK_MEDIA = 0x01, XG_HK_XPORT = 0x02 };

typedef struct {
    bool on;                 /* the gate's switch (default off)                                 */
    uint8_t hooked;          /* XG_HK_*: sites the resident accepted (reported by the port)     */
    uint8_t rc_media;        /* last hook_set rc per site, 0xFF = never tried                    */
    uint8_t rc_xport;
    uint8_t last_media;      /* the last pending media byte seen                                 */
    uint8_t last_cmd;        /* the last pending transport byte seen                             */
    uint8_t why_media;       /* XG_WHY_* of the last pending media pass                          */
    uint8_t why_xport;       /* XG_WHY_* of the last pending transport pass                      */
    uint8_t m_held;          /* a media event is being held                                      */
    uint8_t x_held;          /* a transport command is being held ...                            */
    uint8_t x_cmd;           /* ... this one                                                     */
    uint16_t x_wait;         /* passes it has been held                                          */
    uint32_t media_run;      /* pending media passes handed to the stock                         */
    uint32_t media_defer;    /* pending media passes held                                        */
    uint32_t xport_run;      /* pending transport passes handed to the stock                     */
    uint32_t xport_defer;    /* pending transport passes held                                    */
    uint16_t media_release;  /* held media events later handed to the stock                      */
    uint16_t xport_release;  /* held stops handed to the stock after the file operation ended    */
    uint16_t xport_capped;   /* held stops handed to the stock at the cap                        */
} xg_t;

void xg_init(xg_t *g);
/* switch the gate; off forgets what is being held (the stock runs it on its next pass) */
void xg_enable(xg_t *g, bool on);
/* engine loop, media site: media = 0x41359E45, sel_type = 0x413CA3F5, rec_state = 0x4191EF79 */
int xg_media(xg_t *g, uint8_t media, uint8_t sel_type, uint8_t rec_state);
/* engine loop, transport site: cmd = 0x41058ACE, fileop = 0x413CA3EA */
int xg_xport(xg_t *g, uint8_t cmd, uint8_t fileop);

/* the status record (op 0x1F, REMOTE_PROTOCOL §10.10), little-endian */
#define XG_RAW 34
#define XG_LAYOUT 1
#define XG_F_BUILT 0x01      /* set by the port: the gate is in this payload                         */
#define XG_F_ON    0x02      /* the gate is on (switched in the engine loop)                         */
/* 0x04 / 0x08: XG_HK_MEDIA / XG_HK_XPORT << 2 - the site is hooked                                     */
#define XG_F_WANT  0x10      /* set by the port: asked on (always, ota_main from ngver 3), applied on the next pass */
#define XG_F_SVC   0x20      /* set by the port: the resident gave a hook service (ota_loader_v2 +)   */
void xg_encode(const xg_t *g, uint8_t flags, uint8_t audio_state, uint8_t raw[XG_RAW]);

#endif
