// Build mock-good/missing/unknown dylibs from test-query-xlib-mock.c (the latter
// with -DMISSING_EXPORT/-DUNKNOWN_ABI), then pass the three paths in that order.
// On Linux, test-only CommonCrypto and Mach-O header shims are required.
#define _GNU_SOURCE
#ifdef __linux__
#define st_mtimespec st_mtim
#endif
#include <assert.h>
#include "rdna4_xlate.h"
static void check_forwarding(const char *path, rdna4x_backend_capabilities caps) {
    rdna4x_real real; rdna4x_deps deps; char error[256] = {0};
    assert(rdna4x_real_open(&real, &deps, NULL, NULL, path, error, sizeof error) == 0);
    real.capabilities = caps;
    uint8_t *code = NULL, *meta = NULL; size_t cl = 0, ml = 0;
    assert(deps.translate(deps.ctx, "ll", 2, NULL, &code, &cl, &meta, &ml, error, sizeof error) == 0);
    assert(deps.translate_linked(deps.ctx, "ll", 2, NULL, NULL, 0, &code, &cl, &meta, &ml, error, sizeof error) == 0);
    assert(deps.lower_arguments(deps.ctx, (const uint8_t *)"spv", 3, (const uint8_t *)"meta", 4, &code, &cl, &meta, &ml, error, sizeof error) == 0);
    unsigned *seen = dlsym(real.xl.h, "seen");
    assert(seen && *seen == (rdna4x_query_uses_quad(&caps) ? 7u : 56u));
    dlclose(real.xl.h);
}
int main(int argc, char **argv) {
    assert(argc == 4);
    check_forwarding(argv[1], rdna4x_query_caps(1, 1, 32, 0x10, 0x80));
    check_forwarding(argv[1], rdna4x_query_caps(1, 1, 32, 0x10, 0));
    rdna4x_xlib lib; char error[256] = {0};
    assert(rdna4x_xlib_load(&lib, argv[2], error, sizeof error) != 0 && !lib.h && strstr(error, "entry points"));
    assert(rdna4x_xlib_load(&lib, argv[3], error, sizeof error) != 0 && !lib.h && strstr(error, "unknown ABI"));
    uint8_t uuid[16] = {0}, ov[32] = {0}; char a[17], b[17], d[17];
    rdna4x_backend_capabilities quad = rdna4x_query_caps(1, 1, 32, 0x10, 0x80), enumeration = quad, disabled = quad;
    enumeration.subgroup_supported_operations = 0; disabled.flags = 0;
    rdna4x_key_with_capabilities(a, uuid, uuid, ov, "test", &quad);
    rdna4x_key_with_capabilities(b, uuid, uuid, ov, "test", &enumeration);
    rdna4x_key_with_capabilities(d, uuid, uuid, ov, "test", &disabled);
    assert(strcmp(a, b) && strcmp(a, d) && strcmp(b, d));
    for (unsigned field = 0; field < 6; ++field) {
        rdna4x_backend_capabilities changed = quad;
        uint32_t fields[6]; memcpy(fields, &changed, sizeof fields); fields[field] ^= 1;
        memcpy(&changed, fields, sizeof changed);
        rdna4x_key_with_capabilities(b, uuid, uuid, ov, "test", &changed); assert(strcmp(a, b));
    }
    puts("QUERY_XLIB_PASS real ABI3 dlsym direct/linked/postpass forwarding quad+enumeration, missing export, unknown ABI, cache capability identity");
}
