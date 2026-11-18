/* OpenGrand cave port - Phase 4 executor gate (slot payload ota_main, Identify feat3 0x08; core module
 * src/system/xgate.c).
 *
 * Two engine-loop (task 24) sites of the ota_loader_v3 allow-list:
 *   #13 0x40143AC4  bl FUN_40127594  media executor (USB memory mount / unmount)   -> hk_xg_media
 *   #16 0x40143AD8  bl FUN_40123E34  audio-file transport executor                 -> hk_xg_xport
 * Context contract (task 24): the resident's own task - sequential, the hook may call and return.
 * The stubs (src/power_hooks.c) call xgc_*_pass(): 1 = `b` to the stock executor, 0 = back to the site + 4 without it
 * (the pending byte is left as it was; the stock runs it on a later pass).  The bodies only read stock RAM.
 *
 * Default OFF: no site is hooked until op 1F 01 01 (a bound controller); the engine loop then asks the resident for
 * the two sites once.  Under ota_loader_v2 both are refused (rc 2 OTA_HK_E_SITE - not in its allow-list), under the
 * first ota_loader there is no hook service at all: the gate is "on" without a hook and the stock path is untouched.
 * 1F 01 00 (anyone) unhooks on the next pass.  Nothing is kept across a power cycle or an apply-now.
 *
 * Op 0x1F (REMOTE_PROTOCOL §10.10), answered at once on the requesting port:
 *   1F | 1F 00      -> 5F 00 00 <pack7 of the XG_RAW record>
 *   1F 01 <0|1>     -> 5F 01 <rc> <pack7 of the record>   rc 0 ok, 1 bad argument, 2 not bound (on only) */
#ifndef CA79NG_XGATE_CAVE_H
#define CA79NG_XGATE_CAVE_H

#include <stdint.h>

#define XGC_OP 0x1F
#define XGC_SITE_MEDIA 0x40143AC4u   /* bl FUN_40127594 */
#define XGC_SITE_XPORT 0x40143AD8u   /* bl FUN_40123E34 */
#define XGC_WORD_MEDIA 0xEBFF8EB2u
#define XGC_WORD_XPORT 0xEBFF80D5u
enum { XGC_RC_OK = 0, XGC_RC_ARG = 1, XGC_RC_NOT_BOUND = 2 };

/* task 24, the stubs: 1 = run the stock executor, 0 = not this pass */
int xgc_media_pass(void);
int xgc_xport_pass(void);
/* task 24, ng_tick: apply the switch (hook / unhook) */
void xgc_tick(void);
/* task 19, ng_sysex: op 0x1F (the cave checked the tag, F7 and len >= 8) */
void xgc_sysex(uint8_t port, const uint8_t *buf, uint32_t len);

/* src/power_hooks.c: hook / unhook site 0 (media) or 1 (transport) through the resident's service; the
 * OTA_HK_* rc, 0xFF = no service (attach() never called) */
int xgh_set(int which);
int xgh_clear(int which);
int xgh_have_svc(void);

#endif
