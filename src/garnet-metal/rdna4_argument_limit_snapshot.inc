#ifndef RDNA4_ARGUMENT_LIMIT_SNAPSHOT_INC
#define RDNA4_ARGUMENT_LIMIT_SNAPSHOT_INC

#include "rdna4_argument_limit_snapshot.h"

#include <dlfcn.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

/*
 * st_mtime is the portable POSIX spelling on both Darwin and Linux.  Apple
 * clang exposes the same seconds through st_mtimespec, but strict C modes and
 * cross-sysroot checks do not guarantee that member name.  The inode, size and
 * same-handle re-export carry the identity proof; seconds are a metadata
 * backstop, so resetting the nanosecond component keeps this source portable.
 */
#define RDNA4_ARGUMENT_LIMIT_MTIME_SEC(st) ((st).st_mtime)
#define RDNA4_ARGUMENT_LIMIT_MTIME_NSEC(st) (0)

static void rdna4_argument_limit_copy_string(char *dst, size_t capacity,
                                             const char *src) {
    if (!dst || !capacity) return;
    if (!src) {
        dst[0] = '\0';
        return;
    }
    snprintf(dst, capacity, "%s", src);
}

static void rdna4_argument_limit_record_source(
    rdna4_argument_limit_snapshot *snapshot, const char *path) {
    if (!snapshot || !path || !path[0] || strlen(path) >= sizeof(snapshot->source_path))
        return;
    rdna4_argument_limit_copy_string(snapshot->source_path,
                                     sizeof(snapshot->source_path), path);
    struct stat st;
    if (stat(path, &st) != 0) return;
    snapshot->source_stat_valid = 1;
    snapshot->source_device = (uint64_t)(uintmax_t)st.st_dev;
    snapshot->source_inode = (uint64_t)(uintmax_t)st.st_ino;
    snapshot->source_size = (uint64_t)(uintmax_t)st.st_size;
    snapshot->source_mtime_seconds =
        (uint64_t)RDNA4_ARGUMENT_LIMIT_MTIME_SEC(st);
    snapshot->source_mtime_nanoseconds =
        (uint64_t)RDNA4_ARGUMENT_LIMIT_MTIME_NSEC(st);
}

static int rdna4_argument_limit_policy_valid(
    const rdna4_argument_limit_policy *policy) {
    if (!policy ||
        (policy->comparison_samplers != 0 &&
         policy->comparison_samplers != 1) ||
        policy->objects_per_configuration == 0 ||
        policy->objects_per_configuration >
            RDNA4_ARGUMENT_LIMIT_DEFAULT_OBJECTS_PER_CONFIG)
        return 0;
    const uint32_t required_dummy =
        policy->comparison_samplers
            ? RDNA4_ARGUMENT_LIMIT_DEFAULT_DUMMY_OBJECTS
            : 1u;
    return policy->reserve_sampler_objects >= required_dummy;
}

static int rdna4_argument_limit_raw_valid(
    const rdna4_radv_darwin_argument_limits_v1 *raw,
    const rdna4_radv_darwin_host_facts_v1 *facts) {
    const uint32_t known_flags =
        RDNA4_RADV_DARWIN_LIMITS_READ_ONLY |
        RDNA4_RADV_DARWIN_LIMITS_NO_VULKAN_OBJECT |
        RDNA4_RADV_DARWIN_LIMITS_NO_PIPELINE_UUID;
    const uint32_t required_flags = known_flags;
    const uint32_t known_valid =
        RDNA4_RADV_DARWIN_LIMITS_VALID_STABLE_CORE |
        RDNA4_RADV_DARWIN_LIMITS_VALID_MEMORY;
    if (!raw || raw->struct_size != RDNA4_ARGUMENT_LIMIT_RAW_SIZE ||
        raw->abi_version != RDNA4_ARGUMENT_LIMIT_SOURCE_ABI ||
        raw->flags != required_flags || (raw->flags & ~known_flags) ||
        !(raw->valid_flags & RDNA4_RADV_DARWIN_LIMITS_VALID_STABLE_CORE) ||
        (raw->valid_flags & ~known_valid))
        return 0;
    if (!raw->build_identity[0] ||
        !memchr(raw->build_identity, '\0', sizeof(raw->build_identity)) ||
        strncmp(raw->build_identity, "rdal-v1 ", 8) != 0 ||
        raw->build_identity[8] == '\0')
        return 0;
    if (!raw->api_version || !raw->driver_version || !raw->driver_id ||
        !raw->max_descriptor_set_size || !raw->max_sampler_allocations ||
        raw->max_bound_descriptor_sets < 5u ||
        !raw->max_per_set_descriptors || !raw->max_storage_buffer_range ||
        !raw->max_uab_descriptors_all_pools ||
        !raw->max_per_stage_descriptor_update_after_bind_samplers ||
        !raw->max_descriptor_set_update_after_bind_samplers ||
        !raw->max_update_after_bind_descriptors_in_all_pools)
        return 0;

    const int wants_memory =
        facts && (facts->valid_flags & RDNA4_RADV_DARWIN_FACT_VRAM_BYTES);
    const int has_memory =
        (raw->valid_flags & RDNA4_RADV_DARWIN_LIMITS_VALID_MEMORY) != 0;
    if (has_memory != wants_memory) return 0;
    if (has_memory &&
        (!raw->max_memory_allocation_size || !raw->max_buffer_size))
        return 0;
    if (!has_memory &&
        (raw->max_memory_allocation_size || raw->max_buffer_size))
        return 0;
    return 1;
}

