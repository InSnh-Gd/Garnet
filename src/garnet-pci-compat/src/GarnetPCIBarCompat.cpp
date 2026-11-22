//
//  GarnetPCIBarCompat.cpp — Early Root PCI BAR0 compatibility kext for RDNA4 on macOS 25G332.
//
//  Installs a bounded prologue hook on IOPCIConfigurator::probeBaseAddressRegister inside
//  kmod_start (~405 ms before PCI configuration) and restores original bytes in _antimain.
//  Zero Lilu dependency; hook path is 100% lock-free, allocation-free, and IOLog-free.
//
#include <IOKit/IOLib.h>
#include <IOKit/IOService.h>
#include <IOKit/pci/IOPCIBridge.h>
#include <IOKit/pci/IOPCIDevice.h>
#include <i386/machine_routines.h>
#include <libkern/c++/OSMetaClass.h>
#include <libkern/c++/OSSymbol.h>
#include <mach/kmod.h>
#include <pexpert/pexpert.h>

#include "pci_bar_compat_pure.h"

extern "C" void iopci_bridge_initialize(void) __asm__("__ZN11IOPCIBridge10initializeEv");

extern "C" void garnet_pci_bar_compat_trampoline(
    void *self, void *device, uint32_t lastBarNum, uint32_t resetMask);

__asm__(
    ".text\n"
    ".p2align 6\n"
    "_garnet_pci_bar_compat_trampoline:\n"
    ".rept 48\n"
    "  nop\n"
    ".endr\n"
);

namespace {

using ProbeFn = void (*)(void *self, void *device, uint32_t lastBarNum, uint32_t resetMask);
using Read16Fn = uint16_t (*)(void *, void *, uint32_t, void *);
using Write16Fn = void (*)(void *, void *, uint32_t, uint16_t, void *);
using Read32Fn = uint32_t (*)(void *, void *, uint32_t, void *);
using Write32Fn = void (*)(void *, void *, uint32_t, uint32_t, void *);
using RangeInitFn = void (*)(void *, uint32_t, uint64_t, uint64_t, uint64_t);

garnet_pcibar::VerifiedTarget gTarget {};
garnet_pcibar::ProbeOps gOps {};
garnet_pcibar::ProbeCounters gCounters {};
garnet_pcibar::CompatStatus gStatus = garnet_pcibar::CompatStatus::kNotEvaluated;
bool gHookInstalled = false;
bool gLateLoadRefused = false;
ProbeFn gOrigProbe = nullptr;

inline uint64_t read_cr0(void) {
    uint64_t val = 0;
    __asm__ volatile("movq %%cr0, %0" : "=r"(val));
    return val;
}

inline void write_cr0(uint64_t val) {
    __asm__ volatile("movq %0, %%cr0" : : "r"(val) : "memory");
}

inline void write_ro_bytes(uintptr_t dstAddr, const uint8_t *src, size_t len) {
    volatile uint8_t *dst = reinterpret_cast<volatile uint8_t *>(dstAddr);
    for (size_t i = 0; i < len; ++i) {
        dst[i] = src[i];
    }
}

void install_detour_locked(uintptr_t trampolineAddr, const uint8_t *trampolineBytes, size_t trampLen,
                           uintptr_t entryAddr, const uint8_t *detourBytes, size_t detourLen) {
    const boolean_t prevInts = ml_set_interrupts_enabled(FALSE);
    const uint64_t cr0 = read_cr0();
    const bool wp = (cr0 & (1ULL << 16)) != 0ULL;
    if (wp) write_cr0(cr0 & ~(1ULL << 16));

    write_ro_bytes(trampolineAddr, trampolineBytes, trampLen);
    write_ro_bytes(entryAddr, detourBytes, detourLen);
    __asm__ volatile("mfence" ::: "memory");

    if (wp) write_cr0(cr0);
    ml_set_interrupts_enabled(prevInts);
}

void restore_detour_locked(uintptr_t entryAddr, const uint8_t *origBytes, size_t origLen) {
    const boolean_t prevInts = ml_set_interrupts_enabled(FALSE);
    const uint64_t cr0 = read_cr0();
    const bool wp = (cr0 & (1ULL << 16)) != 0ULL;
    if (wp) write_cr0(cr0 & ~(1ULL << 16));

    write_ro_bytes(entryAddr, origBytes, origLen);
    __asm__ volatile("mfence" ::: "memory");

    if (wp) write_cr0(cr0);
    ml_set_interrupts_enabled(prevInts);
}

// Runs inside mp_rendezvous_no_intrs with interrupts disabled:
// MUST stay 100% lock-free, allocation-free, and MUST NOT call IOLog.
void hooked_probeBaseAddressRegister(void *self, void *device,
                                     uint32_t lastBarNum, uint32_t resetMask) {
    if (garnet_pcibar::handle_probe_base_address_register(
            gOps, gCounters, self, device, lastBarNum, resetMask)) {
        return;
    }
    gOrigProbe(self, device, lastBarNum, resetMask);
}

uint32_t query_configurator_instances(void) {
    const OSSymbol *sym = OSSymbol::withCStringNoCopy("IOPCIConfigurator");
    if (!sym) return 0u;
    const OSMetaClass *mc = OSMetaClass::getMetaClassWithName(sym);
    const uint32_t count = mc ? mc->getInstanceCount() : 0u;
    sym->release();
    return count;
}

} // namespace

