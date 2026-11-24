/* SPDX-License-Identifier: LGPL-3.0-or-later */
/* garnetxlate C ABI header (ABI version 3 + Stage 1/2 optional extensions). */

#ifndef GARNETXLATE_H
#define GARNETXLATE_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define GARNETX_ABI_VERSION 3u

#define GARNETX_RC_OK       0
#define GARNETX_RC_REFUSED  1
#define GARNETX_RC_PANIC    2
#define GARNETX_RC_BAD_ARGS 3

typedef struct RDNA4xDep {
    const uint8_t *symbol;
    size_t symbol_len;
    const uint8_t *ll;
    size_t ll_len;
} RDNA4xDep;

typedef struct RDNA4xDynamicLibrary {
    const uint8_t *name;
    size_t name_len;
    const uint8_t *ll;
    size_t ll_len;
} RDNA4xDynamicLibrary;

typedef struct RDNA4xBackendCapabilities {
    uint32_t struct_size;
    uint32_t version;
    uint32_t flags;
    uint32_t subgroup_size;
    uint32_t subgroup_supported_stages;
    uint32_t subgroup_supported_operations;
} RDNA4xBackendCapabilities;

typedef struct GarnetxIcbVertexAttr {
    uint32_t location;
    uint32_t buffer_index;
    uint32_t offset;
    uint32_t vk_format;
    uint32_t step_function;
    uint32_t step_rate;
} GarnetxIcbVertexAttr;

uint32_t garnetx_abi_version(void);
void garnetx_free(uint8_t *ptr, size_t len);

int garnetx_translate(
    const uint8_t *ll,
    size_t ll_len,
    const uint32_t *local_xyz,
    uint8_t **spv,
    size_t *spv_len,
    uint8_t **meta,
    size_t *meta_len,
    char *err,
    size_t err_len);

int garnetx_translate_linked(
    const uint8_t *ll,
    size_t ll_len,
    const uint32_t *local_xyz,
    const RDNA4xDep *deps,
    size_t ndeps,
    uint8_t **spv,
    size_t *spv_len,
    uint8_t **meta,
    size_t *meta_len,
    char *err,
    size_t err_len);

int garnetx_lower_argument_resources(
    const uint8_t *input_spv,
    size_t input_spv_len,
    const uint8_t *input_meta,
    size_t input_meta_len,
    uint8_t **spv,
    size_t *spv_len,
    uint8_t **meta,
    size_t *meta_len,
    char *err,
    size_t err_len);

int garnetx_translate_with_capabilities(
    const uint8_t *ll,
    size_t ll_len,
    const uint32_t *local_xyz,
    uint8_t **spv,
    size_t *spv_len,
    uint8_t **meta,
    size_t *meta_len,
    char *err,
    size_t err_len,
    const RDNA4xBackendCapabilities *capabilities);

int garnetx_translate_linked_with_capabilities(
    const uint8_t *ll,
    size_t ll_len,
    const uint32_t *local_xyz,
    const RDNA4xDep *deps,
    size_t ndeps,
    uint8_t **spv,
    size_t *spv_len,
    uint8_t **meta,
    size_t *meta_len,
    char *err,
    size_t err_len,
    const RDNA4xBackendCapabilities *capabilities);

int garnetx_lower_argument_resources_with_capabilities(
    const uint8_t *input_spv,
    size_t input_spv_len,
    const uint8_t *input_meta,
    size_t input_meta_len,
    uint8_t **spv,
    size_t *spv_len,
    uint8_t **meta,
    size_t *meta_len,
    char *err,
    size_t err_len,
    const RDNA4xBackendCapabilities *capabilities);

int garnetx_lower_icb_stage(
    const uint8_t *spv,
    size_t spv_len,
    uint32_t stage_kind,
    uint32_t stage_set,
    const GarnetxIcbVertexAttr *vattrs,
    size_t nvattrs,
    uint8_t **out_spv,
    size_t *out_spv_len,
    uint32_t *out_table_binding,
    uint32_t *out_vs_params_binding,
    char *err,
    size_t err_len);

/* Metal 3 Stage 2/3 extensions: MTLDynamicLibrary, MTLVisibleFunctionTable,
 * Object/Mesh stage detection, and hardware Ray Query + MTLIntersectionFunctionTable.
 * All additions preserve GARNETX_ABI_VERSION == 3u and every existing symbol. */

#define GARNETX_STAGE_VERTEX   0u
#define GARNETX_STAGE_FRAGMENT 1u
#define GARNETX_STAGE_KERNEL   2u
#define GARNETX_STAGE_OBJECT   3u
#define GARNETX_STAGE_MESH     4u

#define GARNETX_IFT_KIND_BOUNDING_BOX     0u
#define GARNETX_IFT_KIND_TRIANGLE_ANY_HIT 1u

typedef struct RDNA4xIntersectionFunction {
    const uint8_t *symbol;
    size_t symbol_len;
    const uint8_t *ll;
    size_t ll_len;
    uint32_t kind; /* GARNETX_IFT_KIND_* */
} RDNA4xIntersectionFunction;

int garnetx_list_exported_symbols(
    const uint8_t *ll,
    size_t ll_len,
    uint8_t **out_json,
    size_t *out_json_len,
    char *err,
    size_t err_len);

int garnetx_detect_stage(
    const uint8_t *ll,
    size_t ll_len,
    uint32_t *out_stage_kind,
    char *err,
    size_t err_len);

int garnetx_translate_linked_libraries(
    const uint8_t *ll,
    size_t ll_len,
    const uint32_t *local_xyz,
    const RDNA4xDep *deps,
    size_t ndeps,
    const RDNA4xDynamicLibrary *dynlibs,
    size_t ndynlibs,
    const RDNA4xDep *visible_fns,
    size_t nvisible_fns,
    const RDNA4xBackendCapabilities *capabilities,
    uint8_t **spv,
    size_t *spv_len,
    uint8_t **meta,
    size_t *meta_len,
    char *err,
    size_t err_len);

int garnetx_translate_stage3(
    const uint8_t *ll,
    size_t ll_len,
    const uint32_t *local_xyz,
    const RDNA4xDep *deps,
    size_t ndeps,
    const RDNA4xDynamicLibrary *dynlibs,
    size_t ndynlibs,
    const RDNA4xDep *visible_fns,
    size_t nvisible_fns,
    const RDNA4xIntersectionFunction *intersection_fns,
    size_t nintersection_fns,
    const RDNA4xBackendCapabilities *capabilities,
    uint8_t **spv,
    size_t *spv_len,
    uint8_t **meta,
    size_t *meta_len,
    char *err,
    size_t err_len);

#ifdef __cplusplus
}
#endif

#endif /* GARNETXLATE_H */
