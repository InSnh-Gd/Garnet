#include "amd/amdgpu_regs.h"

#include <atomic>
#include <cassert>
#include <cstdio>
#include <thread>

int main() {
    uint32_t regs[7] {};
    amdgpu::DeviceContext ctx {};
    ctx.rmmio = regs;
    ctx.rmmioSize = sizeof(regs);
    amdgpu::WVRAM32_via_mm(ctx, 0x80000004ull, 42);
    assert(regs[0] == 0 && regs[1] == 0 && regs[6] == 0);
    assert(amdgpu::RVRAM32_via_mm(ctx, 0) == 0xffffffffu);

    ctx.mmIndexLock = IOLockAlloc();
    assert(ctx.mmIndexLock);
    constexpr uint32_t salt = 0x13579bdu;
    auto write = [&](uint32_t hi, uint32_t low) {
        amdgpu::WVRAM32_via_mm(ctx, (uint64_t(hi) << 31) | low,
                              hi ^ low ^ salt);
    };
    write(1, 4);
    std::atomic<unsigned> finished { 0 };
    auto writer = [&](uint32_t hi) {
        for (uint32_t i = 1; i <= 50000; ++i) write(hi, i * 4);
        finished.fetch_add(1);
    };
    std::thread a(writer, 2), b(writer, 3);
    do {
        // Observe each completed operation as one tuple under the same window lock.
        IOLockLock(ctx.mmIndexLock);
        assert(regs[1] == (regs[6] ^ (regs[0] & 0x7ffffffcu) ^ salt));
        IOLockUnlock(ctx.mmIndexLock);
    } while (finished.load() != 2);
    a.join();
    b.join();
    assert(amdgpu::RVRAM32_via_mm(ctx, 0x180000004ull) == regs[1]);
    assert(regs[6] == 3 && regs[0] == 0x80000004u);
    regs[1] = 42;
    (void)amdgpu::RVRAM32_via_mm(ctx, (1ull << 31) | 4);
    finished = 0;
    auto reader = [&](uint32_t hi) {
        for (unsigned i = 0; i < 50000; ++i)
            assert(amdgpu::RVRAM32_via_mm(ctx, (uint64_t(hi) << 31) | (hi * 4)) == 42);
        finished.fetch_add(1);
    };
    std::thread c(reader, 2), d(reader, 3);
    do {
        IOLockLock(ctx.mmIndexLock);
        assert((regs[0] & 0x7ffffffcu) == regs[6] * 4);
        IOLockUnlock(ctx.mmIndexLock);
    } while (finished.load() != 2);
    c.join();
    d.join();
    IOLockFree(ctx.mmIndexLock);
    ctx.mmIndexLock = nullptr;
    std::puts("mm_index_lock_test: PASS (100000 concurrent writes, 100000 concurrent reads, null-lock refusal)");
}
