//
//  native_agdc_pure.h - the pure half of the NATIVE AGDC service (kext 0.0.614, #11 step 11h.3): every DECISION of `pipeagdc` (accel action 88), judged from arguments alone.
//  No kernel header: tests/native_agdc_test.cpp compiles this very file (and amd/native_agdc_flow.h, the sequencing built on it) and drives it, with planted breaks
//  (tests/native_agdc_plant.sh).
//
//  WHAT THIS IS. IOPresentment needs an AppleGraphicsDeviceControl object (AGDC) to answer four vendor commands (1 / 0x980 / 0x921 / 0x711).
//  On a native boot no AGDC instance exists ("agdc 0x0" in the wrangler log), and the class has a pure virtual, so `RDNA4AGDC` is ours to build: a genuine AGDC object
//  on a vtable copy whose slots 0 / 1 / 266 are ours. This header models the native AGDC decision checks:
//      slide        two anchors that must AGREE: the live AGDC metaclass minus its System-KC static address, and the live IOAccelDisplayPipe metaclass minus ITS static address
//                   (both are in the one System KC, one slide; the statics are read from the extracted kexts' symbols, see kAgdcMeta / kPipeMeta);
//      framebuffer  the primary RDNA4 framebuffer instance itself, not an external accelerator pipe;
//      guard        none (no external pipe guard exists). Instead: the display boot-arg latch, and a refusal while row-120's mode HOLD is up (the 0x921 reply's timing must be the framebuffer's 60 Hz).
//
//  Everything stays behind boot-arg garnet-metal-disp=1 (the latch of native_disp.cpp): with it OFF the verb is not admitted (native_disp_pure.h kLastAction / action_admitted) and this file's
//  flow answers "off" before it reads anything.
//
#pragma once
#include <stddef.h>
#include <stdint.h>

