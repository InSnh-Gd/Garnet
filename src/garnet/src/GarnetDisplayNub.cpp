//
//  GarnetDisplayNub.cpp - see the header. Pure logic: amd/native_fb_pure.h (tests/native_disp_test.cpp), the shared interface: GarnetDisplayOps.h.
//
//  0.0.658 (ABI 2): ONE nub PER DISPLAY INDEX. `fbpublish 2` publishes index 1 (the monitor B: instance 2, OTG2, DDC line 3, 2560x1440), `fbpublish 1` index 2 (the monitor A: instance 1, OTG1, DDC line 2, 1920x1080); the verb's
//  argument is the disp2 INSTANCE and ONE table (GarnetDisplayOps.h garnetdisp_geom_for_index) maps it. Publishing one index reads and writes only that instance's state: the other nub, snapshot and flags are untouched.
//
//  What this file WRITES: nothing on the hardware. `fbpublish <inst>` reads the live plane gates (rdna4dcn::fbLive: reads only) and the display's EDID blocks 0 and 1 over its DDC line (rdna4dcn::fbEdid: the same
//  engine sequence as `accel ddcread`, the DC_I2C engine's registers only, through the DCN allowlist), then allocates one nub object and its property dictionary and registers the nub in the
//  IORegistry under the GPU's IOPCIDevice. The order is the contract: the software-only refusals (rdna4fb::pre_verdict) BEFORE any hardware read, the live gates BEFORE the DDC engine is touched,
//  the snapshot judged (garnetdisp_snap_valid) BEFORE the nub exists.
//
#include "GarnetDisplayNub.hpp"
#include "GarnetDriver.hpp"
#include "GarnetDisplayOps.h"
#include "amd/native_display_identity.h"   // the canonical retained primary on this GPU
#include "amd/native_edid_map_pure.h"      // proven connector -> local DDC line / head table
#include "amd/native_fb_pure.h"
#include "amd/native_disp.h"          // rdna4disp_pipe_adopted
#include "amd/native_s1c.h"           // n1c_ws_is_open
#include "amd/amdgpu_log.h"
#include "dcn/dcn.hpp"         // fbView / fbLive / fbEdid
#include "dcn/disp2.h"         // RDNA4D2_GATES, RDNA4D2_GT_*
#include "dcn/dispread.h"      // RDNA4DR_ statuses

#include <IOKit/IOLib.h>
#include <libkern/OSAtomic.h>
#include <libkern/c++/OSData.h>
#include <libkern/c++/OSNumber.h>
#include <libkern/c++/OSString.h>
#include <pexpert/pexpert.h>

#define FBLOG(fmt, ...) AMDGPU_LOG("fb", fmt, ##__VA_ARGS__)

IOService *garnet_driver_pci(void);   // GarnetDriver.cpp: the GPU's IOPCIDevice (not retained)

OSDefineMetaClassAndStructors(GarnetDisplayNub, IOService)

