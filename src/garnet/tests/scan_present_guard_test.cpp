// Run with tests/scan_present_guard_test.sh. Production glue is extracted verbatim.
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
static bool same_table(const Table &a, const Table &b) {
    if (a.consoleMc != b.consoleMc || a.consoleBytes != b.consoleBytes || a.pendActive != b.pendActive ||
        a.pendSlot != b.pendSlot || a.pendMc != b.pendMc || a.pendId != b.pendId || a.pendTarget != b.pendTarget ||
        a.front != b.front || a.presents != b.presents || a.latched != b.latched || a.replaced != b.replaced ||
        a.repeats != b.repeats || a.firstLatchFrame != b.firstLatchFrame || a.lastLatchFrame != b.lastLatchFrame) return false;
    for (uint32_t i = 0; i < kMaxSlots; ++i) {
        const Slot &x = a.s[i], &y = b.s[i];
        if (x.used != y.used || x.mc != y.mc || x.bytes != y.bytes || x.latchedFrame != y.latchedFrame ||
            x.presents != y.presents || x.latches != y.latches) return false;
    }
    return true;
}
static void refused(uint32_t want) {
    const Table before = gScan.tbl;
    const uint64_t seq = gScan.presentSeq, rejects = gScan.refused;
    uint64_t out[3] = {101, 102, 103};
    const uint32_t rc = scanPresent(0, 0, out);
    expect(rc == want, "expected refusal return code");
    expect(hw.writes == 0, "no program_flip on refusal");
    expect(same_table(before, gScan.tbl), "all slot, pending and present statistics unchanged");
    expect(gScan.presentSeq == seq && gScan.refused == rejects + 1, "only refusal count advances");
    expect(out[0] == 101 && out[1] == 102 && out[2] == 103, "refusal leaves output untouched");
    expect(!lock.held && calls.back() == 'U', "refusal unlocks");
}
static void succeeds() {
    const uint64_t seq = gScan.presentSeq, presents = gScan.tbl.presents, replaced = gScan.tbl.replaced;
    const uint32_t writes = hw.writes;
    uint64_t out[3] = {};
    expect(scanPresent(0, 0, out) == kOk, "idle target succeeds");
    expect(hw.writes == writes + 1 && hw.programmed == gScan.tbl.s[0].mc, "exactly one flip to selected MC");
    expect(gScan.presentSeq == seq + 1 && gScan.tbl.presents == presents + 1 &&
           gScan.tbl.replaced == replaced, "successful present accounted once without replacing an unresolved flip");
    expect(gScan.tbl.pendActive && gScan.tbl.pendSlot == 0 && gScan.tbl.pendId == seq + 1, "pending identity published");
    expect(out[0] == seq + 1 && out[1] == 21 && out[2] == 9, "output id, target frame, vupdates");
    expect(calls.find("REFW") != std::string::npos && !lock.held, "live reads precede flip and unlock");
}
static rdna4n_scan_status status() {
    const Table before = gScan.tbl;
    const uint64_t seq = gScan.presentSeq, rejects = gScan.refused;
    rdna4n_scan_status out;
    std::memset(&out, 0xA5, sizeof(out));
    expect(scanStatus(&out) == kOk, "status query succeeds even if a hardware read fails");
    expect(hw.writes == 0 && same_table(before, gScan.tbl) && gScan.presentSeq == seq && gScan.refused == rejects,
           "status does not program flips or change present bookkeeping");
    expect(!lock.held && out.acquired == 1 && out.presents == before.presents, "status unlocks and reports actual accounting");
    return out;
}
static void status_cases() {
    for (uint32_t slot = 0; slot < kMaxSlots; ++slot) {
        setup("status primary reuse"); hw.plane = gScan.tbl.s[slot].mc;
        const auto primary = status();
        expect((primary.slot[slot].flags & RDNA4N_SCANSLOT_REUSABLE) == 0, "PRIMARY slot is not REUSABLE");
        expect((primary.slot[slot].flags & RDNA4N_SCANSLOT_INUSE) != 0, "PRIMARY slot is INUSE even when EARLIEST differs");
        expect((primary.slot[(slot + 1) % kMaxSlots].flags & RDNA4N_SCANSLOT_REUSABLE) != 0, "PRIMARY occupancy does not block an unrelated idle slot");
        setup("status earliest reuse"); hw.early = gScan.tbl.s[slot].mc; hw.pending = false;
        const auto earliest = status();
        expect((earliest.slot[slot].flags & RDNA4N_SCANSLOT_REUSABLE) == 0 &&
               (earliest.slot[slot].flags & RDNA4N_SCANSLOT_INUSE) != 0, "EARLIEST slot is INUSE and not REUSABLE");
        setup("status front reuse"); gScan.tbl.front = slot;
        const auto front = status();
        expect((front.slot[slot].flags & RDNA4N_SCANSLOT_REUSABLE) == 0 && front.front_slot == slot,
               "front slot is not REUSABLE even when live addresses differ");
        setup("status pending reuse"); (void)present_begin(gScan.tbl, slot, 7, 20);
        const auto pending = status();
        expect((pending.slot[slot].flags & RDNA4N_SCANSLOT_REUSABLE) == 0 &&
               (pending.slot[slot].flags & RDNA4N_SCANSLOT_PENDING) != 0 && pending.pending_slot == slot,
               "pending slot is PENDING and not REUSABLE");
    }
    for (int fault = 0; fault < 4; ++fault) {
        setup(fault == 0 ? "status primary read failure" : fault == 1 ? "status earliest read failure" :
              fault == 2 ? "status both reads fail" : "status zero primary");
        if (fault == 0 || fault == 2) hw.primaryRc = -1;
        if (fault == 1 || fault == 2) hw.earlyRc = -1;
        if (fault == 3) hw.plane = 0;
        const auto out = status();
        for (uint32_t slot = 0; slot < kMaxSlots; ++slot)
            expect((out.slot[slot].flags & RDNA4N_SCANSLOT_REUSABLE) == 0, "unknown live address exposes no REUSABLE slot");
    }
    setup("status idle and recovery"); hw.plane = gScan.tbl.s[0].mc;
    expect((status().slot[0].flags & RDNA4N_SCANSLOT_REUSABLE) == 0, "busy target initially not REUSABLE");
    hw.plane = gScan.tbl.consoleMc;
    const auto idle = status();
    for (uint32_t slot = 0; slot < kMaxSlots; ++slot)
        expect(idle.slot[slot].used == 1 && idle.slot[slot].flags == RDNA4N_SCANSLOT_REUSABLE,
               "idle registered slot is REUSABLE and has no stale flags");
    calls.clear(); succeeds();
    setup("status unregistered"); gScan.tbl.s[1].used = false;
    const auto unused = status();
    expect(unused.slot[1].used == 0 && unused.slot[1].flags == 0, "unregistered output flags are cleared");
    setup("status poll latch"); (void)present_begin(gScan.tbl, 0, 7, 20); hw.flipControl = 0;
    rdna4n_scan_status latched {};
    expect(scanStatus(&latched) == kOk && latched.front_slot == 0 && latched.pending_slot == kNoSlot &&
           latched.latched == 1 && latched.latch_poll == 1 && latched.presents == 1 && hw.writes == 0 &&
           (latched.slot[0].flags & (RDNA4N_SCANSLOT_REUSABLE | RDNA4N_SCANSLOT_PENDING)) == 0,
           "status resolves real poll latch before reporting front as not REUSABLE");
    setup("status unavailable"); gDcn.armed = false;
    expect(scanStatus(&latched) == kNotReady && calls.empty(), "unarmed status fails before hardware access");
}
int main() {
    setup("primary reuse"); hw.plane = gScan.tbl.s[0].mc; refused(kBusy);
    // Same acquired session recovers when the hardware no longer references the target.
    scenario = "busy then recover"; hw.plane = gScan.tbl.consoleMc; calls.clear(); succeeds();
    setup("front reuse"); gScan.tbl.front = 0; refused(kBusy);
    setup("pending reuse"); (void)present_begin(gScan.tbl, 0, 7, 20); refused(kBusy);
    setup("earliest reuse"); hw.early = gScan.tbl.s[0].mc; hw.pending = false; refused(kBusy);
    setup("primary read failure"); hw.primaryRc = -1; refused(kNotReady);
    setup("earliest read failure"); hw.earlyRc = -1; refused(kNotReady);
    setup("both reads fail"); hw.primaryRc = hw.earlyRc = -1; refused(kNotReady);
    setup("zero primary"); hw.plane = 0; refused(kBusy);
    setup("idle flip"); succeeds();
    setup("preserve other pending"); (void)present_begin(gScan.tbl, 1, 7, 20); refused(kBusy);
    scenario = "other pending latches then recover"; hw.flipControl = 0; calls.clear(); succeeds();
    setup("failed MMIO rollback");
    (void)present_begin(gScan.tbl, 1, 7, 20);
    expect(latch_observe(gScan.tbl, false, 19), "prior flip is accounted before testing MMIO rollback");
    const Table before = gScan.tbl; hw.flipRc = -1;
    uint64_t out[3] = {101, 102, 103};
    expect(scanPresent(0, 0, out) == kNotReady, "failed MMIO returns NotReady");
    expect(hw.writes == 1 && same_table(before, gScan.tbl), "failed flip restores whole table including replaced and per-slot statistics");
    expect(gScan.presentSeq == 7 && gScan.refused == 1 && out[0] == 101 && out[1] == 102 && out[2] == 103,
           "failed flip preserves sequence and outputs");
    expect(!lock.held, "failed flip unlocks");
    scenario = "MMIO retry recovery"; hw.flipRc = DCN41_OK; calls.clear(); succeeds();
    setup("poll latches target before guard");
    (void)present_begin(gScan.tbl, 0, 7, 20); hw.flipControl = 0;
    expect(scanPresent(0, 0, out) == kBusy, "newly latched target is now front and busy");
    expect(hw.writes == 0 && gScan.tbl.front == 0 && !gScan.tbl.pendActive && gScan.tbl.latched == 1 &&
           gScan.latchPoll == 1 && gScan.tbl.presents == 1 && gScan.presentSeq == 7,
           "real poll accounts prior latch without counting a new present");
    setup("unregistered"); gScan.tbl.s[0].used = false;
    expect(scanPresent(0, 0, out) == kNotFound && hw.writes == 0 && calls.find('R') == std::string::npos, "unregistered rejected before live reads");
    setup("invalid arguments");
    expect(scanPresent(kMaxSlots, 0, out) == kBadArg && scanPresent(0, 1, out) == kBadArg && calls.empty(), "invalid arguments rejected before lock or MMIO");
    status_cases();
    std::printf("scan_present_guard: %d checks, %d failed\n", checks, failures);
    return failures ? 1 : 0;
}
