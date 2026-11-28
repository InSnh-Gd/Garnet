#include "dcn/dcn41_vbl_irq_pure.h"
#include "dcn/dcn41_det_pure.h"
#include "dcn/dcn41_dpms_pure.h"
#include "amd/garnet_power_policy_pure.h"
#include "amd/rdna4_sleep_pure.h"
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <initializer_list>
#include <mutex>
#include <thread>

using IOSelect = uint32_t;
using UInt32 = uint32_t;
using IOReturn = int;
using vm_size_t = size_t;
using kern_return_t = IOReturn;
struct IOService {};
constexpr IOSelect kIOFBVBLInterruptType = 0x76626c20u;
constexpr IOReturn kIOReturnSuccess = 0;
constexpr IOReturn kIOReturnUnsupported = 1;
constexpr IOReturn kIOReturnBadArgument = 2;
constexpr IOReturn kIOReturnNoMemory = 3;
constexpr IOReturn kIOReturnNotReady = 4;
constexpr UInt32 kEnabledInterruptState = 1;

struct OSObject {
	std::atomic<int> retainCount{1};
	void retain() { ++retainCount; }
	void release() { --retainCount; }
};
struct IOLock {
	std::mutex mutex;
	bool held = false;
	void (*beforeLock)() = nullptr;
};
void IOLockLock(IOLock *lock) {
	if (lock->beforeLock) lock->beforeLock();
	lock->mutex.lock();
	lock->held = true;
}
void IOLockUnlock(IOLock *lock) { lock->held = false; lock->mutex.unlock(); }
#define OSDynamicCast(type, object) static_cast<type *>(object)
static inline void rdna4_test_log(const char *, ...) {}
#define RDNA4LOG(...) rdna4_test_log(__VA_ARGS__)

struct IOTimerEventSource {
	bool enabled = false;
	bool scheduled = false;
	unsigned period = 0;
	void enable() { enabled = true; }
	void disable() { enabled = false; }
	void cancelTimeout() { scheduled = false; }
	void setTimeoutUS(unsigned timeout) { period = timeout; scheduled = true; }
};

