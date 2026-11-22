#ifndef RDNA4_NULL_SHAPE_H
#define RDNA4_NULL_SHAPE_H

/*
 * Versioned, bounded description of a null image or texel-buffer shape.
 *
 * ABI 9 moves away from the 48-bit sampled-image bitmap. Metadata carries a
 * sorted list of canonical shape words; this file validates that list and
 * derives dense per-access ordinals for the descriptor tails. The legacy
 * bitmap remains available for ABI 5..8.
 */

#include <stddef.h>
#include <stdint.h>

#define RDNA4NS_ABI_LEGACY_MIN 5u
#define RDNA4NS_ABI_FULL_SHAPE 9u
#define RDNA4NS_REFLECTION_VERSION 65u
#define RDNA4NS_LEGACY_SHAPES 48u
#define RDNA4NS_SAMPLED_SHAPES 33u
#define RDNA4NS_STORAGE_SHAPES 21u
#define RDNA4NS_TEXEL_SHAPES 3u
#define RDNA4NS_IMAGE_SHAPES \
    (RDNA4NS_SAMPLED_SHAPES + RDNA4NS_STORAGE_SHAPES)
#define RDNA4NS_MAX_SHAPES \
    (RDNA4NS_IMAGE_SHAPES + 2u * RDNA4NS_TEXEL_SHAPES)

/* ABI 9 keeps the three shared resource sets. Texel-buffer null slots live
 * beside the image arrays, using the same access-specific dense ordinal. */
#define RDNA4NS_TABLE_BINDING 672u
#define RDNA4NS_IMAGE_QUERY_BINDING 673u
#define RDNA4NS_UNIFORM_TEXEL_BINDING 1u
#define RDNA4NS_STORAGE_TEXEL_BINDING 1u

/* A shared pipeline token buffer has one header layout across all stages. */
static inline int rdna4ns_table_abi_compatible(uint64_t a, uint64_t b) {
    return !a || !b || a == b;
}

typedef enum {
    RDNA4NS_DIM_1D = 0,
    RDNA4NS_DIM_2D = 1,
    RDNA4NS_DIM_3D = 2,
    RDNA4NS_DIM_CUBE = 3,
    RDNA4NS_DIM_BUFFER = 4,
} rdna4ns_dim;

typedef enum {
    RDNA4NS_COMP_FLOAT = 0,
    RDNA4NS_COMP_UINT = 1,
    RDNA4NS_COMP_SINT = 2,
} rdna4ns_component;

typedef enum {
    RDNA4NS_ACCESS_SAMPLED = 0,
    RDNA4NS_ACCESS_STORAGE = 1,
    RDNA4NS_ACCESS_UNIFORM_TEXEL = 2,
    RDNA4NS_ACCESS_STORAGE_TEXEL = 3,
    RDNA4NS_ACCESS_COUNT = 4,
} rdna4ns_access;

typedef enum {
    RDNA4NS_OK = 0,
    RDNA4NS_INVALID,
    RDNA4NS_RESERVED_BITS,
    RDNA4NS_UNKNOWN_ABI,
    RDNA4NS_DUPLICATE,
    RDNA4NS_UNSORTED,
    RDNA4NS_CAPACITY,
    RDNA4NS_OVERFLOW,
    RDNA4NS_LEGACY_RANGE,
    RDNA4NS_LEGACY_INVALID,
} rdna4ns_status;

typedef struct {
    uint8_t dim;
    uint8_t component;
    uint8_t access;
    uint8_t arrayed;
    uint8_t depth;
    uint8_t multisampled;
} rdna4ns_shape;

typedef struct {
    uint32_t words[RDNA4NS_MAX_SHAPES];
    uint16_t count;
    uint16_t access_count[RDNA4NS_ACCESS_COUNT];
} rdna4ns_registry;

