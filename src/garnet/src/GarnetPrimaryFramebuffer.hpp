//
//  GarnetPrimaryFramebuffer.hpp — Built-in IOFramebuffer primary display personality for Garnet.
//
#pragma once

#include <IOKit/IOLocks.h>
#include <IOKit/graphics/IOFramebuffer.h>
#include <kern/thread.h>

#include "dcn/dcn41_cursor_pure.h"
#include "dcn/dcn41_ddc_pad_pure.h"
#include "dcn/dcn41_det_pure.h"
#include "dcn/dcn41_vbl_irq_pure.h"
#include "dcn/dp_edid_aux.h"
#include "dcn/primary_fb_pure.h"

class IOPCIDevice;
class IOWorkLoop;
class IOTimerEventSource;
class GarnetDriver;

void garnet_primary_fb_notify_driver_ready(GarnetDriver *driver);
void garnet_primary_fb_notify_wake(GarnetDriver *driver);
void garnet_primary_fb_notify_vbl(uint32_t otg);
IOMemoryDescriptor *rdna4_scanout_descriptor(bool viaFramebuffer);

// Primary IOFramebuffer matched directly against the AMD RDNA4 PCI device (score 200000).
// Named with the "AMD" class prefix so macOS CoreDisplay::GetGPUVendorForFramebufferService
// classifies it natively as AMD without process-local dyld offset patching
// (insight credited to RX4Darwin/AMDRadeonRDNA4.kext commit b4751220, BSD 3-Clause).
// Declares ONLY overrides of existing IOService / IOFramebuffer virtual methods so the
// vtable layout remains strictly identical to macOS IOFramebuffer (350 slots).
class AMDGarnetPrimaryFramebuffer : public IOFramebuffer {
	OSDeclareDefaultStructors(AMDGarnetPrimaryFramebuffer)
	using super = IOFramebuffer;

public:
	void free() override;
	IOService *probe(IOService *provider, SInt32 *score) override;
	bool start(IOService *provider) override;
	void stop(IOService *provider) override;

	bool isConsoleDevice() override;
	IODeviceMemory *getApertureRange(IOPixelAperture aperture) override;
	IODeviceMemory *getVRAMRange() override;
	IOReturn enableController() override;

	const char *getPixelFormats() override;
	IOItemCount getDisplayModeCount() override;
	IOReturn getDisplayModes(IODisplayModeID *allDisplayModes) override;
	IOReturn getInformationForDisplayMode(IODisplayModeID displayMode,
	                                      IODisplayModeInformation *info) override;
	UInt64 getPixelFormatsForDisplayMode(IODisplayModeID displayMode, IOIndex depth) override;
	IOReturn getPixelInformation(IODisplayModeID displayMode, IOIndex depth,
	                             IOPixelAperture aperture,
	                             IOPixelInformation *pixelInfo) override;
	IOReturn getCurrentDisplayMode(IODisplayModeID *displayMode, IOIndex *depth) override;
	IOReturn setDisplayMode(IODisplayModeID displayMode, IOIndex depth) override;
	IOReturn getStartupDisplayMode(IODisplayModeID *displayMode, IOIndex *depth) override;

	IOReturn setAttribute(IOSelect attribute, uintptr_t value) override;
	IOReturn getAttribute(IOSelect attribute, uintptr_t *value) override;
	IOReturn getTimingInfoForDisplayMode(IODisplayModeID displayMode,
	                                     IOTimingInformation *info) override;

	IOItemCount getConnectionCount() override;
	IOReturn setAttributeForConnection(IOIndex connectIndex, IOSelect attribute,
	                                   uintptr_t value) override;
	IOReturn getAttributeForConnection(IOIndex connectIndex, IOSelect attribute,
	                                   uintptr_t *value) override;
	bool hasDDCConnect(IOIndex connectIndex) override;
	IOReturn getDDCBlock(IOIndex connectIndex, UInt32 blockNumber, IOSelect blockType,
	                     IOOptionBits options, UInt8 *data, IOByteCount *length) override;
	IOReturn registerForInterruptType(IOSelect interruptType,
	                                  IOFBInterruptProc proc, OSObject *target,
	                                  void *ref, void **interruptRef) override;
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Woverloaded-virtual"
	IOReturn unregisterInterrupt(void *interruptRef) override;
#pragma clang diagnostic pop
	IOReturn setInterruptState(void *interruptRef, UInt32 state) override;
	IOReturn setCursorImage(void *cursorImage) override;
	IOReturn setCursorState(SInt32 x, SInt32 y, bool visible) override;

