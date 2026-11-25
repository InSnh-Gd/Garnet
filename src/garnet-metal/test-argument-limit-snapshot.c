#define _POSIX_C_SOURCE 200809L

#define GARNET_ARGUMENT_LIMIT_SNAPSHOT_ONLY 1
#include "garnet_backend.m"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

static unsigned checks;

static void require(int condition, const char *message) {
    ++checks;
    if (!condition) {
        fprintf(stderr, "ARGUMENT_LIMIT_SNAPSHOT_FAIL %s\n", message);
        exit(1);
    }
}

static rdna4_radv_darwin_argument_limits_v1 valid_raw(int memory) {
    rdna4_radv_darwin_argument_limits_v1 raw;
    memset(&raw, 0, sizeof(raw));
    raw.struct_size = sizeof(raw);
    raw.abi_version = 1;
    raw.flags = RDNA4_RADV_DARWIN_LIMITS_READ_ONLY |
                RDNA4_RADV_DARWIN_LIMITS_NO_VULKAN_OBJECT |
                RDNA4_RADV_DARWIN_LIMITS_NO_PIPELINE_UUID;
    raw.valid_flags = RDNA4_RADV_DARWIN_LIMITS_VALID_STABLE_CORE;
    snprintf(raw.build_identity, sizeof(raw.build_identity),
             "rdal-v1 26.3.0-devel test");
    raw.api_version = 0x00401000u;
    raw.driver_version = 0x00000001u;
    raw.driver_id = 5u;
    raw.max_descriptor_set_size = 8388606u;
    raw.max_sampler_allocations = 65536u;
    raw.max_bound_descriptor_sets = 32u;
    raw.max_per_set_descriptors = 22369621u;
    raw.max_storage_buffer_range = UINT32_MAX;
    raw.max_uab_descriptors_all_pools = 67108863u;

    raw.max_per_stage_descriptor_samplers = 8388606u;
    raw.max_per_stage_descriptor_uniform_buffers = 8388606u;
    raw.max_per_stage_descriptor_storage_buffers = 8388606u;
    raw.max_per_stage_descriptor_sampled_images = 8388606u;
    raw.max_per_stage_descriptor_storage_images = 8388606u;
    raw.max_per_stage_descriptor_input_attachments = 8388606u;
    raw.max_per_stage_resources = 8388606u;

    raw.max_descriptor_set_samplers = 8388606u;
    raw.max_descriptor_set_uniform_buffers = 8388606u;
    raw.max_descriptor_set_uniform_buffers_dynamic = 16u;
    raw.max_descriptor_set_storage_buffers = 8388606u;
    raw.max_descriptor_set_storage_buffers_dynamic = 8u;
    raw.max_descriptor_set_sampled_images = 8388606u;
    raw.max_descriptor_set_storage_images = 8388606u;
    raw.max_descriptor_set_input_attachments = 8388606u;

    raw.max_per_stage_descriptor_update_after_bind_samplers = 8388606u;
    raw.max_per_stage_descriptor_update_after_bind_uniform_buffers = 8388606u;
    raw.max_per_stage_descriptor_update_after_bind_storage_buffers = 8388606u;
    raw.max_per_stage_descriptor_update_after_bind_sampled_images = 8388606u;
    raw.max_per_stage_descriptor_update_after_bind_storage_images = 8388606u;
    raw.max_per_stage_descriptor_update_after_bind_input_attachments = 8388606u;
    raw.max_per_stage_update_after_bind_resources = 8388606u;

    raw.max_descriptor_set_update_after_bind_samplers = 8388606u;
    raw.max_descriptor_set_update_after_bind_uniform_buffers = 8388606u;
    raw.max_descriptor_set_update_after_bind_uniform_buffers_dynamic = 16u;
    raw.max_descriptor_set_update_after_bind_storage_buffers = 8388606u;
    raw.max_descriptor_set_update_after_bind_storage_buffers_dynamic = 8u;
    raw.max_descriptor_set_update_after_bind_sampled_images = 8388606u;
    raw.max_descriptor_set_update_after_bind_storage_images = 8388606u;
    raw.max_descriptor_set_update_after_bind_input_attachments = 8388606u;
    raw.max_update_after_bind_descriptors_in_all_pools = 67108863u;

    raw.max_fragment_combined_output_resources = 8388606u;
    raw.max_color_attachments = 8u;
    raw.max_fragment_output_attachments = 8u;
    raw.max_fragment_dual_src_attachments = 1u;
    if (memory) {
        raw.valid_flags |= RDNA4_RADV_DARWIN_LIMITS_VALID_MEMORY;
        raw.max_memory_allocation_size = 4294967296u;
        raw.max_buffer_size = 4294967296u;
    }
    return raw;
}

static int fake_export_calls;
static rdna4_radv_darwin_argument_limits_v1 fake_export_raw;
static int fake_export_result;

static int fake_export(const rdna4_radv_darwin_host_facts_v1 *facts,
                       rdna4_radv_darwin_argument_limits_v1 *out) {
    (void)facts;
    ++fake_export_calls;
    if (fake_export_result) return fake_export_result;
    *out = fake_export_raw;
    return 0;
}

typedef struct {
    unsigned open_calls;
    unsigned close_calls;
    unsigned resolve_calls;
    void *symbol;
} fake_loader_context;

static void *fake_open(void *context, const char *path) {
    fake_loader_context *ctx = context;
    (void)path;
    ++ctx->open_calls;
    return ctx;
}

