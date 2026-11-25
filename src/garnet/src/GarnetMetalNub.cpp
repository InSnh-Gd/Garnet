//
//  GarnetMetalNub.cpp - see the header. Pure logic: amd/native_metal_pure.h (tests/native_metal_test.cpp).
//
//  What this file WRITES: nothing on the hardware. It allocates one 4 KiB physically-contiguous system-memory page (the stamp page, on the aux kext's
//  device_open hook), one nub object and its property dictionary. It registers the nub in the IORegistry under GarnetDriver.
//
#include "GarnetMetalNub.hpp"
#include "GarnetDriver.hpp"
#include "GarnetMetalOps.h"
#include "garnet_version.h"
#include "GarnetNativeClient.hpp"   // 0.0.656 (G6): op_native_open creates the client on the accelerator route
#include "amd/native_metal_pure.h"
#include "amd/native_s1b.h"
#include "amd/native_s1c.h"
#include "amd/garnet_multisession_pure.h"
#include "amd/native_disp.h"         // 0.0.613: the display pipe glue (behind boot-arg garnet-metal-disp=1)
#include "amd/native_disp_pure.h"
#include "amd/amdgpu_log.h"
#include "amd/amdgpu_asic.h"

#include <IOKit/IOLib.h>
#include <IOKit/IOBufferMemoryDescriptor.h>
#include <IOKit/pci/IOPCIDevice.h>
#include <libkern/OSAtomic.h>
#include <pexpert/pexpert.h>

#define MNLOG(fmt, ...) AMDGPU_LOG("metal", fmt, ##__VA_ARGS__)

static_assert(garnetmetal::kNotReady == (uint32_t)kIOReturnNotReady && garnetmetal::kBadArg == (uint32_t)kIOReturnBadArgument && garnetmetal::kNotFound == (uint32_t)kIOReturnNotFound &&
              garnetmetal::kUnsupported == (uint32_t)kIOReturnUnsupported && garnetmetal::kExclusive == (uint32_t)kIOReturnExclusiveAccess &&
              garnetmetal::kBusy == (uint32_t)kIOReturnBusy && garnetmetal::kNoMemory == (uint32_t)kIOReturnNoMemory && garnetmetal::kOk == (uint32_t)kIOReturnSuccess,
              "the pure header's codes are the IOReturn.h values");

static_assert(GARNET_METAL_UC_GARNETN == GARNETN_UC_TYPE, "the aux kext's copy of the 'GRNT' type equals the user-client ABI's");

OSDefineMetaClassAndStructors(GarnetMetalNub, IOService)

// 0.0.613: the mask the aux kext's factories see (op_factory_mask below, and the display glue's fact check): the boot-arg garnet-metal-fact mask, read on every call as before, plus the
// fact bits `pipe adopt` turned on at run time. With boot-arg garnet-metal-disp absent the OR is a no-op (rdna4disp::fact_mask returns the base mask): the 0.0.612 value, bit for bit.
uint32_t garnetmetal_factory_mask_now(void) {
	uint32_t v = 0;
	const bool present = PE_parse_boot_argn("garnet-metal-fact", &v, sizeof(v));
	return rdna4disp::fact_mask(rdna4disp_latched_on(), garnetmetal::factory_mask(present, v), rdna4disp_fact_bits());
}

