// test-dynlib-vft.c: Pure-C host verification suite for Metal 3 dynamic libraries (Task 3.2,
// '.grdl' container + Stage 2 SHA + exported symbol JSON) and visible function tables (Task 3.3,
// patch 0074 buffer layout + 'visible_function_tables' JSON parser + Footnote 10 exclusion +
// runtime source pins).
//
// Build & run on Linux or macOS:
//   cc -std=c11 -D_GNU_SOURCE -O2 -Wall -Wextra -Wshadow -Wsign-conversion -Werror -Isrc/garnet-metal src/garnet-metal/test-dynlib-vft.c -o test-dynlib-vft
//   ./test-dynlib-vft src/garnet-metal

#include "rdna4_dynlib_vft.h"

#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static int g_pass = 0;
static int g_fail = 0;

#define CHECK(cond, msg) do {                                                        \
    if (cond) {                                                                      \
        g_pass++;                                                                    \
    } else {                                                                         \
        fprintf(stderr, "FAIL [%s:%d]: %s\n", __FILE__, __LINE__, (msg));            \
        g_fail++;                                                                    \
    }                                                                                \
} while (0)

static void recompute_grdl_file_crc(uint8_t *buf, size_t len) {
    uint32_t fcrc = rdna4dl_compute_file_crc(buf, len);
    rdna4dl_put_u32(buf + offsetof(rdna4dl_header_t, file_crc32), fcrc);
}

static void test_grdl_roundtrip(void) {
    // 1. Single-module AIR bitcode library
    const uint8_t air_mod[] = { 'B', 'C', 0xc0, 0xde, 0x11, 0x22, 0x33, 0x44, 0x55, 0x66 };
    rdna4dl_container_t c;
    rdna4dl_init(&c);
    CHECK(rdna4dl_set_install_name(&c, "@executable_path/libmath.metallib", 33) == RDNA4DL_OK,
          "set install name");
    CHECK(rdna4dl_add_symbol(&c, "scale_vec4") == RDNA4DL_OK, "add symbol 1");
    CHECK(rdna4dl_add_symbol(&c, "bias_vec4") == RDNA4DL_OK, "add symbol 2");
    CHECK(rdna4dl_add_symbol(&c, "scale_vec4") == RDNA4DL_OK, "dedup symbol on add");
    CHECK(c.symbol_count == 2u, "dedup keeps symbol count == 2");
    CHECK(rdna4dl_add_symbol_len(&c, "bias_vec4", 9, 1) == RDNA4DL_ERR_DUPLICATE,
          "reject_duplicate catches duplicate symbol");
    CHECK(rdna4dl_add_module(&c, RDNA4DL_PAYLOAD_AIR_BITCODE, air_mod, sizeof(air_mod)) == RDNA4DL_OK,
          "add AIR module");

    uint8_t *wire = NULL;
    size_t wire_len = 0;
    CHECK(rdna4dl_serialize(&c, &wire, &wire_len) == RDNA4DL_OK, "serialize single-module GRDL");
    CHECK(wire != NULL && wire_len > sizeof(rdna4dl_header_t), "wire buffer non-empty");
    CHECK(memcmp(wire, "GRDL", 4) == 0, "GRDL magic at byte 0");

    rdna4dl_container_t out;
    rdna4dl_init(&out);
    CHECK(rdna4dl_deserialize(wire, wire_len, &out) == RDNA4DL_OK, "deserialize single-module GRDL");
    CHECK(strcmp(out.install_name, "@executable_path/libmath.metallib") == 0, "install_name matches");
    CHECK(out.symbol_count == 2u, "symbol_count round-trip");
    CHECK(rdna4dl_has_symbol(&out, "scale_vec4") == 1, "has scale_vec4");
    CHECK(rdna4dl_has_symbol(&out, "bias_vec4") == 1, "has bias_vec4");
    CHECK(rdna4dl_has_symbol(&out, "missing_fn") == 0, "missing symbol returns 0");
    CHECK(out.module_count == 1u, "module_count round-trip");
    CHECK(out.modules[0].kind == RDNA4DL_PAYLOAD_AIR_BITCODE, "module kind round-trip");
    CHECK(out.modules[0].size == sizeof(air_mod) &&
          memcmp(out.modules[0].data, air_mod, sizeof(air_mod)) == 0, "module bytes round-trip");
    CHECK(rdna4dl_is_bitcode_magic(out.modules[0].data, out.modules[0].size) == 1, "bitcode magic detect");
    rdna4dl_destroy(&out);
    free(wire);
    rdna4dl_destroy(&c);

    // 2. Multi-module mixed payload + 0 symbols
    const char *ir_mod = "; ModuleID = 'dyn'\ndefine float @helper(float %x) { ret float %x }\n";
    const uint8_t mtlb_mod[] = { 'M', 'T', 'L', 'B', 0x01, 0x00, 0x02, 0x00 };
    const uint8_t bc_wrap[] = { 0xde, 0xc0, 0x17, 0x0b, 0xaa, 0xbb };
    CHECK(rdna4dl_is_metallib_magic(mtlb_mod, sizeof(mtlb_mod)) == 1, "MTLB magic detect");
    CHECK(rdna4dl_is_bitcode_magic(bc_wrap, sizeof(bc_wrap)) == 1, "bitcode wrapper magic detect");

    rdna4dl_init(&c);
    CHECK(rdna4dl_set_install_name(&c, "@rpath/libmixed.grdl", 20) == RDNA4DL_OK, "set mixed install name");
    CHECK(rdna4dl_add_module(&c, RDNA4DL_PAYLOAD_LLVM_IR, ir_mod, strlen(ir_mod)) == RDNA4DL_OK, "add IR mod");
    CHECK(rdna4dl_add_module(&c, RDNA4DL_PAYLOAD_METALLIB, mtlb_mod, sizeof(mtlb_mod)) == RDNA4DL_OK, "add MTLB mod");
    CHECK(rdna4dl_add_module(&c, RDNA4DL_PAYLOAD_AIR_BITCODE, bc_wrap, sizeof(bc_wrap)) == RDNA4DL_OK, "add wrap mod");

    CHECK(rdna4dl_serialize(&c, &wire, &wire_len) == RDNA4DL_OK, "serialize mixed 0-symbol GRDL");
    rdna4dl_header_t hdr;
    rdna4dl_decode_header(wire, &hdr);
    CHECK(hdr.payload_kind == RDNA4DL_PAYLOAD_LLVM_IR, "primary payload_kind in header");
    CHECK(hdr.symbol_count == 0u && hdr.module_count == 3u, "header counts for mixed GRDL");

    rdna4dl_init(&out);
    CHECK(rdna4dl_deserialize(wire, wire_len, &out) == RDNA4DL_OK, "deserialize mixed 0-symbol GRDL");
    CHECK(out.symbol_count == 0u && out.module_count == 3u, "mixed counts round-trip");
    CHECK(out.modules[0].kind == RDNA4DL_PAYLOAD_LLVM_IR, "mod 0 kind");
    CHECK(out.modules[1].kind == RDNA4DL_PAYLOAD_METALLIB, "mod 1 kind");
    CHECK(out.modules[2].kind == RDNA4DL_PAYLOAD_AIR_BITCODE, "mod 2 kind");
    rdna4dl_destroy(&out);
    free(wire);
    rdna4dl_destroy(&c);
}

