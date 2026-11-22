//
//  GarnetDriver.hpp — shared RDNA 4 GPU bring-up service
//  on x86 macOS.
//
//  A plain IOService on the GPU's IOPCIDevice, opt-in via boot-arg
//  "garnet=1" (legacy "rdna4bringup=1" accepted as fallback).
//
//  Stages (each gated; later ones only run when explicitly requested):
//    survey (default)      read-only: BAR5, on-die IP discovery, IP/PSP/SMU state
//    garnet-smu=1          read-only SMU TestMessage + version (benign mailbox)
//    garnet-psp=1          FIRST WRITING STAGE: PSP bootloader chain -> SOS alive
//
//  VRAM use: everything this kext puts in VRAM lives at `vramBase`, chosen at
//  runtime to sit above the UEFI boot framebuffer (which the video console keeps
//  drawing into) and inside the BAR0 CPU aperture.
//
#ifndef GarnetDriver_hpp
#define GarnetDriver_hpp

#include <IOKit/IOService.h>
#include <IOKit/IOPlatformExpert.h>
#include <IOKit/pci/IOPCIDevice.h>
#include <IOKit/IOMemoryDescriptor.h>
#include <IOKit/IOWorkLoop.h>
#include <IOKit/IOInterruptEventSource.h>
#include <pexpert/pexpert.h>

#include "ipdiscovery.hpp"
#include "psp.hpp"
#include "amd/amdgpu_regs.h"
#include "amd/rdna4_log.h"
#include "amd/rdna4_sleep_pure.h"
#include "amd/rebar_pure.h"   // 0.0.663: Resizable BAR planning (pure)
#include "amd/rebar_bartest_pure.h"   // high-VRAM direct CPU BAR <-> GPU bidirectional test (pure)
#include "dcn/display_state_pure.h"   // in-kext display state & primary hand-off (pure)
#include "amd/amdgpu_init.h"
struct RDNA4Counters;

#define RDNA4LOG(fmt, ...) ::amdgpu::rdna4_logf("Garnet: " fmt "\n", ##__VA_ARGS__)

// Retained lifetime pin for kernel peers querying the bring-up service.
IOService *garnet_driver_service_retained(void);

class GarnetDriver : public IOService {
	OSDeclareDefaultStructors(GarnetDriver)
	using super = IOService;

public:
	IOService *probe(IOService *provider, SInt32 *score) override;
	bool       start(IOService *provider) override;
	void       stop(IOService *provider) override;
	void       free() override;

	uint32_t   registerPrimaryFramebuffer(IOService *fb, const rdna4peer::PrimaryView &candidate,
	                                      rdna4peer::Snapshot *outSnapshot = nullptr,
	                                      rdna4peer::PrimaryView *outStored = nullptr);
	uint32_t   unregisterPrimaryFramebuffer(IOService *fb, uint64_t expectedGeneration = 0);
	bool       queryPrimaryFramebuffer(rdna4peer::Snapshot *outSnapshot,
	                                   rdna4peer::PrimaryView *outPrimary,
	                                   IOService **outRetainedFb = nullptr) const;
	bool       snapshotDisplayState(rdna4peer::Snapshot *outSnapshot) const;

	// --- userspace access (GarnetUserClient, Stage A6) ---
	IOReturn newUserClient(task_t owningTask, void *securityID, UInt32 type,
	                       OSDictionary *properties, IOUserClient **handler) override;
	amdgpu::BringupContext *bringupContext();
	amdgpu::DeviceContext  *deviceContext() { return &dev; }
	const IpDiscovery      &ipDisc() const { return ipDiscovery; }
	bool interruptsArmed() const { return intArmed; }
	// Verb dispatch called from the user client, never from start().
	// outExtra, when non-null, is an array of kAccelExtraScalars verb-specific
	// u64s handed straight back to userspace as scalarOutput[3..]. Rule 14: a
	// verb whose result matters must not depend on the shared log buffer.
	// 13 is the CEILING, not a preference: IOConnectCallScalarMethod's
	// io_scalar_inband64_t is uint64_t[16] (device_types.h:105) and the extras
	// start at scalarOutput[3], so 3 + 13 = 16 uses every slot there is. A 14th
	// extra would be dropped silently on the way out, which is exactly the kind
	// of quiet loss rule 14 exists to prevent.
	static constexpr unsigned kAccelExtraScalars = 13;
	// 0.0.194: `argScalar` is scalarInput[1] — a verb-specific argument, 0 when
	// userspace sent only the action. `vmib` (42) reads it as the VA to walk.
	IOReturn accelExperiment(uint32_t action, uint64_t *outInstalled,
	                         uint64_t *outTtlCalls, uint64_t *outFirstUnsupported,
	                         uint64_t *outExtra = nullptr, uint64_t argScalar = 0);
	void noteSubmit() { submits++; }
	void noteFence(bool ok) { if (ok) fencesCompleted++; else fenceTimeouts++; }
	void fillCounters(struct RDNA4Counters *out);

