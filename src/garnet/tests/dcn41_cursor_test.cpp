// dcn41_cursor_test.cpp — Host tests for DCN 4.01 128x128 hardware cursor helpers.
#include <cstdint>
#include <cstdio>
#include <vector>

#include "dcn41_allow.h"
#include "dcn/dcn41_cursor_pure.h"

namespace {

constexpr uint32_t kSeg2Base = 0x000034c0u;

int gChecks = 0;
int gFails = 0;

void check(bool cond, const char *msg) {
	++gChecks;
	if (!cond) {
		++gFails;
		std::printf("FAIL: %s\n", msg);
	}
}

struct RegWrite {
	uint32_t offset;
	uint32_t value;
};

struct MockCursorHw {
	uint32_t regs[0x4000] {};
	std::vector<RegWrite> writes;
	std::vector<uint32_t> reads;
	uint32_t writeAttempts { 0u };
	uint32_t failOnWriteNum { 0u }; // 0 = never fail

	static uint32_t rd(void *cookie, uint32_t seg2Off) {
		auto *self = static_cast<MockCursorHw *>(cookie);
		const uint32_t absDw = kSeg2Base + seg2Off;
		check(!dcn41_is_read_hazard(absDw), "cursor register read must not hit read hazard");
		self->reads.push_back(seg2Off);
		return (seg2Off < 0x4000u) ? self->regs[seg2Off] : 0xffffffffu;
	}

	static bool wr(void *cookie, uint32_t seg2Off, uint32_t value) {
		auto *self = static_cast<MockCursorHw *>(cookie);
		const uint32_t absDw = kSeg2Base + seg2Off;
		check(dcn41_allow_classify(absDw, 0x20000u, nullptr, nullptr) == DCN41_ALLOW_OK,
		      "cursor register write must be inside dcn41 allowlist");
		check(!dcn41_is_read_hazard(absDw),
		      "cursor register write must not hit Navi 44 hazard register");
		++self->writeAttempts;
		if (self->failOnWriteNum != 0u && self->writeAttempts == self->failOnWriteNum) {
			return false;
		}
		self->writes.push_back(RegWrite { seg2Off, value });
		if (seg2Off < 0x4000u) self->regs[seg2Off] = value;
		return true;
	}

	rdna4cursor::CursorHw hw() {
		return rdna4cursor::CursorHw { this, &MockCursorHw::rd, &MockCursorHw::wr };
	}

