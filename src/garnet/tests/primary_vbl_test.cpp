#include "dcn/dcn41_vbl_irq_pure.h"
#include <cstdint>
#include <cstdio>

using IOSelect = uint32_t;
using UInt32 = uint32_t;
using IOReturn = int;
constexpr IOSelect kIOFBVBLInterruptType = 0x76626c20u;
constexpr IOReturn kIOReturnSuccess = 0;
constexpr IOReturn kIOReturnUnsupported = 1;
constexpr IOReturn kIOReturnBadArgument = 2;
constexpr IOReturn kIOReturnNoMemory = 3;
constexpr IOReturn kIOReturnNotReady = 4;
constexpr UInt32 kEnabledInterruptState = 1;

struct OSObject {
	int retainCount = 1;
	void retain() { ++retainCount; }
	void release() { --retainCount; }
};
struct IOLock { bool held = false; };
void IOLockLock(IOLock *lock) { lock->held = true; }
void IOLockUnlock(IOLock *lock) { lock->held = false; }
#define OSDynamicCast(type, object) static_cast<type *>(object)

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
	bool fStopping = false;
	bool timerAvailable = true;
	bool hwVblAvailable = false;
	uint64_t fakeNowNs = 1000000000ULL;
	uint64_t fVblLastHwNs = 0u;
	uint64_t fVblTimerCount = 0u;
	unsigned hwSyncCalls = 0u;
	bool dispatchHeld = false;
	unsigned drainCalls = 0u;
	bool ensureVblTimer() { return timerAvailable; }
	unsigned vblPeriodUs() const { return 16667; }
	uint64_t vblNowNs() const { return fakeNowNs; }
	bool vblHwActive() const { return hwVblAvailable && __atomic_load_n(&fVblHwArmed, __ATOMIC_ACQUIRE); }
	uint32_t vblInitialTimeoutUs() const { return rdna4vbl::initial_timer_us(vblPeriodUs(), vblHwActive()); }
	void syncHardwareVbl(bool want) {
		++hwSyncCalls;
		__atomic_store_n(&fVblHwArmed, want && hwVblAvailable && !fStopping, __ATOMIC_RELEASE);
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
}

int main() {
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

	// Stage 3A: hardware VBL mode suppresses timer callbacks while HW VBL is fresh,
	// and falls back seamlessly to timer cadence if HW VBL stalls.
	AMDGarnetPrimaryFramebuffer hwFb;
	hwFb.hwVblAvailable = true;
	CallbackState hwState { &hwFb, &target };
	expect(hwFb.registerForInterruptType(kIOFBVBLInterruptType, callback, &target,
	                                     &hwState, &hwState.interrupt) == kIOReturnSuccess,
	       "HW VBL registration succeeds");
	expect(hwFb.fVblHwArmed && hwFb.timer.period == rdna4vbl::kWatchdogMinUs,
	       "HW VBL schedules timer at low-rate watchdog cadence");
	hwFb.fVblLastHwNs = hwFb.fakeNowNs;
	hwFb.fakeNowNs += 10000000ULL; // 10 ms < 2 * 16.667 ms
	hwFb.fire();
	expect(hwState.calls == 0u && hwFb.timer.period == rdna4vbl::kWatchdogMinUs,
	       "fresh HW VBL suppresses duplicate timer callback and preserves watchdog cadence");
	hwFb.fakeNowNs += 50000000ULL; // 60 ms >= 2 * 16.667 ms -> HW VBL stalled
	hwFb.fire();
	expect(hwState.calls == 1u && hwFb.timer.period == 16667u,
	       "stalled HW VBL falls back to timer callback at full refresh cadence");
	expect(hwFb.setInterruptState(hwState.interrupt, 0) == kIOReturnSuccess && !hwFb.fVblHwArmed,
	       "disabling VBL disarms hardware VBL source");
	expect(hwFb.unregisterInterrupt(hwState.interrupt) == kIOReturnSuccess && hwFb.drainCalls == 1u,
	       "unregisterInterrupt invokes synchronous dispatch drain barrier");
	expect(target.retainCount == 1 && !hwFb.dispatchHeld && !framebuffer.dispatchHeld,
	       "target retain/release and dispatch lock stay balanced across normal and re-entrant paths");

	std::printf("primary_vbl_test: %d checks, %d failed\n", checks, failures);
	return failures ? 1 : 0;
}
