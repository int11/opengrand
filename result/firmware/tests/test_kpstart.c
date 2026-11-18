/* Startup settings owned by the core (src/kproto/kpstart.c): snapshot = serialization of the settings store the
 * S1 walk keeps (seen keys + side table, minus the excluded keys), diff against the store, set / value frames,
 * the save debouncer, and the kvstore records - atomic across a power cut, refused without room
 * (the emulator round trip is tools/roundtrip_startup.py). */
#include <string.h>

#include "ca79ng/btsrc.h"
#include "ca79ng/kpstart.h"
#include "ca79ng/power.h"
#include "test.h"

#define FSEC 8192u
typedef struct {
    uint8_t mem[2][FSEC];
    long budget;   /* bytes that may still be programmed, -1 = unlimited */
    int erases;
} ksu_flash_t;

static bool kf_erase(void *c, uint32_t s)
{
    ksu_flash_t *f = c;
    memset(f->mem[s], 0xFF, FSEC);
    f->erases++;
    return true;
}

static bool kf_program(void *c, uint32_t s, uint32_t off, const void *d, uint32_t n)
{
    ksu_flash_t *f = c;
    const uint8_t *p = d;
    for (uint32_t i = 0; i < n; i++) {
        if (f->budget == 0)
            return false;
        if (f->budget > 0)
            f->budget--;
        f->mem[s][off + i] &= p[i];
    }
    return true;
}

static bool kf_read(void *c, uint32_t s, uint32_t off, void *d, uint32_t n)
{
    memcpy(d, ((ksu_flash_t *)c)->mem[s] + off, n);
    return true;
}

static ksu_flash_t kf;
static const hal_flash_t KFLASH = {&kf, FSEC, kf_erase, kf_program, kf_read};
static param_store_t store;
static kst_t st;

static void see(param_id_t id, int slot) { st.seen[id] |= 1u << slot; }

/* a store as the S1 walk leaves it: defaults, a few keys read (seen), one side-table value */
static void setup(void)
{
    static const kst_io_t io = {0};
    param_store_defaults(&store);
    kst_init(&st, &store, &io);
    see(PRM_REVERB_TYPE, param_slot_from_part(0x00));
    see(PRM_SPEAKER_VOLUME, param_slot_from_part(0x7F));
    see(PRM_BLUETOOTH_AUDIO, param_slot_from_part(0x7F));
    see(PRM_METRONOME_TEMPO, param_slot_from_part(0x7F));
    {
        static const uint8_t tempo[2] = {0x00, 0x78};   /* the walk's value (no table default for it) */
        param_put(&store, PRM_METRONOME_TEMPO, param_slot_from_part(0x7F), tempo, 2);
    }
    see(PRM_METRONOME, param_slot_from_part(0x7F));      /* excluded: run state */
    see(PRM_AUTO_POWER_OFF, param_slot_from_part(0x7F)); /* a startup setting now (was: excluded) */
    st.idx[0].key[0] = 0x51; st.idx[0].key[1] = 0x1A; st.idx[0].key[2] = 0x7F;
    st.idx[0].len = 3; st.idx[0].data[0] = 2; st.idx[0].data[1] = 0x45; st.idx[0].data[2] = 0;
}

static const uint8_t *find(const uint8_t *snap, int n, uint8_t v1, uint8_t v2, uint8_t part)
{
    const uint8_t *r;
    int l;
    for (uint32_t pos = KSU_SNAP_HEAD; (l = ksu_rec(snap, (uint32_t)n, pos, &r)) > 0; pos += (uint32_t)l)
        if (r[0] == v1 && r[1] == v2 && r[2] == part)
            return r;
    return 0;
}

TEST(ksu_snapshot_keys_and_bytes)
{
    uint8_t snap[KSU_SNAP_MAX], one = 1;
    uint16_t count = 0;
    int n;
    setup();
    param_put(&store, PRM_BLUETOOTH_AUDIO, param_slot_from_part(0x7F), &one, 1);
    param_put(&store, PRM_SOUND_PREVIEW_VOLUME, param_slot_from_part(0x7F), &one, 1); /* never read by a walk */
    n = ksu_snap_build(&store, &st, snap, sizeof snap);
    CHECK(n > 0);
    CHECK(ksu_snap_check(snap, (uint32_t)n, &count));
    CHECK_EQ(count, 6);                        /* 5 seen + 1 side table; Metronome left out (AutoPowerOff in since the NG part) */
    CHECK(find(snap, n, 0x51, 0x2F, 0x7F) && find(snap, n, 0x51, 0x2F, 0x7F)[4] == 1);
    CHECK(find(snap, n, 0x55, 0x08, 0x00) != 0);
    CHECK(find(snap, n, 0x56, 0x01, 0x7F) && find(snap, n, 0x56, 0x01, 0x7F)[3] == 2);
    CHECK(find(snap, n, 0x51, 0x1A, 0x7F) && find(snap, n, 0x51, 0x1A, 0x7F)[5] == 0x45);
    CHECK(!find(snap, n, 0x56, 0x08, 0x7F));
    CHECK(find(snap, n, 0x51, 0x00, 0x7F) != 0);   /* AutoPowerOff / LockMode follow the mode too */
    CHECK(!find(snap, n, 0x50, 0x1F, 0x7F));
    CHECK_EQ(ksu_snap_build(&store, &st, snap, 20), -1);
    /* malformed */
    snap[4]++;
    CHECK(!ksu_snap_check(snap, (uint32_t)n, 0));
    snap[4]--;
    CHECK(!ksu_snap_check(snap, (uint32_t)n - 1, 0));
    /* exclusions */
    CHECK(ksu_excluded(0x52, 0x02) && ksu_excluded(0x52, 0x01) && ksu_excluded(0x56, 0x0B) && ksu_excluded(0x51, 0x3D));
    CHECK(!ksu_excluded(0x52, 0x1E) && !ksu_excluded(0x51, 0x2F) && !ksu_excluded(0x55, 0x00) && !ksu_excluded(0x56, 0x02));
    CHECK(!ksu_excluded(0x51, 0x00) && !ksu_excluded(0x51, 0x22));   /* AutoPowerOff / LockMode */
}

