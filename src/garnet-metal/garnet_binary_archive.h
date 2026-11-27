// garnet_binary_archive.h: pure-C versioned Garnet binary archive ('GRBA') codec.
// No Metal, no Foundation, no Vulkan — shared by GarnetDevice.m and host C tests.
// Layout: [rdna4ba_header_t (128 B)][entry table (N * 88 B)][archive VkPipelineCache][entry payloads...]
// Integrity: whole-archive CRC32 (header[0..124) + bytes[128..total)) + archive cache CRC32 + per-entry CRC32.
#ifndef RDNA4_BINARY_ARCHIVE_H
#define RDNA4_BINARY_ARCHIVE_H

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

#define RDNA4BA_MAGIC             "GRBA"
#define RDNA4BA_FORMAT_VERSION    UINT32_C(1)
#define RDNA4BA_ABI_VERSION       UINT32_C(1)
#define RDNA4BA_SPV_MAGIC         UINT32_C(0x07230203)
#define RDNA4BA_MAX_ENTRIES       UINT32_C(4096)
#define RDNA4BA_MAX_ENTRY_PAYLOAD (UINT64_C(80) << 20)
#define RDNA4BA_MAX_ARCHIVE_BYTES (UINT64_C(256) << 20)

enum {
    RDNA4BA_KIND_COMPUTE  = 1u,
    RDNA4BA_KIND_RENDER   = 2u,
    RDNA4BA_KIND_FUNCTION = 3u,
};

#define RDNA4BA_ENTRY_F_SECONDARY (UINT32_C(1) << 0)
#define RDNA4BA_ENTRY_F_VK_CACHE  (UINT32_C(1) << 1)
#define RDNA4BA_ENTRY_F_MASK      (RDNA4BA_ENTRY_F_SECONDARY | RDNA4BA_ENTRY_F_VK_CACHE)

typedef enum {
    RDNA4BA_OK = 0,
    RDNA4BA_ERR_INVALID,
    RDNA4BA_ERR_TRUNCATED,
    RDNA4BA_ERR_MAGIC,
    RDNA4BA_ERR_VERSION,
    RDNA4BA_ERR_HEADER,
    RDNA4BA_ERR_ABI_MISMATCH,
    RDNA4BA_ERR_DEVICE_MISMATCH,
    RDNA4BA_ERR_ARCHIVE_CRC,
    RDNA4BA_ERR_ENTRY_CRC,
    RDNA4BA_ERR_BOUNDS,
    RDNA4BA_ERR_DUPLICATE,
    RDNA4BA_ERR_OOM,
    RDNA4BA_ERR_IO,
} rdna4ba_status_t;

static inline const char *rdna4ba_status_str(rdna4ba_status_t s) {
    switch (s) {
    case RDNA4BA_OK:                  return "ok";
    case RDNA4BA_ERR_INVALID:         return "invalid binary archive argument";
    case RDNA4BA_ERR_TRUNCATED:       return "truncated binary archive";
    case RDNA4BA_ERR_MAGIC:           return "bad magic (not a Garnet GRBA binary archive)";
    case RDNA4BA_ERR_VERSION:         return "unsupported binary archive format version";
    case RDNA4BA_ERR_HEADER:          return "malformed binary archive header";
    case RDNA4BA_ERR_ABI_MISMATCH:    return "binary archive ABI version mismatch";
    case RDNA4BA_ERR_DEVICE_MISMATCH: return "binary archive GPU/driver/translator fingerprint mismatch";
    case RDNA4BA_ERR_ARCHIVE_CRC:     return "binary archive whole-archive CRC32 mismatch (corrupt/tampered)";
    case RDNA4BA_ERR_ENTRY_CRC:       return "binary archive entry CRC32 mismatch (corrupt/tampered)";
    case RDNA4BA_ERR_BOUNDS:          return "binary archive size or offset out of bounds";
    case RDNA4BA_ERR_DUPLICATE:       return "duplicate pipeline key in binary archive";
    case RDNA4BA_ERR_OOM:             return "out of memory";
    case RDNA4BA_ERR_IO:              return "binary archive file I/O failed";
    }
    return "unknown binary archive error";
}

// ---- Little-endian wire helpers ----
static inline uint32_t rdna4ba_get_u32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}
static inline uint64_t rdna4ba_get_u64(const uint8_t *p) {
    return (uint64_t)rdna4ba_get_u32(p) | ((uint64_t)rdna4ba_get_u32(p + 4) << 32);
}
static inline void rdna4ba_put_u32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)(v & 0xFFu); p[1] = (uint8_t)((v >> 8) & 0xFFu);
    p[2] = (uint8_t)((v >> 16) & 0xFFu); p[3] = (uint8_t)((v >> 24) & 0xFFu);
}
static inline void rdna4ba_put_u64(uint8_t *p, uint64_t v) {
    rdna4ba_put_u32(p, (uint32_t)(v & 0xFFFFFFFFu));
    rdna4ba_put_u32(p + 4, (uint32_t)(v >> 32));
}

// ---- Pure-C FIPS 180-4 SHA-256 (canonical descriptor/pipeline keys) ----
typedef struct {
    uint32_t state[8];
    uint64_t bitlen;
    uint8_t buf[64];
    size_t used;
} rdna4ba_sha256_ctx;

static inline uint32_t rdna4ba_rotr32(uint32_t x, unsigned n) {
    return (x >> n) | (x << (32u - n));
}

