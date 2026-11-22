//
//  GarnetPrimaryFramebuffer.cpp — Built-in IOFramebuffer primary display personality for Garnet/RDNA4.
//
#include "GarnetPrimaryFramebuffer.hpp"

#include "GarnetDriver.hpp"
#include "GarnetDriverInternal.hpp"
#include "amd/amdgpu_asic.h"
#include "amd/amdgpu_smu.h"
#include "amd/native_disp.h"
#include "amd/rdna4_log.h"
#include "amd/smu_dal_pure.h"
#include "dcn/dcn41_modeset.h"
#include "dcn/dispread_flow.h"
#include "dcn41_allow_ranges.h"
#include "dcn41_dmub.h"
#include "dcn41_regs.h"

#include <IOKit/IOLib.h>
#include <IOKit/IOLocks.h>
#include <IOKit/IOMemoryDescriptor.h>
#include <IOKit/IOPlatformExpert.h>
#include <IOKit/IOTimerEventSource.h>
#include <IOKit/IOWorkLoop.h>
#include <IOKit/graphics/IOGraphicsInterfaceTypes.h>
#include <IOKit/graphics/IOGraphicsTypes.h>
#include <IOKit/pci/IOPCIDevice.h>
#include <IOKit/pwr_mgt/IOPM.h>
#include <kern/clock.h>
#include <libkern/OSAtomic.h>
#include <pexpert/pexpert.h>
#include <string.h>

static_assert(sizeof(IOFramebuffer) == 464,
              "IOFramebuffer size must match 25G332 model.json (464)");

OSDefineMetaClassAndStructors(AMDGarnetPrimaryFramebuffer, IOFramebuffer)

namespace {

IOLock *gPrimaryFbLock = nullptr;
AMDGarnetPrimaryFramebuffer *gPrimaryFbInstance = nullptr;

IOLock *primary_fb_lock() {
	IOLock *lk = __atomic_load_n(&gPrimaryFbLock, __ATOMIC_ACQUIRE);
	if (lk) return lk;
	IOLock *fresh = IOLockAlloc();
	if (!fresh) return nullptr;
	if (!OSCompareAndSwapPtr(nullptr, fresh, &gPrimaryFbLock)) {
		IOLockFree(fresh);
	}
	return __atomic_load_n(&gPrimaryFbLock, __ATOMIC_ACQUIRE);
}

AMDGarnetPrimaryFramebuffer *retain_primary_fb() {
	IOLock *lk = primary_fb_lock();
	if (!lk) return nullptr;
	IOLockLock(lk);
	AMDGarnetPrimaryFramebuffer *fb = gPrimaryFbInstance;
	if (fb) fb->retain();
	IOLockUnlock(lk);
	return fb;
}

GarnetDriver *retain_driver() {
	IOService *svc = garnet_driver_service_retained();
	if (!svc) return nullptr;
	GarnetDriver *b = OSDynamicCast(GarnetDriver, svc);
	if (!b) svc->release();
	return b;
}

bool read_console_and_bar0(IOPCIDevice *pci,
                           uint64_t *outBar0Phys,
                           uint64_t *outBar0Size,
                           uint64_t *outConsolePhys,
                           uint64_t *outConsoleLen,
                           PE_Video *outVideo) {
	if (!pci) return false;
	IODeviceMemory *bar0 = pci->getDeviceMemoryWithRegister(kIOPCIConfigBaseAddress0);
	if (!bar0) return false;
	const uint64_t bar0Phys = static_cast<uint64_t>(bar0->getPhysicalAddress());
	const uint64_t bar0Size = static_cast<uint64_t>(bar0->getLength());
	const uint32_t bar0Lo = pci->configRead32(kIOPCIConfigBaseAddress0);
	const uint32_t bar0Hi = ((bar0Lo & 0x6u) == 0x4u) ? pci->configRead32(kIOPCIConfigBaseAddress1) : 0u;
	const uint64_t cfgBar0Phys = rdna4rebar::cfg_bar_addr(bar0Lo, bar0Hi);
	if (!rdna4rebar::bar_addr_agree(bar0Phys, cfgBar0Phys)) {
		RDNA4LOG("primaryfb: refusing BAR0 — IOPCIFamily phys %#llx != config space %#llx",
		         static_cast<unsigned long long>(bar0Phys),
		         static_cast<unsigned long long>(cfgBar0Phys));
		return false;
	}

	PE_Video video {};
	IOPlatformExpert *pe = IOService::getPlatform();
	if (!pe || pe->getConsoleInfo(&video) != kIOReturnSuccess) return false;

	const uint64_t consolePhys = rdna4fbprimary::consolePhysicalAddress(video.v_baseAddr);
	const uint64_t height = static_cast<uint64_t>(video.v_height);
	const uint64_t rowBytes = static_cast<uint64_t>(video.v_rowBytes);
	if (height == 0u || rowBytes == 0u || height > (0xffffffffffffffffull / rowBytes)) return false;
	const uint64_t consoleLen = height * rowBytes;

	if (!rdna4fbprimary::validateApertureRange(bar0Phys, bar0Size, consolePhys, consoleLen)) {
		return false;
	}
	if (outBar0Phys) *outBar0Phys = bar0Phys;
	if (outBar0Size) *outBar0Size = bar0Size;
	if (outConsolePhys) *outConsolePhys = consolePhys;
	if (outConsoleLen) *outConsoleLen = consoleLen;
	if (outVideo) *outVideo = video;
	return true;
}

}  // namespace

void garnet_primary_fb_notify_driver_ready(GarnetDriver *driver) {
	if (!driver) return;
	AMDGarnetPrimaryFramebuffer *fb = retain_primary_fb();
	if (!fb) return;
	fb->onDriverReady(driver);
	fb->release();
}

void garnet_primary_fb_notify_wake(GarnetDriver *driver) {
	if (!driver) return;
	AMDGarnetPrimaryFramebuffer *fb = retain_primary_fb();
	if (!fb) return;
	fb->onSystemWake(driver);
	fb->release();
}

IOMemoryDescriptor *rdna4_scanout_descriptor(bool viaFramebuffer) {
	if (viaFramebuffer) {
		AMDGarnetPrimaryFramebuffer *fb = retain_primary_fb();
		if (!fb) return nullptr;
		IODeviceMemory *mem = fb->getVRAMRange();
		fb->release();
		return mem;
	}
	GarnetDriver *driver = retain_driver();
	if (!driver) return nullptr;
	rdna4peer::Snapshot snap {};
	const bool ok = driver->snapshotDisplayState(&snap);
	driver->release();
	if (!ok || snap.consolePhysBase == 0u || snap.console.length == 0u) return nullptr;
	return IODeviceMemory::withRange(static_cast<IOPhysicalAddress>(snap.consolePhysBase),
	                                 static_cast<IOPhysicalLength>(snap.console.length));
}

bool AMDGarnetPrimaryFramebuffer::captureConsoleAndBar0(IOPCIDevice *pci) {
	PE_Video video {};
	uint64_t bar0Phys = 0u, bar0Size = 0u, consolePhys = 0u, consoleLen = 0u;
	if (!read_console_and_bar0(pci, &bar0Phys, &bar0Size, &consolePhys, &consoleLen, &video)) {
		return false;
	}
	fBar0Phys = bar0Phys;
	fBar0Size = bar0Size;
	fConsolePhys = consolePhys;
	fConsoleLen = consoleLen;

	const uint32_t w = static_cast<uint32_t>(video.v_width);
	const uint32_t h = static_cast<uint32_t>(video.v_height);
	const uint32_t rb = static_cast<uint32_t>(video.v_rowBytes);
	rebuildModeTable(w, h, rb);
	fCurrentMode = static_cast<IODisplayModeID>(fModeTable.activeModeId);
	fCurrentDepth = 0;
	return true;
}