TEST(ksu_diff_and_frames)
{
    uint8_t snap[KSU_SNAP_MAX], f[KSU_FRAME_MAX], one = 1;
    const uint8_t *r;
    int n;
    setup();
    param_put(&store, PRM_BLUETOOTH_AUDIO, param_slot_from_part(0x7F), &one, 1);
    n = ksu_snap_build(&store, &st, snap, sizeof snap);
    r = find(snap, n, 0x51, 0x2F, 0x7F);
    CHECK_EQ(ksu_rec_differs(r, &store, &st), 0);
    one = 0;
    param_put(&store, PRM_BLUETOOTH_AUDIO, param_slot_from_part(0x7F), &one, 1);   /* stock booted with it off */
    CHECK_EQ(ksu_rec_differs(r, &store, &st), 1);
    CHECK_EQ(ksu_set_frame(r, f), 11);
    {
        static const uint8_t want[] = {0xF0, 0x40, 0x7F, 0x10, 0x08, 0x02, 0x51, 0x2F, 0x7F, 0x01, 0xF7};
        CHECK_MEM(f, want, sizeof want);
    }
    CHECK_EQ(ksu_value_frame(r, f), 11);
    CHECK(f[4] == 0x04 && f[5] == 0x1E);
    /* side table: same / different / missing */
    r = find(snap, n, 0x51, 0x1A, 0x7F);
    CHECK_EQ(ksu_rec_differs(r, &store, &st), 0);
    st.idx[0].data[1] = 0x40;
    CHECK_EQ(ksu_rec_differs(r, &store, &st), 1);
    st.idx[0].len = 0;
    CHECK_EQ(ksu_rec_differs(r, &store, &st), -1);
}

TEST(ksu_debounce)
{
    ksu_deb_t d;
    ksu_deb_init(&d, KSU_QUIET_MS, KSU_GAP_MS);
    CHECK(!ksu_deb_due(&d, 100000));
    for (uint32_t t = 0; t < 10; t++)          /* ten changes 1 s apart */
        ksu_deb_change(&d, 1000 + t * 1000);
    CHECK(!ksu_deb_due(&d, 12999));
    CHECK(ksu_deb_due(&d, 13000));             /* 3 s quiet after the last */
    ksu_deb_saved(&d, 13000);
    CHECK(!ksu_deb_due(&d, 30000));            /* nothing new */
    ksu_deb_change(&d, 14000);
    CHECK(!ksu_deb_due(&d, 17000));            /* quiet, but 10 s since the last save not yet */
    CHECK(ksu_deb_due(&d, 23000));
    ksu_deb_change(&d, 0xFFFFF000u);           /* clock wrap */
    CHECK(ksu_deb_due(&d, 0xFFFFF000u + 3000u + 0x2000u));
}