static void fake_close(void *context, void *handle) {
    fake_loader_context *ctx = context;
    ++ctx->close_calls;
    require(handle == ctx, "loader closes the handle it opened");
}

static void *fake_resolve(void *context, void *handle, const char *name) {
    fake_loader_context *ctx = context;
    ++ctx->resolve_calls;
    require(handle == ctx, "loader resolves on its handle");
    require(!strcmp(name, RDNA4_ARGUMENT_LIMIT_EXPORT_NAME),
            "loader requests the versioned export");
    return ctx->symbol;
}

static rdna4_argument_limit_open_observation observation_from_raw(
    const rdna4_radv_darwin_argument_limits_v1 *raw) {
    rdna4_argument_limit_open_observation o;
    memset(&o, 0, sizeof(o));
    o.struct_size = sizeof(o);
    o.observed_flags = RDNA4_ARGUMENT_LIMIT_OBSERVED_CORE_MASK |
                       RDNA4_ARGUMENT_LIMIT_OBSERVED_MEMORY_LIMITS;
    o.api_version = raw->api_version;
    o.driver_version = raw->driver_version;
    o.driver_id = raw->driver_id;
    o.max_descriptor_set_size = raw->max_descriptor_set_size;
    o.max_sampler_allocations = raw->max_sampler_allocations;
    o.max_bound_descriptor_sets = raw->max_bound_descriptor_sets;
    o.max_per_set_descriptors = raw->max_per_set_descriptors;
    o.max_storage_buffer_range = raw->max_storage_buffer_range;
    o.max_uab_descriptors_all_pools = raw->max_uab_descriptors_all_pools;
    o.max_per_stage_descriptor_samplers =
        raw->max_per_stage_descriptor_samplers;
    o.max_per_stage_descriptor_uniform_buffers =
        raw->max_per_stage_descriptor_uniform_buffers;
    o.max_per_stage_descriptor_storage_buffers =
        raw->max_per_stage_descriptor_storage_buffers;
    o.max_per_stage_descriptor_sampled_images =
        raw->max_per_stage_descriptor_sampled_images;
    o.max_per_stage_descriptor_storage_images =
        raw->max_per_stage_descriptor_storage_images;
    o.max_per_stage_descriptor_input_attachments =
        raw->max_per_stage_descriptor_input_attachments;
    o.max_per_stage_resources = raw->max_per_stage_resources;
    o.max_descriptor_set_samplers = raw->max_descriptor_set_samplers;
    o.max_descriptor_set_uniform_buffers =
        raw->max_descriptor_set_uniform_buffers;
    o.max_descriptor_set_uniform_buffers_dynamic =
        raw->max_descriptor_set_uniform_buffers_dynamic;
    o.max_descriptor_set_storage_buffers =
        raw->max_descriptor_set_storage_buffers;
    o.max_descriptor_set_storage_buffers_dynamic =
        raw->max_descriptor_set_storage_buffers_dynamic;
    o.max_descriptor_set_sampled_images =
        raw->max_descriptor_set_sampled_images;
    o.max_descriptor_set_storage_images =
        raw->max_descriptor_set_storage_images;
    o.max_descriptor_set_input_attachments =
        raw->max_descriptor_set_input_attachments;
    o.max_per_stage_descriptor_update_after_bind_samplers =
        raw->max_per_stage_descriptor_update_after_bind_samplers;
    o.max_per_stage_descriptor_update_after_bind_uniform_buffers =
        raw->max_per_stage_descriptor_update_after_bind_uniform_buffers;
    o.max_per_stage_descriptor_update_after_bind_storage_buffers =
        raw->max_per_stage_descriptor_update_after_bind_storage_buffers;
    o.max_per_stage_descriptor_update_after_bind_sampled_images =
        raw->max_per_stage_descriptor_update_after_bind_sampled_images;
    o.max_per_stage_descriptor_update_after_bind_storage_images =
        raw->max_per_stage_descriptor_update_after_bind_storage_images;
    o.max_per_stage_descriptor_update_after_bind_input_attachments =
        raw->max_per_stage_descriptor_update_after_bind_input_attachments;
    o.max_per_stage_update_after_bind_resources =
        raw->max_per_stage_update_after_bind_resources;
    o.max_descriptor_set_update_after_bind_samplers =
        raw->max_descriptor_set_update_after_bind_samplers;
    o.max_descriptor_set_update_after_bind_uniform_buffers =
        raw->max_descriptor_set_update_after_bind_uniform_buffers;
    o.max_descriptor_set_update_after_bind_uniform_buffers_dynamic =
        raw->max_descriptor_set_update_after_bind_uniform_buffers_dynamic;
    o.max_descriptor_set_update_after_bind_storage_buffers =
        raw->max_descriptor_set_update_after_bind_storage_buffers;
    o.max_descriptor_set_update_after_bind_storage_buffers_dynamic =
        raw->max_descriptor_set_update_after_bind_storage_buffers_dynamic;
    o.max_descriptor_set_update_after_bind_sampled_images =
        raw->max_descriptor_set_update_after_bind_sampled_images;
    o.max_descriptor_set_update_after_bind_storage_images =
        raw->max_descriptor_set_update_after_bind_storage_images;
    o.max_descriptor_set_update_after_bind_input_attachments =
        raw->max_descriptor_set_update_after_bind_input_attachments;
    o.max_update_after_bind_descriptors_in_all_pools =
        raw->max_update_after_bind_descriptors_in_all_pools;
    o.max_fragment_combined_output_resources =
        raw->max_fragment_combined_output_resources;
    o.max_color_attachments = raw->max_color_attachments;
    o.max_fragment_output_attachments = raw->max_fragment_output_attachments;
    o.max_fragment_dual_src_attachments = raw->max_fragment_dual_src_attachments;
    o.max_memory_allocation_size = raw->max_memory_allocation_size;
    o.max_buffer_size = raw->max_buffer_size;
    return o;
}