static inline void rdna4ba_sha256_block(rdna4ba_sha256_ctx *c, const uint8_t blk[64]) {
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
    for (int i = 0; i < 16; i++) {
        w[i] = ((uint32_t)blk[i * 4] << 24) | ((uint32_t)blk[i * 4 + 1] << 16) |
               ((uint32_t)blk[i * 4 + 2] << 8) | (uint32_t)blk[i * 4 + 3];
    }
    for (int i = 16; i < 64; i++) {
        uint32_t s0 = rdna4ba_rotr32(w[i - 15], 7) ^ rdna4ba_rotr32(w[i - 15], 18) ^ (w[i - 15] >> 3);
        uint32_t s1 = rdna4ba_rotr32(w[i - 2], 17) ^ rdna4ba_rotr32(w[i - 2], 19) ^ (w[i - 2] >> 10);
        w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }
    uint32_t a = c->state[0], b = c->state[1], d = c->state[2], e = c->state[3];
    uint32_t f = c->state[4], g = c->state[5], h = c->state[6], j = c->state[7];
    for (int i = 0; i < 64; i++) {
        uint32_t S1 = rdna4ba_rotr32(f, 6) ^ rdna4ba_rotr32(f, 11) ^ rdna4ba_rotr32(f, 25);
        uint32_t ch = (f & g) ^ ((~f) & h);
        uint32_t t1 = j + S1 + ch + k[i] + w[i];
        uint32_t S0 = rdna4ba_rotr32(a, 2) ^ rdna4ba_rotr32(a, 13) ^ rdna4ba_rotr32(a, 22);
        uint32_t maj = (a & b) ^ (a & d) ^ (b & d);
        uint32_t t2 = S0 + maj;
        j = h; h = g; g = f; f = e + t1; e = d; d = b; b = a; a = t1 + t2;
    }
    c->state[0] += a; c->state[1] += b; c->state[2] += d; c->state[3] += e;
    c->state[4] += f; c->state[5] += g; c->state[6] += h; c->state[7] += j;
}

static inline void rdna4ba_sha256_init(rdna4ba_sha256_ctx *c) {
    c->state[0] = 0x6a09e667u; c->state[1] = 0xbb67ae85u; c->state[2] = 0x3c6ef372u; c->state[3] = 0xa54ff53au;
    c->state[4] = 0x510e527fu; c->state[5] = 0x9b05688cu; c->state[6] = 0x1f83d9abu; c->state[7] = 0x5be0cd19u;
    c->bitlen = 0;
    c->used = 0;
}

static inline void rdna4ba_sha256_update(rdna4ba_sha256_ctx *c, const void *data, size_t len) {
    const uint8_t *p = (const uint8_t *)data;
    if (!len) return;
    c->bitlen += (uint64_t)len * 8u;
    if (c->used) {
        size_t take = 64u - c->used;
        if (take > len) take = len;
        memcpy(c->buf + c->used, p, take);
        c->used += take; p += take; len -= take;
        if (c->used == 64u) { rdna4ba_sha256_block(c, c->buf); c->used = 0; }
    }
    while (len >= 64u) { rdna4ba_sha256_block(c, p); p += 64u; len -= 64u; }
    if (len) { memcpy(c->buf, p, len); c->used = len; }
}

static inline void rdna4ba_sha256_update_u32(rdna4ba_sha256_ctx *c, uint32_t v) {
    uint8_t b[4]; rdna4ba_put_u32(b, v); rdna4ba_sha256_update(c, b, 4);
}
static inline void rdna4ba_sha256_update_u64(rdna4ba_sha256_ctx *c, uint64_t v) {
    uint8_t b[8]; rdna4ba_put_u64(b, v); rdna4ba_sha256_update(c, b, 8);
}

static inline void rdna4ba_sha256_final(rdna4ba_sha256_ctx *c, uint8_t out[32]) {
    size_t i = c->used;
    c->buf[i++] = 0x80u;
    if (i > 56u) {
        memset(c->buf + i, 0, 64u - i);
        rdna4ba_sha256_block(c, c->buf);
        i = 0;
    }
    memset(c->buf + i, 0, 56u - i);
    for (int k = 0; k < 8; k++) c->buf[56 + k] = (uint8_t)(c->bitlen >> (56 - k * 8));
    rdna4ba_sha256_block(c, c->buf);
    for (int k = 0; k < 8; k++) {
        out[k * 4 + 0] = (uint8_t)(c->state[k] >> 24);
        out[k * 4 + 1] = (uint8_t)(c->state[k] >> 16);
        out[k * 4 + 2] = (uint8_t)(c->state[k] >> 8);
        out[k * 4 + 3] = (uint8_t)(c->state[k]);
    }
}

static inline void rdna4ba_sha256(const void *data, size_t len, uint8_t out[32]) {
    rdna4ba_sha256_ctx c;
    rdna4ba_sha256_init(&c);
    rdna4ba_sha256_update(&c, data, len);
    rdna4ba_sha256_final(&c, out);
}

static inline uint64_t rdna4ba_key_u64_from_sha(const uint8_t sha[32]) {
    return rdna4ba_get_u64(sha);
}

static inline int rdna4ba_spv_header_ok(const void *p, size_t n) {
    return p != NULL && n >= 20u && (n & 3u) == 0 && rdna4ba_get_u32((const uint8_t *)p) == RDNA4BA_SPV_MAGIC;
}

