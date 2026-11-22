// Run with dp_pending_latch_test.sh; production glue is extracted verbatim.
#include "scanout_pure.h"
#include "../src/GarnetNativeABI.h"
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <strings.h>

using namespace rdna4scan;
static int checks = 0, failures = 0;
static const char *scenario = "setup";
static void expect(bool ok, const char *what) {
    ++checks;
    if (!ok) { ++failures; std::printf("FAIL [%s]: %s\n", scenario, what); }
}

struct IOLock { bool held = false; };
static IOLock lock;
static std::string calls;
static void IOLockLock(IOLock *l) { expect(l == &lock && !l->held, "lock acquired once"); l->held = true; calls += 'L'; }
static void IOLockUnlock(IOLock *l) { expect(l == &lock && l->held, "lock released once"); l->held = false; calls += 'U'; }
static void locked() { expect(lock.held, "hardware boundary runs under scan lock"); }
static uint64_t now_ns() { return 1000; }
static void test_log(const char *, ...) {}
#define RDNA4LOG(...) test_log(__VA_ARGS__)
constexpr int DCN41_OK = 0;
constexpr uint32_t kOffFlipControl = 0x1234;
struct Device {};
static struct { Device d; const char *tag = nullptr; bool armed = true; } gDcn;
struct ScanGeom { uint32_t w, h, pitchPx, fmt, sw; bool dcc; };
namespace rdna4peer { constexpr uint32_t kLeaseLocalDisplay = 1u; }
struct rdna4_display_txn { bool ok = true; explicit rdna4_display_txn(uint32_t) {} };
// The full production ScanState declaration, not a test copy of its fields.
#include "scan_present_state.inc"
static bool latchDuringLiveRead = false;
static struct Hardware {
    uint64_t plane = 0x100000, early = 0x100000, programmed = 0;
    bool pending = true;
    int primaryRc = DCN41_OK, earlyRc = DCN41_OK, flipRc = DCN41_OK;
    uint32_t flipControl = 0x100, frame = 20, writes = 0;
} hw;
static void scan_read_geom(ScanGeom *g) { locked(); calls += 'G'; *g = {64, 64, 64, kFmtArgb8888, 0, false}; }
static int dcn41_hubp_read_primary_addr(Device *, uint32_t inst, uint64_t *out) {
    locked(); expect(inst == 0, "PRIMARY reads HUBP0"); calls += 'R';
    if (hw.primaryRc == DCN41_OK) *out = hw.plane;
    return hw.primaryRc;
}
static int dcn41_hubp_is_flip_pending(Device *, uint32_t inst, bool *pending, uint64_t *early) {
    locked(); expect(inst == 0, "EARLIEST reads HUBP0"); calls += 'E';
    if (latchDuringLiveRead) {
        hw.flipControl = 0;
        hw.pending = false;
        latchDuringLiveRead = false;
    }
    if (hw.earlyRc == DCN41_OK) { *pending = hw.pending; *early = hw.early; }
    return hw.earlyRc;
}
static int dcn41_otg_get_frame_count(Device *, uint32_t inst, uint32_t *raw) {
    locked(); expect(inst == 0, "frame count reads OTG0"); calls += 'F'; *raw = hw.frame; return DCN41_OK;
}
static uint32_t hubp_reg(uint32_t offset, uint32_t inst) {
    expect(offset == kOffFlipControl && inst == 0, "poll reads flip control"); return offset;
}
static uint32_t dcn_rreg(decltype(gDcn) *, uint32_t reg) {
    locked(); expect(reg == kOffFlipControl, "poll register boundary"); calls += 'C'; return hw.flipControl;
}
static int dcn41_hubp_program_flip(Device *, uint32_t inst, uint64_t mc, uint32_t vmid, bool tmz, bool immediate) {
    locked(); expect(inst == 0 && vmid == 0 && !tmz && !immediate, "flip arguments remain HUBP0 VMID0 non-immediate");
    calls += 'W'; ++hw.writes; hw.programmed = mc; return hw.flipRc;
}
// All three production functions are included unchanged, including their logging.
#include "scan_present_actual.inc"

static void setup(const char *name) {
    scenario = name; expect(!lock.held, "previous scenario released lock");
    gScan = ScanState{}; gScan.lock = &lock; gScan.acquired = true;
    gScan.width = gScan.height = gScan.pitchPx = 64;
    gScan.presentSeq = 7; gScan.vupdates = 9;
    table_init(gScan.tbl, 0x100000, 0x10000);
    for (uint32_t i = 0; i < kMaxSlots; ++i) {
        uint32_t slot = kNoSlot;
        expect(slot_register(gScan.tbl, 0x200000 + i * 0x10000, 0x10000, &slot) == kOk && slot == i, "register distinct slot");
    }
    hw = Hardware{}; gDcn.tag = nullptr; gDcn.armed = true; calls.clear();
}
// The previous flip latches after the poll, while EARLIEST still names the console.
int main() {
    setup("pending flip latches between poll and final live read");
    (void)present_begin(gScan.tbl, 1, 7, 20);
    hw.plane = gScan.tbl.s[1].mc;
    latchDuringLiveRead = true;
    uint64_t out[3] = {101, 102, 103};
    const uint32_t rc = scanPresent(0, 0, out);
    expect(rc == kBusy && hw.writes == 0, "unresolved prior flip defers the new present");
    expect(gScan.presentSeq == 7 && gScan.tbl.presents == 1 && gScan.tbl.replaced == 0 &&
           out[0] == 101 && out[1] == 102 && out[2] == 103,
           "deferral preserves sequence, accounting and outputs");
    // On the buggy path the new address hides the slot that just latched.
    if (hw.writes) { hw.plane = hw.programmed; hw.flipControl = 0x100; hw.pending = true; }
    rdna4n_scan_status st {};
    expect(scanStatus(&st) == kOk, "status succeeds after the interleaving");
    expect((st.slot[1].flags & RDNA4N_SCANSLOT_REUSABLE) == 0,
           "the just-latched slot is never exposed for overwriting");
    expect(st.replaced == 0, "a flip accepted by hardware is not counted as replaced");
    // The next call observes the latch and can use the third slot normally.
    if (!hw.writes) {
        expect(st.front_slot == 1 && st.latched == 1, "next poll accounts prior latch");
        expect(scanPresent(0, 0, out) == kOk && hw.writes == 1 && out[0] == 8,
               "retry succeeds after observing prior latch");
    }
    setup("prior flip still pending");
    (void)present_begin(gScan.tbl, 1, 7, 20);
    hw.plane = gScan.tbl.s[1].mc;
    expect(scanPresent(0, 0, out) == kBusy && hw.writes == 0 && gScan.tbl.pendSlot == 1,
           "a genuinely pending flip is also preserved");
    setup("idle present");
    expect(scanPresent(0, 0, out) == kOk && hw.writes == 1 && out[0] == 8,
           "idle present still succeeds");
    std::printf("dp_pending_latch: %d checks, %d failed\n", checks, failures);
    return failures ? 1 : 0;
}
