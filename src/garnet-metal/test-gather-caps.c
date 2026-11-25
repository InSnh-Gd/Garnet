// cc -std=c11 -O2 -Wall -Wextra -Werror test-gather-caps.c -o /tmp/test-gather-caps && /tmp/test-gather-caps
#include <limits.h>
#include <stdio.h>
#include "garnet_gather_caps.h"

static unsigned checks, failures;
#define CHECK(message, condition) do { \
    checks++; \
    if (condition) printf("PASS: %s\n", message); \
    else { printf("FAIL: %s (line %d)\n", message, __LINE__); failures++; } \
} while (0)

int main(void) {
    rdna4_gather_caps absent =
        rdna4_gather_caps_from_vulkan(0, -8, 7);
    CHECK("feature false is reported as unsupported",
          !absent.supported && !rdna4_gather_caps_supported(&absent));
    CHECK("feature false preserves the queried gather limits",
          absent.min_texel_gather_offset == -8 &&
          absent.max_texel_gather_offset == 7);
    CHECK("runtime Offset fails closed without the feature",
          !rdna4_gather_offset_ok(&absent, RDNA4_GATHER_OFFSET_RUNTIME, 0, 0));
    CHECK("plural ConstOffsets fail closed without the feature",
          !rdna4_gather_offset_ok(&absent,
                                  RDNA4_GATHER_OFFSET_CONST_OFFSETS, 0, 0));
    CHECK("core single ConstOffset remains available without the feature",
          rdna4_gather_offset_ok(&absent,
                                 RDNA4_GATHER_OFFSET_CONSTANT, -8, 7));
    CHECK("core single ConstOffset is still range checked",
          !rdna4_gather_offset_ok(&absent,
                                  RDNA4_GATHER_OFFSET_CONSTANT, -9, 0) &&
          !rdna4_gather_offset_ok(&absent,
                                  RDNA4_GATHER_OFFSET_CONSTANT, 0, 8));

    rdna4_gather_caps present =
        rdna4_gather_caps_from_vulkan(2, -8, 7);
    CHECK("nonzero feature support is normalized to one",
          present.supported == 1 &&
          rdna4_gather_caps_supported(&present));
    CHECK("inclusive lower and upper bounds are accepted",
          rdna4_gather_offset_ok(&present,
                                 RDNA4_GATHER_OFFSET_RUNTIME, -8, -8) &&
          rdna4_gather_offset_ok(&present,
                                 RDNA4_GATHER_OFFSET_RUNTIME, 7, 7));
    CHECK("either component outside the bounds is rejected",
          !rdna4_gather_offset_ok(&present,
                                  RDNA4_GATHER_OFFSET_RUNTIME, -9, 0) &&
          !rdna4_gather_offset_ok(&present,
                                  RDNA4_GATHER_OFFSET_RUNTIME, 0, 8));
    CHECK("runtime Offset and ConstOffsets use the feature gate",
          rdna4_gather_offset_requires_extended(
              RDNA4_GATHER_OFFSET_RUNTIME) &&
          rdna4_gather_offset_requires_extended(
              RDNA4_GATHER_OFFSET_CONST_OFFSETS) &&
          !rdna4_gather_offset_requires_extended(
              RDNA4_GATHER_OFFSET_CONSTANT));
    CHECK("unknown operand kinds fail closed",
          !rdna4_gather_offset_ok(&present,
                                  (rdna4_gather_offset_kind)99, 0, 0));

    const int32_t good_offsets[4][2] = {
        {-8, -8}, {7, 7}, {-1, 3}, {0, 0}
    };
    CHECK("plural ConstOffsets accepts four independently valid offsets",
          rdna4_gather_const_offsets_ok(&present, good_offsets));
    int32_t bad_offsets[4][2] = {
        {-8, -8}, {7, 7}, {-1, 3}, {0, 0}
    };
    bad_offsets[2][1] = 8;
    CHECK("one out-of-range member rejects the whole ConstOffsets operand",
          !rdna4_gather_const_offsets_ok(&present, bad_offsets));
    bad_offsets[2][1] = 3;
    CHECK("plural ConstOffsets requires the feature",
          !rdna4_gather_const_offsets_ok(&absent, good_offsets));
    CHECK("null inputs fail closed",
          !rdna4_gather_caps_supported(NULL) &&
          !rdna4_gather_offset_ok(NULL,
                                  RDNA4_GATHER_OFFSET_CONSTANT, 0, 0) &&
          !rdna4_gather_const_offsets_ok(NULL, good_offsets) &&
          !rdna4_gather_const_offsets_ok(&present, NULL));

    rdna4_gather_caps malformed =
        rdna4_gather_caps_from_vulkan(1, 4, 3);
    CHECK("an inverted range rejects every offset",
          !rdna4_gather_offset_ok(&malformed,
                                  RDNA4_GATHER_OFFSET_CONSTANT, 4, 4) &&
          !rdna4_gather_offset_ok(&malformed,
                                  RDNA4_GATHER_OFFSET_RUNTIME, 0, 0));
    rdna4_gather_caps huge_max =
        rdna4_gather_caps_from_vulkan(1, -8, UINT32_MAX);
    CHECK("uint32 maximum gather limit safely covers all positive int32 offsets",
          rdna4_gather_offset_ok(&huge_max,
                                 RDNA4_GATHER_OFFSET_RUNTIME, INT32_MAX, INT32_MAX) &&
          !rdna4_gather_offset_ok(&huge_max,
                                  RDNA4_GATHER_OFFSET_RUNTIME, INT32_MIN, 0));

    printf("%u checks, %u failures\n", checks, failures);
    return failures ? 1 : 0;
}
