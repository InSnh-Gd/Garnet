// test-host-display.c: consolidated host C11 tests for GarnetMetal display,
// IOSurface geometry, texture transfer bounds, display commit ordering, and CRC sampling.
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "garnet_pitch.h"
#include "garnet_texture_transfer.h"
#include "garnet_display_commits.h"
#include "garnet_crc.h"
#include "garnet_plane.h"

static unsigned g_checks, g_fails;

#define CHECK(name, cond) do { \
    g_checks++; \
    if (!(cond)) { \
        g_fails++; \
        fprintf(stderr, "FAIL [%s:%d] %s\n", __FILE__, __LINE__, (name)); \
    } \
} while (0)

static void suite_pitch(void) {
    CHECK("pitch 960x540 bpr 4096", rdna4p_explicit_pitch_reason(4096, 960, 4, 3840, 0, 1, 1, 1, 1) == RDNA4P_EXPLICIT_TRY);
    CHECK("pitch 88x60 bpr 384", rdna4p_explicit_pitch_reason(384, 88, 4, 512, 0, 1, 1, 1, 1) == RDNA4P_EXPLICIT_TRY);
    CHECK("pitch 3752x60 bpr 15040 unaligned", rdna4p_explicit_pitch_reason(15040, 3752, 4, 15104, 0, 1, 1, 1, 1) == RDNA4P_EXPLICIT_PITCH);
    CHECK("pitch 916x514 bpr 3904 unaligned", rdna4p_explicit_pitch_reason(3904, 916, 4, 3840, 0, 1, 1, 1, 1) == RDNA4P_EXPLICIT_PITCH);
    CHECK("pitch IconServices RGBA16F 268x268 bpr 2144", rdna4p_explicit_pitch_reason(2144, 268, 8, 2304, 0, 1, 1, 1, 1) == RDNA4P_EXPLICIT_PITCH);
    CHECK("pitch 210x118 bpr 896", rdna4p_explicit_pitch_reason(896, 210, 4, 1024, 0, 1, 1, 1, 1) == RDNA4P_EXPLICIT_TRY);
    CHECK("pitch natural match", rdna4p_explicit_pitch_reason(4096, 1024, 4, 4096, 0, 1, 1, 1, 1) == RDNA4P_EXPLICIT_MATCH);
    CHECK("pitch smaller than row bytes", rdna4p_explicit_pitch_reason(383, 96, 4, 512, 0, 1, 1, 1, 1) == RDNA4P_EXPLICIT_PITCH);
    CHECK("pitch missing extension", rdna4p_explicit_pitch_reason(4096, 960, 4, 3840, 0, 1, 1, 1, 0) == RDNA4P_EXPLICIT_NO_EXTENSION);
    CHECK("pitch image flags refuse", rdna4p_explicit_pitch_reason(4096, 960, 4, 3840, 1, 1, 1, 1, 1) == RDNA4P_EXPLICIT_FLAGS);
    CHECK("pitch multiple mip levels refuse", rdna4p_explicit_pitch_reason(4096, 960, 4, 3840, 0, 2, 1, 1, 1) == RDNA4P_EXPLICIT_SHAPE);
    CHECK("pitch multiple array layers refuse", rdna4p_explicit_pitch_reason(4096, 960, 4, 3840, 0, 1, 2, 1, 1) == RDNA4P_EXPLICIT_SHAPE);
    CHECK("pitch multiple samples refuse", rdna4p_explicit_pitch_reason(4096, 960, 4, 3840, 0, 1, 1, 4, 1) == RDNA4P_EXPLICIT_SHAPE);
    CHECK("pitch width overflow refuses", rdna4p_explicit_pitch_reason(4096, SIZE_MAX, 4, 3840, 0, 1, 1, 1, 1) == RDNA4P_EXPLICIT_SHAPE);
}

