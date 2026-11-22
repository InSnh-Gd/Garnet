#include "dcn_internal.hpp"

namespace rdna4dcn {

// =====================================================================================================================================================================================================
// build 0.0.622 (multi-monitor track, stage M1; an internal design note): THE THREE READ-ONLY INSTRUMENTS. Decisions: dispread.h; sequences: dispread_flow.h; host test:
// tests/native_dispread_test.cpp (with planted breaks in tests/native_dispread_plant.sh).
//   ddcread   <line> <block>  the ONLY one of the three that writes registers: the DC_I2C engine's own (BASE_IDX 2, absolute 0x5358..0x539e, allowlist range "DC..DIG6" 0x5358..0x53d3), each write through
//             dcn41_allow_write, and only after the arbitration read says the engine is free. Needs bind() (the allowlist), like the DCN verbs 74-77.
//   dmubring  [page]          register READS through the read-only device attach() built; ring memory only through amdgpu::RBAR0_32 (the BAR0 aperture) - NEVER MM_INDEX. This card's ring sits at VRAM offset
//             ~15.7 GiB (REGION4, an earlier analysis), outside the 256 MiB aperture, so the verb reports RING_UNREACHABLE and the decode half is exercised by the host test only.
//   dispcensus [page]         register READS through the same read-only device.
// =====================================================================================================================================================================================================
namespace {
struct DdcStore {
	bool     valid { false };
	uint32_t line { 0 }, block { 0 }, status { 0 }, arb { 0 }, arbAfter { 0 }, sw { 0 }, seq { 0 };
	bool     released { false };
	uint8_t  data[RDNA4DR_EDID_BLOCK] { };
};
DdcStore gDdc;
volatile uint32_t gDdcBusy = 0;
uint32_t gDdcSeq = 0;

// The environment dispread_flow.h's ddc_read runs over: reads through the bound device (counted), writes ONLY through the allowlist.
struct DdcKextEnv {
	bool refusedFlag { false };
	uint32_t rd(uint32_t baseIdx, uint32_t off) {
		const uint32_t a = dcn41_abs_rd(&gDcn.d, off, baseIdx);   // 0.0.628: the READ-ONLY twin (BASE_IDX 1 = the prescale register); wr2 below keeps dcn41_abs
		return a == DCN41_BAD_OFFSET ? 0xFFFFFFFFu : dcn_rreg(&gDcn, a);
	}
	void wr2(uint32_t off, uint32_t v) {
		if (!dcn_display_txn_ok("ddcread register write")) { refusedFlag = true; return; }
		const uint32_t a = dcn41_abs(&gDcn.d, off, 2u);
		if (a == DCN41_BAD_OFFSET || !dcn41_allow_write(&gDcn.allow, a, v, "ddcread")) {
			refusedFlag = true;
			RDNA4LOG("ddcread: REFUSED write offset %#x = %#010x (abs %#010x) by the allowlist", off, v, a);
			return;
		}
		amdgpu::WREG32(*gDcn.dev, a, v);
	}
	bool refused() const { return refusedFlag; }
	void delay_us(uint32_t us) { dcn_udelay(nullptr, us); }   // the file's ONE delay glue (IODelay); tests/native_s2d_test.cpp pins that there is exactly one
};
// Reads only, through the read-only device (no bind needed).
struct RoKextEnv {
	uint32_t rd(uint32_t baseIdx, uint32_t off) {
		const uint32_t a = dcn41_abs_rd(&gLr.d, off, baseIdx);   // 0.0.628 (M4a): BASE_IDX 1 / 3 resolve for READS; this environment has no write member
		return a == DCN41_BAD_OFFSET ? 0xFFFFFFFFu : gLr.d.rreg(gLr.d.cookie, a);
	}
};
// The ring memory reader: the BAR0 aperture and nothing else.
struct Bar0Mem {
	amdgpu::DeviceContext *dev;
	uint32_t rd32(uint64_t off) { return amdgpu::RBAR0_32(*dev, off); }
};
}  // namespace

// build 0.0.624 (stage M1.5): the kext's existing serialised MM_INDEX READER (GarnetDriver.cpp; gVramMmLock and the MM-priority scope inside it). Declared here, defined there; region4read's ONLY VRAM access.

namespace {
// region4read's memory: the sequence (rdna4dr::region4_read) can only call rd(), which is the read-only reader above. There is no write member to call.
struct R4Mem {
	bool rd(uint64_t vramOff, uint32_t *dst, uint32_t n) { return rdna4_vram_read_mm(vramOff, dst, n); }
};
struct R4Store {
	bool     valid { false };
	uint32_t status { 0 }, seq { 0 }, off { 0 }, dwords { 0 };
	uint64_t base { 0 };
	uint32_t d[RDNA4DR_R4_MAX_DWORDS] { };
};
R4Store gR4;
volatile uint32_t gR4Busy = 0;
uint32_t gR4Seq = 0;
}  // namespace


uint32_t dcn_ddc_edid_read(uint32_t ddcLine, uint8_t out[256]) {
	if (__atomic_exchange_n(&gDdcBusy, 1u, __ATOMIC_ACQ_REL) != 0u) return RDNA4DR_BUSY;
	uint32_t status = RDNA4DR_OK;
	DdcKextEnv env;
	for (uint32_t block = 0; block < GARNET_DISP_EDID_BLOCKS && status == RDNA4DR_OK; block++) {
		const rdna4dr::DdcResult r = rdna4dr::ddc_read(env, ddcLine, block);
		status = r.status;
		RDNA4LOG("fbpublish: DDC line %u block %u -> status %u (%s); released %u", ddcLine, block, r.status, rdna4dr_status_name(r.status), r.released ? 1u : 0u);
		if (r.status == RDNA4DR_OK) for (unsigned i = 0; i < RDNA4DR_EDID_BLOCK; i++) out[block * RDNA4DR_EDID_BLOCK + i] = r.data[i];
	}
	++gDdcSeq;
	__atomic_store_n(&gDdcBusy, 0u, __ATOMIC_RELEASE);
	return status;
}

uint32_t ddcRead(uint64_t arg, uint64_t *out, unsigned outCount) {
	if (!out || outCount < 13u) return RDNA4DR_BAD_ARG;
	for (unsigned i = 0; i < 13u; i++) out[i] = 0u;
	rdna4_display_txn txn(rdna4peer::kLeaseLocalDisplay);
	if (!txn.ok) { rdna4dr_ddc_fill(out, RDNA4DR_BUSY, 0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u, nullptr); return RDNA4DR_BUSY; }
	if (!rdna4dr_ddc_arg_ok(arg)) { rdna4dr_ddc_fill(out, RDNA4DR_BAD_ARG, 0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u, nullptr); return RDNA4DR_BAD_ARG; }
	const uint32_t block = (uint32_t)(arg & 0xFFu), line = (uint32_t)((arg >> 8) & 0xFFu), page = (uint32_t)((arg >> 16) & 0xFFu);
	if (!__atomic_load_n(&gDcn.armed, __ATOMIC_ACQUIRE) || !gDcn.dev) { rdna4dr_ddc_fill(out, RDNA4DR_NO_DEVICE, page, 0u, 0u, 0u, 0u, 0u, 0u, 0u, nullptr); return RDNA4DR_NO_DEVICE; }
	if (__atomic_exchange_n(&gDdcBusy, 1u, __ATOMIC_ACQ_REL) != 0u) { rdna4dr_ddc_fill(out, RDNA4DR_BUSY, page, 0u, 0u, 0u, 0u, 0u, 0u, 0u, nullptr); return RDNA4DR_BUSY; }
	uint32_t status;
	if (page == 1u) {                                              // the stored bytes 88..127 of the last page-0 read of THIS (line, block): no bus access at all
		if (!gDdc.valid || gDdc.line != line || gDdc.block != block) {
			status = RDNA4DR_NO_DATA;
			rdna4dr_ddc_fill(out, status, page, 0u, 0u, 0u, 0u, 0u, 0u, 0u, nullptr);
		} else {
			status = gDdc.status;
			rdna4dr_ddc_fill(out, status, page, rdna4dr_edid_sum(gDdc.data) == 0u && status == RDNA4DR_OK, block == 0u && rdna4dr_edid_header_ok(gDdc.data), gDdc.released, gDdc.seq, gDdc.arbAfter, gDdc.arb, gDdc.sw, gDdc.data);
		}
	} else {
		DdcKextEnv env;
		const rdna4dr::DdcResult r = rdna4dr::ddc_read(env, line, block);
		gDdc.valid = true; gDdc.line = line; gDdc.block = block; gDdc.status = r.status; gDdc.arb = r.arb; gDdc.arbAfter = r.arbAfter; gDdc.sw = r.swStatus;
		gDdc.released = r.released; gDdc.seq = ++gDdcSeq;
		for (unsigned i = 0; i < RDNA4DR_EDID_BLOCK; i++) gDdc.data[i] = r.status == RDNA4DR_OK ? r.data[i] : 0u;
		status = r.status;
		const bool csum = status == RDNA4DR_OK && rdna4dr_edid_sum(gDdc.data) == 0u;
		rdna4dr_ddc_fill(out, status, 0u, csum, block == 0u && status == RDNA4DR_OK && rdna4dr_edid_header_ok(gDdc.data), r.released, gDdc.seq, r.arbAfter, r.arb, r.swStatus, status == RDNA4DR_OK ? gDdc.data : nullptr);
		RDNA4LOG("ddcread: line %u block %u -> status %u (%s); arb %#010x after release %#010x, sw_status %#010x, polls %u, released %u, checksum %s",
		       line, block, status, rdna4dr_status_name(status), r.arb, r.arbAfter, r.swStatus, r.polls, r.released ? 1u : 0u, status != RDNA4DR_OK ? "n/a" : csum ? "OK" : "BAD");
		if (status == RDNA4DR_OK)
			for (unsigned row = 0; row < 8u; row++) {
				const uint8_t *d = gDdc.data + row * 16u;
				RDNA4LOG("ddcread: %03x: %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x", row * 16u,
				       d[0], d[1], d[2], d[3], d[4], d[5], d[6], d[7], d[8], d[9], d[10], d[11], d[12], d[13], d[14], d[15]);
			}
	}
	__atomic_store_n(&gDdcBusy, 0u, __ATOMIC_RELEASE);
	return status;
}

// build 0.0.633: verb 99 `scdcread <line> <off> [len]` - a READ-ONLY SCDC read from the HDMI sink (slave 0x54) over the SAME DC_I2C engine sequence as ddcread (rdna4dr::i2c_xfer: arbitration verdict first,
// bounded poll, always released) and the same busy flag (one engine, one transaction at a time). The only bus write is the 1-byte register offset (rdna4dr_plan_scdc). Needs bind() like ddcread (the engine's registers are
// written through the DCN allowlist). The three-line answer is also in the driver log so an unattended run is readable without the CLI.
uint32_t scdcRead(uint64_t arg, uint64_t *out, unsigned outCount) {
	if (!out || outCount < 13u) return RDNA4DR_BAD_ARG;
	for (unsigned i = 0; i < 13u; i++) out[i] = 0u;
	rdna4_display_txn txn(rdna4peer::kLeaseLocalDisplay);
	if (!txn.ok) { rdna4dr_scdc_fill(out, RDNA4DR_BUSY, 0u, 0u, 0u, 0u, 0u, 0u, nullptr); return RDNA4DR_BUSY; }
	if (!rdna4dr_scdc_arg_ok(arg)) { rdna4dr_scdc_fill(out, RDNA4DR_BAD_ARG, 0u, 0u, 0u, 0u, 0u, 0u, nullptr); return RDNA4DR_BAD_ARG; }
	const uint32_t line = (uint32_t)(arg & 0xFFu), off = (uint32_t)((arg >> 8) & 0xFFu), len = (uint32_t)((arg >> 16) & 0xFFu);
	if (!__atomic_load_n(&gDcn.armed, __ATOMIC_ACQUIRE) || !gDcn.dev) { rdna4dr_scdc_fill(out, RDNA4DR_NO_DEVICE, len, 0u, 0u, 0u, 0u, 0u, nullptr); return RDNA4DR_NO_DEVICE; }
	if (__atomic_exchange_n(&gDdcBusy, 1u, __ATOMIC_ACQ_REL) != 0u) { rdna4dr_scdc_fill(out, RDNA4DR_BUSY, len, 0u, 0u, 0u, 0u, 0u, nullptr); return RDNA4DR_BUSY; }
	DdcKextEnv env;
	const rdna4dr::DdcResult r = rdna4dr::scdc_read(env, line, off, len);
	const uint32_t seq = ++gDdcSeq;
	rdna4dr_scdc_fill(out, r.status, len, r.released, seq, r.arbAfter, r.arb, r.swStatus, r.status == RDNA4DR_OK ? r.data : nullptr);
	RDNA4LOG("scdcread: line %u offset %#04x len %u -> status %u (%s); arb %#010x after release %#010x, sw_status %#010x, polls %u, released %u",
	       line, off, len, r.status, rdna4dr_status_name(r.status), r.arb, r.arbAfter, r.swStatus, r.polls, r.released ? 1u : 0u);
	if (r.status == RDNA4DR_OK) {
		RDNA4LOG("scdcread: %#04x: %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x", off,
		       r.data[0], len > 1u ? r.data[1] : 0u, len > 2u ? r.data[2] : 0u, len > 3u ? r.data[3] : 0u, len > 4u ? r.data[4] : 0u, len > 5u ? r.data[5] : 0u, len > 6u ? r.data[6] : 0u, len > 7u ? r.data[7] : 0u,
		       len > 8u ? r.data[8] : 0u, len > 9u ? r.data[9] : 0u, len > 10u ? r.data[10] : 0u, len > 11u ? r.data[11] : 0u, len > 12u ? r.data[12] : 0u, len > 13u ? r.data[13] : 0u, len > 14u ? r.data[14] : 0u, len > 15u ? r.data[15] : 0u);
	}
	__atomic_store_n(&gDdcBusy, 0u, __ATOMIC_RELEASE);
	return r.status;
}

uint32_t dmubRing(uint64_t arg, uint64_t *out, unsigned outCount) {
	if (!out || outCount < 13u) return RDNA4DR_BAD_ARG;
	for (unsigned i = 0; i < 13u; i++) out[i] = 0u;
	if (!rdna4dr_dmub_arg_ok(arg)) { out[0] = RDNA4DR_BAD_ARG; return RDNA4DR_BAD_ARG; }
	if (gLr.state != RDNA4LR_READY) { out[0] = RDNA4DR_NO_DEVICE; return RDNA4DR_NO_DEVICE; }
	RoKextEnv ro;
	if (arg == RDNA4DR_DMUB_PAGE_SCRATCH) {                           // the SCRATCH bank, registers only
		uint32_t vals[RDNA4DR_DMUB_SCRATCH_COUNT];
		for (unsigned i = 0; i < RDNA4DR_DMUB_SCRATCH_COUNT; i++) vals[i] = ro.rd(2u, RDNA4DR_DMUB_SCRATCH_FIRST + i);
		out[0] = (uint64_t)RDNA4DR_OK | ((uint64_t)RDNA4DR_DMUB_SCRATCH_COUNT << 8);
		rdna4dr_pack_regs(out, 1u, vals, RDNA4DR_DMUB_SCRATCH_COUNT);
		return RDNA4DR_OK;
	}
	struct dcn41_dmub_state st;
	if (dcn41_dmub_probe(&gLr.d, &st) != DCN41_OK) { out[0] = RDNA4DR_NO_DEVICE; return RDNA4DR_NO_DEVICE; }
	amdgpu::DeviceContext *dev = static_cast<amdgpu::DeviceContext *>(gLr.d.cookie);
	const uint64_t bar0Size = dev ? amdgpu::bar0_cpu_bound(*dev) : 0ull;
	const bool known = st.ring_map != DCN41_DMUB_MAP_UNKNOWN && st.ring_addr != 0ull;
	const uint64_t ringOff = (known && st.ring_addr >= st.fb_base_mc) ? st.ring_addr - st.fb_base_mc : ~0ull;
	uint32_t sane = 0u;
	const uint32_t ncmd = rdna4dr_ring_count(st.inbox1_size, st.inbox1_wptr, &sane);
	if (arg == 0u) {
		struct rdna4dr_dmub_summary s;
		for (unsigned i = 0; i < sizeof(s); i++) ((uint8_t *)&s)[i] = 0;
		const uint32_t pv = rdna4dr::ring_page_verdict(1u, known && ringOff != ~0ull, st.inbox1_size, st.inbox1_wptr, ringOff, bar0Size);
		s.status = pv == RDNA4DR_PAGE_RANGE ? (uint32_t)RDNA4DR_OK : pv;
		s.verdict = st.verdict; s.ring_map = st.ring_map; s.enabled = st.enabled; s.soft_reset = st.soft_reset; s.dal_fw = st.dal_fw; s.mailbox_rdy = st.mailbox_rdy;
		s.reachable = (known && ringOff != ~0ull && rdna4dr_ring_in_bar0(ringOff, ncmd ? (uint64_t)ncmd * RDNA4DR_DMUB_CMD_SIZE : (uint64_t)RDNA4DR_DMUB_CMD_SIZE, bar0Size)) ? 1u : 0u;
		s.ring_sane = sane; s.ncmd = ncmd;
		s.cntl = st.cntl; s.cntl2 = st.cntl2; s.sec_cntl = st.sec_cntl; s.scratch0 = st.scratch0; s.scratch7 = st.scratch7; s.scratch14 = st.scratch14; s.scratch15 = st.scratch15;
		s.fault_addr = st.fault_addr; s.inbox_base = st.inbox1_base; s.inbox_size = st.inbox1_size; s.inbox_wptr = st.inbox1_wptr; s.inbox_rptr = st.inbox1_rptr;
		s.region4_off = st.region4_offset; s.region4_off_hi = st.region4_offset_high; s.ring_addr = st.ring_addr; s.fb_base_mc = st.fb_base_mc; s.ring_vram_off = ringOff; s.bar0_size = bar0Size;
		rdna4dr_dmub_summary_pack(&s, out);
		RDNA4LOG("dmubring: summary status %u (%s); verdict %u map %u enabled %u reset %u dal_fw %u mailbox_rdy %u; SCRATCH0 %#010x CNTL %#010x; inbox base %#010x size %#x wptr %#x rptr %#x; "
		       "ring GPU %#llx = VRAM offset %#llx, BAR0 aperture %#llx, %u command(s), reachable %u",
		       s.status, rdna4dr_status_name(s.status), s.verdict, s.ring_map, s.enabled, s.soft_reset, s.dal_fw, s.mailbox_rdy, s.scratch0, s.cntl, s.inbox_base, s.inbox_size, s.inbox_wptr, s.inbox_rptr,
		       (unsigned long long)s.ring_addr, (unsigned long long)s.ring_vram_off, (unsigned long long)s.bar0_size, s.ncmd, s.reachable);
		return s.status;
	}
	// a command page: header + the first 32 payload bytes of command (page - 1), read through the BAR0 aperture ONLY
	const uint32_t page = (uint32_t)arg;
	const uint32_t pv = rdna4dr::ring_page_verdict(page, known && ringOff != ~0ull, st.inbox1_size, st.inbox1_wptr, ringOff, bar0Size);
	if (pv != RDNA4DR_OK) {
		rdna4dr_dmub_cmd_pack(out, pv, page - 1u, 0u, ncmd, nullptr);
		return pv;
	}
	Bar0Mem mem { dev };
	uint32_t header = 0u, payload[8] = { 0 };
	rdna4dr::ring_read_cmd(mem, ringOff, page - 1u, &header, payload);
	rdna4dr_dmub_cmd_pack(out, RDNA4DR_OK, page - 1u, header, ncmd, payload);
	const struct rdna4dr_dmub_hdr h = rdna4dr_dmub_decode(header);
	RDNA4LOG("dmubring: command %u: header %#010x type %u (%s) sub %u (%s) payload_bytes %u ret_status %u multi %u reg_based %u; payload %08x %08x %08x %08x %08x %08x %08x %08x", page - 1u, header,
	       h.type, rdna4dr_dmub_type_name(h.type), h.sub_type, rdna4dr_dmub_subtype_name(h.type, h.sub_type), h.payload_bytes, h.ret_status, h.multi_cmd_pending, h.is_reg_based,
	       payload[0], payload[1], payload[2], payload[3], payload[4], payload[5], payload[6], payload[7]);
	return RDNA4DR_OK;
}

// build 0.0.624 (stage M1.5): verb 94 `region4read <offset> [dwords]`. READ-ONLY. Page 0 probes the DMCUB registers (dcn41_dmub_probe: register READS), computes the REGION4 window base live
// (rdna4dr_region4_base) and reads the dwords with rdna4_vram_read_mm; pages 1 and 2 return the stored rest with no hardware access at all. No register is written, no MM_INDEX path is written here.
uint32_t region4Read(uint64_t arg, uint64_t *out, unsigned outCount) {
	if (!out || outCount < 13u) return RDNA4DR_BAD_ARG;
	for (unsigned i = 0; i < 13u; i++) out[i] = 0u;
	if (!rdna4dr_r4_arg_ok(arg)) { out[0] = RDNA4DR_BAD_ARG; return RDNA4DR_BAD_ARG; }
	const uint32_t off = (uint32_t)(arg & 0xFFFFFFFFull), dwords = (uint32_t)((arg >> 32) & 0xFFu), page = (uint32_t)((arg >> 40) & 0xFFu);
	if (gLr.state != RDNA4LR_READY) { out[0] = RDNA4DR_NO_DEVICE; return RDNA4DR_NO_DEVICE; }
	if (__atomic_exchange_n(&gR4Busy, 1u, __ATOMIC_ACQ_REL) != 0u) { rdna4dr_r4_fill(out, RDNA4DR_BUSY, page, 0u, off, dwords, ~0ull, nullptr); return RDNA4DR_BUSY; }
	uint32_t status;
	if (page != 0u) {                                              // the stored dwords of the last page-0 read of THIS (offset, dwords): no hardware access
		if (!gR4.valid || gR4.off != off || gR4.dwords != dwords) {
			status = RDNA4DR_NO_DATA;
			rdna4dr_r4_fill(out, status, page, 0u, off, dwords, ~0ull, nullptr);
		} else {
			status = gR4.status;
			rdna4dr_r4_fill(out, status, page, gR4.seq, off, dwords, gR4.base, status == RDNA4DR_OK ? gR4.d : nullptr);
		}
	} else {
		struct dcn41_dmub_state st;
		if (dcn41_dmub_probe(&gLr.d, &st) != DCN41_OK) {
			status = RDNA4DR_NO_DEVICE;
			rdna4dr_r4_fill(out, status, 0u, 0u, off, dwords, ~0ull, nullptr);
		} else {
			amdgpu::DeviceContext *dev = static_cast<amdgpu::DeviceContext *>(gLr.d.cookie);
			const uint64_t vramSize = dev ? dev->vramSizeBytes : 0ull;
			const uint32_t enabled = (st.region4_top & DCN41_DMCUB_REGION4_TOP_ADDRESS__DMCUB_REGION4_ENABLE_MASK) != 0u ? 1u : 0u;
			R4Mem mem;
			uint64_t base = ~0ull;
			uint32_t d[RDNA4DR_R4_MAX_DWORDS] = { 0 };
			status = rdna4dr::region4_read(mem, st.region4_offset, st.region4_offset_high, enabled, st.fb_base_mc, vramSize, off, dwords, &base, d);
			gR4.valid = true; gR4.status = status; gR4.seq = ++gR4Seq; gR4.off = off; gR4.dwords = dwords; gR4.base = base;
			for (unsigned i = 0; i < RDNA4DR_R4_MAX_DWORDS; i++) gR4.d[i] = (status == RDNA4DR_OK && i < dwords) ? d[i] : 0u;
			rdna4dr_r4_fill(out, status, 0u, gR4.seq, off, dwords, status == RDNA4DR_REGION4_BAD ? (((uint64_t)st.region4_offset_high << 32) | st.region4_offset) : base, status == RDNA4DR_OK ? gR4.d : nullptr);
			if (status == RDNA4DR_REGION4_BAD) { out[2] = st.fb_base_mc; out[3] = vramSize; out[4] = enabled; }
			RDNA4LOG("region4read: +%#x x%u -> status %u (%s); REGION4 OFFSET %#010x HIGH %#010x enabled %u, FB base %#llx, window base (VRAM offset) %#llx, VRAM %#llx", off, dwords, status, rdna4dr_status_name(status),
			       st.region4_offset, st.region4_offset_high, enabled, (unsigned long long)st.fb_base_mc, (unsigned long long)base, (unsigned long long)vramSize);
			if (status == RDNA4DR_OK)
				for (uint32_t r = 0; r < dwords; r += 8u) {
					char row[96];
					unsigned len = 0;
					row[0] = 0;
					for (uint32_t k = 0; k < 8u && r + k < dwords && len + 10u < sizeof(row); k++) len += (unsigned)snprintf(row + len, sizeof(row) - len, " %08x", d[r + k]);
					RDNA4LOG("region4read: %05x:%s", off + r * 4u, row);
				}
		}
	}
	__atomic_store_n(&gR4Busy, 0u, __ATOMIC_RELEASE);
	return status;
}

uint32_t dispCensus(uint64_t arg, uint64_t *out, unsigned outCount) {
	if (!out || outCount < 13u) return RDNA4DR_BAD_ARG;
	for (unsigned i = 0; i < 13u; i++) out[i] = 0u;
	if (!rdna4dr_census_arg_ok(arg)) { out[0] = RDNA4DR_BAD_ARG; return RDNA4DR_BAD_ARG; }
	if (gLr.state != RDNA4LR_READY) { out[0] = RDNA4DR_NO_DEVICE; return RDNA4DR_NO_DEVICE; }
	RoKextEnv ro;
	const uint32_t st = rdna4dr::census_page(ro, (uint32_t)arg, out);
	if (st == RDNA4DR_OK) {
		const uint32_t n = rdna4dr_census_in_page((uint32_t)arg);
		for (uint32_t i = 0; i < n; i++) {
			const uint64_t w = out[1u + i / 2u];
			const struct rdna4dr_reg *r = rdna4dr_census_reg((uint32_t)arg, i);
			RDNA4LOG("dispcensus: %-38s abs %#07x = %#010x", r->name, dcn41_abs_rd(&gLr.d, r->off, r->base_idx), (uint32_t)((i & 1u) ? (w >> 32) : (w & 0xFFFFFFFFull)));   // 0.0.628: the absolute BAR5 dword too (0xffffffff = unresolved)
		}
	}
	return st;
}


// =====================================================================================================================================================================================================
// build 0.0.625 (stages M2 / M3; an internal design note "M1.5 result" section 4): verbs 95 `dmubsend`, 96 `dmubmode`, 97 `dmubctx` - the FIRST code that sends to the display firmware. All behind boot-arg
// garnet-dmubcmd=1 (OFF: RDNA4DR_CMD_OFF, nothing touched, not even a register read). Decisions: dcn/dmubcmd.h; sequences: dcn/dmubcmd_flow.h (host test src/garnet/tests/native_dmubcmd_test.cpp + native_dmubcmd_plant.sh).
// The writes: VRAM only through rdna4_vram_write_mm (the existing locked MM_INDEX writer); ONE register, DMCUB_INBOX1_WPTR, through dcn41_allow_write (needs bind(), like ddcread). One verb at a time (gDmBusy).
// =====================================================================================================================================================================================================

namespace {
volatile uint32_t gDmBusy = 0;
rdna4dm_mode_state gDmMode;           // the `dmubmode save` copy, this boot only
struct DmKextEnv {
	bool probe(rdna4dm::Probe *p) {
		struct dcn41_dmub_state st;
		if (gLr.state != RDNA4LR_READY || dcn41_dmub_probe(&gLr.d, &st) != DCN41_OK) return false;
		amdgpu::DeviceContext *dev = static_cast<amdgpu::DeviceContext *>(gLr.d.cookie);
		p->vramSize = dev ? dev->vramSizeBytes : 0ull;
		p->r4Enabled = (st.region4_top & DCN41_DMCUB_REGION4_TOP_ADDRESS__DMCUB_REGION4_ENABLE_MASK) != 0u ? 1u : 0u;   // the same expression region4Read uses
		p->r4Lo = st.region4_offset; p->r4Hi = st.region4_offset_high; p->fbBaseMc = st.fb_base_mc;
		p->pre.enabled = st.enabled; p->pre.soft_reset = st.soft_reset;
		p->pre.ring_region4 = st.ring_map == DCN41_DMUB_MAP_REGION4 ? 1u : 0u;
		p->pre.ring_at_base = (st.ring_addr != 0ull && st.ring_addr == (((uint64_t)st.region4_offset_high << 32) | st.region4_offset)) ? 1u : 0u;   // ring byte 0 IS the REGION4 window base
		p->pre.size = st.inbox1_size; p->pre.wptr = st.inbox1_wptr; p->pre.rptr = st.inbox1_rptr;
		return true;
	}
	bool rd(uint64_t off, uint32_t *dst, uint32_t n) { return rdna4_vram_read_mm(off, dst, n); }
	bool wr(uint64_t off, const uint32_t *src, uint32_t n) { return rdna4_vram_write_mm(off, src, n); }
	uint32_t reg(uint32_t off) {
		const uint32_t a = dcn41_abs(&gLr.d, off, RDNA4DM_REG_BASE_IDX);
		return a == DCN41_BAD_OFFSET ? 0xFFFFFFFFu : gLr.d.rreg(gLr.d.cookie, a);
	}
	uint32_t rptr() { return reg(RDNA4DM_REG_INBOX1_RPTR); }
	uint32_t wptr() { return reg(RDNA4DM_REG_INBOX1_WPTR); }
	uint32_t frames() { return reg(RDNA4DM_REG_OTG0_FRAMECOUNT); }
	bool wrWptr(uint32_t v) {      // THE register write of these verbs, through the DCN write allowlist
		if (!dcn_display_txn_ok("dmubsend register write")) return false;
		const uint32_t a = dcn41_abs(&gDcn.d, RDNA4DM_REG_INBOX1_WPTR, RDNA4DM_REG_BASE_IDX);
		if (a == DCN41_BAD_OFFSET || !dcn41_allow_write(&gDcn.allow, a, v, "dmubsend")) {
			RDNA4LOG("dmubsend: REFUSED write offset %#x = %#010x (abs %#010x) by the allowlist", RDNA4DM_REG_INBOX1_WPTR, v, a);
			return false;
		}
		amdgpu::WREG32(*gDcn.dev, a, v);
		return true;
	}
	void delay_us(uint32_t us) { dcn_udelay(nullptr, us); }
	uint64_t now_us() { return now_ns() / 1000ull; }
};
// the common entry: zero out[], the latch, a bound display layer, single flight. Returns 0 = proceed (busy flag held), else the status (out[0] already filled).
uint32_t dm_enter(uint64_t *out, unsigned outCount, bool needBound) {
	if (!out || outCount < 13u) return RDNA4DR_BAD_ARG;
	for (unsigned i = 0; i < 13u; i++) out[i] = 0u;
	if (!dm_latched_on()) { out[0] = RDNA4DR_CMD_OFF; return RDNA4DR_CMD_OFF; }
	if (gLr.state != RDNA4LR_READY || (needBound && (!__atomic_load_n(&gDcn.armed, __ATOMIC_ACQUIRE) || !gDcn.dev))) { out[0] = RDNA4DR_NO_DEVICE; return RDNA4DR_NO_DEVICE; }
	if (__atomic_exchange_n(&gDmBusy, 1u, __ATOMIC_ACQ_REL) != 0u) { out[0] = RDNA4DR_BUSY; return RDNA4DR_BUSY; }
	return 0u;
}
}  // namespace


// 0.0.626 (F4): the dispatcher asks this BEFORE bind(): with garnet-dmubcmd absent (the default) verb 95 must not even bind the display layer (bind() allocates a lock, reads registers and arms gDcn).
bool dmubCmdLatchedOn() { return dm_latched_on(); }

uint32_t dmubSend(uint64_t arg, uint64_t *out, unsigned outCount) {
	rdna4_display_txn txn(rdna4peer::kLeaseLocalDisplay);
	if (!txn.ok) {
		if (out && outCount > 0) { for (unsigned i = 0; i < outCount && i < 13u; i++) out[i] = 0u; out[0] = RDNA4DR_BUSY; }
		return RDNA4DR_BUSY;
	}
	const uint32_t early = dm_enter(out, outCount, true);
	if (early != 0u) return early;
	uint32_t h = 0, d1 = 0, d2 = 0, replaySrc = RDNA4DM_NO_REPLAY;
	rdna4dm_send_out o;
	DmKextEnv env;
	if (rdna4dm_is_replay_arg(arg)) {       // 0.0.626 (F8): `dmubsend replay <slot_off>`
		if (disp2Held() || disp2HeldInst(RDNA4D2_INST_MONA)) {                // 0.0.652 (M5): a HELD monitor B plane (0.0.655: or a HELD monitor A plane): a replay could resend a teardown slot sent earlier in the boot
			for (unsigned i = 0; i < sizeof(o); i++) ((uint8_t *)&o)[i] = 0;
			o.status = RDNA4DR_HELD_REFUSED; o.base = ~0ull;
		} else if (rdna4dm_replay_unarg(arg, &replaySrc) != 0u) {
			for (unsigned i = 0; i < sizeof(o); i++) ((uint8_t *)&o)[i] = 0;
			o.status = RDNA4DR_REPLAY_REFUSED; o.base = ~0ull;
		} else o = rdna4dm::send(env, 0u, 0u, 0u, replaySrc);
	} else if (rdna4dm_is_tpl_arg(arg)) {   // 0.0.630 (M4c): `dmubsend pclk|phyc ...` - a mainline VBIOS template NAMED BY ID (never raw dwords from userland)
		uint32_t tplId = RDNA4DM_TPL_NONE;
		if (rdna4dm_tpl_unarg(arg, &tplId) != 0u) {
			for (unsigned i = 0; i < sizeof(o); i++) ((uint8_t *)&o)[i] = 0;
			o.status = RDNA4DR_PAYLOAD_REFUSED; o.base = ~0ull;
		} else if (rdna4dm_tpl_held_refuses(tplId) && disp2Held()) {      // 0.0.652 (M5): 0.0.653: EVERY monitor B template (ids 8..15) is REFUSED while the plane is HELD
			for (unsigned i = 0; i < sizeof(o); i++) ((uint8_t *)&o)[i] = 0;
			o.status = RDNA4DR_HELD_REFUSED; o.base = ~0ull;
		} else if (rdna4dm_tpl_mona_held_refuses(tplId) && disp2HeldInst(RDNA4D2_INST_MONA)) {      // 0.0.655: EVERY monitor A template (ids 3..7: otg1 / phyc / digc - the teardowns pclk-otg1-off and phyc-disable above all) is REFUSED while the MONA's plane is HELD
			for (unsigned i = 0; i < sizeof(o); i++) ((uint8_t *)&o)[i] = 0;
			o.status = RDNA4DR_HELD_REFUSED; o.base = ~0ull;
		} else o = rdna4dm::send(env, 0u, 0u, 0u, RDNA4DM_NO_REPLAY, tplId);
		if (rdna4dm_tpl_get(tplId)) RDNA4LOG("dmubsend template %s: slot %08x %08x %08x %08x %08x (11 zero dwords)", rdna4dm_tpl_get(tplId)->name, rdna4dm_tpl_get(tplId)->dw[0], rdna4dm_tpl_get(tplId)->dw[1], rdna4dm_tpl_get(tplId)->dw[2], rdna4dm_tpl_get(tplId)->dw[3], rdna4dm_tpl_get(tplId)->dw[4]);
	} else {
		rdna4dm_send_unarg(arg, &h, &d1, &d2);
		if (rdna4dm_slot_held_refuses(h, d1, d2) && disp2Held()) {        // 0.0.653: a raw slot aimed at the monitor B's context 0x7800 (detect / setmode / enable / disable): REFUSED while the plane is HELD
			for (unsigned i = 0; i < sizeof(o); i++) ((uint8_t *)&o)[i] = 0;
			o.status = RDNA4DR_HELD_REFUSED; o.base = ~0ull;
		} else if (rdna4dm_slot_mona_held_refuses(h, d1, d2) && disp2HeldInst(RDNA4D2_INST_MONA)) {        // 0.0.655: ... and one aimed at the monitor A's context 0x7000 (DFP3, HDMI SINK-B) while the MONA's plane is HELD
			for (unsigned i = 0; i < sizeof(o); i++) ((uint8_t *)&o)[i] = 0;
			o.status = RDNA4DR_HELD_REFUSED; o.base = ~0ull;
		} else o = rdna4dm::send(env, h, d1, d2);
	}
	rdna4dm_send_pack(out, &o);
	RDNA4LOG("dmubsend%s: header %#010x d1 %#x d2 %#x -> status %u (%s); sent %u; WPTR %#x -> %#x, RPTR %#x -> %#x after %u polls (%u us); OTG0 frames %u -> %u; VBIOS vars %u; window base %#llx; replay source %#x",
	       replaySrc != RDNA4DM_NO_REPLAY ? " replay" : "", o.hdr, o.d1, o.d2, o.status, rdna4dr_status_name(o.status), o.sent, o.old_wptr, o.new_wptr, o.rptr_start, o.rptr_end, o.polls, o.elapsed_us, o.frames0, o.frames1, o.vars, (unsigned long long)o.base, replaySrc);
	__atomic_store_n(&gDmBusy, 0u, __ATOMIC_RELEASE);
	return o.status;
}

uint32_t dmubMode(uint64_t arg, uint64_t *out, unsigned outCount) {
	rdna4_display_txn txn(rdna4peer::kLeaseLocalDisplay);
	if (!txn.ok) {
		if (out && outCount > 0) { for (unsigned i = 0; i < outCount && i < 13u; i++) out[i] = 0u; out[0] = RDNA4DR_BUSY; }
		return RDNA4DR_BUSY;
	}
	const uint32_t early = dm_enter(out, outCount, false);
	if (early != 0u) return early;
	DmKextEnv env;
	const rdna4dm::ModeResult r = rdna4dm::mode(env, arg, gDmMode);
	out[0] = (uint64_t)(r.status & 0xFFu) | ((arg & 0xFFFu) << 8) | ((uint64_t)(r.saved & 1u) << 24) | ((uint64_t)(r.dirty & 1u) << 25);
	out[1] = r.base;
	rdna4dm_dwords_pack(out, 2u, r.block, RDNA4DM_MODE_DWORDS);
	rdna4dm_dwords_pack(out, 6u, r.before, RDNA4DM_MODE_DWORDS);
	RDNA4LOG("dmubmode: op %#llx -> status %u (%s); saved %u dirty %u; window base %#llx; block %08x %08x %08x %08x %08x %08x %08x %08x", (unsigned long long)arg, r.status, rdna4dr_status_name(r.status), r.saved, r.dirty,
	       (unsigned long long)r.base, r.block[0], r.block[1], r.block[2], r.block[3], r.block[4], r.block[5], r.block[6], r.block[7]);
	__atomic_store_n(&gDmBusy, 0u, __ATOMIC_RELEASE);
	return r.status;
}

uint32_t dmubCtx(uint64_t arg, uint64_t *out, unsigned outCount) {
	rdna4_display_txn txn(rdna4peer::kLeaseLocalDisplay);
	if (!txn.ok) {
		if (out && outCount > 0) { for (unsigned i = 0; i < outCount && i < 13u; i++) out[i] = 0u; out[0] = RDNA4DR_BUSY; }
		return RDNA4DR_BUSY;
	}
	const uint32_t early = dm_enter(out, outCount, false);
	if (early != 0u) return early;
	DmKextEnv env;
	const rdna4dm::CtxResult r = rdna4dm::ctx(env, arg);
	out[0] = r.status & 0xFFu;
	out[1] = r.base;
	out[2] = (uint64_t)r.ctx | ((uint64_t)r.value << 32);
	out[3] = (uint64_t)r.before | ((uint64_t)r.after << 32);
	RDNA4LOG("dmubctx: ctx %#x status dword (+0x1a0) <- %#010x -> status %u (%s); before %#010x, read back %#010x; window base %#llx", r.ctx, r.value, r.status, rdna4dr_status_name(r.status), r.before, r.after, (unsigned long long)r.base);
	__atomic_store_n(&gDmBusy, 0u, __ATOMIC_RELEASE);
	return r.status;
}

}  // namespace rdna4dcn