using IOFBInterruptProc = void (*)(OSObject *, void *);
using thread_call_param_t = void *;
struct FakeCall {
	void (*callback)(void *, void *);
	void *owner;
	void *arg = nullptr;
	bool pending = false;
};
using thread_call_t = FakeCall *;
constexpr unsigned THREAD_CALL_PRIORITY_KERNEL = 0;
static FakeCall fakeCall{};
static thread_call_t thread_call_allocate_with_priority(void (*callback)(void *, void *), void *owner, unsigned) {
	fakeCall = {callback, owner};
	return &fakeCall;
}
static void thread_call_enter1_delayed(thread_call_t call, void *arg, uint64_t) {
	call->arg = arg;
	call->pending = true;
}
static void nanoseconds_to_absolutetime(uint64_t ns, uint64_t *absolute) { *absolute = ns; }
static void clock_absolutetime_interval_to_deadline(uint64_t interval, uint64_t *deadline) { *deadline = interval; }
namespace rdna4peer {
constexpr unsigned kLeaseLocalDisplay = 5;
struct PrimaryView { uint64_t generation, fbEntryID; };
struct Snapshot { uint64_t generation; };
}
namespace amdgpu {
struct DeviceContext {
	bool smuOnline = true, pinned = true, failPin = false;
	bool graphicsQuarantined = false;
	unsigned pins = 0, unpins = 0;
	bool graphicsPowerQuarantined() const { return graphicsQuarantined; }
};
struct SMUContext {
	bool sleepPending = false, wakeUnverified = false, postWake = false;
	unsigned wakePins = 0;
};
struct BringupContext {
	DeviceContext *dev;
	SMUContext smu;
	bool gfxOffSleepObserved = true;
	uint32_t smuVersion = 0x660001;
};
struct SmuSeq { bool held = true; };
enum class GfxOffPowerEvent { WillSleep, WillNotSleep, WillPowerOn, HasPoweredOn, Shutdown };
static IOReturn gfx_off_power_event(DeviceContext &, SMUContext &smu, GfxOffPowerEvent event) {
	if (event == GfxOffPowerEvent::WillSleep || event == GfxOffPowerEvent::Shutdown) {
		smu.sleepPending = true;
		smu.postWake = false;
	} else if (event == GfxOffPowerEvent::WillNotSleep && !smu.wakeUnverified) {
		smu.sleepPending = false;
	}
	return event == GfxOffPowerEvent::WillNotSleep && smu.sleepPending ? kIOReturnNotReady : kIOReturnSuccess;
}
static IOReturn smu_pin_uclk_top(DeviceContext &dev) {
	++dev.pins;
	if (dev.failPin) return kIOReturnNotReady;
	dev.pinned = true;
	return kIOReturnSuccess;
}
static IOReturn smu_set_uclk_hard_min(DeviceContext &dev, unsigned) {
	++dev.unpins;
	dev.pinned = false;
	return kIOReturnSuccess;
}
static bool smu_display_wake_pin_allowed(const SMUContext &smu) { return smu.postWake; }
static IOReturn smu_repin_uclk_after_wake(DeviceContext &dev, SMUContext &smu, uint32_t version) {
	++smu.wakePins;
	if (!smu.postWake || version != 0x660001 || dev.failPin) return kIOReturnNotReady;
	dev.pinned = true;
	return kIOReturnSuccess;
}
}
struct GarnetDriver : OSObject {
	amdgpu::DeviceContext dev;
	amdgpu::BringupContext bringup{&dev, {}};
	uint64_t generation = 7;
	bool lease = false, contended = false, wakePciReady = true;
	IOLock sleepMutex;
	IOLock *sleepLock = &sleepMutex;
	rdna4sleep::State sleepState;
	void logPciSleepState(const char *) {}
	static IOReturn sleepWakeCallback(void *, void *, UInt32, IOService *, void *, vm_size_t);
	amdgpu::DeviceContext *deviceContext() { return &dev; }
	amdgpu::BringupContext *bringupContext() { return &bringup; }
	bool displayWakePciReady() const { return lease && wakePciReady; }
	bool displayGateBegin(unsigned, uint64_t, uint64_t *token) {
		if (contended) return false;
		lease = true;
		*token = 1;
		return true;
	}
	void displayGateEnd(uint64_t) { lease = false; }
	bool queryPrimaryFramebuffer(void *, rdna4peer::PrimaryView *view) {
		*view = {generation, 22};
		return lease;
	}
	bool snapshotDisplayState(rdna4peer::Snapshot *snapshot) {
		*snapshot = {generation};
		return true;
	}
};
static GarnetDriver *fakeDriver = nullptr;
static amdgpu::BringupContext gBringup{};
static GarnetDriver *retain_driver() {
	if (fakeDriver) fakeDriver->retain();
	return fakeDriver;
}
struct FakeOptc {
	uint32_t value = 0, elapsedUs = 0;
	unsigned reads = 0, maps = 0;
	void (*beforeFirstRead)() = nullptr;
};
struct MmioAccess { FakeOptc *optc; };
static bool acquire_mmio(FakeOptc *optc, GarnetDriver *, MmioAccess *mmio) {
	++optc->maps;
	mmio->optc = optc;
	return true;
}
static void release_mmio(MmioAccess &) {}
static uint32_t cursor_rd_seg2(void *cookie, uint32_t) {
	auto &optc = *static_cast<MmioAccess *>(cookie)->optc;
	if (!optc.reads && optc.beforeFirstRead) optc.beforeFirstRead();
	++optc.reads;
	return optc.value;
}
static bool cursor_wr_seg2(void *cookie, uint32_t, uint32_t value) {
	static_cast<MmioAccess *>(cookie)->optc->value = value;
	return true;
}
static void aux_delay_us(void *cookie, uint32_t us) { static_cast<MmioAccess *>(cookie)->optc->elapsedUs += us; }
static void ddcpad_sleep_ms(void *, uint32_t) {}
struct AMDGarnetPrimaryFramebuffer : OSObject {
	IOLock lock;
	IOTimerEventSource timer;
	IOLock *fVblLock = &lock;
	IOTimerEventSource *fVblTimer = &timer;
	IOFBInterruptProc fVblProc = nullptr;
	OSObject *fVblTarget = nullptr;
	void *fVblRef = nullptr;
	bool fVblEnabled = false;
	bool fVblHwArmed = false;
	bool fDpmsOff = false;
	bool fStopping = false;
	bool timerAvailable = true;
	bool hwVblAvailable = false;
	uint32_t fVblOtg = 0u;
	uint64_t fakeNowNs = 1000000000ULL;
	uint64_t fVblLastHwNs = 0u;
	uint64_t fVblHwCount = 0u;
	uint64_t fVblTimerCount = 0u;
	unsigned hwSyncCalls = 0u;
	unsigned ackRecoverCalls = 0u;
	bool dispatchHeld = false;
	unsigned drainCalls = 0u;
	IOLock dpmsLock;
	IOLock *fDpmsLock = &dpmsLock;
	FakeOptc optc;
	FakeOptc *fPci = &optc;
	struct { bool found = true, isDp = true, auxValid = true; unsigned otg = 0, aux = 0; } fActiveOutput;
	unsigned fPowerState = 2, fConnPower = 1;
	IOLock *fCursorLock = nullptr;
	bool fCursorHwAvailable = false, fCursorNeedsProgram = false;
	unsigned wakeRetrains = 0;
	bool fDet0Expanded = true, fUclkForcePin = false, fUclkUnpinTripped = false;
	uint64_t fPeerGeneration = 7;
	enum class UclkFloor { Unmanaged, Pinned, MayBeLowered };
	UclkFloor fUclkFloor = UclkFloor::Pinned;
	thread_call_t fUclkWatchdog = nullptr;
	uintptr_t fUclkWatchGeneration = 0;
	uint8_t fUclkRecoveryAttempts = 0;
	uint64_t getRegistryEntryID() const { return 22; }
	void updateUclkPolicyLocked(GarnetDriver *, const rdna4det::Hw &, uint32_t, uint32_t, uint32_t, uint32_t,
	                            uint64_t, bool);
	uint64_t acquireUclkWatchLeaseLocked(GarnetDriver *);
	bool pinUclkForModesetLocked(GarnetDriver *, bool systemWake = false);
	void onSystemWake(GarnetDriver *);
	void drainSystemTransitions();
	bool applyDpmsLocked(bool wantOn, GarnetDriver *driver, bool systemWake) {
		if (!wantOn || !pinUclkForModesetLocked(driver, systemWake)) return false;
		++wakeRetrains;
		fDpmsOff = false;
		return true;
	}
	void optimizeDet0AndUclkLocked(GarnetDriver *driver) {
		MmioAccess mmio{&optc};
		const rdna4det::Hw hw{&mmio, cursor_rd_seg2, cursor_wr_seg2, aux_delay_us};
		updateUclkPolicyLocked(driver, hw, 3840u, 4000u, 2160u, 2222u, 533250000u, false);
	}
	void scheduleUclkWatchdogLocked();
	static void onUclkWatchdog(thread_call_param_t, thread_call_param_t);
	bool ensureVblTimer() { return timerAvailable; }
	unsigned vblPeriodUs() const { return 16667; }
	uint64_t vblNowNs() const { return fakeNowNs; }
	bool vblHwActive() const { return hwVblAvailable && __atomic_load_n(&fVblHwArmed, __ATOMIC_ACQUIRE); }
	uint32_t vblInitialTimeoutUs() const;
	void onHardwareVbl(uint32_t otg);
	void syncHardwareVbl(bool want) {
		++hwSyncCalls;
		__atomic_store_n(&fVblHwArmed, want && hwVblAvailable && !fStopping, __ATOMIC_RELEASE);
	}
	void recoverHardwareVblAckIfNeeded() {
		++ackRecoverCalls;
	}
	void vblDispatchEnter() { dispatchHeld = true; }
	void vblDispatchExit() { dispatchHeld = false; }
	void vblDispatchDrain() {
		++drainCalls;
		// Re-entrant unregisterInside runs while dispatchHeld == true without deadlocking.
	}
	static void vblRetainTarget(OSObject *t) { if (t) t->retain(); }
	static void vblReleaseTarget(OSObject *t) { if (t) t->release(); }
	static void onVblTimer(OSObject *, IOTimerEventSource *);
	IOReturn registerForInterruptType(IOSelect, IOFBInterruptProc, OSObject *, void *, void **);
	IOReturn unregisterInterrupt(void *);
	IOReturn setInterruptState(void *, UInt32);
	void fire() {
		if (timer.enabled && timer.scheduled) {
			timer.scheduled = false;
			onVblTimer(this, &timer);
		}
	}
};

