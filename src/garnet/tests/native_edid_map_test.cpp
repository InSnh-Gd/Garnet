// Host checks for the display-nub EDID/head adapter. No kernel, VM or GPU access.
#include <cstdint>
#include <cstddef>
#include <cstdio>
#include <cstring>

#include "amd/native_edid_map_pure.h"

using namespace rdna4edid;

static int gRun, gFail;
static void expect(bool ok, const char *what) {
    gRun++;
    if (!ok) { gFail++; std::printf("FAIL: %s\n", what); }
}
static void expect_u(const char *what, uint64_t got, uint64_t want) {
    gRun++;
    if (got != want) {
        gFail++;
        std::printf("FAIL: %s: got %#llx want %#llx\n", what,
                    static_cast<unsigned long long>(got),
                    static_cast<unsigned long long>(want));
    }
}

static void fill_edid(uint8_t e[256], uint8_t marker = 0x5a) {
    std::memset(e, 0, 256);
    e[0] = 0x00; e[1] = 0xff; e[2] = 0xff; e[3] = 0xff;
    e[4] = 0xff; e[5] = 0xff; e[6] = 0xff; e[7] = 0x00;
    e[8] = 0x10; e[9] = 0xac; e[10] = marker; e[11] = 0x40;
    e[126] = 1;
    uint32_t sum = 0;
    for (unsigned i = 0; i < 127; ++i) sum += e[i];
    e[127] = static_cast<uint8_t>(0u - sum);
    e[128] = 0x02;
    sum = 0;
    for (unsigned i = 128; i < 255; ++i) sum += e[i];
    e[255] = static_cast<uint8_t>(0u - sum);
}

static void put_u32(uint8_t *p, uint32_t v) {
    p[0] = static_cast<uint8_t>(v);
    p[1] = static_cast<uint8_t>(v >> 8);
    p[2] = static_cast<uint8_t>(v >> 16);
    p[3] = static_cast<uint8_t>(v >> 24);
}

static ConnectorDesc desc(uint32_t type, uint32_t ddc, uint32_t hpd,
                          uint32_t headOtg = 0, uint32_t headProven = 0) {
    ConnectorDesc d {};
    d.version = kDescVersion;
    d.size = kDescSize;
    d.connectorObjId = 0x3100 | type;
    d.encoderObjId = 0x2100 | type;
    d.ddcLine = ddc;
    d.hpdPin = hpd;
    d.type = type;
    d.headOtg = headOtg;
    d.headProven = headProven;
    return d;
}

static ConnectorList list(const char *text) {
    ConnectorList l {};
    const Parse p = parse_connectors(text, &l);
    expect_u("parse_connectors: test input", p, kParseOk);
    return l;
}