static void test_grdl_tamper_and_truncation(void) {
    const uint8_t payload[] = { 'B', 'C', 0xc0, 0xde, 1, 2, 3, 4, 5, 6, 7, 8 };
    rdna4dl_container_t c;
    rdna4dl_init(&c);
    CHECK(rdna4dl_set_install_name(&c, "@rpath/libsec.grdl", 18) == RDNA4DL_OK, "sec install name");
    CHECK(rdna4dl_add_symbol(&c, "sym_one") == RDNA4DL_OK, "sec sym 1");
    CHECK(rdna4dl_add_symbol(&c, "sym_two") == RDNA4DL_OK, "sec sym 2");
    CHECK(rdna4dl_add_module(&c, RDNA4DL_PAYLOAD_AIR_BITCODE, payload, sizeof(payload)) == RDNA4DL_OK, "sec mod");

    uint8_t *wire = NULL;
    size_t wire_len = 0;
    CHECK(rdna4dl_serialize(&c, &wire, &wire_len) == RDNA4DL_OK, "serialize baseline");

    rdna4dl_container_t out;
    rdna4dl_init(&out);

    // Truncated header
    CHECK(rdna4dl_deserialize(wire, 12, &out) == RDNA4DL_ERR_TRUNCATED, "truncated header rejected");
    CHECK(rdna4dl_deserialize(wire, sizeof(rdna4dl_header_t) - 1u, &out) == RDNA4DL_ERR_TRUNCATED,
          "63-byte header rejected");
    // Truncated body
    CHECK(rdna4dl_deserialize(wire, wire_len - 1u, &out) == RDNA4DL_ERR_TRUNCATED, "truncated body rejected");

    // Trailing garbage
    uint8_t *grown = (uint8_t *)malloc(wire_len + 1u);
    CHECK(grown != NULL, "alloc grown");
    memcpy(grown, wire, wire_len);
    grown[wire_len] = 0xAAu;
    CHECK(rdna4dl_deserialize(grown, wire_len + 1u, &out) == RDNA4DL_ERR_BOUNDS, "trailing byte rejected");
    free(grown);

    uint8_t *mut = (uint8_t *)malloc(wire_len);
    CHECK(mut != NULL, "alloc mut");

    // Foreign magic ('MTLB' & 'GRBA')
    memcpy(mut, wire, wire_len);
    memcpy(mut, "MTLB", 4);
    CHECK(rdna4dl_deserialize(mut, wire_len, &out) == RDNA4DL_ERR_MAGIC, "MTLB magic rejected");
    memcpy(mut, "GRBA", 4);
    CHECK(rdna4dl_deserialize(mut, wire_len, &out) == RDNA4DL_ERR_MAGIC, "GRBA magic rejected");

    // Bad format version
    memcpy(mut, wire, wire_len);
    rdna4dl_put_u32(mut + 4, 2u);
    CHECK(rdna4dl_deserialize(mut, wire_len, &out) == RDNA4DL_ERR_VERSION, "bad version rejected");

    // Bad header size
    memcpy(mut, wire, wire_len);
    rdna4dl_put_u32(mut + 8, 60u);
    CHECK(rdna4dl_deserialize(mut, wire_len, &out) == RDNA4DL_ERR_HEADER, "bad header_size rejected");

    // Bad ABI version
    memcpy(mut, wire, wire_len);
    rdna4dl_put_u32(mut + 12, 99u);
    CHECK(rdna4dl_deserialize(mut, wire_len, &out) == RDNA4DL_ERR_ABI_MISMATCH, "bad abi_version rejected");

    // Non-zero reserved
    memcpy(mut, wire, wire_len);
    rdna4dl_put_u32(mut + 36, 1u);
    CHECK(rdna4dl_deserialize(mut, wire_len, &out) == RDNA4DL_ERR_HEADER, "nonzero reserved rejected");

    // Non-zero flags
    memcpy(mut, wire, wire_len);
    mut[20u] ^= 0x01u;
    CHECK(rdna4dl_deserialize(mut, wire_len, &out) == RDNA4DL_ERR_HEADER, "nonzero flags rejected");

    // Bit-flip tamper in file_crc32, installName, symbol, payload -> CRC mismatch
    const size_t flip_offsets[] = {
        offsetof(rdna4dl_header_t, file_crc32), // header file_crc32 byte
        sizeof(rdna4dl_header_t) + 2u,          // install_name byte
        sizeof(rdna4dl_header_t) + 24u,         // symbol table byte
        wire_len - 1u                           // last payload byte
    };
    for (size_t i = 0; i < sizeof(flip_offsets) / sizeof(flip_offsets[0]); i++) {
        memcpy(mut, wire, wire_len);
        mut[flip_offsets[i]] ^= 0x01u;
        CHECK(rdna4dl_deserialize(mut, wire_len, &out) == RDNA4DL_ERR_CRC,
              "bit flip detected by whole-file CRC32");
    }

    // Payload CRC32 mismatch with valid whole-file CRC32
    memcpy(mut, wire, wire_len);
    mut[offsetof(rdna4dl_header_t, payload_crc32)] ^= 0x80u;
    recompute_grdl_file_crc(mut, wire_len);
    CHECK(rdna4dl_deserialize(mut, wire_len, &out) == RDNA4DL_ERR_PAYLOAD_CRC,
          "payload CRC32 mismatch rejected even with valid file CRC32");

    // Embedded NUL in installName (with recomputed valid CRCs)
    memcpy(mut, wire, wire_len);
    mut[sizeof(rdna4dl_header_t) + 3u] = '\0';
    recompute_grdl_file_crc(mut, wire_len);
    CHECK(rdna4dl_deserialize(mut, wire_len, &out) == RDNA4DL_ERR_INVALID,
          "embedded NUL in installName rejected");

    // Duplicate symbol in wire format (overwrite "sym_two" with "sym_one" + recompute file CRC)
    memcpy(mut, wire, wire_len);
    size_t sym2_off = sizeof(rdna4dl_header_t) + 18u + 4u + 7u + 4u;
    CHECK(memcmp(mut + sym2_off, "sym_two", 7u) == 0, "locate sym_two offset");
    memcpy(mut + sym2_off, "sym_one", 7u);
    recompute_grdl_file_crc(mut, wire_len);
    CHECK(rdna4dl_deserialize(mut, wire_len, &out) == RDNA4DL_ERR_DUPLICATE,
          "duplicate wire symbol rejected");

    // Invalid module kind in wire format (with recomputed file CRC)
    memcpy(mut, wire, wire_len);
    size_t mod_hdr_off = sym2_off + 7u;
    rdna4dl_put_u32(mut + mod_hdr_off, 99u);
    recompute_grdl_file_crc(mut, wire_len);
    CHECK(rdna4dl_deserialize(mut, wire_len, &out) == RDNA4DL_ERR_HEADER,
          "invalid module kind rejected");

    free(mut);
    free(wire);
    rdna4dl_destroy(&out);
    rdna4dl_destroy(&c);
}