enum : UInt32 {
	kIOMessageCanSystemSleep, kIOMessageSystemWillSleep, kIOMessageSystemWillPowerOff,
	kIOMessageSystemWillRestart, kIOMessageSystemWillNotSleep, kIOMessageSystemWillPowerOn,
	kIOMessageSystemHasPoweredOn,
};
struct WakeTransition {
	std::mutex mutex;
	std::condition_variable changed;
	unsigned sequence = 0, reachedAux = 0, readAux = 0, systemAck = 0;
	bool reachedDrain = false;
	bool pauseWakeDispatch = false, pausedWakeDispatch = false, releaseWakeDispatch = false;
	template<class Predicate> void wait(std::unique_lock<std::mutex> &lock, Predicate predicate) {
		if (!changed.wait_for(lock, std::chrono::seconds(5), predicate)) std::abort();
	}
};
static WakeTransition *wakeTransition = nullptr;
static AMDGarnetPrimaryFramebuffer *activePrimary = nullptr;
static AMDGarnetPrimaryFramebuffer *retain_primary_fb() {
	if (wakeTransition && wakeTransition->pauseWakeDispatch) {
		auto &transition = *wakeTransition;
		std::unique_lock<std::mutex> lock(transition.mutex);
		if (!transition.pausedWakeDispatch) {
			transition.pausedWakeDispatch = true;
			transition.changed.notify_all();
			transition.wait(lock, [&] { return transition.releaseWakeDispatch; });
		}
	}
	if (activePrimary) activePrimary->retain();
	return activePrimary;
}
static IOReturn acknowledgeSleepWakeNotification(void *argument) {
	auto &transition = *static_cast<WakeTransition *>(argument);
	std::lock_guard<std::mutex> lock(transition.mutex);
	transition.systemAck = ++transition.sequence;
	transition.changed.notify_all();
	return kIOReturnSuccess;
}

