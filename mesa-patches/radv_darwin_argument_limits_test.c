/*
 * Host-only checks for the versioned RADV Darwin argument-limit export.
 * No Vulkan instance, native connection or GPU work is created.
 */

#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <vulkan/vulkan_core.h>

#include "radv_darwin_argument_limits.h"

static unsigned checks;

static void require(int condition, const char *message)
{
    ++checks;
    if (!condition) {
        fprintf(stderr, "RADV_DARWIN_ARGUMENT_LIMITS_FAIL %s\n", message);
        exit(1);
    }
}

uint32_t vk_get_driver_version(void)
{
    return VK_MAKE_VERSION(26, 3, 98);
}

static struct radv_darwin_host_facts_v1 base_facts(void)
{
    return (struct radv_darwin_host_facts_v1){
        .struct_size = sizeof(struct radv_darwin_host_facts_v1),
        .valid_flags = RADV_DARWIN_FACT_VENDOR_ID | RADV_DARWIN_FACT_DEVICE_ID | RADV_DARWIN_FACT_GC_VERSION |
                       RADV_DARWIN_FACT_VRAM_BYTES | RADV_DARWIN_FACT_NATIVE_ABI | RADV_DARWIN_FACT_KEXT_BUILD,
        .vendor_id = 0x1002,
        .device_id = 0x7590,
        .gc_version = 0x0c0000,
        .native_abi_major = 1,
        .native_abi_minor = 11,
        .kext_build = 675,
        .vram_bytes = UINT64_C(16) << 30,
    };
}

