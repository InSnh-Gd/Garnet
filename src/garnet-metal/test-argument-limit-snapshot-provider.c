#include "garnet_argument_limit_snapshot.h"

#include <string.h>

int radv_darwin_get_argument_limits_v1(
    const rdna4_radv_darwin_host_facts_v1 *facts,
    rdna4_radv_darwin_argument_limits_v1 *out) {
    (void)facts;
    if (!out) return -22;
    memset(out, 0, sizeof(*out));
    out->struct_size = sizeof(*out);
    out->abi_version = 1;
    out->flags = RDNA4_RADV_DARWIN_LIMITS_READ_ONLY |
                 RDNA4_RADV_DARWIN_LIMITS_NO_VULKAN_OBJECT |
                 RDNA4_RADV_DARWIN_LIMITS_NO_PIPELINE_UUID;
    out->valid_flags = RDNA4_RADV_DARWIN_LIMITS_VALID_STABLE_CORE;
    memcpy(out->build_identity, "rdal-v1 provider-test",
           sizeof("rdal-v1 provider-test"));
    out->api_version = 0x00401000u;
    out->driver_version = 1u;
    out->driver_id = 5u;
    out->max_descriptor_set_size = 8388606u;
    out->max_sampler_allocations = 65536u;
    out->max_bound_descriptor_sets = 32u;
    out->max_per_set_descriptors = 22369621u;
    out->max_storage_buffer_range = UINT32_MAX;
    out->max_uab_descriptors_all_pools = 67108863u;

    out->max_per_stage_descriptor_samplers = 8388606u;
    out->max_per_stage_descriptor_uniform_buffers = 8388606u;
    out->max_per_stage_descriptor_storage_buffers = 8388606u;
    out->max_per_stage_descriptor_sampled_images = 8388606u;
    out->max_per_stage_descriptor_storage_images = 8388606u;
    out->max_per_stage_descriptor_input_attachments = 8388606u;
    out->max_per_stage_resources = 8388606u;
    out->max_descriptor_set_samplers = 8388606u;
    out->max_descriptor_set_uniform_buffers = 8388606u;
    out->max_descriptor_set_uniform_buffers_dynamic = 16u;
    out->max_descriptor_set_storage_buffers = 8388606u;
    out->max_descriptor_set_storage_buffers_dynamic = 8u;
    out->max_descriptor_set_sampled_images = 8388606u;
    out->max_descriptor_set_storage_images = 8388606u;
    out->max_descriptor_set_input_attachments = 8388606u;

    out->max_per_stage_descriptor_update_after_bind_samplers = 8388606u;
    out->max_per_stage_descriptor_update_after_bind_uniform_buffers = 8388606u;
    out->max_per_stage_descriptor_update_after_bind_storage_buffers = 8388606u;
    out->max_per_stage_descriptor_update_after_bind_sampled_images = 8388606u;
    out->max_per_stage_descriptor_update_after_bind_storage_images = 8388606u;
    out->max_per_stage_descriptor_update_after_bind_input_attachments = 8388606u;
    out->max_per_stage_update_after_bind_resources = 8388606u;
    out->max_descriptor_set_update_after_bind_samplers = 8388606u;
    out->max_descriptor_set_update_after_bind_uniform_buffers = 8388606u;
    out->max_descriptor_set_update_after_bind_uniform_buffers_dynamic = 16u;
    out->max_descriptor_set_update_after_bind_storage_buffers = 8388606u;
    out->max_descriptor_set_update_after_bind_storage_buffers_dynamic = 8u;
    out->max_descriptor_set_update_after_bind_sampled_images = 8388606u;
    out->max_descriptor_set_update_after_bind_storage_images = 8388606u;
    out->max_descriptor_set_update_after_bind_input_attachments = 8388606u;
    out->max_update_after_bind_descriptors_in_all_pools = 67108863u;

    out->max_fragment_combined_output_resources = 8388606u;
    out->max_color_attachments = 8u;
    out->max_fragment_output_attachments = 8u;
    out->max_fragment_dual_src_attachments = 1u;
    return 0;
}
