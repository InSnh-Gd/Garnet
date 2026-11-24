// test-binary-archive.c: pure-C host unit tests for rdna4_binary_archive.h (Task 3.1).
// Build/run (GCC -Werror & Clang ASan+UBSan):
//   gcc -std=c11 -D_GNU_SOURCE -O2 -Wall -Wextra -Werror -Isrc/garnet-metal -o "$TMPDIR/test-binary-archive" src/garnet-metal/test-binary-archive.c && "$TMPDIR/test-binary-archive"
//   clang -std=c11 -D_GNU_SOURCE -O1 -g -fsanitize=address,undefined -Wall -Wextra -Werror -Isrc/garnet-metal -o "$TMPDIR/test-binary-archive-asan" src/garnet-metal/test-binary-archive.c && "$TMPDIR/test-binary-archive-asan"
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "rdna4_binary_archive.h"

static int fails, checks;
#define CHECK(name, cond, ...) do { \
    int ok_ = (cond) ? 1 : 0; \
    checks++; \
    printf("%s %s: ", ok_ ? "ok  " : "FAIL", name); \
    printf(__VA_ARGS__); \
    printf("\n"); \
    if (!ok_) fails++; \
} while (0)

static rdna4ba_fingerprint_t make_fp(void) {
    rdna4ba_fingerprint_t fp;
    memset(&fp, 0, sizeof fp);
    fp.garnet_abi_version = RDNA4BA_ABI_VERSION;
    fp.translator_abi_version = 3u;
    fp.vendor_id = 0x1002u;
    fp.device_id = 0x7590u; // RX 9060 XT (Navi 44)
    fp.driver_version = 0x06803001u;
    fp.flags = 1u;
    for (int i = 0; i < 16; i++) fp.pipeline_cache_uuid[i] = (uint8_t)(0xB0 + i);
    for (int i = 0; i < 32; i++) fp.translator_hash[i] = (uint8_t)(0x40 + i * 3);
    return fp;
}

static void fill_spv(uint8_t *buf, size_t len, uint32_t seed) {
    rdna4ba_put_u32(buf + 0, RDNA4BA_SPV_MAGIC);
    rdna4ba_put_u32(buf + 4, 0x00010400u);
    rdna4ba_put_u32(buf + 8, 0x00000000u);
    rdna4ba_put_u32(buf + 12, 64u);
    rdna4ba_put_u32(buf + 16, 0u);
    for (size_t i = 20; i < len; i++) buf[i] = (uint8_t)((i * 31u + seed) & 0xFFu);
}

static void make_key(const char *label, uint8_t sha[32], uint64_t *u64) {
    rdna4ba_sha256(label, strlen(label), sha);
    *u64 = rdna4ba_key_u64_from_sha(sha);
}

static void test_sha256_and_helpers(void) {
    // NIST FIPS 180-4 test vectors: "" and "abc" and 112-byte multi-block.
    static const uint8_t exp_empty[32] = {
        0xe3,0xb0,0xc4,0x42,0x98,0xfc,0x1c,0x14,0x9a,0xfb,0xf4,0xc8,0x99,0x6f,0xb9,0x24,
        0x27,0xae,0x41,0xe4,0x64,0x9b,0x93,0x4c,0xa4,0x95,0x99,0x1b,0x78,0x52,0xb8,0x55
    };
    static const uint8_t exp_abc[32] = {
        0xba,0x78,0x16,0xbf,0x8f,0x01,0xcf,0xea,0x41,0x41,0x40,0xde,0x5d,0xae,0x22,0x23,
        0xb0,0x03,0x61,0xa3,0x96,0x17,0x7a,0x9c,0xb4,0x10,0xff,0x61,0xf2,0x00,0x15,0xad
    };
    static const char multi[] =
        "abcdefghbcdefghicdefghijdefghijkefghijklfghijklmghijklmnhijklmno"
        "ijklmnopjklmnopqklmnopqrlmnopqrsmnopqrstnopqrstu";
    static const uint8_t exp_multi[32] = {
        0xcf,0x5b,0x16,0xa7,0x78,0xaf,0x83,0x80,0x03,0x6c,0xe5,0x9e,0x7b,0x04,0x92,0x37,
        0x0b,0x24,0x9b,0x11,0xe8,0xf0,0x7a,0x51,0xaf,0xac,0x45,0x03,0x7a,0xfe,0xe9,0xd1
    };

    uint8_t got[32];
    rdna4ba_sha256("", 0, got);
    CHECK("sha256 empty", memcmp(got, exp_empty, 32) == 0, "NIST empty vector");
    rdna4ba_sha256("abc", 3, got);
    CHECK("sha256 abc", memcmp(got, exp_abc, 32) == 0, "NIST abc vector");

    // Incremental update across chunk boundaries.
    rdna4ba_sha256_ctx ctx;
    rdna4ba_sha256_init(&ctx);
    rdna4ba_sha256_update(&ctx, multi, 17);
    rdna4ba_sha256_update(&ctx, multi + 17, 50);
    rdna4ba_sha256_update(&ctx, multi + 67, strlen(multi) - 67);
    rdna4ba_sha256_final(&ctx, got);
    CHECK("sha256 multi-block incremental", memcmp(got, exp_multi, 32) == 0, "NIST 112-byte vector");

    uint8_t spv[24];
    fill_spv(spv, sizeof spv, 1);
    CHECK("spv header valid", rdna4ba_spv_header_ok(spv, sizeof spv), "24B valid SPIR-V");
    CHECK("spv header rejects short/unaligned/bad-magic",
          !rdna4ba_spv_header_ok(NULL, 24) &&
          !rdna4ba_spv_header_ok(spv, 16) &&
          !rdna4ba_spv_header_ok(spv, 22) &&
          (spv[0] ^= 1, !rdna4ba_spv_header_ok(spv, sizeof spv)),
          "negative SPIR-V checks");
}