// ---- state (one nub per boot; never withdrawn) ------------------------------------------------------------------------------------------------------------------------------------------
namespace {
constexpr unsigned   kNubs = 2;        // 0.0.658: slot 0 = display index 1 (the monitor B), slot 1 = display index 2 (the monitor A): slot = index - 1
IOLock              *gLock;            // created on first use; held across a whole fbpublish (serialises two callers, of either instance)
GarnetDisplayNub    *gNub[kNubs];      // our reference while the nub lives
volatile uint32_t    gExists[kNubs];   // 1 from the moment the nub is attached; NEVER cleared (no re-publish of that index this boot, even if the object is freed)
volatile uint32_t    gHaveSnap[kNubs]; // 1 once gSnap[slot] holds the published snapshot (set BEFORE the nub is registered, never cleared)
GarnetDispSnap          gSnap[kNubs];     // the IMMUTABLE snapshots: each written once, under gLock, before its registerService
uint8_t              gPropScratch[128]; // the accepted registry EDID source's block 0: scratch of the verb, used only under gLock
volatile UInt32      gFb2Latch = 0;    // 0 unread, 1 off, 2 on (boot-arg garnet-fb2)

void ensure_lock() {
	if (gLock) return;
	IOLock *l = IOLockAlloc();
	if (l && !OSCompareAndSwapPtr(nullptr, l, &gLock)) IOLockFree(l);
}

// boot-arg garnet-fb2 == 1, read and latched ONCE (first writer wins; never re-read)
bool fb2_latched_on() {
	UInt32 l = gFb2Latch;
	if (l == 0u) {
		uint32_t v = 0;
		const bool present = PE_parse_boot_argn("garnet-fb2", &v, sizeof(v));
		OSCompareAndSwap(0u, rdna4fb::fb2_on(present, v) ? 2u : 1u, &gFb2Latch);
		l = gFb2Latch;
		FBLOG("boot-arg garnet-fb2 %s: fbpublish is %s for this boot", present ? (v == 1u ? "=1" : "present but not 1") : "absent", l == 2u ? "ENABLED" : "OFF");
	}
	return l == 2u;
}

// ---- the ops table's hooks (GarnetDisplayOps.h) -------------------------------------------------------------------------------------------------------------------------------------------
int op_snapshot(void *nub, GarnetDispSnap *out) {
	int slot = -1;                                          // the slot whose nub this is (identity); a nub of ours answers only with ITS OWN index's snapshot
	for (unsigned i = 0; i < kNubs; i++) if (nub != nullptr && nub == (void *)gNub[i]) slot = (int)i;
	const bool ok = rdna4fb::snapshot_hook_verdict(slot >= 0, slot >= 0 && __atomic_load_n(&gHaveSnap[slot], __ATOMIC_ACQUIRE) != 0u, out != nullptr) == 0;
	if (!ok) return -1;
	*out = gSnap[slot];                                     // gSnap[slot] is immutable once gHaveSnap[slot] is 1
	return 0;
}
int op_power(void *nub, uint32_t state) {
	(void)nub;
	FBLOG("framebuffer power state %u recorded (M5: no hardware action; the monitor B keeps its last image)", state);
	return 0;
}
void op_trace(uint32_t ev, uint64_t a, uint64_t b) {
	FBLOG("framebuffer trace: event %u a %#llx b %#llx", ev, (unsigned long long)a, (unsigned long long)b);
}
const GarnetDispOps gDispOps = {
	GARNET_DISP_OPS_MAGIC, GARNET_DISP_ABI, (uint32_t)sizeof(GarnetDispOps), 673u, 0u, 0u,
	op_snapshot, op_power, op_trace,
	nullptr, nullptr, nullptr,
};

// The EDID cross-check source. The owner is the canonical retained primary of
// THIS GPU (root's native_display_identity helper), never the first
// IOFramebuffer that happens to be a PCI child.
//
// Legacy local RDNA4FB publishes only "EDID,DDC<line>". The upstream display
// backend publishes the same key for an HDMI/DVI sink and "EDID,AUX<line>" for
// a DP/USB-C sink, plus AtomBIOS,Connectors and (local 0004) a structured
// "...-Connector" descriptor. An upstream EDID is converted for a nub only
// when the connector list names exactly one connector on this route's DDC
// line, the descriptor repeats the same line/HPD/type, the property's bus
// agrees with that type, and any head the descriptor declares is this route's
// OTG. The caller then compares block 0 with its own DDC read.
struct RetainedProperty {
	OSObject *object { nullptr };
	RetainedProperty(IOService *svc, const char *key) { object = svc && key ? svc->copyProperty(key) : nullptr; }
	~RetainedProperty() { if (object) object->release(); }
	RetainedProperty(const RetainedProperty &) = delete;
	RetainedProperty &operator=(const RetainedProperty &) = delete;
	template <class T> T *cast() const { return OSDynamicCast(T, object); }
};

bool same_data(IOService *svc, const char *key, OSData *first) {
	RetainedProperty again(svc, key);
	OSData *d = again.cast<OSData>();
	return d && first && d->getBytesNoCopy() && first->getBytesNoCopy() &&
	       d->getLength() == first->getLength() &&
	       memcmp(d->getBytesNoCopy(), first->getBytesNoCopy(), d->getLength()) == 0;
}

bool same_string(IOService *svc, const char *key, OSString *first) {
	RetainedProperty again(svc, key);
	OSString *s = again.cast<OSString>();
	const char *a = s ? s->getCStringNoCopy() : nullptr;
	const char *b = first ? first->getCStringNoCopy() : nullptr;
	return a && b && strcmp(a, b) == 0;
}

bool same_number(IOService *svc, const char *key, OSNumber *first) {
	RetainedProperty again(svc, key);
	OSNumber *n = again.cast<OSNumber>();
	return n && first && n->unsigned64BitValue() == first->unsigned64BitValue();
}

bool rdna4fb_edid_prop(uint32_t index, const char *name, uint8_t out[128]) {
	if (!name || !out) return false;
	rdna4edid::Bus legacyBus = rdna4edid::kBusNone;
	uint32_t propLine = 0u;
	if (!rdna4edid::parse_edid_property(name, &legacyBus, &propLine)) {
		FBLOG("fbpublish: EDID property %s is not EDID,AUX<line> / EDID,DDC<line>", name);
		return false;
	}
	rdna4edid::Route route;
	if (!rdna4edid::route_for_ddc(propLine, &route)) {
		FBLOG("fbpublish: DDC line %u is not one of the local nub routes", propLine);
		return false;
	}
	if (route.index != index) {
		FBLOG("fbpublish: DDC line %u belongs to display index %u, not %u: EDID source REFUSED", propLine, route.index, index);
		return false;
	}
	IOService *svc = nullptr;
	uint64_t primaryGeneration = 0u;
	if (!rdna4_display_primary(&svc, nullptr, &primaryGeneration)) {
		FBLOG("fbpublish: no canonical retained primary framebuffer on this GPU: EDID source REFUSED");
		return false;
	}
	bool found = false;
	RetainedProperty upstreamMarker(svc, "Garnet,ConnectorTable");
	const bool upstream = upstreamMarker.object != nullptr;
	if (upstream) {
		RetainedProperty connectorsProp(svc, "AtomBIOS,Connectors");
		RetainedProperty connectorCountProp(svc, "AtomBIOS,ConnectorCount");
		OSString *connectors = connectorsProp.cast<OSString>();
		OSNumber *connectorCount = connectorCountProp.cast<OSNumber>();
		rdna4edid::ConnectorList list {};
		if (!connectors || !connectorCount ||
		    rdna4edid::parse_connectors(connectors->getCStringNoCopy(), &list) != rdna4edid::kParseOk ||
		    !rdna4edid::count_matches((uint32_t)connectorCount->unsigned32BitValue(), list)) {
			FBLOG("fbpublish: upstream connector list missing, malformed or inconsistent: EDID source REFUSED");
		} else {
			const rdna4edid::Connector *hit = nullptr;
			uint32_t hits = 0u;
			for (uint32_t i = 0u; i < list.count; ++i)
				if (list.c[i].ddcLine == route.ddcLine) { hit = &list.c[i]; ++hits; }
			rdna4edid::Bus typeBus = rdna4edid::kBusNone;
			if (hits != 1u) {
				FBLOG("fbpublish: %u connectors on DDC line %u (need exactly one): EDID source REFUSED", hits, route.ddcLine);
			} else if (!rdna4edid::bus_for_type(hit->type, &typeBus)) {
				FBLOG("fbpublish: connector %u on DDC line %u has no EDID-capable bus: REFUSED", hit->index, hit->ddcLine);
			} else {
				char edidKey[32], descKey[40];
				// The upstream property's bus comes from the proven connector
				// type, not from the legacy EDID,DDC<line> argument.
				snprintf(edidKey, sizeof edidKey, "EDID,%s%u", rdna4edid::bus_name(typeBus), propLine);
				snprintf(descKey, sizeof descKey, "EDID,%s%u-Connector", rdna4edid::bus_name(typeBus), propLine);
				RetainedProperty edidProp(svc, edidKey);
				RetainedProperty descProp(svc, descKey);
				OSData *edidData = edidProp.cast<OSData>();
				OSData *descData = descProp.cast<OSData>();
				rdna4edid::ConnectorDesc desc {};
				const bool descPresent = descData != nullptr;
				const bool descOk = descPresent && descData->getBytesNoCopy() &&
				                    rdna4edid::parse_desc(static_cast<const uint8_t *>(descData->getBytesNoCopy()), descData->getLength(), &desc);
				uint32_t v = rdna4edid::kPropertyMissing;
				if (!edidData || !edidData->getBytesNoCopy()) {
					FBLOG("fbpublish: upstream property %s missing: EDID source REFUSED", edidKey);
				} else {
					const size_t edidLen = edidData->getLength();
					v = rdna4edid::evaluate(route, list, &desc, descOk, typeBus, propLine, (uint32_t)edidLen);
					if (v == rdna4edid::kDescMissing && descPresent) v = rdna4edid::kDescBad;
					if (v == rdna4edid::kOk && edidLen >= 128u) {
						// The registry has no multi-property transaction. The
						// first copies stay alive; read every input again and
						// require identical bytes/count/list before accepting
						// this EDID/descriptor pair.
						const bool stable = same_string(svc, "AtomBIOS,Connectors", connectors) &&
						                    same_number(svc, "AtomBIOS,ConnectorCount", connectorCount) &&
						                    same_data(svc, edidKey, edidData) &&
						                    same_data(svc, descKey, descData);
						if (!stable) {
							v = rdna4edid::kDescMismatch;
							FBLOG("fbpublish: upstream EDID/connector properties changed while they were read: REFUSED");
						} else {
							memcpy(out, edidData->getBytesNoCopy(), 128u);
							found = true;
						}
					}
					FBLOG("fbpublish: upstream %s (%zu bytes), connector %u type %u ddc%u hpd%u, head %s%u -> %u (%s)",
					      edidKey, edidLen, hit->index, hit->type, hit->ddcLine, hit->hpdPin,
					      desc.headProven ? "OTG" : "unclaimed/", desc.headProven ? desc.headOtg : 0u,
					      v, rdna4edid::verdict_name(v));
				}
			}
		}
	} else {
		const OSMetaClass *mc = svc->getMetaClass();
		const char *className = mc ? mc->getClassName() : nullptr;
		RetainedProperty publisherProp(svc, "IOPersonalityPublisher");
		RetainedProperty bundleProp(svc, "CFBundleIdentifier");
		OSString *publisher = publisherProp.cast<OSString>();
		OSString *bundleID = bundleProp.cast<OSString>();
		const char *pub = publisher ? publisher->getCStringNoCopy() : nullptr;
		const char *bid = bundleID ? bundleID->getCStringNoCopy() : nullptr;
		const bool builtinOwner = rdna4edid::builtin_owner_identity(className, pub, bid);
		const uint32_t v = rdna4edid::legacy_evaluate(route, legacyBus, propLine, 128u);
		RetainedProperty legacyProp(svc, name);
		OSData *d = legacyProp.cast<OSData>();
		if (!builtinOwner) {
			FBLOG("fbpublish: primary class %s is neither the upstream backend nor a proven local primary leaf: EDID source REFUSED", className ? className : "(null)");
		} else if (v != rdna4edid::kOk) {
			FBLOG("fbpublish: legacy EDID property %s refused: %s", name, rdna4edid::verdict_name(v));
		} else if (d && d->getLength() == 128u && d->getBytesNoCopy()) {
			if (!same_data(svc, name, d)) {
				FBLOG("fbpublish: legacy EDID property changed while it was read: REFUSED");
			} else {
				memcpy(out, d->getBytesNoCopy(), 128u);
				found = true;
			}
		} else {
			FBLOG("fbpublish: legacy EDID property %s missing or not 128 bytes", name);
		}
	}
	// The property set was read over several calls; a primary handoff or
	// generation change in between invalidates every value just copied.
	if (found) {
		IOService *again = nullptr;
		uint64_t generation = 0u;
		const bool same = rdna4_display_primary(&again, nullptr, &generation) &&
		                  again == svc && generation == primaryGeneration;
		if (again) again->release();
		if (!same) {
			FBLOG("fbpublish: primary changed while its EDID source was read: REFUSED");
			found = false;
		}
	}
	svc->release();
	return found;
}
}  // namespace

