#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include "rdna4_buffer_registry.h"

int main(void) {
    size_t bytes = 0;
    assert(rdna4_buffer_registry_size(257, 6968, &bytes) && bytes == 6968);
    assert(!rdna4_buffer_registry_size(257, 6967, &bytes));
    assert(!rdna4_buffer_registry_size(UINT64_MAX, UINT64_MAX, &bytes));
    assert(!rdna4_buffer_registry_size((UINT64_MAX - 800) / 24,
                                      UINT64_MAX, &bytes));
    assert(!rdna4_buffer_registry_size(0, 799, &bytes));
    assert(rdna4_buffer_registry_size(0, 800, &bytes) && bytes == 800);
    rdna4_buffer_table direct = {0};
    assert(rdna4_buffer_table_set(&direct, 4, UINT64_C(0x1800000000),
                                   UINT64_C(0x200000000), 256, 1));
    assert(rdna4_buffer_registry_size(257, UINT32_MAX, &bytes));
    uint8_t *table = malloc(bytes);
    assert(table && rdna4_buffer_registry_init(table, bytes, &direct, 257, UINT32_MAX));
    assert(!memcmp(table, &direct, sizeof direct));
    uint64_t header[4];
    memcpy(header, table + 768, sizeof header);
    assert(header[0] == 257 && header[1] == 24 && header[2] == bytes && header[3] == 0);
    for (unsigned i = 0; i < 257; ++i)
        assert(rdna4_buffer_registry_set(table, bytes, i, UINT64_C(0x1800000000) + i,
                                           UINT64_C(0x200000000), i & 1));
    rdna4_buffer_registry_row row;
    memcpy(&row, table + 800 + 255 * 24, sizeof row);
    assert(row.base == UINT64_C(0x18000000ff) && row.length == UINT64_C(0x200000000) &&
           row.access == RDNA4_BUFFER_READ);
    assert(!rdna4_buffer_registry_set(table, bytes, 257, 1, 1, 0));
    assert(!rdna4_buffer_registry_set(table, bytes, 0, UINT64_MAX - 1, 2, 0));
    assert(!rdna4_buffer_registry_set(table, bytes, 0, 0, 1, 0));
    assert(!rdna4_buffer_registry_set(table, bytes, 0, 1, 0, 0));
    header[3] = 1;
    memcpy(table + 768, header, sizeof header);
    assert(!rdna4_buffer_registry_set(table, bytes, 0, 1, 1, 0));
    header[3] = 0; header[0] = UINT64_MAX;
    memcpy(table + 768, header, sizeof header);
    assert(!rdna4_buffer_registry_set(table, bytes, 0, 1, 1, 0));
    free(table);
    puts("BUFFER_REGISTRY_PASS rows=257 exact_layout=1 high_half=1 overflow_rejected=1 descriptor_capacity=1");
    return 0;
}
