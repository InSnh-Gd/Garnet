//
//  GarnetDisplayGate.cpp — primary IOFramebuffer lease gate, peer diagnostics,
//  and serialized display transaction helpers.
//
#include "GarnetDriver.hpp"
#include "GarnetDisplayOps.h"
#include "GarnetPrimaryFramebuffer.hpp"
#include "amd/amdgpu_asic.h"
#include "amd/native_disp.h"
#include <IOKit/IOLocks.h>
#include <IOKit/IORegistryEntry.h>
#include <kern/thread.h>

static_assert(static_cast<uint32_t>(amdgpu::AsicId::Navi48) == rdna4peer::kAsicNavi48,
              "peer ASIC mapping");
static_assert(static_cast<uint32_t>(amdgpu::AsicId::Navi44) == rdna4peer::kAsicNavi44,
              "peer ASIC mapping");

static uint64_t display_thread_owner() {
	return static_cast<uint64_t>(reinterpret_cast<uintptr_t>(current_thread()));
}

static uint32_t display_asic_id() {
	const amdgpu::AsicInfo *a = amdgpu::asic_current();
	if (!a) return rdna4peer::kAsicUnknown;
	return a->id == amdgpu::AsicId::Navi44 ? rdna4peer::kAsicNavi44 :
	       a->id == amdgpu::AsicId::Navi48 ? rdna4peer::kAsicNavi48 :
	                                         rdna4peer::kAsicUnknown;
}

bool GarnetDriver::buildDisplayPeerSnapshot(uint32_t state,
                                            rdna4peer::Snapshot &out,
                                            uint32_t &reason) const {
	out = rdna4peer::Snapshot {};
	reason = rdna4peer::kNotReady;
	const amdgpu::AsicInfo *asic = amdgpu::asic_current();
	if (!pciDevice || !asic || vramMB == 0) return false;

	const uint32_t vd = pciDevice->configRead32(kIOPCIConfigVendorID);
	const uint64_t vramBytes = static_cast<uint64_t>(vramMB) << 20;
	uint64_t consoleOff = 0, consoleLen = 0;
	uint32_t consoleW = 0, consoleH = 0, consoleRow = 0;
	if (!consoleGeometry(&consoleOff, &consoleLen, &consoleW, &consoleH, &consoleRow)) {
		reason = rdna4peer::kBadRange;
		return false;
	}
	if (bootFbDepth != 32u) {
		reason = rdna4peer::kBadRange;
		return false;
	}

	rdna4peer::Layout layout {};
	rdna4peer::LayoutIn in {};
	in.vramBytes = vramBytes;
	in.vramBase = vramBase;
	in.vramLimit = bar0Limit;
	in.fixedLength = rdna4peer::kFixedReserve;
	in.holdLength = amdgpu::kVramHiTotalReserve;
	in.consoleOffset = consoleOff;
	in.consoleLength = consoleLen;
	in.haveConsole = true;
	reason = rdna4peer::build_layout(in, layout);
	if (reason != rdna4peer::kOk) return false;

	out.version = rdna4peer::kVersion;
	out.state = state;
	out.asic = display_asic_id();
	out.displayIndex = 0;
	out.vendor = static_cast<uint16_t>(vd);
	out.device = static_cast<uint16_t>(vd >> 16);
	out.providerEntryID = pciDevice->getRegistryEntryID();
	out.bar0Phys = bar0Phys;
	out.bar0Size = bar0Size;
	out.vramBytes = vramBytes;
	out.vramBase = vramBase;
	out.vramLimit = bar0Limit;
	out.console = layout.console;
	out.consolePhysBase = bootFbPhys;
	out.consoleWidth = consoleW;
	out.consoleHeight = consoleH;
	out.consoleRowBytes = consoleRow;
	out.consoleDepth = bootFbDepth;
	out.fixed = layout.fixed;
	out.pool = layout.pool;
	out.hold = layout.hold;
	out.grant = displayGrant.present ? displayGrant : rdna4peer::no_grant();

	rdna4peer::Expected expected {};
	expected.vendor = out.vendor;
	expected.device = out.device;
	expected.asic = out.asic;
	expected.providerEntryID = out.providerEntryID;
	expected.bar0Phys = out.bar0Phys;
	expected.bar0Size = out.bar0Size;
	reason = rdna4peer::validate_identity(out, expected);
	if (reason != rdna4peer::kOk) return false;
	reason = rdna4peer::validate_snapshot_layout(out);
	return reason == rdna4peer::kOk;
}

