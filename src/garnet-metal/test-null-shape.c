#include "garnet_null_shape.h"

#include <stdio.h>
#include <stdlib.h>

static unsigned checks;

static void require(int condition, const char *message) {
    ++checks;
    if (!condition) {
        fprintf(stderr, "NULL_SHAPE_FAIL %s\n", message);
        exit(1);
    }
}

static rdna4ns_shape shape(unsigned dim, unsigned arrayed, unsigned component,
                           unsigned depth, unsigned multisampled,
                           unsigned access) {
    return (rdna4ns_shape){
        .dim = (uint8_t)dim,
        .arrayed = (uint8_t)arrayed,
        .component = (uint8_t)component,
        .depth = (uint8_t)depth,
        .multisampled = (uint8_t)multisampled,
        .access = (uint8_t)access,
    };
}

static void test_word_layout_and_validation(void) {
    rdna4ns_shape value = shape(RDNA4NS_DIM_2D, 1, RDNA4NS_COMP_UINT,
                                0, 1, RDNA4NS_ACCESS_SAMPLED);
    require(rdna4ns_shape_valid(&value), "2D MS array uint sampled is legal");
    uint32_t word = rdna4ns_shape_encode(&value);
    require(word == 0x99, "canonical word layout");
    rdna4ns_shape decoded;
    require(rdna4ns_shape_decode(word, &decoded) == RDNA4NS_OK,
            "canonical word decodes");
    require(rdna4ns_shape_equal(&value, &decoded), "canonical round trip");

    value = shape(RDNA4NS_DIM_2D, 1, RDNA4NS_COMP_FLOAT, 1, 1,
                  RDNA4NS_ACCESS_SAMPLED);
    require(rdna4ns_shape_valid(&value), "2D MS depth array sampled is legal");

    require(rdna4ns_shape_decode(1u << 10, &decoded) ==
                RDNA4NS_RESERVED_BITS,
            "reserved bits are rejected");
    value = shape(RDNA4NS_DIM_2D, 0, RDNA4NS_COMP_FLOAT, 1, 0,
                  RDNA4NS_ACCESS_STORAGE);
    require(!rdna4ns_shape_valid(&value), "storage depth is rejected");
    value = shape(RDNA4NS_DIM_2D, 0, RDNA4NS_COMP_UINT, 1, 0,
                  RDNA4NS_ACCESS_SAMPLED);
    require(!rdna4ns_shape_valid(&value), "integer depth is rejected");
    value = shape(RDNA4NS_DIM_3D, 1, RDNA4NS_COMP_FLOAT, 0, 0,
                  RDNA4NS_ACCESS_SAMPLED);
    require(!rdna4ns_shape_valid(&value), "3D array is rejected");
    value = shape(RDNA4NS_DIM_CUBE, 0, RDNA4NS_COMP_FLOAT, 0, 0,
                  RDNA4NS_ACCESS_STORAGE);
    require(rdna4ns_shape_valid(&value), "MSL 2.1 cube storage is legal");
    require(rdna4ns_shape_decode(0x103, &decoded) == RDNA4NS_OK &&
                rdna4ns_shape_equal(&value, &decoded),
            "cube storage has an exact canonical word");
    value.arrayed = 1;
    require(rdna4ns_shape_decode(0x10b, &decoded) == RDNA4NS_OK &&
                rdna4ns_shape_equal(&value, &decoded),
            "cube array storage has an exact canonical word");
    value = shape(RDNA4NS_DIM_2D, 0, RDNA4NS_COMP_FLOAT, 0, 1,
                  RDNA4NS_ACCESS_STORAGE);
    require(!rdna4ns_shape_valid(&value), "storage MS is rejected");
    value = shape(RDNA4NS_DIM_2D, 0, RDNA4NS_COMP_FLOAT, 0, 0,
                  RDNA4NS_ACCESS_UNIFORM_TEXEL);
    require(!rdna4ns_shape_valid(&value), "image dim texel is rejected");
    value = shape(RDNA4NS_DIM_BUFFER, 0, RDNA4NS_COMP_FLOAT, 0, 0,
                  RDNA4NS_ACCESS_SAMPLED);
    require(!rdna4ns_shape_valid(&value), "buffer sampled image is rejected");
    value = shape(RDNA4NS_DIM_BUFFER, 0, RDNA4NS_COMP_SINT, 0, 0,
                  RDNA4NS_ACCESS_STORAGE_TEXEL);
    require(rdna4ns_shape_valid(&value), "storage texel buffer is legal");
}

