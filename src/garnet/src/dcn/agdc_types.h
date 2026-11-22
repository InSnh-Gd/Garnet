// agdc_types.h — pure AGDC wire constants, reply builders, and WindowServer hold decision.
// Extracted cleanly into native DCN without any external display pipe guard dependency.
#ifndef RDNA4_DCN_AGDC_TYPES_H
#define RDNA4_DCN_AGDC_TYPES_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define RDNA4_AGDC_CMD_VENDOR_INFO    0x1u
#define RDNA4_AGDC_CMD_GPU_CAPABILITY 0x980u
#define RDNA4_AGDC_CMD_LINK_CONFIG    0x921u
#define RDNA4_AGDC_CMD_PIPELINE_CAPS  0x711u
#define RDNA4_AGDC_VENDOR_INFO_LEN    0x2cu
#define RDNA4_AGDC_GPU_CAP_LEN        0xdcu
#define RDNA4_AGDC_LINK_CONFIG_LEN    0xb0u
#define RDNA4_AGDC_PIPELINE_CAPS_LEN  0x196cu
#define RDNA4_AGDC_MAX_FB             7u

static inline void rdna4_le32(uint8_t *p, uint32_t off, uint32_t v)
{
    p[off] = (uint8_t)v;
    p[off + 1u] = (uint8_t)(v >> 8);
    p[off + 2u] = (uint8_t)(v >> 16);
    p[off + 3u] = (uint8_t)(v >> 24);
}

static inline void rdna4_le64(uint8_t *p, uint32_t off, uint64_t v)
{
    rdna4_le32(p, off, (uint32_t)v);
    rdna4_le32(p, off + 4u, (uint32_t)(v >> 32));
}

static inline uint32_t rdna4_agdc_rd32(const uint8_t *p, uint32_t off)
{
    return (uint32_t)p[off] | ((uint32_t)p[off + 1u] << 8) | ((uint32_t)p[off + 2u] << 16) |
           ((uint32_t)p[off + 3u] << 24);
}

static inline int rdna4_agdc_fill_vendor_info(uint8_t *out, size_t len)
{
    if (!out || len != RDNA4_AGDC_VENDOR_INFO_LEN) return 1;
    for (size_t i = 0; i < len; i++) out[i] = 0;
    rdna4_le32(out, 0x00u, 0x30000u);
    rdna4_le64(out, 0x04u, 0x444d41u);
    rdna4_le32(out, 0x24u, 0x1002u);
    rdna4_le32(out, 0x28u, 2u);
    return 0;
}

static inline int rdna4_agdc_fill_gpu_capability(uint8_t *out, size_t len, uint64_t pci, const uint64_t *fbs, uint32_t nfb)
{
    if (!out || len != RDNA4_AGDC_GPU_CAP_LEN || nfb == 0u || nfb > RDNA4_AGDC_MAX_FB || !fbs) return 1;
    for (size_t i = 0; i < len; i++) out[i] = 0;
    uint64_t mask = 0;
    for (uint32_t i = 0; i < nfb; i++) {
        mask |= (uint64_t)2u << i;
        rdna4_le64(out, 0x3cu + 8u * i, fbs[i]);
    }
    rdna4_le64(out, 0x00u, mask);
    rdna4_le32(out, 0x20u, nfb);
    rdna4_le32(out, 0x30u, nfb);
    rdna4_le64(out, 0x34u, pci);
    return 0;
}

#define RDNA4_AGDC_HBLANK     280u
#define RDNA4_AGDC_HSYNC_OFF  88u
#define RDNA4_AGDC_HSYNC_W    44u
#define RDNA4_AGDC_VBLANK     45u
#define RDNA4_AGDC_VSYNC_OFF  4u
#define RDNA4_AGDC_VSYNC_W    5u
#define RDNA4_AGDC_REFRESH_HZ 60u