// ---- Wire structures (fixed little-endian sizes, zero implicit padding) ----
typedef struct {
    uint32_t garnet_abi_version;
    uint32_t translator_abi_version;
    uint32_t vendor_id;
    uint32_t device_id;
    uint32_t driver_version;
    uint32_t flags;
    uint8_t pipeline_cache_uuid[16];
    uint8_t translator_hash[32];
} rdna4ba_fingerprint_t;
_Static_assert(sizeof(rdna4ba_fingerprint_t) == 72, "rdna4ba_fingerprint_t wire size");

typedef struct {
    char magic[4];
    uint32_t format_version;
    uint32_t header_size;
    uint32_t flags;
    rdna4ba_fingerprint_t fingerprint;
    uint32_t entry_count;
    uint32_t entry_size;
    uint64_t total_size;
    uint64_t vk_cache_offset;
    uint64_t vk_cache_size;
    uint32_t vk_cache_crc32;
    uint32_t archive_crc32;
} rdna4ba_header_t;
_Static_assert(sizeof(rdna4ba_header_t) == 128, "rdna4ba_header_t wire size");
_Static_assert(offsetof(rdna4ba_header_t, archive_crc32) == 124, "archive_crc32 must trail header");

typedef struct {
    uint32_t kind;
    uint32_t flags;
    uint64_t key_u64;
    uint8_t key_sha[32];
    uint64_t payload_offset;
    uint64_t payload_size;
    uint32_t spv0_size;
    uint32_t meta0_size;
    uint32_t spv1_size;
    uint32_t meta1_size;
    uint32_t vk_cache_size;
    uint32_t payload_crc32;
} rdna4ba_entry_disk_t;
_Static_assert(sizeof(rdna4ba_entry_disk_t) == 88, "rdna4ba_entry_disk_t wire size");

// ---- In-memory archive structures ----
typedef struct {
    uint32_t kind;
    uint32_t flags;
    uint64_t key_u64;
    uint8_t key_sha[32];
    const void *spv0;     uint32_t spv0_size;
    const void *meta0;    uint32_t meta0_size;
    const void *spv1;     uint32_t spv1_size;
    const void *meta1;    uint32_t meta1_size;
    const void *vk_cache; uint32_t vk_cache_size;
} rdna4ba_entry_view_t;

typedef struct {
    uint32_t kind;
    uint32_t flags;
    uint64_t key_u64;
    uint8_t key_sha[32];
    uint32_t spv0_size;
    uint32_t meta0_size;
    uint32_t spv1_size;
    uint32_t meta1_size;
    uint32_t vk_cache_size;
    uint32_t payload_crc32;
    uint64_t payload_size;
    uint8_t *payload;
} rdna4ba_entry_t;

typedef struct {
    rdna4ba_fingerprint_t fingerprint;
    rdna4ba_entry_t *entries;
    uint32_t count;
    uint32_t cap;
    uint8_t *vk_cache;
    size_t vk_cache_size;
    uint32_t vk_cache_crc32;
} rdna4ba_archive_t;

static inline int rdna4ba_kind_ok(uint32_t kind) {
    return kind == RDNA4BA_KIND_COMPUTE || kind == RDNA4BA_KIND_RENDER || kind == RDNA4BA_KIND_FUNCTION;
}

static inline void rdna4ba_init(rdna4ba_archive_t *a, const rdna4ba_fingerprint_t *fp) {
    memset(a, 0, sizeof *a);
    if (fp) a->fingerprint = *fp;
    (void)rdna4crc32(0, "", 0); // prime CRC lookup table before concurrent archive operations
}

static inline void rdna4ba_destroy(rdna4ba_archive_t *a) {
    if (!a) return;
    for (uint32_t i = 0; i < a->count; i++) free(a->entries[i].payload);
    free(a->entries);
    free(a->vk_cache);
    memset(a, 0, sizeof *a);
}

static inline rdna4ba_status_t rdna4ba_fingerprint_check(const rdna4ba_fingerprint_t *got,
                                                         const rdna4ba_fingerprint_t *want) {
    if (!got || !want) return RDNA4BA_OK;
    if (got->garnet_abi_version != want->garnet_abi_version ||
        got->translator_abi_version != want->translator_abi_version)
        return RDNA4BA_ERR_ABI_MISMATCH;
    if (got->vendor_id != want->vendor_id ||
        got->device_id != want->device_id ||
        got->driver_version != want->driver_version ||
        got->flags != want->flags ||
        memcmp(got->pipeline_cache_uuid, want->pipeline_cache_uuid, sizeof got->pipeline_cache_uuid) != 0 ||
        memcmp(got->translator_hash, want->translator_hash, sizeof got->translator_hash) != 0)
        return RDNA4BA_ERR_DEVICE_MISMATCH;
    return RDNA4BA_OK;
}

static inline uint64_t rdna4ba_current_bytes(const rdna4ba_archive_t *a) {
    uint64_t total = sizeof(rdna4ba_header_t) +
                     (uint64_t)a->count * sizeof(rdna4ba_entry_disk_t) +
                     (uint64_t)a->vk_cache_size;
    for (uint32_t i = 0; i < a->count; i++) total += a->entries[i].payload_size;
    return total;
}

