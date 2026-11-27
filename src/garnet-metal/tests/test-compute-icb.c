/* Pure-C host verification for Metal 3 Compute ICB ABI, validation, SPIR-V expander,
 * compute indirect SPIR-V rewriter, source pins, and (with -DRDNA4_TEST_LAVAPIPE)
 * live Vulkan 1.4 LavaPipe GPU execution of the compute ICB pipeline. */

#include <assert.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(__has_include)
#if __has_include(<vulkan/vulkan.h>)
#define RDNA4_TEST_HAS_VULKAN_H 1
#endif
#elif defined(RDNA4_TEST_LAVAPIPE)
#define RDNA4_TEST_HAS_VULKAN_H 1
#endif

#ifdef RDNA4_TEST_LAVAPIPE
#define VK_NO_PROTOTYPES
#include <vulkan/vulkan.h>
#include <dlfcn.h>
#endif

#ifdef RDNA4_TEST_HAS_VULKAN_H
#include "garnet_spvrefl.h"
#endif
#include "garnet_icb_abi.h"

/* -------------------------------------------------------------------------
 * 1. Exact 640-byte ABI parity with translator patch 0072
 * ------------------------------------------------------------------------- */
_Static_assert(sizeof(rdna4_icb_header_t) == 64, "header is 64 bytes");
_Static_assert(sizeof(rdna4_icb_slot_t) == 640, "render slot is 640 bytes");
_Static_assert(sizeof(rdna4_icb_compute_slot_t) == 640, "compute slot is 640 bytes");
_Static_assert(RDNA4_ICB_HEADER_BYTES == 64u, "header bytes macro");
_Static_assert(RDNA4_ICB_SLOT_BYTES == 640u, "slot bytes macro");
_Static_assert(RDNA4_ICB_TABLE_BINDING == 676u, "compute ICB BDA table binding == 676");
_Static_assert(RDNA4_ICB_COMPUTE_PSO_BIT == 0x80000000ULL, "compute PSO token bit");
_Static_assert(RDNA4_ICB_KIND_CONCURRENT_DISPATCH == 5u, "kind ConcurrentDispatch == 5");
_Static_assert(RDNA4_ICB_KIND_CONCURRENT_DISPATCH_THREADS == 6u, "kind ConcurrentDispatchThreads == 6");
_Static_assert(RDNA4_ICB_SLOT_FLAG_PIPELINE_SET == 1u, "flag pipeline_set");
_Static_assert(RDNA4_ICB_SLOT_FLAG_INVALID == 2u, "flag invalid");
_Static_assert(RDNA4_ICB_SLOT_FLAG_BARRIER == 4u, "flag barrier");
_Static_assert(RDNA4_ICB_SLOT_FLAG_STAGE_IN_REGION == 8u, "flag stage_in_region");
_Static_assert(RDNA4_ICB_CMD_BIT_CONCURRENT_DISPATCH == (1u << 5), "cmd bit ConcurrentDispatch");
_Static_assert(RDNA4_ICB_CMD_BIT_CONCURRENT_DISPATCH_THREADS == (1u << 6), "cmd bit ConcurrentDispatchThreads");

/* Cross-check every 0072 field offset inside the 640-byte compute slot. */
_Static_assert(offsetof(rdna4_icb_compute_slot_t, kind) == 0, "0072 kind @ 0");
_Static_assert(offsetof(rdna4_icb_compute_slot_t, reserved0) == 4, "0072 reserved0 @ 4");
_Static_assert(offsetof(rdna4_icb_compute_slot_t, reserved1) == 8, "0072 reserved1 @ 8");
_Static_assert(offsetof(rdna4_icb_compute_slot_t, flags) == 12, "0072 flags @ 12");
_Static_assert(offsetof(rdna4_icb_compute_slot_t, pipeline_token) == 16, "0072 pipeline_token @ 16");
_Static_assert(offsetof(rdna4_icb_compute_slot_t, kernel_bind_mask) == 24, "0072 kernel_bind_mask @ 24");
_Static_assert(offsetof(rdna4_icb_compute_slot_t, threadgroup_mem_mask) == 28, "0072 threadgroup_mem_mask @ 28");
_Static_assert(offsetof(rdna4_icb_compute_slot_t, grid_size) == 32, "0072 grid_size @ 32");
_Static_assert(offsetof(rdna4_icb_compute_slot_t, threadgroup_size) == 44, "0072 threadgroup_size @ 44");
_Static_assert(offsetof(rdna4_icb_compute_slot_t, stage_in_origin) == 56, "0072 stage_in_origin @ 56");
_Static_assert(offsetof(rdna4_icb_compute_slot_t, stage_in_size) == 68, "0072 stage_in_size @ 68");
_Static_assert(offsetof(rdna4_icb_compute_slot_t, kernel_bda) == 96, "0072 kernel_bda @ 96");
_Static_assert(offsetof(rdna4_icb_compute_slot_t, threadgroup_mem_length) == 352, "0072 tg_mem_length @ 352");
_Static_assert(offsetof(rdna4_icb_compute_slot_t, pad) == 480, "0072 pad @ 480");

/* Cross-check compute expander config offsets used by rdna4_icb_emit_compute_expand_spv. */
_Static_assert(sizeof(rdna4_icb_compute_bucket_t) == 32, "compute bucket == 32B");
_Static_assert(offsetof(rdna4_icb_compute_expand_cfg_t, buckets) == 64, "cfg.buckets @ 64");
_Static_assert(offsetof(rdna4_icb_compute_expand_cfg_t, parent_k_table) == 8256, "cfg.parent_k_table @ 8256");
_Static_assert(sizeof(rdna4_icb_compute_expand_cfg_t) == 9024, "cfg size == 9024");
_Static_assert(RDNA4_ICB_TABLE_V2_STRIDE == sizeof(rdna4_buffer_table), "table stride == 768B");

static void test_command_types_and_validators(void) {
    /* Render-only valid */
    assert(rdna4_icb_command_types_valid(RDNA4_ICB_CMD_BIT_DRAW));
    assert(rdna4_icb_command_types_valid(RDNA4_ICB_RENDER_CMD_MASK));
    assert(rdna4_icb_is_render_types(RDNA4_ICB_RENDER_CMD_MASK));
    assert(!rdna4_icb_is_compute_types(RDNA4_ICB_RENDER_CMD_MASK));

    /* Compute-only valid */
    assert(rdna4_icb_command_types_valid(RDNA4_ICB_CMD_BIT_CONCURRENT_DISPATCH));
    assert(rdna4_icb_command_types_valid(RDNA4_ICB_CMD_BIT_CONCURRENT_DISPATCH_THREADS));
    assert(rdna4_icb_command_types_valid(RDNA4_ICB_COMPUTE_CMD_MASK));
    assert(rdna4_icb_is_compute_types(RDNA4_ICB_COMPUTE_CMD_MASK));
    assert(!rdna4_icb_is_render_types(RDNA4_ICB_COMPUTE_CMD_MASK));

    /* Disallowed: empty, mixed render+compute, unknown/mesh bits */
    assert(!rdna4_icb_command_types_valid(0u));
    assert(!rdna4_icb_command_types_valid(RDNA4_ICB_CMD_BIT_DRAW | RDNA4_ICB_CMD_BIT_CONCURRENT_DISPATCH));
    assert(!rdna4_icb_command_types_valid(RDNA4_ICB_CMD_BIT_DRAW_INDEXED | RDNA4_ICB_CMD_BIT_CONCURRENT_DISPATCH_THREADS));
    assert(!rdna4_icb_command_types_valid(RDNA4_ICB_CMD_BIT_CONCURRENT_DISPATCH | (1u << 4)));
    assert(!rdna4_icb_command_types_valid(1u << 7));
    assert(!rdna4_icb_command_types_valid(0xffffffffu));

    /* Threadgroup memory validator */
    assert(rdna4_icb_compute_tg_mem_ok(0u, 0u, 65536u));
    assert(rdna4_icb_compute_tg_mem_ok(0u, 16u, 65536u));
    assert(rdna4_icb_compute_tg_mem_ok(31u, 65536u, 65536u));
    assert(!rdna4_icb_compute_tg_mem_ok(32u, 16u, 65536u));
    assert(!rdna4_icb_compute_tg_mem_ok(0u, 12u, 65536u));       /* not 16B aligned */
    assert(!rdna4_icb_compute_tg_mem_ok(0u, 65552u, 65536u));    /* > device limit */
    assert(!rdna4_icb_compute_tg_mem_ok(0u, (uint64_t)UINT32_MAX + 16ull, 0u));

    /* Stage-in region validator */
    {
        const uint64_t o0[3] = { 0, 0, 0 }, s0[3] = { 128, 64, 1 };
        const uint64_t o1[3] = { UINT32_MAX - 10u, 0, 0 }, s1[3] = { 10u, 1u, 1u }, s2[3] = { 11u, 1u, 1u };
        const uint64_t o3[3] = { (uint64_t)UINT32_MAX + 1ull, 0, 0 }, s3[3] = { 1u, 1u, 1u };
        assert(rdna4_icb_compute_stage_in_ok(o0, s0));
        assert(rdna4_icb_compute_stage_in_ok(o1, s1));
        assert(!rdna4_icb_compute_stage_in_ok(o1, s2)); /* overflow */
        assert(!rdna4_icb_compute_stage_in_ok(o3, s3));
    }

    /* Dispatch group ceiling math */
    uint32_t out[3] = { 99, 99, 99 }, act[3] = { 0, 0, 0 };
    const uint32_t g_tg[3] = { 7, 3, 2 };
    const uint32_t tg64[3] = { 64, 1, 1 };
    assert(rdna4_icb_compute_dispatch_groups(RDNA4_ICB_KIND_CONCURRENT_DISPATCH, g_tg, tg64, out, act) == 1);
    assert(out[0] == 7u && out[1] == 3u && out[2] == 2u);
    assert(act[0] == 448u && act[1] == 3u && act[2] == 2u);

    const uint32_t g_th_exact[3] = { 128, 8, 4 };
    const uint32_t tg_2d[3] = { 16, 4, 2 };
    assert(rdna4_icb_compute_dispatch_groups(RDNA4_ICB_KIND_CONCURRENT_DISPATCH_THREADS, g_th_exact, tg_2d, out, act) == 1);
    assert(out[0] == 8u && out[1] == 2u && out[2] == 2u);
    assert(act[0] == 128u && act[1] == 8u && act[2] == 4u);

    const uint32_t g_th_partial[3] = { 100, 5, 1 };
    assert(rdna4_icb_compute_dispatch_groups(RDNA4_ICB_KIND_CONCURRENT_DISPATCH_THREADS, g_th_partial, tg_2d, out, act) == 1);
    assert(out[0] == 7u && out[1] == 2u && out[2] == 1u);
    assert(act[0] == 100u && act[1] == 5u && act[2] == 1u);

    /* Overflow-safe ceiling division at UINT32_MAX */
    const uint32_t g_max[3] = { UINT32_MAX, UINT32_MAX, 1u };
    assert(rdna4_icb_compute_dispatch_groups(RDNA4_ICB_KIND_CONCURRENT_DISPATCH_THREADS, g_max, tg64, out, act) == 1);
    assert(out[0] == ((UINT32_MAX - 1u) / 64u + 1u));
    assert(out[1] == UINT32_MAX);
    assert(out[2] == 1u);
    assert(act[0] == UINT32_MAX);

    /* Zero grid -> 0 groups, valid no-op */
    const uint32_t g_zero[3] = { 10, 0, 1 };
    assert(rdna4_icb_compute_dispatch_groups(RDNA4_ICB_KIND_CONCURRENT_DISPATCH_THREADS, g_zero, tg64, out, act) == 1);
    assert(out[0] == 0u && out[1] == 0u && out[2] == 0u);
    assert(act[0] == 0u && act[1] == 0u && act[2] == 0u);

    /* Zero threadgroup -> rejected */
    const uint32_t tg_zero[3] = { 64, 0, 1 };
    assert(rdna4_icb_compute_dispatch_groups(RDNA4_ICB_KIND_CONCURRENT_DISPATCH_THREADS, g_th_exact, tg_zero, out, act) == 0);
}

