//
//  GarnetDriverStages.cpp — Read-only hardware survey, PSP/firmware staging,
//  and bring-up ladder stages extracted from GarnetDriver.cpp.
//
#include "GarnetDriver.hpp"
#include "GarnetDriverInternal.hpp"
#include "regs.hpp"
#include "GarnetNativeClient.hpp"
#include "amd/amdgpu_asic.h"
#include "amd/amdgpu_discovery.h"
#include "amd/amdgpu_init.h"
#include "amd/amdgpu_smu.h"
#include "amd/garnet_power_policy_pure.h"
#include "amd/native_s1b.h"
#include "fw/fw_table.h"
#include "psp.hpp"
#include <IOKit/IOLib.h>
#include <libkern/c++/OSData.h>
#include <libkern/c++/OSSymbol.h>

namespace {
// rdna4::HwAccess adapter: keeps the IOKit class single-inheritance.
struct HwBridge final : rdna4::HwAccess {
	GarnetDriver *o;
	explicit HwBridge(GarnetDriver *owner) : o(owner) {}
	uint32_t regReadIp (uint16_t h, uint8_t i, uint8_t s, uint32_t d) override { return o->regReadIp(h, i, s, d); }
	bool     regWriteIp(uint16_t h, uint8_t i, uint8_t s, uint32_t d, uint32_t v) override { return o->regWriteIp(h, i, s, d, v); }
	bool     vramWrite (uint64_t off, const void *src, size_t len) override { return o->vramWrite(off, src, len); }
	bool     vramMemset(uint64_t off, uint8_t v, size_t len) override { return o->vramMemset(off, v, len); }
};
} // namespace

// ---------------------------------------------------------------------------
// Read-only survey stages
// ---------------------------------------------------------------------------

bool GarnetDriver::probeMemSize() {
	uint32_t off = rdna4::kNbifMemsizeByteFallback, discOff;
	if (ipDiscovery.isValid() &&
	    ipDiscovery.regByteOffset(IpDiscovery::HwNbif, 0, rdna4::kNbifMemsizeSeg, rdna4::kNbifMemsizeDword, discOff))
		off = discOff;
	vramMB = regRead32(off);
	if (vramMB == 0 || vramMB == 0xFFFFFFFF) {
		RDNA4LOG("mmio: RCC_CONFIG_MEMSIZE read failed (0x%08x) — MMIO not usable", vramMB);
		return false;
	}
	RDNA4LOG("mmio: VRAM size %u MiB (RCC_CONFIG_MEMSIZE @ 0x%x)", vramMB, off);
	setProperty("VRAM,TotalMB", static_cast<uint64_t>(vramMB), 32);
	setProperty("MMIO,Verified", vramMB >= 1024 && vramMB <= 65536);
	return true;
}

bool GarnetDriver::loadOnDieDiscovery() {
	if (!rmmio || vramMB == 0) return false;
	onDieDisc = static_cast<uint8_t *>(IOMalloc(rdna4::kDiscTmrSize));
	if (!onDieDisc) return false;
	uint64_t pos = (static_cast<uint64_t>(vramMB) << 20) - rdna4::kDiscTmrOffset;
	uint32_t *dw = reinterpret_cast<uint32_t *>(onDieDisc);
	for (uint32_t i = 0; i < rdna4::kDiscTmrSize / 4; i++) dw[i] = vramRead32(pos + 4ULL * i);
	RDNA4LOG("discovery: on-die TMR at vram+0x%llx, first dwords %08x %08x", pos, dw[0], dw[1]);
	if (!ipDiscovery.init(onDieDisc, rdna4::kDiscTmrSize)) {
		RDNA4LOG("discovery: on-die TMR did not validate");
		IOFree(onDieDisc, rdna4::kDiscTmrSize); onDieDisc = nullptr;
		return false;
	}
	RDNA4LOG("discovery: on-die binary valid, %u IPs", ipDiscovery.ipCount());
	setProperty("Discovery,Source", "on-die TMR");
	setProperty("Discovery,IPCount", static_cast<uint64_t>(ipDiscovery.ipCount()), 16);
	return true;
}

// RDNA4: choose the chip this boot runs on and its die geometry. The discovered GC version
// wins over the PCI id (Linux keys everything on IP versions); the PCI id is the fallback when
// discovery is unavailable. Geometry: the discovery gc_info table (Linux amdgpu_discovery_get_gfx_info),
// else the chip's fallback row.
bool GarnetDriver::selectAsic(bool haveDiscovery) {
	const uint32_t vendorDevice = pciDevice->configRead32(kIOPCIConfigVendorID);
	const uint16_t device = static_cast<uint16_t>(vendorDevice >> 16);
	const amdgpu::AsicInfo *byPci = amdgpu::asic_from_pci(static_cast<uint16_t>(vendorDevice), device), *byGc = nullptr;
	IpDiscovery::IpEntry gc;
	if (haveDiscovery && ipDiscovery.findIp(IpDiscovery::HwGc, 0, gc)) {
		byGc = amdgpu::asic_from_gc(gc.major, gc.minor, gc.revision);
		if (!byGc) RDNA4LOG("asic: GC v%u.%u.%u is not in the chip table; using the PCI id", gc.major, gc.minor, gc.revision);
	}
	const amdgpu::AsicInfo *a = byGc ? byGc : byPci;
	amdgpu::asic_select(a);
	if (!a) {
		RDNA4LOG("asic: no supported chip for PCI id 0x%04x; refusing startup", device);
		return false;
	}
	if (byGc && byPci && byGc != byPci)
		RDNA4LOG("asic: PCI id 0x%04x says %s but discovery GC says %s; following discovery", device, byPci->family, byGc->family);
	if (byPci) {
		char model[64];
		snprintf(model, sizeof(model), "AMD Radeon %s", amdgpu::asic_pci_name(byPci, device));
		// Publish as NUL-terminated OSData (macOS PCI GPU convention; Geekbench calls CFDataGetBytePtr).
		if (OSData *modelData = OSData::withBytes(model, static_cast<unsigned int>(strlen(model) + 1))) {
			pciDevice->setProperty("model", modelData);
			modelData->release();
		}
	}
	RDNA4LOG("asic: %s (%s, 0x%04x), GC %u.%u.%u, MP0 %u.%u.%u, firmware %s", a->family, amdgpu::asic_pci_name(a, device),
	       device, a->gcMajor, a->gcMinor, a->gcRev, a->mp0Major, a->mp0Minor, a->mp0Rev,
	       amdgpu::fw_chip_complete(a) ? "embedded" : "NOT embedded");
	setProperty("ASIC,Family", a->family);
	setProperty("ASIC,FirmwareEmbedded", amdgpu::fw_chip_complete(a));

	amdgpu::GcGeometry g;
	if (haveDiscovery && amdgpu::gc_info_parse(ipDiscovery.binary(), ipDiscovery.binaryLength(), g)) {
		RDNA4LOG("asic: gc_info v%u.%u: %u SE x %u SA x %u CU, %u RB/SE, %u GL2C", g.gcInfoMajor, g.gcInfoMinor,
		       g.max_shader_engines, g.max_sh_per_se, g.max_cu_per_sh, g.max_backends_per_se, g.num_gl2c);
	} else {
		g = a->fallbackGeometry;
		RDNA4LOG("asic: gc_info unavailable; %s fallback geometry %u SE x %u SA x %u CU, %u RB/SE", a->family,
		       g.max_shader_engines, g.max_sh_per_se, g.max_cu_per_sh, g.max_backends_per_se);
	}
	dev.gc = g;
	setProperty("ASIC,GeometryFromDiscovery", g.fromDiscovery);
	setProperty("ASIC,ShaderEngines", static_cast<uint64_t>(g.max_shader_engines), 32);
	setProperty("ASIC,CUs", static_cast<uint64_t>(g.max_shader_engines * g.max_sh_per_se * g.max_cu_per_sh), 32);
	return true;
}

