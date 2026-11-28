// VM-only provider for exercising the shipping accelerator's real IOKit entry.
// Allocates the shipping software nub; never creates a PCI device or GPU engine.
#include <IOKit/IOService.h>
#include <IOKit/IOUserClient.h>
#include <IOKit/IOLib.h>
#include <libkern/c++/OSMetaClass.h>
#include <pexpert/pexpert.h>
#include "GarnetMetalOps.h"
#define GARNET_R_IOGraphicsAccelerator2_271 IOMemoryDescriptor *
#include "IOAccelFamily2_decl.h"

class GarnetVMFixture : public IOService {
    OSDeclareDefaultStructors(GarnetVMFixture)
    IOService *nub;
    IOLock *lock;
public:
    bool start(IOService *provider) override;
    void stop(IOService *provider) override;
    void free() override;
    IOReturn publish(uint64_t *entryID);
    IOReturn withdraw();
    IOReturn status(uint64_t output[6]);
    IOReturn newUserClient(task_t task, void *securityID, UInt32 type, IOUserClient **handler) override;
};

class GarnetVMFixtureClient : public IOUserClient {
    OSDeclareDefaultStructors(GarnetVMFixtureClient)
    GarnetVMFixture *fixture;
public:
    bool initWithTask(task_t task, void *securityID, UInt32 type, OSDictionary *properties) override;
    bool start(IOService *provider) override;
    IOReturn clientClose() override;
    IOReturn externalMethod(uint32_t selector, IOExternalMethodArguments *args,
                           IOExternalMethodDispatch *dispatch, OSObject *target, void *reference) override;
};

OSDefineMetaClassAndStructors(GarnetVMFixture, IOService)
OSDefineMetaClassAndStructors(GarnetVMFixtureClient, IOUserClient)

bool GarnetVMFixture::start(IOService *provider) {
    uint32_t a, b, c, d, stage = UINT32_MAX;
    __asm__ volatile("cpuid" : "=a"(a), "=b"(b), "=c"(c), "=d"(d) : "a"(1), "c"(0));
    if (!(c & (1u << 31)) || !PE_parse_boot_argn("garnet-stage", &stage, sizeof stage) || stage != 0) return false;
    if (!IOService::start(provider)) return false;
    lock = IOLockAlloc();
    if (!lock) return false;
    setProperty("VMTestOnly", true);
    registerService();
    IOLog("garnetvmfixture: ready; software nub publication requires root selector 0\n");
    return true;
}

IOReturn GarnetVMFixture::publish(uint64_t *entryID) {
    IOLockLock(lock);
    if (nub) { IOLockUnlock(lock); return kIOReturnExclusiveAccess; }
    OSObject *object = OSMetaClass::allocClassWithName("GarnetMetalNub");
    IOService *created = OSDynamicCast(IOService, object);
    if (!created || !created->init()) {
        if (object) object->release();
        IOLockUnlock(lock);
        return kIOReturnNoMemory;
    }
    // Background Metal enumeration must decline this hardware-free provider.
    created->setProperty("Garnet,Ready", 0ull, 32);
    created->setProperty("VMTestOnly", true);
    static const char kFixtureModel[] = "Garnet VM software fixture";
    if (OSData *modelData = OSData::withBytes(kFixtureModel, static_cast<unsigned int>(sizeof(kFixtureModel)))) {
        created->setProperty("model", modelData);
        modelData->release();
    }
    if (!created->attach(this)) {
        created->release(); IOLockUnlock(lock); return kIOReturnInternalError;
    }
    nub = created;
    *entryID = nub->getRegistryEntryID();
    created->retain();
    IOLockUnlock(lock);
    created->registerService(kIOServiceSynchronous);
    created->release();
    IOLog("garnetvmfixture: published shipping GarnetMetalNub id=%llu Ready=0\n", *entryID);
    return kIOReturnSuccess;
}