#include "primary_vbl_actual.inc"

namespace {
int checks = 0;
int failures = 0;
void expect(bool condition, const char *message) {
	++checks;
	if (!condition) {
		++failures;
		std::printf("FAIL: %s\n", message);
	}
}
struct CallbackState {
	AMDGarnetPrimaryFramebuffer *framebuffer;
	OSObject *target;
	void *interrupt = nullptr;
	unsigned calls = 0;
	bool disableInside = false;
	bool unregisterInside = false;
};
void callback(OSObject *target, void *reference) {
	auto *state = static_cast<CallbackState *>(reference);
	expect(target == state->target, "callback receives its registered target");
	expect(!state->framebuffer->lock.held, "callback runs outside the registration lock");
	++state->calls;
	if (state->disableInside) state->framebuffer->setInterruptState(state->interrupt, 0);
	if (state->unregisterInside) state->framebuffer->unregisterInterrupt(state->interrupt);
}

void fireUclkWatchdog() {
	if (fakeCall.pending) {
		fakeCall.pending = false;
		fakeCall.callback(fakeCall.owner, fakeCall.arg);
	}
}

void lateUclkUnderflow() {
	GarnetDriver driver;
	fakeDriver = &driver;
	AMDGarnetPrimaryFramebuffer fb;
	MmioAccess mmio{&fb.optc};
	const rdna4det::Hw hw{&mmio, cursor_rd_seg2, cursor_wr_seg2, aux_delay_us};
	IOLockLock(fb.fDpmsLock);
	fb.updateUclkPolicyLocked(&driver, hw, 3840u, 4000u, 2160u, 2222u, 533250000u, false);
	IOLockUnlock(fb.fDpmsLock);
	expect(!driver.dev.pinned && fb.optc.elapsedUs == 250000u, "clean 250ms trial allows legacy UCLK unpin");
	fb.optc.elapsedUs = 251000u;
	fb.optc.value = rdna4det::kOptcUnderflowMask;
	driver.dev.failPin = true;
	fireUclkWatchdog();
	expect(!driver.dev.pinned && fb.fUclkUnpinTripped && fakeCall.pending,
	       "failed first recovery leaves the floor unknown and schedules a bounded retry");
	driver.dev.failPin = false;
	fireUclkWatchdog();
	expect(driver.dev.pinned && fb.fUclkUnpinTripped,
	       "same-owner OPTC0 underflow after 250ms restores and latches the UCLK floor");
	fakeDriver = nullptr;
}

void lostUclkObservation() {
	GarnetDriver driver;
	fakeDriver = &driver;
	AMDGarnetPrimaryFramebuffer fb;
	MmioAccess mmio{&fb.optc};
	const rdna4det::Hw hw{&mmio, cursor_rd_seg2, cursor_wr_seg2, aux_delay_us};
	IOLockLock(fb.fDpmsLock);
	fb.updateUclkPolicyLocked(&driver, hw, 3840u, 4000u, 2160u, 2222u, 533250000u, false);
	IOLockUnlock(fb.fDpmsLock);
	const auto reads = fb.optc.reads;
	driver.contended = true;
	fireUclkWatchdog();
	expect(fb.fUclkUnpinTripped && fakeCall.pending && !driver.dev.pins && fb.optc.reads == reads,
	       "lease contention trips observation without touching unowned OPTC or UCLK");
	driver.contended = false;
	driver.dev.failPin = true;
	for (unsigned attempt = 0; attempt < 4; ++attempt) fireUclkWatchdog();
	expect(driver.dev.pins == 3 && !driver.dev.pinned && !fakeCall.pending && fb.optc.reads == reads,
	       "unobserved mode tries recovery at most three times without relabelling the floor or clearing OPTC");
	fakeDriver = nullptr;
}

void staleUclkWatchdog() {
	GarnetDriver driver;
	fakeDriver = &driver;
	AMDGarnetPrimaryFramebuffer fb;
	MmioAccess mmio{&fb.optc};
	const rdna4det::Hw hw{&mmio, cursor_rd_seg2, cursor_wr_seg2, aux_delay_us};
	IOLockLock(fb.fDpmsLock);
	fb.updateUclkPolicyLocked(&driver, hw, 3840u, 4000u, 2160u, 2222u, 533250000u, false);
	IOLockUnlock(fb.fDpmsLock);
	const auto reads = fb.optc.reads;
	driver.bringup.smu.sleepPending = true;
	fireUclkWatchdog();
	expect(!driver.dev.pins && fb.optc.reads == reads && fakeCall.pending,
	       "sleep-pending callback defers without dropping monitoring if sleep is cancelled");
	driver.bringup.smu.sleepPending = false;
	++fb.fUclkWatchGeneration;
	fireUclkWatchdog();
	expect(!driver.dev.pins && fb.optc.reads == reads && !fakeCall.pending,
	       "prior modeset callback stops without sampling the next generation");
	fakeDriver = nullptr;
}

void sleepingUclkPolicy() {
	GarnetDriver driver;
	driver.bringup.smu.sleepPending = true;
	AMDGarnetPrimaryFramebuffer fb;
	using Floor = AMDGarnetPrimaryFramebuffer::UclkFloor;
	fb.fUclkFloor = Floor::Unmanaged;
	MmioAccess mmio{&fb.optc};
	const rdna4det::Hw hw{&mmio, cursor_rd_seg2, cursor_wr_seg2, aux_delay_us};
	IOLockLock(fb.fDpmsLock);
	fb.updateUclkPolicyLocked(&driver, hw, 3840u, 4000u, 2160u, 2222u, 533250000u, false);
	expect(!driver.dev.pins && !fb.optc.reads, "sleep-pending device performs no UCLK or OPTC operation");
	driver.bringup.smu.wakeUnverified = true;
	expect(fb.pinUclkForModesetLocked(&driver), "unmanaged mode without forced pin adds no UCLK wake requirement");
	fb.fUclkFloor = Floor::MayBeLowered;
	expect(!fb.pinUclkForModesetLocked(&driver) && !driver.dev.pins,
	       "S3 resume without PMFW proof cannot relight DP when the previous UCLK floor may be lowered");
	fb.fUclkFloor = Floor::Unmanaged;
	fb.fUclkForcePin = true;
	expect(!fb.pinUclkForModesetLocked(&driver) && !driver.dev.pins,
	       "S3 resume without PMFW proof also blocks a forced-pin DP wake");
	driver.bringup.smu = {};
	++driver.generation;
	expect(!fb.pinUclkForModesetLocked(&driver) && !driver.dev.pins,
	       "awake device with a stale canonical generation performs no pre-modeset pin");
	IOLockUnlock(fb.fDpmsLock);
}

void displayOnlyWake() {
	using Floor = AMDGarnetPrimaryFramebuffer::UclkFloor;
	GarnetDriver driver;
	driver.dev.graphicsQuarantined = true;
	driver.dev.pinned = false;
	driver.bringup.smu = {true, true, true};
	AMDGarnetPrimaryFramebuffer fb;
	fb.fDpmsOff = true;
	fb.fUclkFloor = Floor::MayBeLowered;
	fb.onSystemWake(&driver);
	expect(!fb.fDpmsOff && fb.wakeRetrains == 1 && driver.bringup.smu.wakePins == 1 && driver.dev.pinned,
	       "a fresh display-only wake pin permits the DPMS-off path to retrain once");
	expect(driver.dev.graphicsPowerQuarantined() && !driver.dev.pins && !driver.dev.unpins,
	       "display wake does not release graphics quarantine or enter the ordinary UCLK policy");

	driver.bringup.smu.postWake = false;
	driver.dev.pinned = false;
	fb.fDpmsOff = true;
	fb.onSystemWake(&driver);
	expect(fb.fDpmsOff && fb.wakeRetrains == 1 && driver.bringup.smu.wakePins == 1 && !driver.dev.pinned,
	       "a callback during the next pending sleep cannot reuse the old floor for DP wake");

	driver.bringup.smu.postWake = true;
	fb.onSystemWake(&driver);
	expect(!fb.fDpmsOff && fb.wakeRetrains == 2 && driver.bringup.smu.wakePins == 2 && driver.dev.pinned,
	       "the next real postwake window needs its own fresh hardware floor confirmation");

	for (unsigned refusal = 0; refusal < 5; ++refusal) {
		GarnetDriver denied;
		denied.dev.graphicsQuarantined = true;
		denied.bringup.smu = {true, true, true};
		if (refusal == 0) denied.wakePciReady = false;
		if (refusal == 1) denied.bringup.smuVersion = 0;
		if (refusal == 2) ++denied.generation;
		if (refusal == 3) denied.dev.failPin = true;
		if (refusal == 4) denied.bringup.smuVersion ^= 1u;
		AMDGarnetPrimaryFramebuffer visible;
		visible.onSystemWake(&denied);
		expect(!visible.optc.maps && denied.bringup.smu.wakePins == (refusal >= 3 ? 1u : 0u),
		       "visible display leaves AUX untouched unless ownership, PCI, boot version and hard ACK are valid");
	}

	GarnetDriver allowed;
	allowed.dev.graphicsQuarantined = true;
	allowed.bringup.smu = {true, true, true};
	AMDGarnetPrimaryFramebuffer visible;
	visible.onSystemWake(&allowed);
	expect(visible.optc.maps == 1u && allowed.bringup.smu.wakePins == 1u,
	       "visible display restores AUX only after fresh hard ACK");
	GarnetDriver legacy;
	legacy.bringup.smu = {true, true, false};
	AMDGarnetPrimaryFramebuffer unmanaged;
	unmanaged.fUclkFloor = Floor::Unmanaged;
	unmanaged.onSystemWake(&legacy);
	expect(unmanaged.optc.maps == 1u && !legacy.bringup.smu.wakePins && !legacy.dev.pins,
	       "never-managed display keeps legacy wake without PMFW commands");
}
}

