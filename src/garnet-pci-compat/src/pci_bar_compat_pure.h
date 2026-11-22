//
//  pci_bar_compat_pure.h — pure C++17 logic for GarnetPCIBarCompat.kext.
//
//  No kernel headers or allocations; host-tested in tests/pci_bar_compat_test.cpp.
//  Targets macOS 26.7.2 (25G332) com.apple.iokit.IOPCIFamily v2.9 (726.100.6,
//  LC_UUID 227771C6-ADC2-30C7-8408-263F723CF81C) with fail-closed validation.
//
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

namespace garnet_pcibar {

constexpr uint64_t kPageMask = 0xFFFULL;
constexpr uint64_t kPageSize = 4096ULL;
constexpr uint32_t kMaxBackscanPages = 16u;

constexpr uint32_t kMachMagic64 = 0xFEEDFACFu;
constexpr uint32_t kCpuTypeX86_64 = 0x01000007u;
constexpr uint32_t kMhKextBundle = 11u;
constexpr uint32_t kLcSegment64 = 0x19u;
constexpr uint32_t kLcUuid = 0x1Bu;

constexpr uint64_t kOneGiB = 1ULL << 30;
constexpr uint64_t kAbove4GMin = 0x100000000ULL;

constexpr uint16_t kVendorAmd = 0x1002u;
constexpr uint16_t kDevIdNavi44 = 0x7590u;
constexpr uint16_t kDevIdR9070 = 0x7550u;
constexpr uint16_t kDevIdR9700 = 0x7551u;

constexpr uint32_t kIOPCIConfiguratorPFM64 = 0x2000u;
constexpr uint32_t kIOPCIRangeFlag64Bit = 0x80u;
constexpr uint32_t kIOPCIResourceMemory = 0u;
constexpr uint32_t kIOPCIResourcePrefetchMemory = 1u;
constexpr uint32_t kIOPCIResourceIO = 2u;

constexpr uint64_t kMinBarAddressDefault[4] = {
    0x100000ULL, 0x100000ULL, 0x400ULL, 0ULL
};

// Verified 25G332 IOPCIFamily struct offsets (BootKernelExtensions.kc audit).
constexpr size_t kOffConfiguratorFlags = 0x90u;
constexpr size_t kOffConfiguratorRoot = 0x98u;
constexpr size_t kOffEntryParent = 0x00u;
constexpr size_t kOffEntryClassHigh16 = 0x2Eu;
constexpr size_t kOffEntryBusNum = 0x32u;
constexpr size_t kOffEntryVendorDevice = 0x34u;
constexpr size_t kOffEntryRanges = 0x60u;
constexpr size_t kOffEntryBridgeMemRange = 0x98u;
constexpr size_t kOffEntryBridgePfmRange = 0xA0u;
constexpr size_t kOffEntryIsHostBridge = 0x14Du;

constexpr size_t kOffRangeStart = 0x00u;
constexpr size_t kOffRangeSize = 0x08u;
constexpr size_t kOffRangeEnd = 0x28u;
constexpr size_t kOffRangeMinAddr = 0x40u;
constexpr size_t kOffRangeMaxAddr = 0x48u;
constexpr size_t kOffRangeFlags = 0x54u;
constexpr size_t kOffRangeNext = 0x58u;
constexpr size_t kOffRangeNextSub = 0x60u;
constexpr size_t kOffRangeAllocations = 0x68u;

constexpr uint32_t kMaxBridgeDepth = 16u;
constexpr uint32_t kMaxRangeNodes = 64u;

constexpr uint8_t kExpectedIopciUuid[16] = {
    0x22, 0x77, 0x71, 0xC6, 0xAD, 0xC2, 0x30, 0xC7,
    0x84, 0x08, 0x26, 0x3F, 0x72, 0x3C, 0xF8, 0x1C
};

// Unique byte signatures in 25G332 IOPCIFamily __TEXT,__text.
constexpr uint8_t kPrologueSig[28] = {
    0x55, 0x48, 0x89, 0xe5, 0x41, 0x57, 0x41, 0x56,
    0x41, 0x55, 0x41, 0x54, 0x53, 0x48, 0x83, 0xec,
    0x58, 0x89, 0x4d, 0xb4, 0x41, 0x89, 0xd6, 0x49,
    0x89, 0xf5, 0x48, 0x89
};
constexpr uint8_t kSite1Sig[14] = {
    0x48, 0x3d, 0x01, 0x00, 0x00, 0x40, 0x44, 0x8b,
    0x75, 0xd4, 0x41, 0x83, 0xd6, 0xff
};
constexpr uint8_t kSite2Sig[17] = {
    0x48, 0x3d, 0x01, 0x00, 0x00, 0x40, 0x89, 0xc1,
    0x48, 0x0f, 0x42, 0xc8, 0x0f, 0x92, 0xc0, 0x20,
    0xc2
};

constexpr size_t kStolenPrologueBytes = 17u;
constexpr size_t kTrampolineMinNops = 32u;
constexpr size_t kSite1RelOffset = 0x2B2u;
constexpr size_t kSite2RelOffset = 0x3B7u;
constexpr size_t kMinProbeFuncLen = 0x3E8u;
constexpr uint64_t kExpectedEntryHdrOff = 0x3712Eu;

constexpr size_t kCallSiteRead16 = 0x24u;
constexpr size_t kCallSiteWrite16 = 0x46u;
constexpr size_t kCallSiteRead32 = 0x124u;
constexpr size_t kCallSiteWrite32 = 0x13Cu;
constexpr size_t kCallSiteRangeInit = 0x3E3u;

constexpr uint64_t kExpectedRead16HdrOff = 0x306D8u;
constexpr uint64_t kExpectedWrite16HdrOff = 0x32AE6u;
constexpr uint64_t kExpectedRead32HdrOff = 0x2F9E0u;
constexpr uint64_t kExpectedWrite32HdrOff = 0x32B6Au;
constexpr uint64_t kExpectedRangeInitHdrOff = 0x3EEBFu;

enum class CompatStatus : uint32_t {
    kNotEvaluated = 0,
    kInstalled = 1,
    kDisabledByBootArg = 2,
    kLateLoad = 3,
    kHeaderNotFound = 4,
    kBadMachO = 5,
    kUuidMismatch = 6,
    kPatternMissing = 7,
    kPatternDuplicate = 8,
    kPatternOffsetMismatch = 9,
    kHelperCallMismatch = 10,
    kUninstalled = 11,
};

inline const char *status_name(CompatStatus s) {
    switch (s) {
    case CompatStatus::kNotEvaluated: return "not-evaluated";
    case CompatStatus::kInstalled: return "installed";
    case CompatStatus::kDisabledByBootArg: return "disabled-by-boot-arg";
    case CompatStatus::kLateLoad: return "refused-late-load";
    case CompatStatus::kHeaderNotFound: return "header-not-found";
    case CompatStatus::kBadMachO: return "bad-macho";
    case CompatStatus::kUuidMismatch: return "uuid-mismatch";
    case CompatStatus::kPatternMissing: return "pattern-missing";
    case CompatStatus::kPatternDuplicate: return "pattern-duplicate";
    case CompatStatus::kPatternOffsetMismatch: return "pattern-offset-mismatch";
    case CompatStatus::kHelperCallMismatch: return "helper-call-mismatch";
    case CompatStatus::kUninstalled: return "uninstalled";
    }
    return "unknown";
}

struct MachHeader64 {
    uint32_t magic;
    uint32_t cputype;
    uint32_t cpusubtype;
    uint32_t filetype;
    uint32_t ncmds;
    uint32_t sizeofcmds;
    uint32_t flags;
    uint32_t reserved;
};
static_assert(sizeof(MachHeader64) == 32, "MachHeader64 size");

struct LoadCommand {
    uint32_t cmd;
    uint32_t cmdsize;
};
static_assert(sizeof(LoadCommand) == 8, "LoadCommand size");

struct SegmentCommand64 {
    uint32_t cmd;
    uint32_t cmdsize;
    char segname[16];
    uint64_t vmaddr;
    uint64_t vmsize;
    uint64_t fileoff;
    uint64_t filesize;
    uint32_t maxprot;
    uint32_t initprot;
    uint32_t nsects;
    uint32_t flags;
};
static_assert(sizeof(SegmentCommand64) == 72, "SegmentCommand64 size");

struct Section64 {
    char sectname[16];
    char segname[16];
    uint64_t addr;
    uint64_t size;
    uint32_t offset;
    uint32_t align;
    uint32_t reloff;
    uint32_t nreloc;
    uint32_t flags;
    uint32_t reserved1;
    uint32_t reserved2;
    uint32_t reserved3;
};
static_assert(sizeof(Section64) == 80, "Section64 size");

struct UuidCommand {
    uint32_t cmd;
    uint32_t cmdsize;
    uint8_t uuid[16];
};
static_assert(sizeof(UuidCommand) == 24, "UuidCommand size");

template <typename T>
inline T read_unaligned(const void *p) {
    T v {};
    __builtin_memcpy(&v, p, sizeof(T));
    return v;
}

template <typename T>
inline void write_unaligned(void *p, T v) {
    __builtin_memcpy(p, &v, sizeof(T));
}

inline bool fixed_name_eq(const char field[16], const char *want) {
    size_t i = 0;
    for (; i < 16 && want[i] != '\0'; ++i) {
        if (field[i] != want[i]) return false;
    }
    if (i < 16 && field[i] != '\0') return false;
    return want[i] == '\0';
}

inline bool mem_eq(const uint8_t *a, const uint8_t *b, size_t len) {
    for (size_t i = 0; i < len; ++i) {
        if (a[i] != b[i]) return false;
    }
    return true;
}

inline size_t count_pattern_matches(const uint8_t *buf, size_t bufLen,
                                    const uint8_t *pat, size_t patLen,
                                    size_t &firstMatchOffset) {
    firstMatchOffset = 0;
    if (!buf || !pat || patLen == 0 || bufLen < patLen) return 0;
    size_t count = 0;
    const size_t limit = bufLen - patLen;
    for (size_t i = 0; i <= limit; ++i) {
        if (buf[i] == pat[0] && mem_eq(buf + i, pat, patLen)) {
            if (count == 0) firstMatchOffset = i;
            ++count;
        }
    }
    return count;
}

struct VerifiedTarget {
    CompatStatus status { CompatStatus::kNotEvaluated };
    bool uuidVerified { false };
    uintptr_t headerAddr { 0 };
    uintptr_t textStartAddr { 0 };
    uint64_t textSize { 0 };
    uint64_t entryHdrOff { 0 };
    uintptr_t entryAddr { 0 };
    uintptr_t resumeAddr { 0 };
    uintptr_t fnConfigRead16 { 0 };
    uintptr_t fnConfigWrite16 { 0 };
    uintptr_t fnConfigRead32 { 0 };
    uintptr_t fnConfigWrite32 { 0 };
    uintptr_t fnRangeInit { 0 };
    uint8_t stolenBytes[kStolenPrologueBytes] {};
};

inline bool decode_e8_call_hdr_off(const uint8_t *entry, size_t callOff,
                                    uint64_t entryHdrOff, uint64_t &targetHdrOff) {
    if (entry[callOff] != 0xE8u) return false;
    const int32_t rel = read_unaligned<int32_t>(entry + callOff + 1u);
    const int64_t dst = static_cast<int64_t>(entryHdrOff) +
                        static_cast<int64_t>(callOff) + 5 +
                        static_cast<int64_t>(rel);
    if (dst < 0) return false;
    targetHdrOff = static_cast<uint64_t>(dst);
    return true;
}

// Validates a candidate IOPCIFamily mach_header_64 and resolves all hook targets.
// `maxBytes` may be SIZE_MAX when scanning mapped kernel __TEXT after header bounds pass.
inline CompatStatus verify_iopcifamily_target(const void *mhPtr, size_t maxBytes,
                                            uintptr_t anchorAddr, VerifiedTarget &out) {
    out = VerifiedTarget {};
    if (!mhPtr || maxBytes < sizeof(MachHeader64)) {
        out.status = CompatStatus::kBadMachO;
        return out.status;
    }

    const uint8_t *base = static_cast<const uint8_t *>(mhPtr);
    const MachHeader64 hdr = read_unaligned<MachHeader64>(base);
    if (hdr.magic != kMachMagic64 || hdr.cputype != kCpuTypeX86_64 ||
        hdr.filetype != kMhKextBundle) {
        out.status = CompatStatus::kBadMachO;
        return out.status;
    }
    if (hdr.ncmds == 0 || hdr.ncmds > 256u || hdr.sizeofcmds < sizeof(LoadCommand) ||
        hdr.sizeofcmds > 65536u || (hdr.sizeofcmds & 3u) != 0u) {
        out.status = CompatStatus::kBadMachO;
        return out.status;
    }
    if (hdr.sizeofcmds > maxBytes - sizeof(MachHeader64)) {
        out.status = CompatStatus::kBadMachO;
        return out.status;
    }

    const size_t cmdsEnd = sizeof(MachHeader64) + static_cast<size_t>(hdr.sizeofcmds);
    size_t off = sizeof(MachHeader64);
    bool haveUuid = false;
    bool uuidMatches = false;
    bool haveTextSeg = false;
    bool haveTextSect = false;
    uint64_t textHdrOff = 0;
    uint64_t textSize = 0;
    uint64_t textSegSize = 0;

    for (uint32_t i = 0; i < hdr.ncmds; ++i) {
        if (off > cmdsEnd || cmdsEnd - off < sizeof(LoadCommand)) {
            out.status = CompatStatus::kBadMachO;
            return out.status;
        }
        const LoadCommand lc = read_unaligned<LoadCommand>(base + off);
        if (lc.cmdsize < sizeof(LoadCommand) || (lc.cmdsize & 3u) != 0u ||
            lc.cmdsize > cmdsEnd - off) {
            out.status = CompatStatus::kBadMachO;
            return out.status;
        }

        if (lc.cmd == kLcUuid) {
            if (lc.cmdsize < sizeof(UuidCommand) || haveUuid) {
                out.status = CompatStatus::kBadMachO;
                return out.status;
            }
            const UuidCommand uc = read_unaligned<UuidCommand>(base + off);
            haveUuid = true;
            uuidMatches = mem_eq(uc.uuid, kExpectedIopciUuid, sizeof(kExpectedIopciUuid));
        } else if (lc.cmd == kLcSegment64) {
            if (lc.cmdsize < sizeof(SegmentCommand64)) {
                out.status = CompatStatus::kBadMachO;
                return out.status;
            }
            const SegmentCommand64 seg = read_unaligned<SegmentCommand64>(base + off);
            if (seg.nsects > (lc.cmdsize - sizeof(SegmentCommand64)) / sizeof(Section64)) {
                out.status = CompatStatus::kBadMachO;
                return out.status;
            }
            if (fixed_name_eq(seg.segname, "__TEXT")) {
                if (haveTextSeg || seg.vmsize < kPageSize || seg.vmsize > (16ULL << 20) ||
                    cmdsEnd > seg.vmsize) {
                    out.status = CompatStatus::kBadMachO;
                    return out.status;
                }
                haveTextSeg = true;
                textSegSize = seg.vmsize;
                size_t secOff = off + sizeof(SegmentCommand64);
                for (uint32_t s = 0; s < seg.nsects; ++s) {
                    const Section64 sec = read_unaligned<Section64>(base + secOff);
                    if (fixed_name_eq(sec.sectname, "__text")) {
                        if (haveTextSect || sec.addr < seg.vmaddr) {
                            out.status = CompatStatus::kBadMachO;
                            return out.status;
                        }
                        const uint64_t rel = sec.addr - seg.vmaddr;
                        if (rel < cmdsEnd || rel >= seg.vmsize || sec.size == 0 ||
                            sec.size > seg.vmsize - rel) {
                            out.status = CompatStatus::kBadMachO;
                            return out.status;
                        }
                        haveTextSect = true;
                        textHdrOff = rel;
                        textSize = sec.size;
                    }
                    secOff += sizeof(Section64);
                }
            }
        }
        off += lc.cmdsize;
    }

    if (off != cmdsEnd || !haveTextSeg || !haveTextSect) {
        out.status = CompatStatus::kBadMachO;
        return out.status;
    }
    if (maxBytes != SIZE_MAX &&
        (textHdrOff > maxBytes || textSize > maxBytes - textHdrOff ||
         textSegSize > maxBytes)) {
        out.status = CompatStatus::kBadMachO;
        return out.status;
    }
    if (!haveUuid || !uuidMatches) {
        out.status = CompatStatus::kUuidMismatch;
        return out.status;
    }

    const uintptr_t hdrAddr = reinterpret_cast<uintptr_t>(base);
    const uintptr_t textStart = hdrAddr + static_cast<uintptr_t>(textHdrOff);
    if (anchorAddr != 0u &&
        (anchorAddr < textStart || anchorAddr >= textStart + textSize)) {
        out.status = CompatStatus::kHeaderNotFound;
        return out.status;
    }

    out.uuidVerified = true;
    const uint8_t *textBuf = base + textHdrOff;
    const size_t textLen = static_cast<size_t>(textSize);

    size_t entryTextOff = 0;
    size_t site1TextOff = 0;
    size_t site2TextOff = 0;

    const size_t nPrologue = count_pattern_matches(
        textBuf, textLen, kPrologueSig, sizeof(kPrologueSig), entryTextOff);
    if (nPrologue == 0) { out.status = CompatStatus::kPatternMissing; return out.status; }
    if (nPrologue > 1) { out.status = CompatStatus::kPatternDuplicate; return out.status; }

    const size_t nSite1 = count_pattern_matches(
        textBuf, textLen, kSite1Sig, sizeof(kSite1Sig), site1TextOff);
    if (nSite1 == 0) { out.status = CompatStatus::kPatternMissing; return out.status; }
    if (nSite1 > 1) { out.status = CompatStatus::kPatternDuplicate; return out.status; }

    const size_t nSite2 = count_pattern_matches(
        textBuf, textLen, kSite2Sig, sizeof(kSite2Sig), site2TextOff);
    if (nSite2 == 0) { out.status = CompatStatus::kPatternMissing; return out.status; }
    if (nSite2 > 1) { out.status = CompatStatus::kPatternDuplicate; return out.status; }

    const uint64_t entryHdrOff = textHdrOff + entryTextOff;
    if (entryTextOff > textLen - kMinProbeFuncLen ||
        site1TextOff != entryTextOff + kSite1RelOffset ||
        site2TextOff != entryTextOff + kSite2RelOffset ||
        entryHdrOff != kExpectedEntryHdrOff) {
        out.status = CompatStatus::kPatternOffsetMismatch;
        return out.status;
    }

    const uint8_t *entryPtr = textBuf + entryTextOff;
    struct CallSpec { size_t site; uint64_t expectedHdr; uintptr_t *dst; };
    const CallSpec calls[5] = {
        { kCallSiteRead16, kExpectedRead16HdrOff, &out.fnConfigRead16 },
        { kCallSiteWrite16, kExpectedWrite16HdrOff, &out.fnConfigWrite16 },
        { kCallSiteRead32, kExpectedRead32HdrOff, &out.fnConfigRead32 },
        { kCallSiteWrite32, kExpectedWrite32HdrOff, &out.fnConfigWrite32 },
        { kCallSiteRangeInit, kExpectedRangeInitHdrOff, &out.fnRangeInit },
    };
    for (size_t i = 0; i < 5; ++i) {
        uint64_t dstHdr = 0;
        if (!decode_e8_call_hdr_off(entryPtr, calls[i].site, entryHdrOff, dstHdr) ||
            dstHdr < textHdrOff || dstHdr >= textHdrOff + textSize ||
            dstHdr != calls[i].expectedHdr) {
            out.status = CompatStatus::kHelperCallMismatch;
            return out.status;
        }
        *calls[i].dst = hdrAddr + static_cast<uintptr_t>(dstHdr);
    }

    out.headerAddr = hdrAddr;
    out.textStartAddr = textStart;
    out.textSize = textSize;
    out.entryHdrOff = entryHdrOff;
    out.entryAddr = hdrAddr + static_cast<uintptr_t>(entryHdrOff);
    out.resumeAddr = out.entryAddr + kStolenPrologueBytes;
    for (size_t i = 0; i < kStolenPrologueBytes; ++i) {
        out.stolenBytes[i] = entryPtr[i];
    }
    out.status = CompatStatus::kInstalled;
    return out.status;
}

inline bool header_covers_anchor(const void *mhPtr, size_t maxBytes, uintptr_t anchorAddr) {
    if (!mhPtr || maxBytes < sizeof(MachHeader64)) return false;
    const uint8_t *base = static_cast<const uint8_t *>(mhPtr);
    const MachHeader64 hdr = read_unaligned<MachHeader64>(base);
    if (hdr.magic != kMachMagic64 || hdr.cputype != kCpuTypeX86_64 ||
        hdr.filetype != kMhKextBundle) {
        return false;
    }
    if (hdr.ncmds == 0 || hdr.ncmds > 256u || hdr.sizeofcmds < sizeof(LoadCommand) ||
        hdr.sizeofcmds > 65536u || (hdr.sizeofcmds & 3u) != 0u ||
        hdr.sizeofcmds > maxBytes - sizeof(MachHeader64)) {
        return false;
    }
    if (anchorAddr == 0u) return true;
    const size_t cmdsEnd = sizeof(MachHeader64) + static_cast<size_t>(hdr.sizeofcmds);
    size_t off = sizeof(MachHeader64);
    for (uint32_t i = 0; i < hdr.ncmds; ++i) {
        if (off > cmdsEnd || cmdsEnd - off < sizeof(LoadCommand)) return false;
        const LoadCommand lc = read_unaligned<LoadCommand>(base + off);
        if (lc.cmdsize < sizeof(LoadCommand) || (lc.cmdsize & 3u) != 0u ||
            lc.cmdsize > cmdsEnd - off) {
            return false;
        }
        if (lc.cmd == kLcSegment64 && lc.cmdsize >= sizeof(SegmentCommand64)) {
            const SegmentCommand64 seg = read_unaligned<SegmentCommand64>(base + off);
            if (fixed_name_eq(seg.segname, "__TEXT") &&
                seg.nsects <= (lc.cmdsize - sizeof(SegmentCommand64)) / sizeof(Section64) &&
                seg.vmsize >= kPageSize && seg.vmsize <= (16ULL << 20) &&
                cmdsEnd <= seg.vmsize) {
                size_t secOff = off + sizeof(SegmentCommand64);
                for (uint32_t s = 0; s < seg.nsects; ++s) {
                    const Section64 sec = read_unaligned<Section64>(base + secOff);
                    if (fixed_name_eq(sec.sectname, "__text") && sec.addr >= seg.vmaddr) {
                        const uint64_t rel = sec.addr - seg.vmaddr;
                        if (rel >= cmdsEnd && rel < seg.vmsize && sec.size > 0 &&
                            sec.size <= seg.vmsize - rel) {
                            const uintptr_t start =
                                reinterpret_cast<uintptr_t>(base) + static_cast<uintptr_t>(rel);
                            return anchorAddr >= start && anchorAddr < start + sec.size;
                        }
                    }
                    secOff += sizeof(Section64);
                }
            }
        }
        off += lc.cmdsize;
    }
    return false;
}

// Scans backward page-by-page (max 16 pages) from `anchorAddr & ~0xFFF` to locate
// the IOPCIFamily mach_header_64 in a contiguous mapped region `[minReadable, endReadable)`.
inline const void *find_mach_header_backward(uintptr_t anchorAddr,
                                             uintptr_t minReadable,
                                             uintptr_t endReadable) {
    if (anchorAddr == 0 || anchorAddr < minReadable ||
        endReadable < sizeof(MachHeader64) ||
        anchorAddr > endReadable - 1u) {
        return nullptr;
    }
    uintptr_t page = anchorAddr & ~static_cast<uintptr_t>(kPageMask);
    for (uint32_t step = 0; step < kMaxBackscanPages; ++step) {
        if (page < minReadable) break;
        const size_t avail = static_cast<size_t>(endReadable - page);
        const void *cand = reinterpret_cast<const void *>(page);
        if (header_covers_anchor(cand, avail, anchorAddr)) {
            return cand;
        }
        if (page < kPageSize) break;
        page -= kPageSize;
    }
    return nullptr;
}

inline CompatStatus evaluate_load_policy(bool bootArgPresent, uint32_t bootArgValue,
                                         uint32_t iopciDeviceInstances,
                                         uint32_t configuratorInstances) {
    if (bootArgPresent && bootArgValue == 0u) {
        return CompatStatus::kDisabledByBootArg;
    }
    if (iopciDeviceInstances > 0u || configuratorInstances > 0u) {
        return CompatStatus::kLateLoad;
    }
    return CompatStatus::kInstalled;
}

// Encodes 14-byte `ff 25 00 00 00 00 <abs64>` indirect absolute jump.
inline void encode_abs_jmp14(uint8_t *dst, uint64_t targetAddr) {
    dst[0] = 0xFFu;
    dst[1] = 0x25u;
    dst[2] = 0x00u;
    dst[3] = 0x00u;
    dst[4] = 0x00u;
    dst[5] = 0x00u;
    write_unaligned<uint64_t>(dst + 6u, targetAddr);
}

inline void encode_entry_detour17(uint8_t dst[kStolenPrologueBytes], uint64_t hookAddr) {
    encode_abs_jmp14(dst, hookAddr);
    dst[14] = 0x90u;
    dst[15] = 0x90u;
    dst[16] = 0x90u;
}

inline void encode_trampoline31(uint8_t dst[kStolenPrologueBytes + 14u],
                                const uint8_t stolen[kStolenPrologueBytes],
                                uint64_t resumeAddr) {
    for (size_t i = 0; i < kStolenPrologueBytes; ++i) dst[i] = stolen[i];
    encode_abs_jmp14(dst + kStolenPrologueBytes, resumeAddr);
}

constexpr uint64_t max_allowed_bar0_for_device(uint16_t vendor, uint16_t device) {
    if (vendor != kVendorAmd) return 0ULL;
    switch (device) {
    case kDevIdNavi44: return 16ULL << 30;
    case kDevIdR9070:  return 16ULL << 30;
    case kDevIdR9700:  return 32ULL << 30;
    default:           return 0ULL;
    }
}

constexpr bool is_pow2_u64(uint64_t v) {
    return v != 0ULL && (v & (v - 1ULL)) == 0ULL;
}

// Validates a non-destructive 64-bit BAR0 probe (`maskLo` from 0x10, `maskHi32` from 0x14).
inline bool validate_bar0_probe(uint32_t maskLo, uint32_t maskHi32,
                                uint64_t maxAllowedBar0, uint64_t &size64Out) {
    size64Out = 0ULL;
    if ((maskLo & 0xFu) != 0xCu) return false; // must be 64-bit prefetchable memory
    const uint64_t mask64 =
        static_cast<uint64_t>(maskLo & ~0xFu) |
        (static_cast<uint64_t>(maskHi32) << 32);
    if (mask64 == 0ULL) return false;
    const uint64_t size64 = 0ULL - mask64;
    if (!is_pow2_u64(size64)) return false;
    if (size64 <= kOneGiB || size64 > maxAllowedBar0) return false;
    size64Out = size64;
    return true;
}

inline bool gap_fits_aligned(uint64_t gapLo, uint64_t gapHi, uint64_t requiredSize) {
    if (gapHi <= gapLo || (gapHi - gapLo) < requiredSize) return false;
    const uint64_t alignMask = requiredSize - 1ULL;
    if (gapLo > UINT64_MAX - alignMask) return false;
    const uint64_t aligned = (gapLo + alignMask) & ~alignMask;
    return aligned <= gapHi && (gapHi - aligned) >= requiredSize;
}

// Lock-free bounded walk verifying root/host-bridge PFM64 window capacity >= `requiredSize`
// at natural `requiredSize` alignment above 4 GiB.
inline bool check_root_pfm64_capacity(const void *configurator, const void *device,
                                      uint64_t requiredSize) {
    if (!configurator || !device || !is_pow2_u64(requiredSize)) return false;
    const uint8_t *cfg = static_cast<const uint8_t *>(configurator);
    const uint32_t fFlags = read_unaligned<uint32_t>(cfg + kOffConfiguratorFlags);
    if ((fFlags & kIOPCIConfiguratorPFM64) == 0u) return false;
    const void *fRoot = read_unaligned<const void *>(cfg + kOffConfiguratorRoot);
    if (!fRoot) return false;

    const void *hostBridge = nullptr;
    const void *cur = device;
    for (uint32_t depth = 0; depth < kMaxBridgeDepth && cur != nullptr; ++depth) {
        const uint8_t *curBytes = static_cast<const uint8_t *>(cur);
        const void *parent = read_unaligned<const void *>(curBytes + kOffEntryParent);
        const uint8_t isHb = read_unaligned<uint8_t>(curBytes + kOffEntryIsHostBridge);
        if (cur != fRoot && isHb != 0u && parent == fRoot) {
            hostBridge = cur;
            break;
        }
        if (cur == fRoot || parent == nullptr || parent == cur) break;
        cur = parent;
    }
    if (!hostBridge) return false;

    const uint8_t *hb = static_cast<const uint8_t *>(hostBridge);
    const void *rangeHead = read_unaligned<const void *>(hb + kOffEntryBridgePfmRange);
    if (!rangeHead) {
        // Matches Apple IOPCIConfigurator::bridgeTotalResources/bridgeAllocateResources fallback.
        rangeHead = read_unaligned<const void *>(hb + kOffEntryBridgeMemRange);
    }
    if (!rangeHead) return false;

    const void *node = rangeHead;
    for (uint32_t rCount = 0; rCount < kMaxRangeNodes && node != nullptr; ++rCount) {
        const uint8_t *r = static_cast<const uint8_t *>(node);
        const uint64_t start = read_unaligned<uint64_t>(r + kOffRangeStart);
        const uint64_t size = read_unaligned<uint64_t>(r + kOffRangeSize);
        const void *allocHead = read_unaligned<const void *>(r + kOffRangeAllocations);
        const void *nextRange = read_unaligned<const void *>(r + kOffRangeNext);
        const void *sentinel = r + kOffRangeEnd;

        if (size != 0ULL && start <= UINT64_MAX - size) {
            const uint64_t end = start + size;
            const uint64_t winStart = start < kAbove4GMin ? kAbove4GMin : start;
            if (end > winStart && (end - winStart) >= requiredSize) {
                if (!allocHead || allocHead == sentinel) {
                    if (gap_fits_aligned(winStart, end, requiredSize)) return true;
                } else {
                    uint64_t cursor = winStart;
                    const void *sub = allocHead;
                    bool hitZeroSize = false;
                    uint32_t aCount = 0;
                    for (; aCount < kMaxRangeNodes && sub != nullptr && sub != sentinel; ++aCount) {
                        const uint8_t *s = static_cast<const uint8_t *>(sub);
                        const uint64_t sStart = read_unaligned<uint64_t>(s + kOffRangeStart);
                        const uint64_t sSize = read_unaligned<uint64_t>(s + kOffRangeSize);
                        // Stop before reading s + kOffRangeNextSub on interior sentinels (size == 0).
                        if (sSize == 0ULL) {
                            hitZeroSize = true;
                            break;
                        }
                        const void *nextSub = read_unaligned<const void *>(s + kOffRangeNextSub);
                        if (sSize > UINT64_MAX - sStart) break;
                        const uint64_t sEnd = sStart + sSize;
                        if (sStart > cursor) {
                            const uint64_t gapHi = sStart < end ? sStart : end;
                            if (gap_fits_aligned(cursor, gapHi, requiredSize)) return true;
                        }
                        if (sEnd > cursor) cursor = sEnd;
                        if (cursor >= end) break;
                        if (nextSub == sub) break;
                        sub = nextSub;
                    }
                    if ((sub == nullptr || sub == sentinel || hitZeroSize) && cursor < end &&
                        gap_fits_aligned(cursor, end, requiredSize)) {
                        return true;
                    }
                }
            }
        }
        if (nextRange == node) break;
        node = nextRange;
    }
    return false;
}

struct ProbeOps {
    uint16_t (*configRead16)(void *configurator, void *device, uint32_t offset, void *space);
    void (*configWrite16)(void *configurator, void *device, uint32_t offset, uint16_t val, void *space);
    uint32_t (*configRead32)(void *configurator, void *device, uint32_t offset, void *space);
    void (*configWrite32)(void *configurator, void *device, uint32_t offset, uint32_t val, void *space);
    void (*rangeInit)(void *range, uint32_t type, uint64_t start, uint64_t size, uint64_t align);
};

struct ProbeCounters {
    uint64_t interceptCount { 0 };
    uint64_t preservedBar0Bytes { 0 };
    uint64_t passthroughCount { 0 };
    uint64_t refusedBridgeCount { 0 };
    uint32_t deviceId { 0 };
};

inline void *entry_range_slot(void *device, uint32_t idx) {
    uint8_t *dev = static_cast<uint8_t *>(device);
    return read_unaligned<void *>(dev + kOffEntryRanges + static_cast<size_t>(idx) * sizeof(void *));
}

inline void finalize_range(void *range, uint32_t type, uint8_t clean64, uint32_t fFlags) {
    uint8_t *r = static_cast<uint8_t *>(range);
    write_unaligned<uint64_t>(r + kOffRangeMinAddr, kMinBarAddressDefault[type & 3u]);
    if (clean64 == 1u) {
        uint32_t flags = read_unaligned<uint32_t>(r + kOffRangeFlags);
        flags |= kIOPCIRangeFlag64Bit;
        write_unaligned<uint32_t>(r + kOffRangeFlags, flags);
        if ((fFlags & kIOPCIConfiguratorPFM64) != 0u) {
            write_unaligned<uint64_t>(r + kOffRangeMaxAddr, ~0ULL);
        }
    }
}

// Lock-free, allocation-free, interrupts-disabled safe probe handler.
// Returns `true` when it handled an eligible RDNA4 GPU (> 1 GiB BAR0 preserved),
// or `false` when the caller MUST tail-call unmodified `orig_probeBaseAddressRegister`.
inline bool handle_probe_base_address_register(const ProbeOps &ops,
                                               ProbeCounters &counters,
                                               void *configurator,
                                               void *device,
                                               uint32_t lastBarNum,
                                               uint32_t resetMask) {
    if (!configurator || !device || lastBarNum < 5u) {
        __atomic_fetch_add(&counters.passthroughCount, 1ULL, __ATOMIC_RELAXED);
        return false;
    }

    const uint8_t *devBytes = static_cast<const uint8_t *>(device);
    const uint32_t vd = read_unaligned<uint32_t>(devBytes + kOffEntryVendorDevice);
    const uint16_t vendor = static_cast<uint16_t>(vd & 0xFFFFu);
    const uint16_t devId = static_cast<uint16_t>(vd >> 16);
    const uint64_t maxAllowed = max_allowed_bar0_for_device(vendor, devId);
    if (maxAllowed == 0ULL) {
        __atomic_fetch_add(&counters.passthroughCount, 1ULL, __ATOMIC_RELAXED);
        return false;
    }

    const uint16_t savedCmd = ops.configRead16(configurator, device, 0x04u, nullptr);
    ops.configWrite16(configurator, device, 0x04u, static_cast<uint16_t>(savedCmd & 0xFFFCu), nullptr);

    // Non-destructively probe BAR0 (0x10 / 0x14).
    const uint32_t origBar0Lo = ops.configRead32(configurator, device, 0x10u, nullptr);
    ops.configWrite32(configurator, device, 0x10u, 0xFFFFFFFFu, nullptr);
    const uint32_t mask0Lo = ops.configRead32(configurator, device, 0x10u, nullptr);
    ops.configWrite32(configurator, device, 0x10u, origBar0Lo, nullptr);

    uint32_t origBar0Hi = 0;
    uint32_t mask0Hi = 0;
    if ((mask0Lo & 0xFu) == 0xCu) {
        origBar0Hi = ops.configRead32(configurator, device, 0x14u, nullptr);
        ops.configWrite32(configurator, device, 0x14u, 0xFFFFFFFFu, nullptr);
        mask0Hi = ops.configRead32(configurator, device, 0x14u, nullptr);
        ops.configWrite32(configurator, device, 0x14u, origBar0Hi, nullptr);
    }

    uint64_t bar0Size64 = 0;
    if (!validate_bar0_probe(mask0Lo, mask0Hi, maxAllowed, bar0Size64)) {
        ops.configWrite16(configurator, device, 0x04u, savedCmd, nullptr);
        __atomic_fetch_add(&counters.passthroughCount, 1ULL, __ATOMIC_RELAXED);
        return false;
    }

    const uint32_t fFlags = read_unaligned<uint32_t>(
        static_cast<const uint8_t *>(configurator) + kOffConfiguratorFlags);
    if ((fFlags & kIOPCIConfiguratorPFM64) == 0u ||
        !check_root_pfm64_capacity(configurator, device, bar0Size64)) {
        ops.configWrite16(configurator, device, 0x04u, savedCmd, nullptr);
        __atomic_fetch_add(&counters.refusedBridgeCount, 1ULL, __ATOMIC_RELAXED);
        __atomic_fetch_add(&counters.passthroughCount, 1ULL, __ATOMIC_RELAXED);
        return false;
    }

    void *bar0Range = entry_range_slot(device, 0u);
    if (!bar0Range) {
        ops.configWrite16(configurator, device, 0x04u, savedCmd, nullptr);
        __atomic_fetch_add(&counters.passthroughCount, 1ULL, __ATOMIC_RELAXED);
        return false;
    }

    uint32_t effectiveLastBar = lastBarNum > 6u ? 6u : lastBarNum;

    // 1. Expansion ROM (0x30) when effectiveLastBar == 6 (matching Apple display class 0x030000 check).
    if (effectiveLastBar == 6u) {
        const uint32_t origRom = ops.configRead32(configurator, device, 0x30u, nullptr);
        ops.configWrite32(configurator, device, 0x30u, 0xFFFFF800u, nullptr);
        const uint32_t maskRom = ops.configRead32(configurator, device, 0x30u, nullptr);
        ops.configWrite32(configurator, device, 0x30u, origRom, nullptr);
        effectiveLastBar = 5u;

        const uint16_t classHigh = read_unaligned<uint16_t>(devBytes + kOffEntryClassHigh16);
        if (maskRom != 0u && (static_cast<uint32_t>(classHigh) << 16) == 0x030000u) {
            void *romRange = entry_range_slot(device, 6u);
            if (romRange) {
                const uint32_t romStart = (resetMask & 1u) ? 0u : (origRom & 0xFFFFF800u);
                const uint32_t romSize = 0u - (maskRom & 0xFFFFF800u);
                ops.rangeInit(romRange, kIOPCIResourceMemory, romStart, romSize, romSize);
            }
        }
    }

    // 2. BAR0 ONLY: preserve full 64-bit size64 and clean64 = 1, then step barNum -> 2.
    bool bar0Initialized = false;
    {
        uint64_t start64 = static_cast<uint64_t>(origBar0Lo & ~0xFu) |
                           (static_cast<uint64_t>(origBar0Hi) << 32);
        if ((resetMask & (1u << kIOPCIResourcePrefetchMemory)) != 0u) {
            start64 = 0ULL;
        }
        const uint64_t mask64 = 0ULL - bar0Size64;
        if (start64 != mask64) {
            ops.rangeInit(bar0Range, kIOPCIResourcePrefetchMemory,
                          start64, bar0Size64, bar0Size64);
            finalize_range(bar0Range, kIOPCIResourcePrefetchMemory, 1u, fFlags);
            bar0Initialized = true;
        }
    }

    // 3. BAR2..5: exact Apple MAX_BAR_SIZE = 1 GiB semantics (no BAR1/BAR2 corruption).
    uint32_t barNum = 2u;
    while (barNum <= effectiveLastBar) {
        const uint32_t reg = 0x10u + barNum * 4u;
        uint32_t nextBarNum = barNum + 1u;

        const uint32_t origLo = ops.configRead32(configurator, device, reg, nullptr);
        ops.configWrite32(configurator, device, reg, 0xFFFFFFFFu, nullptr);
        uint32_t maskLo = ops.configRead32(configurator, device, reg, nullptr);
        ops.configWrite32(configurator, device, reg, origLo, nullptr);

        if (maskLo == 0u) {
            barNum = nextBarNum;
            continue;
        }

        uint64_t start64 = origLo;
        uint64_t maskHi = 0xFFFFFFFF00000000ULL;
        uint8_t clean64 = 0u;
        uint32_t type = kIOPCIResourceMemory;
        uint64_t typeMask = ~0xFULL;

        if ((maskLo & 1u) != 0u) {
            if (maskLo < 0x10000u) maskLo |= 0xFFFF0000u;
            type = kIOPCIResourceIO;
            typeMask = ~3ULL;
        } else if ((maskLo & 0x6u) == 0x4u) {
            const uint32_t prefetch = maskLo & 0x8u;
            const uint8_t busNum = read_unaligned<uint8_t>(devBytes + kOffEntryBusNum);
            if (prefetch == 0u && busNum != 0u) {
                ops.configWrite32(configurator, device, reg + 4u, 0u, nullptr);
                type = kIOPCIResourceMemory;
                clean64 = 0u;
                nextBarNum = barNum + 2u;
            } else {
                const uint32_t origHi = ops.configRead32(configurator, device, reg + 4u, nullptr);
                start64 |= static_cast<uint64_t>(origHi) << 32;
                ops.configWrite32(configurator, device, reg + 4u, 0xFFFFFFFFu, nullptr);
                maskHi = static_cast<uint64_t>(
                    ops.configRead32(configurator, device, reg + 4u, nullptr)) << 32;
                ops.configWrite32(configurator, device, reg + 4u, origHi, nullptr);
                clean64 = 1u;
                type = prefetch >> 3;
                nextBarNum = barNum + 2u;
            }
        }

        start64 &= typeMask;
        if ((resetMask & (1u << type)) != 0u) start64 = 0ULL;
        const uint64_t mask64 = static_cast<uint64_t>(maskLo & static_cast<uint32_t>(typeMask)) | maskHi;
        const uint64_t size64 = 0ULL - mask64;
        if (size64 > kOneGiB) {
            nextBarNum -= 1u;
        }
        if (start64 != mask64) {
            const uint64_t finalSize = (size64 <= kOneGiB) ? size64 : static_cast<uint32_t>(size64);
            if (size64 > kOneGiB) clean64 = 0u;
            void *range = entry_range_slot(device, barNum);
            if (range) {
                ops.rangeInit(range, type, start64, finalSize, finalSize);
                finalize_range(range, type, clean64, fFlags);
            }
        }
        barNum = nextBarNum;
    }

    ops.configWrite16(configurator, device, 0x04u, savedCmd, nullptr);

    if (bar0Initialized) {
        __atomic_fetch_add(&counters.interceptCount, 1ULL, __ATOMIC_RELAXED);
        __atomic_store_n(&counters.preservedBar0Bytes, bar0Size64, __ATOMIC_RELAXED);
        __atomic_store_n(&counters.deviceId, static_cast<uint32_t>(devId), __ATOMIC_RELAXED);
    } else {
        __atomic_fetch_add(&counters.passthroughCount, 1ULL, __ATOMIC_RELAXED);
    }
    return true;
}

} // namespace garnet_pcibar
