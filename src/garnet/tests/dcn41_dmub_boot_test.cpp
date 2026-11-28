#include "../src/dcn/dcn41_dmub_boot_pure.h"
#include "../src/amd/amdgpu_ucode_extract.h"
#include "../../dcn41/dcn41_allow_ranges.h"

#include <cstdio>
#include <cstring>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

namespace {
int gRun = 0, gFail = 0;
void check(bool ok, const char *msg) {
    ++gRun;
    if (!ok) { ++gFail; std::printf("FAIL  %s\n", msg); }
    else { std::printf("ok    %s\n", msg); }
}

constexpr uint32_t kSegs[5] = { 0x12u, 0xc0u, 0x34c0u, 0x9000u, 0x2403c00u };
constexpr uint32_t kStopCode = DCN41_DMUB_GPINT_STOP_FW << DCN41_DMUB_GPINT_COMMAND_SHIFT;
constexpr uint32_t kGpint = kSegs[2] + DCN41_DMCUB_GPINT_DATAIN1;
constexpr uint32_t kCntl = kSegs[2] + DCN41_DMCUB_CNTL;
constexpr uint32_t kCntl2 = kSegs[2] + DCN41_DMCUB_CNTL2;
constexpr uint32_t kCw4Top = kSegs[2] + DCN41_DMCUB_REGION3_CW4_TOP_ADDRESS;

void test_vbios_inbox_reservation() {
    constexpr uint64_t kVramSize = 16ULL << 30;
    constexpr uint64_t kReserveStart = 0x3dac00000ULL;
    constexpr uint32_t kReserveSize = 4U << 20;
    dcn41_dmub_state state {};
    state.enabled = state.mailbox_rdy = 1;
    state.verdict = DCN41_DMUB_ALIVE_IDLE;
    state.ring_map = DCN41_DMUB_MAP_REGION4;
    state.fb_base_mc = 0x8000000000ULL;
    state.ring_addr = state.fb_base_mc + kReserveStart;
    state.inbox1_size = DCN41_DMUB_RB_SIZE;
    check(dcn41_dmub_ring_in_vram_reservation(&state, kVramSize, kReserveStart, kReserveSize),
          "VBIOS inbox at the observed PSP extended reservation start fits");
    check(!dcn41_dmub_ring_in_vram_reservation(&state, kVramSize, kReserveStart + 0x1000, kReserveSize),
          "inbox below the reservation start is not owned by the reservation");
    check(!dcn41_dmub_ring_in_vram_reservation(&state, kVramSize, kReserveStart, DCN41_DMUB_RB_SIZE - 1),
          "partial overlap cannot prove ownership of the entire inbox");
    check(!dcn41_dmub_ring_in_vram_reservation(&state, kReserveStart + DCN41_DMUB_RB_SIZE, kReserveStart, kReserveSize),
          "reservation crossing VRAM top is not valid");
    check(!dcn41_dmub_ring_in_vram_reservation(&state, kVramSize, UINT64_MAX, kReserveSize),
          "wrapped PSP reservation is not valid");
    state.ring_addr = state.fb_base_mc - DCN41_DMUB_RB_SIZE;
    check(!dcn41_dmub_ring_in_vram_reservation(&state, kVramSize, kReserveStart, kReserveSize),
          "invalid MC-to-VRAM conversion fails closed");
    state.ring_addr = state.fb_base_mc + kReserveStart;
    state.ring_map = DCN41_DMUB_MAP_UNKNOWN;
    check(!dcn41_dmub_ring_in_vram_reservation(&state, kVramSize, kReserveStart, kReserveSize),
          "unknown DMUB mapping does not prove reservation ownership");
}

struct SimDmubHw {
    dcn41_dev dev {};
    std::vector<uint32_t> regs;
    std::vector<uint32_t> originalRegs;
    std::vector<std::string> callOrder;
    uint32_t ignoredReg { UINT32_MAX };
    uint32_t writes { 0 };
    bool injectPreflightFail { false };
    bool injectStopTimeout { false };
    bool omitStopAck { false };
    bool omitStopResponse { false };
    bool injectPwaitTimeout { false };
    bool injectWindowFail { false };
    bool injectPspFail { false };
    bool injectBootTimeout { false };
    bool injectDeadBootStatus { false };
    bool injectCapsFail { false };
    uint32_t capsReplyHeaderXor { 0 };
    bool injectRestoreFail { false };
    dcn41_dmub_feature_caps fakeCaps {};