static void wakeSleepAcknowledgement() {
	for (const auto shutdown : {false, true}) {
		AMDGarnetPrimaryFramebuffer fb;
		GarnetDriver driver;
		driver.dev.graphicsQuarantined = true;
		driver.bringup.smu = {true, true, true};
		gBringup.smu = driver.bringup.smu;
		WakeTransition transition;
		wakeTransition = &transition;
		activePrimary = &fb;
		garnet_primary_fb_resume_system_transitions();
		fb.optc.beforeFirstRead = [] {
			auto &state = *wakeTransition;
			std::unique_lock<std::mutex> lock(state.mutex);
			state.reachedAux = ++state.sequence;
			state.changed.notify_all();
			state.wait(lock, [&] { return state.reachedDrain || state.systemAck; });
			state.readAux = ++state.sequence;
		};
		std::thread wake([&] { fb.onSystemWake(&driver); });
		{
			std::unique_lock<std::mutex> lock(transition.mutex);
			transition.wait(lock, [&] { return transition.reachedAux != 0; });
		}
		fb.dpmsLock.beforeLock = [] {
			auto &state = *wakeTransition;
			std::lock_guard<std::mutex> lock(state.mutex);
			state.reachedDrain = gBringup.smu.sleepPending && !gBringup.smu.postWake;
			state.changed.notify_all();
		};
		std::thread sleep([&] {
			GarnetDriver::sleepWakeCallback(&driver, nullptr,
			    shutdown ? kIOMessageSystemWillPowerOff : kIOMessageSystemWillSleep, nullptr, &transition, 0);
		});
		sleep.join();
		wake.join();
		std::printf("%s: reached AUX=%u; AUX read=%u; system ACK=%u\n",
		            shutdown ? "Shutdown" : "WillSleep", transition.reachedAux, transition.readAux, transition.systemAck);
		expect(driver.bringup.smu.wakePins == 1 && transition.readAux < transition.systemAck,
		       "sleep/shutdown ACK follows an already-admitted wake's AUX read");
		fb.dpmsLock.beforeLock = nullptr;
		const auto maps = fb.optc.maps;
		fb.fUclkFloor = AMDGarnetPrimaryFramebuffer::UclkFloor::Unmanaged;
		fb.onSystemWake(&driver);
		expect(fb.optc.maps == maps, "closed sleep admission also blocks a new unmanaged AUX path");
		gBringup.smu.wakeUnverified = false;
		GarnetDriver::sleepWakeCallback(&driver, nullptr, kIOMessageSystemWillNotSleep, nullptr, nullptr, 0);
		fb.onSystemWake(&driver);
		if (shutdown) {
			expect(fb.optc.maps == maps, "late cancellation cannot reopen shutdown admission");
		} else {
			expect(fb.optc.maps == maps + 1, "a genuine successful sleep cancellation reopens admission");
		}
		activePrimary = nullptr;
		wakeTransition = nullptr;
		gSystemDisplayShutdown = false;
		garnet_primary_fb_resume_system_transitions();
	}
}