static void test_empty_roundtrip(const char *tmp_dir) {
    rdna4ba_fingerprint_t fp = make_fp();
    rdna4ba_archive_t a, b;
    rdna4ba_init(&a, &fp);

    uint8_t *wire = NULL;
    size_t wire_len = 0;
    rdna4ba_status_t st = rdna4ba_serialize(&a, &wire, &wire_len);
    CHECK("empty serialize", st == RDNA4BA_OK && wire != NULL && wire_len == sizeof(rdna4ba_header_t),
          "st=%s len=%zu", rdna4ba_status_str(st), wire_len);

    st = rdna4ba_deserialize(wire, wire_len, &fp, &b);
    CHECK("empty deserialize", st == RDNA4BA_OK && b.count == 0 && b.vk_cache_size == 0,
          "st=%s count=%u", rdna4ba_status_str(st), b.count);
    rdna4ba_destroy(&b);
    free(wire);

    char path[512];
    snprintf(path, sizeof path, "%s/empty.grba", tmp_dir);
    int io_err = 0;
    st = rdna4ba_write_file_atomic(&a, path, &io_err);
    CHECK("empty atomic file write", st == RDNA4BA_OK && io_err == 0, "st=%s errno=%d", rdna4ba_status_str(st), io_err);
    st = rdna4ba_read_file(path, &fp, &b, &io_err);
    CHECK("empty file read", st == RDNA4BA_OK && b.count == 0, "st=%s count=%u", rdna4ba_status_str(st), b.count);
    rdna4ba_destroy(&b);
    rdna4ba_destroy(&a);
    unlink(path);
}