static void test_registry_order_and_ordinals(void) {
    rdna4ns_shape shapes[] = {
        shape(RDNA4NS_DIM_BUFFER, 0, RDNA4NS_COMP_FLOAT, 0, 0,
              RDNA4NS_ACCESS_UNIFORM_TEXEL),
        shape(RDNA4NS_DIM_2D, 0, RDNA4NS_COMP_FLOAT, 0, 0,
              RDNA4NS_ACCESS_SAMPLED),
        shape(RDNA4NS_DIM_2D, 1, RDNA4NS_COMP_FLOAT, 0, 0,
              RDNA4NS_ACCESS_SAMPLED),
        shape(RDNA4NS_DIM_2D, 0, RDNA4NS_COMP_SINT, 0, 0,
              RDNA4NS_ACCESS_STORAGE),
    };
    uint32_t words[4];
    for (unsigned i = 0; i < 4; ++i) words[i] = rdna4ns_shape_encode(&shapes[i]);
    for (unsigned i = 1; i < 4; ++i) {
        uint32_t value = words[i];
        unsigned j = i;
        while (j && value < words[j - 1]) {
            words[j] = words[j - 1];
            --j;
        }
        words[j] = value;
    }

    rdna4ns_registry registry;
    require(rdna4ns_registry_build(words, 4, &registry) == RDNA4NS_OK,
            "sorted registry builds");
    require(registry.access_count[RDNA4NS_ACCESS_SAMPLED] == 2,
            "sampled access count");
    require(registry.access_count[RDNA4NS_ACCESS_STORAGE] == 1,
            "storage access count");
    require(registry.access_count[RDNA4NS_ACCESS_UNIFORM_TEXEL] == 1,
            "uniform texel access count");
    uint16_t ordinal = UINT16_MAX;
    rdna4ns_shape found;
    require(rdna4ns_registry_lookup(
                &registry, rdna4ns_shape_encode(&shapes[2]), &ordinal, &found) ==
                RDNA4NS_OK &&
                ordinal == 1,
            "dense ordinal within access class");
    require(rdna4ns_shape_equal(&found, &shapes[2]), "lookup returns shape");
    uint32_t texel_word = 0;
    rdna4ns_shape texel_shape = {0};
    require(rdna4ns_registry_access_word(&registry,
                RDNA4NS_ACCESS_UNIFORM_TEXEL, 0, &texel_word,
                &texel_shape) == RDNA4NS_OK &&
                texel_word == rdna4ns_shape_encode(&shapes[0]),
            "texel access slot resolves by per-access ordinal");
    uint16_t texel_ordinal = UINT16_MAX;
    require(rdna4ns_registry_texel_ordinal(&registry, &shapes[0],
                                           &texel_ordinal) == RDNA4NS_OK &&
                texel_ordinal == 0,
            "texel lookup returns the dense descriptor slot");

    rdna4ns_registry left = {0}, right = {0}, merged;
    left.words[0] = words[0];
    left.words[1] = words[2];
    left.count = 2;
    right.words[0] = words[1];
    right.words[1] = words[2];
    right.count = 2;
    require(rdna4ns_registry_merge(&left, &right, &merged) == RDNA4NS_OK &&
                merged.count == 3 && merged.words[0] == words[0] &&
                merged.words[1] == words[1] && merged.words[2] == words[2],
            "stage registries merge with deduplication");

    uint32_t duplicate[] = {words[0], words[0]};
    require(rdna4ns_registry_build(duplicate, 2, &registry) ==
                RDNA4NS_DUPLICATE,
            "duplicate words rejected");
    uint32_t reversed[] = {words[1], words[0]};
    require(rdna4ns_registry_build(reversed, 2, &registry) ==
                RDNA4NS_UNSORTED,
            "unsorted words rejected");
    uint32_t unknown[] = {1u << 10};
    require(rdna4ns_registry_build(unknown, 1, &registry) ==
                RDNA4NS_RESERVED_BITS,
            "unknown bits rejected");
}

