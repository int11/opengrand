/* OpenGrand runtime-hook demo payload (test payload ota_hookdemo3 via hook_demo3.ld):
 * the ordinary slot payload layout (ota_payload.ld) plus the stock call targets of the five
 * allow-listed hook sites - the demo's pass-through stubs tail-call these.  Valid for stock
 * V2.19 (CAx9_P06.SYS) only; tools/roundtrip_ota_hooks.py checks each against the `bl` word. */
INCLUDE ota_payload.ld

stk_hk_kawai  = 0x4014A2C8; /* site 0x40147E94: Kawai SysEx handler FUN_4014A2C8 (r0 port, r1 buf, r2 len) */
stk_hk_midi   = 0x4012A7D4; /* site 0x401539AC: MIDI pump loop tail call                                    */
stk_hk_hp     = 0x400ACFA0; /* site 0x40143AB4: headphone poll FUN_400ACFA0                                 */
stk_hk_exec   = 0x400ACD28; /* site 0x40143AB8: shutdown executor FUN_400ACD28                              */
stk_hk_apo    = 0x400ABFE8; /* site 0x400AA874: power switch tick + APO FUN_400ABFE8 (20 ms kernel timer)   */
