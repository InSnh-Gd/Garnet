// garnet_vram_surf.h — Pure C state machine for Phase B WindowServer 320 GB/s OPTIMAL VRAM surface cache.
// Zero macOS/ObjC/Vulkan/locking dependencies; caller holds RDNA4VS.mu around calls and performs all
// Vulkan/IOSurface operations outside RDNA4VS.mu. Host test: test-vram-surf.c.
#ifndef GARNET_VRAM_SURF_H
#define GARNET_VRAM_SURF_H

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#define GARNET_VS_KILL_FILE            "/private/tmp/garnet-no-vram-surf"
#define GARNET_VS_SYNC_WB_FILE         "/private/tmp/garnet-sync-writeback"
#define GARNET_VS_IDLE_FLUSH_NS        50000000ULL     // 50 ms idle quiescence writeback
#define GARNET_VS_CLASS2_MIN_DIM       256u
#define GARNET_VS_CLASS2_WRITES_NEEDED 2u
#define GARNET_VS_CLASS2_WINDOW_NS     1000000000ULL   // 1 s sliding window
#define GARNET_VS_OBS_CAP              128u

// Vulkan VkFormat numeric values (verified via _Static_assert in garnet_resources.m).
#define GARNET_VS_VK_RGBA8_UNORM       37u
#define GARNET_VS_VK_RGBA8_SRGB        43u
#define GARNET_VS_VK_BGRA8_UNORM       44u
#define GARNET_VS_VK_BGRA8_SRGB        50u
#define GARNET_VS_VK_A2R10G10B10_UNORM 58u
#define GARNET_VS_VK_A2B10G10R10_UNORM 64u
#define GARNET_VS_VK_RGBA16_SFLOAT     97u

enum {
    GARNET_VS_FAM_NONE         = 0,
    GARNET_VS_FAM_BGRA8        = 1,
    GARNET_VS_FAM_RGBA8        = 2,
    GARNET_VS_FAM_A2B10G10R10  = 3,
    GARNET_VS_FAM_A2R10G10B10  = 4,
    GARNET_VS_FAM_RGBA16F      = 5,
    GARNET_VS_FAM_SINGLE       = 6,
};

enum {
    GARNET_VS_PROBE_MISS          = 0,
    GARNET_VS_PROBE_HIT           = 1,
    GARNET_VS_PROBE_INCOMPAT_FLUSH = 2,
};

typedef struct {
    uint32_t sid;
    uint64_t base;
    uint64_t ialloc;
    uint64_t poff;
    uint32_t w;
    uint32_t h;
    uint64_t bpr;
} garnet_vs_range_t;

typedef struct {
    uint32_t sid;
    int valid;
    int capture_checked;
    int capture_excluded;
    uint32_t write_count;
    uint64_t window_start_ns;
    uint64_t last_use_ns;
} garnet_vs_obs_t;

typedef struct {
    uint64_t id;
    garnet_vs_range_t range;
    uint32_t fmt_family;
    uint32_t base_vk_fmt;
    uint32_t usage_mask;
    uint32_t bpp;
    uint64_t vram_bytes;
    void *opt_img;
    void *opt_mem;
    void *ibuf;
    void *imem;
    void *owner;
    uint32_t layout;
    int refs;
    int active_cbs;
    int is_disp;
    int stale;
    int in_unused;
    int vram_valid;
    int host_dirty;
    int flushing;
    uint32_t last_seed;
    uint64_t write_gen;
    uint64_t last_write_ns;
    uint64_t last_unref_ns;
    uint64_t last_use_ns;
} garnet_vs_ent_t;

typedef struct {
    void *opt_img;
    void *opt_mem;
    void *ibuf;
    void *imem;
    void *owner;
    uint64_t vram_bytes;
    uint64_t stamp;
} garnet_vs_rel_t;

