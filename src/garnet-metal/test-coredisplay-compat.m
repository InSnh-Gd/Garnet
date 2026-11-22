// Version-independent CoreDisplay compatibility regression, CPU only.
#import <Foundation/Foundation.h>
#include <assert.h>
#include <dlfcn.h>
#define RDNA4LOG(...) do { fprintf(stderr, __VA_ARGS__); fputc('\n', stderr); } while (0)
#include "rdna4_coredisplay_compat.h"

int main(void) {
    @autoreleasepool {
        void *framework = dlopen("/System/Library/Frameworks/CoreDisplay.framework/CoreDisplay", RTLD_LAZY);
        bool (*use_iop)(void) = framework ? dlsym(framework, "CoreDisplay_UseIOPresentment") : NULL;
        bool before = use_iop ? use_iop() : false;

        assert(!rdna4_cd_bound_primary(0));
        // Outside WindowServer (euid != 88), compatibility check must be a safe no-op.
        assert(geteuid() != 88);
        rdna4_coredisplay_compat(0);

        if (use_iop) assert(use_iop() == before);
        printf("COREDISPLAY_COMPAT_PASS gpu_execution=0 iop=%d\n", use_iop ? (int)before : -1);
        return 0;
    }
}
