// Offline SPIR-V and ABI layout self-check for rdna4_icb_abi.h + rdna4_icb_expand.h.
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#include "rdna4_spvrefl.h"
#include "rdna4_tessellation.h"
#include "rdna4_icb_expand.h"

// Cross-check rdna4_tess_draw_params_t dword offsets hardcoded in rdna4_icb_expand.h!
_Static_assert(offsetof(rdna4_tess_draw_params_t, factor_bda) == 0, "tess draw factor_bda offset");
_Static_assert(offsetof(rdna4_tess_draw_params_t, factor_size) == 8, "tess draw factor_size offset");
_Static_assert(offsetof(rdna4_tess_draw_params_t, factor_instance_stride) == 16, "tess draw stride offset");
_Static_assert(offsetof(rdna4_tess_draw_params_t, patch_idx_bda) == 24, "tess draw patch_idx_bda offset");
_Static_assert(offsetof(rdna4_tess_draw_params_t, patch_idx_size) == 32, "tess draw patch_idx_size offset");
_Static_assert(offsetof(rdna4_tess_draw_params_t, cp_idx_bda) == 40, "tess draw cp_idx_bda offset");
_Static_assert(offsetof(rdna4_tess_draw_params_t, cp_idx_size) == 48, "tess draw cp_idx_size offset");
_Static_assert(offsetof(rdna4_tess_draw_params_t, factor_scale) == 56, "tess draw factor_scale offset");
_Static_assert(offsetof(rdna4_tess_draw_params_t, num_patch_cp) == 60, "tess draw num_patch_cp offset");
_Static_assert(offsetof(rdna4_tess_draw_params_t, vb_bda) == 64, "tess draw vb_bda offset");
_Static_assert(offsetof(rdna4_tess_draw_params_t, vb_size) == 312, "tess draw vb_size offset");
_Static_assert(RDNA4_ICB_TESS_PARAMS_STRIDE >= sizeof(rdna4_tess_draw_params_t), "tess params stride fits struct");
_Static_assert(RDNA4_ICB_TESS_PARAMS_STRIDE % 256u == 0u, "tess params stride 256B aligned");
_Static_assert(RDNA4_ICB_VS_PARAMS_STRIDE >= sizeof(rdna4_icb_vs_params_t), "vs params stride fits struct");
_Static_assert(RDNA4_ICB_VS_PARAMS_STRIDE % 256u == 0u, "vs params stride 256B aligned");
_Static_assert(RDNA4_ICB_TABLE_V2_STRIDE == sizeof(rdna4_buffer_table), "table v2 stride matches 768B");

int main(void) {
    uint32_t *words = NULL;
    size_t nwords = 0;
    assert(rdna4_icb_emit_expand_spv(&words, &nwords) == 1);
    assert(words != NULL && nwords > 20);
    assert(words[0] == 0x07230203u);
    assert(words[1] == 0x00010500u);

    RDNA4sMod mod;
    char why[256] = "";
    assert(rdna4s_parse(words, nwords, &mod, why, sizeof why) == 0);
    assert(mod.has_ep && mod.ep_model == 5);
    int has_local_64 = 0;
    for (size_t p = 5; p < nwords; p += words[p] >> 16) {
        if ((words[p] & 0xffffu) == 16 && (words[p] >> 16) == 6 &&
            words[p + 1] == mod.ep_id && words[p + 2] == 17 &&
            words[p + 3] == 64 && words[p + 4] == 1 && words[p + 5] == 1)
            has_local_64 = 1;
    }
    assert(has_local_64);

    RDNA4sRefl refl;
    assert(rdna4s_reflect(&mod, 0, &refl) == 0);
    for (uint32_t b = 0; b < 8; b++) {
        assert(refl.bind[0][b].used == 1);
        assert(refl.bind[0][b].type == VK_DESCRIPTOR_TYPE_STORAGE_BUFFER);
        assert(refl.bind[0][b].count == 1);
    }
    rdna4s_free(&mod);
    free(words);
    printf("ICB_SPV_TEST_PASS words=%zu bindings=8 local=64x1x1\n", nwords);
    return 0;
}
