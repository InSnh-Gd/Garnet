#pragma GCC diagnostic ignored "-Wunused-function"
#include "rdna4_spvrefl.h"
#include "rdna4_argument_descriptor_scan.h"
#include <assert.h>

static unsigned checks;
#define CHECK(condition) do { ++checks; assert(condition); } while (0)

static VkPhysicalDeviceLimits core_limits(void) {
    return (VkPhysicalDeviceLimits){
        .maxPerStageDescriptorSamplers = 100,
        .maxPerStageDescriptorUniformBuffers = 100,
        .maxPerStageDescriptorStorageBuffers = 100,
        .maxPerStageDescriptorSampledImages = 100,
        .maxPerStageDescriptorStorageImages = 100,
        .maxPerStageDescriptorInputAttachments = 100,
        .maxPerStageResources = 1000,
        .maxDescriptorSetSamplers = 100,
        .maxDescriptorSetUniformBuffers = 100,
        .maxDescriptorSetUniformBuffersDynamic = 100,
        .maxDescriptorSetStorageBuffers = 100,
        .maxDescriptorSetStorageBuffersDynamic = 100,
        .maxDescriptorSetSampledImages = 100,
        .maxDescriptorSetStorageImages = 100,
        .maxDescriptorSetInputAttachments = 100,
        .maxFragmentCombinedOutputResources = 1000,
        .maxColorAttachments = 8,
        .maxFragmentOutputAttachments = 8,
        .maxFragmentDualSrcAttachments = 0,
    };
}

