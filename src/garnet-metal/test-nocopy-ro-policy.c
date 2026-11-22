#include <stdio.h>
#include "rdna4_nocopy.h"

int main(void) {
    struct rdna4nc_region regions[] = {
        { 0x1000, 0x1000, RDNA4NC_PROT_READ | RDNA4NC_PROT_WRITE },
        { 0x2000, 0x1000, RDNA4NC_PROT_READ | RDNA4NC_PROT_WRITE },
        { 0x3000, 0x1000, RDNA4NC_PROT_READ | RDNA4NC_PROT_WRITE }
    };
    unsigned checks = 0, failures = 0;
#define CHECK(start, len, want) do { \
    int got = rdna4nc_range_readonly(regions, 3, (start), (len)); \
    checks++; \
    if (got != (want)) { printf("FAIL %u: got %d expected %d\n", checks, got, (want)); failures++; } \
} while (0)
    CHECK(0x1000, 0x3000, 0);
    regions[1].prot = RDNA4NC_PROT_READ;
    CHECK(0x1000, 0x3000, 1);
    CHECK(0x1800, 0x1000, 1);
    CHECK(0x1000, 0x1000, 0);
    CHECK(0x2000, 0x1000, 1);
    CHECK(0x3000, 0x1000, 0);
    CHECK(0x4000, 0, 0);
    CHECK(0, 0, 0);
    CHECK(0, 0x2000, -1);
    CHECK(0x1800, 0x3000, -1);
    CHECK(0x4000, 1, -1);
    CHECK(UINT64_MAX, 1, -1);
    regions[1].start = 0x2800; regions[1].size = 0x800;
    CHECK(0x1000, 0x3000, -1);
    if (rdna4nc_range_readonly(NULL, 0, 1, 1) != -1) failures++;
    checks++;
    if (!failures) printf("PASS %u\n", checks);
    return failures ? 1 : 0;
}
