//
//  GarnetDriver.cpp — Garnet core driver service: read-only survey by default, PSP
//  bootloader chain when garnet-psp=1.
//
#include "GarnetDriver.hpp"
#include "GarnetDriverInternal.hpp"
#include "dcn/scanout_sdma.hpp"
#include "dcn/agdc_service.hpp"
#include "amd/amdgpu_sysmem.h"
#include "GarnetPrimaryFramebuffer.hpp"
#include "regs.hpp"
#include "amd/amdgpu_discovery.h"
#include "amd/amdgpu_init.h"
#include "amd/amdgpu_regs.h"
#include "amd/amdgpu_sdma.h"
#include "GarnetMetalNub.hpp"   // 0.0.610: terminate a published Metal nub at stop
#include "GarnetDisplayNub.hpp"       // 0.0.652 (M5): fbpublish 2
#include "webkit_mmap_hook.hpp"       // Version-independent _mac_file_check_mmap hook for Safari WebKit.GPU
#include <libkern/c++/OSData.h>
#include <libkern/c++/OSSymbol.h>
#include "amd/native_s1b.h"   // 0.0.600 (native S1b): garnet-native=1, default OFF
#include "amd/native_s1c.h"   // 0.0.612: n1c_latch_pci_bars
#include "GarnetNativeClient.hpp"   // 0.0.601 (native S1c): the GARNETN user client
#include "GarnetUserClient.hpp"
#include "dcn/dcn.hpp"
#include "dcn/scanout_pure.h"   // 0.0.603 (native S2a): rdna4scan::accel_exempt
#include "amd/native_display_identity.h"
#include "amd/native_disp.h"            // 0.0.613 (#11 11h.2): the display pipe glue (boot-arg garnet-metal-disp=1)
#include "amd/native_disp_pure.h"
#include "amd/native_agdc_pure.h"      // 0.0.614 (#11 11h.3): the native AGDC service (verb 88)
#include "amd/garnet_hang_recovery_pure.h"        // 0.0.623 (GPU-apps G1): hangtest / hangrecover / hangstat (verbs 100..102, boot-arg garnet-g1=1)
#include "amd/garnet_app_credit_pure.h"        // 0.0.650 (GPU-apps G5 Stage 1): sessstat page 9 (boot-arg garnet-g5=1)
#include "amd/garnet_multisession_pure.h"        // 0.0.627 (GPU-apps G2): sessstat (verb 103, boot-arg garnet-multisession=1)
#include "amd/garnet_app_policy_pure.h"        // 0.0.640 (GPU-apps G4): appallow (verb 104, boot-args garnet-apps=1 + garnet-multisession=1)
#include "dcn/fbname.hpp"
#include <IOKit/IOLib.h>
#include <IOKit/IOMessage.h>
#include <IOKit/pwr_mgt/RootDomain.h>
#include <kern/thread.h>
#include <IOKit/IOMemoryDescriptor.h>
#include <kern/clock.h>

#include "amd/amdgpu_asic.h"   // RDNA4: per-chip table (Navi 48 / Navi 44)
#include "fw/fw_table.h"

static IOService *gBringupSelf { nullptr };     // our GarnetDriver service
static IOLock    *gBringupLifecycleLock { nullptr };

OSDefineMetaClassAndStructors(GarnetDriver, IOService)

namespace {
// Everything the kext puts in VRAM fits in this span above vramBase:
//   +0x000000 fw_pri (1 MiB)   +0x100000 PSP ring/cmd/fence   +0x200000 TMR (4 MiB)
//   +0x1000000 fw_buf (8 MiB)  — mirrors mac-amdgpu's layout, relocated.
constexpr uint64_t kBringupSpan   = 32ULL << 20;
constexpr uint64_t kOneMiB        = 1ULL << 20;
constexpr uint64_t kFallbackBase  = 64ULL << 20;   // if the console FB can't be located
constexpr uint64_t kMaxBar0Map    = rdna4rebar::kMaxBar0Map;   // 0.0.663 (ReBAR item 1): 1 GiB, what Tahoe's IOPCIFamily can size; never map more aperture than this (was 256 MiB)

// Drain write-combining buffers so BAR0 writes are visible to the GPU before
// we ring its doorbell registers.
inline void storeFence() { __asm__ __volatile__("sfence" ::: "memory"); }

} // namespace

// ---------------------------------------------------------------------------
// BAR5 register harness
// ---------------------------------------------------------------------------

// 0.0.612 (review item A): read every PCI BAR of this GPU ONCE, at start, from the IOPCIDevice: the six BAR registers and the expansion ROM, each through getDeviceMemoryWithRegister (the
// FULL assigned range: BAR0 is 16 GiB on a ReBAR boot, not the 256 MiB window bar0Size maps; a 64-bit BAR is one range, its upper-half register yields none). Hands the list to
// native_s1c.cpp, which keeps it (first writer wins) and refuses any BoImportHost page inside one. Read-only: config-space reads and registry data, no register write.
// 0.0.663 (ReBAR item 5): the BAR0 range is the UNION of IOPCIFamily's and our own config-space one (base from the BAR0/BAR1 dwords, size from the ReBAR capability, read-only). A BAR0 that IOPCIFamily dropped
// (it publishes length 0 for a BAR it cannot size) is therefore still guarded, and a disagreement refuses the native client open (n1c_latch_bar0_conflict).
static rdna4rebar::Census readRebarCensus(IOPCIDevice *pci) {
	return rdna4rebar::census_read([pci](uint32_t o) -> uint32_t { return pci->configRead32(o); });
}
static uint64_t readCfgBar0(IOPCIDevice *pci) {
	const uint32_t lo = pci->configRead32(kIOPCIConfigBaseAddress0);
	const uint32_t hi = ((lo & 0x6) == 0x4) ? pci->configRead32(kIOPCIConfigBaseAddress1) : 0;
	return rdna4rebar::cfg_bar_addr(lo, hi);
}
static void latchPciBars(IOPCIDevice *pci) {
	static const UInt8 kRegs[] = { kIOPCIConfigBaseAddress0, kIOPCIConfigBaseAddress1, kIOPCIConfigBaseAddress2, kIOPCIConfigBaseAddress3,
	                                  kIOPCIConfigBaseAddress4, kIOPCIConfigBaseAddress5, kIOPCIConfigExpansionROMBase };
	uint64_t base[7] = {}, size[7] = {};
	uint32_t n = 0;
	uint64_t io0Base = 0, io0Len = 0;
	for (uint32_t i = 0; i < 7u; i++) {
		IODeviceMemory *m = pci->getDeviceMemoryWithRegister(kRegs[i]);
		if (!m) continue;
		const uint64_t len = m->getLength();
		if (len == 0) continue;
		base[n] = (uint64_t)m->getPhysicalAddress(); size[n] = len; n++;
		if (i == 0) { io0Base = base[n - 1]; io0Len = len; }
		RDNA4LOG("pci bars: register %#x -> [%#llx, +%#llx)", kRegs[i], (unsigned long long)base[n - 1], (unsigned long long)len);
	}
	// The config-space BAR0 and the ReBAR capability's current size, then the union.
	const rdna4rebar::Census cen = readRebarCensus(pci);
	const int e0 = rdna4rebar::census_entry(cen, 0u);
	const uint64_t cfgLen = (cen.found && e0 >= 0) ? rdna4rebar::entry_bytes(cen.e[e0].ctrlReg) : 0ull;
	const uint64_t cfgBase = readCfgBar0(pci);
	const rdna4rebar::LatchOut u = rdna4rebar::bar0_latch(io0Base, io0Len, cfgBase, cfgLen);
	if (u.have) {
		if (io0Len != 0 && n > 0 && base[0] == io0Base) { base[0] = u.base; size[0] = u.len; }              // BAR0 is entry 0 when IOPCIFamily published it
		else if (n < 7u) { for (uint32_t k = n; k > 0; k--) { base[k] = base[k - 1]; size[k] = size[k - 1]; } base[0] = u.base; size[0] = u.len; n++; }   // a dropped BAR0 is added
		RDNA4LOG("pci bars: BAR0 union [%#llx, +%#llx) (IOPCIFamily [%#llx, +%#llx), config space %#llx, ReBAR size %#llx)%s", (unsigned long long)u.base, (unsigned long long)u.len,
		       (unsigned long long)io0Base, (unsigned long long)io0Len, (unsigned long long)cfgBase, (unsigned long long)cfgLen, u.conflict ? " - DISAGREE" : "");
	}
	amdgpu::n1c_latch_bar0_conflict(u.conflict);
	amdgpu::n1c_latch_pci_bars(base, size, n);
}

bool GarnetDriver::mapRegisters() {
	IODeviceMemory *bar = pciDevice->getDeviceMemoryWithRegister(kIOPCIConfigBaseAddress5);
	if (!bar) { RDNA4LOG("mmio: BAR5 not present/assigned"); return false; }
	rmmioMap = bar->map();
	if (!rmmioMap) { RDNA4LOG("mmio: failed to map BAR5"); return false; }
	rmmio     = reinterpret_cast<volatile uint32_t *>(rmmioMap->getVirtualAddress());
	rmmioSize = rmmioMap->getLength();
	RDNA4LOG("mmio: BAR5 mapped, %zu KiB", rmmioSize / 1024);
	return rmmio != nullptr;
}

void GarnetDriver::unmapRegisters() {
	// The shared indexed-window lock brackets the null-out. A waiter that
	// already passed its own check cannot still be dereferencing the old map:
	// it holds this lock, and this unmap is waiting for it. The RVRAM/WVRAM
	// helpers and this class's vramRead32/vramWrite32 re-check rmmio after
	// acquiring the lock.
	IOLock *const lock = dev.mmIndexLock;
	if (lock) IOLockLock(lock);
	rmmio = nullptr; rmmioSize = 0;
	dev.rmmio = nullptr; dev.rmmioSize = 0;
	IOMemoryMap *map = rmmioMap;
	rmmioMap = nullptr;
	if (lock) IOLockUnlock(lock);
	if (map) map->release();
}

uint32_t GarnetDriver::regRead32(uint32_t byteOffset) const {
	if (!rmmio || (byteOffset & 3) || static_cast<uint64_t>(byteOffset) + 4 > rmmioSize) return 0xFFFFFFFF;
	return rmmio[byteOffset / 4];
}

bool GarnetDriver::regWrite32(uint32_t byteOffset, uint32_t value) {
	if (!rmmio || (byteOffset & 3) || static_cast<uint64_t>(byteOffset) + 4 > rmmioSize) return false;
	rmmio[byteOffset / 4] = value;
	return true;
}

uint32_t GarnetDriver::regReadIp(uint16_t hwId, uint8_t inst, uint8_t seg, uint32_t dword) {
	uint32_t off;
	if (!ipDiscovery.isValid() || !ipDiscovery.regByteOffset(hwId, inst, seg, dword, off)) return 0xFFFFFFFF;
	return regRead32(off);
}

bool GarnetDriver::regWriteIp(uint16_t hwId, uint8_t inst, uint8_t seg, uint32_t dword, uint32_t value) {
	uint32_t off;
	if (!ipDiscovery.isValid() || !ipDiscovery.regByteOffset(hwId, inst, seg, dword, off)) return false;
	return regWrite32(off, value);
}

uint32_t GarnetDriver::vramRead32(uint64_t pos) const {
	IOLock *const lock = dev.mmIndexLock;
	if (!lock) return 0xFFFFFFFF;   // never touch the window without the shared lock
	IOLockLock(lock);
	if (!rmmio || rmmioSize < 0x1c) {
		IOLockUnlock(lock);
		return 0xFFFFFFFF;
	}
	rmmio[rdna4::kMMIndexHiDword] = static_cast<uint32_t>(pos >> 31);
	rmmio[rdna4::kMMIndexDword]   = (static_cast<uint32_t>(pos) & 0x7ffffffc) | 0x80000000u;
	const uint32_t value = rmmio[rdna4::kMMDataDword];
	IOLockUnlock(lock);
	return value;
}

// Same complete indexed operation as amdgpu::WVRAM32_via_mm, used only when the
// display has delegated an indexed write through the private peer endpoint.
void GarnetDriver::vramWrite32(uint64_t pos, uint32_t value) {
	IOLock *const lock = dev.mmIndexLock;
	if (!lock) return;   // never touch the window without the shared lock
	IOLockLock(lock);
	if (!rmmio || rmmioSize < 0x1c) {
		IOLockUnlock(lock);
		return;
	}
	rmmio[rdna4::kMMIndexHiDword] = static_cast<uint32_t>(pos >> 31);
	rmmio[rdna4::kMMIndexDword]   = (static_cast<uint32_t>(pos) & 0x7ffffffc) | 0x80000000u;
	rmmio[rdna4::kMMDataDword]    = value;
	IOLockUnlock(lock);
}

// ---------------------------------------------------------------------------
// BAR0 VRAM aperture
// ---------------------------------------------------------------------------