int main(void) {
    RDNA4sRefl *v = calloc(1, sizeof *v), *f = calloc(1, sizeof *f);
    CHECK(v && f);
    VkPhysicalDeviceLimits core = core_limits();
    VkPhysicalDeviceVulkan12Properties uab = {0};
    rdna4_argument_descriptor_limits limits = rdna4_argument_descriptor_device_limits(&core, &uab, 0);
    const VkDescriptorType types[] = {VK_DESCRIPTOR_TYPE_SAMPLER,
        VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,
        VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER,
        VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
        VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
        VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC, VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT};
    rdna4_argument_descriptor_usage usage;
    for (unsigned type = 0; type < sizeof types / sizeof types[0]; ++type) {
        memset(v, 0, sizeof *v); memset(f, 0, sizeof *f);
        v->bind[0][7] = (RDNA4sBind){1, types[type], 100, 0};
        CHECK(rdna4_argument_descriptor_scan(v, NULL, 0, &usage));
        CHECK(rdna4_argument_capacity_descriptor_budget(&limits, &usage, NULL) == RDNA4_ARGUMENT_DESCRIPTOR_BUDGET_OK);
        v->bind[0][7].count = 101;
        CHECK(rdna4_argument_descriptor_scan(v, NULL, 0, &usage));
        CHECK(rdna4_argument_capacity_descriptor_budget(&limits, &usage, NULL) != RDNA4_ARGUMENT_DESCRIPTOR_BUDGET_OK);
        v->bind[0][7].count = 50;
        f->bind[1][9] = (RDNA4sBind){1, types[type], 50, 0};
        CHECK(rdna4_argument_descriptor_scan(v, f, 0, &usage));
        CHECK(rdna4_argument_capacity_descriptor_budget(&limits, &usage, NULL) == RDNA4_ARGUMENT_DESCRIPTOR_BUDGET_OK);
        f->bind[1][9].count = 51;
        CHECK(rdna4_argument_descriptor_scan(v, f, 0, &usage));
        CHECK(rdna4_argument_capacity_descriptor_budget(&limits, &usage, NULL) == RDNA4_ARGUMENT_DESCRIPTOR_BUDGET_PIPELINE_OVER_LIMIT);
    }
    uab = (VkPhysicalDeviceVulkan12Properties){
        .maxPerStageDescriptorUpdateAfterBindSamplers = 30,
        .maxPerStageDescriptorUpdateAfterBindUniformBuffers = 30,
        .maxPerStageDescriptorUpdateAfterBindStorageBuffers = 30,
        .maxPerStageDescriptorUpdateAfterBindSampledImages = 30,
        .maxPerStageDescriptorUpdateAfterBindStorageImages = 30,
        .maxPerStageDescriptorUpdateAfterBindInputAttachments = 30,
        .maxPerStageUpdateAfterBindResources = 1000,
        .maxDescriptorSetUpdateAfterBindSamplers = 30,
        .maxDescriptorSetUpdateAfterBindUniformBuffers = 30,
        .maxDescriptorSetUpdateAfterBindUniformBuffersDynamic = 30,
        .maxDescriptorSetUpdateAfterBindStorageBuffers = 30,
        .maxDescriptorSetUpdateAfterBindStorageBuffersDynamic = 30,
        .maxDescriptorSetUpdateAfterBindSampledImages = 30,
        .maxDescriptorSetUpdateAfterBindStorageImages = 30,
        .maxDescriptorSetUpdateAfterBindInputAttachments = 30,
    };
    limits = rdna4_argument_descriptor_device_limits(&core, &uab, 1);
    for (unsigned type = 0; type < sizeof types / sizeof types[0]; ++type) {
        memset(v, 0, sizeof *v); memset(f, 0, sizeof *f);
        v->bind[0][7] = (RDNA4sBind){1, types[type], 30, 0};
        CHECK(rdna4_argument_descriptor_scan(v, NULL, 0, &usage));
        CHECK(rdna4_argument_capacity_descriptor_budget(&limits, &usage, NULL) == RDNA4_ARGUMENT_DESCRIPTOR_BUDGET_OK);
        v->bind[0][7].count = 31;
        CHECK(rdna4_argument_descriptor_scan(v, NULL, 0, &usage));
        CHECK(rdna4_argument_capacity_descriptor_budget(&limits, &usage, NULL) != RDNA4_ARGUMENT_DESCRIPTOR_BUDGET_OK);
        v->bind[0][7].count = 15;
        f->bind[1][9] = (RDNA4sBind){1, types[type], 15, 0};
        CHECK(rdna4_argument_descriptor_scan(v, f, 0, &usage));
        CHECK(rdna4_argument_capacity_descriptor_budget(&limits, &usage, NULL) == RDNA4_ARGUMENT_DESCRIPTOR_BUDGET_OK);
        f->bind[1][9].count = 16;
        CHECK(rdna4_argument_descriptor_scan(v, f, 0, &usage));
        CHECK(rdna4_argument_capacity_descriptor_budget(&limits, &usage, NULL) == RDNA4_ARGUMENT_DESCRIPTOR_BUDGET_PIPELINE_OVER_LIMIT);
    }
    limits = rdna4_argument_descriptor_device_limits(&core, &uab, 0);
    memset(v, 0, sizeof *v); memset(f, 0, sizeof *f);
    v->bind[0][0] = (RDNA4sBind){1, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC, 3, 0};
    CHECK(rdna4_argument_descriptor_scan(v, NULL, 0, &usage));
    limits.max_pipeline[RDNA4_ARGUMENT_DESCRIPTOR_GROUP_UNIFORM_BUFFERS_DYNAMIC] = 3;
    CHECK(rdna4_argument_capacity_descriptor_budget(&limits, &usage, NULL) == RDNA4_ARGUMENT_DESCRIPTOR_BUDGET_OK);
    v->bind[0][0].count = 4;
    CHECK(rdna4_argument_descriptor_scan(v, NULL, 0, &usage));
    CHECK(rdna4_argument_capacity_descriptor_budget(&limits, &usage, NULL) == RDNA4_ARGUMENT_DESCRIPTOR_BUDGET_PIPELINE_OVER_LIMIT);

    /* Ordinary texels join their image group; shared ABI9 binding1 is UAB only. */
    memset(v, 0, sizeof *v); memset(f, 0, sizeof *f);
    v->bind[0][0] = (RDNA4sBind){1, VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER, 2, 0};
    f->bind[1][0] = (RDNA4sBind){1, VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER, 1, 0};
    CHECK(rdna4_argument_descriptor_scan(v, f, 2, &usage));
    const uint32_t stages[3] = {3, 3, 3}, texel[2] = {3, 3}, rows[3] = {10, 11, 12};
    const uint32_t texel_rows[2] = {RDNA4NS_TEXEL_SHAPES, RDNA4NS_TEXEL_SHAPES};
    rdna4_argument_descriptor_usage all;
    CHECK(rdna4_argument_descriptor_tables(&usage, stages, texel, rows, texel_rows, &all));
    CHECK(all.pipeline.sampled_image == 10 && all.pipeline.storage_image == 11 && all.pipeline.sampler == 12);
    CHECK(all.pipeline.uniform_texel_buffer == 2 + RDNA4NS_TEXEL_SHAPES && all.pipeline.storage_texel_buffer == 1 + RDNA4NS_TEXEL_SHAPES);
    CHECK(all.per_stage[0].uniform_texel_buffer == 2 + RDNA4NS_TEXEL_SHAPES && all.per_stage[1].uniform_texel_buffer == RDNA4NS_TEXEL_SHAPES);
    CHECK(all.per_stage[0].sampler == 12 && all.per_stage[1].sampler == 12 && usage.pipeline.sampler == 0);
    rdna4_argument_descriptor_usage before = all;
    usage.pipeline.sampled_image = UINT64_MAX;
    CHECK(!rdna4_argument_descriptor_tables(&usage, stages, texel, rows, texel_rows, &all));
    CHECK(!memcmp(&before, &all, sizeof all));

    memset(v, 0, sizeof *v); memset(f, 0, sizeof *f);
    v->bind[0][160] = (RDNA4sBind){1, VK_DESCRIPTOR_TYPE_SAMPLER, 2, 0};
    v->bind[0][736] = (RDNA4sBind){1, VK_DESCRIPTOR_TYPE_SAMPLER, 2, 0};
    CHECK(rdna4_argument_descriptor_scan(v, NULL, 0, &usage));
    const uint32_t sampler_stages[3] = {0, 0, 1}, no_texel[2] = {0, 0}, sampler_rows[3] = {0, 0, 6};
    const uint32_t no_texel_rows[2] = {0, 0};
    CHECK(rdna4_argument_descriptor_tables(&usage, sampler_stages, no_texel, sampler_rows, no_texel_rows, &all));
    CHECK(usage.pipeline.sampler == 4 && all.pipeline.sampler == 10 && all.per_stage[0].sampler == 10);
    memset(v, 0, sizeof *v); memset(f, 0, sizeof *f);
    f->bind[1][0] = (RDNA4sBind){1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 3, 0};
    f->outmask = 1u << 7;
    CHECK(rdna4_argument_descriptor_scan(v, f, 8, &usage));
    CHECK(usage.fragment_output_attachments == 8);
    limits = rdna4_argument_descriptor_device_limits(&core, &uab, 0);
    limits.max_per_stage_resources = 11;
    limits.max_fragment_combined_output_resources = 11;
    CHECK(rdna4_argument_capacity_descriptor_budget(&limits, &usage, NULL) == RDNA4_ARGUMENT_DESCRIPTOR_BUDGET_OK);
    f->bind[1][0].count = 4;
    CHECK(rdna4_argument_descriptor_scan(v, f, 8, &usage));
    CHECK(rdna4_argument_capacity_descriptor_budget(&limits, &usage, NULL) == RDNA4_ARGUMENT_DESCRIPTOR_BUDGET_RESOURCE_OVER_LIMIT);
    limits.max_per_stage_resources = 100;
    CHECK(rdna4_argument_capacity_descriptor_budget(&limits, &usage, NULL) == RDNA4_ARGUMENT_DESCRIPTOR_BUDGET_FRAGMENT_COMBINED_OUTPUT_OVER_LIMIT);
    f->bind[1][0].count = 3;
    CHECK(rdna4_argument_descriptor_scan(v, f, 9, &usage));
    limits.max_fragment_combined_output_resources = 100;
    CHECK(rdna4_argument_capacity_descriptor_budget(&limits, &usage, NULL) == RDNA4_ARGUMENT_DESCRIPTOR_BUDGET_COLOR_ATTACHMENT_OVER_LIMIT);
    f->outmask = 1u << 8;
    CHECK(rdna4_argument_descriptor_scan(v, f, 8, &usage));
    CHECK(rdna4_argument_capacity_descriptor_budget(&limits, &usage, NULL) == RDNA4_ARGUMENT_DESCRIPTOR_BUDGET_FRAGMENT_OUTPUT_OVER_LIMIT);
    f->bind[1][0].runtime = 1;
    before = usage;
    CHECK(!rdna4_argument_descriptor_scan(v, f, 0, &usage));
    CHECK(!memcmp(&before, &usage, sizeof usage));
    free(v); free(f);
    printf("ARGUMENT_DESCRIPTOR_SCAN_PASS checks=%u production_reflection=1 shared_union=1 abi9_texel=1 overflow_rollback=1 gpu_execution=0\n", checks);
    return 0;
}
