// test-swizzle-placement.c: pure-C host unit tests + assembled GarnetDevice.m source pins
// for Task 2.1 (Texture Swizzle) and Task 2.2 (MTLHeapTypePlacement).
// Build/run:
//   cc -std=c11 -D_GNU_SOURCE -O2 -Wall -Wextra -Werror -Isrc/garnet-metal -o "$TMPDIR/test-swizzle-placement" src/garnet-metal/test-swizzle-placement.c
//   "$TMPDIR/test-swizzle-placement" [assembled-dir]
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "rdna4_swizzle.h"
#include "rdna4_heap_backing.h"

static int fails, checks;
#define CHECK(name, cond, ...) do { \
    int ok_ = (cond) ? 1 : 0; \
    checks++; \
    printf("%s %s: ", ok_ ? "ok  " : "FAIL", name); \
    printf(__VA_ARGS__); \
    printf("\n"); \
    if (!ok_) fails++; \
} while (0)

#define KIB (1ULL << 10)
#define MIB (1ULL << 20)

static char *slurp(const char *dir, const char *name) {
    char p[1024];
    int n = snprintf(p, sizeof p, "%s/%s", dir, name);
    if (n <= 0 || (size_t)n >= sizeof p) return NULL;
    FILE *f = fopen(p, "rb");
    if (!f) return NULL;
    if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return NULL; }
    long sz = ftell(f);
    if (sz < 0 || fseek(f, 0, SEEK_SET) != 0) { fclose(f); return NULL; }
    char *buf = (char *)calloc(1, (size_t)sz + 1);
    if (!buf) { fclose(f); return NULL; }
    if (fread(buf, 1, (size_t)sz, f) != (size_t)sz) { fclose(f); free(buf); return NULL; }
    fclose(f);
    return buf;
}

