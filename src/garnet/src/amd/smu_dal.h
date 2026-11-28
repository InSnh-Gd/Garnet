//
//  smu_dal.h - native step S2-DISPCLK (kext 0.0.604): the DAL mailbox sender and the named experiment steps E1b / E2 / E3 / E4 of
//  driver design. Default OFF: nothing is sent unless the boot-arg rdna4-dalsmc=<1..4> is present AND the boot is a
//  native boot (rdna4-native=1 accepted) whose S1b self-test reported POSITIVE PASS, and the only caller is the RDNA4N selector 15
//  (GarnetNativeClient -> n1c_dal_step) on a user-client thread: never the interrupt handler, never under a display lock.
//
#pragma once
#include <stdint.h>
#include "../GarnetNativeABI.h"
#include "amdgpu_regs.h"
#include "smu_dal_pure.h"

namespace amdgpu {

// One DAL message, as the sender saw it (all filled even on a refusal). rc = the IOReturn the sender returns.
struct DalMsg {
    uint32_t rc;
    uint32_t refuse;     // rdna4dal::Refuse: non-zero = NOTHING was sent
    uint32_t preResp;    // RESP as found before the send
    uint32_t resp;       // RESP after (0 = timeout)
    uint32_t arg;        // ARG after (the output of the message)
    uint32_t us;         // send-to-reply time
};

// The gate every send needs: native boot + S1b POSITIVE PASS, and the boot-arg level in 1..4 (else 0).
uint32_t dal_gated_level();

// The only DAL sender. Checks the allowlist first (pure, smu_dal_pure.h), refuses when RESP == 0 before sending, takes the shared mailbox
// lock (smu_lock_enter), waits at most 500 ms for the reply with IOSleep, and logs one line. Returns kIOReturnSuccess only for RESP == 1.
kern_return_t smu_dal_send(const DeviceContext &dev, uint32_t msg, uint32_t param, DalMsg *m);

// Sleepable opt-in collection outside display locks. The caller resolves the DCN 4.01 timer via the read-only DCN device. On failure no row is usable.
kern_return_t smu_dal_query_clock_snapshot(const DeviceContext &dev, uint32_t dchubTimerAddress,
                                          rdna4dal::ClockSnapshot &snapshot);

// True while a DAL STEP runs (owner 1; the mode trial refuses to start then; 0.0.605). 0.0.607: a clock hold (owner 2) does NOT count - it belongs to the mode trial that holds it.
bool dal_busy();

// 0.0.607, P4: the clock hold of the mode trial's row 120 (SMU clock-hold sequence; pure logic in smu_dal_pure.h, hold_raise / hold_release). All three sleep and wait on the PMFW: trial thread,
// `dcnmode 0` and the kext stop only - never the watchdog, never under a display lock.
uint32_t dal_hold_pre();                       // rdna4dal::HoldPre: 0 = a hold may start now (native + S1b, rdna4-dalsmc=4, E1b done this boot, latch clear, state IDLE / RELEASED, owner idle)
uint32_t dal_hold_state();                     // rdna4dal::HoldSt
uint32_t dal_hold_raise(DeviceContext &dev, uint32_t needDispKhz, uint32_t needDppKhz, rdna4dal::HoldRep *rep, rdna4dal::Decoded *after);   // rdna4dal::HoldRc (0 = HELD)
uint32_t dal_hold_release(DeviceContext &dev, bool restoreBad, rdna4dal::HoldRep *rep);   // rdna4dal::RelRc (0 = RELEASED); restoreBad never releases

// Run one experiment step (1 E1b, 2 E2, 3 E3, 4 E4). Always fills *out; the return value is Success unless the arguments are malformed.
// Sleeps (up to ~65 s for E4). One step at a time: a second concurrent call answers DENIED.
IOReturn dal_run_step(DeviceContext &dev, uint32_t step, rdna4n_dal_result *out);

}  // namespace amdgpu
