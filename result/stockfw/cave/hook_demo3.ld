/* OpenGrand runtime-hook demo payload for ota_loader_v3 (image ota_hookdemo3):
 * hook_demo.ld (the five v2 sites) plus the stock call targets of the twelve sites ota_loader_v3 adds.
 * Valid for stock V2.19 (CAx9_P06.SYS) only; tools/roundtrip_ota_hooks.py checks each against the `bl` word. */
INCLUDE hook_demo.ld

stk_hk_pc      = 0x401475CC; /* site 0x40148C4C: MIDI program change FUN_401475CC (r0 port, r1 status, r2 program)   */
stk_hk_cc      = 0x40148218; /* site 0x40148C60: MIDI control change FUN_40148218 (r0 port, r1 status, r2 cc, r3 val) */
stk_hk_dly     = 0x401638E0; /* site 0x40153998: delayed parameter timers FUN_401638E0 (r0 elapsed)                  */
stk_hk_t20     = 0x4016DE5C; /* site 0x401739E4: task-20 pass FUN_4016DE5C (r0 elapsed)                              */
stk_hk_t20tail = 0x4012A7D4; /* site 0x401739E8: task-20 loop tail                                                   */
stk_hk_mq      = 0x4015DE0C; /* site 0x4016DE80: Music queue FUN_4015DE0C (r0 song state, returns r0)                */
stk_hk_metro   = 0x4016FBC0; /* site 0x4016DE94: metronome FUN_4016FBC0 (r0 = song state + 18)                       */
stk_hk_rmode   = 0x40127594; /* site 0x40143AC4: remote-mode change executor FUN_40127594                            */
stk_hk_media   = 0x40127458; /* site 0x40143AC8: media state Result FUN_40127458                                     */
stk_hk_file    = 0x4014F364; /* site 0x40143ACC: fn 0x24 file-op executor FUN_4014F364                               */
stk_hk_xport   = 0x40123E34; /* site 0x40143AD8: audio file play / record transport FUN_40123E34                     */
stk_hk_notify  = 0x40124068; /* site 0x40143ADC: mode-3 value watch / notify FUN_40124068                            */
