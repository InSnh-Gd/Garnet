#include "amdgpu_sdma.h"
#include "amdgpu_gmc.h"

#include <array>
#include <cstdio>

using namespace amdgpu;

namespace amdgpu {
void rdna4_logf(const char *, ...) {}
void sysmem_free(SysMem &) {}
}

static int failures;
static int checks;

static void check(bool condition, const char *description) {
    ++checks;
    if (!condition) {
        std::fprintf(stderr, "FAIL: %s\n", description);
        ++failures;
    }
}

int main() {
    DeviceContext dev{};
    dev.ip.setBase(IPBlock::GC, 0, 1u);
    dev.ip.setBase(IPBlock::NBIO, 2, 0x100u);
    std::array<uint32_t, 0x10000> registers{}, untouched{};
    registers.fill(0x9a5ea51au);
    constexpr uint32_t flushRegister = 0x0fe0u;
    registers[0x100u + NBIORegs::BIF_BX0_REMAP_HDP_MEM_FLUSH_CNTL] = flushRegister * 4u;
    untouched = registers;
    std::array<uint32_t, 16> ring{};
    std::array<uint32_t, kSDMAWBPageBytes / sizeof(uint32_t)> wb{};
    alignas(uint64_t) std::array<uint8_t, 32> bar2{};
    dev.rmmio = registers.data();
    dev.rmmioSize = sizeof(registers);
    dev.bar0 = reinterpret_cast<volatile uint8_t *>(ring.data());
    dev.bar0Size = sizeof(ring);

    SDMAInstance instance{};
    instance.inited = true;
    instance.enabled = true;
    instance.doorbell_index = 6;
    instance.ring_size_dwords = ring.size();
    instance.ring_ptr_mask = ring.size() - 1;
    instance.wb_cpu = wb.data();
    instance.wptr = 19;
    const uint32_t packet[] = { 0x1248u, 0xfe12u };
    constexpr unsigned wptrLo = kSDMAWBWptrOffset / sizeof(uint32_t);
    constexpr unsigned externalWptrLo = kSDMAWBExtWptrOffset / sizeof(uint32_t);

    check(sdma_ring_write(dev, instance, packet, 2) == 0 && instance.wptr == 19 && ring[3] == 0,
          "unmapped BAR2 refuses Queue0 packet staging without advancing its write pointer");
    check(sdma_kick_doorbell(dev, instance) != kIOReturnSuccess && wb[wptrLo] == 0 && registers == untouched,
          "unmapped BAR2 refuses Queue0 kick without publishing writeback or touching GC");
    check(sdma_q1_kick_doorbell_only(dev, instance, 19, 6) != kIOReturnSuccess && wb[externalWptrLo] == 0,
          "unmapped BAR2 refuses external doorbell before publishing its writeback pointer");

    dev.bar2 = bar2.data();
    dev.bar2Size = 31;
    check(sdma_kick_doorbell(dev, instance) != kIOReturnSuccess && wb[wptrLo] == 0 && registers == untouched,
          "Queue0 doorbell partially outside BAR2 does not publish a write pointer");

    dev.bar2Size = bar2.size();
    instance.doorbell_index = 5;
    check(sdma_kick_doorbell(dev, instance) != kIOReturnSuccess && wb[wptrLo] == 0 && registers == untouched,
          "Queue0 rejects unaligned 64-bit doorbell");

    instance.doorbell_index = 6;
    instance.wptr = 0x40000013ULL;
    registers = untouched;
    check(sdma_ring_write(dev, instance, packet, 2) == 2 && instance.wptr == 0x40000015ULL && ring[3] == 0x1248u,
          "operational BAR2 permits Queue0 packet staging");
    registers = untouched;
    uint64_t doorbell = 0;
    const auto rc = sdma_kick_doorbell(dev, instance);
    memcpy(&doorbell, bar2.data() + 24, sizeof(doorbell));
    untouched[flushRegister] = 0;
    check(rc == kIOReturnSuccess && wb[wptrLo] == 84 && wb[wptrLo + 1] == 1 &&
              doorbell == 0x100000054ULL && registers == untouched,
          "Queue0 kick publishes a 64-bit pointer to writeback and BAR2, with only the NBIO flush write");

    SDMAContext sdma{};
    sdma.instance_present[0] = true;
    sdma.instance[0] = instance;
    check(!sdma_doorbells_proven(dev, sdma),
          "mapped BAR2 and enabled Queue0 alone do not prove the GFXOFF path");

    sdma.instance[0].doorbell_test_passed = true;
    check(sdma_doorbells_proven(dev, sdma), "a proven SDMA0 suffices when SDMA1 is absent");
    sdma.instance_present[1] = true;
    sdma.instance[1] = instance;
    sdma.instance[1].doorbell_index = 4;
    check(!sdma_doorbells_proven(dev, sdma), "a present SDMA1 also needs its own fence proof");
    sdma.instance[1].doorbell_test_passed = true;
    check(sdma_doorbells_proven(dev, sdma), "both proven instances can use their BAR2 slots");
    dev.gfxoffPowerQuarantined = true;
    check(!sdma_doorbells_proven(dev, sdma) && sdma_ring_write(dev, instance, packet, 2) == 0 &&
          sdma_kick_doorbell(dev, instance) == kIOReturnNotReady &&
          wb[wptrLo] == 84 && registers == untouched,
          "quarantine refuses prior BAR2 proof and leaves ring, writeback, and MMIO unchanged");
    dev.gfxoffPowerQuarantined = false;
    dev.bar2Size = 31;
    check(!sdma_doorbells_proven(dev, sdma), "an old fence proof does not bypass current BAR2 bounds");

    auto &timed = sdma.instance[0];
    dev.bar2Size = bar2.size();
    timed.wptr = 0;
    const auto timedOut = sdma_copy_linear_test(dev, timed, 0x10000u, 0x14000u, 4, 0);
    check(timedOut == kIOReturnTimeout, "a kicked copy without a writeback fence times out");
    check(!sdma_doorbells_proven(dev, sdma) && sdma_ring_write(dev, timed, packet, 2) == 0,
          "a missing fence revokes admission and prevents reuse of the ring");

    GMCContext gmc{};
    gmc.vram_alloc.init(0x10000u, 0x10000u);
    VRAMAllocation ringStorage{};
    check(gmc.vram_alloc.alloc(kSDMARingDefaultBytes, kASPageSize, &ringStorage),
          "fixture allocates ring backing");
    timed.ring_gpu_va = ringStorage.gpu_va;
    const auto bytesBeforeRelease = gmc.vram_alloc.bytes_used();
    dev.ip.setBase(IPBlock::GC, 1, 0x200u);
    const auto status = sdma_reg_offset(dev, 0, sdma_regs(dev).STATUS_REG);
    registers[status] = 1;
    check(sdma_quiesce_for_teardown(dev, timed) == kIOReturnTimeout,
          "an idle-looking status cannot prove quiescence after an unresolved fence");
    sdma_release_storage(gmc, timed);
    check(timed.inited && gmc.vram_alloc.bytes_used() == bytesBeforeRelease,
          "teardown keeps the actual ring allocation while DMA may still be in flight");

    auto clean = instance;
    clean.ring_gpu_va = ringStorage.gpu_va;
    registers[status] = UINT32_MAX;
    check(sdma_quiesce_for_teardown(dev, clean) != kIOReturnSuccess && clean.enabled,
          "unreadable engine status does not authorize storage release");
    registers[status] = 1;
    check(sdma_quiesce_for_teardown(dev, clean) == kIOReturnSuccess && !clean.enabled,
          "healthy idle engine with confirmed disabled queue can be torn down");

    std::printf("sdma_doorbell_test: %d/%d checks passed\n", checks - failures, checks);
    return failures ? 1 : 0;
}
