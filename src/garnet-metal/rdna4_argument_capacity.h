#ifndef RDNA4_ARGUMENT_CAPACITY_H
#define RDNA4_ARGUMENT_CAPACITY_H

/*
 * Checked capacity arithmetic for the five-set GPU argument layout.
 *
 * Sets 0/1 contain the stage-local descriptors. Sets 2/3/4 contain the
 * sampled-image, storage-image and sampler arrays. Vulkan's
 * maxDescriptorSet*UpdateAfterBind limits apply to the complete pipeline
 * layout: all shader stages and all elements of pSetLayouts. The
 * maxPerStageDescriptor*UpdateAfterBind limits apply per accessible stage.
 * Non-update-after-bind layouts remain a subset of both ordinary and UAB
 * totals.
 */

#include <stdint.h>

#define RDNA4_ARGUMENT_CAPACITY_CATEGORIES 3u
#define RDNA4_ARGUMENT_CAPACITY_STAGES 2u

typedef enum {
    RDNA4_ARGUMENT_CAPACITY_OK = 0,
    RDNA4_ARGUMENT_CAPACITY_INVALID,
    RDNA4_ARGUMENT_CAPACITY_LIMIT_UNAVAILABLE,
    RDNA4_ARGUMENT_CAPACITY_OVERFLOW,
    RDNA4_ARGUMENT_CAPACITY_SET_COUNT,
    RDNA4_ARGUMENT_CAPACITY_DIRECT_OVER_LIMIT,
    RDNA4_ARGUMENT_CAPACITY_ORDINARY_DIRECT_OVER_LIMIT,
    RDNA4_ARGUMENT_CAPACITY_RESOURCE_OVER_LIMIT,
    RDNA4_ARGUMENT_CAPACITY_ORDINARY_RESOURCE_OVER_LIMIT,
    RDNA4_ARGUMENT_CAPACITY_NO_DESCRIPTORS,
} rdna4_argument_capacity_status;

typedef struct {
    /* UAB properties count layouts with and without the UAB layout flag. */
    uint64_t max_per_stage_uab[RDNA4_ARGUMENT_CAPACITY_CATEGORIES];
    uint64_t max_pipeline_uab[RDNA4_ARGUMENT_CAPACITY_CATEGORIES];
    uint64_t max_per_stage_uab_resources;
    /* Ordinary limits count only the non-UAB stage-local layouts below. */
    uint64_t max_per_stage_ordinary[RDNA4_ARGUMENT_CAPACITY_CATEGORIES];
    uint64_t max_pipeline_ordinary[RDNA4_ARGUMENT_CAPACITY_CATEGORIES];
    uint64_t max_per_stage_ordinary_resources;
    /* maintenance3's guaranteed size; zero means the device query is unavailable. */
    uint32_t max_per_set_descriptors;
    uint32_t max_bound_descriptor_sets;

    /* Direct descriptors already present in sets 0 and 1. */
    uint64_t direct[RDNA4_ARGUMENT_CAPACITY_STAGES][RDNA4_ARGUMENT_CAPACITY_CATEGORIES];
    uint64_t direct_resources[RDNA4_ARGUMENT_CAPACITY_STAGES];

    /* Fixed descriptors added beside a variable table binding. ABI9 uses
     * three update-after-bind texel-buffer slots in set2/3 binding1. */
    uint64_t fixed[RDNA4_ARGUMENT_CAPACITY_STAGES]
                  [RDNA4_ARGUMENT_CAPACITY_CATEGORIES];
    uint64_t fixed_resources[RDNA4_ARGUMENT_CAPACITY_STAGES];
    /* Shared fixed bindings count once across all visible stages. */
    uint64_t fixed_pipeline[RDNA4_ARGUMENT_CAPACITY_CATEGORIES];

    /* Bit 0 is compute/vertex, bit 1 is fragment. */
    uint32_t used_stages[RDNA4_ARGUMENT_CAPACITY_CATEGORIES];
} rdna4_argument_capacity_limits;

typedef struct {
    uint32_t capacity[RDNA4_ARGUMENT_CAPACITY_CATEGORIES];
    uint32_t failed_category;
    int above_guaranteed_limit[RDNA4_ARGUMENT_CAPACITY_CATEGORIES];
} rdna4_argument_capacity_plan;