TEST(ksu_kv_records)
{
    kvstore_t kv;
    uint8_t snap[KSU_SNAP_MAX], back[KSU_SNAP_MAX], one = 1;
    uint32_t gen = 0;
    int n, m;
    memset(&kf, 0xFF, sizeof kf.mem);
    kf.budget = -1;
    CHECK(kv_mount(&kv, &KFLASH, 0, 1));
    CHECK_EQ(ksu_kv_mode(&kv), KSU_STOCK);
    CHECK_EQ(ksu_kv_load(&kv, KSU_P, back, sizeof back, 0), -1);
    CHECK(ksu_kv_set_mode(&kv, KSU_POWEROFF));
    CHECK(!ksu_kv_set_mode(&kv, KSU_MODES));
    CHECK_EQ(ksu_kv_mode(&kv), KSU_POWEROFF);
    /* a big snapshot (every value of the store seen: > 1 chunk) round-trips */
    setup();
    for (int id = 0; id < PRM_COUNT; id++)
        st.seen[id] = 0xFFFFFFFFu;
    n = ksu_snap_build(&store, &st, snap, sizeof snap);
    CHECK(n > (int)KSU_CHUNK);
    CHECK_EQ(ksu_kv_save(&kv, KSU_P, snap, (uint32_t)n, NULL, 0, false), KSU_KV_OK);
    m = ksu_kv_load(&kv, KSU_P, back, sizeof back, &gen);
    CHECK_EQ(m, n);
    CHECK_EQ(gen, 1u);
    CHECK_MEM(back, snap, (size_t)n);
    CHECK_EQ(ksu_kv_load(&kv, KSU_C, back, sizeof back, 0), -1);   /* the other slot is separate */
    {   /* remount: same */
        kvstore_t kv2;
        CHECK(kv_mount(&kv2, &KFLASH, 0, 1));
        CHECK_EQ(ksu_kv_load(&kv2, KSU_P, back, sizeof back, 0), n);
        CHECK_EQ(ksu_kv_mode(&kv2), KSU_POWEROFF);
    }
    /* a power cut anywhere in the next save leaves the old snapshot whole (E9) */
    {
        uint8_t snap2[KSU_SNAP_MAX];
        int n2, cuts = 0, old = 0, neu = 0;
        param_put(&store, PRM_BLUETOOTH_AUDIO, param_slot_from_part(0x7F), &one, 1);
        n2 = ksu_snap_build(&store, &st, snap2, sizeof snap2);
        for (long b = 0; b < (long)ksu_kv_need((uint32_t)n2, 0) + 64; b += 37) {
            kvstore_t k2, k3;
            ksu_flash_t keep = kf;
            CHECK(kv_mount(&k2, &KFLASH, 0, 1));
            kf.budget = b;
            (void)ksu_kv_save(&k2, KSU_P, snap2, (uint32_t)n2, NULL, 0, false);
            kf.budget = -1;
            CHECK(kv_mount(&k3, &KFLASH, 0, 1));
            m = ksu_kv_load(&k3, KSU_P, back, sizeof back, 0);
            if (m == n && memcmp(back, snap, (size_t)n) == 0)
                old++;
            else if (m == n2 && memcmp(back, snap2, (size_t)n2) == 0)
                neu++;
            cuts++;
            kf = keep;
        }
        CHECK(cuts > 10);
        CHECK_EQ(old + neu, cuts);             /* never mixed, never lost */
        CHECK(old > 0 && neu > 0);
    }
    /* no room: refused without allow_compact (no erase), done with it */
    {
        int erases, r;
        CHECK(kv_mount(&kv, &KFLASH, 0, 1));
        while (kv_free(&kv) >= ksu_kv_need((uint32_t)n, 0))
            CHECK_EQ(ksu_kv_save(&kv, KSU_P, snap, (uint32_t)n, NULL, 0, false), KSU_KV_OK);
        erases = kf.erases;
        CHECK_EQ(ksu_kv_save(&kv, KSU_P, snap, (uint32_t)n, NULL, 0, false), KSU_KV_NOROOM);
        CHECK_EQ(kf.erases, erases);
        r = ksu_kv_save(&kv, KSU_P, snap, (uint32_t)n, NULL, 0, true);
        CHECK_EQ(r, KSU_KV_OK);
        CHECK(kf.erases > erases);
        CHECK_EQ(ksu_kv_load(&kv, KSU_P, back, sizeof back, 0), n);
        CHECK_EQ(ksu_kv_mode(&kv), KSU_POWEROFF);   /* compaction kept the other records */
    }
    /* clear: both gone, mode stock */
    CHECK_EQ(ksu_kv_save(&kv, KSU_C, snap, (uint32_t)n, NULL, 0, true), KSU_KV_OK);
    CHECK(ksu_kv_clear(&kv));
    CHECK_EQ(ksu_kv_load(&kv, KSU_P, back, sizeof back, 0), -1);
    CHECK_EQ(ksu_kv_load(&kv, KSU_C, back, sizeof back, 0), -1);
    CHECK_EQ(ksu_kv_mode(&kv), KSU_STOCK);
}


/* ---- the modes govern every setting (user decision 2026-11-13 "A") ---------------------------------------
 * A model of one piano across power cycles, built from the same pieces the cave ports use: the live values of our own
 * settings on NOR (KV_KEY_BT_POLICY = bts_saved_*, KV_KEY_POWER_POLICY = power_policy_t) loaded the way the ports load
 * them, the startup mode / snapshots / NG part, ksu_ng_decide at power-on, AutoPowerOff / LockMode kept by the stock in
 * settings block A between power cycles and a stock sound value (reverb type) that the stock boots at its UserMemory
 * value (here: the factory value). */
typedef struct {
    bts_policy_t bt;
    power_policy_t pw;
    uint8_t apo, lock, reverb;
} piano_t_model;