static rdna4_argument_limit_result
rdna4_argument_limit_begin(rdna4_argument_limit_snapshot *snapshot,
                           const char *path,
                           const rdna4_radv_darwin_host_facts_v1 *facts,
                           const rdna4_argument_limit_policy *policy) {
    if (!snapshot) return RDNA4_ARGUMENT_LIMIT_RESULT_INVALID_ARGUMENT;
    if (snapshot->initialized)
        return RDNA4_ARGUMENT_LIMIT_RESULT_ALREADY_FIXED;

    memset(snapshot, 0, sizeof(*snapshot));
    snapshot->struct_size = sizeof(*snapshot);
    snapshot->source_abi_version = RDNA4_ARGUMENT_LIMIT_SOURCE_ABI;
    snapshot->state = RDNA4_ARGUMENT_LIMIT_STATE_UNAVAILABLE;
    snapshot->initialized = 1;
    snapshot->policy = policy ? *policy
                              : rdna4_argument_limit_default_policy();
    if (facts) {
        snapshot->facts_present = 1;
        snapshot->input_facts = *facts;
    }
    rdna4_argument_limit_record_source(snapshot, path);
    if (!rdna4_argument_limit_policy_valid(&snapshot->policy)) {
        snapshot->init_result = RDNA4_ARGUMENT_LIMIT_RESULT_POLICY_INVALID;
        return snapshot->init_result;
    }
    return RDNA4_ARGUMENT_LIMIT_RESULT_OK;
}

static rdna4_argument_limit_result
rdna4_argument_limit_finish_export(
    rdna4_argument_limit_snapshot *snapshot, rdna4_argument_limit_export_fn fn,
    const rdna4_radv_darwin_host_facts_v1 *facts) {
    if (!fn) {
        snapshot->init_result = RDNA4_ARGUMENT_LIMIT_RESULT_EXPORT_MISSING;
        return snapshot->init_result;
    }
    snapshot->export_present = 1;
    memset(&snapshot->raw, 0, sizeof(snapshot->raw));
    snapshot->export_result = fn(facts, &snapshot->raw);
    snapshot->export_errno = errno;
    if (snapshot->export_result != 0) {
        snapshot->init_result =
            RDNA4_ARGUMENT_LIMIT_RESULT_EXPORT_CALL_FAILED;
        return snapshot->init_result;
    }
    snapshot->result_struct_size = snapshot->raw.struct_size;
    snapshot->result_abi_version = snapshot->raw.abi_version;
    snapshot->result_flags = snapshot->raw.flags;
    snapshot->result_valid_flags = snapshot->raw.valid_flags;
    memcpy(snapshot->build_identity, snapshot->raw.build_identity,
           sizeof(snapshot->build_identity));
    if (snapshot->raw.abi_version != RDNA4_ARGUMENT_LIMIT_SOURCE_ABI) {
        snapshot->init_result =
            RDNA4_ARGUMENT_LIMIT_RESULT_EXPORT_ABI_MISMATCH;
        return snapshot->init_result;
    }
    if (!rdna4_argument_limit_raw_valid(&snapshot->raw, facts)) {
        snapshot->init_result = RDNA4_ARGUMENT_LIMIT_RESULT_EXPORT_MALFORMED;
        return snapshot->init_result;
    }

    snapshot->descriptor_capacity =
        snapshot->raw.max_descriptor_set_update_after_bind_samplers <
                snapshot->raw.max_per_stage_descriptor_update_after_bind_samplers
            ? snapshot->raw.max_descriptor_set_update_after_bind_samplers
            : snapshot->raw.max_per_stage_descriptor_update_after_bind_samplers;
    const rdna4_argument_public_sampler_status plan_status =
        rdna4_argument_public_sampler_limit(
            (int)snapshot->policy.comparison_samplers,
            snapshot->descriptor_capacity,
            snapshot->raw.max_sampler_allocations,
            snapshot->policy.reserve_sampler_objects,
            snapshot->policy.objects_per_configuration,
            &snapshot->sampler_plan);
    if (plan_status == RDNA4_ARGUMENT_PUBLIC_SAMPLER_OVERFLOW) {
        snapshot->init_result = RDNA4_ARGUMENT_LIMIT_RESULT_EXPORT_MALFORMED;
        return snapshot->init_result;
    }
    if (plan_status != RDNA4_ARGUMENT_PUBLIC_SAMPLER_OK) {
        snapshot->init_result =
            RDNA4_ARGUMENT_LIMIT_RESULT_CAPACITY_UNAVAILABLE;
        return snapshot->init_result;
    }

    snapshot->public_sampler_limit = snapshot->sampler_plan.public_limit;
    snapshot->public_sampler_limit_available = 1;
    snapshot->state = RDNA4_ARGUMENT_LIMIT_STATE_READY;
    snapshot->init_result = RDNA4_ARGUMENT_LIMIT_RESULT_OK;
    return snapshot->init_result;
}

