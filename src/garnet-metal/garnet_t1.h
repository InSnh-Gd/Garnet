// garnet_t1.h: pure helpers for native #12 "bundle work" (no Vulkan, no Foundation; host test: test-t1.c).
//  (a) T1 timing aggregation: lock-free count/sum/min/max + a fixed 8-bucket histogram, drained once per 10 s window.
//  (b) T2 persisted pipeline cache file: our header (magic, version, key, RADV UUID/vendor/device, payload length, FNV-1a) around the raw
//      vkGetPipelineCacheData blob, and the check that decides whether a file may be handed to vkCreatePipelineCache as initialData.
#ifndef RDNA4_T1_H
#define RDNA4_T1_H
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <stdio.h>
#include <stdatomic.h>

// ---- (a) histograms ------------------------------------------------------------------------------------------------
#define RDNA4T_NB 8
// Bucket upper bounds in ns: 0.1, 0.4, 1.6, 6.4, 25.6, 102.4, 409.6 ms; the last bucket is everything above.
static inline int rdna4t_bucket(uint64_t ns) {
    uint64_t lim = 100000ULL;
    for (int i = 0; i < RDNA4T_NB - 1; i++, lim *= 4) if (ns < lim) return i;
    return RDNA4T_NB - 1;
}
typedef struct { _Atomic uint64_t n, sum, nmin /* stores ~min so that zero means "no sample" */, max, b[RDNA4T_NB]; } rdna4t_hist;
typedef struct { uint64_t n, sum, min, max, b[RDNA4T_NB]; } rdna4t_snap;
static inline void rdna4t_add(rdna4t_hist *h, uint64_t ns) {
    atomic_fetch_add_explicit(&h->n, 1, memory_order_relaxed);
    atomic_fetch_add_explicit(&h->sum, ns, memory_order_relaxed);
    atomic_fetch_add_explicit(&h->b[rdna4t_bucket(ns)], 1, memory_order_relaxed);
    uint64_t cur = atomic_load_explicit(&h->max, memory_order_relaxed);
    while (ns > cur && !atomic_compare_exchange_weak_explicit(&h->max, &cur, ns, memory_order_relaxed, memory_order_relaxed)) {}
    uint64_t inv = ~ns; cur = atomic_load_explicit(&h->nmin, memory_order_relaxed);
    while (inv > cur && !atomic_compare_exchange_weak_explicit(&h->nmin, &cur, inv, memory_order_relaxed, memory_order_relaxed)) {}
}
// Drains h into s (each field exchanged to zero; a sample landing between two exchanges is attributed to this or the next window, never lost).
static inline void rdna4t_take(rdna4t_hist *h, rdna4t_snap *s) {
    s->n = atomic_exchange(&h->n, 0); s->sum = atomic_exchange(&h->sum, 0); s->max = atomic_exchange(&h->max, 0);
    uint64_t nm = atomic_exchange(&h->nmin, 0); s->min = nm ? ~nm : 0;
    for (int i = 0; i < RDNA4T_NB; i++) s->b[i] = atomic_exchange(&h->b[i], 0);
}
// One summary line (no newline); returns the length written (0 when the window had no samples).
static inline int rdna4t_fmt(const char *name, const rdna4t_snap *s, char *out, size_t cap) {
    if (!s->n) return 0;
    return snprintf(out, cap, "T1 %s n=%llu avg=%.3fms min=%.3fms max=%.3fms hist(<0.1,<0.4,<1.6,<6.4,<25.6,<102,<410,>=410ms)=%llu/%llu/%llu/%llu/%llu/%llu/%llu/%llu",
        name, (unsigned long long)s->n, (double)s->sum / (double)s->n / 1e6, (double)s->min / 1e6, (double)s->max / 1e6,
        (unsigned long long)s->b[0], (unsigned long long)s->b[1], (unsigned long long)s->b[2], (unsigned long long)s->b[3],
        (unsigned long long)s->b[4], (unsigned long long)s->b[5], (unsigned long long)s->b[6], (unsigned long long)s->b[7]);
}