static inline uint32_t rdna4ns_shape_encode(const rdna4ns_shape *shape) {
    if (!shape) return UINT32_MAX;
    return (uint32_t)shape->dim |
           ((uint32_t)(shape->arrayed != 0) << 3) |
           ((uint32_t)shape->component << 4) |
           ((uint32_t)(shape->depth != 0) << 6) |
           ((uint32_t)(shape->multisampled != 0) << 7) |
           ((uint32_t)shape->access << 8);
}

static inline int rdna4ns_shape_valid(const rdna4ns_shape *shape) {
    if (!shape || shape->dim > RDNA4NS_DIM_BUFFER ||
        shape->component > RDNA4NS_COMP_SINT ||
        shape->access >= RDNA4NS_ACCESS_COUNT ||
        shape->arrayed > 1 || shape->depth > 1 || shape->multisampled > 1)
        return 0;

    if (shape->access == RDNA4NS_ACCESS_UNIFORM_TEXEL ||
        shape->access == RDNA4NS_ACCESS_STORAGE_TEXEL) {
        return shape->dim == RDNA4NS_DIM_BUFFER && !shape->arrayed &&
               !shape->depth && !shape->multisampled;
    }
    if (shape->dim == RDNA4NS_DIM_BUFFER) return 0;

    /* A Vulkan 3D image has no array form. MSL 2.1 sections 5.10.6/7
     * permit cube and cube-array writes, with LOD zero on macOS. */
    if (shape->arrayed && shape->dim == RDNA4NS_DIM_3D) return 0;

    /*
     * MSL depth textures are 2D or cube, and depth samples are float-only.
     * A 2D multisample depth image is legal, including its array form.
     * Multisampling is a sampled/depth property; storage images must not
     * carry it.
     */
    if (shape->depth &&
        (shape->access != RDNA4NS_ACCESS_SAMPLED ||
         shape->component != RDNA4NS_COMP_FLOAT ||
         (shape->dim != RDNA4NS_DIM_2D &&
          shape->dim != RDNA4NS_DIM_CUBE)))
        return 0;
    if (shape->multisampled &&
        (shape->access != RDNA4NS_ACCESS_SAMPLED ||
         shape->dim != RDNA4NS_DIM_2D))
        return 0;
    return 1;
}

static inline rdna4ns_status rdna4ns_shape_decode(uint32_t word,
                                                  rdna4ns_shape *shape) {
    if (word & ~UINT32_C(0x3ff)) return RDNA4NS_RESERVED_BITS;
    rdna4ns_shape decoded = {
        .dim = (uint8_t)(word & 7u),
        .arrayed = (uint8_t)((word >> 3) & 1u),
        .component = (uint8_t)((word >> 4) & 3u),
        .depth = (uint8_t)((word >> 6) & 1u),
        .multisampled = (uint8_t)((word >> 7) & 1u),
        .access = (uint8_t)((word >> 8) & 3u),
    };
    if (!rdna4ns_shape_valid(&decoded)) return RDNA4NS_INVALID;
    if (shape) *shape = decoded;
    return RDNA4NS_OK;
}

static inline int rdna4ns_shape_equal(const rdna4ns_shape *a,
                                      const rdna4ns_shape *b) {
    return a && b && a->dim == b->dim && a->component == b->component &&
           a->access == b->access && a->arrayed == b->arrayed &&
           a->depth == b->depth && a->multisampled == b->multisampled;
}

/* True for descriptors carried by the ABI argument-table image tails. */
static inline int rdna4ns_access_is_table_image(unsigned access) {
    return access == RDNA4NS_ACCESS_SAMPLED ||
           access == RDNA4NS_ACCESS_STORAGE;
}

static inline int rdna4ns_access_is_texel(unsigned access) {
    return access == RDNA4NS_ACCESS_UNIFORM_TEXEL ||
           access == RDNA4NS_ACCESS_STORAGE_TEXEL;
}

static inline int rdna4ns_access_is_image(unsigned access) {
    return access == RDNA4NS_ACCESS_SAMPLED ||
           access == RDNA4NS_ACCESS_STORAGE;
}