    SimDmubHw() : regs(0x10000u, 0u) {
        dcn41_dev_init(&dev, this, &SimDmubHw::rd, &SimDmubHw::wr, &SimDmubHw::udelay, kSegs, 0x10000u, 0);
        // Simulate running VBIOS DMUB at boot
        regs[0x34c0u + DCN41_DMCUB_CNTL] = DCN41_DMCUB_CNTL__DMCUB_ENABLE_MASK;
        regs[0x34c0u + DCN41_DMCUB_CNTL2] = 0u;
        regs[0x34c0u + DCN41_DMCUB_SCRATCH0] = DCN41_DMUB_BOOT_MAILBOX_RDY;
        fakeCaps.fw_assisted_mclk_switch_ver = 0x41u;
        fakeCaps.subvp_psr_support = 1u;
        fakeCaps.lsdma_support_in_dmu = 1u;
    }

    static uint32_t rd(void *c, uint32_t a) {
        auto *s = static_cast<SimDmubHw *>(c);
        return s->regs[a];
    }

    static void wr(void *c, uint32_t a, uint32_t v) {
        auto *s = static_cast<SimDmubHw *>(c);
        ++s->writes;
        if (a == s->ignoredReg) return;
        s->regs[a] = v;
        if (a == 0x34c0u + DCN41_DMCUB_GPINT_DATAIN1 && (v & DCN41_DMUB_GPINT_STATUS_MASK) &&
            !s->injectStopTimeout) {
            if (!s->omitStopAck)
                s->regs[a] = v & ~DCN41_DMUB_GPINT_STATUS_MASK;
            if (!s->omitStopResponse)
                s->regs[0x34c0u + DCN41_DMCUB_SCRATCH7] = DCN41_DMUB_GPINT_STOP_FW_RESPONSE;
            if (!s->injectPwaitTimeout)
                s->regs[0x34c0u + DCN41_DMCUB_CNTL] |= DCN41_DMCUB_CNTL__DMCUB_PWAIT_MODE_STATUS_MASK;
        }
        if (a == 0x34c0u + DCN41_DMCUB_CNTL2 && !s->injectBootTimeout &&
            (v & DCN41_DMCUB_CNTL2__DMCUB_SOFT_RESET_MASK) == 0u) {
            s->regs[0x34c0u + DCN41_DMCUB_SCRATCH0] =
                s->injectDeadBootStatus ? UINT32_MAX : DCN41_DMUB_BOOT_DAL_FW |
                DCN41_DMUB_BOOT_MAILBOX_RDY | DCN41_DMUB_BOOT_HW_POWER_INIT_DONE;
        }
        // Validate every write falls inside dcn41_allow_ranges!
        bool allowed = false;
        for (uint32_t i = 0; i < DCN41_ALLOW_RANGE_COUNT; ++i) {
            if (a >= dcn41_allow_ranges[i].abs_lo && a <= dcn41_allow_ranges[i].abs_hi) {
                allowed = true;
                break;
            }
        }
        check(allowed, "DMUB write within dcn41_allow_ranges");
    }

    static void udelay(void *, uint32_t) {}

    bool prepare_handoff(const rdna4dmub::RegionLayout &) {
        callOrder.push_back("prepare");
        originalRegs = regs;
        return !injectPreflightFail;
    }

    int stop_and_reset(uint32_t timeoutUs) {
        callOrder.push_back("stop");
        return dcn41_dmub_stop_and_reset(&dev, timeoutUs);
    }

    int program_windows(const dcn41_dmub_handoff_windows &w) {
        callOrder.push_back("windows");
        if (injectWindowFail) return DCN41_E_STATE;
        return dcn41_dmub_program_windows(&dev, &w);
    }

    bool psp_load_dmub_fw() {
        callOrder.push_back("psp");
        return !injectPspFail;
    }

    int release_and_wait(uint32_t pspVersion, uint32_t timeoutUs) {
        callOrder.push_back("release");
        return dcn41_dmub_release_and_wait(&dev, pspVersion, timeoutUs);
    }

