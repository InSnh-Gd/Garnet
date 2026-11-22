#ifndef RDNA4_DP_DPCD_H
#define RDNA4_DP_DPCD_H

#include <stdint.h>
#include <stddef.h>

namespace rdna4dp {
// Linux drm_dp.h and drm_dp_{clock_recovery,channel_eq}_ok own these fields.
enum class SinkStatus : uint32_t {
    unavailable, observed, invalidRoute, busy, disconnected, timeout, nack,
    shortRead, changed, invalidData, unsupported, deferred
};
enum class AuxReply : uint32_t { ack, defer, nack, disconnected, timeout, error };
struct SinkLink {
    SinkStatus status = SinkStatus::unavailable;
    uint32_t configuredRateKbps = 0, configuredLanes = 0, configuredCoding = 0;
    // 0 unknown, 1 failed, 2 passed; configured values alone never imply success.
    uint32_t clockRecovery = 0, channelEq = 0, trainingDisabled = 0;
    // Atomic production observer's connector proof; pure DPCD bytes cannot supply it.
    uint32_t connector = 0xffffffffu, aux = 0xffffffffu, hpd = 0xffffffffu, encoderObj = 0;
};
struct AuxRoute { uint32_t dp, dig, otg; };
struct AuxResult { AuxReply reply; uint32_t bytes; };

// A backend must pin connector/AUX/HPD identity and serialize against ALL other
// AUX users/training for the entire lease. It must verify the source route, not
// equate DIG index with AUX index. FB exports real per-connector observations
// separately. Its atomic observer below validates binding inside its own lease.
// claim is nonblocking; nativeRead must honor the absolute monotonic deadline.
// valid checks that HPD, route, ownership and link generation remain unchanged.
// The interface has no DPCD write, I2C-over-AUX, reset, training or raw MMIO op.
struct AuxReader {
    void *cookie;
    bool (*claim)(void *, const AuxRoute &, uint64_t *lease);
    bool (*valid)(void *, uint64_t lease);
    AuxResult (*nativeRead)(void *, uint64_t lease, uint32_t address,
                            uint8_t *data, uint32_t bytes, uint64_t deadlineUs);
    void (*release)(void *, uint64_t lease);
    uint64_t (*nowUs)(void *);
    // Optional whole observation: provider owns and releases the complete hardware
    // lease internally. No cached-byte imitation of a live nativeRead lease.
    SinkLink (*observe)(void *, const AuxRoute &) = nullptr;
};

static inline SinkLink decodeDpcd(const uint8_t *config, size_t configBytes,
                                 const uint8_t *status, size_t statusBytes) {
    SinkLink s;
    s.status = SinkStatus::invalidData;
    if (!config || !status || configBytes != 9u || statusBytes != 6u) return s;
    const uint32_t lanes = config[1] & 0xfu;
    if ((lanes != 1u && lanes != 2u && lanes != 4u) || (config[1] & 0x70u)) return s;
    if (config[8] != 1u && config[8] != 2u) return s;
    s.configuredLanes = lanes;
    s.configuredCoding = config[8] == 1u ? 8u : 132u;
    s.trainingDisabled = (config[2] & 0xfu) == 0u ? 1u : 0u;
    if (s.configuredCoding == 8u) {
        switch (config[0]) {
        case 0x06u: s.configuredRateKbps = 1620000u; break;
        case 0x0au: s.configuredRateKbps = 2700000u; break;
        case 0x14u: s.configuredRateKbps = 5400000u; break;
        case 0x1eu: s.configuredRateKbps = 8100000u; break;
        default: s.status = SinkStatus::unsupported; return s; // eDP rate table needs 0x010/0x115.
        }
    } else {
        switch (config[0]) {
        case 0x01u: s.configuredRateKbps = 10000000u; break;
        case 0x04u: s.configuredRateKbps = 13500000u; break;
        case 0x02u: s.configuredRateKbps = 20000000u; break;
        default: s.status = SinkStatus::unsupported; return s;
        }
        // UHBR requires its separate EQ/CDS/LT_FAILED checks, not legacy EQ bits.
        s.status = SinkStatus::unsupported;
        return s;
    }
    // 0xff in defined status fields is not accepted as "all lanes passed".
    if ((status[0] & 0x88u) || (status[1] & 0x88u) || (status[2] & 0x40u)) {
        s.status = SinkStatus::invalidData;
        return s;
    }
    bool cr = true, eq = (status[2] & 1u) != 0u;
    for (uint32_t lane = 0; lane < lanes; ++lane) {
        const uint32_t bits = (status[lane / 2u] >> (4u * (lane % 2u))) & 7u;
        cr = cr && (bits & 1u) != 0u;
        eq = eq && bits == 7u;
    }
    s.clockRecovery = cr ? 2u : 1u;
    s.channelEq = eq ? 2u : 1u;
    s.status = SinkStatus::observed;
    return s;
}

static inline SinkStatus readDpcdBlock(const AuxReader &a, uint64_t lease,
                                       uint64_t start, uint32_t address,
                                       uint8_t *bytes, uint32_t count) {
    const uint64_t deadline = start + 50000ull;
    for (uint32_t attempt = 0; attempt < 3u; ++attempt) {
        const uint64_t now = a.nowUs(a.cookie);
        if (now < start || now >= deadline) return SinkStatus::timeout;
        if (!a.valid(a.cookie, lease)) return SinkStatus::changed;
        const AuxResult r = a.nativeRead(a.cookie, lease, address, bytes, count, deadline);
        const uint64_t after = a.nowUs(a.cookie);
        if (after < now || after >= deadline) return SinkStatus::timeout;
        if (!a.valid(a.cookie, lease)) return SinkStatus::changed;
        switch (r.reply) {
        case AuxReply::ack: return r.bytes == count ? SinkStatus::observed : SinkStatus::shortRead;
        case AuxReply::defer: break;
        case AuxReply::nack: return SinkStatus::nack;
        case AuxReply::disconnected: return SinkStatus::disconnected;
        case AuxReply::timeout: return SinkStatus::timeout;
        case AuxReply::error: return SinkStatus::invalidData;
        }
    }
    return SinkStatus::deferred;
}

static inline SinkLink probeSink(const AuxReader *reader, const AuxRoute &route) {
    SinkLink s;
    if (!reader) return s;
    if (route.dp >= 4u || route.dig >= 4u || route.otg >= 4u) {
        s.status = SinkStatus::invalidRoute; return s;
    }
    const AuxReader &a = *reader;
    if (a.observe) return a.observe(a.cookie, route);
    if (!a.claim || !a.valid || !a.nativeRead || !a.release || !a.nowUs) return s;
    const uint64_t start = a.nowUs(a.cookie);
    if (start > UINT64_MAX - 50000ull) { s.status = SinkStatus::timeout; return s; }
    uint64_t lease = 0;
    if (!a.claim(a.cookie, route, &lease)) { s.status = SinkStatus::busy; return s; }
    uint8_t config[2][9] = {}, status[2][6] = {};
    s.status = SinkStatus::observed;
    for (uint32_t pass = 0; pass < 2u; ++pass) {
        s.status = readDpcdBlock(a, lease, start, 0x100u, config[pass], 9u);
        if (s.status != SinkStatus::observed) break;
        s.status = readDpcdBlock(a, lease, start, 0x202u, status[pass], 6u);
        if (s.status != SinkStatus::observed) break;
    }
    if (s.status == SinkStatus::observed) {
        for (uint32_t i = 0; i < 9u; ++i)
            if (config[0][i] != config[1][i]) s.status = SinkStatus::changed;
        for (uint32_t i = 0; i < 6u; ++i)
            if (status[0][i] != status[1][i]) s.status = SinkStatus::changed;
        if (!a.valid(a.cookie, lease)) s.status = SinkStatus::changed;
        const uint64_t end = a.nowUs(a.cookie);
        if (end < start || end >= start + 50000ull) s.status = SinkStatus::timeout;
        if (s.status == SinkStatus::observed) s = decodeDpcd(config[0], 9u, status[0], 6u);
    }
    a.release(a.cookie, lease);
    return s;
}

static inline const char *sinkStatusName(SinkStatus s) {
    switch (s) {
    case SinkStatus::unavailable: return "transport-unavailable";
    case SinkStatus::observed: return "sink-observed";
    case SinkStatus::invalidRoute: return "unbound-route";
    case SinkStatus::busy: return "lease-unavailable";
    case SinkStatus::disconnected: return "disconnected";
    case SinkStatus::timeout: return "timeout";
    case SinkStatus::nack: return "nack";
    case SinkStatus::shortRead: return "short-read";
    case SinkStatus::changed: return "changed";
    case SinkStatus::invalidData: return "invalid-data";
    case SinkStatus::unsupported: return "unsupported-rate-or-coding";
    case SinkStatus::deferred: return "defer-limit";
    }
    return "unknown";
}
} // namespace rdna4dp
#endif