// 0.0.663 (ReBAR items 2, 3, 6): plan BEFORE mapping. The inputs: IOPCIFamily's BAR0 range, config space's BAR0 address, the ReBAR capability's current BAR0 size (READ-ONLY; the capability is never written),
// the VRAM size and the hi-pool reserve. A refusal maps nothing: the ladder stops at "no-bar0" and the machine keeps its CPU desktop (RDNA4FB), exactly as with no BAR0. The census is published as registry
// properties and logged either way. The console is added by finishVramPlan once it has been located.
bool GarnetDriver::mapVramAperture() {
	IODeviceMemory *bar = pciDevice->getDeviceMemoryWithRegister(kIOPCIConfigBaseAddress0);
	if (!bar) { RDNA4LOG("vram: BAR0 not present/assigned"); return false; }
	const uint64_t fullLen = bar->getLength();
	const rdna4rebar::Census cen = readRebarCensus(pciDevice);
	{
		const int e0 = rdna4rebar::census_entry(cen, 0u), e2 = rdna4rebar::census_entry(cen, 2u);
		RDNA4LOG("rebar census: extended config %s; Resizable BAR capability %s at %#x, %u entr%s", cen.extReadable ? "readable" : "UNREADABLE (all ones)", cen.found ? "found" : "NOT found", cen.capOff, cen.n, cen.n == 1 ? "y" : "ies");
		setProperty("ReBAR,Found", cen.found);
		setProperty("ReBAR,ExtConfigReadable", cen.extReadable);
		if (cen.found) setProperty("ReBAR,CapOffset", (uint64_t)cen.capOff, 32);
		for (uint32_t k = 0; k < 2u; k++) {
			const int e = k == 0 ? e0 : e2;
			const char *nm = k == 0 ? "BAR0" : "BAR2";
			if (e < 0) { RDNA4LOG("rebar census: %s has no resizable entry", nm); continue; }
			const uint32_t cap = cen.e[e].capReg, ctl = cen.e[e].ctrlReg;
			RDNA4LOG("rebar census: %s capability dword %#010x (supported sizes mask %#x, bit k = 1 MiB << k), control dword %#010x -> current size code %u = %llu MiB", nm, cap, rdna4rebar::entry_supported_mask(cap), ctl,
			       rdna4rebar::entry_code(ctl), (unsigned long long)(rdna4rebar::entry_bytes(ctl) >> 20));
			char key[40];
			snprintf(key, sizeof(key), "ReBAR,%s,SupportedMask", nm); setProperty(key, (uint64_t)rdna4rebar::entry_supported_mask(cap), 32);
			snprintf(key, sizeof(key), "ReBAR,%s,CurrentMB", nm);     setProperty(key, (uint64_t)(rdna4rebar::entry_bytes(ctl) >> 20), 64);
		}
	}
	const uint64_t cfgAddr = readCfgBar0(pciDevice);
	const int e0 = rdna4rebar::census_entry(cen, 0u);
	rebarFound = cen.found && e0 >= 0;
	rebarMask = rebarFound ? rdna4rebar::entry_supported_mask(cen.e[e0].capReg) : 0u;
	rebarCurMB = rebarFound ? (rdna4rebar::entry_bytes(cen.e[e0].ctrlReg) >> 20) : 0ull;
	if (rebarFound) RDNA4LOG("rebar census: BAR0 offers sizes mask %#x, smallest %llu MiB, current %llu MiB (ResizeAppleGpuBars code for 'smallest' = the lowest set bit)", rebarMask, (unsigned long long)(rdna4rebar::entry_min_bytes(rebarMask) >> 20), (unsigned long long)rebarCurMB);
	uint32_t fullBarArg = 0;
	const bool fullBarPresent = PE_parse_boot_argn("garnet-full-bar", &fullBarArg, sizeof(fullBarArg));
	const bool fullBarRequested = rdna4rebar::full_bar_enabled(fullBarPresent, fullBarArg);
	bar0FullMode = false;
	static const UInt8 kOtherBars[] = { kIOPCIConfigBaseAddress2, kIOPCIConfigBaseAddress4, kIOPCIConfigBaseAddress5, kIOPCIConfigExpansionROMBase };
	uint64_t otherBase[4] = {}, otherLen[4] = {};
	uint32_t nOther = 0;
	for (uint32_t i = 0; i < 4u; i++) {
		IODeviceMemory *om = pciDevice->getDeviceMemoryWithRegister(kOtherBars[i]);
		if (!om) continue;
		const uint64_t ol = om->getLength();
		if (ol == 0) continue;
		otherBase[nOther] = static_cast<uint64_t>(om->getPhysicalAddress());
		otherLen[nOther]  = ol;
		nOther++;
	}
	rdna4rebar::PlanIn pin {};
	pin.ioLen = fullLen; pin.ioAddr = (uint64_t)bar->getPhysicalAddress(); pin.cfgAddr = cfgAddr;
	pin.rebarPresent = cen.found && e0 >= 0; pin.rebarBytes = pin.rebarPresent ? rdna4rebar::entry_bytes(cen.e[e0].ctrlReg) : 0ull;
	pin.vramBytes = static_cast<uint64_t>(vramMB) << 20; pin.hiReserve = amdgpu::kVramHiTotalReserve;
	pin.consoleOff = rdna4rebar::kNoConsole; pin.consoleLen = 0; pin.mapCap = kMaxBar0Map;
	pin.barOverlap = rdna4rebar::bar_overlaps_any(pin.ioAddr, pin.ioLen, otherBase, otherLen, nOther);
	if (fullBarRequested) {
		pin.mapCap = rdna4rebar::kMaxBar0Full;
		pin.requireFullBar = true;
	}
	rdna4rebar::PlanOut pl = rdna4rebar::plan_bar0(pin);
	bool fullBarFallback = false;
	uint32_t fullRefusalReason = rdna4rebar::kOk;
	if (fullBarRequested && !pl.ok && pl.reason == rdna4rebar::kFullBarNotAssigned) {
		pl = rdna4rebar::plan_bar0_with_fallback(pin, &fullBarFallback, &fullRefusalReason);
		if (fullBarFallback) {
			RDNA4LOG("vram: garnet-full-bar=1 requested but BAR0 (%llu MiB) does not cover VRAM (%llu MiB); falling back to Small-BAR (%llu MiB)",
			         (unsigned long long)(pin.ioLen >> 20), (unsigned long long)(pin.vramBytes >> 20),
			         (unsigned long long)(pl.mapLen >> 20));
			setProperty("BAR0,FullRefusalReason", rdna4rebar::reason_name(fullRefusalReason));
		}
	}
	RDNA4LOG("vram: BAR0 plan (garnet-full-bar=%u, cap %llu MiB): IOPCIFamily [%#llx, +%llu MiB), config space %#llx, ReBAR %s %llu MiB, VRAM %llu MiB -> %s%s (map %llu MiB, vramLimit %llu MiB)",
	       fullBarRequested ? 1u : 0u, (unsigned long long)(pin.mapCap >> 20), (unsigned long long)pin.ioAddr, (unsigned long long)(pin.ioLen >> 20),
	       (unsigned long long)pin.cfgAddr, pin.rebarPresent ? "current" : "absent", (unsigned long long)(pin.rebarBytes >> 20), (unsigned long long)(pin.vramBytes >> 20),
	       pl.ok ? "OK" : "REFUSED: ", pl.ok ? "" : rdna4rebar::reason_name(pl.reason), (unsigned long long)(pl.mapLen >> 20), (unsigned long long)(pl.vramLimit >> 20));
	setProperty("BAR0,PlanReason", rdna4rebar::reason_name(pl.reason));
	setProperty("BAR0,FullRequested", fullBarRequested);
	setProperty("BAR0,FullFallback", fullBarFallback);
	setProperty("BAR0,FullMode", false);
	setProperty("BAR0,MapCapMB", static_cast<uint64_t>(pin.mapCap >> 20), 64);
	if (!pl.ok) { RDNA4LOG("vram: BAR0 refused (%s): nothing is mapped, the native ladder will not run", rdna4rebar::reason_name(pl.reason)); return false; }
	bar0Plan = pin;
	// Map the planned length: up to 1 GiB by default, or up to 16 GiB with explicit garnet-full-bar=1 opt-in.
	const uint64_t winLen  = pl.mapLen;
	IODeviceMemory *win = IODeviceMemory::withSubRange(bar, 0, winLen);
	if (!win) { RDNA4LOG("vram: BAR0 sub-range failed"); return false; }
	bar0Map = win->map(kIOMapWriteCombineCache);
	if (!bar0Map) bar0Map = win->map();
	win->release();                              // the map holds its own reference
	if (!bar0Map) { RDNA4LOG("vram: failed to map BAR0"); return false; }
	bar0     = reinterpret_cast<volatile uint8_t *>(bar0Map->getVirtualAddress());
	bar0Size = bar0Map->getLength();             // item 3: bar0Size is the bytes ACTUALLY mapped
	if (bar0Size != winLen) {
		RDNA4LOG("vram: BAR0 mapped %zu MiB but %llu MiB was planned; refusing", bar0Size >> 20, (unsigned long long)(winLen >> 20));
		unmapVramAperture();
		return false;
	}
	if (fullLen != bar0Size) RDNA4LOG("vram: BAR0 is %llu MiB; mapped the first %zu MiB", fullLen >> 20, bar0Size >> 20);

	bar0Phys = cfgAddr;                          // planned equal to IOPCIFamily's address (plan_bar0 refuses a difference)
	bar0FullMode = fullBarRequested && bar0Size > kMaxBar0Map;
	RDNA4LOG("vram: BAR0 aperture phys 0x%llx, %zu MiB, fullMode=%u, mapped %s", bar0Phys, bar0Size >> 20,
	       bar0FullMode ? 1u : 0u, (bar0Map->getMapOptions() & kIOMapWriteCombineCache) ? "write-combining" : "uncached");
	setProperty("BAR0,Phys", bar0Phys, 64);
	setProperty("BAR0,SizeMB", static_cast<uint64_t>(bar0Size >> 20), 64);
	setProperty("BAR0,FullMode", bar0FullMode);
	return bar0 != nullptr && bar0Size >= 2 * kOneMiB;
}

// The plan again, now that the console is located: judges the console against vramLimit and fixes bar0Limit (what buildDeviceContext hands the allocator as dev.vramLimit).
bool GarnetDriver::finishVramPlan() {
	rdna4rebar::PlanIn pin = bar0Plan;
	if (bootFbOff != ~0ULL) { pin.consoleOff = bootFbOff; pin.consoleLen = bootFbLen; }
	const rdna4rebar::PlanOut pl = rdna4rebar::plan_bar0(pin);
	if (!pl.ok || pl.mapLen != bar0Size) {
		RDNA4LOG("vram: BAR0 plan with the console: REFUSED (%s; planned map %llu MiB, mapped %zu MiB)", rdna4rebar::reason_name(pl.reason), (unsigned long long)(pl.mapLen >> 20), bar0Size >> 20);
		return false;
	}
	bar0Limit = pl.vramLimit;
	RDNA4LOG("vram: vramLimit %llu MiB (mapped %zu MiB, VRAM %u MiB, hi reserve %llu MiB, fullMode=%u)", (unsigned long long)(bar0Limit >> 20), bar0Size >> 20, vramMB, (unsigned long long)(amdgpu::kVramHiTotalReserve >> 20), bar0FullMode ? 1u : 0u);
	setProperty("BAR0,VramLimitMB", static_cast<uint64_t>(bar0Limit >> 20), 64);
	setProperty("BAR0,ValidVramMB", static_cast<uint64_t>(vramMB), 64);
	return true;
}

IOService *garnet_driver_service_retained(void) {
	if (!gBringupLifecycleLock) return nullptr;
	IOLockLock(gBringupLifecycleLock);
	IOService *service = gBringupSelf;
	if (service) service->retain();
	IOLockUnlock(gBringupLifecycleLock);
	return service;
}

bool GarnetDriver::mapDoorbells() {
	if (bar2Map) return true;
	IODeviceMemory *bar = pciDevice->getDeviceMemoryWithRegister(kIOPCIConfigBaseAddress2);
	if (!bar) { RDNA4LOG("doorbell: BAR2 not present/assigned"); return false; }
	{   // 0.0.664 (F2): IOPCIFamily's BAR2 address must be config space's (the self-ring aperture is pointed at the config-space one): same equality rule as BAR0
		const uint32_t clo = pciDevice->configRead32(kIOPCIConfigBaseAddress2);
		const uint32_t chi = ((clo & 0x6) == 0x4) ? pciDevice->configRead32(kIOPCIConfigBaseAddress3) : 0;
		if (!rdna4rebar::bar_addr_agree((uint64_t)bar->getPhysicalAddress(), rdna4rebar::cfg_bar_addr(clo, chi))) {
			RDNA4LOG("doorbell: BAR2 refused: IOPCIFamily address %#llx != config space %#llx", (unsigned long long)bar->getPhysicalAddress(), (unsigned long long)rdna4rebar::cfg_bar_addr(clo, chi));
			return false;
		}
	}
	bar2Map = bar->map();                     // uncached: doorbells are MMIO
	if (!bar2Map) { RDNA4LOG("doorbell: failed to map BAR2"); return false; }
	dev.bar2     = reinterpret_cast<volatile uint8_t *>(bar2Map->getVirtualAddress());
	dev.bar2Size = bar2Map->getLength();
	{
		uint32_t lo = pciDevice->configRead32(kIOPCIConfigBaseAddress2);
		uint32_t hi = ((lo & 0x6) == 0x4) ? pciDevice->configRead32(kIOPCIConfigBaseAddress3) : 0;
		dev.bar2Phys = (static_cast<uint64_t>(hi) << 32) | (lo & ~0xFULL);
	}
	RDNA4LOG("doorbell: BAR2 mapped, %zu KiB at phys 0x%llx", dev.bar2Size >> 10, dev.bar2Phys);
	setProperty("BAR2,SizeKB", static_cast<uint64_t>(dev.bar2Size >> 10), 32);
	return dev.bar2 != nullptr;
}

// 0.0.664 (F4): after a refusal past the map the machine must be exactly the 'no BAR0' state: the map released, bar0 / bar0Size / bar0Phys / bar0Limit / vramBase all zero (nothing later may use a window the checks refused).
void GarnetDriver::dropBar0() {
	unmapVramAperture();
	bar0Phys = 0; bar0Limit = 0; vramBase = 0;
	bar0FullMode = false;
	setProperty("BAR0,FullMode", false);
}

void GarnetDriver::unmapVramAperture() {
	bar0 = nullptr; bar0Size = 0;
	if (bar0Map) { bar0Map->release(); bar0Map = nullptr; }
}

uint64_t GarnetDriver::bar0AccessLimit() const {
	uint64_t cap = static_cast<uint64_t>(bar0Size);
	if (bar0Limit != 0ull && bar0Limit < cap) cap = bar0Limit;
	const uint64_t validVram = static_cast<uint64_t>(vramMB) << 20;
	if (validVram != 0ull && validVram < cap) cap = validVram;
	return cap;
}

uint32_t GarnetDriver::bar0Read32(uint64_t vramOffset) const {
	const uint64_t cap = bar0AccessLimit();
	if (!bar0 || vramOffset > cap || 4ull > cap - vramOffset) return 0xFFFFFFFF;
	return *reinterpret_cast<const volatile uint32_t *>(bar0 + vramOffset);
}

bool GarnetDriver::vramWrite(uint64_t vramOffset, const void *src, size_t len) {
	const uint64_t cap = bar0AccessLimit();
	if (!bar0 || vramOffset > cap || len > cap - vramOffset) return false;
	const uint8_t *s = static_cast<const uint8_t *>(src);
	volatile uint8_t *d = bar0 + vramOffset;
	size_t i = 0;
	for (; i + 4 <= len; i += 4) {
		uint32_t w; __builtin_memcpy(&w, s + i, 4);
		*reinterpret_cast<volatile uint32_t *>(d + i) = w;
	}
	for (; i < len; i++) d[i] = s[i];
	storeFence();
	return true;
}

bool GarnetDriver::vramMemset(uint64_t vramOffset, uint8_t value, size_t len) {
	const uint64_t cap = bar0AccessLimit();
	if (!bar0 || vramOffset > cap || len > cap - vramOffset) return false;
	volatile uint8_t *d = bar0 + vramOffset;
	const uint32_t w = static_cast<uint32_t>(value) * 0x01010101u;
	size_t i = 0;
	for (; i + 4 <= len; i += 4) *reinterpret_cast<volatile uint32_t *>(d + i) = w;
	for (; i < len; i++) d[i] = value;
	storeFence();
	return true;
}