// ---- state (one nub per boot at a time) -----------------------------------------------------------------------------------------------------------------
namespace {
IOLock                *gLock;         // created on first publish
GarnetMetalNub        *gNub;          // our reference while Published
volatile uint32_t      gState = 0;    // garnetmetal::State
volatile uint32_t      gEverPublished = 0;   // 0.0.653: sticky, 1 once a nub was published this boot (fbpublish refuses after that)
volatile uint32_t      gReadyNo = 0;  // 0.0.612: sticky, 1 once the HUNG latch tripped this boot (Garnet,Ready is 0 from then on, never 1 again)
struct Dev {                          // the per-device state the aux kext's device_open hook gets
	uint32_t                    magic;
	IOBufferMemoryDescriptor   *md;
	volatile uint32_t          *va;
	void                       *accel;   // 0.0.656 (G6): the accelerator device_open was called for (identity only, NOT retained): the accelerator route's one valid provider
};
constexpr uint32_t kDevMagic = 0x4D44564Eu;   // 'NVDM'
Dev *gDev;                            // one accelerator per boot

const char kAccelName[] = "Garnet Accelerator";   // IOAccelConfig +0: a pointer into THIS kext, which outlives the aux kext's accelerator

// ---- the ops table's hooks (GarnetMetalOps.h). The aux kext calls these; each decision is garnetmetal:: pure code. --------------------------------------------
void *op_device_open(void *accel, void *nub) {
	if (!nub || !accel || gDev) return nullptr;                                  // one device (0.0.656: and an accelerator to bind it to)
	const mach_vm_address_t mask = garnetmetal::kStampPhysMask;
	IOBufferMemoryDescriptor *md = IOBufferMemoryDescriptor::inTaskWithPhysicalMask(
		kernel_task, kIODirectionInOut | kIOMemoryPhysicallyContiguous | kIOMemoryMapperNone, garnetmetal::kStampBytes, mask);
	if (!md) { MNLOG("device_open: stamp page allocation failed"); return nullptr; }
	if (md->prepare() != kIOReturnSuccess) { md->release(); return nullptr; }
	IOByteCount seg = 0;
	const addr64_t phys = md->getPhysicalSegment(0, &seg, kIOMemoryMapperNone);
	void *cpu = md->getBytesNoCopy();
	if (!cpu || !garnetmetal::stamp_page_ok(phys, seg, md->getLength())) {
		MNLOG("device_open: stamp page unusable (phys %#llx seg %llu len %llu)", (unsigned long long)phys, (unsigned long long)seg, (unsigned long long)md->getLength());
		md->complete(); md->release(); return nullptr;
	}
	bzero(cpu, garnetmetal::kStampBytes);                                             // stamps stay 0: nothing is ever submitted at #9
	Dev *d = (Dev *)IOMalloc(sizeof(Dev));
	if (!d) { md->complete(); md->release(); return nullptr; }
	d->magic = kDevMagic; d->md = md; d->va = (volatile uint32_t *)cpu; d->accel = accel;
	gDev = d;
	MNLOG("device_open: stamp page phys %#llx kva %p", (unsigned long long)phys, cpu);
	return d;
}
void op_device_close(void *ctx) {
	Dev *d = (Dev *)ctx;
	if (!d || d != gDev || d->magic != kDevMagic) return;                          // idempotent: only the live device, once
	gDev = nullptr;
	d->magic = 0; d->accel = nullptr;
	if (d->md) { d->md->complete(); d->md->release(); d->md = nullptr; }
	IOFree(d, sizeof(Dev));
	MNLOG("device_close");
}
int op_populate_config(uint8_t *cfg, uint32_t bytes) {
	const int rc = garnetmetal::config_populate(cfg, bytes, (uint64_t)(uintptr_t)kAccelName);
	if (rc != 0) { MNLOG("populate_config: REFUSED rc %d (a limit the family default left at zero would cap every IOSurface for the boot)", rc); return rc; }
	// (0.0.611: config_populate fills a local copy and copies it into cfg only when the static check passed, so a refusal leaves the family defaults in cfg.)
	// The whole 0x90 bytes and the values IOSurfaceRoot::updateLimits will take as system-wide minimums (NATIVE-S3 review SHOULD-FIX).
	MNLOG("populate_config: ok; IOSurface limits +0x30 %llu +0x32 %llu +0x4c %llu +0x50 %llu +0x54 %llu, +0x47 (type-4 pipe) %u",
	      (unsigned long long)garnetmetal::ld_le(cfg + garnetmetal::kOffLim30, 2), (unsigned long long)garnetmetal::ld_le(cfg + garnetmetal::kOffLim32, 2),
	      (unsigned long long)garnetmetal::ld_le(cfg + garnetmetal::kOffLim4c, 4), (unsigned long long)garnetmetal::ld_le(cfg + garnetmetal::kOffLim50, 4),
	      (unsigned long long)garnetmetal::ld_le(cfg + garnetmetal::kOffLim54, 4), (unsigned)cfg[garnetmetal::kOffPipeUC]);
	for (uint32_t o = 0; o < garnetmetal::kCfgBytes; o += 0x30)
		MNLOG("populate_config: cfg[%#x..] %016llx %016llx %016llx %016llx %016llx %016llx", o,
		      (unsigned long long)garnetmetal::ld_le(cfg + o, 8), (unsigned long long)garnetmetal::ld_le(cfg + o + 8, 8), (unsigned long long)garnetmetal::ld_le(cfg + o + 16, 8),
		      (unsigned long long)garnetmetal::ld_le(cfg + o + 24, 8), (unsigned long long)garnetmetal::ld_le(cfg + o + 32, 8), (unsigned long long)garnetmetal::ld_le(cfg + o + 40, 8));
	return rc;
}
void *op_stamp_memory(void *ctx, uint32_t *n) {
	Dev *d = (Dev *)ctx;
	if (n) *n = 0;
	return (d && d == gDev && d->magic == kDevMagic) ? (void *)d->md : nullptr;
}
volatile uint32_t *op_stamp_va(void *ctx) {
	Dev *d = (Dev *)ctx;
	return (d && d == gDev && d->magic == kDevMagic) ? d->va : nullptr;
}
int op_task_window(void *ctx, uint32_t kind, uint64_t *size, uint64_t *reserve) {
	(void)ctx;
	return garnetmetal::task_window(kind, size, reserve);
}
uint32_t op_factory_mask(void *ctx) {
	(void)ctx;
	return garnetmetal_factory_mask_now();
}
int op_mm_hook(void *ctx, uint32_t op, void *obj) {
	(void)ctx; (void)op; (void)obj;
	return (int)kIOReturnUnsupported;                                              // no map / unmap entry point exists before #10: the aux memory map returns false
}
void op_trace(uint32_t event, uint64_t a, uint64_t b) {
	MNLOG("aux trace: event %u a %#llx b %#llx", event, (unsigned long long)a, (unsigned long long)b);
	rdna4disp_on_trace(event, a, b);                                                 // 0.0.613: the display pipe's trace events (a no-op unless boot-arg garnet-metal-disp=1)
}

// The generic virtual hook (0.0.611): the optional VidMemory / Resource output-parameter slots are handled only when their factory bit is on (default mask 0 = nothing
// is handled, every aux trampoline takes its default); the rest is the trace. Logging is rate limited per (cls, slot) (pure code: garnetmetal::vhook_count / _log_kind).
static garnetmetal::VhookCounts gVhCounts;
int op_vhook(void *ctx, uint32_t cls, uint32_t slot, void *self, const uint64_t *args, uint32_t nargs, uint64_t *ret) {
	(void)ctx;
	const uint32_t n = garnetmetal::vhook_count(gVhCounts, cls, slot);
	uint32_t mask = 0;
	if (cls == GARNET_VC_VIDMEMORY || cls == GARNET_VC_RESOURCE) mask = op_factory_mask(nullptr);
	int h = 0;
	if (cls == GARNET_VC_RESOURCE && slot == 62u) h = rdna4disp_res62(self, args, nargs, ret);   // 0.0.613: type 0xC0 surfaces (11h.1 F3); 0 at once unless boot-arg garnet-metal-disp=1
	if (!h) h = garnetmetal::vhook_handle(mask, cls, slot, args, nargs, ret, garnetmetal::kKernelMin);
	const uint32_t k = garnetmetal::vhook_log_kind(n);
	if (k == garnetmetal::kVhLogFull) MNLOG("vhook: cls %u slot %u nargs %u call %u: %s", cls, slot, nargs, n, h ? "handled" : "not handled (default)");
	else if (k == garnetmetal::kVhLogCount) MNLOG("vhook: cls %u slot %u nargs %u: %u calls so far (last %s)", cls, slot, nargs, n, h ? "handled" : "default");
	return h;
}

// 0.0.656 (G6, ops ABI 3): the accelerator's newUserClient hands a 'GRNT' open here. The client it returns is the SAME IOAccelGarnetNativeClient WindowServer gets, attached to the accelerator (route kRouteAccelerator),
// with GarnetDriver as its owner (all engine state). Admission is the client's own initWithTask (allow-list / root / WindowServer identity, kauth on this thread); nothing is decided here beyond identity.
// Lock order: gLock is NOT held across create (create takes the client lock, and hungLatched takes gLock under it): the nub is retained under the lock and the lock is dropped first.
int32_t op_native_open(void *ctx, void *accel, void *task, void *securityID, uint32_t type, void **handler) {
	if (!handler) return (int32_t)kIOReturnBadArgument;
	Dev *d = (Dev *)ctx;
	const bool live = d != nullptr && d == __atomic_load_n(&gDev, __ATOMIC_ACQUIRE) && d->magic == kDevMagic;
	const bool isAccel = live && accel != nullptr && accel == d->accel;
	GarnetMetalNub *nub = nullptr;
	if (gLock) { IOLockLock(gLock); nub = gNub; if (nub) nub->retain(); IOLockUnlock(gLock); }
	const uint32_t v = garnetmetal::native_open_verdict(type == GARNET_METAL_UC_GARNETN, live, isAccel, nub != nullptr);
	if (v != garnetmetal::kOk) { if (nub) nub->release(); return (int32_t)v; }
	GarnetDriver *owner = OSDynamicCast(GarnetDriver, nub->getProvider());
	IOReturn rc = kIOReturnNotReady;
	if (owner) {
		IOUserClient *uc = nullptr;
		rc = IOAccelGarnetNativeClient::create(owner, static_cast<IOService *>(accel), rdna4native::policy::kRouteAccelerator, (task_t)task, securityID, type, nullptr, &uc);
		if (rc == kIOReturnSuccess) { if (uc) *handler = uc; else rc = kIOReturnInternalError; }
	}
	nub->release();
	return (int32_t)rc;
}

// 0.0.613 (ABI 2): the display-pipe hook and the PCI getter (an internal design note section 3.4). The aux kext calls them only from a table of abi >= 2 that carries GARNET_DISP_F_ON.
int op_disp_hook(void *ctx, uint32_t cls, uint32_t slot, void *self, const uint64_t *args, uint32_t nargs, uint64_t *ret) { return rdna4disp_hook(ctx, cls, slot, self, args, nargs, ret); }
void *op_pci_device(void *ctx) { return rdna4disp_pci_device(ctx); }

// TWO tables, the one published chosen by the latch (rdna4disp::ops_shape): with the display flag ONLY with boot-arg garnet-metal-disp=1 (gOps); otherwise gOpsOff, whose display members are zero (the aux kext then takes every
// 0.0.2 display default). Both are ABI 3 / 152 bytes since 0.0.656 (G6): native_open is the last member and carries its own capability bit.
const GarnetMetalOps gOps = {
	GARNET_METAL_OPS_MAGIC, GARNET_METAL_ABI, (uint32_t)sizeof(GarnetMetalOps), GARNET_KEXT_BUILD, garnetmetal::kOpsFlags, 0u,
	op_device_open, op_device_close, op_populate_config, op_stamp_memory, op_stamp_va, op_task_window,
	op_factory_mask, op_mm_hook, op_trace,
	op_vhook, garnetmetal::kOpsCaps, 0,
	GARNET_DISP_F_ON, 0u, op_disp_hook, op_pci_device,
	op_native_open,
};
// 0.0.656: the OFF table is ABI 3 too (native_open must exist with the display OFF, the normal case): the display members are zero (flags 0, no hook, no PCI getter), which is "display off" to every aux kext.
const GarnetMetalOps gOpsOff = {
	GARNET_METAL_OPS_MAGIC, GARNET_METAL_ABI, (uint32_t)sizeof(GarnetMetalOps), GARNET_KEXT_BUILD, garnetmetal::kOpsFlags, 0u,
	op_device_open, op_device_close, op_populate_config, op_stamp_memory, op_stamp_va, op_task_window,
	op_factory_mask, op_mm_hook, op_trace,
	op_vhook, garnetmetal::kOpsCaps, 0,
	0u, 0u, nullptr, nullptr,
	op_native_open,
};

void ensure_lock() {
	if (gLock) return;
	IOLock *l = IOLockAlloc();
	if (l && !OSCompareAndSwapPtr(nullptr, l, &gLock)) IOLockFree(l);
}

garnetmetal::GateIn gate_now(uint64_t flags, bool wantBootArg) {
	garnetmetal::GateIn g {};
	const amdgpu::NativeS1bState &s = amdgpu::native_s1b_state();
	g.flags = flags;
	g.hello = amdgpu::n1c_hello_done();
	g.s1bGateOn = (s.gate == rdna4native::kGateOn);
	g.s1bRan = s.ran; g.s1bPositive = s.positivePass; g.s1bStopped = s.stopped;
	g.hung = amdgpu::n1c_hung();
	g.m6 = rdna4m6_latched_on();                                                       // 0.0.659 (M6 Stage 1a, R3): boot-arg garnet-m6 == 1 (latched): the display-nub interlock below is lifted
	g.dispNub = rdna4fb_nub_exists();                                                 // 0.0.652 (M5): a GarnetDisplayNub is published: the Metal nub (and the display machine behind it) is REFUSED (garnetmetal::publish_verdict)
	uint32_t v = 0;
	g.bootarg = wantBootArg && PE_parse_boot_argn("garnet-metal", &v, sizeof(v)) && v == 1u;
	return g;
}
} // namespace