static void test_compute_slot_mutators(void) {
    rdna4_icb_header_t hdr;
    memset(&hdr, 0, sizeof hdr);
    hdr.magic = RDNA4_ICB_MAGIC;
    hdr.max_command_count = 8u;
    hdr.command_types = RDNA4_ICB_COMPUTE_CMD_MASK;
    hdr.flags = 0u;
    hdr.max_vertex_binds = 8u;
    hdr.slot_stride = RDNA4_ICB_SLOT_BYTES;

    rdna4_icb_compute_slot_t slot;
    memset(&slot, 0xaa, sizeof slot);
    rdna4_icb_compute_slot_reset(&slot);
    assert(slot.kind == RDNA4_ICB_KIND_EMPTY && slot.flags == 0u && slot.pipeline_token == 0ull);

    const uint64_t comp_tok = RDNA4_ICB_PSO_TOKEN_TAG | RDNA4_ICB_COMPUTE_PSO_BIT | 1ull;

    /* Reject zero pipeline token on compute slot */
    assert(!rdna4_icb_compute_slot_set_pipeline(&slot, hdr.flags, 0ull));
    assert((slot.flags & RDNA4_ICB_SLOT_FLAG_INVALID) != 0u);

    /* Reset and encode full valid slot */
    rdna4_icb_compute_slot_reset(&slot);
    assert(rdna4_icb_compute_slot_set_pipeline(&slot, hdr.flags, comp_tok));
    assert(slot.pipeline_token == comp_tok);
    assert((slot.flags & RDNA4_ICB_SLOT_FLAG_PIPELINE_SET) != 0u);

    assert(rdna4_icb_compute_slot_set_kernel_buffer(&slot, hdr.flags, hdr.max_vertex_binds, 0u, 0x100000ull));
    assert(rdna4_icb_compute_slot_set_kernel_buffer(&slot, hdr.flags, hdr.max_vertex_binds, 7u, 0x200000ull));
    assert(slot.kernel_bind_mask == ((1u << 0) | (1u << 7)));
    assert(slot.kernel_bda[0] == 0x100000ull && slot.kernel_bda[7] == 0x200000ull);
    /* OOB buffer index (>= max_vertex_binds=8) rejected */
    assert(!rdna4_icb_compute_slot_set_kernel_buffer(&slot, hdr.flags, hdr.max_vertex_binds, 8u, 0x300000ull));
    assert((slot.flags & RDNA4_ICB_SLOT_FLAG_INVALID) != 0u);

    /* Re-encode cleanly with tg mem, stage-in, barrier, and dispatch */
    rdna4_icb_compute_slot_reset(&slot);
    assert(rdna4_icb_compute_slot_set_pipeline(&slot, hdr.flags, comp_tok));
    assert(rdna4_icb_compute_slot_set_kernel_buffer(&slot, hdr.flags, hdr.max_vertex_binds, 2u, 0x400000ull));
    assert(rdna4_icb_compute_slot_set_tg_mem(&slot, 0u, 256u));
    assert(rdna4_icb_compute_slot_set_tg_mem(&slot, 31u, 1024u));
    assert(slot.threadgroup_mem_mask == ((1u << 0) | (1u << 31)));
    assert(slot.threadgroup_mem_length[0] == 256u && slot.threadgroup_mem_length[31] == 1024u);
    assert(!rdna4_icb_compute_slot_set_tg_mem(&slot, 32u, 16u));
    assert((slot.flags & RDNA4_ICB_SLOT_FLAG_INVALID) != 0u);

    rdna4_icb_compute_slot_reset(&slot);
    assert(rdna4_icb_compute_slot_set_pipeline(&slot, hdr.flags, comp_tok));
    assert(rdna4_icb_compute_slot_set_kernel_buffer(&slot, hdr.flags, hdr.max_vertex_binds, 2u, 0x400000ull));
    assert(rdna4_icb_compute_slot_set_tg_mem(&slot, 0u, 256u));
    assert(rdna4_icb_compute_slot_set_tg_mem(&slot, 31u, 1024u));
    const uint32_t orig[3] = { 1u, 2u, 3u }, rsz[3] = { 16u, 32u, 4u };
    assert(rdna4_icb_compute_slot_set_stage_in(&slot, orig, rsz));
    assert((slot.flags & RDNA4_ICB_SLOT_FLAG_STAGE_IN_REGION) != 0u);
    assert(slot.stage_in_origin[0] == 1u && slot.stage_in_origin[1] == 2u && slot.stage_in_origin[2] == 3u);
    assert(slot.stage_in_size[0] == 16u && slot.stage_in_size[1] == 32u && slot.stage_in_size[2] == 4u);

    rdna4_icb_compute_slot_set_barrier(&slot, 1);
    assert((slot.flags & RDNA4_ICB_SLOT_FLAG_BARRIER) != 0u);
    rdna4_icb_compute_slot_set_barrier(&slot, 0);
    assert((slot.flags & RDNA4_ICB_SLOT_FLAG_BARRIER) == 0u);
    rdna4_icb_compute_slot_set_barrier(&slot, 1);

    const uint32_t g[3] = { 100u, 1u, 1u }, tg[3] = { 32u, 1u, 1u };
    assert(rdna4_icb_compute_slot_set_dispatch(&slot, hdr.command_types, hdr.flags, RDNA4_ICB_KIND_CONCURRENT_DISPATCH_THREADS, g, tg));
    assert(slot.kind == RDNA4_ICB_KIND_CONCURRENT_DISPATCH_THREADS);
    assert(slot.grid_size[0] == 100u && slot.threadgroup_size[0] == 32u);
    assert((slot.flags & RDNA4_ICB_SLOT_FLAG_BARRIER) != 0u);

    /* Inherit flags forbid explicit pipeline/buffer mutations, allow dispatch without set_pipeline */
    rdna4_icb_header_t inh = hdr;
    inh.flags = RDNA4_ICB_FLAG_INHERIT_PIPELINE | RDNA4_ICB_FLAG_INHERIT_BUFFERS;
    inh.command_types = RDNA4_ICB_CMD_BIT_CONCURRENT_DISPATCH;
    rdna4_icb_compute_slot_reset(&slot);
    assert(!rdna4_icb_compute_slot_set_pipeline(&slot, inh.flags, comp_tok));
    rdna4_icb_compute_slot_reset(&slot);
    assert(!rdna4_icb_compute_slot_set_kernel_buffer(&slot, inh.flags, inh.max_vertex_binds, 0u, 0x100000ull));
    rdna4_icb_compute_slot_reset(&slot);
    /* Disallowed command type (Threads not enabled) */
    assert(!rdna4_icb_compute_slot_set_dispatch(&slot, inh.command_types, inh.flags, RDNA4_ICB_KIND_CONCURRENT_DISPATCH_THREADS, g, tg));
    /* Allowed command type (Threadgroups enabled) */
    rdna4_icb_compute_slot_reset(&slot);
    assert(rdna4_icb_compute_slot_set_dispatch(&slot, inh.command_types, inh.flags, RDNA4_ICB_KIND_CONCURRENT_DISPATCH, g, tg));
    assert(slot.kind == RDNA4_ICB_KIND_CONCURRENT_DISPATCH);
}

static void test_compute_expand_spv(void) {
    uint32_t *words = NULL;
    size_t nwords = 0;
    assert(rdna4_icb_emit_compute_expand_spv(&words, &nwords) == 1);
    assert(words != NULL && nwords > 100u);
    assert(words[0] == 0x07230203u);
    assert(words[1] == 0x00010500u);

    int has_local_64 = 0;
    uint32_t bound_mask = 0u;
    for (size_t p = 5u; p < nwords; ) {
        uint32_t sz = words[p] >> 16, op = words[p] & 0xffffu;
        assert(sz > 0u && p + sz <= nwords);
        if (op == 16u && sz == 6u && words[p + 2u] == 17u &&
            words[p + 3u] == 64u && words[p + 4u] == 1u && words[p + 5u] == 1u)
            has_local_64 = 1;
        if (op == 71u && sz == 4u && words[p + 2u] == 33u && words[p + 3u] < 32u)
            bound_mask |= (1u << words[p + 3u]);
        p += sz;
    }
    assert(has_local_64);
    assert(bound_mask == 0x1fu);

#ifdef RDNA4_TEST_HAS_VULKAN_H
    RDNA4sMod mod;
    char why[256] = "";
    assert(rdna4s_parse(words, nwords, &mod, why, sizeof why) == 0);
    assert(mod.has_ep && mod.ep_model == 5u);

    RDNA4sRefl refl;
    assert(rdna4s_reflect(&mod, 2, &refl) == 0);
    for (uint32_t b = 0; b < 5u; b++) {
        assert(refl.bind[0][b].used == 1);
        assert(refl.bind[0][b].type == VK_DESCRIPTOR_TYPE_STORAGE_BUFFER);
        assert(refl.bind[0][b].count == 1u);
    }
    assert(refl.bind[0][5].used == 0);
    rdna4s_free(&mod);
#endif
    free(words);
}

#ifdef RDNA4_TEST_HAS_VULKAN_H
/* -------------------------------------------------------------------------
 * 2. Synthetic BDA compute shader builder for SPIR-V rewriter & LavaPipe
 * ------------------------------------------------------------------------- */