static inline uint32_t rdna4_agdc_endpoint_dim(uint32_t ra, uint32_t live, uint32_t fallback)
{
    return ra ? ra : ((live && live <= 16384u) ? live : fallback);
}

typedef struct {
    uint32_t h_active, v_active, h_total, v_total;
    uint32_t h_front, h_sync, v_front, v_sync;
    uint64_t pixel_clock_hz;
} rdna4_agdc_raster;

typedef struct {
    uint32_t w, h, h_blank, h_sync_off, h_sync_w, v_blank, v_sync_off, v_sync_w;
    uint64_t pixel_clock;
    uint32_t live;
} rdna4_agdc_timing;

static inline void rdna4_agdc_timing_for(uint32_t w, uint32_t h, const rdna4_agdc_raster *r, rdna4_agdc_timing *t)
{
    if (!t) return;
    if (!w) w = 1920u;
    if (!h) h = 1080u;
    t->w = w;
    t->h = h;
    if (r && r->h_active == w && r->v_active == h && r->h_total > w && r->v_total > h && r->pixel_clock_hz &&
        r->h_sync && r->v_sync &&
        (uint64_t)r->h_front + r->h_sync <= (uint64_t)(r->h_total - w) &&
        (uint64_t)r->v_front + r->v_sync <= (uint64_t)(r->v_total - h)) {
        t->h_blank = r->h_total - w;
        t->h_sync_off = r->h_front;
        t->h_sync_w = r->h_sync;
        t->v_blank = r->v_total - h;
        t->v_sync_off = r->v_front;
        t->v_sync_w = r->v_sync;
        t->pixel_clock = r->pixel_clock_hz;
        t->live = 1u;
        return;
    }
    t->h_blank = RDNA4_AGDC_HBLANK;
    t->h_sync_off = RDNA4_AGDC_HSYNC_OFF;
    t->h_sync_w = RDNA4_AGDC_HSYNC_W;
    t->v_blank = RDNA4_AGDC_VBLANK;
    t->v_sync_off = RDNA4_AGDC_VSYNC_OFF;
    t->v_sync_w = RDNA4_AGDC_VSYNC_W;
    t->pixel_clock = ((uint64_t)w + (uint64_t)RDNA4_AGDC_HBLANK) * ((uint64_t)h + (uint64_t)RDNA4_AGDC_VBLANK) *
                     (uint64_t)RDNA4_AGDC_REFRESH_HZ;
    t->live = 0u;
}

typedef uint32_t (*rdna4_agdc_raster_fn)(void *ctx, rdna4_agdc_raster *r);

static inline void rdna4_agdc_link_timing(uint32_t lw, uint32_t lh, rdna4_agdc_raster_fn fn, void *ctx, rdna4_agdc_timing *lt)
{
    rdna4_agdc_raster lr = { 0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u, 0ull };
    const uint32_t ok = (!(lw == 1920u && lh == 1080u) && fn && fn(ctx, &lr)) ? 1u : 0u;
    rdna4_agdc_timing_for(lw, lh, ok ? &lr : 0, lt);
}

static inline int rdna4_agdc_fill_link_config_t(uint8_t *out, size_t len, const rdna4_agdc_timing *t)
{
    if (!out || !t || len != RDNA4_AGDC_LINK_CONFIG_LEN) return 1;
    for (size_t i = 0; i < len; i++) out[i] = 0;
    rdna4_le64(out, 0x2cu, t->pixel_clock);
    rdna4_le64(out, 0x34u, t->pixel_clock);
    rdna4_le64(out, 0x3cu, t->pixel_clock);
    rdna4_le32(out, 0x44u, t->w);
    rdna4_le32(out, 0x48u, t->h_blank);
    rdna4_le32(out, 0x4cu, t->h_sync_off);
    rdna4_le32(out, 0x50u, t->h_sync_w);
    rdna4_le32(out, 0x54u, t->h);
    rdna4_le32(out, 0x58u, t->v_blank);
    rdna4_le32(out, 0x5cu, t->v_sync_off);
    rdna4_le32(out, 0x60u, t->v_sync_w);
    return 0;
}