void GarnetDriver::surveyIPs() {
	struct { uint16_t id; const char *name; } kWanted[] = {
		{ IpDiscovery::HwGc, "GC" }, { IpDiscovery::HwMmhub, "MMHUB" }, { IpDiscovery::HwMp0, "PSP" },
		{ IpDiscovery::HwMp1, "SMU" }, { IpDiscovery::HwSdma0, "SDMA" }, { IpDiscovery::HwOsssys, "IH" },
		{ IpDiscovery::HwNbif, "NBIF" }, { IpDiscovery::HwDmu, "DCN" }, { IpDiscovery::HwHdp, "HDP" },
		{ IpDiscovery::HwUmc, "UMC" },
	};
	for (auto &w : kWanted) {
		IpDiscovery::IpEntry ip;
		if (!ipDiscovery.findIp(w.id, 0, ip)) continue;
		RDNA4LOG("ip: %-5s v%u.%u.%u  (%u segment base%s)", w.name, ip.major, ip.minor, ip.revision,
		       ip.numBases, ip.numBases == 1 ? "" : "s");
		char key[32], ver[16];
		snprintf(key, sizeof(key), "IP,%s", w.name);
		snprintf(ver, sizeof(ver), "%u.%u.%u", ip.major, ip.minor, ip.revision);
		setProperty(key, ver);
	}
}

void GarnetDriver::surveyPSP() {
	uint32_t boot = regReadIp(IpDiscovery::HwMp0, 0, 0, rdna4::kPspC2PMsg35);
	uint32_t sol  = regReadIp(IpDiscovery::HwMp0, 0, 0, rdna4::kPspC2PMsg81);
	uint32_t ring = regReadIp(IpDiscovery::HwMp0, 0, 0, rdna4::kPspC2PMsg64);
	RDNA4LOG("psp: boot=0x%08x sos=0x%08x ring64=0x%08x  [bootloader %s, sOS %s, C2PMSG_64 resp-flag %s]",
	       boot, sol, ring, (boot & rdna4::kPspBootReadyBit) ? "READY" : "not-ready",
	       sol ? "ALIVE" : "down", (ring & rdna4::kPspRespFlagBit) ? "set" : "clear");
	if (sol && sol != 0xFFFFFFFF) {
		uint32_t v58 = regReadIp(IpDiscovery::HwMp0, 0, 0, rdna4::kPspFwVer58);
		uint32_t v59 = regReadIp(IpDiscovery::HwMp0, 0, 0, rdna4::kPspFwVer59);
		RDNA4LOG("psp: sOS alive: C2PMSG_81=0x%08x C2PMSG_58=0x%08x C2PMSG_59=0x%08x", sol, v58, v59);
		setProperty("PSP,SOSSignOfLife", static_cast<uint64_t>(sol), 32);
		setProperty("PSP,Alive", true);
	}
}

void GarnetDriver::surveySMU() {
	uint32_t on = 0;
	if (!PE_parse_boot_argn("garnet-smu", &on, sizeof(on)) || on == 0) return;
	uint32_t regMsg, regParam, regResp;
	if (!ipDiscovery.regByteOffset(IpDiscovery::HwMp1, 0, rdna4::kSmuSeg, rdna4::kSmuMsgDword,   regMsg)   ||
	    !ipDiscovery.regByteOffset(IpDiscovery::HwMp1, 0, rdna4::kSmuSeg, rdna4::kSmuParamDword, regParam) ||
	    !ipDiscovery.regByteOffset(IpDiscovery::HwMp1, 0, rdna4::kSmuSeg, rdna4::kSmuRespDword,  regResp)) {
		RDNA4LOG("smu: MP1 mailbox not in discovery table"); return;
	}
	if (static_cast<uint64_t>(regMsg) + 4 > rmmioSize || static_cast<uint64_t>(regParam) + 4 > rmmioSize || static_cast<uint64_t>(regResp) + 4 > rmmioSize) return;
	auto send = [&](uint32_t msgId, uint32_t param, uint32_t *ret) -> uint32_t {
		rmmio[regResp / 4] = 0; rmmio[regParam / 4] = param; rmmio[regMsg / 4] = msgId;
		uint32_t resp = 0;
		for (int i = 0; i < 500; i++) { resp = rmmio[regResp / 4]; if (resp) break; IOSleep(1); }
		if (ret) *ret = rmmio[regParam / 4];
		return resp;
	};
	uint32_t ret = 0;
	if (send(rdna4::kSmuMsgTestMessage, 0xC0FFEE, &ret) != rdna4::kSmuRespOK) { RDNA4LOG("smu: TestMessage not acked"); return; }
	RDNA4LOG("smu: PING OK (ret=0x%08x)", ret);
	if (send(rdna4::kSmuMsgGetSmuVersion, 0, &ret) == rdna4::kSmuRespOK) {
		RDNA4LOG("smu: PMFW version %u.%u.%u", (ret >> 16) & 0xff, (ret >> 8) & 0xff, ret & 0xff);
		setProperty("SMU,FirmwareVersion", static_cast<uint64_t>(ret), 32);
	}
	setProperty("SMU,Verified", true);
}

