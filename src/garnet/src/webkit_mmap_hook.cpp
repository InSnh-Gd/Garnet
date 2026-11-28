//
//  webkit_mmap_hook.cpp — Standalone, zero-Lilu, kernel-version-independent
//  _mac_file_check_mmap MACF hook for Safari (com.apple.WebKit.GPU) Metal loading
//  with macOS SSV completely sealed and untouched (Option B).
//
//  Modularized behind garnet_webkit::{install, uninstall} so a future Lilu
//  migration can replace only this translation unit.
//
#include "webkit_mmap_hook.hpp"
#include "webkit_mmap_hook_pure.h"
#include "amd/rdna4_log.h"

#include <IOKit/IOLib.h>
#include <IOKit/IOService.h>
#include <i386/machine_routines.h>
#include <pexpert/pexpert.h>
#include <sys/fcntl.h>
#include <sys/mount.h>
#include <sys/proc.h>
#include <sys/uio.h>
#include <sys/vnode.h>

#ifndef FREAD
#define FREAD 0x0001
#endif

extern "C" {
proc_t current_proc(void);
int garnet_webkit_mmap_trampoline(void *cred, void *fg, int prot, int flags,
                                  uint64_t file_pos, int *maxprot);
}

__asm__(
    ".text\n"
    ".p2align 6\n"
    ".globl _garnet_webkit_mmap_trampoline\n"
    "_garnet_webkit_mmap_trampoline:\n"
    ".rept 64\n"
    "  nop\n"
    ".endr\n"
);

