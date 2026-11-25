// test-io-residency.c: pure-C host unit tests + source/structure pins for
// Task 4.1 (MTLIOCommandQueue) and Task 4.2 (MTLResidencySet).
// Build/run:
//   cc -std=c11 -D_GNU_SOURCE -O2 -Wall -Wextra -Werror -Isrc/garnet-metal -o "$TMPDIR/test-io-residency" src/garnet-metal/test-io-residency.c
//   "$TMPDIR/test-io-residency" src/garnet-metal "$TMPDIR/assembled"
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "rdna4_devcaps.h"
#include "rdna4_io_queue.h"
#include "rdna4_residency_set.h"

static int fails, checks;
#define CHECK(name, cond, ...) do { \
    int ok_ = (cond) ? 1 : 0; \
    checks++; \
    printf("%s %s: ", ok_ ? "ok  " : "FAIL", name); \
    printf(__VA_ARGS__); \
    printf("\n"); \
    if (!ok_) fails++; \
} while (0)

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

static void test_io_compression_and_desc(void) {
    int err = 0;
    const char *why = NULL;
    uint32_t conc = 0;

    CHECK("io max commands matches devcaps (8192)",
          RDNA4_IO_MAX_COMMANDS == 8192u && RDNA4_IO_MAX_COMMANDS == RDNA4DC_IO_MAX_COMMANDS,
          "max=%u", RDNA4_IO_MAX_COMMANDS);

    CHECK("io uncompressed handle allowed",
          rdna4io_check_compression(RDNA4IO_COMP_NONE, &err, &why) == RDNA4IO_OK &&
          err == RDNA4IO_ERR_NONE && why == NULL,
          "NONE ok");

    int codecs[] = {
        RDNA4IO_COMP_ZLIB, RDNA4IO_COMP_LZFSE, RDNA4IO_COMP_LZ4,
        RDNA4IO_COMP_LZMA, RDNA4IO_COMP_LZBITMAP, -2, 99
    };
    int all_refused = 1;
    for (size_t i = 0; i < sizeof(codecs) / sizeof(codecs[0]); i++) {
        err = 0; why = NULL;
        rdna4io_reason_t r = rdna4io_check_compression(codecs[i], &err, &why);
        if (r != RDNA4IO_R_UNSUPPORTED_COMPRESSION || err != RDNA4IO_ERR_INTERNAL || !why || !why[0])
            all_refused = 0;
    }
    CHECK("io compressed/unknown methods explicitly refused (never garbage)",
          all_refused, "7 codecs/unknowns refused");

    CHECK("io queue desc serial forces concurrency=1",
          rdna4io_check_queue_desc(RDNA4IO_QUEUE_SERIAL, RDNA4IO_PRIO_HIGH, 16, &conc, &why) == RDNA4IO_OK &&
          conc == 1u,
          "conc=%u", conc);
    CHECK("io queue desc concurrent default and clamp",
          rdna4io_check_queue_desc(RDNA4IO_QUEUE_CONCURRENT, RDNA4IO_PRIO_NORMAL, 0, &conc, &why) == RDNA4IO_OK &&
          conc == RDNA4_IO_DEFAULT_CONCURRENCY &&
          rdna4io_check_queue_desc(RDNA4IO_QUEUE_CONCURRENT, RDNA4IO_PRIO_LOW, 2, &conc, &why) == RDNA4IO_OK &&
          conc == 2u &&
          rdna4io_check_queue_desc(RDNA4IO_QUEUE_CONCURRENT, RDNA4IO_PRIO_HIGH, 999, &conc, &why) == RDNA4IO_OK &&
          conc == RDNA4_IO_MAX_CONCURRENCY,
          "clamped=%u", conc);
    CHECK("io queue desc rejects invalid type or priority",
          rdna4io_check_queue_desc(2, RDNA4IO_PRIO_NORMAL, 0, &conc, &why) == RDNA4IO_R_INVALID_ARG && why != NULL &&
          rdna4io_check_queue_desc(-1, RDNA4IO_PRIO_NORMAL, 0, &conc, &why) == RDNA4IO_R_INVALID_ARG &&
          rdna4io_check_queue_desc(RDNA4IO_QUEUE_CONCURRENT, 3, 0, &conc, &why) == RDNA4IO_R_INVALID_ARG &&
          rdna4io_check_queue_desc(RDNA4IO_QUEUE_SERIAL, -1, 0, &conc, &why) == RDNA4IO_R_INVALID_ARG,
          "invalid enums refused");
}

