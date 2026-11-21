/* OpenGrand — RAM canary: fill large RAM blocks that the stock firmware is believed never to use with an
 * address-dependent pattern, then keep re-reading them and count the words that no longer hold it.  It is the field
 * proof a block needs before OpenGrand code may live there (hardware baseline study, layout L1).
 *
 * Platform-neutral: the regions are a table the port hands in (address the reports use, size, and where the words
 * actually are - the same address on the piano, a test buffer on the host).  The work is bounded per step: the
 * port calls rcn_step() once per engine-loop pass with a word budget, so a pass costs the same whether 256 KB or
 * 16 MB are armed.  Nothing is written before rcn_arm(), and nothing at all outside the armed regions.
 *
 * Fill first (regions in index order), then verify: the verify cursor walks the armed regions round robin, each
 * region's sweep ends at its last word (sweeps + 1, the sweep's mismatch count becomes changed_last).  A changed word
 * is reported, never written back: whatever the stock (or anything else) put there stays, and the same word is
 * counted again by the next sweep while it stays changed - changed_last is "how many words differ now", min / max
 * address are sticky since the arm, first_* is the first mismatch ever seen (address, the value read, the sweep it
 * was found in). */
#ifndef CA79NG_RAMCANARY_H
#define CA79NG_RAMCANARY_H

#include <stddef.h>
#include <stdint.h>

#define RCN_REGIONS_MAX 8u
#define RCN_XOR 0xC79A5A5Cu           /* the R1 canary's pattern (probe op 0x09): word at a = a ^ RCN_XOR */
#define RCN_PASS_WORDS 2048u          /* words filled / verified per engine-loop pass (8 KB, half the D-cache) */
#define RCN_STATUS 32                 /* rcn_encode_status raw bytes */
#define RCN_REGION 48                 /* rcn_encode_region raw bytes */

static inline uint32_t rcn_pattern(uint32_t addr) { return addr ^ RCN_XOR; }

typedef struct {
    uint32_t lo, hi;                 /* reported addresses: [lo, hi), both 4-aligned                       */
    uintptr_t base;                  /* where the word of lo is (the same as lo on the piano)              */
} rcn_region_t;

enum { RCN_OFF = 0, RCN_FILL = 1, RCN_VERIFY = 2 };   /* per-region state (rcn_reg_t.state) */
enum { RCN_PH_OFF = 0, RCN_PH_FILL = 1, RCN_PH_VERIFY = 2 };

typedef struct {
    uint8_t state;                   /* RCN_*                                                               */
    uint32_t cur;                    /* word index: the fill cursor (RCN_FILL) or the verify cursor          */
    uint32_t sweeps;                 /* verify sweeps completed since the arm                               */
    uint32_t cnt;                    /* mismatches so far in the running sweep                              */
    uint32_t changed_last;           /* mismatches of the last completed sweep                              */
    uint32_t changed_max;            /* largest changed_last since the arm                                  */
    uint32_t min_addr, max_addr;     /* lowest / highest changed address seen since the arm (0 = none)      */
    uint32_t first_addr, first_val;  /* the first mismatch seen: its address and the value read there       */
    uint32_t first_sweep;            /* the sweep (1-based) it was found in, 0 = none                       */
} rcn_reg_t;

typedef struct {
    const rcn_region_t *tab;
    uint8_t n;
    uint8_t phase;                   /* RCN_PH_*                                                             */
    uint8_t vi;                      /* region the verify cursor is in                                      */
    uint16_t armed, filled, dirty;   /* region bit masks: armed, fill complete, any mismatch seen           */
    uint32_t arms;                   /* rcn_arm calls that started a run (since rcn_init)                     */
    uint32_t passes;                 /* rcn_step calls with work since the arm                               */
    uint32_t filled_words, verified_words;   /* since the arm                                               */
    rcn_reg_t r[RCN_REGIONS_MAX];
} rcn_t;

void rcn_init(rcn_t *c, const rcn_region_t *tab, uint8_t n);
/* mask = regions to fill and watch; any run before ends (its records are cleared).  0 ok, 1 bad mask (0, or a
 * region the table does not have). */
int rcn_arm(rcn_t *c, uint16_t mask);
/* stop: no further reads or writes; the records stay readable until the next arm */
void rcn_disarm(rcn_t *c);
/* one step of at most `budget` words (fill, else verify); returns the words touched */
uint32_t rcn_step(rcn_t *c, uint32_t budget);
/* total words of the region mask (sizes only) */
uint32_t rcn_words(const rcn_t *c, uint16_t mask);

/* status record (RCN_STATUS bytes, LE): 0 layout 1 · 1 regions in the table · 2 phase · 3 0 · 4 u16 armed · 6 u16 filled
 * · 8 u16 dirty · 10 u16 words per pass · 12 u32 passes · 16 u32 words filled · 20 u32 words verified · 24 u32 last
 * pass us · 28 u32 longest pass us (the port's timing: 0 where it has none) */
void rcn_encode_status(const rcn_t *c, uint32_t budget, uint32_t last_us, uint32_t max_us, uint8_t raw[RCN_STATUS]);
/* region record (RCN_REGION bytes, LE): 0 state · 1 armed 0/1 · 2 u16 0 · 4 u32 lo · 8 u32 hi · 12 u32 cursor (words)
 * · 16 u32 sweeps · 20 u32 changed_last · 24 u32 changed_max · 28 u32 min addr · 32 u32 max addr · 36 u32 first addr
 * · 40 u32 first value · 44 u32 first sweep.  i outside the table: all zero. */
void rcn_encode_region(const rcn_t *c, unsigned i, uint8_t raw[RCN_REGION]);

#endif
