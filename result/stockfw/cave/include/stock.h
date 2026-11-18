/* Prototypes for the stock CA79 V2.19 routines the cave calls.
 * The addresses live in stock.ld; this header only gives them types.
 * Context rules:
 *   - everything here runs in User mode, in a task context;
 *   - the tx_* writers take a per-port semaphore, so they are task-only;
 *   - flash reads must be wrapped in dis_dsp/ena_dsp and gated on flash_busy. */
#ifndef CA79NG_STOCK_H
#define CA79NG_STOCK_H

typedef unsigned char u8;
typedef unsigned short u16;
typedef unsigned int u32;

void stk_tx_port0(const u8 *buf, u32 len);
void stk_tx_port1(const u8 *buf, u32 len);
void stk_tx_port2(const u8 *buf, u32 len);
void stk_tx_port3(const u8 *buf, u32 len);

void stk_flash_copy(u32 src_flash, void *dst_ram, u32 len);
int  stk_flash_busy(void);
void stk_dis_dsp(void);
void stk_ena_dsp(void);
void stk_dly_tsk(u32 ms);

int  stk_media_present(void);          /* 0 = a stick is mounted */
int  stk_fs_ready(int drive);          /* stk_fs_ready(0) -> 0 = ok */
int  stk_file_write(const char *path, u32 len, const void *src, u32 *written);
int  stk_file_close_write(int which);  /* 1 = the write handle */
int  stk_file_size(const char *path, u32 *size);
int  stk_file_delete(const char *path);

extern u32 stk_write_handle_ptr;       /* non-zero while some writer owns the handle */

/* Read-only state for the phase2c probe.  `const volatile`: the cave must never write
 * these, and the stock code / ISRs change them under our feet. */
int stk_bt_dialect(void);              /* 1 = AT dialect, 0 = iWRAP; reads two registers only */
extern const volatile u8  stk_apo_block[28];
extern const volatile u8  stk_a2dp_flag;
extern const volatile u8  stk_bt_ready;
extern const volatile u8  stk_special_mode;
extern const volatile u8  stk_binding;
extern const volatile u32 stk_port3_fwd;
extern const volatile u8  stk_bt_rx_ring[0x800];
extern const volatile u16 stk_bt_rx_rd;
extern const volatile u16 stk_bt_rx_wr;
extern const volatile u8  stk_bt_line_len;

#endif
