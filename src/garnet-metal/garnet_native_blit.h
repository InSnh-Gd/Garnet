#ifndef RDNA4_NATIVE_BLIT_H
#define RDNA4_NATIVE_BLIT_H
#include <stdbool.h>
#include <stdint.h>
#include <errno.h>
#include <stdlib.h>
#include <vulkan/vulkan.h>

typedef struct {
    VkDeviceMemory src, dst;
    uint64_t srcOffset, dstOffset, size;
    uint32_t pattern;
    bool fill;
} RDNA4NativeOp;
typedef struct {
    RDNA4NativeOp *ops;
    size_t count, capacity;
    bool blocked;
} RDNA4NativeJournal;
typedef struct {
    int (*copy)(VkDevice, VkDeviceMemory, uint64_t, VkDeviceMemory, uint64_t, uint64_t, uint64_t *);
    int (*fill)(VkDevice, VkDeviceMemory, uint64_t, uint64_t, uint32_t, uint64_t *);
    int (*wait)(VkDevice, uint64_t, uint64_t, bool *);
} RDNA4NativeBridge;

static inline void rdna4_native_record(RDNA4NativeJournal *j, RDNA4NativeOp op) {
    if (j->blocked) return;
    if (!op.dst || (!op.fill && !op.src) || !op.size || op.size > 1024u * 1024u ||
        ((op.srcOffset | op.dstOffset | op.size) & 3) ||
        op.srcOffset > UINT64_MAX - op.size || op.dstOffset > UINT64_MAX - op.size ||
        (!op.fill && op.src == op.dst && op.srcOffset < op.dstOffset + op.size &&
         op.dstOffset < op.srcOffset + op.size)) { j->blocked = true; return; }
    if (j->count == j->capacity) {
        size_t capacity = j->capacity ? j->capacity * 2 : 8;
        if (capacity < j->capacity || capacity > SIZE_MAX / sizeof(op)) { j->blocked = true; return; }
        void *ops = realloc(j->ops, capacity * sizeof(op));
        if (!ops) { j->blocked = true; return; }
        j->ops = ops; j->capacity = capacity;
    }
    j->ops[j->count++] = op;
}

// Caller holds the global submit lock, has drained Vulkan, and quarantines on error.
// Only the first unsubmitted ENOSYS permits replay of the Vulkan recording.
static inline int rdna4_native_execute(const RDNA4NativeJournal *j, RDNA4NativeBridge b,
                                     VkDevice dev, bool *fallback) {
    *fallback = false;
    for (size_t i = 0; i < j->count; ++i) {
        RDNA4NativeOp op = j->ops[i]; uint64_t seq = 0;
        int r = op.fill ? b.fill(dev, op.dst, op.dstOffset, op.size, op.pattern, &seq) :
            b.copy(dev, op.src, op.srcOffset, op.dst, op.dstOffset, op.size, &seq);
        if (r == -ENOSYS && !i && !seq) { *fallback = true; return 0; }
        if (r) return r;
        if (!seq) return -EIO;
        bool busy = true;
        r = b.wait(dev, seq, 2000000000ULL, &busy);
        if (r) return r;
        if (busy) return -ETIMEDOUT;
    }
    return 0;
}
#endif