static inline uint64_t rdna4_argument_capacity_min_u64(uint64_t a, uint64_t b) {
    return a < b ? a : b;
}

static inline int rdna4_argument_capacity_add_u64(uint64_t a, uint64_t b,
                                                  uint64_t *out) {
    if (!out || b > UINT64_MAX - a) return 0;
    *out = a + b;
    return 1;
}

#define RDNA4_ARGUMENT_DESCRIPTOR_TYPE_GROUPS 8u

typedef enum {
    RDNA4_ARGUMENT_DESCRIPTOR_GROUP_SAMPLERS = 0,
    RDNA4_ARGUMENT_DESCRIPTOR_GROUP_UNIFORM_BUFFERS,
    RDNA4_ARGUMENT_DESCRIPTOR_GROUP_UNIFORM_BUFFERS_DYNAMIC,
    RDNA4_ARGUMENT_DESCRIPTOR_GROUP_STORAGE_BUFFERS,
    RDNA4_ARGUMENT_DESCRIPTOR_GROUP_STORAGE_BUFFERS_DYNAMIC,
    RDNA4_ARGUMENT_DESCRIPTOR_GROUP_SAMPLED_IMAGES,
    RDNA4_ARGUMENT_DESCRIPTOR_GROUP_STORAGE_IMAGES,
    RDNA4_ARGUMENT_DESCRIPTOR_GROUP_INPUT_ATTACHMENTS,
} rdna4_argument_descriptor_type_group;

/*
 * Counts are per Vulkan descriptor type. A caller can use the same checker for
 * ordinary layouts by counting only non-UAB sets, or for update-after-bind
 * layouts by counting all sets. `pipeline` must count a shared set once even
 * when it is accessible to both stages.
 */
typedef struct {
    uint64_t sampler;
    uint64_t combined_image_sampler;
    uint64_t sampled_image;
    uint64_t storage_image;
    uint64_t uniform_texel_buffer;
    uint64_t storage_texel_buffer;
    uint64_t uniform_buffer;
    uint64_t uniform_buffer_dynamic;
    uint64_t storage_buffer;
    uint64_t storage_buffer_dynamic;
    uint64_t input_attachment;
} rdna4_argument_descriptor_counts;

typedef struct {
    uint64_t max_per_stage[RDNA4_ARGUMENT_CAPACITY_STAGES]
                          [RDNA4_ARGUMENT_DESCRIPTOR_TYPE_GROUPS];
    uint64_t max_pipeline[RDNA4_ARGUMENT_DESCRIPTOR_TYPE_GROUPS];
    uint64_t max_per_stage_resources;
    uint64_t max_fragment_combined_output_resources;
    uint64_t max_color_attachments;
    uint64_t max_fragment_output_attachments;
    uint64_t max_fragment_dual_src_attachments;
} rdna4_argument_descriptor_limits;

typedef struct {
    rdna4_argument_descriptor_counts per_stage[RDNA4_ARGUMENT_CAPACITY_STAGES];
    rdna4_argument_descriptor_counts pipeline;
    uint64_t fragment_color_attachments;
    uint64_t fragment_output_attachments;
    uint64_t fragment_dual_src_attachments;
} rdna4_argument_descriptor_usage;

typedef enum {
    RDNA4_ARGUMENT_DESCRIPTOR_BUDGET_OK = 0,
    RDNA4_ARGUMENT_DESCRIPTOR_BUDGET_INVALID,
    RDNA4_ARGUMENT_DESCRIPTOR_BUDGET_TYPE_OVER_LIMIT,
    RDNA4_ARGUMENT_DESCRIPTOR_BUDGET_PIPELINE_OVER_LIMIT,
    RDNA4_ARGUMENT_DESCRIPTOR_BUDGET_RESOURCE_OVER_LIMIT,
    RDNA4_ARGUMENT_DESCRIPTOR_BUDGET_FRAGMENT_COMBINED_OUTPUT_OVER_LIMIT,
    RDNA4_ARGUMENT_DESCRIPTOR_BUDGET_COLOR_ATTACHMENT_OVER_LIMIT,
    RDNA4_ARGUMENT_DESCRIPTOR_BUDGET_FRAGMENT_OUTPUT_OVER_LIMIT,
    RDNA4_ARGUMENT_DESCRIPTOR_BUDGET_DUAL_SRC_OVER_LIMIT,
    RDNA4_ARGUMENT_DESCRIPTOR_BUDGET_OVERFLOW,
} rdna4_argument_descriptor_budget_status;