namespace {

struct MmioAccess {
	volatile uint32_t *rmmio;
	size_t rmmioDwords;
	uint32_t seg1Base;
	uint32_t seg2Base;
	IOMemoryMap *tempMap;
};

bool acquire_mmio(IOPCIDevice *pci, GarnetDriver *driver, MmioAccess *out) {
	if (!out) return false;
	*out = MmioAccess {};
	out->seg1Base = 0x00c0u;
	out->seg2Base = rdna4aux::kDefaultDmuSeg2Base;
	if (driver) {
		const amdgpu::DeviceContext *dc = driver->deviceContext();
		if (dc && dc->rmmio && dc->rmmioSize >= 0x40000u) {
			out->rmmio = dc->rmmio;
			out->rmmioDwords = dc->rmmioSize / 4u;
		}
		IpDiscovery::IpEntry e {};
		if (driver->ipDisc().isValid() && driver->ipDisc().findIp(IpDiscovery::HwDmu, 0, e) && e.numBases >= 3u) {
			out->seg1Base = e.bases[1];
			out->seg2Base = e.bases[2];
		}
	}
	if (!out->rmmio && pci) {
		IODeviceMemory *bar5 = pci->getDeviceMemoryWithRegister(kIOPCIConfigBaseAddress5);
		if (!bar5) return false;
		IOMemoryMap *m = bar5->map();
		if (!m) return false;
		out->tempMap = m;
		out->rmmio = reinterpret_cast<volatile uint32_t *>(m->getVirtualAddress());
		out->rmmioDwords = m->getLength() / 4u;
	}
	return out->rmmio != nullptr && out->rmmioDwords >= 0x10000u;
}

void release_mmio(MmioAccess &m) {
	if (m.tempMap) {
		m.tempMap->release();
		m.tempMap = nullptr;
	}
	m.rmmio = nullptr;
	m.rmmioDwords = 0u;
}

bool dcn41_abs_allowed(uint32_t absDw) {
	for (uint32_t i = 0u; i < DCN41_HARD_DENY_COUNT; ++i) {
		if (absDw >= dcn41_hard_deny[i].abs_lo && absDw <= dcn41_hard_deny[i].abs_hi) return false;
	}
	for (uint32_t i = 0u; i < DCN41_DENY_BLOCK_COUNT; ++i) {
		if (absDw >= dcn41_deny_blocks[i].abs_lo && absDw <= dcn41_deny_blocks[i].abs_hi) return false;
	}
	for (uint32_t i = 0u; i < DCN41_ALLOW_RANGE_COUNT; ++i) {
		if (absDw >= dcn41_allow_ranges[i].abs_lo && absDw <= dcn41_allow_ranges[i].abs_hi) return true;
	}
	return false;
}

uint32_t aux_mmio_read(void *cookie, uint32_t seg2Off) {
	const auto *c = static_cast<const MmioAccess *>(cookie);
	if (!c || !c->rmmio || rdna4modeset::is_read_hazard_seg(2u, seg2Off)) return 0xffffffffu;
	const uint64_t dw = static_cast<uint64_t>(c->seg2Base) + seg2Off;
	if (dw >= c->rmmioDwords) return 0xffffffffu;
	return c->rmmio[dw];
}

bool aux_mmio_write(void *cookie, uint32_t seg2Off, uint32_t value) {
	const auto *c = static_cast<const MmioAccess *>(cookie);
	if (!c || !c->rmmio) return false;
	// Strictly allow writes ONLY inside the DCN41 DP_AUX0..3 register window (seg2 + 0x16b2..0x1716).
	if (seg2Off < rdna4aux::kAuxBase0 || seg2Off > (rdna4aux::kAuxBase0 + 4u * rdna4aux::kAuxStride - 1u))
		return false;
	const uint64_t dw = static_cast<uint64_t>(c->seg2Base) + seg2Off;
	if (dw >= c->rmmioDwords) return false;
	// Never read back AUX_SW_DATA after writing: it is an auto-incrementing FIFO port.
	c->rmmio[dw] = value;
	return true;
}

uint64_t aux_now_us(void *) {
	uint64_t t = 0u, ns = 0u;
	clock_get_uptime(&t);
	absolutetime_to_nanoseconds(t, &ns);
	return ns / 1000ULL;
}

void aux_delay_us(void *, uint32_t us) {
	if (us >= 1000u) IOSleep(us / 1000u);
	else IODelay(us);
}

void ddcpad_sleep_ms(void *, uint32_t ms) {
	if (ms) IOSleep(ms);
}

// HDMI DDC I2C fallback environment for rdna4dr::ddc_read (dispread_flow.h).
struct HdmiDdcEnv {
	const MmioAccess *m;
	bool ref;
	uint32_t rd(uint32_t baseIdx, uint32_t off) const {
		if (!m || !m->rmmio || rdna4modeset::is_read_hazard_seg(baseIdx, off)) return 0xffffffffu;
		const uint64_t base = (baseIdx == 1u) ? m->seg1Base : m->seg2Base;
		const uint64_t dw = base + off;
		return (dw < m->rmmioDwords) ? m->rmmio[dw] : 0xffffffffu;
	}
	void wr2(uint32_t off, uint32_t value) {
		if (!m || !m->rmmio) { ref = true; return; }
		const uint32_t absDw = m->seg2Base + off;
		if (absDw >= m->rmmioDwords || !dcn41_abs_allowed(absDw)) { ref = true; return; }
		m->rmmio[absDw] = value;
	}
	bool refused() const { return ref; }
	void delay_us(uint32_t us) const { aux_delay_us(nullptr, us); }
};

__attribute__((noinline))
bool read_hdmi_edid_i2c(const MmioAccess &m, uint32_t hpdHint, uint8_t *outBuf, size_t *outLen,
                        int32_t exactLine = -1) {
	if (!outBuf || !outLen) return false;
	*outLen = 0u;
	// Try exactLine for passive DP++ adapters on AUX pins, or HDMI DDC lines 2/3 (+ hpdHint).
	const uint32_t exactArr[4] = { static_cast<uint32_t>(exactLine >= 0 ? exactLine : 0), 0u, 0u, 0u };
	const uint32_t defaultArr[4] = { (hpdHint < 4u ? hpdHint : 2u), 2u, 3u, 0u };
	const uint32_t *lines = (exactLine >= 0 && exactLine < 4) ? exactArr : defaultArr;
	const uint32_t lineCount = (exactLine >= 0 && exactLine < 4) ? 1u : 4u;
	for (uint32_t li = 0u; li < lineCount; ++li) {
		const uint32_t line = lines[li];
		if (exactLine < 0 && line != 2u && line != 3u) continue;
		HdmiDdcEnv env { &m, false };
		const rdna4dr::DdcResult b0 = rdna4dr::ddc_read(env, line, 0u);
		if (b0.status != RDNA4DR_OK || !rdna4edidmodes::baseBlockValid(b0.data)) continue;
		memcpy(outBuf, b0.data, rdna4edidmodes::BlockSize);
		size_t totalBlocks = 1u;
		size_t declared = outBuf[126];
		if (declared > rdna4edidmodes::MaxBlocks - 1u) declared = rdna4edidmodes::MaxBlocks - 1u;
		for (size_t b = 1u; b <= declared; ++b) {
			const rdna4dr::DdcResult bx = rdna4dr::ddc_read(env, line, static_cast<uint32_t>(b));
			if (bx.status != RDNA4DR_OK || !rdna4edidmodes::blockChecksumOk(bx.data)) break;
			if (b >= 2u && rdna4edidmodes::baseBlockValid(bx.data) &&
			    memcmp(bx.data, outBuf, rdna4edidmodes::BlockSize) == 0) break;
			memcpy(outBuf + b * rdna4edidmodes::BlockSize, bx.data, rdna4edidmodes::BlockSize);
			totalBlocks = b + 1u;
		}
		*outLen = totalBlocks * rdna4edidmodes::BlockSize;
		return true;
	}
	return false;
}

// ---- SMU DAL mailbox helpers (disjoint from PPSMC; serialized via shared SmuSeq lock) ----
bool dal_send_msg(const MmioAccess &m, uint32_t msg, uint32_t param, uint32_t *outArg) {
	if (!m.rmmio || rdna4dal::kRegMsg >= m.rmmioDwords ||
	    rdna4dal::kRegArg >= m.rmmioDwords || rdna4dal::kRegResp >= m.rmmioDwords)
		return false;
	amdgpu::SmuSeq seq;
	if (!seq.held) return false;
	const uint32_t pre = m.rmmio[rdna4dal::kRegResp];
	if (pre == 0u || pre == 0xffffffffu) return false;
	m.rmmio[rdna4dal::kRegResp] = 0u;
	m.rmmio[rdna4dal::kRegArg] = param;
	m.rmmio[rdna4dal::kRegMsg] = msg;
	(void)m.rmmio[rdna4dal::kRegMsg];
	for (uint32_t i = 0u; i < 500u; ++i) {
		const uint32_t resp = m.rmmio[rdna4dal::kRegResp];
		if (resp != 0u) {
			if (outArg) *outArg = m.rmmio[rdna4dal::kRegArg];
			return resp == rdna4dal::kRespOk;
		}
		IOSleep(1u);
	}
	return false;
}

uint32_t dal_query_clk_max_khz(const MmioAccess &m, uint32_t clkId) {
	uint32_t arg = 0u;
	if (!dal_send_msg(m, rdna4dal::kMsgGetDpmFreqByIndex, (clkId << 16) | rdna4dal::kIndexAll, &arg))
		return 0u;
	const uint32_t count = rdna4dal::level_count(arg);
	if (count == 0u || count > rdna4dal::kDpmMaxLevels) return 0u;
	uint32_t maxMhz = 0u;
	for (uint32_t idx = 0u; idx < count; ++idx) {
		uint32_t val = 0u;
		if (dal_send_msg(m, rdna4dal::kMsgGetDpmFreqByIndex, (clkId << 16) | idx, &val)) {
			const uint32_t mhz = rdna4dal::level_mhz(val);
			if (mhz > maxMhz) maxMhz = mhz;
		}
	}
	return maxMhz * 1000u;
}

bool dal_raise_clocks(const MmioAccess &m, uint32_t dispClkKhz, uint32_t dppClkKhz,
                      uint32_t maxDispKhz, uint32_t maxDppKhz) {
	const uint32_t dispMhz = (dispClkKhz + 999u) / 1000u;
	const uint32_t dppMhz = (dppClkKhz + 999u) / 1000u;
	if (maxDispKhz > 0u && dispMhz * 1000u > maxDispKhz) return false;
	if (maxDppKhz > 0u && dppMhz * 1000u > maxDppKhz) return false;

	// Raise DISPCLK first, then DPPCLK (Linux dcn401_clk_mgr order).
	if (!dal_send_msg(m, rdna4dal::kMsgSetHardMinByFreq,
	                  rdna4dal::param_hardmin(rdna4dal::kClkDispclk, dispMhz), nullptr))
		return false;
	if (!dal_send_msg(m, rdna4dal::kMsgSetHardMinByFreq,
	                  rdna4dal::param_hardmin(rdna4dal::kClkDppclk, dppMhz), nullptr))
		return false;
	const uint32_t wantMask = (1u << rdna4dal::kClkDispclk) | (1u << rdna4dal::kClkDppclk);
	for (uint32_t i = 0u; i < 100u; ++i) {
		uint32_t st = 0u;
		if (dal_send_msg(m, rdna4dal::kMsgReturnHardMinStatus, 0u, &st) &&
		    (st & wantMask) == wantMask)
			break;
		IOSleep(2u);
	}
	if (m.rmmio && rdna4dal::kRegDfs1 < m.rmmioDwords) {
		const uint32_t pll = m.rmmio[rdna4dal::kRegPllReq];
		const uint32_t d0 = m.rmmio[rdna4dal::kRegDfs0];
		const uint32_t d1 = m.rmmio[rdna4dal::kRegDfs1];
		const uint32_t dent = m.rmmio[rdna4dal::kRegDentist];
		const uint32_t vco = rdna4dal::vco_khz(pll);
		const uint32_t curDisp = rdna4dal::dfs_did_ok(d0) ? rdna4dal::did_khz(vco, d0) : 0u;
		const uint32_t curDpp  = rdna4dal::dfs_did_ok(d1) ? rdna4dal::did_khz(vco, d1) : 0u;
		RDNA4LOG("primaryfb: SMU clocks req DISP=%uMHz DPP=%uMHz -> live DISP=%ukHz DPP=%ukHz (dent=0x%08x d0=0x%x d1=0x%x)",
		         dispMhz, dppMhz, curDisp, curDpp, dent, d0, d1);
	}
	return true;
}

// ---- DMUB inbox1 command submission for DP PHY transmitter control ----
bool dmub_submit_one(const MmioAccess &m, const struct dcn41_dmub_cmd &cmd) {
	if (!m.rmmio) return false;
	// Exact DCN 4.1 DMCUB offsets from dcn41_regs.h (matches dcn41_dmub.c::dcn41_dmub_probe).
	const uint32_t rCntl   = m.seg2Base + DCN41_DMCUB_CNTL;
	const uint32_t rCntl2  = m.seg2Base + DCN41_DMCUB_CNTL2;
	const uint32_t rScr0   = m.seg2Base + DCN41_DMCUB_SCRATCH0;
	const uint32_t rIbBase = m.seg2Base + DCN41_DMCUB_INBOX1_BASE_ADDRESS;
	const uint32_t rIbSize = m.seg2Base + DCN41_DMCUB_INBOX1_SIZE;
	const uint32_t rIbWptr = m.seg2Base + DCN41_DMCUB_INBOX1_WPTR;
	const uint32_t rIbRptr = m.seg2Base + DCN41_DMCUB_INBOX1_RPTR;
	const uint32_t rCw4Off = m.seg2Base + DCN41_DMCUB_REGION3_CW4_OFFSET;
	const uint32_t rCw4Hi  = m.seg2Base + DCN41_DMCUB_REGION3_CW4_OFFSET_HIGH;
	const uint32_t rCw4Bas = m.seg2Base + DCN41_DMCUB_REGION3_CW4_BASE_ADDRESS;
	const uint32_t rCw4Top = m.seg2Base + DCN41_DMCUB_REGION3_CW4_TOP_ADDRESS;
	const uint32_t rR4Off  = m.seg2Base + DCN41_DMCUB_REGION4_OFFSET;
	const uint32_t rR4Hi   = m.seg2Base + DCN41_DMCUB_REGION4_OFFSET_HIGH;
	const uint32_t rR4Top  = m.seg2Base + DCN41_DMCUB_REGION4_TOP_ADDRESS;
	const uint32_t rFbBase = m.seg2Base + DCN41_DCN_VM_FB_LOCATION_BASE;
	if (rFbBase >= m.rmmioDwords) return false;

	const uint32_t cntl  = m.rmmio[rCntl];
	const uint32_t cntl2 = m.rmmio[rCntl2];
	const uint32_t scr0  = m.rmmio[rScr0];
	const uint32_t ibBase = m.rmmio[rIbBase];
	const uint32_t ibSize = m.rmmio[rIbSize];
	const uint32_t wptr   = m.rmmio[rIbWptr];
	const uint32_t rptr   = m.rmmio[rIbRptr];

	if ((cntl & DCN41_DMCUB_CNTL__DMCUB_ENABLE_MASK) == 0u) return false;
	if ((cntl2 & DCN41_DMCUB_CNTL2__DMCUB_SOFT_RESET_MASK) != 0u) return false;
	if ((scr0 & DCN41_DMUB_BOOT_MAILBOX_RDY) == 0u) return false;
	if (ibSize < DCN41_DMUB_CMD_SIZE || (ibSize % DCN41_DMUB_CMD_SIZE) != 0u || ibSize > 0x10000u) return false;
	if (rptr != wptr || wptr >= ibSize || (wptr % DCN41_DMUB_CMD_SIZE) != 0u) return false;

	// Locate inbox1 ring in VRAM via CW4 (cached window) or REGION4 (uncached window), matching dcn41_dmub.c.
	uint64_t ringMc = 0ULL;
	const uint32_t cw4Top = m.rmmio[rCw4Top];
	const uint32_t cw4Base = m.rmmio[rCw4Bas] & DCN41_DMCUB_REGION3_CW4_BASE_ADDRESS__DMCUB_REGION3_CW4_BASE_ADDRESS_MASK;
	const uint32_t cw4TopAddr = cw4Top & DCN41_DMCUB_REGION3_CW4_TOP_ADDRESS__DMCUB_REGION3_CW4_TOP_ADDRESS_MASK;
	const uint32_t inboxM = ibBase & DCN41_DMCUB_REGION3_CW4_BASE_ADDRESS__DMCUB_REGION3_CW4_BASE_ADDRESS_MASK;
	const uint32_t r4Top = m.rmmio[rR4Top];
	const uint32_t r4TopAddr = r4Top & DCN41_DMCUB_REGION4_TOP_ADDRESS__DMCUB_REGION4_TOP_ADDRESS_MASK;

	if ((cw4Top & DCN41_DMCUB_REGION3_CW4_TOP_ADDRESS__DMCUB_REGION3_CW4_ENABLE_MASK) != 0u &&
	    ibBase != DCN41_DMUB_REGION4_INBOX1_BASE && inboxM >= cw4Base &&
	    static_cast<uint64_t>(inboxM) + ibSize - 1ULL <= cw4TopAddr) {
		const uint64_t cw4Mc = (static_cast<uint64_t>(m.rmmio[rCw4Hi] & 0xffffu) << 32) | m.rmmio[rCw4Off];
		ringMc = cw4Mc + (inboxM - cw4Base);
	} else if (ibBase == DCN41_DMUB_REGION4_INBOX1_BASE &&
	           (r4Top & DCN41_DMCUB_REGION4_TOP_ADDRESS__DMCUB_REGION4_ENABLE_MASK) != 0u &&
	           (ibSize - 1u) <= r4TopAddr) {
		ringMc = (static_cast<uint64_t>(m.rmmio[rR4Hi] & 0xffffu) << 32) | m.rmmio[rR4Off];
	} else {
		return false;
	}

	const uint64_t fbMc = static_cast<uint64_t>(m.rmmio[rFbBase] & DCN41_DCN_VM_FB_LOCATION_BASE__FB_BASE_MASK) << 24;
	if (ringMc < fbMc) return false;
	const uint64_t slotVramOff = (ringMc - fbMc) + wptr;

	uint32_t dwords[16] {};
	memcpy(dwords, &cmd, sizeof(dwords));
	if (!rdna4_vram_write_mm(slotVramOff, dwords, 16u)) return false;

	const uint32_t nextWptr = (wptr + DCN41_DMUB_CMD_SIZE) % ibSize;
	if (!dcn41_abs_allowed(rIbWptr)) return false;
	m.rmmio[rIbWptr] = nextWptr;
	(void)m.rmmio[rIbWptr];

	for (uint32_t i = 0u; i < 50000u; ++i) {
		if (m.rmmio[rIbRptr] == nextWptr) return true;
		IODelay(10u);
	}
	return false;
}

uint32_t mmio_rd_seg(const MmioAccess *m, uint32_t segIdx, uint32_t off) {
	if (!m || !m->rmmio || rdna4modeset::is_read_hazard_seg(segIdx, off)) return 0xffffffffu;
	const uint32_t base = (segIdx == 1u) ? m->seg1Base : m->seg2Base;
	const uint64_t dw = static_cast<uint64_t>(base) + off;
	return (dw < m->rmmioDwords) ? m->rmmio[dw] : 0xffffffffu;
}

bool mmio_wr_seg(const MmioAccess *m, uint32_t segIdx, uint32_t off, uint32_t val) {
	if (!m || !m->rmmio) return false;
	const uint32_t base = (segIdx == 1u) ? m->seg1Base : m->seg2Base;
	const uint32_t absDw = base + off;
	if (absDw >= m->rmmioDwords || !dcn41_abs_allowed(absDw)) return false;
	// Never read back FIFO data ports (e.g. VPG_GENERIC_PACKET_DATA) after writing.
	m->rmmio[absDw] = val;
	return true;
}

struct ModesetCtx {
	MmioAccess mmio;
	rdna4aux::ActiveOutput out;
	uint32_t maxDispClkKhz;
	uint32_t maxDppClkKhz;
};

uint32_t modeset_rd_seg(void *cookie, uint32_t segIdx, uint32_t off) {
	const auto *c = static_cast<const ModesetCtx *>(cookie);
	return c ? mmio_rd_seg(&c->mmio, segIdx, off) : 0xffffffffu;
}

bool modeset_wr_seg(void *cookie, uint32_t segIdx, uint32_t off, uint32_t val) {
	const auto *c = static_cast<const ModesetCtx *>(cookie);
	return c ? mmio_wr_seg(&c->mmio, segIdx, off, val) : false;
}

uint32_t cursor_rd_seg2(void *cookie, uint32_t seg2Off) {
	return mmio_rd_seg(static_cast<const MmioAccess *>(cookie), 2u, seg2Off);
}

bool cursor_wr_seg2(void *cookie, uint32_t seg2Off, uint32_t val) {
	return mmio_wr_seg(static_cast<const MmioAccess *>(cookie), 2u, seg2Off, val);
}

bool modeset_raise_clocks(void *cookie, uint32_t dispClkKhz, uint32_t dppClkKhz) {
	const auto *c = static_cast<const ModesetCtx *>(cookie);
	if (!c) return false;
	return dal_raise_clocks(c->mmio, dispClkKhz, dppClkKhz, c->maxDispClkKhz, c->maxDppClkKhz);
}

bool modeset_phy_control(void *cookie, uint32_t action, uint8_t rateCode, uint8_t lanes,
                         uint8_t swing, uint8_t preEmph) {
	const auto *c = static_cast<const ModesetCtx *>(cookie);
	if (!c) return false;
	// Exact hardware-verified DCN 4.1 DMUB DIG1_TRANSMITTER_CONTROL ABI (matches dpphy.cpp / atomfirmware.h):
	//   Disable  = 0 (connObjId = 0x13 DP, lanenum = 0, symclk = 0)
	//   Enable   = 1 (digMode = 0 DP, lanenum = lanes, symclk = rate*27000/10, connObjId = 0)
	//   SetDrive = 11 (mode_laneset = (swing&3)|((preEmph&3)<<3), lanenum = lanes, connObjId = 0x13)
	const uint32_t symClkKhz = rdna4aux::linkCodeToRateKbps(rateCode) / 10u; // e.g. 540000 or 810000
	uint8_t atomAction = 1u;
	uint8_t modeLaneSet = 0u;
	uint8_t laneCount = lanes;
	uint32_t pclkKhz = symClkKhz;
	uint8_t connObjId = 0u;
	if (action == 2u) {
		atomAction = 0u; // ATOM_TRANSMITTER_ACTION_DISABLE
		laneCount = 0u;
		pclkKhz = 0u;
		connObjId = 0x13u;
	} else if (action == 3u) {
		atomAction = 11u; // TRANSMITTER_CONTROL_SET_VOLTAGE_AND_PREEMPASIS
		modeLaneSet = static_cast<uint8_t>((swing & 3u) | ((preEmph & 3u) << 3));
		connObjId = 0x13u;
	}
	const uint8_t phyId = static_cast<uint8_t>((c->out.link < 4u) ? c->out.link : c->out.dig);
	struct dcn41_dmub_cmd cmd {};
	dcn41_dmub_build_transmitter_control(
	    &cmd,
	    phyId,
	    atomAction,
	    modeLaneSet,
	    laneCount,
	    pclkKhz,
	    static_cast<uint8_t>(c->out.hpd + 1u),         // 1-based HPD select
	    0u,                                            // digfe_sel = 0 (matches Linux/dpphy.cpp)
	    connObjId,
	    0u, 0u);
	return dmub_submit_one(c->mmio, cmd);
}

__attribute__((noinline))
void log_edid_and_dpcd(const rdna4aux::ActiveOutput &out,
                       const uint8_t *edid, size_t edidLen, bool extWrapped,
                       const rdna4aux::DpcdCaps &dpcd) {
	char monName[14] {};
	rdna4aux::extractMonitorName(edid, edidLen, monName);
	const uint16_t mfg = (edidLen >= 10u) ? static_cast<uint16_t>((edid[8] << 8) | edid[9]) : 0u;
	const uint16_t prod = (edidLen >= 12u) ? static_cast<uint16_t>(edid[10] | (edid[11] << 8)) : 0u;
	const uint8_t declaredExt = (edidLen >= 128u) ? edid[126] : 0u;
	RDNA4LOG("primaryfb: live EDID ok: '%s' mfg=0x%04x prod=0x%04x len=%zuB (declaredExt=%u wrapped=%u) via OTG%u->DIG%u(mode=%u)->AUX%u/HPD%u",
	         monName[0] ? monName : "unknown", mfg, prod, edidLen, declaredExt, extWrapped ? 1u : 0u,
	         out.otg, out.dig, out.digMode, out.aux, out.hpd);

	// Log all parsed EDID/CTA/DisplayID timings (uncapped) so we see every native/high-refresh mode.
	// Heap-allocated to keep kernel stack frames well under the 4 KiB __chkstk_darwin threshold.
	auto *allModes = static_cast<rdna4edidmodes::ModeList *>(IOMalloc(sizeof(rdna4edidmodes::ModeList)));
	if (allModes) {
		bzero(allModes, sizeof(*allModes));
		const size_t nAll = rdna4edidmodes::build(edid, edidLen, 7680u, 4320u, 7680u * 4u, *allModes);
		RDNA4LOG("primaryfb: EDID limits valid=%u refresh=%u..%u mHz maxH=%u Hz maxPclk=%llu Hz totalModes=%zu",
		         allModes->limits.valid ? 1u : 0u, allModes->limits.minRefreshMilliHz,
		         allModes->limits.maxRefreshMilliHz, allModes->limits.maxHHz,
		         static_cast<unsigned long long>(allModes->limits.maxPixelClockHz), nAll);
		for (size_t i = 0u; i < nAll && i < 16u; ++i) {
			const auto &t = allModes->modes[i].timing;
			RDNA4LOG("primaryfb: EDID mode[%zu] src=%u %ux%u@%u.%03uHz pclk=%lluHz htot=%llu vtot=%llu",
			         i, t.source, t.width, t.height, t.refreshMilliHz() / 1000u, t.refreshMilliHz() % 1000u,
			         static_cast<unsigned long long>(t.pixelClockHz),
			         static_cast<unsigned long long>(t.hTotal()), static_cast<unsigned long long>(t.vTotal()));
		}
		IOFree(allModes, sizeof(rdna4edidmodes::ModeList));
	}
	for (size_t blk = 2u; blk * 128u < edidLen; ++blk) {
		const uint8_t *b = edid + blk * 128u;
		RDNA4LOG("primaryfb: EDID block %zu tag=0x%02x ver=0x%02x len=%u: %02x%02x%02x%02x %02x%02x%02x%02x %02x%02x%02x%02x %02x%02x%02x%02x",
		         blk, b[0], b[1], b[2],
		         b[0], b[1], b[2], b[3], b[4], b[5], b[6], b[7],
		         b[8], b[9], b[10], b[11], b[12], b[13], b[14], b[15]);
	}
	if (dpcd.valid) {
		RDNA4LOG("primaryfb: DPCD rev=0x%02x maxRate=0x%02x maxLanes=%u tps3=%u tps4=%u fecCap=0x%02x | curRate=0x%02x curLanes=%u curDsc=%u lanes=%02x%02x%02x",
		         dpcd.dpcdRev, dpcd.maxLinkRate, dpcd.maxLaneCount,
		         dpcd.tps3Supported ? 1u : 0u, dpcd.tps4Supported ? 1u : 0u, dpcd.fecCap,
		         dpcd.curLinkBwSet, dpcd.curLaneCountSet, dpcd.curDscEnable,
		         dpcd.laneStatus[0], dpcd.laneStatus[1], dpcd.laneStatus[2]);
		RDNA4LOG("primaryfb: DPCD DSC=%u rev=0x%02x slice1=0x%02x slice2=0x%02x lineDepth=0x%02x maxW=0x%02x | extMaxRate=0x%02x extMaxLanes=%u UHBR valid=%u rates=0x%02x",
		         dpcd.dscSupported ? 1u : 0u, dpcd.dscRev, dpcd.dscSliceCap1, dpcd.dscSliceCap2,
		         dpcd.dscLineBufDepth, dpcd.dscMaxSliceWidth, dpcd.extMaxLinkRate, dpcd.extMaxLaneCount,
		         dpcd.uhbrValid ? 1u : 0u, dpcd.uhbrRates);
	}
}

}  // namespace

