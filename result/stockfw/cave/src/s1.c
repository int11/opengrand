/* OpenGrand OTA loader v4 - STAGE-1, the Bluetooth-updatable resident (image ota_stage1, loader slots A/B).
 *
 * Loaded by stage-0 (src/s0.c, frozen; interface include/s0.h) into the window 0x40176000 after CRC + signature,
 * entered through the entry table below.  Everything ota_loader_v3's resident did except the escape keys and the
 * built-in cave lives here (contract REMOTE_PROTOCOL.md 10.3 + 10.12):
 *   - main boot selection (main slots A/B, highest seq, 3 unconfirmed tries, confirm after ~60 s, CRC + HMAC every
 *     boot, copy to OTA_RUN_BASE, cache sync) - not in SAFE mode (stage-0 escape E2);
 *   - "no main" mode: the main dropped / none bootable -> a minimal responder answers Identify (feat1 OTA bit);
 *   - the OTA ops 0x0E-0x16 for BOTH slot kinds: upload (verify in RAM BEFORE erase; loaders with stage-0's own
 *     rules and signature check), reject, loader confirm (explicit, authenticated - no time-based confirm for a
 *     loader), apply now, RESELECT (hand the loader choice back to stage-0), interlocks that never erase or
 *     reject the last confirmed loader;
 *   - the runtime hook service (17-site allow-list, struct ota_svc version 3 = + stage-0's NOR service for the main);
 *   - fast upload (image version 2, ota.h "fast upload"): Begin's reply offers v2 data frames (CRC-32 each, up to
 *     OTA_D2_MAX bytes, optional LZ tokens decoded straight into the staging area, every frame answered with the
 *     cumulative `received`) next to the old 200 B frames; still RAM only until Commit.
 * NOR writes ONLY through stage-0's guarded service (boot->svc); no stock NOR writer is linked here (s1.ld has no
 * stock_norw.ld - a reference does not link).  Never pokes the stage-0 hook sites 0x40143AE0 / 0x40147E1C.
 * Stage-0 scopes that service by caller: called from stage-1 code (this window) it reaches the slot sectors + kvstore;
 * the main gets the same table (hk_svc.nor) but its calls come from main code, so stage-0 lets it write the kvstore
 * ONLY.  Hence every slot write here is a direct (tail) call from stage-1 code - never a function pointer handed to
 * the main, never a wrapper called by it.
 */
#include "stock.h"
#include "ota.h"
#include "s0.h"

#define TAG0 0x43
#define TAG1 0x41
#define TAG2 0x37
#define TAG3 0x39

#define S1_API          1u         /* stage-1 API (status 88; main header 0x2A must be <= it) */

u32 stk_time_us(void);
int stk_flash_jobs(void);        /* FUN_401342AC: 1 while the deferred writer has an active job */
extern const volatile u8 stk_cur_task;

/* ---- runtime hook word write (privileged single-word store + cache maintenance, see ota_loader_v3) --------- */
void ota_hook_poke(u32 addr, u32 word);
__asm__(
    "    .pushsection .text.ota_hook_poke, \"ax\", %progbits\n"
    "    .arm\n"
    "    .global ota_hook_poke\n"
    "    .type   ota_hook_poke, %function\n"
    "ota_hook_poke:\n"
    "    push    {r0, r1, r4, r5, ip, lr}\n"
    "    mov     r4, r0\n"
    "    mov     r5, r1\n"
    "    ldr     r0, =ota_poke_body\n"
    "    svc     #0\n"
    "    pop     {r0, r1, r4, r5, ip, lr}\n"
    "    bx      lr\n"
    "    .ltorg\n"
    "    .size   ota_hook_poke, . - ota_hook_poke\n"
    "    .global ota_poke_body\n"
    "    .type   ota_poke_body, %function\n"
    "ota_poke_body:\n"                        /* SVC mode, IRQ off; r4 = site, r5 = word */
    "    str     r5, [r4]\n"
    "    mcr     p15, 0, r4, c7, c10, 1\n"    /* clean D-cache line by MVA */
    "    mov     r0, #0\n"
    "    mcr     p15, 0, r0, c7, c10, 4\n"    /* drain write buffer */
    "    mcr     p15, 0, r4, c7, c5, 1\n"     /* invalidate I-cache line by MVA */
    "    ldr     lr, =stk_svc_return_ptr\n"
    "    ldr     lr, [lr]\n"
    "    bx      lr\n"
    "    .ltorg\n"
    "    .size   ota_poke_body, . - ota_poke_body\n"
    "    .popsection\n");

/* ---- tunables --------------------------------------------------------------------------------------------- */
#define STEP            0x1000u
#define PAGES_PER_TICK  8u
#define MAX_TRIES       3u         /* unconfirmed main boots */
#define OTA_CONFIRM_TICKS 7500u    /* ~60 s at 8 ms */
#define BUSY_MAX        3750u      /* ~30 s */
#define BOOT_BUSY_MAX   250u       /* ~2 s at boot -> no main this boot */
#define LDR_MAX         (S0_HDR_SIZE + S0_WINDOW_SIZE)

enum { OE_OK = 0, OE_ARGS = 1, OE_AUTH = 2, OE_BUSY = 3, OE_SLOT = 4, OE_LEN = 5, OE_SEQ = 6,
       OE_OFFSET = 7, OE_STATE = 8, OE_INCOMPLETE = 9, OE_CRC = 10, OE_HEADER = 11, OE_MAC = 12,
       OE_FLASH_BUSY = 13, OE_ERASE = 14, OE_PROGRAM = 15, OE_READBACK = 16, OE_GUARD = 17,
       OE_SEAL = 18, OE_BOOTING = 19, OE_QUIESCE = 20, OE_INSTALL = 21,
       OE_UNCONFIRMED = 22, OE_LAST = 23, OE_SAFE = 24, OE_API = 25 };
enum { J_IDLE = 0, J_RECV, J_COMMIT_Q, J_VERIFY, J_ERASE, J_PROG, J_READBACK, J_SEAL, J_DONE, J_ERROR };
enum { B_START = 0, B_SCAN, B_LOAD, B_VERIFY, B_COPY, B_MARK, B_DONE };
enum { BR_SELECTING = 0, BR_SLOT = 1, BR_EMPTY = 2, BR_ESCAPE = 3, BR_FALLBACK = 4, BR_BUSY = 5, BR_LIVE = 6,
       BR_SAFE = 7, BR_API = 8 };
enum { AP_IDLE = 0, AP_LOAD, AP_VERIFY, AP_QUIESCE, AP_SWITCH, AP_DONE, AP_ERROR };
#define CAPS            0x1F       /* status[48]: as ota_loader_v3 */
#define CAPS2           0x1F       /* status[89]: 1 loader slots, 2 confirm op, 4 info op, 8 svc v3 nor, 0x10 reselect */
enum { ACT_A = 0, ACT_B = 1, ACT_NONE = 2, ACT_SEL = 0x7F };
#define F_VALID     0x01
#define F_CONFIRMED 0x02
#define F_REJECTED  0x04
#define F_BOOTED    0x08
#define F_BAD       0x10
#define F_EXHAUSTED 0x20
#define F_SCANNED   0x40
#define F_LIVEOK    0x80
enum { NR_OK = 0, NR_BUSY = 1, NR_FAIL = 2, NR_GUARD = 3 };   /* = S0_NOR_* */

/* slot index: 0 main A, 1 main B, 2 loader A, 3 loader B */
#define NSLOT 4
#define IS_LDR(i) ((i) >= 2)
static const u32 slot_base[NSLOT] = { S0_SLOT_MAIN_A, S0_SLOT_MAIN_B, S0_SLOT_LDR_A, S0_SLOT_LDR_B };
#define STAGE ((u8 *)OTA_STAGE)
#define VBUF  ((u8 *)OTA_VBUF)

/* ---- state (.data: loaded by stage-0, so fresh at every load) --------------------------------------------- */
typedef struct { u32 h[8]; u32 n; u32 nb; u8 buf[64]; } sha_t;
struct slot_info { u32 seq; u8 flags; u8 tries; u8 stver; };

static const struct s0_boot *boot = 0;
static const struct s0_svc *svc = 0;
static const u8 *okey = 0;                    /* key for mains and auth tags (v4.0: the root key) */
static const struct ota_entry *volatile active = 0;
static volatile u8 active_id = ACT_SEL;
static volatile u8 boot_state = B_START;
static volatile u8 boot_reason = BR_SELECTING;
static struct slot_info slot[NSLOT] = { { 0, 0, 0, 0 }, { 0, 0, 0, 0 }, { 0, 0, 0, 0 }, { 0, 0, 0, 0 } };
static u8 hdrc[NSLOT][0x30] = { { 0 } };     /* each slot's image header 0x00..0x2F (op 0x0E) */
static u32 max_seq = 0, max_lseq = 0;
static u8 cand[2] = { 0, 0 };
static u8 ncand = 0, ci = 0, api_refused = 0;
static u32 boot_busy = 0;

