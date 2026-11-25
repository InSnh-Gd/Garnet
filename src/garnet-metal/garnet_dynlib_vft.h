// garnet_dynlib_vft.h: pure-C Metal 3 dynamic library ('.grdl') codec, visible function table
// (0074) buffer encoder, reflection JSON parser, Stage 2 link cache key, and Footnote 10 checker.
// No Metal, no Foundation, no Vulkan — shared by GarnetDevice.m and Linux/macOS host C tests.
#ifndef RDNA4_DYNLIB_VFT_H
#define RDNA4_DYNLIB_VFT_H

#include <errno.h>
#include <fcntl.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "garnet_crc.h"

// =================================================================================================
// 1. Versioned Garnet dynamic library container ('.grdl', magic 'GRDL')
// Layout:
//   [rdna4dl_header_t (64 B)]
//   [install_name (install_name_len B, UTF-8, non-empty, no NUL)]
//   [symbol_count * (uint32_t len + len UTF-8 bytes)]
//   [module_count * (uint32_t kind + uint32_t size + size payload bytes)]
// Integrity:
//   - file_crc32    = CRC32(header[0..60) || bytes[64..total_size))
//   - payload_crc32 = CRC32 over all module payload bytes in order
// =================================================================================================

#define RDNA4DL_MAGIC              "GRDL"
#define RDNA4DL_FORMAT_VERSION     UINT32_C(1)
#define RDNA4DL_ABI_VERSION        UINT32_C(1)
#define RDNA4DL_MAX_INSTALL_NAME   UINT32_C(4096)
#define RDNA4DL_MAX_SYMBOLS        UINT32_C(4096)
#define RDNA4DL_MAX_SYMBOL_LEN     UINT32_C(4096)
#define RDNA4DL_MAX_MODULES        UINT32_C(256)
#define RDNA4DL_MAX_MODULE_BYTES   (UINT64_C(32) << 20)
#define RDNA4DL_MAX_FILE_BYTES     (UINT64_C(64) << 20)

enum {
    RDNA4DL_PAYLOAD_AIR_BITCODE = 1u,
    RDNA4DL_PAYLOAD_LLVM_IR     = 2u,
    RDNA4DL_PAYLOAD_METALLIB    = 3u,
};

typedef enum {
    RDNA4DL_OK = 0,
    RDNA4DL_ERR_INVALID,
    RDNA4DL_ERR_TRUNCATED,
    RDNA4DL_ERR_MAGIC,
    RDNA4DL_ERR_VERSION,
    RDNA4DL_ERR_ABI_MISMATCH,
    RDNA4DL_ERR_HEADER,
    RDNA4DL_ERR_CRC,
    RDNA4DL_ERR_PAYLOAD_CRC,
    RDNA4DL_ERR_BOUNDS,
    RDNA4DL_ERR_DUPLICATE,
    RDNA4DL_ERR_JSON,
    RDNA4DL_ERR_OOM,
    RDNA4DL_ERR_IO,
} rdna4dl_status_t;

static inline const char *rdna4dl_status_str(rdna4dl_status_t st) {
    switch (st) {
    case RDNA4DL_OK:               return "ok";
    case RDNA4DL_ERR_INVALID:      return "invalid dynamic library argument";
    case RDNA4DL_ERR_TRUNCATED:    return "truncated GRDL dynamic library";
    case RDNA4DL_ERR_MAGIC:        return "bad magic (not a Garnet GRDL dynamic library)";
    case RDNA4DL_ERR_VERSION:      return "unsupported GRDL format version";
    case RDNA4DL_ERR_ABI_MISMATCH: return "GRDL ABI version mismatch";
    case RDNA4DL_ERR_HEADER:       return "malformed GRDL header or payload metadata";
    case RDNA4DL_ERR_CRC:          return "GRDL whole-file CRC32 mismatch (corrupt/tampered)";
    case RDNA4DL_ERR_PAYLOAD_CRC:  return "GRDL payload CRC32 mismatch (corrupt/tampered)";
    case RDNA4DL_ERR_BOUNDS:       return "GRDL size or count out of bounds";
    case RDNA4DL_ERR_DUPLICATE:    return "duplicate exported symbol in GRDL dynamic library";
    case RDNA4DL_ERR_JSON:         return "malformed exported symbols JSON";
    case RDNA4DL_ERR_OOM:          return "out of memory";
    case RDNA4DL_ERR_IO:           return "GRDL file I/O failed";
    }
    return "unknown GRDL dynamic library error";
}

// ---- Little-endian wire helpers ----
static inline uint32_t rdna4dl_get_u32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}
static inline uint64_t rdna4dl_get_u64(const uint8_t *p) {
    return (uint64_t)rdna4dl_get_u32(p) | ((uint64_t)rdna4dl_get_u32(p + 4) << 32);
}
static inline void rdna4dl_put_u32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)(v & 0xFFu); p[1] = (uint8_t)((v >> 8) & 0xFFu);
    p[2] = (uint8_t)((v >> 16) & 0xFFu); p[3] = (uint8_t)((v >> 24) & 0xFFu);
}
static inline void rdna4dl_put_u64(uint8_t *p, uint64_t v) {
    rdna4dl_put_u32(p, (uint32_t)(v & 0xFFFFFFFFu));
    rdna4dl_put_u32(p + 4, (uint32_t)(v >> 32));
}

static inline int rdna4dl_payload_kind_ok(uint32_t kind) {
    return kind == RDNA4DL_PAYLOAD_AIR_BITCODE ||
           kind == RDNA4DL_PAYLOAD_LLVM_IR ||
           kind == RDNA4DL_PAYLOAD_METALLIB;
}

static inline int rdna4dl_is_bitcode_magic(const void *data, size_t len) {
    if (!data || len < 4u) return 0;
    const uint8_t *b = (const uint8_t *)data;
    // Raw LLVM bitcode ('B','C',0xC0,0xDE) or LLVM bitcode wrapper (0xDE,0xC0,0x17,0x0B).
    return (b[0] == 0x42u && b[1] == 0x43u && b[2] == 0xC0u && b[3] == 0xDEu) ||
           (b[0] == 0xDEu && b[1] == 0xC0u && b[2] == 0x17u && b[3] == 0x0Bu);
}

static inline int rdna4dl_is_metallib_magic(const void *data, size_t len) {
    if (!data || len < 4u) return 0;
    return memcmp(data, "MTLB", 4u) == 0;
}

typedef struct {
    char magic[4];
    uint32_t format_version;
    uint32_t header_size;
    uint32_t abi_version;
    uint32_t payload_kind;
    uint32_t flags;
    uint32_t install_name_len;
    uint32_t symbol_count;
    uint32_t module_count;
    uint32_t reserved;
    uint64_t total_size;
    uint64_t payload_bytes;
    uint32_t payload_crc32;
    uint32_t file_crc32;
} rdna4dl_header_t;
_Static_assert(sizeof(rdna4dl_header_t) == 64, "rdna4dl_header_t wire size");
_Static_assert(offsetof(rdna4dl_header_t, total_size) == 40, "rdna4dl_header_t total_size offset");
_Static_assert(offsetof(rdna4dl_header_t, file_crc32) == 60, "rdna4dl_header_t file_crc32 must trail header");

typedef struct {
    uint32_t kind;
    uint32_t size;
    uint8_t *data;
} rdna4dl_module_t;

typedef struct {
    char *install_name;
    char **symbols;
    uint32_t symbol_count;
    uint32_t symbol_cap;
    rdna4dl_module_t *modules;
    uint32_t module_count;
    uint32_t module_cap;
} rdna4dl_container_t;

static inline void rdna4dl_init(rdna4dl_container_t *c) {
    if (!c) return;
    memset(c, 0, sizeof *c);
    (void)rdna4crc32(0, "", 0); // prime CRC lookup table before concurrent operations
}

static inline void rdna4dl_destroy(rdna4dl_container_t *c) {
    if (!c) return;
    free(c->install_name);
    for (uint32_t i = 0; i < c->symbol_count; i++) free(c->symbols[i]);
    free(c->symbols);
    for (uint32_t i = 0; i < c->module_count; i++) free(c->modules[i].data);
    free(c->modules);
    memset(c, 0, sizeof *c);
}

static inline int rdna4dl_str_has_nul(const char *s, size_t len) {
    return len > 0 && memchr(s, '\0', len) != NULL;
}

static inline rdna4dl_status_t rdna4dl_set_install_name(rdna4dl_container_t *c,
                                                        const char *name, size_t len) {
    if (!c || !name || len == 0 || len > RDNA4DL_MAX_INSTALL_NAME) return RDNA4DL_ERR_INVALID;
    if (rdna4dl_str_has_nul(name, len)) return RDNA4DL_ERR_INVALID;
    char *copy = (char *)malloc(len + 1u);
    if (!copy) return RDNA4DL_ERR_OOM;
    memcpy(copy, name, len);
    copy[len] = '\0';
    free(c->install_name);
    c->install_name = copy;
    return RDNA4DL_OK;
}

static inline int rdna4dl_has_symbol(const rdna4dl_container_t *c, const char *sym) {
    if (!c || !sym || !sym[0]) return 0;
    for (uint32_t i = 0; i < c->symbol_count; i++) {
        if (strcmp(c->symbols[i], sym) == 0) return 1;
    }
    return 0;
}

