#include "amdgpu_mes.h"
#include <cstddef>
#include <cstdio>

namespace amdgpu {
#define MES_LOG(...) ((void)0)
#include "mes_resources_actual.inc"
#undef MES_LOG
}

static unsigned verify_wire_layouts() {
    using namespace amdgpu;
    unsigned fail = 0;
    auto check = [&](const char *name, size_t got, size_t want) {
        if (got != want) {
            ++fail;
            std::printf("FAIL %s: got=%zu want=%zu\n", name, got, want);
        }
    };
    check("sizeof(MES_SetHwResources)", sizeof(MES_SetHwResources), 256);
    check("MES_SetHwResources.api_status", offsetof(MES_SetHwResources, api_status), 200);
    check("sizeof(MES_SetHwResources1)", sizeof(MES_SetHwResources1), 256);
    check("MES_SetHwResources1.api_status", offsetof(MES_SetHwResources1, api_status), 8);
    check("MES_SetHwResources1.cleaner_shader_fence_mc_addr", offsetof(MES_SetHwResources1, cleaner_shader_fence_mc_addr), 64);
    check("sizeof(MES_QuerySchedStatus)", sizeof(MES_QuerySchedStatus), 256);
    check("MES_QuerySchedStatus.api_status", offsetof(MES_QuerySchedStatus, api_status), 8);
    check("sizeof(MES_AddQueue)", sizeof(MES_AddQueue), 256);
    check("MES_AddQueue.trap_handler_addr", offsetof(MES_AddQueue, trap_handler_addr), 136);
    check("MES_AddQueue.api_status", offsetof(MES_AddQueue, api_status), 152);
    check("sizeof(MES_RemoveQueue)", sizeof(MES_RemoveQueue), 256);
    check("MES_RemoveQueue.api_status", offsetof(MES_RemoveQueue, api_status), 24);
    check("sizeof(MES_ResetQueue)", sizeof(MES_ResetQueue), 256);
    check("MES_ResetQueue.api_status", offsetof(MES_ResetQueue, api_status), 112);
    return fail;
}

int main() {
    const auto resources = amdgpu::queue_resources();
    unsigned failures = verify_wire_layouts();
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
    std::printf("MES resources & wire layouts: %u failed; production mask, no hardware access\n", failures);
    return failures ? 1 : 0;
}
