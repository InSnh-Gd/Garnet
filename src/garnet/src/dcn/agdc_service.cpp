#include "agdc_service.hpp"
#include "agdc_types.h"
#include "dcn.hpp"
#include "../GarnetDisplayNub.hpp"
#include "../amd/native_agdc_flow.h"
#include "../amd/native_disp.h"
#include "../amd/native_display_identity.h"
#include "../amd/native_m6_pure.h"
#include "../amd/rdna4_log.h"

#include <IOKit/IOLib.h>
#include <IOKit/IOLocks.h>
#include <IOKit/IOService.h>
#include <libkern/c++/OSSymbol.h>
#include <sys/proc.h>

extern "C" {
int proc_selfpid(void);
void proc_selfname(char *buf, int size);
}

IOService *garnet_driver_pci(void);
IOService *garnet_driver_service(void);
uint32_t rdna4_scanout_live_dims(uint32_t *w, uint32_t *h);

#define AGDCLOG(fmt, ...) ::amdgpu::rdna4_logf("agdc: " fmt "\n", ##__VA_ARGS__)

namespace {

constexpr char kAgdcToken[] = "native-agdc";
constexpr uintptr_t kKernelHalf = 0xffffff7000000000ull;
constexpr unsigned kVtHdr = 2;

constexpr uintptr_t kAgdcVtable        = 0x13d44478ull;
constexpr unsigned  kAgdcVtSlots       = 276;
constexpr uintptr_t kAgdcOpNew         = 0x13d3fea6ull;
constexpr uintptr_t kAgdcCtorMeta      = 0x13d3fe34ull;
constexpr uintptr_t kAgdcD2            = 0x13d3fe54ull;
constexpr uintptr_t kAgdcOpDelete      = 0x13d3febaull;
constexpr uintptr_t kAgdcStart         = 0x13d40392ull;
constexpr uintptr_t kAgdcNewUserClient = 0x13d41cbeull;
constexpr uintptr_t kAgdcVendorBase    = 0x13d41c34ull;
constexpr uintptr_t kAgdcGetMetaClass  = 0x13d3fe5eull;
constexpr uint32_t  kAgdcClassSize     = 0x110u;

const uint8_t kBytesOpNew[]    = {0x55, 0x48, 0x89, 0xe5, 0x48, 0x89, 0xfe, 0x48, 0x8d, 0x3d};
const uint8_t kBytesCtorMeta[] = {0x55, 0x48, 0x89, 0xe5, 0x53, 0x50, 0x48, 0x89, 0xfb, 0xe8};
const uint8_t kBytesD2[]       = {0x55, 0x48, 0x89, 0xe5, 0x5d, 0xe9};
const uint8_t kBytesOpDelete[] = {0x55, 0x48, 0x89, 0xe5, 0x48, 0x89, 0xf2, 0x48, 0x89, 0xfe, 0x48, 0x8d, 0x3d};

using AgdcOpNewFn    = void *(*)(size_t);
using AgdcCtorFn     = void (*)(void *, const OSMetaClass *);
using AgdcD2Fn       = void (*)(void *);
using AgdcOpDeleteFn = void (*)(void *, size_t);

inline bool agdc_kptr(const void *p) { return reinterpret_cast<uintptr_t>(p) >= kKernelHalf; }

const char *agdc_class_name(const OSObject *obj) {
    if (!agdc_kptr(obj)) return "(not a kernel pointer)";
    const OSMetaClass *mc = obj->getMetaClass();
    return agdc_kptr(mc) ? mc->getClassName() : "(no metaclass)";
}

IOLock *gAgdcLock = nullptr;
inline void agdc_lock_init() {
    if (!gAgdcLock) gAgdcLock = IOLockAlloc();
}

struct {
    uint32_t published, lastStatus, startOk;
    uintptr_t slide;
    void **vtCopy;
    IOService *obj, *fb, *pci;
    AgdcD2Fn d2;
    AgdcOpDeleteFn opDelete;
    uint64_t regId;
    uint64_t calls, vendorInfo, gpuCap, linkCfg, pipeCaps, other, badLen, logged;
    uint32_t cmdSeen[16];
    uint64_t cmdCount[16];
    uint32_t dumped711, dumpedCmd;
    uint32_t holdMs, holdFired, holdEntered, holdSkipped;
} gAg {};

rdna4m6::AgdcList gAgm {};
IOService *gAgmObj[rdna4m6::kInstCount] = {nullptr, nullptr, nullptr};

void agdc_count_cmd(uint32_t cmd) {
    for (unsigned i = 0; i < 16; ++i) {
        if (gAg.cmdCount[i] && gAg.cmdSeen[i] == cmd) {
            gAg.cmdCount[i]++;
            return;
        }
        if (!gAg.cmdCount[i]) {
            gAg.cmdSeen[i] = cmd;
            gAg.cmdCount[i] = 1;
            return;
        }
    }
}

void agdc_dump_reply(uint32_t cmd, uint32_t type, const uint8_t *buf, size_t len, uint32_t kr) {
    if (!buf || len < 4u) return;
    const size_t nd = len / 4u;
    size_t nz = 0;
    for (size_t i = 0; i < nd; ++i) {
        if (rdna4_agdc_rd32(buf, static_cast<uint32_t>(i * 4u))) ++nz;
    }
    AGDCLOG("REPLYDUMP cmd %#x type %#x len %#lx kr %#x dwords %lu nonzero %lu",
            cmd, type, static_cast<unsigned long>(len), kr,
            static_cast<unsigned long>(nd), static_cast<unsigned long>(nz));
    char line[192];
    size_t used = 0;
    unsigned onLine = 0;
    line[0] = '\0';
    for (size_t i = 0; i < nd; ++i) {
        const uint32_t v = rdna4_agdc_rd32(buf, static_cast<uint32_t>(i * 4u));
        if (!v) continue;
        const int n = snprintf(line + used, sizeof(line) - used, " +%#06lx=%#010x",
                               static_cast<unsigned long>(i * 4u), v);
        if (n > 0 && static_cast<size_t>(n) < sizeof(line) - used) {
            used += static_cast<size_t>(n);
            ++onLine;
        }
        if (onLine >= 6u || used >= sizeof(line) - 24u) {
            AGDCLOG("REPLYDUMP cmd %#x type %#x dw%s", cmd, type, line);
            line[0] = '\0';
            used = 0;
            onLine = 0;
        }
    }
    if (onLine) AGDCLOG("REPLYDUMP cmd %#x type %#x dw%s", cmd, type, line);
}

__attribute__((noinline)) void agdc_endpoint(uint32_t *w, uint32_t *h) {
    uint32_t cw = 0u, ch = 0u;
    (void)rdna4_scanout_live_dims(&cw, &ch);
    *w = rdna4_agdc_endpoint_dim(0u, cw, 1920u);
    *h = rdna4_agdc_endpoint_dim(0u, ch, 1080u);
}

uint32_t agdc_live_raster(void *ctx, rdna4_agdc_raster *lr) {
    (void)ctx;
    return rdna4dcn::liveRaster(&lr->h_active, &lr->v_active, &lr->h_total, &lr->v_total,
                                &lr->h_front, &lr->h_sync, &lr->v_front, &lr->v_sync,
                                &lr->pixel_clock_hz);
}

__attribute__((noinline)) uint32_t agdc_link_config(uint8_t *out, size_t len, uint32_t lw, uint32_t lh) {
    rdna4_agdc_timing lt {};
    rdna4_agdc_link_timing(lw, lh, &agdc_live_raster, nullptr, &lt);
    const uint32_t kr = rdna4_agdc_fill_link_config_t(out, len, &lt) ? 0xe00002c2u : 0u;
    if (kr) gAg.badLen++;
    if (gAg.linkCfg == 1u) {
        AGDCLOG("LINKCFG endpoint %ux%u pixelClock %llu blank %u/%u timing %s",
                lt.w, lt.h, static_cast<unsigned long long>(lt.pixel_clock), lt.h_blank, lt.v_blank,
                lt.live ? "OTG raster with programmed or table clock" : "CEA 1080p blanking");
    }
    return kr;
}

uint32_t agdc_vendor(void *self, uint32_t cmd, unsigned long *in, unsigned long inLen,
                     unsigned long *outp, unsigned long *outLen, void *args) {
    (void)in;
    (void)inLen;
    (void)args;
    gAg.calls++;
    agdc_count_cmd(cmd);
    if (gAg.holdMs && !gAg.holdFired && cmd == RDNA4_AGDC_CMD_LINK_CONFIG) {
        char hn[20] = {0};
        proc_selfname(hn, static_cast<int>(sizeof(hn)));
        const int hpid = proc_selfpid();
        const uint32_t ms = rdna4_agdc_hold_ms(gAg.holdMs, gAg.holdFired, cmd, hn,
                                               static_cast<uint32_t>(sizeof(hn)));
        if (ms) {
            gAg.holdFired = 1u;
            gAg.holdEntered++;
            AGDCLOG("HOLD enter call #%llu cmd %#x pid %d (%s) sleeping %u ms",
                    static_cast<unsigned long long>(gAg.calls), cmd, hpid, hn, ms);
            IOSleep(ms);
            AGDCLOG("HOLD exit after %u ms (entered %u, skipped %u)",
                    ms, gAg.holdEntered, gAg.holdSkipped);
        } else {
            gAg.holdSkipped++;
            AGDCLOG("HOLD SKIPPED (fail-closed) - caller pid %d name '%s' not WindowServer or ms %u out of range",
                    hpid, hn, gAg.holdMs);
        }
    }

    uint32_t kr = 0xe00002c7u;
    const size_t len = outLen ? static_cast<size_t>(*outLen) : 0u;
    const uint64_t oor0 = gAgm.nOor;
    const bool m6done = gAgm.nfb > 1u &&
                        rdna4m6::agdc_answer(gAgm, cmd, reinterpret_cast<uint8_t *>(outp), len,
                                             reinterpret_cast<uint64_t>(gAg.pci), &kr);
    if (gAgm.nOor != oor0 && (gAgm.nOor <= 4u || (gAgm.nOor & 0x3ffu) == 0u)) {
        AGDCLOG("m6: endpoint dword %llu outside %u-entry 0x980 list (cmd %#x): answered as DP; #%llu",
                static_cast<unsigned long long>(gAgm.lastOor), gAgm.nfb, cmd,
                static_cast<unsigned long long>(gAgm.nOor));
    }

    const uint32_t capType = (cmd == RDNA4_AGDC_CMD_PIPELINE_CAPS && outp && len >= 8u)
                                 ? rdna4_agdc_rd32(reinterpret_cast<const uint8_t *>(outp), 4u)
                                 : 0u;
    if (m6done) {
        if (cmd == RDNA4_AGDC_CMD_GPU_CAPABILITY) gAg.gpuCap++;
        else if (cmd == RDNA4_AGDC_CMD_LINK_CONFIG) gAg.linkCfg++;
        else gAg.pipeCaps++;
        if (kr) gAg.badLen++;
    } else if (cmd == RDNA4_AGDC_CMD_VENDOR_INFO) {
        gAg.vendorInfo++;
        kr = rdna4_agdc_fill_vendor_info(reinterpret_cast<uint8_t *>(outp), len) ? 0xe00002c2u : 0u;
        if (kr) gAg.badLen++;
    } else if (cmd == RDNA4_AGDC_CMD_GPU_CAPABILITY) {
        gAg.gpuCap++;
        const uint64_t fbs[1] = {reinterpret_cast<uint64_t>(gAg.fb)};
        kr = (gAg.fb && gAg.pci &&
              !rdna4_agdc_fill_gpu_capability(reinterpret_cast<uint8_t *>(outp), len,
                                              reinterpret_cast<uint64_t>(gAg.pci), fbs, 1u))
                 ? 0u
                 : 0xe00002c2u;
        if (kr) gAg.badLen++;
        if (!(gAg.dumpedCmd & 2u)) {
            gAg.dumpedCmd |= 2u;
            agdc_dump_reply(cmd, 0u, reinterpret_cast<const uint8_t *>(outp), len, kr);
        }
    } else if (cmd == RDNA4_AGDC_CMD_LINK_CONFIG) {
        gAg.linkCfg++;
        uint32_t lw = 0u, lh = 0u;
        agdc_endpoint(&lw, &lh);
        kr = agdc_link_config(reinterpret_cast<uint8_t *>(outp), len, lw, lh);
        if (!(gAg.dumpedCmd & 1u)) {
            gAg.dumpedCmd |= 1u;
            agdc_dump_reply(cmd, 0u, reinterpret_cast<const uint8_t *>(outp), len, kr);
        }
    } else if (cmd == RDNA4_AGDC_CMD_PIPELINE_CAPS) {
        gAg.pipeCaps++;
        uint32_t w = 0u, h = 0u;
        agdc_endpoint(&w, &h);
        kr = rdna4_agdc_fill_pipeline_caps(reinterpret_cast<uint8_t *>(outp), len, w, h) ? 0xe00002c2u : 0u;
        if (kr) gAg.badLen++;
        const bool oneBit = capType && !(capType & (capType - 1u)) && capType <= 0x8000u;
        const uint32_t mask = oneBit ? capType : 0u;
        if (oneBit ? !(gAg.dumped711 & mask) : !(gAg.dumpedCmd & 4u)) {
            if (oneBit) gAg.dumped711 |= mask;
            else gAg.dumpedCmd |= 4u;
            agdc_dump_reply(cmd, capType, reinterpret_cast<const uint8_t *>(outp), len, kr);
        }
    } else {
        gAg.other++;
    }

    if (gAg.logged < 48u) {
        gAg.logged++;
        char pn[20] = {0};
        proc_selfname(pn, static_cast<int>(sizeof(pn)));
        AGDCLOG("vendor_doDeviceAttribute #%llu on %p cmd %#x out %p len %#lx from pid %d (%s) -> %#x",
                static_cast<unsigned long long>(gAg.calls), self, cmd, outp,
                static_cast<unsigned long>(len), proc_selfpid(), pn, kr);
    }
    return kr;
}

void agdc_d1(void *self) {
    if (gAg.d2) gAg.d2(self);
}

void agdc_d0(void *self) {
    AGDCLOG("deleting AGDC nub %p", self);
    if (gAg.d2) gAg.d2(self);
    if (gAg.opDelete) gAg.opDelete(self, kAgdcClassSize);
}

bool agdc_bytes(uintptr_t fn, const uint8_t *want, size_t n) {
    return memcmp(reinterpret_cast<const uint8_t *>(fn), want, n) == 0;
}

uint32_t agdc_class_checks(const OSMetaClass *mc, uintptr_t slide) {
    if (mc->getClassSize() != kAgdcClassSize) {
        AGDCLOG("REFUSED - class size %u, want %#x", mc->getClassSize(), kAgdcClassSize);
        return rdna4agdc::kClassSize;
    }
    if (!agdc_bytes(kAgdcOpNew + slide, kBytesOpNew, sizeof(kBytesOpNew)) ||
        !agdc_bytes(kAgdcCtorMeta + slide, kBytesCtorMeta, sizeof(kBytesCtorMeta)) ||
        !agdc_bytes(kAgdcD2 + slide, kBytesD2, sizeof(kBytesD2)) ||
        !agdc_bytes(kAgdcOpDelete + slide, kBytesOpDelete, sizeof(kBytesOpDelete))) {
        AGDCLOG("REFUSED - expected instruction prologue mismatch");
        return rdna4agdc::kCodeBytes;
    }
    void **vt = reinterpret_cast<void **>(kAgdcVtable + slide + 16);
    if (reinterpret_cast<uintptr_t>(vt[184]) - slide != kAgdcStart ||
        reinterpret_cast<uintptr_t>(vt[238]) - slide != kAgdcNewUserClient ||
        reinterpret_cast<uintptr_t>(vt[267]) - slide != kAgdcVendorBase ||
        reinterpret_cast<uintptr_t>(vt[7]) - slide != kAgdcGetMetaClass) {
        AGDCLOG("REFUSED - AGDC vtable slots 7/184/238/267 mismatch");
        return rdna4agdc::kVtable;
    }
    return rdna4agdc::kPublished;
}

bool agdc_have_wrangler() {
    OSDictionary *m = IOService::serviceMatching("AppleGPUWrangler");
    IOService *w = m ? IOService::copyMatchingService(m) : nullptr;
    if (!w) {
        AGDCLOG("REFUSED - no AppleGPUWrangler");
        return false;
    }
    return true;
}

uint32_t agdc_build_locked(const OSMetaClass *mc, uintptr_t slide, IOService *fb,
                           IOService *pci, IOService *provider) {
    void **vt = reinterpret_cast<void **>(kAgdcVtable + slide + 16);
    void **copy = static_cast<void **>(IOMalloc((kAgdcVtSlots + kVtHdr) * sizeof(void *)));
    if (!copy) return rdna4agdc::kAlloc;
    memcpy(copy, vt - kVtHdr, (kAgdcVtSlots + kVtHdr) * sizeof(void *));
    copy[kVtHdr + 0] = reinterpret_cast<void *>(&agdc_d1);
    copy[kVtHdr + 1] = reinterpret_cast<void *>(&agdc_d0);
    copy[kVtHdr + 266] = reinterpret_cast<void *>(&agdc_vendor);
    gAg.d2 = reinterpret_cast<AgdcD2Fn>(kAgdcD2 + slide);
    gAg.opDelete = reinterpret_cast<AgdcOpDeleteFn>(kAgdcOpDelete + slide);
    gAg.fb = fb;
    gAg.pci = pci;
    gAg.slide = slide;
    gAg.vtCopy = copy;

    void *mem = reinterpret_cast<AgdcOpNewFn>(kAgdcOpNew + slide)(kAgdcClassSize);
    if (!mem) {
        IOFree(copy, (kAgdcVtSlots + kVtHdr) * sizeof(void *));
        gAg.vtCopy = nullptr;
        return rdna4agdc::kAlloc;
    }
    reinterpret_cast<AgdcCtorFn>(kAgdcCtorMeta + slide)(mem, mc);
    if (*reinterpret_cast<void ***>(mem) != vt) {
        AGDCLOG("constructor left unexpected vptr %p (want %p)", *reinterpret_cast<void ***>(mem), vt);
    }
    *reinterpret_cast<void ***>(mem) = copy + kVtHdr;
    mc->instanceConstructed();

    IOService *obj = static_cast<IOService *>(static_cast<OSObject *>(mem));
    if (!obj->init(nullptr)) {
        obj->release();
        return rdna4agdc::kStartFailed;
    }
    obj->setName("RDNA4AGDC");
    obj->setProperty("Garnet,AGDC", kAgdcToken);
    if (!obj->attach(provider)) {
        obj->release();
        return rdna4agdc::kStartFailed;
    }
    gAg.obj = obj;
    const bool ok = obj->start(provider);
    gAg.startOk = ok ? 1u : 0u;
    if (!ok) {
        AGDCLOG("start(provider %p) returned false - detaching", provider);
        obj->detach(provider);
        obj->release();
        gAg.obj = nullptr;
        return rdna4agdc::kStartFailed;
    }
    gAg.regId = obj->getRegistryEntryID();
    gAg.published = 1u;
    AGDCLOG("PUBLISHED %p regID %#llx under %s; start calls %llu (vi %llu cap %llu link %llu pipe %llu)",
            obj, static_cast<unsigned long long>(gAg.regId), agdc_class_name(provider),
            static_cast<unsigned long long>(gAg.calls), static_cast<unsigned long long>(gAg.vendorInfo),
            static_cast<unsigned long long>(gAg.gpuCap), static_cast<unsigned long long>(gAg.linkCfg),
            static_cast<unsigned long long>(gAg.pipeCaps));
    return rdna4agdc::kPublished;
}

void agdc_fill_out(uint32_t st, uint64_t *out, unsigned count) {
    uint64_t v[13] = {
        st,
        static_cast<uint64_t>(gAg.published) | (static_cast<uint64_t>(gAg.startOk) << 1),
        gAg.regId,
        gAg.calls,
        gAg.vendorInfo | (gAg.gpuCap << 32),
        gAg.other | (gAg.badLen << 32),
        0, 0, 0, 0, 0, 0, 0,
    };
    for (unsigned i = 0; i < 7; ++i) {
        v[6 + i] = (static_cast<uint64_t>(gAg.cmdSeen[i]) << 32) |
                   (gAg.cmdCount[i] & 0xffffffffull);
    }
    if (out) {
        for (unsigned i = 0; i < count && i < 13u; ++i) out[i] = v[i];
    }
}

struct AgdcNativeEnv {
    IOService *fb = nullptr;
    IOService *pci = nullptr;
    IOService *provider = nullptr;
    IOService *mfb[rdna4m6::kInstCount] = {nullptr, nullptr, nullptr};
    uint32_t mn = 0;