// The UEFI GOP framebuffer the kernel console (and IONDRVFramebuffer) draw
// into is VRAM behind BAR0 — Linux reserves it as "stolen VGA memory" at VRAM
// offset 0. Find it so nothing we stage lands on it.
// build 0.0.544 item 4b/4c: the console's size as getConsoleInfo reported it at start (0 until then), for the
// consumers that must follow the resolution (the fill set's members, gfx_fillset.h RDNA4_FS_GEO_*). Written once, here; read-only after.
static uint32_t gRDNA4ConsoleW { 0u }, gRDNA4ConsoleH { 0u };
bool rdna4_console_size(uint32_t &w, uint32_t &h) {
	if (!gRDNA4ConsoleW || !gRDNA4ConsoleH) return false;
	w = gRDNA4ConsoleW; h = gRDNA4ConsoleH;
	return true;
}

// 0.0.613: the console region for the display pipe's present. Known only when the boot capture located it inside BAR0 and rowBytes * height is its length.
bool GarnetDriver::consoleGeometry(uint64_t *off, uint64_t *len, uint32_t *w, uint32_t *h, uint32_t *rowBytes) const {
	if (bootFbOff == ~0ULL || bootFbLen == 0 || bootFbRow == 0 || !gRDNA4ConsoleW || !gRDNA4ConsoleH) return false;
	const uint64_t cap = bar0AccessLimit();
	uint64_t verifiedOff = rdna4rebar::kNoConsole;
	if (!rdna4rebar::console_in_bar0(bar0Phys, cap, bootFbPhys, bootFbLen, &verifiedOff) || verifiedOff != bootFbOff) return false;
	if (bootFbRow * gRDNA4ConsoleH != bootFbLen) return false;
	if (off) *off = bootFbOff;
	if (len) *len = bootFbLen;
	if (w) *w = gRDNA4ConsoleW;
	if (h) *h = gRDNA4ConsoleH;
	if (rowBytes) *rowBytes = static_cast<uint32_t>(bootFbRow);
	return true;
}
bool GarnetDriver::consoleWrite(uint64_t consoleOff, const void *src, size_t len) {
	uint64_t off = 0, clen = 0;
	if (!consoleGeometry(&off, &clen, nullptr, nullptr, nullptr)) return false;
	if (len == 0 || consoleOff > clen || len > clen - consoleOff) return false;              // the write stays inside the console, overflow-safe
	rdna4_display_txn txn(rdna4peer::kLeaseLocalDisplay);
	if (!txn.ok) return false;
	return vramWrite(off + consoleOff, src, len);
}


bool GarnetDriver::captureBootFramebuffer() {
	PE_Video video {};
	IOPlatformExpert *platform = getPlatform();
	if (!platform || platform->getConsoleInfo(&video) != kIOReturnSuccess) {
		RDNA4LOG("vram: getConsoleInfo failed — boot framebuffer location unknown");
		return false;
	}
	bootFbPhys = rdna4fbprimary::consolePhysicalAddress(video.v_baseAddr);
	bootFbLen  = static_cast<uint64_t>(video.v_rowBytes) * video.v_height;
	bootFbRow  = static_cast<uint64_t>(video.v_rowBytes);                                           // 0.0.613
	bootFbDepth = static_cast<uint32_t>(video.v_depth);
	gRDNA4ConsoleW = static_cast<uint32_t>(video.v_width); gRDNA4ConsoleH = static_cast<uint32_t>(video.v_height);   // build 0.0.544 4b
	if (bootFbPhys == 0 || bootFbLen == 0) {
		RDNA4LOG("vram: console info empty (base 0x%llx len 0x%llx)", bootFbPhys, bootFbLen);
		return false;
	}
	const uint64_t validBound = bar0AccessLimit();
	uint64_t off = rdna4rebar::kNoConsole;
	const bool inBar0 = rdna4rebar::console_in_bar0(bar0Phys, validBound, bootFbPhys, bootFbLen, &off);
	bootFbOff = inBar0 ? off : ~0ULL;
	RDNA4LOG("vram: console framebuffer phys 0x%llx %lux%lu rowBytes %lu depth %lu (%llu KiB) vs BAR0 [0x%llx, +%llu MiB valid) -> %s",
	       bootFbPhys, video.v_width, video.v_height, video.v_rowBytes, video.v_depth, bootFbLen >> 10,
	       (unsigned long long)bar0Phys, (unsigned long long)(validBound >> 20),
	       inBar0 ? "inside BAR0" : "NOT inside BAR0 (console writes refused, offset not guessed)");
	if (inBar0) {
		RDNA4LOG("vram: console framebuffer occupies vram+[0x%llx, 0x%llx)", bootFbOff, bootFbOff + bootFbLen);
		setProperty("BootFB,VramOffset", bootFbOff, 64);
		setProperty("BootFB,Length", bootFbLen, 64);
	}
	return true;
}

bool GarnetDriver::chooseVramBase() {
	const uint64_t cap = bar0AccessLimit();
	uint64_t base;
	if (bootFbOff != ~0ULL) {
		// First choice: just above the console FB (1 MiB aligned). If that does
		// not fit in the aperture (FB parked at the top), fall back to offset 0
		// provided the whole span ends below the FB.
		if (bootFbOff > ~0ULL - bootFbLen || bootFbOff + bootFbLen > ~0ULL - (kOneMiB - 1)) {
			RDNA4LOG("vram: console FB range wraps; refusing");
			return false;
		}
		base = (bootFbOff + bootFbLen + kOneMiB - 1) & ~(kOneMiB - 1);
		if (base > cap || kBringupSpan > cap - base) {
			if (kBringupSpan <= bootFbOff && kBringupSpan <= cap) {
				RDNA4LOG("vram: no room above the console FB; using vram+0 (FB starts at 0x%llx)", bootFbOff);
				base = 0;
			} else {
				RDNA4LOG("vram: console FB at vram+0x%llx (%llu KiB) leaves no %llu MiB window in a %llu MiB BAR0; refusing",
				       bootFbOff, bootFbLen >> 10, kBringupSpan >> 20, (unsigned long long)(cap >> 20));
				return false;
			}
		}
	} else {
		base = kFallbackBase;                                             // unknown: assume it is low
		RDNA4LOG("vram: console FB not located in BAR0; using fallback base %llu MiB", base >> 20);
		if (base > cap || kBringupSpan > cap - base) {
			RDNA4LOG("vram: fallback base does not fit in a %llu MiB BAR0; refusing", (unsigned long long)(cap >> 20));
			return false;
		}
	}
	// Belt and braces: never overlap the console FB whatever the arithmetic said.
	if (bootFbOff != ~0ULL && base < bootFbOff + bootFbLen && base + kBringupSpan > bootFbOff) {
		RDNA4LOG("vram: chosen base overlaps console FB; refusing");
		return false;
	}
	vramBase = base;
	RDNA4LOG("vram: bring-up region vram+[0x%llx, 0x%llx) (%llu MiB in)", vramBase, vramBase + kBringupSpan, vramBase >> 20);
	setProperty("Garnet,VramBaseOffset", vramBase, 64);
	return true;
}

// Write a pattern through BAR0 and read it back through the MM_INDEX/MM_DATA
// indirect window (the path that already read the on-die discovery table).
// Both address VRAM by byte offset; if they agree, BAR0 offset == VRAM offset
// and our MC arithmetic (FB_LOCATION_BASE + offset) stands. Touches only 16 bytes
// at `off` and restores the original bytes. High 32 bits are mixed into the pattern
// so 32-bit wrap/aliasing above 4 GiB is detected.
bool GarnetDriver::apertureCheck(uint64_t off) {
	const uint32_t offLo = static_cast<uint32_t>(off);
	const uint32_t offHi = static_cast<uint32_t>(off >> 32);
	const uint32_t pat[4] = { 0x4E343821u ^ (offHi * 0x9E3779B9u), 0xA5C3F00Du ^ offHi, offLo, ~offLo ^ (offHi << 16) };
	uint32_t before[4], mm[4], bar[4];
	for (uint64_t i = 0; i < 4; i++) before[i] = vramRead32(off + 4 * i);
	if (!vramWrite(off, pat, sizeof(pat))) return false;
	for (uint64_t i = 0; i < 4; i++) { mm[i] = vramRead32(off + 4 * i); bar[i] = bar0Read32(off + 4 * i); }
	bool ok = true;
	for (int i = 0; i < 4; i++) ok = ok && mm[i] == pat[i] && bar[i] == pat[i];
	RDNA4LOG("vram: aperture check at vram+0x%llx: wrote %08x %08x %08x %08x; MM window read %08x %08x %08x %08x; "
	       "BAR0 read %08x %08x %08x %08x -> %s (was %08x %08x %08x %08x)",
	       off, pat[0], pat[1], pat[2], pat[3], mm[0], mm[1], mm[2], mm[3], bar[0], bar[1], bar[2], bar[3],
	       ok ? "MATCH" : "MISMATCH", before[0], before[1], before[2], before[3]);
	// 0.0.663 (ReBAR item 4): put back the bytes that were there (0.0.662 zeroed them). The restore is read back through the MM window; a byte that does not come back refuses like a mismatch.
	if (!vramWrite(off, before, sizeof(before))) ok = false;
	for (uint64_t i = 0; i < 4; i++) {
		const uint32_t back = vramRead32(off + 4 * i);
		if (back != before[i]) { RDNA4LOG("vram: aperture check at vram+0x%llx: RESTORE FAILED at dword %llu (wanted %08x, read %08x)", off, i, before[i], back); ok = false; }
	}
	return ok;
}

// 0.0.663 (ReBAR item 4): three points across the window (rdna4rebar::aperture_points): proves BAR0 offset == VRAM offset at the bottom, the middle and the top of the planned window, BEFORE the allocator exists
// (stagePSP calls this ahead of buildDeviceContext; the visible pool never starts until all three MATCH). In full-BAR mode (> 1 GiB) also checks 4/8/12 GiB boundary points before any write. Any mismatch, or a degenerate window, refuses.
bool GarnetDriver::apertureCheckWindow() {
	uint64_t pt[3] = {};
	if (bar0Limit == 0 || !rdna4rebar::aperture_points(vramBase, bar0Limit, bar0Size, pt)) {
		RDNA4LOG("vram: aperture check: no valid check points for vramBase %#llx vramLimit %#llx mapped %zu; refusing", (unsigned long long)vramBase, (unsigned long long)bar0Limit, bar0Size);
		setProperty("Garnet,ApertureCheck", "mismatch");
		return false;
	}
	const uint64_t consOff = bootFbOff == ~0ULL ? rdna4rebar::kNoConsole : bootFbOff;
	if (!rdna4rebar::layout_ok(consOff, bootFbLen, vramBase, pt, vramBase + rdna4rebar::kVisAllocOffset, bar0Limit)) {
		RDNA4LOG("vram: aperture check: vramBase %#llx, a check point or the visible pool overlaps the console [%#llx, +%#llx) or its 64 KiB cursor; refusing before any write", (unsigned long long)vramBase, (unsigned long long)bootFbOff, (unsigned long long)bootFbLen);
		setProperty("Garnet,ApertureCheck", "mismatch");
		return false;
	}
	uint64_t extra[rdna4rebar::kMaxExtraAperturePoints] = {};
	const uint32_t nExtra = rdna4rebar::aperture_boundary_points(vramBase, bar0Limit, bar0Size, static_cast<uint64_t>(vramMB) << 20, extra);
	if (nExtra > 0u && !rdna4rebar::layout_points_clear_console(consOff, bootFbLen, extra, nExtra)) {
		RDNA4LOG("vram: aperture check: full-BAR boundary point overlaps console [%#llx, +%#llx) or its 64 KiB cursor; refusing before any write", (unsigned long long)bootFbOff, (unsigned long long)bootFbLen);
		setProperty("Garnet,ApertureCheck", "mismatch");
		return false;
	}
	bool all = true;
	for (int i = 0; i < 3; i++) all = apertureCheck(pt[i]) && all;     // every point is tried and logged even after a mismatch
	for (uint32_t i = 0; i < nExtra; i++) all = apertureCheck(extra[i]) && all;
	RDNA4LOG("vram: aperture checks at vram+[%#llx, %#llx, %#llx] (+%u full-BAR boundary pts) -> %s", (unsigned long long)pt[0], (unsigned long long)pt[1], (unsigned long long)pt[2], nExtra, all ? "ALL MATCH" : "MISMATCH");
	setProperty("Garnet,ApertureCheck", all ? "match" : "mismatch");
	return all;
}


static amdgpu::BringupContext gBringup;   // one ladder per boot; constructed at kext load
amdgpu::BringupContext &garnet_bringup_ctx(void) { return gBringup; }

// 0.0.201 — the MM_INDEX/MM_DATA window is three register accesses with no hardware
// atomicity: a second user between the index write and the data access redirects the
// data to (or from) whatever VRAM that user indexed. Linux holds mmio_idx_lock for
// exactly this. At run time our users are the read verbs (vmib, vmstate), the
// pagecopy observer and the residency copy that writes Apple's resource bytes into
// VRAM; the gfxmap poller and the IH handler never touch the window (RREG32/WREG32
// are BAR5-direct, Apple's register writes are blocked). rdna4_vram_read_mm and
// rdna4_vram_write_mm hold this mutex per <=64-dword batch. Thread context only.
IOLock *gVramMmLock { nullptr };

// Process-lifetime lock borrowed by every DeviceContext (`ctx.mmIndexLock`).
// RVRAM32_via_mm/WVRAM32_via_mm take it around the complete HI/INDEX/DATA
// operation, and the display's delegated kOpVramAccess goes through the same
// local primitives. Never freed: UserClient/compute/SMU callers do not hold a
// display lease and may still be inside it when the display drains or the
// service stops, exactly like gVramMmLock.
static IOLock *gMmIndexWindowLock { nullptr };

// build 0.0.528 (; amd/gmc_tlb83_pure.h) — `gfxneuter 83 | M << 8`. M 1 ON (339), M 2 OFF (595, the default and the
// boot value), bare 83 reads, any other M refused unchanged (st 11), a continuous arm refuses a change (st 5), ON without the leaf lock
// refuses (st 12, stays OFF). The ONLY writer of amdgpu::gTlb83On. It writes no register, no page table and nothing of Apple's.
uint32_t rdna4_tlb83_switch(uint32_t m, uint32_t contRefused, uint32_t *st) {
	const uint32_t want = rdna4_tlb83_mode_of_m(m);
	const char *how = " - read only";
	if (contRefused) { *st = 5u; how = " - REFUSED: a continuous arm stands, unchanged"; }
	else if (want == RDNA4_TLB83_M_BAD) { *st = 11u; how = " - REFUSED (unknown M), unchanged"; }
	else if (want != RDNA4_TLB83_M_READ) {
		if (want == RDNA4_TLB83_ON && !amdgpu::gTlb83Lock) { *st = 12u; how = " - ON REFUSED: no leaf lock, unchanged"; }
		else { __atomic_store_n(&amdgpu::gTlb83On, want, __ATOMIC_RELEASE); how = " - CHANGED BY THIS VERB"; }
	}
	const uint32_t now = __atomic_load_n(&amdgpu::gTlb83On, __ATOMIC_ACQUIRE);
	RDNA4LOG(RDNA4_TLB83_REPORT1_FMT, RDNA4_TLB83_REPORT1_ARGS(&amdgpu::gTlb83S, rdna4_tlb83_mode_name(now), how));
	RDNA4LOG(RDNA4_TLB83_REPORT2_FMT, RDNA4_TLB83_REPORT2_ARGS(&amdgpu::gTlb83S));
	RDNA4LOG(RDNA4_TLB83_REPORT3_FMT, RDNA4_TLB83_REPORT3_ARGS(&amdgpu::gTlb83S, amdgpu::gTlb83Lock ? "present" : "MISSING"));
	return now;
}

