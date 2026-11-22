#ifndef RDNA4_DCN_INTERNAL_HPP
#define RDNA4_DCN_INTERNAL_HPP

#include "dcn.hpp"
#include "GarnetDriver.hpp"
#include "ipdiscovery.hpp"
#include "amd/amdgpu_regs.h"
#include <IOKit/IOLib.h>
#include <kern/clock.h>
#include <kern/thread.h>
#include <libkern/OSAtomic.h>
#include <pexpert/pexpert.h>
#include "amd/smu_dal.h"
#include "amd/native_s1b.h"
#include "amd/native_s1c.h"
#include "GarnetNativeABI.h"
#include "scanout_pure.h"
#include "amd/native_disp_pure.h"
#include "amd/native_disp.h"
#include "modetrial_pure.h"
#include "GarnetDisplayOps.h"

extern "C" {
#include "dcn41.h"
#include "dcn41_allow.h"
#include "dcn41_otg_timing.h"
#include "dcn41_modes.h"
}
#include "liveraster.h"
#include "dp_link.h"
#include "dp_aux_transport.h"
#include "dispread_flow.h"
#include "dmubcmd_flow.h"
#include "scanx_flow.h"
#include "disp2_flow.h"
extern "C" {
#include "dcn41_dmub.h"
}

bool rdna4_vram_read_mm(uint64_t vramOffset, uint32_t *dst, uint32_t dwords);
bool rdna4_vram_write_mm(uint64_t vramOffset, const uint32_t *src, uint32_t dwords);

namespace rdna4dcn {

inline constexpr uint32_t kDceLogBudget = 12;
inline constexpr uint64_t kDceStormCap = 200000;
inline constexpr int32_t kVTotalDeltaLines = 20;

struct DcnState {
	GarnetDriver        *owner      { nullptr };
	amdgpu::DeviceContext *dev      { nullptr };
	bool                 armed      { false };
	uint32_t             initStatus { 0xFFFFFFFFu };
	uint32_t             seg[DCN41_NUM_SEGS] { 0, 0, 0, 0, 0 };
	struct dcn41_dev     d          {};
	struct dcn41_allow_state allow  {};
	const char          *tag        { "?" };

	// what we enabled, so `dcnvbl 0` and the storm guard can undo exactly that
	bool     srcEnabled { false };
	uint8_t  srcKind    { 0 };            // enum dcn41_irq_kind
	uint8_t  srcInst    { 0 };
	uint64_t enabledAtNs { 0 };
	uint64_t entriesAtEnable { 0 };
	bool     stormTripped { false };

	// IH counters
	uint64_t dceEntries { 0 };
	uint64_t dceByKind[DCN41_IRQ_KIND_COUNT] { };
	uint64_t dceVupdate[DCN41_NUM_PIPES] { };
	uint64_t dceVstartup[DCN41_NUM_PIPES] { };
	uint64_t dceUnclassified { 0 };
	uint64_t ackFailures { 0 };
	uint64_t irqRefusals { 0 };
	uint32_t irqRefusalLogs { 0 };
	uint64_t firstEntryNs { 0 }, lastEntryNs { 0 };
	uint32_t logBudget { 0 };

	// ---- T3-FLIP state ----
	bool     flipPrepared { false };
	uint64_t origMc       { 0 };      // the console plane address, saved before the first flip
	uint64_t testMc       { 0 };      // our test buffer, MC space
	uint64_t testVramOff  { 0 };      // the same buffer as a VRAM byte offset (for the BAR0 fill)
	uint64_t testBytes    { 0 };
	uint32_t fbWidth      { 0 };      // ALL FOUR read from the HUBP's own registers, never assumed
	uint32_t fbHeight     { 0 };
	uint32_t fbPitchPx    { 0 };
	uint32_t fbPixFmt     { 0 };
	uint32_t fbSwMode     { 0xFFFFFFFFu };
	uint32_t flipHubp     { 0 };
	uint32_t flipOtg      { 0 };
	bool     flipHeld     { false };  // our buffer is on screen RIGHT NOW
	uint32_t heldFrames   { 0 };
	uint32_t heldFrameCap { 0 };
	uint64_t flipsIn      { 0 }, flipsOut { 0 }, watchdogRestores { 0 };

