#include "dcn/dcn41_mall_pure.h"
#include "dcn41_allow.h"

#include <cstdio>
#include <vector>

namespace {

unsigned checks = 0, failures = 0;

void expect(bool condition, const char *message) {
	++checks;
	if (!condition) {
		++failures;
		std::printf("FAIL: %s\n", message);
	}
}

constexpr uint32_t kSegs[] = {DCN41_SEG0_EXPECTED, DCN41_SEG1_EXPECTED, DCN41_SEG2_EXPECTED,
                              DCN41_SEG3_EXPECTED, DCN41_SEG4_EXPECTED};
constexpr uint32_t kMall = kSegs[2] + DCN41_HUBP_DCHUBP_MALL_CONFIG(0);
constexpr uint32_t kArb = kSegs[2] + DCN41_DCHUBBUB_ARB_MALL_CNTL;
constexpr uint32_t kRp = kSegs[2] + DCN41_DMCUB_INBOX1_RPTR;
constexpr uint32_t kWp = kSegs[2] + DCN41_DMCUB_INBOX1_WPTR;
constexpr uint32_t kBoot = kSegs[2] + DCN41_DMCUB_SCRATCH0;
constexpr uint32_t kOtherMall = kSegs[2] + DCN41_HUBP_DCHUBP_MALL_CONFIG(1);
constexpr uint32_t kHubp = kSegs[2] + DCN41_HUBP_DCHUBP_CNTL(0);
constexpr uint32_t kOtherHubp = kSegs[2] + DCN41_HUBP_DCHUBP_CNTL(1);
constexpr uint32_t kSurfaceLo = kSegs[2] + DCN41_HUBPREQ_DCSURF_SURFACE_EARLIEST_INUSE(0);
constexpr uint32_t kBusy = DCN41_DCHUBBUB_ARB_MALL_CNTL__GLOBAL_USE_MALL_FOR_SS_MASK |
                           DCN41_DCHUBBUB_ARB_MALL_CNTL__MALL_IN_USE_MASK |
                           DCN41_DCHUBBUB_ARB_MALL_CNTL__MALL_PREFETCH_COMPLETE_MASK;
constexpr uint64_t kFront = 0x40000000u;

struct SimCab {
	dcn41_dev dev {};
	dcn41_allow_state allow {};
	dcn41_dmub_ring ring {};
	uint32_t regs[0x10000] {};
	uint32_t commands[64] {};
	bool rejectEnable = false, rejectDisable = false, dropSelect = false;
	std::vector<uint32_t> writes;

