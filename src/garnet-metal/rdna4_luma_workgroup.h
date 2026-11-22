#ifndef RDNA4_LUMA_WORKGROUP_H
#define RDNA4_LUMA_WORKGROUP_H

enum { RDNA4_LUMA_WORKGROUP_SPEC = 3, RDNA4_LUMA_ELEMENT_BYTES = 16 };

static int rdna4_luma_workgroup(const char *kernel, const RDNA4sMod *module, const uint32_t *words,
                                size_t count, uint32_t **output) {
    *output = NULL;
    if (!kernel || (strcmp(kernel, "compute_average_luma") && strcmp(kernel, "compute_sum_luma"))) return 0;

    uint32_t arrayId = 0, workgroupId = 0;
    for (uint32_t id = 1; id < module->bound; ++id) {
        const RDNA4sId *variable = &module->id[id];
        if (variable->op != 59 || variable->sc != 4) continue;
        if (arrayId || variable->elem >= module->bound) return 0;
        const RDNA4sId *pointer = &module->id[variable->elem];
        if (pointer->op != 32 || pointer->sc != 4 || pointer->elem >= module->bound) return 0;
        arrayId = pointer->elem;
        workgroupId = id;
    }
    if (!arrayId) return 0;
    const RDNA4sId *array = &module->id[arrayId];
    if (array->op != 28 || array->n >= module->bound || array->elem >= module->bound ||
        (array->stride && array->stride != RDNA4_LUMA_ELEMENT_BYTES)) return 0;
    const RDNA4sId *vector = &module->id[array->elem];
    if (vector->op != 23 || vector->n != 4 || vector->elem >= module->bound) return 0;
    const RDNA4sId *scalar = &module->id[vector->elem];
    if (scalar->op != 22 || scalar->width != 32) return 0;

    size_t firstType = 0, arrayWord = 0, clearWord = 0;
    uint32_t lengthType = 0, lengthValue = 0, nullArray = 0;
    int inEntry = 0;
    const int average = !strcmp(kernel, "compute_average_luma") && module->has_ep && module->ep_model == 5;
    for (size_t pos = 5; pos < count; pos += words[pos] >> 16) {
        const uint32_t opcode = words[pos] & 0xffff, size = words[pos] >> 16;
        if (opcode == 71 && size == 4 && words[pos + 2] == 1 &&
            words[pos + 3] == RDNA4_LUMA_WORKGROUP_SPEC) return 0;
        if (!firstType && opcode >= 19 && opcode <= 39) firstType = pos;
        if (opcode == 43 && size == 4 && words[pos + 2] == array->n) {
            lengthType = words[pos + 1];
            lengthValue = words[pos + 3];
        }
        if (opcode == 28 && size == 4 && words[pos + 1] == arrayId) arrayWord = pos;
        if (!average) continue;
        if (opcode == 46 && size == 3 && words[pos + 1] == arrayId) nullArray = words[pos + 2];
        if (opcode == 54 && size == 5) inEntry = words[pos + 2] == module->ep_id;
        if (inEntry && opcode == 248 && size == 2) {
            inEntry = 0;
            const size_t start = pos + 2;
            if (nullArray && count - start >= 7 && words[start] == ((3u << 16) | 62) &&
                words[start + 1] == workgroupId && words[start + 2] == nullArray &&
                words[start + 3] == ((4u << 16) | 224)) {
                const uint32_t scope = words[start + 4], semantics = words[start + 6];
                if (scope < module->bound && semantics < module->bound && words[start + 5] == scope &&
                    module->id[scope].op == 43 && module->id[scope].cval == 2 &&
                    module->id[semantics].op == 43 && module->id[semantics].cval == 264)
                    clearWord = start;
            }
        }
    }
    if (!firstType || arrayWord < firstType || !lengthValue || lengthType >= module->bound ||
        module->id[lengthType].op != 21 || module->id[lengthType].width != 32) return 0;

    uint32_t *result = malloc((count + 8) * sizeof(*result));
    if (!result) return 0;
    const uint32_t lengthId = module->bound;
    const uint32_t decoration[] = {(4u << 16) | 71, lengthId, 1, RDNA4_LUMA_WORKGROUP_SPEC};
    const uint32_t constant[] = {(4u << 16) | 50, lengthType, lengthId, lengthValue};
    memcpy(result, words, firstType * sizeof(*words));
    memcpy(result + firstType, decoration, sizeof(decoration));
    memcpy(result + firstType + 4, words + firstType, (arrayWord - firstType) * sizeof(*words));
    memcpy(result + arrayWord + 4, constant, sizeof(constant));
    memcpy(result + arrayWord + 8, words + arrayWord, (count - arrayWord) * sizeof(*words));
    result[arrayWord + 11] = lengthId;
    if (clearWord > arrayWord)
        for (size_t index = 0; index < 7; ++index) result[clearWord + 8 + index] = 1u << 16;
    result[3] = lengthId + 1;
    *output = result;
    return 1;
}

static inline int rdna4_luma_workgroup_bytes_ok(uint64_t bytes, uint32_t limit) {
    return bytes <= limit && bytes % RDNA4_LUMA_ELEMENT_BYTES == 0;
}

static inline int rdna4_luma_workgroup_fits(uint64_t bytes, const uint32_t local[3]) {
    return bytes / RDNA4_LUMA_ELEMENT_BYTES >= (uint64_t)local[0] * local[1] * local[2];
}

#endif
