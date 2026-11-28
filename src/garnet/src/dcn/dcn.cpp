//
//  dcn.cpp — the kext side of Track D stage 1: our own DCN 4.1 display control.
//
//  Three jobs, and nothing else:
//    1. arm the DCN 4.1 layer (src/dcn41) against the DMU segment bases THIS card's IP
//       discovery table reports, with the write allowlist armed against the same bases;
//    2. classify, acknowledge and count DCE interrupts out of our IH ring (T2-VBL);
//    3. answer the two read-mostly verbs `dcnstate` and `dcnvbl`.
//
//  SAFETY. Display programming uses dcn_wreg(). Explicit DPCD queries also call
//  the same-PCI FB's serialized fixed native AUX reads (FIFO/arbitration writes,
//  no sink writes, reset or training). dcn_wreg()
//  calls dcn41_allow_write() first and returns without touching hardware on false. There
//  is no second display-programming path in dcn41: it holds only this callback.
//  The allowlist refuses everything outside the explicit DCN 4.1 display ranges, and
//  refuses the SMN indirect windows and the named SMU/PSP/SMUIO blocks before it even
//  consults those ranges (src/dcn41/dcn41_allow.h). Nothing in this file reaches power,
//  voltage, clock-limit or firmware-ROM state, and T2 writes exactly one field:
//  OTG_GLOBAL_SYNC_STATUS's interrupt enable/ack bits on the lit OTG.
//
#include "dcn_internal.hpp"
#include "dcn41_dmub_boot_pure.h"
#include "dcn41_regs.h"
#include "amd/amdgpu_ucode_extract.h"
#include "fw/fw_table.h"
#include <libkern/c++/OSArray.h>
#include <libkern/c++/OSDictionary.h>
#include <libkern/c++/OSNumber.h>