static void test_stable_core(void)
{
    struct radv_darwin_argument_limits_v1 limits;
    memset(&limits, 0xa5, sizeof(limits));

    require(radv_darwin_get_argument_limits_v1(NULL, &limits) == 0, "NULL facts still return core limits");
    require(limits.struct_size == sizeof(limits), "output struct size");
    require(limits.abi_version == RADV_DARWIN_ARGUMENT_LIMITS_ABI, "ABI version");
    require(limits.flags == (RADV_DARWIN_LIMITS_READ_ONLY | RADV_DARWIN_LIMITS_NO_VULKAN_OBJECT |
                             RADV_DARWIN_LIMITS_NO_PIPELINE_UUID),
            "read-only flags");
    require(limits.valid_flags == RADV_DARWIN_LIMITS_VALID_STABLE_CORE, "memory is unavailable without facts");
    require(!strcmp(limits.build_identity, "rdal-v1 26.3.0-devel (git-test000000)"), "build identity");
    require(strstr(limits.build_identity, "uuid") == NULL, "build identity is not a fabricated pipeline UUID");
    require(limits.api_version == VK_MAKE_VERSION(1, 4, VK_HEADER_VERSION), "API version");
    require(limits.driver_version == VK_MAKE_VERSION(26, 3, 98), "driver version");
    require(limits.driver_id == VK_DRIVER_ID_MESA_RADV, "driver ID");

    require(limits.max_descriptor_set_size == 8388606u, "shared descriptor-set formula");
    require(limits.max_sampler_allocations == 65536u, "sampler allocation count");
    require(limits.max_bound_descriptor_sets == 32u, "bound descriptor sets");
    require(limits.max_per_set_descriptors == 22369621u, "maintenance3 per-set descriptors");
    require(limits.max_storage_buffer_range == UINT32_MAX, "storage buffer range");
    require(limits.max_uab_descriptors_all_pools == 67108863u, "UAB descriptors in all pools");

    require(limits.max_per_stage_descriptor_samplers == 8388606u &&
                limits.max_per_stage_descriptor_uniform_buffers == 8388606u &&
                limits.max_per_stage_descriptor_storage_buffers == 8388606u &&
                limits.max_per_stage_descriptor_sampled_images == 8388606u &&
                limits.max_per_stage_descriptor_storage_images == 8388606u &&
                limits.max_per_stage_descriptor_input_attachments == 8388606u &&
                limits.max_per_stage_resources == 8388606u,
            "ordinary per-stage limits");
    require(limits.max_descriptor_set_samplers == 8388606u && limits.max_descriptor_set_uniform_buffers == 8388606u &&
                limits.max_descriptor_set_uniform_buffers_dynamic == 16u &&
                limits.max_descriptor_set_storage_buffers == 8388606u &&
                limits.max_descriptor_set_storage_buffers_dynamic == 8u &&
                limits.max_descriptor_set_sampled_images == 8388606u &&
                limits.max_descriptor_set_storage_images == 8388606u &&
                limits.max_descriptor_set_input_attachments == 8388606u,
            "ordinary pipeline limits");

    require(limits.max_per_stage_descriptor_update_after_bind_samplers == 8388606u &&
                limits.max_per_stage_descriptor_update_after_bind_uniform_buffers == 8388606u &&
                limits.max_per_stage_descriptor_update_after_bind_storage_buffers == 8388606u &&
                limits.max_per_stage_descriptor_update_after_bind_sampled_images == 8388606u &&
                limits.max_per_stage_descriptor_update_after_bind_storage_images == 8388606u &&
                limits.max_per_stage_descriptor_update_after_bind_input_attachments == 8388606u &&
                limits.max_per_stage_update_after_bind_resources == 8388606u,
            "UAB per-stage limits");
    require(limits.max_descriptor_set_update_after_bind_samplers == 8388606u &&
                limits.max_descriptor_set_update_after_bind_uniform_buffers == 8388606u &&
                limits.max_descriptor_set_update_after_bind_uniform_buffers_dynamic == 16u &&
                limits.max_descriptor_set_update_after_bind_storage_buffers == 8388606u &&
                limits.max_descriptor_set_update_after_bind_storage_buffers_dynamic == 8u &&
                limits.max_descriptor_set_update_after_bind_sampled_images == 8388606u &&
                limits.max_descriptor_set_update_after_bind_storage_images == 8388606u &&
                limits.max_descriptor_set_update_after_bind_input_attachments == 8388606u,
            "UAB whole-pipeline limits");

    require(limits.max_fragment_combined_output_resources == 8388606u, "fragment combined resources");
    require(limits.max_color_attachments == 8u && limits.max_fragment_output_attachments == 8u &&
                limits.max_fragment_dual_src_attachments == 1u,
            "fragment and color attachment limits");
    require(limits.max_memory_allocation_size == 0 && limits.max_buffer_size == 0,
            "chip-dependent fields are not guessed");
}

static void test_memory_facts(void)
{
    struct radv_darwin_argument_limits_v1 limits;
    struct radv_darwin_host_facts_v1 facts = base_facts();
    require(radv_darwin_get_argument_limits_v1(&facts, &limits) == 0, "full facts accepted");
    require(limits.valid_flags == (RADV_DARWIN_LIMITS_VALID_STABLE_CORE | RADV_DARWIN_LIMITS_VALID_MEMORY),
            "memory valid flag");
    require(limits.max_memory_allocation_size == UINT64_C(8) << 30 && limits.max_buffer_size == UINT64_C(8) << 30,
            "16 GiB input is capped at 8 GiB");

    facts.vram_bytes = UINT64_C(1) << 30;
    require(radv_darwin_get_argument_limits_v1(&facts, &limits) == 0 &&
                limits.max_memory_allocation_size == UINT64_C(0xfffffffc) &&
                limits.max_buffer_size == UINT64_C(0xfffffffc),
            "small input retains the backend allocation floor");

    facts.vram_bytes = UINT64_C(6) << 30;
    require(radv_darwin_get_argument_limits_v1(&facts, &limits) == 0 && limits.max_memory_allocation_size == UINT64_C(6)
                                                                                                                 << 30,
            "memory-dependent 6 GiB limit");

    facts.vram_bytes = (UINT64_C(4) << 30) + 1;
    require(radv_darwin_get_argument_limits_v1(&facts, &limits) == 0 &&
                limits.max_memory_allocation_size == (UINT64_C(4) << 30) + (UINT64_C(256) << 20),
            "native VRAM is rounded to 256 MiB before the 8 GiB clamp");

    facts.vram_bytes = UINT64_C(8) << 30;
    require(radv_darwin_get_argument_limits_v1(&facts, &limits) == 0 && limits.max_memory_allocation_size == UINT64_C(8)
                                                                                                                 << 30,
            "last legal capped VRAM size");
}