static inline rdna4dl_status_t rdna4dl_add_symbol_len(rdna4dl_container_t *c,
                                                      const char *sym, size_t len,
                                                      int reject_duplicate) {
    if (!c || !sym || len == 0 || len > RDNA4DL_MAX_SYMBOL_LEN) return RDNA4DL_ERR_INVALID;
    if (rdna4dl_str_has_nul(sym, len)) return RDNA4DL_ERR_INVALID;
    for (uint32_t i = 0; i < c->symbol_count; i++) {
        if (strlen(c->symbols[i]) == len && memcmp(c->symbols[i], sym, len) == 0)
            return reject_duplicate ? RDNA4DL_ERR_DUPLICATE : RDNA4DL_OK;
    }
    if (c->symbol_count >= RDNA4DL_MAX_SYMBOLS) return RDNA4DL_ERR_BOUNDS;
    if (c->symbol_count == c->symbol_cap) {
        uint32_t ncap = c->symbol_cap ? (c->symbol_cap * 2u) : 8u;
        if (ncap > RDNA4DL_MAX_SYMBOLS) ncap = RDNA4DL_MAX_SYMBOLS;
        char **ns = (char **)realloc(c->symbols, (size_t)ncap * sizeof(*ns));
        if (!ns) return RDNA4DL_ERR_OOM;
        c->symbols = ns;
        c->symbol_cap = ncap;
    }
    char *copy = (char *)malloc(len + 1u);
    if (!copy) return RDNA4DL_ERR_OOM;
    memcpy(copy, sym, len);
    copy[len] = '\0';
    c->symbols[c->symbol_count++] = copy;
    return RDNA4DL_OK;
}

static inline rdna4dl_status_t rdna4dl_add_symbol(rdna4dl_container_t *c, const char *sym) {
    if (!sym) return RDNA4DL_ERR_INVALID;
    return rdna4dl_add_symbol_len(c, sym, strlen(sym), 0);
}

static inline uint64_t rdna4dl_total_payload_bytes(const rdna4dl_container_t *c) {
    uint64_t sum = 0;
    for (uint32_t i = 0; i < c->module_count; i++) sum += (uint64_t)c->modules[i].size;
    return sum;
}

static inline rdna4dl_status_t rdna4dl_add_module(rdna4dl_container_t *c, uint32_t kind,
                                                  const void *data, size_t size) {
    if (!c || !rdna4dl_payload_kind_ok(kind) || !data || size == 0) return RDNA4DL_ERR_INVALID;
    if ((uint64_t)size > RDNA4DL_MAX_MODULE_BYTES) return RDNA4DL_ERR_BOUNDS;
    if (c->module_count >= RDNA4DL_MAX_MODULES) return RDNA4DL_ERR_BOUNDS;
    if ((uint64_t)size > RDNA4DL_MAX_FILE_BYTES - rdna4dl_total_payload_bytes(c))
        return RDNA4DL_ERR_BOUNDS;
    if (c->module_count == c->module_cap) {
        uint32_t ncap = c->module_cap ? (c->module_cap * 2u) : 4u;
        if (ncap > RDNA4DL_MAX_MODULES) ncap = RDNA4DL_MAX_MODULES;
        rdna4dl_module_t *nm = (rdna4dl_module_t *)realloc(c->modules, (size_t)ncap * sizeof(*nm));
        if (!nm) return RDNA4DL_ERR_OOM;
        c->modules = nm;
        c->module_cap = ncap;
    }
    uint8_t *copy = (uint8_t *)malloc(size);
    if (!copy) return RDNA4DL_ERR_OOM;
    memcpy(copy, data, size);
    c->modules[c->module_count].kind = kind;
    c->modules[c->module_count].size = (uint32_t)size;
    c->modules[c->module_count].data = copy;
    c->module_count++;
    return RDNA4DL_OK;
}

static inline void rdna4dl_encode_header(uint8_t out[sizeof(rdna4dl_header_t)],
                                         const rdna4dl_header_t *h) {
    memcpy(out + 0, h->magic, 4u);
    rdna4dl_put_u32(out + 4, h->format_version);
    rdna4dl_put_u32(out + 8, h->header_size);
    rdna4dl_put_u32(out + 12, h->abi_version);
    rdna4dl_put_u32(out + 16, h->payload_kind);
    rdna4dl_put_u32(out + 20, h->flags);
    rdna4dl_put_u32(out + 24, h->install_name_len);
    rdna4dl_put_u32(out + 28, h->symbol_count);
    rdna4dl_put_u32(out + 32, h->module_count);
    rdna4dl_put_u32(out + 36, h->reserved);
    rdna4dl_put_u64(out + 40, h->total_size);
    rdna4dl_put_u64(out + 48, h->payload_bytes);
    rdna4dl_put_u32(out + 56, h->payload_crc32);
    rdna4dl_put_u32(out + 60, h->file_crc32);
}

static inline void rdna4dl_decode_header(const uint8_t in[sizeof(rdna4dl_header_t)],
                                         rdna4dl_header_t *h) {
    memcpy(h->magic, in + 0, 4u);
    h->format_version = rdna4dl_get_u32(in + 4);
    h->header_size = rdna4dl_get_u32(in + 8);
    h->abi_version = rdna4dl_get_u32(in + 12);
    h->payload_kind = rdna4dl_get_u32(in + 16);
    h->flags = rdna4dl_get_u32(in + 20);
    h->install_name_len = rdna4dl_get_u32(in + 24);
    h->symbol_count = rdna4dl_get_u32(in + 28);
    h->module_count = rdna4dl_get_u32(in + 32);
    h->reserved = rdna4dl_get_u32(in + 36);
    h->total_size = rdna4dl_get_u64(in + 40);
    h->payload_bytes = rdna4dl_get_u64(in + 48);
    h->payload_crc32 = rdna4dl_get_u32(in + 56);
    h->file_crc32 = rdna4dl_get_u32(in + 60);
}

static inline uint32_t rdna4dl_compute_file_crc(const uint8_t *buf, size_t len) {
    uint32_t crc = rdna4crc32(0, buf, offsetof(rdna4dl_header_t, file_crc32));
    if (len > sizeof(rdna4dl_header_t))
        crc = rdna4crc32(crc, buf + sizeof(rdna4dl_header_t), len - sizeof(rdna4dl_header_t));
    return crc;
}

static inline rdna4dl_status_t rdna4dl_serialize(const rdna4dl_container_t *c,
                                                 uint8_t **out_buf, size_t *out_len) {
    if (!c || !out_buf || !out_len) return RDNA4DL_ERR_INVALID;
    *out_buf = NULL;
    *out_len = 0;
    if (!c->install_name || !c->install_name[0]) return RDNA4DL_ERR_INVALID;
    if (c->module_count == 0 || c->module_count > RDNA4DL_MAX_MODULES) return RDNA4DL_ERR_INVALID;
    if (c->symbol_count > RDNA4DL_MAX_SYMBOLS) return RDNA4DL_ERR_BOUNDS;

    size_t iname_len = strlen(c->install_name);
    if (iname_len == 0 || iname_len > RDNA4DL_MAX_INSTALL_NAME) return RDNA4DL_ERR_BOUNDS;

    uint64_t total = sizeof(rdna4dl_header_t) + (uint64_t)iname_len;
    for (uint32_t i = 0; i < c->symbol_count; i++) {
        if (!c->symbols[i] || !c->symbols[i][0]) return RDNA4DL_ERR_INVALID;
        size_t slen = strlen(c->symbols[i]);
        if (slen > RDNA4DL_MAX_SYMBOL_LEN) return RDNA4DL_ERR_BOUNDS;
        uint64_t add = 4u + (uint64_t)slen;
        if (add > RDNA4DL_MAX_FILE_BYTES - total) return RDNA4DL_ERR_BOUNDS;
        total += add;
    }

    uint64_t payload_bytes = 0;
    uint32_t payload_crc = 0;
    for (uint32_t i = 0; i < c->module_count; i++) {
        const rdna4dl_module_t *m = &c->modules[i];
        if (!rdna4dl_payload_kind_ok(m->kind) || !m->data || m->size == 0 ||
            (uint64_t)m->size > RDNA4DL_MAX_MODULE_BYTES)
            return RDNA4DL_ERR_INVALID;
        uint64_t add = 8u + (uint64_t)m->size;
        if (add > RDNA4DL_MAX_FILE_BYTES - total) return RDNA4DL_ERR_BOUNDS;
        total += add;
        payload_bytes += (uint64_t)m->size;
        payload_crc = rdna4crc32(payload_crc, m->data, m->size);
    }

    uint8_t *buf = (uint8_t *)malloc((size_t)total);
    if (!buf) return RDNA4DL_ERR_OOM;

    size_t cursor = sizeof(rdna4dl_header_t);
    memcpy(buf + cursor, c->install_name, iname_len);
    cursor += iname_len;

    for (uint32_t i = 0; i < c->symbol_count; i++) {
        size_t slen = strlen(c->symbols[i]);
        rdna4dl_put_u32(buf + cursor, (uint32_t)slen);
        cursor += 4u;
        memcpy(buf + cursor, c->symbols[i], slen);
        cursor += slen;
    }

    for (uint32_t i = 0; i < c->module_count; i++) {
        const rdna4dl_module_t *m = &c->modules[i];
        rdna4dl_put_u32(buf + cursor, m->kind);
        rdna4dl_put_u32(buf + cursor + 4u, m->size);
        cursor += 8u;
        memcpy(buf + cursor, m->data, m->size);
        cursor += m->size;
    }

    rdna4dl_header_t h;
    memset(&h, 0, sizeof h);
    memcpy(h.magic, RDNA4DL_MAGIC, 4u);
    h.format_version = RDNA4DL_FORMAT_VERSION;
    h.header_size = (uint32_t)sizeof(rdna4dl_header_t);
    h.abi_version = RDNA4DL_ABI_VERSION;
    h.payload_kind = c->modules[0].kind;
    h.flags = 0u;
    h.install_name_len = (uint32_t)iname_len;
    h.symbol_count = c->symbol_count;
    h.module_count = c->module_count;
    h.reserved = 0u;
    h.total_size = total;
    h.payload_bytes = payload_bytes;
    h.payload_crc32 = payload_crc;
    h.file_crc32 = 0u;
    rdna4dl_encode_header(buf, &h);
    h.file_crc32 = rdna4dl_compute_file_crc(buf, (size_t)total);
    rdna4dl_put_u32(buf + offsetof(rdna4dl_header_t, file_crc32), h.file_crc32);

    *out_buf = buf;
    *out_len = (size_t)total;
    return RDNA4DL_OK;
}