	void onDriverReady(GarnetDriver *driver);
	void onSystemWake(GarnetDriver *driver);
	void onHardwareVbl(uint32_t otg);

private:
	void initForPM();
	void readProviderEdid(IOService *provider);
	bool readLiveHardwareEdid(IOPCIDevice *pci, GarnetDriver *driver);
	void queryClockCaps(IOPCIDevice *pci, GarnetDriver *driver);
	void rebuildModeTable(uint32_t w, uint32_t h, uint32_t rb);
	bool applyHardwareModeLocked(const rdna4edidmodes::DetailedTiming &target,
	                             bool forceRetrain = false,
	                             GarnetDriver *knownDriver = nullptr);
	bool applyDpmsLocked(bool wantOn, GarnetDriver *knownDriver = nullptr);
	void updateUclkPolicyLocked(GarnetDriver *driver, const rdna4det::Hw &detHw,
	                            uint32_t hActive, uint32_t hTotal,
	                            uint32_t vActive, uint32_t vTotal,
	                            uint64_t pixelClockHz, bool dscEnabled);
	void optimizeDet0AndUclkLocked(GarnetDriver *driver);
	bool captureConsoleAndBar0(IOPCIDevice *pci);
	bool ensureVblTimer();
	void teardownVblTimer();
	uint32_t vblPeriodUs() const;
	uint64_t vblNowNs() const;
	bool vblHwActive() const;
	uint32_t vblInitialTimeoutUs() const;
	void syncHardwareVbl(bool want);
	void recoverHardwareVblAckIfNeeded();
	void vblDispatchEnter();
	void vblDispatchExit();
	void vblDispatchDrain();
	static void vblRetainTarget(OSObject *target);
	static void vblReleaseTarget(OSObject *target);
	static void onVblTimer(OSObject *owner, IOTimerEventSource *sender);
	void initCursorSlot(GarnetDriver *driver);
	void teardownCursorSlot();
	void disableCursorLocked(GarnetDriver *driver);
	uint32_t currentPixelClockKhz() const;

	IOPCIDevice *fPci { nullptr };
	rdna4fbprimary::ModeTable fModeTable {};
	rdna4aux::ActiveOutput fActiveOutput {};
	rdna4aux::DpcdCaps fDpcdCaps {};
	uint32_t fMaxDispClkKhz { 0u };
	uint32_t fMaxDppClkKhz { 0u };
	uint8_t fActiveLinkRate { 0u };
	bool fActiveDscOn { false };
	uint8_t fEdid[rdna4edidmodes::BlockSize * rdna4edidmodes::MaxBlocks] {};
	size_t fEdidLen { 0u };
	uint64_t fBar0Phys { 0u };
	uint64_t fBar0Size { 0u };
	uint64_t fConsolePhys { 0u };
	uint64_t fConsoleLen { 0u };
	IODisplayModeID fCurrentMode { 1 };
	IOIndex fCurrentDepth { 0 };
	uint32_t fPowerState { 2u };
	uint32_t fConnPower { 1u };
	IOLock *fDpmsLock { nullptr };
	bool fDpmsAllowed { true };
	bool fDpmsOff { false };
	bool fDpmsSavedCursorVisible { false };
	bool fDet0Allowed { true };
	bool fUclkForcePin { false };
	bool fDet0Expanded { false };
	bool fUclkUnpinned { false };
	bool fPmRegistered { false };
	bool fPeerRegistered { false };
	uint64_t fPeerGeneration { 0u };
	IOLock *fVblLock { nullptr };
	IOLock *fVblDispatchLock { nullptr };
	thread_t fVblDispatchOwner { nullptr };
	IOWorkLoop *fVblWorkLoop { nullptr };
	IOTimerEventSource *fVblTimer { nullptr };
	IOFBInterruptProc fVblProc { nullptr };
	OSObject *fVblTarget { nullptr };
	void *fVblRef { nullptr };
	bool fVblEnabled { false };
	bool fVblIrqAllowed { true };
	bool fVblHwArmed { false };
	uint32_t fVblOtg { 0u };
	uint64_t fVblLastHwNs { 0u };
	uint64_t fVblHwCount { 0u };
	uint64_t fVblTimerCount { 0u };
	bool fStopping { false };

	IOLock *fCursorLock { nullptr };
	IODeviceMemory *fCursorMem { nullptr };
	IOMemoryMap *fCursorMap { nullptr };
	uint32_t *fCursorVram { nullptr };
	uint32_t *fCursorStaging { nullptr };
	uint64_t fCursorMcAddr { 0u };
	uint32_t fCursorPipe { 0u };
	uint32_t fCursorW { 0u };
	uint32_t fCursorH { 0u };
	uint32_t fCursorHotX { 0u };
	uint32_t fCursorHotY { 0u };
	int32_t fCursorX { 0 };
	int32_t fCursorY { 0 };
	bool fCursorVisible { false };
	bool fCursorHwAvailable { false };
	bool fCursorNeedsProgram { false };
	bool fCursorDpmsSuspended { false };
};