	// ---- the same-mode re-timing ----
	bool     goldenValid  { false };    // the timing as it was BEFORE we ever wrote it, this boot
	struct dcn41_otg_timing golden {};
	uint64_t retimings    { 0 }, retimingStalls { 0 }, goldenRestores { 0 };
};

extern DcnState gDcn;

// ---- build 0.0.603 (native S2a): the native scanout state. Everything below is inert unless a native RDNA4N client Acquires. ----
// LOCK ORDER (kext-wide, pinned by tests/native_s2a_test.cpp): the native client's lock (amd/native_s1c.cpp) -> gScan.lock, never the other way. The
// interrupt handler takes ONLY gScan.lock; nothing sleeps while holding it (the restore drops it around every wait), so ihEntry can
// never wait on a sleeper. One field is read WITHOUT the lock: `active` (ihEntry's cheap test).
struct ScanState {
	IOLock  *lock        { nullptr };
	volatile uint32_t active { 0 };     // 1 from Acquire until the restore completes
	bool     acquired    { false };
	bool     restoring   { false };
	bool     haveConsole { false };     // the console MC was learned at a first Acquire this boot (kept across releases for `dcnflip 0`)
	bool     geomWarned  { false };
	bool     wantRestore { false };     // set by the IRQ handler (which may not sleep): the watchdog thread does the restore
	const char *wantWhy  { nullptr };
	uint64_t consoleMc   { 0 };
	uint64_t consoleBytes { 0 };
	uint32_t width { 0 }, height { 0 }, pitchPx { 0 };
	uint32_t gen         { 0 };         // bumped at every Acquire; a pin recorded under an older one is stale
	rdna4scan::Table tbl   {};
	rdna4scan::FcExt fc    {};
	rdna4scan::Storm storm {};
	uint64_t presentSeq { 0 }, vupdates { 0 }, latchIrq { 0 }, latchPoll { 0 }, refused { 0 }, geomRefused { 0 };
	uint64_t wdRestores { 0 }, stormTrips { 0 }, acquires { 0 }, restores { 0 }, restoreFailures { 0 };
	bool     restoreFailed { false };   // the last restore was NOT verified: the plane may still be scanning a client buffer, so pinned BOs must be LEAKED, never freed
	volatile uint32_t watchdogAlive { 0 };   // watchdog threads not yet finished (GarnetDriver::stop waits for 0 before it unmaps the registers)
	uint64_t firstLatchNs { 0 }, lastLatchNs { 0 }, lastActivityNs { 0 };
};

extern ScanState gScan;
extern rdna4mt::State gMt;
extern IOLock *gMtLock;
extern volatile uint32_t gMtHoldAlive;
extern volatile uint32_t gMtWdAlive;
extern rdna4lr_ro gLr;

/* dcn_4_1_0_offset.h, instance 0, BASE_IDX 2 (absolutes for OTG0/HUBP0 in the comments). */
enum : uint32_t {
    kOffCrcCntl      = 0x1b65,   /* 0x5025 */
    kOffCrcWinAX     = 0x1b66,   /* 0x5026 */
    kOffCrcWinAY     = 0x1b67,   /* 0x5027 */
    kOffCrcWinBX     = 0x1b68,   /* 0x5028 */
    kOffCrcWinBY     = 0x1b69,   /* 0x5029 */
    kOffCrcDataRG    = 0x1b6a,   /* 0x502a */
    kOffCrcDataB     = 0x1b6b,   /* 0x502b */
    kOffSurfConfig   = 0x05e5,   /* HUBP0_DCSURF_SURFACE_CONFIG        0x3aa5 */
    kOffTilingConfig = 0x05e7,   /* HUBP0_DCSURF_TILING_CONFIG         0x3aa7 */
    kOffViewportDim  = 0x05eb,   /* HUBP0_DCSURF_PRI_VIEWPORT_DIMENSION 0x3aab */
    kOffSurfPitch    = 0x0607,   /* HUBPREQ0_DCSURF_SURFACE_PITCH      0x3ac7 */
    kOffVmidSettings = 0x0609,   /* HUBPREQ0_VMID_SETTINGS_0 (VMID bits 3:0) - build 0.0.603 */
    kOffViewportStart = 0x05e9,  /* HUBP0_DCSURF_PRI_VIEWPORT_START - build 0.0.603 */
    kOffFlipControl  = 0x0613,   /* HUBPREQ0_DCSURF_FLIP_CONTROL       (SURFACE_FLIP_PENDING bit 8) - build 0.0.603 */
    kOffSurfControl  = 0x0612,   /* HUBPREQ0_DCSURF_SURFACE_CONTROL    (PRIMARY_SURFACE_DCC_EN bit 1) - build 0.0.603 */
};

struct ScanGeom { uint32_t w, h, pitchPx, fmt, sw; bool dcc; };

uint64_t now_ns();
bool dm_latched_on();
bool d2_latched_on();
uint32_t dcn_rreg(void *cookie, uint32_t abs_dword);
bool dcn_display_txn_ok(const char *what);
void dcn_wreg(void *cookie, uint32_t abs_dword, uint32_t value);
void dcn_udelay(void *cookie, uint32_t us);
int lit_otg();
uint32_t otg_reg(uint32_t off0, uint32_t otg);
uint32_t hubp_reg(uint32_t off0, uint32_t hubp);

IOLock *scan_lock();
void scan_read_geom(ScanGeom *g);
bool scanIrq(const struct dcn41_irq &irq, uint64_t nowNs, int *ar);
void mt_golden_at_bind();
bool mt_emergency_restore(const char *why);

void wait_for_dcn_threads_quiescent();
uint32_t dcn_ddc_edid_read(uint32_t ddcLine, uint8_t out[256]);
void dcn_log_dp_preflight(uint32_t otg, const rdna4dp::Timing &target, const char *why);

}  // namespace rdna4dcn

#endif  // RDNA4_DCN_INTERNAL_HPP