static void test_optional_facts(void)
{
    struct radv_darwin_argument_limits_v1 limits;
    struct radv_darwin_host_facts_v1 facts = {
        .struct_size = sizeof(facts),
        .valid_flags = RADV_DARWIN_FACT_NATIVE_ABI | RADV_DARWIN_FACT_KEXT_BUILD,
        .native_abi_major = 1,
        .native_abi_minor = 11,
        .kext_build = 675,
    };
    require(radv_darwin_get_argument_limits_v1(&facts, &limits) == 0 &&
                limits.valid_flags == RADV_DARWIN_LIMITS_VALID_STABLE_CORE,
            "ABI/build facts do not fabricate memory limits");

    facts = base_facts();
    facts.device_id = 0x7550;
    facts.gc_version = 0x0c0001;
    require(radv_darwin_get_argument_limits_v1(&facts, &limits) == 0, "gfx1201 identity pair accepted");
}

static void test_rejections(void)
{
    struct radv_darwin_argument_limits_v1 limits;
    struct radv_darwin_host_facts_v1 facts = base_facts();

    require(radv_darwin_get_argument_limits_v1(&facts, NULL) == -EINVAL, "NULL output rejected");

    facts = base_facts();
    facts.struct_size--;
    require(radv_darwin_get_argument_limits_v1(&facts, &limits) == -EINVAL, "facts struct size rejected");

    facts = base_facts();
    facts.valid_flags |= 1u << 31;
    require(radv_darwin_get_argument_limits_v1(&facts, &limits) == -EINVAL, "unknown facts flags rejected");

    facts = base_facts();
    facts.reserved[0] = 1;
    require(radv_darwin_get_argument_limits_v1(&facts, &limits) == -EINVAL, "reserved facts word rejected");

    facts = base_facts();
    facts.vendor_id = 0x8086;
    require(radv_darwin_get_argument_limits_v1(&facts, &limits) == -EINVAL, "wrong vendor rejected");

    facts = base_facts();
    facts.device_id = 0x1234;
    require(radv_darwin_get_argument_limits_v1(&facts, &limits) == -EINVAL, "unknown device rejected");

    facts = base_facts();
    facts.gc_version = 0x0c0002;
    require(radv_darwin_get_argument_limits_v1(&facts, &limits) == -EINVAL, "unknown GC version rejected");

    facts = base_facts();
    facts.gc_version = 0x0c0001;
    require(radv_darwin_get_argument_limits_v1(&facts, &limits) == -EINVAL, "PCI/GC mismatch rejected");

    facts = base_facts();
    facts.vram_bytes = 0;
    require(radv_darwin_get_argument_limits_v1(&facts, &limits) == -EINVAL, "zero advertised VRAM rejected");

    facts = base_facts();
    facts.vram_bytes = UINT64_MAX;
    require(radv_darwin_get_argument_limits_v1(&facts, &limits) == -EINVAL, "VRAM alignment overflow rejected");
}

int main(void)
{
    require(sizeof(struct radv_darwin_host_facts_v1) == 48, "host facts v1 layout");
    require(sizeof(struct radv_darwin_argument_limits_v1) == 272, "argument limits v1 layout");
    test_stable_core();
    test_memory_facts();
    test_optional_facts();
    test_rejections();
    printf("RADV_DARWIN_ARGUMENT_LIMITS_PASS checks=%u "
           "vulkan_object=0 native_open=0 gpu_execution=0\n",
           checks);
    return 0;
}