typedef struct {
    int on;
    uint64_t cap_bytes;
    uint64_t next_id;
    garnet_vs_ent_t *e;
    size_t n, c;
    uint64_t live_bytes;
    uint64_t unused_bytes;
    size_t dirty_count;
    garnet_vs_rel_t *rl;
    size_t nrl, crl;
    garnet_vs_obs_t obs[GARNET_VS_OBS_CAP];
    uint64_t obs_tick;
    // Telemetry counters
    uint64_t created, hits, misses, incompat_flushes, relocations;
    uint64_t uploads, uploads_skipped, lazy_writes, sync_writes, idle_flushes;
    uint64_t evictions, capture_excluded_sids;
} garnet_vs_cache_t;

static inline int garnet_vs_should_enable(int is_ws, int test_opt_in, int kill_present) {
    return (is_ws || test_opt_in) && !kill_present ? 1 : 0;
}

// Per-texture Path (b) IOSurface upload-elision state packed in a single atomic uint64_t:
// bit 32 = valid flag (allows seed == 0 to be cached unambiguously), bits 31..0 = last uploaded seed.
static inline uint64_t garnet_vs_pathb_pack(uint32_t seed) {
    return (1ULL << 32) | (uint64_t)seed;
}

static inline int garnet_vs_pathb_should_upload(uint64_t state, uint32_t current_seed) {
    return ((state >> 32) != 0 && (uint32_t)state == current_seed) ? 0 : 1;
}

// Classify Vulkan format into compatibility family and return explicit VkImageFormatListCreateInfo view list
// (2 entries for UNORM/sRGB pairs so RADV keeps GFX12 DCC with VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT; 0 otherwise).
static inline uint32_t garnet_vs_format_family(uint32_t vk_fmt, uint32_t *base_fmt_out,
                                               uint32_t list_out[2], uint32_t *list_count_out) {
    uint32_t fam = GARNET_VS_FAM_NONE;
    uint32_t base = vk_fmt;
    uint32_t cnt = 0;
    if (vk_fmt == GARNET_VS_VK_BGRA8_UNORM || vk_fmt == GARNET_VS_VK_BGRA8_SRGB) {
        fam = GARNET_VS_FAM_BGRA8;
        base = GARNET_VS_VK_BGRA8_UNORM;
        if (list_out) { list_out[0] = GARNET_VS_VK_BGRA8_UNORM; list_out[1] = GARNET_VS_VK_BGRA8_SRGB; }
        cnt = 2;
    } else if (vk_fmt == GARNET_VS_VK_RGBA8_UNORM || vk_fmt == GARNET_VS_VK_RGBA8_SRGB) {
        fam = GARNET_VS_FAM_RGBA8;
        base = GARNET_VS_VK_RGBA8_UNORM;
        if (list_out) { list_out[0] = GARNET_VS_VK_RGBA8_UNORM; list_out[1] = GARNET_VS_VK_RGBA8_SRGB; }
        cnt = 2;
    } else if (vk_fmt == GARNET_VS_VK_A2B10G10R10_UNORM) {
        fam = GARNET_VS_FAM_A2B10G10R10;
    } else if (vk_fmt == GARNET_VS_VK_A2R10G10B10_UNORM) {
        fam = GARNET_VS_FAM_A2R10G10B10;
    } else if (vk_fmt == GARNET_VS_VK_RGBA16_SFLOAT) {
        fam = GARNET_VS_FAM_RGBA16F;
    } else if (vk_fmt != 0) {
        fam = GARNET_VS_FAM_SINGLE;
    }
    if (base_fmt_out) *base_fmt_out = base;
    if (list_count_out) *list_count_out = cnt;
    return fam;
}

static inline int garnet_vs_format_compatible(uint32_t entry_fam, uint32_t entry_base_fmt, uint32_t req_vk_fmt) {
    if (entry_fam == GARNET_VS_FAM_NONE) return 0;
    const uint32_t req_fam = garnet_vs_format_family(req_vk_fmt, NULL, NULL, NULL);
    if (req_fam != entry_fam) return 0;
    if (entry_fam == GARNET_VS_FAM_SINGLE) return req_vk_fmt == entry_base_fmt ? 1 : 0;
    return 1;
}

static inline int garnet_vs_symbol_is_capture(const char *sym) {
    if (!sym || !sym[0]) return 0;
    return strstr(sym, "CaptureSurfaceMetal") != NULL ||
           strstr(sym, "_XHWCaptureDesktop") != NULL ||
           strstr(sym, "CreateCompositeDestination") != NULL ||
           strstr(sym, "ScreenCapture") != NULL ||
           strstr(sym, "CGXCapture") != NULL ||
           strstr(sym, "WSDisplayCapture") != NULL;
}