// GC IP base (BASE_IDX 0) from IP discovery. 0 if unresolved.
uint32_t rdna4_gc_base(void) {
	if (!gBringup.dev) return 0;
	return gBringup.dev->ip.get(amdgpu::IPBlock::GC);
}

// 0.0.194 — GC register base for an arbitrary segment (gc_12_0_0_offset.h's
// *_BASE_IDX), 0 if that segment is absent. The CP IB registers `vmib` reads
// (CP_IB1_BASE_LO 0x20cc and friends) are BASE_IDX 1, so rdna4_gc_base()
// would land them on a completely different register.
uint32_t rdna4_gc_base_seg(uint8_t seg) {
	if (!gBringup.dev) return 0;
	if (!gBringup.dev->ip.isResolved(amdgpu::IPBlock::GC, (int)seg)) return 0;
	return gBringup.dev->ip.getBase(amdgpu::IPBlock::GC, (int)seg);
}

// Absolute-dword MMIO access for callers that hold no DeviceContext.
// RREG32/WREG32 bounds-check internally against rmmioSize, so no guard here.
uint32_t rdna4_reg_read32(uint32_t reg) {
	if (!gBringup.dev) return 0xFFFFFFFFu;
	return amdgpu::RREG32(*gBringup.dev, reg);
}

void rdna4_reg_write32(uint32_t reg, uint32_t value) {
	if (!gBringup.dev) return;
	amdgpu::WREG32(*gBringup.dev, reg, value);
}

bool rdna4_vram_read_mm(uint64_t vramOffset, uint32_t *dst, uint32_t dwords) {
	if (!gBringup.dev || !dst || dwords == 0 || dwords > 64) return false;
	if ((vramOffset & 3) != 0) return false;
	const uint64_t bytes = (uint64_t)dwords * 4u;
	const uint64_t size = gBringup.dev->vramSizeBytes;
	if (vramOffset > size || bytes > size - vramOffset) return false;
	if (gVramMmLock) IOLockLock(gVramMmLock);
	for (uint32_t i = 0; i < dwords; i++)
		dst[i] = amdgpu::RVRAM32_via_mm(*gBringup.dev, vramOffset + (uint64_t)i * 4);
	if (gVramMmLock) IOLockUnlock(gVramMmLock);
	return true;
}

bool rdna4_vram_write_mm(uint64_t vramOffset, const uint32_t *src, uint32_t dwords) {
	if (!gBringup.dev || !src || dwords == 0 || dwords > 64 || !gVramMmLock) return false;
	if ((vramOffset & 3) != 0) return false;
	const uint64_t bytes = (uint64_t)dwords * 4u;
	const uint64_t size = gBringup.dev->vramSizeBytes;
	if (vramOffset > size || bytes > size - vramOffset) return false;
	IOLockLock(gVramMmLock);
	for (uint32_t i = 0; i < dwords; i++)
		amdgpu::WVRAM32_via_mm(*gBringup.dev, vramOffset + (uint64_t)i * 4, src[i]);
	IOLockUnlock(gVramMmLock);
	return true;
}

// 0.0.201 — may Apple's residency copy write VRAM [off, off + len) (0-based)? Only
// inside our device-only hi pool, and only while that pool has handed out NOTHING:
// the verbatim allocator cannot enumerate live ranges, so bytes_used() == 0 is the
// one exact proof of "no overlap with our allocations" it offers. By construction
// that also keeps the copy off the boot framebuffer [0, 8 MiB), the PSP / GART PT /
// PDB0 / fw_buf reservations below 0x2000000, the BAR0 pool [0x2000000, 0x10000000)
// our rings and self-tests use, and the 768 MiB tail (PSP TMR, IP discovery, Apple's
// page-table arena). Returns 0 = allowed, else 1 no context, 2 no hi pool, 3 outside
// the hi pool, 4 the hi pool has live allocations, 5 empty or misaligned, 6 overflow.
uint32_t rdna4_vram_apple_dest_check(uint64_t off, uint64_t len) {
	if (!gBringup.dev || !gBringup.gmc.inited) return 1;
	const auto &g = gBringup.gmc;
	if (!g.vram_alloc_hi.is_inited() || g.vram_hi_size == 0) return 2;
	if (len == 0 || (off & 3) != 0) return 5;
	if (off + len < off) return 6;
	if (off < g.vram_hi_base || off + len > g.vram_hi_base + g.vram_hi_size) return 3;
	if (off < (8ull << 20) || off + len > gBringup.dev->vramSizeBytes) return 3;
	if (g.vram_alloc_hi.bytes_used() != 0) return 4;
	return 0;
}

// =====================================================================================================
// 0.0.272 — MILESTONE 4 ROUTE c' WALL 3: a GPU-side copy into RDNA4FB's scanout.
//
// The copy runs on OUR SDMA0 QUEUE0, the ring the ladder brought up and proved (stage 15's SDMACopyTest),
// never on queues 1-7 / SDMA1 1-2, which `sdmamap` and the drain hand to Apple's eight rings. Apple's
// queues keep their own IOLock inside writeTail; QUEUE0 has no other runtime user (its self-tests run at
// boot), so gScanoutLock serialises every submission of ours and nothing of Apple's is contended.
//
// Addressing, every term already proven on this card:
//   - destination = gmc.vram_start + scanout VRAM offset, the MC convention of every ladder allocation
//     (gpu_va - vram_start = BAR0 offset, gmc_vram_alloc_init); the scanout offset comes from RDNA4FB's own
//     Console,BaseAddress minus OUR BAR0 physical base - two different sources that must agree;
//   - the geometry, clip, overlap and per-row bounds are the pure scanout_copy.h, host-tested;
//   - every row's destination is re-checked with rdna4_scanout_row_dst_ok right before its packet is built.
// The positive control (mode 1) copies kext-written bars through a pre-flight copy between two scratch
// buffers FIRST (the scanout is not touched unless that lands and reads back), then into a 256x64
// rectangle, and reads every pixel back through BAR0. The surface copy refuses until that has passed.
// =====================================================================================================
#include "dcn/scanout_copy.h"
#include "dcn/gfx_flipmode.h"
#include "dcn/scanout_full.h"
#include "dcn/scanout_verify_pure.h"
#include "amd/sdma_gcr.h"
#include "amd/sdma_dcc.h"

// =====================================================================================================
// 0.0.417 (an internal design note, D1) — action 82 `sdmadcc [0|1|2]`.
//
// SDMA0_DCC_CNTL (GC BASE_IDX 0, offset 0x0034) turns on no-PTE read DECOMPRESSION and write COMPRESSION for
// our SDMA0 QUEUE0 (VMID 0, FB aperture passed through —), which is why a uniform block reads back as a
// clear constant. `0` reads SDMA0 and SDMA1 raw and decodes every field per set; `1` CAPTURES SDMA0's current
// value on first use, writes `captured & ~0x00015554` (only the eight *_COMP_EN_n bits), reads back and
// reports MATCH/MISMATCH (idempotent); `2` writes the captured value back. Any other argument is REFUSED, and
// so is `2` before any capture. SDMA1 is READ ONLY and never written; SDMA0_DCC_CNTL is the ONLY register this
// verb writes. The argument rules, the mask and the field decode are the host-tested amd/sdma_dcc.h.
// =====================================================================================================
static uint32_t gSdmaDccRestore  { 0 };     // the captured SDMA0_DCC_CNTL value (RESTORE)
static bool     gSdmaDccCaptured { false }; // set once, by the first `sdmadcc 1` of the boot OR by E1's default write below (0.0.418)

uint32_t rdna4_sdmadcc_control(uint64_t arg, uint64_t *out, unsigned count) {
	uint64_t v[9] = { 0 };
	rdna4_display_txn txn(rdna4peer::kLeaseLocalDisplay);
	const uint32_t op = rdna4_sdma_dcc_op(arg, gSdmaDccCaptured ? 1 : 0);
	if (!txn.ok) {
		v[7] = kRDNA4DccStNoContext;
		RDNA4LOG("sdmadcc: REFUSED: no display transaction");
		goto done;
	}
	v[0] = op;
	if (op == kRDNA4DccOpRefused) {
		v[7] = kRDNA4DccStBadArg;
		RDNA4LOG("sdmadcc: argument %llu REFUSED (0 reads, 1 set, 2 restore; 2 before any capture is refused)",
		       (unsigned long long)arg);
		goto done;
	}
	if (!gBringup.dev) { v[7] = kRDNA4DccStNoContext; goto done; }
	{
		const uint32_t gc = rdna4_gc_base();
		if (!gc) { v[7] = kRDNA4DccStNoGc; goto done; }
		const uint32_t reg0 = gc + RDNA4_SDMA_DCC_CNTL_OFF;
		const uint32_t reg1 = gc + RDNA4_SDMA1_DCC_CNTL_OFF;
		// Both registers are READ on every mode (including before a write) so the report always shows the raw
		// pair. SDMA1 is never written.
		const uint32_t s0 = rdna4_reg_read32(reg0);
		const uint32_t s1 = rdna4_reg_read32(reg1);
		v[1] = s0; v[2] = s1;
		if (op == kRDNA4DccOpRead) {
			v[7] = kRDNA4DccStOk;
			RDNA4LOG("sdmadcc: SDMA0_DCC_CNTL %#010x (GC[0]+%#x) SDMA1_DCC_CNTL %#010x (GC[0]+%#x); "
			       "force-bypass %u/%u; set0 rd ovr %u comp %u wr ovr %u comp %u; set1 %u %u %u %u; "
			       "set2 %u %u %u %u; set3 %u %u %u %u",
			       s0, (unsigned)RDNA4_SDMA_DCC_CNTL_OFF, s1, (unsigned)RDNA4_SDMA1_DCC_CNTL_OFF,
			       rdna4_sdma_dcc_force_bypass(s0), rdna4_sdma_dcc_force_bypass(s1),
			       rdna4_sdma_dcc_rd_override(s0, 0u), rdna4_sdma_dcc_rd_comp(s0, 0u),
			       rdna4_sdma_dcc_wr_override(s0, 0u), rdna4_sdma_dcc_wr_comp(s0, 0u),
			       rdna4_sdma_dcc_rd_override(s0, 1u), rdna4_sdma_dcc_rd_comp(s0, 1u),
			       rdna4_sdma_dcc_wr_override(s0, 1u), rdna4_sdma_dcc_wr_comp(s0, 1u),
			       rdna4_sdma_dcc_rd_override(s0, 2u), rdna4_sdma_dcc_rd_comp(s0, 2u),
			       rdna4_sdma_dcc_wr_override(s0, 2u), rdna4_sdma_dcc_wr_comp(s0, 2u),
			       rdna4_sdma_dcc_rd_override(s0, 3u), rdna4_sdma_dcc_rd_comp(s0, 3u),
			       rdna4_sdma_dcc_wr_override(s0, 3u), rdna4_sdma_dcc_wr_comp(s0, 3u));
			goto done;
		}
		if (op == kRDNA4DccOpSet) {
			if (!gSdmaDccCaptured) { gSdmaDccRestore = s0; gSdmaDccCaptured = true; }
			const uint32_t target = rdna4_sdma_dcc_cleared(gSdmaDccRestore);
			v[3] = gSdmaDccRestore; v[4] = target;
			rdna4_reg_write32(reg0, target);
			const uint32_t rb = rdna4_reg_read32(reg0);
			v[5] = rb; v[6] = (rb == target) ? 1u : 0u;
			v[7] = (rb == target) ? kRDNA4DccStOk : kRDNA4DccStMismatch;
			RDNA4LOG("sdmadcc: SET SDMA0_DCC_CNTL %#010x -> %#010x (mask %#x), read back %#010x %s",
			       gSdmaDccRestore, target, (unsigned)RDNA4_DCC_NOPTE_COMP_EN_MASK, rb,
			       (rb == target) ? "MATCH" : "MISMATCH");
			goto done;
		}
		// op == kRDNA4DccOpRestore (READ and SET were handled above).
		rdna4_reg_write32(reg0, gSdmaDccRestore);
		const uint32_t rb = rdna4_reg_read32(reg0);
		v[3] = gSdmaDccRestore; v[4] = gSdmaDccRestore; v[5] = rb;
		v[6] = (rb == gSdmaDccRestore) ? 1u : 0u;
		v[7] = (rb == gSdmaDccRestore) ? kRDNA4DccStOk : kRDNA4DccStMismatch;
		RDNA4LOG("sdmadcc: RESTORE SDMA0_DCC_CNTL <- %#010x, read back %#010x %s",
		       gSdmaDccRestore, rb, (rb == gSdmaDccRestore) ? "MATCH" : "MISMATCH");
	}
done:
	v[8] = gSdmaDccCaptured ? 1u : 0u;
	if (out) for (unsigned i = 0; i < count && i < 9; i++) out[i] = v[i];
	return (uint32_t)v[7];
}

