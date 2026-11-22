#include "rdna4_null_shape.h"
#include <assert.h>
#include <stdio.h>

int main(void) {
    assert(rdna4ns_table_abi_compatible(9, 9));
    assert(rdna4ns_table_abi_compatible(10, 10));
    assert(!rdna4ns_table_abi_compatible(9, 10));
    assert(!rdna4ns_table_abi_compatible(10, 9));
    assert(rdna4ns_table_abi_compatible(0, 9));
    assert(rdna4ns_table_abi_compatible(10, 0));
    assert(!rdna4ns_table_abi_compatible(UINT64_C(0x100000009), 9));

    const uint32_t vertex_words[] = {0x011, 0x101, 0x214};
    const uint32_t fragment_words[] = {0x001, 0x101, 0x224};
    rdna4ns_registry vertex, fragment, shared;
    assert(rdna4ns_registry_build(vertex_words, 3, &vertex) == RDNA4NS_OK);
    assert(rdna4ns_registry_build(fragment_words, 3, &fragment) == RDNA4NS_OK);
    assert(rdna4ns_registry_merge(&vertex, &fragment, &shared) == RDNA4NS_OK);
    uint32_t remap[64] = {0}; uint8_t access[64] = {0};
    assert(rdna4ns_registry_remap(&shared, vertex_words, 3, remap, access) == RDNA4NS_OK);
    assert(remap[0] == 1 && remap[1] == 0 && remap[2] == 0);
    assert(access[0] == RDNA4NS_ACCESS_SAMPLED && access[1] == RDNA4NS_ACCESS_STORAGE &&
           access[2] == RDNA4NS_ACCESS_UNIFORM_TEXEL);
    for (unsigned i = 0; i < 3; ++i) remap[i] = fragment_words[i];
    assert(rdna4ns_registry_remap(&shared, remap, 3, remap, access) == RDNA4NS_OK);
    assert(remap[0] == 0 && remap[1] == 0 && remap[2] == 1);
    assert(rdna4ns_registry_remap(&vertex, fragment_words, 3, remap, access) == RDNA4NS_INVALID);
    const uint32_t bad[] = {0x101, 0x001};
    assert(rdna4ns_registry_remap(&shared, bad, 2, remap, access) == RDNA4NS_UNSORTED);
    assert(rdna4ns_registry_remap(&shared, NULL, 0, NULL, NULL) == RDNA4NS_OK);
    uint32_t descriptor = UINT32_MAX;
    assert(rdna4ns_texel_null_index(1, 0, &descriptor) == RDNA4NS_OK &&
           descriptor == 1);
    assert(rdna4ns_texel_null_index(65, 2, &descriptor) == RDNA4NS_OK &&
           descriptor == 67);
    assert(rdna4ns_texel_null_index(0, 0, &descriptor) == RDNA4NS_INVALID);
    assert(rdna4ns_texel_null_index(UINT32_MAX, 1, &descriptor) ==
           RDNA4NS_OVERFLOW);
    assert(rdna4ns_texel_descriptor_count(5, 3, &descriptor) == RDNA4NS_OK &&
           descriptor == 8);
    assert(rdna4ns_texel_descriptor_count(UINT32_MAX, 1, &descriptor) ==
           RDNA4NS_OVERFLOW);
    puts("NULL_REMAP_PASS cross-stage image and texel ordinals, in-place, missing and malformed");
    return 0;
}
