#ifndef RDNA4_BUFFER_REGISTRY_H
#define RDNA4_BUFFER_REGISTRY_H
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include "rdna4_buffer_ranges.h"

#define RDNA4_BUFFER_REGISTRY_VERSION 3u
#define RDNA4_BUFFER_REGISTRY_MIN_BYTES UINT64_C(800)
#define RDNA4_BUFFER_REGISTRY_ROW_BYTES UINT64_C(24)

typedef struct {
    uint64_t row_count, row_stride_bytes, table_bytes, reserved;
} rdna4_buffer_registry_header;
typedef struct {
    uint64_t base, length, access;
} rdna4_buffer_registry_row;

_Static_assert(sizeof(rdna4_buffer_table) == 768, "buffer table direct prefix ABI");
_Static_assert(sizeof(rdna4_buffer_registry_header) == 32, "buffer registry header ABI");
_Static_assert(sizeof(rdna4_buffer_registry_row) == 24, "buffer registry row ABI");

static inline int rdna4_buffer_registry_size(uint64_t count, uint64_t limit,
                                             size_t *bytes) {
    if (!bytes || count > (UINT64_MAX - RDNA4_BUFFER_REGISTRY_MIN_BYTES) /
                              RDNA4_BUFFER_REGISTRY_ROW_BYTES) return 0;
    uint64_t extent = RDNA4_BUFFER_REGISTRY_MIN_BYTES +
                      count * RDNA4_BUFFER_REGISTRY_ROW_BYTES;
    // Uploads round to 256 bytes, so admission includes that allocation extent.
    if (extent > limit || extent > SIZE_MAX - 255) return 0;
    *bytes = (size_t)extent;
    return 1;
}

static inline int rdna4_buffer_registry_init(void *table, size_t bytes,
        const rdna4_buffer_table *direct, uint64_t count, uint64_t limit) {
    size_t expected = 0;
    if (!table || !direct || !rdna4_buffer_registry_size(count, limit, &expected) ||
        bytes != expected) return 0;
    memset(table, 0, bytes);
    memcpy(table, direct, sizeof(*direct));
    rdna4_buffer_registry_header header = {
        count, RDNA4_BUFFER_REGISTRY_ROW_BYTES, bytes, 0
    };
    memcpy((uint8_t *)table + sizeof(*direct), &header, sizeof(header));
    return 1;
}

static inline int rdna4_buffer_registry_set(void *table, size_t bytes,
        uint64_t index, uint64_t base, uint64_t length, int readonly) {
    if (!table || bytes < RDNA4_BUFFER_REGISTRY_MIN_BYTES || !base || !length ||
        length > UINT64_MAX - base) return 0;
    rdna4_buffer_registry_header header;
    memcpy(&header, (uint8_t *)table + sizeof(rdna4_buffer_table), sizeof(header));
    size_t expected = 0;
    if (header.row_stride_bytes != RDNA4_BUFFER_REGISTRY_ROW_BYTES || header.reserved ||
        !rdna4_buffer_registry_size(header.row_count, bytes, &expected) ||
        expected != bytes || header.table_bytes != bytes || index >= header.row_count)
        return 0;
    rdna4_buffer_registry_row row = {
        base, length, RDNA4_BUFFER_READ | (readonly ? 0 : RDNA4_BUFFER_WRITE)
    };
    memcpy((uint8_t *)table + RDNA4_BUFFER_REGISTRY_MIN_BYTES +
           (size_t)index * sizeof(row), &row, sizeof(row));
    return 1;
}
#endif