// ---------------------------------------------------------------------------
// rdna4-psp=1 — FIRST WRITING STAGE: PSP bootloader chain -> SOS alive
// ---------------------------------------------------------------------------

void GarnetDriver::stagePSP() {
	uint32_t on = 0;
	if (!PE_parse_boot_argn("garnet-psp", &on, sizeof(on)) || on == 0) on = targetStage >= 5;
	if (!on) return;
	RDNA4LOG("psp-stage: requested (rdna4-psp=1) — this stage WRITES to VRAM and the PSP mailbox");
	setProperty("PSP,Stage", "requested");

	HwBridge hw(this);
	rdna4::PspLoader psp(hw);
	// RDNA4: the SOS container of the selected chip (psp_14_0_3_sos on Navi 48, psp_14_0_2_sos on Navi 44).
	const amdgpu::AsicInfo *chip = amdgpu::asic_current();
	if (!chip) {
		RDNA4LOG("psp-stage: no chip selected; aborting");
		setProperty("PSP,Stage", "no-asic"); return;
	}
	const amdgpu::FwBlob *sos = amdgpu::fw_get(amdgpu::FwId::PSP_SOS);
	if (!amdgpu::fw_chip_complete(chip) || !sos || !sos->data) {
		RDNA4LOG("psp-stage: %s firmware not embedded in this build (make CHIPS=...); aborting", chip->family);
		setProperty("PSP,Stage", "no-firmware"); return;
	}
	RDNA4LOG("psp-stage: SOS container %s (%lu bytes)", sos->name, static_cast<unsigned long>(sos->size));
	if (!psp.parseContainer(sos->data, sos->size)) {
		RDNA4LOG("psp-stage: embedded container failed to parse; aborting"); setProperty("PSP,Stage", "bad-container"); return;
	}
	setProperty("PSP,ContainerSOSVersion", static_cast<uint64_t>(psp.sosVersion()), 32);
	// Check the selected chip's firmware before the aperture test writes VRAM.
	if (!mapVramAperture())        { RDNA4LOG("psp-stage: no usable BAR0 aperture; aborting"); setProperty("PSP,Stage", "no-bar0"); return; }
	captureBootFramebuffer();      // best effort; chooseVramBase() copes if unknown
	if (!chooseVramBase())         { dropBar0(); setProperty("PSP,Stage", "no-vram-region"); return; }
	if (!finishVramPlan())         { dropBar0(); RDNA4LOG("psp-stage: BAR0 plan refused with the console located; aborting (BAR0 unmapped: the no-BAR0 state)"); setProperty("PSP,Stage", "no-bar0"); return; }
	if (!apertureCheckWindow())    { dropBar0(); RDNA4LOG("psp-stage: aperture check failed — BAR0 offsets are not VRAM offsets here; refusing (BAR0 unmapped: the no-BAR0 state)"); setProperty("PSP,Stage", "aperture-mismatch"); return; }
	if (!psp.setFwPriOffset(vramBase)) { setProperty("PSP,Stage", "bad-fwpri"); return; }
	if (!psp.preflight()) { RDNA4LOG("psp-stage: preflight refused; nothing sent to the PSP"); setProperty("PSP,Stage", "preflight-refused"); return; }
	setProperty("PSP,VramMCBase", psp.mcBase(), 64);
	setProperty("PSP,FwPriMC", psp.fwPriMC(), 64);

	const bool ok = psp.runChain();
	setProperty("PSP,Stage", ok ? "sos-alive" : "chain-failed");
	setProperty("PSP,LastStep", psp.lastStep());
	setProperty("PSP,LastStatus", static_cast<uint64_t>(psp.lastBootloaderStatus()), 32);
	if (ok) RDNA4LOG("psp-stage: SUCCESS — SOS alive (sign-of-life 0x%08x)", psp.lastBootloaderStatus());
	else    RDNA4LOG("psp-stage: FAILED at step %s, bootloader status 0x%08x", psp.lastStep(), psp.lastBootloaderStatus());
	surveyPSP();   // record the live state either way
	if (ok && targetStage > 5) {
		if (buildDeviceContext()) { dev.psoCAlive = true; runStages(targetStage); }
	}
}

// ---------------------------------------------------------------------------
// Compat layer for the ported mac-amdgpu modules (src/amd/*)
// ---------------------------------------------------------------------------

bool GarnetDriver::buildDeviceContext() {
	// Stages >= 6 DMA into system memory (IH ring, GART-mapped buffers, MES
	// queues): the PCI nub must be a bus master. Memory space is already on.
	pciDevice->setBusMasterEnable(true);
	RDNA4LOG("dev: PCI command 0x%04x (bus master %s)", pciDevice->configRead16(kIOPCIConfigCommand),
	       (pciDevice->configRead16(kIOPCIConfigCommand) & 0x4) ? "ON" : "off");
	dev.rmmio = rmmio; dev.rmmioSize = rmmioSize;
	dev.bar0 = bar0; dev.bar0Size = bar0Size; dev.bar0Phys = bar0Phys;
	dev.vramSizeBytes = static_cast<uint64_t>(vramMB) << 20;
	dev.vramBase  = vramBase;
	dev.vramLimit = bar0Limit;   // 0.0.663 (ReBAR item 3): rdna4rebar::plan_bar0's clamp (== bar0Size at a 256 MiB BAR0); finishVramPlan has run (stagePSP)
	uint32_t fbBase = regReadIp(IpDiscovery::HwMmhub, 0, 0, 0x0554);
	if (fbBase == 0xFFFFFFFF) { RDNA4LOG("dev: cannot read FB_LOCATION_BASE"); return false; }
	dev.vramMcBase = static_cast<uint64_t>(fbBase & 0x00FFFFFF) << 24;
	if (!amdgpu::fill_ip_base_table(ipDiscovery, dev.ip)) { RDNA4LOG("dev: IP base table incomplete"); return false; }
	RDNA4LOG("dev: context ready — MC base 0x%llx, region vram+[0x%llx, 0x%llx), GC base 0x%x MP0 base 0x%x MMHUB base 0x%x",
	       dev.vramMcBase, dev.vramBase, dev.vramLimit, dev.ip.get(amdgpu::IPBlock::GC),
	       dev.ip.get(amdgpu::IPBlock::MP0), dev.ip.get(amdgpu::IPBlock::MMHUB));
	return true;
}