bool rdna4fb_nub_exists_idx(uint32_t index) { return index >= 1u && index <= kNubs && __atomic_load_n(&gExists[index - 1u], __ATOMIC_ACQUIRE) != 0u; }      // 0.0.658: the nub of ONE display index
bool rdna4fb_nub_exists(void) { return rdna4fb_nub_exists_idx(GARNET_DISP_INDEX) || rdna4fb_nub_exists_idx(GARNET_DISPA_INDEX); }                                         // ANY display nub (the interlocks: pipeadopt, pipearm 1, the Metal publish)

// ---- the verb ------------------------------------------------------------------------------------------------------------------------------------------------------------------------------
uint32_t GarnetDisplayNub::publishVerb(GarnetDriver *owner, uint64_t arg, uint64_t *out, unsigned count) {
	(void)owner;
	if (!out || count < 13u) return rdna4fb::kBadArg;
	for (unsigned i = 0; i < count; i++) out[i] = 0u;
	ensure_lock();
	if (!gLock) { out[0] = rdna4fb::kPublishFailed; return rdna4fb::kPublishFailed; }
	IOLockLock(gLock);
	uint32_t st;
	GarnetDisplayNub *nub = nullptr;
	// 0.0.658: the argument is the disp2 INSTANCE; ONE table (garnetdisp_geom_for_index) gives its display index, OTG, DDC line and mode. A bad argument leaves index 0 / no geometry and is refused by pre_verdict before anything is used.
	const uint32_t inst = rdna4fb::inst_of_arg(arg), index = rdna4fb::index_of_arg(arg);
	GarnetDispGeom geom;
	bzero(&geom, sizeof geom);
	const bool haveGeom = garnetdisp_geom_for_index(index, &geom) != 0;
	rdna4edid::Route route;
	bzero(&route, sizeof route);
	const bool routeOk = rdna4edid::route_for_inst(inst, &route) && route.nub &&
	                     route.index == index && route.otg == geom.otg &&
	                     route.ddcLine == geom.ddcLine && route.w == geom.w && route.h == geom.h;
	do {
		// ---- phase 1: software facts only (no register is read)
		rdna4dcn::FbView v;
		bzero(&v, sizeof v);
		rdna4dcn::fbView(&v, inst);
		rdna4fb::Pre pre;
		bzero(&pre, sizeof pre);
		pre.argOk = rdna4fb::arg_ok(arg) && haveGeom && routeOk;
		pre.latched = fb2_latched_on();
		pre.nubExists = rdna4fb_nub_exists_idx(index);                      // THIS index's nub: publishing the monitor B never blocks the monitor A and the reverse
		pre.held = v.held; pre.pinned = v.pinned; pre.devOk = v.devOk;
		pre.pipeAdopted = rdna4disp_pipe_adopted();
		pre.wsOpen = amdgpu::n1c_ws_is_open();
		pre.metalNub = garnetmetal_nub_published();                         // 0.0.653: the Metal nub (and the accelerator behind it) was published earlier this boot
		pre.m6 = rdna4m6_latched_on();                                     // 0.0.659 (M6 Stage 1a, R2): under garnet-m6 the three interlocks above are lifted (the routing guard replaces them)
		st = rdna4fb::pre_verdict(pre);
		FBLOG("fbpublish %llu (instance %u -> display index %u, OTG%u, DDC line %u): arg %d latched %d nub %d held %d pinned %d dev %d pipe-adopted %d ws-open %d metal-nub %d -> %u (%s)", (unsigned long long)arg, inst, index, geom.otg, geom.ddcLine,
		      (int)pre.argOk, (int)pre.latched, (int)pre.nubExists, (int)pre.held, (int)pre.pinned, (int)pre.devOk, (int)pre.pipeAdopted, (int)pre.wsOpen, (int)pre.metalNub, st, rdna4fb::status_name(st));
		if (st != rdna4fb::kOk) break;
		// ---- phase 2a: the plane's live gates (READS ONLY), before the DDC engine is touched
		uint32_t holdBad = 0xFFFFFFFFu, gates[RDNA4D2_GATES];
		for (uint32_t i = 0; i < RDNA4D2_GATES; i++) gates[i] = 0u;
		const uint32_t ls = rdna4dcn::fbLive(inst, &holdBad, gates);
		if (ls == RDNA4D2_BUSY) { st = rdna4fb::kBusy; break; }
		if (ls != RDNA4D2_OK) { st = rdna4fb::kNoDevice; break; }
		out[10] = ((uint64_t)(gates[RDNA4D2_GT_EARLY_HI] & 0xFFFFu) << 32) | gates[RDNA4D2_GT_EARLY_LO];
		out[11] = (uint64_t)gates[RDNA4D2_GT_HUBP_CNTL] | ((uint64_t)gates[RDNA4D2_GT_FLIP_CTL] << 32);
		out[12] = (uint64_t)gates[RDNA4D2_GT_VMFAULT] | ((uint64_t)holdBad << 32);
		FBLOG("fbpublish: live gates (instance %u): EARLIEST_INUSE %#llx (A %#llx) HUBP<inst>_DCHUBP_CNTL %#010x FLIP_CONTROL %#010x DCN_VM_FAULT_STATUS %#010x -> hold verdict %#x", inst, (unsigned long long)out[10], (unsigned long long)v.mc[0],
		      gates[RDNA4D2_GT_HUBP_CNTL], gates[RDNA4D2_GT_FLIP_CTL], gates[RDNA4D2_GT_VMFAULT], holdBad);
		st = rdna4fb::live_verdict(holdBad);
		if (st != rdna4fb::kOk) break;
		// ---- phase 2b: the display's EDID (blocks 0 and 1, its DDC line), then the registry
		// source (legacy EDID,DDC<line>, or a provenance-checked upstream AUX/DDC conversion)
		uint8_t edid[256];
		uint8_t *const prop = gPropScratch;
		bzero(prop, 128u);
		const uint32_t es = rdna4dcn::fbEdid(geom.ddcLine, edid);
		if (es == RDNA4DR_BUSY) { st = rdna4fb::kBusy; break; }
		rdna4fb::Data d;
		bzero(&d, sizeof d);
		d.edidStatus = es;
		d.block0Sum = garnetdisp_edid_sum_ok(edid); d.block0Hdr = garnetdisp_edid_header_ok(edid); d.block1Sum = garnetdisp_edid_sum_ok(edid + 128);
		d.propPresent = rdna4fb_edid_prop(index, rdna4fb::edid_prop_name(geom.ddcLine), prop);
		d.block0EqProp = d.propPresent && memcmp(edid, prop, 128u) == 0;
		d.apertureOk = rdna4fb::aperture_ok_len(v.bar0Phys, v.bar0Size, v.offA, geom.aperLen) && rdna4fb::len_in_alloc(geom.aperLen, v.bufBytes);
		out[9] = (uint64_t)d.block0Sum | ((uint64_t)d.block0Hdr << 1) | ((uint64_t)d.block1Sum << 2) | ((uint64_t)d.propPresent << 3) | ((uint64_t)d.block0EqProp << 4) | ((uint64_t)d.apertureOk << 5) | ((uint64_t)es << 8);
		FBLOG("fbpublish: EDID read status %u (%s); block 0 checksum %d header %d, block 1 checksum %d; primary %s %s, block 0 %s it; aperture BAR0 phys %#llx + offset %#llx (BAR0 size %#llx, %llu bytes, plane buffer %llu bytes) %s",
		      es, rdna4dr_status_name(es), (int)d.block0Sum, (int)d.block0Hdr, (int)d.block1Sum, rdna4fb::edid_prop_name(geom.ddcLine), d.propPresent ? "present" : "MISSING", d.block0EqProp ? "EQUALS" : "DIFFERS from",
		      (unsigned long long)v.bar0Phys, (unsigned long long)v.offA, (unsigned long long)v.bar0Size, (unsigned long long)geom.aperLen, (unsigned long long)v.bufBytes, d.apertureOk ? "ok" : "REFUSED");
		st = rdna4fb::data_verdict(d);
		if (st != rdna4fb::kOk) break;
		// ---- the snapshot, judged before the nub exists
		GarnetDispSnap snap;
		rdna4fb::snap_build_idx(&snap, index, v.bar0Phys, v.offA, v.mc[0], v.mc[1], edid);
		const uint32_t sv = garnetdisp_snap_valid(&snap);
		if (sv != GARNET_DSV_OK) { FBLOG("fbpublish: the snapshot failed validation (verdict %u)", sv); st = rdna4fb::kSnapshot; break; }
		// ---- publish: the nub is created, the snapshot stored BEFORE registration (the aux kext reads it in start), the nub attached to the GPU's PCI device
		const unsigned slot = index - 1u;
		IOService *pci = garnet_driver_pci();
		OSDictionary *props = OSDictionary::withCapacity(3);
		nub = OSTypeAlloc(GarnetDisplayNub);
		st = rdna4fb::kPublishFailed;
		if (pci && props && nub && nub->init(props)) {
			OSNumber *idx = OSNumber::withNumber((uint64_t)index, 32);
			OSNumber *abi = OSNumber::withNumber((uint64_t)GARNET_DISP_ABI, 32);
			if (idx && abi) {
				nub->setProperty(GARNET_DISP_INDEX_KEY, idx);
				nub->setProperty("Garnet,DisplayABI", abi);
				gSnap[slot] = snap;
				__atomic_store_n(&gHaveSnap[slot], 1u, __ATOMIC_RELEASE);
				if (nub->attach(pci)) {
					gNub[slot] = nub;                                                 // our reference (from alloc) is kept for the rest of the boot
					__atomic_store_n(&gExists[slot], 1u, __ATOMIC_RELEASE);
					st = rdna4fb::kOk;
				} else __atomic_store_n(&gHaveSnap[slot], 0u, __ATOMIC_RELEASE);
			}
			if (idx) idx->release();
			if (abi) abi->release();
		}
		if (props) props->release();
		if (st != rdna4fb::kOk) break;
		out[1] = nub->getRegistryEntryID();
		out[2] = snap.aperPhys; out[3] = snap.aperLen; out[4] = snap.mcA; out[5] = snap.mcB;
		out[6] = (uint64_t)snap.w | ((uint64_t)snap.h << 16) | ((uint64_t)snap.index << 32);
		out[7] = (uint64_t)snap.refresh1616 | ((uint64_t)snap.edidLen << 32);
		out[8] = snap.pixHz;
		nub->retain();                                                                // held across registerService(): see below
	} while (false);
	if (st != rdna4fb::kOk) {
		IOLockUnlock(gLock);
		if (nub && !rdna4fb_nub_exists_idx(index)) nub->release();                     // a failed publish: AFTER the unlock
		out[0] = st;
		FBLOG("fbpublish %llu: REFUSED / FAILED: %s", (unsigned long long)arg, rdna4fb::status_name(st));
		return st;
	}
	IOLockUnlock(gLock);
	FBLOG("fbpublish: nub published (id %llu), GarnetDisplayIndex %u (instance %u), aperture %#llx + %#llx, A %#llx B %#llx, %ux%u refresh %#x: the aux framebuffer may now match it",
	      (unsigned long long)out[1], index, inst, (unsigned long long)out[2], (unsigned long long)out[3], (unsigned long long)out[4], (unsigned long long)out[5], geom.w, geom.h, (unsigned)geom.refresh1616);
	nub->registerService();                                                           // outside the lock: matching runs the aux kext's start, which calls the ops
	nub->release();                                                                   // the extra reference taken above
	out[0] = rdna4fb::kOk;
	return rdna4fb::kOk;
}