namespace rdna4agdc {

// ---- the verb ----------------------------------------------------------------------------------------------------------------------------------------------
constexpr uint32_t kActAgdc = 88u;    // accel action 88: `pipeagdc [0|1]` - 0 reads the state, 1 publishes (rdna4disp::verb_args_ok admits exactly these two arguments)
constexpr bool arg_ok(uint64_t arg) { return arg <= 1ull; }

// ---- status codes ------------------------------------------------------------------------------------------------------------------------------------------
// Numbers 0 and 3..13 keep the TRANSLATION route's meaning (DisplayPipeGuard.cpp's agdc_publish_locked), so the two decode tables only differ at 1, 2, 14 and 15.
enum Status : uint32_t {
    kPublished = 0, kOff = 1, kUnused2 = 2, kNoAgdc = 3, kSlide = 4, kClassSize = 5, kCodeBytes = 6, kVtable = 7, kNoFbPci = 8, kNoWrangler = 9, kAlloc = 10, kStartFailed = 11,
    kAlready = 12, kBadArg = 13, kHeld = 14, kNoFamily = 15, kStatusCount = 16
};
constexpr const char *status_name(uint32_t s) {
    return s == kPublished ? "PUBLISHED" : s == kUnused2 ? "(unused here: the translation route's \"safety core not armed\")" : s == kOff ? "display is OFF (boot-arg garnet-metal-disp is not 1)" : s == kNoAgdc ? "AppleGraphicsDeviceControl class is not loaded" :
           s == kSlide ? "the two slide anchors disagree (or the slide is not page aligned)" : s == kClassSize ? "AGDC class size is not 0x110" :
           s == kCodeBytes ? "a function we call does not carry its expected first bytes" : s == kVtable ? "AGDC vtable slots 7/184/238/267 are not the expected functions" :
           s == kNoFbPci ? "no GarnetPrimaryFramebuffer / PCI device / provider" : s == kNoWrangler ? "no AppleGPUWrangler (AGDC::start would wait for it)" : s == kAlloc ? "allocation failed" :
           s == kStartFailed ? "AGDC::start returned false" : s == kAlready ? "already published" : s == kBadArg ? "bad argument" :
           s == kHeld ? "row-120 mode hold is up (the link timing would not be the framebuffer's)" : s == kNoFamily ? "IOAccelDisplayPipe class (the second slide anchor) is not loaded" : "unknown";
}

// ---- the slide ---------------------------------------------------------------------------------------------------------------------------------------------
// Static addresses from the extracted x86_64 System KC for macOS 25G332 (retained for optional diagnostics and VM probe tests;
// runtime AGDC resolution in resolve_agdc_runtime() no longer depends on these fixed offsets).
constexpr uint64_t kAgdcMeta = 0x13d458e8ull;
constexpr uint64_t kPipeMeta = 0x14be66d8ull;
constexpr uint64_t kKernelHalf = 0xffffff7000000000ull;     // the bound DisplayPipeGuard.cpp's dpg_kptr and native_disp_pure.h use
constexpr bool kptr_ok(uint64_t p) { return p >= kKernelHalf; }
struct SlideOut { uint32_t status; uint64_t slide; };
// Both metaclass pointers must be kernel pointers, the two slides must be equal, and the slide must be page aligned. Anything else is kSlide (a zero metaclass is the caller's kNoAgdc /
// kNoFamily, decided before this). A kernel pointer is always ABOVE both statics (the static_assert below), so the subtractions cannot wrap once kptr_ok holds for both.
static_assert(kKernelHalf > kAgdcMeta && kKernelHalf > kPipeMeta, "a kernel pointer is above both static addresses: the slide subtractions cannot wrap");
constexpr SlideOut slide_decide(uint64_t agdcMeta, uint64_t pipeMeta) {
    return (!kptr_ok(agdcMeta) || !kptr_ok(pipeMeta)) ? SlideOut{ kSlide, 0ull } :
           ((agdcMeta - kAgdcMeta) != (pipeMeta - kPipeMeta) || ((agdcMeta - kAgdcMeta) & 0xfffull) != 0ull) ? SlideOut{ kSlide, 0ull } :
           SlideOut{ kPublished, agdcMeta - kAgdcMeta };
}

// ---- build-independent runtime AGDC resolution -------------------------------------------------------------------------------------------------------------
constexpr uint32_t kAgdcClassSize = 0x110u;
constexpr unsigned kAgdcVtSlots = 276u;
constexpr unsigned kMetaAllocSlot = 17u;
constexpr uint64_t kAgdcTextSpanMax = 0x100000ull;
constexpr size_t kAgdcScanWindowMax = 2048u;

struct AgdcResolved {
    uint32_t status;
    uint64_t vtable;       // live __ZTV26AppleGraphicsDeviceControl + 16 (slot 0 pointer)
    uint64_t opNew;        // live AppleGraphicsDeviceControl::operator new(size_t)
    uint64_t ctorMeta;     // live AppleGraphicsDeviceControl::C2(const OSMetaClass *)
    uint64_t d2;           // live AppleGraphicsDeviceControl::~D2()
    uint64_t opDelete;     // live AppleGraphicsDeviceControl::operator delete(void *, size_t)
    uint64_t getMetaClass; // live AppleGraphicsDeviceControl::getMetaClass()
    uint64_t kallocView;   // shared kalloc_type_view loaded by opNew & opDelete
};

namespace detail {
inline int32_t rd_i32(const uint8_t *p) {
    const uint32_t u = static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) |
                       (static_cast<uint32_t>(p[2]) << 16) | (static_cast<uint32_t>(p[3]) << 24);
    return static_cast<int32_t>(u);
}
inline bool eq_bytes(const uint8_t *a, const uint8_t *b, size_t n) {
    for (size_t i = 0; i < n; ++i) if (a[i] != b[i]) return false;
    return true;
}
inline uint64_t abs_diff(uint64_t a, uint64_t b) { return a >= b ? (a - b) : (b - a); }
} // namespace detail