static void routes() {
    Route r {};
    expect(route_for_inst(0, &r) && !r.nub && r.index == 0u && r.otg == 0u &&
           r.ddcLine == kDdcNone, "route: instance 0 is the DP console display, OTG0, no nub");
    expect(route_for_inst(1, &r) && r.nub && r.index == 2u && r.otg == 1u &&
           r.ddcLine == 2u && r.w == 1920u && r.h == 1080u,
           "route: instance 1 is monitor A, index 2, OTG1, DDC2, 1920x1080");
    expect(route_for_inst(2, &r) && r.nub && r.index == 1u && r.otg == 2u &&
           r.ddcLine == 3u && r.w == 2560u && r.h == 1440u,
           "route: instance 2 is monitor B, index 1, OTG2, DDC3, 2560x1440");
    expect(!route_for_inst(3, &r), "route: an unknown instance is refused");
    expect(route_for_index(1, &r) && r.inst == 2u && r.otg == 2u,
           "route: index 1 maps back to instance 2");
    expect(route_for_index(2, &r) && r.inst == 1u && r.otg == 1u,
           "route: index 2 maps back to instance 1");
    expect(!route_for_index(0, &r) && !route_for_index(3, &r),
           "route: the DP/unknown display indices have no nub route");
    expect(route_for_ddc(2, &r) && r.inst == 1u, "route: DDC2 belongs to instance 1");
    expect(route_for_ddc(3, &r) && r.inst == 2u, "route: DDC3 belongs to instance 2");
    expect(!route_for_ddc(0, &r) && !route_for_ddc(1, &r) && !route_for_ddc(4, &r),
           "route: other DDC lines are not local nub routes");

    GarnetDispGeom g;
    expect(garnetdisp_geom_for_index(GARNET_DISPA_INDEX, &g) &&
           g.inst == GARNET_DISP_INST_MONA && g.otg == GARNET_DISPA_OTG &&
           g.ddcLine == GARNET_DISPA_DDC_LINE && g.w == GARNET_DISPA_W &&
           g.h == GARNET_DISPA_H, "route: monitor A agrees with the shared geometry table");
    expect(garnetdisp_geom_for_index(GARNET_DISP_INDEX, &g) &&
           g.inst == GARNET_DISP_INST_MONB && g.otg == GARNET_DISP_OTG &&
           g.ddcLine == GARNET_DISP_DDC_LINE && g.w == GARNET_DISP_W &&
           g.h == GARNET_DISP_H, "route: monitor B agrees with the shared geometry table");

    Route bad {};
    expect(!route_valid(bad), "route: the default route is invalid");
    bad = Route { 1u, 2u, 1u, 2u, 1920u, 1080u, true };
    expect(route_valid(bad) && route_matches_geom(bad), "route: a complete nub route is valid");
    bad.otg = 2u;
    expect(route_valid(bad) && !route_matches_geom(bad),
           "route: a structurally valid route with the wrong OTG is still refused");
}

static void properties() {
    Bus b = kBusNone;
    uint32_t line = 0;
    expect(parse_edid_property("EDID,AUX3", &b, &line) && b == kBusAux && line == 3u,
           "property: EDID,AUX3");
    expect(parse_edid_property("EDID,DDC2", &b, &line) && b == kBusDdc && line == 2u,
           "property: EDID,DDC2");
    expect(!parse_edid_property("EDID,AUX", &b, &line) &&
           !parse_edid_property("EDID,UDC3", &b, &line) &&
           !parse_edid_property("EDID,AUX3x", &b, &line) &&
           !parse_edid_property("EDID,DDC-1", &b, &line) &&
           !parse_edid_property("EDID,AUX1234567890", &b, &line) &&
           !parse_edid_property(nullptr, &b, &line),
           "property: malformed names are refused");
    expect(std::strcmp(bus_name(kBusAux), "AUX") == 0 &&
           std::strcmp(bus_name(kBusDdc), "DDC") == 0 &&
           std::strcmp(bus_name(kBusNone), "") == 0, "property: bus names");

    expect(!local_leaf_name("RDNA4FB") && !local_leaf_name("AMDRDNA4FB") &&
           !local_leaf_name("IONDRVFramebuffer") && !local_leaf_name("IOFramebuffer") &&
           !local_leaf_name("AMDFramebuffer") && !local_leaf_name(nullptr),
           "legacy owner: retired external leaf names are refused");
    expect(!legacy_owner_identity("RDNA4FB", "com.hackintosh.RDNA4FB", nullptr) &&
           !legacy_owner_identity("AMDRDNA4FB", nullptr, "com.hackintosh.RDNA4FB") &&
           !legacy_owner_identity("RDNA4FB", "com.hackintosh.RDNA4FB", "com.hackintosh.RDNA4FB"),
           "legacy owner: retired external RDNA4FB bundle identity is refused");
    expect(builtin_owner_identity("GarnetPrimaryFramebuffer", "org.garnet.driver", nullptr) &&
           builtin_owner_identity("AMDGarnetPrimaryFramebuffer", nullptr, "org.garnet.driver") &&
           builtin_owner_identity("GarnetPrimaryFramebuffer", "org.garnet.driver", "org.garnet.driver"),
           "builtin owner: GarnetPrimaryFramebuffer plus org.garnet.driver publisher/bundle identity");
    expect(!builtin_owner_identity("IONDRVFramebuffer", "org.garnet.driver", nullptr) &&
           !builtin_owner_identity("GarnetPrimaryFramebuffer", "com.apple.iokit.IONDRVSupport", nullptr) &&
           !builtin_owner_identity("GarnetPrimaryFramebuffer", nullptr, "com.apple.iokit.IONDRVSupport") &&
           !builtin_owner_identity("GarnetPrimaryFramebuffer", nullptr, nullptr) &&
           !builtin_owner_identity(nullptr, "org.garnet.driver", nullptr),
           "builtin owner: generic/missing identity is refused (no marker means no fallback)");

    Bus typeBus = kBusNone;
    expect(bus_for_type(kTypeDp, &typeBus) && typeBus == kBusAux &&
           bus_for_type(kTypeUsbC, &typeBus) && typeBus == kBusAux &&
           bus_for_type(kTypeHdmiA, &typeBus) && typeBus == kBusDdc &&
           bus_for_type(kTypeDviD, &typeBus) && typeBus == kBusDdc,
           "property: DP/USB-C are AUX and HDMI/DVI are DDC");
    expect(!bus_for_type(kTypeVga, &typeBus) && !bus_for_type(kTypeUnknown, &typeBus),
           "property: VGA/unknown do not carry an EDID bus");
}