static inline rdna4dl_status_t rdna4dl_deserialize(const void *raw, size_t len,
                                                   rdna4dl_container_t *out) {
    if (!raw || !out) return RDNA4DL_ERR_INVALID;
    if (len < sizeof(rdna4dl_header_t)) return RDNA4DL_ERR_TRUNCATED;
    if ((uint64_t)len > RDNA4DL_MAX_FILE_BYTES) return RDNA4DL_ERR_BOUNDS;

    const uint8_t *buf = (const uint8_t *)raw;
    rdna4dl_header_t h;
    rdna4dl_decode_header(buf, &h);

    if (memcmp(h.magic, RDNA4DL_MAGIC, 4u) != 0) return RDNA4DL_ERR_MAGIC;
    if (h.format_version != RDNA4DL_FORMAT_VERSION) return RDNA4DL_ERR_VERSION;
    if (h.abi_version != RDNA4DL_ABI_VERSION) return RDNA4DL_ERR_ABI_MISMATCH;
    if (h.header_size != sizeof(rdna4dl_header_t) || h.flags != 0u || h.reserved != 0u ||
        !rdna4dl_payload_kind_ok(h.payload_kind))
        return RDNA4DL_ERR_HEADER;
    if (h.total_size != (uint64_t)len)
        return (uint64_t)len < h.total_size ? RDNA4DL_ERR_TRUNCATED : RDNA4DL_ERR_BOUNDS;
    if (h.install_name_len == 0 || h.install_name_len > RDNA4DL_MAX_INSTALL_NAME ||
        h.symbol_count > RDNA4DL_MAX_SYMBOLS ||
        h.module_count == 0 || h.module_count > RDNA4DL_MAX_MODULES)
        return RDNA4DL_ERR_BOUNDS;

    // Whole-file CRC32 runs before decoding variable-length body tables.
    if (rdna4dl_compute_file_crc(buf, len) != h.file_crc32)
        return RDNA4DL_ERR_CRC;

    size_t cursor = sizeof(rdna4dl_header_t);
    if ((size_t)h.install_name_len > len - cursor) return RDNA4DL_ERR_TRUNCATED;

    rdna4dl_container_t tmp;
    rdna4dl_init(&tmp);
    rdna4dl_status_t st = rdna4dl_set_install_name(&tmp, (const char *)(buf + cursor),
                                                   (size_t)h.install_name_len);
    if (st != RDNA4DL_OK) { rdna4dl_destroy(&tmp); return st; }
    cursor += (size_t)h.install_name_len;

    for (uint32_t i = 0; i < h.symbol_count; i++) {
        if (4u > len - cursor) { rdna4dl_destroy(&tmp); return RDNA4DL_ERR_TRUNCATED; }
        uint32_t slen = rdna4dl_get_u32(buf + cursor);
        cursor += 4u;
        if (slen == 0 || slen > RDNA4DL_MAX_SYMBOL_LEN) {
            rdna4dl_destroy(&tmp);
            return RDNA4DL_ERR_BOUNDS;
        }
        if ((size_t)slen > len - cursor) { rdna4dl_destroy(&tmp); return RDNA4DL_ERR_TRUNCATED; }
        st = rdna4dl_add_symbol_len(&tmp, (const char *)(buf + cursor), (size_t)slen, 1);
        if (st != RDNA4DL_OK) { rdna4dl_destroy(&tmp); return st; }
        cursor += (size_t)slen;
    }

    uint64_t seen_payload_bytes = 0;
    uint32_t payload_crc = 0;
    for (uint32_t i = 0; i < h.module_count; i++) {
        if (8u > len - cursor) { rdna4dl_destroy(&tmp); return RDNA4DL_ERR_TRUNCATED; }
        uint32_t mkind = rdna4dl_get_u32(buf + cursor);
        uint32_t msize = rdna4dl_get_u32(buf + cursor + 4u);
        cursor += 8u;
        if (!rdna4dl_payload_kind_ok(mkind) || (i == 0 && mkind != h.payload_kind)) {
            rdna4dl_destroy(&tmp);
            return RDNA4DL_ERR_HEADER;
        }
        if (msize == 0 || (uint64_t)msize > RDNA4DL_MAX_MODULE_BYTES) {
            rdna4dl_destroy(&tmp);
            return RDNA4DL_ERR_BOUNDS;
        }
        if ((size_t)msize > len - cursor) { rdna4dl_destroy(&tmp); return RDNA4DL_ERR_TRUNCATED; }
        payload_crc = rdna4crc32(payload_crc, buf + cursor, (size_t)msize);
        seen_payload_bytes += (uint64_t)msize;
        st = rdna4dl_add_module(&tmp, mkind, buf + cursor, (size_t)msize);
        if (st != RDNA4DL_OK) { rdna4dl_destroy(&tmp); return st; }
        cursor += (size_t)msize;
    }

    if (cursor != len || seen_payload_bytes != h.payload_bytes) {
        rdna4dl_destroy(&tmp);
        return RDNA4DL_ERR_BOUNDS;
    }
    if (payload_crc != h.payload_crc32) {
        rdna4dl_destroy(&tmp);
        return RDNA4DL_ERR_PAYLOAD_CRC;
    }

    *out = tmp;
    return RDNA4DL_OK;
}

static inline rdna4dl_status_t rdna4dl_write_file_atomic(const rdna4dl_container_t *c,
                                                         const char *path, int *out_errno) {
    if (out_errno) *out_errno = 0;
    if (!c || !path || !path[0]) {
        if (out_errno) *out_errno = EINVAL;
        return RDNA4DL_ERR_INVALID;
    }
    uint8_t *buf = NULL;
    size_t len = 0;
    rdna4dl_status_t st = rdna4dl_serialize(c, &buf, &len);
    if (st != RDNA4DL_OK) return st;

    char tmp[4096];
    int n = snprintf(tmp, sizeof tmp, "%s.tmp.XXXXXX", path);
    if (n <= 0 || (size_t)n >= sizeof tmp) {
        free(buf);
        if (out_errno) *out_errno = ENAMETOOLONG;
        return RDNA4DL_ERR_IO;
    }
    int fd = mkstemp(tmp);
    if (fd < 0) {
        int en = errno;
        free(buf);
        if (out_errno) *out_errno = en;
        return RDNA4DL_ERR_IO;
    }

    int fail = 0;
    size_t written = 0;
    while (written < len) {
        ssize_t w = write(fd, buf + written, len - written);
        if (w < 0 && errno == EINTR) continue;
        if (w <= 0) { fail = (w == 0) ? EIO : errno; break; }
        written += (size_t)w;
    }
    free(buf);
    if (!fail && fsync(fd) != 0) fail = errno;
    if (!fail && fchmod(fd, 0644) != 0) fail = errno;
    if (close(fd) != 0 && !fail) fail = errno;
    if (!fail && rename(tmp, path) != 0) fail = errno;
    if (fail) {
        (void)unlink(tmp);
        if (out_errno) *out_errno = fail;
        return RDNA4DL_ERR_IO;
    }
    return RDNA4DL_OK;
}

static inline rdna4dl_status_t rdna4dl_read_file(const char *path, rdna4dl_container_t *out,
                                                 int *out_errno) {
    if (out_errno) *out_errno = 0;
    if (!path || !path[0] || !out) {
        if (out_errno) *out_errno = EINVAL;
        return RDNA4DL_ERR_INVALID;
    }
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) { if (out_errno) *out_errno = errno; return RDNA4DL_ERR_IO; }
    struct stat sb;
    if (fstat(fd, &sb) != 0) {
        int en = errno;
        close(fd);
        if (out_errno) *out_errno = en;
        return RDNA4DL_ERR_IO;
    }
    if (!S_ISREG(sb.st_mode)) {
        close(fd);
        if (out_errno) *out_errno = EINVAL;
        return RDNA4DL_ERR_INVALID;
    }
    if (sb.st_size < (off_t)sizeof(rdna4dl_header_t)) {
        close(fd);
        return RDNA4DL_ERR_TRUNCATED;
    }
    if ((uint64_t)sb.st_size > RDNA4DL_MAX_FILE_BYTES) {
        close(fd);
        return RDNA4DL_ERR_BOUNDS;
    }

    size_t len = (size_t)sb.st_size;
    uint8_t *buf = (uint8_t *)malloc(len);
    if (!buf) { close(fd); return RDNA4DL_ERR_OOM; }

    size_t got = 0;
    while (got < len) {
        ssize_t r = read(fd, buf + got, len - got);
        if (r < 0 && errno == EINTR) continue;
        if (r < 0) {
            int en = errno;
            free(buf);
            close(fd);
            if (out_errno) *out_errno = en;
            return RDNA4DL_ERR_IO;
        }
        if (r == 0) break;
        got += (size_t)r;
    }
    uint8_t extra = 0;
    ssize_t tail = 0;
    if (got == len) {
        do { tail = read(fd, &extra, 1u); } while (tail < 0 && errno == EINTR);
    }
    close(fd);
    if (got != len) { free(buf); return RDNA4DL_ERR_TRUNCATED; }
    if (tail != 0) { free(buf); return tail > 0 ? RDNA4DL_ERR_BOUNDS : RDNA4DL_ERR_IO; }

    rdna4dl_status_t st = rdna4dl_deserialize(buf, len, out);
    free(buf);
    return st;
}