bool garnetmetal_nub_published(void) { return __atomic_load_n(&gEverPublished, __ATOMIC_ACQUIRE) != 0u; }   // 0.0.653

// ---- the selectors --------------------------------------------------------------------------------------------------------------------------------------
IOReturn GarnetMetalNub::selectorPublish(GarnetDriver *owner, uint64_t flags, uint64_t out[4]) {
	if (!owner || !out) return kIOReturnBadArgument;
	ensure_lock();
	if (!gLock) return kIOReturnNoMemory;
	IOLockLock(gLock);
	const garnetmetal::GateIn g = gate_now(flags, true);
	const uint32_t verdict = garnetmetal::publish_verdict(g, (garnetmetal::State)gState);
	if (verdict != garnetmetal::kOk) {
		IOLockUnlock(gLock);
		MNLOG("nub publish refused: %#x (hello %d flags %llu bootarg %d gate %d ran %d positive %d stopped %d hung %d state %u)", verdict, (int)g.hello, (unsigned long long)flags,
		      (int)g.bootarg, (int)g.s1bGateOn, (int)g.s1bRan, (int)g.s1bPositive, (int)g.s1bStopped, (int)g.hung, (unsigned)gState);
		return (IOReturn)verdict;
	}
	GarnetMetalNub *nub = OSTypeAlloc(GarnetMetalNub);
	OSDictionary *props = OSDictionary::withCapacity(4);
	IOReturn rc = kIOReturnNoMemory;
	if (nub && props && nub->init(props)) {
		IOPCIDevice *pci = OSDynamicCast(IOPCIDevice, owner->getProvider());
		char modelName[64];
		snprintf(modelName, sizeof(modelName), "AMD Radeon %s",
		         amdgpu::asic_pci_name(amdgpu::asic_current(), pci ? pci->configRead16(kIOPCIConfigDeviceID) : 0));
		OSString *model = OSString::withCString(modelName);
		const rdna4disp::OpsShape shape = rdna4disp::ops_shape(rdna4disp_latched_on());   // 0.0.613: OFF reports what 0.0.612 did (ABI 1, 120 bytes)
		OSNumber *abi = OSNumber::withNumber((uint64_t)shape.abi, 32);
		OSNumber *va = OSNumber::withNumber((uint64_t)garnetmetal::kTaskVaSize, 64);
		amdgpu::BringupContext *ctx = owner->bringupContext();
		OSNumber *vram = OSNumber::withNumber(ctx ? ctx->gmc.real_vram_size : 0ull, 64);
		OSNumber *ready = OSNumber::withNumber((uint64_t)garnetmetal::ready_at_publish(__atomic_load_n(&gReadyNo, __ATOMIC_ACQUIRE) != 0u, g.hung), 32);   // 0.0.612: 1 while usable
		// Immutable schema v1: queries can discover memory limits without opening RADV.
		const uint64_t visible = ctx ? ctx->gmc.vram_alloc.size() : 0ull;
		const uint64_t high = ctx && ctx->gmc.vram_alloc_hi.is_inited() ? ctx->gmc.vram_alloc_hi.size() : 0ull;
		const uint64_t pool = visible + high;
		const uint64_t largest = visible > high ? visible : high;
		const char *memoryKeys[] = {"version", "physical", "pool", "visible", "applicationBudget", "maxBuffer", "compositeMaxBuffer"};
		const uint64_t memoryValues[] = {1ull, ctx ? ctx->gmc.real_vram_size : 0ull, pool, visible,
		    rdna4native::g2::app_budget_ceiling(pool, 0ull), largest < 0xFFFFFFFCull ? largest : 0xFFFFFFFCull,
		    largest < (8ull << 30) ? largest : (8ull << 30)};
		OSDictionary *memory = OSDictionary::withCapacity(7);
		bool memoryOk = memory != nullptr;
		for (uint32_t i = 0; memoryOk && i < 7; i++) {
			OSNumber *number = OSNumber::withNumber(memoryValues[i], 64);
			memoryOk = number && memory->setObject(memoryKeys[i], number);
			if (number) number->release();
		}
		if (model && abi && va && vram && ready && memoryOk && nub->setProperty("Garnet,MemoryCaps", memory)) {
			nub->setProperty("model", model);
			nub->setProperty("Garnet,MetalABI", abi);
			nub->setProperty("Garnet,TaskVASize", va);
			nub->setProperty("Garnet,VRAMBytes", vram);
			nub->setProperty("Garnet,Ready", ready);
			if (nub->attach(owner)) {
				gNub = nub;                                                       // our reference (from alloc) is kept until withdraw
				gState = garnetmetal::sm_after_publish((garnetmetal::State)gState);
				__atomic_store_n(&gEverPublished, 1u, __ATOMIC_RELEASE);
				out[0] = 1; out[1] = nub->getRegistryEntryID(); out[2] = shape.abi; out[3] = (uint64_t)shape.size;
				rc = kIOReturnSuccess;
			}
		}
		if (model) model->release();
		if (abi) abi->release();
		if (va) va->release();
		if (vram) vram->release();
		if (ready) ready->release();
		if (memory) memory->release();
	}
	if (props) props->release();
	if (rc != kIOReturnSuccess) {
		IOLockUnlock(gLock);
		if (nub) nub->release();                                                  // AFTER the unlock: the last release runs free(), which takes gLock
		MNLOG("nub publish failed: %#x", (unsigned)rc);
		return rc;
	}
	nub->retain();                                                                // held across registerService(): a racing withdraw must not free the nub under it
	IOLockUnlock(gLock);
	MNLOG("nub published (id %llu, ops ABI %u, %u bytes): the aux kext may now match it", (unsigned long long)out[1], (unsigned)out[2], (unsigned)out[3]);
	nub->registerService();                                                       // outside the lock: matching may run the aux kext's probe, which calls back into the ops
	nub->release();                                                               // the extra reference taken under the lock
	return kIOReturnSuccess;
}

