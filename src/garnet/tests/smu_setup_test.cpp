#include "amdgpu_smu.h"
#include <cstdio>
#include <vector>

constexpr kern_return_t kIOReturnNoResources = static_cast<kern_return_t>(0xe00002beu);

namespace {
struct Message { uint32_t id, param; };
std::vector<Message> messages;
bool high_written;
bool reject_high;
bool reject_hardmin;
bool reject_softmin;
bool reject_enable;
bool bad_uclk_freq;
bool fine_uclk;
unsigned failures;
void expect(bool value, const char *description) {
    if (!value) { ++failures; std::printf("FAIL: %s\n", description); }
}
}

namespace amdgpu {
bool smu_lock_enter() { return true; }
void smu_lock_exit() {}
static kern_return_t uclk_wait_pending_reply(const DeviceContext &) { return kIOReturnSuccess; }
uint64_t smu_driver_table_mc(const DeviceContext &) { return 0; }
kern_return_t smu_get_driver_if_version(const DeviceContext &, uint32_t *value) {
    *value = 0x33;
    return kIOReturnSuccess;
}
kern_return_t smu_send_msg_with_param(const DeviceContext &, uint32_t id,
                                     uint32_t param, uint32_t *value) {
    messages.push_back({id, param});
    if (value) *value = 0;
    if (id == PPSMC::NotifyPowerSource && param != 0) return kIOReturnError;
    if (id == PPSMC::SetAllowedFeaturesMaskHigh) {
        if (reject_high) return kIOReturnUnsupported;
        high_written = true;
    }
    if (id == PPSMC::SetAllowedFeaturesMaskLow && !high_written)
        return kIOReturnNotReady;
    if (id == PPSMC::EnableAllSmuFeatures && reject_enable)
        return kIOReturnUnsupported;
    if (id == PPSMC::GetDpmFreqByIndex && value) {
        const uint32_t clk = param >> 16;
        const uint32_t idx = param & 0xFFFFu;
        if (clk == PPCLK::UCLK) {
            if (fine_uclk) {
                if (idx == 0xFFu) *value = 0x80000002u;
                else if (idx == 1u) *value = bad_uclk_freq ? 0u : 0x800004eau;
                else return kIOReturnError;
            } else {
                if (idx == 0xFFu) *value = 6u;
                else if (idx == 5u) *value = bad_uclk_freq ? 0u : 1258u;
                else return kIOReturnError;
            }
        } else {
            return kIOReturnError;
        }
    }
    if (id == PPSMC::SetHardMinByFreq && reject_hardmin)
        return kIOReturnUnsupported;
    if (id == PPSMC::SetSoftMinByFreq && reject_softmin)
        return kIOReturnUnsupported;
    return kIOReturnSuccess;
}
kern_return_t smu_send_msg(const DeviceContext &dev, uint32_t id) {
    return smu_send_msg_with_param(dev, id, 0, nullptr);
}
#define SMU_LOG(...) ((void)0)
#include "smu_setup_actual.inc"
#undef SMU_LOG
}

