#include <assert.h>
#include <stdio.h>
#include "rdna4_nocopy.h"

static void range(uint64_t p, uint64_t len, uint64_t align,
                  uint64_t want_base, uint64_t want_span, uint64_t want_delta) {
    uint64_t base = 0, span = 0, delta = 0;
    assert(rdna4nc_import_range(p, len, align, &base, &span, &delta) == 0);
    assert(base == want_base && span == want_span && delta == want_delta);
}

static void invalid(uint64_t p, uint64_t len, uint64_t align) {
    uint64_t base = 17, span = 19, delta = 23;
    assert(rdna4nc_import_range(p, len, align, &base, &span, &delta) != 0);
    assert(base == 17 && span == 19 && delta == 23);
}

int main(void) {
    range(0x10000, 65536, 4096, 0x10000, 65536, 0);
    range(0x10001, 65536, 4096, 0x10000, 69632, 1);
    range(0x10000, 65535, 4096, 0x10000, 65536, 0);
    range(0x10fff, 2, 4096, 0x10000, 8192, 4095);
    range(0x14001, 16384, 16384, 0x14000, 32768, 1);
    range(7, 3, 1, 7, 3, 0);
    range(UINT64_MAX - 8191, 4096, 4096, UINT64_MAX - 8191, 4096, 0);
    range(0, 1, UINT64_C(1) << 63, 0, UINT64_C(1) << 63, 0);
    invalid(UINT64_MAX - 1, 2, 4096);
    invalid(UINT64_MAX - 1, 1, 4096);
    invalid(UINT64_MAX - 4095, 4095, 4096);
    invalid(4096, 0, 4096);
    invalid(4096, 1, 0);
    invalid(4096, 1, 3);
    puts("PASS test-nocopy-range (14 checks)");
    return 0;
}