static inline rdna4ba_status_t rdna4ba_set_vk_cache(rdna4ba_archive_t *a, const void *data, size_t size) {
    if (!a || (size > 0 && !data)) return RDNA4BA_ERR_INVALID;
    if ((uint64_t)size > RDNA4BA_MAX_ENTRY_PAYLOAD) return RDNA4BA_ERR_BOUNDS;
    uint64_t base = rdna4ba_current_bytes(a) - (uint64_t)a->vk_cache_size;
    if ((uint64_t)size > RDNA4BA_MAX_ARCHIVE_BYTES - base) return RDNA4BA_ERR_BOUNDS;
    uint8_t *copy = NULL;
    if (size > 0) {
        copy = (uint8_t *)malloc(size);
        if (!copy) return RDNA4BA_ERR_OOM;
        memcpy(copy, data, size);
    }
    free(a->vk_cache);
    a->vk_cache = copy;
    a->vk_cache_size = size;
    a->vk_cache_crc32 = size ? rdna4crc32(0, copy, size) : 0u;
    return RDNA4BA_OK;
}

static inline int rdna4ba_find_index(const rdna4ba_archive_t *a, uint32_t kind,
                                     uint64_t key_u64, const uint8_t key_sha[32]) {
    if (!a || !key_sha) return -1;
    for (uint32_t i = 0; i < a->count; i++) {
        const rdna4ba_entry_t *e = &a->entries[i];
        if (e->kind == kind && e->key_u64 == key_u64 && memcmp(e->key_sha, key_sha, 32) == 0)
            return (int)i;
    }
    return -1;
}

static inline const rdna4ba_entry_t *rdna4ba_find(const rdna4ba_archive_t *a, uint32_t kind,
                                                  uint64_t key_u64, const uint8_t key_sha[32]) {
    int idx = rdna4ba_find_index(a, kind, key_u64, key_sha);
    return idx >= 0 ? &a->entries[idx] : NULL;
}

static inline const rdna4ba_entry_t *rdna4ba_find_by_sha(const rdna4ba_archive_t *a, uint32_t kind,
                                                         const uint8_t key_sha[32]) {
    if (!key_sha) return NULL;
    return rdna4ba_find(a, kind, rdna4ba_key_u64_from_sha(key_sha), key_sha);
}

static inline rdna4ba_entry_view_t rdna4ba_entry_view(const rdna4ba_entry_t *e) {
    rdna4ba_entry_view_t v;
    memset(&v, 0, sizeof v);
    if (!e) return v;
    v.kind = e->kind;
    v.flags = e->flags;
    v.key_u64 = e->key_u64;
    memcpy(v.key_sha, e->key_sha, 32);
    size_t off = 0;
    if (e->spv0_size)     { v.spv0     = e->payload + off; v.spv0_size     = e->spv0_size;     off += e->spv0_size; }
    if (e->meta0_size)    { v.meta0    = e->payload + off; v.meta0_size    = e->meta0_size;    off += e->meta0_size; }
    if (e->spv1_size)     { v.spv1     = e->payload + off; v.spv1_size     = e->spv1_size;     off += e->spv1_size; }
    if (e->meta1_size)    { v.meta1    = e->payload + off; v.meta1_size    = e->meta1_size;    off += e->meta1_size; }
    if (e->vk_cache_size) { v.vk_cache = e->payload + off; v.vk_cache_size = e->vk_cache_size; }
    return v;
}

