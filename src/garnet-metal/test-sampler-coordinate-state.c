#include <stdio.h>
#include <string.h>

#include "garnet_sampler.h"

static int failures;

#define CHECK(name, condition) do { \
    int ok_ = (condition); \
    printf("%s %s\n", ok_ ? "ok  " : "FAIL", name); \
    if (!ok_) ++failures; \
} while (0)

static RDNA4SampCfg pixel(void) {
    return (RDNA4SampCfg){
        .minF = 0, .magF = 0, .mipF = 0, .as = 0, .at = 0, .ar = 0,
        .border = 0, .cmp = 1, .unnorm = 1, .lodMin = 0, .lodMax = 0,
        .anisotropy = 1,
    };
}

static RDNA4SampCfg normalized(void) {
    return (RDNA4SampCfg){
        .minF = 1, .magF = 1, .mipF = 2, .as = 2, .at = 0, .ar = 3,
        .border = 0, .cmp = 1, .unnorm = 1, .lodMin = 0.25f,
        .lodMax = 12.5f, .anisotropy = 1,
    };
}

static const char *normalizedInfo(const RDNA4SampCfg *cfg, int comparison,
                                  VkSamplerCreateInfo *out) {
    return rdna4_sampler_info_normalized(cfg, comparison, 1, 16.0f, 0, out);
}

int main(void) {
    RDNA4SampCfg cfg = pixel();
    VkSamplerCreateInfo normal = {0}, comparison = {0}, legacy = {0};
    const char *error = normalizedInfo(&cfg, 0, &normal);
    CHECK("pixel ordinary is accepted by the normalized backend",
        error == NULL);
    CHECK("normalized backend disables Vulkan unnormalized coordinates",
        normal.unnormalizedCoordinates == VK_FALSE);
    CHECK("normalized backend preserves the pixel filter",
        normal.minFilter == VK_FILTER_NEAREST &&
        normal.magFilter == VK_FILTER_NEAREST);
    CHECK("normalized backend keeps mipLodBias zero",
        normal.mipLodBias == 0.0f);

    error = normalizedInfo(&cfg, 1, &comparison);
    CHECK("pixel comparison is accepted only by the normalized backend",
        error == NULL && comparison.compareEnable == VK_TRUE &&
        comparison.compareOp == VK_COMPARE_OP_LESS &&
        comparison.unnormalizedCoordinates == VK_FALSE);
    CHECK("normalized comparison keeps mipLodBias zero",
        comparison.mipLodBias == 0.0f);

    error = rdna4_sampler_info(&cfg, 0, 1, 16.0f, 0, &legacy);
    CHECK("legacy pixel ordinary remains unnormalized",
        error == NULL && legacy.unnormalizedCoordinates == VK_TRUE);
    CHECK("legacy pixel comparison remains rejected",
        rdna4_sampler_info(&cfg, 1, 1, 16.0f, 0, &legacy) != NULL);

    cfg = pixel();
    cfg.as = 2;
    CHECK("normalized backend still rejects pixel repeat",
        normalizedInfo(&cfg, 0, &normal) != NULL);
    cfg = pixel();
    cfg.as = 1;
    CHECK("normalized backend still rejects pixel mirror-clamp",
        normalizedInfo(&cfg, 0, &normal) != NULL);
    cfg = pixel();
    cfg.magF = 1;
    CHECK("normalized backend still rejects unequal pixel filters",
        normalizedInfo(&cfg, 0, &normal) != NULL);
    cfg = pixel();
    cfg.mipF = 1;
    CHECK("normalized backend still rejects pixel mip filtering",
        normalizedInfo(&cfg, 0, &normal) != NULL);
    cfg = pixel();
    cfg.anisotropy = 2;
    CHECK("normalized backend still rejects pixel anisotropy",
        normalizedInfo(&cfg, 0, &normal) != NULL);

    cfg = normalized();
    cfg.unnorm = 0;
    error = normalizedInfo(&cfg, 0, &normal);
    CHECK("source-normalized ordinary keeps normalized coordinates",
        error == NULL && normal.unnormalizedCoordinates == VK_FALSE);
    error = normalizedInfo(&cfg, 1, &comparison);
    CHECK("source-normalized comparison enables comparison",
        error == NULL && comparison.compareEnable == VK_TRUE &&
        comparison.compareOp == VK_COMPARE_OP_LESS);

    {
        VkSamplerCreateInfo untouched;
        memset(&untouched, 0x5a, sizeof untouched);
        VkSamplerCreateInfo before = untouched;
        cfg = pixel();
        cfg.at = 3;
        CHECK("failed pixel validation leaves output untouched",
            normalizedInfo(&cfg, 1, &untouched) != NULL &&
            memcmp(&before, &untouched, sizeof untouched) == 0);
    }

    if (failures) {
        printf("sampler-coordinate-state: %d failure(s)\n", failures);
        return 1;
    }
    puts("sampler-coordinate-state: pixel validation, normalized backend, "
         "comparison and legacy separation PASS");
    return 0;
}