static volatile u8 job = J_IDLE;
static volatile u8 job_err = OE_OK;
static u8 job_slot = 0;                       /* slot index */
static volatile u32 job_rx = 0;
static u32 job_len = 0, job_crc = 0, job_seq = 0;
static u32 job_busy = 0;
static volatile u8 rej_req = 0;               /* Reject-mask bits (rej_bit: mains 0x01/0x02, loaders 0x10/0x20) */
static volatile u8 conf_req = 0;

static u32 v_pos = 0, v_total = 0, v_crc_all = 0, v_crc_pay = 0;
static sha_t v_mac = { { 0 }, 0, 0, { 0 } };
static u32 io_pos = 0;
static u8 seal_step = 0;

static u32 challenge = 0, nonce_ctr = 0, key_id = 0;
volatile u32 ota_alive = 0;
static u16 busy_defers = 0;

volatile u8 ota_gate = 0;
volatile u8 ota_in_sysex = 0;
volatile u8 ota_in_task20 = 0;
static volatile u8 ap_state = AP_IDLE;
static volatile u8 ap_err = OE_OK;
static u8 ap_target = 0;
static u8 ap_count = 0;
static u8 active_live = 0;
static u32 ap_wait = 0;
static u8 ap_tick_old = 0;
static volatile u8 released = 0;              /* RESELECT asked: the main is gone, nothing more to do */

struct hk_site { u32 site, orig; u8 ctx; };
static const struct hk_site hk_sites[] = {
    { 0x40147E94u, 0xEB00090Bu, OTA_HK_TASK19 },   /* bl FUN_4014A2C8 Kawai SysEx (Phase 3-1)      */
    { 0x401539ACu, 0xEBFF5B88u, OTA_HK_TASK19 },   /* bl 0x4012A7D4 MIDI pump loop tail             */
    { 0x40143AB4u, 0xEBFDA539u, OTA_HK_TASK24 },   /* bl FUN_400ACFA0 headphone poll (engine loop)  */
    { 0x40143AB8u, 0xEBFDA49Au, OTA_HK_TASK24 },   /* bl FUN_400ACD28 shutdown executor (engine loop) */
    { 0x400AA874u, 0xEB0005DBu, OTA_HK_KTIMER },   /* bl FUN_400ABFE8 APO / power switch, 20 ms     */
    { 0x40148C4Cu, 0xEBFFFA5Eu, OTA_HK_TASK19 },   /* bl FUN_401475CC MIDI program change -> voice  */
    { 0x40148C60u, 0xEBFFFD6Cu, OTA_HK_TASK19 },   /* bl FUN_40148218 MIDI control change (0x7A ..) */
    { 0x40153998u, 0xEB003FD0u, OTA_HK_TASK19 },   /* bl FUN_401638E0 delayed parameter timers      */
    { 0x401739E4u, 0xEBFFE91Cu, OTA_HK_TASK20 },   /* bl FUN_4016DE5C song/rhythm/metronome/rec pass */
    { 0x401739E8u, 0xEBFEDB79u, OTA_HK_TASK20 },   /* bl 0x4012A7D4 task-20 loop tail               */
    { 0x4016DE80u, 0xEBFFBFE1u, OTA_HK_TASK20 },   /* bl FUN_4015DE0C Music queue                   */
    { 0x4016DE94u, 0xEB000749u, OTA_HK_TASK20 },   /* bl FUN_4016FBC0 metronome                     */
    { 0x40143AC4u, 0xEBFF8EB2u, OTA_HK_TASK24 },   /* bl FUN_40127594 USB memory mount/unmount exec */
    { 0x40143AC8u, 0xEBFF8E62u, OTA_HK_TASK24 },   /* bl FUN_40127458 media state Result            */
    { 0x40143ACCu, 0xEB002E24u, OTA_HK_TASK24 },   /* bl FUN_4014F364 fn 0x24 file-op executor      */
    { 0x40143AD8u, 0xEBFF80D5u, OTA_HK_TASK24 },   /* bl FUN_40123E34 audio play/rec transport      */
    { 0x40143ADCu, 0xEBFF8161u, OTA_HK_TASK24 },   /* bl FUN_40124068 mode-3 value watch / notify   */
};
#define HK_N (sizeof hk_sites / sizeof hk_sites[0])
static u32 hk_word[HK_N] = { 0 };
static u8 hk_n = 0;
static u8 hk_last = 0;
static u16 hk_refused = 0, hk_sets = 0, hk_clears = 0, hk_anomaly = 0;
volatile u8 ota_hk_open = 0;
static u8 hk_task = 0;
static u8 attach_pending = 0;
static u32 run_len = 0;

/* ---- small helpers ---------------------------------------------------------------------------------------- */
static u32 get32(const u8 *p) { return p[0] | (u32)p[1] << 8 | (u32)p[2] << 16 | (u32)p[3] << 24; }
static u32 get16(const u8 *p) { return p[0] | (u32)p[1] << 8; }
static void put32(u8 *p, u32 v) { p[0] = (u8)v; p[1] = (u8)(v >> 8); p[2] = (u8)(v >> 16); p[3] = (u8)(v >> 24); }
static void put16(u8 *p, u32 v) { p[0] = (u8)v; p[1] = (u8)(v >> 8); }

static u32 crc32_step(u32 crc, const u8 *p, u32 n)
{
    static const u32 tbl[16] = {
        0x00000000u, 0x1DB71064u, 0x3B6E20C8u, 0x26D930ACu, 0x76DC4190u, 0x6B6B51F4u,
        0x4DB26158u, 0x5005713Cu, 0xEDB88320u, 0xF00F9344u, 0xD6D6A3E8u, 0xCB61B38Cu,
        0x9B64C2B0u, 0x86D3D2D4u, 0xA00AE278u, 0xBDBDF21Cu,
    };
    while (n--) {
        crc ^= *p++;
        crc = (crc >> 4) ^ tbl[crc & 0x0F];
        crc = (crc >> 4) ^ tbl[crc & 0x0F];
    }
    return crc;
}

/* ---- SHA-256 / HMAC-SHA256 -------------------------------------------------------------------------------- */
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
    u32 w[64], a, b, c, d, e, f, g, k, i, t1, t2;
    for (i = 0; i < 16; i++)
        w[i] = (u32)p[4 * i] << 24 | (u32)p[4 * i + 1] << 16 | (u32)p[4 * i + 2] << 8 | p[4 * i + 3];
    for (; i < 64; i++) {
        u32 s0 = ror(w[i - 15], 7) ^ ror(w[i - 15], 18) ^ (w[i - 15] >> 3);
        u32 s1 = ror(w[i - 2], 17) ^ ror(w[i - 2], 19) ^ (w[i - 2] >> 10);
        w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }
    a = h[0]; b = h[1]; c = h[2]; d = h[3]; e = h[4]; f = h[5]; g = h[6]; k = h[7];
    for (i = 0; i < 64; i++) {
        t1 = k + (ror(e, 6) ^ ror(e, 11) ^ ror(e, 25)) + ((e & f) ^ (~e & g)) + K256[i] + w[i];
        t2 = (ror(a, 2) ^ ror(a, 13) ^ ror(a, 22)) + ((a & b) ^ (a & c) ^ (b & c));
        k = g; g = f; f = e; e = d + t1; d = c; c = b; b = a; a = t1 + t2;
    }
    h[0] += a; h[1] += b; h[2] += c; h[3] += d; h[4] += e; h[5] += f; h[6] += g; h[7] += k;
}

static void sha_init(sha_t *s)
{
    static const u32 iv[8] = { 0x6a09e667u, 0xbb67ae85u, 0x3c6ef372u, 0xa54ff53au,
                               0x510e527fu, 0x9b05688cu, 0x1f83d9abu, 0x5be0cd19u };
    u32 i;
    for (i = 0; i < 8; i++)
        s->h[i] = iv[i];
    s->n = 0;
    s->nb = 0;
}

static void sha_update(sha_t *s, const u8 *p, u32 n)
{
    s->n += n;
    while (n) {
        if (s->nb == 0 && n >= 64) {
            sha_block(s->h, p);
            p += 64;
            n -= 64;
            continue;
        }
        s->buf[s->nb++] = *p++;
        n--;
        if (s->nb == 64) {
            sha_block(s->h, s->buf);
            s->nb = 0;
        }
    }
}

