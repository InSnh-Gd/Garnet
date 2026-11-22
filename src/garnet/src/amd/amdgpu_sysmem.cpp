// amdgpu_sysmem.cpp — host system-memory allocator and EFI memory-map derived host RAM ceiling for DMA validation.

#include "amdgpu_sysmem.h"
#include "ramtop_pure.h"
#include "rdna4_log.h"

#include <IOKit/IOBufferMemoryDescriptor.h>
#include <IOKit/IOLib.h>
#include <IOKit/IOMemoryDescriptor.h>
#include <pexpert/pexpert.h>

#define SYSMEMLOG(fmt, ...) ::amdgpu::rdna4_logf("sysmem-ramtop: " fmt "\n", ##__VA_ARGS__)

namespace amdgpu {

namespace {

uint32_t gRamTopDone = 0u;
uint64_t gRamTopPhysMem = 0ull;
uint64_t gRamTopDerived = RDNA4_RT_LEGACY_TOP;
rdna4_rt_res gRamTopRes {};

void log_ramtop_result(const char *how) {
    SYSMEMLOG("host-page RAM bound %#llx (%s; legacy floor %#llx, map reason %s). "
              "EFI map: %u descriptor(s), %u RAM, %u device window(s), %u empty, %u out of order; "
              "RAM total %llu MiB vs installed %llu MiB.",
              (unsigned long long)gRamTopDerived, how, (unsigned long long)RDNA4_RT_LEGACY_TOP,
              rdna4_rt_reason_name(gRamTopRes.reason), gRamTopRes.descs, gRamTopRes.ram_descs,
              gRamTopRes.dev_descs, gRamTopRes.empty_descs, gRamTopRes.unsorted,
              (unsigned long long)(gRamTopRes.ram_bytes >> 20),
              (unsigned long long)(gRamTopPhysMem >> 20));
}

} // namespace

void sysmem_ramtop_init() {
    if (gRamTopDone) return;
    gRamTopDone = 1u;

    const boot_args *ba = reinterpret_cast<const boot_args *>(PE_state.bootArgs);
    if (!ba) {
        gRamTopRes.reason = RDNA4_RT_NO_MAP;
        gRamTopRes.value = RDNA4_RT_LEGACY_TOP;
        log_ramtop_result("PE_state.bootArgs is null");
        return;
    }

    const uint64_t phys  = ba->MemoryMap;
    const uint64_t bytes = ba->MemoryMapSize;
    const uint32_t dsz   = ba->MemoryMapDescriptorSize;
    gRamTopPhysMem       = ba->PhysicalMemorySize;

    if (!phys || (phys & 0xfffull) || !bytes ||
        bytes > (uint64_t)RDNA4_RT_DESC_MAX * RDNA4_RT_DESC_COUNT_MAX ||
        phys + bytes > RDNA4_RT_HOLE_LO) {
        gRamTopRes.reason = RDNA4_RT_BAD_HEADER;
        gRamTopRes.value = RDNA4_RT_LEGACY_TOP;
        log_ramtop_result("boot_args names a map we refuse to map");
        return;
    }

    const char *how = "read";
    if (IOMemoryDescriptor *md = IOMemoryDescriptor::withPhysicalAddress(
            (IOPhysicalAddress)phys, (IOByteCount)bytes, kIODirectionIn)) {
        if (md->prepare(kIODirectionIn) == kIOReturnSuccess) {
            if (IOMemoryMap *map = md->map()) {
                rdna4_rt_map in {};
                in.desc      = reinterpret_cast<const uint8_t *>(map->getVirtualAddress());
                in.bytes     = bytes;
                in.desc_size = dsz;
                in.phys_mem  = gRamTopPhysMem;
                in.floor     = RDNA4_RT_LEGACY_TOP;
                (void)rdna4_rt_eval(&in, &gRamTopRes);
                map->release();
            } else {
                how = "IOMemoryMap::map failed";
                gRamTopRes.reason = RDNA4_RT_NO_MAP;
            }
            md->complete(kIODirectionIn);
        } else {
            how = "IOMemoryDescriptor::prepare failed";
            gRamTopRes.reason = RDNA4_RT_NO_MAP;
        }
        md->release();
    } else {
        how = "IOMemoryDescriptor::withPhysicalAddress failed";
        gRamTopRes.reason = RDNA4_RT_NO_MAP;
    }

    if (gRamTopRes.value < RDNA4_RT_LEGACY_TOP) gRamTopRes.value = RDNA4_RT_LEGACY_TOP;
    gRamTopDerived = gRamTopRes.value;
    log_ramtop_result(how);
}

uint64_t sysmem_ramtop_derived_or_zero() {
    return (gRamTopDone != 0u && gRamTopRes.reason == RDNA4_RT_OK) ? gRamTopDerived : 0ull;
}

kern_return_t sysmem_alloc(SysMem &m, uint64_t size, uint64_t align) {
    sysmem_free(m);
    if (size == 0) return kIOReturnBadArgument;
    if (align < 4096) align = 4096;
    if (align & (align - 1)) return kIOReturnBadArgument;
    // physical mask: 48-bit addresses, low bits force alignment
    const mach_vm_address_t mask = 0x0000FFFFFFFFFFFFULL & ~(align - 1);
    IOBufferMemoryDescriptor *md = IOBufferMemoryDescriptor::inTaskWithPhysicalMask(
        kernel_task, kIODirectionInOut | kIOMemoryPhysicallyContiguous | kIOMemoryMapperNone,
        size, mask);
    if (!md) return kIOReturnNoMemory;
    if (md->prepare() != kIOReturnSuccess) { md->release(); return kIOReturnNoMemory; }
    IOByteCount seg = 0;
    addr64_t phys = md->getPhysicalSegment(0, &seg, kIOMemoryMapperNone);
    void *cpu = md->getBytesNoCopy();
    if (!phys || !cpu || seg < size) { md->complete(); md->release(); return kIOReturnNoMemory; }
    bzero(cpu, (size_t)size);
    m.md = md; m.cpu = cpu; m.bus = phys; m.size = md->getLength();
    return kIOReturnSuccess;
}

void sysmem_free(SysMem &m) {
    if (m.md) { m.md->complete(); m.md->release(); }
    m.md = nullptr; m.cpu = nullptr; m.bus = 0; m.size = 0;
}

} // namespace amdgpu