typedef struct {
    uint32_t failed_stage;
    uint32_t failed_group;
} rdna4_argument_descriptor_budget_plan;

static inline int rdna4_argument_descriptor_counts_by_group(
    const rdna4_argument_descriptor_counts *counts,
    uint64_t groups[RDNA4_ARGUMENT_DESCRIPTOR_TYPE_GROUPS],
    uint64_t *resources) {
    if (!counts || !groups || !resources) return 0;

    if (!rdna4_argument_capacity_add_u64(
            counts->sampler, counts->combined_image_sampler,
            &groups[RDNA4_ARGUMENT_DESCRIPTOR_GROUP_SAMPLERS]) ||
        !rdna4_argument_capacity_add_u64(
            counts->uniform_buffer, counts->uniform_buffer_dynamic,
            &groups[RDNA4_ARGUMENT_DESCRIPTOR_GROUP_UNIFORM_BUFFERS]) ||
        !rdna4_argument_capacity_add_u64(
            counts->storage_buffer, counts->storage_buffer_dynamic,
            &groups[RDNA4_ARGUMENT_DESCRIPTOR_GROUP_STORAGE_BUFFERS]) ||
        !rdna4_argument_capacity_add_u64(
            counts->combined_image_sampler, counts->sampled_image,
            &groups[RDNA4_ARGUMENT_DESCRIPTOR_GROUP_SAMPLED_IMAGES]) ||
        !rdna4_argument_capacity_add_u64(
            groups[RDNA4_ARGUMENT_DESCRIPTOR_GROUP_SAMPLED_IMAGES],
            counts->uniform_texel_buffer,
            &groups[RDNA4_ARGUMENT_DESCRIPTOR_GROUP_SAMPLED_IMAGES]) ||
        !rdna4_argument_capacity_add_u64(
            counts->storage_image, counts->storage_texel_buffer,
            &groups[RDNA4_ARGUMENT_DESCRIPTOR_GROUP_STORAGE_IMAGES]))
        return 0;

    groups[RDNA4_ARGUMENT_DESCRIPTOR_GROUP_UNIFORM_BUFFERS_DYNAMIC] =
        counts->uniform_buffer_dynamic;
    groups[RDNA4_ARGUMENT_DESCRIPTOR_GROUP_STORAGE_BUFFERS_DYNAMIC] =
        counts->storage_buffer_dynamic;
    groups[RDNA4_ARGUMENT_DESCRIPTOR_GROUP_INPUT_ATTACHMENTS] =
        counts->input_attachment;

    /* The resource total excludes standalone samplers but counts both image
     * and buffer descriptor types exactly once. */
    uint64_t total = 0;
    if (!rdna4_argument_capacity_add_u64(
            total,
            groups[RDNA4_ARGUMENT_DESCRIPTOR_GROUP_SAMPLED_IMAGES], &total) ||
        !rdna4_argument_capacity_add_u64(
            total,
            groups[RDNA4_ARGUMENT_DESCRIPTOR_GROUP_STORAGE_IMAGES], &total) ||
        !rdna4_argument_capacity_add_u64(
            total,
            groups[RDNA4_ARGUMENT_DESCRIPTOR_GROUP_UNIFORM_BUFFERS], &total) ||
        !rdna4_argument_capacity_add_u64(
            total,
            groups[RDNA4_ARGUMENT_DESCRIPTOR_GROUP_STORAGE_BUFFERS], &total) ||
        !rdna4_argument_capacity_add_u64(
            total,
            groups[RDNA4_ARGUMENT_DESCRIPTOR_GROUP_INPUT_ATTACHMENTS],
            &total))
        return 0;
    *resources = total;
    return 1;
}

