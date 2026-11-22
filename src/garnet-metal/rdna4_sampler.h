#ifndef RDNA4_SAMPLER_H
#define RDNA4_SAMPLER_H

#include <math.h>
#include <stddef.h>
#include <vulkan/vulkan.h>

typedef struct {
    int minF, magF, mipF, as, at, ar, border, cmp;
    int unnorm;
    float lodMin, lodMax;
    unsigned anisotropy;
} RDNA4SampCfg;

/* Metal's comparison enum has the same values as VkCompareOp, including Never. */
static inline const char *rdna4_sampler_info(const RDNA4SampCfg *c, int comparison,
    int anisotropyEnabled, float maxAnisotropy, int mirrorClampEnabled,
    VkSamplerCreateInfo *out) {
    if (!c || !out || c->minF < 0 || c->minF > 1 || c->magF < 0 || c->magF > 1 ||
        c->mipF < 0 || c->mipF > 2 || c->border < 0 || c->border > 2 || c->cmp < 0 || c->cmp > 7)
        return "invalid sampler filter, border or comparison function";
    if (!isfinite(c->lodMin) || !isfinite(c->lodMax) || c->lodMin < 0 || c->lodMax < c->lodMin)
        return "sampler LOD clamps must be finite, nonnegative and ordered";
    if (!c->anisotropy || c->anisotropy > 16)
        return "sampler maxAnisotropy must be in 1..16";
    if (c->anisotropy > 1 && (!anisotropyEnabled || !isfinite(maxAnisotropy) || c->anisotropy > maxAnisotropy))
        return "sampler anisotropy exceeds the enabled backend limit";
    VkSamplerAddressMode address[3];
    int modes[3] = {c->as, c->at, c->ar}, zero = 0;
    for (unsigned i = 0; i < 3; ++i) switch (modes[i]) {
        case 0: address[i] = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE; break;
        case 1:
            if (!mirrorClampEnabled) return "sampler MirrorClampToEdge requires samplerMirrorClampToEdge";
            address[i] = VK_SAMPLER_ADDRESS_MODE_MIRROR_CLAMP_TO_EDGE;
            break;
        case 2: address[i] = VK_SAMPLER_ADDRESS_MODE_REPEAT; break;
        case 3: address[i] = VK_SAMPLER_ADDRESS_MODE_MIRRORED_REPEAT; break;
        case 4: address[i] = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER; zero = 1; break;
        case 5: address[i] = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER; break;
        default: return "unsupported sampler address mode";
    }
    if (c->unnorm && (c->minF != c->magF || c->mipF != 0 || c->anisotropy != 1 || comparison ||
        c->as == 1 || c->as == 2 || c->as == 3 ||
        c->at == 1 || c->at == 2 || c->at == 3 ||
        c->ar == 1 || c->ar == 2 || c->ar == 3))
        return "pixel-coordinate sampler requires equal filters, no mip/anisotropy/compare and clamp addressing";
    *out = (VkSamplerCreateInfo){ .sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,
        .magFilter = c->magF ? VK_FILTER_LINEAR : VK_FILTER_NEAREST,
        .minFilter = c->minF ? VK_FILTER_LINEAR : VK_FILTER_NEAREST,
        .mipmapMode = c->mipF == 2 ? VK_SAMPLER_MIPMAP_MODE_LINEAR : VK_SAMPLER_MIPMAP_MODE_NEAREST,
        .addressModeU = address[0], .addressModeV = address[1], .addressModeW = address[2],
        .mipLodBias = 0, .anisotropyEnable = c->anisotropy > 1, .maxAnisotropy = (float)c->anisotropy,
        .compareEnable = comparison != 0, .compareOp = (VkCompareOp)c->cmp,
        .minLod = c->mipF == 0 || c->unnorm ? 0 : c->lodMin,
        .maxLod = c->mipF == 0 || c->unnorm ? 0 : (c->lodMax > 1000.0f ? 1000.0f : c->lodMax),
        .borderColor = zero || c->border == 0 ? VK_BORDER_COLOR_FLOAT_TRANSPARENT_BLACK :
            c->border == 1 ? VK_BORDER_COLOR_FLOAT_OPAQUE_BLACK : VK_BORDER_COLOR_FLOAT_OPAQUE_WHITE,
        .unnormalizedCoordinates = c->unnorm != 0 };
    return NULL;
}

/*
 * ABI 8 lowers pixel-coordinate sampling through a normalized Vulkan sampler.
 * Validate the original Metal configuration first, then clone only the
 * coordinate mode so an illegal pixel sampler cannot become legal by lowering.
 */
static inline const char *rdna4_sampler_info_normalized(const RDNA4SampCfg *c,
    int comparison, int anisotropyEnabled, float maxAnisotropy,
    int mirrorClampEnabled, VkSamplerCreateInfo *out) {
    if (!c || !c->unnorm) {
        return rdna4_sampler_info(c, comparison, anisotropyEnabled,
            maxAnisotropy, mirrorClampEnabled, out);
    }
    VkSamplerCreateInfo validated;
    const char *invalid = rdna4_sampler_info(c, 0, anisotropyEnabled,
        maxAnisotropy, mirrorClampEnabled, &validated);
    if (invalid) return invalid;
    RDNA4SampCfg normalized = *c;
    normalized.unnorm = 0;
    return rdna4_sampler_info(&normalized, comparison, anisotropyEnabled,
        maxAnisotropy, mirrorClampEnabled, out);
}

#endif