static void connectors() {
    expect_u("descriptor: size", sizeof(ConnectorDesc), kDescSize);
    expect_u("descriptor: ddcLine offset", offsetof(ConnectorDesc, ddcLine), 16u);
    expect_u("descriptor: hpdPin offset", offsetof(ConnectorDesc, hpdPin), 20u);
    expect_u("descriptor: type offset", offsetof(ConnectorDesc, type), 24u);
    expect_u("descriptor: headOtg offset", offsetof(ConnectorDesc, headOtg), 28u);
    expect_u("descriptor: headProven offset", offsetof(ConnectorDesc, headProven), 32u);
    ConnectorList l {};
    expect_u("connectors: empty", parse_connectors("", &l), kParseEmpty);
    expect_u("connectors: null", parse_connectors(nullptr, &l), kParseEmpty);
    expect_u("connectors: the 7590-shaped list", parse_connectors(
                 "DisplayPort/ddc0/hpd1,DisplayPort/ddc1/hpd2,HDMI-A/ddc2/hpd3", &l), kParseOk);
    expect_u("connectors: count", l.count, 3u);
    expect_u("connectors: first type", l.c[0].type, kTypeDp);
    expect_u("connectors: second line", l.c[1].ddcLine, 1u);
    expect_u("connectors: third type", l.c[2].type, kTypeHdmiA);
    expect_u("connectors: third HPD", l.c[2].hpdPin, 3u);
    expect_u("connectors: order is recorded", l.c[2].index, 2u);
    expect_u("connectors: syntax", parse_connectors("DisplayPort/ddc0/hpd1,", &l), kParseSyntax);
    expect_u("connectors: missing slash", parse_connectors("DisplayPort", &l), kParseSyntax);
    expect_u("connectors: missing ddc prefix", parse_connectors("DisplayPort/x0/hpd1", &l), kParseSyntax);
    expect_u("connectors: missing hpd prefix", parse_connectors("DisplayPort/ddc0/x1", &l), kParseSyntax);
    expect_u("connectors: unknown type", parse_connectors("eDP/ddc0/hpd1", &l), kParseType);
    expect_u("connectors: bad number", parse_connectors("DisplayPort/ddcx/hpd1", &l), kParseNumber);
    expect_u("connectors: too many", parse_connectors(
                 "DisplayPort/ddc0/hpd1,DisplayPort/ddc1/hpd2,DisplayPort/ddc2/hpd3,"
                 "DisplayPort/ddc3/hpd4,DisplayPort/ddc4/hpd5,DisplayPort/ddc5/hpd6,"
                 "DisplayPort/ddc6/hpd7,DisplayPort/ddc7/hpd8,DisplayPort/ddc8/hpd9", &l),
             kParseTooMany);

    uint8_t d[kDescSize] {};
    const ConnectorDesc in = desc(kTypeDp, 3u, 4u, 2u, 1u);
    put_u32(d + 0, in.version); put_u32(d + 4, in.size);
    put_u32(d + 8, in.connectorObjId); put_u32(d + 12, in.encoderObjId);
    put_u32(d + 16, in.ddcLine); put_u32(d + 20, in.hpdPin);
    put_u32(d + 24, in.type); put_u32(d + 28, in.headOtg);
    put_u32(d + 32, in.headProven);
    ConnectorDesc out {};
    expect(parse_desc(d, sizeof d, &out) && out.version == 1u && out.size == kDescSize &&
           out.connectorObjId == in.connectorObjId && out.ddcLine == 3u &&
           out.hpdPin == 4u && out.type == kTypeDp && out.headOtg == 2u &&
           out.headProven == 1u, "descriptor: little-endian round trip");
    expect(!parse_desc(d, sizeof d - 1u, &out) && !parse_desc(nullptr, sizeof d, &out),
           "descriptor: wrong length/NULL refused");
    uint8_t bad[kDescSize] {};
    std::memcpy(bad, d, sizeof bad);
    put_u32(bad, 2u);
    expect(!parse_desc(bad, sizeof bad, &out), "descriptor: wrong version refused");
}