static void test_io_bounds_and_texture_plan(void) {
    CHECK("io load_bytes bounds & NULL guard",
          rdna4io_check_load_bytes(1024, 0, 1024, 1) == RDNA4IO_OK &&
          rdna4io_check_load_bytes(1024, 1024, 0, 0) == RDNA4IO_OK &&
          rdna4io_check_load_bytes(1024, 1025, 0, 1) == RDNA4IO_R_SRC_OOB &&
          rdna4io_check_load_bytes(1024, 0, 1, 0) == RDNA4IO_R_INVALID_ARG &&
          rdna4io_check_load_bytes(1024, 1000, 25, 1) == RDNA4IO_R_SRC_OOB &&
          rdna4io_check_load_bytes(1024, UINT64_MAX - 10, 20, 1) == RDNA4IO_R_SRC_OOB,
          "bytes checks ok");

    CHECK("io load_buffer bounds & wraparound overflow",
          rdna4io_check_load_buffer(4096, 512, 2048, 256, 1792) == RDNA4IO_OK &&
          rdna4io_check_load_buffer(4096, 4096, 2048, 2048, 0) == RDNA4IO_OK &&
          rdna4io_check_load_buffer(4096, 4097, 2048, 0, 0) == RDNA4IO_R_SRC_OOB &&
          rdna4io_check_load_buffer(4096, 0, 2048, 2049, 0) == RDNA4IO_R_DST_OOB &&
          rdna4io_check_load_buffer(4096, 4000, 2048, 0, 128) == RDNA4IO_R_SRC_OOB &&
          rdna4io_check_load_buffer(4096, 0, 2048, 2000, 128) == RDNA4IO_R_DST_OOB &&
          rdna4io_check_load_buffer(4096, UINT64_MAX - 8, 2048, 0, 16) == RDNA4IO_R_SRC_OOB &&
          rdna4io_check_load_buffer(4096, 0, 2048, UINT64_MAX - 8, 16) == RDNA4IO_R_DST_OOB,
          "buffer checks ok");

    rdna4io_tex_plan_t plan;
    // Mipmapped 2D texture 64x32 RGBA8 (6 levels), load level 2 (16x8) subrect 8x4 at (4,2) with padded bpr=64.
    // Exact source span = 3 * 64 + 32 = 224 (no trailing padding required after final row!).
    memset(&plan, 0, sizeof plan);
    CHECK("io plan 2D mip subrect with padded bpr exact span",
          rdna4io_plan_texture_load(300, 76, 64, 0, 64, 32, 1, 6, 1, 0, 0, 0, 4,
                                    0, 2, 4, 2, 0, 8, 4, 1, &plan) == RDNA4IO_OK &&
          plan.bw == 8 && plan.bh == 4 && plan.depth == 1 &&
          plan.row_bytes == 32 && plan.row_stride == 64 &&
          plan.src_span == 224 && plan.packed_bytes == 128,
          "span=%llu packed=%llu", (unsigned long long)plan.src_span,
          (unsigned long long)plan.packed_bytes);
    CHECK("io plan 2D rejects file 1 byte short of exact span",
          rdna4io_plan_texture_load(299, 76, 64, 0, 64, 32, 1, 6, 1, 0, 0, 0, 4,
                                    0, 2, 4, 2, 0, 8, 4, 1, &plan) == RDNA4IO_R_SRC_OOB,
          "299 < 76+224 refused");

    // 3D texture 16x8x4 RGBA8, level 1 (8x4x2), load full 8x4x2 with bpr=64, bpi=512.
    // Exact span = 1 * 512 + 3 * 64 + 32 = 736.
    CHECK("io plan 3D multi-slice exact span & strides",
          rdna4io_plan_texture_load(1000, 264, 64, 512, 16, 8, 4, 4, 1, 0, 1, 0, 4,
                                    0, 1, 0, 0, 0, 8, 4, 2, &plan) == RDNA4IO_OK &&
          plan.src_span == 736 && plan.packed_bytes == 256,
          "3D span=%llu packed=%llu", (unsigned long long)plan.src_span,
          (unsigned long long)plan.packed_bytes);
    CHECK("io plan 3D rejects bpi < bpr*bh",
          rdna4io_plan_texture_load(4096, 0, 64, 255, 16, 8, 4, 4, 1, 0, 1, 0, 4,
                                    0, 1, 0, 0, 0, 8, 4, 2, &plan) == RDNA4IO_R_BAD_LAYOUT,
          "small bpi refused");

    // 1D texture ignores bpr=0; 2D multi-row rejects bpr=0 and bpr < row_bytes.
    CHECK("io plan 1D allows bpr=0 and 2D rejects bpr=0 when bh>1",
          rdna4io_plan_texture_load(1024, 0, 0, 0, 64, 1, 1, 1, 4, 1, 0, 0, 4,
                                    3, 0, 8, 0, 0, 16, 1, 1, &plan) == RDNA4IO_OK &&
          plan.src_span == 64 && plan.packed_bytes == 64 &&
          rdna4io_plan_texture_load(1024, 0, 0, 0, 16, 16, 1, 1, 1, 0, 0, 0, 4,
                                    0, 0, 0, 0, 0, 8, 4, 1, &plan) == RDNA4IO_R_BAD_LAYOUT &&
          rdna4io_plan_texture_load(1024, 0, 28, 0, 16, 16, 1, 1, 1, 0, 0, 0, 4,
                                    0, 0, 0, 0, 0, 8, 4, 1, &plan) == RDNA4IO_R_BAD_LAYOUT,
          "1D/2D bpr rules enforced");

    // BC7 (16 B per 4x4 block): aligned 8x8 ok, edge mip 2x2 ok, unaligned interior refused.
    CHECK("io plan BC7 alignment and edge mip rules",
          rdna4io_plan_texture_load(1024, 0, 32, 0, 16, 16, 1, 5, 1, 0, 0, 1, 16,
                                    0, 0, 4, 4, 0, 8, 8, 1, &plan) == RDNA4IO_OK &&
          plan.bw == 2 && plan.bh == 2 && plan.row_bytes == 32 && plan.packed_bytes == 64 &&
          rdna4io_plan_texture_load(1024, 0, 16, 0, 16, 16, 1, 5, 1, 0, 0, 1, 16,
                                    0, 3, 0, 0, 0, 2, 2, 1, &plan) == RDNA4IO_OK &&
          plan.bw == 1 && plan.bh == 1 && plan.packed_bytes == 16 &&
          rdna4io_plan_texture_load(1024, 0, 32, 0, 16, 16, 1, 5, 1, 0, 0, 1, 16,
                                    0, 0, 2, 0, 0, 4, 4, 1, &plan) == RDNA4IO_R_BAD_LAYOUT &&
          rdna4io_plan_texture_load(1024, 0, 32, 0, 16, 16, 1, 5, 1, 0, 0, 1, 16,
                                    0, 0, 0, 0, 0, 6, 4, 1, &plan) == RDNA4IO_R_BAD_LAYOUT,
          "BC alignment enforced");

    // OOB level/slice/origin/size and 64-bit overflows.
    CHECK("io plan rejects OOB slice/level/extent and 64-bit overflow",
          rdna4io_plan_texture_load(4096, 0, 64, 0, 16, 16, 1, 2, 2, 0, 0, 0, 4,
                                    2, 0, 0, 0, 0, 4, 4, 1, &plan) == RDNA4IO_R_DST_OOB &&
          rdna4io_plan_texture_load(4096, 0, 64, 0, 16, 16, 1, 2, 2, 0, 0, 0, 4,
                                    0, 2, 0, 0, 0, 4, 4, 1, &plan) == RDNA4IO_R_DST_OOB &&
          rdna4io_plan_texture_load(4096, 0, 64, 0, 16, 16, 1, 2, 2, 0, 0, 0, 4,
                                    0, 1, 0, 0, 0, 9, 4, 1, &plan) == RDNA4IO_R_DST_OOB &&
          rdna4io_plan_texture_load(4096, 0, 64, 0, 16, 16, 1, 2, 2, 0, 0, 0, 4,
                                    0, 0, 0, 0, 0, 0, 4, 1, &plan) == RDNA4IO_R_DST_OOB &&
          rdna4io_plan_texture_load(UINT64_MAX, 0, UINT64_MAX, 0, UINT64_MAX, 4, 1, 1, 1, 0, 0, 1, 16,
                                    0, 0, 0, 0, 0, UINT64_MAX, 4, 1, &plan) == RDNA4IO_R_BAD_LAYOUT &&
          rdna4io_plan_texture_load(4096, UINT64_MAX - 8, 64, 0, 16, 16, 1, 1, 1, 0, 0, 0, 4,
                                    0, 0, 0, 0, 0, 4, 4, 1, &plan) == RDNA4IO_R_SRC_OOB,
          "OOB and overflow refused");
}

