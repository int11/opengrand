// emuhooks.c - StockEmu's (tools/stockemu.py) per-event Unicorn callbacks in C.
//
// StockEmu counts every executed instruction (a block hook), logs every CPU store to RAM (a write hook over SDRAM,
// the vector page, map_ram), runs the no-progress loop detector at block starts and replaces stock functions by
// Python code (emu.hook: a code hook per hooked address).  As Python callbacks the first three ran once per basic
// block / store - tens of millions of times per round trip, ~75 % of its CPU (cProfile of roundtrip_m4,
// 2026-11-14) - and every hooked address was its own Unicorn code hook: Unicorn's instruction-count hook (emu_start
// count=, kept: it gives the exact limit and the exact pc the store / peripheral logs record) runs before EVERY
// instruction and walks the whole code-hook list each time, so the time per instruction grew with the number of
// hooks (~0.8 ns per hook per instruction; a Piano rig has ~50).  Here:
//   block  insns += the previous block's length; shift = Thumb (CPSR.T) ? 1 : 2; the new block's start / length;
//          block count; trace (when on, up to the log limit); the no-progress detector (same arming / compare)
//   store  epoch += 1; the Write record (addr, size, value masked to size, pc) up to the log limit, else dropped
//          += 1; a store into a guard range goes to the Python callback (the guard log and its stop, as before)
//   code   ONE code hook for every hooked address and the guard ranges' execution check: an address in a guard
//          range -> the Python guard-exec callback (it stops the run; nothing else runs for that instruction, as
//          when its hook came first in the list); a hooked address (open-addressing table) -> the Python dispatcher
//          (StockEmu._on_fn_hook, unchanged)
// Every stop reason is still decided in Python, in the same order.
//
// Built on first use by stockemu.py (cc -O2 -shared -fPIC, into result/stockfw/build/native/); the Unicorn entry
// points are handed in as function pointers, so the library links against nothing.
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

typedef int (*reg_read_t)(void *uc, int regid, void *value);
typedef int (*reg_read_batch_t)(void *uc, int const *regs, void **vals, int count);
typedef int (*emu_stop_t)(void *uc);
typedef void (*guard_cb_t)(uint64_t addr, uint64_t size, uint64_t value, uint64_t pc);
typedef void (*np_cb_t)(uint64_t addr);
typedef void (*code_cb_t)(uint64_t addr, uint64_t size);

#define NSIG 16
#define MAXGUARD 32
#define NP_TRIES 8          // stockemu.py _NP_TRIES
#define HTAB 4096           // hooked-address table (power of two; StockEmu keeps it at most half full)

typedef struct {
    // set once (StockEmu construction)
    reg_read_t reg_read;
    reg_read_batch_t reg_read_batch;
    emu_stop_t emu_stop;
    guard_cb_t guard_cb;
    np_cb_t np_cb;
    int32_t reg_pc, reg_cpsr;
    int32_t sig_regs[NSIG];
    // per call (StockEmu._reset_run_state)
    uint64_t log_limit;
    uint64_t insns, blk_start, blk_len, nblk, epoch, dropped;
    int64_t blk_shift;
    int64_t trace_on;
    uint64_t np_window, np_next, np_armed_at, np_tries;
    int64_t np_armed;
    uint64_t np_addr, np_epoch, np_gen;
    uint64_t np_regs[NSIG];
    // guard ranges [lo, hi)
    uint64_t nguard;
    uint64_t glo[MAXGUARD], ghi[MAXGUARD];
    // logs: writes as (addr, size, value, pc) quadruples, trace as block addresses
    uint64_t *wbuf;
    uint64_t wlen, wcap;
    uint64_t *tbuf;
    uint64_t tlen, tcap;
    // the code hook: guard execution check (gx_on) and the hooked addresses (stored as addr | 1 - they are 4-aligned;
    // 0 = an empty slot)
    code_cb_t gx_cb;
    code_cb_t fn_cb;
    int64_t gx_on;
    uint64_t htab[HTAB];
} eh_state;

static int grow(uint64_t **buf, uint64_t *cap, uint64_t need) {
    if (need <= *cap) return 1;
    uint64_t nc = *cap ? *cap * 2 : 4096;
    while (nc < need) nc *= 2;
    uint64_t *nb = realloc(*buf, nc * sizeof(uint64_t));
    if (!nb) return 0;
    *buf = nb;
    *cap = nc;
    return 1;
}

static void read_sig(eh_state *s, void *uc, uint64_t *out) {
    void *ptrs[NSIG];
    for (int i = 0; i < NSIG; i++) {
        out[i] = 0;
        ptrs[i] = &out[i];
    }
    s->reg_read_batch(uc, s->sig_regs, ptrs, NSIG);
}

