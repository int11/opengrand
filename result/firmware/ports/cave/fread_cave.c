/* OpenGrand cave port - fn 0x24 file read with 7-bit DataPacket counters.  See fread_cave.h.
 *
 * Stock routines and globals (result/stockfw/cave/stock_fread.ld; capstone disassembly of CAx9_P06.SYS -
 * REA was not connected):
 *   0x413CA3EB  executor due (FUN_4014F364 clears it on entry)       0x413CA3EA  job code (cleared before the Result)
 *   0x413C9FC4  the received FileName                                0x40B00000  read buffer (< 1 MB)
 *   0x4136D820  the file-transfer semaphore id (word)                0x413CA3DE  binding (0 = unbound)
 *   0x413CA3E7  Cancel flag (FUN_4014D664 tests and clears it)
 *   0x413CA384  the stop-and-wait sender's semaphore id (word)       0x413CA3DE  binding 4 = the panel (kept on 16 failures)
 *   0x413CA3E5  upload state (FUN_4014F96C 1, FUN_4014FB94 2)        0x413CA3B4  upload length (TopOfData, bytes)
 * Write side and Cancel: (capstone disassembly, REA not connected). */
#include "fread_cave.h"

#define A_DUE 0x413CA3EBu
#define A_JOB 0x413CA3EAu
#define A_NAME 0x413C9FC4u
#define A_BUF 0x40B00000u
#define A_SEM 0x4136D820u
#define A_BOUND 0x413CA3DEu
#define A_TXSEM 0x413CA384u
#define A_UPSTATE 0x413CA3E5u
#define A_UPLEN 0x413CA3B4u
#define A_DEVID 0x413B5404u
#define JOB_READ 4u
#define DEV 0x7Fu
#define FW_MAX 0x100000u   /* the 1 MB buffer at 0x40B00000 (the Read job's own limit, 0x4014F6A0) */

int32_t stk_fsize(const char *name, uint32_t *size);                 /* FUN_400A9238: 0 = ok            */
int32_t stk_fread(const char *name, uint8_t *dst);                   /* FUN_400A9408: 0 = ok            */
void stk_wai_sem(uint32_t id, int32_t tmo);                          /* FUN_40086E94                    */
void stk_sig_sem(uint32_t id);                                       /* FUN_4008819C                    */
int32_t stk_fa_top(uint32_t dev, uint32_t size);                     /* FUN_4014B6E0: TopOfData, < 0 fail */
uint32_t stk_fa_build(uint32_t dev, const uint8_t *data, uint32_t size, uint32_t counter, uint8_t *out); /* FUN_4014D7E0 */
int32_t stk_fa_cancel(void);                                         /* FUN_4014D664: 1 = Cancel seen   */
int32_t stk_fa_end(uint32_t dev);                                    /* FUN_4014B7D8: EndOfData         */
void stk_fa_result_ok(uint32_t v);                                   /* FUN_4014F294                    */
void stk_fa_result(uint32_t code);                                   /* FUN_4014DC0C: 7E 03 00 hi lo    */
/* the stop-and-wait sender FUN_4014B638's parts (its body is reproduced in send_frame with a Cancel test) */
void stk_fa_flags_clear(void);                                       /* FUN_4014B4C0: ACK / NAK / Wait = 0 */
void stk_fa_tx(const uint8_t *frame, uint32_t len);                  /* FUN_4014AD04: to *0x413CA3DE      */
int32_t stk_fa_wait(uint32_t ticks);                                 /* FUN_4014B4E0: 0 ACK, 1 NAK, 2 time */
void stk_dly_tsk(uint32_t ms);                                       /* FUN_40089658                      */
void stk_unbind(void);                                               /* FUN_4014B55C                      */
void stk_reset_a(void);                                              /* FUN_4014B5A0                      */
void stk_reset_b(void);                                              /* FUN_4014B5D0                      */
void stk_reset_c(void);                                              /* FUN_4014B610: upload state 0 + sig_sem */

static volatile uint8_t attached = 0xFF;
static volatile uint16_t reads = 0;   /* Read jobs run here (debug: read with the payload's memory dump) */
static volatile uint16_t cancels = 0; /* Cancels seen while the sender waited for an ACK (debug) */

void frc_attached(int rc)
{
    attached = (uint8_t)rc;
}

