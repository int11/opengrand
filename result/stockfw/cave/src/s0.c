/* OpenGrand OTA loader v4 - STAGE-0, the frozen part of image ota_loader_v4 (USB only; interface include/s0.h,
 * contract REMOTE_PROTOCOL.md section 10.12).
 *
 * Two stock instructions are redirected here by the .SYS (tools/make_phase2_manifest.py):
 *   sysex-mfr    0x40147E1C  `bne 0x40147E98`   -> `bne s0_h1`    (unknown manufacturer ID)
 *   engine-tail  0x40143AE0  `bl  0x40142C90`   -> `bl  s0_tick`  (engine loop tail, task 24, ~8 ms)
 * and loader copy #1 of the stock boot is extended to 0x40176000 so this code, its .data and the root key at
 * S0_KEY_VA reach SDRAM.  Nothing else of OpenGrand is in the .SYS.
 *
 * Job, in engine-loop passes, never blocking the stock boot:
 *   1. escape keys (BootProgram mailbox, MainProgram's copy, stock key-scan state): E1 A0+A#0+B0 -> pure stock
 *      for this boot; E2 A0+A#0 -> SAFE (the older loader first, the stage-1 contract: no main);
 *   2. both loader slot states (frozen view, 0x30 B), candidates = sealed, not rejected, confirmed or < 3 tries;
 *      highest seq first (E2: lowest);
 *   3. per candidate: unconfirmed -> program the next attempt halfword FIRST and read it back (no try mark, no
 *      run); header check; NOR -> stage-1 window 4 KB per pass with CRC-32 and the signature; cache sync; entry
 *      table check; init(&s0_boot).  Any failure -> next candidate; none -> stock for this boot;
 *   4. then every pass stage1->tick(), every 0x7D SysEx stage1->sysex(); RESELECT on request.
 * Plus the frozen service table s0_svc for stage-1 and the main: the ONLY callers of the stock NOR writers
 * (guarded by the frozen range table s0_nor_ranges: slot sectors + kvstore always, three gates OFF until stage-1
 * switches them ON for this boot with nor_enable; never erase / page-program the running loader sector; scoped by
 * caller: stage-0 / stage-1 code may write every ON entry, any other caller - the main - only the kvstore and gates
 * granted main_writable), cache sync and the one signature check.
 *
 * Flash rules (as ota_loader_v3): stock writers only inside dis_dsp after the stock idle check, the stock busy flag
 * 0x4139DAC4 raised around each call and back to 0 before ena_dsp (the only stock global written, besides the NORTi
 * error word 0x4139E778 that the stock dly_tsk stores under dis_dsp - the stock shutdown path does the same).
 * Everything here runs in User mode (task 24 / task 19); CP15 only through the stock SWI #0 path.
 * Code: Thumb C (size: the frozen budget must keep room for an Ed25519 verifier), ARM where entered from outside -
 * the two hook entries, s0_sysex, the three NOR service functions and the cache sync (4-aligned ARM entry points);
 * sig_check is a Thumb entry (call it with blx / bx: ARMv5TE interworking).  Stage-1's entry points must be ARM
 * (4-aligned) - a Thumb stage-1 uses ARM trampolines, as the Thumb main does (src/ota_payload.c).
 */
#include "stock.h"
#include "s0.h"

/* stock NOR writers (stock_norw.ld - linked into stage-0 ONLY), deferred-writer jobs (stock_ota.ld) */
int stk_nor_erase(u32 cpu_addr);
int stk_nor_prog_buf(u32 cpu_addr, const void *src, u32 len);
int stk_nor_prog_hw(u32 cpu_addr, u32 hw);
void stk_flash_busy_set(int on);
int stk_flash_jobs(void);

extern const u8 s0_key[32];      /* linker: S0_KEY_VA, patched in from secrets/ota_key.bin */
extern const volatile u8 stk_key_state[88];
extern const volatile u8 stk_key_scan_on;

#define STEP            0x1000u    /* NOR bytes read + hashed per engine-loop pass */
#define BOOT_BUSY_MAX   1250u      /* ~10 s of a stock writer holding the chip at boot */
#define ESC_WAIT_MAX    250u       /* passes to wait for the stock key scan (normally 0) */
#define ESC1_CODE       0x40000007u /* BootProgram key code: keys 0-2 = A0 A#0 B0 only */
#define ESC2_CODE       0x40000003u /* keys 0-1 = A0 A#0 only */
#define MBOX_MAGIC      0x55AAA55Au
#define KEY_DOWN(b)     (((b) & 0xE0u) == 0xE0u)

enum { S_START = 0, S_SCAN, S_CAND, S_MARK, S_HDR, S_LOAD, S_RUN, S_STOCK };

