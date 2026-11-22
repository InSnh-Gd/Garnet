// Behavioral host tests for the RDNA4 display/device resource contract.
// No kernel, VM, GPU, or source-text inspection is involved.
#include <atomic>
#include <cstdio>
#include <cstdint>
#include <mutex>
#include <thread>
#include <vector>

#include "dcn/display_state_pure.h"

using namespace rdna4peer;

static int gRun;
static int gFail;

static void expect(bool ok, const char *what) {
    gRun++;
    if (!ok) {
        gFail++;
        std::printf("FAIL: %s\n", what);
    }
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

static constexpr uint64_t MiB = 1ull << 20;
static constexpr uint64_t kVram = 16304ull * MiB;
static constexpr uint64_t kBarPhys = 0xD0000000ull;
static constexpr uint64_t kBarSize = 256ull * MiB;
static constexpr uint64_t kBase = 15ull * MiB;
static constexpr uint64_t kLimit = kBarSize;
static constexpr uint64_t kWidth = 1920;
static constexpr uint64_t kHeight = 1080;
static constexpr uint64_t kRowBytes = kWidth * 4;
static constexpr uint64_t kConsoleBytes = kRowBytes * kHeight;
static constexpr uint64_t kProvider = 0x1234;
static constexpr uint64_t kFramebuffer = 0x5678;

static Layout make_layout() {
    Layout layout {};
    LayoutIn in { kVram, kBase, kLimit, kFixedReserve, 768ull * MiB,
                  0, kConsoleBytes, true };
    const uint32_t status = build_layout(in, layout);
    expect_u("layout: standard reservation table", status, kOk);
    return layout;
}

static Snapshot snapshot(bool withGrant = false) {
    const Layout layout = make_layout();
    Snapshot s {};
    s.version = kVersion;
    s.state = kStateReady;
    s.asic = kAsicNavi44;
    s.displayIndex = 0;
    s.vendor = 0x1002;
    s.device = 0x7590;
    s.generation = 1;
    s.providerEntryID = kProvider;
    s.bar0Phys = kBarPhys;
    s.bar0Size = kBarSize;
    s.vramBytes = kVram;
    s.vramBase = kBase;
    s.vramLimit = kLimit;
    s.console = layout.console;
    s.consolePhysBase = kBarPhys + s.console.offset;
    s.consoleWidth = static_cast<uint32_t>(kWidth);
    s.consoleHeight = static_cast<uint32_t>(kHeight);
    s.consoleRowBytes = static_cast<uint32_t>(kRowBytes);
    s.consoleDepth = 32;
    s.fixed = layout.fixed;
    s.pool = layout.pool;
    s.hold = layout.hold;
    s.grant = withGrant ? Grant { true, layout.pool.offset, 16 * MiB, 7 } : no_grant();
    return s;
}

static Expected expected() {
    return Expected { 0x1002, 0x7590, kAsicNavi44, kProvider, kBarPhys, kBarSize };
}

static PrimaryView make_primary_view(const Snapshot &s, uint32_t modeWidth = kWidth,
                                     uint32_t modeHeight = kHeight,
                                     uint32_t modeRowBytes = kRowBytes) {
    PrimaryView p {};
    p.version = kVersion;
    p.asic = s.asic;
    p.vendor = s.vendor;
    p.device = s.device;
    p.generation = s.generation;
    p.providerEntryID = s.providerEntryID;
    p.fbEntryID = kFramebuffer;
    p.bar0Phys = s.bar0Phys;
    p.bar0Size = s.bar0Size;
    p.consolePhysBase = s.consolePhysBase;
    p.consoleVramOffset = s.console.offset;
    p.consoleLength = s.console.length;
    p.consoleWidth = s.consoleWidth;
    p.consoleHeight = s.consoleHeight;
    p.consoleRowBytes = s.consoleRowBytes;
    p.consoleDepth = s.consoleDepth;
    p.modeWidth = modeWidth;
    p.modeHeight = modeHeight;
    p.modeRowBytes = modeRowBytes;
    p.modeDepth = 32;
    p.displayIndex = 0;
    p.head = 0;
    p.otg = 0;
    return p;
}

static void arithmetic_checks() {
    uint64_t out = 0;
    expect(!add_ok(~0ull, 1, out), "range: add overflow refused");
    expect(add_ok(~0ull - 1, 1, out) && out == ~0ull,
           "range: add at the last value accepted");
    expect(!mul_ok(~0ull, 2, out), "range: multiply overflow refused");
    expect(mul_ok(0, ~0ull, out) && out == 0, "range: zero multiply accepted");
    expect(!ranges_overlap(Range { 0, 10 }, Range { 10, 10 }),
           "range: adjacent ranges do not overlap");
    expect(ranges_overlap(Range { 0, 10 }, Range { 9, 2 }),
           "range: one-byte overlap detected");
    expect(!ranges_overlap(Range { 4, 0 }, Range { 0, 10 }),
           "range: empty range never overlaps");
    expect(range_contains(Range { 10, 10 }, Range { 10, 10 }),
           "range: exact containment accepted");
    expect(!range_contains(Range { 10, 10 }, Range { 19, 2 }),
           "range: end outside containment refused");
    expect(!range_contains(Range { ~0ull - 2, 3 }, Range { ~0ull - 1, 2 }),
           "range: overflowing containment refused");
}

static void layout_checks() {
    const Layout l = make_layout();
    expect_u("layout: console starts at zero", l.console.offset, 0);
    expect_u("layout: fixed begins at vramBase", l.fixed.offset, kBase);
    expect_u("layout: fixed keeps its full reserve", l.fixed.length, kFixedReserve);
    expect_u("layout: pool begins after fixed", l.pool.offset, kBase + kFixedReserve);
    expect_u("layout: pool ends at vramLimit", l.pool.offset + l.pool.length, kLimit);
    expect_u("layout: hold is at the top", l.hold.offset, kVram - 768ull * MiB);

    Layout x {};
    LayoutIn in { kVram, 14 * MiB, kLimit, kFixedReserve, 768ull * MiB,
                  0, 14 * MiB, true };
    expect_u("layout: console ending at fixed base accepted", build_layout(in, x), kOk);
    in.consoleLength = 15 * MiB;
    expect_u("layout: console crossing fixed refused", build_layout(in, x), kRangeOverlap);
    in.consoleOffset = kLimit - 1;
    in.consoleLength = 2;
    expect_u("layout: console crossing pool refused", build_layout(in, x), kRangeOverlap);
    in.consoleOffset = kVram - 1;
    expect_u("layout: console crossing VRAM end refused", build_layout(in, x), kRangeOverflow);
    in.consoleOffset = 0;
    in.consoleLength = 0;
    in.vramBase = kLimit;
    expect_u("layout: no pool workspace refused", build_layout(in, x), kNoWorkspace);
    in.vramBase = kLimit + 1;
    expect_u("layout: base above limit refused", build_layout(in, x), kBadRange);
    in.vramBase = kBase;
    in.vramBytes = kLimit + 767ull * MiB;
    expect_u("layout: hold crossing pool refused", build_layout(in, x), kRangeOverlap);
    in = LayoutIn { kVram, kBase, kLimit, kFixedReserve, 768ull * MiB, 0, 0, false };
    expect_u("layout: no console still has a valid pool", build_layout(in, x), kOk);
}

static void peer_checks() {
    Snapshot s = snapshot();
    const Expected e = expected();
    expect_u("peer: ready snapshot accepted", validate_peer(s, e, true), kOk);
    expect_u("peer: layout validates independently", validate_snapshot_layout(s), kOk);

    s.version++;
    expect_u("peer: version mismatch refused", validate_peer(s, e, true), kBadVersion);
    s = snapshot();
    s.state = kStateStarting;
    expect_u("peer: starting refused", validate_peer(s, e, true), kNotReady);
    s.state = kStateStopping;
    expect_u("peer: stopping refused", validate_peer(s, e, true), kStopping);
    s.state = kStateStopped;
    expect_u("peer: stopped refused", validate_peer(s, e, true), kStopped);

    s = snapshot();
    s.device = 0x7550;
    expect_u("peer: wrong device refused", validate_peer(s, e, true), kIdentityMismatch);
    s = snapshot();
    s.providerEntryID++;
    expect_u("peer: wrong provider refused", validate_peer(s, e, true), kIdentityMismatch);
    s = snapshot();
    s.asic = kAsicUnknown;
    expect_u("peer: unknown ASIC refused", validate_peer(s, e, true), kAsicMismatch);
    s = snapshot();
    Expected wildcard = e;
    wildcard.asic = kAsicUnknown;
    expect_u("peer: pre-discovery caller may wildcard the ASIC",
             validate_peer(s, wildcard, true), kOk);
    s = snapshot();
    s.bar0Phys++;
    expect_u("peer: wrong BAR0 address refused", validate_peer(s, e, true), kBarMismatch);
    s = snapshot();
    s.pool.offset++;
    expect_u("peer: gap before pool refused", validate_snapshot_layout(s), kBadRange);
    s = snapshot();
    s.fixed.length = 0;
    expect_u("peer: empty fixed reserve refused", validate_snapshot_layout(s), kBadRange);
    s = snapshot();
    s.grant = Grant { true, kBase, 1 * MiB, 7 };
    expect_u("peer: grant outside pool refused", validate_peer(s, e, true), kGrantOutsidePool);
    s = snapshot();
    s.grant = Grant { true, s.pool.offset, s.pool.length, 7 };
    expect_u("peer: whole-pool grant accepted", validate_peer(s, e, true), kOk);
    s = snapshot();
    s.grant = Grant { true, s.pool.offset + s.pool.length - 1, 2, 7 };
    expect_u("peer: grant crossing pool refused", validate_peer(s, e, true), kGrantOutsidePool);
    s = snapshot();
    s.grant = Grant { true, s.pool.offset, ~0ull, 7 };
    expect_u("peer: overflowing grant refused", validate_peer(s, e, true), kGrantOutsidePool);
}

static void snapshot_overflow_checks() {
    Snapshot s = snapshot();
    s.console.offset = 4;
    s.consolePhysBase = ~0ull;
    expect_u("snapshot: console physical overflow refused",
             validate_snapshot_layout(s), kRangeOverflow);
    s = snapshot();
    s.console.length = 0;
    expect_u("snapshot: empty console refused", validate_snapshot_layout(s), kBadRange);
    s = snapshot();
    s.consoleWidth = 0;
    expect_u("snapshot: zero console width refused", validate_snapshot_layout(s), kBadRange);
    s = snapshot();
    s.consoleRowBytes = static_cast<uint32_t>(~0ull);
    s.consoleHeight = 2;
    s.console.length = 1;
    expect_u("snapshot: row overflow refused", validate_snapshot_layout(s), kBadRange);
    s = snapshot();
    s.consoleDepth = 24;
    expect_u("snapshot: non-32-bit console refused", validate_snapshot_layout(s), kBadRange);
    s = snapshot();
    s.console.offset = s.pool.offset;
    s.consolePhysBase = s.bar0Phys + s.console.offset;
    expect_u("snapshot: console overlapping pool refused",
             validate_snapshot_layout(s), kRangeOverlap);
}

static void surface_checks() {
    Snapshot s = snapshot();
    const Expected e = expected();
    Surface out {};
    expect_u("surface: absent grant refused",
             surface_from_grant(s, e, kWidth, kHeight, out), kGrantAbsent);
    s = snapshot(true);
    expect_u("surface: 1080p surface fits",
             surface_from_grant(s, e, kWidth, kHeight, out), kOk);
    expect_u("surface: physical base uses the grant",
             out.physBase, kBarPhys + s.pool.offset);
    expect_u("surface: row bytes", out.rowBytes, kRowBytes);
    expect(out.width == kWidth && out.height == kHeight,
           "surface: dimensions preserved");
    expect_u("surface: zero width refused",
             surface_from_grant(s, e, 0, kHeight, out), kBadSurface);
    expect_u("surface: row overflow refused",
             surface_from_grant(s, e, 0x40000000u, 1, out), kSurfaceOverflow);
    s.grant.length = kSurfaceTail;
    expect_u("surface: too-small grant refused",
             surface_from_grant(s, e, 1280, 720, out), kGrantTooSmall);
    s = snapshot(true);
    s.state = kStateStopping;
    expect_u("surface: stopping peer cannot grant",
             surface_from_grant(s, e, 1280, 720, out), kStopping);
}

static GateState ready_gate() {
    GateState g {};
    gate_reset(g);
    expect_u("gate: starts in STARTING", g.state, kStateStarting);
    expect_u("gate: transition to READY", gate_ready(g), kOk);
    expect(g.generation != 0, "gate: READY has a nonzero generation");
    return g;
}

static void gate_lifecycle_checks() {
    GateState g = ready_gate();
    Snapshot s = snapshot();
    expect_u("gate: local acquire validates snapshot",
             validate_local_acquire(s, g, g.generation, kFramebuffer,
                                    kLeaseLocalDisplay, 0, 0), kOk);
    uint64_t token = 0;
    expect_u("gate: first acquire succeeds",
             gate_acquire(g, g.generation, kFramebuffer, kLeaseLocalDisplay,
                          0x1111, token), kOk);
    expect(token != 0 && g.activeLease == 1 && g.activeDepth == 1,
           "gate: first acquire records one lease");
    expect(gate_held(g, g.generation, 0x1111, kFramebuffer),
           "gate: holder is recognised for its framebuffer");
    expect(!gate_held(g, g.generation, 0x1111, kFramebuffer + 1),
           "gate: another framebuffer cannot ride the lease");
    expect(!gate_held(g, g.generation, 0x2222, kFramebuffer),
           "gate: another thread cannot ride the lease");
    expect(gate_held(g, g.generation, 0x1111, 0),
           "gate: a caller naming no framebuffer keeps passive nesting");
    const uint64_t first = token;
    expect_u("gate: same owner/fb/kind nests",
             gate_acquire(g, g.generation, kFramebuffer, kLeaseLocalDisplay,
                          0x1111, token), kOk);
    expect_u("gate: nested acquire returns the same token", token, first);
    expect_u("gate: nested depth", g.activeDepth, 2);
    expect_u("gate: another framebuffer conflicts",
             gate_acquire(g, g.generation, kFramebuffer + 1,
                          kLeaseLocalDisplay, 0x1111, token),
             kOwnershipConflict);
    expect_u("gate: same owner different kind conflicts",
             gate_acquire(g, g.generation, kFramebuffer, kLeaseDisplayControl,
                          0x1111, token), kOwnershipConflict);
    expect_u("gate: another owner conflicts",
             gate_acquire(g, g.generation, kFramebuffer, kLeaseLocalDisplay,
                          0x2222, token), kOwnershipConflict);
    expect_u("gate: stale generation refused",
             gate_acquire(g, g.generation - 1, kFramebuffer,
                          kLeaseLocalDisplay, 0x1111, token), kBadGeneration);
    expect_u("gate: wrong release owner refused",
             gate_release(g, first, kFramebuffer, g.generation, 0x2222),
             kOwnershipConflict);
    expect_u("gate: nested release succeeds",
             gate_release(g, first, kFramebuffer, g.generation, 0x1111), kOk);
    expect_u("gate: outer release succeeds",
             gate_release(g, first, kFramebuffer, g.generation, 0x1111), kOk);
    expect_u("gate: release clears the lease", g.activeLease, 0);

    expect_u("gate: STOPPING begins", gate_begin_stop(g), kOk);
    expect_u("gate: STOPPING state", g.state, kStateStopping);
    expect_u("gate: normal acquire refused while stopping",
             gate_acquire(g, g.generation, kFramebuffer, kLeaseLocalDisplay,
                          0x1111, token), kStopping);
    s.state = kStateStopping;
    s.generation = g.generation;
    expect_u("gate: stop owner validates during STOPPING",
             validate_local_acquire(s, g, g.generation, kProvider,
                                    kLeaseDisplayStop, 0, 0), kOk);
    expect_u("gate: stop owner acquires the drain-exclusive lease",
             gate_acquire(g, g.generation, kFramebuffer, kLeaseDisplayStop,
                          0x1111, token), kOk);
    expect_u("gate: stop cannot finish with an active lease",
             gate_finish_stop(g), kOwnershipConflict);
    expect_u("gate: stop lease releases", gate_release(g, token, kFramebuffer,
                                                       g.generation, 0x1111), kOk);
    expect_u("gate: stop finishes after drain", gate_finish_stop(g), kOk);
    expect_u("gate: stopped state", g.state, kStateStopped);
    expect_u("gate: acquire after stop refused",
             gate_acquire(g, g.generation, kFramebuffer, kLeaseLocalDisplay,
                          0x1111, token), kStopped);

    GateState draining = ready_gate();
    uint64_t inFlight = 0;
    expect_u("drain: transaction starts before STOPPING",
             gate_acquire(draining, draining.generation, kFramebuffer,
                          kLeaseDisplayControl, 0x1111, inFlight), kOk);
    expect_u("drain: STOPPING begins under the in-flight transaction",
             gate_begin_stop(draining), kOk);
    expect(gate_held(draining, draining.generation, 0x1111, kFramebuffer),
           "drain: the holder may finish its own transaction");
    expect(!gate_held(draining, draining.generation, 0x2222, kFramebuffer),
           "drain: another owner is not admitted");
    expect_u("drain: no new acquisition while stopping",
             gate_acquire(draining, draining.generation, kFramebuffer,
                          kLeaseDisplayControl, 0x2222, token), kStopping);
    expect_u("drain: the holder releases",
             gate_release(draining, inFlight, kFramebuffer,
                          draining.generation, 0x1111), kOk);
    expect_u("drain: STOPPED after the last lease", gate_finish_stop(draining), kOk);

    GateState aborted {};
    gate_reset(aborted);
    expect_u("gate: STARTING may enter STOPPING", gate_begin_stop(aborted), kOk);
    expect_u("gate: aborted start drains", gate_finish_stop(aborted), kOk);
}

static void transaction_revalidation_checks() {
    const Snapshot s = snapshot();
    GateState g = ready_gate();
    expect_u("txn: unchanged snapshot accepted",
             validate_local_acquire(s, g, g.generation, kFramebuffer,
                                    kLeaseLocalDisplay, 0, 0), kOk);
    Snapshot stale = s;
    stale.generation = g.generation + 1;
    expect_u("txn: snapshot generation mismatch refused",
             validate_local_acquire(stale, g, g.generation, kFramebuffer,
                                    kLeaseLocalDisplay, 0, 0), kBadGeneration);
    GateState disconnected = g;
    disconnected.generation++;
    expect_u("txn: gate generation mismatch refused",
             validate_local_acquire(s, disconnected, s.generation, kFramebuffer,
                                    kLeaseLocalDisplay, 0, 0), kBadGeneration);
    Snapshot bad = s;
    bad.pool.offset++;
    expect_u("txn: invalid snapshot layout refused",
             validate_local_acquire(bad, g, g.generation, kFramebuffer,
                                    kLeaseLocalDisplay, 0, 0), kBadRange);
    expect_u("txn: empty framebuffer id refused",
             validate_local_acquire(s, g, g.generation, 0,
                                    kLeaseLocalDisplay, 0, 0), kBadCaller);
    expect_u("txn: unknown lease kind refused",
             validate_local_acquire(s, g, g.generation, kFramebuffer,
                                    0, 0, 0), kBadRequest);
    expect_u("txn: head outside the contract refused",
             validate_local_acquire(s, g, g.generation, kFramebuffer,
                                    kLeaseLocalDisplay, kMaxDisplayHead + 1, 0),
             kBadRequest);
}

static void survey_stop_checks() {
    // A survey/partial start never produced a resource table: every range is
    // zero. It still has an identity and a generation, so its own stop path
    // must be able to acquire the drain-exclusive teardown lease without
    // pretending the layout was valid.
    Snapshot starting {};
    starting.version = kVersion;
    starting.state = kStateStarting;
    starting.asic = kAsicNavi44;
    starting.vendor = 0x1002;
    starting.device = 0x7590;
    starting.providerEntryID = kProvider;
    starting.bar0Phys = kBarPhys;
    starting.bar0Size = kBarSize;
    starting.vramBytes = kVram;

    GateState g {};
    gate_reset(g);
    starting.generation = g.generation;
    expect_u("survey: normal local acquire refused before READY",
             validate_local_acquire(starting, g, g.generation, kFramebuffer,
                                    kLeaseLocalDisplay, 0, 0), kNotReady);
    expect_u("survey: STARTING may enter STOPPING", gate_begin_stop(g), kOk);

    Snapshot stopping = starting;
    stopping.state = kStateStopping;
    stopping.generation = g.generation;
    expect_u("survey: a foreign framebuffer cannot own the stop lease",
             validate_local_acquire(stopping, g, g.generation, kFramebuffer,
                                    kLeaseDisplayStop, 0, 0), kBadCaller);
    expect_u("survey: stop owner validates the incomplete snapshot",
             validate_local_acquire(stopping, g, g.generation, kProvider,
                                    kLeaseDisplayStop, 0, 0), kOk);
    uint64_t token = 0;
    expect_u("survey: stop owner acquires the drain-exclusive lease",
             gate_acquire(g, g.generation, kProvider, kLeaseDisplayStop,
                          0x1111, token), kOk);
    expect(token != 0, "survey: stop owner holds a real token");
    expect_u("survey: normal transactions stay refused while stopping",
             validate_local_acquire(stopping, g, g.generation, kFramebuffer,
                                    kLeaseLocalDisplay, 0, 0), kStopping);
    expect_u("survey: stop lease releases", gate_release(g, token, kProvider,
                                                         g.generation, 0x1111), kOk);
    expect_u("survey: STOPPED after the drain", gate_finish_stop(g), kOk);

    // The teardown exemption is only for STOPPING. A READY snapshot with no
    // ranges still has to pass the complete layout check for a normal lease.
    GateState ready = ready_gate();
    Snapshot zero = starting;
    zero.state = kStateReady;
    zero.generation = ready.generation;
    expect_u("survey: READY with zero ranges still fails the full layout",
             validate_local_acquire(zero, ready, ready.generation, kFramebuffer,
                                    kLeaseLocalDisplay, 0, 0), kBadRange);
}

static void lease_drain_checks() {
    expect(lease_fb_is_primary(false, 0, kFramebuffer, kLeaseDisplayInit),
           "lease: bootstrap precedes primary registration");
    expect(!lease_fb_is_primary(false, 0, kFramebuffer, kLeaseDisplayControl),
           "lease: a control lease needs a registered primary");
    expect(lease_fb_is_primary(true, kFramebuffer, kFramebuffer,
                               kLeaseDisplayControl),
           "lease: the registered framebuffer owns the control lease");
    expect(!lease_fb_is_primary(true, kFramebuffer, kFramebuffer + 1,
                                kLeaseDisplayControl),
           "lease: another framebuffer cannot take a control lease");

    // STOPPING drains the transactions that already exist. The generation a
    // lease acquired at stays valid for that same thread until it releases;
    // new threads and new leases are refused.
    GateState stopping = ready_gate();
    Snapshot inFlightSnapshot = snapshot(true);
    inFlightSnapshot.generation = stopping.generation;
    uint64_t inFlightToken = 0;
    expect_u("lease: control lease starts before STOPPING",
             gate_acquire(stopping, stopping.generation, kFramebuffer,
                          kLeaseDisplayControl, 0x4444, inFlightToken), kOk);
    const uint64_t leaseGeneration = inFlightSnapshot.generation;
    expect_u("lease: STOPPING begins without invalidating the lease",
             gate_begin_stop(stopping), kOk);
    expect_u("lease: STOPPING keeps the lease generation",
             stopping.generation, leaseGeneration);
    expect(gate_held(stopping, leaseGeneration, 0x4444, kFramebuffer),
           "lease: in-flight owner stays admitted while STOPPING");
    expect(!gate_held(stopping, leaseGeneration, 0x5555, kFramebuffer),
           "lease: a new thread cannot ride the draining lease");
    uint64_t newToken = 0;
    expect_u("lease: a new display lease is refused while STOPPING",
             gate_acquire(stopping, stopping.generation, kFramebuffer,
                          kLeaseDisplayControl, 0x5555, newToken), kStopping);
    expect_u("lease: the draining lease releases",
             gate_release(stopping, inFlightToken, kFramebuffer,
                          stopping.generation, 0x4444), kOk);
    expect_u("lease: STOPPED after the drain", gate_finish_stop(stopping), kOk);
}

static void primary_checks() {
    GateState g = ready_gate();
    const Snapshot s = snapshot();
    PrimaryView stored {};
    PrimaryView candidate = make_primary_view(s);
    expect_u("primary: candidate validates", validate_primary(s, candidate), kOk);
    expect_u("primary: first registration succeeds",
             primary_register(g, stored, candidate), kOk);
    expect_u("primary: exact re-registration is idempotent",
             primary_register(g, stored, candidate), kOk);

    PrimaryView changed = candidate;
    changed.modeWidth -= 4;
    changed.modeRowBytes -= 16;
    expect_u("primary: changed mode is a sticky conflict",
             primary_register(g, stored, changed), kPrimaryConflict);
    PrimaryView out {};
    uint64_t token = 0;
    expect_u("primary: query refuses after a metadata conflict",
             primary_query(g, stored, g.generation, out), kPrimaryConflict);
    expect_u("primary: conflicted gate refuses an external display lease",
             gate_acquire(g, g.generation, kFramebuffer, kLeaseDisplayControl,
                          0x1111, token), kPrimaryConflict);
    Snapshot conflicted = s;
    conflicted.generation = g.generation;
    expect_u("primary: conflicted gate refuses a local display lease",
             validate_local_acquire(conflicted, g, g.generation, kFramebuffer,
                                    kLeaseLocalDisplay, 0, 0), kPrimaryConflict);
    expect_u("primary: conflicted gate still starts the drain",
             gate_begin_stop(g), kOk);
    Snapshot stopping = conflicted;
    stopping.state = kStateStopping;
    stopping.generation = g.generation;
    expect_u("primary: conflicted gate still allows stop teardown",
             validate_local_acquire(stopping, g, g.generation, kProvider,
                                    kLeaseDisplayStop, 0, 0), kOk);

    g = ready_gate();
    stored = {};
    candidate = make_primary_view(s);
    expect_u("primary: setup for different framebuffer",
             validate_primary(s, candidate), kOk);
    expect_u("primary: register first framebuffer",
             primary_register(g, stored, candidate), kOk);
    PrimaryView otherFb = candidate;
    otherFb.fbEntryID++;
    expect_u("primary: different framebuffer is a sticky conflict",
             primary_register(g, stored, otherFb), kPrimaryConflict);
    expect_u("primary: query still refuses after framebuffer conflict",
             primary_query(g, stored, g.generation, out), kPrimaryConflict);
    expect_u("primary: original owner can reset the conflict",
             primary_unregister(g, stored, kFramebuffer, s.generation), kOk);
    expect_u("primary: re-register after explicit unregister",
             primary_register(g, stored, candidate), kOk);

    expect_u("primary: zero generation query accepted",
             primary_query(g, stored, 0, out), kOk);
    expect_u("primary: stale query refused",
             primary_query(g, stored, g.generation + 1, out), kBadGeneration);
    expect_u("primary: wrong framebuffer unregister refused",
             primary_unregister(g, stored, kFramebuffer + 1, s.generation),
             kBadCaller);

    expect_u("primary: lease for the transaction test",
             gate_acquire(g, g.generation, kFramebuffer, kLeaseLocalDisplay,
                          0x1111, token), kOk);
    expect_u("primary: registration refused under an active lease",
             primary_register(g, stored, candidate), kOwnershipConflict);
    expect_u("primary: unregister refused under an active lease",
             primary_unregister(g, stored, kFramebuffer, s.generation),
             kOwnershipConflict);
    expect_u("primary: query refused under an active lease",
             primary_query(g, stored, 0, out), kOwnershipConflict);
    expect_u("primary: the lease holder may revalidate its registration",
             primary_query(g, stored, 0, out, 0x1111), kOk);
    expect_u("primary: release ends the transaction",
             gate_release(g, token, kFramebuffer, g.generation, 0x1111), kOk);
    expect_u("primary: unregister works once the lease is gone",
             primary_unregister(g, stored, kFramebuffer, s.generation), kOk);

    PrimaryView bad = make_primary_view(s);
    bad.displayIndex = 1;
    expect_u("primary: non-console display index refused",
             validate_primary(s, bad), kPrimaryMismatch);
    bad = make_primary_view(s);
    bad.head = 1;
    expect_u("primary: non-OTG0 head refused",
             validate_primary(s, bad), kPrimaryMismatch);
    bad = make_primary_view(s);
    bad.otg = 1;
    expect_u("primary: non-OTG0 OTG refused",
             validate_primary(s, bad), kPrimaryMismatch);
    bad = make_primary_view(s);
    bad.modeRowBytes = static_cast<uint32_t>(kRowBytes + 4);
    expect_u("primary: mode row wider than console refused",
             validate_primary(s, bad), kPrimaryMismatch);
    bad = make_primary_view(s, kWidth - 4, kHeight, kRowBytes - 16);
    expect_u("primary: smaller mode is not a valid console identity",
             validate_primary(s, bad), kPrimaryMismatch);
    bad = make_primary_view(s);
    bad.generation++;
    expect_u("primary: stale registration generation refused",
             validate_primary(s, bad), kBadGeneration);
    bad = make_primary_view(s);
    bad.fbEntryID = 0;
    expect_u("primary: zero framebuffer entry ID refused",
             validate_primary(s, bad), kPrimaryMismatch);
}

static void concurrency_check() {
    GateState g = ready_gate();
    std::mutex mu;
    std::atomic<bool> start { false };
    constexpr unsigned kThreads = 16;
    uint32_t result[kThreads] {};
    uint64_t token[kThreads] {};
    std::vector<std::thread> threads;
    threads.reserve(kThreads);
    for (unsigned i = 0; i < kThreads; i++) {
        threads.emplace_back([&, i] {
            while (!start.load(std::memory_order_acquire)) {
            }
            std::lock_guard<std::mutex> lock(mu);
            result[i] = gate_acquire(g, g.generation, kFramebuffer + i,
                                     kLeaseLocalDisplay, 0x9000u + i, token[i]);
        });
    }
    start.store(true, std::memory_order_release);
    for (auto &t : threads) t.join();
    unsigned wins = 0, conflicts = 0;
    unsigned winner = 0;
    for (unsigned i = 0; i < kThreads; i++) {
        if (result[i] == kOk) {
            wins++;
            winner = i;
        } else if (result[i] == kOwnershipConflict) {
            conflicts++;
        }
    }
    expect_u("concurrency: exactly one display owner", wins, 1);
    expect_u("concurrency: all other attempts conflict", conflicts, kThreads - 1);
    expect(g.activeDepth == 1 && g.activeLease == 1,
           "concurrency: one lease record");
    std::lock_guard<std::mutex> lock(mu);
    expect_u("concurrency: winner can release",
             gate_release(g, token[winner], kFramebuffer + winner,
                          g.generation, 0x9000u + winner), kOk);
    expect(g.activeLease == 0 && g.activeDepth == 0,
           "concurrency: release drains the record");
}

static void startup_order_checks() {
    AttachState displayFirst {};
    attach_reset(displayFirst);
    expect(attach_should_retry(displayFirst),
           "startup: display-first starts in WAITING");
    expect_u("startup: a pre-READY probe is not fatal",
             attach_note(displayFirst, kNotReady), kNotReady);
    expect(attach_should_retry(displayFirst),
           "startup: pre-READY keeps a retry path");
    expect_u("startup: a starting/stopping peer is not fatal",
             attach_note(displayFirst, kStopping), kStopping);
    expect(attach_should_retry(displayFirst),
           "startup: STOPPING does not latch the display closed");
    expect_u("startup: a later READY probe registers",
             attach_note(displayFirst, kOk), kOk);
    expect(!attach_should_retry(displayFirst),
           "startup: registration ends the retry loop");
    expect_u("startup: attempts are counted", displayFirst.attempts, 3);

    AttachState peerFirst {};
    attach_reset(peerFirst);
    expect_u("startup: peer-first registers on the first probe",
             attach_note(peerFirst, kOk), kOk);
    expect_u("startup: peer-first needs one attempt", peerFirst.attempts, 1);
    attach_stop(peerFirst);
    expect(!attach_should_retry(peerFirst),
           "startup: stop ends retries");
    expect_u("startup: stopped is sticky",
             attach_note(peerFirst, kOk), kStopped);
}

static void full_bar_64bit_checks() {
    const uint64_t kHiBarPhys = 0x7C00000000ull;
    const uint64_t kFullBar   = 16ull * 1024ull * MiB;
    const uint64_t kValidVram = 16304ull * MiB;
    const uint64_t kFullLimit = 15472ull * MiB;

    Layout lay {};
    LayoutIn lin { kValidVram, kBase, kFullLimit, kFixedReserve, 768ull * MiB,
                   0, kConsoleBytes, true };
    expect_u("fullbar-peer: 16 GiB layout builds cleanly", build_layout(lin, lay), kOk);

    Snapshot s = snapshot();
    s.bar0Phys = kHiBarPhys;
    s.bar0Size = kFullBar;
    s.vramBytes = kValidVram;
    s.vramLimit = kFullLimit;
    s.consolePhysBase = kHiBarPhys;
    s.console = lay.console;
    s.fixed = lay.fixed;
    s.pool = lay.pool;
    s.hold = lay.hold;
    Expected e = expected();
    e.bar0Phys = kHiBarPhys;
    e.bar0Size = kFullBar;
    expect_u("fullbar-peer: 64-bit 16 GiB snapshot validates", validate_peer(s, e, true), kOk);

    PrimaryView pv {};
    pv.version = kVersion;
    pv.asic = s.asic; pv.vendor = s.vendor; pv.device = s.device;
    pv.generation = s.generation; pv.providerEntryID = s.providerEntryID;
    pv.fbEntryID = kFramebuffer;
    pv.bar0Phys = kHiBarPhys; pv.bar0Size = kFullBar;
    pv.consolePhysBase = kHiBarPhys; pv.consoleVramOffset = 0;
    pv.consoleLength = kConsoleBytes; pv.consoleWidth = kWidth;
    pv.consoleHeight = kHeight; pv.consoleRowBytes = kRowBytes; pv.consoleDepth = 32;
    pv.modeWidth = kWidth; pv.modeHeight = kHeight; pv.modeRowBytes = kRowBytes; pv.modeDepth = 32;
    expect_u("fullbar-peer: 64-bit PrimaryView validates", validate_primary(s, pv), kOk);

    // Console in hi-pool [15472 MiB, 15536 MiB) must be refused by both build_layout and validate_snapshot_layout
    Layout badLay {};
    LayoutIn hiCons = lin;
    hiCons.consoleOffset = kFullLimit;
    hiCons.consoleLength = 8ull * MiB;
    expect_u("fullbar-peer: console in hi pool refused by build_layout", build_layout(hiCons, badLay), kBadRange);

    Snapshot sHi = s;
    sHi.console.offset = kFullLimit;
    sHi.console.length = kConsoleBytes;
    sHi.consolePhysBase = kHiBarPhys + sHi.console.offset;
    expect_u("fullbar-peer: console past vramLimit refused by validate_snapshot_layout",
             validate_snapshot_layout(sHi), kBadRange);

    // Stale low 32-bit console physical address against high 64-bit BAR0 refused
    Snapshot sStale = s;
    sStale.consolePhysBase = 0xD0000000ull;
    expect_u("fullbar-peer: stale console phys outside high BAR0 refused",
             validate_snapshot_layout(sStale), kPrimaryMismatch);

    // vramLimit exceeding mapped bar0Size refused
    Snapshot sTrunc = s;
    sTrunc.bar0Size = 1024ull * MiB;
    expect_u("fullbar-peer: vramLimit > bar0Size refused",
             validate_snapshot_layout(sTrunc), kBadRange);

    // Wrapping bar0Phys + bar0Size refused
    Snapshot sWrap = s;
    sWrap.bar0Phys = ~0ull - 4096ull;
    sWrap.consolePhysBase = sWrap.bar0Phys;
    expect_u("fullbar-peer: wrapping bar0Phys + bar0Size refused",
             validate_snapshot_layout(sWrap), kRangeOverflow);
}

int main() {
    arithmetic_checks();
    layout_checks();
    peer_checks();
    snapshot_overflow_checks();
    surface_checks();
    gate_lifecycle_checks();
    transaction_revalidation_checks();
    survey_stop_checks();
    lease_drain_checks();
    primary_checks();
    concurrency_check();
    startup_order_checks();
    full_bar_64bit_checks();
    std::printf("native_display_peer_test: %d checks, %d failed\n", gRun, gFail);
    return gFail ? 1 : 0;
}