static inline rdna4_argument_descriptor_budget_status
rdna4_argument_capacity_descriptor_budget(
    const rdna4_argument_descriptor_limits *limits,
    const rdna4_argument_descriptor_usage *usage,
    rdna4_argument_descriptor_budget_plan *plan) {
    if (!limits || !usage)
        return RDNA4_ARGUMENT_DESCRIPTOR_BUDGET_INVALID;
    if (plan)
        *plan = (rdna4_argument_descriptor_budget_plan){
            .failed_stage = UINT32_MAX,
            .failed_group = UINT32_MAX,
        };

    for (unsigned stage = 0; stage < RDNA4_ARGUMENT_CAPACITY_STAGES; ++stage) {
        uint64_t groups[RDNA4_ARGUMENT_DESCRIPTOR_TYPE_GROUPS];
        uint64_t resources = 0;
        if (!rdna4_argument_descriptor_counts_by_group(
                &usage->per_stage[stage], groups, &resources))
            return RDNA4_ARGUMENT_DESCRIPTOR_BUDGET_OVERFLOW;
        for (unsigned group = 0; group < RDNA4_ARGUMENT_DESCRIPTOR_TYPE_GROUPS;
             ++group) {
            if (groups[group] > limits->max_per_stage[stage][group]) {
                if (plan) {
                    plan->failed_stage = stage;
                    plan->failed_group = group;
                }
                return RDNA4_ARGUMENT_DESCRIPTOR_BUDGET_TYPE_OVER_LIMIT;
            }
        }
        if (stage == 1 &&
            !rdna4_argument_capacity_add_u64(
                resources, usage->fragment_color_attachments, &resources))
            return RDNA4_ARGUMENT_DESCRIPTOR_BUDGET_OVERFLOW;
        if (resources > limits->max_per_stage_resources)
            return RDNA4_ARGUMENT_DESCRIPTOR_BUDGET_RESOURCE_OVER_LIMIT;
    }

    uint64_t pipeline_groups[RDNA4_ARGUMENT_DESCRIPTOR_TYPE_GROUPS];
    uint64_t pipeline_resources = 0;
    if (!rdna4_argument_descriptor_counts_by_group(
            &usage->pipeline, pipeline_groups, &pipeline_resources))
        return RDNA4_ARGUMENT_DESCRIPTOR_BUDGET_OVERFLOW;
    for (unsigned group = 0; group < RDNA4_ARGUMENT_DESCRIPTOR_TYPE_GROUPS;
         ++group) {
        if (pipeline_groups[group] > limits->max_pipeline[group]) {
            if (plan)
                plan->failed_group = group;
            return RDNA4_ARGUMENT_DESCRIPTOR_BUDGET_PIPELINE_OVER_LIMIT;
        }
    }

    uint64_t fragment_groups[RDNA4_ARGUMENT_DESCRIPTOR_TYPE_GROUPS];
    uint64_t fragment_resources = 0;
    if (!rdna4_argument_descriptor_counts_by_group(
            &usage->per_stage[1], fragment_groups, &fragment_resources))
        return RDNA4_ARGUMENT_DESCRIPTOR_BUDGET_OVERFLOW;

    uint64_t combined_outputs = 0;
    if (!rdna4_argument_capacity_add_u64(
            fragment_groups[RDNA4_ARGUMENT_DESCRIPTOR_GROUP_STORAGE_BUFFERS],
            fragment_groups[RDNA4_ARGUMENT_DESCRIPTOR_GROUP_STORAGE_IMAGES],
            &combined_outputs) ||
        !rdna4_argument_capacity_add_u64(
            combined_outputs, usage->fragment_color_attachments,
            &combined_outputs))
        return RDNA4_ARGUMENT_DESCRIPTOR_BUDGET_OVERFLOW;
    if (combined_outputs > limits->max_fragment_combined_output_resources)
        return RDNA4_ARGUMENT_DESCRIPTOR_BUDGET_FRAGMENT_COMBINED_OUTPUT_OVER_LIMIT;
    if (usage->fragment_color_attachments > limits->max_color_attachments)
        return RDNA4_ARGUMENT_DESCRIPTOR_BUDGET_COLOR_ATTACHMENT_OVER_LIMIT;
    if (usage->fragment_output_attachments >
        limits->max_fragment_output_attachments)
        return RDNA4_ARGUMENT_DESCRIPTOR_BUDGET_FRAGMENT_OUTPUT_OVER_LIMIT;
    if (usage->fragment_dual_src_attachments >
        limits->max_fragment_dual_src_attachments)
        return RDNA4_ARGUMENT_DESCRIPTOR_BUDGET_DUAL_SRC_OVER_LIMIT;

    return RDNA4_ARGUMENT_DESCRIPTOR_BUDGET_OK;
}

/* Samplers do not count against maxPerStage[UpdateAfterBind]Resources. */
static inline int rdna4_argument_capacity_counts_resources(unsigned category) {
    return category < 2u;
}