	// --- hardware access used by the stage modules (via rdna4::HwAccess bridge) ---
	uint32_t regReadIp (uint16_t hwId, uint8_t inst, uint8_t seg, uint32_t dword);
	bool     regWriteIp(uint16_t hwId, uint8_t inst, uint8_t seg, uint32_t dword, uint32_t value);
	bool     vramWrite (uint64_t vramOffset, const void *src, size_t len);   // via BAR0, fenced
	bool     vramMemset(uint64_t vramOffset, uint8_t value, size_t len);     // via BAR0, fenced
	// 0.0.613 (the display pipe's v1 present): the console buffer (the GOP / RDNA4FB scanout) inside BAR0. consoleGeometry is false until the boot capture found it inside BAR0 and its
	// length equals rowBytes * height; consoleWrite is bounds-checked against the console's length (overflow-safe) before the fenced BAR0 write.
	bool     consoleGeometry(uint64_t *off, uint64_t *len, uint32_t *w, uint32_t *h, uint32_t *rowBytes) const;
	bool     consoleWrite(uint64_t consoleOff, const void *src, size_t len);
	bool     bar0WriteCombined() const { return bar0Map && (bar0Map->getMapOptions() & kIOMapWriteCombineCache) != 0; }   // 0.0.615 (W1): the option the BAR0 map was made with (the same test mapVramAperture logs)
	bool     displayGateBegin(uint32_t kind, uint64_t expectedFbEntryID, uint64_t *token);
	void     displayGateEnd(uint64_t token);
	bool     displayGateHeld() const;
	bool     displayGateHeldFor(uint64_t expectedFbEntryID) const;
	bool     displayPeerActive() const;        // READY snapshot and gate at one generation

private:
	// --- register harness (BAR5) ---
	bool     mapRegisters();
	void     unmapRegisters();
	uint32_t regRead32 (uint32_t byteOffset) const;
	bool     regWrite32(uint32_t byteOffset, uint32_t value);
	uint32_t vramRead32(uint64_t pos) const;           // MM_INDEX/MM_DATA indirect
	void     vramWrite32(uint64_t pos, uint32_t value); // MM_INDEX/MM_DATA indirect

	// --- VRAM aperture (BAR0, CPU-visible window onto the start of VRAM) ---
	bool mapVramAperture();
	void unmapVramAperture();
	uint64_t bar0AccessLimit() const;
	uint32_t bar0Read32(uint64_t vramOffset) const;
	bool captureBootFramebuffer();      // where the UEFI/GOP console lives in VRAM
	bool chooseVramBase();              // pick vramBase clear of it
	bool apertureCheck(uint64_t off);   // BAR0 write == MM-window read at the same offset? (0.0.663: the original bytes are saved and RESTORED)
	bool finishVramPlan();              // 0.0.663 (ReBAR item 2/3): re-plan with the console located; sets bar0Limit or refuses
	void dropBar0();                    // 0.0.664 (F4): unmap BAR0 and clear every BAR0 field: exactly the 'no BAR0' state
	bool apertureCheckWindow();         // 0.0.663 (ReBAR item 4): apertureCheck at vramBase, the middle and vramLimit - 64 KiB, before the allocator exists
	IOReturn runHighBarBidirTest(uint64_t arg, uint64_t out[kAccelExtraScalars]);
	bool buildDisplayPeerSnapshot(uint32_t state, rdna4peer::Snapshot &out, uint32_t &reason) const;
	void publishDisplayPeer(uint32_t state);   // identity/state/one reservation table; no GRNT selector
	void publishDisplayDiagnostics(const rdna4peer::Snapshot &s,
	                               const rdna4peer::GateState &g,
	                               const rdna4peer::PrimaryView &p,
	                               bool primaryService, const char *status);
	bool displayPeerCallerUnderGpu(IOService *caller) const;
	bool displayPeerCallerOnGpu(IOService *caller, uint64_t framebufferEntryID) const;
	bool waitForDisplayLeasesAndReleasePrimary();

	// --- stages ---
	bool probeMemSize();
	bool loadOnDieDiscovery();
	bool selectAsic(bool haveDiscovery);   // Pick a supported chip and its die geometry, or refuse startup.
	void surveyIPs();
	void surveyPSP();
	void surveySMU();
	void stagePSP();       // garnet-psp=1 (== garnet-stage=5)
	bool buildDeviceContext();   // fills `dev` for the ported amdgpu modules
	bool mapDoorbells();         // BAR2 -> dev.bar2 (CP/MES/SDMA stages)
	void runStages(uint32_t target);   // garnet-stage=N: mac-amdgpu BringupStage ladder beyond SOS

