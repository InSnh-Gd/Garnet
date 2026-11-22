#include "amdgpu_gmc.h"
#include "amdgpu_field_defs.h"
#include "rebar_pure.h"
#include <cstdio>
#include <map>
#include <memory>
#include <vector>

constexpr kern_return_t kIOReturnNoSpace = 0x2db;
namespace amdgpu {
bool fail_dma;
unsigned dma_allocations, dma_frees;
struct Zeroed { uint64_t offset, bytes; };
std::vector<Zeroed> zeroed;
std::map<uint32_t, uint32_t> registers;
kern_return_t sysmem_alloc(SysMem &memory, uint64_t bytes, uint64_t) {
    if (fail_dma) return kIOReturnNoMemory;
    memory.bus = 0x1ee0a3000ull + (++dma_allocations * 4096);
    memory.size = bytes;
    return kIOReturnSuccess;
}
void sysmem_free(SysMem &memory) {
    if (memory.bus) ++dma_frees;
    memory = {};
}
void zero_vram(const DeviceContext &, uint64_t offset, uint8_t, uint64_t bytes) {
    zeroed.push_back({offset, bytes});
}
#define GMC_LOG(...) ((void)0)
#define bar0_memset_vram zero_vram
#define WREG32(dev, reg, value) (registers[(reg)] = (value))
#define RREG32(dev, reg) (registers[(reg)])
#include "gmc_resources_actual.inc"
#undef RREG32
#undef WREG32
#undef bar0_memset_vram
#undef GMC_LOG
}

int main() {
    using namespace amdgpu;
    unsigned failures = 0;
    const auto expect = [&](bool ok, const char *message) {
        if (!ok) { ++failures; std::printf("FAIL %s\n", message); }
    };
    DeviceContext dev {};
    dev.vramMcBase = 0x8000000000ull;
    dev.vramBase = 32ull << 20;
    dev.vramLimit = 256ull << 20;
    dev.ip.setBase(IPBlock::GC, 0, 0);
    for (unsigned scenario = 0; scenario < 3; ++scenario) {
        auto gmc = std::make_unique<GMCContext>();
        gmc->inited = true;
        gmc->vram_start = dev.vramMcBase;
        gmc->real_vram_size = 16ull << 30;
        gmc->gart_size = 256ull << 20;
        gmc->vram_base_offset = 0x200000000ull;
        fail_dma = scenario == 1;
        dev.vramLimit = scenario == 2 ? dev.vramBase + 0x780000 : 256ull << 20;
        zeroed.clear();
        dma_allocations = dma_frees = 0;
        const auto result = gmc_alloc_resources(dev, *gmc);
        if (scenario != 0) {
            expect(result != kIOReturnSuccess, "allocation failure returned");
            expect(gmc->gart_pt_bus == 0, "failure leaves resources uncommitted");
            gmc_release_resources(*gmc);
            expect(dma_allocations == dma_frees, "failure releases DMA allocation");
            continue;
        }
        expect(result == kIOReturnSuccess, "resource allocation succeeds");
        const auto scratch = gmc->mem_scratch_bus;
        expect(scratch >= dev.vramMC(dev.vramBase) &&
               scratch < dev.vramMC(dev.vramLimit), "scratch is VRAM, not host physical");
        bool cleared = false;
        for (const auto &range : zeroed)
            cleared |= dev.vramMC(range.offset) == scratch && range.bytes >= 4096;
        expect(cleared, "default scratch page is zeroed");
        HubContext hub {};
        hub.ip = IPBlock::GC;
        hub.vm_system_aperture_default_addr_lo = 20;
        hub.vm_system_aperture_default_addr_hi = 21;
        hub_init_system_aperture_regs(dev, *gmc, hub, false);
        const uint64_t programmed = (uint64_t(registers[21]) << 44) |
                                    (uint64_t(registers[20]) << 12);
        expect(programmed == scratch - gmc->vram_start + gmc->vram_base_offset &&
               programmed < gmc->vram_base_offset + gmc->real_vram_size,
               "default register encodes an in-range VRAM address without wrap");
        expect(gmc_alloc_resources(dev, *gmc) == kIOReturnSuccess && dma_allocations == 1,
               "repeat allocation keeps one dummy DMA page");
        gmc_release_resources(*gmc);
        gmc_release_resources(*gmc);
        expect(dma_frees == dma_allocations && gmc->mem_scratch_bus == 0,
               "release is complete and idempotent");
    }
    std::printf("GMC resources: %u failed; production allocation/register code\n", failures);
    return failures ? 1 : 0;
}