void eh_block(void *uc, uint64_t addr, uint32_t size, void *ud) {
    eh_state *s = (eh_state *)ud;
    s->insns += s->blk_len;  // the previous block ran to its end
    uint64_t cpsr = 0;
    s->reg_read(uc, s->reg_cpsr, &cpsr);
    s->blk_shift = (cpsr & 0x20) ? 1 : 2;  // Thumb: halfwords (a 32-bit BL / BLX counts as two)
    s->blk_start = addr;
    s->blk_len = size >> s->blk_shift;
    s->nblk += 1;
    if (s->trace_on && s->tlen < s->log_limit && grow(&s->tbuf, &s->tcap, s->tlen + 1))
        s->tbuf[s->tlen++] = addr;
    if (!s->np_window) return;
    // the no-progress detector (the steps of the former Python _no_progress_check)
    if (s->np_armed) {
        if (s->np_addr == addr) {
            if (s->epoch == s->np_epoch) {
                uint64_t now[NSIG];
                read_sig(s, uc, now);
                if (memcmp(now, s->np_regs, sizeof now) == 0) {
                    s->np_cb(addr);  // sets the stop reason (with the polled addresses)
                    s->emu_stop(uc);
                    return;
                }
            }
            s->np_tries += 1;
            if (s->np_tries >= NP_TRIES) s->np_armed = 0;
        } else if (s->nblk - s->np_armed_at > s->np_window) {
            s->np_armed = 0;
        }
    }
    if (!s->np_armed && s->nblk >= s->np_next) {
        s->np_armed = 1;
        s->np_addr = addr;
        read_sig(s, uc, s->np_regs);
        s->np_epoch = s->epoch;
        s->np_armed_at = s->nblk;
        s->np_next = s->nblk + s->np_window;
        s->np_tries = 0;
        s->np_gen += 1;  // the polled set starts again (Python keys it by this generation)
    }
}

void eh_write(void *uc, int type, uint64_t addr, int size, int64_t value, void *ud) {
    eh_state *s = (eh_state *)ud;
    (void)type;
    s->epoch += 1;
    uint64_t v = (uint64_t)value;
    if (size < 8) v &= (((uint64_t)1) << (8 * size)) - 1;
    uint64_t pc = 0;
    s->reg_read(uc, s->reg_pc, &pc);
    if (s->wlen < s->log_limit && grow(&s->wbuf, &s->wcap, 4 * (s->wlen + 1))) {
        uint64_t *w = s->wbuf + 4 * s->wlen++;
        w[0] = addr;
        w[1] = (uint64_t)size;
        w[2] = v;
        w[3] = pc;
    } else {
        s->dropped += 1;
    }
    for (uint64_t i = 0; i < s->nguard; i++) {
        if (s->glo[i] <= addr && addr < s->ghi[i]) {
            s->guard_cb(addr, (uint64_t)size, v, pc);
            break;
        }
    }
}

static inline uint64_t hslot(uint64_t a) { return ((a >> 1) * 0x9E3779B97F4A7C15ull) >> 52; }  // 12 bits

void eh_code(void *uc, uint64_t addr, uint32_t size, void *ud) {
    eh_state *s = (eh_state *)ud;
    (void)uc;
    if (s->gx_on) {
        for (uint64_t i = 0; i < s->nguard; i++) {
            if (s->glo[i] <= addr && addr < s->ghi[i]) {
                s->gx_cb(addr, size);   // logs the hit and stops the run
                return;
            }
        }
    }
    for (uint64_t k = hslot(addr);; k = (k + 1) & (HTAB - 1)) {
        uint64_t a = s->htab[k];
        if (a == (addr | 1)) {
            s->fn_cb(addr, size);
            return;
        }
        if (a == 0) return;
    }
}

// StockEmu.hook / unhook: the table rebuilt from the list of hooked addresses
void eh_set_hooks(eh_state *s, const uint64_t *addrs, uint64_t n) {
    memset(s->htab, 0, sizeof s->htab);
    for (uint64_t i = 0; i < n; i++) {
        uint64_t k = hslot(addrs[i]);
        while (s->htab[k] != 0 && s->htab[k] != (addrs[i] | 1)) k = (k + 1) & (HTAB - 1);
        s->htab[k] = addrs[i] | 1;
    }
}

void eh_free(eh_state *s) {
    free(s->wbuf);
    free(s->tbuf);
    s->wbuf = s->tbuf = NULL;
    s->wlen = s->wcap = s->tlen = s->tcap = 0;
}