	// --- interrupts (garnet-interrupts=1) ---
	// The GPU signals the host with a PCI message-signalled interrupt when the
	// IH ring goes non-empty. Until 0.0.18 the ring was brought up with
	// IH_RB_CNTL.ENABLE_INTR = 0 and drained by polling; this arms the real
	// path: MSI index -> IOInterruptEventSource on our own work loop -> drain
	// and classify. Gated because an interrupt we fail to ack is a live-lock,
	// and the ladder's polled path is the known-good fallback.
	bool armInterrupts();
	void disarmInterrupts();
	static void interruptTrampoline(OSObject *owner, IOInterruptEventSource *src, int count);
	void handleInterrupt(int count);
	void publishInterruptCounters();
	void logPciSleepState(const char *tag);
	static IOReturn sleepWakeCallback(void *target, void *refCon, UInt32 messageType,
	                                  IOService *provider, void *messageArgument, vm_size_t argSize);

	IOWorkLoop             *workLoop  { nullptr };
	IOInterruptEventSource *intSource { nullptr };
	IONotifier             *sleepNotifier { nullptr };
	IOLock                 *sleepLock { nullptr };
	rdna4sleep::State       sleepState {};
	bool     intArmed     { false };
	uint64_t irqCount     { 0 };   // times the handler ran
	uint64_t irqEntries   { 0 };   // IH entries drained
	uint64_t irqEop       { 0 };   // src 181 CP end-of-pipe
	uint64_t irqFaults    { 0 };   // src 0 UTCL2/VM fault
	uint64_t irqCpError   { 0 };   // src 183/184/185 command-stream errors
	uint64_t irqOther     { 0 };
	uint32_t irqLogBudget { 0 };
	// 0.0.385 — class A's walk is 43 src-0 entries on three boots and 102 on a fourth, and the
	// shared 32-line budget above could not print even one whole walk. src-0 entries now draw on their OWN
	// budget and print dw1-3 and src_data[2..3] (the element count and the timing) instead of the two-dword
	// line. Read-only, unconditional, and BOUNDED: at most kIrqSrc0LogLines lines per boot however many faults
	// arrive. On a boot with no VM fault it prints nothing and the log is byte-identical to 0.0.384's.
	// 128 = the widest walk we have measured (arm6's 102) plus room for a second burst and the class-B
	// NACK entries that precede it (arm10's ordering: 8 PA writes, then the read-NACKs). WORST CASE IS
	// 128 LINES FOR THE WHOLE BOOT.
	static constexpr uint32_t kIrqSrc0LogLines = 128u;
	uint32_t irqSrc0LogBudget { 0 };
	uint64_t submits         { 0 };
	uint64_t fencesCompleted { 0 };
	uint64_t fenceTimeouts   { 0 };

	IOPCIDevice       *pciDevice  { nullptr };
	IOMemoryMap       *rmmioMap   { nullptr };
	volatile uint32_t *rmmio      { nullptr };
	size_t             rmmioSize  { 0 };
	IOMemoryMap       *bar2Map    { nullptr };
	IOMemoryMap       *bar0Map    { nullptr };
	volatile uint8_t  *bar0       { nullptr };
	size_t             bar0Size   { 0 };
	uint64_t           bar0Phys   { 0 };
	bool               rebarFound { false };  // 0.0.664: the census result for BAR0 (vramstat reports it)
	uint32_t           rebarMask  { 0 };      // BAR0's supported-sizes mask (bit k = 1 MiB << k)
	uint64_t           rebarCurMB { 0 };      // BAR0's current size in MiB
	uint64_t           bar0Limit  { 0 };      // 0.0.663: vramLimit planned by rdna4rebar::plan_bar0 (== bar0Size at a 256 MiB BAR0); 0 until finishVramPlan succeeds
	bool               bar0FullMode { false };// true only when garnet-full-bar=1 succeeded with a > 1 GiB full BAR0 mapping
	rdna4rebar::PlanIn   bar0Plan   {};         // 0.0.663: the plan's inputs, kept so finishVramPlan can add the console
	uint64_t           bootFbPhys { 0 };      // console framebuffer, physical
	uint64_t           bootFbLen  { 0 };
	uint64_t           bootFbRow  { 0 };      // 0.0.613: its rowBytes (getConsoleInfo v_rowBytes)
	uint32_t           bootFbDepth { 0 };     // getConsoleInfo v_depth
	uint64_t           bootFbOff  { ~0ULL };  // its VRAM offset, or ~0 if not in BAR0
	uint64_t           vramBase   { 0 };      // our bring-up region (VRAM byte offset)
	rdna4peer::Grant   displayGrant {};       // only a locally-approved extra display surface appears here
	IOLock            *displayPeerLock { nullptr };
	rdna4peer::Snapshot displayPeerSnapshot {};
	rdna4peer::GateState displayPeerGate {};
	rdna4peer::PrimaryView displayPrimary {};
	IOService         *displayPrimaryService { nullptr };
	uint32_t           displayPeerRefusals { 0 };
	uint8_t           *onDieDisc  { nullptr };
	uint32_t           vramMB     { 0 };
	IpDiscovery        ipDiscovery;
	amdgpu::DeviceContext dev;          // compat layer for src/amd/* modules
	uint32_t           targetStage { 0 };
};

#endif /* GarnetDriver_hpp */