// Pointer-free registry mirror of the peer snapshot, the primary registration
// and the one reservation table. Diagnostics only: the callPlatformFunction
// endpoint below remains the authority, and these properties are refreshed at
// state transitions, registration and unregistration (not on every lease).
void GarnetDriver::publishDisplayDiagnostics(const rdna4peer::Snapshot &s,
                                            const rdna4peer::GateState &g,
                                            const rdna4peer::PrimaryView &p,
                                            bool primaryService,
                                            const char *status) {
	setProperty(rdna4peer::kPropState, static_cast<uint64_t>(s.state), 32);
	setProperty(rdna4peer::kPropStateName, rdna4peer::state_name(s.state));
	setProperty(rdna4peer::kPropStatus, status ? status : "?");
	setProperty(rdna4peer::kPropGeneration, s.generation, 64);
	setProperty(rdna4peer::kPropActiveLeases, static_cast<uint64_t>(g.activeLease), 32);
	setProperty(rdna4peer::kPropVersion, static_cast<uint64_t>(s.version), 32);
	setProperty(rdna4peer::kPropVendor, static_cast<uint64_t>(s.vendor), 16);
	setProperty(rdna4peer::kPropDevice, static_cast<uint64_t>(s.device), 16);
	setProperty(rdna4peer::kPropProvider, s.providerEntryID, 64);
	setProperty(rdna4peer::kPropBar0Phys, s.bar0Phys, 64);
	setProperty(rdna4peer::kPropBar0Size, s.bar0Size, 64);
	setProperty(rdna4peer::kPropVramBytes, s.vramBytes, 64);
	setProperty(rdna4peer::kPropVramBase, s.vramBase, 64);
	setProperty(rdna4peer::kPropVramLimit, s.vramLimit, 64);
	setProperty(rdna4peer::kPropConsoleOffset, s.console.offset, 64);
	setProperty(rdna4peer::kPropConsoleLength, s.console.length, 64);
	setProperty(rdna4peer::kPropFixedOffset, s.fixed.offset, 64);
	setProperty(rdna4peer::kPropFixedLength, s.fixed.length, 64);
	setProperty(rdna4peer::kPropPoolOffset, s.pool.offset, 64);
	setProperty(rdna4peer::kPropPoolLength, s.pool.length, 64);
	setProperty(rdna4peer::kPropHoldOffset, s.hold.offset, 64);
	setProperty(rdna4peer::kPropHoldLength, s.hold.length, 64);
	setProperty(rdna4peer::kPropGrantPresent, s.grant.present);
	setProperty(rdna4peer::kPropGrantOffset, s.grant.offset, 64);
	setProperty(rdna4peer::kPropGrantLength, s.grant.length, 64);
	setProperty(rdna4peer::kPropGrantOwner, s.grant.owner, 64);

	const bool registered = g.primaryRegistered != 0 && p.fbEntryID != 0;
	const char *primary = g.primaryConflict ? "conflict" :
	                      !registered ? "absent" :
	                      !primaryService ? "unreferenced" : "ok";
	setProperty(rdna4peer::kPropPrimaryRegistered, registered);
	setProperty(rdna4peer::kPropPrimaryConflict, g.primaryConflict != 0);
	setProperty(rdna4peer::kPropPrimaryStatus, primary);
	setProperty(rdna4peer::kPropPrimaryEntryID, p.fbEntryID, 64);
	setProperty(rdna4peer::kPropPrimaryProvider, p.providerEntryID, 64);
	setProperty(rdna4peer::kPropPrimaryGeneration, p.generation, 64);
	setProperty(rdna4peer::kPropPrimaryVendor, static_cast<uint64_t>(p.vendor), 16);
	setProperty(rdna4peer::kPropPrimaryDevice, static_cast<uint64_t>(p.device), 16);
	setProperty(rdna4peer::kPropPrimaryAsic, static_cast<uint64_t>(p.asic), 32);
	setProperty(rdna4peer::kPropPrimaryDisplayIndex,
	            static_cast<uint64_t>(p.displayIndex), 32);
	setProperty(rdna4peer::kPropPrimaryHead, static_cast<uint64_t>(p.head), 32);
	setProperty(rdna4peer::kPropPrimaryOtg, static_cast<uint64_t>(p.otg), 32);
	setProperty(rdna4peer::kPropPrimaryModeWidth,
	            static_cast<uint64_t>(p.modeWidth), 32);
	setProperty(rdna4peer::kPropPrimaryModeHeight,
	            static_cast<uint64_t>(p.modeHeight), 32);
	setProperty(rdna4peer::kPropPrimaryModeRowBytes,
	            static_cast<uint64_t>(p.modeRowBytes), 32);
	setProperty(rdna4peer::kPropPrimaryModeDepth,
	            static_cast<uint64_t>(p.modeDepth), 32);
	setProperty(rdna4peer::kPropPrimaryConsolePhys, p.consolePhysBase, 64);
	setProperty(rdna4peer::kPropPrimaryConsoleOffset, p.consoleVramOffset, 64);
	setProperty(rdna4peer::kPropPrimaryConsoleLength, p.consoleLength, 64);
	setProperty(rdna4peer::kPropPrimaryConsoleWidth,
	            static_cast<uint64_t>(p.consoleWidth), 32);
	setProperty(rdna4peer::kPropPrimaryConsoleHeight,
	            static_cast<uint64_t>(p.consoleHeight), 32);
	setProperty(rdna4peer::kPropPrimaryConsoleRowBytes,
	            static_cast<uint64_t>(p.consoleRowBytes), 32);
	setProperty(rdna4peer::kPropPrimaryConsoleDepth,
	            static_cast<uint64_t>(p.consoleDepth), 32);
}