// Publish the option-B' (rdna4-pdb0=1) result. `RDNA4,PDB0` is a single
// sentence so `ioreg` alone answers "did the alias work"; `RDNA4,PDB0Regs`
// carries the seven GFXHUB CONTEXT0 registers the design turns on. Both are
// absent entirely when the boot-arg is off, which is itself the answer.
static void publishPDB0On(IOService *svc, const amdgpu::BringupContext &ctx) {
	const amdgpu::PDB0SelfTest &t = ctx.pdb0Test;
	if (!ctx.gmc.pdb0_want) return;
	char s[192];
	if (!t.ran) {
		snprintf(s, sizeof(s), "off: rdna4-pdb0=1 but the PDB0 was not enabled "
		         "(build refused or SDMA not up)");
	} else {
		snprintf(s, sizeof(s),
		         "%s: alias %s, identity %s, gart %s%s",
		         ctx.gmc.pdb0_active ? "on" : "off",
		         t.alias_ok    ? "ok" : "FAILED",
		         t.identity_ok ? "ok" : "FAILED",
		         t.gart_ok     ? "ok" : "FAILED",
		         t.reverted ? " (CONTEXT0 REVERTED to the flat table)" : "");
	}
	svc->setProperty("Garnet,PDB0", s);
	char r[256];
	snprintf(r, sizeof(r),
	         "CNTL(0x1624)=%#010x BASE(0x168f/0x1690)=%#010x:%#010x "
	         "START(0x16af/0x16b0)=%#010x:%#010x END(0x16cf/0x16d0)=%#010x:%#010x "
	         "depth=%u block_size=%u",
	         t.regs[0], t.regs[2], t.regs[1], t.regs[4], t.regs[3],
	         t.regs[6], t.regs[5],
	         (unsigned)((t.regs[0] >> 1) & 0x3u),
	         (unsigned)((t.regs[0] >> 4) & 0xFu));
	svc->setProperty("Garnet,PDB0Regs", r);
	if (t.ran) {
		svc->setProperty("Garnet,PDB0Addr", ctx.gmc.pdb0_mc, 64);
		svc->setProperty("Garnet,PDB0AliasAddr", t.alias_addr, 64);
		svc->setProperty("Garnet,PDB0BadDwords",
		                 static_cast<uint64_t>(t.alias_bad + t.identity_bad + t.gart_bad), 32);
	}
}

// 0.0.185 — `RDNA4,SDMAQ1Test` answers ONE question in one sentence, so `ioreg`
// alone settles whether `sdmamap` may be run on this boot: does BAR2 doorbell
// dword 0x202 reach SDMA0 QUEUE1? "MMIO only" means the queue works and the
// doorbell does not route — then the S2A window has to be widened before
// Apple's ring is handed over, and `sdmamap` will refuse.
static void publishSDMAQ1On(IOService *svc, const amdgpu::BringupContext &ctx) {
	if (!ctx.sdmaQ1TestWant) return;
	const amdgpu::SDMAQ1TestResult &t = ctx.sdmaQ1Test;
	char s[224];
	if (!t.ran) {
		snprintf(s, sizeof(s), "skipped: rdna4-sdma-q1-test=1 but SDMA0 QUEUE0 was "
		         "not running (or no VRAM for the scratch ring)");
	} else if (t.doorbell_ok) {
		snprintf(s, sizeof(s), "doorbell dword %#x ROUTES to SDMA0 QUEUE1 — fence in "
		         "%llu us (ring %#llx, %u dwords)", t.doorbell_index,
		         (unsigned long long)t.doorbell_us,
		         (unsigned long long)t.ring_gpu_va, t.ring_dwords);
	} else if (t.mmio_ok) {
		snprintf(s, sizeof(s), "MMIO ONLY: QUEUE1 executes (fence in %llu us via "
		         "RB_WPTR) but doorbell dword %#x does NOT route — widen the S2A "
		         "window before sdmamap", (unsigned long long)t.mmio_us,
		         t.doorbell_index);
	} else {
		snprintf(s, sizeof(s), "FAILED: neither the doorbell nor MMIO fenced "
		         "(fence slot read %#010x) — QUEUE1 is not executing; do NOT run "
		         "sdmamap", t.fence_last);
	}
	svc->setProperty("Garnet,SDMAQ1Test", s);
	if (t.ran) {
		char r[192];
		snprintf(r, sizeof(r), "RB_CNTL=%#010x RPTR=%#010x WPTR=%#010x "
		         "doorbell=%#x fence_gpu=%#llx last=%#010x",
		         t.rb_cntl, t.rb_rptr, t.rb_wptr, t.doorbell_index,
		         (unsigned long long)t.fence_gpu, t.fence_last);
		svc->setProperty("Garnet,SDMAQ1TestRegs", r);
	}
}

// 0.0.191 — `RDNA4,SRBMTest` answers's open question in one sentence:
// does an SDMA SRBM_WRITE packet change a register on this card, from the ring
// and from an IB, for a harmless scratch register and for the VM-context
// register Apple programs? "ok(apple)" means the form Apple emits (header
// 0xf000000e + the register's DWORD index) worked; "ok(linux)" means only the
// form sdma_v7_0_ring_emit_wreg emits (header 0x0000000e + reg<<2) worked —
// which would mean our `xlatregs` hook has been writing a well-formed packet
// with the wrong operand all along.
static const char *srbmLegWord(const amdgpu::SDMASRBMTargetResult &r,
                               bool ib)
{
	const uint32_t a = ib ? 2u : 0u;
	const uint32_t l = ib ? 3u : 1u;
	if (r.landed[a]) return "ok(apple)";
	if (r.landed[l]) return "ok(linux)";
	if (ib && r.landed[4]) return "ok(plain-hdr)";
	if (!r.mmio_ok)  return "VOID(reg not MMIO-writable)";
	// Distinguish "the engine never ran the packet" from "it ran and the
	// register did not move" — they have completely different causes.
	const bool fenced = r.fenced[a] || r.fenced[l] || (ib && r.fenced[4]);
	return fenced ? "FAILED(fenced, reg unchanged)" : "FAILED(no fence)";
}

