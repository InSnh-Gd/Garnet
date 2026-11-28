// Exercise the production postpass queue, input ownership, cache publication and deadline.
#include "garnet_xlate.h"
#include <stdatomic.h>

static unsigned checks;
static _Atomic unsigned calls, releases;
static _Atomic size_t stackSeen;
static unsigned delayMs;
static int refuse, oversize;

static void require(int ok, const char *message) {
    checks++;
    if (!ok) { fprintf(stderr, "ARGUMENT_WORKER_FAIL %s\n", message); exit(1); }
}

static int lower(void *context, const uint8_t *spv, size_t spvBytes, const uint8_t *meta, size_t metaBytes,
    uint8_t **code, size_t *codeBytes, uint8_t **json, size_t *jsonBytes, char *error, size_t errorBytes) {
    (void)context;
    atomic_fetch_add(&calls, 1);
    atomic_store(&stackSeen, pthread_get_stacksize_np(pthread_self()));
    if (delayMs) usleep(delayMs * 1000);
    if (spvBytes != 64 || spv[20] != 37 || metaBytes != 2 || memcmp(meta, "{}", 2)) return 3;
    if (refuse) { snprintf(error, errorBytes, "authored postpass refusal"); return 1; }
    *codeBytes = oversize ? RDNA4X_MAX_SPV + 4 : spvBytes;
    *code = calloc(1, *codeBytes); *jsonBytes = 2; *json = malloc(2);
    if (!*code || !*json) return 3;
    memcpy(*code, spv, spvBytes); (*code)[20] = 91; memcpy(*json, "{}", 2);
    return 0;
}

static void release(void *context, uint8_t *bytes, size_t length) {
    (void)context; (void)length;
    atomic_fetch_add(&releases, 1); free(bytes);
}

static int exists(const char *directory, const char *sha, const char *extension) {
    char path[RDNA4X_PATH_MAX];
    return rdna4x_file_path(path, sizeof path, directory, sha, extension) && rdna4x_exists(path);
}

static void key(char sha[65], char byte) { memset(sha, byte, 64); sha[64] = 0; }
static int publishedByte(const char *directory, const char *sha) {
    char path[RDNA4X_PATH_MAX]; uint8_t byte = 0;
    if (!rdna4x_file_path(path, sizeof path, directory, sha, RDNA4X_EXT_SPV)) return -1;
    int fd = open(path, O_RDONLY | O_NOFOLLOW); if (fd < 0) return -1;
    ssize_t count = pread(fd, &byte, 1, 20); close(fd); return count == 1 ? byte : -1;
}

static int reader(void *context, const uint8_t *air, size_t length, char **text, size_t *size, char *error, size_t errorBytes) {
    (void)context; (void)air; (void)length; (void)error; (void)errorBytes;
    *text = strdup("authored"); *size = 8; return *text ? 0 : 1;
}
static void freeText(void *context, char *text) { (void)context; free(text); }
static int translate(void *context, const char *text, size_t length, const uint32_t *local,
    uint8_t **spv, size_t *sl, uint8_t **meta, size_t *ml, char *error, size_t errorBytes) {
    (void)context; (void)text; (void)length; (void)local; (void)error; (void)errorBytes;
    *spv = calloc(1, 64); *sl = 64; *meta = malloc(2); *ml = 2;
    if (!*spv || !*meta) return 3;
    (*spv)[0] = 3; (*spv)[1] = 2; (*spv)[2] = 0x23; (*spv)[3] = 7; (*spv)[20] = 37;
    memcpy(*meta, "{}", 2); return 0;
}
static int translateLinked(void *context, const char *text, size_t length, const uint32_t *local,
    const rdna4x_ldep_ll *deps, size_t count, uint8_t **spv, size_t *sl, uint8_t **meta, size_t *ml, char *error, size_t errorBytes) {
    if (count != 1 || strcmp(deps[0].symbol, "dep") || deps[0].llLen != 8) return 3;
    return translate(context, text, length, local, spv, sl, meta, ml, error, errorBytes);
}

