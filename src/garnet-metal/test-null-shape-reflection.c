#include "garnet_spvrefl.h"

#include <stdio.h>
#include <stdlib.h>

static unsigned checks;

static void require(int condition, const char *message) {
    ++checks;
    if (!condition) {
        fprintf(stderr, "NULL_SHAPE_REFLECTION_FAIL %s\n", message);
        exit(1);
    }
}

static uint32_t *read_words(const char *path, size_t *count) {
    FILE *file = fopen(path, "rb");
    if (!file) return NULL;
    if (fseek(file, 0, SEEK_END) || ftell(file) <= 0) {
        fclose(file);
        return NULL;
    }
    long size = ftell(file);
    if (size <= 0 || size % 4) {
        fclose(file);
        return NULL;
    }
    rewind(file);
    uint32_t *words = malloc((size_t)size);
    if (!words || fread(words, 1, (size_t)size, file) != (size_t)size) {
        free(words);
        fclose(file);
        return NULL;
    }
    fclose(file);
    *count = (size_t)size / 4;
    return words;
}

static uint32_t expected_word(unsigned dim, unsigned arrayed,
                              unsigned component, unsigned depth,
                              unsigned multisampled, unsigned access) {
    rdna4ns_shape shape = {
        .dim = (uint8_t)dim,
        .arrayed = (uint8_t)arrayed,
        .component = (uint8_t)component,
        .depth = (uint8_t)depth,
        .multisampled = (uint8_t)multisampled,
        .access = (uint8_t)access,
    };
    return rdna4ns_shape_encode(&shape);
}

int main(int argc, char **argv) {
    require(argc == 3, "usage: test <valid.spv> <invalid.spv>");
    RDNA4sMod module = {0};
    RDNA4sRefl reflection = {0};
    char why[256] = {0};
    size_t count = 0;
    uint32_t *words = read_words(argv[1], &count);
    require(words != NULL, "read valid fixture");
    require(rdna4s_parse(words, count, &module, why, sizeof why) == 0,
            "parse valid fixture");
    require(rdna4s_reflect(&module, 2, &reflection) == 0,
            "reflect valid fixture");
    require(!reflection.argumentNullShapesInvalid,
            "valid fixture has no unknown shapes");
    require(reflection.argumentNullShapeCount == 15,
            "fifteen unique typed shapes");
    const uint32_t expected[] = {
        expected_word(1, 0, 0, 0, 0, 0),
        expected_word(1, 1, 0, 0, 0, 0),
        expected_word(1, 0, 0, 1, 0, 0),
        expected_word(1, 1, 0, 1, 0, 0),
        expected_word(1, 0, 0, 0, 1, 0),
        expected_word(1, 1, 0, 0, 1, 0),
        expected_word(1, 0, 0, 0, 0, 1),
        expected_word(2, 0, 1, 0, 0, 1),
        expected_word(1, 1, 1, 0, 0, 1),
        expected_word(4, 0, 0, 0, 0, 2),
        expected_word(4, 0, 1, 0, 0, 2),
        expected_word(4, 0, 2, 0, 0, 2),
        expected_word(4, 0, 0, 0, 0, 3),
        expected_word(4, 0, 1, 0, 0, 3),
        expected_word(4, 0, 2, 0, 0, 3),
    };
    uint32_t sorted[15];
    memcpy(sorted, expected, sizeof expected);
    for (unsigned i = 1; i < 15; ++i) {
        uint32_t value = sorted[i];
        unsigned j = i;
        while (j && value < sorted[j - 1]) {
            sorted[j] = sorted[j - 1];
            --j;
        }
        sorted[j] = value;
    }
    require(memcmp(reflection.argumentNullShapes, sorted,
                   sizeof sorted) == 0,
            "canonical sorted shapes");
    require(reflection.argumentNullImages ==
                ((UINT64_C(1) << 2) | (UINT64_C(1) << 3) |
                 (UINT64_C(1) << 10) | (UINT64_C(1) << 11)),
            "legacy sampled bitmap preserved");
    require(reflection.bind[2][0].used &&
                reflection.bind[2][0].type ==
                    VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE &&
                reflection.bind[3][0].used &&
                reflection.bind[3][0].type ==
                    VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
            "table descriptor categories");
    require(reflection.bind[2][1].used &&
                reflection.bind[2][1].type ==
                    VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER &&
                reflection.bind[3][1].used &&
                reflection.bind[3][1].type ==
                    VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER,
            "texel descriptor bindings");
    require((reflection.descriptor_features &
                (RDNA4S_NONUNIFORM_UNIFORM_TEXEL | RDNA4S_NONUNIFORM_STORAGE_TEXEL)) ==
                (RDNA4S_NONUNIFORM_UNIFORM_TEXEL | RDNA4S_NONUNIFORM_STORAGE_TEXEL) &&
                !(reflection.descriptor_features & RDNA4S_NONUNIFORM_OTHER),
            "texel nonuniform capabilities have their own backend gates");
    rdna4s_free(&module);
    free(words);

    memset(&module, 0, sizeof module);
    memset(&reflection, 0, sizeof reflection);
    why[0] = '\0';
    words = read_words(argv[2], &count);
    require(words != NULL, "read invalid fixture");
    require(rdna4s_parse(words, count, &module, why, sizeof why) == 0,
            "parse invalid fixture");
    require(rdna4s_reflect(&module, 2, &reflection) == 0,
            "reflect invalid fixture");
    require(reflection.argumentNullShapesInvalid,
            "invalid multisample shape is flagged");
    rdna4s_free(&module);
    free(words);

    printf("NULL_SHAPE_REFLECTION_PASS %u checks\n", checks);
    return 0;
}