// =====================================================================================================
// E1 (0.0.418, an internal design note) — THE SDMA0_DCC_CNTL CLEAR IS THE DEFAULT.
//
// THE SITE, cited: SDMA0's QUEUE0 ring is up when `sdma_init_full` returns in the ladder's SDMAInit stage
// (src/amd/amdgpu_init.cpp, the BringupStage::SDMAInit case). 0.0.657 (GitHub issue #1, fixed on the public repo in
// 1a04d8e): that case now calls this function IMMEDIATELY after `sdma_init_full` succeeds and BEFORE the ladder's own
// SDMA copy test and copy_sweep (through 0.0.656 those two ran first, on the boot default `0x0000aabe` with no-PTE
// compression ON, and their results disagreed from run to run). So from the first SDMA packet of the boot onward,
// `fire`, the copy test, the sweep, the boot chain, `scanout` and the pipe guard all run with no-PTE compression OFF.
//
// The write happens ONCE per boot. `GarnetDriver::runStages` still calls this function at the first kext-layer
// point after the stage (`ctx.reached >= BringupStage::SDMAInit`); by then it has run, so that call logs that it
// already ran and writes nothing (gSdmaDccDefaultDone, set on the opt-out path and right after the write, NOT on the
// 'no DeviceContext' / 'GC base did not resolve' skips, so a skipped early call is retried by the later one).
//
// It captures the boot value into the SAME RESTORE pair the `sdmadcc` verb uses, writes
// `rdna4_sdma_dcc_cleared` of it, reads back, and logs ONE line. `rdna4-sdmadcc=0` skips it; absent or nonzero
// runs it (rdna4_sdma_dcc_default_on). SDMA1 is never written and no other register is written at all.
// =====================================================================================================
static bool gSdmaDccDefaultDone { false };   // the default clear (or its opt-out) has been decided this boot
void rdna4_sdmadcc_default(void) {
	if (gSdmaDccDefaultDone) {
		RDNA4LOG("sdmadcc: DEFAULT already handled earlier this boot (right after sdma_init_full, before the copy test) - no second write");
		return;
	}
	uint32_t ba = 0;
	const uint32_t present = PE_parse_boot_argn("garnet-sdmadcc", &ba, sizeof(ba)) ? 1u : 0u;
	if (!rdna4_sdma_dcc_default_on(present, ba)) {
		gSdmaDccDefaultDone = true;
		RDNA4LOG("sdmadcc: DEFAULT SKIPPED - garnet-sdmadcc=0 (SDMA0_DCC_CNTL left as it booted)");
		return;
	}
	if (!gBringup.dev) { RDNA4LOG("sdmadcc: DEFAULT SKIPPED - no DeviceContext"); return; }
	const uint32_t gc = rdna4_gc_base();
	if (!gc) { RDNA4LOG("sdmadcc: DEFAULT SKIPPED - GC BASE_IDX 0 did not resolve"); return; }
	const uint32_t reg0 = gc + RDNA4_SDMA_DCC_CNTL_OFF;
	if (!gSdmaDccCaptured) { gSdmaDccRestore = rdna4_reg_read32(reg0); gSdmaDccCaptured = true; }
	const uint32_t target = rdna4_sdma_dcc_cleared(gSdmaDccRestore);
	rdna4_reg_write32(reg0, target);
	gSdmaDccDefaultDone = true;
	const uint32_t rb = rdna4_reg_read32(reg0);
	RDNA4LOG("sdmadcc: DEFAULT SDMA0_DCC_CNTL %#010x -> %#010x (mask %#x), read back %#010x %s",
	       gSdmaDccRestore, target, (unsigned)RDNA4_DCC_NOPTE_COMP_EN_MASK, rb,
	       (rb == target) ? "MATCH" : "MISMATCH");
}

static IOService *gBringupPci { nullptr };      // our IOPCIDevice, for finding RDNA4FB under it
IOService *garnet_driver_pci(void) { return gBringupPci; }
IOService *garnet_driver_service(void) { return gBringupSelf; }
// 0.0.613 (amd/native_disp.h): the display pipe's window onto the console buffer. No-ops (false) before start() published the service.
bool rdna4_console_region(uint64_t *off, uint64_t *len, uint32_t *w, uint32_t *h, uint32_t *rowBytes) {
	return gBringupSelf && static_cast<GarnetDriver *>(gBringupSelf)->consoleGeometry(off, len, w, h, rowBytes);
}
bool rdna4_bar0_write_combined(void) {   // 0.0.615 (W1): `pipe stat` flag bit 32; false before start() published the service
	return gBringupSelf && static_cast<GarnetDriver *>(gBringupSelf)->bar0WriteCombined();
}
bool rdna4_console_write(uint64_t consoleOff, const void *src, size_t len) {
	return gBringupSelf && static_cast<GarnetDriver *>(gBringupSelf)->consoleWrite(consoleOff, src, len);
}
// Per-VMID TLB / walker-cache invalidate via gmc_flush_gpu_tlb (amd/amdgpu_gmc.h).
bool rdna4_gmc_flush_tlb_vmid(uint32_t vmid, uint32_t flush_type) {
	if (!gBringup.dev || !gBringup.gmc.inited) return false;
	if (!gBringup.gmc.gfxhub.inited) return false;
	if (vmid > 15) return false;
	const kern_return_t r = amdgpu::gmc_flush_gpu_tlb(*gBringup.dev, gBringup.gmc,
	                                                  gBringup.gmc.gfxhub, vmid,
	                                                  flush_type);
	if (r != kIOReturnSuccess)
		RDNA4LOG("gmc-flush-tlb: GFXHUB vmid %u type %u returned %#x - the TLB was NOT "
		       "invalidated", vmid, flush_type, r);
	return r == kIOReturnSuccess;
}


// ---------------------------------------------------------------------------
// Userspace access
// ---------------------------------------------------------------------------

amdgpu::BringupContext *GarnetDriver::bringupContext() { return &gBringup; }

void GarnetDriver::fillCounters(RDNA4Counters *out) {
	if (!out) return;
	out->irq_count        = irqCount;
	out->irq_entries      = irqEntries;
	out->irq_eop          = irqEop;
	out->irq_faults       = irqFaults;
	out->irq_cp_errors    = irqCpError;
	out->irq_other        = irqOther;
	out->submits          = submits;
	out->fences_completed = fencesCompleted;
	out->fence_timeouts   = fenceTimeouts;
}

IOReturn GarnetDriver::newUserClient(task_t owningTask, void *securityID, UInt32 type,
                                      OSDictionary *properties, IOUserClient **handler) {
	if (!handler) return kIOReturnBadArgument;
	// NATIVE S1c (0.0.601): type 'GRNT' is the native user client; every other type is the legacy GarnetUserClient, exactly as before.
	if (type == GARNETN_UC_TYPE) return IOAccelGarnetNativeClient::create(this, this, rdna4native::policy::kRouteBringup, owningTask, securityID, type, properties, handler);   // 0.0.656 (G6): the GarnetDriver route (WindowServer, root tools); the accelerator route is GarnetMetalNub's native_open
	auto *uc = OSTypeAlloc(GarnetUserClient);
	if (!uc) return kIOReturnNoMemory;
	if (!uc->initWithTask(owningTask, securityID, type, properties)) {
		uc->release(); return kIOReturnNotPermitted;
	}
	if (!uc->attach(this)) { uc->release(); return kIOReturnInternalError; }
	if (!uc->start(this))  { uc->detach(this); uc->release(); return kIOReturnInternalError; }
	*handler = uc;
	return kIOReturnSuccess;
}

// ---------------------------------------------------------------------------
// Interrupts
// ---------------------------------------------------------------------------
//
// The card is a PCIe device with MSI capability; macOS exposes each interrupt
// the device can raise as a numbered source on the IOPCIDevice, and
// getInterruptType() says which of them is message-signalled. We take the
// first messaged source — INTx on a modern Radeon is shared and level
// triggered, and acking it needs register knowledge we would rather not
// depend on during bring-up.
//
// The handler runs on our own work loop (IOInterruptEventSource dispatches
// there, not in primary interrupt context), so draining the ring, reading
// registers and logging are all legal.
//
// Acking: IH v7 rearms through IH_RB_RPTR, which ih_process() already writes
// at the end of every drain (RPTR_REARM is set when enable_cpu_intr is on).
// So "handle" == "drain the ring", and an empty drain is harmless.

void GarnetDriver::interruptTrampoline(OSObject *owner, IOInterruptEventSource *src, int count) {
	auto *self = OSDynamicCast(GarnetDriver, owner);
	if (self) self->handleInterrupt(count);
	(void)src;
}

void GarnetDriver::handleInterrupt(int count) {
	irqCount++;
	amdgpu::BringupContext &ctx = gBringup;
	if (!ctx.ih.inited) return;

	amdgpu::IHEntry e[32];
	uint32_t total = 0;
	// Drain until the ring reports empty; a single MSI can cover many entries.
	for (int pass = 0; pass < 16; pass++) {
		uint32_t n = amdgpu::ih_poll(dev, ctx.ih, e, 32, 0);
		if (n == 0) break;
		total += n;
		bool passWake = false;
		for (uint32_t i = 0; i < n; i++) {
			// DCE (display) entries first, by CLIENT id: the DCN source ids overlap nothing in the
			// switch below, but a DCE entry must be recognised by client AND source together
			// (dcn41_ih_to_irq returns NONE for any other client). rdna4dcn classifies it, acknowledges
			// it the way amdgpu_dm_irq_handler does — ack before anything else — counts it by kind and
			// instance, and keeps its own small log budget, so these never land in irqOther.
			if (rdna4dcn::ihEntry(e[i].client_id, e[i].src_id, e[i].src_data[0])) continue;
			if (rdna4native::irqwait::should_wake_on_iv(e[i].src_id)) passWake = true;
			switch (e[i].src_id) {
			case 0:   irqFaults++;  break;   // UTCL2 / VM fault
			case 181: irqEop++;     break;   // CP end-of-pipe
			case 183: case 184: case 185: irqCpError++; break;
			default:  irqOther++;   break;
			}
			// 0.0.385 — src 0 (UTCL2 / VM fault) gets its OWN budget and its OWN line.
			// dw1-3 are reconstructed from the decode (amdgpu_ih.h: DW1 = timestamp_lo, DW2 =
			// timestamp_hi in [15:0], DW3 = pasid | node_id << 16).
			if (e[i].src_id == 0) {
				if (irqSrc0LogBudget > 0) {
					irqSrc0LogBudget--;
					const uint32_t dw1 = (uint32_t)(e[i].timestamp & 0xFFFFFFFFull);
					const uint32_t dw2 = (uint32_t)((e[i].timestamp >> 32) & 0xFFFFull);
					const uint32_t dw3 = (uint32_t)e[i].pasid | ((uint32_t)e[i].node_id << 16);
					RDNA4LOG("irq: src0 fault client %u ring %u vmid %u vmidsrc %u data %08x %08x %08x %08x; "
					       "dw1 %08x dw2 %08x dw3 %08x (ts %#llx, pasid %#x, node %u)",
					       e[i].client_id, e[i].ring_id, e[i].vmid, e[i].vmid_src,
					       e[i].src_data[0], e[i].src_data[1], e[i].src_data[2], e[i].src_data[3],
					       dw1, dw2, dw3, (unsigned long long)e[i].timestamp, e[i].pasid, e[i].node_id);
				}
			} else if (irqLogBudget > 0) {
				irqLogBudget--;
				RDNA4LOG("irq: client %u src %u ring %u vmid %u data %08x %08x",
				       e[i].client_id, e[i].src_id, e[i].ring_id, e[i].vmid,
				       e[i].src_data[0], e[i].src_data[1]);
			}
		}
		if (passWake) amdgpu::n1c_irq_notify();
	}
	irqEntries += total;
	(void)count;
}

bool GarnetDriver::armInterrupts() {
	if (intArmed) return true;
	if (!pciDevice) return false;

	int msiIndex = -1;
	for (int i = 0; i < 8; i++) {
		int type = 0;
		if (pciDevice->getInterruptType(i, &type) != kIOReturnSuccess) break;
		RDNA4LOG("irq: source %d type %#x%s", i, type,
		       (type & kIOInterruptTypePCIMessaged) ? " (message-signalled)" : " (level/INTx)");
		if (type & kIOInterruptTypePCIMessaged) { msiIndex = i; break; }
	}
	if (msiIndex < 0) {
		RDNA4LOG("irq: no message-signalled interrupt source — staying on the polled path");
		return false;
	}

	workLoop = IOWorkLoop::workLoop();
	if (!workLoop) { RDNA4LOG("irq: no work loop"); return false; }
	intSource = IOInterruptEventSource::interruptEventSource(
		this, &GarnetDriver::interruptTrampoline, pciDevice, msiIndex);
	if (!intSource) {
		RDNA4LOG("irq: interruptEventSource(index %d) failed", msiIndex);
		workLoop->release(); workLoop = nullptr; return false;
	}
	if (workLoop->addEventSource(intSource) != kIOReturnSuccess) {
		RDNA4LOG("irq: addEventSource failed");
		intSource->release(); intSource = nullptr;
		workLoop->release(); workLoop = nullptr; return false;
	}
	intSource->enable();
	intArmed = true;
	irqLogBudget = 32;
	irqSrc0LogBudget = kIrqSrc0LogLines;   // 0.0.385: src-0 entries get their own, wider budget
	RDNA4LOG("irq: armed on MSI source %d — IH ring will run with ENABLE_INTR=1", msiIndex);
	// A SEPARATE line, deliberately: the line above is 0.0.384's verbatim, so nothing that reads it moves.
	RDNA4LOG("irq: src-0 (UTCL2 / VM fault) entries have their OWN log budget of %u line(s) this boot and print "
	       "dw1-3 and src_data[2..3] (: class A's walk is 43-102 entries and the shared 32 could not "
	       "carry one). Worst case %u extra line(s) for the whole boot; a fault-free boot prints none of them.",
	       irqSrc0LogBudget, kIrqSrc0LogLines);
	return true;
}

void GarnetDriver::disarmInterrupts() {
	if (intSource) {
		intSource->disable();
		if (workLoop) workLoop->removeEventSource(intSource);
		intSource->release(); intSource = nullptr;
	}
	if (workLoop) { workLoop->release(); workLoop = nullptr; }
	intArmed = false;
}

void GarnetDriver::publishInterruptCounters() {
	if (!intArmed) return;
	setProperty("Garnet,IRQArmed", true);
	setProperty("Garnet,IRQCount", irqCount, 64);
	setProperty("Garnet,IRQEntries", irqEntries, 64);
	setProperty("Garnet,IRQEop", irqEop, 64);
	setProperty("Garnet,IRQFaults", irqFaults, 64);
	setProperty("Garnet,IRQCPErrors", irqCpError, 64);
	setProperty("Garnet,IRQOther", irqOther, 64);
	uint64_t waitSleeps = 0, waitWakes = 0;
	amdgpu::n1c_irq_wait_stats(&waitSleeps, &waitWakes);
	setProperty("Garnet,IRQWaitSleeps", waitSleeps, 64);
	setProperty("Garnet,IRQWaitWakes", waitWakes, 64);
	RDNA4LOG("irq: %llu interrupts, %llu entries (eop %llu, faults %llu, cp-errors %llu, other %llu, wait-sleeps %llu, wait-wakes %llu)",
	       irqCount, irqEntries, irqEop, irqFaults, irqCpError, irqOther, waitSleeps, waitWakes);
}

// ---------------------------------------------------------------------------
// IOService lifecycle
// ---------------------------------------------------------------------------

IOService *GarnetDriver::probe(IOService *provider, SInt32 *score) {
	uint32_t on = 0;
	if (!PE_parse_boot_argn("garnet", &on, sizeof(on)) || on == 0) return nullptr;
	if (!super::probe(provider, score)) return nullptr;
	auto *pci = OSDynamicCast(IOPCIDevice, provider);
	if (!pci) return nullptr;
	uint32_t vendorDevice = pci->configRead32(kIOPCIConfigVendorID);
	uint16_t device = static_cast<uint16_t>(vendorDevice >> 16);
	// RDNA4: every chip of amdgpu_asic.h (Navi 48 7550/7551, Navi 44 7590), and only when its
	// firmware is embedded (a CHIPS=navi48 build has empty Navi 44 stubs and must not claim a 9060 XT).
	const amdgpu::AsicInfo *asic = amdgpu::asic_from_pci(static_cast<uint16_t>(vendorDevice & 0xffff), device);
	if (!asic) {
		RDNA4LOG("probe: 0x%08x not Navi 48 / Navi 44, ignoring", vendorDevice); return nullptr;
	}
	if (!amdgpu::fw_chip_complete(asic)) {
		RDNA4LOG("probe: %s %s (0x%04x) but this build has no %s firmware (make CHIPS=...); ignoring",
		       asic->family, amdgpu::asic_pci_name(asic, device), device, asic->family);
		return nullptr;
	}
	RDNA4LOG("probe: %s %s (0x%04x) — bring-up enabled", asic->family, amdgpu::asic_pci_name(asic, device), device);
	return this;
}

