/*
 * Compare RADV's pure argument-limit export with the properties of a physical
 * device. --pure-only performs only dlopen/dlsym and the export call: it does
 * not create a VkInstance, enumerate devices, create a logical device, or
 * submit work. The normal query-only mode enumerates the physical device and
 * may therefore perform RADV's short native open while reading properties.
 */
#include <dlfcn.h>
#include <errno.h>
#include <inttypes.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <vulkan/vulkan.h>

/*
 * Standalone mirror of ABI v1 from
 * include/darwin/radv_darwin_argument_limits.h. The size assertions make a
 * drift in this diagnostic fail at compile time instead of producing a
 * plausible but truncated comparison.
 */
#define PROBE_ARGUMENT_LIMITS_ABI 1u

enum probe_host_fact_flags {
    PROBE_FACT_VENDOR_ID = 1u << 0,
    PROBE_FACT_DEVICE_ID = 1u << 1,
    PROBE_FACT_GC_VERSION = 1u << 2,
    PROBE_FACT_VRAM_BYTES = 1u << 3,
    PROBE_FACT_NATIVE_ABI = 1u << 4,
    PROBE_FACT_KEXT_BUILD = 1u << 5,
};

enum probe_limit_flags {
    PROBE_LIMITS_READ_ONLY = 1u << 0,
    PROBE_LIMITS_NO_VULKAN_OBJECT = 1u << 1,
    PROBE_LIMITS_NO_PIPELINE_UUID = 1u << 2,
};

enum probe_limit_valid_flags {
    PROBE_LIMITS_VALID_STABLE_CORE = 1u << 0,
    PROBE_LIMITS_VALID_MEMORY = 1u << 1,
};

struct probe_host_facts_v1 {
    uint32_t struct_size;
    uint32_t valid_flags;
    uint32_t vendor_id;
    uint32_t device_id;
    uint32_t gc_version;
    uint32_t native_abi_major;
    uint32_t native_abi_minor;
    uint32_t kext_build;
    uint32_t reserved[2];
    uint64_t vram_bytes;
};

struct probe_argument_limits_v1 {
    uint32_t struct_size;
    uint32_t abi_version;
    uint32_t flags;
    uint32_t valid_flags;
    char build_identity[64];

    uint32_t api_version;
    uint32_t driver_version;
    uint32_t driver_id;

    uint32_t max_descriptor_set_size;
    uint32_t max_sampler_allocations;
    uint32_t max_bound_descriptor_sets;
    uint32_t max_per_set_descriptors;
    uint32_t max_storage_buffer_range;
    uint32_t max_uab_descriptors_all_pools;

    uint32_t max_per_stage_descriptor_samplers;
    uint32_t max_per_stage_descriptor_uniform_buffers;
    uint32_t max_per_stage_descriptor_storage_buffers;
    uint32_t max_per_stage_descriptor_sampled_images;
    uint32_t max_per_stage_descriptor_storage_images;
    uint32_t max_per_stage_descriptor_input_attachments;
    uint32_t max_per_stage_resources;

    uint32_t max_descriptor_set_samplers;
    uint32_t max_descriptor_set_uniform_buffers;
    uint32_t max_descriptor_set_uniform_buffers_dynamic;
    uint32_t max_descriptor_set_storage_buffers;
    uint32_t max_descriptor_set_storage_buffers_dynamic;
    uint32_t max_descriptor_set_sampled_images;
    uint32_t max_descriptor_set_storage_images;
    uint32_t max_descriptor_set_input_attachments;

    uint32_t max_per_stage_descriptor_update_after_bind_samplers;
    uint32_t max_per_stage_descriptor_update_after_bind_uniform_buffers;
    uint32_t max_per_stage_descriptor_update_after_bind_storage_buffers;
    uint32_t max_per_stage_descriptor_update_after_bind_sampled_images;
    uint32_t max_per_stage_descriptor_update_after_bind_storage_images;
    uint32_t max_per_stage_descriptor_update_after_bind_input_attachments;
    uint32_t max_per_stage_update_after_bind_resources;

    uint32_t max_descriptor_set_update_after_bind_samplers;
    uint32_t max_descriptor_set_update_after_bind_uniform_buffers;
    uint32_t max_descriptor_set_update_after_bind_uniform_buffers_dynamic;
    uint32_t max_descriptor_set_update_after_bind_storage_buffers;
    uint32_t max_descriptor_set_update_after_bind_storage_buffers_dynamic;
    uint32_t max_descriptor_set_update_after_bind_sampled_images;
    uint32_t max_descriptor_set_update_after_bind_storage_images;
    uint32_t max_descriptor_set_update_after_bind_input_attachments;
    uint32_t max_update_after_bind_descriptors_in_all_pools;

    uint32_t max_fragment_combined_output_resources;
    uint32_t max_color_attachments;
    uint32_t max_fragment_output_attachments;
    uint32_t max_fragment_dual_src_attachments;

    uint64_t max_memory_allocation_size;
    uint64_t max_buffer_size;
};

typedef int (*pure_limits_fn)(const struct probe_host_facts_v1 *, struct probe_argument_limits_v1 *);

_Static_assert(sizeof(struct probe_host_facts_v1) == 48, "host facts v1 ABI");
_Static_assert(sizeof(struct probe_argument_limits_v1) == 272, "argument limits v1 ABI");

