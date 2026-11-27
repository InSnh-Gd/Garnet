#ifndef RDNA4_INDIRECT_COMPUTE_H
#define RDNA4_INDIRECT_COMPUTE_H

#include "garnet_spvrefl.h"

#define RDNA4I_MAX_WORDS (16u * 1024u * 1024u)

/* Whole-group indirect dispatch gets its grid from the GPU, without CPU argument reads. */
static inline int rdna4s_indirect_compute(const uint32_t *words, size_t count, uint32_t offset,
                                const uint32_t local[3], uint32_t **output, size_t *output_count,
                                char *why, size_t why_size) {
    RDNA4sMod module = {0};
    uint32_t *slots = NULL, *result = NULL;
    int failed = 1;
    *output = NULL;
    *output_count = 0;
#define RDNA4I_FAIL(message) do { snprintf(why, why_size, "%s", message); goto done; } while (0)
    if (count < 5 || count > RDNA4I_MAX_WORDS || !local[0] || !local[1] || !local[2])
        RDNA4I_FAIL("invalid indirect compute module or local size");
    if (rdna4s_parse(words, count, &module, why, why_size)) goto done;
    if (!module.has_ep || module.ep_model != 5) RDNA4I_FAIL("indirect module is not a compute kernel");
    uint32_t push = 0, scalar = 0, vector = 0, pointer = 0, groups = 0;
    for (uint32_t id = 1; id < module.bound; ++id) {
        RDNA4sId *value = &module.id[id];
        if (value->op != 59 || value->sc != 9) continue;
        if (push) RDNA4I_FAIL("multiple compute push-constant blocks");
        RDNA4sId *type = &module.id[module.id[value->elem].elem];
        if (type->op != 30 || type->n != 12 || !type->moff)
            RDNA4I_FAIL("indirect dispatch metadata is not twelve scalar words");
        scalar = type->mem[0];
        if (module.id[scalar].op != 21 || module.id[scalar].width != 32)
            RDNA4I_FAIL("indirect dispatch metadata is not 32-bit integer data");
        for (uint32_t member = 0; member < 12; ++member)
            if (type->mem[member] != scalar || type->moff[member] != offset + 4 * member)
                RDNA4I_FAIL("indirect dispatch metadata layout differs from the translator ABI");
        push = id;
    }
    if (!push) {
        result = malloc(count * sizeof(*result));
        if (!result) RDNA4I_FAIL("indirect compute transformation allocation failed");
        memcpy(result, words, count * sizeof(*result));
        *output = result;
        *output_count = count;
        result = NULL;
        failed = 0;
        goto done;
    }
    for (uint32_t id = 1; id < module.bound; ++id) {
        RDNA4sId *type = &module.id[id];
        if (type->op == 23 && type->elem == scalar && type->n == 3) { vector = id; break; }
    }
    for (uint32_t id = 1; vector && id < module.bound; ++id) {
        RDNA4sId *type = &module.id[id];
        if (type->op == 32 && type->sc == 1 && type->elem == vector) { pointer = id; break; }
    }
    for (uint32_t id = 1; pointer && id < module.bound; ++id) {
        RDNA4sId *value = &module.id[id];
        if (value->op == 59 && value->sc == 1 && value->elem == pointer && value->builtin == 24) {
            groups = id;
            break;
        }
    }
    slots = calloc((size_t)module.bound + 1, sizeof(*slots));
    result = malloc((count * 4 + 64) * sizeof(*result));
    if (!slots || !result) RDNA4I_FAIL("indirect compute transformation allocation failed");
    uint32_t next = words[3];
    int add_vector = !vector, add_pointer = !pointer, add_groups = !groups;
    if (add_vector) vector = next++;
    if (add_pointer) pointer = next++;
    if (add_groups) groups = next++;
    uint32_t zero = next++, locals[3];
    for (unsigned dimension = 0; dimension < 3; ++dimension) locals[dimension] = next++;
    size_t length = 5;
    memcpy(result, words, 5 * sizeof(*words));
    int decorated = 0, declared = 0, entries = 0;
#define RDNA4I_EMIT(opcode, ...) do { \
    const uint32_t operands[] = {__VA_ARGS__}; \
    const size_t size = sizeof(operands) / sizeof(operands[0]); \
    result[length++] = (uint32_t)((size + 1) << 16) | (opcode); \
    memcpy(result + length, operands, sizeof(operands)); length += size; \
} while (0)
    for (size_t position = 5; position < count;) {
        const uint32_t *instruction = words + position;
        uint32_t size = instruction[0] >> 16, opcode = instruction[0] & 0xffff;
        if (!decorated && opcode >= 19 && opcode <= 39) {
            if (add_groups) RDNA4I_EMIT(71, groups, 11, 24);
            decorated = 1;
        }
        if (!declared && opcode == 54) {
            if (add_vector) RDNA4I_EMIT(23, vector, scalar, 3);
            if (add_pointer) RDNA4I_EMIT(32, pointer, 1, vector);
            if (add_groups) RDNA4I_EMIT(59, pointer, groups, 1);
            RDNA4I_EMIT(43, scalar, zero, 0);
            for (unsigned dimension = 0; dimension < 3; ++dimension)
                RDNA4I_EMIT(43, scalar, locals[dimension], local[dimension]);
            declared = 1;
        }
        if (opcode == 15) {
            if (++entries != 1) RDNA4I_FAIL("multiple entry points in an indirect compute module");
            memcpy(result + length, instruction, size * sizeof(*words));
            if (add_groups) {
                result[length] += 1u << 16;
                result[length + size] = groups;
                ++length;
            }
            length += size;
            position += size;
            continue;
        }
        if ((opcode == 65 || opcode == 66) && size >= 4 && instruction[3] == push) {
            if (size != 5 || instruction[2] >= module.bound || instruction[4] >= module.bound ||
                module.id[instruction[4]].op != 43 || module.id[instruction[4]].cval >= 12)
                RDNA4I_FAIL("unsupported access to indirect dispatch metadata");
            slots[instruction[2]] = module.id[instruction[4]].cval + 1;
        }
        if (opcode == 61 && size >= 4 && instruction[3] < module.bound && slots[instruction[3]]) {
            if (size != 4 || instruction[1] != scalar)
                RDNA4I_FAIL("unsupported load from indirect dispatch metadata");
            uint32_t member = slots[instruction[3]] - 1, id = instruction[2];
            if (member >= 3 && member < 9) {
                RDNA4I_EMIT(83, scalar, id, zero);
            } else {
                uint32_t loaded = next++, dimension = member % 3;
                RDNA4I_EMIT(61, vector, loaded, groups);
                if (member < 3) {
                    uint32_t extracted = next++;
                    RDNA4I_EMIT(81, scalar, extracted, loaded, dimension);
                    RDNA4I_EMIT(132, scalar, id, extracted, locals[dimension]);
                } else {
                    RDNA4I_EMIT(81, scalar, id, loaded, dimension);
                }
            }
        } else {
            if (opcode == 61 && size >= 4 && instruction[3] == push)
                RDNA4I_FAIL("whole-block indirect dispatch metadata load is unsupported");
            memcpy(result + length, instruction, size * sizeof(*words));
            length += size;
        }
        position += size;
    }
    if (!decorated || !declared) RDNA4I_FAIL("indirect compute module has no type or function section");
    result[3] = next;
    *output = result;
    *output_count = length;
    result = NULL;
    failed = 0;
done:
    free(result);
    free(slots);
    rdna4s_free(&module);
#undef RDNA4I_EMIT
#undef RDNA4I_FAIL
    return failed;
}

#endif