static piano_t_model pm;
static uint8_t blka_apo, blka_lock;   /* settings block A: what the stock writes at the off and loads at the next boot */
static const uint32_t MODEL_CODE = 0x55AA5A01u, DEST = 0xA5A5AA01u;   /* this piano's block A words 0 / 3 */
static ksu_ng_t pm_kind;
static int pm_restored;

static void pm_flash_blank(void)
{
    memset(&kf, 0xFF, sizeof kf.mem);
    kf.budget = -1;
    kf.erases = 0;
}

/* the settings store as the S1 walk leaves it after the stock boot: the values below, every one seen */
static void pm_store(void)
{
    static const kst_io_t io = {0};
    uint8_t lock[2] = {pm.lock, 0};
    param_store_defaults(&store);
    kst_init(&st, &store, &io);
    param_put(&store, PRM_AUTO_POWER_OFF, SLOT_SYSTEM, &pm.apo, 1);
    param_put(&store, PRM_LOCK_MODE, SLOT_SYSTEM, lock, 2);
    param_put(&store, PRM_REVERB_TYPE, param_slot_from_part(0x00), &pm.reverb, 1);
    see(PRM_AUTO_POWER_OFF, SLOT_SYSTEM);
    see(PRM_LOCK_MODE, SLOT_SYSTEM);
    see(PRM_REVERB_TYPE, param_slot_from_part(0x00));
}

/* a restore as startup_cave.c does it: every record that differs from the store becomes a set (here: written into
 * the model) */
static void pm_restore(const uint8_t *snap, int n)
{
    const uint8_t *r;
    int l;
    pm_store();
    for (uint32_t pos = KSU_SNAP_HEAD; (l = ksu_rec(snap, (uint32_t)n, pos, &r)) > 0; pos += (uint32_t)l) {
        if (ksu_rec_differs(r, &store, &st) == 0)
            continue;
        pm_restored++;
        if (r[0] == 0x51 && r[1] == 0x00)
            pm.apo = r[4];
        else if (r[0] == 0x51 && r[1] == 0x22)
            pm.lock = r[4];
        else if (r[0] == 0x55 && r[1] == 0x08 && r[2] == 0x00)
            pm.reverb = r[4];
    }
}

/* the NG part now (startup_cave.c write_snapshot: from the ports' RAM) */
static uint32_t pm_ng(uint8_t ng[KSU_NG_MAX])
{
    uint8_t bt[BTS_SAVED_RAW], pw[POWER_POLICY_RAW];
    bts_saved_encode(&pm.bt, bt);
    power_policy_encode(&pm.pw, pw);
    return ksu_ng_build(bt, BTS_SAVED_RAW, pw, POWER_POLICY_RAW, ng);
}

/* a snapshot of the model now into slot s (with the NG part unless old_build) */
static void pm_save(ksu_slot_t s, bool old_build)
{
    kvstore_t kv;
    uint8_t snap[KSU_SNAP_MAX], ng[KSU_NG_MAX];
    uint32_t ngn = pm_ng(ng);
    int n;
    pm_store();
    n = ksu_snap_build(&store, &st, snap, sizeof snap);
    CHECK(n > 0);
    CHECK(kv_mount(&kv, &KFLASH, 0, 1));
    CHECK_EQ(ksu_kv_save(&kv, s, snap, (uint32_t)n, old_build ? NULL : ng, old_build ? 0 : ngn, true), KSU_KV_OK);
}

/* a user change of our own settings: the port writes the live value (bt_cave.c kv_tick, power_cave.c kv_save) */
static void pm_live_write(void)
{
    kvstore_t kv;
    uint8_t bt[BTS_SAVED_RAW], pw[POWER_POLICY_RAW];
    bts_saved_encode(&pm.bt, bt);
    power_policy_encode(&pm.pw, pw);
    CHECK(kv_mount(&kv, &KFLASH, 0, 1));
    CHECK(kv_put(&kv, KV_KEY_BT_POLICY, bt, sizeof bt));
    CHECK(kv_put(&kv, KV_KEY_POWER_POLICY, pw, sizeof pw));
}

static void pm_change(uint8_t recon, const char *pref, bool follow, bool apo_standby, uint8_t apo, uint8_t lock,
                      uint8_t reverb)
{
    pm.bt.recon = recon;
    pm.bt.has_pref = pref != 0;
    memset(pm.bt.pref, 0, 6);
    if (pref)
        memcpy(pm.bt.pref, pref, 6);
    if (follow)
        pm.bt.flags |= BTS_F_FOLLOW;
    else
        pm.bt.flags &= (uint8_t)~BTS_F_FOLLOW;
    pm.pw.apo_standby = apo_standby;
    pm.apo = apo;
    pm.lock = lock;
    pm.reverb = reverb;
    pm_live_write();
}

/* power off (the stock writes block A when APO / Lock changed) and on: the ports load the live values, then the startup
 * module decides (at_boot false = a live apply: nothing restored) */