/*
 * Build a registry from the canonical ABI 9 metadata list. The wire order is
 * strictly ascending by the full 32-bit word, with no duplicates. Ordinals are
 * dense within each access class; the complete list remains globally sorted so
 * the translator and native decoder do not depend on discovery order.
 */
static inline rdna4ns_status rdna4ns_registry_build(const uint32_t *words,
                                                     size_t count,
                                                     rdna4ns_registry *out) {
    if (!out || (!words && count)) return RDNA4NS_INVALID;
    if (count > RDNA4NS_MAX_SHAPES) return RDNA4NS_CAPACITY;
    *out = (rdna4ns_registry){0};
    for (size_t i = 0; i < count; ++i) {
        rdna4ns_shape shape;
        rdna4ns_status status = rdna4ns_shape_decode(words[i], &shape);
        if (status != RDNA4NS_OK) return status;
        if (i && words[i] == words[i - 1]) return RDNA4NS_DUPLICATE;
        if (i && words[i] < words[i - 1]) return RDNA4NS_UNSORTED;
        out->words[i] = words[i];
        ++out->access_count[shape.access];
        if ((shape.access == RDNA4NS_ACCESS_UNIFORM_TEXEL ||
             shape.access == RDNA4NS_ACCESS_STORAGE_TEXEL) &&
            out->access_count[shape.access] > RDNA4NS_TEXEL_SHAPES)
            return RDNA4NS_CAPACITY;
    }
    if ((uint32_t)out->access_count[RDNA4NS_ACCESS_SAMPLED] +
            out->access_count[RDNA4NS_ACCESS_STORAGE] >
        RDNA4NS_IMAGE_SHAPES)
        return RDNA4NS_CAPACITY;
    out->count = (uint16_t)count;
    return RDNA4NS_OK;
}

static inline rdna4ns_status rdna4ns_registry_build_abi(
    uint32_t table_abi, const uint32_t *words, size_t count,
    rdna4ns_registry *out) {
    if (table_abi < RDNA4NS_ABI_FULL_SHAPE) {
        if (!out || words || count) return RDNA4NS_INVALID;
        *out = (rdna4ns_registry){0};
        return RDNA4NS_OK;
    }
    if (table_abi != RDNA4NS_ABI_FULL_SHAPE) return RDNA4NS_UNKNOWN_ABI;
    return rdna4ns_registry_build(words, count, out);
}

static inline rdna4ns_status rdna4ns_registry_lookup(
    const rdna4ns_registry *registry, uint32_t word, uint16_t *ordinal,
    rdna4ns_shape *shape) {
    if (!registry || registry->count > RDNA4NS_MAX_SHAPES)
        return RDNA4NS_INVALID;
    uint16_t seen[RDNA4NS_ACCESS_COUNT] = {0};
    for (uint16_t i = 0; i < registry->count; ++i) {
        rdna4ns_shape item;
        rdna4ns_status status = rdna4ns_shape_decode(registry->words[i], &item);
        if (status != RDNA4NS_OK) return status;
        if (registry->words[i] == word) {
            if (ordinal) *ordinal = seen[item.access];
            if (shape) *shape = item;
            return RDNA4NS_OK;
        }
        ++seen[item.access];
    }
    return RDNA4NS_INVALID;
}

static inline uint16_t rdna4ns_registry_access_count(
    const rdna4ns_registry *registry, unsigned access) {
    return registry && access < RDNA4NS_ACCESS_COUNT
               ? registry->access_count[access]
               : 0;
}

