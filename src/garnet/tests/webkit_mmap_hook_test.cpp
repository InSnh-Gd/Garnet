//
//  webkit_mmap_hook_test.cpp — Host ASan+UBSan unit tests for Garnet's
//  version-independent _mac_file_check_mmap hook (Option B).
//
#include "webkit_mmap_hook_pure.h"

#include <cstdio>
#include <cstring>
#include <vector>

using namespace garnet_webkit;

static int gPassed = 0;
static int gFailed = 0;

static void check(const char *name, bool cond) {
    if (cond) {
        ++gPassed;
    } else {
        ++gFailed;
        std::printf("FAIL: %s\n", name);
    }
}

static void push_le32(std::vector<uint8_t> &v, uint32_t x) {
    for (int i = 0; i < 4; ++i) v.push_back(static_cast<uint8_t>((x >> (i * 8)) & 0xFFu));
}

static void push_le64(std::vector<uint8_t> &v, uint64_t x) {
    for (int i = 0; i < 8; ++i) v.push_back(static_cast<uint8_t>((x >> (i * 8)) & 0xFFu));
}

static std::vector<uint8_t> build_synthetic_kernel_macho(bool includeAllSyms, bool includeMpRend) {
    struct SymSpec { const char *name; uint64_t addr; uint8_t ntype; };
    std::vector<SymSpec> syms;
    syms.push_back({ "_unrelated_debug", 0xFFFFFF8000100000ULL, 0x24u }); // STAB: must ignore
    syms.push_back({ "_current_proc",    0xFFFFFF8000210040ULL, 0x0Fu });
    syms.push_back({ "_vn_getpath",      0xFFFFFF8000320180ULL, 0x0Fu });
    syms.push_back({ "_proc_best_name",  0xFFFFFF8000430220ULL, 0x0Eu });
    syms.push_back({ "_fg_get_vnode",    0xFFFFFF8000540300ULL, 0x0Eu });
    if (includeAllSyms) {
        syms.push_back({ "_mac_file_check_mmap", 0xFFFFFF8000650440ULL, 0x0Fu });
    }
    if (includeMpRend) {
        syms.push_back({ "_mp_rendezvous_no_intrs", 0xFFFFFF8000112200ULL, 0x0Fu });
    }
    syms.push_back({ "_mac_file_check_mmap_downgrade", 0xFFFFFF8000650990ULL, 0x0Fu }); // prefix collision guard

    std::vector<uint8_t> strtab;
    strtab.push_back(0);
    std::vector<uint32_t> strOffs;
    for (const auto &s : syms) {
        strOffs.push_back(static_cast<uint32_t>(strtab.size()));
        const size_t n = std::strlen(s.name);
        for (size_t i = 0; i <= n; ++i) strtab.push_back(static_cast<uint8_t>(s.name[i]));
    }

    std::vector<uint8_t> symtab;
    for (size_t i = 0; i < syms.size(); ++i) {
        push_le32(symtab, strOffs[i]);
        symtab.push_back(syms[i].ntype);
        symtab.push_back(1u); // n_sect
        symtab.push_back(0u);
        symtab.push_back(0u);
        push_le64(symtab, syms[i].addr);
    }

    // mach_header_64 (32 B) + dummy LC_UUID (24 B) + LC_SYMTAB (24 B) = 80 B
    const uint32_t symoff = 0x100u;
    const uint32_t stroff = symoff + static_cast<uint32_t>(symtab.size());
    const size_t totalSize = stroff + strtab.size();

    std::vector<uint8_t> file(totalSize, 0u);
    auto set32 = [&](size_t off, uint32_t v) {
        for (int i = 0; i < 4; ++i) file[off + i] = static_cast<uint8_t>((v >> (i * 8)) & 0xFFu);
    };
    set32(0,  0xFEEDFACFu); // MH_MAGIC_64
    set32(4,  0x01000007u); // CPU_TYPE_X86_64
    set32(8,  3u);          // CPU_SUBTYPE_X86_64_ALL
    set32(12, 2u);          // MH_EXECUTE
    set32(16, 2u);          // ncmds
    set32(20, 48u);         // sizeofcmds (24 + 24)

    // LC_UUID at +32
    set32(32, 0x1Bu);
    set32(36, 24u);

    // LC_SYMTAB at +56
    set32(56, 0x2u);
    set32(60, 24u);
    set32(64, symoff);
    set32(68, static_cast<uint32_t>(syms.size()));
    set32(72, stroff);
    set32(76, static_cast<uint32_t>(strtab.size()));

    std::memcpy(file.data() + symoff, symtab.data(), symtab.size());
    std::memcpy(file.data() + stroff, strtab.data(), strtab.size());
    return file;
}

