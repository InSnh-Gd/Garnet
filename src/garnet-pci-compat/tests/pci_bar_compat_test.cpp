//
//  pci_bar_compat_test.cpp — Host unit tests for pci_bar_compat_pure.h.
//
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>

#include "pci_bar_compat_pure.h"

namespace {

int gChecks = 0;
int gFailures = 0;

#define CHECK(cond)                                                              \
    do {                                                                         \
        ++gChecks;                                                               \
        if (!(cond)) {                                                           \
            ++gFailures;                                                         \
            std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
        }                                                                        \
    } while (0)

using namespace garnet_pcibar;

// ---- Synthetic IOPCIFamily Mach-O Builder ---------------------------------------------------

struct PageAlignedBuffer {
    std::vector<uint8_t> backing;
    size_t offset { 0 };
    size_t logicalSize { 0 };

    PageAlignedBuffer() = default;
    PageAlignedBuffer(const PageAlignedBuffer &other) { *this = other; }
    PageAlignedBuffer &operator=(const PageAlignedBuffer &other) {
        if (this == &other) return *this;
        if (other.logicalSize == 0u) {
            clear();
            return *this;
        }
        allocate(other.logicalSize, 0u);
        std::memcpy(data(), other.data(), logicalSize);
        return *this;
    }
    PageAlignedBuffer(PageAlignedBuffer &&) = default;
    PageAlignedBuffer &operator=(PageAlignedBuffer &&) = default;

    void allocate(size_t sz, uint8_t fill = 0u) {
        backing.assign(sz + 4096u, fill);
        const uintptr_t raw = reinterpret_cast<uintptr_t>(backing.data());
        const uintptr_t aligned = (raw + 4095ULL) & ~4095ULL;
        offset = static_cast<size_t>(aligned - raw);
        logicalSize = sz;
    }

    void clear() {
        backing.clear();
        offset = 0u;
        logicalSize = 0u;
    }

    bool empty() const { return logicalSize == 0u; }
    uint8_t *data() { return backing.data() + offset; }
    const uint8_t *data() const { return backing.data() + offset; }
    size_t size() const { return logicalSize; }
    uint8_t &operator[](size_t idx) { return data()[idx]; }
    const uint8_t &operator[](size_t idx) const { return data()[idx]; }
};

struct SyntheticIopci {
    PageAlignedBuffer image;
    uint64_t anchorOff { 0x2076u };
    uint64_t entryOff { kExpectedEntryHdrOff }; // 0x3712e
    uint64_t textOff { 0x650u };
    uint64_t textSize { 0x44173u };
};

void write_e8(uint8_t *buf, uint64_t entryHdr, size_t callSiteOff, uint64_t targetHdr) {
    buf[entryHdr + callSiteOff] = 0xE8u;
    const int64_t rel = static_cast<int64_t>(targetHdr) -
                        (static_cast<int64_t>(entryHdr) + static_cast<int64_t>(callSiteOff) + 5);
    write_unaligned<int32_t>(buf + entryHdr + callSiteOff + 1u, static_cast<int32_t>(rel));
}

void populate_probe_and_helpers(uint8_t *b, uint64_t entryOff,
                              uint64_t r16, uint64_t w16,
                              uint64_t r32, uint64_t w32,
                              uint64_t ri) {
    std::memcpy(b + entryOff, kPrologueSig, sizeof(kPrologueSig));
    std::memcpy(b + entryOff + 0x1Du, kCallCtxRead16, sizeof(kCallCtxRead16));
    std::memcpy(b + entryOff + 0x3Eu, kCallCtxWrite16, sizeof(kCallCtxWrite16));
    std::memcpy(b + entryOff + 0xBFu, kAnchorClassHigh2E, sizeof(kAnchorClassHigh2E));
    std::memcpy(b + entryOff + 0x11Cu, kCallCtxRead32, sizeof(kCallCtxRead32));
    std::memcpy(b + entryOff + 0x134u, kCallCtxWrite32, sizeof(kCallCtxWrite32));
    std::memcpy(b + entryOff + 0x197u, kAnchorBusNum32, sizeof(kAnchorBusNum32));
    std::memcpy(b + entryOff + kSite1RelOffset, kSite1Sig, sizeof(kSite1Sig));
    std::memcpy(b + entryOff + kSite2RelOffset, kSite2Sig, sizeof(kSite2Sig));
    std::memcpy(b + entryOff + 0x3CEu, kAnchorRangesRangeInitCtx, sizeof(kAnchorRangesRangeInitCtx));
    std::memcpy(b + entryOff + 0x3F6u, kAnchorFinalizeRange, sizeof(kAnchorFinalizeRange));

    write_e8(b, entryOff, kCallSiteRead16, r16);
    write_e8(b, entryOff, kCallSiteWrite16, w16);
    write_e8(b, entryOff, kCallSiteRead32, r32);
    write_e8(b, entryOff, kCallSiteWrite32, w32);
    write_e8(b, entryOff, kCallSiteRangeInit, ri);

    for (uint64_t h : { r16, w16, r32, w32, ri }) {
        std::memcpy(b + h, kHelperPrologue4, sizeof(kHelperPrologue4));
    }
}

SyntheticIopci make_valid_synthetic_iopci() {
    SyntheticIopci s;
    const size_t totalSize = 0x54000u;
    s.image.allocate(totalSize, 0x90u);

    MachHeader64 hdr {};
    hdr.magic = kMachMagic64;
    hdr.cputype = kCpuTypeX86_64;
    hdr.cpusubtype = 3u;
    hdr.filetype = kMhKextBundle;
    hdr.ncmds = 2u;
    hdr.sizeofcmds = sizeof(SegmentCommand64) + sizeof(Section64) + sizeof(UuidCommand);
    write_unaligned(s.image.data(), hdr);

    SegmentCommand64 seg {};
    seg.cmd = kLcSegment64;
    seg.cmdsize = sizeof(SegmentCommand64) + sizeof(Section64);
    std::memcpy(seg.segname, "__TEXT", 6);
    seg.vmaddr = 0xFFFFFF80029EA000ULL;
    seg.vmsize = totalSize;
    seg.fileoff = 0;
    seg.filesize = totalSize;
    seg.nsects = 1u;
    write_unaligned(s.image.data() + sizeof(MachHeader64), seg);

    Section64 sec {};
    std::memcpy(sec.sectname, "__text", 6);
    std::memcpy(sec.segname, "__TEXT", 6);
    sec.addr = seg.vmaddr + s.textOff;
    sec.size = s.textSize;
    sec.offset = static_cast<uint32_t>(s.textOff);
    write_unaligned(s.image.data() + sizeof(MachHeader64) + sizeof(SegmentCommand64), sec);

    UuidCommand uc {};
    uc.cmd = kLcUuid;
    uc.cmdsize = sizeof(UuidCommand);
    std::memcpy(uc.uuid, kExpectedIopciUuid, sizeof(kExpectedIopciUuid));
    write_unaligned(s.image.data() + sizeof(MachHeader64) + seg.cmdsize, uc);

    populate_probe_and_helpers(
        s.image.data(), s.entryOff,
        kExpectedRead16HdrOff, kExpectedWrite16HdrOff,
        kExpectedRead32HdrOff, kExpectedWrite32HdrOff,
        kExpectedRangeInitHdrOff);
    return s;
}

// ---- Simulated PCI Device, Config Space, and Bridge Tree ----------------------------------

struct FakeRange {
    uint8_t raw[0x80] {};