/* ---- cache sync: D-clean by MVA + drain + I-invalidate, privileged through the stock SWI #0 dispatcher (handler
 * 0x400A8884: SWI 0 -> bx r0 in SVC mode, IRQs off; the body returns through *0x4009D000 = the stock return stub).
 * SDRAM is mapped write-back + read-allocate (MMU table 0x4009D038): code just copied may still sit in dirty D-cache
 * lines and old instructions in the I-cache - clean the range by MVA, invalidate the whole I-cache, drain the write
 * buffer, all before the exception return refetches (the reasoning of ota_loader_v3's field-proven cache sync).
 * The instruction order differs from v3's on purpose: no 16-byte run equal to the stock firmware's own cache routine. */
void s0_cache_sync(u32 addr, u32 len);
__asm__(
    "    .pushsection .text.s0_cache_sync, \"ax\", %progbits\n"
    "    .arm\n"
    "    .global s0_cache_sync\n"
    "    .type   s0_cache_sync, %function\n"
    "s0_cache_sync:\n"
    "    push    {r0, r1, r4, r5, ip, lr}\n"
    "    add     r5, r0, r1\n"                    /* r5 = end */
    "    bic     r4, r0, #31\n"                   /* r4 = first line */
    "    ldr     r0, =s0_cache_body\n"
    "    svc     #0\n"
    "    pop     {r0, r1, r4, r5, ip, lr}\n"
    "    bx      lr\n"
    "    .ltorg\n"
    "    .size   s0_cache_sync, . - s0_cache_sync\n"
    "    .global s0_cache_body\n"
    "    .type   s0_cache_body, %function\n"
    "s0_cache_body:\n"                            /* SVC mode, IRQ off; r4 = line, r5 = end */
    "1:  mcr     p15, 0, r4, c7, c10, 1\n"        /* clean D-cache line by MVA */
    "    add     r4, r4, #32\n"
    "    cmp     r5, r4\n"
    "    bhi     1b\n"
    "    mov     r0, #0\n"
    "    mcr     p15, 0, r0, c7, c5, 0\n"         /* invalidate the whole I-cache */
    "    mcr     p15, 0, r0, c7, c10, 4\n"        /* drain write buffer (before the exception return refetches) */
    "    ldr     lr, =stk_svc_return_ptr\n"
    "    ldr     lr, [lr]\n"
    "    bx      lr\n"
    "    .ltorg\n"
    "    .size   s0_cache_body, . - s0_cache_body\n"
    "    .popsection\n");

/* ---- the two hook entries --------------------------------------------------------------------------------
 * s0_h1: reached from the stock SysEx end handler when buf[1] is not 0x40/0x7E/0x7F/0x41: ip = buf[1], r0 = total
 * length incl. F0/F7, r7 = port, r9 = buffer (r4-r9 callee saved for the stock function we are inside of; its
 * epilogue uses r8).  Anything not claimed goes to 0x40147E98 (stock: drop) with every register as it came.
 * s0_tick: the engine loop's `bl 0x40142C90` (result unused): our pass, then tail-call it; r4 is live in the loop. */
__asm__(
    "    .pushsection .text.entry, \"ax\", %progbits\n"
    "    .arm\n"
    "    .global s0_h1\n"
    "    .type   s0_h1, %function\n"
    "s0_h1:\n"
    "    cmp     ip, #0x7D\n"
    "    bne     1f\n"
    "    push    {r0-r9, ip, lr}\n"               /* the frame handed to stage-1 (12 words, 8-byte aligned) */
    "    mov     r3, sp\n"
    "    mov     r2, r0\n"
    "    mov     r1, r9\n"
    "    mov     r0, r7\n"
    "    bl      s0_sysex\n"
    "    pop     {r0-r9, ip, lr}\n"
    "1:  b       stk_end_handler_done\n"
    "    .size   s0_h1, . - s0_h1\n"
    "    .global s0_tick\n"
    "    .type   s0_tick, %function\n"
    "s0_tick:\n"
    "    push    {r4, lr}\n"
    "    bl      s0_step\n"
    "    pop     {r4, lr}\n"
    "    b       stk_engine_tail\n"
    "    .size   s0_tick, . - s0_tick\n"
    "    .popsection\n");

/* ---- state (.data: copied from NOR by the stock boot, so fresh at every power-on) ------------------------- */
typedef struct { u32 h[8]; u32 n; u32 nb; u8 buf[64]; } sha_t;

static int nor_erase(u32 a);
static int nor_prog(u32 a, const void *src, u32 n);
static int nor_prog_hw(u32 a, u32 hw);
static int sig_check(const u8 *hdr, const u8 *payload, u32 len);
static int nor_enable(u32 gate, u32 main_writable);

