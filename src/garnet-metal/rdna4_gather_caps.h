// Pure-C capability contract for Vulkan image-gather offsets.
//
// VkPhysicalDeviceFeatures::shaderImageGatherExtended controls the extended
// gather operands:
//   * SPIR-V Offset (runtime offsets) requires the feature.
//   * SPIR-V ConstOffsets (four independent constant offsets) requires it too.
//   * SPIR-V ConstOffset (one constant offset) is core gather and does not
//     require the feature, though its values still use the gather limits.
//
// minTexelGatherOffset/maxTexelGatherOffset apply to every component of all
// three operand forms. Image dimension restrictions such as Cube still apply;
// this capability object only describes the device's gathered-offset support.
#ifndef RDNA4_GATHER_CAPS_H
#define RDNA4_GATHER_CAPS_H

#include <stdint.h>

typedef struct {
    int supported;                         // exact shaderImageGatherExtended value after bool normalization
    int32_t min_texel_gather_offset;       // VkPhysicalDeviceLimits::minTexelGatherOffset
    uint32_t max_texel_gather_offset;      // VkPhysicalDeviceLimits::maxTexelGatherOffset
} rdna4_gather_caps;

typedef enum {
    RDNA4_GATHER_OFFSET_CONSTANT = 0,      // SPIR-V ConstOffset
    RDNA4_GATHER_OFFSET_RUNTIME = 1,       // SPIR-V Offset
    RDNA4_GATHER_OFFSET_CONST_OFFSETS = 2  // one entry of SPIR-V ConstOffsets
} rdna4_gather_offset_kind;

static inline rdna4_gather_caps rdna4_gather_caps_from_vulkan(
    int feature_supported, int32_t min_texel_gather_offset,
    uint32_t max_texel_gather_offset) {
    return (rdna4_gather_caps){
        .supported = feature_supported ? 1 : 0,
        .min_texel_gather_offset = min_texel_gather_offset,
        .max_texel_gather_offset = max_texel_gather_offset,
    };
}

static inline int rdna4_gather_caps_supported(const rdna4_gather_caps *caps) {
    return caps && caps->supported;
}

static inline int rdna4_gather_offset_requires_extended(
    rdna4_gather_offset_kind kind) {
    return kind == RDNA4_GATHER_OFFSET_RUNTIME ||
           kind == RDNA4_GATHER_OFFSET_CONST_OFFSETS;
}

static inline int rdna4_gather_offset_in_range(
    const rdna4_gather_caps *caps, int32_t x, int32_t y) {
    if (!caps) return 0;
    // Vulkan permits minTexelGatherOffset to be non-positive. A positive
    // minimum is still a valid (if unusual) range as long as max covers it.
    if (caps->min_texel_gather_offset > 0 &&
        (uint32_t)caps->min_texel_gather_offset > caps->max_texel_gather_offset)
        return 0;
    if (x < caps->min_texel_gather_offset || y < caps->min_texel_gather_offset)
        return 0;
    if (x > 0 && (uint32_t)x > caps->max_texel_gather_offset) return 0;
    if (y > 0 && (uint32_t)y > caps->max_texel_gather_offset) return 0;
    return 1;
}

// Checks one 2D offset. Constant offsets are core and remain admissible when
// the extended feature is absent; runtime and ConstOffsets fail closed unless
// the feature was both available and enabled.
static inline int rdna4_gather_offset_ok(
    const rdna4_gather_caps *caps, rdna4_gather_offset_kind kind,
    int32_t x, int32_t y) {
    if (!caps) return 0;
    if (kind != RDNA4_GATHER_OFFSET_CONSTANT &&
        kind != RDNA4_GATHER_OFFSET_RUNTIME &&
        kind != RDNA4_GATHER_OFFSET_CONST_OFFSETS)
        return 0;
    if (rdna4_gather_offset_requires_extended(kind) && !caps->supported)
        return 0;
    return rdna4_gather_offset_in_range(caps, x, y);
}

// ConstOffsets carries exactly four 2D offsets; all four must be in range.
static inline int rdna4_gather_const_offsets_ok(
    const rdna4_gather_caps *caps, const int32_t offsets[4][2]) {
    if (!caps || !caps->supported || !offsets) return 0;
    for (unsigned i = 0; i < 4; i++)
        if (!rdna4_gather_offset_in_range(caps, offsets[i][0], offsets[i][1]))
            return 0;
    return 1;
}

#endif
