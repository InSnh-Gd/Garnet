#include <assert.h>
#include <stddef.h>
#include <stdio.h>
#include "rdna4_buffer_ranges.h"

int main(void) {
    const uint64_t gib = UINT64_C(1) << 30;
    uint64_t offset = 0, size = 0;
    assert(sizeof(rdna4_buffer_table) == 768);
    assert(offsetof(rdna4_buffer_table, length) == 256);
    assert(offsetof(rdna4_buffer_table, access) == 512);
    assert(rdna4_buffer_range(8 * gib, 4 * gib - 4, 8));
    assert(rdna4_buffer_range(8 * gib, 8 * gib, 0));
    assert(!rdna4_buffer_range(8 * gib, 8 * gib, 1));
    assert(!rdna4_buffer_range(8 * gib, UINT64_MAX, 1));
    assert(!rdna4_buffer_range(UINT64_MAX, UINT64_MAX - 1, 2));
    assert(rdna4_buffer_view(8 * gib, 4 * gib + 256, 4096, 256,
                              UINT32_MAX, &offset, &size));
    assert(offset == 4 * gib + 256 + 4096 && size == 4 * gib - 256);
    assert(rdna4_buffer_view(8 * gib, 0, 0, 256, 65536, &offset, &size));
    assert(offset == 0 && size == 65536);
    assert(rdna4_buffer_view(8 * gib, 8 * gib - 4, 0, 4, 65536, &offset, &size));
    assert(offset == 8 * gib - 4 && size == 4);
    assert(!rdna4_buffer_view(8 * gib, 8 * gib, 0, 4, 65536, &offset, &size));
    assert(!rdna4_buffer_view(8 * gib, 1, 0, 256, 65536, &offset, &size));
    assert(!rdna4_buffer_view(8 * gib, 0, 0, 4, 0, &offset, &size));
    assert(!rdna4_buffer_view(8 * gib, 4 * gib, UINT64_MAX, 1, 65536, &offset, &size));
    assert(!rdna4_buffer_view(8 * gib, 0, UINT64_MAX - gib, 1, 2 * gib, &offset, &size));
    assert(rdna4_buffer_view(128, 16, 4096, 16, 65536, &offset, &size));
    assert(offset == 4112 && size == 112);

    rdna4_buffer_table table = {0};
    assert(rdna4_buffer_table_set(&table, 7, 32 * gib, 8 * gib, 4 * gib, 1));
    assert(table.address[7] == 36 * gib && table.length[7] == 4 * gib);
    assert(table.access[7] == RDNA4_BUFFER_READ && table.address[6] == 0);
    assert(rdna4_buffer_table_set(&table, 7, 32 * gib, 8 * gib, 0, 0));
    assert(table.access[7] == (RDNA4_BUFFER_READ | RDNA4_BUFFER_WRITE));
    rdna4_buffer_table saved = table;
    assert(!rdna4_buffer_table_set(&table, 31, 1, 1, 0, 0));
    assert(!rdna4_buffer_table_set(&table, 7, 0, 1, 0, 0));
    assert(!rdna4_buffer_table_set(&table, 7, 1, 1, 1, 0));
    assert(!rdna4_buffer_table_set(&table, 7, UINT64_MAX - 1, 2, 0, 0));
    assert(table.address[7] == saved.address[7] && table.length[7] == saved.length[7] &&
           table.access[7] == saved.access[7]);
    puts("buffer-ranges: 64-bit bounds, descriptor windows, layout and atomic rejection PASS");
    return 0;
}
