#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "garnet_tessellation.h"

static int has_cap(const uint32_t *w, size_t nw, uint32_t cap) {
    for (size_t p = 5; p < nw; p += w[p] >> 16)
        if ((w[p] & 0xffffu) == 17 && (w[p] >> 16) == 2 && w[p + 1] == cap) return 1;
    return 0;
}

static int has_exec_mode(const uint32_t *w, size_t nw, uint32_t ep, uint32_t mode) {
    for (size_t p = 5; p < nw; p += w[p] >> 16)
        if ((w[p] & 0xffffu) == 16 && (w[p] >> 16) >= 3 && w[p + 1] == ep && w[p + 2] == mode) return 1;
    return 0;
}

static int has_exec_mode_arg(const uint32_t *w, size_t nw, uint32_t ep, uint32_t mode, uint32_t arg) {
    for (size_t p = 5; p < nw; p += w[p] >> 16)
        if ((w[p] & 0xffffu) == 16 && (w[p] >> 16) >= 4 && w[p + 1] == ep && w[p + 2] == mode && w[p + 3] == arg) return 1;
    return 0;
}

static int has_builtin_dec(const uint32_t *w, size_t nw, uint32_t target, uint32_t bi) {
    for (size_t p = 5; p < nw; p += w[p] >> 16)
        if ((w[p] & 0xffffu) == 71 && (w[p] >> 16) >= 4 && w[p + 1] == target && w[p + 2] == 11 && w[p + 3] == bi) return 1;
    return 0;
}

static int has_loc_patch(const uint32_t *w, size_t nw, uint32_t target, uint32_t loc) {
    int got_loc = 0, got_patch = 0;
    for (size_t p = 5; p < nw; p += w[p] >> 16) {
        if ((w[p] & 0xffffu) == 71 && w[p + 1] == target) {
            if (w[p + 2] == 30 && (w[p] >> 16) >= 4 && w[p + 3] == loc) got_loc = 1;
            if (w[p + 2] == 15) got_patch = 1;
        }
    }
    return got_loc && got_patch;
}

