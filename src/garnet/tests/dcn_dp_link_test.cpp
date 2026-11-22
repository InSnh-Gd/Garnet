#include <cstdio>
#include <cstdint>
#include <cstring>
#include <cstdarg>
#include "dp_link.h"
#include "dcn.hpp"

static uint32_t regs[0x10000];
static unsigned reads, writes, checks, failures;
static uint32_t changeAddress = rdna4dp::unknown, changeValue, changeAfter;
static rdna4lr_ro gLr;
static void expect(bool ok, const char *label) {
    ++checks;
    if (!ok) { ++failures; std::printf("FAIL %s\n", label); }
}
static uint32_t rd(void *, uint32_t a) {
    ++reads;
    if (a == changeAddress && reads >= changeAfter) return changeValue;
    return a < 0x10000u ? regs[a] : rdna4dp::unknown;
}
static void wr(void *, uint32_t, uint32_t) { ++writes; }
static void delay(void *, uint32_t) { ++writes; }
static void logLine(const char *fmt, ...) {
    va_list args;
    va_start(args, fmt);
    std::vprintf(fmt, args);
    va_end(args);
    std::putchar('\n');
}
#define RDNA4LOG(...) logLine(__VA_ARGS__)
namespace rdna4dcn {
static bool gatheredUnbound = false;
static bool dpGatherUnbound() { return gatheredUnbound; }
static rdna4dp::SinkLink boundTestObserve(void *, const rdna4dp::AuxRoute &) {
    rdna4dp::SinkLink s;
    if (gatheredUnbound) s.status = rdna4dp::SinkStatus::invalidRoute;
    return s;
}
static const rdna4dp::AuxReader kDpBoundReader {nullptr,nullptr,nullptr,nullptr,nullptr,nullptr,boundTestObserve};
#include "dcn_dp_actual.inc"
}
static void setup(unsigned dp = 0, unsigned be = 0, unsigned otg = 0) {
    std::memset(regs, 0, sizeof(regs));
    gLr = {};
    reads = writes = 0;
    changeAddress = rdna4dp::unknown;
    const uint32_t bases[] = {0x12u, 0xc0u, 0x34c0u, 0x9000u, 0x2403c00u};
    expect(rdna4lr_ro_build(&gLr, nullptr, rd, bases, 0x10000u), "read-only device builds");
    gLr.d.wreg = wr;
    gLr.d.udelay = delay;
    const unsigned f = 0x34c0u + dp * 0x124u, b = 0x34c0u + be * 0x124u;
    regs[0x34c0u + 0x1b43u + otg * 0x80u] = 1;
    regs[f + 0x2122u] = 0x10201u;
    regs[f + 0x211fu] = 0x200u;
    regs[f + 0x2120u] = 0x40000000u;
    regs[f + 0x2093u] = otg;
    regs[f + 0x2094u] = 0x10u;
    regs[f + 0x2095u] = 1;
    regs[b + 0x20bcu] = (1u << dp) << 8;
    regs[b + 0x20bbu] = 0x10u;
    regs[b + 0x20bdu] = 1;
    regs[b + 0x211eu] = 0x110u;
    regs[b + 0x2121u] = 3;
}
static rdna4dp::Snapshot sample(unsigned otg = 0) {
    rdna4dp::Snapshot s;
    (void)rdna4dcn::dpSnapshot(otg, &s);
    expect(writes == 0, "snapshot never writes");
    return s;
}
static void routes() {
    for (unsigned dp = 0; dp < 4; ++dp) for (unsigned be = 0; be < 4; ++be) {
        setup(dp, be, dp);
        const auto s = sample(dp);
        expect(s.status == rdna4dp::Status::ready && s.dp == dp && s.otg == dp && s.dig == be,
               "DP0..3 resolves independent BE and OTG routing");
        expect(s.activeMask == (1u << dp) && s.bpc == 10 && s.format == rdna4dp::Format::rgb,
               "observed 0x200/0x40000000 decodes RGB10");
        expect(s.lanes == 4 && s.trained == 1 && s.dsc == 0 && s.fecActive == 0,
               "link lane training DSC FEC source fields");
        expect(s.rateKbps == 0 && s.coding == rdna4dp::unknown && s.phy == rdna4dp::unknown,
               "unobserved transport facts stay unknown");
    }
    setup();
    regs[0x34c0u + 0x2094u] = regs[0x34c0u + 0x2095u] = regs[0x34c0u + 0x20bdu] = 0;
    const auto sparse = sample();
    expect(sparse.status == rdna4dp::Status::noRoute && sparse.activeMask == 1 && sparse.dp == rdna4dp::unknown,
           "sparse logged DP0 facts alone do not prove the route");
    setup();
    regs[0x34c0u + 0x2122u] = 1;
    expect(sample().dp == rdna4dp::unknown, "enabled but inactive is not a live console");
    setup();
    regs[0x34c0u + 0x2093u] |= 0x100000u;
    expect(sample().status == rdna4dp::Status::noRoute, "digital bypass route refused");
    setup();
    regs[0x34c0u + 0x213du] = 0x10u;
    expect(sample().status == rdna4dp::Status::noRoute, "pending pixel format refused");
    setup();
    regs[0x34c0u + 0x2094u] = 0x13u;
    expect(sample().status == rdna4dp::Status::noRoute, "HDMI FE cannot be claimed as DP");
    setup();
    regs[0x34c0u + 0x20bbu] = 0x15u;
    expect(sample().status == rdna4dp::Status::noRoute, "FE and BE modes must agree");
    setup();
    regs[0x34c0u + 0x1b43u + 0x80u] = 1;
    expect(sample().status == rdna4dp::Status::ambiguous, "multiple OTGs do not establish console identity");
    setup();
    regs[0x34c0u + 0x124u + 0x20bcu] = 0x100;
    regs[0x34c0u + 0x124u + 0x20bbu] = 0x10;
    regs[0x34c0u + 0x124u + 0x20bdu] = 1;
    expect(sample().status == rdna4dp::Status::ambiguous, "duplicate BE route refused");
    setup();
    regs[0x34c0u + 0x124u + 0x2122u] = 0x10001;
    regs[0x34c0u + 0x124u + 0x2094u] = 0x10;
    regs[0x34c0u + 0x124u + 0x2095u] = 1;
    expect(sample().status == rdna4dp::Status::ambiguous, "duplicate live FE route refused");
}
static void invalidReads() {
    for (unsigned instance = 0; instance < 4; ++instance) {
        for (const auto off : rdna4dp::streamOffsets) {
            setup();
            regs[0x34c0u + instance * 0x124u + off] = rdna4dp::unknown;
            expect(sample().status == rdna4dp::Status::unreadable, "every stream all-ones read rejected");
        }
        for (const auto off : rdna4dp::linkOffsets) {
            setup();
            regs[0x34c0u + instance * 0x124u + off] = rdna4dp::unknown;
            expect(sample().status == rdna4dp::Status::unreadable, "every link all-ones read rejected");
        }
        setup();
        regs[0x34c0u + 0x1b43u + instance * 0x80u] = rdna4dp::unknown;
        expect(sample().status == rdna4dp::Status::unreadable, "every OTG all-ones read rejected");
    }
    for (const auto off : rdna4dp::streamOffsets) {
        setup();
        changeAddress = 0x34c0u + off;
        changeAfter = 85;
        changeValue = regs[changeAddress] ^ (off == 0x2120u ? 0x1000000u : 1u);
        expect(sample().status == rdna4dp::Status::changed, "stale stream field rejected");
    }
    for (unsigned j = 0; j < 6; ++j) {
        setup();
        changeAddress = 0x34c0u + rdna4dp::linkOffsets[j];
        changeAfter = 85;
        const uint32_t bits[] = {0x100u, 1u, 1u, 0x10u, 1u, 0x10u};
        changeValue = regs[changeAddress] ^ bits[j];
        expect(sample().status == rdna4dp::Status::changed, "stale link field rejected");
    }
    setup();
    changeAddress = 0x34c0u + 0x2122u;
    changeAfter = 85;
    changeValue = 0x10201u ^ 0x100000u;
    expect(sample().status == rdna4dp::Status::ready, "unowned transient keepout bit ignored");
    setup();
    changeAddress = 0x34c0u + 0x1b43u;
    changeAfter = 85;
    changeValue = 0;
    expect(sample().status == rdna4dp::Status::changed, "console OTG changed during read refused");
    setup();
    changeAddress = 0x34c0u + 0x2122u;
    changeAfter = 85;
    changeValue = rdna4dp::unknown;
    expect(sample().status == rdna4dp::Status::unreadable, "second sample all-ones rejected");
    setup();
    gLr.d.mmio_dwords = 0x5000u;
    expect(sample().status == rdna4dp::Status::unreadable, "BAR bounds checked before reading");
}
static void formats() {
    const unsigned depths[] = {6, 8, 10, 12, 16};
    for (unsigned d = 0; d < 8; ++d) {
        setup();
        regs[0x34c0u + 0x211fu] = d << 8;
        expect(sample().bpc == (d < 5 ? depths[d] : 0), "wire depth enum including reserved codes");
    }
    const rdna4dp::Format f[] = {rdna4dp::Format::rgb, rdna4dp::Format::y422, rdna4dp::Format::y420, rdna4dp::Format::yOnly};
    for (unsigned p = 0; p < 4; ++p) {
        setup();
        regs[0x34c0u + 0x211fu] |= p << 4;
        expect(sample().format == f[p], "uncompressed packing format enum");
    }
    setup();
    regs[0x34c0u + 0x2120u] |= 4u << 24;
    expect(sample().format == rdna4dp::Format::y444, "MISC distinguishes Y444 from RGB packing");
    setup();
    regs[0x34c0u + 0x2124u] = 0x40u;
    expect(sample().format == rdna4dp::Format::unknown, "VSC colorimetry is not guessed from MISC");
    setup();
    regs[0x34c0u + 0x211fu] |= 1u;
    expect(sample().dsc == 1 && sample().format == rdna4dp::Format::unknown, "DSC not priced as uncompressed");
    setup();
    regs[0x34c0u + 0x212du] = 0x70u;
    expect(sample().fecEnabled == 1 && sample().fecActive == 1, "FEC enabled and active observed separately");
    for (unsigned lanes = 0; lanes < 4; ++lanes) {
        setup();
        regs[0x34c0u + 0x2121u] = lanes;
        expect(sample().lanes == (lanes == 2 ? 0 : lanes + 1), "one two four lane codes only");
    }
}
static void bandwidth() {
    const rdna4dp::Timing target{2560, 1440, 2720, 1525, 497750000ull};
    setup();
    rdna4dp::Snapshot s;
    rdna4dp::Bandwidth b;
    expect(rdna4dcn::dpPreflight(0, &target, &s, &b) == 1 && b.decision == rdna4dp::Decision::unknown &&
           b.requiredBps == 14932500000ull && b.payloadBps == 0, "real preflight RGB10 exact demand with unknown rate");
    rdna4dcn::dp_log_preflight(0, target, "synthetic-route-plus-observed-DP0-format");
    rdna4dcn::gatheredUnbound = true;
    expect(rdna4dcn::dpPreflight(0, &target, &s, &b) == 1 &&
           s.sink.status == rdna4dp::SinkStatus::invalidRoute && !s.rateFromSinkStatus &&
           !s.rateKbps && b.decision == rdna4dp::Decision::unknown,
           "actual preflight refuses observed but console-unbound per-connector transport");
    rdna4dcn::gatheredUnbound = false;
    // Synthetic verified transport inputs exercise the production capacity helper.
    // The production gather deliberately does not manufacture this verification.
    s.coding = 8;
    const uint32_t rates[] = {1620000, 2700000, 5400000, 8100000};
    const uint32_t laneCounts[] = {1u, 2u, 4u};
    for (auto rate : rates) for (auto lanes : laneCounts) {
        s.rateKbps = rate; s.lanes = lanes;
        b = rdna4dp::bandwidth(s, target);
        const uint64_t capacity = static_cast<uint64_t>(rate) * 800ull * lanes;
        expect(b.payloadBps == capacity && b.requiredBps == 14932500000ull &&
               b.decision == (capacity >= b.requiredBps ? rdna4dp::Decision::fitsPayload : rdna4dp::Decision::exceedsPayload),
               "RBR HBR HBR2 HBR3 exact bandwidth across lanes");
    }
    s.rateKbps = 5400000; s.lanes = 4;
    const rdna4dp::Format formats[] = {rdna4dp::Format::rgb, rdna4dp::Format::y444, rdna4dp::Format::y422, rdna4dp::Format::y420, rdna4dp::Format::yOnly};
    const uint64_t needs[] = {14932500000ull, 14932500000ull, 9955000000ull, 7466250000ull, 4977500000ull};
    for (unsigned i = 0; i < 5; ++i) {
        s.format = formats[i];
        expect(rdna4dp::bandwidth(s, target).requiredBps == needs[i], "exact format bandwidth including 420");
    }
    s.format = rdna4dp::Format::rgb; s.bpc = 8;
    expect(rdna4dp::bandwidth(s, target).requiredBps == 11946000000ull, "RGB8 exact EDID clock demand");
    s.fecEnabled = s.fecActive = 1;
    expect(rdna4dp::bandwidth(s, target).payloadBps == 16761600000ull, "Linux 97 percent FEC payload allowance");
    s.fecActive = 0;
    expect(rdna4dp::bandwidth(s, target).decision == rdna4dp::Decision::unknown, "FEC mismatch cannot grant capacity");
    s.fecEnabled = 0;
    s.dsc = 1;
    expect(rdna4dp::bandwidth(s, target).requiredBps == 0 && rdna4dp::bandwidth(s, target).decision == rdna4dp::Decision::unknown,
           "DSC PPS allocation unknown");
    s.dsc = 0; s.sst = 0;
    expect(rdna4dp::bandwidth(s, target).decision == rdna4dp::Decision::unknown, "MST allocation unknown");
    s.sst = 1; s.trained = 0;
    expect(rdna4dp::bandwidth(s, target).decision == rdna4dp::Decision::unknown, "untrained link refused");
    s.trained = 1; s.rateKbps = 10000000;
    expect(rdna4dp::bandwidth(s, target).decision == rdna4dp::Decision::unknown, "UHBR not treated as HBR");
    expect(rdna4dcn::dpPreflight(0, nullptr, &s, &b) && b.decision == rdna4dp::Decision::invalidTiming, "missing timing refused");
    auto invalid = target; invalid.pixelHz = UINT64_MAX;
    expect(rdna4dp::bandwidth(s, invalid).decision == rdna4dp::Decision::invalidTiming, "overflow timing refused");
    expect(writes == 0, "preflight has zero writes");
}
int main() {
    routes(); invalidReads(); formats(); bandwidth();
    std::printf("dcn_dp_link: %u checks, %u failed, writes=%u\n", checks, failures, writes);
    return failures ? 1 : 0;
}