/*
 * The sampled-image tail is indexed by the highest reflected image shape, so
 * a sparse mask still needs every slot from zero through that shape.
 */
static inline int rdna4_argument_capacity_tail(uint64_t mask, uint32_t shape_limit,
                                               uint32_t *tail) {
    if (!shape_limit || shape_limit > 64u) return 0;
    if (shape_limit < 64u && (mask >> shape_limit)) return 0;
    uint32_t count = 0;
    for (uint64_t bit = mask; bit; bit >>= 1) {
        if (count == shape_limit) return 0;
        ++count;
    }
    if (tail) *tail = count;
    return 1;
}

static inline int rdna4_argument_capacity_descriptor_count(
    unsigned category, uint64_t token_count, uint64_t null_images,
    int comparison_samplers, uint32_t *descriptor_count) {
    if (category >= RDNA4_ARGUMENT_CAPACITY_CATEGORIES || !token_count ||
        !descriptor_count) return 0;
    uint64_t result = token_count;
    if (category == 0) {
        uint32_t tail = 0;
        if (!rdna4_argument_capacity_tail(null_images, 48u, &tail) ||
            !rdna4_argument_capacity_add_u64(result, tail, &result)) return 0;
    } else if (category == 2 && comparison_samplers) {
        if (token_count > UINT64_MAX / 2u) return 0;
        result *= 2u;
    }
    if (result > UINT32_MAX) return 0;
    *descriptor_count = (uint32_t)result;
    return 1;
}

/* Returns live rows after reserving the ABI dummy token. */
static inline int rdna4_argument_capacity_token_limit(
    unsigned category, uint32_t descriptor_capacity, uint64_t null_images,
    int comparison_samplers, uint32_t *token_limit) {
    if (category >= RDNA4_ARGUMENT_CAPACITY_CATEGORIES || !token_limit) return 0;
    uint64_t per_token = category == 2 && comparison_samplers ? 2u : 1u;
    uint64_t reserved = per_token;
    if (category == 0) {
        uint32_t tail = 0;
        if (!rdna4_argument_capacity_tail(null_images, 48u, &tail) ||
            !rdna4_argument_capacity_add_u64(reserved, tail, &reserved)) return 0;
    }
    if (reserved > descriptor_capacity) return 0;
    uint64_t limit = (descriptor_capacity - reserved) / per_token;
    if (limit > UINT32_MAX) return 0;
    *token_limit = (uint32_t)limit;
    return 1;
}

/*
 * ABI 7 table: seven uint32 header/limit words, two uint32 words per token,
 * and eight uint32 dimension words per live sampled-image token when image
 * queries are enabled.
 */
static inline int rdna4_argument_capacity_table_bytes(
    const uint64_t rows[RDNA4_ARGUMENT_CAPACITY_CATEGORIES], int image_queries,
    uint64_t max_storage_buffer_range, uint64_t *bytes) {
    if (!rows || !bytes || !max_storage_buffer_range) return 0;
    uint64_t words = 7;
    for (unsigned category = 0; category < RDNA4_ARGUMENT_CAPACITY_CATEGORIES;
         ++category) {
        if (rows[category] >= UINT32_MAX) return 0;
        uint64_t add = 2u * (rows[category] + 1u);
        if (!rdna4_argument_capacity_add_u64(words, add, &words)) return 0;
    }
    if (image_queries) {
        if (rows[0] >= UINT32_MAX) return 0;
        uint64_t add = 8u * (rows[0] + 1u);
        if (!rdna4_argument_capacity_add_u64(words, add, &words)) return 0;
    }
    if (words > UINT64_MAX / sizeof(uint32_t)) return 0;
    uint64_t total = words * sizeof(uint32_t);
    if (total > max_storage_buffer_range) return 0;
    *bytes = total;
    return 1;
}

static inline int rdna4_argument_capacity_pool_budget(
    const uint64_t descriptor_counts[RDNA4_ARGUMENT_CAPACITY_CATEGORIES],
    uint32_t set_count, uint32_t max_bound_descriptor_sets,
    uint64_t max_update_after_bind_descriptors_in_all_pools,
    uint64_t *total_descriptors) {
    if (!descriptor_counts || set_count > max_bound_descriptor_sets) return 0;
    uint64_t total = 0;
    for (unsigned category = 0; category < RDNA4_ARGUMENT_CAPACITY_CATEGORIES;
         ++category) {
        if (!rdna4_argument_capacity_add_u64(total, descriptor_counts[category],
                                             &total)) return 0;
    }
    if (total > max_update_after_bind_descriptors_in_all_pools) return 0;
    if (total_descriptors) *total_descriptors = total;
    return 1;
}