static void test_sizes_and_plan(void) {
    require(sizeof(rdna4_radv_darwin_host_facts_v1) == 48,
            "host facts are the 48-byte v1 ABI");
    require(sizeof(rdna4_radv_darwin_argument_limits_v1) == 272,
            "argument limits are the 272-byte v1 ABI");

    rdna4_argument_limit_snapshot snapshot;
    memset(&snapshot, 0, sizeof(snapshot));
    fake_export_calls = 0;
    fake_export_result = 0;
    fake_export_raw = valid_raw(0);
    require(rdna4_argument_limit_snapshot_init_from_export(
                &snapshot, fake_export, NULL, NULL) ==
                RDNA4_ARGUMENT_LIMIT_RESULT_OK,
            "valid v1 export becomes ready");
    require(snapshot.state == RDNA4_ARGUMENT_LIMIT_STATE_READY &&
                snapshot.export_present &&
                snapshot.export_result == 0 &&
                fake_export_calls == 1,
            "snapshot records one successful immutable source");
    require(snapshot.descriptor_capacity == 8388606u &&
                snapshot.sampler_plan.table_limit == 4194302u &&
                snapshot.sampler_plan.object_limit == 21844u &&
                snapshot.public_sampler_limit == 21844u,
            "whole-layout UAB, dummy reservation and K=3 bound the public count");

    uint32_t limit = 0;
    require(rdna4_argument_limit_snapshot_public_limit(&snapshot, &limit) ==
                    RDNA4_ARGUMENT_LIMIT_RESULT_OK &&
                limit == 21844u,
            "getter returns the frozen public count");
    require(rdna4_argument_limit_snapshot_public_limit(&snapshot, &limit) ==
                    RDNA4_ARGUMENT_LIMIT_RESULT_OK &&
                limit == 21844u && fake_export_calls == 1,
            "repeated getter is exact and does not query the source");

    rdna4_argument_limit_snapshot before = snapshot;
    require(rdna4_argument_limit_snapshot_init_from_export(
                &snapshot, fake_export, NULL, NULL) ==
                RDNA4_ARGUMENT_LIMIT_RESULT_ALREADY_FIXED &&
                !memcmp(&before, &snapshot, sizeof(snapshot)) &&
                fake_export_calls == 1,
            "second initialization is rejected without mutation");
}

static void test_admission(void) {
    rdna4_argument_limit_snapshot snapshot;
    memset(&snapshot, 0, sizeof(snapshot));
    fake_export_result = 0;
    fake_export_raw = valid_raw(0);
    require(rdna4_argument_limit_snapshot_init_from_export(
                &snapshot, fake_export, NULL, NULL) ==
                RDNA4_ARGUMENT_LIMIT_RESULT_OK,
            "admission fixture snapshot is ready");

    int admitted = -1;
    require(rdna4_argument_limit_snapshot_admit(
                &snapshot, 21843u, 1, 65533u, 3u, &admitted) ==
                RDNA4_ARGUMENT_PUBLIC_SAMPLER_OK &&
                admitted,
            "last legal unique configuration and three-handle request");
    require(rdna4_argument_limit_snapshot_admit(
                &snapshot, 21844u, 1, 0, 0, &admitted) ==
                RDNA4_ARGUMENT_PUBLIC_SAMPLER_AT_LIMIT &&
                !admitted,
            "first new unique configuration above the public limit is rejected");
    require(rdna4_argument_limit_snapshot_admit(
                &snapshot, 21844u, 0, 65534u, 2u, &admitted) ==
                RDNA4_ARGUMENT_PUBLIC_SAMPLER_OK &&
                admitted,
            "reuse at the limit can consume the final two object slots");
    require(rdna4_argument_limit_snapshot_admit(
                &snapshot, 21844u, 0, 65534u, 3u, &admitted) ==
                RDNA4_ARGUMENT_PUBLIC_SAMPLER_OBJECT_LIMIT &&
                !admitted,
            "one-over three-handle object request is rejected");
}

