#include <assert.h>
#include <stdio.h>
#include "garnet_query_caps.h"
int main(void) {
    rdna4x_backend_capabilities c = rdna4x_query_caps(1, 1, 32, 0x10, 0x80);
    assert(rdna4x_query_supported(&c) && rdna4x_query_uses_quad(&c));
    c.subgroup_size = 3; assert(rdna4x_query_supported(&c) && !rdna4x_query_uses_quad(&c));
    c.subgroup_size = 4; c.subgroup_supported_stages = 0x20; assert(!rdna4x_query_uses_quad(&c));
    c.subgroup_supported_stages = 0x10; c.subgroup_supported_operations = 0; assert(!rdna4x_query_uses_quad(&c));
    c = rdna4x_query_caps(0, 1, 32, 0x10, 0x80); assert(!rdna4x_query_supported(&c));
    c = rdna4x_query_caps(1, 0, 32, 0x10, 0x80); assert(!rdna4x_query_supported(&c));
    c = rdna4x_query_caps(1, 1, 0, 0, 0); assert(rdna4x_query_supported(&c) && !rdna4x_query_uses_quad(&c));
    c.flags |= 4; assert(!rdna4x_query_caps_valid(&c));
    c = rdna4x_query_caps(1, 1, 32, 0x10, 0x80); c.version++; assert(!rdna4x_query_caps_valid(&c));
    c.version--; c.struct_size--; assert(!rdna4x_query_caps_valid(&c));
    assert(!rdna4x_query_supported(NULL));
    puts("QUERY_CAPS_PASS missing language/device, quad prerequisites, enumeration, unknown flags/version/size");
}
