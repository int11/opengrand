/* Stock CA79 V2.19 (CAx9_P06.SYS, sha256 09ad8570...d933c8) entry points and globals the
 * OpenGrand core port binds to (result/firmware/ports/cave/cave_stock.h).  Linked only into
 * images with the CORE feature bit (cave_core.ld); stock.ld stays as the other caves see it.
 * Evidence: REA analysis and disassembly of the stock image. */

stk_time_us      = 0x400A8340; /* FUN_400A8340: *0x410342FC + TIM12 * 1000 / period, reads only     */
stk_param_apply  = 0x4016053C; /* FUN_4016053C(ctx, part, id, data*) -> 0 / 0xF0     */

/* NORTi context bytes, read only - the twai_sem E_CTX checks (0x40086F0C-0x40086F78) */
stk_int_nest     = 0x4139E79D; /* s8 interrupt nesting depth                                     */
stk_dsp_disabled = 0x4139E79B; /* s8 dispatch disabled (dis_dsp)                                 */
stk_cur_task     = 0x4139E668; /* u8 current task id; 0x21 = kernel pseudo task (timer handlers) */
