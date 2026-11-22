#ifndef RDNA4_NOCOPY_H
#define RDNA4_NOCOPY_H

#include <stdint.h>

/* Leave outputs untouched if the enclosing aligned import range overflows. */
static inline int rdna4nc_import_range(uint64_t p, uint64_t len, uint64_t align,
                                      uint64_t *base, uint64_t *span, uint64_t *delta) {
    if (!len || !align || (align & (align - 1)) || len > UINT64_MAX - p) return -1;
    uint64_t end = p + len, mask = align - 1;
    uint64_t padding = (align - (end & mask)) & mask;
    if (padding > UINT64_MAX - end) return -1;
    uint64_t start = p & ~mask;
    *base = start;
    *span = end + padding - start;
    *delta = p - start;
    return 0;
}

enum { RDNA4NC_PROT_READ = 1u, RDNA4NC_PROT_WRITE = 2u, RDNA4NC_PROT_EXEC = 4u };

struct rdna4nc_region { uint64_t start, size; uint32_t prot; };

/* Regions describe nonoverlapping VM mappings; gaps take precedence over RO. */
static inline int rdna4nc_range_readonly(const struct rdna4nc_region *regions, unsigned n,
                                       uint64_t start, uint64_t len) {
    if (len > UINT64_MAX - start) return -1;
    const uint64_t end = start + len;
    int readonly = 0;
    while (start < end) {
        unsigned i;
        for (i = 0; i < n; i++) {
            if (regions[i].start <= start && start - regions[i].start < regions[i].size) break;
        }
        if (i == n) return -1;
        if (!(regions[i].prot & RDNA4NC_PROT_WRITE)) readonly = 1;
        uint64_t remaining = regions[i].size - (start - regions[i].start);
        start += remaining < end - start ? remaining : end - start;
    }
    return readonly;
}

static inline void rdna4nc_protection_text(uint32_t flags, char out[4]) {
    out[0] = (flags & RDNA4NC_PROT_READ) ? 'r' : '-';
    out[1] = (flags & RDNA4NC_PROT_WRITE) ? 'w' : '-';
    out[2] = (flags & RDNA4NC_PROT_EXEC) ? 'x' : '-';
    out[3] = '\0';
}

#endif