__attribute__((noinline))
void AMDGarnetPrimaryFramebuffer::queryClockCaps(IOPCIDevice *pci, GarnetDriver *driver) {
	MmioAccess m {};
	if (!acquire_mmio(pci, driver, &m)) return;
	const uint32_t dispMax = dal_query_clk_max_khz(m, rdna4dal::kClkDispclk);
	const uint32_t dppMax = dal_query_clk_max_khz(m, rdna4dal::kClkDppclk);
	release_mmio(m);
	if (dispMax > 0u) fMaxDispClkKhz = dispMax;
	if (dppMax > 0u) fMaxDppClkKhz = dppMax;
	if (fMaxDispClkKhz || fMaxDppClkKhz) {
		RDNA4LOG("primaryfb: SMU DAL clock caps DISPCLK=%u kHz DPPCLK=%u kHz",
		         fMaxDispClkKhz, fMaxDppClkKhz);
	}
}

__attribute__((noinline))
void AMDGarnetPrimaryFramebuffer::rebuildModeTable(uint32_t w, uint32_t h, uint32_t rb) {
	const bool canDpSwitch = fActiveOutput.found && fActiveOutput.isDp &&
	                         fActiveOutput.auxValid && fDpcdCaps.valid;
	uint64_t maxPixClkHz = 600000000ULL;
	if (canDpSwitch) {
		maxPixClkHz = rdna4modeset::maxSupportedPixelClockHz(
		    fDpcdCaps, fMaxDispClkKhz, fMaxDppClkKhz, /*allowRetrain=*/true, /*allowDsc=*/true);
	}
	// Keep hardware modes at native console geometry so GarnetMetal accelerated scanout stays 1:1
	// while macOS CoreDisplay/WindowServer automatically generates all HiDPI scaled resolution tiers
	// across every exposed refresh rate.
	rdna4fbprimary::buildModeTable(fEdidLen ? fEdid : nullptr, fEdidLen, w, h, rb, &fModeTable,
	                               maxPixClkHz, /*sameSizeOnly=*/true,
	                               /*synthesizeRefreshSteps=*/canDpSwitch);
	RDNA4LOG("primaryfb: mode table built: %zu modes (maxPixClk=%llu Hz, canDpSwitch=%u, activeId=%u)",
	         fModeTable.list.count, static_cast<unsigned long long>(maxPixClkHz),
	         canDpSwitch ? 1u : 0u, fModeTable.activeModeId);
	for (size_t i = 0u; i < fModeTable.list.count && i < 16u; ++i) {
		const auto &m = fModeTable.list.modes[i];
		RDNA4LOG("primaryfb:   modeId=%u %ux%u@%u.%03uHz pclk=%lluHz src=%u",
		         m.modeId, m.timing.width, m.timing.height,
		         m.timing.refreshMilliHz() / 1000u, m.timing.refreshMilliHz() % 1000u,
		         static_cast<unsigned long long>(m.timing.pixelClockHz), m.timing.source);
	}
}

