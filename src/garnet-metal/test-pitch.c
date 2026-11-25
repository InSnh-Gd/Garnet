#include <stdio.h>
#include "garnet_pitch.h"

static int checks, failures;
#define CHECK(name, value, expected) do { \
    int got_ = (value); checks++; \
    if (got_ != (expected)) { failures++; printf("FAIL %s: got %d expected %d\n", (name), got_, (expected)); } \
} while (0)

int main(void) {
    CHECK("960x540, bpr 4096", rdna4p_explicit_pitch_reason(4096, 960, 4, 3840, 0, 1, 1, 1, 1), RDNA4P_EXPLICIT_TRY);
    CHECK("88x60, bpr 384", rdna4p_explicit_pitch_reason(384, 88, 4, 512, 0, 1, 1, 1, 1), RDNA4P_EXPLICIT_TRY);
    CHECK("3752x60, bpr 15040 is unaligned", rdna4p_explicit_pitch_reason(15040, 3752, 4, 15104, 0, 1, 1, 1, 1), RDNA4P_EXPLICIT_PITCH);
    CHECK("916x514, bpr 3904 is unaligned", rdna4p_explicit_pitch_reason(3904, 916, 4, 3840, 0, 1, 1, 1, 1), RDNA4P_EXPLICIT_PITCH);
    CHECK("IconServices RGBA16Float 268x268, bpr 2144 requires copies", rdna4p_explicit_pitch_reason(2144, 268, 8, 2304, 0, 1, 1, 1, 1), RDNA4P_EXPLICIT_PITCH);
    CHECK("210x118, bpr 896", rdna4p_explicit_pitch_reason(896, 210, 4, 1024, 0, 1, 1, 1, 1), RDNA4P_EXPLICIT_TRY);
    CHECK("natural pitch match does not retry", rdna4p_explicit_pitch_reason(4096, 1024, 4, 4096, 0, 1, 1, 1, 1), RDNA4P_EXPLICIT_MATCH);
    CHECK("pitch smaller than row bytes", rdna4p_explicit_pitch_reason(383, 96, 4, 512, 0, 1, 1, 1, 1), RDNA4P_EXPLICIT_PITCH);
    CHECK("missing extension", rdna4p_explicit_pitch_reason(4096, 960, 4, 3840, 0, 1, 1, 1, 0), RDNA4P_EXPLICIT_NO_EXTENSION);
    CHECK("image flags conservatively refuse", rdna4p_explicit_pitch_reason(4096, 960, 4, 3840, 1, 1, 1, 1, 1), RDNA4P_EXPLICIT_FLAGS);
    CHECK("multiple mip levels refuse", rdna4p_explicit_pitch_reason(4096, 960, 4, 3840, 0, 2, 1, 1, 1), RDNA4P_EXPLICIT_SHAPE);
    CHECK("multiple array layers refuse", rdna4p_explicit_pitch_reason(4096, 960, 4, 3840, 0, 1, 2, 1, 1), RDNA4P_EXPLICIT_SHAPE);
    CHECK("multiple samples refuse", rdna4p_explicit_pitch_reason(4096, 960, 4, 3840, 0, 1, 1, 4, 1), RDNA4P_EXPLICIT_SHAPE);
    CHECK("width multiplication overflow refuses", rdna4p_explicit_pitch_reason(4096, SIZE_MAX, 4, 3840, 0, 1, 1, 1, 1), RDNA4P_EXPLICIT_SHAPE);
    printf("test-pitch: %d checks, %d failed\n", checks, failures);
    return failures != 0;
}
