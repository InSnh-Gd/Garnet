// test-fencewait.c: host test of garnet_fencewait.h (bounded-slice fence wait,
// sub-ms wakeup, non-blocking serial completion queue) plus source pins on
// garnet_commands.m and garnet_commands.m.
// Build/run: cc -O1 -Wall -Wextra -Werror -pthread -o /tmp/test-fencewait test-fencewait.c && /tmp/test-fencewait .
#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include "garnet_fencewait.h"

static int fails;
#define CHECK(name, cond, ...) do { \
    int ok_ = (cond); \
    printf("%s %s: ", ok_ ? "ok  " : "FAIL", name); \
    printf(__VA_ARGS__); \
    printf("\n"); \
    if (!ok_) fails++; \
} while (0)

static uint64_t mono_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
}

typedef struct {
    int calls;
    int succeed_at;
    int fail_at;
    int fail_rc;
    uint64_t last_ns;
    int pending_count;
    int pending_slices[8];
} mock_ctx;

static int mock_wait(void *p, uint64_t ns) {
    mock_ctx *c = (mock_ctx *)p;
    c->calls++;
    c->last_ns = ns;
    if (c->fail_at && c->calls == c->fail_at) return c->fail_rc;
    if (c->succeed_at && c->calls >= c->succeed_at) return 0;
    return RDNA4FW_TIMEOUT;
}

static void mock_pending(void *p, int slices) {
    mock_ctx *c = (mock_ctx *)p;
    if (c->pending_count < 8) c->pending_slices[c->pending_count] = slices;
    c->pending_count++;
}

// ---- sub-ms wakeup latency fixture ----
typedef struct {
    pthread_mutex_t mu;
    pthread_cond_t cv;
    int signaled;
    uint64_t t_signal;
    uint64_t t_wake;
} fast_fence_t;

static int fast_wait_slice(void *p, uint64_t ns) {
    (void)ns;
    fast_fence_t *f = (fast_fence_t *)p;
    pthread_mutex_lock(&f->mu);
    while (!f->signaled) pthread_cond_wait(&f->cv, &f->mu);
    f->t_wake = mono_ns();
    pthread_mutex_unlock(&f->mu);
    return 0;
}

static void *fast_gpu_signal(void *p) {
    fast_fence_t *f = (fast_fence_t *)p;
    struct timespec ts = { 0, 200000 };   // 200 us simulated GPU work
    nanosleep(&ts, NULL);
    pthread_mutex_lock(&f->mu);
    f->signaled = 1;
    f->t_signal = mono_ns();
    pthread_cond_signal(&f->cv);
    pthread_mutex_unlock(&f->mu);
    return NULL;
}

// ---- non-blocking serial completion queue fixture ----
typedef struct {
    pthread_mutex_t mu;
    pthread_cond_t cv;
    int entered;
    int release;
    int release_rc;
} gate_t;

typedef struct {
    pthread_mutex_t cq_mu;
    int order[2];
    int count;
} serial_cq_t;

typedef struct {
    int id;
    gate_t *gate;
    serial_cq_t *cq;
    int result;
    int slices;
} waiter_arg_t;

static int gated_wait_slice(void *p, uint64_t ns) {
    (void)ns;
    gate_t *g = (gate_t *)p;
    pthread_mutex_lock(&g->mu);
    g->entered = 1;
    pthread_cond_broadcast(&g->cv);
    while (!g->release) pthread_cond_wait(&g->cv, &g->mu);
    int rc = g->release_rc;
    pthread_mutex_unlock(&g->mu);
    return rc;
}

static void *concurrent_waiter(void *p) {
    waiter_arg_t *a = (waiter_arg_t *)p;
    a->result = rdna4fw_wait_bounded(gated_wait_slice, a->gate, 0,
                                     RDNA4FW_MAX_SLICES, RDNA4FW_SLICE_NS,
                                     NULL, &a->slices);
    pthread_mutex_lock(&a->cq->cq_mu);
    if (a->cq->count < 2) a->cq->order[a->cq->count++] = a->id;
    pthread_mutex_unlock(&a->cq->cq_mu);
    return NULL;
}

static int cmp_u64(const void *a, const void *b) {
    uint64_t x = *(const uint64_t *)a, y = *(const uint64_t *)b;
    return (x > y) - (x < y);
}

