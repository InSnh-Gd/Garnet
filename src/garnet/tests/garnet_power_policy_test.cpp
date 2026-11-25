#include "amd/garnet_power_policy_pure.h"

#include <cstdio>

namespace {
int gChecks = 0;
int gFail = 0;
void expect(bool cond, const char *msg) {
	++gChecks;
	if (!cond) {
		++gFail;
		std::printf("FAIL: %s\n", msg);
	}
}
}  // namespace

int main() {
	using namespace rdna4pwr;

	// Boot-arg latches
	expect(gfx_cg_enabled(false, 0u), "garnet-gfx-cg absent defaults ON");
	expect(gfx_cg_enabled(true, 1u), "garnet-gfx-cg=1 enables CG");
	expect(!gfx_cg_enabled(true, 0u), "garnet-gfx-cg=0 disables CG");

	expect(det0_expand_enabled(false, 0u), "garnet-det0 absent defaults ON");
	expect(!det0_expand_enabled(true, 0u), "garnet-det0=0 disables DET0 expansion");

	expect(!uclk_force_pin(false, 0u), "garnet-uclk-pin absent allows auto DET0-guarded unpin");
	expect(!uclk_force_pin(true, 0u), "garnet-uclk-pin=0 allows auto DET0-guarded unpin");
	expect(uclk_force_pin(true, 1u), "garnet-uclk-pin=1 forces UCLK pinned");

	expect(!gfxoff_opt_in(false, 0u), "garnet-gfxoff absent defaults OFF");
	expect(!gfxoff_opt_in(true, 0u), "garnet-gfxoff=0 stays OFF");
	expect(gfxoff_opt_in(true, 1u), "garnet-gfxoff=1 opts into GFXOFF");

	expect(!idle_clamp_enabled(false, 0u), "garnet-idle-clamp absent defaults OFF (SMUPowerState::Auto)");
	expect(!idle_clamp_enabled(false, 1u), "garnet-idle-clamp absent ignores uninitialized value");
	expect(!idle_clamp_enabled(true, 0u), "garnet-idle-clamp=0 stays OFF (Auto)");
	expect(idle_clamp_enabled(true, 1u), "garnet-idle-clamp=1 clamps GFXCLK to 200 MHz Low");
	expect(idle_clamp_enabled(true, 2u), "garnet-idle-clamp nonzero opts into Low clamp");

	// Exact live RX 9060 XT defaults -> upstream GFX12 gated values
	expect(gfx12_cgtt_mgcg_override(0x000007FFu, true) == 0x00000040u,
	       "RLC_CGTT_MGCG_OVERRIDE 0x7FF -> 0x40 (all upstream GFX12 overrides cleared, MGLS untouched)");
	expect(gfx12_cgtt_mgcg_override(0x00000040u, false) == 0x00000767u,
	       "RLC_CGTT_MGCG_OVERRIDE disable restores MGCG/FGCG/PerfClk overrides");
	expect(gfx12_cgcg_cgls_ctrl(0x0001003Cu, true) == 0x0000363Fu,
	       "RLC_CGCG_CGLS_CTRL 0x1003C -> 0x363F (CGCG+CGLS enabled, threshold 0x36, delay 0x0F)");
	expect(gfx12_cgcg_cgls_ctrl(0x9000363Fu, true) == 0x9000363Fu,
	       "RLC_CGCG_CGLS_CTRL preserves upper control bits [31:27]");
	expect(gfx12_cgcg_cgls_ctrl(0x9000363Fu, false) == 0x9000363Cu,
	       "RLC_CGCG_CGLS_CTRL disable clears CGCG_EN & CGLS_EN only");
	expect(gfx12_cgcg_cgls_ctrl(0x0000003Cu, true) == 0x0000363Fu,
	       "RLC_CGCG_CGLS_CTRL_3D 0x3C -> 0x363F");
	expect(gfx12_cp_rb_wptr_poll_cntl(0u) == 0x00900100u,
	       "CP_RB_WPTR_POLL_CNTL sets IDLE_POLL_COUNT=0x90 POLL_FREQUENCY=0x100");
	expect(gfx12_cp_int_cntl_idle(0x04000000u, true) == 0x043C0000u,
	       "CP_INT_CNTL sets bits 18..21 while preserving TIME_STAMP_INT_ENABLE");
	expect(gfx12_cp_int_cntl_idle(0x043C0000u, false) == 0x04000000u,
	       "CP_INT_CNTL disable clears only bits 18..21");
	expect(gfx12_sdma_rlc_cgcg_ctrl(0x00010000u, true) == 0x00010002u,
	       "SDMA*_RLC_CGCG_CTRL sets CGCG_INT_ENABLE bit 1");
	expect(gfx12_sdma_rlc_cgcg_ctrl(0x00010002u, false) == 0x00010000u,
	       "SDMA*_RLC_CGCG_CTRL disable clears bit 1");

	// Mode-aware UCLK unpin timing margin policy (prevents OPTC0 underflow on 4K@160Hz DSC).
	const uint32_t det4k60 = det0_buffer_us(kDet0ExpandedBytes, 3840u, 4000u, 533250000ULL);
	const uint32_t vb4k60  = vblank_us(2160u, 2222u, 4000u, 533250000ULL);
	expect(det4k60 >= 280u && det4k60 <= 295u, "4K@60Hz 576 KiB DET0 holds ~288 us");
	expect(vb4k60 >= 460u && vb4k60 <= 470u, "4K@60Hz VBLANK is ~465 us");
	expect(uclk_unpin_safe_for_mode(false, true, false, det4k60, vb4k60),
	       "4K@60Hz non-DSC with 576 KiB DET0 is safe to unpin UCLK");
	expect(!uclk_unpin_safe_for_mode(true, true, false, det4k60, vb4k60),
	       "garnet-uclk-pin=1 blocks UCLK unpin even on 4K@60Hz");
	expect(!uclk_unpin_safe_for_mode(false, false, false, det4k60, vb4k60),
	       "unexpanded DET0 blocks UCLK unpin");

	const uint32_t det4k160 = det0_buffer_us(kDet0ExpandedBytes, 3840u, 4000u, 1479000000ULL);
	const uint32_t vb4k160  = vblank_us(2160u, 2200u, 4000u, 1479000000ULL);
	expect(det4k160 >= 100u && det4k160 <= 110u, "4K@160Hz DSC 576 KiB DET0 holds only ~103 us");
	expect(vb4k160 >= 105u && vb4k160 <= 112u, "4K@160Hz DSC VBLANK is only ~108 us");
	expect(!uclk_unpin_safe_for_mode(false, true, true, det4k160, vb4k160),
	       "4K@160Hz DSC stays pinned at top UCLK to prevent top-1/10th OPTC0 underflow");
	expect(!uclk_unpin_safe_for_mode(false, true, true, det4k60, vb4k60),
	       "DSC always forces UCLK pinned even with wide timing margins");

	const uint32_t det4k120 = det0_buffer_us(kDet0ExpandedBytes, 3840u, 4000u, 1066500000ULL);
	const uint32_t vb4k120  = vblank_us(2160u, 2222u, 4000u, 1066500000ULL);
	expect(!uclk_unpin_safe_for_mode(false, true, false, det4k120, vb4k120),
	       "4K@120Hz stays pinned (DET0 ~144 us, VBLANK ~232 us)");

	const uint32_t det1440p120 = det0_buffer_us(kDet0ExpandedBytes, 2560u, 2720u, 497750000ULL);
	const uint32_t vb1440p120  = vblank_us(1440u, 1525u, 2720u, 497750000ULL);
	expect(uclk_unpin_safe_for_mode(false, true, false, det1440p120, vb1440p120),
	       "1440p@120Hz non-DSC has sufficient DET0 (~314 us) and VBLANK (~464 us)");

	const uint32_t det1440p144 = det0_buffer_us(kDet0ExpandedBytes, 2560u, 2720u, 580000000ULL);
	const uint32_t vb1440p144  = vblank_us(1440u, 1481u, 2720u, 580000000ULL);
	expect(!uclk_unpin_safe_for_mode(false, true, false, det1440p144, vb1440p144),
	       "1440p@144Hz short VBLANK (~192 us) keeps UCLK pinned");

	// Degenerate / overflow bounds fail closed (return 0 -> keep pinned).
	expect(det0_buffer_us(0u, 3840u, 4000u, 533250000ULL) == 0u, "zero DET fails closed");
	expect(det0_buffer_us(kDet0ExpandedBytes, 3840u, 3000u, 533250000ULL) == 0u, "hTotal < hActive fails closed");
	expect(det0_buffer_us(kDet0ExpandedBytes, 3840u, 4000u, 0ULL) == 0u, "zero pixel clock fails closed");
	expect(vblank_us(2160u, 2160u, 4000u, 533250000ULL) == 0u, "zero vblank fails closed");
	expect(vblank_us(2160u, 2222u, 4000u, 0ULL) == 0u, "zero pixel clock vblank fails closed");

	std::printf("garnet_power_policy_test: %d checks, %d failed\n", gChecks, gFail);
	return gFail ? 1 : 0;
}