bool GarnetDriver::start(IOService *provider) {
	if (!super::start(provider)) return false;
	if (!gVramMmLock) gVramMmLock = IOLockAlloc();   // never freed: a verb may hold it at stop
	rdna4_scanout_sdma_init_locks();
	if (!amdgpu::gTlb83Lock) amdgpu::gTlb83Lock = IOLockAlloc();   // build 0.0.528 (switch 83's leaf lock): never freed, same reason
	if (!gBringupLifecycleLock) gBringupLifecycleLock = IOLockAlloc();
	if (!displayPeerLock) displayPeerLock = IOLockAlloc();
	if (!gBringupLifecycleLock || !displayPeerLock) {
		RDNA4LOG("start: display peer locks unavailable");
		return false;
	}
	// One lock for every indexed-window operation: the local RVRAM/WVRAM
	// primitives, GarnetDriver::vramRead32/vramWrite32, and the display's
	// delegated kOpVramAccess. It must exist before STARTING is published or
	// any indexed access can happen; a start without it fails closed.
	if (!gMmIndexWindowLock) gMmIndexWindowLock = IOLockAlloc();
	if (!gMmIndexWindowLock) {
		RDNA4LOG("start: indexed MMIO window lock unavailable");
		return false;
	}
	dev.mmIndexLock = gMmIndexWindowLock;   // borrowed, never owned by the service
	pciDevice = OSDynamicCast(IOPCIDevice, provider);
	gBringupPci = pciDevice;
	// Derive host RAM top once from the EFI memory map before sysmem import checks run.
	amdgpu::sysmem_ramtop_init();
	if (!pciDevice) { RDNA4LOG("start: no PCI provider"); return false; }
	pciDevice->setMemoryEnable(true);
	latchPciBars(pciDevice);   // 0.0.612 (review item A): every BAR's FULL base and size, once, for BoImportHost's device-page refusal
	publishDisplayPeer(rdna4peer::kStateStarting);

	if (!mapRegisters()) { RDNA4LOG("start: BAR5 unavailable — aborting"); return false; }
	if (!probeMemSize())  { RDNA4LOG("start: MMIO not usable — aborting"); unmapRegisters(); return false; }
	const bool haveDiscovery = loadOnDieDiscovery();
	if (!selectAsic(haveDiscovery)) { unmapRegisters(); return false; }
	if (!haveDiscovery) {
		RDNA4LOG("start: IP discovery unavailable — MMIO works, but no register map");
	} else {
		surveyIPs();
		surveyPSP();
		surveySMU();
		if (!PE_parse_boot_argn("garnet-stage", &targetStage, sizeof(targetStage))) targetStage = 0;
		if (targetStage) RDNA4LOG("stages: rdna4-stage=%u (mac-amdgpu BringupStage numbering; 5 = PSP SOS)", targetStage);
		uint32_t wantIrq = 1;
		PE_parse_boot_argn("garnet-interrupts", &wantIrq, sizeof(wantIrq));
		if (wantIrq != 0) gBringup.wantInterrupts = armInterrupts();
		stagePSP();        // no-op unless rdna4-psp=1 or rdna4-stage>=5
		publishInterruptCounters();
	}
	rdna4dcn::attach(this);
	IOLockLock(gBringupLifecycleLock);
	if (gBringupSelf && gBringupSelf != this) gBringupSelf->release();
	retain();
	gBringupSelf = this;
	IOLockUnlock(gBringupLifecycleLock);
	if (!sleepLock) sleepLock = IOLockAlloc();
	{
		uint32_t sysSleepArg = 1u;
		PE_parse_boot_argn("garnet-syssleep", &sysSleepArg, sizeof(sysSleepArg));
		if (sysSleepArg != 0u && sleepLock && !sleepNotifier) {
			sleepNotifier = registerPrioritySleepWakeInterest(&GarnetDriver::sleepWakeCallback, this, nullptr);
			RDNA4LOG("sleep: priority sleep/wake interest %s", sleepNotifier ? "registered" : "failed");
		}
	}
	publishDisplayPeer(rdna4peer::kStateReady);
	garnet_webkit::install(this);
	registerService();
	garnet_primary_fb_notify_driver_ready(this);
	RDNA4LOG("start: ready");
	return true;
}

// Direct CPU BAR0 <-> GPU bidirectional verification on allocator-granted exclusive regions
// (4/8/12 GiB boundaries + highest valid offset below vramLimit). Uses bar0Virt ONLY (WC drain + HDP flush)
// plus SDMA0 QUEUE0 COPY_LINEAR — never MM_INDEX. Fails closed if BAR0 does not cover the targets.
IOReturn GarnetDriver::runHighBarBidirTest(uint64_t arg, uint64_t out[kAccelExtraScalars]) {
	using namespace rdna4rebar;
	for (unsigned i = 0; i < kAccelExtraScalars; i++) out[i] = 0;
	out[8]  = bar0Size;
	out[9]  = bar0Limit;
	out[10] = bar0Phys;
	out[11] = (bar0FullMode ? 1ull : 0ull) | (bar0WriteCombined() ? 2ull : 0ull);
	if (!bartest_arg_ok(arg)) { out[0] = kBtBadArg; return kIOReturnBadArgument; }
	if (amdgpu::n1c_hung()) { out[0] = kBtSdmaNotReady; return kIOReturnSuccess; }
	if (!bar0 || bar0Size == 0 || bar0Limit == 0 || !gBringup.dev) { out[0] = kBtNoBar0; return kIOReturnSuccess; }
	if (!gBringup.gmc.vram_alloc.is_inited()) { out[0] = kBtAllocFail; return kIOReturnSuccess; }
	if (!gBringup.sdma.instance[0].inited || !gBringup.sdma.instance[0].enabled) { out[0] = kBtSdmaNotReady; return kIOReturnSuccess; }

	const uint64_t visBase    = vramBase + kVisAllocOffset;
	const uint64_t vramBytes  = static_cast<uint64_t>(vramMB) << 20;
	const uint64_t consoleOff = bootFbOff == ~0ULL ? kNoConsole : bootFbOff;
	uint64_t targets[kMaxBarTestTargets] = {};
	uint32_t nTargets = 0;
	const uint32_t planSt = bartest_plan_targets(bar0FullMode, arg, visBase, bar0Limit, bar0Size,
	                                             vramBytes, consoleOff, bootFbLen, targets, &nTargets);
	if (planSt != kBtOk) {
		out[0] = planSt;
		RDNA4LOG("bartest: plan refused (%u: %s) arg=%llu fullMode=%u bar0Size=%zuMiB vramLimit=%lluMiB",
		         planSt, bartest_status_name(planSt), (unsigned long long)arg, bar0FullMode ? 1u : 0u,
		         bar0Size >> 20, (unsigned long long)(bar0Limit >> 20));
		return kIOReturnSuccess;
	}
	out[1] = nTargets;

	amdgpu::VRAMAllocation canary {}, scratch {};
	if (!amdgpu::n1c_vram_alloc_excl(gBringup, kBarTestCanaryBytes, 65536ull, &canary)) {
		out[0] = kBtAllocFail;
		return kIOReturnSuccess;
	}
	if (!amdgpu::n1c_vram_alloc_excl(gBringup, kBarTestCanaryBytes, 65536ull, &scratch)) {
		amdgpu::n1c_vram_free_excl(gBringup, canary);
		out[0] = kBtAllocFail;
		return kIOReturnSuccess;
	}
	const uint64_t vramStart  = gBringup.gmc.vram_start;
	const uint64_t canaryOff  = canary.gpu_va - vramStart;
	const uint64_t scratchOff = scratch.gpu_va - vramStart;
	if (arg == 0ull && canaryOff >= 4ull * kGiB) {
		amdgpu::n1c_vram_free_excl(gBringup, scratch);
		amdgpu::n1c_vram_free_excl(gBringup, canary);
		out[0] = kBtAllocFail;
		RDNA4LOG("bartest: low canary landed at %#llx (>= 4 GiB); refusing", (unsigned long long)canaryOff);
		return kIOReturnSuccess;
	}

	static volatile uint32_t gBarTestFenceSeq = 0x5B400000u;
	struct DirectBarOps {
		amdgpu::DeviceContext *d;
		void     bar0_write32(uint64_t off, uint32_t val) { amdgpu::WBAR0_32(*d, off, val); }
		uint32_t bar0_read32(uint64_t off)                { return amdgpu::RBAR0_32(*d, off); }
		void     wc_and_hdp_flush()                       { storeFence(); amdgpu::amdgpu_hdp_flush(*d); }
		void     hdp_flush()                              { amdgpu::amdgpu_hdp_flush(*d); }
		bool     gpu_copy(uint64_t srcMc, uint64_t dstMc, uint32_t bytes) {
			const uint32_t f = __atomic_add_fetch(&gBarTestFenceSeq, 1u, __ATOMIC_RELAXED);
			return rdna4_sdma_copy_linear_sync(srcMc, dstMc, bytes, f, 200u) == 0u;
		}
	};
	DirectBarOps ops { gBringup.dev };

	uint32_t passed = 0;
	uint32_t finalSt = kBtOk;
	for (uint32_t i = 0; i < nTargets; i++) {
		const uint64_t targetOff = targets[i];
		out[12] = targetOff;
		amdgpu::VRAMAllocation blk {};
		if (!amdgpu::n1c_vram_alloc_excl_at(gBringup, vramStart + targetOff, kBarTestBlockBytes, &blk)) {
			finalSt = kBtAllocFail;
			out[3] = i; out[4] = targetOff;
			RDNA4LOG("bartest: target[%u] vram+%#llx exclusive alloc_at refused (in use or out of range)",
			         i, (unsigned long long)targetOff);
			break;
		}
		BarTestPointIn pin {
			vramStart, visBase, bar0Limit, bar0Size, vramBytes,
			consoleOff, bootFbLen, targetOff, canaryOff, scratchOff
		};
		const BarTestPointOut ptRes = bartest_run_point(ops, pin);
		amdgpu::n1c_vram_free_excl(gBringup, blk);
		if (ptRes.status != kBtOk) {
			finalSt = ptRes.status;
			out[3] = i; out[4] = targetOff; out[5] = ptRes.firstBadIdx; out[6] = ptRes.want; out[7] = ptRes.got;
			RDNA4LOG("bartest: target[%u] vram+%#llx FAILED (%u: %s) idx=%u want=%#010x got=%#010x",
			         i, (unsigned long long)targetOff, ptRes.status, bartest_status_name(ptRes.status),
			         ptRes.firstBadIdx, ptRes.want, ptRes.got);
			break;
		}
		passed++;
		RDNA4LOG("bartest: target[%u] vram+%#llx PASSED (direct BAR0 <-> SDMA bidirectional + guards + canary %#llx)",
		         i, (unsigned long long)targetOff, (unsigned long long)canaryOff);
	}
	amdgpu::n1c_vram_free_excl(gBringup, scratch);
	amdgpu::n1c_vram_free_excl(gBringup, canary);
	out[0] = finalSt;
	out[2] = passed;
	return kIOReturnSuccess;
}

