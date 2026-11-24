// Pure-C state machine, bounds, command-limit, cancellation, compression, and queue-scheduling
// helpers for Metal 3 Fast Resource Loading (MTLIOFileHandle / MTLIOCommandQueue / MTLIOCommandBuffer).
// Shared by rdna4_io_queue_impl.inc and host C unit tests (test-io-residency.c).
#ifndef RDNA4_IO_QUEUE_H
#define RDNA4_IO_QUEUE_H

#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#define RDNA4_IO_MAX_COMMANDS 8192u
#define RDNA4_IO_MAX_CONCURRENCY 8u
#define RDNA4_IO_DEFAULT_CONCURRENCY 4u
#define RDNA4_IO_STAGING_CHUNK (4u * 1024u * 1024u)

// Metal 3 MTLIOStatus raw values (verified against Apple <Metal/MTLIOCommandBuffer.h>).
enum {
    RDNA4IO_STATUS_PENDING   = 0,
    RDNA4IO_STATUS_CANCELLED = 1,
    RDNA4IO_STATUS_ERROR     = 2,
    RDNA4IO_STATUS_COMPLETE  = 3,
};

// Metal 3 MTLIOError raw values (verified against Apple <Metal/MTLIOCommandQueue.h>).
enum {
    RDNA4IO_ERR_NONE        = 0,
    RDNA4IO_ERR_URL_INVALID = 1,
    RDNA4IO_ERR_INTERNAL    = 2,
};

// Metal 3 MTLIOCommandQueueType & MTLIOPriority raw values.
enum {
    RDNA4IO_QUEUE_CONCURRENT = 0,
    RDNA4IO_QUEUE_SERIAL     = 1,
};

enum {
    RDNA4IO_PRIO_HIGH   = 0,
    RDNA4IO_PRIO_NORMAL = 1,
    RDNA4IO_PRIO_LOW    = 2,
};

// Metal 3 MTLIOCompressionMethod raw values (-1 = uncompressed).
enum {
    RDNA4IO_COMP_NONE     = -1,
    RDNA4IO_COMP_ZLIB     = 0,
    RDNA4IO_COMP_LZFSE    = 1,
    RDNA4IO_COMP_LZ4      = 2,
    RDNA4IO_COMP_LZMA     = 3,
    RDNA4IO_COMP_LZBITMAP = 4,
};

// Detailed internal reason codes for diagnostics and host testing.
typedef enum {
    RDNA4IO_OK = 0,
    RDNA4IO_R_INVALID_URL,
    RDNA4IO_R_UNSUPPORTED_COMPRESSION,
    RDNA4IO_R_INVALID_ARG,
    RDNA4IO_R_ALREADY_COMMITTED,
    RDNA4IO_R_COMMAND_LIMIT,
    RDNA4IO_R_SRC_OOB,
    RDNA4IO_R_DST_OOB,
    RDNA4IO_R_BAD_LAYOUT,
    RDNA4IO_R_READ_FAILED,
    RDNA4IO_R_GPU_FAILED,
    RDNA4IO_R_EVENT_FAILED,
    RDNA4IO_R_CANCELLED,
    RDNA4IO_R_DEVICE_MISMATCH,
} rdna4io_reason_t;

typedef enum {
    RDNA4IO_PHASE_RECORDING = 0,
    RDNA4IO_PHASE_ENQUEUED  = 1,
    RDNA4IO_PHASE_COMMITTED = 2,
    RDNA4IO_PHASE_RUNNING   = 3,
    RDNA4IO_PHASE_TERMINAL  = 4,
} rdna4io_phase_t;