// Publish only facts that have been captured in full.  In particular, a
// survey-only boot never becomes READY: no BAR0 map means no usable resource
// snapshot and the display side must remain fail-closed.
void GarnetDriver::publishDisplayPeer(uint32_t state) {
	if (!displayPeerLock) return;

	if (state == rdna4peer::kStateStarting) {
		const uint32_t vd = pciDevice ? pciDevice->configRead32(kIOPCIConfigVendorID) : 0u;
		rdna4peer::Snapshot s {};
		s.version = rdna4peer::kVersion;
		s.state = rdna4peer::kStateStarting;
		s.asic = display_asic_id();
		s.vendor = static_cast<uint16_t>(vd);
		s.device = static_cast<uint16_t>(vd >> 16);
		s.providerEntryID = pciDevice ? pciDevice->getRegistryEntryID() : 0;
		rdna4peer::GateState g {};
		IOLockLock(displayPeerLock);
		rdna4peer::gate_reset(displayPeerGate);
		s.generation = displayPeerGate.generation;
		displayPeerSnapshot = s;
		g = displayPeerGate;
		IOLockUnlock(displayPeerLock);
		publishDisplayDiagnostics(s, g, rdna4peer::PrimaryView {}, false, "starting");
		return;
	}

	if (state == rdna4peer::kStateReady) {
		rdna4peer::Snapshot s {};
		uint32_t reason = rdna4peer::kNotReady;
		const bool ok = buildDisplayPeerSnapshot(state, s, reason);
		rdna4peer::GateState g {};
		rdna4peer::PrimaryView p {};
		bool service = false;
		bool ready = false;
		IOLockLock(displayPeerLock);
		if (ok && displayPeerGate.state == rdna4peer::kStateStarting &&
		    rdna4peer::gate_ready(displayPeerGate) == rdna4peer::kOk) {
			s.generation = displayPeerGate.generation;
			displayPeerSnapshot = s;
			g = displayPeerGate;
			p = displayPrimary;
			service = displayPrimaryService != nullptr;
			ready = true;
		}
		IOLockUnlock(displayPeerLock);
		if (ready) {
			RDNA4LOG("display-peer: READY generation %llu, provider 0x%llx, BAR0 %#llx+%#llx, console %#llx+%#llx",
			         (unsigned long long)s.generation, (unsigned long long)s.providerEntryID,
			         (unsigned long long)s.bar0Phys, (unsigned long long)s.bar0Size,
			         (unsigned long long)s.consolePhysBase, (unsigned long long)s.console.length);
			publishDisplayDiagnostics(s, g, p, service, "ready");
			setProperty("Garnet,DisplayPeerLayout", "ok");
			return;
		}
		RDNA4LOG("display-peer: REFUSED READY: %s (stage/resource snapshot is incomplete)",
		         rdna4peer::reason_name(reason));
		IOLockLock(displayPeerLock);
		s = displayPeerSnapshot;
		g = displayPeerGate;
		IOLockUnlock(displayPeerLock);
		publishDisplayDiagnostics(s, g, rdna4peer::PrimaryView {}, false,
		                          rdna4peer::reason_name(reason));
		setProperty("Garnet,DisplayPeerLayout", rdna4peer::reason_name(reason));
		return;
	}

	if (state == rdna4peer::kStateStopping) {
		rdna4peer::Snapshot s {};
		rdna4peer::GateState g {};
		rdna4peer::PrimaryView p {};
		bool service = false;
		IOLockLock(displayPeerLock);
		if (displayPeerGate.state == rdna4peer::kStateReady ||
		    displayPeerGate.state == rdna4peer::kStateStarting)
			(void)rdna4peer::gate_begin_stop(displayPeerGate);
		displayPeerSnapshot.state = rdna4peer::kStateStopping;
		displayPeerSnapshot.generation = displayPeerGate.generation;
		s = displayPeerSnapshot;
		g = displayPeerGate;
		p = displayPrimary;
		service = displayPrimaryService != nullptr;
		IOLockUnlock(displayPeerLock);
		publishDisplayDiagnostics(s, g, p, service, "stopping");
		return;
	}

	if (state == rdna4peer::kStateStopped) {
		bool stopped = false;
		rdna4peer::Snapshot s {};
		rdna4peer::GateState g {};
		IOLockLock(displayPeerLock);
		if (displayPeerGate.state == rdna4peer::kStateStopping &&
		    rdna4peer::gate_finish_stop(displayPeerGate) == rdna4peer::kOk) {
			displayPeerSnapshot.state = rdna4peer::kStateStopped;
			displayPeerSnapshot.generation = displayPeerGate.generation;
			displayPrimary = rdna4peer::PrimaryView {};
			stopped = true;
		}
		s = displayPeerSnapshot;
		g = displayPeerGate;
		IOLockUnlock(displayPeerLock);
		publishDisplayDiagnostics(s, g, rdna4peer::PrimaryView {}, false,
		                          stopped ? "stopped" : "stopping");
	}
}