// =================================================================================================
// 2. Pure-C Stage 2 SHA-256 link cache key (deps + dynlibs + visible functions)
// =================================================================================================

typedef struct {
    uint32_t state[8];
    uint64_t bitlen;
    uint8_t buf[64];
    size_t used;
} rdna4dl_sha256_ctx;

static inline uint32_t rdna4dl_rotr32(uint32_t x, unsigned n) {
    return (x >> n) | (x << (32u - n));
}

static inline void rdna4dl_sha256_block(rdna4dl_sha256_ctx *ctx, const uint8_t blk[64]) {
    static const uint32_t k[64] = {
        0x428a2f98u, 0x71374491u, 0xb5c0fbcfu, 0xe9b5dba5u, 0x3956c25bu, 0x59f111f1u, 0x923f82a4u, 0xab1c5ed5u,
        0xd807aa98u, 0x12835b01u, 0x243185beu, 0x550c7dc3u, 0x72be5d74u, 0x80deb1feu, 0x9bdc06a7u, 0xc19bf174u,
        0xe49b69c1u, 0xefbe4786u, 0x0fc19dc6u, 0x240ca1ccu, 0x2de92c6fu, 0x4a7484aau, 0x5cb0a9dcu, 0x76f988dau,
        0x983e5152u, 0xa831c66du, 0xb00327c8u, 0xbf597fc7u, 0xc6e00bf3u, 0xd5a79147u, 0x06ca6351u, 0x14292967u,
        0x27b70a85u, 0x2e1b2138u, 0x4d2c6dfcu, 0x53380d13u, 0x650a7354u, 0x766a0abbu, 0x81c2c92eu, 0x92722c85u,
        0xa2bfe8a1u, 0xa81a664bu, 0xc24b8b70u, 0xc76c51a3u, 0xd192e819u, 0xd6990624u, 0xf40e3585u, 0x106aa070u,
        0x19a4c116u, 0x1e376c08u, 0x2748774cu, 0x34b0bcb5u, 0x391c0cb3u, 0x4ed8aa4au, 0x5b9cca4fu, 0x682e6ff3u,
        0x748f82eeu, 0x78a5636fu, 0x84c87814u, 0x8cc70208u, 0x90befffau, 0xa4506cebu, 0xbef9a3f7u, 0xc67178f2u,
    };
    uint32_t w[64];
    for (size_t i = 0; i < 16u; i++) {
        w[i] = ((uint32_t)blk[i * 4u] << 24) | ((uint32_t)blk[i * 4u + 1u] << 16) |
               ((uint32_t)blk[i * 4u + 2u] << 8) | (uint32_t)blk[i * 4u + 3u];
    }
    for (size_t i = 16u; i < 64u; i++) {
        uint32_t s0 = rdna4dl_rotr32(w[i - 15u], 7u) ^ rdna4dl_rotr32(w[i - 15u], 18u) ^ (w[i - 15u] >> 3);
        uint32_t s1 = rdna4dl_rotr32(w[i - 2u], 17u) ^ rdna4dl_rotr32(w[i - 2u], 19u) ^ (w[i - 2u] >> 10);
        w[i] = w[i - 16u] + s0 + w[i - 7u] + s1;
    }
    uint32_t a = ctx->state[0], b = ctx->state[1], d = ctx->state[2], e = ctx->state[3];
    uint32_t f = ctx->state[4], g = ctx->state[5], h = ctx->state[6], j = ctx->state[7];
    for (size_t i = 0; i < 64u; i++) {
        uint32_t S1 = rdna4dl_rotr32(f, 6u) ^ rdna4dl_rotr32(f, 11u) ^ rdna4dl_rotr32(f, 25u);
        uint32_t ch = (f & g) ^ ((~f) & h);
        uint32_t t1 = j + S1 + ch + k[i] + w[i];
        uint32_t S0 = rdna4dl_rotr32(a, 2u) ^ rdna4dl_rotr32(a, 13u) ^ rdna4dl_rotr32(a, 22u);
        uint32_t maj = (a & b) ^ (a & d) ^ (b & d);
        uint32_t t2 = S0 + maj;
        j = h; h = g; g = f; f = e + t1; e = d; d = b; b = a; a = t1 + t2;
    }
    ctx->state[0] += a; ctx->state[1] += b; ctx->state[2] += d; ctx->state[3] += e;
    ctx->state[4] += f; ctx->state[5] += g; ctx->state[6] += h; ctx->state[7] += j;
}

static inline void rdna4dl_sha256_init(rdna4dl_sha256_ctx *ctx) {
    ctx->state[0] = 0x6a09e667u; ctx->state[1] = 0xbb67ae85u;
    ctx->state[2] = 0x3c6ef372u; ctx->state[3] = 0xa54ff53au;
    ctx->state[4] = 0x510e527fu; ctx->state[5] = 0x9b05688cu;
    ctx->state[6] = 0x1f83d9abu; ctx->state[7] = 0x5be0cd19u;
    ctx->bitlen = 0;
    ctx->used = 0;
}

static inline void rdna4dl_sha256_update(rdna4dl_sha256_ctx *ctx, const void *data, size_t len) {
    const uint8_t *p = (const uint8_t *)data;
    if (!len) return;
    ctx->bitlen += (uint64_t)len * 8u;
    if (ctx->used) {
        size_t take = 64u - ctx->used;
        if (take > len) take = len;
        memcpy(ctx->buf + ctx->used, p, take);
        ctx->used += take; p += take; len -= take;
        if (ctx->used == 64u) { rdna4dl_sha256_block(ctx, ctx->buf); ctx->used = 0; }
    }
    while (len >= 64u) { rdna4dl_sha256_block(ctx, p); p += 64u; len -= 64u; }
    if (len) { memcpy(ctx->buf, p, len); ctx->used = len; }
}

static inline void rdna4dl_sha256_final(rdna4dl_sha256_ctx *ctx, uint8_t out[32]) {
    size_t i = ctx->used;
    ctx->buf[i++] = 0x80u;
    if (i > 56u) {
        memset(ctx->buf + i, 0, 64u - i);
        rdna4dl_sha256_block(ctx, ctx->buf);
        i = 0;
    }
    memset(ctx->buf + i, 0, 56u - i);
    for (size_t k = 0; k < 8u; k++)
        ctx->buf[56u + k] = (uint8_t)(ctx->bitlen >> (56u - k * 8u));
    rdna4dl_sha256_block(ctx, ctx->buf);
    for (size_t k = 0; k < 8u; k++) {
        out[k * 4u + 0u] = (uint8_t)(ctx->state[k] >> 24);
        out[k * 4u + 1u] = (uint8_t)(ctx->state[k] >> 16);
        out[k * 4u + 2u] = (uint8_t)(ctx->state[k] >> 8);
        out[k * 4u + 3u] = (uint8_t)(ctx->state[k]);
    }
}

static inline int rdna4dl_hex_sha_ok(const char *s) {
    if (!s) return 0;
    for (size_t i = 0; i < 64u; i++) {
        char ch = s[i];
        if (!((ch >= '0' && ch <= '9') || (ch >= 'a' && ch <= 'f'))) return 0;
    }
    return s[64] == '\0';
}

typedef struct {
    uint8_t section;
    const char *name;
    const char *sha;
} rdna4dl_key_item_t;

static inline int rdna4dl_key_item_cmp(const void *a, const void *b) {
    const rdna4dl_key_item_t *x = (const rdna4dl_key_item_t *)a;
    const rdna4dl_key_item_t *y = (const rdna4dl_key_item_t *)b;
    if (x->section != y->section) return x->section < y->section ? -1 : 1;
    int cmp = strcmp(x->name, y->name);
    return cmp ? cmp : strcmp(x->sha, y->sha);
}

