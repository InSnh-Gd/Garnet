// PORTED from lemonade-sdk/mac-amdgpu (MIT) dext/amdgpu/amdgpu_smu.h @ 3bdeed2.
// No longer verbatim — see "Deviations from reference" in src/amd/smu_v14_0.cpp
// and the marked blocks below. Keep the reference's names/semantics.
//
//  amdgpu_smu.h — SMU v14_0_3 mailbox interface.
//
//  Ports the SMU mailbox primitives from upstream Linux:
//      drivers/gpu/drm/amd/pm/swsmu/smu_cmn.c
//        smu_cmn_wait_for_response
//        smu_cmn_send_smc_msg_with_param
//        smu_cmn_send_smc_msg
//
//  Pre-requisites that must be true before SMU mailbox is usable:
//      - PSP SOS loaded (psp_load_sos succeeded)
//      - PSP km_ring created (psp_ring_create succeeded)
//      - PMFW (smu_14_0_3.bin) loaded through the PSP by the PSPFwLoad
//        stage — this module assumes that already happened.
//
//  Until PMFW is loaded the SMU silicon doesn't respond to messages.
//  smu_test_message() returns kIOReturnNotReady if MP1 is unresolved
//  and a timeout if SMU never answers.
//
//  Mailbox registers (MP1 BASE_IDX 1 — NOT 0; base 0 routes the writes
//  to a different physical register and the SMU never answers):
//      msg   = regMP1_SMN_C2PMSG_66  (dword 0x0082)
//      param = regMP1_SMN_C2PMSG_82  (dword 0x0092)
//      resp  = regMP1_SMN_C2PMSG_90  (dword 0x009A)
//  Response codes: 1 OK, 0xFF failed, 0xFE unknown, 0xFD prereq, 0xFC busy
//  (amdgpu::SMUResp in amdgpu_ip.h). Identical to the reference.
//

#pragma once

#include <stdint.h>
#include "amdgpu_regs.h"
#include "garnet_gfxoff_pure.h"
#include "garnet_power_policy_pure.h"

