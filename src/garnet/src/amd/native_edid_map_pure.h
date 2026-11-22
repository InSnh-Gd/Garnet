//
//  native_edid_map_pure.h - the pure half of the display-nub EDID adapter.
//
//  It keeps ONE table for the local M6 instances:
//    instance 0 = the DP console display, OTG0, no display nub;
//    instance 1 = monitor A, GarnetDisplayIndex 2, OTG1, DDC line 2;
//    instance 2 = monitor B, GarnetDisplayIndex 1, OTG2, DDC line 3.
//  The table is checked against GarnetDisplayOps.h's per-index geometry table.
//
//  The upstream display backend publishes the sink's EDID as "EDID,AUX<n>" or
//  "EDID,DDC<n>", where <n> is the AtomBIOS path record's DDC/AUX line. It also
//  publishes AtomBIOS,Connectors and, with the local 0004 patch, a structured
//  "EDID,<bus><n>-Connector" descriptor. An EDID is converted for a local nub
//  only when all of these agree: the local table has exactly that DDC line, the
//  connector list names exactly one connector on it, its type matches the
//  property's bus, the descriptor repeats the same line/HPD/type, and (when the
//  descriptor declares a head) that head is this route's OTG. The caller then
//  compares EDID block 0 byte for byte with its own DDC read.
//
//  A descriptor with headProven = 0 claims no upstream head and must leave
//  headOtg = 0. That is not a missing local head: the line -> OTG/index row
//  above is the head source. A descriptor with headProven = 1 must agree with
//  that row, so a conflicting upstream claim is refused. Thus the upstream
//  head can corroborate or veto the local route, but is never used to invent a
//  connector -> OTG pairing.
//
//  The AtomBIOS connector ORDER is recorded for diagnostics only. No function
//  derives a head or OTG from it: OTG comes from the local table above.
//
#pragma once
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "../GarnetDisplayOps.h"