__attribute__((noinline))
bool AMDGarnetPrimaryFramebuffer::readLiveHardwareEdid(IOPCIDevice *pci, GarnetDriver *driver) {
	if (!pci) return false;
	MmioAccess m {};
	if (!acquire_mmio(pci, driver, &m)) return false;

	const rdna4aux::Io io { &m, aux_mmio_read, aux_mmio_write, aux_now_us, aux_delay_us };
	const rdna4ddcpad::DdcPadHw padHw { &m, cursor_rd_seg2, cursor_wr_seg2, ddcpad_sleep_ms };
	const rdna4aux::ActiveOutput out = rdna4aux::detectActiveOutput(io);
	fActiveOutput = out;
	bool gotEdid = false;
	auto *tmpEdid = static_cast<uint8_t *>(IOMalloc(rdna4edidmodes::BlockSize * rdna4edidmodes::MaxBlocks));
	if (tmpEdid) {
		bzero(tmpEdid, rdna4edidmodes::BlockSize * rdna4edidmodes::MaxBlocks);
		if (out.found && out.isDp && out.auxValid) {
			size_t tmpLen = 0u;
			bool extWrapped = false;
			rdna4aux::AuxDiag diag {};
			uint32_t rc = rdna4aux::kIoError;
			for (uint32_t tryIdx = 0u; tryIdx < 3u; ++tryIdx) {
				rc = rdna4aux::readEdidOverAux(io, out.aux, out.hpd, tmpEdid,
				                               rdna4edidmodes::MaxBlocks, &tmpLen, &extWrapped, &diag);
				if (rc == rdna4aux::kOk && tmpLen >= rdna4edidmodes::BlockSize) break;
				IOSleep(5u);
			}
			if ((rc != rdna4aux::kOk || tmpLen < rdna4edidmodes::BlockSize) &&
			    out.aux < rdna4ddcpad::kMaxDdcLines &&
			    rdna4ddcpad::set_ddc_pad_mode(padHw, static_cast<uint8_t>(out.aux), false)) {
				rc = rdna4aux::readEdidOverAux(io, out.aux, out.hpd, tmpEdid,
				                               rdna4edidmodes::MaxBlocks, &tmpLen, &extWrapped, &diag);
			}
			rdna4aux::DpcdCaps dpcd {};
			(void)rdna4aux::readDpcdCaps(io, out.aux, out.hpd, &dpcd);
			fDpcdCaps = dpcd;
			if (dpcd.valid) {
				fActiveLinkRate = dpcd.curLinkBwSet;
				fActiveDscOn = (dpcd.curDscEnable & 1u) != 0u;
			}
			if (rc == rdna4aux::kOk && tmpLen >= rdna4edidmodes::BlockSize) {
				memcpy(fEdid, tmpEdid, tmpLen);
				fEdidLen = tmpLen;
				gotEdid = true;
				log_edid_and_dpcd(out, fEdid, fEdidLen, extWrapped, fDpcdCaps);
			} else if (out.aux < rdna4ddcpad::kMaxDdcLines &&
			           rdna4ddcpad::set_ddc_pad_mode(padHw, static_cast<uint8_t>(out.aux), true) &&
			           read_hdmi_edid_i2c(m, out.hpd, tmpEdid, &tmpLen, static_cast<int32_t>(out.aux)) &&
			           tmpLen >= rdna4edidmodes::BlockSize) {
				memcpy(fEdid, tmpEdid, tmpLen);
				fEdidLen = tmpLen;
				gotEdid = true;
				RDNA4LOG("primaryfb: passive DP++ I2C EDID succeeded on DDC%u (%zu B)", out.aux, tmpLen);
			} else {
				if (out.aux < rdna4ddcpad::kMaxDdcLines) {
					(void)rdna4ddcpad::set_ddc_pad_mode(padHw, static_cast<uint8_t>(out.aux), false);
				}
				RDNA4LOG("primaryfb: live DP AUX%u/HPD%u EDID failed rc=%u (ctl=0x%08x arb=0x%08x sw=0x%08x sts=0x%08x hpd=0x%08x OTG%u DIG%u seg2=0x%x)",
				         out.aux, out.hpd, rc, diag.ctl, diag.arb, diag.sw, diag.sts, diag.hpd,
				         out.otg, out.dig, m.seg2Base);
			}
		} else if (out.found && !out.isDp) {
			if (out.hpd < rdna4ddcpad::kMaxDdcLines) {
				(void)rdna4ddcpad::set_ddc_pad_mode(padHw, static_cast<uint8_t>(out.hpd), true);
			}
			size_t tmpLen = 0u;
			if (read_hdmi_edid_i2c(m, out.hpd, tmpEdid, &tmpLen) && tmpLen >= rdna4edidmodes::BlockSize) {
				memcpy(fEdid, tmpEdid, tmpLen);
				fEdidLen = tmpLen;
				gotEdid = true;
				log_edid_and_dpcd(out, fEdid, fEdidLen, false, fDpcdCaps);
			} else {
				RDNA4LOG("primaryfb: live HDMI DDC EDID read failed (OTG%u DIG%u mode=%u HPD%u)",
				         out.otg, out.dig, out.digMode, out.hpd);
			}
		} else {
			RDNA4LOG("primaryfb: active output detect found=%u isDp=%u auxValid=%u (OTG%u DIG%u mode=%u HPD%u seg2=0x%x)",
			         out.found ? 1u : 0u, out.isDp ? 1u : 0u, out.auxValid ? 1u : 0u,
			         out.otg, out.dig, out.digMode, out.hpd, m.seg2Base);
		}
		IOFree(tmpEdid, rdna4edidmodes::BlockSize * rdna4edidmodes::MaxBlocks);
	}
	release_mmio(m);
	return gotEdid;
}

