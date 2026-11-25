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

	std::printf("garnet_power_policy_test: %d checks, %d failed\n", gChecks, gFail);
	return gFail ? 1 : 0;
}