static unsigned compare_count;
static unsigned compare_mismatches;

static int
parse_env_u64(const char *name, uint64_t *value, int *present)
{
    const char *text = getenv(name);
    if (!text || !text[0]) {
        *present = 0;
        return 1;
    }
    char *end = NULL;
    errno = 0;
    const uint64_t parsed = strtoull(text, &end, 0);
    if (errno || !end || *end || end == text) {
        fprintf(stderr, "ARGUMENT_LIMITS_FAIL invalid %s=%s\n", name, text);
        return 0;
    }
    *value = parsed;
    *present = 1;
    return 1;
}

static int
load_pure_limits(void *library, pure_limits_fn *function)
{
    dlerror();
    *function = (pure_limits_fn)dlsym(library, "radv_darwin_get_argument_limits_v1");
    const char *error = dlerror();
    if (error) {
        fprintf(stderr, "ARGUMENT_LIMITS_EXPORT_MISSING symbol=radv_darwin_get_argument_limits_v1 dlerror=%s\n",
                error);
        *function = NULL;
        return 0;
    }
    if (!*function) {
        fprintf(stderr,
                "ARGUMENT_LIMITS_EXPORT_MISSING symbol=radv_darwin_get_argument_limits_v1 dlerror=null-symbol\n");
        return 0;
    }
    return 1;
}

static void
print_pure_u32(const char *name, uint32_t value)
{
    printf("ARGUMENT_LIMITS_PURE key=%s value=%" PRIu32 "\n", name, value);
}

static void
print_pure_u64(const char *name, uint64_t value)
{
    printf("ARGUMENT_LIMITS_PURE key=%s value=%" PRIu64 "\n", name, value);
}

#define PURE_U32(field) print_pure_u32(#field, limits->field)
#define PURE_U64(field) print_pure_u64(#field, limits->field)

static int
print_pure_limits(const struct probe_argument_limits_v1 *limits)
{
    static const uint32_t known_limit_flags =
        PROBE_LIMITS_READ_ONLY | PROBE_LIMITS_NO_VULKAN_OBJECT | PROBE_LIMITS_NO_PIPELINE_UUID;
    static const uint32_t known_valid_flags = PROBE_LIMITS_VALID_STABLE_CORE | PROBE_LIMITS_VALID_MEMORY;

    if (limits->struct_size != sizeof(*limits) || limits->abi_version != PROBE_ARGUMENT_LIMITS_ABI) {
        fprintf(stderr, "ARGUMENT_LIMITS_PURE_FAIL bad-abi size=%" PRIu32 " abi=%" PRIu32 "\n", limits->struct_size,
                limits->abi_version);
        return 0;
    }
    if ((limits->flags & known_limit_flags) != known_limit_flags || (limits->flags & ~known_limit_flags)) {
        fprintf(stderr, "ARGUMENT_LIMITS_PURE_FAIL bad-flags flags=0x%08" PRIx32 "\n", limits->flags);
        return 0;
    }
    if (!(limits->valid_flags & PROBE_LIMITS_VALID_STABLE_CORE) || (limits->valid_flags & ~known_valid_flags) ||
        !limits->build_identity[0] || memchr(limits->build_identity, '\0', sizeof(limits->build_identity)) == NULL) {
        fprintf(stderr, "ARGUMENT_LIMITS_PURE_FAIL bad-validity flags=0x%08" PRIx32 "\n", limits->valid_flags);
        return 0;
    }

    printf("ARGUMENT_LIMITS_PURE status=pass struct_size=%" PRIu32 " abi=%" PRIu32 " flags=0x%08" PRIx32
           " valid_flags=0x%08" PRIx32 " build_identity=\"%s\" pipeline_cache_uuid=absent\n",
           limits->struct_size, limits->abi_version, limits->flags, limits->valid_flags, limits->build_identity);
    PURE_U32(api_version);
    PURE_U32(driver_version);
    PURE_U32(driver_id);
    PURE_U32(max_descriptor_set_size);
    PURE_U32(max_sampler_allocations);
    PURE_U32(max_bound_descriptor_sets);
    PURE_U32(max_per_set_descriptors);
    PURE_U32(max_storage_buffer_range);
    PURE_U32(max_uab_descriptors_all_pools);
    PURE_U32(max_per_stage_descriptor_samplers);
    PURE_U32(max_per_stage_descriptor_uniform_buffers);
    PURE_U32(max_per_stage_descriptor_storage_buffers);
    PURE_U32(max_per_stage_descriptor_sampled_images);
    PURE_U32(max_per_stage_descriptor_storage_images);
    PURE_U32(max_per_stage_descriptor_input_attachments);
    PURE_U32(max_per_stage_resources);
    PURE_U32(max_descriptor_set_samplers);
    PURE_U32(max_descriptor_set_uniform_buffers);
    PURE_U32(max_descriptor_set_uniform_buffers_dynamic);
    PURE_U32(max_descriptor_set_storage_buffers);
    PURE_U32(max_descriptor_set_storage_buffers_dynamic);
    PURE_U32(max_descriptor_set_sampled_images);
    PURE_U32(max_descriptor_set_storage_images);
    PURE_U32(max_descriptor_set_input_attachments);
    PURE_U32(max_per_stage_descriptor_update_after_bind_samplers);
    PURE_U32(max_per_stage_descriptor_update_after_bind_uniform_buffers);
    PURE_U32(max_per_stage_descriptor_update_after_bind_storage_buffers);
    PURE_U32(max_per_stage_descriptor_update_after_bind_sampled_images);
    PURE_U32(max_per_stage_descriptor_update_after_bind_storage_images);
    PURE_U32(max_per_stage_descriptor_update_after_bind_input_attachments);
    PURE_U32(max_per_stage_update_after_bind_resources);
    PURE_U32(max_descriptor_set_update_after_bind_samplers);
    PURE_U32(max_descriptor_set_update_after_bind_uniform_buffers);
    PURE_U32(max_descriptor_set_update_after_bind_uniform_buffers_dynamic);
    PURE_U32(max_descriptor_set_update_after_bind_storage_buffers);
    PURE_U32(max_descriptor_set_update_after_bind_storage_buffers_dynamic);
    PURE_U32(max_descriptor_set_update_after_bind_sampled_images);
    PURE_U32(max_descriptor_set_update_after_bind_storage_images);
    PURE_U32(max_descriptor_set_update_after_bind_input_attachments);
    PURE_U32(max_update_after_bind_descriptors_in_all_pools);
    PURE_U32(max_fragment_combined_output_resources);
    PURE_U32(max_color_attachments);
    PURE_U32(max_fragment_output_attachments);
    PURE_U32(max_fragment_dual_src_attachments);
    PURE_U64(max_memory_allocation_size);
    PURE_U64(max_buffer_size);
    return 1;
}