static inline void garnet_vs_init(garnet_vs_cache_t *c, int on, uint64_t cap_bytes) {
    memset(c, 0, sizeof *c);
    c->on = on ? 1 : 0;
    c->cap_bytes = cap_bytes ? cap_bytes : (512ULL << 20);
}

static inline void garnet_vs_destroy(garnet_vs_cache_t *c) {
    free(c->e);
    free(c->rl);
    const int on = c->on;
    const uint64_t cap = c->cap_bytes;
    memset(c, 0, sizeof *c);
    c->on = on;
    c->cap_bytes = cap;
}

static inline garnet_vs_ent_t *garnet_vs_find(garnet_vs_cache_t *c, uint64_t id) {
    if (!id) return NULL;
    for (size_t i = 0; i < c->n; i++) {
        if (c->e[i].id == id) return &c->e[i];
    }
    return NULL;
}

static inline int garnet_vs_range_eq(const garnet_vs_range_t *a, const garnet_vs_range_t *b) {
    return a->sid == b->sid && a->base == b->base && a->ialloc == b->ialloc &&
           a->poff == b->poff && a->w == b->w && a->h == b->h && a->bpr == b->bpr;
}

static inline void garnet_vs_clear_unused(garnet_vs_cache_t *c, garnet_vs_ent_t *e) {
    if (e->in_unused) {
        c->unused_bytes -= e->vram_bytes;
        e->in_unused = 0;
    }
}

static inline void garnet_vs_clear_dirty(garnet_vs_cache_t *c, garnet_vs_ent_t *e) {
    if (e->host_dirty) {
        e->host_dirty = 0;
        if (c->dirty_count > 0) c->dirty_count--;
    }
}

static inline void garnet_vs_release_idx(garnet_vs_cache_t *c, size_t idx, uint64_t stamp) {
    garnet_vs_ent_t *e = &c->e[idx];
    garnet_vs_clear_unused(c, e);
    garnet_vs_clear_dirty(c, e);
    if (c->nrl == c->crl) {
        c->crl = c->crl ? c->crl * 2 : 16;
        c->rl = (garnet_vs_rel_t *)realloc(c->rl, c->crl * sizeof *c->rl);
    }
    c->rl[c->nrl++] = (garnet_vs_rel_t){
        e->opt_img, e->opt_mem, e->ibuf, e->imem, e->owner, e->vram_bytes, stamp
    };
    c->live_bytes -= e->vram_bytes;
    memmove(&c->e[idx], &c->e[idx + 1], (c->n - idx - 1) * sizeof *c->e);
    c->n--;
}

// ---- Write-observation & capture-exclusion admission (Class 1 / Class 2) ----

static inline garnet_vs_obs_t *garnet_vs_obs_find(garnet_vs_cache_t *c, uint32_t sid) {
    if (!sid) return NULL;
    for (uint32_t i = 0; i < GARNET_VS_OBS_CAP; i++) {
        if (c->obs[i].valid && c->obs[i].sid == sid) return &c->obs[i];
    }
    return NULL;
}

static inline int garnet_vs_obs_needs_bt(garnet_vs_cache_t *c, uint32_t sid) {
    if (!c->on || !sid) return 0;
    garnet_vs_obs_t *o = garnet_vs_obs_find(c, sid);
    return (!o || !o->capture_checked) ? 1 : 0;
}