static rdna4_argument_limit_result
rdna4_argument_limit_finish_handle(
    rdna4_argument_limit_snapshot *snapshot, void *handle,
    const rdna4_radv_darwin_host_facts_v1 *facts) {
    if (!handle) {
        snapshot->init_result =
            RDNA4_ARGUMENT_LIMIT_RESULT_LIBRARY_OPEN_FAILED;
        return snapshot->init_result;
    }

    void *symbol = NULL;
    dlerror();
    symbol = dlsym(handle, RDNA4_ARGUMENT_LIMIT_EXPORT_NAME);
    const char *error = dlerror();
    if (error) {
        rdna4_argument_limit_copy_string(
            snapshot->loader_error, sizeof(snapshot->loader_error), error);
        snapshot->init_result = RDNA4_ARGUMENT_LIMIT_RESULT_EXPORT_MISSING;
        return snapshot->init_result;
    }
    if (!symbol) {
        snapshot->init_result = RDNA4_ARGUMENT_LIMIT_RESULT_EXPORT_MISSING;
        return snapshot->init_result;
    }
    rdna4_argument_limit_export_fn fn = NULL;
    _Static_assert(sizeof(fn) == sizeof(symbol),
                   "POSIX function and object pointers differ");
    memcpy(&fn, &symbol, sizeof(fn));
    return rdna4_argument_limit_finish_export(snapshot, fn, facts);
}

rdna4_argument_limit_policy rdna4_argument_limit_default_policy(void) {
    return (rdna4_argument_limit_policy){
        .comparison_samplers = 1,
        .reserve_sampler_objects =
            RDNA4_ARGUMENT_LIMIT_DEFAULT_DUMMY_OBJECTS,
        .objects_per_configuration =
            RDNA4_ARGUMENT_LIMIT_DEFAULT_OBJECTS_PER_CONFIG,
    };
}

rdna4_argument_limit_result rdna4_argument_limit_snapshot_init_from_path(
    rdna4_argument_limit_snapshot *snapshot, const char *path,
    const rdna4_radv_darwin_host_facts_v1 *facts,
    const rdna4_argument_limit_policy *policy) {
    if (!snapshot) return RDNA4_ARGUMENT_LIMIT_RESULT_INVALID_ARGUMENT;
    if (snapshot->initialized)
        return RDNA4_ARGUMENT_LIMIT_RESULT_ALREADY_FIXED;
    if (!path || !path[0]) {
        rdna4_argument_limit_snapshot staged;
        memset(&staged, 0, sizeof(staged));
        rdna4_argument_limit_begin(&staged, path, facts, policy);
        staged.init_result = RDNA4_ARGUMENT_LIMIT_RESULT_INVALID_ARGUMENT;
        *snapshot = staged;
        return staged.init_result;
    }

    /*
     * Keep a private copy of the policy so the loader path and explicit
     * export path share one immutability rule.
     */
    rdna4_argument_limit_snapshot staged;
    memset(&staged, 0, sizeof(staged));
    rdna4_argument_limit_result result =
        rdna4_argument_limit_begin(&staged, path, facts, policy);
    if (result != RDNA4_ARGUMENT_LIMIT_RESULT_OK) {
        *snapshot = staged;
        return result;
    }

    dlerror();
    void *handle = dlopen(path, RTLD_NOW | RTLD_LOCAL);
    const char *error = handle ? NULL : dlerror();
    if (!handle) {
        rdna4_argument_limit_copy_string(
            staged.loader_error, sizeof(staged.loader_error), error);
        staged.init_result = RDNA4_ARGUMENT_LIMIT_RESULT_LIBRARY_OPEN_FAILED;
        *snapshot = staged;
        return staged.init_result;
    }
    result = rdna4_argument_limit_finish_handle(&staged, handle, facts);
    dlclose(handle);
    *snapshot = staged;
    return result;
}