static inline rdna4ba_status_t rdna4ba_put(rdna4ba_archive_t *a, const rdna4ba_entry_view_t *in) {
    if (!a || !in || !rdna4ba_kind_ok(in->kind)) return RDNA4BA_ERR_INVALID;
    if (in->flags & ~RDNA4BA_ENTRY_F_MASK) return RDNA4BA_ERR_INVALID;
    if ((in->spv0_size && !in->spv0) || (in->meta0_size && !in->meta0) ||
        (in->spv1_size && !in->spv1) || (in->meta1_size && !in->meta1) ||
        (in->vk_cache_size && !in->vk_cache))
        return RDNA4BA_ERR_INVALID;
    if (in->kind != RDNA4BA_KIND_RENDER && (in->spv1_size || in->meta1_size))
        return RDNA4BA_ERR_INVALID;

    uint64_t psize = (uint64_t)in->spv0_size + (uint64_t)in->meta0_size +
                     (uint64_t)in->spv1_size + (uint64_t)in->meta1_size +
                     (uint64_t)in->vk_cache_size;
    if (psize > RDNA4BA_MAX_ENTRY_PAYLOAD) return RDNA4BA_ERR_BOUNDS;

    int existing = rdna4ba_find_index(a, in->kind, in->key_u64, in->key_sha);
    uint64_t cur = rdna4ba_current_bytes(a);
    if (existing >= 0) {
        uint64_t base = cur - a->entries[existing].payload_size;
        if (psize > RDNA4BA_MAX_ARCHIVE_BYTES - base) return RDNA4BA_ERR_BOUNDS;
    } else {
        if (a->count >= RDNA4BA_MAX_ENTRIES) return RDNA4BA_ERR_BOUNDS;
        uint64_t need = sizeof(rdna4ba_entry_disk_t) + psize;
        if (need > RDNA4BA_MAX_ARCHIVE_BYTES - cur) return RDNA4BA_ERR_BOUNDS;
    }

    uint8_t *buf = NULL;
    if (psize > 0) {
        buf = (uint8_t *)malloc((size_t)psize);
        if (!buf) return RDNA4BA_ERR_OOM;
        size_t off = 0;
        if (in->spv0_size)     { memcpy(buf + off, in->spv0,     in->spv0_size);     off += in->spv0_size; }
        if (in->meta0_size)    { memcpy(buf + off, in->meta0,    in->meta0_size);    off += in->meta0_size; }
        if (in->spv1_size)     { memcpy(buf + off, in->spv1,     in->spv1_size);     off += in->spv1_size; }
        if (in->meta1_size)    { memcpy(buf + off, in->meta1,    in->meta1_size);    off += in->meta1_size; }
        if (in->vk_cache_size) { memcpy(buf + off, in->vk_cache, in->vk_cache_size); }
    }

    rdna4ba_entry_t *dst = NULL;
    if (existing >= 0) {
        dst = &a->entries[existing];
        free(dst->payload);
    } else {
        if (a->count == a->cap) {
            uint32_t ncap = a->cap ? (a->cap * 2u) : 8u;
            if (ncap > RDNA4BA_MAX_ENTRIES) ncap = RDNA4BA_MAX_ENTRIES;
            rdna4ba_entry_t *ne = (rdna4ba_entry_t *)realloc(a->entries, (size_t)ncap * sizeof(*ne));
            if (!ne) { free(buf); return RDNA4BA_ERR_OOM; }
            a->entries = ne;
            a->cap = ncap;
        }
        dst = &a->entries[a->count++];
    }

    uint32_t flags = in->flags;
    if (in->spv1_size || in->meta1_size) flags |= RDNA4BA_ENTRY_F_SECONDARY;
    else flags &= ~RDNA4BA_ENTRY_F_SECONDARY;
    if (in->vk_cache_size) flags |= RDNA4BA_ENTRY_F_VK_CACHE;
    else flags &= ~RDNA4BA_ENTRY_F_VK_CACHE;

    dst->kind = in->kind;
    dst->flags = flags;
    dst->key_u64 = in->key_u64;
    memcpy(dst->key_sha, in->key_sha, 32);
    dst->spv0_size = in->spv0_size;
    dst->meta0_size = in->meta0_size;
    dst->spv1_size = in->spv1_size;
    dst->meta1_size = in->meta1_size;
    dst->vk_cache_size = in->vk_cache_size;
    dst->payload_size = psize;
    dst->payload_crc32 = psize ? rdna4crc32(0, buf, (size_t)psize) : 0u;
    dst->payload = buf;
    return RDNA4BA_OK;
}

static inline void rdna4ba_encode_header(uint8_t out[sizeof(rdna4ba_header_t)], const rdna4ba_header_t *h) {
    memcpy(out + 0, h->magic, 4);
    rdna4ba_put_u32(out + 4, h->format_version);
    rdna4ba_put_u32(out + 8, h->header_size);
    rdna4ba_put_u32(out + 12, h->flags);
    rdna4ba_put_u32(out + 16, h->fingerprint.garnet_abi_version);
    rdna4ba_put_u32(out + 20, h->fingerprint.translator_abi_version);
    rdna4ba_put_u32(out + 24, h->fingerprint.vendor_id);
    rdna4ba_put_u32(out + 28, h->fingerprint.device_id);
    rdna4ba_put_u32(out + 32, h->fingerprint.driver_version);
    rdna4ba_put_u32(out + 36, h->fingerprint.flags);
    memcpy(out + 40, h->fingerprint.pipeline_cache_uuid, 16);
    memcpy(out + 56, h->fingerprint.translator_hash, 32);
    rdna4ba_put_u32(out + 88, h->entry_count);
    rdna4ba_put_u32(out + 92, h->entry_size);
    rdna4ba_put_u64(out + 96, h->total_size);
    rdna4ba_put_u64(out + 104, h->vk_cache_offset);
    rdna4ba_put_u64(out + 112, h->vk_cache_size);
    rdna4ba_put_u32(out + 120, h->vk_cache_crc32);
    rdna4ba_put_u32(out + 124, h->archive_crc32);
}

static inline void rdna4ba_decode_header(const uint8_t in[sizeof(rdna4ba_header_t)], rdna4ba_header_t *h) {
    memcpy(h->magic, in + 0, 4);
    h->format_version = rdna4ba_get_u32(in + 4);
    h->header_size = rdna4ba_get_u32(in + 8);
    h->flags = rdna4ba_get_u32(in + 12);
    h->fingerprint.garnet_abi_version = rdna4ba_get_u32(in + 16);
    h->fingerprint.translator_abi_version = rdna4ba_get_u32(in + 20);
    h->fingerprint.vendor_id = rdna4ba_get_u32(in + 24);
    h->fingerprint.device_id = rdna4ba_get_u32(in + 28);
    h->fingerprint.driver_version = rdna4ba_get_u32(in + 32);
    h->fingerprint.flags = rdna4ba_get_u32(in + 36);
    memcpy(h->fingerprint.pipeline_cache_uuid, in + 40, 16);
    memcpy(h->fingerprint.translator_hash, in + 56, 32);
    h->entry_count = rdna4ba_get_u32(in + 88);
    h->entry_size = rdna4ba_get_u32(in + 92);
    h->total_size = rdna4ba_get_u64(in + 96);
    h->vk_cache_offset = rdna4ba_get_u64(in + 104);
    h->vk_cache_size = rdna4ba_get_u64(in + 112);
    h->vk_cache_crc32 = rdna4ba_get_u32(in + 120);
    h->archive_crc32 = rdna4ba_get_u32(in + 124);
}