namespace {

using MacFileCheckMmapFn = int (*)(void *cred, void *fg, int prot, int flags,
                                   uint64_t file_pos, int *maxprot);
using FgGetVnodeFn       = vnode_t (*)(void *fg);
using ProcBestNameFn     = const char *(*)(proc_t p);
using MpRendezvousFn     = void (*)(void (*action_func)(void *), void *arg);

constexpr char   kKernelPath[]        = "/System/Library/Kernels/kernel";
constexpr size_t kHeaderReadBytes     = 64u * 1024u;
constexpr size_t kSymChunkBytes       = 64u * 1024u;

garnet_webkit::HookStatus      gStatus         = garnet_webkit::HookStatus::kNotEvaluated;
garnet_webkit::ResolvedTargets gTargets        {};
garnet_webkit::DecodedPrologue gPrologue       {};
MacFileCheckMmapFn             gOrigMmap       = nullptr;
FgGetVnodeFn                   gFgGetVnode     = nullptr;
ProcBestNameFn                 gProcBestName   = nullptr;
MpRendezvousFn                 gMpRendezvous   = nullptr;

volatile uint32_t              gInstalled      = 0u;
volatile uint32_t              gUninstalling   = 0u;
volatile uint32_t              gActiveCalls    = 0u;
volatile uint64_t              gAllowCount     = 0ULL;
volatile uint64_t              gPassDenyCount  = 0ULL;

inline uint64_t read_cr0() {
    uint64_t val = 0;
    __asm__ volatile("movq %%cr0, %0" : "=r"(val));
    return val;
}

inline void write_cr0(uint64_t val) {
    __asm__ volatile("movq %0, %%cr0" : : "r"(val) : "memory");
}

inline void serialize_icache() {
    uint32_t eax = 0u, ebx = 0u, ecx = 0u, edx = 0u;
    __asm__ volatile("cpuid" : "+a"(eax), "=b"(ebx), "=c"(ecx), "=d"(edx) :: "memory");
}

inline void write_ro_bytes(uintptr_t dstAddr, const uint8_t *src, size_t len) {
    volatile uint8_t *dst = reinterpret_cast<volatile uint8_t *>(dstAddr);
    for (size_t i = 0; i < len; ++i) {
        dst[i] = src[i];
    }
}

void write_kernel_text_local(uintptr_t dstAddr, const uint8_t *src, size_t len) {
    const boolean_t prevInts = ml_set_interrupts_enabled(FALSE);
    const uint64_t cr0 = read_cr0();
    const bool wp = (cr0 & (1ULL << 16)) != 0ULL;
    if (wp) write_cr0(cr0 & ~(1ULL << 16));

    write_ro_bytes(dstAddr, src, len);
    __asm__ volatile("mfence" ::: "memory");

    if (wp) write_cr0(cr0);
    serialize_icache();
    ml_set_interrupts_enabled(prevInts);
}

struct RendezvousPatchCtx {
    volatile uint32_t claimed;
    volatile uint32_t done;
    uintptr_t         dstAddr;
    const uint8_t    *src;
    size_t            len;
};

void rendezvous_patch_action(void *arg) {
    auto *ctx = static_cast<RendezvousPatchCtx *>(arg);
    if (__atomic_exchange_n(&ctx->claimed, 1u, __ATOMIC_ACQ_REL) == 0u) {
        const uint64_t cr0 = read_cr0();
        const bool wp = (cr0 & (1ULL << 16)) != 0ULL;
        if (wp) write_cr0(cr0 & ~(1ULL << 16));

        write_ro_bytes(ctx->dstAddr, ctx->src, ctx->len);
        __asm__ volatile("mfence" ::: "memory");

        if (wp) write_cr0(cr0);
        __atomic_store_n(&ctx->done, 1u, __ATOMIC_RELEASE);
    } else {
        while (__atomic_load_n(&ctx->done, __ATOMIC_ACQUIRE) == 0u) {
            __asm__ volatile("pause" ::: "memory");
        }
    }
    serialize_icache();
}

void patch_kernel_text_smp(uintptr_t dstAddr, const uint8_t *src, size_t len) {
    if (gMpRendezvous) {
        RendezvousPatchCtx ctx { 0u, 0u, dstAddr, src, len };
        gMpRendezvous(&rendezvous_patch_action, &ctx);
    } else {
        write_kernel_text_local(dstAddr, src, len);
    }
}

bool read_exact_vnode(vnode_t vp, vfs_context_t ctx, off_t offset, void *buf, size_t len) {
    uint8_t *dst = static_cast<uint8_t *>(buf);
    size_t done = 0u;
    while (done < len) {
        const size_t chunk = (len - done > 0x100000u) ? 0x100000u : (len - done);
        int resid = 0;
        const int err = vn_rdwr(UIO_READ, vp, reinterpret_cast<caddr_t>(dst + done),
                                static_cast<int>(chunk), offset + static_cast<off_t>(done),
                                UIO_SYSSPACE, 0, vfs_context_ucred(ctx), &resid,
                                vfs_context_proc(ctx));
        if (err != 0) return false;
        const size_t got = chunk - static_cast<size_t>(resid);
        if (got == 0u) return false;
        done += got;
    }
    return true;
}

garnet_webkit::HookStatus read_disk_kernel_symbols(garnet_webkit::DiskSymbols &out) {
    out = garnet_webkit::DiskSymbols {};
    vfs_context_t ctx = vfs_context_current();
    if (!ctx) return garnet_webkit::HookStatus::kKernelOpenFailed;

    vnode_t vp = nullptr;
    if (vnode_open(kKernelPath, FREAD, 0, 0, &vp, ctx) != 0 || !vp) {
        return garnet_webkit::HookStatus::kKernelOpenFailed;
    }

    garnet_webkit::HookStatus status = garnet_webkit::HookStatus::kInvalidMachO;
    struct vnode_attr va {};
    VATTR_INIT(&va);
    VATTR_WANTED(&va, va_data_size);
    uint64_t fileSize = 0ULL;
    if (vnode_getattr(vp, &va, ctx) != 0 || !VATTR_IS_SUPPORTED(&va, va_data_size) ||
        va.va_data_size < 64ULL || va.va_data_size > garnet_webkit::kMaxKernelFileBytes) {
        vnode_close(vp, FREAD, ctx);
        return garnet_webkit::HookStatus::kInvalidMachO;
    }
    fileSize = va.va_data_size;

    const size_t hdrReadLen = (fileSize < kHeaderReadBytes)
                                  ? static_cast<size_t>(fileSize)
                                  : kHeaderReadBytes;
    auto *hdrBuf = static_cast<uint8_t *>(IOMalloc(hdrReadLen));
    if (!hdrBuf) {
        vnode_close(vp, FREAD, ctx);
        return garnet_webkit::HookStatus::kKernelOpenFailed;
    }

    garnet_webkit::SymtabLayout lay {};
    if (!read_exact_vnode(vp, ctx, 0, hdrBuf, hdrReadLen) ||
        !garnet_webkit::parse_macho64_symtab(hdrBuf, hdrReadLen, fileSize, lay)) {
        IOFree(hdrBuf, hdrReadLen);
        vnode_close(vp, FREAD, ctx);
        return garnet_webkit::HookStatus::kInvalidMachO;
    }
    IOFree(hdrBuf, hdrReadLen);

    auto *strtab = static_cast<char *>(IOMalloc(lay.strsize));
    auto *symBuf = static_cast<uint8_t *>(IOMalloc(kSymChunkBytes));
    if (!strtab || !symBuf) {
        if (strtab) IOFree(strtab, lay.strsize);
        if (symBuf) IOFree(symBuf, kSymChunkBytes);
        vnode_close(vp, FREAD, ctx);
        return garnet_webkit::HookStatus::kKernelOpenFailed;
    }

    if (read_exact_vnode(vp, ctx, static_cast<off_t>(lay.stroff), strtab, lay.strsize)) {
        const size_t totalSymBytes = static_cast<size_t>(lay.nsyms) * 16u;
        size_t pos = 0u;
        bool ok = true;
        while (pos < totalSymBytes) {
            size_t chunk = totalSymBytes - pos;
            if (chunk > kSymChunkBytes) chunk = kSymChunkBytes;
            chunk &= ~static_cast<size_t>(15u);
            if (chunk == 0u) break;
            if (!read_exact_vnode(vp, ctx, static_cast<off_t>(lay.symoff + pos), symBuf, chunk)) {
                ok = false;
                break;
            }
            garnet_webkit::scan_nlist64_chunk(symBuf, chunk, strtab, lay.strsize, out);
            pos += chunk;
            if (garnet_webkit::has_required_symbols(out) && out.mpRendezvousNoIntrs != 0ULL) {
                break;
            }
        }
        status = (ok && garnet_webkit::has_required_symbols(out))
                     ? garnet_webkit::HookStatus::kInstalled
                     : garnet_webkit::HookStatus::kSymbolsMissing;
    }

    IOFree(symBuf, kSymChunkBytes);
    IOFree(strtab, lay.strsize);
    vnode_close(vp, FREAD, ctx);
    return status;
}

// Hot-path wrapper for _mac_file_check_mmap.
// Zero locks, zero heap allocations, zero IOLog.
int hooked_mac_file_check_mmap(void *cred, void *fg, int prot, int flags,
                               uint64_t file_pos, int *maxprot) {
    __atomic_add_fetch(&gActiveCalls, 1u, __ATOMIC_ACQ_REL);
    int r = gOrigMmap(cred, fg, prot, flags, file_pos, maxprot);

    // Fast path: already allowed (or uninstalling) -> immediate return.
    if (r == 0 || __atomic_load_n(&gUninstalling, __ATOMIC_ACQUIRE) != 0u) {
        __atomic_sub_fetch(&gActiveCalls, 1u, __ATOMIC_ACQ_REL);
        return r;
    }

    // Cheap early filters before touching vnodes or paths.
    if (fg != nullptr && maxprot != nullptr && (prot & garnet_webkit::kVmProtExecute) != 0) {
        proc_t p = current_proc();
        const char *pname = (p && gProcBestName) ? gProcBestName(p) : nullptr;
        if (garnet_webkit::is_webkit_proc_name(pname)) {
            vnode_t vp = gFgGetVnode ? gFgGetVnode(fg) : nullptr;
            if (vp != nullptr && vnode_isreg(vp) != 0) {
                mount_t mp = vnode_mount(vp);
                const bool mountNoExec = (mp == nullptr) || ((vfs_flags(mp) & MNT_NOEXEC) != 0ULL);
                if (!mountNoExec) {
                    char pathBuf[256];
                    int pathLen = static_cast<int>(sizeof(pathBuf));
                    if (vn_getpath(vp, pathBuf, &pathLen) == 0) {
                        pathBuf[sizeof(pathBuf) - 1u] = '\0';
                        if (garnet_webkit::should_allow_webkit_mmap(
                                r, true, prot, pname, true, false, pathBuf)) {
                            *maxprot |= garnet_webkit::kVmProtExecute;
                            __atomic_add_fetch(&gAllowCount, 1ULL, __ATOMIC_RELAXED);
                            __atomic_sub_fetch(&gActiveCalls, 1u, __ATOMIC_ACQ_REL);
                            return 0;
                        }
                    }
                }
            }
        }
    }

    __atomic_add_fetch(&gPassDenyCount, 1ULL, __ATOMIC_RELAXED);
    __atomic_sub_fetch(&gActiveCalls, 1u, __ATOMIC_ACQ_REL);
    return r;
}

void publish_telemetry(IOService *owner) {
    if (!owner) return;
    owner->setProperty("Garnet,WebKitMmapHook",
                       __atomic_load_n(&gInstalled, __ATOMIC_ACQUIRE) != 0u);
    owner->setProperty("Garnet,WebKitMmapStatus", garnet_webkit::status_name(gStatus));
    owner->setProperty("Garnet,WebKitMmapPrologueBytes",
                       static_cast<uint64_t>(gPrologue.len), 32);
    owner->setProperty("Garnet,WebKitMmapRendezvous", gMpRendezvous != nullptr);
}

} // namespace

