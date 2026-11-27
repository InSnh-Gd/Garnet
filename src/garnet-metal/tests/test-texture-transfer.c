#include <stdio.h>
#include <string.h>
#include "garnet_texture_transfer.h"

int main(void) {
    const size_t widths[] = {0, 1, 2, 64, UINT32_MAX, SIZE_MAX};
    const size_t heights[] = {0, 1, 2, 3, 4096};
    const size_t depths[] = {0, 1, 2, 3, 4096};
    const size_t bpps[] = {1, 4, 8};
    const size_t strides[] = {0, 1, 8, 16, 256, 4096, SIZE_MAX / 2, SIZE_MAX};
    unsigned checks = 0;
    for (unsigned w = 0; w < 6; ++w) for (unsigned h = 0; h < 5; ++h)
    for (unsigned d = 0; d < 5; ++d) for (unsigned b = 0; b < 3; ++b)
    for (unsigned r = 0; r < 8; ++r) for (unsigned i = 0; i < 8; ++i) {
        size_t width = widths[w], height = heights[h], depth = depths[d];
        size_t bpp = bpps[b], pitch = strides[r], image = strides[i];
        // These bounded dimensions keep the independent exact oracle below 128 bits.
        __uint128_t row = (__uint128_t)width * bpp, packed = row * height * depth;
        __uint128_t span = height ? (__uint128_t)(height - 1) * pitch + row : 0;
        __uint128_t minimum_image = (__uint128_t)pitch * height;
        if (depth > 1) span += (__uint128_t)(depth - 1) * image;
        int expected = width && height && depth && row <= SIZE_MAX && packed <= SIZE_MAX &&
            pitch >= row && span <= SIZE_MAX && (depth == 1 || image >= minimum_image);
        rdna4_texture_transfer actual = {13, 17, 19}, before = actual;
        int ok = rdna4_texture_transfer_layout(width, height, depth, bpp, pitch, image, &actual);
        if (ok != expected || (ok && (actual.row_bytes != row || actual.packed_bytes != packed || actual.span != span)) ||
            (!ok && memcmp(&actual, &before, sizeof actual))) {
            fprintf(stderr, "TEXTURE_TRANSFER_FAIL w=%zu h=%zu d=%zu bpp=%zu pitch=%zu image=%zu accepted=%d expected=%d\n",
                width, height, depth, bpp, pitch, image, ok, expected);
            return 1;
        }
        ++checks;
    }
    if (rdna4_texture_transfer_pointer(NULL, 16) ||
        rdna4_texture_transfer_pointer((void *)(UINTPTR_MAX - 15), 16) ||
        !rdna4_texture_transfer_pointer((void *)(UINTPTR_MAX - 15), 15)) return 1;
    printf("TEXTURE_TRANSFER_PASS exact_oracle_checks=%u pointer_edges=3\n", checks);
    return 0;
}
