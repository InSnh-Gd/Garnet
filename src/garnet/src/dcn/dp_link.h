#ifndef RDNA4_DP_LINK_H
#define RDNA4_DP_LINK_H

#include "liveraster.h"
#include "dp_dpcd.h"
#include "dp_binding.h"

// Linux AMD DC: dcn_4_1_0_{offset,sh_mask}.h, enc401_read_state,
// enc401_stream_encoder_dp_set_stream_attribute, link_enc2_read_state,
// enc1_dig_connect_to_otg and dcn10_link_encoder_connect_dig_be_to_fe.
namespace rdna4dp {
constexpr uint32_t unknown = 0xffffffffu;
enum class Status : uint32_t { ready, noDevice, unreadable, changed, noRoute, ambiguous };
enum class Format : uint32_t { unknown, rgb, y444, y422, y420, yOnly };
enum class Decision : uint32_t { unknown, fitsPayload, exceedsPayload, invalidTiming };
struct Timing { uint32_t width, height, hTotal, vTotal; uint64_t pixelHz; };
struct Snapshot {
    Status status = Status::noDevice;
    uint32_t activeMask = 0, dp = unknown, otg = unknown, dig = unknown, hpd = unknown;
    uint32_t lanes = 0, bpc = 0, dsc = unknown, fecEnabled = unknown, fecActive = unknown;
    uint32_t trained = unknown, sst = unknown; // source bits only, not sink training evidence
    // Rate/coding require a bound, stable sink observation; PHY rate is not inferred.
    // Do not infer these from DTO, M/N, EDID, lane capability or GOP resolution.
    uint32_t rateKbps = 0, coding = unknown, phy = unknown;
    uint32_t rateFromSinkStatus = 0;
    rdna4_binding_raw witness {};
    SinkLink sink;
    Format format = Format::unknown;
};
struct Bandwidth {
    Decision decision = Decision::unknown;
    uint64_t requiredBps = 0, payloadBps = 0;
};

// Four DIO instances, BASE_IDX 2. DP stream/FE and BE routing are independent.
constexpr uint32_t stride = 0x124u;
static const auto &streamOffsets = rdna4_binding_stream_offsets;
static const auto &linkOffsets = rdna4_binding_link_offsets;
using Raw = rdna4_binding_raw;

static inline bool read2(rdna4lr_ro *ro, uint32_t offset, uint32_t &value) {
    const uint32_t address = dcn41_abs_rd(&ro->d, offset, 2u);
    if (address == DCN41_BAD_OFFSET) return false;
    value = ro->d.rreg(ro->d.cookie, address);
    return value != unknown;
}
static inline uint32_t routeRead(void *p, uint32_t offset) {
    uint32_t value = unknown;
    (void)read2(static_cast<rdna4lr_ro *>(p), offset, value);
    return value;
}
static inline bool gather(rdna4lr_ro *ro, Raw &r) { return rdna4_binding_read(routeRead, ro, &r); }
static inline bool same(const Raw &a, const Raw &b) { return rdna4_binding_same(&a, &b); }
static inline Snapshot snapshot(rdna4lr_ro *ro, uint32_t consoleOtg) {
    Snapshot s;
    if (!ro || ro->state != RDNA4LR_READY || consoleOtg >= 4u) return s;
    Raw a = {}, b = {};
    if (!gather(ro, a) || !gather(ro, b)) { s.status = Status::unreadable; return s; }
    if (!same(a, b)) { s.status = Status::changed; return s; }
    s.witness = b;
    s.status = Status::noRoute;
    uint32_t lit = 0;
    for (uint32_t i = 0; i < 4; ++i) {
        if (b.otg[i] & 1u) ++lit;
        if ((b.stream[i][0] & 0x10001u) == 0x10001u) s.activeMask |= 1u << i;
    }
    // The existing console caller knows one lit OTG, not a general HUBP/MPC/ODM route.
    if (lit > 1u) { s.status = Status::ambiguous; return s; }
    if (!(b.otg[consoleOtg] & 1u)) return s;
    for (uint32_t i = 0; i < 4; ++i) {
        const uint32_t *r = b.stream[i];
        if ((r[0] & 0x10001u) != 0x10001u) continue;
        // Bypass, reset, pending pixel format or disabled FE cannot bind a live stream.
        if ((r[5] & 7u) != consoleOtg || (r[5] & 0x100000u) ||
            (r[6] & 0x30u) != 0x10u || !(r[7] & 1u) || (r[4] & 0x10u)) continue;
        if ((r[6] & 7u) != 0u && (r[6] & 7u) != 5u) continue;
        if (s.dp != unknown) { s.status = Status::ambiguous; s.dp = unknown; return s; }
        s.dp = i;
    }
    if (s.dp == unknown) return s;
    s.otg = consoleOtg;
    const uint32_t *r = b.stream[s.dp];
    s.dsc = r[1] & 1u;
    const uint32_t depths[] = {6u, 8u, 10u, 12u, 16u};
    const uint32_t depth = (r[1] >> 8) & 7u;
    if (!s.dsc && depth < 5u) s.bpc = depths[depth];
    if (!s.dsc) {
        switch ((r[1] >> 4) & 3u) {
        case 0: // RGB and Y444 share a hardware packing; VSC makes MISC0 non-authoritative.
            if (!(r[3] & 0x40u)) {
                const uint32_t color = (r[2] >> 25) & 3u;
                if (color == 0u) s.format = Format::rgb;
                else if (color == 2u) s.format = Format::y444;
            }
            break;
        case 1: s.format = Format::y422; break;
        case 2: s.format = Format::y420; break;
        case 3: s.format = Format::yOnly; break;
        }
    }
    for (uint32_t i = 0; i < 4; ++i) {
        const uint32_t *l = b.link[i];
        if (!(l[2] & 1u) || (l[1] & 0x30u) != 0x10u ||
            ((l[0] >> 8) & 0x7fu) != (1u << s.dp)) continue;
        if (s.dig != unknown) { s.status = Status::ambiguous; s.dig = unknown; return s; }
        s.dig = i;
    }
    if (s.dig == unknown) return s;
    const uint32_t *l = b.link[s.dig];
    s.hpd = (l[0] >> 28) & 7u;
    const uint32_t mode = l[1] & 7u;
    if ((mode != 0u && mode != 5u) || mode != (r[6] & 7u)) return s;
    s.sst = mode == 0u ? 1u : 0u;
    s.trained = (l[3] & 0x110u) == 0x110u ? 1u : 0u;
    const uint32_t lanes = (l[4] & 3u) + 1u;
    if (lanes == 1u || lanes == 2u || lanes == 4u) s.lanes = lanes;
    s.fecEnabled = (l[5] >> 4) & 1u;
    s.fecActive = (l[5] >> 6) & 1u;
    s.status = Status::ready;
    return s;
}

static inline Snapshot snapshotWithSink(rdna4lr_ro *ro, uint32_t consoleOtg, const AuxReader *reader) {
    Snapshot s = snapshot(ro, consoleOtg);
    if (s.status != Status::ready) return s;
    s.sink = probeSink(reader, {s.dp, s.dig, s.otg});
    if (s.sink.status != SinkStatus::observed) return s;
    const Snapshot after = snapshot(ro, consoleOtg);
    if (after.status != Status::ready || !same(s.witness, after.witness)) {
        s.status = Status::changed;
        s.sink = {};
        s.sink.status = SinkStatus::changed;
        return s;
    }
    if (s.trained == 1u && s.sst == 1u && s.lanes == s.sink.configuredLanes &&
        s.sink.configuredCoding == 8u && s.sink.trainingDisabled == 1u &&
        s.sink.clockRecovery == 2u && s.sink.channelEq == 2u) {
        s.rateKbps = s.sink.configuredRateKbps;
        s.coding = s.sink.configuredCoding;
        s.rateFromSinkStatus = 1u;
    }
    return s;
}

// Exact uncompressed stream demand, including fractional bits/pixel for 4:2:0.
// Capacity follows Linux link_validation.c (8b/10b, 97% FEC efficiency).
// A payload fit is a bandwidth result only, never a modeset/training permission.
static inline Bandwidth bandwidth(const Snapshot &s, const Timing &t) {
    Bandwidth b;
    if (!t.width || !t.height || t.hTotal <= t.width || t.vTotal <= t.height ||
        !t.pixelHz || t.pixelHz > 4000000000ull) { b.decision = Decision::invalidTiming; return b; }
    uint32_t halfBpp = 0;
    if (!s.dsc && s.bpc && s.bpc <= 16u) {
        switch (s.format) {
        case Format::rgb: case Format::y444: halfBpp = 6u * s.bpc; break;
        case Format::y422: halfBpp = 4u * s.bpc; break;
        case Format::y420: halfBpp = 3u * s.bpc; break;
        case Format::yOnly: halfBpp = 2u * s.bpc; break;
        case Format::unknown: break;
        }
    }
    if (halfBpp) b.requiredBps = (t.pixelHz * halfBpp + 1ull) / 2ull;
    const bool hbr = s.rateKbps == 1620000u || s.rateKbps == 2700000u ||
                     s.rateKbps == 5400000u || s.rateKbps == 8100000u;
    if (s.status != Status::ready || s.trained != 1u || s.sst != 1u ||
        !hbr || s.coding != 8u || (s.lanes != 1u && s.lanes != 2u && s.lanes != 4u) ||
        s.fecEnabled > 1u || s.fecActive != s.fecEnabled) return b;
    b.payloadBps = static_cast<uint64_t>(s.rateKbps) * 1000ull * s.lanes * 8ull / 10ull;
    if (s.fecActive) b.payloadBps = b.payloadBps * 97ull / 100ull;
    if (b.requiredBps) b.decision = b.requiredBps <= b.payloadBps ? Decision::fitsPayload : Decision::exceedsPayload;
    return b;
}
static inline const char *decisionName(Decision d) {
    switch (d) {
    case Decision::unknown: return "unknown";
    case Decision::fitsPayload: return "fits-payload-only";
    case Decision::exceedsPayload: return "exceeds-payload";
    case Decision::invalidTiming: return "invalid-timing";
    }
    return "unknown";
}
static inline const char *statusName(Status s) {
    switch (s) {
    case Status::ready: return "coherent-route";
    case Status::noDevice: return "no-device-or-console-OTG";
    case Status::unreadable: return "unreadable";
    case Status::changed: return "changed-during-read";
    case Status::noRoute: return "route-unknown";
    case Status::ambiguous: return "ambiguous-route";
    }
    return "unknown";
}
} // namespace rdna4dp
#endif
