// CPU/framework observation checks; no driver or GPU work.
#import <Foundation/Foundation.h>
#include <assert.h>
#define RDNA4LOG(...) do { fprintf(stderr, __VA_ARGS__); fputc('\n', stderr); } while (0)
#include "rdna4_compositor_observation.h"

int main(void) {
#if defined(__x86_64__)
    @autoreleasepool {
        void *framework = dlopen("/System/Library/PrivateFrameworks/SkyLight.framework/SkyLight",
                                RTLD_LAZY | RTLD_LOCAL);
        assert(framework);
        void *symbol = dlsym(RTLD_DEFAULT, "SLSMainConnectionID");
        assert(symbol);
        const uint8_t *base = rdna4_observation_image(symbol, rdna4_observation_skylight_uuid);
        if (!base) { puts("UNSUPPORTED_SKYLIGHT_IMAGE"); return 77; }
        uint8_t wrong[16] = {0};
        assert(!rdna4_observation_image(symbol, wrong));
        assert(!rdna4_observation_image(NULL, rdna4_observation_skylight_uuid));
        uint32_t magic = 0;
        assert(rdna4_observation_read(base, 0, &magic, sizeof magic));
        assert(magic == MH_MAGIC_64);
        assert(!rdna4_observation_read(NULL, 0, &magic, sizeof magic));
        assert(rdna4_compositor_snapshot(0) == 1);
        // An ordinary process must never schedule the server-only accessor calls.
        assert(geteuid() != 88);
        rdna4_compositor_observe();
        puts("COMPOSITOR_OBSERVATION_PASS gpu_execution=0 server_callbacks_tested=0");
    }
    return 0;
#else
    puts("UNSUPPORTED_ARCHITECTURE");
    return 77;
#endif
}
