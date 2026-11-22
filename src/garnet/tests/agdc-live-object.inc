// VM-only constructor/init/release coverage. No attachment, start, publication,
// framebuffer or PCI substitutes. The generated body stops before attachment.
static uint32_t gObjectVendorCalls;
static uint32_t agdc_vendor(void *, uint32_t, unsigned long *, unsigned long,
                            unsigned long *, unsigned long *, void *) {
    ++gObjectVendorCalls;
    return static_cast<uint32_t>(kIOReturnUnsupported);
}
static constexpr unsigned kVtHdr = 2;
#include "agdc-production-object.h"

static void agdc_object_probe(IOService *probe, const OSMetaClass *meta, uintptr_t slide) {
    const uint32_t before = meta->getInstanceCount();
    const uint32_t status = agdc_construct_only(meta, slide, nullptr, nullptr, nullptr);
    uint32_t constructed = meta->getInstanceCount(), retain = 0;
    bool identity = false;
    if (status == 0 && gAg.obj) {
        identity = gAg.obj->getMetaClass() == meta;
        retain = static_cast<uint32_t>(gAg.obj->getRetainCount());
        gAg.obj->release();
        gAg.obj = nullptr;
    }
    const uint32_t after = meta->getInstanceCount();
    if (gAg.vtCopy) {
        IOFree(gAg.vtCopy, (kAgdcVtSlots + kVtHdr) * sizeof(void *));
        gAg.vtCopy = nullptr;
    }
    const bool passed = status == 0 && identity && retain == 1 &&
                        constructed == before + 1 && after == before && gObjectVendorCalls == 0;
    probe->setProperty("ObjectChecksPassed", passed);
    probe->setProperty("ObjectStatus", static_cast<uint64_t>(status), 32);
    probe->setProperty("ObjectCountBefore", static_cast<uint64_t>(before), 32);
    probe->setProperty("ObjectCountConstructed", static_cast<uint64_t>(constructed), 32);
    probe->setProperty("ObjectCountAfterRelease", static_cast<uint64_t>(after), 32);
    probe->setProperty("ObjectRetainCount", static_cast<uint64_t>(retain), 32);
    probe->setProperty("ObjectIdentityPassed", identity);
    probe->setProperty("ObjectVendorCalls", static_cast<uint64_t>(gObjectVendorCalls), 32);
    probe->setProperty("DisplayAttachmentAttempted", false);
    IOLog("rdna4-agdc-object: %s status=%u identity=%u retain=%u count=%u/%u/%u vendor=%u; no attach/start/publish\n",
          passed ? "PASS" : "FAIL", status, identity, retain, before, constructed, after, gObjectVendorCalls);
}