static void test_swizzle(void) {
    const rdna4swz_t id = rdna4swz_identity();
    const rdna4swz_t bgra = { RDNA4SWZ_BLUE, RDNA4SWZ_GREEN, RDNA4SWZ_RED, RDNA4SWZ_ALPHA };
    const rdna4swz_t consts = { RDNA4SWZ_ZERO, RDNA4SWZ_ONE, RDNA4SWZ_ALPHA, RDNA4SWZ_RED };
    const rdna4swz_t depth_rrr1 = { RDNA4SWZ_RED, RDNA4SWZ_RED, RDNA4SWZ_RED, RDNA4SWZ_ONE };
    const rdna4swz_t a8_aaa1 = { RDNA4SWZ_ALPHA, RDNA4SWZ_ALPHA, RDNA4SWZ_ALPHA, RDNA4SWZ_ONE };
    rdna4swz_t out;
    rdna4swz_vk_t vk;
    const char *why = NULL;

    // Basic validation & identity checks.
    CHECK("swz identity valid", rdna4swz_valid(id) && rdna4swz_is_identity(id), "RGBA");
    CHECK("swz bgra valid non-id", rdna4swz_valid(bgra) && !rdna4swz_is_identity(bgra), "BGRA");
    CHECK("swz out-of-range rejected",
          !rdna4swz_valid((rdna4swz_t){ 6, RDNA4SWZ_GREEN, RDNA4SWZ_BLUE, RDNA4SWZ_ALPHA }) &&
          !rdna4swz_valid((rdna4swz_t){ RDNA4SWZ_RED, 7, RDNA4SWZ_BLUE, RDNA4SWZ_ALPHA }) &&
          !rdna4swz_valid((rdna4swz_t){ RDNA4SWZ_RED, RDNA4SWZ_GREEN, 255, RDNA4SWZ_ALPHA }) &&
          !rdna4swz_valid((rdna4swz_t){ RDNA4SWZ_RED, RDNA4SWZ_GREEN, RDNA4SWZ_BLUE, 6 }),
          "channel > 5 rejected");

    // Apple Metal spec composition example: parent=[R,1,1,G], view=[R,G,A,B] -> [R,1,G,1].
    const rdna4swz_t apple_parent = { RDNA4SWZ_RED, RDNA4SWZ_ONE, RDNA4SWZ_ONE, RDNA4SWZ_GREEN };
    const rdna4swz_t apple_view   = { RDNA4SWZ_RED, RDNA4SWZ_GREEN, RDNA4SWZ_ALPHA, RDNA4SWZ_BLUE };
    CHECK("swz Apple doc compose example",
          rdna4swz_compose(apple_parent, apple_view, &out) &&
          out.r == RDNA4SWZ_RED && out.g == RDNA4SWZ_ONE && out.b == RDNA4SWZ_GREEN && out.a == RDNA4SWZ_ONE,
          "got (%u,%u,%u,%u)", out.r, out.g, out.b, out.a);

    // Double BGRA swap restores identity.
    CHECK("swz double BGRA restores identity",
          rdna4swz_compose(bgra, bgra, &out) && rdna4swz_is_identity(out),
          "got (%u,%u,%u,%u)", out.r, out.g, out.b, out.a);

    // Identity view preserves parent swizzle; Zero/One in view override parent channels.
    CHECK("swz identity view inherits parent",
          rdna4swz_compose(consts, id, &out) &&
          out.r == RDNA4SWZ_ZERO && out.g == RDNA4SWZ_ONE && out.b == RDNA4SWZ_ALPHA && out.a == RDNA4SWZ_RED,
          "got (%u,%u,%u,%u)", out.r, out.g, out.b, out.a);
    CHECK("swz view Zero/One override parent",
          rdna4swz_compose(bgra, (rdna4swz_t){ RDNA4SWZ_ZERO, RDNA4SWZ_RED, RDNA4SWZ_ONE, RDNA4SWZ_BLUE }, &out) &&
          out.r == RDNA4SWZ_ZERO && out.g == RDNA4SWZ_BLUE && out.b == RDNA4SWZ_ONE && out.a == RDNA4SWZ_RED,
          "got (%u,%u,%u,%u)", out.r, out.g, out.b, out.a);

    // Invalid inputs to compose/to_vk fail cleanly.
    CHECK("swz compose rejects bad input or NULL out",
          !rdna4swz_compose((rdna4swz_t){ 9, 2, 3, 4 }, id, &out) &&
          !rdna4swz_compose(id, (rdna4swz_t){ 2, 3, 4, 9 }, &out) &&
          !rdna4swz_compose(id, id, NULL),
          "refused");

    // Vulkan component mapping: ordinary, BGRA, Zero/One, depth, and A8Unorm (stored as R8_UNORM).
    CHECK("vk map identity RGBA/BGRA",
          rdna4swz_to_vk(0, id, &vk) && rdna4swz_vk_is_identity(vk) &&
          vk.r == RDNA4VKSWZ_R && vk.g == RDNA4VKSWZ_G && vk.b == RDNA4VKSWZ_B && vk.a == RDNA4VKSWZ_A,
          "RGBA -> identity");
    CHECK("vk map BGRA swap",
          rdna4swz_to_vk(0, bgra, &vk) && !rdna4swz_vk_is_identity(vk) &&
          vk.r == RDNA4VKSWZ_B && vk.g == RDNA4VKSWZ_G && vk.b == RDNA4VKSWZ_R && vk.a == RDNA4VKSWZ_A,
          "BGRA -> B,G,R,A");
    CHECK("vk map depth R,R,R,1",
          rdna4swz_to_vk(0, depth_rrr1, &vk) && !rdna4swz_vk_is_identity(vk) &&
          vk.r == RDNA4VKSWZ_R && vk.g == RDNA4VKSWZ_R && vk.b == RDNA4VKSWZ_R && vk.a == RDNA4VKSWZ_ONE,
          "D32F -> R,R,R,1");
    CHECK("vk map A8 identity -> (0,0,0,R)",
          rdna4swz_to_vk(RDNA4SWZ_FMT_A8, id, &vk) && !rdna4swz_vk_is_identity(vk) &&
          vk.r == RDNA4VKSWZ_ZERO && vk.g == RDNA4VKSWZ_ZERO && vk.b == RDNA4VKSWZ_ZERO && vk.a == RDNA4VKSWZ_R,
          "A8 identity");
    CHECK("vk map A8 (A,A,A,1) -> (R,R,R,1)",
          rdna4swz_to_vk(RDNA4SWZ_FMT_A8, a8_aaa1, &vk) &&
          vk.r == RDNA4VKSWZ_R && vk.g == RDNA4VKSWZ_R && vk.b == RDNA4VKSWZ_R && vk.a == RDNA4VKSWZ_ONE,
          "A8 composed");
    CHECK("vk map A8 composed view (R,0,G,A) over (A,A,A,1)",
          rdna4swz_compose(a8_aaa1, (rdna4swz_t){ RDNA4SWZ_RED, RDNA4SWZ_ZERO, RDNA4SWZ_GREEN, RDNA4SWZ_ALPHA }, &out) &&
          rdna4swz_to_vk(RDNA4SWZ_FMT_A8, out, &vk) &&
          vk.r == RDNA4VKSWZ_R && vk.g == RDNA4VKSWZ_ZERO && vk.b == RDNA4VKSWZ_R && vk.a == RDNA4VKSWZ_ONE,
          "got (%u,%u,%u,%u)", vk.r, vk.g, vk.b, vk.a);
    CHECK("vk map rejects bad enum",
          !rdna4swz_to_vk(0, (rdna4swz_t){ 0, 1, 2, 6 }, &vk) && !rdna4swz_to_vk(0, id, NULL),
          "refused");

    // Key packing distinguishes swizzles deterministically.
    CHECK("swz key distinct", rdna4swz_key(id) != rdna4swz_key(bgra) && rdna4swz_key(bgra) != rdna4swz_key(consts),
          "id=0x%08x bgra=0x%08x", rdna4swz_key(id), rdna4swz_key(bgra));

    // Usage validation: descriptor vs view rules.
    CHECK("swz identity allowed on all usages",
          rdna4swz_check(id, 0, 0, 0, &why) &&
          rdna4swz_check(id, RDNA4SWZ_USE_RT | RDNA4SWZ_USE_WRITE, 0, 0, &why) &&
          rdna4swz_check(id, RDNA4SWZ_USE_WRITE, RDNA4SWZ_FMT_TEXEL, 0, &why),
          "identity unrestricted");
    CHECK("swz descriptor non-id allowed for ShaderRead (+PFV)",
          rdna4swz_check(bgra, RDNA4SWZ_USE_READ, 0, 0, &why) &&
          rdna4swz_check(consts, RDNA4SWZ_USE_READ | RDNA4SWZ_USE_PFV, 0, 0, &why),
          "read-only allowed");
    CHECK("swz descriptor non-id refused for RenderTarget",
          !rdna4swz_check(bgra, RDNA4SWZ_USE_RT, 0, 0, &why) && why != NULL &&
          !rdna4swz_check(bgra, RDNA4SWZ_USE_READ | RDNA4SWZ_USE_RT, 0, 0, &why),
          "why=%s", why ? why : "");
    CHECK("swz descriptor non-id refused for ShaderWrite",
          !rdna4swz_check(bgra, RDNA4SWZ_USE_WRITE, 0, 0, &why) && why != NULL &&
          !rdna4swz_check(bgra, RDNA4SWZ_USE_READ | RDNA4SWZ_USE_WRITE, 0, 0, &why),
          "why=%s", why ? why : "");
    CHECK("swz descriptor non-id refused for Unknown(0) and TextureBuffer",
          !rdna4swz_check(bgra, 0, 0, 0, &why) &&
          !rdna4swz_check(bgra, RDNA4SWZ_USE_READ, RDNA4SWZ_FMT_TEXEL, 0, &why),
          "refused");
    CHECK("swz view non-id allowed on RT|ShaderRead parent, refused on ShaderWrite or RT-only",
          rdna4swz_check(bgra, RDNA4SWZ_USE_READ | RDNA4SWZ_USE_RT | RDNA4SWZ_USE_PFV, 0, 1, &why) &&
          !rdna4swz_check(bgra, RDNA4SWZ_USE_RT, 0, 1, &why) &&
          !rdna4swz_check(bgra, RDNA4SWZ_USE_READ | RDNA4SWZ_USE_WRITE, 0, 1, &why) &&
          !rdna4swz_check(bgra, RDNA4SWZ_USE_WRITE, 0, 1, &why) &&
          !rdna4swz_check(bgra, 0, 0, 1, &why),
          "view rules enforced");
    CHECK("swz check refuses bad enum even on view",
          !rdna4swz_check((rdna4swz_t){ 2, 3, 4, 6 }, RDNA4SWZ_USE_READ, 0, 1, &why) && why != NULL,
          "why=%s", why ? why : "");
}