// Builds a minimal valid TES SPIR-V module exercising control-point, per-patch, instance_id, and PrimitiveId inputs.
static void build_test_tes(uint32_t domain_mode, int with_prim_id, int inst_loc,
                           uint8_t cp_scalar, uint8_t cp_comps, uint32_t cp_count,
                           uint8_t patch_scalar, uint8_t patch_comps,
                           uint32_t **out_w, size_t *out_nw) {
    rdna4_tcs_ctx_t ctx = {0};
    rdna4_tcs_ctx_t *k = &ctx;
    k->next_id = 1;
    uint32_t fn = rdna4_tcs_id(k);
    k->t_void = rdna4_tcs_id(k); rdna4_tb_op(&k->types, 19, 2); rdna4_tb_push(&k->types, k->t_void);
    k->t_u32  = rdna4_tcs_id(k); rdna4_tb_op(&k->types, 21, 4); rdna4_tb_push(&k->types, k->t_u32);  rdna4_tb_push(&k->types, 32); rdna4_tb_push(&k->types, 0);
    k->t_i32  = rdna4_tcs_id(k); rdna4_tb_op(&k->types, 21, 4); rdna4_tb_push(&k->types, k->t_i32);  rdna4_tb_push(&k->types, 32); rdna4_tb_push(&k->types, 1);
    k->t_f32  = rdna4_tcs_id(k); rdna4_tb_op(&k->types, 22, 3); rdna4_tb_push(&k->types, k->t_f32);  rdna4_tb_push(&k->types, 32);
    uint32_t t_v3f = rdna4_tcs_val_type(k, RDNA4_TSCALAR_F32, 3);
    uint32_t t_v4f = rdna4_tcs_val_type(k, RDNA4_TSCALAR_F32, 4);
    uint32_t t_fn = rdna4_tcs_id(k); rdna4_tb_op(&k->types, 33, 3); rdna4_tb_push(&k->types, t_fn); rdna4_tb_push(&k->types, k->t_void);

    uint32_t v_tc = rdna4_tcs_var(k, rdna4_tcs_ptr(k, 1, t_v3f), 1); rdna4_tb_dec(&k->dec, v_tc, 11, 1, 13);
    uint32_t v_pos = rdna4_tcs_var(k, rdna4_tcs_ptr(k, 3, t_v4f), 3); rdna4_tb_dec(&k->dec, v_pos, 11, 1, 0);
    if (with_prim_id) {
        uint32_t v_pid = rdna4_tcs_var(k, rdna4_tcs_ptr(k, 1, k->t_u32), 1);
        rdna4_tb_dec(&k->dec, v_pid, 11, 1, 7);
    }
    if (cp_count > 0 && cp_scalar) {
        uint32_t c_len = rdna4_tcs_u32(k, cp_count);
        uint32_t et = rdna4_tcs_val_type(k, cp_scalar, cp_comps);
        uint32_t at = rdna4_tcs_arr(k, et, c_len);
        uint32_t v_cp = rdna4_tcs_var(k, rdna4_tcs_ptr(k, 1, at), 1);
        rdna4_tb_dec(&k->dec, v_cp, 30, 1, 0);
    }
    if (patch_scalar) {
        uint32_t pt = rdna4_tcs_val_type(k, patch_scalar, patch_comps);
        uint32_t v_pa = rdna4_tcs_var(k, rdna4_tcs_ptr(k, 1, pt), 1);
        rdna4_tb_dec(&k->dec, v_pa, 30, 1, 1);
        rdna4_tb_dec(&k->dec, v_pa, 15, 0, 0);
    }
    if (inst_loc >= 0) {
        uint32_t v_ii = rdna4_tcs_var(k, rdna4_tcs_ptr(k, 1, k->t_u32), 1);
        rdna4_tb_dec(&k->dec, v_ii, 30, 1, (uint32_t)inst_loc);
        rdna4_tb_dec(&k->dec, v_ii, 15, 0, 0);
    }

    uint32_t lbl = rdna4_tcs_id(k);
    rdna4_tb_op(&k->code, 54, 5); rdna4_tb_push(&k->code, k->t_void); rdna4_tb_push(&k->code, fn); rdna4_tb_push(&k->code, 0); rdna4_tb_push(&k->code, t_fn);
    rdna4_tb_op(&k->code, 248, 2); rdna4_tb_push(&k->code, lbl);
    rdna4_tb_op(&k->code, 253, 1); rdna4_tb_op(&k->code, 56, 1);

    rdna4_tspv_buf_t mod = {0};
    rdna4_tb_push(&mod, 0x07230203u); rdna4_tb_push(&mod, 0x00010500u); rdna4_tb_push(&mod, 0);
    rdna4_tb_push(&mod, k->next_id); rdna4_tb_push(&mod, 0);
    rdna4_tb_op(&mod, 17, 2); rdna4_tb_push(&mod, 1);
    rdna4_tb_op(&mod, 17, 2); rdna4_tb_push(&mod, 3);
    if (k->need_f16) { rdna4_tb_op(&mod, 17, 2); rdna4_tb_push(&mod, 9); }
    if (k->need_i16) { rdna4_tb_op(&mod, 17, 2); rdna4_tb_push(&mod, 22); }
    if (k->need_f16 || k->need_i16) { rdna4_tb_op(&mod, 17, 2); rdna4_tb_push(&mod, 4436); }
    rdna4_tb_op(&mod, 14, 3); rdna4_tb_push(&mod, 0); rdna4_tb_push(&mod, 1);
    rdna4_tb_op(&mod, 15, (uint16_t)(3 + rdna4_tb_str_words("main") + k->niface));
    rdna4_tb_push(&mod, 2); rdna4_tb_push(&mod, fn); rdna4_tb_str(&mod, "main");
    for (uint32_t i = 0; i < k->niface; i++) rdna4_tb_push(&mod, k->iface[i]);
    rdna4_tb_op(&mod, 16, 3); rdna4_tb_push(&mod, fn); rdna4_tb_push(&mod, domain_mode);
    rdna4_tb_op(&mod, 16, 3); rdna4_tb_push(&mod, fn); rdna4_tb_push(&mod, 1);
    rdna4_tb_op(&mod, 16, 3); rdna4_tb_push(&mod, fn); rdna4_tb_push(&mod, 5);
    rdna4_tb_append(&mod, &k->dec); rdna4_tb_append(&mod, &k->types);
    rdna4_tb_append(&mod, &k->vars); rdna4_tb_append(&mod, &k->code);
    rdna4_tb_free(&k->dec); rdna4_tb_free(&k->types); rdna4_tb_free(&k->vars); rdna4_tb_free(&k->code);
    assert(!mod.oom);
    *out_w = mod.w; *out_nw = mod.len;
}

