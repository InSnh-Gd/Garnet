#include "amdgpu_ip.h"
#include "amdgpu_regs.h"
#include "amdgpu_psp.h"
#include <cstdio>
#include <vector>

namespace amdgpu {
struct GMCContext { unsigned gfxhub; uint64_t gart_pt_bus; };
struct CPContext {
    uint64_t tmr_pfp_ic, tmr_pfp_dc, tmr_me_ic, tmr_me_dc;
    uint64_t tmr_mec_ic, tmr_mec_dc0, tmr_mec_dc1;
};
struct BringupContext {
    DeviceContext *dev;
    bool gfxhubReady, wantInterrupts, cpLegacyRb;
    GMCContext gmc;
    CPContext cp;
    struct { uint64_t tmr_fw_addr_by_type[128]; } psp;
    unsigned gart;
};
bool powered, reject_flush, reject_cp;
unsigned early_flushes, successful_flushes;
std::vector<char> trace;
kern_return_t gmc_gfxhub_gart_enable(DeviceContext &, GMCContext &) {
    trace.push_back('G'); return kIOReturnSuccess;
}
void gmc_hdp_flush(DeviceContext &) { trace.push_back('H'); }
kern_return_t gmc_flush_gpu_tlb(DeviceContext &, GMCContext &, unsigned, unsigned, unsigned) {
    trace.push_back('T');
    if (!powered) { ++early_flushes; return kIOReturnTimeout; }
    if (reject_flush) return kIOReturnTimeout;
    ++successful_flushes;
    return kIOReturnSuccess;
}
kern_return_t gart_init(DeviceContext &, GMCContext &, unsigned &) {
    trace.push_back('A'); return kIOReturnSuccess;
}
kern_return_t doorbell_init(DeviceContext &, DoorbellState &) { return kIOReturnSuccess; }
void cp_set_eop_interrupt(DeviceContext &, bool) {}
void gmc_log_vm_faults(DeviceContext &, GMCContext &) {}
kern_return_t cp_init_full(DeviceContext &, GMCContext &, CPContext &, bool) {
    trace.push_back('C');
    if (reject_cp) return kIOReturnError;
    powered = true;
    return kIOReturnSuccess;
}
#define INIT_LOG(...) ((void)0)
#include "init_gart_actual.inc"
#undef INIT_LOG
}

int main() {
    unsigned failures = 0;
    for (unsigned scenario = 0; scenario < 3; ++scenario) {
        amdgpu::DeviceContext dev {};
        dev.ip.setBase(amdgpu::IPBlock::GC, 0, 0x1260);
        amdgpu::BringupContext ctx {};
        ctx.dev = &dev;
        amdgpu::powered = false;
        amdgpu::reject_flush = scenario == 1;
        amdgpu::reject_cp = scenario == 2;
        amdgpu::early_flushes = amdgpu::successful_flushes = 0;
        amdgpu::trace.clear();
        const auto result = amdgpu::run_cp_init(ctx);
        const auto expected = scenario == 0 ? kIOReturnSuccess :
                              scenario == 1 ? kIOReturnTimeout : kIOReturnError;
        if (result != expected || amdgpu::early_flushes != 0 ||
            amdgpu::successful_flushes != (scenario == 0 ? 1u : 0u)) {
            ++failures;
            std::printf("FAIL CPInit scenario=%u result=%#x early=%u completed=%u\n",
                        scenario, result, amdgpu::early_flushes, amdgpu::successful_flushes);
        }
    }
    std::printf("GART init: %u failed; simulated GC power and TLB ACK\n", failures);
    return failures ? 1 : 0;
}