bool GarnetDriver::displayPeerCallerOnGpu(IOService *caller,
                                          uint64_t framebufferEntryID) const {
	if (!caller || framebufferEntryID == 0 ||
	    caller->getRegistryEntryID() != framebufferEntryID)
		return false;
	return displayPeerCallerUnderGpu(caller);
}

bool GarnetDriver::displayPeerCallerUnderGpu(IOService *caller) const {
	if (!pciDevice || !caller) return false;
	IOService *at = caller;
	bool underGpu = false;
	for (uint32_t i = 0; at && i < 16u; i++) {
		if (at == pciDevice) {
			underGpu = true;
			break;
		}
		at = at->getProvider();
	}
	// Under-GPU ancestry alone is not ownership: only our IOFramebuffer leaf
	// (never IONDRVFramebuffer) may register or query primary display state.
	return underGpu && caller->metaCast("IOFramebuffer") != nullptr &&
	       caller->metaCast("IONDRVFramebuffer") == nullptr;
}

bool GarnetDriver::displayGateBegin(uint32_t kind, uint64_t expectedFbEntryID,
                                    uint64_t *token) {
	if (!token) return false;
	*token = 0;
	if (!displayPeerLock) return false;
	IOLockLock(displayPeerLock);
	const rdna4peer::Snapshot snapshot = displayPeerSnapshot;
	uint64_t fbEntryID = expectedFbEntryID;
	if (fbEntryID == 0 && kind == rdna4peer::kLeaseDisplayStop)
		fbEntryID = snapshot.providerEntryID;
	else if (fbEntryID == 0)
		fbEntryID = displayPeerGate.primaryRegistered ? displayPrimary.fbEntryID :
		                                               snapshot.providerEntryID;
	uint32_t why = rdna4peer::validate_local_acquire(snapshot, displayPeerGate,
	                                                 displayPeerGate.generation,
	                                                 fbEntryID, kind, 0, 0);
	if (why == rdna4peer::kOk)
		why = rdna4peer::gate_acquire(displayPeerGate,
		                              snapshot.generation,
		                              fbEntryID, kind,
		                              display_thread_owner(), *token);
	if (why != rdna4peer::kOk) {
		const bool log = displayPeerRefusals++ < 16u;
		IOLockUnlock(displayPeerLock);
		if (log)
			RDNA4LOG("display-peer: local lease REFUSED: %s (kind %u, fb %#llx)",
			         rdna4peer::reason_name(why), kind, (unsigned long long)fbEntryID);
		return false;
	}
	IOLockUnlock(displayPeerLock);
	return true;
}

