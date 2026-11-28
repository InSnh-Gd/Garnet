#include "amd/garnet_power_policy_pure.h"

#include <cstdio>
#include <initializer_list>

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
	expect(fams2_enabled(false, 0u), "garnet-fams2 absent allows a verified DAL path");
	expect(!fams2_enabled(true, 0u), "garnet-fams2=0 disallows FAMS2");

	expect(idle_dpm_enabled(false, 0u), "garnet-idle-dpm absent defaults ON");
	expect(idle_dpm_enabled(true, 1u), "garnet-idle-dpm=1 enables idle governor");
	expect(!idle_dpm_enabled(true, 0u), "garnet-idle-dpm=0 disables idle governor");

	// Activity-driven SMU GFXCLK idle governor state machine.
	{
		constexpr uint64_t kMs = 1000ULL * 1000ULL;
		IdleDpmGovernor gov {};
		gov.init(/*dpmEnabled=*/true, /*clampEnabled=*/false, /*nowNs=*/1000ULL * kMs);
		expect(gov.active() && !gov.is_low(), "governor starts active in SMUPowerState::Auto");

		// Not idle long enough yet (999 ms < 1000 ms).
		expect(gov.on_tick(1999ULL * kMs, false) == IdleDpmAction::None,
		       "999 ms idle stays in Auto");

		// In-flight submission holds off Auto -> Low even well past 1000 ms.
		expect(gov.on_submit_begin(1999ULL * kMs) == IdleDpmAction::None,
		       "submit_begin while Auto emits zero SMU traffic (deduplicated)");
		expect(gov.on_tick(5000ULL * kMs, false) == IdleDpmAction::None,
		       "in-flight submission holds off idle drop past 1000 ms");
		gov.on_submit_end(5000ULL * kMs);
		expect(gov.on_tick(5999ULL * kMs, false) == IdleDpmAction::None,
		       "999 ms after submit_end stays in Auto");

		// Unretired ring work (hwBusy) also holds off idle drop.
		expect(gov.on_tick(7500ULL * kMs, true) == IdleDpmAction::None,
		       "hwBusy refreshes idle clock and holds off Low");
		expect(gov.on_tick(8499ULL * kMs, false) == IdleDpmAction::None,
		       "999 ms after hwBusy clears stays in Auto");

		// 1000 ms after last activity with zero in-flight work -> Auto -> Low.
		const IdleDpmAction dropAct = gov.on_tick(8500ULL * kMs, false);
		expect(dropAct == IdleDpmAction::SetLow, "1000 ms idle triggers Auto -> Low");
		// Failed SMU transition does not lie about state (allows clean retry).
		gov.commit(dropAct, false);
		expect(!gov.is_low() && gov.idleDropCount == 0u, "failed SetLow leaves state Auto for retry");
		expect(gov.on_tick(8750ULL * kMs, false) == IdleDpmAction::SetLow, "retry emits SetLow");
		gov.commit(IdleDpmAction::SetLow, true);
		expect(gov.is_low() && gov.idleDropCount == 1u, "committed SetLow enters Low state");

		// Repeated idle ticks while Low emit zero SMU traffic.
		expect(gov.on_tick(9500ULL * kMs, false) == IdleDpmAction::None,
		       "already Low emits no duplicate SetLow");
		expect(gov.on_tick(20000ULL * kMs, false) == IdleDpmAction::None,
		       "long idle stays silent in Low");

		// GPU submission while Low triggers Low -> Auto boost once (deduplicated).
		const IdleDpmAction boostAct = gov.on_activity(21000ULL * kMs);
		expect(boostAct == IdleDpmAction::SetAuto, "activity while Low requests Low -> Auto");
		gov.commit(boostAct, true);
		expect(!gov.is_low() && gov.boostCount == 1u, "committed SetAuto restores Auto");
		expect(gov.on_activity(21001ULL * kMs) == IdleDpmAction::None,
		       "subsequent frames while Auto emit zero SMU spam");
		expect(gov.on_activity(21002ULL * kMs) == IdleDpmAction::None,
		       "continuous submissions emit zero SMU spam");

		// Drop back to Low after another 1000 ms quiet window, then test submit_begin boost.
		expect(gov.on_tick(22002ULL * kMs, false) == IdleDpmAction::SetLow,
		       "second 1000 ms idle window triggers SetLow");
		gov.commit(IdleDpmAction::SetLow, true);
		expect(gov.on_submit_begin(23000ULL * kMs) == IdleDpmAction::SetAuto,
		       "submit_begin while Low requests SetAuto");
		gov.commit(IdleDpmAction::SetAuto, true);
		gov.on_submit_end(23050ULL * kMs);
		expect(gov.inFlight == 0u && gov.boostCount == 2u && gov.idleDropCount == 2u,
		       "paired submit_begin/end tracks counts accurately");

		// Manual `garnettest power` override locks out governor permanently for the boot.
		gov.on_manual_override(IdleDpmGovernor::kStateAuto);
		expect(!gov.active(), "manual garnettest power locks out automatic governor");
		expect(gov.on_tick(99000ULL * kMs, false) == IdleDpmAction::None,
		       "manual override blocks Auto -> Low");
		gov.on_manual_override(IdleDpmGovernor::kStateLow);
		expect(gov.on_activity(100000ULL * kMs) == IdleDpmAction::None,
		       "manual Low override blocks Low -> Auto");
		expect(gov.on_tick(100100ULL * kMs, true) == IdleDpmAction::None,
		       "manual Low override stays locked even when hwBusy");

		// Kill-switch garnet-idle-dpm=0 disables governor completely.
		IdleDpmGovernor offGov {};
		offGov.init(/*dpmEnabled=*/false, /*clampEnabled=*/false, 0ULL);
		expect(!offGov.active() && !offGov.is_low(), "garnet-idle-dpm=0 stays inactive in Auto");
		expect(offGov.on_tick(10000ULL * kMs, false) == IdleDpmAction::None,
		       "garnet-idle-dpm=0 never drops to Low");

		// garnet-idle-clamp=1 bypasses governor completely (stays in Low).
		IdleDpmGovernor clampGov {};
		clampGov.init(/*dpmEnabled=*/true, /*clampEnabled=*/true, 0ULL);
		expect(!clampGov.active() && clampGov.is_low(), "garnet-idle-clamp=1 bypasses governor in Low");
		expect(clampGov.on_activity(5000ULL * kMs) == IdleDpmAction::None,
		       "garnet-idle-clamp=1 never boosts to Auto");
		expect(clampGov.on_tick(5000ULL * kMs, true) == IdleDpmAction::None,
		       "garnet-idle-clamp=1 stays clamped even when hwBusy");

		// Light desktop compositing hysteresis: 1..5 submits/250 ms (4..20 Hz) drops
		// Auto -> Low and stays in Low; >= 8 submits/250 ms (32 Hz) or 50 ms hwBusy boosts.
		IdleDpmGovernor deskGov {};
		deskGov.init(/*dpmEnabled=*/true, /*clampEnabled=*/false, 1000ULL * kMs);
		// Sustained 24+ Hz (6 submits / 250 ms) keeps Auto alive indefinitely.
		expect(deskGov.on_tick(1500ULL * kMs, false, kIdleDpmKeepAutoSubmits) == IdleDpmAction::None,
		       "6 submits/250ms keeps Auto alive");
		expect(deskGov.on_tick(2250ULL * kMs, false, kIdleDpmKeepAutoSubmits) == IdleDpmAction::None,
		       "sustained 24 Hz keeps Auto alive past 1000 ms");
		// Drop to light idle desktop rate (4 submits / 250 ms = 16 Hz) -> drops to Low after 1000 ms.
		expect(deskGov.on_tick(2500ULL * kMs, false, 4u) == IdleDpmAction::None, "250 ms light desktop still Auto");
		expect(deskGov.on_tick(3000ULL * kMs, false, 3u) == IdleDpmAction::None, "750 ms light desktop still Auto");
		expect(deskGov.on_tick(3250ULL * kMs, false, 4u) == IdleDpmAction::SetLow,
		       "1000 ms of light desktop frames (< 6/250ms) drops Auto -> Low");
		deskGov.commit(IdleDpmAction::SetLow, true);
		expect(deskGov.is_low(), "light desktop entered Low");

		// While Low, 50 ms probe + 200 ms window with 4 submits and !hwBusy stays in Low.
		uint32_t nextMs = 0u; bool resetWin = false;
		expect(deskGov.on_low_probe(3300ULL * kMs, false, 1u, &nextMs, &resetWin) == IdleDpmAction::None &&
		       nextMs == kIdleDpmWindowRemainMs && !resetWin,
		       "50 ms probe with 1 idle submit arms 200 ms window remainder");
		expect(deskGov.on_low_probe(3500ULL * kMs, false, 4u, &nextMs, &resetWin) == IdleDpmAction::None &&
		       nextMs == 0u && resetWin && deskGov.is_low(),
		       "250 ms window with 4 idle submits resets and stays in Low");

		// Single heavy CB still running at 50 ms probe (hwBusy == true) boosts Low -> Auto.
		expect(deskGov.on_low_probe(4050ULL * kMs, true, 1u, &nextMs, &resetWin) == IdleDpmAction::SetAuto &&
		       resetWin,
		       "single heavy CB still busy at 50 ms probe boosts Low -> Auto");
		deskGov.commit(IdleDpmAction::SetAuto, true);
		expect(deskGov.on_tick(5050ULL * kMs, false, 2u) == IdleDpmAction::SetLow,
		       "returns to Low 1000 ms after heavy CB finishes");
		deskGov.commit(IdleDpmAction::SetLow, true);

		// High-rate burst (>= 8 submits / 250 ms) boosts Low -> Auto.
		expect(deskGov.on_low_probe(5100ULL * kMs, false, kIdleDpmBoostSubmits, &nextMs, &resetWin) == IdleDpmAction::SetAuto &&
		       resetWin,
		       "burst >= 8 submits/250ms boosts Low -> Auto");
		deskGov.commit(IdleDpmAction::SetAuto, true);
		expect(!deskGov.is_low() && deskGov.boostCount == 2u && deskGov.idleDropCount == 2u,
		       "hysteresis boost/drop counters match");
	}

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
	Fams2ModeProof proof {};
	expect(!uclk_unpin_safe_for_mode(false, true, true, det4k160, vb4k160, proof),
	       "without DAL/DML/FAMS2 proof, DSC stays pinned");
	proof = {true, true, true, true, true, 0x41u, 7u, 7u, false};
	expect(uclk_unpin_safe_for_mode(false, false, true, det4k160, vb4k160, proof),
	       "confirmed FAMS2/MALL permits DSC even without Stage 3 timing margins");
	expect(uclk_unpin_safe_for_mode(false, false, false, 0u, 0u, proof),
	       "confirmed FAMS2/MALL does not require a single expanded DET pipe");
	expect(!uclk_unpin_safe_for_mode(true, false, true, det4k160, vb4k160, proof),
	       "forced UCLK pin overrides confirmed hardware programming");
	for (bool Fams2ModeProof::*field : {&Fams2ModeProof::allowed, &Fams2ModeProof::dalActive,
	                                    &Fams2ModeProof::dmlPstateSupported,
	                                    &Fams2ModeProof::configCompleted, &Fams2ModeProof::mallProgrammed}) {
		Fams2ModeProof incomplete = proof;
		incomplete.*field = false;
		expect(!uclk_unpin_safe_for_mode(false, true, true, det4k160, vb4k160, incomplete),
		       "missing any FAMS2 dependency prevents a framebuffer UCLK floor change");
	}
	Fams2ModeProof stale = proof;
	stale.liveModeGeneration = 8u;
	expect(!uclk_unpin_safe_for_mode(false, true, true, det4k160, vb4k160, stale), "stale config keeps UCLK pinned");
	stale.liveModeGeneration = stale.configGeneration = 0u;
	expect(!uclk_unpin_safe_for_mode(false, true, true, det4k160, vb4k160, stale), "no active mode binding keeps UCLK pinned");
	proof.fwVersion = 21u;
	expect(!uclk_unpin_safe_for_mode(false, true, true, det4k160, vb4k160, proof), "decimal 21 is not the firmware's packed version");
	proof.fwVersion = 0x41u;
	proof.underflowLatched = true;
	expect(!uclk_unpin_safe_for_mode(false, true, false, det4k60, vb4k60, proof),
	       "an underflow overrides even the legacy timing-margin fallback");

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
