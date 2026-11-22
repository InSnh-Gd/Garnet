// Pure OpenGL-on-Metal surface table and IOAccelDeviceRegion geometry parser.
// Host-testable with zero macOS, IOKit, IOSurface, or Objective-C dependencies.
// Adapted from RX4Darwin/AMDRadeonRDNA4.kext (tools/metal/native/RDNA4MTLDevice.mm,
// commit 1a034e6a, next @ dded456d, Copyright (c) 2026 Sunneva N. Mariu, BSD 3-Clause).
// Standalone C11 surface state helper without private codegen service paths.
#ifndef RDNA4_GL_SURFACE_PURE_H
#define RDNA4_GL_SURFACE_PURE_H

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#ifdef __cplusplus
extern "C" {
#endif

#define RDNA4GL_MAX_SURFACES    64u
#define RDNA4GL_NUM_BUFFERS     2u
#define RDNA4GL_MAX_DIM         16384u
#define RDNA4GL_CONNECT_TAG     0x47524e5400000000ULL // 'GRNT' << 32 (disjoint from 32-bit io_connect_t)
#define RDNA4GL_CTRL_GET_BUFFER 0x14u                 // SLSFlushSurfaceWithOptionsAndIndex

typedef struct {
    uint8_t active;
    uint32_t sid;
    uint64_t connect;
    uint32_t width;
    uint32_t height;
    uint32_t buf_width;
    uint32_t buf_height;
    void *buffers[RDNA4GL_NUM_BUFFERS];
} rdna4gl_slot;

typedef struct {
    uint32_t next_seq;
    rdna4gl_slot slots[RDNA4GL_MAX_SURFACES];
} rdna4gl_table;

static inline void rdna4gl_init(rdna4gl_table *t) {
    if (t) memset(t, 0, sizeof *t);
}

static inline int rdna4gl_is_connect(uint64_t c) {
    return (c & ~0xffffffULL) == RDNA4GL_CONNECT_TAG && (c & 0xffffffULL) != 0ULL;
}

// IOAccelDeviceRegion layout: { uint32_t num_rects; int16_t bounds[4] (x, y, w, h); ... }.
static inline int rdna4gl_parse_shape(const void *region, size_t region_bytes,
                                      uint32_t *out_w, uint32_t *out_h) {
    if (!region || region_bytes < 12u || !out_w || !out_h) return 0;
    int16_t bounds[4];
    memcpy(bounds, (const uint8_t *)region + 4u, sizeof bounds);
    if (bounds[2] <= 0 || bounds[3] <= 0) return 0;
    uint32_t w = (uint32_t)bounds[2];
    uint32_t h = (uint32_t)bounds[3];
    if (w > RDNA4GL_MAX_DIM || h > RDNA4GL_MAX_DIM) return 0;
    *out_w = w;
    *out_h = h;
    return 1;
}

static inline rdna4gl_slot *rdna4gl_find_by_connect(rdna4gl_table *t, uint64_t connect) {
    if (!t || !rdna4gl_is_connect(connect)) return NULL;
    for (unsigned i = 0; i < RDNA4GL_MAX_SURFACES; i++) {
        if (t->slots[i].active && t->slots[i].connect == connect)
            return &t->slots[i];
    }
    return NULL;
}

static inline rdna4gl_slot *rdna4gl_find_by_sid(rdna4gl_table *t, uint32_t sid) {
    if (!t || sid == 0u) return NULL;
    for (unsigned i = 0; i < RDNA4GL_MAX_SURFACES; i++) {
        if (t->slots[i].active && t->slots[i].sid == sid)
            return &t->slots[i];
    }
    return NULL;
}

static inline void rdna4gl_retire_slot_buffers(rdna4gl_slot *s,
                                               void *retired[RDNA4GL_NUM_BUFFERS],
                                               unsigned *n_retired) {
    if (n_retired) *n_retired = 0u;
    if (!s) return;
    unsigned n = 0u;
    for (unsigned i = 0; i < RDNA4GL_NUM_BUFFERS; i++) {
        if (s->buffers[i]) {
            if (retired && n_retired) retired[n++] = s->buffers[i];
            s->buffers[i] = NULL;
        }
    }
    if (n_retired) *n_retired = n;
    s->buf_width = 0u;
    s->buf_height = 0u;
}

static inline int rdna4gl_create(rdna4gl_table *t, uint32_t sid, uint64_t *out_connect) {
    if (!t || sid == 0u || !out_connect) return 0;
    unsigned free_idx = RDNA4GL_MAX_SURFACES;
    for (unsigned i = 0; i < RDNA4GL_MAX_SURFACES; i++) {
        if (!t->slots[i].active) {
            free_idx = i;
            break;
        }
    }
    if (free_idx == RDNA4GL_MAX_SURFACES) return 0;

    uint64_t connect = 0ULL;
    for (unsigned attempt = 0; attempt <= RDNA4GL_MAX_SURFACES; attempt++) {
        uint32_t seq = (t->next_seq % 0xffffffu) + 1u;
        t->next_seq = seq;
        uint64_t cand = RDNA4GL_CONNECT_TAG | (uint64_t)seq;
        if (!rdna4gl_find_by_connect(t, cand)) {
            connect = cand;
            break;
        }
    }
    if (!connect) return 0;

    rdna4gl_slot *s = &t->slots[free_idx];
    memset(s, 0, sizeof *s);
    s->active = 1u;
    s->sid = sid;
    s->connect = connect;
    *out_connect = connect;
    return 1;
}

static inline int rdna4gl_destroy(rdna4gl_table *t, uint64_t connect,
                                  void *retired[RDNA4GL_NUM_BUFFERS],
                                  unsigned *n_retired) {
    if (n_retired) *n_retired = 0u;
    rdna4gl_slot *s = rdna4gl_find_by_connect(t, connect);
    if (!s) return 0;
    rdna4gl_retire_slot_buffers(s, retired, n_retired);
    memset(s, 0, sizeof *s);
    return 1;
}

static inline int rdna4gl_set_shape(rdna4gl_table *t, uint64_t connect,
                                    const void *region, size_t region_bytes,
                                    void *retired[RDNA4GL_NUM_BUFFERS],
                                    unsigned *n_retired) {
    if (n_retired) *n_retired = 0u;
    rdna4gl_slot *s = rdna4gl_find_by_connect(t, connect);
    if (!s) return 0;
    uint32_t w = 0u, h = 0u;
    if (!rdna4gl_parse_shape(region, region_bytes, &w, &h)) return 0;
    if (w != s->width || h != s->height) {
        s->width = w;
        s->height = h;
        rdna4gl_retire_slot_buffers(s, retired, n_retired);
    }
    return 1;
}

static inline int rdna4gl_needs_buffer(const rdna4gl_slot *s, unsigned idx) {
    if (!s || !s->active || idx >= RDNA4GL_NUM_BUFFERS) return 0;
    if (s->width == 0u || s->height == 0u) return 0;
    return (!s->buffers[idx] || s->buf_width != s->width || s->buf_height != s->height) ? 1 : 0;
}

static inline void rdna4gl_install_buffers(rdna4gl_slot *s, void *b0, void *b1,
                                           void *retired[RDNA4GL_NUM_BUFFERS],
                                           unsigned *n_retired) {
    if (n_retired) *n_retired = 0u;
    if (!s || !s->active) return;
    rdna4gl_retire_slot_buffers(s, retired, n_retired);
    s->buffers[0] = b0;
    s->buffers[1] = b1;
    s->buf_width = s->width;
    s->buf_height = s->height;
}

static inline void *rdna4gl_control_buffer(const rdna4gl_table *t, uint64_t connect,
                                           uint32_t cmd, uint32_t arg) {
    if (!t || !rdna4gl_is_connect(connect)) return NULL;
    if (cmd != RDNA4GL_CTRL_GET_BUFFER || arg >= RDNA4GL_NUM_BUFFERS) return NULL;
    for (unsigned i = 0; i < RDNA4GL_MAX_SURFACES; i++) {
        if (t->slots[i].active && t->slots[i].connect == connect)
            return t->slots[i].buffers[arg];
    }
    return NULL;
}

#ifdef __cplusplus
}
#endif

#endif // RDNA4_GL_SURFACE_PURE_H
