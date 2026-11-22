// Pure C bounded-slice fence wait loop (no Vulkan, no ObjC, no locking).
// GarnetMetal runs this on a concurrent waiter queue and delivers rdna4FenceDone on the serial completion queue.
#ifndef RDNA4_FENCEWAIT_H
#define RDNA4_FENCEWAIT_H
#include <stdint.h>

#define RDNA4FW_SLICE_NS   2000000000ULL   // 2 s per vkWaitForFences slice (matches rdna4_oneshot and test-large-memory-lifetime)
#define RDNA4FW_MAX_SLICES 30              // 30 x 2 s = 60 s fence timeout budget
#define RDNA4FW_TIMEOUT    2               // == VK_TIMEOUT (asserted in rdna4_command_buffer_impl.inc)

static inline int rdna4fw_log_pending(int slices) {
    return slices == 1 || slices == 2 || slices == 10;   // 2 s, 4 s, 20 s
}

static inline int rdna4fw_wait_bounded(int (*wait_slice)(void *, uint64_t), void *ctx,
                                       int start_slices, int max_slices, uint64_t slice_ns,
                                       void (*on_pending)(void *, int), int *slices_out) {
    int r = RDNA4FW_TIMEOUT;
    int slices = start_slices < 0 ? 0 : start_slices;
    if (!wait_slice) {
        if (slices_out) *slices_out = slices;
        return r;
    }
    while (r == RDNA4FW_TIMEOUT && slices < max_slices) {
        r = wait_slice(ctx, slice_ns);
        slices++;
        if (r == RDNA4FW_TIMEOUT && on_pending && rdna4fw_log_pending(slices))
            on_pending(ctx, slices);
    }
    if (slices_out) *slices_out = slices;
    return r;
}
#endif