const struct s0_svc s0_svc = {
    S0_SVC_MAGIC, S0_SVC_VERSION, sizeof(struct s0_svc),
    nor_erase, nor_prog, nor_prog_hw, s0_cache_sync, sig_check, nor_enable
};

struct s0_boot s0_boot = {
    S0_BOOT_MAGIC, S0_BOOT_VERSION, sizeof(struct s0_boot), S0_VERSION, 0xFF, S0R_SELECTING, 0, S0_REQ_NONE,
    S0_ALG_HMAC, S0_MAX_TRIES, 0, s0_key, 0, S0_WINDOW, S0_WINDOW_SIZE, &s0_svc,
    { { 0, 0, 0, { 0, 0 } }, { 0, 0, 0, { 0, 0 } } }, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, { 0, 0 }
};

/* THE NOR range table (include/s0.h): frozen, const - gates only switch entries ON.  Gate entries carry their bit. */
const struct s0_nor_range s0_nor_ranges[S0_NOR_RANGES] = {
    { S0_NOR_SLOTS_LO, S0_NOR_SLOTS_HI, 0, 0, { 0, 0 } },              /* slots: stage-0 / stage-1 only     */
    { S0_NOR_KV_LO, S0_NOR_KV_HI, 0, S0RF_MAIN, { 0, 0 } },            /* kvstore: + the main               */
    { S0_NOR_G0_LO, S0_NOR_G0_HI, 1u << 0, 0, { 0, 0 } },              /* gate 0: unused_2 + TouchCalib     */
    { S0_NOR_G1_LO, S0_NOR_G1_HI, 1u << 1, 0, { 0, 0 } },              /* gate 1: 6 - 8 MB                  */
    { S0_NOR_G2_LO, S0_NOR_G2_HI, 1u << 2, 0, { 0, 0 } },              /* gate 2: 8 - 16 MB (mirror risk)   */
};
/* no entry may ever reach BootProgram (the table is const: this also holds after any nor_enable) */
#define S0_ABOVE_BOOT(lo, hi) ((lo) >= S0_NOR_BOOT_HI && (hi) > (lo))
_Static_assert(S0_ABOVE_BOOT(S0_NOR_SLOTS_LO, S0_NOR_SLOTS_HI) && S0_ABOVE_BOOT(S0_NOR_KV_LO, S0_NOR_KV_HI) &&
               S0_ABOVE_BOOT(S0_NOR_G0_LO, S0_NOR_G0_HI) && S0_ABOVE_BOOT(S0_NOR_G1_LO, S0_NOR_G1_HI) &&
               S0_ABOVE_BOOT(S0_NOR_G2_LO, S0_NOR_G2_HI), "a NOR range entry reaches BootProgram");
_Static_assert(sizeof(struct s0_boot) == 76 && sizeof(struct s0_svc) == 32 && sizeof(struct s0_nor_range) == 12,
               "frozen layout");

static const struct s1_entry *volatile s0_active = 0;
volatile u8 s0_in_sysex = 0;     /* task 19 writes, task 24 reads under dis_dsp */
volatile u8 s0_state = S_START;
static u8 cand[2] = { 0, 0 };
static u8 ncand = 0, ci = 0;
static u32 busy_n = 0, esc_wait = 0, pos = 0, crc = 0;
static u8 hdr[S0_HDR_SIZE] = { 0 };
static sha_t mac = { { 0 }, 0, 0, { 0 } };

static const u32 lbase[2] = { S0_SLOT_LDR_A, S0_SLOT_LDR_B };

/* ---- small helpers ------------------------------------------------------------------------------------------ */
static u32 get32(const u8 *p) { return p[0] | (u32)p[1] << 8 | (u32)p[2] << 16 | (u32)p[3] << 24; }
static u32 get16(const u8 *p) { return p[0] | (u32)p[1] << 8; }

static u32 crc32_step(u32 c, const u8 *p, u32 n)
{
    while (n--) {
        u32 k;
        c ^= *p++;
        for (k = 0; k < 8; k++)
            c = (c >> 1) ^ (0xEDB88320u & (0u - (c & 1u)));
    }
    return c;
}

