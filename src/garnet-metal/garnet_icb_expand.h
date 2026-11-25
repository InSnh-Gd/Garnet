// Pure-C SPIR-V generator for the Metal 2 GPU Indirect Command Buffer expander.
// Expands direct or GPU-indirect slot ranges in strict slot order without CPU readback.
#ifndef RDNA4_ICB_EXPAND_H
#define RDNA4_ICB_EXPAND_H

#include "garnet_icb_abi.h"
#include "garnet_buffer_ranges.h"
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#define RDNA4_ICB_MAX_EXPAND_BUCKETS 160u /* 32 candidate pipelines * 5 primitive topologies */

typedef struct {
    uint64_t token;
    uint32_t is_tess;
    uint32_t prim_type;
} rdna4_icb_bucket_t;

typedef struct {
    uint32_t direct_location;             /* +0   (dword 0) */
    uint32_t direct_length;               /* +4   (dword 1) */
    uint32_t use_indirect_range;          /* +8   (dword 2) */
    uint32_t max_output_slots;            /* +12  (dword 3) */
    uint32_t inherit_pipeline;            /* +16  (dword 4) */
    uint32_t inherit_buffers;             /* +20  (dword 5) */
    uint32_t num_buckets;                 /* +24  (dword 6) */
    uint32_t _pad0;                       /* +28  (dword 7) */
    uint64_t parent_pipeline_token;       /* +32  (dword 8..9) */
    uint64_t parent_tess_factor_bda;      /* +40  (dword 10..11) */
    uint64_t parent_tess_factor_size;     /* +48  (dword 12..13) */
    uint32_t parent_tess_factor_stride;   /* +56  (dword 14) */
    float    parent_tess_factor_scale;    /* +60  (dword 15) */
    rdna4_icb_bucket_t buckets[RDNA4_ICB_MAX_EXPAND_BUCKETS]; /* +64 (dwords 16..655) */
    rdna4_buffer_table parent_v_table;    /* +2624 (dwords 656..847) */
    rdna4_buffer_table parent_f_table;    /* +3392 (dwords 848..1039) */
    uint64_t parent_vb_bda[32];           /* +4160 (dwords 1040..1103) */
    uint64_t parent_vb_size[32];          /* +4416 (dwords 1104..1167) */
} rdna4_icb_expand_cfg_t;

#if defined(__STDC_VERSION__) && __STDC_VERSION__ >= 201112L
_Static_assert(sizeof(rdna4_icb_bucket_t) == 16, "rdna4_icb_bucket_t must be 16 bytes");
_Static_assert(sizeof(rdna4_icb_expand_cfg_t) == 4672, "rdna4_icb_expand_cfg_t must be 4672 bytes");
_Static_assert(offsetof(rdna4_icb_expand_cfg_t, parent_pipeline_token) == 32, "cfg parent_pipeline_token offset");
_Static_assert(offsetof(rdna4_icb_expand_cfg_t, parent_tess_factor_scale) == 60, "cfg parent_tess_factor_scale offset");
_Static_assert(offsetof(rdna4_icb_expand_cfg_t, buckets) == 64, "cfg buckets offset");
_Static_assert(offsetof(rdna4_icb_expand_cfg_t, parent_v_table) == 2624, "cfg parent_v_table offset");
_Static_assert(offsetof(rdna4_icb_expand_cfg_t, parent_f_table) == 3392, "cfg parent_f_table offset");
_Static_assert(offsetof(rdna4_icb_expand_cfg_t, parent_vb_bda) == 4160, "cfg parent_vb_bda offset");
_Static_assert(offsetof(rdna4_icb_expand_cfg_t, parent_vb_size) == 4416, "cfg parent_vb_size offset");
#endif

typedef struct {
    uint32_t *w;
    size_t len, cap;
    int oom;
} rdna4_icbe_buf_t;

static inline void rdna4_icbe_push(rdna4_icbe_buf_t *b, uint32_t x) {
    if (b->oom) return;
    if (b->len == b->cap) {
        size_t nc = b->cap ? b->cap * 2 : 512;
        uint32_t *nw = (uint32_t *)realloc(b->w, nc * sizeof(uint32_t));
        if (!nw) { b->oom = 1; return; }
        b->w = nw; b->cap = nc;
    }
    b->w[b->len++] = x;
}

static inline void rdna4_icbe_op(rdna4_icbe_buf_t *b, uint16_t op, uint16_t wc) {
    rdna4_icbe_push(b, ((uint32_t)wc << 16) | op);
}

static inline void rdna4_icbe_str(rdna4_icbe_buf_t *b, const char *s) {
    size_t n = strlen(s) + 1;
    size_t nw = (n + 3) / 4;
    for (size_t i = 0; i < nw; i++) {
        uint32_t w = 0;
        for (size_t k = 0; k < 4 && (i * 4 + k) < n; k++)
            w |= (uint32_t)(uint8_t)s[i * 4 + k] << (k * 8);
        rdna4_icbe_push(b, w);
    }
}

static inline void rdna4_icbe_dec(rdna4_icbe_buf_t *b, uint32_t target, uint32_t dec, int has_arg, uint32_t arg) {
    rdna4_icbe_op(b, 71, has_arg ? 4 : 3);
    rdna4_icbe_push(b, target); rdna4_icbe_push(b, dec);
    if (has_arg) rdna4_icbe_push(b, arg);
}

static inline void rdna4_icbe_mdec(rdna4_icbe_buf_t *b, uint32_t st, uint32_t mi, uint32_t dec, uint32_t arg) {
    rdna4_icbe_op(b, 72, 5);
    rdna4_icbe_push(b, st); rdna4_icbe_push(b, mi); rdna4_icbe_push(b, dec); rdna4_icbe_push(b, arg);
}

typedef struct {
    rdna4_icbe_buf_t dec, types, vars, code;
    uint32_t next_id;
    uint32_t t_void, t_bool, t_u32, t_v3u32, t_fn;
    uint32_t t_p_in_v3u32, t_p_sb_u32, t_rt_u32, t_st_buf, t_p_sb_st;
    uint32_t v_gid, v_buf[8];
    uint32_t c_u32[64];
} rdna4_icbe_ctx_t;

static inline uint32_t rdna4_icbe_id(rdna4_icbe_ctx_t *k) { return k->next_id++; }

static inline uint32_t rdna4_icbe_const(rdna4_icbe_ctx_t *k, uint32_t val) {
    if (val < 64 && k->c_u32[val]) return k->c_u32[val];
    uint32_t id = rdna4_icbe_id(k);
    rdna4_icbe_op(&k->types, 43, 4);
    rdna4_icbe_push(&k->types, k->t_u32); rdna4_icbe_push(&k->types, id); rdna4_icbe_push(&k->types, val);
    if (val < 64) k->c_u32[val] = id;
    return id;
}

static inline uint32_t rdna4_icbe_op1(rdna4_icbe_ctx_t *k, uint16_t op, uint32_t rt, uint32_t a) {
    uint32_t id = rdna4_icbe_id(k);
    rdna4_icbe_op(&k->code, op, 4);
    rdna4_icbe_push(&k->code, rt); rdna4_icbe_push(&k->code, id); rdna4_icbe_push(&k->code, a);
    return id;
}

static inline uint32_t rdna4_icbe_op2(rdna4_icbe_ctx_t *k, uint16_t op, uint32_t rt, uint32_t a, uint32_t b) {
    uint32_t id = rdna4_icbe_id(k);
    rdna4_icbe_op(&k->code, op, 5);
    rdna4_icbe_push(&k->code, rt); rdna4_icbe_push(&k->code, id); rdna4_icbe_push(&k->code, a); rdna4_icbe_push(&k->code, b);
    return id;
}