#undef PURE_U32
#undef PURE_U64

static void
compare_u32(const char *name, uint32_t pure, uint32_t property)
{
    ++compare_count;
    if (pure != property) {
        ++compare_mismatches;
        printf("ARGUMENT_LIMITS_COMPARE_MISMATCH key=%s pure=%" PRIu32 " property=%" PRIu32 "\n", name, pure,
               property);
    }
}

static void
compare_u64(const char *name, uint64_t pure, uint64_t property)
{
    ++compare_count;
    if (pure != property) {
        ++compare_mismatches;
        printf("ARGUMENT_LIMITS_COMPARE_MISMATCH key=%s pure=%" PRIu64 " property=%" PRIu64 "\n", name, pure,
               property);
    }
}

#define COMPARE_U32(name, property) compare_u32(#name, limits->name, (uint32_t)(property))
#define COMPARE_U64(name, property) compare_u64(#name, limits->name, (uint64_t)(property))

static int
compare_pure_to_properties(const struct probe_argument_limits_v1 *limits, const VkPhysicalDeviceProperties *base,
                           const VkPhysicalDeviceVulkan12Properties *v12, const VkPhysicalDeviceVulkan13Properties *v13,
                           const VkPhysicalDeviceMaintenance3Properties *m3,
                           const VkPhysicalDeviceMaintenance4Properties *m4)
{
    const VkPhysicalDeviceLimits *l = &base->limits;
    compare_count = 0;
    compare_mismatches = 0;

    COMPARE_U32(api_version, base->apiVersion);
    COMPARE_U32(driver_version, base->driverVersion);
    COMPARE_U32(driver_id, v12->driverID);
    COMPARE_U32(max_sampler_allocations, l->maxSamplerAllocationCount);
    COMPARE_U32(max_bound_descriptor_sets, l->maxBoundDescriptorSets);
    COMPARE_U32(max_per_set_descriptors, m3->maxPerSetDescriptors);
    COMPARE_U32(max_storage_buffer_range, l->maxStorageBufferRange);
    COMPARE_U32(max_uab_descriptors_all_pools, v12->maxUpdateAfterBindDescriptorsInAllPools);

    COMPARE_U32(max_per_stage_descriptor_samplers, l->maxPerStageDescriptorSamplers);
    COMPARE_U32(max_per_stage_descriptor_uniform_buffers, l->maxPerStageDescriptorUniformBuffers);
    COMPARE_U32(max_per_stage_descriptor_storage_buffers, l->maxPerStageDescriptorStorageBuffers);
    COMPARE_U32(max_per_stage_descriptor_sampled_images, l->maxPerStageDescriptorSampledImages);
    COMPARE_U32(max_per_stage_descriptor_storage_images, l->maxPerStageDescriptorStorageImages);
    COMPARE_U32(max_per_stage_descriptor_input_attachments, l->maxPerStageDescriptorInputAttachments);
    COMPARE_U32(max_per_stage_resources, l->maxPerStageResources);
    COMPARE_U32(max_descriptor_set_samplers, l->maxDescriptorSetSamplers);
    COMPARE_U32(max_descriptor_set_uniform_buffers, l->maxDescriptorSetUniformBuffers);
    COMPARE_U32(max_descriptor_set_uniform_buffers_dynamic, l->maxDescriptorSetUniformBuffersDynamic);
    COMPARE_U32(max_descriptor_set_storage_buffers, l->maxDescriptorSetStorageBuffers);
    COMPARE_U32(max_descriptor_set_storage_buffers_dynamic, l->maxDescriptorSetStorageBuffersDynamic);
    COMPARE_U32(max_descriptor_set_sampled_images, l->maxDescriptorSetSampledImages);
    COMPARE_U32(max_descriptor_set_storage_images, l->maxDescriptorSetStorageImages);
    COMPARE_U32(max_descriptor_set_input_attachments, l->maxDescriptorSetInputAttachments);

    COMPARE_U32(max_per_stage_descriptor_update_after_bind_samplers,
                v12->maxPerStageDescriptorUpdateAfterBindSamplers);
    COMPARE_U32(max_per_stage_descriptor_update_after_bind_uniform_buffers,
                v12->maxPerStageDescriptorUpdateAfterBindUniformBuffers);
    COMPARE_U32(max_per_stage_descriptor_update_after_bind_storage_buffers,
                v12->maxPerStageDescriptorUpdateAfterBindStorageBuffers);
    COMPARE_U32(max_per_stage_descriptor_update_after_bind_sampled_images,
                v12->maxPerStageDescriptorUpdateAfterBindSampledImages);
    COMPARE_U32(max_per_stage_descriptor_update_after_bind_storage_images,
                v12->maxPerStageDescriptorUpdateAfterBindStorageImages);
    COMPARE_U32(max_per_stage_descriptor_update_after_bind_input_attachments,
                v12->maxPerStageDescriptorUpdateAfterBindInputAttachments);
    COMPARE_U32(max_per_stage_update_after_bind_resources, v12->maxPerStageUpdateAfterBindResources);
    COMPARE_U32(max_descriptor_set_update_after_bind_samplers, v12->maxDescriptorSetUpdateAfterBindSamplers);
    COMPARE_U32(max_descriptor_set_update_after_bind_uniform_buffers,
                v12->maxDescriptorSetUpdateAfterBindUniformBuffers);
    COMPARE_U32(max_descriptor_set_update_after_bind_uniform_buffers_dynamic,
                v12->maxDescriptorSetUpdateAfterBindUniformBuffersDynamic);
    COMPARE_U32(max_descriptor_set_update_after_bind_storage_buffers,
                v12->maxDescriptorSetUpdateAfterBindStorageBuffers);
    COMPARE_U32(max_descriptor_set_update_after_bind_storage_buffers_dynamic,
                v12->maxDescriptorSetUpdateAfterBindStorageBuffersDynamic);
    COMPARE_U32(max_descriptor_set_update_after_bind_sampled_images,
                v12->maxDescriptorSetUpdateAfterBindSampledImages);
    COMPARE_U32(max_descriptor_set_update_after_bind_storage_images,
                v12->maxDescriptorSetUpdateAfterBindStorageImages);
    COMPARE_U32(max_descriptor_set_update_after_bind_input_attachments,
                v12->maxDescriptorSetUpdateAfterBindInputAttachments);
    COMPARE_U32(max_update_after_bind_descriptors_in_all_pools, v12->maxUpdateAfterBindDescriptorsInAllPools);

    COMPARE_U32(max_fragment_combined_output_resources, l->maxFragmentCombinedOutputResources);
    COMPARE_U32(max_color_attachments, l->maxColorAttachments);
    COMPARE_U32(max_fragment_output_attachments, l->maxFragmentOutputAttachments);
    COMPARE_U32(max_fragment_dual_src_attachments, l->maxFragmentDualSrcAttachments);

    /* The shared descriptor-set formula feeds all six ordinary families and
     * the fragment-combined cap in physical properties.
     */
    COMPARE_U32(max_descriptor_set_size, l->maxDescriptorSetSamplers);
    COMPARE_U32(max_descriptor_set_size, l->maxDescriptorSetUniformBuffers);
    COMPARE_U32(max_descriptor_set_size, l->maxDescriptorSetStorageBuffers);
    COMPARE_U32(max_descriptor_set_size, l->maxDescriptorSetSampledImages);
    COMPARE_U32(max_descriptor_set_size, l->maxDescriptorSetStorageImages);
    COMPARE_U32(max_descriptor_set_size, l->maxDescriptorSetInputAttachments);
    COMPARE_U32(max_descriptor_set_size, l->maxPerStageResources);
    COMPARE_U32(max_descriptor_set_size, l->maxFragmentCombinedOutputResources);

    if (limits->valid_flags & PROBE_LIMITS_VALID_MEMORY) {
        COMPARE_U64(max_memory_allocation_size, m3->maxMemoryAllocationSize);
        COMPARE_U64(max_buffer_size, m4->maxBufferSize);
        COMPARE_U64(max_buffer_size, v13->maxBufferSize);
    } else {
        printf("ARGUMENT_LIMITS_COMPARE_SKIP memory=no-host-facts\n");
    }

    if (compare_mismatches) {
        printf("ARGUMENT_LIMITS_COMPARE_FAIL compared=%u mismatches=%u\n", compare_count, compare_mismatches);
        return 0;
    }
    printf("ARGUMENT_LIMITS_COMPARE_PASS compared=%u mismatches=0\n", compare_count);
    return 1;
}

