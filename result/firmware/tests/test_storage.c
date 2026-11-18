/* Tests for the kvstore (with power-loss injection) and the update container. */
#include <stdlib.h>

#include "ca79ng/storage.h"
#include "ca79ng/update.h"
#include "ca79ng/util.h"
#include "test.h"

/* ---- RAM flash with NOR semantics and a programmable "power cut" ---- */
#define FSEC 4096u
typedef struct {
    uint8_t mem[4][FSEC];
    long budget; /* bytes that may still be programmed, -1 = unlimited */
} fake_flash_t;

static bool ff_erase(void *c, uint32_t s)
{
    memset(((fake_flash_t *)c)->mem[s], 0xFF, FSEC);
    return true;
}

static bool ff_program(void *c, uint32_t s, uint32_t off, const void *d, uint32_t n)
{
    fake_flash_t *f = c;
    const uint8_t *p = d;
    for (uint32_t i = 0; i < n; i++) {
        if (f->budget == 0)
            return false; /* power lost mid-write: the rest never lands */
        if (f->budget > 0)
            f->budget--;
        f->mem[s][off + i] &= p[i];
    }
    return true;
}

static bool ff_read(void *c, uint32_t s, uint32_t off, void *d, uint32_t n)
{
    memcpy(d, ((fake_flash_t *)c)->mem[s] + off, n);
    return true;
}

static fake_flash_t ff;
static const hal_flash_t FLASH = {&ff, FSEC, ff_erase, ff_program, ff_read};

TEST(test_kv_basic)
{
    memset(&ff, 0xFF, sizeof ff.mem);
    ff.budget = -1;
    kvstore_t kv;
    CHECK(kv_mount(&kv, &FLASH, 0, 1));
    CHECK(kv_put(&kv, 1, "hello", 5));
    CHECK(kv_put(&kv, 2, "world!", 6));
    CHECK(kv_put(&kv, 1, "HELLO", 5));
    char buf[16] = {0};
    CHECK_EQ(kv_get(&kv, 1, buf, sizeof buf), 5);
    CHECK_MEM(buf, "HELLO", 5);
    CHECK(kv_delete(&kv, 2));
    CHECK_EQ(kv_get(&kv, 2, buf, sizeof buf), -1);

    /* remount sees the same state */
    kvstore_t kv2;
    CHECK(kv_mount(&kv2, &FLASH, 0, 1));
    CHECK_EQ(kv_get(&kv2, 1, buf, sizeof buf), 5);
    CHECK_MEM(buf, "HELLO", 5);
    CHECK_EQ(kv_get(&kv2, 2, buf, sizeof buf), -1);
}

TEST(test_kv_compaction)
{
    memset(&ff, 0xFF, sizeof ff.mem);
    ff.budget = -1;
    kvstore_t kv;
    CHECK(kv_mount(&kv, &FLASH, 0, 1));
    uint8_t v[100];
    for (int i = 0; i < 200; i++) { /* far more than one sector of writes */
        memset(v, i, sizeof v);
        CHECK(kv_put(&kv, (uint16_t)(i % 5), v, sizeof v));
    }
    CHECK(kv.generation > 1);
    kvstore_t kv2;
    CHECK(kv_mount(&kv2, &FLASH, 0, 1));
    for (int k = 0; k < 5; k++) {
        uint8_t out[100];
        CHECK_EQ(kv_get(&kv2, (uint16_t)k, out, sizeof out), 100);
        CHECK_EQ(out[0], 195 + k);
    }
}

TEST(test_kv_power_loss)
{
    /* For every possible cut point of one write, the store must come back
     * with either the old or the new value — never garbage. */
    for (long cut = 0; cut < 40; cut++) {
        memset(&ff, 0xFF, sizeof ff.mem);
        ff.budget = -1;
        kvstore_t kv;
        kv_mount(&kv, &FLASH, 0, 1);
        kv_put(&kv, 7, "old-value", 9);
        ff.budget = cut;
        kv_put(&kv, 7, "new-value", 9);
        ff.budget = -1;
        kvstore_t kv2;
        CHECK(kv_mount(&kv2, &FLASH, 0, 1));
        char buf[16] = {0};
        int n = kv_get(&kv2, 7, buf, sizeof buf);
        CHECK_EQ(n, 9);
        CHECK(memcmp(buf, "old-value", 9) == 0 || memcmp(buf, "new-value", 9) == 0);
        /* and it keeps working afterwards */
        CHECK(kv_put(&kv2, 8, "x", 1));
    }
}