static void test_io_cb_state_machine(void) {
    rdna4io_cb_state_t s;
    rdna4io_cb_init(&s);
    CHECK("io cb initial state is RECORDING/PENDING",
          s.phase == RDNA4IO_PHASE_RECORDING && s.status == RDNA4IO_STATUS_PENDING && s.cmd_count == 0,
          "phase=%d status=%d", s.phase, s.status);

    // Exactly 8,192 commands succeed; 8,193rd fails with RDNA4IO_R_COMMAND_LIMIT.
    int all_8192_ok = 1;
    for (uint32_t i = 0; i < RDNA4_IO_MAX_COMMANDS; i++) {
        if (rdna4io_cb_add_command(&s) != RDNA4IO_OK) { all_8192_ok = 0; break; }
    }
    CHECK("io cb accepts exactly 8192 commands",
          all_8192_ok && s.cmd_count == 8192u && s.sticky_reason == RDNA4IO_OK,
          "cmd_count=%u", s.cmd_count);
    CHECK("io cb rejects 8193rd command and latches error",
          rdna4io_cb_add_command(&s) == RDNA4IO_R_COMMAND_LIMIT &&
          s.cmd_count == 8192u &&
          s.sticky_reason == RDNA4IO_R_COMMAND_LIMIT &&
          s.sticky_mtl_err == RDNA4IO_ERR_INTERNAL,
          "reason=%s", rdna4io_reason_text(s.sticky_reason));

    // Sticky error prevents execution on begin_run and yields ERROR status.
    CHECK("io cb with sticky error fails cleanly on commit + begin_run",
          rdna4io_cb_commit(&s) == 1 &&
          rdna4io_cb_commit(&s) == 0 &&
          rdna4io_cb_add_command(&s) == RDNA4IO_R_ALREADY_COMMITTED &&
          rdna4io_cb_begin_run(&s) == 0 &&
          s.phase == RDNA4IO_PHASE_TERMINAL &&
          s.status == RDNA4IO_STATUS_ERROR &&
          rdna4io_cb_request_cancel(&s) == 0 &&
          s.status == RDNA4IO_STATUS_ERROR,
          "status=%d", s.status);

    // Normal happy path: enqueue -> add -> commit -> begin_run -> finish(OK) -> COMPLETE.
    rdna4io_cb_init(&s);
    CHECK("io cb normal lifecycle to COMPLETE",
          rdna4io_cb_enqueue(&s) == 1 &&
          rdna4io_cb_enqueue(&s) == 0 &&
          rdna4io_cb_add_command(&s) == RDNA4IO_OK &&
          rdna4io_cb_commit(&s) == 1 &&
          rdna4io_cb_begin_run(&s) == 1 &&
          s.phase == RDNA4IO_PHASE_RUNNING &&
          rdna4io_cb_finish(&s, RDNA4IO_OK, RDNA4IO_ERR_NONE) == 1 &&
          rdna4io_cb_finish(&s, RDNA4IO_R_GPU_FAILED, RDNA4IO_ERR_INTERNAL) == 0 &&
          s.status == RDNA4IO_STATUS_COMPLETE &&
          rdna4io_cb_request_cancel(&s) == 0 &&
          s.status == RDNA4IO_STATUS_COMPLETE,
          "COMPLETE locked");

    // tryCancel before commit.
    rdna4io_cb_init(&s);
    CHECK("io cb tryCancel before commit blocks encoding and completes CANCELLED",
          rdna4io_cb_add_command(&s) == RDNA4IO_OK &&
          rdna4io_cb_request_cancel(&s) == 1 &&
          s.status == RDNA4IO_STATUS_CANCELLED &&
          rdna4io_cb_add_command(&s) == RDNA4IO_R_CANCELLED &&
          rdna4io_cb_commit(&s) == 1 &&
          rdna4io_cb_begin_run(&s) == 0 &&
          s.phase == RDNA4IO_PHASE_TERMINAL &&
          s.status == RDNA4IO_STATUS_CANCELLED &&
          s.sticky_reason == RDNA4IO_R_CANCELLED,
          "status=%d reason=%s", s.status, rdna4io_reason_text(s.sticky_reason));

    // tryCancel mid-run.
    rdna4io_cb_init(&s);
    CHECK("io cb tryCancel while running sets should_stop and finishes CANCELLED",
          rdna4io_cb_commit(&s) == 1 &&
          rdna4io_cb_begin_run(&s) == 1 &&
          !rdna4io_cb_should_stop(&s) &&
          rdna4io_cb_request_cancel(&s) == 1 &&
          rdna4io_cb_should_stop(&s) &&
          rdna4io_cb_finish(&s, RDNA4IO_R_CANCELLED, RDNA4IO_ERR_INTERNAL) == 1 &&
          s.status == RDNA4IO_STATUS_CANCELLED,
          "mid-run cancel ok");

    // Sticky first-error preservation.
    rdna4io_cb_init(&s);
    rdna4io_cb_latch_error(&s, RDNA4IO_ERR_URL_INVALID, RDNA4IO_R_SRC_OOB);
    rdna4io_cb_latch_error(&s, RDNA4IO_ERR_INTERNAL, RDNA4IO_R_DST_OOB);
    CHECK("io cb preserves first latched error",
          s.sticky_reason == RDNA4IO_R_SRC_OOB && s.sticky_mtl_err == RDNA4IO_ERR_URL_INVALID,
          "first=%s", rdna4io_reason_text(s.sticky_reason));
}