static void test_reservation_arithmetic(void) {
    rdna4_argument_limit_snapshot snapshot;
    memset(&snapshot, 0, sizeof(snapshot));
    fake_export_result = 0;
    fake_export_raw = valid_raw(0);
    require(rdna4_argument_limit_snapshot_init_from_export(
                &snapshot, fake_export, NULL, NULL) ==
                RDNA4_ARGUMENT_LIMIT_RESULT_OK,
            "reservation fixture snapshot is ready");

    /*
     * Model the ABI8 device budget: every admitted public configuration
     * reserves the worst-case three backend handles.  The ordinary sampler
     * is instantiated immediately; the other two remain unused reservations
     * until comparison/normalized-normal lowering needs them.
     */
    uint64_t actual_objects = 2u; /* existing normal/comparison dummies */
    rdna4_argument_sampler_budget budget = {0};
    int admitted = 0;
    for (uint32_t live = 0;
         live < snapshot.public_sampler_limit; ++live) {
        require(rdna4_argument_limit_snapshot_budget_reserve_configuration(
                    &snapshot, &budget, live, 0, actual_objects,
                    &admitted) ==
                        RDNA4_ARGUMENT_PUBLIC_SAMPLER_OK &&
                    admitted,
                "each legal public configuration reserves three handles");
        int charged = 0;
        require(rdna4_argument_limit_snapshot_budget_charge_reserved(
                    &snapshot, &budget, &charged) ==
                        RDNA4_ARGUMENT_PUBLIC_SAMPLER_OK &&
                    charged,
                "the normal representation consumes one reservation");
        ++actual_objects;
    }
    require(actual_objects == 21846u &&
                budget.reserved_objects == 65532u &&
                budget.reserved_actual_objects == 21844u,
            "full ABI8 reservation holds 21844 unique configurations");

    require(rdna4_argument_limit_snapshot_budget_can_charge_direct(
                &snapshot, &budget, actual_objects, 2u, &admitted) ==
                    RDNA4_ARGUMENT_PUBLIC_SAMPLER_OK &&
                admitted,
            "two direct/dummy objects remain outside the reservations");
    actual_objects += 2u;
    require(rdna4_argument_limit_snapshot_budget_can_charge_direct(
                &snapshot, &budget, actual_objects, 1u, &admitted) ==
                    RDNA4_ARGUMENT_PUBLIC_SAMPLER_OBJECT_LIMIT &&
                !admitted,
            "one-over direct object fails before the backend limit");
    require(rdna4_argument_limit_snapshot_budget_reserve_configuration(
                &snapshot, &budget, snapshot.public_sampler_limit, 0,
                actual_objects, &admitted) ==
                    RDNA4_ARGUMENT_PUBLIC_SAMPLER_AT_LIMIT &&
                !admitted,
            "one-over unique configuration is rejected before object charge");

    int released = 0;
    require(rdna4_argument_limit_snapshot_budget_release_configuration(
                &snapshot, &budget, 1u, &released) ==
                    RDNA4_ARGUMENT_PUBLIC_SAMPLER_OK &&
                released && budget.reserved_objects == 65529u &&
                budget.reserved_actual_objects == 21843u,
            "configuration rollback returns all three reserved slots");

    /*
     * Model the production lifetime for one configuration: the reservation
     * owns all three worst-case handles, a failed comparison creation rolls
     * back only its reserved charge, and wrapper teardown releases the whole
     * configuration before VkSampler destruction.
     */
    rdna4_argument_sampler_budget lifetime = {0};
    uint64_t lifetime_actual = 2u;
    const uint32_t lifetime_unique = 21843u;
    require(rdna4_argument_limit_snapshot_budget_reserve_configuration(
                &snapshot, &lifetime, lifetime_unique, 0, lifetime_actual,
                &admitted) == RDNA4_ARGUMENT_PUBLIC_SAMPLER_OK &&
                admitted && lifetime.reserved_objects == 3u &&
                lifetime.reserved_actual_objects == 0u,
            "a source-pixel configuration reserves three object slots");
    int charged = 0;
    require(rdna4_argument_limit_snapshot_budget_charge_reserved(
                &snapshot, &lifetime, &charged) ==
                    RDNA4_ARGUMENT_PUBLIC_SAMPLER_OK &&
                charged && lifetime.reserved_actual_objects == 1u,
            "the normal representation moves one reserved slot to actual");
    ++lifetime_actual;
    require(rdna4_argument_limit_snapshot_budget_charge_reserved(
                &snapshot, &lifetime, &charged) ==
                    RDNA4_ARGUMENT_PUBLIC_SAMPLER_OK &&
                charged && lifetime.reserved_actual_objects == 2u,
            "a comparison representation consumes the second slot");
    ++lifetime_actual;
    released = 0;
    require(rdna4_argument_limit_snapshot_budget_release_reserved(
                &snapshot, &lifetime, &released) ==
                    RDNA4_ARGUMENT_PUBLIC_SAMPLER_OK &&
                released && lifetime.reserved_actual_objects == 1u &&
                lifetime_actual == 4u,
            "a failed comparison creation rolls back exactly one slot");
    --lifetime_actual;
    require(rdna4_argument_limit_snapshot_budget_release_configuration(
                &snapshot, &lifetime, 1u, &released) ==
                    RDNA4_ARGUMENT_PUBLIC_SAMPLER_OK &&
                released && lifetime.reserved_objects == 0u &&
                lifetime.reserved_actual_objects == 0u,
            "configuration teardown returns the remaining reservation");
    --lifetime_actual;
    require(lifetime_actual == 2u,
            "only direct/dummy handles remain after configuration teardown");
}