static void publishSRBMOn(IOService *svc, const amdgpu::BringupContext &ctx) {
	if (!ctx.srbmTestWant) return;
	const amdgpu::SDMASRBMTestResult &t = ctx.srbmTest;
	char s[320];
	if (!t.ran) {
		snprintf(s, sizeof(s), "skipped: rdna4-srbm-test=1 but SDMA0 QUEUE0 was "
		         "not running (or no VRAM for the IB page)");
	} else {
		snprintf(s, sizeof(s),
		         "ring-scratch %s, ib-scratch %s, ring-ctx2 %s, ib-ctx2 %s",
		         srbmLegWord(t.scratch, false), srbmLegWord(t.scratch, true),
		         srbmLegWord(t.ctx2, false),    srbmLegWord(t.ctx2, true));
	}
	svc->setProperty("Garnet,SRBMTest", s);
	if (t.ran) {
		char r[352];
		snprintf(r, sizeof(r),
		         "scratch dw %#x before %#010x want %#010x read ring %#010x/%#010x "
		         "ib %#010x/%#010x/%#010x | ctx2 dw %#x before %#010x want %#010x "
		         "read ring %#010x/%#010x ib %#010x/%#010x/%#010x | IB mc %#llx",
		         t.scratch.reg_dw, t.scratch.before, t.scratch.value,
		         t.scratch.readback[0], t.scratch.readback[1],
		         t.scratch.readback[2], t.scratch.readback[3], t.scratch.readback[4],
		         t.ctx2.reg_dw, t.ctx2.before, t.ctx2.value,
		         t.ctx2.readback[0], t.ctx2.readback[1],
		         t.ctx2.readback[2], t.ctx2.readback[3], t.ctx2.readback[4],
		         (unsigned long long)t.ib_gpu_va);
		svc->setProperty("Garnet,SRBMTestRegs", r);
	}
}

// 0.0.193 — RDNA4,VMFragTest. One line that answers INCREMENT 1's question:
// does gfx12's walker accept an Apple-SHAPED tree (DEPTH 1 / BLOCK_SIZE 7,
// 64 KiB L1 entries, 16-entry sub-tables) written in gfx12 ENCODING?
static void publishVMFragOn(IOService *svc, const amdgpu::BringupContext &ctx) {
	if (!ctx.vmfragTestWant) return;
	const amdgpu::VMFragTestResult &t = ctx.vmfragTest;
	char s[320];
	if (!t.ran) {
		snprintf(s, sizeof(s), "skipped: rdna4-vmfrag-test=1 but GFXHUB or SDMA0 "
		         "QUEUE0 was not ready (or no VRAM for the table)");
	} else {
		snprintf(s, sizeof(s),
		         "sub-table %s%s (%u dw wrong), 64K leaf %s (%u dw wrong); "
		         "CONTEXT1_CNTL %#010x; last STATUS_LO32 %#010x walker %u",
		         t.sub_ok ? "ok" : "FAILED",
		         t.inverted ? "(inverse-P)" : "", t.sub_bad,
		         t.leaf_ok ? "ok" : "FAILED", t.leaf_bad,
		         t.ctx1_cntl, t.fault_lo, t.fault_walker);
	}
	svc->setProperty("Garnet,VMFragTest", s);
	if (ctx.vmfragNoValidWant) {   // M4-WS-VMID-VALID: leg (c), only with rdna4-vmfrag-novalid=1
		char nv[256];
		if (!t.nv_ran)
			snprintf(nv, sizeof(nv), "not run: leg (b) did not pass with VALID, so a bare-base result would mean nothing");
		else
			snprintf(nv, sizeof(nv), "base LO %#010x (bit 0 %s): copy %s (%u dw wrong); STATUS_LO32 %#010x walker %u vmid %u "
			         "ADDR_LO %#010x -> %s", t.nv_base_lo, (t.nv_base_lo & 1u) ? "SET, leg void" : "clear",
			         t.nv_ok ? "ok" : "FAILED", t.nv_bad, t.nv_fault_lo, (t.nv_fault_lo >> 1) & 7u,
			         (t.nv_fault_lo >> 20) & 0xfu, t.nv_fault_addr_lo,
			         t.nv_ok ? "gfx12 IGNORED bit 0" : "gfx12 CHECKS bit 0 (or the leg failed otherwise)");
		svc->setProperty("Garnet,VMFragNoValid", nv);
	}
	if (t.ran) {
		char r[256];
		snprintf(r, sizeof(r), "table mc %#llx, data mc %#llx, VA start %#llx, "
		         "sub-table VA %#llx, 64K leaf VA %#llx",
		         (unsigned long long)t.table_mc, (unsigned long long)t.data_mc,
		         (unsigned long long)t.va_start, (unsigned long long)t.sub_va,
		         (unsigned long long)t.leaf_va);
		svc->setProperty("Garnet,VMFragTestGeom", r);
	}
}

// NATIVE S1b (0.0.600, an internal design note step S1b + review MUST-FIX 4): rdna4-native=1 asks for the reserved-VMID VM
// self-test at the END of the ladder. It is REFUSED (logged, nothing native runs) when the Apple accelerator experiment, the boot
// chain, any of the four self-tests (they rewrite CONTEXT1 with RETRY = 1) or the two Apple-only hooks are also armed. Absent (every
// current config) it reads one boot-arg, finds it 0 and does nothing else: no log line, no property, no register. noinline and
// file-static so runStages' frame does not grow.
static __attribute__((noinline)) uint32_t nativeS1bGate(IOService *svc) {
	auto bootArgU32 = [](const char *name) -> uint32_t {
		uint32_t v = 0;
		return PE_parse_boot_argn(name, &v, sizeof(v)) ? v : 0u;
	};
	rdna4native::GateArgs ga {};
	ga.native = bootArgU32("garnet-native");
	if (ga.native == 0u) return (uint32_t)rdna4native::kGateOff;
	ga.accelExperiment = bootArgU32("garnet-accel-experiment");
	ga.bootChain       = bootArgU32("garnet-boot-chain");
	ga.sdmaQ1Test      = bootArgU32("garnet-sdma-q1-test");
	ga.sdmaQnTest      = bootArgU32("garnet-sdma-qn-test");
	ga.srbmTest        = bootArgU32("garnet-srbm-test");
	ga.vmfragTest      = bootArgU32("garnet-vmfrag-test");
	ga.eopBridge       = bootArgU32("garnet-eop-bridge");
	ga.shaderCache     = bootArgU32("garnet-shader-cache");
	const uint32_t gate = (uint32_t)rdna4native::gate_decide(ga);
	amdgpu::native_s1b_set_gate(gate, rdna4native::gate_conflicts(ga));
	if (gate == rdna4native::kGateRefused) {
		RDNA4LOG("native-s1b: REFUSED rdna4-native=%u: conflicting boot-args armed (mask %#x: 1 accel-experiment, 2 boot-chain, 4 sdma-q1-test, "
		       "8 sdma-qn-test, 0x10 srbm-test, 0x20 vmfrag-test, 0x40 eop-bridge, 0x80 shader-cache). Nothing native runs on this boot.",
		       ga.native, rdna4native::gate_conflicts(ga));
	} else {
		RDNA4LOG("native-s1b: rdna4-native=%u accepted (no Apple-path or self-test boot-arg armed); the VM self-test runs after stage 17", ga.native);
	}
	char nb[384];
	amdgpu::native_s1b_format(nb, sizeof(nb));
	svc->setProperty("Garnet,NativeS1b", nb);
	return gate;
}
static __attribute__((noinline)) void publishNativeS1bOn(IOService *svc) {
	char nb[384];
	amdgpu::native_s1b_format(nb, sizeof(nb));
	svc->setProperty("Garnet,NativeS1b", nb);
}