namespace garnet_webkit {

void install(IOService *owner) {
    if (__atomic_load_n(&gInstalled, __ATOMIC_ACQUIRE) != 0u) {
        publish_telemetry(owner);
        return;
    }

    uint32_t argVal = 1u;
    const bool present = PE_parse_boot_argn("garnet-webkitgpu", &argVal, sizeof(argVal)) != FALSE;
    if (!enabled_by_boot_arg(present, argVal)) {
        gStatus = HookStatus::kDisabledByBootArg;
        amdgpu::rdna4_logf("Garnet: webkit-mmap: disabled by boot-arg garnet-webkitgpu=0\n");
        publish_telemetry(owner);
        return;
    }

    DiskSymbols disk {};
    const HookStatus symSt = read_disk_kernel_symbols(disk);
    if (symSt != HookStatus::kInstalled) {
        gStatus = symSt;
        amdgpu::rdna4_logf("Garnet: webkit-mmap: failed to read kernel LC_SYMTAB (%s); inactive\n",
                           status_name(symSt));
        publish_telemetry(owner);
        return;
    }

    const uint64_t rtCurrentProc = reinterpret_cast<uint64_t>(&current_proc);
    const uint64_t rtVnGetpath   = reinterpret_cast<uint64_t>(&vn_getpath);
    const HookStatus resSt = resolve_runtime_targets(disk, rtCurrentProc, rtVnGetpath, gTargets);
    if (resSt != HookStatus::kInstalled) {
        gStatus = resSt;
        amdgpu::rdna4_logf("Garnet: webkit-mmap: dual-anchor slide verification failed (%s); inactive\n",
                           status_name(resSt));
        publish_telemetry(owner);
        return;
    }

    const uint8_t *targetCode = reinterpret_cast<const uint8_t *>(gTargets.macFileCheckMmap);
    if (!decode_safe_prologue(targetCode, kMaxPrologueBytes, gPrologue)) {
        gStatus = HookStatus::kUnsupportedPrologue;
        amdgpu::rdna4_logf("Garnet: webkit-mmap: unsupported _mac_file_check_mmap prologue at %#llx "
                           "(first4=%02x %02x %02x %02x); inactive\n",
                           static_cast<unsigned long long>(gTargets.macFileCheckMmap),
                           targetCode[0], targetCode[1], targetCode[2], targetCode[3]);
        publish_telemetry(owner);
        return;
    }

    uint8_t trampBytes[kTrampolineCapBytes] {};
    size_t trampLen = 0u;
    uint8_t detourBytes[kMaxPrologueBytes] {};
    const uint64_t resumeAddr = gTargets.macFileCheckMmap + gPrologue.len;
    const uint64_t hookAddr   = reinterpret_cast<uint64_t>(&hooked_mac_file_check_mmap);
    if (!encode_trampoline(trampBytes, sizeof(trampBytes), gPrologue, resumeAddr, trampLen) ||
        !encode_detour(detourBytes, sizeof(detourBytes), gPrologue.len, hookAddr)) {
        gStatus = HookStatus::kUnsupportedPrologue;
        publish_telemetry(owner);
        return;
    }

    gFgGetVnode   = reinterpret_cast<FgGetVnodeFn>(gTargets.fgGetVnode);
    gProcBestName = reinterpret_cast<ProcBestNameFn>(gTargets.procBestName);
    gMpRendezvous = gTargets.mpRendezvousNoIntrs
                        ? reinterpret_cast<MpRendezvousFn>(gTargets.mpRendezvousNoIntrs)
                        : nullptr;
    gOrigMmap     = &garnet_webkit_mmap_trampoline;

    // 1. Populate our kext's aligned .text trampoline BEFORE activating the detour.
    write_kernel_text_local(reinterpret_cast<uintptr_t>(&garnet_webkit_mmap_trampoline),
                            trampBytes, trampLen);

    // 2. Patch _mac_file_check_mmap entry atomically across CPUs.
    __atomic_store_n(&gUninstalling, 0u, __ATOMIC_RELEASE);
    patch_kernel_text_smp(static_cast<uintptr_t>(gTargets.macFileCheckMmap),
                          detourBytes, gPrologue.len);

    __atomic_store_n(&gInstalled, 1u, __ATOMIC_RELEASE);
    gStatus = HookStatus::kInstalled;
    amdgpu::rdna4_logf("Garnet: webkit-mmap: installed on _mac_file_check_mmap=%#llx "
                       "(slide=%#llx, prologue=%zuB, smp_rendezvous=%u)\n",
                       static_cast<unsigned long long>(gTargets.macFileCheckMmap),
                       static_cast<unsigned long long>(gTargets.slide),
                       gPrologue.len, gMpRendezvous ? 1u : 0u);
    publish_telemetry(owner);
}

void uninstall() {
    if (__atomic_load_n(&gInstalled, __ATOMIC_ACQUIRE) == 0u) return;

    __atomic_store_n(&gUninstalling, 1u, __ATOMIC_RELEASE);
    if (gTargets.macFileCheckMmap != 0ULL && gPrologue.len >= kMinDetourBytes) {
        patch_kernel_text_smp(static_cast<uintptr_t>(gTargets.macFileCheckMmap),
                              gPrologue.bytes, gPrologue.len);
    }

    // Drain any in-flight wrapper invocations before kext stop completes.
    for (uint32_t i = 0; i < 200u && __atomic_load_n(&gActiveCalls, __ATOMIC_ACQUIRE) != 0u; ++i) {
        IOSleep(1);
    }

    __atomic_store_n(&gInstalled, 0u, __ATOMIC_RELEASE);
    gStatus = HookStatus::kUninstalled;
    amdgpu::rdna4_logf("Garnet: webkit-mmap: restored _mac_file_check_mmap "
                       "(allowed=%llu, passed_deny=%llu)\n",
                       static_cast<unsigned long long>(__atomic_load_n(&gAllowCount, __ATOMIC_RELAXED)),
                       static_cast<unsigned long long>(__atomic_load_n(&gPassDenyCount, __ATOMIC_RELAXED)));
}

} // namespace garnet_webkit