static void test_grdl_atomic_file_io(void) {
    const char *base = getenv("TMPDIR");
    if (!base || !base[0]) base = "/tmp";
    char dir_tmpl[512];
    int dn = snprintf(dir_tmpl, sizeof(dir_tmpl), "%s/grdl-test-XXXXXX", base);
    CHECK(dn > 0 && (size_t)dn < sizeof(dir_tmpl), "temp dir path fits");
    char *dir = mkdtemp(dir_tmpl);
    CHECK(dir != NULL, "mkdtemp succeeded");
    if (!dir) return;

    char fpath[600];
    snprintf(fpath, sizeof(fpath), "%s/libtest.grdl", dir);

    const uint8_t m1[] = { 'B', 'C', 0xc0, 0xde, 0x42, 0x43, 0x44, 0x45 };
    const uint8_t m2[] = { 'B', 'C', 0xc0, 0xde, 0x99, 0x88, 0x77, 0x66, 0x55 };

    rdna4dl_container_t c1;
    rdna4dl_init(&c1);
    CHECK(rdna4dl_set_install_name(&c1, "@rpath/v1.grdl", 14) == RDNA4DL_OK, "c1 name");
    CHECK(rdna4dl_add_symbol(&c1, "fn_v1") == RDNA4DL_OK, "c1 sym");
    CHECK(rdna4dl_add_module(&c1, RDNA4DL_PAYLOAD_AIR_BITCODE, m1, sizeof(m1)) == RDNA4DL_OK, "c1 mod");

    int ioe = 0;
    CHECK(rdna4dl_write_file_atomic(&c1, fpath, &ioe) == RDNA4DL_OK && ioe == 0, "atomic write v1");

    rdna4dl_container_t rd;
    rdna4dl_init(&rd);
    CHECK(rdna4dl_read_file(fpath, &rd, &ioe) == RDNA4DL_OK && ioe == 0, "read v1");
    CHECK(strcmp(rd.install_name, "@rpath/v1.grdl") == 0 && rdna4dl_has_symbol(&rd, "fn_v1") == 1,
          "v1 file contents match");
    rdna4dl_destroy(&rd);

    // Atomic overwrite with v2
    rdna4dl_container_t c2;
    rdna4dl_init(&c2);
    CHECK(rdna4dl_set_install_name(&c2, "@rpath/v2.grdl", 14) == RDNA4DL_OK, "c2 name");
    CHECK(rdna4dl_add_symbol(&c2, "fn_v2") == RDNA4DL_OK, "c2 sym");
    CHECK(rdna4dl_add_module(&c2, RDNA4DL_PAYLOAD_AIR_BITCODE, m2, sizeof(m2)) == RDNA4DL_OK, "c2 mod");
    CHECK(rdna4dl_write_file_atomic(&c2, fpath, &ioe) == RDNA4DL_OK && ioe == 0, "atomic overwrite v2");

    rdna4dl_init(&rd);
    CHECK(rdna4dl_read_file(fpath, &rd, &ioe) == RDNA4DL_OK, "read v2");
    CHECK(strcmp(rd.install_name, "@rpath/v2.grdl") == 0 &&
          rdna4dl_has_symbol(&rd, "fn_v2") == 1 && rdna4dl_has_symbol(&rd, "fn_v1") == 0 &&
          rd.modules[0].size == sizeof(m2), "v2 overwrite contents match");
    rdna4dl_destroy(&rd);

    // Verify no leftover .tmp files in dir
    int tmp_leaks = 0;
    DIR *dp = opendir(dir);
    CHECK(dp != NULL, "opendir scratch");
    if (dp) {
        struct dirent *de;
        while ((de = readdir(dp)) != NULL) {
            if (strstr(de->d_name, ".tmp.") != NULL) tmp_leaks++;
        }
        closedir(dp);
    }
    CHECK(tmp_leaks == 0, "no atomic temp file leak");

    // Negative file cases: ENOENT, directory, truncated file, tampered file
    char missing[620];
    snprintf(missing, sizeof(missing), "%s/does-not-exist.grdl", dir);
    CHECK(rdna4dl_read_file(missing, &rd, &ioe) == RDNA4DL_ERR_IO && ioe == ENOENT,
          "missing file returns IO/ENOENT");
    CHECK(rdna4dl_read_file(dir, &rd, &ioe) == RDNA4DL_ERR_INVALID,
          "directory path rejected");

    // Tamper 1 byte in file on disk
    FILE *fp = fopen(fpath, "r+b");
    CHECK(fp != NULL, "open file for tamper");
    if (fp) {
        CHECK(fseek(fp, -1, SEEK_END) == 0, "seek end-1");
        int b = fgetc(fp);
        CHECK(b != EOF && fseek(fp, -1, SEEK_END) == 0, "read/reseek last byte");
        fputc((b ^ 0xff) & 0xff, fp);
        fclose(fp);
    }
    CHECK(rdna4dl_read_file(fpath, &rd, &ioe) == RDNA4DL_ERR_CRC, "tampered disk file rejected");

    // Truncate disk file
    CHECK(truncate(fpath, 32) == 0, "truncate disk file");
    CHECK(rdna4dl_read_file(fpath, &rd, &ioe) == RDNA4DL_ERR_TRUNCATED, "truncated disk file rejected");

    unlink(fpath);
    rmdir(dir);
    rdna4dl_destroy(&c1);
    rdna4dl_destroy(&c2);
}