static inline int garnet_vs_note_ws_write(garnet_vs_cache_t *c, uint32_t sid, uint32_t w, uint32_t h,
                                          int is_disp, int is_capture_bt, uint64_t now_ns) {
    if (!c->on || !sid || is_disp) return 0;
    garnet_vs_obs_t *o = garnet_vs_obs_find(c, sid);
    if (!o) {
        uint32_t v = 0;
        int foundFree = 0;
        for (uint32_t i = 0; i < GARNET_VS_OBS_CAP; i++) {
            if (!c->obs[i].valid) { v = i; foundFree = 1; break; }
            if (c->obs[i].last_use_ns < c->obs[v].last_use_ns) v = i;
        }
        (void)foundFree;
        o = &c->obs[v];
        memset(o, 0, sizeof *o);
        o->sid = sid;
        o->valid = 1;
    }
    o->last_use_ns = ++c->obs_tick;
    if (!o->capture_checked) {
        o->capture_checked = 1;
        o->capture_excluded = is_capture_bt ? 1 : 0;
        if (o->capture_excluded) c->capture_excluded_sids++;
    }
    if (o->capture_excluded) return 0;
    if (w < GARNET_VS_CLASS2_MIN_DIM || h < GARNET_VS_CLASS2_MIN_DIM) return 0;
    if (o->write_count == 0 || now_ns < o->window_start_ns ||
        (now_ns - o->window_start_ns) > GARNET_VS_CLASS2_WINDOW_NS) {
        o->window_start_ns = now_ns;
        o->write_count = 1;
    } else {
        o->write_count++;
    }
    return o->write_count >= GARNET_VS_CLASS2_WRITES_NEEDED ? 1 : 0;
}

static inline void garnet_vs_mark_capture_excluded(garnet_vs_cache_t *c, uint32_t sid) {
    if (!c->on || !sid) return;
    garnet_vs_obs_t *o = garnet_vs_obs_find(c, sid);
    if (!o) {
        uint32_t v = 0;
        for (uint32_t i = 0; i < GARNET_VS_OBS_CAP; i++) {
            if (!c->obs[i].valid) { v = i; break; }
            if (c->obs[i].last_use_ns < c->obs[v].last_use_ns) v = i;
        }
        o = &c->obs[v];
        memset(o, 0, sizeof *o);
        o->sid = sid;
        o->valid = 1;
    }
    o->last_use_ns = ++c->obs_tick;
    if (!o->capture_excluded) {
        o->capture_excluded = 1;
        c->capture_excluded_sids++;
    }
    o->capture_checked = 1;
}

static inline int garnet_vs_should_promote(garnet_vs_cache_t *c, uint32_t sid, uint32_t w, uint32_t h,
                                           int is_disp, int is_readonly, size_t plane_count, int req_write_usage) {
    if (!c->on || !sid || is_readonly || plane_count > 1 || !req_write_usage) return 0;
    if (is_disp) return 1;
    if (w < GARNET_VS_CLASS2_MIN_DIM || h < GARNET_VS_CLASS2_MIN_DIM) return 0;
    garnet_vs_obs_t *o = garnet_vs_obs_find(c, sid);
    if (!o || o->capture_excluded) return 0;
    return o->write_count >= GARNET_VS_CLASS2_WRITES_NEEDED ? 1 : 0;
}

// ---- Probe / Add / Unref / Retire ----