    static const OSMetaClass *meta(const char *name) {
        const OSSymbol *nm = OSSymbol::withCString(name);
        const OSMetaClass *mc = nm ? OSMetaClass::getMetaClassWithName(nm) : nullptr;
        if (nm) nm->release();
        return mc;
    }

    bool latch_on() { return rdna4disp_latched_on(); }
    bool published() { return gAg.published != 0u; }
    bool mode_held() { return rdna4dcn::modeHoldActive(); }
    uint64_t agdc_meta() { return static_cast<uint64_t>(reinterpret_cast<uintptr_t>(meta("AppleGraphicsDeviceControl"))); }
    uint64_t pipe_meta() { return static_cast<uint64_t>(reinterpret_cast<uintptr_t>(meta("IOAccelDisplayPipe"))); }

    uint32_t class_checks(uint64_t am, uint64_t slide) {
        return agdc_class_checks(reinterpret_cast<const OSMetaClass *>(static_cast<uintptr_t>(am)),
                                 static_cast<uintptr_t>(slide));
    }

    bool have_targets() {
        IOService *primary = nullptr;
        if (!rdna4_display_primary(&primary)) {
            if (primary) primary->release();
            AGDCLOG("native: REFUSED - no validated primary framebuffer on target GPU");
            return false;
        }
        fb = primary;
        pci = garnet_driver_pci();
        provider = garnet_driver_service();
        bool ok = fb && pci && provider;
        if (!ok) {
            AGDCLOG("native: REFUSED - fb %p (%s), pci %p, provider %p",
                    fb, fb ? agdc_class_name(fb) : "none", pci, provider);
        }
        if (ok && rdna4m6_latched_on()) ok = collect_m6();
        return ok;
    }