// Order-independent, deduplicated Stage 2 translation cache key covering entry + deps + dynlibs + visible_fns.
static inline int rdna4dl_stage2_link_sha(
    char out[65],
    const char *entry_sha,
    size_t ndeps, const char *const *dep_names, const char *const *dep_shas,
    size_t ndynlibs, const char *const *dynlib_names, const char *const *dynlib_shas,
    size_t nvis, const char *const *vis_names, const char *const *vis_shas)
{
    if (!out) return -1;
    out[0] = '\0';
    if (!rdna4dl_hex_sha_ok(entry_sha)) return -1;
    if (ndeps > 4096u || ndynlibs > 4096u || nvis > 4096u) return -1;
    if ((ndeps && (!dep_names || !dep_shas)) ||
        (ndynlibs && (!dynlib_names || !dynlib_shas)) ||
        (nvis && (!vis_names || !vis_shas)))
        return -1;

    size_t total = ndeps + ndynlibs + nvis;
    rdna4dl_key_item_t *items = (rdna4dl_key_item_t *)calloc(total ? total : 1u, sizeof *items);
    if (!items) return -1;

    size_t pos = 0;
    for (size_t i = 0; i < ndeps; i++) {
        if (!dep_names[i] || !dep_names[i][0] || !rdna4dl_hex_sha_ok(dep_shas[i])) {
            free(items);
            return -1;
        }
        items[pos++] = (rdna4dl_key_item_t){ 1u, dep_names[i], dep_shas[i] };
    }
    for (size_t i = 0; i < ndynlibs; i++) {
        if (!dynlib_names[i] || !dynlib_names[i][0] || !rdna4dl_hex_sha_ok(dynlib_shas[i])) {
            free(items);
            return -1;
        }
        items[pos++] = (rdna4dl_key_item_t){ 2u, dynlib_names[i], dynlib_shas[i] };
    }
    for (size_t i = 0; i < nvis; i++) {
        if (!vis_names[i] || !vis_names[i][0] || !rdna4dl_hex_sha_ok(vis_shas[i])) {
            free(items);
            return -1;
        }
        items[pos++] = (rdna4dl_key_item_t){ 3u, vis_names[i], vis_shas[i] };
    }

    if (total > 1u) qsort(items, total, sizeof *items, rdna4dl_key_item_cmp);

    rdna4dl_sha256_ctx ctx;
    rdna4dl_sha256_init(&ctx);
    static const char tag[] = "rdna4x-stage2-v1\n";
    rdna4dl_sha256_update(&ctx, tag, sizeof tag - 1u);
    rdna4dl_sha256_update(&ctx, entry_sha, 64u);
    rdna4dl_sha256_update(&ctx, "\n", 1u);
    for (size_t i = 0; i < total; i++) {
        if (i > 0 && rdna4dl_key_item_cmp(&items[i], &items[i - 1u]) == 0) continue;
        uint8_t sec = items[i].section;
        rdna4dl_sha256_update(&ctx, &sec, 1u);
        rdna4dl_sha256_update(&ctx, items[i].name, strlen(items[i].name));
        rdna4dl_sha256_update(&ctx, "\0", 1u);
        rdna4dl_sha256_update(&ctx, items[i].sha, 64u);
        rdna4dl_sha256_update(&ctx, "\n", 1u);
    }
    free(items);

    uint8_t digest[32];
    rdna4dl_sha256_final(&ctx, digest);
    static const char hex[] = "0123456789abcdef";
    for (size_t i = 0; i < 32u; i++) {
        out[i * 2u] = hex[digest[i] >> 4];
        out[i * 2u + 1u] = hex[digest[i] & 0x0Fu];
    }
    out[64] = '\0';
    return 0;
}

// =================================================================================================
// 3. Visible Function Table (0074 ABI) buffer encoder, Footnote 10 checker, & JSON parsers
// =================================================================================================

#define RDNA4VFT_ABI_VERSION        UINT32_C(1)
#define RDNA4VFT_HEADER_WORDS       UINT32_C(1)
#define RDNA4VFT_HANDLE_NULL        UINT32_C(0)
#define RDNA4VFT_MAX_SLOTS          UINT32_C(65536)
#define RDNA4VFT_MAX_HANDLES        UINT32_C(4096)
#define RDNA4VFT_MAX_TABLES         UINT32_C(31)
#define RDNA4VFT_MAX_ATTACH_BUFFERS UINT32_C(31)
#define RDNA4VFT_MAX_NESTED_TABLES  UINT32_C(31)
#define RDNA4VFT_JSON_MAX_DEPTH     64u

typedef enum {
    RDNA4VFT_OK = 0,
    RDNA4VFT_ERR_INVALID,
    RDNA4VFT_ERR_OOB,
    RDNA4VFT_ERR_INVALID_HANDLE,
    RDNA4VFT_ERR_BOUNDS,
    RDNA4VFT_ERR_ABI,
    RDNA4VFT_ERR_JSON,
    RDNA4VFT_ERR_DUPLICATE,
    RDNA4VFT_ERR_FOOTNOTE10_MESH_FN_PTR,
    RDNA4VFT_ERR_FOOTNOTE10_MESH_RT,
    RDNA4VFT_ERR_OOM,
} rdna4vft_status_t;

static inline const char *rdna4vft_status_str(rdna4vft_status_t st) {
    switch (st) {
    case RDNA4VFT_OK:                        return "ok";
    case RDNA4VFT_ERR_INVALID:               return "invalid visible function table argument";
    case RDNA4VFT_ERR_OOB:                   return "visible function table slot index out of bounds";
    case RDNA4VFT_ERR_INVALID_HANDLE:        return "invalid visible function handle id for pipeline";
    case RDNA4VFT_ERR_BOUNDS:                return "visible function table size or buffer capacity exceeded";
    case RDNA4VFT_ERR_ABI:                   return "unsupported visible function table reflection ABI";
    case RDNA4VFT_ERR_JSON:                  return "malformed visible function table reflection JSON";
    case RDNA4VFT_ERR_DUPLICATE:             return "duplicate visible function handle or symbol";
    case RDNA4VFT_ERR_FOOTNOTE10_MESH_FN_PTR:
        return "Footnote 10: mesh/object render pipelines cannot combine render function pointers or visible function tables";
    case RDNA4VFT_ERR_FOOTNOTE10_MESH_RT:
        return "Footnote 10: mesh/object render pipelines cannot combine render-stage ray tracing";
    case RDNA4VFT_ERR_OOM:                   return "out of memory";
    }
    return "unknown visible function table error";
}

// GPU storage-buffer layout matching patch 0074:
//   word[0]        = slot_count (0 for empty/null table; >= 4 bytes bound)
//   word[1 + slot] = 1-based handle_id (0 = null/unbound slot -> safe zero/no-op default)
typedef struct {
    uint32_t slot_count;
    uint32_t max_handle_id;
    uint32_t *slots;
} rdna4vft_table_t;

static inline size_t rdna4vft_buffer_words(uint32_t slot_count) {
    return (size_t)RDNA4VFT_HEADER_WORDS + (size_t)slot_count;
}

static inline size_t rdna4vft_buffer_bytes(uint32_t slot_count) {
    return rdna4vft_buffer_words(slot_count) * sizeof(uint32_t);
}

static inline rdna4vft_status_t rdna4vft_init(rdna4vft_table_t *t, uint32_t slot_count,
                                              uint32_t max_handle_id) {
    if (!t) return RDNA4VFT_ERR_INVALID;
    memset(t, 0, sizeof *t);
    if (slot_count > RDNA4VFT_MAX_SLOTS || max_handle_id > RDNA4VFT_MAX_HANDLES)
        return RDNA4VFT_ERR_BOUNDS;
    if (slot_count > 0) {
        t->slots = (uint32_t *)calloc((size_t)slot_count, sizeof(uint32_t));
        if (!t->slots) return RDNA4VFT_ERR_OOM;
    }
    t->slot_count = slot_count;
    t->max_handle_id = max_handle_id;
    return RDNA4VFT_OK;
}

static inline void rdna4vft_destroy(rdna4vft_table_t *t) {
    if (!t) return;
    free(t->slots);
    memset(t, 0, sizeof *t);
}

static inline rdna4vft_status_t rdna4vft_set_slot(rdna4vft_table_t *t, uint32_t slot,
                                                  uint32_t handle_id) {
    if (!t) return RDNA4VFT_ERR_INVALID;
    if (slot >= t->slot_count) return RDNA4VFT_ERR_OOB;
    if (handle_id != RDNA4VFT_HANDLE_NULL && handle_id > t->max_handle_id)
        return RDNA4VFT_ERR_INVALID_HANDLE;
    t->slots[slot] = handle_id;
    return RDNA4VFT_OK;
}

static inline rdna4vft_status_t rdna4vft_set_range(rdna4vft_table_t *t, uint32_t start,
                                                   uint32_t count, const uint32_t *handle_ids) {
    if (!t) return RDNA4VFT_ERR_INVALID;
    if (start > t->slot_count || count > t->slot_count - start) return RDNA4VFT_ERR_OOB;
    if (count == 0) return RDNA4VFT_OK;
    if (!handle_ids) return RDNA4VFT_ERR_INVALID;
    for (uint32_t i = 0; i < count; i++) {
        if (handle_ids[i] != RDNA4VFT_HANDLE_NULL && handle_ids[i] > t->max_handle_id)
            return RDNA4VFT_ERR_INVALID_HANDLE;
    }
    memcpy(t->slots + start, handle_ids, (size_t)count * sizeof(uint32_t));
    return RDNA4VFT_OK;
}

static inline uint32_t rdna4vft_get_slot(const rdna4vft_table_t *t, uint32_t slot) {
    if (!t || slot >= t->slot_count || !t->slots) return RDNA4VFT_HANDLE_NULL;
    return t->slots[slot];
}

static inline rdna4vft_status_t rdna4vft_encode_buffer(const rdna4vft_table_t *t,
                                                       uint32_t *dst_words, size_t dst_word_cap,
                                                       size_t *out_words) {
    if (out_words) *out_words = 0;
    if (!t || !dst_words) return RDNA4VFT_ERR_INVALID;
    size_t need = rdna4vft_buffer_words(t->slot_count);
    if (dst_word_cap < need) return RDNA4VFT_ERR_BOUNDS;
    dst_words[0] = t->slot_count;
    if (t->slot_count > 0)
        memcpy(dst_words + RDNA4VFT_HEADER_WORDS, t->slots, (size_t)t->slot_count * sizeof(uint32_t));
    if (out_words) *out_words = need;
    return RDNA4VFT_OK;
}