typedef struct {
    int use_vec2_array_table; /* 0: RuntimeArray<u64> StorageBuffer, 1: Array<uvec2, 96> Uniform */
    int with_push_constants;  /* 12-word ThreadsDynamic block */
    int with_barrier;         /* emit OpControlBarrier */
    int with_entry_phi;       /* branch from entry into merge with OpPhi referencing entry label */
    int kernel_mode;          /* 0: out[gid] = in[gid]*2 + active_threads.x; 1: out[gid] = in[gid] + 1000 + gid */
} synth_shader_opts_t;

static void build_synth_compute_spv(const synth_shader_opts_t *opt, uint32_t **out_words, size_t *out_nwords) {
    rdna4_icbc_buf_t dec = {0}, types = {0}, vars = {0}, code = {0}, spv = {0};
    uint32_t nid = 1u;

    uint32_t fn_main = nid++;
    uint32_t t_void = nid++, t_u32 = nid++, t_u64 = nid++, t_v2u32 = nid++, t_v3u32 = nid++, t_fn = nid++;
    uint32_t t_p_in_v3 = nid++, t_tbl_el = 0u, t_tbl_arr = nid++, t_tbl_st = nid++, t_p_tbl = nid++, t_p_tbl_el = nid++;
    uint32_t t_psb_st = nid++, t_p_psb_st = nid++, t_p_psb_u32 = nid++;
    uint32_t t_pc_st = 0u, t_p_pc_st = 0u, t_p_pc_u32 = 0u;
    uint32_t v_gid = nid++, v_tbl = nid++, v_pc = 0u;
    uint32_t c0 = nid++, c1 = nid++, c2 = nid++, c4 = nid++, c96 = nid++, c1000 = nid++;
    uint32_t c_sc2 = nid++, c_sem = nid++;
    uint32_t t_bool = nid++, c_true = nid++;

    uint32_t tbl_sc = opt->use_vec2_array_table ? 2u : 12u; /* Uniform vs StorageBuffer */
    t_tbl_el = opt->use_vec2_array_table ? t_v2u32 : t_u64;

    if (opt->with_push_constants) {
        t_pc_st = nid++; t_p_pc_st = nid++; t_p_pc_u32 = nid++; v_pc = nid++;
    }

    /* Types */
    { uint32_t a[] = { t_void }; rdna4_icbc_emit_op(&types, 19, a, 1); }
    { uint32_t a[] = { t_bool }; rdna4_icbc_emit_op(&types, 20, a, 1); }
    { uint32_t a[] = { t_u32, 32u, 0u }; rdna4_icbc_emit_op(&types, 21, a, 3); }
    { uint32_t a[] = { t_u64, 64u, 0u }; rdna4_icbc_emit_op(&types, 21, a, 3); }
    { uint32_t a[] = { t_v2u32, t_u32, 2u }; rdna4_icbc_emit_op(&types, 23, a, 3); }
    { uint32_t a[] = { t_v3u32, t_u32, 3u }; rdna4_icbc_emit_op(&types, 23, a, 3); }
    { uint32_t a[] = { t_fn, t_void }; rdna4_icbc_emit_op(&types, 33, a, 2); }
    { uint32_t a[] = { t_p_in_v3, 1u, t_v3u32 }; rdna4_icbc_emit_op(&types, 32, a, 3); }
    { uint32_t a[] = { t_u32, c0, 0u }; rdna4_icbc_emit_op(&types, 43, a, 3); }
    { uint32_t a[] = { t_u32, c1, 1u }; rdna4_icbc_emit_op(&types, 43, a, 3); }
    { uint32_t a[] = { t_u32, c2, 2u }; rdna4_icbc_emit_op(&types, 43, a, 3); }
    { uint32_t a[] = { t_u32, c4, 4u }; rdna4_icbc_emit_op(&types, 43, a, 3); }
    { uint32_t a[] = { t_u32, c96, 96u }; rdna4_icbc_emit_op(&types, 43, a, 3); }
    { uint32_t a[] = { t_u32, c1000, 1000u }; rdna4_icbc_emit_op(&types, 43, a, 3); }
    { uint32_t a[] = { t_u32, c_sc2, 2u }; rdna4_icbc_emit_op(&types, 43, a, 3); }
    { uint32_t a[] = { t_u32, c_sem, 0x108u }; rdna4_icbc_emit_op(&types, 43, a, 3); }
    { uint32_t a[] = { t_bool, c_true }; rdna4_icbc_emit_op(&types, 41, a, 2); }

    /* Table struct at binding 676 */
    if (opt->use_vec2_array_table) {
        uint32_t a[] = { t_tbl_arr, t_tbl_el, c96 }; rdna4_icbc_emit_op(&types, 28, a, 3);
    } else {
        uint32_t a[] = { t_tbl_arr, t_tbl_el }; rdna4_icbc_emit_op(&types, 29, a, 2);
    }
    { uint32_t a[] = { t_tbl_st, t_tbl_arr }; rdna4_icbc_emit_op(&types, 30, a, 2); }
    { uint32_t a[] = { t_p_tbl, tbl_sc, t_tbl_st }; rdna4_icbc_emit_op(&types, 32, a, 3); }
    { uint32_t a[] = { t_p_tbl_el, tbl_sc, t_tbl_el }; rdna4_icbc_emit_op(&types, 32, a, 3); }

    /* PhysicalStorageBuffer u32 element wrapper struct { uint32_t v; } */
    { uint32_t a[] = { t_psb_st, t_u32 }; rdna4_icbc_emit_op(&types, 30, a, 2); }
    { uint32_t a[] = { t_p_psb_st, 5349u, t_psb_st }; rdna4_icbc_emit_op(&types, 32, a, 3); }
    { uint32_t a[] = { t_p_psb_u32, 5349u, t_u32 }; rdna4_icbc_emit_op(&types, 32, a, 3); }

    if (opt->with_push_constants) {
        uint32_t a[13];
        a[0] = t_pc_st;
        for (int i = 0; i < 12; i++) a[1 + i] = t_u32;
        rdna4_icbc_emit_op(&types, 30, a, 13);
        uint32_t ap1[] = { t_p_pc_st, 9u, t_pc_st }; rdna4_icbc_emit_op(&types, 32, ap1, 3);
        uint32_t ap2[] = { t_p_pc_u32, 9u, t_u32 }; rdna4_icbc_emit_op(&types, 32, ap2, 3);
    }

    /* Spec constants 0..2 for LocalSizeId (ThreadsDynamic) */
    uint32_t scx = nid++, scy = nid++, scz = nid++, wg_sz = nid++;
    { uint32_t a[] = { t_u32, scx, 32u }; rdna4_icbc_emit_op(&types, 50, a, 3); }
    { uint32_t a[] = { t_u32, scy, 1u }; rdna4_icbc_emit_op(&types, 50, a, 3); }
    { uint32_t a[] = { t_u32, scz, 1u }; rdna4_icbc_emit_op(&types, 50, a, 3); }
    { uint32_t a[] = { t_v3u32, wg_sz, scx, scy, scz }; rdna4_icbc_emit_op(&types, 51, a, 5); }

    /* Decorations */
    rdna4_icbc_dec(&dec, v_gid, 11u, 1, 28u);
    rdna4_icbc_dec(&dec, wg_sz, 11u, 1, 25u); /* WorkgroupSize */
    rdna4_icbc_dec(&dec, scx, 1u, 1, 0u);    /* SpecId 0 */
    rdna4_icbc_dec(&dec, scy, 1u, 1, 1u);    /* SpecId 1 */
    rdna4_icbc_dec(&dec, scz, 1u, 1, 2u);    /* SpecId 2 */
    rdna4_icbc_dec(&dec, t_tbl_arr, 6u, 1, 8u);
    rdna4_icbc_mdec(&dec, t_tbl_st, 0u, 35u, 0u);
    rdna4_icbc_dec(&dec, t_tbl_st, opt->use_vec2_array_table ? 3u : 2u, 0, 0u);
    rdna4_icbc_dec(&dec, v_tbl, 34u, 1, 0u);
    rdna4_icbc_dec(&dec, v_tbl, 33u, 1, RDNA4_ICB_TABLE_BINDING);
    rdna4_icbc_dec(&dec, v_tbl, 24u, 0, 0u);
    rdna4_icbc_mdec(&dec, t_psb_st, 0u, 35u, 0u);
    rdna4_icbc_dec(&dec, t_psb_st, 2u, 0, 0u);
    if (opt->with_push_constants) {
        rdna4_icbc_dec(&dec, t_pc_st, 2u, 0, 0u);
        for (uint32_t m = 0; m < 12u; m++) rdna4_icbc_mdec(&dec, t_pc_st, m, 35u, m * 4u);
    }

    /* Global variables */
    { uint32_t a[] = { t_p_in_v3, v_gid, 1u }; rdna4_icbc_emit_op(&vars, 59, a, 3); }
    { uint32_t a[] = { t_p_tbl, v_tbl, tbl_sc }; rdna4_icbc_emit_op(&vars, 59, a, 3); }
    if (opt->with_push_constants) {
        uint32_t a[] = { t_p_pc_st, v_pc, 9u }; rdna4_icbc_emit_op(&vars, 59, a, 3);
    }

    /* Function body */
    uint32_t l_entry = nid++, l_then = 0u, l_merge = 0u;
    if (opt->with_entry_phi) { l_then = nid++; l_merge = nid++; }
    { uint32_t a[] = { t_void, fn_main, 0u, t_fn }; rdna4_icbc_emit_op(&code, 54, a, 4); }
    { uint32_t a[] = { l_entry }; rdna4_icbc_emit_op(&code, 248, a, 1); }

    uint32_t phi_val = c0;
    if (opt->with_entry_phi) {
        uint32_t a_sm[] = { l_merge, 0u }; rdna4_icbc_emit_op(&code, 247, a_sm, 2);
        uint32_t a_bc[] = { c_true, l_then, l_merge }; rdna4_icbc_emit_op(&code, 250, a_bc, 3);
        { uint32_t a[] = { l_then }; rdna4_icbc_emit_op(&code, 248, a, 1); }
        { uint32_t a[] = { l_merge }; rdna4_icbc_emit_op(&code, 249, a, 1); }
        { uint32_t a[] = { l_merge }; rdna4_icbc_emit_op(&code, 248, a, 1); }
        phi_val = nid++;
        uint32_t a_phi[] = { t_u32, phi_val, c0, l_then, c1, l_entry };
        rdna4_icbc_emit_op(&code, 245, a_phi, 6);
    }

    uint32_t gv = nid++, gx = nid++, gx_u64 = nid++, byte_off = nid++;
    { uint32_t a[] = { t_v3u32, gv, v_gid }; rdna4_icbc_emit_op(&code, 61, a, 3); }
    { uint32_t a[] = { t_u32, gx, gv, 0u }; rdna4_icbc_emit_op(&code, 81, a, 4); }
    { uint32_t a[] = { t_u64, gx_u64, gx }; rdna4_icbc_emit_op(&code, 113, a, 3); }
    uint32_t c4_u64 = nid++;
    { uint32_t a[] = { t_u64, c4_u64, 4u, 0u }; rdna4_icbc_emit_op(&types, 43, a, 4); }
    { uint32_t a[] = { t_u64, byte_off, gx_u64, c4_u64 }; rdna4_icbc_emit_op(&code, 132, a, 4); }

    /* Load BDA for slot 0 (input) and slot 1 (output) from binding 676 */
    uint32_t ac0 = nid++, raw0 = nid++, addr0 = 0u;
    uint32_t ac1 = nid++, raw1 = nid++, addr1 = 0u;
    { uint32_t a[] = { t_p_tbl_el, ac0, v_tbl, c0, c0 }; rdna4_icbc_emit_op(&code, 65, a, 5); }
    { uint32_t a[] = { t_tbl_el, raw0, ac0 }; rdna4_icbc_emit_op(&code, 61, a, 3); }
    { uint32_t a[] = { t_p_tbl_el, ac1, v_tbl, c0, c1 }; rdna4_icbc_emit_op(&code, 65, a, 5); }
    { uint32_t a[] = { t_tbl_el, raw1, ac1 }; rdna4_icbc_emit_op(&code, 61, a, 3); }
    if (opt->use_vec2_array_table) {
        addr0 = nid++; addr1 = nid++;
        uint32_t b0[] = { t_u64, addr0, raw0 }; rdna4_icbc_emit_op(&code, 124, b0, 3); /* OpBitcast */
        uint32_t b1[] = { t_u64, addr1, raw1 }; rdna4_icbc_emit_op(&code, 124, b1, 3);
    } else {
        addr0 = raw0; addr1 = raw1;
    }

    uint32_t p0_addr = nid++, p0_st = nid++, p0_u32 = nid++, in_val = nid++;
    { uint32_t a[] = { t_u64, p0_addr, addr0, byte_off }; rdna4_icbc_emit_op(&code, 128, a, 4); }
    { uint32_t a[] = { t_p_psb_st, p0_st, p0_addr }; rdna4_icbc_emit_op(&code, 120, a, 3); } /* OpConvertUToPtr */
    { uint32_t a[] = { t_p_psb_u32, p0_u32, p0_st, c0 }; rdna4_icbc_emit_op(&code, 65, a, 4); }
    { uint32_t a[] = { t_u32, in_val, p0_u32, 2u, 4u }; rdna4_icbc_emit_op(&code, 61, a, 5); } /* Aligned 4 */

    uint32_t extra = c0;
    if (opt->with_push_constants) {
        uint32_t pc_ac0 = nid++, pc_v0 = nid++, pc_ac3 = nid++, pc_v3 = nid++, pc_ac9 = nid++, pc_v9 = nid++;
        { uint32_t a[] = { t_p_pc_u32, pc_ac0, v_pc, c0 }; rdna4_icbc_emit_op(&code, 65, a, 4); }
        { uint32_t a[] = { t_u32, pc_v0, pc_ac0 }; rdna4_icbc_emit_op(&code, 61, a, 3); } /* threads.x */
        uint32_t c3 = nid++, c9 = nid++;
        { uint32_t a[] = { t_u32, c3, 3u }; rdna4_icbc_emit_op(&types, 43, a, 3); }
        { uint32_t a[] = { t_u32, c9, 9u }; rdna4_icbc_emit_op(&types, 43, a, 3); }
        { uint32_t a[] = { t_p_pc_u32, pc_ac3, v_pc, c3 }; rdna4_icbc_emit_op(&code, 65, a, 4); }
        { uint32_t a[] = { t_u32, pc_v3, pc_ac3 }; rdna4_icbc_emit_op(&code, 61, a, 3); } /* tbase.x -> 0 */
        { uint32_t a[] = { t_p_pc_u32, pc_ac9, v_pc, c9 }; rdna4_icbc_emit_op(&code, 65, a, 4); }
        { uint32_t a[] = { t_u32, pc_v9, pc_ac9 }; rdna4_icbc_emit_op(&code, 61, a, 3); } /* tgpg.x -> NumWorkgroups.x */
        uint32_t sum1 = nid++, sum2 = nid++;
        { uint32_t a[] = { t_u32, sum1, pc_v0, pc_v3 }; rdna4_icbc_emit_op(&code, 128, a, 4); }
        { uint32_t a[] = { t_u32, sum2, sum1, pc_v9 }; rdna4_icbc_emit_op(&code, 128, a, 4); }
        extra = sum2; /* active_threads.x + 0 + NumWorkgroups.x */
    }

    if (opt->with_barrier) {
        uint32_t a[] = { c_sc2, c_sc2, c_sem };
        rdna4_icbc_emit_op(&code, 224, a, 3); /* OpControlBarrier */
    }

    uint32_t res_val = nid++;
    if (opt->kernel_mode == 0) {
        uint32_t twice = nid++, t1 = nid++;
        { uint32_t a[] = { t_u32, twice, in_val, c2 }; rdna4_icbc_emit_op(&code, 132, a, 4); }
        { uint32_t a[] = { t_u32, t1, twice, extra }; rdna4_icbc_emit_op(&code, 128, a, 4); }
        { uint32_t a[] = { t_u32, res_val, t1, phi_val }; rdna4_icbc_emit_op(&code, 128, a, 4); }
    } else {
        uint32_t t1 = nid++;
        { uint32_t a[] = { t_u32, t1, in_val, c1000 }; rdna4_icbc_emit_op(&code, 128, a, 4); }
        { uint32_t a[] = { t_u32, res_val, t1, gx }; rdna4_icbc_emit_op(&code, 128, a, 4); }
    }

    uint32_t p1_addr = nid++, p1_st = nid++, p1_u32 = nid++;
    { uint32_t a[] = { t_u64, p1_addr, addr1, byte_off }; rdna4_icbc_emit_op(&code, 128, a, 4); }
    { uint32_t a[] = { t_p_psb_st, p1_st, p1_addr }; rdna4_icbc_emit_op(&code, 120, a, 3); } /* OpConvertUToPtr */
    { uint32_t a[] = { t_p_psb_u32, p1_u32, p1_st, c0 }; rdna4_icbc_emit_op(&code, 65, a, 4); }
    { uint32_t a[] = { p1_u32, res_val, 2u, 4u }; rdna4_icbc_emit_op(&code, 62, a, 4); }

    rdna4_icbc_emit_op(&code, 253, NULL, 0);
    rdna4_icbc_emit_op(&code, 56, NULL, 0);

    /* Assemble SPIR-V 1.5 module */
    rdna4_icbc_push(&spv, 0x07230203u);
    rdna4_icbc_push(&spv, 0x00010500u);
    rdna4_icbc_push(&spv, 0u);
    rdna4_icbc_push(&spv, nid);
    rdna4_icbc_push(&spv, 0u);
    { uint32_t a[] = { 1u }; rdna4_icbc_emit_op(&spv, 17, a, 1); }    /* Shader */
    { uint32_t a[] = { 11u }; rdna4_icbc_emit_op(&spv, 17, a, 1); }   /* Int64 */
    { uint32_t a[] = { 5347u }; rdna4_icbc_emit_op(&spv, 17, a, 1); } /* PhysicalStorageBufferAddresses */
    { uint32_t a[] = { 5348u, 1u }; rdna4_icbc_emit_op(&spv, 14, a, 2); } /* PhysicalStorageBuffer64, GLSL450 */
    {
        uint32_t n_iface = opt->with_push_constants ? 3u : 2u;
        rdna4_icbc_op(&spv, 15, (uint16_t)(3u + 2u + n_iface));
        rdna4_icbc_push(&spv, 5u);
        rdna4_icbc_push(&spv, fn_main);
        rdna4_icbc_str(&spv, "main");
        rdna4_icbc_push(&spv, v_gid);
        rdna4_icbc_push(&spv, v_tbl);
        if (opt->with_push_constants) rdna4_icbc_push(&spv, v_pc);
    }
    for (size_t i = 0; i < dec.len; i++) rdna4_icbc_push(&spv, dec.w[i]);
    for (size_t i = 0; i < types.len; i++) rdna4_icbc_push(&spv, types.w[i]);
    for (size_t i = 0; i < vars.len; i++) rdna4_icbc_push(&spv, vars.w[i]);
    for (size_t i = 0; i < code.len; i++) rdna4_icbc_push(&spv, code.w[i]);

    free(dec.w); free(types.w); free(vars.w); free(code.w);
    assert(!spv.oom);
    *out_words = spv.w;
    *out_nwords = spv.len;
}