static void sha_final(sha_t *s, u8 out[32])
{
    u32 bits_lo = s->n << 3, bits_hi = s->n >> 29, i;
    s->buf[s->nb++] = 0x80;
    if (s->nb > 56) {
        while (s->nb < 64)
            s->buf[s->nb++] = 0;
        sha_block(s->h, s->buf);
        s->nb = 0;
    }
    while (s->nb < 56)
        s->buf[s->nb++] = 0;
    for (i = 0; i < 4; i++) {
        s->buf[56 + i] = (u8)(bits_hi >> (24 - 8 * i));
        s->buf[60 + i] = (u8)(bits_lo >> (24 - 8 * i));
    }
    sha_block(s->h, s->buf);
    for (i = 0; i < 32; i++)
        out[i] = (u8)(s->h[i >> 2] >> (24 - 8 * (i & 3)));
}

static void hmac_init(sha_t *s)
{
    u8 pad[64];
    u32 i;
    for (i = 0; i < 64; i++)
        pad[i] = (u8)((i < 32 ? okey[i] : 0) ^ 0x36);
    sha_init(s);
    sha_update(s, pad, 64);
}

static void hmac_final(sha_t *s, u8 out[32])
{
    u8 pad[64], inner[32];
    u32 i;
    sha_final(s, inner);
    for (i = 0; i < 64; i++)
        pad[i] = (u8)((i < 32 ? okey[i] : 0) ^ 0x5C);
    sha_init(s);
    sha_update(s, pad, 64);
    sha_update(s, inner, 32);
    sha_final(s, out);
}

static u32 get_key_id(void)
{
    if (!key_id) {
        sha_t s;
        u8 out[32];
        hmac_init(&s);
        sha_update(&s, (const u8 *)"C79 key id", 10);
        hmac_final(&s, out);
        key_id = get32(out);
    }
    return key_id;
}

/* ---- NOR: reads with the stock copier, writes ONLY through stage-0's guarded service ---------------------- */
static int nor_read(u32 src, u8 *dst, u32 len)
{
    int busy;
    stk_dis_dsp();
    busy = stk_flash_busy() || stk_flash_jobs();
    if (!busy)
        stk_flash_copy(src, dst, len);
    stk_ena_dsp();
    return busy ? NR_BUSY : NR_OK;
}

static int nor_erase(u32 a) { return svc->nor_erase(a); }
static int nor_program(u32 a, const u8 *src) { return svc->nor_prog(a, src, OTA_PAGE); }
static int nor_program_hw(u32 a, u32 hw) { return svc->nor_prog_hw(a, hw); }

/* ---- image checks ----------------------------------------------------------------------------------------- */
/* a main image (format 1, or format 2 type 1): 0 ok, OE_HEADER, OE_API */
static int main_hdr(const u8 *h, u32 total)
{
    u32 len = get32(h + OTA_H_LEN), fmt = get16(h + OTA_H_FORMAT);
    if (get32(h + OTA_H_MAGIC) != OTA_HDR_MAGIC || get16(h + OTA_H_HDRSIZE) != OTA_HDR_SIZE ||
        len + OTA_HDR_SIZE != total || len < sizeof(struct ota_entry) || len > OTA_PAYLOAD_MAX ||
        get32(h + OTA_H_LOAD) != OTA_RUN_BASE || get32(h + OTA_H_ENTRY) != 0 ||
        get32(h + OTA_H_STOCK) != OTA_STOCK_TAG || get32(h + OTA_H_KEYID) != get_key_id())
        return OE_HEADER;
    if (fmt == 1)
        return get32(h + OTA_H_ABI) == OTA_ABI ? OE_OK : OE_API;
    if (fmt < 2 || h[S0_H_TYPE] != S0_TYPE_MAIN || h[S0_H_ALG] != S0_ALG_HMAC)
        return OE_HEADER;
    return get32(h + OTA_H_ABI) == OTA_ABI && h[S0_H_MINAPI] <= S1_API ? OE_OK : OE_API;
}

/* a loader image: exactly stage-0's rules (src/s0.c S_HDR); the signature by stage-0's own check later */
static int ldr_hdr(const u8 *h, u32 total)
{
    u32 n = get32(h + S0_H_LEN), e = get32(h + S0_H_ENTRY);
    return get32(h + S0_H_MAGIC) == S0_HDR_MAGIC && get16(h + S0_H_FORMAT) >= 2 &&
           get16(h + S0_H_HDRSIZE) == S0_HDR_SIZE && n + S0_HDR_SIZE == total && n >= sizeof(struct s1_entry) &&
           n <= S0_WINDOW_SIZE && get32(h + S0_H_LOAD) == S0_WINDOW && !(e & 3u) &&
           e <= n - sizeof(struct s1_entry) && get32(h + S0_H_STOCK) == S0_STOCK_TAG &&
           get32(h + S0_H_ABI) == S0_ABI && h[S0_H_TYPE] == S0_TYPE_LOADER && h[S0_H_ALG] == S0_ALG_HMAC &&
           get32(h + S0_H_KEYID) == boot->key_id ? OE_OK : OE_HEADER;
}

/* verifier over STAGE (boot / apply: mains; commit: either kind) */
static u8 v_ldr = 0;
static int v_start(u32 total, int ldr)
{
    int rc;
    if (total < OTA_HDR_SIZE || total > (ldr ? LDR_MAX : OTA_IMAGE_MAX))
        return OE_HEADER;
    rc = ldr ? ldr_hdr(STAGE, total) : main_hdr(STAGE, total);
    if (rc != OE_OK)
        return rc;
    v_ldr = (u8)ldr;
    v_total = total;
    v_pos = OTA_HDR_SIZE;
    v_crc_all = crc32_step(0xFFFFFFFFu, STAGE, OTA_HDR_SIZE);
    v_crc_pay = 0xFFFFFFFFu;
    if (!ldr) {
        hmac_init(&v_mac);
        sha_update(&v_mac, STAGE, OTA_H_MAC);
    }
    return OE_OK;
}

static int v_step(void)
{
    u32 n = v_total - v_pos;
    if (n > STEP)
        n = STEP;
    v_crc_all = crc32_step(v_crc_all, STAGE + v_pos, n);
    v_crc_pay = crc32_step(v_crc_pay, STAGE + v_pos, n);
    if (!v_ldr)
        sha_update(&v_mac, STAGE + v_pos, n);
    v_pos += n;
    return v_pos < v_total;
}

static int v_finish(void)
{
    u8 mac[32];
    u32 i, diff = 0;
    if ((v_crc_pay ^ 0xFFFFFFFFu) != get32(STAGE + OTA_H_CRC))
        return OE_CRC;
    if (v_ldr)
        return svc->sig_check(STAGE, STAGE + S0_HDR_SIZE, v_total - S0_HDR_SIZE) == S0_SIG_OK ? OE_OK : OE_MAC;
    hmac_final(&v_mac, mac);
    for (i = 0; i < OTA_MAC_LEN; i++)
        diff |= mac[i] ^ STAGE[OTA_H_MAC + i];
    return diff ? OE_MAC : OE_OK;
}

/* ---- slot states ------------------------------------------------------------------------------------------- */
static void parse_state(u32 i, const u8 *st)
{
    struct slot_info *s = &slot[i];
    u32 k, used = 0, magic = IS_LDR(i) ? S0_LDR_MAGIC : S0_MAIN_MAGIC;
    s->flags = F_SCANNED;
    s->seq = get32(st + S0_ST_SEQ);
    s->stver = get16(st + S0_ST_VERSION) == 0xFFFF ? 0 : (u8)get16(st + S0_ST_VERSION);
    if (get32(st + S0_ST_MAGIC) == magic && s->seq != 0xFFFFFFFFu)
        s->flags |= F_VALID;
    if (get16(st + S0_ST_CONFIRM) != 0xFFFF)
        s->flags |= F_CONFIRMED;
    if (get16(st + S0_ST_REJECT) != 0xFFFF)
        s->flags |= F_REJECTED;
    if (!IS_LDR(i) && get16(st + S0_ST_LIVE) != 0xFFFF)
        s->flags |= F_LIVEOK;
    for (k = 0; k < S0_ATTEMPTS; k++)
        if (get16(st + S0_ST_ATTEMPT + 2 * k) != 0xFFFF)
            used = k + 1;
    s->tries = (u8)used;
    if (!(s->flags & F_CONFIRMED) && used >= (IS_LDR(i) ? S0_MAX_TRIES : MAX_TRIES))
        s->flags |= F_EXHAUSTED;
    if (IS_LDR(i)) {    /* what stage-0 knows of this boot */
        u8 f = boot->lslot[i - 2].flags;
        s->flags |= f & (S0SF_BOOTED | S0SF_BAD);
    }
    if (s->flags & F_VALID) {
        if (IS_LDR(i) ? s->seq > max_lseq : s->seq > max_seq) {
            if (IS_LDR(i))
                max_lseq = s->seq;
            else
                max_seq = s->seq;
        }
    }
}