static void test_boot_arg() {
    check("boot-arg absent => default ON", enabled_by_boot_arg(false, 0u));
    check("boot-arg =1 => ON", enabled_by_boot_arg(true, 1u));
    check("boot-arg =0 => kill-switch OFF", !enabled_by_boot_arg(true, 0u));
}

static void test_macho_and_dual_anchor_slide() {
    const auto img = build_synthetic_kernel_macho(true, true);
    SymtabLayout lay {};
    check("parse_macho64_symtab succeeds",
          parse_macho64_symtab(img.data(), img.size(), img.size(), lay));
    check("symtab nsyms matches", lay.nsyms == 8u);

    DiskSymbols syms {};
    // Feed in 32-byte chunks (2 entries at a time) to verify chunked scanning
    const uint8_t *symBase = img.data() + lay.symoff;
    const size_t symBytes = static_cast<size_t>(lay.nsyms) * 16u;
    const char *strBase = reinterpret_cast<const char *>(img.data() + lay.stroff);
    for (size_t off = 0; off < symBytes; off += 32u) {
        const size_t n = (symBytes - off < 32u) ? (symBytes - off) : 32u;
        scan_nlist64_chunk(symBase + off, n, strBase, lay.strsize, syms);
    }
    check("all required disk symbols resolved", has_required_symbols(syms));
    check("exact _mac_file_check_mmap not shadowed by _downgrade",
          syms.macFileCheckMmap == 0xFFFFFF8000650440ULL);
    check("optional _mp_rendezvous_no_intrs resolved",
          syms.mpRendezvousNoIntrs == 0xFFFFFF8000112200ULL);

    // Dual-anchor slide (simulate BootKC +0xe8000 + KASLR +0x12340000 = +0x12428000)
    const uint64_t slide = 0x12428000ULL;
    ResolvedTargets tgt {};
    check("dual-anchor slide succeeds",
          resolve_runtime_targets(syms, syms.currentProc + slide, syms.vnGetpath + slide, tgt)
              == HookStatus::kInstalled);
    check("resolved slide matches", tgt.slide == slide);
    check("resolved macFileCheckMmap matches",
          tgt.macFileCheckMmap == syms.macFileCheckMmap + slide);
    check("resolved fgGetVnode matches",
          tgt.fgGetVnode == syms.fgGetVnode + slide);
    check("resolved procBestName matches",
          tgt.procBestName == syms.procBestName + slide);

    // Mismatched second anchor fails closed
    check("mismatched dual anchor fails closed",
          resolve_runtime_targets(syms, syms.currentProc + slide, syms.vnGetpath + slide + 0x1000ULL, tgt)
              == HookStatus::kSlideMismatch);
    // Unaligned slide fails closed
    check("unaligned slide fails closed",
          resolve_runtime_targets(syms, syms.currentProc + slide + 0x10ULL, syms.vnGetpath + slide + 0x10ULL, tgt)
              == HookStatus::kSlideMismatch);
    // Non-canonical runtime anchor fails closed
    check("non-canonical anchor fails closed",
          resolve_runtime_targets(syms, 0x00007FFF00001000ULL, 0x00007FFF00002000ULL, tgt)
              == HookStatus::kBadTargetAddr);

    // Missing required symbol fails closed
    const auto missing = build_synthetic_kernel_macho(false, false);
    SymtabLayout lay2 {};
    check("missing-sym image parses header",
          parse_macho64_symtab(missing.data(), missing.size(), missing.size(), lay2));
    DiskSymbols syms2 {};
    scan_nlist64_chunk(missing.data() + lay2.symoff, lay2.nsyms * 16u,
                       reinterpret_cast<const char *>(missing.data() + lay2.stroff),
                       lay2.strsize, syms2);
    check("missing _mac_file_check_mmap fails closed",
          resolve_runtime_targets(syms2, 0xFFFFFF8010210040ULL, 0xFFFFFF8010320180ULL, tgt)
              == HookStatus::kSymbolsMissing);

    // Corrupted Mach-O magic fails closed
    auto badMagic = img;
    badMagic[0] ^= 0xFFu;
    check("bad Mach-O magic fails closed",
          !parse_macho64_symtab(badMagic.data(), badMagic.size(), badMagic.size(), lay2));
}