rdna4_argument_limit_result rdna4_argument_limit_snapshot_init_from_handle(
    rdna4_argument_limit_snapshot *snapshot, void *handle, const char *path,
    const rdna4_radv_darwin_host_facts_v1 *facts,
    const rdna4_argument_limit_policy *policy) {
    if (!snapshot) return RDNA4_ARGUMENT_LIMIT_RESULT_INVALID_ARGUMENT;
    if (snapshot->initialized)
        return RDNA4_ARGUMENT_LIMIT_RESULT_ALREADY_FIXED;
    rdna4_argument_limit_snapshot staged;
    memset(&staged, 0, sizeof(staged));
    rdna4_argument_limit_result result =
        rdna4_argument_limit_begin(&staged, path, facts, policy);
    if (result == RDNA4_ARGUMENT_LIMIT_RESULT_OK)
        result = rdna4_argument_limit_finish_handle(&staged, handle, facts);
    *snapshot = staged;
    return result;
}

rdna4_argument_limit_result rdna4_argument_limit_snapshot_init_from_loader(
    rdna4_argument_limit_snapshot *snapshot,
    const rdna4_argument_limit_loader *loader, const char *path,
    const rdna4_radv_darwin_host_facts_v1 *facts,
    const rdna4_argument_limit_policy *policy) {
    if (!snapshot) return RDNA4_ARGUMENT_LIMIT_RESULT_INVALID_ARGUMENT;
    if (snapshot->initialized)
        return RDNA4_ARGUMENT_LIMIT_RESULT_ALREADY_FIXED;
    if (!loader || !loader->open_library || !loader->close_library ||
        !loader->resolve_symbol || !path || !path[0]) {
        rdna4_argument_limit_snapshot staged;
        memset(&staged, 0, sizeof(staged));
        rdna4_argument_limit_begin(&staged, path, facts, policy);
        staged.init_result = RDNA4_ARGUMENT_LIMIT_RESULT_INVALID_ARGUMENT;
        *snapshot = staged;
        return staged.init_result;
    }

    rdna4_argument_limit_snapshot staged;
    memset(&staged, 0, sizeof(staged));
    rdna4_argument_limit_result result =
        rdna4_argument_limit_begin(&staged, path, facts, policy);
    if (result != RDNA4_ARGUMENT_LIMIT_RESULT_OK) {
        *snapshot = staged;
        return result;
    }

    void *handle = loader->open_library(loader->context, path);
    if (!handle) {
        staged.init_result =
            RDNA4_ARGUMENT_LIMIT_RESULT_LIBRARY_OPEN_FAILED;
        *snapshot = staged;
        return staged.init_result;
    }
    void *symbol = loader->resolve_symbol(loader->context, handle,
                                          RDNA4_ARGUMENT_LIMIT_EXPORT_NAME);
    if (!symbol) {
        staged.init_result = RDNA4_ARGUMENT_LIMIT_RESULT_EXPORT_MISSING;
    } else {
        rdna4_argument_limit_export_fn fn = NULL;
        _Static_assert(sizeof(fn) == sizeof(symbol),
                       "POSIX function and object pointers differ");
        memcpy(&fn, &symbol, sizeof(fn));
        result = rdna4_argument_limit_finish_export(&staged, fn, facts);
        if (result != RDNA4_ARGUMENT_LIMIT_RESULT_OK)
            staged.init_result = result;
    }
    loader->close_library(loader->context, handle);
    *snapshot = staged;
    return staged.init_result;
}

rdna4_argument_limit_result rdna4_argument_limit_snapshot_init_from_export(
    rdna4_argument_limit_snapshot *snapshot,
    rdna4_argument_limit_export_fn export_fn,
    const rdna4_radv_darwin_host_facts_v1 *facts,
    const rdna4_argument_limit_policy *policy) {
    if (!snapshot) return RDNA4_ARGUMENT_LIMIT_RESULT_INVALID_ARGUMENT;
    if (snapshot->initialized)
        return RDNA4_ARGUMENT_LIMIT_RESULT_ALREADY_FIXED;
    rdna4_argument_limit_snapshot staged;
    memset(&staged, 0, sizeof(staged));
    rdna4_argument_limit_result result =
        rdna4_argument_limit_begin(&staged, NULL, facts, policy);
    if (result == RDNA4_ARGUMENT_LIMIT_RESULT_OK)
        result = rdna4_argument_limit_finish_export(&staged, export_fn, facts);
    *snapshot = staged;
    return result;
}