	void clearLog() {
		writes.clear();
		reads.clear();
		writeAttempts = 0u;
		failOnWriteNum = 0u;
	}
};

void test_allowlist_and_program_all_pipes() {
	for (uint32_t pipe = 0u; pipe < rdna4cursor::kCursorMaxPipes; ++pipe) {
		MockCursorHw mock;
		const uint32_t primLo = rdna4cursor::hubp_reg(pipe, rdna4cursor::kSeg2HubpPrimaryAddr);
		const uint32_t primHi = rdna4cursor::hubp_reg(pipe, rdna4cursor::kSeg2HubpPrimaryAddrHigh);
		check(!dcn41_is_read_hazard(kSeg2Base + primLo), "primary addr low readable");
		check(!dcn41_is_read_hazard(kSeg2Base + primHi), "primary addr high readable");

		const uint32_t mallReg = rdna4cursor::hubp_reg(pipe, rdna4cursor::kSeg2HubpMallConfig);
		mock.regs[mallReg] = 0xffu;

		rdna4cursor::CursorPlaneConfig cfg {};
		cfg.pipe = pipe;
		cfg.mcAddr = 0x0080017ea000ull;
		cfg.width = 64u;
		cfg.height = 128u;
		cfg.x = 200;
		cfg.y = 100;
		cfg.hotX = 8u;
		cfg.hotY = 12u;
		cfg.pixClkKhz = 148500u;
		cfg.enabled = true;

		check(rdna4cursor::program_cursor_plane(mock.hw(), cfg), "program_cursor_plane succeeds");
		check(!mock.writes.empty(), "program emits writes");
		check(mock.writes.front().offset == rdna4cursor::otg_reg(pipe, rdna4cursor::kSeg2OtgMasterUpdateLock) &&
		          mock.writes.front().value == 1u,
		      "first write locks OTG master update lock");
		check(mock.writes.back().offset == rdna4cursor::otg_reg(pipe, rdna4cursor::kSeg2OtgMasterUpdateLock) &&
		          mock.writes.back().value == 0u,
		      "last write unlocks OTG master update lock");

		// USE_MALL_FOR_CURSOR (bit 2) cleared, other bits preserved.
		check(mock.regs[mallReg] == 0xfbu, "USE_MALL_FOR_CURSOR=0 cleared via RMW");

		// CURSOR_SETTINGS CHUNK_HDL_ADJUST=3
		check(mock.regs[rdna4cursor::hubp_reg(pipe, rdna4cursor::kSeg2HubpCursorSettings)] == (3u << 8),
		      "CHUNK_HDL_ADJUST=3");

		// CURSOR_CONTROL: EN=1, REQ_MODE=1(bit2), MODE=2(bits9:8), PITCH=1(128px bits17:16), LINES=2(4 lines bits25:24)
		const uint32_t wantCtrl = 1u | (1u << 2) | (2u << 8) | (1u << 16) | (2u << 24);
		check(mock.regs[rdna4cursor::hubp_reg(pipe, rdna4cursor::kSeg2HubpCursorControl)] == wantCtrl,
		      "CURSOR_CONTROL fields match EN=1 REQ_MODE=1 ARGB PITCH=128 LINES=4");

		// Address split
		check(mock.regs[rdna4cursor::hubp_reg(pipe, rdna4cursor::kSeg2HubpCursorAddr)] == 0x017ea000u,
		      "cursor addr low");
		check(mock.regs[rdna4cursor::hubp_reg(pipe, rdna4cursor::kSeg2HubpCursorAddrHigh)] == 0x80u,
		      "cursor addr high");

		// Size (64x128 -> h-1=127, w-1=63)
		check(mock.regs[rdna4cursor::hubp_reg(pipe, rdna4cursor::kSeg2HubpCursorSize)] ==
		          ((128u - 1u) | ((64u - 1u) << 16)),
		      "cursor size encodes 64x128");

		// DPP scale/matrix/control
		check(mock.regs[rdna4cursor::dpp_reg(pipe, rdna4cursor::kSeg2DppCursorCmScaleGY)] == 0x3c00u,
		      "FP16 1.0 scale GY");
		check(mock.regs[rdna4cursor::dpp_reg(pipe, rdna4cursor::kSeg2DppCursorCmScaleRB)] == 0x3c00u,
		      "FP16 1.0 scale RB");
		check(mock.regs[rdna4cursor::dpp_reg(pipe, rdna4cursor::kSeg2DppCursorCmMatrix)] == 0u,
		      "CM matrix 0");
		const uint32_t wantCmCtrl = 1u | (1u << 2) | (2u << 4) | (1u << 7);
		check(mock.regs[rdna4cursor::dpp_reg(pipe, rdna4cursor::kSeg2DppCursorCmControl)] == wantCmCtrl,
		      "CM_CUR0_CONTROL fields match");

		// Also verify disable path on same pipe passes allowlist.
		mock.clearLog();
		check(rdna4cursor::disable_cursor_plane(mock.hw(), pipe), "disable_cursor_plane succeeds");
		check(mock.writes.size() == 4u, "disable locks, writes 2 controls, unlocks");
		check(mock.regs[rdna4cursor::hubp_reg(pipe, rdna4cursor::kSeg2HubpCursorControl)] ==
		          rdna4cursor::cursor_control_word(false),
		      "disable clears CURSOR_ENABLE");
		check(mock.regs[rdna4cursor::dpp_reg(pipe, rdna4cursor::kSeg2DppCursorCmControl)] ==
		          rdna4cursor::cursor_cm_control_word(false),
		      "disable clears CUR0_EN");
	}
}

void test_position_and_hotspot_encoding() {
	rdna4cursor::EncodedPos pos {};
	// Positive coords: (1000, 500), hot (10, 20) -> tl=(990, 480), hx=0, hy=0
	rdna4cursor::encode_cursor_position(1000, 500, 64u, 64u, 10u, 20u, 148500u, &pos);
	check(pos.px == 990u && pos.py == 480u, "positive coords top-left");
	check(pos.hx == 0u && pos.hy == 0u, "positive coords zero hw hotspot");
	check(pos.posReg == (480u | (990u << 15)), "bit-15 X position encoding");
	check(pos.hotReg == 0u, "hotspot reg 0");
	check(pos.dstOffset == static_cast<uint32_t>((990ull * 50000ull) / 148500ull),
	      "dstXOffset formula");

	// Negative top-left: x=5, hotX=15 -> tlX=-10 -> px=0, hx=10
	// Large negative: y=-50, hotY=20 -> tlY=-70 on 32x48 -> py=0, hy=47 (curH - 1)
	rdna4cursor::encode_cursor_position(5, -50, 32u, 48u, 15u, 20u, 0u, &pos);
	check(pos.px == 0u && pos.py == 0u, "negative top-left clamps px/py to 0");
	check(pos.hx == 10u && pos.hy == 47u, "negative top-left clamps hw hotspot into [0, dim-1]");
	check(pos.hotReg == (47u | (10u << 16)), "hotspot reg bit layout");
	check(pos.dstOffset == 0u, "zero pixel clock produces dstOffset=0");
}

void test_move_visibility_only_controls() {
	MockCursorHw mock;
	rdna4cursor::CursorPlaneConfig cfg {};
	cfg.pipe = 0u;
	cfg.mcAddr = 0x8001000000ull;
	cfg.width = 64u;
	cfg.height = 64u;
	cfg.x = 300;
	cfg.y = 400;
	cfg.hotX = 4u;
	cfg.hotY = 4u;
	cfg.pixClkKhz = 594000u;
	cfg.enabled = true;

	// Move without visibility change: lock, Pos, Hot, DstOff, unlock -> 5 writes, NO CursorControl/CmControl.
	check(rdna4cursor::move_cursor_plane(mock.hw(), cfg, false), "move without visibility change");
	check(mock.writes.size() == 5u, "5 writes on pure move");
	bool wroteCtrl = false;
	for (const auto &w : mock.writes) {
		if (w.offset == rdna4cursor::kSeg2HubpCursorControl ||
		    w.offset == rdna4cursor::kSeg2DppCursorCmControl) {
			wroteCtrl = true;
		}
	}
	check(!wroteCtrl, "pure move never writes control registers");

	// Move WITH visibility change: lock, Pos, Hot, DstOff, CursorCtrl, CmCtrl, unlock -> 7 writes.
	mock.clearLog();
	cfg.enabled = false;
	check(rdna4cursor::move_cursor_plane(mock.hw(), cfg, true), "move with visibility change");
	check(mock.writes.size() == 7u, "7 writes on visibility toggle");
	check(mock.regs[rdna4cursor::kSeg2HubpCursorControl] == rdna4cursor::cursor_control_word(false),
	      "visibility off clears CURSOR_ENABLE");
	check(mock.regs[rdna4cursor::kSeg2DppCursorCmControl] == rdna4cursor::cursor_cm_control_word(false),
	      "visibility off clears CUR0_EN");

	// Fail on intermediate write: lock must still be released.
	mock.clearLog();
	mock.failOnWriteNum = 3u;
	check(!rdna4cursor::move_cursor_plane(mock.hw(), cfg, false), "fails on injected write error");
	check(mock.writes.back().offset == rdna4cursor::kSeg2OtgMasterUpdateLock &&
	          mock.writes.back().value == 0u,
	      "OTG lock released after write failure");
}

void test_pack_64x128_hidpi_and_premult() {
	// Source 64x128 tight rows: fill pixel (x=63, y=127) with half-alpha cyan, (0,0) transparent, (1,0) opaque.
	std::vector<uint32_t> src(64u * 128u, 0x00123456u);
	src[0] = 0x00ff0000u;              // alpha=0 -> premult=0
	src[1] = 0xff112233u;              // alpha=255 -> unchanged
	src[127u * 64u + 63u] = 0x80ff8000u; // a=128, r=255->128, g=128->64, b=0

	std::vector<uint32_t> dst(rdna4cursor::kCursorPixels, 0xdeadbeefu);
	rdna4cursor::pack_cursor_128x128(src.data(), 64u, 128u, dst.data());

	check(dst[0] == 0u, "alpha 0 premultiplies to 0");
	check(dst[1] == 0xff112233u, "alpha 255 preserved");
	const uint32_t corner = dst[127u * 128u + 63u];
	const uint32_t wantR = (255u * 128u + 127u) / 255u;
	const uint32_t wantG = (128u * 128u + 127u) / 255u;
	check(corner == ((0x80u << 24) | (wantR << 16) | (wantG << 8)), "64x128 premultiply + 128 stride");
	check(dst[127u * 128u + 64u] == 0u && dst[64] == 0u, "padding columns beyond width=64 zeroed");

	// Oversized input (>128) clamps safely without stride corruption.
	std::vector<uint32_t> big(200u * 150u, 0xffffffffu);
	big[127u * 200u + 127u] = 0xff010203u;
	rdna4cursor::pack_cursor_128x128(big.data(), 200u, 150u, dst.data());
	check(dst[127u * 128u + 127u] == 0xff010203u, "oversized source clamps cleanly");
}

void test_plan_cursor_slot() {
	rdna4cursor::CursorSlotPlan plan {};
	// Console at +0x10000, size 1920*1080*4 = 0x7e9000 -> end = 0x7f9000 -> 64K align = 0x800000.
	check(rdna4cursor::plan_cursor_slot(0x1000000000ull, 0x20000000ull,
	                                    0x1000010000ull, 0x7e9000ull,
	                                    0x8000010000ull, &plan),
	      "valid slot plan");
	check(plan.slotOffset == 0x800000ull, "64K aligned after console");
	check(plan.physAddr == 0x1000800000ull, "physAddr matches");
	check(plan.mcAddr == 0x8000800000ull, "mcAddr matches");
	check(plan.sizeBytes == rdna4cursor::kCursorSlotBytes, "size is 64 KiB");

	// Refusal cases
	check(!rdna4cursor::plan_cursor_slot(0x1000000000ull, 0x20000000ull,
	                                     0x1000010000ull, 0x7e9000ull,
	                                     0x8000010000ull, nullptr),
	      "null out refused");
	check(!rdna4cursor::plan_cursor_slot(0x1000000000ull, 0x7ff000ull,
	                                     0x1000000000ull, 0x7e9000ull,
	                                     0x8000000000ull, &plan),
	      "insufficient BAR0 tail for 64K slot refused");
	check(!rdna4cursor::plan_cursor_slot(0x1000000000ull, 0x20000000ull,
	                                     0x0fffffffffull, 0x10000ull,
	                                     0x8000000000ull, &plan),
	      "console below BAR0 refused");
	check(!rdna4cursor::plan_cursor_slot(0x1000000000ull, 0x20000000ull,
	                                     0x1000100000ull, 0x7e9000ull,
	                                     0x0000050000ull, &plan),
	      "hubpPrimaryMc < consoleOff refused");
	check(!rdna4cursor::plan_cursor_slot(0x1000000000ull, 0x800000ull,
	                                     0x1000000000ull, 0x7f1000ull,
	                                     0x8000000000ull, &plan),
	      "align pad overflow BAR0 refused");
}

} // namespace

int main() {
	test_allowlist_and_program_all_pipes();
	test_position_and_hotspot_encoding();
	test_move_visibility_only_controls();
	test_pack_64x128_hidpi_and_premult();
	test_plan_cursor_slot();
	std::printf("dcn41_cursor_test: %d checks, %d failed\n", gChecks, gFails);
	return gFails ? 1 : 0;
}