static void staleSystemWakeDispatch() {
	AMDGarnetPrimaryFramebuffer fb;
	GarnetDriver driver;
	driver.bringup.smu = {true, true, true};
	gBringup.smu = driver.bringup.smu;
	WakeTransition transition;
	transition.pauseWakeDispatch = true;
	wakeTransition = &transition;
	activePrimary = &fb;
	std::thread wake([&] {
		GarnetDriver::sleepWakeCallback(&driver, nullptr, kIOMessageSystemHasPoweredOn, nullptr, nullptr, 0);
	});
	{
		std::unique_lock<std::mutex> lock(transition.mutex);
		transition.wait(lock, [&] { return transition.pausedWakeDispatch; });
	}
	GarnetDriver::sleepWakeCallback(&driver, nullptr, kIOMessageSystemWillSleep, nullptr, &transition, 0);
	{
		std::lock_guard<std::mutex> lock(transition.mutex);
		transition.releaseWakeDispatch = true;
		transition.changed.notify_all();
	}
	wake.join();
	expect(transition.systemAck && !fb.optc.maps && !driver.bringup.smu.wakePins,
	       "an older HasPoweredOn delayed before framebuffer dispatch cannot cross a new sleep ACK");
	activePrimary = nullptr;
	wakeTransition = nullptr;
	garnet_primary_fb_resume_system_transitions();
}