template <class Mem>
inline uint64_t resolve_stub(const Mem &mem, uint64_t addr) {
    uint8_t b[6] {};
    if (!mem.ptr_ok(addr) || !mem.read_bytes(addr, b, sizeof(b))) return addr;
    if (b[0] == 0xffu && b[1] == 0x25u) {
        const int64_t disp = detail::rd_i32(b + 2);
        const uint64_t got = static_cast<uint64_t>(static_cast<int64_t>(addr) + 6 + disp);
        uint64_t target = 0;
        if (mem.ptr_ok(got) && mem.read_u64(got, &target) && mem.ptr_ok(target)) return target;
    }
    return addr;
}

template <class Mem>
inline AgdcResolved resolve_agdc_runtime(const Mem &mem, uint64_t mcAddr, uint32_t classSize,
                                         uint64_t ioServiceC2, uint64_t ioServiceD2) {
    AgdcResolved out {};
    out.status = kCodeBytes;
    if (classSize != kAgdcClassSize) {
        out.status = kClassSize;
        return out;
    }
    uint64_t mvt = 0, mvt0 = 0, mvt1 = 0, allocFn = 0;
    if (!mem.ptr_ok(mcAddr) || !mem.read_u64(mcAddr, &mvt) || !mem.ptr_ok(mvt) ||
        !mem.read_u64(mvt + 0u * 8u, &mvt0) || !mem.ptr_ok(mvt0) ||
        !mem.read_u64(mvt + 1u * 8u, &mvt1) || !mem.ptr_ok(mvt1) ||
        !mem.read_u64(mvt + kMetaAllocSlot * 8u, &allocFn) || !mem.ptr_ok(allocFn)) {
        return out;
    }

    static constexpr uint8_t kAllocBytes[8] = {0x55, 0x48, 0x89, 0xe5, 0x31, 0xc0, 0x5d, 0xc3};
    uint8_t abuf[8] {};
    if (!mem.read_bytes(allocFn, abuf, sizeof(abuf)) || !detail::eq_bytes(abuf, kAllocBytes, sizeof(abuf))) {
        return out;
    }

    const uint64_t wantC2 = resolve_stub(mem, ioServiceC2);
    const uint64_t wantD2 = resolve_stub(mem, ioServiceD2);
    if (!mem.ptr_ok(wantC2) || !mem.ptr_ok(wantD2) || wantC2 == wantD2) {
        return out;
    }

    const uint64_t loAnchor = (mvt0 < allocFn) ? mvt0 : allocFn;
    const uint64_t hiAnchor = (mvt0 > allocFn) ? mvt0 : allocFn;
    if (hiAnchor - loAnchor > 0x1000ull) {
        return out;
    }
    const uint64_t backMax = (loAnchor & 0xfffull) < 0x40ull ? (loAnchor & 0xfffull) : 0x40ull;
    const uint64_t winLo = loAnchor - backMax;
    uint64_t winHi = hiAnchor + 0x100ull;
    if (!(mvt1 > hiAnchor && (mvt1 - hiAnchor) >= 0x100ull)) {
        const uint64_t pageEnd = (hiAnchor & ~0xfffull) + 0x1000ull;
        if (winHi > pageEnd) winHi = pageEnd;
    }
    if (winHi <= winLo || (winHi - winLo) > kAgdcScanWindowMax) {
        return out;
    }

    const size_t winLen = static_cast<size_t>(winHi - winLo);
    uint8_t win[kAgdcScanWindowMax] {};
    if (!mem.read_bytes(winLo, win, winLen)) {
        return out;
    }

    static constexpr uint8_t kPreGetMeta[7] = {0x55, 0x48, 0x89, 0xe5, 0x48, 0x8d, 0x05};
    static constexpr uint8_t kPreC2[10]     = {0x55, 0x48, 0x89, 0xe5, 0x53, 0x50, 0x48, 0x89, 0xfb, 0xe8};
    static constexpr uint8_t kMidC2[3]      = {0x48, 0x8d, 0x05};
    static constexpr uint8_t kPostC2[10]    = {0x48, 0x89, 0x03, 0x48, 0x83, 0xc4, 0x08, 0x5b, 0x5d, 0xc3};
    static constexpr uint8_t kPreD2[6]      = {0x55, 0x48, 0x89, 0xe5, 0x5d, 0xe9};
    static constexpr uint8_t kPreNew[10]    = {0x55, 0x48, 0x89, 0xe5, 0x48, 0x89, 0xfe, 0x48, 0x8d, 0x3d};
    static constexpr uint8_t kPreDel[13]    = {0x55, 0x48, 0x89, 0xe5, 0x48, 0x89, 0xf2, 0x48, 0x89, 0xfe, 0x48, 0x8d, 0x3d};

    unsigned nGetMeta = 0, nC2 = 0, nD2 = 0, nNew = 0, nDel = 0;
    uint64_t newTarget = 0, delTarget = 0, viewNew = 0, viewDel = 0;

    for (size_t i = 0; i < winLen; ++i) {
        const uint64_t addr = winLo + i;
        if (i + 13u <= winLen && detail::eq_bytes(win + i, kPreGetMeta, 7) &&
            win[i + 11] == 0x5du && win[i + 12] == 0xc3u) {
            const uint64_t target = static_cast<uint64_t>(static_cast<int64_t>(addr) + 11 + detail::rd_i32(win + i + 7));
            if (target == mcAddr) {
                out.getMetaClass = addr;
                ++nGetMeta;
            }
        }
        if (i + 31u <= winLen && detail::eq_bytes(win + i, kPreC2, 10) &&
            detail::eq_bytes(win + i + 14, kMidC2, 3) &&
            detail::eq_bytes(win + i + 21, kPostC2, 10)) {
            const uint64_t callee = static_cast<uint64_t>(static_cast<int64_t>(addr) + 14 + detail::rd_i32(win + i + 10));
            const uint64_t vt = static_cast<uint64_t>(static_cast<int64_t>(addr) + 21 + detail::rd_i32(win + i + 17));
            if (resolve_stub(mem, callee) == wantC2 && mem.ptr_ok(vt) && (vt & 7u) == 0u) {
                out.ctorMeta = addr;
                out.vtable = vt;
                ++nC2;
            }
        }
        if (i + 10u <= winLen && detail::eq_bytes(win + i, kPreD2, 6)) {
            const uint64_t callee = static_cast<uint64_t>(static_cast<int64_t>(addr) + 10 + detail::rd_i32(win + i + 6));
            if (resolve_stub(mem, callee) == wantD2) {
                out.d2 = addr;
                ++nD2;
            }
        }
        if (i + 20u <= winLen && detail::eq_bytes(win + i, kPreNew, 10) &&
            win[i + 14] == 0x5du && win[i + 15] == 0xe9u) {
            const uint64_t vw = static_cast<uint64_t>(static_cast<int64_t>(addr) + 14 + detail::rd_i32(win + i + 10));
            const uint64_t tgt = resolve_stub(mem, static_cast<uint64_t>(static_cast<int64_t>(addr) + 20 + detail::rd_i32(win + i + 16)));
            if (mem.ptr_ok(vw) && mem.ptr_ok(tgt)) {
                out.opNew = addr;
                viewNew = vw;
                newTarget = tgt;
                ++nNew;
            }
        }
        if (i + 23u <= winLen && detail::eq_bytes(win + i, kPreDel, 13) &&
            win[i + 17] == 0x5du && win[i + 18] == 0xe9u) {
            const uint64_t vw = static_cast<uint64_t>(static_cast<int64_t>(addr) + 17 + detail::rd_i32(win + i + 13));
            const uint64_t tgt = resolve_stub(mem, static_cast<uint64_t>(static_cast<int64_t>(addr) + 23 + detail::rd_i32(win + i + 19)));
            if (mem.ptr_ok(vw) && mem.ptr_ok(tgt)) {
                out.opDelete = addr;
                viewDel = vw;
                delTarget = tgt;
                ++nDel;
            }
        }
    }

    if (nGetMeta != 1u || nC2 != 1u || nD2 != 1u || nNew != 1u || nDel != 1u ||
        viewNew != viewDel || newTarget == delTarget) {
        return out;
    }
    out.kallocView = viewNew;

    // Verify AGDC vtable structure and boundary against live memory.
    out.status = kVtable;
    uint64_t hdr0 = 1, hdr1 = 1, s0 = 0, s1 = 0, s7 = 0, s184 = 0, s238 = 0, s266 = 0, s267 = 0, s275 = 0, sEnd = 1;
    if (!mem.read_u64(out.vtable - 16u, &hdr0) || !mem.read_u64(out.vtable - 8u, &hdr1) ||
        hdr0 != 0ull || hdr1 != 0ull ||
        !mem.read_u64(out.vtable + 0u * 8u, &s0) || !mem.read_u64(out.vtable + 1u * 8u, &s1) ||
        !mem.read_u64(out.vtable + 7u * 8u, &s7) || !mem.read_u64(out.vtable + 184u * 8u, &s184) ||
        !mem.read_u64(out.vtable + 238u * 8u, &s238) || !mem.read_u64(out.vtable + 266u * 8u, &s266) ||
        !mem.read_u64(out.vtable + 267u * 8u, &s267) || !mem.read_u64(out.vtable + 275u * 8u, &s275) ||
        !mem.read_u64(out.vtable + kAgdcVtSlots * 8u, &sEnd) || sEnd != 0ull) {
        return out;
    }
    if (s7 != out.getMetaClass ||
        !mem.ptr_ok(s0) || !mem.ptr_ok(s1) || !mem.ptr_ok(s184) ||
        !mem.ptr_ok(s238) || !mem.ptr_ok(s266) || !mem.ptr_ok(s267) || !mem.ptr_ok(s275)) {
        return out;
    }
    static constexpr uint8_t kUd2Bytes[6] = {0x55, 0x48, 0x89, 0xe5, 0x0f, 0x0b};
    uint8_t d0b[6] {}, d1b[6] {};
    if (!mem.read_bytes(s1, d1b, sizeof(d1b)) || !mem.read_bytes(s0, d0b, sizeof(d0b)) ||
        !detail::eq_bytes(d1b, kUd2Bytes, sizeof(d1b)) || !detail::eq_bytes(d0b, kUd2Bytes, sizeof(d0b))) {
        return out;
    }
    if (detail::abs_diff(s184, allocFn) >= kAgdcTextSpanMax ||
        detail::abs_diff(s238, allocFn) >= kAgdcTextSpanMax ||
        detail::abs_diff(s267, allocFn) >= kAgdcTextSpanMax ||
        detail::abs_diff(s275, allocFn) >= kAgdcTextSpanMax ||
        detail::abs_diff(s266, allocFn) < kAgdcTextSpanMax) {
        return out;
    }

    out.status = kPublished;
    return out;
}

// ---- the framebuffer --------------------------------------------------------------------------------------------------------------------------------------
// Which LOCAL leaf names count as ours (the class rename of fbname swaps the OSMetaClass name pointer, so both are legal).
// This is a leaf-name check only, never an identity proof: the kernel glue proves the target-GPU primary separately and the
// generic Apple IONDRVFramebuffer must not be added here.
constexpr bool str_eq(const char *a, const char *b) {
    while (*a && *a == *b) { ++a; ++b; }
    return *a == *b;
}
constexpr bool fb_name_ok(const char *name) {
    return name != nullptr &&
           (str_eq(name, "GarnetPrimaryFramebuffer") || str_eq(name, "AMDGarnetPrimaryFramebuffer"));
}

} // namespace rdna4agdc