static inline int garnet_vs_probe(garnet_vs_cache_t *c, const garnet_vs_range_t *r,
                                  uint32_t req_vk_fmt, uint32_t req_usage_mask,
                                  uint64_t now_ns, uint64_t fence_stamp, uint64_t *out_id) {
    if (out_id) *out_id = 0;
    if (!c->on || !r || !r->sid) return GARNET_VS_PROBE_MISS;

    // 1. Detect relocation (same sid, different backing address/size) or overlapping sub-range view.
    for (size_t i = 0; i < c->n;) {
        garnet_vs_ent_t *e = &c->e[i];
        if (e->range.sid != r->sid || e->stale) { i++; continue; }
        if (garnet_vs_range_eq(&e->range, r)) { i++; continue; }
        if (e->range.base != r->base || e->range.ialloc != r->ialloc) {
            // Relocated backing: old IOSurface allocation is gone; mark stale and retire when idle.
            c->relocations++;
            e->stale = 1;
            garnet_vs_clear_unused(c, e);
            garnet_vs_clear_dirty(c, e);
            if (e->refs == 0 && e->active_cbs == 0 && !e->flushing) {
                garnet_vs_release_idx(c, i, fence_stamp);
                continue;
            }
            i++;
            continue;
        }
        // Same backing (base, ialloc) with different geometry/plane offset -> flush & invalidate before fallback.
        e->stale = 1;
        garnet_vs_clear_unused(c, e);
        c->incompat_flushes++;
        if (e->refs == 0 && e->active_cbs == 0 && !e->flushing && (!e->host_dirty || e->layout == 0)) {
            garnet_vs_release_idx(c, i, fence_stamp);
            continue;
        }
        if (out_id) *out_id = e->id;
        return GARNET_VS_PROBE_INCOMPAT_FLUSH;
    }

    // 2. Match exact physical range.
    for (size_t i = 0; i < c->n; i++) {
        garnet_vs_ent_t *e = &c->e[i];
        if (e->stale || !garnet_vs_range_eq(&e->range, r)) continue;
        if (garnet_vs_format_compatible(e->fmt_family, e->base_vk_fmt, req_vk_fmt) &&
            (e->usage_mask & req_usage_mask) == req_usage_mask) {
            garnet_vs_clear_unused(c, e);
            e->refs++;
            e->last_use_ns = now_ns;
            c->hits++;
            if (out_id) *out_id = e->id;
            return GARNET_VS_PROBE_HIT;
        }
        e->stale = 1;
        garnet_vs_clear_unused(c, e);
        c->incompat_flushes++;
        if (e->refs == 0 && e->active_cbs == 0 && !e->flushing && (!e->host_dirty || e->layout == 0)) {
            garnet_vs_release_idx(c, i, fence_stamp);
            if (out_id) *out_id = 0;
            return GARNET_VS_PROBE_INCOMPAT_FLUSH;
        }
        if (out_id) *out_id = e->id;
        return GARNET_VS_PROBE_INCOMPAT_FLUSH;
    }

    c->misses++;
    return GARNET_VS_PROBE_MISS;
}

static inline uint64_t garnet_vs_add(garnet_vs_cache_t *c, const garnet_vs_range_t *r,
                                     uint32_t vk_fmt, uint32_t usage_mask, uint64_t vram_bytes,
                                     void *opt_img, void *opt_mem, void *ibuf, void *imem, void *owner,
                                     uint64_t now_ns) {
    if (c->n == c->c) {
        const size_t nc = c->c ? c->c * 2 : 16;
        garnet_vs_ent_t *ne = (garnet_vs_ent_t *)realloc(c->e, nc * sizeof *c->e);
        if (!ne) return 0;
        c->e = ne;
        c->c = nc;
    }
    uint32_t base_fmt = vk_fmt;
    const uint32_t fam = garnet_vs_format_family(vk_fmt, &base_fmt, NULL, NULL);
    const uint64_t id = ++c->next_id;
    garnet_vs_ent_t e;
    memset(&e, 0, sizeof e);
    e.id = id;
    e.range = *r;
    e.fmt_family = fam;
    e.base_vk_fmt = base_fmt;
    e.usage_mask = usage_mask;
    e.bpp = 4u;
    e.vram_bytes = vram_bytes;
    e.opt_img = opt_img;
    e.opt_mem = opt_mem;
    e.ibuf = ibuf;
    e.imem = imem;
    e.owner = owner;
    e.refs = 1;
    e.last_use_ns = now_ns;
    c->e[c->n++] = e;
    c->live_bytes += vram_bytes;
    c->created++;
    return id;
}

static inline void garnet_vs_retire_stale_ready(garnet_vs_cache_t *c, uint64_t fence_stamp) {
    for (size_t i = 0; i < c->n;) {
        garnet_vs_ent_t *e = &c->e[i];
        if (e->stale && e->refs == 0 && e->active_cbs == 0 && !e->flushing && !e->host_dirty) {
            garnet_vs_release_idx(c, i, fence_stamp);
        } else {
            i++;
        }
    }
}

static inline int garnet_vs_unref(garnet_vs_cache_t *c, uint64_t id, uint64_t now_ns, uint64_t fence_stamp) {
    for (size_t i = 0; i < c->n; i++) {
        if (c->e[i].id != id) continue;
        garnet_vs_ent_t *e = &c->e[i];
        if (e->refs > 0) e->refs--;
        if (e->refs == 0) {
            if (e->stale) {
                garnet_vs_clear_unused(c, e);
                if (e->active_cbs == 0 && !e->flushing && !e->host_dirty)
                    garnet_vs_release_idx(c, i, fence_stamp);
            } else {
                e->last_unref_ns = now_ns;
                if (!e->in_unused) {
                    e->in_unused = 1;
                    c->unused_bytes += e->vram_bytes;
                }
            }
        }
        return 1;
    }
    return 0;
}