/* ---- SHA-256 / HMAC-SHA256 (FIPS 180-4, RFC 2104) ----------------------------------------------------------- */
static const u32 K256[64] = {
    0x428a2f98u, 0x71374491u, 0xb5c0fbcfu, 0xe9b5dba5u, 0x3956c25bu, 0x59f111f1u, 0x923f82a4u, 0xab1c5ed5u,
    0xd807aa98u, 0x12835b01u, 0x243185beu, 0x550c7dc3u, 0x72be5d74u, 0x80deb1feu, 0x9bdc06a7u, 0xc19bf174u,
    0xe49b69c1u, 0xefbe4786u, 0x0fc19dc6u, 0x240ca1ccu, 0x2de92c6fu, 0x4a7484aau, 0x5cb0a9dcu, 0x76f988dau,
    0x983e5152u, 0xa831c66du, 0xb00327c8u, 0xbf597fc7u, 0xc6e00bf3u, 0xd5a79147u, 0x06ca6351u, 0x14292967u,
    0x27b70a85u, 0x2e1b2138u, 0x4d2c6dfcu, 0x53380d13u, 0x650a7354u, 0x766a0abbu, 0x81c2c92eu, 0x92722c85u,
    0xa2bfe8a1u, 0xa81a664bu, 0xc24b8b70u, 0xc76c51a3u, 0xd192e819u, 0xd6990624u, 0xf40e3585u, 0x106aa070u,
    0x19a4c116u, 0x1e376c08u, 0x2748774cu, 0x34b0bcb5u, 0x391c0cb3u, 0x4ed8aa4au, 0x5b9cca4fu, 0x682e6ff3u,
    0x748f82eeu, 0x78a5636fu, 0x84c87814u, 0x8cc70208u, 0x90befffau, 0xa4506cebu, 0xbef9a3f7u, 0xc67178f2u,
};

static u32 ror(u32 x, u32 n) { return (x >> n) | (x << (32 - n)); }

static void sha_block(u32 *h, const u8 *p)
{
    u32 w[64], s[8], i, t1, t2;
    for (i = 0; i < 16; i++)
        w[i] = (u32)p[4 * i] << 24 | (u32)p[4 * i + 1] << 16 | (u32)p[4 * i + 2] << 8 | p[4 * i + 3];
    for (; i < 64; i++)
        w[i] = w[i - 16] + (ror(w[i - 15], 7) ^ ror(w[i - 15], 18) ^ (w[i - 15] >> 3)) + w[i - 7] +
               (ror(w[i - 2], 17) ^ ror(w[i - 2], 19) ^ (w[i - 2] >> 10));
    for (i = 0; i < 8; i++)
        s[i] = h[i];
    for (i = 0; i < 64; i++) {
        t1 = s[7] + (ror(s[4], 6) ^ ror(s[4], 11) ^ ror(s[4], 25)) + ((s[4] & s[5]) ^ (~s[4] & s[6])) + K256[i] + w[i];
        t2 = (ror(s[0], 2) ^ ror(s[0], 13) ^ ror(s[0], 22)) + ((s[0] & s[1]) ^ (s[0] & s[2]) ^ (s[1] & s[2]));
        s[7] = s[6]; s[6] = s[5]; s[5] = s[4]; s[4] = s[3] + t1; s[3] = s[2]; s[2] = s[1]; s[1] = s[0]; s[0] = t1 + t2;
    }
    for (i = 0; i < 8; i++)
        h[i] += s[i];
}

static void sha_update(sha_t *s, const u8 *p, u32 n)
{
    s->n += n;
    while (n--) {
        s->buf[s->nb++] = *p++;
        if (s->nb == 64) {
            sha_block(s->h, s->buf);
            s->nb = 0;
        }
    }
}

static void sha_final(sha_t *s, u8 out[32])
{
    u32 i;
    u8 pad = 0x80, z = 0, len[8];
    for (i = 0; i < 4; i++) {
        len[i] = (u8)((s->n >> 29) >> (24 - 8 * i));      /* bit count, 64-bit big-endian */
        len[4 + i] = (u8)((s->n << 3) >> (24 - 8 * i));
    }
    sha_update(s, &pad, 1);
    while (s->nb != 56)
        sha_update(s, &z, 1);
    sha_update(s, len, 8);
    for (i = 0; i < 32; i++)
        out[i] = (u8)(s->h[i >> 2] >> (24 - 8 * (i & 3)));
}

/* HMAC with the root key: pad byte 0x36 (inner) / 0x5C (outer) */
static void hmac_start(sha_t *s, u8 padb)
{
    static const u32 iv[8] = { 0x6a09e667u, 0xbb67ae85u, 0x3c6ef372u, 0xa54ff53au,
                               0x510e527fu, 0x9b05688cu, 0x1f83d9abu, 0x5be0cd19u };
    u32 i;
    u8 b;
    for (i = 0; i < 8; i++)
        s->h[i] = iv[i];
    s->n = 0;
    s->nb = 0;
    for (i = 0; i < 64; i++) {
        b = (u8)((i < 32 ? s0_key[i] : 0) ^ padb);
        sha_update(s, &b, 1);
    }
}

static void hmac_end(sha_t *s, u8 out[32])
{
    u8 inner[32];
    sha_final(s, inner);
    hmac_start(s, 0x5C);
    sha_update(s, inner, 32);
    sha_final(s, out);
}