static void test_io_queue_scheduler(void) {
    rdna4io_qsched_t q;

    // 1. Serial queue enforces strict FIFO even with out-of-order commit.
    rdna4io_qsched_init(&q, 1, 4);
    uint64_t s1 = rdna4io_qsched_reserve(&q, 0);
    uint64_t s2 = rdna4io_qsched_reserve(&q, 1);
    CHECK("serial queue holds s2 until earlier enqueued s1 commits and completes",
          q.max_concurrency == 1u &&
          rdna4io_qsched_pick(&q) == 0 &&
          rdna4io_qsched_mark_committed(&q, s1) == 1 &&
          rdna4io_qsched_pick(&q) == s1 &&
          rdna4io_qsched_pick(&q) == 0 &&
          rdna4io_qsched_complete(&q, s1) == 1 &&
          rdna4io_qsched_pick(&q) == s2 &&
          rdna4io_qsched_complete(&q, s2) == 1 &&
          q.count == 0 && q.active_workers == 0,
          "serial FIFO verified");
    rdna4io_qsched_destroy(&q);

    // 2. Concurrent queue runs up to max_concurrency in parallel and respects enqueueBarrier.
    rdna4io_qsched_init(&q, 0, 2);
    uint64_t a = rdna4io_qsched_reserve(&q, 1);
    uint64_t b = rdna4io_qsched_reserve(&q, 1);
    uint64_t c = rdna4io_qsched_reserve(&q, 1);
    rdna4io_qsched_barrier(&q);
    uint64_t d = rdna4io_qsched_reserve(&q, 1);
    uint64_t p1 = rdna4io_qsched_pick(&q);
    uint64_t p2 = rdna4io_qsched_pick(&q);
    uint64_t p3 = rdna4io_qsched_pick(&q);
    CHECK("concurrent queue caps active_workers at max_concurrency=2",
          p1 == a && p2 == b && p3 == 0 && q.active_workers == 2u,
          "p1=%llu p2=%llu p3=%llu", (unsigned long long)p1, (unsigned long long)p2, (unsigned long long)p3);
    CHECK("post-barrier slot d stays blocked until a,b,c all complete",
          rdna4io_qsched_complete(&q, a) == 1 &&
          rdna4io_qsched_pick(&q) == c &&
          rdna4io_qsched_pick(&q) == 0 &&
          rdna4io_qsched_complete(&q, b) == 1 &&
          rdna4io_qsched_pick(&q) == 0 &&
          rdna4io_qsched_complete(&q, c) == 1 &&
          rdna4io_qsched_pick(&q) == d &&
          rdna4io_qsched_complete(&q, d) == 1 &&
          q.count == 0 && q.active_workers == 0,
          "enqueueBarrier epoch enforced");

    // 3. Cancelling an uncommitted pre-barrier slot unblocks post-barrier work (no deadlock).
    uint64_t x = rdna4io_qsched_reserve(&q, 0);
    rdna4io_qsched_barrier(&q);
    uint64_t y = rdna4io_qsched_reserve(&q, 1);
    CHECK("cancel pending pre-barrier slot unblocks post-barrier slot",
          rdna4io_qsched_pick(&q) == 0 &&
          rdna4io_qsched_cancel_pending(&q, x) == 1 &&
          rdna4io_qsched_cancel_pending(&q, x) == 0 &&
          rdna4io_qsched_pick(&q) == y &&
          rdna4io_qsched_cancel_pending(&q, y) == 0 &&
          rdna4io_qsched_complete(&q, y) == 1 &&
          q.count == 0,
          "cancel unblocks barrier");

    // 4. Non-head cancelled slot (done=1 while earlier slot is active, not compacted) MUST reject
    //    subsequent mark_committed() (prevents _committedStrong retain cycle on tryCancel vs commit race).
    uint64_t head = rdna4io_qsched_reserve(&q, 1);
    uint64_t tail = rdna4io_qsched_reserve(&q, 0);
    CHECK("non-head cancelled slot rejects subsequent mark_committed",
          rdna4io_qsched_pick(&q) == head &&
          rdna4io_qsched_cancel_pending(&q, tail) == 1 &&
          q.count == 2 &&
          rdna4io_qsched_mark_committed(&q, tail) == 0 &&
          rdna4io_qsched_mark_committed(&q, head) == 0 &&
          rdna4io_qsched_pick(&q) == 0 &&
          rdna4io_qsched_complete(&q, head) == 1 &&
          q.count == 0 && q.active_workers == 0,
          "done/running slots refuse mark_committed");
    rdna4io_qsched_destroy(&q);
}

