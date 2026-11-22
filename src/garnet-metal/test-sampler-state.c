#include <stdio.h>
#include <string.h>

#include "rdna4_sampler.h"

static int failures;

#define CHECK(name, condition) do { \
    int ok_ = (condition); \
    printf("%s %s\n", ok_ ? "ok  " : "FAIL", name); \
    if (!ok_) ++failures; \
} while (0)

static RDNA4SampCfg valid(void) {
    return (RDNA4SampCfg){
        .minF = 1, .magF = 1, .mipF = 2, .as = 2, .at = 0, .ar = 3,
        .border = 2, .cmp = 0, .unnorm = 0, .lodMin = 0.25f, .lodMax = 12.5f,
        .anisotropy = 1,
    };
}

static const char *samplerInfo(const RDNA4SampCfg *cfg, int comparison,
    int anisotropyEnabled, float maxAnisotropy, VkSamplerCreateInfo *out) {
    return rdna4_sampler_info(cfg, comparison, anisotropyEnabled, maxAnisotropy,
        0, out);
}

int main(void) {
    RDNA4SampCfg cfg = valid();
    VkSamplerCreateInfo normal, compare;
    const char *error = samplerInfo(&cfg, 0, 1, 16.0f, &normal);
    CHECK("normal sampler is accepted", error == NULL);
    error = samplerInfo(&cfg, 1, 1, 16.0f, &compare);
    CHECK("comparison sampler is accepted", error == NULL);
    CHECK("normal sampler disables comparison", normal.compareEnable == VK_FALSE);
    CHECK("comparison sampler enables comparison even for Never",
        compare.compareEnable == VK_TRUE && compare.compareOp == VK_COMPARE_OP_NEVER);
    CHECK("normal and comparison preserve shared sampler state",
        normal.magFilter == compare.magFilter && normal.minFilter == compare.minFilter &&
        normal.mipmapMode == compare.mipmapMode &&
        normal.addressModeU == compare.addressModeU &&
        normal.addressModeV == compare.addressModeV &&
        normal.addressModeW == compare.addressModeW &&
        normal.borderColor == compare.borderColor &&
        normal.minLod == compare.minLod && normal.maxLod == compare.maxLod &&
        normal.unnormalizedCoordinates == compare.unnormalizedCoordinates);
    CHECK("sampler mip bias stays zero",
        normal.mipLodBias == 0.0f && compare.mipLodBias == 0.0f);
    CHECK("LOD clamps and Metal address enums map exactly",
        normal.minLod == 0.25f && normal.maxLod == 12.5f &&
        normal.addressModeU == VK_SAMPLER_ADDRESS_MODE_REPEAT &&
        normal.addressModeV == VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE &&
        normal.addressModeW == VK_SAMPLER_ADDRESS_MODE_MIRRORED_REPEAT &&
        normal.borderColor == VK_BORDER_COLOR_FLOAT_OPAQUE_WHITE);

    for (int function = 0; function <= 7; ++function) {
        cfg.cmp = function;
        error = samplerInfo(&cfg, 1, 1, 16.0f, &compare);
        CHECK("comparison function Never..Always maps exactly",
            error == NULL && compare.compareEnable == VK_TRUE &&
            compare.compareOp == (VkCompareOp)function);
    }

    cfg = valid();
    cfg.anisotropy = 4;
    error = samplerInfo(&cfg, 0, 1, 8.0f, &normal);
    CHECK("supported anisotropy is enabled with the same value",
        error == NULL && normal.anisotropyEnable == VK_TRUE &&
        normal.maxAnisotropy == 4.0f);
    CHECK("unsupported anisotropy is rejected",
        samplerInfo(&cfg, 0, 0, 16.0f, &normal) != NULL);
    CHECK("anisotropy above the backend limit is rejected",
        samplerInfo(&cfg, 0, 1, 2.0f, &normal) != NULL);

    cfg = valid();
    cfg.as = cfg.at = cfg.ar = 1;   // MTLSamplerAddressModeMirrorClampToEdge
    error = rdna4_sampler_info(&cfg, 0, 0, 16.0f, 1, &normal);
    CHECK("MirrorClampToEdge maps when the feature is enabled",
        error == NULL &&
        normal.addressModeU == VK_SAMPLER_ADDRESS_MODE_MIRROR_CLAMP_TO_EDGE &&
        normal.addressModeV == VK_SAMPLER_ADDRESS_MODE_MIRROR_CLAMP_TO_EDGE &&
        normal.addressModeW == VK_SAMPLER_ADDRESS_MODE_MIRROR_CLAMP_TO_EDGE);
    cfg.minF = cfg.magF = 0;
    cfg.mipF = 0;
    cfg.unnorm = 1;
    cfg.as = 1;
    cfg.at = cfg.ar = 0;
    {
        VkSamplerCreateInfo untouched;
        memset(&untouched, 0x5a, sizeof untouched);
        VkSamplerCreateInfo before = untouched;
        error = rdna4_sampler_info(&cfg, 0, 0, 16.0f, 1, &untouched);
        CHECK("pixel-coordinate sampling rejects MirrorClampToEdge",
            error != NULL && memcmp(&before, &untouched, sizeof untouched) == 0);
    }

    static const struct {
        const char *name;
        RDNA4SampCfg cfg;
        int comparison;
        int anisotropyEnabled;
        float maxAnisotropy;
    } invalid[] = {
        { "negative filter", { .minF = -1, .magF = 0, .mipF = 0, .as = 0, .at = 0, .ar = 0, .border = 0, .cmp = 0, .anisotropy = 1 }, 0, 0, 16 },
        { "unknown filter", { .minF = 2, .magF = 0, .mipF = 0, .as = 0, .at = 0, .ar = 0, .border = 0, .cmp = 0, .anisotropy = 1 }, 0, 0, 16 },
        { "unknown mip filter", { .minF = 0, .magF = 0, .mipF = 3, .as = 0, .at = 0, .ar = 0, .border = 0, .cmp = 0, .anisotropy = 1 }, 0, 0, 16 },
        { "unknown border", { .minF = 0, .magF = 0, .mipF = 0, .as = 0, .at = 0, .ar = 0, .border = 3, .cmp = 0, .anisotropy = 1 }, 0, 0, 16 },
        { "unknown comparison", { .minF = 0, .magF = 0, .mipF = 0, .as = 0, .at = 0, .ar = 0, .border = 0, .cmp = 8, .anisotropy = 1 }, 0, 0, 16 },
        { "negative LOD", { .minF = 0, .magF = 0, .mipF = 0, .as = 0, .at = 0, .ar = 0, .border = 0, .cmp = 0, .lodMin = -1, .lodMax = 1, .anisotropy = 1 }, 0, 0, 16 },
        { "reversed LOD", { .minF = 0, .magF = 0, .mipF = 0, .as = 0, .at = 0, .ar = 0, .border = 0, .cmp = 0, .lodMin = 2, .lodMax = 1, .anisotropy = 1 }, 0, 0, 16 },
        { "zero anisotropy", { .minF = 0, .magF = 0, .mipF = 0, .as = 0, .at = 0, .ar = 0, .border = 0, .cmp = 0, .anisotropy = 0 }, 0, 0, 16 },
        { "excess anisotropy", { .minF = 0, .magF = 0, .mipF = 0, .as = 0, .at = 0, .ar = 0, .border = 0, .cmp = 0, .anisotropy = 17 }, 0, 1, 16 },
        { "MirrorClampToEdge without feature", { .minF = 0, .magF = 0, .mipF = 0, .as = 1, .at = 0, .ar = 0, .border = 0, .cmp = 0, .anisotropy = 1 }, 0, 0, 16 },
        { "unnormalized different filters", { .minF = 0, .magF = 1, .mipF = 0, .as = 0, .at = 0, .ar = 0, .border = 0, .cmp = 0, .unnorm = 1, .anisotropy = 1 }, 0, 0, 16 },
        { "unnormalized mip filter", { .minF = 0, .magF = 0, .mipF = 1, .as = 0, .at = 0, .ar = 0, .border = 0, .cmp = 0, .unnorm = 1, .anisotropy = 1 }, 0, 0, 16 },
        { "unnormalized repeat", { .minF = 0, .magF = 0, .mipF = 0, .as = 2, .at = 0, .ar = 0, .border = 0, .cmp = 0, .unnorm = 1, .anisotropy = 1 }, 0, 0, 16 },
        { "unnormalized anisotropy", { .minF = 0, .magF = 0, .mipF = 0, .as = 0, .at = 0, .ar = 0, .border = 0, .cmp = 0, .unnorm = 1, .anisotropy = 2 }, 0, 1, 16 },
        { "backend rejects unnormalized comparison (Vulkan VUID)", { .minF = 0, .magF = 0, .mipF = 0, .as = 0, .at = 0, .ar = 0, .border = 0, .cmp = 1, .unnorm = 1, .anisotropy = 1 }, 1, 0, 16 },
    };
    for (size_t i = 0; i < sizeof invalid / sizeof invalid[0]; ++i) {
        VkSamplerCreateInfo untouched;
        memset(&untouched, 0x5a, sizeof untouched);
        VkSamplerCreateInfo before = untouched;
        error = samplerInfo(&invalid[i].cfg, invalid[i].comparison,
            invalid[i].anisotropyEnabled, invalid[i].maxAnisotropy, &untouched);
        CHECK(invalid[i].name, error != NULL &&
            memcmp(&before, &untouched, sizeof untouched) == 0);
    }
    CHECK("null configuration is rejected",
        samplerInfo(NULL, 0, 0, 16.0f, &normal) != NULL);

    if (failures) {
        printf("sampler-state: %d failure(s)\n", failures);
        return 1;
    }
    puts("sampler-state: normal/compare representations, Never..Always, state mapping, "
         "anisotropy, LOD and atomic rejection PASS");
    return 0;
}