int main() {
    amdgpu::DeviceContext dev {};
    for (bool unsupported : {false, true}) {
        reject_high = unsupported;
        reject_hardmin = false;
        reject_enable = false;
        bad_uclk_freq = false;
        fine_uclk = unsupported;
        high_written = false;
        messages.clear();
        expect(amdgpu::smu_smc_hw_setup(dev, 0x8002e00000) == kIOReturnSuccess,
               "setup completes with explicit feature-mask fallback");
        size_t hi = messages.size(), lo = messages.size();
        unsigned ac = 0, enable = 0, hard_uclk = 0, hard_fclk = 0;
        for (size_t i = 0; i < messages.size(); ++i) {
            const auto &m = messages[i];
            if (m.id == amdgpu::PPSMC::NotifyPowerSource) {
                ++ac;
                expect(m.param == 0, "SMU 14 AC power source is zero, not DC");
            }
            if (m.id == amdgpu::PPSMC::SetAllowedFeaturesMaskHigh) hi = i;
            if (m.id == amdgpu::PPSMC::SetAllowedFeaturesMaskLow) lo = i;
            if (m.id == amdgpu::PPSMC::EnableAllSmuFeatures) ++enable;
            if (m.id == amdgpu::PPSMC::SetHardMinByFreq) {
                if (m.param == ((amdgpu::PPCLK::UCLK << 16) | 1258u)) ++hard_uclk;
                if ((m.param >> 16) == amdgpu::PPCLK::FCLK) ++hard_fclk;
            }
        }
        expect(ac == 1 && enable == 1, "power source and master enable sent once");
        expect(hi < messages.size(), "high feature word is sent");
        if (unsupported)
            expect(lo == messages.size(), "high-word rejection does not send an invalid low-word commit");
        else
            expect(hi < lo && lo < messages.size(), "high feature word precedes low-word commit");
        expect(hard_uclk == 1, "UCLK table pins top level (1258 MHz) via SetHardMinByFreq");
        expect(hard_fclk == 0, "FCLK is left unpinned so PMFW keeps low idle SOCCLK/power");
    }

    // Fallback to SetSoftMinByFreq when SetHardMinByFreq is rejected.
    reject_high = false; reject_hardmin = true; reject_enable = false; bad_uclk_freq = false; fine_uclk = false; high_written = false;
    messages.clear();
    expect(amdgpu::smu_smc_hw_setup(dev, 0x8002e00000) == kIOReturnSuccess, "soft-min fallback setup completes");
    unsigned soft_uclk = 0;
    for (const auto &m : messages)
        if (m.id == amdgpu::PPSMC::SetSoftMinByFreq && m.param == ((amdgpu::PPCLK::UCLK << 16) | 1258u)) ++soft_uclk;
    expect(soft_uclk == 1, "SetHardMin rejection falls back to SetSoftMinByFreq for UCLK");

    dev.smuOnline = true;
    uint32_t pinnedMhz = 0;
    expect(amdgpu::smu_pin_uclk_top(dev, &pinnedMhz) != kIOReturnSuccess && !pinnedMhz,
           "a rejected hard floor cannot authorize a modeset, even when a soft floor would succeed");
    reject_hardmin = false;
    reject_softmin = true;
    messages.clear();
    expect(amdgpu::smu_set_uclk_hard_min(dev, 0) != kIOReturnSuccess && messages.size() == 1 &&
           messages.front().id == amdgpu::PPSMC::SetSoftMinByFreq,
           "a failed soft-floor clear leaves the hard floor untouched");
    reject_softmin = false;
    expect(amdgpu::smu_pin_uclk_top(dev, &pinnedMhz) == kIOReturnSuccess && pinnedMhz == 1258u,
           "a confirmed hard floor reports the queried top level");

    // Out-of-range frequency is skipped safely without sending a bogus floor.
    reject_hardmin = false; bad_uclk_freq = true; high_written = false;
    messages.clear();
    expect(amdgpu::smu_smc_hw_setup(dev, 0x8002e00000) == kIOReturnSuccess, "bad UCLK frequency is non-fatal");
    unsigned any_pin = 0;
    for (const auto &m : messages)
        if (m.id == amdgpu::PPSMC::SetHardMinByFreq || m.id == amdgpu::PPSMC::SetSoftMinByFreq) ++any_pin;
    expect(any_pin == 0, "bad UCLK frequency skips pinning safely");

    // Failed EnableAllSmuFeatures skips DPM floor pinning.
    reject_enable = true; bad_uclk_freq = false; high_written = false;
    messages.clear();
    expect(amdgpu::smu_smc_hw_setup(dev, 0x8002e00000) == kIOReturnSuccess, "disabled DPM setup completes");
    unsigned any_dpm_query = 0;
    for (const auto &m : messages)
        if (m.id == amdgpu::PPSMC::GetDpmFreqByIndex || m.id == amdgpu::PPSMC::SetHardMinByFreq) ++any_dpm_query;
    expect(any_dpm_query == 0, "failed EnableAllSmuFeatures skips DPM clock pinning");

    // Wave 10: SMU IF 0x2E..0x33 verification and effective GFXCLK decoding.
    expect(!amdgpu::smu_driver_if_verified(0x2Du), "IF 0x2D is below verified range");
    expect(amdgpu::smu_driver_if_verified(0x2Eu), "IF 0x2E is verified");
    expect(amdgpu::smu_driver_if_verified(0x33u), "IF 0x33 (RX 9060 XT PMFW) is verified");
    expect(!amdgpu::smu_driver_if_verified(0x34u), "unknown newer IF 0x34 stays unverified");

    // Active load (RX 9060 XT live 143 W FP32 snapshot: CurrClock stuck at 1000, PreDs 3088).
    amdgpu::SmuMetrics active {};
    active.CurrClock[amdgpu::SMUClk::GFXCLK] = 1000u;
    active.CurrClock[amdgpu::SMUClk::UCLK]   = 1258u;
    active.AverageGfxclkFrequencyTarget = 3130u;
    active.AverageGfxclkFrequencyPreDs  = 3088u;
    active.AverageGfxclkFrequencyPostDs = 2900u;
    active.AverageGfxActivity = 100u;
    active.AverageUclkActivity = 3u;
    active.AvgTemperature[amdgpu::SMUTemp::EDGE]    = 49u;
    active.AvgTemperature[amdgpu::SMUTemp::HOTSPOT] = 53u;
    expect(amdgpu::smu_effective_gfxclk_mhz(active) == 3088u,
           "active load selects AverageGfxclkFrequencyPreDs over static CurrClock 1000");
    expect(amdgpu::smu_metrics_plausible(active), "active RX 9060 XT metrics pass plausibility");

    // Low activity (<= 5%) selects PostDs (deep-sleep effective clock).
    amdgpu::SmuMetrics idle = active;
    idle.AverageGfxActivity = 5u;
    idle.AverageGfxclkFrequencyPostDs = 420u;
    expect(amdgpu::smu_effective_gfxclk_mhz(idle) == 420u,
           "low activity (<= 5%) selects AverageGfxclkFrequencyPostDs");
    idle.AverageGfxActivity = 6u;
    expect(amdgpu::smu_effective_gfxclk_mhz(idle) == 3088u,
           "activity > 5% switches back to AverageGfxclkFrequencyPreDs");

    // Fallbacks when primary average clock is 0.
    amdgpu::SmuMetrics fb = idle;
    fb.AverageGfxActivity = 0u;
    fb.AverageGfxclkFrequencyPostDs = 0u;
    expect(amdgpu::smu_effective_gfxclk_mhz(fb) == 3088u,
           "zero PostDs at idle falls back to PreDs");
    fb.AverageGfxclkFrequencyPreDs = 0u;
    expect(amdgpu::smu_effective_gfxclk_mhz(fb) == 1000u,
           "zero PreDs and PostDs falls back to CurrClock[GFXCLK]");

    // Plausibility rejects corrupted/shifted fields.
    amdgpu::SmuMetrics badClk = active;
    badClk.AverageGfxclkFrequencyPreDs = 5500u;
    expect(!amdgpu::smu_metrics_plausible(badClk), "out-of-range effective GFXCLK fails plausibility");
    amdgpu::SmuMetrics badTemp = active;
    badTemp.AvgTemperature[amdgpu::SMUTemp::HOTSPOT] = 40u;
    expect(!amdgpu::smu_metrics_plausible(badTemp), "hotspot < edge fails plausibility");
    amdgpu::SmuMetrics badAct = active;
    badAct.AverageGfxActivity = 101u;
    expect(!amdgpu::smu_metrics_plausible(badAct), "activity > 100% fails plausibility");

    std::printf("smu setup: %u failed; simulated PMFW, no hardware access\n", failures);
    return failures ? 1 : 0;
}