// Exact host mirror of patch 0074 shader lookup: clamps OOB index to 0 and returns 0 on OOB/null.
static inline uint32_t rdna4vft_simulate_lookup(const uint32_t *buf_words, size_t buf_word_count,
                                                uint32_t slot) {
    if (!buf_words || buf_word_count < RDNA4VFT_HEADER_WORDS) return RDNA4VFT_HANDLE_NULL;
    uint32_t sz = buf_words[0];
    if ((size_t)sz > buf_word_count - RDNA4VFT_HEADER_WORDS)
        sz = (uint32_t)(buf_word_count - RDNA4VFT_HEADER_WORDS);
    int ok = slot < sz;
    uint32_t ix = ok ? (slot + RDNA4VFT_HEADER_WORDS) : 0u;
    uint32_t raw = buf_words[ix];
    return ok ? raw : RDNA4VFT_HANDLE_NULL;
}

// ---- Footnote 10 (AMD 6000-series / Metal 3) mesh vs render fn-ptr / RT exclusion ----
enum {
    RDNA4FN10_HAS_OBJECT_STAGE      = UINT32_C(1) << 0,
    RDNA4FN10_HAS_MESH_STAGE        = UINT32_C(1) << 1,
    RDNA4FN10_HAS_VERTEX_FN_PTRS    = UINT32_C(1) << 2,
    RDNA4FN10_HAS_FRAGMENT_FN_PTRS  = UINT32_C(1) << 3,
    RDNA4FN10_HAS_OBJECT_FN_PTRS    = UINT32_C(1) << 4,
    RDNA4FN10_HAS_MESH_FN_PTRS      = UINT32_C(1) << 5,
    RDNA4FN10_HAS_RENDER_RAYTRACING = UINT32_C(1) << 6,
    RDNA4FN10_SUPPORT_ADD_BIN_FNS   = UINT32_C(1) << 7,
};

static inline rdna4vft_status_t rdna4vft_check_footnote10(uint32_t flags, const char **why_out) {
    if (why_out) *why_out = NULL;
    const uint32_t mesh_stages = RDNA4FN10_HAS_OBJECT_STAGE | RDNA4FN10_HAS_MESH_STAGE;
    const uint32_t fn_ptr_bits = RDNA4FN10_HAS_VERTEX_FN_PTRS | RDNA4FN10_HAS_FRAGMENT_FN_PTRS |
                                 RDNA4FN10_HAS_OBJECT_FN_PTRS | RDNA4FN10_HAS_MESH_FN_PTRS |
                                 RDNA4FN10_SUPPORT_ADD_BIN_FNS;
    if (flags & mesh_stages) {
        if (flags & fn_ptr_bits) {
            if (why_out) *why_out = rdna4vft_status_str(RDNA4VFT_ERR_FOOTNOTE10_MESH_FN_PTR);
            return RDNA4VFT_ERR_FOOTNOTE10_MESH_FN_PTR;
        }
        if (flags & RDNA4FN10_HAS_RENDER_RAYTRACING) {
            if (why_out) *why_out = rdna4vft_status_str(RDNA4VFT_ERR_FOOTNOTE10_MESH_RT);
            return RDNA4VFT_ERR_FOOTNOTE10_MESH_RT;
        }
    } else if (flags & (RDNA4FN10_HAS_OBJECT_FN_PTRS | RDNA4FN10_HAS_MESH_FN_PTRS)) {
        if (why_out) *why_out = rdna4vft_status_str(RDNA4VFT_ERR_FOOTNOTE10_MESH_FN_PTR);
        return RDNA4VFT_ERR_FOOTNOTE10_MESH_FN_PTR;
    }
    return RDNA4VFT_OK;
}

// ---- Reflection structures for `"visible_function_tables"` ----
typedef struct {
    uint32_t param_index;
    uint32_t metal_index;
    int has_descriptor;
    uint32_t desc_set;
    uint32_t desc_binding;
    uint32_t desc_count;
} rdna4vft_table_binding_t;

typedef struct {
    uint32_t handle_id;
    char *symbol;
} rdna4vft_handle_entry_t;

typedef struct {
    int present;
    uint32_t abi_version;
    uint32_t header_words;
    uint32_t max_handle_id;
    rdna4vft_table_binding_t tables[RDNA4VFT_MAX_TABLES];
    uint32_t table_count;
    rdna4vft_handle_entry_t *handles;
    uint32_t handle_count;
    uint32_t handle_cap;
} rdna4vft_reflection_t;

static inline void rdna4vft_refl_init(rdna4vft_reflection_t *r) {
    if (r) memset(r, 0, sizeof *r);
}

static inline void rdna4vft_refl_destroy(rdna4vft_reflection_t *r) {
    if (!r) return;
    for (uint32_t i = 0; i < r->handle_count; i++) free(r->handles[i].symbol);
    free(r->handles);
    memset(r, 0, sizeof *r);
}

static inline uint32_t rdna4vft_refl_find_handle(const rdna4vft_reflection_t *r, const char *sym) {
    if (!r || !r->present || !sym || !sym[0]) return RDNA4VFT_HANDLE_NULL;
    for (uint32_t i = 0; i < r->handle_count; i++) {
        if (strcmp(r->handles[i].symbol, sym) == 0) return r->handles[i].handle_id;
    }
    return RDNA4VFT_HANDLE_NULL;
}

static inline const char *rdna4vft_refl_find_symbol(const rdna4vft_reflection_t *r,
                                                    uint32_t handle_id) {
    if (!r || !r->present || handle_id == RDNA4VFT_HANDLE_NULL) return NULL;
    for (uint32_t i = 0; i < r->handle_count; i++) {
        if (r->handles[i].handle_id == handle_id) return r->handles[i].symbol;
    }
    return NULL;
}

static inline int rdna4vft_refl_has_table_metal_index(const rdna4vft_reflection_t *r,
                                                      uint32_t metal_index) {
    if (!r || !r->present) return 0;
    for (uint32_t i = 0; i < r->table_count; i++) {
        if (r->tables[i].metal_index == metal_index) return 1;
    }
    return 0;
}

// ---- Minimal strict JSON scanner shared by symbols & VFT reflection parsers ----
typedef struct {
    const char *s;
    size_t len;
    size_t pos;
} rdna4j_cur_t;

static inline void rdna4j_ws(rdna4j_cur_t *cur) {
    while (cur->pos < cur->len) {
        char ch = cur->s[cur->pos];
        if (ch == ' ' || ch == '\t' || ch == '\r' || ch == '\n') cur->pos++;
        else break;
    }
}

static inline int rdna4j_peek(rdna4j_cur_t *cur) {
    rdna4j_ws(cur);
    return cur->pos < cur->len ? (unsigned char)cur->s[cur->pos] : -1;
}

static inline int rdna4j_consume(rdna4j_cur_t *cur, char ch) {
    rdna4j_ws(cur);
    if (cur->pos < cur->len && cur->s[cur->pos] == ch) {
        cur->pos++;
        return 1;
    }
    return 0;
}

static inline int rdna4j_hex_digit(char ch, uint32_t *out) {
    if (ch >= '0' && ch <= '9') { *out = (uint32_t)(ch - '0'); return 1; }
    if (ch >= 'a' && ch <= 'f') { *out = (uint32_t)(10 + (ch - 'a')); return 1; }
    if (ch >= 'A' && ch <= 'F') { *out = (uint32_t)(10 + (ch - 'A')); return 1; }
    return 0;
}

static inline int rdna4j_parse_string(rdna4j_cur_t *cur, char *out, size_t cap, size_t *out_len) {
    if (!rdna4j_consume(cur, '"')) return 0;
    size_t n = 0;
    while (cur->pos < cur->len) {
        unsigned char ch = (unsigned char)cur->s[cur->pos++];
        if (ch == '"') {
            if (out && cap > 0) {
                if (n >= cap) return 0;
                out[n] = '\0';
            }
            if (out_len) *out_len = n;
            return 1;
        }
        if (ch < 0x20u) return 0;
        if (ch != '\\') {
            if (out) {
                if (n + 1u >= cap) return 0;
                out[n] = (char)ch;
            }
            n++;
            continue;
        }
        if (cur->pos >= cur->len) return 0;
        char esc = cur->s[cur->pos++];
        char mapped = 0;
        switch (esc) {
        case '"':  mapped = '"'; break;
        case '\\': mapped = '\\'; break;
        case '/':  mapped = '/'; break;
        case 'b':  mapped = '\b'; break;
        case 'f':  mapped = '\f'; break;
        case 'n':  mapped = '\n'; break;
        case 'r':  mapped = '\r'; break;
        case 't':  mapped = '\t'; break;
        case 'u': {
            if (cur->len - cur->pos < 4u) return 0;
            uint32_t cp = 0;
            for (size_t k = 0; k < 4u; k++) {
                uint32_t d = 0;
                if (!rdna4j_hex_digit(cur->s[cur->pos + k], &d)) return 0;
                cp = (cp << 4) | d;
            }
            cur->pos += 4u;
            if (cp == 0 || (cp >= 0xD800u && cp <= 0xDFFFu)) return 0; // disallow NUL & unpaired surrogates
            uint8_t utf8[3];
            size_t ulen = 0;
            if (cp <= 0x7Fu) {
                utf8[0] = (uint8_t)cp; ulen = 1u;
            } else if (cp <= 0x7FFu) {
                utf8[0] = (uint8_t)(0xC0u | (cp >> 6));
                utf8[1] = (uint8_t)(0x80u | (cp & 0x3Fu));
                ulen = 2u;
            } else {
                utf8[0] = (uint8_t)(0xE0u | (cp >> 12));
                utf8[1] = (uint8_t)(0x80u | ((cp >> 6) & 0x3Fu));
                utf8[2] = (uint8_t)(0x80u | (cp & 0x3Fu));
                ulen = 3u;
            }
            if (out) {
                if (n + ulen >= cap) return 0;
                memcpy(out + n, utf8, ulen);
            }
            n += ulen;
            continue;
        }
        default:
            return 0;
        }
        if (out) {
            if (n + 1u >= cap) return 0;
            out[n] = mapped;
        }
        n++;
    }
    return 0;
}