static void test_compute_indirect_spv_transform(void) {
    /* Case A: u64 RuntimeArray table + ThreadsDynamic push constants + OpPhi on entry block + no barrier */
    synth_shader_opts_t opt = {
        .use_vec2_array_table = 0,
        .with_push_constants = 1,
        .with_barrier = 0,
        .with_entry_phi = 1,
        .kernel_mode = 0,
    };
    uint32_t *in_w = NULL, *out_w = NULL;
    size_t in_n = 0, out_n = 0;
    build_synth_compute_spv(&opt, &in_w, &in_n);

    int has_bar = -1, allow_fb = -1;
    rdna4s_icb_compute_inspect(in_w, in_n, &has_bar, &allow_fb);
    assert(has_bar == 0 && allow_fb == 1);

    const uint32_t loc[3] = { 32u, 1u, 1u };
    char why[256] = "";
    assert(rdna4s_icb_compute_indirect(in_w, in_n, 0u, 1, loc, &out_w, &out_n, why, sizeof why) == 0);
    assert(out_w != NULL && out_n > in_n);

    /* Transformed module must parse cleanly, have NumWorkgroups builtin, and rewrite OpPhi */
    RDNA4sMod mod;
    assert(rdna4s_parse(out_w, out_n, &mod, why, sizeof why) == 0);
    assert(mod.has_ep && mod.ep_model == 5u);
    int found_num_wg = 0, found_ret_blocks = 0, found_phi = 0;
    for (uint32_t id = 1u; id < mod.bound; id++) {
        if (mod.id[id].op == 59u && mod.id[id].sc == 1u && mod.id[id].builtin == 24)
            found_num_wg = 1;
    }
    assert(found_num_wg);
    for (size_t p = 5u; p < out_n; p += out_w[p] >> 16) {
        uint32_t op = out_w[p] & 0xffffu;
        if (op == 253u) found_ret_blocks++;
        if (op == 245u) found_phi++;
    }
    assert(found_ret_blocks >= 2); /* guard early return + main return */
    assert(found_phi == 1);
    rdna4s_free(&mod);
    free(out_w);
    free(in_w);

    /* Case B: v2u32 fixed-size OpTypeArray(96) Uniform table + no push constants */
    opt.use_vec2_array_table = 1;
    opt.with_push_constants = 0;
    opt.with_entry_phi = 0;
    build_synth_compute_spv(&opt, &in_w, &in_n);
    assert(rdna4s_icb_compute_indirect(in_w, in_n, 0u, 0, loc, &out_w, &out_n, why, sizeof why) == 0);
    assert(rdna4s_parse(out_w, out_n, &mod, why, sizeof why) == 0);
    found_ret_blocks = 0;
    for (size_t p = 5u; p < out_n; p += out_w[p] >> 16) {
        if ((out_w[p] & 0xffffu) == 253u) found_ret_blocks++;
    }
    assert(found_ret_blocks >= 2);
    rdna4s_free(&mod);
    free(out_w);
    free(in_w);

    /* Case C: shader with OpControlBarrier -> NO divergent guard, allow_fallback == 0 */
    opt.use_vec2_array_table = 0;
    opt.with_push_constants = 1;
    opt.with_barrier = 1;
    build_synth_compute_spv(&opt, &in_w, &in_n);
    rdna4s_icb_compute_inspect(in_w, in_n, &has_bar, &allow_fb);
    assert(has_bar == 1 && allow_fb == 0);
    assert(rdna4s_icb_compute_indirect(in_w, in_n, 0u, 1, loc, &out_w, &out_n, why, sizeof why) == 0);
    found_ret_blocks = 0;
    for (size_t p = 5u; p < out_n; p += out_w[p] >> 16) {
        if ((out_w[p] & 0xffffu) == 253u) found_ret_blocks++;
    }
    assert(found_ret_blocks == 1); /* barrier shader must NOT get early-return guard */
    free(out_w);

    /* Negative: zero local size rejected */
    const uint32_t bad_loc[3] = { 0u, 1u, 1u };
    assert(rdna4s_icb_compute_indirect(in_w, in_n, 0u, 1, bad_loc, &out_w, &out_n, why, sizeof why) != 0);
    assert(out_w == NULL && why[0] != '\0');
    free(in_w);
}
#endif /* RDNA4_TEST_HAS_VULKAN_H */