// Simulated allocation refcounts for verifying snapshot retention and zero-leak retirement.
typedef struct {
    int refs[8];
} mock_refs_t;

static void mock_release(uintptr_t key, void *ctx) {
    mock_refs_t *m = (mock_refs_t *)ctx;
    if (key >= 1 && key <= 8) m->refs[key - 1]--;
}

static void test_residency_set(void) {
    rdna4rs_set_t rs;
    rdna4rs_diff_t diff = {0};

    CHECK("residency capacity validation",
          rdna4rs_validate_capacity(0) &&
          rdna4rs_validate_capacity(64) &&
          rdna4rs_validate_capacity(RDNA4RS_MAX_INITIAL_CAPACITY) &&
          !rdna4rs_validate_capacity((uint64_t)RDNA4RS_MAX_INITIAL_CAPACITY + 1u) &&
          !rdna4rs_validate_capacity(UINT64_MAX) &&
          !rdna4rs_init(&rs, UINT64_MAX),
          "capacity bounds checked");

    CHECK("residency init empty",
          rdna4rs_init(&rs, 4) &&
          rdna4rs_count(&rs) == 0 &&
          rdna4rs_allocated_size(&rs) == 0 &&
          rdna4rs_generation(&rs) == 0 &&
          !rdna4rs_contains(&rs, 1),
          "empty init ok");

    // Staged additions are invisible until commit.
    CHECK("residency staged adds invisible before commit",
          rdna4rs_add(&rs, 1, 4096) &&
          rdna4rs_add(&rs, 2, 8192) &&
          rdna4rs_add(&rs, 1, 4096) && // duplicate dedup
          rdna4rs_count(&rs) == 0 &&
          rdna4rs_allocated_size(&rs) == 0 &&
          !rdna4rs_contains(&rs, 1) && !rdna4rs_contains(&rs, 2),
          "staged isolated");

    // Add + remove before commit cancels pending add cleanly.
    int rem_pending = 0;
    CHECK("residency add+remove before commit cancels pending add",
          rdna4rs_add(&rs, 3, 16384) &&
          rdna4rs_remove(&rs, 3, &rem_pending) && rem_pending == 1 &&
          rdna4rs_commit(&rs, &diff) &&
          diff.added_count == 2 && diff.removed_count == 0 &&
          rdna4rs_count(&rs) == 2 &&
          rdna4rs_allocated_size(&rs) == 12288 &&
          rdna4rs_generation(&rs) == 1 &&
          rdna4rs_contains(&rs, 1) && rdna4rs_contains(&rs, 2) && !rdna4rs_contains(&rs, 3),
          "count=%zu size=%llu gen=%llu", rdna4rs_count(&rs),
          (unsigned long long)rdna4rs_allocated_size(&rs), (unsigned long long)rdna4rs_generation(&rs));
    rdna4rs_diff_destroy(&diff);

    // Remove + re-add of already-committed allocation before commit preserves it with zero diff.
    CHECK("residency remove+re-add before commit preserves committed member",
          rdna4rs_remove(&rs, 1, &rem_pending) && rem_pending == 0 &&
          rdna4rs_contains(&rs, 1) &&
          rdna4rs_add(&rs, 1, 4096) &&
          rdna4rs_commit(&rs, &diff) &&
          diff.added_count == 0 && diff.removed_count == 0 &&
          rdna4rs_count(&rs) == 2 && rdna4rs_contains(&rs, 1),
          "preserved");
    rdna4rs_diff_destroy(&diff);

    // removeAllAllocations + re-add of one existing + one new before commit.
    rdna4rs_remove_all(&rs);
    CHECK("residency removeAll invisible before commit",
          rdna4rs_count(&rs) == 2 && rdna4rs_contains(&rs, 1) && rdna4rs_contains(&rs, 2),
          "still 2 before commit");
    CHECK("residency removeAll + add(1) + add(4) + commit drops only 2 and adds only 4",
          rdna4rs_add(&rs, 1, 4096) &&
          rdna4rs_add(&rs, 4, 32768) &&
          rdna4rs_commit(&rs, &diff) &&
          diff.added_count == 1 && diff.added[0] == 4 &&
          diff.removed_count == 1 && diff.removed[0] == 2 &&
          rdna4rs_count(&rs) == 2 &&
          rdna4rs_allocated_size(&rs) == 36864 &&
          rdna4rs_contains(&rs, 1) && !rdna4rs_contains(&rs, 2) && rdna4rs_contains(&rs, 4),
          "count=%zu size=%llu", rdna4rs_count(&rs), (unsigned long long)rdna4rs_allocated_size(&rs));
    rdna4rs_diff_destroy(&diff);

    // requestResidency / endResidency state toggle.
    rdna4rs_request_residency(&rs);
    int req_on = rs.resident_requested;
    rdna4rs_end_residency(&rs);
    CHECK("residency requestResidency / endResidency toggle",
          req_on == 1 && rs.resident_requested == 0,
          "on=%d off=%d", req_on, rs.resident_requested);

    // Overflow-safe size accounting saturates without UB.
    CHECK("residency size accounting saturates safely on uint64 overflow",
          rdna4rs_add(&rs, 5, UINT64_MAX - 10) &&
          rdna4rs_commit(&rs, &diff) &&
          rdna4rs_allocated_size(&rs) == UINT64_MAX,
          "size=0x%llx", (unsigned long long)rdna4rs_allocated_size(&rs));
    rdna4rs_diff_destroy(&diff);
    rdna4rs_destroy(&rs);

    // Full snapshot lifetime & in-flight command-buffer retention simulation (no UAF, no leaks).
    mock_refs_t refs;
    memset(&refs, 0, sizeof refs);
    rdna4rs_init(&rs, 0);
    // App creates A(1), B(2), C(3).
    refs.refs[0] = 1; refs.refs[1] = 1; refs.refs[2] = 1;
    // Add A, B and commit -> snapshot S1 retains A, B.
    rdna4rs_add(&rs, 1, 1024);
    rdna4rs_add(&rs, 2, 2048);
    rdna4rs_commit(&rs, &diff);
    rdna4rs_diff_destroy(&diff);
    rdna4rs_snapshot_t *s1 = rdna4rs_snapshot_create(&rs);
    for (size_t i = 0; i < s1->count; i++) refs.refs[s1->entries[i].key - 1]++;
    // CB1 attaches S1 (refcount on S1 becomes 2: set + CB1).
    rdna4rs_snapshot_t *cb1_snap = rdna4rs_snapshot_retain(s1);
    // App drops its own strong refs to A and B!
    refs.refs[0]--; refs.refs[1]--;
    CHECK("residency S1 keeps A and B alive after app releases them",
          refs.refs[0] == 1 && refs.refs[1] == 1 && s1->refs == 2,
          "refs A=%d B=%d S1.refs=%u", refs.refs[0], refs.refs[1], s1->refs);

    // App removes A, adds C, commits S2 (set drops its ref to S1, creates S2 retaining B, C),
    // CB2 attaches S2, app drops C.
    rdna4rs_remove(&rs, 1, NULL);
    rdna4rs_add(&rs, 3, 4096);
    rdna4rs_commit(&rs, &diff);
    rdna4rs_diff_destroy(&diff);
    rdna4rs_snapshot_release(s1, mock_release, &refs); // set releases S1; CB1 still holds S1!
    rdna4rs_snapshot_t *s2 = rdna4rs_snapshot_create(&rs);
    for (size_t i = 0; i < s2->count; i++) refs.refs[s2->entries[i].key - 1]++;
    rdna4rs_snapshot_t *cb2_snap = rdna4rs_snapshot_retain(s2);
    refs.refs[2]--; // app drops C
    CHECK("residency A stays alive while CB1 is in-flight even after remove+commit",
          refs.refs[0] == 1 && refs.refs[1] == 2 && refs.refs[2] == 1 && cb1_snap->refs == 1,
          "refs A=%d B=%d C=%d", refs.refs[0], refs.refs[1], refs.refs[2]);

    // CB1 completes and releases its snapshot -> A is reclaimed immediately (0), B/C stay live.
    int s1_freed = rdna4rs_snapshot_release(cb1_snap, mock_release, &refs);
    CHECK("residency CB1 retirement reclaims removed A immediately without affecting B/C",
          s1_freed == 1 && refs.refs[0] == 0 && refs.refs[1] == 1 && refs.refs[2] == 1,
          "refs A=%d B=%d C=%d", refs.refs[0], refs.refs[1], refs.refs[2]);

    // Set clears & commits empty S3; CB2 still holds S2 keeping B and C alive.
    rdna4rs_remove_all(&rs);
    rdna4rs_commit(&rs, &diff);
    rdna4rs_diff_destroy(&diff);
    rdna4rs_snapshot_release(s2, mock_release, &refs); // set drops S2
    CHECK("residency CB2 keeps B and C alive after removeAll+commit",
          refs.refs[1] == 1 && refs.refs[2] == 1,
          "refs B=%d C=%d", refs.refs[1], refs.refs[2]);

    // CB2 completes -> B and C drop to 0 (zero leaks!).
    int s2_freed = rdna4rs_snapshot_release(cb2_snap, mock_release, &refs);
    CHECK("residency CB2 retirement drops B and C to 0 (zero leaks)",
          s2_freed == 1 && refs.refs[0] == 0 && refs.refs[1] == 0 && refs.refs[2] == 0,
          "refs A=%d B=%d C=%d", refs.refs[0], refs.refs[1], refs.refs[2]);
    rdna4rs_destroy(&rs);
}