__attribute__((noinline))
bool AMDGarnetPrimaryFramebuffer::applyHardwareMode(const rdna4edidmodes::DetailedTiming &target) {
	if (!fPci || !fActiveOutput.found || !fActiveOutput.isDp || !fActiveOutput.auxValid || !fDpcdCaps.valid)
		return false;
	GarnetDriver *driver = retain_driver();
	if (!driver) {
		RDNA4LOG("primaryfb: setDisplayMode deferred/refused: GarnetDriver not ready yet");
		return false;
	}
	if (fMaxDispClkKhz == 0u || fMaxDppClkKhz == 0u) {
		queryClockCaps(fPci, driver);
	}
	if (fMaxDispClkKhz == 0u || fMaxDppClkKhz == 0u) {
		RDNA4LOG("primaryfb: setDisplayMode refused: SMU DAL clock caps unavailable");
		driver->release();
		return false;
	}
	auto *ctx = static_cast<ModesetCtx *>(IOMalloc(sizeof(ModesetCtx)));
	if (!ctx) {
		driver->release();
		return false;
	}
	bzero(ctx, sizeof(*ctx));
	if (!acquire_mmio(fPci, driver, &ctx->mmio)) {
		driver->release();
		IOFree(ctx, sizeof(ModesetCtx));
		return false;
	}
	ctx->out = fActiveOutput;
	ctx->maxDispClkKhz = fMaxDispClkKhz;
	ctx->maxDppClkKhz = fMaxDppClkKhz;

	rdna4modeset::Hw hw {};
	hw.cookie = ctx;
	hw.rdSeg = modeset_rd_seg;
	hw.wrSeg = modeset_wr_seg;
	hw.raiseClocks = modeset_raise_clocks;
	hw.phyControl = modeset_phy_control;
	hw.nowUs = aux_now_us;
	hw.delayUs = aux_delay_us;
	hw.auxIo = rdna4aux::Io { &ctx->mmio, aux_mmio_read, aux_mmio_write, aux_now_us, aux_delay_us };

	const rdna4modeset::ApplyResult r = rdna4modeset::applyMode(
	    hw, fActiveOutput, fDpcdCaps, target, fMaxDispClkKhz, fMaxDppClkKhz,
	    /*allowRetrain=*/true, /*allowDsc=*/true);

	if (r.status == rdna4modeset::kOk) {
		(void)rdna4aux::readDpcdCaps(hw.auxIo, fActiveOutput.aux, fActiveOutput.hpd, &fDpcdCaps);
		fActiveLinkRate = fDpcdCaps.curLinkBwSet;
		fActiveDscOn = (fDpcdCaps.curDscEnable & 1u) != 0u;
		const uint32_t db = fActiveOutput.dig * rdna4modeset::kSeg2DigStride;
		const uint32_t link = (fActiveOutput.link < 4u) ? fActiveOutput.link : fActiveOutput.dig;
		const uint32_t be = link * rdna4modeset::kSeg2DigStride;
		RDNA4LOG("primaryfb: setDisplayMode HW OK: %ux%u@%u.%03uHz (measured=%u.%03uHz, tier=%u, rate=0x%02x, dsc=%u, link=%u, lanes=%02x%02x%02x)",
		         target.width, target.height, target.refreshMilliHz() / 1000u, target.refreshMilliHz() % 1000u,
		         r.measuredMilliHz / 1000u, r.measuredMilliHz % 1000u, static_cast<unsigned>(r.tierUsed),
		         fActiveLinkRate, fActiveDscOn ? 1u : 0u, link,
		         fDpcdCaps.laneStatus[0], fDpcdCaps.laneStatus[1], fDpcdCaps.laneStatus[2]);
		RDNA4LOG("primaryfb:   regs stream=0x%08x vidM=0x%06x vidN=0x%06x dphy=0x%08x pfmt=0x%08x vbid=0x%08x gsp11=0x%08x",
		         modeset_rd_seg(ctx, 2u, rdna4modeset::kSeg2Dp0VidStreamCntl + db),
		         modeset_rd_seg(ctx, 2u, rdna4modeset::kSeg2Dp0VidM + db) & 0xffffffu,
		         modeset_rd_seg(ctx, 2u, rdna4modeset::kSeg2Dp0VidN + db) & 0xffffffu,
		         modeset_rd_seg(ctx, 2u, rdna4modeset::kSeg2Dp0DphyCntl + be),
		         modeset_rd_seg(ctx, 2u, rdna4modeset::kSeg2Dp0PixelFormat + db),
		         modeset_rd_seg(ctx, 2u, rdna4modeset::kSeg2Dp0MsaVbidMisc + db),
		         modeset_rd_seg(ctx, 2u, rdna4modeset::kSeg2Dp0Gsp11Cntl + db));
		const uint32_t dscBase = fActiveOutput.otg * rdna4modeset::kSeg2DscStride;
		const uint32_t odmB = fActiveOutput.otg * rdna4modeset::kSeg2OdmStride;
		const uint32_t hb = fActiveOutput.otg * rdna4modeset::kSeg2HubpStride;
		RDNA4LOG("primaryfb:   dsc pg=0x%08x vpgPwr=0x%08x cfg0=0x%08x st=0x%08x optcFmt=0x%08x hubpB0=0x%08x fifo=0x%08x vtim=0x%08x",
		         modeset_rd_seg(ctx, 2u, rdna4modeset::kSeg2Domain16PgStat + fActiveOutput.otg * 2u),
		         modeset_rd_seg(ctx, 2u, rdna4modeset::kSeg2Vpg0MemPwr + db),
		         modeset_rd_seg(ctx, 2u, rdna4modeset::kSeg2Dscc0Cfg0 + dscBase),
		         modeset_rd_seg(ctx, 2u, rdna4modeset::kSeg2Dscc0Status + dscBase),
		         modeset_rd_seg(ctx, 2u, rdna4modeset::kSeg2OptcDataFmt + odmB),
		         modeset_rd_seg(ctx, 2u, rdna4modeset::kSeg2HubpReq0Blank0 + hb),
		         modeset_rd_seg(ctx, 2u, rdna4modeset::kSeg2Dig0FifoCtrl0 + db),
		         modeset_rd_seg(ctx, 2u, rdna4modeset::kSeg2Dp0VidTiming + db));
	} else {
		RDNA4LOG("primaryfb: setDisplayMode HW FAILED status=%u (target=%ux%u@%u.%03uHz, measured=%u.%03uHz, tier=%u, rolledBack=%u)",
		         r.status, target.width, target.height,
		         target.refreshMilliHz() / 1000u, target.refreshMilliHz() % 1000u,
		         r.measuredMilliHz / 1000u, r.measuredMilliHz % 1000u,
		         static_cast<unsigned>(r.tierUsed), r.rolledBack ? 1u : 0u);
	}

	release_mmio(ctx->mmio);
	if (driver) driver->release();
	IOFree(ctx, sizeof(ModesetCtx));
	return r.status == rdna4modeset::kOk;
}

void AMDGarnetPrimaryFramebuffer::readProviderEdid(IOService *provider) {
	fEdidLen = 0u;
	bzero(fEdid, sizeof(fEdid));
	if (!provider) return;

	static const char *const kEdidKeys[] = { "DDC,EDID", "ATY,EDID" };
	for (const char *key : kEdidKeys) {
		OSObject *obj = provider->copyProperty(key);
		if (!obj) continue;
		OSData *data = OSDynamicCast(OSData, obj);
		if (data && data->getBytesNoCopy() && data->getLength() >= rdna4edidmodes::BlockSize) {
			size_t copyLen = data->getLength();
			if (copyLen > sizeof(fEdid)) copyLen = sizeof(fEdid);
			copyLen -= (copyLen % rdna4edidmodes::BlockSize);
			if (copyLen >= rdna4edidmodes::BlockSize) {
				memcpy(fEdid, data->getBytesNoCopy(), copyLen);
				fEdidLen = copyLen;
				obj->release();
				return;
			}
		}
		obj->release();
	}
}

void AMDGarnetPrimaryFramebuffer::free() {
	teardownVblTimer();
	teardownCursorSlot();
	if (fVblLock) {
		IOLockFree(fVblLock);
		fVblLock = nullptr;
	}
	if (fCursorLock) {
		IOLockFree(fCursorLock);
		fCursorLock = nullptr;
	}
	super::free();
}

IOService *AMDGarnetPrimaryFramebuffer::probe(IOService *provider, SInt32 *score) {
	IOPCIDevice *pci = OSDynamicCast(IOPCIDevice, provider);
	if (!pci) return nullptr;
	const uint16_t vendor = pci->configRead16(kIOPCIConfigVendorID);
	const uint16_t device = pci->configRead16(kIOPCIConfigDeviceID);
	if (amdgpu::asic_from_pci(vendor, device) == nullptr) {
		return nullptr;
	}
	if (!read_console_and_bar0(pci, nullptr, nullptr, nullptr, nullptr, nullptr)) {
		return nullptr;
	}
	if (score) *score = 200000;
	return this;
}

bool AMDGarnetPrimaryFramebuffer::start(IOService *provider) {
	IOPCIDevice *pci = OSDynamicCast(IOPCIDevice, provider);
	if (!pci) return false;
	fPci = pci;
	fEdidLen = 0u;
	bzero(fEdid, sizeof(fEdid));
	{
		GarnetDriver *earlyDriver = retain_driver();
		const bool liveOk = readLiveHardwareEdid(pci, earlyDriver);
		if (earlyDriver) {
			queryClockCaps(pci, earlyDriver);
			earlyDriver->release();
		}
		if (!liveOk) readProviderEdid(provider);
	}
	if (fEdidLen >= rdna4edidmodes::BlockSize) {
		if (OSData *edidProp = OSData::withBytes(fEdid, static_cast<unsigned int>(fEdidLen))) {
			setProperty("DDC,EDID", edidProp);
			edidProp->release();
		}
	}
	if (!captureConsoleAndBar0(pci)) return false;

	fPowerState = 2u;
	fConnPower = 1u;
	fPmRegistered = false;
	fPeerRegistered = false;
	fPeerGeneration = 0u;
	fStopping = false;
	fCursorW = 0u;
	fCursorH = 0u;
	fCursorHotX = 0u;
	fCursorHotY = 0u;
	fCursorX = 0;
	fCursorY = 0;
	fCursorVisible = false;
	fCursorHwAvailable = false;
	fCursorNeedsProgram = false;
	if (!fVblLock) {
		fVblLock = IOLockAlloc();
		if (!fVblLock) return false;
	}
	if (!fCursorLock) {
		fCursorLock = IOLockAlloc();
		if (!fCursorLock) return false;
	}
	{
		GarnetDriver *cursorDriver = retain_driver();
		initCursorSlot(cursorDriver);
		if (cursorDriver) cursorDriver->release();
	}

	if (!super::start(provider)) return false;

	IOLock *lk = primary_fb_lock();
	if (lk) {
		IOLockLock(lk);
		gPrimaryFbInstance = this;
		IOLockUnlock(lk);
	}

	GarnetDriver *driver = retain_driver();
	if (driver) {
		onDriverReady(driver);
		driver->release();
	}

	RDNA4LOG("primaryfb: started console %ux%u (rowBytes=%u, modes=%u, edid=%zuB, phys=%#llx)",
	         fModeTable.consoleWidth, fModeTable.consoleHeight, fModeTable.consoleRowBytes,
	         static_cast<unsigned>(fModeTable.list.count), fEdidLen,
	         static_cast<unsigned long long>(fConsolePhys));
	return true;
}