/* ---- THE signature check (alg-specific code only below this line) -------------------------------------------
 * sig_begin: 0 = the header names an algorithm this stage-0 implements and the root key's id; sig_update /
 * sig_end stream the signed bytes hdr[0:0x30] || payload.  S0_ALG_ED25519 would be added here (and only here). */
static u32 key_id(void)
{
    u8 out[32];
    hmac_start(&mac, 0x36);
    sha_update(&mac, (const u8 *)"C79 key id", 10);
    hmac_end(&mac, out);
    return get32(out);
}

static int sig_begin(const u8 *h)
{
    u32 alg = get16(h + S0_H_FORMAT) == 1 && h[S0_H_ALG] == 0 ? S0_ALG_HMAC : h[S0_H_ALG];
    if (alg != S0_ALG_HMAC || get32(h + S0_H_KEYID) != s0_boot.key_id)
        return S0_SIG_HEADER;
    hmac_start(&mac, 0x36);
    sha_update(&mac, h, S0_H_MAC);
    return S0_SIG_OK;
}

static int sig_end(const u8 *h)
{
    u8 out[32];
    u32 i, diff = 0;
    hmac_end(&mac, out);
    for (i = 0; i < 16; i++)
        diff |= out[i] ^ h[S0_H_MAC + i];
    return diff ? S0_SIG_MAC : S0_SIG_OK;
}

/* service: one call over a RAM image (stage-1 checks a loader upload with it before anything is erased).  Shares
 * the context with the boot path, which never runs at the same time (stage-1 runs only after the boot finished). */
static int sig_check(const u8 *h, const u8 *payload, u32 len)
{
    int rc = sig_begin(h);
    if (rc != S0_SIG_OK)
        return rc;
    sha_update(&mac, payload, len);
    return sig_end(h);
}

/* ---- NOR -------------------------------------------------------------------------------------------------- */
static int flash_idle(void)
{
    return !stk_flash_busy() && !stk_flash_jobs();
}

static int nor_read(u32 src, void *dst, u32 len)
{
    int ok;
    stk_dis_dsp();
    ok = flash_idle();
    if (ok)
        stk_flash_copy(src, dst, len);
    stk_ena_dsp();
    return ok ? S0_NOR_OK : S0_NOR_BUSY;
}

static int trusted(u32 from)
{
    return from - S0_TRUST_LO < S0_TRUST_HI - S0_TRUST_LO;
}

/* the table entry holding address a (0 = none) */
static const struct s0_nor_range *range_of(u32 a)
{
    const struct s0_nor_range *r;
    for (r = s0_nor_ranges; r < s0_nor_ranges + S0_NOR_RANGES; r++)
        if (a >= r->lo && a < r->hi)
            return r;
    return 0;
}

/* HARD guard: the whole access inside ONE table entry (include/s0.h) that is ON and open to the caller, aligned;
 * erase / page program never in the loader sector stage-0 runs (s0_boot.slot); never below S0_NOR_BOOT_HI whatever
 * the table says.  Checked before anything else; these three are the only callers of the writers.
 * Caller scope: `from` = the return address of the service call.  Stage-0 itself and the stage-1 window
 * [S0_TRUST_LO, S0_TRUST_HI) get every ON entry; every other caller - the main (core RW / run area 0x40180000+),
 * stock code, anything else - only S0RF_MAIN entries (the kvstore) and the gates granted main_writable. */
static int guard(u32 a, u32 n, u32 align, int whole, u32 from)
{
    const struct s0_boot *b = &s0_boot;
    const struct s0_nor_range *r = range_of(a);
    if (r && a >= S0_NOR_BOOT_HI && n != 0 && (a & (align - 1)) == 0 && n <= r->hi - a &&
        (!r->gate || (b->nor_gates & r->gate)) &&
        (trusted(from) || (r->flags & S0RF_MAIN) || (b->nor_gates_main & r->gate)) &&
        !(whole && b->slot < 2 && a >= lbase[b->slot] && a < lbase[b->slot] + S0_SECTOR))
        return 1;
    s0_boot.guard_trips++;
    return 0;
}

/* the caller of the service function this is used in (ARM, noinline: lr at entry = the caller's return address) */
#define FROM() ((u32)__builtin_return_address(0))

/* service: switch gate `gate` ON for this boot (main_writable: also for the main).  Only grows the masks.  Before a
 * gate's first enable: its first S0_ALIAS_LEN bytes must differ from BootProgram's (else a mirror -> refused). */