static inline const char *rdna4io_reason_text(rdna4io_reason_t r) {
    switch (r) {
    case RDNA4IO_OK: return "ok";
    case RDNA4IO_R_INVALID_URL: return "invalid or unreadable file URL";
    case RDNA4IO_R_UNSUPPORTED_COMPRESSION: return "unsupported I/O compression method";
    case RDNA4IO_R_INVALID_ARG: return "invalid I/O argument";
    case RDNA4IO_R_ALREADY_COMMITTED: return "cannot encode commands after commit";
    case RDNA4IO_R_COMMAND_LIMIT: return "exceeded Metal 3 limit of 8192 I/O commands per command buffer";
    case RDNA4IO_R_SRC_OOB: return "source handle range exceeds file size or overflows";
    case RDNA4IO_R_DST_OOB: return "destination range exceeds buffer/texture bounds or overflows";
    case RDNA4IO_R_BAD_LAYOUT: return "invalid texture stride, alignment, or format for I/O load";
    case RDNA4IO_R_READ_FAILED: return "file read failed or reached premature EOF";
    case RDNA4IO_R_GPU_FAILED: return "GPU staging upload failed";
    case RDNA4IO_R_EVENT_FAILED: return "waited shared event failed or timed out";
    case RDNA4IO_R_CANCELLED: return "I/O command buffer was cancelled";
    case RDNA4IO_R_DEVICE_MISMATCH: return "resource or handle belongs to a different device";
    default: return "internal I/O error";
    }
}

// Compression negotiation: uncompressed handles succeed; compressed handles are refused
// explicitly (never silently treated as raw bytes).
static inline rdna4io_reason_t rdna4io_check_compression(int method, int *out_mtl_err,
                                                         const char **why) {
    if (out_mtl_err) *out_mtl_err = RDNA4IO_ERR_NONE;
    if (why) *why = NULL;
    if (method == RDNA4IO_COMP_NONE) return RDNA4IO_OK;
    if (out_mtl_err) *out_mtl_err = RDNA4IO_ERR_INTERNAL;
    if (why) {
        switch (method) {
        case RDNA4IO_COMP_ZLIB:     *why = "MTLIOCompressionMethodZlib compressed I/O handles are unsupported"; break;
        case RDNA4IO_COMP_LZFSE:    *why = "MTLIOCompressionMethodLZFSE compressed I/O handles are unsupported"; break;
        case RDNA4IO_COMP_LZ4:      *why = "MTLIOCompressionMethodLZ4 compressed I/O handles are unsupported"; break;
        case RDNA4IO_COMP_LZMA:     *why = "MTLIOCompressionMethodLZMA compressed I/O handles are unsupported"; break;
        case RDNA4IO_COMP_LZBITMAP: *why = "MTLIOCompressionMethodLZBitmap compressed I/O handles are unsupported"; break;
        default:                    *why = "unknown MTLIOCompressionMethod is unsupported"; break;
        }
    }
    return RDNA4IO_R_UNSUPPORTED_COMPRESSION;
}

static inline rdna4io_reason_t rdna4io_check_queue_desc(long type, long priority,
                                                        uint64_t max_in_flight,
                                                        uint32_t *out_concurrency,
                                                        const char **why) {
    if (why) *why = NULL;
    if (type != RDNA4IO_QUEUE_CONCURRENT && type != RDNA4IO_QUEUE_SERIAL) {
        if (why) *why = "invalid MTLIOCommandQueueType";
        return RDNA4IO_R_INVALID_ARG;
    }
    if (priority != RDNA4IO_PRIO_HIGH && priority != RDNA4IO_PRIO_NORMAL &&
        priority != RDNA4IO_PRIO_LOW) {
        if (why) *why = "invalid MTLIOPriority";
        return RDNA4IO_R_INVALID_ARG;
    }
    uint32_t conc = 1u;
    if (type == RDNA4IO_QUEUE_CONCURRENT) {
        if (max_in_flight == 0) conc = RDNA4_IO_DEFAULT_CONCURRENCY;
        else if (max_in_flight > RDNA4_IO_MAX_CONCURRENCY) conc = RDNA4_IO_MAX_CONCURRENCY;
        else conc = (uint32_t)max_in_flight;
    }
    if (out_concurrency) *out_concurrency = conc;
    return RDNA4IO_OK;
}

static inline int rdna4io_range_ok(uint64_t limit, uint64_t offset, uint64_t size) {
    return offset <= limit && size <= limit - offset;
}

static inline rdna4io_reason_t rdna4io_check_load_bytes(uint64_t file_size, uint64_t src_off,
                                                        uint64_t size, int has_dst_ptr) {
    if (size && !has_dst_ptr) return RDNA4IO_R_INVALID_ARG;
    if (!rdna4io_range_ok(file_size, src_off, size)) return RDNA4IO_R_SRC_OOB;
    return RDNA4IO_OK;
}

