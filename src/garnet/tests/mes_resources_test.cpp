#include "amdgpu_mes.h"
#include <cstdio>

namespace amdgpu {
#define MES_LOG(...) ((void)0)
#include "mes_resources_actual.inc"
#undef MES_LOG
}

int main() {
    const auto resources = amdgpu::queue_resources();
    unsigned failures = 0;
    // GC 12.0.0 and 12.0.1: two MEC pipes, four queues per pipe.
    // Reserve queues 0/1 for kernel queues, including our pipe0/queue0.
    for (unsigned pipe = 0; pipe < 8; ++pipe) {
        const unsigned expected = pipe < 2 ? 0xcu : 0u;
        if (resources.compute_hqd_mask[pipe] != expected) {
            ++failures;
            std::printf("FAIL pipe%u mask=%#x expected=%#x\n", pipe,
                        resources.compute_hqd_mask[pipe], expected);
        }
    }
    if (resources.gfx_hqd_mask[0] != 0xfe || resources.gfx_hqd_mask[1] != 0 ||
        resources.vmid_mask_gfxhub != 0xfe || resources.vmid_mask_mmhub != 0xfe)
        ++failures;
    std::printf("MES resources: %u failed; production mask, no hardware access\n", failures);
    return failures ? 1 : 0;
}
