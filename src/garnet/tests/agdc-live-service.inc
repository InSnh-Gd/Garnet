// VM-only test of the shared AGDC builder and vendor callback with a real VM
// framebuffer. It does not run native_flow or admit a VM GPU to the driver.
#include <sys/proc.h>
#include "amd/native_m6_pure.h"

static constexpr unsigned kVtHdr = 2;
static struct { uint32_t width, height; } gRa;
static uint32_t gVMRasterRefusals;
static bool rdna4_scanout_live_dims(uint32_t *, uint32_t *) { return false; }
namespace rdna4dcn {
static uint32_t liveRaster(uint32_t *, uint32_t *, uint32_t *, uint32_t *,
                           uint32_t *, uint32_t *, uint32_t *, uint32_t *, uint64_t *) {
    ++gVMRasterRefusals;
    return 1;
}
}
static const char *dpg_class(const void *object) {
    return static_cast<const OSObject *>(object)->getMetaClass()->getClassName();
}
static bool dpg_kptr(const void *object) {
    return rdna4agdc::kptr_ok(reinterpret_cast<uintptr_t>(object));
}
// The extracted production callback and builder below are unmodified. These
// accessors are the VM boundary; no target-GPU timing is fabricated.
#include "agdc-production-service.h"

static void agdc_service_probe(IOService *probe, IOService *framebuffer,
                               const OSMetaClass *meta, uintptr_t slide) {
    IORegistryEntry *parent = framebuffer->getParentEntry(gIOServicePlane);
    const bool havePCI = parent && parent->metaCast("IOPCIDevice");
    const bool haveWrangler = agdc_have_wrangler();
    probe->setProperty("VMFramebufferClass", framebuffer->getMetaClass()->getClassName());
    probe->setProperty("VMFramebufferID", framebuffer->getRegistryEntryID(), 64);
    probe->setProperty("VMPCIID", havePCI ? parent->getRegistryEntryID() : 0, 64);
    probe->setProperty("VMHasPCI", havePCI);
    probe->setProperty("VMHasWrangler", haveWrangler);
    probe->setProperty("NativeFlowExecuted", false);
    if (!havePCI || !haveWrangler) {
        probe->setProperty("ServiceChecksPassed", false);
        return;
    }
    const uint32_t status = agdc_build_locked(meta, slide, framebuffer,
        static_cast<IOService *>(parent), probe);
    const bool identity = gAg.obj && gAg.obj->getMetaClass() == meta;
    const bool passed = status == 0 && gAg.published && gAg.startOk && identity &&
                        gAg.vendorInfo > 0 && gAg.gpuCap > 0;
    probe->setProperty("ServiceStartChecksPassed", passed);
    probe->setProperty("ServiceStatus", static_cast<uint64_t>(status), 32);
    probe->setProperty("ServiceID", gAg.regId, 64);
    probe->setProperty("ServiceStartOK", gAg.startOk != 0);
    probe->setProperty("ServiceVendorInfoCalls", gAg.vendorInfo, 64);
    probe->setProperty("ServiceGPUCapabilityCalls", gAg.gpuCap, 64);
    probe->setProperty("ServiceRasterRefusals", static_cast<uint64_t>(gVMRasterRefusals), 32);
    if (gAg.obj) gAg.obj->setProperty("RDNA4,VMTestOnly", true);
    IOLog("rdna4-agdc-service: start %s status=%u start=%u vendor=%llu gpu=%llu; awaiting service registration, VM framebuffer only\n",
          passed ? "PASS" : "FAIL", status, gAg.startOk, gAg.vendorInfo, gAg.gpuCap);
}