namespace rdna4edid {

enum : uint32_t {
    kDdcNone = 0xFFFFFFFFu,
    kDescVersion = 1u,
    kDescSize = 36u,
    kMaxConnectors = 8u,
};

enum Bus : uint32_t { kBusNone = 0u, kBusAux = 1u, kBusDdc = 2u };

enum Type : uint32_t {
    kTypeUnknown = 0u,
    kTypeDp = 1u,
    kTypeHdmiA = 2u,
    kTypeDviD = 3u,
    kTypeVga = 4u,
    kTypeUsbC = 5u,
};

struct Route {
    uint32_t inst;
    uint32_t index;      // GarnetDisplayIndex; 0 for the DP console display
    uint32_t otg;
    uint32_t ddcLine;    // kDdcNone when the local path has no proven DDC line
    uint32_t w;
    uint32_t h;
    bool nub;            // true: GarnetDisplayNub / GarnetFramebuffer; false: the DP console display
};

inline bool route_valid(const Route &r) {
    return r.inst < 3u &&
           ((r.inst == 0u && !r.nub && r.index == 0u && r.otg == 0u && r.ddcLine == kDdcNone) ||
            (r.nub && r.index != 0u && r.ddcLine != kDdcNone && r.w != 0u && r.h != 0u));
}

// A caller-built Route is not trusted either: every published field must still
// be the shared geometry table's row for that instance/index.
inline bool route_matches_geom(const Route &r) {
    if (!route_valid(r)) return false;
    if (!r.nub) return r.inst == 0u && r.index == 0u && r.otg == 0u && r.ddcLine == kDdcNone;
    GarnetDispGeom g;
    return garnetdisp_geom_for_index(r.index, &g) && g.inst == r.inst && g.otg == r.otg &&
           g.ddcLine == r.ddcLine && g.w == r.w && g.h == r.h;
}

inline bool route_for_inst(uint32_t inst, Route *out) {
    if (!out) return false;
    Route r {};
    switch (inst) {
    case 0u:
        r = Route { 0u, 0u, 0u, kDdcNone, 0u, 0u, false };
        break;
    case GARNET_DISP_INST_MONA:
        r = Route { GARNET_DISP_INST_MONA, GARNET_DISPA_INDEX, GARNET_DISPA_OTG,
                    GARNET_DISPA_DDC_LINE, GARNET_DISPA_W, GARNET_DISPA_H, true };
        break;
    case GARNET_DISP_INST_MONB:
        r = Route { GARNET_DISP_INST_MONB, GARNET_DISP_INDEX, GARNET_DISP_OTG,
                    GARNET_DISP_DDC_LINE, GARNET_DISP_W, GARNET_DISP_H, true };
        break;
    default:
        return false;
    }
    GarnetDispGeom g;
    if (r.nub && (!garnetdisp_geom_for_index(r.index, &g) ||
                  g.inst != r.inst || g.otg != r.otg || g.ddcLine != r.ddcLine ||
                  g.w != r.w || g.h != r.h))
        return false;
    *out = r;
    return route_matches_geom(*out);
}

inline bool route_for_index(uint32_t index, Route *out) {
    if (!out) return false;
    const uint32_t inst = index == GARNET_DISP_INDEX ? GARNET_DISP_INST_MONB
                         : index == GARNET_DISPA_INDEX ? GARNET_DISP_INST_MONA : 0u;
    if (!inst) return false;
    return route_for_inst(inst, out);
}

inline bool route_for_ddc(uint32_t ddcLine, Route *out) {
    if (!out) return false;
    for (uint32_t inst = 0u; inst < 3u; ++inst) {
        Route r {};
        if (route_for_inst(inst, &r) && r.nub && r.ddcLine == ddcLine) {
            *out = r;
            return true;
        }
    }
    return false;
}

inline bool bus_for_type(uint32_t type, Bus *bus) {
    if (!bus) return false;
    switch (type) {
    case kTypeDp:
    case kTypeUsbC: *bus = kBusAux; return true;
    case kTypeHdmiA:
    case kTypeDviD: *bus = kBusDdc; return true;
    default: return false;
    }
}

inline const char *bus_name(Bus bus) {
    switch (bus) {
    case kBusAux: return "AUX";
    case kBusDdc: return "DDC";
    default: return "";
    }
}

// Standalone Garnet embeds GarnetPrimaryFramebuffer directly and retires the
// legacy external RDNA4FB kext, so legacy RDNA4FB owner identity is refused.
inline bool local_leaf_name(const char *name) {
    (void)name;
    return false;
}

inline bool legacy_owner_identity(const char *className, const char *publisher,
                                  const char *bundleIdentifier) {
    (void)className; (void)publisher; (void)bundleIdentifier;
    return false;
}

inline bool builtin_owner_identity(const char *className, const char *publisher,
                                   const char *bundleIdentifier) {
    if (!className || (strcmp(className, "GarnetPrimaryFramebuffer") != 0 &&
                       strcmp(className, "AMDGarnetPrimaryFramebuffer") != 0))
        return false;
    return (publisher && strcmp(publisher, "org.garnet.driver") == 0) ||
           (bundleIdentifier && strcmp(bundleIdentifier, "org.garnet.driver") == 0);
}

// "EDID,AUX3" -> kBusAux / 3; "EDID,DDC2" -> kBusDdc / 2. Anything else is refused.
inline bool parse_edid_property(const char *name, Bus *bus, uint32_t *line) {
    if (!name || !bus || !line) return false;
    const char prefix[] = "EDID,";
    for (size_t i = 0u; i < sizeof(prefix) - 1u; ++i)
        if (name[i] != prefix[i]) return false;
    const char *p = name + sizeof(prefix) - 1u;
    Bus b = kBusNone;
    if (p[0] == 'A' && p[1] == 'U' && p[2] == 'X') { b = kBusAux; p += 3; }
    else if (p[0] == 'D' && p[1] == 'D' && p[2] == 'C') { b = kBusDdc; p += 3; }
    else return false;
    if (*p < '0' || *p > '9') return false;
    uint64_t n = 0u;
    unsigned digits = 0u;
    while (*p >= '0' && *p <= '9') {
        if (++digits > 9u) return false;
        n = n * 10u + static_cast<uint32_t>(*p - '0');
        ++p;
    }
    if (*p != '\0' || n > 0xFFFFFFFEull) return false;
    *bus = b;
    *line = static_cast<uint32_t>(n);
    return true;
}

struct Connector {
    uint32_t index;     // AtomBIOS order: diagnostics only, never an OTG/head source
    uint32_t type;
    uint32_t ddcLine;
    uint32_t hpdPin;
};

struct ConnectorList {
    uint32_t count;
    Connector c[kMaxConnectors];
};

enum Parse : uint32_t {
    kParseOk = 0u,
    kParseEmpty,
    kParseTooMany,
    kParseSyntax,
    kParseType,
    kParseNumber,
    kParseCount,
};

inline bool parse_uint(const char **pp, uint32_t *out) {
    const char *p = *pp;
    if (*p < '0' || *p > '9') return false;
    uint64_t n = 0u;
    unsigned digits = 0u;
    while (*p >= '0' && *p <= '9') {
        if (++digits > 9u) return false;
        n = n * 10u + static_cast<uint32_t>(*p - '0');
        ++p;
    }
    if (n > 0xFFFFFFFFull) return false;
    *pp = p;
    *out = static_cast<uint32_t>(n);
    return true;
}

inline bool parse_type(const char *p, size_t n, uint32_t *type) {
    struct Name { const char *s; size_t n; uint32_t t; };
    static const Name names[] = {
        { "DisplayPort", 11u, kTypeDp },
        { "HDMI-A", 6u, kTypeHdmiA },
        { "DVI-D", 5u, kTypeDviD },
        { "VGA", 3u, kTypeVga },
        { "USB-C", 5u, kTypeUsbC },
    };
    for (const Name &x : names) {
        if (x.n != n) continue;
        bool same = true;
        for (size_t i = 0u; i < n; ++i)
            if (p[i] != x.s[i]) { same = false; break; }
        if (same) { *type = x.t; return true; }
    }
    return false;
}

// The upstream diagnostic list: "DisplayPort/ddc0/hpd1,HDMI-A/ddc2/hpd3".
inline Parse parse_connectors(const char *text, ConnectorList *out) {
    if (!out) return kParseSyntax;
    ConnectorList l {};
    if (!text || !*text) { *out = l; return kParseEmpty; }
    const char *p = text;
    while (*p) {
        if (l.count >= kMaxConnectors) return kParseTooMany;
        const char *slash1 = strchr(p, '/');
        if (!slash1 || slash1 == p) return kParseSyntax;
        uint32_t type = kTypeUnknown;
        if (!parse_type(p, static_cast<size_t>(slash1 - p), &type)) return kParseType;
        const char *q = slash1 + 1;
        if (q[0] != 'd' || q[1] != 'd' || q[2] != 'c') return kParseSyntax;
        q += 3;
        uint32_t ddc = 0u, hpd = 0u;
        if (!parse_uint(&q, &ddc)) return kParseNumber;
        if (q[0] != '/' || q[1] != 'h' || q[2] != 'p' || q[3] != 'd') return kParseSyntax;
        q += 4;
        if (!parse_uint(&q, &hpd)) return kParseNumber;
        if (*q != ',' && *q != '\0') return kParseSyntax;
        Connector &c = l.c[l.count];
        c.index = l.count;
        c.type = type;
        c.ddcLine = ddc;
        c.hpdPin = hpd;
        ++l.count;
        if (*q == ',') {
            if (q[1] == '\0') return kParseSyntax;
            ++q;
        }
        p = q;
    }
    *out = l;
    return kParseOk;
}

// 0004's structured per-EDID descriptor. Plain 9 x uint32 little-endian POD.
struct ConnectorDesc {
    uint32_t version;
    uint32_t size;
    uint32_t connectorObjId;
    uint32_t encoderObjId;
    uint32_t ddcLine;
    uint32_t hpdPin;
    uint32_t type;
    uint32_t headOtg;
    uint32_t headProven;    // 1: headOtg is the lit boot pipe's OTG; 0: not claimed
};

static_assert(sizeof(ConnectorDesc) == kDescSize, "EDID connector descriptor is 36 bytes");

inline bool parse_desc(const uint8_t *bytes, size_t len, ConnectorDesc *out) {
    if (!bytes || !out || len != kDescSize) return false;
    uint32_t v[9] {};
    for (unsigned i = 0u; i < 9u; ++i) {
        const uint8_t *b = bytes + 4u * i;
        v[i] = static_cast<uint32_t>(b[0]) | (static_cast<uint32_t>(b[1]) << 8) |
               (static_cast<uint32_t>(b[2]) << 16) | (static_cast<uint32_t>(b[3]) << 24);
    }
    ConnectorDesc d { v[0], v[1], v[2], v[3], v[4], v[5], v[6], v[7], v[8] };
    if (d.version != kDescVersion) return false;
    *out = d;
    return true;
}

enum Verdict : uint32_t {
    kOk = 0u,
    kNoRoute,
    kNotNub,
    kNoConnectorList,
    kConnectorCount,
    kNoConnectorForLine,
    kAmbiguousLine,
    kHpdMissing,
    kUnknownType,
    kDescMissing,
    kDescBad,
    kDescMismatch,
    kPropertyBus,
    kPropertyLine,
    kPropertyMissing,
    kPropertyLength,
    kHeadConflict,
    kLegacyBus,
    kEdidBad,
    kEdidMismatch,
    kVerdictCount,
};

inline const char *verdict_name(uint32_t v) {
    switch (v) {
    case kOk: return "ok";
    case kNoRoute: return "no proven local route";
    case kNotNub: return "not a display-nub route";
    case kNoConnectorList: return "AtomBIOS connector list missing or empty";
    case kConnectorCount: return "AtomBIOS connector count disagrees with its list";
    case kNoConnectorForLine: return "no connector on this DDC/AUX line";
    case kAmbiguousLine: return "more than one connector on this DDC/AUX line";
    case kHpdMissing: return "connector has no proven HPD pin";
    case kUnknownType: return "connector type is not EDID-capable";
    case kDescMissing: return "EDID connector descriptor missing";
    case kDescBad: return "EDID connector descriptor malformed";
    case kDescMismatch: return "EDID connector descriptor disagrees with the connector list";
    case kPropertyBus: return "EDID property bus does not match the connector type";
    case kPropertyLine: return "EDID property line is not this route's DDC line";
    case kPropertyMissing: return "EDID property missing";
    case kPropertyLength: return "EDID property is neither 128 nor 256 bytes";
    case kHeadConflict: return "declared head/OTG is not this local route";
    case kLegacyBus: return "legacy framebuffer EDID must be an EDID,DDC<line> property";
    case kEdidBad: return "EDID header or checksum invalid";
    case kEdidMismatch: return "EDID block 0 differs from the local DDC read";
    default: return "unknown";
    }
}

// Strict association for the new upstream backend. `propLine` comes from the
// property NAME; the list and descriptor must independently agree with it.
inline uint32_t evaluate(const Route &r, const ConnectorList &list,
                         const ConnectorDesc *desc, bool descPresent,
                         Bus propBus, uint32_t propLine, uint32_t propLen) {
    if (!route_matches_geom(r)) return kNoRoute;
    if (!r.nub) return kNotNub;
    if (list.count == 0u || list.count > kMaxConnectors) return kNoConnectorList;
    const Connector *hit = nullptr;
    uint32_t hits = 0u;
    for (uint32_t i = 0u; i < list.count; ++i) {
        if (list.c[i].ddcLine == r.ddcLine) {
            hit = &list.c[i];
            ++hits;
        }
    }
    if (hits == 0u) return kNoConnectorForLine;
    if (hits != 1u) return kAmbiguousLine;
    if (hit->hpdPin == 0u) return kHpdMissing;
    Bus typeBus = kBusNone;
    if (!bus_for_type(hit->type, &typeBus)) return kUnknownType;
    if (!descPresent || !desc) return kDescMissing;
    if (desc->version != kDescVersion || desc->size != kDescSize ||
        desc->connectorObjId == 0u || desc->encoderObjId == 0u ||
        desc->headProven > 1u ||
        (desc->headProven == 0u && desc->headOtg != 0u))
        return kDescBad;
    if (desc->ddcLine != r.ddcLine || desc->hpdPin != hit->hpdPin ||
        desc->type != hit->type)
        return kDescMismatch;
    if (propBus != typeBus) return kPropertyBus;
    if (propLine != r.ddcLine) return kPropertyLine;
    if (propLen != 128u && propLen != 256u) return kPropertyLength;
    if (desc->headProven != 0u && desc->headOtg != r.otg) return kHeadConflict;
    return kOk;
}

// The pre-migration local RDNA4FB leaf publishes only EDID,DDC<line>. Keep that
// path working, but it is a different owner class and never gets the upstream
// conversion. The route still comes from the one local table.
inline uint32_t legacy_evaluate(const Route &r, Bus propBus, uint32_t propLine, uint32_t propLen) {
    if (!route_matches_geom(r)) return kNoRoute;
    if (!r.nub) return kNotNub;
    if (propBus != kBusDdc) return kLegacyBus;
    if (propLine != r.ddcLine) return kPropertyLine;
    if (propLen != 128u && propLen != 256u) return kPropertyLength;
    return kOk;
}

inline bool block_ok(const uint8_t *b) {
    return b && garnetdisp_edid_header_ok(b) && garnetdisp_edid_sum_ok(b);
}

inline uint32_t crosscheck(const uint8_t local[256], const uint8_t upstream[256]) {
    if (!local || !upstream) return kEdidBad;
    if (!block_ok(local) || !garnetdisp_edid_sum_ok(local + 128)) return kEdidBad;
    if (!block_ok(upstream)) return kEdidBad;
    if (memcmp(local, upstream, 128u) != 0) return kEdidMismatch;
    return kOk;
}

inline bool count_matches(uint32_t declared, const ConnectorList &list) {
    return declared != 0u && declared == list.count && list.count <= kMaxConnectors;
}

// GarnetDisplayNub::framebufferOf identity gate: one IOFramebuffer child of OUR
// nub, the nub's own ops table valid for the requested index and a snapshot
// carrying exactly that index. Anything else is refused.
enum FbVerdict : uint32_t {
    kFbOk = 0u,
    kFbNotOwned,
    kFbBadOps,
    kFbBadIndex,
    kFbNoSnapshot,
    kFbNoChild,
    kFbManyChildren,
    kFbBadClass,
};

inline FbVerdict framebuffer_verdict(bool ownNub, bool opsOk, bool indexOk,
                                     bool snapshotPublished, uint32_t children,
                                     bool classOk) {
    if (!ownNub) return kFbNotOwned;
    if (!opsOk) return kFbBadOps;
    if (!indexOk) return kFbBadIndex;
    if (!snapshotPublished) return kFbNoSnapshot;
    if (children == 0u) return kFbNoChild;
    if (children != 1u) return kFbManyChildren;
    if (!classOk) return kFbBadClass;
    return kFbOk;
}

inline const char *fb_verdict_name(uint32_t v) {
    switch (v) {
    case kFbOk: return "ok";
    case kFbNotOwned: return "not one of our nubs";
    case kFbBadOps: return "ops table invalid or wrong ABI";
    case kFbBadIndex: return "nub/snapshot index mismatch";
    case kFbNoSnapshot: return "snapshot not published";
    case kFbNoChild: return "no framebuffer child";
    case kFbManyChildren: return "more than one framebuffer child";
    case kFbBadClass: return "child is not our framebuffer class";
    default: return "unknown";
    }
}

}  // namespace rdna4edid