static inline void rdna4ba_encode_entry_disk(uint8_t out[sizeof(rdna4ba_entry_disk_t)], const rdna4ba_entry_disk_t *d) {
    rdna4ba_put_u32(out + 0, d->kind);
    rdna4ba_put_u32(out + 4, d->flags);
    rdna4ba_put_u64(out + 8, d->key_u64);
    memcpy(out + 16, d->key_sha, 32);
    rdna4ba_put_u64(out + 48, d->payload_offset);
    rdna4ba_put_u64(out + 56, d->payload_size);
    rdna4ba_put_u32(out + 64, d->spv0_size);
    rdna4ba_put_u32(out + 68, d->meta0_size);
    rdna4ba_put_u32(out + 72, d->spv1_size);
    rdna4ba_put_u32(out + 76, d->meta1_size);
    rdna4ba_put_u32(out + 80, d->vk_cache_size);
    rdna4ba_put_u32(out + 84, d->payload_crc32);
}

static inline void rdna4ba_decode_entry_disk(const uint8_t in[sizeof(rdna4ba_entry_disk_t)], rdna4ba_entry_disk_t *d) {
    d->kind = rdna4ba_get_u32(in + 0);
    d->flags = rdna4ba_get_u32(in + 4);
    d->key_u64 = rdna4ba_get_u64(in + 8);
    memcpy(d->key_sha, in + 16, 32);
    d->payload_offset = rdna4ba_get_u64(in + 48);
    d->payload_size = rdna4ba_get_u64(in + 56);
    d->spv0_size = rdna4ba_get_u32(in + 64);
    d->meta0_size = rdna4ba_get_u32(in + 68);
    d->spv1_size = rdna4ba_get_u32(in + 72);
    d->meta1_size = rdna4ba_get_u32(in + 76);
    d->vk_cache_size = rdna4ba_get_u32(in + 80);
    d->payload_crc32 = rdna4ba_get_u32(in + 84);
}

static inline uint32_t rdna4ba_compute_archive_crc(const uint8_t *buf, size_t len) {
    uint32_t crc = rdna4crc32(0, buf, offsetof(rdna4ba_header_t, archive_crc32));
    if (len > sizeof(rdna4ba_header_t))
        crc = rdna4crc32(crc, buf + sizeof(rdna4ba_header_t), len - sizeof(rdna4ba_header_t));
    return crc;
}

static inline rdna4ba_status_t rdna4ba_serialize(const rdna4ba_archive_t *a, uint8_t **out_buf, size_t *out_len) {
    if (!a || !out_buf || !out_len) return RDNA4BA_ERR_INVALID;
    *out_buf = NULL; *out_len = 0;
    if (a->count > RDNA4BA_MAX_ENTRIES) return RDNA4BA_ERR_BOUNDS;
    uint64_t total = rdna4ba_current_bytes(a);
    if (total > RDNA4BA_MAX_ARCHIVE_BYTES) return RDNA4BA_ERR_BOUNDS;

    uint8_t *buf = (uint8_t *)malloc((size_t)total);
    if (!buf) return RDNA4BA_ERR_OOM;

    uint64_t table_end = sizeof(rdna4ba_header_t) + (uint64_t)a->count * sizeof(rdna4ba_entry_disk_t);
    uint64_t cursor = table_end;
    uint64_t vk_off = 0;
    if (a->vk_cache_size > 0) {
        vk_off = cursor;
        memcpy(buf + vk_off, a->vk_cache, a->vk_cache_size);
        cursor += (uint64_t)a->vk_cache_size;
    }

    for (uint32_t i = 0; i < a->count; i++) {
        const rdna4ba_entry_t *e = &a->entries[i];
        rdna4ba_entry_disk_t d;
        d.kind = e->kind;
        d.flags = e->flags;
        d.key_u64 = e->key_u64;
        memcpy(d.key_sha, e->key_sha, 32);
        d.payload_offset = cursor;
        d.payload_size = e->payload_size;
        d.spv0_size = e->spv0_size;
        d.meta0_size = e->meta0_size;
        d.spv1_size = e->spv1_size;
        d.meta1_size = e->meta1_size;
        d.vk_cache_size = e->vk_cache_size;
        d.payload_crc32 = e->payload_crc32;
        rdna4ba_encode_entry_disk(buf + sizeof(rdna4ba_header_t) + (size_t)i * sizeof(rdna4ba_entry_disk_t), &d);
        if (e->payload_size > 0) {
            memcpy(buf + cursor, e->payload, (size_t)e->payload_size);
            cursor += e->payload_size;
        }
    }

    rdna4ba_header_t h;
    memset(&h, 0, sizeof h);
    memcpy(h.magic, RDNA4BA_MAGIC, 4);
    h.format_version = RDNA4BA_FORMAT_VERSION;
    h.header_size = (uint32_t)sizeof(rdna4ba_header_t);
    h.flags = 0u;
    h.fingerprint = a->fingerprint;
    h.entry_count = a->count;
    h.entry_size = (uint32_t)sizeof(rdna4ba_entry_disk_t);
    h.total_size = total;
    h.vk_cache_offset = vk_off;
    h.vk_cache_size = (uint64_t)a->vk_cache_size;
    h.vk_cache_crc32 = a->vk_cache_size ? a->vk_cache_crc32 : 0u;
    h.archive_crc32 = 0u;
    rdna4ba_encode_header(buf, &h);
    h.archive_crc32 = rdna4ba_compute_archive_crc(buf, (size_t)total);
    rdna4ba_put_u32(buf + offsetof(rdna4ba_header_t, archive_crc32), h.archive_crc32);

    *out_buf = buf;
    *out_len = (size_t)total;
    return RDNA4BA_OK;
}