static void pm_cycle(bool at_boot)
{
    kvstore_t kv;
    uint8_t raw[BTS_SAVED_RAW + 6], snap[KSU_SNAP_MAX], ng[KSU_NG_MAX];
    int n = -1, ngn = -1;
    ksu_mode_t mode;
    bool all;
    if (at_boot) {
        blka_apo = pm.apo;
        blka_lock = pm.lock;
        memset(&pm, 0, sizeof pm);
        pm.apo = blka_apo;                  /* the stock: block A */
        pm.lock = blka_lock;
        pm.reverb = 0;                      /* the stock: UserMemory (factory here) */
        bts_policy_default(&pm.bt);         /* the ports: defaults, then the live values on NOR */
        power_policy_default(&pm.pw);
    }
    pm_restored = 0;
    CHECK(kv_mount(&kv, &KFLASH, 0, 1));
    if (at_boot) {
        int l = kv_get(&kv, KV_KEY_BT_POLICY, raw, sizeof raw);
        if (l > 0)
            (void)bts_saved_apply(&pm.bt, raw, (uint32_t)l);
        l = kv_get(&kv, KV_KEY_POWER_POLICY, raw, sizeof raw);
        if (l > 0)
            (void)power_policy_decode(&pm.pw, raw, (uint16_t)l);
    }
    mode = ksu_kv_mode(&kv);
    all = ksu_kv_mode_all(&kv);
    if (mode != KSU_STOCK) {
        ksu_slot_t s = mode == KSU_CURRENT ? KSU_C : KSU_P;
        n = ksu_kv_load(&kv, s, snap, sizeof snap, NULL);
        ngn = n > 0 ? ksu_kv_load_ng(&kv, s, ng, sizeof ng) : -1;
    }
    pm_kind = ksu_ng_decide(mode, all, at_boot, ngn > 0);
    if (pm_kind == KSU_NG_DEFAULTS) {
        bts_policy_t d;
        bts_policy_default(&d);
        bts_saved_encode(&d, raw);
        CHECK(bts_saved_apply(&pm.bt, raw, BTS_SAVED_RAW));
        power_policy_default(&pm.pw);
        n = ksu_snap_factory(ksu_apo_factory(MODEL_CODE, DEST), snap, sizeof snap);
    } else if (pm_kind == KSU_NG_SAVED) {
        const uint8_t *bt, *pw;
        uint8_t btn, pwn;
        CHECK(ksu_ng_parts(ng, (uint32_t)ngn, &bt, &btn, &pw, &pwn));
        CHECK(bts_saved_apply(&pm.bt, bt, btn));
        CHECK(power_policy_decode(&pm.pw, pw, pwn));
    }
    if (at_boot && n > 0 && (mode != KSU_STOCK || pm_kind == KSU_NG_DEFAULTS))
        pm_restore(snap, n);
}

static void pm_set_mode(ksu_mode_t m)
{
    kvstore_t kv;
    CHECK(kv_mount(&kv, &KFLASH, 0, 1));
    CHECK(ksu_kv_set_mode(&kv, m));
}

static const char PHONE[6] = {0x02, 0x1a, 0x7d, 0xda, 0x11, (char)0xa1};
static const char PC[6] = {0x02, 0x1a, 0x7d, 0xda, 0x13, 0x23};

static void pm_fresh(void)
{
    pm_flash_blank();
    memset(&pm, 0, sizeof pm);
    bts_policy_default(&pm.bt);
    power_policy_default(&pm.pw);
    blka_apo = pm.apo = 3;   /* the dump: APO index 3, lock 0 */
    blka_lock = pm.lock = 0;
}

TEST(ksu_all_mode_stock_defaults)
{
    pm_fresh();
    pm_set_mode(KSU_STOCK);   /* chosen with the NG part: KV_KEY_STARTUP_ALL */
    pm_change(BTS_RC_OFF, PC, true, false, 2, 1, 5);
    pm_cycle(true);
    CHECK_EQ(pm_kind, KSU_NG_DEFAULTS);
    CHECK_EQ(pm.bt.recon, BTS_RC_PREF_LAST);
    CHECK_EQ(pm.bt.has_pref, 0);
    CHECK(!(pm.bt.flags & BTS_F_FOLLOW));
    CHECK(pm.pw.apo_standby);                                /* power_policy_default */
    CHECK_EQ(pm.apo, ksu_apo_factory(MODEL_CODE, DEST));     /* 1 = 15 min for destination 0xA5A5AA01 */
    CHECK_EQ(pm.apo, 1);
    CHECK_EQ(pm.lock, 0);
    CHECK_EQ(pm.reverb, 0);                                  /* the stock sound state: as before (UserMemory) */
    CHECK_EQ(pm_restored, 2);                                /* APO 2 -> 1, Lock 1 -> 0 */
    /* the factory rule (FUN_400B0B9C) */
    CHECK_EQ(ksu_apo_factory(0, 0xAAA55504u), 0);
    CHECK_EQ(ksu_apo_factory(0xAA5AA502u, 0x555AA503u), 0);
    CHECK_EQ(ksu_apo_factory(0xAA5AA502u, 0x12345678u), 1);
    CHECK_EQ(ksu_apo_factory(0x55AA5A01u, 0x12345678u), 0);
}