__attribute__((noinline, target("arm"))) static int nor_enable(u32 gate, u32 main_writable)
{
    struct s0_boot *b = &s0_boot;
    const struct s0_nor_range *r;
    u32 g[S0_ALIAS_LEN / 4], bp[S0_ALIAS_LEN / 4];    /* word-aligned: the stock copier stores words */
    u32 i, diff = 0;
    u8 bit;
    if (!trusted(FROM()) || gate >= S0_GATES) {
        b->guard_trips++;
        return S0_NOR_GUARD;
    }
    bit = (u8)(1u << gate);
    if (!(b->nor_gates & bit)) {
        for (r = s0_nor_ranges; r->gate != bit; r++)
            ;
        if (nor_read(r->lo, g, sizeof g) != S0_NOR_OK || nor_read(S0_NOR_BOOT_LO, bp, sizeof bp) != S0_NOR_OK)
            return S0_NOR_BUSY;
        for (i = 0; i < S0_ALIAS_LEN / 4; i++)
            diff |= g[i] ^ bp[i];
        if (!diff)
            return S0_NOR_ALIAS;
    }
    b->nor_gates |= bit;
    if (main_writable)
        b->nor_gates_main |= bit;
    return S0_NOR_OK;
}

#define OWN_CHIP(call)                              \
    do {                                            \
        int rc;                                     \
        stk_dis_dsp();                              \
        if (!flash_idle()) {                        \
            stk_ena_dsp();                          \
            return S0_NOR_BUSY;                     \
        }                                           \
        stk_flash_busy_set(1);                      \
        rc = (call);                                \
        stk_flash_busy_set(0);                      \
        stk_ena_dsp();                              \
        return rc ? S0_NOR_FAIL : S0_NOR_OK;        \
    } while (0)

__attribute__((noinline, target("arm"))) static int nor_erase(u32 a)
{
    if (!guard(a, S0_SECTOR, S0_SECTOR, 1, FROM()))
        return S0_NOR_GUARD;
    OWN_CHIP(stk_nor_erase(a));
}

__attribute__((noinline, target("arm"))) static int nor_prog(u32 a, const void *src, u32 n)
{
    if (!guard(a, n, 2, 1, FROM()))
        return S0_NOR_GUARD;
    if ((n & 1) || n > S0_PAGE || (a & (S0_PAGE - 1)) + n > S0_PAGE) {
        s0_boot.guard_trips++;
        return S0_NOR_GUARD;
    }
    OWN_CHIP(stk_nor_prog_buf(a, src, n));
}

__attribute__((noinline, target("arm"))) static int nor_prog_hw(u32 a, u32 hw)
{
    if (!guard(a, 2, 2, 0, FROM()))
        return S0_NOR_GUARD;
    OWN_CHIP(stk_nor_prog_hw(a, hw & 0xFFFF));
}

/* ---- boot ------------------------------------------------------------------------------------------------- */
/* 1 = E1 (stock), 2 = E2 (safe), 0 = none, -1 = ask again */
static int escape_check(void)
{
    const volatile u32 *mbox = (const volatile u32 *)0x8001F7F8u;   /* BootProgram key mailbox */
    struct s0_boot *b = &s0_boot;
    u32 i, down = 0, low = 0;
    if (esc_wait == 0) {
        b->diag_mbox0 = mbox[0];
        b->diag_mbox1 = mbox[1];
        b->diag_copy = *(const volatile u32 *)0x40000000u;             /* MainProgram's copy */
        if (b->diag_mbox0 == MBOX_MAGIC)
            b->diag_esc |= b->diag_mbox1 == ESC1_CODE ? S0E_MBOX : b->diag_mbox1 == ESC2_CODE ? S0E_MBOX2 : 0;
        b->diag_esc |= b->diag_copy == ESC1_CODE ? S0E_COPY : b->diag_copy == ESC2_CODE ? S0E_COPY2 : 0;
    }
    if (!stk_key_scan_on && esc_wait < ESC_WAIT_MAX && !b->diag_esc) {
        esc_wait++;
        return -1;
    }
    b->diag_esc_wait = (u8)(esc_wait > 255 ? 255 : esc_wait);
    if (stk_key_scan_on) {
        b->diag_esc |= S0E_SCAN_ON;
        for (i = 0; i < 88; i++)
            if (KEY_DOWN(stk_key_state[i])) {
                down++;
                if (i < 7)
                    low |= 1u << i;
            }
        b->diag_keys_down = (u8)down;
        b->diag_keys_low = (u8)low;
        if (down == 3 && low == 0x07)
            b->diag_esc |= S0E_KEYS;
        if (down == 2 && low == 0x03)
            b->diag_esc |= S0E_KEYS2;
    }
    if (b->diag_esc & (S0E_MBOX | S0E_COPY | S0E_KEYS))
        return 1;
    return (b->diag_esc & (S0E_MBOX2 | S0E_COPY2 | S0E_KEYS2)) ? 2 : 0;
}