// Native bringup/display verb dispatch, reachable from the admin user client.
IOReturn GarnetDriver::accelExperiment(uint32_t action, uint64_t *outInstalled,
                                        uint64_t *outTtlCalls, uint64_t *outFirstUnsupported,
                                        uint64_t *outExtra, uint64_t argScalar) {
	if (outExtra)
		for (unsigned i = 0; i < kAccelExtraScalars; i++) outExtra[i] = 0;
	if (outInstalled)        *outInstalled = 0;
	if (outTtlCalls)         *outTtlCalls = 0;
	if (outFirstUnsupported) *outFirstUnsupported = 0;
	// G1 actions 100..102 act only on the native kernel GFX queue and native test context.
	if (rdna4native::g1::is_g1_action(action)) {
		if (!rdna4native::g1::native_exempt(amdgpu::n1c_g1_latched_on(), action, argScalar)) return kIOReturnBadArgument;
		uint64_t v[rdna4native::g1::kOutN] = { 0 };
		const IOReturn kr = amdgpu::n1c_g1_verb(action, argScalar, gBringup, v);
		RDNA4LOG("accel-experiment: %s %llu -> kr %#x code %llu", action == rdna4native::g1::kActHangTest ? "hangtest" : action == rdna4native::g1::kActHangRecover ? "hangrecover" : "hangstat",
		       (unsigned long long)argScalar, kr, (unsigned long long)v[0]);
		if (outExtra)
			for (unsigned i = 0; i < rdna4native::g1::kOutN && i < kAccelExtraScalars; i++) outExtra[i] = v[i];
		return kr;
	}
	// action 103 (0.0.627, GPU-apps G2; boot-arg garnet-multisession=1 only): `sessstat <0..8>`
	if (rdna4native::g2::is_g2_action(action)) {
		const bool g5page = rdna4native::g5::args_ok(amdgpu::n1c_g5_latched_on(), action, argScalar);
		if (!g5page && !rdna4native::g2::native_exempt(amdgpu::n1c_multi_latched_on(), action, argScalar)) return kIOReturnBadArgument;
		uint64_t v[rdna4native::g2::kOutN] = { 0 };
		const IOReturn kr = g5page ? amdgpu::n1c_g5_stat(v) : amdgpu::n1c_g2_stat(argScalar, v);
		RDNA4LOG("accel-experiment: sessstat %llu -> kr %#x code %llu", (unsigned long long)argScalar, kr, (unsigned long long)v[0]);
		if (outExtra)
			for (unsigned i = 0; i < rdna4native::g2::kOutN && i < kAccelExtraScalars; i++) outExtra[i] = v[i];
		return kr;
	}
	// actions 104 (`appallow`) & 110 (`appdeny`): admin-only legacy user client; lock-free static tables in kext data.
	if (rdna4native::g4::is_g4_action(action)) {
		if (!rdna4native::g4::native_exempt(amdgpu::n1c_apps_enabled(), action, argScalar)) return kIOReturnBadArgument;
		uint64_t v[rdna4native::g4::kOutN] = { 0 };
		const bool isDeny = action == rdna4native::g4::kActAppDeny;
		const IOReturn kr = isDeny ? amdgpu::n1c_app_deny_verb(argScalar, v) : amdgpu::n1c_app_verb(argScalar, v);
		RDNA4LOG("accel-experiment: %s op %llu -> kr %#x code %llu entries %llu", isDeny ? "appdeny" : "appallow", (unsigned long long)v[2], kr, (unsigned long long)v[0], (unsigned long long)v[1]);
		if (outExtra)
			for (unsigned i = 0; i < rdna4native::g4::kOutN && i < kAccelExtraScalars; i++) outExtra[i] = v[i];
		return kr;
	}
	// action 109 (enable-rdna4-16g-bar Task 4.1): direct CPU BAR0 <-> GPU bidirectional verification on exclusive high-VRAM regions.
	// Root-only legacy client; fails closed if BAR0 does not cover targets; never falls back to MM_INDEX.
	if (rdna4rebar::bartest_is_action(action)) {
		uint64_t v[kAccelExtraScalars] = { 0 };
		const IOReturn kr = runHighBarBidirTest(argScalar, v);
		RDNA4LOG("accel-experiment: bartest %llu -> kr %#x status %llu (%s), passed %llu/%llu",
		         (unsigned long long)argScalar, kr, (unsigned long long)v[0],
		         rdna4rebar::bartest_status_name(static_cast<uint32_t>(v[0])),
		         (unsigned long long)v[2], (unsigned long long)v[1]);
		if (outExtra)
			for (unsigned i = 0; i < kAccelExtraScalars; i++) outExtra[i] = v[i];
		return kr;
	}
	// action 115 (ABI 1.14): root-only kernel kill-switch control (0 = off, 1 = on, 2 = read only).
	if (rdna4native::policy::killswitch_is_action(action)) {
		if (!rdna4native::policy::killswitch_arg_ok(argScalar)) return kIOReturnBadArgument;
		if (argScalar == rdna4native::policy::kKillSwitchOff) amdgpu::n1c_set_kill_switch(false);
		else if (argScalar == rdna4native::policy::kKillSwitchOn) amdgpu::n1c_set_kill_switch(true);
		const uint64_t now = amdgpu::n1c_kill_switch() ? 1ull : 0ull;
		if (outExtra) outExtra[0] = now;
		return kIOReturnSuccess;
	}
	if (!rdna4scan::accel_exempt(action, argScalar) && !rdna4disp::native_exempt(rdna4disp_latched_on(), action, argScalar)) {
	if (action != 0 && amdgpu::native_s1b_refuse(0)) return kIOReturnNotPermitted;
	}
	// 0.0.613: the action bound again, here (the user client checks it first): 83..87 exist only with the latch ON.
	if (action > rdna4disp::kLastOldAction && !rdna4disp::action_admitted(rdna4disp_latched_on(), action)) return kIOReturnBadArgument;
	if (action == 0) {
		publishInterruptCounters();
		return kIOReturnSuccess;              // status only
	}
	// actions 83..87 (boot-arg garnet-metal-disp=1 only): native display pipe control.
	// exclusive, so `pipe arm 0` and `fbname 0` stay reachable while WindowServer holds the exclusive RDNA4N connection.
	//   83 pipeadopt            : turn the resource facts on, ask the accelerator for its probe (requestProbe(1)), verify the pipe is a GarnetDisplayPipe of ours on RDNA4FB, publish the capabilities.
	//   84 pipearm [0|1]        : 1 writes accel+0xccf (the type-4 user-client gate) once a verified pipe exists; 0 is ALWAYS allowed.
	//   85 pipestat [0|1|2|3|4] : the counters, in five pages (3: scan-owned + submit interval, 4: the vblank timestamps, 0.0.618).
	//   86 pipestamps           : READ-ONLY dump of the event machine's stamp words (SUSPECTED layout).
	//   87 pipeshortcut [0|1]   : the slot-62 'already prepared' shortcut switch (default ON; guarded by the SysMemory flag bit 4 either way).
	//   89 pipevbl [0|1]        : (0.0.618) the vblank-timestamp switch: 1 (the default with the latch ON) writes the transaction's +0x178 / +0x188 (next vblank time, next + one period; mach_absolute_time units) in the perform hook, 0 = 0.0.617 behaviour. `pipestat 4` shows the counts.
	//   90 pipereload [0|1]     : (0.0.619) the operator restart window: 0 (the default) opens a one-shot 15 s window in which the next uid-88 WindowServer client close while armed does not auto-disarm and slot-267 calls are not counted by the restart guard; 1 only reads it. Then `killall -9 WindowServer`.
	// action 106 (0.0.659, M6 Stage 1a; the same latch as 83..90): `m6stat [0|1|2|3]` - READ-ONLY report of the multi-display routing: per pipe instance the submits, the surface IDs (3 expected per display), the ambiguous ones, the refused presents and
	// their reasons, the performs completed without a copy, the vblank stamps per OTG and the AGDC answers. Writes nothing, reads no register. Served by rdna4disp_verb.
	// action 108 (0.0.663, ReBAR + Stage 2 review S3; the same latch as 83..90): `vramstat` - READ-ONLY report of the kernel allocator's visible-VRAM state: the pools' totals and free bytes, vramLimit, the mapped BAR0 bytes and BAR0's physical
	// address. Writes nothing and reads no register; the pool figures are plain loads of the allocator's counters (a diagnostic, like QueryInfo's). v[0] status (0 ok, 1 the ladder / allocator is not up: the BAR0 fields are still filled),
	// v[1] visible total, v[2] visible free, v[3] hi total, v[4] hi free, v[5] vramLimit, v[6] mapped BAR0 bytes, v[7] BAR0 phys, v[8] VRAM bytes, v[9] vramBase, v[10] gmc visible size, v[11] bit0 = hi pool exists, v[12] hi pool base.
	if (action == rdna4disp::kActVramStat) {
		uint64_t v[13] = { 0 };
		const uint32_t st = rdna4disp::verb_args_ok(action, argScalar) ? 0u : rdna4disp::kBadArg;
		if (st == 0u) {
			v[5] = bar0Limit; v[6] = bar0Size; v[7] = bar0Phys; v[8] = static_cast<uint64_t>(vramMB) << 20; v[9] = vramBase;
			if (gBringup.dev && gBringup.gmc.vram_alloc.is_inited()) {
				const amdgpu::GMCContext &g = gBringup.gmc;
				v[1] = g.vram_alloc.size(); v[2] = g.vram_alloc.bytes_free();
				if (g.vram_alloc_hi.is_inited()) { v[3] = g.vram_alloc_hi.size(); v[4] = g.vram_alloc_hi.bytes_free(); v[11] = 1ull; v[12] = g.vram_hi_base; }
				v[10] = g.visible_vram_size;
			} else v[0] = 1ull;
			// 0.0.664: the census for the step-B3 choice of ResizeAppleGpuBars: bit1 = the capability was found for BAR0, bits 8..31 = its current size in MiB, bits 32..63 = its supported-sizes mask (bit k = 1 MiB << k)
			v[11] |= (rebarFound ? 2ull : 0ull) | ((rebarCurMB & 0xFFFFFFull) << 8) | ((uint64_t)rebarMask << 32);
			if (bar0FullMode) v[11] |= 4ull;   // bit2 = full BAR mode active (garnet-full-bar=1, BAR0 > 1 GiB)
		}
		RDNA4LOG("accel-experiment: vramstat -> status %u: visible %llu / %llu KiB free, hi %llu / %llu KiB free, vramLimit %#llx, BAR0 mapped %#llx at %#llx", st == rdna4disp::kBadArg ? 2u : (uint32_t)v[0],
		       (unsigned long long)(v[2] >> 10), (unsigned long long)(v[1] >> 10), (unsigned long long)(v[4] >> 10), (unsigned long long)(v[3] >> 10), (unsigned long long)v[5], (unsigned long long)v[6], (unsigned long long)v[7]);
		if (outExtra)
			for (unsigned i = 0; i < 13 && i < kAccelExtraScalars; i++) outExtra[i] = v[i];
		return st == rdna4disp::kBadArg ? kIOReturnBadArgument : kIOReturnSuccess;
	}
	if (action == 83 || action == 84 || action == 85 || action == 86 || action == 87 || action == 89 || action == 90 || action == rdna4disp::kActM6Stat) {
		uint64_t v[13] = { 0 };
		const uint32_t st = rdna4disp_verb(action, argScalar, v, 13);
		RDNA4LOG("accel-experiment: pipe verb %u arg %llu -> status %u", action, (unsigned long long)argScalar, st);
		if (outExtra)
			for (unsigned i = 0; i < 13 && i < kAccelExtraScalars; i++) outExtra[i] = v[i];
		return st == rdna4disp::kBadArg ? kIOReturnBadArgument : kIOReturnSuccess;
	}
	// action 88 (0.0.614, #11 11h.3; boot-arg garnet-metal-disp=1 only): `pipeagdc [0|1]` - the NATIVE AGDC service (amd/native_agdc_pure.h). 1 builds Apple's AppleGraphicsDeviceControl object on a vtable copy
	// whose slots 0 / 1 / 266 are ours (IOPresentment's "Unable to get AGDC information" goes away); 0 only reads the state. Refused with the boot-arg OFF, while a row-120 hold is up, and on any failed check.
	// Served by DisplayPipeGuard.cpp (the AGDC state and the reply fillers live there); the admission and the exemption above are the generic rdna4disp ones.
	// actions 91..93 (0.0.622, multi-monitor stage M1; boot-arg garnet-metal-disp=1 only, like 83..90): the display instruments `ddcread <line> <block>`, `dmubring [page]`, `dispcensus [page]` (dcn/dispread.h).
	// dmubring and dispcensus only READ registers (through the read-only device attach() built); ddcread writes ONLY the DC_I2C engine's registers, through the DCN write allowlist, after the arbitration read says the engine is
	// free (dcn/dispread_flow.h). No command is ever sent to the DMUB and 91..93 read no VRAM through MM_INDEX (94, below, does: READ-ONLY). Served by rdna4dcn (dcn/dcn.cpp).
	// action 94 (0.0.624, stage M1.5; same latch): `region4read <off> [n]` - READ-ONLY dump of the DMUB REGION4 window in VRAM (the first 64 KiB, 64 dwords a call), the window base computed live from the DMCUB registers, the
	// memory read through rdna4_vram_read_mm (the existing MM_INDEX reader). No register and no VRAM byte is written. Served by rdna4dcn::region4Read (dcn/dcn.cpp).
	// action 107 (0.0.661, M6 Stage 1b; the same latch as 83..90): `m6xstat [0|1|2]` - READ-ONLY report of instance 2's scanout (the monitor B: acquired, slots, EARLIEST2, FLIP_PENDING, the counters, the watchdog restores, the live HUBP2 health registers).
	// Writes nothing; with garnet-m6 / garnet-m6flip OFF it reads nothing either (v[0] bits 0..1 name the latches). Served by rdna4dcn::scanXReport (dcn/dcn.cpp).
	if (action == rdna4disp::kActM6XStat) {
		uint64_t v[13] = { 0 };
		if (rdna4m6_latched_on() && rdna4m6flip_latched_on()) (void)rdna4dcn::bind(this);
		const uint32_t st = rdna4disp::verb_args_ok(action, argScalar) ? rdna4dcn::scanXReport(argScalar, v, 13) : rdna4disp::kBadArg;
		RDNA4LOG("accel-experiment: m6xstat page %llu -> status %u (latch words %#llx)", (unsigned long long)argScalar, st, (unsigned long long)v[0]);
		if (outExtra)
			for (unsigned i = 0; i < 13 && i < kAccelExtraScalars; i++) outExtra[i] = v[i];
		return st == rdna4disp::kBadArg ? kIOReturnBadArgument : kIOReturnSuccess;
	}
	if (action == RDNA4DR_ACT_DDCREAD || action == RDNA4DR_ACT_DMUBRING || action == RDNA4DR_ACT_DISPCENSUS || action == RDNA4DR_ACT_REGION4READ) {
		uint64_t v[13] = { 0 };
		uint32_t st;
		if (!rdna4disp::verb_args_ok(action, argScalar)) {
			st = RDNA4DR_BAD_ARG;
			v[0] = st;
		} else if (action == RDNA4DR_ACT_DDCREAD) {
			(void)rdna4dcn::bind(this);
			st = rdna4dcn::ddcRead(argScalar, v, 13);
		} else if (action == RDNA4DR_ACT_DMUBRING) {
			st = rdna4dcn::dmubRing(argScalar, v, 13);
		} else if (action == RDNA4DR_ACT_REGION4READ) {
			st = rdna4dcn::region4Read(argScalar, v, 13);
		} else {
			st = rdna4dcn::dispCensus(argScalar, v, 13);
		}
		RDNA4LOG("accel-experiment: %s %llu -> status %u (%s)", action == RDNA4DR_ACT_DDCREAD ? "ddcread" : action == RDNA4DR_ACT_DMUBRING ? "dmubring" : action == RDNA4DR_ACT_REGION4READ ? "region4read" : "dispcensus",
		       (unsigned long long)argScalar, st, rdna4dr_status_name(st));
		if (outExtra)
			for (unsigned i = 0; i < 13 && i < kAccelExtraScalars; i++) outExtra[i] = v[i];
		return kIOReturnSuccess;
	}
	// actions 95..97 (0.0.625, stages M2 / M3; boot-arg garnet-metal-disp=1 for the admission AND boot-arg garnet-dmubcmd=1 inside the verb): `dmubsend <slotspec>`, `dmubmode <op>`, `dmubctx <ctx> status <v>` (dcn/dmubcmd.h) - the FIRST
	// verbs that send to the display firmware. With garnet-dmubcmd absent each returns RDNA4DR_CMD_OFF and touches nothing. dmubsend needs bind() (only with the latch ON; its one register write, DMCUB_INBOX1_WPTR, goes through the DCN write allowlist).
	if (action == RDNA4DM_ACT_SEND || action == RDNA4DM_ACT_MODE || action == RDNA4DM_ACT_CTX) {
		uint64_t v[13] = { 0 };
		if (action == RDNA4DM_ACT_SEND && rdna4dcn::dmubCmdLatchedOn()) (void)rdna4dcn::bind(this);   // 0.0.626 (F4): the latch FIRST - OFF must touch nothing (bind() allocates a lock, reads registers, sets gDcn.armed)
		const uint32_t st = action == RDNA4DM_ACT_SEND ? rdna4dcn::dmubSend(argScalar, v, 13) : action == RDNA4DM_ACT_MODE ? rdna4dcn::dmubMode(argScalar, v, 13) : rdna4dcn::dmubCtx(argScalar, v, 13);
		RDNA4LOG("accel-experiment: %s %#llx -> status %u (%s)", action == RDNA4DM_ACT_SEND ? "dmubsend" : action == RDNA4DM_ACT_MODE ? "dmubmode" : "dmubctx", (unsigned long long)argScalar, st, rdna4dr_status_name(st));
		if (outExtra)
			for (unsigned i = 0; i < 13 && i < kAccelExtraScalars; i++) outExtra[i] = v[i];
		return kIOReturnSuccess;
	}
	if (action == 88) {
		uint64_t v[13] = { 0 };
		const uint32_t st = rdna4disp::verb_args_ok(action, argScalar) ? rdna4_agdc_native_control(argScalar, v, 13) : (uint32_t)rdna4agdc::kBadArg;
		RDNA4LOG("accel-experiment: pipeagdc %llu -> status %u (%s)", (unsigned long long)argScalar, st, rdna4agdc::status_name(st));
		if (outExtra)
			for (unsigned i = 0; i < 13 && i < kAccelExtraScalars; i++) outExtra[i] = v[i];
		return st == rdna4agdc::kBadArg ? kIOReturnBadArgument : kIOReturnSuccess;
	}
	// action 99 (0.0.633; boot-arg garnet-metal-disp=1 only, like 91): `scdcread <line> <off> [len]` - READ-ONLY SCDC status read from the HDMI sink over the DC_I2C engine, the same engine sequence and containment as ddcread
	// (dcn/dispread_flow.h i2c_xfer; the only bus write is the 1-byte register offset). Served by rdna4dcn::scdcRead (dcn/dcn.cpp).
	if (action == RDNA4DR_ACT_SCDCREAD) {
		uint64_t v[13] = { 0 };
		uint32_t st;
		if (!rdna4disp::verb_args_ok(action, argScalar)) {
			st = RDNA4DR_BAD_ARG;
			v[0] = st;
		} else {
			(void)rdna4dcn::bind(this);
			st = rdna4dcn::scdcRead(argScalar, v, 13);
		}
		RDNA4LOG("accel-experiment: scdcread %#llx -> status %u (%s)", (unsigned long long)argScalar, st, rdna4dr_status_name(st));
		if (outExtra)
			for (unsigned i = 0; i < 13 && i < kAccelExtraScalars; i++) outExtra[i] = v[i];
		return kIOReturnSuccess;
	}
	// action 98 (0.0.631, stage M4d; boot-arg garnet-metal-disp=1 for the admission AND boot-arg garnet-disp2=1 inside the verb): `disp2 timing|connect|off|status` (dcn/disp2.h) - OTG1 / ODM1 / OPP1 / DPG1 and DIG2's
	// front end for a test pattern on the HDMI display PHY C drives (the DMUB's `pclk otg1-on` / `phyc enable` first). With garnet-disp2 absent it returns RDNA4D2_OFF and touches nothing (no bind, no read).
	if (action == RDNA4D2_ACT) {
		uint64_t v[13] = { 0 };
		if (rdna4dcn::disp2LatchedOn()) (void)rdna4dcn::bind(this);   // the latch FIRST - OFF must touch nothing (bind() allocates a lock, reads registers, sets gDcn.armed)
		const uint32_t st = rdna4dcn::disp2(argScalar, v, 13, &gBringup);   // 0.0.635: &gBringup = the VRAM allocator the monitor B plane's two buffers come from (amdgpu::n1c_d2_alloc)
		RDNA4LOG("accel-experiment: disp2 %#llx -> status %u (%s)", (unsigned long long)argScalar, st, rdna4d2_status_name(st));
		if (outExtra)
			for (unsigned i = 0; i < 13 && i < kAccelExtraScalars; i++) outExtra[i] = v[i];
		return kIOReturnSuccess;
	}
	// action 105 (0.0.652, multi-monitor stage M5; boot-arg garnet-metal-disp=1 for the admission AND boot-arg garnet-fb2=1 inside the verb): `fbpublish 2` (the monitor B) / `fbpublish 1` (the monitor A, 0.0.658) - after `disp2 plane N` / `show N` / `fbhold N` (the plane HELD, the pair pinned), build the
	// IMMUTABLE snapshot (the live gates re-read, the monitor B's EDID over DDC line 3, the aperture) and publish GarnetDisplayNub (GarnetDisplayIndex 1) under the GPU's PCI device: the aux kext's GarnetFramebuffer matches it. Refuses with a named status
	// (rdna4fb::Status) and no register read when the plane is not held, a pipe is adopted, a WindowServer native session is open, or a nub already exists; WRITES NO REGISTER but the DC_I2C engine's (the EDID read, as `accel ddcread`).
	// There is NO withdraw in M5: a reboot is the only way back. Served by GarnetDisplayNub.cpp (decisions: amd/native_fb_pure.h).
	if (action == rdna4disp::kActFbPublish) {
		uint64_t v[13] = { 0 };
		const uint32_t st = GarnetDisplayNub::publishVerb(this, argScalar, v, 13);
		RDNA4LOG("accel-experiment: fbpublish %llu -> status %u (%s)", (unsigned long long)argScalar, st, rdna4fb::status_name(st));
		if (outExtra)
			for (unsigned i = 0; i < 13 && i < kAccelExtraScalars; i++) outExtra[i] = v[i];
		return kIOReturnSuccess;
	}
	if (action == 78) {
		uint64_t v[13] = { 0 };
		const uint32_t st = rdna4fbname::control(argScalar, v, 13);
		RDNA4LOG("accel-experiment: fbname %llu -> status %u", (unsigned long long)argScalar, st);
		if (outExtra)
			for (unsigned i = 0; i < 13 && i < kAccelExtraScalars; i++) outExtra[i] = v[i];
		return kIOReturnSuccess;
	}
	// build 0.0.518: `dcnflip 1002..1240` is flip mode's UNARMED A/B test of N = arg - 1000 flips
	// (rdna4_fm_test; PC T-F1), and dcnflip's own pattern flip (1..30) is REFUSED while flip mode (switch 74) is ON: both would
	// program HUBP0. `dcnflip 0` (the restore of dcnflip's own pattern) is unchanged.
	if (action == 76 && rdna4_fm_test_n(argScalar) != 0u) {
		uint64_t v[13] = { 0 };
		const uint32_t st = rdna4_fm_test(rdna4_fm_test_n(argScalar), v, 13);
		RDNA4LOG("accel-experiment: dcnflip %llu (flip mode's A/B test) -> status %u", (unsigned long long)argScalar, st);
		if (outExtra)
			for (unsigned i = 0; i < 13 && i < kAccelExtraScalars; i++) outExtra[i] = v[i];
		return kIOReturnSuccess;
	}
	if (action == 76 && argScalar != 0u && rdna4_fm_on()) {
		RDNA4LOG("accel-experiment: dcnflip %llu REFUSED - flip mode (switch 74) is ON and owns HUBP0's address; `gfxneuter 586` "
		       "turns it OFF (restoring A)", (unsigned long long)argScalar);
		if (outExtra) { outExtra[0] = 0; outExtra[1] = 0x70u; }
		return kIOReturnSuccess;
	}
	if (action == 74 || action == 75 || action == 76 || action == 77) {
		(void)rdna4dcn::bind(this);
		uint64_t v[13] = { 0 };
		const uint32_t st = action == 74 ? rdna4dcn::state(argScalar, v, 13)
		                  : action == 75 ? rdna4dcn::vbl(argScalar, v, 13)
		                  : action == 76 ? rdna4dcn::flip(argScalar, v, 13)
		                                 : rdna4dcn::mode(argScalar, v, 13);
		RDNA4LOG("accel-experiment: %s %llu -> status %u",
		       action == 74 ? "dcnstate" : action == 75 ? "dcnvbl"
		       : action == 76 ? "dcnflip" : "dcnmode",
		       (unsigned long long)argScalar, st);
		if (outExtra)
			for (unsigned i = 0; i < 13 && i < kAccelExtraScalars; i++) outExtra[i] = v[i];
		return kIOReturnSuccess;
	}
	if (action == 79) {
		uint64_t v[13] = { 0 };
		const uint32_t st = rdna4_agdc_hold_control(argScalar, v, 13);
		RDNA4LOG("accel-experiment: agdchold %llu -> status %u (0 ok, 13 above the ceiling)",
		       (unsigned long long)argScalar, st);
		if (outExtra)
			for (unsigned i = 0; i < 13 && i < kAccelExtraScalars; i++) outExtra[i] = v[i];
		return kIOReturnSuccess;
	}
	if (action == 60) {
		uint64_t v[13] = { 0 };
		const uint32_t st = rdna4_scanout_control(argScalar, v, 13);
		RDNA4LOG("accel-experiment: scanout %llu -> status %u", (unsigned long long)argScalar, st);
		if (outExtra)
			for (unsigned i = 0; i < 13 && i < kAccelExtraScalars; i++) outExtra[i] = v[i];
		return kIOReturnSuccess;
	}
	if (action == 82) {
		uint64_t v[13] = { 0 };
		const uint32_t st = rdna4_sdmadcc_control(argScalar, v, 13);
		RDNA4LOG("accel-experiment: sdmadcc %llu -> status %u (0 ok, 1 bad argument, 2 no context, 3 no GC base, "
		       "4 read-back mismatch)", (unsigned long long)argScalar, st);
		if (outExtra)
			for (unsigned i = 0; i < 13 && i < kAccelExtraScalars; i++) outExtra[i] = v[i];
		return kIOReturnSuccess;
	}
	return kIOReturnUnsupported;
}

