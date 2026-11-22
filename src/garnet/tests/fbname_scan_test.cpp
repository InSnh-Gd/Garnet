// fbname_scan_test.cpp — prove the runtime class rename REFUSES rather than writes whenever the
// OSMetaClass layout is not exactly what it expects (0.0.308).
//
// Build and run on the host Mac (no kernel headers, no hardware, no PC):
//     clang++ -std=c++17 -Wall -Wextra -Werror -O1 -fsanitize=address,undefined \
//         -I src/garnet/src/dcn \
//         src/garnet/tests/fbname_scan_test.cpp -o /tmp/fbnametest && /tmp/fbnametest .
//
// It compiles the SAME header the kext compiles. The property under test is the one that makes a
// live-kernel pointer write safe to attempt at all: a wrong offset must be a REFUSAL, never a write.

#include <cstdio>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>

#include "fbname_scan.h"
#include "fbname.hpp"

static int checks, failures;
#define CHECK(cond, ...) do { checks++; if (!(cond)) { failures++; printf("FAIL %s:%d: ", __FILE__, __LINE__); \
    printf(__VA_ARGS__); printf("\n"); } } while (0)

// A plausible kernel pointer for the tests: canonical high-half, 8-byte aligned.
static uintptr_t kp(uintptr_t low) { return 0xffff000000000000ull | (low & ~0x7ull); }

static std::string slurp(const std::string &path)
{
    std::ifstream f(path, std::ios::binary);
    std::stringstream s;
    s << f.rdbuf();
    return s.str();
}