static inline rdna4io_reason_t rdna4io_check_load_buffer(uint64_t file_size, uint64_t src_off,
                                                         uint64_t dst_len, uint64_t dst_off,
                                                         uint64_t size) {
    if (!rdna4io_range_ok(file_size, src_off, size)) return RDNA4IO_R_SRC_OOB;
    if (!rdna4io_range_ok(dst_len, dst_off, size)) return RDNA4IO_R_DST_OOB;
    return RDNA4IO_OK;
}

typedef struct {
    uint64_t bw, bh, depth;
    uint64_t row_bytes;
    uint64_t row_stride;
    uint64_t img_stride;
    uint64_t src_span;
    uint64_t packed_bytes;
} rdna4io_tex_plan_t;

// Overflow-safe validation and staging layout calculation for loadTexture:...
static inline rdna4io_reason_t rdna4io_plan_texture_load(
    uint64_t file_size, uint64_t src_off,
    uint64_t src_bpr, uint64_t src_bpi,
    uint64_t tex_w, uint64_t tex_h, uint64_t tex_d,
    uint64_t tex_levels, uint64_t tex_layers,
    int is_1d, int is_3d, int is_bc, uint32_t bpp,
    uint64_t slice, uint64_t level,
    uint64_t ox, uint64_t oy, uint64_t oz,
    uint64_t sw, uint64_t sh, uint64_t sd,
    rdna4io_tex_plan_t *out) {
    if (!out || !bpp || !tex_w || !tex_h || !tex_levels || !tex_layers)
        return RDNA4IO_R_BAD_LAYOUT;
    if (level >= tex_levels || slice >= tex_layers) return RDNA4IO_R_DST_OOB;
    uint64_t lw = level < 64 ? (tex_w >> level) : 0; if (!lw) lw = 1;
    uint64_t lh = is_1d ? 1 : (level < 64 ? (tex_h >> level) : 0); if (!lh) lh = 1;
    uint64_t ld = is_3d ? (level < 64 ? (tex_d >> level) : 0) : 1; if (!ld) ld = 1;
    if (!sw || !sh || !sd) return RDNA4IO_R_DST_OOB;
    if (is_1d && (oy != 0 || sh != 1 || oz != 0 || sd != 1)) return RDNA4IO_R_DST_OOB;
    if (!is_3d && (oz != 0 || sd != 1)) return RDNA4IO_R_DST_OOB;
    if (!rdna4io_range_ok(lw, ox, sw) || !rdna4io_range_ok(lh, oy, sh) ||
        !rdna4io_range_ok(ld, oz, sd))
        return RDNA4IO_R_DST_OOB;
    if (is_bc) {
        if ((ox & 3u) || (oy & 3u) ||
            ((sw & 3u) && ox + sw != lw) ||
            ((sh & 3u) && oy + sh != lh))
            return RDNA4IO_R_BAD_LAYOUT;
    }
    uint64_t bw = is_bc ? ((sw - 1u) / 4u + 1u) : sw;
    uint64_t bh = is_bc ? ((sh - 1u) / 4u + 1u) : sh;
    uint64_t row_bytes = 0, slice_packed = 0, packed = 0;
    if (__builtin_mul_overflow(bw, (uint64_t)bpp, &row_bytes)) return RDNA4IO_R_BAD_LAYOUT;
    if (__builtin_mul_overflow(row_bytes, bh, &slice_packed)) return RDNA4IO_R_BAD_LAYOUT;
    if (__builtin_mul_overflow(slice_packed, sd, &packed)) return RDNA4IO_R_BAD_LAYOUT;

    uint64_t row_stride = src_bpr;
    if (is_1d) {
        row_stride = row_bytes;
    } else if (row_stride == 0) {
        if (bh > 1) return RDNA4IO_R_BAD_LAYOUT;
        row_stride = row_bytes;
    } else if (row_stride < row_bytes) {
        return RDNA4IO_R_BAD_LAYOUT;
    }

    uint64_t min_img = 0;
    if (__builtin_mul_overflow(row_stride, bh, &min_img)) return RDNA4IO_R_BAD_LAYOUT;
    uint64_t img_stride = src_bpi;
    if (sd > 1) {
        if (img_stride < min_img) return RDNA4IO_R_BAD_LAYOUT;
    } else {
        img_stride = min_img;
    }

    // Exact source span in bytes: (sd - 1) * img_stride + (bh - 1) * row_stride + row_bytes.
    uint64_t span = 0, z_extra = 0;
    if (__builtin_mul_overflow(bh - 1u, row_stride, &span) ||
        __builtin_add_overflow(span, row_bytes, &span))
        return RDNA4IO_R_SRC_OOB;
    if (sd > 1) {
        if (__builtin_mul_overflow(sd - 1u, img_stride, &z_extra) ||
            __builtin_add_overflow(span, z_extra, &span))
            return RDNA4IO_R_SRC_OOB;
    }
    if (!rdna4io_range_ok(file_size, src_off, span)) return RDNA4IO_R_SRC_OOB;

    out->bw = bw;
    out->bh = bh;
    out->depth = sd;
    out->row_bytes = row_bytes;
    out->row_stride = row_stride;
    out->img_stride = img_stride;
    out->src_span = span;
    out->packed_bytes = packed;
    return RDNA4IO_OK;
}