void GarnetDriver::stop(IOService *provider) {
	garnet_webkit::uninstall();
	if (sleepNotifier) {
		sleepNotifier->remove();
		sleepNotifier = nullptr;
	}
	publishDisplayPeer(rdna4peer::kStateStopping);
	rdna4fbname::restore_on_unload();
	rdna4dcn::primaryVblShutdown();
	rdna4dcn::scanShutdown();
	GarnetDisplayNub::shutdown();
	GarnetMetalNub::shutdown();
	if (waitForDisplayLeasesAndReleasePrimary()) {
		amdgpu::smu_idle_dpm_stop();
		amdgpu::bringup_teardown(gBringup);
		disarmInterrupts();
		if (bar2Map) {
			bar2Map->release();
			bar2Map = nullptr;
			dev.bar2 = nullptr;
			dev.bar2Size = 0;
		}
		unmapVramAperture();
		unmapRegisters();
	}
	publishDisplayPeer(rdna4peer::kStateStopped);
	IOService *service = nullptr;
	if (gBringupLifecycleLock) {
		IOLockLock(gBringupLifecycleLock);
		if (gBringupSelf == this) {
			service = gBringupSelf;
			gBringupSelf = nullptr;
		}
		IOLockUnlock(gBringupLifecycleLock);
	}
	if (service) service->release();
	super::stop(provider);
}

void GarnetDriver::free() {
	// mmIndexLock is a borrowed process-lifetime global (gMmIndexWindowLock);
	// local indexed callers can still be inside it at service stop, so the
	// service never frees it. The kext unload contract retires those threads.
	if (sleepLock) { IOLockFree(sleepLock); sleepLock = nullptr; }
	if (displayPrimaryService) { displayPrimaryService->release(); displayPrimaryService = nullptr; }
	if (onDieDisc) { IOFree(onDieDisc, rdna4::kDiscTmrSize); onDieDisc = nullptr; }
	super::free();
}

void GarnetDriver::logPciSleepState(const char *tag) {
	if (!pciDevice) return;
	const uint16_t vid = pciDevice->configRead16(kIOPCIConfigVendorID);
	const uint16_t did = pciDevice->configRead16(kIOPCIConfigDeviceID);
	const uint16_t cmd = pciDevice->configRead16(kIOPCIConfigCommand);
	const uint16_t sts = pciDevice->configRead16(kIOPCIConfigStatus);
	const uint32_t bar0Lo = pciDevice->configRead32(kIOPCIConfigBaseAddress0);
	const uint32_t bar0Hi = pciDevice->configRead32(kIOPCIConfigBaseAddress1);
	RDNA4LOG("sleep: %s pci=%04x:%04x cmd=0x%04x sts=0x%04x bar0=0x%08x_%08x",
	         tag ? tag : "?", vid, did, cmd, sts, bar0Hi, bar0Lo);
}

IOReturn GarnetDriver::sleepWakeCallback(void *target, void *, UInt32 messageType,
                                         IOService *, void *messageArgument, vm_size_t) {
	auto *self = static_cast<GarnetDriver *>(target);
	if (!self) return kIOReturnSuccess;

	auto ackIfNeeded = [&]() {
		if (messageArgument &&
		    (messageType == kIOMessageCanSystemSleep ||
		     messageType == kIOMessageSystemWillSleep ||
		     messageType == kIOMessageSystemWillPowerOff ||
		     messageType == kIOMessageSystemWillRestart)) {
			(void)acknowledgeSleepWakeNotification(messageArgument);
		}
	};

	rdna4sleep::Message msg {};
	const char *tag = nullptr;
	if (messageType == kIOMessageSystemWillSleep) {
		msg = rdna4sleep::Message::WillSleep;
		tag = "before-sleep";
	} else if (messageType == kIOMessageSystemWillNotSleep) {
		msg = rdna4sleep::Message::WillNotSleep;
	} else if (messageType == kIOMessageSystemWillPowerOn) {
		msg = rdna4sleep::Message::WillPowerOn;
		tag = "waking";
	} else if (messageType == kIOMessageSystemHasPoweredOn) {
		msg = rdna4sleep::Message::HasPoweredOn;
		tag = "woken";
	} else {
		ackIfNeeded();
		return kIOReturnSuccess;
	}

	rdna4sleep::Actions act {};
	uint32_t readBack = 0u;
	if (self->sleepLock) IOLockLock(self->sleepLock);
	const uint64_t cap = self->bar0AccessLimit();
	const uint64_t markOff =
	    (self->vramBase != 0u && self->vramBase <= cap && 4u <= cap - self->vramBase)
	        ? self->vramBase
	        : ((self->bootFbOff != 0u && self->bootFbOff != ~0ULL && cap >= 4u) ? 0u : ~0ULL);
	bool writeOk = false;
	if (msg == rdna4sleep::Message::WillSleep && self->bar0 && markOff != ~0ULL) {
		auto *w = reinterpret_cast<volatile uint32_t *>(self->bar0 + markOff);
		*w = rdna4sleep::kVramSentinel;
		storeFence();
		writeOk = (*w == rdna4sleep::kVramSentinel);
	} else if (msg == rdna4sleep::Message::HasPoweredOn && self->bar0 && markOff != ~0ULL) {
		auto *w = reinterpret_cast<volatile uint32_t *>(self->bar0 + markOff);
		readBack = *w;
	}
	act = self->sleepState.on_message(msg, writeOk, readBack);
	if (act.logPci) self->logPciSleepState(tag);
	if (act.checkVramMark) {
		RDNA4LOG("sleep: VRAM %s through sleep (readback=0x%08x)",
		         readBack == rdna4sleep::kVramSentinel ? "kept its contents" : "lost its contents",
		         readBack);
	}
	if (self->sleepLock) IOLockUnlock(self->sleepLock);

	if (act.quiesceEngines) {
		garnet_primary_fb_notify_sleep(self, true);
	} else if (act.recoverDisplayPads) {
		garnet_primary_fb_notify_wake(self);
	} else if (act.resumeEngines) {
		garnet_primary_fb_notify_sleep(self, false);
	}
	ackIfNeeded();
	return kIOReturnSuccess;
}
