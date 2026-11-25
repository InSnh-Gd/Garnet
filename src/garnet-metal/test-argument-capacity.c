#include "garnet_argument_capacity.h"

#include <stdio.h>
#include <stdlib.h>

static unsigned checks;

static void require(int condition, const char *message) {
    ++checks;
    if (!condition) {
        fprintf(stderr, "ARGUMENT_CAPACITY_FAIL %s\n", message);
        exit(1);
    }
}

static void test_shape_and_descriptor_counts(void) {
    uint32_t tail = 0;
    require(rdna4_argument_capacity_tail((1ull << 2) | (1ull << 10), 48, &tail) &&
                tail == 11,
            "shape tail uses the highest reflected shape");
    require(rdna4_argument_capacity_tail(1ull << 47, 48, &tail) && tail == 48,
            "last legal shape tail");
    require(!rdna4_argument_capacity_tail(1ull << 48, 48, &tail),
            "shape outside the ABI is rejected");

    uint32_t descriptors = 0;
    require(rdna4_argument_capacity_descriptor_count(
                0, 6, (1ull << 2) | (1ull << 10), 0, &descriptors) &&
                descriptors == 17,
            "sampled token and shape descriptors");
    require(rdna4_argument_capacity_descriptor_count(1, 6, 0, 0, &descriptors) &&
                descriptors == 6,
            "storage descriptors have no shape tail");
    require(rdna4_argument_capacity_descriptor_count(2, 6, 0, 1, &descriptors) &&
                descriptors == 12,
            "dual-use samplers use two descriptor regions");
    require(rdna4_argument_capacity_descriptor_count(
                2, UINT32_MAX / 2u, 0, 1, &descriptors) &&
                descriptors == UINT32_MAX - 1,
            "last legal dual-use descriptor count");
    require(!rdna4_argument_capacity_descriptor_count(
                2, UINT32_MAX / 2u + 1u, 0, 1, &descriptors),
            "dual-use descriptor overflow is rejected");
    require(!rdna4_argument_capacity_descriptor_count(
                0, UINT32_MAX, (1ull << 2), 0, &descriptors),
            "shape-tail descriptor overflow is rejected");
}

static rdna4_argument_capacity_limits base_limits(void) {
    return (rdna4_argument_capacity_limits){
        .max_per_stage_uab = {1000, 1000, 1000},
        .max_pipeline_uab = {1000, 1000, 1000},
        .max_per_stage_uab_resources = 100000,
        .max_per_stage_ordinary = {1000, 1000, 1000},
        .max_pipeline_ordinary = {1000, 1000, 1000},
        .max_per_stage_ordinary_resources = 100000,
        .max_per_set_descriptors = 1000,
        .max_bound_descriptor_sets = 8,
        .used_stages = {3, 3, 3},
    };
}

