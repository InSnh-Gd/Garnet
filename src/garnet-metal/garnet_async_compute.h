#pragma once
#include <stdint.h>

// Kill-switch file: when present at RADV device open, disable dedicated async compute queue.
#define GARNET_NO_ASYNC_COMPUTE_FILE "/private/tmp/garnet-no-async-compute"

// Per-MTLCommandQueue hardware ordering tracker across GFX (0) and Compute (1) queues.
typedef struct {
    int last_was_compute;     // 0 = GFX, 1 = Compute
    uint64_t last_value;      // 0 = no timeline submission recorded yet
} garnet_async_qorder_t;

typedef struct {
    uint64_t wait_value;      // 0 = same hardware queue or first submission (no cross-queue wait)
    uint64_t signal_value;    // monotonically increasing (> 0)
} garnet_async_submit_order_t;

// Device-level gate for enabling the dedicated Vulkan compute queue (MEC).
// WindowServer always stays on the GFX queue to isolate 4K@160Hz compositing/scanout.
static inline int garnet_async_compute_device_enable(int has_dedicated_compute_qf,
                                                     int has_timeline_semaphore,
                                                     int is_windowserver,
                                                     int kill_switch_exists) {
    return (has_dedicated_compute_qf && has_timeline_semaphore &&
            !is_windowserver && !kill_switch_exists) ? 1 : 0;
}

// Whether a pristine segment 0 of a command buffer can switch from GFX to the compute queue
// when a computeCommandEncoder is created.
static inline int garnet_async_compute_can_switch_segment(int device_async_on,
                                                          int is_windowserver,
                                                          uint32_t segment_count,
                                                          int segment_ended,
                                                          int already_compute,
                                                          int has_work,
                                                          int has_events,
                                                          int has_sync_items,
                                                          int has_arg_intents,
                                                          int has_touches,
                                                          int host_only) {
    if (!device_async_on || is_windowserver) return 0;
    if (segment_count != 1u || segment_ended || already_compute) return 0;
    if (has_work || has_events || has_sync_items || has_arg_intents || has_touches || host_only)
        return 0;
    return 1;
}

// Whether a texture bound in a compute dispatch can run directly on the dedicated compute queue.
// IOSurfaces, depth/stencil textures, and textures still in graphics attachment layouts stay on GFX.
static inline int garnet_async_compute_texture_ok(int is_iosurface,
                                                  int is_depth_or_stencil,
                                                  int in_graphics_attachment_layout) {
    return (!is_iosurface && !is_depth_or_stencil && !in_graphics_attachment_layout) ? 1 : 0;
}

// Whether an active compute segment must fall back to a GFX segment before encoding a dispatch.
static inline int garnet_async_compute_needs_gfx_fallback(int is_compute_segment,
                                                          int need_arg_prep,
                                                          int has_icb_or_rt_owners,
                                                          int has_incompatible_texture) {
    if (!is_compute_segment) return 0;
    return (need_arg_prep || has_icb_or_rt_owners || has_incompatible_texture) ? 1 : 0;
}

// Advance same-MTLCommandQueue timeline ordering for a submission to GFX (0) or Compute (1).
// Same-queue submissions stay hardware-FIFO without waits; GFX <-> Compute transitions wait
// on the prior queue's last signaled timeline value.
static inline garnet_async_submit_order_t
garnet_async_qorder_next(garnet_async_qorder_t *st, int is_compute) {
    garnet_async_submit_order_t out = { 0u, 0u };
    const int cur = is_compute ? 1 : 0;
    if (st->last_value > 0u && st->last_was_compute != cur)
        out.wait_value = st->last_value;
    st->last_value++;
    st->last_was_compute = cur;
    out.signal_value = st->last_value;
    return out;
}