static void test_roundtrip_lookup_update_and_collision(const char *tmp_dir) {
    rdna4ba_fingerprint_t fp = make_fp();
    rdna4ba_archive_t a;
    rdna4ba_init(&a, &fp);

    uint8_t arch_vk[64];
    for (size_t i = 0; i < sizeof arch_vk; i++) arch_vk[i] = (uint8_t)(0x90 + i);
    CHECK("set archive vk_cache", rdna4ba_set_vk_cache(&a, arch_vk, sizeof arch_vk) == RDNA4BA_OK, "64 B");

    // 1. Compute entry
    uint8_t k_comp[32]; uint64_t u_comp = 0;
    make_key("compute:add_u32", k_comp, &u_comp);
    uint8_t comp_spv[128]; fill_spv(comp_spv, sizeof comp_spv, 11);
    const char comp_meta[] = "{\"kernel_dispatch\":\"Workgroups\",\"local_size\":[64,1,1]}";
    uint8_t comp_vk[48]; memset(comp_vk, 0x3C, sizeof comp_vk);
    rdna4ba_entry_view_t ev;
    memset(&ev, 0, sizeof ev);
    ev.kind = RDNA4BA_KIND_COMPUTE; ev.key_u64 = u_comp; memcpy(ev.key_sha, k_comp, 32);
    ev.spv0 = comp_spv; ev.spv0_size = sizeof comp_spv;
    ev.meta0 = comp_meta; ev.meta0_size = (uint32_t)strlen(comp_meta);
    ev.vk_cache = comp_vk; ev.vk_cache_size = sizeof comp_vk;
    CHECK("put compute", rdna4ba_put(&a, &ev) == RDNA4BA_OK && a.count == 1, "count=%u", a.count);

    // 2. Render entry (vertex + fragment + vk_cache)
    uint8_t k_rend[32]; uint64_t u_rend = 0;
    make_key("render:tri_rgba8", k_rend, &u_rend);
    uint8_t vs_spv[96], fs_spv[112];
    fill_spv(vs_spv, sizeof vs_spv, 22);
    fill_spv(fs_spv, sizeof fs_spv, 33);
    const char vs_meta[] = "{\"stage\":\"Vertex\"}";
    const char fs_meta[] = "{\"stage\":\"Fragment\"}";
    uint8_t rend_vk[80]; memset(rend_vk, 0x5A, sizeof rend_vk);
    memset(&ev, 0, sizeof ev);
    ev.kind = RDNA4BA_KIND_RENDER; ev.key_u64 = u_rend; memcpy(ev.key_sha, k_rend, 32);
    ev.spv0 = vs_spv; ev.spv0_size = sizeof vs_spv;
    ev.meta0 = vs_meta; ev.meta0_size = (uint32_t)strlen(vs_meta);
    ev.spv1 = fs_spv; ev.spv1_size = sizeof fs_spv;
    ev.meta1 = fs_meta; ev.meta1_size = (uint32_t)strlen(fs_meta);
    ev.vk_cache = rend_vk; ev.vk_cache_size = sizeof rend_vk;
    CHECK("put render", rdna4ba_put(&a, &ev) == RDNA4BA_OK && a.count == 2, "count=%u", a.count);

    // 3. Vertex-only render entry (no spv1/meta1)
    uint8_t k_vonly[32]; uint64_t u_vonly = 0;
    make_key("render:vertex_only", k_vonly, &u_vonly);
    memset(&ev, 0, sizeof ev);
    ev.kind = RDNA4BA_KIND_RENDER; ev.key_u64 = u_vonly; memcpy(ev.key_sha, k_vonly, 32);
    ev.spv0 = vs_spv; ev.spv0_size = sizeof vs_spv;
    CHECK("put vertex-only render", rdna4ba_put(&a, &ev) == RDNA4BA_OK && a.count == 3, "count=%u", a.count);

    // 4. Standalone function entry + same SHA as compute (different kind -> distinct entry!)
    memset(&ev, 0, sizeof ev);
    ev.kind = RDNA4BA_KIND_FUNCTION; ev.key_u64 = u_comp; memcpy(ev.key_sha, k_comp, 32);
    ev.spv0 = comp_spv; ev.spv0_size = sizeof comp_spv;
    CHECK("put function with same sha as compute", rdna4ba_put(&a, &ev) == RDNA4BA_OK && a.count == 4, "count=%u", a.count);

    // 5. Forced 64-bit key_u64 collision with distinct 256-bit key_sha
    uint8_t k_coll[32];
    memcpy(k_coll, k_comp, 32);
    k_coll[31] ^= 0xFFu;
    uint8_t coll_spv[64]; fill_spv(coll_spv, sizeof coll_spv, 77);
    memset(&ev, 0, sizeof ev);
    ev.kind = RDNA4BA_KIND_COMPUTE; ev.key_u64 = u_comp; memcpy(ev.key_sha, k_coll, 32);
    ev.spv0 = coll_spv; ev.spv0_size = sizeof coll_spv;
    CHECK("put 64-bit collision distinct sha256", rdna4ba_put(&a, &ev) == RDNA4BA_OK && a.count == 5, "count=%u", a.count);

    // 6. Duplicate key update (replace compute entry in place with new SPIR-V & no vk_cache)
    uint8_t comp_spv2[160]; fill_spv(comp_spv2, sizeof comp_spv2, 99);
    const char comp_meta2[] = "{\"kernel_dispatch\":\"Workgroups\",\"local_size\":[32,1,1]}";
    memset(&ev, 0, sizeof ev);
    ev.kind = RDNA4BA_KIND_COMPUTE; ev.key_u64 = u_comp; memcpy(ev.key_sha, k_comp, 32);
    ev.spv0 = comp_spv2; ev.spv0_size = sizeof comp_spv2;
    ev.meta0 = comp_meta2; ev.meta0_size = (uint32_t)strlen(comp_meta2);
    CHECK("duplicate key update keeps count", rdna4ba_put(&a, &ev) == RDNA4BA_OK && a.count == 5, "count=%u", a.count);

    // Atomic file write & reload
    char path[512];
    snprintf(path, sizeof path, "%s/roundtrip.grba", tmp_dir);
    int io_err = 0;
    rdna4ba_status_t st = rdna4ba_write_file_atomic(&a, path, &io_err);
    CHECK("atomic write roundtrip.grba", st == RDNA4BA_OK && io_err == 0, "st=%s", rdna4ba_status_str(st));

    rdna4ba_archive_t r;
    st = rdna4ba_read_file(path, &fp, &r, &io_err);
    CHECK("reload roundtrip.grba", st == RDNA4BA_OK && r.count == 5 &&
          r.vk_cache_size == sizeof arch_vk && memcmp(r.vk_cache, arch_vk, sizeof arch_vk) == 0,
          "st=%s count=%u vk=%zu", rdna4ba_status_str(st), r.count, r.vk_cache_size);

    // Verify hits, updated payload, render slices, collision separation, and misses
    const rdna4ba_entry_t *ec = rdna4ba_find_by_sha(&r, RDNA4BA_KIND_COMPUTE, k_comp);
    rdna4ba_entry_view_t vc = rdna4ba_entry_view(ec);
    CHECK("updated compute hit exact bytes",
          ec != NULL && vc.spv0_size == sizeof comp_spv2 && memcmp(vc.spv0, comp_spv2, sizeof comp_spv2) == 0 &&
          vc.meta0_size == strlen(comp_meta2) && memcmp(vc.meta0, comp_meta2, strlen(comp_meta2)) == 0 &&
          vc.vk_cache_size == 0 && (vc.flags & RDNA4BA_ENTRY_F_VK_CACHE) == 0,
          "spv=%u meta=%u vk=%u flags=0x%x", vc.spv0_size, vc.meta0_size, vc.vk_cache_size, vc.flags);

    const rdna4ba_entry_t *ec2 = rdna4ba_find(&r, RDNA4BA_KIND_COMPUTE, u_comp, k_coll);
    rdna4ba_entry_view_t vc2 = rdna4ba_entry_view(ec2);
    CHECK("collision entry separated by sha256",
          ec2 != NULL && vc2.spv0_size == sizeof coll_spv && memcmp(vc2.spv0, coll_spv, sizeof coll_spv) == 0,
          "spv=%u", vc2.spv0_size);

    const rdna4ba_entry_t *er = rdna4ba_find_by_sha(&r, RDNA4BA_KIND_RENDER, k_rend);
    rdna4ba_entry_view_t vr = rdna4ba_entry_view(er);
    CHECK("render hit all 5 slices exact",
          er != NULL && (vr.flags == (RDNA4BA_ENTRY_F_SECONDARY | RDNA4BA_ENTRY_F_VK_CACHE)) &&
          vr.spv0_size == sizeof vs_spv && memcmp(vr.spv0, vs_spv, sizeof vs_spv) == 0 &&
          vr.meta0_size == strlen(vs_meta) && memcmp(vr.meta0, vs_meta, strlen(vs_meta)) == 0 &&
          vr.spv1_size == sizeof fs_spv && memcmp(vr.spv1, fs_spv, sizeof fs_spv) == 0 &&
          vr.meta1_size == strlen(fs_meta) && memcmp(vr.meta1, fs_meta, strlen(fs_meta)) == 0 &&
          vr.vk_cache_size == sizeof rend_vk && memcmp(vr.vk_cache, rend_vk, sizeof rend_vk) == 0,
          "flags=0x%x vs=%u fs=%u vk=%u", vr.flags, vr.spv0_size, vr.spv1_size, vr.vk_cache_size);

    const rdna4ba_entry_t *evo = rdna4ba_find_by_sha(&r, RDNA4BA_KIND_RENDER, k_vonly);
    rdna4ba_entry_view_t vvo = rdna4ba_entry_view(evo);
    CHECK("vertex-only render hit", evo != NULL && vvo.spv0_size == sizeof vs_spv && vvo.spv1_size == 0 &&
          (vvo.flags & RDNA4BA_ENTRY_F_SECONDARY) == 0, "flags=0x%x", vvo.flags);

    const rdna4ba_entry_t *ef = rdna4ba_find_by_sha(&r, RDNA4BA_KIND_FUNCTION, k_comp);
    CHECK("function hit distinct from compute", ef != NULL && ef->spv0_size == sizeof comp_spv, "spv=%u", ef ? ef->spv0_size : 0);

    // Miss + FailOnBinaryArchiveMiss behavior across archive list
    uint8_t k_miss[32]; uint64_t u_miss = 0;
    make_key("compute:never_added", k_miss, &u_miss);
    CHECK("miss unknown key", rdna4ba_find(&r, RDNA4BA_KIND_COMPUTE, u_miss, k_miss) == NULL, "returns NULL");
    CHECK("miss wrong kind", rdna4ba_find(&r, RDNA4BA_KIND_RENDER, u_comp, k_comp) == NULL, "compute key not render");

    rdna4ba_archive_t empty_arch;
    rdna4ba_init(&empty_arch, &fp);
    const rdna4ba_archive_t *list[2] = { &empty_arch, &r };
    const rdna4ba_entry_t *hit = NULL;
    for (int i = 0; i < 2 && !hit; i++) hit = rdna4ba_find_by_sha(list[i], RDNA4BA_KIND_COMPUTE, k_comp);
    CHECK("multi-archive search hits 2nd archive", hit != NULL, "found in list[1]");
    hit = NULL;
    for (int i = 0; i < 2 && !hit; i++) hit = rdna4ba_find_by_sha(list[i], RDNA4BA_KIND_COMPUTE, k_miss);
    CHECK("fail-on-miss sees NULL across all archives", hit == NULL, "triggers FailOnBinaryArchiveMiss");

    rdna4ba_destroy(&empty_arch);
    rdna4ba_destroy(&r);
    rdna4ba_destroy(&a);
    unlink(path);
}

