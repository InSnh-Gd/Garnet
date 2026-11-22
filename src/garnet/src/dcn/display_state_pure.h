// display_state_pure.h - pure local RDNA4 display/device state and reservation contract.
//
// The display kext and GarnetDriver exchange one versioned POD request/reply
// through the local display peer contract (display_state_pure.h). This
// header stays free of IOKit types so the same validation and state rules run
// in host tests and in the kernel. It is not a public ABI and has no GRNT
// selector.
#pragma once

#include <stdint.h>

namespace rdna4peer {

constexpr uint32_t kVersion = 1u;
constexpr uint64_t kMiB = 1ull << 20;
constexpr uint64_t kFixedReserve = 24ull * kMiB;
constexpr uint64_t kSurfaceTail = 128ull;
constexpr uint32_t kMaxDisplayHead = 15u;
constexpr uint32_t kMaxDisplayOtg = 7u;

// Request.flags for kOpVramAccess: zero reads one dword, this bit writes one.
// The display never programs MM_INDEX/MM_INDEX_HI/MM_DATA itself; this endpoint
// is the only remote path into the local indexed window.

constexpr char kPropVersion[] = "Garnet,DisplayPeerVersion";
constexpr char kPropState[] = "Garnet,DisplayPeerState";
constexpr char kPropVendor[] = "Garnet,DisplayPeerVendor";
constexpr char kPropDevice[] = "Garnet,DisplayPeerDevice";
constexpr char kPropProvider[] = "Garnet,DisplayPeerProvider";
constexpr char kPropBar0Phys[] = "Garnet,DisplayPeerBAR0Phys";
constexpr char kPropBar0Size[] = "Garnet,DisplayPeerBAR0Size";
constexpr char kPropVramBytes[] = "Garnet,DisplayPeerVRAMBytes";
constexpr char kPropVramBase[] = "Garnet,DisplayPeerVRAMBase";
constexpr char kPropVramLimit[] = "Garnet,DisplayPeerVRAMLimit";
constexpr char kPropConsoleOffset[] = "Garnet,DisplayPeerConsoleOffset";
constexpr char kPropConsoleLength[] = "Garnet,DisplayPeerConsoleLength";
constexpr char kPropFixedOffset[] = "Garnet,DisplayPeerFixedOffset";
constexpr char kPropFixedLength[] = "Garnet,DisplayPeerFixedLength";
constexpr char kPropPoolOffset[] = "Garnet,DisplayPeerPoolOffset";
constexpr char kPropPoolLength[] = "Garnet,DisplayPeerPoolLength";
constexpr char kPropHoldOffset[] = "Garnet,DisplayPeerHoldOffset";
constexpr char kPropHoldLength[] = "Garnet,DisplayPeerHoldLength";
constexpr char kPropGrantPresent[] = "Garnet,DisplayPeerGrantPresent";
constexpr char kPropGrantOffset[] = "Garnet,DisplayPeerGrantOffset";
constexpr char kPropGrantLength[] = "Garnet,DisplayPeerGrantLength";
constexpr char kPropGrantOwner[] = "Garnet,DisplayPeerGrantOwner";
// Diagnostics only.  The endpoint above stays the authority; these properties
// exist so an offline runbook can read the primary/generation/reservation state
// without calling into the endpoint.  They never contain pointers.
constexpr char kPropStateName[] = "Garnet,DisplayPeerStateName";
constexpr char kPropStatus[] = "Garnet,DisplayPeerStatus";
constexpr char kPropGeneration[] = "Garnet,DisplayPeerGeneration";
constexpr char kPropActiveLeases[] = "Garnet,DisplayPeerActiveLeases";
constexpr char kPropPrimaryRegistered[] = "Garnet,DisplayPrimaryRegistered";
constexpr char kPropPrimaryConflict[] = "Garnet,DisplayPrimaryConflict";
constexpr char kPropPrimaryStatus[] = "Garnet,DisplayPrimaryStatus";
constexpr char kPropPrimaryEntryID[] = "Garnet,DisplayPrimaryEntryID";
constexpr char kPropPrimaryProvider[] = "Garnet,DisplayPrimaryProvider";
constexpr char kPropPrimaryGeneration[] = "Garnet,DisplayPrimaryGeneration";
constexpr char kPropPrimaryVendor[] = "Garnet,DisplayPrimaryVendor";
constexpr char kPropPrimaryDevice[] = "Garnet,DisplayPrimaryDevice";
constexpr char kPropPrimaryAsic[] = "Garnet,DisplayPrimaryAsic";
constexpr char kPropPrimaryDisplayIndex[] = "Garnet,DisplayPrimaryDisplayIndex";
constexpr char kPropPrimaryHead[] = "Garnet,DisplayPrimaryHead";
constexpr char kPropPrimaryOtg[] = "Garnet,DisplayPrimaryOTG";
constexpr char kPropPrimaryModeWidth[] = "Garnet,DisplayPrimaryModeWidth";
constexpr char kPropPrimaryModeHeight[] = "Garnet,DisplayPrimaryModeHeight";
constexpr char kPropPrimaryModeRowBytes[] = "Garnet,DisplayPrimaryModeRowBytes";
constexpr char kPropPrimaryModeDepth[] = "Garnet,DisplayPrimaryModeDepth";
constexpr char kPropPrimaryConsolePhys[] = "Garnet,DisplayPrimaryConsolePhys";
constexpr char kPropPrimaryConsoleOffset[] = "Garnet,DisplayPrimaryConsoleOffset";
constexpr char kPropPrimaryConsoleLength[] = "Garnet,DisplayPrimaryConsoleLength";
constexpr char kPropPrimaryConsoleWidth[] = "Garnet,DisplayPrimaryConsoleWidth";
constexpr char kPropPrimaryConsoleHeight[] = "Garnet,DisplayPrimaryConsoleHeight";
constexpr char kPropPrimaryConsoleRowBytes[] = "Garnet,DisplayPrimaryConsoleRowBytes";
constexpr char kPropPrimaryConsoleDepth[] = "Garnet,DisplayPrimaryConsoleDepth";

enum State : uint32_t {
    kStateStarting = 0,
    kStateReady = 1,
    kStateStopping = 2,
    kStateStopped = 3,
};

// Values intentionally match amdgpu::AsicId in amdgpu_asic.h. The kernel
// side has a static_assert for that mapping.
enum Asic : uint32_t {
    kAsicUnknown = 0,
    kAsicNavi48 = 1,
    kAsicNavi44 = 2,
};

enum LeaseKind : uint32_t {
    kLeaseDisplayInit = 1,
    kLeaseDisplayControl = 2,
    kLeaseDisplayStop = 3,
    kLeaseDisplaySurface = 4,
    kLeaseLocalDisplay = 5,
};

enum Reason : uint32_t {
    kOk = 0,
    kBadVersion = 1,
    kNotReady = 2,
    kStopping = 3,
    kStopped = 4,
    kIdentityMismatch = 5,
    kBarMismatch = 6,
    kBadRequest = 7,
    kBadRange = 8,
    kRangeOverflow = 9,
    kRangeOverlap = 10,
    kNoWorkspace = 11,
    kGrantAbsent = 12,
    kGrantOutsidePool = 13,
    kGrantTooSmall = 14,
    kBadSurface = 15,
    kSurfaceOverflow = 16,
    kOwnershipConflict = 17,
    kBadGeneration = 18,
    kBadCaller = 19,
    kPrimaryAbsent = 20,
    kPrimaryConflict = 21,
    kPrimaryMismatch = 22,
    kLeaseMissing = 23,
    kNoMemory = 24,
    kAsicMismatch = 25,
};

inline const char *reason_name(uint32_t r) {
    switch (r) {
    case kOk: return "ok";
    case kBadVersion: return "peer version mismatch";
    case kNotReady: return "peer not ready";
    case kStopping: return "peer stopping";
    case kStopped: return "peer stopped";
    case kIdentityMismatch: return "peer identity mismatch";
    case kBarMismatch: return "peer BAR0 mismatch";
    case kBadRequest: return "invalid peer request";
    case kBadRange: return "invalid resource range";
    case kRangeOverflow: return "resource range overflows";
    case kRangeOverlap: return "resource ranges overlap";
    case kNoWorkspace: return "no local workspace";
    case kGrantAbsent: return "no display-surface grant";
    case kGrantOutsidePool: return "display grant is outside the local pool";
    case kGrantTooSmall: return "display grant is too small";
    case kBadSurface: return "invalid display surface";
    case kSurfaceOverflow: return "display surface arithmetic overflows";
    case kOwnershipConflict: return "display transaction ownership conflict";
    case kBadGeneration: return "stale peer generation";
    case kBadCaller: return "caller is not a framebuffer on the peer GPU";
    case kPrimaryAbsent: return "no verified primary display";
    case kPrimaryConflict: return "a different primary display is registered";
    case kPrimaryMismatch: return "primary display does not match the resource snapshot";
    case kLeaseMissing: return "display lease token does not exist";
    case kNoMemory: return "display grant allocation failed";
    case kAsicMismatch: return "peer ASIC mismatch";
    default: return "?";
    }
}

inline const char *state_name(uint32_t state) {
    switch (state) {
    case kStateStarting: return "starting";
    case kStateReady: return "ready";
    case kStateStopping: return "stopping";
    case kStateStopped: return "stopped";
    default: return "unknown";
    }
}

struct Range {
    uint64_t offset;
    uint64_t length;
};

struct Grant {
    bool present;
    uint64_t offset;
    uint64_t length;
    uint64_t owner;
};

struct Snapshot {
    uint32_t version;
    uint32_t state;
    uint32_t asic;
    uint32_t displayIndex;
    uint16_t vendor;
    uint16_t device;
    uint64_t generation;
    uint64_t providerEntryID;
    uint64_t bar0Phys;
    uint64_t bar0Size;
    uint64_t vramBytes;
    uint64_t vramBase;
    uint64_t vramLimit;
    Range console;
    uint64_t consolePhysBase;
    uint32_t consoleWidth;
    uint32_t consoleHeight;
    uint32_t consoleRowBytes;
    uint32_t consoleDepth;
    Range fixed;
    Range pool;
    Range hold;
    Grant grant;
};

struct PrimaryView {
    uint32_t version;
    uint32_t asic;
    uint16_t vendor;
    uint16_t device;
    uint64_t generation;
    uint64_t providerEntryID;
    uint64_t fbEntryID;
    uint64_t bar0Phys;
    uint64_t bar0Size;
    uint64_t consolePhysBase;
    uint64_t consoleVramOffset;
    uint64_t consoleLength;
    uint32_t consoleWidth;
    uint32_t consoleHeight;
    uint32_t consoleRowBytes;
    uint32_t consoleDepth;
    uint32_t modeWidth;
    uint32_t modeHeight;
    uint32_t modeRowBytes;
    uint32_t modeDepth;
    uint32_t displayIndex;
    uint32_t head;
    uint32_t otg;
};

struct Expected {
    uint16_t vendor;
    uint16_t device;
    uint32_t asic;
    uint64_t providerEntryID;
    uint64_t bar0Phys;
    uint64_t bar0Size;
};

struct LayoutIn {
    uint64_t vramBytes;
    uint64_t vramBase;
    uint64_t vramLimit;
    uint64_t fixedLength;
    uint64_t holdLength;
    uint64_t consoleOffset;
    uint64_t consoleLength;
    bool haveConsole;
};

struct Layout {
    Range console;
    Range fixed;
    Range pool;
    Range hold;
};

struct Surface {
    bool valid;
    uint64_t physBase;
    uint32_t rowBytes;
    uint32_t width;
    uint32_t height;
};

// One lease at a time. The kernel protects this POD with one IOLock; the
// host concurrency test protects it with a mutex around the same functions.
struct GateState {
    uint32_t state;
    uint32_t activeLease;
    uint32_t activeDepth;
    uint32_t primaryRegistered;
    uint32_t primaryConflict;
    uint64_t generation;
    uint64_t nextToken;
    uint64_t activeToken;
    uint64_t activeFbEntryID;
    uint64_t activeOwner;
    uint32_t activeKind;
};

inline Grant no_grant() {
    Grant g {};
    g.present = false;
    return g;
}

inline bool add_ok(uint64_t a, uint64_t b, uint64_t &out) {
    if (a > ~0ull - b) return false;
    out = a + b;
    return true;
}

inline bool mul_ok(uint64_t a, uint64_t b, uint64_t &out) {
    if (a != 0 && b > ~0ull / a) return false;
    out = a * b;
    return true;
}

inline bool ranges_overlap(Range a, Range b) {
    uint64_t ae = 0, be = 0;
    if (!add_ok(a.offset, a.length, ae) || !add_ok(b.offset, b.length, be) ||
        a.length == 0 || b.length == 0)
        return false;
    return a.offset < be && b.offset < ae;
}

inline bool range_contains(Range outer, Range inner) {
    uint64_t oe = 0, ie = 0;
    return add_ok(outer.offset, outer.length, oe) && add_ok(inner.offset, inner.length, ie) &&
           outer.length != 0 && inner.length != 0 && inner.offset >= outer.offset && ie <= oe;
}

inline bool range_holds_dword(Range outer, uint64_t offset) {
    uint64_t end = 0;
    return add_ok(outer.offset, outer.length, end) && outer.length >= 4u &&
           offset >= outer.offset && offset <= end - 4u;
}

inline uint32_t build_layout(const LayoutIn &in, Layout &out) {
    out = Layout {};
    if (in.vramBytes == 0 || in.fixedLength == 0 || in.vramBase > in.vramLimit ||
        in.vramLimit > in.vramBytes || in.holdLength > in.vramBytes)
        return kBadRange;
    if (in.vramLimit <= in.vramBase || in.fixedLength >= in.vramLimit - in.vramBase)
        return kNoWorkspace;
    uint64_t fixedEnd = 0;
    if (!add_ok(in.vramBase, in.fixedLength, fixedEnd)) return kRangeOverflow;
    if (in.vramLimit <= fixedEnd) return kNoWorkspace;

    out.fixed.offset = in.vramBase;
    out.fixed.length = in.fixedLength;
    out.pool.offset = fixedEnd;
    out.pool.length = in.vramLimit - fixedEnd;
    out.hold.offset = in.vramBytes - in.holdLength;
    out.hold.length = in.holdLength;
    if (ranges_overlap(out.pool, out.hold)) return kRangeOverlap;

    if (in.haveConsole && in.consoleLength) {
        out.console.offset = in.consoleOffset;
        out.console.length = in.consoleLength;
        uint64_t ce = 0;
        if (!add_ok(out.console.offset, out.console.length, ce) || ce > in.vramBytes)
            return kRangeOverflow;
        if (ranges_overlap(out.console, out.fixed) || ranges_overlap(out.console, out.pool) ||
            ranges_overlap(out.console, out.hold))
            return kRangeOverlap;
        if (ce > in.vramLimit) return kBadRange;
    }
    return kOk;
}

inline uint32_t validate_identity(const Snapshot &s, const Expected &e) {
    if (s.version != kVersion) return kBadVersion;
    if (s.vendor != e.vendor || s.device != e.device) return kIdentityMismatch;
    // The publisher must always name its ASIC.  A caller that has not parsed
    // discovery yet may wildcard the expected ASIC: PCI identity and the BAR
    // are already provable from config space, which breaks the
    // discovery/peer chicken-and-egg.  The display's own identity table still
    // validates the ASIC before it trusts any display data.
    if (s.asic == kAsicUnknown) return kAsicMismatch;
    if (e.asic != kAsicUnknown && s.asic != e.asic) return kAsicMismatch;
    if (e.providerEntryID == 0 || s.providerEntryID == 0 ||
        s.providerEntryID != e.providerEntryID)
        return kIdentityMismatch;
    if (s.bar0Phys == 0 || s.bar0Size == 0 || s.bar0Phys != e.bar0Phys ||
        s.bar0Size != e.bar0Size)
        return kBarMismatch;
    return kOk;
}

inline uint32_t validate_snapshot_layout(const Snapshot &s) {
    if (s.vramBytes == 0 || s.vramBase > s.vramLimit || s.vramLimit > s.vramBytes)
        return kBadRange;
    if (s.bar0Size == 0 || s.vramLimit > s.bar0Size) return kBadRange;
    uint64_t barEnd = 0;
    if (!add_ok(s.bar0Phys, s.bar0Size, barEnd)) return kRangeOverflow;
    if (s.fixed.length == 0 || s.pool.length == 0) return kBadRange;

    uint64_t fixedEnd = 0, poolEnd = 0, holdEnd = 0;
    if (!add_ok(s.fixed.offset, s.fixed.length, fixedEnd) ||
        !add_ok(s.pool.offset, s.pool.length, poolEnd) ||
        !add_ok(s.hold.offset, s.hold.length, holdEnd))
        return kRangeOverflow;
    if (fixedEnd > s.vramBytes || poolEnd > s.vramBytes || holdEnd > s.vramBytes)
        return kRangeOverflow;
    if (s.fixed.offset != s.vramBase || s.pool.offset != fixedEnd ||
        poolEnd != s.vramLimit || s.hold.offset != s.vramBytes - s.hold.length)
        return kBadRange;
    if (ranges_overlap(s.fixed, s.pool) || ranges_overlap(s.fixed, s.hold) ||
        ranges_overlap(s.pool, s.hold))
        return kRangeOverlap;

    if (s.console.length == 0 || s.consoleWidth == 0 || s.consoleHeight == 0 ||
        s.consoleRowBytes == 0 || s.consoleDepth != 32)
        return kBadRange;
    uint64_t pixels = 0, consoleEnd = 0;
    if (!mul_ok(s.consoleRowBytes, s.consoleHeight, pixels)) return kSurfaceOverflow;
    // The declared console range has to hold the declared geometry.  A short
    // range is a bad layout, not an identity mismatch: keep the two refusal
    // classes separate so callers can tell a malformed snapshot from a
    // framebuffer that disagrees with a well-formed one.
    if (pixels > s.console.length) return kBadRange;
    if (!add_ok(s.console.offset, s.console.length, consoleEnd)) return kRangeOverflow;
    if (s.console.offset > s.bar0Size || consoleEnd > s.bar0Size ||
        consoleEnd > s.vramLimit || consoleEnd > s.vramBytes)
        return kBadRange;
    uint64_t consolePhys = 0, consolePhysEnd = 0;
    if (!add_ok(s.bar0Phys, s.console.offset, consolePhys) ||
        !add_ok(s.consolePhysBase, s.console.length, consolePhysEnd))
        return kRangeOverflow;
    if (s.consolePhysBase != consolePhys)
        return kPrimaryMismatch;
    if (ranges_overlap(s.console, s.fixed) || ranges_overlap(s.console, s.pool) ||
        ranges_overlap(s.console, s.hold))
        return kRangeOverlap;

    if (s.grant.present) {
        Range g { s.grant.offset, s.grant.length };
        if (!range_contains(s.pool, g)) return kGrantOutsidePool;
        if (s.grant.length < kSurfaceTail) return kGrantTooSmall;
    }
    return kOk;
}

inline uint32_t validate_peer(const Snapshot &s, const Expected &e, bool requireReady) {
    uint32_t why = validate_identity(s, e);
    if (why != kOk) return why;
    if (requireReady && s.state != kStateReady) {
        if (s.state == kStateStopping) return kStopping;
        if (s.state == kStateStopped) return kStopped;
        return kNotReady;
    }
    return validate_snapshot_layout(s);
}

inline uint32_t validate_primary(const Snapshot &s, const PrimaryView &p) {
    if (s.state != kStateReady) return s.state == kStateStopping ? kStopping : kNotReady;
    if (p.version != kVersion) return kBadVersion;
    if (p.generation != s.generation) return kBadGeneration;
    if (p.providerEntryID == 0 || p.providerEntryID != s.providerEntryID ||
        p.fbEntryID == 0)
        return kPrimaryMismatch;
    if (p.vendor != s.vendor || p.device != s.device || p.asic != s.asic)
        return kIdentityMismatch;
    if (p.bar0Phys != s.bar0Phys || p.bar0Size != s.bar0Size) return kBarMismatch;
    if (p.consolePhysBase != s.consolePhysBase ||
        p.consoleVramOffset != s.console.offset ||
        p.consoleLength != s.console.length ||
        p.consoleWidth != s.consoleWidth ||
        p.consoleHeight != s.consoleHeight ||
        p.consoleRowBytes != s.consoleRowBytes ||
        p.consoleDepth != s.consoleDepth)
        return kPrimaryMismatch;
    if (p.displayIndex != 0 || p.head != 0 || p.otg != 0) return kPrimaryMismatch;
    // The primary identity carries the mode the console actually scans out.
    // A smaller mode is not a harmless subset: identity consumers compare the
    // mode against the console range, so only exact equality is usable.
    if (p.modeWidth == 0 || p.modeHeight == 0 || p.modeRowBytes == 0 ||
        p.modeDepth != 32 || p.modeWidth != p.consoleWidth ||
        p.modeHeight != p.consoleHeight || p.modeRowBytes != p.consoleRowBytes)
        return kPrimaryMismatch;
    uint64_t modeBytes = 0;
    if (!mul_ok(p.modeRowBytes, p.modeHeight, modeBytes) ||
        modeBytes > p.consoleLength)
        return kSurfaceOverflow;
    return kOk;
}

inline uint32_t surface_from_validated_grant(const Snapshot &s, uint32_t width,
                                             uint32_t height, Surface &out) {
    out = Surface {};
    if (s.state != kStateReady)
        return s.state == kStateStopping ? kStopping :
               (s.state == kStateStopped ? kStopped : kNotReady);
    if (!s.grant.present) return kGrantAbsent;
    if (width == 0 || height == 0) return kBadSurface;
    uint64_t row = 0, bytes = 0;
    if (!mul_ok(width, 4ull, row) || row > 0xFFFFFFFFull)
        return kSurfaceOverflow;
    if (!mul_ok(row, height, bytes) || !add_ok(bytes, kSurfaceTail, bytes))
        return kSurfaceOverflow;
    if (s.grant.length < bytes) return kGrantTooSmall;
    uint64_t phys = 0, end = 0, barEnd = 0;
    if (!add_ok(s.bar0Phys, s.grant.offset, phys) ||
        !add_ok(phys, bytes, end) || !add_ok(s.bar0Phys, s.bar0Size, barEnd) ||
        end > barEnd)
        return kRangeOverflow;
    out.valid = true;
    out.physBase = phys;
    out.rowBytes = static_cast<uint32_t>(row);
    out.width = width;
    out.height = height;
    return kOk;
}

inline uint32_t surface_from_grant(const Snapshot &s, const Expected &e,
                                   uint32_t width, uint32_t height, Surface &out) {
    uint32_t why = validate_peer(s, e, true);
    if (why != kOk) {
        out = Surface {};
        return why;
    }
    return surface_from_validated_grant(s, width, height, out);
}

inline void gate_reset(GateState &g) {
    g = GateState {};
    g.state = kStateStarting;
}

inline uint32_t gate_ready(GateState &g) {
    if (g.state != kStateStarting) return kNotReady;
    g.generation++;
    if (g.generation == 0) g.generation = 1;
    g.primaryConflict = 0;
    g.state = kStateReady;
    return kOk;
}

inline uint32_t gate_begin_stop(GateState &g) {
    if (g.state == kStateStopping || g.state == kStateStopped) return kStopping;
    if (g.state != kStateReady && g.state != kStateStarting) return kNotReady;
    // STOPPING must not invalidate a lease that is already draining: its
    // display transaction keeps the generation it acquired at. New requests
    // are refused by the state check, and the generation advances once, in
    // gate_finish_stop, after the last lease is gone. A partial start never
    // owned a lease, so give its teardown a valid nonzero generation.
    if (g.generation == 0) g.generation = 1;
    g.state = kStateStopping;
    return kOk;
}

inline uint32_t gate_finish_stop(GateState &g) {
    if (g.state != kStateStopping) return kStopping;
    if (g.activeLease != 0) return kOwnershipConflict;
    g.generation++;
    if (g.generation == 0) g.generation = 1;
    g.state = kStateStopped;
    g.primaryRegistered = 0;
    g.primaryConflict = 0;
    return kOk;
}

inline bool gate_drained(const GateState &g) {
    return g.activeLease == 0;
}

// True when `owner` (a thread token) holds the lease at `generation`, and,
// when `fbEntryID` is nonzero, that lease belongs to exactly that framebuffer.
// STOPPING blocks new acquisitions but an already-held transaction keeps its
// permission until it releases: the stop path drains, it does not cut a
// half-finished display operation off in the middle. A conflicted primary
// keeps its old lease record but only a STOP teardown lease survives it.
inline bool gate_held(const GateState &g, uint64_t generation, uint64_t owner,
                      uint64_t fbEntryID) {
    const bool liveState = g.state == kStateReady || g.state == kStateStopping;
    if (!liveState || g.activeLease == 0) return false;
    if (g.generation != generation || g.activeOwner != owner) return false;
    if (g.primaryConflict && g.activeKind != kLeaseDisplayStop) return false;
    return fbEntryID == 0 || g.activeFbEntryID == fbEntryID;
}

// A non-init display lease belongs to the framebuffer that registered the
// primary. The bootstrap init lease is deliberately exempt: it exists to
// discover the console and pipe that the registration is then built from.
// No primary means no post-init remote lease yet.
inline bool lease_fb_is_primary(bool primaryRegistered, uint64_t primaryFbEntryID,
                                uint64_t requestFbEntryID, uint32_t kind) {
    if (kind == kLeaseDisplayInit) return true;
    return primaryRegistered && primaryFbEntryID != 0 &&
           primaryFbEntryID == requestFbEntryID;
}

inline uint32_t gate_acquire(GateState &g, uint64_t expectedGeneration,
                             uint64_t fbEntryID, uint32_t kind, uint64_t owner,
                             uint64_t &token) {
    token = 0;
    const bool stopOwner = kind == kLeaseDisplayStop &&
                           g.state == kStateStopping && g.activeLease == 0;
    if (g.state != kStateReady && !stopOwner)
        return g.state == kStateStopping ? kStopping :
               g.state == kStateStopped ? kStopped : kNotReady;
    if (expectedGeneration != g.generation) return kBadGeneration;
    if (fbEntryID == 0) return kBadCaller;
    if (kind < kLeaseDisplayInit || kind > kLeaseLocalDisplay) return kBadRequest;
    // A duplicate registration latches primaryConflict.  The old primary may
    // still be attached, but nobody may keep writing display state as if that
    // registration (or a mode change) had succeeded.  Only the drain-exclusive
    // stop lease is safe: it exists to shut the path down.
    if (g.primaryConflict && kind != kLeaseDisplayStop)
        return kPrimaryConflict;
    if (g.activeLease != 0) {
        if (owner != 0 && g.activeOwner == owner &&
            g.activeFbEntryID == fbEntryID && g.activeKind == kind) {
            g.activeDepth++;
            token = g.activeToken;
            return kOk;
        }
        return kOwnershipConflict;
    }
    g.nextToken++;
    if (g.nextToken == 0) g.nextToken = 1;
    g.activeLease = 1;
    g.activeDepth = 1;
    g.activeToken = g.nextToken;
    g.activeFbEntryID = fbEntryID;
    g.activeOwner = owner;
    g.activeKind = kind;
    token = g.activeToken;
    return kOk;
}

inline uint32_t gate_release(GateState &g, uint64_t token, uint64_t fbEntryID,
                             uint64_t expectedGeneration, uint64_t owner) {
    if (token == 0 || g.activeLease == 0 || token != g.activeToken)
        return kLeaseMissing;
    if (fbEntryID != 0 && fbEntryID != g.activeFbEntryID)
        return kBadCaller;
    if (expectedGeneration != 0 && expectedGeneration != g.generation)
        return kBadGeneration;
    if (owner != 0 && owner != g.activeOwner) return kOwnershipConflict;
    if (g.activeDepth > 1) {
        g.activeDepth--;
        return kOk;
    }
    g.activeDepth = 0;
    g.activeLease = 0;
    g.activeToken = 0;
    g.activeFbEntryID = 0;
    g.activeOwner = 0;
    g.activeKind = 0;
    return kOk;
}

inline uint32_t primary_register(GateState &g, PrimaryView &stored,
                                 const PrimaryView &candidate) {
    if (g.state != kStateReady)
        return g.state == kStateStopping ? kStopping : kNotReady;
    // The primary identity must not change under a running display
    // transaction: consumers hold the snapshot and would otherwise act on a
    // stale generation.
    if (g.activeLease != 0) return kOwnershipConflict;
    if (g.primaryConflict) return kPrimaryConflict;
    if (!g.primaryRegistered) {
        stored = candidate;
        g.primaryRegistered = 1;
        return kOk;
    }
    if (stored.version != candidate.version || stored.asic != candidate.asic ||
        stored.vendor != candidate.vendor || stored.device != candidate.device ||
        stored.generation != candidate.generation ||
        stored.providerEntryID != candidate.providerEntryID ||
        stored.fbEntryID != candidate.fbEntryID ||
        stored.bar0Phys != candidate.bar0Phys || stored.bar0Size != candidate.bar0Size ||
        stored.consolePhysBase != candidate.consolePhysBase ||
        stored.consoleVramOffset != candidate.consoleVramOffset ||
        stored.consoleLength != candidate.consoleLength ||
        stored.consoleWidth != candidate.consoleWidth ||
        stored.consoleHeight != candidate.consoleHeight ||
        stored.consoleRowBytes != candidate.consoleRowBytes ||
        stored.consoleDepth != candidate.consoleDepth ||
        stored.modeWidth != candidate.modeWidth ||
        stored.modeHeight != candidate.modeHeight ||
        stored.modeRowBytes != candidate.modeRowBytes ||
        stored.modeDepth != candidate.modeDepth ||
        stored.displayIndex != candidate.displayIndex ||
        stored.head != candidate.head || stored.otg != candidate.otg) {
        // The first primary stays latched, but no caller may treat a later
        // conflicting registration as a successful fallback to it.  A mode
        // or console change is just as dangerous as a different framebuffer:
        // never keep serving the old screen as if re-registration succeeded.
        g.primaryConflict = 1;
        return kPrimaryConflict;
    }
    return kOk;
}

// Local transaction entry validates the snapshot again under the peer lock.
// The caller's thread is already known locally, so this checks everything
// except remote caller ancestry.
inline uint32_t validate_local_acquire(const Snapshot &s, const GateState &g,
                                       uint64_t expectedGeneration,
                                       uint64_t fbEntryID, uint32_t kind,
                                       uint32_t head, uint32_t otg) {
    const bool stopOwner = kind == kLeaseDisplayStop &&
                           g.state == kStateStopping && s.state == kStateStopping;
    if ((!stopOwner && (g.state != kStateReady || s.state != kStateReady)) ||
        (stopOwner && g.activeLease != 0))
        return (g.state == kStateStopping || s.state == kStateStopping) ? kStopping :
               (g.state == kStateStopped || s.state == kStateStopped) ? kStopped :
               kNotReady;
    if (!s.generation || s.generation != g.generation ||
        expectedGeneration != g.generation)
        return kBadGeneration;
    if (fbEntryID == 0) return kBadCaller;
    if (kind < kLeaseDisplayInit || kind > kLeaseLocalDisplay) return kBadRequest;
    if (head > kMaxDisplayHead || otg > kMaxDisplayOtg) return kBadRequest;
    if (g.primaryConflict && kind != kLeaseDisplayStop)
        return kPrimaryConflict;
    if (stopOwner) {
        // A survey/partial start may never have produced a complete resource
        // table.  The local stop owner still has to unwind whatever it did
        // initialize.  Bind that unwind to the same generation/provider, but
        // do not make teardown depend on data and surface ranges it never
        // used.  Normal acquisitions still run the full layout validation.
        if (s.providerEntryID == 0 || fbEntryID != s.providerEntryID)
            return kBadCaller;
        return kOk;
    }
    const uint32_t why = validate_snapshot_layout(s);
    if (why != kOk) return why;
    return kOk;
}

// Kernel-only seam: a non-null caller of the private endpoint must be the
// Apple IONDRV framebuffer this display plugin extends under the target GPU.
// A bare IOService child of the GPU, or any other IOFramebuffer family, is not
// an owner of the display resources. The caller's registry ID still has to
// match the framebuffer named in the request separately.

// The display delegates every indexed VRAM access (MM_INDEX/MM_INDEX_HI/
// MM_DATA) to the local owner. That endpoint may only read/write while the
// calling framebuffer holds a display lease on this thread, so a remote indexed
// access is part of a lease-gated display transaction, not an unaudited probe.
// Local paths reach the same window through the local lock; the display never
// programs the window itself.

inline uint32_t primary_unregister(GateState &g, PrimaryView &stored,
                                   uint64_t fbEntryID, uint64_t generation) {
    if (g.activeLease != 0) return kOwnershipConflict;
    if (!g.primaryRegistered) return kPrimaryAbsent;
    if (fbEntryID != stored.fbEntryID) return kBadCaller;
    if (generation != stored.generation) return kBadGeneration;
    stored = PrimaryView {};
    g.primaryRegistered = 0;
    g.primaryConflict = 0;
    return kOk;
}

// `owner` is the querying thread token when the caller already holds a display
// lease.  Reading the primary identity mid-transaction is legal for that
// holder (it is how a consumer revalidates under its own lease), and refused
// for everyone else.  owner == 0 means "no identity to prove", so an active
// lease refuses rather than exposing a registration that may be changing.
inline uint32_t primary_query(const GateState &g, const PrimaryView &stored,
                              uint64_t expectedGeneration, PrimaryView &out,
                              uint64_t owner = 0) {
    if (g.state != kStateReady)
        return g.state == kStateStopping ? kStopping : kNotReady;
    if (g.primaryConflict) return kPrimaryConflict;
    if (g.activeLease != 0 && (owner == 0 || g.activeOwner != owner))
        return kOwnershipConflict;
    if (!g.primaryRegistered) return kPrimaryAbsent;
    if (expectedGeneration != 0 && expectedGeneration != g.generation)
        return kBadGeneration;
    if (stored.generation != g.generation) return kBadGeneration;
    out = stored;
    return kOk;
}

// Display-first startup.  The display plugin may attach before GarnetDriver has
// published READY (or before it has even registered its service).  A failed
// probe never becomes a permanent refusal: the state stays WAITING and the
// caller retries from its next IOKit entry point / display transaction.
enum AttachPhase : uint32_t {
    kAttachWaiting = 0,
    kAttachRegistered = 1,
    kAttachStopped = 2,
};

struct AttachState {
    uint32_t phase;
    uint32_t attempts;
    uint32_t lastStatus;
};

inline void attach_reset(AttachState &a) {
    a = AttachState {};
    a.phase = kAttachWaiting;
}

inline uint32_t attach_note(AttachState &a, uint32_t status) {
    a.attempts++;
    a.lastStatus = status;
    if (a.phase == kAttachStopped) return kStopped;
    if (status == kOk) {
        a.phase = kAttachRegistered;
        return kOk;
    }
    a.phase = kAttachWaiting;
    return a.lastStatus;
}

inline void attach_stop(AttachState &a) {
    a.phase = kAttachStopped;
}

inline bool attach_should_retry(const AttachState &a) {
    return a.phase == kAttachWaiting;
}

} // namespace rdna4peer
