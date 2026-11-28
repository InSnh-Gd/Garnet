// test-pipeline-fallback.c: verifies render fallback clears
// stale single-stage spvcache metadata (mv_ = nil; mf_ = nil;) before downstream
// descriptor/argument-table checks so single-stage hit + single-stage miss never
// fails with GarnetMetal Code=49, and pins compute placeholder metadata isolation.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int runs = 0, fails = 0;
#define CHECK(name, cond) do { \
    runs++; \
    if (!(cond)) { fails++; fprintf(stderr, "FAIL: %s\n", name); } \
} while (0)

static char *slurp(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *b = malloc((size_t)n + 1);
    if (!b) { fclose(f); return NULL; }
    if (fread(b, 1, (size_t)n, f) != (size_t)n) { fclose(f); free(b); return NULL; }
    b[n] = 0;
    fclose(f);
    return b;
}

typedef struct {
    int has_arg_table;
    unsigned abi_ver;
} stage_meta_t;

// Mirrors garnet_pipelines.m render fallback + rdna4_argument_table_stages_compatible.
static int render_fallback_build_ok(const stage_meta_t *mv_in, const stage_meta_t *mf_in,
                                    int vs_hit, int fs_hit, int clear_on_fb) {
    const stage_meta_t *mv = vs_hit ? mv_in : NULL;
    const stage_meta_t *mf = fs_hit ? mf_in : NULL;
    int fb = (!vs_hit || !fs_hit);
    if (fb && clear_on_fb) {
        mv = NULL;
        mf = NULL;
    }
    int rv_present = !fb;
    int rf_present = !fb;
    // Code=49 triggers if metadata claims argument_resource_tables without reflected stage or ABI mismatch.
    if ((mv && mv->has_arg_table && !rv_present) || (mf && mf->has_arg_table && !rf_present))
        return 0;
    unsigned av = (mv && mv->has_arg_table) ? mv->abi_ver : 0u;
    unsigned bv = (mf && mf->has_arg_table) ? mf->abi_ver : 0u;
    if (av && bv && av != bv) return 0;
    return 1;
}

int main(int argc, char **argv) {
    const char *dir = argc > 1 ? argv[1] : ".";
    const stage_meta_t arg_meta = { .has_arg_table = 1, .abi_ver = 10u };

    // Single-stage spvcache hit (VS hit with argument tables, FS miss) must succeed when fallback clears metadata,
    // and would fail with Code=49 without clearing.
    CHECK("model: VS hit + FS miss fails without clear", !render_fallback_build_ok(&arg_meta, &arg_meta, 1, 0, 0));
    CHECK("model: VS hit + FS miss succeeds with clear", render_fallback_build_ok(&arg_meta, &arg_meta, 1, 0, 1));
    CHECK("model: VS miss + FS hit fails without clear", !render_fallback_build_ok(&arg_meta, &arg_meta, 0, 1, 0));
    CHECK("model: VS miss + FS hit succeeds with clear", render_fallback_build_ok(&arg_meta, &arg_meta, 0, 1, 1));
    CHECK("model: both miss succeeds", render_fallback_build_ok(&arg_meta, &arg_meta, 0, 0, 1));
    CHECK("model: both hit with matching ABI succeeds", render_fallback_build_ok(&arg_meta, &arg_meta, 1, 1, 1));

    char path[1024];
    snprintf(path, sizeof path, "%s/garnet_pipelines.m", dir);
    char *src = slurp(path);
    CHECK("source: read garnet_pipelines.m", src != NULL);
    if (src) {
        const char *fbPos = strstr(src, "fb = YES; _fallback = YES;");
        const char *dslPos = strstr(src, "rdna4_argument_table_stages_compatible(mv_, mf_)");
        CHECK("source: render fallback branch present", fbPos != NULL && dslPos != NULL && fbPos < dslPos);
        if (fbPos && dslPos) {
            const char *clearPos = strstr(fbPos, "mv_ = nil; mf_ = nil;");
            CHECK("source: render fallback immediately clears mv_ and mf_ before argument-table checks",
                  clearPos != NULL && clearPos < dslPos && (size_t)(clearPos - fbPos) < 128u);
        }
        free(src);
    }
    snprintf(path, sizeof path, "%s/garnet_compute_pipelines.m", dir);
    char *csrc = slurp(path);
    CHECK("source: read garnet_compute_pipelines.m", csrc != NULL);
    if (csrc) {
        // Compute fallback placeholder audit: initPlaceholderWithDevice sets _noop = YES and never touches meta.
        const char *cpPos = strstr(csrc, "- (instancetype)initPlaceholderWithDevice:(id)dev function:(id)fn");
        const char *cpEnd = cpPos ? strstr(cpPos, "- (RDNA4ComputePipelineState *)rdna4Real") : NULL;
        CHECK("source: compute placeholder exists", cpPos != NULL && cpEnd != NULL && cpPos < cpEnd);
        if (cpPos && cpEnd) {
            size_t len = (size_t)(cpEnd - cpPos);
            char *slice = malloc(len + 1);
            memcpy(slice, cpPos, len);
            slice[len] = 0;
            CHECK("source: compute placeholder uses _noop without parsing sidecar metadata",
                  strstr(slice, "_noop = YES;") != NULL && strstr(slice, "argument_resource_tables") == NULL);
            free(slice);
        }
        free(csrc);
    }

    printf("test-pipeline-fallback: %d checks, %d failed\n", runs, fails);
    return fails ? 1 : 0;
}