// ---------------------------------------------------------------------------
// Per-MTLIOCommandBuffer state machine (8,192 command cap, error latch, cancel).
// ---------------------------------------------------------------------------
typedef struct {
    rdna4io_phase_t phase;
    int status;
    uint32_t cmd_count;
    int cancel_requested;
    rdna4io_reason_t sticky_reason;
    int sticky_mtl_err;
} rdna4io_cb_state_t;

static inline void rdna4io_cb_init(rdna4io_cb_state_t *s) {
    memset(s, 0, sizeof *s);
    s->phase = RDNA4IO_PHASE_RECORDING;
    s->status = RDNA4IO_STATUS_PENDING;
}

static inline void rdna4io_cb_latch_error(rdna4io_cb_state_t *s, int mtl_err,
                                          rdna4io_reason_t reason) {
    if (!s || reason == RDNA4IO_OK) return;
    if (s->sticky_reason == RDNA4IO_OK) {
        s->sticky_reason = reason;
        s->sticky_mtl_err = mtl_err ? mtl_err : RDNA4IO_ERR_INTERNAL;
    }
}

static inline rdna4io_reason_t rdna4io_cb_add_command(rdna4io_cb_state_t *s) {
    if (!s) return RDNA4IO_R_INVALID_ARG;
    if (s->phase >= RDNA4IO_PHASE_COMMITTED) return RDNA4IO_R_ALREADY_COMMITTED;
    if (s->cancel_requested) return RDNA4IO_R_CANCELLED;
    if (s->cmd_count >= RDNA4_IO_MAX_COMMANDS) {
        rdna4io_cb_latch_error(s, RDNA4IO_ERR_INTERNAL, RDNA4IO_R_COMMAND_LIMIT);
        return RDNA4IO_R_COMMAND_LIMIT;
    }
    s->cmd_count++;
    return RDNA4IO_OK;
}

// Returns 1 if transitioned RECORDING -> ENQUEUED, 0 otherwise.
static inline int rdna4io_cb_enqueue(rdna4io_cb_state_t *s) {
    if (!s || s->phase != RDNA4IO_PHASE_RECORDING) return 0;
    s->phase = RDNA4IO_PHASE_ENQUEUED;
    return 1;
}

// Returns 1 if transitioned to COMMITTED (first commit), 0 if already committed/terminal.
static inline int rdna4io_cb_commit(rdna4io_cb_state_t *s) {
    if (!s || s->phase >= RDNA4IO_PHASE_COMMITTED) return 0;
    s->phase = RDNA4IO_PHASE_COMMITTED;
    return 1;
}