static inline rdna4ba_status_t rdna4ba_deserialize(const void *raw, size_t len,
                                                   const rdna4ba_fingerprint_t *expected_fp,
                                                   rdna4ba_archive_t *out) {
    if (!raw || !out) return RDNA4BA_ERR_INVALID;
    if (len < sizeof(rdna4ba_header_t)) return RDNA4BA_ERR_TRUNCATED;
    if ((uint64_t)len > RDNA4BA_MAX_ARCHIVE_BYTES) return RDNA4BA_ERR_BOUNDS;

    const uint8_t *buf = (const uint8_t *)raw;
    rdna4ba_header_t h;
    rdna4ba_decode_header(buf, &h);

    if (memcmp(h.magic, RDNA4BA_MAGIC, 4) != 0) return RDNA4BA_ERR_MAGIC;
    if (h.format_version != RDNA4BA_FORMAT_VERSION) return RDNA4BA_ERR_VERSION;
    if (h.header_size != sizeof(rdna4ba_header_t) ||
        h.entry_size != sizeof(rdna4ba_entry_disk_t) ||
        h.flags != 0u)
        return RDNA4BA_ERR_HEADER;
    if (h.total_size != (uint64_t)len)
        return (uint64_t)len < h.total_size ? RDNA4BA_ERR_TRUNCATED : RDNA4BA_ERR_BOUNDS;
    if (h.entry_count > RDNA4BA_MAX_ENTRIES) return RDNA4BA_ERR_BOUNDS;

    // Whole-archive CRC32 runs before fingerprint match so tampered headers report corruption.
    if (rdna4ba_compute_archive_crc(buf, len) != h.archive_crc32)
        return RDNA4BA_ERR_ARCHIVE_CRC;

    rdna4ba_status_t fp_st = rdna4ba_fingerprint_check(&h.fingerprint, expected_fp);
    if (fp_st != RDNA4BA_OK) return fp_st;

    uint64_t table_bytes = (uint64_t)h.entry_count * sizeof(rdna4ba_entry_disk_t);
    if (table_bytes > h.total_size - sizeof(rdna4ba_header_t)) return RDNA4BA_ERR_TRUNCATED;
    uint64_t cursor = sizeof(rdna4ba_header_t) + table_bytes;

    if (h.vk_cache_size == 0) {
        if (h.vk_cache_offset != 0 || h.vk_cache_crc32 != 0) return RDNA4BA_ERR_HEADER;
    } else {
        if (h.vk_cache_size > RDNA4BA_MAX_ENTRY_PAYLOAD) return RDNA4BA_ERR_BOUNDS;
        if (h.vk_cache_offset != cursor || h.vk_cache_size > h.total_size - cursor)
            return RDNA4BA_ERR_BOUNDS;
        if (rdna4crc32(0, buf + h.vk_cache_offset, (size_t)h.vk_cache_size) != h.vk_cache_crc32)
            return RDNA4BA_ERR_ARCHIVE_CRC;
        cursor += h.vk_cache_size;
    }

    rdna4ba_archive_t tmp;
    rdna4ba_init(&tmp, &h.fingerprint);
    if (h.vk_cache_size > 0) {
        rdna4ba_status_t st = rdna4ba_set_vk_cache(&tmp, buf + h.vk_cache_offset, (size_t)h.vk_cache_size);
        if (st != RDNA4BA_OK) { rdna4ba_destroy(&tmp); return st; }
    }

    for (uint32_t i = 0; i < h.entry_count; i++) {
        rdna4ba_entry_disk_t d;
        rdna4ba_decode_entry_disk(buf + sizeof(rdna4ba_header_t) + (size_t)i * sizeof(rdna4ba_entry_disk_t), &d);
        if (!rdna4ba_kind_ok(d.kind) || (d.flags & ~RDNA4BA_ENTRY_F_MASK)) {
            rdna4ba_destroy(&tmp); return RDNA4BA_ERR_HEADER;
        }
        if (d.kind != RDNA4BA_KIND_RENDER && (d.spv1_size || d.meta1_size)) {
            rdna4ba_destroy(&tmp); return RDNA4BA_ERR_HEADER;
        }
        uint64_t sum = (uint64_t)d.spv0_size + (uint64_t)d.meta0_size +
                       (uint64_t)d.spv1_size + (uint64_t)d.meta1_size +
                       (uint64_t)d.vk_cache_size;
        if (sum != d.payload_size || d.payload_size > RDNA4BA_MAX_ENTRY_PAYLOAD) {
            rdna4ba_destroy(&tmp); return RDNA4BA_ERR_BOUNDS;
        }
        if (d.payload_offset != cursor || d.payload_size > h.total_size - cursor) {
            rdna4ba_destroy(&tmp); return RDNA4BA_ERR_BOUNDS;
        }
        uint32_t got_crc = d.payload_size ? rdna4crc32(0, buf + d.payload_offset, (size_t)d.payload_size) : 0u;
        if (got_crc != d.payload_crc32) {
            rdna4ba_destroy(&tmp); return RDNA4BA_ERR_ENTRY_CRC;
        }
        if (rdna4ba_find_index(&tmp, d.kind, d.key_u64, d.key_sha) >= 0) {
            rdna4ba_destroy(&tmp); return RDNA4BA_ERR_DUPLICATE;
        }

        const uint8_t *p = buf + d.payload_offset;
        size_t off = 0;
        rdna4ba_entry_view_t v;
        memset(&v, 0, sizeof v);
        v.kind = d.kind;
        v.flags = d.flags;
        v.key_u64 = d.key_u64;
        memcpy(v.key_sha, d.key_sha, 32);
        if (d.spv0_size)     { v.spv0     = p + off; v.spv0_size     = d.spv0_size;     off += d.spv0_size; }
        if (d.meta0_size)    { v.meta0    = p + off; v.meta0_size    = d.meta0_size;    off += d.meta0_size; }
        if (d.spv1_size)     { v.spv1     = p + off; v.spv1_size     = d.spv1_size;     off += d.spv1_size; }
        if (d.meta1_size)    { v.meta1    = p + off; v.meta1_size    = d.meta1_size;    off += d.meta1_size; }
        if (d.vk_cache_size) { v.vk_cache = p + off; v.vk_cache_size = d.vk_cache_size; }

        rdna4ba_status_t st = rdna4ba_put(&tmp, &v);
        if (st != RDNA4BA_OK) { rdna4ba_destroy(&tmp); return st; }
        cursor += d.payload_size;
    }

    if (cursor != h.total_size) {
        rdna4ba_destroy(&tmp);
        return RDNA4BA_ERR_BOUNDS;
    }

    *out = tmp;
    return RDNA4BA_OK;
}