// ---- Layout, CB activity, Upload & Write Coherency ----

static inline uint32_t garnet_vs_get_layout(garnet_vs_cache_t *c, uint64_t id) {
    garnet_vs_ent_t *e = garnet_vs_find(c, id);
    return e ? e->layout : 0u;
}

static inline void garnet_vs_set_layout(garnet_vs_cache_t *c, uint64_t id, uint32_t layout) {
    garnet_vs_ent_t *e = garnet_vs_find(c, id);
    if (e) e->layout = layout;
}

static inline void garnet_vs_set_bpp(garnet_vs_cache_t *c, uint64_t id, uint32_t bpp) {
    garnet_vs_ent_t *e = garnet_vs_find(c, id);
    if (e && bpp) e->bpp = bpp;
}

static inline void garnet_vs_set_disp(garnet_vs_cache_t *c, uint64_t id, int is_disp) {
    garnet_vs_ent_t *e = garnet_vs_find(c, id);
    if (e) e->is_disp = is_disp ? 1 : 0;
}

static inline int garnet_vs_is_flushing(garnet_vs_cache_t *c, uint64_t id) {
    garnet_vs_ent_t *e = garnet_vs_find(c, id);
    return (e && e->flushing) ? 1 : 0;
}

static inline void garnet_vs_cb_begin(garnet_vs_cache_t *c, uint64_t id) {
    garnet_vs_ent_t *e = garnet_vs_find(c, id);
    if (e) e->active_cbs++;
}

static inline void garnet_vs_cb_end(garnet_vs_cache_t *c, uint64_t id) {
    garnet_vs_ent_t *e = garnet_vs_find(c, id);
    if (e && e->active_cbs > 0) e->active_cbs--;
}

// Read-only peek: returns 1 if accessing `id` with `need_contents=1` would require a host->VRAM upload.
// A valid GPU-dirty DisplaySurface owns newer pixels in VRAM than host RAM; never overwrite it from stale host pages.
static inline int garnet_vs_peek_needs_upload(const garnet_vs_cache_t *c, uint64_t id, uint32_t current_seed) {
    if (!id) return 1;
    for (size_t i = 0; i < c->n; i++) {
        if (c->e[i].id == id) {
            if (!c->e[i].vram_valid) return 1;
            if (current_seed == c->e[i].last_seed) return 0;
            if (c->e[i].is_disp && c->e[i].host_dirty) return 0;
            return 1;
        }
    }
    return 1;
}

// Returns 1 if caller must upload host _ibuf -> opt_img before GPU use, 0 to skip upload.
static inline int garnet_vs_begin_access(garnet_vs_cache_t *c, uint64_t id, uint32_t current_seed,
                                         int need_contents, int will_write, int full_overwrite) {
    garnet_vs_ent_t *e = garnet_vs_find(c, id);
    if (!e) return need_contents ? 1 : 0;
    if (need_contents) {
        if (e->vram_valid && e->is_disp && e->host_dirty && current_seed != e->last_seed) {
            e->last_seed = current_seed;
            c->uploads_skipped++;
            return 0;
        }
        if (!e->vram_valid || current_seed != e->last_seed) {
            e->vram_valid = 1;
            e->last_seed = current_seed;
            c->uploads++;
            return 1;
        }
        c->uploads_skipped++;
        return 0;
    }
    if (will_write && full_overwrite) {
        e->vram_valid = 1;
        e->last_seed = current_seed;
    }
    c->uploads_skipped++;
    return 0;
}

