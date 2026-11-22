#include <cassert>
#include <cstdio>
#include <cstring>
#include <limits>
#include <array>
#include <map>
#include "native_buffer_pure.h"

using namespace rdna4native;
using namespace rdna4native::s1c;

// Hardware boundary: interpret only the packets produced by the production runner.
struct Device {
    std::array<uint32_t, 128> memory{};
    VaEnt entries[2]{{0x20000, 256, 0, 1, RDNA4N_VM_PAGE_READABLE},
                     {0x400000, 256, 0, 2, RDNA4N_VM_PAGE_WRITEABLE}};
    unsigned fail = 0, gates = 0, waits = 0, prepared = 0, submissions = 0;
    bool sourceLive = true, host = false;
    uint32_t gate() { return ++gates == fail ? kAborted : kOk; }
    BufferBo bo(uint32_t h) { return {256, h == 2 || (h == 1 && sourceLive), host}; }
    const VaEnt *maps() { return entries; }
    uint32_t map_count() { return 2; }
    uint32_t idle() { ++waits; return fail == 3 ? kTimeout : kOk; }
    uint32_t prepare() { ++prepared; return fail == 4 ? kNoMemory : kOk; }
    uint32_t submit(const uint32_t *dw, uint32_t n, uint64_t *seq) {
        ++submissions;
        if (fail == 5) return kNoResources;
        assert(waits == 1 && gates == 2 && prepared == 1);
        assert(n >= 25 && dw[0] == 0xc0065800 && dw[7] == 0x1c3f1);
        unsigned i = 8;
        for (; i < n && dw[i] == 0xc0055000; i += 7) {
            assert(dw[i + 3] == 0 && dw[i + 5] == 0);
            const unsigned bytes = dw[i + 6] & 0x7fff;
            const unsigned dest = 64 + (dw[i + 4] - 0x400000) / 4;
            assert(bytes > 0 && dest + bytes / 4 <= memory.size());
            for (unsigned j = 0; j < bytes / 4; ++j)
                memory[dest + j] = dw[i + 1] == 0xc0300000 ? dw[i + 2] : memory[(dw[i + 2] - 0x20000) / 4 + j];
        }
        assert(dw[i] == 0xc0004200 && dw[i + 2] == 0xc0065800 && dw[i + 9] == 0x1c3f1);
        *seq = 7;
        return kOk;
    }
};

static void runner_tests() {
    const rdna4n_buffer_op copy{1, 2, 0, 0, 4, 8, 64, 0};
    for (unsigned fail = 0; fail <= 5; ++fail) {
        Device dev;
        for (unsigned i = 0; i < dev.memory.size(); ++i) dev.memory[i] = i;
        const auto before = dev.memory;
        dev.fail = fail;
        uint64_t seq = 99;
        const uint32_t rc = buffer_run(dev, copy, false, &seq);
        if (fail) {
            const uint32_t errors[]{0, kAborted, kAborted, kTimeout, kNoMemory, kNoResources};
            assert(rc == errors[fail] && seq == 0 && before == dev.memory);
            assert(dev.submissions == (fail == 5 ? 1 : 0));
        } else {
            assert(rc == kOk && seq == 7);
            for (unsigned i = 0; i < dev.memory.size(); ++i)
                assert(dev.memory[i] == (i >= 66 && i < 82 ? before[i - 65] : before[i]));
        }
    }
    for (unsigned failure = 0; failure < 8; ++failure) {
        Device dev;
        dev.memory.fill(0xdeadbeef);
        const auto before = dev.memory;
        auto q = copy;
        if (failure == 0) q.src_handle = 0xffffffff;
        if (failure == 1) dev.sourceLive = false;
        if (failure == 2) dev.host = true;
        if (failure == 3) dev.entries[0].flags = RDNA4N_VM_PAGE_WRITEABLE;
        if (failure == 4) dev.entries[1].flags = RDNA4N_VM_PAGE_READABLE;
        if (failure == 5) dev.entries[1].size = 16;
        if (failure == 6) q.dst_offset = UINT64_MAX - 3;
        if (failure == 7) q.dst_handle = 0;
        uint64_t seq = 99;
        assert(buffer_run(dev, q, false, &seq) != kOk && seq == 0);
        assert(dev.waits == 0 && dev.prepared == 0 && dev.submissions == 0 && dev.memory == before);
    }
    Device dev;
    dev.memory.fill(0xdeadbeef);
    const rdna4n_buffer_op fill{0, 2, 0x12345678, 0, 0, 4, 64, 0};
    uint64_t seq = 0;
    assert(buffer_run(dev, fill, true, &seq) == kOk && seq == 7);
    for (unsigned i = 0; i < dev.memory.size(); ++i)
        assert(dev.memory[i] == (i >= 65 && i < 81 ? 0x12345678u : 0xdeadbeefu));
    std::puts("PASS: production runner copy/fill, ownership/access, gate/wait/allocation/admission failures, destination guards");
}