static void test_missing_and_malformed(void) {
    fake_loader_context context = {0};
    rdna4_argument_limit_loader loader = {
        .context = &context,
        .open_library = fake_open,
        .close_library = fake_close,
        .resolve_symbol = fake_resolve,
    };
    rdna4_argument_limit_snapshot snapshot;
    memset(&snapshot, 0, sizeof(snapshot));

    require(rdna4_argument_limit_snapshot_init_from_loader(
                &snapshot, &loader, "/missing/provider.dylib", NULL, NULL) ==
                RDNA4_ARGUMENT_LIMIT_RESULT_EXPORT_MISSING &&
                snapshot.state == RDNA4_ARGUMENT_LIMIT_STATE_UNAVAILABLE &&
                context.open_calls == 1 && context.resolve_calls == 1 &&
                context.close_calls == 1,
            "old bundle without the export becomes stable unavailable");
    uint32_t sentinel = 0xabcdef01u;
    require(rdna4_argument_limit_snapshot_public_limit(&snapshot, &sentinel) ==
                    RDNA4_ARGUMENT_LIMIT_RESULT_CAPACITY_UNAVAILABLE &&
                sentinel == 0xabcdef01u,
            "unavailable getter does not manufacture a numeric fallback");
    rdna4_argument_limit_snapshot missing_before = snapshot;
    require(rdna4_argument_limit_snapshot_init_from_loader(
                &snapshot, &loader, "/missing/provider.dylib", NULL, NULL) ==
                RDNA4_ARGUMENT_LIMIT_RESULT_ALREADY_FIXED &&
                !memcmp(&missing_before, &snapshot, sizeof(snapshot)),
            "missing export stays unavailable on a second call");

    fake_export_raw = valid_raw(0);
    fake_export_raw.abi_version = 2;
    memset(&snapshot, 0, sizeof(snapshot));
    require(rdna4_argument_limit_snapshot_init_from_export(
                &snapshot, fake_export, NULL, NULL) ==
                RDNA4_ARGUMENT_LIMIT_RESULT_EXPORT_ABI_MISMATCH,
            "wrong source ABI fails closed");

    fake_export_raw = valid_raw(0);
    fake_export_raw.struct_size = 256;
    memset(&snapshot, 0, sizeof(snapshot));
    require(rdna4_argument_limit_snapshot_init_from_export(
                &snapshot, fake_export, NULL, NULL) ==
                RDNA4_ARGUMENT_LIMIT_RESULT_EXPORT_MALFORMED,
            "wrong source struct size fails closed");

    fake_export_raw = valid_raw(0);
    fake_export_raw.flags &= ~RDNA4_RADV_DARWIN_LIMITS_READ_ONLY;
    memset(&snapshot, 0, sizeof(snapshot));
    require(rdna4_argument_limit_snapshot_init_from_export(
                &snapshot, fake_export, NULL, NULL) ==
                RDNA4_ARGUMENT_LIMIT_RESULT_EXPORT_MALFORMED,
            "missing read-only source flag fails closed");

    fake_export_raw = valid_raw(0);
    fake_export_raw.build_identity[0] = '\0';
    memset(&snapshot, 0, sizeof(snapshot));
    require(rdna4_argument_limit_snapshot_init_from_export(
                &snapshot, fake_export, NULL, NULL) ==
                RDNA4_ARGUMENT_LIMIT_RESULT_EXPORT_MALFORMED,
            "missing build identity fails closed");

    fake_export_raw = valid_raw(0);
    snprintf(fake_export_raw.build_identity,
             sizeof(fake_export_raw.build_identity), "rdal-v1 ");
    memset(&snapshot, 0, sizeof(snapshot));
    require(rdna4_argument_limit_snapshot_init_from_export(
                &snapshot, fake_export, NULL, NULL) ==
                RDNA4_ARGUMENT_LIMIT_RESULT_EXPORT_MALFORMED,
            "prefix-only build identity fails closed");

    fake_export_result = -EINVAL;
    memset(&snapshot, 0, sizeof(snapshot));
    require(rdna4_argument_limit_snapshot_init_from_export(
                &snapshot, fake_export, NULL, NULL) ==
                RDNA4_ARGUMENT_LIMIT_RESULT_EXPORT_CALL_FAILED &&
                snapshot.export_result == -EINVAL &&
                snapshot.state == RDNA4_ARGUMENT_LIMIT_STATE_UNAVAILABLE,
            "negative export result is retained and unavailable");
    fake_export_result = 0;

    rdna4_radv_darwin_host_facts_v1 facts;
    memset(&facts, 0, sizeof(facts));
    facts.struct_size = sizeof(facts);
    facts.valid_flags = RDNA4_RADV_DARWIN_FACT_VRAM_BYTES;
    facts.vram_bytes = UINT64_C(8) << 30;
    fake_export_raw = valid_raw(0);
    memset(&snapshot, 0, sizeof(snapshot));
    require(rdna4_argument_limit_snapshot_init_from_export(
                &snapshot, fake_export, &facts, NULL) ==
                RDNA4_ARGUMENT_LIMIT_RESULT_EXPORT_MALFORMED,
            "VRAM facts require VALID_MEMORY output");

    rdna4_argument_limit_policy bad = rdna4_argument_limit_default_policy();
    bad.objects_per_configuration = 4;
    fake_export_raw = valid_raw(0);
    memset(&snapshot, 0, sizeof(snapshot));
    require(rdna4_argument_limit_snapshot_init_from_export(
                &snapshot, fake_export, NULL, &bad) ==
                RDNA4_ARGUMENT_LIMIT_RESULT_POLICY_INVALID,
            "policy rejects more than three backend handles");
}

