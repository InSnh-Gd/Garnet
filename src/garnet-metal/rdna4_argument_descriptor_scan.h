#ifndef RDNA4_ARGUMENT_DESCRIPTOR_SCAN_H
#define RDNA4_ARGUMENT_DESCRIPTOR_SCAN_H

#include "rdna4_argument_capacity.h"

/* Include after rdna4_spvrefl.h: these are the actual layout reflection rows. */
static inline int rdna4_argument_descriptor_add(
    rdna4_argument_descriptor_counts *counts, VkDescriptorType type,
    uint64_t count) {
    uint64_t *value;
    switch (type) {
    case VK_DESCRIPTOR_TYPE_SAMPLER: value = &counts->sampler; break;
    case VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER: value = &counts->combined_image_sampler; break;
    case VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE: value = &counts->sampled_image; break;
    case VK_DESCRIPTOR_TYPE_STORAGE_IMAGE: value = &counts->storage_image; break;
    case VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER: value = &counts->uniform_texel_buffer; break;
    case VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER: value = &counts->storage_texel_buffer; break;
    case VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER: value = &counts->uniform_buffer; break;
    case VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC: value = &counts->uniform_buffer_dynamic; break;
    case VK_DESCRIPTOR_TYPE_STORAGE_BUFFER: value = &counts->storage_buffer; break;
    case VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC: value = &counts->storage_buffer_dynamic; break;
    case VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT: value = &counts->input_attachment; break;
#ifdef VK_KHR_acceleration_structure
    case VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR: return 1;
#endif
    default: return 0;
    }
    return rdna4_argument_capacity_add_u64(*value, count, value);
}

static inline int rdna4_argument_descriptor_scan(
    const RDNA4sRefl *vertex, const RDNA4sRefl *fragment,
    uint64_t color_attachments, rdna4_argument_descriptor_usage *usage) {
    if (!usage) return 0;
    rdna4_argument_descriptor_usage result = {0};
    const RDNA4sRefl *stages[] = {vertex, fragment};
    for (unsigned stage = 0; stage < 2; ++stage) {
        const RDNA4sRefl *reflection = stages[stage];
        if (!reflection) continue;
        for (unsigned binding = 0; binding < RDNA4S_MAXBIND; ++binding) {
            const RDNA4sBind *slot = &reflection->bind[stage][binding];
            if (!slot->used) continue;
            if (slot->runtime || !slot->count ||
                !rdna4_argument_descriptor_add(&result.per_stage[stage], slot->type, slot->count) ||
                !rdna4_argument_descriptor_add(&result.pipeline, slot->type, slot->count))
                return 0;
        }
    }
    result.fragment_color_attachments = color_attachments;
    if (fragment)
        for (uint32_t mask = fragment->outmask; mask; mask >>= 1)
            ++result.fragment_output_attachments;
    *usage = result;
    return 1;
}

/* Shared layouts contribute once to the pipeline, once to every visible stage. */
static inline int rdna4_argument_descriptor_tables(
    const rdna4_argument_descriptor_usage *ordinary,
    const uint32_t table_stages[3], const uint32_t texel_stages[2],
    const uint32_t descriptors[3], const uint32_t texel_descriptors[2],
    rdna4_argument_descriptor_usage *usage) {
    if (!ordinary || !table_stages || !texel_stages || !descriptors ||
        !texel_descriptors || !usage)
        return 0;
    rdna4_argument_descriptor_usage result = *ordinary;
    const VkDescriptorType types[] = {VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,
        VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, VK_DESCRIPTOR_TYPE_SAMPLER,
        VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER, VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER};
    for (unsigned i = 0; i < 5; ++i) {
        uint32_t stages = i < 3 ? table_stages[i] : texel_stages[i - 3];
        uint64_t count = i < 3 ? descriptors[i] : texel_descriptors[i - 3];
        if (!stages) continue;
        if ((stages & ~3u) ||
            !rdna4_argument_descriptor_add(&result.pipeline, types[i], count)) return 0;
        for (unsigned stage = 0; stage < 2; ++stage)
            if ((stages & (1u << stage)) &&
                !rdna4_argument_descriptor_add(&result.per_stage[stage], types[i], count)) return 0;
    }
    *usage = result;
    return 1;
}