TEST(ksu_all_mode_poweroff_last_values)
{
    pm_fresh();
    pm_set_mode(KSU_POWEROFF);
    pm_change(BTS_RC_PREF, PHONE, true, false, 0, 0, 4);
    pm_save(KSU_P, false);                                   /* the debounced save after the change */
    pm_cycle(true);
    CHECK_EQ(pm_kind, KSU_NG_SAVED);
    CHECK_EQ(pm.bt.recon, BTS_RC_PREF);
    CHECK(pm.bt.has_pref && memcmp(pm.bt.pref, PHONE, 6) == 0);
    CHECK(pm.bt.flags & BTS_F_FOLLOW);
    CHECK(!pm.pw.apo_standby);
    CHECK_EQ(pm.apo, 0);
    CHECK_EQ(pm.reverb, 4);
    /* a change the save did not catch (cut within the 3 s): P stays - NG part and stock part alike */
    pm_change(BTS_RC_OFF, PC, false, true, 2, 0, 6);
    pm_cycle(true);
    CHECK_EQ(pm.bt.recon, BTS_RC_PREF);
    CHECK(memcmp(pm.bt.pref, PHONE, 6) == 0);
    CHECK(pm.bt.flags & BTS_F_FOLLOW);
    CHECK(!pm.pw.apo_standby);
    CHECK_EQ(pm.apo, 0);                                     /* block A had 2: the restore sets 0 */
    CHECK_EQ(pm.reverb, 4);
}

TEST(ksu_all_mode_current_saved_values)
{
    pm_fresh();
    pm_change(BTS_RC_PREF, PC, false, true, 3, 2, 7);
    pm_save(KSU_C, false);                                   /* "save now" (1C 02) ... */
    pm_set_mode(KSU_CURRENT);                                /* ... sets the mode current */
    pm_change(BTS_RC_OFF, PHONE, true, false, 0, 0, 1);      /* later changes are not saved */
    pm_cycle(true);
    CHECK_EQ(pm_kind, KSU_NG_SAVED);
    CHECK_EQ(pm.bt.recon, BTS_RC_PREF);
    CHECK(memcmp(pm.bt.pref, PC, 6) == 0);
    CHECK(!(pm.bt.flags & BTS_F_FOLLOW));
    CHECK(pm.pw.apo_standby);
    CHECK_EQ(pm.apo, 3);
    CHECK_EQ(pm.lock, 2);
    CHECK_EQ(pm.reverb, 7);
    pm_cycle(true);                                          /* every power-on the same */
    CHECK_EQ(pm.apo, 3);
    CHECK(memcmp(pm.bt.pref, PC, 6) == 0);
}

TEST(ksu_all_compat_old_builds)
{
    /* (a) a snapshot P of a build before (no NG part) and its mode record (no KV_KEY_STARTUP_ALL): our settings keep
     * the live values on NOR, the stock part is restored as before */
    kvstore_t kv;
    pm_fresh();
    CHECK(kv_mount(&kv, &KFLASH, 0, 1));
    {
        const uint8_t m[2] = {1, KSU_POWEROFF};   /* exactly what a build before writes */
        CHECK(kv_put(&kv, KV_KEY_STARTUP_MODE, m, sizeof m));
    }
    pm_change(BTS_RC_PREF, PHONE, false, true, 1, 0, 4);
    pm_save(KSU_P, true);
    pm_change(BTS_RC_OFF, PC, true, false, 1, 0, 4);         /* live values after the old snapshot */
    pm_cycle(true);
    CHECK_EQ(pm_kind, KSU_NG_KEEP);
    CHECK_EQ(pm.bt.recon, BTS_RC_OFF);                        /* NOR now, not reset, not the snapshot */
    CHECK(pm.bt.has_pref && memcmp(pm.bt.pref, PC, 6) == 0);
    CHECK(pm.bt.flags & BTS_F_FOLLOW);
    CHECK(!pm.pw.apo_standby);
    CHECK_EQ(pm.reverb, 4);
    CHECK(!ksu_kv_mode_all(&kv));
    /* the next save by this build carries the NG part: from then on the mode governs it */
    pm_save(KSU_P, false);
    pm_change(BTS_RC_PREF, PHONE, false, true, 1, 0, 4);
    pm_cycle(true);
    CHECK_EQ(pm_kind, KSU_NG_SAVED);
    CHECK_EQ(pm.bt.recon, BTS_RC_OFF);
    /* (b) the mode never chosen (absent) or stock chosen in a build before: nothing changes at all */
    pm_fresh();
    pm_change(BTS_RC_OFF, PC, true, false, 2, 1, 4);
    pm_cycle(true);
    CHECK_EQ(pm_kind, KSU_NG_KEEP);
    CHECK_EQ(pm.bt.recon, BTS_RC_OFF);
    CHECK(memcmp(pm.bt.pref, PC, 6) == 0);
    CHECK_EQ(pm.apo, 2);                                      /* the stock kept it (block A) */
    CHECK_EQ(pm.lock, 1);
    CHECK_EQ(pm_restored, 0);
    CHECK(kv_mount(&kv, &KFLASH, 0, 1));
    {
        const uint8_t m[2] = {1, KSU_STOCK};
        CHECK(kv_put(&kv, KV_KEY_STARTUP_MODE, m, sizeof m));
    }
    pm_cycle(true);
    CHECK_EQ(pm_kind, KSU_NG_KEEP);
    /* (c) an old live record (9 bytes + 0: no flags byte meaning) leaves follow off */
    {
        bts_policy_t p;
        const uint8_t old[9] = {1, BTS_RC_PREF, 1, 1, 2, 3, 4, 5, 6};
        bts_policy_default(&p);
        p.flags |= BTS_F_FOLLOW;
        CHECK(bts_saved_apply(&p, old, sizeof old));
        CHECK(!(p.flags & BTS_F_FOLLOW));
        CHECK_EQ(p.recon, BTS_RC_PREF);
    }
}