static void test_prologue_decoder_and_encoding() {
    // 1. Live macOS 26.7.2 (25G332) _mac_file_check_mmap 17-byte prologue
    const uint8_t live25g332[] = {
        0x55,                               // push rbp
        0x48, 0x89, 0xE5,                   // mov rbp, rsp
        0x41, 0x57,                         // push r15
        0x41, 0x56,                         // push r14
        0x41, 0x55,                         // push r13
        0x41, 0x54,                         // push r12
        0x53,                               // push rbx
        0x48, 0x83, 0xEC, 0x28,             // sub $0x28, rsp  (total 17 B)
        0x49, 0x89, 0xF6,                   // mov rsi, r14 (next insn, should not be stolen)
    };
    DecodedPrologue p {};
    check("25G332 live prologue decodes", decode_safe_prologue(live25g332, sizeof(live25g332), p));
    check("25G332 prologue exact boundary = 17 B", p.len == 17u);
    check("25G332 stolen bytes preserved", std::memcmp(p.bytes, live25g332, 17u) == 0);

    uint8_t detour[kMaxPrologueBytes] {};
    uint8_t tramp[kTrampolineCapBytes] {};
    size_t trampLen = 0u;
    const uint64_t hookAddr   = 0xFFFFFF7F91234560ULL;
    const uint64_t resumeAddr = 0xFFFFFF8000650440ULL + p.len;
    check("encode_detour succeeds", encode_detour(detour, sizeof(detour), p.len, hookAddr));
    check("detour jmp opcode FF 25 00 00 00 00",
          detour[0] == 0xFFu && detour[1] == 0x25u &&
          detour[2] == 0x00u && detour[3] == 0x00u &&
          detour[4] == 0x00u && detour[5] == 0x00u);
    check("detour target address encoded", read_le64(detour + 6u) == hookAddr);
    check("detour trailing padding is NOPs",
          detour[14] == 0x90u && detour[15] == 0x90u && detour[16] == 0x90u);

    check("encode_trampoline succeeds",
          encode_trampoline(tramp, sizeof(tramp), p, resumeAddr, trampLen));
    check("trampoline length = 17 + 14 = 31", trampLen == 31u);
    check("trampoline prefix matches stolen prologue", std::memcmp(tramp, live25g332, 17u) == 0);
    check("trampoline jump target matches resumeAddr", read_le64(tramp + 17u + 6u) == resumeAddr);

    // 2. Future compiler variant: endbr64 + fewer pushes + sub imm32, rsp + mov reg, [rbp-0x30]
    const uint8_t future1[] = {
        0xF3, 0x0F, 0x1E, 0xFA,             // endbr64 (4)
        0x55,                               // push rbp (5)
        0x48, 0x89, 0xE5,                   // mov rbp, rsp (8)
        0x41, 0x57,                         // push r15 (10)
        0x48, 0x81, 0xEC, 0x20, 0x01, 0x00, 0x00, // sub $0x120, rsp (17)
        0x48, 0x89, 0x7D, 0xD0,             // mov rdi, [rbp-0x30]
    };
    check("future endbr64 + sub imm32 prologue decodes",
          decode_safe_prologue(future1, sizeof(future1), p) && p.len == 17u);

    // 3. Future variant with reg-to-reg moves & stack save
    const uint8_t future2[] = {
        0x55,                               // push rbp (1)
        0x48, 0x89, 0xE5,                   // mov rbp, rsp (4)
        0x41, 0x56,                         // push r14 (6)
        0x53,                               // push rbx (7)
        0x48, 0x83, 0xEC, 0x10,             // sub $0x10, rsp (11)
        0x49, 0x89, 0xFE,                   // mov rdi, r14 (14)
    };
    check("future 14-byte exact prologue decodes",
          decode_safe_prologue(future2, sizeof(future2), p) && p.len == 14u);

    // 4. Reject RIP-relative instruction inside first 14 bytes (e.g. mov rax, [rip+disp32]: 48 8B 05 xx xx xx xx)
    const uint8_t badRipRel[] = {
        0x55, 0x48, 0x89, 0xE5,
        0x41, 0x57,
        0x48, 0x8B, 0x05, 0x10, 0x20, 0x00, 0x00, // RIP-relative load!
        0x53, 0x90, 0x90
    };
    check("RIP-relative load in prologue rejected (fail-closed)",
          !decode_safe_prologue(badRipRel, sizeof(badRipRel), p));

    // 5. Reject CALL inside first 14 bytes
    const uint8_t badCall[] = {
        0x55, 0x48, 0x89, 0xE5,
        0x41, 0x57, 0x41, 0x56,
        0xE8, 0x00, 0x00, 0x00, 0x00,
        0x53, 0x90
    };
    check("CALL in prologue rejected (fail-closed)",
          !decode_safe_prologue(badCall, sizeof(badCall), p));

    // 6. Reject missing frame-pointer prologue
    const uint8_t badNoFrame[] = {
        0x41, 0x57, 0x41, 0x56, 0x41, 0x55, 0x41, 0x54,
        0x53, 0x48, 0x83, 0xEC, 0x28, 0x90, 0x90, 0x90
    };
    check("non-frame-pointer prologue rejected",
          !decode_safe_prologue(badNoFrame, sizeof(badNoFrame), p));
}