// 0.0.617 (K5, cosmetic): About This Mac / system_profiler fall back to RDNA4FB's IOFBMemorySize ("VRAM (Total): 14 MB") because Apple's drivers publish "VRAM,totalMB" (32-bit) and "VRAM,totalsize" (8-byte
// OSData, bytes) on the GPU's IOPCIDevice and ours only had "VRAM,TotalMB" (capital T, on the service). Native boots only (called inside the native latch); the old key stays. No register is touched.
static __attribute__((noinline)) void publishNativeVramOn(IOService *pci, uint32_t vramMB) {
	if (!pci || vramMB == 0u || vramMB == 0xFFFFFFFFu) return;
	pci->setProperty("VRAM,totalMB", static_cast<uint64_t>(vramMB), 32);
	const uint64_t bytes = static_cast<uint64_t>(vramMB) << 20;
	OSData *d = OSData::withBytes(&bytes, sizeof(bytes));
	if (d) { pci->setProperty("VRAM,totalsize", d); d->release(); }
	RDNA4LOG("native: published VRAM,totalMB = %u and VRAM,totalsize = %llu on the IOPCIDevice (About This Mac)", vramMB, (unsigned long long)bytes);
}

void GarnetDriver::runStages(uint32_t target) {
	if (target > (uint32_t)amdgpu::BringupStage::Max) target = (uint32_t)amdgpu::BringupStage::Max;
	amdgpu::BringupContext &ctx = garnet_bringup_ctx();
	ctx.dev = &dev;
	ctx.reached = amdgpu::BringupStage::IPDiscovery;     // discovery done by the survey; SOS by stagePSP
	uint32_t full = 0;
	// SMU feature enable is REQUIRED for the GFX core to power up (stage 11+ hardware finding,
	//: default it on for targets >= RLCInit; rdna4-smu-basic=1 forces it off.
	uint32_t basic = 0;
	ctx.smuFullSetup = (PE_parse_boot_argn("garnet-smu-full", &full, sizeof(full)) && full != 0) ||
	                   (target >= (uint32_t)amdgpu::BringupStage::RLCInit &&
	                    !(PE_parse_boot_argn("garnet-smu-basic", &basic, sizeof(basic)) && basic != 0));
	uint32_t mnf = 0;
	ctx.mesNonFatal = PE_parse_boot_argn("garnet-mes-nonfatal", &mnf, sizeof(mnf)) && mnf != 0;
	uint32_t legacyRb = 0;
	ctx.cpLegacyRb = PE_parse_boot_argn("garnet-cp-legacy-rb", &legacyRb, sizeof(legacyRb)) && legacyRb != 0;
	// The kernel compute queue is proven as of 0.0.29 (MES maps it, the MEC
	// executes indirect buffers on it, fences land in ~78 us), so it is on by
	// default; rdna4-kcq=0 turns it off.
	uint32_t clamp = 0;
	const bool clampPresent = PE_parse_boot_argn("garnet-idle-clamp", &clamp, sizeof(clamp));
	ctx.idleClamp = rdna4pwr::idle_clamp_enabled(clampPresent, clamp);
	uint32_t idleDpm = 1;
	const bool idleDpmPresent = PE_parse_boot_argn("garnet-idle-dpm", &idleDpm, sizeof(idleDpm));
	ctx.idleDpmEnabled = rdna4pwr::idle_dpm_enabled(idleDpmPresent, idleDpm);
	uint32_t kcq = 1;
	PE_parse_boot_argn("garnet-kcq", &kcq, sizeof(kcq));
	ctx.wantKcq = (kcq != 0);
	uint32_t hsa = 0;
	ctx.hsaAbi = PE_parse_boot_argn("garnet-hsa-abi", &hsa, sizeof(hsa)) && hsa != 0;
	uint32_t legacyDb = 0;
	ctx.doorbellLegacy = PE_parse_boot_argn("garnet-doorbell-legacy", &legacyDb, sizeof(legacyDb)) && legacyDb != 0;
	dev.doorbell.index.legacy_map = ctx.doorbellLegacy;
	//: move our GART bump region to the top of the aperture so it stops
	// contending with Apple's page 0 (its completion frame). MUST be set before
	// the ladder reaches GMCInit, which is where gmc_mc_init reads it. Off by
	// default — this relocates the CP ring, MQD, WB and MES buffers.
	uint32_t gartHigh = 0;
	ctx.gmc.gart_high_bump =
	    PE_parse_boot_argn("garnet-gart-high", &gartHigh, sizeof(gartHigh)) && gartHigh != 0;
	// rdna4-pdb0=1 (0.0.183, option B'): give GFXHUB VMID-0 a two-level page
	// table whose PDB0 aliases MC [0, 16 GiB) onto VRAM physical [0, 16 GiB),
	// so Apple's 0-based VRAM addresses (MQDs at 0x0fffd000, CONST_FILLs at
	// 0x3d6c00000, IB bases) become valid GPU addresses without rewriting
	// them. MMHUB is never touched — the VBIOS scanout goes through it.
	// MUST be set before the ladder reaches CPInit, which is where
	// ensure_gfxhub_gart -> gmc_gfxhub_gart_enable programs CONTEXT0.
	// Default OFF: with this clear, not one byte of behaviour changes.
	uint32_t pdb0 = 0;
	ctx.gmc.pdb0_want =
	    PE_parse_boot_argn("garnet-pdb0", &pdb0, sizeof(pdb0)) && pdb0 != 0;
	if (ctx.gmc.pdb0_want)
		RDNA4LOG("stages: rdna4-pdb0=1 — GFXHUB VMID-0 will use the two-level PDB0 "
		       "(option B'); MMHUB stays on the flat table");
	// rdna4-sdma-q1-test=1 (0.0.185): at the END of stage 15, prove whether BAR2
	// doorbell dword 0x202 reaches SDMA0 QUEUE1, using OUR OWN 4 KiB ring and one
	// FENCE packet. Open question 1 of notes/re/sdma-takeover-design.md; it must
	// be answered before `sdmamap` points the engine at Apple's ring, because a
	// silent takeover failure and a doorbell that does not route look identical.
	// QUEUE1 is left DISABLED and the scratch ring freed, so this changes nothing
	// a later boot depends on. Default OFF.
	uint32_t q1t = 0;
	ctx.sdmaQ1TestWant =
	    PE_parse_boot_argn("garnet-sdma-q1-test", &q1t, sizeof(q1t)) && q1t != 0;
	if (ctx.sdmaQ1TestWant)
		RDNA4LOG("stages: rdna4-sdma-q1-test=1 — the SDMA0 QUEUE1 doorbell-routing "
		       "self-test will run at the end of SDMAInit (doorbell dword 0x%x)",
		       static_cast<unsigned>(amdgpu::kSDMAExtSlots[0].doorbell_dword));
	// rdna4-sdma-qn-test=1 (0.0.196): the same FENCE self-test, once per entry
	// of amdgpu::kSDMAExtSlots — every (instance, queue, doorbell) triple the
	// generalised `sdmamap` may hand Apple. Each leg leaves its queue DISABLED
	// and frees its scratch ring, so a test boot still ends inert. It answers the
	// one thing no amount of reading settles: whether the single routed S2A
	// window (RANGE_OFFSET 0x200, RANGE_SIZE 0x14) reaches SDMA1 as well as
	// SDMA0. Default OFF.
	uint32_t qnt = 0;
	ctx.sdmaQnTestWant =
	    PE_parse_boot_argn("garnet-sdma-qn-test", &qnt, sizeof(qnt)) && qnt != 0;
	if (ctx.sdmaQnTestWant)
		RDNA4LOG("stages: rdna4-sdma-qn-test=1 — %u (instance, queue, doorbell) "
		       "triples will be fence-tested at the end of SDMAInit",
		       amdgpu::kSDMAExtSlotCount);
	// rdna4-srbm-test=1 (0.0.191): at the very END of stage 15, ask whether an
	// SDMA_OP_SRBM_WRITE packet from OUR SDMA0 QUEUE0 changes a register at all —
	// in the ring and from an INDIRECT_BUFFER, to a free scratch register and to
	// GCVM_CONTEXT2_PAGE_TABLE_BASE_ADDR_LO32. measured Apple's SDMA IB
	// fencing while those very registers stayed zero; until this is settled every
	// VMID-2 theory rests on a packet nobody has seen work on this silicon.
	// Both registers are restored through MMIO afterwards. Default OFF.
	uint32_t srbmt = 0;
	ctx.srbmTestWant =
	    PE_parse_boot_argn("garnet-srbm-test", &srbmt, sizeof(srbmt)) && srbmt != 0;
	if (ctx.srbmTestWant)
		RDNA4LOG("stages: rdna4-srbm-test=1 — the SDMA SRBM_WRITE self-test will run "
		       "at the end of SDMAInit (SCRATCH_REG7 and GCVM_CONTEXT2 PT base LO, "
		       "ring and IB, both operand encodings; every leg restores the register)");
	// rdna4-vmfrag-test=1 (0.0.193): INCREMENT 1 of the Apple page-table
	// encoding fix. At the very end of stage 15, build an APPLE-SHAPED
	// tree (DEPTH 1 / BLOCK_SIZE 7, a 256 MiB root entry, 64 KiB L1 entries,
	// 16-entry sub-tables) in OUR VRAM using GFX12 ENTRY ENCODING, put it on
	// GFXHUB CONTEXT1 (VMID 1 — nothing else on this machine uses it), and copy
	// through it with SDMA IBs submitted under VMID 1. CONTEXT1's six registers
	// are saved and restored; Apple's arena is never read or written. Default
	// OFF: with this clear the self-test does not run at all.
	uint32_t vmfragt = 0;
	ctx.vmfragTestWant =
	    PE_parse_boot_argn("garnet-vmfrag-test", &vmfragt, sizeof(vmfragt)) && vmfragt != 0;
	if (ctx.vmfragTestWant)
		RDNA4LOG("stages: rdna4-vmfrag-test=1 — the Apple-shaped-page-table self-test "
		       "will run at the end of SDMAInit on GFXHUB CONTEXT1/VMID 1 "
		       "(saved and restored); Apple's arena is not touched");
	// M4-WS-VMID-VALID: leg (c) of that self-test - the leaf copy again on a base WITHOUT the VALID bit. A SEPARATE boot-arg, honoured
	// only together with rdna4-vmfrag-test; absent (every variant) it is false and the self-test is exactly 0.0.193's.
	uint32_t vmfragnv = 0;
	ctx.vmfragNoValidWant = ctx.vmfragTestWant &&
	    PE_parse_boot_argn("garnet-vmfrag-novalid", &vmfragnv, sizeof(vmfragnv)) && vmfragnv == 1;
	if (ctx.vmfragNoValidWant)
		RDNA4LOG("stages: rdna4-vmfrag-novalid=1 — vmfrag leg (c) will copy through CONTEXT1 with its base WITHOUT the "
		       "VALID bit (retry off for that leg; CONTEXT1 restored after)");
	// NATIVE S1b (0.0.600): the gate for rdna4-native=1 (default OFF; see nativeS1bGate above). Absent, it reads one boot-arg and returns.
	const uint32_t nativeGate = nativeS1bGate(this);
	IOAccelGarnetNativeClient::latchBootArgs();   // 0.0.612: boot-arg garnet-metal-ws is read ONCE here (default absent = OFF); the RDNA4N open policy uses the latched value
	mapDoorbells();   // NBIO doorbell path init (stage 2) needs the BAR2 physical base
	RDNA4LOG("stages: running ladder to %u (%s); smu-full=%d", target,
	       amdgpu::stage_name((amdgpu::BringupStage)target), ctx.smuFullSetup);
	setProperty("Garnet,StageTarget", static_cast<uint64_t>(target), 32);
	// A previous load of this kext this boot already took the GPU through
	// PSPFwLoad? The marker lives on the IOPCIDevice, which outlives our
	// unload, so a kmutil reload adopts the provisioned GPU instead of asking
	// the PSP to load firmware it already holds (which never fences).
	// Has an earlier load of this kext already run the ladder this boot? The
	// marker lives on the IOPCIDevice, which outlives our unload.
	//
	// REFUSE to run again. Measured: unloading is clean (MES
	// REMOVE_QUEUE, engines halted, buffers freed — the machine carries on),
	// but `kmutil load` afterwards restarts the box every time. Not a panic:
	// the SMC reports shutdown cause 5, a clean OS-initiated restart, with no
	// panic report and nothing in our log past stage 2.
	//
	// What it looks like on the monitor says where it happens: the desktop
	// turns green and purple while staying recognisably the same image, then
	// goes black, then the link drops. That is the display engine scanning the
	// same framebuffer bytes through a changed translation — MMHUB — so the
	// culprit is stage 3 (GMCInit) re-programming the page-table base, system
	// aperture and L2/TLB config of the hub the console is scanning through.
	// Skipping the PSP stages (0.0.20's adopt path) could never have helped.
	// Reproduced at the login window and with a user logged in.
	//
	// The honest fix is a GPU reset before re-initialising, which we cannot do
	// while the display lives on this card. So: one ladder per boot. Iteration
	// happens through the user client, which needs no reload.
	ctx.adoptProvisioned = pciDevice && pciDevice->getProperty("Garnet,ProvisionedThisBoot") != nullptr;
	if (ctx.adoptProvisioned) {
		RDNA4LOG("stages: REFUSING to re-run — this boot already took the GPU through the ladder. "
		       "Re-initialising a live GPU (SMU/IMU/RLC) restarts the machine. Reboot to test a "
		       "new build; use the user client to iterate without one.");
		setProperty("Garnet,LadderSkipped", "already-run-this-boot");
		setProperty("Garnet,StageReached", static_cast<uint64_t>(ctx.reached), 32);
		return;
	}

	kern_return_t r = amdgpu::bringup_to(ctx, (amdgpu::BringupStage)target);
	if (pciDevice && ctx.reached >= amdgpu::BringupStage::PSPFwLoad)
		pciDevice->setProperty("Garnet,ProvisionedThisBoot", true);
	setProperty("Garnet,StageReached", static_cast<uint64_t>(ctx.reached), 32);
	setProperty("Garnet,StageReachedName", amdgpu::stage_name(ctx.reached));
	setProperty("Garnet,StageResult", static_cast<uint64_t>(r), 32);
	if (r != kIOReturnSuccess) setProperty("Garnet,StageFailed", amdgpu::stage_name(ctx.failedAt));
	if (ctx.smuOnline) {
		setProperty("SMU,FirmwareVersion", static_cast<uint64_t>(ctx.smuVersion), 32);
		if (ctx.dev && ctx.dev->smuDriverIfVersion != 0u) {
			setProperty("SMU,DriverIfVersion", static_cast<uint64_t>(ctx.dev->smuDriverIfVersion), 32);
			setProperty("SMU,MetricsLayoutVerified", amdgpu::smu_driver_if_verified(ctx.dev->smuDriverIfVersion));
		}
	}
	if (ctx.mesFailed) setProperty("Garnet,MESFailed", true);
	if (ctx.reached >= amdgpu::BringupStage::ComputeDispatch || ctx.failedAt == amdgpu::BringupStage::ComputeDispatch) {
		setProperty("Garnet,ComputeDispatch", ctx.computePassed ? "passed" : "failed");
		setProperty("Garnet,IBTest", ctx.computeResult.ib_test_passed ? "passed" : "failed");
		setProperty("Garnet,ComputeLanesCorrect", static_cast<uint64_t>(ctx.computeResult.lanes_checked - ctx.computeResult.lanes_mismatched), 32);
		setProperty("Garnet,ComputeObserved0", static_cast<uint64_t>(ctx.computeResult.observed[0]), 32);
	}
	if (ctx.reached >= amdgpu::BringupStage::PM4Test || ctx.failedAt == amdgpu::BringupStage::PM4Test) {
		setProperty("Garnet,GFXQueueMappedByMES", ctx.cpLegacyRb ? "legacy-rb" : (ctx.kgqMapped ? "yes" : "no"));
		setProperty("Garnet,CPRingTest", ctx.cpRingTestPassed ? "passed" : "failed");
		setProperty("Garnet,ComputeQueueMapped", ctx.kcqMapped ? "yes" : "no");
		setProperty("Garnet,ComputeQueueTest", ctx.kcqTestPassed ? "passed" : "failed");
		setProperty("Garnet,CPEopTest", ctx.cpEopPassed ? "passed" : "failed");
		setProperty("Garnet,CPWriteDataTest", ctx.cpWriteDataPassed ? "passed" : "failed");
	}
	if (ctx.reached >= amdgpu::BringupStage::SDMAInit) {
		setProperty("Garnet,SDMACopyTest", ctx.sdmaCopyPassed ? "passed" : "failed");
		setProperty("Garnet,SDMACopyMismatched", static_cast<uint64_t>(ctx.sdmaCopy.mismatched), 32);
		// E1 (0.0.418, an internal design note): the SDMA0_DCC_CNTL no-PTE compression clear is the DEFAULT.
		// 0.0.657: the SDMAInit stage already applied it right after `sdma_init_full` (before its own copy test and
		// sweep), so this call is then a logged no-op (gSdmaDccDefaultDone). `rdna4-sdmadcc=0` skips it.
		rdna4_sdmadcc_default();
	}
	publishPDB0On(this, ctx);
	publishSDMAQ1On(this, ctx);
	publishSRBMOn(this, ctx);
	publishVMFragOn(this, ctx);
	// NATIVE S1b (0.0.600): once, after the ladder, only when the gate accepted rdna4-native=1. Every wait inside is bounded at 2 s.
	if (nativeGate == rdna4native::kGateOn) {
		(void)amdgpu::native_s1b_run(ctx);
		publishNativeS1bOn(this);
		if (amdgpu::native_s1b_latched()) publishNativeVramOn(pciDevice, vramMB);   // 0.0.617 (K5): only on a native boot whose self-test latched
	}
	RDNA4LOG("stages: ladder %s — reached %u (%s), result 0x%x", r == kIOReturnSuccess ? "complete" : "stopped",
	       (unsigned)ctx.reached, amdgpu::stage_name(ctx.reached), r);
}