// Requests cancellation. Returns 1 if cancellation took effect (not already terminal).
// Before commit, status immediately reflects CANCELLED; commit then finalizes cleanly.
static inline int rdna4io_cb_request_cancel(rdna4io_cb_state_t *s) {
    if (!s || s->phase == RDNA4IO_PHASE_TERMINAL) return 0;
    s->cancel_requested = 1;
    if (s->phase < RDNA4IO_PHASE_RUNNING) s->status = RDNA4IO_STATUS_CANCELLED;
    return 1;
}

static inline int rdna4io_cb_should_stop(const rdna4io_cb_state_t *s) {
    return !s || s->cancel_requested || s->phase == RDNA4IO_PHASE_TERMINAL;
}

// Worker claims a COMMITTED buffer to run.
// Returns 1 if execution should proceed; 0 if it terminated immediately (cancelled or sticky error).
static inline int rdna4io_cb_begin_run(rdna4io_cb_state_t *s) {
    if (!s || s->phase != RDNA4IO_PHASE_COMMITTED) return 0;
    if (s->cancel_requested) {
        s->phase = RDNA4IO_PHASE_TERMINAL;
        s->status = RDNA4IO_STATUS_CANCELLED;
        rdna4io_cb_latch_error(s, RDNA4IO_ERR_INTERNAL, RDNA4IO_R_CANCELLED);
        return 0;
    }
    if (s->sticky_reason != RDNA4IO_OK) {
        s->phase = RDNA4IO_PHASE_TERMINAL;
        s->status = RDNA4IO_STATUS_ERROR;
        return 0;
    }
    s->phase = RDNA4IO_PHASE_RUNNING;
    return 1;
}

// Transitions to TERMINAL exactly once. Returns 1 on the transition, 0 if already terminal.
static inline int rdna4io_cb_finish(rdna4io_cb_state_t *s, rdna4io_reason_t reason, int mtl_err) {
    if (!s || s->phase == RDNA4IO_PHASE_TERMINAL) return 0;
    s->phase = RDNA4IO_PHASE_TERMINAL;
    if (reason == RDNA4IO_R_CANCELLED || (s->cancel_requested && reason == RDNA4IO_OK)) {
        s->status = RDNA4IO_STATUS_CANCELLED;
        rdna4io_cb_latch_error(s, RDNA4IO_ERR_INTERNAL, RDNA4IO_R_CANCELLED);
    } else if (reason != RDNA4IO_OK) {
        s->status = RDNA4IO_STATUS_ERROR;
        rdna4io_cb_latch_error(s, mtl_err ? mtl_err : RDNA4IO_ERR_INTERNAL, reason);
    } else if (s->sticky_reason != RDNA4IO_OK) {
        s->status = RDNA4IO_STATUS_ERROR;
    } else {
        s->status = RDNA4IO_STATUS_COMPLETE;
    }
    return 1;
}

// ---------------------------------------------------------------------------
// Queue slot & barrier epoch scheduler (serial FIFO, concurrent + enqueueBarrier).
// ---------------------------------------------------------------------------
typedef struct {
    uint64_t id;
    uint64_t epoch;
    int committed;
    int running;
    int done;
} rdna4io_qslot_t;

typedef struct {
    int is_serial;
    uint32_t max_concurrency;
    uint32_t active_workers;
    uint64_t next_id;
    uint64_t current_epoch;
    rdna4io_qslot_t *slots;
    size_t count;
    size_t cap;
} rdna4io_qsched_t;

static inline void rdna4io_qsched_init(rdna4io_qsched_t *q, int is_serial,
                                       uint32_t max_concurrency) {
    memset(q, 0, sizeof *q);
    q->is_serial = is_serial ? 1 : 0;
    q->max_concurrency = is_serial ? 1u : (max_concurrency ? max_concurrency : 1u);
}

static inline void rdna4io_qsched_destroy(rdna4io_qsched_t *q) {
    if (!q) return;
    free(q->slots);
    memset(q, 0, sizeof *q);
}