    bool nub_published(uint32_t inst) { return rdna4fb_nub_exists_idx(rdna4m6::index_of_inst(inst)); }
    bool fb_started(uint32_t inst) {
        mfb[inst] = GarnetDisplayNub::framebufferOf(rdna4m6::index_of_inst(inst));
        return mfb[inst] != nullptr;
    }
    void refuse(uint32_t inst) {
        AGDCLOG("native: REFUSED - display nub %u published without started framebuffer",
                rdna4m6::index_of_inst(inst));
    }
    bool collect_m6() {
        mn = 1u;
        return rdna4m6::collect_flow(*this, &mn);
    }
    bool wrangler() { return agdc_have_wrangler(); }

    uint32_t build(uint64_t am, uint64_t slide) {
        if (mn > 1u) {
            bool have[rdna4m6::kInstCount] = {true, mfb[rdna4m6::kInstMonA] != nullptr,
                                              mfb[rdna4m6::kInstMonB] != nullptr};
            uint32_t order[rdna4m6::kInstCount];
            const uint32_t k = rdna4m6::list_insts(have, order);
            for (uint32_t j = 0; j < k; ++j) {
                IOService *o = (order[j] == rdna4m6::kInstDp) ? fb : mfb[order[j]];
                gAgm.fb[j] = reinterpret_cast<uint64_t>(o);
                gAgm.inst[j] = order[j];
                gAgmObj[j] = o;
            }
            gAgm.nfb = k;
        }
        const uint32_t st = agdc_build_locked(
            reinterpret_cast<const OSMetaClass *>(static_cast<uintptr_t>(am)),
            static_cast<uintptr_t>(slide), fb, pci, provider);
        if (st != 0u) gAgm.nfb = 0u;
        return st;
    }