static void test_placement_heap(void) {
    rdna4hb_heap h;
    rdna4hb_res r1 = {0}, r2 = {0}, r3 = {0}, r4 = {0};
    rdna4hb_reclaim gone = {0};
    uint64_t blk = 0, ext = 0;

    // Validation & overflow checks via rdna4hb_place_check.
    CHECK("place_check valid aligned boundaries",
          rdna4hb_place_check(64 * KIB, 0, 4096, 256, 4096, 0, &ext) == RDNA4HB_R_OK && ext == 4096 &&
          rdna4hb_place_check(64 * KIB, 60 * KIB, 100, 256, 4096, 0, &ext) == RDNA4HB_R_OK && ext == 4096,
          "extent=%llu", (unsigned long long)ext);
    CHECK("place_check rejects misaligned to alignment or granularity",
          rdna4hb_place_check(64 * KIB, 256, 100, 4096, 1, 0, &ext) == RDNA4HB_R_INVALID &&
          rdna4hb_place_check(64 * KIB, 256, 100, 256, 4096, 0, &ext) == RDNA4HB_R_INVALID &&
          rdna4hb_place_check(64 * KIB, 1, 100, 16, 16, 0, &ext) == RDNA4HB_R_INVALID,
          "misaligned refused");
    CHECK("place_check rejects zero/dedicated/non-pow2 gran",
          rdna4hb_place_check(0, 0, 4096, 256, 4096, 0, &ext) == RDNA4HB_R_INVALID &&
          rdna4hb_place_check(64 * KIB, 0, 0, 256, 4096, 0, &ext) == RDNA4HB_R_INVALID &&
          rdna4hb_place_check(64 * KIB, 0, 4096, 0, 4096, 0, &ext) == RDNA4HB_R_INVALID &&
          rdna4hb_place_check(64 * KIB, 0, 4096, 256, 3000, 0, &ext) == RDNA4HB_R_INVALID &&
          rdna4hb_place_check(64 * KIB, 0, 4096, 256, 4096, 1, &ext) == RDNA4HB_R_INVALID,
          "invalid params refused");
    CHECK("place_check rejects OOB and 64-bit wraparound overflow",
          rdna4hb_place_check(64 * KIB, 64 * KIB, 1, 1, 1, 0, &ext) == RDNA4HB_R_NO_SPACE &&
          rdna4hb_place_check(64 * KIB, 60 * KIB, 4097, 256, 4096, 0, &ext) == RDNA4HB_R_NO_SPACE &&
          rdna4hb_place_check(64 * KIB, UINT64_MAX & ~UINT64_C(4095), 4096, 4096, 4096, 0, &ext) == RDNA4HB_R_NO_SPACE &&
          rdna4hb_place_check(UINT64_MAX, UINT64_MAX & ~UINT64_C(4095), 4096, 4096, 4096, 0, &ext) == RDNA4HB_R_NO_SPACE &&
          rdna4hb_place_check(64 * KIB, 0, UINT64_MAX, 4096, 4096, 0, &ext) == RDNA4HB_R_INVALID,
          "OOB and overflow refused");

    // Cold placement heap accounting before first allocation.
    rdna4hb_init(&h, 256 * KIB, RDNA4HB_MAX_NORMAL);
    CHECK("cold placement heap used=0 maxAvail=size",
          rdna4hb_placement_used(&h) == 0 &&
          rdna4hb_placement_max_available(&h, 4096, 4096) == 256 * KIB &&
          rdna4hb_placement_max_available(&h, 0, 4096) == 0 &&
          rdna4hb_placement_max_available(&h, UINT64_MAX, 4096) == 0,
          "used=%llu max=%llu", (unsigned long long)rdna4hb_placement_used(&h),
          (unsigned long long)rdna4hb_placement_max_available(&h, 4096, 4096));

    // First placement asks for a single heap-sized contiguous block.
    int k = rdna4hb_place(&h, 0, 32 * KIB, 4096, 4096, 1u, 0, 0, 0, &r1);
    CHECK("first placement requests heap-sized block",
          k == RDNA4HB_R_NEED_BLOCK && r1.block_size == 256 * KIB && r1.offset == 0 && h.backing == 0,
          "k=%d block_size=%llu", k, (unsigned long long)r1.block_size);
    CHECK("add placement block",
          rdna4hb_add_placement_block(&h, r1.block_size, 0, 1, 0, (void *)0x1000, (void *)0x2000, &blk) &&
          h.backing == 256 * KIB,
          "blk=%llu backing=%llu", (unsigned long long)blk, (unsigned long long)h.backing);

    // Place disjoint resources at explicit offsets.
    CHECK("place A at [0, 32 KiB)",
          rdna4hb_place(&h, 0, 32 * KIB, 4096, 4096, 1u, 1, 0, 0, &r1) == RDNA4HB_R_OK &&
          r1.block == blk && r1.offset == 0 && r1.range_size == 32 * KIB &&
          rdna4hb_handle_open(&h, blk),
          "off=%llu lease=%llu", (unsigned long long)r1.offset, (unsigned long long)r1.lease);
    CHECK("place B at [128 KiB, 192 KiB)",
          rdna4hb_place(&h, 128 * KIB, 64 * KIB, 64 * KIB, 4096, 1u, 0, 0, 0, &r2) == RDNA4HB_R_OK &&
          r2.block == blk && r2.offset == 128 * KIB && r2.range_size == 64 * KIB &&
          rdna4hb_handle_open(&h, blk),
          "off=%llu lease=%llu", (unsigned long long)r2.offset, (unsigned long long)r2.lease);
    CHECK("disjoint accounting: used=96 KiB, maxAvail(4K)=96 KiB, maxAvail(64K)=64 KiB",
          rdna4hb_placement_used(&h) == 96 * KIB &&
          rdna4hb_placement_max_available(&h, 4096, 4096) == 96 * KIB &&
          rdna4hb_placement_max_available(&h, 64 * KIB, 4096) == 64 * KIB,
          "used=%llu max4K=%llu max64K=%llu",
          (unsigned long long)rdna4hb_placement_used(&h),
          (unsigned long long)rdna4hb_placement_max_available(&h, 4096, 4096),
          (unsigned long long)rdna4hb_placement_max_available(&h, 64 * KIB, 4096));

    // Explicit-offset aliasing: full overlap AND partial overlap MUST succeed on placement heap.
    CHECK("explicit full alias C at [0, 32 KiB)",
          rdna4hb_place(&h, 0, 32 * KIB, 4096, 4096, 1u, 1, 0, 0, &r3) == RDNA4HB_R_OK &&
          r3.block == blk && r3.offset == 0 && r3.lease != r1.lease &&
          rdna4hb_handle_open(&h, blk),
          "lease=%llu", (unsigned long long)r3.lease);
    CHECK("full alias does not double-count usedSize",
          rdna4hb_placement_used(&h) == 96 * KIB && h.backing == 256 * KIB,
          "used=%llu backing=%llu", (unsigned long long)rdna4hb_placement_used(&h), (unsigned long long)h.backing);
    CHECK("explicit partial overlap D at [16 KiB, 64 KiB)",
          rdna4hb_place(&h, 16 * KIB, 48 * KIB, 4096, 4096, 1u, 0, 0, 0, &r4) == RDNA4HB_R_OK &&
          r4.block == blk && r4.offset == 16 * KIB &&
          rdna4hb_handle_open(&h, blk),
          "off=%llu lease=%llu", (unsigned long long)r4.offset, (unsigned long long)r4.lease);
    CHECK("partial overlap merges intervals: used=128 KiB, maxAvail(4K)=64 KiB",
          rdna4hb_placement_used(&h) == 128 * KIB &&
          rdna4hb_placement_max_available(&h, 4096, 4096) == 64 * KIB,
          "used=%llu max4K=%llu",
          (unsigned long long)rdna4hb_placement_used(&h),
          (unsigned long long)rdna4hb_placement_max_available(&h, 4096, 4096));

    // Incompatible memoryTypeBits or host_import mismatch refuses without leaking capacity.
    CHECK("incompatible memoryTypeBits refused cleanly",
          rdna4hb_place(&h, 0, 4096, 4096, 4096, 2u, 0, 0, 0, &r1) == RDNA4HB_R_NO_SPACE &&
          rdna4hb_place(&h, 0, 4096, 4096, 4096, 1u, 1, 1, 0, &r1) == RDNA4HB_R_NO_SPACE,
          "backing=%llu", (unsigned long long)h.backing);

    // Re-read first lease id? Wait: r1 was overwritten by failed call above; save ids explicitly in fresh test below.
    // Close all 4 leases & handles, verify placement block stays alive until teardown (rdna4hb_take_block).
    for (size_t i = 0; i < h.nlease; i++) rdna4hb_release(&h, h.lease[i].id, 10);
    CHECK("makeAliasable drops placement usedSize immediately before reap",
          rdna4hb_placement_used(&h) == 0 &&
          rdna4hb_placement_max_available(&h, 4096, 4096) == 256 * KIB,
          "used=%llu max=%llu", (unsigned long long)rdna4hb_placement_used(&h),
          (unsigned long long)rdna4hb_placement_max_available(&h, 4096, 4096));
    rdna4hb_reap(&h, 9);
    CHECK("unreaped leases stay tracked until serial completes", h.nlease == 4, "nlease=%zu", h.nlease);
    rdna4hb_reap(&h, 10);
    CHECK("completed serial reaps placement leases", h.nlease == 0, "nlease=%zu", h.nlease);
    for (int i = 0; i < 4; i++) rdna4hb_handle_close(&h, blk);
    CHECK("placement block is preserved while heap lives (not auto-evicted)",
          !rdna4hb_has_reclaimable(&h) && !rdna4hb_take_reclaimable(&h, &gone) && h.backing == 256 * KIB,
          "backing=%llu", (unsigned long long)h.backing);
    CHECK("automatic allocator ignores placement block",
          rdna4hb_alloc(&h, 4096, 4096, 4096, 1u, 0, 0, 0, NULL, &r1) == RDNA4HB_R_NO_SPACE,
          "isolated");
    CHECK("teardown reclaims placement block cleanly",
          rdna4hb_take_block(&h, &gone) && gone.id == blk && gone.size == 256 * KIB &&
          gone.host_mapped == 1 && h.backing == 0 && !rdna4hb_take_block(&h, &gone),
          "reclaimed=%llu backing=%llu", (unsigned long long)gone.size, (unsigned long long)h.backing);
    rdna4hb_destroy(&h);

    // Granularity charge vs extent separation + selective single-alias retirement.
    rdna4hb_init(&h, 64 * KIB, RDNA4HB_MAX_NORMAL);
    rdna4hb_add_placement_block(&h, 64 * KIB, 0, 0, 0, (void *)0x3000, NULL, &blk);
    CHECK("place small buffer 100 B (gran 4 KiB) at 0 and alias 200 B at 0",
          rdna4hb_place(&h, 0, 100, 256, 4096, 1u, 0, 0, 0, &r1) == RDNA4HB_R_OK &&
          rdna4hb_place(&h, 0, 200, 256, 4096, 1u, 0, 0, 0, &r2) == RDNA4HB_R_OK &&
          rdna4hb_placement_used(&h) == 200 &&
          rdna4hb_placement_max_available(&h, 4096, 4096) == 60 * KIB,
          "used=%llu max=%llu", (unsigned long long)rdna4hb_placement_used(&h),
          (unsigned long long)rdna4hb_placement_max_available(&h, 4096, 4096));
    rdna4hb_release(&h, r2.lease, 1);
    CHECK("retiring larger alias restores smaller live alias charge",
          rdna4hb_placement_used(&h) == 100 &&
          rdna4hb_placement_max_available(&h, 4096, 4096) == 60 * KIB,
          "used=%llu", (unsigned long long)rdna4hb_placement_used(&h));
    rdna4hb_release(&h, r1.lease, 1);
    rdna4hb_reap(&h, 1);
    CHECK("all aliases retired -> used=0, max=64 KiB, nlease=0",
          rdna4hb_placement_used(&h) == 0 &&
          rdna4hb_placement_max_available(&h, 4096, 4096) == 64 * KIB &&
          h.nlease == 0,
          "used=%llu nlease=%zu", (unsigned long long)rdna4hb_placement_used(&h), h.nlease);
    rdna4hb_destroy(&h);
}

