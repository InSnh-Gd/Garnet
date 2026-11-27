#ifndef RDNA4_DCN41_DMUB_BOOT_PURE_H
#define RDNA4_DCN41_DMUB_BOOT_PURE_H

#include <stdint.h>
#include "../../../dcn41/dcn41_dmub.h"

namespace rdna4dmub {

inline bool dmcub_fw_opt_in(bool present, uint32_t arg) {
    return present && arg == 1u;
}

constexpr uint32_t kDmubStackSize       = 0x20000u;
constexpr uint32_t kDmubContextSize     = 0x80000u;
constexpr uint32_t kDmubVbiosWindowSize = 0x10000u; // 64 KiB
constexpr uint32_t kDmubMailboxSize     = DCN41_DMUB_RB_SIZE * 2u; // 8 KiB inbox1 + 8 KiB outbox1
constexpr uint32_t kDmubDefaultTraceSz  = 0x10000u;
constexpr uint32_t kDmubDefaultFwState  = 0x10000u;
constexpr uint32_t kDmubDefaultShared   = 0x1000u;
constexpr uint32_t kDmubScratchMemSize  = 0x1000u;
constexpr uint8_t kFams2Version21 = (2u << 5) | 1u;

inline uint32_t align_up_4k(uint32_t v) {
    return (v + 0xFFFu) & ~0xFFFu;
}

struct RegionLayout {
    bool valid { false };
    uint32_t totalBytes { 0 };
    uint32_t cw0Bytes { 0 };
    uint32_t bssBytes { 0 };
    uint64_t fams2IbGpuAddr { 0 };
    uint32_t fams2IbAllocBytes { 0 };
    uint64_t sharedStateGpuAddr { 0 };
    uint32_t sharedStateSize { 0 };
    uint64_t traceGpuAddr { 0 };
    uint32_t traceSize { 0 };
    uint64_t mailboxGpuAddr { 0 };
    uint32_t mailboxSize { 0 };
    dcn41_dmub_handoff_windows hw {};
};

// Compute contiguous 4 KiB-aligned VRAM/MC region layout mirroring upstream dmub_srv_calc_region_info().
// Addresses are MC addresses; this PSP path does not write code or stack window registers.
// Only reserve an IB if the caller already knows a negotiated protocol requires it. The resulting
// allocation size includes page padding and does not represent a firmware wire size. FAMS2.1 uses the ring only.
inline RegionLayout plan_region_layout(uint64_t baseMc, uint64_t capacityBytes,
                                       uint32_t instConstBytes, uint32_t bssDataBytes,
                                       const dcn41_dmub_fw_meta_info &meta,
                                       uint32_t vbiosBytes = kDmubVbiosWindowSize,
                                       uint32_t negotiatedIbBytes = 0u) {
    RegionLayout out {};
    constexpr uint32_t maxWindowBytes = 0x1000000u - 0x1000u;
    if (baseMc == 0ULL || (baseMc & 0xFFFull) != 0ULL || !instConstBytes || !vbiosBytes ||
        instConstBytes > maxWindowBytes || bssDataBytes != 0u || vbiosBytes > maxWindowBytes ||
        meta.trace_buffer_size > maxWindowBytes || meta.fw_region_size > maxWindowBytes ||
        meta.shared_state_size > maxWindowBytes || negotiatedIbBytes > UINT32_MAX - 0xFFFu)
        return out;

    const uint32_t traceSz = align_up_4k(meta.trace_buffer_size ? meta.trace_buffer_size : kDmubDefaultTraceSz);
    const uint32_t fwStateSz = align_up_4k(meta.fw_region_size ? meta.fw_region_size : kDmubDefaultFwState);
    const uint32_t sharedSz = align_up_4k(meta.shared_state_size >= kDmubDefaultShared ? meta.shared_state_size : kDmubDefaultShared);

    const uint32_t cw0Sz = align_up_4k(instConstBytes);
    const uint32_t cw1Sz = align_up_4k(kDmubStackSize + kDmubContextSize);
    const uint32_t cw2Sz = align_up_4k(bssDataBytes);
    const uint32_t cw3Sz = align_up_4k(vbiosBytes);
    const uint32_t cw4Sz = align_up_4k(kDmubMailboxSize);
    const uint32_t cw5Sz = traceSz;
    const uint32_t cw6Sz = fwStateSz;
    const uint32_t r6Sz  = sharedSz;
    const uint32_t ibSz  = align_up_4k(negotiatedIbBytes);
    const uint32_t scrSz = align_up_4k(kDmubScratchMemSize);

    const uint64_t total = static_cast<uint64_t>(cw0Sz) + cw1Sz + cw2Sz + cw3Sz +
                           cw4Sz + cw5Sz + cw6Sz + r6Sz + ibSz + scrSz;
    constexpr uint64_t addrLimit = 1ULL << 48;
    if (total > UINT32_MAX || total > capacityBytes || baseMc >= addrLimit || total > addrLimit - baseMc) return out;

    uint64_t cur = baseMc;
    auto take = [&](uint32_t sz) -> uint64_t {
        const uint64_t addr = cur;
        cur += sz;
        return addr;
    };

    out.cw0Bytes = cw0Sz;
    out.bssBytes = cw2Sz;
    out.hw.cw0 = { take(cw0Sz), DCN41_DMUB_CW0_BASE, DCN41_DMUB_CW0_BASE + cw0Sz - 1u };
    out.hw.cw1 = { take(cw1Sz), DCN41_DMUB_CW1_BASE, DCN41_DMUB_CW1_BASE + cw1Sz - 1u };
    const uint64_t cw2Addr = take(cw2Sz);
    out.hw.cw2 = { cw2Addr, DCN41_DMUB_CW0_BASE + cw0Sz,
                   cw2Sz ? (DCN41_DMUB_CW0_BASE + cw0Sz + cw2Sz) : (DCN41_DMUB_CW0_BASE + cw0Sz) };
    out.hw.cw3 = { take(cw3Sz), DCN41_DMUB_CW3_BASE, DCN41_DMUB_CW3_BASE + cw3Sz };

    out.mailboxGpuAddr = take(cw4Sz);
    out.mailboxSize = cw4Sz;
    out.hw.cw4 = { out.mailboxGpuAddr, DCN41_DMUB_CW4_BASE, DCN41_DMUB_CW4_BASE + cw4Sz };
    out.hw.inbox1_base  = DCN41_DMUB_CW4_BASE;
    out.hw.inbox1_size  = DCN41_DMUB_RB_SIZE;
    out.hw.outbox1_base = DCN41_DMUB_CW4_BASE + DCN41_DMUB_RB_SIZE;
    out.hw.outbox1_size = DCN41_DMUB_RB_SIZE;

    out.traceGpuAddr = take(cw5Sz);
    out.traceSize = cw5Sz;
    out.hw.cw5 = { out.traceGpuAddr, DCN41_DMUB_CW5_BASE, DCN41_DMUB_CW5_BASE + cw5Sz };
    out.hw.outbox0_base = DCN41_DMUB_OUTBOX0_TRACE_BASE;
    out.hw.outbox0_size = cw5Sz - DCN41_DMUB_TRACE_ENTRY_OFFSET;

    out.hw.cw6 = { take(cw6Sz), DCN41_DMUB_CW6_BASE, DCN41_DMUB_CW6_BASE + cw6Sz };
    out.sharedStateGpuAddr = take(r6Sz);
    out.sharedStateSize = r6Sz;
    out.hw.region6 = { out.sharedStateGpuAddr, DCN41_DMUB_CW6_BASE, DCN41_DMUB_CW6_BASE + r6Sz };

    if (ibSz != 0u) {
        out.fams2IbGpuAddr = take(ibSz);
        out.fams2IbAllocBytes = ibSz;
    }
    (void)take(scrSz);

    out.totalBytes = static_cast<uint32_t>(total);
    out.valid = true;
    return out;
}

enum class HandoffStage : uint8_t {
    DisabledByPolicy = 0,
    NoFirmware,
    InvalidMetadata,
    InvalidLayout,
    VbiosPreflightFailed,
    StopRefused,
    StopTimeout,
    StopFailed,
    WindowProgramFailed,
    PspLoadFailed,
    BootWaitTimeout,
    FeatureCapsFailed,
    ActiveDalFw,
};

struct HandoffResult {
    HandoffStage stage { HandoffStage::DisabledByPolicy };
    bool usingDalFw { false };
    bool fallbackVbios { true };
    bool recoveryRequired { false };
    uint32_t fwVersion { 0 };
    uint8_t fams2Version { 0 };
    dcn41_dmub_feature_caps caps {};
    RegionLayout layout {};
};

// Pure fail-closed VBIOS -> DAL DMUB handoff coordinator.
// sosFwVersion is the PSP SOS version (MP0 C2PMSG_58), not the version from the signed DAL image.
// Ops contract:
//   bool prepare_handoff(const RegionLayout &layout)
//   int  stop_and_reset(uint32_t timeout_us) (DCN41_E_STATE certifies no writes)
//   int  program_windows(const dcn41_dmub_handoff_windows &w)
//   bool psp_load_dmub_fw()
//   int  release_and_wait(uint32_t sos_fw_version, uint32_t timeout_us)
//   bool submit_and_read_reply(dcn41_dmub_cmd &cmd, uint32_t timeout_us)
//        Submit the command, wait until consumed, and read back the same inbox slot.
//   bool restore_vbios(uint32_t timeout_us)
template <class Ops>
inline HandoffResult execute_handoff(Ops &ops, bool bootArgPresent, uint32_t bootArgVal,
                                     const uint8_t *signedPayload, uint32_t signedPayloadBytes,
                                     uint32_t bssDataBytes, uint64_t arenaMc, uint64_t arenaBytes,
                                     uint32_t sosFwVersion, uint32_t stopTimeoutUs = 1000000u,
                                     uint32_t bootTimeoutUs = 1000000u,
                                     uint32_t vbiosBytes = kDmubVbiosWindowSize) {
    HandoffResult res {};
    if (!dmcub_fw_opt_in(bootArgPresent, bootArgVal)) {
        res.stage = HandoffStage::DisabledByPolicy;
        return res;
    }
    if (!signedPayload || signedPayloadBytes == 0u) {
        res.stage = HandoffStage::NoFirmware;
        return res;
    }
    dcn41_dmub_fw_meta_info meta {};
    uint32_t codeBytes = 0;
    if (dcn41_dmub_fw_meta_from_psp(signedPayload, signedPayloadBytes, &meta, &codeBytes) != DCN41_OK ||
        meta.dal_fw != 1u) {
        res.stage = HandoffStage::InvalidMetadata;
        return res;
    }
    res.fwVersion = meta.fw_version;
    res.layout = plan_region_layout(arenaMc, arenaBytes, codeBytes, bssDataBytes, meta, vbiosBytes);
    if (!res.layout.valid) {
        res.stage = HandoffStage::InvalidLayout;
        return res;
    }
    if (!ops.prepare_handoff(res.layout)) {
        res.stage = HandoffStage::VbiosPreflightFailed;
        return res;
    }
    const auto fail = [&](HandoffStage stage) {
        res.stage = stage;
        res.fallbackVbios = ops.restore_vbios(bootTimeoutUs);
        res.recoveryRequired = !res.fallbackVbios;
        return res;
    };
    const int stopResult = ops.stop_and_reset(stopTimeoutUs);
    if (stopResult == DCN41_E_STATE) {
        res.stage = HandoffStage::StopRefused;
        return res;
    }
    if (stopResult != DCN41_OK) {
        return fail(stopResult == DCN41_E_TIMEOUT ? HandoffStage::StopTimeout : HandoffStage::StopFailed);
    }
    if (ops.program_windows(res.layout.hw) != DCN41_OK) {
        return fail(HandoffStage::WindowProgramFailed);
    }
    if (!ops.psp_load_dmub_fw()) {
        return fail(HandoffStage::PspLoadFailed);
    }
    if (ops.release_and_wait(sosFwVersion, bootTimeoutUs) != DCN41_OK) {
        return fail(HandoffStage::BootWaitTimeout);
    }
    dcn41_dmub_cmd query {};
    dcn41_dmub_build_query_feature_caps(&query);
    const uint32_t replyHeader = query.header & ~DCN41_DMUB_HDR_RET_STATUS_MASK;
    if (!ops.submit_and_read_reply(query, bootTimeoutUs) || query.header != replyHeader ||
        query.payload[offsetof(dcn41_dmub_feature_caps, fw_assisted_mclk_switch_ver)] != kFams2Version21) {
        return fail(HandoffStage::FeatureCapsFailed);
    }
    for (uint32_t index = 0; index < sizeof(res.caps); ++index)
        reinterpret_cast<uint8_t *>(&res.caps)[index] = query.payload[index];
    res.fams2Version = res.caps.fw_assisted_mclk_switch_ver;
    res.usingDalFw = true;
    res.fallbackVbios = false;
    res.stage = HandoffStage::ActiveDalFw;
    return res;
}

} // namespace rdna4dmub

#endif /* RDNA4_DCN41_DMUB_BOOT_PURE_H */