static void test_capacity_plan(void) {
    rdna4_argument_capacity_limits fixed = base_limits();
    fixed.max_per_stage_ordinary[0] = fixed.max_pipeline_ordinary[0] = 0;
    fixed.max_per_stage_uab[0] = fixed.max_pipeline_uab[0] = 10;
    fixed.fixed[0][0] = fixed.fixed[1][0] = 3;
    fixed.fixed_pipeline[0] = 3;
    fixed.fixed_resources[0] = fixed.fixed_resources[1] = 3;
    rdna4_argument_capacity_plan fixed_plan;
    require(rdna4_argument_capacity_calculate(&fixed, &fixed_plan) == RDNA4_ARGUMENT_CAPACITY_OK &&
        fixed_plan.capacity[0] == 7,
        "shared fixed texel binding counted once in UAB pipeline and excluded ordinary");
    fixed.fixed_pipeline[0] = UINT64_MAX;
    fixed.direct[0][0] = 1;
    fixed.max_per_stage_ordinary[0] = fixed.max_pipeline_ordinary[0] = 1;
    require(rdna4_argument_capacity_calculate(&fixed, &fixed_plan) == RDNA4_ARGUMENT_CAPACITY_OVERFLOW,
        "fixed shared pipeline descriptor overflow rejects before publication");
    rdna4_argument_capacity_limits limits = base_limits();
    limits.direct[0][0] = 5;
    limits.direct[1][0] = 7;
    limits.direct[0][1] = 2;
    limits.direct[1][1] = 3;
    limits.max_per_stage_uab[1] = 10;
    limits.direct_resources[0] = 20;
    limits.direct_resources[1] = 30;
    rdna4_argument_capacity_plan plan;
    require(rdna4_argument_capacity_calculate(&limits, &plan) ==
                RDNA4_ARGUMENT_CAPACITY_OK,
            "checked capacity plan");
    require(plan.capacity[0] == 988 && plan.capacity[1] == 7 &&
                plan.capacity[2] == 1000,
            "pipeline and per-stage limits include direct descriptors");

    limits = base_limits();
    limits.max_per_stage_uab[2] = 96;
    limits.max_pipeline_uab[2] = 96;
    limits.direct[0][2] = 2;
    limits.direct[1][2] = 4;
    require(rdna4_argument_capacity_calculate(&limits, &plan) ==
                RDNA4_ARGUMENT_CAPACITY_OK &&
                plan.capacity[2] == 90,
            "pipeline UAB limit subtracts both stages' direct descriptors");

    limits = base_limits();
    limits.max_per_stage_uab[0] = 100;
    limits.max_pipeline_uab[0] = 90;
    limits.direct[0][0] = 2;
    limits.direct[1][0] = 3;
    require(rdna4_argument_capacity_calculate(&limits, &plan) ==
                RDNA4_ARGUMENT_CAPACITY_OK &&
                plan.capacity[0] == 85,
            "shared table capacity fits stage and whole-pipeline totals");

    limits = base_limits();
    limits.max_per_set_descriptors = 32;
    require(rdna4_argument_capacity_calculate(&limits, &plan) ==
                RDNA4_ARGUMENT_CAPACITY_OK &&
                plan.capacity[0] == 1000 && plan.capacity[1] == 1000 &&
                plan.capacity[2] == 1000 &&
                plan.above_guaranteed_limit[0] &&
                plan.above_guaranteed_limit[1] &&
                plan.above_guaranteed_limit[2],
            "maxPerSetDescriptors is advisory, not an absolute cap");

    limits = base_limits();
    limits.max_per_set_descriptors = 1000;
    require(rdna4_argument_capacity_calculate(&limits, &plan) ==
                RDNA4_ARGUMENT_CAPACITY_OK &&
                !plan.above_guaranteed_limit[0] &&
                !plan.above_guaranteed_limit[1] &&
                !plan.above_guaranteed_limit[2],
            "layout within the guaranteed per-set size is reported as such");

    limits = base_limits();
    limits.max_per_set_descriptors = 0;
    require(rdna4_argument_capacity_calculate(&limits, &plan) ==
                RDNA4_ARGUMENT_CAPACITY_LIMIT_UNAVAILABLE,
            "unqueried maxPerSetDescriptors is rejected conservatively");

    limits = base_limits();
    limits.max_bound_descriptor_sets = 4;
    require(rdna4_argument_capacity_calculate(&limits, &plan) ==
                RDNA4_ARGUMENT_CAPACITY_SET_COUNT,
            "five-set layout requires maxBoundDescriptorSets >= 5");

    for (unsigned category = 0; category < 3; ++category) {
        limits = base_limits();
        limits.direct[1][category] =
            limits.max_per_stage_uab[category] + 1u;
        require(rdna4_argument_capacity_calculate(&limits, &plan) ==
                    RDNA4_ARGUMENT_CAPACITY_DIRECT_OVER_LIMIT &&
                    plan.failed_category == category,
                "each direct category over-limit is rejected");
    }

    limits = base_limits();
    limits.max_pipeline_uab[1] = 5;
    limits.direct[0][1] = 2;
    limits.direct[1][1] = 4;
    require(rdna4_argument_capacity_calculate(&limits, &plan) ==
                RDNA4_ARGUMENT_CAPACITY_DIRECT_OVER_LIMIT &&
                plan.failed_category == 1,
            "UAB pipeline total includes direct descriptors from both stages");

    limits = base_limits();
    limits.max_pipeline_uab[0] = 13;
    limits.direct[0][0] = 5;
    limits.direct[1][0] = 7;
    require(rdna4_argument_capacity_calculate(&limits, &plan) ==
                RDNA4_ARGUMENT_CAPACITY_OK &&
                plan.capacity[0] == 1,
            "last legal pipeline headroom subtracts the direct total");

    limits = base_limits();
    limits.max_per_stage_ordinary[2] = 1;
    limits.direct[1][2] = 2;
    require(rdna4_argument_capacity_calculate(&limits, &plan) ==
                RDNA4_ARGUMENT_CAPACITY_ORDINARY_DIRECT_OVER_LIMIT &&
                plan.failed_category == 2,
            "ordinary per-stage limits still bound non-UAB direct samplers");

    limits = base_limits();
    limits.max_pipeline_ordinary[0] = 10;
    limits.direct[0][0] = 5;
    limits.direct[1][0] = 6;
    require(rdna4_argument_capacity_calculate(&limits, &plan) ==
                RDNA4_ARGUMENT_CAPACITY_ORDINARY_DIRECT_OVER_LIMIT &&
                plan.failed_category == 0,
            "ordinary pipeline totals bound both direct sets");

    limits = base_limits();
    limits.max_per_stage_ordinary_resources = 19;
    limits.direct_resources[0] = 20;
    require(rdna4_argument_capacity_calculate(&limits, &plan) ==
                RDNA4_ARGUMENT_CAPACITY_ORDINARY_RESOURCE_OVER_LIMIT,
            "ordinary per-stage resource limits bound the direct set");

    limits = base_limits();
    limits.direct_resources[1] =
        limits.max_per_stage_uab_resources + 1u;
    require(rdna4_argument_capacity_calculate(&limits, &plan) ==
                RDNA4_ARGUMENT_CAPACITY_RESOURCE_OVER_LIMIT,
            "direct resources over the aggregate limit are rejected");

    limits = base_limits();
    limits.max_per_stage_uab_resources = 100;
    limits.direct_resources[0] = 10;
    limits.direct_resources[1] = 20;
    require(rdna4_argument_capacity_calculate(&limits, &plan) ==
                RDNA4_ARGUMENT_CAPACITY_OK,
            "aggregate stage budget is shared");
    require((uint64_t)plan.capacity[0] + plan.capacity[1] <= 80 &&
                plan.capacity[2] == 1000,
            "resource aggregate budget excludes sampler descriptors");

    limits = base_limits();
    limits.max_per_stage_uab_resources = 0;
    limits.used_stages[0] = 0;
    limits.used_stages[1] = 0;
    require(rdna4_argument_capacity_calculate(&limits, &plan) ==
                RDNA4_ARGUMENT_CAPACITY_OK &&
                plan.capacity[2] == 1000,
            "sampler-only table ignores the resource aggregate budget");

    limits = base_limits();
    limits.max_per_stage_uab[0] = UINT64_C(1) << 32;
    limits.max_pipeline_uab[0] = UINT64_C(1) << 32;
    limits.max_per_set_descriptors = UINT32_MAX;
    require(rdna4_argument_capacity_calculate(&limits, &plan) ==
                RDNA4_ARGUMENT_CAPACITY_OVERFLOW &&
                plan.failed_category == 0,
            "capacity larger than the uint32 table representation is rejected");

    limits = base_limits();
    limits.max_per_stage_uab[0] = 0;
    require(rdna4_argument_capacity_calculate(&limits, &plan) ==
                RDNA4_ARGUMENT_CAPACITY_NO_DESCRIPTORS,
            "zero category headroom is rejected");
}

