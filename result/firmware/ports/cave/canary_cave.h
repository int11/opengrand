/* OpenGrand cave port - RAM canary over the big SDRAM blocks no stock code references (core module
 * src/system/ramcanary.c; the field proof the hardware baseline study asks for before main layout L1 uses them).
 *
 * No hook site: the engine loop (task 24) already calls ng_tick every ~8 ms; cnc_tick answers the queued request and,
 * while armed, does one bounded step (RCN_PASS_WORDS words: fill, then verify) and times it with hal_time_us.  Task 19
 * (ng_sysex) only copies the request.  Nothing is written anywhere before an arm from a bound controller, and nothing
 * at boot: a power cycle always leaves the blocks to the stock as it found them.  The only RAM written is the armed
 * regions of CNC_TAB (each with a 4 KB guard at both ends) and this port's own payload .data.
 *
 * Op 0x20 (REMOTE_PROTOCOL §10.13), answered on the requesting port from the next engine-loop pass:
 *   20 | 20 00           -> 60 00 <pack7 of the RCN_STATUS record>                 any port
 *   20 01 <lo7> <hi7>    -> 60 01 <rc> <lo7> <hi7>   arm: mask = lo7 | hi7 << 7    a bound controller only
 *                           (the rule of the power ops: the session core's table, else the stock binding)
 *                           rc 0 armed (a run before is restarted), 1 bad mask, 2 not bound; then the armed mask
 *   20 02 <region>       -> 60 02 <region> <pack7 of the RCN_REGION record>          any port
 *   20 03                -> 60 03 00 <lo7> <hi7>      disarm (the records stay)     any port
 * Other forms: silent.  Busy (a request already queued): dropped. */
#ifndef CA79NG_CANARY_CAVE_H
#define CA79NG_CANARY_CAVE_H

#include <stdint.h>

#define CNC_OP 0x20
#define CNC_REGIONS 8u
enum { CNC_RC_OK = 0, CNC_RC_ARG = 1, CNC_RC_NOT_BOUND = 2 };

/* task 24, ng_tick */
void cnc_tick(void);
/* task 19, ng_sysex: op 0x20 (the cave checked the tag, F7 and len >= 8) */
void cnc_sysex(uint8_t port, const uint8_t *buf, uint32_t len);

#endif