int main(int argc, const char **argv) {
    if (argc != 2) return 2;
    alarm(20);
    static rdna4x_engine engine, missing, combined, legacy;
    rdna4x_deps dependencies = {.release = release, .lower_arguments = lower};
    rdna4x_engine_init(&engine, &dependencies, NULL, NULL, NULL, RDNA4X_WORKER_STACK);
    uint8_t spv[64] = {3, 2, 0x23, 7}; spv[20] = 37;
    char sha[65], error[256]; key(sha, 'a');
    require(!mkdir(argv[1], 0700), "fresh output directory");
    int rc = rdna4x_engine_lower_arguments(&engine, argv[1], sha, spv, sizeof spv,
        (const uint8_t *)"{}", 2, 2000, error, sizeof error);
    require(rc == RDNA4X_OK && atomic_load(&calls) == 1 && atomic_load(&releases) == 2, "postpass completion and output release");
    require(atomic_load(&stackSeen) >= RDNA4X_WORKER_STACK, "postpass runs on the 64-MiB worker stack");
    require(exists(argv[1], sha, RDNA4X_EXT_SPV) && exists(argv[1], sha, RDNA4X_EXT_META) &&
        !exists(argv[1], sha, RDNA4X_EXT_INFLIGHT), "complete pair publication and marker retirement");
    rc = rdna4x_engine_lower_arguments(&engine, argv[1], sha, spv, sizeof spv,
        (const uint8_t *)"{}", 2, 2000, error, sizeof error);
    require(rc == RDNA4X_OK && atomic_load(&calls) == 1, "cache hit does not rerun the postpass");

    key(sha, 'b'); delayMs = 300;
    uint8_t *owned = malloc(sizeof spv), *metadata = malloc(2);
    require(owned && metadata, "caller allocation"); memcpy(owned, spv, sizeof spv); memcpy(metadata, "{}", 2);
    struct timeval start, finish; gettimeofday(&start, NULL);
    rc = rdna4x_engine_lower_arguments(&engine, argv[1], sha, owned, sizeof spv,
        metadata, 2, 20, error, sizeof error);
    gettimeofday(&finish, NULL);
    memset(owned, 0, sizeof spv); memset(metadata, 0, 2); free(owned); free(metadata);
    long elapsed = (finish.tv_sec - start.tv_sec) * 1000 + (finish.tv_usec - start.tv_usec) / 1000;
    require(rc == RDNA4X_E_TIMEOUT && elapsed < 250 && !exists(argv[1], sha, RDNA4X_EXT_SPV), "caller deadline is independent of postpass completion");
    rc = rdna4x_engine_lower_arguments(&engine, argv[1], sha, spv, sizeof spv,
        (const uint8_t *)"{}", 2, 2000, error, sizeof error);
    require(rc == RDNA4X_OK && atomic_load(&calls) == 2, "retry shares the job and owns both caller inputs");
    delayMs = 0; key(sha, 'c'); refuse = 1;
    rc = rdna4x_engine_lower_arguments(&engine, argv[1], sha, spv, sizeof spv,
        (const uint8_t *)"{}", 2, 2000, error, sizeof error);
    require(rc == RDNA4X_E_REFUSED && strstr(error, "authored postpass refusal") &&
        exists(argv[1], sha, RDNA4X_EXT_FAIL) && !exists(argv[1], sha, RDNA4X_EXT_SPV), "refusal preserves the reason without publishing code");
    refuse = 0; key(sha, 'd'); oversize = 1;
    rc = rdna4x_engine_lower_arguments(&engine, argv[1], sha, spv, sizeof spv,
        (const uint8_t *)"{}", 2, 2000, error, sizeof error);
    require(rc == RDNA4X_E_CAP && !exists(argv[1], sha, RDNA4X_EXT_SPV), "postpass output is capped before publication");
    oversize = 0;
    rc = rdna4x_engine_lower_arguments(&engine, argv[1], sha, spv, RDNA4X_MAX_SPV + 4,
        (const uint8_t *)"{}", 2, 2000, error, sizeof error);
    require(rc == RDNA4X_E_CAP && atomic_load(&calls) == 4, "input is capped before copying or queueing");
    key(sha, 'e'); dependencies.lower_arguments = NULL;
    rdna4x_engine_init(&missing, &dependencies, NULL, NULL, NULL, RDNA4X_WORKER_STACK);
    rc = rdna4x_engine_lower_arguments(&missing, argv[1], sha, spv, sizeof spv,
        (const uint8_t *)"{}", 2, 2000, error, sizeof error);
    require(rc == RDNA4X_E_UNAVAILABLE && error[0] && !exists(argv[1], sha, RDNA4X_EXT_SPV), "missing optional export is a controlled error");

    dependencies = (rdna4x_deps){.air_to_text = reader, .free_text = freeText, .translate = translate,
        .translate_linked = translateLinked, .release = release, .lower_arguments = lower, .lowerTranslatedArguments = 1};
    rdna4x_engine_init(&combined, &dependencies, NULL, NULL, NULL, RDNA4X_WORKER_STACK);
    key(sha, 'f');
    rc = rdna4x_engine_run(&combined, argv[1], sha, "plain", spv, 16, 2000, error, sizeof error);
    require(rc == RDNA4X_OK && publishedByte(argv[1], sha) == 91 && atomic_load(&calls) == 5 && atomic_load(&releases) == 10, "AIR translation and postpass form one worker job with complete output release");
    key(sha, '0'); rdna4x_ldep dependency = {.symbol = "dep", .air = spv, .airLen = 16};
    rc = rdna4x_engine_run_linked(&combined, argv[1], sha, "linked", spv, 16, &dependency, 1, 2000, error, sizeof error);
    require(rc == RDNA4X_OK && publishedByte(argv[1], sha) == 91 && atomic_load(&calls) == 6 && atomic_load(&releases) == 14, "linked translation uses the same postpass and publication path");
    key(sha, '1'); dependencies.lower_arguments = NULL;
    rdna4x_engine_init(&legacy, &dependencies, NULL, NULL, NULL, RDNA4X_WORKER_STACK);
    rc = rdna4x_engine_run(&legacy, argv[1], sha, "legacy", spv, 16, 2000, error, sizeof error);
    require(rc == RDNA4X_OK && publishedByte(argv[1], sha) == 37 && atomic_load(&calls) == 6 && atomic_load(&releases) == 16,
        "a translator without the optional export retains ordinary AIR translation");
    printf("ARGUMENT_WORKER_PASS checks=%u gpu_execution=0\n", checks);
    return 0;
}
