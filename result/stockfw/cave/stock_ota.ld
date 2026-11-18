/* Stock CA79 V2.19 state the OTA code reads (stage-0, stage-1, the main).  The NOR WRITERS are not here:
 * stock_norw.ld, linked into stage-0 only (s0.ld) - any other reference fails to link.
 * Evidence: REA analysis of the stock image.                                                       */

stk_flash_jobs   = 0x401342AC; /* () 1 while a deferred-writer job is active (0x4139DAAC +0/+8)  */

/* The loader only.  Read-only stock state for the escape keys: the local
 * key-scan state the stock keeps for every key (FUN_40154C18 / baseline FUN_40154D0C store the
 * key scanner byte with the low 4 bits cleared; upper 3 bits = the key's sensors, 0xE0 = fully
 * down - the same test as the BootProgram stub) and the flag TaskInitial raises once the scan
 * runs (FUN_40154D94 at 0x400AB170, before the app tasks incl. the engine loop are created).
 * Index 0 = A0 (BootProgram key code order: uppermost E A B C = 0x80D08000).                    */
stk_key_state    = 0x413CAE98; /* const u8[88] */
stk_key_scan_on  = 0x413CAF38; /* const u8     */

/* The stock SWI #0 return stub (*0x4009D000 = 0x400A88BC): every privileged routine reached
 * through `svc #0` with r0 = routine (handler 0x400A8884: SWI 0 -> bx r0, SVC mode, IRQ off)
 * ends with `ldr lr, [0x4009D000]; bx lr` - the stock cache wrappers 0x4009D444/0x4009D52C do. */
stk_svc_return_ptr = 0x4009D000;