static void parse(struct s0_slot *s, const u8 *st)
{
    u32 i, used = 0;
    s->flags = S0SF_SCANNED;
    s->seq = get32(st + S0_ST_SEQ);
    if (get32(st + S0_ST_MAGIC) == S0_LDR_MAGIC && s->seq != 0xFFFFFFFFu)
        s->flags |= S0SF_VALID;
    if (get16(st + S0_ST_CONFIRM) != 0xFFFF)
        s->flags |= S0SF_CONFIRMED;
    if (get16(st + S0_ST_REJECT) != 0xFFFF)
        s->flags |= S0SF_REJECTED;
    for (i = 0; i < S0_ATTEMPTS; i++)
        if (get16(st + S0_ST_ATTEMPT + 2 * i) != 0xFFFF)
            used = i + 1;
    s->tries = (u8)used;
    if (!(s->flags & S0SF_CONFIRMED) && used >= S0_MAX_TRIES)
        s->flags |= S0SF_EXHAUSTED;
}

static void to_stock(u8 reason)
{
    s0_boot.reason = reason;
    s0_boot.slot = 0xFF;
    s0_state = S_STOCK;
}

static void skip(void)
{
    s0_boot.lslot[cand[ci]].flags |= S0SF_BAD;
    ci++;
    s0_state = S_CAND;
}

/* a stock writer holds the chip: 1 = wait (this pass is over), 0 = gave up */
static int wait_busy(void)
{
    s0_boot.busy_defers++;
    return ++busy_n <= BOOT_BUSY_MAX;
}

static int in_window(u32 p, u32 len)
{
    return p >= S0_WINDOW && p < S0_WINDOW + len && !(p & 3u);
}

static void finish(void)
{
    struct s0_boot *b = &s0_boot;
    u32 len = get32(hdr + S0_H_LEN), e = S0_WINDOW + get32(hdr + S0_H_ENTRY);
    const struct s1_entry *t = (const struct s1_entry *)e;
    u8 c = cand[ci];
    if ((crc ^ 0xFFFFFFFFu) != get32(hdr + S0_H_CRC) || sig_end(hdr) != S0_SIG_OK) {
        skip();
        return;
    }
    s0_cache_sync(S0_WINDOW, len);
    if (t->magic != S1_ENTRY_MAGIC || t->abi != S0_ABI || t->size < sizeof(struct s1_entry) ||
        !in_window((u32)t->init, len) || !in_window((u32)t->sysex, len) || !in_window((u32)t->tick, len) ||
        !in_window((u32)t->release, len)) {
        skip();
        return;
    }
    b->slot = c;
    b->lslot[c].flags |= S0SF_BOOTED;
    if (b->reason != S0R_RESELECT)
        b->reason = (b->flags & S0F_SAFE) ? S0R_SAFE : S0R_SLOT;
    if (t->init(b) != 0) {
        b->lslot[c].flags &= (u8)~S0SF_BOOTED;
        b->slot = 0xFF;
        skip();
        return;
    }
    s0_active = t;
    s0_state = S_RUN;
}

