#include "amdgpu_mes.h"
#include <cstddef>
#include <cstdio>
#include <fstream>
#include <sstream>
#include <string>

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

    // Upstream mes_v12_0.c / amdgpu_mes.h parity checks.
    check("kMES_VERSION_MASK", kMES_VERSION_MASK, 0x00000FFFu);
    check("kMESQueueType_SCHQ", kMESQueueType_SCHQ, 3u);
    check("kSetHwRsrcFlag_unmapped_doorbell_handling_BASIC_PLUS",
          kSetHwRsrcFlag_unmapped_doorbell_handling_BASIC_PLUS, 2u << 19);
    check("mes_oversubscription_timer(0x4b)", mes_oversubscription_timer(0x0000004bu), 0u);
    check("mes_oversubscription_timer(0x8a)", mes_oversubscription_timer(0x0000008au), 0u);
    check("mes_oversubscription_timer(0x1008a masked)", mes_oversubscription_timer(0x0001008au), 0u);
    check("mes_oversubscription_timer(0x8b)", mes_oversubscription_timer(0x0000008bu), 50u);
    check("mes_oversubscription_timer(0x1008b masked)", mes_oversubscription_timer(0x0001008bu), 50u);
    check("mes_oversubscription_timer(0x1f8a 12-bit mask)", mes_oversubscription_timer(0x00001f8au), 50u);
    return fail;
}

static unsigned verify_mes_init_order() {
    unsigned fail = 0;
    std::string src;
    for (const char *path : {"../src/amd/mes_v12_1.cpp", "src/garnet/src/amd/mes_v12_1.cpp"}) {
        std::ifstream f(path);
        if (f) {
            std::ostringstream ss;
            ss << f.rdbuf();
            src = ss.str();
            break;
        }
    }
    if (src.empty()) {
        std::printf("FAIL: unable to open mes_v12_1.cpp for init-order check\n");
        return 1;
    }
    const size_t fn = src.find("mes_init_full(DeviceContext");
    if (fn == std::string::npos) {
        std::printf("FAIL: mes_init_full not found\n");
        return 1;
    }
    const size_t p_kiq_q = src.find("mes_queue_init(dev, mes, MESPipe::KIQ)", fn);
    const size_t p_sch_q = src.find("mes_queue_init(dev, mes, MESPipe::Sched)", fn);
    const size_t p_rsrc  = src.find("mes_set_hw_resources(", fn);
    const size_t p_rsrc1 = src.find("mes_set_hw_resources_1(", fn);
    const size_t p_agg   = src.find("mes_init_aggregated_doorbell(", fn);
    const size_t p_query = src.find("mes_query_sched_status(", fn);
    if (p_kiq_q == std::string::npos || p_sch_q == std::string::npos ||
        p_rsrc == std::string::npos || p_rsrc1 == std::string::npos ||
        p_agg == std::string::npos || p_query == std::string::npos ||
        !(p_kiq_q < p_sch_q && p_sch_q < p_rsrc && p_rsrc < p_rsrc1 && p_rsrc1 < p_agg && p_agg < p_query)) {
        ++fail;
        std::printf("FAIL: mes_init_full order must be KIQ init -> SCHED init -> set_hw_resources -> set_hw_resources_1 -> init_aggregated_doorbell -> query_sched_status\n");
    }
    if (src.find("ComputeMqdOff::header") == std::string::npos ||
        src.find("kMESQueueType_SCHQ") == std::string::npos) {
        ++fail;
        std::printf("FAIL: mes_queue_init must use ComputeMqdOff::header and map SCHQ via KIQ\n");
    }
    if (src.find("gmc.vram_alloc.alloc(kMES_Resource1Bytes") == std::string::npos) {
        ++fail;
        std::printf("FAIL: mes_set_hw_resources_1 must allocate resource_1 in VRAM\n");
    }
    if (src.find("kSetHwRsrcFlag_unmapped_doorbell_handling_BASIC_PLUS") == std::string::npos) {
        ++fail;
        std::printf("FAIL: mes_set_hw_resources must use BASIC_PLUS (mode 2)\n");
    }
    if (src.find("mes_oversubscription_timer(mes.sched_version)") == std::string::npos) {
        ++fail;
        std::printf("FAIL: mes_set_hw_resources must gate oversubscription_timer on sched_version\n");
    }
    return fail;
}

int main() {
    const auto resources = amdgpu::queue_resources();
    unsigned failures = verify_wire_layouts() + verify_mes_init_order();
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
