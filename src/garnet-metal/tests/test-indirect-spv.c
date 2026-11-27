#include "garnet_indirect_compute.h"
#include <errno.h>

static void require(int condition, const char *what) {
    if (!condition) { fprintf(stderr, "INDIRECT_SPV_FAIL %s\n", what); exit(1); }
}

int main(int argc, char **argv) {
    if (argc == 2 && !strcmp(argv[1], "--help")) {
        puts("Usage: test-indirect-spv INPUT.spv OUTPUT.spv LOCAL_X LOCAL_Y LOCAL_Z OFFSET\n"
             "Transforms ThreadsDynamic dispatch metadata into GPU grid builtins and checks reflection invariants.");
        return 0;
    }
    if (argc != 7) return 2;
    uint32_t dimensions[4];
    for (int i = 0; i < 4; ++i) {
        char *end;
        errno = 0;
        unsigned long value = strtoul(argv[3+i], &end, 10);
        if (errno || *end || !*argv[3+i] || value > UINT32_MAX) return 2;
        dimensions[i] = (uint32_t)value;
    }
    FILE *file = fopen(argv[1], "rb");
    require(file != NULL, "input open");
    require(fseek(file, 0, SEEK_END) == 0, "input seek");
    long bytes = ftell(file);
    require(bytes >= 20 && bytes % 4 == 0, "input length");
    rewind(file);
    uint32_t *source = malloc((size_t)bytes);
    require(source != NULL, "input allocation");
    require(fread(source, 1, (size_t)bytes, file) == (size_t)bytes, "input read");
    require(fclose(file) == 0, "input close");
    uint32_t *output = NULL;
    size_t count = 0;
    char why[256] = "";
    require(rdna4s_indirect_compute(source, (size_t)bytes / 4, dimensions[3], dimensions, &output, &count, why, sizeof why) == 0, why);
    RDNA4sMod before = {0}, after = {0};
    require(rdna4s_parse(source, (size_t)bytes / 4, &before, why, sizeof why) == 0, why);
    require(rdna4s_parse(output, count, &after, why, sizeof why) == 0, why);
    RDNA4sRefl *old_reflection = calloc(1, sizeof(*old_reflection)), *new_reflection = calloc(1, sizeof(*new_reflection));
    require(old_reflection && new_reflection, "reflection allocation");
    require(rdna4s_reflect(&before, 2, old_reflection) == 0, old_reflection->why);
    require(rdna4s_reflect(&after, 2, new_reflection) == 0, new_reflection->why);
    require(memcmp(old_reflection, new_reflection, sizeof(*old_reflection)) == 0, "descriptor and push-constant layout unchanged");
    require(before.ep_id == after.ep_id && !strcmp(before.ep_name, after.ep_name), "entry point unchanged");
    unsigned grids = 0;
    for (uint32_t id = 1; id < after.bound; ++id)
        grids += after.id[id].op == 59 && after.id[id].sc == 1 && after.id[id].builtin == 24;
    if (old_reflection->pc_size) require(grids == 1, "one NumWorkgroups input");
    else require(count * 4 == (size_t)bytes && !memcmp(source, output, (size_t)bytes), "kernel without dispatch metadata unchanged");
    file = fopen(argv[2], "wb");
    require(file != NULL && fwrite(output, 4, count, file) == count && fclose(file) == 0, "output write");
    free(output);
    const uint32_t zero_local[3] = {0,1,1};
    require(rdna4s_indirect_compute(source, (size_t)bytes / 4, dimensions[3], zero_local, &output, &count, why, sizeof why) != 0 && !output && !count,
            "invalid local size rejected");
    require(rdna4s_indirect_compute(source, 4, dimensions[3], dimensions, &output, &count, why, sizeof why) != 0 && !output && !count,
            "truncated module rejected");
    if (old_reflection->pc_size)
        require(rdna4s_indirect_compute(source, (size_t)bytes / 4, dimensions[3] + 4, dimensions, &output, &count, why, sizeof why) != 0 && !output && !count,
                "metadata offset mismatch rejected");
    printf("INDIRECT_SPV_PASS kernel=%s local=%u,%u,%u reflection_unchanged=1 malformed_rejected=1\n",
            before.ep_name, dimensions[0], dimensions[1], dimensions[2]);
    rdna4s_free(&before);
    rdna4s_free(&after);
    free(old_reflection);
    free(new_reflection);
    free(source);
    return 0;
}