static int bootable(u32 i)
{
    return (slot[i].flags & (F_VALID | F_REJECTED | F_EXHAUSTED | F_BAD)) == F_VALID;
}

static int ldr_ok(u32 i)        /* a loader stage-0 would boot as a confirmed fallback */
{
    return (slot[i].flags & (F_VALID | F_CONFIRMED | F_REJECTED)) == (F_VALID | F_CONFIRMED);
}

/* ---- shared by boot and apply: NOR -> staging, install ------------------------------------------------------ */
enum { LS_MORE = 0, LS_DONE, LS_FAIL, LS_BUSY };

static int load_step(u32 s)
{
    u32 base = slot_base[s] + OTA_IMAGE_OFF, n = io_pos ? v_total - io_pos : STEP;
    int rc;
    if (n > STEP)
        n = STEP;
    if (nor_read(base + io_pos, STAGE + io_pos, n) == NR_BUSY)
        return LS_BUSY;
    if (io_pos == 0) {
        u32 len = get32(STAGE + OTA_H_LEN);
        if (len > OTA_PAYLOAD_MAX)
            return LS_FAIL;
        v_total = OTA_HDR_SIZE + len;
    }
    io_pos += n;
    if (io_pos < v_total)
        return LS_MORE;
    rc = v_start(v_total, 0);
    if (rc == OE_API)
        api_refused = 1;
    return rc == OE_OK ? LS_DONE : LS_FAIL;
}

static int in_run(u32 a, u32 len, u32 align)
{
    return a >= OTA_RUN_BASE && a < OTA_RUN_BASE + len && !(a & (align - 1));
}

static int install(void)
{
    u32 len = v_total - OTA_HDR_SIZE, w;
    const struct ota_entry *e = (const struct ota_entry *)OTA_RUN_BASE;
    u32 *dst = (u32 *)OTA_RUN_BASE;
    const u8 *src = STAGE + OTA_HDR_SIZE;
    int bad;
    run_len = 0;
    for (w = 0; w < (len + 3) / 4; w++)
        dst[w] = get32(src + 4 * w);
    svc->cache_sync(OTA_RUN_BASE, len);
    bad = (crc32_step(0xFFFFFFFFu, (const u8 *)OTA_RUN_BASE, len) ^ 0xFFFFFFFFu) != get32(STAGE + OTA_H_CRC) ||
           e->magic != OTA_ENTRY_MAGIC || e->abi != OTA_ABI ||
           !in_run((u32)e->sysex, len, 4) || !in_run((u32)e->tick, len, 4) ||
           (e->quiesce && !in_run((u32)e->quiesce, len, 4)) ||
           ((e->feat & OTA_EF_ATTACH) && (len < sizeof(struct ota_entry_x) ||
                                          !in_run((u32)((const struct ota_entry_x *)e)->attach, len, 4)));
    if (!bad)
        run_len = len;
    return bad;
}

/* ---- runtime hooks (as ota_loader_v3) ------------------------------------------------------------------------ */
static int hk_find(u32 site)
{
    u32 i;
    for (i = 0; i < HK_N; i++)
        if (hk_sites[i].site == site)
            return (int)i;
    return -1;
}

static int hk_refuse(int rc)
{
    hk_last = (u8)rc;
    if (hk_refused != 0xFFFF)
        hk_refused++;
    return rc;
}

static int hk_ctx_ok(void)
{
    return ota_hk_open && stk_cur_task == hk_task && active_id <= ACT_B && run_len != 0;
}

__attribute__((noinline)) int ota_hook_set(u32 site, u32 orig, u32 target, u32 flags)
{
    u32 off, word;
    int i;
    if (!hk_ctx_ok())
        return hk_refuse(OTA_HK_E_CTX);
    if (ota_gate)
        return hk_refuse(OTA_HK_E_BUSY);
    i = hk_find(site);
    if (i < 0)
        return hk_refuse(OTA_HK_E_SITE);
    if (orig != hk_sites[i].orig)
        return hk_refuse(OTA_HK_E_ORIG);
    if ((flags & ~OTA_HKF_NOBLOCK) || (hk_sites[i].ctx == OTA_HK_KTIMER && !(flags & OTA_HKF_NOBLOCK)))
        return hk_refuse(OTA_HK_E_FLAGS);
    if (hk_word[i])
        return hk_refuse(OTA_HK_E_STATE);
    if (!in_run(target, run_len, 4))
        return hk_refuse(OTA_HK_E_TARGET);
    if (*(const volatile u32 *)site != orig) {
        hk_anomaly++;
        return hk_refuse(OTA_HK_E_ORIG);
    }
    off = (target - site - 8) >> 2;
    word = (orig & 0xFF000000u) | (off & 0x00FFFFFFu);
    ota_hook_poke(site, word);
    if (*(const volatile u32 *)site != word) {
        ota_hook_poke(site, orig);
        hk_anomaly++;
        return hk_refuse(OTA_HK_E_VERIFY);
    }
    hk_word[i] = word;
    hk_n++;
    hk_sets++;
    return OTA_HK_OK;
}

__attribute__((noinline)) int ota_hook_clear(u32 site)
{
    int i;
    if (!hk_ctx_ok())
        return hk_refuse(OTA_HK_E_CTX);
    i = hk_find(site);
    if (i < 0)
        return hk_refuse(OTA_HK_E_SITE);
    if (!hk_word[i])
        return hk_refuse(OTA_HK_E_STATE);
    if (*(const volatile u32 *)site != hk_word[i])
        hk_anomaly++;
    ota_hook_poke(site, hk_sites[i].orig);
    hk_word[i] = 0;
    hk_n--;
    hk_clears++;
    return OTA_HK_OK;
}

static void hooks_remove_all(void)
{
    u32 i;
    for (i = 0; i < HK_N; i++)
        if (hk_word[i]) {
            if (*(const volatile u32 *)hk_sites[i].site != hk_word[i])
                hk_anomaly++;
            ota_hook_poke(hk_sites[i].site, hk_sites[i].orig);
            hk_word[i] = 0;
            hk_clears++;
        }
    hk_n = 0;
}

static struct ota_svc hk_svc = {
    OTA_SVC_MAGIC, OTA_SVC_VERSION, ota_hook_set, ota_hook_clear, &ota_in_sysex, &ota_in_task20, 0
};

/* ---- main boot selection ------------------------------------------------------------------------------------ */
static void activate(const struct ota_entry *e, u8 id, u8 reason)
{
    boot_reason = reason;
    active_id = id;
    ota_alive = 0;
    active_live = reason == BR_LIVE;
    attach_pending = e && (e->feat & OTA_EF_ATTACH);
    active = e;
    boot_state = B_DONE;
}

static void no_main(u8 reason)
{
    activate(0, ACT_NONE, reason);
}

static void next_candidate(void)
{
    if (ci < ncand) {
        io_pos = 0;
        boot_busy = 0;
        boot_state = B_LOAD;
    } else {
        no_main(!ncand ? BR_EMPTY : api_refused ? BR_API : BR_FALLBACK);
    }
}

static void fail_candidate(void)
{
    slot[cand[ci]].flags |= F_BAD;
    ci++;
    next_candidate();
}

static int boot_wait(int rc)
{
    if (rc != NR_BUSY)
        return 0;
    busy_defers++;
    if (++boot_busy > BOOT_BUSY_MAX)
        no_main(BR_BUSY);
    return 1;
}

/* read state 0x30 + header 0x30 of one slot into slot[] / hdrc[]: 1 = busy */
static int scan_slot(u32 i)
{
    u8 st[0x30];
    if (nor_read(slot_base[i], st, sizeof st) == NR_BUSY ||
        nor_read(slot_base[i] + OTA_IMAGE_OFF, hdrc[i], sizeof hdrc[i]) == NR_BUSY)
        return 1;
    parse_state(i, st);
    return 0;
}

