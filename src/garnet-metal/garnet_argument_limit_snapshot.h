#ifndef RDNA4_ARGUMENT_LIMIT_SNAPSHOT_H
#define RDNA4_ARGUMENT_LIMIT_SNAPSHOT_H

/*
 * Immutable, pre-open RADV argument limits for the Darwin Metal driver.
 *
 * The snapshot is read once from radv_darwin_get_argument_limits_v1 in the
 * same bundle that supplies libvulkan_radeon.dylib.  It never creates a
 * Vulkan instance/device, calls rdna4_radv_open(), or inspects live sampler
 * counts.  Getter and admission functions below are pure reads of this
 * frozen value.
 *
 * The structs mirror include/darwin/radv_darwin_argument_limits.h from
 * Mesa patch 0022 (ABI 1).  Keep the local mirror because the Metal bundle
 * is built before a patched Mesa source tree is available at compile time.
 */

#include <stddef.h>
#include <stdint.h>

#include "garnet_argument_public_sampler.h"

#ifdef __cplusplus
extern "C" {
#endif

#define RDNA4_ARGUMENT_LIMIT_EXPORT_NAME "radv_darwin_get_argument_limits_v1"
#define RDNA4_ARGUMENT_LIMIT_SOURCE_ABI 1u
#define RDNA4_ARGUMENT_LIMIT_FACTS_SIZE 48u
#define RDNA4_ARGUMENT_LIMIT_RAW_SIZE 272u
#define RDNA4_ARGUMENT_LIMIT_SOURCE_PATH_SIZE 1024u
#define RDNA4_ARGUMENT_LIMIT_LOADER_ERROR_SIZE 192u

/* ABI8 pixel sampling can create normal, normalized-normal and comparison. */
#define RDNA4_ARGUMENT_LIMIT_DEFAULT_OBJECTS_PER_CONFIG 3u
/* One normal and one comparison dummy representation are outside the count. */
#define RDNA4_ARGUMENT_LIMIT_DEFAULT_DUMMY_OBJECTS 2u

enum rdna4_radv_darwin_host_fact_flags {
    RDNA4_RADV_DARWIN_FACT_VENDOR_ID = 1u << 0,
    RDNA4_RADV_DARWIN_FACT_DEVICE_ID = 1u << 1,
    RDNA4_RADV_DARWIN_FACT_GC_VERSION = 1u << 2,
    RDNA4_RADV_DARWIN_FACT_VRAM_BYTES = 1u << 3,
    RDNA4_RADV_DARWIN_FACT_NATIVE_ABI = 1u << 4,
    RDNA4_RADV_DARWIN_FACT_KEXT_BUILD = 1u << 5,
};

enum rdna4_radv_darwin_argument_limit_flags {
    RDNA4_RADV_DARWIN_LIMITS_READ_ONLY = 1u << 0,
    RDNA4_RADV_DARWIN_LIMITS_NO_VULKAN_OBJECT = 1u << 1,
    RDNA4_RADV_DARWIN_LIMITS_NO_PIPELINE_UUID = 1u << 2,
};

enum rdna4_radv_darwin_argument_limit_valid_flags {
    RDNA4_RADV_DARWIN_LIMITS_VALID_STABLE_CORE = 1u << 0,
    RDNA4_RADV_DARWIN_LIMITS_VALID_MEMORY = 1u << 1,
};

typedef struct rdna4_radv_darwin_host_facts_v1 {
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
} rdna4_radv_darwin_host_facts_v1;

typedef struct rdna4_radv_darwin_argument_limits_v1 {
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
} rdna4_radv_darwin_argument_limits_v1;

_Static_assert(sizeof(rdna4_radv_darwin_host_facts_v1) ==
                   RDNA4_ARGUMENT_LIMIT_FACTS_SIZE,
               "RADV Darwin host facts ABI mismatch");
_Static_assert(sizeof(rdna4_radv_darwin_argument_limits_v1) ==
                   RDNA4_ARGUMENT_LIMIT_RAW_SIZE,
               "RADV Darwin argument limits ABI mismatch");

typedef enum {
    RDNA4_ARGUMENT_LIMIT_STATE_UNINITIALIZED = 0,
    RDNA4_ARGUMENT_LIMIT_STATE_READY,
    RDNA4_ARGUMENT_LIMIT_STATE_UNAVAILABLE,
} rdna4_argument_limit_state;

typedef enum {
    RDNA4_ARGUMENT_LIMIT_RESULT_OK = 0,
    RDNA4_ARGUMENT_LIMIT_RESULT_INVALID_ARGUMENT,
    RDNA4_ARGUMENT_LIMIT_RESULT_LIBRARY_OPEN_FAILED,
    RDNA4_ARGUMENT_LIMIT_RESULT_EXPORT_MISSING,
    RDNA4_ARGUMENT_LIMIT_RESULT_EXPORT_CALL_FAILED,
    RDNA4_ARGUMENT_LIMIT_RESULT_EXPORT_ABI_MISMATCH,
    RDNA4_ARGUMENT_LIMIT_RESULT_EXPORT_MALFORMED,
    RDNA4_ARGUMENT_LIMIT_RESULT_POLICY_INVALID,
    RDNA4_ARGUMENT_LIMIT_RESULT_CAPACITY_UNAVAILABLE,
    RDNA4_ARGUMENT_LIMIT_RESULT_ALREADY_FIXED,
    RDNA4_ARGUMENT_LIMIT_RESULT_CROSSCHECK_UNAVAILABLE,
    RDNA4_ARGUMENT_LIMIT_RESULT_CROSSCHECK_IDENTITY_MISMATCH,
    RDNA4_ARGUMENT_LIMIT_RESULT_CROSSCHECK_LIMIT_MISMATCH,
    RDNA4_ARGUMENT_LIMIT_RESULT_SOURCE_CHANGED,
} rdna4_argument_limit_result;

typedef struct {
    /*
     * The policy is immutable with the source snapshot.  comparison_samplers
     * includes the ordinary/comparison dual-use descriptor region.  The
     * object counts reserve dummy/static/internal objects and charge at most
     * three backend handles for each unique public configuration.
     */
    uint32_t comparison_samplers;
    uint32_t reserve_sampler_objects;
    uint32_t objects_per_configuration;
} rdna4_argument_limit_policy;

/*
 * Runtime accounting paired with one frozen snapshot.
 *
 * reserved_objects is the sum of objects_per_configuration for every live
 * public argument configuration. reserved_actual_objects is the subset that
 * has materialized as live VkSampler handles. Therefore:
 *
 *   committed = live_actual_objects +
 *               (reserved_objects - reserved_actual_objects)
 *
 * Admission tests the committed value. Materializing a reserved handle moves
 * one object from the unused side to the actual side without increasing the
 * commitment. Callers serialize these counters and the live object count.
 */
typedef struct {
    uint64_t reserved_objects;
    uint64_t reserved_actual_objects;
} rdna4_argument_sampler_budget;

typedef struct {
    void *context;
    void *(*open_library)(void *context, const char *path);
    void (*close_library)(void *context, void *handle);
    void *(*resolve_symbol)(void *context, void *handle, const char *name);
} rdna4_argument_limit_loader;

typedef int (*rdna4_argument_limit_export_fn)(
    const rdna4_radv_darwin_host_facts_v1 *facts,
    rdna4_radv_darwin_argument_limits_v1 *out);

typedef struct {
    uint32_t struct_size;
    rdna4_argument_limit_state state;
    rdna4_argument_limit_result init_result;
    uint32_t initialized;

    uint32_t source_abi_version;
    uint32_t export_present;
    int export_result;
    int export_errno;
    uint32_t result_struct_size;
    uint32_t result_abi_version;
    uint32_t result_flags;
    uint32_t result_valid_flags;
    char build_identity[64];
    char loader_error[RDNA4_ARGUMENT_LIMIT_LOADER_ERROR_SIZE];

    char source_path[RDNA4_ARGUMENT_LIMIT_SOURCE_PATH_SIZE];
    uint32_t source_stat_valid;
    uint64_t source_device;
    uint64_t source_inode;
    uint64_t source_size;
    uint64_t source_mtime_seconds;
    uint64_t source_mtime_nanoseconds;

    uint32_t facts_present;
    rdna4_radv_darwin_host_facts_v1 input_facts;
    rdna4_radv_darwin_argument_limits_v1 raw;

    rdna4_argument_limit_policy policy;
    uint64_t descriptor_capacity;
    rdna4_argument_public_sampler_plan sampler_plan;
    uint32_t public_sampler_limit;
    uint32_t public_sampler_limit_available;
} rdna4_argument_limit_snapshot;

enum rdna4_argument_limit_open_observation_flags {
    RDNA4_ARGUMENT_LIMIT_OBSERVED_API_IDENTITY = 1u << 0,
    RDNA4_ARGUMENT_LIMIT_OBSERVED_DESCRIPTOR_LIMITS = 1u << 1,
    RDNA4_ARGUMENT_LIMIT_OBSERVED_FRAGMENT_LIMITS = 1u << 2,
    RDNA4_ARGUMENT_LIMIT_OBSERVED_MEMORY_LIMITS = 1u << 3,
};

#define RDNA4_ARGUMENT_LIMIT_OBSERVED_CORE_MASK \
    (RDNA4_ARGUMENT_LIMIT_OBSERVED_API_IDENTITY | \
     RDNA4_ARGUMENT_LIMIT_OBSERVED_DESCRIPTOR_LIMITS | \
     RDNA4_ARGUMENT_LIMIT_OBSERVED_FRAGMENT_LIMITS)

/*
 * A caller fills this from VkPhysicalDeviceProperties,
 * VkPhysicalDeviceVulkan12Properties and maintenance3 after the lazy open.
 * Passing these values into a pure comparison keeps Vulkan out of the
 * snapshot implementation and proves the frozen values are not updated.
 */
typedef struct {
    uint32_t struct_size;
    uint32_t observed_flags;

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
} rdna4_argument_limit_open_observation;

rdna4_argument_limit_policy rdna4_argument_limit_default_policy(void);

/*
 * Every init entry point freezes exactly one attempt in `snapshot`.  A second
 * call returns ALREADY_FIXED without changing any field.  On any source or
 * ABI failure the state remains UNAVAILABLE while the diagnostic fields and
 * any received raw bytes remain available for logging.
 */
rdna4_argument_limit_result rdna4_argument_limit_snapshot_init_from_path(
    rdna4_argument_limit_snapshot *snapshot, const char *path,
    const rdna4_radv_darwin_host_facts_v1 *facts,
    const rdna4_argument_limit_policy *policy);
rdna4_argument_limit_result rdna4_argument_limit_snapshot_init_from_handle(
    rdna4_argument_limit_snapshot *snapshot, void *handle, const char *path,
    const rdna4_radv_darwin_host_facts_v1 *facts,
    const rdna4_argument_limit_policy *policy);
rdna4_argument_limit_result rdna4_argument_limit_snapshot_init_from_loader(
    rdna4_argument_limit_snapshot *snapshot,
    const rdna4_argument_limit_loader *loader, const char *path,
    const rdna4_radv_darwin_host_facts_v1 *facts,
    const rdna4_argument_limit_policy *policy);
rdna4_argument_limit_result rdna4_argument_limit_snapshot_init_from_export(
    rdna4_argument_limit_snapshot *snapshot,
    rdna4_argument_limit_export_fn export_fn,
    const rdna4_radv_darwin_host_facts_v1 *facts,
    const rdna4_argument_limit_policy *policy);

/*
 * Pure reads.  They never call the loader, open RADV, or consume a live
 * object count.  `out_limit` and `admitted` are untouched on failure.
 */
rdna4_argument_limit_result rdna4_argument_limit_snapshot_public_limit(
    const rdna4_argument_limit_snapshot *snapshot, uint32_t *out_limit);
rdna4_argument_public_sampler_status rdna4_argument_limit_snapshot_admit(
    const rdna4_argument_limit_snapshot *snapshot,
    uint32_t live_unique_configurations, int new_unique_configuration,
    uint64_t live_sampler_objects, uint64_t requested_objects, int *admitted);
rdna4_argument_public_sampler_status
rdna4_argument_limit_snapshot_budget_committed(
    const rdna4_argument_sampler_budget *budget,
    uint64_t live_actual_objects, uint64_t *committed);
rdna4_argument_public_sampler_status
rdna4_argument_limit_snapshot_budget_reserve_configuration(
    const rdna4_argument_limit_snapshot *snapshot,
    rdna4_argument_sampler_budget *budget,
    uint32_t live_unique_configurations, uint32_t existing_actual_handles,
    uint64_t live_actual_objects, int *admitted);
rdna4_argument_public_sampler_status
rdna4_argument_limit_snapshot_budget_charge_reserved(
    const rdna4_argument_limit_snapshot *snapshot,
    rdna4_argument_sampler_budget *budget, int *charged);
rdna4_argument_public_sampler_status
rdna4_argument_limit_snapshot_budget_release_reserved(
    const rdna4_argument_limit_snapshot *snapshot,
    rdna4_argument_sampler_budget *budget, int *released);
rdna4_argument_public_sampler_status
rdna4_argument_limit_snapshot_budget_release_configuration(
    const rdna4_argument_limit_snapshot *snapshot,
    rdna4_argument_sampler_budget *budget, uint32_t actual_handles,
    int *released);
rdna4_argument_public_sampler_status
rdna4_argument_limit_snapshot_budget_can_charge_direct(
    const rdna4_argument_limit_snapshot *snapshot,
    const rdna4_argument_sampler_budget *budget,
    uint64_t live_actual_objects, uint64_t requested_objects,
    int *admitted);

/*
 * Compare the original snapshot with live full-open values.  The snapshot is
 * const and is never repaired or replaced.  A source-path comparison is
 * separate so it does not depend on Vulkan types.
 */
rdna4_argument_limit_result rdna4_argument_limit_snapshot_crosscheck(
    const rdna4_argument_limit_snapshot *snapshot,
    const rdna4_argument_limit_open_observation *observation);
rdna4_argument_limit_result rdna4_argument_limit_snapshot_source_matches_path(
    const rdna4_argument_limit_snapshot *snapshot, const char *path);

#ifdef __cplusplus
}
#endif

#endif