IOReturn GarnetMetalNub::selectorWithdraw(uint64_t flags, uint64_t out[1]) {
	if (!out) return kIOReturnBadArgument;
	ensure_lock();
	if (!gLock) return kIOReturnNoMemory;
	IOLockLock(gLock);
	const garnetmetal::GateIn g = gate_now(flags, false);
	const uint32_t verdict = garnetmetal::withdraw_verdict(g, (garnetmetal::State)gState);
	if (verdict != garnetmetal::kOk) { IOLockUnlock(gLock); return (IOReturn)verdict; }
	GarnetMetalNub *nub = gNub;
	out[0] = gState;
	gNub = nullptr;
	gState = garnetmetal::sm_after_withdraw((garnetmetal::State)gState);
	IOLockUnlock(gLock);
	if (nub) { nub->terminate(); nub->release(); }
	rdna4disp_on_withdraw();                                                        // 0.0.613: forget the pipes AFTER the accelerator was told to stop (a no-op unless boot-arg garnet-metal-disp=1)
	MNLOG("nub withdrawn");
	return kIOReturnSuccess;
}

// 0.0.612 (W3): called by native_s1c.cpp's hang_announce right after the HUNG latch is set. The sticky word is stored FIRST (a publish that races us reads it under gLock or is
// ordered before our write of the property, which waits for gLock), then the published nub's property goes to 0. No nub yet: gLock may not even exist; the sticky word covers it.
void GarnetMetalNub::hungLatched() {
	__atomic_store_n(&gReadyNo, garnetmetal::ready_sticky_after(__atomic_load_n(&gReadyNo, __ATOMIC_ACQUIRE) != 0u, true) ? 1u : 0u, __ATOMIC_RELEASE);
	rdna4disp_on_hung();                                                            // 0.0.617 (K2): a declined device with an ARMED display pipe is fatal to WindowServer (m11h5-3): disarm first (no lock taken; a no-op unless boot-arg garnet-metal-disp=1 and armed)
	if (!gLock) { MNLOG("HUNG latched: Garnet,Ready = 0 (no nub is published)"); return; }
	IOLockLock(gLock);
	GarnetMetalNub *nub = gNub;
	if (nub) nub->setProperty("Garnet,Ready", (unsigned long long)garnetmetal::kReadyNo, 32);
	IOLockUnlock(gLock);
	MNLOG("HUNG latched: Garnet,Ready = 0 (%s)", nub ? "written on the published nub" : "no nub is published");
}