static uint8_t rd8(uint32_t a)
{
    return *(const volatile uint8_t *)(uintptr_t)a;
}

static void wr8(uint32_t a, uint8_t v)
{
    *(volatile uint8_t *)(uintptr_t)a = v;
}

static uint32_t rd32(uint32_t a)
{
    return *(const volatile uint32_t *)(uintptr_t)a;
}

/* FUN_4014B638 (16 tries of send + 250-tick ACK wait, 5 ms apart; then the binding dropped unless it is the panel)
 * plus Cancel: the stock wait FUN_4014B4E0 only ends on ACK / NAK / time - a 24 7D sent instead of the ACK left the
 * job resending for 16 x 250 ticks and then unbinding the app (field 2026-11-16: Busy until the app re-CONNECTed).
 * The wait runs as ten 25-tick slices with the Cancel flag tested after each.  0 = ACK, -1 = failed, -2 = Cancel.
 * Retry budget: the stock counts NAKs and silent waits alike (16 in all).  A 1 MB read died on that (field 2026-11-16): over a long transfer some notifications reach the app damaged, the app
 * NAKs each one and the resends can be damaged too.  A NAK means the controller is there, so here only silent tries
 * (no ACK / NAK within 250 ticks) count against the stock's 16; NAKed ones count against TX_TRIES sends in all.
 * Silence alone ends exactly as before (16 sends, 16 x 250 ticks, the binding dropped). */
#define TX_TRIES 64u
#define TX_SILENT 16u
static int32_t send_frame(const uint8_t *f, uint32_t len)
{
    uint32_t sem = rd32(A_TXSEM);
    stk_wai_sem(sem, -1);
    stk_fa_flags_clear();
    for (unsigned tries = 0, silent = 0; tries < TX_TRIES && silent < TX_SILENT; tries++) {
        int32_t r = 2;
        if (tries)
            stk_dly_tsk(5);
        stk_fa_tx(f, len);
        for (unsigned s = 0; s < 10 && r == 2; s++) {
            r = stk_fa_wait(25);
            if (r != 0 && stk_fa_cancel())
                r = 3;
        }
        if (r == 0 || r == 3) {
            stk_sig_sem(sem);
            if (r == 3)
                cancels++;
            return r == 0 ? 0 : -2;
        }
        if (r == 2)
            silent++;
    }
    if (rd8(A_BOUND) != 4u) {   /* FUN_4014B550: the panel keeps its binding */
        stk_unbind();
        stk_reset_a();
        stk_reset_b();
        stk_reset_c();
    }
    stk_sig_sem(sem);
    return -1;
}

/* FUN_4014D974 without its two faults: one DataPacket per 21 bytes until the builder marks the last one
 * (bit 7 of its length), counter byte & 0x7F.  0 = done, < 0 = send failed / cancelled (stock: 0xF0).
 * The builder is called as the stock calls it for packets 0 and 1 (counter 0 / 1, the data from 21 (c - 1) on for
 * c >= 1): its group offsets stay 7 i / 21 + 7 i - no 16-bit smulbb overflow past 229 KB - and its size tests
 * see what the stock's see (its unsigned `size - 7` test only differs from the stock's for a remainder < 7 at
 * offset 0, which is packet 0 alone, called exactly as the stock does). */
static int32_t data_packets(const uint8_t *buf, uint32_t size)
{
    wr8(0x413CA3E7u, 0);   /* FUN_4014D5CC: Cancel flag cleared before the first packet */
    for (uint32_t c = 0;; c++) {
        uint8_t f[0x24];
        uint32_t base = c ? 21u * (c - 1u) : 0u, n;
        uint8_t ctr = (uint8_t)(c & 0x7Fu), wrote = (uint8_t)(c ? 1u : 0u);
        for (unsigned i = 0; i < sizeof f; i++)
            f[i] = 0;
        if (!rd8(A_BOUND))
            return 0;   /* FUN_4014D8E0: unbound -> 0, the job goes on to EndOfData */
        n = stk_fa_build(DEV, buf + base, size - base, wrote, f);
        if (n == 0)
            return 0;
        /* the builder wrote counter `wrote` and XOR(f[1..len-3]) & 0x7F: the counter enters the XOR once */
        f[7] = ctr;
        f[(n & 0x7Fu) - 2u] ^= (uint8_t)(ctr ^ wrote);
        if (send_frame(f, n & 0x7Fu) != 0)
            return -1;   /* failed or cancelled (Cancel instead of the ACK): the job's Result 0xF0 */
        if (n & 0x80u)
            return 0;   /* last packet */
        if (stk_fa_cancel())
            return -1;
    }
}