static void test_symbols_json_parser(void) {
    rdna4dl_container_t c;
    rdna4dl_init(&c);

    const char *j1 = " [ \"fn_a\" , \"_Z5scaleDv4_f\", \"a\\\"b\\\\c\\/d\" ] ";
    CHECK(rdna4dl_parse_symbols_json(j1, strlen(j1), &c) == RDNA4DL_OK, "parse valid symbols JSON");
    CHECK(c.symbol_count == 3u, "parsed symbol count == 3");
    CHECK(rdna4dl_has_symbol(&c, "fn_a") == 1, "has fn_a");
    CHECK(rdna4dl_has_symbol(&c, "_Z5scaleDv4_f") == 1, "has mangled sym");
    CHECK(rdna4dl_has_symbol(&c, "a\"b\\c/d") == 1, "unescaped JSON string");
    rdna4dl_destroy(&c);

    // Duplicate entry rejected strictly
    rdna4dl_init(&c);
    CHECK(rdna4dl_parse_symbols_json("[\"dup\",\"dup\"]", 13, &c) == RDNA4DL_ERR_DUPLICATE,
          "duplicate symbol in JSON rejected");
    rdna4dl_destroy(&c);

    // Empty array
    rdna4dl_init(&c);
    CHECK(rdna4dl_parse_symbols_json("[]", 2, &c) == RDNA4DL_OK && c.symbol_count == 0u,
          "empty JSON array OK");
    rdna4dl_destroy(&c);

    // Negative cases
    const char *bad_cases[] = {
        "",
        "{}",
        "[123]",
        "[\"\"]",
        "[\"ok\",]",
        "[\"ok\"] trailing",
        "[\"unterminated",
        "[\"bad\\xescape\"]",
        "[\"nul\\u0000byte\"]"
    };
    for (size_t i = 0; i < sizeof(bad_cases) / sizeof(bad_cases[0]); i++) {
        rdna4dl_init(&c);
        CHECK(rdna4dl_parse_symbols_json(bad_cases[i], strlen(bad_cases[i]), &c) != RDNA4DL_OK,
              "malformed symbols JSON rejected");
        rdna4dl_destroy(&c);
    }
}

static void test_stage2_link_sha(void) {
    const char *e1 = "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef";
    const char *e2 = "fedcba9876543210fedcba9876543210fedcba9876543210fedcba9876543210";
    const char *s1 = "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa";
    const char *s2 = "bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb";
    const char *s3 = "cccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccc";

    const char *depN_a[] = { "dep_b", "dep_a" };
    const char *depS_a[] = { s2, s1 };
    const char *depN_b[] = { "dep_a", "dep_b", "dep_a" };
    const char *depS_b[] = { s1, s2, s1 };

    const char *libN_a[] = { "@rpath/lib2", "@rpath/lib1" };
    const char *libS_a[] = { s3, s1 };
    const char *libN_b[] = { "@rpath/lib1", "@rpath/lib2" };
    const char *libS_b[] = { s1, s3 };

    const char *visN_a[] = { "v2", "v1" };
    const char *visS_a[] = { s2, s3 };
    const char *visN_b[] = { "v1", "v2" };
    const char *visS_b[] = { s3, s2 };

    char k1[65], k2[65], k3[65], k4[65], k5[65];
    CHECK(rdna4dl_stage2_link_sha(k1, e1, 2, depN_a, depS_a, 2, libN_a, libS_a, 2, visN_a, visS_a) == 0,
          "stage2 key 1");
    CHECK(rdna4dl_stage2_link_sha(k2, e1, 3, depN_b, depS_b, 2, libN_b, libS_b, 2, visN_b, visS_b) == 0,
          "stage2 key 2 (permuted + duplicate)");
    CHECK(strcmp(k1, k2) == 0, "stage2 key is order-independent and deduplicated");
    CHECK(strlen(k1) == 64u, "stage2 key is 64 hex chars");

    // Different entry SHA -> different key
    CHECK(rdna4dl_stage2_link_sha(k3, e2, 2, depN_a, depS_a, 2, libN_a, libS_a, 2, visN_a, visS_a) == 0,
          "stage2 key different entry");
    CHECK(strcmp(k1, k3) != 0, "different entry changes key");

    // Category separation: same pair in dynlibs vs visible_fns -> different keys
    const char *oneN[] = { "shared_fn" };
    const char *oneS[] = { s1 };
    CHECK(rdna4dl_stage2_link_sha(k4, e1, 0, NULL, NULL, 1, oneN, oneS, 0, NULL, NULL) == 0,
          "key with dynlib only");
    CHECK(rdna4dl_stage2_link_sha(k5, e1, 0, NULL, NULL, 0, NULL, NULL, 1, oneN, oneS) == 0,
          "key with visible fn only");
    CHECK(strcmp(k4, k5) != 0, "dynlib vs visible_fn categories produce distinct keys");

    // Invalid entry SHA rejected
    CHECK(rdna4dl_stage2_link_sha(k1, "SHORT", 0, NULL, NULL, 0, NULL, NULL, 0, NULL, NULL) != 0,
          "short sha rejected");
    CHECK(rdna4dl_stage2_link_sha(k1, "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA",
                                  0, NULL, NULL, 0, NULL, NULL, 0, NULL, NULL) != 0,
          "uppercase hex sha rejected");
}