static void suite_texture_transfer(void) {
    const size_t widths[] = {0, 1, 2, 64, UINT32_MAX, SIZE_MAX};
    const size_t heights[] = {0, 1, 2, 3, 4096};
    const size_t depths[] = {0, 1, 2, 3, 4096};
    const size_t bpps[] = {1, 4, 8};
    const size_t strides[] = {0, 1, 8, 16, 256, 4096, SIZE_MAX / 2, SIZE_MAX};
    unsigned bad = 0;
    for (unsigned w = 0; w < 6; ++w) for (unsigned h = 0; h < 5; ++h)
    for (unsigned d = 0; d < 5; ++d) for (unsigned b = 0; b < 3; ++b)
    for (unsigned r = 0; r < 8; ++r) for (unsigned i = 0; i < 8; ++i) {
        size_t width = widths[w], height = heights[h], depth = depths[d];
        size_t bpp = bpps[b], pitch = strides[r], image = strides[i];
        __uint128_t row = (__uint128_t)width * bpp, packed = row * height * depth;
        __uint128_t span = height ? (__uint128_t)(height - 1) * pitch + row : 0;
        __uint128_t minimum_image = (__uint128_t)pitch * height;
        if (depth > 1) span += (__uint128_t)(depth - 1) * image;
        int expected = width && height && depth && row <= SIZE_MAX && packed <= SIZE_MAX &&
            pitch >= row && span <= SIZE_MAX && (depth == 1 || image >= minimum_image);
        rdna4_texture_transfer actual = {13, 17, 19}, before = actual;
        int ok = rdna4_texture_transfer_layout(width, height, depth, bpp, pitch, image, &actual);
        if (ok != expected || (ok && (actual.row_bytes != row || actual.packed_bytes != packed || actual.span != span)) ||
            (!ok && memcmp(&actual, &before, sizeof actual))) {
            bad++;
        }
    }
    CHECK("texture transfer 28800 exact oracle checks", bad == 0);
    CHECK("texture transfer pointer edges",
          !rdna4_texture_transfer_pointer(NULL, 16) &&
          !rdna4_texture_transfer_pointer((void *)(UINTPTR_MAX - 15), 16) &&
          rdna4_texture_transfer_pointer((void *)(UINTPTR_MAX - 15), 15));
}

static void suite_display_commits(void) {
    rdna4_display_commit entries[128] = {{0}}, *head = NULL;
    unsigned sid[128] = {0}; uint64_t order[128] = {0}, clock = 0;
    srand(495);
    for (unsigned step = 0; step < 100000; step++) {
        unsigned i = (unsigned)rand() % 128;
        if (sid[i]) {
            rdna4dc_commit_remove(&head, &entries[i]); sid[i] = 0; order[i] = 0;
        } else {
            sid[i] = (unsigned)rand() % 32 + 1; order[i] = ++clock;
            rdna4dc_commit_add(&head, &entries[i], sid[i], order[i]);
            rdna4dc_commit_add(&head, &entries[i], sid[i], order[i]);
        }
        unsigned surface = (unsigned)rand() % 32 + 1;
        uint64_t stamp = clock ? (unsigned)rand() % clock : 0;
        int expected = 0;
        for (unsigned n = 0; n < 128; n++) expected |= sid[n] == surface && order[n] > stamp;
        assert(rdna4dc_commit_newer(head, surface, stamp) == expected);
        unsigned actual = 0, count = 0;
        for (unsigned n = 0; n < 128; n++) count += sid[n] != 0;
        for (rdna4_display_commit *p = head; p; p = p->next) assert(++actual <= count);
        assert(actual == count);
    }
    for (unsigned i = 0; i < 128; i++) rdna4dc_commit_remove(&head, &entries[i]);
    CHECK("display commits 100k oracle drained", head == NULL);

    rdna4df_t state; rdna4df_init(&state, 0); rdna4df_activate(&state, 3);
    uint32_t flags[3] = {4, 4, 4};
    int slot = rdna4df_pick(&state, flags); rdna4df_submit(&state, slot, 8, 10); state.sub_commit[slot] = 1;
    rdna4dc_commit_invalidate(&state, 7, 2);
    CHECK("display commits different surface does not supersede", !rdna4df_is_superseded(&state, slot));
    rdna4dc_commit_invalidate(&state, 8, 1);
    CHECK("display commits equal commit does not supersede", !rdna4df_is_superseded(&state, slot));
    rdna4dc_commit_invalidate(&state, 8, 2);
    CHECK("display commits newer commit supersedes", !rdna4df_complete_surf(&state, slot, 0, 10, 8) && state.superseded == 1);
    slot = rdna4df_pick(&state, flags); rdna4df_submit(&state, slot, 8, 11); state.sub_commit[slot] = 2;
    CHECK("display commits fresh submit completes", rdna4df_complete_surf(&state, slot, 0, 11, 8));
    rdna4df_retry_note(&state, slot, 8, 11, 2, RDNA4DF_C_FINAL, -EBUSY);
    rdna4dc_commit_invalidate(&state, 7, 3);
    CHECK("display commits retry ignores other surface", state.retry_slot == slot && state.retry_dropped == 0);
    rdna4dc_commit_invalidate(&state, 8, 1);
    CHECK("display commits retry ignores older commit", state.retry_slot == slot && state.retry_dropped == 0);
    rdna4dc_commit_invalidate(&state, 8, 2);
    CHECK("display commits retry ignores equal commit", state.retry_slot == slot && state.retry_dropped == 0);
    rdna4dc_commit_invalidate(&state, 8, 3);
    CHECK("display commits retry dropped on newer commit", state.retry_slot == -1 && state.retry_dropped == 1);
}

