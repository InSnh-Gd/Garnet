//
//  webkit_mmap_hook_pure.h — Pure, host-testable logic for Garnet's standalone
//  kernel-version-independent _mac_file_check_mmap MACF hook (Option B).
//
//  Zero kernel headers, zero hardcoded offsets, zero kernel UUIDs:
//    1. Parses Mach-O 64-bit LC_SYMTAB from /System/Library/Kernels/kernel.
//    2. Computes and cross-verifies dual-anchor __TEXT slide against live KPI
//       addresses (&current_proc and &vn_getpath).
//    3. Decodes x86_64 function prologue at instruction boundaries (fail-closed
//       on any RIP-relative, branch, call, or unrecognized opcode) and encodes
//       a 14-byte absolute detour + executable trampoline.
//    4. Enforces strict fail-closed Seatbelt exemption policy: only when
//       Seatbelt already denied a VM_PROT_EXECUTE mmap from com.apple.WebKit*
//       targeting a regular non-MNT_NOEXEC file under
//       /Library/GPUBundles/GarnetMetal.bundle/.
//
#pragma once

#include <stddef.h>
#include <stdint.h>
#include <string.h>

namespace garnet_webkit {

constexpr uint64_t kKernelCanonicalMin = 0xFFFFFF8000000000ULL;
constexpr uint64_t kPageMask           = 0xFFFULL;

constexpr size_t   kMinDetourBytes     = 14u;   // FF 25 00 00 00 00 <u64 target>
constexpr size_t   kMaxPrologueBytes   = 32u;
constexpr size_t   kTrampolineCapBytes = 64u;   // >= kMaxPrologueBytes + kMinDetourBytes

constexpr uint64_t kMaxKernelFileBytes = 64ULL * 1024ULL * 1024ULL;
constexpr uint32_t kMaxSymtabEntries   = 2000000u;
constexpr uint32_t kMaxStrtabBytes     = 16u * 1024u * 1024u;

constexpr int      kVmProtExecute      = 0x04;

constexpr char kWebKitProcPrefix[]     = "com.apple.WebKit";
constexpr char kGarnetBundlePrefix[]   = "/Library/GPUBundles/GarnetMetal.bundle/";

enum class HookStatus : uint32_t {
    kNotEvaluated = 0,
    kDisabledByBootArg,
    kKernelOpenFailed,
    kInvalidMachO,
    kSymbolsMissing,
    kSlideMismatch,
    kBadTargetAddr,
    kUnsupportedPrologue,
    kInstalled,
    kUninstalled,
};

inline const char *status_name(HookStatus s) {
    switch (s) {
    case HookStatus::kNotEvaluated:        return "not-evaluated";
    case HookStatus::kDisabledByBootArg:   return "disabled-by-boot-arg";
    case HookStatus::kKernelOpenFailed:    return "kernel-open-failed";
    case HookStatus::kInvalidMachO:        return "invalid-macho";
    case HookStatus::kSymbolsMissing:      return "symbols-missing";
    case HookStatus::kSlideMismatch:       return "slide-mismatch";
    case HookStatus::kBadTargetAddr:       return "bad-target-addr";
    case HookStatus::kUnsupportedPrologue: return "unsupported-prologue";
    case HookStatus::kInstalled:           return "installed";
    case HookStatus::kUninstalled:         return "uninstalled";
    }
    return "unknown";
}

// Default ON; boot-arg garnet-webkitgpu=0 disables the hook cleanly.
inline bool enabled_by_boot_arg(bool present, uint32_t value) {
    return !present || value != 0u;
}

inline bool is_kernel_text_addr(uint64_t addr) {
    return addr >= kKernelCanonicalMin;
}

inline uint32_t read_le32(const uint8_t *p) {
    return static_cast<uint32_t>(p[0])
         | (static_cast<uint32_t>(p[1]) << 8)
         | (static_cast<uint32_t>(p[2]) << 16)
         | (static_cast<uint32_t>(p[3]) << 24);
}

inline uint64_t read_le64(const uint8_t *p) {
    return static_cast<uint64_t>(read_le32(p))
         | (static_cast<uint64_t>(read_le32(p + 4)) << 32);
}

inline void write_le64(uint8_t *p, uint64_t v) {
    for (size_t i = 0; i < 8u; ++i) {
        p[i] = static_cast<uint8_t>((v >> (i * 8u)) & 0xFFu);
    }
}

struct SymtabLayout {
    uint32_t symoff  { 0 };
    uint32_t nsyms   { 0 };
    uint32_t stroff  { 0 };
    uint32_t strsize { 0 };
};

// Parse mach_header_64 + load commands from a header prefix buffer and locate LC_SYMTAB.
inline bool parse_macho64_symtab(const uint8_t *hdrBuf, size_t hdrLen,
                                 uint64_t fileSize, SymtabLayout &out) {
    out = SymtabLayout {};
    if (!hdrBuf || hdrLen < 32u || fileSize < 32u || fileSize > kMaxKernelFileBytes) {
        return false;
    }
    const uint32_t magic      = read_le32(hdrBuf + 0);
    const uint32_t cputype    = read_le32(hdrBuf + 4);
    const uint32_t ncmds      = read_le32(hdrBuf + 16);
    const uint32_t sizeofcmds = read_le32(hdrBuf + 20);
    if (magic != 0xFEEDFACFu || cputype != 0x01000007u) {
        return false;
    }
    if (ncmds == 0u || sizeofcmds < 24u || sizeofcmds > hdrLen - 32u ||
        32ULL + static_cast<uint64_t>(sizeofcmds) > fileSize) {
        return false;
    }

    size_t off = 32u;
    const size_t end = 32u + static_cast<size_t>(sizeofcmds);
    for (uint32_t i = 0; i < ncmds; ++i) {
        if (off + 8u > end) return false;
        const uint32_t cmd     = read_le32(hdrBuf + off);
        const uint32_t cmdsize = read_le32(hdrBuf + off + 4u);
        if (cmdsize < 8u || (cmdsize & 3u) != 0u || cmdsize > end - off) {
            return false;
        }
        if (cmd == 0x2u) { // LC_SYMTAB
            if (cmdsize < 24u) return false;
            const uint32_t symoff  = read_le32(hdrBuf + off + 8u);
            const uint32_t nsyms   = read_le32(hdrBuf + off + 12u);
            const uint32_t stroff  = read_le32(hdrBuf + off + 16u);
            const uint32_t strsize = read_le32(hdrBuf + off + 20u);
            if (nsyms == 0u || nsyms > kMaxSymtabEntries ||
                strsize == 0u || strsize > kMaxStrtabBytes) {
                return false;
            }
            const uint64_t symBytes = static_cast<uint64_t>(nsyms) * 16ULL;
            if (static_cast<uint64_t>(symoff) > fileSize ||
                symBytes > fileSize - static_cast<uint64_t>(symoff)) {
                return false;
            }
            if (static_cast<uint64_t>(stroff) > fileSize ||
                static_cast<uint64_t>(strsize) > fileSize - static_cast<uint64_t>(stroff)) {
                return false;
            }
            out.symoff  = symoff;
            out.nsyms   = nsyms;
            out.stroff  = stroff;
            out.strsize = strsize;
            return true;
        }
        off += cmdsize;
    }
    return false;
}

struct DiskSymbols {
    uint64_t currentProc         { 0 };
    uint64_t vnGetpath           { 0 };
    uint64_t procBestName        { 0 };
    uint64_t fgGetVnode          { 0 };
    uint64_t macFileCheckMmap    { 0 };
    uint64_t mpRendezvousNoIntrs { 0 }; // optional; falls back to local cli/CR0 if 0
};

inline bool has_required_symbols(const DiskSymbols &s) {
    return s.currentProc != 0ULL &&
           s.vnGetpath != 0ULL &&
           s.procBestName != 0ULL &&
           s.fgGetVnode != 0ULL &&
           s.macFileCheckMmap != 0ULL;
}

inline bool cstr_equals_bounded(const char *strtab, size_t strsize,
                                uint32_t strx, const char *target) {
    if (!strtab || !target || strx >= strsize) return false;
    const size_t avail = strsize - static_cast<size_t>(strx);
    const char *s = strtab + strx;
    size_t i = 0;
    while (target[i] != '\0') {
        if (i >= avail || s[i] != target[i]) return false;
        ++i;
    }
    return (i < avail) && (s[i] == '\0');
}

// Scan a chunk of 16-byte nlist_64 records against an in-memory string table.
inline void scan_nlist64_chunk(const uint8_t *nlistBuf, size_t nlistBytes,
                               const char *strtab, size_t strsize,
                               DiskSymbols &out) {
    if (!nlistBuf || !strtab || strsize == 0u) return;
    const size_t count = nlistBytes / 16u;
    for (size_t i = 0; i < count; ++i) {
        const uint8_t *e = nlistBuf + i * 16u;
        const uint8_t nType = e[4];
        if ((nType & 0xE0u) != 0u) continue;        // skip N_STAB debug entries
        if ((nType & 0x0Eu) != 0x0Eu) continue;     // require N_SECT defined symbol
        const uint32_t strx = read_le32(e);
        if (strx == 0u || strx >= strsize) continue;
        const uint64_t val = read_le64(e + 8u);
        if (val == 0ULL) continue;

        if (!out.currentProc && cstr_equals_bounded(strtab, strsize, strx, "_current_proc")) {
            out.currentProc = val;
        } else if (!out.vnGetpath && cstr_equals_bounded(strtab, strsize, strx, "_vn_getpath")) {
            out.vnGetpath = val;
        } else if (!out.procBestName && cstr_equals_bounded(strtab, strsize, strx, "_proc_best_name")) {
            out.procBestName = val;
        } else if (!out.fgGetVnode && cstr_equals_bounded(strtab, strsize, strx, "_fg_get_vnode")) {
            out.fgGetVnode = val;
        } else if (!out.macFileCheckMmap && cstr_equals_bounded(strtab, strsize, strx, "_mac_file_check_mmap")) {
            out.macFileCheckMmap = val;
        } else if (!out.mpRendezvousNoIntrs && cstr_equals_bounded(strtab, strsize, strx, "_mp_rendezvous_no_intrs")) {
            out.mpRendezvousNoIntrs = val;
        }
    }
}

struct ResolvedTargets {
    uint64_t slide               { 0 };
    uint64_t procBestName        { 0 };
    uint64_t fgGetVnode          { 0 };
    uint64_t macFileCheckMmap    { 0 };
    uint64_t mpRendezvousNoIntrs { 0 };
};

// Cross-check dual KPI anchors (&current_proc, &vn_getpath) to derive the exact
// __TEXT_EXEC slide (KASLR + BootKC delta) with zero hardcoded offsets.
inline HookStatus resolve_runtime_targets(const DiskSymbols &disk,
                                          uint64_t runtimeCurrentProc,
                                          uint64_t runtimeVnGetpath,
                                          ResolvedTargets &out) {
    out = ResolvedTargets {};
    if (!has_required_symbols(disk)) {
        return HookStatus::kSymbolsMissing;
    }
    if (!is_kernel_text_addr(runtimeCurrentProc) || !is_kernel_text_addr(runtimeVnGetpath)) {
        return HookStatus::kBadTargetAddr;
    }
    const uint64_t slide1 = runtimeCurrentProc - disk.currentProc;
    const uint64_t slide2 = runtimeVnGetpath - disk.vnGetpath;
    if (slide1 != slide2 || (slide1 & kPageMask) != 0ULL) {
        return HookStatus::kSlideMismatch;
    }

    const uint64_t procBest = disk.procBestName + slide1;
    const uint64_t fgVnode  = disk.fgGetVnode + slide1;
    const uint64_t mmapFn   = disk.macFileCheckMmap + slide1;
    const uint64_t mpRend   = disk.mpRendezvousNoIntrs ? (disk.mpRendezvousNoIntrs + slide1) : 0ULL;

    if (!is_kernel_text_addr(procBest) || !is_kernel_text_addr(fgVnode) ||
        !is_kernel_text_addr(mmapFn)) {
        return HookStatus::kBadTargetAddr;
    }
    if (mpRend != 0ULL && !is_kernel_text_addr(mpRend)) {
        return HookStatus::kBadTargetAddr;
    }

    out.slide               = slide1;
    out.procBestName        = procBest;
    out.fgGetVnode          = fgVnode;
    out.macFileCheckMmap    = mmapFn;
    out.mpRendezvousNoIntrs = mpRend;
    return HookStatus::kInstalled;
}

struct DecodedPrologue {
    size_t  len { 0 };
    uint8_t bytes[kMaxPrologueBytes] {};
};

// Decode one safe, position-independent ModRM instruction starting at `opPos` (opcode byte).
// Explicitly rejects mod==00 (so RIP-relative [rip+disp32] is impossible).
inline size_t decode_safe_modrm_insn(const uint8_t *code, size_t avail,
                                     size_t startPos, size_t opPos, bool allowRegReg) {
    if (opPos + 1u >= avail) return 0u;
    const uint8_t modrm = code[opPos + 1u];
    const uint8_t mod   = (modrm >> 6) & 0x3u;
    const uint8_t rm    = modrm & 0x7u;
    size_t end = opPos + 2u;

    if (mod == 3u) {
        return allowRegReg ? (end - startPos) : 0u;
    }
    if (mod == 0u) {
        return 0u; // reject all mod==00 including RIP-relative [rip+disp32]
    }
    if (rm == 4u) {
        // SIB byte present
        if (end >= avail) return 0u;
        ++end;
    }
    const size_t disp = (mod == 1u) ? 1u : 4u;
    if (end + disp > avail) return 0u;
    end += disp;
    return end - startPos;
}

// Decode one whitelisted position-independent x86_64 prologue instruction at `pos`.
// Returns instruction length in bytes, or 0 to fail closed on any unsafe/unknown opcode.
inline size_t decode_safe_prologue_insn(const uint8_t *code, size_t avail, size_t pos) {
    if (pos >= avail) return 0u;
    const uint8_t b0 = code[pos];

    // 1-byte NOP
    if (b0 == 0x90u) return 1u;

    // 2-byte NOP: 66 90
    if (b0 == 0x66u && pos + 1u < avail && code[pos + 1u] == 0x90u) return 2u;

    // Multi-byte NOP: 0F 1F ...
    if (b0 == 0x0Fu && pos + 2u < avail && code[pos + 1u] == 0x1Fu) {
        const uint8_t m = code[pos + 2u];
        if (m == 0x00u) return 3u;
        if (m == 0x40u && pos + 3u < avail) return 4u;
        if (m == 0x44u && pos + 4u < avail && code[pos + 3u] == 0x00u) return 5u;
        return 0u;
    }

    // push r64 (50..57)
    if (b0 >= 0x50u && b0 <= 0x57u) return 1u;

    // 32-bit mov r/m32, r32 (89) or xor r/m32, r32 (31/33)
    if (b0 == 0x89u) {
        return decode_safe_modrm_insn(code, avail, pos, pos, true);
    }
    if (b0 == 0x31u || b0 == 0x33u) {
        if (pos + 1u >= avail) return 0u;
        return ((code[pos + 1u] & 0xC0u) == 0xC0u) ? 2u : 0u;
    }

    // REX prefix (40..4F)
    if (b0 >= 0x40u && b0 <= 0x4Fu) {
        if (pos + 1u >= avail) return 0u;
        const uint8_t b1 = code[pos + 1u];

        // REX.B push r8..r15: 41 50..57
        if (b0 == 0x41u && b1 >= 0x50u && b1 <= 0x57u) return 2u;

        // sub/and imm8, rsp: 48 83 EC/E4 imm8
        if (b0 == 0x48u && b1 == 0x83u && pos + 3u < avail &&
            (code[pos + 2u] == 0xECu || code[pos + 2u] == 0xE4u)) {
            return 4u;
        }
        // sub imm32, rsp: 48 81 EC imm32
        if (b0 == 0x48u && b1 == 0x81u && pos + 6u < avail && code[pos + 2u] == 0xECu) {
            return 7u;
        }
        // REX mov (89 / 8B)
        if (b1 == 0x89u || b1 == 0x8Bu) {
            return decode_safe_modrm_insn(code, avail, pos, pos + 1u, true);
        }
        // REX lea (8D) — memory operand only (mod 1/2, never mod 0 RIP-rel)
        if (b1 == 0x8Du) {
            return decode_safe_modrm_insn(code, avail, pos, pos + 1u, false);
        }
        // REX xor reg, reg (31 / 33)
        if (b1 == 0x31u || b1 == 0x33u) {
            if (pos + 2u >= avail) return 0u;
            return ((code[pos + 2u] & 0xC0u) == 0xC0u) ? 3u : 0u;
        }
        return 0u;
    }

    return 0u;
}

// Decode a relocatable SysV x86_64 frame-pointer prologue covering at least
// kMinDetourBytes (14 B) and at most kMaxPrologueBytes (32 B). Fail-closed.
inline bool decode_safe_prologue(const uint8_t *code, size_t avail, DecodedPrologue &out) {
    out = DecodedPrologue {};
    if (!code || avail < kMinDetourBytes) return false;
    const size_t limit = avail < kMaxPrologueBytes ? avail : kMaxPrologueBytes;

    size_t pos = 0u;
    // Optional endbr64 (F3 0F 1E FA)
    if (limit >= 4u && code[0] == 0xF3u && code[1] == 0x0Fu &&
        code[2] == 0x1Eu && code[3] == 0xFAu) {
        pos = 4u;
    }

    // Require standard frame-pointer entry: push rbp (55); mov rbp, rsp (48 89 E5 or 48 8B EC)
    if (pos + 4u > limit) return false;
    if (code[pos] != 0x55u) return false;
    const bool movRbpRsp =
        (code[pos + 1u] == 0x48u && code[pos + 2u] == 0x89u && code[pos + 3u] == 0xE5u) ||
        (code[pos + 1u] == 0x48u && code[pos + 2u] == 0x8Bu && code[pos + 3u] == 0xECu);
    if (!movRbpRsp) return false;
    pos += 4u;

    while (pos < kMinDetourBytes) {
        const size_t insnLen = decode_safe_prologue_insn(code, limit, pos);
        if (insnLen == 0u || pos + insnLen > limit) return false;
        pos += insnLen;
    }
    if (pos > kMaxPrologueBytes) return false;

    out.len = pos;
    memcpy(out.bytes, code, pos);
    return true;
}

inline void encode_abs_jmp14(uint8_t out[kMinDetourBytes], uint64_t target) {
    out[0] = 0xFFu;
    out[1] = 0x25u;
    out[2] = 0x00u;
    out[3] = 0x00u;
    out[4] = 0x00u;
    out[5] = 0x00u;
    write_le64(out + 6u, target);
}

inline bool encode_detour(uint8_t *out, size_t outCap, size_t prologueLen, uint64_t hookTarget) {
    if (!out || prologueLen < kMinDetourBytes || prologueLen > kMaxPrologueBytes ||
        prologueLen > outCap) {
        return false;
    }
    encode_abs_jmp14(out, hookTarget);
    for (size_t i = kMinDetourBytes; i < prologueLen; ++i) {
        out[i] = 0x90u;
    }
    return true;
}

inline bool encode_trampoline(uint8_t *out, size_t outCap, const DecodedPrologue &p,
                              uint64_t resumeAddr, size_t &outLen) {
    outLen = 0u;
    if (!out || p.len < kMinDetourBytes || p.len > kMaxPrologueBytes) return false;
    const size_t need = p.len + kMinDetourBytes;
    if (need > outCap) return false;
    memcpy(out, p.bytes, p.len);
    encode_abs_jmp14(out + p.len, resumeAddr);
    outLen = need;
    return true;
}

inline bool has_prefix(const char *s, const char *prefix) {
    if (!s || !prefix) return false;
    while (*prefix != '\0') {
        if (*s != *prefix) return false;
        ++s;
        ++prefix;
    }
    return true;
}

inline bool is_webkit_proc_name(const char *procName) {
    return has_prefix(procName, kWebKitProcPrefix);
}

inline bool is_garnet_bundle_path(const char *path) {
    if (!has_prefix(path, kGarnetBundlePrefix)) return false;
    const size_t prefLen = sizeof(kGarnetBundlePrefix) - 1u;
    const char *rel = path + prefLen;
    if (rel[0] == '\0') return false; // must target a file inside the bundle

    // Reject control characters and any ".." path traversal component.
    size_t segStart = 0u;
    for (size_t i = 0u; ; ++i) {
        const unsigned char ch = static_cast<unsigned char>(rel[i]);
        if (ch == '\0' || ch == '/') {
            const size_t segLen = i - segStart;
            if (segLen == 2u && rel[segStart] == '.' && rel[segStart + 1u] == '.') {
                return false;
            }
            if (ch == '\0') break;
            segStart = i + 1u;
        } else if (ch < 0x20u || ch == 0x7Fu) {
            return false;
        }
    }
    return true;
}

// Fail-closed runtime policy for _mac_file_check_mmap.
inline bool should_allow_webkit_mmap(int origRet, bool hasMaxProt, int prot,
                                     const char *procName, bool isRegularFile,
                                     bool mountNoExec, const char *path) {
    if (origRet == 0) return false;                       // fast path: already allowed
    if (!hasMaxProt) return false;
    if ((prot & kVmProtExecute) == 0) return false;       // only executable mapping denials
    if (!is_webkit_proc_name(procName)) return false;     // com.apple.WebKit* only
    if (!isRegularFile || mountNoExec) return false;      // regular file on exec-capable mount
    if (!is_garnet_bundle_path(path)) return false;       // GarnetMetal.bundle subtree only
    return true;
}

} // namespace garnet_webkit