static void test_vft_buffer_encoder(void) {
    // 1. Empty table (slot_count = 0)
    rdna4vft_table_t empty;
    CHECK(rdna4vft_init(&empty, 0, 0) == RDNA4VFT_OK, "init empty VFT");
    uint32_t ebuf[2] = { 0xdeadbeefu, 0xdeadbeefu };
    size_t written = 0;
    CHECK(rdna4vft_encode_buffer(&empty, ebuf, 1, &written) == RDNA4VFT_OK, "encode empty VFT");
    CHECK(written == 1u && ebuf[0] == 0u, "empty VFT encodes [0]");
    CHECK(rdna4vft_simulate_lookup(ebuf, written, 0u) == RDNA4VFT_HANDLE_NULL, "empty lookup OOB -> 0");
    CHECK(rdna4vft_set_slot(&empty, 0u, 0u) == RDNA4VFT_ERR_OOB, "empty set_slot 0 -> OOB");
    rdna4vft_destroy(&empty);

    // 2. Normal table with slots, swap, null, OOB, atomic range validation
    rdna4vft_table_t t;
    CHECK(rdna4vft_init(&t, 4, 3) == RDNA4VFT_OK, "init 4-slot VFT maxHandle=3");
    CHECK(rdna4vft_buffer_words(4) == 5u && rdna4vft_buffer_bytes(4) == 20u, "buffer size helper");

    uint32_t buf[5] = {0};
    CHECK(rdna4vft_encode_buffer(&t, buf, 4, &written) == RDNA4VFT_ERR_BOUNDS,
          "undersized encode buffer rejected");
    CHECK(rdna4vft_encode_buffer(&t, buf, 5, &written) == RDNA4VFT_OK && written == 5u,
          "encode initial zero VFT");
    CHECK(buf[0] == 4u && buf[1] == 0u && buf[2] == 0u && buf[3] == 0u && buf[4] == 0u,
          "initial VFT header=4, all slots null");

    CHECK(rdna4vft_set_slot(&t, 0, 1) == RDNA4VFT_OK, "slot 0 -> 1");
    CHECK(rdna4vft_set_slot(&t, 1, 2) == RDNA4VFT_OK, "slot 1 -> 2");
    CHECK(rdna4vft_set_slot(&t, 2, 3) == RDNA4VFT_OK, "slot 2 -> 3");
    CHECK(rdna4vft_set_slot(&t, 3, RDNA4VFT_HANDLE_NULL) == RDNA4VFT_OK, "slot 3 -> null");
    volatile uint32_t oob_slot = 4u;
    volatile uint32_t oob_max = UINT32_MAX;
    CHECK(rdna4vft_set_slot(&t, oob_slot, 1) == RDNA4VFT_ERR_OOB, "OOB slot 4 rejected");
    CHECK(rdna4vft_set_slot(&t, oob_max, 1) == RDNA4VFT_ERR_OOB, "OOB UINT32_MAX rejected");
    CHECK(rdna4vft_set_slot(&t, 0, 4) == RDNA4VFT_ERR_INVALID_HANDLE, "handle > max_handle_id rejected");
    CHECK(rdna4vft_get_slot(&t, 0) == 1u, "rejected handle did not overwrite slot 0");

    CHECK(rdna4vft_encode_buffer(&t, buf, 5, &written) == RDNA4VFT_OK, "encode populated VFT");
    CHECK(buf[0] == 4u && buf[1] == 1u && buf[2] == 2u && buf[3] == 3u && buf[4] == 0u,
          "encoded words match 0074 ABI");
    CHECK(rdna4vft_simulate_lookup(buf, 5, 0) == 1u, "lookup slot 0 -> 1");
    CHECK(rdna4vft_simulate_lookup(buf, 5, 1) == 2u, "lookup slot 1 -> 2");
    CHECK(rdna4vft_simulate_lookup(buf, 5, 2) == 3u, "lookup slot 2 -> 3");
    CHECK(rdna4vft_simulate_lookup(buf, 5, 3) == 0u, "lookup null slot 3 -> 0");
    CHECK(rdna4vft_simulate_lookup(buf, 5, 4) == 0u, "lookup OOB slot 4 -> 0 (no hang)");
    CHECK(rdna4vft_simulate_lookup(buf, 5, 9999u) == 0u, "lookup OOB slot 9999 -> 0");

    // Atomic set_range failure: slot 0 valid, slot 1 invalid -> neither changes!
    const uint32_t bad_range[2] = { 3u, 99u };
    CHECK(rdna4vft_set_range(&t, 0, 2, bad_range) == RDNA4VFT_ERR_INVALID_HANDLE,
          "set_range rejects invalid mid-range handle");
    CHECK(rdna4vft_get_slot(&t, 0) == 1u && rdna4vft_get_slot(&t, 1) == 2u,
          "set_range failure leaves existing slots untouched");

    // Overflow-safe set_range check
    CHECK(rdna4vft_set_range(&t, UINT32_MAX, 2, bad_range) == RDNA4VFT_ERR_OOB,
          "set_range integer overflow rejected");

    // Dynamic swap on the same table
    const uint32_t swap_range[3] = { 3u, 0u, 1u };
    CHECK(rdna4vft_set_range(&t, 0, 3, swap_range) == RDNA4VFT_OK, "dynamic swap via set_range");
    CHECK(rdna4vft_encode_buffer(&t, buf, 5, &written) == RDNA4VFT_OK, "re-encode after swap");
    CHECK(buf[0] == 4u && buf[1] == 3u && buf[2] == 0u && buf[3] == 1u && buf[4] == 0u,
          "swapped buffer words exact");
    CHECK(rdna4vft_simulate_lookup(buf, 5, 0) == 3u &&
          rdna4vft_simulate_lookup(buf, 5, 1) == 0u &&
          rdna4vft_simulate_lookup(buf, 5, 2) == 1u, "simulated lookups reflect dynamic swap");

    rdna4vft_destroy(&t);
    rdna4vft_destroy(&t); // idempotent destroy check
}