namespace rdna4dcn {

DcnState gDcn;

ScanState gScan;

rdna4mt::State gMt;
IOLock *gMtLock = nullptr;
volatile uint32_t gMtHoldAlive = 0;   // 0.0.609: hold runner threads not yet finished (scanShutdown waits for 0 before the registers are unmapped)
volatile uint32_t gMtWdAlive = 0;   // watchdog threads not yet finished (scanShutdown waits for 0 before the registers are unmapped)

// build 0.0.515: the read-only raster device (liveraster.h), built by attach() at start().
rdna4lr_ro gLr {};
// Its read callback: a plain BAR5 read through the device context, no allowlist bookkeeping (it is not the armed layer).
uint32_t lr_rreg(void *cookie, uint32_t abs_dword) {
	return amdgpu::RREG32(*static_cast<amdgpu::DeviceContext *>(cookie), abs_dword);
}

uint64_t now_ns() {
	uint64_t abs = mach_absolute_time();
	uint64_t ns = 0;
	absolutetime_to_nanoseconds(abs, &ns);
	return ns;
}

// build 0.0.625: boot-arg garnet-dmubcmd=1 (default OFF) latches the three DMUB command verbs (95 / 96 / 97). Read and latched ONCE (attach() at start() reads it; the first verb call reads it if attach did not); first writer wins.
volatile uint32_t gDmLatch = RDNA4DM_LATCH_UNSET;
bool dm_latched_on() {
	uint32_t l = __atomic_load_n(&gDmLatch, __ATOMIC_ACQUIRE);
	if (l == RDNA4DM_LATCH_UNSET) {
		uint32_t v = 0;
		const bool present = PE_parse_boot_argn("garnet-dmubcmd", &v, sizeof(v));
		uint32_t unset = RDNA4DM_LATCH_UNSET;
		__atomic_compare_exchange_n(&gDmLatch, &unset, rdna4dm_latch_value(present ? 1 : 0, v), false, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE);
		l = __atomic_load_n(&gDmLatch, __ATOMIC_ACQUIRE);
		RDNA4LOG("dmubcmd: boot-arg garnet-dmubcmd %s: the DMUB command verbs (95 dmubsend, 96 dmubmode, 97 dmubctx) are %s for this boot", present ? (v == 1u ? "=1" : "present but not 1") : "absent", rdna4dm_latch_is_on(l) ? "ENABLED" : "OFF");
	}
	return rdna4dm_latch_is_on(l);
}

// build 0.0.631: boot-arg garnet-disp2=1 (default OFF) latches verb 98 `disp2` (the second-display test pattern, stage M4d). Same shape as the dmubcmd latch: read once, first writer wins.
volatile uint32_t gD2Latch = RDNA4D2_LATCH_UNSET;
bool d2_latched_on() {
	uint32_t l = __atomic_load_n(&gD2Latch, __ATOMIC_ACQUIRE);
	if (l == RDNA4D2_LATCH_UNSET) {
		uint32_t v = 0;
		const bool present = PE_parse_boot_argn("garnet-disp2", &v, sizeof(v));
		uint32_t unset = RDNA4D2_LATCH_UNSET;
		__atomic_compare_exchange_n(&gD2Latch, &unset, rdna4d2_latch_value(present ? 1 : 0, v), false, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE);
		l = __atomic_load_n(&gD2Latch, __ATOMIC_ACQUIRE);
		RDNA4LOG("disp2: boot-arg garnet-disp2 %s: verb 98 (disp2 timing|connect|off|status: OTG1 -> DIG2 test pattern) is %s for this boot", present ? (v == 1u ? "=1" : "present but not 1") : "absent", rdna4d2_latch_is_on(l) ? "ENABLED" : "OFF");
	}
	return rdna4d2_latch_is_on(l);
}

// ---- the dcn41 device callbacks -------------------------------------------------------
// rreg is ungated (a read cannot change the card) but counted; wreg is the allowlist gate.

uint32_t dcn_rreg(void *cookie, uint32_t abs_dword) {
	auto *s = static_cast<DcnState *>(cookie);
	dcn41_allow_note_read(&s->allow);
	return amdgpu::RREG32(*s->dev, abs_dword);
}

bool dcn_display_txn_ok(const char *what) {
	if (rdna4displaytxn::held()) return true;
	static uint32_t refused = 0;
	if (__atomic_fetch_add(&refused, 1u, __ATOMIC_RELAXED) < 8u)
		RDNA4LOG("dcn: REFUSED %s: no display transaction is held by this thread", what);
	return false;
}

void dcn_wreg(void *cookie, uint32_t abs_dword, uint32_t value) {
	auto *s = static_cast<DcnState *>(cookie);
	if (!dcn_display_txn_ok("register write")) return;
	if (!dcn41_allow_write(&s->allow, abs_dword, value, s->tag)) {
		const char *why = nullptr;
		const int reason = dcn41_allow_classify(abs_dword, s->allow.mmio_dwords, nullptr, &why);
		RDNA4LOG("dcn: REFUSED write %#010x = %#010x (tag %s) — allowlist says %s: %s",
		       abs_dword, value, s->tag, dcn41_allow_reason_name(reason), why ? why : "(no text)");
		return;
	}
	amdgpu::WREG32(*s->dev, abs_dword, value);
}

void dcn_udelay(void *cookie, uint32_t us) {
	(void)cookie;
	IODelay(us);
}

// ---- which OTG is lit -----------------------------------------------------------------
// Read, never assumed: T1 found OTG0, but the census is a measurement of one
// boot and the GOP is free to pick another pipe. -1 when none is master-enabled.
int lit_otg() {
	if (!gDcn.armed) return -1;
	for (uint32_t i = 0; i < DCN41_NUM_PIPES; i++) {
		bool enabled = false;
		uint32_t w = 0, h = 0;
		if (dcn41_otg_get_active_size(&gDcn.d, i, &enabled, &w, &h) == DCN41_OK && enabled)
			return (int)i;
	}
	return -1;
}



// Synchronous service call retains provider ownership; no raw MMIO callback crosses kexts.
uint32_t dpConnectorSnapshot(uint32_t connector, rdna4_dpcd_snapshot *out, uint64_t deadlineUs) {
	if (!out) return RDNA4_AUX_INVALID;
	*out = {};
	out->version = RDNA4_DPCD_VERSION; out->size = sizeof(*out);
	out->connector = connector; out->status = RDNA4_AUX_UNAVAILABLE;
	auto *pci = gDcn.owner ? OSDynamicCast(IOPCIDevice, gDcn.owner->getProvider()) : nullptr;
	if (!pci) return out->status;
	const uint64_t pciId = pci->getRegistryEntryID();
	rdna4_dpcd_request request {RDNA4_DPCD_VERSION, sizeof(request), connector, 0, pciId, deadlineUs};
	const uint64_t startedUs = now_ns() / 1000u;
	const uint32_t valid = rdna4_dpcd_validate(&request, pciId, startedUs);
	if (valid != RDNA4_AUX_OK) return out->status = valid;
	OSIterator *children = pci->getChildIterator(gIOServicePlane);
	if (!children) return out->status;
	IOService *fb = nullptr;
	while (auto *entry = children->getNextObject()) {
		auto *service = OSDynamicCast(IOService, entry);
		if (service && service->metaCast("AMDGarnetPrimaryFramebuffer") && service->getProvider() == pci) {
			if (fb) { fb->release(); fb = nullptr; break; }
			fb = service; fb->retain();
		}
	}
	children->release();
	if (!fb) return out->status;
	const OSSymbol *function = OSSymbol::withCString(RDNA4_DPCD_FUNCTION);
	if (function) {
		const IOReturn rc = fb->callPlatformFunction(function, false, &request, out, nullptr, nullptr);
		function->release();
		if (rc != kIOReturnSuccess) out->status = RDNA4_AUX_UNAVAILABLE;
	} else out->status = RDNA4_AUX_UNAVAILABLE;
	fb->release();
	if (out->version != RDNA4_DPCD_VERSION || out->size != sizeof(*out) ||
	    out->pciRegistryId != pciId || out->connector != connector ||
	    out->status > RDNA4_AUX_UNAVAILABLE || out->sampledUs >= deadlineUs ||
	    (out->status == RDNA4_AUX_OK && (!out->generation || out->aux >= 4u || out->hpd >= 4u ||
	        out->sampledUs < startedUs || now_ns() / 1000u >= deadlineUs)))
		out->status = RDNA4_AUX_INVALID;
	return out->status;
}

uint32_t dpBoundSnapshot(uint32_t dp, uint32_t dig, uint32_t otg, rdna4_bound_snapshot *out, uint64_t deadlineUs) {
	if (!out) return RDNA4_AUX_INVALID;
	*out = {};
	out->version = 1u; out->size = sizeof(*out);
	out->binding = RDNA4_BIND_INVALID; out->link.status = RDNA4_AUX_UNAVAILABLE;
	auto *pci = gDcn.owner ? OSDynamicCast(IOPCIDevice, gDcn.owner->getProvider()) : nullptr;
	if (!pci) return out->link.status;
	const uint64_t startedUs = now_ns() / 1000u;
	rdna4_bound_request request {1u, sizeof(request), dp, dig, otg, 0, pci->getRegistryEntryID(), deadlineUs};
	const uint32_t valid = rdna4_bound_validate(&request, request.pciRegistryId, startedUs);
	if (valid != RDNA4_AUX_OK) return out->link.status = valid;
	OSIterator *children = pci->getChildIterator(gIOServicePlane);
	if (!children) return out->link.status;
	IOService *fb = nullptr;
	while (auto *entry = children->getNextObject()) {
		auto *service = OSDynamicCast(IOService, entry);
		if (service && service->metaCast("AMDGarnetPrimaryFramebuffer") && service->getProvider() == pci) {
			if (fb) { fb->release(); fb = nullptr; break; }
			fb = service; fb->retain();
		}
	}
	children->release();
	if (!fb) return out->link.status;
	const OSSymbol *function = OSSymbol::withCString(RDNA4_DPCD_BOUND_FUNCTION);
	if (function) {
		const IOReturn rc = fb->callPlatformFunction(function, false, &request, out, nullptr, nullptr);
		function->release();
		if (rc != kIOReturnSuccess) out->link.status = RDNA4_AUX_UNAVAILABLE;
	}
	fb->release();
	const auto &link = out->link;
	if (link.status == RDNA4_AUX_OK &&
	    (out->version != 1u || out->size != sizeof(*out) || out->binding != RDNA4_BIND_OK ||
	     out->dp != dp || out->dig != dig || out->otg != otg ||
	     link.version != RDNA4_DPCD_VERSION || link.size != sizeof(link) ||
	     link.pciRegistryId != request.pciRegistryId || !link.generation || link.connector >= RDNA4_DPCD_PORTS ||
	     link.directDp != 1u || (link.connectorObj & 0xfffff0ffu) != 0x3013u ||
	     link.aux >= 4u || link.hpd >= 4u || rdna4_binding_encoder(link.encoderObj) != dig ||
	     ((out->beControl >> 28) & 7u) != link.hpd || (out->beControl & 7u) ||
	     ((out->beControl >> 8) & 0x7fu) != (1u << dp) || (out->feControl & 7u) != otg || (out->feControl & 0x100000u) ||
	     link.sampledUs < startedUs || link.sampledUs >= deadlineUs || now_ns() / 1000u >= deadlineUs))
		out->link.status = RDNA4_AUX_INVALID;
	if (link.status == RDNA4_AUX_OK && ((out->phyMux & 3u) || (out->portType & 1u))) out->link.status = RDNA4_AUX_INVALID;
	return out->link.status;
}

static rdna4dp::SinkLink dpObserveBound(void *, const rdna4dp::AuxRoute &route) {
	rdna4_bound_snapshot result {};
	const uint32_t status = dpBoundSnapshot(route.dp, route.dig, route.otg, &result, now_ns() / 1000u + 50000u);
	const auto &raw = result.link;
	rdna4dp::SinkLink link;
	if (status == RDNA4_AUX_OK) link = rdna4dp::decodeDpcd(raw.config, sizeof(raw.config), raw.lanes, sizeof(raw.lanes));
	else if (result.binding == RDNA4_BIND_CHANGED) link.status = rdna4dp::SinkStatus::changed;
	else if (status == RDNA4_AUX_BUSY) link.status = rdna4dp::SinkStatus::busy;
	else if (result.binding != RDNA4_BIND_OK && result.binding != RDNA4_BIND_TRANSPORT && result.binding != RDNA4_BIND_INVALID) link.status = rdna4dp::SinkStatus::invalidRoute;
	else switch (status) {
		case RDNA4_AUX_BUSY: link.status = rdna4dp::SinkStatus::busy; break;
		case RDNA4_AUX_DISCONNECTED: link.status = rdna4dp::SinkStatus::disconnected; break;
		case RDNA4_AUX_TIMEOUT: link.status = rdna4dp::SinkStatus::timeout; break;
		case RDNA4_AUX_SHORT: link.status = rdna4dp::SinkStatus::shortRead; break;
		case RDNA4_AUX_DEFER: link.status = rdna4dp::SinkStatus::deferred; break;
		case RDNA4_AUX_CHANGED: link.status = rdna4dp::SinkStatus::changed; break;
		case RDNA4_AUX_NACK: link.status = rdna4dp::SinkStatus::nack; break;
		default: link.status = rdna4dp::SinkStatus::unavailable; break;
	}
	if (status == RDNA4_AUX_OK && link.status == rdna4dp::SinkStatus::observed) {
		link.connector = raw.connector; link.aux = raw.aux; link.hpd = raw.hpd; link.encoderObj = raw.encoderObj;
	}
	RDNA4LOG("dcn-dpcd-bound: DP %u BE %u OTG %u connector %u encoder %#x AUX %u HPD %u BE_CNTL %#x FE_SOURCE %#x PHY_MUX %#x PORT_TYPE %#x binding %u transport %u sink %s configured-kbps %u lanes %u coding %u CR/EQ %u/%u",
	       route.dp, route.dig, route.otg, raw.connector, raw.encoderObj, raw.aux, raw.hpd,
	       result.beControl, result.feControl, result.phyMux, result.portType, result.binding, status, rdna4dp::sinkStatusName(link.status),
	       link.configuredRateKbps, link.configuredLanes, link.configuredCoding, link.clockRecovery, link.channelEq);
	return link;
}

static const rdna4dp::AuxReader kDpBoundReader {nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, dpObserveBound};

static bool dpGatherUnbound() {
	bool observed = false;
	const uint64_t deadline = now_ns() / 1000u + 50000u;
	for (uint32_t i = 0; i < RDNA4_DPCD_PORTS; ++i) {
		rdna4_dpcd_snapshot raw {};
		const uint32_t status = dpConnectorSnapshot(i, &raw, deadline);
		const rdna4dp::SinkLink link = status == RDNA4_AUX_OK
		    ? rdna4dp::decodeDpcd(raw.config, sizeof(raw.config), raw.lanes, sizeof(raw.lanes)) : rdna4dp::SinkLink{};
		RDNA4LOG("dcn-dpcd: connector %u PCI %llu generation %llu AUX %u HPD %u objects %#x/%#x transport %u sink %s configured-kbps %u lanes %u coding %u CR/EQ %u/%u; console-binding UNKNOWN",
		       i, (unsigned long long)raw.pciRegistryId, (unsigned long long)raw.generation,
		       raw.aux, raw.hpd, raw.connectorObj, raw.encoderObj, status,
		       rdna4dp::sinkStatusName(link.status), link.configuredRateKbps, link.configuredLanes,
		       link.configuredCoding, link.clockRecovery, link.channelEq);
		observed = observed || status == RDNA4_AUX_OK;
		if (status == RDNA4_AUX_UNAVAILABLE && !raw.pciRegistryId) break;
		if (now_ns() / 1000u >= deadline) break;
	}
	return observed;
}

uint32_t dpSnapshot(uint32_t consoleOtg, rdna4dp::Snapshot *out, const rdna4dp::AuxReader *reader) {
	if (!out) return 0u;
	*out = rdna4dp::snapshotWithSink(&gLr, consoleOtg, reader ? reader : &kDpBoundReader);
	// Keep per-connector diagnostics available when no console binding was proved.
	if (!reader && out->sink.status != rdna4dp::SinkStatus::observed) (void)dpGatherUnbound();
	return out->status == rdna4dp::Status::ready ? 1u : 0u;
}

uint32_t dpPreflight(uint32_t consoleOtg, const rdna4dp::Timing *target,
                     rdna4dp::Snapshot *source, rdna4dp::Bandwidth *bandwidth, const rdna4dp::AuxReader *reader) {
	if (!source || !bandwidth) return 0u;
	const uint32_t ready = dpSnapshot(consoleOtg, source, reader);
	*bandwidth = rdna4dp::bandwidth(*source, target ? *target : rdna4dp::Timing{});
	return ready;
}

static void dp_log_preflight(uint32_t otg, const rdna4dp::Timing &target, const char *why) {
	rdna4dp::Snapshot s;
	rdna4dp::Bandwidth b;
	(void)dpPreflight(otg, &target, &s, &b);
	RDNA4LOG("dcn-dp %s: status %s active-mask %#x DP %u FE %u OTG %u BE %u PHY %u HPD %u; lanes %u source-trained %u SST %u bpc %u format %u DSC %u FEC enable/active %u/%u; rate-kbps %u coding %u (0 rate/bpc/lanes/format or 4294967295 = unknown)",
	       why, rdna4dp::statusName(s.status), s.activeMask, s.dp, s.dp, s.otg, s.dig, s.phy, s.hpd,
	       s.lanes, s.trained, s.sst, s.bpc, static_cast<uint32_t>(s.format), s.dsc, s.fecEnabled, s.fecActive, s.rateKbps, s.coding);
	RDNA4LOG("dcn-dp %s: sink %s configured-rate-kbps %u lanes %u coding %u training-disabled %u clock-recovery %u channel-eq %u (0 unknown/1 failed/2 passed); rate-from-bound-sink-status %u connector %u AUX %u HPD %u encoder %#x",
	       why, rdna4dp::sinkStatusName(s.sink.status), s.sink.configuredRateKbps, s.sink.configuredLanes,
	       s.sink.configuredCoding, s.sink.trainingDisabled, s.sink.clockRecovery, s.sink.channelEq, s.rateFromSinkStatus, s.sink.connector, s.sink.aux, s.sink.hpd, s.sink.encoderObj);
	RDNA4LOG("dcn-dp %s: target %ux%u totals %ux%u pixel-Hz %llu demand-bps %llu payload-bps %llu decision %s; console rate/coding require verified source-to-sink binding; geometry transition and inverse ownership unimplemented, no write plan",
	       why, target.width, target.height, target.hTotal, target.vTotal, (unsigned long long)target.pixelHz,
	       (unsigned long long)b.requiredBps, (unsigned long long)b.payloadBps, rdna4dp::decisionName(b.decision));
}

void dcn_log_dp_preflight(uint32_t otg, const rdna4dp::Timing &target, const char *why) {
	dp_log_preflight(otg, target, why);
}

// build 0.0.515: NO bind dependency. 0.0.514 went through lit_otg(), which answers -1 unless
// gDcn.armed (set only by bind, i.e. only after a DCN verb 74-77 ran), so a native-1440p boot's first LINKCFG reply used the CEA
// fallback. The raster now comes from the READ-ONLY device attach() built at start() (liveraster.h: its write callback
// writes nothing; the two readers read OTG_CONTROL, OTG_H/V_BLANK_START_END and OTG_H/V_TOTAL only). Same reads, same answers.
uint32_t liveRaster(uint32_t *hAct, uint32_t *vAct, uint32_t *hTot, uint32_t *vTot, uint32_t *hFront, uint32_t *hSync,
                    uint32_t *vFront, uint32_t *vSync, uint64_t *pixelClockHz) {
	return rdna4lr_live(&gLr, hAct, vAct, hTot, vTot, hFront, hSync, vFront, vSync, pixelClockHz);
}

// build 0.0.542: `accel scanout full`'s display read - the read-only device only (never gDcn, never a write).
uint32_t roScanSurface(rdna4_sf_dcn *s) { return rdna4lr_scan_surface(&gLr, s); }

static bool addDalSnapshotNumber(OSDictionary &dict, const char *key, uint32_t value) {
	OSNumber *number = OSNumber::withNumber(value, 32);
	if (!number) return false;
	const bool ok = dict.setObject(key, number);
	number->release();
	return ok;
}

static bool addDalSnapshotRow(OSDictionary &root, const rdna4dal::SnapshotClock &clock,
                              const rdna4dal::ClockSnapshotRow &value) {
	OSDictionary *row = OSDictionary::withCapacity(5);
	OSArray *levels = OSArray::withCapacity(value.count);
	bool ok = row && levels;
	for (uint32_t index = 0; ok && index < value.count; ++index) {
		OSNumber *frequency = OSNumber::withNumber(value.kHz[index], 32);
		ok = frequency && levels->setObject(frequency);
		if (frequency) frequency->release();
	}
	if (ok) {
		ok = row->setObject("LevelsKHz", levels) &&
		     addDalSnapshotNumber(*row, "RawCount", value.rawCount) &&
		     addDalSnapshotNumber(*row, "FineGrained", value.fineGrained);
		if (ok && clock.id != rdna4dal::kClkDppclk)
			ok = addDalSnapshotNumber(*row, "DcMaxKHz", value.dcMaxKHz) &&
			     addDalSnapshotNumber(*row, "RawDcMax", value.rawDcMax);
		if (ok) ok = root.setObject(clock.name, row);
	}
	if (levels) levels->release();
	if (row) row->release();
	return ok;
}

static void publishDalClockSnapshot(GarnetDriver *owner) {
	const auto *dev = owner->deviceContext();
	if (!dev || !dev->smuOnline) {
		RDNA4LOG("dmcub-fw: DAL clock snapshot unavailable: SMU not online");
		return;
	}
	const uint32_t timer = dcn41_abs_rd(&gLr.d, rdna4dal::kDchubTimerOffset, 2u);
	if (timer == DCN41_BAD_OFFSET) return;
	auto *snapshot = static_cast<rdna4dal::ClockSnapshot *>(IOMalloc(sizeof(rdna4dal::ClockSnapshot)));
	if (!snapshot) return;
	if (amdgpu::smu_dal_query_clock_snapshot(*dev, timer, *snapshot) == kIOReturnSuccess) {
		OSDictionary *root = OSDictionary::withCapacity(11);
		bool ok = root && addDalSnapshotNumber(*root, "UmcChannels", snapshot->umcChannels) &&
		    addDalSnapshotNumber(*root, "Clk0PllReq", snapshot->references.pllRequest) &&
		    addDalSnapshotNumber(*root, "Clk0DpRefDfs", snapshot->references.dpRefDfs) &&
		    addDalSnapshotNumber(*root, "DchubGlobalTimerCntl", snapshot->references.dchubTimer);
		for (const auto &clock : rdna4dal::kSnapshotClocks) {
			if (!ok) break;
			ok = addDalSnapshotRow(*root, clock, snapshot->*clock.row);
		}
		if (ok && owner->setProperty("garnet-dal-clock-snapshot", root)) {
			RDNA4LOG("dmcub-fw: recorded DAL clock snapshot (7 clocks, %u UMC channels; query-only)", snapshot->umcChannels);
		}
		if (root) root->release();
	}
	IOFree(snapshot, sizeof(rdna4dal::ClockSnapshot));
}

// Report only the first firmware refusal; an unread prerequisite is not a claim about hardware state.
static void preflightDalDmub(GarnetDriver *owner, bool hasDcn401) {
	uint32_t optIn = 0;
	const bool present = PE_parse_boot_argn("garnet-dmcub-fw", &optIn, sizeof(optIn));
	if (!rdna4dmub::dmcub_fw_opt_in(present, optIn)) return;
	if (hasDcn401) publishDalClockSnapshot(owner);

	const auto *firmware = amdgpu::fw_get(amdgpu::FwId::DCN_DMUB);
	if (!firmware || !firmware->data || !firmware->size) {
		RDNA4LOG("dmcub-fw: preflight refused: DAL firmware not embedded; no STOP or PSP load");
		return;
	}
	amdgpu::UcodePayload payloads[amdgpu::kMaxUcodePayloadsPerFile];
	dcn41_dmub_fw_meta_info metadata {};
	if (amdgpu::amdgpu_ucode_extract(amdgpu::kHostFwFile_DCN_DMUB, firmware->data, firmware->size, payloads) != 1u ||
	    dcn41_dmub_fw_meta_from_psp(firmware->data + payloads[0].offset_bytes, payloads[0].size_bytes, &metadata, nullptr) != DCN41_OK ||
	    metadata.dal_fw != 1u) {
		RDNA4LOG("dmcub-fw: preflight refused: malformed DAL firmware payload; no STOP or PSP load");
		return;
	}
	dcn41_dmub_state current {};
	if (gLr.state != RDNA4LR_READY || dcn41_dmub_probe(&gLr.d, &current) != DCN41_OK ||
	    current.dal_fw || current.verdict != DCN41_DMUB_ALIVE_IDLE || current.ring_map == DCN41_DMUB_MAP_UNKNOWN) {
		RDNA4LOG("dmcub-fw: preflight refused: no known idle VBIOS mailbox; no STOP or PSP load");
		return;
	}
	const auto *bringup = owner->bringupContext();
	if (!bringup || bringup->dev != owner->deviceContext() || !bringup->psp.ringCreated || !bringup->psp.tmrSetUp ||
	    !bringup->gmc.vram_alloc.is_inited()) {
		RDNA4LOG("dmcub-fw: preflight refused: PSP or VRAM allocator unavailable; no STOP or PSP load");
		return;
	}
	if (!dcn41_dmub_ring_in_vram_reservation(&current, bringup->dev->vramSizeBytes,
	                                     bringup->psp.fwReserveExtVramOffset, bringup->psp.fwReserveExtSize)) {
		RDNA4LOG("dmcub-fw: preflight refused: VBIOS inbox not confirmed in PSP extended reservation; no STOP or PSP load");
		return;
	}
	const uint32_t cw0 = dcn41_abs(&gLr.d, DCN41_DMCUB_REGION3_CW0_TOP_ADDRESS, DCN41_DMCUB_REGION3_CW0_TOP_ADDRESS_BASE_IDX);
	const uint32_t cw1 = dcn41_abs(&gLr.d, DCN41_DMCUB_REGION3_CW1_TOP_ADDRESS, DCN41_DMCUB_REGION3_CW1_TOP_ADDRESS_BASE_IDX);
	if (cw0 == DCN41_BAD_OFFSET || cw1 == DCN41_BAD_OFFSET) {
		RDNA4LOG("dmcub-fw: preflight refused: cannot resolve current firmware windows; no STOP or PSP load");
		return;
	}
	const uint32_t codeWindow = gLr.d.rreg(gLr.d.cookie, cw0);
	const uint32_t stackWindow = gLr.d.rreg(gLr.d.cookie, cw1);
	RDNA4LOG("dmcub-fw: preflight refused: VBIOS inbox within PSP extended reservation, but firmware recovery unverified (DAL version=%#x, CW0_TOP=%#x, CW1_TOP=%#x, inbox_map=%u); no STOP or PSP load",
	         metadata.fw_version, codeWindow, stackWindow, static_cast<unsigned>(current.ring_map));
}

// Build the read-only raster device from the same DMU bases bind() uses. Only the opt-in DMUB preflight reads registers here.
void attach(GarnetDriver *owner) {
	(void)dm_latched_on();                                   // 0.0.625: latch garnet-dmubcmd at start (no register, no memory touched)
	(void)d2_latched_on();                                   // 0.0.631: latch garnet-disp2 at start (no register, no memory touched)
	if (!owner || gLr.state != RDNA4LR_NONE) return;
	amdgpu::DeviceContext *dev = owner->deviceContext();
	if (!dev || !dev->rmmio || dev->rmmioSize == 0) {
		RDNA4LOG("dcn: read-only raster path NOT BUILT - no mapped BAR5 in the device context (the LINKCFG reply keeps the CEA fallback)");
		return;
	}
	const IpDiscovery &disc = owner->ipDisc();
	IpDiscovery::IpEntry e {};
	if (!disc.isValid() || !disc.findIp(IpDiscovery::HwDmu, 0, e) || e.numBases < DCN41_NUM_SEGS) {
		RDNA4LOG("dcn: read-only raster path NOT BUILT - no DMU (hwId 271) entry with %u segments in discovery", DCN41_NUM_SEGS);
		return;
	}
	uint32_t seg[DCN41_NUM_SEGS];
	for (uint32_t i = 0; i < DCN41_NUM_SEGS; i++) seg[i] = e.bases[i];
	const uint32_t ok = rdna4lr_ro_build(&gLr, dev, lr_rreg, seg, (uint32_t)(dev->rmmioSize / 4));
	RDNA4LOG("dcn: read-only raster path %s (dcn41_dev_init %d; segs %08x %08x %08x %08x %08x; no register read or written here)",
	       ok ? "BUILT" : "NOT BUILT", gLr.init_rc, seg[0], seg[1], seg[2], seg[3], seg[4]);
	preflightDalDmub(owner, e.major == 4u && e.minor == 1u && e.revision == 0u);
}

static uint32_t bind_locked(GarnetDriver *owner);
// build 0.0.603: bind() is serialised - the RDNA4N selectors and the legacy accel verbs can both reach it concurrently on a native boot.
uint32_t bind(GarnetDriver *owner) {
	if (__atomic_load_n(&gDcn.armed, __ATOMIC_ACQUIRE)) return 0;
	static IOLock *bindLock = nullptr;
	if (bindLock == nullptr) {
		IOLock *nl = IOLockAlloc();
		if (nl != nullptr && !OSCompareAndSwapPtr(nullptr, nl, (void *volatile *)&bindLock)) IOLockFree(nl);
	}
	if (bindLock == nullptr) return 1;
	IOLockLock(bindLock);
	const uint32_t r = bind_locked(owner);
	IOLockUnlock(bindLock);
	return r;
}
static uint32_t bind_locked(GarnetDriver *owner) {
	if (gDcn.armed) return 0;
	if (!owner) return 1;

	gDcn.owner = owner;
	gDcn.dev = owner->deviceContext();
	if (!gDcn.dev || !gDcn.dev->rmmio || gDcn.dev->rmmioSize == 0) {
		RDNA4LOG("dcn: no mapped BAR5 — display layer stays INERT");
		return 2;
	}

	// The segment bases come from the card, not from a constant: IP discovery hwId 271
	// (DMU) instance 0.
	const IpDiscovery &disc = owner->ipDisc();
	if (!disc.isValid()) {
		RDNA4LOG("dcn: IP discovery is not valid — display layer stays INERT (no segment bases)");
		return 3;
	}
	IpDiscovery::IpEntry e {};
	if (!disc.findIp(IpDiscovery::HwDmu, 0, e) || e.numBases < DCN41_NUM_SEGS) {
		RDNA4LOG("dcn: no DMU (hwId 271) entry with %u segments in discovery — display layer stays INERT",
		       DCN41_NUM_SEGS);
		return 4;
	}
	for (uint32_t i = 0; i < DCN41_NUM_SEGS; i++) gDcn.seg[i] = e.bases[i];

	const uint32_t mmioDwords = (uint32_t)(gDcn.dev->rmmioSize / 4);

	// Arm the allowlist FIRST. If it refuses the bases, nothing else is armed and every
	// write stays refused — deny by default, all the way up.
	const int ar = dcn41_allow_init(&gDcn.allow, gDcn.seg, mmioDwords);
	if (ar != DCN41_ALLOW_OK) {
		RDNA4LOG("dcn: the write allowlist REFUSED to arm (%s) for segs %08x %08x %08x %08x %08x, "
		       "BAR5 %u dwords — display layer stays INERT",
		       dcn41_allow_reason_name(ar), gDcn.seg[0], gDcn.seg[1], gDcn.seg[2], gDcn.seg[3],
		       gDcn.seg[4], mmioDwords);
		gDcn.initStatus = 5;
		return 5;
	}

	const int dr = dcn41_dev_init(&gDcn.d, &gDcn, dcn_rreg, dcn_wreg, dcn_udelay, gDcn.seg,
	                              mmioDwords, 0);
	if (dr != DCN41_OK) {
		RDNA4LOG("dcn: dcn41_dev_init failed %d — display layer stays INERT", dr);
		gDcn.initStatus = 6;
		return 6;
	}
	// Flip addresses are confined to the display's own frame-buffer window as the hardware
	// reports it (T1: BASE 0x8000, TOP 0x83fb -> MC 0x80_0000_0000..0x83_ffff_ffff).
	{
		const uint32_t fbBase = amdgpu::RREG32(*gDcn.dev, gDcn.seg[2] + 0x475u);
		const uint32_t fbTop  = amdgpu::RREG32(*gDcn.dev, gDcn.seg[2] + 0x476u);
		if (fbTop > fbBase)
			(void)dcn41_dev_set_scanout_window(&gDcn.d, (uint64_t)fbBase << 24,
			                                   ((uint64_t)fbTop + 1u) << 24);
	}

	__atomic_store_n(&gDcn.armed, true, __ATOMIC_RELEASE);
	gDcn.initStatus = 0;
	RDNA4LOG("dcn: display layer ARMED — segs %08x %08x %08x %08x %08x, BAR5 %u dwords, "
	       "allowlist %u ranges / %u writable dwords (deny by default)",
	       gDcn.seg[0], gDcn.seg[1], gDcn.seg[2], gDcn.seg[3], gDcn.seg[4], mmioDwords,
	       dcn41_allow_range_count(), dcn41_allow_total_dwords());
	// build 0.0.603 (S2a review MUST-FIX): on a NATIVE boot the golden timing copy is taken HERE, at bind - before any native
	// call can write a display register - so `dcnmode 0` (exempt from the native refusal) always restores the boot capture, not whatever
	// the first dcnmode call happened to find. Reads only; a non-native boot takes it lazily in mode(), exactly as before.
	if (!gDcn.goldenValid && amdgpu::native_s1b_state().gate == rdna4native::kGateOn) {
		const int gOtg = lit_otg();
		if (gOtg >= 0 && dcn41_otg_read_timing(&gDcn.d, (uint32_t)gOtg, &gDcn.golden) == DCN41_OK) {
			gDcn.goldenValid = true;
			RDNA4LOG("dcn: GOLDEN timing capture taken at bind on a native boot (OTG %d)", gOtg);
		}
	}
	// build 0.0.605 (S2d review MUST-FIX): the FULL golden copy - every register a mode trial can write - is taken here on a native boot, read-only, before any native
	// call can write a display register. `dcnmode 0` and the trial's restore both put THIS copy back.
	mt_golden_at_bind();
	return 0;
}

// ---- T3-FLIP ---------------------------------------------------------------------------
//
// GEOMETRY IS READ, NOT ASSUMED. This matters more here than anywhere else: the lit RASTER is
// 2560x1440 (T1) but the PLANE the HUBP fetches is 1920x1080 at pitch 1920, ARGB8888,
// SW_MODE 0 (linear), and the DPP scaler (DSCL_MODE 1) upscales it. A buffer shaped like the raster
// would be read as 1920x1080 at pitch 1920 anyway, because those registers are the HUBP's and we do
// not touch them. So the buffer is sized and strided from HUBP0's OWN viewport, pitch and format
// registers, every one of them re-read on each prepare.
//
// THE RESTORE IS GUARANTEED ON EVERY PATH:
//   1. one exit path - every failure inside flip() goes to the same restore;
//   2. `flipHeld` persists in module state, and dcnstate, dcnvbl AND dcnflip all restore first if it
//      is set, so the very next call of any of the three puts the console back;
//   3. the hold is IOSleep in <= 250 ms slices under a hard cap, so no argument can hold longer;
//   4. an INDEPENDENT INTERRUPT WATCHDOG: the flip enables VUPDATE_NO_LOCK (proven on hardware in
//     ) and counts frames while held; past heldFrameCap the IH handler itself reprograms the
//      console address and says so. That is a real watchdog on the work loop, not a promise;
//   5. `dcnflip 0` restores unconditionally and is idempotent, callable at any time.
// A panic is the one thing none of these survive, and a panic reboots, after which the GOP relights.

/* Absolute address of an OTG-instanced register from its instance-0 offset. OTG stride is 0x80
 * (regOTG0_OTG_CRC_CNTL 0x1b65, regOTG1_OTG_CRC_CNTL 0x1be5, dcn_4_1_0_offset.h). */
uint32_t otg_reg(uint32_t off0, uint32_t otg) { return gDcn.seg[2] + off0 + 0x80u * otg; }
/* HUBP/HUBPREQ stride is 0xdc (regHUBP0_DCSURF_SURFACE_CONFIG 0x5e5, HUBP1 0x6c1). */
uint32_t hubp_reg(uint32_t off0, uint32_t hubp) { return gDcn.seg[2] + off0 + 0xdcu * hubp; }


void crc_enable(uint32_t otg, uint32_t w, uint32_t h) {
    gDcn.tag = "crcset";
    /* optc1_configure_crc order: the four windows, then CONT_EN + SELECT + EN in one update. */
    dcn_wreg(&gDcn, otg_reg(kOffCrcWinAX, otg), (0u & 0x7FFFu) | ((w & 0x7FFFu) << 16));
    dcn_wreg(&gDcn, otg_reg(kOffCrcWinAY, otg), (0u & 0x7FFFu) | ((h & 0x7FFFu) << 16));
    dcn_wreg(&gDcn, otg_reg(kOffCrcWinBX, otg), (0u & 0x7FFFu) | ((w & 0x7FFFu) << 16));
    dcn_wreg(&gDcn, otg_reg(kOffCrcWinBY, otg), (0u & 0x7FFFu) | ((h & 0x7FFFu) << 16));
    uint32_t v = dcn_rreg(&gDcn, otg_reg(kOffCrcCntl, otg));
    v &= ~(0x00000001u | 0x00000010u | 0x00700000u);
    v |= 0x00000010u;                   /* OTG_CRC_CONT_EN  (mask 0x10,  shift 4)  */
    v |= (0u << 20) & 0x00700000u;      /* OTG_CRC0_SELECT = 0                      */
    v |= 0x00000001u;                   /* OTG_CRC_EN       (mask 0x1,   shift 0)  */
    dcn_wreg(&gDcn, otg_reg(kOffCrcCntl, otg), v);
}

void crc_disable(uint32_t otg) {
    gDcn.tag = "crcoff";
    uint32_t v = dcn_rreg(&gDcn, otg_reg(kOffCrcCntl, otg));
    v &= ~(0x00000001u | 0x00000010u);
    dcn_wreg(&gDcn, otg_reg(kOffCrcCntl, otg), v);
}

void crc_read(uint32_t otg, uint32_t *rg, uint32_t *b) {
    if (rg) *rg = dcn_rreg(&gDcn, otg_reg(kOffCrcDataRG, otg));
    if (b)  *b  = dcn_rreg(&gDcn, otg_reg(kOffCrcDataB, otg));
}

/* Wait n frames on the lit OTG by watching its own frame counter; bounded, never an open loop. */
void wait_frames(uint32_t otg, uint32_t n) {
    uint32_t start = 0, cur = 0;
    if (dcn41_otg_get_frame_count(&gDcn.d, otg, &start) != DCN41_OK) { IOSleep(20 * n); return; }
    for (uint32_t i = 0; i < 40u * n; i++) {          /* 40 x 1 ms per frame is ~2.4x a 60 Hz frame */
        IOSleep(1);
        if (dcn41_otg_get_frame_count(&gDcn.d, otg, &cur) != DCN41_OK) return;
        if (dcn41_frame_delta(start, cur) >= n) return;
    }
}

/* Measure the refresh over `ms` by the OTG's own frame counter and a monotonic clock. Returns
 * millihertz; 0 if anything went wrong. This is the witness that a timing change is REAL. */
uint64_t measure_refresh_mhz(uint32_t otg, uint32_t ms, uint32_t *frames_out) {
    uint32_t f0 = 0, f1 = 0;
    if (frames_out) *frames_out = 0;
    if (dcn41_otg_get_frame_count(&gDcn.d, otg, &f0) != DCN41_OK) return 0;
    const uint64_t t0 = now_ns();
    IOSleep(ms);
    if (dcn41_otg_get_frame_count(&gDcn.d, otg, &f1) != DCN41_OK) return 0;
    const uint64_t t1 = now_ns();
    const uint32_t frames = dcn41_frame_delta(f0, f1);
    if (frames_out) *frames_out = frames;
    if (t1 <= t0) return 0;
    return dcn41_refresh_mhz(f0, f1, t1 - t0);
}

/* Program the plane address. Returns the dcn41 status. */
int flip_program(uint64_t mc, const char *tag) {
    const int layout = dcn41_hubp_require_linear(&gDcn.d, gDcn.flipHubp);
    if (layout != DCN41_OK) return layout;
    gDcn.tag = tag;
    return dcn41_hubp_program_flip(&gDcn.d, gDcn.flipHubp, mc, 0u, false, false);
}

/* THE restore. Unconditional, idempotent, and the only way the console address is put back. */
void flip_restore(const char *why) {
    if (!gDcn.flipPrepared || gDcn.origMc == 0) return;
    const int r = flip_program(gDcn.origMc, "flipback");
    if (r == DCN41_OK) {
        gDcn.flipHeld = false;
        gDcn.heldFrames = 0;
        gDcn.flipsOut++;
    }
    RDNA4LOG("dcn-flip: RESTORE (%s) -> console plane %#llx, status %d",
           why, (unsigned long long)gDcn.origMc, r);
}

void flip_restore_if_held(const char *why) {
    if (gDcn.flipHeld) {
        gDcn.watchdogRestores++;
        flip_restore(why);
    }
}

/* Read the plane geometry off the hardware and build the test buffer. Idempotent. */
uint32_t flip_prepare() {
    if (gDcn.flipPrepared) return 0;
    if (!gDcn.armed) return 1;

    const int otg = lit_otg();
    if (otg < 0) { RDNA4LOG("dcn-flip: no OTG is master-enabled — REFUSING"); return 2; }
    gDcn.flipOtg = (uint32_t)otg;
    gDcn.flipHubp = (uint32_t)otg;      /* T1: OTG0 + HUBP0; HUBP_VTG_SEL ties HUBPn to OTGn */
    if (dcn41_hubp_require_linear(&gDcn.d, gDcn.flipHubp) != DCN41_OK) return 5;

    uint64_t cur = 0;
    if (dcn41_hubp_read_primary_addr(&gDcn.d, gDcn.flipHubp, &cur) != DCN41_OK || cur == 0) {
        RDNA4LOG("dcn-flip: could not read HUBP%u's primary address (got %#llx) — REFUSING",
               gDcn.flipHubp, (unsigned long long)cur);
        return 3;
    }
    gDcn.origMc = cur;

    const uint32_t dim   = dcn_rreg(&gDcn, hubp_reg(kOffViewportDim,  gDcn.flipHubp));
    const uint32_t pitch = dcn_rreg(&gDcn, hubp_reg(kOffSurfPitch,    gDcn.flipHubp));
    const uint32_t cfg   = dcn_rreg(&gDcn, hubp_reg(kOffSurfConfig,   gDcn.flipHubp));
    const uint32_t tile  = dcn_rreg(&gDcn, hubp_reg(kOffTilingConfig, gDcn.flipHubp));
    gDcn.fbWidth   = dim & 0xFFFFu;             /* PRI_VIEWPORT_WIDTH  */
    gDcn.fbHeight  = (dim >> 16) & 0xFFFFu;     /* PRI_VIEWPORT_HEIGHT */
    gDcn.fbPitchPx = (pitch & 0xFFFFu) + 1u;    /* hubp401 writes surface_pitch - 1 */
    gDcn.fbPixFmt  = cfg & 0x7Fu;               /* SURFACE_PIXEL_FORMAT */
    gDcn.fbSwMode  = (tile >> 0) & 0x1Fu;       /* SW_MODE */

    if (gDcn.fbWidth == 0 || gDcn.fbHeight == 0 || gDcn.fbPitchPx < gDcn.fbWidth ||
        gDcn.fbWidth > 8192u || gDcn.fbHeight > 8192u || gDcn.fbPitchPx > 16384u) {
        RDNA4LOG("dcn-flip: implausible geometry %ux%u pitch %u — REFUSING",
               gDcn.fbWidth, gDcn.fbHeight, gDcn.fbPitchPx);
        return 4;
    }
    if (gDcn.fbPixFmt != 8u && gDcn.fbPixFmt != 10u) {
        RDNA4LOG("dcn-flip: SURFACE_PIXEL_FORMAT %u is not a 32-bit ARGB/ABGR format — REFUSING "
               "(the pattern writer only knows 8888)", gDcn.fbPixFmt);
        return 5;
    }

    amdgpu::BringupContext *bc = gDcn.owner ? gDcn.owner->bringupContext() : nullptr;
    if (!bc || !bc->gmc.vram_alloc.is_inited()) { RDNA4LOG("dcn-flip: no VRAM allocator"); return 6; }
    const uint64_t bytes = (uint64_t)gDcn.fbPitchPx * gDcn.fbHeight * 4ull;
    amdgpu::VRAMAllocation a {};
    if (!bc->gmc.vram_alloc.alloc(bytes, 65536, &a)) {
        RDNA4LOG("dcn-flip: could not allocate %llu bytes for the test plane", (unsigned long long)bytes);
        return 7;
    }
    gDcn.testMc = a.gpu_va;
    gDcn.testBytes = bytes;
    // gmc_vram_alloc_init: alloc_base_mc = gmc.vram_start + vram byte offset, and
    // "gpu_va - vram_start = BAR0 offset". Use the allocator's OWN base, and refuse if the two
    // records of where VRAM starts disagree rather than filling the wrong memory.
    if (bc->gmc.vram_start != gDcn.dev->vramMcBase || a.gpu_va < bc->gmc.vram_start) {
        RDNA4LOG("dcn-flip: gmc.vram_start %#llx vs dev.vramMcBase %#llx vs gpu_va %#llx disagree — REFUSING",
               (unsigned long long)bc->gmc.vram_start, (unsigned long long)gDcn.dev->vramMcBase,
               (unsigned long long)a.gpu_va);
        bc->gmc.vram_alloc.free(a);
        return 10;
    }
    gDcn.testVramOff = a.gpu_va - bc->gmc.vram_start;

    /* The buffer must be inside the frame-buffer window the hardware reports, and inside the BAR0
     * aperture we are about to write it through. Both checked before a single byte is written. */
    if (!amdgpu::bar0_range_ok(*gDcn.dev, gDcn.testVramOff, bytes)) {
        RDNA4LOG("dcn-flip: the allocation (vram+%#llx, %llu bytes) is outside the %zu-byte BAR0 "
               "aperture — REFUSING", (unsigned long long)gDcn.testVramOff,
               (unsigned long long)bytes, gDcn.dev->bar0Size);
        return 8;
    }
    if (gDcn.testMc == gDcn.origMc) { RDNA4LOG("dcn-flip: allocation aliases the console — REFUSING"); return 9; }

    /* The pattern. Linear ARGB8888 (SW_MODE 0 confirmed above), so a row is fbPitchPx dwords and
     * memory order is the screen order: four full-width horizontal bands, top to bottom
     * RED, GREEN, BLUE, WHITE, each a quarter of the plane's height. The scaler stretches the
     * 1920x1080 plane over the 2560x1440 raster, so the user sees four bands filling the screen. */
    static const uint32_t kBand[4] = { 0xFFFF0000u, 0xFF00FF00u, 0xFF0000FFu, 0xFFFFFFFFu };
    volatile uint32_t *px = (volatile uint32_t *)(gDcn.dev->bar0 + gDcn.testVramOff);
    for (uint32_t y = 0; y < gDcn.fbHeight; y++) {
        const uint32_t c = kBand[(y * 4u) / gDcn.fbHeight > 3u ? 3u : (y * 4u) / gDcn.fbHeight];
        volatile uint32_t *row = px + (uint64_t)y * gDcn.fbPitchPx;
        for (uint32_t x = 0; x < gDcn.fbPitchPx; x++) row[x] = c;
    }
    amdgpu::storeFence();

    gDcn.flipPrepared = true;
    RDNA4LOG("dcn-flip: PREPARED — plane read from HUBP%u: %ux%u pitch %u px, SURFACE_PIXEL_FORMAT %u, "
           "SW_MODE %u; console plane %#llx; test plane %#llx (vram+%#llx, %llu bytes) filled with "
       "4 bands R/G/B/W; OTG %u",
       gDcn.flipHubp, gDcn.fbWidth, gDcn.fbHeight, gDcn.fbPitchPx, gDcn.fbPixFmt, gDcn.fbSwMode,
       (unsigned long long)gDcn.origMc, (unsigned long long)gDcn.testMc,
       (unsigned long long)gDcn.testVramOff, (unsigned long long)bytes, gDcn.flipOtg);
    return 0;
}

// ---- the IH path ----------------------------------------------------------------------

// build 0.0.603: defined with the rest of the scanout code (dcn_scanout_mt.cpp). Called with scan.active set; takes the scanout
// lock, acknowledges the interrupt inside it, does the latch / storm bookkeeping. Returns true when it acknowledged (*ar = the status).
void scanEscape(const char *why);     // the public functions below are declared in dcn.hpp; repeated here because ihEntry precedes them
void scanLogState();

bool ihEntry(uint32_t clientId, uint32_t srcId, uint32_t srcData0) {
	if (clientId != DCN41_IH_CLIENT_DCE) return false;
	if (!gDcn.armed) {
		// Still ours: report it as handled so the caller does not double-count, but we
		// cannot acknowledge it without the register layer.
		gDcn.dceEntries++;
		gDcn.dceUnclassified++;
		return true;
	}
	bool notifyPrimaryVbl = false;
	uint32_t notifyPrimaryOtg = 0u;
	bool notifyPrimaryHpd = false;
	uint32_t notifyPrimaryHpdPin = 0u;
	bool notifyPrimaryHpdShort = false;
	bool notifyPrimaryHpdSense = false;
	{
		// The work-loop entry is a complete, non-blocking attempt at the same local
		// display transaction every other DCN write path takes.  If another thread
		// owns the transaction, consume the entry without touching DCN registers:
		// no ACK, no plane write and no source disable.  Returning true only says
		// this DCE entry was consumed rather than falling through to the generic IH
		// accounting; it is not an ACK-success result.
		rdna4_display_txn txn(rdna4peer::kLeaseLocalDisplay);
		if (!txn.ok) {
			gDcn.dceEntries++;
			gDcn.irqRefusals++;
			// Mark ACK recovery so the watchdog can un-wedge a latched DCN level interrupt.
			__atomic_store_n(&gDcn.irqAckRecoveryNeeded, true, __ATOMIC_RELEASE);
			__atomic_store_n(&gDcn.hpdAckRecoveryNeeded, true, __ATOMIC_RELEASE);
			if (gDcn.irqRefusalLogs < 8u) {
				gDcn.irqRefusalLogs++;
				RDNA4LOG("dcn-irq: REFUSED src %u data %08x: no display transaction; entry consumed without ACK or register write",
				       srcId, srcData0);
			}
			return true;
		}

		const struct dcn41_irq irq = dcn41_ih_to_irq(clientId, srcId, srcData0);
		const uint64_t t = now_ns();
		gDcn.dceEntries++;
		if (gDcn.firstEntryNs == 0) gDcn.firstEntryNs = t;
		gDcn.lastEntryNs = t;

		if (irq.kind == DCN41_IRQ_NONE) {
			gDcn.dceUnclassified++;
			if (gDcn.logBudget > 0) {
				gDcn.logBudget--;
				RDNA4LOG("dcn-irq: DCE entry src %u data %08x did not classify", srcId, srcData0);
			}
			return true;
		}

		if (irq.kind < DCN41_IRQ_KIND_COUNT) gDcn.dceByKind[irq.kind]++;
		if (irq.kind == DCN41_IRQ_VUPDATE_NO_LOCK && irq.inst < DCN41_NUM_PIPES)
			gDcn.dceVupdate[irq.inst]++;
		if (irq.kind == DCN41_IRQ_VSTARTUP && irq.inst < DCN41_NUM_PIPES)
			gDcn.dceVstartup[irq.inst]++;

		// T3-FLIP's INDEPENDENT WATCHDOG. While our test plane is on screen the flip enables
		// VUPDATE_NO_LOCK, so this runs once per frame on the work loop: past the cap the handler
		// reprograms the console address itself, without any userspace call. This is the answer to
		// "a wedged step still restores the original plane address".
		if (gDcn.flipHeld && irq.kind == DCN41_IRQ_VUPDATE_NO_LOCK) {
			gDcn.heldFrames++;
			if (gDcn.heldFrames > gDcn.heldFrameCap) {
				gDcn.watchdogRestores++;
				RDNA4LOG("dcn-flip: WATCHDOG — held %u frames, cap %u; restoring the console plane from "
				       "the interrupt handler", gDcn.heldFrames, gDcn.heldFrameCap);
				flip_restore("interrupt watchdog");
			}
		}

		// Acknowledge BEFORE anything else, as amdgpu_dm_irq_handler does (it calls
		// dc_interrupt_ack first for every DCN source). The ack is a register write and goes
		// through the allowlist like every other.
		// build 0.0.603: while a native client holds the plane, the ack AND the scanout bookkeeping run under the scanout lock (the
		// ack is a read-modify-write of OTG_GLOBAL_SYNC_STATUS, which Acquire / restore also modify); otherwise exactly the old two lines.
		int ar = 0;
		if (__atomic_load_n(&gScan.active, __ATOMIC_ACQUIRE) == 0u || !scanIrq(irq, t, &ar)) {
			gDcn.tag = "irqack";
			ar = dcn41_irq_ack(&gDcn.d, irq);
		}
		if (ar != DCN41_OK) gDcn.ackFailures++;
		else if (irq.kind == DCN41_IRQ_VUPDATE_NO_LOCK) {
			__atomic_store_n(&gDcn.irqAckRecoveryNeeded, false, __ATOMIC_RELEASE);
		}

		if (gDcn.logBudget > 0) {
			gDcn.logBudget--;
			RDNA4LOG("dcn-irq: kind %u inst %u (src %u data %08x) ack %d — entry %llu at %llu ns",
			       irq.kind, irq.inst, srcId, srcData0, ar,
			       (unsigned long long)gDcn.dceEntries, (unsigned long long)t);
		}

		// Stage 3A: primary framebuffer VBLANK delivery on VUPDATE_NO_LOCK (shared cleanly with
		// native scanout, matching Linux amdgpu_dm) with rate-based storm guard (> 1000/s).
		if (irq.kind == DCN41_IRQ_VUPDATE_NO_LOCK &&
		    __atomic_load_n(&gDcn.primaryVblEnabled, __ATOMIC_ACQUIRE) &&
		    irq.inst == gDcn.primaryVblOtg) {
			if (rdna4vbl::record_irq(gDcn.primaryVblStorm, t)) {
				struct dcn41_irq off { DCN41_IRQ_VUPDATE_NO_LOCK, gDcn.primaryVblOtg };
				__atomic_store_n(&gDcn.primaryVblEnabled, false, __ATOMIC_RELEASE);
				if (__atomic_load_n(&gScan.active, __ATOMIC_ACQUIRE) == 0u || off.inst != 0u) {
					gDcn.tag = "vblstorm";
					(void)dcn41_irq_set(&gDcn.d, off, false);
				}
				RDNA4LOG("dcn-irq: PRIMARY VBL STORM GUARD — > %u VUPDATE/s on OTG %u; hardware VBL DISABLED (falling back to timer)",
				         rdna4vbl::kStormWindowMaxIrqs, off.inst);
			} else {
				notifyPrimaryVbl = true;
				notifyPrimaryOtg = irq.inst;
			}
		}

		// Primary framebuffer DisplayPort HPD / HPD_RX delivery with rate storm guard.
		if ((irq.kind == DCN41_IRQ_HPD || irq.kind == DCN41_IRQ_HPD_RX) &&
		    __atomic_load_n(&gDcn.primaryHpdEnabled, __ATOMIC_ACQUIRE) &&
		    irq.inst == gDcn.primaryHpdPin) {
			if (ar == DCN41_OK) {
				__atomic_store_n(&gDcn.hpdAckRecoveryNeeded, false, __ATOMIC_RELEASE);
			}
			if (rdna4hpd::record_hpd_irq(gDcn.primaryHpdStorm, t)) {
				struct dcn41_irq offHpd { DCN41_IRQ_HPD, gDcn.primaryHpdPin };
				struct dcn41_irq offRx  { DCN41_IRQ_HPD_RX, gDcn.primaryHpdPin };
				__atomic_store_n(&gDcn.primaryHpdEnabled, false, __ATOMIC_RELEASE);
				gDcn.tag = "hpdstorm";
				(void)dcn41_irq_set(&gDcn.d, offHpd, false);
				(void)dcn41_irq_set(&gDcn.d, offRx, false);
				RDNA4LOG("dcn-irq: PRIMARY HPD STORM GUARD — > %u HPD IRQs/s on HPD %u; HPD IRQ DISABLED",
				         rdna4hpd::kHpdStormMaxIrqs, offHpd.inst);
			} else {
				const uint32_t hs = dcn_rreg(&gDcn, gDcn.seg[2] + rdna4hpd::hpd_status_reg(irq.inst));
				notifyPrimaryHpd = true;
				notifyPrimaryHpdPin = irq.inst;
				notifyPrimaryHpdShort = (irq.kind == DCN41_IRQ_HPD_RX);
				notifyPrimaryHpdSense = rdna4hpd::hpd_sense_delayed(hs);
			}
		}

		// Storm guard: disable what we enabled rather than let it spin the work loop.
		// build 0.0.603: not while a native client holds the plane or primary VBL is active -
		// both paths enforce a rate guard instead of a cumulative cap (~20 min at 160 Hz).
		if (gDcn.srcEnabled && !gDcn.stormTripped &&
		    __atomic_load_n(&gScan.active, __ATOMIC_ACQUIRE) == 0u &&
		    !__atomic_load_n(&gDcn.primaryVblEnabled, __ATOMIC_ACQUIRE) &&
		    gDcn.dceEntries - gDcn.entriesAtEnable > kDceStormCap) {
			struct dcn41_irq off { gDcn.srcKind, gDcn.srcInst };
			gDcn.tag = "stormoff";
			(void)dcn41_irq_set(&gDcn.d, off, false);
			gDcn.srcEnabled = false;
			gDcn.stormTripped = true;
			RDNA4LOG("dcn-irq: STORM GUARD — %llu entries since the enable exceeded the cap %llu; "
			       "kind %u inst %u DISABLED by the handler",
			       (unsigned long long)(gDcn.dceEntries - gDcn.entriesAtEnable),
			       (unsigned long long)kDceStormCap, off.kind, off.inst);
		}
	}
	// Dispatch primary FB callbacks AFTER releasing rdna4_display_txn (zero DCN locks held).
	if (notifyPrimaryVbl) garnet_primary_fb_notify_vbl(notifyPrimaryOtg);
	if (notifyPrimaryHpd) garnet_primary_fb_notify_hpd(notifyPrimaryHpdPin, notifyPrimaryHpdShort, notifyPrimaryHpdSense);
	return true;
}

// ---- the verbs ------------------------------------------------------------------------

// `accel dcnstate` — read-only.
//
// arg 1 additionally runs the allowlist's ON-HARDWARE self-test. It uses a SEPARATE
// allowlist state so the live counters stay clean, and it is genuinely harmless: a refused
// write touches nothing, and the one legal address it exercises is classified, not written.
uint32_t state(uint64_t arg, uint64_t *out, unsigned outCount) {
	if (gDcn.initStatus == 0xFFFFFFFFu && gDcn.owner) (void)bind(gDcn.owner);
	// dcnstate reports even when another thread owns the display transaction.
	// Its one write-capable side effect (restoring a held flip) is allowed only
	// when this call obtained the transaction; a conflict leaves the report
	// read-only instead of bypassing the gate.
	rdna4_display_txn txn(rdna4peer::kLeaseLocalDisplay);
	if (txn.ok) {
		flip_restore_if_held("entry to dcnstate");
	} else if (gDcn.flipHeld) {
		RDNA4LOG("dcn-state: held flip restore deferred: display transaction conflict");
	}

	const int otg = lit_otg();
	uint32_t ha = 0, va = 0, ht = 0, vt = 0, hf = 0, hs = 0, vf = 0, vs = 0;
	uint64_t pixelHz = 0;
	(void)rdna4lr_live(&gLr, &ha, &va, &ht, &vt, &hf, &hs, &vf, &vs, &pixelHz);
	// Only a programmed DTO is an observed clock; table fallback is not link evidence.
	if (otg < 0 || !rdna4lr_dp_clock(&gLr, static_cast<uint32_t>(otg), &pixelHz)) pixelHz = 0;
	dp_log_preflight(otg >= 0 ? static_cast<uint32_t>(otg) : rdna4dp::unknown,
	                 {ha, va, ht, vt, pixelHz}, "query-current-raster");
	uint32_t frameCount = 0, gss = 0, pos = 0;
	if (otg >= 0) {
		(void)dcn41_otg_get_frame_count(&gDcn.d, (uint32_t)otg, &frameCount);
		struct dcn41_otg_irq_status st {};
		if (dcn41_otg_irq_status(&gDcn.d, (uint32_t)otg, &st) == DCN41_OK) gss = st.raw;
		struct dcn41_otg_position p {};
		if (dcn41_otg_get_position(&gDcn.d, (uint32_t)otg, &p) == DCN41_OK)
			pos = (p.vertical_count << 16) | (p.horizontal_count & 0xFFFFu);
	}

	RDNA4LOG("dcn-state: armed %d (init status %u) lit OTG %d, frame count %u, "
	       "GLOBAL_SYNC_STATUS %#010x, position %#010x",
	       gDcn.armed ? 1 : 0, gDcn.initStatus, otg, frameCount, gss, pos);
	RDNA4LOG("dcn-state: allowlist %u ranges, writes %llu (allowed %llu refused %llu), reads %llu; "
	       "refusal reasons unarmed %llu window %llu indirect %llu block %llu range %llu; "
	       "last refused %#010x (%s)",
	       dcn41_allow_range_count(), (unsigned long long)gDcn.allow.writes_seen,
	       (unsigned long long)gDcn.allow.allowed, (unsigned long long)gDcn.allow.refused,
	       (unsigned long long)gDcn.allow.reads_seen,
	       (unsigned long long)gDcn.allow.by_reason[DCN41_ALLOW_UNARMED],
	       (unsigned long long)gDcn.allow.by_reason[DCN41_ALLOW_WINDOW],
	       (unsigned long long)gDcn.allow.by_reason[DCN41_ALLOW_INDIRECT],
	       (unsigned long long)gDcn.allow.by_reason[DCN41_ALLOW_BLOCK],
	       (unsigned long long)gDcn.allow.by_reason[DCN41_ALLOW_RANGE],
	       gDcn.allow.last_refused_abs,
	       dcn41_allow_reason_name((int)gDcn.allow.last_refused_reason));
	for (uint32_t i = 0; i < gDcn.allow.n_allow_samples; i++)
		RDNA4LOG("dcn-state:   allowed sample %u: seq %u %#010x = %#010x tag %s",
		       i, gDcn.allow.allow[i].seq, gDcn.allow.allow[i].abs_dword,
		       gDcn.allow.allow[i].value, gDcn.allow.allow[i].tag);
	for (uint32_t i = 0; i < gDcn.allow.n_refuse_samples; i++)
		RDNA4LOG("dcn-state:   REFUSED sample %u: seq %u %#010x = %#010x tag %s reason %s",
		       i, gDcn.allow.refuse[i].seq, gDcn.allow.refuse[i].abs_dword,
		       gDcn.allow.refuse[i].value, gDcn.allow.refuse[i].tag,
		       dcn41_allow_reason_name((int)gDcn.allow.refuse[i].reason));

	const uint64_t spanNs = (gDcn.lastEntryNs > gDcn.firstEntryNs)
	                      ? gDcn.lastEntryNs - gDcn.firstEntryNs : 0;
	RDNA4LOG("dcn-state: DCE IH entries %llu (unclassified %llu, txn refusals %llu, ack failures %llu) over %llu ns; "
	       "VSTARTUP %llu VUPDATE_NO_LOCK %llu PFLIP %llu HPD %llu HPD_RX %llu VLINE0 %llu OUTBOX %llu",
	       (unsigned long long)gDcn.dceEntries, (unsigned long long)gDcn.dceUnclassified,
	       (unsigned long long)gDcn.irqRefusals, (unsigned long long)gDcn.ackFailures,
	       (unsigned long long)spanNs,
	       (unsigned long long)gDcn.dceByKind[DCN41_IRQ_VSTARTUP],
	       (unsigned long long)gDcn.dceByKind[DCN41_IRQ_VUPDATE_NO_LOCK],
	       (unsigned long long)gDcn.dceByKind[DCN41_IRQ_PFLIP],
	       (unsigned long long)gDcn.dceByKind[DCN41_IRQ_HPD],
	       (unsigned long long)gDcn.dceByKind[DCN41_IRQ_HPD_RX],
	       (unsigned long long)gDcn.dceByKind[DCN41_IRQ_VLINE0],
	       (unsigned long long)gDcn.dceByKind[DCN41_IRQ_DMCUB_OUTBOX]);
	RDNA4LOG("dcn-state: per-OTG VUPDATE %llu %llu %llu %llu; VSTARTUP %llu %llu %llu %llu; "
	       "source %s (kind %u inst %u), storm guard %s; primaryVbl %s (OTG %u, storm %s)",
	       (unsigned long long)gDcn.dceVupdate[0], (unsigned long long)gDcn.dceVupdate[1],
	       (unsigned long long)gDcn.dceVupdate[2], (unsigned long long)gDcn.dceVupdate[3],
	       (unsigned long long)gDcn.dceVstartup[0], (unsigned long long)gDcn.dceVstartup[1],
	       (unsigned long long)gDcn.dceVstartup[2], (unsigned long long)gDcn.dceVstartup[3],
	       gDcn.srcEnabled ? "ENABLED" : "off", gDcn.srcKind, gDcn.srcInst,
	       gDcn.stormTripped ? "TRIPPED" : "clear",
	       gDcn.primaryVblEnabled ? "ENABLED" : "off", gDcn.primaryVblOtg,
	       gDcn.primaryVblStorm.tripped ? "TRIPPED" : "clear");

	scanLogState();   // build 0.0.603: one line, and only when a native client has ever taken the plane

	if (arg == 1) {
		// The allowlist, exercised against this card's own addresses, in the kernel.
		struct dcn41_allow_state probe {};
		const int pr = dcn41_allow_init(&probe, gDcn.seg, (uint32_t)(gDcn.dev ? gDcn.dev->rmmioSize / 4 : 0));
		const uint32_t legal   = gDcn.seg[2] + 0x1b88u;   // OTG0_OTG_GLOBAL_SYNC_STATUS
		const uint32_t smu     = 0x00016282u;             // regMP1_SMN_C2PMSG_66, the SMU mailbox
		const uint32_t indirect = 0x0000000eu;            // BIF_BX1_PCIE_INDEX2
		const uint32_t stray   = 0x0000a000u;             // GC BASE_IDX 1, one past the top range
		const bool okLegal = dcn41_allow_write(&probe, legal, 0u, "selftest");
		const bool okSmu   = dcn41_allow_write(&probe, smu, 0u, "selftest");
		const bool okInd   = dcn41_allow_write(&probe, indirect, 0u, "selftest");
		const bool okStray = dcn41_allow_write(&probe, stray, 0u, "selftest");
		RDNA4LOG("dcn-selftest: init %s; legal %#010x -> %s; SMU %#010x -> %s; indirect %#010x -> %s; "
		       "stray %#010x -> %s; allowed %llu refused %llu (indirect %llu block %llu range %llu)",
		       dcn41_allow_reason_name(pr), legal, okLegal ? "ALLOWED" : "refused",
		       smu, okSmu ? "*** ALLOWED — BUG ***" : "refused",
		       indirect, okInd ? "*** ALLOWED — BUG ***" : "refused",
		       stray, okStray ? "*** ALLOWED — BUG ***" : "refused",
		       (unsigned long long)probe.allowed, (unsigned long long)probe.refused,
		       (unsigned long long)probe.by_reason[DCN41_ALLOW_INDIRECT],
		       (unsigned long long)probe.by_reason[DCN41_ALLOW_BLOCK],
		       (unsigned long long)probe.by_reason[DCN41_ALLOW_RANGE]);
		// NOTE: the legal address was classified, not written — dcn41_allow_write only decides.
		if (out && outCount > 12)
			out[12] = (uint64_t)((okLegal ? 1u : 0u) | (okSmu ? 2u : 0u) | (okInd ? 4u : 0u) |
			                     (okStray ? 8u : 0u));
	}

	if (out) {
		if (outCount > 0)  out[0]  = gDcn.armed ? 1u : 0u;
		if (outCount > 1)  out[1]  = gDcn.initStatus;
		if (outCount > 2)  out[2]  = (uint64_t)(int64_t)otg;
		if (outCount > 3)  out[3]  = frameCount;
		if (outCount > 4)  out[4]  = gss;
		if (outCount > 5)  out[5]  = gDcn.dceEntries;
		if (outCount > 6)  out[6]  = gDcn.dceByKind[DCN41_IRQ_VSTARTUP];
		if (outCount > 7)  out[7]  = gDcn.dceByKind[DCN41_IRQ_VUPDATE_NO_LOCK];
		if (outCount > 8)  out[8]  = spanNs;
		if (outCount > 9)  out[9]  = gDcn.allow.allowed;
		if (outCount > 10) out[10] = gDcn.allow.refused;
		if (outCount > 11) out[11] = (uint64_t)gDcn.srcEnabled << 8 |
		                             (uint64_t)gDcn.srcKind << 4 | (uint64_t)gDcn.srcInst;
	}
	return gDcn.armed ? 0u : (gDcn.initStatus == 0xFFFFFFFFu ? 1u : gDcn.initStatus);
}

// `accel dcnvbl <n>` — T2-VBL.
//   0  disable whatever we enabled (idempotent, safe to call when nothing is enabled)
//   1  enable VUPDATE_NO_LOCK on the lit OTG
//   2  enable VSTARTUP on the lit OTG
// One source at a time, by construction: an enable disables the previous one first.
uint32_t vbl(uint64_t arg, uint64_t *out, unsigned outCount) {
	rdna4_display_txn txn(rdna4peer::kLeaseLocalDisplay);
	if (!txn.ok) {
		if (out && outCount > 0) out[0] = 0;
		return (uint32_t)kIOReturnNotPermitted;
	}
	if (gDcn.initStatus == 0xFFFFFFFFu && gDcn.owner) (void)bind(gDcn.owner);
	flip_restore_if_held("entry to dcnvbl");
	if (!gDcn.armed) {
		RDNA4LOG("dcn-vbl: display layer is not armed (status %u) — REFUSING", gDcn.initStatus);
		if (out && outCount > 0) out[0] = 0;
		return gDcn.initStatus;
	}
	if (arg > 2) {
		RDNA4LOG("dcn-vbl: unknown argument %llu (0 off, 1 VUPDATE_NO_LOCK, 2 VSTARTUP) — REFUSING",
		       (unsigned long long)arg);
		return 2;
	}

	// Always disable what is currently on first: one source at a time is the whole point of
	// T2, and it also makes `dcnvbl 0` the unconditional escape hatch.
	if (gDcn.srcEnabled) {
		struct dcn41_irq off { gDcn.srcKind, gDcn.srcInst };
		gDcn.tag = "irqoff";
		const int r = dcn41_irq_set(&gDcn.d, off, false);
		RDNA4LOG("dcn-vbl: disabled kind %u inst %u -> %d", off.kind, off.inst, r);
		gDcn.srcEnabled = false;
		if (off.kind == DCN41_IRQ_VUPDATE_NO_LOCK && off.inst == gDcn.primaryVblOtg) {
			__atomic_store_n(&gDcn.primaryVblEnabled, false, __ATOMIC_RELEASE);
		}
	}
	if (arg == 0 && __atomic_load_n(&gDcn.primaryVblEnabled, __ATOMIC_ACQUIRE)) {
		struct dcn41_irq off { DCN41_IRQ_VUPDATE_NO_LOCK, gDcn.primaryVblOtg };
		gDcn.tag = "irqoff";
		(void)dcn41_irq_set(&gDcn.d, off, false);
		__atomic_store_n(&gDcn.primaryVblEnabled, false, __ATOMIC_RELEASE);
	}

	const int otg = lit_otg();
	uint32_t gssBefore = 0, gssAfter = 0, frameCount = 0;
	int rc = 0;
	if (otg >= 0) {
		struct dcn41_otg_irq_status st {};
		if (dcn41_otg_irq_status(&gDcn.d, (uint32_t)otg, &st) == DCN41_OK) gssBefore = st.raw;
		(void)dcn41_otg_get_frame_count(&gDcn.d, (uint32_t)otg, &frameCount);
	}

	if (arg == 0) {
		gDcn.stormTripped = false;
		RDNA4LOG("dcn-vbl: all DCN interrupt sources are off (lit OTG %d, GLOBAL_SYNC_STATUS %#010x, "
		       "frame count %u); DCE entries so far %llu",
		       otg, gssBefore, frameCount, (unsigned long long)gDcn.dceEntries);
	} else {
		if (otg < 0) {
			RDNA4LOG("dcn-vbl: no OTG is master-enabled — REFUSING to enable a source with no lit pipe");
			return 3;
		}
		struct dcn41_irq on {
			(uint8_t)(arg == 1 ? DCN41_IRQ_VUPDATE_NO_LOCK : DCN41_IRQ_VSTARTUP),
			(uint8_t)otg
		};
		// Reset the measurement window so a rate can be computed from this enable alone.
		gDcn.firstEntryNs = 0;
		gDcn.lastEntryNs = 0;
		gDcn.entriesAtEnable = gDcn.dceEntries;
		gDcn.logBudget = kDceLogBudget;
		gDcn.tag = "irqset";
		rc = dcn41_irq_set(&gDcn.d, on, true);
		struct dcn41_otg_irq_status st {};
		if (dcn41_otg_irq_status(&gDcn.d, (uint32_t)otg, &st) == DCN41_OK) gssAfter = st.raw;
		if (rc == DCN41_OK) {
			gDcn.srcEnabled = true;
			gDcn.srcKind = on.kind;
			gDcn.srcInst = on.inst;
			gDcn.enabledAtNs = now_ns();
			gDcn.stormTripped = false;
		}
		RDNA4LOG("dcn-vbl: ENABLE kind %u (%s) on OTG %d -> %d; GLOBAL_SYNC_STATUS %#010x -> %#010x "
		       "(VSTARTUP_INT_EN %u, VUPDATE_NO_LOCK_INT_EN %u); frame count %u; "
		       "entries at enable %llu",
		       on.kind, arg == 1 ? "VUPDATE_NO_LOCK" : "VSTARTUP", otg, rc, gssBefore, gssAfter,
		       st.vstartup_int_en, st.vupdate_no_lock_int_en, frameCount,
		       (unsigned long long)gDcn.entriesAtEnable);
		// The enable is a read-modify-write of ONE register; if the allowlist refused it, the
		// read-back above shows the enable bit still clear and the refusal is already logged
		// and counted. That distinction is exactly what T2's falsifier needs.
	}

	if (out) {
		if (outCount > 0)  out[0]  = gDcn.armed ? 1u : 0u;
		if (outCount > 1)  out[1]  = (uint64_t)(int64_t)rc;
		if (outCount > 2)  out[2]  = (uint64_t)(int64_t)otg;
		if (outCount > 3)  out[3]  = frameCount;
		if (outCount > 4)  out[4]  = gssBefore;
		if (outCount > 5)  out[5]  = gssAfter;
		if (outCount > 6)  out[6]  = gDcn.dceEntries;
		if (outCount > 7)  out[7]  = gDcn.entriesAtEnable;
		if (outCount > 8)  out[8]  = gDcn.allow.allowed;
		if (outCount > 9)  out[9]  = gDcn.allow.refused;
		if (outCount > 10) out[10] = gDcn.allow.last_refused_abs;
		if (outCount > 11) out[11] = (uint64_t)gDcn.srcEnabled << 8 |
		                             (uint64_t)gDcn.srcKind << 4 | (uint64_t)gDcn.srcInst;
		if (outCount > 12) out[12] = gDcn.srcEnabled ? gDcn.enabledAtNs : 0;
	}
	return (rc == DCN41_OK) ? 0u : (0x80u | ((uint32_t)(-rc) & 0x7fu));
}

bool primaryVblIrqSet(GarnetDriver *owner, uint32_t otg, bool enable) {
	if (otg >= DCN41_NUM_PIPES) return false;
	if (enable) {
		if (!owner || !owner->interruptsArmed()) return false;
		if (!__atomic_load_n(&gDcn.armed, __ATOMIC_ACQUIRE) && bind(owner) != 0u) return false;
		if (__atomic_load_n(&gDcn.primaryVblStorm.tripped, __ATOMIC_ACQUIRE)) return false;
		if (__atomic_load_n(&gDcn.primaryVblEnabled, __ATOMIC_ACQUIRE) && gDcn.primaryVblOtg == otg) {
			return true;
		}
	} else {
		if (!__atomic_load_n(&gDcn.armed, __ATOMIC_ACQUIRE)) return false;
		if (!__atomic_load_n(&gDcn.primaryVblEnabled, __ATOMIC_ACQUIRE)) return true;
	}

	auto vupdateOwnedByOther = [](uint32_t pipe) -> bool {
		if (__atomic_load_n(&gScan.active, __ATOMIC_ACQUIRE) != 0u && pipe == 0u) return true;
		if (gDcn.srcEnabled && gDcn.srcKind == DCN41_IRQ_VUPDATE_NO_LOCK && gDcn.srcInst == pipe) return true;
		return false;
	};

	// Bounded retry around rdna4_display_txn: ihEntry() holds the local-display lease for only
	// a few microseconds per frame and never sleeps, so transient collisions clear quickly.
	for (uint32_t attempt = 0u; attempt < 25u; ++attempt) {
		rdna4_display_txn txn(rdna4peer::kLeaseLocalDisplay);
		if (!txn.ok) {
			IODelay(100u);
			continue;
		}

		if (enable) {
			if (__atomic_load_n(&gDcn.primaryVblEnabled, __ATOMIC_ACQUIRE) && gDcn.primaryVblOtg != otg) {
				const uint8_t prevOtg = gDcn.primaryVblOtg;
				__atomic_store_n(&gDcn.primaryVblEnabled, false, __ATOMIC_RELEASE);
				if (!vupdateOwnedByOther(prevOtg)) {
					struct dcn41_irq prev { DCN41_IRQ_VUPDATE_NO_LOCK, prevOtg };
					gDcn.tag = "pvbloff";
					(void)dcn41_irq_set(&gDcn.d, prev, false);
				}
			}
			rdna4vbl::reset_storm(gDcn.primaryVblStorm);
			if (!vupdateOwnedByOther(otg)) {
				struct dcn41_irq on { DCN41_IRQ_VUPDATE_NO_LOCK, static_cast<uint8_t>(otg) };
				gDcn.tag = "pvblon";
				const int rc = dcn41_irq_set(&gDcn.d, on, true);
				if (rc != DCN41_OK) return false;
			}
			__atomic_store_n(&gDcn.irqAckRecoveryNeeded, false, __ATOMIC_RELEASE);
			gDcn.primaryVblOtg = static_cast<uint8_t>(otg);
			__atomic_store_n(&gDcn.primaryVblEnabled, true, __ATOMIC_RELEASE);
			return true;
		}

		if (__atomic_load_n(&gDcn.primaryVblEnabled, __ATOMIC_ACQUIRE)) {
			const uint8_t curOtg = gDcn.primaryVblOtg;
			__atomic_store_n(&gDcn.primaryVblEnabled, false, __ATOMIC_RELEASE);
			if (!vupdateOwnedByOther(curOtg)) {
				struct dcn41_irq off { DCN41_IRQ_VUPDATE_NO_LOCK, curOtg };
				gDcn.tag = "pvbloff";
				const int rc = dcn41_irq_set(&gDcn.d, off, false);
				return rc == DCN41_OK;
			}
		}
		return true;
	}
	RDNA4LOG("dcn-vbl: primaryVblIrqSet(otg=%u, en=%u) timed out waiting for display lease",
	         otg, enable ? 1u : 0u);
	return false;
}

bool primaryVblIrqActive() {
	return __atomic_load_n(&gDcn.armed, __ATOMIC_ACQUIRE) &&
	       __atomic_load_n(&gDcn.primaryVblEnabled, __ATOMIC_ACQUIRE) &&
	       !__atomic_load_n(&gDcn.primaryVblStorm.tripped, __ATOMIC_ACQUIRE);
}

void recoverVupdateAckUnderTxn(uint32_t otg, bool checkHwStatus) {
	if (otg >= DCN41_NUM_PIPES) return;
	if (!__atomic_load_n(&gDcn.armed, __ATOMIC_ACQUIRE)) return;
	if (!rdna4displaytxn::held()) return;
	bool need = __atomic_load_n(&gDcn.irqAckRecoveryNeeded, __ATOMIC_ACQUIRE);
	if (!need && checkHwStatus) {
		struct dcn41_otg_irq_status st {};
		if (dcn41_otg_irq_status(&gDcn.d, otg, &st) == DCN41_OK &&
		    st.vupdate_no_lock_int_en && st.vupdate_no_lock_int_status) {
			need = true;
		}
	}
	if (!need) return;
	struct dcn41_irq irq { DCN41_IRQ_VUPDATE_NO_LOCK, static_cast<uint8_t>(otg) };
	gDcn.tag = "irqrecov";
	if (dcn41_irq_ack(&gDcn.d, irq) == DCN41_OK) {
		__atomic_store_n(&gDcn.irqAckRecoveryNeeded, false, __ATOMIC_RELEASE);
		if (checkHwStatus) {
			RDNA4LOG("dcn-irq: watchdog cleared latched VUPDATE_NO_LOCK ACK on OTG %u", otg);
		}
	}
}

void primaryVblRecoverAckIfNeeded(uint32_t otg) {
	if (otg >= DCN41_NUM_PIPES) return;
	if (!__atomic_load_n(&gDcn.armed, __ATOMIC_ACQUIRE)) return;
	rdna4_display_txn txn(rdna4peer::kLeaseLocalDisplay);
	if (!txn.ok) return;
	IOLock *l = (otg == 0u) ? gScan.lock : nullptr;
	if (l != nullptr) IOLockLock(l);
	recoverVupdateAckUnderTxn(otg, /*checkHwStatus=*/true);
	if (l != nullptr) IOLockUnlock(l);
}

bool primaryHpdIrqSet(GarnetDriver *owner, uint32_t hpd, bool enable) {
	if (hpd >= DCN41_NUM_HPD) return false;
	if (enable) {
		if (!owner || !owner->interruptsArmed()) return false;
		if (!__atomic_load_n(&gDcn.armed, __ATOMIC_ACQUIRE) && bind(owner) != 0u) return false;
		if (__atomic_load_n(&gDcn.primaryHpdStorm.tripped, __ATOMIC_ACQUIRE)) return false;
		if (__atomic_load_n(&gDcn.primaryHpdEnabled, __ATOMIC_ACQUIRE) && gDcn.primaryHpdPin == hpd) {
			const uint32_t destMask = rdna4hpd::hpd_interrupt_dest_mask(hpd);
			const uint32_t destVal = dcn_rreg(&gDcn, gDcn.seg[2] + rdna4hpd::kSeg2HpdInterruptDest);
			if (destVal != 0xffffffffu && (destVal & destMask) == 0u) {
				return true;
			}
		}
	} else {
		if (!__atomic_load_n(&gDcn.armed, __ATOMIC_ACQUIRE)) return false;
		if (!__atomic_load_n(&gDcn.primaryHpdEnabled, __ATOMIC_ACQUIRE)) return true;
	}

	for (uint32_t attempt = 0u; attempt < 25u; ++attempt) {
		rdna4_display_txn txn(rdna4peer::kLeaseLocalDisplay);
		if (!txn.ok) {
			IODelay(100u);
			continue;
		}
		if (enable) {
			if (__atomic_load_n(&gDcn.primaryHpdEnabled, __ATOMIC_ACQUIRE) && gDcn.primaryHpdPin != hpd) {
				const uint8_t prevPin = gDcn.primaryHpdPin;
				__atomic_store_n(&gDcn.primaryHpdEnabled, false, __ATOMIC_RELEASE);
				gDcn.tag = "phpdoff";
				(void)dcn41_irq_set(&gDcn.d, dcn41_irq { DCN41_IRQ_HPD, prevPin }, false);
				(void)dcn41_irq_set(&gDcn.d, dcn41_irq { DCN41_IRQ_HPD_RX, prevPin }, false);
			}
			rdna4hpd::reset_hpd_storm(gDcn.primaryHpdStorm);
			const uint8_t pin = static_cast<uint8_t>(hpd);
			// Reclaim HPD/HPD_RX routing from UEFI GOP's DMCUB to Host IH before arming.
			const uint32_t destAbs = gDcn.seg[2] + rdna4hpd::kSeg2HpdInterruptDest;
			const uint32_t destMask = rdna4hpd::hpd_interrupt_dest_mask(pin);
			const uint32_t destBefore = dcn_rreg(&gDcn, destAbs);
			uint32_t destAfter = destBefore;
			if (destBefore != 0xffffffffu && (destBefore & destMask) != 0u) {
				gDcn.tag = "phpdrt";
				dcn_wreg(&gDcn, destAbs, destBefore & ~destMask);
				destAfter = dcn_rreg(&gDcn, destAbs);
			}
			const bool hostRouted = (destAfter != 0xffffffffu) && ((destAfter & destMask) == 0u);
			RDNA4LOG("dcn-hpd: HPD%u interrupt dest 0x%08x -> 0x%08x (host=%u)",
			         pin, destBefore, destAfter, hostRouted ? 1u : 0u);
			gDcn.tag = "phpdon";
			if (dcn41_irq_set(&gDcn.d, dcn41_irq { DCN41_IRQ_HPD, pin }, true) != DCN41_OK) {
				return false;
			}
			gDcn.tag = "phpdrx";
			if (dcn41_irq_set(&gDcn.d, dcn41_irq { DCN41_IRQ_HPD_RX, pin }, true) != DCN41_OK) {
				gDcn.tag = "phpdoff";
				(void)dcn41_irq_set(&gDcn.d, dcn41_irq { DCN41_IRQ_HPD, pin }, false);
				return false;
			}
			__atomic_store_n(&gDcn.hpdAckRecoveryNeeded, false, __ATOMIC_RELEASE);
			gDcn.primaryHpdPin = pin;
			__atomic_store_n(&gDcn.primaryHpdEnabled, true, __ATOMIC_RELEASE);
			return true;
		}
		if (__atomic_load_n(&gDcn.primaryHpdEnabled, __ATOMIC_ACQUIRE)) {
			const uint8_t curPin = gDcn.primaryHpdPin;
			__atomic_store_n(&gDcn.primaryHpdEnabled, false, __ATOMIC_RELEASE);
			gDcn.tag = "phpdoff";
			const int r1 = dcn41_irq_set(&gDcn.d, dcn41_irq { DCN41_IRQ_HPD, curPin }, false);
			const int r2 = dcn41_irq_set(&gDcn.d, dcn41_irq { DCN41_IRQ_HPD_RX, curPin }, false);
			return r1 == DCN41_OK && r2 == DCN41_OK;
		}
		return true;
	}
	RDNA4LOG("dcn-hpd: primaryHpdIrqSet(hpd=%u, en=%u) timed out waiting for display lease",
	         hpd, enable ? 1u : 0u);
	return false;
}

void primaryHpdRecoverAck(uint32_t hpd) {
	if (hpd >= DCN41_NUM_HPD) return;
	if (!__atomic_load_n(&gDcn.armed, __ATOMIC_ACQUIRE)) return;
	rdna4_display_txn txn(rdna4peer::kLeaseLocalDisplay);
	if (!txn.ok) return;
	bool need = __atomic_load_n(&gDcn.hpdAckRecoveryNeeded, __ATOMIC_ACQUIRE);
	if (!need) {
		const uint32_t hs = dcn_rreg(&gDcn, gDcn.seg[2] + rdna4hpd::hpd_status_reg(hpd));
		if (hs != 0xffffffffu && (hs & rdna4hpd::kHpdAnyIrqStatusMask) != 0u) {
			need = true;
		}
	}
	if (!need) return;
	const uint8_t pin = static_cast<uint8_t>(hpd);
	gDcn.tag = "hpdrecov";
	(void)dcn41_irq_ack(&gDcn.d, dcn41_irq { DCN41_IRQ_HPD, pin });
	(void)dcn41_irq_ack(&gDcn.d, dcn41_irq { DCN41_IRQ_HPD_RX, pin });
	__atomic_store_n(&gDcn.hpdAckRecoveryNeeded, false, __ATOMIC_RELEASE);
}

void primaryHpdShutdown() {
	if (!__atomic_load_n(&gDcn.armed, __ATOMIC_ACQUIRE)) return;
	(void)primaryHpdIrqSet(nullptr, gDcn.primaryHpdPin, false);
	__atomic_store_n(&gDcn.primaryHpdEnabled, false, __ATOMIC_RELEASE);
}

void primaryVblShutdown() {
	if (!__atomic_load_n(&gDcn.armed, __ATOMIC_ACQUIRE)) return;
	primaryHpdShutdown();
	(void)primaryVblIrqSet(nullptr, gDcn.primaryVblOtg, false);
	__atomic_store_n(&gDcn.primaryVblEnabled, false, __ATOMIC_RELEASE);
	__atomic_store_n(&gDcn.armed, false, __ATOMIC_RELEASE);
}


// `accel dcnflip <n>` — T3-FLIP.
//   0        restore the console plane unconditionally and report. Idempotent, allocates nothing,
//            safe to run at any time from any state. THE EMERGENCY COMMAND.
//   1        phase (a): flip in, one frame, machine witnesses, flip back immediately.
//   2..30     phase (b): the same, holding the test pattern for that many SECONDS so a person can
//            see it, then the same unconditional restore.
//
// Everything between the flip in and the restore runs INSIDE THIS KERNEL CALL, so a dropped ssh
// session cannot strand the display: the caller dying does not stop a kernel call that has already
// begun. On top of that the interrupt watchdog (see ihEntry) restores from the work loop if the hold
// overruns its frame cap, and `flipHeld` makes the next dcnstate/dcnvbl/dcnflip restore as well.
uint32_t flip(uint64_t arg, uint64_t *out, unsigned outCount) {
	rdna4_display_txn txn(rdna4peer::kLeaseLocalDisplay);
	if (!txn.ok) {
		if (out && outCount > 0) out[0] = 0;
		return (uint32_t)kIOReturnNotPermitted;
	}
	if (gDcn.initStatus == 0xFFFFFFFFu && gDcn.owner) (void)bind(gDcn.owner);
	flip_restore_if_held("entry to dcnflip");

	uint64_t addrBack = 0;
	uint32_t st = 0;
	uint32_t c0rg = 0, c0b = 0, c1rg = 0, c1b = 0, t0rg = 0, t0b = 0, t1rg = 0, t1b = 0;
	uint32_t r0rg = 0, r0b = 0;
	uint32_t aw = 0, ah = 0, witness = 0, holdMs = 0, framesHeld = 0;
	bool pendNow = false, pendAfter = false, otgEnabled = false;
	uint64_t earliest = 0;
	clock_sec_t s0 = 0, s1 = 0; clock_nsec_t n0 = 0, n1 = 0;

	if (!gDcn.armed) {
		RDNA4LOG("dcn-flip: display layer is not armed (status %u) — REFUSING", gDcn.initStatus);
		st = gDcn.initStatus ? gDcn.initStatus : 1u;
		goto report;
	}

	if (arg == 0) {
		// Restore only. If we never prepared, there is nothing of ours on screen; still read the
		// plane back so the caller can verify it.
		if (gDcn.flipPrepared) flip_restore("explicit dcnflip 0");
		// build 0.0.603: on a native boot the plane may belong to the RDNA4N scanout path (which never sets flipPrepared): the same
		// shared restore the client's Release / close / death and the watchdog run. A no-op unless a native client took the plane.
		scanEscape("explicit dcnflip 0");
		(void)dcn41_hubp_read_primary_addr(&gDcn.d, gDcn.flipHubp, &addrBack);
		RDNA4LOG("dcn-flip: dcnflip 0 — plane address now %#llx (console was %#llx); held %d, "
		       "flips in %llu out %llu, watchdog restores %llu",
		       (unsigned long long)addrBack, (unsigned long long)gDcn.origMc,
		       gDcn.flipHeld ? 1 : 0, (unsigned long long)gDcn.flipsIn,
		       (unsigned long long)gDcn.flipsOut, (unsigned long long)gDcn.watchdogRestores);
		goto report;
	}
	if (arg > 30u) {
		RDNA4LOG("dcn-flip: hold %llu is above the 30 s cap — REFUSING", (unsigned long long)arg);
		st = 0x20u;
		goto report;
	}
	holdMs = (arg == 1u) ? 0u : (uint32_t)arg * 1000u;

	st = flip_prepare();
	if (st) goto report;

	// The CRC window is the OTG's own active raster (2560x1440 on this machine) — the CRC is taken
	// at the timing generator, AFTER the DPP scaler, so it is in raster space, not plane space.
	(void)dcn41_otg_get_active_size(&gDcn.d, gDcn.flipOtg, &otgEnabled, &aw, &ah);
	if (!otgEnabled || aw == 0 || ah == 0) {
		RDNA4LOG("dcn-flip: OTG %u reports no active size — REFUSING", gDcn.flipOtg);
		st = 0x21u;
		goto report;
	}
	crc_enable(gDcn.flipOtg, aw, ah);
	wait_frames(gDcn.flipOtg, 3);
	crc_read(gDcn.flipOtg, &c0rg, &c0b);
	wait_frames(gDcn.flipOtg, 2);
	crc_read(gDcn.flipOtg, &c1rg, &c1b);

	// The completion witness AND the watchdog: VUPDATE_NO_LOCK, proven on hardware in.
	gDcn.heldFrames = 0;
	gDcn.heldFrameCap = (holdMs + 4000u) / 16u;
	{
		struct dcn41_irq on { (uint8_t)DCN41_IRQ_VUPDATE_NO_LOCK, (uint8_t)gDcn.flipOtg };
		gDcn.tag = "irqset";
		(void)dcn41_irq_set(&gDcn.d, on, true);
		gDcn.srcEnabled = true; gDcn.srcKind = on.kind; gDcn.srcInst = on.inst;
	}

	// ---- THE FLIP. flipHeld is set BEFORE the write, so every restore path already knows. ----
	gDcn.flipHeld = true;
	clock_get_calendar_nanotime(&s0, &n0);
	st = (uint32_t)(int32_t)(-flip_program(gDcn.testMc, "flipin"));
	gDcn.flipsIn++;
	(void)dcn41_hubp_is_flip_pending(&gDcn.d, gDcn.flipHubp, &pendNow, &earliest);
	RDNA4LOG("dcn-flip: FLIP IN to %#llx at wall %llu.%09u — status %u, flip pending now %d, "
	       "hold %u ms, watchdog cap %u frames",
	       (unsigned long long)gDcn.testMc, (unsigned long long)s0, (unsigned)n0, st,
	       pendNow ? 1 : 0, holdMs, gDcn.heldFrameCap);

	wait_frames(gDcn.flipOtg, 2);
	(void)dcn41_hubp_is_flip_pending(&gDcn.d, gDcn.flipHubp, &pendAfter, &earliest);
	crc_read(gDcn.flipOtg, &t0rg, &t0b);
	wait_frames(gDcn.flipOtg, 2);
	crc_read(gDcn.flipOtg, &t1rg, &t1b);

	// ---- the hold, in slices, abortable by the watchdog ----
	for (uint32_t done = 0; done < holdMs && gDcn.flipHeld; done += 250u) IOSleep(250);

	// ---- THE RESTORE. Unconditional. ----
	//: read the watchdog counter BEFORE the restore zeroes it, or the log reports 0 frames
	// after a fifteen-second hold and reads as if the watchdog never counted.
	framesHeld = gDcn.heldFrames;
	flip_restore(arg == 1u ? "phase (a), one frame" : "end of the visible hold");
	clock_get_calendar_nanotime(&s1, &n1);
	wait_frames(gDcn.flipOtg, 3);
	crc_read(gDcn.flipOtg, &r0rg, &r0b);
	(void)dcn41_hubp_read_primary_addr(&gDcn.d, gDcn.flipHubp, &addrBack);

	{
		struct dcn41_irq off { gDcn.srcKind, gDcn.srcInst };
		gDcn.tag = "irqoff";
		(void)dcn41_irq_set(&gDcn.d, off, false);
		gDcn.srcEnabled = false;
	}
	crc_disable(gDcn.flipOtg);

	witness = (pendNow ? 1u : 0u) | (pendAfter ? 0u : 2u)
	        | ((earliest == gDcn.testMc) ? 4u : 0u)
	        | ((addrBack == gDcn.origMc) ? 8u : 0u)
	        | (((c0rg != t0rg) || (c0b != t0b)) ? 16u : 0u)
	        | (((r0rg == c0rg) && (r0b == c0b)) ? 32u : 0u)
	        | ((c0rg == c1rg && c0b == c1b) ? 64u : 0u);
	RDNA4LOG("dcn-flip: VISIBLE WINDOW wall %llu.%09u -> %llu.%09u (%u ms requested); held frames %u",
	       (unsigned long long)s0, (unsigned)n0, (unsigned long long)s1, (unsigned)n1, holdMs,
	       framesHeld);
	RDNA4LOG("dcn-flip: CRC console %08x/%08x and %08x/%08x; test %08x/%08x and %08x/%08x; "
	       "restored %08x/%08x", c0rg, c0b, c1rg, c1b, t0rg, t0b, t1rg, t1b, r0rg, r0b);
	RDNA4LOG("dcn-flip: WITNESS %#x — pending-now %d, pending-cleared %d, earliest-inuse==test %d, "
	       "address-restored %d, CRC-changed %d, CRC-returned %d, console-CRC-stable %d; "
	       "plane now %#llx (console %#llx); allowlist allowed %llu refused %llu",
	       witness, (witness & 1) ? 1 : 0, (witness & 2) ? 1 : 0, (witness & 4) ? 1 : 0,
	       (witness & 8) ? 1 : 0, (witness & 16) ? 1 : 0, (witness & 32) ? 1 : 0,
	       (witness & 64) ? 1 : 0, (unsigned long long)addrBack, (unsigned long long)gDcn.origMc,
	       (unsigned long long)gDcn.allow.allowed, (unsigned long long)gDcn.allow.refused);
	if (addrBack != gDcn.origMc) {
		RDNA4LOG("dcn-flip: *** THE PLANE DID NOT RESTORE *** reprogramming once more");
		flip_restore("restore verification failed");
		(void)dcn41_hubp_read_primary_addr(&gDcn.d, gDcn.flipHubp, &addrBack);
		RDNA4LOG("dcn-flip: after the second restore the plane reads %#llx", (unsigned long long)addrBack);
	}

report:
	if (out) {
		if (outCount > 0)  out[0]  = gDcn.armed ? 1u : 0u;
		if (outCount > 1)  out[1]  = st;
		if (outCount > 2)  out[2]  = (uint64_t)gDcn.flipOtg | ((uint64_t)gDcn.flipHubp << 8);
		if (outCount > 3)  out[3]  = gDcn.origMc;
		if (outCount > 4)  out[4]  = gDcn.testMc;
		if (outCount > 5)  out[5]  = witness;
		if (outCount > 6)  out[6]  = (uint64_t)c0rg | ((uint64_t)c0b << 32);
		if (outCount > 7)  out[7]  = (uint64_t)t0rg | ((uint64_t)t0b << 32);
		if (outCount > 8)  out[8]  = (uint64_t)r0rg | ((uint64_t)r0b << 32);
		if (outCount > 9)  out[9]  = gDcn.allow.allowed;
		if (outCount > 10) out[10] = gDcn.allow.refused;
		if (outCount > 11) out[11] = (uint64_t)gDcn.fbWidth | ((uint64_t)gDcn.fbHeight << 16)
		                           | ((uint64_t)gDcn.fbPitchPx << 32) | ((uint64_t)gDcn.fbPixFmt << 48);
		if (outCount > 12) out[12] = addrBack;
	}
	return st;
}


// ---- the same-mode re-timing: the first mode-set -----------------------------------------------
//
// `accel dcnmode <n>`:
//   0       READ-ONLY report, plus the emergency: if the live timing has drifted from the capture
//           taken before we ever wrote anything this boot, write that capture back and say so.
//   1       capture, write the identical values back, re-capture, require every dword to match.
//   2..30   the same with that many seconds of dwell between the write and the verification, so a
//           person can watch for a flicker and so the frame counter can be sampled across a window.
//
// WHY THIS IS THE SAFEST POSSIBLE FIRST MODE-SET. Nothing is computed. The values written are the
// values read one instruction earlier, so the raster cannot change: the failure mode "we computed a
// timing the monitor cannot show" does not exist here. What it does exercise is everything else a
// real mode-set needs - the register set, Linux's write order, the double buffering and the latch -
// on a live, lit, already-link-trained DisplayPort output. src/dcn41/dcn41_otg_timing.c holds the
// register table; its host tests prove that writing a capture back leaves a register model
// byte-identical, that each write carries the value read from that address, that the order is
// Linux's optc1_program_timing order, and that the write set never touches OTG_CONTROL, the update
// lock, the interrupt enables, the CRC or the flip registers.
//
// NOT DONE HERE, deliberately: the OTG update lock is NOT taken. Linux takes it when values change,
// to make them latch together; identical values have nothing to tear, and a lock left set would
// freeze the pipe. Taking it is a separate, later, opt-in step.
uint32_t mode(uint64_t arg, uint64_t *out, unsigned outCount) {
	rdna4_display_txn txn(rdna4peer::kLeaseLocalDisplay);
	if (!txn.ok) {
		if (out && outCount > 0) out[0] = 0;
		return (uint32_t)kIOReturnNotPermitted;
	}
	if (gDcn.initStatus == 0xFFFFFFFFu && gDcn.owner) (void)bind(gDcn.owner);
	flip_restore_if_held("entry to dcnmode");

	struct dcn41_otg_timing before {}, after {};
	struct dcn41_timing_decoded db {}, da {};
	struct dcn41_otg_timing toWrite {};
	uint32_t st = 0, diffs = 0xFFFFFFFFu, fw = 0, fr = 0, dwellMs = 0;
	uint32_t f0 = 0, f1 = 0, stalls = 0, frames0 = 0, frames1 = 0, frames2 = 0;
	uint64_t rate0 = 0, rate1 = 0, rate2 = 0;
	bool realChange = false;
	int otg = -1, rcW = 0;

	if (!gDcn.armed) {
		RDNA4LOG("dcn-mode: display layer is not armed (status %u) — REFUSING", gDcn.initStatus);
		st = gDcn.initStatus ? gDcn.initStatus : 1u;
		goto report;
	}
	// 0 read-only; 1..30 the same-mode re-timing with (arg) seconds of dwell; 101..130 the REAL
	// change (v_total + kVTotalDeltaLines) with (arg - 100) seconds of dwell. Anything else refused.
	realChange = (arg >= 101u && arg <= 130u);
	if (arg > 30u && !realChange) {
		RDNA4LOG("dcn-mode: argument %llu is not 0, 1..30 or 101..130 — REFUSING",
		       (unsigned long long)arg);
		st = 0x20u;
		goto report;
	}
	otg = lit_otg();
	if (otg < 0) {
		RDNA4LOG("dcn-mode: no OTG is master-enabled — REFUSING");
		st = 2u;
		goto report;
	}
	dwellMs = realChange ? (uint32_t)(arg - 100u) * 1000u
	                     : ((arg <= 1u) ? 0u : (uint32_t)arg * 1000u);

	if (dcn41_otg_read_timing(&gDcn.d, (uint32_t)otg, &before) != DCN41_OK) {
		RDNA4LOG("dcn-mode: could not read OTG %d's timing — REFUSING", otg);
		st = 3u;
		goto report;
	}
	dcn41_otg_timing_decode(&before, &db);

	// The golden capture: the timing as it was BEFORE this kext ever wrote a timing register this
	// boot. Taken once, on the first dcnmode call of any kind, and never overwritten.
	if (!gDcn.goldenValid) {
		gDcn.golden = before;
		gDcn.goldenValid = true;
		RDNA4LOG("dcn-mode: GOLDEN capture taken (OTG %d, before any timing write this boot)", otg);
	}

	RDNA4LOG("dcn-mode: OTG %d timing — %ux%u active, h_total %u v_total %u, h sync %u v sync %u, "
	       "blank h %u..%u v %u..%u, vstartup %u vupdate %u/%u vready %u, master_en %u, lock held %u, "
	       "frame count %u",
	       otg, db.h_active, db.v_active, db.h_total, db.v_total, db.h_sync_width, db.v_sync_width,
	       db.h_blank_start, db.h_blank_end, db.v_blank_start, db.v_blank_end, db.vstartup,
	       db.vupdate_offset, db.vupdate_width, db.vready_offset, db.master_en, db.update_lock_held,
	       before.frame_count);
	for (uint32_t i = 0; i < DCN41_TIMING_W_COUNT; i++)
		RDNA4LOG("dcn-mode:   W %-24s %#010x = %#010x", dcn41_timing_w_name(i),
		       gDcn.seg[2] + dcn41_timing_w_offset(i) + 0x80u * (uint32_t)otg, before.w[i]);
	for (uint32_t i = 0; i < DCN41_TIMING_R_COUNT; i++)
		RDNA4LOG("dcn-mode:   R %-24s %#010x = %#010x", dcn41_timing_r_name(i),
		       gDcn.seg[2] + dcn41_timing_r_offset(i) + 0x80u * (uint32_t)otg, before.r[i]);

	if (arg == 0u) {
		// build 0.0.605: on a native boot the FULL golden set (every register a mode trial writes, taken at bind) is restored first - it also ends a running trial, which
		// restores and answers ABORT. Then the timing capture is re-read, so the drift check below judges the machine as it is now. A non-native boot skips all of this.
		if (mt_emergency_restore("dcnmode 0")) {
			(void)dcn41_otg_read_timing(&gDcn.d, (uint32_t)otg, &before);
			dcn41_otg_timing_decode(&before, &db);
		}
		// Read-only, plus the emergency restore if the live timing has drifted from the golden one.
		uint32_t gw = 0, gr = 0;
		const uint32_t drift = dcn41_otg_timing_diff(&gDcn.golden, &before, &gw, &gr);
		if (drift != 0 && gDcn.goldenValid && gDcn.golden.otg == (uint32_t)otg) {
			RDNA4LOG("dcn-mode: *** the live timing differs from the golden capture in %u dword(s) "
			       "(first writable %s, first read-only %s) — RESTORING the golden capture ***",
			       drift, dcn41_timing_w_name(gw), dcn41_timing_r_name(gr));
			gDcn.tag = "modegold";
			rcW = dcn41_otg_write_timing(&gDcn.d, (uint32_t)otg, &gDcn.golden);
			gDcn.goldenRestores++;
			RDNA4LOG("dcn-mode: golden restore status %d", rcW);
		} else {
			RDNA4LOG("dcn-mode: dcnmode 0 — read-only; the live timing matches the golden capture "
			       "(%u dwords differ)", drift);
		}
		diffs = drift;
		goto report;
	}

	// ---- THE WRITE ----
	// Either the identical set (the proven same-mode re-timing) or, for arg 101..130, the same set
	// with ONE field changed: OTG_V_TOTAL, plus kVTotalDeltaLines lines. See the header comment.
	f0 = before.frame_count;
	toWrite = before;
	if (realChange) {
		const int adj = dcn41_otg_timing_adjust_v_total(&toWrite, kVTotalDeltaLines);
		if (adj != DCN41_OK) {
			RDNA4LOG("dcn-mode: the v_total adjustment (%d lines) was REFUSED by the guard (%d) — "
			       "nothing written", kVTotalDeltaLines, adj);
			st = 0x50u;
			goto report;
		}
		// Measure the refresh we are about to change, so the claim is a comparison and not an assertion.
		rate0 = measure_refresh_mhz((uint32_t)otg, 2000, &frames0);
		RDNA4LOG("dcn-mode: BEFORE the change — measured %llu mHz over %u frames in 2 s; "
		       "OTG_V_TOTAL %#010x -> %#010x (v_total %u -> %u)",
		       (unsigned long long)rate0, frames0, before.w[DCN41_TW_V_TOTAL],
		       toWrite.w[DCN41_TW_V_TOTAL], (before.w[DCN41_TW_V_TOTAL] & 0x7FFFu) + 1u,
		       (toWrite.w[DCN41_TW_V_TOTAL] & 0x7FFFu) + 1u);
	}
	gDcn.tag = realChange ? "modereal" : "modeset";
	rcW = dcn41_otg_write_timing(&gDcn.d, (uint32_t)otg, &toWrite);
	gDcn.retimings++;
	RDNA4LOG("dcn-mode: WROTE %s timing set to OTG %d (%u registers) -> status %d; dwell %u ms",
	       realChange ? "a CHANGED" : "the identical", otg, DCN41_TIMING_W_COUNT, rcW, dwellMs);
	if (rcW != DCN41_OK) {
		st = 0x30u;
		// fall through to the unconditional restore below
	}

	wait_frames((uint32_t)otg, 2);

	// With the change in, measure again: this is the whole proof that the hardware really changed.
	if (realChange) {
		rate1 = measure_refresh_mhz((uint32_t)otg, 2000, &frames1);
		RDNA4LOG("dcn-mode: WITH the change — measured %llu mHz over %u frames in 2 s (was %llu mHz)",
		       (unsigned long long)rate1, frames1, (unsigned long long)rate0);
	}

	// ---- the dwell, with a frame-counter STALL WATCHDOG ----
	// The values are identical, so there is no bad state to restore from; the real hazard is the OTG
	// stopping. That is what this watches for, and its answer is to write the capture back at once
	// and say so, rather than sit through the rest of the dwell.
	if (dwellMs) {
		uint32_t last = 0;
		(void)dcn41_otg_get_frame_count(&gDcn.d, (uint32_t)otg, &last);
		for (uint32_t done = 0; done < dwellMs; done += 250u) {
			IOSleep(250);
			uint32_t now = 0;
			if (dcn41_otg_get_frame_count(&gDcn.d, (uint32_t)otg, &now) != DCN41_OK) break;
			if (dcn41_frame_delta(last, now) == 0) {
				stalls++;
				gDcn.retimingStalls++;
				RDNA4LOG("dcn-mode: *** STALL WATCHDOG — the frame counter did not advance in 250 ms "
				       "(stuck at %u) — writing the capture back NOW ***", now);
				gDcn.tag = "modestall";
				(void)dcn41_otg_write_timing(&gDcn.d, (uint32_t)otg, &before);   /* the ORIGINAL */
				break;
			}
			last = now;
		}
	}

	// ---- THE UNCONDITIONAL RESTORE: write the saved capture back on every path ----
	gDcn.tag = "moderestore";
	(void)dcn41_otg_write_timing(&gDcn.d, (uint32_t)otg, &before);
	wait_frames((uint32_t)otg, 2);

	if (dcn41_otg_read_timing(&gDcn.d, (uint32_t)otg, &after) != DCN41_OK) {
		RDNA4LOG("dcn-mode: could not re-read OTG %d's timing", otg);
		st = st ? st : 4u;
		goto report;
	}
	dcn41_otg_timing_decode(&after, &da);
	diffs = dcn41_otg_timing_diff(&before, &after, &fw, &fr);
	f1 = after.frame_count;
	RDNA4LOG("dcn-mode: VERIFY — %u dword(s) differ after the write-back (first writable %s, first "
	       "read-only %s); active %ux%u -> %ux%u; frame count %u -> %u (delta %u); stalls %u; "
	       "allowlist allowed %llu refused %llu",
	       diffs, diffs ? dcn41_timing_w_name(fw) : "none", diffs ? dcn41_timing_r_name(fr) : "none",
	       db.h_active, db.v_active, da.h_active, da.v_active, f0, f1,
	       dcn41_frame_delta(f0, f1), stalls,
	       (unsigned long long)gDcn.allow.allowed, (unsigned long long)gDcn.allow.refused);
	if (diffs != 0) {
		for (uint32_t i = 0; i < DCN41_TIMING_W_COUNT; i++)
			if (before.w[i] != after.w[i])
				RDNA4LOG("dcn-mode:   DIFFERS %-24s %#010x -> %#010x", dcn41_timing_w_name(i),
				       before.w[i], after.w[i]);
		for (uint32_t i = 0; i < DCN41_TIMING_R_COUNT; i++)
			if (before.r[i] != after.r[i])
				RDNA4LOG("dcn-mode:   DIFFERS(ro) %-24s %#010x -> %#010x", dcn41_timing_r_name(i),
				       before.r[i], after.r[i]);
		if (st == 0) st = 0x40u;
	}
	if (realChange) {
		rate2 = measure_refresh_mhz((uint32_t)otg, 2000, &frames2);
		RDNA4LOG("dcn-mode: AFTER the restore — measured %llu mHz over %u frames in 2 s "
		       "(before %llu, with the change %llu)", (unsigned long long)rate2, frames2,
		       (unsigned long long)rate0, (unsigned long long)rate1);
	}
	if (dcn41_frame_delta(f0, f1) == 0) {
		RDNA4LOG("dcn-mode: *** the frame counter did not advance across the whole operation — the OTG "
		       "may have stopped ***");
		if (st == 0) st = 0x41u;
	}

report:
	if (out) {
		if (outCount > 0)  out[0]  = gDcn.armed ? 1u : 0u;
		if (outCount > 1)  out[1]  = st;
		if (outCount > 2)  out[2]  = (uint64_t)(int64_t)otg;
		if (outCount > 3)  out[3]  = diffs;
		if (outCount > 4)  out[4]  = (uint64_t)db.h_active | ((uint64_t)db.v_active << 16)
		                           | ((uint64_t)db.h_total << 32) | ((uint64_t)db.v_total << 48);
		if (outCount > 5)  out[5]  = (uint64_t)da.h_active | ((uint64_t)da.v_active << 16)
		                           | ((uint64_t)da.h_total << 32) | ((uint64_t)da.v_total << 48);
		if (outCount > 6)  out[6]  = f0;
		if (outCount > 7)  out[7]  = f1;
		if (outCount > 8)  out[8]  = dcn41_frame_delta(f0, f1);
		if (outCount > 9)  out[9]  = gDcn.allow.allowed;
		if (outCount > 10) out[10] = gDcn.allow.refused;
		if (outCount > 11) out[11] = (uint64_t)stalls | ((uint64_t)gDcn.retimings << 16)
		                           | ((uint64_t)gDcn.goldenRestores << 32);
		if (outCount > 12) out[12] = (uint64_t)(rate0 & 0xFFFFFu) | ((uint64_t)(rate1 & 0xFFFFFu) << 20)
		                           | ((uint64_t)(rate2 & 0xFFFFFu) << 40)
		                           | ((uint64_t)(db.update_lock_held ? 1u : 0u) << 60)
		                           | ((uint64_t)(da.master_en ? 1u : 0u) << 61)
		                           | ((uint64_t)(realChange ? 1u : 0u) << 62);
	}
	return st;
}

// ---- build 0.0.518: flip mode (switch 74) and its unarmed A/B test ------------------------------
//
// Every access below goes through gDcn.d, the BOUND device (bind() above: allowlist armed first, then dcn41_dev_init with
// dcn_wreg as the only writer). The 0.0.515 read-only raster device (gLr) is never used here. HUBP0 only (T1: OTG0 + HUBP0);
// fmHubpGeom refuses unless OTG0 is the lit one. The one register-writing call is fmFlip ->
// dcn41_hubp_program_flip(HUBP0, mc, vmid 0, tmz 0, immediate false), whose writes are the ones dcnflip's 975 allowlisted
// writes already exercised, and which dcn41 refuses for any address outside the exact set {A, B} while flip mode holds it.

uint32_t fmArmed() { return gDcn.armed ? 1u : 0u; }

uint32_t fmTestFlipHeld() { return gDcn.flipHeld ? 1u : 0u; }

int fmHubpGeom(uint32_t *vpW, uint32_t *vpH, uint32_t *pitchPx, uint32_t *fmt, uint32_t *swMode) {
	if (!gDcn.armed) return DCN41_E_UNINIT;
	if (lit_otg() != 0) return DCN41_E_INST;
	const uint32_t dim   = dcn_rreg(&gDcn, hubp_reg(kOffViewportDim,  0u));
	const uint32_t pitch = dcn_rreg(&gDcn, hubp_reg(kOffSurfPitch,    0u));
	const uint32_t cfg   = dcn_rreg(&gDcn, hubp_reg(kOffSurfConfig,   0u));
	const uint32_t tile  = dcn_rreg(&gDcn, hubp_reg(kOffTilingConfig, 0u));
	*vpW = dim & 0xFFFFu; *vpH = (dim >> 16) & 0xFFFFu;
	*pitchPx = (pitch & 0xFFFFu) + 1u;          /* hubp401 writes surface_pitch - 1 */
	*fmt = cfg & 0x7Fu; *swMode = tile & 0x1Fu;
	return DCN41_OK;
}

int fmReadFront(uint32_t *pending, uint64_t *earliest) {
	if (!gDcn.armed) return DCN41_E_UNINIT;
	bool p = true;
	const int rc = dcn41_hubp_is_flip_pending(&gDcn.d, 0u, &p, earliest);
	*pending = p ? 1u : 0u;
	return rc;
}

int fmReadPrimary(uint64_t *mc) {
	if (!gDcn.armed) return DCN41_E_UNINIT;
	return dcn41_hubp_read_primary_addr(&gDcn.d, 0u, mc);
}

int fmFlip(uint64_t mc, const char *tag) {
	if (!gDcn.armed) return DCN41_E_UNINIT;
	rdna4_display_txn txn(rdna4peer::kLeaseLocalDisplay);
	if (!txn.ok) return DCN41_E_UNINIT;
	const int layout = dcn41_hubp_require_linear(&gDcn.d, 0u);
	if (layout != DCN41_OK) return layout;
	gDcn.tag = tag;
	return dcn41_hubp_program_flip(&gDcn.d, 0u, mc, 0u, false, false);
}

// The exact flip set {A, B} (n 2) or none (n 0). Also seeds the device's request record from the programmed address when no
// flip was ever requested this boot (a software field only: Linux's hubp->request_address), so the pending test compares
// EARLIEST_INUSE against what the hardware was last told, not against 0.
int fmSetExact(uint64_t a, uint64_t b, uint32_t n) {
	if (!gDcn.armed) return DCN41_E_UNINIT;
	if (n != 0u && !gDcn.d.hubp_request_valid[0]) {
		uint64_t cur = 0;
		const int r = dcn41_hubp_read_primary_addr(&gDcn.d, 0u, &cur);
		if (r != DCN41_OK || cur == 0) return r != DCN41_OK ? r : DCN41_E_ADDR;
		gDcn.d.hubp_request_addr[0] = cur;
		gDcn.d.hubp_request_valid[0] = 1;
	}
	const uint64_t ab[2] = { a, b };
	return dcn41_dev_set_flip_exact(&gDcn.d, n ? ab : nullptr, n);
}

int fmFrameCount(uint32_t *fc) {
	if (!gDcn.armed) return DCN41_E_UNINIT;
	return dcn41_otg_get_frame_count(&gDcn.d, 0u, fc);
}

uint64_t fmAllowCounts(uint64_t *refused) {
	if (refused) *refused = gDcn.allow.refused;
	return gDcn.allow.allowed;
}

// build 0.0.519: the A/B test's CRC witness. OTG0 only (fmFlip and fmFrameCount are HUBP0/OTG0), the
// same crc_enable/crc_read/crc_disable dcnflip uses (: the CRC is taken at the timing generator over the active raster).
int fmCrcBegin() {
	if (!gDcn.armed) return DCN41_E_UNINIT;
	rdna4_display_txn txn(rdna4peer::kLeaseLocalDisplay);
	if (!txn.ok) return DCN41_E_UNINIT;
	if (lit_otg() != 0) return DCN41_E_INST;
	bool en = false;
	uint32_t aw = 0, ah = 0;
	if (dcn41_otg_get_active_size(&gDcn.d, 0u, &en, &aw, &ah) != DCN41_OK || !en || aw == 0 || ah == 0) return DCN41_E_INST;
	crc_enable(0u, aw, ah);
	wait_frames(0u, 3u);
	return DCN41_OK;
}

int fmCrcRead(uint32_t frames, uint32_t *rg, uint32_t *b) {
	if (!gDcn.armed) return DCN41_E_UNINIT;
	if (frames) wait_frames(0u, frames);
	crc_read(0u, rg, b);
	return DCN41_OK;
}

void fmCrcEnd() {
	if (!gDcn.armed) return;
	rdna4_display_txn txn(rdna4peer::kLeaseLocalDisplay);
	if (!txn.ok) return;
	crc_disable(0u);
}

}  // namespace rdna4dcn