static inline uint32_t rdna4_icbe_sel(rdna4_icbe_ctx_t *k, uint32_t rt, uint32_t c, uint32_t tv, uint32_t fv) {
    uint32_t id = rdna4_icbe_id(k);
    rdna4_icbe_op(&k->code, 169, 6);
    rdna4_icbe_push(&k->code, rt); rdna4_icbe_push(&k->code, id);
    rdna4_icbe_push(&k->code, c); rdna4_icbe_push(&k->code, tv); rdna4_icbe_push(&k->code, fv);
    return id;
}

static inline uint32_t rdna4_icbe_ld(rdna4_icbe_ctx_t *k, uint32_t var_id, uint32_t idx_id) {
    uint32_t ac = rdna4_icbe_id(k), val = rdna4_icbe_id(k);
    uint32_t c0 = rdna4_icbe_const(k, 0);
    rdna4_icbe_op(&k->code, 65, 6);
    rdna4_icbe_push(&k->code, k->t_p_sb_u32); rdna4_icbe_push(&k->code, ac);
    rdna4_icbe_push(&k->code, var_id); rdna4_icbe_push(&k->code, c0); rdna4_icbe_push(&k->code, idx_id);
    rdna4_icbe_op(&k->code, 61, 4);
    rdna4_icbe_push(&k->code, k->t_u32); rdna4_icbe_push(&k->code, val); rdna4_icbe_push(&k->code, ac);
    return val;
}

static inline uint32_t rdna4_icbe_ld_c(rdna4_icbe_ctx_t *k, uint32_t var_id, uint32_t off) {
    return rdna4_icbe_ld(k, var_id, rdna4_icbe_const(k, off));
}

static inline void rdna4_icbe_st(rdna4_icbe_ctx_t *k, uint32_t var_id, uint32_t idx_id, uint32_t val_id) {
    uint32_t ac = rdna4_icbe_id(k);
    uint32_t c0 = rdna4_icbe_const(k, 0);
    rdna4_icbe_op(&k->code, 65, 6);
    rdna4_icbe_push(&k->code, k->t_p_sb_u32); rdna4_icbe_push(&k->code, ac);
    rdna4_icbe_push(&k->code, var_id); rdna4_icbe_push(&k->code, c0); rdna4_icbe_push(&k->code, idx_id);
    rdna4_icbe_op(&k->code, 62, 3);
    rdna4_icbe_push(&k->code, ac); rdna4_icbe_push(&k->code, val_id);
}

static inline void rdna4_icbe_lbl(rdna4_icbe_ctx_t *k, uint32_t lbl) {
    rdna4_icbe_op(&k->code, 248, 2); rdna4_icbe_push(&k->code, lbl);
}

static inline void rdna4_icbe_br(rdna4_icbe_ctx_t *k, uint32_t lbl) {
    rdna4_icbe_op(&k->code, 249, 2); rdna4_icbe_push(&k->code, lbl);
}

static inline void rdna4_icbe_smerge(rdna4_icbe_ctx_t *k, uint32_t merge_lbl) {
    rdna4_icbe_op(&k->code, 247, 3); rdna4_icbe_push(&k->code, merge_lbl); rdna4_icbe_push(&k->code, 0);
}

static inline void rdna4_icbe_lmerge(rdna4_icbe_ctx_t *k, uint32_t merge_lbl, uint32_t cont_lbl) {
    rdna4_icbe_op(&k->code, 246, 4); rdna4_icbe_push(&k->code, merge_lbl); rdna4_icbe_push(&k->code, cont_lbl); rdna4_icbe_push(&k->code, 0);
}

static inline void rdna4_icbe_bcond(rdna4_icbe_ctx_t *k, uint32_t cond, uint32_t t_lbl, uint32_t f_lbl) {
    rdna4_icbe_op(&k->code, 250, 4); rdna4_icbe_push(&k->code, cond); rdna4_icbe_push(&k->code, t_lbl); rdna4_icbe_push(&k->code, f_lbl);
}

static inline uint32_t rdna4_icbe_phi1(rdna4_icbe_ctx_t *k, uint32_t rt, uint32_t v0, uint32_t l0, uint32_t v1, uint32_t l1) {
    uint32_t id = rdna4_icbe_id(k);
    rdna4_icbe_op(&k->code, 245, 7);
    rdna4_icbe_push(&k->code, rt); rdna4_icbe_push(&k->code, id);
    rdna4_icbe_push(&k->code, v0); rdna4_icbe_push(&k->code, l0);
    rdna4_icbe_push(&k->code, v1); rdna4_icbe_push(&k->code, l1);
    return id;
}