static void test_vft_reflection_json(void) {
    // 1. Realistic Stage 2 compute reflection JSON with descriptor object & null descriptor
    const char *compute_json =
        "{"
        "  \"stage\": \"compute\","
        "  \"bindings\": ["
        "    {\"kind\": \"Buffer\", \"metal_index\": 0, \"descriptor\": {\"set\": 0, \"binding\": 0, \"count\": 1}},"
        "    {\"kind\": \"Buffer\", \"metal_index\": 2, \"descriptor\": {\"set\": 0, \"binding\": 2, \"count\": 1}}"
        "  ],"
        "  \"flags\": [true, false, null, -3.5e2],"
        "  \"visible_function_tables\": {"
        "    \"abi_version\": 1,"
        "    \"header_words\": 1,"
        "    \"tables\": ["
        "      {\"param_index\": 2, \"metal_index\": 2, \"descriptor\": {\"set\": 0, \"binding\": 2, \"count\": 1}},"
        "      {\"param_index\": 3, \"metal_index\": 4, \"descriptor\": null}"
        "    ],"
        "    \"handles\": ["
        "      {\"handle_id\": 1, \"symbol\": \"fn_add\"},"
        "      {\"handle_id\": 2, \"symbol\": \"fn_mul\"},"
        "      {\"handle_id\": 3, \"symbol\": \"fn_xor\"}"
        "    ]"
        "  }"
        "}";

    rdna4vft_reflection_t r;
    rdna4vft_refl_init(&r);
    CHECK(rdna4vft_parse_reflection_json(compute_json, strlen(compute_json), &r) == RDNA4VFT_OK,
          "parse Stage 2 compute VFT reflection JSON");
    CHECK(r.present == 1u, "VFT metadata present");
    CHECK(r.abi_version == 1u && r.header_words == 1u, "ABI version and header_words == 1");
    CHECK(r.table_count == 2u, "2 VFT table bindings parsed");
    CHECK(r.tables[0].param_index == 2u && r.tables[0].metal_index == 2u &&
          r.tables[0].has_descriptor == 1u && r.tables[0].desc_set == 0u &&
          r.tables[0].desc_binding == 2u && r.tables[0].desc_count == 1u, "table 0 descriptor exact");
    CHECK(rdna4vft_refl_has_table_metal_index(&r, 2u) == 1 &&
          rdna4vft_refl_has_table_metal_index(&r, 4u) == 1 &&
          rdna4vft_refl_has_table_metal_index(&r, 9u) == 0, "table metal_index lookup");
    CHECK(r.tables[1].param_index == 3u && r.tables[1].metal_index == 4u &&
          r.tables[1].has_descriptor == 0u, "table 1 null descriptor handled");
    CHECK(r.handle_count == 3u && r.max_handle_id == 3u, "3 handles parsed, max_handle_id == 3");
    CHECK(rdna4vft_refl_find_handle(&r, "fn_add") == 1u, "find fn_add -> 1");
    CHECK(rdna4vft_refl_find_handle(&r, "fn_mul") == 2u, "find fn_mul -> 2");
    CHECK(rdna4vft_refl_find_handle(&r, "fn_xor") == 3u, "find fn_xor -> 3");
    CHECK(rdna4vft_refl_find_handle(&r, "fn_missing") == 0u, "missing symbol -> 0");
    CHECK(strcmp(rdna4vft_refl_find_symbol(&r, 2u), "fn_mul") == 0, "find handle 2 -> fn_mul");
    CHECK(rdna4vft_refl_find_symbol(&r, 0u) == NULL && rdna4vft_refl_find_symbol(&r, 9u) == NULL,
          "invalid handle id -> NULL");
    rdna4vft_refl_destroy(&r);

    // 2. Absent and null visible_function_tables -> OK with present == 0
    const char *no_vft = "{\"stage\": \"fragment\", \"bindings\": []}";
    rdna4vft_refl_init(&r);
    CHECK(rdna4vft_parse_reflection_json(no_vft, strlen(no_vft), &r) == RDNA4VFT_OK && r.present == 0u,
          "absent visible_function_tables -> OK, present=0");
    rdna4vft_refl_destroy(&r);

    const char *null_vft = "{\"visible_function_tables\": null}";
    rdna4vft_refl_init(&r);
    CHECK(rdna4vft_parse_reflection_json(null_vft, strlen(null_vft), &r) == RDNA4VFT_OK && r.present == 0u,
          "null visible_function_tables -> OK, present=0");
    rdna4vft_refl_destroy(&r);

    // 3. Negative JSON cases
    const char *bad_abi =
        "{\"visible_function_tables\":{\"abi_version\":2,\"header_words\":1,\"tables\":[],\"handles\":[]}}";
    CHECK(rdna4vft_parse_reflection_json(bad_abi, strlen(bad_abi), &r) == RDNA4VFT_ERR_ABI,
          "abi_version != 1 rejected");

    const char *bad_hdr =
        "{\"visible_function_tables\":{\"abi_version\":1,\"header_words\":2,\"tables\":[],\"handles\":[]}}";
    CHECK(rdna4vft_parse_reflection_json(bad_hdr, strlen(bad_hdr), &r) == RDNA4VFT_ERR_ABI,
          "header_words != 1 rejected");

    const char *zero_hid =
        "{\"visible_function_tables\":{\"abi_version\":1,\"header_words\":1,\"tables\":[],"
        "\"handles\":[{\"handle_id\":0,\"symbol\":\"bad\"}]}}";
    CHECK(rdna4vft_parse_reflection_json(zero_hid, strlen(zero_hid), &r) == RDNA4VFT_ERR_INVALID_HANDLE,
          "handle_id == 0 rejected");

    const char *dup_hid =
        "{\"visible_function_tables\":{\"abi_version\":1,\"header_words\":1,\"tables\":[],"
        "\"handles\":[{\"handle_id\":1,\"symbol\":\"a\"},{\"handle_id\":1,\"symbol\":\"b\"}]}}";
    CHECK(rdna4vft_parse_reflection_json(dup_hid, strlen(dup_hid), &r) == RDNA4VFT_ERR_DUPLICATE,
          "duplicate handle_id rejected");

    const char *dup_sym =
        "{\"visible_function_tables\":{\"abi_version\":1,\"header_words\":1,\"tables\":[],"
        "\"handles\":[{\"handle_id\":1,\"symbol\":\"a\"},{\"handle_id\":2,\"symbol\":\"a\"}]}}";
    CHECK(rdna4vft_parse_reflection_json(dup_sym, strlen(dup_sym), &r) == RDNA4VFT_ERR_DUPLICATE,
          "duplicate handle symbol rejected");

    const char *oob_metal_idx =
        "{\"visible_function_tables\":{\"abi_version\":1,\"header_words\":1,"
        "\"tables\":[{\"param_index\":0,\"metal_index\":31,\"descriptor\":null}],\"handles\":[]}}";
    CHECK(rdna4vft_parse_reflection_json(oob_metal_idx, strlen(oob_metal_idx), &r) == RDNA4VFT_ERR_BOUNDS,
          "metal_index >= 31 rejected");

    // Deep nesting (> 64) rejected without stack overflow
    char deep[256];
    size_t pos = 0;
    for (int i = 0; i < 80; i++) deep[pos++] = '[';
    for (int i = 0; i < 80; i++) deep[pos++] = ']';
    deep[pos] = '\0';
    CHECK(rdna4vft_parse_reflection_json(deep, pos, &r) == RDNA4VFT_ERR_JSON,
          "deeply nested JSON rejected cleanly");
}