/* -------------------------------------------------------------------------
 * 3. Source & assembled GarnetDevice.m contract pins
 * ------------------------------------------------------------------------- */
static char *read_text_file(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return NULL; }
    long len = ftell(f);
    if (len < 0) { fclose(f); return NULL; }
    rewind(f);
    char *buf = (char *)malloc((size_t)len + 1u);
    if (!buf) { fclose(f); return NULL; }
    size_t got = fread(buf, 1u, (size_t)len, f);
    fclose(f);
    buf[got] = '\0';
    return buf;
}

static void assert_file_contains(const char *path, const char *const *needles, size_t n) {
    char *txt = read_text_file(path);
    if (!txt) {
        fprintf(stderr, "COMPUTE_ICB_FAIL cannot open %s\n", path);
        exit(1);
    }
    for (size_t i = 0; i < n; i++) {
        if (!strstr(txt, needles[i])) {
            fprintf(stderr, "COMPUTE_ICB_FAIL %s missing pin: %s\n", path, needles[i]);
            free(txt);
            exit(1);
        }
    }
    free(txt);
}

static void test_source_pins(void) {
    static const char *const pipe_pins[] = {
        "rdna4PrepareICBGPUVariant:",
        "lower_icb_stage",
        "rdna4s_icb_compute_indirect",
        "rdna4s_icb_compute_inspect",
        "rdna4ICBToken",
        "supportIndirectCommandBuffers",
        "gpuResourceID",
    };
    static const char *const enc_pins[] = {
        "executeCommandsInBuffer:",
        "indirectBufferOffset:",
        "rdna4ICBComputeBarrier",
        "VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT",
        "setStageInRegion:",
    };
    static const char *const arg_pins[] = {
        "MTLDataTypeComputePipeline",
        "setComputePipelineState:",
        "setComputePipelineStates:",
    };
    static const char *const icb_pins[] = {
        "RDNA4IndirectComputeCommand",
        "MTLIndirectComputeCommand",
        "indirectComputeCommandAtIndex:",
        "concurrentDispatchThreadgroups:",
        "concurrentDispatchThreads:",
        "setThreadgroupMemoryLength:",
        "setBarrier",
        "clearBarrier",
        "rdna4_icb_emit_compute_expand_spv",
        "rdna4ExecuteComputeOnEncoder:",
        "rdna4ExecuteComputeICBGPU:",
        "vkCmdDispatchIndirect",
        "cannot mix render and compute commands",
    };
    static const char *const hdr_pins[] = {
        "RDNA4_COMPUTE_ICB_ENABLED",
        "RDNA4ComputeICBInternal",
    };
    assert_file_contains("src/garnet-metal/garnet_metal_internal.h", hdr_pins, sizeof hdr_pins / sizeof hdr_pins[0]);
    assert_file_contains("src/garnet-metal/garnet_pipelines.m", pipe_pins, sizeof pipe_pins / sizeof pipe_pins[0]);
    assert_file_contains("src/garnet-metal/garnet_commands.m", enc_pins, sizeof enc_pins / sizeof enc_pins[0]);
    assert_file_contains("src/garnet-metal/garnet_arguments.m", arg_pins, sizeof arg_pins / sizeof arg_pins[0]);
    assert_file_contains("src/garnet-metal/garnet_icb.m", icb_pins, sizeof icb_pins / sizeof icb_pins[0]);

    const char *assembled = getenv("RDNA4_TEST_ASSEMBLED_DEVICE");
    if (assembled && assembled[0]) {
        static const char *const asm_inc_pins[] = {
            "#include \"garnet_icb_abi.h\"",
            "#include \"garnet_arguments.m\"",
            "#include \"garnet_icb.m\"",
        };
        assert_file_contains(assembled, pipe_pins, sizeof pipe_pins / sizeof pipe_pins[0]);
        assert_file_contains(assembled, enc_pins, sizeof enc_pins / sizeof enc_pins[0]);
        assert_file_contains(assembled, asm_inc_pins, sizeof asm_inc_pins / sizeof asm_inc_pins[0]);
    }
}

/* -------------------------------------------------------------------------
 * 4. Optional live Vulkan 1.4 LavaPipe GPU compute ICB execution test
 * ------------------------------------------------------------------------- */
#ifdef RDNA4_TEST_LAVAPIPE
typedef struct {
    VkBuffer buf;
    VkDeviceMemory mem;
    void *map;
    uint64_t bda;
    VkDeviceSize size;
} lvp_buf_t;

#define LVP_VK(expr) do { VkResult r_ = (expr); if (r_ != VK_SUCCESS) { \
    fprintf(stderr, "LAVAPIPE_FAIL %s = %d at line %d\n", #expr, (int)r_, __LINE__); exit(1); } } while (0)

static uint32_t lvp_find_mem(const VkPhysicalDeviceMemoryProperties *mp, uint32_t bits, VkMemoryPropertyFlags want) {
    for (uint32_t i = 0; i < mp->memoryTypeCount; i++) {
        if ((bits & (1u << i)) && ((mp->memoryTypes[i].propertyFlags & want) == want)) return i;
    }
    fprintf(stderr, "LAVAPIPE_FAIL no suitable memory type\n");
    exit(1);
}

static lvp_buf_t lvp_alloc_buf(VkDevice dev, const VkPhysicalDeviceMemoryProperties *mp,
                               PFN_vkCreateBuffer pCreateBuffer,
                               PFN_vkGetBufferMemoryRequirements pGetMemReq,
                               PFN_vkAllocateMemory pAllocMem,
                               PFN_vkBindBufferMemory pBindMem,
                               PFN_vkMapMemory pMapMem,
                               PFN_vkGetBufferDeviceAddress pGetBda,
                               VkDeviceSize sz, VkBufferUsageFlags usage) {
    lvp_buf_t b;
    memset(&b, 0, sizeof b);
    b.size = sz;
    VkBufferCreateInfo bci = {
        .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
        .size = sz,
        .usage = usage | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
    };
    LVP_VK(pCreateBuffer(dev, &bci, NULL, &b.buf));
    VkMemoryRequirements mr;
    pGetMemReq(dev, b.buf, &mr);
    VkMemoryAllocateFlagsInfo afi = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_FLAGS_INFO,
        .flags = VK_MEMORY_ALLOCATE_DEVICE_ADDRESS_BIT,
    };
    VkMemoryAllocateInfo mai = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .pNext = &afi,
        .allocationSize = mr.size,
        .memoryTypeIndex = lvp_find_mem(mp, mr.memoryTypeBits,
                                        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT),
    };
    LVP_VK(pAllocMem(dev, &mai, NULL, &b.mem));
    LVP_VK(pBindMem(dev, b.buf, b.mem, 0));
    LVP_VK(pMapMem(dev, b.mem, 0, sz, 0, &b.map));
    memset(b.map, 0, (size_t)sz);
    VkBufferDeviceAddressInfo bdi = {
        .sType = VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO,
        .buffer = b.buf,
    };
    b.bda = pGetBda(dev, &bdi);
    assert(b.bda != 0ull);
    return b;
}