namespace amdgpu {

//
// smu_send_msg_with_param — port of smu_cmn_send_smc_msg_with_param.
// Synchronous. Returns kIOReturnSuccess if SMU responded with OK
// (0x01), kIOReturnDeviceError for other non-zero responses, and
// kIOReturnTimeout if the SMU never answered.
//
//   msgId      — PPSMC message id (PPSMC::TestMessage, etc.)
//   param      — message parameter; 0 for messages that don't use it
//   outReturn  — out param for SMU's reply payload (read from C2PMSG_82
//                after the response is received); nullable
//
kern_return_t smu_send_msg_with_param(const DeviceContext &dev,
                                      uint32_t msgId,
                                      uint32_t param,
                                      uint32_t *outReturn);

//
// smu_send_msg — convenience wrapper, no parameter, no return read.
//
kern_return_t smu_send_msg(const DeviceContext &dev, uint32_t msgId);

// 0.0.604 (shared SMU mailbox lock): the ONE lock shared by every PPSMC message (smu_send_msg_with_param takes it itself), the
// DAL sender (smu_dal.cpp) and any caller that needs several messages back to back. Recursive for the owning thread. smu_lock_enter()
// returns false only when the lock could not be allocated (nothing is held then; smu_lock_exit must not be called).
bool smu_lock_enter();
void smu_lock_exit();
struct SmuSeq {
    bool held;
    SmuSeq() : held(smu_lock_enter()) {}
    ~SmuSeq() { if (held) smu_lock_exit(); }
    SmuSeq(const SmuSeq &) = delete;
    SmuSeq &operator=(const SmuSeq &) = delete;
};

//
// smu_test_message — port of the basic ping. PPSMC::TestMessage with
// param=0xABCD0001; SMU echoes back param+1. Used to verify the
// mailbox is live before any other SMU traffic.
//
kern_return_t smu_test_message(const DeviceContext &dev,
                               uint32_t *outEcho);

//
// smu_get_version — read SMU PMFW version number.
//
kern_return_t smu_get_version(const DeviceContext &dev, uint32_t *outVer);

// [ADDED here] smu_get_driver_if_version — PPSMC::GetDriverIfVersion.
// The reference inlines this message inside smu_smc_hw_setup; broken
// out so a caller can sanity-check the SMC interface version without
// touching clocks or power. Same message, same param, same traffic.
kern_return_t smu_get_driver_if_version(const DeviceContext &dev,
                                        uint32_t *outVer);

//
// PPSMC IDs for the driver-table transfer protocol. We use these
// to point SMU at a DRAM-resident copy of the SMU driver table so
// the SMU can DMA tool/config tables in and out of our sysmem.
// Subset of smu_v14_0_2_ppsmc.h — full file is per-ASIC.
//
namespace PPSMCTable {
    constexpr uint32_t SetDriverDramAddrHigh = 0x0E;
    constexpr uint32_t SetDriverDramAddrLow  = 0x0F;
    constexpr uint32_t SetToolsDramAddrHigh  = 0x10;
    constexpr uint32_t SetToolsDramAddrLow   = 0x11;
    constexpr uint32_t TransferTableSmu2Dram = 0x12;
    constexpr uint32_t TransferTableDram2Smu = 0x13;
}

// PSP firmware type id for the pptable blob (GFX_FW_TYPE_PPTABLE).
constexpr uint32_t kPSP_FW_TYPE_PPTABLE = 73;

//
// smu_set_driver_dram_addr — point SMU at a DRAM-resident driver
// table buffer. Caller owns a single buffer (e.g. 64 KB, 16 KB-aligned)
// and passes the address the SMU should use.
//
// On the reference (Apple Silicon / Thunderbolt) that is the bus
// address of a DART-mapped IOBufferMemoryDescriptor. Here it is
// whichever address space the caller chose:
//   * VRAM  — an MC address, dev.vramMC(dev.vramBase + offset)
//   * sysmem — amdgpu::SysMem::bus (== physical; no IOMMU on this box)
// smu_smc_hw_setup() below uses VRAM, exactly like the reference.
//
// This is the equivalent of upstream's amdgpu_table_setup flow for
// smu14: see smu_v14_0.c smu_v14_0_set_tool_table_location +
// the driver_pptable / driver_table allocations in init_smc_tables.
// We don't allocate the upstream zoo of tables; we expose the
// primitive and let a future port chunk decide which tables to
// upload via TransferTableDram2Smu.
//
kern_return_t smu_set_driver_dram_addr(const DeviceContext &dev,
                                       uint64_t bus_addr);

// Same shape for the SMU tool (telemetry) DRAM area.
kern_return_t smu_set_tools_dram_addr(const DeviceContext &dev,
                                      uint64_t bus_addr);

//
// smu_transfer_table_dram_to_smu — instruct SMU to consume the
// driver table at the current DRAM addr. `table_id` is the SMU's
// internal table index (asic-specific subset of `SMU_TABLE_*`).
//
kern_return_t smu_transfer_table_dram_to_smu(const DeviceContext &dev,
                                             uint32_t table_id);

//
// smu_transfer_table_smu_to_dram — opposite direction. SMU writes
// the current table contents into our DRAM buffer (telemetry
// snapshots, OD readback, etc.).
//
kern_return_t smu_transfer_table_smu_to_dram(const DeviceContext &dev,
                                             uint32_t table_id);

// ---------------------------------------------------------------------
// SMU driver_table staging area (VRAM).
//
// The reference carves 64 KB out of PSPContext::fwBuf with the PSP
// module's bump allocator. GarnetDriver has no PSPContext yet (the
// PSP module is a separate in-flight file) and DeviceContext
// carries no VRAMBumpAllocator, so the table lives at a FIXED byte
// offset from dev.vramBase — the first 64 KB of the reserved VRAM region
// reserves for the bump allocator:
//
//   dev.vramBase + 0x0E00000 .. + 0x0E10000   (64 KB, = +14 MiB)
//
// That is immediately above fw_buf (+0x1000000, 8 MiB) so it collides
// with nothing in the documented fixed layout. It DOES overlap the
// start of the future VRAMBumpAllocator arena: whoever wires that
// allocator up must either reserve the first 64 KB or pass an explicit
// MC address to smu_smc_hw_setup().
// ---------------------------------------------------------------------
constexpr uint64_t kSMUDriverTableVRAMOffset = 0x0E00000;   // from dev.vramBase
constexpr uint64_t kSMUDriverTableSize       = 0x10000;     // 64 KB

// MC address of the fixed driver_table slot (0 if the context has no
// VRAM geometry or the slot falls outside the mapped aperture).
uint64_t smu_driver_table_mc(const DeviceContext &dev);

//
// smu_smc_hw_setup — minimal port of upstream `smu_smc_hw_setup`
// (amdgpu_smu.c:1662) for smu_v14_0_3 / Navi 48.
//
// Mirrors the SMU PMFW handshake that runs between PSP fw_load and the
// BOOTLOAD_STATUS poll in upstream's IP-block init. The reference's
// v0.1.18-0.1.19 confirmed that PSP accepts every command (LOAD_IP_FW +
// AUTOLOAD_RLC + LOAD_ASD all resp=0) but `regRLC_RLCS_BOOTLOAD_STATUS`
// stays at 0. Hypothesis: PMFW must run `EnableAllSmuFeatures` to bring
// DPM / GFX clocks online before the IMU autoload state machine can
// complete.
//
// Message order (unchanged from the reference):
//   1. GetDriverIfVersion         — sanity-check SMC IF version (fatal).
//   2. SetDriverDramAddrHigh+Low  — point SMU at the 64 KB VRAM-resident
//                                   driver_table (fatal).
//   3. UseDefaultPPTable          — IFWI default powerplay table (best effort).
//   4. RunDcBtc                   — boot-time calibration (fatal).
//   5. NotifyPowerSource(AC)      — best effort.
//   6. SetAllowedFeaturesMaskHigh — 0xFFFFFFFF (best effort).
//      SetAllowedFeaturesMaskLow  — only after High succeeds.
//   7. EnableAllSmuFeatures       — the master DPM switch (logged, not fatal).
//   8. GetRunningSmuFeaturesLow   — diagnostics only.
//      GetRunningSmuFeaturesHigh
//
// What this does NOT port (same list as the reference):
//   - pptable upload (TransferTableDram2Smu) — relies on the PMFW/IFWI
//     default pptable (step 3).
//   - SetDefaultDpmTable / SetMinDeepSleepDcefclk — informational.
//   - Thermal alert + display change notifications — non-critical.
//   - Max sustainable clocks query — informational.
//
// Pre-conditions:
//   - PSP fw_load complete (SMU PMFW running, responds to TestMessage)
//   - dev.vramMcBase / vramBase / vramLimit valid when driver_table_mc == 0
//
// *** DANGER ***  Steps 3-7 change power state on a live card. On this
// box the firmware is still scanning the UEFI console framebuffer out of
// VRAM while this runs; enabling DPM can move GFX/SOC clocks and (on the
// reference hardware) pins the fan to maximum until DPM is parked again
// (smu_disable_all_features). Only call this when the operator is at the
// machine.
//
//   driver_table_mc — address the SMU should use for the driver table.
//                     0 (default) = the fixed VRAM slot above, which is
//                     also zeroed before the address is handed to PMFW.
//
// [DEVIATION] the reference takes `PSPContext &psp` here and bump-
// allocates out of psp.fwBuf. See the VRAM note above.
//
kern_return_t smu_smc_hw_setup(DeviceContext &dev,
                               uint64_t driver_table_mc = 0);

// [ADDED here] smu_hw_init — the SMU IP-block hw_init, i.e. the body of
// `case BringupStage::SMUInit` in the reference's amdgpu_init.cpp:785.
// That file is not ported yet, so the stage lives here:
//     1. smu_test_message()            — mailbox ping (fatal)
//     2. smu_get_version()             — PMFW version (logged)
//     3. smu_get_driver_if_version()   — SMC IF version (logged)
//     4. dev.smuOnline = true
//     5. smu_smc_hw_setup()            — only when runSmcHwSetup
//
// Steps 1-3 are read-only as far as the GPU's power state is concerned
// (they only poke the mailbox, exactly like GarnetDriver::surveySMU).
// Step 5 changes clocks/power — see the DANGER note above, which is why
// `runSmcHwSetup` has no default: the integrator must choose explicitly.
// A failure inside smc_hw_setup is logged and NOT propagated, matching
// the reference's SMUInit stage.
kern_return_t smu_hw_init(DeviceContext &dev, bool runSmcHwSetup);

// [ADDED here] Feature control + clock clamps. These live in the
// reference's user-client (dext/MacAMDGPU.cpp, selectors 33 and 40)
// rather than in smu_v14_0.cpp; this kext has no user client, so the
// PMFW-facing halves are ported here unchanged.

// PPSMC::DisableAllSmuFeatures — park DPM. The reference uses this to
// drop the fan after EnableAllSmuFeatures pins it to max (no chip-
// specific fan curve is uploaded). Reverse by re-running smc_hw_setup.
kern_return_t smu_disable_all_features(const DeviceContext &dev);

// PPSMC::SetSoftMax/SetSoftMinByFreq on one PPCLK domain.
// param = (clk_id << 16) | freq_mhz, per upstream
// smu_v14_0_set_soft_freq_limited_range (smu_v14_0.c:1099).
// max_mhz == 0 skips the max side (reference semantics: "leave max
// alone"); min is always sent, because 0 there is the meaningful
// "unclamp the lower bound". 0xFFFF means "PMFW picks". Max is sent
// before min, as upstream does.
kern_return_t smu_set_soft_freq_range(const DeviceContext &dev,
                                      uint32_t clk_id,
                                      uint32_t min_mhz,
                                      uint32_t max_mhz);

// Coarse power states, values and GFXCLK clamp pairs copied from the
// reference's kMacAMDGPUPowerState* handler (MacAMDGPU.cpp:1608).
namespace SMUPowerState {
    constexpr uint32_t Auto    = 0;   // min 0,    max PMFW-pick
    constexpr uint32_t Low     = 1;   // min 0,    max 200 MHz
    constexpr uint32_t Nominal = 2;   // same as Auto
    constexpr uint32_t High    = 3;   // min 1500 MHz, max untouched
    constexpr uint32_t Peak    = 4;   // min = max = 2400 MHz
}

// *** DANGER *** moves real GFX clocks. See smu_smc_hw_setup's note.
kern_return_t smu_set_power_state(const DeviceContext &dev, uint32_t state);

// Stage 3C: UCLK hard-min control (0 = unpin hard+soft min; >0 = pin floor)
// and opt-in SMU GFXOFF (PPSMC::AllowGfxOff / DisallowGfxOff).
kern_return_t smu_set_uclk_hard_min(const DeviceContext &dev, uint32_t mhz);
kern_return_t smu_pin_uclk_top(const DeviceContext &dev, uint32_t *outTopMhz = nullptr);
kern_return_t smu_set_gfx_off(const DeviceContext &dev, bool allow);

// Per-driver SMU runtime state: refcounted GC MMIO GFXOFF guard with
// ~100 ms delayed-idle AllowGfxOff hysteresis and activity-driven GFXCLK idle DPM governor.
enum class DisplayWakePhase : uint8_t { Idle, Sleeping, Waking, PoweredOn };
struct SMUContext {
    rdna4gfxoff::GuardState   gfxoff {};
    rdna4pwr::IdleDpmGovernor idleDpm {};
    const DeviceContext      *gfxoffDev   { nullptr };
    void                     *gfxoffTimer { nullptr }; // thread_call_t in kernel
    bool                      inited      { false };
    bool                      powerEpochManaged { false };
    bool                      sleepPending { false };
    bool                      wakeUnverified { false };
    const DeviceContext      *displayWakeDev { nullptr };
    DisplayWakePhase          displayWakePhase { DisplayWakePhase::Idle };
};

// Refcounted GC MMIO GFXOFF guard.
kern_return_t gfx_off_ctrl(const DeviceContext &dev, SMUContext &smu, bool allow);
kern_return_t gfx_off_boot_init(const DeviceContext &dev, SMUContext &smu,
                                bool bootArgPresent, uint32_t bootArgValue,
                                bool *outAllowed = nullptr);
kern_return_t gfx_off_teardown(const DeviceContext &dev, SMUContext &smu);
void          gfx_off_register_context(SMUContext *smu);
enum class GfxOffPowerEvent : uint8_t { WillSleep, WillNotSleep, WillPowerOn, HasPoweredOn, Shutdown };
kern_return_t gfx_off_power_event(DeviceContext &dev, SMUContext &smu, GfxOffPowerEvent event);
bool smu_display_wake_pin_allowed(const SMUContext &smu);
kern_return_t smu_repin_uclk_after_wake(const DeviceContext &dev, SMUContext &smu, uint32_t expectedVersion);
SMUContext   *gfx_off_active_context();

// Balanced RAII guard for GC MMIO critical sections.
struct ScopedGfxOffGuard {
    const DeviceContext *dev;
    SMUContext          *ctx;
    kern_return_t        kr;
    bool                 held;