static inline int rdna4_agdc_fill_link_config(uint8_t *out, size_t len, uint32_t w, uint32_t h)
{
    if (!out || len != RDNA4_AGDC_LINK_CONFIG_LEN) return 1;
    rdna4_agdc_timing t;
    rdna4_agdc_timing_for(w, h, 0, &t);
    return rdna4_agdc_fill_link_config_t(out, len, &t);
}

#define RDNA4_AGDC_SCALER_TYPE 0x10u
#define RDNA4_AGDC_IB_TYPE     0x40u
#define RDNA4_AGDC_IB_STRIDE   0x32cu
#define RDNA4_AGDC_IB_LATENCY  0x3f800000u

static inline int rdna4_agdc_fill_pipeline_caps(uint8_t *out, size_t len, uint32_t w, uint32_t h)
{
    if (!out || len != RDNA4_AGDC_PIPELINE_CAPS_LEN) return 1;
    const uint32_t type = rdna4_agdc_rd32(out, 0x04u);
    for (size_t i = 0; i < len; i++) out[i] = 0;
    if (type == RDNA4_AGDC_IB_TYPE) {
        rdna4_le32(out, 0x08u, 1u);
        rdna4_le32(out, 0x0cu, 1u);
        rdna4_le32(out, 0x10u, RDNA4_AGDC_IB_LATENCY);
        rdna4_le32(out, 0x14u, RDNA4_AGDC_IB_LATENCY);
        rdna4_le32(out, 0x18u, h);
        rdna4_le32(out, 0x1cu, w);
        rdna4_le64(out, 0x20u, 1u);
        return 0;
    }
    if (type == RDNA4_AGDC_SCALER_TYPE) {
        rdna4_le32(out, 0x08u, 1u);
        rdna4_le32(out, 0x0cu, 0x00080004u);
        rdna4_le32(out, 0x28u, 1u);
        rdna4_le32(out, 0x30u, w);
        rdna4_le32(out, 0x34u, h);
        rdna4_le32(out, 0x38u, w);
        rdna4_le32(out, 0x3cu, h);
        rdna4_le32(out, 0x40u, 0x00050005u);
        rdna4_le32(out, 0x44u, 4u);
        rdna4_le32(out, 0x1b0u, 1u);
        rdna4_le32(out, 0x1b8u, w);
        rdna4_le32(out, 0x1bcu, h);
        rdna4_le32(out, 0x1c0u, w);
        rdna4_le32(out, 0x1c4u, h);
        rdna4_le32(out, 0x1c8u, 0x00050005u);
        rdna4_le32(out, 0x1ccu, 4u);
    }
    return 0;
}

#define RDNA4_AGDC_HOLD_TOKEN  "agdc-hold 0.0.322"
#define RDNA4_AGDC_HOLD_PROC   "WindowServer"
#define RDNA4_AGDC_HOLD_MAX_MS 5000u

static inline uint32_t rdna4_agdc_hold_ms(uint32_t armedMs, uint32_t fired, uint32_t cmd,
                                          const char *procName, uint32_t nameCap)
{
    static const char want[] = RDNA4_AGDC_HOLD_PROC;
    unsigned i = 0;
    if (armedMs == 0u || armedMs > RDNA4_AGDC_HOLD_MAX_MS) return 0u;
    if (fired) return 0u;
    if (cmd != RDNA4_AGDC_CMD_LINK_CONFIG) return 0u;
    if (!procName || nameCap == 0u) return 0u;
    for (; i < (unsigned)(sizeof(want) - 1u); i++) {
        if (i >= nameCap) return 0u;
        if (procName[i] != want[i]) return 0u;
    }
    if (i >= nameCap || procName[i] != '\0') return 0u;
    return armedMs;
}

#ifdef __cplusplus
}
#endif

#endif