    void finish(uint32_t st) {
        if (fb && st != rdna4agdc::kPublished) fb->release();
        for (uint32_t i = 0; i < rdna4m6::kInstCount; ++i) {
            if (mfb[i] && st != rdna4agdc::kPublished) mfb[i]->release();
            mfb[i] = nullptr;
        }
        fb = nullptr;
    }
};

} // namespace

void rdna4_agdc_m6_stat(uint64_t o[8]) {
    o[0] = gAgm.lastNfb;
    o[1] = gAgm.n921;
    o[2] = gAgm.n711;
    o[3] = gAgm.lastEp;
    o[4] = gAgm.nfb;
    o[5] = gAgm.nOor;
    o[6] = gAgm.lastOor;
    o[7] = 0;
}

void rdna4_agdc_m6_stat2(uint64_t o[12]) {
    for (unsigned i = 0; i < rdna4m6::kInstCount; ++i) {
        o[i] = gAgm.n921i[i];
        o[3 + i] = gAgm.n711i[i];
    }
    o[6] = gAgm.epSeen;
    o[7] = gAgm.nfb;
    o[8] = gAgm.nOor;
    o[9] = gAgm.lastEp;
    o[10] = 0;
    o[11] = 0;
}

uint32_t rdna4_agdc_native_control(uint64_t arg, uint64_t *out, unsigned count) {
    agdc_lock_init();
    AgdcNativeEnv env;
    IOLockLock(gAgdcLock);
    const uint32_t st = rdna4agdc::native_flow(env, arg);
    if (arg == 1ull) gAg.lastStatus = st;
    IOLockUnlock(gAgdcLock);
    env.finish(st);
    if (arg == 1ull) AGDCLOG("native publish -> status %u (%s)", st, rdna4agdc::status_name(st));
    agdc_fill_out(st, out, count);
    return st;
}

uint32_t rdna4_agdc_hold_control(uint64_t arg, uint64_t *out, unsigned count) {
    uint32_t st = 0;
    if (arg != 0) {
        if (arg > static_cast<uint64_t>(RDNA4_AGDC_HOLD_MAX_MS)) {
            st = 13u;
            AGDCLOG("hold REFUSED - %llu ms above %u ms ceiling",
                    static_cast<unsigned long long>(arg), RDNA4_AGDC_HOLD_MAX_MS);
        } else {
            gAg.holdMs = static_cast<uint32_t>(arg);
            gAg.holdFired = 0u;
            AGDCLOG("hold ARMED %u ms on cmd %#x (WindowServer only, one shot)",
                    gAg.holdMs, RDNA4_AGDC_CMD_LINK_CONFIG);
        }
    }
    uint64_t v[13] = {
        st, gAg.holdMs, gAg.holdFired, gAg.holdEntered, gAg.holdSkipped, gAg.calls, gAg.linkCfg,
        0, 0, 0, 0, 0, 0,
    };
    if (out) {
        for (unsigned i = 0; i < count && i < 13u; ++i) out[i] = v[i];
    }
    return st;
}
