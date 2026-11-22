#include "amd/native_display_identity_pure.h"

#include <cassert>
#include <cstdio>

int main() {
    const rdna4peer::Expected expected { 0x1002, 0x7590, rdna4peer::kAsicNavi44,
        100, 0xd0000000ull, 256ull << 20 };
    rdna4peer::PrimaryView primary {};
    primary.version = rdna4peer::kVersion;
    primary.asic = expected.asic;
    primary.vendor = expected.vendor;
    primary.device = expected.device;
    primary.generation = 1;
    primary.providerEntryID = expected.providerEntryID;
    primary.fbEntryID = 101;
    primary.bar0Phys = expected.bar0Phys;
    primary.bar0Size = expected.bar0Size;
    primary.consolePhysBase = expected.bar0Phys;
    primary.consoleLength = 7680u * 1080u;
    primary.consoleWidth = primary.modeWidth = 1920;
    primary.consoleHeight = primary.modeHeight = 1080;
    primary.consoleRowBytes = primary.modeRowBytes = 7680;
    primary.consoleDepth = primary.modeDepth = 32;
    const auto accepts = [&expected](const rdna4peer::PrimaryView &p) {
        return rdna4primary::valid(p, expected, 101, 0, 7680u * 1080u,
                                  1920, 1080, 7680);
    };
    assert(accepts(primary));
    auto bad = primary;
    bad.version++;
    assert(!accepts(bad));
    bad = primary; bad.vendor++;
    assert(!accepts(bad));
    bad = primary; bad.bar0Phys += 4096;
    assert(!accepts(bad));
    bad = primary; bad.bar0Size -= 4096;
    assert(!accepts(bad));
    bad = primary;
    bad.providerEntryID++;
    assert(!accepts(bad));
    bad = primary; bad.fbEntryID++;
    assert(!accepts(bad));
    bad = primary; bad.device = 0x7550;
    assert(!accepts(bad));
    bad = primary; bad.asic = rdna4peer::kAsicNavi48;
    assert(!accepts(bad));
    bad = primary; bad.generation = 0;
    assert(!accepts(bad));
    bad = primary; bad.head = 1;
    assert(!accepts(bad));
    bad = primary; bad.otg = 2;
    assert(!accepts(bad));
    bad = primary; bad.displayIndex = 1;
    assert(!accepts(bad));
    bad = primary; bad.consolePhysBase++;
    assert(!accepts(bad));
    bad = primary; bad.consoleLength--;
    assert(!accepts(bad));
    bad = primary; bad.consoleVramOffset += 4096;
    assert(!accepts(bad));
    bad = primary; bad.consoleWidth--;
    assert(!accepts(bad));
    bad = primary; bad.consoleHeight--;
    assert(!accepts(bad));
    bad = primary; bad.consoleRowBytes += 4;
    assert(!accepts(bad));
    bad = primary; bad.consoleDepth = 24;
    assert(!accepts(bad));
    bad = primary; bad.modeWidth = 1280;
    assert(!accepts(bad));
    bad = primary; bad.modeHeight = 720;
    assert(!accepts(bad));
    bad = primary; bad.modeRowBytes -= 4;
    assert(!accepts(bad));
    bad = primary; bad.modeDepth = 24;
    assert(!accepts(bad));
    auto wrong = expected;
    wrong.providerEntryID = 0;
    assert(!rdna4primary::valid(primary, wrong, 101, 0, primary.consoleLength,
                              1920, 1080, 7680));
    wrong = expected; wrong.bar0Size = primary.consoleLength - 1;
    assert(!rdna4primary::valid(primary, wrong, 101, 0, primary.consoleLength,
                              1920, 1080, 7680));
    wrong = expected; wrong.bar0Phys = ~0ull - 4;
    bad = primary; bad.bar0Phys = wrong.bar0Phys;
    assert(!rdna4primary::valid(bad, wrong, 101, 8, primary.consoleLength,
                              1920, 1080, 7680));
    assert(!rdna4primary::valid(primary, expected, 101, 0, primary.consoleLength,
                              1921, 1080, 7680));
    assert(!rdna4primary::valid(primary, expected, 101, 0, primary.consoleLength,
                              1920, 0, 7680));
    auto padded = primary;
    padded.consoleVramOffset = 4096;
    padded.consolePhysBase += 4096;
    padded.consoleRowBytes = padded.modeRowBytes = 8192;
    padded.consoleLength = 8192u * 1080u;
    assert(rdna4primary::valid(padded, expected, 101, 4096, padded.consoleLength,
                              1920, 1080, 8192));

    rdna4peer::Snapshot s {};
    s.version = primary.version;
    s.state = rdna4peer::kStateReady;
    s.asic = primary.asic;
    s.vendor = primary.vendor;
    s.device = primary.device;
    s.generation = primary.generation;
    s.providerEntryID = primary.providerEntryID;
    s.bar0Phys = primary.bar0Phys;
    s.bar0Size = primary.bar0Size;
    s.vramBytes = 16ull << 30;
    s.vramBase = 16ull << 20;
    s.vramLimit = s.bar0Size;
    s.console = { primary.consoleVramOffset, primary.consoleLength };
    s.consolePhysBase = primary.consolePhysBase;
    s.consoleWidth = primary.consoleWidth;
    s.consoleHeight = primary.consoleHeight;
    s.consoleRowBytes = primary.consoleRowBytes;
    s.consoleDepth = primary.consoleDepth;
    s.fixed = { s.vramBase, rdna4peer::kFixedReserve };
    s.pool = { s.fixed.offset + s.fixed.length,
               s.vramLimit - s.fixed.offset - s.fixed.length };
    s.hold = { s.vramBytes - (768ull << 20), 768ull << 20 };
    const auto acceptsView = [&expected](const rdna4peer::Snapshot &snap,
                                         const rdna4peer::PrimaryView &view) {
        return rdna4primary::valid_snapshot_view(snap, view, expected, 101);
    };
    assert(acceptsView(s, primary));
    auto badSnap = s; badSnap.generation++;
    assert(!acceptsView(badSnap, primary));
    auto badView = primary; badView.generation++;
    assert(!acceptsView(s, badView));
    badSnap = s; badSnap.state = rdna4peer::kStateStopping;
    assert(!acceptsView(badSnap, primary));
    badSnap = s; badSnap.consoleWidth--;
    assert(!acceptsView(badSnap, primary));
    badView = primary; badView.modeWidth--;
    assert(!acceptsView(s, badView));
    badSnap = s; badSnap.pool.offset = 0;
    assert(!acceptsView(badSnap, primary));
    badSnap = s; badSnap.displayIndex = 1;
    assert(!acceptsView(badSnap, primary));
    std::puts("native_display_identity_test: PASS");
}