static void association() {
    Route monb {}, mona {}, dp {};
    expect(route_for_inst(2, &monb) && route_for_inst(1, &mona) && route_for_inst(0, &dp),
           "association: routes");

    ConnectorList monbList = list("DisplayPort/ddc3/hpd4");
    ConnectorList mona7590 = list("DisplayPort/ddc0/hpd1,DisplayPort/ddc1/hpd2,HDMI-A/ddc2/hpd3");
    ConnectorDesc monbDesc = desc(kTypeDp, 3u, 4u, 2u, 1u);
    ConnectorDesc monaDesc = desc(kTypeHdmiA, 2u, 3u, 0u, 0u);

    expect_u("association: monitor B, proven AUX connector and head",
             evaluate(monb, monbList, &monbDesc, true, kBusAux, 3u, 256u), kOk);
    expect_u("association: monitor A on the 7590 HDMI-A/ddc2 connector",
             evaluate(mona, mona7590, &monaDesc, true, kBusDdc, 2u, 128u), kOk);
    expect_u("association: monitor B has no ddc3 connector in the 7590 layout",
             evaluate(monb, mona7590, &monbDesc, true, kBusAux, 3u, 256u), kNoConnectorForLine);
    expect_u("association: the DP console instance has no nub route",
             evaluate(dp, monbList, &monbDesc, true, kBusAux, 3u, 256u), kNotNub);
    Route forged = mona;
    forged.otg = 2u;
    expect_u("association: a forged route/OTG is refused",
             evaluate(forged, mona7590, &monaDesc, true, kBusDdc, 2u, 128u), kNoRoute);
    expect_u("association: empty connector list",
             evaluate(monb, ConnectorList {}, &monbDesc, true, kBusAux, 3u, 256u), kNoConnectorList);

    ConnectorList ambiguous = list("DisplayPort/ddc3/hpd4,HDMI-A/ddc3/hpd5");
    ConnectorDesc amb = desc(kTypeHdmiA, 3u, 5u);
    expect_u("association: two connectors sharing the line are ambiguous",
             evaluate(monb, ambiguous, &amb, true, kBusDdc, 3u, 128u), kAmbiguousLine);

    ConnectorList noHpd = list("DisplayPort/ddc3/hpd0");
    expect_u("association: HPD0 is refused",
             evaluate(monb, noHpd, &monbDesc, true, kBusAux, 3u, 256u), kHpdMissing);
    ConnectorList vga = list("VGA/ddc3/hpd4");
    ConnectorDesc vgaDesc = desc(kTypeVga, 3u, 4u);
    expect_u("association: VGA is not an EDID connector",
             evaluate(monb, vga, &vgaDesc, true, kBusDdc, 3u, 128u), kUnknownType);

    expect_u("association: missing descriptor",
             evaluate(monb, monbList, nullptr, false, kBusAux, 3u, 256u), kDescMissing);
    ConnectorDesc bad = monbDesc;
    bad.version = 2u;
    expect_u("association: descriptor version", evaluate(monb, monbList, &bad, true, kBusAux, 3u, 256u), kDescBad);
    bad = monbDesc; bad.connectorObjId = 0u;
    expect_u("association: descriptor connector object", evaluate(monb, monbList, &bad, true, kBusAux, 3u, 256u), kDescBad);
    bad = monbDesc; bad.encoderObjId = 0u;
    expect_u("association: descriptor encoder object", evaluate(monb, monbList, &bad, true, kBusAux, 3u, 256u), kDescBad);
    bad = monbDesc; bad.headProven = 2u;
    expect_u("association: descriptor head flag", evaluate(monb, monbList, &bad, true, kBusAux, 3u, 256u), kDescBad);
    bad = monbDesc; bad.headProven = 0u; bad.headOtg = 2u;
    expect_u("association: an unclaimed head must not carry an OTG",
             evaluate(monb, monbList, &bad, true, kBusAux, 3u, 256u), kDescBad);
    bad = monbDesc; bad.ddcLine = 2u;
    expect_u("association: descriptor line mismatch", evaluate(monb, monbList, &bad, true, kBusAux, 3u, 256u), kDescMismatch);
    bad = monbDesc; bad.hpdPin = 5u;
    expect_u("association: descriptor HPD mismatch", evaluate(monb, monbList, &bad, true, kBusAux, 3u, 256u), kDescMismatch);
    bad = monbDesc; bad.type = kTypeHdmiA;
    expect_u("association: descriptor type mismatch", evaluate(monb, monbList, &bad, true, kBusAux, 3u, 256u), kDescMismatch);

    expect_u("association: AUX property for an HDMI connector",
             evaluate(mona, mona7590, &monaDesc, true, kBusAux, 2u, 128u), kPropertyBus);
    expect_u("association: property names the other line",
             evaluate(monb, monbList, &monbDesc, true, kBusAux, 2u, 256u), kPropertyLine);
    expect_u("association: property length",
             evaluate(monb, monbList, &monbDesc, true, kBusAux, 3u, 64u), kPropertyLength);
    expect_u("association: a declared head equal to this route",
             evaluate(monb, monbList, &monbDesc, true, kBusAux, 3u, 256u), kOk);
    ConnectorDesc wrongHead = desc(kTypeDp, 3u, 4u, 1u, 1u);
    expect_u("association: a proven wrong head/OTG is refused",
             evaluate(monb, monbList, &wrongHead, true, kBusAux, 3u, 256u), kHeadConflict);
    ConnectorDesc noHeadClaim = desc(kTypeDp, 3u, 4u, 0u, 0u);
    expect_u("association: no head claim does not fabricate one",
             evaluate(monb, monbList, &noHeadClaim, true, kBusAux, 3u, 256u), kOk);

    // The connector's position in the AtomBIOS list is never an OTG source.
    ConnectorList reordered = list("HDMI-A/ddc2/hpd3,DisplayPort/ddc0/hpd1,DisplayPort/ddc3/hpd4");
    expect_u("association: connector order does not change monitor B's line/OTG",
             evaluate(monb, reordered, &monbDesc, true, kBusAux, 3u, 256u), kOk);
    expect_u("association: connector order does not change monitor A's line/OTG",
             evaluate(mona, reordered, &monaDesc, true, kBusDdc, 2u, 128u), kOk);

    expect(count_matches(1u, monbList) && count_matches(3u, mona7590) &&
           !count_matches(0u, monbList) && !count_matches(2u, monbList),
           "association: declared connector count must equal the parsed list");

    expect_u("legacy: EDID,DDC3 for monitor B", legacy_evaluate(monb, kBusDdc, 3u, 128u), kOk);
    expect_u("legacy: EDID,AUX3 is not the local property", legacy_evaluate(monb, kBusAux, 3u, 128u), kLegacyBus);
    expect_u("legacy: the other line is refused", legacy_evaluate(monb, kBusDdc, 2u, 128u), kPropertyLine);
    expect_u("legacy: short property refused", legacy_evaluate(monb, kBusDdc, 3u, 64u), kPropertyLength);
}

