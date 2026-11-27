#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "garnet_recover_pure.h"

static int g_failures = 0;

static void expect_true(bool cond, const char *msg) {
    if (!cond) {
        fprintf(stderr, "FAIL: %s\n", msg);
        g_failures++;
    }
}

static void expect_str(const char *got, const char *want, const char *msg) {
    if (strcmp(got, want) != 0) {
        fprintf(stderr, "FAIL: %s\n  got : '%s'\n  want: '%s'\n", msg, got, want);
        g_failures++;
    }
}

static void test_empty_and_null_boot_args(void) {
    char out[GARNET_BOOT_ARGS_BUF_SIZE];
    size_t out_len = 999u;

    expect_true(garnet_recover_build_safe_boot_args(NULL, 0u, out, sizeof(out), &out_len), "NULL input succeeds");
    expect_str(out, GARNET_DISABLE_BOOT_ARGS, "NULL input emits disable flags");
    expect_true(out_len == strlen(GARNET_DISABLE_BOOT_ARGS), "NULL input exact out_len");

    expect_true(garnet_recover_build_safe_boot_args("   \t\r\n ", 7u, out, sizeof(out), &out_len), "whitespace-only succeeds");
    expect_str(out, GARNET_DISABLE_BOOT_ARGS, "whitespace-only emits disable flags");
}

static void test_strip_and_preserve_tokens(void) {
    char out[GARNET_BOOT_ARGS_BUF_SIZE];
    size_t out_len = 0u;
    const char *raw = "-v keepsyms=1 garnet=1 garnet-stage=17 garnet-aux=1 garnet-rebar-compat=1 garnet-full-bar=1 amfi=0x80 agdpmod=pikera";

    expect_true(garnet_recover_build_safe_boot_args(raw, strlen(raw), out, sizeof(out), &out_len), "standard input succeeds");
    expect_str(
        out,
        "-v keepsyms=1 garnet-stage=17 garnet-full-bar=1 amfi=0x80 agdpmod=pikera garnet=0 garnet-aux=0 garnet-rebar-compat=0",
        "strips only garnet=, garnet-aux=, garnet-rebar-compat= and preserves unrelated args"
    );
    expect_true(out_len == strlen(out), "out_len matches strlen");
}

static void test_embedded_nul_and_duplicates(void) {
    char out[GARNET_BOOT_ARGS_BUF_SIZE];
    size_t out_len = 0u;
    const char raw[] = "garnet=1 -v garnet=0 garnet-aux=2\0GARBAGE_AFTER_NUL";

    expect_true(garnet_recover_build_safe_boot_args(raw, sizeof(raw), out, sizeof(out), &out_len), "embedded NUL succeeds");
    expect_str(out, "-v garnet=0 garnet-aux=0 garnet-rebar-compat=0", "stops at NUL and deduplicates stripped tokens");
}

static void test_buffer_overflow_refused(void) {
    char tiny[16];
    size_t out_len = 42u;
    expect_true(!garnet_recover_build_safe_boot_args("-v", 2u, tiny, sizeof(tiny), &out_len), "tiny buffer fails cleanly");
    expect_true(tiny[0] == '\0', "tiny buffer cleared on failure");

    char huge[1200];
    memset(huge, 'a', sizeof(huge));
    huge[sizeof(huge) - 1u] = '\0';
    char out[GARNET_BOOT_ARGS_BUF_SIZE];
    expect_true(!garnet_recover_build_safe_boot_args(huge, sizeof(huge), out, sizeof(out), &out_len), "oversized input refused");
    expect_true(out[0] == '\0', "oversized output cleared");
}

static void test_exact_max_boundary(void) {
    const size_t disable_len = strlen(GARNET_DISABLE_BOOT_ARGS);
    const size_t prefix_len = GARNET_BOOT_ARGS_MAX - disable_len - 1u; /* room for 1 space */
    char in[GARNET_BOOT_ARGS_BUF_SIZE];
    memset(in, 'x', prefix_len);
    in[prefix_len] = '\0';

    char out[GARNET_BOOT_ARGS_BUF_SIZE];
    size_t out_len = 0u;
    expect_true(garnet_recover_build_safe_boot_args(in, prefix_len, out, sizeof(out), &out_len), "exact 1023-byte boundary fits");
    expect_true(out_len == GARNET_BOOT_ARGS_MAX, "exact 1023-byte out_len");

    /* Adding 1 more byte must fail cleanly without overflow */
    in[prefix_len] = 'y';
    in[prefix_len + 1u] = '\0';
    expect_true(!garnet_recover_build_safe_boot_args(in, prefix_len + 1u, out, sizeof(out), &out_len), "1024-byte result refused");
}

static void test_key_decoding(void) {
    expect_true(garnet_recover_decode_key(0u, (uint16_t)'1') == GARNET_MENU_SAFE_BOOT_ONCE, "key 1 -> safe boot once");
    expect_true(garnet_recover_decode_key(0u, (uint16_t)'2') == GARNET_MENU_SAFE_BOOT_UNINSTALL, "key 2 -> uninstall");
    expect_true(garnet_recover_decode_key(GARNET_EFI_SCAN_ESC, 0u) == GARNET_MENU_CANCEL, "EFI SCAN_ESC -> cancel");
    expect_true(garnet_recover_decode_key(0u, 0x001Bu) == GARNET_MENU_CANCEL, "ASCII ESC -> cancel");
    expect_true(garnet_recover_decode_key(0u, (uint16_t)'q') == GARNET_MENU_CANCEL, "q -> cancel");
    expect_true(garnet_recover_decode_key(0u, (uint16_t)'Q') == GARNET_MENU_CANCEL, "Q -> cancel");
    expect_true(garnet_recover_decode_key(0u, (uint16_t)'3') == GARNET_MENU_IGNORE, "other key ignored");
    expect_true(garnet_recover_decode_key(0x0001u, 0u) == GARNET_MENU_IGNORE, "arrow scan code ignored");
}

int main(void) {
    test_empty_and_null_boot_args();
    test_strip_and_preserve_tokens();
    test_embedded_nul_and_duplicates();
    test_buffer_overflow_refused();
    test_exact_max_boundary();
    test_key_decoding();

    if (g_failures != 0) {
        fprintf(stderr, "recover_pure_test: %d failure(s)\n", g_failures);
        return 1;
    }
    printf("PASS recover_pure_test (all cases)\n");
    return 0;
}
