#pragma once
/* garnet_gpl.h - pure-C policy and key helpers for Graphics Pipeline Library (GPL) fast-link
 * and Vulkan 1.3 / EDS3 dynamic states in GarnetMetal. Header-only, testable on host without Metal/Vulkan. */
#ifndef GARNET_GPL_H
#define GARNET_GPL_H

#include <stdint.h>

#define GARNET_NOGPL_FILE     "/private/tmp/garnet-nogpl"

/* Vulkan VkPrimitiveTopology numeric constants (validated statically in garnet_pipelines.m). */
#define GARNET_VK_TOPO_POINT_LIST     0u
#define GARNET_VK_TOPO_LINE_LIST      1u
#define GARNET_VK_TOPO_LINE_STRIP     2u
#define GARNET_VK_TOPO_TRIANGLE_LIST  3u
#define GARNET_VK_TOPO_TRIANGLE_STRIP 4u

/* RADV exposes dynamicPrimitiveTopologyUnrestricted = false: LIST <-> STRIP within the same
 * topology class (LINE or TRIANGLE) shares one VkPipeline via vkCmdSetPrimitiveTopology +
 * vkCmdSetPrimitiveRestartEnable; crossing classes (TRIANGLE -> LINE -> POINT) fast-links a tiny
 * pre-rasterization library against the cached fragment-shader library. */
static inline uint32_t garnet_gpl_topo_class(uint32_t vk_topo) {
    switch (vk_topo) {
        case GARNET_VK_TOPO_POINT_LIST:
            return GARNET_VK_TOPO_POINT_LIST;
        case GARNET_VK_TOPO_LINE_LIST:
        case GARNET_VK_TOPO_LINE_STRIP:
            return GARNET_VK_TOPO_LINE_LIST;
        case GARNET_VK_TOPO_TRIANGLE_LIST:
        case GARNET_VK_TOPO_TRIANGLE_STRIP:
            return GARNET_VK_TOPO_TRIANGLE_LIST;
        default:
            return vk_topo;
    }
}

static inline int garnet_gpl_is_strip_topo(uint32_t vk_topo) {
    return vk_topo == GARNET_VK_TOPO_LINE_STRIP || vk_topo == GARNET_VK_TOPO_TRIANGLE_STRIP;
}

/* Decide whether a render pipeline state qualifies for GPL 3-library fast-link.
 * Excludes tessellation, ICB, fallback placeholders, rasterizer-off / vertex-only,
 * GPU argument-buffer pipelines (sets 2..4), and explicit binary-archive cache scopes. */
static inline int garnet_gpl_eligible(int gpl_enabled, int has_frag, int is_tess, int is_icb,
                                      int is_fallback, int rast_off, int arg_tables_active,
                                      int use_ba_cache, int kill_switch) {
    return (gpl_enabled && has_frag && !is_tess && !is_icb && !is_fallback &&
            !rast_off && !arg_tables_active && !use_ba_cache && !kill_switch) ? 1 : 0;
}

/* Variant cache key for RDNA4RenderPipelineState._variants / _fastKey.
 * When GPL is active (and programmable sample locations are not in use), dynamic Vulkan 1.3 + EDS3
 * states fold out of the variant key so cull/front/depth/stencil/fillMode/depthClamp/LIST<->STRIP
 * changes never create new pipelines or re-bind pipelines. */
static inline uint64_t garnet_gpl_variant_key(uint32_t topo, uint32_t cull, uint32_t front,
                                              uint32_t dk, uint32_t rflags,
                                              int use_gpl, int eds3_poly, int eds3_clamp) {
    const uint32_t rf = rflags & 0x7Fu;
    if (!use_gpl || (rf & 4u)) {
        return (uint64_t)topo | ((uint64_t)cull << 8) | ((uint64_t)front << 16) |
               ((uint64_t)dk << 24) | ((uint64_t)rf << 56);
    }
    const uint32_t tc = garnet_gpl_topo_class(topo);
    uint32_t krf = rf;
    if (eds3_poly) krf &= ~1u;
    if (eds3_clamp) krf &= ~2u;
    return (uint64_t)tc | ((uint64_t)krf << 56);
}

/* Pre-rasterization library key within a GPL-enabled PSO. */
static inline uint64_t garnet_gpl_vslib_key(uint32_t topo, uint32_t rflags,
                                            int eds3_poly, int eds3_clamp) {
    const uint32_t tc = garnet_gpl_topo_class(topo);
    uint32_t krf = rflags & 0x7Bu; /* exclude bit 2 (sampleLoc falls back to monolithic) */
    if (eds3_poly) krf &= ~1u;
    if (eds3_clamp) krf &= ~2u;
    return (uint64_t)tc | ((uint64_t)krf << 8);
}

#endif
