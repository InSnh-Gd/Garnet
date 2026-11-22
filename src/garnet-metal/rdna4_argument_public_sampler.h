#ifndef RDNA4_ARGUMENT_PUBLIC_SAMPLER_H
#define RDNA4_ARGUMENT_PUBLIC_SAMPLER_H

/*
 * Pure arithmetic for MTLDevice.maxArgumentBufferSamplerCount and admission,
 * once an immutable limit snapshot is available.
 *
 * The public count is based on unique normalized configurations, not wrapper
 * objects. ABI8 pixel configurations can require three backend VkSampler
 * objects (normal, normalizedNormal, comparison), so the caller supplies the
 * tested worst-case objects_per_configuration rather than this helper
 * assuming two. There is deliberately no pre-open numeric fallback: an
 * unavailable source must not be turned into a published limit that changes
 * after the lazy RADV open.
 */

#include <stdint.h>

#include "rdna4_argument_capacity.h"

typedef enum {
    RDNA4_ARGUMENT_PUBLIC_SAMPLER_OK = 0,
    RDNA4_ARGUMENT_PUBLIC_SAMPLER_INVALID,
    RDNA4_ARGUMENT_PUBLIC_SAMPLER_LIMIT_UNAVAILABLE,
    RDNA4_ARGUMENT_PUBLIC_SAMPLER_OVERFLOW,
    RDNA4_ARGUMENT_PUBLIC_SAMPLER_NO_CONFIGURATIONS,
    RDNA4_ARGUMENT_PUBLIC_SAMPLER_AT_LIMIT,
    RDNA4_ARGUMENT_PUBLIC_SAMPLER_OBJECT_LIMIT,
} rdna4_argument_public_sampler_status;

typedef struct {
    uint32_t table_limit;
    uint32_t object_limit;
    uint32_t public_limit;
} rdna4_argument_public_sampler_plan;

/*
 * reserve_sampler_objects is supplied by the owner policy. It includes dummy
 * objects and any fixed object headroom that must not be consumed by public
 * configurations. It is not the current live configuration count. A backend
 * object shared by a public configuration, direct binding or static sampler
 * must be counted once, not once per use.
 */
static inline rdna4_argument_public_sampler_status
rdna4_argument_public_sampler_limit(
    int comparison_samplers, uint64_t descriptor_capacity,
    uint64_t max_sampler_allocations, uint64_t reserve_sampler_objects,
    unsigned objects_per_configuration,
    rdna4_argument_public_sampler_plan *plan) {
    if (!plan || (comparison_samplers != 0 && comparison_samplers != 1) ||
        !objects_per_configuration)
        return RDNA4_ARGUMENT_PUBLIC_SAMPLER_INVALID;
    *plan = (rdna4_argument_public_sampler_plan){0};
    if (!max_sampler_allocations)
        return RDNA4_ARGUMENT_PUBLIC_SAMPLER_LIMIT_UNAVAILABLE;
    if (!descriptor_capacity)
        return RDNA4_ARGUMENT_PUBLIC_SAMPLER_LIMIT_UNAVAILABLE;
    if (descriptor_capacity > UINT32_MAX)
        return RDNA4_ARGUMENT_PUBLIC_SAMPLER_OVERFLOW;

    uint32_t table_limit = 0;
    if (!rdna4_argument_capacity_token_limit(
            2u, (uint32_t)descriptor_capacity, 0, comparison_samplers,
            &table_limit))
        return RDNA4_ARGUMENT_PUBLIC_SAMPLER_NO_CONFIGURATIONS;

    uint64_t object_limit = max_sampler_allocations > reserve_sampler_objects
        ? (max_sampler_allocations - reserve_sampler_objects) /
              objects_per_configuration
        : 0;
    if (object_limit > UINT32_MAX) object_limit = UINT32_MAX;
    plan->table_limit = table_limit;
    plan->object_limit = (uint32_t)object_limit;
    plan->public_limit =
        table_limit < object_limit ? table_limit : (uint32_t)object_limit;
    return RDNA4_ARGUMENT_PUBLIC_SAMPLER_OK;
}

/*
 * A reused normalized configuration does not consume another public slot.
 * requested_objects covers only backend objects not already live.
 */
static inline rdna4_argument_public_sampler_status
rdna4_argument_public_sampler_admit(
    uint32_t public_limit, uint32_t live_unique_configurations,
    int new_unique_configuration, uint64_t live_sampler_objects,
    uint64_t requested_objects, uint64_t max_sampler_allocations,
    int *admitted) {
    if ((new_unique_configuration != 0 && new_unique_configuration != 1) ||
        !admitted || !max_sampler_allocations)
        return RDNA4_ARGUMENT_PUBLIC_SAMPLER_INVALID;
    *admitted = 0;
    if (new_unique_configuration &&
        live_unique_configurations >= public_limit)
        return RDNA4_ARGUMENT_PUBLIC_SAMPLER_AT_LIMIT;
    if (live_sampler_objects > max_sampler_allocations ||
        requested_objects >
            max_sampler_allocations - live_sampler_objects)
        return RDNA4_ARGUMENT_PUBLIC_SAMPLER_OBJECT_LIMIT;
    *admitted = 1;
    return RDNA4_ARGUMENT_PUBLIC_SAMPLER_OK;
}

#endif