void AMDGarnetPrimaryFramebuffer::onDriverReady(GarnetDriver *driver) {
	if (!driver) return;
	if (fEdidLen < rdna4edidmodes::BlockSize && fPci && readLiveHardwareEdid(fPci, driver)) {
		if (OSData *edidProp = OSData::withBytes(fEdid, static_cast<unsigned int>(fEdidLen))) {
			setProperty("DDC,EDID", edidProp);
			edidProp->release();
		}
		rebuildModeTable(fModeTable.consoleWidth, fModeTable.consoleHeight, fModeTable.consoleRowBytes);
		fCurrentMode = static_cast<IODisplayModeID>(fModeTable.activeModeId);
	}
	if ((fMaxDispClkKhz == 0u || fMaxDppClkKhz == 0u) && fPci) {
		queryClockCaps(fPci, driver);
	}
	if (!fCursorHwAvailable) {
		initCursorSlot(driver);
	}
	// Never call IOFramebuffer::handleEvent(kIOFBNotifyDisplayModeDidChange, nullptr) from GarnetDriver's
	// thread: handleEvent() is only defined for power-state notifications under IOFramebuffer's own lock.
	if (fPeerRegistered) return;
	rdna4peer::Snapshot snap {};
	if (!driver->snapshotDisplayState(&snap)) return;
	if (!rdna4fbprimary::primaryMatchesSnapshot(fBar0Phys, fBar0Size, fConsolePhys, fConsoleLen, snap)) {
		RDNA4LOG("primaryfb: refusing peer registration — local BAR0/console [%#llx+%#llx, console %#llx+%#llx] disagrees with snapshot [%#llx+%#llx, limit %#llx, console %#llx+%#llx]",
		         static_cast<unsigned long long>(fBar0Phys), static_cast<unsigned long long>(fBar0Size),
		         static_cast<unsigned long long>(fConsolePhys), static_cast<unsigned long long>(fConsoleLen),
		         static_cast<unsigned long long>(snap.bar0Phys), static_cast<unsigned long long>(snap.bar0Size),
		         static_cast<unsigned long long>(snap.vramLimit),
		         static_cast<unsigned long long>(snap.consolePhysBase), static_cast<unsigned long long>(snap.console.length));
		return;
	}

	rdna4peer::PrimaryView candidate =
	    rdna4fbprimary::makePrimaryView(snap, getRegistryEntryID());

	rdna4peer::Snapshot outSnap {};
	rdna4peer::PrimaryView stored {};
	const uint32_t rc = driver->registerPrimaryFramebuffer(this, candidate, &outSnap, &stored);
	if (rc == rdna4peer::kOk) {
		fPeerRegistered = true;
		fPeerGeneration = stored.generation;
		RDNA4LOG("primaryfb: registered canonical peer (gen=%llu, entry=%#llx)",
		         static_cast<unsigned long long>(fPeerGeneration),
		         static_cast<unsigned long long>(candidate.fbEntryID));
	}
}

void AMDGarnetPrimaryFramebuffer::stop(IOService *provider) {
	if (fVblLock) IOLockLock(fVblLock);
	fStopping = true;
	fVblEnabled = false;
	fVblProc = nullptr;
	fVblTarget = nullptr;
	fVblRef = nullptr;
	if (fVblLock) IOLockUnlock(fVblLock);
	teardownVblTimer();

	{
		GarnetDriver *cursorDriver = retain_driver();
		if (fCursorLock) IOLockLock(fCursorLock);
		if (fCursorHwAvailable) {
			disableCursorLocked(cursorDriver);
		}
		fCursorVisible = false;
		fCursorHwAvailable = false;
		if (fCursorLock) IOLockUnlock(fCursorLock);
		if (cursorDriver) cursorDriver->release();
		teardownCursorSlot();
	}

	IOLock *lk = primary_fb_lock();
	if (lk) {
		IOLockLock(lk);
		if (gPrimaryFbInstance == this) gPrimaryFbInstance = nullptr;
		IOLockUnlock(lk);
	}

	if (fPeerRegistered) {
		GarnetDriver *driver = retain_driver();
		if (driver) {
			driver->unregisterPrimaryFramebuffer(this, fPeerGeneration);
			driver->release();
		}
		fPeerRegistered = false;
		fPeerGeneration = 0u;
	}

	fPci = nullptr;
	super::stop(provider);
}

uint32_t AMDGarnetPrimaryFramebuffer::vblPeriodUs() const {
	rdna4fbprimary::ModeInfoSummary summary {};
	if (rdna4fbprimary::modeInfoForId(fModeTable, fCurrentMode, &summary) &&
	    summary.refreshFixed16_16 > 0u) {
		const uint64_t us = (1000000ULL << 16) / static_cast<uint64_t>(summary.refreshFixed16_16);
		if (us < 2000ULL) return 2000u;
		if (us > 33333ULL) return 33333u;
		return static_cast<uint32_t>(us);
	}
	return 16667u;
}

void AMDGarnetPrimaryFramebuffer::onVblTimer(OSObject *owner, IOTimerEventSource *sender) {
	AMDGarnetPrimaryFramebuffer *self = OSDynamicCast(AMDGarnetPrimaryFramebuffer, owner);
	if (!self || !sender) return;

	IOFBInterruptProc proc = nullptr;
	OSObject *target = nullptr;
	void *ref = nullptr;
	bool enabled = false;

	if (self->fVblLock) IOLockLock(self->fVblLock);
	enabled = self->fVblEnabled && !self->fStopping;
	proc = self->fVblProc;
	target = self->fVblTarget;
	ref = self->fVblRef;
	if (self->fVblLock) IOLockUnlock(self->fVblLock);

	if (!enabled || !proc) return;
	proc(target, ref);

	if (self->fVblLock) IOLockLock(self->fVblLock);
	enabled = self->fVblEnabled && !self->fStopping;
	if (self->fVblLock) IOLockUnlock(self->fVblLock);

	if (enabled) sender->setTimeoutUS(self->vblPeriodUs());
}

bool AMDGarnetPrimaryFramebuffer::ensureVblTimer() {
	if (fVblTimer) return true;
	IOWorkLoop *wl = getWorkLoop();
	if (!wl) return false;
	IOTimerEventSource *timer =
	    IOTimerEventSource::timerEventSource(this, &AMDGarnetPrimaryFramebuffer::onVblTimer);
	if (!timer) return false;
	if (wl->addEventSource(timer) != kIOReturnSuccess) {
		timer->release();
		return false;
	}
	timer->disable();
	fVblWorkLoop = wl;
	fVblTimer = timer;
	return true;
}

void AMDGarnetPrimaryFramebuffer::teardownVblTimer() {
	if (fVblTimer) {
		fVblTimer->disable();
		fVblTimer->cancelTimeout();
		if (fVblWorkLoop) fVblWorkLoop->removeEventSource(fVblTimer);
		fVblTimer->release();
		fVblTimer = nullptr;
	}
	fVblWorkLoop = nullptr;
}

void AMDGarnetPrimaryFramebuffer::initForPM() {
	if (fPmRegistered) return;
	fPmRegistered = true;
	static IOPMPowerState powerStates[3] = {
		{ 1, kIOPMPreventSystemSleep,                  0,            0,            0, 0, 0, 0, 0, 0, 0, 0 },
		{ 1, kIOPMPreventSystemSleep,                  0,            kIOPMPowerOn, 0, 0, 0, 0, 0, 0, 0, 0 },
		{ 1, kIOPMDeviceUsable | kIOPMPreventSystemSleep, kIOPMPowerOn, kIOPMPowerOn, 0, 0, 0, 0, 0, 0, 0, 0 },
	};
	registerPowerDriver(this, powerStates, 3);
	temporaryPowerClampOn();
	changePowerStateTo(2);
	IOService *p = getProvider();
	if (p) p->setProperty("IOPMIsPowerManaged", true);
}

IOReturn AMDGarnetPrimaryFramebuffer::enableController() {
	initForPM();
	ensureVblTimer();
	return kIOReturnSuccess;
}

bool AMDGarnetPrimaryFramebuffer::isConsoleDevice() {
	return fConsolePhys != 0u && fConsoleLen != 0u;
}

IODeviceMemory *AMDGarnetPrimaryFramebuffer::getApertureRange(IOPixelAperture aperture) {
	if (aperture != kIOFBSystemAperture || !fConsolePhys || !fConsoleLen) return nullptr;
	return IODeviceMemory::withRange(static_cast<IOPhysicalAddress>(fConsolePhys),
	                                 static_cast<IOPhysicalLength>(fConsoleLen));
}

IODeviceMemory *AMDGarnetPrimaryFramebuffer::getVRAMRange() {
	if (!fConsolePhys || !fConsoleLen) return nullptr;
	return IODeviceMemory::withRange(static_cast<IOPhysicalAddress>(fConsolePhys),
	                                 static_cast<IOPhysicalLength>(fConsoleLen));
}

const char *AMDGarnetPrimaryFramebuffer::getPixelFormats() {
	static const char kFormats[] = IO32BitDirectPixels "\0";
	return kFormats;
}

IOItemCount AMDGarnetPrimaryFramebuffer::getDisplayModeCount() {
	return static_cast<IOItemCount>(fModeTable.list.count);
}

IOReturn AMDGarnetPrimaryFramebuffer::getDisplayModes(IODisplayModeID *allDisplayModes) {
	if (!allDisplayModes) return kIOReturnBadArgument;
	for (size_t i = 0u; i < fModeTable.list.count; ++i) {
		allDisplayModes[i] = static_cast<IODisplayModeID>(fModeTable.list.modes[i].modeId);
	}
	return kIOReturnSuccess;
}

IOReturn AMDGarnetPrimaryFramebuffer::getInformationForDisplayMode(IODisplayModeID displayMode,
                                                               IODisplayModeInformation *info) {
	if (!info) return kIOReturnBadArgument;
	rdna4fbprimary::ModeInfoSummary summary {};
	if (!rdna4fbprimary::modeInfoForId(fModeTable, displayMode, &summary)) {
		return kIOReturnBadArgument;
	}
	bzero(info, sizeof(*info));
	info->nominalWidth = summary.nominalWidth;
	info->nominalHeight = summary.nominalHeight;
	info->refreshRate = summary.refreshFixed16_16;
	info->maxDepthIndex = static_cast<IOIndex>(summary.maxDepthIndex);
	info->flags = kDisplayModeValidFlag | kDisplayModeSafeFlag |
	              kDisplayModeValidForHiResFlag | kDisplayModeAcceleratorBackedFlag;
	if ((summary.flags & 0x4u) != 0u) {
		info->flags |= kDisplayModeDefaultFlag;
	}
	if ((summary.flags & kDisplayModeNativeFlag) != 0u) {
		info->flags |= kDisplayModeNativeFlag;
	}
	return kIOReturnSuccess;
}

UInt64 AMDGarnetPrimaryFramebuffer::getPixelFormatsForDisplayMode(IODisplayModeID, IOIndex) {
	return 0;
}

IOReturn AMDGarnetPrimaryFramebuffer::getPixelInformation(IODisplayModeID displayMode,
                                                      IOIndex depth,
                                                      IOPixelAperture aperture,
                                                      IOPixelInformation *pixelInfo) {
	if (!pixelInfo || depth < 0) return kIOReturnBadArgument;
	rdna4fbprimary::PixelSpec px {};
	if (!rdna4fbprimary::pixelSpecForMode(fModeTable, displayMode, depth,
	                                      aperture == kIOFBSystemAperture, &px)) {
		return kIOReturnBadArgument;
	}
	bzero(pixelInfo, sizeof(*pixelInfo));
	pixelInfo->bytesPerRow = px.bytesPerRow;
	pixelInfo->bytesPerPlane = px.bytesPerPlane;
	pixelInfo->bitsPerPixel = px.bitsPerPixel;
	pixelInfo->pixelType = kIORGBDirectPixels;
	pixelInfo->componentCount = px.componentCount;
	pixelInfo->bitsPerComponent = px.bitsPerComponent;
	for (size_t i = 0u; i < 3u; ++i) pixelInfo->componentMasks[i] = px.componentMasks[i];
	memcpy(pixelInfo->pixelFormat, px.pixelFormat, sizeof(px.pixelFormat));
	pixelInfo->activeWidth = px.activeWidth;
	pixelInfo->activeHeight = px.activeHeight;
	return kIOReturnSuccess;
}