static void test_crosscheck(void) {
    rdna4_argument_limit_snapshot snapshot;
    memset(&snapshot, 0, sizeof(snapshot));
    fake_export_raw = valid_raw(1);
    require(rdna4_argument_limit_snapshot_init_from_export(
                &snapshot, fake_export, NULL, NULL) ==
                RDNA4_ARGUMENT_LIMIT_RESULT_EXPORT_MALFORMED,
            "VALID_MEMORY without VRAM facts is rejected");

    /*
     * Build a valid memory snapshot by using the same permissive facts shape
     * accepted by the 0022 export fixture.
     */
    rdna4_radv_darwin_host_facts_v1 facts;
    memset(&facts, 0, sizeof(facts));
    facts.struct_size = sizeof(facts);
    facts.valid_flags = RDNA4_RADV_DARWIN_FACT_VENDOR_ID |
                        RDNA4_RADV_DARWIN_FACT_DEVICE_ID |
                        RDNA4_RADV_DARWIN_FACT_GC_VERSION |
                        RDNA4_RADV_DARWIN_FACT_VRAM_BYTES;
    facts.vendor_id = 0x1002;
    facts.device_id = 0x7590;
    facts.gc_version = 0x0c0000;
    facts.vram_bytes = UINT64_C(8) << 30;
    memset(&snapshot, 0, sizeof(snapshot));
    require(rdna4_argument_limit_snapshot_init_from_export(
                &snapshot, fake_export, &facts, NULL) ==
                RDNA4_ARGUMENT_LIMIT_RESULT_OK,
            "full facts and memory output become ready");

    rdna4_argument_limit_open_observation o = observation_from_raw(&snapshot.raw);
    rdna4_argument_limit_snapshot before = snapshot;
    require(rdna4_argument_limit_snapshot_crosscheck(&snapshot, &o) ==
                RDNA4_ARGUMENT_LIMIT_RESULT_OK &&
                !memcmp(&before, &snapshot, sizeof(snapshot)),
            "matching full-open values validate without updating the snapshot");

    o.api_version++;
    require(rdna4_argument_limit_snapshot_crosscheck(&snapshot, &o) ==
                RDNA4_ARGUMENT_LIMIT_RESULT_CROSSCHECK_IDENTITY_MISMATCH,
            "API identity mismatch is named");

    o = observation_from_raw(&snapshot.raw);
    o.max_per_stage_descriptor_update_after_bind_samplers--;
    require(rdna4_argument_limit_snapshot_crosscheck(&snapshot, &o) ==
                RDNA4_ARGUMENT_LIMIT_RESULT_CROSSCHECK_LIMIT_MISMATCH,
            "raw UAB limit mismatch is named");

#define REQUIRE_LIMIT_FIELD_MISMATCH(field)                                  \
    do {                                                                     \
        rdna4_argument_limit_open_observation changed =                      \
            observation_from_raw(&snapshot.raw);                             \
        require(changed.field > 0, #field " fixture is nonzero");            \
        --changed.field;                                                     \
        require(rdna4_argument_limit_snapshot_crosscheck(                    \
                    &snapshot, &changed) ==                                  \
                    RDNA4_ARGUMENT_LIMIT_RESULT_CROSSCHECK_LIMIT_MISMATCH,   \
                #field " mismatch is compared");                             \
    } while (0)
    REQUIRE_LIMIT_FIELD_MISMATCH(max_descriptor_set_size);
    REQUIRE_LIMIT_FIELD_MISMATCH(max_sampler_allocations);
    REQUIRE_LIMIT_FIELD_MISMATCH(max_bound_descriptor_sets);
    REQUIRE_LIMIT_FIELD_MISMATCH(max_per_set_descriptors);
    REQUIRE_LIMIT_FIELD_MISMATCH(max_storage_buffer_range);
    REQUIRE_LIMIT_FIELD_MISMATCH(max_uab_descriptors_all_pools);
    REQUIRE_LIMIT_FIELD_MISMATCH(max_per_stage_descriptor_samplers);
    REQUIRE_LIMIT_FIELD_MISMATCH(max_per_stage_descriptor_uniform_buffers);
    REQUIRE_LIMIT_FIELD_MISMATCH(max_per_stage_descriptor_storage_buffers);
    REQUIRE_LIMIT_FIELD_MISMATCH(max_per_stage_descriptor_sampled_images);
    REQUIRE_LIMIT_FIELD_MISMATCH(max_per_stage_descriptor_storage_images);
    REQUIRE_LIMIT_FIELD_MISMATCH(max_per_stage_descriptor_input_attachments);
    REQUIRE_LIMIT_FIELD_MISMATCH(max_per_stage_resources);
    REQUIRE_LIMIT_FIELD_MISMATCH(max_descriptor_set_samplers);
    REQUIRE_LIMIT_FIELD_MISMATCH(max_descriptor_set_uniform_buffers);
    REQUIRE_LIMIT_FIELD_MISMATCH(
        max_descriptor_set_uniform_buffers_dynamic);
    REQUIRE_LIMIT_FIELD_MISMATCH(max_descriptor_set_storage_buffers);
    REQUIRE_LIMIT_FIELD_MISMATCH(
        max_descriptor_set_storage_buffers_dynamic);
    REQUIRE_LIMIT_FIELD_MISMATCH(max_descriptor_set_sampled_images);
    REQUIRE_LIMIT_FIELD_MISMATCH(max_descriptor_set_storage_images);
    REQUIRE_LIMIT_FIELD_MISMATCH(max_descriptor_set_input_attachments);
    REQUIRE_LIMIT_FIELD_MISMATCH(
        max_per_stage_descriptor_update_after_bind_samplers);
    REQUIRE_LIMIT_FIELD_MISMATCH(
        max_per_stage_descriptor_update_after_bind_uniform_buffers);
    REQUIRE_LIMIT_FIELD_MISMATCH(
        max_per_stage_descriptor_update_after_bind_storage_buffers);
    REQUIRE_LIMIT_FIELD_MISMATCH(
        max_per_stage_descriptor_update_after_bind_sampled_images);
    REQUIRE_LIMIT_FIELD_MISMATCH(
        max_per_stage_descriptor_update_after_bind_storage_images);
    REQUIRE_LIMIT_FIELD_MISMATCH(
        max_per_stage_descriptor_update_after_bind_input_attachments);
    REQUIRE_LIMIT_FIELD_MISMATCH(
        max_per_stage_update_after_bind_resources);
    REQUIRE_LIMIT_FIELD_MISMATCH(
        max_descriptor_set_update_after_bind_samplers);
    REQUIRE_LIMIT_FIELD_MISMATCH(
        max_descriptor_set_update_after_bind_uniform_buffers);
    REQUIRE_LIMIT_FIELD_MISMATCH(
        max_descriptor_set_update_after_bind_uniform_buffers_dynamic);
    REQUIRE_LIMIT_FIELD_MISMATCH(
        max_descriptor_set_update_after_bind_storage_buffers);
    REQUIRE_LIMIT_FIELD_MISMATCH(
        max_descriptor_set_update_after_bind_storage_buffers_dynamic);
    REQUIRE_LIMIT_FIELD_MISMATCH(
        max_descriptor_set_update_after_bind_sampled_images);
    REQUIRE_LIMIT_FIELD_MISMATCH(
        max_descriptor_set_update_after_bind_storage_images);
    REQUIRE_LIMIT_FIELD_MISMATCH(
        max_descriptor_set_update_after_bind_input_attachments);
    REQUIRE_LIMIT_FIELD_MISMATCH(
        max_update_after_bind_descriptors_in_all_pools);
    REQUIRE_LIMIT_FIELD_MISMATCH(max_fragment_combined_output_resources);
    REQUIRE_LIMIT_FIELD_MISMATCH(max_color_attachments);
    REQUIRE_LIMIT_FIELD_MISMATCH(max_fragment_output_attachments);
    REQUIRE_LIMIT_FIELD_MISMATCH(max_fragment_dual_src_attachments);
    REQUIRE_LIMIT_FIELD_MISMATCH(max_memory_allocation_size);
    REQUIRE_LIMIT_FIELD_MISMATCH(max_buffer_size);
#undef REQUIRE_LIMIT_FIELD_MISMATCH

    o = observation_from_raw(&snapshot.raw);
    o.max_buffer_size--;
    require(rdna4_argument_limit_snapshot_crosscheck(&snapshot, &o) ==
                RDNA4_ARGUMENT_LIMIT_RESULT_CROSSCHECK_LIMIT_MISMATCH,
            "memory limit mismatch is named");

    o = observation_from_raw(&snapshot.raw);
    o.observed_flags &= ~RDNA4_ARGUMENT_LIMIT_OBSERVED_MEMORY_LIMITS;
    require(rdna4_argument_limit_snapshot_crosscheck(&snapshot, &o) ==
                RDNA4_ARGUMENT_LIMIT_RESULT_CROSSCHECK_LIMIT_MISMATCH,
            "missing required open memory observation fails closed");

    o = observation_from_raw(&snapshot.raw);
    o.observed_flags &= ~RDNA4_ARGUMENT_LIMIT_OBSERVED_DESCRIPTOR_LIMITS;
    require(rdna4_argument_limit_snapshot_crosscheck(&snapshot, &o) ==
                RDNA4_ARGUMENT_LIMIT_RESULT_CROSSCHECK_UNAVAILABLE,
            "incomplete core observation is unavailable, not a match");
    require(!memcmp(&before, &snapshot, sizeof(snapshot)),
            "all crosscheck attempts leave the original snapshot fixed");
}

static void test_source_identity(void) {
    char path[] = "/tmp/rdna4-limit-snapshot-XXXXXX";
    int fd = mkstemp(path);
    require(fd >= 0, "source identity fixture can be created");
    require(write(fd, "source-a", 8) == 8, "source identity fixture writes");
    close(fd);

    fake_loader_context context = {
        .symbol = (void *)fake_export,
    };
    rdna4_argument_limit_loader loader = {
        .context = &context,
        .open_library = fake_open,
        .close_library = fake_close,
        .resolve_symbol = fake_resolve,
    };
    rdna4_argument_limit_snapshot snapshot;
    memset(&snapshot, 0, sizeof(snapshot));
    fake_export_raw = valid_raw(0);
    fake_export_result = 0;
    require(rdna4_argument_limit_snapshot_init_from_loader(
                &snapshot, &loader, path, NULL, NULL) ==
                RDNA4_ARGUMENT_LIMIT_RESULT_OK &&
                snapshot.source_stat_valid,
            "snapshot retains source file identity");
    require(rdna4_argument_limit_snapshot_source_matches_path(&snapshot, path) ==
                RDNA4_ARGUMENT_LIMIT_RESULT_OK,
            "unchanged source path matches");

    fd = open(path, O_WRONLY | O_APPEND);
    require(fd >= 0 && write(fd, "b", 1) == 1, "source can be changed");
    close(fd);
    require(rdna4_argument_limit_snapshot_source_matches_path(&snapshot, path) ==
                RDNA4_ARGUMENT_LIMIT_RESULT_SOURCE_CHANGED,
            "changed source path is detected");
    unlink(path);
}

static int fake_export_from_vram_fact(
    const rdna4_radv_darwin_host_facts_v1 *facts,
    rdna4_radv_darwin_argument_limits_v1 *out) {
    ++fake_export_calls;
    if (!facts || facts->struct_size != sizeof(*facts) ||
        !(facts->valid_flags & RDNA4_RADV_DARWIN_FACT_VRAM_BYTES) ||
        !facts->vram_bytes)
        return -EINVAL;
    const uint64_t align = UINT64_C(256) << 20;
    if (facts->vram_bytes > UINT64_MAX - (align - 1)) return -EINVAL;
    const uint64_t fixed = (facts->vram_bytes + align - 1) & ~(align - 1);
    const uint64_t ceiling = UINT64_C(8) << 30;
    const uint64_t limit = fixed < ceiling ? fixed : ceiling;
    *out = valid_raw(1);
    out->max_memory_allocation_size = limit;
    out->max_buffer_size = limit;
    return 0;
}

static void test_8gb_pool_crosscheck(void) {
    const uint64_t pool_8g = UINT64_C(7288) << 20;
    const uint64_t phys_8g = UINT64_C(8112) << 20;
    const uint64_t aligned_pool_8g = UINT64_C(7424) << 20;

    rdna4_radv_darwin_host_facts_v1 facts;
    memset(&facts, 0, sizeof(facts));
    facts.struct_size = sizeof(facts);
    facts.valid_flags = RDNA4_RADV_DARWIN_FACT_VENDOR_ID |
                        RDNA4_RADV_DARWIN_FACT_DEVICE_ID |
                        RDNA4_RADV_DARWIN_FACT_GC_VERSION |
                        RDNA4_RADV_DARWIN_FACT_VRAM_BYTES;
    facts.vendor_id = 0x1002;
    facts.device_id = 0x7590;
    facts.gc_version = 0x0c0000;

    /* RADV full-open calculates limits from vram_vis_total + vram_hi_total (pool). */
    facts.vram_bytes = pool_8g;
    rdna4_argument_limit_snapshot pool_snap;
    memset(&pool_snap, 0, sizeof(pool_snap));
    require(rdna4_argument_limit_snapshot_init_from_export(
                &pool_snap, fake_export_from_vram_fact, &facts, NULL) ==
                RDNA4_ARGUMENT_LIMIT_RESULT_OK &&
                pool_snap.raw.max_memory_allocation_size == aligned_pool_8g &&
                pool_snap.raw.max_buffer_size == aligned_pool_8g,
            "8GB pool fact rounds up to 7424 MiB");

    rdna4_argument_limit_open_observation full_open =
        observation_from_raw(&pool_snap.raw);
    full_open.max_memory_allocation_size = aligned_pool_8g;
    full_open.max_buffer_size = aligned_pool_8g;
    require(rdna4_argument_limit_snapshot_crosscheck(&pool_snap, &full_open) ==
                RDNA4_ARGUMENT_LIMIT_RESULT_OK,
            "8GB pool pre-open snapshot matches full RADV open");

    /* Old bug: passing physical (~8112 MiB -> 8 GiB) mismatches full-open pool (7424 MiB). */
    facts.vram_bytes = phys_8g;
    rdna4_argument_limit_snapshot phys_snap;
    memset(&phys_snap, 0, sizeof(phys_snap));
    require(rdna4_argument_limit_snapshot_init_from_export(
                &phys_snap, fake_export_from_vram_fact, &facts, NULL) ==
                RDNA4_ARGUMENT_LIMIT_RESULT_OK &&
                phys_snap.raw.max_memory_allocation_size == (UINT64_C(8) << 30),
            "8GB raw physical rounds up to 8 GiB");
    require(rdna4_argument_limit_snapshot_crosscheck(&phys_snap, &full_open) ==
                RDNA4_ARGUMENT_LIMIT_RESULT_CROSSCHECK_LIMIT_MISMATCH,
            "8GB raw physical snapshot mismatches full-open pool limit");
}

int main(int argc, char **argv) {
    test_sizes_and_plan();
    test_admission();
    test_reservation_arithmetic();
    test_missing_and_malformed();
    test_crosscheck();
    test_8gb_pool_crosscheck();
    test_source_identity();

    if (argc == 2) {
        rdna4_argument_limit_snapshot snapshot;
        memset(&snapshot, 0, sizeof(snapshot));
        require(rdna4_argument_limit_snapshot_init_from_path(
                    &snapshot, argv[1], NULL, NULL) ==
                    RDNA4_ARGUMENT_LIMIT_RESULT_OK,
                "default dlopen/dlsym loader reads the versioned provider");
        uint32_t limit = 0;
        require(rdna4_argument_limit_snapshot_public_limit(&snapshot, &limit) ==
                    RDNA4_ARGUMENT_LIMIT_RESULT_OK &&
                    limit == 21844u,
                "dlopen-loaded provider computes the same frozen limit");

        void *handle = dlopen(argv[1], RTLD_NOW | RTLD_LOCAL);
        require(handle != NULL,
                "same-handle fixture opens the exact provider path");
        rdna4_argument_limit_snapshot same_handle;
        memset(&same_handle, 0, sizeof(same_handle));
        require(rdna4_argument_limit_snapshot_init_from_handle(
                    &same_handle, handle, argv[1], NULL, NULL) ==
                    RDNA4_ARGUMENT_LIMIT_RESULT_OK &&
                    !memcmp(&same_handle.raw, &snapshot.raw,
                            sizeof(snapshot.raw)) &&
                    !strcmp(same_handle.build_identity,
                            snapshot.build_identity),
                "same-handle re-export matches the frozen raw snapshot");
        require(rdna4_argument_limit_snapshot_init_from_handle(
                    &same_handle, handle, argv[1], NULL, NULL) ==
                    RDNA4_ARGUMENT_LIMIT_RESULT_ALREADY_FIXED,
                "same-handle observation is itself immutable");
        dlclose(handle);
    }

    printf("ARGUMENT_LIMIT_SNAPSHOT_PASS checks=%u gpu_execution=0 "
           "vulkan_object=0 native_open=0\n",
           checks);
    return 0;
}