static void suite_crc(void) {
    CHECK("crc32 check value", rdna4crc32(0, "123456789", 9) == 0xCBF43926u);
    CHECK("crc32 incremental", rdna4crc32(rdna4crc32(0, "1234", 4), "56789", 5) == 0xCBF43926u);
    CHECK("crc32 empty", rdna4crc32(0, "", 0) == 0);
    const uint32_t W = 2560, H = 1440; const size_t pitch = (size_t)W * 4, rb = pitch;
    uint8_t *a = malloc(pitch * H), *b = malloc(pitch * H), *scr = malloc(rb);
    assert(a && b && scr);
    for (size_t i = 0; i < pitch * H; i++) a[i] = (uint8_t)(i * 2654435761u >> 13);
    memcpy(b, a, pitch * H);
    rdna4crc_samp_t sa, sb; uint32_t out[8];
    CHECK("crc rows at step 64", rdna4crc_sample(&sa, a, pitch, rb, H, 64, scr) == 23 && sa.step == 64);
    CHECK("crc default step", rdna4crc_sample(&sb, a, pitch, rb, H, 0, scr) == 23 && sb.step == RDNA4CRC_DEFAULT_STEP);
    CHECK("crc cap", rdna4crc_sample(&sb, a, pitch, rb, 100000, 1, scr) == RDNA4CRC_MAXROWS);
    rdna4crc_sample(&sa, a, pitch, rb, H, 64, scr); rdna4crc_sample(&sb, b, pitch, rb, H, 64, scr);
    CHECK("crc identical no diff", rdna4crc_diff(&sa, &sb, out, 8) == 0 && rdna4crc_sig(&sa) == rdna4crc_sig(&sb));
    b[(size_t)128 * pitch + 4000] ^= 1; rdna4crc_sample(&sb, b, pitch, rb, H, 64, scr);
    int nd = rdna4crc_diff(&sa, &sb, out, 8);
    CHECK("crc 1 bit in sampled row 128", nd == 1 && out[0] == 128 && rdna4crc_sig(&sa) != rdna4crc_sig(&sb));
    memcpy(b, a, pitch * H); b[(size_t)129 * pitch + 10] ^= 0xFF; rdna4crc_sample(&sb, b, pitch, rb, H, 64, scr);
    CHECK("crc unsampled row 129 not seen", rdna4crc_diff(&sa, &sb, out, 8) == 0);
    memcpy(b, a, pitch * H); b[(size_t)0] ^= 1; b[(size_t)1408 * pitch + pitch - 1] ^= 1; rdna4crc_sample(&sb, b, pitch, rb, H, 64, scr);
    nd = rdna4crc_diff(&sa, &sb, out, 8);
    CHECK("crc first and last sampled rows", nd == 2 && out[0] == 0 && out[1] == 1408);
    nd = rdna4crc_diff(&sa, &sb, out, 1);
    CHECK("crc diff total beyond max", nd == 2 && out[0] == 0);
    uint8_t *p = malloc((size_t)(pitch + 64) * H);
    assert(p);
    for (size_t i = 0; i < (pitch + 64) * H; i++) p[i] = (uint8_t)i;
    rdna4crc_samp_t s1, s2; rdna4crc_sample(&s1, p, pitch + 64, rb, H, 64, scr);
    p[(size_t)64 * (pitch + 64) + pitch + 3] ^= 1; rdna4crc_sample(&s2, p, pitch + 64, rb, H, 64, scr);
    CHECK("crc pitch padding ignored", rdna4crc_diff(&s1, &s2, out, 8) == 0);
    rdna4crc_samp_t s3 = s1; s3.rows = 22;
    CHECK("crc row-count mismatch counts", rdna4crc_diff(&s1, &s3, out, 8) == 1);
    rdna4crc_stats_t st; memset(&st, 0, sizeof st);
    rdna4crc_samp_t a1 = sa, a2 = sa, bb = sa, c = sa;
    CHECK("crc account clean", rdna4crc_account(&st, &a1, &a2, &bb, 500000, 400000, 0) == 0 && st.checked == 1 && st.cost_n == 1);
    a2 = sb; CHECK("crc account unstable", rdna4crc_account(&st, &a1, &a2, &bb, 700000, 600000, 1) == RDNA4CRC_F_UNSTABLE && st.src_unstable == 1);
    a2 = sa; bb = sb; CHECK("crc account slot != src", rdna4crc_account(&st, &a1, &a2, &bb, 900000, 800000, 0) == RDNA4CRC_F_SLOT_NE && st.slot_ne_src == 1);
    a2 = sb; bb = sb; CHECK("crc account both", rdna4crc_account(&st, &a1, &a2, &bb, 100, 50, 0) == (RDNA4CRC_F_UNSTABLE | RDNA4CRC_F_SLOT_NE) && st.checked == 4);
    CHECK("crc account present clean", rdna4crc_account_present(&st, &a1, &c, 1000, 0, 0) == 0 && st.present_checked == 1);
    c = sb; CHECK("crc account present changed", rdna4crc_account_present(&st, &a1, &c, 1000, 0, 1) == 1 && st.src_chg_present == 1 && st.present_checked == 2);
    CHECK("crc cost avg/max", st.cost_max == 900000 && st.cost_n == 6 && st.cost_sum == 500000 + 700000 + 900000 + 100 + 2000);
    CHECK("crc classify later/outside", st.chg_later_cb == 2 && st.chg_outside == 1);
    rdna4crc_stats_t st2; memset(&st2, 0, sizeof st2);
    CHECK("crc noslot b NULL skips slot compare", rdna4crc_account(&st2, &a1, &a1, NULL, 1000, 0, 0) == 0 && st2.slot_n == 0 && st2.checked == 1);
    CHECK("crc present-change not double counted", rdna4crc_account_present(&st2, &a1, &sb, 1, 1, 1) == 1 && st2.chg_later_cb == 0 && st2.chg_outside == 0);
    CHECK("crc present-change classified when new", rdna4crc_account_present(&st2, &a1, &sb, 1, 0, 0) == 1 && st2.chg_outside == 1);
    char line[500]; rdna4crc_summary(&st, line, sizeof line);
    CHECK("crc summary text",
          strstr(line, "frames_checked 4") && strstr(line, "slot_ne_src 2") &&
          strstr(line, "src_changed_before_present 1") && strstr(line, "crc_cost_max_us 900.0") &&
          strstr(line, "slot_read_max_us 800.0") && strstr(line, "changed_by_later_cb"));
    free(p); free(scr); free(b); free(a);
}

