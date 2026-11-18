/* OpenGrand cave port - GPIO diagnostic, READ ONLY (power-family slot payloads = ota_main; core
 * module src/system/gpiodiag.c).
 *
 * No hook site: the engine loop (task 24) already calls ng_tick every ~8 ms; gdc_tick samples the watched pins there
 * and answers the queued request.  Task 19 (ng_sysex) only copies the request.  Nothing here writes a GPIO / SYSCFG
 * register or stock RAM - the loads are the MMIO words and stock bytes listed in ca79ng/gpiodiag.h.
 *
 * Op 0x17 (REMOTE_PROTOCOL §10.11), answered on the requesting port from the next engine-loop pass:
 *   17 | 17 00        -> 57 00 <pack7 of the GD_SNAP snapshot>                     any port
 *   17 01             -> 57 01 <pack7 of the change log>                           any port
 *   17 02 <0|1>       -> 57 02 <rc> <flags> <port>   notices off / on              on = a bound controller only
 *                        (the rule of the power ops: the session core's table, else the stock binding)
 *                        rc 0 ok, 1 bad argument, 2 not bound; flags = snapshot flags, port = notice port (0x7F none)
 * Notice (notices on, to the port that switched them on while it stays bound - else they switch off):
 *   57 7F <pack7: n u8, seq u16 (newest), n events oldest first>   n <= 4, at most one per GDC_NOTICE_MS; the first
 *   change after a quiet spell goes out on the pass that saw it.  Busy (a request already queued): dropped. */
#ifndef CA79NG_GPIO_CAVE_H
#define CA79NG_GPIO_CAVE_H

#include <stdint.h>

#define GDC_OP 0x17
#define GDC_NOTICE_MS 40u
#define GDC_NOTICE_MAX 4u
enum { GDC_RC_OK = 0, GDC_RC_ARG = 1, GDC_RC_NOT_BOUND = 2 };

/* task 24, ng_tick */
void gdc_tick(void);
/* task 19, ng_sysex: op 0x17 (the cave checked the tag, F7 and len >= 8) */
void gdc_sysex(uint8_t port, const uint8_t *buf, uint32_t len);

#endif