static void test_source_pins(const char *dir) {
    char *m = slurp(dir, "GarnetDevice.m");
    CHECK("assembled GarnetDevice.m readable", m != NULL, "%s/GarnetDevice.m", dir);
    if (!m) return;
    CHECK("pin: swizzle static assertions present",
          strstr(m, "RDNA4SWZ_FMT_A8 == RDNA4F_A8") != NULL &&
          strstr(m, "(int)MTLTextureSwizzleAlpha == RDNA4SWZ_ALPHA") != NULL &&
          strstr(m, "(int)VK_COMPONENT_SWIZZLE_A == RDNA4VKSWZ_A") != NULL,
          "static asserts");
    CHECK("pin: descriptor swizzle read & validated in texture paths",
          strstr(m, "_swz = d.swizzle;") != NULL &&
          strstr(m, "rdna4swz_check(rdna4_swz_from_mtl(d.swizzle), (unsigned long)d.usage, 0, 0, &swzWhy)") != NULL &&
          strstr(m, "rdna4swz_check(rdna4_swz_from_mtl(d.swizzle), (unsigned long)d.usage, RDNA4SWZ_FMT_TEXEL, 0, &swzWhy)") != NULL &&
          strstr(m, "rdna4swz_compose(rdna4_swz_from_mtl(root->_swz), rdna4_swz_from_mtl(swz), &composed)") != NULL &&
          strstr(m, "rdna4swz_check(composed, (unsigned long)root->_usage, 0, 1, &swzWhy)") != NULL,
          "swizzle descriptor, texel-buffer & view wiring");
    {
        const char *tb = strstr(m, "- (instancetype)initTextureBufferWithDevice:(id)dev descriptor:(MTLTextureDescriptor *)d\n"
                                   "    buffer:(RDNA4Buffer *)buffer offset:(NSUInteger)off error:(NSError **)err {");
        const char *tb_end = tb ? strstr(tb, "\n- (BOOL)rdna4IsTexel") : NULL;
        const char *tb_chk = tb ? strstr(tb, "RDNA4SWZ_FMT_TEXEL") : NULL;
        CHECK("pin: initTextureBufferWithDevice enforces RDNA4SWZ_FMT_TEXEL guard",
              tb != NULL && tb_end != NULL && tb_chk != NULL && tb_chk < tb_end,
              "covers both device and buffer-backed texture buffers");
    }
    CHECK("pin: placement heap creation and offset entrypoints wired",
          strstr(m, "d.type != MTLHeapTypeAutomatic && d.type != MTLHeapTypePlacement") != NULL &&
          strstr(m, "- (MTLHeapType)type { return _type; }") != NULL &&
          strstr(m, "rdna4AcquirePlacementBackingForBuffer:") != NULL &&
          strstr(m, "- (NSUInteger)heapOffset { return [self rdna4ResourceHeapOffset]; }") != NULL &&
          strstr(m, "rdna4hb_placement_used(&_hb)") != NULL &&
          strstr(m, "rdna4hb_placement_max_available(&_hb, al, granularity)") != NULL,
          "placement heap wiring");
    CHECK("pin: non-offset calls rejected on placement heaps",
          strstr(m, "if (_type != MTLHeapTypeAutomatic) { RDNA4_ONCE(\"heap newBufferWithLength:options: on a placement heap") != NULL &&
          strstr(m, "if (_type != MTLHeapTypeAutomatic) { RDNA4_ONCE(\"heap newTextureWithDescriptor: on a placement heap") != NULL,
          "automatic/placement dispatch guard");
    free(m);
}

int main(int argc, char **argv) {
    test_swizzle();
    test_placement_heap();
    if (argc > 1 && argv[1][0]) test_source_pins(argv[1]);
    printf("%s test-swizzle-placement (%d checks, %d failed)\n", fails ? "FAIL" : "PASS", checks, fails);
    return fails ? 1 : 0;
}