static int plane_run(const rdna4pl_in *in, rdna4pl_out *o) {
    return rdna4pl_geom(in, o) && rdna4pl_bound(in, o);
}

static void suite_plane(void) {
    rdna4pl_out o;
    const uint64_t P0 = 2073600, ALLOC = 2073600 + 1920 * 540;
    rdna4pl_in p0 = { .pc = 2, .plane = 0, .pw = 1920, .ph = 1080, .pbpe = 1, .pbpr = 1920, .poff = 0, .alloc = ALLOC, .dw = 1920, .dh = 1080, .dbpp = 1 };
    rdna4pl_in p1 = { .pc = 2, .plane = 1, .pw = 960, .ph = 540, .pbpe = 2, .pbpr = 1920, .poff = P0, .alloc = ALLOC, .dw = 960, .dh = 540, .dbpp = 2 };
    CHECK("plane 420v plane 0", plane_run(&p0, &o) && o.off == 0 && o.rowlen == 1920);
    CHECK("plane 420v plane 1", plane_run(&p1, &o) && o.off == P0 && o.rowlen == 960);
    rdna4pl_in q = p1; q.plane = 2;
    CHECK("plane == count refused 74", !plane_run(&q, &o) && o.code == 74);
    q = p1; q.plane = 7;
    CHECK("plane > count refused 74", !plane_run(&q, &o) && o.code == 74);
    q = p1; q.pc = 3; q.plane = 2;
    CHECK("plane 3-plane plane 2 accepted", plane_run(&q, &o));
    q = p1; q.poff = ALLOC - 1920 * 540 + 2;
    CHECK("plane overflow alloc refused 81", !plane_run(&q, &o) && o.code == 81);
    q = p1; q.poff = UINT64_MAX;
    CHECK("plane below base refused 81", !plane_run(&q, &o) && o.code == 81);
    q = p1; q.poff = P0 + 1;
    CHECK("plane unaligned offset refused 81/93", !plane_run(&q, &o) && (o.code == 81 || o.code == 93));
    q = p1; q.alloc = ALLOC + 4096; q.poff = P0 + 1;
    CHECK("plane odd offset inside alloc refused 93", !plane_run(&q, &o) && o.code == 93);
    q = p1; q.dw = 1920; q.dh = 1080;
    CHECK("plane descriptor mismatch refused 75", !plane_run(&q, &o) && o.code == 75);
    q = p1; q.dbpp = 1;
    CHECK("plane R8 on RG8 refused 76", !plane_run(&q, &o) && o.code == 76);
    q = p1; q.pbpr = 1000;
    CHECK("plane bpr below width*bpe refused 77", !plane_run(&q, &o) && o.code == 77);
    CHECK("plane exact fit accepted", plane_run(&p1, &o) && P0 + 1920ull * 540 == ALLOC);
    q = p1; q.alloc = ALLOC - 1;
    CHECK("plane one byte short refused 81", !plane_run(&q, &o) && o.code == 81);
    q = p1; q.pbpr = 0xFFFFFFFFFFFF0000ull; q.ph = 0x100000;
    CHECK("plane bpr*height overflow refused 81", !rdna4pl_bound(&q, &o) && o.code == 81);
    q = p1; q.pbpr = 960;
    CHECK("plane RG8 960 bpr refused 77", !plane_run(&q, &o) && o.code == 77);
    q = p1; q.pbpr = 2048; q.alloc = P0 + 2048ull * 540;
    CHECK("plane padded rowlen 1024", plane_run(&q, &o) && o.rowlen == 1024);
    rdna4pl_in ci = { .pc = 1, .plane = 0, .pw = 320, .ph = 576, .pbpe = 8, .pbpr = 2560, .poff = 0, .alloc = 2560ull * 576, .dw = 308, .dh = 514, .dbpp = 8 };
    CHECK("plane CI sub-rect accepted", plane_run(&ci, &o) && o.rowlen == 320);
    q = ci; q.dw = 330;
    CHECK("plane CI dw>pw refused 75", !plane_run(&q, &o) && o.code == 75);
    q = ci; q.dh = 577;
    CHECK("plane CI dh>ph refused 75", !plane_run(&q, &o) && o.code == 75);
    q = ci; q.dw = 320; q.dh = 576;
    CHECK("plane CI exact unchanged", plane_run(&q, &o) && o.rowlen == 320);
    rdna4pl_in s1 = { .pc = 1, .plane = 0, .pw = 64, .ph = 64, .pbpe = 4, .pbpr = 256, .poff = 0, .alloc = 16384, .dw = 64, .dh = 64, .dbpp = 4 };
    CHECK("plane pc 1 plane 0", plane_run(&s1, &o) && o.off == 0 && o.rowlen == 64);
    rdna4pl_in s0 = s1; s0.pc = 0;
    CHECK("plane pc 0 plane 0", plane_run(&s0, &o) && o.off == 0 && o.rowlen == 64);
    q = s1; q.plane = 1;
    CHECK("plane pc 1 plane 1 refused 74", !plane_run(&q, &o) && o.code == 74 && !strcmp(o.why, "plane 1 of a single-plane surface"));
    q = s0; q.plane = 1;
    CHECK("plane pc 0 plane 1 refused 74", !plane_run(&q, &o) && o.code == 74);
    q = s1; q.alloc = 256 * 64 - 1;
    CHECK("plane pc 1 overflow refused 81", !plane_run(&q, &o) && o.code == 81);
    CHECK("plane bind aligned fits", rdna4pl_bind_ok(P0, 1920 * 540, 256, ALLOC + 1000));
    CHECK("plane bind unaligned -> path b", !rdna4pl_bind_ok(P0, 1920 * 540, 4096, ALLOC + 1000));
    CHECK("plane bind oversize -> path b", !rdna4pl_bind_ok(P0, 1920 * 540 + 1, 256, ALLOC));
    CHECK("plane bind past import -> path b", !rdna4pl_bind_ok(ALLOC + 1, 0, 256, ALLOC));
}

int main(void) {
    suite_pitch();
    suite_texture_transfer();
    suite_display_commits();
    suite_crc();
    suite_plane();
    printf("test-host-display: %s (%u checks, %u failed)\n", g_fails ? "FAIL" : "PASS", g_checks, g_fails);
    return g_fails != 0;
}