static inline void garnet_vs_note_gpu_write(garnet_vs_cache_t *c, uint64_t id, uint32_t current_seed,
                                            uint64_t now_ns, int sync_writeback) {
    garnet_vs_ent_t *e = garnet_vs_find(c, id);
    if (!e) return;
    e->vram_valid = 1;
    e->last_seed = current_seed;
    e->write_gen++;
    e->last_write_ns = now_ns;
    if (sync_writeback) {
        garnet_vs_clear_dirty(c, e);
        c->sync_writes++;
    } else {
        if (!e->host_dirty) {
            e->host_dirty = 1;
            c->dirty_count++;
        }
        c->lazy_writes++;
    }
}

static inline void garnet_vs_invalidate_cpu_write(garnet_vs_cache_t *c, uint64_t id) {
    garnet_vs_ent_t *e = garnet_vs_find(c, id);
    if (!e) return;
    e->vram_valid = 0;
    garnet_vs_clear_dirty(c, e);
}

// ---- Idle & Sync Flush Scheduling ----

static inline int garnet_vs_pick_idle_flush(garnet_vs_cache_t *c, uint64_t now_ns,
                                            uint64_t *out_id, uint64_t *out_gen) {
    for (size_t i = 0; i < c->n; i++) {
        garnet_vs_ent_t *e = &c->e[i];
        if (!e->host_dirty || e->flushing || e->active_cbs > 0 || e->layout == 0) continue;
        if (now_ns < e->last_write_ns || (now_ns - e->last_write_ns) < GARNET_VS_IDLE_FLUSH_NS) continue;
        e->flushing = 1;
        if (out_id) *out_id = e->id;
        if (out_gen) *out_gen = e->write_gen;
        return 1;
    }
    return 0;
}

static inline int garnet_vs_begin_sync_flush(garnet_vs_cache_t *c, uint64_t id, uint64_t *out_gen) {
    garnet_vs_ent_t *e = garnet_vs_find(c, id);
    if (!e || !e->host_dirty || e->flushing || e->layout == 0) return 0;
    e->flushing = 1;
    if (out_gen) *out_gen = e->write_gen;
    return 1;
}

static inline void garnet_vs_finish_flush(garnet_vs_cache_t *c, uint64_t id, uint64_t snap_gen, int ok) {
    garnet_vs_ent_t *e = garnet_vs_find(c, id);
    if (!e) return;
    e->flushing = 0;
    if (ok && e->host_dirty && e->write_gen == snap_gen) {
        garnet_vs_clear_dirty(c, e);
        c->idle_flushes++;
    }
}

// ---- LRU Eviction & Fence-Gated Release ----

static inline int garnet_vs_pick_evict_candidate(garnet_vs_cache_t *c, uint64_t *out_id, int *out_needs_flush) {
    if (c->unused_bytes <= c->cap_bytes) return 0;
    size_t best = c->n;
    for (size_t i = 0; i < c->n; i++) {
        const garnet_vs_ent_t *e = &c->e[i];
        if (e->refs != 0 || e->active_cbs != 0 || e->flushing || !e->in_unused) continue;
        if (best == c->n || e->last_unref_ns < c->e[best].last_unref_ns) best = i;
    }
    if (best == c->n) return 0;
    if (out_id) *out_id = c->e[best].id;
    if (out_needs_flush) *out_needs_flush = c->e[best].host_dirty ? 1 : 0;
    return 1;
}

static inline int garnet_vs_retire(garnet_vs_cache_t *c, uint64_t id, uint64_t fence_stamp) {
    for (size_t i = 0; i < c->n; i++) {
        if (c->e[i].id != id) continue;
        if (c->e[i].refs != 0 || c->e[i].active_cbs != 0 || c->e[i].flushing || c->e[i].host_dirty) return 0;
        c->evictions++;
        garnet_vs_release_idx(c, i, fence_stamp);
        return 1;
    }
    return 0;
}

static inline int garnet_vs_pop_release(garnet_vs_cache_t *c, uint64_t completed_stamp, garnet_vs_rel_t *out) {
    for (size_t i = 0; i < c->nrl; i++) {
        if (c->rl[i].stamp <= completed_stamp) {
            if (out) *out = c->rl[i];
            memmove(&c->rl[i], &c->rl[i + 1], (c->nrl - i - 1) * sizeof *c->rl);
            c->nrl--;
            return 1;
        }
    }
    return 0;
}

#endif