int frc_pass(void)
{
    uint32_t size = 0;
    int32_t rc;
    if (!rd8(A_DUE) || rd8(A_JOB) != JOB_READ)
        return 1;
    wr8(A_DUE, 0);
    reads++;
    rc = stk_fsize((const char *)A_NAME, &size);
    if (rc == 0 && size >= 0x100000u)
        rc = 0xE8;
    else if (rc == 0) {
        uint32_t sem = *(const volatile uint32_t *)A_SEM;
        int held = 1;
        stk_wai_sem(sem, -1);
        rc = stk_fread((const char *)A_NAME, (uint8_t *)A_BUF);
        if (rc == 0) {
            if (stk_fa_top(DEV, size) < 0 || data_packets((const uint8_t *)A_BUF, size) < 0)
                rc = -1;
            else {
                stk_sig_sem(sem);
                held = 0;
                rc = stk_fa_end(DEV);
            }
        }
        if (held)
            stk_sig_sem(sem);
    }
    wr8(A_JOB, 0);
    if (rc == 0)
        stk_fa_result_ok(0);
    else
        stk_fa_result(rc < 0 ? 0xF0u : (uint32_t)rc & 0xFFFFu);
    return 0;
}

/* ---- write side (upload 24 05 + name, 24 01 TopOfData, 24 02 DataPackets, 24 7B) ----------------------------------
 * The stock receiver FUN_4014FB94 puts each DataPacket's three groups at (counter x 3 + g) x 7 of 0x40B00000 with the
 * counter byte as sent (0x4014FBB8 ldrb r0, [r7, #7]).  The app sends counter & 0x7F (a SysEx data byte), so packet 128
 * lands on offset 0 again and no group offset ever reaches a length > 2681: state 2 never comes, 24 7B answers 0xED and
 * nothing is written.
 *
 * Here nothing is answered and no stock routine is called: the stock dispatcher 0x4014A9E4 receives every frame and does
 * all of it (unpack, checksum, NAK / ACK, state 2, Result).  The front only rewrites what the receiver reads, so that the
 * stock works on the true packet index k (counted past the 7-bit counter, the index nearest the last one):
 *   k < 127   nothing changes - the stock's offsets are the true ones (every upload <= 2667 B is the stock's, byte for byte);
 *   k >= 127  the counter byte becomes 127 (the checksum byte patched by the same XOR) and the length word 0x413CA3B4 is
 *             set to len - 21 (k - 127) for that packet: the receiver writes into slot 127 (offset 2667..2687) with the
 *             group lengths and the completion test (a group offset >= length -> state 2) of the true offset 21 k;
 *   next frame of the binding (before the stock sees it): slot 127 is moved to 21 k (packet 127 itself: kept aside -
 *             later packets reuse the slot) and the length word is put back; 24 7B puts packet 127 back into slot 127.
 * A frame the stock rejects resets the upload (FUN_4014B610: state 0, length 0) - then nothing is moved or put back.
 * Field 2026-11-16: the earlier version answered from here (frc_rx: the receiver body + ACK from the front) and the
 * piano stopped answering at the first DataPacket */

#define FW_SLOT 127u
#define FW_SLOT_OFF (FW_SLOT * 21u)   /* 2667 */

static int32_t fw_last = -1;    /* task 19: the highest packet index of this upload, -1 = none yet */
static int32_t fw_staged = -1;  /* the packet the stock was handed in slot 127, -1 = none */
static uint32_t fw_len = 0;       /* the upload length when it was staged */
static uint32_t fw_fake = 0;       /* the length word set for it, 0 = left as it was */
static uint8_t fw_saved = 0;      /* packet 127's bytes are in fw_save */
static uint8_t fw_save[21] = {0};
static volatile uint16_t fw_pkts = 0;   /* packets handed to the stock through slot 127 (debug) */

static void wr32(uint32_t a, uint32_t v)
{
    *(volatile uint32_t *)(uintptr_t)a = v;
}

static void fw_copy(volatile uint8_t *d, const volatile uint8_t *s, uint32_t n)
{
    while (n--)
        *d++ = *s++;
}