static void boot_step(void)
{
    u32 i;
    int rc;
    switch (boot_state) {
    case B_START:
        get_key_id();
        boot_state = B_SCAN;
        /* fall through */
    case B_SCAN:
        for (i = 0; i < NSLOT; i++) {
            if (slot[i].flags & F_SCANNED)
                continue;
            if (boot_wait(scan_slot(i) ? NR_BUSY : NR_OK))
                return;
        }
        if (boot->flags & S0F_SAFE) {
            no_main(BR_SAFE);     /* the stage-0 escape E2: never start a main in this boot */
            return;
        }
        ncand = 0;
        for (i = 0; i < 2; i++)
            if (bootable(i))
                cand[ncand++] = (u8)i;
        if (ncand == 2 && slot[cand[1]].seq > slot[cand[0]].seq) {
            cand[0] = 1;
            cand[1] = 0;
        }
        ci = 0;
        next_candidate();
        return;
    case B_LOAD:
        rc = load_step(cand[ci]);
        if (rc == LS_BUSY) {
            boot_wait(NR_BUSY);
            return;
        }
        if (rc == LS_FAIL)
            fail_candidate();
        else if (rc == LS_DONE)
            boot_state = B_VERIFY;
        return;
    case B_VERIFY:
        if (v_step())
            return;
        if (v_finish() != OE_OK) {
            fail_candidate();
            return;
        }
        boot_state = B_COPY;
        return;
    case B_COPY:
        if (install()) {
            fail_candidate();
            return;
        }
        boot_busy = 0;
        boot_state = B_MARK;
        /* fall through */
    case B_MARK: {
        struct slot_info *si = &slot[cand[ci]];
        if (!(si->flags & F_CONFIRMED)) {
            rc = nor_program_hw(slot_base[cand[ci]] + S0_ST_ATTEMPT + 2u * si->tries, 0);
            if (boot_wait(rc))
                return;
            if (rc != NR_OK) {
                fail_candidate();
                return;
            }
            si->tries++;
        }
        si->flags |= F_BOOTED;
        activate((const struct ota_entry *)OTA_RUN_BASE, cand[ci], BR_SLOT);
        return;
    }
    default:
        return;
    }
}

/* ---- apply now ---------------------------------------------------------------------------------------------- */
static void ap_fail(u8 e)
{
    ota_gate = 0;
    ap_err = e;
    ap_state = AP_ERROR;
}

static void apply_step(void)
{
    const struct ota_entry *a = active;
    int rc;
    switch (ap_state) {
    case AP_LOAD:
        rc = load_step(ap_target);
        if (rc == LS_BUSY) {
            busy_defers++;
            if (++ap_wait > BUSY_MAX)
                ap_fail(OE_FLASH_BUSY);
            return;
        }
        ap_wait = 0;
        if (rc == LS_FAIL)
            ap_fail(OE_HEADER);
        else if (rc == LS_DONE)
            ap_state = AP_VERIFY;
        return;
    case AP_VERIFY:
        if (v_step())
            return;
        rc = v_finish();
        if (rc != OE_OK) {
            slot[ap_target].flags |= F_BAD;
            ap_fail((u8)rc);
            return;
        }
        ap_wait = 0;
        ota_gate = 1;
        ap_state = AP_QUIESCE;
        return;
    case AP_QUIESCE:
        ap_tick_old = 0;
        if (a && a->quiesce && a->quiesce() != 0) {
            if (++ap_wait > OTA_QUIESCE_TICKS) {
                ap_fail(OE_QUIESCE);
                return;
            }
            ap_tick_old = 1;
            return;
        }
        stk_dis_dsp();
        if (ota_in_sysex || ota_in_task20) {
            stk_ena_dsp();
            if (++ap_wait > OTA_QUIESCE_TICKS)
                ap_fail(OE_QUIESCE);
            return;
        }
        hooks_remove_all();
        active = 0;
        run_len = 0;
        stk_ena_dsp();
        ap_state = AP_SWITCH;
        /* fall through */
    case AP_SWITCH:
        ap_count++;
        if (ap_target == OTA_TGT_BUILTIN || ap_target == OTA_TGT_RESELECT) {
            no_main(BR_LIVE);
        } else if (install()) {
            no_main(BR_LIVE);
            ota_gate = 0;
            ap_err = OE_INSTALL;
            ap_state = AP_ERROR;
            return;
        } else {
            slot[ap_target].flags &= (u8)~F_BAD;
            activate((const struct ota_entry *)OTA_RUN_BASE, ap_target, BR_LIVE);
        }
        ota_gate = 0;
        ap_err = OE_OK;
        ap_state = AP_DONE;
        if (ap_target == OTA_TGT_RESELECT) {
            released = 1;
            ((struct s0_boot *)boot)->request = S0_REQ_RESELECT;   /* the one field stage-1 writes */
        }
        return;
    default:
        return;
    }
}

static int apply_busy(void)
{
    u8 s = ap_state;
    return s >= AP_LOAD && s <= AP_SWITCH;
}

/* ---- upload / reject / confirm jobs ------------------------------------------------------------------------- */
static void job_fail(u8 e)
{
    job_err = e;
    job = J_ERROR;
}

static int job_wait(int rc)
{
    if (rc != NR_BUSY) {
        job_busy = 0;
        return 0;
    }
    busy_defers++;
    if (++job_busy > BUSY_MAX)
        job_fail(OE_FLASH_BUSY);
    return 1;
}

static u8 rej_bit(u32 i) { return (u8)(i < 2 ? 1u << i : 0x10u << (i - 2)); }

static void do_reject(void)
{
    u32 i;
    for (i = 0; i < NSLOT; i++) {
        int rc;
        if (!(rej_req & rej_bit(i)))
            continue;
        if (slot[i].flags & F_REJECTED) {
            rej_req &= (u8)~rej_bit(i);
            continue;
        }
        rc = nor_program_hw(slot_base[i] + S0_ST_REJECT, 0);
        if (rc == NR_BUSY) {
            busy_defers++;
            return;
        }
        if (rc == NR_OK)
            slot[i].flags |= F_REJECTED;
        rej_req &= (u8)~rej_bit(i);
    }
}

static void do_conf_ldr(void)
{
    u32 i = 2u + boot->slot;
    int rc;
    if (boot->slot > 1 || (slot[i].flags & F_CONFIRMED)) {
        conf_req = 0;
        return;
    }
    rc = nor_program_hw(slot_base[i] + S0_ST_CONFIRM, 0);
    if (rc == NR_BUSY) {
        busy_defers++;
        return;
    }
    if (rc == NR_OK)
        slot[i].flags |= F_CONFIRMED;
    conf_req = 0;
}

static void do_confirm(void)
{
    u8 id = active_id, flag = active_live ? F_LIVEOK : F_CONFIRMED;
    if (id > ACT_B || (slot[id].flags & flag) || ota_alive < OTA_CONFIRM_TICKS)
        return;
    switch (nor_program_hw(slot_base[id] + (active_live ? S0_ST_LIVE : S0_ST_CONFIRM), 0)) {
    case NR_BUSY: busy_defers++; return;
    default: slot[id].flags |= flag; return;
    }
}