static void recompute_archive_crc(uint8_t *wire, size_t len) {
    uint32_t crc = rdna4ba_compute_archive_crc(wire, len);
    rdna4ba_put_u32(wire + offsetof(rdna4ba_header_t, archive_crc32), crc);
}

static void test_corruption_and_rejection(const char *tmp_dir) {
    rdna4ba_fingerprint_t fp = make_fp();
    rdna4ba_archive_t a;
    rdna4ba_init(&a, &fp);

    uint8_t arch_vk[32]; memset(arch_vk, 0x77, sizeof arch_vk);
    CHECK("setup vk_cache", rdna4ba_set_vk_cache(&a, arch_vk, sizeof arch_vk) == RDNA4BA_OK, "ok");

    uint8_t k1[32], k2[32]; uint64_t u1 = 0, u2 = 0;
    make_key("comp:k1", k1, &u1);
    make_key("comp:k2", k2, &u2);
    uint8_t spv[64]; fill_spv(spv, sizeof spv, 5);
    rdna4ba_entry_view_t ev;
    memset(&ev, 0, sizeof ev);
    ev.kind = RDNA4BA_KIND_COMPUTE; ev.key_u64 = u1; memcpy(ev.key_sha, k1, 32);
    ev.spv0 = spv; ev.spv0_size = sizeof spv;
    CHECK("setup entry 1", rdna4ba_put(&a, &ev) == RDNA4BA_OK, "ok");
    ev.key_u64 = u2; memcpy(ev.key_sha, k2, 32);
    CHECK("setup entry 2", rdna4ba_put(&a, &ev) == RDNA4BA_OK, "ok");

    uint8_t *orig = NULL; size_t olen = 0;
    CHECK("setup serialize", rdna4ba_serialize(&a, &orig, &olen) == RDNA4BA_OK, "len=%zu", olen);

    rdna4ba_archive_t out;
    memset(&out, 0, sizeof out);

    // 1. Truncated & trailing bytes
    CHECK("reject NULL/0", rdna4ba_deserialize(NULL, olen, &fp, &out) == RDNA4BA_ERR_INVALID &&
                           rdna4ba_deserialize(orig, 0, &fp, &out) == RDNA4BA_ERR_TRUNCATED, "null/empty");
    CHECK("reject header-1", rdna4ba_deserialize(orig, sizeof(rdna4ba_header_t) - 1, &fp, &out) == RDNA4BA_ERR_TRUNCATED, "127 B");
    CHECK("reject truncated by 1", rdna4ba_deserialize(orig, olen - 1, &fp, &out) == RDNA4BA_ERR_TRUNCATED, "olen-1");

    uint8_t *extra = (uint8_t *)malloc(olen + 1);
    memcpy(extra, orig, olen); extra[olen] = 0;
    CHECK("reject trailing byte", rdna4ba_deserialize(extra, olen + 1, &fp, &out) == RDNA4BA_ERR_BOUNDS, "olen+1");
    free(extra);

    uint8_t *mut = (uint8_t *)malloc(olen);

    // 2. Wrong magic (Apple MTLB metallib)
    memcpy(mut, orig, olen);
    memcpy(mut, "MTLB", 4);
    CHECK("reject MTLB magic", rdna4ba_deserialize(mut, olen, &fp, &out) == RDNA4BA_ERR_MAGIC, "foreign metallib");

    // 3. Version mismatch
    memcpy(mut, orig, olen);
    rdna4ba_put_u32(mut + 4, RDNA4BA_FORMAT_VERSION + 1u);
    CHECK("reject version 2", rdna4ba_deserialize(mut, olen, &fp, &out) == RDNA4BA_ERR_VERSION, "v2");
    rdna4ba_put_u32(mut + 4, 0u);
    CHECK("reject version 0", rdna4ba_deserialize(mut, olen, &fp, &out) == RDNA4BA_ERR_VERSION, "v0");

    // 4. Malformed header fields
    memcpy(mut, orig, olen); rdna4ba_put_u32(mut + 8, 64u);
    CHECK("reject bad header_size", rdna4ba_deserialize(mut, olen, &fp, &out) == RDNA4BA_ERR_HEADER, "hdr_size=64");
    memcpy(mut, orig, olen); rdna4ba_put_u32(mut + 12, 1u);
    CHECK("reject unknown header flags", rdna4ba_deserialize(mut, olen, &fp, &out) == RDNA4BA_ERR_HEADER, "flags=1");
    memcpy(mut, orig, olen); rdna4ba_put_u32(mut + 92, 44u);
    CHECK("reject bad entry_size", rdna4ba_deserialize(mut, olen, &fp, &out) == RDNA4BA_ERR_HEADER, "entry_size=44");
    memcpy(mut, orig, olen); rdna4ba_put_u32(mut + 88, RDNA4BA_MAX_ENTRIES + 1u);
    CHECK("reject entry_count > max", rdna4ba_deserialize(mut, olen, &fp, &out) == RDNA4BA_ERR_BOUNDS, "count>max");

    // 5. Whole-archive CRC tamper (header bit flip, payload bit flip)
    memcpy(mut, orig, olen); mut[28] ^= 0x01u; // flip device_id without updating CRC
    CHECK("reject header tamper", rdna4ba_deserialize(mut, olen, &fp, &out) == RDNA4BA_ERR_ARCHIVE_CRC, "flipped hdr bit");
    memcpy(mut, orig, olen); mut[olen - 5] ^= 0x80u;
    CHECK("reject payload tamper", rdna4ba_deserialize(mut, olen, &fp, &out) == RDNA4BA_ERR_ARCHIVE_CRC, "flipped payload bit");

    // 6. Per-entry CRC tamper (recompute archive CRC, leave entry payload_crc32 stale)
    memcpy(mut, orig, olen);
    mut[olen - 3] ^= 0x40u;
    recompute_archive_crc(mut, olen);
    CHECK("reject per-entry CRC mismatch", rdna4ba_deserialize(mut, olen, &fp, &out) == RDNA4BA_ERR_ENTRY_CRC, "entry CRC caught");

    // 7. Archive vk_cache CRC tamper (recompute archive CRC, leave vk_cache_crc32 stale)
    memcpy(mut, orig, olen);
    uint64_t vk_off = rdna4ba_get_u64(mut + 104);
    mut[vk_off + 4] ^= 0x10u;
    recompute_archive_crc(mut, olen);
    CHECK("reject archive vk_cache CRC mismatch", rdna4ba_deserialize(mut, olen, &fp, &out) == RDNA4BA_ERR_ARCHIVE_CRC, "vk_cache CRC caught");

    // 8. ABI & Device fingerprint mismatches (valid CRC, different expected fingerprint)
    rdna4ba_fingerprint_t bad_fp = fp;
    bad_fp.garnet_abi_version = 99u;
    CHECK("reject garnet ABI mismatch", rdna4ba_deserialize(orig, olen, &bad_fp, &out) == RDNA4BA_ERR_ABI_MISMATCH, "garnet_abi");
    bad_fp = fp; bad_fp.translator_abi_version = 2u;
    CHECK("reject translator ABI mismatch", rdna4ba_deserialize(orig, olen, &bad_fp, &out) == RDNA4BA_ERR_ABI_MISMATCH, "xlate_abi");
    bad_fp = fp; bad_fp.vendor_id = 0x10DEu;
    CHECK("reject vendor mismatch", rdna4ba_deserialize(orig, olen, &bad_fp, &out) == RDNA4BA_ERR_DEVICE_MISMATCH, "vendor");
    bad_fp = fp; bad_fp.device_id = 0x7550u; // Navi 48 vs Navi 44
    CHECK("reject PCI device mismatch", rdna4ba_deserialize(orig, olen, &bad_fp, &out) == RDNA4BA_ERR_DEVICE_MISMATCH, "device_id");
    bad_fp = fp; bad_fp.driver_version ^= 1u;
    CHECK("reject driver version mismatch", rdna4ba_deserialize(orig, olen, &bad_fp, &out) == RDNA4BA_ERR_DEVICE_MISMATCH, "driver");
    bad_fp = fp; bad_fp.flags ^= 1u;
    CHECK("reject fp flags mismatch", rdna4ba_deserialize(orig, olen, &bad_fp, &out) == RDNA4BA_ERR_DEVICE_MISMATCH, "fp.flags");
    bad_fp = fp; bad_fp.pipeline_cache_uuid[7] ^= 1u;
    CHECK("reject RADV UUID mismatch", rdna4ba_deserialize(orig, olen, &bad_fp, &out) == RDNA4BA_ERR_DEVICE_MISMATCH, "uuid");
    bad_fp = fp; bad_fp.translator_hash[15] ^= 1u;
    CHECK("reject translator hash mismatch", rdna4ba_deserialize(orig, olen, &bad_fp, &out) == RDNA4BA_ERR_DEVICE_MISMATCH, "xlate_hash");

    // 9. Crafted malformed entry table (with valid archive CRC)
    // 9a. Invalid kind
    memcpy(mut, orig, olen);
    rdna4ba_put_u32(mut + sizeof(rdna4ba_header_t) + 0, 99u);
    recompute_archive_crc(mut, olen);
    CHECK("reject invalid entry kind", rdna4ba_deserialize(mut, olen, &fp, &out) == RDNA4BA_ERR_HEADER, "kind=99");

    // 9b. Compute entry claiming secondary stage
    memcpy(mut, orig, olen);
    rdna4ba_put_u32(mut + sizeof(rdna4ba_header_t) + 72, 16u);
    recompute_archive_crc(mut, olen);
    CHECK("reject compute with spv1", rdna4ba_deserialize(mut, olen, &fp, &out) == RDNA4BA_ERR_HEADER, "compute spv1");

    // 9c. Payload sub-slice sum mismatch
    memcpy(mut, orig, olen);
    rdna4ba_put_u32(mut + sizeof(rdna4ba_header_t) + 64, 32u); // spv0_size != payload_size
    recompute_archive_crc(mut, olen);
    CHECK("reject sub-slice sum mismatch", rdna4ba_deserialize(mut, olen, &fp, &out) == RDNA4BA_ERR_BOUNDS, "sum!=payload_size");

    // 9d. Overlapping payload_offset on entry 1
    memcpy(mut, orig, olen);
    uint64_t off0 = rdna4ba_get_u64(mut + sizeof(rdna4ba_header_t) + 48);
    rdna4ba_put_u64(mut + sizeof(rdna4ba_header_t) + sizeof(rdna4ba_entry_disk_t) + 48, off0);
    recompute_archive_crc(mut, olen);
    CHECK("reject overlapping payload_offset", rdna4ba_deserialize(mut, olen, &fp, &out) == RDNA4BA_ERR_BOUNDS, "overlap");

    // 9e. Duplicate key in deserialized wire buffer
    memcpy(mut, orig, olen);
    uint8_t *e1 = mut + sizeof(rdna4ba_header_t) + sizeof(rdna4ba_entry_disk_t);
    rdna4ba_put_u64(e1 + 8, u1);
    memcpy(e1 + 16, k1, 32);
    recompute_archive_crc(mut, olen);
    CHECK("reject duplicate wire key", rdna4ba_deserialize(mut, olen, &fp, &out) == RDNA4BA_ERR_DUPLICATE, "dup key");

    // 10. File I/O negative cases (missing file, directory, invalid args)
    int io_err = 0;
    char missing[512];
    snprintf(missing, sizeof missing, "%s/does-not-exist.grba", tmp_dir);
    CHECK("reject missing file", rdna4ba_read_file(missing, &fp, &out, &io_err) == RDNA4BA_ERR_IO && io_err == ENOENT,
          "errno=%d", io_err);
    CHECK("reject directory as archive", rdna4ba_read_file(tmp_dir, &fp, &out, &io_err) == RDNA4BA_ERR_INVALID,
          "dir rejected");
    char bad_write[512];
    snprintf(bad_write, sizeof bad_write, "%s/no-such-subdir/out.grba", tmp_dir);
    CHECK("reject unwritable parent dir", rdna4ba_write_file_atomic(&a, bad_write, &io_err) == RDNA4BA_ERR_IO,
          "errno=%d", io_err);

    free(mut);
    free(orig);
    rdna4ba_destroy(&a);
}