/*
 * Internal samplers may require up to three Vulkan objects for one ABI8 pixel
 * configuration (normal, normalizedNormal and comparison). This aggregate
 * check takes the actual counts; other_sampler_objects includes
 * direct/static/dummy samplers and any per-configuration representations not
 * represented by the first two counts. The public configuration count never
 * includes those helper objects.
 */
static inline int rdna4_argument_capacity_sampler_objects(
    uint64_t argument_buffer_configurations, uint64_t comparison_configurations,
    uint64_t other_sampler_objects,
    uint64_t max_sampler_allocations, uint64_t *sampler_objects) {
    if (comparison_configurations > argument_buffer_configurations ||
        !max_sampler_allocations) return 0;
    uint64_t objects = other_sampler_objects;
    if (!rdna4_argument_capacity_add_u64(objects, argument_buffer_configurations,
                                         &objects) ||
        !rdna4_argument_capacity_add_u64(objects, comparison_configurations,
                                         &objects))
        return 0;
    if (objects > max_sampler_allocations) return 0;
    if (sampler_objects) *sampler_objects = objects;
    return 1;
}

static inline rdna4_argument_capacity_status rdna4_argument_capacity_calculate(
    const rdna4_argument_capacity_limits *limits,
    rdna4_argument_capacity_plan *plan) {
    if (!limits || !plan) return RDNA4_ARGUMENT_CAPACITY_INVALID;
    *plan = (rdna4_argument_capacity_plan){ .failed_category = UINT32_MAX };

    /*
     * maintenance3 reports a guaranteed descriptor-set size, not an absolute
     * ceiling. A zero value here is the cache's "not queried" sentinel, so
     * conservatively reject rather than treating it as unlimited.
     */
    if (!limits->max_per_set_descriptors)
        return RDNA4_ARGUMENT_CAPACITY_LIMIT_UNAVAILABLE;
    if (limits->max_bound_descriptor_sets < 5u)
        return RDNA4_ARGUMENT_CAPACITY_SET_COUNT;

    uint64_t direct_total[RDNA4_ARGUMENT_CAPACITY_CATEGORIES] = {0};
    for (unsigned stage = 0; stage < RDNA4_ARGUMENT_CAPACITY_STAGES; ++stage) {
        for (unsigned category = 0; category < RDNA4_ARGUMENT_CAPACITY_CATEGORIES;
             ++category) {
            uint64_t per_stage = 0;
            if (!rdna4_argument_capacity_add_u64(
                    limits->direct[stage][category],
                    limits->fixed[stage][category], &per_stage))
                return RDNA4_ARGUMENT_CAPACITY_OVERFLOW;
            if (!rdna4_argument_capacity_add_u64(
                    direct_total[category], limits->direct[stage][category],
                    &direct_total[category]))
                return RDNA4_ARGUMENT_CAPACITY_OVERFLOW;
            if (per_stage >
                limits->max_per_stage_uab[category]) {
                plan->failed_category = category;
                return RDNA4_ARGUMENT_CAPACITY_DIRECT_OVER_LIMIT;
            }
            if (limits->direct[stage][category] >
                limits->max_per_stage_ordinary[category]) {
                plan->failed_category = category;
                return RDNA4_ARGUMENT_CAPACITY_ORDINARY_DIRECT_OVER_LIMIT;
            }
        }
        uint64_t stage_resources = 0;
        if (!rdna4_argument_capacity_add_u64(
                limits->direct_resources[stage],
                limits->fixed_resources[stage], &stage_resources))
            return RDNA4_ARGUMENT_CAPACITY_OVERFLOW;
        if (stage_resources >
            limits->max_per_stage_uab_resources) {
            return RDNA4_ARGUMENT_CAPACITY_RESOURCE_OVER_LIMIT;
        }
        if (limits->direct_resources[stage] >
            limits->max_per_stage_ordinary_resources)
            return RDNA4_ARGUMENT_CAPACITY_ORDINARY_RESOURCE_OVER_LIMIT;
    }

    for (unsigned category = 0; category < RDNA4_ARGUMENT_CAPACITY_CATEGORIES;
         ++category) {
        uint64_t pipeline_total = 0;
        if (!rdna4_argument_capacity_add_u64(direct_total[category],
                limits->fixed_pipeline[category], &pipeline_total))
            return RDNA4_ARGUMENT_CAPACITY_OVERFLOW;
        if (pipeline_total > limits->max_pipeline_uab[category]) {
            plan->failed_category = category;
            return RDNA4_ARGUMENT_CAPACITY_DIRECT_OVER_LIMIT;
        }
        if (direct_total[category] > limits->max_pipeline_ordinary[category]) {
            plan->failed_category = category;
            return RDNA4_ARGUMENT_CAPACITY_ORDINARY_DIRECT_OVER_LIMIT;
        }
        if (!limits->used_stages[category]) continue;
        uint64_t capacity =
            limits->max_pipeline_uab[category] - pipeline_total;
        for (unsigned stage = 0; stage < RDNA4_ARGUMENT_CAPACITY_STAGES; ++stage) {
            if (!(limits->used_stages[category] & (1u << stage))) continue;
            capacity = rdna4_argument_capacity_min_u64(
                capacity, limits->max_per_stage_uab[category] -
                              limits->direct[stage][category] -
                              limits->fixed[stage][category]);
        }
        if (!capacity) {
            plan->failed_category = category;
            return RDNA4_ARGUMENT_CAPACITY_NO_DESCRIPTORS;
        }
        if (capacity > UINT32_MAX) {
            plan->failed_category = category;
            return RDNA4_ARGUMENT_CAPACITY_OVERFLOW;
        }
        plan->capacity[category] = (uint32_t)capacity;
        plan->above_guaranteed_limit[category] =
            capacity > limits->max_per_set_descriptors;
    }

    /*
     * The aggregate resource budget is shared by the categories visible to a
     * stage. Shrink the smallest capacity first so every remaining category
     * gets a deterministic, nonzero share while the total stays within the
     * stage budget.
     */
    for (unsigned stage = 0; stage < RDNA4_ARGUMENT_CAPACITY_STAGES; ++stage) {
        unsigned categories[RDNA4_ARGUMENT_CAPACITY_CATEGORIES];
        unsigned count = 0;
        uint64_t total = 0;
        for (unsigned category = 0; category < RDNA4_ARGUMENT_CAPACITY_CATEGORIES;
             ++category) {
            if (!rdna4_argument_capacity_counts_resources(category) ||
                !(limits->used_stages[category] & (1u << stage))) continue;
            categories[count++] = category;
            if (!rdna4_argument_capacity_add_u64(total, plan->capacity[category],
                                                 &total))
                return RDNA4_ARGUMENT_CAPACITY_RESOURCE_OVER_LIMIT;
        }
        if (!count) continue;
        uint64_t stage_resources = 0;
        if (!rdna4_argument_capacity_add_u64(
                limits->direct_resources[stage],
                limits->fixed_resources[stage], &stage_resources))
            return RDNA4_ARGUMENT_CAPACITY_RESOURCE_OVER_LIMIT;
        if (stage_resources >
            limits->max_per_stage_uab_resources)
            return RDNA4_ARGUMENT_CAPACITY_RESOURCE_OVER_LIMIT;
        uint64_t budget = limits->max_per_stage_uab_resources -
                          stage_resources;
        if (total <= budget) continue;

        for (unsigned i = 0; i < count; ++i)
            for (unsigned j = i + 1; j < count; ++j)
                if (plan->capacity[categories[j]] <
                    plan->capacity[categories[i]]) {
                    unsigned swap = categories[i];
                    categories[i] = categories[j];
                    categories[j] = swap;
                }
        for (unsigned i = 0; i < count; ++i) {
            unsigned category = categories[i];
            uint64_t share = budget / (count - i);
            if (plan->capacity[category] > share)
                plan->capacity[category] = (uint32_t)share;
            if (!plan->capacity[category]) {
                plan->failed_category = category;
                return RDNA4_ARGUMENT_CAPACITY_NO_DESCRIPTORS;
            }
            budget -= plan->capacity[category];
        }
    }
    return RDNA4_ARGUMENT_CAPACITY_OK;
}

#endif
