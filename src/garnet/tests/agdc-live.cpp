// VM-only probe of shipping AGDC guards, with optional isolated object checks.
#include <IOKit/IOService.h>
#include <IOKit/IOLib.h>
#include <libkern/c++/OSMetaClass.h>
#include <pexpert/pexpert.h>
#include "native_agdc_pure.h"
#define DPGLOG(...) IOLog(__VA_ARGS__)
#include "agdc-production-guards.h"
#ifdef RDNA4_AGDC_OBJECT_TEST
#include "agdc-live-object.inc"
#elif defined(RDNA4_AGDC_SERVICE_TEST)
#include <kern/thread_call.h>
#include "agdc-live-service.inc"
#endif

class RDNA4AGDCLive : public IOService {
    OSDeclareDefaultStructors(RDNA4AGDCLive)
public:
    bool start(IOService *provider) override;
#ifdef RDNA4_AGDC_SERVICE_TEST
    void stop(IOService *provider) override;
private:
    const OSMetaClass *serviceMeta_ = nullptr;
    uintptr_t serviceSlide_ = 0;
    thread_call_t serviceCall_ = nullptr;
    IONotifier *serviceNotifier_ = nullptr;
    IONotifier *publishedNotifier_ = nullptr;
    bool serviceRan_ = false;
    static bool wranglerReady(void *target, void *, IOService *, IONotifier *);
    static bool servicePublished(void *target, void *, IOService *, IONotifier *);
    static void runService(thread_call_param_t target, thread_call_param_t);
#endif
};
OSDefineMetaClassAndStructors(RDNA4AGDCLive, IOService)

#ifdef RDNA4_AGDC_SERVICE_TEST
bool RDNA4AGDCLive::wranglerReady(void *target, void *, IOService *, IONotifier *) {
    auto *probe = static_cast<RDNA4AGDCLive *>(target);
    probe->retain();
    if (thread_call_enter(probe->serviceCall_)) probe->release();
    return true;
}

bool RDNA4AGDCLive::servicePublished(void *target, void *, IOService *service, IONotifier *) {
    auto *probe = static_cast<RDNA4AGDCLive *>(target);
    const bool registered = service == gAg.obj && service->getRegistryEntryID() == gAg.regId;
    const bool passed = registered && gAg.published && gAg.startOk &&
                        service->getMetaClass() == probe->serviceMeta_ && gAg.vendorInfo > 0 && gAg.gpuCap > 0;
    probe->setProperty("ServiceRegistered", registered);
    probe->setProperty("ServiceChecksPassed", passed);
    IOLog("garnet-agdc-service: registration %s regID=%#llx; actual VM service, no native GPU\n",
          passed ? "PASS" : "FAIL", gAg.regId);
    return true;
}

void RDNA4AGDCLive::runService(thread_call_param_t target, thread_call_param_t) {
    auto *probe = static_cast<RDNA4AGDCLive *>(target);
    if (!probe->serviceRan_) {
        probe->serviceRan_ = true;
        probe->setProperty("ServiceWaitingForWrangler", false);
        agdc_service_probe(probe, probe->getProvider(), probe->serviceMeta_, probe->serviceSlide_);
        if (gAg.published) {
            OSDictionary *matching = IOService::registryEntryIDMatching(gAg.regId);
            if (matching) {
                probe->publishedNotifier_ = IOService::addMatchingNotification(gIOFirstMatchNotification,
                    matching, &RDNA4AGDCLive::servicePublished, probe);
                matching->release();
            }
        }
    }
    probe->release();
}

void RDNA4AGDCLive::stop(IOService *provider) {
    if (serviceNotifier_) {
        serviceNotifier_->remove();
        serviceNotifier_ = nullptr;
    }
    if (publishedNotifier_) {
        publishedNotifier_->remove();
        publishedNotifier_ = nullptr;
    }
    if (serviceCall_) {
        if (thread_call_cancel(serviceCall_)) release();
        thread_call_free(serviceCall_);
        serviceCall_ = nullptr;
    }
    IOService::stop(provider);
}
#endif

static const OSMetaClass *lookup(const char *name) {
    const OSSymbol *symbol = OSSymbol::withCString(name);
    const OSMetaClass *meta = symbol ? OSMetaClass::getMetaClassWithName(symbol) : nullptr;
    if (symbol) symbol->release();
    return meta;
}

bool RDNA4AGDCLive::start(IOService *provider) {
    uint32_t a, b, c, d, stage = UINT32_MAX;
    __asm__ volatile("cpuid" : "=a"(a), "=b"(b), "=c"(c), "=d"(d) : "a"(1), "c"(0));
    if (!(c & (1u << 31)) || !PE_parse_boot_argn("garnet-stage", &stage, sizeof stage) || stage != 0) return false;
    if (!IOService::start(provider)) return false;
    (void)kAgdcVtSlots;
    const OSMetaClass *agdc = lookup("AppleGraphicsDeviceControl"), *pipe = lookup("IOAccelDisplayPipe");
    uint32_t status = !agdc ? rdna4agdc::kNoAgdc : !pipe ? rdna4agdc::kNoFamily : rdna4agdc::kPublished;
    uintptr_t slide = 0;
    if (status == rdna4agdc::kPublished) {
        const auto result = rdna4agdc::slide_decide(reinterpret_cast<uintptr_t>(agdc), reinterpret_cast<uintptr_t>(pipe));
        status = result.status;
        slide = result.slide;
        if (status == rdna4agdc::kPublished) status = agdc_class_checks(agdc, slide);
    }
    setProperty("VMTestOnly", true);
#ifdef RDNA4_AGDC_OBJECT_TEST
    setProperty("ReadOnlyProbe", false);
    if (status == rdna4agdc::kPublished) agdc_object_probe(this, agdc, slide);
#elif defined(RDNA4_AGDC_SERVICE_TEST)
    setProperty("ReadOnlyProbe", false);
    if (status == rdna4agdc::kPublished) {
        serviceMeta_ = agdc;
        serviceSlide_ = slide;
        serviceCall_ = thread_call_allocate(&RDNA4AGDCLive::runService, this);
        OSDictionary *matching = IOService::serviceMatching("AppleGPUWrangler");
        if (!serviceCall_ || !matching) {
            if (matching) matching->release();
            if (serviceCall_) thread_call_free(serviceCall_);
            serviceCall_ = nullptr;
            IOService::stop(provider);
            return false;
        }
        setProperty("ServiceWaitingForWrangler", true);
        // Start outside the matching callback after Wrangler has fully matched.
        serviceNotifier_ = IOService::addMatchingNotification(gIOFirstMatchNotification,
            matching, &RDNA4AGDCLive::wranglerReady, this);
        matching->release();
        if (!serviceNotifier_) {
            thread_call_free(serviceCall_);
            serviceCall_ = nullptr;
            IOService::stop(provider);
            return false;
        }
    }
#else
    setProperty("ReadOnlyProbe", true);
#endif
    setProperty("Status", static_cast<uint64_t>(status), 32);
    setProperty("StatusName", status == 0 ? "GUARDS_PASS" : rdna4agdc::status_name(status));
    setProperty("ClassSize", static_cast<uint64_t>(agdc ? agdc->getClassSize() : 0), 32);
    setProperty("ProductionSourceSHA256", RDNA4_AGDC_SOURCE_SHA256);
    registerService();
    IOLog("garnet-agdc-live: guards status=%u (%s), size=%#x; VM-only probe\n",
          status, status == 0 ? "GUARDS_PASS" : rdna4agdc::status_name(status), agdc ? agdc->getClassSize() : 0);
    return true;
}
