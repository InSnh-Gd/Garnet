#include "native_display_identity.h"
#include "native_display_identity_pure.h"
#include "amdgpu_asic.h"
#include "../GarnetDriver.hpp"

namespace {

struct RetainedService {
    IOService *service;
    ~RetainedService() { if (service) service->release(); }
};

} // namespace

bool rdna4_display_primary(IOService **outRetainedFb, rdna4peer::PrimaryView *out,
                          uint64_t *outGeneration) {
    if (outRetainedFb) *outRetainedFb = nullptr;
    if (out) *out = {};
    if (outGeneration) *outGeneration = 0;
    if (!outRetainedFb) return false;
    RetainedService retainedPeer { garnet_driver_service_retained() };
    auto *peer = OSDynamicCast(GarnetDriver, retainedPeer.service);
    if (!peer) return false;
    auto *pci = OSDynamicCast(IOPCIDevice, peer->getProvider());
    if (!pci) return false;
    pci->retain();
    RetainedService retainedPci { pci };
    const uint32_t identity = pci->configRead32(kIOPCIConfigVendorID);
    const auto vendor = static_cast<uint16_t>(identity);
    const auto device = static_cast<uint16_t>(identity >> 16);
    const amdgpu::AsicInfo *asic = amdgpu::asic_from_pci(vendor, device);
    if (!asic) return false;
    IODeviceMemory *bar = pci->getDeviceMemoryWithRegister(kIOPCIConfigBaseAddress0);
    if (!bar) return false;
    const rdna4peer::Expected expected { vendor, device,
        static_cast<uint32_t>(asic->id), pci->getRegistryEntryID(),
        bar->getPhysicalAddress(), bar->getLength() };

    rdna4peer::Snapshot snapshot {};
    rdna4peer::PrimaryView primary {};
    IOService *fb = nullptr;
    if (!peer->queryPrimaryFramebuffer(&snapshot, &primary, &fb)) {
        if (fb) fb->release();
        return false;
    }
    bool valid = fb && rdna4primary::valid_snapshot_view(snapshot, primary, expected,
                                                         fb->getRegistryEntryID());
    // Revalidate the complete view so a handoff cannot mix console or mode snapshots.
    if (valid) {
        rdna4peer::Snapshot currentSnap {};
        rdna4peer::PrimaryView currentPrimary {};
        valid = peer->queryPrimaryFramebuffer(&currentSnap, &currentPrimary, nullptr) &&
                currentSnap.generation == snapshot.generation &&
                rdna4primary::valid_snapshot_view(currentSnap, currentPrimary, expected,
                                                  fb->getRegistryEntryID()) &&
                rdna4primary::valid(currentPrimary, expected,
                    fb->getRegistryEntryID(), snapshot.console.offset,
                    snapshot.console.length, snapshot.consoleWidth,
                    snapshot.consoleHeight, snapshot.consoleRowBytes);
    }
    if (!valid) {
        if (fb) fb->release();
        return false;
    }
    *outRetainedFb = fb;
    if (out) *out = primary;
    if (outGeneration) *outGeneration = primary.generation;
    return true;
}

bool rdna4_display_is_primary(IOService *fb) {
    if (!fb) return false;
    IOService *primary = nullptr;
    if (!rdna4_display_primary(&primary)) return false;
    const bool same = primary == fb;
    primary->release();
    return same;
}