static void test_large_payload_and_bounds(void) {
    rdna4ba_fingerprint_t fp = make_fp();
    rdna4ba_archive_t a;
    rdna4ba_init(&a, &fp);

    // 2 MiB SPIR-V + 64 KiB meta + 512 KiB vk_cache round-trip
    const size_t spv_sz = 2u << 20;
    const size_t meta_sz = 64u << 10;
    const size_t vk_sz = 512u << 10;
    uint8_t *spv = (uint8_t *)malloc(spv_sz);
    uint8_t *meta = (uint8_t *)malloc(meta_sz);
    uint8_t *vk = (uint8_t *)malloc(vk_sz);
    fill_spv(spv, spv_sz, 123);
    memset(meta, 'm', meta_sz);
    memset(vk, 0xEE, vk_sz);

    uint8_t key[32]; uint64_t ku64 = 0;
    make_key("compute:large_2mib", key, &ku64);
    rdna4ba_entry_view_t ev;
    memset(&ev, 0, sizeof ev);
    ev.kind = RDNA4BA_KIND_COMPUTE; ev.key_u64 = ku64; memcpy(ev.key_sha, key, 32);
    ev.spv0 = spv; ev.spv0_size = (uint32_t)spv_sz;
    ev.meta0 = meta; ev.meta0_size = (uint32_t)meta_sz;
    ev.vk_cache = vk; ev.vk_cache_size = (uint32_t)vk_sz;
    CHECK("put 2.5+ MiB entry", rdna4ba_put(&a, &ev) == RDNA4BA_OK, "ok");

    uint8_t *wire = NULL; size_t wlen = 0;
    CHECK("serialize large archive", rdna4ba_serialize(&a, &wire, &wlen) == RDNA4BA_OK && wlen > spv_sz + meta_sz + vk_sz,
          "len=%zu", wlen);
    rdna4ba_archive_t r;
    CHECK("deserialize large archive", rdna4ba_deserialize(wire, wlen, &fp, &r) == RDNA4BA_OK && r.count == 1, "ok");
    const rdna4ba_entry_t *e = rdna4ba_find_by_sha(&r, RDNA4BA_KIND_COMPUTE, key);
    rdna4ba_entry_view_t v = rdna4ba_entry_view(e);
    CHECK("large entry bit-exact",
          e != NULL && v.spv0_size == spv_sz && memcmp(v.spv0, spv, spv_sz) == 0 &&
          v.meta0_size == meta_sz && memcmp(v.meta0, meta, meta_sz) == 0 &&
          v.vk_cache_size == vk_sz && memcmp(v.vk_cache, vk, vk_sz) == 0, "2.56 MiB verified");

    // Bounds enforcement: over RDNA4BA_MAX_ENTRY_PAYLOAD rejected without allocation
    ev.spv0_size = (uint32_t)RDNA4BA_MAX_ENTRY_PAYLOAD;
    ev.meta0_size = 1u;
    ev.vk_cache_size = 0u;
    CHECK("reject entry > MAX_ENTRY_PAYLOAD", rdna4ba_put(&a, &ev) == RDNA4BA_ERR_BOUNDS, ">32 MiB rejected");

    // Invalid compute with secondary stage rejected in rdna4ba_put
    ev.spv0_size = 64u; ev.meta0_size = 0u; ev.spv1 = spv; ev.spv1_size = 64u;
    CHECK("reject compute with secondary stage", rdna4ba_put(&a, &ev) == RDNA4BA_ERR_INVALID, "invalid stage");

    // Oversized archive vk_cache rejected
    CHECK("reject archive vk_cache > MAX_ENTRY_PAYLOAD",
          rdna4ba_set_vk_cache(&a, spv, (size_t)RDNA4BA_MAX_ENTRY_PAYLOAD + 1u) == RDNA4BA_ERR_BOUNDS, ">32 MiB vk_cache");

    // Oversized deserialize buffer length rejected immediately
    rdna4ba_archive_t tmp;
    CHECK("reject deserialize > MAX_ARCHIVE_BYTES",
          rdna4ba_deserialize(wire, (size_t)RDNA4BA_MAX_ARCHIVE_BYTES + 1u, &fp, &tmp) == RDNA4BA_ERR_BOUNDS,
          ">256 MiB rejected");

    free(wire);
    free(spv); free(meta); free(vk);
    rdna4ba_destroy(&r);
    rdna4ba_destroy(&a);
}

int main(void) {
    const char *env_tmp = getenv("TMPDIR");
    char dir_tmpl[512];
    snprintf(dir_tmpl, sizeof dir_tmpl, "%s/rdna4ba-test.XXXXXX", (env_tmp && env_tmp[0]) ? env_tmp : "/tmp");
    char *tmp_dir = mkdtemp(dir_tmpl);
    CHECK("create scratch dir", tmp_dir != NULL, "%s", tmp_dir ? tmp_dir : "(failed)");
    if (!tmp_dir) return 1;

    test_sha256_and_helpers();
    test_empty_roundtrip(tmp_dir);
    test_roundtrip_lookup_update_and_collision(tmp_dir);
    test_corruption_and_rejection(tmp_dir);
    test_large_payload_and_bounds();

    (void)rmdir(tmp_dir);
    printf("SUMMARY: %d checks, %d failed\n", checks, fails);
    return fails != 0 ? 1 : 0;
}