void GarnetDriver::displayGateEnd(uint64_t token) {
	if (!displayPeerLock || token == 0) return;
	IOLockLock(displayPeerLock);
	const uint32_t why = rdna4peer::gate_release(displayPeerGate, token, 0, 0,
	                                             display_thread_owner());
	const bool log = why != rdna4peer::kOk && displayPeerRefusals++ < 16u;
	IOLockUnlock(displayPeerLock);
	if (why == rdna4peer::kOk) return;
	if (log)
		RDNA4LOG("display-peer: local lease RELEASE REFUSED: %s (token %#llx)",
		         rdna4peer::reason_name(why), (unsigned long long)token);
}

bool GarnetDriver::displayGateHeld() const {
	return displayGateHeldFor(0);
}

bool GarnetDriver::displayGateHeldFor(uint64_t expectedFbEntryID) const {
	if (!displayPeerLock) return false;
	IOLockLock(displayPeerLock);
	const bool held = displayPeerSnapshot.state == displayPeerGate.state &&
	                  rdna4peer::gate_held(displayPeerGate,
	                                       displayPeerSnapshot.generation,
	                                       display_thread_owner(),
	                                       expectedFbEntryID);
	IOLockUnlock(displayPeerLock);
	return held;
}

bool GarnetDriver::displayPeerActive() const {
	if (!displayPeerLock) return false;
	IOLockLock(displayPeerLock);
	const bool active = displayPeerSnapshot.state == rdna4peer::kStateReady &&
	                    displayPeerGate.state == rdna4peer::kStateReady &&
	                    displayPeerSnapshot.generation == displayPeerGate.generation;
	IOLockUnlock(displayPeerLock);
	return active;
}

bool GarnetDriver::waitForDisplayLeasesAndReleasePrimary() {
	if (!displayPeerLock) return true;
	bool drained = false;
	// Existing transactions are short display operations.  STOPPING has
	// already refused every new lease, so stop waits for the real drain: no
	// timeout, no unmapping under a live writer, and no post-stop teardown.
	// Logging is bounded so a wedged owner is visible without flooding.
	uint64_t ms = 0;
	uint32_t active = 0, logs = 0;
	for (;;) {
		IOLockLock(displayPeerLock);
		drained = rdna4peer::gate_drained(displayPeerGate);
		active = displayPeerGate.activeLease;
		IOLockUnlock(displayPeerLock);
		if (drained) break;
		const bool mark = ms == 1000u || ms == 2000u || ms == 5000u ||
		                  ms == 10000u || (ms >= 30000u && ms % 30000u == 0u);
		if (mark && logs++ < 16u) {
			RDNA4LOG("display-peer: STOPPING drain waiting at %llu ms (%u active lease(s))",
			         (unsigned long long)ms, active);
		}
		IOSleep(1);
		ms++;
	}
	IOService *release = nullptr;
	IOLockLock(displayPeerLock);
	release = displayPrimaryService;
	displayPrimaryService = nullptr;
	displayPrimary = rdna4peer::PrimaryView {};
	displayPeerGate.primaryRegistered = 0;
	IOLockUnlock(displayPeerLock);
	if (release) release->release();
	RDNA4LOG("display-peer: STOPPING drain complete after %llu ms",
	         (unsigned long long)ms);
	return true;
}

