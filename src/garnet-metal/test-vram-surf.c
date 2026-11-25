// test-vram-surf.c — Host ASan/UBSan TDD suite for Phase B WindowServer OPTIMAL VRAM surface cache
// (garnet_vram_surf.h): admission, capture exclusion, physical-range sharing, DCC format list,
// incompatible fallback flush, seed/clear upload elision, 50ms idle flush + writeGen race guard,
// LRU eviction with dirty flush, fence-gated release, and kill switches.
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include "garnet_vram_surf.h"

static int g_checks = 0, g_fails = 0;
#define CHECK(name, cond) do { \
    g_checks++; \
    if (!(cond)) { g_fails++; printf("FAIL: %s\n", name); } \
} while (0)

int main(void) {
    // 1. Enable gate & kill-switch
    CHECK("WS enabled without kill file", garnet_vs_should_enable(1, 0, 0) == 1);
    CHECK("WS disabled with /private/tmp/garnet-no-vram-surf", garnet_vs_should_enable(1, 0, 1) == 0);
    CHECK("Non-WS disabled by default", garnet_vs_should_enable(0, 0, 0) == 0);
    CHECK("Non-WS test opt-in works without kill file", garnet_vs_should_enable(0, 1, 0) == 1);
    CHECK("Kill file overrides test opt-in", garnet_vs_should_enable(0, 1, 1) == 0);

    // 2. Format family + explicit VkImageFormatListCreateInfo (DCC preservation)
    uint32_t baseFmt = 0, list[2] = {0, 0}, listCnt = 0;
    uint32_t fam = garnet_vs_format_family(GARNET_VS_VK_BGRA8_SRGB, &baseFmt, list, &listCnt);
    CHECK("BGRA8_SRGB maps to BGRA8 family with explicit 2-format list",
          fam == GARNET_VS_FAM_BGRA8 && baseFmt == GARNET_VS_VK_BGRA8_UNORM &&
          listCnt == 2 && list[0] == GARNET_VS_VK_BGRA8_UNORM && list[1] == GARNET_VS_VK_BGRA8_SRGB);
    CHECK("BGRA8_UNORM and BGRA8_SRGB are format-compatible",
          garnet_vs_format_compatible(fam, baseFmt, GARNET_VS_VK_BGRA8_UNORM) == 1 &&
          garnet_vs_format_compatible(fam, baseFmt, GARNET_VS_VK_BGRA8_SRGB) == 1);
    CHECK("RGBA8_UNORM is NOT compatible with BGRA8 family",
          garnet_vs_format_compatible(fam, baseFmt, GARNET_VS_VK_RGBA8_UNORM) == 0);

    uint32_t f16Base = 0, f16List[2] = {0, 0}, f16Cnt = 99;
    uint32_t f16Fam = garnet_vs_format_family(GARNET_VS_VK_RGBA16_SFLOAT, &f16Base, f16List, &f16Cnt);
    CHECK("RGBA16F needs no mutable format list (full DCC)",
          f16Fam == GARNET_VS_FAM_RGBA16F && f16Base == GARNET_VS_VK_RGBA16_SFLOAT && f16Cnt == 0);

    // 3. Capture stack classifier
    CHECK("CaptureSurfaceMetal detected as capture", garnet_vs_symbol_is_capture("-[WSDisplay CaptureSurfaceMetal:]") == 1);
    CHECK("_XHWCaptureDesktop detected as capture", garnet_vs_symbol_is_capture("_XHWCaptureDesktop") == 1);
    CHECK("CreateCompositeDestination detected as capture", garnet_vs_symbol_is_capture("SLXCreateCompositeDestination") == 1);
    CHECK("ScreenCapture detected as capture", garnet_vs_symbol_is_capture("ScreenCaptureCopyFrame") == 1);
    CHECK("Normal compositor symbol is NOT capture", garnet_vs_symbol_is_capture("WSCompositeLayerTree") == 0);
    CHECK("NULL symbol is NOT capture", garnet_vs_symbol_is_capture(NULL) == 0);

    // 4. Admission boundaries (Class 1 vs Class 2 vs external app window vs capture)
    garnet_vs_cache_t c;
    garnet_vs_init(&c, 1, 100ULL << 20); // 100 MiB cap for eviction tests

    CHECK("Class 1 (_disp=1) promotes directly on first creation",
          garnet_vs_should_promote(&c, 101, 3840, 2160, 1, 0, 1, 1) == 1);
    CHECK("Read-only or multi-plane surface NEVER promotes even if _disp=1",
          garnet_vs_should_promote(&c, 101, 3840, 2160, 1, 1, 1, 1) == 0 &&
          garnet_vs_should_promote(&c, 101, 3840, 2160, 1, 0, 2, 1) == 0 &&
          garnet_vs_should_promote(&c, 101, 3840, 2160, 1, 0, 1, 0) == 0);

    // External app window (2648x1880, usage 0x5, 0 WindowServer writes) MUST NEVER promote
    CHECK("External app window (0 WS writes) NEVER promotes",
          garnet_vs_should_promote(&c, 200, 2648, 1880, 0, 0, 1, 1) == 0);

    // Class 2 requires >= 2 observed WS GPU writes within 1 s
    CHECK("First write needs capture backtrace check", garnet_vs_obs_needs_bt(&c, 201) == 1);
    CHECK("1st WS write to sid 201 does NOT yet promote",
          garnet_vs_note_ws_write(&c, 201, 1920, 1080, 0, 0, 1000000000ULL) == 0 &&
          garnet_vs_should_promote(&c, 201, 1920, 1080, 0, 0, 1, 1) == 0);
    CHECK("Backtrace verdict cached after 1st write", garnet_vs_obs_needs_bt(&c, 201) == 0);
    CHECK("2nd WS write within 1 s enables Class 2 promotion",
          garnet_vs_note_ws_write(&c, 201, 1920, 1080, 0, 0, 1500000000ULL) == 1 &&
          garnet_vs_should_promote(&c, 201, 1920, 1080, 0, 0, 1, 1) == 1);

    // Small surface (< 256x256) never promotes even with >= 2 writes
    garnet_vs_note_ws_write(&c, 202, 128, 128, 0, 0, 1000000000ULL);
    garnet_vs_note_ws_write(&c, 202, 128, 128, 0, 0, 1100000000ULL);
    CHECK("Small surface (<256x256) NEVER promotes",
          garnet_vs_should_promote(&c, 202, 128, 128, 0, 0, 1, 1) == 0);

    // Capture destination sid is permanently excluded even after >= 2 writes/s
    garnet_vs_note_ws_write(&c, 203, 3840, 2160, 0, 1 /* capture */, 1000000000ULL);
    garnet_vs_note_ws_write(&c, 203, 3840, 2160, 0, 0, 1100000000ULL);
    garnet_vs_note_ws_write(&c, 203, 3840, 2160, 0, 0, 1200000000ULL);
    CHECK("Capture-excluded sid NEVER promotes even with repeated writes",
          garnet_vs_should_promote(&c, 203, 3840, 2160, 0, 0, 1, 1) == 0 &&
          c.capture_excluded_sids == 1);

    // Window expiry: 2nd write > 1s after 1st resets window (does NOT promote yet)
    garnet_vs_note_ws_write(&c, 204, 1920, 1080, 0, 0, 1000000000ULL);
    CHECK("2nd write > 1s later resets window and does not promote",
          garnet_vs_note_ws_write(&c, 204, 1920, 1080, 0, 0, 2500000000ULL) == 0 &&
          garnet_vs_should_promote(&c, 204, 1920, 1080, 0, 0, 1, 1) == 0);

    // 5. Physical-range sharing across wrappers (usage 0x1 read-only + sRGB alias)
    const uint32_t supersetUse = 0x1F; // SAMPLED | COLOR_ATT | TRANSFER_SRC | TRANSFER_DST | INPUT_ATT
    garnet_vs_range_t r1 = { .sid = 101, .base = 0x1000000, .ialloc = 33554432, .poff = 0,
                             .w = 3840, .h = 2160, .bpr = 15360 };
    uint64_t id1 = 0, hitId = 0;
    CHECK("cold probe misses",
          garnet_vs_probe(&c, &r1, GARNET_VS_VK_BGRA8_UNORM, 0x17, 1000, 1, &hitId) == GARNET_VS_PROBE_MISS);
    id1 = garnet_vs_add(&c, &r1, GARNET_VS_VK_BGRA8_UNORM, supersetUse, 32ULL << 20,
                        (void *)0x1001, (void *)0x2001, (void *)0x3001, (void *)0x4001, (void *)0x5001, 1000);
    CHECK("add returns nonzero id with refs=1", id1 != 0 && c.n == 1 && c.live_bytes == (32ULL << 20));

    // Read-only usage 0x1 + BGRA8_SRGB wrapper MUST hit same entry!
    CHECK("sRGB read-only wrapper hits same physical range and shares optImg",
          garnet_vs_probe(&c, &r1, GARNET_VS_VK_BGRA8_SRGB, 0x05 /* SAMPLED|TRANSFER */, 2000, 2, &hitId) == GARNET_VS_PROBE_HIT &&
          hitId == id1 && c.hits == 1);

    // Incompatible format (RGBA8_UNORM on BGRA8 entry) MUST return INCOMPAT_FLUSH and mark stale
    uint64_t incompatId = 0;
    CHECK("incompatible format returns INCOMPAT_FLUSH",
          garnet_vs_probe(&c, &r1, GARNET_VS_VK_RGBA8_UNORM, 0x05, 3000, 3, &incompatId) == GARNET_VS_PROBE_INCOMPAT_FLUSH &&
          incompatId == id1 && c.incompat_flushes == 1);
    // Clean up both refs on id1 (now stale)
    garnet_vs_unref(&c, id1, 3100, 3);
    garnet_vs_unref(&c, id1, 3200, 3);
    garnet_vs_rel_t rel;
    CHECK("stale unref'd entry retires cleanly at fence stamp 3",
          c.n == 0 && garnet_vs_pop_release(&c, 2, &rel) == 0 &&
          garnet_vs_pop_release(&c, 3, &rel) == 1 && rel.opt_img == (void *)0x1001 && rel.ibuf == (void *)0x3001);

    // 6. Upload elision (cold vs hot vs seed change vs full clear) & lazy write + 50 ms idle flush
    uint64_t id2 = garnet_vs_add(&c, &r1, GARNET_VS_VK_BGRA8_UNORM, supersetUse, 32ULL << 20,
                                 (void *)0x1002, (void *)0x2002, (void *)0x3002, (void *)0x4002, (void *)0x5002, 4000);
    CHECK("cold read needs upload", garnet_vs_begin_access(&c, id2, 7, 1, 0, 0) == 1);
    CHECK("hot read with same seed skips upload", garnet_vs_begin_access(&c, id2, 7, 1, 0, 0) == 0);
    CHECK("seed change triggers upload", garnet_vs_begin_access(&c, id2, 8, 1, 0, 0) == 1);
    CHECK("full-screen clear skips upload even on new seed", garnet_vs_begin_access(&c, id2, 9, 0, 1, 1) == 0);
    CHECK("subsequent read with seed 9 hits VRAM without upload", garnet_vs_begin_access(&c, id2, 9, 1, 0, 0) == 0);

    // CPU replaceRegion invalidation forces re-upload even with unchanged seed
    garnet_vs_invalidate_cpu_write(&c, id2);
    CHECK("CPU write invalidation forces re-upload", garnet_vs_begin_access(&c, id2, 9, 1, 0, 0) == 1);

    // Lazy GPU write marks dirty & arms idle flush; active_cbs blocks idle flush until CB finishes
    garnet_vs_set_layout(&c, id2, 2 /* COLOR_ATTACHMENT_OPTIMAL */);
    garnet_vs_cb_begin(&c, id2);
    garnet_vs_note_gpu_write(&c, id2, 9, 1000000000ULL, 0 /* lazy */);
    CHECK("lazy write marks dirtyCount=1", c.dirty_count == 1 && c.lazy_writes == 1);

    uint64_t fid = 0, fgen = 0;
    CHECK("idle flush skips while CB is active",
          garnet_vs_pick_idle_flush(&c, 1000000000ULL + GARNET_VS_IDLE_FLUSH_NS + 1, &fid, &fgen) == 0);
    garnet_vs_cb_end(&c, id2);
    CHECK("idle flush skips before 50 ms",
          garnet_vs_pick_idle_flush(&c, 1000000000ULL + GARNET_VS_IDLE_FLUSH_NS - 1, &fid, &fgen) == 0);
    CHECK("idle flush picks dirty idle entry at >= 50 ms",
          garnet_vs_pick_idle_flush(&c, 1000000000ULL + GARNET_VS_IDLE_FLUSH_NS, &fid, &fgen) == 1 &&
          fid == id2 && fgen == 1);

    // Stale writeGen race guard: if a newer write incremented write_gen, finish_flush MUST NOT clear host_dirty!
    garnet_vs_note_gpu_write(&c, id2, 10, 1100000000ULL, 0);
    garnet_vs_finish_flush(&c, id2, fgen /* old gen 1 */, 1);
    CHECK("writeGen mismatch preserves host_dirty=1", c.dirty_count == 1);

    // Now flush with matching gen clears host_dirty and drops dirty_count to 0
    CHECK("second idle flush picks updated gen=2",
          garnet_vs_pick_idle_flush(&c, 1100000000ULL + GARNET_VS_IDLE_FLUSH_NS, &fid, &fgen) == 1 &&
          fid == id2 && fgen == 2);
    garnet_vs_finish_flush(&c, id2, fgen, 1);
    CHECK("matching writeGen clears host_dirty and dirtyCount=0", c.dirty_count == 0 && c.idle_flushes == 1);

    // 7. LRU cap eviction with dirty flush before retirement
    // Cap is 100 MiB. Put three 40 MiB unused entries (120 MiB > 100 MiB) -> oldest dirty entry selected for flush+evict
    garnet_vs_unref(&c, id2, 10, 4); // 32 MiB unused
    garnet_vs_range_t r2 = r1; r2.sid = 102; r2.base = 0x2000000;
    garnet_vs_range_t r3 = r1; r3.sid = 103; r3.base = 0x3000000;
    uint64_t id3 = garnet_vs_add(&c, &r2, GARNET_VS_VK_BGRA8_UNORM, supersetUse, 40ULL << 20,
                                 (void *)0x1003, (void *)0x2003, (void *)0x3003, (void *)0x4003, (void *)0x5003, 5000);
    garnet_vs_set_layout(&c, id3, 2);
    garnet_vs_note_gpu_write(&c, id3, 1, 5000, 0); // dirty!
    garnet_vs_unref(&c, id3, 20, 5); // 72 MiB unused
    uint64_t id4 = garnet_vs_add(&c, &r3, GARNET_VS_VK_BGRA8_UNORM, supersetUse, 40ULL << 20,
                                 (void *)0x1004, (void *)0x2004, (void *)0x3004, (void *)0x4004, (void *)0x5004, 6000);
    garnet_vs_unref(&c, id4, 30, 6); // 112 MiB unused > 100 MiB cap!

    uint64_t evId = 0; int evDirty = 0;
    CHECK("eviction picks oldest unused entry id2 (last_unref=10, clean)",
          garnet_vs_pick_evict_candidate(&c, &evId, &evDirty) == 1 && evId == id2 && evDirty == 0);
    CHECK("retire id2 drops unused under cap (80 MiB <= 100 MiB)",
          garnet_vs_retire(&c, evId, 7) == 1 && c.unused_bytes == (80ULL << 20) &&
          garnet_vs_pick_evict_candidate(&c, &evId, &evDirty) == 0);

    // Lower cap to 50 MiB -> oldest remaining is dirty id3 -> eviction reports needs_flush=1
    c.cap_bytes = 50ULL << 20;
    CHECK("eviction of dirty entry id3 reports needs_flush=1",
          garnet_vs_pick_evict_candidate(&c, &evId, &evDirty) == 1 && evId == id3 && evDirty == 1);
    CHECK("garnet_vs_retire refuses dirty entry before flush",
          garnet_vs_retire(&c, evId, 8) == 0 && c.dirty_count == 1);
    uint64_t syncGen = 0;
    CHECK("sync flush begins on dirty id3", garnet_vs_begin_sync_flush(&c, evId, &syncGen) == 1);
    garnet_vs_finish_flush(&c, evId, syncGen, 1);
    CHECK("after sync flush, dirtyCount=0 and retire succeeds",
          c.dirty_count == 0 && garnet_vs_retire(&c, evId, 8) == 1 && c.unused_bytes == (40ULL << 20));

    // Read-only peek & capture exclusion helper
    CHECK("peek reports no upload needed on hot entry", garnet_vs_peek_needs_upload(&c, id4, 0) == 1);
    garnet_vs_mark_capture_excluded(&c, 999);
    CHECK("mark_capture_excluded permanently blocks Class 2 promotion",
          garnet_vs_note_ws_write(&c, 999, 1920, 1080, 0, 0, 2000000000ULL) == 0 &&
          garnet_vs_should_promote(&c, 999, 1920, 1080, 0, 0, 1, 1) == 0);

    // 8. Wave 4 Path (b) seed-elided upload state helpers
    CHECK("cold Path (b) state 0 always requires upload (even for seed 0)",
          garnet_vs_pathb_should_upload(0ULL, 0u) == 1 &&
          garnet_vs_pathb_should_upload(0ULL, 7u) == 1);
    const uint64_t pb0 = garnet_vs_pathb_pack(0u);
    CHECK("packed seed 0 skips upload on seed 0 and re-uploads on seed 1",
          garnet_vs_pathb_should_upload(pb0, 0u) == 0 &&
          garnet_vs_pathb_should_upload(pb0, 1u) == 1);
    const uint64_t pb42 = garnet_vs_pathb_pack(42u);
    CHECK("packed seed 42 skips on 42 and re-uploads on 43",
          garnet_vs_pathb_should_upload(pb42, 42u) == 0 &&
          garnet_vs_pathb_should_upload(pb42, 43u) == 1);

    garnet_vs_destroy(&c);
    printf("test-vram-surf: %d checks, %d failed\n", g_checks, g_fails);
    return g_fails ? 1 : 0;
}