/* ---- update container ---- */
static void put32(uint8_t *p, uint32_t v) { p[0] = v >> 24; p[1] = v >> 16; p[2] = v >> 8; p[3] = (uint8_t)v; }

/* Builds a 1-section container in buf, returns its length (mirrors tools/mkupd.py). */
static size_t build_container(uint8_t *buf, const uint8_t *payload, uint32_t plen, uint32_t version)
{
    memset(buf, 0, 512);
    memcpy(buf, UPD_MAGIC, 8);
    buf[9] = 1;  /* hdr version */
    buf[11] = 1; /* count */
    buf[12] = 0x04; buf[13] = 0x1E; buf[15] = 0x02;
    put32(buf + 16, version);
    uint32_t table_end = UPD_HDR_FIXED + UPD_ENTRY_SIZE;
    uint32_t off = (table_end + 4 + 15) & ~15u;
    uint8_t *e = buf + UPD_HDR_FIXED;
    put32(e, UPD_SEC_MAIN);
    put32(e + 8, off);
    put32(e + 12, plen);
    put32(e + 16, 0x40060000);
    ca_sha256_t c;
    ca_sha256_init(&c);
    ca_sha256_update(&c, payload, plen);
    ca_sha256_final(&c, e + 24);
    memcpy(buf + off, payload, plen);
    put32(buf + 24, off + plen);
    uint8_t zeros[64] = {0};
    uint32_t crc = ca_crc32(0, buf, 32);
    crc = ca_crc32(crc, zeros, 64);
    crc = ca_crc32(crc, buf + UPD_HDR_FIXED, UPD_ENTRY_SIZE);
    put32(buf + table_end, crc);
    return off + plen;
}

TEST(test_update_container)
{
    static uint8_t buf[512];
    const uint8_t payload[] = "firmware-image-bytes";
    size_t n = build_container(buf, payload, sizeof payload, 0x00010000);
    upd_policy_t pol = {.group = 0x04, .machine = 0x1E, .sub = 2, .current_version = 0x00000900};
    upd_info_t info;
    CHECK_EQ(upd_verify(buf, n, &pol, &info), UPD_OK);
    CHECK_EQ(info.count, 1);
    CHECK_EQ(info.sec[0].type, UPD_SEC_MAIN);
    CHECK_EQ(info.sec[0].load, 0x40060000);

    pol.sub = 1; /* CA99 must refuse a CA79 image */
    CHECK_EQ(upd_verify(buf, n, &pol, &info), UPD_E_MODEL);
    pol.sub = 2;
    pol.current_version = 0x00020000;
    CHECK_EQ(upd_verify(buf, n, &pol, &info), UPD_E_DOWNGRADE);
    pol.current_version = 0;

    buf[n - 3] ^= 1; /* corrupt payload */
    CHECK_EQ(upd_verify(buf, n, &pol, &info), UPD_E_HASH);
    buf[n - 3] ^= 1;
    buf[17] ^= 1; /* corrupt header */
    CHECK_EQ(upd_verify(buf, n, &pol, &info), UPD_E_HDRCRC);
    buf[17] ^= 1;
    CHECK_EQ(upd_verify(buf, n - 1, &pol, &info), UPD_E_BOUNDS);
}

TEST(test_boot_ab)
{
    boot_state_t b;
    boot_state_init(&b);
    CHECK_EQ(boot_select(&b), 0);
    boot_mark_pending(&b, 1, 0x00020000);
    CHECK_EQ(boot_select(&b), 1);
    CHECK_EQ(boot_select(&b), 1);
    CHECK_EQ(boot_select(&b), 1);
    CHECK_EQ(boot_select(&b), 0); /* never confirmed -> rollback */
    boot_mark_pending(&b, 1, 0x00020001);
    CHECK_EQ(boot_select(&b), 1);
    boot_confirm(&b, 1);
    CHECK_EQ(b.active, 1);
    CHECK_EQ(boot_select(&b), 1);
}

void suite_storage(void)
{
    puts("storage/update:");
    RUN(test_kv_basic);
    RUN(test_kv_compaction);
    RUN(test_kv_power_loss);
    RUN(test_update_container);
    RUN(test_boot_ab);
}