extern "C" kern_return_t garnet_pci_compat_kmod_start(kmod_info_t *ki, void *data) {
    (void)ki;
    (void)data;

    uint32_t bootArgVal = 1u;
    const bool bootArgPresent =
        PE_parse_boot_argn("garnet-rebar-compat", &bootArgVal, sizeof(bootArgVal)) != FALSE;

    const uint32_t pciDevCount =
        IOPCIDevice::metaClass ? IOPCIDevice::metaClass->getInstanceCount() : 0u;
    const uint32_t cfgCount = query_configurator_instances();

    const garnet_pcibar::CompatStatus policy = garnet_pcibar::evaluate_load_policy(
        bootArgPresent, bootArgVal, pciDevCount, cfgCount);
    if (policy != garnet_pcibar::CompatStatus::kInstalled) {
        gStatus = policy;
        gLateLoadRefused = (policy == garnet_pcibar::CompatStatus::kLateLoad);
        IOLog("GarnetPCIBarCompat: inactive (%s, pciDevs=%u, cfgs=%u)\n",
              garnet_pcibar::status_name(policy), pciDevCount, cfgCount);
        return KERN_SUCCESS;
    }

    const uintptr_t anchor = reinterpret_cast<uintptr_t>(&iopci_bridge_initialize);
    if (anchor < 0xFFFFFF8000000000ULL) {
        gStatus = garnet_pcibar::CompatStatus::kHeaderNotFound;
        IOLog("GarnetPCIBarCompat: invalid anchor address %p\n",
              reinterpret_cast<void *>(anchor));
        return KERN_SUCCESS;
    }

    const uintptr_t span = static_cast<uintptr_t>(garnet_pcibar::kMaxBackscanPages - 1u) *
                           static_cast<uintptr_t>(garnet_pcibar::kPageSize);
    const uintptr_t page = anchor & ~static_cast<uintptr_t>(garnet_pcibar::kPageMask);
    const uintptr_t minReadable = (page >= span) ? (page - span) : 0ULL;
    const void *mh = garnet_pcibar::find_mach_header_backward(anchor, minReadable, UINTPTR_MAX);
    if (!mh) {
        gStatus = garnet_pcibar::CompatStatus::kHeaderNotFound;
        IOLog("GarnetPCIBarCompat: failed to locate IOPCIFamily mach_header_64\n");
        return KERN_SUCCESS;
    }

    const garnet_pcibar::CompatStatus st =
        garnet_pcibar::verify_iopcifamily_target(mh, SIZE_MAX, anchor, gTarget);
    if (st != garnet_pcibar::CompatStatus::kInstalled) {
        gStatus = st;
        IOLog("GarnetPCIBarCompat: verification failed (%s); no patch applied\n",
              garnet_pcibar::status_name(st));
        return KERN_SUCCESS;
    }

    gOps.configRead16 = reinterpret_cast<Read16Fn>(gTarget.fnConfigRead16);
    gOps.configWrite16 = reinterpret_cast<Write16Fn>(gTarget.fnConfigWrite16);
    gOps.configRead32 = reinterpret_cast<Read32Fn>(gTarget.fnConfigRead32);
    gOps.configWrite32 = reinterpret_cast<Write32Fn>(gTarget.fnConfigWrite32);
    gOps.rangeInit = reinterpret_cast<RangeInitFn>(gTarget.fnRangeInit);

    uint8_t trampBytes[garnet_pcibar::kStolenPrologueBytes + 14u] {};
    uint8_t detourBytes[garnet_pcibar::kStolenPrologueBytes] {};
    garnet_pcibar::encode_trampoline31(trampBytes, gTarget.stolenBytes, gTarget.resumeAddr);
    garnet_pcibar::encode_entry_detour17(
        detourBytes, reinterpret_cast<uint64_t>(&hooked_probeBaseAddressRegister));

    const uintptr_t trampAddr = reinterpret_cast<uintptr_t>(&garnet_pci_bar_compat_trampoline);
    gOrigProbe = reinterpret_cast<ProbeFn>(&garnet_pci_bar_compat_trampoline);

    install_detour_locked(trampAddr, trampBytes, sizeof(trampBytes),
                          gTarget.entryAddr, detourBytes, sizeof(detourBytes));
    __atomic_store_n(&gHookInstalled, true, __ATOMIC_RELEASE);
    gStatus = garnet_pcibar::CompatStatus::kInstalled;
    IOLog("GarnetPCIBarCompat: hook installed on IOPCIFamily entry=%p\n",
          reinterpret_cast<void *>(gTarget.entryAddr));
    return KERN_SUCCESS;
}