    bool submit_and_read_reply(dcn41_dmub_cmd &cmd, uint32_t timeoutUs) {
        callOrder.push_back("caps");
        const uint32_t expected = dcn41_dmub_header(DCN41_DMUB_CMD_QUERY_FEATURE_CAPS, 0u, sizeof(fakeCaps), true);
        check(cmd.header == expected, "capability query asks firmware to clear ret_status after consuming request");
        cmd.header = (expected & ~DCN41_DMUB_HDR_RET_STATUS_MASK) ^ capsReplyHeaderXor;
        std::memcpy(cmd.payload, &fakeCaps, sizeof(fakeCaps));
        return !injectCapsFail && timeoutUs != 0u;
    }

    bool restore_vbios(uint32_t) {
        callOrder.push_back("restore");
        if (injectRestoreFail) return false;
        regs = originalRegs;
        return true;
    }
};

std::vector<uint8_t> make_signed_fixture(bool dalFw, uint32_t footerBytes = 256u) {
    std::vector<uint8_t> buf(256u + 4096u + footerBytes, 0u);
    dcn41_dmub_fw_meta_info meta {};
    meta.magic_value = DCN41_DMUB_FW_META_MAGIC;
    meta.fw_region_size = 0x4000u;
    meta.trace_buffer_size = 0x10000u;
    meta.fw_version = 0x0A005000u;
    meta.dal_fw = dalFw ? 1u : 0u;
    meta.shared_state_size = 0x1000u;
    std::memcpy(buf.data() + buf.size() - footerBytes - 64u - 4u, &meta, sizeof(meta));
    return buf;
}
} // namespace