// ---- (b) pipeline-cache file ---------------------------------------------------------------------------------------
#define RDNA4PC_VERSION 1u
// NOTE: the 8-byte pipeline-cache magic below is the legacy wire signature
// "N48PCv1". It is a serialized on-disk format and is NOT renamed with the
// RDNA4 software identities (cf. N48N/N48D serialized constants policy).
#define RDNA4PC_MAX_PAYLOAD      (256ULL << 20)
#define RDNA4PC_WS_MAX_PAYLOAD   (32ULL << 20)
#define RDNA4PC_SLOW_COMPILE_NS  (500ULL * 1000000ULL)
#define RDNA4PC_BATCH_PIPES      8ULL
#define RDNA4PC_SAVE_DEBOUNCE_NS (5ULL * 1000000000ULL)
typedef struct {
    char magic[8];            // "N48PCv1" legacy wire signature (kept, not a chip name)
    uint32_t version;         // RDNA4PC_VERSION
    uint32_t hdr_size;        // sizeof(rdna4pc_hdr)
    uint8_t key[32];          // sha256(cache policy | complete RADV library contents)
    uint8_t uuid[16];         // VkPhysicalDeviceProperties::pipelineCacheUUID
    uint32_t vendor, device;  // VkPhysicalDeviceProperties vendorID / deviceID
    uint64_t payload_len;     // bytes of the vkGetPipelineCacheData blob that follow
    uint64_t payload_fnv;     // FNV-1a 64 of the blob
} rdna4pc_hdr;
static inline uint64_t rdna4pc_fnv(const void *p, size_t n) {
    const uint8_t *b = (const uint8_t *)p; uint64_t h = 1469598103934665603ULL;
    for (size_t i = 0; i < n; i++) { h ^= b[i]; h *= 1099511628211ULL; }
    return h;
}
static inline void rdna4pc_fill(rdna4pc_hdr *h, const uint8_t key[32], const uint8_t uuid[16], uint32_t vendor, uint32_t device, const void *payload, size_t len) {
    memset(h, 0, sizeof *h); memcpy(h->magic, "N48PCv1", 8); h->version = RDNA4PC_VERSION; h->hdr_size = (uint32_t)sizeof *h;
    memcpy(h->key, key, 32); memcpy(h->uuid, uuid, 16); h->vendor = vendor; h->device = device; h->payload_len = len; h->payload_fnv = rdna4pc_fnv(payload, len);
}
// 0 = usable; the payload pointer/length are returned. Otherwise a reason code (rdna4pc_why).
enum { RDNA4PC_OK = 0, RDNA4PC_SHORT, RDNA4PC_MAGIC, RDNA4PC_VERSION_BAD, RDNA4PC_KEY, RDNA4PC_UUID, RDNA4PC_LEN, RDNA4PC_HASH, RDNA4PC_VKHDR };
static inline const char *rdna4pc_why(int c) {
    static const char *w[] = { "ok", "file shorter than header", "bad magic", "bad version/header size", "key mismatch (other cache policy or RADV library)",
        "RADV pipelineCacheUUID/vendor/device mismatch", "payload length mismatch", "payload hash mismatch (corrupt)", "Vulkan cache header invalid" };
    return c >= 0 && c <= RDNA4PC_VKHDR ? w[c] : "?";
}
// The Vulkan-defined first 32 bytes of the blob (VkPipelineCacheHeaderVersionOne): headerSize, headerVersion(1), vendorID, deviceID, pipelineCacheUUID[16].
static inline int rdna4pc_vk_header_ok(const void *payload, size_t len, const uint8_t uuid[16], uint32_t vendor, uint32_t device) {
    uint32_t w[4]; if (len < 32) return 0; memcpy(w, payload, 16);
    return w[0] >= 32 && w[0] <= len && w[1] == 1 && w[2] == vendor && w[3] == device && !memcmp((const uint8_t *)payload + 16, uuid, 16);
}
static inline uint64_t rdna4pc_max_payload(int is_ws) {
    return is_ws ? RDNA4PC_WS_MAX_PAYLOAD : RDNA4PC_MAX_PAYLOAD;
}
static inline int rdna4pc_should_incremental_save(uint64_t compile_ns, uint64_t pipes_new, uint64_t pipes_saved, uint64_t now_ns, uint64_t last_save_ns) {
    if (pipes_new <= pipes_saved) return 0;
    if (last_save_ns != 0 && now_ns >= last_save_ns && (now_ns - last_save_ns) < RDNA4PC_SAVE_DEBOUNCE_NS) return 0;
    if (compile_ns >= RDNA4PC_SLOW_COMPILE_NS || (pipes_new - pipes_saved) >= RDNA4PC_BATCH_PIPES) return 1;
    return 0;
}
static inline int rdna4pc_check_bounded(const void *file, size_t flen, const uint8_t key[32], const uint8_t uuid[16], uint32_t vendor, uint32_t device, uint64_t max_payload, const void **payload, size_t *plen) {
    rdna4pc_hdr h; if (flen < sizeof h) return RDNA4PC_SHORT;
    memcpy(&h, file, sizeof h);
    if (memcmp(h.magic, "N48PCv1", 8)) return RDNA4PC_MAGIC;
    if (h.version != RDNA4PC_VERSION || h.hdr_size != sizeof h) return RDNA4PC_VERSION_BAD;
    if (memcmp(h.key, key, 32)) return RDNA4PC_KEY;
    if (memcmp(h.uuid, uuid, 16) || h.vendor != vendor || h.device != device) return RDNA4PC_UUID;
    if (h.payload_len > max_payload || h.payload_len != flen - sizeof h) return RDNA4PC_LEN;
    const uint8_t *p = (const uint8_t *)file + sizeof h;
    if (rdna4pc_fnv(p, (size_t)h.payload_len) != h.payload_fnv) return RDNA4PC_HASH;
    if (!rdna4pc_vk_header_ok(p, (size_t)h.payload_len, uuid, vendor, device)) return RDNA4PC_VKHDR;
    *payload = p; *plen = (size_t)h.payload_len; return RDNA4PC_OK;
}
static inline int rdna4pc_check(const void *file, size_t flen, const uint8_t key[32], const uint8_t uuid[16], uint32_t vendor, uint32_t device, const void **payload, size_t *plen) {
    return rdna4pc_check_bounded(file, flen, key, uuid, vendor, device, RDNA4PC_MAX_PAYLOAD, payload, plen);
}
#endif
