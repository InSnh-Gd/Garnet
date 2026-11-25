// ABI 3 translation capabilities; Metal 2.2 LOD queries are an optional extension.
#ifndef RDNA4_QUERY_CAPS_H
#define RDNA4_QUERY_CAPS_H
#include <stdint.h>
#define RDNA4X_QUERY_POLICY_REVISION 1u
#define RDNA4X_CAPS_VERSION 1u
#define RDNA4X_CAP_QUERY_LANGUAGE 1u
#define RDNA4X_CAP_QUERY_DEVICE 2u
typedef struct {
    uint32_t struct_size, version, flags, subgroup_size;
    uint32_t subgroup_supported_stages, subgroup_supported_operations;
} rdna4x_backend_capabilities;
_Static_assert(sizeof(rdna4x_backend_capabilities) == 24, "ABI 3 capability layout");
static inline rdna4x_backend_capabilities rdna4x_query_caps(int language, int device,
    uint32_t size, uint32_t stages, uint32_t operations) {
    return (rdna4x_backend_capabilities){24u, RDNA4X_CAPS_VERSION,
        (language ? RDNA4X_CAP_QUERY_LANGUAGE : 0u) | (device ? RDNA4X_CAP_QUERY_DEVICE : 0u),
        size, stages, operations};
}
static inline int rdna4x_query_caps_valid(const rdna4x_backend_capabilities *c) {
    return c && c->struct_size == sizeof *c && c->version == RDNA4X_CAPS_VERSION && !(c->flags & ~3u);
}
static inline int rdna4x_query_supported(const rdna4x_backend_capabilities *c) {
    return rdna4x_query_caps_valid(c) && c->flags == 3u;
}
// Fragment quad operations do not require subgroupQuadOperationsInAllStages.
static inline int rdna4x_query_uses_quad(const rdna4x_backend_capabilities *c) {
    return rdna4x_query_supported(c) && c->subgroup_size >= 4u &&
        (c->subgroup_supported_stages & 0x10u) && (c->subgroup_supported_operations & 0x80u);
}
#endif