static inline int rdna4_icb_emit_expand_spv(uint32_t **out_words, size_t *out_nwords) {
    if (!out_words || !out_nwords) return 0;
    rdna4_icbe_ctx_t ctx;
    memset(&ctx, 0, sizeof ctx);
    ctx.next_id = 1;

    uint32_t fn_main = rdna4_icbe_id(&ctx);
    ctx.t_void = rdna4_icbe_id(&ctx);
    ctx.t_bool = rdna4_icbe_id(&ctx);
    ctx.t_u32 = rdna4_icbe_id(&ctx);
    ctx.t_v3u32 = rdna4_icbe_id(&ctx);
    ctx.t_fn = rdna4_icbe_id(&ctx);
    ctx.t_p_in_v3u32 = rdna4_icbe_id(&ctx);
    ctx.t_p_sb_u32 = rdna4_icbe_id(&ctx);
    ctx.t_rt_u32 = rdna4_icbe_id(&ctx);
    ctx.t_st_buf = rdna4_icbe_id(&ctx);
    ctx.t_p_sb_st = rdna4_icbe_id(&ctx);
    ctx.v_gid = rdna4_icbe_id(&ctx);
    for (int i = 0; i < 8; i++) ctx.v_buf[i] = rdna4_icbe_id(&ctx);

    rdna4_icbe_op(&ctx.types, 19, 2); rdna4_icbe_push(&ctx.types, ctx.t_void);
    rdna4_icbe_op(&ctx.types, 20, 2); rdna4_icbe_push(&ctx.types, ctx.t_bool);
    rdna4_icbe_op(&ctx.types, 21, 4); rdna4_icbe_push(&ctx.types, ctx.t_u32); rdna4_icbe_push(&ctx.types, 32); rdna4_icbe_push(&ctx.types, 0);
    rdna4_icbe_op(&ctx.types, 23, 4); rdna4_icbe_push(&ctx.types, ctx.t_v3u32); rdna4_icbe_push(&ctx.types, ctx.t_u32); rdna4_icbe_push(&ctx.types, 3);
    rdna4_icbe_op(&ctx.types, 33, 3); rdna4_icbe_push(&ctx.types, ctx.t_fn); rdna4_icbe_push(&ctx.types, ctx.t_void);
    rdna4_icbe_op(&ctx.types, 32, 4); rdna4_icbe_push(&ctx.types, ctx.t_p_in_v3u32); rdna4_icbe_push(&ctx.types, 1); rdna4_icbe_push(&ctx.types, ctx.t_v3u32);
    rdna4_icbe_op(&ctx.types, 32, 4); rdna4_icbe_push(&ctx.types, ctx.t_p_sb_u32); rdna4_icbe_push(&ctx.types, 12); rdna4_icbe_push(&ctx.types, ctx.t_u32);
    rdna4_icbe_op(&ctx.types, 29, 3); rdna4_icbe_push(&ctx.types, ctx.t_rt_u32); rdna4_icbe_push(&ctx.types, ctx.t_u32);
    rdna4_icbe_op(&ctx.types, 30, 3); rdna4_icbe_push(&ctx.types, ctx.t_st_buf); rdna4_icbe_push(&ctx.types, ctx.t_rt_u32);
    rdna4_icbe_op(&ctx.types, 32, 4); rdna4_icbe_push(&ctx.types, ctx.t_p_sb_st); rdna4_icbe_push(&ctx.types, 12); rdna4_icbe_push(&ctx.types, ctx.t_st_buf);

    rdna4_icbe_dec(&ctx.dec, ctx.v_gid, 11, 1, 28); // BuiltIn GlobalInvocationId
    rdna4_icbe_dec(&ctx.dec, ctx.t_rt_u32, 6, 1, 4); // ArrayStride 4
    rdna4_icbe_mdec(&ctx.dec, ctx.t_st_buf, 0, 35, 0); // Offset 0
    rdna4_icbe_dec(&ctx.dec, ctx.t_st_buf, 2, 0, 0);   // Block
    for (uint32_t i = 0; i < 8; i++) {
        rdna4_icbe_dec(&ctx.dec, ctx.v_buf[i], 34, 1, 0);
        rdna4_icbe_dec(&ctx.dec, ctx.v_buf[i], 33, 1, i);
        if (i < 3) rdna4_icbe_dec(&ctx.dec, ctx.v_buf[i], 24, 0, 0); // NonWritable
        else rdna4_icbe_dec(&ctx.dec, ctx.v_buf[i], 25, 0, 0);       // NonReadable
    }

    rdna4_icbe_op(&ctx.vars, 59, 4); rdna4_icbe_push(&ctx.vars, ctx.t_p_in_v3u32); rdna4_icbe_push(&ctx.vars, ctx.v_gid); rdna4_icbe_push(&ctx.vars, 1);
    for (int i = 0; i < 8; i++) {
        rdna4_icbe_op(&ctx.vars, 59, 4); rdna4_icbe_push(&ctx.vars, ctx.t_p_sb_st); rdna4_icbe_push(&ctx.vars, ctx.v_buf[i]); rdna4_icbe_push(&ctx.vars, 12);
    }

    uint32_t c0 = rdna4_icbe_const(&ctx, 0), c1 = rdna4_icbe_const(&ctx, 1), c2 = rdna4_icbe_const(&ctx, 2), c3 = rdna4_icbe_const(&ctx, 3);
    uint32_t c4 = rdna4_icbe_const(&ctx, 4), c16 = rdna4_icbe_const(&ctx, 16), c31 = rdna4_icbe_const(&ctx, 31);
    uint32_t c_64g_hi = rdna4_icbe_const(&ctx, 0x10u);
    uint32_t c_vs_str = rdna4_icbe_const(&ctx, RDNA4_ICB_VS_PARAMS_STRIDE / 4u);
    uint32_t c_ts_str = rdna4_icbe_const(&ctx, RDNA4_ICB_TESS_PARAMS_STRIDE / 4u);
    uint32_t c_tb_str = rdna4_icbe_const(&ctx, RDNA4_ICB_TABLE_V2_STRIDE / 4u);
    uint32_t c_slot_dw = rdna4_icbe_const(&ctx, RDNA4_ICB_SLOT_DWORDS);
    uint32_t c_64 = rdna4_icbe_const(&ctx, 64), c_128 = rdna4_icbe_const(&ctx, 128);
    uint32_t c_78 = rdna4_icbe_const(&ctx, 78);

    uint32_t b_entry = rdna4_icbe_id(&ctx), b_act = rdna4_icbe_id(&ctx), b_slot = rdna4_icbe_id(&ctx);
    uint32_t b_hdr_v = rdna4_icbe_id(&ctx), b_hdr_c = rdna4_icbe_id(&ctx), b_hdr_d = rdna4_icbe_id(&ctx);
    uint32_t b_emit = rdna4_icbe_id(&ctx), b_dl_h = rdna4_icbe_id(&ctx), b_dl_b = rdna4_icbe_id(&ctx);
    uint32_t b_dl_c = rdna4_icbe_id(&ctx), b_dl_d = rdna4_icbe_id(&ctx), b_ret = rdna4_icbe_id(&ctx);

    // Function main
    rdna4_icbe_op(&ctx.code, 54, 5);
    rdna4_icbe_push(&ctx.code, ctx.t_void); rdna4_icbe_push(&ctx.code, fn_main); rdna4_icbe_push(&ctx.code, 0); rdna4_icbe_push(&ctx.code, ctx.t_fn);
    rdna4_icbe_lbl(&ctx, b_entry);

    uint32_t gid_v = rdna4_icbe_id(&ctx), k = rdna4_icbe_id(&ctx);
    rdna4_icbe_op(&ctx.code, 61, 4); rdna4_icbe_push(&ctx.code, ctx.t_v3u32); rdna4_icbe_push(&ctx.code, gid_v); rdna4_icbe_push(&ctx.code, ctx.v_gid);
    rdna4_icbe_op(&ctx.code, 81, 5); rdna4_icbe_push(&ctx.code, ctx.t_u32); rdna4_icbe_push(&ctx.code, k); rdna4_icbe_push(&ctx.code, gid_v); rdna4_icbe_push(&ctx.code, 0);

    uint32_t max_out = rdna4_icbe_ld_c(&ctx, ctx.v_buf[1], 3);
    uint32_t k_in = rdna4_icbe_op2(&ctx, 176, ctx.t_bool, k, max_out); // ULessThan
    rdna4_icbe_smerge(&ctx, b_ret);
    rdna4_icbe_bcond(&ctx, k_in, b_act, b_ret);

    // b_act: determine active range and output bases
    rdna4_icbe_lbl(&ctx, b_act);
    uint32_t dir_loc = rdna4_icbe_ld_c(&ctx, ctx.v_buf[1], 0);
    uint32_t dir_len = rdna4_icbe_ld_c(&ctx, ctx.v_buf[1], 1);
    uint32_t use_ind = rdna4_icbe_ld_c(&ctx, ctx.v_buf[1], 2);
    uint32_t inh_pipe = rdna4_icbe_ld_c(&ctx, ctx.v_buf[1], 4);
    uint32_t inh_buf = rdna4_icbe_ld_c(&ctx, ctx.v_buf[1], 5);
    uint32_t num_bkt = rdna4_icbe_ld_c(&ctx, ctx.v_buf[1], 6);
    uint32_t ptok_lo = rdna4_icbe_ld_c(&ctx, ctx.v_buf[1], 8);
    uint32_t ptok_hi = rdna4_icbe_ld_c(&ctx, ctx.v_buf[1], 9);
    uint32_t pf_scale = rdna4_icbe_ld_c(&ctx, ctx.v_buf[1], 15);
    uint32_t ind_loc = rdna4_icbe_ld_c(&ctx, ctx.v_buf[2], 0);
    uint32_t ind_len = rdna4_icbe_ld_c(&ctx, ctx.v_buf[2], 1);
    uint32_t u_ind_b = rdna4_icbe_op2(&ctx, 171, ctx.t_bool, use_ind, c0);
    uint32_t loc = rdna4_icbe_sel(&ctx, ctx.t_u32, u_ind_b, ind_loc, dir_loc);
    uint32_t rlen = rdna4_icbe_sel(&ctx, ctx.t_u32, u_ind_b, ind_len, dir_len);
    uint32_t max_cmd = rdna4_icbe_ld_c(&ctx, ctx.v_buf[0], 1);
    uint32_t cmd_types = rdna4_icbe_ld_c(&ctx, ctx.v_buf[0], 2);

    uint32_t c_len = rdna4_icbe_op2(&ctx, 176, ctx.t_bool, k, rlen);
    uint32_t c_loc = rdna4_icbe_op2(&ctx, 178, ctx.t_bool, loc, max_cmd); // ULessThanEqual
    uint32_t rem_cmd = rdna4_icbe_op2(&ctx, 130, ctx.t_u32, max_cmd, loc);
    uint32_t c_rem = rdna4_icbe_op2(&ctx, 176, ctx.t_bool, k, rem_cmd);
    uint32_t act0 = rdna4_icbe_op2(&ctx, 167, ctx.t_bool, c_len, c_loc);
    uint32_t in_range = rdna4_icbe_op2(&ctx, 167, ctx.t_bool, act0, c_rem);

    uint32_t vpbase = rdna4_icbe_op2(&ctx, 132, ctx.t_u32, k, c_vs_str);
    uint32_t tpbase = rdna4_icbe_op2(&ctx, 132, ctx.t_u32, k, c_ts_str);
    uint32_t tbbase = rdna4_icbe_op2(&ctx, 132, ctx.t_u32, k, c_tb_str);

    rdna4_icbe_smerge(&ctx, b_emit);
    rdna4_icbe_bcond(&ctx, in_range, b_slot, b_emit);

    // b_slot: read slot[loc + k], validate draw, populate vs/tess params & v/f tables
    rdna4_icbe_lbl(&ctx, b_slot);
    uint32_t slot_idx = rdna4_icbe_op2(&ctx, 128, ctx.t_u32, loc, k);
    uint32_t smul = rdna4_icbe_op2(&ctx, 132, ctx.t_u32, slot_idx, c_slot_dw);
    uint32_t sbase = rdna4_icbe_op2(&ctx, 128, ctx.t_u32, c16, smul);

    uint32_t kind = rdna4_icbe_ld(&ctx, ctx.v_buf[0], sbase);
    uint32_t prim = rdna4_icbe_ld(&ctx, ctx.v_buf[0], rdna4_icbe_op2(&ctx, 128, ctx.t_u32, sbase, c1));
    uint32_t itype = rdna4_icbe_ld(&ctx, ctx.v_buf[0], rdna4_icbe_op2(&ctx, 128, ctx.t_u32, sbase, c2));
    uint32_t sflags = rdna4_icbe_ld(&ctx, ctx.v_buf[0], rdna4_icbe_op2(&ctx, 128, ctx.t_u32, sbase, c3));
    uint32_t stok_lo = rdna4_icbe_ld(&ctx, ctx.v_buf[0], rdna4_icbe_op2(&ctx, 128, ctx.t_u32, sbase, c4));
    uint32_t stok_hi = rdna4_icbe_ld(&ctx, ctx.v_buf[0], rdna4_icbe_op2(&ctx, 128, ctx.t_u32, sbase, rdna4_icbe_const(&ctx, 5)));
    uint32_t scnt = rdna4_icbe_ld(&ctx, ctx.v_buf[0], rdna4_icbe_op2(&ctx, 128, ctx.t_u32, sbase, rdna4_icbe_const(&ctx, 8)));
    uint32_t sinst = rdna4_icbe_ld(&ctx, ctx.v_buf[0], rdna4_icbe_op2(&ctx, 128, ctx.t_u32, sbase, rdna4_icbe_const(&ctx, 9)));
    uint32_t sfirst = rdna4_icbe_ld(&ctx, ctx.v_buf[0], rdna4_icbe_op2(&ctx, 128, ctx.t_u32, sbase, rdna4_icbe_const(&ctx, 10)));
    uint32_t sbinst = rdna4_icbe_ld(&ctx, ctx.v_buf[0], rdna4_icbe_op2(&ctx, 128, ctx.t_u32, sbase, rdna4_icbe_const(&ctx, 11)));
    uint32_t sbvert = rdna4_icbe_ld(&ctx, ctx.v_buf[0], rdna4_icbe_op2(&ctx, 128, ctx.t_u32, sbase, rdna4_icbe_const(&ctx, 12)));
    uint32_t spcp = rdna4_icbe_ld(&ctx, ctx.v_buf[0], rdna4_icbe_op2(&ctx, 128, ctx.t_u32, sbase, rdna4_icbe_const(&ctx, 13)));
    uint32_t ststride = rdna4_icbe_ld(&ctx, ctx.v_buf[0], rdna4_icbe_op2(&ctx, 128, ctx.t_u32, sbase, rdna4_icbe_const(&ctx, 14)));
    uint32_t ibda_lo = rdna4_icbe_ld(&ctx, ctx.v_buf[0], rdna4_icbe_op2(&ctx, 128, ctx.t_u32, sbase, rdna4_icbe_const(&ctx, 16)));
    uint32_t ibda_hi = rdna4_icbe_ld(&ctx, ctx.v_buf[0], rdna4_icbe_op2(&ctx, 128, ctx.t_u32, sbase, rdna4_icbe_const(&ctx, 17)));
    uint32_t pbda_lo = rdna4_icbe_ld(&ctx, ctx.v_buf[0], rdna4_icbe_op2(&ctx, 128, ctx.t_u32, sbase, rdna4_icbe_const(&ctx, 18)));
    uint32_t pbda_hi = rdna4_icbe_ld(&ctx, ctx.v_buf[0], rdna4_icbe_op2(&ctx, 128, ctx.t_u32, sbase, rdna4_icbe_const(&ctx, 19)));
    uint32_t fbda_lo = rdna4_icbe_ld(&ctx, ctx.v_buf[0], rdna4_icbe_op2(&ctx, 128, ctx.t_u32, sbase, rdna4_icbe_const(&ctx, 20)));
    uint32_t fbda_hi = rdna4_icbe_ld(&ctx, ctx.v_buf[0], rdna4_icbe_op2(&ctx, 128, ctx.t_u32, sbase, rdna4_icbe_const(&ctx, 21)));

    // Validate draw
    uint32_t k_ge1 = rdna4_icbe_op2(&ctx, 174, ctx.t_bool, kind, c1);
    uint32_t k_le4 = rdna4_icbe_op2(&ctx, 178, ctx.t_bool, kind, c4);
    uint32_t k_ok = rdna4_icbe_op2(&ctx, 167, ctx.t_bool, k_ge1, k_le4);
    uint32_t f_inv = rdna4_icbe_op2(&ctx, 199, ctx.t_u32, sflags, c2);
    uint32_t f_inv0 = rdna4_icbe_op2(&ctx, 170, ctx.t_bool, f_inv, c0);
    uint32_t inh_p_b = rdna4_icbe_op2(&ctx, 171, ctx.t_bool, inh_pipe, c0);
    uint32_t f_ps = rdna4_icbe_op2(&ctx, 199, ctx.t_u32, sflags, c1);
    uint32_t f_ps1 = rdna4_icbe_op2(&ctx, 171, ctx.t_bool, f_ps, c0);
    uint32_t pso_ok = rdna4_icbe_op2(&ctx, 166, ctx.t_bool, inh_p_b, f_ps1);
    uint32_t kind_m1 = rdna4_icbe_op2(&ctx, 130, ctx.t_u32, kind, c1);
    uint32_t kbit = rdna4_icbe_op2(&ctx, 196, ctx.t_u32, c1, kind_m1); // ShiftLeftLogical
    uint32_t c_perm = rdna4_icbe_op2(&ctx, 199, ctx.t_u32, cmd_types, kbit);
    uint32_t c_perm1 = rdna4_icbe_op2(&ctx, 171, ctx.t_bool, c_perm, c0);
    uint32_t nz_cnt = rdna4_icbe_op2(&ctx, 171, ctx.t_bool, scnt, c0);
    uint32_t nz_inst = rdna4_icbe_op2(&ctx, 171, ctx.t_bool, sinst, c0);
    uint32_t v01 = rdna4_icbe_op2(&ctx, 167, ctx.t_bool, k_ok, f_inv0);
    uint32_t v23 = rdna4_icbe_op2(&ctx, 167, ctx.t_bool, pso_ok, c_perm1);
    uint32_t v45 = rdna4_icbe_op2(&ctx, 167, ctx.t_bool, nz_cnt, nz_inst);
    uint32_t s_valid = rdna4_icbe_op2(&ctx, 167, ctx.t_bool, rdna4_icbe_op2(&ctx, 167, ctx.t_bool, v01, v23), v45);
    uint32_t is_tess = rdna4_icbe_op2(&ctx, 174, ctx.t_bool, kind, c3);
    uint32_t is_idx_g = rdna4_icbe_op2(&ctx, 170, ctx.t_bool, kind, c2);
    uint32_t s_d2 = rdna4_icbe_sel(&ctx, ctx.t_u32, is_idx_g, c0, sfirst);
    uint32_t s_tok_lo = rdna4_icbe_sel(&ctx, ctx.t_u32, inh_p_b, ptok_lo, stok_lo);
    uint32_t s_tok_hi = rdna4_icbe_sel(&ctx, ctx.t_u32, inh_p_b, ptok_hi, stok_hi);

    // Populate out_vs_params[k] scalar fields (dwords 0..7)
    rdna4_icbe_st(&ctx, ctx.v_buf[4], vpbase, kind);
    rdna4_icbe_st(&ctx, ctx.v_buf[4], rdna4_icbe_op2(&ctx, 128, ctx.t_u32, vpbase, c1), itype);
    rdna4_icbe_st(&ctx, ctx.v_buf[4], rdna4_icbe_op2(&ctx, 128, ctx.t_u32, vpbase, c2), sbvert);
    rdna4_icbe_st(&ctx, ctx.v_buf[4], rdna4_icbe_op2(&ctx, 128, ctx.t_u32, vpbase, c3), prim);
    rdna4_icbe_st(&ctx, ctx.v_buf[4], rdna4_icbe_op2(&ctx, 128, ctx.t_u32, vpbase, c4), ibda_lo);
    rdna4_icbe_st(&ctx, ctx.v_buf[4], rdna4_icbe_op2(&ctx, 128, ctx.t_u32, vpbase, rdna4_icbe_const(&ctx, 5)), ibda_hi);
    rdna4_icbe_st(&ctx, ctx.v_buf[4], rdna4_icbe_op2(&ctx, 128, ctx.t_u32, vpbase, rdna4_icbe_const(&ctx, 6)), c0);
    rdna4_icbe_st(&ctx, ctx.v_buf[4], rdna4_icbe_op2(&ctx, 128, ctx.t_u32, vpbase, rdna4_icbe_const(&ctx, 7)), c0);

    // Populate out_tess_params[k] header fields (dwords 0..15)
    uint32_t fb_nz = rdna4_icbe_op2(&ctx, 171, ctx.t_bool, rdna4_icbe_op2(&ctx, 198, ctx.t_u32, fbda_lo, fbda_hi), c0);
    uint32_t pb_nz = rdna4_icbe_op2(&ctx, 171, ctx.t_bool, rdna4_icbe_op2(&ctx, 198, ctx.t_u32, pbda_lo, pbda_hi), c0);
    uint32_t ib_nz = rdna4_icbe_op2(&ctx, 171, ctx.t_bool, rdna4_icbe_op2(&ctx, 198, ctx.t_u32, ibda_lo, ibda_hi), c0);
    uint32_t fsz_hi = rdna4_icbe_sel(&ctx, ctx.t_u32, fb_nz, c_64g_hi, c0);
    uint32_t psz_hi = rdna4_icbe_sel(&ctx, ctx.t_u32, pb_nz, c_64g_hi, c0);
    uint32_t isz_hi = rdna4_icbe_sel(&ctx, ctx.t_u32, ib_nz, c_64g_hi, c0);
    rdna4_icbe_st(&ctx, ctx.v_buf[5], tpbase, fbda_lo);
    rdna4_icbe_st(&ctx, ctx.v_buf[5], rdna4_icbe_op2(&ctx, 128, ctx.t_u32, tpbase, c1), fbda_hi);
    rdna4_icbe_st(&ctx, ctx.v_buf[5], rdna4_icbe_op2(&ctx, 128, ctx.t_u32, tpbase, c2), c0);
    rdna4_icbe_st(&ctx, ctx.v_buf[5], rdna4_icbe_op2(&ctx, 128, ctx.t_u32, tpbase, c3), fsz_hi);
    rdna4_icbe_st(&ctx, ctx.v_buf[5], rdna4_icbe_op2(&ctx, 128, ctx.t_u32, tpbase, c4), rdna4_icbe_sel(&ctx, ctx.t_u32, fb_nz, ststride, c0));
    rdna4_icbe_st(&ctx, ctx.v_buf[5], rdna4_icbe_op2(&ctx, 128, ctx.t_u32, tpbase, rdna4_icbe_const(&ctx, 5)), c0);
    rdna4_icbe_st(&ctx, ctx.v_buf[5], rdna4_icbe_op2(&ctx, 128, ctx.t_u32, tpbase, rdna4_icbe_const(&ctx, 6)), pbda_lo);
    rdna4_icbe_st(&ctx, ctx.v_buf[5], rdna4_icbe_op2(&ctx, 128, ctx.t_u32, tpbase, rdna4_icbe_const(&ctx, 7)), pbda_hi);
    rdna4_icbe_st(&ctx, ctx.v_buf[5], rdna4_icbe_op2(&ctx, 128, ctx.t_u32, tpbase, rdna4_icbe_const(&ctx, 8)), c0);
    rdna4_icbe_st(&ctx, ctx.v_buf[5], rdna4_icbe_op2(&ctx, 128, ctx.t_u32, tpbase, rdna4_icbe_const(&ctx, 9)), psz_hi);
    rdna4_icbe_st(&ctx, ctx.v_buf[5], rdna4_icbe_op2(&ctx, 128, ctx.t_u32, tpbase, rdna4_icbe_const(&ctx, 10)), ibda_lo);
    rdna4_icbe_st(&ctx, ctx.v_buf[5], rdna4_icbe_op2(&ctx, 128, ctx.t_u32, tpbase, rdna4_icbe_const(&ctx, 11)), ibda_hi);
    rdna4_icbe_st(&ctx, ctx.v_buf[5], rdna4_icbe_op2(&ctx, 128, ctx.t_u32, tpbase, rdna4_icbe_const(&ctx, 12)), c0);
    rdna4_icbe_st(&ctx, ctx.v_buf[5], rdna4_icbe_op2(&ctx, 128, ctx.t_u32, tpbase, rdna4_icbe_const(&ctx, 13)), isz_hi);
    rdna4_icbe_st(&ctx, ctx.v_buf[5], rdna4_icbe_op2(&ctx, 128, ctx.t_u32, tpbase, rdna4_icbe_const(&ctx, 14)), pf_scale);
    rdna4_icbe_st(&ctx, ctx.v_buf[5], rdna4_icbe_op2(&ctx, 128, ctx.t_u32, tpbase, rdna4_icbe_const(&ctx, 15)), spcp);

    // Zero tail [31] of vs_params and tables, and loop s = 0..30 to copy tables & vb_bdas
    uint32_t inh_buf_b = rdna4_icbe_op2(&ctx, 171, ctx.t_bool, inh_buf, c0);
    uint32_t s_var = rdna4_icbe_id(&ctx), s_next = rdna4_icbe_id(&ctx);
    rdna4_icbe_br(&ctx, b_hdr_v);

    rdna4_icbe_lbl(&ctx, b_hdr_v);
    rdna4_icbe_op(&ctx.code, 245, 7);
    rdna4_icbe_push(&ctx.code, ctx.t_u32); rdna4_icbe_push(&ctx.code, s_var);
    rdna4_icbe_push(&ctx.code, c0); rdna4_icbe_push(&ctx.code, b_slot);
    rdna4_icbe_push(&ctx.code, s_next); rdna4_icbe_push(&ctx.code, b_hdr_c);
    uint32_t s_lt31 = rdna4_icbe_op2(&ctx, 176, ctx.t_bool, s_var, c31);
    rdna4_icbe_lmerge(&ctx, b_hdr_d, b_hdr_c);
    uint32_t b_hdr_body = rdna4_icbe_id(&ctx);
    rdna4_icbe_bcond(&ctx, s_lt31, b_hdr_body, b_hdr_d);

    rdna4_icbe_lbl(&ctx, b_hdr_body);
    uint32_t s2 = rdna4_icbe_op2(&ctx, 196, ctx.t_u32, s_var, c1);
    // Slot vertex_bda at sbase + 24 + 2*s, fragment_bda at sbase + 88 + 2*s
    uint32_t sv_off = rdna4_icbe_op2(&ctx, 128, ctx.t_u32, sbase, rdna4_icbe_op2(&ctx, 128, ctx.t_u32, rdna4_icbe_const(&ctx, 24), s2));
    uint32_t sf_off = rdna4_icbe_op2(&ctx, 128, ctx.t_u32, sbase, rdna4_icbe_op2(&ctx, 128, ctx.t_u32, rdna4_icbe_const(&ctx, 88), s2));
    uint32_t sv_lo = rdna4_icbe_ld(&ctx, ctx.v_buf[0], sv_off);
    uint32_t sv_hi = rdna4_icbe_ld(&ctx, ctx.v_buf[0], rdna4_icbe_op2(&ctx, 128, ctx.t_u32, sv_off, c1));
    uint32_t sf_lo = rdna4_icbe_ld(&ctx, ctx.v_buf[0], sf_off);
    uint32_t sf_hi = rdna4_icbe_ld(&ctx, ctx.v_buf[0], rdna4_icbe_op2(&ctx, 128, ctx.t_u32, sf_off, c1));

    // Parent v_table at cfg + 656 + 2*s (addr), 656+64+2*s (len), 656+128+2*s (acc); f_table at +848
    uint32_t pva_off = rdna4_icbe_op2(&ctx, 128, ctx.t_u32, rdna4_icbe_const(&ctx, 656), s2);
    uint32_t pvl_off = rdna4_icbe_op2(&ctx, 128, ctx.t_u32, pva_off, c_64);
    uint32_t pvc_off = rdna4_icbe_op2(&ctx, 128, ctx.t_u32, pva_off, c_128);
    uint32_t pfa_off = rdna4_icbe_op2(&ctx, 128, ctx.t_u32, rdna4_icbe_const(&ctx, 848), s2);
    uint32_t pfl_off = rdna4_icbe_op2(&ctx, 128, ctx.t_u32, pfa_off, c_64);
    uint32_t pfc_off = rdna4_icbe_op2(&ctx, 128, ctx.t_u32, pfa_off, c_128);

    uint32_t pv_lo = rdna4_icbe_ld(&ctx, ctx.v_buf[1], pva_off);
    uint32_t pv_hi = rdna4_icbe_ld(&ctx, ctx.v_buf[1], rdna4_icbe_op2(&ctx, 128, ctx.t_u32, pva_off, c1));
    uint32_t pvl_lo = rdna4_icbe_ld(&ctx, ctx.v_buf[1], pvl_off);
    uint32_t pvl_hi = rdna4_icbe_ld(&ctx, ctx.v_buf[1], rdna4_icbe_op2(&ctx, 128, ctx.t_u32, pvl_off, c1));
    uint32_t pvc_lo = rdna4_icbe_ld(&ctx, ctx.v_buf[1], pvc_off);
    uint32_t pvc_hi = rdna4_icbe_ld(&ctx, ctx.v_buf[1], rdna4_icbe_op2(&ctx, 128, ctx.t_u32, pvc_off, c1));

    uint32_t pf_lo = rdna4_icbe_ld(&ctx, ctx.v_buf[1], pfa_off);
    uint32_t pf_hi = rdna4_icbe_ld(&ctx, ctx.v_buf[1], rdna4_icbe_op2(&ctx, 128, ctx.t_u32, pfa_off, c1));
    uint32_t pfl_lo = rdna4_icbe_ld(&ctx, ctx.v_buf[1], pfl_off);
    uint32_t pfl_hi = rdna4_icbe_ld(&ctx, ctx.v_buf[1], rdna4_icbe_op2(&ctx, 128, ctx.t_u32, pfl_off, c1));
    uint32_t pfc_lo = rdna4_icbe_ld(&ctx, ctx.v_buf[1], pfc_off);
    uint32_t pfc_hi = rdna4_icbe_ld(&ctx, ctx.v_buf[1], rdna4_icbe_op2(&ctx, 128, ctx.t_u32, pfc_off, c1));

    uint32_t sv_nz = rdna4_icbe_op2(&ctx, 171, ctx.t_bool, rdna4_icbe_op2(&ctx, 198, ctx.t_u32, sv_lo, sv_hi), c0);
    uint32_t sf_nz = rdna4_icbe_op2(&ctx, 171, ctx.t_bool, rdna4_icbe_op2(&ctx, 198, ctx.t_u32, sf_lo, sf_hi), c0);

    uint32_t ev_lo = rdna4_icbe_sel(&ctx, ctx.t_u32, inh_buf_b, pv_lo, sv_lo);
    uint32_t ev_hi = rdna4_icbe_sel(&ctx, ctx.t_u32, inh_buf_b, pv_hi, sv_hi);
    uint32_t evl_lo = rdna4_icbe_sel(&ctx, ctx.t_u32, inh_buf_b, pvl_lo, c0);
    uint32_t evl_hi = rdna4_icbe_sel(&ctx, ctx.t_u32, inh_buf_b, pvl_hi, rdna4_icbe_sel(&ctx, ctx.t_u32, sv_nz, c_64g_hi, c0));
    uint32_t evc_lo = rdna4_icbe_sel(&ctx, ctx.t_u32, inh_buf_b, pvc_lo, rdna4_icbe_sel(&ctx, ctx.t_u32, sv_nz, c3, c0));
    uint32_t evc_hi = rdna4_icbe_sel(&ctx, ctx.t_u32, inh_buf_b, pvc_hi, c0);

    uint32_t ef_lo = rdna4_icbe_sel(&ctx, ctx.t_u32, inh_buf_b, pf_lo, sf_lo);
    uint32_t ef_hi = rdna4_icbe_sel(&ctx, ctx.t_u32, inh_buf_b, pf_hi, sf_hi);
    uint32_t efl_lo = rdna4_icbe_sel(&ctx, ctx.t_u32, inh_buf_b, pfl_lo, c0);
    uint32_t efl_hi = rdna4_icbe_sel(&ctx, ctx.t_u32, inh_buf_b, pfl_hi, rdna4_icbe_sel(&ctx, ctx.t_u32, sf_nz, c_64g_hi, c0));
    uint32_t efc_lo = rdna4_icbe_sel(&ctx, ctx.t_u32, inh_buf_b, pfc_lo, rdna4_icbe_sel(&ctx, ctx.t_u32, sf_nz, c3, c0));
    uint32_t efc_hi = rdna4_icbe_sel(&ctx, ctx.t_u32, inh_buf_b, pfc_hi, c0);

    // Write out_v_table (v_buf[6]) and out_f_table (v_buf[7])
    uint32_t ta_off = rdna4_icbe_op2(&ctx, 128, ctx.t_u32, tbbase, s2);
    uint32_t tl_off = rdna4_icbe_op2(&ctx, 128, ctx.t_u32, ta_off, c_64);
    uint32_t tc_off = rdna4_icbe_op2(&ctx, 128, ctx.t_u32, ta_off, c_128);
    rdna4_icbe_st(&ctx, ctx.v_buf[6], ta_off, ev_lo);
    rdna4_icbe_st(&ctx, ctx.v_buf[6], rdna4_icbe_op2(&ctx, 128, ctx.t_u32, ta_off, c1), ev_hi);
    rdna4_icbe_st(&ctx, ctx.v_buf[6], tl_off, evl_lo);
    rdna4_icbe_st(&ctx, ctx.v_buf[6], rdna4_icbe_op2(&ctx, 128, ctx.t_u32, tl_off, c1), evl_hi);
    rdna4_icbe_st(&ctx, ctx.v_buf[6], tc_off, evc_lo);
    rdna4_icbe_st(&ctx, ctx.v_buf[6], rdna4_icbe_op2(&ctx, 128, ctx.t_u32, tc_off, c1), evc_hi);
    rdna4_icbe_st(&ctx, ctx.v_buf[7], ta_off, ef_lo);
    rdna4_icbe_st(&ctx, ctx.v_buf[7], rdna4_icbe_op2(&ctx, 128, ctx.t_u32, ta_off, c1), ef_hi);
    rdna4_icbe_st(&ctx, ctx.v_buf[7], tl_off, efl_lo);
    rdna4_icbe_st(&ctx, ctx.v_buf[7], rdna4_icbe_op2(&ctx, 128, ctx.t_u32, tl_off, c1), efl_hi);
    rdna4_icbe_st(&ctx, ctx.v_buf[7], tc_off, efc_lo);
    rdna4_icbe_st(&ctx, ctx.v_buf[7], rdna4_icbe_op2(&ctx, 128, ctx.t_u32, tc_off, c1), efc_hi);

    // Parent VB BDAs at cfg + 1040 + 2*s, parent VB sizes at cfg + 1104 + 2*s
    uint32_t pvb_off = rdna4_icbe_op2(&ctx, 128, ctx.t_u32, rdna4_icbe_const(&ctx, 1040), s2);
    uint32_t pvbs_off = rdna4_icbe_op2(&ctx, 128, ctx.t_u32, rdna4_icbe_const(&ctx, 1104), s2);
    uint32_t pvb_lo = rdna4_icbe_ld(&ctx, ctx.v_buf[1], pvb_off);
    uint32_t pvb_hi = rdna4_icbe_ld(&ctx, ctx.v_buf[1], rdna4_icbe_op2(&ctx, 128, ctx.t_u32, pvb_off, c1));
    uint32_t pvbs_lo = rdna4_icbe_ld(&ctx, ctx.v_buf[1], pvbs_off);
    uint32_t pvbs_hi = rdna4_icbe_ld(&ctx, ctx.v_buf[1], rdna4_icbe_op2(&ctx, 128, ctx.t_u32, pvbs_off, c1));
    uint32_t ovb_lo = rdna4_icbe_sel(&ctx, ctx.t_u32, inh_buf_b, pvb_lo, sv_lo);
    uint32_t ovb_hi = rdna4_icbe_sel(&ctx, ctx.t_u32, inh_buf_b, pvb_hi, sv_hi);
    uint32_t ovbs_lo = rdna4_icbe_sel(&ctx, ctx.t_u32, inh_buf_b, pvbs_lo, c0);
    uint32_t ovbs_hi = rdna4_icbe_sel(&ctx, ctx.t_u32, inh_buf_b, pvbs_hi, rdna4_icbe_sel(&ctx, ctx.t_u32, sv_nz, c_64g_hi, c0));

    uint32_t vpv_off = rdna4_icbe_op2(&ctx, 128, ctx.t_u32, vpbase, rdna4_icbe_op2(&ctx, 128, ctx.t_u32, rdna4_icbe_const(&ctx, 8), s2));
    rdna4_icbe_st(&ctx, ctx.v_buf[4], vpv_off, ovb_lo);
    rdna4_icbe_st(&ctx, ctx.v_buf[4], rdna4_icbe_op2(&ctx, 128, ctx.t_u32, vpv_off, c1), ovb_hi);

    uint32_t tpv_off = rdna4_icbe_op2(&ctx, 128, ctx.t_u32, tpbase, rdna4_icbe_op2(&ctx, 128, ctx.t_u32, c16, s2));
    uint32_t tpvs_off = rdna4_icbe_op2(&ctx, 128, ctx.t_u32, tpbase, rdna4_icbe_op2(&ctx, 128, ctx.t_u32, c_78, s2));
    rdna4_icbe_st(&ctx, ctx.v_buf[5], tpv_off, ovb_lo);
    rdna4_icbe_st(&ctx, ctx.v_buf[5], rdna4_icbe_op2(&ctx, 128, ctx.t_u32, tpv_off, c1), ovb_hi);
    rdna4_icbe_st(&ctx, ctx.v_buf[5], tpvs_off, ovbs_lo);
    rdna4_icbe_st(&ctx, ctx.v_buf[5], rdna4_icbe_op2(&ctx, 128, ctx.t_u32, tpvs_off, c1), ovbs_hi);

    rdna4_icbe_br(&ctx, b_hdr_c);
    rdna4_icbe_lbl(&ctx, b_hdr_c);
    rdna4_icbe_op(&ctx.code, 128, 5);
    rdna4_icbe_push(&ctx.code, ctx.t_u32); rdna4_icbe_push(&ctx.code, s_next); rdna4_icbe_push(&ctx.code, s_var); rdna4_icbe_push(&ctx.code, c1);
    rdna4_icbe_br(&ctx, b_hdr_v);

    rdna4_icbe_lbl(&ctx, b_hdr_d);
    /* Zero [31] slots of out_vs_params, out_v_table, and out_f_table. */
    uint32_t vp31 = rdna4_icbe_op2(&ctx, 128, ctx.t_u32, vpbase, rdna4_icbe_const(&ctx, 70));
    rdna4_icbe_st(&ctx, ctx.v_buf[4], vp31, c0);
    rdna4_icbe_st(&ctx, ctx.v_buf[4], rdna4_icbe_op2(&ctx, 128, ctx.t_u32, vp31, c1), c0);
    uint32_t t31_a = rdna4_icbe_op2(&ctx, 128, ctx.t_u32, tbbase, rdna4_icbe_const(&ctx, 62));
    uint32_t t31_l = rdna4_icbe_op2(&ctx, 128, ctx.t_u32, t31_a, c_64);
    uint32_t t31_c = rdna4_icbe_op2(&ctx, 128, ctx.t_u32, t31_a, c_128);
    for (int tv = 6; tv <= 7; tv++) {
        rdna4_icbe_st(&ctx, ctx.v_buf[tv], t31_a, c0);
        rdna4_icbe_st(&ctx, ctx.v_buf[tv], rdna4_icbe_op2(&ctx, 128, ctx.t_u32, t31_a, c1), c0);
        rdna4_icbe_st(&ctx, ctx.v_buf[tv], t31_l, c0);
        rdna4_icbe_st(&ctx, ctx.v_buf[tv], rdna4_icbe_op2(&ctx, 128, ctx.t_u32, t31_l, c1), c0);
        rdna4_icbe_st(&ctx, ctx.v_buf[tv], t31_c, c0);
        rdna4_icbe_st(&ctx, ctx.v_buf[tv], rdna4_icbe_op2(&ctx, 128, ctx.t_u32, t31_c, c1), c0);
    }
    rdna4_icbe_br(&ctx, b_emit);

    // b_emit: phi results across [b_act -> b_emit] vs [b_hdr_d -> b_emit], then emit num_buckets draw commands
    rdna4_icbe_lbl(&ctx, b_emit);
    uint32_t c_false_id = rdna4_icbe_id(&ctx);
    rdna4_icbe_op(&ctx.types, 42, 3); rdna4_icbe_push(&ctx.types, ctx.t_bool); rdna4_icbe_push(&ctx.types, c_false_id);
    uint32_t p_valid = rdna4_icbe_phi1(&ctx, ctx.t_bool, s_valid, b_hdr_d, c_false_id, b_act);
    uint32_t p_itess = rdna4_icbe_phi1(&ctx, ctx.t_bool, is_tess, b_hdr_d, c_false_id, b_act);
    uint32_t p_prim = rdna4_icbe_phi1(&ctx, ctx.t_u32, prim, b_hdr_d, c0, b_act);
    uint32_t p_tlo = rdna4_icbe_phi1(&ctx, ctx.t_u32, s_tok_lo, b_hdr_d, c0, b_act);
    uint32_t p_thi = rdna4_icbe_phi1(&ctx, ctx.t_u32, s_tok_hi, b_hdr_d, c0, b_act);
    uint32_t p_d0 = rdna4_icbe_phi1(&ctx, ctx.t_u32, scnt, b_hdr_d, c0, b_act);
    uint32_t p_d1 = rdna4_icbe_phi1(&ctx, ctx.t_u32, sinst, b_hdr_d, c0, b_act);
    uint32_t p_d2 = rdna4_icbe_phi1(&ctx, ctx.t_u32, s_d2, b_hdr_d, c0, b_act);
    uint32_t p_d3 = rdna4_icbe_phi1(&ctx, ctx.t_u32, sbinst, b_hdr_d, c0, b_act);

    uint32_t b_var = rdna4_icbe_id(&ctx), b_next = rdna4_icbe_id(&ctx);
    rdna4_icbe_br(&ctx, b_dl_h);
    rdna4_icbe_lbl(&ctx, b_dl_h);
    rdna4_icbe_op(&ctx.code, 245, 7);
    rdna4_icbe_push(&ctx.code, ctx.t_u32); rdna4_icbe_push(&ctx.code, b_var);
    rdna4_icbe_push(&ctx.code, c0); rdna4_icbe_push(&ctx.code, b_emit);
    rdna4_icbe_push(&ctx.code, b_next); rdna4_icbe_push(&ctx.code, b_dl_c);
    uint32_t b_lt = rdna4_icbe_op2(&ctx, 176, ctx.t_bool, b_var, num_bkt);
    rdna4_icbe_lmerge(&ctx, b_dl_d, b_dl_c);
    rdna4_icbe_bcond(&ctx, b_lt, b_dl_b, b_dl_d);

    rdna4_icbe_lbl(&ctx, b_dl_b);
    uint32_t b_off = rdna4_icbe_op2(&ctx, 128, ctx.t_u32, c16, rdna4_icbe_op2(&ctx, 196, ctx.t_u32, b_var, c2));
    uint32_t bt_lo = rdna4_icbe_ld(&ctx, ctx.v_buf[1], b_off);
    uint32_t bt_hi = rdna4_icbe_ld(&ctx, ctx.v_buf[1], rdna4_icbe_op2(&ctx, 128, ctx.t_u32, b_off, c1));
    uint32_t bt_tess = rdna4_icbe_ld(&ctx, ctx.v_buf[1], rdna4_icbe_op2(&ctx, 128, ctx.t_u32, b_off, c2));
    uint32_t bt_prim = rdna4_icbe_ld(&ctx, ctx.v_buf[1], rdna4_icbe_op2(&ctx, 128, ctx.t_u32, b_off, c3));

    uint32_t eq_lo = rdna4_icbe_op2(&ctx, 170, ctx.t_bool, p_tlo, bt_lo);
    uint32_t eq_hi = rdna4_icbe_op2(&ctx, 170, ctx.t_bool, p_thi, bt_hi);
    uint32_t eq_tok = rdna4_icbe_op2(&ctx, 167, ctx.t_bool, eq_lo, eq_hi);
    uint32_t bt_tess_b = rdna4_icbe_op2(&ctx, 171, ctx.t_bool, bt_tess, c0);
    uint32_t not_itess = rdna4_icbe_op1(&ctx, 168, ctx.t_bool, p_itess); // LogicalNot
    uint32_t eq_prim = rdna4_icbe_op2(&ctx, 170, ctx.t_bool, p_prim, bt_prim);
    uint32_t match_g = rdna4_icbe_op2(&ctx, 167, ctx.t_bool, not_itess, eq_prim);
    uint32_t match_k = rdna4_icbe_sel(&ctx, ctx.t_bool, bt_tess_b, p_itess, match_g);
    uint32_t m0 = rdna4_icbe_op2(&ctx, 167, ctx.t_bool, p_valid, eq_tok);
    uint32_t matched = rdna4_icbe_op2(&ctx, 167, ctx.t_bool, m0, match_k);

    uint32_t draw_idx = rdna4_icbe_op2(&ctx, 128, ctx.t_u32, rdna4_icbe_op2(&ctx, 132, ctx.t_u32, k, num_bkt), b_var);
    uint32_t dbase = rdna4_icbe_op2(&ctx, 196, ctx.t_u32, draw_idx, c2); // * 4 dwords
    rdna4_icbe_st(&ctx, ctx.v_buf[3], dbase, rdna4_icbe_sel(&ctx, ctx.t_u32, matched, p_d0, c0));
    rdna4_icbe_st(&ctx, ctx.v_buf[3], rdna4_icbe_op2(&ctx, 128, ctx.t_u32, dbase, c1), rdna4_icbe_sel(&ctx, ctx.t_u32, matched, p_d1, c0));
    rdna4_icbe_st(&ctx, ctx.v_buf[3], rdna4_icbe_op2(&ctx, 128, ctx.t_u32, dbase, c2), rdna4_icbe_sel(&ctx, ctx.t_u32, matched, p_d2, c0));
    rdna4_icbe_st(&ctx, ctx.v_buf[3], rdna4_icbe_op2(&ctx, 128, ctx.t_u32, dbase, c3), rdna4_icbe_sel(&ctx, ctx.t_u32, matched, p_d3, c0));

    rdna4_icbe_br(&ctx, b_dl_c);
    rdna4_icbe_lbl(&ctx, b_dl_c);
    rdna4_icbe_op(&ctx.code, 128, 5);
    rdna4_icbe_push(&ctx.code, ctx.t_u32); rdna4_icbe_push(&ctx.code, b_next); rdna4_icbe_push(&ctx.code, b_var); rdna4_icbe_push(&ctx.code, c1);
    rdna4_icbe_br(&ctx, b_dl_h);

    rdna4_icbe_lbl(&ctx, b_dl_d);
    rdna4_icbe_br(&ctx, b_ret);
    rdna4_icbe_lbl(&ctx, b_ret);
    rdna4_icbe_op(&ctx.code, 253, 1);
    rdna4_icbe_op(&ctx.code, 56, 1);

    rdna4_icbe_buf_t spv = {0};
    rdna4_icbe_push(&spv, 0x07230203u);
    rdna4_icbe_push(&spv, 0x00010500u);
    rdna4_icbe_push(&spv, 0);
    rdna4_icbe_push(&spv, ctx.next_id);
    rdna4_icbe_push(&spv, 0);
    rdna4_icbe_op(&spv, 17, 2); rdna4_icbe_push(&spv, 1); // Capability Shader
    rdna4_icbe_op(&spv, 14, 3); rdna4_icbe_push(&spv, 0); rdna4_icbe_push(&spv, 1); // Logical GLSL450
    rdna4_icbe_op(&spv, 15, (uint16_t)(3 + 2 + 9));
    rdna4_icbe_push(&spv, 5); // GLCompute
    rdna4_icbe_push(&spv, fn_main);
    rdna4_icbe_str(&spv, "main");
    rdna4_icbe_push(&spv, ctx.v_gid);
    for (int i = 0; i < 8; i++) rdna4_icbe_push(&spv, ctx.v_buf[i]);
    rdna4_icbe_op(&spv, 16, 6);
    rdna4_icbe_push(&spv, fn_main); rdna4_icbe_push(&spv, 17); // LocalSize
    rdna4_icbe_push(&spv, 64); rdna4_icbe_push(&spv, 1); rdna4_icbe_push(&spv, 1);
    for (size_t i = 0; i < ctx.dec.len; i++) rdna4_icbe_push(&spv, ctx.dec.w[i]);
    for (size_t i = 0; i < ctx.types.len; i++) rdna4_icbe_push(&spv, ctx.types.w[i]);
    for (size_t i = 0; i < ctx.vars.len; i++) rdna4_icbe_push(&spv, ctx.vars.w[i]);
    for (size_t i = 0; i < ctx.code.len; i++) rdna4_icbe_push(&spv, ctx.code.w[i]);

    free(ctx.dec.w); free(ctx.types.w); free(ctx.vars.w); free(ctx.code.w);
    if (ctx.dec.oom || ctx.types.oom || ctx.vars.oom || ctx.code.oom || spv.oom) {
        free(spv.w); return 0;
    }
    *out_words = spv.w;
    *out_nwords = spv.len;
    return 1;
}

#endif