uint32_t GarnetDriver::registerPrimaryFramebuffer(IOService *fb,
                                                 const rdna4peer::PrimaryView &candidate,
                                                 rdna4peer::Snapshot *outSnapshot,
                                                 rdna4peer::PrimaryView *outStored) {
	if (!displayPeerLock) return rdna4peer::kNotReady;
	if (!displayPeerCallerUnderGpu(fb) || candidate.fbEntryID == 0 ||
	    fb->getRegistryEntryID() != candidate.fbEntryID)
		return rdna4peer::kBadCaller;

	uint32_t status = rdna4peer::kOk;
	rdna4peer::Snapshot afterSnapshot {};
	rdna4peer::GateState afterGate {};
	rdna4peer::PrimaryView afterPrimary {};
	bool afterService = false;

	IOLockLock(displayPeerLock);
	const rdna4peer::Snapshot current = displayPeerSnapshot;
	if (displayPeerGate.state != rdna4peer::kStateReady ||
	    current.state != rdna4peer::kStateReady) {
		status = (displayPeerGate.state == rdna4peer::kStateStopping ||
		          current.state == rdna4peer::kStateStopping) ? rdna4peer::kStopping :
		         (displayPeerGate.state == rdna4peer::kStateStopped ||
		          current.state == rdna4peer::kStateStopped) ? rdna4peer::kStopped :
		         rdna4peer::kNotReady;
	} else if (displayPrimaryService && fb != displayPrimaryService) {
		status = rdna4peer::kBadCaller;
	} else if (candidate.version != rdna4peer::kVersion) {
		status = rdna4peer::kBadVersion;
	} else if (!current.generation || candidate.generation != current.generation ||
	           displayPeerGate.generation != current.generation) {
		status = rdna4peer::kBadGeneration;
	} else if (candidate.vendor != current.vendor || candidate.device != current.device ||
	           candidate.asic != current.asic ||
	           candidate.providerEntryID != current.providerEntryID ||
	           candidate.bar0Phys != current.bar0Phys ||
	           candidate.bar0Size != current.bar0Size) {
		status = rdna4peer::kIdentityMismatch;
	} else if (candidate.consoleLength == 0 || candidate.consoleWidth == 0 ||
	           candidate.consoleHeight == 0 || candidate.consoleRowBytes == 0 ||
	           candidate.modeWidth == 0 || candidate.modeHeight == 0 ||
	           candidate.modeRowBytes == 0 || candidate.head > rdna4peer::kMaxDisplayHead ||
	           candidate.otg > rdna4peer::kMaxDisplayOtg) {
		status = rdna4peer::kBadRequest;
	} else {
		uint64_t consoleEnd = 0, consolePhys = 0;
		if (candidate.consolePhysBase != current.consolePhysBase ||
		    candidate.consoleVramOffset != current.console.offset ||
		    candidate.consoleLength != current.console.length ||
		    !rdna4peer::add_ok(current.bar0Phys, candidate.consoleVramOffset, consolePhys) ||
		    consolePhys != candidate.consolePhysBase) {
			status = rdna4peer::kPrimaryMismatch;
		} else if (!rdna4peer::add_ok(candidate.consoleVramOffset, candidate.consoleLength, consoleEnd) ||
		    consoleEnd > current.bar0Size ||
		    consoleEnd > current.vramLimit ||
		    consoleEnd > current.vramBytes) {
			status = rdna4peer::kBadRange;
		} else {
			status = rdna4peer::primary_register(displayPeerGate, displayPrimary, candidate);
			if (status == rdna4peer::kOk && !displayPrimaryService) {
				fb->retain();
				displayPrimaryService = fb;
			}
			if (status == rdna4peer::kOk) {
				if (outSnapshot) *outSnapshot = current;
				if (outStored) *outStored = displayPrimary;
			}
		}
	}
	afterSnapshot = displayPeerSnapshot;
	afterGate = displayPeerGate;
	afterPrimary = displayPrimary;
	afterService = displayPrimaryService != nullptr;
	IOLockUnlock(displayPeerLock);

	publishDisplayDiagnostics(afterSnapshot, afterGate, afterPrimary, afterService,
	                          status == rdna4peer::kOk ? "registered" :
	                          rdna4peer::reason_name(status));
	return status;
}