extern "C" kern_return_t garnet_pci_compat_kmod_stop(kmod_info_t *ki, void *data) {
    (void)ki;
    (void)data;

    if (__atomic_load_n(&gHookInstalled, __ATOMIC_ACQUIRE) && gTarget.entryAddr != 0u) {
        restore_detour_locked(gTarget.entryAddr, gTarget.stolenBytes,
                              garnet_pcibar::kStolenPrologueBytes);
        __atomic_store_n(&gHookInstalled, false, __ATOMIC_RELEASE);
        gStatus = garnet_pcibar::CompatStatus::kUninstalled;
    }
    return KERN_SUCCESS;
}

class GarnetPCIBarCompat : public IOService {
    OSDeclareDefaultStructors(GarnetPCIBarCompat)

public:
    bool start(IOService *provider) override;
    bool serializeProperties(OSSerialize *serialize) const override;
    IOReturn setProperties(OSObject *properties) override;

private:
    void publishDiagnostics();
};

#define super IOService
OSDefineMetaClassAndStructors(GarnetPCIBarCompat, IOService)

void GarnetPCIBarCompat::publishDiagnostics() {
    setProperty("GarnetPCIBarCompat,Status", garnet_pcibar::status_name(gStatus));
    setProperty("GarnetPCIBarCompat,HookInstalled",
                __atomic_load_n(&gHookInstalled, __ATOMIC_ACQUIRE));
    setProperty("GarnetPCIBarCompat,TargetUUIDVerified", gTarget.uuidVerified);
    setProperty("GarnetPCIBarCompat,InterceptCount",
                __atomic_load_n(&gCounters.interceptCount, __ATOMIC_RELAXED), 64);
    setProperty("GarnetPCIBarCompat,PreservedBar0Bytes",
                __atomic_load_n(&gCounters.preservedBar0Bytes, __ATOMIC_RELAXED), 64);
    setProperty("GarnetPCIBarCompat,DeviceID",
                static_cast<uint64_t>(__atomic_load_n(&gCounters.deviceId, __ATOMIC_RELAXED)), 32);
    setProperty("GarnetPCIBarCompat,PassthroughCount",
                __atomic_load_n(&gCounters.passthroughCount, __ATOMIC_RELAXED), 64);
    setProperty("GarnetPCIBarCompat,RefusedBridgeCount",
                __atomic_load_n(&gCounters.refusedBridgeCount, __ATOMIC_RELAXED), 64);
    setProperty("GarnetPCIBarCompat,LateLoadRefused", gLateLoadRefused);
}

bool GarnetPCIBarCompat::start(IOService *provider) {
    if (!super::start(provider)) return false;

    publishDiagnostics();
    registerService();
    return true;
}

bool GarnetPCIBarCompat::serializeProperties(OSSerialize *serialize) const {
    const_cast<GarnetPCIBarCompat *>(this)->publishDiagnostics();
    return super::serializeProperties(serialize);
}

IOReturn GarnetPCIBarCompat::setProperties(OSObject *properties) {
    (void)properties;
    return kIOReturnUnsupported;
}