static void test_lavapipe_gpu_compute_icb(void) {
    void *lib = dlopen("libvulkan.so.1", RTLD_NOW);
    if (!lib) { fprintf(stderr, "LAVAPIPE_FAIL dlopen libvulkan.so.1: %s\n", dlerror()); exit(1); }
    PFN_vkGetInstanceProcAddr gipa = NULL;
    { void *sym = dlsym(lib, "vkGetInstanceProcAddr"); memcpy(&gipa, &sym, sizeof gipa); }
    assert(gipa != NULL);

    #define LOAD_GLOBAL(name) PFN_##name p_##name = (PFN_##name)gipa(VK_NULL_HANDLE, #name); assert(p_##name != NULL)
    LOAD_GLOBAL(vkCreateInstance);
    VkApplicationInfo ai = { .sType = VK_STRUCTURE_TYPE_APPLICATION_INFO, .apiVersion = VK_API_VERSION_1_3 };
    VkInstanceCreateInfo ici = { .sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, .pApplicationInfo = &ai };
    VkInstance inst = VK_NULL_HANDLE;
    LVP_VK(p_vkCreateInstance(&ici, NULL, &inst));

    #define LOAD_INST(name) PFN_##name p_##name = (PFN_##name)gipa(inst, #name); assert(p_##name != NULL)
    LOAD_INST(vkDestroyInstance);
    LOAD_INST(vkEnumeratePhysicalDevices);
    LOAD_INST(vkGetPhysicalDeviceProperties);
    LOAD_INST(vkGetPhysicalDeviceMemoryProperties);
    LOAD_INST(vkGetPhysicalDeviceQueueFamilyProperties);
    LOAD_INST(vkCreateDevice);
    LOAD_INST(vkGetDeviceProcAddr);

    uint32_t ndev = 0;
    LVP_VK(p_vkEnumeratePhysicalDevices(inst, &ndev, NULL));
    assert(ndev >= 1u);
    VkPhysicalDevice pdevs[8];
    if (ndev > 8u) ndev = 8u;
    LVP_VK(p_vkEnumeratePhysicalDevices(inst, &ndev, pdevs));
    VkPhysicalDevice pd = pdevs[0];
    for (uint32_t i = 0; i < ndev; i++) {
        VkPhysicalDeviceProperties props;
        p_vkGetPhysicalDeviceProperties(pdevs[i], &props);
        if (props.deviceType == VK_PHYSICAL_DEVICE_TYPE_CPU) { pd = pdevs[i]; break; }
    }
    VkPhysicalDeviceMemoryProperties mp;
    p_vkGetPhysicalDeviceMemoryProperties(pd, &mp);

    uint32_t nqf = 0;
    p_vkGetPhysicalDeviceQueueFamilyProperties(pd, &nqf, NULL);
    VkQueueFamilyProperties qfp[16];
    if (nqf > 16u) nqf = 16u;
    p_vkGetPhysicalDeviceQueueFamilyProperties(pd, &nqf, qfp);
    uint32_t qfi = UINT32_MAX;
    for (uint32_t i = 0; i < nqf; i++) {
        if (qfp[i].queueFlags & VK_QUEUE_COMPUTE_BIT) { qfi = i; break; }
    }
    assert(qfi != UINT32_MAX);

    float prio = 1.0f;
    VkDeviceQueueCreateInfo qci = {
        .sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
        .queueFamilyIndex = qfi,
        .queueCount = 1u,
        .pQueuePriorities = &prio,
    };
    VkPhysicalDeviceVulkan12Features f12 = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES,
        .bufferDeviceAddress = VK_TRUE,
    };
    VkPhysicalDeviceFeatures2 f2 = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2,
        .pNext = &f12,
        .features = { .shaderInt64 = VK_TRUE },
    };
    VkDeviceCreateInfo dci = {
        .sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
        .pNext = &f2,
        .queueCreateInfoCount = 1u,
        .pQueueCreateInfos = &qci,
    };
    VkDevice dev = VK_NULL_HANDLE;
    LVP_VK(p_vkCreateDevice(pd, &dci, NULL, &dev));

    #define LOAD_DEV(name) PFN_##name p_##name = (PFN_##name)p_vkGetDeviceProcAddr(dev, #name); assert(p_##name != NULL)
    LOAD_DEV(vkDestroyDevice); LOAD_DEV(vkGetDeviceQueue); LOAD_DEV(vkDeviceWaitIdle);
    LOAD_DEV(vkCreateBuffer); LOAD_DEV(vkDestroyBuffer); LOAD_DEV(vkGetBufferMemoryRequirements);
    LOAD_DEV(vkAllocateMemory); LOAD_DEV(vkFreeMemory); LOAD_DEV(vkBindBufferMemory);
    LOAD_DEV(vkMapMemory); LOAD_DEV(vkGetBufferDeviceAddress);
    LOAD_DEV(vkCreateShaderModule); LOAD_DEV(vkDestroyShaderModule);
    LOAD_DEV(vkCreateDescriptorSetLayout); LOAD_DEV(vkDestroyDescriptorSetLayout);
    LOAD_DEV(vkCreatePipelineLayout); LOAD_DEV(vkDestroyPipelineLayout);
    LOAD_DEV(vkCreateComputePipelines); LOAD_DEV(vkDestroyPipeline);
    LOAD_DEV(vkCreateDescriptorPool); LOAD_DEV(vkDestroyDescriptorPool);
    LOAD_DEV(vkAllocateDescriptorSets); LOAD_DEV(vkUpdateDescriptorSets);
    LOAD_DEV(vkCreateCommandPool); LOAD_DEV(vkDestroyCommandPool);
    LOAD_DEV(vkAllocateCommandBuffers); LOAD_DEV(vkBeginCommandBuffer); LOAD_DEV(vkEndCommandBuffer);
    LOAD_DEV(vkCmdBindPipeline); LOAD_DEV(vkCmdBindDescriptorSets);
    LOAD_DEV(vkCmdDispatch); LOAD_DEV(vkCmdDispatchIndirect); LOAD_DEV(vkCmdPipelineBarrier);
    LOAD_DEV(vkQueueSubmit);

    VkQueue queue = VK_NULL_HANDLE;
    p_vkGetDeviceQueue(dev, qfi, 0u, &queue);

    /* 1. Build compute ICB expander pipeline directly from rdna4_icb_emit_compute_expand_spv() */
    uint32_t *exp_w = NULL; size_t exp_n = 0;
    assert(rdna4_icb_emit_compute_expand_spv(&exp_w, &exp_n) == 1);
    VkShaderModule exp_sm = VK_NULL_HANDLE;
    VkShaderModuleCreateInfo smci = {
        .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
        .codeSize = exp_n * 4u, .pCode = exp_w,
    };
    LVP_VK(p_vkCreateShaderModule(dev, &smci, NULL, &exp_sm));
    free(exp_w);

    VkDescriptorSetLayoutBinding exp_b[5];
    for (uint32_t i = 0; i < 5u; i++)
        exp_b[i] = (VkDescriptorSetLayoutBinding){ i, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1u, VK_SHADER_STAGE_COMPUTE_BIT, NULL };
    VkDescriptorSetLayout exp_dsl = VK_NULL_HANDLE;
    VkDescriptorSetLayoutCreateInfo dslci = {
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
        .bindingCount = 5u, .pBindings = exp_b,
    };
    LVP_VK(p_vkCreateDescriptorSetLayout(dev, &dslci, NULL, &exp_dsl));
    VkPipelineLayout exp_pl = VK_NULL_HANDLE;
    VkPipelineLayoutCreateInfo plci = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
        .setLayoutCount = 1u, .pSetLayouts = &exp_dsl,
    };
    LVP_VK(p_vkCreatePipelineLayout(dev, &plci, NULL, &exp_pl));
    VkPipeline exp_pipe = VK_NULL_HANDLE;
    VkComputePipelineCreateInfo cpci = {
        .sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
        .stage = { .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
                   .stage = VK_SHADER_STAGE_COMPUTE_BIT, .module = exp_sm, .pName = "main" },
        .layout = exp_pl,
    };
    LVP_VK(p_vkCreateComputePipelines(dev, VK_NULL_HANDLE, 1u, &cpci, NULL, &exp_pipe));
    p_vkDestroyShaderModule(dev, exp_sm, NULL);

    /* 2. Build Kernel A (ThreadsDynamic + push constants + BDA + entry phi) & Kernel B (second pipeline) */
    VkDescriptorSetLayoutBinding k_b = {
        RDNA4_ICB_TABLE_BINDING, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC, 1u, VK_SHADER_STAGE_COMPUTE_BIT, NULL
    };
    VkDescriptorSetLayout k_dsl = VK_NULL_HANDLE;
    VkDescriptorSetLayoutCreateInfo k_dslci = {
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
        .bindingCount = 1u, .pBindings = &k_b,
    };
    LVP_VK(p_vkCreateDescriptorSetLayout(dev, &k_dslci, NULL, &k_dsl));
    VkPushConstantRange k_pcr = { VK_SHADER_STAGE_COMPUTE_BIT, 0u, 48u };
    VkPipelineLayout k_pl = VK_NULL_HANDLE;
    VkPipelineLayoutCreateInfo k_plci = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
        .setLayoutCount = 1u, .pSetLayouts = &k_dsl,
        .pushConstantRangeCount = 1u, .pPushConstantRanges = &k_pcr,
    };
    LVP_VK(p_vkCreatePipelineLayout(dev, &k_plci, NULL, &k_pl));

    const uint32_t loc32[3] = { 32u, 1u, 1u };
    VkSpecializationMapEntry sme[3] = { { 0, 0, 4 }, { 1, 4, 4 }, { 2, 8, 4 } };
    VkSpecializationInfo si32 = { 3u, sme, sizeof loc32, loc32 };
    VkPipeline k_pipe[2] = { VK_NULL_HANDLE, VK_NULL_HANDLE };
    for (int ki = 0; ki < 2; ki++) {
        synth_shader_opts_t so = {
            .use_vec2_array_table = (ki == 1),
            .with_push_constants = (ki == 0),
            .with_barrier = 0,
            .with_entry_phi = (ki == 0),
            .kernel_mode = ki,
        };
        uint32_t *raw_w = NULL, *ind_w = NULL;
        size_t raw_n = 0, ind_n = 0;
        char why[256] = "";
        build_synth_compute_spv(&so, &raw_w, &raw_n);
        assert(rdna4s_icb_compute_indirect(raw_w, raw_n, 0u, so.with_push_constants, loc32,
                                           &ind_w, &ind_n, why, sizeof why) == 0);
        free(raw_w);
        VkShaderModule ksm = VK_NULL_HANDLE;
        VkShaderModuleCreateInfo ksmci = {
            .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
            .codeSize = ind_n * 4u, .pCode = ind_w,
        };
        LVP_VK(p_vkCreateShaderModule(dev, &ksmci, NULL, &ksm));
        free(ind_w);
        VkComputePipelineCreateInfo kci = {
            .sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
            .stage = { .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
                       .stage = VK_SHADER_STAGE_COMPUTE_BIT, .module = ksm, .pName = "main",
                       .pSpecializationInfo = &si32 },
            .layout = k_pl,
        };
        LVP_VK(p_vkCreateComputePipelines(dev, VK_NULL_HANDLE, 1u, &kci, NULL, &k_pipe[ki]));
        p_vkDestroyShaderModule(dev, ksm, NULL);
    }

    /* 3. Allocate GPU buffers: dataIn, dataMid, dataFallback, icbBacking, cfgBuf, rangeBuf, outDispatches, outKTable */
    const uint32_t N_SLOTS = 6u;
    const uint32_t N_BUCKETS = 2u;
    const VkBufferUsageFlags ssbo = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
    const VkBufferUsageFlags ind_ssbo = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT;

    lvp_buf_t b_in = lvp_alloc_buf(dev, &mp, p_vkCreateBuffer, p_vkGetBufferMemoryRequirements, p_vkAllocateMemory, p_vkBindBufferMemory, p_vkMapMemory, p_vkGetBufferDeviceAddress, 128u * 4u, ssbo);
    lvp_buf_t b_mid = lvp_alloc_buf(dev, &mp, p_vkCreateBuffer, p_vkGetBufferMemoryRequirements, p_vkAllocateMemory, p_vkBindBufferMemory, p_vkMapMemory, p_vkGetBufferDeviceAddress, 128u * 4u, ssbo);
    lvp_buf_t b_fb = lvp_alloc_buf(dev, &mp, p_vkCreateBuffer, p_vkGetBufferMemoryRequirements, p_vkAllocateMemory, p_vkBindBufferMemory, p_vkMapMemory, p_vkGetBufferDeviceAddress, 128u * 4u, ssbo);
    lvp_buf_t b_icb = lvp_alloc_buf(dev, &mp, p_vkCreateBuffer, p_vkGetBufferMemoryRequirements, p_vkAllocateMemory, p_vkBindBufferMemory, p_vkMapMemory, p_vkGetBufferDeviceAddress, rdna4_icb_backing_bytes(N_SLOTS), ssbo);
    lvp_buf_t b_cfg = lvp_alloc_buf(dev, &mp, p_vkCreateBuffer, p_vkGetBufferMemoryRequirements, p_vkAllocateMemory, p_vkBindBufferMemory, p_vkMapMemory, p_vkGetBufferDeviceAddress, sizeof(rdna4_icb_compute_expand_cfg_t), ssbo);
    lvp_buf_t b_rng = lvp_alloc_buf(dev, &mp, p_vkCreateBuffer, p_vkGetBufferMemoryRequirements, p_vkAllocateMemory, p_vkBindBufferMemory, p_vkMapMemory, p_vkGetBufferDeviceAddress, 16u, ssbo);
    lvp_buf_t b_disp = lvp_alloc_buf(dev, &mp, p_vkCreateBuffer, p_vkGetBufferMemoryRequirements, p_vkAllocateMemory, p_vkBindBufferMemory, p_vkMapMemory, p_vkGetBufferDeviceAddress, (VkDeviceSize)N_SLOTS * N_BUCKETS * sizeof(VkDispatchIndirectCommand), ind_ssbo);
    lvp_buf_t b_ktbl = lvp_alloc_buf(dev, &mp, p_vkCreateBuffer, p_vkGetBufferMemoryRequirements, p_vkAllocateMemory, p_vkBindBufferMemory, p_vkMapMemory, p_vkGetBufferDeviceAddress, (VkDeviceSize)N_SLOTS * RDNA4_ICB_TABLE_V2_STRIDE, ssbo);

    uint32_t *in_p = (uint32_t *)b_in.map;
    uint32_t *mid_p = (uint32_t *)b_mid.map;
    uint32_t *fb_p = (uint32_t *)b_fb.map;
    for (uint32_t i = 0; i < 128u; i++) {
        in_p[i] = i + 10u;
        mid_p[i] = 0xdeadbeefu;
        fb_p[i] = 0xcafebabeu;
    }

    /* Populate 640B compute ICB header + 6 slots (matching 0072 GPU layout) */
    const uint64_t tokA = RDNA4_ICB_PSO_TOKEN_TAG | RDNA4_ICB_COMPUTE_PSO_BIT | 1ull;
    const uint64_t tokB = RDNA4_ICB_PSO_TOKEN_TAG | RDNA4_ICB_COMPUTE_PSO_BIT | 2ull;
    rdna4_icb_header_t *hdr = (rdna4_icb_header_t *)b_icb.map;
    hdr->magic = RDNA4_ICB_MAGIC;
    hdr->max_command_count = N_SLOTS;
    hdr->command_types = RDNA4_ICB_COMPUTE_CMD_MASK;
    hdr->flags = 0u;
    hdr->max_vertex_binds = 4u;
    hdr->slot_stride = RDNA4_ICB_SLOT_BYTES;

    rdna4_icb_compute_slot_t *slots = (rdna4_icb_compute_slot_t *)((uint8_t *)b_icb.map + RDNA4_ICB_HEADER_BYTES);
    /* Slot 0: Kernel A, ConcurrentDispatch, grid=(2,1,1), tg=(32,1,1) -> 64 threads write b_mid[0..63] */
    {
        const uint32_t g[3] = { 2u, 1u, 1u }, tg[3] = { 32u, 1u, 1u };
        assert(rdna4_icb_compute_slot_set_pipeline(&slots[0], hdr->flags, tokA));
        assert(rdna4_icb_compute_slot_set_kernel_buffer(&slots[0], hdr->flags, hdr->max_vertex_binds, 0u, b_in.bda));
        assert(rdna4_icb_compute_slot_set_kernel_buffer(&slots[0], hdr->flags, hdr->max_vertex_binds, 1u, b_mid.bda));
        assert(rdna4_icb_compute_slot_set_dispatch(&slots[0], hdr->command_types, hdr->flags, RDNA4_ICB_KIND_CONCURRENT_DISPATCH, g, tg));
    }
    /* Slot 1: Kernel B, ConcurrentDispatchThreads, grid=(45,1,1), tg=(32,1,1) + barrier!
     * Reads b_mid[0..44] written by Slot 0 and writes back b_mid[i] += 1000 + i; lanes 45..63 MUST be guarded! */
    {
        const uint32_t g[3] = { 45u, 1u, 1u }, tg[3] = { 32u, 1u, 1u };
        assert(rdna4_icb_compute_slot_set_pipeline(&slots[1], hdr->flags, tokB));
        assert(rdna4_icb_compute_slot_set_kernel_buffer(&slots[1], hdr->flags, hdr->max_vertex_binds, 0u, b_mid.bda));
        assert(rdna4_icb_compute_slot_set_kernel_buffer(&slots[1], hdr->flags, hdr->max_vertex_binds, 1u, b_mid.bda));
        rdna4_icb_compute_slot_set_barrier(&slots[1], 1);
        assert(rdna4_icb_compute_slot_set_dispatch(&slots[1], hdr->command_types, hdr->flags, RDNA4_ICB_KIND_CONCURRENT_DISPATCH_THREADS, g, tg));
    }
    /* Slot 2: Empty (no-op) */
    /* Slot 3: Invalid slot (must be skipped by expander) */
    {
        const uint32_t g[3] = { 1u, 1u, 1u }, tg[3] = { 32u, 1u, 1u };
        assert(rdna4_icb_compute_slot_set_pipeline(&slots[3], hdr->flags, tokB));
        assert(rdna4_icb_compute_slot_set_kernel_buffer(&slots[3], hdr->flags, hdr->max_vertex_binds, 0u, b_in.bda));
        assert(rdna4_icb_compute_slot_set_kernel_buffer(&slots[3], hdr->flags, hdr->max_vertex_binds, 1u, b_fb.bda));
        assert(rdna4_icb_compute_slot_set_dispatch(&slots[3], hdr->command_types, hdr->flags, RDNA4_ICB_KIND_CONCURRENT_DISPATCH, g, tg));
        slots[3].flags |= RDNA4_ICB_SLOT_FLAG_INVALID;
    }
    /* Slot 4: Kernel A, ConcurrentDispatchThreads with unlisted tg=(13,1,1) -> hits ALLOW_FALLBACK bucket (32,1,1),
     * writes ONLY b_fb[0..16], leaves b_fb[17..127] untouched! */
    {
        const uint32_t g[3] = { 17u, 1u, 1u }, tg[3] = { 13u, 1u, 1u };
        assert(rdna4_icb_compute_slot_set_pipeline(&slots[4], hdr->flags, tokA));
        assert(rdna4_icb_compute_slot_set_kernel_buffer(&slots[4], hdr->flags, hdr->max_vertex_binds, 0u, b_in.bda));
        assert(rdna4_icb_compute_slot_set_kernel_buffer(&slots[4], hdr->flags, hdr->max_vertex_binds, 1u, b_fb.bda));
        assert(rdna4_icb_compute_slot_set_dispatch(&slots[4], hdr->command_types, hdr->flags, RDNA4_ICB_KIND_CONCURRENT_DISPATCH_THREADS, g, tg));
    }
    /* Slot 5: Valid slot OUTSIDE indirect range {loc=0, len=5} -> must NOT execute! */
    {
        const uint32_t g[3] = { 1u, 1u, 1u }, tg[3] = { 32u, 1u, 1u };
        assert(rdna4_icb_compute_slot_set_pipeline(&slots[5], hdr->flags, tokB));
        assert(rdna4_icb_compute_slot_set_kernel_buffer(&slots[5], hdr->flags, hdr->max_vertex_binds, 0u, b_fb.bda));
        assert(rdna4_icb_compute_slot_set_kernel_buffer(&slots[5], hdr->flags, hdr->max_vertex_binds, 1u, b_fb.bda));
        assert(rdna4_icb_compute_slot_set_dispatch(&slots[5], hdr->command_types, hdr->flags, RDNA4_ICB_KIND_CONCURRENT_DISPATCH, g, tg));
    }

    /* Indirect range = { location = 0, length = 5 } */
    ((uint32_t *)b_rng.map)[0] = 0u;
    ((uint32_t *)b_rng.map)[1] = 5u;

    rdna4_icb_compute_expand_cfg_t *cfg = (rdna4_icb_compute_expand_cfg_t *)b_cfg.map;
    cfg->use_indirect_range = 1u;
    cfg->max_output_slots = N_SLOTS;
    cfg->inherit_pipeline = 0u;
    cfg->inherit_buffers = 0u;
    cfg->num_buckets = N_BUCKETS;
    cfg->buckets[0] = (rdna4_icb_compute_bucket_t){ tokA, 32u, 1u, 1u, RDNA4_ICB_COMPUTE_BUCKET_ALLOW_FALLBACK, 0u, 0u };
    cfg->buckets[1] = (rdna4_icb_compute_bucket_t){ tokB, 32u, 1u, 1u, RDNA4_ICB_COMPUTE_BUCKET_ALLOW_FALLBACK, 0u, 0u };

    /* Descriptor pool & sets */
    VkDescriptorPoolSize dps[2] = {
        { VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 8u },
        { VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC, 4u },
    };
    VkDescriptorPool dp = VK_NULL_HANDLE;
    VkDescriptorPoolCreateInfo dpci = {
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
        .maxSets = 4u, .poolSizeCount = 2u, .pPoolSizes = dps,
    };
    LVP_VK(p_vkCreateDescriptorPool(dev, &dpci, NULL, &dp));
    VkDescriptorSet exp_set = VK_NULL_HANDLE, k_set = VK_NULL_HANDLE;
    VkDescriptorSetAllocateInfo dsai1 = { .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO, .descriptorPool = dp, .descriptorSetCount = 1u, .pSetLayouts = &exp_dsl };
    VkDescriptorSetAllocateInfo dsai2 = { .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO, .descriptorPool = dp, .descriptorSetCount = 1u, .pSetLayouts = &k_dsl };
    LVP_VK(p_vkAllocateDescriptorSets(dev, &dsai1, &exp_set));
    LVP_VK(p_vkAllocateDescriptorSets(dev, &dsai2, &k_set));

    VkDescriptorBufferInfo exp_bi[5] = {
        { b_icb.buf, 0, b_icb.size }, { b_cfg.buf, 0, b_cfg.size }, { b_rng.buf, 0, 8u },
        { b_disp.buf, 0, b_disp.size }, { b_ktbl.buf, 0, b_ktbl.size },
    };
    VkWriteDescriptorSet exp_wds[5];
    for (uint32_t i = 0; i < 5u; i++) {
        exp_wds[i] = (VkWriteDescriptorSet){
            .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
            .dstSet = exp_set, .dstBinding = i, .descriptorCount = 1u,
            .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, .pBufferInfo = &exp_bi[i],
        };
    }
    VkDescriptorBufferInfo k_bi = { b_ktbl.buf, 0, sizeof(rdna4_buffer_table) };
    VkWriteDescriptorSet k_wds = {
        .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
        .dstSet = k_set, .dstBinding = RDNA4_ICB_TABLE_BINDING, .descriptorCount = 1u,
        .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC, .pBufferInfo = &k_bi,
    };
    p_vkUpdateDescriptorSets(dev, 5u, exp_wds, 0u, NULL);
    p_vkUpdateDescriptorSets(dev, 1u, &k_wds, 0u, NULL);

    /* Record & submit expander + per-slot compute barrier + vkCmdDispatchIndirect loop */
    VkCommandPool cp = VK_NULL_HANDLE;
    VkCommandPoolCreateInfo cp_ci = { .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO, .queueFamilyIndex = qfi };
    LVP_VK(p_vkCreateCommandPool(dev, &cp_ci, NULL, &cp));
    VkCommandBuffer cmd = VK_NULL_HANDLE;
    VkCommandBufferAllocateInfo cbai = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, .commandPool = cp, .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY, .commandBufferCount = 1u };
    LVP_VK(p_vkAllocateCommandBuffers(dev, &cbai, &cmd));
    VkCommandBufferBeginInfo cbbi = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
    LVP_VK(p_vkBeginCommandBuffer(cmd, &cbbi));

    p_vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, exp_pipe);
    p_vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, exp_pl, 0u, 1u, &exp_set, 0u, NULL);
    p_vkCmdDispatch(cmd, 1u, 1u, 1u);

    VkMemoryBarrier mb_exp = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER,
        .srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT,
        .dstAccessMask = VK_ACCESS_INDIRECT_COMMAND_READ_BIT | VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT,
    };
    p_vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                           VK_PIPELINE_STAGE_DRAW_INDIRECT_BIT | VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                           0u, 1u, &mb_exp, 0u, NULL, 0u, NULL);

    VkMemoryBarrier mb_slot = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER,
        .srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT,
        .dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT,
    };
    for (uint32_t k = 0; k < N_SLOTS; k++) {
        if (k > 0u) {
            p_vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                                   0u, 1u, &mb_slot, 0u, NULL, 0u, NULL);
        }
        uint32_t dyn = k * RDNA4_ICB_TABLE_V2_STRIDE;
        for (uint32_t b = 0; b < N_BUCKETS; b++) {
            p_vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, k_pipe[b]);
            p_vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, k_pl, 0u, 1u, &k_set, 1u, &dyn);
            VkDeviceSize off = ((VkDeviceSize)k * N_BUCKETS + b) * sizeof(VkDispatchIndirectCommand);
            p_vkCmdDispatchIndirect(cmd, b_disp.buf, off);
        }
    }
    LVP_VK(p_vkEndCommandBuffer(cmd));
    VkSubmitInfo sub = { .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .commandBufferCount = 1u, .pCommandBuffers = &cmd };
    LVP_VK(p_vkQueueSubmit(queue, 1u, &sub, VK_NULL_HANDLE));
    LVP_VK(p_vkDeviceWaitIdle(dev));

    /* Verify expanded dispatch table */
    const VkDispatchIndirectCommand *dc = (const VkDispatchIndirectCommand *)b_disp.map;
    assert(dc[0 * 2 + 0].x == 2u && dc[0 * 2 + 0].y == 1u && dc[0 * 2 + 0].z == 1u);
    assert(dc[0 * 2 + 1].x == 0u);
    assert(dc[1 * 2 + 0].x == 0u);
    assert(dc[1 * 2 + 1].x == 2u && dc[1 * 2 + 1].y == 1u && dc[1 * 2 + 1].z == 1u); /* ceil(45/32)=2 */
    assert(dc[2 * 2 + 0].x == 0u && dc[2 * 2 + 1].x == 0u);                         /* empty */
    assert(dc[3 * 2 + 0].x == 0u && dc[3 * 2 + 1].x == 0u);                         /* invalid */
    assert(dc[4 * 2 + 0].x == 1u && dc[4 * 2 + 0].y == 1u && dc[4 * 2 + 0].z == 1u);/* fallback ceil(17/32)=1 */
    assert(dc[4 * 2 + 1].x == 0u);
    assert(dc[5 * 2 + 0].x == 0u && dc[5 * 2 + 1].x == 0u);                         /* outside indirect range */

    /* Verify Slot 0 + barrier + Slot 1 outputs (including ThreadsDynamic active_threads + NumWorkgroups + entry guard!) */
    for (uint32_t i = 0; i < 128u; i++) {
        uint32_t want;
        if (i < 64u) {
            /* Slot 0: in[i]*2 + active_threads.x(64) + tbase.x(0) + NumWorkgroups.x(2) + phi(0) */
            want = in_p[i] * 2u + 64u + 2u;
            /* Slot 1 (after barrier): lanes 0..44 add 1000 + i; lanes 45..63 guarded! */
            if (i < 45u) want += 1000u + i;
        } else {
            want = 0xdeadbeefu;
        }
        if (mid_p[i] != want) {
            fprintf(stderr, "LAVAPIPE_FAIL mid[%u]=%u want=%u\n", i, mid_p[i], want);
            exit(1);
        }
    }

    /* Verify Slot 4 fallback + guard (only 0..16 written, 17..127 untouched; Slot 3 invalid & Slot 5 OOB skipped) */
    for (uint32_t i = 0; i < 128u; i++) {
        uint32_t want = (i < 17u) ? (in_p[i] * 2u + 17u + 1u) : 0xcafebabeu;
        if (fb_p[i] != want) {
            fprintf(stderr, "LAVAPIPE_FAIL fb[%u]=%u want=%u\n", i, fb_p[i], want);
            exit(1);
        }
    }

    /* Subtest 2: inherit_pipeline=1 + inherit_buffers=1 + direct range { location=1, length=1 } */
    for (uint32_t i = 0; i < 128u; i++) fb_p[i] = 0x55555555u;
    hdr->flags = RDNA4_ICB_FLAG_INHERIT_PIPELINE | RDNA4_ICB_FLAG_INHERIT_BUFFERS;
    rdna4_icb_compute_slot_reset(&slots[1]);
    {
        const uint32_t g[3] = { 23u, 1u, 1u }, tg[3] = { 32u, 1u, 1u };
        assert(rdna4_icb_compute_slot_set_dispatch(&slots[1], hdr->command_types, hdr->flags,
                                                   RDNA4_ICB_KIND_CONCURRENT_DISPATCH_THREADS, g, tg));
    }
    memset(cfg, 0, sizeof *cfg);
    cfg->direct_location = 1u;
    cfg->direct_length = 1u;
    cfg->use_indirect_range = 0u;
    cfg->max_output_slots = 1u;
    cfg->inherit_pipeline = 1u;
    cfg->inherit_buffers = 1u;
    cfg->num_buckets = 1u;
    cfg->parent_pipeline_token = tokA;
    cfg->buckets[0] = (rdna4_icb_compute_bucket_t){ tokA, 32u, 1u, 1u, RDNA4_ICB_COMPUTE_BUCKET_ALLOW_FALLBACK, 0u, 0u };
    assert(rdna4_buffer_table_set(&cfg->parent_k_table, 0u, b_in.bda, 128u * 4u, 0u, 0));
    assert(rdna4_buffer_table_set(&cfg->parent_k_table, 1u, b_fb.bda, 128u * 4u, 0u, 0));

    LVP_VK(p_vkBeginCommandBuffer(cmd, &cbbi));
    p_vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, exp_pipe);
    p_vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, exp_pl, 0u, 1u, &exp_set, 0u, NULL);
    p_vkCmdDispatch(cmd, 1u, 1u, 1u);
    p_vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                           VK_PIPELINE_STAGE_DRAW_INDIRECT_BIT | VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                           0u, 1u, &mb_exp, 0u, NULL, 0u, NULL);
    {
        uint32_t dyn0 = 0u;
        p_vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, k_pipe[0]);
        p_vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, k_pl, 0u, 1u, &k_set, 1u, &dyn0);
        p_vkCmdDispatchIndirect(cmd, b_disp.buf, 0u);
    }
    LVP_VK(p_vkEndCommandBuffer(cmd));
    LVP_VK(p_vkQueueSubmit(queue, 1u, &sub, VK_NULL_HANDLE));
    LVP_VK(p_vkDeviceWaitIdle(dev));
    for (uint32_t i = 0; i < 128u; i++) {
        uint32_t want = (i < 23u) ? (in_p[i] * 2u + 23u + 1u) : 0x55555555u;
        if (fb_p[i] != want) {
            fprintf(stderr, "LAVAPIPE_FAIL inherit fb[%u]=%u want=%u\n", i, fb_p[i], want);
            exit(1);
        }
    }

    /* Cleanup */
    p_vkDestroyCommandPool(dev, cp, NULL);
    p_vkDestroyDescriptorPool(dev, dp, NULL);
    p_vkDestroyPipeline(dev, exp_pipe, NULL);
    p_vkDestroyPipelineLayout(dev, exp_pl, NULL);
    p_vkDestroyDescriptorSetLayout(dev, exp_dsl, NULL);
    for (int ki = 0; ki < 2; ki++) p_vkDestroyPipeline(dev, k_pipe[ki], NULL);
    p_vkDestroyPipelineLayout(dev, k_pl, NULL);
    p_vkDestroyDescriptorSetLayout(dev, k_dsl, NULL);
    lvp_buf_t *all_b[] = { &b_in, &b_mid, &b_fb, &b_icb, &b_cfg, &b_rng, &b_disp, &b_ktbl };
    for (size_t i = 0; i < sizeof all_b / sizeof all_b[0]; i++) {
        p_vkDestroyBuffer(dev, all_b[i]->buf, NULL);
        p_vkFreeMemory(dev, all_b[i]->mem, NULL);
    }
    p_vkDestroyDevice(dev, NULL);
    p_vkDestroyInstance(inst, NULL);
    dlclose(lib);
    printf("COMPUTE_ICB_LAVAPIPE_PASS slots=%u buckets=%u\n", N_SLOTS, N_BUCKETS);
}
#endif /* RDNA4_TEST_LAVAPIPE */

int main(void) {
    test_command_types_and_validators();
    test_compute_slot_mutators();
    test_compute_expand_spv();
#ifdef RDNA4_TEST_HAS_VULKAN_H
    test_compute_indirect_spv_transform();
#endif
    test_source_pins();
#ifdef RDNA4_TEST_LAVAPIPE
    test_lavapipe_gpu_compute_icb();
#endif
    printf("COMPUTE_ICB_TEST_PASS abi=640B spv=ok pins=ok\n");
    return 0;
}