IOReturn GarnetVMFixture::withdraw() {
    IOLockLock(lock);
    IOService *old = nub;
    nub = nullptr;
    IOLockUnlock(lock);
    if (!old) return kIOReturnNotFound;
    old->terminate(kIOServiceRequired | kIOServiceSynchronous);
    old->detach(this);
    old->release();
    IOLog("garnetvmfixture: withdrew software nub\n");
    return kIOReturnSuccess;
}

IOReturn GarnetVMFixture::status(uint64_t output[6]) {
    IOLockLock(lock);
    IOService *provider = nub;
    if (provider) provider->retain();
    IOLockUnlock(lock);
    if (!provider) return kIOReturnNotFound;
    const GarnetMetalOps *ops = nullptr;
    IOReturn result = provider->callPlatformFunction(GARNET_METAL_FN_SYMBOL, false, &ops, nullptr, nullptr, nullptr);
    if (result == kIOReturnSuccess && ops) {
        IORegistryEntry *entry = provider->getChildEntry(gIOServicePlane);
        auto *accelerator = entry ? static_cast<IOGraphicsAccelerator2 *>(entry->metaCast("IOGraphicsAccelerator2")) : nullptr;
        unsigned int stamps = 0;
        output[0] = ops->abi;
        output[1] = ops->size;
        output[2] = ops->kext_build;
        output[3] = ops->caps;
        output[4] = accelerator && accelerator->getStampMemory(&stamps) != nullptr;
        output[5] = stamps;
    } else if (result == kIOReturnSuccess) result = kIOReturnNotReady;
    provider->release();
    return result;
}

void GarnetVMFixture::stop(IOService *provider) {
    if (lock) (void)withdraw();
    IOService::stop(provider);
}

void GarnetVMFixture::free() {
    if (lock) { IOLockFree(lock); lock = nullptr; }
    IOService::free();
}

IOReturn GarnetVMFixture::newUserClient(task_t task, void *securityID, UInt32 type, IOUserClient **handler) {
    if (!handler || type != 0) return kIOReturnBadArgument;
    *handler = nullptr;
    auto *client = OSTypeAlloc(GarnetVMFixtureClient);
    if (!client) return kIOReturnNoMemory;
    if (!client->initWithTask(task, securityID, type, nullptr)) { client->release(); return kIOReturnNotPrivileged; }
    if (!client->attach(this)) { client->release(); return kIOReturnInternalError; }
    if (!client->start(this)) { client->detach(this); client->release(); return kIOReturnInternalError; }
    *handler = client;
    return kIOReturnSuccess;
}

bool GarnetVMFixtureClient::initWithTask(task_t task, void *securityID, UInt32 type, OSDictionary *properties) {
    return IOUserClient::initWithTask(task, securityID, type, properties) &&
        clientHasPrivilege(securityID, kIOClientPrivilegeAdministrator) == kIOReturnSuccess;
}

bool GarnetVMFixtureClient::start(IOService *provider) {
    fixture = OSDynamicCast(GarnetVMFixture, provider);
    return fixture && IOUserClient::start(provider);
}

IOReturn GarnetVMFixtureClient::clientClose() { terminate(); return kIOReturnSuccess; }

IOReturn GarnetVMFixtureClient::externalMethod(uint32_t selector, IOExternalMethodArguments *args,
                                             IOExternalMethodDispatch *, OSObject *, void *) {
    if (!args || !fixture) return kIOReturnNotReady;
    if (args->scalarInputCount || args->structureInputSize || args->structureOutputSize ||
        args->structureInputDescriptor || args->structureOutputDescriptor) return kIOReturnBadArgument;
    if (selector == 0) {
        if (args->scalarOutputCount != 1 || !args->scalarOutput) return kIOReturnBadArgument;
        return fixture->publish(args->scalarOutput);
    }
    if (selector == 1) {
        if (args->scalarOutputCount) return kIOReturnBadArgument;
        return fixture->withdraw();
    }
    if (selector == 2) {
        if (args->scalarOutputCount != 6 || !args->scalarOutput) return kIOReturnBadArgument;
        return fixture->status(args->scalarOutput);
    }
    return kIOReturnUnsupported;
}