int main() {
	wakeSleepAcknowledgement();
	staleSystemWakeDispatch();
	lateUclkUnderflow();
	lostUclkObservation();
	staleUclkWatchdog();
	sleepingUclkPolicy();
	displayOnlyWake();
	AMDGarnetPrimaryFramebuffer framebuffer;
	OSObject target;
	CallbackState state { &framebuffer, &target };
	expect(framebuffer.registerForInterruptType(kIOFBVBLInterruptType, callback, &target,
	                                           &state, &state.interrupt) == kIOReturnSuccess,
	       "VBL registration succeeds");
	expect(framebuffer.fVblEnabled && framebuffer.timer.enabled && framebuffer.timer.scheduled,
	       "registration enables and schedules the first VBL without a separate enable call");
	framebuffer.fire();
	expect(state.calls == 1, "initial VBL reaches the registered callback");
	expect(framebuffer.timer.scheduled && framebuffer.timer.period == 16667,
	       "VBL callback schedules the next event at the current refresh period");
	framebuffer.fire();
	expect(state.calls == 2, "VBL delivery continues after the first event");
	expect(framebuffer.setInterruptState(state.interrupt, 0) == kIOReturnSuccess,
	       "explicit disable succeeds");
	expect(!framebuffer.timer.enabled && !framebuffer.timer.scheduled,
	       "explicit disable cancels the timer");
	const unsigned beforeDisable = state.calls;
	framebuffer.fire();
	expect(state.calls == beforeDisable, "disabled VBL never invokes the callback");
	expect(framebuffer.setInterruptState(state.interrupt, kEnabledInterruptState) == kIOReturnSuccess,
	       "explicit re-enable succeeds");
	framebuffer.fire();
	expect(state.calls == beforeDisable + 1, "re-enabled VBL resumes callbacks");
	state.disableInside = true;
	framebuffer.fire();
	expect(!framebuffer.timer.scheduled && !framebuffer.fVblEnabled,
	       "a callback may disable itself without being rescheduled");
	state.disableInside = false;
	framebuffer.setInterruptState(state.interrupt, kEnabledInterruptState);
	state.unregisterInside = true;
	framebuffer.fire();
	expect(!framebuffer.timer.scheduled && !framebuffer.fVblProc,
	       "a callback may unregister itself without being rescheduled");
	expect(framebuffer.setInterruptState(state.interrupt, kEnabledInterruptState) == kIOReturnNotReady,
	       "unregistered VBL cannot be re-enabled");
	expect(framebuffer.setInterruptState(&state, kEnabledInterruptState) == kIOReturnBadArgument,
	       "an unrelated interrupt reference is rejected");
	expect(framebuffer.unregisterInterrupt(&state) == kIOReturnBadArgument,
	       "unregister rejects an unrelated reference");
	state.unregisterInside = false;
	framebuffer.registerForInterruptType(kIOFBVBLInterruptType, callback, &target, &state, &state.interrupt);
	framebuffer.fStopping = true;
	const unsigned beforeStop = state.calls;
	framebuffer.fire();
	expect(state.calls == beforeStop && !framebuffer.timer.scheduled,
	       "stopping the framebuffer suppresses and stops VBL delivery");
	AMDGarnetPrimaryFramebuffer rejected;
	void *interrupt = nullptr;
	expect(rejected.registerForInterruptType(0, callback, &target, &state, &interrupt) == kIOReturnUnsupported,
	       "non-VBL interrupt registration remains unsupported");
	expect(rejected.registerForInterruptType(kIOFBVBLInterruptType, nullptr, &target, &state, &interrupt) == kIOReturnBadArgument,
	       "a missing callback is rejected");
	expect(rejected.registerForInterruptType(kIOFBVBLInterruptType, callback, &target, &state, nullptr) == kIOReturnBadArgument,
	       "a missing interrupt-reference output is rejected");
	rejected.timerAvailable = false;
	expect(rejected.registerForInterruptType(kIOFBVBLInterruptType, callback, &target, &state, &interrupt) == kIOReturnNoMemory,
	       "timer setup failure is reported");

	// Stage 3A: hardware VBL mode uses full refresh cadence while cold/stalled,
	// suppresses timer callbacks while HW VBL is fresh, and falls back cleanly on stall.
	AMDGarnetPrimaryFramebuffer hwFb;
	hwFb.hwVblAvailable = true;
	CallbackState hwState { &hwFb, &target };
	expect(hwFb.registerForInterruptType(kIOFBVBLInterruptType, callback, &target,
	                                     &hwState, &hwState.interrupt) == kIOReturnSuccess,
	       "HW VBL registration succeeds");
	expect(hwFb.fVblHwArmed && hwFb.timer.period == 16667u,
	       "cold HW VBL (no IRQ received yet) arms full refresh period instead of 100ms stall");
	hwFb.onHardwareVbl(0u);
	expect(hwState.calls == 1u && hwFb.fVblHwCount == 1u,
	       "hardware VBL delivers callback and stamps fVblLastHwNs");
	hwFb.fakeNowNs += 10000000ULL; // 10 ms < 2 * 16.667 ms
	hwFb.fire();
	expect(hwState.calls == 1u && hwFb.timer.period == rdna4vbl::kWatchdogMinUs,
	       "fresh HW VBL suppresses duplicate timer callback and switches to watchdog cadence");
	hwFb.fakeNowNs += 50000000ULL; // 60 ms >= 2 * 16.667 ms -> HW VBL stalled
	hwFb.fire();
	expect(hwState.calls == 2u && hwFb.timer.period == 16667u && hwFb.ackRecoverCalls == 1u,
	       "stalled HW VBL falls back to timer callback at full refresh cadence and probes ACK recovery");
	const unsigned syncBeforeToggle = hwFb.hwSyncCalls;
	expect(hwFb.setInterruptState(hwState.interrupt, 0) == kIOReturnSuccess &&
	       hwFb.fVblHwArmed && hwFb.hwSyncCalls == syncBeforeToggle,
	       "disabling VBL cancels timer without churning hardware VBL arm state");
	// Re-enabling while HW VBL is still stalled must arm periodUs, NOT 100 ms.
	expect(hwFb.setInterruptState(hwState.interrupt, kEnabledInterruptState) == kIOReturnSuccess &&
	       hwFb.hwSyncCalls == syncBeforeToggle && hwFb.timer.period == 16667u,
	       "re-enabling VBL while HW VBL is stalled keeps full refresh cadence (no 100ms stall)");
	// A hardware VBL arriving while fVblEnabled == false must still refresh fVblLastHwNs without firing callback.
	expect(hwFb.setInterruptState(hwState.interrupt, 0) == kIOReturnSuccess,
	       "disable VBL before background HW VBL tick");
	const unsigned callsBeforeBgHw = hwState.calls;
	hwFb.onHardwareVbl(0u);
	expect(hwState.calls == callsBeforeBgHw && hwFb.fVblLastHwNs == hwFb.fakeNowNs,
	       "background HW VBL refreshes freshness timestamp without invoking disabled callback");
	hwFb.fakeNowNs += 5000000ULL; // 5 ms < 2 * 16.667 ms
	expect(hwFb.setInterruptState(hwState.interrupt, kEnabledInterruptState) == kIOReturnSuccess &&
	       hwFb.timer.period == rdna4vbl::kWatchdogMinUs,
	       "re-enabling VBL after fresh background HW VBL uses low-rate watchdog cadence");
	expect(hwFb.unregisterInterrupt(hwState.interrupt) == kIOReturnSuccess &&
	       !hwFb.fVblHwArmed && hwFb.drainCalls == 1u,
	       "unregisterInterrupt disarms hardware VBL and invokes synchronous dispatch drain barrier");
	expect(target.retainCount == 1 && !hwFb.dispatchHeld && !framebuffer.dispatchHeld,
	       "target retain/release and dispatch lock stay balanced across normal and re-entrant paths");

	std::printf("primary_vbl_test: %d checks, %d failed\n", checks, failures);
	return failures ? 1 : 0;
}