static inline rdna4ns_status rdna4ns_registry_access_word(
    const rdna4ns_registry *registry, unsigned access, uint16_t ordinal,
    uint32_t *word, rdna4ns_shape *shape) {
    if (!registry || access >= RDNA4NS_ACCESS_COUNT ||
        registry->count > RDNA4NS_MAX_SHAPES)
        return RDNA4NS_INVALID;
    uint16_t seen = 0;
    for (uint16_t i = 0; i < registry->count; ++i) {
        rdna4ns_shape item;
        rdna4ns_status status = rdna4ns_shape_decode(registry->words[i], &item);
        if (status != RDNA4NS_OK) return status;
        if (item.access != access) continue;
        if (ordinal != seen++) continue;
        if (word) *word = registry->words[i];
        if (shape) *shape = item;
        return RDNA4NS_OK;
    }
    return RDNA4NS_INVALID;
}

/* ABI 10 keeps live token indices at zero and places typed null views after them. */
static inline rdna4ns_status rdna4ns_texel_null_index(
    uint32_t token_count, uint32_t ordinal, uint32_t *descriptor_index) {
    if (!token_count || !descriptor_index) return RDNA4NS_INVALID;
    if (ordinal > UINT32_MAX - token_count) return RDNA4NS_OVERFLOW;
    *descriptor_index = token_count + ordinal;
    return RDNA4NS_OK;
}

static inline rdna4ns_status rdna4ns_texel_descriptor_count(
    uint32_t token_count, uint32_t null_shape_count,
    uint32_t *descriptor_count) {
    if (!token_count || !descriptor_count) return RDNA4NS_INVALID;
    if (null_shape_count > UINT32_MAX - token_count)
        return RDNA4NS_OVERFLOW;
    *descriptor_count = token_count + null_shape_count;
    return RDNA4NS_OK;
}

/* Union two already-canonical stage registries into one canonical registry. */
static inline rdna4ns_status rdna4ns_registry_merge(
    const rdna4ns_registry *a, const rdna4ns_registry *b,
    rdna4ns_registry *out) {
    if (!a || !b || !out || a->count > RDNA4NS_MAX_SHAPES ||
        b->count > RDNA4NS_MAX_SHAPES)
        return RDNA4NS_INVALID;
    uint32_t words[RDNA4NS_MAX_SHAPES];
    size_t i = 0, j = 0, count = 0;
    while (i < a->count || j < b->count) {
        uint32_t next;
        if (j == b->count || (i < a->count && a->words[i] < b->words[j]))
            next = a->words[i++];
        else if (i == a->count || b->words[j] < a->words[i])
            next = b->words[j++];
        else {
            next = a->words[i];
            ++i;
            ++j;
        }
        if (count == RDNA4NS_MAX_SHAPES) return RDNA4NS_CAPACITY;
        words[count++] = next;
    }
    return rdna4ns_registry_build(words, count, out);
}

/* Binding 674 maps a stage's global sorted position to a shared access ordinal.
 * The output may alias local_words; the validated local copy keeps this safe. */
static inline rdna4ns_status rdna4ns_registry_remap(
    const rdna4ns_registry *shared, const uint32_t *local_words, size_t count,
    uint32_t *ordinals, uint8_t *accesses) {
    if (!shared || (!ordinals && count) || (!accesses && count)) return RDNA4NS_INVALID;
    rdna4ns_registry local;
    rdna4ns_status status = rdna4ns_registry_build(local_words, count, &local);
    if (status != RDNA4NS_OK) return status;
    for (size_t i = 0; i < count; ++i) {
        rdna4ns_shape shape;
        uint16_t ordinal = 0;
        status = rdna4ns_registry_lookup(shared, local.words[i], &ordinal, &shape);
        if (status != RDNA4NS_OK) return status;
        ordinals[i] = ordinal;
        accesses[i] = shape.access;
    }
    return RDNA4NS_OK;
}

/*
 * ABI 5..8 legacy shape:
 *   2*dim + arrayed + 8*depth + 16*component, sampled and non-MS only.
 * Bit order is intentionally preserved; it is not the ABI 9 word layout.
 */