int main(int argc, char **argv)
{
    const std::string root = argc > 1 ? argv[1] : ".";
    const uintptr_t SYM   = kp(0x1234568);     // the interned OSSymbol for "RDNA4FB"
    const uintptr_t SUPER = kp(0x2345678);     // the superclass OSMetaClass
    const uintptr_t VTBL  = kp(0x3456788);

    // ---- 1. the layout the SDK header describes: className is word 3 ----
    {
        uintptr_t w[RDNA4_FBNAME_SCAN_WORDS] = { VTBL, 0, SUPER, SYM, 0x30, 0, 0, 0 };
        int idx = rdna4_fbname_find_unique(w, RDNA4_FBNAME_SCAN_WORDS, SYM);
        CHECK(idx == 3, "className found at word %d, expected 3", idx);
        CHECK(rdna4_fbname_layout_ok(w, RDNA4_FBNAME_SCAN_WORDS, idx), "layout accepted");
        CHECK(w[idx - 1] == SUPER, "the word before className is superClassLink");
    }

    // ---- 2. a DIFFERENT layout is still found, because the offset is searched not assumed ----
    {
        uintptr_t w[RDNA4_FBNAME_SCAN_WORDS] = { VTBL, SUPER, SYM, 0, 0, 0, 0, 0 };
        int idx = rdna4_fbname_find_unique(w, RDNA4_FBNAME_SCAN_WORDS, SYM);
        CHECK(idx == 2, "a kernel that orders the fields differently is still handled (idx %d)", idx);
        CHECK(rdna4_fbname_layout_ok(w, RDNA4_FBNAME_SCAN_WORDS, idx), "and its layout is accepted");
    }

    // ---- 3. THE LOAD-BEARING REFUSALS ----
    {
        // absent: the symbol is not in the structure at all
        uintptr_t w[RDNA4_FBNAME_SCAN_WORDS] = { VTBL, 0, SUPER, kp(0x999), 0, 0, 0, 0 };
        CHECK(rdna4_fbname_find_unique(w, RDNA4_FBNAME_SCAN_WORDS, SYM) == RDNA4_FBNAME_FOUND_NONE,
              "a layout with no matching word REFUSES");
    }
    {
        // ambiguous: two words hold it, so which one is className is not decidable
        uintptr_t w[RDNA4_FBNAME_SCAN_WORDS] = { VTBL, SYM, SUPER, SYM, 0, 0, 0, 0 };
        CHECK(rdna4_fbname_find_unique(w, RDNA4_FBNAME_SCAN_WORDS, SYM) == RDNA4_FBNAME_FOUND_AMBIGUOUS,
              "two matching words REFUSE rather than pick one");
    }
    {
        // className appearing to be word 0 would mean we had found the vtable pointer
        uintptr_t w[RDNA4_FBNAME_SCAN_WORDS] = { SYM, 0, 0, 0, 0, 0, 0, 0 };
        int idx = rdna4_fbname_find_unique(w, RDNA4_FBNAME_SCAN_WORDS, SYM);
        CHECK(idx == 0, "word 0 is found...");
        CHECK(!rdna4_fbname_layout_ok(w, RDNA4_FBNAME_SCAN_WORDS, idx),
              "...but REFUSED, because word 0 is the vtable pointer, not className");
    }
    {
        // the word before className is not a pointer -> not superClassLink -> refuse
        uintptr_t w[RDNA4_FBNAME_SCAN_WORDS] = { VTBL, 0, 0x30, SYM, 0, 0, 0, 0 };
        int idx = rdna4_fbname_find_unique(w, RDNA4_FBNAME_SCAN_WORDS, SYM);
        CHECK(idx == 3, "found at 3");
        CHECK(!rdna4_fbname_layout_ok(w, RDNA4_FBNAME_SCAN_WORDS, idx),
              "but REFUSED: the preceding word is not a plausible superClassLink");
    }

    // ---- 4. argument guards, none of which may be a write ----
    {
        uintptr_t w[RDNA4_FBNAME_SCAN_WORDS] = { VTBL, 0, SUPER, SYM, 0, 0, 0, 0 };
        CHECK(rdna4_fbname_find_unique(nullptr, RDNA4_FBNAME_SCAN_WORDS, SYM) == RDNA4_FBNAME_BAD_ARG, "NULL words");
        CHECK(rdna4_fbname_find_unique(w, 0, SYM) == RDNA4_FBNAME_BAD_ARG, "zero count");
        CHECK(rdna4_fbname_find_unique(w, RDNA4_FBNAME_SCAN_WORDS + 1, SYM) == RDNA4_FBNAME_BAD_ARG,
              "a count past the scan window is refused");
        CHECK(rdna4_fbname_find_unique(w, RDNA4_FBNAME_SCAN_WORDS, 0) == RDNA4_FBNAME_BAD_ARG, "null target");
        CHECK(!rdna4_fbname_layout_ok(nullptr, RDNA4_FBNAME_SCAN_WORDS, 3), "NULL words in layout_ok");
        CHECK(!rdna4_fbname_layout_ok(w, RDNA4_FBNAME_SCAN_WORDS, -1), "negative index");
        CHECK(!rdna4_fbname_layout_ok(w, RDNA4_FBNAME_SCAN_WORDS, (int)RDNA4_FBNAME_SCAN_WORDS),
              "index at the end of the window");
    }

    // ---- 5. the pointer plausibility filter ----
    CHECK(rdna4_fbname_plausible_kptr(kp(0x1000)), "a canonical aligned kernel pointer is plausible");
    CHECK(!rdna4_fbname_plausible_kptr(0), "NULL is not");
    CHECK(!rdna4_fbname_plausible_kptr(kp(0x1000) | 4), "a misaligned pointer is not");
    CHECK(!rdna4_fbname_plausible_kptr(0x0000700000001000ull), "a userspace address is not");
    CHECK(!rdna4_fbname_plausible_kptr(0x30), "a small integer (a size field) is not");
    CHECK(!rdna4_fbname_plausible_kptr(0xfffe000000001000ull), "a non-canonical high half is not");

    // ---- 6. only the proven local leaf may be renamed ----
    CHECK(rdna4fbname::fbname_leaf_is_local("GarnetPrimaryFramebuffer"), "the local leaf is eligible");
    CHECK(rdna4fbname::fbname_leaf_is_local("AMDGarnetPrimaryFramebuffer"), "the already-renamed local leaf is still the local metaclass");
    CHECK(!rdna4fbname::fbname_leaf_is_local("RDNA4FB") && !rdna4fbname::fbname_leaf_is_local("AMDRDNA4FB"),
          "retired external RDNA4FB names are refused");
    CHECK(!rdna4fbname::fbname_leaf_is_local("IONDRVFramebuffer"), "the generic Apple framebuffer is refused");
    CHECK(!rdna4fbname::fbname_leaf_is_local("IOFramebuffer"), "the Apple base class is refused");
    CHECK(!rdna4fbname::fbname_leaf_is_local("AMDFramebuffer"), "an Apple AMD framebuffer is refused");
    CHECK(!rdna4fbname::fbname_leaf_is_local("GarnetPrimaryFramebuffer2") && !rdna4fbname::fbname_leaf_is_local("") &&
          !rdna4fbname::fbname_leaf_is_local(nullptr), "near-misses, empty names and NULL are refused");
    CHECK(rdna4fbname::fbname_leaf_is_apple_shared("IONDRVFramebuffer") &&
          rdna4fbname::fbname_leaf_is_apple_shared("IOFramebuffer") &&
          rdna4fbname::fbname_leaf_is_apple_shared("AMDFramebuffer"),
          "the generic/shared Apple classes are identified for the refusal log");
    CHECK(!rdna4fbname::fbname_leaf_is_apple_shared("GarnetPrimaryFramebuffer") &&
          !rdna4fbname::fbname_leaf_is_apple_shared("AMDGarnetPrimaryFramebuffer") &&
          !rdna4fbname::fbname_leaf_is_apple_shared(nullptr),
          "the local leaf is never classified as Apple-shared");

    // ---- 7. the live glue keeps the identity and exact-metaclass proofs ----
    const std::string src = slurp(root + "/src/garnet/src/dcn/fbname.cpp");
    CHECK(!src.empty(), "fbname.cpp is readable from the tree root");
    CHECK(src.find("#include \"amd/native_display_identity.h\"") != std::string::npos,
          "fbname obtains the verified target-GPU primary");
    CHECK(src.find("rdna4_display_primary(&fb)") != std::string::npos,
          "fbname no longer discovers a framebuffer by class-name matching");
    CHECK(src.find("fbname_leaf_is_local(leaf)") != std::string::npos &&
          src.find("FBN_UNPROVEN_LEAF") != std::string::npos,
          "generic/unproven leaf classes have an explicit refusal");
    CHECK(src.find("OSMetaClass::getMetaClassWithName(want)") != std::string::npos &&
          src.find("cmc != registered") != std::string::npos,
          "the live leaf must be backed by the registered local metaclass, not just its name");
    CHECK(src.find("serviceMatching(RDNA4_FBNAME") == std::string::npos &&
          src.find("find_framebuffer") == std::string::npos,
          "the old class-name lookup path is gone");
    // Order is the safety property: the verified primary comes first, the local-leaf refusal before any
    // OSSymbol work, and the actually registered metaclass is proven before the className word is written.
    const size_t lookup = src.find("IOService *fb = find_primary();");
    const size_t meta = src.find("const OSMetaClass *cmc = fb->getMetaClass();");
    const size_t leafref = src.find("fbname_leaf_is_local(leaf)");
    const size_t symbol = src.find("OSSymbol::withCString(wantStr)");
    const size_t write = src.find("reinterpret_cast<uintptr_t *>(mc)[idx] = reinterpret_cast<uintptr_t>(fresh);");
    CHECK(lookup != std::string::npos && meta != std::string::npos && leafref != std::string::npos &&
          symbol != std::string::npos && write != std::string::npos &&
          lookup < meta && meta < leafref && leafref < symbol && symbol < write,
          "order: verified primary -> live metaclass -> local-leaf refusal -> symbol search -> write");
    CHECK(src.find("restore_on_unload(void)") != std::string::npos &&
          src.find("(void)do_restore(\"kext unload\");") != std::string::npos,
          "the unload path still restores an armed rename");
    CHECK(src.find("\"IONDRV") == std::string::npos && src.find("\"AMDFramebuffer\"") == std::string::npos,
          "the glue never names an Apple class as a rename candidate");

    printf("fbname_scan_test: %d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