IOService *GarnetDisplayNub::framebufferOf(uint32_t index) {
	if (index < 1u || index > kNubs || !gLock) return nullptr;
	IOLockLock(gLock);
	GarnetDisplayNub *nub = gNub[index - 1u];
	if (nub) nub->retain();
	IOLockUnlock(gLock);
	if (!nub) return nullptr;
	RetainedProperty idxProp(nub, GARNET_DISP_INDEX_KEY);
	OSNumber *idx = idxProp.cast<OSNumber>();
	const uint32_t nubIndex = idx ? (uint32_t)idx->unsigned32BitValue() : 0u;
	GarnetDispSnap snap;
	bzero(&snap, sizeof snap);
	const bool haveSnap = __atomic_load_n(&gHaveSnap[index - 1u], __ATOMIC_ACQUIRE) != 0u &&
	                      op_snapshot(nub, &snap) == 0 && snap.index == index &&
	                      garnetdisp_snap_valid(&snap) == GARNET_DSV_OK;
	IOService *found = nullptr;
	uint32_t matches = 0u;
	bool classOk = false;
	if (OSIterator *it = nub->getChildIterator(gIOServicePlane)) {
		while (OSObject *o = it->getNextObject()) {
			IOService *svc = OSDynamicCast(IOService, o);
			const OSMetaClass *mc = svc ? svc->getMetaClass() : nullptr;
			const char *cn = mc ? mc->getClassName() : nullptr;
			if (!svc || svc->getProvider() != nub || !svc->metaCast("IOFramebuffer")) continue;
			++matches;
			if (cn && strcmp(cn, "GarnetFramebuffer") == 0 && matches == 1u) { found = svc; found->retain(); classOk = true; }
			else if (found) { found->release(); found = nullptr; }      // more than one: never pick "the first"
		}
		it->release();
	}
	IOLockLock(gLock);
	const bool stillOwned = gNub[index - 1u] == nub;
	IOLockUnlock(gLock);
	nub->release();
	const bool indexOk = nubIndex == index && snap.index == index;
	const bool opsOk = garnetdisp_ops_check_for(&gDispOps, index) == GARNET_DOV_OK;
	const uint32_t v = rdna4edid::framebuffer_verdict(stillOwned, opsOk, indexOk, haveSnap, matches, classOk);
	if (v != rdna4edid::kFbOk) {
		if (found) found->release();
		FBLOG("framebufferOf(%u): REFUSED: %s (%u framebuffer child(ren))", index, rdna4edid::fb_verdict_name(v), matches);
		return nullptr;
	}
	return found;
}

