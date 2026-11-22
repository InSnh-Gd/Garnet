// Logical buffer ranges are 64-bit; descriptor views retain Vulkan's own limits.
#ifndef RDNA4_BUFFER_RANGES_H
#define RDNA4_BUFFER_RANGES_H
#include <stdint.h>

#define RDNA4_BUFFER_LEGACY_MAX UINT64_C(0xfffffffc)
#define RDNA4_BUFFER_TABLE_VERSION 2u
#define RDNA4_BUFFER_READ UINT64_C(1)
#define RDNA4_BUFFER_WRITE UINT64_C(2)
typedef struct {
    uint64_t address[32];
    uint64_t length[32];
    uint64_t access[32];
} rdna4_buffer_table;

static inline int rdna4_buffer_range(uint64_t length, uint64_t offset, uint64_t size) {
    return offset <= length && size <= length - offset;
}

// A view begins at the relocated buffer offset. BDA uses the full logical length
// from the table, independent of this bounded descriptor window.
static inline int rdna4_buffer_view(uint64_t length, uint64_t offset, uint64_t binding_offset,
                                     uint64_t alignment, uint64_t limit,
                                     uint64_t *view_offset, uint64_t *view_size) {
    if (offset >= length || !limit || offset > UINT64_MAX - binding_offset) return 0;
    uint64_t start = binding_offset + offset;
    if (alignment && start % alignment) return 0;
    uint64_t size = length - offset;
    if (size > limit) size = limit;
    if (size > UINT64_MAX - start) return 0;
    *view_offset = start;
    *view_size = size;
    return 1;
}

static inline int rdna4_buffer_table_set(rdna4_buffer_table *t, unsigned index,
                                          uint64_t base, uint64_t length,
                                          uint64_t offset, int readonly) {
    if (index >= 31 || !base || offset >= length || length > UINT64_MAX - base) return 0;
    t->address[index] = base + offset;
    t->length[index] = length - offset;
    t->access[index] = RDNA4_BUFFER_READ | (readonly ? 0 : RDNA4_BUFFER_WRITE);
    return 1;
}
#endif