int main(int argc, char **argv) {
    test_vbios_inbox_reservation();
    check(!rdna4dmub::dmcub_fw_opt_in(false, 0u), "garnet-dmcub-fw absent defaults OFF");
    check(!rdna4dmub::dmcub_fw_opt_in(true, 0u), "garnet-dmcub-fw=0 disables DAL DMUB");
    check(rdna4dmub::dmcub_fw_opt_in(true, 1u), "garnet-dmcub-fw=1 opts in");

    const auto inst = make_signed_fixture(true);
    constexpr uint64_t kArenaMc = 0x8020000000ULL;
    constexpr uint64_t kArenaSz = 4ULL * 1024ULL * 1024ULL;

    const dcn41_dmub_fw_meta_info emptyMeta {};
    const auto ringOnly = rdna4dmub::plan_region_layout(kArenaMc, kArenaSz, 4096u, 0u, emptyMeta);
    check(ringOnly.valid && ringOnly.fams2IbGpuAddr == 0u && ringOnly.fams2IbAllocBytes == 0u,
          "ring-only FAMS2 v2.1 reserves no indirect command buffer");
    const auto withIb = rdna4dmub::plan_region_layout(kArenaMc, kArenaSz, 4096u, 0u, emptyMeta,
        rdna4dmub::kDmubVbiosWindowSize, 0x1001u);
    check(withIb.valid && withIb.fams2IbGpuAddr % 4096u == 0u &&
          withIb.fams2IbGpuAddr == withIb.sharedStateGpuAddr + withIb.sharedStateSize &&
          withIb.fams2IbAllocBytes == 0x2000u && withIb.totalBytes == ringOnly.totalBytes + 0x2000u,
          "caller-supplied indirect buffer gets a separate page-rounded allocation");
    check(!rdna4dmub::plan_region_layout(kArenaMc, withIb.totalBytes - 1u, 4096u, 0u, emptyMeta,
              rdna4dmub::kDmubVbiosWindowSize, 0x1001u).valid &&
          !rdna4dmub::plan_region_layout(kArenaMc + 1u, kArenaSz, 4096u, 0u, emptyMeta,
              rdna4dmub::kDmubVbiosWindowSize, 0x1001u).valid,
          "indirect allocation must fit the arena and use an aligned base");
    for (const uint32_t excessiveBytes : {UINT32_MAX, UINT32_MAX - 0xFFFu}) {
        check(!rdna4dmub::plan_region_layout(kArenaMc, UINT64_MAX, 4096u, 0u, emptyMeta,
                  rdna4dmub::kDmubVbiosWindowSize, excessiveBytes).valid,
              "indirect size and total cannot wrap 32-bit allocation lengths");
    }

    for (const uint32_t footerBytes : {256u, 512u}) {
        const auto signedSlice = make_signed_fixture(true, footerBytes);
        dcn41_dmub_fw_meta_info meta {};
        uint32_t codeBytes = 0;
        check(dcn41_dmub_fw_meta_from_psp(signedSlice.data(), static_cast<uint32_t>(signedSlice.size()), &meta, &codeBytes) ==
                  DCN41_OK && meta.fw_version == 0x0A005000u && meta.dal_fw == 1u && codeBytes == 4096u,
              "DMUB meta scan skips signed PSP header/footer");
        check(dcn41_dmub_fw_meta_from_psp(signedSlice.data(), 512u, &meta, nullptr) == DCN41_E_ARG,
              "truncated signed PSP image is rejected before scanning");
        SimDmubHw hw;
        const auto result = rdna4dmub::execute_handoff(hw, true, 1u, signedSlice.data(),
            static_cast<uint32_t>(signedSlice.size()), 0u, kArenaMc, kArenaSz, 0u, 4u, 4u);
        check(result.usingDalFw && result.layout.cw0Bytes == 4096u,
              "handoff uses signed input and sizes CW0 from stripped code length");
    }

    if (argc > 1) {
        std::ifstream input(argv[1], std::ios::binary);
        const std::vector<uint8_t> file((std::istreambuf_iterator<char>(input)), {});
        amdgpu::UcodePayload payloads[amdgpu::kMaxUcodePayloadsPerFile] {};
        const uint32_t count = amdgpu::amdgpu_ucode_extract(amdgpu::kHostFwFile_DCN_DMUB, file.data(), file.size(), payloads);
        check(count == 1u, "real firmware produces one signed PSP payload");
        if (count == 1u) {
            SimDmubHw hw;
            const auto &payload = payloads[0];
            const auto result = rdna4dmub::execute_handoff(hw, true, 1u, file.data() + payload.offset_bytes,
                payload.size_bytes, 0u, kArenaMc, kArenaSz, 0u, 4u, 4u);
            check(result.usingDalFw && result.fwVersion != 0u && result.layout.valid,
                  "handoff accepts the real signed payload selected by the loader");
        }
    }

    for (const uint32_t existingRequest : {kStopCode, 1u << DCN41_DMUB_GPINT_STATUS_SHIFT}) {
        SimDmubHw hw;
        hw.regs[kGpint] = existingRequest;
        hw.regs[kCntl] |= DCN41_DMCUB_CNTL__DMCUB_PWAIT_MODE_STATUS_MASK;
        hw.regs[kSegs[2] + DCN41_DMCUB_SCRATCH7] = DCN41_DMUB_GPINT_STOP_FW_RESPONSE;
        check(dcn41_dmub_stop_and_reset(&hw.dev, 4u) == DCN41_E_STATE &&
              hw.writes == 0u && hw.regs[kGpint] == existingRequest,
              "in-flight GPINT and pre-acked STOP are rejected without changing another request");
    }

    {
        SimDmubHw hw;
        hw.regs[kCntl] |= DCN41_DMCUB_CNTL__DMCUB_PWAIT_MODE_STATUS_MASK;
        hw.regs[kSegs[2] + DCN41_DMCUB_SCRATCH7] = DCN41_DMUB_GPINT_STOP_FW_RESPONSE;
        check(dcn41_dmub_stop_and_reset(&hw.dev, 4u) == DCN41_E_STATE && hw.writes == 0u,
              "pre-existing DEADDEAD and PWAIT cannot certify a new STOP");
    }

    for (const uint32_t ignoredReg : {kCntl2, kCntl}) {
        SimDmubHw hw;
        hw.ignoredReg = ignoredReg;
        hw.regs[kCw4Top] = DCN41_DMCUB_REGION3_CW4_TOP_ADDRESS__DMCUB_REGION3_CW4_ENABLE_MASK;
        check(dcn41_dmub_stop_and_reset(&hw.dev, 4u) == DCN41_E_READBACK && hw.regs[kCw4Top] != 0u,
              "dropped reset or disable write leaves VBIOS windows intact");
    }

    {
        SimDmubHw hw;
        hw.omitStopAck = true;
        hw.regs[kCw4Top] = DCN41_DMCUB_REGION3_CW4_TOP_ADDRESS__DMCUB_REGION3_CW4_ENABLE_MASK;
        check(dcn41_dmub_stop_and_reset(&hw.dev, 4u) == DCN41_E_TIMEOUT && hw.regs[kCw4Top] != 0u &&
              hw.regs[kCntl2] == 0u && hw.regs[kGpint] == 0u,
              "DEADDEAD and PWAIT do not replace the GPINT STOP acknowledgement");
    }
    {
        SimDmubHw hw;
        hw.omitStopResponse = true;
        hw.regs[kCw4Top] = DCN41_DMCUB_REGION3_CW4_TOP_ADDRESS__DMCUB_REGION3_CW4_ENABLE_MASK;
        check(dcn41_dmub_stop_and_reset(&hw.dev, 4u) == DCN41_E_TIMEOUT && hw.regs[kCw4Top] != 0u &&
              hw.regs[kCntl2] == 0u && hw.regs[kGpint] == 0u,
              "GPINT STOP acknowledgement and PWAIT do not replace DEADDEAD");
    }

    // 1. Default OFF fails closed to VBIOS without touching HW
    {
        SimDmubHw hw;
        auto res = rdna4dmub::execute_handoff(hw, false, 0u, inst.data(), (uint32_t)inst.size(),
                                              0u, kArenaMc, kArenaSz, 0x003a1014u, 8u, 8u);
        check(!res.usingDalFw && res.fallbackVbios &&
              res.stage == rdna4dmub::HandoffStage::DisabledByPolicy && hw.callOrder.empty(),
              "default OFF touches no hardware and falls back to VBIOS DMUB");
    }

    // 2. Opt-in happy path
    {
        SimDmubHw hw;
        auto res = rdna4dmub::execute_handoff(hw, true, 1u, inst.data(), (uint32_t)inst.size(),
                                              0u, kArenaMc, kArenaSz, 0x003a1014u, 8u, 8u);
        check(res.usingDalFw && !res.fallbackVbios &&
              res.stage == rdna4dmub::HandoffStage::ActiveDalFw &&
              res.fams2Version == 0x41u && res.fwVersion == 0x0A005000u &&
              hw.callOrder == std::vector<std::string>{"prepare", "stop", "windows", "psp", "release", "caps"},
              "opt-in happy path executes strict STOP->WINDOWS->PSP->RELEASE->CAPS sequence");
        check((hw.regs[0x34c0u + DCN41_DMCUB_INBOX1_BASE_ADDRESS] == DCN41_DMUB_CW4_BASE) &&
              (hw.regs[0x34c0u + DCN41_DMCUB_INBOX1_SIZE] == DCN41_DMUB_RB_SIZE) &&
              (hw.regs[0x34c0u + DCN41_DMCUB_OUTBOX1_BASE_ADDRESS] == DCN41_DMUB_CW4_BASE + DCN41_DMUB_RB_SIZE) &&
              ((hw.regs[0x34c0u + DCN41_DMCUB_CNTL2] & DCN41_DMCUB_CNTL2__DMCUB_SOFT_RESET_MASK) == 0u),
              "CW4 inbox/outbox configured and soft reset released");
        check(res.layout.fams2IbGpuAddr == 0ULL && res.layout.fams2IbAllocBytes == 0u &&
              res.layout.totalBytes <= kArenaSz,
              "handoff for FAMS2 v2.1 leaves indirect command buffers unallocated");
        check(hw.regs[kSegs[2] + DCN41_DMCUB_SCRATCH15] == (0x003a1014u & 0x001100FFu),
              "SCRATCH15 uses the caller's masked PSP version, not DAL firmware metadata");
        check(res.layout.hw.cw1.top - res.layout.hw.cw1.base + 1u == 640u * 1024u,
              "CW1 provides 128 KiB stack and 512 KiB context");
        check(hw.regs[0x34c0u + DCN41_DMCUB_OUTBOX0_BASE_ADDRESS] == DCN41_DMUB_OUTBOX0_TRACE_BASE &&
              hw.regs[0x34c0u + DCN41_DMCUB_OUTBOX0_SIZE] == res.layout.traceSize - DCN41_DMUB_TRACE_ENTRY_OFFSET &&
              hw.regs[0x34c0u + DCN41_DMCUB_REGION3_CW0_TOP_ADDRESS] == 0u &&
              hw.regs[0x34c0u + DCN41_DMCUB_REGION3_CW1_TOP_ADDRESS] == 0u &&
              hw.regs[0x34c0u + DCN41_DMCUB_SEC_CNTL] == 0u,
              "trace ring is mapped without writing PSP's code, stack or security registers");
    }

    // 3. Planted defect: STOP_FW missing 0xDEADDEAD timeout -> fail closed
    {
        SimDmubHw hw;
        hw.injectStopTimeout = true;
        hw.injectRestoreFail = true;
        auto res = rdna4dmub::execute_handoff(hw, true, 1u, inst.data(), (uint32_t)inst.size(),
                                              0u, kArenaMc, kArenaSz, 0x003a1014u, 4u, 4u);
        check(!res.usingDalFw && !res.fallbackVbios && res.recoveryRequired &&
              res.stage == rdna4dmub::HandoffStage::StopTimeout &&
              hw.callOrder == std::vector<std::string>{"prepare", "stop", "restore"},
              "STOP timeout tries rollback before PSP load; failed rollback is visible");
        check((hw.regs[0x34c0u + DCN41_DMCUB_CNTL] & DCN41_DMCUB_CNTL__DMCUB_ENABLE_MASK) != 0u &&
              (hw.regs[0x34c0u + DCN41_DMCUB_CNTL2] & DCN41_DMCUB_CNTL2__DMCUB_SOFT_RESET_MASK) == 0u,
              "missing STOP acknowledgement must not assert reset");
    }

    // 4. Planted defect: PSP load failure -> fail closed
    {
        SimDmubHw hw;
        hw.injectPspFail = true;
        auto res = rdna4dmub::execute_handoff(hw, true, 1u, inst.data(), (uint32_t)inst.size(),
                                              0u, kArenaMc, kArenaSz, 0x003a1014u, 4u, 4u);
        check(!res.usingDalFw && res.fallbackVbios &&
              res.stage == rdna4dmub::HandoffStage::PspLoadFailed &&
              hw.callOrder == std::vector<std::string>{"prepare", "stop", "windows", "psp", "restore"} &&
              hw.regs == hw.originalRegs,
              "PSP failure reports VBIOS only after the recovery callback succeeds");
    }

    // 5. Planted defect: DAL boot SCRATCH0 timeout -> fail closed
    {
        SimDmubHw hw;
        hw.injectBootTimeout = true;
        auto res = rdna4dmub::execute_handoff(hw, true, 1u, inst.data(), (uint32_t)inst.size(),
                                              0u, kArenaMc, kArenaSz, 0x003a1014u, 4u, 4u);
        check(!res.usingDalFw && res.fallbackVbios &&
              res.stage == rdna4dmub::HandoffStage::BootWaitTimeout,
              "planted DAL SCRATCH0 timeout fails closed");
    }

    // 6. Planted defect: dal_fw=0 or bad metadata or tiny arena -> fail closed
    {
        SimDmubHw hw;
        const auto badMeta = make_signed_fixture(false);
        auto res1 = rdna4dmub::execute_handoff(hw, true, 1u, badMeta.data(), (uint32_t)badMeta.size(),
                                               0u, kArenaMc, kArenaSz, 0x003a1014u, 4u, 4u);
        auto res2 = rdna4dmub::execute_handoff(hw, true, 1u, inst.data(), (uint32_t)inst.size(),
                                               0u, kArenaMc, 0x4000ULL, 0x003a1014u, 4u, 4u);
        check(res1.stage == rdna4dmub::HandoffStage::InvalidMetadata &&
              res2.stage == rdna4dmub::HandoffStage::InvalidLayout && hw.callOrder.empty(),
              "non-DAL firmware or tiny arena rejected cleanly without HW touch");
    }

    {
        SimDmubHw hw;
        hw.fakeCaps.fw_assisted_mclk_switch_ver = 21u;
        const auto result = rdna4dmub::execute_handoff(hw, true, 1u, inst.data(), (uint32_t)inst.size(),
                                                       0u, kArenaMc, kArenaSz, 0u, 4u, 4u);
        check(!result.usingDalFw && result.stage == rdna4dmub::HandoffStage::FeatureCapsFailed,
              "reject a FAMS capability byte other than (2 << 5) | 1");
    }
    {
        SimDmubHw hw;
        hw.fakeCaps.subvp_psr_support = 0u;
        const auto result = rdna4dmub::execute_handoff(hw, true, 1u, inst.data(), (uint32_t)inst.size(),
                                                       0u, kArenaMc, kArenaSz, 0u, 4u, 4u);
        check(result.usingDalFw && result.caps.subvp_psr_support == 0u && result.fams2Version == 0x41u,
              "FAMS2 v2.1 is valid without the separate SubVP+PSR capability");
    }
    for (const uint32_t headerXor : { DCN41_DMUB_HDR_RET_STATUS_MASK, 1u, 1u << DCN41_DMUB_HDR_PAYLOAD_BYTES_SHIFT }) {
        SimDmubHw hw;
        hw.capsReplyHeaderXor = headerXor;
        const auto result = rdna4dmub::execute_handoff(hw, true, 1u, inst.data(), (uint32_t)inst.size(),
                                                       0u, kArenaMc, kArenaSz, 0u, 4u, 4u);
        check(!result.usingDalFw && result.stage == rdna4dmub::HandoffStage::FeatureCapsFailed &&
              result.fallbackVbios && hw.callOrder.back() == "restore",
              "reject uncleared ret_status, wrong command or wrong size even with valid FAMS bytes");
    }
    {
        SimDmubHw hw;
        hw.injectCapsFail = true;
        const auto result = rdna4dmub::execute_handoff(hw, true, 1u, inst.data(), (uint32_t)inst.size(),
                                                       0u, kArenaMc, kArenaSz, 0u, 4u, 4u);
        check(!result.usingDalFw && result.stage == rdna4dmub::HandoffStage::FeatureCapsFailed &&
              result.fallbackVbios && hw.callOrder.back() == "restore",
              "consume timeout invalidates an otherwise well-formed reply");
    }
    {
        SimDmubHw hw;
        hw.injectPreflightFail = true;
        const auto result = rdna4dmub::execute_handoff(hw, true, 1u, inst.data(), (uint32_t)inst.size(),
                                                       0u, kArenaMc, kArenaSz, 0u, 4u, 4u);
        check(!result.usingDalFw && result.fallbackVbios && !result.recoveryRequired &&
              result.stage == rdna4dmub::HandoffStage::VbiosPreflightFailed &&
              hw.callOrder == std::vector<std::string>{"prepare"} && hw.regs == hw.originalRegs,
              "lack of a verified VBIOS recovery path blocks every mutation");
    }
    {
        SimDmubHw hw;
        hw.injectDeadBootStatus = true;
        const auto result = rdna4dmub::execute_handoff(hw, true, 1u, inst.data(), (uint32_t)inst.size(),
                                                       0u, kArenaMc, kArenaSz, 0u, 4u, 4u);
        check(result.stage == rdna4dmub::HandoffStage::BootWaitTimeout && !result.usingDalFw &&
              result.fallbackVbios && hw.regs == hw.originalRegs,
              "all-ones SCRATCH0 from dead MMIO does not mean DAL booted");
    }
    {
        SimDmubHw hw;
        hw.injectWindowFail = true;
        const auto result = rdna4dmub::execute_handoff(hw, true, 1u, inst.data(), (uint32_t)inst.size(),
                                                       0u, kArenaMc, kArenaSz, 0u, 4u, 4u);
        check(result.stage == rdna4dmub::HandoffStage::WindowProgramFailed && result.fallbackVbios &&
              hw.callOrder == std::vector<std::string>{"prepare", "stop", "windows", "restore"},
              "program-window failure rolls back without sending PSP load");
    }
    {
        SimDmubHw hw;
        hw.injectPwaitTimeout = true;
        hw.injectRestoreFail = true;
        const auto result = rdna4dmub::execute_handoff(hw, true, 1u, inst.data(), (uint32_t)inst.size(),
                                                       0u, kArenaMc, kArenaSz, 0u, 4u, 4u);
        check(result.stage == rdna4dmub::HandoffStage::StopTimeout && result.recoveryRequired &&
              hw.regs[0x34c0u + DCN41_DMCUB_CNTL2] == 0u &&
              hw.regs[0x34c0u + DCN41_DMCUB_GPINT_DATAIN1] == 0u,
              "STOP acknowledgement without PWAIT cannot reset and clears pending GPINT");
    }
    std::printf("%d/%d checks passed\n", gRun - gFail, gRun);
    return gFail == 0 ? 0 : 1;
}