    uint64_t start() const { return read_unaligned<uint64_t>(raw + kOffRangeStart); }
    uint64_t size() const { return read_unaligned<uint64_t>(raw + kOffRangeSize); }
    uint64_t proposedSize() const { return read_unaligned<uint64_t>(raw + 0x20u); }
    uint64_t align() const { return read_unaligned<uint64_t>(raw + 0x38u); }
    uint64_t minAddr() const { return read_unaligned<uint64_t>(raw + kOffRangeMinAddr); }
    uint64_t maxAddr() const { return read_unaligned<uint64_t>(raw + kOffRangeMaxAddr); }
    uint8_t type() const { return read_unaligned<uint8_t>(raw + 0x50u); }
    uint32_t flags() const { return read_unaligned<uint32_t>(raw + kOffRangeFlags); }

    void setWindow(uint64_t s, uint64_t sz, FakeRange *nextRange = nullptr, void *allocs = nullptr) {
        std::memset(raw, 0, sizeof(raw));
        write_unaligned<uint64_t>(raw + kOffRangeStart, s);
        write_unaligned<uint64_t>(raw + kOffRangeSize, sz);
        write_unaligned<uint64_t>(raw + kOffRangeEnd, s + sz);
        write_unaligned<void *>(raw + kOffRangeNext, nextRange);
        write_unaligned<void *>(raw + kOffRangeAllocations,
                                allocs ? allocs : static_cast<void *>(raw + kOffRangeEnd));
    }

    void setAlloc(uint64_t s, uint64_t sz, void *nextSub = nullptr) {
        std::memset(raw, 0, sizeof(raw));
        write_unaligned<uint64_t>(raw + kOffRangeStart, s);
        write_unaligned<uint64_t>(raw + kOffRangeSize, sz);
        write_unaligned<uint64_t>(raw + kOffRangeEnd, s + sz);
        write_unaligned<void *>(raw + kOffRangeNextSub, nextSub);
    }
};

struct FakeConfigEntry {
    uint8_t raw[0x160] {};

    void setParent(FakeConfigEntry *p) { write_unaligned<void *>(raw + kOffEntryParent, p); }
    void setHostBridge(bool hb) { write_unaligned<uint8_t>(raw + kOffEntryIsHostBridge, hb ? 1u : 0u); }
    void setVendorDevice(uint16_t v, uint16_t d) {
        write_unaligned<uint32_t>(raw + kOffEntryVendorDevice,
                                  static_cast<uint32_t>(v) | (static_cast<uint32_t>(d) << 16));
    }
    void setClassCode(uint32_t cc) { write_unaligned<uint32_t>(raw + 0x2Cu, cc); }
    void setBusNum(uint8_t bus) { write_unaligned<uint8_t>(raw + kOffEntryBusNum, bus); }
    void setRangeSlot(uint32_t idx, void *r) {
        write_unaligned<void *>(raw + kOffEntryRanges + static_cast<size_t>(idx) * sizeof(void *), r);
    }
    void setBridgePfm(FakeRange *r) { write_unaligned<void *>(raw + kOffEntryBridgePfmRange, r); }
    void setBridgeMem(FakeRange *r) { write_unaligned<void *>(raw + kOffEntryBridgeMemRange, r); }
};

struct FakeConfigurator {
    uint8_t raw[0x100] {};
    void setFlags(uint32_t f) { write_unaligned<uint32_t>(raw + kOffConfiguratorFlags, f); }
    void setRoot(FakeConfigEntry *r) { write_unaligned<void *>(raw + kOffConfiguratorRoot, r); }
};

struct PciSim {
    uint16_t cmd { 0x0007u };
    uint32_t cfg[64] {}; // dwords 0x00..0xFC
    uint32_t mask[64] {};
    uint32_t readCalls { 0 };
    uint32_t writeCalls { 0 };
    uint32_t cmdDisabledDuringProbe { 0 };
    FakeRange ranges[7] {};
};

static PciSim *gSim = nullptr;

uint16_t sim_read16(void *, void *, uint32_t off, void *) {
    ++gSim->readCalls;
    if (off == 0x04u) return gSim->cmd;
    return 0;
}
void sim_write16(void *, void *, uint32_t off, uint16_t val, void *) {
    ++gSim->writeCalls;
    if (off == 0x04u) {
        gSim->cmd = val;
        if ((val & 0x3u) == 0u) ++gSim->cmdDisabledDuringProbe;
    }
}
uint32_t sim_read32(void *, void *, uint32_t off, void *) {
    ++gSim->readCalls;
    const uint32_t idx = off >> 2;
    return idx < 64u ? gSim->cfg[idx] : 0u;
}
void sim_write32(void *, void *, uint32_t off, uint32_t val, void *) {
    ++gSim->writeCalls;
    const uint32_t idx = off >> 2;
    if (idx >= 64u) return;
    if ((off >= 0x10u && off <= 0x24u && val == 0xFFFFFFFFu) ||
        (off == 0x30u && val == 0xFFFFF800u)) {
        gSim->cfg[idx] = gSim->mask[idx];
    } else {
        gSim->cfg[idx] = val;
    }
}
void sim_range_init(void *range, uint32_t type, uint64_t start, uint64_t size, uint64_t align) {
    uint8_t *r = static_cast<uint8_t *>(range);
    std::memset(r, 0, 0x80u);
    write_unaligned<uint8_t>(r + 0x50u, static_cast<uint8_t>(type));
    write_unaligned<uint64_t>(r + 0x00u, start);
    write_unaligned<uint64_t>(r + 0x10u, size);
    write_unaligned<uint64_t>(r + 0x20u, size);
    write_unaligned<uint64_t>(r + 0x28u, start);
    write_unaligned<uint64_t>(r + 0x38u, align ? align : size);
    write_unaligned<uint64_t>(r + 0x48u, 0xFFFFFFFFULL);
}

ProbeOps sim_ops() {
    return ProbeOps { sim_read16, sim_write16, sim_read32, sim_write32, sim_range_init };
}

struct TestRig {
    FakeConfigurator cfg;
    FakeConfigEntry root;
    FakeConfigEntry hb;
    FakeConfigEntry gpp0;
    FakeConfigEntry swUp;
    FakeConfigEntry swDn;
    FakeConfigEntry dev;
    FakeRange hbPfm;
    PciSim pci;

