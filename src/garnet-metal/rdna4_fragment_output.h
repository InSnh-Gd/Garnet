#ifndef RDNA4_FRAGMENT_OUTPUT_H
#define RDNA4_FRAGMENT_OUTPUT_H

typedef struct {
    uint32_t id;
    uint32_t lanes;
    size_t declaration;
} RDNA4FragmentOutput;

static inline uint32_t rdna4s_fragment_output_lanes(const RDNA4FragmentOutput *outputs, size_t count, uint32_t id)
{
    for (size_t idx = 0; idx < count; idx++)
        if (outputs[idx].id == id) return outputs[idx].lanes;
    return 0;
}

static inline int rdna4s_widen_fragment_output(const uint32_t *words, size_t *count, const RDNA4sMod *module,
                                       uint32_t **normalized)
{
    *normalized = NULL;
    if (!module->has_ep || module->ep_model != 4) return 0;
    RDNA4FragmentOutput outputs[8];
    size_t output_count = 0;
    uint32_t float_type[5] = {0}, pointer_type[5] = {0};
    for (uint32_t id = 1; id < module->bound; id++) {
        const RDNA4sId *var = &module->id[id];
        if (var->op == 22 && var->width == 32) float_type[1] = id;
        if (var->op != 59 || var->sc != 3 || var->loc < 0 || var->builtin >= 0 || var->elem >= module->bound)
            continue;
        const RDNA4sId *pointer = &module->id[var->elem];
        if (pointer->op != 32 || pointer->sc != 3 || pointer->elem >= module->bound) continue;
        const RDNA4sId *type = &module->id[pointer->elem];
        uint32_t lanes = 1;
        if (type->op == 23 && type->elem < module->bound && type->n >= 2 && type->n <= 4) {
            lanes = type->n;
            type = &module->id[type->elem];
        }
        if (type->op != 22 || type->width != 16) continue;
        if (output_count == sizeof outputs / sizeof outputs[0]) return 0;
        outputs[output_count++] = (RDNA4FragmentOutput){id, lanes, 0};
    }
    if (!output_count) return 0;
    if (float_type[1]) {
        for (uint32_t id = 1; id < module->bound; id++) {
            const RDNA4sId *type = &module->id[id];
            if (type->op == 23 && type->elem == float_type[1] && type->n >= 2 && type->n <= 4)
                float_type[type->n] = id;
        }
        for (uint32_t id = 1; id < module->bound; id++) {
            const RDNA4sId *type = &module->id[id];
            if (type->op != 32 || type->sc != 3) continue;
            for (uint32_t lanes = 1; lanes <= 4; lanes++)
                if (float_type[lanes] && type->elem == float_type[lanes]) pointer_type[lanes] = id;
        }
    }

    size_t functions = 0, stores = 0, entries = 0;
    int float16 = 0;
    for (size_t pos = 5; pos < *count;) {
        uint32_t op = words[pos] & 0xffff, width = words[pos] >> 16;
        if (!width || width > *count - pos) return 0;
        if (op == 54 && !functions) functions = pos;
        if (op == 15) entries++;
        if (op == 17 && width == 2 && words[pos + 1] == 9) float16 = 1;
        if (functions || op == 74) {
            size_t first = 1;
            if (functions && op == 62 && width >= 3 &&
                rdna4s_fragment_output_lanes(outputs, output_count, words[pos + 1])) {
                stores++;
                first = 2;
            }
            for (size_t idx = first; idx < width; idx++) {
                if (op == 12 && idx == 4) continue;
                if (rdna4s_fragment_output_lanes(outputs, output_count, words[pos + idx])) return 0;
            }
        } else if (op == 59 && width >= 4) {
            if (width >= 5 && rdna4s_fragment_output_lanes(outputs, output_count, words[pos + 4])) return 0;
            for (size_t idx = 0; idx < output_count; idx++) {
                if (outputs[idx].id != words[pos + 2]) continue;
                if (width != 4) return 0;
                outputs[idx].declaration = pos;
            }
        }
        pos += width;
    }
    if (!float16 || entries != 1 || !stores || !functions || stores > 0x3fffffu - module->bound - 8) return 0;
    for (size_t idx = 0; idx < output_count; idx++)
        if (!outputs[idx].declaration) return 0;
    const size_t max_type_words = 3 + 4 * (3 + 4);
    const size_t extra_words = max_type_words + stores * 4;
    if (*count > SIZE_MAX / sizeof(uint32_t) - extra_words) return -1;
    uint32_t *result = malloc((*count + extra_words) * sizeof *result);
    if (!result) return -1;
    memcpy(result, words, 5 * sizeof *result);
    size_t dst = 5;
    uint32_t next_id = module->bound;
    for (size_t pos = 5; pos < functions; pos += words[pos] >> 16) {
        uint32_t width = words[pos] >> 16;
        if ((words[pos] & 0xffff) == 59 && width == 4 &&
            rdna4s_fragment_output_lanes(outputs, output_count, words[pos + 2])) continue;
        memcpy(result + dst, words + pos, width * sizeof *result);
        dst += width;
    }
    if (!float_type[1]) {
        float_type[1] = next_id++;
        result[dst++] = (3u << 16) | 22u;
        result[dst++] = float_type[1];
        result[dst++] = 32;
    }
    for (size_t idx = 0; idx < output_count; idx++) {
        uint32_t lanes = outputs[idx].lanes;
        if (!float_type[lanes]) {
            float_type[lanes] = next_id++;
            result[dst++] = (4u << 16) | 23u;
            result[dst++] = float_type[lanes];
            result[dst++] = float_type[1];
            result[dst++] = lanes;
        }
        if (!pointer_type[lanes]) {
            pointer_type[lanes] = next_id++;
            result[dst++] = (4u << 16) | 32u;
            result[dst++] = pointer_type[lanes];
            result[dst++] = 3;
            result[dst++] = float_type[lanes];
        }
        memcpy(result + dst, words + outputs[idx].declaration, 4 * sizeof *result);
        result[dst + 1] = pointer_type[lanes];
        dst += 4;
    }
    for (size_t pos = functions; pos < *count; pos += words[pos] >> 16) {
        uint32_t op = words[pos] & 0xffff, width = words[pos] >> 16;
        uint32_t lanes = op == 62 && width >= 3 ? rdna4s_fragment_output_lanes(outputs, output_count, words[pos + 1]) : 0;
        uint32_t widened = 0;
        if (lanes) {
            widened = next_id++;
            result[dst++] = (4u << 16) | 115u;
            result[dst++] = float_type[lanes];
            result[dst++] = widened;
            result[dst++] = words[pos + 2];
        }
        memcpy(result + dst, words + pos, width * sizeof *result);
        if (widened) result[dst + 2] = widened;
        dst += width;
    }
    result[3] = next_id;
    *count = dst;
    *normalized = result;
    return 0;
}

#endif