    explicit ScopedGfxOffGuard(const DeviceContext &d,
                               SMUContext *c = gfx_off_active_context())
        : dev(&d), ctx(c), kr(kIOReturnSuccess), held(false) {
        if (ctx != nullptr) {
            kr = gfx_off_ctrl(*dev, *ctx, false);
            held = (kr == kIOReturnSuccess);
        }
    }
    ~ScopedGfxOffGuard() {
        if (held && ctx != nullptr) {
            (void)gfx_off_ctrl(*dev, *ctx, true);
        }
    }
    bool ok() const { return kr == kIOReturnSuccess; }
    kern_return_t status() const { return kr; }
    ScopedGfxOffGuard(const ScopedGfxOffGuard &) = delete;
    ScopedGfxOffGuard &operator=(const ScopedGfxOffGuard &) = delete;
};

// Activity-driven SMU GFXCLK idle governor (`garnet-idle-dpm`).
// Serialized via SmuSeq on a dedicated IOWorkLoop/IOTimerEventSource (never
// in IRQ context, never holding GPU ring locks across SMU mailbox I/O).
using IdleDpmBusyFn = bool (*)(void *);
void smu_idle_dpm_start(DeviceContext &dev, SMUContext &smu,
                        bool dpmEnabled, bool idleClamp,
                        IdleDpmBusyFn hwBusyFn, void *hwBusyCtx);
void smu_idle_dpm_stop();
// Notify governor of GFX/KCQ/SDMA submission activity.
// `canBlock == true` boosts Low -> Auto synchronously (caller must hold no GPU
// ring/scanout lock); `canBlock == false` updates timestamp lock-free and kicks
// the governor workloop asynchronously when currently in Low.
void smu_idle_dpm_note_activity(bool canBlock, bool heavy = false);
// Lock out automatic idle governance after manual `garnettest power <state>`.
void smu_idle_dpm_manual_override(uint32_t state);

//=====================================================================
//  Telemetry — the SMU metrics table
//=====================================================================
//
// PMFW keeps a metrics snapshot internally and DMAs it into the driver
// table on request: TransferTableSmu2Dram with table id TABLE_SMU_METRICS.
// Our driver table already lives in VRAM at kSMUDriverTableVRAMOffset and
// smu_smc_hw_setup has already told the SMU where it is, so reading
// telemetry is one message plus a copy back.
//
// THE LAYOUT BELOW IS A BINARY CONTRACT WITH THE FIRMWARE. It mirrors
// SmuMetrics_t from smu14_driver_if_v14_0.h, which is versioned: the ppt
// driver pins SMU14_DRIVER_IF_VERSION_SMU_V14_0_2 = 0x2E. If the PMFW on
// the card reports a different interface version, every field below is at
// the wrong offset and the numbers are nonsense — so smu_read_metrics
// checks the version first and refuses rather than reporting garbage.
constexpr uint32_t kSMUTableMetrics        = 5;      // TABLE_SMU_METRICS
constexpr uint32_t kSMUDriverIfVersion_14_0_2 = 0x2E;
// IF 0x33 (RX 9060 XT / Navi44 PMFW) verified empirically against raw[00..31]:
// all SmuMetrics offsets match 0x2E (dGPU_W_MAX, FCLK, UCLK, temps, power, activity).
constexpr uint32_t kSMUDriverIfVersion_14_0_2_MaxVerified = 0x33;
constexpr uint32_t kSMUPpClkCount   = 11;   // PPCLK_COUNT
constexpr uint32_t kSMUSviPlaneCount = 4;   // SVI_PLANE_COUNT
constexpr uint32_t kSMUTempCount    = 12;   // TEMP_COUNT
constexpr uint32_t kSMUThrottlerCount = 21; // THROTTLER_COUNT
constexpr uint32_t kSMUD3HotCount   = 4;    // D3HOT_SEQUENCE_COUNT

// PPCLK_e indices into CurrClock[].
namespace SMUClk {
    constexpr uint32_t GFXCLK = 0, SOCCLK = 1, UCLK = 2, FCLK = 3,
                       DCLK_0 = 4, VCLK_0 = 5, DISPCLK = 6, DPPCLK = 7,
                       DPREFCLK = 8, DCFCLK = 9, DTBCLK = 10;
}
// TEMP_e indices into AvgTemperature[].
namespace SMUTemp {
    constexpr uint32_t EDGE = 0, HOTSPOT = 1, HOTSPOT_GFX = 2, HOTSPOT_SOC = 3,
                       MEM = 4, VR_GFX = 5, VR_SOC = 6, VR_MEM0 = 7,
                       VR_MEM1 = 8, LIQUID0 = 9, LIQUID1 = 10, PLX = 11;
}

struct SmuMetrics {
    uint32_t CurrClock[kSMUPpClkCount];
    uint16_t AverageGfxclkFrequencyTarget;
    uint16_t AverageGfxclkFrequencyPreDs;
    uint16_t AverageGfxclkFrequencyPostDs;
    uint16_t AverageFclkFrequencyPreDs;
    uint16_t AverageFclkFrequencyPostDs;
    uint16_t AverageMemclkFrequencyPreDs;
    uint16_t AverageMemclkFrequencyPostDs;
    uint16_t AverageVclk0Frequency;
    uint16_t AverageDclk0Frequency;
    uint16_t AverageVclk1Frequency;
    uint16_t AverageDclk1Frequency;
    uint16_t AveragePCIeBusy;
    uint16_t dGPU_W_MAX;
    uint16_t padding;
    uint16_t MovingAverage[16];        // Target..Padding, see the header
    uint32_t MetricsCounter;
    uint16_t AvgVoltage[kSMUSviPlaneCount];
    uint16_t AvgCurrent[kSMUSviPlaneCount];
    uint16_t AverageGfxActivity;
    uint16_t AverageUclkActivity;
    uint16_t AverageVcn0ActivityPercentage;
    uint16_t Vcn1ActivityPercentage;
    uint32_t EnergyAccumulator;
    uint16_t AverageSocketPower;
    uint16_t AverageTotalBoardPower;
    uint16_t AvgTemperature[kSMUTempCount];
    uint16_t AvgTemperatureFanIntake;
    uint8_t  PcieRate;
    uint8_t  PcieWidth;
    uint8_t  AvgFanPwm;
    uint8_t  Padding[1];
    uint16_t AvgFanRpm;
    uint8_t  ThrottlingPercentage[kSMUThrottlerCount];
    uint8_t  VmaxThrottlingPercentage;
    uint8_t  padding1[2];
    uint32_t D3HotEntryCountPerMode[kSMUD3HotCount];
    uint32_t D3HotExitCountPerMode[kSMUD3HotCount];
    uint32_t ArmMsgReceivedCountPerMode[kSMUD3HotCount];
    uint16_t ApuSTAPMSmartShiftLimit;
    uint16_t ApuSTAPMLimit;
    uint16_t AvgApuSocketPower;
    uint16_t AverageUclkActivity_MAX;
    uint32_t PublicSerialNumberLower;
    uint32_t PublicSerialNumberUpper;
};
static_assert(sizeof(SmuMetrics) == 260, "SmuMetrics must match SmuMetrics_t");

// Upstream smu_v14_0_2_ppt.c pins 0x2E; RX 9060 XT PMFW reports 0x33 with the
// exact same 260-byte SmuMetrics layout (verified empirically on hardware).
constexpr bool smu_driver_if_verified(uint32_t if_ver) {
    return if_ver >= kSMUDriverIfVersion_14_0_2 &&
           if_ver <= kSMUDriverIfVersion_14_0_2_MaxVerified;
}

// Effective GFXCLK in MHz, mirroring upstream smu_v14_0_2_ppt.c: PMFW leaves
// CurrClock[GFXCLK] fixed at 1000 MHz on IF 0x33, while AverageGfxclkFrequency
// PostDs (idle / <= 5% busy) and PreDs (active) carry the live DPM clock.
constexpr uint32_t smu_effective_gfxclk_mhz(const SmuMetrics &m) {
    const uint32_t primary  = (m.AverageGfxActivity <= 5u)
                                  ? (uint32_t)m.AverageGfxclkFrequencyPostDs
                                  : (uint32_t)m.AverageGfxclkFrequencyPreDs;
    if (primary != 0u) return primary;
    const uint32_t fallback = (m.AverageGfxActivity <= 5u)
                                  ? (uint32_t)m.AverageGfxclkFrequencyPreDs
                                  : (uint32_t)m.AverageGfxclkFrequencyPostDs;
    if (fallback != 0u) return fallback;
    return m.CurrClock[SMUClk::GFXCLK];
}

constexpr bool smu_metrics_plausible(const SmuMetrics &m) {
    const uint32_t gfx  = smu_effective_gfxclk_mhz(m);
    const uint32_t mem  = m.CurrClock[SMUClk::UCLK];
    const uint32_t edge = m.AvgTemperature[SMUTemp::EDGE];
    const uint32_t hot  = m.AvgTemperature[SMUTemp::HOTSPOT];
    return gfx <= 5000u && mem <= 5000u &&
           edge <= 125u && hot <= 125u && hot >= edge &&
           m.AverageGfxActivity <= 100u && m.AverageUclkActivity <= 100u;
}

// smu_read_metrics checks the IF version against the verified range (0x2E..0x33)
// and range-checks decoded values so unknown newer PMFW versions stay flagged.
struct SmuMetricsResult {
    SmuMetrics metrics;
    uint32_t   if_version;        // what PMFW reported
    bool       layout_verified;   // smu_driver_if_verified(if_version)
    bool       values_plausible;  // smu_metrics_plausible(metrics)
};

// Ask PMFW for a fresh snapshot and copy it out of the driver table.
kern_return_t smu_read_metrics(const DeviceContext &dev, SmuMetricsResult *out);

// Summary into the driver log, including the layout caveat and a raw hex dump
// of the first 128 bytes so the offsets can be checked by eye.
void smu_log_metrics(const SmuMetricsResult &r);

} // namespace amdgpu