static char *slurp(const char *dir, const char *name) {
    char path[1024];
    snprintf(path, sizeof path, "%s/%s", dir, name);
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return NULL; }
    long n = ftell(f);
    if (n < 0 || fseek(f, 0, SEEK_SET) != 0) { fclose(f); return NULL; }
    char *b = (char *)malloc((size_t)n + 1);
    if (!b) { fclose(f); return NULL; }
    size_t r = fread(b, 1, (size_t)n, f);
    fclose(f);
    b[r] = 0;
    return b;
}

int main(int argc, char **argv) {
    const char *dir = argc > 1 ? argv[1] : ".";

    // ---- 1. constants & bounded-slice policy ----
    CHECK("constants: 30 x 2 s = 60 s",
          RDNA4FW_SLICE_NS == 2000000000ULL && RDNA4FW_MAX_SLICES == 30 &&
          (uint64_t)RDNA4FW_MAX_SLICES * RDNA4FW_SLICE_NS == 60000000000ULL &&
          RDNA4FW_TIMEOUT == 2,
          "slice=%llu max=%d total=%llu timeout=%d",
          (unsigned long long)RDNA4FW_SLICE_NS, RDNA4FW_MAX_SLICES,
          (unsigned long long)((uint64_t)RDNA4FW_MAX_SLICES * RDNA4FW_SLICE_NS),
          RDNA4FW_TIMEOUT);

    {
        mock_ctx c = { .succeed_at = 1 };
        int slices = -1;
        int r = rdna4fw_wait_bounded(mock_wait, &c, 0, RDNA4FW_MAX_SLICES, RDNA4FW_SLICE_NS, mock_pending, &slices);
        CHECK("immediate success",
              r == 0 && slices == 1 && c.calls == 1 && c.last_ns == RDNA4FW_SLICE_NS && c.pending_count == 0,
              "r=%d slices=%d calls=%d pending=%d", r, slices, c.calls, c.pending_count);
    }

    {
        mock_ctx c = { .fail_at = 1, .fail_rc = -4 };   // VK_ERROR_DEVICE_LOST
        int slices = -1;
        int r = rdna4fw_wait_bounded(mock_wait, &c, 0, RDNA4FW_MAX_SLICES, RDNA4FW_SLICE_NS, mock_pending, &slices);
        CHECK("device lost stops immediately",
              r == -4 && slices == 1 && c.calls == 1 && c.pending_count == 0,
              "r=%d slices=%d calls=%d pending=%d", r, slices, c.calls, c.pending_count);
    }

    {
        mock_ctx c = { .succeed_at = 4 };
        int slices = -1;
        int r = rdna4fw_wait_bounded(mock_wait, &c, 0, RDNA4FW_MAX_SLICES, RDNA4FW_SLICE_NS, mock_pending, &slices);
        CHECK("multi-slice recovery logs pending at 1 and 2",
              r == 0 && slices == 4 && c.calls == 4 && c.pending_count == 2 &&
              c.pending_slices[0] == 1 && c.pending_slices[1] == 2,
              "r=%d slices=%d pending=%d (%d,%d)", r, slices, c.pending_count,
              c.pending_slices[0], c.pending_slices[1]);
    }

    {
        mock_ctx c = { 0 };
        int slices = -1;
        int r = rdna4fw_wait_bounded(mock_wait, &c, 0, RDNA4FW_MAX_SLICES, RDNA4FW_SLICE_NS, mock_pending, &slices);
        CHECK("60 s timeout exhausts 30 slices and logs pending at 1, 2, 10",
              r == RDNA4FW_TIMEOUT && slices == 30 && c.calls == 30 && c.pending_count == 3 &&
              c.pending_slices[0] == 1 && c.pending_slices[1] == 2 && c.pending_slices[2] == 10,
              "r=%d slices=%d calls=%d pending=%d (%d,%d,%d)", r, slices, c.calls, c.pending_count,
              c.pending_slices[0], c.pending_slices[1], c.pending_slices[2]);
    }

    {
        // RDNA4M_TEST_TIMEOUT_MS continuation: first 1 ms slice timed out (start_slices = 1),
        // remaining 29 slices of 2 s bring total to 30.
        mock_ctx c = { 0 };
        int slices = 1;
        int r = rdna4fw_wait_bounded(mock_wait, &c, slices, RDNA4FW_MAX_SLICES, RDNA4FW_SLICE_NS, NULL, &slices);
        CHECK("test-timeout continuation runs 29 slices to total 30",
              r == RDNA4FW_TIMEOUT && slices == 30 && c.calls == 29 && c.last_ns == RDNA4FW_SLICE_NS,
              "r=%d slices=%d calls=%d", r, slices, c.calls);
    }

    {
        int slices = -1;
        int r = rdna4fw_wait_bounded(NULL, NULL, 0, RDNA4FW_MAX_SLICES, RDNA4FW_SLICE_NS, NULL, &slices);
        CHECK("null wait_fn fails closed", r == RDNA4FW_TIMEOUT && slices == 0, "r=%d slices=%d", r, slices);
    }

    // ---- 2. sub-ms wakeup latency (no 2 ms polling floor) ----
    {
        enum { N = 15 };
        uint64_t wake_overhead_ns[N];
        for (int i = 0; i < N; i++) {
            fast_fence_t f = { .mu = PTHREAD_MUTEX_INITIALIZER, .cv = PTHREAD_COND_INITIALIZER };
            pthread_t gpu;
            pthread_create(&gpu, NULL, fast_gpu_signal, &f);
            int slices = 0;
            int r = rdna4fw_wait_bounded(fast_wait_slice, &f, 0, RDNA4FW_MAX_SLICES, RDNA4FW_SLICE_NS, NULL, &slices);
            pthread_join(gpu, NULL);
            pthread_cond_destroy(&f.cv);
            pthread_mutex_destroy(&f.mu);
            if (r != 0 || slices != 1) fails++;
            wake_overhead_ns[i] = f.t_wake > f.t_signal ? f.t_wake - f.t_signal : 0;
        }
        qsort(wake_overhead_ns, N, sizeof wake_overhead_ns[0], cmp_u64);
        uint64_t med_us = wake_overhead_ns[N / 2] / 1000ULL;
        CHECK("sub-ms wakeup overhead (median < 500 us, well below old 2000 us floor)",
              med_us < 500ULL, "median=%llu us min=%llu us max=%llu us",
              (unsigned long long)med_us,
              (unsigned long long)(wake_overhead_ns[0] / 1000ULL),
              (unsigned long long)(wake_overhead_ns[N - 1] / 1000ULL));
    }

    // ---- 3. stuck fence on concurrent waiter does not block serial completion queue ----
    {
        gate_t stuck = { .mu = PTHREAD_MUTEX_INITIALIZER, .cv = PTHREAD_COND_INITIALIZER };
        gate_t fast = { .mu = PTHREAD_MUTEX_INITIALIZER, .cv = PTHREAD_COND_INITIALIZER, .release = 1, .release_rc = 0 };
        serial_cq_t cq = { .cq_mu = PTHREAD_MUTEX_INITIALIZER };
        waiter_arg_t a1 = { .id = 1, .gate = &stuck, .cq = &cq };
        waiter_arg_t a2 = { .id = 2, .gate = &fast, .cq = &cq };
        pthread_t t1, t2;
        pthread_create(&t1, NULL, concurrent_waiter, &a1);
        pthread_mutex_lock(&stuck.mu);
        while (!stuck.entered) pthread_cond_wait(&stuck.cv, &stuck.mu);
        pthread_mutex_unlock(&stuck.mu);

        // Waiter 1 is blocked inside wait_slice; Waiter 2 must complete and deliver onto cq immediately.
        uint64_t t0 = mono_ns();
        pthread_create(&t2, NULL, concurrent_waiter, &a2);
        pthread_join(t2, NULL);
        uint64_t dt_us = (mono_ns() - t0) / 1000ULL;

        pthread_mutex_lock(&cq.cq_mu);
        int count_while_stuck = cq.count;
        int first_id = cq.order[0];
        pthread_mutex_unlock(&cq.cq_mu);

        // Now release the stuck fence with timeout (exhausting 30 slices).
        pthread_mutex_lock(&stuck.mu);
        stuck.release = 1;
        stuck.release_rc = RDNA4FW_TIMEOUT;
        pthread_cond_broadcast(&stuck.cv);
        pthread_mutex_unlock(&stuck.mu);
        pthread_join(t1, NULL);

        CHECK("stuck fence does not block serial completion queue",
              count_while_stuck == 1 && first_id == 2 && a2.result == 0 && a2.slices == 1 &&
              a1.result == RDNA4FW_TIMEOUT && a1.slices == RDNA4FW_MAX_SLICES &&
              cq.count == 2 && cq.order[1] == 1,
              "fast_us=%llu order=[%d,%d] stuck_slices=%d",
              (unsigned long long)dt_us, cq.order[0], cq.order[1], a1.slices);
        pthread_cond_destroy(&stuck.cv); pthread_mutex_destroy(&stuck.mu);
        pthread_cond_destroy(&fast.cv); pthread_mutex_destroy(&fast.mu);
        pthread_mutex_destroy(&cq.cq_mu);
    }

    // ---- 4. production source pins ----
    char *cb = slurp(dir, "garnet_command_buffer.m");
    char *cq = slurp(dir, "garnet_commands.m");
    CHECK("sources readable", cb != NULL && cq != NULL, "%s", dir);
    if (cb && cq) {
        CHECK("pin: rdna4FencePoll removed from command buffer and queue",
              strstr(cb, "rdna4FencePoll") == NULL && strstr(cq, "rdna4FencePoll") == NULL,
              "no 2 ms polling method");
        const char *fw = strstr(cb, "- (void)rdna4FenceWait:");
        const char *fw_end = fw ? strstr(fw, "\n}\n") : NULL;
        size_t fw_len = (fw && fw_end) ? (size_t)(fw_end - fw) : 0;
        char *fw_body = fw_len ? (char *)malloc(fw_len + 1) : NULL;
        if (fw_body) { memcpy(fw_body, fw, fw_len); fw_body[fw_len] = 0; }
        CHECK("pin: rdna4FenceWait has no dispatch_after",
              fw_body != NULL && strstr(fw_body, "dispatch_after") == NULL,
              "no timer floor in rdna4FenceWait");
        CHECK("pin: concurrent interactive waiter queue + bounded slices",
              strstr(cb, "DISPATCH_QUEUE_CONCURRENT") != NULL &&
              strstr(cb, "QOS_CLASS_USER_INTERACTIVE") != NULL &&
              strstr(cb, "rdna4_fence_wait_queue()") != NULL &&
              fw_body != NULL && strstr(fw_body, "rdna4fw_wait_bounded(") != NULL &&
              strstr(fw_body, "RDNA4FW_MAX_SLICES") != NULL &&
              strstr(fw_body, "RDNA4FW_SLICE_NS") != NULL,
              "concurrent queue + 30x2s slices");
        CHECK("pin: ownership transfer to serial completionQueue (no waiter-thread UAF/race)",
              fw_body != NULL &&
              strstr(fw_body, "CFBridgingRetain(self)") != NULL &&
              strstr(fw_body, "CFBridgingRetain(segment)") != NULL &&
              strstr(fw_body, "CFBridgingRetain(queue)") != NULL &&
              strstr(fw_body, "(__bridge_transfer RDNA4CommandBuffer *)cbRef") != NULL &&
              strstr(fw_body, "(__bridge_transfer RDNA4CommandSegment *)segRef") != NULL &&
              strstr(fw_body, "(__bridge_transfer RDNA4CommandQueue *)qRef") != NULL &&
              strstr(fw_body, "dispatch_async(completionQueue, ^{") != NULL,
              "CFBridgingRetain -> __bridge_transfer on completionQueue");
        CHECK("pin: FENCE PENDING, FENCE STALL, and GARNET_TEST_TIMEOUT_MS preserved",
              strstr(cb, "FENCE PENDING:") != NULL &&
              strstr(cb, "FENCE STALL:") != NULL &&
              fw_body != NULL && strstr(fw_body, "GARNET_TEST_TIMEOUT_MS") != NULL &&
              strstr(fw_body, "RDNA4M_TEST_TIMEOUT_MS") != NULL &&
              strstr(fw_body, "TEST_TIMEOUT:") != NULL,
              "diagnostics and test hook intact");
        const char *iow = strstr(cb, "- (void)rdna4IOWritten:");
        const char *iow_end = iow ? strstr(iow, "\n}\n") : NULL;
        size_t iow_len = (iow && iow_end) ? (size_t)(iow_end - iow) : 0;
        char *iow_body = iow_len ? (char *)malloc(iow_len + 1) : NULL;
        if (iow_body) { memcpy(iow_body, iow, iow_len); iow_body[iow_len] = 0; }
        CHECK("pin: Task A rdna4IOWritten linear IOSurface deferral preserved",
              iow_body != NULL &&
              strstr(iow_body, "[t rdna4IsIOS]") != NULL &&
              strstr(iow_body, "_scheduleAfterPublish = YES;") != NULL &&
              strstr(iow_body, "![t rdna4IOSLinear]") == NULL,
              "Task A intact");
        free(fw_body);
        free(iow_body);
    }
    free(cb);
    free(cq);

    printf(fails ? "FENCEWAIT_FAIL %d\n" : "FENCEWAIT_PASS\n", fails);
    return fails != 0;
}