#undef COMPARE_U32
#undef COMPARE_U64

static void
print_uuid(const uint8_t uuid[VK_UUID_SIZE])
{
    for (unsigned i = 0; i < VK_UUID_SIZE; ++i)
        printf("%02x", uuid[i]);
}

#define PROPERTY_U32(key, value)                                                                                      \
    printf("ARGUMENT_LIMITS_PROPERTY key=%s value=%" PRIu32 "\n", (key), (uint32_t)(value))
#define PROPERTY_U64(key, value)                                                                                      \
    printf("ARGUMENT_LIMITS_PROPERTY key=%s value=%" PRIu64 "\n", (key), (uint64_t)(value))

static void
print_properties(uint32_t index, const VkPhysicalDeviceProperties *base, const VkPhysicalDeviceVulkan12Properties *v12,
                 const VkPhysicalDeviceVulkan13Properties *v13, const VkPhysicalDeviceMaintenance3Properties *m3,
                 const VkPhysicalDeviceMaintenance4Properties *m4, const VkPhysicalDeviceSubgroupProperties *subgroup,
                 const VkPhysicalDeviceFeatures *features, const VkPhysicalDeviceVulkan12Features *v12_features)
{
    const VkPhysicalDeviceLimits *l = &base->limits;
    printf("ARGUMENT_LIMITS_PROPERTY key=device_index value=%" PRIu32 "\n", index);
    printf("ARGUMENT_LIMITS_PROPERTY key=device_name value=\"%s\"\n", base->deviceName);
    printf("ARGUMENT_LIMITS_PROPERTY key=vendor_id value=0x%04" PRIx32 "\n", base->vendorID);
    printf("ARGUMENT_LIMITS_PROPERTY key=device_id value=0x%04" PRIx32 "\n", base->deviceID);
    printf("ARGUMENT_LIMITS_PROPERTY key=api_version value=%" PRIu32 "\n", base->apiVersion);
    printf("ARGUMENT_LIMITS_PROPERTY key=driver_version value=%" PRIu32 "\n", base->driverVersion);
    printf("ARGUMENT_LIMITS_PROPERTY key=vulkan12_driver_id value=%" PRIu32 "\n", v12->driverID);
    printf("ARGUMENT_LIMITS_PROPERTY key=pipeline_cache_uuid value=");
    print_uuid(base->pipelineCacheUUID);
    printf(" source=physical-device\n");

    PROPERTY_U32("max_sampler_allocations", l->maxSamplerAllocationCount);
    PROPERTY_U32("max_bound_descriptor_sets", l->maxBoundDescriptorSets);
    PROPERTY_U32("max_per_set_descriptors", m3->maxPerSetDescriptors);
    PROPERTY_U32("max_storage_buffer_range", l->maxStorageBufferRange);
    PROPERTY_U32("max_uab_descriptors_all_pools", v12->maxUpdateAfterBindDescriptorsInAllPools);
    PROPERTY_U32("max_memory_allocation_count", l->maxMemoryAllocationCount);
    PROPERTY_U64("max_memory_allocation_size", m3->maxMemoryAllocationSize);
    PROPERTY_U64("max_buffer_size_maintenance4", m4->maxBufferSize);
    PROPERTY_U64("max_buffer_size_vulkan13", v13->maxBufferSize);

    PROPERTY_U32("max_per_stage_descriptor_samplers", l->maxPerStageDescriptorSamplers);
    PROPERTY_U32("max_per_stage_descriptor_uniform_buffers", l->maxPerStageDescriptorUniformBuffers);
    PROPERTY_U32("max_per_stage_descriptor_storage_buffers", l->maxPerStageDescriptorStorageBuffers);
    PROPERTY_U32("max_per_stage_descriptor_sampled_images", l->maxPerStageDescriptorSampledImages);
    PROPERTY_U32("max_per_stage_descriptor_storage_images", l->maxPerStageDescriptorStorageImages);
    PROPERTY_U32("max_per_stage_descriptor_input_attachments", l->maxPerStageDescriptorInputAttachments);
    PROPERTY_U32("max_per_stage_resources", l->maxPerStageResources);
    PROPERTY_U32("max_descriptor_set_samplers", l->maxDescriptorSetSamplers);
    PROPERTY_U32("max_descriptor_set_uniform_buffers", l->maxDescriptorSetUniformBuffers);
    PROPERTY_U32("max_descriptor_set_uniform_buffers_dynamic", l->maxDescriptorSetUniformBuffersDynamic);
    PROPERTY_U32("max_descriptor_set_storage_buffers", l->maxDescriptorSetStorageBuffers);
    PROPERTY_U32("max_descriptor_set_storage_buffers_dynamic", l->maxDescriptorSetStorageBuffersDynamic);
    PROPERTY_U32("max_descriptor_set_sampled_images", l->maxDescriptorSetSampledImages);
    PROPERTY_U32("max_descriptor_set_storage_images", l->maxDescriptorSetStorageImages);
    PROPERTY_U32("max_descriptor_set_input_attachments", l->maxDescriptorSetInputAttachments);

    PROPERTY_U32("max_per_stage_descriptor_update_after_bind_samplers",
                 v12->maxPerStageDescriptorUpdateAfterBindSamplers);
    PROPERTY_U32("max_per_stage_descriptor_update_after_bind_uniform_buffers",
                 v12->maxPerStageDescriptorUpdateAfterBindUniformBuffers);
    PROPERTY_U32("max_per_stage_descriptor_update_after_bind_storage_buffers",
                 v12->maxPerStageDescriptorUpdateAfterBindStorageBuffers);
    PROPERTY_U32("max_per_stage_descriptor_update_after_bind_sampled_images",
                 v12->maxPerStageDescriptorUpdateAfterBindSampledImages);
    PROPERTY_U32("max_per_stage_descriptor_update_after_bind_storage_images",
                 v12->maxPerStageDescriptorUpdateAfterBindStorageImages);
    PROPERTY_U32("max_per_stage_descriptor_update_after_bind_input_attachments",
                 v12->maxPerStageDescriptorUpdateAfterBindInputAttachments);
    PROPERTY_U32("max_per_stage_update_after_bind_resources", v12->maxPerStageUpdateAfterBindResources);
    PROPERTY_U32("max_descriptor_set_update_after_bind_samplers", v12->maxDescriptorSetUpdateAfterBindSamplers);
    PROPERTY_U32("max_descriptor_set_update_after_bind_uniform_buffers",
                 v12->maxDescriptorSetUpdateAfterBindUniformBuffers);
    PROPERTY_U32("max_descriptor_set_update_after_bind_uniform_buffers_dynamic",
                 v12->maxDescriptorSetUpdateAfterBindUniformBuffersDynamic);
    PROPERTY_U32("max_descriptor_set_update_after_bind_storage_buffers",
                 v12->maxDescriptorSetUpdateAfterBindStorageBuffers);
    PROPERTY_U32("max_descriptor_set_update_after_bind_storage_buffers_dynamic",
                 v12->maxDescriptorSetUpdateAfterBindStorageBuffersDynamic);
    PROPERTY_U32("max_descriptor_set_update_after_bind_sampled_images",
                 v12->maxDescriptorSetUpdateAfterBindSampledImages);
    PROPERTY_U32("max_descriptor_set_update_after_bind_storage_images",
                 v12->maxDescriptorSetUpdateAfterBindStorageImages);
    PROPERTY_U32("max_descriptor_set_update_after_bind_input_attachments",
                 v12->maxDescriptorSetUpdateAfterBindInputAttachments);
    PROPERTY_U32("max_update_after_bind_descriptors_in_all_pools",
                 v12->maxUpdateAfterBindDescriptorsInAllPools);

    PROPERTY_U32("max_fragment_combined_output_resources", l->maxFragmentCombinedOutputResources);
    PROPERTY_U32("max_color_attachments", l->maxColorAttachments);
    PROPERTY_U32("max_fragment_output_attachments", l->maxFragmentOutputAttachments);
    PROPERTY_U32("max_fragment_dual_src_attachments", l->maxFragmentDualSrcAttachments);

    PROPERTY_U32("sampler_anisotropy", features->samplerAnisotropy);
    printf("ARGUMENT_LIMITS_PROPERTY key=max_sampler_anisotropy value=%.9g\n", (double)l->maxSamplerAnisotropy);
    printf("ARGUMENT_LIMITS_PROPERTY key=max_sampler_lod_bias value=%.9g\n", (double)l->maxSamplerLodBias);
    PROPERTY_U32("sampler_mirror_clamp_to_edge", v12_features->samplerMirrorClampToEdge);

    PROPERTY_U32("uab_runtime_descriptor_array", v12_features->runtimeDescriptorArray);
    PROPERTY_U32("uab_variable_descriptor_count", v12_features->descriptorBindingVariableDescriptorCount);
    PROPERTY_U32("uab_partially_bound", v12_features->descriptorBindingPartiallyBound);
    PROPERTY_U32("uab_sampled_image_update_after_bind", v12_features->descriptorBindingSampledImageUpdateAfterBind);
    PROPERTY_U32("uab_uniform_buffer_update_after_bind", v12_features->descriptorBindingUniformBufferUpdateAfterBind);
    PROPERTY_U32("uab_storage_buffer_update_after_bind", v12_features->descriptorBindingStorageBufferUpdateAfterBind);
    PROPERTY_U32("uab_storage_image_update_after_bind", v12_features->descriptorBindingStorageImageUpdateAfterBind);
    PROPERTY_U32("uab_uniform_texel_buffer_update_after_bind",
                 v12_features->descriptorBindingUniformTexelBufferUpdateAfterBind);
    PROPERTY_U32("uab_storage_texel_buffer_update_after_bind",
                 v12_features->descriptorBindingStorageTexelBufferUpdateAfterBind);
    PROPERTY_U32("uab_update_unused_while_pending", v12_features->descriptorBindingUpdateUnusedWhilePending);
    PROPERTY_U32("nonuniform_sampled_images", v12_features->shaderSampledImageArrayNonUniformIndexing);
    PROPERTY_U32("nonuniform_storage_images", v12_features->shaderStorageImageArrayNonUniformIndexing);
    PROPERTY_U32("quad_divergent_implicit_lod", v12->quadDivergentImplicitLod);

    PROPERTY_U32("subgroup_size", subgroup->subgroupSize);
    PROPERTY_U32("subgroup_supported_stages", subgroup->supportedStages);
    PROPERTY_U32("subgroup_supported_operations", subgroup->supportedOperations);
    PROPERTY_U32("subgroup_fragment_quad",
                 (subgroup->supportedStages & VK_SHADER_STAGE_FRAGMENT_BIT) &&
                       (subgroup->supportedOperations & VK_SUBGROUP_FEATURE_QUAD_BIT)
                    ? 1
                    : 0);
}