static void test_runtime_policy() {
    const char *goodPath = "/Library/GPUBundles/GarnetMetal.bundle/Contents/MacOS/GarnetMetal";
    const char *goodDylib = "/Library/GPUBundles/GarnetMetal.bundle/Contents/Resources/libvulkan_radeon.dylib";

    // Positive cases
    check("WebKit.GPU + GarnetMetal + EPERM + PROT_EXEC => ALLOW",
          should_allow_webkit_mmap(1, true, 0x5, "com.apple.WebKit.GPU", true, false, goodPath));
    check("WebKit.WebContent + libvulkan_radeon + EPERM + PROT_EXEC => ALLOW",
          should_allow_webkit_mmap(1, true, 0x4, "com.apple.WebKit.WebContent", true, false, goodDylib));

    // Fast path: origRet == 0 must NEVER trigger override
    check("origRet == 0 fast path returns false",
          !should_allow_webkit_mmap(0, true, 0x5, "com.apple.WebKit.GPU", true, false, goodPath));

    // Negative security boundaries
    check("missing maxprot pointer refused",
          !should_allow_webkit_mmap(1, false, 0x5, "com.apple.WebKit.GPU", true, false, goodPath));
    check("non-exec prot refused",
          !should_allow_webkit_mmap(1, true, 0x1, "com.apple.WebKit.GPU", true, false, goodPath));
    check("non-WebKit process refused",
          !should_allow_webkit_mmap(1, true, 0x5, "com.apple.Safari", true, false, goodPath));
    check("null process name refused",
          !should_allow_webkit_mmap(1, true, 0x5, nullptr, true, false, goodPath));
    check("non-regular file refused",
          !should_allow_webkit_mmap(1, true, 0x5, "com.apple.WebKit.GPU", false, false, goodPath));
    check("MNT_NOEXEC mount refused",
          !should_allow_webkit_mmap(1, true, 0x5, "com.apple.WebKit.GPU", true, true, goodPath));
    check("other GPUBundle refused",
          !should_allow_webkit_mmap(1, true, 0x5, "com.apple.WebKit.GPU", true, false,
                                    "/Library/GPUBundles/Evil.bundle/Contents/MacOS/Evil"));
    check("bundle root dir itself refused",
          !should_allow_webkit_mmap(1, true, 0x5, "com.apple.WebKit.GPU", true, false,
                                    "/Library/GPUBundles/GarnetMetal.bundle/"));
    check("path traversal /../ refused",
          !should_allow_webkit_mmap(1, true, 0x5, "com.apple.WebKit.GPU", true, false,
                                    "/Library/GPUBundles/GarnetMetal.bundle/../Evil.dylib"));
    check("path traversal trailing /.. refused",
          !should_allow_webkit_mmap(1, true, 0x5, "com.apple.WebKit.GPU", true, false,
                                    "/Library/GPUBundles/GarnetMetal.bundle/Contents/.."));
    check("control char in path refused",
          !should_allow_webkit_mmap(1, true, 0x5, "com.apple.WebKit.GPU", true, false,
                                    "/Library/GPUBundles/GarnetMetal.bundle/a\nb"));
}

int main() {
    test_boot_arg();
    test_macho_and_dual_anchor_slide();
    test_prologue_decoder_and_encoding();
    test_runtime_policy();
    std::printf("webkit_mmap_hook_test: %d passed, %d failed\n", gPassed, gFailed);
    return gFailed ? 1 : 0;
}