TEST(ksu_all_live_apply_restores_nothing)
{
    pm_fresh();
    pm_set_mode(KSU_STOCK);
    pm_change(BTS_RC_OFF, PC, true, false, 2, 1, 5);
    pm_cycle(false);                                         /* a live apply: the RAM stays, nothing restored */
    CHECK_EQ(pm_kind, KSU_NG_KEEP);
    CHECK_EQ(pm.bt.recon, BTS_RC_OFF);
    CHECK(memcmp(pm.bt.pref, PC, 6) == 0);
    CHECK_EQ(pm.apo, 2);
    CHECK_EQ(pm_restored, 0);
    pm_set_mode(KSU_CURRENT);
    pm_save(KSU_C, false);
    pm_change(BTS_RC_PREF, PHONE, false, true, 0, 0, 1);
    pm_cycle(false);
    CHECK_EQ(pm_kind, KSU_NG_KEEP);
    CHECK_EQ(pm.bt.recon, BTS_RC_PREF);
    CHECK_EQ(pm.reverb, 1);
    CHECK_EQ(pm_restored, 0);
    for (int m = 0; m < KSU_MODES; m++)
        for (int a = 0; a < 2; a++)
            for (int h = 0; h < 2; h++)
                CHECK_EQ(ksu_ng_decide((ksu_mode_t)m, a, false, h), KSU_NG_KEEP);
}