IOService *GarnetDisplayNub::published() { return gNub[0] ? gNub[0] : gNub[1]; }      // 0.0.658: the first published nub (the monitor B's, else the monitor A's); NOT retained

void GarnetDisplayNub::shutdown() {
	if (!gLock) return;
	for (unsigned i = 0; i < kNubs; i++) {
		IOLockLock(gLock);
		GarnetDisplayNub *nub = gNub[i];
		gNub[i] = nullptr;
		IOLockUnlock(gLock);
		if (nub) { nub->terminate(); nub->release(); FBLOG("display nub (index %u) terminated at kext stop", i + 1u); }
	}
}

IOReturn GarnetDisplayNub::callPlatformFunction(const OSSymbol *functionName, bool waitForFunction, void *param1, void *param2, void *param3, void *param4) {
	if (functionName && functionName->isEqualTo(GARNET_DISP_FN_SYMBOL)) {
		if (!param1) return kIOReturnBadArgument;
		*(const GarnetDispOps **)param1 = &gDispOps;
		return kIOReturnSuccess;
	}
	return super::callPlatformFunction(functionName, waitForFunction, param1, param2, param3, param4);
}

void GarnetDisplayNub::free() {
	for (unsigned i = 0; i < kNubs; i++) if (gNub[i] == this) gNub[i] = nullptr;      // gExists[i] stays 1: no re-publish of that index this boot, and the interlocks stay on
	super::free();
}