IOService *GarnetMetalNub::published() { return gNub; }   // 0.0.613: identity only (NOT retained)

// 0.0.659 (M6): properties of the published nub that the bundle (in WindowServer) reads: the latch word and the surface table (garnet_multidisplay_route_pure.h). false = no nub is published / no memory. Takes this file's lock only (the caller holds none of ours).
bool GarnetMetalNub::setPublishedData(const char *key, const void *bytes, uint32_t len) {
	if (!key || !bytes || !len || !gLock) return false;
	OSData *d = OSData::withBytes(bytes, len);
	if (!d) return false;
	IOLockLock(gLock);
	GarnetMetalNub *nub = gNub;
	const bool ok = nub && nub->setProperty(key, d);
	IOLockUnlock(gLock);
	d->release();
	return ok;
}
bool GarnetMetalNub::setPublishedNumber(const char *key, uint64_t v) {
	if (!key || !gLock) return false;
	IOLockLock(gLock);
	GarnetMetalNub *nub = gNub;
	const bool ok = nub && nub->setProperty(key, v, 32);
	IOLockUnlock(gLock);
	return ok;
}

IOService *GarnetMetalNub::registeredAccelerator() {      // 0.0.656 (G6): identity only (NOT retained)
	Dev *d = __atomic_load_n(&gDev, __ATOMIC_ACQUIRE);
	return (d && d->magic == kDevMagic) ? static_cast<IOService *>(d->accel) : nullptr;
}