static void check_spv_parse(const uint32_t *w, size_t nw, uint32_t want_model) {
    RDNA4sMod m = {0}; char why[256] = "";
    int rc = rdna4s_parse(w, nw, &m, why, sizeof why);
    if (rc) { fprintf(stderr, "rdna4s_parse failed: %s\n", why); assert(0); }
    assert(m.has_ep && m.ep_model == want_model);
    if (want_model == 0 || want_model == 2) {
        RDNA4sRefl r = {0};
        assert(rdna4s_reflect(&m, 0, &r) == 0);
        if (want_model == 2) assert(r.nva == 0);
    }
    rdna4s_free(&m);
}

int main(void) {
    assert(sizeof(rdna4_tess_draw_params_t) == 560);

    // 1. Pass-through VS
    uint32_t *vs = NULL; size_t vs_nw = 0;
    assert(rdna4_tess_emit_vs_spv(&vs, &vs_nw));
    check_spv_parse(vs, vs_nw, 0);
    free(vs);

    // 2. Triangle & Quad inspection, patching, TCS generation & 16-bit capability gating
    for (int dom = 0; dom <= 1; dom++) {
        uint32_t *tes = NULL; size_t tes_nw = 0;
        build_test_tes(dom ? 24u : 22u, 1, 2, RDNA4_TSCALAR_F32, 4, dom ? 4 : 3,
                       RDNA4_TSCALAR_F32, 2, &tes, &tes_nw);
        RDNA4sMod m = {0}; char why[256] = "";
        assert(!rdna4s_parse(tes, tes_nw, &m, why, sizeof why));
        rdna4_tess_cfg_t cfg = {0};
        cfg.control_point_count = dom ? 4u : 3u;
        cfg.has_instance_id = 1;
        cfg.instance_id_loc = 2;
        cfg.max_tess_factor = 16.0f;
        cfg.tess_set = 2;
        assert(rdna4_tess_inspect_tes(&m, &cfg, why, sizeof why));
        assert(cfg.domain == (dom ? RDNA4_TESS_DOM_QUAD : RDNA4_TESS_DOM_TRI));
        assert(cfg.has_patch_id && cfg.patch_id_loc == 3);
        assert(cfg.has_instance_id && cfg.instance_id_loc == 2);
        assert(cfg.num_inputs == 2);
        rdna4s_free(&m);

        cfg.inputs[0].has_attr = 1; cfg.inputs[0].vfmt = RDNA4_TFMT_F32x4; cfg.inputs[0].buf_idx = 0;
        cfg.inputs[0].step_func = RDNA4_TESS_VSTEP_PER_PATCH_CP; cfg.inputs[0].step_rate = 1; cfg.inputs[0].buf_stride = 16;
        cfg.inputs[1].has_attr = 1; cfg.inputs[1].vfmt = RDNA4_TFMT_U8x2_N; cfg.inputs[1].buf_idx = 1;
        cfg.inputs[1].step_func = RDNA4_TESS_VSTEP_PER_PATCH; cfg.inputs[1].step_rate = 1; cfg.inputs[1].buf_stride = 4;

        static const uint8_t parts[4] = { RDNA4_TESS_PART_POW2, RDNA4_TESS_PART_INTEGER, RDNA4_TESS_PART_FRAC_ODD, RDNA4_TESS_PART_FRAC_EVEN };
        static const uint32_t sp_modes[4] = { 1, 1, 3, 2 };
        for (int p = 0; p < 4; p++) {
            cfg.partition = parts[p];
            cfg.winding = (p & 1) ? RDNA4_TESS_WIND_CCW : RDNA4_TESS_WIND_CW;
            cfg.factor_step = (uint8_t)p;
            cfg.cp_idx_type = (uint8_t)(p % 3);
            cfg.factor_scale_enabled = (uint8_t)(p & 1);
            assert(rdna4_tess_validate_cfg(&cfg, 64, 32, why, sizeof why));

            uint32_t *ptes = NULL; size_t ptes_nw = 0;
            assert(rdna4_tess_patch_tes_spv(tes, tes_nw, &cfg, &ptes, &ptes_nw));
            check_spv_parse(ptes, ptes_nw, 2);
            assert(has_exec_mode(ptes, ptes_nw, 1, sp_modes[p]));
            assert(has_exec_mode(ptes, ptes_nw, 1, cfg.winding == RDNA4_TESS_WIND_CW ? 4u : 5u));
            assert(!has_builtin_dec(ptes, ptes_nw, cfg.patch_id_var, 7));
            assert(has_loc_patch(ptes, ptes_nw, cfg.patch_id_var, cfg.patch_id_loc));
            free(ptes);

            uint32_t *tcs = NULL; size_t tcs_nw = 0;
            assert(rdna4_tess_emit_tcs_spv(&cfg, &tcs, &tcs_nw));
            check_spv_parse(tcs, tcs_nw, 1);
            assert(!has_cap(tcs, tcs_nw, 9));    // Float16 omitted for 32-bit outputs
            assert(!has_cap(tcs, tcs_nw, 22));   // Int16 omitted for 32-bit outputs
            assert(!has_cap(tcs, tcs_nw, 4436)); // StorageInputOutput16 omitted
            free(tcs);
        }
        free(tes);
    }

    // 3. 16-bit capability emission + every vertex format family
    {
        uint32_t *tes = NULL; size_t tes_nw = 0;
        build_test_tes(22u, 0, -1, RDNA4_TSCALAR_F16, 4, 3, RDNA4_TSCALAR_S16, 2, &tes, &tes_nw);
        RDNA4sMod m = {0}; char why[256] = "";
        assert(!rdna4s_parse(tes, tes_nw, &m, why, sizeof why));
        rdna4_tess_cfg_t cfg = {0};
        cfg.control_point_count = 3; cfg.max_tess_factor = 64.0f; cfg.tess_set = 5;
        assert(rdna4_tess_inspect_tes(&m, &cfg, why, sizeof why));
        rdna4s_free(&m);

        static const uint8_t fmts[] = {
            RDNA4_TFMT_U8x1, RDNA4_TFMT_S8x3, RDNA4_TFMT_U8x4_N, RDNA4_TFMT_S8x4_N, RDNA4_TFMT_BGRA8_N,
            RDNA4_TFMT_U16x2, RDNA4_TFMT_S16x3, RDNA4_TFMT_U16x4_N, RDNA4_TFMT_S16x2_N, RDNA4_TFMT_F16x3,
            RDNA4_TFMT_F32x2, RDNA4_TFMT_S32x4, RDNA4_TFMT_U32x1, RDNA4_TFMT_S1010102_N, RDNA4_TFMT_U1010102_N
        };
        for (size_t i = 0; i < sizeof fmts; i++) {
            cfg.inputs[0].has_attr = 1; cfg.inputs[0].vfmt = fmts[i]; cfg.inputs[0].buf_idx = 0;
            cfg.inputs[0].step_func = RDNA4_TESS_VSTEP_PER_PATCH_CP; cfg.inputs[0].step_rate = 1; cfg.inputs[0].buf_stride = 32;
            cfg.inputs[1].has_attr = (i & 1); cfg.inputs[1].vfmt = fmts[i]; cfg.inputs[1].buf_idx = 1;
            cfg.inputs[1].step_func = RDNA4_TESS_VSTEP_CONST;
            assert(rdna4_tess_validate_cfg(&cfg, 64, 32, why, sizeof why));
            uint32_t *tcs = NULL; size_t tcs_nw = 0;
            assert(rdna4_tess_emit_tcs_spv(&cfg, &tcs, &tcs_nw));
            check_spv_parse(tcs, tcs_nw, 1);
            assert(has_cap(tcs, tcs_nw, 9));
            assert(has_cap(tcs, tcs_nw, 22));
            assert(has_cap(tcs, tcs_nw, 4436));
            free(tcs);
        }
        free(tes);
    }

    // 4. Zero-control-point patch
    {
        uint32_t *tes = NULL; size_t tes_nw = 0;
        build_test_tes(22u, 1, 0, RDNA4_TSCALAR_NONE, 0, 0, RDNA4_TSCALAR_U32, 1, &tes, &tes_nw);
        RDNA4sMod m = {0}; char why[256] = "";
        assert(!rdna4s_parse(tes, tes_nw, &m, why, sizeof why));
        rdna4_tess_cfg_t cfg = {0};
        cfg.control_point_count = 0; cfg.has_instance_id = 1; cfg.instance_id_loc = 0;
        cfg.max_tess_factor = 16.0f; cfg.tess_set = 2;
        assert(rdna4_tess_inspect_tes(&m, &cfg, why, sizeof why));
        rdna4s_free(&m);
        assert(cfg.num_inputs == 1 && cfg.inputs[0].is_patch);
        assert(rdna4_tess_validate_cfg(&cfg, 64, 32, why, sizeof why));
        uint32_t *tcs = NULL; size_t tcs_nw = 0;
        assert(rdna4_tess_emit_tcs_spv(&cfg, &tcs, &tcs_nw));
        check_spv_parse(tcs, tcs_nw, 1);
        assert(has_exec_mode_arg(tcs, tcs_nw, 1, 26u, 1u)); // OutputVertices clamped to 1
        free(tcs);
        free(tes);
    }

    // 5. Pipeline cache key isolation & negative validation checks
    {
        rdna4_tess_cfg_t a = {0};
        a.max_tess_factor = 16.0f; a.control_point_count = 3; a.tess_set = 2;
        a.num_inputs = 1;
        a.inputs[0] = (rdna4_tess_input_t){ .location = 0, .has_attr = 1, .vfmt = RDNA4_TFMT_F32x4,
            .buf_idx = 0, .step_func = RDNA4_TESS_VSTEP_PER_PATCH_CP, .tes_scalar = RDNA4_TSCALAR_F32, .tes_comps = 4,
            .step_rate = 1, .attr_offset = 0, .buf_stride = 16 };
        static const uint32_t dummy_tes[4] = { 1, 2, 3, 4 };
        uint64_t k0 = rdna4_tess_pipeline_cache_key(&a, dummy_tes, sizeof dummy_tes);
        rdna4_tess_cfg_t b = a;
        b.partition = RDNA4_TESS_PART_FRAC_ODD; assert(rdna4_tess_pipeline_cache_key(&b, dummy_tes, sizeof dummy_tes) != k0);
        b = a; b.winding = RDNA4_TESS_WIND_CCW; assert(rdna4_tess_pipeline_cache_key(&b, dummy_tes, sizeof dummy_tes) != k0);
        b = a; b.factor_step = RDNA4_TESS_FSTEP_PER_PATCH; assert(rdna4_tess_pipeline_cache_key(&b, dummy_tes, sizeof dummy_tes) != k0);
        b = a; b.factor_scale_enabled = 1; assert(rdna4_tess_pipeline_cache_key(&b, dummy_tes, sizeof dummy_tes) != k0);
        b = a; b.max_tess_factor = 32.0f; assert(rdna4_tess_pipeline_cache_key(&b, dummy_tes, sizeof dummy_tes) != k0);
        b = a; b.cp_idx_type = RDNA4_TESS_CPIDX_U16; assert(rdna4_tess_pipeline_cache_key(&b, dummy_tes, sizeof dummy_tes) != k0);
        b = a; b.inputs[0].vfmt = RDNA4_TFMT_F16x4; assert(rdna4_tess_pipeline_cache_key(&b, dummy_tes, sizeof dummy_tes) != k0);

        char why[256] = "";
        assert(rdna4_tess_validate_cfg(&a, 64, 32, why, sizeof why));
        b = a; b.max_tess_factor = 0.0f; assert(!rdna4_tess_validate_cfg(&b, 64, 32, why, sizeof why));
        b = a; b.max_tess_factor = 65.0f; assert(!rdna4_tess_validate_cfg(&b, 64, 32, why, sizeof why));
        b = a; b.max_tess_factor = NAN; assert(!rdna4_tess_validate_cfg(&b, 64, 32, why, sizeof why));
        b = a; b.control_point_count = 33; assert(!rdna4_tess_validate_cfg(&b, 64, 32, why, sizeof why));
        b = a; b.inputs[0].step_func = RDNA4_TESS_VSTEP_PER_VERTEX; assert(!rdna4_tess_validate_cfg(&b, 64, 32, why, sizeof why));
        b = a; b.inputs[0].step_func = RDNA4_TESS_VSTEP_PER_INSTANCE; assert(!rdna4_tess_validate_cfg(&b, 64, 32, why, sizeof why));
        b = a; b.inputs[0].buf_stride = 2049; assert(!rdna4_tess_validate_cfg(&b, 64, 32, why, sizeof why));
        b = a; b.inputs[0].attr_offset = 2048; assert(!rdna4_tess_validate_cfg(&b, 64, 32, why, sizeof why));
        b = a; b.inputs[0].buf_idx = 31; assert(!rdna4_tess_validate_cfg(&b, 64, 32, why, sizeof why));
        b = a; b.inputs[0].is_patch = 1; assert(!rdna4_tess_validate_cfg(&b, 64, 32, why, sizeof why));
    }

    printf("TEST_TESSELLATION_SPV_PASS\n");
    return 0;
}