rdna4_argument_limit_result rdna4_argument_limit_snapshot_public_limit(
    const rdna4_argument_limit_snapshot *snapshot, uint32_t *out_limit) {
    if (!snapshot || !out_limit)
        return RDNA4_ARGUMENT_LIMIT_RESULT_INVALID_ARGUMENT;
    if (snapshot->state != RDNA4_ARGUMENT_LIMIT_STATE_READY ||
        !snapshot->public_sampler_limit_available)
        return RDNA4_ARGUMENT_LIMIT_RESULT_CAPACITY_UNAVAILABLE;
    *out_limit = snapshot->public_sampler_limit;
    return RDNA4_ARGUMENT_LIMIT_RESULT_OK;
}

rdna4_argument_public_sampler_status rdna4_argument_limit_snapshot_admit(
    const rdna4_argument_limit_snapshot *snapshot,
    uint32_t live_unique_configurations, int new_unique_configuration,
    uint64_t live_sampler_objects, uint64_t requested_objects, int *admitted) {
    if (!snapshot || !admitted)
        return RDNA4_ARGUMENT_PUBLIC_SAMPLER_INVALID;
    if (snapshot->state != RDNA4_ARGUMENT_LIMIT_STATE_READY ||
        !snapshot->public_sampler_limit_available)
        return RDNA4_ARGUMENT_PUBLIC_SAMPLER_LIMIT_UNAVAILABLE;
    return rdna4_argument_public_sampler_admit(
        snapshot->public_sampler_limit, live_unique_configurations,
        new_unique_configuration, live_sampler_objects, requested_objects,
        snapshot->raw.max_sampler_allocations, admitted);
}

rdna4_argument_public_sampler_status
rdna4_argument_limit_snapshot_budget_committed(
    const rdna4_argument_sampler_budget *budget,
    uint64_t live_actual_objects, uint64_t *committed) {
    if (!budget || !committed ||
        budget->reserved_objects < budget->reserved_actual_objects)
        return RDNA4_ARGUMENT_PUBLIC_SAMPLER_INVALID;
    const uint64_t unused =
        budget->reserved_objects - budget->reserved_actual_objects;
    if (live_actual_objects > UINT64_MAX - unused)
        return RDNA4_ARGUMENT_PUBLIC_SAMPLER_OVERFLOW;
    *committed = live_actual_objects + unused;
    return RDNA4_ARGUMENT_PUBLIC_SAMPLER_OK;
}

rdna4_argument_public_sampler_status
rdna4_argument_limit_snapshot_budget_reserve_configuration(
    const rdna4_argument_limit_snapshot *snapshot,
    rdna4_argument_sampler_budget *budget,
    uint32_t live_unique_configurations, uint32_t existing_actual_handles,
    uint64_t live_actual_objects, int *admitted) {
    if (!snapshot || !budget || !admitted)
        return RDNA4_ARGUMENT_PUBLIC_SAMPLER_INVALID;
    *admitted = 0;
    if (snapshot->state != RDNA4_ARGUMENT_LIMIT_STATE_READY ||
        !snapshot->public_sampler_limit_available)
        return RDNA4_ARGUMENT_PUBLIC_SAMPLER_LIMIT_UNAVAILABLE;
    const uint32_t objects_per_configuration =
        snapshot->policy.objects_per_configuration;
    if (!objects_per_configuration ||
        existing_actual_handles > objects_per_configuration)
        return RDNA4_ARGUMENT_PUBLIC_SAMPLER_INVALID;

    uint64_t committed = 0;
    rdna4_argument_public_sampler_status status =
        rdna4_argument_limit_snapshot_budget_committed(
            budget, live_actual_objects, &committed);
    if (status != RDNA4_ARGUMENT_PUBLIC_SAMPLER_OK) return status;
    const uint64_t requested =
        objects_per_configuration - existing_actual_handles;
    status = rdna4_argument_limit_snapshot_admit(
        snapshot, live_unique_configurations, 1, committed, requested,
        admitted);
    if (status != RDNA4_ARGUMENT_PUBLIC_SAMPLER_OK || !*admitted)
        return status;
    if (UINT64_MAX - budget->reserved_objects <
            objects_per_configuration ||
        UINT64_MAX - budget->reserved_actual_objects <
            existing_actual_handles)
        return RDNA4_ARGUMENT_PUBLIC_SAMPLER_OVERFLOW;
    budget->reserved_objects += objects_per_configuration;
    budget->reserved_actual_objects += existing_actual_handles;
    return RDNA4_ARGUMENT_PUBLIC_SAMPLER_OK;
}