static void test_capacity_and_legacy(void) {
    rdna4ns_shape shapes[] = {
        shape(RDNA4NS_DIM_1D, 0, RDNA4NS_COMP_FLOAT, 0, 0,
              RDNA4NS_ACCESS_SAMPLED),
        shape(RDNA4NS_DIM_2D, 0, RDNA4NS_COMP_FLOAT, 0, 0,
              RDNA4NS_ACCESS_SAMPLED),
        shape(RDNA4NS_DIM_2D, 0, RDNA4NS_COMP_FLOAT, 0, 0,
              RDNA4NS_ACCESS_STORAGE),
    };
    uint32_t words[3];
    for (unsigned i = 0; i < 3; ++i) words[i] = rdna4ns_shape_encode(&shapes[i]);
    for (unsigned i = 1; i < 3; ++i) {
        uint32_t value = words[i];
        unsigned j = i;
        while (j && value < words[j - 1]) {
            words[j] = words[j - 1];
            --j;
        }
        words[j] = value;
    }
    rdna4ns_registry registry;
    require(rdna4ns_registry_build(words, 3, &registry) == RDNA4NS_OK,
            "capacity registry");
    uint32_t descriptors = 0;
    require(rdna4ns_table_descriptor_count(0, 7, &registry, &descriptors) ==
                RDNA4NS_OK &&
                descriptors == 9,
            "sampled tail count");
    require(rdna4ns_table_descriptor_count(1, 7, &registry, &descriptors) ==
                RDNA4NS_OK &&
                descriptors == 8,
            "storage tail count");
    require(rdna4ns_table_descriptor_count(2, 7, &registry, &descriptors) ==
                RDNA4NS_OK &&
                descriptors == 7,
            "sampler count unchanged");
    require(rdna4ns_table_descriptor_count(0, UINT32_MAX, &registry,
                                           &descriptors) ==
                RDNA4NS_OVERFLOW,
            "tail overflow rejected");

    uint32_t legacy = UINT32_MAX;
    rdna4ns_shape image = shape(RDNA4NS_DIM_CUBE, 1, RDNA4NS_COMP_SINT,
                                0, 0, RDNA4NS_ACCESS_SAMPLED);
    require(rdna4ns_shape_image_legacy(&image, &legacy) == RDNA4NS_OK &&
                legacy == 39,
            "ABI9 image shape maps to the legacy dummy slot");
    rdna4ns_shape texel = shape(RDNA4NS_DIM_BUFFER, 0, RDNA4NS_COMP_FLOAT,
                                0, 0, RDNA4NS_ACCESS_STORAGE_TEXEL);
    require(rdna4ns_shape_image_legacy(&texel, &legacy) ==
                RDNA4NS_INVALID,
            "texel shape cannot use the image dummy factory");

    uint64_t mask = (UINT64_C(1) << 0) | (UINT64_C(1) << 23) |
                    (UINT64_C(1) << 39);
    require(rdna4ns_registry_from_legacy(mask, &registry) == RDNA4NS_OK &&
                registry.count == 3,
            "legacy 48-bit registry conversion");
    require(registry.access_count[RDNA4NS_ACCESS_SAMPLED] == 3,
            "legacy shapes are sampled");
    require(!rdna4ns_registry_from_legacy(UINT64_C(1) << 48, &registry),
            "legacy shape 48 rejected");
    uint32_t word = 0;
    require(rdna4ns_legacy_shape(39, &word) == RDNA4NS_OK &&
                rdna4ns_shape_decode(word, NULL) == RDNA4NS_OK,
            "last legal legacy shape is valid");
    require(rdna4ns_legacy_shape(47, &word) == RDNA4NS_LEGACY_INVALID,
            "legacy ordinal 47 is depth with an integer component");
    require(rdna4ns_legacy_shape(48, &word) == RDNA4NS_LEGACY_RANGE,
            "legacy range ends at 48");
}