static void test_footnote10_exclusion(void) {
    const char *why = NULL;

    // Allowed combinations
    CHECK(rdna4vft_check_footnote10(0u, &why) == RDNA4VFT_OK && why == NULL,
          "plain render pipeline allowed");
    CHECK(rdna4vft_check_footnote10(RDNA4FN10_HAS_VERTEX_FN_PTRS | RDNA4FN10_HAS_FRAGMENT_FN_PTRS |
                                    RDNA4FN10_SUPPORT_ADD_BIN_FNS, &why) == RDNA4VFT_OK,
          "vertex/fragment function pointers without mesh/object allowed");
    CHECK(rdna4vft_check_footnote10(RDNA4FN10_HAS_OBJECT_STAGE | RDNA4FN10_HAS_MESH_STAGE, &why) == RDNA4VFT_OK,
          "object+mesh pipeline without fn pointers or render RT allowed");

    // Refused combinations (Metal 3 Footnote 10)
    const uint32_t bad_fn_ptr_combos[] = {
        RDNA4FN10_HAS_MESH_STAGE | RDNA4FN10_HAS_VERTEX_FN_PTRS,
        RDNA4FN10_HAS_MESH_STAGE | RDNA4FN10_HAS_FRAGMENT_FN_PTRS,
        RDNA4FN10_HAS_MESH_STAGE | RDNA4FN10_HAS_OBJECT_FN_PTRS,
        RDNA4FN10_HAS_MESH_STAGE | RDNA4FN10_HAS_MESH_FN_PTRS,
        RDNA4FN10_HAS_MESH_STAGE | RDNA4FN10_SUPPORT_ADD_BIN_FNS,
        RDNA4FN10_HAS_OBJECT_STAGE | RDNA4FN10_HAS_FRAGMENT_FN_PTRS,
        RDNA4FN10_HAS_OBJECT_FN_PTRS,
        RDNA4FN10_HAS_MESH_FN_PTRS
    };
    for (size_t i = 0; i < sizeof(bad_fn_ptr_combos) / sizeof(bad_fn_ptr_combos[0]); i++) {
        why = NULL;
        CHECK(rdna4vft_check_footnote10(bad_fn_ptr_combos[i], &why) == RDNA4VFT_ERR_FOOTNOTE10_MESH_FN_PTR &&
              why != NULL && why[0] != '\0',
              "Footnote 10 forbids mesh/object + render fn pointers");
    }
    const uint32_t bad_rt_combos[] = {
        RDNA4FN10_HAS_MESH_STAGE | RDNA4FN10_HAS_RENDER_RAYTRACING,
        RDNA4FN10_HAS_OBJECT_STAGE | RDNA4FN10_HAS_RENDER_RAYTRACING
    };
    for (size_t i = 0; i < sizeof(bad_rt_combos) / sizeof(bad_rt_combos[0]); i++) {
        why = NULL;
        CHECK(rdna4vft_check_footnote10(bad_rt_combos[i], &why) == RDNA4VFT_ERR_FOOTNOTE10_MESH_RT &&
              why != NULL && why[0] != '\0',
              "Footnote 10 forbids mesh/object + render ray tracing");
    }
}

static char *read_entire_text_file(const char *path) {
    FILE *fp = fopen(path, "rb");
    if (!fp) return NULL;
    if (fseek(fp, 0, SEEK_END) != 0) { fclose(fp); return NULL; }
    long sz = ftell(fp);
    if (sz < 0 || fseek(fp, 0, SEEK_SET) != 0) { fclose(fp); return NULL; }
    char *buf = (char *)malloc((size_t)sz + 1u);
    if (!buf) { fclose(fp); return NULL; }
    size_t n = fread(buf, 1u, (size_t)sz, fp);
    fclose(fp);
    buf[n] = '\0';
    return buf;
}