rdna4_argument_public_sampler_status
rdna4_argument_limit_snapshot_budget_charge_reserved(
    const rdna4_argument_limit_snapshot *snapshot,
    rdna4_argument_sampler_budget *budget, int *charged) {
    if (!snapshot || !budget || !charged)
        return RDNA4_ARGUMENT_PUBLIC_SAMPLER_INVALID;
    *charged = 0;
    if (snapshot->state != RDNA4_ARGUMENT_LIMIT_STATE_READY ||
        !snapshot->public_sampler_limit_available)
        return RDNA4_ARGUMENT_PUBLIC_SAMPLER_LIMIT_UNAVAILABLE;
    if (budget->reserved_objects < budget->reserved_actual_objects)
        return RDNA4_ARGUMENT_PUBLIC_SAMPLER_INVALID;
    if (budget->reserved_actual_objects == budget->reserved_objects)
        return RDNA4_ARGUMENT_PUBLIC_SAMPLER_OBJECT_LIMIT;
    ++budget->reserved_actual_objects;
    *charged = 1;
    return RDNA4_ARGUMENT_PUBLIC_SAMPLER_OK;
}

rdna4_argument_public_sampler_status
rdna4_argument_limit_snapshot_budget_release_reserved(
    const rdna4_argument_limit_snapshot *snapshot,
    rdna4_argument_sampler_budget *budget, int *released) {
    if (!snapshot || !budget || !released)
        return RDNA4_ARGUMENT_PUBLIC_SAMPLER_INVALID;
    *released = 0;
    if (snapshot->state != RDNA4_ARGUMENT_LIMIT_STATE_READY ||
        !snapshot->public_sampler_limit_available)
        return RDNA4_ARGUMENT_PUBLIC_SAMPLER_LIMIT_UNAVAILABLE;
    if (!budget->reserved_actual_objects ||
        budget->reserved_objects < budget->reserved_actual_objects)
        return RDNA4_ARGUMENT_PUBLIC_SAMPLER_INVALID;
    --budget->reserved_actual_objects;
    *released = 1;
    return RDNA4_ARGUMENT_PUBLIC_SAMPLER_OK;
}

rdna4_argument_public_sampler_status
rdna4_argument_limit_snapshot_budget_release_configuration(
    const rdna4_argument_limit_snapshot *snapshot,
    rdna4_argument_sampler_budget *budget, uint32_t actual_handles,
    int *released) {
    if (!snapshot || !budget || !released)
        return RDNA4_ARGUMENT_PUBLIC_SAMPLER_INVALID;
    *released = 0;
    if (snapshot->state != RDNA4_ARGUMENT_LIMIT_STATE_READY ||
        !snapshot->public_sampler_limit_available)
        return RDNA4_ARGUMENT_PUBLIC_SAMPLER_LIMIT_UNAVAILABLE;
    const uint64_t reserved = snapshot->policy.objects_per_configuration;
    if (!reserved || actual_handles > reserved ||
        budget->reserved_objects < reserved ||
        budget->reserved_actual_objects < actual_handles)
        return RDNA4_ARGUMENT_PUBLIC_SAMPLER_INVALID;
    budget->reserved_objects -= reserved;
    budget->reserved_actual_objects -= actual_handles;
    *released = 1;
    return RDNA4_ARGUMENT_PUBLIC_SAMPLER_OK;
}

rdna4_argument_public_sampler_status
rdna4_argument_limit_snapshot_budget_can_charge_direct(
    const rdna4_argument_limit_snapshot *snapshot,
    const rdna4_argument_sampler_budget *budget,
    uint64_t live_actual_objects, uint64_t requested_objects,
    int *admitted) {
    if (!snapshot || !budget || !admitted)
        return RDNA4_ARGUMENT_PUBLIC_SAMPLER_INVALID;
    *admitted = 0;
    if (snapshot->state != RDNA4_ARGUMENT_LIMIT_STATE_READY ||
        !snapshot->public_sampler_limit_available)
        return RDNA4_ARGUMENT_PUBLIC_SAMPLER_LIMIT_UNAVAILABLE;
    uint64_t committed = 0;
    rdna4_argument_public_sampler_status status =
        rdna4_argument_limit_snapshot_budget_committed(
            budget, live_actual_objects, &committed);
    if (status != RDNA4_ARGUMENT_PUBLIC_SAMPLER_OK) return status;
    if (requested_objects > UINT64_MAX - committed)
        return RDNA4_ARGUMENT_PUBLIC_SAMPLER_OVERFLOW;
    if (committed + requested_objects >
        snapshot->raw.max_sampler_allocations)
        return RDNA4_ARGUMENT_PUBLIC_SAMPLER_OBJECT_LIMIT;
    *admitted = 1;
    return RDNA4_ARGUMENT_PUBLIC_SAMPLER_OK;
}