	SimCab() {
		auto read = [](void *cookie, uint32_t addr) { return static_cast<SimCab *>(cookie)->regs[addr]; };
		auto write = [](void *cookie, uint32_t addr, uint32_t value) {
			auto &sim = *static_cast<SimCab *>(cookie);
			if (!dcn41_allow_write(&sim.allow, addr, value, "mall-test")) return;
			sim.writes.push_back(addr);
			if (addr == kMall && sim.dropSelect) return;
			if (addr == kWp) {
				const uint32_t header = sim.commands[sim.regs[kRp] / sizeof(uint32_t)];
				if (header == 0x0200024bu && !sim.rejectEnable) sim.regs[kArb] = kBusy;
				if (header == 0x0200004bu && !sim.rejectDisable) sim.regs[kArb] = 0;
				sim.regs[kRp] = value;
			}
			sim.regs[addr] = value;
		};
		expect(dcn41_dev_init(&dev, this, read, write, [](void *, uint32_t) {}, kSegs, 0x10000, 0) == DCN41_OK &&
		       dcn41_allow_init(&allow, kSegs, 0x10000) == DCN41_ALLOW_OK, "attach mock DCN");
		dev.hubp_request_addr[0] = regs[kSurfaceLo] = kFront;
		dev.hubp_request_valid[0] = 1u;
		regs[kBoot] = DCN41_DMUB_BOOT_DAL_FW | DCN41_DMUB_BOOT_MAILBOX_RDY |
		              DCN41_DMUB_BOOT_HW_POWER_INIT_DONE;
		regs[kMall] = 0x80000010u;
		regs[kOtherMall] = 0x80000008u;
		for (uint32_t hubp = 1u; hubp < DCN41_NUM_PIPES; ++hubp)
			regs[kSegs[2] + DCN41_HUBP_DCHUBP_CNTL(hubp)] = DCN41_HUBP_DCHUBP_CNTL__HUBP_BLANK_EN_MASK;
		regs[kSegs[2] + DCN41_OTG_OTG_CONTROL(0)] = DCN41_OTG_OTG_CONTROL__OTG_MASTER_EN_MASK;
		dcn41_dmub_state boot {};
		boot.verdict = DCN41_DMUB_ALIVE_IDLE;
		boot.ring_map = DCN41_DMUB_MAP_CW4;
		boot.ring_addr = 0x100u;
		boot.inbox1_size = sizeof(commands);
		auto memRead = [](void *cookie, uint64_t addr) {
			return static_cast<SimCab *>(cookie)->commands[(addr - 0x100u) / sizeof(uint32_t)];
		};
		auto memWrite = [](void *cookie, uint64_t addr, uint32_t value) {
			static_cast<SimCab *>(cookie)->commands[(addr - 0x100u) / sizeof(uint32_t)] = value;
		};
		expect(dcn41_dmub_ring_attach(&ring, &dev, &boot, memRead, memWrite, this) == DCN41_OK, "attach mock inbox");
	}
};

void check_transaction() {
	using namespace rdna4mall;
	using amdgpu::AsicId;
	SimCab sim;
	CabState state {};
	CabFront front { AsicId::Navi44, 8u, 0u, 0u, 11u, kFront, 3840u * 2160u * 4u,
	                 64u, 64u, true, true, true, false, false, false };
	front.sealed = false;
	expect(cab_enter(sim.ring, state, front, 11u) == DCN41_E_STATE && sim.writes.empty(), "unsealed data cannot arm CAB");
	front.sealed = true;
	expect(cab_enter(sim.ring, state, front, 12u) == DCN41_E_STATE && sim.writes.empty(), "stale topology cannot arm CAB");
	front.psr = true;
	expect(cab_enter(sim.ring, state, front, 11u) == DCN41_E_STATE && sim.writes.empty(), "PSR prevents CAB");
	front.psr = false;
	sim.regs[kBoot] &= ~DCN41_DMUB_BOOT_DAL_FW;
	expect(cab_enter(sim.ring, state, front, 11u) == DCN41_E_STATE && sim.writes.empty(), "VBIOS DMUB cannot arm CAB");
	sim.regs[kBoot] |= DCN41_DMUB_BOOT_DAL_FW;
	sim.regs[kHubp] = 1u << DCN41_HUBP_DCHUBP_CNTL__HUBP_VTG_SEL__SHIFT;
	expect(cab_enter(sim.ring, state, front, 11u) == DCN41_E_STATE && sim.writes.empty(), "wrong HUBP timing route refuses CAB");
	sim.regs[kHubp] = 0u;
	sim.regs[kOtherHubp] = 0u;
	expect(cab_enter(sim.ring, state, front, 11u) == DCN41_E_STATE && sim.writes.empty(), "second unaccounted plane refuses CAB");
	sim.regs[kOtherHubp] = DCN41_HUBP_DCHUBP_CNTL__HUBP_BLANK_EN_MASK;
	sim.regs[kSurfaceLo] += 0x100u;
	expect(cab_enter(sim.ring, state, front, 11u) == DCN41_E_STATE && sim.writes.empty(), "EARLIEST_INUSE not the sealed front");
	sim.regs[kSurfaceLo] = kFront;
	sim.dropSelect = true;
	expect(cab_enter(sim.ring, state, front, 11u) == DCN41_E_READBACK && cab_leave(sim.ring, state) == DCN41_OK &&
	       sim.commands[0] == 0u, "denied HUBP write never submits FIT");
	sim.dropSelect = false;
	sim.writes.clear();
	const int entered = cab_enter(sim.ring, state, front, 11u);
	expect(entered == DCN41_OK && sim.writes == std::vector<uint32_t>{kMall, kWp} &&
	       sim.regs[kMall] == 0x8000000eu && sim.regs[kOtherMall] == 0x80000008u &&
	       sim.commands[0] == 0x0200024bu && sim.commands[1] == 16u,
	       "CAB touches only the mapped HUBP and publishes FIT after cursor/prefetch selection");
	sim.rejectDisable = true;
	expect(cab_leave(sim.ring, state) == DCN41_E_TIMEOUT && sim.regs[kMall] == 0x8000000eu,
	       "NO_IDLE consumed without arbiter quiescence still forbids producer mutation");
	sim.rejectDisable = false;
	sim.writes.clear();
	expect(cab_leave(sim.ring, state) == DCN41_OK && sim.writes == std::vector<uint32_t>{kWp, kMall} &&
	       sim.regs[kMall] == 0x80000010u && sim.regs[kArb] == 0u && sim.allow.refused == 0u,
	       "retry releases arbiter before restoring only owned HUBP fields");
	sim.writes.clear();
	expect(cab_leave(sim.ring, state) == DCN41_OK && sim.writes.empty(), "idle pre-write barrier is an MMIO no-op");
	sim.rejectEnable = true;
	expect(cab_enter(sim.ring, state, front, 11u) == DCN41_E_TIMEOUT &&
	       sim.writes == std::vector<uint32_t>{kMall, kWp, kWp, kMall} && sim.regs[kMall] == 0x80000010u,
	       "FIT consumption without arbiter activation rolls back before returning");
}

} // namespace