static inline rdna4ns_status rdna4ns_legacy_shape(uint32_t legacy,
                                                  uint32_t *word) {
    if (legacy >= RDNA4NS_LEGACY_SHAPES) return RDNA4NS_LEGACY_RANGE;
    rdna4ns_shape shape = {
        .dim = (uint8_t)((legacy & 7u) >> 1),
        .arrayed = (uint8_t)(legacy & 1u),
        .depth = (uint8_t)((legacy >> 3) & 1u),
        .component = (uint8_t)((legacy >> 4) & 3u),
        .multisampled = 0,
        .access = RDNA4NS_ACCESS_SAMPLED,
    };
    if (!rdna4ns_shape_valid(&shape)) return RDNA4NS_LEGACY_INVALID;
    if (word) *word = rdna4ns_shape_encode(&shape);
    return RDNA4NS_OK;
}

/* Convert an ABI 5..8 nullImages bitmap into the ABI 9 sorted registry form. */
static inline rdna4ns_status rdna4ns_registry_from_legacy(
    uint64_t mask, rdna4ns_registry *out) {
    if (!out) return RDNA4NS_INVALID;
    uint32_t words[RDNA4NS_LEGACY_SHAPES];
    size_t count = 0;
    for (uint32_t legacy = 0; legacy < RDNA4NS_LEGACY_SHAPES; ++legacy) {
        if (!(mask & (UINT64_C(1) << legacy))) continue;
        rdna4ns_status status = rdna4ns_legacy_shape(legacy, &words[count]);
        if (status != RDNA4NS_OK) return status;
        ++count;
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
    return rdna4ns_registry_build(words, count, out);
}

/*
 * Map an ABI9 sampled/storage image shape to the ABI5..8 48-slot shape word.
 * This is only a native dummy-factory bridge. Texel-buffer shapes deliberately
 * fail because they require VkBufferView, not an image view.
 */
static inline rdna4ns_status rdna4ns_shape_image_legacy(
    const rdna4ns_shape *shape, uint32_t *legacy) {
    if (!shape || !rdna4ns_access_is_image(shape->access))
        return RDNA4NS_INVALID;
    if (!rdna4ns_shape_valid(shape) || shape->dim > RDNA4NS_DIM_CUBE)
        return RDNA4NS_INVALID;
    uint32_t value = 2u * shape->dim + (shape->arrayed != 0) +
                     8u * (shape->depth != 0) +
                     16u * shape->component;
    if (value >= RDNA4NS_LEGACY_SHAPES) return RDNA4NS_LEGACY_RANGE;
    if (legacy) *legacy = value;
    return RDNA4NS_OK;
}

/*
 * Texel-buffer identities have exactly three legal component variants per
 * access class. Their descriptor ordinal is the per-access sorted position,
 * not a bit number.
 */
static inline rdna4ns_status rdna4ns_registry_texel_ordinal(
    const rdna4ns_registry *registry, const rdna4ns_shape *shape,
    uint16_t *ordinal) {
    if (!registry || !shape || !rdna4ns_access_is_texel(shape->access))
        return RDNA4NS_INVALID;
    uint32_t word = rdna4ns_shape_encode(shape);
    return rdna4ns_registry_lookup(registry, word, ordinal, NULL);
}

/*
 * ABI 9 image tables append per-access null slots after the real tokens:
 * sampled to category 0 and storage to category 1. Texel buffers are not
 * members of these image arrays and must be bound by their own descriptor
 * path. Category 2 (samplers) is unaffected.
 */
static inline rdna4ns_status rdna4ns_table_descriptor_count(
    unsigned category, uint32_t token_count,
    const rdna4ns_registry *registry, uint32_t *descriptor_count) {
    if (!registry || !descriptor_count || category > 2)
        return RDNA4NS_INVALID;
    uint64_t count = token_count;
    if (category < 2) {
        unsigned access =
            category ? RDNA4NS_ACCESS_STORAGE : RDNA4NS_ACCESS_SAMPLED;
        count += registry->access_count[access];
    }
    if (count > UINT32_MAX) return RDNA4NS_OVERFLOW;
    *descriptor_count = (uint32_t)count;
    return RDNA4NS_OK;
}

#endif