static int rdna4_argument_limit_core_matches(
    const rdna4_radv_darwin_argument_limits_v1 *raw,
    const rdna4_argument_limit_open_observation *o) {
#define RDNA4_ARG_LIMIT_EQ(field) (raw->field == o->field)
    return RDNA4_ARG_LIMIT_EQ(max_descriptor_set_size) &&
           RDNA4_ARG_LIMIT_EQ(max_sampler_allocations) &&
           RDNA4_ARG_LIMIT_EQ(max_bound_descriptor_sets) &&
           RDNA4_ARG_LIMIT_EQ(max_per_set_descriptors) &&
           RDNA4_ARG_LIMIT_EQ(max_storage_buffer_range) &&
           RDNA4_ARG_LIMIT_EQ(max_uab_descriptors_all_pools) &&
           RDNA4_ARG_LIMIT_EQ(max_per_stage_descriptor_samplers) &&
           RDNA4_ARG_LIMIT_EQ(max_per_stage_descriptor_uniform_buffers) &&
           RDNA4_ARG_LIMIT_EQ(max_per_stage_descriptor_storage_buffers) &&
           RDNA4_ARG_LIMIT_EQ(max_per_stage_descriptor_sampled_images) &&
           RDNA4_ARG_LIMIT_EQ(max_per_stage_descriptor_storage_images) &&
           RDNA4_ARG_LIMIT_EQ(max_per_stage_descriptor_input_attachments) &&
           RDNA4_ARG_LIMIT_EQ(max_per_stage_resources) &&
           RDNA4_ARG_LIMIT_EQ(max_descriptor_set_samplers) &&
           RDNA4_ARG_LIMIT_EQ(max_descriptor_set_uniform_buffers) &&
           RDNA4_ARG_LIMIT_EQ(max_descriptor_set_uniform_buffers_dynamic) &&
           RDNA4_ARG_LIMIT_EQ(max_descriptor_set_storage_buffers) &&
           RDNA4_ARG_LIMIT_EQ(max_descriptor_set_storage_buffers_dynamic) &&
           RDNA4_ARG_LIMIT_EQ(max_descriptor_set_sampled_images) &&
           RDNA4_ARG_LIMIT_EQ(max_descriptor_set_storage_images) &&
           RDNA4_ARG_LIMIT_EQ(max_descriptor_set_input_attachments) &&
           RDNA4_ARG_LIMIT_EQ(
               max_per_stage_descriptor_update_after_bind_samplers) &&
           RDNA4_ARG_LIMIT_EQ(
               max_per_stage_descriptor_update_after_bind_uniform_buffers) &&
           RDNA4_ARG_LIMIT_EQ(
               max_per_stage_descriptor_update_after_bind_storage_buffers) &&
           RDNA4_ARG_LIMIT_EQ(
               max_per_stage_descriptor_update_after_bind_sampled_images) &&
           RDNA4_ARG_LIMIT_EQ(
               max_per_stage_descriptor_update_after_bind_storage_images) &&
           RDNA4_ARG_LIMIT_EQ(
               max_per_stage_descriptor_update_after_bind_input_attachments) &&
           RDNA4_ARG_LIMIT_EQ(max_per_stage_update_after_bind_resources) &&
           RDNA4_ARG_LIMIT_EQ(
               max_descriptor_set_update_after_bind_samplers) &&
           RDNA4_ARG_LIMIT_EQ(
               max_descriptor_set_update_after_bind_uniform_buffers) &&
           RDNA4_ARG_LIMIT_EQ(
               max_descriptor_set_update_after_bind_uniform_buffers_dynamic) &&
           RDNA4_ARG_LIMIT_EQ(
               max_descriptor_set_update_after_bind_storage_buffers) &&
           RDNA4_ARG_LIMIT_EQ(
               max_descriptor_set_update_after_bind_storage_buffers_dynamic) &&
           RDNA4_ARG_LIMIT_EQ(
               max_descriptor_set_update_after_bind_sampled_images) &&
           RDNA4_ARG_LIMIT_EQ(
               max_descriptor_set_update_after_bind_storage_images) &&
           RDNA4_ARG_LIMIT_EQ(
               max_descriptor_set_update_after_bind_input_attachments) &&
           RDNA4_ARG_LIMIT_EQ(
               max_update_after_bind_descriptors_in_all_pools) &&
           RDNA4_ARG_LIMIT_EQ(max_fragment_combined_output_resources) &&
           RDNA4_ARG_LIMIT_EQ(max_color_attachments) &&
           RDNA4_ARG_LIMIT_EQ(max_fragment_output_attachments) &&
           RDNA4_ARG_LIMIT_EQ(max_fragment_dual_src_attachments);
#undef RDNA4_ARG_LIMIT_EQ
}