static void job_step(void)
{
    u32 base = slot_base[job_slot], i, pad;
    int rc;

    switch (job) {
    case J_COMMIT_Q:
        pad = (job_len + OTA_PAGE - 1) & ~(OTA_PAGE - 1);
        for (i = job_len; i < pad; i++)
            STAGE[i] = 0xFF;
        rc = v_start(job_len, IS_LDR(job_slot));
        if (rc != OE_OK) {
            job_fail((u8)rc);
            return;
        }
        job_busy = 0;
        job = J_VERIFY;
        return;
    case J_VERIFY:
        if (v_step())
            return;
        if ((v_crc_all ^ 0xFFFFFFFFu) != job_crc) {
            job_fail(OE_CRC);
            return;
        }
        rc = v_finish();
        if (rc != OE_OK) {
            job_fail((u8)rc);
            return;
        }
        job = J_ERASE;   /* everything checked in RAM: only now does the flash get touched */
        return;
    case J_ERASE:
        rc = nor_erase(base);
        if (job_wait(rc))
            return;
        if (rc != NR_OK) {
            job_fail(rc == NR_GUARD ? OE_GUARD : OE_ERASE);
            return;
        }
        slot[job_slot].flags = F_SCANNED;
        slot[job_slot].tries = 0;
        for (i = 0; i < sizeof hdrc[0]; i++)
            hdrc[job_slot][i] = 0xFF;
        io_pos = 0;
        job = J_PROG;
        return;
    case J_PROG:
        pad = (job_len + OTA_PAGE - 1) & ~(OTA_PAGE - 1);
        for (i = 0; i < PAGES_PER_TICK && io_pos < pad; i++) {
            const u8 *pg = STAGE + io_pos;
            u32 k, blank = 1;
            for (k = 0; k < OTA_PAGE; k++)
                if (pg[k] != 0xFF) { blank = 0; break; }
            if (!blank) {
                rc = nor_program(base + OTA_IMAGE_OFF + io_pos, pg);
                if (job_wait(rc))
                    return;
                if (rc != NR_OK) {
                    job_fail(rc == NR_GUARD ? OE_GUARD : OE_PROGRAM);
                    return;
                }
            }
            io_pos += OTA_PAGE;
        }
        if (io_pos >= pad) {
            io_pos = 0;
            job = J_READBACK;
        }
        return;
    case J_READBACK: {
        u32 n = job_len - io_pos;
        if (n > OTA_VBUF_SIZE)
            n = OTA_VBUF_SIZE;
        rc = nor_read(base + OTA_IMAGE_OFF + io_pos, VBUF, n);
        if (job_wait(rc))
            return;
        for (i = 0; i < n; i++)
            if (VBUF[i] != STAGE[io_pos + i]) {
                job_fail(OE_READBACK);
                return;
            }
        io_pos += n;
        if (io_pos >= job_len) {
            seal_step = 0;
            job = J_SEAL;
        }
        return;
    }
    case J_SEAL:
        /* seq, state version, then the magic last: the magic is the commit point */
        while (seal_step < 5) {
            static const u8 off[5] = { S0_ST_SEQ, S0_ST_SEQ + 2, S0_ST_VERSION, S0_ST_MAGIC, S0_ST_MAGIC + 2 };
            u32 magic = IS_LDR(job_slot) ? S0_LDR_MAGIC : S0_MAIN_MAGIC;
            u32 v = seal_step < 2 ? job_seq : seal_step == 2 ? 1u : magic;
            rc = nor_program_hw(base + off[seal_step], (seal_step == 1 || seal_step == 4) ? v >> 16 : v & 0xFFFF);
            if (job_wait(rc))
                return;
            if (rc != NR_OK) {
                job_fail(rc == NR_GUARD ? OE_GUARD : OE_SEAL);
                return;
            }
            seal_step++;
        }
        rc = scan_slot(job_slot) ? NR_BUSY : NR_OK;
        if (job_wait(rc))
            return;
        if (!(slot[job_slot].flags & F_VALID) || slot[job_slot].seq != job_seq) {
            job_fail(OE_SEAL);
            return;
        }
        job_err = OE_OK;
        job = J_DONE;
        return;
    default:
        return;
    }
}

/* ---- I/O ----------------------------------------------------------------------------------------------------- */
static void o_tx(u32 port, const u8 *buf, u32 len)
{
    switch (port & 3) {
    case 0: stk_tx_port0(buf, len); break;
    case 1: stk_tx_port1(buf, len); break;
    case 2: stk_tx_port2(buf, len); break;
    default: stk_tx_port3(buf, len); break;
    }
}

static u32 pack7(u8 *out, const u8 *in, u32 n)
{
    u32 o = 0;
    while (n) {
        u32 g = n < 7 ? n : 7, i;
        u8 hi = 0;
        for (i = 0; i < g; i++)
            if (in[i] & 0x80)
                hi |= (u8)(1u << i);
        out[o++] = hi;
        for (i = 0; i < g; i++)
            out[o++] = in[i] & 0x7F;
        in += g;
        n -= g;
    }
    return o;
}

static u32 unpack7(u8 *out, u32 max, const u8 *in, u32 n)
{
    u32 o = 0;
    while (n) {
        u32 g = n - 1 < 7 ? n - 1 : 7, i;
        u8 hi = *in++;
        n--;
        if (o + g > max)
            return 0xFFFFFFFFu;
        for (i = 0; i < g; i++)
            out[o++] = (u8)((in[i] & 0x7F) | ((hi >> i) & 1 ? 0x80 : 0));
        in += g;
        n -= g;
    }
    return o;
}

static void reply(u32 port, u8 op, const u8 *raw, u32 n)
{
    u8 msg[8 + 136];
    u32 i = 0;
    msg[i++] = 0xF0; msg[i++] = 0x7D;
    msg[i++] = TAG0; msg[i++] = TAG1; msg[i++] = TAG2; msg[i++] = TAG3;
    msg[i++] = (u8)(op | 0x40);
    i += pack7(msg + i, raw, n);     /* n <= OTA_STATUS_LEN3 (112 -> 128 packed, 7 + 128 + 1 <= 144) */
    msg[i++] = 0xF7;
    o_tx(port, msg, i);
}

static void new_challenge(void)
{
    sha_t s;
    u8 m[12], out[32];
    put32(m, stk_time_us());
    put32(m + 4, ++nonce_ctr);
    put32(m + 8, challenge);
    hmac_init(&s);
    sha_update(&s, (const u8 *)"C79N", 4);
    sha_update(&s, m, sizeof m);
    hmac_final(&s, out);
    challenge = get32(out);
}

static int auth_ok(u8 op, const u8 *args, u32 n, const u8 *tag)
{
    sha_t s;
    u8 hdr[9], out[32];
    u32 i, diff = 0;
    hdr[0] = 'C'; hdr[1] = '7'; hdr[2] = '9'; hdr[3] = 'A'; hdr[4] = op;
    put32(hdr + 5, challenge);
    hmac_init(&s);
    sha_update(&s, hdr, sizeof hdr);
    sha_update(&s, args, n);
    hmac_final(&s, out);
    for (i = 0; i < OTA_TAG_LEN; i++)
        diff |= out[i] ^ tag[i];
    new_challenge();
    return diff == 0 && challenge != 0;
}

static void reply_rc(u32 port, u8 op, u8 rc)
{
    u8 r[9];
    r[0] = rc;
    put32(r + 1, challenge);
    /* Begin also says what upload framing this loader takes (ota.h "fast upload"); older apps read rc only */
    r[5] = OTA_BC_DATA2 | OTA_BC_LZ;
    r[6] = OTA_D2_WINDOW;
    put16(r + 7, OTA_D2_MAX);
    reply(port, op, r, op == OP_OTA_BEGIN ? 9 : 5);
}

static void send_status(u32 port)
{
    u8 r[OTA_STATUS_LEN3];
    u32 i;
    for (i = 0; i < sizeof r; i++)
        r[i] = 0;
    if (!challenge)
        new_challenge();
    r[0] = 3;
    r[1] = job;
    r[2] = job_err;
    r[3] = boot_state == B_DONE;
    r[4] = active_id;
    r[5] = boot_reason;
    r[6] = (u8)(IS_LDR(job_slot) ? OTA_TGT_LDR_A + (job_slot - 2) : job_slot);
    r[7] = (u8)((rej_req & 3) | (rej_req & 0x30));
    put32(r + 8, job_rx);
    put32(r + 12, job_len);
    put32(r + 16, challenge);
    put32(r + 20, get_key_id());
    for (i = 0; i < 4; i++) {
        u8 *q = r + (i < 2 ? 24 + 6 * i : 72 + 6 * (i - 2));
        put32(q, slot[i].seq);
        q[4] = slot[i].flags;
        q[5] = slot[i].tries;
    }
    put16(r + 36, boot->guard_trips);
    put16(r + 38, busy_defers);
    put32(r + 40, OTA_PAYLOAD_MAX + OTA_HDR_SIZE);
    r[44] = OTA_ABI;
    r[45] = OTA_CHUNK;
    put16(r + 46, max_seq & 0xFFFF);
    r[48] = CAPS;
    r[49] = ap_state;
    r[50] = ap_err;
    r[51] = ap_target;
    r[52] = active_live;
    r[53] = ap_count;
    put32(r + 54, boot->diag_mbox0);
    put32(r + 58, boot->diag_mbox1);
    put32(r + 62, boot->diag_copy);
    r[66] = boot->diag_esc;
    r[67] = boot->diag_keys_down;
    r[68] = boot->diag_keys_low;
    r[69] = boot->diag_esc_wait;
    r[70] = hk_n;
    r[71] = (u8)(hk_refused > 255 ? 255 : hk_refused);
    r[84] = boot->slot;
    r[85] = boot->reason;
    r[86] = boot->flags;
    r[87] = boot->s0_version;
    r[88] = S1_API;
    r[89] = CAPS2;
    if (boot->slot < 2)
        put32(r + 90, get32(hdrc[2 + boot->slot] + S0_H_VERSION));
    put32(r + 94, boot->key_id);
    put32(r + 98, get_key_id());
    r[102] = OTA_ABI;
    r[103] = OTA_ABI;
    put32(r + 104, LDR_MAX);
    put16(r + 108, max_lseq & 0xFFFF);
    r[110] = boot->reselects;
    if (boot->size >= S0_BOOT_SIZE1 + 2)            /* stage-0 v2: the NOR gates (bits 0-2 ON, bits 4-6 main too) */
        r[111] = (u8)((boot->nor_gates & 0x0F) | (boot->nor_gates_main & 0x0F) << 4);
    reply(port, OP_OTA_STATUS, r, sizeof r);
}