static void boot_step(void)
{
    struct s0_boot *b = &s0_boot;
    u8 st[S0_ST_VIEW];
    u32 i, n;
    int rc;

    switch (s0_state) {
    case S_START:
        b->key_id = key_id();
        rc = escape_check();
        if (rc < 0)
            return;
        if (rc == 1) {
            to_stock(S0R_ESCAPE);
            return;
        }
        if (rc == 2)
            b->flags |= S0F_SAFE;
        s0_state = S_SCAN;
        /* fall through */
    case S_SCAN:
        for (i = 0; i < 2; i++) {
            if (b->lslot[i].flags & S0SF_SCANNED)
                continue;
            if (nor_read(lbase[i], st, sizeof st) != S0_NOR_OK) {
                if (!wait_busy())
                    to_stock(S0R_BUSY);
                return;
            }
            busy_n = 0;
            parse(&b->lslot[i], st);
        }
        ncand = 0;
        for (i = 0; i < 2; i++)
            if ((b->lslot[i].flags & (S0SF_VALID | S0SF_REJECTED | S0SF_EXHAUSTED)) == S0SF_VALID)
                cand[ncand++] = (u8)i;
        /* normal: highest seq first; SAFE: lowest first (the rollback target) */
        if (ncand == 2 && (b->lslot[cand[1]].seq > b->lslot[cand[0]].seq) != !!(b->flags & S0F_SAFE)) {
            cand[0] = 1;
            cand[1] = 0;
        }
        ci = 0;
        s0_state = S_CAND;
        /* fall through */
    case S_CAND:
        if (ci >= ncand) {
            to_stock(ncand ? S0R_FAILED : S0R_NONE);
            return;
        }
        busy_n = 0;
        if (b->lslot[cand[ci]].flags & S0SF_CONFIRMED) {
            s0_state = S_HDR;
            return;
        }
        if (b->flags & S0F_NOMARK) {
            skip();
            return;
        }
        s0_state = S_MARK;
        /* fall through */
    case S_MARK: {
        struct s0_slot *s = &b->lslot[cand[ci]];
        u32 a = lbase[cand[ci]] + S0_ST_ATTEMPT + 2u * s->tries;
        u8 back[2];
        rc = nor_prog_hw(a, 0);
        if (rc == S0_NOR_OK)
            rc = nor_read(a, back, 2) == S0_NOR_OK && back[0] == 0 && back[1] == 0 ? S0_NOR_OK : S0_NOR_FAIL;
        if (rc == S0_NOR_BUSY) {
            if (!wait_busy()) {
                b->flags |= S0F_NOMARK;   /* from now on: confirmed candidates only, without a mark */
                skip();
            }
            return;
        }
        if (rc != S0_NOR_OK) {
            skip();
            return;
        }
        s->tries++;
        s0_state = S_HDR;
        return;
    }
    case S_HDR:
        if (nor_read(lbase[cand[ci]] + S0_IMAGE_OFF, hdr, sizeof hdr) != S0_NOR_OK) {
            if (!wait_busy())
                to_stock(S0R_BUSY);
            return;
        }
        busy_n = 0;
        n = get32(hdr + S0_H_LEN);
        if (get32(hdr + S0_H_MAGIC) != S0_HDR_MAGIC || get16(hdr + S0_H_FORMAT) < 2 ||
            get16(hdr + S0_H_HDRSIZE) != S0_HDR_SIZE || n < sizeof(struct s1_entry) || n > S0_WINDOW_SIZE ||
            get32(hdr + S0_H_LOAD) != S0_WINDOW || (get32(hdr + S0_H_ENTRY) & 3u) ||
            get32(hdr + S0_H_ENTRY) > n - sizeof(struct s1_entry) || get32(hdr + S0_H_STOCK) != S0_STOCK_TAG ||
            get32(hdr + S0_H_ABI) != S0_ABI || hdr[S0_H_TYPE] != S0_TYPE_LOADER || sig_begin(hdr) != S0_SIG_OK) {
            skip();
            return;
        }
        crc = 0xFFFFFFFFu;
        pos = 0;
        s0_state = S_LOAD;
        return;
    case S_LOAD:
        n = get32(hdr + S0_H_LEN) - pos;
        if (n > STEP)
            n = STEP;
        if (nor_read(lbase[cand[ci]] + S0_IMAGE_OFF + S0_HDR_SIZE + pos, (u8 *)S0_WINDOW + pos, n) != S0_NOR_OK) {
            if (!wait_busy())
                to_stock(S0R_BUSY);
            return;
        }
        busy_n = 0;
        crc = crc32_step(crc, (const u8 *)S0_WINDOW + pos, n);
        sha_update(&mac, (const u8 *)S0_WINDOW + pos, n);
        pos += n;
        if (pos >= get32(hdr + S0_H_LEN))
            finish();
        return;
    default:
        return;
    }
}

/* engine loop pass (s0_tick) */
void s0_step(void)
{
    struct s0_boot *b = &s0_boot;
    const struct s1_entry *e;
    if (s0_state == S_RUN) {
        e = s0_active;
        if (b->request == S0_REQ_RESELECT && e->release() == 0) {
            stk_dis_dsp();
            if (!s0_in_sysex) {
                s0_active = 0;           /* no SysEx enters the old stage-1 from now on */
                stk_ena_dsp();
                b->request = S0_REQ_NONE;
                b->lslot[0].flags = 0;
                b->lslot[1].flags = 0;
                b->slot = 0xFF;
                b->reason = S0R_RESELECT;
                b->flags = (u8)((b->flags & ~S0F_NOMARK) | S0F_RESELECTED);
                b->nor_gates = 0;        /* the next stage-1 decides afresh (no stage-1 code runs now) */
                b->nor_gates_main = 0;
                if (b->reselects != 0xFF)
                    b->reselects++;
                busy_n = 0;
                s0_state = S_SCAN;
                return;
            }
            stk_ena_dsp();
        }
        e->tick();
        return;
    }
    boot_step();
}

/* 0x7D SysEx (task 19): mark the call in flight BEFORE reading the dispatch pointer (task 24 drops it under dis_dsp
 * only while no call is in flight, so either it sees us or we see 0) */
__attribute__((target("arm"))) void s0_sysex(u32 port, const u8 *buf, u32 len, u32 *frame)
{
    const struct s1_entry *e;
    s0_in_sysex = 1;
    e = s0_active;
    if (e)
        e->sysex(port, buf, len, frame);
    s0_in_sysex = 0;
}