static void edid_crosscheck() {
    uint8_t a[256], b[256];
    fill_edid(a);
    std::memcpy(b, a, sizeof b);
    expect_u("EDID: equal block 0", crosscheck(a, b), kOk);
    fill_edid(b, 0x5b);
    expect_u("EDID: different product/serial bytes", crosscheck(a, b), kEdidMismatch);
    std::memcpy(b, a, sizeof b);
    b[40] ^= 1u;
    expect_u("EDID: bad upstream checksum", crosscheck(a, b), kEdidBad);
    std::memcpy(b, a, sizeof b);
    b[0] = 1u;
    expect_u("EDID: bad upstream header", crosscheck(a, b), kEdidBad);
    fill_edid(b);
    b[200] ^= 1u;
    expect_u("EDID: local block 1 checksum", crosscheck(b, a), kEdidBad);
    expect_u("EDID: NULL", crosscheck(nullptr, a), kEdidBad);
}

static void framebuffer_gate() {
    expect_u("framebuffer: unique own child", framebuffer_verdict(true, true, true, true, 1u, true), kFbOk);
    expect_u("framebuffer: not our nub", framebuffer_verdict(false, true, true, true, 1u, true), kFbNotOwned);
    expect_u("framebuffer: bad ops", framebuffer_verdict(true, false, true, true, 1u, true), kFbBadOps);
    expect_u("framebuffer: wrong index", framebuffer_verdict(true, true, false, true, 1u, true), kFbBadIndex);
    expect_u("framebuffer: no snapshot", framebuffer_verdict(true, true, true, false, 1u, true), kFbNoSnapshot);
    expect_u("framebuffer: no child", framebuffer_verdict(true, true, true, true, 0u, false), kFbNoChild);
    expect_u("framebuffer: two children", framebuffer_verdict(true, true, true, true, 2u, false), kFbManyChildren);
    expect_u("framebuffer: foreign child class", framebuffer_verdict(true, true, true, true, 1u, false), kFbBadClass);

    // The single decision, over every combination of its inputs. The order is
    // part of the contract: ownership, ops, index, snapshot, child count, class.
    unsigned matrix = 0;
    for (unsigned children = 0u; children <= 2u; ++children) {
        for (unsigned bits = 0u; bits < 32u; ++bits) {
            const bool own = (bits & 1u) != 0u;
            const bool ops = (bits & 2u) != 0u;
            const bool index = (bits & 4u) != 0u;
            const bool snapshot = (bits & 8u) != 0u;
            const bool klass = (bits & 16u) != 0u;
            uint32_t want = kFbOk;
            if (!own) want = kFbNotOwned;
            else if (!ops) want = kFbBadOps;
            else if (!index) want = kFbBadIndex;
            else if (!snapshot) want = kFbNoSnapshot;
            else if (children == 0u) want = kFbNoChild;
            else if (children != 1u) want = kFbManyChildren;
            else if (!klass) want = kFbBadClass;
            expect_u("framebuffer: full verdict matrix", framebuffer_verdict(own, ops, index, snapshot, children, klass), want);
            ++matrix;
        }
    }
    expect_u("framebuffer: matrix covered every combination", matrix, 96u);
}

static void verdict_names() {
    for (uint32_t v = 0u; v < kVerdictCount; ++v)
        expect(verdict_name(v) != nullptr && *verdict_name(v) != '\0',
               "verdict: every EDID verdict has text");
    for (uint32_t v = 0u; v <= kFbBadClass; ++v)
        expect(fb_verdict_name(v) != nullptr && *fb_verdict_name(v) != '\0',
               "verdict: every framebuffer verdict has text");
}

int main() {
    routes();
    properties();
    connectors();
    association();
    edid_crosscheck();
    framebuffer_gate();
    verdict_names();
    std::printf("%s: %d/%d checks passed\n", gFail ? "FAIL" : "PASS", gRun - gFail, gRun);
    return gFail ? 1 : 0;
}