IOReturn AMDGarnetPrimaryFramebuffer::getCurrentDisplayMode(IODisplayModeID *displayMode,
                                                        IOIndex *depth) {
	if (displayMode) *displayMode = fCurrentMode;
	if (depth) *depth = fCurrentDepth;
	return kIOReturnSuccess;
}

IOReturn AMDGarnetPrimaryFramebuffer::setDisplayMode(IODisplayModeID displayMode, IOIndex depth) {
	if (depth != 0) return kIOReturnUnsupported;
	rdna4fbprimary::PixelSpec px {};
	if (!rdna4fbprimary::pixelSpecForMode(fModeTable, displayMode, depth, true, &px)) {
		return kIOReturnUnsupported;
	}
	if (static_cast<uint64_t>(px.bytesPerPlane) > fConsoleLen) {
		return kIOReturnUnsupported;
	}
	if (displayMode == fCurrentMode && depth == fCurrentDepth) {
		return kIOReturnSuccess;
	}
	rdna4edidmodes::DetailedTiming target {};
	if (!rdna4fbprimary::modeForId(fModeTable, displayMode, &target)) {
		return kIOReturnUnsupported;
	}
	RDNA4LOG("primaryfb: setDisplayMode request id=%d -> %d (%ux%u@%u.%03uHz pclk=%lluHz)",
	         static_cast<int>(fCurrentMode), static_cast<int>(displayMode),
	         target.width, target.height,
	         target.refreshMilliHz() / 1000u, target.refreshMilliHz() % 1000u,
	         static_cast<unsigned long long>(target.pixelClockHz));
	if (!applyHardwareMode(target)) {
		return kIOReturnError;
	}
	fCurrentMode = displayMode;
	fCurrentDepth = depth;
	rdna4disp_on_mode_change();
	if (fVblTimer) {
		bool enabled = false;
		if (fVblLock) IOLockLock(fVblLock);
		enabled = fVblEnabled && !fStopping;
		if (fVblLock) IOLockUnlock(fVblLock);
		if (enabled) fVblTimer->setTimeoutUS(vblPeriodUs());
	}
	return kIOReturnSuccess;
}

IOReturn AMDGarnetPrimaryFramebuffer::getStartupDisplayMode(IODisplayModeID *displayMode,
                                                        IOIndex *depth) {
	if (displayMode) *displayMode = static_cast<IODisplayModeID>(fModeTable.activeModeId);
	if (depth) *depth = 0;
	return kIOReturnSuccess;
}

IOReturn AMDGarnetPrimaryFramebuffer::setAttribute(IOSelect attribute, uintptr_t value) {
	if (attribute == kIOPowerAttribute) {
		const uint32_t next = static_cast<uint32_t>(value);
		if (next < 2u && fPowerState >= 2u) {
			handleEvent(kIOFBNotifyWillPowerOff, nullptr);
		} else if (next >= 2u && fPowerState < 2u) {
			handleEvent(kIOFBNotifyDidPowerOn, nullptr);
		}
		fPowerState = next;
		return kIOReturnSuccess;
	}
	return super::setAttribute(attribute, value);
}

IOReturn AMDGarnetPrimaryFramebuffer::getAttribute(IOSelect attribute, uintptr_t *value) {
	if (attribute == kIOPowerAttribute) {
		if (value) *value = fPowerState;
		return kIOReturnSuccess;
	}
	if (attribute == kIOHardwareCursorAttribute) {
		if (value) *value = fCursorHwAvailable ? 1u : 0u;
		return kIOReturnSuccess;
	}
	return super::getAttribute(attribute, value);
}

IOReturn AMDGarnetPrimaryFramebuffer::getTimingInfoForDisplayMode(IODisplayModeID displayMode,
                                                              IOTimingInformation *info) {
	if (!info) return kIOReturnBadArgument;
	rdna4fbprimary::TimingSpec ts {};
	if (!rdna4fbprimary::timingSpecForMode(fModeTable, displayMode, &ts)) {
		return kIOReturnBadArgument;
	}
	bzero(info, sizeof(*info));
	info->appleTimingID = kIOTimingIDInvalid;
	info->flags = kIODetailedTimingValid;
	info->detailedInfo.v2.pixelClock = ts.pixelClockHz;
	info->detailedInfo.v2.minPixelClock = ts.pixelClockHz;
	info->detailedInfo.v2.maxPixelClock = ts.pixelClockHz;
	info->detailedInfo.v2.horizontalActive = ts.hActive;
	info->detailedInfo.v2.horizontalBlanking = ts.hBlank;
	info->detailedInfo.v2.horizontalSyncOffset = ts.hSyncOffset;
	info->detailedInfo.v2.horizontalSyncPulseWidth = ts.hSyncWidth;
	info->detailedInfo.v2.verticalActive = ts.vActive;
	info->detailedInfo.v2.verticalBlanking = ts.vBlank;
	info->detailedInfo.v2.verticalSyncOffset = ts.vSyncOffset;
	info->detailedInfo.v2.verticalSyncPulseWidth = ts.vSyncWidth;
	info->detailedInfo.v2.horizontalSyncConfig = ts.hPositive ? kIOSyncPositivePolarity : 0u;
	info->detailedInfo.v2.verticalSyncConfig = ts.vPositive ? kIOSyncPositivePolarity : 0u;
	return kIOReturnSuccess;
}

IOItemCount AMDGarnetPrimaryFramebuffer::getConnectionCount() {
	return 1;
}

IOReturn AMDGarnetPrimaryFramebuffer::setAttributeForConnection(IOIndex connectIndex,
                                                            IOSelect attribute,
                                                            uintptr_t value) {
	if (connectIndex != 0) return kIOReturnBadArgument;
	if (attribute == kConnectionPower) {
		fConnPower = static_cast<uint32_t>(value);
		return kIOReturnSuccess;
	}
	return super::setAttributeForConnection(connectIndex, attribute, value);
}

IOReturn AMDGarnetPrimaryFramebuffer::getAttributeForConnection(IOIndex connectIndex,
                                                            IOSelect attribute,
                                                            uintptr_t *value) {
	if (connectIndex != 0) return kIOReturnBadArgument;
	switch (attribute) {
		case kConnectionFlags:
			// External desktop monitor (leave kIOConnectionBuiltIn clear).
			if (value) *value = 0;
			return kIOReturnSuccess;
		case kConnectionCheckEnable:
			if (value) *value = 1;
			return kIOReturnSuccess;
		case kConnectionEnable:
			if (value) *value = 1;
			return kIOReturnSuccess;
		case kConnectionSyncEnable:
			if (value) *value = 0xff;
			return kIOReturnSuccess;
		case kConnectionPower:
			if (value) *value = fConnPower;
			return kIOReturnSuccess;
		case kConnectionSupportsHLDDCSense:
			return hasDDCConnect(connectIndex) ? kIOReturnSuccess : kIOReturnUnsupported;
		case kConnectionSupportsAppleSense:
		case kConnectionSupportsLLDDCSense:
			return kIOReturnUnsupported;
		default:
			return super::getAttributeForConnection(connectIndex, attribute, value);
	}
}

bool AMDGarnetPrimaryFramebuffer::hasDDCConnect(IOIndex connectIndex) {
	if (connectIndex != 0) return false;
	if (fEdidLen < rdna4edidmodes::BlockSize && fPci) {
		GarnetDriver *driver = retain_driver();
		if (readLiveHardwareEdid(fPci, driver)) {
			if (OSData *edidProp = OSData::withBytes(fEdid, static_cast<unsigned int>(fEdidLen))) {
				setProperty("DDC,EDID", edidProp);
				edidProp->release();
			}
			if (driver && (fMaxDispClkKhz == 0u || fMaxDppClkKhz == 0u)) {
				queryClockCaps(fPci, driver);
			}
			if (fModeTable.consoleWidth > 0u && fModeTable.consoleHeight > 0u) {
				rebuildModeTable(fModeTable.consoleWidth, fModeTable.consoleHeight, fModeTable.consoleRowBytes);
				fCurrentMode = static_cast<IODisplayModeID>(fModeTable.activeModeId);
			}
		}
		if (driver) driver->release();
	}
	return fEdidLen >= rdna4edidmodes::BlockSize && rdna4edidmodes::baseBlockValid(fEdid);
}

IOReturn AMDGarnetPrimaryFramebuffer::getDDCBlock(IOIndex connectIndex, UInt32 blockNumber,
                                              IOSelect blockType, IOOptionBits,
                                              UInt8 *data, IOByteCount *length) {
	if (connectIndex != 0 || blockType != kIODDCBlockTypeEDID) return kIOReturnUnsupported;
	if (!data || !length || *length < rdna4edidmodes::BlockSize) return kIOReturnBadArgument;
	uint8_t block[rdna4edidmodes::BlockSize] {};
	if (!rdna4fbprimary::sliceDdcBlock(fEdid, fEdidLen, blockNumber, block)) {
		return kIOReturnNotFound;
	}
	memcpy(data, block, sizeof(block));
	*length = sizeof(block);
	return kIOReturnSuccess;
}

IOReturn AMDGarnetPrimaryFramebuffer::registerForInterruptType(IOSelect interruptType,
                                                            IOFBInterruptProc proc,
                                                            OSObject *target,
                                                            void *ref,
                                                            void **interruptRef) {
	if (interruptType != kIOFBVBLInterruptType) return kIOReturnUnsupported;
	if (!proc || !interruptRef) return kIOReturnBadArgument;
	if (!ensureVblTimer()) return kIOReturnNoMemory;

	if (fVblLock) IOLockLock(fVblLock);
	fVblEnabled = false;
	fVblProc = proc;
	fVblTarget = target;
	fVblRef = ref;
	if (fVblLock) IOLockUnlock(fVblLock);

	*interruptRef = &fVblProc;
	return setInterruptState(*interruptRef, kEnabledInterruptState);
}

IOReturn AMDGarnetPrimaryFramebuffer::unregisterInterrupt(void *interruptRef) {
	if (interruptRef != &fVblProc) return kIOReturnBadArgument;

	if (fVblLock) IOLockLock(fVblLock);
	fVblEnabled = false;
	fVblProc = nullptr;
	fVblTarget = nullptr;
	fVblRef = nullptr;
	if (fVblLock) IOLockUnlock(fVblLock);

	if (fVblTimer) {
		fVblTimer->disable();
		fVblTimer->cancelTimeout();
	}
	return kIOReturnSuccess;
}

IOReturn AMDGarnetPrimaryFramebuffer::setInterruptState(void *interruptRef, UInt32 state) {
	if (interruptRef != &fVblProc) return kIOReturnBadArgument;
	const bool want = (state != 0u);

	if (fVblLock) IOLockLock(fVblLock);
	const bool registered = (fVblProc != nullptr) && !fStopping;
	if (registered) fVblEnabled = want;
	if (fVblLock) IOLockUnlock(fVblLock);

	if (!registered) return want ? kIOReturnNotReady : kIOReturnSuccess;

	if (want) {
		if (!ensureVblTimer()) return kIOReturnNoMemory;
		fVblTimer->enable();
		fVblTimer->setTimeoutUS(vblPeriodUs());
	} else if (fVblTimer) {
		fVblTimer->disable();
		fVblTimer->cancelTimeout();
	}
	return kIOReturnSuccess;
}

void AMDGarnetPrimaryFramebuffer::disableCursorLocked(GarnetDriver *driver) {
	const bool wasVisible = fCursorVisible;
	fCursorVisible = false;
	if (wasVisible && fPci) {
		MmioAccess mmio {};
		if (acquire_mmio(fPci, driver, &mmio)) {
			const rdna4cursor::CursorHw hw { &mmio, cursor_rd_seg2, cursor_wr_seg2 };
			(void)rdna4cursor::disable_cursor_plane(hw, fCursorPipe);
			release_mmio(mmio);
		}
	}
}