#undef PROPERTY_U32
#undef PROPERTY_U64

int
main(int argc, char **argv)
{
    if (argc < 2) {
        fprintf(stderr, "usage: %s VULKAN_LIBRARY [--pure-only]\n", argv[0]);
        return 2;
    }
    int pure_only = 0;
    for (int i = 2; i < argc; ++i) {
        if (!strcmp(argv[i], "--pure-only")) {
            pure_only = 1;
        } else {
            fprintf(stderr, "ARGUMENT_LIMITS_FAIL unknown-argument=%s\n", argv[i]);
            return 2;
        }
    }

    struct probe_host_facts_v1 facts = {.struct_size = sizeof(facts)};
    uint64_t parsed = 0;
    int present = 0;
    if (!parse_env_u64("RADV_LIMITS_VENDOR_ID", &parsed, &present))
        return 2;
    if (present) {
        facts.valid_flags |= PROBE_FACT_VENDOR_ID;
        facts.vendor_id = (uint32_t)parsed;
    }
    if (!parse_env_u64("RADV_LIMITS_DEVICE_ID", &parsed, &present))
        return 2;
    if (present) {
        facts.valid_flags |= PROBE_FACT_DEVICE_ID;
        facts.device_id = (uint32_t)parsed;
    }
    if (!parse_env_u64("RADV_LIMITS_GC_VERSION", &parsed, &present))
        return 2;
    if (present) {
        facts.valid_flags |= PROBE_FACT_GC_VERSION;
        facts.gc_version = (uint32_t)parsed;
    }
    if (!parse_env_u64("RADV_LIMITS_VRAM_BYTES", &parsed, &present))
        return 2;
    if (present) {
        facts.valid_flags |= PROBE_FACT_VRAM_BYTES;
        facts.vram_bytes = parsed;
    }
    const int have_facts = facts.valid_flags != 0;

    void *library = dlopen(argv[1], RTLD_NOW | RTLD_LOCAL);
    if (!library) {
        fprintf(stderr, "ARGUMENT_LIMITS_FAIL dlopen: %s\n", dlerror());
        return 1;
    }

    pure_limits_fn pure_limits = NULL;
    const int pure_available = load_pure_limits(library, &pure_limits);
    struct probe_argument_limits_v1 limits;
    memset(&limits, 0, sizeof(limits));
    int pure_valid = 0;
    if (pure_available) {
        const int result = pure_limits(have_facts ? &facts : NULL, &limits);
        printf("ARGUMENT_LIMITS_PURE_CALL result=%d facts=%s\n", result, have_facts ? "provided" : "null");
        if (!result)
            pure_valid = print_pure_limits(&limits);
        else
            fprintf(stderr, "ARGUMENT_LIMITS_PURE_FAIL call-result=%d\n", result);
    }

    if (pure_only) {
        dlclose(library);
        if (!pure_available)
            return 3;
        if (!pure_valid)
            return 1;
        printf("ARGUMENT_LIMITS_PURE_ONLY_PASS instance=0 physical_device_enumeration=0 native_probe=0 "
               "logical_device=0 gpu_execution=0\n");
        return 0;
    }

    PFN_vkGetInstanceProcAddr get = (PFN_vkGetInstanceProcAddr)dlsym(library, "vk_icdGetInstanceProcAddr");
    if (!get)
        get = (PFN_vkGetInstanceProcAddr)dlsym(library, "vkGetInstanceProcAddr");
    if (!get) {
        fprintf(stderr, "ARGUMENT_LIMITS_FAIL no-instance-entry-point\n");
        dlclose(library);
        return 1;
    }
    PFN_vkCreateInstance create = (PFN_vkCreateInstance)get(VK_NULL_HANDLE, "vkCreateInstance");
    PFN_vkEnumerateInstanceVersion version =
        (PFN_vkEnumerateInstanceVersion)get(VK_NULL_HANDLE, "vkEnumerateInstanceVersion");
    uint32_t api = VK_API_VERSION_1_0;
    VkResult result = version ? version(&api) : VK_SUCCESS;
    if (!create || result != VK_SUCCESS || api < VK_API_VERSION_1_2) {
        fprintf(stderr, "ARGUMENT_LIMITS_FAIL instance-support result=%d api=%" PRIu32 "\n", result, api);
        dlclose(library);
        return 2;
    }
    VkApplicationInfo app = {
        .sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
        .pApplicationName = "rdna4-argument-limits",
        .apiVersion = api < VK_API_VERSION_1_3 ? api : VK_API_VERSION_1_3,
    };
    VkInstanceCreateInfo info = {.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, .pApplicationInfo = &app};
    VkInstance instance = VK_NULL_HANDLE;
    result = create(&info, NULL, &instance);
    if (result != VK_SUCCESS) {
        fprintf(stderr, "ARGUMENT_LIMITS_FAIL instance=%d\n", result);
        dlclose(library);
        return 1;
    }

    PFN_vkDestroyInstance destroy = (PFN_vkDestroyInstance)get(instance, "vkDestroyInstance");
    PFN_vkEnumeratePhysicalDevices enumerate = (PFN_vkEnumeratePhysicalDevices)get(instance, "vkEnumeratePhysicalDevices");
    PFN_vkGetPhysicalDeviceProperties properties =
        (PFN_vkGetPhysicalDeviceProperties)get(instance, "vkGetPhysicalDeviceProperties");
    PFN_vkGetPhysicalDeviceProperties2 properties2 =
        (PFN_vkGetPhysicalDeviceProperties2)get(instance, "vkGetPhysicalDeviceProperties2");
    PFN_vkGetPhysicalDeviceFeatures2 features2 =
        (PFN_vkGetPhysicalDeviceFeatures2)get(instance, "vkGetPhysicalDeviceFeatures2");
    PFN_vkGetPhysicalDeviceMemoryProperties memory_properties =
        (PFN_vkGetPhysicalDeviceMemoryProperties)get(instance, "vkGetPhysicalDeviceMemoryProperties");
    int status = 1;
    VkPhysicalDevice *devices = NULL;
    if (!destroy || !enumerate || !properties || !properties2 || !features2 || !memory_properties) {
        fprintf(stderr, "ARGUMENT_LIMITS_FAIL missing-physical-device-entry-point\n");
        goto cleanup;
    }

    uint32_t count = 0;
    result = enumerate(instance, &count, NULL);
    if (result != VK_SUCCESS || !count) {
        fprintf(stderr, "ARGUMENT_LIMITS_FAIL enumerate=%d count=%" PRIu32 "\n", result, count);
        goto cleanup;
    }
    devices = calloc(count, sizeof(*devices));
    if (!devices) {
        fprintf(stderr, "ARGUMENT_LIMITS_FAIL device-allocation\n");
        goto cleanup;
    }
    result = enumerate(instance, &count, devices);
    if (result != VK_SUCCESS || !count) {
        fprintf(stderr, "ARGUMENT_LIMITS_FAIL enumerate=%d count=%" PRIu32 "\n", result, count);
        goto cleanup;
    }

    status = 0;
    int compare_ok = pure_valid;
    for (uint32_t i = 0; i < count; ++i) {
        VkPhysicalDeviceProperties base;
        properties(devices[i], &base);
        if (base.apiVersion < VK_API_VERSION_1_3) {
            printf("ARGUMENT_LIMITS_MISSING device=%" PRIu32 " name=%s needs-vulkan-1.3\n", i, base.deviceName);
            status = 2;
            continue;
        }

        VkPhysicalDeviceSubgroupProperties subgroup = {
            .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SUBGROUP_PROPERTIES,
        };
        VkPhysicalDeviceMaintenance3Properties m3 = {
            .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MAINTENANCE_3_PROPERTIES,
            .pNext = &subgroup,
        };
        VkPhysicalDeviceMaintenance4Properties m4 = {
            .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MAINTENANCE_4_PROPERTIES,
            .pNext = &m3,
        };
        VkPhysicalDeviceVulkan13Properties v13 = {
            .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_PROPERTIES,
            .pNext = &m4,
        };
        VkPhysicalDeviceVulkan12Properties v12 = {
            .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_PROPERTIES,
            .pNext = &v13,
        };
        VkPhysicalDeviceProperties2 p = {
            .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2,
            .pNext = &v12,
        };
        properties2(devices[i], &p);

        VkPhysicalDeviceVulkan12Features v12_features = {
            .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES,
        };
        VkPhysicalDeviceFeatures2 f = {
            .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2,
            .pNext = &v12_features,
        };
        features2(devices[i], &f);

        VkPhysicalDeviceMemoryProperties memory;
        memory_properties(devices[i], &memory);
        printf("ARGUMENT_LIMITS_MEMORY heaps=%" PRIu32 " types=%" PRIu32 "\n", memory.memoryHeapCount,
               memory.memoryTypeCount);
        for (uint32_t heap = 0; heap < memory.memoryHeapCount; ++heap)
            printf("ARGUMENT_LIMITS_MEMORY_HEAP index=%" PRIu32 " size=%" PRIu64 " flags=0x%08" PRIx32 "\n", heap,
                   (uint64_t)memory.memoryHeaps[heap].size, memory.memoryHeaps[heap].flags);

        print_properties(i, &p.properties, &v12, &v13, &m3, &m4, &subgroup, &f.features, &v12_features);
        if (pure_valid && !compare_pure_to_properties(&limits, &p.properties, &v12, &v13, &m3, &m4))
            compare_ok = 0;
    }

    if (!pure_available)
        status = 2;
    else if (!compare_ok)
        status = 1;
    if (!status)
        printf("ARGUMENT_LIMITS_PASS devices=%" PRIu32
               " instance=1 physical_device_enumeration=1 native_probe_may_open=1 logical_device=0 "
               "gpu_execution=0 pure_export=1 compare=1\n",
               count);
    else
        printf("ARGUMENT_LIMITS_PARTIAL devices=%" PRIu32
               " instance=1 physical_device_enumeration=1 native_probe_may_open=1 logical_device=0 "
               "gpu_execution=0 pure_export=%d compare=%d\n",
               count, pure_available, pure_valid && compare_ok);

cleanup:
    free(devices);
    if (destroy)
        destroy(instance, NULL);
    dlclose(library);
    return status;
}