static void test_source_pins(const char *src_dir, const char *asm_dir) {
    char *io_inc = slurp(src_dir, "garnet_sparse_io.m");
    char *rs_inc = slurp(src_dir, "garnet_resources.m");
    char *cq_inc = slurp(src_dir, "garnet_commands.m");
    char *cb_inc = slurp(src_dir, "garnet_commands.m");
    char *dev_m  = slurp(asm_dir, "GarnetDevice.m");

    CHECK("source files and assembled GarnetDevice.m readable",
          io_inc && rs_inc && cq_inc && cb_inc && dev_m,
          "src=%s asm=%s", src_dir, asm_dir);
    if (!io_inc || !rs_inc || !cq_inc || !cb_inc || !dev_m) goto out;

    CHECK("pin: io_queue_impl documents Wave 2B integration & factory selectors",
          strstr(io_inc, "newIOFileHandleWithURL:(NSURL *)url error:") != NULL &&
          strstr(io_inc, "newIOFileHandleWithURL:(NSURL *)url compressionMethod:") != NULL &&
          strstr(io_inc, "newIOCommandQueueWithDescriptor:(id)descriptor error:") != NULL &&
          strstr(io_inc, "RDNA4_IO_MAX_COMMANDS == RDNA4DC_IO_MAX_COMMANDS") != NULL,
          "Wave 2B header + static assert");

    CHECK("pin: io_queue_impl enforces compression check, 8192 limit, pread, GPU lock, events, cancel",
          strstr(io_inc, "rdna4io_check_compression(") != NULL &&
          strstr(io_inc, "rdna4io_plan_texture_load(") != NULL &&
          strstr(io_inc, "rdna4io_cb_add_command(") != NULL &&
          strstr(io_inc, "pread(_fd,") != NULL &&
          strstr(io_inc, "rdna4_io_gpu_mu") != NULL &&
          strstr(io_inc, "rdna4_event_set_value(") != NULL &&
          strstr(io_inc, "rdna4_event_fail_signals(") != NULL &&
          strstr(io_inc, "rdna4_schedule_kick()") != NULL &&
          strstr(io_inc, "- (void)tryCancel") != NULL &&
          strstr(io_inc, "RDNA4_DNR(RDNA4IOFileHandle)") != NULL &&
          strstr(io_inc, "RDNA4_DNR(RDNA4IOCommandQueue)") != NULL &&
          strstr(io_inc, "RDNA4_DNR(RDNA4IOCommandBuffer)") != NULL,
          "IO queue wiring complete");

    // Pin verifier-audited lifecycle, ARC ownership, and unresolved-staging quarantine invariants.
    {
        const char *fin = strstr(io_inc, "- (void)rdna4IOFinishWithReason:");
        const char *fin_end = fin ? strstr(fin, "\n- (void)commit") : NULL;
        const char *st_write = fin ? strstr(fin, "rdna4IOWriteStatus:") : NULL;
        const char *cb_fin = fin ? strstr(fin, "rdna4io_cb_finish(&_state,") : NULL;
        CHECK("pin: rdna4IOFinishWithReason flushes status buffer BEFORE publishing TERMINAL",
              fin != NULL && fin_end != NULL && st_write != NULL && cb_fin != NULL &&
              st_write < cb_fin && cb_fin < fin_end &&
              strstr(io_inc, "_finishing = YES;") != NULL,
              "single-finalizer before TERMINAL");
        CHECK("pin: all rdna4_oneshot sites track &unresolved and skip staging release on hang",
              strstr(io_inc, "}, NULL);") == NULL &&
              strstr(io_inc, "}, &unresolved);") != NULL &&
              strstr(io_inc, "if (!unresolved) [_q rdna4IOReleaseStaging:stg customScratch:custom];") != NULL,
              "unresolved staging stays quarantined");
        CHECK("pin: newScratchBufferWithMinimumSize: transfers +1 ARC ownership",
              strstr(io_inc, "(__bridge_transfer id)((void *(*)(id, SEL, NSUInteger))objc_msgSend)") != NULL,
              "no +1 ARC leak on scratchBufferAllocator");
    }

    CHECK("pin: residency_set_impl documents Wave 2B integration & accepts sparse heaps/textures",
          strstr(rs_inc, "newResidencySetWithDescriptor:(id)descriptor error:") != NULL &&
          strstr(rs_inc, "rdna4rs_commit(&_state, &diff)") != NULL &&
          strstr(rs_inc, "@implementation RDNA4ResidencySnapshot") != NULL &&
          strstr(rs_inc, "@implementation RDNA4ResidencySet") != NULL &&
          strstr(rs_inc, "objc_getClass(\"RDNA4SparseHeap\")") != NULL &&
          strstr(rs_inc, "sparse textures/heaps are not supported") == NULL &&
          strstr(rs_inc, "RDNA4_DNR(RDNA4ResidencySet)") != NULL,
          "residency set impl complete");

    CHECK("pin: command_queue_impl wires residency set management and snapshot attachment",
          strstr(cq_inc, "- (void)addResidencySet:(id)residencySet") != NULL &&
          strstr(cq_inc, "- (void)addResidencySets:(const id __unsafe_unretained *)residencySets count:") != NULL &&
          strstr(cq_inc, "- (void)removeResidencySet:(id)residencySet") != NULL &&
          strstr(cq_inc, "- (void)removeResidencySets:(const id __unsafe_unretained *)residencySets count:") != NULL &&
          strstr(cq_inc, "- (NSArray *)rdna4ResidencySetSnapshots") != NULL &&
          strstr(cq_inc, "[cb rdna4AttachQueueResidencySnapshots];") != NULL,
          "queue residency wiring");

    CHECK("pin: command_buffer_impl wires useResidencySet, BDA registry merge, and retirement release",
          strstr(cb_inc, "- (void)useResidencySet:(id)residencySet") != NULL &&
          strstr(cb_inc, "- (void)useResidencySets:(const id __unsafe_unretained *)residencySets count:") != NULL &&
          strstr(cb_inc, "[self rdna4AttachQueueResidencySnapshots];") != NULL &&
          strstr(cb_inc, "rdna4AddBuffersToSet:resources device:dev") != NULL &&
          strstr(cb_inc, "[_residencySnapshots removeAllObjects];") != NULL &&
          strstr(cb_inc, "[_residencyHeapResources removeAllObjects];") != NULL,
          "command buffer residency wiring");

    // Verify rdna4PoolDone owns residency release (so unresolved/quarantined CBs never UAF).
    {
        const char *pd = strstr(cb_inc, "- (void)rdna4PoolDone {");
        const char *pd_end = pd ? strstr(pd, "\n}\n") : NULL;
        const char *rs_rel = pd ? strstr(pd, "[_residencySnapshots removeAllObjects];") : NULL;
        const char *hp_rel = pd ? strstr(pd, "[_residencyHeapResources removeAllObjects];") : NULL;
        CHECK("pin: residency snapshots released inside rdna4PoolDone",
              pd != NULL && pd_end != NULL && rs_rel != NULL && hp_rel != NULL &&
              rs_rel < pd_end && hp_rel < pd_end,
              "retirement-gated release verified");
    }

    CHECK("pin: assembled GarnetDevice.m includes queue and command-buffer residency hooks",
          strstr(dev_m, "- (void)addResidencySet:(id)residencySet") != NULL &&
          strstr(dev_m, "- (void)useResidencySet:(id)residencySet") != NULL &&
          strstr(dev_m, "rdna4AddBuffersToSet:resources device:dev") != NULL,
          "assembled device verified");

out:
    free(io_inc);
    free(rs_inc);
    free(cq_inc);
    free(cb_inc);
    free(dev_m);
}

int main(int argc, char **argv) {
    test_io_compression_and_desc();
    test_io_bounds_and_texture_plan();
    test_io_cb_state_machine();
    test_io_queue_scheduler();
    test_residency_set();
    if (argc >= 3 && argv[1][0] && argv[2][0]) test_source_pins(argv[1], argv[2]);
    printf("%s test-io-residency (%d checks, %d failed)\n", fails ? "FAIL" : "PASS", checks, fails);
    return fails ? 1 : 0;
}