// ---- Atomic file serialization / deserialization ----
static inline rdna4ba_status_t rdna4ba_write_file_atomic(const rdna4ba_archive_t *a, const char *path, int *out_errno) {
    if (out_errno) *out_errno = 0;
    if (!a || !path || !path[0]) { if (out_errno) *out_errno = EINVAL; return RDNA4BA_ERR_INVALID; }
    uint8_t *buf = NULL;
    size_t len = 0;
    rdna4ba_status_t st = rdna4ba_serialize(a, &buf, &len);
    if (st != RDNA4BA_OK) return st;

    char tmp[4096];
    int n = snprintf(tmp, sizeof tmp, "%s.tmp.XXXXXX", path);
    if (n <= 0 || (size_t)n >= sizeof tmp) {
        free(buf);
        if (out_errno) *out_errno = ENAMETOOLONG;
        return RDNA4BA_ERR_IO;
    }
    int fd = mkstemp(tmp);
    if (fd < 0) {
        int en = errno;
        free(buf);
        if (out_errno) *out_errno = en;
        return RDNA4BA_ERR_IO;
    }

    int fail = 0;
    size_t written = 0;
    while (written < len) {
        ssize_t w = write(fd, buf + written, len - written);
        if (w < 0 && errno == EINTR) continue;
        if (w <= 0) { fail = w == 0 ? EIO : errno; break; }
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
        return RDNA4BA_ERR_IO;
    }
    return RDNA4BA_OK;
}

static inline rdna4ba_status_t rdna4ba_read_file(const char *path, const rdna4ba_fingerprint_t *expected_fp,
                                                 rdna4ba_archive_t *out, int *out_errno) {
    if (out_errno) *out_errno = 0;
    if (!path || !path[0] || !out) { if (out_errno) *out_errno = EINVAL; return RDNA4BA_ERR_INVALID; }
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) { if (out_errno) *out_errno = errno; return RDNA4BA_ERR_IO; }
    struct stat sb;
    if (fstat(fd, &sb) != 0) { int en = errno; close(fd); if (out_errno) *out_errno = en; return RDNA4BA_ERR_IO; }
    if (!S_ISREG(sb.st_mode)) { close(fd); if (out_errno) *out_errno = EINVAL; return RDNA4BA_ERR_INVALID; }
    if (sb.st_size < (off_t)sizeof(rdna4ba_header_t)) { close(fd); return RDNA4BA_ERR_TRUNCATED; }
    if ((uint64_t)sb.st_size > RDNA4BA_MAX_ARCHIVE_BYTES) { close(fd); return RDNA4BA_ERR_BOUNDS; }

    size_t len = (size_t)sb.st_size;
    uint8_t *buf = (uint8_t *)malloc(len);
    if (!buf) { close(fd); return RDNA4BA_ERR_OOM; }

    size_t got = 0;
    while (got < len) {
        ssize_t r = read(fd, buf + got, len - got);
        if (r < 0 && errno == EINTR) continue;
        if (r < 0) { int en = errno; free(buf); close(fd); if (out_errno) *out_errno = en; return RDNA4BA_ERR_IO; }
        if (r == 0) break;
        got += (size_t)r;
    }
    uint8_t extra = 0;
    ssize_t tail = 0;
    if (got == len) {
        do { tail = read(fd, &extra, 1); } while (tail < 0 && errno == EINTR);
    }
    close(fd);
    if (got != len) { free(buf); return RDNA4BA_ERR_TRUNCATED; }
    if (tail != 0) { free(buf); return tail > 0 ? RDNA4BA_ERR_BOUNDS : RDNA4BA_ERR_IO; }

    rdna4ba_status_t st = rdna4ba_deserialize(buf, len, expected_fp, out);
    free(buf);
    return st;
}

#endif // RDNA4_BINARY_ARCHIVE_H