static inline rdna4_argument_descriptor_limits rdna4_argument_descriptor_device_limits(
    const VkPhysicalDeviceLimits *ordinary,
    const VkPhysicalDeviceVulkan12Properties *uab, int update_after_bind) {
    rdna4_argument_descriptor_limits result = {0};
    uint64_t stage[RDNA4_ARGUMENT_DESCRIPTOR_TYPE_GROUPS] = {
        update_after_bind ? uab->maxPerStageDescriptorUpdateAfterBindSamplers : ordinary->maxPerStageDescriptorSamplers,
        update_after_bind ? uab->maxPerStageDescriptorUpdateAfterBindUniformBuffers : ordinary->maxPerStageDescriptorUniformBuffers,
        UINT64_MAX,
        update_after_bind ? uab->maxPerStageDescriptorUpdateAfterBindStorageBuffers : ordinary->maxPerStageDescriptorStorageBuffers,
        UINT64_MAX,
        update_after_bind ? uab->maxPerStageDescriptorUpdateAfterBindSampledImages : ordinary->maxPerStageDescriptorSampledImages,
        update_after_bind ? uab->maxPerStageDescriptorUpdateAfterBindStorageImages : ordinary->maxPerStageDescriptorStorageImages,
        update_after_bind ? uab->maxPerStageDescriptorUpdateAfterBindInputAttachments : ordinary->maxPerStageDescriptorInputAttachments,
    };
    for (unsigned s = 0; s < 2; ++s)
        for (unsigned group = 0; group < RDNA4_ARGUMENT_DESCRIPTOR_TYPE_GROUPS; ++group)
            result.max_per_stage[s][group] = stage[group];
    const uint64_t pipeline[] = {
        update_after_bind ? uab->maxDescriptorSetUpdateAfterBindSamplers : ordinary->maxDescriptorSetSamplers,
        update_after_bind ? uab->maxDescriptorSetUpdateAfterBindUniformBuffers : ordinary->maxDescriptorSetUniformBuffers,
        update_after_bind ? uab->maxDescriptorSetUpdateAfterBindUniformBuffersDynamic : ordinary->maxDescriptorSetUniformBuffersDynamic,
        update_after_bind ? uab->maxDescriptorSetUpdateAfterBindStorageBuffers : ordinary->maxDescriptorSetStorageBuffers,
        update_after_bind ? uab->maxDescriptorSetUpdateAfterBindStorageBuffersDynamic : ordinary->maxDescriptorSetStorageBuffersDynamic,
        update_after_bind ? uab->maxDescriptorSetUpdateAfterBindSampledImages : ordinary->maxDescriptorSetSampledImages,
        update_after_bind ? uab->maxDescriptorSetUpdateAfterBindStorageImages : ordinary->maxDescriptorSetStorageImages,
        update_after_bind ? uab->maxDescriptorSetUpdateAfterBindInputAttachments : ordinary->maxDescriptorSetInputAttachments,
    };
    for (unsigned group = 0; group < RDNA4_ARGUMENT_DESCRIPTOR_TYPE_GROUPS; ++group)
        result.max_pipeline[group] = pipeline[group];
    result.max_per_stage_resources = update_after_bind ? uab->maxPerStageUpdateAfterBindResources : ordinary->maxPerStageResources;
    result.max_fragment_combined_output_resources = ordinary->maxFragmentCombinedOutputResources;
    result.max_color_attachments = ordinary->maxColorAttachments;
    result.max_fragment_output_attachments = ordinary->maxFragmentOutputAttachments;
    result.max_fragment_dual_src_attachments = ordinary->maxFragmentDualSrcAttachments;
    return result;
}

#endif
