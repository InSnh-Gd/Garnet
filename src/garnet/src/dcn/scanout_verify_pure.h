// scanout_verify_pure.h — pure 16x16 scanout verify cell map and sampling helpers.
// Extracted cleanly into native DCN without any external display pipe guard dependency.
#ifndef RDNA4_DCN_SCANOUT_VERIFY_PURE_H
#define RDNA4_DCN_SCANOUT_VERIFY_PURE_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define RDNA4_DPG_VERIFY_CELLS     16u
#define RDNA4_DPG_VERIFY_SAMPLES   (RDNA4_DPG_VERIFY_CELLS * RDNA4_DPG_VERIFY_CELLS)
#define RDNA4_DPG_VERIFY_MAP_WORDS 4u
#define RDNA4_DPG_VERIFY_FIRST     8u
#define RDNA4_DPG_VERIFY_BUDGET    48u

static inline uint32_t rdna4_dpg_cell16(uint32_t x, uint32_t y, uint32_t w, uint32_t h)
{
    if (w == 0u || h == 0u) return 0u;
    uint64_t cx = ((uint64_t)x * RDNA4_DPG_VERIFY_CELLS) / w;
    uint64_t cy = ((uint64_t)y * RDNA4_DPG_VERIFY_CELLS) / h;
    if (cx >= RDNA4_DPG_VERIFY_CELLS) cx = RDNA4_DPG_VERIFY_CELLS - 1u;
    if (cy >= RDNA4_DPG_VERIFY_CELLS) cy = RDNA4_DPG_VERIFY_CELLS - 1u;
    return (uint32_t)(cy * RDNA4_DPG_VERIFY_CELLS + cx);
}

static inline void rdna4_dpg_cell16_centre(uint32_t cx, uint32_t cy, uint32_t w, uint32_t h, uint32_t *x, uint32_t *y)
{
    uint64_t px = (((uint64_t)(2u * cx + 1u) * w) / (2u * RDNA4_DPG_VERIFY_CELLS));
    uint64_t py = (((uint64_t)(2u * cy + 1u) * h) / (2u * RDNA4_DPG_VERIFY_CELLS));
    if (w && px >= w) px = w - 1u;
    if (h && py >= h) py = h - 1u;
    if (x) *x = (uint32_t)px;
    if (y) *y = (uint32_t)py;
}

static inline void rdna4_dpg_verify_map_set(uint64_t *map, uint32_t cell)
{
    if (map && cell < RDNA4_DPG_VERIFY_SAMPLES) map[cell >> 6] |= (uint64_t)1u << (cell & 63u);
}

static inline int rdna4_dpg_verify_map_get(const uint64_t *map, uint32_t cell)
{
    if (!map || cell >= RDNA4_DPG_VERIFY_SAMPLES) return 0;
    return (int)((map[cell >> 6] >> (cell & 63u)) & 1u);
}

static inline int rdna4_dpg_verify_due(uint64_t token, uint64_t lastToken, uint32_t runs)
{
    return runs < RDNA4_DPG_VERIFY_BUDGET && (runs == 0u || token != lastToken);
}

#ifdef __cplusplus
}
#endif

#endif
