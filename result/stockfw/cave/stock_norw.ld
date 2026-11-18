/* Stock CA79 V2.19 NOR write routines and the busy flag setter - linked into STAGE-0 ONLY (s0.ld).  Stage-0's
 * guarded service (src/s0.c nor_erase / nor_prog / nor_prog_hw) is their only caller; stage-1 and the main write NOR
 * through that service (include/s0.h struct s0_svc), and a reference to these names from them does not link.
 * Evidence: REA analysis of the stock image (all three run unmodified on the emulator NOR model).               */

stk_nor_erase      = 0x40133C38; /* (cpu addr) sector erase; poll FUN_40133B68 (dly_tsk(100))    */
stk_nor_prog_buf   = 0x40133D50; /* (cpu addr, src, len <= 512) write buffer; even len: no globals */
stk_nor_prog_hw    = 0x401339EC; /* (cpu addr, hw) one halfword; busy wait, writes no RAM         */
stk_flash_busy_set = 0x4013403C; /* (0|1) the flag stk_flash_busy reads: 1 around one writer call, under dis_dsp */