/* bytes of packet k in a file of len bytes (0..21) */
static uint32_t fw_bytes(uint32_t k, uint32_t len)
{
    uint32_t off = 21u * k;
    return len <= off ? 0u : (len - off >= 21u ? 21u : len - off);
}

static uint32_t fw_index(uint8_t c)
{
    uint32_t d;
    if (fw_last < 0)
        return c;
    d = (uint32_t)(c - (uint32_t)fw_last) & 0x7Fu;
    if (d <= 64u)
        return (uint32_t)fw_last + d;   /* 0 = the same packet again, 1 = the next one */
    return (uint32_t)fw_last >= 128u - d ? (uint32_t)fw_last - (128u - d) : c;
}

/* the packet staged last time has been through the stock: move it out of slot 127, put the length word back */
static void fw_commit(void)
{
    uint8_t st = rd8(A_UPSTATE);
    if (fw_staged >= 0 && (st == 1u || st == 2u)) {
        uint32_t k = (uint32_t)fw_staged;
        const volatile uint8_t *slot = (const volatile uint8_t *)(uintptr_t)(A_BUF + FW_SLOT_OFF);
        if (k == FW_SLOT) {
            fw_copy(fw_save, slot, fw_bytes(k, fw_len));
            fw_saved = 1;
        } else {
            fw_copy((volatile uint8_t *)(uintptr_t)(A_BUF + 21u * k), slot, fw_bytes(k, fw_len));
        }
        if (fw_fake && rd32(A_UPLEN) == fw_fake)
            wr32(A_UPLEN, fw_len);
    }
    fw_staged = -1;
    fw_fake = 0;
}

int frc_rx(uint32_t port, uint8_t *m, uint32_t n)
{
    uint32_t len, idx, p, g;
    if (n < 8u || m[1] != 0x40u || m[3] != 0x24u || m[4] != 0x08u || m[5] != 0x02u)
        return 0;
    if (m[2] != 0x7Fu && m[2] != (uint8_t)rd32(A_DEVID))
        return 0;   /* FUN_4014A2C8's device test (FUN_40147D60) */
    if (rd8(A_BOUND) != (uint8_t)(port + 1u))
        return 0;   /* FUN_40151BCC: the binding's only */
    fw_commit();
    if (m[6] == 0x01u) {
        fw_last = -1;   /* TopOfData: a new upload (the stock stores it) */
        fw_saved = 0;
        return 0;
    }
    if (m[6] == 0x7Bu) {
        if (fw_saved && rd8(A_UPSTATE) == 2u)
            fw_copy((volatile uint8_t *)(uintptr_t)(A_BUF + FW_SLOT_OFF), fw_save, fw_bytes(FW_SLOT, rd32(A_UPLEN)));
        fw_saved = 0;
        return 0;
    }
    if (m[6] != 0x02u || n < 10u || rd8(A_UPSTATE) != 1u)
        return 0;
    len = rd32(A_UPLEN);
    if (len >= FW_MAX)
        return 0;   /* past the buffer: the stock (offsets < 2.7 KB, ends in 0xED) */
    idx = fw_index(m[7]);
    if (fw_last < 0 || idx > (uint32_t)fw_last)
        fw_last = (int32_t)idx;
    if (idx < FW_SLOT || 21u * idx > len + 21u)
        return 0;   /* the stock's own offsets / not a packet of this upload */
    /* where the receiver will read the checksum: after the groups as the true offset gives them (FUN_4014FB94) */
    for (p = 8u, g = 0; g < 3u; g++) {
        uint32_t off = 21u * idx + 7u * g;
        if (len <= off)
            break;
        p += ((len - 7u <= off) ? ((len - off) & 0xFFu) : 7u) + 1u;
    }
    if (p + 1u >= n || m[p + 1u] != 0xF7u)
        return 0;   /* malformed: the stock NAKs it */
    m[p] = (uint8_t)((m[p] ^ m[7] ^ FW_SLOT) & 0x7Fu);
    m[7] = (uint8_t)FW_SLOT;
    fw_len = len;
    fw_staged = (int32_t)idx;
    if (idx != FW_SLOT) {
        fw_fake = len + FW_SLOT_OFF - 21u * idx;   /* > 2645: the receiver sees slot 127 at the true offset */
        wr32(A_UPLEN, fw_fake);
    }
    fw_pkts++;
    return 0;
}