TEST(ksu_all_ng_record)
{
    kvstore_t kv;
    uint8_t snap[KSU_SNAP_MAX], ng[KSU_NG_MAX], ng2[KSU_NG_MAX], back[KSU_NG_MAX];
    const uint8_t *bt, *pw;
    uint8_t btn, pwn;
    uint32_t ngn, ngn2;
    int n;
    pm_fresh();
    pm_store();
    n = ksu_snap_build(&store, &st, snap, sizeof snap);
    ngn = pm_ng(ng);
    CHECK_EQ(ngn, 3u + BTS_SAVED_RAW + POWER_POLICY_RAW);
    CHECK(ksu_ng_parts(ng, ngn, &bt, &btn, &pw, &pwn));
    CHECK(btn == BTS_SAVED_RAW && pwn == POWER_POLICY_RAW && bt == ng + 2 && pw == ng + 3 + BTS_SAVED_RAW);
    CHECK(!ksu_ng_parts(ng, ngn - 1, 0, 0, 0, 0));
    CHECK_EQ(ksu_ng_build(ng, 20, ng, 20, back), 0u);        /* does not fit */
    CHECK(kv_mount(&kv, &KFLASH, 0, 1));
    CHECK_EQ(ksu_kv_load_ng(&kv, KSU_P, back, sizeof back), -1);
    CHECK_EQ(ksu_kv_save(&kv, KSU_P, snap, (uint32_t)n, ng, ngn, false), KSU_KV_OK);   /* gen 1, bank 0 */
    CHECK_EQ(ksu_kv_load_ng(&kv, KSU_P, back, sizeof back), (int)ngn);
    CHECK_MEM(back, ng, ngn);
    CHECK_EQ(ksu_kv_load_ng(&kv, KSU_C, back, sizeof back), -1);
    pm.bt.recon = BTS_RC_OFF;
    ngn2 = pm_ng(ng2);
    CHECK_EQ(ksu_kv_save(&kv, KSU_P, snap, (uint32_t)n, ng2, ngn2, false), KSU_KV_OK);  /* gen 2, bank 1 */
    CHECK_EQ(ksu_kv_load_ng(&kv, KSU_P, back, sizeof back), (int)ngn2);
    CHECK_MEM(back, ng2, ngn2);
    /* a build before writes gen 3 into bank 0 (no NG record): the gen-1 NG record of bank 0 is not this snapshot's */
    CHECK_EQ(ksu_kv_save(&kv, KSU_P, snap, (uint32_t)n, NULL, 0, false), KSU_KV_OK);
    CHECK_EQ(ksu_kv_load(&kv, KSU_P, snap, sizeof snap, NULL), n);
    CHECK_EQ(ksu_kv_load_ng(&kv, KSU_P, back, sizeof back), -1);
    CHECK_EQ(ksu_kv_need((uint32_t)n, ngn), ksu_kv_need((uint32_t)n, 0) + kv_record_size((uint16_t)(12u + ngn)));
    /* a power cut anywhere in a save with an NG part: the old pair or the new pair, never mixed */
    {
        int old = 0, neu = 0, cuts = 0;
        CHECK_EQ(ksu_kv_save(&kv, KSU_P, snap, (uint32_t)n, ng, ngn, false), KSU_KV_OK);
        for (long b = 0; b < (long)ksu_kv_need((uint32_t)n, ngn2) + 64; b += 7) {
            kvstore_t k2, k3;
            ksu_flash_t keep = kf;
            int m;
            CHECK(kv_mount(&k2, &KFLASH, 0, 1));
            kf.budget = b;
            (void)ksu_kv_save(&k2, KSU_P, snap, (uint32_t)n, ng2, ngn2, false);
            kf.budget = -1;
            CHECK(kv_mount(&k3, &KFLASH, 0, 1));
            CHECK_EQ(ksu_kv_load(&k3, KSU_P, snap, sizeof snap, NULL), n);
            m = ksu_kv_load_ng(&k3, KSU_P, back, sizeof back);
            if (m == (int)ngn && memcmp(back, ng, ngn) == 0)
                old++;
            else if (m == (int)ngn2 && memcmp(back, ng2, ngn2) == 0)
                neu++;
            cuts++;
            kf = keep;
        }
        CHECK(cuts > 5);
        CHECK_EQ(old + neu, cuts);
        CHECK(old > 0 && neu > 0);
    }
    /* the mode's "all" record: set by ksu_kv_set_mode and ksu_kv_clear, stale when a build before changed the mode */
    CHECK(kv_mount(&kv, &KFLASH, 0, 1));
    CHECK(!ksu_kv_mode_all(&kv));
    CHECK(ksu_kv_set_mode(&kv, KSU_CURRENT));
    CHECK(ksu_kv_mode_all(&kv));
    {
        const uint8_t m[2] = {1, KSU_POWEROFF};
        CHECK(kv_put(&kv, KV_KEY_STARTUP_MODE, m, sizeof m));
    }
    CHECK(!ksu_kv_mode_all(&kv));
    CHECK(ksu_kv_clear(&kv));
    CHECK_EQ(ksu_kv_mode(&kv), KSU_STOCK);
    CHECK(ksu_kv_mode_all(&kv));                             /* a factory reset / 1C 03: stock = factory everything */
    /* the factory snapshot */
    n = ksu_snap_factory(1, snap, sizeof snap);
    {
        static const uint8_t want[] = {'K', 'S', 1, 0, 2, 0, 0, 0, 0x51, 0x00, 0x7F, 1, 1, 0x51, 0x22, 0x7F, 2, 0, 0};
        CHECK_EQ(n, (int)sizeof want);
        CHECK_MEM(snap, want, sizeof want);
        CHECK(ksu_snap_check(snap, (uint32_t)n, 0));
    }
}

/* save timing (startup_cave.c suc_pump): a change of our own settings starts the same debounce as a stock change */
TEST(ksu_all_ng_change_debounce)
{
    ksu_deb_t d;
    uint32_t stock_changes = 0, ng_changes = 0, last = 0;
    ksu_deb_init(&d, KSU_QUIET_MS, KSU_GAP_MS);
    for (uint32_t t = 0; t < 30000; t += 100) {
        if (t == 1000)
            ng_changes++;   /* 1D 06: the reconnect choice */
        if (t == 2000)
            ng_changes++;   /* 18 01: the power policy */
        if (stock_changes + ng_changes != last) {
            last = stock_changes + ng_changes;
            ksu_deb_change(&d, t);
        }
        if (ksu_deb_due(&d, t)) {
            CHECK_EQ(t, 5000u);   /* 3 s after the last change */
            ksu_deb_saved(&d, t);
        }
    }
    CHECK(!d.dirty);
}

void suite_kpstart(void);
void suite_kpstart(void)
{
    printf("startup settings (kpstart)\n");
    RUN(ksu_snapshot_keys_and_bytes);
    RUN(ksu_diff_and_frames);
    RUN(ksu_debounce);
    RUN(ksu_kv_records);
    RUN(ksu_all_mode_stock_defaults);
    RUN(ksu_all_mode_poweroff_last_values);
    RUN(ksu_all_mode_current_saved_values);
    RUN(ksu_all_compat_old_builds);
    RUN(ksu_all_live_apply_restores_nothing);
    RUN(ksu_all_ng_record);
    RUN(ksu_all_ng_change_debounce);
}