static void send_hooks(u32 port, u32 first)
{
    u8 r[OTA_HOOKS_LEN2];
    u32 i, k = 0;
    if (first > HK_N)
        first = HK_N;
    r[0] = 2;
    r[1] = (u8)HK_N;
    r[2] = hk_n;
    r[3] = hk_last;
    put16(r + 4, hk_refused);
    put16(r + 6, hk_sets);
    put16(r + 8, hk_clears);
    put16(r + 10, hk_anomaly);
    r[12] = (u8)first;
    for (i = first; i < HK_N && k < OTA_HOOKS_PAGE; i++, k++) {
        u8 *q = r + 14 + 9 * k;
        u32 now = *(const volatile u32 *)hk_sites[i].site;
        put32(q, hk_sites[i].site);
        q[4] = (u8)((hk_word[i] ? 1 : 0) | hk_sites[i].ctx << 4 |
                    (now != (hk_word[i] ? hk_word[i] : hk_sites[i].orig) ? 0x80 : 0));
        put32(q + 5, now);
    }
    r[13] = (u8)k;
    reply(port, OP_OTA_HOOKS, r, 14 + 9 * k);
}

/* target byte -> slot index, or -1 */
static int tgt_index(u32 t)
{
    return t <= 1 ? (int)t : (t == OTA_TGT_LDR_A || t == OTA_TGT_LDR_B) ? (int)(t - OTA_TGT_LDR_A + 2) : -1;
}

static void send_info(u32 port, u32 t)
{
    u8 r[OTA_INFO_LEN];
    int i = tgt_index(t);
    u32 k;
    if (i < 0)
        return;
    r[0] = (u8)t;
    r[1] = slot[i].flags;
    put32(r + 2, slot[i].seq);
    r[6] = slot[i].tries;
    r[7] = slot[i].stver;
    for (k = 0; k < 0x30; k++)
        r[8 + k] = (slot[i].flags & F_VALID) ? hdrc[i][k] : 0;
    reply(port, OP_OTA_INFO, r, sizeof r);
}

static int job_busy_now(void)
{
    u8 j = job;
    return j >= J_COMMIT_Q && j <= J_SEAL;
}

static int ldr_running_confirmed(void)
{
    return boot->slot < 2 && (slot[2 + boot->slot].flags & F_CONFIRMED);
}

/* LZ tokens of one v2 frame (LZ4 block layout: token = literal count << 4 | match length - 4, 15 = more length bytes
 * that follow, 255 = keep adding; literals; match distance u16 LE; a frame ends after a token's literals or match)
 * decoded into STAGE from pos, never past lim, back references only into STAGE[0, pos).  New pos, or ~0 if malformed. */
#define LZ_BAD 0xFFFFFFFFu
static u32 lz_decode(const u8 *s, u32 n, u32 pos, u32 lim)
{
    u32 i = 0;
    while (i < n) {
        u32 t = s[i++], len = t >> 4, d, k;
        if (len == 15) {
            u8 b;
            do {
                if (i >= n)
                    return LZ_BAD;
                b = s[i++];
                len += b;
            } while (b == 255);
        }
        if (len > n - i || len > lim - pos)
            return LZ_BAD;
        for (k = 0; k < len; k++)
            STAGE[pos++] = s[i++];
        if (i == n)
            break;
        if (n - i < 2)
            return LZ_BAD;
        d = s[i] | (u32)s[i + 1] << 8;
        i += 2;
        len = (t & 15) + 4;
        if ((t & 15) == 15) {
            u8 b;
            do {
                if (i >= n)
                    return LZ_BAD;
                b = s[i++];
                len += b;
            } while (b == 255);
        }
        if (d == 0 || d > pos || len > lim - pos)
            return LZ_BAD;
        for (k = 0; k < len; k++, pos++)
            STAGE[pos] = STAGE[pos - d];
    }
    return pos;
}

/* v2 data frame (ota.h "fast upload"): unpacked into VBUF (free while receiving: only the commit's read-back uses it),
 * CRC first, then stored / decoded at off == received.  Every frame is answered: rc · received · off. */
static void data2(u32 port, const u8 *p, u32 n)
{
    u8 r[9];
    u32 off = (u32)(p[0] & 0x3F) << 14 | (u32)(p[1] & 0x7F) << 7 | (p[2] & 0x7F), m, k, end;
    r[0] = OE_OK;
    if (job != J_RECV) {
        r[0] = OE_STATE;
    } else if (off > job_rx) {
        r[0] = OE_OFFSET;
    } else if (off == job_rx) {
        m = unpack7(VBUF, 5 + OTA_D2_MAX, p + 3, n - 3);
        if (m == 0xFFFFFFFFu)                       /* longer than any frame the loader advertised */
            r[0] = OE_ARGS;
        else if (m < 5 || (crc32_step(0xFFFFFFFFu, VBUF + 4, m - 4) ^ 0xFFFFFFFFu) != get32(VBUF))
            r[0] = OE_CRC;                          /* damaged / cut short on the way: the sender sends it again */
        else if (m < 6 || VBUF[4] > OTA_D2_LZ)
            r[0] = OE_ARGS;
        else if (VBUF[4] & OTA_D2_LZ) {
            end = lz_decode(VBUF + 5, m - 5, off, job_len);
            if (end == LZ_BAD || end == off)
                r[0] = OE_ARGS;
            else
                job_rx = end;
        } else if (m - 5 > job_len - off) {
            r[0] = OE_LEN;
        } else {
            for (k = 0; k < m - 5; k++)
                STAGE[off + k] = VBUF[5 + k];
            job_rx = off + m - 5;
        }
    }                                 /* off < received: a frame already held (resent / duplicated) - rc 0 */
    put32(r + 1, job_rx);
    put32(r + 5, off);
    reply(port, OP_OTA_DATA, r, sizeof r);
}