void AMDGarnetPrimaryFramebuffer::teardownCursorSlot() {
	IOMemoryMap *map = nullptr;
	IODeviceMemory *mem = nullptr;
	uint32_t *staging = nullptr;
	if (fCursorLock) IOLockLock(fCursorLock);
	map = fCursorMap;
	mem = fCursorMem;
	staging = fCursorStaging;
	fCursorMap = nullptr;
	fCursorMem = nullptr;
	fCursorStaging = nullptr;
	fCursorVram = nullptr;
	fCursorMcAddr = 0u;
	fCursorHwAvailable = false;
	fCursorVisible = false;
	if (fCursorLock) IOLockUnlock(fCursorLock);
	if (map) map->release();
	if (mem) mem->release();
	if (staging) IOFree(staging, rdna4cursor::kCursorSlotBytes);
}

void AMDGarnetPrimaryFramebuffer::initCursorSlot(GarnetDriver *driver) {
	if (fCursorHwAvailable || fStopping || !fPci || !fCursorLock) return;
	uint32_t hwCursorArg = 1u;
	if (PE_parse_boot_argn("garnet-hwcursor", &hwCursorArg, sizeof(hwCursorArg)) && hwCursorArg == 0u) {
		RDNA4LOG("primaryfb: hwcursor disabled by boot-arg garnet-hwcursor=0");
		return;
	}

	const uint32_t pipe = fActiveOutput.found ? fActiveOutput.otg : 0u;
	if (pipe >= rdna4cursor::kCursorMaxPipes) return;

	MmioAccess mmio {};
	if (!acquire_mmio(fPci, driver, &mmio)) return;
	const uint32_t lo =
	    mmio_rd_seg(&mmio, 2u, rdna4cursor::hubp_reg(pipe, rdna4cursor::kSeg2HubpPrimaryAddr));
	const uint32_t hi =
	    mmio_rd_seg(&mmio, 2u, rdna4cursor::hubp_reg(pipe, rdna4cursor::kSeg2HubpPrimaryAddrHigh));
	release_mmio(mmio);

	if (lo == 0xffffffffu || hi == 0xffffffffu) return;
	const uint64_t primaryMc = (static_cast<uint64_t>(hi & 0xffffu) << 32) | lo;
	if (!primaryMc) return;

	uint64_t cursorApertureLimit = fBar0Size;
	if (driver) {
		rdna4peer::Snapshot snap {};
		if (driver->snapshotDisplayState(&snap) && snap.state == rdna4peer::kStateReady) {
			if (snap.bar0Phys != fBar0Phys) return;
			if (snap.vramLimit != 0u && snap.vramLimit < cursorApertureLimit) {
				cursorApertureLimit = snap.vramLimit;
			}
		}
	}

	rdna4cursor::CursorSlotPlan plan {};
	if (!rdna4cursor::plan_cursor_slot(fBar0Phys, cursorApertureLimit, fConsolePhys, fConsoleLen,
	                                    primaryMc, &plan)) {
		return;
	}

	uint32_t *staging = static_cast<uint32_t *>(IOMalloc(rdna4cursor::kCursorSlotBytes));
	if (!staging) return;
	bzero(staging, rdna4cursor::kCursorSlotBytes);

	IODeviceMemory *mem = IODeviceMemory::withRange(static_cast<IOPhysicalAddress>(plan.physAddr),
	                                                rdna4cursor::kCursorSlotBytes);
	if (!mem) {
		IOFree(staging, rdna4cursor::kCursorSlotBytes);
		return;
	}
	IOMemoryMap *map = mem->map();
	if (!map || !map->getVirtualAddress() || map->getLength() < rdna4cursor::kCursorSlotBytes) {
		if (map) map->release();
		mem->release();
		IOFree(staging, rdna4cursor::kCursorSlotBytes);
		return;
	}

	uint32_t *vram = reinterpret_cast<uint32_t *>(map->getVirtualAddress());
	bzero(vram, rdna4cursor::kCursorSlotBytes);

	bool published = false;
	IOLockLock(fCursorLock);
	if (!fCursorHwAvailable && !fStopping) {
		fCursorStaging = staging;
		fCursorMem = mem;
		fCursorMap = map;
		fCursorVram = vram;
		fCursorMcAddr = plan.mcAddr;
		fCursorPipe = pipe;
		fCursorHwAvailable = true;
		published = true;
	}
	IOLockUnlock(fCursorLock);
	if (!published) {
		map->release();
		mem->release();
		IOFree(staging, rdna4cursor::kCursorSlotBytes);
		return;
	}

	RDNA4LOG("primaryfb: hwcursor slot pipe=%u phys=%#llx mc=%#llx size=%u",
	         pipe, static_cast<unsigned long long>(plan.physAddr),
	         static_cast<unsigned long long>(plan.mcAddr), plan.sizeBytes);
}

uint32_t AMDGarnetPrimaryFramebuffer::currentPixelClockKhz() const {
	rdna4fbprimary::TimingSpec ts {};
	if (rdna4fbprimary::timingSpecForMode(fModeTable, fCurrentMode, &ts) &&
	    ts.pixelClockHz >= 1000ull) {
		return static_cast<uint32_t>(ts.pixelClockHz / 1000ull);
	}
	return 0u;
}

IOReturn AMDGarnetPrimaryFramebuffer::setCursorImage(void *cursorImage) {
	if (!fCursorHwAvailable || fStopping || !fCursorLock) return kIOReturnUnsupported;
	GarnetDriver *driver = retain_driver();
	IOLockLock(fCursorLock);
	if (!fCursorHwAvailable || fStopping || !fCursorVram || !fCursorStaging) {
		IOLockUnlock(fCursorLock);
		if (driver) driver->release();
		return kIOReturnUnsupported;
	}

	if (!cursorImage) {
		disableCursorLocked(driver);
		IOLockUnlock(fCursorLock);
		if (driver) driver->release();
		return kIOReturnUnsupported;
	}

	bzero(fCursorStaging, rdna4cursor::kCursorSlotBytes);
	IOHardwareCursorDescriptor desc {};
	desc.majorVersion = kHardwareCursorDescriptorMajorVersion;
	desc.minorVersion = kHardwareCursorDescriptorMinorVersion;
	desc.height = rdna4cursor::kCursorMaxDim;
	desc.width = rdna4cursor::kCursorMaxDim;
	desc.bitDepth = kIO32ARGBPixelFormat;

	IOHardwareCursorInfo info {};
	info.majorVersion = kHardwareCursorInfoMajorVersion;
	info.minorVersion = kHardwareCursorInfoMinorVersion;
	info.hardwareCursorData = reinterpret_cast<UInt8 *>(fCursorStaging);

	const bool converted = convertCursorImage(cursorImage, &desc, &info);
	if (!converted || info.cursorWidth == 0u || info.cursorHeight == 0u ||
	    info.cursorWidth > rdna4cursor::kCursorMaxDim ||
	    info.cursorHeight > rdna4cursor::kCursorMaxDim) {
		disableCursorLocked(driver);
		IOLockUnlock(fCursorLock);
		if (driver) driver->release();
		return kIOReturnUnsupported;
	}

	rdna4cursor::pack_cursor_128x128(fCursorStaging, info.cursorWidth, info.cursorHeight,
	                                 fCursorVram);
	__sync_synchronize();

	fCursorW = info.cursorWidth;
	fCursorH = info.cursorHeight;
	fCursorHotX = info.cursorHotSpotX;
	fCursorHotY = info.cursorHotSpotY;
	fCursorNeedsProgram = true;

	bool ok = true;
	if (fCursorVisible && fPci) {
		MmioAccess mmio {};
		ok = acquire_mmio(fPci, driver, &mmio);
		if (ok) {
			const rdna4cursor::CursorHw hw { &mmio, cursor_rd_seg2, cursor_wr_seg2 };
			const rdna4cursor::CursorPlaneConfig cfg {
				fCursorPipe,
				fCursorMcAddr,
				fCursorW,
				fCursorH,
				fCursorX,
				fCursorY,
				fCursorHotX,
				fCursorHotY,
				currentPixelClockKhz(),
				true,
			};
			ok = rdna4cursor::program_cursor_plane(hw, cfg);
			if (ok) fCursorNeedsProgram = false;
			release_mmio(mmio);
		}
	}

	IOLockUnlock(fCursorLock);
	if (driver) driver->release();
	return ok ? kIOReturnSuccess : kIOReturnError;
}

IOReturn AMDGarnetPrimaryFramebuffer::setCursorState(SInt32 x, SInt32 y, bool visible) {
	if (!fCursorHwAvailable || fStopping || !fCursorLock) return kIOReturnUnsupported;
	GarnetDriver *driver = retain_driver();
	IOLockLock(fCursorLock);
	if (!fCursorHwAvailable || fStopping) {
		IOLockUnlock(fCursorLock);
		if (driver) driver->release();
		return kIOReturnUnsupported;
	}

	const bool visibilityChanged = (visible != fCursorVisible);
	fCursorX = x;
	fCursorY = y;
	fCursorVisible = visible;

	if (fCursorW == 0u || fCursorH == 0u) {
		IOLockUnlock(fCursorLock);
		if (driver) driver->release();
		return kIOReturnSuccess;
	}

	MmioAccess mmio {};
	bool ok = fPci && acquire_mmio(fPci, driver, &mmio);
	if (ok) {
		const rdna4cursor::CursorHw hw { &mmio, cursor_rd_seg2, cursor_wr_seg2 };
		const rdna4cursor::CursorPlaneConfig cfg {
			fCursorPipe,
			fCursorMcAddr,
			fCursorW,
			fCursorH,
			fCursorX,
			fCursorY,
			fCursorHotX,
			fCursorHotY,
			currentPixelClockKhz(),
			fCursorVisible,
		};
		if (fCursorNeedsProgram) {
			ok = rdna4cursor::program_cursor_plane(hw, cfg);
			if (ok) fCursorNeedsProgram = false;
		} else {
			ok = rdna4cursor::move_cursor_plane(hw, cfg, visibilityChanged);
		}
		release_mmio(mmio);
	}

	IOLockUnlock(fCursorLock);
	if (driver) driver->release();
	return ok ? kIOReturnSuccess : kIOReturnError;
}

void AMDGarnetPrimaryFramebuffer::onSystemWake(GarnetDriver *driver) {
	if (fStopping) return;
	if (fPci && fActiveOutput.found && fActiveOutput.isDp && fActiveOutput.auxValid &&
	    fActiveOutput.aux < rdna4ddcpad::kMaxDdcLines) {
		MmioAccess mmio {};
		if (acquire_mmio(fPci, driver, &mmio)) {
			const rdna4ddcpad::DdcPadHw padHw { &mmio, cursor_rd_seg2, cursor_wr_seg2, ddcpad_sleep_ms };
			if (rdna4ddcpad::set_ddc_pad_mode(padHw, static_cast<uint8_t>(fActiveOutput.aux), false)) {
				RDNA4LOG("primaryfb: wake restored DP AUX%u pads to AUX mode", fActiveOutput.aux);
			}
			release_mmio(mmio);
		}
	}
	if (fCursorLock) {
		IOLockLock(fCursorLock);
		if (fCursorHwAvailable) fCursorNeedsProgram = true;
		IOLockUnlock(fCursorLock);
	}
}