rdna4_argument_limit_result rdna4_argument_limit_snapshot_crosscheck(
    const rdna4_argument_limit_snapshot *snapshot,
    const rdna4_argument_limit_open_observation *observation) {
    if (!snapshot || !observation)
        return RDNA4_ARGUMENT_LIMIT_RESULT_INVALID_ARGUMENT;
    if (snapshot->state != RDNA4_ARGUMENT_LIMIT_STATE_READY)
        return RDNA4_ARGUMENT_LIMIT_RESULT_CROSSCHECK_UNAVAILABLE;
    if (observation->struct_size != sizeof(*observation))
        return RDNA4_ARGUMENT_LIMIT_RESULT_INVALID_ARGUMENT;
    if ((observation->observed_flags &
         RDNA4_ARGUMENT_LIMIT_OBSERVED_CORE_MASK) !=
        RDNA4_ARGUMENT_LIMIT_OBSERVED_CORE_MASK)
        return RDNA4_ARGUMENT_LIMIT_RESULT_CROSSCHECK_UNAVAILABLE;
    if (snapshot->raw.api_version != observation->api_version ||
        snapshot->raw.driver_version != observation->driver_version ||
        snapshot->raw.driver_id != observation->driver_id)
        return RDNA4_ARGUMENT_LIMIT_RESULT_CROSSCHECK_IDENTITY_MISMATCH;
    if (!rdna4_argument_limit_core_matches(&snapshot->raw, observation))
        return RDNA4_ARGUMENT_LIMIT_RESULT_CROSSCHECK_LIMIT_MISMATCH;
    if ((snapshot->raw.valid_flags &
         RDNA4_RADV_DARWIN_LIMITS_VALID_MEMORY) &&
        (!(observation->observed_flags &
           RDNA4_ARGUMENT_LIMIT_OBSERVED_MEMORY_LIMITS) ||
         snapshot->raw.max_memory_allocation_size !=
             observation->max_memory_allocation_size ||
         snapshot->raw.max_buffer_size != observation->max_buffer_size))
        return RDNA4_ARGUMENT_LIMIT_RESULT_CROSSCHECK_LIMIT_MISMATCH;
    return RDNA4_ARGUMENT_LIMIT_RESULT_OK;
}

rdna4_argument_limit_result rdna4_argument_limit_snapshot_source_matches_path(
    const rdna4_argument_limit_snapshot *snapshot, const char *path) {
    if (!snapshot) return RDNA4_ARGUMENT_LIMIT_RESULT_INVALID_ARGUMENT;
    if (!snapshot->initialized || !snapshot->source_stat_valid)
        return RDNA4_ARGUMENT_LIMIT_RESULT_CROSSCHECK_UNAVAILABLE;
    const char *use = path && path[0] ? path : snapshot->source_path;
    if (!use || !use[0])
        return RDNA4_ARGUMENT_LIMIT_RESULT_CROSSCHECK_UNAVAILABLE;
    struct stat st;
    if (stat(use, &st) != 0)
        return RDNA4_ARGUMENT_LIMIT_RESULT_CROSSCHECK_UNAVAILABLE;
    if (snapshot->source_device != (uint64_t)(uintmax_t)st.st_dev ||
        snapshot->source_inode != (uint64_t)(uintmax_t)st.st_ino ||
        snapshot->source_size != (uint64_t)(uintmax_t)st.st_size ||
        snapshot->source_mtime_seconds !=
            (uint64_t)RDNA4_ARGUMENT_LIMIT_MTIME_SEC(st) ||
        snapshot->source_mtime_nanoseconds !=
            (uint64_t)RDNA4_ARGUMENT_LIMIT_MTIME_NSEC(st))
        return RDNA4_ARGUMENT_LIMIT_RESULT_SOURCE_CHANGED;
    return RDNA4_ARGUMENT_LIMIT_RESULT_OK;
}

#endif