static void mapping_tests() {
    assert(buffer_reset_loses({8, 2}, 2, 7)); // pending operation dropped by another session's reset
    assert(!buffer_reset_loses({8, 2}, 2, 8)); // completed before reset
    assert(!buffer_reset_loses({8, 2}, 3, 7)); // reopened slot
    assert(!buffer_reset_loses({0, 0}, 0, 0));
    struct Pages {
        std::map<std::pair<uint64_t, uint32_t>, uint64_t> entries;
        uint64_t next = 0x100000;
        uint64_t alloc_page() { next += 4096; return next; }
        uint64_t rd(uint64_t pa, uint32_t i) { return entries[{pa, i}]; }
        void wr(uint64_t pa, uint32_t i, uint64_t value) { entries[{pa, i}] = value; }
    } pages;
    const uint64_t root = pages.alloc_page(), command = pages.alloc_page();
    const uint64_t flags = leaf_flags(RDNA4N_VM_PAGE_READABLE | RDNA4N_VM_PAGE_EXECUTABLE, true, false);
    assert(pt_map(root, kBufferIbVa, 1, command, flags, pages) == kOk);
    uint64_t leaf = 0;
    assert(pt_walk(root, kBufferIbVa, pages, &leaf) == command);
    assert((leaf & kPteWrite) == 0 && (leaf & kPteRead) && (leaf & kPteExec));
    assert(kBufferIbVa + kPage <= kVaFirst);
    const VaEnt maps[]{{0x800000020000ull, 128, 64, 1, RDNA4N_VM_PAGE_READABLE | RDNA4N_VM_PAGE_WRITEABLE}};
    rdna4n_buffer_op q{1, 1, 0, 0, 64, 128, 64, 0};
    uint64_t src = 0, dst = 0;
    assert(buffer_resolve(q, false, maps, 1, 192, 192, &src, &dst) == kOk);
    assert(src == 0xffff800000020000ull && dst == src + 64);
    uint32_t dw[32];
    assert(buffer_build(dw, 32, src, dst, 64, false, 0) == 32);
    assert(dw[11] == 0xffff8000 && dw[13] == 0xffff8000);
    q.dst_offset = 132;
    assert(buffer_resolve(q, false, maps, 1, 192, 192, &src, &dst) == kBadArg);
    std::puts("PASS: protected IB page-table mapping, nonzero BO mapping offsets, high canonical VA, exact-end bounds");
}

int main() {
    runner_tests();
    mapping_tests();
    const VaEnt maps[] = {{0x20000, 2 * 1024 * 1024, 0, 1, RDNA4N_VM_PAGE_READABLE},
                          {0x400000, 2 * 1024 * 1024, 0, 2, RDNA4N_VM_PAGE_WRITEABLE}};
    rdna4n_buffer_op q{1, 2, 0, 0, 0, 0, 1024 * 1024, 0};
    uint64_t src = 0, dst = 0;
    assert(buffer_resolve(q, false, maps, 2, 2 * 1024 * 1024, 2 * 1024 * 1024, &src, &dst) == kOk);
    uint32_t dw[1024];
    const uint32_t n = buffer_build(dw, 1024, src, dst, q.length, false, 0x12345678);
    assert(n > 0 && n < 1024);
    // Interpret the emitted DMA packets, not a separate copy implementation.
    uint32_t source[262144], target[262144];
    for (unsigned i = 0; i < 262144; ++i) source[i] = i ^ 0x5a5a5a5a;
    std::memset(target, 0, sizeof target);
    unsigned copied = 0;
    for (unsigned i = 8; i + 7 <= n && dw[i] == 0xc0055000; i += 7) {
        assert(dw[i + 1] == 0xe0300000);
        const uint32_t bytes = dw[i + 6] & ~(1u << 30);
        assert(bytes <= 32736 && !(bytes & 3) && (dw[i + 6] & (1u << 30)));
        assert(dw[i + 2] == src + copied && dw[i + 4] == dst + copied);
        std::memcpy(reinterpret_cast<char *>(target) + copied,
                    reinterpret_cast<char *>(source) + copied, bytes);
        copied += bytes;
    }
    assert(copied == q.length && std::memcmp(source, target, sizeof target) == 0);
    assert(dw[8 + 33 * 7] == 0xc0004200 && dw[10 + 33 * 7] == 0xc0065800);
    assert(buffer_build(dw, 1024, 0, dst, 64, true, 0x12345678) == 32);
    assert(dw[9] == 0xc0300000 && dw[10] == 0x12345678 && dw[11] == 0);
    uint32_t untouched[1024];
    std::memset(untouched, 0xa5, sizeof untouched);
    std::memcpy(dw, untouched, sizeof dw);
    assert(buffer_build(dw, 1, src, dst, 64, false, 0) == 0);
    assert(std::memcmp(dw, untouched, sizeof dw) == 0);
    for (unsigned bad = 0; bad < 8; ++bad) {
        auto invalid = q;
        if (bad == 0) invalid.length = 0;
        if (bad == 1) invalid.length++;
        if (bad == 2) invalid.length = RDNA4N_BUFFER_MAX_BYTES + 4;
        if (bad == 3) invalid.dst_offset = std::numeric_limits<uint64_t>::max() - 3;
        if (bad == 4) invalid.src_offset = 2 * 1024 * 1024;
        if (bad == 5) invalid.flags = 1;
        if (bad == 6) invalid.reserved = 1;
        if (bad == 7) invalid.src_handle = 2;
        assert(buffer_resolve(invalid, false, maps, 2, 2 * 1024 * 1024, 2 * 1024 * 1024, &src, &dst) != kOk);
        assert(std::memcmp(dw, untouched, sizeof dw) == 0);
    }
    q.src_handle = 1; q.dst_handle = 1; q.length = 64; q.dst_offset = 4;
    assert(buffer_resolve(q, false, maps, 2, 2097152, 2097152, &src, &dst) == kBadArg);
    q = {0, 2, 0x76543210, 0, 0, 0, 64, 0};
    assert(buffer_resolve(q, true, maps, 2, 0, 2097152, &src, &dst) == kOk);
    q.src_offset = 4;
    assert(buffer_resolve(q, true, maps, 2, 0, 2097152, &src, &dst) == kBadArg);
    std::puts("PASS: native buffer bounds, permissions, overlap, fill/copy packet execution, atomic refusal");
}