static inline int rdna4j_parse_u32(rdna4j_cur_t *cur, uint32_t *out) {
    rdna4j_ws(cur);
    if (cur->pos >= cur->len) return 0;
    char first = cur->s[cur->pos];
    if (first < '0' || first > '9') return 0;
    uint64_t val = 0;
    if (first == '0') {
        cur->pos++;
        if (cur->pos < cur->len && cur->s[cur->pos] >= '0' && cur->s[cur->pos] <= '9') return 0;
    } else {
        while (cur->pos < cur->len && cur->s[cur->pos] >= '0' && cur->s[cur->pos] <= '9') {
            uint64_t d = (uint64_t)(cur->s[cur->pos++] - '0');
            val = val * 10u + d;
            if (val > UINT32_MAX) return 0;
        }
    }
    if (cur->pos < cur->len &&
        (cur->s[cur->pos] == '.' || cur->s[cur->pos] == 'e' || cur->s[cur->pos] == 'E'))
        return 0;
    *out = (uint32_t)val;
    return 1;
}

static inline int rdna4j_match_lit(rdna4j_cur_t *cur, const char *lit) {
    rdna4j_ws(cur);
    size_t n = strlen(lit);
    if (cur->len - cur->pos < n || memcmp(cur->s + cur->pos, lit, n) != 0) return 0;
    cur->pos += n;
    return 1;
}

static inline int rdna4j_skip_value(rdna4j_cur_t *cur, unsigned depth) {
    if (depth > RDNA4VFT_JSON_MAX_DEPTH) return 0;
    int c = rdna4j_peek(cur);
    if (c < 0) return 0;
    if (c == '"') return rdna4j_parse_string(cur, NULL, 0, NULL);
    if (c == '{') {
        cur->pos++;
        if (rdna4j_consume(cur, '}')) return 1;
        for (;;) {
            if (!rdna4j_parse_string(cur, NULL, 0, NULL)) return 0;
            if (!rdna4j_consume(cur, ':')) return 0;
            if (!rdna4j_skip_value(cur, depth + 1u)) return 0;
            if (rdna4j_consume(cur, '}')) return 1;
            if (!rdna4j_consume(cur, ',')) return 0;
        }
    }
    if (c == '[') {
        cur->pos++;
        if (rdna4j_consume(cur, ']')) return 1;
        for (;;) {
            if (!rdna4j_skip_value(cur, depth + 1u)) return 0;
            if (rdna4j_consume(cur, ']')) return 1;
            if (!rdna4j_consume(cur, ',')) return 0;
        }
    }
    if (c == 't') return rdna4j_match_lit(cur, "true");
    if (c == 'f') return rdna4j_match_lit(cur, "false");
    if (c == 'n') return rdna4j_match_lit(cur, "null");
    if (c == '-' || (c >= '0' && c <= '9')) {
        if (cur->s[cur->pos] == '-') cur->pos++;
        if (cur->pos >= cur->len || cur->s[cur->pos] < '0' || cur->s[cur->pos] > '9') return 0;
        if (cur->s[cur->pos] == '0') {
            cur->pos++;
            if (cur->pos < cur->len && cur->s[cur->pos] >= '0' && cur->s[cur->pos] <= '9') return 0;
        } else {
            while (cur->pos < cur->len && cur->s[cur->pos] >= '0' && cur->s[cur->pos] <= '9') cur->pos++;
        }
        if (cur->pos < cur->len && cur->s[cur->pos] == '.') {
            cur->pos++;
            if (cur->pos >= cur->len || cur->s[cur->pos] < '0' || cur->s[cur->pos] > '9') return 0;
            while (cur->pos < cur->len && cur->s[cur->pos] >= '0' && cur->s[cur->pos] <= '9') cur->pos++;
        }
        if (cur->pos < cur->len && (cur->s[cur->pos] == 'e' || cur->s[cur->pos] == 'E')) {
            cur->pos++;
            if (cur->pos < cur->len && (cur->s[cur->pos] == '+' || cur->s[cur->pos] == '-')) cur->pos++;
            if (cur->pos >= cur->len || cur->s[cur->pos] < '0' || cur->s[cur->pos] > '9') return 0;
            while (cur->pos < cur->len && cur->s[cur->pos] >= '0' && cur->s[cur->pos] <= '9') cur->pos++;
        }
        return 1;
    }
    return 0;
}

// Parse garnetx_list_exported_symbols JSON array (`["sym1","sym2"]`) into `out`.
static inline rdna4dl_status_t rdna4dl_parse_symbols_json(const char *json, size_t len,
                                                          rdna4dl_container_t *out) {
    if (!json || len == 0 || !out) return RDNA4DL_ERR_INVALID;
    rdna4j_cur_t cur = { json, len, 0 };
    if (!rdna4j_consume(&cur, '[')) return RDNA4DL_ERR_JSON;
    if (!rdna4j_consume(&cur, ']')) {
        char symbuf[RDNA4DL_MAX_SYMBOL_LEN + 1u];
        for (;;) {
            size_t slen = 0;
            if (!rdna4j_parse_string(&cur, symbuf, sizeof symbuf, &slen))
                return RDNA4DL_ERR_JSON;
            if (slen == 0) return RDNA4DL_ERR_INVALID;
            rdna4dl_status_t st = rdna4dl_add_symbol_len(out, symbuf, slen, 1);
            if (st != RDNA4DL_OK) return st;
            if (rdna4j_consume(&cur, ']')) break;
            if (!rdna4j_consume(&cur, ',')) return RDNA4DL_ERR_JSON;
        }
    }
    rdna4j_ws(&cur);
    return cur.pos == cur.len ? RDNA4DL_OK : RDNA4DL_ERR_JSON;
}

static inline rdna4vft_status_t rdna4vft_refl_add_handle(rdna4vft_reflection_t *r,
                                                         uint32_t handle_id,
                                                         const char *sym, size_t sym_len) {
    if (handle_id == RDNA4VFT_HANDLE_NULL || handle_id > RDNA4VFT_MAX_HANDLES ||
        !sym || sym_len == 0 || sym_len > RDNA4DL_MAX_SYMBOL_LEN)
        return RDNA4VFT_ERR_INVALID_HANDLE;
    for (uint32_t i = 0; i < r->handle_count; i++) {
        if (r->handles[i].handle_id == handle_id ||
            (strlen(r->handles[i].symbol) == sym_len &&
             memcmp(r->handles[i].symbol, sym, sym_len) == 0))
            return RDNA4VFT_ERR_DUPLICATE;
    }
    if (r->handle_count >= RDNA4VFT_MAX_HANDLES) return RDNA4VFT_ERR_BOUNDS;
    if (r->handle_count == r->handle_cap) {
        uint32_t ncap = r->handle_cap ? (r->handle_cap * 2u) : 8u;
        if (ncap > RDNA4VFT_MAX_HANDLES) ncap = RDNA4VFT_MAX_HANDLES;
        rdna4vft_handle_entry_t *nh =
            (rdna4vft_handle_entry_t *)realloc(r->handles, (size_t)ncap * sizeof(*nh));
        if (!nh) return RDNA4VFT_ERR_OOM;
        r->handles = nh;
        r->handle_cap = ncap;
    }
    char *copy = (char *)malloc(sym_len + 1u);
    if (!copy) return RDNA4VFT_ERR_OOM;
    memcpy(copy, sym, sym_len);
    copy[sym_len] = '\0';
    r->handles[r->handle_count].handle_id = handle_id;
    r->handles[r->handle_count].symbol = copy;
    r->handle_count++;
    if (handle_id > r->max_handle_id) r->max_handle_id = handle_id;
    return RDNA4VFT_OK;
}

