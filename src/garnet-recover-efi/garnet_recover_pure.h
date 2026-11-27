/*
 * garnet_recover_pure.h — Pure freestanding C11 helpers for GarnetRecover.efi.
 *
 * Keeps all NVRAM GUIDs, boot-args token stripping/appending, bounds checks,
 * and UEFI menu key decoding testable on the host under ASan/UBSan.
 */
#ifndef GARNET_RECOVER_PURE_H
#define GARNET_RECOVER_PURE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define GARNET_BOOT_ARGS_MAX 1023u
#define GARNET_BOOT_ARGS_BUF_SIZE (GARNET_BOOT_ARGS_MAX + 1u)

#define GARNET_DISABLE_BOOT_ARGS "garnet=0 garnet-aux=0 garnet-rebar-compat=0"
#define GARNET_RECOVER_ACTION_UNINSTALL "uninstall"

/* Apple Boot GUID: 7C436110-AB2A-4BBB-A880-FE41995C9F82 */
#define GARNET_APPLE_BOOT_GUID_INIT \
    { 0x7C436110u, 0xAB2Au, 0x4BBBu, { 0xA8u, 0x80u, 0xFEu, 0x41u, 0x99u, 0x5Cu, 0x9Fu, 0x82u } }

/* Garnet Vendor GUID: 6E727447-6172-4E65-9452-65636F766572 */
#define GARNET_VENDOR_GUID_INIT \
    { 0x6E727447u, 0x6172u, 0x4E65u, { 0x94u, 0x52u, 0x65u, 0x63u, 0x6Fu, 0x76u, 0x65u, 0x72u } }

#define GARNET_EFI_SCAN_ESC 0x0017u

typedef enum garnet_recover_choice {
    GARNET_MENU_IGNORE = 0,
    GARNET_MENU_SAFE_BOOT_ONCE = 1,
    GARNET_MENU_SAFE_BOOT_UNINSTALL = 2,
    GARNET_MENU_CANCEL = 3
} garnet_recover_choice_t;

static inline bool garnet_recover_is_space(char c) {
    return c == ' ' || c == '\t' || c == '\r' || c == '\n';
}

static inline bool garnet_recover_has_prefix(const char *s, size_t len, const char *prefix, size_t prefix_len) {
    if (len < prefix_len) {
        return false;
    }
    for (size_t i = 0; i < prefix_len; ++i) {
        if (s[i] != prefix[i]) {
            return false;
        }
    }
    return true;
}

/*
 * Strip existing kill-switch targets so newly appended =0 flags are unambiguous
 * regardless of whether XNU picks the first or last occurrence.
 */
static inline bool garnet_recover_is_strip_token(const char *tok, size_t len) {
    return garnet_recover_has_prefix(tok, len, "garnet=", 7u) ||
           garnet_recover_has_prefix(tok, len, "garnet-aux=", 11u) ||
           garnet_recover_has_prefix(tok, len, "garnet-rebar-compat=", 20u);
}

static inline bool garnet_recover_append_bytes(char *out, size_t out_cap, size_t *pos, const char *src, size_t len) {
    if (*pos > out_cap || len >= out_cap - *pos) {
        return false;
    }
    for (size_t i = 0; i < len; ++i) {
        out[*pos + i] = src[i];
    }
    *pos += len;
    out[*pos] = '\0';
    return true;
}

/*
 * Build safe boot-args from raw NVRAM bytes (may or may not be NUL-terminated).
 * Preserves unrelated tokens, strips conflicting Garnet kill-switch tokens,
 * and appends GARNET_DISABLE_BOOT_ARGS within out_cap (and GARNET_BOOT_ARGS_MAX).
 */
static inline bool garnet_recover_build_safe_boot_args(
    const char *in,
    size_t in_len,
    char *out,
    size_t out_cap,
    size_t *out_len
) {
    static const char kDisable[] = GARNET_DISABLE_BOOT_ARGS;
    const size_t disable_len = sizeof(kDisable) - 1u;

    if (out == NULL || out_cap == 0u || out_len == NULL) {
        return false;
    }
    out[0] = '\0';
    *out_len = 0u;

    const size_t effective_cap = (out_cap > GARNET_BOOT_ARGS_BUF_SIZE) ? GARNET_BOOT_ARGS_BUF_SIZE : out_cap;
    size_t pos = 0u;

    if (in != NULL && in_len > 0u) {
        size_t actual_len = 0u;
        while (actual_len < in_len && in[actual_len] != '\0') {
            actual_len++;
        }

        size_t i = 0u;
        while (i < actual_len) {
            while (i < actual_len && garnet_recover_is_space(in[i])) {
                i++;
            }
            if (i >= actual_len) {
                break;
            }
            const size_t start = i;
            while (i < actual_len && !garnet_recover_is_space(in[i])) {
                i++;
            }
            const size_t tok_len = i - start;
            if (garnet_recover_is_strip_token(in + start, tok_len)) {
                continue;
            }
            if (pos > 0u) {
                if (!garnet_recover_append_bytes(out, effective_cap, &pos, " ", 1u)) {
                    out[0] = '\0';
                    return false;
                }
            }
            if (!garnet_recover_append_bytes(out, effective_cap, &pos, in + start, tok_len)) {
                out[0] = '\0';
                return false;
            }
        }
    }

    if (pos > 0u) {
        if (!garnet_recover_append_bytes(out, effective_cap, &pos, " ", 1u)) {
            out[0] = '\0';
            return false;
        }
    }
    if (!garnet_recover_append_bytes(out, effective_cap, &pos, kDisable, disable_len)) {
        out[0] = '\0';
        return false;
    }

    *out_len = pos;
    return true;
}

static inline garnet_recover_choice_t garnet_recover_decode_key(uint16_t scan_code, uint16_t unicode_char) {
    if (scan_code == GARNET_EFI_SCAN_ESC || unicode_char == 0x001Bu ||
        unicode_char == (uint16_t)'q' || unicode_char == (uint16_t)'Q') {
        return GARNET_MENU_CANCEL;
    }
    if (unicode_char == (uint16_t)'1') {
        return GARNET_MENU_SAFE_BOOT_ONCE;
    }
    if (unicode_char == (uint16_t)'2') {
        return GARNET_MENU_SAFE_BOOT_UNINSTALL;
    }
    return GARNET_MENU_IGNORE;
}

#ifdef __cplusplus
}
#endif

#endif /* GARNET_RECOVER_PURE_H */