uint32_t GarnetDriver::unregisterPrimaryFramebuffer(IOService *fb,
                                                   uint64_t expectedGeneration) {
	if (!displayPeerLock) return rdna4peer::kNotReady;
	if (!displayPeerCallerUnderGpu(fb)) return rdna4peer::kBadCaller;

	IOService *release = nullptr;
	uint32_t status = rdna4peer::kOk;
	rdna4peer::Snapshot afterSnapshot {};
	rdna4peer::GateState afterGate {};
	rdna4peer::PrimaryView afterPrimary {};
	bool afterService = false;

	IOLockLock(displayPeerLock);
	const uint64_t gen = expectedGeneration ? expectedGeneration : displayPrimary.generation;
	status = rdna4peer::primary_unregister(displayPeerGate, displayPrimary,
	                                         fb->getRegistryEntryID(), gen);
	if (status == rdna4peer::kOk) {
		release = displayPrimaryService;
		displayPrimaryService = nullptr;
	}
	afterSnapshot = displayPeerSnapshot;
	afterGate = displayPeerGate;
	afterPrimary = displayPrimary;
	afterService = displayPrimaryService != nullptr;
	IOLockUnlock(displayPeerLock);

	publishDisplayDiagnostics(afterSnapshot, afterGate, afterPrimary, afterService,
	                          status == rdna4peer::kOk ? "unregistered" :
	                          rdna4peer::reason_name(status));
	if (release) release->release();
	return status;
}

bool GarnetDriver::queryPrimaryFramebuffer(rdna4peer::Snapshot *outSnapshot,
                                            rdna4peer::PrimaryView *outPrimary,
                                            IOService **outRetainedFb) const {
	if (outRetainedFb) *outRetainedFb = nullptr;
	if (!displayPeerLock) return false;

	rdna4peer::PrimaryView primary {};
	bool ok = false;
	IOLockLock(displayPeerLock);
	const uint32_t status = rdna4peer::primary_query(displayPeerGate, displayPrimary,
	                                                 displayPeerSnapshot.generation,
	                                                 primary, display_thread_owner());
	if (status == rdna4peer::kOk && displayPrimaryService &&
	    displayPeerSnapshot.generation == displayPeerGate.generation &&
	    primary.generation == displayPeerSnapshot.generation) {
		if (outSnapshot) *outSnapshot = displayPeerSnapshot;
		if (outPrimary) *outPrimary = primary;
		if (outRetainedFb) {
			displayPrimaryService->retain();
			*outRetainedFb = displayPrimaryService;
		}
		ok = true;
	}
	IOLockUnlock(displayPeerLock);
	return ok;
}

bool GarnetDriver::snapshotDisplayState(rdna4peer::Snapshot *outSnapshot) const {
	if (!outSnapshot || !displayPeerLock) return false;
	bool ok = false;
	IOLockLock(displayPeerLock);
	if (displayPeerGate.state == rdna4peer::kStateReady &&
	    displayPeerSnapshot.state == rdna4peer::kStateReady &&
	    displayPeerSnapshot.generation != 0 &&
	    displayPeerSnapshot.generation == displayPeerGate.generation) {
		*outSnapshot = displayPeerSnapshot;
		ok = true;
	}
	IOLockUnlock(displayPeerLock);
	return ok;
}

bool rdna4displaytxn::begin(uint32_t kind, uint64_t expectedFbEntryID,
                            uint64_t *token) {
	IOService *service = garnet_driver_service_retained();
	if (!service) return false;
	const bool ok = static_cast<GarnetDriver *>(service)->displayGateBegin(
		kind, expectedFbEntryID, token);
	service->release();
	return ok;
}

void rdna4displaytxn::end(uint64_t token) {
	IOService *service = garnet_driver_service_retained();
	if (!service) return;
	static_cast<GarnetDriver *>(service)->displayGateEnd(token);
	service->release();
}

bool rdna4displaytxn::held(void) {
	IOService *service = garnet_driver_service_retained();
	if (!service) return false;
	const bool held = static_cast<GarnetDriver *>(service)->displayGateHeld();
	service->release();
	return held;
}

bool rdna4displaytxn::held_for(uint64_t expectedFbEntryID) {
	IOService *service = garnet_driver_service_retained();
	if (!service) return false;
	const bool held =
		static_cast<GarnetDriver *>(service)->displayGateHeldFor(expectedFbEntryID);
	service->release();
	return held;
}