static inline rdna4vft_status_t rdna4vft_parse_table_entry(rdna4j_cur_t *cur,
                                                           rdna4vft_table_binding_t *out) {
    memset(out, 0, sizeof *out);
    if (!rdna4j_consume(cur, '{')) return RDNA4VFT_ERR_JSON;
    int seen_param = 0, seen_metal = 0;
    if (!rdna4j_consume(cur, '}')) {
        char key[64];
        for (;;) {
            size_t klen = 0;
            if (!rdna4j_parse_string(cur, key, sizeof key, &klen) || !rdna4j_consume(cur, ':'))
                return RDNA4VFT_ERR_JSON;
            if (strcmp(key, "param_index") == 0) {
                if (!rdna4j_parse_u32(cur, &out->param_index)) return RDNA4VFT_ERR_JSON;
                seen_param = 1;
            } else if (strcmp(key, "metal_index") == 0) {
                if (!rdna4j_parse_u32(cur, &out->metal_index)) return RDNA4VFT_ERR_JSON;
                seen_metal = 1;
            } else if (strcmp(key, "descriptor") == 0) {
                if (rdna4j_peek(cur) == 'n') {
                    if (!rdna4j_match_lit(cur, "null")) return RDNA4VFT_ERR_JSON;
                    out->has_descriptor = 0;
                } else if (rdna4j_consume(cur, '{')) {
                    int has_set = 0, has_bind = 0, has_cnt = 0;
                    if (!rdna4j_consume(cur, '}')) {
                        char dkey[64];
                        for (;;) {
                            size_t dklen = 0;
                            if (!rdna4j_parse_string(cur, dkey, sizeof dkey, &dklen) ||
                                !rdna4j_consume(cur, ':'))
                                return RDNA4VFT_ERR_JSON;
                            if (strcmp(dkey, "set") == 0) {
                                if (!rdna4j_parse_u32(cur, &out->desc_set)) return RDNA4VFT_ERR_JSON;
                                has_set = 1;
                            } else if (strcmp(dkey, "binding") == 0) {
                                if (!rdna4j_parse_u32(cur, &out->desc_binding)) return RDNA4VFT_ERR_JSON;
                                has_bind = 1;
                            } else if (strcmp(dkey, "count") == 0) {
                                if (!rdna4j_parse_u32(cur, &out->desc_count)) return RDNA4VFT_ERR_JSON;
                                has_cnt = 1;
                            } else {
                                if (!rdna4j_skip_value(cur, 3u)) return RDNA4VFT_ERR_JSON;
                            }
                            if (rdna4j_consume(cur, '}')) break;
                            if (!rdna4j_consume(cur, ',')) return RDNA4VFT_ERR_JSON;
                        }
                    }
                    if (!has_set || !has_bind || !has_cnt) return RDNA4VFT_ERR_JSON;
                    out->has_descriptor = 1;
                } else {
                    return RDNA4VFT_ERR_JSON;
                }
            } else {
                if (!rdna4j_skip_value(cur, 2u)) return RDNA4VFT_ERR_JSON;
            }
            if (rdna4j_consume(cur, '}')) break;
            if (!rdna4j_consume(cur, ',')) return RDNA4VFT_ERR_JSON;
        }
    }
    if (!seen_param || !seen_metal) return RDNA4VFT_ERR_JSON;
    if (out->metal_index >= RDNA4VFT_MAX_TABLES) return RDNA4VFT_ERR_BOUNDS;
    return RDNA4VFT_OK;
}

static inline rdna4vft_status_t rdna4vft_parse_vft_object(rdna4j_cur_t *cur,
                                                          rdna4vft_reflection_t *out) {
    if (!rdna4j_consume(cur, '{')) return RDNA4VFT_ERR_JSON;
    int seen_abi = 0, seen_hdr = 0, seen_tables = 0, seen_handles = 0;
    if (!rdna4j_consume(cur, '}')) {
        char key[64];
        for (;;) {
            size_t klen = 0;
            if (!rdna4j_parse_string(cur, key, sizeof key, &klen) || !rdna4j_consume(cur, ':'))
                return RDNA4VFT_ERR_JSON;
            if (strcmp(key, "abi_version") == 0) {
                if (!rdna4j_parse_u32(cur, &out->abi_version)) return RDNA4VFT_ERR_JSON;
                seen_abi = 1;
            } else if (strcmp(key, "header_words") == 0) {
                if (!rdna4j_parse_u32(cur, &out->header_words)) return RDNA4VFT_ERR_JSON;
                seen_hdr = 1;
            } else if (strcmp(key, "tables") == 0) {
                if (!rdna4j_consume(cur, '[')) return RDNA4VFT_ERR_JSON;
                if (!rdna4j_consume(cur, ']')) {
                    for (;;) {
                        if (out->table_count >= RDNA4VFT_MAX_TABLES) return RDNA4VFT_ERR_BOUNDS;
                        rdna4vft_status_t st =
                            rdna4vft_parse_table_entry(cur, &out->tables[out->table_count]);
                        if (st != RDNA4VFT_OK) return st;
                        out->table_count++;
                        if (rdna4j_consume(cur, ']')) break;
                        if (!rdna4j_consume(cur, ',')) return RDNA4VFT_ERR_JSON;
                    }
                }
                seen_tables = 1;
            } else if (strcmp(key, "handles") == 0) {
                if (!rdna4j_consume(cur, '[')) return RDNA4VFT_ERR_JSON;
                if (!rdna4j_consume(cur, ']')) {
                    char symbuf[RDNA4DL_MAX_SYMBOL_LEN + 1u];
                    for (;;) {
                        if (!rdna4j_consume(cur, '{')) return RDNA4VFT_ERR_JSON;
                        uint32_t hid = 0;
                        size_t slen = 0;
                        int got_hid = 0, got_sym = 0;
                        if (!rdna4j_consume(cur, '}')) {
                            char hkey[64];
                            for (;;) {
                                size_t hklen = 0;
                                if (!rdna4j_parse_string(cur, hkey, sizeof hkey, &hklen) ||
                                    !rdna4j_consume(cur, ':'))
                                    return RDNA4VFT_ERR_JSON;
                                if (strcmp(hkey, "handle_id") == 0) {
                                    if (!rdna4j_parse_u32(cur, &hid)) return RDNA4VFT_ERR_JSON;
                                    got_hid = 1;
                                } else if (strcmp(hkey, "symbol") == 0) {
                                    if (!rdna4j_parse_string(cur, symbuf, sizeof symbuf, &slen))
                                        return RDNA4VFT_ERR_JSON;
                                    got_sym = 1;
                                } else {
                                    if (!rdna4j_skip_value(cur, 3u)) return RDNA4VFT_ERR_JSON;
                                }
                                if (rdna4j_consume(cur, '}')) break;
                                if (!rdna4j_consume(cur, ',')) return RDNA4VFT_ERR_JSON;
                            }
                        }
                        if (!got_hid || !got_sym) return RDNA4VFT_ERR_JSON;
                        rdna4vft_status_t st = rdna4vft_refl_add_handle(out, hid, symbuf, slen);
                        if (st != RDNA4VFT_OK) return st;
                        if (rdna4j_consume(cur, ']')) break;
                        if (!rdna4j_consume(cur, ',')) return RDNA4VFT_ERR_JSON;
                    }
                }
                seen_handles = 1;
            } else {
                if (!rdna4j_skip_value(cur, 2u)) return RDNA4VFT_ERR_JSON;
            }
            if (rdna4j_consume(cur, '}')) break;
            if (!rdna4j_consume(cur, ',')) return RDNA4VFT_ERR_JSON;
        }
    }
    if (!seen_abi || !seen_hdr || !seen_tables || !seen_handles) return RDNA4VFT_ERR_JSON;
    if (out->abi_version != RDNA4VFT_ABI_VERSION || out->header_words != RDNA4VFT_HEADER_WORDS)
        return RDNA4VFT_ERR_ABI;
    out->present = 1;
    return RDNA4VFT_OK;
}

// Parse top-level garnetxlate reflection JSON (extracting optional `"visible_function_tables"`).
// When `"visible_function_tables"` is absent or null, returns RDNA4VFT_OK with `out->present == 0`.
static inline rdna4vft_status_t rdna4vft_parse_reflection_json(const char *json, size_t len,
                                                               rdna4vft_reflection_t *out) {
    if (!json || len == 0 || !out) return RDNA4VFT_ERR_INVALID;
    rdna4vft_reflection_t tmp;
    rdna4vft_refl_init(&tmp);

    rdna4j_cur_t cur = { json, len, 0 };
    if (!rdna4j_consume(&cur, '{')) return RDNA4VFT_ERR_JSON;
    if (!rdna4j_consume(&cur, '}')) {
        char key[128];
        for (;;) {
            size_t klen = 0;
            if (!rdna4j_parse_string(&cur, key, sizeof key, &klen) || !rdna4j_consume(&cur, ':')) {
                rdna4vft_refl_destroy(&tmp);
                return RDNA4VFT_ERR_JSON;
            }
            if (strcmp(key, "visible_function_tables") == 0) {
                if (tmp.present) {
                    rdna4vft_refl_destroy(&tmp);
                    return RDNA4VFT_ERR_DUPLICATE;
                }
                if (rdna4j_peek(&cur) == 'n') {
                    if (!rdna4j_match_lit(&cur, "null")) {
                        rdna4vft_refl_destroy(&tmp);
                        return RDNA4VFT_ERR_JSON;
                    }
                } else {
                    rdna4vft_status_t st = rdna4vft_parse_vft_object(&cur, &tmp);
                    if (st != RDNA4VFT_OK) {
                        rdna4vft_refl_destroy(&tmp);
                        return st;
                    }
                }
            } else {
                if (!rdna4j_skip_value(&cur, 1u)) {
                    rdna4vft_refl_destroy(&tmp);
                    return RDNA4VFT_ERR_JSON;
                }
            }
            if (rdna4j_consume(&cur, '}')) break;
            if (!rdna4j_consume(&cur, ',')) {
                rdna4vft_refl_destroy(&tmp);
                return RDNA4VFT_ERR_JSON;
            }
        }
    }
    rdna4j_ws(&cur);
    if (cur.pos != cur.len) {
        rdna4vft_refl_destroy(&tmp);
        return RDNA4VFT_ERR_JSON;
    }
    *out = tmp;
    return RDNA4VFT_OK;
}

#endif // RDNA4_DYNLIB_VFT_H
