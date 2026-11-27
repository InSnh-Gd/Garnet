#include "garnet_spvrefl.h"
#include "garnet_fragment_output.h"

static void require(int condition, const char *message)
{
    if (condition) return;
    fprintf(stderr, "FRAGMENT_OUTPUT_FAIL %s\n", message);
    exit(1);
}

int main(int argc, char **argv)
{
    if (argc != 4) return 2;
    FILE *input = fopen(argv[1], "rb");
    require(input && fseek(input, 0, SEEK_END) == 0, "input open and seek");
    long bytes = ftell(input);
    require(bytes >= 20 && bytes % 4 == 0, "input size");
    rewind(input);
    uint32_t *source = malloc((size_t)bytes);
    require(source && fread(source, 1, (size_t)bytes, input) == (size_t)bytes, "input read");
    fclose(input);

    RDNA4sMod before, after;
    char why[256] = "";
    size_t count = (size_t)bytes / 4;
    require(!rdna4s_parse(source, count, &before, why, sizeof why), why);
    uint32_t *normalized = NULL;
    require(!rdna4s_widen_fragment_output(source, &count, &before, &normalized), "normalization");
    if (strcmp(argv[3], "any")) require((normalized != NULL) == !strcmp(argv[3], "widened"), "expected rewrite decision");
    const uint32_t *result = normalized ? normalized : source;
    if (normalized) {
        require(!rdna4s_parse(result, count, &after, why, sizeof why), why);
        RDNA4sRefl *old_layout = calloc(1, sizeof *old_layout), *new_layout = calloc(1, sizeof *new_layout);
        require(old_layout && new_layout, "reflection allocation");
        require(!rdna4s_reflect(&before, 0, old_layout) && !rdna4s_reflect(&after, 0, new_layout), "fragment reflection");
        require(!memcmp(old_layout, new_layout, sizeof *old_layout) && before.ep_id == after.ep_id &&
                !strcmp(before.ep_name, after.ep_name), "entry point and reflection preserved");
        uint32_t *second = NULL;
        require(!rdna4s_widen_fragment_output(result, &count, &after, &second) && !second, "rewrite is idempotent");
        free(old_layout);
        free(new_layout);
        rdna4s_free(&after);
    }
    FILE *output = fopen(argv[2], "wb");
    require(output && fwrite(result, 4, count, output) == count && fclose(output) == 0, "output write");
    puts(normalized ? "widened" : "unchanged");
    rdna4s_free(&before);
    free(normalized);
    free(source);
    return 0;
}