int main() {
	using namespace rdna4mall;
	using amdgpu::AsicId;

	constexpr uint64_t mib = 1u << 20;
	expect(physical_bytes(AsicId::Navi44) == 32u * mib &&
	       physical_bytes(AsicId::Navi48) == 64u * mib, "physical Infinity Cache capacities");
	expect(display_budget(AsicId::Navi44, 8u) == 32u * mib &&
	       display_budget(AsicId::Navi44, 6u) == 16u * mib, "BIOS memory channels round down to a power of two");
	expect(display_budget(AsicId::Navi48, 16u) == 64u * mib &&
	       display_budget(AsicId::Navi48, 8u) == 32u * mib &&
	       display_budget(AsicId::Navi44, 16u) == 32u * mib,
	       "display CAB reflects actual memory channels up to each chip's physical capacity");
	expect(display_budget(AsicId::Navi44, 0u) == 0u && display_budget(AsicId::Navi44, 1u) == 0u &&
	       display_budget(AsicId::Unknown, 16u) == 0u, "unsupported topology cannot allocate CAB");

	const uint64_t cab = display_budget(AsicId::Navi44, 8u);
	expect(cab_ways(cab, 0u, 0u) == 0u, "no memory demand needs zero ways");
	expect(cab_ways(cab, cab / 16u - 128u, 0u) == 1u &&
	       cab_ways(cab, cab / 16u - 64u, 0u) == 2u, "CAB ways account for upstream two-line alignment margin");
	expect(cab_ways(cab, cab - 128u, 0u) == 16u &&
	       cab_ways(cab, cab - 128u, 65536u) == kDoesNotFit, "cursor bytes are included before way fit");
	expect(cab_ways(cab, UINT64_MAX, 1u) == kDoesNotFit && cab_ways(0u, 1u, 0u) == kDoesNotFit,
	       "overflow and unknown budget never admit CAB");

	dcn41_dmub_cmd cmd {};
	build_cab_command(cmd, kFit, 16u);
	expect(cmd.header == 0x0200024bu && cmd.payload[0] == 16u && cmd.payload[1] == 0u &&
	       cmd.payload[59] == 0u, "FIT command exactly matches Linux DMUB type, subtype and payload");
	build_cab_command(cmd, kNoIdle, 0u);
	expect(cmd.header == 0x0200004bu && cmd.payload[0] == 0u, "CAB stop clears allocation and uses NO_IDLE");
	build_cab_command(cmd, kNotFit, 0u);
	expect(cmd.header == 0x0200034bu && cmd.payload[0] == 0u, "NOT_FIT matches the firmware ABI");
	build_cab_command(cmd, kFit, kDoesNotFit);
	expect(cmd.header == 0x0200034bu && cmd.payload[0] == 0u, "invalid FIT count never reaches firmware");
	check_transaction();

	std::printf("dcn41_fams2_mall_test: %u/%u passed\n", checks - failures, checks);
	return failures != 0;
}