void GarnetMetalNub::shutdown() {
	if (!gLock) return;
	IOLockLock(gLock);
	GarnetMetalNub *nub = gNub;
	if (nub) { gNub = nullptr; gState = garnetmetal::sm_after_withdraw((garnetmetal::State)gState); }
	IOLockUnlock(gLock);
	if (nub) { nub->terminate(); nub->release(); MNLOG("nub terminated at kext stop"); }
	rdna4disp_on_withdraw();
}

IOReturn GarnetMetalNub::callPlatformFunction(const OSSymbol *functionName, bool waitForFunction, void *param1, void *param2, void *param3, void *param4) {
	if (functionName && functionName->isEqualTo(GARNET_METAL_FN_SYMBOL)) {
		if (!param1) return kIOReturnBadArgument;
		*(const GarnetMetalOps **)param1 = rdna4disp::ops_shape(rdna4disp_latched_on()).dispFlags != 0u ? &gOps : &gOpsOff;
		return kIOReturnSuccess;
	}
	return super::callPlatformFunction(functionName, waitForFunction, param1, param2, param3, param4);
}

void GarnetMetalNub::free() {
	if (gLock) { IOLockLock(gLock); gState = garnetmetal::sm_after_free((garnetmetal::State)gState); IOLockUnlock(gLock); }
	super::free();
}