// Reserves an ordered slot in the queue (returns slot id >= 1, or 0 on OOM).
static inline uint64_t rdna4io_qsched_reserve(rdna4io_qsched_t *q, int committed) {
    if (!q) return 0;
    if (q->count == q->cap) {
        size_t ncap = q->cap ? q->cap * 2u : 16u;
        if (ncap < q->cap || ncap > SIZE_MAX / sizeof(*q->slots)) return 0;
        rdna4io_qslot_t *ns = (rdna4io_qslot_t *)realloc(q->slots, ncap * sizeof(*ns));
        if (!ns) return 0;
        q->slots = ns;
        q->cap = ncap;
    }
    uint64_t id = ++q->next_id;
    q->slots[q->count++] = (rdna4io_qslot_t){
        .id = id, .epoch = q->current_epoch, .committed = committed ? 1 : 0, .running = 0, .done = 0
    };
    return id;
}

static inline void rdna4io_qsched_barrier(rdna4io_qsched_t *q) {
    if (!q) return;
    // Only advance epoch when there is at least one active/pending slot in the current epoch.
    for (size_t i = 0; i < q->count; i++) {
        if (!q->slots[i].done && q->slots[i].epoch == q->current_epoch) {
            q->current_epoch++;
            return;
        }
    }
}

static inline int rdna4io_qsched_mark_committed(rdna4io_qsched_t *q, uint64_t id) {
    if (!q || !id) return 0;
    for (size_t i = 0; i < q->count; i++) {
        if (q->slots[i].id == id) {
            if (q->slots[i].done || q->slots[i].running) return 0;
            q->slots[i].committed = 1;
            return 1;
        }
    }
    return 0;
}

// Compacts leading finished slots to keep the queue O(in-flight).
static inline void rdna4io_qsched_compact(rdna4io_qsched_t *q) {
    size_t lead = 0;
    while (lead < q->count && q->slots[lead].done) lead++;
    if (!lead) return;
    size_t rem = q->count - lead;
    if (rem) memmove(q->slots, q->slots + lead, rem * sizeof(*q->slots));
    q->count = rem;
}

// Attempts to cancel an unstarted slot before a worker picks it up.
// Returns 1 if removed/completed before running, 0 if not found or already running/done.
static inline int rdna4io_qsched_cancel_pending(rdna4io_qsched_t *q, uint64_t id) {
    if (!q || !id) return 0;
    for (size_t i = 0; i < q->count; i++) {
        if (q->slots[i].id == id) {
            if (q->slots[i].running || q->slots[i].done) return 0;
            q->slots[i].done = 1;
            rdna4io_qsched_compact(q);
            return 1;
        }
    }
    return 0;
}

// Picks the next eligible committed slot respecting concurrency, serial FIFO, and barrier epochs.
// Returns slot id (> 0) and marks it running, or 0 if nothing is eligible right now.
static inline uint64_t rdna4io_qsched_pick(rdna4io_qsched_t *q) {
    if (!q || q->active_workers >= q->max_concurrency) return 0;
    uint64_t min_incomplete_epoch = UINT64_MAX;
    size_t first_incomplete = SIZE_MAX;
    for (size_t i = 0; i < q->count; i++) {
        if (q->slots[i].done) continue;
        if (first_incomplete == SIZE_MAX) first_incomplete = i;
        if (q->slots[i].epoch < min_incomplete_epoch) min_incomplete_epoch = q->slots[i].epoch;
    }
    if (first_incomplete == SIZE_MAX) return 0;
    if (q->is_serial) {
        rdna4io_qslot_t *head = &q->slots[first_incomplete];
        if (!head->committed || head->running) return 0;
        head->running = 1;
        q->active_workers++;
        return head->id;
    }
    for (size_t i = first_incomplete; i < q->count; i++) {
        rdna4io_qslot_t *s = &q->slots[i];
        if (s->done || s->running || !s->committed) continue;
        if (s->epoch > min_incomplete_epoch) continue;
        s->running = 1;
        q->active_workers++;
        return s->id;
    }
    return 0;
}

static inline int rdna4io_qsched_complete(rdna4io_qsched_t *q, uint64_t id) {
    if (!q || !id) return 0;
    for (size_t i = 0; i < q->count; i++) {
        if (q->slots[i].id == id) {
            if (q->slots[i].running && q->active_workers > 0) q->active_workers--;
            q->slots[i].running = 0;
            q->slots[i].done = 1;
            rdna4io_qsched_compact(q);
            return 1;
        }
    }
    return 0;
}

#endif // RDNA4_IO_QUEUE_H