static void test_token_limits(void) {
    uint32_t limit = 0;
    require(rdna4_argument_capacity_token_limit(0, 10, 1ull << 3, 0, &limit) &&
                limit == 5,
            "sampled live-token limit reserves the dummy and sparse tail");
    require(rdna4_argument_capacity_token_limit(1, 10, 0, 0, &limit) &&
                limit == 9,
            "storage live-token limit reserves the dummy row");
    require(rdna4_argument_capacity_token_limit(2, 10, 0, 1, &limit) &&
                limit == 4,
            "dual-use sampler limit reserves the dummy in both payload regions");
    require(rdna4_argument_capacity_token_limit(2, 11, 0, 0, &limit) &&
                limit == 10,
            "normal sampler live-token limit");
    require(rdna4_argument_capacity_token_limit(2, 96, 0, 1, &limit) &&
                limit == 47,
            "mixed 96-slot sampler table admits 47 live configurations");
    require(rdna4_argument_capacity_token_limit(2, 96, 0, 0, &limit) &&
                limit == 95,
            "normal 96-slot sampler table reserves one dummy row");
    require(!rdna4_argument_capacity_token_limit(0, 3, 1ull << 3, 0, &limit),
            "dummy tail larger than the descriptor capacity is rejected");
}

static void test_table_and_pool_limits(void) {
    uint64_t bytes = 0;
    const uint64_t empty_rows[3] = {0, 0, 0};
    require(rdna4_argument_capacity_table_bytes(empty_rows, 0, 52, &bytes) &&
                bytes == 52,
            "empty ABI table bytes");
    require(rdna4_argument_capacity_table_bytes(empty_rows, 1, 84, &bytes) &&
                bytes == 84,
            "image-query table bytes");
    const uint64_t rows[3] = {1, 2, 3};
    require(rdna4_argument_capacity_table_bytes(rows, 0, 100, &bytes) &&
                bytes == 100,
            "last legal table buffer range");
    require(!rdna4_argument_capacity_table_bytes(rows, 0, 99, &bytes),
            "table buffer range overflow is rejected");
    require(!rdna4_argument_capacity_table_bytes(rows, 0, 0, &bytes),
            "unqueried table buffer range is rejected");
    const uint64_t invalid_rows[3] = {UINT32_MAX, 0, 0};
    require(!rdna4_argument_capacity_table_bytes(invalid_rows, 0, UINT64_MAX, &bytes),
            "table token overflow is rejected");

    const uint64_t counts[3] = {3, 4, 5};
    uint64_t total = 0;
    require(rdna4_argument_capacity_pool_budget(counts, 3, 5, 12, &total) &&
                total == 12,
            "last legal global UAB pool total");
    require(!rdna4_argument_capacity_pool_budget(counts, 3, 5, 11, &total),
            "global UAB pool overflow is rejected");
    require(!rdna4_argument_capacity_pool_budget(counts, 6, 5, 12, &total),
            "descriptor set count overflow is rejected");

}

static void test_sampler_object_limits(void) {
    uint64_t objects = 0;
    require(rdna4_argument_capacity_sampler_objects(
                10, 4, 6, 20, &objects) &&
                objects == 20,
            "public configurations plus other internal sampler objects");
    require(!rdna4_argument_capacity_sampler_objects(
                10, 4, 6, 19, &objects),
            "sampler object overflow is rejected");
    require(!rdna4_argument_capacity_sampler_objects(
                11, 12, 0, 22, &objects),
            "comparison configurations cannot exceed public configurations");
    require(!rdna4_argument_capacity_sampler_objects(
                UINT64_MAX, 0, 1, UINT64_MAX, &objects),
            "sampler object addition overflow is rejected");
}

int main(void) {
    test_shape_and_descriptor_counts();
    test_capacity_plan();
    test_token_limits();
    test_table_and_pool_limits();
    test_sampler_object_limits();
    printf("ARGUMENT_CAPACITY_PASS checks=%u gpu_execution=0\n", checks);
    return 0;
}