static void test_source_pins(const char *src_dir) {
    char p1[512], p2[512];
    snprintf(p1, sizeof(p1), "%s/rdna4_shader_runtime_impl.inc", src_dir);
    snprintf(p2, sizeof(p2), "%s/rdna4_dynlib_vft_impl.inc", src_dir);

    char *rt = read_entire_text_file(p1);
    CHECK(rt != NULL, "read rdna4_shader_runtime_impl.inc");
    if (rt) {
        CHECK(strstr(rt, "#include \"rdna4_dynlib_vft.h\"") != NULL, "runtime includes rdna4_dynlib_vft.h");
        CHECK(strstr(rt, "\"garnetx_list_exported_symbols\"") != NULL, "runtime dlsym garnetx_list_exported_symbols");
        CHECK(strstr(rt, "\"garnetx_translate_linked_libraries\"") != NULL, "runtime dlsym garnetx_translate_linked_libraries");
        CHECK(strstr(rt, "rdna4x_list_exported_symbols_for_module") != NULL, "runtime symbol enumeration helper");
        CHECK(strstr(rt, "rdna4x_parse_vft_from_meta") != NULL, "runtime VFT reflection helper");
        CHECK(strstr(rt, "rdna4x_compile_stage_with_linkage") != NULL, "runtime Stage 2 compile helper");
        CHECK(strstr(rt, "rdna4dl_stage2_link_sha") != NULL, "runtime uses Stage 2 link SHA");
        CHECK(strstr(rt, "rdna4vft_parse_reflection_json") != NULL, "runtime parses VFT reflection JSON");
        CHECK(strstr(rt, "\"preloadedLibraries\"") != NULL, "runtime inspects compute preloadedLibraries");
        CHECK(strstr(rt, "\"vertexPreloadedLibraries\"") != NULL, "runtime inspects vertexPreloadedLibraries");
        CHECK(strstr(rt, "\"fragmentPreloadedLibraries\"") != NULL, "runtime inspects fragmentPreloadedLibraries");
        CHECK(strstr(rt, "\"binaryFunctions\"") != NULL, "runtime inspects binaryFunctions");
        CHECK(strstr(rt, "binaryFunction modules or total size exceeds limit") != NULL,
              "runtime refuses binaryFunctions cap overflow instead of truncating");
        CHECK(strstr(rt, "out of memory copying Stage 2 translation outputs") != NULL,
              "runtime fails cleanly on Stage 2 output copy OOM");
        free(rt);
    }

    char *impl = read_entire_text_file(p2);
    CHECK(impl != NULL, "read rdna4_dynlib_vft_impl.inc");
    if (impl) {
        CHECK(strstr(impl, "rdna4_dl_objc_new1") != NULL && strstr(impl, "CFBridgingRelease(raw)") != NULL,
              "dynamic new* selector transfers +1 ownership via CFBridgingRelease");
        CHECK(strstr(impl, "MTLLibrary belongs to a different MTLDevice") != NULL,
              "newDynamicLibrary rejects foreign-device MTLLibrary");
        CHECK(strstr(impl, "@implementation RDNA4DynamicLibrary") != NULL, "implements RDNA4DynamicLibrary");
        CHECK(strstr(impl, "@implementation RDNA4FunctionHandle") != NULL, "implements RDNA4FunctionHandle");
        CHECK(strstr(impl, "@implementation RDNA4VisibleFunctionTable") != NULL, "implements RDNA4VisibleFunctionTable");
        CHECK(strstr(impl, "- (BOOL)serializeToURL:(NSURL *)url error:(NSError **)error") != NULL,
              "implements serializeToURL:error:");
        CHECK(strstr(impl, "- (instancetype)initWithDevice:(id)dev url:(NSURL *)url error:(NSError **)error") != NULL,
              "implements initWithDevice:url:error:");
        CHECK(strstr(impl, "- (void)setFunction:(id<MTLFunctionHandle>)function atIndex:(NSUInteger)index") != NULL,
              "implements setFunction:atIndex:");
        CHECK(strstr(impl, "- (void)setFunctions:(const id<MTLFunctionHandle> __unsafe_unretained *)functions withRange:(NSRange)range") != NULL,
              "implements setFunctions:withRange:");
        CHECK(strstr(impl, "- (void)setBuffer:(id<MTLBuffer>)buffer offset:(NSUInteger)offset atIndex:(NSUInteger)index") != NULL,
              "implements setBuffer:offset:atIndex:");
        CHECK(strstr(impl, "- (void)setBuffers:(const id<MTLBuffer> __unsafe_unretained *)buffers") != NULL,
              "implements setBuffers:offsets:withRange:");
        CHECK(strstr(impl, "- (void)setVisibleFunctionTable:(id<MTLVisibleFunctionTable>)functionTable atBufferIndex:(NSUInteger)bufferIndex") != NULL,
              "implements setVisibleFunctionTable:atBufferIndex:");
        CHECK(strstr(impl, "- (void)setVisibleFunctionTables:(const id<MTLVisibleFunctionTable> __unsafe_unretained *)functionTables") != NULL,
              "implements setVisibleFunctionTables:withBufferRange:");
        CHECK(strstr(impl, "RDNA4_RESOURCE_SPI") != NULL, "includes RDNA4_RESOURCE_SPI (gpuResourceID + MTLResource)");
        CHECK(strstr(impl, "rdna4vft_encode_buffer") != NULL, "uses pure-C 0074 VFT buffer encoder");
        CHECK(strstr(impl, "rdna4vft_check_footnote10") != NULL, "enforces Footnote 10 mutual exclusion");
        CHECK(strstr(impl, "_backing.contents") == NULL && strstr(impl, "_backing.length") == NULL,
              "uses message syntax on RDNA4Buffer (avoids undeclared property error)");
        free(impl);
    }
}

int main(int argc, char **argv) {
    const char *src_dir = (argc > 1 && argv[1] && argv[1][0]) ? argv[1] : "src/garnet-metal";
    test_grdl_roundtrip();
    test_grdl_tamper_and_truncation();
    test_grdl_atomic_file_io();
    test_symbols_json_parser();
    test_stage2_link_sha();
    test_vft_buffer_encoder();
    test_vft_reflection_json();
    test_footnote10_exclusion();
    test_source_pins(src_dir);

    printf("test-dynlib-vft: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