    void initR9060XT16G() {
        cfg.setFlags(kIOPCIConfiguratorPFM64);
        cfg.setRoot(&root);
        hb.setParent(&root);
        hb.setHostBridge(true);
        hbPfm.setWindow(0x460000000ULL, 0xF8A0000000ULL);
        hb.setBridgePfm(&hbPfm);

        gpp0.setParent(&hb);
        swUp.setParent(&gpp0);
        swDn.setParent(&swUp);
        dev.setParent(&swDn);
        dev.setVendorDevice(kVendorAmd, kDevIdNavi44);
        dev.setClassCode(0x030000u);
        dev.setBusNum(3u);
        for (uint32_t i = 0; i < 7u; ++i) {
            std::memset(pci.ranges[i].raw, 0, sizeof(pci.ranges[i].raw));
            dev.setRangeSlot(i, &pci.ranges[i]);
        }

        // RX 9060 XT 16 GiB BAR0 + 2 MiB BAR2 + 256 B BAR4 + 512 KiB BAR5 + 128 KiB ROM.
        pci.cmd = 0x0007u;
        pci.cfg[0x10 >> 2] = 0x0000000Cu;
        pci.cfg[0x14 >> 2] = 0x000000F8u;
        pci.mask[0x10 >> 2] = 0x0000000Cu;
        pci.mask[0x14 >> 2] = 0xFFFFFFFCu; // 16 GiB

        pci.cfg[0x18 >> 2] = 0x0000000Cu;
        pci.cfg[0x1C >> 2] = 0x000000FCu;
        pci.mask[0x18 >> 2] = 0xFFE0000Cu;
        pci.mask[0x1C >> 2] = 0xFFFFFFFFu; // 2 MiB

        pci.cfg[0x20 >> 2] = 0x0000E001u;
        pci.mask[0x20 >> 2] = 0xFFFFFF01u; // 256 B IO

        pci.cfg[0x24 >> 2] = 0xF0000000u;
        pci.mask[0x24 >> 2] = 0xFFF80000u; // 512 KiB MMIO

        pci.cfg[0x30 >> 2] = 0xF0100000u;
        pci.mask[0x30 >> 2] = 0xFFFE0000u; // 128 KiB ROM

        gSim = &pci;
    }
};

// ---- Tests ---------------------------------------------------------------------------------

void test_real_bootkc_if_available() {
    const char *candidates[] = {
        "/home/gdzzc/Projects/WhateverRed-Navi44/build-offline/vm-25G332/kc/BootKernelExtensions.kc",
        "../../build-offline/vm-25G332/kc/BootKernelExtensions.kc",
    };
    PageAlignedBuffer kc;
    for (const char *p : candidates) {
        std::ifstream ifs(p, std::ios::binary);
        if (ifs) {
            ifs.seekg(0, std::ios::end);
            const std::streamoff sz = ifs.tellg();
            if (sz > 0x3000000) {
                kc.allocate(static_cast<size_t>(sz));
                ifs.seekg(0, std::ios::beg);
                if (ifs.read(reinterpret_cast<char *>(kc.data()), sz)) break;
                kc.clear();
            }
        }
    }
    if (kc.empty()) {
        std::printf("note: BootKernelExtensions.kc not found locally; skipping real-KC scan\n");
        return;
    }

    const MachHeader64 kcHdr = read_unaligned<MachHeader64>(kc.data());
    CHECK(kcHdr.magic == kMachMagic64);

    // Find com.apple.iokit.IOPCIFamily LC_FILESET_ENTRY (cmd == 0x80000035).
    size_t off = sizeof(MachHeader64);
    uint64_t iopciFileOff = 0;
    bool foundEntry = false;
    for (uint32_t i = 0; i < kcHdr.ncmds && off + 8 <= kc.size(); ++i) {
        const LoadCommand lc = read_unaligned<LoadCommand>(kc.data() + off);
        if (lc.cmd == 0x80000035u && lc.cmdsize >= 32u && off + lc.cmdsize <= kc.size()) {
            const uint64_t fileoff = read_unaligned<uint64_t>(kc.data() + off + 16u);
            const uint32_t nameOff = read_unaligned<uint32_t>(kc.data() + off + 24u);
            if (nameOff < lc.cmdsize) {
                const char *name = reinterpret_cast<const char *>(kc.data() + off + nameOff);
                const size_t maxName = lc.cmdsize - nameOff;
                if (strnlen(name, maxName) < maxName &&
                    std::strcmp(name, "com.apple.iokit.IOPCIFamily") == 0) {
                    iopciFileOff = fileoff;
                    foundEntry = true;
                    break;
                }
            }
        }
        off += lc.cmdsize;
    }
    CHECK(foundEntry);
    if (!foundEntry || iopciFileOff >= kc.size()) return;

    const uintptr_t kcStart = reinterpret_cast<uintptr_t>(kc.data());
    const uintptr_t kcEnd = kcStart + kc.size();
    const uintptr_t anchor = kcStart + static_cast<uintptr_t>(iopciFileOff + 0x2076u);
    const void *mh = find_mach_header_backward(anchor, kcStart, kcEnd);
    CHECK(mh == kc.data() + iopciFileOff);

    VerifiedTarget vt {};
    const CompatStatus st = verify_iopcifamily_target(
        mh, kc.size() - static_cast<size_t>(iopciFileOff), anchor, vt);
    CHECK(st == CompatStatus::kInstalled);
    CHECK(vt.uuidVerified);
    CHECK(vt.entryHdrOff == kExpectedEntryHdrOff);
    CHECK(vt.resumeAddr - vt.entryAddr == kStolenPrologueBytes);
    CHECK(vt.fnConfigRead16 - vt.headerAddr == kExpectedRead16HdrOff);
    CHECK(vt.fnConfigWrite16 - vt.headerAddr == kExpectedWrite16HdrOff);
    CHECK(vt.fnConfigRead32 - vt.headerAddr == kExpectedRead32HdrOff);
    CHECK(vt.fnConfigWrite32 - vt.headerAddr == kExpectedWrite32HdrOff);
    CHECK(vt.fnRangeInit - vt.headerAddr == kExpectedRangeInitHdrOff);
    CHECK(mem_eq(vt.stolenBytes, kPrologueSig, kStolenPrologueBytes));
    std::printf("real BootKernelExtensions.kc IOPCIFamily verification passed\n");
}

void test_macho_and_signature_verification() {
    auto syn = make_valid_synthetic_iopci();
    const uintptr_t base = reinterpret_cast<uintptr_t>(syn.image.data());
    const uintptr_t anchor = base + syn.anchorOff;

    // 1. Valid synthetic image + backward scan (exact-uuid fast path).
    CHECK(find_mach_header_backward(anchor, base, base + syn.image.size()) == syn.image.data());
    VerifiedTarget vt {};
    CHECK(verify_iopcifamily_target(syn.image.data(), syn.image.size(), anchor, vt) ==
          CompatStatus::kInstalled);
    CHECK(vt.uuidVerified);
    CHECK(vt.matchMode == MatchMode::kExactUuid);
    CHECK(vt.hasDetectedUuid);
    {
        char ustr[37] {};
        format_uuid(vt.detectedUuid, ustr);
        CHECK(std::strcmp(ustr, "227771C6-ADC2-30C7-8408-263F723CF81C") == 0);
    }
    CHECK(vt.entryHdrOff == kExpectedEntryHdrOff);

    // 2. False-positive 0xFEEDFACF on page 1 before anchor must not fool backward scan.
    {
        auto s2 = syn;
        MachHeader64 fake {};
        fake.magic = kMachMagic64;
        fake.cputype = kCpuTypeX86_64;
        fake.filetype = kMhKextBundle;
        write_unaligned(s2.image.data() + 0x1000u, fake);
        const uintptr_t b2 = reinterpret_cast<uintptr_t>(s2.image.data());
        CHECK(find_mach_header_backward(b2 + s2.anchorOff, b2, b2 + s2.image.size()) ==
              s2.image.data());
    }

    // 3. Too far back (> 16 pages) fails header lookup.
    CHECK(find_mach_header_backward(anchor, base + 0x2000u, base + syn.image.size()) == nullptr);
    CHECK(find_mach_header_backward(base + 0x12000u, base, base + syn.image.size()) == nullptr);

    // 4a. Sibling macOS 26.x UUID + shifted function & helper offsets -> structural match.
    {
        auto s2 = syn;
        const size_t uuidOff = sizeof(MachHeader64) + sizeof(SegmentCommand64) + sizeof(Section64) + 8u;
        s2.image[uuidOff] ^= 0x01u;
        std::memset(s2.image.data() + s2.textOff, 0x90, s2.textSize);
        const uint64_t shifted = s2.entryOff + 0x400u;
        const uint64_t r16 = kExpectedRead16HdrOff + 0x180u;
        const uint64_t w16 = kExpectedWrite16HdrOff + 0x180u;
        const uint64_t r32 = kExpectedRead32HdrOff + 0x180u;
        const uint64_t w32 = kExpectedWrite32HdrOff + 0x180u;
        const uint64_t ri = kExpectedRangeInitHdrOff + 0x180u;
        populate_probe_and_helpers(s2.image.data(), shifted, r16, w16, r32, w32, ri);

        VerifiedTarget v2 {};
        CHECK(verify_iopcifamily_target(s2.image.data(), s2.image.size(), 0u, v2) ==
              CompatStatus::kInstalled);
        CHECK(!v2.uuidVerified);
        CHECK(v2.matchMode == MatchMode::kStructural);
        CHECK(v2.hasDetectedUuid);
        CHECK(v2.entryHdrOff == shifted);
        CHECK(v2.fnConfigRead16 - v2.headerAddr == r16);
        CHECK(v2.fnConfigWrite16 - v2.headerAddr == w16);
        CHECK(v2.fnConfigRead32 - v2.headerAddr == r32);
        CHECK(v2.fnConfigWrite32 - v2.headerAddr == w32);
        CHECK(v2.fnRangeInit - v2.headerAddr == ri);

        // 4b. Structural fails closed if a struct displacement byte changes (e.g. fFlags 0x91 -> 0x99).
        auto sBadStruct = s2;
        sBadStruct.image[shifted + 0x3F6u + 21u] = 0x99u;
        VerifiedTarget vBadStruct {};
        CHECK(verify_iopcifamily_target(sBadStruct.image.data(), sBadStruct.image.size(), 0u, vBadStruct) ==
              CompatStatus::kStructLayoutMismatch);
        CHECK(vBadStruct.entryAddr == 0u);

        // 4c. Structural fails closed if two helpers alias or lack push-rbp prologue.
        auto sAlias = s2;
        write_e8(sAlias.image.data(), shifted, kCallSiteWrite16, r16);
        VerifiedTarget vAlias {};
        CHECK(verify_iopcifamily_target(sAlias.image.data(), sAlias.image.size(), 0u, vAlias) ==
              CompatStatus::kHelperCallMismatch);

        auto sNoPro = s2;
        sNoPro.image[ri] = 0xC3u;
        VerifiedTarget vNoPro {};
        CHECK(verify_iopcifamily_target(sNoPro.image.data(), sNoPro.image.size(), 0u, vNoPro) ==
              CompatStatus::kHelperCallMismatch);

        // 4d. All-zero UUID -> kUuidMismatch.
        auto sZeroUuid = s2;
        std::memset(sZeroUuid.image.data() + uuidOff, 0, 16u);
        VerifiedTarget vZero {};
        CHECK(verify_iopcifamily_target(sZeroUuid.image.data(), sZeroUuid.image.size(), 0u, vZero) ==
              CompatStatus::kUuidMismatch);
        CHECK(!vZero.uuidVerified);
        CHECK(vZero.entryAddr == 0u);
    }

    // 5. Missing UUID -> kUuidMismatch.
    {
        auto s2 = syn;
        MachHeader64 h = read_unaligned<MachHeader64>(s2.image.data());
        h.ncmds = 1u;
        h.sizeofcmds = sizeof(SegmentCommand64) + sizeof(Section64);
        write_unaligned(s2.image.data(), h);
        VerifiedTarget v2 {};
        CHECK(verify_iopcifamily_target(s2.image.data(), s2.image.size(), 0u, v2) ==
              CompatStatus::kUuidMismatch);
    }

    // 6. Wrong filetype / truncated / bad Mach-O -> kBadMachO.
    {
        auto s2 = syn;
        MachHeader64 h = read_unaligned<MachHeader64>(s2.image.data());
        h.filetype = 2u; // MH_EXECUTE
        write_unaligned(s2.image.data(), h);
        VerifiedTarget v2 {};
        CHECK(verify_iopcifamily_target(s2.image.data(), s2.image.size(), 0u, v2) ==
              CompatStatus::kBadMachO);
        CHECK(verify_iopcifamily_target(syn.image.data(), 16u, 0u, v2) ==
              CompatStatus::kBadMachO);
        CHECK(verify_iopcifamily_target(syn.image.data(), syn.textOff + 100u, 0u, v2) ==
              CompatStatus::kBadMachO);
    }

    // 7. Missing prologue / site1 / site2 -> kPatternMissing.
    for (size_t corruptOff : { syn.entryOff + 5u,
                               syn.entryOff + kSite1RelOffset + 3u,
                               syn.entryOff + kSite2RelOffset + 4u }) {
        auto s2 = syn;
        s2.image[corruptOff] ^= 0xFFu;
        VerifiedTarget v2 {};
        CHECK(verify_iopcifamily_target(s2.image.data(), s2.image.size(), 0u, v2) ==
              CompatStatus::kPatternMissing);
    }

    // 8. Duplicate prologue / site1 / site2 -> kPatternDuplicate.
    {
        auto s2 = syn;
        std::memcpy(s2.image.data() + syn.textOff + 0x100u, kPrologueSig, sizeof(kPrologueSig));
        VerifiedTarget v2 {};
        CHECK(verify_iopcifamily_target(s2.image.data(), s2.image.size(), 0u, v2) ==
              CompatStatus::kPatternDuplicate);
    }
    {
        auto s2 = syn;
        std::memcpy(s2.image.data() + syn.textOff + 0x200u, kSite1Sig, sizeof(kSite1Sig));
        VerifiedTarget v2 {};
        CHECK(verify_iopcifamily_target(s2.image.data(), s2.image.size(), 0u, v2) ==
              CompatStatus::kPatternDuplicate);
    }
    {
        auto s2 = syn;
        std::memcpy(s2.image.data() + syn.textOff + 0x300u, kSite2Sig, sizeof(kSite2Sig));
        VerifiedTarget v2 {};
        CHECK(verify_iopcifamily_target(s2.image.data(), s2.image.size(), 0u, v2) ==
              CompatStatus::kPatternDuplicate);
    }

    // 9. Shifted site1 / shifted site2 / shifted entry -> kPatternOffsetMismatch.
    {
        auto s2 = syn;
        std::memset(s2.image.data() + syn.entryOff + kSite1RelOffset, 0x90, sizeof(kSite1Sig));
        std::memcpy(s2.image.data() + syn.entryOff + kSite1RelOffset + 1u, kSite1Sig, sizeof(kSite1Sig));
        VerifiedTarget v2 {};
        CHECK(verify_iopcifamily_target(s2.image.data(), s2.image.size(), 0u, v2) ==
              CompatStatus::kPatternOffsetMismatch);
    }
    {
        auto s2 = syn;
        std::memset(s2.image.data() + syn.entryOff + kSite2RelOffset, 0x90, sizeof(kSite2Sig));
        std::memcpy(s2.image.data() + syn.entryOff + kSite2RelOffset - 1u, kSite2Sig, sizeof(kSite2Sig));
        VerifiedTarget v2 {};
        CHECK(verify_iopcifamily_target(s2.image.data(), s2.image.size(), 0u, v2) ==
              CompatStatus::kPatternOffsetMismatch);
    }
    {
        auto s2 = syn;
        std::memset(s2.image.data() + syn.entryOff, 0x90, 0x450u);
        const uint64_t shifted = syn.entryOff + 0x100u;
        std::memcpy(s2.image.data() + shifted, kPrologueSig, sizeof(kPrologueSig));
        std::memcpy(s2.image.data() + shifted + kSite1RelOffset, kSite1Sig, sizeof(kSite1Sig));
        std::memcpy(s2.image.data() + shifted + kSite2RelOffset, kSite2Sig, sizeof(kSite2Sig));
        write_e8(s2.image.data(), shifted, kCallSiteRead16, kExpectedRead16HdrOff);
        write_e8(s2.image.data(), shifted, kCallSiteWrite16, kExpectedWrite16HdrOff);
        write_e8(s2.image.data(), shifted, kCallSiteRead32, kExpectedRead32HdrOff);
        write_e8(s2.image.data(), shifted, kCallSiteWrite32, kExpectedWrite32HdrOff);
        write_e8(s2.image.data(), shifted, kCallSiteRangeInit, kExpectedRangeInitHdrOff);
        VerifiedTarget v2 {};
        CHECK(verify_iopcifamily_target(s2.image.data(), s2.image.size(), 0u, v2) ==
              CompatStatus::kPatternOffsetMismatch);
    }

    // 10. Helper call mismatch (each of the 5 call sites).
    for (size_t callOff : { kCallSiteRead16, kCallSiteWrite16, kCallSiteRead32,
                            kCallSiteWrite32, kCallSiteRangeInit }) {
        auto s2 = syn;
        s2.image[syn.entryOff + callOff + 1u] ^= 0x04u;
        VerifiedTarget v2 {};
        CHECK(verify_iopcifamily_target(s2.image.data(), s2.image.size(), 0u, v2) ==
              CompatStatus::kHelperCallMismatch);
    }
}

void test_policy_and_detour_encoding() {
    CHECK(evaluate_load_policy(false, 0u, 0u, 0u) == CompatStatus::kInstalled);
    CHECK(evaluate_load_policy(true, 1u, 0u, 0u) == CompatStatus::kInstalled);
    CHECK(evaluate_load_policy(true, 0u, 0u, 0u) == CompatStatus::kDisabledByBootArg);
    CHECK(evaluate_load_policy(true, 0u, 5u, 2u) == CompatStatus::kDisabledByBootArg);
    CHECK(evaluate_load_policy(false, 1u, 1u, 0u) == CompatStatus::kLateLoad);
    CHECK(evaluate_load_policy(false, 1u, 0u, 1u) == CompatStatus::kLateLoad);

    uint8_t tramp[kStolenPrologueBytes + 14u] {};
    encode_trampoline31(tramp, kPrologueSig, 0xFFFFFF8002A2113FULL);
    CHECK(mem_eq(tramp, kPrologueSig, kStolenPrologueBytes));
    CHECK(tramp[17] == 0xFFu && tramp[18] == 0x25u &&
          tramp[19] == 0u && tramp[20] == 0u && tramp[21] == 0u && tramp[22] == 0u);
    CHECK(read_unaligned<uint64_t>(tramp + 23u) == 0xFFFFFF8002A2113FULL);

    uint8_t detour[kStolenPrologueBytes] {};
    encode_entry_detour17(detour, 0xFFFFFF8012345678ULL);
    CHECK(detour[0] == 0xFFu && detour[1] == 0x25u);
    CHECK(read_unaligned<uint64_t>(detour + 6u) == 0xFFFFFF8012345678ULL);
    CHECK(detour[14] == 0x90u && detour[15] == 0x90u && detour[16] == 0x90u);
}

void test_non_target_and_device_allowlist() {
    TestRig rig;
    rig.initR9060XT16G();
    ProbeCounters cnt {};
    const ProbeOps ops = sim_ops();

    // Type-1 PCI bridge (lastBarNum == 1) -> passthrough, zero config reads.
    CHECK(!handle_probe_base_address_register(ops, cnt, &rig.cfg, &rig.dev, 1u, 0u));
    CHECK(cnt.passthroughCount == 1u);
    CHECK(rig.pci.readCalls == 0u && rig.pci.writeCalls == 0u);

    // Non-RDNA4 AMD device (1002:73BF) -> passthrough, zero config reads.
    rig.dev.setVendorDevice(kVendorAmd, 0x73BFu);
    CHECK(!handle_probe_base_address_register(ops, cnt, &rig.cfg, &rig.dev, 6u, 0u));
    CHECK(cnt.passthroughCount == 2u);
    CHECK(rig.pci.readCalls == 0u);

    // Intel device (8086:27A0) -> passthrough, zero config reads.
    rig.dev.setVendorDevice(0x8086u, 0x27A0u);
    CHECK(!handle_probe_base_address_register(ops, cnt, &rig.cfg, &rig.dev, 6u, 0u));
    CHECK(cnt.passthroughCount == 3u);
    CHECK(rig.pci.readCalls == 0u);

    CHECK(max_allowed_bar0_for_device(kVendorAmd, kDevIdNavi44) == (16ULL << 30));
    CHECK(max_allowed_bar0_for_device(kVendorAmd, kDevIdR9070) == (16ULL << 30));
    CHECK(max_allowed_bar0_for_device(kVendorAmd, kDevIdR9700) == (32ULL << 30));
    CHECK(max_allowed_bar0_for_device(kVendorAmd, 0x744Cu) == 0ULL);
    CHECK(max_allowed_bar0_for_device(0x10DEu, kDevIdNavi44) == 0ULL);
}

void test_16g_and_32g_preservation_and_bar2_integrity() {
    TestRig rig;
    rig.initR9060XT16G();
    ProbeCounters cnt {};
    const ProbeOps ops = sim_ops();

    CHECK(handle_probe_base_address_register(ops, cnt, &rig.cfg, &rig.dev, 6u, 0u));
    CHECK(cnt.interceptCount == 1u);
    CHECK(cnt.preservedBar0Bytes == (16ULL << 30));
    CHECK(cnt.deviceId == kDevIdNavi44);
    CHECK(cnt.refusedBridgeCount == 0u);
    CHECK(cnt.passthroughCount == 0u);
    CHECK(rig.pci.cmdDisabledDuringProbe >= 1u);
    CHECK(rig.pci.cmd == 0x0007u);
    // Original BAR0 and BAR2 dwords preserved intact.
    CHECK(rig.pci.cfg[0x10 >> 2] == 0x0000000Cu);
    CHECK(rig.pci.cfg[0x14 >> 2] == 0x000000F8u);
    CHECK(rig.pci.cfg[0x18 >> 2] == 0x0000000Cu);
    CHECK(rig.pci.cfg[0x1C >> 2] == 0x000000FCu);

    // BAR0 preserved at 16 GiB, 64-bit PFM, maxAddress = ~0ULL.
    CHECK(rig.pci.ranges[0].type() == kIOPCIResourcePrefetchMemory);
    CHECK(rig.pci.ranges[0].start() == 0xF800000000ULL);
    CHECK(rig.pci.ranges[0].proposedSize() == (16ULL << 30));
    CHECK(rig.pci.ranges[0].align() == (16ULL << 30));
    CHECK(rig.pci.ranges[0].minAddr() == 0x100000ULL);
    CHECK(rig.pci.ranges[0].maxAddr() == ~0ULL);
    CHECK((rig.pci.ranges[0].flags() & kIOPCIRangeFlag64Bit) != 0u);

    // BAR1 slot (0x14) MUST remain untouched (proposedSize == 0) so BAR2 is uncorrupted.
    CHECK(rig.pci.ranges[1].proposedSize() == 0ULL);

    // BAR2 doorbell MUST be cleanly probed at 0x18 (2 MiB, 64-bit PFM).
    CHECK(rig.pci.ranges[2].type() == kIOPCIResourcePrefetchMemory);
    CHECK(rig.pci.ranges[2].start() == 0xFC00000000ULL);
    CHECK(rig.pci.ranges[2].proposedSize() == (2ULL << 20));
    CHECK(rig.pci.ranges[2].minAddr() == 0x100000ULL);
    CHECK(rig.pci.ranges[2].maxAddr() == ~0ULL);
    CHECK((rig.pci.ranges[2].flags() & kIOPCIRangeFlag64Bit) != 0u);

    // BAR3 untouched, BAR4 = 256 B IO, BAR5 = 512 KiB MEM, ROM = 128 KiB MEM.
    CHECK(rig.pci.ranges[3].proposedSize() == 0ULL);
    CHECK(rig.pci.ranges[4].type() == kIOPCIResourceIO);
    CHECK(rig.pci.ranges[4].proposedSize() == 256ULL);
    CHECK(rig.pci.ranges[4].minAddr() == 0x400ULL);
    CHECK(rig.pci.ranges[5].type() == kIOPCIResourceMemory);
    CHECK(rig.pci.ranges[5].proposedSize() == (512ULL << 10));
    CHECK(rig.pci.ranges[5].minAddr() == 0x100000ULL);
    CHECK(rig.pci.ranges[6].type() == kIOPCIResourceMemory);
    CHECK(rig.pci.ranges[6].proposedSize() == (128ULL << 10));

    // Reset mask clears BAR0 & BAR2 & ROM starts cleanly; non-display skips ROM.
    rig.initR9060XT16G();
    rig.dev.setClassCode(0x040300u);
    CHECK(handle_probe_base_address_register(ops, cnt, &rig.cfg, &rig.dev, 6u, 0x7u));
    CHECK(rig.pci.ranges[0].start() == 0ULL);
    CHECK(rig.pci.ranges[2].start() == 0ULL);
    CHECK(rig.pci.ranges[6].proposedSize() == 0ULL);

    // BAR2 > 1 GiB still obeys Apple's 1 GiB MAX_BAR_SIZE (BAR0-only preservation).
    rig.initR9060XT16G();
    rig.pci.mask[0x18 >> 2] = 0x8000000Cu; // 2 GiB
    rig.pci.mask[0x1C >> 2] = 0xFFFFFFFFu;
    CHECK(handle_probe_base_address_register(ops, cnt, &rig.cfg, &rig.dev, 5u, 0u));
    CHECK(rig.pci.ranges[0].proposedSize() == (16ULL << 30));
    CHECK(rig.pci.ranges[2].proposedSize() == 0x80000000u);
    CHECK((rig.pci.ranges[2].flags() & kIOPCIRangeFlag64Bit) == 0u);

    // 0x7550 supports 16 GiB, rejects 32 GiB; 0x7551 supports 32 GiB, rejects 64 GiB.
    rig.initR9060XT16G();
    rig.dev.setVendorDevice(kVendorAmd, kDevIdR9070);
    CHECK(handle_probe_base_address_register(ops, cnt, &rig.cfg, &rig.dev, 6u, 0u));
    rig.pci.mask[0x14 >> 2] = 0xFFFFFFF8u; // 32 GiB
    CHECK(!handle_probe_base_address_register(ops, cnt, &rig.cfg, &rig.dev, 6u, 0u));
    CHECK(rig.pci.cmd == 0x0007u);

    rig.dev.setVendorDevice(kVendorAmd, kDevIdR9700);
    CHECK(handle_probe_base_address_register(ops, cnt, &rig.cfg, &rig.dev, 6u, 0u));
    CHECK(cnt.preservedBar0Bytes == (32ULL << 30));
    rig.pci.mask[0x14 >> 2] = 0xFFFFFFF0u; // 64 GiB -> oversized
    CHECK(!handle_probe_base_address_register(ops, cnt, &rig.cfg, &rig.dev, 6u, 0u));
}

void test_small_bar_passthrough_and_invalid_bar0() {
    TestRig rig;
    const ProbeOps ops = sim_ops();
    ProbeCounters cnt {};

    // 256 MiB small-BAR recovery -> passthrough.
    rig.initR9060XT16G();
    rig.pci.mask[0x10 >> 2] = 0xF000000Cu;
    rig.pci.mask[0x14 >> 2] = 0xFFFFFFFFu;
    CHECK(!handle_probe_base_address_register(ops, cnt, &rig.cfg, &rig.dev, 6u, 0u));
    CHECK(cnt.interceptCount == 0u);
    CHECK(cnt.refusedBridgeCount == 0u);
    CHECK(cnt.passthroughCount == 1u);
    CHECK(rig.pci.cmd == 0x0007u);

    // 1 GiB BAR0 -> passthrough (stock Apple handles <= 1 GiB).
    rig.initR9060XT16G();
    rig.pci.mask[0x10 >> 2] = 0xC000000Cu;
    rig.pci.mask[0x14 >> 2] = 0xFFFFFFFFu;
    CHECK(!handle_probe_base_address_register(ops, cnt, &rig.cfg, &rig.dev, 6u, 0u));
    CHECK(cnt.refusedBridgeCount == 0u);

    // Non-prefetchable 64-bit -> rejected -> passthrough.
    rig.initR9060XT16G();
    rig.pci.mask[0x10 >> 2] = 0x00000004u;
    CHECK(!handle_probe_base_address_register(ops, cnt, &rig.cfg, &rig.dev, 6u, 0u));

    // 32-bit prefetchable -> rejected -> passthrough.
    rig.initR9060XT16G();
    rig.pci.mask[0x10 >> 2] = 0x00000008u;
    CHECK(!handle_probe_base_address_register(ops, cnt, &rig.cfg, &rig.dev, 6u, 0u));

    // Zero mask -> rejected -> passthrough.
    rig.initR9060XT16G();
    rig.pci.mask[0x10 >> 2] = 0x0000000Cu;
    rig.pci.mask[0x14 >> 2] = 0x00000000u;
    CHECK(!handle_probe_base_address_register(ops, cnt, &rig.cfg, &rig.dev, 6u, 0u));

    // Non-power-of-two (24 GiB) -> rejected -> passthrough.
    rig.initR9060XT16G();
    rig.pci.mask[0x14 >> 2] = 0xFFFFFFFAu;
    CHECK(!handle_probe_base_address_register(ops, cnt, &rig.cfg, &rig.dev, 6u, 0u));
    CHECK(rig.pci.cmd == 0x0007u);
}

void test_bridge_pfm64_sufficiency_and_bounds() {
    TestRig rig;
    const ProbeOps ops = sim_ops();
    ProbeCounters cnt {};

    // 1. PFM64 disabled (npci=0x2000) -> RefusedBridgeCount + passthrough.
    rig.initR9060XT16G();
    rig.cfg.setFlags(0u);
    CHECK(!handle_probe_base_address_register(ops, cnt, &rig.cfg, &rig.dev, 6u, 0u));
    CHECK(cnt.refusedBridgeCount == 1u);
    CHECK(cnt.passthroughCount == 1u);
    CHECK(rig.pci.cmd == 0x0007u);

    // 2. Synthetic 2 GiB root window < 16 GiB -> refused.
    rig.initR9060XT16G();
    rig.hbPfm.setWindow(0x100000000ULL, 2ULL << 30);
    CHECK(!handle_probe_base_address_register(ops, cnt, &rig.cfg, &rig.dev, 6u, 0u));
    CHECK(cnt.refusedBridgeCount == 2u);

    // 3. Unaligned 16 GiB window [24 GiB, 40 GiB): aligned 32 GiB leaves only 8 GiB -> refused!
    rig.initR9060XT16G();
    rig.hbPfm.setWindow(0x600000000ULL, 16ULL << 30);
    CHECK(!handle_probe_base_address_register(ops, cnt, &rig.cfg, &rig.dev, 6u, 0u));
    CHECK(cnt.refusedBridgeCount == 3u);

    // 4. Fragmented by allocation [32 GiB, 33 GiB) inside [17.5 GiB, 48 GiB) -> refused!
    // Terminates with Apple's real interior sentinel (&hbPfm.raw[kOffRangeEnd]).
    rig.initR9060XT16G();
    FakeRange alloc1;
    alloc1.setAlloc(0x800000000ULL, 1ULL << 30, rig.hbPfm.raw + kOffRangeEnd);
    rig.hbPfm.setWindow(0x460000000ULL, 0x7A0000000ULL, nullptr, &alloc1); // ends at 48 GiB
    CHECK(!handle_probe_base_address_register(ops, cnt, &rig.cfg, &rig.dev, 6u, 0u));
    CHECK(cnt.refusedBridgeCount == 4u);

    // 5. Same allocation + Apple &hbPfm.end tail sentinel but window ends at 64 GiB -> fits [48 GiB, 64 GiB)!
    rig.hbPfm.setWindow(0x460000000ULL, 0xBA0000000ULL, nullptr, &alloc1); // ends at 64 GiB
    CHECK(handle_probe_base_address_register(ops, cnt, &rig.cfg, &rig.dev, 6u, 0u));
    CHECK(cnt.interceptCount == 1u);

    // 5b. Exact 0x80-byte IOPCIRange with Apple interior &r->end sentinel (empty & trailing gap) + zero-size sub node:
    // under ASan, reading r + kOffRangeEnd + kOffRangeNextSub (r + 0x88) traps immediately.
    {
        std::vector<uint8_t> exactObj(0x80u, 0u);
        uint8_t *r = exactObj.data();
        write_unaligned<uint64_t>(r + kOffRangeStart, 0x460000000ULL);
        write_unaligned<uint64_t>(r + kOffRangeSize, 0xF8A0000000ULL);
        write_unaligned<uint64_t>(r + kOffRangeEnd, 0x460000000ULL + 0xF8A0000000ULL);
        write_unaligned<void *>(r + kOffRangeAllocations, r + kOffRangeEnd);
        rig.hb.setBridgePfm(reinterpret_cast<FakeRange *>(r));
        CHECK(check_root_pfm64_capacity(&rig.cfg, &rig.dev, 16ULL << 30));

        // Non-empty alloc whose nextSub is a zero-size node inside an exact 0x10-byte buffer.
        std::vector<uint8_t> zeroSub(0x10u, 0u);
        FakeRange aExact;
        aExact.setAlloc(0x800000000ULL, 1ULL << 30, zeroSub.data());
        write_unaligned<void *>(r + kOffRangeAllocations, &aExact);
        CHECK(check_root_pfm64_capacity(&rig.cfg, &rig.dev, 16ULL << 30));
    }

    // 5c. Null allocHead legacy fixture and null bar0Range / start64==mask64 guards.
    {
        rig.initR9060XT16G();
        write_unaligned<void *>(rig.hbPfm.raw + kOffRangeAllocations, nullptr);
        CHECK(check_root_pfm64_capacity(&rig.cfg, &rig.dev, 16ULL << 30));

        const uint64_t prevIntercepts = cnt.interceptCount;
        const uint64_t prevPassthroughs = cnt.passthroughCount;
        rig.dev.setRangeSlot(0u, nullptr);
        CHECK(!handle_probe_base_address_register(ops, cnt, &rig.cfg, &rig.dev, 6u, 0u));
        CHECK(cnt.interceptCount == prevIntercepts);
        CHECK(cnt.passthroughCount == prevPassthroughs + 1u);

        rig.initR9060XT16G();
        rig.pci.cfg[0x10 >> 2] = rig.pci.mask[0x10 >> 2];
        rig.pci.cfg[0x14 >> 2] = rig.pci.mask[0x14 >> 2];
        CHECK(handle_probe_base_address_register(ops, cnt, &rig.cfg, &rig.dev, 6u, 0u));
        CHECK(cnt.interceptCount == prevIntercepts);
        CHECK(cnt.passthroughCount == prevPassthroughs + 2u);
        CHECK(rig.pci.ranges[0].proposedSize() == 0ULL);
    }

    // 6. PFM null + MEM fallback >= 16 GiB -> accepted (matches Apple bridgeTotalResources).
    rig.initR9060XT16G();
    rig.hb.setBridgePfm(nullptr);
    rig.hb.setBridgeMem(&rig.hbPfm);
    CHECK(handle_probe_base_address_register(ops, cnt, &rig.cfg, &rig.dev, 6u, 0u));
    CHECK(cnt.interceptCount == 2u);

    // 7. Parent cycle or missing host bridge fails closed without looping.
    rig.initR9060XT16G();
    rig.swDn.setParent(&rig.swUp);
    rig.swUp.setParent(&rig.swDn);
    CHECK(!check_root_pfm64_capacity(&rig.cfg, &rig.dev, 16ULL << 30));

    // 8. Range list self-cycle fails closed without looping.
    rig.initR9060XT16G();
    rig.hbPfm.setWindow(0x100000000ULL, 1ULL << 30, &rig.hbPfm);
    CHECK(!check_root_pfm64_capacity(&rig.cfg, &rig.dev, 16ULL << 30));
}

} // namespace

int main() {
    test_real_bootkc_if_available();
    test_macho_and_signature_verification();
    test_policy_and_detour_encoding();
    test_non_target_and_device_allowlist();
    test_16g_and_32g_preservation_and_bar2_integrity();
    test_small_bar_passthrough_and_invalid_bar0();
    test_bridge_pfm64_sufficiency_and_bounds();

    std::printf("pci_bar_compat_test: %d checks, %d failed\n", gChecks, gFailures);
    return gFailures == 0 ? 0 : 1;
}