static void ota_op(u32 port, u8 op, const u8 *p, u32 n)
{
    u8 raw[OTA_CHUNK + 8];
    u32 m;

    if (op == OP_OTA_STATUS) {
        send_status(port);
        return;
    }
    if (op == OP_OTA_HOOKS) {
        send_hooks(port, n >= 1 ? (u32)(p[0] & 0x7F) : 0);
        return;
    }
    if (op == OP_OTA_INFO) {
        if (n == 1)
            send_info(port, p[0]);
        return;
    }
    if (op == OP_OTA_DATA) {
        u32 off, k;
        u8 r[5];
        if (n < 3)
            return;
        if (p[0] & OTA_D2_FLAG) {
            data2(port, p, n);
            return;
        }
        off =(u32)(p[0] & 0x7F) << 14 | (u32)(p[1] & 0x7F) << 7 | (p[2] & 0x7F);
        m = unpack7(raw, OTA_CHUNK, p + 3, n - 3);
        r[0] = OE_OK;
        if (job != J_RECV)
            r[0] = OE_STATE;
        else if (m == 0xFFFFFFFFu || m == 0)
            r[0] = OE_ARGS;
        else if (off != job_rx)
            r[0] = OE_OFFSET;
        else if (m > job_len - job_rx)
            r[0] = OE_LEN;
        else {
            for (k = 0; k < m; k++)
                STAGE[off + k] = raw[k];
            job_rx = off + m;
        }
        put32(r + 1, job_rx);
        reply(port, OP_OTA_DATA, r, 5);
        return;
    }
    m = unpack7(raw, sizeof raw, p, n);
    if (op == OP_OTA_BEGIN) {
        u32 len, crc, seq;
        int s;
        if (m != 13 + OTA_TAG_LEN)
            return;
        s = tgt_index(raw[0]); len = get32(raw + 1); crc = get32(raw + 5); seq = get32(raw + 9);
        if (!auth_ok(op, raw, 13, raw + 13)) { reply_rc(port, op, OE_AUTH); return; }
        if (boot_state != B_DONE) { reply_rc(port, op, OE_BOOTING); return; }
        if (job_busy_now() || apply_busy() || released) { reply_rc(port, op, OE_BUSY); return; }
        if (s < 0 || (s < 2 && s == active_id) || (IS_LDR(s) && s - 2 == boot->slot)) { reply_rc(port, op, OE_SLOT); return; }
        if (IS_LDR(s) && !ldr_running_confirmed()) { reply_rc(port, op, OE_UNCONFIRMED); return; }
        if (len < OTA_HDR_SIZE + (IS_LDR(s) ? sizeof(struct s1_entry) : sizeof(struct ota_entry)) ||
            len > (IS_LDR(s) ? LDR_MAX : OTA_IMAGE_MAX)) { reply_rc(port, op, OE_LEN); return; }
        if (seq <= (IS_LDR(s) ? max_lseq : max_seq) || seq == 0xFFFFFFFFu) { reply_rc(port, op, OE_SEQ); return; }
        job = J_IDLE;
        job_slot = (u8)s; job_len = len; job_crc = crc; job_seq = seq;
        job_rx = 0;
        job_err = OE_OK;
        job = J_RECV;
        reply_rc(port, op, OE_OK);
        return;
    }
    if (op == OP_OTA_COMMIT) {
        if (m != 1 + OTA_TAG_LEN)
            return;
        if (!auth_ok(op, raw, 1, raw + 1)) { reply_rc(port, op, OE_AUTH); return; }
        if (job != J_RECV || tgt_index(raw[0]) != job_slot) { reply_rc(port, op, OE_STATE); return; }
        if (job_rx != job_len) { reply_rc(port, op, OE_INCOMPLETE); return; }
        job = J_COMMIT_Q;
        reply_rc(port, op, OE_OK);
        return;
    }
    if (op == OP_OTA_REJECT) {
        u8 mask;
        u32 i, keep = 0;
        if (m != 1 + OTA_TAG_LEN)
            return;
        mask = raw[0];
        if (!auth_ok(op, raw, 1, raw + 1)) { reply_rc(port, op, OE_AUTH); return; }
        if (boot_state != B_DONE) { reply_rc(port, op, OE_BOOTING); return; }
        if (mask == 0 || (mask & ~0x33u)) { reply_rc(port, op, OE_ARGS); return; }
        if (job_busy_now() || apply_busy()) { reply_rc(port, op, OE_BUSY); return; }
        if (mask & 0x30) {
            /* after the reject a confirmed, sealed, not rejected loader must remain */
            for (i = 2; i < NSLOT; i++)
                if (ldr_ok(i) && !(mask & rej_bit(i)) && !(rej_req & rej_bit(i)))
                    keep++;
            if (!keep) { reply_rc(port, op, OE_LAST); return; }
        }
        rej_req |= mask;
        reply_rc(port, op, OE_OK);
        return;
    }
    if (op == OP_OTA_CONFIRM) {
        if (m != 1 + OTA_TAG_LEN)
            return;
        if (!auth_ok(op, raw, 1, raw + 1)) { reply_rc(port, op, OE_AUTH); return; }
        if (boot->slot > 1 || tgt_index(raw[0]) != 2 + boot->slot) { reply_rc(port, op, OE_SLOT); return; }
        if (!(slot[2 + boot->slot].flags & F_CONFIRMED))
            conf_req = 1;
        reply_rc(port, op, OE_OK);
        return;
    }
    if (op == OP_OTA_APPLY) {
        u8 t;
        if (m != 1 + OTA_TAG_LEN)
            return;
        if (!auth_ok(op, raw, 1, raw + 1)) { reply_rc(port, op, OE_AUTH); return; }
        if (boot_state != B_DONE) { reply_rc(port, op, OE_BOOTING); return; }
        t = raw[0];
        if (t > OTA_TGT_RESELECT) { reply_rc(port, op, OE_ARGS); return; }
        if ((boot->flags & S0F_SAFE) && (t <= 1 || t == OTA_TGT_NEWEST)) { reply_rc(port, op, OE_SAFE); return; }
        if (job == J_RECV || job_busy_now() || apply_busy() || rej_req || conf_req || released) {
            reply_rc(port, op, OE_BUSY);
            return;
        }
        if (t == OTA_TGT_NEWEST) {
            u32 i;
            t = OTA_TGT_BUILTIN;
            for (i = 0; i < 2; i++)
                if (bootable(i) && (t == OTA_TGT_BUILTIN || slot[i].seq > slot[t].seq))
                    t = (u8)i;
        } else if (t <= 1 && (slot[t].flags & (F_VALID | F_REJECTED)) != F_VALID) {
            reply_rc(port, op, OE_SLOT);
            return;
        }
        ap_target = t;
        ap_err = OE_OK;
        ap_wait = 0;
        io_pos = 0;
        if (t == OTA_TGT_BUILTIN || t == OTA_TGT_RESELECT) {
            ota_gate = 1;
            ap_state = AP_QUIESCE;
        } else {
            ap_state = AP_LOAD;
        }
        reply_rc(port, op, OE_OK);
        return;
    }
}

/* ---- the "no main" responder: Identify only (ngver 1, base 2.19, feat0 identify, feat1 OTA) ------------------- */
static void none_sysex(u32 port, const u8 *buf, u32 len)
{
    static const u8 id[13] = { 0xF0, 0x7D, TAG0, TAG1, TAG2, TAG3, 0x41, 1, 2, 19, 0x01, 0x04, 0xF7 };
    if (len == 8 && buf[6] == 0x01)
        o_tx(port, id, sizeof id);
}

/* ---- the stage-1 entries -------------------------------------------------------------------------------------- */
static int s1_init(const struct s0_boot *b)
{
    /* the v1 sizes: this stage-1 reads appended fields (nor_gates) only where `size` covers them */
    if (b->magic != S0_BOOT_MAGIC || b->size < S0_BOOT_SIZE1 || !b->svc ||
        b->svc->magic != S0_SVC_MAGIC || b->svc->size < S0_SVC_SIZE1)
        return 1;
    boot = b;
    svc = b->svc;
    okey = b->key;
    hk_svc.nor = svc;            /* the main's calls come from main code: stage-0 gives them the kvstore only */
    return 0;
}

static void s1_tick(void)
{
    const struct ota_entry *a;
    u8 ap;
    hk_task = stk_cur_task;
    if (released)
        return;
    if (boot_state != B_DONE) {
        boot_step();
    } else if (job >= J_COMMIT_Q && job <= J_SEAL) {
        if (rej_req && job == J_COMMIT_Q)
            do_reject();
        else
            job_step();
    } else if (apply_busy()) {
        apply_step();
    } else if (rej_req) {
        do_reject();
    } else if (conf_req) {
        do_conf_ldr();
    } else {
        do_confirm();
    }
    a = active;
    ap = ap_state;
    if (a && ap != AP_SWITCH) {
        if (ota_alive < OTA_CONFIRM_TICKS)
            ota_alive++;
        ota_hk_open = 1;
        if (attach_pending && ap != AP_QUIESCE) {
            attach_pending = 0;
            ((const struct ota_entry_x *)a)->attach(&hk_svc);
        }
        if (ap != AP_QUIESCE || ap_tick_old)
            a->tick();
        ota_hk_open = 0;
    }
}

static void s1_sysex(u32 port, const u8 *buf, u32 len, u32 *frame)
{
    const struct ota_entry *a;
    (void)frame;
    if (len >= 8 && buf[0] == 0xF0 && buf[1] == 0x7D && buf[len - 1] == 0xF7 &&
        buf[2] == TAG0 && buf[3] == TAG1 && buf[4] == TAG2 && buf[5] == TAG3 &&
        buf[6] >= OP_OTA_INFO && buf[6] <= OP_OTA_HOOKS) {
        if (boot)
            ota_op(port, buf[6], buf + 7, len - 8);
        return;
    }
    ota_in_sysex = 1;
    a = ota_gate ? 0 : active;
    if (a)
        a->sysex(port, buf, len);
    else if (!ota_gate && boot_state == B_DONE && active_id == ACT_NONE)
        none_sysex(port, buf, len);
    ota_in_sysex = 0;
}

/* RESELECT: 0 once the main is gone (apply target 4 dropped it and every hook) and nothing is in flight */
static int s1_release(void)
{
    return released && !active && !hk_n && !ota_in_sysex && !ota_in_task20 && !job_busy_now() ? 0 : 1;
}

__attribute__((section(".rodata.s1_entry"), used))
const struct s1_entry s1_entry = {
    S1_ENTRY_MAGIC, S0_ABI, sizeof(struct s1_entry), s1_init, s1_sysex, s1_tick, s1_release
};