static void test_abi_gate(void) {
    rdna4ns_registry registry;
    require(rdna4ns_registry_build_abi(8, NULL, 0, &registry) == RDNA4NS_OK &&
                !registry.count,
            "legacy ABI has no shape list");
    require(rdna4ns_registry_build_abi(9, NULL, 0, &registry) == RDNA4NS_OK,
            "ABI 9 permits an empty list");
    require(rdna4ns_registry_build_abi(10, NULL, 0, &registry) == RDNA4NS_OK,
            "ABI 10 permits an empty list");
    rdna4ns_shape texel =
        shape(RDNA4NS_DIM_BUFFER, 0, RDNA4NS_COMP_FLOAT, 0, 0,
              RDNA4NS_ACCESS_UNIFORM_TEXEL);
    uint32_t texel_word = rdna4ns_shape_encode(&texel);
    require(rdna4ns_registry_build_abi(10, &texel_word, 1, &registry) ==
                    RDNA4NS_OK &&
                registry.count == 1 && registry.words[0] == texel_word,
            "ABI 10 accepts valid uniform texel shape registry");
    require(rdna4ns_registry_build_abi(11, NULL, 0, &registry) ==
                RDNA4NS_UNKNOWN_ABI,
            "future ABI rejected");
    rdna4ns_shape sampled =
        shape(RDNA4NS_DIM_2D, 0, RDNA4NS_COMP_FLOAT, 0, 0,
              RDNA4NS_ACCESS_SAMPLED);
    uint32_t word = rdna4ns_shape_encode(&sampled);
    require(rdna4ns_registry_build_abi(8, &word, 1, &registry) ==
                RDNA4NS_INVALID,
            "legacy ABI cannot smuggle a shape list");
}

static void test_full_matrix_fits_bound(void) {
    uint32_t words[RDNA4NS_MAX_SHAPES + 1] = {0};
    size_t count = 0;
    for (unsigned dim = 0; dim <= RDNA4NS_DIM_BUFFER; ++dim)
        for (unsigned arrayed = 0; arrayed <= 1; ++arrayed)
            for (unsigned component = 0; component <= RDNA4NS_COMP_SINT;
                 ++component)
                for (unsigned depth = 0; depth <= 1; ++depth)
                    for (unsigned multisampled = 0; multisampled <= 1;
                         ++multisampled)
                        for (unsigned access = 0;
                             access < RDNA4NS_ACCESS_COUNT; ++access) {
                            rdna4ns_shape value =
                                shape(dim, arrayed, component, depth,
                                      multisampled, access);
                            if (!rdna4ns_shape_valid(&value)) continue;
                            require(count < RDNA4NS_MAX_SHAPES,
                                    "legal shape matrix fits the ABI bound");
                            words[count++] = rdna4ns_shape_encode(&value);
                        }
    for (size_t i = 1; i < count; ++i) {
        uint32_t value = words[i];
        size_t j = i;
        while (j && value < words[j - 1]) {
            words[j] = words[j - 1];
            --j;
        }
        words[j] = value;
    }
    rdna4ns_registry registry;
    require(count == 60, "all canonical legal shapes are represented");
    require(rdna4ns_registry_build(words, count, &registry) == RDNA4NS_OK,
            "full legal matrix builds");
    require(registry.access_count[RDNA4NS_ACCESS_SAMPLED] == 33 &&
                registry.access_count[RDNA4NS_ACCESS_STORAGE] == 21 &&
                registry.access_count[RDNA4NS_ACCESS_UNIFORM_TEXEL] == 3 &&
                registry.access_count[RDNA4NS_ACCESS_STORAGE_TEXEL] == 3,
            "full legal matrix access counts");
    words[RDNA4NS_MAX_SHAPES] = words[0];
    require(rdna4ns_registry_build(words, RDNA4NS_MAX_SHAPES + 1,
                                   &registry) == RDNA4NS_CAPACITY,
            "one-over registry capacity is rejected before decode");
}

int main(void) {
    test_word_layout_and_validation();
    test_registry_order_and_ordinals();
    test_capacity_and_legacy();
    test_abi_gate();
    test_full_matrix_fits_bound();
    printf("NULL_SHAPE_PASS %u checks\n", checks);
    return 0;
}
