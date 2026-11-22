#include <initializer_list>
#define main dcn_link_regression_main
#include "../../../tests/dcn_dp_link_test.cpp"
#undef main

struct AuxModel {
    uint8_t config[9] = {0x14, 0x84, 0, 0, 0, 0, 0, 0, 1};
    uint8_t status[6] = {0x77, 0x77, 1, 0, 0, 0};
    unsigned claims = 0, releases = 0, transfers = 0, validates = 0;
    bool owned = false, busy = false, bound = true;
    unsigned failAt = 0, invalidateAt = 0, deferCount = 0, mutateAt = 0;
    bool shortReply = false, changeSource = false, changeStatus = false;
    rdna4dp::AuxReply failure = rdna4dp::AuxReply::nack;
    uint64_t now = 100, step = 100;
    static bool claim(void *p, const rdna4dp::AuxRoute &route, uint64_t *lease) {
        auto &m = *static_cast<AuxModel *>(p);
        ++m.claims;
        expect(!m.owned && route.dp == 0 && route.dig == 0 && route.otg == 0, "claim pins actual source route");
        if (m.busy || !m.bound) return false;
        m.owned = true;
        *lease = 0x12345678ull;
        return true;
    }
    static bool valid(void *p, uint64_t lease) {
        auto &m = *static_cast<AuxModel *>(p);
        ++m.validates;
        expect(m.owned && lease == 0x12345678ull, "validity checks occur under matching lease");
        return m.bound && (!m.invalidateAt || m.validates < m.invalidateAt);
    }
    static rdna4dp::AuxResult nativeRead(void *p, uint64_t lease, uint32_t address,
                                      uint8_t *bytes, uint32_t count, uint64_t deadline) {
        auto &m = *static_cast<AuxModel *>(p);
        ++m.transfers;
        expect(m.owned && lease == 0x12345678ull && deadline == 50100ull, "fixed deadline and lease reach native read");
        expect((address == 0x100 && count == 9) || (address == 0x202 && count == 6), "only exact configured-link and lane-status native reads");
        m.now += m.step;
        if (m.failAt == m.transfers) return {m.failure, 0};
        if (m.deferCount) { --m.deferCount; return {rdna4dp::AuxReply::defer, 0}; }
        if (m.mutateAt == m.transfers) {
            if (m.changeStatus) m.status[0] ^= 1u;
            else m.config[0] = 0x1e;
        }
        std::memcpy(bytes, address == 0x100 ? m.config : m.status, count);
        if (m.changeSource && m.transfers == 4) regs[0x34c0u + 0x2121u] = 1;
        return {rdna4dp::AuxReply::ack, m.shortReply ? count - 1 : count};
    }
    static void release(void *p, uint64_t lease) {
        auto &m = *static_cast<AuxModel *>(p);
        expect(m.owned && lease == 0x12345678ull, "release is exactly the owned lease");
        m.owned = false;
        ++m.releases;
    }
    static uint64_t clock(void *p) { return static_cast<AuxModel *>(p)->now; }
    rdna4dp::AuxReader reader() { return {this, claim, valid, nativeRead, release, clock}; }
};
static rdna4dp::SinkLink probe(AuxModel &model) {
    const auto reader = model.reader();
    const auto result = rdna4dp::probeSink(&reader, {0, 0, 0});
    expect(!model.owned && model.releases == (model.busy || !model.bound ? 0u : 1u), "all probe exits release once if acquired");
    return result;
}
static void decoding() {
    const uint8_t rates[] = {6, 10, 20, 30};
    const uint32_t expectedRates[] = {1620000, 2700000, 5400000, 8100000};
    const uint8_t lanes[] = {1, 2, 4};
    for (unsigned r = 0; r < 4; ++r) for (auto lane : lanes) {
        AuxModel m;
        m.config[0] = rates[r]; m.config[1] = 0x80 | lane;
        const auto s = rdna4dp::decodeDpcd(m.config, 9, m.status, 6);
        expect(s.status == rdna4dp::SinkStatus::observed && s.configuredRateKbps == expectedRates[r] &&
               s.configuredCoding == 8 && s.configuredLanes == lane && s.clockRecovery == 2 && s.channelEq == 2,
               "DPCD RBR/HBR/HBR2/HBR3 and 1/2/4 lanes decode exactly");
        for (unsigned badLane = 0; badLane < lane; ++badLane) {
            auto saved = m.status[badLane / 2];
            m.status[badLane / 2] &= ~(1u << (4u * (badLane % 2)));
            const auto bad = rdna4dp::decodeDpcd(m.config, 9, m.status, 6);
            expect(bad.clockRecovery == 1 && bad.channelEq == 1, "every active lane must have CR and EQ bits");
            m.status[badLane / 2] = saved;
        }
    }
    AuxModel m;
    m.status[2] = 0;
    auto s = rdna4dp::decodeDpcd(m.config, 9, m.status, 6);
    expect(s.clockRecovery == 2 && s.channelEq == 1, "CR alone does not establish interlane EQ");
    m.status[2] = 1; m.status[0] = 0x11;
    s = rdna4dp::decodeDpcd(m.config, 9, m.status, 6);
    expect(s.clockRecovery == 2 && s.channelEq == 1, "CR without EQ and symbol lock fails EQ");
    for (unsigned lane = 0; lane < 16; ++lane) {
        m.config[1] = static_cast<uint8_t>(lane);
        s = rdna4dp::decodeDpcd(m.config, 9, m.status, 6);
        expect((s.status == rdna4dp::SinkStatus::observed) == (lane == 1 || lane == 2 || lane == 4), "reserved lane counts rejected");
    }
    m.config[1] = 4;
    for (unsigned coding = 0; coding < 4; ++coding) {
        m.config[8] = coding;
        s = rdna4dp::decodeDpcd(m.config, 9, m.status, 6);
        expect(s.configuredCoding == (coding == 1 ? 8u : coding == 2 ? 132u : 0u), "coding exact selection, not a capability mask");
    }
    const uint8_t uhbr[] = {1, 4, 2};
    const uint32_t uhbrRates[] = {10000000, 13500000, 20000000};
    m.config[8] = 2;
    for (unsigned i = 0; i < 3; ++i) {
        m.config[0] = uhbr[i];
        s = rdna4dp::decodeDpcd(m.config, 9, m.status, 6);
        expect(s.configuredRateKbps == uhbrRates[i] && s.status == rdna4dp::SinkStatus::unsupported && s.channelEq == 0,
               "UHBR config decodes without claiming legacy training proof");
    }
    m.config[8] = 1;
    for (auto rate : {0u, 1u, 255u}) {
        m.config[0] = rate;
        expect(rdna4dp::decodeDpcd(m.config, 9, m.status, 6).status == rdna4dp::SinkStatus::unsupported,
               "eDP table and unknown rates are not guessed");
    }
    m.config[0] = 20;
    std::memset(m.status, 255, 6);
    expect(rdna4dp::decodeDpcd(m.config, 9, m.status, 6).status == rdna4dp::SinkStatus::invalidData, "all-ones lane data cannot pass training");
    expect(rdna4dp::decodeDpcd(nullptr, 9, m.status, 6).status == rdna4dp::SinkStatus::invalidData, "null config refused");
    expect(rdna4dp::decodeDpcd(m.config, 8, m.status, 6).status == rdna4dp::SinkStatus::invalidData, "short config refused");
    expect(rdna4dp::decodeDpcd(m.config, 9, m.status, 5).status == rdna4dp::SinkStatus::invalidData, "short status refused");
}
static void transport() {
    AuxModel m;
    auto s = probe(m);
    expect(s.status == rdna4dp::SinkStatus::observed && m.transfers == 4, "stable probe uses two complete observations");
    for (unsigned stage = 1; stage <= 4; ++stage) {
        for (auto failure : {rdna4dp::AuxReply::nack, rdna4dp::AuxReply::disconnected, rdna4dp::AuxReply::timeout, rdna4dp::AuxReply::error}) {
            AuxModel f;
            f.failAt = stage; f.failure = failure;
            s = probe(f);
            expect(s.status != rdna4dp::SinkStatus::observed && s.configuredRateKbps == 0 && f.transfers == stage,
                   "every transfer failure discards partial data and stops");
        }
    }
    AuxModel busy; busy.busy = true;
    expect(probe(busy).status == rdna4dp::SinkStatus::busy && busy.transfers == 0, "busy means no transfer or release");
    AuxModel unbound; unbound.bound = false;
    expect(probe(unbound).status == rdna4dp::SinkStatus::busy && unbound.transfers == 0, "backend must establish connector identity");
    AuxModel shortRead; shortRead.shortReply = true;
    expect(probe(shortRead).status == rdna4dp::SinkStatus::shortRead, "ACK with wrong byte count refused");
    AuxModel defer; defer.deferCount = 3;
    expect(probe(defer).status == rdna4dp::SinkStatus::deferred && defer.transfers == 3, "DEFER has finite three-attempt budget");
    AuxModel retry; retry.deferCount = 2;
    expect(probe(retry).status == rdna4dp::SinkStatus::observed && retry.transfers == 6, "bounded DEFER can recover");
    AuxModel expired; expired.step = 50000;
    expect(probe(expired).status == rdna4dp::SinkStatus::timeout && expired.transfers == 1, "late ACK is not successful data");
    for (unsigned check = 1; check <= 9; ++check) {
        AuxModel lost; lost.invalidateAt = check;
        expect(probe(lost).status == rdna4dp::SinkStatus::changed, "HPD generation or lease loss invalidates any probe stage");
    }
    for (bool status : {false, true}) {
        AuxModel changed; changed.mutateAt = 3; changed.changeStatus = status;
        expect(probe(changed).status == rdna4dp::SinkStatus::changed, "changing config or lane status is discarded");
    }
    expect(rdna4dp::probeSink(nullptr, {0, 0, 0}).status == rdna4dp::SinkStatus::unavailable, "production without backend stays unavailable");
    AuxModel invalid; auto reader = invalid.reader();
    expect(rdna4dp::probeSink(&reader, {4, 0, 0}).status == rdna4dp::SinkStatus::invalidRoute && invalid.claims == 0, "invalid source route cannot acquire");
    reader.nativeRead = nullptr;
    expect(rdna4dp::probeSink(&reader, {0, 0, 0}).status == rdna4dp::SinkStatus::unavailable && invalid.claims == 0, "incomplete transport cannot acquire");
    AuxModel overflow; overflow.now = UINT64_MAX;
    auto overflowReader = overflow.reader();
    expect(rdna4dp::probeSink(&overflowReader, {0, 0, 0}).status == rdna4dp::SinkStatus::timeout && overflow.claims == 0, "deadline overflow refused before acquisition");
}
static void production() {
    const rdna4dp::Timing target{2560, 1440, 2720, 1525, 497750000ull};
    for (unsigned scenario = 0; scenario < 7; ++scenario) {
        setup();
        AuxModel m;
        if (scenario == 1) m.status[1] = 0x11;
        if (scenario == 2) m.config[2] = 1;
        if (scenario == 3) m.config[1] = 2;
        if (scenario == 4) regs[0x34c0u + 0x211eu] = 0x100;
        if (scenario == 5) m.changeSource = true;
        if (scenario == 6) { m.config[0] = 1; m.config[8] = 2; }
        const auto reader = m.reader();
        rdna4dp::Snapshot s;
        rdna4dp::Bandwidth b;
        (void)rdna4dcn::dpPreflight(0, &target, &s, &b, &reader);
        if (scenario == 0) {
            expect(s.rateKbps == 5400000 && s.coding == 8 && s.rateFromSinkStatus == 1 &&
                   b.decision == rdna4dp::Decision::fitsPayload && b.requiredBps == 14932500000ull && b.payloadBps == 17280000000ull,
                   "real preflight accepts bound stable sink HBR2 configuration plus training status");
            std::printf("SYNTHETIC_BOUND_DPCD: rate=%u coding=%u sink_eq=%u demand=%llu payload=%llu\n", s.rateKbps, s.coding,
                        s.sink.channelEq, (unsigned long long)b.requiredBps, (unsigned long long)b.payloadBps);
        } else {
            expect(s.rateKbps == 0 && s.rateFromSinkStatus == 0 && b.decision == rdna4dp::Decision::unknown,
                   "sink config alone, ongoing training, source mismatch or UHBR cannot establish trained legacy rate");
        }
        expect(!m.owned && m.releases == 1 && writes == 0, "production preflight releases and performs no register writes");
    }
    setup();
    rdna4dp::Snapshot s;
    rdna4dp::Bandwidth b;
    expect(rdna4dcn::dpPreflight(0, &target, &s, &b) && s.sink.status == rdna4dp::SinkStatus::unavailable &&
           s.rateKbps == 0 && b.decision == rdna4dp::Decision::unknown, "default production entry never starts AUX and never invents rate");
    rdna4dcn::dp_log_preflight(0, target, "production-no-verified-AUX-backend");
}
int main() {
    if (dcn_link_regression_main()) return 1;
    decoding(); transport(); production();
    std::printf("dpcd_probe: %u total checks, %u failed, register-writes=%u\n", checks, failures, writes);
    return failures ? 1 : 0;
}
